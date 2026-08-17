/**
* @file alpbrpacker.h
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

#pragma once

#include <array>
#include <string>
#include <vector>

#include "llgltfmaterial.h"
#include "llimage.h"
#include "llpointer.h"

// Largest packed output we will produce. Matches
// LLViewerFetchedTexture::MAX_IMAGE_SIZE_DEFAULT, restated here so the engine
// stays free of viewer texture headers.
constexpr S32 AL_PBR_PACK_MAX_DIM = 2048;

// The glTF metallic-roughness dielectric F0, 0.04 linear, written as the sRGB
// byte the legacy specular map is sampled as. 1.055 * 0.04^(1/2.4) - 0.055
// is 0.2209, so 56.
constexpr U8 AL_PACK_DIELECTRIC_F0_SRGB = 56;

// Which of Second Life's two material systems a recipe targets. They are the
// same channel-routing problem with different destinations, so they share the
// engine entirely and differ only in their recipe table.
enum class ALPackMode : U8
{
    GLTF_PBR = 0,   // base colour / normal / ORM / emissive
    SPEC_GLOSS,     // diffuse / normal+gloss / specular+environment
    PBR_FALLBACK,   // glTF maps in, SpecGloss approximation out
    COUNT
};

// The maps a creator feeds in. These are authoring-side concepts and do not
// map one-to-one onto the textures Second Life actually stores -- that
// translation is the whole point of a recipe. Several are shared between the
// two modes under different names: BASE_COLOR is the SpecGloss diffuse map,
// and EMISSIVE is its emissive mask.
enum class ALPackSlot : U8
{
    BASE_COLOR = 0,
    EMISSIVE,
    OCCLUSION,
    ROUGHNESS,
    METALLIC,
    NORMAL,
    OPACITY,
    GLOSSINESS,
    SPECULAR_COLOR,
    SPECULAR_ENV,
    COUNT
};

// Destinations for ALPackMode::SPEC_GLOSS. Second Life's legacy materials
// carry six authoring maps in three uploads, because glossiness rides in the
// normal map's alpha and environment intensity in the specular map's --
// see materialF.glsl, which multiplies the Glossiness and Environment sliders
// by those two channels.
enum ALSpecGlossTexture : U8
{
    AL_SPECGLOSS_DIFFUSE = 0,
    AL_SPECGLOSS_NORMAL,
    AL_SPECGLOSS_SPECULAR,
    AL_SPECGLOSS_COUNT
};

// Where a packed image lands. Interpreted against the recipe's mode:
// LLGLTFMaterial::TextureInfo for GLTF_PBR, ALSpecGlossTexture for SPEC_GLOSS.
using ALPackDest = U8;

// Enough destination slots for either mode, so the UI can hold one fixed array
// of output cards rather than one per mode.
constexpr size_t AL_PACK_MAX_OUTPUTS = 4;
static_assert((size_t)LLGLTFMaterial::GLTF_TEXTURE_INFO_COUNT <= AL_PACK_MAX_OUTPUTS,
              "glTF gained a texture slot; widen AL_PACK_MAX_OUTPUTS");
static_assert((size_t)AL_SPECGLOSS_COUNT <= AL_PACK_MAX_OUTPUTS,
              "SpecGloss gained a texture slot; widen AL_PACK_MAX_OUTPUTS");

// Which channel of a source image feeds one output channel. LUMINANCE is for
// the case where a creator hands us a colour image for a scalar map; MAX_RGB
// answers "is there anything here at all", which is what an emissive mask
// wants -- a saturated red emitter has a low luminance but is fully emitting.
enum class ALPackChannel : U8
{
    RED = 0,
    GREEN,
    BLUE,
    ALPHA,
    LUMINANCE,
    MAX_RGB
};

// One operand: either a channel of an ingest slot, or a flat constant. An
// unbound slot resolves to a constant during recipe resolution, so the pack
// loop never has to special-case a missing map.
struct ALPackChannelSource
{
    ALPackSlot    mSlot     = ALPackSlot::COUNT;    // COUNT means "use mConstant"
    ALPackChannel mChannel  = ALPackChannel::RED;
    bool          mInvert   = false;
    U8            mConstant = 255;

    // What this operand becomes when its map is absent. Negative means "ask
    // neutralValue()", which answers for how the *material* consumes the slot.
    // An expression can consume the same slot differently and has to say so:
    // metalness neutral-fills to white as an ORM channel, but as the factor
    // interpolating towards a metal tint the safe absent value is black.
    S16 mAbsent = -1;

    bool isConstant() const { return mSlot == ALPackSlot::COUNT; }

    static ALPackChannelSource constant(U8 value);
    static ALPackChannelSource from(ALPackSlot slot, ALPackChannel channel, bool invert = false);
    // As from(), but with an explicit value for when the map is missing.
    static ALPackChannelSource fromOr(ALPackSlot slot, ALPackChannel channel, U8 absent, bool invert = false);
};

enum class ALPackOp : U8
{
    COPY = 0,   // mA
    LERP,       // mA + (mB - mA) * mT
};

// What one output channel evaluates to. Deliberately flat rather than a tree:
// every conversion this needs is an interpolation with an optional modulator,
// and keeping it flat keeps the UI's routing overlay a simple walk.
struct ALPackChannelExpr
{
    ALPackChannelExpr() = default;

    // Implicit, so a recipe that just routes a channel reads as it always did.
    ALPackChannelExpr(const ALPackChannelSource& source) : mA(source) {}

    ALPackOp            mOp = ALPackOp::COPY;
    ALPackChannelSource mA;
    ALPackChannelSource mB;
    ALPackChannelSource mT;
    // Multiplied into the result. Defaults to a constant 255, the identity, so
    // it costs nothing where it is not wanted. This is how occlusion folds into
    // a diffuse map without the expression needing to nest.
    ALPackChannelSource mScale;

    static ALPackChannelExpr lerp(const ALPackChannelSource& a,
                                  const ALPackChannelSource& b,
                                  const ALPackChannelSource& t);
    ALPackChannelExpr& scaledBy(const ALPackChannelSource& scale);
};

// The operands of an expression, so resolution, sizing, conforming and the
// UI's routing overlay each walk them without repeating the list.
inline std::array<ALPackChannelSource*, 4> operandsOf(ALPackChannelExpr& expr)
{
    return { &expr.mA, &expr.mB, &expr.mT, &expr.mScale };
}

inline std::array<const ALPackChannelSource*, 4> operandsOf(const ALPackChannelExpr& expr)
{
    return { &expr.mA, &expr.mB, &expr.mT, &expr.mScale };
}

// One packed image to produce.
struct ALPackTarget
{
    std::string                      mName;
    ALPackDest                       mDest = 0;
    S8                               mComponents = 3;
    std::array<ALPackChannelExpr, 4> mChannels;
    bool                             mEnabled = true;
};

// The full packing description. Kept as a plain list so that adding a texture
// slot Second Life does not have today (occlusion as its own map, clearcoat,
// sheen, anisotropy...) is a table entry rather than an engine change. The
// same is what let the SpecGloss mode be a second table rather than a second
// engine.
class ALPBRPackRecipe
{
public:
    // Base Color / Normal / ORM / Emissive -- what SL's glTF materials store.
    static ALPBRPackRecipe secondLifeDefault();

    // Diffuse / Normal+Glossiness / Specular+Environment -- what SL's legacy
    // materials store. No JSON asset exists for these, so the outputs are
    // uploaded as loose textures and assigned per face.
    static ALPBRPackRecipe secondLifeSpecGloss();

    // The same three legacy textures, approximated from glTF PBR maps, for a
    // fallback material on content that also ships a PBR version.
    static ALPBRPackRecipe secondLifeFallback();

    static ALPBRPackRecipe forMode(ALPackMode mode);

    std::vector<ALPackTarget> mTargets;
};

using ALPackInputSet = std::array<LLPointer<LLImageRaw>, (size_t)ALPackSlot::COUNT>;

struct ALPackOutput
{
    std::string           mName;
    ALPackDest            mDest = 0;
    LLPointer<LLImageRaw> mImage;
};

using ALPackOutputSet = std::vector<ALPackOutput>;

namespace ALPBRPacker
{
    // The value a channel must take when its source map is absent. This is the
    // identity for however the renderer consumes that channel, not simply
    // black: roughness, metallic, glossiness and environment intensity are all
    // multiplied by a slider, so 255 leaves that slider in full control;
    // occlusion of 255 means unoccluded; a missing normal is a flat
    // (128,128,255) tangent-space normal.
    U8 neutralValue(ALPackSlot slot, ALPackChannel channel);

    const char* slotName(ALPackSlot slot);

    // Read one channel out of an interleaved pixel, tolerating source images
    // with fewer components than the channel asks for. Exposed so the UI can
    // preview exactly what the pack will read.
    U8 sampleChannel(const U8* pixel, S8 components, ALPackChannel channel);

    // Decode an image file into an LLImageRaw, normalising component count so
    // downstream scaling works (LLImageRaw::scale rejects 2-component images).
    LLPointer<LLImageRaw> loadRaw(const std::string& path, std::string& err);

    // Run the recipe. Targets whose every channel resolves to a neutral are
    // omitted from the output entirely -- glTF treats an absent texture as the
    // identity, so the render is unchanged and an upload is saved.
    bool pack(const ALPackInputSet& inputs,
              const ALPBRPackRecipe& recipe,
              S32 max_dim,
              ALPackOutputSet& outputs,
              std::string& err);
}
