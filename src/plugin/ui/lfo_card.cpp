/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program.
 * If not, see <https://www.gnu.org/licenses/>.
 */

#include "plugin/ui/lfo_card.h"

#include "device/format_value.h"
#include "mod/lfo/lfo.h"

#include "plugin/gui_bridge.h"
#include "plugin/ui/celestial.h"
#include "plugin/ui/plot_grid.h"
#include "plugin/ui/trace_marker.h"
#include "plugin/ui/theme.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace feathers::ui {

namespace lfo = mod::lfo;

namespace {

constexpr float kPlayheadSize = 3.0f;

/// A WHOLE-SETTINGS EDIT, which is what a chip press is — the MSEG's `MsegShapeEdit` for this device,
/// and the same reasoning: the row encoder is the one place that knows the shape, so undo writes the
/// way the card does rather than restating it. A switch is one value moving from one
/// setting to another, not a field edit somebody has to replay in order.
class LfoShapeEdit : public EditRecord {
public:
	LfoShapeEdit(backplane::DeviceHost& host, DeviceInstanceId id, mod::lfo::Settings before,
	             mod::lfo::Settings after)
	    : host_(host), device_(id), before_(before), after_(after) {}

	void undo() const override { write(before_); }
	void redo() const override { write(after_); }
	const char* verb() const override { return "Shape"; }

private:
	void write(const mod::lfo::Settings& settings) const {
		const mod::lfo::Row row = mod::lfo::rowOf(settings);
		backplane::ControlGestureScope gesture(host_, device_); // one host gesture per undo (host slots, T6b)
		for (size_t i = 0; i < row.size(); ++i) {
			gesture.setField(i, row[i]);
		}
	}

	backplane::DeviceHost& host_;
	DeviceInstanceId device_;
	mod::lfo::Settings before_;
	mod::lfo::Settings after_;
};

constexpr std::array<const char*, LfoCard::kBoxCount> kCaptions { "Base", "Rate", "Sm",  "Amt",
	                                                                  "Ph",   "Pk",   "Cr",  "Tr",
	                                                                  "Seed", "St" };

} // namespace

