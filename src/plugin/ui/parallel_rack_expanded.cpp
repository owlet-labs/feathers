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
constexpr float kModeRow = kCellHeight + ParallelRackExpanded::kPad;

/// EACH RAIL IS HANDED ITS OWN CHAIN as the handle, so its cast is the identity.
rack::InnerChain* chainItself(void* surface) { return static_cast<rack::InnerChain*>(surface); }

} // namespace

ParallelRackExpanded::ParallelRackExpanded(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id,
                                           Notifier* notifier)
    : host_(host), registry_(registry), id_(id), notifier_(notifier), pin_(host) {
	presets_ = makeRackDevicePresets(host, id, notifier);
	for (size_t i = 0; i < kKnobs.size(); ++i) {
		controls_.push_back(
		    std::make_unique<DeviceControl>(host, id, pr::kDescriptor, static_cast<size_t>(kKnobs[i])));
		auto knob = std::make_unique<Knob>(*controls_.back(), kCaptions[i], notifier);
		knob->setFaceDiameter(Knob::kEngineRailDiameter);
		addChild(knob.get());
		knobs_.push_back(std::move(knob));
	}
	// STAGE 2: THE MODE (a dropdown) AND THE POSITION, every control the card offers (window ⊇ card).
	modeControl_ = std::make_unique<DeviceControl>(host, id, pr::kDescriptor, static_cast<size_t>(pr::Param::Mode));
	positionControl_ = std::make_unique<DeviceControl>(host, id, pr::kDescriptor, static_cast<size_t>(pr::Param::Position));
	modeBox_ = std::make_unique<ValueBox>(*modeControl_, "Mode");
	addChild(modeBox_.get());
	position_ = std::make_unique<Knob>(*positionControl_, "Position", notifier);
	position_->setFaceDiameter(Knob::kEngineRailDiameter);
	addChild(position_.get());
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
	slope_ = std::make_unique<SegmentedControl>(*own(pr::Param::Slope), registry, pr::kSlopeFaceNames, notifier);
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
	gated_ = { { keyTrack_.get(), pr::Param::KeyTrack }, { position_.get(), pr::Param::Position },   { unused_.get(), pr::Param::Unused },
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
	(void)showForMode();
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
		              std::max(addSlotBounds().bottom(), bandsBottom()) + kPad };
	for (const auto& r : rails_) {
		// A CLOSED RAIL IS A CHANNEL STRIP NOW (backlog OC), and needs its column's height; an open one its floor.
		p.y = std::max(p.y, r->rail->collapsed() ? Rail::kCollapsedChromeHeight + ChainStrip::kColumnMinimumHeight
		                                         : r->rail->openMinimumHeight());
	}
	return p;
}

float ParallelRackExpanded::dividerY() const { return kPad + kModeRow + kCellHeight + kPad * 0.5f; }

float ParallelRackExpanded::knobY(size_t i) const {
	if (i < kTopKnobs) {
		return kPad + kModeRow;
	}
	const auto row = static_cast<float>((i - kTopKnobs) / kPerRow);
	return dividerY() + kPad * 0.5f + row * (kCellHeight + kPad);
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
	return kColumnWidth + 2.0f * kPad + (bandsColumn() ? kColumnWidth + kPad : 0.0f);
}

float ParallelRackExpanded::bandsBottom() const {
	// The second column's foot: the count, the filtering, the slope, then the four corners one under another.
	return bandsTop() + 2.0f * (ValueBox::kHeight + kPad * 0.5f) + 2.0f * kSegmentRow + 4.0f * (kCellHeight + kPad * 0.5f);
}

float ParallelRackExpanded::bandsTop() const { return kPad + kModeRow; }


visage::Bounds ParallelRackExpanded::addSlotBounds() const {
	// SMALL, UNDER THE MACROS: the column is as wide as four macros and no wider.
	return { kPad, dividerY() + kPad * 0.5f + kMacroRows * (kCellHeight + kPad), kAddWidth, kAddHeight };
}

bool ParallelRackExpanded::addShown() const {
	const auto mode = static_cast<pr::Mode>(std::lround(modeControl_->value()));
	return (pr::kRoleModes & pr::kModeBit(mode)) == 0;
}

visage::Bounds ParallelRackExpanded::addBounds() const { return addShown() ? addSlotBounds() : visage::Bounds {}; }

visage::Bounds ParallelRackExpanded::dividerBounds() const { return { kPad, dividerY(), kColumnWidth, 1.0f }; }

