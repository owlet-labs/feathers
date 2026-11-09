/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program.
 * If not, see <https://www.gnu.org/licenses/>.
 *
 * --- Additional terms under GNU GPL version 3 section 7 ---
 * This file requires preservation of the above copyright notice and author attribution
 * in all copies or substantial portions of this file.
 */

#pragma once

#include "plugin/ui/device_preset_provider.h"
#include "plugin/ui/featherverb_meter.h"
#include "plugin/ui/file_dialog.h"
#include "plugin/ui/generic_expanded.h"
#include "fx/conv/ir_browse.h"
#include "plugin/ui/ir_view.h"
#include "plugin/ui/item_stepper.h"
#include "plugin/ui/preset_browser.h"

#include <memory>

namespace feathers::fx::featherverb {
class Featherverb;
}

namespace feathers::ui {

class Knob;
class OwnedControl;
class SwitchChip;

//--------------------------------------------------------------------------------------------------//
// FEATHERVERB'S EXPANDED WINDOW: THE GRID, AND THE CONV DRAWER BELOW IT (backlog JD).                //
//--------------------------------------------------------------------------------------------------//
//
// The convolution controls live in a drawer that comes out of the bottom, and only in the expanded
// window, not the card. So the drawer is this panel's, under its grid:
//
// - **A STRIP** that is always there — a chevron, CONVOLUTION, and the file (or "Generated room", or why
//   the last file would not load). A press folds the drawer or opens it.
// - **OPEN**: the impulse (`IrView`: the waveform, the markers, the MSEG envelope, drag-in), then one row of
//   the conv controls — Conv Mode with Blend inside it, Conv Predelay, Conv Feedback, and Stretch.
// - **THE CONV CONTROLS LEAVE THE GRID** (`drawnBySubclass`) and are SEATED here (`seatCell`): the grid's own
//   cells, not second copies over the same fields. Stretch is not a parameter (it rebuilds the IR) and is a
//   knob of this panel's over the device's shape, committed when the hand lets go.
// - **HONEST WHILE CONV IS OFF**: Predelay, Feedback and Stretch change nothing then, so they are greyed.
//
// The device is reached through `DeviceHost::containerSurfaceOf`, asked afresh every time: a leaf with a
// body is filed there (`device::BodyOps`), and the instance is replaced whole by a patch load.
class FeatherverbExpanded final : public GenericExpanded {
public:
	FeatherverbExpanded(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id,
	                    const device::Descriptor& desc, Notifier* notifier = nullptr);
	~FeatherverbExpanded() override;

	void draw(visage::Canvas& canvas) override;
	void resized() override;
	[[nodiscard]] visage::Point minimumSize() const override;
	/// **THE WIDTH IS OPEN, THE HEIGHT IS NOT** (backlog EI): the impulse is drawn across the whole width, so a
	/// wider window is a wider picture; the height is the grid's and the drawer's, fixed cells, plus `kPad`.
	[[nodiscard]] visage::Point maximumSize() const override;
	bool updateContent() override;

	[[nodiscard]] bool drawerOpen() const noexcept { return open_; }
	void setDrawerOpen(bool open);

	/// **THE IMPULSE BROWSER** (forward/back arrows):
	/// step to the previous or next IR in the current file's folder (`conv::irFilesBeside`, wrapping, skipping any
	/// that will not load), or list them all from the name. Nothing to browse on the generated room.
	[[nodiscard]] bool canBrowse() const;
	bool stepIr(int step);
	bool showIrList();
	/// The list's rows, the current one marked "X" — what `showIrList` shows.
	[[nodiscard]] std::vector<std::string> irListForTest() const;
	/// A choice as the list prints it: "Generated · Plate", or the file's name.
	static std::string labelOf(const fx::conv::IrChoice& choice);
	/// The strip, for a case pressing its arrows.
	[[nodiscard]] visage::Frame& stripForTest() const;
	/// The browser (`ItemStepper`), for a case pressing its arrows; the arrows' x in ITS coordinates.
	[[nodiscard]] ItemStepper& stepperForTest() const;
	static float prevArrowXForTest();
	static float nextArrowXForTest();

	/// **CHOOSE A FOLDER**: the list ends in "Choose
	/// folder...", which opens a menu that walks the disk a level at a time (`conv::folderRows`) — at the next tick, because
	/// the list's own menu is still closing when it is chosen. "Use this folder" loads the folder's first impulse that
	/// loads, and the arrows then walk that folder.
	static constexpr int kChooseFolderAction = 0;
	/// **THE OS'S OWN DIALOGS**: "Load IR..." picks one
	/// audio file, "Choose folder..." a folder (`ui::FileDialog`, async; the answer arrives on the message thread). Where
	/// no dialog can be shown (`FileDialog::available`), Load IR... is greyed and Choose folder... falls back to the menu.
	static constexpr int kLoadIrAction = 1;
	void loadIrFromDialog();
	void chooseFolderFromDialog();
	[[nodiscard]] FileDialog& fileDialogForTest() { return irDialog_; }