LfoCard::LfoCard(backplane::DeviceHost& host, DeviceInstanceId id, ModState& mod, EditHistory& history)
	: host_(host), device_(id), mod_(mod), history_(history) {
	for (size_t i = 0; i < kBoxCount; ++i) {
		controls_[i] = std::make_unique<DeviceControl>(host_, device_, mod::lfo::kDescriptor,
		                                              static_cast<size_t>(layout()[i]), &mod_, &history_);
		boxes_[i] = std::make_unique<ValueBox>(*controls_[i], kCaptions[i]);
		addChild(boxes_[i].get());
		bindBox(i);
		// **RATE READS HZ ON THE HZ BASE** (backlog NJ, decided 2026-10-17): there the number is a frequency,
		// `lfo::rateHz`'s own, and "1X" would be a multiple of a base that does not exist. Elsewhere it is the
		// declared multiple. The base is read at each draw, so a pick of Time Base relabels it at once.
		if (layout()[i] == lfo::Param::TimeScale) {
			boxes_[i]->setReadoutFormatter([this](double v) { return rateFace(v); }, true);
		}
	}
	// THE CHOICES — compact, uncaptioned, in the chip row. The menu is the box's own, opened from the
	// descriptor (`ValueBox::choiceMenuAvailable`); nothing here opts in.
	for (size_t i = 0; i < kChoiceCount; ++i) {
		const lfo::Param param = choices()[i];
		choiceControls_[i] = std::make_unique<DeviceControl>(host_, device_, mod::lfo::kDescriptor,
		                                                    static_cast<size_t>(param), &mod_, &history_);
		choiceBoxes_[i] = std::make_unique<ValueBox>(*choiceControls_[i], "");
		choiceBoxes_[i]->setCompact(true);
		choiceBoxes_[i]->setReadoutFormatter([param](double v) { return choiceFace(param, v); });
		// THE WAVEFORM IS THE BOXES' GATE, so a pick re-lays the rows in the same frame rather than
		// at the next `updateContent`.
		choiceBoxes_[i]->onValueChanged([this]() {
			resized();
			redraw();
		});
		addChild(choiceBoxes_[i].get());
	}
	// THE TRIG PAD (backlog MV): the device's Trig control, and the fire count it lights from.
	trigControl_ = std::make_unique<DeviceControl>(host_, device_, mod::lfo::kDescriptor,
	                                               static_cast<size_t>(lfo::Param::Trig), &mod_, &history_);
	trigPad_ = std::make_unique<TrigPad>(*trigControl_, [this](uint32_t& fires) {
		return host_.trigFiresAt(device_, fires);
	});
	// AND ITS NORMAL (backlog NU): the right-click's "Notes fire Trig", and the dot that shows it made.
	notesTrigControl_ = std::make_unique<DeviceControl>(host_, device_, mod::lfo::kDescriptor,
	                                                    static_cast<size_t>(lfo::Param::NotesTrig), &mod_, &history_);
	trigPad_->setNormal({ notesTrigControl_.get() });
	addChild(trigPad_.get());
	watches_.push_back(mod_.watch(trigPad_.get())); // its ring is the mapping signal
	onMouseDown() = [this](const visage::MouseEvent& e) {
		// **THE PRESS IS THE CARD'S TO HAND BACK, AND THIS CARD WAS KEEPING IT.** `FxCard` takes the
		// drag and the release — `onMouseDrag`/`onMouseUp` are assigned in its constructor — and says
		// in as many words that the press belongs here: every other card in this editor opens its
		// handler with this line, and this one was the only one of thirteen that did not.
		//
		// WHAT THAT COSTS IS EVERY BUTTON ON THE HEAD (the head's buttons stopped responding).
		// `noteCardPress` is what dispatches the head's marks, what records
		// the press so `noteCardRelease` can FOLD on release — the fold is a head press since the `(-)`
		// mark was retired — and what fires `onCardPressed`, which is how a lane learns that Backspace
		// and Delete now mean THIS slot. Without it the head was inert, the card could not be folded,
		// and pressing it left the key target naming whichever card was pressed last: the identical
		// defect `fx_card.h` records against the DAHDSR's value boxes, arriving by the other road.
		noteCardPress(e);
		// **THE SLOT MENU, WHEN A HOST BINDS ONE** — Feathers' fx lane, which holds modulators since 2026-10-04, and a
		// chain's lane (`onSlotMenu`). Every audio card makes this call after `noteCardPress`; the modulator cards did
		// not, so in a lane their dots were drawn and dead (the head-verb sweep found all five). The strip binds no
		// `onSlotMenu` — it opens its menu from the head mark — so there this is a no-op.
		if (slotMenuAt(e.position)) {
			pressSlotMenu();
			return;
		}
		if (enablePressAt(e.position)) {
			return;
		}
		if (e.position.y < bodyTopEdge()) {
			return;
		}
		for (size_t chip = 0; chip < kChipCount; ++chip) {
			if (chipBounds(chip).contains(e.position)) {
				if (chipLive(chip)) {
					toggleChip(chip);
				}
				return;
			}
		}
	};
}
const std::array<lfo::Param, LfoCard::kBoxCount>& LfoCard::layout() {
	static const std::array<lfo::Param, kBoxCount> k {
		// ROW ONE — THE MSEG'S FIVE, in the MSEG's order, because a modulator's time and depth are the
		// same questions whichever shape is making them and a hand should not have to look twice.
		// The window's two halves sit side by side for that card's stated reason: the shape fits inside
		// `Base x Rate`, and either alone says nothing about how long it takes.
		lfo::Param::TimeBase,
		lfo::Param::TimeScale,
		lfo::Param::Smoothing,
		lfo::Param::Amount,
		lfo::Param::Phase,
		// ROW TWO — WHAT THIS DEVICE ADDS. The three skew weights in the order they happen around the
		// cycle, so the row reads left to right as the wave does, and then the seed.
		lfo::Param::SkewPrePeak,
		lfo::Param::SkewPreCross,
		lfo::Param::SkewPreTrough,
		lfo::Param::Seed,
		// AND STEREO PHASE (backlog PA), every wave's, so it packs after whichever of the above are live: fourth with
		// a shaped wave, second with a walk. The fifth column is Carry's (`chipBounds`), which neither reaches.
		lfo::Param::StereoPhase,
	};
	return k;
}

const std::array<lfo::Param, LfoCard::kChoiceCount>& LfoCard::choices() {
	static const std::array<lfo::Param, kChoiceCount> k { lfo::Param::Waveform, lfo::Param::Mode };
	return k;
}

std::string LfoCard::rateFace(double value) const {
	const auto base = static_cast<size_t>(std::max(0L, std::lround(host_.fieldAt(device_, static_cast<size_t>(lfo::Param::TimeBase)))));
	lfo::Settings s {};
	s.timeBase = base;
	s.timeScale = static_cast<float>(value);
	if (s.synced()) {
		return device::formatValue(lfo::kParamDescs[static_cast<size_t>(lfo::Param::TimeScale)], value);
	}
	// THE ENGINE'S RULE FOR THE FREQUENCY — `rateHz` with no tempo is exactly the Hz base's reading.
	char out[32] = {};
	std::snprintf(out, sizeof(out), "%.3gHz", lfo::rateHz(s, 0.0));
	return out;
}

std::string LfoCard::choiceFace(lfo::Param param, double value) {
	const auto setting = static_cast<size_t>(std::max(0L, std::lround(value)));
	// SHORTENED FOR THE CELL, NOT RENAMED — the env card's rule, and only where it is needed. The
	// device declares "Random Glide" and "LFO Retrig", and those are the names in the menu, the route
	// list, the tooltip and the host's parameter list; a choice cell of this card's chip row holds neither
	// at the readout size, which is a LAYOUT fact rather than a naming one. The rest fit as declared.
	if (param == lfo::Param::Waveform) {
		static constexpr std::array<const char*, static_cast<size_t>(lfo::Waveform::kCount)> kShort {
			"Sine", "Triangle", "Square", "Random", "Glide",
		};
		return setting < kShort.size() ? kShort[setting] : "?";
	}
	if (param == lfo::Param::Mode) {
		static constexpr std::array<const char*, static_cast<size_t>(lfo::Mode::kCount)> kShort {
			"Gate", "Loop", "Once", "Free", "Retrig",
		};
		return setting < kShort.size() ? kShort[setting] : "?";
	}
	return "?";
}

