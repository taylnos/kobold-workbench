/**
* @file alpbrpacker.cpp
* @brief Channel packing engine for glTF PBR material textures
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

// Deliberately linden_common.h rather than llviewerprecompiledheaders.h: this
// file is also compiled standalone by the unit test target, which has no
// viewer precompiled header.
#include "linden_common.h"

#include "alpbrpacker.h"

#include "lldir.h"
#include "llprofiler.h"

// static
ALPackChannelSource ALPackChannelSource::constant(U8 value)
{
    ALPackChannelSource src;
    src.mSlot = ALPackSlot::COUNT;
    src.mConstant = value;
    return src;
}

// static
ALPackChannelSource ALPackChannelSource::from(ALPackSlot slot, ALPackChannel channel, bool invert)
{
    ALPackChannelSource src;
    src.mSlot = slot;
    src.mChannel = channel;
    src.mInvert = invert;
    return src;
}

// static
ALPBRPackRecipe ALPBRPackRecipe::secondLifeDefault()
{
    ALPBRPackRecipe recipe;

    // Base Color. Alpha comes from the base colour image's own alpha channel;
    // when a creator supplies a separate opacity map the UI rebinds this
    // channel to ALPackSlot::OPACITY, so the engine needs no fallback rule.
    // A fully opaque alpha is dropped after packing by optimizeAwayAlpha().
    {
        ALPackTarget target;
        target.mName = "Base Color";
        target.mDest = LLGLTFMaterial::GLTF_TEXTURE_INFO_BASE_COLOR;
        target.mComponents = 4;
        target.mChannels[0] = ALPackChannelSource::from(ALPackSlot::BASE_COLOR, ALPackChannel::RED);
        target.mChannels[1] = ALPackChannelSource::from(ALPackSlot::BASE_COLOR, ALPackChannel::GREEN);
        target.mChannels[2] = ALPackChannelSource::from(ALPackSlot::BASE_COLOR, ALPackChannel::BLUE);
        target.mChannels[3] = ALPackChannelSource::from(ALPackSlot::BASE_COLOR, ALPackChannel::ALPHA);
        recipe.mTargets.push_back(target);
    }

    // Normal. glTF is OpenGL convention (+Y up); a DirectX-convention source is
    // corrected by setting mInvert on the green channel.
    {
        ALPackTarget target;
        target.mName = "Normal";
        target.mDest = LLGLTFMaterial::GLTF_TEXTURE_INFO_NORMAL;
        target.mComponents = 3;
        target.mChannels[0] = ALPackChannelSource::from(ALPackSlot::NORMAL, ALPackChannel::RED);
        target.mChannels[1] = ALPackChannelSource::from(ALPackSlot::NORMAL, ALPackChannel::GREEN);
        target.mChannels[2] = ALPackChannelSource::from(ALPackSlot::NORMAL, ALPackChannel::BLUE);
        recipe.mTargets.push_back(target);
    }

    // ORM -- the reason this feature exists. Occlusion red, roughness green,
    // metalness blue. A gloss/smoothness source is corrected by setting
    // mInvert on the green channel.
    {
        ALPackTarget target;
        target.mName = "Metallic Roughness";
        target.mDest = LLGLTFMaterial::GLTF_TEXTURE_INFO_METALLIC_ROUGHNESS;
        target.mComponents = 3;
        target.mChannels[0] = ALPackChannelSource::from(ALPackSlot::OCCLUSION, ALPackChannel::RED);
        target.mChannels[1] = ALPackChannelSource::from(ALPackSlot::ROUGHNESS, ALPackChannel::RED);
        target.mChannels[2] = ALPackChannelSource::from(ALPackSlot::METALLIC, ALPackChannel::RED);
        recipe.mTargets.push_back(target);
    }

    {
        ALPackTarget target;
        target.mName = "Emissive";
        target.mDest = LLGLTFMaterial::GLTF_TEXTURE_INFO_EMISSIVE;
        target.mComponents = 3;
        target.mChannels[0] = ALPackChannelSource::from(ALPackSlot::EMISSIVE, ALPackChannel::RED);
        target.mChannels[1] = ALPackChannelSource::from(ALPackSlot::EMISSIVE, ALPackChannel::GREEN);
        target.mChannels[2] = ALPackChannelSource::from(ALPackSlot::EMISSIVE, ALPackChannel::BLUE);
        recipe.mTargets.push_back(target);
    }

    return recipe;
}

// static
ALPBRPackRecipe ALPBRPackRecipe::secondLifeSpecGloss()
{
    ALPBRPackRecipe recipe;

    // Diffuse. Alpha is whatever the face's diffuse alpha mode makes of it --
    // transparency, an alpha mask, or an emissive mask. Which source feeds it
    // is the UI's call, since the three are mutually exclusive: LLMaterial
    // carries one mDiffuseAlphaMode, not one per interpretation.
    {
        ALPackTarget target;
        target.mName = "Diffuse";
        target.mDest = AL_SPECGLOSS_DIFFUSE;
        target.mComponents = 4;
        target.mChannels[0] = ALPackChannelSource::from(ALPackSlot::BASE_COLOR, ALPackChannel::RED);
        target.mChannels[1] = ALPackChannelSource::from(ALPackSlot::BASE_COLOR, ALPackChannel::GREEN);
        target.mChannels[2] = ALPackChannelSource::from(ALPackSlot::BASE_COLOR, ALPackChannel::BLUE);
        target.mChannels[3] = ALPackChannelSource::from(ALPackSlot::BASE_COLOR, ALPackChannel::ALPHA);
        recipe.mTargets.push_back(target);
    }

    // Normal, carrying glossiness in alpha. materialF.glsl reads it as
    // "glossiness *= vNt.a" against the Glossiness slider, so an absent
    // glossiness map neutral-fills to 255 and the slider stays in charge.
    // Second Life reads normals in the same OpenGL convention glTF does, so
    // the DirectX green flip applies here unchanged.
    {
        ALPackTarget target;
        target.mName = "Normal";
        target.mDest = AL_SPECGLOSS_NORMAL;
        target.mComponents = 4;
        target.mChannels[0] = ALPackChannelSource::from(ALPackSlot::NORMAL, ALPackChannel::RED);
        target.mChannels[1] = ALPackChannelSource::from(ALPackSlot::NORMAL, ALPackChannel::GREEN);
        target.mChannels[2] = ALPackChannelSource::from(ALPackSlot::NORMAL, ALPackChannel::BLUE);
        target.mChannels[3] = ALPackChannelSource::from(ALPackSlot::GLOSSINESS, ALPackChannel::RED);
        recipe.mTargets.push_back(target);
    }

    // Specular tint, carrying environment intensity in alpha -- read as
    // "env = env_intensity * spec.a" against the Environment slider.
    {
        ALPackTarget target;
        target.mName = "Specular";
        target.mDest = AL_SPECGLOSS_SPECULAR;
        target.mComponents = 4;
        target.mChannels[0] = ALPackChannelSource::from(ALPackSlot::SPECULAR_COLOR, ALPackChannel::RED);
        target.mChannels[1] = ALPackChannelSource::from(ALPackSlot::SPECULAR_COLOR, ALPackChannel::GREEN);
        target.mChannels[2] = ALPackChannelSource::from(ALPackSlot::SPECULAR_COLOR, ALPackChannel::BLUE);
        target.mChannels[3] = ALPackChannelSource::from(ALPackSlot::SPECULAR_ENV, ALPackChannel::RED);
        recipe.mTargets.push_back(target);
    }

    return recipe;
}

// static
ALPBRPackRecipe ALPBRPackRecipe::forMode(ALPackMode mode)
{
    return (mode == ALPackMode::SPEC_GLOSS) ? secondLifeSpecGloss() : secondLifeDefault();
}

U8 ALPBRPacker::sampleChannel(const U8* pixel, S8 components, ALPackChannel channel)
{
    switch (channel)
    {
    case ALPackChannel::RED:
        return pixel[0];
    case ALPackChannel::GREEN:
        return (components > 1) ? pixel[1] : pixel[0];
    case ALPackChannel::BLUE:
        return (components > 2) ? pixel[2] : pixel[0];
    case ALPackChannel::ALPHA:
        return (components > 3) ? pixel[3] : 255;
    case ALPackChannel::LUMINANCE:
        if (components < 3)
        {
            return pixel[0];
        }
        // Rec. 709 luma, fixed point: 0.2126 / 0.7152 / 0.0722 scaled by 256.
        return (U8)(((U32)pixel[0] * 54 + (U32)pixel[1] * 183 + (U32)pixel[2] * 19) >> 8);
    }
    return pixel[0];
}

namespace
{

    // Promote gray+alpha to RGBA. LLImageRaw::scale() only accepts 1, 3 or 4
    // components, so a 2-component decode would fail to resize later.
    LLPointer<LLImageRaw> normalize_components(LLPointer<LLImageRaw> raw)
    {
        if (raw.isNull() || raw->getComponents() != 2)
        {
            return raw;
        }

        const S32 width = raw->getWidth();
        const S32 height = raw->getHeight();
        LLPointer<LLImageRaw> expanded = new LLImageRaw((U16)width, (U16)height, 4);
        if (expanded->isBufferInvalid())
        {
            return nullptr;
        }

        LLImageDataSharedLock lock_src(raw);
        LLImageDataLock lock_dst(expanded);

        const U8* src = raw->getData();
        U8* dst = expanded->getData();
        const U32 pixel_count = (U32)width * (U32)height;
        for (U32 i = 0; i < pixel_count; ++i)
        {
            const U8 gray = src[i * 2];
            dst[i * 4 + 0] = gray;
            dst[i * 4 + 1] = gray;
            dst[i * 4 + 2] = gray;
            dst[i * 4 + 3] = src[i * 2 + 1];
        }

        return expanded;
    }
}

U8 ALPBRPacker::neutralValue(ALPackSlot slot, ALPackChannel channel)
{
    // A missing normal map is a flat tangent-space normal (0,0,1) encoded as
    // (128,128,255) -- not white, which would be a nonsensical (1,1,1) vector.
    if (slot == ALPackSlot::NORMAL)
    {
        switch (channel)
        {
        case ALPackChannel::RED:
        case ALPackChannel::GREEN:
        case ALPackChannel::LUMINANCE:
            return 128;
        default:
            return 255;
        }
    }

    // Everything else is the identity for how the renderer consumes it:
    // roughness and metallic are multiplied by their glTF factors, glossiness
    // and environment intensity by their legacy-material sliders, so 255 leaves
    // that control in charge rather than pinning the result to zero; occlusion
    // of 255 means unoccluded; base colour, diffuse, specular tint and emissive
    // are multiplied by their factors; a missing opacity is fully opaque.
    return 255;
}

const char* ALPBRPacker::slotName(ALPackSlot slot)
{
    switch (slot)
    {
    case ALPackSlot::BASE_COLOR:     return "Base Color";
    case ALPackSlot::EMISSIVE:       return "Emissive";
    case ALPackSlot::OCCLUSION:      return "Occlusion";
    case ALPackSlot::ROUGHNESS:      return "Roughness";
    case ALPackSlot::METALLIC:       return "Metallic";
    case ALPackSlot::NORMAL:         return "Normal";
    case ALPackSlot::OPACITY:        return "Opacity";
    case ALPackSlot::GLOSSINESS:     return "Glossiness";
    case ALPackSlot::SPECULAR_COLOR: return "Specular Color";
    case ALPackSlot::SPECULAR_ENV:   return "Specular Environment";
    default:                         return "Unknown";
    }
}

LLPointer<LLImageRaw> ALPBRPacker::loadRaw(const std::string& path, std::string& err)
{
    err.clear();

    const std::string extension = gDirUtilp->getExtension(path);
    const EImageCodec codec = LLImageBase::getCodecFromExtension(extension);
    if (codec == IMG_CODEC_INVALID)
    {
        err = "Unsupported image format: ." + extension;
        return nullptr;
    }

    LLPointer<LLImageFormatted> formatted = LLImageFormatted::createFromType((S8)codec);
    if (formatted.isNull())
    {
        err = "No decoder available for ." + extension;
        return nullptr;
    }

    if (!formatted->load(path))
    {
        err = "Could not read " + gDirUtilp->getBaseFileName(path);
        return nullptr;
    }

    // Matters for J2C, harmless elsewhere: decode at full resolution.
    formatted->setDiscardLevel(0);

    LLPointer<LLImageRaw> raw = new LLImageRaw;
    if (!formatted->decode(raw, 0.f) || raw->isBufferInvalid())
    {
        err = "Could not decode " + gDirUtilp->getBaseFileName(path);
        return nullptr;
    }

    raw = normalize_components(raw);
    if (raw.isNull())
    {
        err = "Out of memory decoding " + gDirUtilp->getBaseFileName(path);
        return nullptr;
    }

    return raw;
}

bool ALPBRPacker::pack(const ALPackInputSet& inputs,
                       const ALPBRPackRecipe& recipe,
                       S32 max_dim,
                       ALPackOutputSet& outputs,
                       std::string& err)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;

    outputs.clear();
    err.clear();

    if (max_dim <= 0)
    {
        max_dim = AL_PBR_PACK_MAX_DIM;
    }

    for (const ALPackTarget& target : recipe.mTargets)
    {
        if (!target.mEnabled)
        {
            continue;
        }

        const S8 components = target.mComponents;
        if (components < 1 || components > 4)
        {
            err = target.mName + ": invalid component count";
            return false;
        }

        // Resolve the bindings. A channel whose source map was not supplied
        // becomes its neutral constant, so the fill loop below never has to
        // reason about missing inputs.
        std::array<ALPackChannelSource, 4> resolved = target.mChannels;
        bool any_real_source = false;
        for (S8 c = 0; c < components; ++c)
        {
            ALPackChannelSource& source = resolved[c];
            if (source.isConstant())
            {
                continue;
            }

            const LLPointer<LLImageRaw>& image = inputs[(size_t)source.mSlot];
            if (image.isNull() || image->isBufferInvalid())
            {
                source = ALPackChannelSource::constant(neutralValue(source.mSlot, source.mChannel));
            }
            else
            {
                any_real_source = true;
            }
        }

        // Nothing real contributes, so the whole texture would be a flat
        // neutral. glTF treats an absent texture as that same identity, so
        // omit it and save the upload.
        if (!any_real_source)
        {
            continue;
        }

        // Output size is the largest contributing input, clamped and biased to
        // a power of two. Unlike the glTF import path this will upscale a
        // smaller map rather than always downsampling to one particular input.
        S32 width = 0;
        S32 height = 0;
        for (S8 c = 0; c < components; ++c)
        {
            if (resolved[c].isConstant())
            {
                continue;
            }
            const LLPointer<LLImageRaw>& image = inputs[(size_t)resolved[c].mSlot];
            width = llmax(width, (S32)image->getWidth());
            height = llmax(height, (S32)image->getHeight());
        }

        width = LLImageRaw::biasedDimToPowerOfTwo(width, max_dim);
        height = LLImageRaw::biasedDimToPowerOfTwo(height, max_dim);

        // Conform every contributing source to the output size once, rather
        // than per channel -- ORM reads three different slots, and a normal map
        // reads the same slot three times.
        //
        // Note scaled() takes an exclusive lock internally, and LLSharedMutex
        // asserts if a thread holding a shared lock asks for an exclusive one,
        // so no lock may be held here.
        std::array<LLPointer<LLImageRaw>, (size_t)ALPackSlot::COUNT> conformed;
        for (S8 c = 0; c < components; ++c)
        {
            if (resolved[c].isConstant())
            {
                continue;
            }

            const size_t slot = (size_t)resolved[c].mSlot;
            if (conformed[slot].notNull())
            {
                continue;
            }

            // Non-const copy of the pointer: LLPointer propagates constness
            // through operator->, and scaled() is non-const.
            LLPointer<LLImageRaw> image = inputs[slot];
            if (image->getWidth() == width && image->getHeight() == height)
            {
                conformed[slot] = image;
            }
            else
            {
                conformed[slot] = image->scaled(width, height);
                if (conformed[slot].isNull() || conformed[slot]->isBufferInvalid())
                {
                    err = target.mName + ": could not resize " + slotName(resolved[c].mSlot);
                    return false;
                }
            }
        }

        LLPointer<LLImageRaw> packed = new LLImageRaw((U16)width, (U16)height, components);
        if (packed->isBufferInvalid())
        {
            err = target.mName + ": out of memory allocating packed image";
            return false;
        }

        {
            LLImageDataLock lock_dst(packed);
            U8* dst = packed->getData();
            const U32 pixel_count = (U32)width * (U32)height;

            for (S8 c = 0; c < components; ++c)
            {
                const ALPackChannelSource& source = resolved[c];

                if (source.isConstant())
                {
                    const U8 value = source.mInvert ? (U8)(255 - source.mConstant) : source.mConstant;
                    for (U32 i = 0; i < pixel_count; ++i)
                    {
                        dst[i * components + c] = value;
                    }
                    continue;
                }

                const LLPointer<LLImageRaw>& src_image = conformed[(size_t)source.mSlot];
                LLImageDataSharedLock lock_src(src_image);

                const U8* src = src_image->getData();
                const S8 src_components = src_image->getComponents();

                for (U32 i = 0; i < pixel_count; ++i)
                {
                    const U8 value = sampleChannel(src + (size_t)i * src_components, src_components, source.mChannel);
                    dst[i * components + c] = source.mInvert ? (U8)(255 - value) : value;
                }
            }
        }

        // Drop a fully opaque alpha channel rather than spending a quarter of
        // the upload on constant 255.
        if (components == 4)
        {
            packed->optimizeAwayAlpha();
        }

        ALPackOutput output;
        output.mName = target.mName;
        output.mDest = target.mDest;
        output.mImage = packed;
        outputs.push_back(output);
    }

    if (outputs.empty())
    {
        err = "No source maps supplied.";
        return false;
    }

    return true;
}
