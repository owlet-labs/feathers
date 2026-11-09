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

#include "plugin/ui/featherverb_expanded.h"
#include "plugin/ui/menu_anchor.h"

#include "fx/conv/ir_ref.h"
#include "fx/featherverb/featherverb.h"
#include "plugin/ui/knob.h"
#include "plugin/ui/switch_chip.h"
#include "plugin/ui/theme.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "fx/conv/ir_browse.h"

namespace feathers::ui {

namespace fv = fx::featherverb;

namespace {

/// **STRETCH, A KNOB OVER THE DEVICE'S SHAPE RATHER THAN A PARAMETER.** It rebuilds the IR's spectra, so it
/// cannot be modulated and is not automatable; it is saved in the body. The knob reads log2 of the factor
/// (x0.25 .. x4, 1 in the middle), shows the drag live, and hands the device the new shape once, when the
/// hand lets go — a rebuild per pixel of drag would be tens of milliseconds each.
class StretchControl final : public OwnedControl {
public:
	using Surface = std::function<fv::Featherverb*()>;
	/// `edited` is told after a shape the device took (backlog JV: the host cannot see a body move).
	StretchControl(Surface surface, std::function<void()> edited)
	    : surface_(std::move(surface)), edited_(std::move(edited)) {}

	const device::ParamDesc& desc() const override { return kDesc; }
	double value() const override {
		if (dragging_) {
			return pending_;
		}
		const fv::Featherverb* verb = surface_();
		return verb != nullptr ? std::log2(static_cast<double>(verb->irShape().stretch)) : 0.0;
	}
	void setValue(double value) override {
		pending_ = std::clamp(value, kDesc.min, kDesc.max);
		if (!dragging_) {
			apply();
		}
	}
	device::ModDisplay display() const override { return {}; }
	void setAmount(float) override {}
	void beginGesture() override {
		pending_ = value();
		dragging_ = true;
	}
	void endGesture() override {
		dragging_ = false;
		apply();
	}
	std::string text() const override {
		char buf[16];
		std::snprintf(buf, sizeof buf, "x%.2f", std::exp2(value()));
		return buf;
	}

private:
	void apply() {
		fv::Featherverb* verb = surface_();
		if (verb == nullptr) {
			return;
		}
		const fx::conv::IrShape was = verb->irShape();
		fx::conv::IrShape shape = was;
		shape.stretch = std::clamp(static_cast<float>(std::exp2(pending_)), fx::conv::kIrStretchMin, fx::conv::kIrStretchMax);
		if (verb->setIrShape(shape) && !(shape == was)) {
			edited_();
		}
	}

	static constexpr device::ParamDesc kDesc {
	    .name = "Stretch",
	    .min = -2.0,
	    .max = 2.0,
	    .def = 0.0,
	    .stepped = false,
	    .unit = fx::ParamUnit::Number,
	    .names = {},
	    .modulatable = false,
	    .offsetsMayLeaveRange = false,
	    .tooltip = "The impulse made longer and lower together, or shorter and higher — as a tape played slower "
	               "or faster. Rebuilds the impulse, so it follows the hand when it lets go",
	    .shortName = "Stretch"};

	Surface surface_;
	std::function<void()> edited_;
	double pending_ = 0.0;
	bool dragging_ = false;
};

/// **LITE, A SWITCH OVER THE DEVICE'S SHAPE.** The chosen form is "Cheaper tail": the IR past `conv::kLiteSplit` runs
/// at half rate — measured 17% less work on a 6 s IR at 96 kHz, and duller above a quarter of the rate in the
/// late tail. It rebuilds the kernel, so it is saved in the body and applied when clicked.
class LiteControl final : public OwnedControl {
public:
	using Surface = std::function<fv::Featherverb*()>;
	/// `edited` as `StretchControl`'s.
	LiteControl(Surface surface, std::function<void()> edited)
	    : surface_(std::move(surface)), edited_(std::move(edited)) {}

