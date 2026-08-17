/**
 * @file alpbrpacker_test.cpp
 * @brief Unit tests for the glTF PBR channel packing engine
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

#include "linden_common.h"

#include <set>

#include "../test/lltut.h"

#include "../alpbrpacker.h"

namespace
{
    // A flat single-colour source map.
    LLPointer<LLImageRaw> make_image(S32 width, S32 height, S8 components,
                                     U8 r, U8 g = 0, U8 b = 0, U8 a = 255)
    {
        LLPointer<LLImageRaw> image = new LLImageRaw((U16)width, (U16)height, components);
        U8* data = image->getData();
        const U32 pixel_count = (U32)width * (U32)height;
        const U8 channels[4] = { r, g, b, a };

        for (U32 i = 0; i < pixel_count; ++i)
        {
            for (S8 c = 0; c < components; ++c)
            {
                data[i * components + c] = channels[c];
            }
        }
        return image;
    }

    U8 pixel_at(const LLPointer<LLImageRaw>& image, U32 index, S8 channel)
    {
        return image->getData()[(size_t)index * image->getComponents() + channel];
    }

    const ALPackOutput* find_output(const ALPackOutputSet& outputs, ALPackDest dest)
    {
        for (const ALPackOutput& output : outputs)
        {
            if (output.mDest == dest)
            {
                return &output;
            }
        }
        return nullptr;
    }
}

namespace tut
{
    struct pbrpacker_data
    {
        ALPackInputSet   mInputs;
        ALPBRPackRecipe  mRecipe;
        ALPackOutputSet  mOutputs;
        std::string      mError;

        pbrpacker_data()
            : mRecipe(ALPBRPackRecipe::secondLifeDefault())
        {
        }

        bool pack()
        {
            return ALPBRPacker::pack(mInputs, mRecipe, 2048, mOutputs, mError);
        }
    };

    typedef test_group<pbrpacker_data> pbrpacker_t;
    typedef pbrpacker_t::object pbrpacker_object;
    tut::pbrpacker_t tut_pbrpacker("LLPBRPacker");

    // Roughness alone must still produce an ORM, with occlusion and metalness
    // filled white so their factors keep full range. A black fill would pin
    // metalness to zero and darken everything with phantom occlusion.
    template<> template<>
    void pbrpacker_object::test<1>()
    {
        mInputs[(size_t)ALPackSlot::ROUGHNESS] = make_image(64, 64, 3, 90, 90, 90);

        ensure("pack succeeds", pack());

        const ALPackOutput* orm = find_output(mOutputs, LLGLTFMaterial::GLTF_TEXTURE_INFO_METALLIC_ROUGHNESS);
        ensure("ORM produced", orm != nullptr);
        ensure_equals("occlusion neutral is white", (S32)pixel_at(orm->mImage, 0, 0), 255);
        ensure_equals("roughness carried through", (S32)pixel_at(orm->mImage, 0, 1), 90);
        ensure_equals("metalness neutral is white", (S32)pixel_at(orm->mImage, 0, 2), 255);
    }

    // Metalness alone: same rule, mirrored.
    template<> template<>
    void pbrpacker_object::test<2>()
    {
        mInputs[(size_t)ALPackSlot::METALLIC] = make_image(32, 32, 3, 200, 200, 200);

        ensure("pack succeeds", pack());

        const ALPackOutput* orm = find_output(mOutputs, LLGLTFMaterial::GLTF_TEXTURE_INFO_METALLIC_ROUGHNESS);
        ensure("ORM produced", orm != nullptr);
        ensure_equals("occlusion neutral is white", (S32)pixel_at(orm->mImage, 0, 0), 255);
        ensure_equals("roughness neutral is white", (S32)pixel_at(orm->mImage, 0, 1), 255);
        ensure_equals("metalness carried through", (S32)pixel_at(orm->mImage, 0, 2), 200);
    }

    // All three supplied separately land in the right channels.
    template<> template<>
    void pbrpacker_object::test<3>()
    {
        mInputs[(size_t)ALPackSlot::OCCLUSION] = make_image(16, 16, 3, 10, 10, 10);
        mInputs[(size_t)ALPackSlot::ROUGHNESS] = make_image(16, 16, 3, 20, 20, 20);
        mInputs[(size_t)ALPackSlot::METALLIC]  = make_image(16, 16, 3, 30, 30, 30);

        ensure("pack succeeds", pack());

        const ALPackOutput* orm = find_output(mOutputs, LLGLTFMaterial::GLTF_TEXTURE_INFO_METALLIC_ROUGHNESS);
        ensure("ORM produced", orm != nullptr);
        ensure_equals("occlusion in red", (S32)pixel_at(orm->mImage, 0, 0), 10);
        ensure_equals("roughness in green", (S32)pixel_at(orm->mImage, 0, 1), 20);
        ensure_equals("metalness in blue", (S32)pixel_at(orm->mImage, 0, 2), 30);
    }

    // A gloss source is roughness upside down.
    template<> template<>
    void pbrpacker_object::test<4>()
    {
        mInputs[(size_t)ALPackSlot::ROUGHNESS] = make_image(8, 8, 3, 200, 200, 200);

        for (ALPackTarget& target : mRecipe.mTargets)
        {
            if (target.mDest == LLGLTFMaterial::GLTF_TEXTURE_INFO_METALLIC_ROUGHNESS)
            {
                target.mChannels[1].mInvert = true;
            }
        }

        ensure("pack succeeds", pack());

        const ALPackOutput* orm = find_output(mOutputs, LLGLTFMaterial::GLTF_TEXTURE_INFO_METALLIC_ROUGHNESS);
        ensure("ORM produced", orm != nullptr);
        ensure_equals("gloss inverted to roughness", (S32)pixel_at(orm->mImage, 0, 1), 55);
        // The neutral fills must not be inverted along with it.
        ensure_equals("occlusion still white", (S32)pixel_at(orm->mImage, 0, 0), 255);
        ensure_equals("metalness still white", (S32)pixel_at(orm->mImage, 0, 2), 255);
    }

    // Targets with no real source at all are dropped, not emitted flat.
    template<> template<>
    void pbrpacker_object::test<5>()
    {
        mInputs[(size_t)ALPackSlot::BASE_COLOR] = make_image(16, 16, 3, 128, 64, 32);

        ensure("pack succeeds", pack());
        ensure_equals("only base colour emitted", (S32)mOutputs.size(), 1);

        const ALPackOutput* base = find_output(mOutputs, LLGLTFMaterial::GLTF_TEXTURE_INFO_BASE_COLOR);
        ensure("base colour produced", base != nullptr);
        ensure("no ORM", find_output(mOutputs, LLGLTFMaterial::GLTF_TEXTURE_INFO_METALLIC_ROUGHNESS) == nullptr);
        ensure("no normal", find_output(mOutputs, LLGLTFMaterial::GLTF_TEXTURE_INFO_NORMAL) == nullptr);
        ensure("no emissive", find_output(mOutputs, LLGLTFMaterial::GLTF_TEXTURE_INFO_EMISSIVE) == nullptr);
    }

    // A fully opaque alpha is dropped rather than uploaded as constant 255.
    template<> template<>
    void pbrpacker_object::test<6>()
    {
        mInputs[(size_t)ALPackSlot::BASE_COLOR] = make_image(16, 16, 4, 10, 20, 30, 255);

        ensure("pack succeeds", pack());

        const ALPackOutput* base = find_output(mOutputs, LLGLTFMaterial::GLTF_TEXTURE_INFO_BASE_COLOR);
        ensure("base colour produced", base != nullptr);
        ensure_equals("opaque alpha optimised away", (S32)base->mImage->getComponents(), 3);
        ensure_equals("red kept", (S32)pixel_at(base->mImage, 0, 0), 10);
        ensure_equals("green kept", (S32)pixel_at(base->mImage, 0, 1), 20);
        ensure_equals("blue kept", (S32)pixel_at(base->mImage, 0, 2), 30);
    }

    // A meaningful alpha survives.
    template<> template<>
    void pbrpacker_object::test<7>()
    {
        mInputs[(size_t)ALPackSlot::BASE_COLOR] = make_image(16, 16, 4, 10, 20, 30, 128);

        ensure("pack succeeds", pack());

        const ALPackOutput* base = find_output(mOutputs, LLGLTFMaterial::GLTF_TEXTURE_INFO_BASE_COLOR);
        ensure("base colour produced", base != nullptr);
        ensure_equals("alpha kept", (S32)base->mImage->getComponents(), 4);
        ensure_equals("alpha value kept", (S32)pixel_at(base->mImage, 0, 3), 128);
    }

    // Mismatched source sizes conform to the largest contributor rather than
    // to whichever map happened to arrive first.
    template<> template<>
    void pbrpacker_object::test<8>()
    {
        mInputs[(size_t)ALPackSlot::OCCLUSION] = make_image(32, 32, 3, 10, 10, 10);
        mInputs[(size_t)ALPackSlot::ROUGHNESS] = make_image(128, 128, 3, 20, 20, 20);

        ensure("pack succeeds", pack());

        const ALPackOutput* orm = find_output(mOutputs, LLGLTFMaterial::GLTF_TEXTURE_INFO_METALLIC_ROUGHNESS);
        ensure("ORM produced", orm != nullptr);
        ensure_equals("width follows largest input", (S32)orm->mImage->getWidth(), 128);
        ensure_equals("height follows largest input", (S32)orm->mImage->getHeight(), 128);
    }

    // Nothing supplied is an error, not an empty success.
    template<> template<>
    void pbrpacker_object::test<9>()
    {
        ensure("pack fails with no input", !pack());
        ensure("error reported", !mError.empty());
    }

    // A missing normal map's neutral is a flat tangent-space normal, which is
    // the one slot where white would be wrong.
    template<> template<>
    void pbrpacker_object::test<10>()
    {
        ensure_equals("normal red neutral",
                      (S32)ALPBRPacker::neutralValue(ALPackSlot::NORMAL, ALPackChannel::RED), 128);
        ensure_equals("normal green neutral",
                      (S32)ALPBRPacker::neutralValue(ALPackSlot::NORMAL, ALPackChannel::GREEN), 128);
        ensure_equals("normal blue neutral",
                      (S32)ALPBRPacker::neutralValue(ALPackSlot::NORMAL, ALPackChannel::BLUE), 255);
        ensure_equals("roughness neutral is white",
                      (S32)ALPBRPacker::neutralValue(ALPackSlot::ROUGHNESS, ALPackChannel::RED), 255);
        ensure_equals("metallic neutral is white",
                      (S32)ALPBRPacker::neutralValue(ALPackSlot::METALLIC, ALPackChannel::RED), 255);
        ensure_equals("occlusion neutral is white",
                      (S32)ALPBRPacker::neutralValue(ALPackSlot::OCCLUSION, ALPackChannel::RED), 255);
    }

    // One packed ORM file feeding three slots by channel -- the Substance
    // export case the old import path could not express.
    template<> template<>
    void pbrpacker_object::test<11>()
    {
        LLPointer<LLImageRaw> packed = make_image(16, 16, 3, 11, 22, 33);
        mInputs[(size_t)ALPackSlot::OCCLUSION] = packed;
        mInputs[(size_t)ALPackSlot::ROUGHNESS] = packed;
        mInputs[(size_t)ALPackSlot::METALLIC]  = packed;

        for (ALPackTarget& target : mRecipe.mTargets)
        {
            if (target.mDest == LLGLTFMaterial::GLTF_TEXTURE_INFO_METALLIC_ROUGHNESS)
            {
                target.mChannels[0] = ALPackChannelSource::from(ALPackSlot::OCCLUSION, ALPackChannel::RED);
                target.mChannels[1] = ALPackChannelSource::from(ALPackSlot::ROUGHNESS, ALPackChannel::GREEN);
                target.mChannels[2] = ALPackChannelSource::from(ALPackSlot::METALLIC, ALPackChannel::BLUE);
            }
        }

        ensure("pack succeeds", pack());

        const ALPackOutput* orm = find_output(mOutputs, LLGLTFMaterial::GLTF_TEXTURE_INFO_METALLIC_ROUGHNESS);
        ensure("ORM produced", orm != nullptr);
        ensure_equals("red from red", (S32)pixel_at(orm->mImage, 0, 0), 11);
        ensure_equals("green from green", (S32)pixel_at(orm->mImage, 0, 1), 22);
        ensure_equals("blue from blue", (S32)pixel_at(orm->mImage, 0, 2), 33);
    }

    // A single-channel grayscale source is legal for the scalar maps.
    template<> template<>
    void pbrpacker_object::test<12>()
    {
        mInputs[(size_t)ALPackSlot::ROUGHNESS] = make_image(16, 16, 1, 77);

        ensure("pack succeeds", pack());

        const ALPackOutput* orm = find_output(mOutputs, LLGLTFMaterial::GLTF_TEXTURE_INFO_METALLIC_ROUGHNESS);
        ensure("ORM produced", orm != nullptr);
        ensure_equals("grayscale read into green", (S32)pixel_at(orm->mImage, 0, 1), 77);
    }

    // ---- SpecGloss ----------------------------------------------------------

    // Glossiness rides in the normal map's alpha, so supplying it alone still
    // has to produce a normal upload: a flat tangent-space normal carrying the
    // gloss. There is nowhere else in the legacy material for it to live.
    template<> template<>
    void pbrpacker_object::test<13>()
    {
        mRecipe = ALPBRPackRecipe::secondLifeSpecGloss();
        mInputs[(size_t)ALPackSlot::GLOSSINESS] = make_image(32, 32, 1, 180);

        ensure("pack succeeds", pack());
        ensure_equals("only the normal map is produced", (S32)mOutputs.size(), 1);

        const ALPackOutput* normal = find_output(mOutputs, AL_SPECGLOSS_NORMAL);
        ensure("normal produced", normal != nullptr);
        ensure_equals("flat normal red", (S32)pixel_at(normal->mImage, 0, 0), 128);
        ensure_equals("flat normal green", (S32)pixel_at(normal->mImage, 0, 1), 128);
        ensure_equals("flat normal blue", (S32)pixel_at(normal->mImage, 0, 2), 255);
        ensure_equals("gloss in alpha", (S32)pixel_at(normal->mImage, 0, 3), 180);
    }

    // Normal and glossiness together: the pairing the mode exists for.
    template<> template<>
    void pbrpacker_object::test<14>()
    {
        mRecipe = ALPBRPackRecipe::secondLifeSpecGloss();
        mInputs[(size_t)ALPackSlot::NORMAL]     = make_image(64, 64, 3, 120, 130, 240);
        mInputs[(size_t)ALPackSlot::GLOSSINESS] = make_image(64, 64, 1, 64);

        ensure("pack succeeds", pack());

        const ALPackOutput* normal = find_output(mOutputs, AL_SPECGLOSS_NORMAL);
        ensure("normal produced", normal != nullptr);
        ensure_equals("normal keeps four channels", (S32)normal->mImage->getComponents(), 4);
        ensure_equals("normal red", (S32)pixel_at(normal->mImage, 0, 0), 120);
        ensure_equals("normal green", (S32)pixel_at(normal->mImage, 0, 1), 130);
        ensure_equals("normal blue", (S32)pixel_at(normal->mImage, 0, 2), 240);
        ensure_equals("gloss in alpha", (S32)pixel_at(normal->mImage, 0, 3), 64);
    }

    // Without a glossiness map the neutral fill is a fully opaque alpha, which
    // is dropped -- the Glossiness slider is then the only control, exactly as
    // for a normal map uploaded without one.
    template<> template<>
    void pbrpacker_object::test<15>()
    {
        mRecipe = ALPBRPackRecipe::secondLifeSpecGloss();
        mInputs[(size_t)ALPackSlot::NORMAL] = make_image(32, 32, 3, 128, 128, 255);

        ensure("pack succeeds", pack());

        const ALPackOutput* normal = find_output(mOutputs, AL_SPECGLOSS_NORMAL);
        ensure("normal produced", normal != nullptr);
        ensure_equals("opaque alpha dropped", (S32)normal->mImage->getComponents(), 3);
    }

    // Specular tint and environment intensity pack into one upload the same
    // way, environment in alpha.
    template<> template<>
    void pbrpacker_object::test<16>()
    {
        mRecipe = ALPBRPackRecipe::secondLifeSpecGloss();
        mInputs[(size_t)ALPackSlot::SPECULAR_COLOR] = make_image(32, 32, 3, 200, 150, 100);
        mInputs[(size_t)ALPackSlot::SPECULAR_ENV]   = make_image(32, 32, 1, 40);

        ensure("pack succeeds", pack());

        const ALPackOutput* specular = find_output(mOutputs, AL_SPECGLOSS_SPECULAR);
        ensure("specular produced", specular != nullptr);
        ensure_equals("tint red", (S32)pixel_at(specular->mImage, 0, 0), 200);
        ensure_equals("tint green", (S32)pixel_at(specular->mImage, 0, 1), 150);
        ensure_equals("tint blue", (S32)pixel_at(specular->mImage, 0, 2), 100);
        ensure_equals("environment in alpha", (S32)pixel_at(specular->mImage, 0, 3), 40);
    }

    // Environment alone gets a white tint to ride on, so the Specular Color
    // swatch stays in control of the highlight.
    template<> template<>
    void pbrpacker_object::test<17>()
    {
        mRecipe = ALPBRPackRecipe::secondLifeSpecGloss();
        mInputs[(size_t)ALPackSlot::SPECULAR_ENV] = make_image(16, 16, 1, 90);

        ensure("pack succeeds", pack());
        ensure_equals("only the specular map is produced", (S32)mOutputs.size(), 1);

        const ALPackOutput* specular = find_output(mOutputs, AL_SPECGLOSS_SPECULAR);
        ensure("specular produced", specular != nullptr);
        ensure_equals("tint neutral is white", (S32)pixel_at(specular->mImage, 0, 0), 255);
        ensure_equals("environment in alpha", (S32)pixel_at(specular->mImage, 0, 3), 90);
    }

    // The diffuse map is independent of the other two, and a diffuse with no
    // alpha payload sheds its alpha channel like any other opaque colour map.
    template<> template<>
    void pbrpacker_object::test<18>()
    {
        mRecipe = ALPBRPackRecipe::secondLifeSpecGloss();
        mInputs[(size_t)ALPackSlot::BASE_COLOR] = make_image(64, 64, 3, 10, 20, 30);

        ensure("pack succeeds", pack());
        ensure_equals("only the diffuse map is produced", (S32)mOutputs.size(), 1);

        const ALPackOutput* diffuse = find_output(mOutputs, AL_SPECGLOSS_DIFFUSE);
        ensure("diffuse produced", diffuse != nullptr);
        ensure_equals("opaque alpha dropped", (S32)diffuse->mImage->getComponents(), 3);
        ensure_equals("diffuse red", (S32)pixel_at(diffuse->mImage, 0, 0), 10);
    }

    // An emissive mask bound into the diffuse alpha, which is what the floater
    // does when that slot is filled. Nothing in the engine special-cases it --
    // it is the same alpha rebinding opacity uses.
    template<> template<>
    void pbrpacker_object::test<19>()
    {
        mRecipe = ALPBRPackRecipe::secondLifeSpecGloss();
        mInputs[(size_t)ALPackSlot::BASE_COLOR] = make_image(32, 32, 3, 60, 70, 80);
        mInputs[(size_t)ALPackSlot::EMISSIVE]   = make_image(32, 32, 1, 200);

        for (ALPackTarget& target : mRecipe.mTargets)
        {
            if (target.mDest == AL_SPECGLOSS_DIFFUSE)
            {
                target.mChannels[3] = ALPackChannelSource::from(ALPackSlot::EMISSIVE, ALPackChannel::RED);
            }
        }

        ensure("pack succeeds", pack());

        const ALPackOutput* diffuse = find_output(mOutputs, AL_SPECGLOSS_DIFFUSE);
        ensure("diffuse produced", diffuse != nullptr);
        ensure_equals("mask kept in alpha", (S32)diffuse->mImage->getComponents(), 4);
        ensure_equals("diffuse red", (S32)pixel_at(diffuse->mImage, 0, 0), 60);
        ensure_equals("emissive mask in alpha", (S32)pixel_at(diffuse->mImage, 0, 3), 200);
    }

    // The new slots multiply against a slider, so their neutral is white for
    // the same reason roughness and metalness are.
    template<> template<>
    void pbrpacker_object::test<20>()
    {
        ensure_equals("glossiness neutral is white",
                      (S32)ALPBRPacker::neutralValue(ALPackSlot::GLOSSINESS, ALPackChannel::RED), 255);
        ensure_equals("specular colour neutral is white",
                      (S32)ALPBRPacker::neutralValue(ALPackSlot::SPECULAR_COLOR, ALPackChannel::RED), 255);
        ensure_equals("specular environment neutral is white",
                      (S32)ALPBRPacker::neutralValue(ALPackSlot::SPECULAR_ENV, ALPackChannel::RED), 255);
    }

    // The two modes must not collide: a SpecGloss destination is only ever read
    // against a SpecGloss recipe, so the two enums may overlap numerically, but
    // each recipe has to keep its own destinations distinct.
    template<> template<>
    void pbrpacker_object::test<21>()
    {
        ALPBRPackRecipe spec_gloss = ALPBRPackRecipe::secondLifeSpecGloss();
        ensure_equals("three SpecGloss targets", (S32)spec_gloss.mTargets.size(), 3);

        std::set<ALPackDest> seen;
        for (const ALPackTarget& target : spec_gloss.mTargets)
        {
            ensure("SpecGloss destinations are distinct", seen.insert(target.mDest).second);
            ensure("every SpecGloss upload carries alpha", target.mComponents == 4);
        }

        ALPBRPackRecipe pbr = ALPBRPackRecipe::forMode(ALPackMode::GLTF_PBR);
        ensure_equals("four glTF targets", (S32)pbr.mTargets.size(), 4);
    }
}