const std::array<LfoCard::Chip, LfoCard::kChipCount>& LfoCard::chips() {
	static const std::array<Chip, kChipCount> k {
		// WAVEFORM AND MODE ARE NOT CHIPS ANY MORE — see `choices()` (backlog GG). A chip is a switch.
		Chip { lfo::Param::PingPong, "P-P" },
		Chip { lfo::Param::Bipolar, "±" },
		// **POLY — one shape per note, or one for the device**. A switch, so a chip: two states,
		// both visible by being the only two.
		Chip { lfo::Param::Poly, "Poly" },
		// **SMOOTH THROUGH RETRIGGER** (backlog MM): a new note's smoothing starts where the last note's output stood.
		// Live only where it changes something — see `chipLive`.
		Chip { lfo::Param::SmoothCarry, "Carry" },
		// "Sm" IS GONE WITH ITS PARAM — the bottom of the Smoothing box is its off.
		// On this device the switch was hiding a second fault as well: the engine tested
		// `smoothing > -10.0f` against a declared minimum of -11, so the knob's lowest octave did
		// nothing either. Both are the same kind of lie and both are fixed by the range carrying "off".
		// **AND NOT A SKEW.** `SkewPreTrough` sat here as a sixth chip: a 0..100% time weight driven by
		// a TOGGLE, which flipped it between 50 and 100 and read "on" above 0.5. Ninety-nine of its
		// hundred positions could not be reached by hand, and the two that could were a number the
		// label printed where every other chip prints a word. A chip is a switch — see `toggleChip`,
		// and the MSEG's list. The three skews are value boxes now, where a weight belongs.
	};
	return k;
}

bool LfoCard::chipLive(size_t index) const {
	if (index >= kChipCount) {
		return false;
	}
	const lfo::Settings s = settings();
	switch (chips()[index].param) {
	// AND NOT UNDER LFO RETRIG WITH THE NORMAL BROKEN (backlog NU), which is Free less the song lock: `mod::polyRuns`.
	case lfo::Param::Poly: return mod::polyRuns(s.mode, s.notesTrig);
	// CARRY CHANGES SOUND ONLY UNDER POLY (mono already smooths through), NOT IN FREE (which is mono whatever Poly
	// says), and only with some smoothing to carry — `mod::PolyShape::carrySmoothing`.
	case lfo::Param::SmoothCarry: return s.poly && mod::polyRuns(s.mode, s.notesTrig) && mod::smoothingTau(s.smoothing) > 0.0;
	default: return true;
	}
}

bool LfoCard::chipOn(size_t index) const {
	if (index >= kChipCount) return false;
	const lfo::Param param = chips()[index].param;
	return lfo::valueOf(settings(), param) >= 0.5f;
}

std::string LfoCard::chipLabel(size_t index) const {
	if (index >= kChipCount) return {};
	return chips()[index].label;
}

void LfoCard::toggleChip(size_t index) {
	if (index >= kChipCount) return;
	const lfo::Param param = chips()[index].param;
	lfo::Settings s = settings();
	const lfo::Settings was = s;
	const double before = lfo::valueOf(s, param);
	// A SWITCH, and only a switch: a control with more than two states is a `choices()` box.
	lfo::setValueOf(&s, param, before >= 0.5 ? 0.0f : 1.0f);
	writeSettings(s);
	// **AND IT IS ONE UNDO STEP, WHICH IT WAS NOT.** Every box on this card writes through
	// `DeviceControl`, which records history; the chips wrote straight through `writeSettings` and
	// recorded nothing, so a chip could not be undone while changing the rate by box could. The
	// MSEG's `toggleChip` banks a `MsegShapeEdit` for exactly this reason, and `LfoShapeEdit` is the
	// same entry for this device.
	history_.record(std::make_unique<LfoShapeEdit>(host_, device_, was, s));
	redraw();
}

visage::Bounds LfoCard::editorBounds() const {
	return { bodyLeft() + kPadX + kTrigColumn, bodyTop() + kGap, bodyWidth() - 2.0f * kPadX - kTrigColumn,
		     kEditorHeight };
}

visage::Bounds LfoCard::trigPadBounds() const {
	const visage::Bounds box = editorBounds();
	return { bodyLeft() + kPadX, box.y() + (box.height() - TrigPad::kDiameter) * 0.5f, TrigPad::kDiameter,
		     TrigPad::kDiameter };
}

visage::Bounds LfoCard::editorGrabBounds() const {
	return editorBounds();
}

float LfoCard::boxRowTop(size_t row) const {
	return bodyTop() + kGap + kEditorHeight + kGap
	       + static_cast<float>(row) * (ValueBox::kHeight + kRowGap);
}

