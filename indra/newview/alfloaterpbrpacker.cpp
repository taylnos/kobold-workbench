/**
* @file alfloaterpbrpacker.cpp
* @brief Floater for packing source maps into Second Life glTF PBR textures
*
* $LicenseInfo:firstyear=2026&license=viewerlgpl$
* Alchemy Viewer Source Code
* Copyright (C) 2026, Alchemy Viewer Project.
*
* This library is free software; you can redistribute it and/or
* modify it under the terms of the GNU Lesser General Public
* License as published by the Free Software Foundation;
* version 2.1 of the License only.
*
* This library is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
* Lesser General Public License for more details.
*
* You should have received a copy of the GNU Lesser General Public
* License along with this library; if not, write to the Free Software
* Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
*
* $/LicenseInfo$
*/

#include "llviewerprecompiledheaders.h"

#include "alfloaterpbrpacker.h"

// library
#include "alsamplerstate.h"
#include "fsyspath.h"
#include "llbutton.h"
#include "llcheckboxctrl.h"
#include "llcombobox.h"
#include "lldir.h"
#include "llfile.h"
#include "llimagepng.h"
#include "llrender.h"
#include "llrender2dutils.h"
#include "lltextbox.h"
#include "workqueue.h"

// newview
#include "llfloaterreg.h"
#include "lllocalgltfmaterials.h"
#include "llmaterialeditor.h"
#include "llnotificationsutil.h"
#include "lltinygltfhelper.h"
#include "llviewermenufile.h"
#include "llviewertexturelist.h"

namespace
{
    // Maps an ingest slot onto its XUI widget names and which of the optional
    // per-slot controls it exposes. Base colour and emissive copy RGB straight
    // through; the normal map offers only a green flip; the scalar maps get a
    // full source-channel choice.
    struct SlotDef
    {
        ALPackSlot  mSlot;
        const char* mPrefix;
        bool        mHasChannel;
        bool        mHasInvert;
    };

    // Matches LL_LOCAL_TIMER_HEARTBEAT, the cadence the local texture system
    // already polls its own files at.
    constexpr F32 SOURCE_WATCH_PERIOD = 3.f;

    // Side of the cached per-slot thumbnail source. Re-rendered on every
    // channel or invert toggle, so it stays small -- but large enough to still
    // look sharp in a 120px card.
    //
    // Square, matching the square preview panels: since the draw stretches to
    // fill like any other texture swatch, an aspect-preserving intermediate
    // would only discard resolution on the axis about to be stretched back up.
    constexpr S32 SLOT_THUMB_SIZE = 128;

    const SlotDef sSlotDefs[] =
    {
        { ALPackSlot::BASE_COLOR, "base_color", false, false },
        { ALPackSlot::EMISSIVE,   "emissive",   false, false },
        { ALPackSlot::OCCLUSION,  "occlusion",  true,  true  },
        { ALPackSlot::ROUGHNESS,  "roughness",  true,  true  },
        { ALPackSlot::METALLIC,   "metallic",   true,  true  },
        { ALPackSlot::NORMAL,     "normal",     false, true  },
        { ALPackSlot::OPACITY,    "opacity",    true,  true  },
    };

    // The packed textures, in LLGLTFMaterial::TextureInfo order so a result's
    // mDest doubles as its card index.
    struct OutputDef
    {
        LLGLTFMaterial::TextureInfo mDest;
        const char*                 mPrefix;
    };

    const OutputDef sOutputDefs[] =
    {
        { LLGLTFMaterial::GLTF_TEXTURE_INFO_BASE_COLOR,        "out_base_color" },
        { LLGLTFMaterial::GLTF_TEXTURE_INFO_NORMAL,            "out_normal"     },
        { LLGLTFMaterial::GLTF_TEXTURE_INFO_METALLIC_ROUGHNESS, "out_orm"       },
        { LLGLTFMaterial::GLTF_TEXTURE_INFO_EMISSIVE,          "out_emissive"   },
    };

    struct LoadResult
    {
        LLPointer<LLImageRaw> mImage;
        std::string           mError;
    };

    struct PackResult
    {
        ALPackOutputSet          mOutputs;
        std::vector<std::string> mPaths;
        std::vector<std::string> mWarnings;
        std::string              mError;
        bool                     mOk = false;
    };

    // Find a target in a recipe by the material slot it feeds.
    ALPackTarget* find_target(ALPBRPackRecipe& recipe, LLGLTFMaterial::TextureInfo dest)
    {
        for (ALPackTarget& target : recipe.mTargets)
        {
            if (target.mDest == dest)
            {
                return &target;
            }
        }
        return nullptr;
    }

    // Filename tokens that identify a source map. Order matters: the longer,
    // less ambiguous forms have to be tested before the short suffixes, or
    // "..._normal" would be claimed by the metalness token "_n".
    struct SlotHint
    {
        const char* mToken;
        ALPackSlot  mSlot;
        bool        mImpliesInvert;
    };

    const SlotHint sSlotHints[] =
    {
        { "basecolor",        ALPackSlot::BASE_COLOR, false },
        { "base_color",       ALPackSlot::BASE_COLOR, false },
        { "albedo",           ALPackSlot::BASE_COLOR, false },
        { "diffuse",          ALPackSlot::BASE_COLOR, false },
        { "_color",           ALPackSlot::BASE_COLOR, false },
        { "_col",             ALPackSlot::BASE_COLOR, false },
        { "_bc",              ALPackSlot::BASE_COLOR, false },

        { "normal",           ALPackSlot::NORMAL,     false },
        { "_nrm",             ALPackSlot::NORMAL,     false },
        { "_norm",            ALPackSlot::NORMAL,     false },

        { "ambientocclusion", ALPackSlot::OCCLUSION,  false },
        { "occlusion",        ALPackSlot::OCCLUSION,  false },
        { "_ao",              ALPackSlot::OCCLUSION,  false },
        { "_occ",             ALPackSlot::OCCLUSION,  false },

        { "roughness",        ALPackSlot::ROUGHNESS,  false },
        { "_rough",           ALPackSlot::ROUGHNESS,  false },
        { "_rgh",             ALPackSlot::ROUGHNESS,  false },
        // Gloss and smoothness are roughness upside down.
        { "glossiness",       ALPackSlot::ROUGHNESS,  true  },
        { "gloss",            ALPackSlot::ROUGHNESS,  true  },
        { "smoothness",       ALPackSlot::ROUGHNESS,  true  },

        { "metalness",        ALPackSlot::METALLIC,   false },
        { "metallic",         ALPackSlot::METALLIC,   false },
        { "_metal",           ALPackSlot::METALLIC,   false },
        { "_mtl",             ALPackSlot::METALLIC,   false },

        { "emissive",         ALPackSlot::EMISSIVE,   false },
        { "emission",         ALPackSlot::EMISSIVE,   false },
        { "_emis",            ALPackSlot::EMISSIVE,   false },

        { "opacity",          ALPackSlot::OPACITY,    false },
        { "_alpha",           ALPackSlot::OPACITY,    false },
        { "transparen",       ALPackSlot::OPACITY,    false },
    };

