/**
* @file alfloaterpbrpacker.h
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

#pragma once

#include <array>
#include <filesystem>
#include <string>
#include <vector>

#include "alpbrpacker.h"
#include "lleventtimer.h"
#include "llfloater.h"
#include "llviewertexture.h"

class LLButton;
class LLCheckBoxCtrl;
class LLComboBox;
class LLTextBox;

class ALFloaterPBRPacker final : public LLFloater, public LLEventTimer
{
    friend class LLFloaterReg;

private:
    ALFloaterPBRPacker(const LLSD& key);
    ~ALFloaterPBRPacker();

public:
    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void onClose(bool app_quitting) override;
    void draw() override;
    bool tick() override;

private:
    // One card per ingest slot. mInvert is a toggle button rather than a
    // checkbox so the state reads as a pressed icon and carries no label to
    // translate. mChannel and mInvert are only present where they mean
    // something -- see sSlotDefs.
    // mThumb is a transparent button rather than a panel: the preview is the
    // slot's hit area, since clicking the image is what people reach for
    // before hunting for a browse button.
    struct SlotUI
    {
        LLView*     mCard    = nullptr;
        LLButton*   mClear   = nullptr;
        LLComboBox* mChannel = nullptr;
        LLButton*   mInvert  = nullptr;
        LLButton*   mThumb   = nullptr;
    };

    // One card per packed texture, indexed by LLGLTFMaterial::TextureInfo so a
    // result can be filed straight into its slot.
    struct OutputUI
    {
        LLView*    mCard  = nullptr;
        LLView*    mThumb = nullptr;
        LLTextBox* mSize  = nullptr;
    };

    // Preset order must match the combo_box items in the XUI.
    enum EPreset
    {
        PRESET_SEPARATE = 0,      // three separate greyscale maps, all read from red
        PRESET_PACKED_ORM,        // one already-packed ORM/ARM mask, read R/G/B
        PRESET_SEPARATE_GLOSS,    // separate maps, roughness supplied as gloss
    };

    static constexpr size_t OUTPUT_COUNT = (size_t)LLGLTFMaterial::GLTF_TEXTURE_INFO_COUNT;

    void onBrowse(ALPackSlot slot);
    void onFilePicked(ALPackSlot slot, const std::vector<std::string>& filenames);
    void onClear(ALPackSlot slot);
    void onPack();
    void onSave();
    void onSaveLocationPicked(const std::vector<std::string>& filenames);
    void onSettingChanged();
    void onMakeLocalMaterial();
    void onSendToMaterialEditor();
    void onAddFiles();
    void onFilesPicked(const std::vector<std::string>& filenames);
    void onPresetChanged();
    void applyPreset(S32 preset);

    LLPointer<LLImageRaw> outputFor(LLGLTFMaterial::TextureInfo dest) const;
    std::string           suggestedMaterialName() const;

    void            refreshControls();
    void            setStatus(const std::string& message);
    ALPBRPackRecipe buildRecipe() const;
    bool            hasAnyInput() const;

    // Route a file to a slot with an explicit channel/invert, used by the
    // filename heuristics and the presets.
    void setSlotRouting(ALPackSlot slot, ALPackChannel channel, bool invert);

    // Rebuild a slot's thumbnail to show exactly what the pack will read out
    // of it -- selected channel, inversion applied.
    void rebuildSlotThumb(ALPackSlot slot);
    void markSlotThumbsDirty();

    // Drop the packed result and blank the output cards.
    void clearOutputs();

    // Write a glTF file beside the packed PNGs describing them as one material.
    // This is what the local material system consumes -- it loads from a
    // .gltf/.glb on disk and owns the textures itself.
    bool writeLocalMaterialFile(std::string& path_out, std::string& error_out);

    // Rect of a widget in this floater's own drawing coordinates. The slot
    // widgets live inside card panels, so their getRect() is card-relative.
    LLRect localRectOf(const LLView* view) const;

    bool slotInverted(ALPackSlot slot) const;

    // Kick off an off-thread decode of one source file into its slot.
    // auto_repack re-packs once every outstanding load has landed.
    void loadSlot(ALPackSlot slot, const std::string& path, bool auto_repack);
    void noteSourceTime(ALPackSlot slot);

    static ALPackChannel channelFromIndex(S32 index);

    std::array<SlotUI, (size_t)ALPackSlot::COUNT>     mSlotUI;
    std::array<std::string, (size_t)ALPackSlot::COUNT> mSlotPaths;
    // The per-slot "click to choose ..." hint from the XUI, kept so an empty
    // slot can show it again after a loaded slot replaced it with the path.
    std::array<std::string, (size_t)ALPackSlot::COUNT> mSlotEmptyTip;
    ALPackInputSet                                    mInputs;

    // Source-file watching. A changed timestamp is not acted on until it has
    // held still for a tick, because paint programs and bakers write
    // progressively and a half-written file decodes to garbage or not at all.
    std::array<std::filesystem::file_time_type, (size_t)ALPackSlot::COUNT> mSourceTimes;
    std::array<bool, (size_t)ALPackSlot::COUNT>                            mSourceSettling{};

    // Small copies of each source, kept so a channel or invert change can
    // re-render its thumbnail without touching the full-size image.
    std::array<LLPointer<LLImageRaw>, (size_t)ALPackSlot::COUNT>     mSlotThumbSrc;
    std::array<LLPointer<LLViewerTexture>, (size_t)ALPackSlot::COUNT> mSlotThumbTex;
    std::array<bool, (size_t)ALPackSlot::COUNT>                       mSlotThumbDirty{};

    std::array<OutputUI, OUTPUT_COUNT>                   mOutputUI;
    std::array<LLPointer<LLViewerTexture>, OUTPUT_COUNT> mOutputTex;

    ALPackOutputSet          mOutputs;
    std::vector<std::string> mOutputPaths;

    // Stem shared by every packed file this session writes, so a re-pack
    // overwrites in place and the local material picks up the change.
    std::string mTempStem;
    std::string mGltfPath;
    LLUUID      mLocalMaterialId;   // tracking id of the local material, if made

    LLButton*   mPackBtn     = nullptr;
    LLButton*   mSaveBtn     = nullptr;
    LLButton*   mMakeLocalBtn = nullptr;
    LLButton*   mSendToEditorBtn = nullptr;
    LLButton*   mAddFilesBtn = nullptr;
    LLComboBox* mPresetCombo = nullptr;
    LLComboBox* mMaxSizeCombo = nullptr;
    LLCheckBoxCtrl* mAutoRepackCheck = nullptr;
    LLTextBox*  mStatusText = nullptr;

    S32  mPendingLoads = 0;
    bool mRepackWhenLoaded = false;
    bool mPacking = false;
    bool mOutputThumbsDirty = false;
};
