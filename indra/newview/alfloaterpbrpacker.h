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
#include "lluiimage.h"
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
    // One card per ingest slot, bound to a generic XUI card when the material
    // type changes -- the two modes ingest different maps but the cards are
    // interchangeable, so there is one set of widgets rather than one per mode.
    // mInvert is a toggle button rather than a checkbox so the state reads as a
    // pressed icon and carries no label to translate. mChannel and mInvert are
    // left null where they mean nothing for the slot, and their widgets hidden.
    // mThumb is a transparent button rather than a panel: the preview is the
    // slot's hit area, since clicking the image is what people reach for
    // before hunting for a browse button.
    struct SlotUI
    {
        LLView*     mCard    = nullptr;
        LLTextBox*  mLabel   = nullptr;
        LLButton*   mClear   = nullptr;
        LLComboBox* mChannel = nullptr;
        LLButton*   mInvert  = nullptr;
        LLButton*   mThumb   = nullptr;
    };

    // One card per packed texture, indexed by the result's mDest so a result
    // can be filed straight into its slot.
    // mWarn is a transparent button in the preview's inner corner: the preview
    // is painted after the child views, so the caution glyph is drawn by the
    // floater and the widget exists only to own the hit area and the tooltip.
    struct OutputUI
    {
        LLView*    mCard  = nullptr;
        LLTextBox* mLabel = nullptr;
        LLView*    mThumb = nullptr;
        LLTextBox* mSize  = nullptr;
        LLButton*  mWarn  = nullptr;
    };

    // Preset order must match the combo_box items in the XUI.
    enum EPreset
    {
        PRESET_SEPARATE = 0,      // three separate greyscale maps, all read from red
        PRESET_PACKED_ORM,        // one already-packed ORM/ARM mask, read R/G/B
        PRESET_SEPARATE_GLOSS,    // separate maps, roughness supplied as gloss
    };

    enum ESpecGlossPreset
    {
        SG_PRESET_SEPARATE = 0,   // glossiness and environment each read from red
        SG_PRESET_FROM_ROUGHNESS, // glossiness supplied as a roughness map
    };

    static constexpr size_t OUTPUT_COUNT = AL_PACK_MAX_OUTPUTS;

    void onBrowse(ALPackSlot slot);
    void onFilePicked(ALPackSlot slot, const std::vector<std::string>& filenames);
    void onClear(ALPackSlot slot);

    // The card callbacks are bound once and resolve their slot through
    // mCardSlot at click time. They cannot capture the slot, because
    // LLUICtrl::setCommitCallback and LLButton::setClickedCallback *connect* to
    // a signal rather than replace it -- rebinding per mode change stacked a
    // handler each time, so one click opened a file picker per flip.
    void onCardBrowse(size_t card);
    void onCardClear(size_t card);
    void onPack();
    void onSave();
    void onSaveLocationPicked(const std::vector<std::string>& filenames);
    void onSettingChanged();
    void onMakeLocalMaterial();
    void onSendToMaterialEditor();
    void onApplyToSelection();
    void onAddFiles();
    void onFilesPicked(const std::vector<std::string>& filenames);
    void onPresetChanged();
    void applyPreset(S32 preset);
    void onModeChanged();

    // Point the generic cards at the slots and packed textures this mode uses,
    // label them, and hide the controls the mode has no use for. Everything
    // loaded so far is dropped: the slots mean different things now.
    void applyMode(ALPackMode mode);

    LLPointer<LLImageRaw> outputFor(ALPackDest dest) const;
    std::string           suggestedMaterialName() const;

    // Which source feeds the diffuse map's alpha. Second Life stores one
    // diffuse alpha mode per face, so opacity and an emissive mask cannot both
    // be honoured -- an explicit emissive mask wins, and the pack warns when
    // both are supplied.
    ALPackSlot diffuseAlphaSlot() const;

    void            refreshControls();
    ALPBRPackRecipe buildRecipe() const;
    bool            hasAnyInput() const;

    // Raise a system toast. The floater has no status line: advisory warnings
    // hang off the packed texture they concern, and everything else that is
    // worth saying is worth saying where the user is looking.
    static void notifyUser(const std::string& message);

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

    // Register each packed map with the local texture system, reusing the unit
    // already registered for a path rather than adding a second one. Legacy
    // materials have no asset to hold the set together, so the maps travel as
    // loose textures and this is how they get a world id to assign.
    bool registerLocalTextures(std::string& error_out);
    bool hasLocalTextures() const;

    // Whether a packed map kept its alpha channel. Drives the applied alpha
    // mode, where a dropped alpha honestly means "opaque".
    bool outputHasAlpha(ALPackDest dest) const;

    // Take a fresh stem for the packed files. Called when the material type
    // changes so a re-pack cannot overwrite files a local asset still reads.
    void newTempStem();

    // Rect of a widget in this floater's own drawing coordinates. The slot
    // widgets live inside card panels, so their getRect() is card-relative.
    LLRect localRectOf(const LLView* view) const;

    bool slotInverted(ALPackSlot slot) const;

    // Kick off an off-thread decode of one source file into its slot.
    // auto_repack re-packs once every outstanding load has landed.
    void loadSlot(ALPackSlot slot, const std::string& path, bool auto_repack);

    // The same for a file that feeds several slots at once, such as a packed
    // ORM mask read three ways: one decode and one thumbnail rescale, shared.
    void loadSlots(const std::vector<ALPackSlot>& slots, const std::string& path, bool auto_repack);
    void noteSourceTime(ALPackSlot slot);

    static ALPackChannel channelFromIndex(S32 index);

    ALPackMode              mMode = ALPackMode::GLTF_PBR;
    // Bumped by applyMode(). Worker replies carry the value they were queued
    // under and discard themselves if it has moved on, because the slots they
    // were headed for may no longer exist and the file stem they wrote under
    // has been replaced. A counter rather than comparing mMode, so a round trip
    // back to the same mode still invalidates.
    U32                     mModeGeneration = 0;
    std::vector<ALPackSlot> mActiveSlots;    // ingest slots this mode uses
    std::vector<ALPackDest> mActiveOutputs;  // packed textures this mode produces

    // The physical cards, looked up and wired once in postBuild. A mode change
    // re-points mSlotUI and mOutputUI at these rather than touching the widgets
    // or their callbacks again.
    std::vector<SlotUI>     mCards;
    std::vector<OutputUI>   mOutputCards;
    // Which ingest slot each card carries right now, ALPackSlot::COUNT for a
    // card this mode leaves unused. This is what the once-bound card callbacks
    // dispatch through.
    std::vector<ALPackSlot> mCardSlot;

    std::array<SlotUI, (size_t)ALPackSlot::COUNT>     mSlotUI;
    std::array<std::string, (size_t)ALPackSlot::COUNT> mSlotPaths;
    // The per-slot "click to choose ..." hint, kept so an empty slot can show
    // it again after a loaded slot replaced it with the path.
    std::array<std::string, (size_t)ALPackSlot::COUNT> mSlotEmptyTip;
    // The slot's title, used for status lines as well as the card, so a message
    // says "Diffuse" in SpecGloss mode where it says "Base Color" in the other.
    std::array<std::string, (size_t)ALPackSlot::COUNT> mSlotLabel;
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
    // Advisory text for each packed texture, joined per destination. Empty
    // means the card shows no caution glyph.
    std::array<std::string, OUTPUT_COUNT>                mOutputWarnings;
    LLPointer<LLUIImage>                                 mWarnIcon;

    ALPackOutputSet          mOutputs;
    std::vector<std::string> mOutputPaths;

    // Stem shared by every packed file this session writes, so a re-pack
    // overwrites in place and the local material or local textures pick up the
    // change.
    std::string mTempStem;
    std::string mGltfPath;
    LLUUID      mLocalMaterialId;   // tracking id of the local material, if made

    // Tracking ids of the local textures backing each packed map, indexed by
    // mDest. SpecGloss only -- the glTF path hands its files to the local
    // material system, which owns its own textures.
    std::array<LLUUID, OUTPUT_COUNT> mLocalTextureIds;

    LLButton*   mPackBtn     = nullptr;
    LLButton*   mSaveBtn     = nullptr;
    LLButton*   mMakeLocalBtn = nullptr;
    LLButton*   mSendToEditorBtn = nullptr;
    LLButton*   mApplyToSelectionBtn = nullptr;
    LLButton*   mAddFilesBtn = nullptr;
    LLComboBox* mModeCombo   = nullptr;
    LLComboBox* mPresetCombo = nullptr;      // glTF PBR presets
    LLComboBox* mPresetSGCombo = nullptr;    // SpecGloss presets
    LLComboBox* mMaxSizeCombo = nullptr;
    LLCheckBoxCtrl* mAutoRepackCheck = nullptr;

    S32  mPendingLoads = 0;
    bool mRepackWhenLoaded = false;
    bool mPacking = false;
    bool mOutputThumbsDirty = false;
};