    // Already-packed masks. These feed several slots from one file, each
    // reading a different channel.
    const char* const sCombinedORMTokens[] =
    {
        "occlusionroughnessmetallic", "_orm", "_arm", "_ora", "_occlusionroughnessmetallic"
    };

    const char* const sCombinedMRTokens[] =
    {
        "metallicroughness", "roughnessmetallic", "_mr", "_rm"
    };

    bool contains_any(const std::string& haystack, const char* const* tokens, size_t count)
    {
        for (size_t i = 0; i < count; ++i)
        {
            if (haystack.find(tokens[i]) != std::string::npos)
            {
                return true;
            }
        }
        return false;
    }

    // Cheap statistics over a channel. Strided so a 2048 square map costs
    // thousands of reads rather than millions -- these only drive advisory text.
    void channel_stats(const LLImageRaw* image, S8 channel, U8& min_out, U8& max_out, F32& mean_out)
    {
        min_out = 255;
        max_out = 0;
        mean_out = 0.f;

        if (!image || image->getComponents() <= channel)
        {
            return;
        }

        LLImageDataSharedLock lock(image);

        const U8* data = image->getData();
        const S8 components = image->getComponents();
        const U32 pixel_count = (U32)image->getWidth() * (U32)image->getHeight();
        if (!data || pixel_count == 0)
        {
            return;
        }

        const U32 stride = llmax(1u, pixel_count / 4096u);
        U64 total = 0;
        U32 samples = 0;

        for (U32 i = 0; i < pixel_count; i += stride)
        {
            const U8 value = data[(size_t)i * components + channel];
            min_out = llmin(min_out, value);
            max_out = llmax(max_out, value);
            total += value;
            ++samples;
        }

        if (samples > 0)
        {
            mean_out = (F32)total / (F32)samples;
        }
    }

    void collect_warnings(const ALPackInputSet& inputs, S32 max_dim, std::vector<std::string>& warnings)
    {
        for (size_t i = 0; i < inputs.size(); ++i)
        {
            const LLPointer<LLImageRaw>& image = inputs[i];
            if (image.isNull())
            {
                continue;
            }
            if (image->getWidth() > max_dim || image->getHeight() > max_dim)
            {
                warnings.push_back(llformat("%s is larger than %d and will be downscaled.",
                                            ALPBRPacker::slotName((ALPackSlot)i), max_dim));
            }
        }

        U8 min_value = 0;
        U8 max_value = 0;
        F32 mean = 0.f;

        const LLPointer<LLImageRaw>& occlusion = inputs[(size_t)ALPackSlot::OCCLUSION];
        if (occlusion.notNull())
        {
            channel_stats(occlusion, 0, min_value, max_value, mean);
            if (min_value == 255)
            {
                warnings.push_back("Occlusion is uniformly white and can be left out.");
            }
        }

        const LLPointer<LLImageRaw>& metallic = inputs[(size_t)ALPackSlot::METALLIC];
        if (metallic.notNull())
        {
            channel_stats(metallic, 0, min_value, max_value, mean);
            if (min_value == max_value)
            {
                warnings.push_back("Metallic is a constant value; the Metalness Factor alone would do.");
            }
        }

        const LLPointer<LLImageRaw>& normal = inputs[(size_t)ALPackSlot::NORMAL];
        if (normal.notNull() && normal->getComponents() >= 3)
        {
            channel_stats(normal, 1, min_value, max_value, mean);
            if (mean < 118.f)
            {
                warnings.push_back("Normal green averages low, which suggests a DirectX-convention map.");
            }
        }

        const LLPointer<LLImageRaw>& base_color = inputs[(size_t)ALPackSlot::BASE_COLOR];
        if (base_color.notNull() && base_color->getComponents() == 4)
        {
            channel_stats(base_color, 3, min_value, max_value, mean);
            if (min_value < 255)
            {
                warnings.push_back("Base colour carries alpha; set Alpha Mode in the material editor.");
            }
        }
    }

    // Draw a texture filling a rect, exactly as LLTextureCtrl draws a swatch:
    // checkerboard behind for alpha, then the whole image stretched to fill.
    // The panels are square, so square maps -- effectively all of them -- come
    // out untouched, and anything non-square is distorted the same way every
    // other texture swatch in the viewer distorts it.
    void draw_texture_in_rect(LLViewerTexture* texture, const LLRect& rect)
    {
        if (!texture)
        {
            // The same empty-slot placeholder LLTextureCtrl draws, so a slot
            // with nothing in it reads identically here and in the material
            // editor.
            gl_rect_2d(rect, LLColor4::grey, true);
            gl_draw_x(rect, LLColor4::black);
            return;
        }

        gl_rect_2d_checkerboard(rect);
        gl_draw_scaled_image(rect.mLeft, rect.mBottom, rect.getWidth(), rect.getHeight(), texture);

        stop_glerror();
    }

    std::string sanitize_for_filename(const std::string& name)
    {
        std::string out;
        out.reserve(name.size());
        for (char c : name)
        {
            if (c != ' ')
            {
                out.push_back(c);
            }
        }
        return out;
    }
}

ALFloaterPBRPacker::ALFloaterPBRPacker(const LLSD& key)
    : LLFloater(key)
    , LLEventTimer(SOURCE_WATCH_PERIOD)
{
    // A stable stem for every packed map this session writes, so re-packing
    // overwrites in place -- which is what makes the local texture system
    // notice and refresh the in-world preview. Named rather than a bare uuid
    // because these show up in the user's Local Textures list until the
    // floater is closed.
    LLUUID session;
    session.generate();
    mTempStem = gDirUtilp->add(gDirUtilp->getTempDir(),
                               "PBRPacker_" + session.asString().substr(0, 8));
}