bool LfoCard::boxIsLive(size_t index) const {
	// **ASKED OF THE DESCRIPTOR AND OF THE GATE'S CURRENT VALUE**, which is why it cannot be a free
	// function on the descriptor alone: reading the gate needs the instance. `device::relevantFor`
	// does the deciding; this supplies the number. The same two lines `GenericCard::relevant` is, and
	// deliberately not a copy of its reasoning — that comment is the one to read.
	if (index >= kBoxCount) {
		return true;
	}
	const auto& descs = mod::lfo::kDescriptor.params;
	const auto param = static_cast<size_t>(layout()[index]);
	if (param >= descs.size()) {
		return true;
	}
	const device::ParamDesc& p = descs[param];
	if (p.gateControl < 0) {
		return true;
	}
	const auto gate = static_cast<size_t>(p.gateControl);
	if (gate >= descs.size()) {
		return true;
	}
	return device::relevantFor(p, host_.fieldAt(device_, gate));
}

visage::Bounds LfoCard::boxBounds(size_t index) const {
	// EVERY ROW IS DIVIDED INTO `kBoxesPerRow`, INCLUDING THE SHORT ONE. The second row holds four of
	// the five places, so the boxes in it are the same width as the boxes above them and the columns
	// line up; a row that shared its width among however many it happened to hold would put four wide
	// boxes under five narrow ones and look like a different card.
	const float usable = bodyWidth() - 2.0f * kPadX;
	const float each = (usable - kRowGap * (kBoxesPerRow - 1)) / static_cast<float>(kBoxesPerRow);
	const size_t row = index / kBoxesPerRow;
	// **THE LIVE BOXES PACK FROM THE LEFT OF THEIR ROW**, so a hidden control leaves no hole. With a
	// shaped wave the second row reads `Pk Cr Tr`; with Random it reads `Seed`, in the first column
	// rather than the fourth. Counting only live predecessors is what does it — the column is a
	// position among what is SHOWN, not among what exists.
	size_t column = 0;
	for (size_t earlier = row * kBoxesPerRow; earlier < index; ++earlier) {
		if (boxIsLive(earlier)) {
			++column;
		}
	}
	return { bodyLeft() + kPadX + static_cast<float>(column) * (each + kRowGap), boxRowTop(row), each,
		     ValueBox::kHeight };
}

visage::Bounds LfoCard::cellBounds(size_t cell) const {
	// **A CHOICE CELL IS TWO UNITS WIDE AND A CHIP ONE.** Four equal quarters gave a menu box 38.5px of
	// text, which held "Triangle" (33) until the dropdown caret took its 7 — and a switch captioned
	// "P-P" or "±" needs a third of that. Measured, not guessed: the fit case in
	// `test_gui_widgets_devices` asks every face against the box's readout width.
	constexpr size_t kCells = kChoiceCount + kRowChips;
	constexpr float kUnits = 2.0f * kChoiceCount + kRowChips;
	const float usable = bodyWidth() - 2.0f * kPadX;
	const float unit = (usable - kRowGap * (kCells - 1)) / kUnits;
	float x = bodyLeft() + kPadX;
	for (size_t before = 0; before < cell; ++before) {
		x += (before < kChoiceCount ? 2.0f : 1.0f) * unit + kRowGap;
	}
	return { x, boxRowTop(kBoxRows - 1) + ValueBox::kHeight + kGap, (cell < kChoiceCount ? 2.0f : 1.0f) * unit,
		     kChipHeight };
}

visage::Bounds LfoCard::chipBounds(size_t index) const {
	if (index < kRowChips) {
		return cellBounds(kChoiceCount + index);
	}
	// **CARRY (backlog MM) SITS AT THE END OF THE SECOND BOX ROW**, in its last column, which nothing else ever
	// takes: that row shows the three skews or the Seed, never more than three at once. The chip row could not hold a
	// fourth chip — MEASURED by the fit case: at a whole unit each the choices fell to 27.5px against "Triangle"'s
	// 33, and at three quarters each the chips fell to 19px against "±"'s 20.
	const float usable = bodyWidth() - 2.0f * kPadX;
	const float each = (usable - kRowGap * (kBoxesPerRow - 1)) / static_cast<float>(kBoxesPerRow);
	return { bodyLeft() + kPadX + static_cast<float>(kBoxesPerRow - 1) * (each + kRowGap),
		     boxRowTop(1) + 0.5f * (ValueBox::kHeight - kChipHeight), each, kChipHeight };
}

visage::Bounds LfoCard::choiceBounds(size_t index) const {
	return cellBounds(index);
}

std::pair<float, float> LfoCard::toUnit(visage::Point point) const {
	const visage::Bounds box = editorBounds();
	const float x = std::clamp((point.x - box.x()) / std::max(box.width(), 1.0f), 0.0f, 1.0f);
	const float up = 1.0f - std::clamp((point.y - box.y()) / std::max(box.height(), 1.0f), 0.0f, 1.0f);
	const bool bipolar = settings().bipolar;
	return { x, bipolar ? up * 2.0f - 1.0f : up };
}

visage::Point LfoCard::fromUnit(float x, float y) const {
	const visage::Bounds box = editorBounds();
	const bool bipolar = settings().bipolar;
	const float up = bipolar ? (y + 1.0f) * 0.5f : y;
	return { box.x() + std::clamp(x, 0.0f, 1.0f) * box.width(),
	     box.bottom() - std::clamp(up, 0.0f, 1.0f) * box.height() };
}

