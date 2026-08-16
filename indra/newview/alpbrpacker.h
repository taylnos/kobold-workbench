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

// The maps a creator feeds in. These are authoring-side concepts and do not
// map one-to-one onto the four textures Second Life actually stores -- that
// translation is the whole point of a recipe.
enum class ALPackSlot : U8
{
    BASE_COLOR = 0,
    EMISSIVE,
    OCCLUSION,
    ROUGHNESS,
    METALLIC,
    NORMAL,
    OPACITY,
    COUNT
};

// Which channel of a source image feeds one output channel. LUMINANCE is for
// the case where a creator hands us a colour image for a scalar map.
enum class ALPackChannel : U8
{
    RED = 0,
    GREEN,
    BLUE,
    ALPHA,
    LUMINANCE
};

// Binding for a single output channel: either a channel of an ingest slot, or
// a flat constant. An unbound slot resolves to its neutral constant during
// recipe resolution, so the pack loop never has to special-case a missing map.
struct ALPackChannelSource
{
    ALPackSlot    mSlot     = ALPackSlot::COUNT;    // COUNT means "use mConstant"
    ALPackChannel mChannel  = ALPackChannel::RED;
    bool          mInvert   = false;
    U8            mConstant = 255;

    bool isConstant() const { return mSlot == ALPackSlot::COUNT; }

    static ALPackChannelSource constant(U8 value);
    static ALPackChannelSource from(ALPackSlot slot, ALPackChannel channel, bool invert = false);
};

// One packed image to produce.
struct ALPackTarget
{
    std::string                        mName;
    LLGLTFMaterial::TextureInfo        mDest = LLGLTFMaterial::GLTF_TEXTURE_INFO_BASE_COLOR;
    S8                                 mComponents = 3;
    std::array<ALPackChannelSource, 4> mChannels;
    bool                               mEnabled = true;
};

// The full packing description. Kept as a plain list so that adding a texture
// slot Second Life does not have today (occlusion as its own map, clearcoat,
// sheen, anisotropy...) is a table entry rather than an engine change.
class ALPBRPackRecipe
{
public:
    // Base Color / Normal / ORM / Emissive -- what SL stores today.
    static ALPBRPackRecipe secondLifeDefault();

    std::vector<ALPackTarget> mTargets;
};

using ALPackInputSet = std::array<LLPointer<LLImageRaw>, (size_t)ALPackSlot::COUNT>;

struct ALPackOutput
{
    std::string                 mName;
    LLGLTFMaterial::TextureInfo mDest = LLGLTFMaterial::GLTF_TEXTURE_INFO_BASE_COLOR;
    LLPointer<LLImageRaw>       mImage;
};

using ALPackOutputSet = std::vector<ALPackOutput>;

namespace ALPBRPacker
{
    // The value a channel must take when its source map is absent. This is the
    // identity for however glTF consumes that channel, not simply black:
    // roughness and metallic are multiplied by their factors, so 255 leaves the
    // factor in full control; occlusion of 255 means unoccluded; a missing
    // normal is a flat (128,128,255) tangent-space normal.
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