ALFloaterPBRPacker::~ALFloaterPBRPacker()
{
}

bool ALFloaterPBRPacker::postBuild()
{
    for (const SlotDef& def : sSlotDefs)
    {
        SlotUI& ui = mSlotUI[(size_t)def.mSlot];
        const std::string prefix(def.mPrefix);

        ui.mCard = getChild<LLView>(prefix + "_card");

        ui.mClear = getChild<LLButton>(prefix + "_clear");
        ui.mClear->setClickedCallback([this, slot = def.mSlot](LLUICtrl*, const LLSD&) { onClear(slot); });

        if (def.mHasChannel)
        {
            ui.mChannel = getChild<LLComboBox>(prefix + "_channel");
            ui.mChannel->setCommitCallback([this](LLUICtrl*, const LLSD&) { onSettingChanged(); });
        }

        if (def.mHasInvert)
        {
            ui.mInvert = getChild<LLButton>(prefix + "_invert");
            ui.mInvert->setCommitCallback([this](LLUICtrl*, const LLSD&) { onSettingChanged(); });
        }

        // The preview itself opens the picker.
        ui.mThumb = getChild<LLButton>(prefix + "_thumb");
        ui.mThumb->setClickedCallback([this, slot = def.mSlot](LLUICtrl*, const LLSD&) { onBrowse(slot); });
        mSlotEmptyTip[(size_t)def.mSlot] = ui.mThumb->getToolTip();
    }

    mPackBtn = getChild<LLButton>("pack");
    mPackBtn->setClickedCallback([this](LLUICtrl*, const LLSD&) { onPack(); });

    mSaveBtn = getChild<LLButton>("save_to_disk");
    mSaveBtn->setClickedCallback([this](LLUICtrl*, const LLSD&) { onSave(); });

    mMakeLocalBtn = getChild<LLButton>("make_local_material");
    mMakeLocalBtn->setClickedCallback([this](LLUICtrl*, const LLSD&) { onMakeLocalMaterial(); });

    mSendToEditorBtn = getChild<LLButton>("send_to_editor");
    mSendToEditorBtn->setClickedCallback([this](LLUICtrl*, const LLSD&) { onSendToMaterialEditor(); });

    mMaxSizeCombo = getChild<LLComboBox>("max_size");
    mMaxSizeCombo->setCommitCallback([this](LLUICtrl*, const LLSD&) { onSettingChanged(); });

    mAutoRepackCheck = getChild<LLCheckBoxCtrl>("auto_repack");

    // Auto Size (value 0) by default: follow the largest source map.
    mMaxSizeCombo->setValue(LLSD::Integer(0));

    mAddFilesBtn = getChild<LLButton>("add_files");
    mAddFilesBtn->setClickedCallback([this](LLUICtrl*, const LLSD&) { onAddFiles(); });

    mPresetCombo = getChild<LLComboBox>("preset");
    mPresetCombo->setCommitCallback([this](LLUICtrl*, const LLSD&) { onPresetChanged(); });

    // Default to reading an already-packed ORM mask by channel. That is the
    // plain glTF 2.0 case and what most creators arrive with -- a Substance
    // export already carries a combined occlusionRoughnessMetallic map. Anyone
    // feeding three separate greyscale maps switches the preset, and the
    // filename heuristics in onFilesPicked() correct the routing on their own.
    //
    // Selected by value rather than index so the combo can be ordered for
    // readability without silently remapping the presets.
    mPresetCombo->setValue(LLSD::Integer(PRESET_PACKED_ORM));
    applyPreset(PRESET_PACKED_ORM);

    for (const OutputDef& def : sOutputDefs)
    {
        OutputUI& out = mOutputUI[(size_t)def.mDest];
        const std::string prefix(def.mPrefix);
        out.mCard  = getChild<LLView>(prefix + "_card");
        out.mThumb = getChild<LLView>(prefix + "_thumb");
        out.mSize  = getChild<LLTextBox>(prefix + "_size");
    }

    mStatusText = getChild<LLTextBox>("status");

    refreshControls();

    return LLFloater::postBuild();
}

void ALFloaterPBRPacker::onOpen(const LLSD& key)
{
    mEventTimer.start();
    refreshControls();
}

void ALFloaterPBRPacker::onClose(bool app_quitting)
{
    mEventTimer.stop();

    // A local material outlives this floater on purpose: it behaves like any
    // other local asset, so it stays registered and its files stay on disk for
    // as long as the viewer runs. Closing the packer must not yank a material
    // out from under whatever the user has already applied it to.
    clearOutputs();
}

// static
ALPackChannel ALFloaterPBRPacker::channelFromIndex(S32 index)
{
    switch (index)
    {
    case 1:  return ALPackChannel::GREEN;
    case 2:  return ALPackChannel::BLUE;
    case 3:  return ALPackChannel::ALPHA;
    case 4:  return ALPackChannel::LUMINANCE;
    default: return ALPackChannel::RED;
    }
}

bool ALFloaterPBRPacker::hasAnyInput() const
{
    for (const LLPointer<LLImageRaw>& image : mInputs)
    {
        if (image.notNull())
        {
            return true;
        }
    }
    return false;
}

void ALFloaterPBRPacker::onBrowse(ALPackSlot slot)
{
    LLHandle<ALFloaterPBRPacker> handle = getDerivedHandle<ALFloaterPBRPacker>();

    LLFilePickerReplyThread::startPicker(
        [handle, slot](const std::vector<std::string>& filenames,
                       LLFilePicker::ELoadFilter,
                       LLFilePicker::ESaveFilter)
        {
            if (ALFloaterPBRPacker* self = handle.get())
            {
                self->onFilePicked(slot, filenames);
            }
        },
        LLFilePicker::FFLOAD_IMAGE,
        false);
}

void ALFloaterPBRPacker::onFilePicked(ALPackSlot slot, const std::vector<std::string>& filenames)
{
    if (filenames.empty())
    {
        return;
    }

    loadSlot(slot, filenames.front(), false);
}