lfo::Settings LfoCard::settings() const {
	lfo::Row row {};
	const size_t present = std::min(host_.fieldsPresentAt(device_), row.size());
	for (size_t i = 0; i < present; ++i) {
		row[i] = host_.fieldAt(device_, i);
	}
	return lfo::settingsOf(row, present);
}

void LfoCard::writeSettings(const lfo::Settings& s) {
	const lfo::Row row = lfo::rowOf(s);
	// ONE USER ACTION, ONE HOST GESTURE per control it moves (host slots, T6b review).
	backplane::ControlGestureScope gesture(host_, device_);
	for (size_t i = 0; i < row.size(); ++i) {
		gesture.setField(i, row[i]);
	}
}

void LfoCard::drawContent(visage::Canvas& canvas) {
	const visage::Bounds box = editorBounds();

	// OSCILLOSCOPE BACKGROUND — dark with subtle border.
	canvas.setColor(kVoid);
	canvas.roundedRectangle(box.x(), box.y(), box.width(), box.height(), 4.0f);
	canvas.setColor(withAlpha(kLine, 0.3f));
	canvas.roundedRectangleBorder(box.x(), box.y(), box.width(), box.height(), 4.0f, 1.0f);

	// **THE GRID — and it is no longer a uniform crosshatch.** It drew eight verticals AND eight
	// horizontals at one alpha, which gives a plot 14 lines of equal weight and no structure: nothing
	// in it says which division is the important one. The design: faint grid lines on both axes —
	// a horizontal at 50% amplitude, verticals at 1/4 of the window and fainter at 1/8. So one horizontal at half the
	// amplitude, quarters, and eighths fainter under them.
	//
	// SHARED WITH THE MSEG, and shared for a reason that is not tidiness — `plot_grid.h` also owns the
	// divisions a drag sticks to, and a snap that lands somewhere the picture does not draw is a
	// control fighting the hand for reasons nothing on screen explains.
	plot::drawGrid(canvas, box);

	const lfo::Settings s = settings();

	// ZERO/CENTER LINE.
	const float zeroY = s.bipolar ? 0.5f : 1.0f;
	canvas.setColor(withAlpha(kValue, 0.5f));
	canvas.segment(box.x() + 2.0f, box.y() + (1.0f - zeroY) * box.height(), box.right() - 2.0f,
	                box.y() + (1.0f - zeroY) * box.height(), 1.0f, true);

	// THE WAVEFORM.
	const std::vector<std::pair<float, float>> points = curve();

	// Filled area under the curve (oscilloscope style).
	canvas.setColor(withAlpha(kValue, 0.12f));
	for (size_t i = 1; i < points.size(); ++i) {
		const visage::Point a = fromUnit(points[i - 1].first, points[i - 1].second);
		const visage::Point b = fromUnit(points[i].first, points[i].second);
		const float baseY = box.y() + (1.0f - zeroY) * box.height();
		// Quad: a -> b -> base -> base
		canvas.rectangle(std::min(a.x, b.x), std::min(a.y, baseY),
		                 std::max(std::abs(b.x - a.x), 1.0f), std::abs(a.y - baseY));
	}

	// Waveform line — bright. **AND BOTH EARS WHILE STEREO PHASE IS NOT 0** (backlog PA), in the stereo ring's two colours:
	// the right ear's line first, so the left one — where the playhead star rides — is the one on top.
	const std::vector<std::pair<float, float>> right = curveRight();
	rightStrokes_ = 0;
	if (!right.empty()) {
		canvas.setColor(kModulationR);
		for (size_t i = 1; i < right.size(); ++i) {
			const visage::Point a = fromUnit(right[i - 1].first, right[i - 1].second);
			const visage::Point b = fromUnit(right[i].first, right[i].second);
			canvas.segment(a.x, a.y, b.x, b.y, 2.0f, true);
			++rightStrokes_;
		}
	}
	canvas.setColor(right.empty() ? kValue : kModulationL);
	for (size_t i = 1; i < points.size(); ++i) {
		const visage::Point a = fromUnit(points[i - 1].first, points[i - 1].second);
		const visage::Point b = fromUnit(points[i].first, points[i].second);
		canvas.segment(a.x, a.y, b.x, b.y, 2.0f, true);
	}

	// Playhead star.
	if (const Marker mark = marker(); mark.drawn) {
		starGlyph(canvas, mark.at.x, mark.at.y, kPlayheadSize, kAccent, 1.0f);
	}

	// THE CHIPS. Drawn at the bottom of the card.
	for (size_t chip = 0; chip < kChipCount; ++chip) {
		const visage::Bounds b = chipBounds(chip);
		const bool on = chipOn(chip);
		// A DEAD CHIP (`chipLive`) IS DRAWN FAINT and does not take a press.
		const float live = chipLive(chip) ? 1.0f : 0.35f;
		canvas.setColor(on ? withAlpha(kAccent, 0.22f * live) : kVoid);
		canvas.roundedRectangle(b.x(), b.y(), b.width(), b.height(), 3.0f);
		canvas.setColor(on ? withAlpha(kAccent, live) : withAlpha(kLine, 0.8f * live));
		canvas.roundedRectangleBorder(b.x(), b.y(), b.width(), b.height(), 3.0f, 1.0f);
		canvas.setColor(withAlpha(on ? kInk : kMuted, live));
		canvas.text(chipLabel(chip), font(kKnobLabelSize), visage::Font::kCenter, b.x(), b.y(),
		            b.width(), b.height());
	}
	// THE CHOICES' CELLS — outlined as an unlit chip, so the row reads as one row; the box inside
	// draws the value. See `drawChoiceCell`.
	for (size_t choice = 0; choice < kChoiceCount; ++choice) {
		drawChoiceCell(canvas, choiceBounds(choice));
	}
}

