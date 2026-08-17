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
ALPackChannelSource ALPackChannelSource::fromOr(ALPackSlot slot, ALPackChannel channel, U8 absent, bool invert)
{
    ALPackChannelSource src = from(slot, channel, invert);
    src.mAbsent = (S16)absent;
    return src;
}

// static
ALPackChannelExpr ALPackChannelExpr::lerp(const ALPackChannelSource& a,
                                          const ALPackChannelSource& b,
                                          const ALPackChannelSource& t)
{
    ALPackChannelExpr expr;
    expr.mOp = ALPackOp::LERP;
    expr.mA = a;
    expr.mB = b;
    expr.mT = t;
    return expr;
}

ALPackChannelExpr& ALPackChannelExpr::scaledBy(const ALPackChannelSource& scale)
{
    mScale = scale;
    return *this;
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
ALPBRPackRecipe ALPBRPackRecipe::secondLifeFallback()
{
    ALPBRPackRecipe recipe;

    // Approximates a glTF PBR material with the three legacy textures, for
    // content that ships both. Every figure below is read off
    // class3/deferred/materialF.glsl rather than off the archived Khronos
    // spec-gloss conversion, because Second Life's legacy path is Blinn-Phong
    // with a probe reflection, not a spec-gloss microfacet model.

    // Diffuse. The emissive map has to be composited into the diffuse colour,
    // because the legacy renderer's only emission is
    // "color = mix(color, diffcol.rgb, emissive)" -- the mask lerps the lit
    // result towards the raw diffuse texel, so a red glow is only red if the
    // diffuse texel is red. Occlusion rides in as the scale, which also means
    // it modulates the emissive; that is wrong by the book and invisible in
    // practice, and it keeps the expression flat.
    //
    // The mask is MAX_RGB rather than luminance: a saturated red emitter has a
    // luminance of 0.21 and would come out barely emitting.
    {
        ALPackTarget target;
        target.mName = "Diffuse";
        target.mDest = AL_SPECGLOSS_DIFFUSE;
        target.mComponents = 4;

        const ALPackChannelSource mask =
            ALPackChannelSource::fromOr(ALPackSlot::EMISSIVE, ALPackChannel::MAX_RGB, 0);
        const ALPackChannelSource occlusion =
            ALPackChannelSource::from(ALPackSlot::OCCLUSION, ALPackChannel::RED);

        for (S8 c = 0; c < 3; ++c)
        {
            const ALPackChannel channel = (ALPackChannel)((U8)ALPackChannel::RED + c);
            target.mChannels[c] = ALPackChannelExpr::lerp(
                ALPackChannelSource::from(ALPackSlot::BASE_COLOR, channel),
                ALPackChannelSource::from(ALPackSlot::EMISSIVE, channel),
                mask).scaledBy(occlusion);
        }

        // Alpha is the emissive mask, so the face wants DIFFUSE_ALPHA_MODE_
        // EMISSIVE. With no emissive map this resolves to a constant zero and
        // the floater rebinds it to opacity instead.
        target.mChannels[3] = mask;
        recipe.mTargets.push_back(target);
    }

    // Normal is a straight copy, and glossiness is exactly one minus
    // roughness: materialF.glsl states that (1 - glossiness) is what the
    // legacy path treats as perceptual roughness, and feeds that same number
    // to sampleReflectionProbesLegacy to pick the probe mip. glTF roughness is
    // perceptual too, so the two domains line up with no curve fitting.
    //
    // An absent roughness map inverts its white neutral to zero glossiness,
    // which matches glTF defaulting roughnessFactor to fully rough.
    {
        ALPackTarget target;
        target.mName = "Normal";
        target.mDest = AL_SPECGLOSS_NORMAL;
        target.mComponents = 4;
        target.mChannels[0] = ALPackChannelSource::from(ALPackSlot::NORMAL, ALPackChannel::RED);
        target.mChannels[1] = ALPackChannelSource::from(ALPackSlot::NORMAL, ALPackChannel::GREEN);
        target.mChannels[2] = ALPackChannelSource::from(ALPackSlot::NORMAL, ALPackChannel::BLUE);
        target.mChannels[3] = ALPackChannelSource::from(ALPackSlot::ROUGHNESS, ALPackChannel::RED, true);
        recipe.mTargets.push_back(target);
    }

    // Specular is the metallic-roughness F0: dielectric 0.04 for insulators,
    // the base colour for metal. The map is sampled as sRGB -- the deferred
    // writer re-encodes with linear_to_srgb before storing it -- so the
    // dielectric constant is written in sRGB, not as 10/255.
    //
    // No alpha, so no environment intensity. That looks like the obvious home
    // for metalness and is not: applyLegacyEnv() takes the specular value and
    // never reads it, then does
    //     color = mix(color, reflected_color*0.5, envIntensity)
    // -- an untinted probe mixed *over* the surface colour, which is a clear
    // coat. Driving it from metalness washes a red metal towards grey sky.
    // The metal reflection comes from applyGlossEnv() instead, which multiplies
    // the probe by this map's rgb and is gated on glossiness alone, so a
    // polished metal already reflects in its own colour with the environment
    // slider at zero.
    {
        ALPackTarget target;
        target.mName = "Specular";
        target.mDest = AL_SPECGLOSS_SPECULAR;
        target.mComponents = 3;

        const ALPackChannelSource metallic =
            ALPackChannelSource::fromOr(ALPackSlot::METALLIC, ALPackChannel::RED, 0);

        for (S8 c = 0; c < 3; ++c)
        {
            const ALPackChannel channel = (ALPackChannel)((U8)ALPackChannel::RED + c);
            target.mChannels[c] = ALPackChannelExpr::lerp(
                ALPackChannelSource::constant(AL_PACK_DIELECTRIC_F0_SRGB),
                ALPackChannelSource::from(ALPackSlot::BASE_COLOR, channel),
                metallic);
        }

        recipe.mTargets.push_back(target);
    }

    return recipe;
}

// static
ALPBRPackRecipe ALPBRPackRecipe::forMode(ALPackMode mode)
{
    switch (mode)
    {
    case ALPackMode::SPEC_GLOSS:   return secondLifeSpecGloss();
    case ALPackMode::PBR_FALLBACK: return secondLifeFallback();
    default:                       return secondLifeDefault();
    }
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
    case ALPackChannel::MAX_RGB:
        if (components < 3)
        {
            return pixel[0];
        }
        return llmax(pixel[0], llmax(pixel[1], pixel[2]));
    }
    return pixel[0];
}