bool ALFloaterPBRPacker::slotInverted(ALPackSlot slot) const
{
    const LLButton* invert = mSlotUI[(size_t)slot].mInvert;
    return invert && invert->getToggleState();
}

LLRect ALFloaterPBRPacker::localRectOf(const LLView* view) const
{
    // Slot widgets are nested inside their card panel, so getRect() is
    // card-relative. Go via screen space to land back in floater coordinates,
    // which is what draw() paints in.
    LLRect rect = view->calcScreenRect();
    const LLRect origin = calcScreenRect();
    rect.translate(-origin.mLeft, -origin.mBottom);
    return rect;
}

void ALFloaterPBRPacker::setSlotRouting(ALPackSlot slot, ALPackChannel channel, bool invert)
{
    const SlotUI& ui = mSlotUI[(size_t)slot];
    if (ui.mChannel)
    {
        ui.mChannel->setValue(LLSD::Integer((S32)channel));
    }
    if (ui.mInvert)
    {
        ui.mInvert->setToggleState(invert);
    }
    mSlotThumbDirty[(size_t)slot] = true;
}

void ALFloaterPBRPacker::markSlotThumbsDirty()
{
    mSlotThumbDirty.fill(true);
}

void ALFloaterPBRPacker::clearOutputs()
{
    mOutputs.clear();
    mOutputPaths.clear();
    mOutputTex.fill(nullptr);
    mOutputThumbsDirty = false;

    for (OutputUI& out : mOutputUI)
    {
        if (out.mSize)
        {
            out.mSize->setText(std::string());
        }
    }
}

void ALFloaterPBRPacker::rebuildSlotThumb(ALPackSlot slot)
{
    const size_t index = (size_t)slot;
    mSlotThumbDirty[index] = false;
    mSlotThumbTex[index] = nullptr;

    LLPointer<LLImageRaw> src = mSlotThumbSrc[index];
    if (src.isNull() || src->isBufferInvalid())
    {
        return;
    }

    const SlotUI& ui = mSlotUI[index];
    const bool invert = slotInverted(slot);

    const S32 width = src->getWidth();
    const S32 height = src->getHeight();
    LLPointer<LLImageRaw> thumb = new LLImageRaw((U16)width, (U16)height, 3);
    if (thumb->isBufferInvalid())
    {
        return;
    }

    LLImageDataSharedLock lock_src(src);
    LLImageDataLock lock_dst(thumb);

    const U8* in = src->getData();
    U8* out = thumb->getData();
    const S8 in_components = src->getComponents();
    const U32 pixel_count = (U32)width * (U32)height;

    if (ui.mChannel)
    {
        // A scalar slot: show the single channel the pack will actually read,
        // as grey, so the channel choice and inversion are both visible.
        const ALPackChannel channel = channelFromIndex(ui.mChannel->getValue().asInteger());
        for (U32 i = 0; i < pixel_count; ++i)
        {
            U8 value = ALPBRPacker::sampleChannel(in + (size_t)i * in_components, in_components, channel);
            if (invert)
            {
                value = (U8)(255 - value);
            }
            out[i * 3 + 0] = value;
            out[i * 3 + 1] = value;
            out[i * 3 + 2] = value;
        }
    }
    else
    {
        // A colour slot: show it as it is, but honour a normal-map green flip
        // so that toggle has visible feedback too.
        for (U32 i = 0; i < pixel_count; ++i)
        {
            const U8* pixel = in + (size_t)i * in_components;
            out[i * 3 + 0] = ALPBRPacker::sampleChannel(pixel, in_components, ALPackChannel::RED);
            U8 green = ALPBRPacker::sampleChannel(pixel, in_components, ALPackChannel::GREEN);
            if (invert)
            {
                green = (U8)(255 - green);
            }
            out[i * 3 + 1] = green;
            out[i * 3 + 2] = ALPBRPacker::sampleChannel(pixel, in_components, ALPackChannel::BLUE);
        }
    }

    mSlotThumbTex[index] = LLViewerTextureManager::getLocalTexture(thumb.get(), false);
}

void ALFloaterPBRPacker::onAddFiles()
{
    LLHandle<ALFloaterPBRPacker> handle = getDerivedHandle<ALFloaterPBRPacker>();

    LLFilePickerReplyThread::startPicker(
        [handle](const std::vector<std::string>& filenames,
                 LLFilePicker::ELoadFilter,
                 LLFilePicker::ESaveFilter)
        {
            if (ALFloaterPBRPacker* self = handle.get())
            {
                self->onFilesPicked(filenames);
            }
        },
        LLFilePicker::FFLOAD_IMAGE,
        true);
}

void ALFloaterPBRPacker::onFilesPicked(const std::vector<std::string>& filenames)
{
    std::vector<std::string> unmatched;
    S32 assigned = 0;

    for (const std::string& path : filenames)
    {
        std::string name = gDirUtilp->getBaseFileName(path, true);
        LLStringUtil::toLower(name);

        // An already-packed ORM feeds three slots off one file, each reading a
        // different channel. This is the case the old import path could not
        // express at all.
        if (contains_any(name, sCombinedORMTokens, LL_ARRAY_SIZE(sCombinedORMTokens)))
        {
            loadSlot(ALPackSlot::OCCLUSION, path, false);
            loadSlot(ALPackSlot::ROUGHNESS, path, false);
            loadSlot(ALPackSlot::METALLIC, path, false);
            setSlotRouting(ALPackSlot::OCCLUSION, ALPackChannel::RED, false);
            setSlotRouting(ALPackSlot::ROUGHNESS, ALPackChannel::GREEN, false);
            setSlotRouting(ALPackSlot::METALLIC, ALPackChannel::BLUE, false);
            ++assigned;
            continue;
        }

        if (contains_any(name, sCombinedMRTokens, LL_ARRAY_SIZE(sCombinedMRTokens)))
        {
            loadSlot(ALPackSlot::ROUGHNESS, path, false);
            loadSlot(ALPackSlot::METALLIC, path, false);
            setSlotRouting(ALPackSlot::ROUGHNESS, ALPackChannel::GREEN, false);
            setSlotRouting(ALPackSlot::METALLIC, ALPackChannel::BLUE, false);
            ++assigned;
            continue;
        }

        // Longest match wins rather than first match, so a name carrying more
        // than one token ("rock_normal_col") is decided by the most specific
        // one instead of by the order of this table.
        const SlotHint* best = nullptr;
        size_t best_length = 0;
        for (const SlotHint& hint : sSlotHints)
        {
            const size_t length = strlen(hint.mToken);
            if (length > best_length && name.find(hint.mToken) != std::string::npos)
            {
                best = &hint;
                best_length = length;
            }
        }

        if (best)
        {
            loadSlot(best->mSlot, path, false);
            setSlotRouting(best->mSlot, ALPackChannel::RED, best->mImpliesInvert);
            ++assigned;
        }
        else
        {
            unmatched.push_back(gDirUtilp->getBaseFileName(path));
        }
    }

    if (!unmatched.empty())
    {
        std::string list;
        for (const std::string& name : unmatched)
        {
            if (!list.empty())
            {
                list += ", ";
            }
            list += name;
        }
        setStatus(llformat("Assigned %d file%s. Could not place: %s",
                           assigned, assigned == 1 ? "" : "s", list.c_str()));
    }
}