void ParallelRackExpanded::layOut() {
	const float cell = kColumnWidth / static_cast<float>(kPerRow);
	// THE MODE ROW: the dropdown at the top of the left cell, the position's knob in the right one.
	modeBox_->setBounds(kPad, kPad, cell, ValueBox::kHeight);
	position_->setBounds(kPad + cell, kPad, cell, kCellHeight);
	// UNUSED UNDER THE MODE, THE COLUMN'S FULL WIDTH: its two words need the room (they ran together in one cell), and it
	// shows only in the role modes, where the position's knob does not.
	unused_->setBounds(kPad, kPad + kCellHeight - SwitchChip::kHeight, kColumnWidth, SwitchChip::kHeight);
	// THE BANDS COLUMN, beside the macros: count and filtering stacked, the slope, then the corners A VERTICAL STACK, one per
	// row, lowest first — the order they stand in on the spectrum.
	const float bx = kColumnWidth + 2.0f * kPad;
	float y = bandsTop();
	bandsBox_->setBounds(bx, y, kColumnWidth, ValueBox::kHeight);
	y += ValueBox::kHeight + kPad * 0.5f;
	filteringBox_->setBounds(bx, y, kColumnWidth, ValueBox::kHeight);
	y += ValueBox::kHeight + kPad * 0.5f;
	slope_->setBounds(bx, y, kColumnWidth, SwitchChip::kHeight);
	y += kSegmentRow;
	keyTrack_->setBounds(bx, y, kColumnWidth, SwitchChip::kHeight);
	y += kSegmentRow;
	for (size_t k = 0; k < 4; ++k) {
		cuts_[k]->setBounds(bx, y + static_cast<float>(k) * (kCellHeight + kPad * 0.5f), kColumnWidth, kCellHeight);
	}
	for (size_t i = 0; i < knobs_.size(); ++i) {
		const size_t col = i < kTopKnobs ? i : (i - kTopKnobs) % kPerRow;
		knobs_[i]->setBounds(kPad + static_cast<float>(col) * cell, knobY(i), cell, kCellHeight);
	}
	// THE RAILS LEFT TO RIGHT IN CHAIN ORDER, after the column — under the strip or the bar when the mode has one.
	const float top = railsTop();
	const float x0 = railsLeft();
	float x = x0;
	for (auto& r : rails_) {
		const float wide = r->rail->railWidth();
		r->rail->setBounds(x, top, wide, std::max(0.0f, static_cast<float>(height()) - top));
		x += wide;
	}
	const float across = std::max(x - x0 + phantomsWidth(), kPlaceholderWidth);
	spectrum_->setBounds(x0, kPad * 0.5f, across, RackSpectrumStrip::kWindowHeight);
	positionBar_->setBounds(x0, kPad * 0.5f, across, RackPositionBar::kHeight);
	// A ROLE NO CHAIN FILLS (§2a): a PHANTOM RAIL after the last real one, an empty chain's face.
	for (auto& phantom : phantoms_) {
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
		for (size_t i = 0; i < rails_.size(); ++i) {
			if (rails_[i]->rail->collapsed() == rack->open(i)) {
				(void)rails_[i]->rail->setCollapsed(!rack->open(i));
				rebuilt = true;
			}
		}
		applying_ = false;
	}
	// THE LABELS FOLLOW THE POSITION: "2" is the second chain now — and the ROLE the mode gives it (§2a), or none.
	const auto mode = static_cast<pr::Mode>(std::lround(modeControl_->value()));
	const size_t roles = pr::roleCount(mode, rails_.size(), pr::bandCountOf(bandControls_[0]->value()));
	for (size_t i = 0; i < rails_.size(); ++i) {
		std::string label = std::to_string(i + 1);
		if ((pr::kRoleModes & pr::kModeBit(mode)) != 0) {
			label += i < roles ? " " + ParallelRackCard::roleName(mode, i) : " (not used)";
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
	const bool spectrumShown = (pr::kSpectrumModes & pr::kModeBit(static_cast<pr::Mode>(std::lround(mode)))) != 0;
	if (spectrum_->isVisible() != spectrumShown) {
		spectrum_->setVisible(spectrumShown);
		changed = true;
	}
	for (const auto& [frame, param] : gated_) {
		const bool shown = pr::controlShown(param, mode, bands);
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
	const bool shown = showForMode();
	bool moved = follow() || shown;
	if (rails_.size() != before || missingRoles() != missingBefore || shown) {
		minimumSizeChanged();
		layOut();
	}
	moved = spectrum_->refresh() || moved;
	moved = (positionBar_->isVisible() && positionBar_->refresh()) || moved;
	moved = relayArm() || moved;
	if (moved) {
		redraw();
	}
	return moved;
}

bool ParallelRackExpanded::relayArm() {
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
	for (auto& r : rails_) {
		r->rail->setOuterModulation(mod, history, [this] { return watchedDevice(); });
	}
}

void ParallelRackExpanded::setPresentedOn(SurfaceLayers* host) {
	presenter_ = host;
	for (auto& r : rails_) {
		r->rail->setPresentedOn(host);
	}
}

void ParallelRackExpanded::onInnerNestChanged() {
	// ONE NEST FOR EVERY RAIL: the nest is the holder's, and the chains share one instance counter (gap G3).
	for (auto& r : rails_) {
		r->rail->setNest(innerNest());
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
	canvas.text("+ Chain", font(kKnobLabelSize), visage::Font::kCenter, add.x(), add.y(), add.width(), add.height());
}

} // namespace feathers::ui