namespace
{
    // a * b / 255, rounded. 255 is the identity, which is what makes an absent
    // scale free and an absent occlusion map a no-op.
    inline U8 scaleByte(U8 a, U8 b)
    {
        return (b == 255) ? a : (U8)(((U32)a * (U32)b + 127u) / 255u);
    }

    inline U8 lerpByte(U8 a, U8 b, U8 t)
    {
        if (t == 0)   { return a; }
        if (t == 255) { return b; }
        return (U8)(((U32)a * (255u - t) + (U32)b * t + 127u) / 255u);
    }

    // A resolved operand: either a constant, or a channel of one conformed
    // image. Bound once per output channel rather than per pixel.
    struct Operand
    {
        const U8*     mData = nullptr;      // null: mConstant
        S8            mComponents = 0;
        ALPackChannel mChannel = ALPackChannel::RED;
        bool          mInvert = false;
        U8            mConstant = 255;

        U8 sample(U32 index) const
        {
            if (!mData)
            {
                return mConstant;
            }
            const U8 value = ALPBRPacker::sampleChannel(mData + (size_t)index * mComponents,
                                                        mComponents, mChannel);
            return mInvert ? (U8)(255 - value) : value;
        }
    };

    Operand bindOperand(const ALPackChannelSource& source,
                        const std::array<LLPointer<LLImageRaw>, (size_t)ALPackSlot::COUNT>& conformed)
    {
        Operand operand;

        if (source.isConstant())
        {
            operand.mConstant = source.mInvert ? (U8)(255 - source.mConstant) : source.mConstant;
            return operand;
        }

        const LLPointer<LLImageRaw>& image = conformed[(size_t)source.mSlot];
        if (image.isNull())
        {
            // Resolution already replaced every absent slot with a constant, so
            // this cannot happen; fall back to the identity rather than
            // dereferencing null if a future recipe finds a way.
            return operand;
        }

        operand.mData       = image->getData();
        operand.mComponents = image->getComponents();
        operand.mChannel    = source.mChannel;
        operand.mInvert     = source.mInvert;
        return operand;
    }

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