void ALFloaterPBRPacker::applyPreset(S32 preset)
{
    switch (preset)
    {
    case PRESET_PACKED_ORM: // one already-packed mask read three ways
        setSlotRouting(ALPackSlot::OCCLUSION, ALPackChannel::RED, false);
        setSlotRouting(ALPackSlot::ROUGHNESS, ALPackChannel::GREEN, false);
        setSlotRouting(ALPackSlot::METALLIC, ALPackChannel::BLUE, false);
        break;

    case PRESET_SEPARATE_GLOSS: // separate maps, roughness supplied as gloss
        setSlotRouting(ALPackSlot::OCCLUSION, ALPackChannel::RED, false);
        setSlotRouting(ALPackSlot::ROUGHNESS, ALPackChannel::RED, true);
        setSlotRouting(ALPackSlot::METALLIC, ALPackChannel::RED, false);
        break;

    case PRESET_SEPARATE:
    default:
        setSlotRouting(ALPackSlot::OCCLUSION, ALPackChannel::RED, false);
        setSlotRouting(ALPackSlot::ROUGHNESS, ALPackChannel::RED, false);
        setSlotRouting(ALPackSlot::METALLIC, ALPackChannel::RED, false);
        break;
    }
}

void ALFloaterPBRPacker::onPresetChanged()
{
    if (!mPresetCombo)
    {
        return;
    }

    applyPreset(mPresetCombo->getValue().asInteger());
    onSettingChanged();
}

void ALFloaterPBRPacker::loadSlot(ALPackSlot slot, const std::string& path, bool auto_repack)
{
    LL::WorkQueue::ptr_t main_queue = LL::WorkQueue::getInstance("mainloop");
    LL::WorkQueue::ptr_t general_queue = LL::WorkQueue::getInstance("General");
    if (!main_queue || !general_queue)
    {
        setStatus("Worker threads unavailable.");
        return;
    }

    setStatus("Loading " + gDirUtilp->getBaseFileName(path) + "...");

    auto result = std::make_shared<LoadResult>();
    LLHandle<ALFloaterPBRPacker> handle = getDerivedHandle<ALFloaterPBRPacker>();

    ++mPendingLoads;
    if (auto_repack)
    {
        mRepackWhenLoaded = true;
    }

    const bool posted = main_queue->postTo(
        general_queue,
        [path, result]() { result->mImage = ALPBRPacker::loadRaw(path, result->mError); },
        [handle, slot, path, result]()
        {
            ALFloaterPBRPacker* self = handle.get();
            if (!self)
            {
                return;
            }

            --self->mPendingLoads;

            if (result->mImage.isNull())
            {
                // A decode failure during a watch is usually the paint program
                // still writing. Leave the slot alone; the next tick retries.
                self->setStatus(result->mError);
            }
            else
            {
                self->mInputs[(size_t)slot] = result->mImage;
                self->mSlotPaths[(size_t)slot] = path;
                self->noteSourceTime(slot);

                // Keep a small copy for the slot thumbnail so toggling channel
                // or invert never has to touch the full-size image again.
                self->mSlotThumbSrc[(size_t)slot] = result->mImage->scaled(SLOT_THUMB_SIZE, SLOT_THUMB_SIZE);
                self->mSlotThumbDirty[(size_t)slot] = true;

                // Whatever was packed no longer matches the inputs, so retire
                // it rather than leaving Save and Apply pointed at stale maps.
                self->clearOutputs();

                self->setStatus(llformat("%s: %dx%d",
                                         ALPBRPacker::slotName(slot),
                                         (S32)result->mImage->getWidth(),
                                         (S32)result->mImage->getHeight()));
            }

            self->refreshControls();

            if (self->mPendingLoads == 0 && self->mRepackWhenLoaded)
            {
                self->mRepackWhenLoaded = false;
                self->onPack();
            }
        });

    if (!posted)
    {
        // The reply will never run, so unwind here or auto re-pack would stay
        // blocked behind a load that never lands.
        --mPendingLoads;
        mRepackWhenLoaded = false;
        setStatus("Could not queue the decode.");
    }
}

void ALFloaterPBRPacker::noteSourceTime(ALPackSlot slot)
{
    const size_t index = (size_t)slot;
    mSourceSettling[index] = false;

    const std::string& path = mSlotPaths[index];
    if (path.empty())
    {
        return;
    }

    std::error_code ec;
    const auto stamp = std::filesystem::last_write_time(fsyspath(path), ec);
    if (!ec)
    {
        mSourceTimes[index] = stamp;
    }
}

bool ALFloaterPBRPacker::tick()
{
    if (!mAutoRepackCheck || !mAutoRepackCheck->getValue().asBoolean())
    {
        return false;
    }

    // Do not stack work: a pack in flight or a load still landing means the
    // next tick is soon enough.
    if (mPacking || mPendingLoads > 0)
    {
        return false;
    }

    for (const SlotDef& def : sSlotDefs)
    {
        const size_t index = (size_t)def.mSlot;
        const std::string& path = mSlotPaths[index];
        if (path.empty())
        {
            continue;
        }

        std::error_code ec;
        const auto stamp = std::filesystem::last_write_time(fsyspath(path), ec);
        if (ec)
        {
            // Missing or momentarily locked. Try again next tick rather than
            // tearing the slot down -- exports routinely replace files.
            continue;
        }

        if (stamp != mSourceTimes[index])
        {
            // Seen it move, but a DCC tool writes progressively. Record the new
            // stamp and require it to hold still for a full tick before decoding.
            mSourceTimes[index] = stamp;
            mSourceSettling[index] = true;
            continue;
        }

        if (mSourceSettling[index])
        {
            mSourceSettling[index] = false;
            loadSlot(def.mSlot, path, true);
        }
    }

    return false;
}

