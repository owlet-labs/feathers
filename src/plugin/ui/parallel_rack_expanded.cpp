/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

#include "plugin/ui/parallel_rack_expanded.h"
#include "plugin/ui/rack_list_view.h"
#include "plugin/ui/menu_anchor.h"

#include "plugin/ui/stored_edit.h"
#include "plugin/ui/theme.h"
#include "rack/inner_chain.h"

#include <algorithm>
#include <array>

namespace feathers::ui {

namespace pr = feathers::rack::parallel;

namespace {

/// THE PANEL'S KNOBS, IN CELL ORDER: Mix and Gain, then the macros.
constexpr std::array<pr::Param, 10> kKnobs {
	pr::Param::Mix,    pr::Param::Gain,   pr::Param::Macro1, pr::Param::Macro2, pr::Param::Macro3,
	pr::Param::Macro4, pr::Param::Macro5, pr::Param::Macro6, pr::Param::Macro7, pr::Param::Macro8,
};
constexpr std::array<const char*, 10> kCaptions { "Mix", "Gain", "M1", "M2", "M3", "M4", "M5", "M6", "M7", "M8" };
/// THE MACROS: TWO ACROSS AND FOUR TALL in a narrow block at the left; Mix and Gain stand above them on their own row.
constexpr size_t kPerRow = 2;
constexpr size_t kTopKnobs = 2;
constexpr float kMacroRows = 4.0f;
/// A KNOB CELL AT THE ENGINE RAIL'S FACE SIZE (38), not the footer's 52: four rows of macros have to fit under Mix and Gain.
constexpr float kCellHeight = Knob::columnHeight(Knob::kEngineRailDiameter);
/// THE MODE ROW above Mix and Gain (stage 2): one knob cell and its gap.
/// THE VIEW'S ROW sits under the mode row, over the macros.
constexpr float kViewRow = (SwitchChip::kHeight + ParallelRackExpanded::kPad * 0.5f) + (ValueBox::kHeight + ParallelRackExpanded::kPad * 0.5f)
                           + (SwitchChip::kHeight + ParallelRackExpanded::kPad * 0.5f); // and the Tabs view's Front choice under them
constexpr float kModeRow = kCellHeight + ParallelRackExpanded::kPad + kViewRow;

/// EACH RAIL IS HANDED ITS OWN CHAIN as the handle, so its cast is the identity.
rack::InnerChain* chainItself(void* surface) { return static_cast<rack::InnerChain*>(surface); }

} // namespace

ParallelRackExpanded::ParallelRackExpanded(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id,
                                           Notifier* notifier)
    : host_(host), registry_(registry), id_(id), notifier_(notifier), pin_(host) {
	presets_ = makeRackDevicePresets(host, id, notifier);
	// THE WINDOW'S PADS AND TABS FOCUS THIS PANEL (a press walks up to a frame that accepts keys), and the keys go on to the viewed chain's
	// column, whose selection takes Ctrl+C / X / V: pasting into a chain with no card to press works from the pad or tab.
	setAcceptsKeystrokes(true);
	onKeyPress() = [this](const visage::KeyEvent& e) {
		ChainRail* rail = viewedRail();
		// A NAME BEING TYPED OWNS THE KEYBOARD (see the card): a chord the field leaves to the host must not act on the chain.
		return !(chainRename_ != nullptr && chainRename_->renaming()) && model_ != nullptr && model_->view() != RackViewKind::Strips && rail != nullptr && rail->rail != nullptr
		       && rail->rail->column() != nullptr && rail->rail->column()->processKeyPress(e);
	};
	for (size_t i = 0; i < kKnobs.size(); ++i) {
		controls_.push_back(
		    std::make_unique<DeviceControl>(host, id, pr::kDescriptor, static_cast<size_t>(kKnobs[i])));
		if (i < kTopKnobs) {
			auto knob = std::make_unique<Knob>(*controls_.back(), kCaptions[i], notifier);
			knob->setFaceDiameter(Knob::kEngineRailDiameter);
			addChild(knob.get());
			knobs_.push_back(std::move(knob));
			continue;
		}
		// A MACRO IS THE SAME SLOT THE SYNTH'S MACROS ARE (right-click: type, polarity, Rename), read through the rack's polarity.
		const size_t n = i - kTopKnobs;
		macroViews_.push_back(std::make_unique<BipolarMacro>(*controls_.back(), [this, n] {
			const pr::Rack* rack = rackNow();
			return rack != nullptr && rack->macroBipolar(n);
		}));
		auto slot = std::make_unique<MacroSlot>(*macroViews_.back(), kCaptions[i], macroNaming(n), macroHooks(n), notifier);
		slot->setFaceDiameter(Knob::kEngineRailDiameter);
		addChild(slot.get());
		macroSlots_.push_back(std::move(slot));
		// A MACRO IS A SOURCE INSIDE THE RACK as well as a destination of its host: its arrow arms it for every chain.
		auto arm = std::make_unique<SourceArm>(macroMod_, device::Source::core(n));
		arm->setTooltip("Arm Macro " + std::to_string(n + 1)
		                + " as a modulation source: then drag a control inside the chains to set how far it moves it. Press again to put it away");
		addChild(arm.get());
		macroArms_.push_back(std::move(arm));
	}
	// STAGE 2: THE MODE (a dropdown) AND THE POSITION, every control the card offers (window ⊇ card).
	modeControl_ = std::make_unique<DeviceControl>(host, id, pr::kDescriptor, static_cast<size_t>(pr::Param::Mode));
	positionControl_ = std::make_unique<DeviceControl>(host, id, pr::kDescriptor, static_cast<size_t>(pr::Param::Position));
	modeBox_ = std::make_unique<ValueBox>(*modeControl_, "Mode");
	addChild(modeBox_.get());
	position_ = std::make_unique<Knob>(*positionControl_, "Position", notifier);
	position_->setFaceDiameter(Knob::kEngineRailDiameter);
	addChild(position_.get());
	viewControl_ = std::make_unique<DeviceControl>(host, id, pr::kDescriptor, static_cast<size_t>(pr::Param::View));
	playingControl_ = std::make_unique<DeviceControl>(host, id, pr::kDescriptor, static_cast<size_t>(pr::Param::Playing));
	viewSeg_ = std::make_unique<SegmentedControl>(*viewControl_, registry, pr::kViewNames, notifier);
	playingBox_ = std::make_unique<ValueBox>(*playingControl_, "Playing");
	// WHAT TABS PUTS IN FRONT: the chain's cards, or the first device's window — two states on the face, in a row of their own, Tabs only.
	frontControl_ = std::make_unique<DeviceControl>(host, id, pr::kDescriptor, static_cast<size_t>(pr::Param::Front));
	frontSeg_ = std::make_unique<SegmentedControl>(*frontControl_, registry, pr::kFrontNames, notifier);
	addChild(frontSeg_.get());
	frontSeg_->setVisible(false);
	viewPairControl_ = std::make_unique<RackViewPairControl>(*viewControl_);
	viewPair_ = std::make_unique<SegmentedControl>(*viewPairControl_, registry, RackViewPairControl::kNames, notifier);
	addChild(viewSeg_.get());
	addChild(viewPair_.get());
	addChild(playingBox_.get());
	// WHICH CHAIN AND WHICH PAGE THE VIEW IS ON: the window's alone (the card has no tabs or pads to point them at).
	viewedControl_ = std::make_unique<DeviceControl>(host, id, pr::kDescriptor, static_cast<size_t>(pr::Param::ViewedChain));
	pageControl_ = std::make_unique<DeviceControl>(host, id, pr::kDescriptor, static_cast<size_t>(pr::Param::PadsPage));
	viewedBox_ = std::make_unique<ValueBox>(*viewedControl_, "Viewing");
	pageBox_ = std::make_unique<ValueBox>(*pageControl_, "Page");
	addChild(viewedBox_.get());
	addChild(pageBox_.get());
	unusedControl_ = std::make_unique<DeviceControl>(host, id, pr::kDescriptor, static_cast<size_t>(pr::Param::Unused));
	unused_ = std::make_unique<SegmentedControl>(*unusedControl_, registry, pr::kUnusedFaceNames, notifier);
	addChild(unused_.get());
	// BANDS (step 3), under "+ Chain": the count and the filtering (dropdowns), the slope (a chip), the four corners.
	const auto own = [&](pr::Param p) {
		bandControls_.push_back(std::make_unique<DeviceControl>(host, id, pr::kDescriptor, static_cast<size_t>(p)));
		return bandControls_.back().get();
	};
	bandsBox_ = std::make_unique<ValueBox>(*own(pr::Param::BandCount), "Bands");
	filteringBox_ = std::make_unique<ValueBox>(*own(pr::Param::Filtering), "Filtering");
	slope_ = std::make_unique<ValueBox>(*own(pr::Param::Slope), "Slope");
	addChild(bandsBox_.get());
	addChild(filteringBox_.get());
	addChild(slope_.get());
	for (size_t k = 0; k < 4; ++k) {
		cuts_[k] = std::make_unique<Knob>(*own(static_cast<pr::Param>(static_cast<size_t>(pr::Param::Cut1) + k)),
		                                  pr::kCutCaptions[k], notifier);
		cuts_[k]->setFaceDiameter(Knob::kEngineRailDiameter);
		addChild(cuts_[k].get());
	}
	keyTrack_ = std::make_unique<SwitchChip>(*own(pr::Param::KeyTrack), "Key Track");
	addChild(keyTrack_.get());
	gated_ = { { keyTrack_.get(), pr::Param::KeyTrack }, { playingBox_.get(), pr::Param::Playing }, { position_.get(), pr::Param::Position },   { unused_.get(), pr::Param::Unused },
	           { bandsBox_.get(), pr::Param::BandCount },  { filteringBox_.get(), pr::Param::Filtering },
	           { slope_.get(), pr::Param::Slope },         { cuts_[0].get(), pr::Param::Cut1 },
	           { cuts_[1].get(), pr::Param::Cut2 },        { cuts_[2].get(), pr::Param::Cut3 },
	           { cuts_[3].get(), pr::Param::Cut4 } };
	// STEP 4'S VIEWS OVER THE RAILS (§7a): the spectrum strip in Bands, Mid/Side and Left/Right; the position bar in Select
	// and Blend. The card's own, so the window offers everything the card does.
	spectrum_ = std::make_unique<RackSpectrumStrip>(
	    host_, id_,
	    std::array<DeviceControl*, 4> { bandControls_[3].get(), bandControls_[4].get(), bandControls_[5].get(),
	                                    bandControls_[6].get() },
	    *modeControl_, *bandControls_[0]);
	addChild(spectrum_.get());
	positionBar_ = std::make_unique<RackPositionBar>(host_, id_, *positionControl_, *modeControl_);
	addChild(positionBar_.get());
	gated_.push_back({ positionBar_.get(), pr::Param::Position });
	// THE TABS AND PADS VIEWS: the bar over the rails and the pads panel beside them, hidden until `View` asks. The chain strips keep
	// every per-chain control (level, mute, solo, key, choke) for the chain in front; an empty pad offers "+".
	// FOLLOW PLAYED NOTE: a two-state chip under the pads panel (the window's alone — the card has no room), off by default.
	followControl_ = std::make_unique<DeviceControl>(host_, id_, pr::kDescriptor, static_cast<size_t>(pr::Param::FollowPlayed));
	followChip_ = std::make_unique<SwitchChip>(*followControl_, "Follow played note");
	addChild(followChip_.get());
	followChip_->setVisible(false);
	model_ = std::make_unique<RackViewAdapter>(host_, id_,
	                                           RackViewAdapter::Controls { playingControl_.get(), modeControl_.get(), positionControl_.get(),
	                                                                       controls_[1].get(), followControl_.get() });
	tabs_ = std::make_unique<RackTabBar>(*model_, RackFlow::TopDown);
	pane_ = std::make_unique<RackWindowPane>();
	addChild(pane_.get());
	pads_ = std::make_unique<RackPadsPanel>(*model_, pulse_, RackFlow::TopDown);
	// "+" ON AN EMPTY PAD ASKS WHICH DEVICE (the lanes' own add menu); the pick makes the chain on that pad's key.
	// THE VIEWING AND PLAYING BOXES PICK A CHAIN BY NAME (a menu of the chains' one label each), not by a typed number.
	// THEIR READOUT IS THE CHAIN'S LABEL (the control's own text, not the unit's number), cut with "..." from the end when it does not fit.
	for (auto* box : { viewedBox_.get(), playingBox_.get() }) {
		box->setReadoutFormatter([box](double) { return box->control().text(); }, false);
		box->setElideReadout(true);
	}
	viewedBox_->onPicker([this] {
		showMenuAt(chainPickerMenu(false), *viewedBox_, visage::Bounds { 0.0f, 0.0f, static_cast<float>(viewedBox_->width()), static_cast<float>(viewedBox_->height()) });
	});
	playingBox_->onPicker([this] {
		showMenuAt(chainPickerMenu(true), *playingBox_, visage::Bounds { 0.0f, 0.0f, static_cast<float>(playingBox_->width()), static_cast<float>(playingBox_->height()) });
	});
	// "RENAME…" FROM A TAB OR A USED PAD'S RIGHT PRESS: the one chain label everywhere reads it back.
	chainRename_ = std::make_unique<RackChainRename>(*model_, *this, notifier_);
	tabs_->setContextHandler([this](size_t slot, const visage::Bounds& anchor) { chainRename_->showMenu(slot, *tabs_, anchor); });
	tabs_->setRenameHandler([this](size_t slot, const visage::Bounds& anchor) { chainRename_->begin(slot, *tabs_, anchor); });
	list_ = std::make_unique<RackListView>(*model_, host_, id_, *chainRename_);
	list_->setSummaryFn([this](size_t slot) { return model_->chainLabel(slot); });
	addChild(list_.get());
	list_->setVisible(false);
	pads_->gridForTest().setContextHandler([this](size_t slot, const visage::Bounds& anchor) { chainRename_->showMenu(slot, pads_->gridForTest(), anchor); });
	padAdd_ = std::make_unique<RackPadAdd>(*model_, notifier_);
	padAdd_->onAdded([this](size_t) {
		if (follow()) {
			minimumSizeChanged();
			redraw();
		}
	});
	pads_->gridForTest().setAddHandler([this](int key) {
		const int index = key - padKey(model_->page(), 0);
		RackPadGrid& grid = pads_->gridForTest();
		const visage::Point c = grid.plusCentre(index);
		const float a = RackPadGrid::kPlusArea;
		padAdd_->open(key, grid, visage::Bounds { c.x - a * 0.5f, c.y - a * 0.5f, a, a });
	});
	addChild(tabs_.get());
	addChild(pads_.get());
	tabs_->setVisible(false);
	pads_->setVisible(false);
	// THE COLUMN'S TWO FOLD HEADERS, in its top pad, and the frames the inspector's fold hides.
	inspectorHeader_ = std::make_unique<FoldHeader>("INSPECTOR");
	macrosHeader_ = std::make_unique<FoldHeader>("MACROS");
	inspectorHeader_->onToggle = [this] {
		if (setInspectorFolded(!inspectorFolded()) && notifier_ != nullptr) {
			notifier_->say(inspectorFolded() ? "Inspector minimised" : "Inspector shown");
		}
	};
	macrosHeader_->onToggle = [this] {
		if (setMacrosFolded(!macrosFolded()) && notifier_ != nullptr) {
			notifier_->say(macrosFolded() ? "Macros minimised" : "Macros shown");
		}
	};
	addChild(inspectorHeader_.get());
	addChild(macrosHeader_.get());
	inspectorFrames_ = { modeBox_.get(), position_.get(), unused_.get(), viewSeg_.get(), viewPair_.get(), playingBox_.get(), viewedBox_.get(),
	                     pageBox_.get(), frontSeg_.get(), bandsBox_.get(), filteringBox_.get(), slope_.get(), keyTrack_.get(),
	                     knobs_[0].get(), knobs_[1].get() };
	for (auto& cut : cuts_) {
		inspectorFrames_.push_back(cut.get());
	}
	inspectorHeader_->setFolded(inspectorFolded());
	macrosHeader_->setFolded(macrosFolded());
	foldsAppliedMacros_ = macrosFolded();
	foldsAppliedInspector_ = inspectorFolded();
	(void)showForMode();
	applyFolds();
	onMouseDown() = [this](const visage::MouseEvent& e) {
		if (addBounds().contains(e.position)) {
			addChain();
			return;
		}
	};
	(void)follow();
}

ParallelRackExpanded::~ParallelRackExpanded() {
	(void)relay_.release(); // the host's cards must not stay lit for a modulator nobody can see
	// THE PANE'S DEVICE WINDOW IS ON A RAIL'S MODULATION LIST: it goes first.
	if (pane_ != nullptr) {
		pane_->release();
	}
	for (auto& r : rails_) {
		removeChild(r->rail.get());
	}
	rails_.clear(); // before the pin lets the instance go
}

std::vector<pr::Param> ParallelRackExpanded::addressedParamsForTest() const {
	return std::vector<pr::Param>(kKnobs.begin(), kKnobs.end());
}

float ParallelRackExpanded::railsWidth() const {
	float w = 0.0f;
	for (const auto& r : rails_) {
		w += r->rail->railWidth();
	}
	return w;
}

visage::Point ParallelRackExpanded::minimumSize() const {
	visage::Point p { railsLeft() + railsWidth() + phantomsWidth(),
		              std::max(leftColumnFoot().bottom(), bandsBottom()) + kPad };
	// STRIPS: THE ADD BUTTON STANDS AFTER THE LAST STRIP, so the window is as wide as it needs.
	if (model_ != nullptr && model_->view() == RackViewKind::Strips && addShown()) {
		p.x += kPad * 0.5f + kAddWidth + kPad;
	}
	// PADS: the panel stands before the rails, so the window is at least its width and height.
	if (model_ != nullptr && model_->view() == RackViewKind::Pads) {
		const visage::Point n = pads_->naturalSize();
		p.x = std::max(p.x, railsLeft() + n.x + kPad + 34.0f); // and a collapsed rail (34 px) beside it
		p.y = std::max(p.y, railsTop() + n.y + kPad);
	}
	// **THE PANE: THE WINDOW SIZES TO WHAT IS IN FRONT** — the front device's window at its natural size, the rest of the chain's rail beside
	// it, under the tab bar. Not the sum of every chain's rail, and not the largest tab: a tab switch reflows (the rack tells us, `follow`).
	const bool paneFront = model_ != nullptr && paneOn() && pane_->hosting() != RackWindowPane::Hosting::Nothing;
	if (paneFront) {
		const visage::Point n = pane_->naturalSize();
		const ChainRail* front = viewedRail();
		const bool pads = model_->view() == RackViewKind::Pads;
		// TABS: after the tab column. PADS: beside the pads panel (and its Follow chip), which stands before the pane and the rail.
		const float top = railsTop();
		const visage::Point padsN = pads ? pads_->naturalSize() : visage::Point { 0.0f, 0.0f };
		const float padsAcross = pads ? padsN.x + kPad : selectorWidth() + kPad * 0.5f;
		p.x = railsLeft() + padsAcross + n.x + kPad * 0.5f + (front != nullptr ? front->rail->railWidth() : 0.0f) + kPad;
		p.y = std::max(std::max(leftColumnFoot().bottom(), bandsBottom()) + kPad, top + n.y + kPad);
		if (pads) {
			p.y = std::max(p.y, top + padsN.y + kPad * 0.5f + SwitchChip::kHeight + kPad);
		}
		if (front != nullptr) {
			p.y = std::max(p.y, top + (front->rail->collapsed() ? Rail::kCollapsedChromeHeight + ChainStrip::kColumnMinimumHeight
			                                                     : front->rail->openMinimumHeight()));
		}
		return p;
	}
	// **TABS, NO PANE: THE WINDOW IS THE TAB COLUMN, THE FRONT CHAIN'S RAIL AND NOTHING ELSE** — never the sum of every chain's rail, so eight tabs
	// make the window no wider than two over the same front content.
	if (model_ != nullptr && tabbedView(model_->view())) {
		const ChainRail* front = viewedRail();
		p.x = railsLeft() + selectorWidth() + kPad * 0.5f + (front != nullptr ? front->rail->railWidth() : 0.0f) + kPad;
		p.y = std::max(std::max(leftColumnFoot().bottom(), bandsBottom()) + kPad,
		               railsTop() + tabsColumnLength() + kPad * 0.5f + RackTabBar::kThickness + kPad);
		if (front != nullptr) {
			p.y = std::max(p.y, railsTop() + (front->rail->collapsed() ? Rail::kCollapsedChromeHeight + ChainStrip::kColumnMinimumHeight
			                                                           : front->rail->openMinimumHeight()));
		}
		return p;
	}
	for (const auto& r : rails_) {
		// A CLOSED RAIL IS A CHANNEL STRIP NOW (backlog OC), and needs its column's height; an open one its floor.
		p.y = std::max(p.y, r->rail->collapsed() ? Rail::kCollapsedChromeHeight + ChainStrip::kColumnMinimumHeight
		                                         : r->rail->openMinimumHeight());
	}
	return p;
}

bool ParallelRackExpanded::paneOn() const {
	return model_ != nullptr && !hostedInPane_ && (tabbedView(model_->view()) || model_->view() == RackViewKind::Pads)
	       && std::lround(frontControl_->value()) >= 1;
}

ParallelRackExpanded::ChainRail* ParallelRackExpanded::viewedRail() const {
	const size_t viewed = model_ != nullptr ? model_->viewedChain() : kNoChain;
	return viewed < rails_.size() ? rails_[viewed].get() : nullptr;
}

bool ParallelRackExpanded::validatePane() {
	if (pane_ == nullptr || pane_->hosting() == RackWindowPane::Hosting::Nothing) {
		return false;
	}
	// ASKED OF THE RACK AFRESH, before the rails follow: the chain in front and its bridge's life token, compared and never dereferenced.
	const pr::Rack* rack = rackNow();
	const size_t viewed = model_ != nullptr ? model_->viewedChain() : kNoChain;
	const rack::InnerChain* chain = rack != nullptr && viewed < rack->chainCount() ? rack->chainAt(viewed) : nullptr;
	const rack::RackBridge* bridge = chain != nullptr ? chain->bridge() : nullptr;
	if (!paneOn() || bridge == nullptr) {
		pane_->release();
		return true;
	}
	return pane_->validate(rack->chainIdAt(viewed), bridge->fragmentLife());
}

bool ParallelRackExpanded::syncPane() {
	bool changed = false;
	ChainRail* front = viewedRail();
	if (!paneOn() || front == nullptr || front->rail->column() == nullptr) {
		changed = pane_->hosting() != RackWindowPane::Hosting::Nothing;
		pane_->release();
	}
	else {
		changed = pane_->follow(front->rail->column(), front->id);
	}
	// THE FIRST CARD IS HELD BACK FROM THE FRONT RAIL'S COLUMN WHILE THE PANE SHOWS IT, and from no other (a rebuilt column starts with none held).
	const bool hosting = pane_->hosting() != RackWindowPane::Hosting::Nothing;
	for (auto& r : rails_) {
		if (ChainColumn* column = r->rail->column(); column != nullptr) {
			column->setHiddenLead(hosting && r.get() == front ? 1 : 0);
		}
	}
	return changed;
}

float ParallelRackExpanded::dividerY() const { return kPad + kModeRow + kCellHeight + kPad * 0.5f; }

float ParallelRackExpanded::inspectorShift() const {
	// THE INSPECTOR FOLDED, THE MACROS RISE UNDER ITS HEADER AND THEIR OWN: what stood above them (the mode row, the view rows and Mix and Gain)
	// is gone, so the first macro row stands two header rows down (the inspector's, in the top pad, and the macros').
	return inspectorFolded() ? dividerY() + kPad * 0.5f - 2.0f * kPad : 0.0f;
}

bool ParallelRackExpanded::macrosFolded() const { return host_.fieldAt(id_, static_cast<size_t>(pr::Param::MacrosFold)) >= 0.5f; }
bool ParallelRackExpanded::inspectorFolded() const { return host_.fieldAt(id_, static_cast<size_t>(pr::Param::InspectorFold)) >= 0.5f; }

bool ParallelRackExpanded::setMacrosFolded(bool folded) {
	if (macrosFolded() == folded) {
		return false;
	}
	host_.setField(id_, static_cast<size_t>(pr::Param::MacrosFold), folded ? 1.0f : 0.0f);
	applyFolds();
	return true;
}

bool ParallelRackExpanded::setInspectorFolded(bool folded) {
	if (inspectorFolded() == folded) {
		return false;
	}
	host_.setField(id_, static_cast<size_t>(pr::Param::InspectorFold), folded ? 1.0f : 0.0f);
	applyFolds();
	return true;
}

void ParallelRackExpanded::applyFolds() {
	const bool inspector = inspectorFolded();
	const bool macros = macrosFolded();
	const bool inspectorChanged = inspector != foldsAppliedInspector_;
	foldsAppliedInspector_ = inspector;
	foldsAppliedMacros_ = macros;
	inspectorHeader_->setFolded(inspector);
	macrosHeader_->setFolded(macros);
	// FOLDED, A HALF'S FRAMES ARE HIDDEN; UNFOLDED, THEY ARE LIFTED TO SHOWN AND THE MODE'S OWN RULES (the gates, the view pair) HIDE WHAT THE MODE HIDES.
	for (visage::Frame* frame : inspectorFrames_) {
		if (frame->isVisible() == inspector) {
			frame->setVisible(!inspector);
		}
	}
	for (auto& slot : macroSlots_) {
		if (slot->isVisible() == macros) {
			slot->setVisible(!macros);
		}
	}
	for (auto& arm : macroArms_) {
		if (arm->isVisible() == macros) {
			arm->setVisible(!macros);
		}
	}
	if (inspectorChanged && !inspector) {
		laidOutMode_ = -1.0; // the gates and the view pair are asked afresh
		(void)showForMode();
	}
	layOut();
	minimumSizeChanged();
	redraw();
}

float ParallelRackExpanded::knobY(size_t i) const {
	if (i < kTopKnobs) {
		return kPad + kModeRow;
	}
	const auto row = static_cast<float>((i - kTopKnobs) / kPerRow);
	return dividerY() + kPad * 0.5f + row * (kCellHeight + kPad) - inspectorShift();
}

bool ParallelRackExpanded::bandsColumn() const {
	return std::lround(modeControl_->value()) == static_cast<long>(pr::Mode::Bands);
}

float ParallelRackExpanded::phantomsWidth() const {
	float w = 0.0f;
	for (const auto& phantom : phantoms_) {
		w += phantom->rail->railWidth();
	}
	return w;
}

float ParallelRackExpanded::railsLeft() const {
	// IN BANDS A SECOND COLUMN BESIDE THE MACROS holds the band controls, so the whole of it sits in the window's height
	// (it ran off the bottom when it hung under "+ Chain"); the rails start after it.
	return kColumnWidth + 2.0f * kPad + (bandsColumn() && !inspectorFolded() ? kColumnWidth + kPad : 0.0f);
}

float ParallelRackExpanded::bandsBottom() const {
	// The second column's foot: the count, the filtering, the slope, then the four corners one under another.
	return bandsTop() + 2.0f * (ValueBox::kHeight + kPad * 0.5f) + 2.0f * kSegmentRow + 4.0f * (kCellHeight + kPad * 0.5f);
}

float ParallelRackExpanded::bandsTop() const { return kPad + kModeRow; }


float ParallelRackExpanded::tabsColumnLength() const {
	if (model_ != nullptr && model_->view() == RackViewKind::List) {
		return RackListView::heightFor(model_->chainCount()); // every row; the layout cuts it to the room and the list scrolls
	}
	const size_t n = model_ != nullptr ? tabbedChains(*model_).size() : 0;
	return static_cast<float>(n) * RackTabBar::kMaxLength;
}

float ParallelRackExpanded::selectorWidth() const {
	return model_ != nullptr && model_->view() == RackViewKind::List ? RackListView::kColumnWidth : RackTabBar::kThickness;
}

visage::Bounds ParallelRackExpanded::leftColumnFoot() const {
	// WHERE THE LEFT COLUMN ENDS: under the macros (the button used to stand here, and the column's height is still what it was).
	return { kPad, dividerY() + kPad * 0.5f + (macrosFolded() ? 0.0f : kMacroRows * (kCellHeight + kPad)) - inspectorShift(), kAddWidth, kAddHeight };
}

visage::Bounds ParallelRackExpanded::addSlotBounds() const {
	// STRIPS: AFTER THE LAST CHAIN'S STRIP (and the roles' placeholders), level with the strips' heads, so it moves as chains are added. The other
	// views keep it under the macros: their rails are one chain's, and the button adds a chain whichever is in front.
	if (model_ != nullptr && model_->view() == RackViewKind::Strips) {
		return { railsLeft() + railsWidth() + phantomsWidth() + kPad * 0.5f, railsTop() + kPad, kAddWidth, kAddHeight };
	}
	// TABS: A "+" AT THE END OF THE TAB COLUMN, one tab's width, under the last tab.
	if (model_ != nullptr && tabbedView(model_->view())) {
		return { railsLeft(), railsTop() + std::min(tabsColumnLength(), std::max(0.0f, static_cast<float>(height()) - railsTop() - kPad - RackTabBar::kThickness)) + kPad * 0.5f,
		         RackTabBar::kThickness, RackTabBar::kThickness };
	}
	return leftColumnFoot();
}

bool ParallelRackExpanded::addShown() const {
	const auto mode = static_cast<pr::Mode>(std::lround(modeControl_->value()));
	// TABS HOLD AT MOST EIGHT CHAINS: past them the add control is gone (not greyed).
	if (model_ != nullptr && model_->view() == RackViewKind::Tabs && model_->chainCount() >= kMaxRackTabs) { // the list has no such cap
		return false;
	}
	return (pr::kRoleModes & pr::kModeBit(mode)) == 0;
}

visage::Bounds ParallelRackExpanded::addBounds() const { return addShown() ? addSlotBounds() : visage::Bounds {}; }

// THE H-LINE IS THE TOP EDGE OF THE GAP between Mix and Gain and the macros, and the macros' header fills the rest of it.
visage::Bounds ParallelRackExpanded::dividerBounds() const {
	return inspectorFolded() ? visage::Bounds {} : visage::Bounds { kPad, dividerY() - kPad * 0.5f, kColumnWidth, 1.0f };
}

visage::PopupMenu ParallelRackExpanded::chainPickerMenu(bool playing) {
	DeviceControl& control = playing ? *playingControl_ : *viewedControl_;
	visage::PopupMenu menu;
	const long current = std::lround(control.value());
	if (playing) {
		menu.addOption(0, "Position").select(current == 0);
	}
	for (size_t i = 0; i < model_->chainCount(); ++i) {
		const int id = static_cast<int>(playing ? i + 1 : i);
		menu.addOption(id, model_->chainLabel(i)).select(current == id);
	}
	const std::weak_ptr<void> alive = alive_;
	DeviceControl* target = &control;
	menu.onSelection() = [alive, target](int id) {
		if (alive.expired() || id < 0) {
			return;
		}
		// ONE EDIT, bracketed as a hand's pick of any choice is.
		target->beginGesture();
		target->setValue(static_cast<double>(id));
		target->endGesture();
	};
	return menu;
}

void ParallelRackExpanded::layOut() {
	const float cell = kColumnWidth / static_cast<float>(kPerRow);
	// THE FOLD HEADERS, in the column's top pad: the inspector's, then the macros'. Re-set unconditionally, as every bound here is.
	// STACKED, EACH DIRECTLY OVER ITS HALF: the inspector's in the top pad over the mode row, the macros' in the gap over the first macro row
	// (under the H-line when the inspector is open, right under the inspector's header when it is folded).
	inspectorHeader_->setBounds(kPad, 0.0f, kColumnWidth, kPad);
	{
		const float firstMacroRow = dividerY() + kPad * 0.5f - inspectorShift();
		const float headerTop = inspectorFolded() ? firstMacroRow - kPad : firstMacroRow - kPad + 1.0f;
		macrosHeader_->setBounds(kPad, headerTop, kColumnWidth, inspectorFolded() ? kPad : kPad - 1.0f);
	}
	// THE MODE ROW: the dropdown at the top of the left cell, the position's knob in the right one.
	modeBox_->setBounds(kPad, kPad, cell, ValueBox::kHeight);
	position_->setBounds(kPad + cell, kPad, cell, kCellHeight);
	// UNUSED UNDER THE MODE, THE COLUMN'S FULL WIDTH: its two words need the room (they ran together in one cell), and it
	// shows only in the role modes, where the position's knob does not.
	unused_->setBounds(kPad, kPad + kCellHeight - SwitchChip::kHeight, kColumnWidth, SwitchChip::kHeight);
	// THE VIEW (no audio effect) UNDER THE MODE ROW, in two rows of its own so nothing runs together: the selector across the WHOLE column
	// (a third of it each: "Strips", "Tabs" and "Pads" need the room), then three equal boxes — Select's playing chain, the chain the
	// window is looking at, and the page of pads. A box that is not shown keeps its cell, so the others never move.
	{
		const float vy = kPad + kCellHeight + kPad * 0.5f;
		const float selY = vy + (SwitchChip::kHeight - SwitchChip::kHeight) * 0.5f;
		viewSeg_->setBounds(kPad, selY, kColumnWidth, SwitchChip::kHeight);
		viewPair_->setBounds(kPad, selY, kColumnWidth, SwitchChip::kHeight);
		const float vy2 = vy + SwitchChip::kHeight + kPad * 0.5f;
		const float third = std::floor((kColumnWidth - 2.0f * kPad * 0.5f) / 3.0f);
		const float gap = kPad * 0.5f;
		playingBox_->setBounds(kPad, vy2, third, ValueBox::kHeight);
		viewedBox_->setBounds(kPad + third + gap, vy2, third, ValueBox::kHeight);
		pageBox_->setBounds(kPad + 2.0f * (third + gap), vy2, kColumnWidth - 2.0f * (third + gap), ValueBox::kHeight);
		// WHAT TABS PUTS IN FRONT, in a row of its own under them (Tabs only, and not in a pane, where it would choose nothing).
		frontSeg_->setBounds(kPad, vy2 + ValueBox::kHeight + gap, kColumnWidth, SwitchChip::kHeight);
	}
	// THE BANDS COLUMN, beside the macros: count and filtering stacked, the slope, then the corners A VERTICAL STACK, one per
	// row, lowest first — the order they stand in on the spectrum.
	const float bx = kColumnWidth + 2.0f * kPad;
	float y = bandsTop();
	bandsBox_->setBounds(bx, y, kColumnWidth, ValueBox::kHeight);
	y += ValueBox::kHeight + kPad * 0.5f;
	filteringBox_->setBounds(bx, y, kColumnWidth, ValueBox::kHeight);
	y += ValueBox::kHeight + kPad * 0.5f;
	slope_->setBounds(bx, y, kColumnWidth, ValueBox::kHeight);
	y += ValueBox::kHeight + kPad * 0.5f;
	keyTrack_->setBounds(bx, y, kColumnWidth, SwitchChip::kHeight);
	y += kSegmentRow;
	for (size_t k = 0; k < 4; ++k) {
		cuts_[k]->setBounds(bx, y + static_cast<float>(k) * (kCellHeight + kPad * 0.5f), kColumnWidth, kCellHeight);
	}
	for (size_t i = 0; i < kKnobs.size(); ++i) {
		const size_t col = i < kTopKnobs ? i : (i - kTopKnobs) % kPerRow;
		visage::Frame& cellFrame = i < kTopKnobs ? static_cast<visage::Frame&>(*knobs_[i])
		                                          : static_cast<visage::Frame&>(*macroSlots_[i - kTopKnobs]);
		cellFrame.setBounds(kPad + static_cast<float>(col) * cell, knobY(i), i < kTopKnobs ? cell : kMacroKnobWidth, kCellHeight);
		if (i >= kTopKnobs) {
			// THE ARROW BESIDE ITS KNOB, level with the dial's face (a Chain's macro row).
			macroArms_[i - kTopKnobs]->setBounds(kPad + static_cast<float>(col) * cell + kMacroKnobWidth,
			                                     knobY(i) + (Knob::kEngineRailDiameter - SourceArm::kHeight) * 0.5f,
			                                     cell - kMacroKnobWidth, SourceArm::kHeight);
		}
	}
	// THE RAILS LEFT TO RIGHT IN CHAIN ORDER, after the column — under the strip or the bar when the mode has one.
	float top = railsTop();
	const float x0 = railsLeft();
	float railX0 = x0;
	// TABS: the bar across the top of the rails and only the chain in front below it. PADS: the panel before the rails, and only the
	// selected pad's chain. STRIPS: every rail, as before.
	const RackViewKind view = model_->view();
	tabs_->setVisible(view == RackViewKind::Tabs);
	list_->setVisible(view == RackViewKind::List);
	frontSeg_->setVisible((tabbedView(view) || view == RackViewKind::Pads) && !hostedInPane_ && !inspectorFolded());
	pads_->setVisible(view == RackViewKind::Pads);
	followChip_->setVisible(view == RackViewKind::Pads);
	if (view == RackViewKind::List) {
		// **THE LIST IS THE TABS' COLUMN, WIDER**: the same place (after the controls column), `kColumnWidth` across, as tall as the rows need or the
		// window has room for, and scrolling past that. The cards or the pane follow it as they follow the tabs.
		const float room = std::max(0.0f, static_cast<float>(height()) - top - kPad);
		list_->setBounds(x0, top, RackListView::kColumnWidth, std::min(room, tabsColumnLength()));
		railX0 = x0 + RackListView::kColumnWidth + kPad * 0.5f;
	}
	else if (view == RackViewKind::Tabs) {
		// **THE TABS RUN DOWN THE LEFT SIDE, AFTER THE CONTROLS COLUMN** (a thin column, as the card's is in a left-right lane): the bar no longer
		// stretches across the window, so it sets no width — the content (the rail's cards, or the pane) does. Each tab is at most `kMaxLength` long,
		// so a few chains are a short column, not tabs spread over the window's height.
		tabs_->setFlow(RackFlow::LeftRight);
		const float room = std::max(0.0f, static_cast<float>(height()) - top - kPad);
		tabs_->setBounds(x0, top, RackTabBar::kThickness, std::min(room, tabsColumnLength()));
		railX0 = x0 + RackTabBar::kThickness + kPad * 0.5f;
	}
	else if (view == RackViewKind::Pads) {
		const visage::Point n = pads_->naturalSize();
		pads_->setBounds(x0, top, n.x, n.y);
		followChip_->setBounds(x0, top + n.y + kPad * 0.5f, n.x, SwitchChip::kHeight);
		railX0 = x0 + n.x + kPad;
	}
	const size_t viewed = model_->viewedChain();
	laidOutView_ = static_cast<int>(view);
	laidOutViewed_ = viewed;
	// THE PANE: the front device's own window at its natural size, the rest of the chain's rail beside it (the rail's column holds the first
	// card back while the pane shows it). Re-set unconditionally, as every bound here is: a scale change moves no logical rectangle.
	const bool paneShown = pane_->hosting() != RackWindowPane::Hosting::Nothing && paneOn();
	pane_->setVisible(paneShown);
	if (paneShown) {
		const visage::Point n = pane_->naturalSize();
		// TABS: after the tab column. PADS: after the pads panel (`railX0` is already past it), the grid, strip and kit level staying where they are.
		const float paneX = railX0;
		pane_->setBounds(paneX, top, n.x, n.y);
		railX0 = paneX + n.x + kPad * 0.5f;
		laidOutPane_ = n;
	}
	else {
		laidOutPane_ = { 0.0f, 0.0f };
	}
	float x = railX0;
	for (size_t i = 0; i < rails_.size(); ++i) {
		auto& r = rails_[i];
		const bool shown = view == RackViewKind::Strips || i == viewed;
		r->rail->setVisible(shown);
		if (!shown) {
			continue;
		}
		const float wide = r->rail->railWidth();
		r->rail->setBounds(x, top, wide, std::max(0.0f, static_cast<float>(height()) - top));
		x += wide;
	}
	const float across = std::max(x - railX0 + phantomsWidth(), kPlaceholderWidth);
	spectrum_->setBounds(x0, kPad * 0.5f, across, RackSpectrumStrip::kWindowHeight);
	positionBar_->setBounds(x0, kPad * 0.5f, across, RackPositionBar::kHeight);
	// A ROLE NO CHAIN FILLS (§2a): a PHANTOM RAIL after the last real one, an empty chain's face.
	for (auto& phantom : phantoms_) {
		phantom->rail->setVisible(view == RackViewKind::Strips);
		if (view != RackViewKind::Strips) {
			continue;
		}
		// THE SAME RAIL A REAL EMPTY CHAIN GETS, at its own width: collapsed, the 34 px channel strip; open, its column.
		const float wide = phantom->rail->railWidth();
		phantom->rail->setBounds(x, top, wide, std::max(0.0f, static_cast<float>(height()) - top));
		x += wide;
	}
	laidOutMode_ = modeControl_->value();
	laidOutBands_ = bandControls_[0]->value();
}

void ParallelRackExpanded::resized() { layOut(); }

void ParallelRackExpanded::addChain() {
	pr::Rack* rack = pr::rackOf(host_.containerSurfaceOf(id_));
	const size_t at = rack != nullptr ? rack->addChain() : SIZE_MAX;
	if (at != SIZE_MAX) {
		// ONE UNDO STEP, the chain whole as it was made — a redo puts back this chain, under this id.
		if (history_ != nullptr) {
			history_->record(std::make_unique<ChainPresenceEdit>(host_, id_, at, rack->snapshotChain(at), true));
		}
		if (follow()) {
			minimumSizeChanged();
			redraw();
		}
		if (notifier_ != nullptr) {
			notifier_->say("Chain added");
		}
	}
}

bool ParallelRackExpanded::moveAcrossChains(uint16_t from, std::span<const DeviceInstanceId> ids, visage::Point inWindow) {
	// **A RACK'S CHAINS SHARE ONE ID COUNTER** (gap G3), so a device keeps its id across them — `FragmentVerbs::moveInto`
	// (ids kept, its routes kept where the new chain's gate admits them, ONE undo step in the source chain's history,
	// which forwards out to the host's).
	ChainColumn* source = nullptr;
	for (const auto& r : rails_) {
		if (r->id == from) {
			source = r->rail->column();
		}
	}
	if (source == nullptr || ids.empty()) {
		return false;
	}
	for (const auto& r : rails_) {
		ChainColumn* target = r->rail->column();
		if (r->id == from || target == nullptr || r->rail->collapsed()) {
			continue;
		}
		const std::optional<size_t> position = target->dropPositionAtWindow(inWindow);
		if (!position.has_value()) {
			continue;
		}
		const std::vector<DeviceInstanceId> moved = source->selection().verbs().moveInto(target->bridge(), ids, *position);
		return !moved.empty();
	}
	return false;
}

void ParallelRackExpanded::removeChain(uint16_t chainId) {
	pr::Rack* rack = pr::rackOf(host_.containerSurfaceOf(id_));
	if (rack == nullptr) {
		return;
	}
	const size_t at = rack->indexOfChain(chainId);
	if (at == SIZE_MAX) {
		return;
	}
	// ONE UNDO STEP, taken BEFORE the chain goes: an undo puts it back whole, at its place, under its id.
	pr::Rack::ChainSnapshot snapshot = rack->snapshotChain(at);
	if (rack->removeChain(at)) {
		if (history_ != nullptr) {
			history_->record(std::make_unique<ChainPresenceEdit>(host_, id_, at, std::move(snapshot), false));
		}
		// THE RAIL GOES ON THE NEXT TICK, not inside its own strip's press: the strip is the frame being pressed.
		if (notifier_ != nullptr) {
			notifier_->say("Chain removed");
		}
	}
}

void ParallelRackExpanded::keepOpenState() {
	if (applying_) {
		return;
	}
	pr::Rack* rack = pr::rackOf(host_.containerSurfaceOf(id_));
	if (rack == nullptr) {
		return;
	}
	// A HAND OPENED OR CLOSED A RAIL: each chain's state, by its stable id, goes to the rack (view state: dirty, no undo step).
	for (const auto& r : rails_) {
		if (const size_t at = rack->indexOfChain(r->id); at != SIZE_MAX) {
			rack->setOpen(at, !r->rail->collapsed());
		}
	}
}

bool ParallelRackExpanded::follow() {
	void* const held = host_.containerSurfaceOf(id_);
	pr::Rack* rack = pr::rackOf(held);
	if (rack != nullptr && rack->serial() != subscribedTo_) {
		// THE RACK'S CHAINS REPLACED UNDER THIS WINDOW (a preset picked in its own browser, an undo, a load): every column and the pane over a
		// chain let go AT ONCE, while the old chains stand, and are bound again by the next `follow`.
		subscribedTo_ = rack->serial();
		replaced_ = rack->subscribeChainsReplaced([this] {
			pane_->release();
			for (auto& r : rails_) {
				if (r != nullptr) {
					(void)r->rail->follow(nullptr);
				}
			}
		});
	}
	std::vector<uint16_t> ids;
	if (rack != nullptr) {
		for (size_t i = 0; i < rack->chainCount(); ++i) {
			ids.push_back(rack->chainIdAt(i));
		}
	}
	bool rebuilt = ids.size() != rails_.size();
	for (size_t i = 0; !rebuilt && i < ids.size(); ++i) {
		rebuilt = ids[i] != rails_[i]->id;
	}
	if (rebuilt) {
		// THE PANE'S WINDOW IS ON A RAIL'S MODULATION LIST AND OVER A CHAIN'S BRIDGE: it goes before any rail is dropped, and is built again after.
		pane_->release();
		// KEEP THE RAILS WHOSE CHAIN STAYED, by id; build the rest.
		std::vector<std::unique_ptr<ChainRail>> next;
		for (size_t i = 0; i < ids.size(); ++i) {
			std::unique_ptr<ChainRail> kept;
			for (auto& r : rails_) {
				if (r != nullptr && r->id == ids[i]) {
					kept = std::move(r);
					break;
				}
			}
			if (kept == nullptr) {
				kept = std::make_unique<ChainRail>();
				kept->id = ids[i];
				kept->caption = ""; // "in parallel, summed into the rack" is the level's tooltip now (backlog OC)
				kept->strip = std::make_unique<ChainStrip>(host_, id_, ids[i], "",
				                                           [this](uint16_t chainId) { removeChain(chainId); });
				kept->rail = std::make_unique<FxLaneRail>(FxLaneRail::Spec { "", kept->caption.c_str(), &chainItself },
				                                          registry_, notifier_);
				kept->strip->setModulation(mod_, history_);
				// A DOUBLE-CLICK ON THE STRIP'S CAPTION RENAMES ITS CHAIN (by id: the field follows the chain, not the place).
				kept->strip->setRenameHandler([this, strip = kept->strip.get()](uint16_t chainId, const visage::Bounds& anchor) {
					if (const size_t slot = model_->slotOfChainId(chainId); slot != kNoChain) {
						chainRename_->begin(slot, *strip, anchor);
					}
				});
				// THE OPEN RAIL DRAWS THE CHAIN'S NAME AS ITS HEADING: a double-click there renames, a right press opens the menu.
				kept->rail->setHeadingHandlers(
				    [this, rail = kept->rail.get(), chainId = ids[i]](const visage::Bounds& anchor) {
					    if (const size_t slot = model_->slotOfChainId(chainId); slot != kNoChain) {
						    chainRename_->showMenu(slot, *rail, anchor);
					    }
				    },
				    [this, rail = kept->rail.get(), chainId = ids[i]](const visage::Bounds& anchor) {
					    if (const size_t slot = model_->slotOfChainId(chainId); slot != kNoChain) {
						    chainRename_->begin(slot, *rail, anchor);
					    }
				    });
				// AND A RIGHT PRESS THERE OPENS ITS MENU (Rename...), as a tab's and a pad's do.
				kept->strip->setContextHandler([this, strip = kept->strip.get()](uint16_t chainId, const visage::Bounds& anchor) {
					if (const size_t slot = model_->slotOfChainId(chainId); slot != kNoChain) {
						chainRename_->showMenu(slot, *strip, anchor);
					}
				});
				kept->rail->setAccessory(kept->strip.get(), ChainStrip::kHeight);
				kept->rail->setAccessoryWhenCollapsed(true); // the header works while the chain is collapsed
				// BUILT AS THE RACK REMEMBERS IT (closed unless it was left open), BEFORE the hook is set so this write is
				// not heard as a hand's; a hand's toggle is written back to the rack (`keepOpenState`).
				kept->rail->setCollapsed(!rack->open(i));
				kept->rail->onRailWidthChanged([this]() {
					keepOpenState();
					resized();
					redraw();
					minimumSizeChanged();
				});
				if (mod_ != nullptr) {
					kept->rail->setOuterModulation(mod_, history_, [this] { return watchedDevice(); });
				}
				if (presenter_ != nullptr) {
					kept->rail->setPresentedOn(presenter_);
				}
				kept->rail->setNest(innerNest());
				// A CARD DROPPED ON ANOTHER CHAIN'S COLUMN MOVES THERE (backlog NQ).
				kept->rail->setOnDropOutside([this, from = ids[i]](std::span<const DeviceInstanceId> moved, visage::Point at) {
					return moveAcrossChains(from, moved, at);
				});
				addChild(kept->rail.get());
			}
			next.push_back(std::move(kept));
		}
		for (auto& r : rails_) {
			if (r != nullptr) {
				removeChild(r->rail.get());
			}
		}
		rails_ = std::move(next);
	}
	// THE RACK'S OPEN STATE WINS (a load changed it, or another window): every rail is brought to it. `applying_` keeps the
	// hook from writing a half-applied picture back over the rack.
	if (rack != nullptr) {
		applying_ = true;
		// IN TABS THE CHAIN IN FRONT IS ALWAYS OPEN and no rail has a collapse control (the chain's stored flag is the card's arrow's, and is
		// left as it is): a collapsed chain was a chain nobody could see how to add an effect to. Strips is as it always was.
		const bool tabs = model_ != nullptr && tabbedView(model_->view());
		for (size_t i = 0; i < rails_.size(); ++i) {
			const bool wantOpen = tabs || rack->open(i);
			rails_[i]->rail->setCollapsible(!tabs);
			if (rails_[i]->rail->collapsed() == wantOpen) {
				(void)rails_[i]->rail->setCollapsed(!wantOpen);
				rebuilt = true;
			}
		}
		applying_ = false;
	}
	// THE LABELS FOLLOW THE CHAIN — and the ROLE the mode gives it (§2a), or none.
	const auto mode = static_cast<pr::Mode>(std::lround(modeControl_->value()));
	const size_t roles = pr::roleCount(mode, rails_.size(), pr::bandCountOf(bandControls_[0]->value()));
	for (size_t i = 0; i < rails_.size(); ++i) {
		// THE CHAIN'S ONE LABEL (its name, else its first device's, else "Chain N"), then the ROLE the mode gives it (§2a), or none.
		std::string label = rack != nullptr ? rack->chainLabel(i) : std::to_string(i + 1);
		if ((pr::kRoleModes & pr::kModeBit(mode)) != 0) {
			label += i < roles ? " · " + ParallelRackCard::roleName(mode, i) : " (not used)";
		}
		rails_[i]->rail->setRailLabel(label);
		rails_[i]->strip->setLabel(label);
	}
	bool moved = rebuilt;
	for (size_t i = 0; i < rails_.size(); ++i) {
		moved = rails_[i]->rail->follow(rack != nullptr ? rack->chainAt(i) : nullptr) || moved;
		moved = rails_[i]->rail->updateCards() > 0 || moved;
		moved = rails_[i]->strip->refresh() || moved;
	}
	// THE PHANTOM RAILS, one per role no chain fills (§2a), rebuilt when that count or what they stand for changes.
	const auto phantomMode = static_cast<pr::Mode>(std::lround(modeControl_->value()));
	if (phantoms_.size() != missingRoles() || phantomMode != phantomMode_) {
		for (auto& phantom : phantoms_) {
			removeChild(phantom->rail.get());
		}
		phantoms_.clear();
		phantomMode_ = phantomMode;
		for (size_t m = 0; m < missingRoles(); ++m) {
			const size_t role = rails_.size() + m;
			// A ROLE NO CHAIN FILLS IS BUILT EXACTLY AS A REAL CHAIN'S RAIL IS (above): an `FxLaneRail` headed by a
			// `ChainStrip`, closed, the header working while closed — only with no chain behind it, so its strip makes
			// the chain on a first edit and its column is the empty chain's (+) and what passes (`setPhantom`).
			auto phantom = std::make_unique<PhantomRail>();
			phantom->label = std::to_string(role + 1) + " " + ParallelRackCard::roleName(phantomMode, role);
			phantom->passes = ParallelRackCard::placeholderText(phantomMode, role);
			phantom->onZone = [this, role] {
				// THE (+): the chain made, and its own add menu opened once its rail exists (the next tick).
				pendingMenuFor_ = materialiseRole(host_, id_, role, history_);
			};
			phantom->strip = std::make_unique<ChainStrip>(host_, id_, uint16_t { 0xFFFF }, "");
			phantom->strip->setMaterialise([this, role] { return materialiseRole(host_, id_, role, history_); });
			phantom->strip->setModulation(mod_, history_);
			phantom->strip->setLabel(phantom->label);
			phantom->strip->setInRail(true);
			phantom->rail = std::make_unique<FxLaneRail>(FxLaneRail::Spec { "", "", &chainItself }, registry_, notifier_);
			phantom->rail->setAccessory(phantom->strip.get(), ChainStrip::kHeight);
			phantom->rail->setAccessoryWhenCollapsed(true);
			phantom->rail->setCollapsed(true);
			phantom->rail->setRailLabel(phantom->label);
			phantom->rail->setPhantom(phantom->passes, phantom->onZone);
			phantom->rail->onRailWidthChanged([this]() {
				resized();
				redraw();
				minimumSizeChanged();
			});
			if (presenter_ != nullptr) {
				phantom->rail->setPresentedOn(presenter_);
			}
			addChild(phantom->rail.get());
			phantoms_.push_back(std::move(phantom));
		}
		rebuilt = true;
		moved = true;
	}
	for (auto& phantom : phantoms_) {
		moved = phantom->strip->refresh() || moved;
	}
	if (pendingMenuFor_ != 0xFFFF) {
		for (auto& r : rails_) {
			if (r->id == pendingMenuFor_ && r->rail->column() != nullptr) {
				pendingMenuFor_ = 0xFFFF;
				r->rail->column()->showAddMenu();
				break;
			}
		}
	}
	pin_.hold(held); // held now, the old let go — after the rails dropped what they built over it (backlog JX)
	if (rebuilt) {
		layOut();
	}
	return moved;
}

size_t ParallelRackExpanded::missingRoles() const {
	const auto mode = static_cast<pr::Mode>(std::lround(modeControl_->value()));
	const size_t roles = pr::roleCount(mode, rails_.size(), pr::bandCountOf(bandControls_[0]->value()));
	return (pr::kRoleModes & pr::kModeBit(mode)) != 0 && roles > rails_.size() ? roles - rails_.size() : 0;
}

float ParallelRackExpanded::railsTop() const {
	const auto mode = static_cast<pr::Mode>(std::lround(modeControl_->value()));
	if ((pr::kSpectrumModes & pr::kModeBit(mode)) != 0) {
		return RackSpectrumStrip::kWindowHeight + kPad;
	}
	if (mode == pr::Mode::Select || mode == pr::Mode::Blend) {
		return RackPositionBar::kHeight + kPad;
	}
	return 0.0f;
}

bool ParallelRackExpanded::showForMode() {
	const double mode = modeControl_->value();
	const double bands = bandControls_[0]->value();
	bool changed = false;
	if (mode != laidOutMode_ || bands != laidOutBands_) {
		layOut();
		changed = true;
	}
	// TABS IS OFFERED ONLY TO A RACK OF EIGHT CHAINS OR FEWER (hidden, not greyed); see the card's.
	const bool tabsOffered = model_->chainCount() <= kMaxRackTabs || model_->view() == RackViewKind::Tabs; // the pair offers List at any count
	if (!inspectorFolded() && (viewSeg_->isVisible() != tabsOffered || viewPair_->isVisible() == tabsOffered)) {
		viewSeg_->setVisible(tabsOffered);
		viewPair_->setVisible(!tabsOffered);
		changed = true;
	}
	const bool spectrumShown = (pr::kSpectrumModes & pr::kModeBit(static_cast<pr::Mode>(std::lround(mode)))) != 0;
	if (spectrum_->isVisible() != spectrumShown) {
		spectrum_->setVisible(spectrumShown);
		changed = true;
	}
	for (const auto& [frame, param] : gated_) {
		// THE INSPECTOR FOLDED hides its gated controls whatever the mode says (the position bar over the rails is not one of them).
		const bool inspectorOwned = std::find(inspectorFrames_.begin(), inspectorFrames_.end(), frame) != inspectorFrames_.end();
		const bool shown = pr::controlShown(param, mode, bands) && !(inspectorOwned && inspectorFolded());
		if (frame->isVisible() != shown) {
			frame->setVisible(shown);
			changed = true;
		}
	}
	return changed;
}

bool ParallelRackExpanded::updateContent() {
	const size_t before = rails_.size();
	const size_t missingBefore = missingRoles();
	if (macrosFolded() != foldsAppliedMacros_ || inspectorFolded() != foldsAppliedInspector_) {
		applyFolds(); // a load, or another editor, moved a fold
	}
	const bool shown = showForMode();
	// THE PANE'S CONTENT FIRST DROPPED IF ITS CHAIN WENT, then the rails follow, then the pane is bound again to what is in front.
	bool moved = validatePane();
	moved = follow() || shown || moved;
	const bool paneMoved = syncPane();
	const visage::Point paneNow = pane_->hosting() != RackWindowPane::Hosting::Nothing ? pane_->naturalSize() : visage::Point { 0.0f, 0.0f };
	if (paneMoved || paneNow.x != laidOutPane_.x || paneNow.y != laidOutPane_.y) {
		moved = true;
		minimumSizeChanged();
		layOut();
	}
	if (pane_->hosting() != RackWindowPane::Hosting::Nothing) {
		moved = pane_->update() > 0 || moved;
	}
	if (rails_.size() != before || missingRoles() != missingBefore || shown) {
		minimumSizeChanged();
		layOut();
	}
	moved = spectrum_->refresh() || moved;
	moved = (positionBar_->isVisible() && positionBar_->refresh()) || moved;
	moved = syncMacroSlots() || moved;
	// THE VIEW MOVED (or the chain in front): the rails are laid out again.
	if (static_cast<int>(model_->view()) != laidOutView_ || model_->viewedChain() != laidOutViewed_) {
		minimumSizeChanged();
		layOut();
		moved = true;
	}
	if (model_->view() != RackViewKind::Strips) {
		tabs_->refresh();
		if (model_->view() == RackViewKind::List) {
			moved = list_->refresh() || moved;
		}
		if (model_->view() == RackViewKind::Pads) {
			pads_->refresh();
			// THE STRIKE PULSE: one look a tick; no frame is told when nothing was struck.
			moved = pulse_.poll(*model_) > 0 || moved;
		}
	}
	moved = relayArm() || moved;
	if (moved) {
		redraw();
	}
	return moved;
}

pr::Rack* ParallelRackExpanded::rackNow() const { return pr::rackOf(host_.containerSurfaceOf(id_)); }

MacroMenuHooks ParallelRackExpanded::macroHooks(size_t n) {
	// **THE RACK'S OWN VERBS, asked afresh by id** (a surface handle is never kept), and only while this window lives (`alive_`):
	// the menu keeps a copy of these until a pick and is parented to the root frame. A reader answers the default, a writer does nothing.
	const std::weak_ptr<void> alive = alive_;
	return {
		.readBipolar =
		    [this, n, alive] {
			    const pr::Rack* rack = alive.expired() ? nullptr : rackNow();
			    return rack != nullptr && rack->macroBipolar(n);
		    },
		.writeBipolar =
		    [this, n, alive](bool bipolar) {
			    pr::Rack* rack = alive.expired() ? nullptr : rackNow();
			    if (rack != nullptr) {
				    rack->setMacroBipolar(n, bipolar);
				    macroSlots_[n]->knob().redraw();
				    macroSlots_[n]->chip().redraw();
			    }
		    },
		.readType =
		    [this, n, alive] {
			    const pr::Rack* rack = alive.expired() ? nullptr : rackNow();
			    return rack != nullptr ? rack->macroType(n) : device::MacroType::Knob;
		    },
		.writeType =
		    [this, n, alive](device::MacroType type) {
			    pr::Rack* rack = alive.expired() ? nullptr : rackNow();
			    if (rack != nullptr) {
				    rack->setMacroType(n, type);
			    }
		    },
		// THE SLOT'S OWN: `MacroSlot` answers Rename with its field.
		.beginRename = [] {},
	};
}

MacroSlot::Naming ParallelRackExpanded::macroNaming(size_t n) {
	const std::weak_ptr<void> alive = alive_;
	return {
		.name =
		    [this, n, alive] {
			    const pr::Rack* rack = alive.expired() ? nullptr : rackNow();
			    return rack != nullptr ? rack->macroName(n) : device::defaultMacroName(n);
		    },
		.renamed =
		    [this, n, alive] {
			    const pr::Rack* rack = alive.expired() ? nullptr : rackNow();
			    return rack != nullptr && rack->macroRenamed(n);
		    },
		.rename =
		    [this, n, alive](const std::string& name) {
			    if (alive.expired()) {
				    return;
			    }
			    // ONE UNDO STEP, by device id: the step outlives this window, so it holds the host and the id and asks for the rack on replay.
			    backplane::DeviceHost& host = host_;
			    const DeviceInstanceId id = id_;
			    const auto rackOfId = [&host, id] { return pr::rackOf(host.containerSurfaceOf(id)); };
			    (void)writeStoredAsEdit<std::string>(
			        history_,
			        [rackOfId, n] {
				        const pr::Rack* rack = rackOfId();
				        return rack != nullptr ? rack->macroName(n) : std::string {};
			        },
			        [rackOfId, n](const std::string& label) {
				        if (pr::Rack* rack = rackOfId()) {
					        rack->setMacroName(n, label);
				        }
			        },
			        name, "Rename");
		    },
	};
}

bool ParallelRackExpanded::syncMacroSlots() {
	bool moved = false;
	for (size_t n = 0; n < macroSlots_.size(); ++n) {
		moved = macroSlots_[n]->syncType() || moved;
		// THE CAPTION ON SHOW AGAINST WHAT IT SHOULD BE: the user's name, or the short one. A rename typed in the slot's own field
		// has already moved the caption; one made elsewhere (an undo, a load, the host) has not.
		const MacroSlot::Naming naming = macroNaming(n);
		const std::string want = naming.renamed() ? naming.name() : std::string(kCaptions[kTopKnobs + n]);
		if (want != macroSlots_[n]->caption()) {
			macroSlots_[n]->refresh();
			moved = true;
		}
	}
	return moved;
}

void ParallelRackExpanded::syncMacroArm() {
	// **A MACRO'S ARROW ARMS THE MACRO IN EVERY CHAIN.** The lend is by the hand's arm only (`armedLocally`: the host's own arm does
	// not count), and a rail the hand armed for something of its own is left alone.
	const bool want = macroMod_.armedLocally();
	for (auto& r : rails_) {
		ModState& state = r->rail->modState();
		r->macroRelay.setTarget(&state);
		if (want && (!state.armedLocally() || r->macroRelay.relayed())) {
			(void)r->macroRelay.relay(macroMod_.source());
		}
		else {
			(void)r->macroRelay.release();
		}
	}
}

bool ParallelRackExpanded::relayArm() {
	syncMacroArm();
	// **THE GLOW OUT THE BACK OF A RACK** (backlog NZ (b); a Chain's panel does the same, `ChainExpanded::relayArm`). Each
	// rail's state is its own, so the first one a hand armed LOCALLY names the source; its inner ids are unique across the
	// Rack's chains, which is how the Rack's table knows which chain holds it. The host answers nobody for what does not
	// export, and the relay then lets go.
	device::Source want = device::Source::nobody();
	if (mod_ != nullptr) {
		for (const auto& r : rails_) {
			const ModState& inner = r->rail->modulation();
			if (inner.armedLocally()) {
				want = host_.containerExport(id_, inner.source());
				break;
			}
		}
	}
	return relay_.relay(want);
}

void ParallelRackExpanded::modulationBound(ModState* mod, EditHistory* history) {
	mod_ = mod;
	history_ = history;
	if (padAdd_ != nullptr) {
		padAdd_->setHistory(history);
	}
	if (list_ != nullptr) {
		list_->setModulation(mod, history);
	}
	if (chainRename_ != nullptr) {
		chainRename_->setHistory(history);
	}
	relay_.setTarget(mod);
	for (auto& c : controls_) {
		c->setModulation(mod, history);
	}
	modeControl_->setModulation(mod, history);
	positionControl_->setModulation(mod, history);
	unusedControl_->setModulation(mod, history);
	position_->setModulation(mod, history);
	for (auto& c : bandControls_) {
		c->setModulation(mod, history);
	}
	for (auto& k : cuts_) {
		k->setModulation(mod, history);
	}
	for (auto& r : rails_) {
		r->strip->setModulation(mod, history);
	}
	for (auto& k : knobs_) {
		k->setModulation(mod, history);
	}
	for (auto& s : macroSlots_) {
		s->setModulation(mod, history);
	}
	for (auto& r : rails_) {
		r->rail->setOuterModulation(mod, history, [this] { return watchedDevice(); });
	}
	macroMod_.setParent(mod);
}

void ParallelRackExpanded::setPresentedOn(SurfaceLayers* host) {
	presenter_ = host;
	for (auto& r : rails_) {
		r->rail->setPresentedOn(host);
	}
	if (pane_ != nullptr) {
		pane_->setPresentedOn(host);
	}
}

void ParallelRackExpanded::onInnerNestChanged() {
	// ONE NEST FOR EVERY RAIL: the nest is the holder's, and the chains share one instance counter (gap G3).
	for (auto& r : rails_) {
		r->rail->setNest(innerNest());
	}
	// WHAT THE PANE HOLDS WAS STAMPED WITH THE OLD NEST: it is built again under the new one.
	if (pane_ != nullptr) {
		pane_->release();
	}
}

void ParallelRackExpanded::draw(visage::Canvas& canvas) {
	// THE H-LINE BETWEEN MIX/GAIN AND THE MACROS.
	const visage::Bounds line = dividerBounds();
	canvas.setColor(withAlpha(kLine, 0.6f));
	canvas.fill(line.x(), line.y(), line.width(), line.height());

	const visage::Bounds add = addBounds();
	if (add.width() <= 0.0f) {
		return; // hidden in a role mode
	}
	canvas.setColor(withAlpha(kLine, 0.6f));
	canvas.roundedRectangle(add.x(), add.y(), add.width(), add.height(), 4.0f);
	canvas.setColor(kMuted);
	canvas.text(add.width() < kAddWidth ? "+" : "+ Chain", font(kKnobLabelSize), visage::Font::kCenter, add.x(), add.y(), add.width(), add.height());
}

} // namespace feathers::ui