	const device::ParamDesc& desc() const override { return kDesc; }
	double value() const override {
		const fv::Featherverb* verb = surface_();
		return verb != nullptr && verb->irShape().lite ? 1.0 : 0.0;
	}
	void setValue(double value) override {
		fv::Featherverb* verb = surface_();
		if (verb == nullptr) {
			return;
		}
		const fx::conv::IrShape was = verb->irShape();
		fx::conv::IrShape shape = was;
		shape.lite = value >= 0.5;
		if (verb->setIrShape(shape) && !(shape == was)) {
			edited_();
		}
	}
	device::ModDisplay display() const override { return {}; }
	void setAmount(float) override {}
	void beginGesture() override {}
	void endGesture() override {}
	std::string text() const override { return value() >= 0.5 ? "On" : "Off"; }

private:
	static constexpr const char* kNames[] = { "Off", "On" };
	static constexpr device::ParamDesc kDesc {
	    .name = "Lite",
	    .min = 0.0,
	    .max = 1.0,
	    .def = 0.0,
	    .stepped = true,
	    .unit = fx::ParamUnit::Choice,
	    .names = kNames,
	    .modulatable = false,
	    .offsetsMayLeaveRange = false,
	    .tooltip = "The late tail at half the sample rate: cheaper, and a little duller up top past the first "
	               "third of a second",
	    .shortName = "Lite"};

	Surface surface_;
	std::function<void()> edited_;
};

} // namespace

/// **THE IMPULSE BROWSER'S SOURCE** (backlog JH, over presets-2's shared `ItemStepper`): the generated rooms, then a
/// folder's IRs (`FeatherverbExpanded::choices`). CACHED — the stepper reads count, label and current on every
/// paint — and re-read only when the device's IR changes (`refresh`, from `updateContent`). A load that fails
/// returns false, so the stepper passes over it exactly as it passes over one known not to load.
class FeatherverbExpanded::IrSource final : public ItemSource {
public:
	explicit IrSource(FeatherverbExpanded& panel) : panel_(panel) {}
	void refresh() {
		choices_ = panel_.choices();
		current_ = indexOf(panel_.currentChoice());
	}
	[[nodiscard]] size_t itemCount() const override { return choices_.size(); }
	[[nodiscard]] std::string itemLabel(size_t index) const override {
		return index < choices_.size() ? labelOf(choices_[index]) : std::string();
	}
	[[nodiscard]] int currentItem() const override { return current_; }
	bool selectItem(size_t index) override {
		if (index >= choices_.size() || !panel_.apply(choices_[index])) {
			return false;
		}
		current_ = static_cast<int>(index);
		return true;
	}
	[[nodiscard]] bool itemsWrap() const override { return true; }
	/// THE LIST'S VERBS: a file through the OS's dialog, and the folder it walks (the OS's picker, or the menu).
	[[nodiscard]] std::vector<ItemAction> itemActions() const override {
		return { { kLoadIrAction, "Load IR...", FileDialog::available() }, { kChooseFolderAction, "Choose folder...", true } };
	}
	void itemAct(int id) override {
		if (id == kLoadIrAction) {
			panel_.loadIrFromDialog();
		}
		else if (id == kChooseFolderAction) {
			panel_.chooseFolderFromDialog();
		}
	}
	/// A LOADED IR THE LIST CANNOT SHOW — moved or deleted since it loaded; it still sounds, so it is still named.
	[[nodiscard]] std::string itemPlaceholder() const override {
		const fx::conv::IrChoice here = panel_.currentChoice();
		return here.generated < 0 && here.path.empty() ? std::string("-") : labelOf(here);
	}

private:
	[[nodiscard]] int indexOf(const fx::conv::IrChoice& here) const {
		for (size_t i = 0; i < choices_.size(); ++i) {
			const fx::conv::IrChoice& c = choices_[i];
			const bool same = here.generated >= 0 ? c.generated == here.generated
			                                      : c.generated < 0
			                                            && std::filesystem::path(c.path).filename()
			                                                   == std::filesystem::path(here.path).filename();
			if (same) {
				return static_cast<int>(i);
			}
		}
		return -1;
	}