void ALFloaterPBRPacker::onClear(ALPackSlot slot)
{
    mInputs[(size_t)slot] = nullptr;
    mSlotPaths[(size_t)slot].clear();
    mSourceSettling[(size_t)slot] = false;
    mSlotThumbSrc[(size_t)slot] = nullptr;
    mSlotThumbTex[(size_t)slot] = nullptr;
    onSettingChanged();
}

void ALFloaterPBRPacker::onSettingChanged()
{
    // Channel routing changed, so any packed result on screen is stale, and
    // every slot thumbnail has to be re-rendered to match the new routing.
    clearOutputs();
    markSlotThumbsDirty();
    refreshControls();
}

ALPBRPackRecipe ALFloaterPBRPacker::buildRecipe() const
{
    ALPBRPackRecipe recipe = ALPBRPackRecipe::secondLifeDefault();

    // The three scalar maps that make up ORM, in channel order.
    const struct { ALPackSlot mSlot; S32 mChannelIndex; } orm_bindings[] =
    {
        { ALPackSlot::OCCLUSION, 0 },
        { ALPackSlot::ROUGHNESS, 1 },
        { ALPackSlot::METALLIC,  2 },
    };

    if (ALPackTarget* orm = find_target(recipe, LLGLTFMaterial::GLTF_TEXTURE_INFO_METALLIC_ROUGHNESS))
    {
        for (const auto& binding : orm_bindings)
        {
            const SlotUI& ui = mSlotUI[(size_t)binding.mSlot];
            ALPackChannelSource& channel = orm->mChannels[binding.mChannelIndex];
            channel = ALPackChannelSource::from(
                binding.mSlot,
                ui.mChannel ? channelFromIndex(ui.mChannel->getValue().asInteger()) : ALPackChannel::RED,
                slotInverted(binding.mSlot));
        }
    }

    // A separate opacity map takes over base colour's alpha. Without one the
    // default recipe keeps the base colour image's own alpha, which is what a
    // creator exporting a single RGBA base colour expects.
    if (mInputs[(size_t)ALPackSlot::OPACITY].notNull())
    {
        if (ALPackTarget* base = find_target(recipe, LLGLTFMaterial::GLTF_TEXTURE_INFO_BASE_COLOR))
        {
            const SlotUI& ui = mSlotUI[(size_t)ALPackSlot::OPACITY];
            base->mChannels[3] = ALPackChannelSource::from(
                ALPackSlot::OPACITY,
                ui.mChannel ? channelFromIndex(ui.mChannel->getValue().asInteger()) : ALPackChannel::RED,
                slotInverted(ALPackSlot::OPACITY));
        }
    }

    // glTF wants OpenGL-convention normals (+Y up). A DirectX-convention source
    // is corrected by inverting green.
    if (slotInverted(ALPackSlot::NORMAL))
    {
        if (ALPackTarget* normal = find_target(recipe, LLGLTFMaterial::GLTF_TEXTURE_INFO_NORMAL))
        {
            normal->mChannels[1].mInvert = true;
        }
    }

    return recipe;
}

void ALFloaterPBRPacker::onPack()
{
    if (mPacking || !hasAnyInput())
    {
        return;
    }

    LL::WorkQueue::ptr_t main_queue = LL::WorkQueue::getInstance("mainloop");
    LL::WorkQueue::ptr_t general_queue = LL::WorkQueue::getInstance("General");
    if (!main_queue || !general_queue)
    {
        setStatus("Worker threads unavailable.");
        return;
    }

    const ALPackInputSet inputs = mInputs;
    const ALPBRPackRecipe recipe = buildRecipe();
    // "Auto Size" is stored as 0: no explicit cap, so the pack follows the
    // largest source map and the engine's own ceiling applies. Resolved here
    // rather than in the engine so the size warnings compare against the same
    // number the pack will use.
    S32 max_dim = mMaxSizeCombo ? mMaxSizeCombo->getValue().asInteger() : 0;
    if (max_dim <= 0)
    {
        max_dim = AL_PBR_PACK_MAX_DIM;
    }
    const std::string stem = mTempStem;

    mPacking = true;
    clearOutputs();
    refreshControls();
    setStatus("Packing...");

    auto result = std::make_shared<PackResult>();
    LLHandle<ALFloaterPBRPacker> handle = getDerivedHandle<ALFloaterPBRPacker>();

    const bool posted = main_queue->postTo(
        general_queue,
        [inputs, recipe, max_dim, stem, result]()
        {
            // Scanning pixels for advisory warnings belongs here rather than on
            // the main thread, even strided.
            collect_warnings(inputs, max_dim, result->mWarnings);

            result->mOk = ALPBRPacker::pack(inputs, recipe, max_dim, result->mOutputs, result->mError);
            if (!result->mOk)
            {
                return;
            }

            // Write the packed maps out here rather than on the main thread --
            // a 2048 square PNG encode is far too slow to do during a frame.
            for (const ALPackOutput& output : result->mOutputs)
            {
                const std::string path = stem + "_" + sanitize_for_filename(output.mName) + ".png";

                LLPointer<LLImagePNG> png = new LLImagePNG;
                if (!png->encode(output.mImage, 0.f) || !png->save(path))
                {
                    result->mOk = false;
                    result->mError = "Could not write packed file " + gDirUtilp->getBaseFileName(path);
                    return;
                }
                result->mPaths.push_back(path);
            }
        },
        [handle, result]()
        {
            ALFloaterPBRPacker* self = handle.get();
            if (!self)
            {
                return;
            }

            self->mPacking = false;

            if (!result->mOk)
            {
                self->setStatus(result->mError);
                self->refreshControls();
                return;
            }

            self->mOutputs = result->mOutputs;
            self->mOutputPaths = result->mPaths;
            self->mOutputThumbsDirty = true;

            // Each packed map reports its own size on its card, so the status
            // line is left for warnings.
            for (const ALPackOutput& output : self->mOutputs)
            {
                const size_t slot = (size_t)output.mDest;
                if (slot < OUTPUT_COUNT && self->mOutputUI[slot].mSize)
                {
                    self->mOutputUI[slot].mSize->setText(
                        llformat("%d x %d",
                                 (S32)output.mImage->getWidth(),
                                 (S32)output.mImage->getHeight()));
                }
            }

            std::string summary;
            for (const std::string& warning : result->mWarnings)
            {
                if (!summary.empty())
                {
                    summary += "   ";
                }
                summary += warning;
            }
            if (summary.empty())
            {
                summary = llformat("Packed %d texture%s.",
                                   (S32)self->mOutputs.size(),
                                   self->mOutputs.size() == 1 ? "" : "s");
            }
            self->setStatus(summary);

            // A local material already exists for this set, so rewrite its
            // glTF and reload it -- that is what makes auto re-pack show up
            // in-world without the user pressing anything.
            if (self->mLocalMaterialId.notNull())
            {
                std::string ignored_error;
                if (self->writeLocalMaterialFile(self->mGltfPath, ignored_error))
                {
                    if (LLLocalGLTFMaterialMgr* mgr = LLLocalGLTFMaterialMgr::getInstance())
                    {
                        mgr->doUpdates();
                    }
                }
            }

            self->refreshControls();
        });

    if (!posted)
    {
        // Same reasoning as loadSlot: without the reply, mPacking would latch
        // on and leave every button disabled.
        mPacking = false;
        setStatus("Could not queue the pack.");
        refreshControls();
    }
}

