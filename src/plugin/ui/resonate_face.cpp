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

#include "plugin/ui/resonate_face.h"

#include "plugin/ui/generic_card.h"
#include "plugin/ui/theme.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdlib>

namespace feathers::ui {

namespace rs = fx::resonate;
using P = rs::Param;

namespace {

struct Named {
	P param;
	const char* caption;
};

/// THE CARD'S SIX, IN THE DESIGNED ORDER — one row.
constexpr Named kCardKnobs[] = { { P::Drive, "Drive" }, { P::Size, "Size" },       { P::Pluck, "Pluck" },
	                             { P::Blend, "Blend" }, { P::Sustain, "Sustain" }, { P::Mix, "Mix" } };

/// THE PANEL'S KNOBS, BY WHERE THEY SIT.
constexpr Named kExciteKnobs[] = { { P::Drive, "Drive" }, { P::Bow, "Bow" } };
/// THE TWO STACKS UNDER EXCITE (v21: all the pluck/jawari controls in two stacks of knobs to the left): how a string is
/// plucked and what it strikes as it swings, the NOTES' (with the ring's: Jawari, Curve and Bounce reach it too) beside the
/// DRONE's, row for row. Curve shapes only the ring's bridge (the strings' point has no shape); in its row the drone's stack has
/// Thread (v21): where along its strings the point sits.
constexpr Named kNotesStack[] = { { P::Pluck, "Pluck" }, { P::Jawari, "Jawari" }, { P::BridgeCurve, "Curve" }, { P::Bounce, "Bounce" },
	                              { P::Sympathy, "Sympathy" } };
constexpr Named kDroneStack[] = { { P::DronePluck, "Pluck" }, { P::DroneJawari, "Jawari" }, { P::DroneThread, "Thread" }, { P::DroneBounce, "Bounce" },
	                              { P::DroneSympathy, "Sympathy" } };
/// WHICH ROW EACH OF THE DRONE'S STACK SITS IN (row for row with the notes').
constexpr int kDroneStackRow[] = { 0, 1, 2, 3, 4 };
static_assert(std::size(kDroneStackRow) == std::size(kDroneStack));
/// THE DRONE ROW, after its chord unit (the Phase box over the Chord box): how hard and how the input reaches the strings, their
/// note, their two times, their width.
constexpr Named kDroneKnobs[] = { { P::DroneInput, "Drive" }, { P::DroneExcite, "Excite" }, { P::Root, "Root" }, { P::DroneAttack, "Attack" },
	                              { P::DroneDecay, "Decay" },     { P::DroneWidth, "Width" } };
constexpr Named kFilterKnobs[] = { { P::Size, "Size" }, { P::Width, "Width" } };
/// OUT — where the signal leaves, the drone's own output among them, and the gourds the drone sounds
/// through beside it (moved here 2026-09-28, to give the drone's second row room for the Cycle's division box).
constexpr Named kOutKnobs[] = { { P::Blend, "Blend" }, { P::Level, "Level" }, { P::Mix, "Mix" },
	                            { P::DroneOut, "Drone" }, { P::DroneBody, "Body" }, { P::DroneTwin, "Twin" } };
/// THE PANEL'S VALUE BOXES — Porta beside its gate, then the offset every head shares, and Sustain by them; then the
/// drone's Chord (its Tuning chip under it).
constexpr Named kToneBoxes[] = { { P::Porta, "Porta" },   { P::Octave, "Octave" }, { P::Semitone, "Semi" },
	                             { P::Fine, "Fine" },     { P::Sustain, "Sustain" }, { P::DroneChord, "Chord" },
	                             { P::DroneCycleSync, "Rate" }, { P::RingDecay, "Ring Decay" },
	                             { P::Tension, "Tension" }, { P::DroneCycleLevel, "Level" },
	                             { P::StringBlend, "String" }, { P::StringExcite, "Excite" }, { P::Pickup, "Pickup" },
	                             { P::BridgeFixed, "Fixed" } };
/// THE DRONE'S SECOND ROW, after its two zone cells (Drone Character, Cycle) and the Cycle's division box: the voice —
/// what it is and how stiff its strings.
constexpr Named kDroneRow2Knobs[] = { { P::DroneVoice, "Voice" }, { P::DroneStiffness, "Stiff" } };
/// THE DRONE'S DEPENDANTS — what its output level being 0 makes inert, and greys. Not Body or Twin: since v11 the
/// gourds colour the notes' strings too, so they are live with the drone silent (Twin needs a Body, below). Not Sympathy
/// (v18): the notes' strings ride the same yielding bridge, so its lightness moves them with the drone silent.
constexpr P kDroneDependants[] = { P::DroneCharacter, P::DroneChord, P::DroneInput,  P::DroneExcite, P::Root,
	                               P::DroneAttack,    P::DroneDecay, P::DroneWidth,  P::DroneCycle,  P::DroneCycleSync,
	                               P::DroneVoice,     P::DroneStiffness, P::DroneCycleLevel, P::DronePluck, P::DroneJawari,
	                               P::DroneBounce, P::DroneThread, P::DroneFollow };

/// THE CARD'S STRING VIEW: a little wider than tall, and the card's largest thing.
constexpr float kCardRingAspect = 0.7f;
constexpr float kCardRingMin = 130.0f;
constexpr float kCardRingMax = 200.0f;

/// THE PANEL'S COLUMNS: EXCITE, the string, and one column to its right — TONE, DRONE, then FILTER and OUT side by
/// side under them (no open space under the drone: FILTER and OUT moved left and down).
constexpr float kPanelExciteWidth = 144.0f; // 96, 112, 124 until v21: two stacks of knobs side by side, "SYMPATHY" needs ~70 px each
constexpr float kPanelToneWidth = 402.0f; // 352 until 2026-09-28: the drone row's sixth knob (Input) at 50
/// THE DRONE ROW: the chord unit (the Phase and Chord boxes stacked) and the six knobs.
constexpr float kDroneUnitWidth = 102.0f;
/// THE DRONE'S TWO ZONE CELLS in its second row.
constexpr float kDroneCellWidth = 76.0f;
/// THE DRONE CHARACTER'S CELL, wider: its caption is two words (renamed 2026-09-28, clear of the string view's Character).
constexpr float kDroneCharacterWidth = 108.0f;
/// THE CYCLE'S DIVISION BOX, beside its cell.
constexpr float kCycleRateWidth = 64.0f;
constexpr float kPanelGap = 16.0f;
constexpr float kPanelRingMin = 300.0f;
/// EXCITE'S KNOBS UNDER THE STRING VIEW (v22): Drive and Bow, each this wide (their captions read at 50).
constexpr float kExciteKnobWidth = 60.0f;

} // namespace

ResonateFace::ResonateFace(Surface surface, backplane::DeviceHost& host, DeviceInstanceId id, WidgetRegistry& registry,
                           Notifier* notifier, visage::Frame& parent)
    : surface_(surface), host_(host), device_(id) {
	if (surface_ == Surface::Card) {
		for (const Named& n : kCardKnobs) {
			addKnob(n.param, n.caption, kCardKnobDiameter, notifier, parent);
		}
	}
	else {
		for (const auto& group : { std::span<const Named>(kExciteKnobs), std::span<const Named>(kNotesStack), std::span<const Named>(kDroneStack), std::span<const Named>(kDroneKnobs), std::span<const Named>(kDroneRow2Knobs),
		                           std::span<const Named>(kFilterKnobs), std::span<const Named>(kOutKnobs) }) {
			for (const Named& n : group) {
				addKnob(n.param, n.caption, kPanelKnobDiameter, notifier, parent);
			}
		}
		for (const Named& n : kToneBoxes) {
			addBox(n.param, n.caption, parent);
		}
		// DRONE DETAIL (backlog PS): a value box and its menu, all four choices seen before one is made. In the window only: the card is
		// the simplified face (the string, the pictograms, six knobs; its own case), and the window may add what the card lacks.
		addBox(P::DroneDetail, "Detail", parent);
		// BOTH STATES ON THE FACE — Poly | Mono — so the choice is seen before it is made.
		voicing_ = std::make_unique<SegmentedControl>(control(P::Voicing), registry, rs::kVoicingNames, notifier);
		parent.addChild(voicing_.get());
		meter_ = std::make_unique<ResonateMeter>(host_, device_, control(P::Pluck));
		parent.addChild(meter_.get());
		// THE DRONE'S TWO ZONED COORDINATES, the house cell (a value box, its caption, its zone): Character, and the
		// Cycle, whose zones name its patterns.
		droneCharacter_ = std::make_unique<ZoneCoordCell>(control(P::DroneCharacter), "Drone Character",
		                                                  GenericCard::zoneNamesFor(rs::kParamDescs[static_cast<size_t>(P::DroneCharacter)]),
		                                                  ZoneSelect::kCoordinatePerLap);
		droneCharacter_->setFaceDiameter(kPanelKnobDiameter);
		parent.addChild(droneCharacter_.get());
		cycle_ = std::make_unique<ZoneCoordCell>(control(P::DroneCycle), "Cycle",
		                                         GenericCard::zoneNamesFor(rs::kParamDescs[static_cast<size_t>(P::DroneCycle)]),
		                                         ZoneSelect::kCoordinatePerLap);
		cycle_->setFaceDiameter(kPanelKnobDiameter);
		parent.addChild(cycle_.get());
		// TWO-STATE CHOICES AS CHIPS (the multivalue rule): Note Release (lit: Ring) and Tuning (lit: Just).
		ringChip_ = std::make_unique<SwitchChip>(control(P::HeadRelease), "Ring");
		parent.addChild(ringChip_.get());
		tuningChip_ = std::make_unique<SwitchChip>(control(P::DroneTuning), "Just");
		transportChip_ = std::make_unique<SwitchChip>(control(P::DroneCycleTransport), "Transport");
		// FOLLOW (v21): whether the drone's scanned voices swell with the main string, stacked under Transport, beside Voice.
		followChip_ = std::make_unique<SwitchChip>(control(P::DroneFollow), "Follow");
		parent.addChild(followChip_.get());
		parent.addChild(transportChip_.get());
		parent.addChild(tuningChip_.get());
	}
	picker_ = std::make_unique<ResonateModelPicker>(control(P::Model));
	parent.addChild(picker_.get());
	ring_ = std::make_unique<ResonateRingView>(host_, device_, control(P::Character), control(P::Morph));
	parent.addChild(ring_.get());
	// CHARACTER AND MORPH AS BOXES, AFTER THE VIEW so they paint over its bands — the modulation road for the
	// two controls the view drags (a picture is not a mod destination; a box is).
	characterBox_ = std::make_unique<ValueBox>(control(P::Character), "Character");
	characterBox_->setCompact(true);
	parent.addChild(characterBox_.get());
	morphBox_ = std::make_unique<ValueBox>(control(P::Morph), "Morph");
	morphBox_->setCompact(true);
	parent.addChild(morphBox_.get());
}

void ResonateFace::addKnob(P p, const char* caption, float diameter, Notifier* notifier, visage::Frame& parent) {
	auto k = std::make_unique<Knob>(control(p), caption, notifier);
	k->setFaceDiameter(diameter);
	parent.addChild(k.get());
	knobs_.push_back({ p, std::move(k) });
}

void ResonateFace::addBox(P p, const char* caption, visage::Frame& parent) {
	auto b = std::make_unique<ValueBox>(control(p), caption);
	parent.addChild(b.get());
	boxes_.push_back({ p, std::move(b) });
}

DeviceControl& ResonateFace::control(P p) const {
	const size_t i = static_cast<size_t>(p);
	if (controls_[i] == nullptr) {
		controls_[i] = std::make_unique<DeviceControl>(host_, device_, rs::kDescriptor, i, mod_, history_);
	}
	return *controls_[i];
}

void ResonateFace::setModulation(ModState* mod, EditHistory* history) {
	mod_ = mod;
	history_ = history;
	for (KnobSlot& k : knobs_) {
		k.knob->setModulation(mod, history);
	}
	for (size_t i = 0; i < static_cast<size_t>(P::kCount); ++i) {
		control(static_cast<P>(i)).setModulation(mod, history);
	}
}

Knob& ResonateFace::knob(P p) const {
	for (const KnobSlot& k : knobs_) {
		if (k.param == p) {
			return *k.knob;
		}
	}
	// A MISS IS A LAYOUT ASKING FOR A CONTROL THIS SURFACE NEVER BUILT — a programming error in the tables
	// above, and it stops here rather than quietly placing some other knob where this one should be.
	assert(false && "ResonateFace::knob: no knob for this control on this surface");
	std::abort();
}

ValueBox& ResonateFace::box(P p) const {
	for (const BoxSlot& b : boxes_) {
		if (b.param == p) {
			return *b.box;
		}
	}
	// SEE `knob`.
	assert(false && "ResonateFace::box: no value box for this control on this surface");
	std::abort();
}

bool ResonateFace::isLive(P p) const {
	// THE CYCLE'S PATTERN DOES NOTHING WHILE IT IS NOT RUNNING (Rate 0), whatever the drone.
	if (p == P::DroneCycle && !(host_.fieldAt(device_, static_cast<size_t>(P::DroneCycleSync)) >= 0.5f)) {
		return false;
	}
	// WHAT PLAYS THE NOTES' STRINGS DOES NOTHING WITHOUT THEM.
	if (p == P::StringExcite || p == P::Pickup) {
		return host_.fieldAt(device_, static_cast<size_t>(P::StringBlend)) > 0.0f;
	}
	// DRONE EXCITE CHOOSES HOW THE INPUT REACHES THE DRONE'S STRINGS, so at Drone Drive 0 (none reaches them) it does nothing
	// (v20: the engine's feed is heard = Drone Drive x the input, before the ears and the blend Excite sets).
	// DRONE BOUNCE AND THREAD ARE THE DRONE STRINGS' BRIDGE'S (v21), so with no Drone Jawari there is none.
	// FOLLOW GATES THE SCANNED VOICES (v21), so at Voice 0 (none) it does nothing.
	if (p == P::DroneFollow) {
		return host_.fieldAt(device_, static_cast<size_t>(P::DroneOut)) > 0.0f && host_.fieldAt(device_, static_cast<size_t>(P::DroneVoice)) > 0.0f;
	}
	if (p == P::DroneBounce || p == P::DroneThread) {
		return host_.fieldAt(device_, static_cast<size_t>(P::DroneOut)) > 0.0f && host_.fieldAt(device_, static_cast<size_t>(P::DroneJawari)) > 0.0f;
	}
	if (p == P::DroneExcite) {
		return host_.fieldAt(device_, static_cast<size_t>(P::DroneOut)) > 0.0f && host_.fieldAt(device_, static_cast<size_t>(P::DroneInput)) > 0.0f;
	}
	// TWIN SPREADS THE BODY'S PEAKS APART, so with no body there is nothing to spread.
	if (p == P::DroneTwin) {
		return host_.fieldAt(device_, static_cast<size_t>(P::DroneBody)) > 0.0f;
	}
	if (p == P::BridgeCurve || p == P::Bounce || p == P::BridgeFixed) {
		return host_.fieldAt(device_, static_cast<size_t>(P::Jawari)) > 0.0f;
	}
	if (std::find(std::begin(kDroneDependants), std::end(kDroneDependants), p) != std::end(kDroneDependants)) {
		return host_.fieldAt(device_, static_cast<size_t>(P::DroneOut)) > 0.0f;
	}
	const device::ParamDesc& d = rs::kParamDescs[static_cast<size_t>(p)];
	if (d.gateControl < 0) {
		return true;
	}
	return device::relevantFor(d, host_.fieldAt(device_, static_cast<size_t>(d.gateControl)));
}

//------------------------------------------------------------------------------------------------//
// LAYOUT                                                                                           //
//------------------------------------------------------------------------------------------------//

void ResonateFace::caption(const char* text, float x, float y, float width) {
	captions_.push_back({ text, { x, y, width, kCaptionHeight } });
}

void ResonateFace::placeAxisBoxes(bool visible) {
	// OVER THE VIEW'S OWN BANDS, in the parent's coordinates — the view says where, the face seats.
	const visage::Bounds c = ring_->characterBand();
	const visage::Bounds m = ring_->morphBand();
	const float x = static_cast<float>(ring_->x());
	const float y = static_cast<float>(ring_->y());
	characterBox_->setBounds(x + c.x(), y + c.y(), c.width(), c.height());
	characterBox_->setVisible(visible);
	morphBox_->setBounds(x + m.x(), y + m.y(), m.width(), m.height());
	morphBox_->setVisible(visible);
}

float ResonateFace::cardRingHeight(float width) {
	return std::clamp(width * kCardRingAspect, kCardRingMin, kCardRingMax);
}

float ResonateFace::cardHeight(float width) {
	return kPickerHeight + kRowGap + cardRingHeight(width) + kRowGap + kCardKnobRows * kCardKnobHeight
	       + (kCardKnobRows - 1.0f) * kBoxGap;
}

float ResonateFace::layOutCard(float left, float top, float width, bool visible) {
	captions_.clear();
	float y = top;
	// THE MODEL, THEN THE STRING — the card's heart — then its six knobs in one row.
	picker_->setBounds(left, y, width, kPickerHeight);
	picker_->setVisible(visible);
	y += kPickerHeight + kRowGap;
	const float ringH = cardRingHeight(width);
	ring_->setBounds(left, y, width, ringH);
	ring_->setVisible(visible);
	placeAxisBoxes(visible);
	y += ringH + kRowGap;
	// TWO ROWS OF THREE, READ ACROSS: one row of six ellipsised every caption at the lane's width
	// ("DRI…", "SUS…" — photographed), and a caption cut to three letters is a knob nobody can name.
	const size_t perRow = std::size(kCardKnobs) / static_cast<size_t>(kCardKnobRows);
	const float cell = width / static_cast<float>(perRow);
	for (size_t i = 0; i < std::size(kCardKnobs); ++i) {
		Knob& k = knob(kCardKnobs[i].param);
		const float row = static_cast<float>(i / perRow);
		k.setBounds(left + cell * static_cast<float>(i % perRow), y + row * (kCardKnobHeight + kBoxGap), cell,
		            kCardKnobHeight);
		k.setVisible(visible);
	}
	return y + kCardKnobRows * kCardKnobHeight + (kCardKnobRows - 1.0f) * kBoxGap;
}

visage::Point ResonateFace::panelMinimum() {
	const float big = kPanelKnobHeight;
	const float excite = kCaptionHeight + static_cast<float>(std::size(kNotesStack)) * big + kBoxGap
	                     + ValueBox::kHeight; // the two stacks (v21), Fixed (v13) beside Drone Detail (PS); EXCITE is under the string view (v22)
	const float tone = kCaptionHeight + 2.0f * ValueBox::kHeight + kBoxGap + kRowGap + 2.0f * (kCaptionHeight + big) + kRowGap
	                   + big + kRowGap; // the drone's second row
	const float string = kPickerHeight + kRowGap + kPanelRingMin + kRowGap + big; // + EXCITE's row (v22)
	return { kPanelExciteWidth + kPanelGap + kPanelRingMin + kPanelGap + kPanelToneWidth,
		     std::max({ excite, tone, string }) };
}

void ResonateFace::layOutPanel(visage::Bounds area) {
	captions_.clear();
	const float top = area.y();
	const float bottom = area.bottom();
	const float big = kPanelKnobHeight;

	// THE LEFT COLUMN IS THE TWO STACKS (v22, because the plugin had grown too tall): the notes' on the left, the drone's on the right,
	// row for row, Fixed under them. EXCITE (the meter, Drive and Bow) went under the string view, where it adds no height.
	const float stacksTop = top;
	const float stackW = kPanelExciteWidth * 0.5f;
	caption("NOTES", area.x(), stacksTop, stackW);
	caption("DRONE", area.x() + stackW, stacksTop, stackW);
	const float stackY = stacksTop + kCaptionHeight;
	for (size_t i = 0; i < std::size(kNotesStack); ++i) {
		Knob& k = knob(kNotesStack[i].param);
		k.setBounds(area.x(), stackY + big * static_cast<float>(i), stackW, big);
		k.setVisible(true);
	}
	for (size_t i = 0; i < std::size(kDroneStack); ++i) {
		Knob& k = knob(kDroneStack[i].param);
		k.setBounds(area.x() + stackW, stackY + big * static_cast<float>(kDroneStackRow[i]), stackW, big);
		k.setVisible(true);
	}
	knob(P::BridgeCurve).setEnabled(isLive(P::BridgeCurve));
	knob(P::Bounce).setEnabled(isLive(P::Bounce));
	// AND UNDER THEM, BRIDGE FIXED (v13): whether the notes' bridge follows how hard their strings are played.
	// FIXED UNDER THE NOTES' STACK, DRONE DETAIL (backlog PS: how many nodes each drone string has) under the drone's, row for row.
	const float underY = stackY + static_cast<float>(std::size(kNotesStack)) * big + kBoxGap;
	box(P::BridgeFixed).setBounds(area.x(), underY, stackW - 0.5f * kBoxGap, ValueBox::kHeight);
	box(P::BridgeFixed).setVisible(true);
	box(P::BridgeFixed).setEnabled(isLive(P::BridgeFixed));
	box(P::DroneDetail).setBounds(area.x() + stackW + 0.5f * kBoxGap, underY, stackW - 0.5f * kBoxGap, ValueBox::kHeight);
	box(P::DroneDetail).setVisible(true);

	// THE STRING — everything the side columns leave, the pictograms along its top.
	const float midX = area.x() + kPanelExciteWidth + kPanelGap;
	const float sides = kPanelExciteWidth + kPanelToneWidth + 2.0f * kPanelGap;
	const float midW = std::max(0.0f, area.width() - sides);
	picker_->setBounds(midX, top, std::min(midW, 300.0f), kPickerHeight);
	picker_->setVisible(true);
	const float ringTop = top + kPickerHeight + kRowGap;
	// EXCITE UNDER THE STRING VIEW (v22): the input meter, then Drive and Bow, one knob-row at the bottom of the middle column.
	const float exciteTop = std::max(ringTop, bottom - big);
	ring_->setBounds(midX, ringTop, midW, std::max(0.0f, exciteTop - kRowGap - ringTop));
	meter_->setBounds(midX, exciteTop, kMeterWidth, big);
	meter_->setVisible(true);
	for (size_t i = 0; i < std::size(kExciteKnobs); ++i) {
		Knob& k = knob(kExciteKnobs[i].param);
		k.setBounds(midX + kMeterWidth + kRowGap + kExciteKnobWidth * static_cast<float>(i), exciteTop, kExciteKnobWidth, big);
		k.setVisible(true);
	}
	ring_->setVisible(true);
	placeAxisBoxes(true);

	// TONE, THEN DRONE — what the heads do, then the sympathetic string under them.
	const float tx = midX + midW + kPanelGap;
	caption("TONE", tx, top, kPanelToneWidth);
	float y = top + kCaptionHeight;
	const float half = std::floor((kPanelToneWidth - kBoxGap) * 0.5f);
	const float row = ValueBox::kHeight;
	// POLY | MONO, THEN THE NOTE RELEASE CHIP (Gate / Ring), then Porta.
	constexpr float kRingChipWidth = 44.0f;
	voicing_->setBounds(tx, y + (row - kSegmentHeight) * 0.5f, half - kRingChipWidth - kBoxGap, kSegmentHeight);
	voicing_->setVisible(true);
	ringChip_->setBounds(tx + half - kRingChipWidth, y + (row - SwitchChip::kHeight) * 0.5f, kRingChipWidth, SwitchChip::kHeight);
	ringChip_->setVisible(true);
	// PORTA, THEN THE NOTES' OWN STRINGS (v10): how much of each note is its string, what plays it, and where it is heard
	// (Pickup, v11).
	const float third = (kPanelToneWidth - half - 4.0f * kBoxGap) / 4.0f; // a quarter since Pickup (v11)
	box(P::Porta).setBounds(tx + half + kBoxGap, y, third, row);
	box(P::Porta).setVisible(isLive(P::Porta));
	box(P::StringBlend).setBounds(tx + half + 2.0f * kBoxGap + third, y, third, row);
	box(P::StringBlend).setVisible(true);
	box(P::StringExcite).setBounds(tx + half + 3.0f * kBoxGap + 2.0f * third, y, third, row);
	box(P::StringExcite).setVisible(true);
	box(P::StringExcite).setEnabled(isLive(P::StringExcite));
	box(P::Pickup).setBounds(tx + half + 4.0f * kBoxGap + 3.0f * third, y, third, row);
	box(P::Pickup).setVisible(true);
	box(P::Pickup).setEnabled(isLive(P::Pickup));
	y += row + kBoxGap;
	// AND RING DECAY BESIDE SUSTAIN (2026-09-28): how long the string the notes read rings; and TENSION, how fast it moves.
	const P shared[] = { P::Octave, P::Semitone, P::Fine, P::Sustain, P::RingDecay, P::Tension };
	const float quarter = (kPanelToneWidth - 5.0f * kBoxGap) / 6.0f;
	for (size_t i = 0; i < std::size(shared); ++i) {
		box(shared[i]).setBounds(tx + (quarter + kBoxGap) * static_cast<float>(i), y, quarter, row);
		box(shared[i]).setVisible(true);
	}
	y += row + kRowGap;
	caption("DRONE", tx, y, kPanelToneWidth);
	y += kCaptionHeight;
	// ROW ONE: THE CHORD UNIT — the Chord that tunes the strings over the Tuning chip that says how exactly — then the
	// six knobs sharing what is left.
	const float unitGap = std::max(0.0f, (big - row - SwitchChip::kHeight) / 3.0f);
	box(P::DroneChord).setBounds(tx, y + unitGap, kDroneUnitWidth, row);
	box(P::DroneChord).setVisible(true);
	tuningChip_->setBounds(tx, y + 2.0f * unitGap + row, kDroneUnitWidth, SwitchChip::kHeight);
	tuningChip_->setVisible(true);
	const float droneKnobW = (kPanelToneWidth - kDroneUnitWidth) / static_cast<float>(std::size(kDroneKnobs));
	for (size_t i = 0; i < std::size(kDroneKnobs); ++i) {
		Knob& k = knob(kDroneKnobs[i].param);
		k.setBounds(tx + kDroneUnitWidth + droneKnobW * static_cast<float>(i), y, droneKnobW, big);
		k.setVisible(true);
	}
	y += big + kRowGap;
	// ROW TWO: THE TWO ZONED COORDINATES (Drone Character, Cycle) wide enough for their captions and zone names
	// (MEASURED on a still: at 50 px "CHARACTER" and "Tanpura" were cut), the Cycle's division box beside its cell, then
	// the voice's two knobs.
	droneCharacter_->setBounds(tx, y, kDroneCharacterWidth, big);
	droneCharacter_->setVisible(true);
	cycle_->setBounds(tx + kDroneCharacterWidth, y, kDroneCellWidth, big);
	cycle_->setVisible(true);
	const float rateX = tx + kDroneCharacterWidth + kDroneCellWidth;
	// THE CYCLE'S RATE OVER ITS LEVEL, one unit beside its cell (Cycle Level, 2026-09-28).
	const float rateGap = std::max(0.0f, (big - 2.0f * row) / 3.0f);
	box(P::DroneCycleSync).setBounds(rateX + kBoxGap, y + rateGap, kCycleRateWidth - 2.0f * kBoxGap, row);
	box(P::DroneCycleSync).setVisible(true);
	box(P::DroneCycleLevel).setBounds(rateX + kBoxGap, y + 2.0f * rateGap + row, kCycleRateWidth - 2.0f * kBoxGap, row);
	box(P::DroneCycleLevel).setVisible(true);
	// AND THE CYCLE'S TRANSPORT SWITCH BESIDE THEM (v11): whether it plucks only while the host plays.
	constexpr float kTransportChipWidth = 54.0f; // the two knobs after it keep 50 px, their captions' width
	// TRANSPORT OVER FOLLOW (v21), the two chips in one column, Follow beside the Voice it gates.
	const float chipGap = std::max(0.0f, (big - 2.0f * SwitchChip::kHeight) / 3.0f);
	transportChip_->setBounds(rateX + kCycleRateWidth, y + chipGap, kTransportChipWidth - kBoxGap, SwitchChip::kHeight);
	transportChip_->setVisible(true);
	followChip_->setBounds(rateX + kCycleRateWidth, y + 2.0f * chipGap + SwitchChip::kHeight, kTransportChipWidth - kBoxGap,
	                       SwitchChip::kHeight);
	followChip_->setVisible(true);
	const float row2X = rateX + kCycleRateWidth + kTransportChipWidth;
	const float knob2W = (tx + kPanelToneWidth - row2X) / static_cast<float>(std::size(kDroneRow2Knobs));
	for (size_t i = 0; i < std::size(kDroneRow2Knobs); ++i) {
		Knob& k = knob(kDroneRow2Knobs[i].param);
		k.setBounds(row2X + knob2W * static_cast<float>(i), y, knob2W, big);

		k.setVisible(true);
	}
	y += big + kRowGap;
	applyDroneEnable();


	// FILTER AND OUT, SIDE BY SIDE UNDER THE DRONE — eight knobs across the column, OUT last, where the signal leaves.
	const float sixth = kPanelToneWidth / 8.0f; // an eighth now: two FILTER, six OUT
	caption("FILTER", tx, y, 2.0f * sixth);
	caption("OUT", tx + 2.0f * sixth, y, 6.0f * sixth);
	y += kCaptionHeight;
	for (size_t i = 0; i < std::size(kFilterKnobs); ++i) {
		Knob& k = knob(kFilterKnobs[i].param);
		k.setBounds(tx + sixth * static_cast<float>(i), y, sixth, big);
		k.setVisible(true);
	}
	for (size_t i = 0; i < std::size(kOutKnobs); ++i) {
		Knob& k = knob(kOutKnobs[i].param);
		k.setBounds(tx + sixth * static_cast<float>(2 + i), y, sixth, big);
		k.setVisible(true);
		k.setEnabled(isLive(kOutKnobs[i].param));
	}
}

void ResonateFace::applyDroneEnable() {
	if (surface_ != Surface::Panel) {
		return;
	}
	for (const P p : kDroneDependants) {
		const bool live = isLive(p);
		if (p == P::DroneCharacter) {
			droneCharacter_->setEnabled(live);
		}
		else if (p == P::DroneFollow) {
			followChip_->setEnabled(live);
		}
		else if (p == P::DroneCycle) {
			cycle_->setEnabled(live);
		}
		else if (p == P::DroneChord || p == P::DroneCycleSync || p == P::DroneCycleLevel) {
			box(p).setEnabled(live);
		}
		else {
			knob(p).setEnabled(live);
		}
	}
}


//------------------------------------------------------------------------------------------------//
// REFRESH                                                                                          //
//------------------------------------------------------------------------------------------------//

ResonateFace::Changes ResonateFace::refresh() {
	Changes c;
	if (surface_ == Surface::Panel) {
		const int voicing = static_cast<int>(std::lround(host_.fieldAt(device_, static_cast<size_t>(P::Voicing))));
		if (voicing != drawnVoicing_) {
			drawnVoicing_ = voicing;
			c.gates = true;
		}
		const int droneOn = host_.fieldAt(device_, static_cast<size_t>(P::DroneOut)) > 0.0f ? 1 : 0;
		if (droneOn != drawnDroneOn_) {
			drawnDroneOn_ = droneOn;
			applyDroneEnable();
		}
		const int cycleOn = isLive(P::DroneCycle) ? 1 : 0;
		if (cycleOn != drawnCycleOn_) {
			drawnCycleOn_ = cycleOn;
			applyDroneEnable();
		}
		const int voiceOn = host_.fieldAt(device_, static_cast<size_t>(P::DroneVoice)) > 0.0f ? 1 : 0;
		if (voiceOn != drawnVoiceOn_) {
			drawnVoiceOn_ = voiceOn;
			applyDroneEnable();
		}
		const int droneBridgeOn = host_.fieldAt(device_, static_cast<size_t>(P::DroneJawari)) > 0.0f ? 1 : 0;
		if (droneBridgeOn != drawnDroneBridgeOn_) {
			drawnDroneBridgeOn_ = droneBridgeOn;
			applyDroneEnable();
		}
		const int driveOn = host_.fieldAt(device_, static_cast<size_t>(P::DroneInput)) > 0.0f ? 1 : 0;
		if (driveOn != drawnDriveOn_) {
			drawnDriveOn_ = driveOn;
			applyDroneEnable();
		}
		const int bridgeOn = isLive(P::BridgeCurve) ? 1 : 0;
		if (bridgeOn != drawnBridgeOn_) {
			drawnBridgeOn_ = bridgeOn;
			knob(P::BridgeCurve).setEnabled(bridgeOn != 0);
			knob(P::Bounce).setEnabled(bridgeOn != 0);
			box(P::BridgeFixed).setEnabled(bridgeOn != 0);
		}
		const int twinOn = isLive(P::DroneTwin) ? 1 : 0;
		if (twinOn != drawnTwinOn_) {
			drawnTwinOn_ = twinOn;
			knob(P::DroneTwin).setEnabled(twinOn != 0);
		}
		const int stringOn = isLive(P::StringExcite) ? 1 : 0;
		if (stringOn != drawnStringOn_) {
			drawnStringOn_ = stringOn;
			box(P::StringExcite).setEnabled(stringOn != 0);
			box(P::Pickup).setEnabled(stringOn != 0);
		}
		if (meter_->refresh()) {
			c.meter = true;
		}
	}
	const int model = static_cast<int>(std::lround(host_.fieldAt(device_, static_cast<size_t>(P::Model))));
	if (model != drawnModel_) {
		drawnModel_ = model;
		picker_->redraw();
		c.picture = true;
	}
	if (ring_->refresh()) {
		c.picture = true;
	}
	return c;
}

//------------------------------------------------------------------------------------------------//
// DRAWING                                                                                          //
//------------------------------------------------------------------------------------------------//

void ResonateFace::drawCaptions(visage::Canvas& canvas) const {
	canvas.setColor(kMuted);
	for (const Caption& c : captions_) {
		drawTracked(canvas, c.text, font(kKnobLabelSize), kKnobLabelTracking, c.box.x(), c.box.y(), c.box.height());
	}
}

//------------------------------------------------------------------------------------------------//
// FOR TESTS                                                                                        //
//------------------------------------------------------------------------------------------------//

std::vector<P> ResonateFace::addressedParams() const {
	std::vector<P> out;
	for (const KnobSlot& k : knobs_) {
		out.push_back(k.param);
	}
	for (const BoxSlot& b : boxes_) {
		out.push_back(b.param);
	}
	if (voicing_ != nullptr) {
		out.push_back(P::Voicing);
		out.push_back(P::DroneCharacter);
		out.push_back(P::DroneCycle);
		out.push_back(P::HeadRelease);
		out.push_back(P::DroneTuning);
		out.push_back(P::DroneCycleTransport);
		out.push_back(P::DroneFollow);
	}
	out.push_back(P::Model);
	out.push_back(P::Character);
	out.push_back(P::Morph);
	return out;
}

visage::Frame* ResonateFace::widgetFor(P p) const {
	for (const KnobSlot& k : knobs_) {
		if (k.param == p) {
			return k.knob.get();
		}
	}
	for (const BoxSlot& b : boxes_) {
		if (b.param == p) {
			return b.box.get();
		}
	}
	switch (p) {
	case P::Voicing: return voicing_.get();
	case P::DroneCharacter: return droneCharacter_.get();
	case P::DroneCycle: return cycle_.get();
	case P::HeadRelease: return ringChip_.get();
	case P::DroneTuning: return tuningChip_.get();
	case P::DroneCycleTransport: return transportChip_.get();
	case P::DroneFollow: return followChip_.get();
	case P::Model: return picker_.get();
	case P::Character: return characterBox_.get();
	case P::Morph: return morphBox_.get();
	default: return nullptr;
	}
}

} // namespace feathers::ui