bool LfoCard::updateContent() {
	// **THE GATE MOVES BY MORE ROADS THAN THE BOX.** Picking the waveform re-lays the card out at the
	// press, but the same value also arrives from an undo, a preset load, host automation and the
	// modulation of another control — and a row that reflows only when a hand did it would show a
	// seed box over a triangle for as long as nobody touched the box. Watched here, where every
	// frame already asks this card whether it is out of date.
	const bool trigChanged = trigPad_->poll();
	const float gate = gateValue();
	if (gate != gateDrawn_) {
		resized();  // stamps gateDrawn_
		redraw();
		return true;
	}
	const mod::Playhead now = playhead();
	if (now == drawnPlayhead_) {
		return trigChanged;
	}
	drawnPlayhead_ = now;
	redraw();
	return true;
}

LfoCard::Marker LfoCard::marker() const {
	Marker m {};
	const mod::Playhead where = playhead();
	if (!where.running()) {
		return {};
	}
	const auto points = curve();
	if (points.empty()) {
		return {};
	}
	// Find the y at the current phase by interpolating the curve.
	//
	// **AND THE RANDOM SCOPE'S X IS NOT THE PHASE — IT IS THE RIGHT-HAND EDGE.** Every shaped wave
	// draws ONE period, so phase 0..1 is the whole width and the star rides it directly. The random
	// scope draws a window of `kScopeSteps` ENDING AT NOW, so "now" is x = 1 by construction and the
	// star sits there; what it still says is the VALUE, which is the whole of what a playhead on a
	// scrolling scope has left to say.
	//
	// **IT USED TO BE `(kScopeSteps - 1 + phase) / kScopeSteps`, AND THAT WAS RIGHT FOR THE WINDOW IT
	// HAD.** That window was eight WHOLE steps ending at the end of the current one, so the fraction
	// placed the star inside the final division. The window slides now (`randomTrace`), which moves
	// the fraction from the star's x into the window's own offset — the same number, doing the
	// scrolling instead of the pointing. Leaving this expression in place would have put the star
	// a fraction of a step ahead of the edge it is supposed to sit on.
	float phase = where.phase;
	if (lfo::isRandom(settings().wave)) {
		phase = 1.0f;
	}
	else {
		// **AND THE STAR ROTATES WITH THE CURVE.** `where.phase` is the ABSOLUTE waveform phase the
		// engine is at; the drawn window now begins at `Settings::phase` (see `curve`), so absolute
		// phase `p` sits at `p - start` along the card. Leaving this as the raw phase would have put
		// the star at the right place on a curve that had moved out from under it — visible the moment
		// Phase is anything but zero, and invisible in every test that leaves it at the default.
		const float start =
		    std::isfinite(settings().phase) ? settings().phase - std::floor(settings().phase) : 0.0f;
		const float rotated = phase - start;
		phase = rotated - std::floor(rotated);
	}
	const auto landing =
	    TraceMarker::locateBy(points.size(), [&points](size_t i) { return points[i].first; }, phase);
	if (!landing.valid) {
		return {};
	}
	const visage::Point a = fromUnit(points[landing.lo].first, points[landing.lo].second);
	const visage::Point b = fromUnit(points[landing.hi].first, points[landing.hi].second);
	m.at = { TraceMarker::at(landing, a.x, b.x), TraceMarker::at(landing, a.y, b.y) };
	m.drawn = true;
	return m;
}

mod::Playhead LfoCard::playhead() const {
	uint32_t ordinal = mod::Playhead::kIdle;
	float phase = 0.0f;
	if (!host_.playheadAt(device_, ordinal, phase)) {
		return {};
	}
	return { ordinal, phase };
}

std::vector<std::pair<float, float>> LfoCard::curve() const {
	return curveAt(0.0f);
}

std::vector<std::pair<float, float>> LfoCard::curveRight() const {
	const float lead = settings().stereoLead();
	return lead != 0.0f ? curveAt(lead) : std::vector<std::pair<float, float>> {};
}