void ALFloaterPBRPacker::onSave()
{
    if (mOutputs.empty())
    {
        return;
    }

    LLHandle<ALFloaterPBRPacker> handle = getDerivedHandle<ALFloaterPBRPacker>();

    LLFilePickerReplyThread::startPicker(
        [handle](const std::vector<std::string>& filenames,
                 LLFilePicker::ELoadFilter,
                 LLFilePicker::ESaveFilter)
        {
            if (ALFloaterPBRPacker* self = handle.get())
            {
                self->onSaveLocationPicked(filenames);
            }
        },
        LLFilePicker::FFSAVE_PNG,
        std::string("packed"));
}

void ALFloaterPBRPacker::onSaveLocationPicked(const std::vector<std::string>& filenames)
{
    if (filenames.empty() || mOutputs.empty())
    {
        return;
    }

    // The picker gives us one filename; each packed map is written beside it
    // with its own suffix.
    const std::string chosen = filenames.front();
    const std::string directory = gDirUtilp->getDirName(chosen);
    const std::string base = gDirUtilp->getBaseFileName(chosen, true);

    S32 written = 0;
    for (const ALPackOutput& output : mOutputs)
    {
        const std::string path = gDirUtilp->add(directory,
                                                base + "_" + sanitize_for_filename(output.mName) + ".png");

        LLPointer<LLImagePNG> png = new LLImagePNG;
        if (!png->encode(output.mImage, 0.f) || !png->save(path))
        {
            setStatus("Could not write " + gDirUtilp->getBaseFileName(path));
            return;
        }
        ++written;
    }

    setStatus(llformat("Wrote %d file%s to %s", written, written == 1 ? "" : "s", directory.c_str()));
}

bool ALFloaterPBRPacker::writeLocalMaterialFile(std::string& path_out, std::string& error_out)
{
    if (mOutputs.empty() || mOutputPaths.size() != mOutputs.size())
    {
        error_out = "Nothing has been packed yet.";
        return false;
    }

    tinygltf::Model model;
    model.asset.version = "2.0";
    model.asset.generator = "Alchemy PBR Texture Packer";

    model.samplers.emplace_back();

    model.materials.resize(1);
    tinygltf::Material& material = model.materials[0];
    material.name = suggestedMaterialName();
    material.doubleSided = false;

    for (size_t i = 0; i < mOutputs.size(); ++i)
    {
        // Reference the packed files by bare name: saveModel writes external
        // image URIs, and the glTF is written into the same directory.
        tinygltf::Image image;
        image.uri = gDirUtilp->getBaseFileName(mOutputPaths[i]);
        const int image_index = (int)model.images.size();
        model.images.push_back(image);

        tinygltf::Texture texture;
        texture.source = image_index;
        texture.sampler = 0;
        const int texture_index = (int)model.textures.size();
        model.textures.push_back(texture);

        switch (mOutputs[i].mDest)
        {
        case LLGLTFMaterial::GLTF_TEXTURE_INFO_BASE_COLOR:
            material.pbrMetallicRoughness.baseColorTexture.index = texture_index;
            break;

        case LLGLTFMaterial::GLTF_TEXTURE_INFO_NORMAL:
            material.normalTexture.index = texture_index;
            break;

        // GLTF_TEXTURE_INFO_OCCLUSION aliases onto this one; the single packed
        // image serves both, which is the ORM convention Second Life relies on
        // and is spec-compliant.
        case LLGLTFMaterial::GLTF_TEXTURE_INFO_METALLIC_ROUGHNESS:
            material.pbrMetallicRoughness.metallicRoughnessTexture.index = texture_index;
            material.occlusionTexture.index = texture_index;
            break;

        case LLGLTFMaterial::GLTF_TEXTURE_INFO_EMISSIVE:
            material.emissiveTexture.index = texture_index;
            // glTF defaults emissiveFactor to black, which would multiply the
            // emissive map away to nothing.
            material.emissiveFactor = { 1.0, 1.0, 1.0 };
            break;

        default:
            break;
        }
    }

    path_out = mTempStem + ".gltf";
    if (!LLTinyGLTFHelper::saveModel(path_out, model))
    {
        error_out = "Could not write " + gDirUtilp->getBaseFileName(path_out);
        return false;
    }

    return true;
}