        // Resolve the bindings. An operand whose source map was not supplied
        // becomes a constant, so the fill loop below never has to reason about
        // missing inputs -- and because that constant is the identity for how
        // the operand is used, a whole conversion degrades gracefully as maps
        // are left out.
        std::array<ALPackChannelExpr, 4> resolved = target.mChannels;
        bool any_real_source = false;
        for (S8 c = 0; c < components; ++c)
        {
            for (ALPackChannelSource* source : operandsOf(resolved[c]))
            {
                if (source->isConstant())
                {
                    continue;
                }

                const LLPointer<LLImageRaw>& image = inputs[(size_t)source->mSlot];
                if (image.isNull() || image->isBufferInvalid())
                {
                    const U8 absent = (source->mAbsent >= 0)
                                    ? (U8)source->mAbsent
                                    : neutralValue(source->mSlot, source->mChannel);
                    *source = ALPackChannelSource::constant(absent);
                }
                else
                {
                    any_real_source = true;
                }
            }
        }

        // Nothing real contributes, so the whole texture would be a flat
        // neutral. Both material systems treat an absent texture as that same
        // identity, so omit it and save the upload.
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
            for (const ALPackChannelSource* source : operandsOf(resolved[c]))
            {
                if (source->isConstant())
                {
                    continue;
                }
                const LLPointer<LLImageRaw>& image = inputs[(size_t)source->mSlot];
                width = llmax(width, (S32)image->getWidth());
                height = llmax(height, (S32)image->getHeight());
            }
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
            for (const ALPackChannelSource* source : operandsOf(resolved[c]))
            {
                if (source->isConstant())
                {
                    continue;
                }

                const size_t slot = (size_t)source->mSlot;
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
                        err = target.mName + ": could not resize " + slotName(source->mSlot);
                        return false;
                    }
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
            // Every contributing image is locked for the whole fill: an
            // expression reads several at once, so they cannot be locked one
            // channel at a time as a plain routing pass could.
            std::vector<std::unique_ptr<LLImageDataSharedLock>> src_locks;
            for (const LLPointer<LLImageRaw>& image : conformed)
            {
                if (image.notNull())
                {
                    src_locks.push_back(std::make_unique<LLImageDataSharedLock>(image));
                }
            }

            LLImageDataLock lock_dst(packed);
            U8* dst = packed->getData();
            const U32 pixel_count = (U32)width * (U32)height;

            for (S8 c = 0; c < components; ++c)
            {
                const ALPackChannelExpr& expr = resolved[c];

                const Operand a     = bindOperand(expr.mA, conformed);
                const Operand b     = bindOperand(expr.mB, conformed);
                const Operand t     = bindOperand(expr.mT, conformed);
                const Operand scale = bindOperand(expr.mScale, conformed);

                // A plain constant copy is the common case by far -- every
                // neutral fill lands here -- so keep it a flat memset-alike
                // rather than paying for the general evaluator per pixel.
                if (expr.mOp == ALPackOp::COPY && a.mData == nullptr && scale.mData == nullptr)
                {
                    const U8 value = scaleByte(a.mConstant, scale.mConstant);
                    for (U32 i = 0; i < pixel_count; ++i)
                    {
                        dst[i * components + c] = value;
                    }
                    continue;
                }

                for (U32 i = 0; i < pixel_count; ++i)
                {
                    U8 value = a.sample(i);
                    if (expr.mOp == ALPackOp::LERP)
                    {
                        value = lerpByte(value, b.sample(i), t.sample(i));
                    }
                    dst[i * components + c] = scaleByte(value, scale.sample(i));
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
