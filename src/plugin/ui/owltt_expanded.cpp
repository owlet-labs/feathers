/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even
 * the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
 * Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

#include "plugin/ui/widget_registry.h"
#include "plugin/ui/menu_anchor.h"
#include "plugin/ui/owltt_expanded.h"

#include "plugin/gui_bridge.h"
#include "plugin/ui/diagnostic.h"
#include "plugin/ui/theme.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <array>

namespace feathers::ui {

namespace dtt = feathers::fx::owltt;


std::span<const OwlttExpanded::KnobGroup> OwlttExpanded::groups() {
	// THE COMPLETE DEVICE. A standing rule for every device: the COMPLETE device, with full controls,
	// is on the expanded view, so nobody has to toggle between card and full view to edit every param.
	//
	// So this is not "the controls the card lacks" any more — it is all of them, the card's three
	// included. The card is a SUBSET for glancing at; the panel is the instrument. A control that
	// appears only on the card would force exactly the toggling the directive forbids.
	//
	// THE PER-BAND CONTROLS ARE THE GRAPH'S, AND ONLY THE GRAPH'S. Knobs duplicating the graph in the
	// popout were removed; the graph alone is enough.
	//
	// Twelve knobs went: low, mid and high threshold, bandwidth, ratio and level. Every one of them
	// is a bar or a strip on the plot above, so the panel was offering each twice — and two controls
	// over one value is not redundancy this codebase can afford, it is two places for a hand to form
	// a habit and one of them to later disagree.
	//
	// THE DIRECTIVE STILL HOLDS AND IS WHY THIS IS SAFE. "The COMPLETE plugin with full controls will
	// be on the expanded view" is about REACHABILITY, not about knobs: the twelve are reachable on
	// this surface, through the picture that is the better control for them. The reachability case
	// asserts the panel covers every declared control and it still passes, because `addressedParams`
	// counts what the graph addresses as well as what the knobs do.
	// MASTER — EVERYTHING THAT IS NOT ONE BAND, in one group: Tone and Output belong with master.
	// The globals lead because `owltt.cpp:648-663` computes `band + (global - 0.5)`, so
	// those four are offsets summed into the twelve beside them; timing, tone and staging follow
	// because none of them is a property of a band either. One caption instead of four is also what
	// buys the vertical room the graph now takes.
	static constexpr KnobEntry kMaster[] = {
		{ dtt::Param::Threshold, "Threshold" },
		{ dtt::Param::Bandwidth, "Bandwidth" },
		{ dtt::Param::Ratio, "Ratio" },
		{ dtt::Param::UpDownSkew, "Up/Down" },
		{ dtt::Param::Attack, "Attack" },
		{ dtt::Param::Release, "Release" },
		// HOW EACH BAND'S TWO CHANNELS SHARE A DETECTOR (backlog LQ) — beside the timing, because it too is about
		// how the detectors listen. The L/R-M/S chip that says what the two channels ARE is in the modal row.
		{ dtt::Param::StereoLink, "Link" },
		// HOW WIDE EACH BAND SENDS (backlog LR): the global Width the card carries, then each band's own, all scaling the
		// Character zone's width (100% is the zone's).
		{ dtt::Param::Width, "Width" },
		// **THE THREE BAND WIDTHS ARE ONE SLIDER BANK (backlog NK)**: a set of three, one per band, in one knob's cell —
		// three cells given back to the grid, and the three read top to bottom as low, mid, high.
		{ dtt::Param::LowWidth, "Band Width", { dtt::Param::LowWidth, dtt::Param::MidWidth, dtt::Param::HighWidth } },
		// THE BOUNDARIES, and they are meant to stop being circles: a frequency split is a POSITION,
		// and these become draggable edges on the graph above. Knobs until then, because
		// reachable-as-a-knob beats unreachable-while-waiting.
		{ dtt::Param::LowCrossover, "Low Xover" },
		{ dtt::Param::HighCrossover, "High Xover" },
		// THE EIGHT-ZONE TONAL PAIR, which are not compression at all — and are the WRONG WIDGET, as
		// noted in review. A circle sweeping 1..8 zones says nothing about what a zone IS. Backlog 23.
		{ dtt::Param::Character, "Character" },
		{ dtt::Param::Vibe, "Vibe" },
		{ dtt::Param::OutputGain, "Output" },
		{ dtt::Param::Blend, "Blend" },
		// THE SOFT CLIPPER'S SWITCH (backlog EL) — Off/On; the firmware toggled it with a click on its
		// Mode encoder. Beside Output and Blend because it is the staging's last word on the sum.
		{ dtt::Param::SoftClip, "Clip" },
	};
	static constexpr KnobGroup kGroups[] = {
		{ "MASTER", kMaster, 0 },
	};
	return kGroups;
}

OwlttExpanded::OwlttExpanded(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id,
                           Notifier* notifier)
    : host_(host), device_(id) {
	// REGISTRY UNUSED SINCE `Knob` STOPPED TAKING ONE — it was keyed by clap_id and this card's
	// controls are a device's, which have none. The PARAMETER is still in the signature because
	// removing it touches every card, `fx_lane` and the fixtures; that pass is pending. Discarded
	// explicitly so the compiler cannot mistake a deferred cleanup for a forgotten use.
	(void)registry;
	// THE PANEL'S OWN GRAPH, over the same device through the same host — the same class the card
	// holds, at twice the height. Nothing is shared between the two instances and nothing needs to
	// be: the meters are a READ off the bridge, so two live graphs cannot fight over a connector.
	graph_ = std::make_unique<OwlttGraph>(host_, device_, notifier);
	// THE CROSSOVERS ARE DRAGGABLE HERE AND NOT ON THE CARD — on the panel's graph, and the card is
	// the reason to take that literally: at 160px its two bar hit-bands
	// already claim 20% of the plot, and a reserved lane plus two more handles would re-crowd exactly
	// what making the graph taller just fixed.
	graph_->setCrossoversDraggable(true);
	addChild(graph_.get());
	// L/R OR M/S (backlog LQ) — two states, so a chip, printing the live choice.
	stereo_ = std::make_unique<SwitchChip>(control(dtt::Param::StereoMode), "Stereo");
	addChild(stereo_.get());
	// THE DRIVE (backlog NB), a bar left of the graph as on the card.
	drive_ = std::make_unique<VerticalFader>(control(dtt::Param::Drive), "Drive");
	if (notifier != nullptr) {
		drive_->setAnnouncer([notifier](const std::string& line) { notifier->say(line); });
	}
	addChild(drive_.get());
	// THE LOOKAHEAD (backlog ND) — a choice of latencies, so a box with its menu (every choice seen before it applies),
	// in the modal row with the other two choices about how the whole device is built.
	lookahead_ = std::make_unique<ValueBox>(control(dtt::Param::Lookahead), "Lookahead");
	lookahead_->setCompact(true);
	addChild(lookahead_.get());
	// KEY TRACK — two states, a chip, in the modal row: the played note places the crossovers.
	keyTrack_ = std::make_unique<SwitchChip>(control(dtt::Param::KeyTrack), "Key Track");
	addChild(keyTrack_.get());

	const auto addStack = [&](const KnobEntry& entry) {
		static constexpr const char* kBandCaptions[] = { "Low", "Mid", "High" };
		std::array<PillStack::Spec, PillStack::kCount> specs {};
		for (size_t k = 0; k < PillStack::kCount; ++k) {
			specs[k] = { &control(entry.bands[k]), nullptr, kBandCaptions[k] };
		}
		auto stack = std::make_unique<PillStack>(std::move(specs), entry.label);
		addChild(stack.get());
		cells_.push_back(stack.get());
		stackParams_.push_back(entry.bands);
		stacks_.push_back(std::move(stack));
	};

	const auto addKnob = [&](dtt::Param param, const char* label) {
		// ZONE KNOBS for Character and Vibe — these controls are divided into zones, and the zone
		// index and position within the zone determine the compressor's tonal character. A regular
		// knob would hide this structure; a zone knob shows it.
		// **THE TWO ZONED CONTROLS ARE NO LONGER KNOBS.** They were a knob plus a phase box, built on
		// the belief that a zone control is bounded; the premise was corrected on 2026-08-29 — most of
		// the firmware's zone knobs allow "just keep turning" past the 8th zone, which keeps driving the
		// phase, and a zone name is just a name assigned to a region of an otherwise infinite space.
		// One infinite line, one unit, as many granularities as it has
		// digits. See `zone_coord_cell.h` for why that makes a dial the wrong shape.
		//
		// `registry` AND `notifier` GO UNUSED HERE and that is not an omission: a coordinate cell is a
		// `ValueBox` and a `ZoneSelect`, and neither takes them. The knob road below still takes both.
		const auto addCoord = [&](dtt::Param coordinate, dtt::Param mode, std::span<const char* const> modeNames,
		                          std::span<const dtt::ZoneName> descriptions) {
			// **A MODE CELL, NOT A REGION DROPDOWN (backlog IW: 2026-09-23).** The dropdown is a mode selector, not a
			// phase-region selector; for these it is the modes, then Free, and since the modes' implementations are
			// unbounded the in-mode phase stays unbounded. The dropdown lists the eight behaviours, then Free; the box is the one unbounded
			// value — in a mode its fraction is how far into the behaviour and its whole part the lap; in Free, the
			// rolling coordinate. See `zoneModeCoordinate`. The box is live in every mode, so it never goes idle.
			std::vector<ZoneSelect::ZoneName> names;
			for (size_t i = 0; i < modeNames.size(); ++i) {
				names.push_back({ modeNames[i], i < descriptions.size() ? descriptions[i].description
				                                                         : "Rolls through all eight as the value turns" });
			}
			auto cell = std::make_unique<ModeValueCell>(control(mode), label, std::move(names),
			                                            [](size_t) { return true; });
			cell->addFollower(static_cast<size_t>(coordinate), std::make_unique<ValueBox>(control(coordinate), ""));
			cell->setFaceDiameter(kKnobDiameter);
			cell->refresh();
			addChild(cell.get());
			cells_.push_back(cell.get());
			modeCells_.push_back(std::move(cell));
			coordParams_.push_back(coordinate);
			coordParams_.push_back(mode);
		};
		if (param == dtt::Param::Character) {
			addCoord(dtt::Param::Character, dtt::Param::CharacterMode, dtt::kCharacterModeNames, dtt::kCharacterZoneNames);
			return;
		}
		if (param == dtt::Param::Vibe) {
			addCoord(dtt::Param::Vibe, dtt::Param::VibeMode, dtt::kVibeModeNames, dtt::kVibeZoneNames);
			return;
		}
		// THE SOFT CLIPPER IS OFF/ON, SO A CHIP, not a dial: it names itself and shows both states at a glance.
		if (param == dtt::Param::SoftClip) {
			auto chip = std::make_unique<SwitchChip>(control(param), label);
			addChild(chip.get());
			cells_.push_back(chip.get());
			extraReached_.push_back(param);
			chips_.push_back(std::move(chip));
			return;
		}
		auto knob = std::make_unique<Knob>(control(param), label, notifier);
		knob->setFaceDiameter(kKnobDiameter);
		addChild(knob.get());
		cells_.push_back(knob.get());
		knobs_.push_back(std::move(knob));
		// RECORDED AS THE KNOB IS BUILT, in the one place that knows which control it was built for,
		// so the reachability invariant reads this rather than a list kept beside it.
		knobParams_.push_back(param);
	};

	// BUILT FROM THE DECLARED GROUPS, in the order they are drawn — so `knobs_` and the layout walk
	// one list and cannot disagree about which knob is where.
	for (const KnobGroup& group : groups()) {
		for (const KnobEntry& entry : group.knobs) {
			if (entry.isStack()) {
				addStack(entry);
			}
			else {
				addKnob(entry.param, entry.label);
			}
		}
	}

	// THE MODE PICKER IS THIS BODY'S ONE HAND-DRAWN CONTROL. The enable square and its caption that
	// shared the row are the window header's chip now (2026-09-26), one for every device.
	onMouseDown() = [this](const visage::MouseEvent& e) {
		if (modeBounds().contains(e.position)) {
			showModeMenu();
		}
	};
}

DeviceControl& OwlttExpanded::control(dtt::Param param) const {
	const size_t index = static_cast<size_t>(param);
	if (controls_[index] == nullptr) {
		// THE GENERIC ROAD — a descriptor and a control index, never a device type. The same road the
		// card takes to the same rows, so the two surfaces cannot disagree about a value.
		controls_[index] = std::make_unique<DeviceControl>(host_, device_, dtt::kDescriptor, index);
	}
	return *controls_[index];
}

bool OwlttExpanded::updateContent() {
	bool changed = graph_ != nullptr && graph_->update();
	// A BANK'S READOUT FOLLOWS A VALUE MOVED WITH NO HAND ON IT (automation, undo) — `PillStack::refresh`.
	for (const auto& stack : stacks_) {
		changed = stack->refresh() || changed;
	}
	return changed;
}

void OwlttExpanded::modulationBound(ModState* mod, EditHistory* history) {
	// THE PLOT'S HANDLES ARE DESTINATIONS TOO, exactly as the knobs are — the same five controls
	// reached two ways. A picture whose handles could not be armed would be a set of controls that
	// silently is not one.
	if (graph_ != nullptr) {
		graph_->setModulation(mod, history);
	}
	for (auto& knob : knobs_) {
		knob->setModulation(mod, history);
	}
	// AND EVERY ADAPTER, INCLUDING THE ONES NO KNOB HOLDS.
	//
	// HARM SHIPPED WITHOUT THIS LINE AND COULD NOT BE MODULATED AT ALL: a Knob forwards to the
	// control it was given, so the knobs' adapters were covered — but `DeviceControl::setAmount` and
	// `DeviceControl::display` both ask the adapter's OWN `mod_`, and every adapter built by `control()`
	// had none. Every destination reported itself a destination and none of them was ever armable.
	// Copied deliberately rather than rediscovered.
	for (size_t i = 0; i < controls_.size(); ++i) {
		control(static_cast<dtt::Param>(i)).setModulation(mod, history);
	}
}

std::string OwlttExpanded::title() const {
	// THE DEVICE'S OWN NAME, from its traits — the same authority the card's head asks, so the panel
	// and the card cannot disagree about which device is open.
	return dtt::kTraits.name;
}

float OwlttExpanded::modalBottom() const {
	return kPad + kSwitchHeight;
}

/// HOW MANY CELLS EACH COLUMN IS WIDE. The three bands are two cells — four knobs in a 2x2 — so they
/// sit as blocks UNDER the graph's three bands rather than as long rows beside it. MASTER is four,
/// because twelve knobs in threes would be four rows and taller than the bands beside it.
///
/// FIXED WIDTHS, NOT TRACKING THE CROSSOVERS. The graph's bands move: they are split by FREQUENCY on
/// a log axis, so aligning a knob column to a band edge would slide controls sideways under the
/// pointer every time a crossover was dragged. `harm_expanded.h` states the rule and this is the case
/// it was written for — a row that re-spaces itself moves controls for no reason a player can see.
/// The columns read as "under the low band" without pretending to be welded to it.
/// SIX CELLS IN THE ONE COLUMN THAT IS LEFT, so MASTER's twelve knobs are two rows of six rather
/// than three of four — a squarer block under a wider picture, and one row less of height for the
/// graph to give back.
constexpr std::array<int, 4> kColumnCells { 6, 4, 4, 4 };

float OwlttExpanded::columnWidth(int column) const {
	const auto at = static_cast<size_t>(std::clamp(column, 0, 3));
	return static_cast<float>(kColumnCells[at]) * kCellWidth;
}

float OwlttExpanded::columnX(int column) const {
	float x = kPad;
	for (int c = 0; c < std::clamp(column, 0, 3); ++c) {
		x += columnWidth(c) + kColumnGap;
	}
	return x;
}

float OwlttExpanded::bodyWidth() const {
	// ONLY THE COLUMNS SOMETHING IS IN. This read `columnX(3) + columnWidth(3)` — always four columns,
	// whether or not anything was in them — so when the three per-band groups left, the panel went on
	// asking for their width and got 938px of empty canvas. Derived from the groups now, so a group
	// leaving takes its column with it and the next one to leave cannot repeat this.
	int last = 0;
	for (const KnobGroup& group : groups()) {
		last = std::max(last, std::clamp(group.column, 0, 3));
	}
	return columnX(last) + columnWidth(last) - kPad;
}

visage::Bounds OwlttExpanded::graphBounds() const {
	// FULL WIDTH, ABOVE THE COLUMNS. The picture is why the panel was opened and it gets the whole
	// span; the three band blocks sit beneath their own thirds of it.
	// RIGHT OF THE DRIVE BAR (backlog NB), which takes its width out of the picture's.
	const float left = kPad + kDriveWidth + kDriveGap;
	return { left, modalBottom() + kGroupGap, std::max(0.0f, width() - kPad - left), kGraphHeight };
}

visage::Bounds OwlttExpanded::driveBounds() const {
	return { kPad, modalBottom() + kGroupGap, kDriveWidth, kGraphHeight };
}

visage::Bounds OwlttExpanded::modeBounds() const {
	// THE CROSSOVER TYPE, IN THE MODAL ROW, because it is a verb like the enable rather than a value:
	// it decides which filter topology the whole device is built from, and every band changes shape
	// when it moves. Right-aligned, the arrangement the card's head and HARM's panel already use for
	// their second occupant.
	return { std::max(kPad, width() - kPad - kModeWidth), kPad, kModeWidth, kSwitchHeight };
}

visage::Bounds OwlttExpanded::stereoBounds() const {
	const visage::Bounds mode = modeBounds();
	return { std::max(kPad, mode.x() - kRowGap - kChipWidth), kPad + (kSwitchHeight - SwitchChip::kHeight) * 0.5f,
		     kChipWidth, SwitchChip::kHeight };
}

visage::Bounds OwlttExpanded::lookaheadBounds() const {
	const visage::Bounds chip = stereoBounds();
	return { std::max(kPad, chip.x() - kRowGap - kLookaheadWidth), kPad + (kSwitchHeight - ValueBox::kCompactHeight) * 0.5f,
		     kLookaheadWidth, ValueBox::kCompactHeight };
}

visage::Bounds OwlttExpanded::keyTrackBounds() const {
	const visage::Bounds box = lookaheadBounds();
	return { std::max(kPad, box.x() - kRowGap - kKeyTrackWidth), kPad + (kSwitchHeight - SwitchChip::kHeight) * 0.5f,
		     kKeyTrackWidth, SwitchChip::kHeight };
}

std::string OwlttExpanded::modeText() const {
	// THE CARD'S OWN WORDS, from the same `kModeNames` its folded head prints, so the two surfaces
	// cannot name one topology differently.
	const auto at = static_cast<int32_t>(std::lround(control(dtt::Param::Mode).value()));
	const int32_t index = at - 1; // the parameter runs 1..10; the names run 0..9
	if (index < 0 || static_cast<size_t>(index) >= std::size(dtt::kModeNames)) {
		return {};
	}
	return dtt::kModeNames[static_cast<size_t>(index)];
}

void OwlttExpanded::chooseMode(size_t index) {
	if (index >= std::size(dtt::kModeNames)) {
		return;
	}
	DeviceControl& c = control(dtt::Param::Mode);
	// BRACKETED AS ONE EDIT, like every other value write on this surface — a choice is still a
	// parameter change and a host recording automation wants one step, not a bare set.
	c.beginGesture();
	c.setValue(static_cast<double>(index) + 1.0); // names are 0-based, the parameter is 1-based
	c.endGesture();
	redraw();
}

void OwlttExpanded::showModeMenu() {
	visage::PopupMenu menu;
	for (size_t i = 0; i < std::size(dtt::kModeNames); ++i) {
		menu.addOption(static_cast<int>(i), dtt::kModeNames[i]);
	}
	menu.onSelection() = [this](int id) {
		if (id >= 0) {
			chooseMode(static_cast<size_t>(id));
		}
	};
	const visage::Bounds box = modeBounds();
	showMenuAt(std::move(menu), *this, box);
}

visage::Point OwlttExpanded::maximumSize() const {
	return { kNoCap, minimumSize().y + kPad };
}

visage::Point OwlttExpanded::minimumSize() const {
	// SIZED FOR THE TALLEST IN THE GRID, which is now the KNOB rather than the zoned control. The
	// coordinate box that replaced the knob-plus-box assembly is about half its height — a caption, a
	// row of numbers and a name, against a 38px face plus a caption plus a readout plus a box — so
	// the row goes back to a plain knob column and the two boxes sit inside it with room to spare.
	const float column = Knob::columnHeight(kKnobDiameter);
	// THE TALLEST COLUMN, NOT THE SUM — summed, the 24 knobs are ~900px and the overlay CLAMPS rather
	// than scrolling (`expanded_overlay.cpp:169`), so the bottom groups would be silently cut off.
	float tallest = 0.0f;
	for (int col = 0; col < 4; ++col) {
		float height = 0.0f;
		for (const KnobGroup& group : groups()) {
			if (group.column != col) {
				continue;
			}
			height += kGroupGap + kCaptionHeight + kRowGap;
			const auto cells = static_cast<size_t>(kColumnCells[static_cast<size_t>(col)]);
			const size_t rows = group.knobs.empty() ? 0 : (group.knobs.size() + cells - 1) / cells;
			height += static_cast<float>(rows) * column
			          + (rows > 1 ? static_cast<float>(rows - 1) * kRowGap : 0.0f);
		}
		tallest = std::max(tallest, height);
	}
	return { 2.0f * kPad + bodyWidth(),
		     modalBottom() + kGroupGap + kGraphHeight + tallest + kPad };
}

void OwlttExpanded::resized() {
	const visage::Bounds plot = graphBounds();
	if (stereo_ != nullptr) {
		const visage::Bounds chip = stereoBounds();
		stereo_->setBounds(chip.x(), chip.y(), chip.width(), chip.height());
	}
	if (lookahead_ != nullptr) {
		const visage::Bounds box = lookaheadBounds();
		lookahead_->setBounds(box.x(), box.y(), box.width(), box.height());
	}
	if (keyTrack_ != nullptr) {
		const visage::Bounds chip = keyTrackBounds();
		keyTrack_->setBounds(chip.x(), chip.y(), chip.width(), chip.height());
	}
	if (drive_ != nullptr) {
		const visage::Bounds bar = driveBounds();
		drive_->setBounds(bar.x(), bar.y(), bar.width(), bar.height());
	}
	if (graph_ != nullptr) {
		graph_->setBounds(plot.x(), plot.y(), plot.width(), plot.height());
#ifdef FEATHERS_DIAGNOSTIC
		diagnostic::plotLaidOut("panel", graph_->positionInWindow().x, graph_->positionInWindow().y,
		                        graph_->width(), graph_->height(), graph_->isVisible());
#endif
	}

	// SIZED FOR THE TALLEST IN THE GRID, which is now the KNOB rather than the zoned control. The
	// coordinate box that replaced the knob-plus-box assembly is about half its height — a caption, a
	// row of numbers and a name, against a 38px face plus a caption plus a readout plus a box — so
	// the row goes back to a plain knob column and the two boxes sit inside it with room to spare.
	const float column = Knob::columnHeight(kKnobDiameter);

	// ONE WALK, PER COLUMN, placing captions and knobs together — so the caption a case reads is the
	// caption `draw` writes, at the rectangle `resized` chose.
	//
	// `next` WALKS THE KNOBS IN DECLARATION ORDER while the groups are visited in that same order,
	// which keeps `knobs_` and this loop in step: the constructor built them by walking `groups()`
	// exactly once, so the nth knob is the nth entry however the groups land on screen.
	captionBounds_.assign(groups().size(), visage::Bounds {});
	size_t next = 0;
	std::array<float, 4> top {};
	top.fill(plot.bottom());
	for (size_t g = 0; g < groups().size(); ++g) {
		const KnobGroup& group = groups()[g];
		const auto col = static_cast<size_t>(std::clamp(group.column, 0, 3));
		const float x = columnX(static_cast<int>(col));
		const float w = columnWidth(static_cast<int>(col));
		const auto cells = static_cast<size_t>(kColumnCells[col]);
		const float cell = w / static_cast<float>(cells);
		top[col] += kGroupGap;
		captionBounds_[g] = { x, top[col], w, kCaptionHeight };
		top[col] += kCaptionHeight + kRowGap;
		for (size_t i = 0; i < group.knobs.size() && next < cells_.size(); ++i, ++next) {
			const size_t line = i / cells;
			const size_t at = i % cells;
			const visage::Bounds box { x + cell * static_cast<float>(at),
				                       top[col] + static_cast<float>(line) * (column + kRowGap), cell, column };
			bool isChip = false;
			for (const auto& chip : chips_) {
				isChip = isChip || cells_[next] == chip.get();
			}
			if (isChip) {
				// A CHIP ON THE KNOBS' FACE LINE, centred where a face would be — it is shorter than a column.
				cells_[next]->setBounds(box.x() + 2.0f, box.y() + 0.5f * (kKnobDiameter - SwitchChip::kHeight),
				                        box.width() - 4.0f, SwitchChip::kHeight);
				continue;
			}
			cells_[next]->setBounds(box.x(), box.y(), box.width(), box.height());
		}
		const size_t rows = group.knobs.empty() ? 0 : (group.knobs.size() + cells - 1) / cells;
		top[col] += static_cast<float>(rows) * column
		            + (rows > 1 ? static_cast<float>(rows - 1) * kRowGap : 0.0f);
	}
}

void OwlttExpanded::draw(visage::Canvas& canvas) {
	// NO PANEL BACKGROUND — the overlay draws one and a second would be a card inside a card. The
	// knobs draw themselves.

	// THE CROSSOVER TYPE. A framed readout rather than a bare word, because it is pressable and a
	// word that opens a menu with nothing around it is not discoverable — the same reason the slot
	// menu draws its dots.
	const visage::Bounds mode = modeBounds();
	canvas.setColor(withAlpha(kLine, 0.55f));
	canvas.roundedRectangleBorder(mode.x(), mode.y(), mode.width(), mode.height(), 3.0f, 1.0f);
	canvas.setColor(kInk);
	canvas.text(modeText(), font(kKnobReadoutSize), visage::Font::kLeft, mode.x() + 7.0f, mode.y(),
	            mode.width() - 20.0f, mode.height());
	// A CHEVRON, GEOMETRY AND NOT A GLYPH — the standing rule: this font has no guaranteed arrow and
	// a missing glyph is an invisible affordance.
	{
		const float cx = mode.right() - 10.0f;
		const float cy = mode.y() + mode.height() * 0.5f;
		canvas.setColor(kMuted);
		canvas.segment(cx - 3.5f, cy - 1.5f, cx, cy + 2.0f, 1.3f, true);
		canvas.segment(cx, cy + 2.0f, cx + 3.5f, cy - 1.5f, 1.3f, true);
	}

	// THE GROUP CAPTIONS, READ FROM `captionBounds_` RATHER THAN RECOMPUTED, so the caption is
	// written where `resized` put it — HARM's rule and its exact words: two places computing one
	// geometry is the seam this lane keeps producing, and it costs nothing to not do it again.
	const std::span<const KnobGroup> all = groups();
	for (size_t g = 0; g < all.size() && g < captionBounds_.size(); ++g) {
		const visage::Bounds caption = captionBounds_[g];
		canvas.setColor(withAlpha(kMuted, 0.75f));
		drawTracked(canvas, all[g].caption, font(kKnobLabelSize), kKnobLabelTracking, caption.x(),
		            caption.y(), caption.height());
		// A HAIRLINE ACROSS THE REST OF THE ROW, so the caption reads as a section head rather than
		// as a stray word above a knob.
		const float textWidth =
		    trackedWidth(font(kKnobLabelSize), all[g].caption, kKnobLabelTracking);
		const float ruleLeft = caption.x() + textWidth + 8.0f;
		if (caption.right() > ruleLeft) {
			canvas.setColor(withAlpha(kLine, 0.55f));
			canvas.fill(ruleLeft, caption.y() + caption.height() * 0.5f, caption.right() - ruleLeft,
			            1.0f);
		}
	}
}

size_t OwlttExpanded::knobCount() {
	size_t n = 0;
	for (const KnobGroup& group : groups()) {
		n += group.knobs.size();
	}
	return n;
}

dtt::Param OwlttExpanded::paramOfKnobForTest(const Knob* knob) const {
	for (size_t i = 0; i < knobs_.size() && i < knobParams_.size(); ++i) {
		if (knobs_[i].get() == knob) {
			return knobParams_[i];
		}
	}
	return dtt::Param::kCount;
}

std::vector<dtt::Param> OwlttExpanded::addressedParamsForTest() const {
	// THE KNOBS ANSWER FOR THEMSELVES, plus the enable this panel's own square switches. Nothing
	// else is claimed: a surface that claimed a control no hand can reach on it would make the
	// reachability invariant pass by assertion rather than by construction.
	std::vector<dtt::Param> out = knobParams_;
	// AND THE COORDINATE BOXES' TWO CONTROLS EACH — fine and coarse, recorded as the widget was
	// built. `extraReached_` carried the phase halves while they hung off a knob; the coordinate box
	// owns both halves outright, so both are recorded together and nothing is left implicit.
	out.insert(out.end(), coordParams_.begin(), coordParams_.end());
	out.insert(out.end(), extraReached_.begin(), extraReached_.end());
	// AND EVERY CONTROL THE PLOT PUTS A HANDLE ON. The twelve per-band controls have no knob on this
	// surface any more and do not need one -- a bar IS the threshold and a strip IS the ratio, drawn
	// where the value is, which is the one case where direct manipulation is not a metaphor.
	//
	// CLAIMED HERE BECAUSE THE PANEL REALLY DOES REACH THEM. The reachability invariant is about
	// whether a hand can get to a control on this surface, not about whether a circle exists for it,
	// and claiming a control this panel could not touch would make that invariant pass by assertion.
	for (size_t band = 0; band < 3; ++band) {
		static constexpr dtt::Param kPerBand[3][4] = {
			{ dtt::Param::LowThreshold, dtt::Param::LowBandwidth, dtt::Param::LowRatio,
			  dtt::Param::LowLevel },
			{ dtt::Param::MidThreshold, dtt::Param::MidBandwidth, dtt::Param::MidRatio,
			  dtt::Param::MidLevel },
			{ dtt::Param::HighThreshold, dtt::Param::HighBandwidth, dtt::Param::HighRatio,
			  dtt::Param::HighLevel },
		};
		for (const dtt::Param p : kPerBand[band]) {
			out.push_back(p);
		}
	}
	// AND THE BESPOKE CONTROL IN THE MODAL ROW: the crossover-type dropdown. Mode had NO surface
	// anywhere until then — there was nowhere to select the crossover type — and the card's
	// reachability table recorded it as "drawn in the head", which was true of the folded head's
	// READOUT and not of any control. (The enable square beside it is the window header's chip now.)
	out.push_back(dtt::Param::Mode);
	// AND THE L/R-M/S CHIP in the same row (backlog LQ).
	out.push_back(dtt::Param::StereoMode);
	// AND THE DRIVE BAR left of the graph (backlog NB).
	out.push_back(dtt::Param::Drive);
	// AND EVERY BAND SLIDER BANK'S THREE (backlog NK), recorded as each was built.
	for (const auto& bands : stackParams_) {
		out.insert(out.end(), bands.begin(), bands.end());
	}
	// AND THE LOOKAHEAD'S BOX in the modal row (backlog ND).
	out.push_back(dtt::Param::Lookahead);
	// AND KEY TRACK'S CHIP beside it.
	out.push_back(dtt::Param::KeyTrack);
	return out;
}

void OwlttExpanded::offeredControls(std::vector<Offered>& out) const {
	// THE MODE, drawn by this view and chosen from its menu (`chooseMode`) — every mode on demand before it applies.
	out.push_back({ &control(dtt::Param::Mode), Road::Face });
}

} // namespace feathers::ui