std::vector<std::pair<float, float>> LfoCard::curveAt(float lead) const {
	const auto s = settings();
	if (lfo::isRandom(s.wave)) {
		// THE STEP THE ENGINE IS ON, which the playhead's ordinal carries for this waveform alone. A
		// card with no running device draws the walk from its own beginning, which is what it will play
		// when it starts.
		const mod::Playhead where = playhead();
		const int step = where.running() ? static_cast<int>(where.ordinal) : 0;
		// **AND HOW FAR INTO THAT STEP, or the window scrolls in lurches** (the oscilloscope scrolled
		// in chunks, one waypoint at a time). The ordinal alone quantises the window to
		// step boundaries; `phase` is the step's own fraction for a walk and arrives packed with the
		// ordinal in one publish, so passing both costs nothing and is the difference between a scope
		// that slides and one that jumps. A card with no running device draws from the walk's start,
		// which is what it will play when it starts.
		// THE RIGHT EAR (backlog PA) is the same walk `lead` steps on: its window ends that far past now.
		const float now = (where.running() ? where.phase : 0.0f) + lead;
		const float whole = std::floor(now);
		return lfo::randomTrace(s, step + static_cast<int>(whole), now - whole);
	}
	// **THE WINDOW STARTS WHERE THE CYCLE STARTS.** When Phase is pushed in any of the non-panning
	// oscillator modes (all but the randoms), the drawn curve updates so phase 0 is still at the start
	// of the drawn window.
	//
	// This drew `eval(s, x)` for x across the card: one period beginning at WAVEFORM phase 0, whatever
	// the Phase control said. The engine begins at `Settings::phase` — `Shape::restart` sets
	// `position_.phase = s.phase` — so pushing Phase moved where the shape STARTS and left the picture
	// showing a cycle beginning somewhere the device never begins. At Phase 0.5 on a square, the card
	// drew a pulse that rises at the left edge and the device played one that falls there.
	//
	// So the drawn window is rotated by the start phase: x = 0 is the start of the cycle, and pushing
	// Phase turns the curve under a fixed frame rather than leaving it still. The randoms are untouched
	// — their scope is a window over the song rather than over one period, and `Phase` is not what
	// places it (see `randomTrace`, above).
	//
	// **WRAPPED BEFORE `eval`, NOT INSIDE IT.** `SkewMap::remap` CLAMPS its argument to 0..1 rather
	// than wrapping — reasonably, since it is a piecewise map over one period — so handing it 1.3
	// would return the value at the end of the cycle for the whole of the overhang, flattening the
	// right-hand side of every rotated curve. `evalWaveform` wraps, `remap` does not, and `eval` calls
	// remap first.
	// AND THE RIGHT EAR (backlog PA) reads the same window `lead` of a cycle on, so its line is this one turned by the lead.
	const float phased = (std::isfinite(s.phase) ? s.phase : 0.0f) + lead;
	const float start = phased - std::floor(phased);
	constexpr size_t kPoints = 128;
	// **ONE PERIOD IS `kPoints - 1` DISTINCT SAMPLES**: the last drawn point sits at x = 1, which is
	// the same phase as x = 0. Smoothing the duplicate as if it were a further step would put a seam
	// at the right-hand edge of a curve that has none.
	constexpr size_t kPeriod = kPoints - 1;
	std::vector<float> wave(kPeriod);
	for (size_t i = 0; i < kPeriod; ++i) {
		const float at = start + static_cast<float>(i) / static_cast<float>(kPeriod);
		wave[i] = lfo::eval(s, at - std::floor(at));
	}

	// **AND WHAT THE SMOOTHING DOES TO IT, DRAWN RATHER THAN LEFT TO BE IMAGINED** (smoothing's
	// effect shown instantly, without the oscilloscope).
	// The card drew the raw wave at every smoothing setting, so the one control on this device with no
	// visible effect was the one whose effect is hardest to predict: a one-pole against a sine is a
	// lag and a flattening, against a square it is the difference between a pulse and something close
	// to a triangle, and none of it could be seen.
	//
	// IT IS A STATIC CURVE because a filter's response to a periodic input is periodic — see
	// `smoothPeriodic`, which solves the steady state in closed form. No scope, no settling to watch.
	//
	// **THE PERIOD COMES FROM THE ENGINE'S OWN RULE**, `lfo::rateHz`, rather than from arithmetic
	// repeated here: the shape depends on `tau / period` and a card that computed the period its own
	// way would draw a different wave from the one playing. `secondsPerBeat` is zero when the host has
	// said nothing, which `rateHz` answers with the same fallback the engine uses, so there is no
	// setting at which this cannot be drawn.
	const double hz = lfo::rateHz(s, host_.secondsPerBeat());
	const double tau = mod::smoothingTau(s.smoothing);
	if (hz > 0.0 && tau > 0.0) {
		lfo::smoothPeriodic(wave, 1.0 / hz, tau);
	}

	std::vector<std::pair<float, float>> c;
	c.reserve(kPoints);
	for (size_t i = 0; i < kPoints; ++i) {
		c.emplace_back(static_cast<float>(i) / static_cast<float>(kPeriod), wave[i % kPeriod]);
	}
	return c;
}