void ALFloaterPBRPacker::onMakeLocalMaterial()
{
    if (mOutputs.empty())
    {
        return;
    }

    std::string error;
    if (!writeLocalMaterialFile(mGltfPath, error))
    {
        setStatus(error);
        return;
    }

    LLLocalGLTFMaterialMgr* material_mgr = LLLocalGLTFMaterialMgr::getInstance();
    if (!material_mgr)
    {
        setStatus("The local material system is unavailable.");
        return;
    }

    const std::string name = suggestedMaterialName();

    if (mLocalMaterialId.notNull())
    {
        // Already registered. The glTF was just rewritten, so prod the manager
        // rather than adding a second unit for the same file.
        material_mgr->doUpdates();
        setStatus("Updated local material \"" + name + "\".");
        return;
    }

    LLUUID tracking_id;
    if (material_mgr->addUnit(mGltfPath, tracking_id) < 1 || tracking_id.isNull())
    {
        setStatus("Could not create a local material.");
        return;
    }

    mLocalMaterialId = tracking_id;
    setStatus("Local material \"" + name + "\" is ready -- pick it from the Local tab of any material picker.");
    refreshControls();
}

LLPointer<LLImageRaw> ALFloaterPBRPacker::outputFor(LLGLTFMaterial::TextureInfo dest) const
{
    for (const ALPackOutput& output : mOutputs)
    {
        if (output.mDest == dest)
        {
            return output.mImage;
        }
    }
    return nullptr;
}

std::string ALFloaterPBRPacker::suggestedMaterialName() const
{
    // Name the material after whichever source map the creator supplied first,
    // which is nearly always <material>_BaseColor and so reads correctly once
    // the suffix is trimmed.
    for (const SlotDef& def : sSlotDefs)
    {
        const std::string& path = mSlotPaths[(size_t)def.mSlot];
        if (path.empty())
        {
            continue;
        }

        std::string name = gDirUtilp->getBaseFileName(path, true);
        const size_t underscore = name.find_last_of('_');
        if (underscore != std::string::npos && underscore > 0)
        {
            name = name.substr(0, underscore);
        }
        if (!name.empty())
        {
            return name;
        }
    }

    return std::string("Packed Material");
}

void ALFloaterPBRPacker::onSendToMaterialEditor()
{
    if (mOutputs.empty())
    {
        return;
    }

    LLMaterialEditor* editor = (LLMaterialEditor*)LLFloaterReg::getInstance("material_editor");
    if (!editor)
    {
        setStatus("Could not open the material editor.");
        return;
    }

    editor->setFromPackedTextures(outputFor(LLGLTFMaterial::GLTF_TEXTURE_INFO_BASE_COLOR),
                                  outputFor(LLGLTFMaterial::GLTF_TEXTURE_INFO_NORMAL),
                                  outputFor(LLGLTFMaterial::GLTF_TEXTURE_INFO_METALLIC_ROUGHNESS),
                                  outputFor(LLGLTFMaterial::GLTF_TEXTURE_INFO_EMISSIVE),
                                  suggestedMaterialName());

    setStatus("Sent to the material editor.");
}

void ALFloaterPBRPacker::refreshControls()
{
    for (const SlotDef& def : sSlotDefs)
    {
        const SlotUI& ui = mSlotUI[(size_t)def.mSlot];
        const std::string& path = mSlotPaths[(size_t)def.mSlot];
        const bool filled = !path.empty();

        // With the filename no longer shown on the card, the full path lives
        // in the tooltip. An empty slot goes back to the XUI hint so the click
        // target still explains itself.
        const std::string tip = filled ? path : mSlotEmptyTip[(size_t)def.mSlot];
        if (ui.mThumb)
        {
            ui.mThumb->setToolTip(tip);
        }
        if (ui.mCard)
        {
            ui.mCard->setToolTip(tip);
        }
        if (ui.mClear)
        {
            ui.mClear->setEnabled(filled);
        }
        if (ui.mChannel)
        {
            ui.mChannel->setEnabled(filled);
        }
        if (ui.mInvert)
        {
            ui.mInvert->setEnabled(filled);
        }
    }

    if (mPackBtn)
    {
        mPackBtn->setEnabled(!mPacking && hasAnyInput());
    }
    if (mSaveBtn)
    {
        mSaveBtn->setEnabled(!mPacking && !mOutputs.empty());
    }
    if (mMakeLocalBtn)
    {
        mMakeLocalBtn->setEnabled(!mPacking && !mOutputs.empty());
    }
    if (mSendToEditorBtn)
    {
        mSendToEditorBtn->setEnabled(!mPacking && !mOutputs.empty());
    }
}

void ALFloaterPBRPacker::setStatus(const std::string& message)
{
    if (mStatusText)
    {
        mStatusText->setText(message);
    }
}

void ALFloaterPBRPacker::draw()
{
    LLFloater::draw();

    // Per-slot thumbnails: what the pack will read out of each source, with
    // the current channel selection and inversion already applied.
    for (const SlotDef& def : sSlotDefs)
    {
        const size_t index = (size_t)def.mSlot;
        const SlotUI& ui = mSlotUI[index];
        if (!ui.mThumb)
        {
            continue;
        }

        // An empty slot still draws: draw_texture_in_rect paints the grey-and-X
        // placeholder for a null texture.
        if (mSlotThumbDirty[index] && mSlotThumbSrc[index].notNull())
        {
            rebuildSlotThumb(def.mSlot);
        }

        // Inset by the panel's border so it stays visible around the preview.
        LLRect thumb_rect = localRectOf(ui.mThumb);
        thumb_rect.stretch(-1);
        draw_texture_in_rect(mSlotThumbTex[index], thumb_rect);
    }

    if (mOutputThumbsDirty)
    {
        mOutputTex.fill(nullptr);
        for (const ALPackOutput& output : mOutputs)
        {
            const size_t slot = (size_t)output.mDest;
            if (slot < OUTPUT_COUNT)
            {
                mOutputTex[slot] = LLViewerTextureManager::getLocalTexture(output.mImage.get(), false);
            }
        }
        mOutputThumbsDirty = false;
    }

    // Packed results, each in its own card. A slot with no result draws
    // nothing, matching how an unfilled source card looks -- the card border
    // is what says the slot exists.
    for (const OutputDef& def : sOutputDefs)
    {
        const size_t slot = (size_t)def.mDest;
        const OutputUI& out = mOutputUI[slot];
        if (!out.mThumb)
        {
            continue;
        }

        LLRect rect = localRectOf(out.mThumb);
        rect.stretch(-1);
        draw_texture_in_rect(mOutputTex[slot], rect);
    }
}