	/// **THE PRESETS: FEATHERS' OWN BROWSER, THIS DEVICE'S BANK** (the reverb reuses the preset browser
	/// rather than growing its own) — the one SKLITTER's panel holds, in the title row. A preset carries the controls AND the body: the
	/// IR by name, its markers, stretch, envelope and Lite (`device_preset.cpp` carries a leaf's body). "Init" is the
	/// declared defaults and the generated Room.
	[[nodiscard]] visage::Frame* titleAccessory() const override { return presets_.get(); }
	[[nodiscard]] PresetBrowser& presetBrowserForTest() const { return *presets_; }
	/// The folder the browser walks: the loaded file's, else the last browsed, else the fixed one.
	[[nodiscard]] std::string browseFolder() const;
	/// Browse `folder`. False, with a note in the strip, when nothing in it will load.
	bool useFolder(const std::string& folder);
	/// The folder a menu will open at on the next tick; empty when none waits.
	[[nodiscard]] const std::string& pendingFolderMenuForTest() const { return pendingFolderMenu_; }

	/// TEST SEAMS.
	[[nodiscard]] IrView& irViewForTest() const { return *view_; }
	[[nodiscard]] Knob& stretchKnobForTest() const { return *stretch_; }
	[[nodiscard]] SwitchChip& liteChipForTest() const { return *lite_; }
	[[nodiscard]] visage::Bounds drawerBoundsForTest() const { return drawerBounds(); }
	/// The words the strip shows.
	[[nodiscard]] std::string stripTextForTest() const { return stripText(); }
	/// THE METER BESIDE INPUT (backlog LA, `featherverb_meter.h`): where it sits, and what it draws.
	[[nodiscard]] visage::Bounds meterBoundsForTest() const { return meterBounds(); }
	[[nodiscard]] featherverb_meter::Fractions meterForTest() const { return featherverb_meter::read(ownHost(), deviceId()); }

	/// The drawer's parts, in logical pixels.
	static constexpr float kStripHeight = 24.0f;
	static constexpr float kViewHeight = 112.0f;

protected:
	[[nodiscard]] bool drawnBySubclass(size_t index) const override;
	void drawBodyExtras(visage::Canvas& canvas) override;

private:
	class Strip;
	class IrSource;
	[[nodiscard]] fx::featherverb::Featherverb* verb() const;
	/// **THE BODY MOVED** — an IR loaded, chosen or reshaped here, through the surface, which no host verb
	/// carries; so the host is told (`DeviceHost::bodyEdited`, backlog JV). After an edit the device took.
	void bodyEdited() { ownHost().bodyEdited(deviceId()); }
	/// What the browser walks now, where it is in it, and a step onto one entry (false if it would not load).
	[[nodiscard]] std::vector<fx::conv::IrChoice> choices() const;
	[[nodiscard]] fx::conv::IrChoice currentChoice() const;
	bool apply(const fx::conv::IrChoice& choice);
	[[nodiscard]] float drawerHeight() const;
	[[nodiscard]] visage::Bounds drawerBounds() const;
	[[nodiscard]] std::string stripText() const;
	/// Grey what Conv Off leaves without effect; true if anything changed.
	bool applyEnables();
	[[nodiscard]] visage::Bounds meterBounds() const;
	featherverb_meter::Fractions shownMeter_ {};

	std::unique_ptr<Strip> strip_;
	std::unique_ptr<IrSource> source_;
	std::unique_ptr<ItemStepper> stepper_;
	std::string shownIrKey_;
	std::unique_ptr<IrView> view_;
	std::unique_ptr<Knob> stretch_;
	std::unique_ptr<OwnedControl> liteControl_;
	std::unique_ptr<SwitchChip> lite_;
	bool open_ = true;
	std::string shownStrip_;
	int shownMode_ = -1;
	/// THE LAST FOLDER BROWSED (a file's), kept while a generated room runs — `choices`.
	mutable std::string lastFolder_;
	bool shownLite_ = false;
	/// The folder menu to open at the next `updateContent`, set by the list's action and by a step into a folder.
	std::string pendingFolderMenu_;
	void showFolderMenu(const std::string& folder);
	std::unique_ptr<DevicePresetProvider> presetProvider_;
	std::unique_ptr<PresetBrowser> presets_; // after its provider, so destroyed before it
	/// LAST, so it is destroyed FIRST: an open dialog is cancelled and its callback never reaches a panel half gone.
	FileDialog irDialog_;
};

} // namespace feathers::ui