std::vector<lfo::Param> LfoCard::addressedParamsForTest() const {
	// **THIS RETURNED `{}` — A REACHABILITY SEAM THAT REPORTED NO REACH AT ALL.** Every other card
	// answers this honestly, and it is what the guards in `test_gui_widgets_devices` read to ask "is
	// there anywhere a hand can put this number". A stub does not fail those guards, it EMPTIES them:
	// a card that addresses nothing trivially addresses nothing it should not, so the case that would
	// have found two unreachable skew weights and a percentage wired to a toggle had nothing to look
	// at. An instrument wired to return "no findings" is worse than no instrument, because it reads
	// like one.
	std::vector<lfo::Param> reached;
	reached.reserve(kBoxCount + kChoiceCount + kChipCount + 1);
	for (const lfo::Param param : layout()) {
		reached.push_back(param);
	}
	for (const lfo::Param param : choices()) {
		reached.push_back(param);
	}
	for (const Chip& chip : chips()) {
		reached.push_back(chip.param);
	}
	// THE ENABLE SQUARE IS THE HEAD'S, not a chip of this card's — `FxCard::enablePressAt` answers it
	// for every device — but it IS reachable, and a sweep asking "what can a hand touch here" would be
	// wrong to leave it out.
	reached.push_back(lfo::Param::Enabled);
	// AND THE TRIG, ON ITS OWN PAD (backlog MV).
	reached.push_back(lfo::Param::Trig);
	// AND ITS NORMAL, ON THE SAME PAD'S RIGHT-CLICK (backlog NU).
	reached.push_back(lfo::Param::NotesTrig);
	return reached;
}

std::string LfoCard::boxCaption(size_t index) const {
	return index < kBoxCount ? kCaptions[index] : "";
}

const visage::Frame* LfoCard::boxFrame(size_t index) const {
	return index < kBoxCount ? boxes_[index].get() : nullptr;
}

void LfoCard::bindBox(size_t index) {
	ValueBox* box = boxes_[index].get();
	box->onValueChanged([this]() { redraw(); });
	box->onEdited([](double, double) {});
}

void LfoCard::resized() {
	// ONE ANSWER FOR WHERE A BOX IS — `boxBounds` — rather than the placement being computed here and
	// the hit-testing computing it again. The card that drew its chips from one arithmetic and tested
	// them against another is the bug this shape cannot have.
	for (size_t i = 0; i < kBoxCount; ++i) {
		// A BOX WHOSE CONTROL DOES NOTHING IN THIS WAVEFORM IS NOT DRAWN — the honesty rule, which the
		// tooltips stated and nothing enforced. Still placed, so that a card whose gate changes while
		// it is folded comes back with its bounds already right.
		const visage::Bounds b = boxBounds(i);
		boxes_[i]->setBounds(b.x(), b.y(), b.width(), b.height());
		boxes_[i]->setVisible(!folded_ && boxIsLive(i));
	}
	for (size_t i = 0; i < kChoiceCount; ++i) {
		seatInChipCell(*choiceBoxes_[i], choiceBounds(i));
		choiceBoxes_[i]->setVisible(!folded_);
	}
	const visage::Bounds trig = trigPadBounds();
	trigPad_->setBounds(trig.x(), trig.y(), trig.width(), trig.height());
	trigPad_->setVisible(!folded_);
	gateDrawn_ = gateValue();
}

float LfoCard::gateValue() const {
	return host_.fieldAt(device_, static_cast<size_t>(lfo::Param::Waveform));
}

bool LfoCard::headSourceFocused() const {
	const device::Descriptor* desc = descriptor();
	const size_t outputs = desc != nullptr ? desc->outputCount() : 0;
	for (size_t out = 0; out < outputs; ++out) {
		if (mod_.source() == sourceOfOutput(out)) {
			return true;
		}
	}
	return false;
}

bool LfoCard::headArmed() const {
	return headSourceFocused() && mod_.armed();
}

void LfoCard::foldedChanged() {
	// THROUGH `boxIsLive`, NOT A BARE `!folded_`. Unfolding used to show every box, including the ones
	// the current waveform has nothing for — so a fold and an unfold was a way to bring back a control
	// the gate had just hidden, and it would stay back until the next layout.
	for (size_t i = 0; i < kBoxCount; ++i) {
		if (boxes_[i] != nullptr) {
			boxes_[i]->setVisible(!folded_ && boxIsLive(i));
		}
	}
	for (const auto& box : choiceBoxes_) {
		if (box != nullptr) {
			box->setVisible(!folded_);
		}
	}
	if (trigPad_ != nullptr) {
		trigPad_->setVisible(!folded_);
	}
}

const char* LfoCard::contentTooltipAt(visage::Point local) const {
	// THE CHIPS ARE DRAWN BY THIS CARD, not seated as widgets — the press road's own hit test.
	if (folded()) {
		return nullptr;
	}
	for (size_t chip = 0; chip < kChipCount; ++chip) {
		if (chipBounds(chip).contains(local)) {
			return controlTooltip(static_cast<size_t>(chips()[chip].param));
		}
	}
	return nullptr;
}

device::Modulatable* LfoCard::enableModulatable() {
	if (!hasEnable()) {
		return nullptr;
	}
	if (enableControl_ == nullptr) {
		enableControl_ = std::make_unique<DeviceControl>(host_, device_, mod::lfo::kDescriptor,
		                                                enableControl(), &mod_, &history_);
	}
	return enableControl_.get();
}

} // namespace feathers::ui