	FeatherverbExpanded& panel_;
	std::vector<fx::conv::IrChoice> choices_;
	int current_ = -1;
};

/// THE STRIP: a chevron and the word, which fold the drawer; the browser (`ItemStepper`, a child, which takes its
/// own presses); and at the far right the Lite chip (a sibling, on top).
class FeatherverbExpanded::Strip final : public visage::Frame {
public:
	static constexpr float kStepperX = 130.0f;
	static constexpr float kArrowW = 16.0f;
	static constexpr float kNameW = 150.0f;

	explicit Strip(FeatherverbExpanded& panel) : panel_(panel) {
		onMouseDown() = [this](const visage::MouseEvent&) { panel_.setDrawerOpen(!panel_.drawerOpen()); };
	}
	void draw(visage::Canvas& canvas) override {
		canvas.setColor(withAlpha(kLine, 0.35f));
		canvas.segment(0.0f, 0.5f, width(), 0.5f, 1.0f, false);
		canvas.setColor(kInk);
		const float cx = 14.0f;
		const float cy = height() * 0.5f;
		// THE CHEVRON: down while open (the drawer is out), right while folded.
		if (panel_.drawerOpen()) {
			canvas.segment(cx - 4.0f, cy - 2.0f, cx, cy + 2.0f, 1.4f, true);
			canvas.segment(cx, cy + 2.0f, cx + 4.0f, cy - 2.0f, 1.4f, true);
		}
		else {
			canvas.segment(cx - 2.0f, cy - 4.0f, cx + 2.0f, cy, 1.4f, true);
			canvas.segment(cx + 2.0f, cy, cx - 2.0f, cy + 4.0f, 1.4f, true);
		}
		canvas.text("CONVOLUTION", font(kControlSize), visage::Font::kLeft, 26.0f, 0.0f, 100.0f, height());
	}

private:
	FeatherverbExpanded& panel_;
};

bool FeatherverbExpanded::canBrowse() const {
	return verb() != nullptr; // the generated rooms are always there to walk
}

std::vector<fx::conv::IrChoice> FeatherverbExpanded::choices() const {
	const fv::Featherverb* v = verb();
	if (v == nullptr) {
		return {};
	}
	return fx::conv::irChoices(fx::conv::kGeneratedRooms, browseFolder());
}

std::string FeatherverbExpanded::browseFolder() const {
	// THE FOLDER: the loaded file's own; on a generated room, the LAST FOLDER BROWSED, so stepping out of the files
	// into the rooms and back does not lose the way; and before any file, the fixed one (made on first use).
	if (const fv::Featherverb* v = verb(); v != nullptr && !v->irPath().empty()) {
		lastFolder_ = std::filesystem::path(v->irPath()).parent_path().string();
	}
	return !lastFolder_.empty() ? lastFolder_ : fx::conv::defaultIrFolder(true);
}

bool FeatherverbExpanded::useFolder(const std::string& folder) {
	fv::Featherverb* v = verb();
	if (v == nullptr) {
		return false;
	}
	// ITS FIRST IMPULSE THAT LOADS, in the order the arrows walk: loading it is what points the browser there.
	for (const std::string& file : fx::conv::irFilesIn(folder)) {
		if (v->loadIr(file)) {
			bodyEdited();
			lastFolder_ = folder;
			view_->clearError();
			source_->refresh();
			stepper_->forgetFailures();
			stepper_->redraw();
			redraw();
			return true;
		}
	}
	view_->setError("nothing in " + std::filesystem::path(folder).filename().string() + " loads");
	redraw();
	return false;
}

void FeatherverbExpanded::loadIrFromDialog() {
	FileDialogOptions o;
	o.mode = FileDialogOptions::Mode::OpenFile;
	o.title = "Load impulse response";
	o.startDir = browseFolder();
	o.filters = { { "Audio", { "wav", "aif", "aiff", "flac" } } };
	o.parent = nativeWindowOf(this);
	(void)irDialog_.open(o, [this](const FileDialogResult& r) {
		fv::Featherverb* v = verb();
		if (v == nullptr) {
			return;
		}
		if (r.chosen()) {
			if (v->loadIr(r.path.string())) {
				bodyEdited();
				view_->clearError();
				source_->refresh();
				stepper_->forgetFailures();
				stepper_->redraw();
			}
			else {
				view_->setError(v->irError());
			}
		}
		else if (r.status == FileDialogResult::Status::Failed) {
			view_->setError(r.error);
		}
		redraw();
	});
}

void FeatherverbExpanded::chooseFolderFromDialog() {
	// NO DIALOG ON THIS DESKTOP (no portal, no zenity or kdialog): the menu that walks the disk, next tick.
	if (!FileDialog::available()) {
		pendingFolderMenu_ = browseFolder();
		return;
	}
	FileDialogOptions o;
	o.mode = FileDialogOptions::Mode::OpenFolder;
	o.title = "Choose impulse folder";
	o.startDir = browseFolder();
	o.parent = nativeWindowOf(this);
	(void)irDialog_.open(o, [this](const FileDialogResult& r) {
		if (r.chosen()) {
			(void)useFolder(r.path.string());
		}
		else if (r.status == FileDialogResult::Status::Failed) {
			view_->setError(r.error);
			redraw();
		}
	});
}

void FeatherverbExpanded::showFolderMenu(const std::string& folder) {
#if defined(_WIN32)
	const char* home = std::getenv("USERPROFILE");
#else
	const char* home = std::getenv("HOME");
#endif
	const std::vector<fx::conv::FolderRow> rows =
	    fx::conv::folderRows(folder, fx::conv::defaultIrFolder(true), home != nullptr ? home : "");
	using Kind = fx::conv::FolderRow::Kind;
	visage::PopupMenu menu;
	for (size_t i = 0; i < rows.size(); ++i) {
		const fx::conv::FolderRow& row = rows[i];
		if (row.kind == Kind::Break) {
			menu.addBreak();
		}
		else if (row.kind == Kind::Header) {
			menu.addOption(-1, row.label).enable(false);
		}
		else {
			menu.addOption(static_cast<int>(i), row.label).enable(row.enabled);
		}
	}
	// A STEP INTO A FOLDER opens the menu again there, at the next tick (this one is still closing); "Use" browses it.
	menu.onSelection() = [this, rows](int id) {
		if (id < 0 || static_cast<size_t>(id) >= rows.size()) {
			return;
		}
		const fx::conv::FolderRow& row = rows[static_cast<size_t>(id)];
		if (row.kind == Kind::Enter) {
			pendingFolderMenu_ = row.path;
		}
		else if (row.kind == Kind::Use) {
			(void)useFolder(row.path);
		}
	};
	showMenuAt(std::move(menu), *stepper_, stepper_->nameBounds());
}

fx::conv::IrChoice FeatherverbExpanded::currentChoice() const {
	const fv::Featherverb* v = verb();
	if (v == nullptr) {
		return {};
	}
	return v->irPath().empty() ? fx::conv::IrChoice { v->generatedRoom(), {} } : fx::conv::IrChoice { -1, v->irPath() };
}

bool FeatherverbExpanded::apply(const fx::conv::IrChoice& choice) {
	fv::Featherverb* v = verb();
	if (v == nullptr) {
		return false;
	}
	if (choice.generated >= 0) {
		v->useGeneratedIr(choice.generated);
	}
	else if (!v->loadIr(choice.path)) {
		return false;
	}
	bodyEdited();
	view_->clearError();
	redraw();
	return true;
}

bool FeatherverbExpanded::stepIr(int step) {
	if (verb() == nullptr || step == 0) {
		return false;
	}
	const int before = source_->currentItem();
	stepper_->step(step);
	return source_->currentItem() != before;
}

std::string FeatherverbExpanded::labelOf(const fx::conv::IrChoice& choice) {
	return choice.generated >= 0 ? std::string("Generated · ") + fx::conv::generatedRoomName(choice.generated)
	                             : fx::conv::irDisplayName(choice.path);
}

std::vector<std::string> FeatherverbExpanded::irListForTest() const {
	// THE SHARED ROW'S OWN LIST (`ItemStepper::listRows`), the current one marked "X" — the house mark.
	std::vector<std::string> rows;
	for (const ItemStepper::Row& row : stepper_->listRows()) {
		rows.push_back((row.current ? "X  " : "   ") + row.label);
	}
	return rows;
}

bool FeatherverbExpanded::showIrList() {
	if (verb() == nullptr) {
		return false;
	}
	stepper_->showList();
	return true;
}

visage::Frame& FeatherverbExpanded::stripForTest() const { return *strip_; }
ItemStepper& FeatherverbExpanded::stepperForTest() const { return *stepper_; }
float FeatherverbExpanded::prevArrowXForTest() { return 0.5f * Strip::kArrowW; }
float FeatherverbExpanded::nextArrowXForTest() { return 1.5f * Strip::kArrowW + Strip::kNameW; }



FeatherverbExpanded::FeatherverbExpanded(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id,
                                         const device::Descriptor& desc, Notifier* notifier)
    : GenericExpanded(host, registry, id, desc, notifier) {
	// THE PRESETS, as SKLITTER's panel builds them. The bank is read now so the first draw lists it; selecting nothing
	// on open means opening the panel changes no sound. A load keeps only the bypass.
	presets_ = std::make_unique<PresetBrowser>(std::vector<std::string> { "Init" },
	                                           std::vector<PresetData> { PresetData { initDevicePreset(desc) } },
	                                           devicePresetBank(desc), PresetBrowser::kPanelStyle);
	presets_->setBounds(0.0f, 0.0f, presets_->barWidth(), PresetBrowser::kPanelStyle.height);
	presets_->loadBankFromDisk();
	presetProvider_ = std::make_unique<DevicePresetProvider>(host, desc, id, std::span<const uint8_t> {}, notifier);
	presets_->setProvider(presetProvider_.get());
	presets_->setNotifier(notifier);
	presets_->onSelected([this](int) { redrawAll(); });
	// THE PANEL OPENS ON THE PRESET THIS DEVICE CAME FROM, and names every later one into the patch (backlog QC).
	bindPresetIdentity(
	    *presets_, [&host, id] { return host.devicePreset(id); },
	    [&host, id](const PresetIdentity& identity) { host.setDevicePreset(id, identity); });
	strip_ = std::make_unique<Strip>(*this);
	source_ = std::make_unique<IrSource>(*this);
	stepper_ = std::make_unique<ItemStepper>(*source_, ItemStepper::Style { kStripHeight - 4.0f, Strip::kNameW, Strip::kArrowW });
	strip_->addChild(stepper_.get()); // on top of the strip: it takes its own presses, the strip folds on the rest
	addChild(strip_.get());
	view_ = std::make_unique<IrView>([this]() { return verb(); }, [this] { bodyEdited(); });
	addChild(view_.get());
	stretch_ = std::make_unique<Knob>(std::make_unique<StretchControl>([this]() { return verb(); }, [this] { bodyEdited(); }), "Stretch", notifier);
	addChild(stretch_.get());
	liteControl_ = std::make_unique<LiteControl>([this]() { return verb(); }, [this] { bodyEdited(); });
	lite_ = std::make_unique<SwitchChip>(*liteControl_, "LITE");
	// NOT A PARAM CONTROL (backlog QK): LITE is the IR body's own state (`LiteControl`), which no source can reach.
	lite_->setShowsModulation(false);
	addChild(lite_.get()); // after the strip, so it is on top of it and takes its own press
	source_->refresh();
}

FeatherverbExpanded::~FeatherverbExpanded() {
	// THE STEPPER IS THE STRIP'S CHILD but is destroyed first (members go in reverse): detached before either goes,
	// so no frame is ever left pointing at a freed child.
	if (strip_ != nullptr && stepper_ != nullptr) {
		strip_->removeChild(stepper_.get());
	}
}

fv::Featherverb* FeatherverbExpanded::verb() const {
	return static_cast<fv::Featherverb*>(ownHost().containerSurfaceOf(deviceId()));
}

bool FeatherverbExpanded::drawnBySubclass(size_t index) const {
	return index == static_cast<size_t>(fv::Param::ConvMode) || index == static_cast<size_t>(fv::Param::ConvPredelay)
	       || index == static_cast<size_t>(fv::Param::ConvFeedback);
}

float FeatherverbExpanded::drawerHeight() const {
	return kStripHeight + (open_ ? kViewHeight + cellHeight() + kRowGap + kPad : 0.0f);
}

visage::Bounds FeatherverbExpanded::drawerBounds() const {
	const float h = drawerHeight();
	return { 0.0f, std::max(0.0f, static_cast<float>(height()) - h), static_cast<float>(width()), h };
}

visage::Point FeatherverbExpanded::minimumSize() const {
	const visage::Point grid = GenericExpanded::minimumSize();
	return { std::max(grid.x, kGridWidth), grid.y + drawerHeight() };
}

visage::Point FeatherverbExpanded::maximumSize() const {
	return { kNoCap, minimumSize().y + kPad };
}

void FeatherverbExpanded::setDrawerOpen(bool open) {
	if (open == open_) {
		return;
	}
	open_ = open;
	resized();
	redraw();
	minimumSizeChanged();
}

std::string FeatherverbExpanded::stripText() const {
	const fv::Featherverb* v = verb();
	if (v == nullptr) {
		return {};
	}
	if (!view_->lastError().empty()) {
		return "could not load: " + view_->lastError();
	}
	if (v->irPath().empty()) {
		return labelOf(currentChoice());
	}
	std::string text = fx::conv::irDisplayName(v->irPath());
	if (!v->irError().empty()) {
		text += " — missing (" + v->irError() + ")";
	}
	else if (v->irTruncated()) {
		text += " — cut at " + std::to_string(static_cast<int>(fv::kConvMaxIrSeconds)) + " s";
	}
	return text;
}

void FeatherverbExpanded::resized() {
	GenericExpanded::resized();
	const visage::Bounds d = drawerBounds();
	strip_->setBounds(0.0f, d.y(), d.width(), kStripHeight);
	stepper_->setBounds(Strip::kStepperX, 0.0f, stepper_->totalWidth(), kStripHeight);
	lite_->setBounds(d.width() - kPad - 48.0f, d.y() + 0.5f * (kStripHeight - SwitchChip::kHeight), 48.0f,
	                 SwitchChip::kHeight);
	const size_t mode = static_cast<size_t>(fv::Param::ConvMode);
	const size_t pre = static_cast<size_t>(fv::Param::ConvPredelay);
	const size_t fb = static_cast<size_t>(fv::Param::ConvFeedback);
	view_->setVisible(open_);
	stretch_->setVisible(open_);
	if (!open_) {
		hideCell(mode);
		hideCell(pre);
		hideCell(fb);
		return;
	}
	const float viewY = d.y() + kStripHeight;
	view_->setBounds(kPad, viewY, std::max(0.0f, d.width() - 2.0f * kPad), kViewHeight);
	const float rowY = viewY + kViewHeight + kRowGap * 0.5f;
	(void)seatCell(mode, kPad, rowY);
	(void)seatCell(pre, kPad + kCellWidth, rowY);
	(void)seatCell(fb, kPad + 2.0f * kCellWidth, rowY);
	stretch_->setFaceDiameter(Knob::kDiameter);
	stretch_->setBounds(kPad + 3.0f * kCellWidth, rowY, kCellWidth, cellHeight());
	(void)applyEnables();
}

bool FeatherverbExpanded::applyEnables() {
	const int mode = static_cast<int>(std::lround(ownHost().fieldAt(deviceId(), static_cast<size_t>(fv::Param::ConvMode))));
	if (mode == shownMode_) {
		return false;
	}
	shownMode_ = mode;
	const bool on = mode != fv::kConvOff;
	for (const fv::Param p : { fv::Param::ConvPredelay, fv::Param::ConvFeedback }) {
		if (auto* knob = dynamic_cast<Knob*>(cellOf(static_cast<size_t>(p))); knob != nullptr) {
			knob->setEnabled(on);
		}
	}
	stretch_->setEnabled(on);
	// THE CHIP HAS NO GREYED STATE, so it goes while Conv is Off — where Lite would change nothing.
	lite_->setVisible(on);
	return true;
}

bool FeatherverbExpanded::updateContent() {
	// THE FOLDER MENU THE LAST TICK ASKED FOR (`showFolderMenu`): opened here, where no other menu is closing.
	if (!pendingFolderMenu_.empty()) {
		const std::string folder = pendingFolderMenu_;
		pendingFolderMenu_.clear();
		// THE PANEL ASKING, NOT A RE-CLICK: whatever list the stepper still has open is replaced rather
		// than toggled shut (`showMenuAt`'s toggle, backlog GG).
		dismissMenuOf(*stepper_);
		showFolderMenu(folder);
	}
	bool told = GenericExpanded::updateContent();
	if (open_ && GenericExpanded::cellOf(static_cast<size_t>(fv::Param::ConvMode)) != nullptr
	    && !GenericExpanded::cellOf(static_cast<size_t>(fv::Param::ConvMode))->isVisible()) {
		resized(); // the base's re-layout hid what the drawer seats
		told = true;
	}
	if (applyEnables()) {
		redraw();
		told = true;
	}
	if (view_->refresh()) {
		told = true;
	}
	if (const fv::Featherverb* v = verb(); v != nullptr && v->irShape().lite != shownLite_) {
		shownLite_ = v->irShape().lite;
		lite_->redraw();
		told = true;
	}
	// THE BROWSER'S LIST FOLLOWS THE DEVICE: a new file, room or source re-reads it (never the per-paint reads).
	if (const fv::Featherverb* v = verb(); v != nullptr) {
		const std::string key = v->irPath() + "#" + std::to_string(v->generatedRoom()) + "#" + std::to_string(v->sourceSerial());
		if (key != shownIrKey_) {
			shownIrKey_ = key;
			source_->refresh();
			stepper_->forgetFailures();
			stepper_->redraw();
			told = true;
		}
	}
	const std::string text = stripText();
	if (text != shownStrip_) {
		shownStrip_ = text;
		view_->redraw(); // the status reads on the waveform now
		told = true;
	}
	// THE METER BESIDE INPUT (backlog LA): a frame only when a bar moved (`FeatherverbCard::updateContent`).
	if (const featherverb_meter::Fractions now = featherverb_meter::read(ownHost(), deviceId()); !(now == shownMeter_)) {
		shownMeter_ = now;
		redraw();
		told = true;
	}
	return told;
}

void FeatherverbExpanded::draw(visage::Canvas& canvas) {
	GenericExpanded::draw(canvas);
}

visage::Bounds FeatherverbExpanded::meterBounds() const {
	// THE CELL AFTER INPUT'S, in its row (Input is the grid's last: `Param::InputGain`). Empty when Input is not laid
	// out, or when its row has no cell to its right.
	const visage::Frame* cell = cellOf(static_cast<size_t>(fv::Param::InputGain));
	if (cell == nullptr || !cell->isVisible()) {
		return {};
	}
	const visage::Bounds b = cell->bounds();
	const float right = static_cast<float>(width()) - bodyRightInset() - bodyToggleReserve();
	if (static_cast<float>(b.right() + b.width()) > right) {
		return {};
	}
	const float wide = std::min(static_cast<float>(b.width()), featherverb_meter::kWidth);
	return { static_cast<float>(b.right()) + 0.5f * (static_cast<float>(b.width()) - wide), static_cast<float>(b.y()), wide,
		     static_cast<float>(b.height()) };
}

void FeatherverbExpanded::drawBodyExtras(visage::Canvas& canvas) {
	featherverb_meter::draw(canvas, meterBounds(), featherverb_meter::read(ownHost(), deviceId()));
}

} // namespace feathers::ui
