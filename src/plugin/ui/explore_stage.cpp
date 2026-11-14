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

#include "plugin/ui/pixel_serial.h"
#include "plugin/ui/like_glyph.h"
#include "plugin/ui/param_edit.h"
#include "device/format_value.h"
#include "plugin/ui/stage_rail.h"
#include "plugin/ui/slot_edit.h"
#include "plugin/ui/menu_anchor.h"
#include "plugin/ui/widget_registry.h"
#include "plugin/ui/param_knob.h"
#include "plugin/ui/travel_control.h"
#include "plugin/ui/explore_stage.h"
#include "plugin/ui/drum_panel.h"
#include "plugin/ui/spectrum_panel.h"

#include "dsp/constellation.h"

#include "plugin/ui/knob.h"
#include "dsp/drum_voice.h"
#include "plugin/params.h"
#include "plugin/sound_hues.h"
#include "plugin/ui/theme.h"
#include "plugin/ui/likes_log.h"

#include <visage/graphics.h>
#include <visage/utils.h>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>

namespace feathers::ui {

namespace {

/// The mock's `.stagelbl` geometry, shared with the Perform surface.
constexpr float kStageLabelTracking = 0.30f;
constexpr float kStageLabelTop = 16.0f;

/// The mock's `.btn`: a 1px border, a 7px radius and small caps at `.18em`.
constexpr float kButtonTracking = 0.18f;
constexpr float kButtonRounding = 7.0f;

/// How far a drag carries a corner encoder through its whole range. The knobs' own number, so a hand
/// that has learned one has learned the other.
constexpr float kEncoderTravel = Knob::kDragRange;

/// The selected star's label: how far off the glyph it sits, and the box it is given. The box is what
/// gets recorded as ink, so it is also what decides which side of the star the label goes on.
constexpr float kStarLabelGap = 16.0f;
constexpr float kStarLabelWidth = 120.0f;
/// How far inside the rim the off-chart mark sits, so the mark and the rim are two things rather than
/// one thick line.
constexpr float kOffChartInset = 7.0f;

/// How finely the rim's falloff is sampled into a gradient. The curve is a cubic and the step at the
/// rim is a step, so this is what decides how soft that edge looks; the rim hairline is drawn over it
/// either way. Photographed before it was settled.
constexpr int kVignetteSamples = 256;

/// **THE PLANET'S NUMBERS** (backlog FE), specified where given and set by eye where not — they are looks, not
/// derivations, and they belong on the real display.
/// - THE LIGHT IS FAR AWAY and the star is the point it stands overhead (the point through which a ray normal to the
///   planet surface towards the true light passes), so there is no altitude and no falloff:
///   the sphere is lit by one direction and the terminator is where it turns away. It got here by eye — a lamp at 5% of
///   the radius lit a white spot, at 60% it read as light, and at infinity it reads as a planet;
/// - the terminator is softened as a light of some angular size softens one; a point light would cut a hard line;
/// - a fairly dim ambience keeps the far side a sphere rather than a hole, raised a little
///   with the altitude so the dark side is not a hole;
/// - the light is white, slightly tinted by the sound's own colour;
/// - the atmosphere is HOW MUCH AIR THE EYE LOOKS THROUGH (greatest around the perimeter, where the eye looks through
///   the most atmosphere into empty space), so its reflectivity is well under
///   the ground's and the night side keeps only a floor of it.
constexpr float kPlanetTerminator = 0.18f;
constexpr float kPlanetAmbience = 0.33f;
constexpr float kPlanetTintShare = 0.25f;
constexpr float kPlanetAirReflectivity = 0.30f;
constexpr float kPlanetAirThickness = 0.045f;
constexpr float kPlanetAirScale = 1.15f;
constexpr float kPlanetAirNightFloor = 0.10f;

const std::string& stageCaption() {
	// The mock's own line for this view, and it is true of ours: the chart IS the selected sound's
	// character space. Built once because draw() runs every frame under ambient motion.
	static const std::string caption = "TERRAIN \xe2\x80\x94 THE SELECTED SOUND'S CHARACTER SPACE";
	return caption;
}

double clampZoom(double zoom) {
	return std::clamp(zoom, ExploreStage::minZoom(), ExploreStage::kMaxZoom);
}

/// The zoom, as a hand reads it. Past ×1 the ladder runs into fractions, and "x0.0" is what three
/// significant figures of a ratio look like when the ratio is 1/48 — a readout that says nothing
/// twice. Below ×1 it is written as the fraction it is.
void formatZoom(char* out, size_t capacity, double zoom) {
	if (zoom >= 1.0) {
		std::snprintf(out, capacity, "x%.1f", zoom);
		return;
	}
	std::snprintf(out, capacity, "x1/%.0f", 1.0 / zoom);
}

using SurroundSlot = ExploreStage::Slot;
using SurroundControl = ExploreStage::SurroundControl;

constexpr bool isBoxSlot(SurroundSlot slot) {
	return slot == SurroundSlot::LeftBox || slot == SurroundSlot::RightBox;
}

constexpr bool isBox(SurroundControl control) {
	return control >= SurroundControl::WarpMode && control < SurroundControl::kCount;
}

/// One knob per sound (the selected sound's is the one drawn), as against one for the whole engine.
constexpr bool isPerSound(SurroundControl control) {
	return control >= SurroundControl::Width && control <= SurroundControl::DrumMetal;
}

/// PLUCK'S SECOND PAGE (KS design §4.2): its row with the left flank's engine-wide knobs swapped for the page's others, in the
/// same slots. Derived, so the two pages cannot disagree about the right flank or the box.
constexpr ExploreStage::SlotRow pluckPageTwoOf(ExploreStage::SlotRow row) {
	for (SurroundControl& c : row) {
		switch (c) {
		case SurroundControl::KsWidth: c = SurroundControl::KsSeed; break;
		case SurroundControl::KsDecay: c = SurroundControl::KsDecayTrack; break;
		case SurroundControl::KsRelease: c = SurroundControl::KsHardness; break;
		case SurroundControl::Smoothness: c = SurroundControl::KsPickup; break;
		default: break;
		}
	}
	return row;
}

/// AND ITS THIRD (Velocity): the String page's Width slot holds the Velocity depth, the other three left slots stand empty.
constexpr ExploreStage::SlotRow pluckPageThreeOf(ExploreStage::SlotRow row) {
	for (SurroundControl& c : row) {
		switch (c) {
		case SurroundControl::KsWidth: c = SurroundControl::KsVelocity; break;
		case SurroundControl::KsDecay:
		case SurroundControl::KsRelease:
		case SurroundControl::Smoothness: c = SurroundControl::None; break;
		default: break;
		}
	}
	return row;
}

/// THE DRUM KIT'S OTHER TWO PAGES (2026-11-06, the kit-wide layer): Kit Fields puts the seven kit trims where the pad's seven
/// scalers stand; Kit Place puts Kit Gamma, Zone and Offset in three of them and empties the rest. The Walk stays on both.
constexpr ExploreStage::SlotRow drumKitFieldsOf(ExploreStage::SlotRow row) {
	for (SurroundControl& c : row) {
		switch (c) {
		case SurroundControl::DrumPitch: c = SurroundControl::DrumKitPitch; break;
		case SurroundControl::DrumLength: c = SurroundControl::DrumKitLength; break;
		case SurroundControl::DrumSweep: c = SurroundControl::DrumKitSweep; break;
		case SurroundControl::DrumTone: c = SurroundControl::DrumKitTone; break;
		case SurroundControl::DrumNoise: c = SurroundControl::DrumKitNoise; break;
		case SurroundControl::DrumSnap: c = SurroundControl::DrumKitSnap; break;
		case SurroundControl::DrumMetal: c = SurroundControl::DrumKitMetal; break;
		default: break;
		}
	}
	return row;
}

constexpr ExploreStage::SlotRow drumKitPlaceOf(ExploreStage::SlotRow row) {
	for (SurroundControl& c : row) {
		switch (c) {
		case SurroundControl::DrumSnap: c = SurroundControl::DrumKitZone; break;
		case SurroundControl::DrumMetal: c = SurroundControl::DrumKitOffset; break;
		case SurroundControl::DrumPitch: c = SurroundControl::DrumKitGamma; break;
		case SurroundControl::DrumLength:
		case SurroundControl::DrumSweep:
		case SurroundControl::DrumTone:
		case SurroundControl::DrumNoise: c = SurroundControl::None; break;
		default: break;
		}
	}
	return row;
}

/// THE RULES A TABLE KEEPS: a box only in a box slot and a knob only in a knob slot (a box is a line tall and a knob a
/// column, so neither fits the other's place); no control twice in one engine's row (it is one widget, so it can stand
/// in one place); and every control seated by SOME row, which is where it waits while it is not drawn.
constexpr bool slotTableValid(const ExploreStage::SlotTable& table) {
	std::array<bool, ExploreStage::kSurroundControlCount> seated {};
	for (const ExploreStage::SlotRow& row : table) {
		// (Pluck's second page seats its own four, through the row it is derived from.)
		std::array<bool, ExploreStage::kSurroundControlCount> inRow {};
		for (size_t s = 0; s < ExploreStage::kSlotCount; ++s) {
			const SurroundControl control = row[s];
			if (control == SurroundControl::None) {
				continue;
			}
			const auto c = static_cast<size_t>(control);
			if (c >= ExploreStage::kSurroundControlCount || isBox(control) != isBoxSlot(static_cast<SurroundSlot>(s))
			    || inRow[c]) {
				return false;
			}
			inRow[c] = true;
			seated[c] = true;
		}
	}
	for (const SurroundControl control : pluckPageTwoOf(table[static_cast<size_t>(OscType::Pluck)])) {
		seated[static_cast<size_t>(control)] = true;
	}
	for (const SurroundControl control : drumKitFieldsOf(table[static_cast<size_t>(OscType::Drum)])) {
		seated[static_cast<size_t>(control)] = true;
	}
	for (const SurroundControl control : drumKitPlaceOf(table[static_cast<size_t>(OscType::Drum)])) {
		seated[static_cast<size_t>(control)] = true;
	}
	for (const SurroundControl control : pluckPageThreeOf(table[static_cast<size_t>(OscType::Pluck)])) {
		seated[static_cast<size_t>(control)] = true;
	}
	for (size_t c = 1; c < ExploreStage::kSurroundControlCount; ++c) {
		if (!seated[c]) {
			return false;
		}
	}
	return true;
}

/// **THE SLOT TABLE (backlog LJ): ENGINE × SLOT → CONTROL.** Each row is read from the engine, one engine at a time; a
/// control is in an engine's row iff that engine reads its parameter, which is the honesty rule, and a control no row
/// seats on this engine is NOT DRAWN (only switching engine could wake it). The sound's own switch,
/// which is on this face, greys a drawn per-sound knob instead (syncWidthKnob).
///
/// Slots, left to right: Left0 Left1 Left2 LeftBox Left3 | Right0 Right1 Right2 Right3 Right4 RightBox.
constexpr ExploreStage::SlotTable kSurroundSlots = [] {
	using C = SurroundControl;
	ExploreStage::SlotTable table {};
	const auto row = [&table](OscType engine) -> ExploreStage::SlotRow& { return table[static_cast<size_t>(engine)]; };
	// MORPH: Grit alone. It scales the morph TABLE's ampNoise (slice_build.cpp), and PreparedCharacter::ampNoise()
	// hands that back for `OscEngine::Morph`, which both OscType::Morph and OscType::V5 map to. MEASURED by reading the
	// build: a plain Morph patch takes buildMorphFor -> oneMorph -> buildMorphSliced(..., s.gritScale). The flagship's
	// controls (Width, FM Depth, Warp, Smoothness and the other four field modifiers) reach nothing here.
	row(OscType::Morph) = { C::None, C::None, C::None, C::None, C::None,
		                    C::None, C::None, C::None, C::None, C::Grit, C::None };
	// STAIR: nothing. ampNoise() returns 0 and there is no flagship tier.
	row(OscType::Stair) = {};
	// V5, THE FLAGSHIP: flagship.cpp is the only reader of Width, Smoothness and the four flagship fields; FM Depth and
	// Warp are its operators (`flagshipChain`), with the warp mode under the Warp knob; Grit as on Morph.
	row(OscType::V5) = { C::Width,    C::FmDepth, C::Warp,     C::WarpMode, C::Smoothness,
		                 C::Quantize, C::Sync,    C::Feedback, C::Points,   C::Grit,       C::None };
	// RESONATE'S OWN SURROUND: engine-wide knobs in V5's slots, Speed in the Warp slot that Resonate
	// had nothing for, the bow mode in the mode box, and the model (what the one string is) under the right flank.
	row(OscType::Resonate) = { C::ResonateWidth, C::Bow,      C::Speed, C::BowMode, C::Shimmer,
		                       C::Stiffness,     C::Coupling, C::Decay, C::Pluck,   C::Travel,  C::Model };
	// BASIC SHAPES (2026-10-03) keeps V5's FM, Warp (and its mode) and Grit (CharacterBank::buildOne keeps V5's recipe
	// and morph modifiers) and V5's two-ear Width (Synth's stereo predicate, backlog KV), and none of V5's other
	// operators yet, so its mix takes the right flank's top four slots and
	// Smoothness's slot is empty.
	// THE OPERATORS (backlog KV): Smoothness in V5's own slot; Quantize, Sync, Feedback and Points on the right flank's
	// second page (ShapesPage), whose box stands in the right box slot. Each reaches the bank's operated shapes bake.
	row(OscType::BasicShapes) = { C::Width, C::FmDepth,  C::Warp, C::WarpMode, C::Smoothness,
		                          C::Sine,  C::Triangle, C::Saw,  C::Square,   C::Grit,       C::ShapesPage };
	// ADDITIVE (2026-10-10, design §4.1): the bank's Partials build reads each sound's Smoothness and Shape Amount
	// (buildPartialsFor) and the synth's terrain stereo builds two ears from Width; it runs no FM, Warp, Grit or flagship
	// field (design C4). Shape Amount takes Warp's slot with the Additive Shape in the mode box, and the six bias knobs
	// (read by the bank on a Partials build alone, character_params.h) take FM Depth's slot and the right flank's five.
	row(OscType::Additive) = { C::Width,   C::Rolloff, C::ShapeAmount, C::AdditiveShape, C::Smoothness,
		                       C::Formant, C::OddEven, C::Comb,        C::Detail,        C::OffGrid,    C::None };
	// PLUCK (2026-10-31, KS design §4.2): the voice's string reads the five field scalers (the bank's String build, through
	// stringFieldsWithOverrides) and every Pluck control (Synth::stringControls), and the pluck's tone reads Smoothness (it is
	// the Additive spectrum, buildPartialsFor). No FM, Warp, Grit, V5 field or Additive bias reaches it. The left flank's
	// engine-wide knobs are on two pages; the box in the left box slot names the page (pluckPageTwoOf has the other).
	row(OscType::Pluck) = { C::KsWidth,      C::KsDecay,   C::KsRelease, C::KsPage,  C::Smoothness,
		                    C::KsBrightness, C::KsSustain, C::KsPick,    C::KsNoise, C::KsStretch,  C::None };
	// DRUM (2026-11-05, drum proposal §6): the selected sound's seven field scalers (the bank's Drum build reads them through
	// drumFieldsWithOverrides) — five on the right flank, Snap and Metal on the left — and the Kit Walk (character_params.h
	// reads it in Kit mode) in the left's other two. currentSlotRow takes out what the mode or the selected kind does not read.
	row(OscType::Drum) = { C::DrumSnap,  C::DrumMetal,  C::DrumWalkX, C::DrumPage, C::DrumWalkY,
		                   C::DrumPitch, C::DrumLength, C::DrumSweep, C::DrumTone, C::DrumNoise, C::None };
	return table;
}();
static_assert(slotTableValid(kSurroundSlots), "the Explore slot table breaks its own rules (see slotTableValid)");

} // namespace

//-------------//
// Constructor //
//-------------//

ExploreStage::ExploreStage(ParamBridge& bridge, WidgetRegistry& registry, SoundSelection& selection,
                           EditHistory& history, TerrainMap& terrain, Notifier* notifier, Clock clock)
    : bridge_(bridge), selection_(selection), history_(history), terrain_(terrain),
      notifier_(notifier), clock_(std::move(clock)), slots_(kSurroundSlots) {
	rebuildShapesOperatorsRow();
	// THE VIEW HALF OF AN UNDO IS THIS SURFACE'S, so it listens. Registered here and taken back in the
	// destructor rather than left to the editor's teardown order: the history outlives every widget by
	// construction, so a surface that did not deregister would leave it holding a dead observer, and
	// "nobody presses undo after teardown" is a habit rather than a guarantee.
	history_.observe(this);
	// And the corners light by what the history holds, so they redraw when it moves — including when
	// something ELSE records, which is the whole point of the history being shared.
	watches_.push_back(history_.watch(this));
	if (!clock_) {
		clock_ = [] { return static_cast<int64_t>(visage::time::milliseconds()); };
	}

	// FEATHERS_EXPLORE_ZOOM=<x> opens the chart at a zoom, and it exists for the same reason
	// FEATHERS_SURFACE does (A17): zoom is a VIEW scale rather than a parameter, so a host cannot
	// drive it and a venue with no pointer cannot reach it — which left the one place the overlay bugs
	// actually lived unphotographable. Clamped like any other zoom; ignored if it is not a number.
	if (const char* opening = std::getenv("FEATHERS_EXPLORE_ZOOM")) {
		const double asked = std::atof(opening);
		if (asked > 0.0) {
			zoom_ = clampZoom(asked);
			terrainZoom_ = zoom_;
		}
	}

	// Everything the chart draws or the scrub writes: the identity of every sound, because the
	// selection can move to any of them, and the engine, because it decides what the map means.
	//
	// AND NOTHING FROM THE PERFORMANCE HALF, which is the mechanical statement of the design.
	// Weather, Path, Pos X/Y and the placements were watched here because they decided where the star
	// sat sideways; nothing on this surface is a function of them now, so a host automating them must
	// not reach it at all. A redraw per automation frame that produces an identical picture is not
	// merely wasted work: it is the text churn this project has already traced atlas corruption to,
	// and Explore under automated Weather is exactly where that churn was seen.
	watches_.push_back(registry.watchToken(static_cast<clap_id>(ParamId::OscType), this));
	watches_.push_back(registry.watchToken(static_cast<clap_id>(ParamId::GlobalGamma), this));
	for (size_t slot = 0; slot < kMaxSounds; ++slot) {
		for (const SoundField field :
		     { SoundField::Zone, SoundField::Offset, SoundField::Gamma, SoundField::Enable }) {
			watches_.push_back(registry.watchToken(static_cast<clap_id>(soundParam(slot, field)), this));
		}
	}
	watches_.push_back(selection_.watch(this));
	for (size_t slot = 0; slot < kMaxSounds; slot++) {
		watches_.push_back(registry.watchToken(static_cast<clap_id>(fmDepthParam(slot)), this));
		watches_.push_back(registry.watchToken(static_cast<clap_id>(smoothnessParam(slot)), this));
		// THE FIVE FIELD MODIFIER KNOBS ARE IN THE TERRAIN'S CONTENT KEY, so the surface has to hear
		// them move or the map goes stale under a turned knob — the exact failure the content-key
		// paragraph in terrain.h describes paying for three times: the old map stays up and the verb
		// looks dead.
		watches_.push_back(registry.watchToken(static_cast<clap_id>(quantizeParam(slot)), this));
		watches_.push_back(registry.watchToken(static_cast<clap_id>(syncParam(slot)), this));
		watches_.push_back(registry.watchToken(static_cast<clap_id>(feedbackParam(slot)), this));
		watches_.push_back(registry.watchToken(static_cast<clap_id>(pointsParam(slot)), this));
		watches_.push_back(registry.watchToken(static_cast<clap_id>(gritParam(slot)), this));
		watches_.push_back(registry.watchToken(static_cast<clap_id>(warpParam(slot)), this));
		// And the additive Shape Amount, which is in the terrain window's key on Additive.
		watches_.push_back(registry.watchToken(static_cast<clap_id>(shapeAmountParam(slot)), this));
		// And the six additive bias knobs, in the terrain window's key on Additive too.
		for (const ParamId id : { additiveRolloffParam(slot), additiveFormantParam(slot), additiveOddEvenParam(slot),
		                          additiveCombParam(slot), additiveDetailParam(slot), additiveOffGridParam(slot) }) {
			watches_.push_back(registry.watchToken(static_cast<clap_id>(id), this));
		}
		// And Pluck's five string scalers, in the terrain window's key on Pluck.
		for (const ParamId id : { ksBrightnessParam(slot), ksSustainParam(slot), ksPickParam(slot), ksNoiseParam(slot),
		                          ksStretchParam(slot) }) {
			watches_.push_back(registry.watchToken(static_cast<clap_id>(id), this));
		}
	}
	// AND THE ENGINE-WIDE WARP MODE, watched once rather than per slot — it is one parameter for the
	// whole engine, and it is in the terrain's content key, so the map must repaint when it moves.
	watches_.push_back(registry.watchToken(static_cast<clap_id>(ParamId::WarpMode), this));
	// The Resonate model is in the terrain window's key, so a change to it has to reach this stage.
	watches_.push_back(registry.watchToken(static_cast<clap_id>(ParamId::ResonateModel), this));
	// AND THE ADDITIVE SHAPE, engine-wide: the mode box names it on Additive, and it is in the terrain window's key there.
	watches_.push_back(registry.watchToken(static_cast<clap_id>(ParamId::AdditiveShape), this));

	// THE FLAGSHIP KNOBS (V16+): a vertical stack of three per-sound controls in the left flank
	// column — Width (the original V15 control), FM Depth (doc §10) and the Smoothness override
	// (doc §11), top to bottom. Eight stock Knobs behind each visible one (one per slot), each bound at
	// construction to its own slot's parameter, because Knob's binding is a constructor argument and
	// that is a property worth keeping (a rebindable control is a control that can be pointing
	// somewhere its label does not say). Selection change is a visibility flip in syncWidthKnob. The
	// registry wiring each Knob does for itself keeps the visible one honest under host automation;
	// enablement is synced per frame from published OscType and the selected slot's switch.
	//
	// WHY A STACK: the flagship now has three continuous per-sound dials and a vertical column reads
	// as "the flagship's controls" without asking the chart to yield more room than the single width
	// knob already reserved — they share the flank column and the disk() reserve is unchanged.
	auto makeKnob = [&](auto& array, size_t slot, const ParamDesc* desc, const char* label) {
		array[slot] = paramKnob(bridge_, registry, desc, label, notifier_);
		array[slot]->setFaceDiameter(kCornerDiameter);
		array[slot]->setVisible(false);
		addChild(array[slot].get());
	};
	// **THE POLAR PAIR**, the same stock Knob as everything else on this surface — over a
	// `TravelControl` rather than a parameter, because a sound's place is patch state and its
	// modulation is one of the core's INTERNAL destinations (params.h). "Same stock Knob ... so it is
	// a modulation destination for free", as the Warp note below says of itself.
	auto makePolar = [&](auto& array, size_t slot, TravelAxis axis, const char* label) {
		TravelControl::Hooks hooks;
		hooks.began = [this] { polarGestureBegan(); };
		hooks.ended = [this](TravelAxis which) { polarGestureEnded(which); };
		array[slot] = std::make_unique<Knob>(
		    std::make_unique<TravelControl>(bridge_, slot, axis, std::move(hooks)), label, notifier_);
		array[slot]->setFaceDiameter(kPolarDiameter);
		array[slot]->setVisible(false);
		addChild(array[slot].get());
	};
	for (size_t slot = 0; slot < kMaxSounds; ++slot) {
		makePolar(radiusKnobs_, slot, TravelAxis::Radius, "Radius");
		makePolar(thetaKnobs_, slot, TravelAxis::Theta, "Theta");
	}
	for (size_t slot = 0; slot < kMaxSounds; ++slot) {
		makeKnob(widthKnobs_, slot, findParam(static_cast<clap_id>(soundParam(slot, SoundField::Width))),
		         "Width");
		makeKnob(fmDepthKnobs_, slot, findParam(static_cast<clap_id>(fmDepthParam(slot))), "FM Depth");
		// WARP DEPTH, DIRECTLY UNDER FM DEPTH. Same stock Knob as every knob on both flanks, so it is
		// a modulation destination for free — and it sits beside FM Depth because the two are the
		// engine's only per-sound AMOUNTS: both go straight through with no phi field behind them and
		// both default to 0, where the three biases around them rest at 0.5.
		makeKnob(warpKnobs_, slot, findParam(static_cast<clap_id>(warpParam(slot))), "Warp");
		// THE ADDITIVE SHAPE AMOUNT (2026-10-10), the same kind of control (a per-sound amount, default 0) in Warp's slot,
		// which Additive does not draw. "Shape", because the box under it names WHICH shape.
		makeKnob(shapeAmountKnobs_, slot, findParam(static_cast<clap_id>(shapeAmountParam(slot))), "Shape");
		makeKnob(
		    smoothnessKnobs_, slot, findParam(static_cast<clap_id>(smoothnessParam(slot))), "Smooth");
		// THE FIVE FIELD MODIFIER KNOBS, right flank. Same makeKnob, so they are stock Knobs with
		// Knob's own registry wiring, Knob's anchor-and-delta gesture and Knob's mod ring — which is
		// what makes them modulation destinations without this file knowing anything about the bus.
		//
		// "Grit" rather than "Amp Noise": modifiers.h calls the audible thing grit in its own code
		// (`const float grit = u * amount * std::abs(sample)`), and "Amp Noise" names the table member
		// a player never sees.
		makeKnob(fieldKnobs_[static_cast<size_t>(FieldKind::Quantize)], slot,
		         findParam(static_cast<clap_id>(quantizeParam(slot))), "Quantize");
		makeKnob(fieldKnobs_[static_cast<size_t>(FieldKind::Sync)], slot,
		         findParam(static_cast<clap_id>(syncParam(slot))), "Sync");
		makeKnob(fieldKnobs_[static_cast<size_t>(FieldKind::Feedback)], slot,
		         findParam(static_cast<clap_id>(feedbackParam(slot))), "Feedback");
		makeKnob(fieldKnobs_[static_cast<size_t>(FieldKind::Points)], slot,
		         findParam(static_cast<clap_id>(pointsParam(slot))), "Points");
		makeKnob(fieldKnobs_[static_cast<size_t>(FieldKind::Grit)], slot,
		         findParam(static_cast<clap_id>(gritParam(slot))), "Grit");
		// THE ADDITIVE BIAS KNOBS (2026-10-10, design §4.1), the same stock Knob: modulation destinations for free.
		const auto bias = [&](AdditiveBiasKind kind, ParamId id, const char* label) {
			makeKnob(additiveBiasKnobs_[static_cast<size_t>(kind)], slot, findParam(static_cast<clap_id>(id)), label);
		};
		bias(AdditiveBiasKind::Rolloff, additiveRolloffParam(slot), "Rolloff");
		bias(AdditiveBiasKind::Formant, additiveFormantParam(slot), "Formant");
		bias(AdditiveBiasKind::OddEven, additiveOddEvenParam(slot), "Odd/Even");
		bias(AdditiveBiasKind::Comb, additiveCombParam(slot), "Comb");
		bias(AdditiveBiasKind::Detail, additiveDetailParam(slot), "Detail");
		bias(AdditiveBiasKind::OffGrid, additiveOffGridParam(slot), "Off-grid");
		// PLUCK'S FIVE FIELD SCALERS (KS design §4.1), stock Knobs: modulation destinations for free.
		const auto str = [&](StringKind kind, ParamId id, const char* label) {
			makeKnob(stringKnobs_[static_cast<size_t>(kind)], slot, findParam(static_cast<clap_id>(id)), label);
		};
		str(StringKind::Brightness, ksBrightnessParam(slot), "Brightness");
		str(StringKind::Sustain, ksSustainParam(slot), "Sustain");
		str(StringKind::Pick, ksPickParam(slot), "Pick");
		str(StringKind::Noise, ksNoiseParam(slot), "Noise");
		str(StringKind::Stretch, ksStretchParam(slot), "Stretch");
		// THE DRUM'S SEVEN FIELD SCALERS (drum proposal §3.2), per sound: in Kit mode the selected PAD's.
		const auto drm = [&](DrumField f, DrumControl dc, const char* label) {
			makeKnob(drumFieldKnobs_[static_cast<size_t>(f)], slot, findParam(static_cast<clap_id>(drumParam(slot, dc))), label);
		};
		drm(DrumField::Pitch, DrumControl::FieldPitch, "Pitch");
		drm(DrumField::Length, DrumControl::FieldLength, "Length");
		drm(DrumField::Sweep, DrumControl::FieldSweep, "Sweep");
		drm(DrumField::Tone, DrumControl::FieldTone, "Tone");
		drm(DrumField::Noise, DrumControl::FieldNoise, "Noise");
		drm(DrumField::Snap, DrumControl::FieldSnap, "Snap");
		drm(DrumField::Metal, DrumControl::FieldMetal, "Metal");

	}
	// RESONATE'S SURROUND: one knob each, engine-wide (there is one string), on the Resonate params. Their
	// names are Resonate's own: each does a different thing from the per-sound knob whose slot it takes.
	constexpr std::array<std::pair<ParamId, const char*>, kResonateKnobCount> kResonateKnobs { {
	    { ParamId::ResonateWidth, "Width" },         { ParamId::ResonateBow, "Bow" },
	    { ParamId::ResonateShimmer, "Shimmer" },     { ParamId::ResonateStiffness, "Stiffness" },
	    { ParamId::ResonateCoupling, "Coupling" },   { ParamId::ResonateDecay, "Decay" },
	    { ParamId::ResonatePluck, "Pluck" },         { ParamId::ResonateTravel, "Travel" },
	    { ParamId::ResonateSpeed, "Speed" },
	} };
	for (size_t i = 0; i < kResonateKnobCount; ++i) {
		makeKnob(resonateKnobs_, i, findParam(static_cast<clap_id>(kResonateKnobs[i].first)), kResonateKnobs[i].second);
	}
	// PLUCK'S ENGINE-WIDE KNOBS, one each (every voice's string reads them).
	constexpr std::array<std::pair<ParamId, const char*>, kPluckKnobCount> kPluckKnobs { {
	    { ParamId::KsWidth, "Width" },      { ParamId::KsDecay, "Decay" },         { ParamId::KsRelease, "Release" },
	    { ParamId::KsSeed, "Seed" },        { ParamId::KsDecayTrack, "Track" },    { ParamId::KsHardness, "Hardness" },
	    { ParamId::KsPickup, "Pickup" },    { ParamId::KsVelocity, "Velocity" },
	} };
	for (size_t i = 0; i < kPluckKnobCount; ++i) {
		makeKnob(pluckKnobs_, i, findParam(static_cast<clap_id>(kPluckKnobs[i].first)), kPluckKnobs[i].second);
	}
	// THE DRUM'S KIT WALK, engine-wide (every pad moves together).
	makeKnob(drumWalkKnobs_, 0, findParam(static_cast<clap_id>(ParamId::DrumWalkX)), "Walk X");
	makeKnob(drumWalkKnobs_, 1, findParam(static_cast<clap_id>(ParamId::DrumWalkY)), "Walk Y");
	// AND THE KIT-WIDE LAYER (2026-11-06), engine-wide: Gamma, Zone, Offset and the seven trims, on the Kit pages.
	constexpr std::array<const char*, kDrumKitKnobCount> kDrumKitLabels { "Kit Gamma", "Kit Zone", "Kit Offset", "Kit Pitch",
		                                                                  "Kit Length", "Kit Sweep", "Kit Tone",  "Kit Noise",
		                                                                  "Kit Snap",   "Kit Metal" };
	for (size_t i = 0; i < kDrumKitKnobCount; ++i) {
		const auto id = static_cast<clap_id>(static_cast<uint32_t>(ParamId::DrumKitGamma) + static_cast<uint32_t>(i));
		makeKnob(drumKitKnobs_, i, findParam(id), kDrumKitLabels[i]);
		// IN THE TERRAIN'S WINDOW (Zone, Offset and Gamma move every row; the trims every place's fields), so the chart hears it.
		watches_.push_back(registry.watchToken(id, this));
	}
	// AND THE WALK, which the stage draws now (the played mark and every pad's arrow).
	watches_.push_back(registry.watchToken(static_cast<clap_id>(ParamId::DrumWalkX), this));
	watches_.push_back(registry.watchToken(static_cast<clap_id>(ParamId::DrumWalkY), this));
	// The mode and every pad's kind decide which of them are drawn (currentSlotRow), so a move of one re-lays the face.
	watches_.push_back(registry.watchToken(static_cast<clap_id>(ParamId::DrumMode), this));
	watches_.push_back(registry.watchToken(static_cast<clap_id>(ParamId::DrumPitchedKind), this));
	for (size_t pad = 0; pad < kMaxSounds; ++pad) {
		watches_.push_back(registry.watchToken(static_cast<clap_id>(drumParam(pad, DrumControl::Kind)), this));
		// AND THE SEVEN SCALERS, which are in the terrain window's key (the picture reads each place through them).
		for (const DrumControl c : { DrumControl::FieldPitch, DrumControl::FieldLength, DrumControl::FieldSweep, DrumControl::FieldTone,
		                             DrumControl::FieldNoise, DrumControl::FieldSnap, DrumControl::FieldMetal }) {
			watches_.push_back(registry.watchToken(static_cast<clap_id>(drumParam(pad, c)), this));
		}
	}
	// And the bow mode, which the mode box names on Resonate.
	watches_.push_back(registry.watchToken(static_cast<clap_id>(ParamId::ResonateBowMode), this));
	// And Stiffness and Coupling, which are in the terrain window's key (a Resonate texel is the shaped pose).
	watches_.push_back(registry.watchToken(static_cast<clap_id>(ParamId::ResonateStiffness), this));
	watches_.push_back(registry.watchToken(static_cast<clap_id>(ParamId::ResonateCoupling), this));
	// BASIC SHAPES' MIX: one knob each, engine-wide. Watched too, because the mix is in the terrain window's key.
	constexpr std::array<std::pair<ParamId, const char*>, kShapesKnobCount> kShapesKnobs { {
	    { ParamId::ShapesSine, "Sine" },
	    { ParamId::ShapesTriangle, "Triangle" },
	    { ParamId::ShapesSaw, "Saw" },
	    { ParamId::ShapesSquare, "Square" },
	} };
	for (size_t i = 0; i < kShapesKnobCount; ++i) {
		makeKnob(shapesKnobs_, i, findParam(static_cast<clap_id>(kShapesKnobs[i].first)), kShapesKnobs[i].second);
		watches_.push_back(registry.watchToken(static_cast<clap_id>(kShapesKnobs[i].first), this));
	}

	onMouseDown() = [this](const visage::MouseEvent& e) {
		// `position`, not `relativePosition()`. See the note in param_row.cpp — the latter is movement,
		// and reading it once cost this project a release's worth of dead sliders.
		// TAKING HOLD FIXES THE GROUND. The view settles before anything moves, so the coordinate a drag
		// is measured in cannot change underneath it — which it would otherwise do on the very first
		// press, since an unsettled view is derived from the sound the press is about to move.
		// THE WARP MODE BOX, TESTED FIRST — before the recentre and before the scrub. It sits in the
		// right flank, which the chart does not claim, so this cannot steal a gesture aimed at the
		// map; and testing it after the scrub would make a press on the box also drag the terrain,
		// which is the "two things on one press" shape the fx cards each spend a comment refusing.
		//
		// ONLY WHILE IT IS LIVE. The box is not drawn off its engines, and a control nobody can see that still
		// opens its menu is the honesty rule broken in the direction nobody checks. So both box slots ask the slot
		// table, as their drawing does (drawSlotBox): the warp, bow or additive-shape mode under the left stack, the
		// Resonate model under the right, or nothing.
		for (const Slot slot : { Slot::LeftBox, Slot::RightBox }) {
			const SurroundControl box = currentSlotRow()[static_cast<size_t>(slot)];
			if (box != SurroundControl::None && slotBounds(slot).contains(e.position)) {
				showBoxMenu(box);
				return;
			}
		}
		// A NEW HAND ENDS THE OLD GESTURE, WHEREVER IT HAD GOT TO. A released drag may still be coasting
		// (`settlingScrub_`); pressing again is the hand saying it has changed its mind, and the edit it
		// interrupts is still an edit — so it is closed here, with its own crumb and its own undo entry,
		// before the new one opens. Left running, the two would share a `departure_` and the first would
		// lose its place in the history.
		if (settlingScrub_) {
			finishScrub();
		}
		if (!everCentred_ || centredOnSlot_ != selection_.slot()) {
			followSelection();
		}
		grabbed_ = hitAt(e.position);
		grabY_ = e.position.y;
		grabFine_ = (e.modifiers & visage::kModifierShift) != 0;
		grabLastY_ = e.position.y;
		switch (grabbed_) {
		case Part::Chart:
			// THE CLOCK STARTS HERE. `starSteppedAt_` is a moment, and one that has never been set is the epoch — so the
			// first drag of a session asked for a quarter of a second's worth of travel in a single event, which is most
			// of a view and exactly the jump the cap exists to stop.
			starSteppedAt_ = clock_ ? clock_() : 0;
			// The departure point, taken BEFORE anything moves: a breadcrumb records where you left, so
			// the trail is the places you have been rather than the place you ended up.
			departure_ = identity();
			{
				// THROUGH THE BUBBLE, because the pointer landed on the glass and what it MEANS is the
				// place on the sheet under it. Flattening first and reading both axes off the flattened
				// point is the only order that works: the map is radial, so y's answer depends on x.
				const visage::Point flat = flatAt(e.position);
				// ONE POINT, ONE WRITE. This was `scrubTo` for the vertical and `driftTo` for the
				// horizontal — a gamma write and a travel write for one press, which is what put gamma
				// on a display axis.
				// AND THROUGH THE LIKES' WARP: the press lands on the drawn world, and means the real place drawn there.
				const WarpPoint pressed = worldOf(travelXAtScreenX(flat.x), travelYAtScreenY(flat.y));
				driftToXY(pressed.x, pressed.y);
				// AND THE PRESS IS ITS OWN TARGET. `update()` walks the star toward `dragWanted*` on
				// every frame the chart is held, so leaving them where the LAST gesture left them means
				// a press-and-hold quietly drags the sound back toward wherever the previous drag ended
				// — a chart that moves under a still hand, from a number nobody set this time round.
				dragWantedHome_ = selectedDriftY();
				dragWantedTravel_ = selectedDrift();
			}
			break;
		case Part::Gamma:
			// THE MAP HOLDS STILL FOR THE DURATION — see `heldGammaBase_`. Recorded at the grab rather
			// than derived later, because by the time the first drag event arrives the base has already
			// moved and the number that would hold the map is gone.
			heldGammaBase_ = selectedGammaBase();
			// INCREMENTS, like rotate, because the state of record accumulates: the base is unbounded,
			// so there is no absolute position for a drag to map onto, and a rewind is the stored number
			// restored rather than the increments subtracted back off (sound_gamma.h).
			departure_ = identity();
			grabValue_ = 0.0;
			break;
		case Part::Zoom:
			grabValue_ = zoom_;
			break;
		case Part::Rotate:
			// The turn accumulates in the generator, so the encoder sends INCREMENTS rather than a
			// position — which is also why a rewind is a stored number restored rather than the turns
			// subtracted back off.
			departure_ = identity();
			grabValue_ = 0.0;
			break;

		case Part::Like:
			// A POPUP TRIGGER (a right press, or ctrl-left on a Mac) opens the heart's menu; any other press is the like.
			if (e.shouldTriggerPopup()) {
				showMenuAt(likesMenu(), *this, partBounds(Part::Like));
			}
			else {
				toggleLike();
			}
			grabbed_ = Part::None;
			break;
		case Part::Mutate:
			mutate();
			grabbed_ = Part::None;
			break;
		case Part::Undo:
			undo();
			grabbed_ = Part::None;
			break;
		case Part::Redo:
			redo();
			grabbed_ = Part::None;
			break;
		default:
			// Rotate and Drift are drawn and labelled and answer nothing: their engine API is being
			// built, and a corner that moved something it had no way to reach would be the one thing the
			// honesty rule forbids outright.
			grabbed_ = Part::None;
			break;
		}
	};
	onMouseDrag() = [this](const visage::MouseEvent& e) {
		switch (grabbed_) {
		case Part::Chart:
			// AT A BOUNDED SPEED SINCE 2026-09-17, so a fling cannot pull unbuilt country into the glass — see
			// `dragStarTo`. Where the pointer asked for is remembered, because the rest of a capped drag arrives on the
			// frames after the event (`update`).
			{
				const visage::Point flat = flatAt(e.position);
				const WarpPoint wanted = worldOf(travelXAtScreenX(flat.x), travelYAtScreenY(flat.y));
				dragWantedHome_ = wanted.y;
				dragWantedTravel_ = wanted.x;
			}
			dragStarTo(dragWantedHome_, dragWantedTravel_);
			// AND THE VIEW COMES ALONG WHEN THE POINT REACHES THE GLASS. Both coordinates are read from
			// the pointer THROUGH the current view, so this has to happen after they are written and
			// before the next event maps another pointer position — the same order a map drag uses.
			//
			// It is a NUDGE, not a re-centre: a centring follow mid-drag would throw the ground a half
			// view every time it fired, and A15's "the world moves only when you move" is what keeps the
			// terrain finishable at 31ms a build. In the middle of the chart keepInsideView returns the
			// centre it was given and nothing moves at all.
			//
			// AND THE VERTICAL AXIS NO LONGER STOPS. It used to: scrubTo clamped home to the band, so at
			// the floor or the ceiling the star came to rest with the void beyond it. Under the one-value
			// design the scrub moves the unbounded base and there is nothing to clamp, so this
			// is now the ONLY thing that decides where the view goes on either axis — which is what makes
			// the crossing of the old edge feel like nothing at all, because nothing happens there.
			settleOn(keepInsideView(viewDriftY(), viewCentre(), homeSpanZones() / 1023.0,
			                        kScrubEdgeMargin),
			         keepInsideView(viewDrift(), travelCentre(), travelSpan(), kScrubEdgeMargin));
			break;
		case Part::Gamma: {
			// THE CORNER NOW CARRIES THE COARSE HALF, and this is the whole point of the vernier
			// arrangement. It used to drive the per-sound Gamma PARAMETER, which meant the one control
			// for "take this sound somewhere else along its own line" stopped dead at ±4 — and that
			// parameter has an id, a host automation lane and a range no host could redraw. So the
			// parameter stays exactly where it was, as the fine control, and the drag accumulates into
			// the unbounded base instead. Same rate as before, so the hand keeps its muscle memory;
			// what changed is that the travel no longer ends.
			const float travel = regearCorner(e);
			const double moved = static_cast<double>(grabY_ - e.position.y) / travel * kGammaTurn;
			// Only the part not yet sent — the same accumulation rotate makes, for the same reason.
			nudgeSelectedGamma(moved - grabValue_);
			grabValue_ = moved;
			break;
		}
		case Part::Rotate: {
			const float travel = regearCorner(e);
			const double moved = static_cast<double>(grabY_ - e.position.y) / travel * kRotateSpan;
			// Only the part not yet sent, because the state of record accumulates: sending the whole
			// drag every frame would turn the plane by the sum of every frame's reading.
			rotateSelectedBy(moved - grabValue_);
			grabValue_ = moved;
			break;
		}
		case Part::Zoom: {
			// Log-scaled, because a zoom is a ratio: the same drag has to mean the same factor at ×1 and
			// at ×32, and a linear encoder would spend nine tenths of its travel above ×8.
			const double decades = static_cast<double>(grabY_ - e.position.y) / kEncoderTravel;
			setZoom(grabValue_ * std::pow(kMaxZoom / minZoom(), decades));
			break;
		}
		default:
			break;
		}
	};
	onMouseUp() = [this](const visage::MouseEvent&) {
		// **THE REST OF A CAPPED DRAG IS NOT ABANDONED AT THE RELEASE.** The cap hands the star a
		// bounded number of views per second, so a quick drag is still travelling when the button comes
		// up — and closing the gesture here threw whatever had not arrived away, leaving the sound short
		// of the place the hand asked for: on the first build with the cap in it, click-to-pan fell
		// short.
		//
		// So the gesture stays OPEN while the world glides to a stop: `update()` keeps walking, at the
		// same bounded rate, and `finishScrub()` closes it — one undo entry, one crumb, one recentre,
		// planted where the hand actually asked for rather than where the clock happened to leave it.
		// Snapping to the target instead would have been two lines, and would have put back exactly the
		// jump the cap exists to prevent: the fling's whole distance arriving in one frame.
		if (grabbed_ == Part::Chart
		    && (selectedDriftY() != dragWantedHome_ || selectedDrift() != dragWantedTravel_)) {
			settlingScrub_ = true;
			grabbed_ = Part::None;
			return;
		}
		if (grabbed_ == Part::Chart || grabbed_ == Part::Gamma || grabbed_ == Part::Rotate) {
			endGesture();
			const SoundIdentity now = identity();
			if (departure_.has_value() && !(*departure_ == now)) {
				// NAMED BY THE CORNER THAT MADE IT, so an undo can say which of the four gestures it is
				// walking back. All four write identity and all four are one entry; they differ only in
				// what a hand would call them.
				const char* verb = grabbed_ == Part::Gamma    ? "gamma"
				                   : grabbed_ == Part::Rotate ? "rotate"
				                                              : "scrub";
				record(selection_.slot(), *departure_, now, verb);
				// The mock plants its crumb at the DEPARTURE point, which is why the trail is a record of
				// where you have been rather than a duplicate of where you are. (The polar knobs leave
				// none — see `polarGestureEnded`.)
				plantCrumb(selection_.slot(), *departure_);
			}
			// PANNED, NOT SNAPPED. A click arrives here having put the star on the pressed point with
			// the view still where it was, so this is the whole journey; a jog that stayed under the
			// cap arrives with only the `keepInsideView` deadzone left to pay. Both are the same move
			// at different lengths, which is why they share one road.
			glideToSelected();
		}
		departure_.reset();
		// **THE HOLD ENDS WHERE THE GESTURE DOES**, and it is released BEFORE the `askForTerrain()`
		// below so that last word is the true base rather than the held one. Cleared unconditionally
		// rather than only for `Part::Gamma`: a hold that outlived its gesture would freeze the map at
		// a gamma nobody is on any more, and the way that fails is silence — the corner keeps reading
		// out while the picture stops answering.
		heldGammaBase_.reset();
		grabbed_ = Part::None;
		// The last word on the window, so the map settles on exactly where the gesture ended rather
		// than on whichever frame the builder happened to sample.
		askForTerrain();
	};

	// THE SPECTRUM TAB (additive design §4.2), LAST: visage paints children after their parent and in order, and hands the
	// mouse to the last child under it first, so it covers every knob and box before it. Its label asks the editor for the
	// spectrum panel (`onSpectrumTab`).
	spectrumTab_ = std::make_unique<SpectrumTab>(bridge_, [this] {
		if (onSpectrumTab) {
			onSpectrumTab();
		}
	});
	spectrumTab_->setVisible(false);
	addChild(spectrumTab_.get());
	drumTab_ = std::make_unique<DrumTab>([this] {
		if (onDrumTab) {
			onDrumTab();
		}
	});
	drumTab_->setVisible(false);
	addChild(drumTab_.get());
}

void ExploreStage::finishScrub() {
	// The close a released drag gets once it has actually arrived — the same four things `onMouseUp`
	// does for every other gesture, only later. `settlingScrub_` is cleared FIRST so that a `recentre`
	// or a record that reached back in here could not find the settle still running.
	settlingScrub_ = false;
	endGesture();
	const SoundIdentity now = identity();
	if (departure_.has_value() && !(*departure_ == now)) {
		record(selection_.slot(), *departure_, now, "scrub");
		plantCrumb(selection_.slot(), *departure_);
	}
	departure_.reset();
	// THE FAST DRAG'S CLOSE, and it glides for the same reason the other two do — this is the release
	// that had the FURTHEST left to travel, since the star was still coasting when the button came up.
	glideToSelected();
	askForTerrain();
}

ExploreStage::~ExploreStage() {
	// TAKE THE OBSERVER BACK. The history outlives this widget, so leaving it pointed here would leave
	// a live object holding an address that has stopped being one.
	history_.observe(nullptr);
}

bool ExploreStage::mouseWheel(const visage::MouseEvent& e) {
	// THE TRACE, behind an environment variable, because the interesting question is not what we do
	// with the event but whether the event arrives at all — and on somebody else's touchpad, at that.
	// Same idiom as the atlas trace. Written to stderr so it lands in the host's engine log.
	if (std::getenv("FEATHERS_TRACE_WHEEL") != nullptr) {
		std::fprintf(stderr,
		             "[feathers] wheel: dy=%.4f dx=%.4f precise_dy=%.4f precise_dx=%.4f "
		             "momentum=%d mods=%d at=(%.0f,%.0f)\n",
		             static_cast<double>(e.wheel_delta_y), static_cast<double>(e.wheel_delta_x),
		             static_cast<double>(e.precise_wheel_delta_y),
		             static_cast<double>(e.precise_wheel_delta_x), e.wheel_momentum ? 1 : 0,
		             e.modifiers, static_cast<double>(e.position.x),
		             static_cast<double>(e.position.y));
		std::fflush(stderr);
	}
	const double notches = static_cast<double>(e.precise_wheel_delta_y);
	if (notches == 0.0) {
		return false;
	}
	// Geometric, because zoom is a ratio: one notch is the same FACTOR at x1 and at x32, which is the
	// same reason the corner encoder is log-scaled. Up is in, which is every map application's rule.
	const double was = zoom_;
	setZoom(zoom_ * std::pow(kZoomStep, notches));

	// REPAINT EVEN WHEN THE ZOOM DID NOT MOVE, and this is a fix rather than belt-and-braces. `setZoom`
	// returns early at either end of the ladder without redrawing — so scrolling out past the minimum
	// used to paint NOTHING while still telling visage the event was handled. Any region corrupted by
	// something else (the atlas-resize garbage this project has already diagnosed, whose trigger is
	// text churn — and the zoom readout churns text on every notch) then had nothing to repaint it, on
	// a surface the hand was actively scrolling over. The reproduction is exactly that: zoom out past the
	// maximum.
	repaint();
	// And say honestly whether anything moved. Claiming an event that changed nothing is what let the
	// press be swallowed silently at the ends of the range.
	return zoom_ != was;
}

//----------//
// Geometry //
//----------//

StageDisk ExploreStage::disk() const {
	// THE CHART YIELDS TO THE WIDTH COLUMN — V16, after the hidden-knob case turned up on a real
	// window: a hidden knob must never happen. The column is reserved BEFORE the square is sized, so
	// the knob is always present and the chart gets what remains.
	//
	// SYMMETRIC, the chosen one of the two shapes the design left open: the same reserve comes off both
	// sides, so the disk keeps the FRAME'S centre — the circle stays concentric with Perform's
	// across a surface switch, which is the both-surfaces-one-aperture rule this file opens with; a
	// left-only reserve would make the instrument jump sideways every time the mode changed. What
	// symmetry costs is a slightly smaller chart at narrow sizes; what it banks is the right flank,
	// which is exactly the room first set aside for future controls. At generous sizes (the
	// shipped 866x534 stage among them) the height binds first, the reserve is free, and the disk
	// is exactly what it always was.
	const float reserve = 2.0f * (kFlankGap + kCornerWidth);
	const float side = std::max(0.0f, std::min(height(), width() - reserve));
	return { visage::Point { width() * 0.5f, height() * 0.5f }, side };
}

bool ExploreStage::hitsDisk(visage::Point pixels) const {
	return disk().contains(pixels);
}

double ExploreStage::minZoom() {
	// THE WIDEST STOP, AND ITS THIRD ANCHOR. It was ×1 — one neighbourhood, a keyhole. Then it was THE
	// BAND, three units of home, on the reasoning that a wider view would spend the extra on nowhere.
	// The infinite world deleted that reasoning (there is no nowhere; every home draws), and span
	// averaging has now deleted the one that replaced it.
	//
	// THE MEASUREMENT THAT USED TO BIND WAS NEVER THE ONE THE RECORD CLAIMED. The map was supposedly
	// sevenfold oversampled at the band stop; measured against the drawn picture instead, the local
	// layer was ALREADY at the noise ceiling there, and stair's glyph pattern (7 × φ³, the fastest
	// thing in either engine) has a Nyquist span of 2,208 zones against the 3,069 we shipped. There was
	// no headroom to widen into — there was a deficit, and a wider stop on its own would have made a
	// louder one.
	//
	// AVERAGING IS WHAT MOVES THE STOP, which is why the two are one change. A texel now reports the
	// MEAN of its own cell rather than a point in the middle of it (terrain.cpp), so a feature finer
	// than a texel is summarised instead of sampled: undersampling became blur, and blur has no Nyquist
	// to run into. What is left bounding the view is the slow orientation field the wide picture is
	// really made of — measured neighbour-step against its own spread: 0.03 out to the old stop, 0.11 at
	// 96 zones a row, 0.20 at 192, 0.39 HERE at 384, 0.76 at 768, and past 1 (noise) at 1536. Twofold
	// margin to where that gradient starts to step, threefold to where it stops meaning anything — and
	// the macro layer is meaned over the cell as well, which is nearly free and buys the margin rather
	// than hoping for it.
	//
	// SIXTEEN TIMES THE OLD REACH, and with it the half of the original requirement (A33) that no zoom could
	// ever answer — maximum zoom-out should show several periods of the lowest-wavelength phi
	// oscillations. Here the slowest orientation angle fits 1.65 times over and the aperture seven;
	// the band stop showed a tenth of the one and half of the other.
	//
	// WHAT STOPS IT GOING FURTHER IS NO LONGER THE PICTURE: it is the sky's integer cast, the gridline
	// ladder and double precision once a span runs to millions of zones. ×1/1536 is one constant away.
	return std::min(kUnitZoom, kBaseZones / kWidestSpanZones);
}

double ExploreStage::homeSpanZones() const {
	return kBaseZones / zoom_;
}

double ExploreStage::travelSpan() const {
	return kBaseTravel / zoom_;
}

namespace {
/// The height and width the chart's spans are mapped over: the disk's bounding square, so a texel is
/// square and the terrain's own grid lands on it one for one.
float chartExtent(const StageDisk& d) {
	return d.radius() * 2.0f;
}
} // namespace

double ExploreStage::viewCentre() const {
	// Settled if it has been settled and the chart still belongs to the same sound; otherwise where it
	// WOULD settle. Answering before the first frame is what stops a press that arrives first from
	// being measured against a centre of zero.
	if (everCentred_ && centredOnSlot_ == selection_.slot()) {
		// THE VIEW FOLLOWS GAMMA, ONE FOR ONE, AND IT IS NOT A PREFERENCE.
		//
		// THE SYMPTOM: adjusting sound gamma made the terrain vanish entirely until the adjusting stopped.
		// Here is why, and it is arithmetic rather than a race. The reachable band is
		// `[gammaSum, gammaSum + 3]` — `homeFloor` IS the gamma sum, see terrain.cpp — so gamma does not
		// move a sound THROUGH the world, it drags the whole world along with it. Hold the view still
		// while gamma moves and the band slides straight out from under it, and then every row of the
		// chart is a place that does not exist, which the builder correctly paints as nothing. Measured
		// on a sound at zone 0 offset 0, where home EQUALS gamma so the view sits on the band's floor:
		// +0.01 of gamma is 16% void, +0.05 is 80%, and +0.10 is the WHOLE CHART. The corner moves
		// `kGammaTurn` = 8.0 of home per encoder travel, so 0.10 is about one part in eighty of one drag
		// — a couple of pixels of hand. `recentre()` on mouse-up puts the view back inside the band,
		// which is exactly why letting go brings the picture back.
		//
		// Following costs nothing and lies about nothing. Gamma moves this sound's home and the band by
		// the same amount, so `home - gammaSum` — the sound's place WITHIN the band — cannot change: the
		// star holds still on screen, the gridlines hold still, and what changes is the terrain itself,
		// which is precisely the feedback the encoder exists to give. A15's rule that the view holds
		// under a SCRUB is untouched, because a scrub writes zone and offset and never moves gamma.
		//
		// DONE HERE RATHER THAN IN THE DRAG HANDLER so that it is true of every road that moves gamma:
		// the corner, the per-sound vernier knob, the global gamma, a preset load, an undo, and a host
		// automation lane. Patching the one gesture we had reproduced would have left the other five.
		// **THE VIEW NO LONGER FOLLOWS GAMMA** (backlog GY: 2026-09-20). This line was
		// `viewCentre_ + (gammaSum() - centredOnGamma_)` — the one-for-one follow — and the long
		// rationale above it was true of a chart whose vertical axis WAS home: the reachable band is
		// `[gammaSum, gammaSum + 3]`, so gamma dragged the whole band and a still view would have
		// watched the world slide out from under it.
		//
		// By design the chart is a window on the 2-space `rotate` decodes. Gamma moves the
		// plane and moves nothing WITHIN it, so a place on the chart keeps its screen position when
		// gamma turns and there is nothing for the view to follow. The terrain changes underneath a
		// stationary star, which is what the corner is for.
		//
		// AND THE MACHINERY BEHIND IT IS GONE TOO, 2026-09-21. With the follow removed,
		// `centredOnGamma_` was a baseline nobody subtracted and `viewGamma()`/`scrubbedGamma_`
		// compensated a follow that no longer happened. They were kept one round longer on the
		// grounds that "the scrub still writes gamma" — and then `scrubTo` turned out to have no
		// caller left in `src/` at all, which removed the last thing that could have written gamma
		// from a chart gesture. `viewGamma()` had no callers, `scrubbedGamma_` had no writers, and
		// `centredOnGamma_` was written once and read nowhere: three dead members kept alive by a
		// comment explaining why they were not dead yet.
		return viewCentre_;
	}
	// BEFORE THE FIRST FRAME, where the view WOULD settle — and that is the sound's place in the
	// plane now, not its home. A press that arrives before anything has drawn is measured against
	// this, so returning a home here would measure a travel against a gamma.
	return viewDriftY();
}

double ExploreStage::travelYAtScreenY(float y) const {
	const StageDisk d = disk();
	const float extent = chartExtent(d);
	if (extent <= 0.0f) {
		return viewCentre();
	}
	// Home grows UPWARD: drift flies you up over the landscape.
	return viewCentre() + static_cast<double>(d.centre().y - y) / static_cast<double>(extent)
	                          * (homeSpanZones() / 1023.0);
}

float ExploreStage::yAtTravelY(double home) const {
	const StageDisk d = disk();
	const float extent = chartExtent(d);
	return d.centre().y
	       - static_cast<float>((home - viewCentre()) / (homeSpanZones() / 1023.0)) * extent;
}

double ExploreStage::travelXAtScreenX(float x) const {
	const StageDisk d = disk();
	const float extent = chartExtent(d);
	if (extent <= 0.0f) {
		return 0.0;
	}
	return travelCentre() + static_cast<double>(x - d.centre().x) / static_cast<double>(extent)
	                            * travelSpan();
}

float ExploreStage::xAtTravelX(double travel) const {
	const StageDisk d = disk();
	return d.centre().x
	       + static_cast<float>((travel - travelCentre()) / travelSpan()) * chartExtent(d);
}

/// **BOTH ARGUMENTS ARE TRAVEL** (backlog GY) — the vertical one first, because that is the order the
/// projection composes them in. It used to be `(home, travel)`, and every caller that kept passing a
/// home after the axes changed drew its mark at a place the chart is not of: the star sat about eight
/// view-heights off centre and pinned to the rim, which showed as the puck stuck on the bottom of the
/// disc.
visage::Point ExploreStage::glassAt(double travelY, double travelX) const {
	return Bubble::toGlass(disk(), { xAtTravelX(travelX), yAtTravelY(travelY) });
}

visage::Point ExploreStage::flatAt(visage::Point glass) const {
	return Bubble::toFlat(disk(), glass);
}

std::shared_ptr<const TerrainLikeState> ExploreStage::likesDrawn() const {
	const std::shared_ptr<const TerrainSnapshot> drawn = terrain_.snapshot();
	return drawn != nullptr ? drawn->likeState : nullptr;
}

WarpPoint ExploreStage::viewOf(double travelX, double travelY) const {
	const std::shared_ptr<const TerrainLikeState> likes = likesDrawn();
	return likes != nullptr ? likes->warp.viewAt({ travelX, travelY }) : WarpPoint { travelX, travelY };
}

WarpPoint ExploreStage::worldOf(double travelX, double travelY) const {
	const std::shared_ptr<const TerrainLikeState> likes = likesDrawn();
	return likes != nullptr ? likes->warp.worldAt({ travelX, travelY }) : WarpPoint { travelX, travelY };
}

double ExploreStage::viewDrift() const {
	return viewOf(selectedDrift(), selectedDriftY()).x;
}

double ExploreStage::viewDriftY() const {
	return viewOf(selectedDrift(), selectedDriftY()).y;
}

visage::Point ExploreStage::sheetAtWorld(double travelX, double travelY) const {
	const WarpPoint at = viewOf(travelX, travelY);
	return { xAtTravelX(at.x), yAtTravelY(at.y) };
}

visage::Point ExploreStage::glassAtWorld(double travelY, double travelX) const {
	return Bubble::toGlass(disk(), sheetAtWorld(travelX, travelY));
}

void ExploreStage::setZoom(double zoom) {
	const double clamped = clampZoom(zoom);
	if (clamped == zoom_) {
		return;
	}
	zoom_ = clamped;
	zoomMovedAt_ = clock_ ? clock_() : 0;
	if (notifier_ != nullptr) {
		char printed[32] = {};
		char reading[24] = {};
		formatZoom(reading, sizeof(reading), zoom_);
		std::snprintf(printed, sizeof(printed), "Zoom %s", reading);
		notifier_->say(printed);
	}
	repaint();
}

//-----------//
// The sound //
//-----------//

clap_id ExploreStage::paramOf(SoundField field) const {
	return static_cast<clap_id>(soundParam(selection_.slot(), field));
}

double ExploreStage::gammaSum() const {
	// ALL THREE HALVES OF GAMMA, and the base is the one that arrived last. The per-sound parameter is
	// the VERNIER (±4, automatable), the global one is the frame (A24), and the base is the unbounded
	// coarse half the corner now carries — see sound_gamma.h. Home is the sum, so the band this chart
	// draws, the floor and ceiling a scrub clamps to, and the rim marker all follow the base without
	// another line: they are all this one number.
	//
	// IN DOUBLE, and it stays in double. Where this feeds a float — the phase offset a texel is built
	// at — the base is split back out and passed on its own road; see askForTerrain.
	return bridge_.value(paramOf(SoundField::Gamma))
	       + bridge_.value(static_cast<clap_id>(ParamId::GlobalGamma)) + selectedGammaBase();
}


double ExploreStage::selectedGammaBase() const {
	return bridge_.soundGamma().base[std::min(selection_.slot(), kMaxSounds - 1)];
}

float ExploreStage::gammaDialPosition(double gamma) {
	// A COMPASS, like rotate's, and for the same reason: the base is unbounded, so there is no end of
	// travel for a dial to point at. One full turn is the vernier's whole range, which makes the face
	// move at a rate the hand can read against the number beside it. Same Euclidean remainder as
	// rotateDialPosition — `fmod` would clamp the negative half to the bottom of the dial.
	const double turns = gamma / kGammaTurn + 0.5;
	if (!std::isfinite(turns)) {
		return 0.0f;
	}
	return static_cast<float>(turns - std::floor(turns));
}

SoundIdentity ExploreStage::identityOf(size_t slot) const {
	// One reading of what belongs to a sound, shared with the cards' clone verb — see sound_identity.h.
	return readIdentity(bridge_, slot);
}

SoundIdentity ExploreStage::identity() const {
	return identityOf(selection_.slot());
}

double ExploreStage::selectedHome() const {
	const SoundIdentity id = identity();
	// Through the sound's own three gammas, base included — `homeOf` sums in double, so the coarse
	// half reaches home intact here exactly as it does in the engine.
	return homeOf(id.zone, id.offset,
	              id.gamma + bridge_.value(static_cast<clap_id>(ParamId::GlobalGamma))
	                  + id.gammaBase);
}

double ExploreStage::selectedDrift() const {
	return bridge_.soundTravel().offset[selection_.slot()].x;
}

/// **THE OTHER HALF OF THE SOUND'S PLACE** (backlog GY). `SoundTravel::offset` has always been an
/// `AnchorPoint` with x AND y, both saved and both decoded by the engine; this surface only ever read
/// the x, because its vertical axis was home. By design the chart is now a window on the decoded
/// plane and this is the coordinate the vertical axis draws.
double ExploreStage::selectedRadius() const {
	return bridge_.soundTravel().orbit[selection_.slot()].radius;
}

double ExploreStage::selectedTheta() const {
	return bridge_.soundTravel().orbit[selection_.slot()].theta;
}

double ExploreStage::selectedDriftY() const {
	return bridge_.soundTravel().offset[selection_.slot()].y;
}

/// **WHERE THE SELECTED SOUND IS PLAYED — its authored place with the engine's live Radius/Theta pull
/// added** (a modulated sound visibly orbits: it moves, but leaves no breadcrumbs and does not
/// re-centre the view). Through `pulledPlace`, the function the engine itself places
/// the sound with, fed the pull the engine published — so this is a report, not a second opinion.
///
/// **ONLY THE DRAWING READS THIS** — the star, and the planet's light that follows the star. Every
/// GESTURE still reads `selectedDrift`/`selectedDriftY`: a drag moves the AUTHORED place, a crumb
/// records the authored place, and the view follows the authored place (`update`'s automation road
/// already says why — the view follows identity and never performance, and a modulator is
/// performance). That split is the whole of "no breadcrumbs, no recentre": nothing that leaves a
/// crumb or moves the view can see the pull.
AnchorPoint ExploreStage::playedPlace() const {
	const size_t slot = selection_.slot();
	const TravelPulls pulls = bridge_.travelPulls();
	const SoundTravel travel = bridge_.soundTravel();
	AnchorPoint at = pulledPlace(travel.offset[slot], travel.orbit[slot], pulls.radius[slot], pulls.theta[slot]);
	// A KIT PAD PLAYS AT ITS PLACE MOVED BY THE WALK (drum proposal D3, character_params' road), so the played mark and its
	// arrow are the Walk's on the selected pad.
	if (drumKitShown()) {
		at.x += bridge_.value(static_cast<clap_id>(ParamId::DrumWalkX));
		at.y += bridge_.value(static_cast<clap_id>(ParamId::DrumWalkY));
	}
	return at;
}

float ExploreStage::rotateDialPosition(double radians) {
	// **A FULL TURN IS 2*PI, AND IT IS NOT THE CORNER'S GEARING.** This read `2.0 * kRotateSpan`,
	// which was 2*pi only because the gearing happened to be pi. The two are different quantities:
	// `kRotateSpan` is how far one encoder travel turns the plane (a statement about the HAND) and
	// this is the period the compass wraps on (a statement about ANGLES). Tying them meant that
	// slowing the corner down — backlog GF, 2026-09-20 — would have made the dial lie, sweeping its
	// whole face for a quarter turn of the plane while the degree text beside it read the truth. The
	// case that says those two can never disagree is what caught it.
	constexpr double kFullTurn = 2.0 * 3.14159265358979323846;
	const double turns = radians / kFullTurn + 0.5;
	// FLOOR, NOT fmod. `x - floor(x)` is the Euclidean remainder and lands in [0, 1) for every finite
	// input, positive or negative; `fmod` keeps the sign of its argument and returns (−1, 1), so the
	// negative half of the range then clamped to the bottom of the dial. Nothing to clamp here — the
	// result is in range by construction.
	if (!std::isfinite(turns)) {
		return 0.0f;
	}
	return static_cast<float>(turns - std::floor(turns));
}

double ExploreStage::selectedRotation() const {
	// THE READOUT IS THE TURN ITSELF, which is what `rotateFor` means and why it is called rather than
	// read off a derived plane. An unturned sound reads 0 — not because its plane is closed (rotate 0
	// is the open plane its home gives it) but because nobody has turned it.
	const SliceOrientationPin own = bridge_.soundOrientations().pin[selection_.slot()];
	return rotateFor(own.pinned ? own : bridge_.orientation());
}

std::string ExploreStage::rotateReadout() const {
	return device::printDegrees(selectedRotation());
}

double ExploreStage::selectedTravel() const {
	// THE ENGINE'S OWN NUMBER, worked out the way character_params.h works it out: the sound's own
	// drift, plus the tour walked over the enabled constellation with Pos X/Y stepping off it, with
	// Weather deciding how much of the gap between the puck and this sound's anchor becomes travel.
	// Added rather than scaled, by design: drift is what the sound IS and the rest is where
	// you are standing.
	AnchorPoint anchors[kMaxSounds];
	size_t count = 0;
	std::optional<size_t> mine;
	for (size_t slot = 0; slot < kMaxSounds; ++slot) {
		const double enable =
		    bridge_.modulated(static_cast<clap_id>(soundParam(slot, SoundField::Enable)));
		if (steppedIndex(enable, kEnableNames.size()) == 0) {
			continue;
		}
		if (slot == selection_.slot()) {
			mine = count;
		}
		anchors[count] = { bridge_.modulated(static_cast<clap_id>(soundParam(slot, SoundField::PlaceX))),
			               bridge_.modulated(
			                   static_cast<clap_id>(soundParam(slot, SoundField::PlaceY))) };
		count++;
	}
	if (!mine.has_value()) {
		// A switched-off sound takes no part in the blend, so the puck cannot carry it anywhere — but it
		// keeps its own drift, because drift is what the sound is rather than where anyone is standing.
		return selectedDrift();
	}
	const std::span<const AnchorPoint> live { anchors, count };
	const AnchorPoint onTour = tourPoint(live, bridge_.modulated(static_cast<clap_id>(ParamId::Path)));
	const double puckX = onTour.x + bridge_.modulated(static_cast<clap_id>(ParamId::PosX));
	const double weather = bridge_.modulated(static_cast<clap_id>(ParamId::Weather));
	return selectedDrift() + weather * (puckX - anchors[*mine].x);
}

//-----------//
// The verbs //
//-----------//

void ExploreStage::commit(clap_id id, double value) {
	if (bridge_.value(id) == value) {
		return;
	}
	if (std::find(gesturesOpen_.begin(), gesturesOpen_.end(), id) == gesturesOpen_.end()) {
		bridge_.beginGesture(id);
		gesturesOpen_.push_back(id);
	}
	bridge_.setValue(id, value);
}

void ExploreStage::commitDrift(size_t slot, double x, double y) {
	const AnchorPoint now = bridge_.soundTravel().offset[std::min(slot, kMaxSounds - 1)];
	if (now.x == x && now.y == y) {
		return;
	}
	// Its own bracket, and not the parameters' bracket: there is no CLAP parameter here for a gesture
	// to be about. What this one does is mark the plugin's STATE dirty when the drag ends, so the host
	// knows the project changed — without it a drifted sound is a sound that silently fails to save.
	if (!driftGestureOpen_) {
		bridge_.beginSoundTravelGesture();
		driftGestureOpen_ = true;
	}
	// THE LAYER'S LOCK (drum proposal §12, stage (c)): the layer stands at its pad's place plus its offset vector at every
	// read, so LOCKED it follows this drag with no write at all. UNLOCKED it stays where it stands: the vector takes the drag
	// back, by exactly the pad's move (clamped only at the vector's own range).
	if (drumKitShown() && slot < kMaxSounds) {
		const auto id = [slot](DrumControl c) { return static_cast<clap_id>(drumParam(slot, c)); };
		const bool layered = std::lround(bridge_.value(id(DrumControl::LayerKind))) > 0;
		const bool locked = std::lround(bridge_.value(id(DrumControl::LayerLock))) != 0;
		if (layered && !locked) {
			commit(id(DrumControl::LayerOffsetX), std::clamp(bridge_.value(id(DrumControl::LayerOffsetX)) - (x - now.x), -1.0, 1.0));
			commit(id(DrumControl::LayerOffsetY), std::clamp(bridge_.value(id(DrumControl::LayerOffsetY)) - (y - now.y), -1.0, 1.0));
		}
	}
	bridge_.setSoundTravel(slot, { x, y });
}

void ExploreStage::rotateSelectedBy(double radians) {
	if (radians == 0.0) {
		return;
	}
	// ONE SCALAR, WRITTEN AS ONE SCALAR. This used to spread the turn across six stored angles at
	// φ⁻ᵏ ratios; the decode reads one turn, and ratio[0] = 1 is why every patch the old spread wrote
	// migrates exactly — the turn a hand produced IS the first angle.
	SliceRotation delta;
	delta.rotate = static_cast<float>(radians);
	// THE DERIVING OVERLOAD, and it matters which one. The three-argument form takes the plane to turn
	// FROM — correct and dangerous, because a surface that has not worked out which plane the sound is
	// on passes a default-constructed generator, which is the DEGENERATE plane, and the sound jumps to
	// it silently on the first click of the encoder. This one works it out from the sound's own pin,
	// the global pin, or the home, which is exactly what the engine will do with the result.
	const SoundIdentity id = identity();
	const auto zone = static_cast<uint16_t>(std::lround(id.zone));
	const auto phaseOffset =
	    static_cast<float>(id.offset + id.gamma
	                       + bridge_.value(static_cast<clap_id>(ParamId::GlobalGamma)));
	//
	// AND IT BANKS WHAT THE TURN WOULD HAVE MOVED. The 2026-08-13 decision superseded A10: a
	// rotation must leave the sound you are standing on alone, and on this chart every drag that is not
	// exactly vertical commits drift — so a turn that wrote only the plane would move the standing sound
	// almost every time. The drift is where it is standing, read here and passed in.
	const size_t slot = std::min(selection_.slot(), kMaxSounds - 1);
	const RotatedSound turned =
	    rotateSoundBy(bridge_.soundOrientations(), bridge_.soundResidue(), slot, zone, phaseOffset,
	                  bridge_.orientation(), delta, bridge_.soundTravel().offset[slot],
	                  // Where this sound has been carried to. Without it the turn is banked against
	                  // the plane of a sound that is not this one.
	                  bridge_.soundGamma().base[slot]);

	if (!planeGestureOpen_) {
		bridge_.beginSoundOrientationGesture();
		planeGestureOpen_ = true;
	}
	// Both writes, inside the one bracket: the plane alone is the behaviour that was rejected, and the
	// residue alone is a sound standing somewhere its plane never was.
	bridge_.setSoundOrientation(slot, turned.planes.pin[slot]);
	bridge_.setSoundResidue(slot, turned.residue.banked[slot]);
	repaint();
}

float ExploreStage::regearCorner(const visage::MouseEvent& e) {
	// **SHIFT MID-DRAG RE-ANCHORS; IT NEVER RE-READS THE TRAVEL ALREADY COVERED** (found 2026-09-26 while
	// hunting rotate that sometimes went the wrong way). The corners send `moved - grabValue_`, and `moved` is
	// measured from `grabY_`. Re-measured at the new gearing, the pixels already sent were suddenly worth a
	// fifth (or five times) as much, and the difference went out as ONE TURN BACKWARD while the hand was
	// still going forward: 100 px up and then Shift was -0.44 rad of Rotate. So a change of gearing starts
	// the count again from the LAST event, and the pixels since then are geared as this event says.
	// Everything sent before is already in the state of record, so nothing is lost or counted twice.
	const bool fine = (e.modifiers & visage::kModifierShift) != 0;
	if (fine != grabFine_) {
		grabFine_ = fine;
		grabY_ = grabLastY_;
		grabValue_ = 0.0;
	}
	grabLastY_ = e.position.y;
	return fine ? kEncoderTravel / Knob::kFineFactor : kEncoderTravel;
}

void ExploreStage::endGesture() {
	for (const clap_id id : gesturesOpen_) {
		bridge_.endGesture(id);
	}
	gesturesOpen_.clear();
	if (driftGestureOpen_) {
		bridge_.endSoundTravelGesture();
		driftGestureOpen_ = false;
	}
	if (planeGestureOpen_) {
		bridge_.endSoundOrientationGesture();
		planeGestureOpen_ = false;
	}
	if (gammaGestureOpen_) {
		bridge_.endSoundGammaGesture();
		gammaGestureOpen_ = false;
	}
}

void ExploreStage::nudgeSelectedGamma(double delta) {
	if (delta == 0.0) {
		return;
	}
	const size_t slot = std::min(selection_.slot(), kMaxSounds - 1);
	if (!gammaGestureOpen_) {
		bridge_.beginSoundGammaGesture();
		gammaGestureOpen_ = true;
	}
	// A PURE FUNCTION ON THE STATE OF RECORD, like driftBy and rotateBy: the carried value is computed
	// from the stored one rather than by adding into a number the surface keeps, so a breadcrumb
	// restores it exactly instead of approximately.
	bridge_.setSoundGamma(slot, nudgeGammaBase(bridge_.soundGamma(), slot, delta).base[slot]);
	repaint();
}

/// **BOTH HALVES OF THE PLACE, IN ONE WRITE** (backlog GY). The chart's two axes are the two
/// components of the sound's travel, so a press or a drag names a POINT and this commits it. Writing
/// them through two separate calls would bracket two gestures for one movement and leave an undo
/// entry that walks back half a drag.
void ExploreStage::driftToXY(double driftX, double driftY) {
	commitDrift(selection_.slot(), driftX, driftY);
	repaint();
}

void ExploreStage::driftTo(double drift) {
	// WHAT THE POINTER ASKED FOR, WHOLE. The chart's horizontal axis is the sound's own drift since
	// the GY refactor, so the value under the hand IS the value written — no performance term to
	// subtract, and therefore no term that a host could move mid-drag to write its inverse into the
	// patch. The compensation this line used to carry, and the captured value that made it safe, are
	// described in the header; both are gone with the thing they compensated for.
	const AnchorPoint now = bridge_.soundTravel().offset[selection_.slot()];
	commitDrift(selection_.slot(), drift, now.y);
	repaint();
}


double ExploreStage::mutationZones() const {
	// ZOOM IS THE MUTATION SCALE, at every zoom: a quarter of the visible span. No cap since the
	// 2026-10-10 decision — the reasoning, and the cap it replaced, are at `mutate` in the header.
	return homeSpanZones() / 4.0;
}

void ExploreStage::polarGestureBegan() {
	// WHERE THE SOUND STOOD, so the close can record one undo step for the whole turn.
	departure_ = identity();
}

void ExploreStage::polarGestureEnded(TravelAxis axis) {
	// **ONE UNDO STEP, NO CRUMB, NO RE-CENTRE** (Radius/Theta changes leave no breadcrumbs and do not
	// re-centre the view), and the two refusals have one
	// reason. These are the only controls that move the sound CONTINUOUSLY across the plane, so a
	// crumb per gesture is a crumb per knob twiddle — the trail fills with places nobody chose to
	// visit — and a view that pans to follow the puck puts it back in the middle and shows nothing
	// moving, which is the whole point of turning the knob. The undo entry stays: the decision covers the
	// trail and the centring, not the history.
	const SoundIdentity now = identity();
	if (departure_.has_value() && !(*departure_ == now)) {
		record(selection_.slot(), *departure_, now, axis == TravelAxis::Radius ? "radius" : "theta");
	}
	departure_.reset();
	askForTerrain();
	repaint();
}

double ExploreStage::mutationTravel() const {
	// The same quarter, in travel at the same scale — A13's matched-span rule applies to the step as
	// much as to the picture, or a roll would be a small move up the chart and an enormous one across.
	return travelSpan() / 4.0;
}

void ExploreStage::mutate() {
	// `mutationZones()` went with the home roll — both axes are travel now and both take
	// `mutationTravel()`, which is the one reach the visible plane has.
	// A plain xorshift, so a dice roll is reproducible from a seed and the surface owns no global.
	diceState_ ^= diceState_ << 13;
	diceState_ ^= diceState_ >> 17;
	diceState_ ^= diceState_ << 5;
	const double unit = static_cast<double>(diceState_) / 4294967296.0 * 2.0 - 1.0;

	diceState_ ^= diceState_ << 13;
	diceState_ ^= diceState_ >> 17;
	diceState_ ^= diceState_ << 5;
	const double sideways = static_cast<double>(diceState_) / 4294967296.0 * 2.0 - 1.0;

	const SoundIdentity before = identity();
	// **THE DICE ROLLS THE PLANE YOU CAN SEE** (backlog GY). It used to roll HOME on this axis — the
	// gamma base — which was the chart's vertical when the chart drew home. By design the
	// vertical is travel-Y, so a roll of home would move the sound to a country the picture is not of:
	// you would press the dice and watch a chart that did not change while the sound did. The
	// condition for the whole refactor was that the terrain encode is honest — where you click is where
	// you go — and a verb that lands you somewhere undrawn breaks it as surely as a bad projection.
	//
	// GAMMA IS STILL IDENTITY AND IS STILL ROLLABLE — by its own corner, deliberately, because moving
	// the whole plane is a different gesture from moving within it. Whether the dice should ALSO take
	// a country now and then is a taste question rather than a consequence of this change.
	//
	// **AND THE ROLL IS DRAWN ON THE WARPED PLANE** (backlog ON stage 4): the same zoom-scaled draw, from
	// the same dice, taken from where the star is DRAWN and carried back through the inverse warp — so in real terms the
	// roll's density picks up the warp's area factor and the likes steer it without a rule of their own. Without likes both
	// maps are the identity and this is the old sum, to the bit.
	const WarpPoint from = viewOf(selectedDrift(), selectedDriftY());
	const WarpPoint to = worldOf(from.x + sideways * mutationTravel(), from.y + unit * mutationTravel());
	driftToXY(to.x, to.y);
	// BOTH AXES, because both are identity. A dice that only rolled home would leave the plane's own
	// dimension out of the one verb whose whole job is to find somewhere you would not have looked.
	// FROM THE DRIFT, not from where the puck has the sound: a roll from the performance point would
	// bank whatever Weather was doing into the patch, and turning Weather down afterwards would leave
	// the sound somewhere nobody rolled.
	endGesture();
	const SoundIdentity after = identity();
	if (!(before == after)) {
		record(selection_.slot(), before, after, "mutate");
		plantCrumb(selection_.slot(), before);
	}
	recentre();
	if (notifier_ != nullptr) {
		notifier_->say("Mutated");
	}
}

namespace {


/// **A LIKE MADE OR REMOVED, AS ONE UNDO STEP** — the whole list either side, so a replay restores exactly what was
/// there, including a like's captured envelope (which nothing could re-measure: the knobs may have moved since).
class LikesEdit final : public EditRecord {
public:
	/// `log` writes the like-set log's deltas for an undo or a redo (the stage's own logger, which owns the history).
	using Log = std::function<void(const char* cause, const TerrainLikes& from, const TerrainLikes& to)>;
	LikesEdit(ParamBridge& bridge, TerrainLikes before, TerrainLikes after, const char* verb, Log log)
	    : bridge_(bridge), before_(std::move(before)), after_(std::move(after)), verb_(verb), log_(std::move(log)) {}
	void undo() const override {
		bridge_.setTerrainLikes(before_);
		log_("undo", after_, before_);
	}
	void redo() const override {
		bridge_.setTerrainLikes(after_);
		log_("redo", before_, after_);
	}
	const char* verb() const override { return verb_; }

private:
	ParamBridge& bridge_;
	TerrainLikes before_;
	TerrainLikes after_;
	const char* verb_;
	Log log_;
};

} // namespace

std::optional<size_t> ExploreStage::likeUnderStar() const {
	const std::vector<TerrainLike>& likes = bridge_.terrainLikes().likes;
	if (likes.empty()) {
		return std::nullopt;
	}
	const TerrainWindow window = terrainWindowNow();
	const visage::Point star = glassAt(viewDriftY(), viewDrift());
	std::optional<size_t> nearest;
	float best = kLikeTouchPixels;
	for (size_t i = 0; i < likes.size(); ++i) {
		if (!likeApplies(likes[i])) {
			continue;
		}
		const PlanePlace place = terrainLikePlaceOn(window, likes[i]);
		// THE HIT TEST USES THE PROJECTED PLACE, and a like whose projection is off the chart cannot be under the star.
		const visage::Point flat = sheetAtWorld(place.x, place.y);
		if (!std::isfinite(flat.x) || !std::isfinite(flat.y) || !disk().contains(flat)) {
			continue;
		}
		const visage::Point at = glassAtWorld(place.y, place.x);
		const float apart = std::hypot(at.x - star.x, at.y - star.y);
		if (apart <= best) {
			best = apart;
			nearest = i;
		}
	}
	return nearest;
}

void ExploreStage::toggleLike() {
	const TerrainLikes before = bridge_.terrainLikes();
	TerrainLikes after = before;
	const char* verb = "like";
	TerrainLike changed;
	if (const std::optional<size_t> on = likeUnderStar()) {
		changed = after.likes[*on];
		after.likes.erase(after.likes.begin() + static_cast<std::ptrdiff_t>(*on));
		verb = "unlike";
	}
	else {
		if (after.likes.size() >= kMaxTerrainLikes) {
			// SAID, NOT SILENT: the stored layout holds kMaxTerrainLikes, and a press that did nothing would look broken.
			if (notifier_ != nullptr) {
				notifier_->say("Likes full: remove one first");
			}
			return;
		}
		changed = stanceNow();
		after.likes.push_back(changed);
		// A LIKE MAKES ITS STANCE A WALK LINE TOO, settled or not, if the walk has not logged it (likes_log.h).
		if (!likeslog::directory().empty()) {
			logTick();
			if (walkLogged_ != walkStance()) {
				logWalkNow(logNow());
			}
		}
	}
	bridge_.setTerrainLikes(after);
	logLikesEdit(verb, before, after);
	history_.record(std::make_unique<LikesEdit>(
	    bridge_, before, after, verb,
	    [this](const char* cause, const TerrainLikes& from, const TerrainLikes& to) { logLikesEdit(cause, from, to); }));
	if (notifier_ != nullptr) {
		notifier_->say(verb == std::string_view("like") ? "Liked" : "Like removed");
	}
	repaint();
}

bool ExploreStage::likeApplies(const TerrainLike& like) const {
	const OscType engine = oscTypeFromValue(bridge_.value(static_cast<clap_id>(ParamId::OscType)));
	// ON DRUM, ALSO ITS KIND (likes v8): a pad switched to another kind is not drawn or warped by its old kind's likes.
	if (engine == OscType::Drum) {
		const bool kit = steppedIndex(bridge_.value(static_cast<clap_id>(ParamId::DrumMode)), kDrumModeNames.size())
		                 == static_cast<size_t>(DrumMode::Kit);
		const clap_id kindId = kit ? static_cast<clap_id>(drumParam(selection_.slot(), DrumControl::Kind))
		                           : static_cast<clap_id>(ParamId::DrumPitchedKind);
		if (like.kind != static_cast<uint16_t>(steppedIndex(bridge_.value(kindId), kDrumKindNames.size()))) {
			return false;
		}
	}
	return like.engine == static_cast<uint16_t>(engine)
	       && like.slot == static_cast<uint16_t>(selection_.slot());
}

visage::PopupMenu ExploreStage::likesMenu() {
	// SAYS WHOSE LIKES IT CLEARS: this engine's, for this sound — every other terrain keeps its own.
	const OscType engine = oscTypeFromValue(bridge_.value(static_cast<clap_id>(ParamId::OscType)));
	const size_t index = static_cast<size_t>(engine);
	char caption[96] = {};
	std::snprintf(caption, sizeof(caption), "Clear all likes on %s, sound %c",
	              index < kOscTypeNames.size() ? kOscTypeNames[index] : "this engine",
	              static_cast<char>('A' + selection_.slot()));
	bool any = false;
	for (const TerrainLike& like : bridge_.terrainLikes().likes) {
		any = any || likeApplies(like);
	}
	visage::PopupMenu menu;
	menu.addOption(kClearLikes, caption).enable(any);
	// THE LIKES' STRENGTH: how strongly every like pulls its sound's warp (×1/×2/×4; stage state, not saved).
	menu.addBreak();
	for (const int s : { 1, 2, 4 }) {
		char label[48] = {};
		std::snprintf(label, sizeof(label), "Like strength \xc3\x97%d", s);
		menu.addOption(kLikeStrength + s, label).select(likeStrength_ == s);
	}
	menu.onSelection() = [this](int id) {
		if (id == kClearLikes) {
			clearLikes();
		}
		else if (id == kLikeStrength + 1 || id == kLikeStrength + 2 || id == kLikeStrength + 4) {
			likeStrength_ = id - kLikeStrength;
			redraw();
		}
	};
	return menu;
}

void ExploreStage::clearLikes() {
	const TerrainLikes before = bridge_.terrainLikes();
	TerrainLikes after;
	for (const TerrainLike& like : before.likes) {
		if (!likeApplies(like)) {
			after.likes.push_back(like);
		}
	}
	if (after == before) {
		return; // nothing of this terrain's to clear; not an edit
	}
	bridge_.setTerrainLikes(after);
	logLikesEdit("clear", before, after);
	history_.record(std::make_unique<LikesEdit>(
	    bridge_, before, after, "clear likes",
	    [this](const char* cause, const TerrainLikes& from, const TerrainLikes& to) { logLikesEdit(cause, from, to); }));
	if (notifier_ != nullptr) {
		notifier_->say("Likes cleared");
	}
	repaint();
}

TerrainLike ExploreStage::stanceNow() const {
	// THE SOUND WHERE IT STANDS, measured through the builder's own road at this instant — its place in real coordinates
	// (never a warped view's), the plane's home, and the envelope. Captured, so a knob turned later cannot rewrite what was
	// liked.
	const TerrainWindow window = terrainWindowNow();
	TerrainLike like;
	like.x = selectedDrift();
	like.y = selectedDriftY();
	like.home = terrainPlaneHome(window);
	like.engine = static_cast<uint16_t>(window.engine);
	like.slot = static_cast<uint16_t>(selection_.slot());
	// AND ON DRUM ITS KIND (likes v8): a like is of the drum it was made on.
	like.kind = window.engine == OscType::Drum ? static_cast<uint16_t>(window.additive.drumKind) : 0;
	like.spectrum = terrainFineSpectrumAt(window, like.x, like.y);
	// AND THE FULL POINT, which is where the like IS on every later plane (terrainLikePlaceOn).
	like.pointCount = static_cast<uint16_t>(terrainPlacePoint(window, like.x, like.y, like.point));
	// AND THE ANGLES AS HEARD, what the phase warp's fit reads (TerrainLike::angles).
	terrainLikeAngles(window, like.x, like.y, like.angles);
	return like;
}

double ExploreStage::logNow() const {
	return logClock_ ? logClock_() : std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::array<double, 4> ExploreStage::walkStance() const {
	return { static_cast<double>(oscTypeFromValue(bridge_.value(static_cast<clap_id>(ParamId::OscType)))),
		     static_cast<double>(selection_.slot()), selectedDrift(), selectedDriftY() };
}

void ExploreStage::logLikesEdit(const char* cause, const TerrainLikes& before, const TerrainLikes& after) {
	if (likeslog::directory().empty()) {
		return;
	}
	const double now = logNow();
	// THE LOG MUST DESCRIBE `before` for the deltas to mean anything: if it does not (no snapshot yet, a load since), the
	// snapshot comes first.
	if (!likesLogged_.has_value() || !(*likesLogged_ == before)) {
		likeslog::Record snapshot;
		snapshot.time = now;
		snapshot.session = likeslog::session();
		snapshot.event = "snapshot";
		snapshot.strength = likeStrength_;
		snapshot.likes = before.likes;
		(void)likeslog::append(snapshot);
	}
	for (likeslog::Record& delta : likeslog::deltas(before, after)) {
		delta.time = now;
		delta.session = likeslog::session();
		delta.cause = cause;
		delta.strength = likeStrength_;
		(void)likeslog::append(delta);
	}
	likesLogged_ = after;
}

void ExploreStage::logWalkNow(double now) {
	likeslog::Record record;
	record.time = now;
	record.session = likeslog::session();
	record.event = "walk";
	record.like = stanceNow();
	record.engine = record.like->engine;
	record.slot = record.like->slot;
	record.strength = likeStrength_;
	record.held = now - walkSince_;
	if (walkLogged_.has_value()) {
		record.dwell = walkLoggedLeft_.value_or(now) - walkLoggedSince_;
	}
	(void)likeslog::append(record);
	walkLogged_ = walkAt_;
	walkLoggedSince_ = walkSince_;
	walkLoggedLeft_.reset();
}

void ExploreStage::logTick() {
	if (likeslog::directory().empty()) {
		return;
	}
	const double now = logNow();
	// A SET THE LOG HAS NOT DESCRIBED — the first frame, a patch load, the host's own undo — is written whole.
	if (const TerrainLikes& set = bridge_.terrainLikes(); !likesLogged_.has_value() || !(*likesLogged_ == set)) {
		likeslog::Record snapshot;
		snapshot.time = now;
		snapshot.session = likeslog::session();
		snapshot.event = "snapshot";
		snapshot.strength = likeStrength_;
		snapshot.likes = set.likes;
		(void)likeslog::append(snapshot);
		likesLogged_ = set;
	}
	// THE WALK: any move restarts the stance's stillness (and marks when the logged one was left); a stance new to the log
	// is written once it has been held still long enough.
	const std::array<double, 4> stance = walkStance();
	if (walkAt_ != stance) {
		if (walkAt_.has_value() && walkAt_ == walkLogged_) {
			walkLoggedLeft_ = now;
		}
		walkAt_ = stance;
		walkSince_ = now;
	}
	if (walkLogged_ != stance && now - walkSince_ >= likeslog::kWalkDwellSeconds) {
		logWalkNow(now);
	}
}

void ExploreStage::record(size_t slot, const SoundIdentity& before, const SoundIdentity& after,
                          const char* verb) {
	// EVERYTHING THAT IS NOT THE IDENTITY RIDES ALONG UNCHANGED, and that is not a formality. An
	// entry carries a slot's whole state now, because the cards' clone changes the switch as well as
	// the identity; nothing this surface does touches the switch, the stances, the placement or the
	// width, so both halves of the entry carry what the slot has — which is exactly what makes
	// walking one of these entries leave them alone (writeSlotState skips values that match).
	//
	// READ WHOLE, NOT ASSEMBLED. This used to build `{ identity, on }` and let the rest of the
	// struct default — so undoing a scrub wrote the DEFAULT placement and an empty stance list over
	// whatever the slot had authored: the sound snapped to the origin and forgot its places. The
	// compiler had been saying so since `place` joined the struct (-Wmissing-field-initializers on
	// this very line); the per-sound width joining the struct is what made somebody read the
	// warning. Exactly the half-a-slot failure SlotState exists to make impossible, one call site
	// short of impossible.
	const SlotState rest = readSlotState(bridge_, slot);
	SlotState was = rest;
	was.identity = before;
	SlotState now = rest;
	now.identity = after;
	recordSlot(history_, bridge_, slot, was, now, verb);
}

void ExploreStage::plantCrumb(size_t slot, const SoundIdentity& at) {
	if (slot >= kMaxSounds) {
		return;
	}
	std::vector<SoundIdentity>& trail = crumbs_[slot];
	if (!trail.empty() && trail.back() == at) {
		return;
	}
	trail.push_back(at);
	if (trail.size() > kMaxCrumbs) {
		trail.erase(trail.begin());
	}
}

PlanePlace ExploreStage::likePlace(size_t index) const {
	const std::vector<TerrainLike>& likes = bridge_.terrainLikes().likes;
	if (index >= likes.size()) {
		return {};
	}
	return terrainLikePlaceOn(terrainWindowNow(), likes[index]);
}

PlanePlace ExploreStage::crumbPlace(size_t index) const {
	const std::vector<SoundIdentity>& trail = crumbs_[selection_.slot()];
	if (index >= trail.size()) {
		return {};
	}
	const SoundIdentity& crumb = trail[index];
	// THE CRUMB'S OWN PLANE: the window the chart would be drawn in had the sound stood at that identity — its turn, its
	// residue, its dials and gamma base — and the point its drift decodes to there. That point is where the crumb IS.
	const TerrainWindow now = terrainWindowNow();
	TerrainWindow then = now;
	then.pin = crumb.plane.pinned ? crumb.plane : bridge_.orientation();
	then.residue = crumb.residue;
	then.gammaBase = crumb.gammaBase;
	then.zoneBaseline = crumb.zone;
	then.offsetBaseline = crumb.offset;
	std::array<double, kTerrainLikePoint> point {};
	const size_t count = terrainPlacePoint(then, crumb.driftX, crumb.driftY, point);
	if (count == 0) {
		return { crumb.driftX, crumb.driftY, 0.0, false };
	}
	return terrainPlaceOn(now, std::span(point).first(count), crumb.driftX, crumb.driftY);
}

visage::Point ExploreStage::crumbAt(size_t index) const {
	const std::vector<SoundIdentity>& trail = crumbs_[selection_.slot()];
	if (index >= trail.size()) {
		return {};
	}
	// BOTH COORDINATES FROM THE CRUMB'S OWN IDENTITY. The x used to be the LIVE travel — one number
	// shared by every crumb — so the whole trail collapsed onto a single column and slid sideways with
	// whatever the sound was doing now: moving the explore point translated the entire history
	// constellation to the coordinate of the destination. The identities were stored
	// correctly; only the projection threw the horizontal half away.
	//
	// AND NOTHING LIVE IS ADDED TO IT. This used to carry the live performance term, so that the trail
	// sat in the same frame as a star drawn at total travel; by design that frame does not
	// move any more, and adding a term that is always the same term is adding nothing. What it did
	// while the star moved was slide the entire history sideways under automated Weather, which is the
	// thing the design forbids.
	// **BOTH COORDINATES ARE THE CRUMB'S OWN TRAVEL NOW** (backlog GY). The vertical used to be the
	// crumb's HOME, reconstructed from its zone, offset and gammas — correct while the chart's vertical
	// axis was home, and nonsense the moment it became travel-Y: a trail laid down at one gamma was
	// being drawn thousands of units off the chart.
	//
	// `SoundIdentity` HAS ALWAYS STORED `driftY` and this projection has always thrown it away — the
	// same shape as the horizontal half that was caught first (moving the explore point translated the
	// entire history constellation), one axis later. Both halves of the stored place are now
	// drawn, which is what makes a crumb a place rather than a projection of one.
	// **AND THE PLACE IS THE CRUMB'S POINT PROJECTED** onto the plane being drawn (otherwise the history does not
	// move with a rotate). On the plane the crumb was planted on it is its drift, to the bit.
	const PlanePlace place = crumbPlace(index);
	return sheetAtWorld(place.x, place.y);
}

bool ExploreStage::crumbIsVisible(size_t index) const {
	const std::vector<SoundIdentity>& trail = crumbs_[selection_.slot()];
	if (index >= trail.size()) {
		return false;
	}
	const visage::Point at = crumbAt(index);
	if (!std::isfinite(at.x) || !std::isfinite(at.y)) {
		return false;
	}
	// On the chart, and nothing weaker. A crumb a little way off the rim is not "nearly here" — at ×64
	// the rim is a thousandth of the identity axis away, so anything outside it is somewhere the view
	// has no relationship with, and its mark would be a mark on another panel.
	return disk().contains(at);
}

size_t ExploreStage::visibleCrumbCount() const {
	size_t visible = 0;
	for (size_t i = 0; i < crumbs_[selection_.slot()].size(); ++i) {
		if (crumbIsVisible(i)) {
			++visible;
		}
	}
	return visible;
}

std::vector<SoundIdentity> ExploreStage::crumbs(size_t slot) const {
	return slot < kMaxSounds ? crumbs_[slot] : std::vector<SoundIdentity> {};
}

void ExploreStage::followEdit(const EditRecord& record) {
	// THE ONE CENTERING ROAD for edits that name a sound: point the surface at it and settle the
	// view on it. Shared by the history walking (editApplied) and by a live record that asks to be
	// followed (editRecorded) — two callers, one road, so the chart cannot come to centre two ways.
	const std::optional<size_t> slot = record.slot();
	if (slot.has_value()) {
		selection_.select(*slot);
		recentre();
	}
	repaint();
}

void ExploreStage::editApplied(const EditRecord& record, const char* verb) {
	// The values are already back — the record wrote them through the bridge, which is the half that
	// must not depend on any surface being alive. What is left is this surface's own business, and it
	// is the same three things applyIdentity used to do after its write.
	followEdit(record);
	if (notifier_ != nullptr) {
		// "Undo scrub" rather than "Undo", because a history shared between surfaces can be walked back
		// through somebody else's verb, and a player who is told only the direction has to go and look
		// at what moved.
		notifier_->say(std::string(verb) + " " + record.verb());
	}
}

void ExploreStage::editRecorded(const EditRecord& record) {
	// A live stance switch (V19 polish — the terrain must move and re-centre when you switch
	// variant): the same pointing as an undo, minus the tray line — the verb announced itself when
	// it ran. A same-slot switch changes no selection and no identity, so without this no road asks
	// the view to re-settle on the sound it just moved.
	followEdit(record);
}

//------------//
// The corners //
//------------//

visage::Bounds ExploreStage::partBounds(Part part) const {
	const StageDisk d = disk();
	const float side = chartExtent(d) + 2.0f * StageDisk::kRimInset;
	const float left = d.centre().x - side * 0.5f;
	const float top = d.centre().y - side * 0.5f;
	const float column = Knob::columnHeight(kCornerDiameter);
	const float polar = Knob::columnHeight(kPolarDiameter);
	const float polarPair = 2.0f * polar + kPolarGap;
	// **ONE LAYOUT FOR THE BOTTOM CORNERS AND THE BUTTONS, YIELDING IN A FIXED ORDER** (backlog LH, 2026-10-04:
	// at some scales Rotate and Theta overlapped the mutate and undo/redo buttons). The square is the stage's height or
	// its width less the flanks, whichever is smaller, so a NARROW stage — every rail open (374 wide at the design
	// size, a 202 square), or a window a host gave at a large UI scale — makes a square too small for the corners and
	// the row together. So:
	//   1. THE ROW SITS BETWEEN THE BOTTOM CORNERS while the square is wide enough for both and the row (348 plus the
	//      gaps) — the mock's place, and every stage at the design size with a rail or more closed.
	//   2. OTHERWISE THE ROW GOES UNDER THE BOTTOM CORNERS: their baseline (Rotate's, Theta's) stays on the square's
	//      foot if the stage has a row's height spare below it, rises by what it lacks if not, and drops below the foot
	//      if the square is too short for Zoom over the polar pair.
	// Every part is then clear of every other wherever the stage holds Zoom, the pair and the row below the square's
	// top (243) and the square holds two corners side by side.
	constexpr float kMutateWidth = 84.0f;
	constexpr float kStepWidth = 52.0f;
	constexpr float kLikeWidth = kButtonHeight + 8.0f;
	constexpr float kRowWidth = kLikeWidth + kButtonGap + kMutateWidth + 2.0f * (kStepWidth + kButtonGap);
	const bool rowBetween = side >= 2.0f * (kCornerWidth + kButtonGap) + kRowWidth;
	const float rightColumn = column + kButtonGap + polarPair;
	float baseline = top + std::max(side, rightColumn);
	if (!rowBetween) {
		baseline = std::max(top + rightColumn, std::min(baseline, height() - kButtonGap - kButtonHeight));
	}
	switch (part) {
	case Part::Gamma:
		return { left, top, kCornerWidth, column };
	case Part::Zoom:
		return { left + side - kCornerWidth, top, kCornerWidth, column };
	case Part::Rotate:
		return { left, baseline - column, kCornerWidth, column };
	case Part::Radius:
	case Part::Theta: {
		// THE POLAR PAIR STACKS UPWARD FROM THE CORNER, so Theta keeps the bottom-right seat Drift had
		// and Radius sits above it. Bottom-anchored rather than centred on the old column, because the
		// corner's job is to stay out of the chart and the chart grows from the middle.
		const float x = left + side - kCornerWidth;
		if (part == Part::Theta) {
			return { x, baseline - polar, kCornerWidth, polar };
		}
		return { x, baseline - polarPair, kCornerWidth, polar };
	}

	case Part::Like:
	case Part::Mutate:
	case Part::Undo:
	case Part::Redo: {
		// The mock's bottom-centre cluster. Mutate is the wide one because it is the verb; undo and
		// redo are its punctuation; the heart (backlog ON) sits on Mutate's left, the other way to find a sound.
		const float x = d.centre().x - kRowWidth * 0.5f;
		const float y = rowBetween ? baseline - kButtonHeight - 2.0f : baseline + kButtonGap;
		if (part == Part::Like) {
			return { x, y, kLikeWidth, kButtonHeight };
		}
		const float mutateX = x + kLikeWidth + kButtonGap;
		if (part == Part::Mutate) {
			return { mutateX, y, kMutateWidth, kButtonHeight };
		}
		if (part == Part::Undo) {
			return { mutateX + kMutateWidth + kButtonGap, y, kStepWidth, kButtonHeight };
		}
		return { mutateX + kMutateWidth + kButtonGap + kStepWidth + kButtonGap, y, kStepWidth,
			     kButtonHeight };
	}
	default:
		return {};
	}
}

visage::Bounds ExploreStage::flankColumn(Flank side) const {
	// The strips beside the chart square — see the header's V16 note. Each is a full-height column
	// as wide as a corner knob, one kFlankGap out from the square's edge; disk() reserved exactly
	// this room before sizing the square, so when the reserve binds the Left strip lands at x = 0.
	// The clamp is for the degenerate frame narrower than the reserve itself, where the square has
	// already yielded to nothing.
	const StageDisk d = disk();
	const float square = chartExtent(d) + 2.0f * StageDisk::kRimInset;
	const float left = std::max(0.0f, d.centre().x - square * 0.5f - kFlankGap - kCornerWidth);
	const float x = side == Flank::Left ? left : width() - left - kCornerWidth;
	return { x, 0.0f, kCornerWidth, height() };
}

visage::Bounds ExploreStage::stackSlotBounds(Flank side, size_t index, size_t count,
                                            size_t insertAfter, float insertHeight) const {
	// THE VERTICAL STACK, centred on the disk: each knob is a full columnHeight(kCornerDiameter) tall
	// and the run's total height is `count` of those plus (count - 1) inter-knob gaps (kGap). The whole
	// thing is placed in a flank strip (flankColumn), so the chart square already reserves this room
	// and never overlaps the knobs — a stack is a placement INTO an existing column, not a new reserve.
	//
	// ONE DEFINITION, ASKED BY BOTH STACKS. The left flank's three and the right flank's five differ
	// only in (side, count), and writing that arithmetic twice is writing the next bug: this file's
	// sibling paid for exactly that shape twice in one week, once in the drawing-vs-hit-test pair and
	// once in a duplicated crossover-to-x conversion.
	const visage::Bounds strip = flankColumn(side);
	const float column = Knob::columnHeight(kCornerDiameter);
	const float gap = Knob::kGap;
	// AND AN OPTIONAL INSERT, which is what stops a non-knob control having to borrow the gap between
	// two knobs. Borrowing is what the Warp dropdown did first, and it does not work: a knob's
	// rectangle is its whole column, the gap belongs to the NEXT knob's hit region, and a box drawn
	// there renders on top of a control that swallows every click aimed at it — the dropdown hidden by
	// another knob's hit box, which is this family's oldest
	// bug and the reason the right flank's case tests geometry and hit-testing as separate claims.
	//
	// SO THE INSERT IS REAL HEIGHT IN THE RUN: everything below it moves down by exactly as much, and
	// the whole stack re-centres, so nothing overlaps and nothing leaves the strip.
	const float insert = (insertAfter < count) ? insertHeight + gap : 0.0f;
	const float run =
	    static_cast<float>(count) * column + static_cast<float>(count - 1) * gap + insert;
	const float shift = (insertAfter < count && index > insertAfter) ? insert : 0.0f;
	const float top =
	    disk().centre().y - run * 0.5f + static_cast<float>(index) * (column + gap) + shift;
	return { strip.x(), top, strip.width(), column };
}

visage::Bounds ExploreStage::flagshipKindBounds(FlagshipKind kind) const {
	// THE DROPDOWN'S ROW IS RESERVED HERE, after Warp — so Smoothness and everything below it move
	// down by the box's height rather than sharing it. See stackSlotBounds for what borrowing cost.
	return stackSlotBounds(Flank::Left, static_cast<size_t>(kind), kFlagshipKindCount,
	                       static_cast<size_t>(FlagshipKind::Warp), kWarpModeBoxHeight);
}

visage::Bounds ExploreStage::fieldKindBounds(FieldKind kind) const {
	// THE MODEL BOX'S ROW IS RESERVED after the last knob, the way the warp box's is on the left.
	return stackSlotBounds(Flank::Right, static_cast<size_t>(kind), kFieldKindCount, kFieldKindCount - 1,
	                       kModelBoxHeight);
}

visage::Bounds ExploreStage::warpModeBounds() const {
	// UNDER THE WARP KNOB ON THE LEFT FLANK — the dropdown belongs near the knob, and near
	// means adjacent to it rather than merely on the same side. Derived FROM the knob's own rectangle,
	// so the box cannot drift from the control it names however the stack is laid out.
	//
	// TOP-ALIGNED IN ITS SLOT AND ONLY A LINE TALL. A knob's slot is a whole column — face, caption,
	// readout — and a dropdown needs one row of it; centring the short box in the tall slot would
	// leave a visible gap between the knob and the control that names what it does.
	// IN THE ROW `flagshipKindBounds` RESERVED FOR IT, which is why this is a derivation and not a
	// clamp any more. The first version squeezed the box into the gap under the knob and clamped it
	// into the strip — geometrically inside, and underneath the next knob's hit region the whole time.
	const visage::Bounds warp = flagshipKindBounds(FlagshipKind::Warp);
	return { warp.x(), warp.bottom() + Knob::kGap, warp.width(), kWarpModeBoxHeight };
}

bool ExploreStage::warpModeLive() const {
	// V5 ONLY. Warp is a flagship operator — it enters through `flagshipChain` — so on Morph or Stair
	// it reaches nothing, and the honesty rule says a control that reaches nothing must look like one.
	// AND ON BASIC SHAPES, which bends its shape ladders' read phase through the same warp (2026-10-03).
	// Asked of the slot table, which the knob above it and the box's drawing and hit test read too, so none of them
	// can come to disagree about whether the box is on this engine's face.
	return slotOf(SurroundControl::WarpMode).has_value();
}

const char* ExploreStage::warpModeLabel() const {
	const auto raw = static_cast<int>(
	    std::lround(bridge_.value(static_cast<clap_id>(ParamId::WarpMode))));
	const int index = std::clamp(raw, 0, static_cast<int>(feathers::WarpMode::kCount) - 1);
	return feathers::kWarpModeNames[static_cast<size_t>(index)];
}

void ExploreStage::chooseWarpMode(size_t index) {
	if (index >= static_cast<size_t>(feathers::WarpMode::kCount)) {
		return;
	}
	// THROUGH THE BRIDGE AS A GESTURE, like every other write in this file: begin, set, end, so a host
	// sees one edit — and the editor's history one step, which until backlog JB this line claimed and did
	// not do (the bracket reached the host's undo only). The other menu picks below share the one helper.
	const auto id = static_cast<clap_id>(ParamId::WarpMode);
	(void)writeParamAsEdit(bridge_, id, static_cast<double>(index), "Warp");
	repaint();
}

void ExploreStage::showWarpModeMenu() {
	visage::PopupMenu menu;
	for (size_t i = 0; i < std::size(feathers::kWarpModeNames); ++i) {
		menu.addOption(static_cast<int>(i), feathers::kWarpModeNames[i]);
	}
	menu.onSelection() = [this](int id) {
		if (id >= 0) {
			chooseWarpMode(static_cast<size_t>(id));
		}
	};
	showMenuAt(std::move(menu), *this, boxBounds(SurroundControl::WarpMode));
}

//----------------//
// The slot table //
//----------------//

visage::Bounds ExploreStage::slotBounds(Slot slot) const {
	switch (slot) {
	case Slot::Left0: return flagshipKindBounds(FlagshipKind::Width);
	case Slot::Left1: return flagshipKindBounds(FlagshipKind::FmDepth);
	case Slot::Left2: return flagshipKindBounds(FlagshipKind::Warp);
	case Slot::LeftBox: return warpModeBounds();
	case Slot::Left3: return flagshipKindBounds(FlagshipKind::Smoothness);
	case Slot::Right0: return fieldKindBounds(FieldKind::Quantize);
	case Slot::Right1: return fieldKindBounds(FieldKind::Sync);
	case Slot::Right2: return fieldKindBounds(FieldKind::Feedback);
	case Slot::Right3: return fieldKindBounds(FieldKind::Points);
	case Slot::Right4: return fieldKindBounds(FieldKind::Grit);
	case Slot::RightBox: return modelBoxBounds();
	case Slot::kCount: break;
	}
	return {};
}

void ExploreStage::rebuildShapesOperatorsRow() {
	pluckPageTwoRow_ = pluckPageTwoOf(slots_[static_cast<size_t>(OscType::Pluck)]);
	pluckPageThreeRow_ = pluckPageThreeOf(slots_[static_cast<size_t>(OscType::Pluck)]);
	drumKitFieldsRow_ = drumKitFieldsOf(slots_[static_cast<size_t>(OscType::Drum)]);
	drumKitPlaceRow_ = drumKitPlaceOf(slots_[static_cast<size_t>(OscType::Drum)]);
	shapesOperatorsRow_ = slots_[static_cast<size_t>(OscType::BasicShapes)];
	for (SurroundControl& c : shapesOperatorsRow_) {
		switch (c) {
		case SurroundControl::Sine: c = SurroundControl::Quantize; break;
		case SurroundControl::Triangle: c = SurroundControl::Sync; break;
		case SurroundControl::Saw: c = SurroundControl::Feedback; break;
		case SurroundControl::Square: c = SurroundControl::Points; break;
		default: break;
		}
	}
}

void ExploreStage::chooseShapesPage(ShapesPage page) {
	shapesPage_ = page;
	syncWidthKnob();
	redraw();
}

void ExploreStage::chooseDrumPage(DrumPage page) {
	drumPage_ = page;
	syncWidthKnob();
	redraw();
}

void ExploreStage::choosePluckPage(PluckPage page) {
	pluckPage_ = page;
	syncWidthKnob();
	redraw();
}

void ExploreStage::setSlotTableForTest(const SlotTable& table) {
	assert(slotTableValid(table));
	slots_ = table;
	rebuildShapesOperatorsRow();
	syncWidthKnob();
	redraw();
}

const ExploreStage::SlotRow& ExploreStage::currentSlotRow() const {
	// The stepped rule (oscTypeFromValue), the ONE shared derivation the tray and the header read too, so the surround
	// and they cannot disagree about which engine the patch is on.
	const auto engine = static_cast<size_t>(oscTypeFromValue(bridge_.value(static_cast<clap_id>(ParamId::OscType))));
	if (engine == static_cast<size_t>(OscType::BasicShapes) && shapesPage_ == ShapesPage::Operators) {
		return shapesOperatorsRow_;
	}
	if (engine == static_cast<size_t>(OscType::Pluck) && pluckPage_ != PluckPage::String) {
		return pluckPage_ == PluckPage::Pluck ? pluckPageTwoRow_ : pluckPageThreeRow_;
	}
	if (engine == static_cast<size_t>(OscType::Drum)) {
		// THE DRUM'S ROW LESS WHAT IS NOT READ (drum proposal §6, the honesty rule; hidden, not greyed): the Walk outside Kit
		// mode, and each scaler the selected sound's kind does not read (drum::kindReads) — the selected PAD's kind in Kit
		// mode, the engine's Kind in Pitched.
		const bool kit = steppedIndex(bridge_.value(static_cast<clap_id>(ParamId::DrumMode)), kDrumModeNames.size())
		                 == static_cast<size_t>(DrumMode::Kit);
		const clap_id kindId = kit ? static_cast<clap_id>(drumParam(selection_.slot(), DrumControl::Kind))
		                           : static_cast<clap_id>(ParamId::DrumPitchedKind);
		const drum::FieldUse reads =
		    drum::kindReads(static_cast<drum::Kind>(steppedIndex(bridge_.value(kindId), kDrumKindNames.size())));
		// THE PAGE (Kit mode only; Pitched is always the Pad page, with no page box): Pad, Kit Fields or Kit Place.
		drumRow_ = !kit || drumPage_ == DrumPage::Pad ? slots_[engine]
		           : drumPage_ == DrumPage::KitFields ? drumKitFieldsRow_
		                                              : drumKitPlaceRow_;
		for (SurroundControl& control : drumRow_) {
			const bool kitOnly = control == SurroundControl::DrumWalkX || control == SurroundControl::DrumWalkY
			                     || control == SurroundControl::DrumPage
			                     || (control >= SurroundControl::DrumKitGamma && control <= SurroundControl::DrumKitMetal);
			const bool shown = kitOnly                                                                   ? kit
			                   : control == SurroundControl::DrumPitch                                       ? reads.pitch
			                   : control == SurroundControl::DrumLength                                      ? reads.length
			                   : control == SurroundControl::DrumSweep                                       ? reads.sweep
			                   : control == SurroundControl::DrumTone                                        ? reads.tone
			                   : control == SurroundControl::DrumNoise                                       ? reads.noise
			                   : control == SurroundControl::DrumSnap                                        ? reads.snap
			                   : control == SurroundControl::DrumMetal                                       ? reads.metal
			                                                                                                 : true;
			if (!shown) {
				control = SurroundControl::None;
			}
		}
		return drumRow_;
	}
	return slots_[std::min(engine, slots_.size() - 1)];
}

std::optional<ExploreStage::Slot> ExploreStage::slotOf(SurroundControl control) const {
	const SlotRow& row = currentSlotRow();
	for (size_t s = 0; s < kSlotCount; ++s) {
		if (row[s] == control) {
			return static_cast<Slot>(s);
		}
	}
	return std::nullopt;
}

ExploreStage::Slot ExploreStage::homeSlotOf(SurroundControl control) const {
	// WHERE A HIDDEN KNOB WAITS: its own engine's slot for it (the first row that seats it). A hidden knob keeps a real
	// rectangle, because a zero-size child is what the create()-positions guard hunts, one setVisible from the origin.
	for (const SlotRow& row : slots_) {
		for (size_t s = 0; s < kSlotCount; ++s) {
			if (row[s] == control) {
				return static_cast<Slot>(s);
			}
		}
	}
	for (size_t s = 0; s < kSlotCount; ++s) {
		if (pluckPageTwoRow_[s] == control || pluckPageThreeRow_[s] == control || drumKitFieldsRow_[s] == control
		    || drumKitPlaceRow_[s] == control) {
			return static_cast<Slot>(s); // Pluck's other pages' own knobs
		}
	}
	return Slot::Left0; // unreachable for a valid table: every control is seated by some row
}

Knob* ExploreStage::surroundKnob(SurroundControl control, size_t sound) const {
	const size_t s = std::min(sound, kMaxSounds - 1);
	const auto c = static_cast<size_t>(control);
	switch (control) {
	case SurroundControl::Width: return widthKnobs_[s].get();
	case SurroundControl::FmDepth: return fmDepthKnobs_[s].get();
	case SurroundControl::Warp: return warpKnobs_[s].get();
	case SurroundControl::Smoothness: return smoothnessKnobs_[s].get();
	case SurroundControl::Quantize:
	case SurroundControl::Sync:
	case SurroundControl::Feedback:
	case SurroundControl::Points:
	case SurroundControl::Grit: return fieldKnobs_[c - static_cast<size_t>(SurroundControl::Quantize)][s].get();
	case SurroundControl::ShapeAmount: return shapeAmountKnobs_[s].get();
	case SurroundControl::Rolloff:
	case SurroundControl::Formant:
	case SurroundControl::OddEven:
	case SurroundControl::Comb:
	case SurroundControl::Detail:
	case SurroundControl::OffGrid: return additiveBiasKnobs_[c - static_cast<size_t>(SurroundControl::Rolloff)][s].get();
	case SurroundControl::KsBrightness:
	case SurroundControl::KsSustain:
	case SurroundControl::KsPick:
	case SurroundControl::KsNoise:
	case SurroundControl::KsStretch: return stringKnobs_[c - static_cast<size_t>(SurroundControl::KsBrightness)][s].get();
	case SurroundControl::DrumPitch:
	case SurroundControl::DrumLength:
	case SurroundControl::DrumSweep:
	case SurroundControl::DrumTone:
	case SurroundControl::DrumNoise:
	case SurroundControl::DrumSnap:
	case SurroundControl::DrumMetal: return drumFieldKnobs_[c - static_cast<size_t>(SurroundControl::DrumPitch)][s].get();
	case SurroundControl::DrumWalkX:
	case SurroundControl::DrumWalkY: return drumWalkKnobs_[c - static_cast<size_t>(SurroundControl::DrumWalkX)].get();
	case SurroundControl::DrumKitGamma:
	case SurroundControl::DrumKitZone:
	case SurroundControl::DrumKitOffset:
	case SurroundControl::DrumKitPitch:
	case SurroundControl::DrumKitLength:
	case SurroundControl::DrumKitSweep:
	case SurroundControl::DrumKitTone:
	case SurroundControl::DrumKitNoise:
	case SurroundControl::DrumKitSnap:
	case SurroundControl::DrumKitMetal: return drumKitKnobs_[c - static_cast<size_t>(SurroundControl::DrumKitGamma)].get();
	case SurroundControl::ResonateWidth:
	case SurroundControl::Bow:
	case SurroundControl::Shimmer:
	case SurroundControl::Stiffness:
	case SurroundControl::Coupling:
	case SurroundControl::Decay:
	case SurroundControl::Pluck:
	case SurroundControl::Travel:
	case SurroundControl::Speed: return resonateKnobs_[c - static_cast<size_t>(SurroundControl::ResonateWidth)].get();
	case SurroundControl::Sine:
	case SurroundControl::Triangle:
	case SurroundControl::Saw:
	case SurroundControl::Square: return shapesKnobs_[c - static_cast<size_t>(SurroundControl::Sine)].get();
	case SurroundControl::KsWidth:
	case SurroundControl::KsDecay:
	case SurroundControl::KsRelease:
	case SurroundControl::KsSeed:
	case SurroundControl::KsDecayTrack:
	case SurroundControl::KsHardness:
	case SurroundControl::KsPickup:
	case SurroundControl::KsVelocity: return pluckKnobs_[c - static_cast<size_t>(SurroundControl::KsWidth)].get();
	case SurroundControl::None:
	case SurroundControl::WarpMode:
	case SurroundControl::BowMode:
	case SurroundControl::AdditiveShape:
	case SurroundControl::Model:
	case SurroundControl::ShapesPage:
	case SurroundControl::KsPage:
	case SurroundControl::DrumPage:
	case SurroundControl::kCount: break;
	}
	return nullptr;
}
// The engine-wide arrays are indexed by the enums their accessors take, so those orders must be the table's.
static_assert(static_cast<size_t>(SurroundControl::Speed) - static_cast<size_t>(SurroundControl::ResonateWidth)
              == static_cast<size_t>(ExploreStage::ResonateKnob::Speed));
static_assert(static_cast<size_t>(SurroundControl::Square) - static_cast<size_t>(SurroundControl::Sine)
              == static_cast<size_t>(ExploreStage::ShapesKnob::Square));
static_assert(static_cast<size_t>(SurroundControl::OffGrid) - static_cast<size_t>(SurroundControl::Rolloff)
              == static_cast<size_t>(ExploreStage::AdditiveBiasKind::OffGrid));
static_assert(static_cast<size_t>(SurroundControl::Grit) - static_cast<size_t>(SurroundControl::Quantize)
              == static_cast<size_t>(ExploreStage::FieldKind::Grit));
static_assert(static_cast<size_t>(SurroundControl::KsStretch) - static_cast<size_t>(SurroundControl::KsBrightness)
              == static_cast<size_t>(ExploreStage::StringKind::Stretch));
static_assert(static_cast<size_t>(SurroundControl::KsVelocity) - static_cast<size_t>(SurroundControl::KsWidth)
              == static_cast<size_t>(ExploreStage::PluckKnob::Velocity));
static_assert(static_cast<size_t>(SurroundControl::DrumMetal) - static_cast<size_t>(SurroundControl::DrumPitch)
              == static_cast<size_t>(ExploreStage::DrumField::Metal));

const char* ExploreStage::boxLabel(SurroundControl control) const {
	switch (control) {
	case SurroundControl::WarpMode: return warpModeLabel();
	case SurroundControl::BowMode: return bowModeLabel();
	case SurroundControl::AdditiveShape: return additiveShapeLabel();
	case SurroundControl::Model: return modelLabel();
	case SurroundControl::ShapesPage: return shapesPage_ == ShapesPage::Mix ? "Page: Mix" : "Page: Operators";
	case SurroundControl::KsPage:
		return pluckPage_ == PluckPage::String ? "Page: String" : pluckPage_ == PluckPage::Pluck ? "Page: Pluck" : "Page: Velocity";
	case SurroundControl::DrumPage:
		return drumPage_ == DrumPage::Pad ? "Page: Pad" : drumPage_ == DrumPage::KitFields ? "Page: Kit Fields" : "Page: Kit Place";
	default: return "";
	}
}

void ExploreStage::showBoxMenu(SurroundControl control) {
	switch (control) {
	case SurroundControl::WarpMode: showWarpModeMenu(); return;
	case SurroundControl::BowMode: showBowModeMenu(); return;
	case SurroundControl::AdditiveShape: showAdditiveShapeMenu(); return;
	case SurroundControl::Model: showModelMenu(); return;
	case SurroundControl::ShapesPage: {
		visage::PopupMenu menu;
		menu.addOption(0, "Mix").select(shapesPage_ == ShapesPage::Mix);
		menu.addOption(1, "Operators").select(shapesPage_ == ShapesPage::Operators);
		menu.onSelection() = [this](int id) {
			if (id >= 0) {
				chooseShapesPage(id == 0 ? ShapesPage::Mix : ShapesPage::Operators);
			}
		};
		showMenuAt(std::move(menu), *this, boxBounds(SurroundControl::ShapesPage));
		return;
	}
	case SurroundControl::KsPage: {
		visage::PopupMenu menu;
		menu.addOption(0, "String").select(pluckPage_ == PluckPage::String);
		menu.addOption(1, "Pluck").select(pluckPage_ == PluckPage::Pluck);
		menu.addOption(2, "Velocity").select(pluckPage_ == PluckPage::Velocity);
		menu.onSelection() = [this](int id) {
			if (id >= 0 && id <= 2) {
				choosePluckPage(static_cast<PluckPage>(id));
			}
		};
		showMenuAt(std::move(menu), *this, boxBounds(SurroundControl::KsPage));
		return;
	}
	case SurroundControl::DrumPage: {
		visage::PopupMenu menu;
		menu.addOption(0, "Pad").select(drumPage_ == DrumPage::Pad);
		menu.addOption(1, "Kit Fields").select(drumPage_ == DrumPage::KitFields);
		menu.addOption(2, "Kit Place").select(drumPage_ == DrumPage::KitPlace);
		menu.onSelection() = [this](int id) {
			if (id >= 0 && id <= 2) {
				chooseDrumPage(static_cast<DrumPage>(id));
			}
		};
		showMenuAt(std::move(menu), *this, boxBounds(SurroundControl::DrumPage));
		return;
	}
	default: return;
	}
}

visage::Bounds ExploreStage::boxBounds(SurroundControl control) const {
	return slotBounds(slotOf(control).value_or(homeSlotOf(control)));
}

visage::Bounds ExploreStage::resonateKnobBounds(ResonateKnob which) const {
	const auto control =
	    static_cast<SurroundControl>(static_cast<size_t>(SurroundControl::ResonateWidth) + static_cast<size_t>(which));
	return slotBounds(slotOf(control).value_or(homeSlotOf(control)));
}

visage::Bounds ExploreStage::shapesKnobBounds(ShapesKnob which) const {
	const auto control =
	    static_cast<SurroundControl>(static_cast<size_t>(SurroundControl::Sine) + static_cast<size_t>(which));
	return slotBounds(slotOf(control).value_or(homeSlotOf(control)));
}

void ExploreStage::setDrumOpen(bool open) {
	if (open == drumOpen_) {
		return;
	}
	drumOpen_ = open;
	syncWidthKnob();
	redraw();
}

void ExploreStage::setSpectrumOpen(bool open) {
	if (open == spectrumOpen_) {
		return;
	}
	spectrumOpen_ = open;
	syncWidthKnob(); // the tab's caret
	redraw();
}

visage::Bounds ExploreStage::spectrumTabBounds() const {
	// UNDER THE RIGHT FLANK, ON THE BOTTOM EDGE — the bottom centre is Mutate/Undo/Redo's row, and the right flank's
	// stack leaves its foot free on Additive (five knobs and the reserved model-box row).
	const visage::Bounds column = flankColumn(Flank::Right);
	const float x = column.x() + (column.width() - SpectrumTab::kWidth) * 0.5f;
	return { x, height() - SpectrumTab::kHeight, SpectrumTab::kWidth, SpectrumTab::kHeight };
}

visage::Bounds ExploreStage::additiveBiasBounds(AdditiveBiasKind kind) const {
	// Rolloff on the left, in FM Depth's slot (design §4.1: "one bias knob in FM Depth's slot"), the rest on the right:
	// the slot table's Additive row says so.
	const auto control =
	    static_cast<SurroundControl>(static_cast<size_t>(SurroundControl::Rolloff) + static_cast<size_t>(kind));
	return slotBounds(slotOf(control).value_or(homeSlotOf(control)));
}

bool ExploreStage::bowModeLive() const {
	return slotOf(SurroundControl::BowMode).has_value();
}

const char* ExploreStage::bowModeLabel() const {
	return kResonateBowModeNames[steppedIndex(bridge_.value(static_cast<clap_id>(ParamId::ResonateBowMode)),
	                                          kResonateBowModeNames.size())];
}

void ExploreStage::chooseBowMode(size_t index) {
	if (index >= kResonateBowModeNames.size()) {
		return;
	}
	const auto id = static_cast<clap_id>(ParamId::ResonateBowMode);
	(void)writeParamAsEdit(bridge_, id, static_cast<double>(index), "Bow");
	repaint();
}

void ExploreStage::showBowModeMenu() {
	visage::PopupMenu menu;
	const std::string current = bowModeLabel();
	for (size_t i = 0; i < kResonateBowModeNames.size(); ++i) {
		menu.addOption(static_cast<int>(i), kResonateBowModeNames[i]).select(current == kResonateBowModeNames[i]);
	}
	menu.onSelection() = [this](int id) {
		if (id >= 0) {
			chooseBowMode(static_cast<size_t>(id));
		}
	};
	showMenuAt(std::move(menu), *this, boxBounds(SurroundControl::BowMode));
}

bool ExploreStage::additiveShapeLive() const {
	// ADDITIVE ONLY: the bank reads `Character::additiveShape` on a Partials build alone (character_params.h fills it on
	// Additive alone), so on every other engine the choice reaches nothing. The slot table's Additive row seats it.
	return slotOf(SurroundControl::AdditiveShape).has_value();
}

const char* ExploreStage::additiveShapeLabel() const {
	return kAdditiveShapeNames[steppedIndex(bridge_.value(static_cast<clap_id>(ParamId::AdditiveShape)),
	                                        kAdditiveShapeNames.size())];
}

void ExploreStage::chooseAdditiveShape(size_t index) {
	if (index >= kAdditiveShapeNames.size()) {
		return;
	}
	const auto id = static_cast<clap_id>(ParamId::AdditiveShape);
	(void)writeParamAsEdit(bridge_, id, static_cast<double>(index), "Shape");
	repaint();
}

void ExploreStage::showAdditiveShapeMenu() {
	// EVERY SHAPE ON THE MENU BEFORE ONE IS APPLIED (the multivalue-toggle rule), the current one ticked.
	visage::PopupMenu menu;
	const std::string current = additiveShapeLabel();
	for (size_t i = 0; i < kAdditiveShapeNames.size(); ++i) {
		menu.addOption(static_cast<int>(i), kAdditiveShapeNames[i]).select(current == kAdditiveShapeNames[i]);
	}
	menu.onSelection() = [this](int id) {
		if (id >= 0) {
			chooseAdditiveShape(static_cast<size_t>(id));
		}
	};
	showMenuAt(std::move(menu), *this, boxBounds(SurroundControl::AdditiveShape));
}

visage::Bounds ExploreStage::modelBoxBounds() const {
	const visage::Bounds last = fieldKindBounds(FieldKind::Grit);
	return { last.x(), last.bottom() + Knob::kGap, last.width(), kModelBoxHeight };
}

bool ExploreStage::modelBoxLive() const {
	// RESONATE ONLY: the model is what the synth's one string is, and only Resonate's voices scan it. The slot table's
	// Resonate row seats it.
	return slotOf(SurroundControl::Model).has_value();
}

const char* ExploreStage::modelLabel() const {
	return kResonateModelNames[steppedIndex(bridge_.value(static_cast<clap_id>(ParamId::ResonateModel)),
	                                        kResonateModelNames.size())];
}

void ExploreStage::chooseModel(size_t index) {
	if (index >= kResonateModelNames.size()) {
		return;
	}
	const auto id = static_cast<clap_id>(ParamId::ResonateModel);
	(void)writeParamAsEdit(bridge_, id, static_cast<double>(index), "Model");
	repaint();
}

void ExploreStage::showModelMenu() {
	visage::PopupMenu menu;
	const std::string current = modelLabel();
	for (size_t i = 0; i < kResonateModelNames.size(); ++i) {
		menu.addOption(static_cast<int>(i), kResonateModelNames[i]).select(current == kResonateModelNames[i]);
	}
	menu.onSelection() = [this](int id) {
		if (id >= 0) {
			chooseModel(static_cast<size_t>(id));
		}
	};
	showMenuAt(std::move(menu), *this, boxBounds(SurroundControl::Model));
}
visage::Bounds ExploreStage::widthKnobBounds() const {
	// The width knob is the top of the flagship stack — kept as its own accessor for the hue chip and
	// the tests that pin the original V16 geometry.
	return flagshipKindBounds(FlagshipKind::Width);
}

void ExploreStage::setModulation(ModState* state, EditHistory* history) {
	// ALL THREE ARRAYS AND ALL EIGHT SLOTS, not just the visible trio: the hidden seven are the same
	// objects, kept alive and sized so a selection change is a visibility flip rather than a rebuild
	// (see syncWidthKnob), and a knob handed a ModState only when it happens to be showing would lose
	// it the moment the rail moved.
	//
	// A knob whose parameter is not a modulation DESTINATION takes this and stays exactly as it was —
	// Knob::mappable asks modDestinationIndexOf first — so this does not have to know which of the
	// three the plugin has declared, and it does not become wrong when that answer changes.
	for (size_t slot = 0; slot < kMaxSounds; ++slot) {
		// THE POLAR PAIR, and this line is the whole of the defect that showed as no mod glow on
		// these knobs: the paragraph below says a knob with no ModState cannot be pointed at, and
		// the corners these replace were not even knobs.
		radiusKnobs_[slot]->setModulation(state, history);
		thetaKnobs_[slot]->setModulation(state, history);
		widthKnobs_[slot]->setModulation(state, history);
		fmDepthKnobs_[slot]->setModulation(state, history);
		warpKnobs_[slot]->setModulation(state, history);
		shapeAmountKnobs_[slot]->setModulation(state, history);
		smoothnessKnobs_[slot]->setModulation(state, history);
		// AND THE FIVE, which are the reason for this work: the field modifier knobs belong on the
		// Explore view as mod targets. A knob with no ModState has a null mod_, modRing() returns
		// before it reads anything, and no drag can point a source at it — so this loop is what makes
		// them destinations in practice rather than only in kModDestinations.
		for (auto& byKind : fieldKnobs_) {
			byKind[slot]->setModulation(state, history);
		}
		// And the six additive bias knobs.
		for (auto& byKind : additiveBiasKnobs_) {
			byKind[slot]->setModulation(state, history);
		}
		// And Pluck's five field scalers.
		for (auto& byKind : stringKnobs_) {
			byKind[slot]->setModulation(state, history);
		}
		// And the drum's seven.
		for (auto& byKind : drumFieldKnobs_) {
			byKind[slot]->setModulation(state, history);
		}
	}
	// AND RESONATE'S SURROUND, engine-wide, one each: modulation destinations like every knob here.
	for (auto& knob : resonateKnobs_) {
		knob->setModulation(state, history);
	}
	// AND BASIC SHAPES' MIX, the same.
	for (auto& knob : shapesKnobs_) {
		knob->setModulation(state, history);
	}
	// AND PLUCK'S ENGINE-WIDE KNOBS.
	for (auto& knob : pluckKnobs_) {
		knob->setModulation(state, history);
	}
	// AND THE DRUM'S KIT WALK.
	for (auto& knob : drumKitKnobs_) {
		knob->setModulation(state, history);
	}
	for (auto& knob : drumWalkKnobs_) {
		knob->setModulation(state, history);
	}
	// AND THE MASTER SWITCH'S FLIPS, steps of the editor's undo (stored state, which the host's undo never sees).
	spectrumTab_->setHistory(history);
}

void ExploreStage::syncWidthKnob() {
	// EVERY KNOB IS SIZED, shown or not: a zero-size child frame is exactly the shape the
	// create()-positions guard hunts — one setVisible away from painting at the window origin — so the
	// hidden seven keep the same real rectangle and visibility alone decides what paints.
	const size_t selected = selection_.slot();
	// The stepped rule, not a homespun threshold — the ONE shared derivation (oscTypeFromValue),
	// which the tray's master-scale knob reads too, so the two width controls and the header cannot
	// disagree about whether a patch is flagship (V12c: on a non-V5 patch the knob is a control that
	// changes nothing, and it must look like one).
	const OscType osc = oscTypeFromValue(bridge_.value(static_cast<clap_id>(ParamId::OscType)));
	// Additive's spectrum tab and drawer are not in the surround's slots; they ask the engine themselves (below).
	const bool additive = osc == OscType::Additive;
	// And a PARKED selected sound is the same honesty case: it is not in the character, so its
	// flagship controls buy nothing until the switch comes back.
	// IN A DRUM KIT EVERY SOUND IS A PAD whatever its switch (drum proposal §3.3), so a pad's knobs never grey for it.
	const bool drumKit = osc == OscType::Drum
	                     && steppedIndex(bridge_.value(static_cast<clap_id>(ParamId::DrumMode)), kDrumModeNames.size())
	                            == static_cast<size_t>(DrumMode::Kit);
	const bool inConstellation =
	    drumKit || steppedIndex(bridge_.value(paramOf(SoundField::Enable)), kEnableNames.size()) != 0;
	// THE POLAR PAIR SITS IN THE BOTTOM-RIGHT CORNER and is live on every engine: a sound's place in
	// its plane is decoded for all of them, so no gate greys these (the honesty rule, read the other
	// way — a control that changes sound stays enabled).
	const auto syncPolar = [&](auto& array, Part part) {
		const visage::Bounds bounds = partBounds(part);
		for (size_t slot = 0; slot < kMaxSounds; ++slot) {
			Knob& knob = *array[slot];
			knob.setBounds(bounds);
			const bool shown = slot == selected;
			if (knob.isVisible() != shown) {
				knob.setVisible(shown);
			}
		}
	};
	syncPolar(radiusKnobs_, Part::Radius);
	syncPolar(thetaKnobs_, Part::Theta);

	// THE FLANKS, FROM THE SLOT TABLE (backlog LJ). Which engine reads which control is the table's (kSurroundSlots,
	// where each row says why); this loop only carries it out, the same way for every knob, so no engine has a code
	// path of its own and a knob placed in another engine's gap needs no line here.
	//
	// NOT DRAWN WHERE ONLY THE ENGINE COULD WAKE THEM: for Explore's engine settings, not drawn is preferred
	// over greyed, since there is no way to ungrey them without switching engine. So the two gates are told apart: the ENGINE (the table) decides whether the knob is on the face at
	// all, and the sound's own switch — which is on this face — decides whether a shown per-sound knob is greyed.
	// Enablement still carries both, so a hidden knob is a dead one too and not merely an undrawn one. The engine-wide
	// knobs (Resonate's surround, the shapes mix) are live whenever shown: a parked selected sound does not grey them.
	//
	// SMOOTHNESS IS LIVE WHEREVER IT IS SEATED, and was not. It used to MULTIPLY the field, and the field is
	// alive over exactly one interval of the first eight offset steps of home (4.6501 to 6.2002,
	// measured) and is exactly zero at the default coordinate — so it was multiplying nothing wherever
	// a patch starts. The top half of the travel now BIASES the field up instead, so the
	// control can ask for smoothness rather than only permit it (see fieldWithOverride). FM DEPTH IS LIVE AS OF
	// 2026-08-22: the knob shipped ahead of its engine and was greyed until T2/T3/T4/T6 of the FM doc landed.
	//
	// EVERY KNOB OF EVERY SOUND IS SIZED: the selected sound's at the slot the table seats it in, the hidden ones
	// at their home slot (homeSlotOf).
	for (size_t c = 1; c < kSurroundControlCount; ++c) {
		const auto control = static_cast<SurroundControl>(c);
		if (isBox(control)) {
			continue; // drawn and hit-tested by the stage itself (drawSlotBox, onMouseDown)
		}
		const std::optional<Slot> seat = slotOf(control);
		const visage::Bounds bounds = slotBounds(seat.value_or(homeSlotOf(control)));
		const bool onFace = seat.has_value();
		if (!isPerSound(control)) {
			Knob& knob = *surroundKnob(control, 0);
			knob.setBounds(bounds);
			if (knob.isVisible() != onFace) {
				knob.setVisible(onFace);
			}
			knob.setEnabled(onFace);
			continue;
		}
		for (size_t slot = 0; slot < kMaxSounds; ++slot) {
			Knob& knob = *surroundKnob(control, slot);
			knob.setBounds(bounds);
			const bool shown = slot == selected && onFace;
			if (knob.isVisible() != shown) {
				knob.setVisible(shown);
			}
			if (slot == selected) {
				knob.setEnabled(onFace && inConstellation);
			}
		}
	}

	// AND THE SPECTRUM TAB (additive design §4.2): on the face on Additive alone (design R9; the 2026-10-04 rule — only an
	// engine switch could wake it elsewhere, so it is not drawn, not greyed). The panel it opens is the editor's.
	if (spectrumTab_ == nullptr) {
		return; // a layout asked for before the constructor reached it
	}
	spectrumTab_->setBounds(spectrumTabBounds());
	if (spectrumTab_->isVisible() != additive) {
		spectrumTab_->setVisible(additive);
	}
	spectrumTab_->setOpen(additive && spectrumOpen_);
	// AND THE DRUM TAB in the same place, on Drum alone (only an engine switch could wake it elsewhere, so not drawn).
	if (drumTab_ != nullptr) {
		const bool drum = osc == OscType::Drum;
		const visage::Bounds tab = spectrumTabBounds();
		drumTab_->setBounds(tab.x(), tab.y(), tab.width(), DrumTab::kHeight);
		if (drumTab_->isVisible() != drum) {
			drumTab_->setVisible(drum);
		}
		drumTab_->setOpen(drum && drumOpen_);
	}
	spectrumTab_->sync(additive);
 }

 void ExploreStage::resized() {
	syncWidthKnob();
}

ExploreStage::Part ExploreStage::hitAt(visage::Point position) const {
	// THE CORNERS AND THE BUTTONS FIRST. They overlap the disk's rim, and a press meant for a control
	// that landed on the chart instead would scrub somebody's sound to wherever the button is.
	for (const Part part : {
		     Part::Like, Part::Mutate, Part::Undo, Part::Redo, Part::Gamma, Part::Zoom, Part::Rotate, Part::Radius,
		     Part::Theta }) {
		if (partBounds(part).contains(position)) {
			return part;
		}
	}
	// THE CIRCLE IS THE CONTROL, not the rectangle it is centred in — the same rule Perform follows,
	// and for the same reason: the stage frame is far wider than the disk.
	return hitsDisk(position) ? Part::Chart : Part::None;
}

//---------//
// Motion  //
//---------//

double ExploreStage::travelCentre() const {
	// The horizontal twin of viewCentre(), and it needs the same before-the-first-frame answer: a press
	// that arrives before anything has drawn would otherwise be measured against a centre of zero.
	return everCentred_ ? travelCentre_ : viewDrift();
}

double ExploreStage::followCentreFor(double starHome, double span, bool throughTop, double margin) {
	// The star is to sit `margin` of a view inside the edge it left. Leaving through the TOP means its
	// home is above the view's, so the view has to rise — home increases upward, and that is the whole
	// of the asymmetry the flag carries. At margin 0.5 the inset is exactly zero and this returns the
	// star's own home, which is what makes centring a VALUE of this function rather than a second path.
	const double inset = span * (0.5 - margin);
	return throughTop ? starHome - inset : starHome + inset;
}

double ExploreStage::keepInsideView(double star, double centre, double span, double margin) {
	// The half-width the point is allowed to reach before the view starts moving with it. At margin 0.15
	// that is 0.35 of a span either side of centre; beyond it the view is placed so the point sits
	// exactly on that limit, which is a pan of only what the overshoot was.
	const double reach = span * (0.5 - margin);
	if (star > centre + reach) {
		return star - reach;
	}
	if (star < centre - reach) {
		return star + reach;
	}
	return centre;
}

void ExploreStage::dragStarTo(double wantedHome, double wantedTravel) {
	// **THE STAR GOES WHERE THE POINTER IS — UP TO A SPEED** (the drag and pan rate are throttled so that
	// unrendered terrain does not come into the viewport). An ordinary drag is under
	// the cap and lands exactly on the pointer, which is what the chart means by pressing somewhere. A FLING is not: the
	// world would arrive faster than the terrain can be built for it, so the star follows at `kViewsPerSecond` and
	// catches up while the hand slows. The view follows the star as it always did, so nothing is ever lost off the glass.
	const int64_t now = clock_ ? clock_() : 0;
	const double elapsed = std::min(0.25, std::max(0.0, static_cast<double>(now - starSteppedAt_) / 1000.0));
	starSteppedAt_ = now;
	const double homeSpan = homeSpanZones() / 1023.0;
	// A CAP IN VIEWS RATHER THAN IN ZONES, because what the builder owes is a fraction of a map: the same demand at every
	// zoom, where a fixed number of zones would be nothing zoomed out and impossible zoomed in.
	// **THE CAP IS ON THE VECTOR, NOT ON ITS COMPONENTS** (a per-axis cap still let a little void
	// in, especially when dragging diagonally).
	//
	// IT USED TO BE PER AXIS: the same `kViewsPerSecond * span * elapsed` step was applied to home and
	// to travel independently, so a drag along one axis moved at the cap and a drag along the diagonal
	// moved at `sqrt(2)` times it. MEASURED over an identical 60-frame fling: 1.152 views of vector on
	// each axis and **1.629 on the diagonal** — 41% faster than the number the speculative margin is
	// sized against, on the one gesture a hand makes most naturally. Void is drag rate x build time, so
	// a diagonal fling ate 41% more ground per build than `kTerrainMargin` was bought to cover.
	//
	// SCALING BOTH COMPONENTS BY ONE FACTOR IS WHAT KEEPS THE DIRECTION. Clamping each axis separately
	// does not merely allow too much, it BENDS a fast diagonal toward whichever axis saturates first —
	// the star would arrive somewhere the pointer never went. One factor, applied to both, moves the
	// star straight at the pointer at a bounded speed.
	const double travelSpanNow = travelSpan();
	// **BOTH GAPS ARE TRAVEL** (backlog GY). The first was the gap in HOME and was closed by `scrubTo`,
	// which writes the unbounded gamma base — that is what made a vertical drag a gamma gesture. It is
	// the gap in travel-Y now, closed by the same write as the horizontal, so a drag moves the sound
	// within the plane and gamma is left to its own corner.
	// AND BOTH ARE MEASURED IN THE VIEW (backlog ON): the cap is a fraction of what is on screen, and under the likes'
	// warp the screen is the warped world. `wanted*` stay REAL places, so an arrival lands on exactly the one asked for.
	const WarpPoint wantedView = viewOf(wantedTravel, wantedHome);
	const WarpPoint starView = viewOf(selectedDrift(), selectedDriftY());
	const double gapHome = homeSpan > 0.0 ? (wantedView.y - starView.y) / homeSpan : 0.0;
	const double gapTravel = travelSpanNow > 0.0 ? (wantedView.x - starView.x) / travelSpanNow : 0.0;
	const double reach = std::hypot(gapHome, gapTravel);
	// A CAP IN VIEWS RATHER THAN IN ZONES, because what the builder owes is a fraction of a map: the same demand at every
	// zoom, where a fixed number of zones would be nothing zoomed out and impossible zoomed in.
	const double step = kViewsPerSecond * elapsed;
	// NO TIME, NO TRAVEL. This read `step <= 0.0 || ... ) return wanted;`, which is a throttle that lets everything
	// through exactly when it is asked twice inside one millisecond — and it showed: scrolling felt
	// unthrottled.
	if (step <= 0.0 || reach <= 0.0) {
		return;
	}
	// AN ORDINARY DRAG IS UNDER THE CAP AND LANDS EXACTLY ON THE POINTER, which is what pressing on the
	// chart MEANS; only a fling is scaled back, and it keeps walking on the frames that follow.
	const double scale = reach <= step ? 1.0 : step / reach;
	// UNDER A WARP AN ARRIVAL IS THE PLACE ASKED FOR, exactly: a round trip through the warp is close, not equal, and the
	// release's settle waits for equality. Without one the arithmetic below is the identity and stays as it always was.
	if (scale == 1.0 && likesDrawn() != nullptr) {
		driftToXY(wantedTravel, wantedHome);
		return;
	}
	const WarpPoint next = worldOf(starView.x + gapTravel * scale * travelSpanNow, starView.y + gapHome * scale * homeSpan);
	driftToXY(next.x, next.y);
}

void ExploreStage::settleOn(double candidateCentre, double candidateTravel) {
	// **AN INSTANT CENTRING ALWAYS WINS OVER A GLIDE IN FLIGHT.** A selection change, a first frame, a
	// history walk and a mid-drag nudge all arrive here, and every one of them is a statement about
	// where the view IS rather than where it is heading. Leaving a glide running would let a stale
	// target drag the view back off whatever just claimed it, one frame later.
	glidingView_ = false;
	placeView(candidateCentre, candidateTravel);
}

/// WHERE THE VIEW ACTUALLY GOES, shared by the teleport and by each step of a glide so the two cannot
/// disagree about what "centred" means. Everything `settleOn` used to do; `settleOn` is now that plus
/// the cancellation above.
void ExploreStage::placeView(double candidateCentre, double candidateTravel) {
	// TRULY CENTRED, ON BOTH AXES — the 2026-08-17 decision, and it retires the band clamp that used
	// to live here. The clamp kept the chart full of places that exist by refusing to centre near the
	// band's ends, and the price was that a fresh patch's sound A — zone 0, offset 0, home == gammaSum ==
	// the floor — was drawn 8px off the BOTTOM RIM of a 534-tall chart while the centre sat at y 267. So
	// "centre on the active point" visibly did nothing in the state a new patch hands you. Where nothing
	// exists, the decision is to show the void. Near the ends the
	// chart is honestly half dark, and the dark half is information — it is the edge of the world, and
	// you can see that you are standing at it.
	viewCentre_ = candidateCentre;
	travelCentre_ = candidateTravel;
	everCentred_ = true;
	centredOnSlot_ = selection_.slot();
	// The gamma this centre was taken at, so that viewCentre() can follow gamma from here. Recorded in
	// the same breath as the centre itself: the two are one measurement, and a centre without its gamma
	// would make the view chase a difference against a baseline nobody set.
	askForTerrain();
}

void ExploreStage::recentre() {
	// **ON THE SOUND'S PLACE IN THE PLANE, NOT ON ITS HOME** (backlog GY). The vertical used to centre
	// on `selectedHome()`, which is what made the view slide when gamma moved. Both axes are travel
	// now, so both halves of the centre come from the same place the star is drawn at.
	settleOn(viewDriftY(), viewDrift());
}

void ExploreStage::glideToSelected() {
	// **THE VIEW PANS TO ITS NEW CENTRE RATHER THAN TELEPORTING**, after a click and after a jog alike.
	//
	// BOTH GESTURES END HERE AND BOTH USED TO JUMP, which is why they are fixed together. A CLICK put
	// the star on the pressed point and `recentre()` assigned the view outright — the whole distance in
	// one frame. A JOG only NUDGED the view while the hand moved — `keepInsideView` lets the star
	// wander `kScrubEdgeMargin` (0.15) of a span from centre before the view moves at all — so the
	// release had up to that much still owed and paid it in one frame too. Same teleport, two
	// distances, which is why a click reads as the worse of the two.
	//
	// READ FROM THE CONSTANT, NOT REMEMBERED: a first draft of this comment said 0.35, which is a
	// number from an older draft of `keepInsideView` and would have quietly mis-taught the next reader
	// about how far a jog can owe.
	//
	// THE DESTINATION IS UNCHANGED — it is still `recentre()`'s, the selected sound. Only the journey
	// is added, and it is the journey that makes the move legible: you can see WHICH WAY the world
	// went, which is the whole argument for bounding the drag rate in the first place.
	glideWantedCentre_ = viewDriftY();
	glideWantedTravel_ = viewDrift();
	glidingView_ = true;
	// THE CLOCK STARTS NOW, not at whatever the last glide left behind. `stepViewGlide` measures
	// elapsed against this, and a stamp minutes old would spend the entire journey in one frame — the
	// exact jump this exists to remove.
	glideSteppedAt_ = clock_ ? clock_() : 0;
	askForTerrain();
}

void ExploreStage::stepViewGlide() {
	if (!glidingView_) {
		return;
	}
	const int64_t now = clock_ ? clock_() : 0;
	const double elapsed =
	    std::min(0.25, std::max(0.0, static_cast<double>(now - glideSteppedAt_) / 1000.0));
	glideSteppedAt_ = now;

	// **THE SAME CAP THE STAR MOVES UNDER, AND DELIBERATELY NOT A SECOND SPEED.** `kViewsPerSecond` is
	// sized against `kTerrainMargin` — void is drag rate times build time — so a view that panned
	// faster than the star would walk the glass onto ground the builder has not been given time for,
	// which is the defect the cap was bought to prevent. And the cap is on the VECTOR, for the reason
	// the diagonal fix records: clamping each axis separately bends the journey toward whichever axis
	// saturates first, so the view would arrive by a route it was never sent on.
	const double homeSpan = homeSpanZones() / 1023.0;
	const double travelSpanNow = travelSpan();
	const double centreNow = viewCentre();
	const double travelNow = travelCentre();
	const double gapHome = homeSpan > 0.0 ? (glideWantedCentre_ - centreNow) / homeSpan : 0.0;
	const double gapTravel =
	    travelSpanNow > 0.0 ? (glideWantedTravel_ - travelNow) / travelSpanNow : 0.0;
	const double reach = std::hypot(gapHome, gapTravel);
	const double step = kViewsPerSecond * elapsed;
	if (reach <= 0.0 || step <= 0.0 || reach <= step) {
		// ARRIVED — landed EXACTLY on the target rather than within a step of it, and the glide closed
		// through `settleOn` so the arrival is an ordinary centring like any other.
		glidingView_ = false;
		settleOn(glideWantedCentre_, glideWantedTravel_);
		return;
	}
	const double scale = step / reach;
	// `placeView` rather than `settleOn`, because `settleOn` cancels a glide and this is the glide.
	placeView(centreNow + gapHome * scale * homeSpan, travelNow + gapTravel * scale * travelSpanNow);
}

void ExploreStage::followSelected(bool throughTop) {
	// AUTOMATION HAS NO GESTURE TO END, so this is the one road on which the view moves while nobody is
	// touching anything — and it fires repeatedly through a single automated sweep (ui-10 counted seven
	// in one test gesture), which is what makes the size of each move worth a constant of its own.
	// Vertical only: this fires on the star leaving through the TOP or BOTTOM, and the horizontal
	// coordinate has not moved, so re-centring it would be a pan nobody asked for.
	settleOn(followCentreFor(viewDriftY(), homeSpanZones() / 1023.0, throughTop, kFollowMargin),
	         travelCentre());
}

// EVERY VALUE IN THIS WINDOW IS A **BASE**, NEVER A MODULATED READING — and that is the opposite of
// what the scope two inches away does, on purpose, for a reason neither file could see alone.
//
// WaveDisplay::characterNow reads `bridge_.modulated()` and says why: "the picture is of the sound
// being played, so it follows host modulation exactly as the engine does." A waveform that ignored
// modulation would be a picture of a sound nobody is hearing.
//
// THE TERRAIN IS NOT THAT KIND OF PICTURE. It is a MAP OF A PLACE — what the instrument would sound
// like at every home and travel in view — and modulation moves where you are STANDING, not what the
// country looks like. The puck moves; the map does not. So the window is assembled from the bases,
// and the star's position, which IS the reading, is drawn from the modulated values a few functions
// up (see the anchors and the puck in `flagshipCharacter`).
//
// AND THE COST OF GETTING IT BACKWARDS IS NOT A WRONG PICTURE, IT IS A REBUILD PER BLOCK. The window
// is the content key; the key gates `TerrainMap::request`. Read `modulated()` here and every
// modulated block asks for a new 128x128 map, which is the expensive half of exactly what the design rules
// out: field controls are modulatable, but modulation does not redraw the terrain. It
// would arrive as a performance complaint rather than as a visibly wrong chart, which is the harder
// kind to trace back to a one-word change.
//
// This held by accident until a case was written for it — nothing but this comment and
// "the terrain window is assembled from bases, not from modulated readings" in test_gui_widgets.cpp
// stops the next hand from making the two widgets "consistent". That case pins BOTH modulatable
// window fields as of 2026-08-24; it pinned only the smoothness scale before, and the FM depth beside
// it could be switched to `modulated()` with every suite staying green.
double ExploreStage::terrainMargin() {
	// ONE READ FOR THE PROCESS, so a run is one arm of an A/B and cannot change its mind halfway.
	static const double margin = [] {
		const char* set = std::getenv("FEATHERS_TERRAIN_MARGIN");
		if (set == nullptr) {
			return kTerrainMargin;
		}
		const double asked = std::atof(set);
		// A MARGIN BELOW ONE WOULD BUILD LESS THAN THE VIEW, which is not a smaller margin but a
		// guaranteed hole, so the floor is 1.0 — the honest "no margin at all" arm.
		return asked >= 1.0 ? asked : kTerrainMargin;
	}();
	return margin;
}

TerrainWindow ExploreStage::terrainWindowNow() const {
	return terrainWindowFor(selection_.slot());
}

TerrainWindow ExploreStage::terrainWindowFor(size_t slot) const {
	const bool selected = slot == selection_.slot();
	TerrainWindow window;
	window.engine = oscTypeFromValue(bridge_.value(static_cast<clap_id>(ParamId::OscType)));
	// THE SELECTED SOUND'S OWN PLANE, resolved the way the engine resolves it — its pin if it has one,
	// otherwise the global audition pin, otherwise derived per texel from that texel's home.
	//
	// This was the global pin alone, and it made ROTATE INVISIBLE. The map could not respond to a turn
	// (it never read the per-sound plane) and could not even be asked to (the window key did not carry
	// it, so nothing looked stale). Compounded by the guarantee that rotate is AUDIBLY inert at home
	// with no drift, the verb had no feedback anywhere except its own dial — rotate seemed not to work
	// at all, since it did not redraw the background.
	//
	// The map stays a pure function of position: an unpinned sound still derives its plane from each
	// texel's own home, and a pinned one carries its plane wherever it goes, which is what the pin
	// MEANS. The window already carries the sound's gamma and offset for the same reason.
	const SliceOrientationPin own = bridge_.soundOrientations().pin[slot];
	window.pin = own.pinned ? own : bridge_.orientation();
	// AND WHAT THIS SOUND'S ROTATIONS BANKED, for the same reason and with the same consequence. The
	// plane alone is the pre-A26 answer: a sound rotated at a drift stands on its residue, so a map
	// built without it draws a NEIGHBOUR of the sound being heard, under a star that says it is the
	// sound. §4a's rule is that the map is truth, and this is the half of the truth it was missing.
	//
	// It is the SELECTED sound's, and that does not break "a pure function of position" any more than
	// the gamma and the offset baseline do: a residue is what this sound carries with it everywhere,
	// and the chart has always been drawn in the selected sound's own coordinates. What stays derived
	// per texel is the plane of an unpinned sound.
	window.residue = bridge_.soundResidue().banked[slot];
	// **THE HOME THE WHOLE PLANE IS TAKEN AT** (backlog GY). This was `window.viewGamma`, a frame two
	// maps were lined up in so a stale picture could be re-registered against a view that FOLLOWED
	// gamma one for one. Nothing follows gamma now — the chart's two axes are both displacements in
	// the decoded plane — so there is no frame to line up and nothing to compensate.
	//
	// WHAT TAKES ITS PLACE IS A REAL INPUT, not a correction: the base completes the home every texel
	// of this map is built at, so moving it makes every texel a different place. That is why it is in
	// the content key where `viewGamma` was deliberately kept out of it.
	window.gammaBase = selected ? heldGammaBase_.value_or(selectedGammaBase()) : bridge_.soundGamma().base[std::min(slot, kMaxSounds - 1)];
	// THE COARSE HALF USED TO TRAVEL BESIDE IT, so the builder could keep it out of the float it built
	// at. It no longer needs telling: a row's base is derived from that row's own home minus the two
	// dials, so the split is the builder's to make and it makes the best one available every time. The
	// field went with the need for it — a written-but-never-read field is a lie waiting for a reader.
	//
	// THE TWO BOUNDED DIALS, which is what every row of the map is built at now. A scrub moves the base
	// and leaves these alone, so a chart drawn at them is a chart of the places this sound can actually
	// be taken to. Under PhiStair the zone is also the pattern family, and it is the only thing that can
	// choose one — which is why the map is one family and the dial is how you change it.
	window.zoneBaseline = bridge_.value(static_cast<clap_id>(soundParam(slot, SoundField::Zone)));
	window.offsetBaseline = bridge_.value(static_cast<clap_id>(soundParam(slot, SoundField::Offset)));
	// **BUILT WIDER THAN THE VIEW** (`kTerrainMargin`), so that a picture one build old still reaches
	// the edges of a chart the view has moved since. The CENTRE is the view's; only the span grows, so
	// the margin is symmetric here and the bias along the drag is the next step rather than this one.
	window.travelYCentre = viewCentre();
	window.travelYSpan = builtSpan(kBaseZones / terrainZoom_ / 1023.0);
	window.travelXCentre = travelCentre();
	window.travelXSpan = builtSpan(kBaseTravel / terrainZoom_);
	// THE SELECTED SOUND'S SEVEN FIELD SCALES — the smoothness override (doc §11), its FM depth, and
	// the five field modifier knobs of 2026-08-25. The flagship builder scales each field by its own
	// before measuring terrain features, so the map shows the scaled (audible) character.
	//
	// THESE AND THE OTHER DESTINATION READS BELOW ARE THE WINDOW FIELDS A ROUTE CAN REACH (Warp depth,
	// Resonate's Stiffness and Coupling, the Basic Shapes mix, and on Additive the Shape Amount and the six
	// bias knobs), and therefore the only ones where `value()` versus `modulated()` is a live question at
	// all. The seven here and Additive's seven are pinned to base by test_gui_widgets_dahdsr's "the terrain
	// window is assembled from bases"; Warp, Resonate's two and the mix are not yet. The engine, the zone
	// baseline and the offset baseline above are read the same way, but Zone, Offset and OscType are not
	// modulation destinations — params.h: *"A32 IS UNTOUCHED. Explore watches IDENTITY — Zone, Offset,
	// Gamma"* — so for those three the two roads cannot differ.
	//
	// BOTH ARE IN THE CONTENT KEY. This comment used to say smoothnessScale was NOT, and concluded
	// that modulating the knob therefore could not ask for a rebuild. terrain.h's hand-written
	// `operator==` names `smoothnessScale` and `fmDepth` in its last line, so that reason was false
	// and only its conclusion was true — held up by the `value()` on this line rather than by the key.
	// A wrong reason for a right behaviour is worse than no reason: it tells the next reader that
	// switching this to `modulated()` is free, and the price is a 128x128 rebuild per modulated block.
	//
	// SEVEN NOW, NOT TWO (2026-08-25) — and every word above applies unchanged to the five that
	// joined, which is why they are read through the same lambda rather than five more spelled-out
	// lines. THIS IS THE SECOND HALF OF THE REQUIREMENT, and it is the half that is a constraint
	// rather than a feature: the controls become modulation destinations, and modulating them
	// must NOT trigger a terrain rebuild (backlog §4). All five are in terrain.h's content key and
	// all five are modulation destinations, so `value()` on this line is now the ONLY thing standing
	// between a moving modulator and a 128x128 rebuild per block.
	//
	// It held before by accident and unguarded. It is guarded now: a case pins these seven (and Additive's
	// seven) to base and fails if anyone switches them to modulated().
	//
	// AND THE WIDGET NEXT DOOR READS THE OPPOSITE ROAD ON PURPOSE, which is the thing neither file
	// recorded until now and the reason this paragraph is longer than it looks like it needs to be.
	// `WaveDisplay::characterNow()` reads MODULATED, and says why: *"the picture is of the sound being
	// played, so it follows host modulation exactly as the engine does."* That is correct THERE and
	// wrong HERE, and the difference is cost, not principle — a scope redraws a cycle, a terrain
	// rebuilds 128x128.
	//
	// So the hazard is a reader who has just learned the scope's rule and "fixes" this to match. The
	// symptom would be a rebuild per modulated block, which is the expensive half of exactly what
	// the design rules out. Backlog §4, and the distinction is now written down at the end that would
	// be edited rather than only at the end that would be cited.
	const auto base = [this](ParamId id) {
		return static_cast<float>(bridge_.value(static_cast<clap_id>(id)));
	};
	const size_t sel = slot;
	window.smoothnessScale = base(smoothnessParam(sel));
	window.fmDepth = base(fmDepthParam(sel));
	window.quantizeScale = base(quantizeParam(sel));
	window.syncScale = base(syncParam(sel));
	window.feedbackScale = base(feedbackParam(sel));
	window.pointScale = base(pointsParam(sel));
	window.gritScale = base(gritParam(sel));
	// WARP: the depth is the selected sound's, the mode is the ENGINE's — so the mode is read from
	// the global parameter and not through `sel`. Both are in the window's content key, so moving
	// either repaints the map (terrain.h says why that is a hand-written list).
	window.warpDepth = base(warpParam(sel));
	window.warpMode = static_cast<feathers::WarpMode>(std::clamp(
	    static_cast<int>(std::lround(bridge_.value(static_cast<clap_id>(ParamId::WarpMode)))), 0,
	    static_cast<int>(feathers::WarpMode::kCount) - 1));
	// AND THE RESONATE STRING'S MODEL, engine-wide like the warp mode and keyed for the same reason.
	window.weaveModel = static_cast<feathers::WeaveModel>(steppedIndex(
	    bridge_.value(static_cast<clap_id>(ParamId::ResonateModel)), kResonateModelNames.size()));
	// And Resonate's Stiffness and Coupling, from the BASE values as the rest of this window is.
	window.resonateShaping.stiffness =
	    static_cast<float>(std::exp2(bridge_.value(static_cast<clap_id>(ParamId::ResonateStiffness))));
	window.resonateShaping.coupling =
	    static_cast<float>(std::exp2(bridge_.value(static_cast<clap_id>(ParamId::ResonateCoupling))));
	// And the Basic Shapes mix — on that engine only, so a shape knob turned on another engine is not a new map.
	if (window.engine == OscType::BasicShapes) {
		const auto base = [this](ParamId id) { return static_cast<float>(bridge_.value(static_cast<clap_id>(id))); };
		window.shapesMix = { base(ParamId::ShapesSine), base(ParamId::ShapesTriangle), base(ParamId::ShapesSaw),
			                 base(ParamId::ShapesSquare) };
	}
	// And the additive Shape and the selected sound's Shape Amount — on Additive only, for the same reason.
	if (window.engine == OscType::Additive) {
		window.additive.shape = static_cast<partials::ShapeKind>(
		    steppedIndex(bridge_.value(static_cast<clap_id>(ParamId::AdditiveShape)), kAdditiveShapeNames.size()));
		window.additive.shapeAmount =
		    static_cast<float>(bridge_.value(static_cast<clap_id>(shapeAmountParam(sel))));
		window.additive.bias = { base(additiveRolloffParam(sel)), base(additiveFormantParam(sel)),
			                     base(additiveOddEvenParam(sel)), base(additiveCombParam(sel)),
			                     base(additiveDetailParam(sel)), base(additiveOffGridParam(sel)) };
	}
	// And the selected sound's five string scalers — on Pluck only: the picture reads the place's material through them.
	if (window.engine == OscType::Pluck) {
		window.additive.string = { base(ksBrightnessParam(sel)), base(ksSustainParam(sel)), base(ksPickParam(sel)),
			                       base(ksNoiseParam(sel)), base(ksStretchParam(sel)) };
	}
	// And the selected sound's drum — on Drum only: its kind (the selected PAD's in Kit mode, the engine's in Pitched) and its
	// seven scalers, which the picture reads each place through.
	if (window.engine == OscType::Drum) {
		const bool kit = steppedIndex(bridge_.value(static_cast<clap_id>(ParamId::DrumMode)), kDrumModeNames.size())
		                 == static_cast<size_t>(DrumMode::Kit);
		const clap_id kindId = kit ? static_cast<clap_id>(drumParam(sel, DrumControl::Kind))
		                           : static_cast<clap_id>(ParamId::DrumPitchedKind);
		window.additive.drumKind = static_cast<drum::Kind>(steppedIndex(bridge_.value(kindId), kDrumKindNames.size()));
		const auto d = [&](DrumControl c) { return base(drumParam(sel, c)); };
		window.additive.drum = { d(DrumControl::FieldPitch), d(DrumControl::FieldLength), d(DrumControl::FieldSweep),
			                     d(DrumControl::FieldTone),  d(DrumControl::FieldNoise),  d(DrumControl::FieldSnap),
			                     d(DrumControl::FieldMetal) };
		// THE KIT-WIDE LAYER (2026-11-06), as the character applies it: the trims on top of the pad's scalers, and Kit Zone,
		// Offset and Gamma moving the place every row is built at (each pad's window, so every pad's mark moves with it too).
		if (kit) {
			const auto g = [&](ParamId id) { return base(id); };
			window.additive.drumKit = { g(ParamId::DrumKitPitch), g(ParamId::DrumKitLength), g(ParamId::DrumKitSweep),
			                            g(ParamId::DrumKitTone),  g(ParamId::DrumKitNoise),  g(ParamId::DrumKitSnap),
			                            g(ParamId::DrumKitMetal) };
			window.zoneBaseline = std::clamp(window.zoneBaseline + bridge_.value(static_cast<clap_id>(ParamId::DrumKitZone)), 0.0, 1023.0);
			window.offsetBaseline += bridge_.value(static_cast<clap_id>(ParamId::DrumKitOffset))
			                         + bridge_.value(static_cast<clap_id>(ParamId::DrumKitGamma));
		}
	}
	// AND THE PATCH'S LIKES (backlog ON): the builder scores every texel against them, so they are in the content key.
	// ONLY THIS ENGINE'S AND THIS SOUND'S (`likeApplies`): another terrain's envelope means nothing on this one.
	std::vector<TerrainLike> now;
	for (const TerrainLike& like : bridge_.terrainLikes().likes) {
		if (like.engine == static_cast<uint16_t>(window.engine) && like.slot == static_cast<uint16_t>(slot)
		    && (window.engine != OscType::Drum || like.kind == static_cast<uint16_t>(window.additive.drumKind))) {
			now.push_back(like);
		}
	}
	if (!selected) {
		window.likes = std::make_shared<const std::vector<TerrainLike>>(std::move(now));
	}
	else {
		if (likesShared_ == nullptr || *likesShared_ != now) {
			likesShared_ = std::make_shared<const std::vector<TerrainLike>>(std::move(now));
		}
		window.likes = likesShared_;
	}
	// **THE LIKES' PHASE WARP** (P-MÖBIUS): derived from this sound's likes, published to the synth for the voice, and carried
	// by the window so every texel is built through the same warp the voice plays.
	window.likeStrength = likeStrength_;
	window.likeWarp = likeWarpFor(slot, window);
	return window;
}

feathers::LikeWarp ExploreStage::likeWarpFor(size_t slot, const TerrainWindow& window) const {
	if (slot >= kMaxSounds) {
		return {};
	}
	// THE KEY IS WHAT THE WARP IS A FUNCTION OF — the engine, the strength and the likes (their angles) — never the world or
	// the view: a like's angles were captured when it was made, so a pan, a rotate or a knob is not a new warp.
	LikeWarpKey key { window.engine, window.likeStrength,
		              window.likes != nullptr ? *window.likes : std::vector<TerrainLike> {} };
	if (!likeKeys_[slot].has_value() || !(*likeKeys_[slot] == key)) {
		likeKeys_[slot] = std::move(key);
		likeWarps_[slot] = deriveLikeWarp(window);
		bridge_.publishLikeWarp(slot, static_cast<uint8_t>(window.engine), likeWarps_[slot]);
	}
	return likeWarps_[slot];
}

void ExploreStage::refreshLikeWarps() const {
	// EVERY SOUND, not only the selected one (likes are local to a single sound, by design — and each
	// sound's voice plays its own): a sound's warp follows its likes whether or not it is on screen.
	for (size_t slot = 0; slot < kMaxSounds; ++slot) {
		(void)terrainWindowFor(slot);
	}
}

TerrainWindow ExploreStage::askForTerrain() {
	// THE ZOOM THE MAP IS BUILT AT FOLLOWS THE VIEW'S ONLY ONCE IT HAS RESTED (`kZoomDwellMs`) OR MOVED FAR (`kZoomRebuildRatio`). Until then the picture on
	// screen is the last map, placed and scaled by the window it was built for (`terrainPlacement`), and a frame is
	// asked for so the rest is noticed without a hand moving.
	if (terrainZoom_ != zoom_) {
		const double apart = std::max(zoom_ / terrainZoom_, terrainZoom_ / zoom_);
		if (clock_() - zoomMovedAt_ >= kZoomDwellMs || apart >= kZoomRebuildRatio) {
			terrainZoom_ = zoom_;
		}
		else {
			repaint();
		}
	}
	const TerrainWindow window = terrainWindowNow();
	terrain_.request(window);
	return window;
}

bool ExploreStage::update() {
	// The width knob first: selection, engine and enablement all move between frames, and each of
	// the syncs is guarded, so a frame where nothing moved costs a handful of reads and no redraw.
	syncWidthKnob();

	// THE POLAR KNOBS READ PATCH STATE, and patch state has no parameter id for the widget registry to
	// notify them by — so a chart drag, an undo or a patch load moves what they show and nothing tells
	// them. Compared here, once a frame, against where the selected sound last stood: a frame where it
	// has not moved costs one read and no redraw, which keeps Explore's "no repaint at rest".
	//
	// AND THE STAR MOVES WITH THEM — it is drawn at the place plus this offset (`playedPlace`) — so a
	// change that did not come from the knobs' own gesture (an undo, a stance, a load) repaints too.
	bool dirty = false;
	const PolarOffset standing = bridge_.soundTravel().orbit[selection_.slot()];
	if (standing != polarSeen_) {
		polarSeen_ = standing;
		radiusKnobs_[selection_.slot()]->redraw();
		thetaKnobs_[selection_.slot()]->redraw();
		dirty = true;
	}

	// **AND THE ENGINE'S PULL ON IT, which is what makes a modulated star orbit.** Published every
	// block whether it moved or not, so it is COMPARED here rather than trusted to announce itself: a
	// frame where no modulator reaches the selected sound's place costs two reads and no repaint, and
	// Explore stays quiet at rest. A frame where one does repaints the chart — the star has moved —
	// and the two knobs, whose marks ride the same numbers. NOTHING ELSE HAPPENS: no crumb, no glide,
	// no terrain request. The view is not asked to follow, because the authored place has not changed.
	{
		const TravelPulls pulls = bridge_.travelPulls();
		const size_t slot = selection_.slot();
		if (pulls.radius[slot] != pullSeenRadius_ || pulls.theta[slot] != pullSeenTheta_) {
			pullSeenRadius_ = pulls.radius[slot];
			pullSeenTheta_ = pulls.theta[slot];
			radiusKnobs_[slot]->redraw();
			thetaKnobs_[slot]->redraw();
			dirty = true;
		}
	}
	const int64_t now = clock_();
	if (!everTicked_) {
		lastTick_ = now;
		everTicked_ = true;
	}
	// **AND NOTHING MARKS THIS SURFACE DIRTY ON A TICK ANY MORE** (the sky was dropped).
	// `shimmer_` used to advance here and set `dirty` unconditionally, so the whole Explore
	// stage re-rasterised every frame forever with no hand on anything — and the only thing it bought
	// was the starfield's twinkle. Measured at ten repaints over ten still frames (backlog GJ). The
	// sky is gone, so the repaints are too: this surface is now quiet at rest and redraws when
	// something actually moves.
	lastTick_ = now;

	// **AND THE REST OF A CAPPED DRAG ARRIVES HERE**, while the chart is held: a fling asks for more than
	// `kViewsPerSecond` allows, so the star keeps walking toward where the pointer last was over the frames that
	// follow (`dragStarTo`), and the view comes along as it does inside the event.
	//
	// **AND THE VIEW'S OWN JOURNEY**, which is a separate walk from the star's: a click moves the star
	// instantly and leaves the view to catch up, so this runs whether or not the chart is held. Marked
	// dirty while it runs, because a pan that nobody repaints is a teleport with extra steps.
	if (glidingView_) {
		stepViewGlide();
		dirty = true;
	}

	// AND AFTER THE RELEASE TOO, until it arrives: `settlingScrub_` is the released drag still coasting
	// to the place the hand asked for. Without it the cap silently kept whatever it had not yet handed
	// over, which is a sound that stops short of the pointer.
	if (grabbed_ == Part::Chart || settlingScrub_) {
		const double wasHome = selectedDriftY();
		const double wasTravel = selectedDrift();
		dragStarTo(dragWantedHome_, dragWantedTravel_);
		if (selectedDriftY() != wasHome || selectedDrift() != wasTravel) {
			settleOn(keepInsideView(viewDriftY(), viewCentre(), homeSpanZones() / 1023.0, kScrubEdgeMargin),
			         keepInsideView(viewDrift(), travelCentre(), travelSpan(), kScrubEdgeMargin));
			dirty = true;
		}
		// ARRIVED, OR STOPPED MOVING AT ALL — and the second half is what stops a settle from running
		// forever. `keepInsideView` and the parameter's own bounds can both refuse the last fraction of
		// what was asked for, and a target that cannot be reached is still a gesture that has to end.
		if (settlingScrub_
		    && ((selectedDriftY() == dragWantedHome_ && selectedDrift() == dragWantedTravel_)
		        || (selectedDriftY() == wasHome && selectedDrift() == wasTravel))) {
			finishScrub();
			dirty = true;
		}
	}

	// The fill: a new snapshot arriving is the only reason a still chart needs repainting.
	const std::shared_ptr<const TerrainSnapshot> terrain = terrain_.snapshot();
	if (terrain != lastSnapshot_) {
		lastSnapshot_ = terrain;
		dirty = true;
	}
	// And the star wandering out of the view under automation or a preset load. Never mid-gesture:
	// re-centring under the hand would move the ground the scrub is measured against.
	//
	// THIS IS THE AUTOMATION ROAD, and A29 is the policy it implements: the view follows automated
	// IDENTITY (this branch) and never follows PERFORMANCE (A32 — the surface does not even watch the
	// performance params). It goes through followSelected rather than recentre so that how FAR it moves
	// is one constant, kFollowMargin, which currently says centre.
	if (grabbed_ == Part::None && everCentred_ && centredOnSlot_ == selection_.slot()) {
		const float y = yAtTravelY(viewDriftY());
		if (y < 0.0f || y > height()) {
			followSelected(y < 0.0f);
			dirty = true;
		}
	}
	if (dirty) {
		repaint();
	}
	return dirty;
}

//---------//
// Drawing //
//---------//

namespace {

} // namespace

int ExploreStage::terrainQuadCount() const {
	// ONE, OR NONE. The map is a picture, and it is now drawn as one — a single textured quad through
	// Canvas::imageRaw instead of a quad per texel. The number is kept readable because it was the
	// answer to "how close is this surface to the renderer's ceiling", and the honest answer changed
	// from "twelve thousand nine hundred, mind the arena" to "one".
	const std::shared_ptr<const TerrainSnapshot> terrain = terrain_.snapshot();
	return terrain->window.travelYSpan > 0.0 && (terrain->rows > 0 || terrain->carried) ? 1 : 0;
}

visage::Bounds ExploreStage::terrainPlacement() const {
	// THE MAP'S OWN COORDINATES, not the widget's. While a zoom is in flight the map on screen belongs
	// to the window it was BUILT for, so it is placed and SCALED by that window; that is what makes a
	// zoom scale the picture it has instead of jumping.
	const std::shared_ptr<const TerrainSnapshot> terrain = terrain_.snapshot();
	const TerrainWindow& window = terrain->window;
	const float cell = std::abs(xAtTravelX(TerrainMap::travelXAtColumn(window, 1))
	                            - xAtTravelX(TerrainMap::travelXAtColumn(window, 0)));
	const float left = xAtTravelX(TerrainMap::travelXAtColumn(window, 0)) - cell * 0.5f;
	const float right =
	    xAtTravelX(TerrainMap::travelXAtColumn(window, TerrainMap::kResolution - 1)) + cell * 0.5f;
	// AND IN THIS FRAME'S GAMMA, which is the half that was missing and the reason the terrain still
	// flickered, the way rotate used to, after the blank was fixed.
	//
	// A snapshot is placed by the window it was built for. That is right for a zoom and right for a pan,
	// where the two windows share a gamma — but the view now FOLLOWS gamma, so during a sweep the view
	// runs away from the map at up to one and a half viewports PER FRAME. Placed by absolute home, a map
	// one gamma-tick old is drawn off the widget entirely: measured through a real corner drag at a
	// hand's speed, the picture sat at y 812..1330 of a 0..534 widget, 0% overlap, on all but the frames
	// where a build had just published with the current window. The buffer was 100% full the whole time,
	// which is exactly why a published-map measurement called it perfect.
	//
	// **AND NOTHING IS TRANSLATED INTO A GAMMA FRAME ANY MORE** (backlog GY). There used to be an
	// `intoNow = viewGamma() - window.viewGamma` added to both ends here, because the view followed
	// gamma one for one and a map built one gamma-tick ago belonged somewhere else on the glass. With
	// both axes displacements in the decoded plane, a map's place on screen is its own travel range
	// against this frame's — gamma moves the whole plane and moves no texel within it, so there is no
	// shift to undo. The line that was the flicker's other half is simply gone.
	const float top = yAtTravelY(TerrainMap::travelYAtRow(window, 0)) - cell * 0.5f;
	const float bottom =
	    yAtTravelY(TerrainMap::travelYAtRow(window, TerrainMap::kResolution - 1)) + cell * 0.5f;
	return { left, top, right - left, bottom - top };
}

TerrainGround::TerrainGround() {
	onDraw() = [this](visage::Canvas& canvas) {
		if (stage_ != nullptr) {
			stage_->drawGround(canvas);
		}
	};
	// **THE LAYER IS NAILED UP BEFORE THE EFFECT GOES ON IT, AND THIS LINE IS LOAD-BEARING** (2026-09-19,
	// on the first build with a runtime bubble switch: a strip of canvas was never rendered and sat black
	// in the viewport, perpendicular to the direction of the pan).
	//
	// `requiresLayer()` is `post_effect_ || backdrop_effect_ || cached_ || masked_ || alpha != 1`, and
	// `Frame::drawToRegion` re-asks it on EVERY draw. So with the effect as the only thing holding the
	// layer up, taking the effect off ran `setNeedsLayer(false)` mid-flight — `removeFromPackedLayer`,
	// the intermediate region dropped, the layer decremented — and a strip that needed paint right then
	// never got it. Permanently black, because nothing comes back to a region that was restructured
	// underneath it.
	//
	// `setCached(true)` is the smallest thing that fixes it and it costs nothing else: `cached_` is
	// read in `requiresLayer()` and NOWHERE ELSE in the framework — it does not suppress a repaint,
	// cache a picture, or change what is drawn. It only means "this frame needs a layer", which this
	// frame does, permanently and for a reason that has nothing to do with which effect is on it.
	//
	// **KEPT AFTER THE SWITCH THAT EXPOSED IT WAS REMOVED** (2026-09-20). Nothing in this codebase
	// takes the post effect off any more, so `requiresLayer()` is true through `post_effect_` alone
	// and this line changes no behaviour today. It stays because the hazard is the FRAMEWORK's, not
	// the switch's: `drawToRegion` re-asks the question on every draw, so the day anyone adds a
	// toggle back — or a mask, or an alpha — this frame's layer starts depending on it again. One
	// line, and the defect it prevents cost a black strip that never repainted.
	setCached(true);
	// THE BUBBLE IS THE WHOLE REASON THIS FRAME EXISTS — see the header. Still set here rather than at
	// a draw, because a first frame that painted flat and then bent would be a visible pop.
	setPostEffect(&bubble_);
	// AND IT IS NOT IN THE WAY. The ground covers the same rectangle as the stage and sits under it, so
	// anything the stage declines would otherwise land HERE and be swallowed by a frame whose whole job
	// is to be looked at. A picture takes no clicks.
	setIgnoresMouseEvents(true, false);
}

void ExploreStage::drawGround(visage::Canvas& canvas) {
	++groundPaints_;
	// THE SHADER CANNOT WORK THE DISC OUT FOR ITSELF — see `u_bubble` in fs_bubble.sc. A fraction of the
	// frame's width rather than a pixel count, so it is the same number at every display scale.
	bubbleRadiusShare_ = width() > 0.0f ? disk().radius() / width() : 0.0f;
	if (ground_ != nullptr) {
		ground_->setBubbleRadiusShare(bubbleRadiusShare_);
	}
	canvas.setColor(tokens::kVoidTerrain);
	canvas.fill(0.0f, 0.0f, width(), height());
	drawTerrainPicture(canvas);
}

void ExploreStage::drawTerrainPicture(visage::Canvas& canvas) {
	// ONE SNAPSHOT FOR THE WHOLE DRAW. The window, the row count and the texels come from the same
	// immutable object, so this frame cannot mix one map's coordinates with another's colours — which
	// is precisely the "colour that lies about where it is" the file's own rule forbids, and precisely
	// what three separate reads of a live map used to allow.
	const std::shared_ptr<const TerrainSnapshot> terrain = terrain_.snapshot();
	const TerrainWindow& window = terrain->window;
	if (window.travelYSpan <= 0.0 || (terrain->rows <= 0 && !terrain->carried)) {
		return;
	}
	// THE ATLAS KEYS AN IMAGE ON ITS DATA POINTER, so the buffer has to be one buffer that gets
	// overwritten — a snapshot is a fresh allocation every publish, and handing those over would be a
	// new atlas entry per build. Copied when the snapshot changes rather than per frame: 64KB about
	// eight times a build, against a build that already costs tens of milliseconds.
	//
	// THE WHOLE BUFFER, because the rows the builder has not reached are no longer empty: they carry
	// the previous map, resampled to this window's grid, which is what stops a rebuild from blanking
	// the chart (see TerrainSnapshot::carried). Copying only the built rows and zeroing the rest —
	// which is what this did — would throw that away and put the flicker straight back.
	//
	// Whatever is genuinely void is void IN the buffer: unreachable rows are cleared by the builder
	// and never-covered positions were never written. So alpha still says "there is nothing here", and
	// it says it about the same places it always did.
	if (terrain != terrainPixelsFrom_) {
		std::copy_n(terrain->texels.begin(), terrainPixels_.size(), terrainPixels_.begin());
		terrainPixelsFrom_ = terrain;
		terrainPixelsSerial_ = freshPixelSerial();
	}
	// **EITHER THE TEXELS OR THE CUBIC'S PICTURE, AND THE SWITCH IS LIVE** (`TerrainMap::fineWanted`).
	// Without `fine` the draw hands `imageRaw` the 128 texels and the atlas filters them BILINEARLY for
	// free — canvas.h says so at the call site. With it, a 512-square picture goes up instead and the
	// magnification is near 1:1. The still-frame difference between the two is RMSE 1.21 of 255; what
	// is being asked is whether they differ while the picture MOVES, which no still frame can answer.
	if (terrain != terrainPixelsFrom_ || terrainFineFrom_ != terrain) {
		if (terrain->fineScale > 0 && !terrain->fine.empty()) {
			terrainFine_.assign(terrain->fine.begin(), terrain->fine.end());
			terrainFineSerial_ = freshPixelSerial();
		}
		else {
			terrainFine_.clear();
		}
		terrainFineFrom_ = terrain;
	}
	const bool fine = !terrainFine_.empty() && terrain->fineScale > 0;
	const int drawnResolution = TerrainMap::kResolution * (fine ? terrain->fineScale : 1);

	// THE MAP'S OWN COORDINATES, not the widget's — the same rule the per-texel draw followed. While a
	// zoom is in flight the map on screen belongs to the window it was BUILT for, so it is placed and
	// SCALED by that window; that is what makes a zoom scale the picture it has instead of jumping.
	const visage::Bounds where = terrainPlacement();
	const float left = where.x();
	const float top = where.y();

	// BGRA8 BECAUSE OUR TEXELS ARE `Color`-SHAPED. terrainColour builds 0xAARRGGBB, which is
	// visage::Color's own order and B,G,R,A in memory; the atlas is RGBA8 and converts on upload.
	// Getting this wrong exchanges red and blue — see the note in the UI plan.
	canvas.setColor(0xffffffffu);
	canvas.imageRaw(reinterpret_cast<const unsigned char*>(fine ? terrainFine_.data() : terrainPixels_.data()),
	                drawnResolution, drawnResolution, visage::ImageFormat::BGRA8, left, top, where.width(),
	                where.height(), fine ? terrainFineSerial_ : terrainPixelsSerial_);
}

void ExploreStage::drawRimMask(visage::Canvas& canvas, const StageDisk& d) const {
	// AND THE CHART IS A CIRCLE. The per-texel draw got both of these from its own loop: it faded each
	// cell by the mock's `195*(1-r³)+55` and skipped anything past the rim. A picture cannot skip its
	// own corners, so both come back as ONE radial brush over the whole surface — transparent where the
	// chart is bright, and the void's own colour from the rim outward, which is the mask.
	//
	// Since the planet (backlog FE) this is only the MASK — transparent inside the rim, the void outside it.
	//
	// **ITS OWN FUNCTION SINCE 2026-09-19, AND THAT IS THE FIX RATHER THAN A TIDY-UP.** It is a statement
	// about the DISK and nothing else — no terrain reaches it — so it must not be skipped on the frames
	// where the terrain is momentarily absent. It was, and a picture drawn by the ground frame in the
	// same visual frame then had nothing hiding its corners. Two call sites now, both unconditional.
	const float radius = d.radius();
	const float reach = std::hypot(std::max(d.centre().x, width() - d.centre().x),
	                               std::max(d.centre().y, height() - d.centre().y));
	const float rimAt = radius / std::max(1.0f, reach);
	const visage::Gradient vignette = visage::Gradient::fromSampleFunction(
	    kVignetteSamples, [rimAt](float t) {
		    if (t >= rimAt) {
			    return visage::Color(tokens::kVoidTerrain);
		    }
		    // INSIDE THE RIM, NOTHING: the planet's own limb darkening is the fade now (backlog FE), and it is earned by
		    // the sphere rather than painted by the mock's `195*(1-r³)+55`.
		    return visage::Color(withAlpha(tokens::kVoidTerrain, 0.0f));
	    });
	canvas.setColor(visage::Brush::radial(vignette, d.centre(), reach));
	canvas.fill(0.0f, 0.0f, width(), height());
}

void ExploreStage::drawTerrain(visage::Canvas& canvas) {
	// THE SPHERE'S OWN LIGHT AND ITS EDGE, which are the half of this that must NOT bend. Both are
	// already statements about the ball as it appears — where the light falls across a disc, where the
	// disc stops — so putting them inside the warped frame would project a projection.
	// **THE MASK IS NOT A FUNCTION OF THE TERRAIN, AND IT USED TO BE SKIPPED AS THOUGH IT WERE** (2026-09-19,
	// on the installed build: a render glitch drew the square extended region for an instant, overflowing
	// the circle).
	//
	// THE GROUND AND THE STAGE EACH TAKE THEIR OWN SNAPSHOT. `drawGround` reads `terrain_.snapshot()`
	// and paints the picture; this function reads it AGAIN, a separate call on a separate frame, and
	// the rim mask that hides the picture's corners was inside an early return guarded on what that
	// SECOND read happened to see. A map published between the two — an empty one, as a world change
	// makes — left the ground painting a picture that the stage then declined to mask. One frame of a
	// bare square, which is exactly what was reported.
	//
	// IT IS OLDER THAN THE MARGIN AND THE MARGIN MADE IT VISIBLE. The map was always the disk's
	// BOUNDING SQUARE, so its corners were always outside the circle; at `kTerrainMargin` 1.35 there is
	// simply a great deal more of it to see.
	//
	// So the mask is drawn unconditionally now, from `disk()` alone, which is what it was always a
	// statement about. Only the parts that genuinely need a map stay behind the guard below.
	const std::shared_ptr<const TerrainSnapshot> terrain = terrain_.snapshot();
	const bool haveMap = terrain->window.travelYSpan > 0.0 && (terrain->rows > 0 || terrain->carried);
	const StageDisk d = disk();
	if (!haveMap) {
		drawRimMask(canvas, d);
		return;
	}

	// **AND LIT AS A PLANET** (backlog FE). The disk is read as a sphere seen
	// head-on and the selected star is a satellite in low orbit over it: its light is MULTIPLIED over the map here, before
	// the rim mask, the grid and the sky, so only the ground is lit. The shader has the geometry; this hands it where the
	// star is, in disk radii, and the light's colour.
	const StageDisk planet = disk();
	const float planetRadius = std::max(1.0f, planet.radius());
	// WHERE THE STAR IS DRAWN, not where it stands on the sheet: the shader lights a sphere in SCREEN
	// coordinates, so it wants the satellite's projected position and would otherwise light the wrong
	// part of the ball — hardest near the rim, which is exactly where the lighting says the most.
	const visage::Point star = glassAt(viewDriftY(), viewDrift());
	float starX = std::isfinite(star.x) ? (star.x - planet.centre().x) / planetRadius : 0.0f;
	float starY = std::isfinite(star.y) ? (star.y - planet.centre().y) / planetRadius : 0.0f;
	// FEATHERS_PLANET_STAR=<x>,<y> PUTS THE SATELLITE SOMEWHERE, in disk radii, and it exists for the reason
	// FEATHERS_EXPLORE_ZOOM does (A17): where the star stands is a CHANNEL rather than a parameter, so a venue with no
	// pointer cannot move it — which left the lighting photographable in one position only.
	if (const char* placed = std::getenv("FEATHERS_PLANET_STAR")) {
		float x = 0.0f;
		float y = 0.0f;
		if (std::sscanf(placed, "%f,%f", &x, &y) == 2 && std::isfinite(x) && std::isfinite(y)) {
			starX = x;
			starY = y;
		}
	}
	const visage::Color hue(kSeedHues[hueIndexOf(bridge_.soundHues(), selection_.slot())]);
	const auto tint = [](float channel) { return 1.0f + kPlanetTintShare * (channel - 1.0f); };
	// **THE PLANET LIGHTING, SWITCHABLE** — two shader draws, `planetShade_` MULTIPLIED over the map
	// and `planetAir_` ADDED over the limb. Neither carries a layer or a gradient, so this gate removes
	// exactly two shapes and nothing structural; it is the other half of the question the MASK switch
	// asks, because "gradient shading" could mean either the vignette or the sphere.
	{
		planetShade_.setUniformValue("u_planet_light", starX, starY, kPlanetTerminator, kPlanetAmbience);
		planetShade_.setUniformValue("u_planet_tint", tint(hue.red()), tint(hue.green()), tint(hue.blue()), 0.0f);
		canvas.setColor(0xffffffffu);
		canvas.shader(&planetShade_, planet.centre().x - planetRadius, planet.centre().y - planetRadius,
		              2.0f * planetRadius, 2.0f * planetRadius);
	}

	// THE MASK GOES ON WHATEVER THE SHADE DID, and it is NOT inside the gate above: it is the thing
	// that hides the map's corners, so skipping it with the lighting would make one switch do two
	// jobs and neither answer cleanly. Its own switch is inside `drawRimMask`.
	drawRimMask(canvas, d);

	// **AND ITS ATMOSPHERE**, ADDED over the map and the rim (backlog FE, "bonus points for atmospheric diffusion"): a thin
	// lit shell at the limb, out past it into the void, and a haze under the satellite.
	//
	planetAir_.setUniformValue("u_planet_light", starX, starY, kPlanetTerminator, kPlanetAmbience);
	planetAir_.setUniformValue("u_planet_tint", tint(hue.red()), tint(hue.green()), tint(hue.blue()), 0.0f);
	planetAir_.setUniformValue("u_planet_air", kPlanetAirReflectivity, kPlanetAirThickness, kPlanetAirScale,
	                           kPlanetAirNightFloor);
	const float airRadius = planetRadius * kPlanetAirScale;
	canvas.setColor(0xffffffffu);
	canvas.shader(&planetAir_, planet.centre().x - airRadius, planet.centre().y - airRadius, 2.0f * airRadius, 2.0f * airRadius);
}

double ExploreStage::gridLineStep(double span) {
	// The line ladder, and every rung it ALREADY REACHED is untouched: this spacing has been reviewed
	// on screen and approved, and the lines were never the problem.
	//
	// IT GAINS RUNGS AT THE TOP because the ladder does. A 64-zone step draws about 48 lines over the
	// 3069-zone stop this surface used to end at — countable, which is why it stopped there — and would
	// draw 768 of them at the new one, which is not a grid but a grey wash, and a few hundred more
	// strings for the atlas to chew on. So the step carries on doubling in fours past the old end, and
	// the count stays where it was approved: 48 lines at 12,288 zones, 48 again at 49,152.
	//
	// THE THRESHOLD IS THE OLD STOP, not a round number near it: below 3072 zones this function returns
	// exactly what it always returned, so no view anyone has seen moves by a pixel.
	if (span <= 6.0) {
		return 1.0;
	}
	if (span <= 24.0) {
		return 4.0;
	}
	if (span <= 96.0) {
		return 16.0;
	}
	if (span <= 3072.0) {
		return 64.0;
	}
	if (span <= 12288.0) {
		return 256.0;
	}
	return 1024.0;
}

double ExploreStage::gridLabelStep(double span) {
	// LABELS ARE AXIS TICKS, NOT ANNOTATIONS ON EVERY LINE, and this is the fix for a bug caught
	// in a screenshot: at a wide view the chart was papered in them. The line step tops out at 64 zones,
	// so the widest stop draws about 48 lines and EVERY ONE carried a label — stacked ten pixels
	// apart, unreadable, and at a large gamma base every string is seven digits and one nobody has
	// drawn before. That second half is why it is worth fixing carefully rather than tastefully: a few
	// hundred novel glyphs per view is the heaviest font-atlas churn this plugin can produce, and
	// atlas growth is the environment every wedge sighting has been correlated with. Correlation,
	// not mechanism — this starves a trigger environment, it does not fix a defect.
	//
	// A MULTIPLE OF THE LINE STEP, by construction rather than by luck, so a label always lands ON a
	// line; and anchored to absolute zone zero rather than to the view, so the same view always labels
	// the same zones and a scroll does not make the numbers crawl.
	//
	// AND THE NUMBER ITSELF IS ABSOLUTE HOME, which is a DECISION and not an accident (A37). It was an
	// accident once: while the chart wrote Zone and Offset, these labels agreed with the Zone parameter
	// only at gamma 0, and the fix on the table was to make them band-relative. The one-value design
	// removed the premise — the chart scrubs HOME now and the Zone dial is a bounded knob summing on
	// top, exactly as gamma sums into the wave index on the hardware — so home IS this axis and labelling
	// it in anything else would be labelling a coordinate the chart does not move. Band-relative here
	// would also read backwards under a drag: a scrub cannot change place-in-band, so the star's own
	// number would sit frozen while it visibly travelled and the numbers on the motionless terrain
	// would crawl.
	const double lines = gridLineStep(span);
	const double perLabel = std::max(1.0, std::ceil((span / lines) / kWantedGridLabels));
	return lines * perLabel;
}

void ExploreStage::drawGrid(visage::Canvas& canvas) {
	const StageDisk d = disk();
	const float radius = d.radius();
	const double span = homeSpanZones();
	// Zone gridlines run CROSSWISE — horizontal lines you fly across — because the chart's primary
	// direction is vertical. The step thins out as the span grows so the lines stay countable.
	const double step = gridLineStep(span);
	const double labelStep = gridLabelStep(span);
	// **THE VERTICAL IS TRAVEL, AND THE LABELS SAY SO** (backlog GY). The old note here explained that
	// `viewCentre()` had to be the FOLLOWED centre so the grid did not sit still while the terrain
	// moved under it — true of a chart whose vertical was home and whose view chased gamma. Neither is
	// true now.
	//
	// WHAT WAS LEFT BEHIND WAS A LIE ON THE FACE OF THE CHART. The lines are still computed in
	// thousandths (`* 1023`, `/ 1023`) because that is the ladder `gridLineStep` was tuned on and the
	// spacing is good — but they were LABELLED `z<N>`, so a travel-Y value was being printed as a zone
	// number. The whole condition for this refactor is that the encode is honest about where you
	// are; a gridline that names the wrong coordinate fails it in the most direct way available.
	//
	// So the arithmetic keeps its units and the LABEL tells the truth: these are travel offsets, in
	// the same +/-0.000 form the drift readout uses, because they are the same quantity.
	const double centreZone = viewCentre() * 1023.0;
	const double first = std::ceil((centreZone - span * 0.5) / step) * step;

	gridLabels_ = 0;
	gridBow_ = 0.0f;
	// The line nearest the middle, kept in case the ticks all miss: see below.
	double nearestZone = 0.0;
	visage::Point nearestLeft {};
	float nearestHalf = 0.0f;
	const visage::Font& label = font(kKnobLabelSize);
	for (double zone = first; zone <= centreZone + span * 0.5; zone += step) {
		const float y = yAtTravelY(zone / 1023.0);
		const float dy = y - d.centre().y;
		if (std::abs(dy) >= radius) {
			continue;
		}
		// Clipped to the circle by arithmetic rather than by drawing over the rim: the chord's own
		// half-width at this height. The chord is measured on the SHEET, and both of its ends are on the
		// world's edge — which the bubble draws on the rim, so the curve ends where the line used to.
		const float half = std::sqrt(radius * radius - dy * dy);
		canvas.setColor(withAlpha(tokens::kGrid, 0.14f));
		const GlassCurve line = curveOnGlass({ d.centre().x - half, y }, { d.centre().x + half, y });
		// HOW FAR THIS LINE BOWS, KEPT AS A NUMBER. A curve cannot be read off a headless canvas, and
		// "the grid is bent" is precisely the kind of claim that a difference against a rendered buffer
		// would accept from any change at all. The deepest bow over the whole grid is the value: a
		// straight grid scores zero, whatever else is on the chart. The line THROUGH the middle is
		// genuinely straight — a diameter is a great circle seen edge-on — so the maximum is taken
		// rather than any one line's.
		const float chordY = (line.points[0].y + line.points[line.count - 1].y) * 0.5f;
		gridBow_ = std::max(gridBow_, std::abs(chordY - line.points[line.count / 2].y));
		for (int i = 1; i < line.count; ++i) {
			canvas.segment(line.points[i - 1].x, line.points[i - 1].y, line.points[i].x,
			               line.points[i].y, 1.0f, false);
		}
		// AND THE LABEL FOLLOWS THE CURVE DOWN. It is written just inside the left end, which the bubble
		// has moved: at this height the line now starts on the rim rather than on the flat chord, and a
		// label left at the old height would sit off its own line.
		const visage::Point leftEnd = line.points[0];
		if (half > nearestHalf) {
			nearestZone = zone;
			nearestLeft = leftEnd;
			nearestHalf = half;
		}
		// Only the ticks, and only where there is chart to write on. `remainder` rather than `fmod` so
		// the test is symmetric about zero — a label at zone −512 is as much a multiple of 512 as one
		// at +512, and the negative half of the axis is exactly where a large base puts you.
		const bool isTick = std::abs(std::remainder(zone, labelStep)) < 0.5;
		if (isTick && half > radius * 0.5f) {
			char printed[16] = {};
			std::snprintf(printed, sizeof(printed), "%+.3f", zone / 1023.0);
			canvas.setColor(withAlpha(tokens::kGrid, 0.40f));
			canvas.text(printed, label, visage::Font::kLeft, leftEnd.x + 6.0f, leftEnd.y - 12.0f,
			            60.0f, 11.0f);
			++gridLabels_;
		}
	}

	// AND AT LEAST ONE, ALWAYS. At the tightest zoom the view spans about a zone, so there may be a
	// single gridline in it and that line may sit near the rim where there is no room to write — which
	// leaves a chart with no number on it at all. The ticks are a ceiling on how many labels there
	// are; this is the floor. Bounded either way is the whole claim.
	if (gridLabels_ == 0 && nearestHalf > 0.0f) {
		char printed[16] = {};
		std::snprintf(printed, sizeof(printed), "%+.3f", nearestZone / 1023.0);
		canvas.setColor(withAlpha(tokens::kGrid, 0.40f));
		canvas.text(printed, label, visage::Font::kLeft, nearestLeft.x + 6.0f, nearestLeft.y - 12.0f,
		            60.0f, 11.0f);
		gridLabels_ = 1;
	}

	// THE IDENTITY RAIL: the column of travel zero, which is where a sound can actually be stored.
	// Everything either side of it is country you reach by standing somewhere, not by being something,
	// and drawing the line is how the surface says so without a caption.
	const float railX = xAtTravelX(0.0);
	if (std::abs(railX - d.centre().x) < radius) {
		const float dx = railX - d.centre().x;
		const float half = std::sqrt(radius * radius - dx * dx);
		canvas.setColor(withAlpha(tokens::kGammaAxis, 0.22f));
		// A MERIDIAN, not a column: the rail is a line on the sheet like any other, so it bows on the
		// glass the same way the zone lines do — away from the middle, hardest where it meets the rim.
		const GlassCurve rail =
		    curveOnGlass({ railX, d.centre().y - half }, { railX, d.centre().y + half });
		for (int i = 1; i < rail.count; ++i) {
			canvas.segment(rail.points[i - 1].x, rail.points[i - 1].y, rail.points[i].x,
			               rail.points[i].y, 1.0f, false);
		}
	}
}

ExploreStage::InkBox ExploreStage::chartBox() const {
	return diskBox(disk());
}

void ExploreStage::overlayLine(visage::Canvas& canvas, visage::Point from, visage::Point to,
                               float thickness) {
	const std::optional<std::pair<visage::Point, visage::Point>> inside =
	    clipSegmentToDisk(disk(), from, to);
	if (!inside.has_value()) {
		return;
	}
	overlayInk_.add(inside->first, inside->second, thickness * 0.5f);
	canvas.segment(inside->first.x, inside->first.y, inside->second.x, inside->second.y, thickness,
	               true);
}

ExploreStage::GlassCurve ExploreStage::curveOnGlass(visage::Point flatFrom,
                                                    visage::Point flatTo) const {
	const StageDisk d = disk();
	GlassCurve curve;
	for (int i = 0; i <= kCurveSteps; ++i) {
		const float t = static_cast<float>(i) / static_cast<float>(kCurveSteps);
		const visage::Point flat { flatFrom.x + (flatTo.x - flatFrom.x) * t,
			                       flatFrom.y + (flatTo.y - flatFrom.y) * t };
		curve.points[curve.count++] = Bubble::toGlass(d, flat);
	}
	return curve;
}

void ExploreStage::overlayCurve(visage::Canvas& canvas, visage::Point flatFrom,
                                visage::Point flatTo, float thickness) {
	const GlassCurve curve = curveOnGlass(flatFrom, flatTo);
	for (int i = 1; i < curve.count; ++i) {
		overlayLine(canvas, curve.points[i - 1], curve.points[i], thickness);
	}
}

void ExploreStage::drawCrumbs(visage::Canvas& canvas) {
	const std::vector<SoundIdentity>& trail = crumbs_[selection_.slot()];
	if (trail.empty()) {
		return;
	}
	const uint32_t hue = kSeedHues[hueIndexOf(bridge_.soundHues(), selection_.slot())];

	// A constellation: star points joined by a faint line, brightening toward the most recent. Every
	// crumb is placed from its OWN stored identity — see crumbAt, which is where the bug was.
	//
	// CLIPPED, AND THE LINE IS THE REASON. A leg is drawn between two places in IDENTITY, and at a
	// tight zoom — where the view spans a fraction of a zone and the crumb was left in another
	// country — that is a line of tens of thousands of pixels. It used to be handed to the renderer at
	// that size, and a screenshot caught the result crossing the whole editor. Every leg now goes through
	// overlayLine, which submits the part on the chart and nothing else. Both ends off the chart is
	// still a leg worth drawing, because the line between two distant places can pass straight through
	// what you are looking at, and refusing those was the first fix's own mistake.
	//
	// AND A LEG IS A LINE ON THE SHEET, so under the bubble it is a curve — `crumbAt` answers in the
	// chart's flat coordinates and `overlayCurve` is what carries it onto the glass. Drawing the leg
	// straight between two warped ends would have been the one place on this surface where a line
	// disagreed with the ground under it.
	canvas.setColor(withAlpha(hue, 0.30f));
	for (size_t i = 1; i < trail.size(); ++i) {
		overlayCurve(canvas, crumbAt(i - 1), crumbAt(i), 0.8f);
	}
	overlayCurve(canvas, crumbAt(trail.size() - 1),
	             sheetAtWorld(selectedDrift(), selectedDriftY()), 0.8f);

	for (size_t i = 0; i < trail.size(); ++i) {
		if (!crumbIsVisible(i)) {
			continue;
		}
		// THE MARK ITSELF IS NOT A PLACE, it is a glyph a few pixels wide saying where one is — so it
		// goes on the glass at the crumb's projected position and its arms stay straight. Visibility is
		// still asked of the flat point, which is the honest question: is this country on the near face.
		const visage::Point at = Bubble::toGlass(disk(), crumbAt(i));
		const float alpha =
		    0.25f + 0.6f * static_cast<float>(i + 1) / static_cast<float>(trail.size());
		const float span = 2.6f + 2.0f * static_cast<float>(i + 1) / static_cast<float>(trail.size());
		canvas.setColor(withAlpha(hue, alpha));
		overlayLine(canvas, { at.x - span, at.y }, { at.x + span, at.y }, 1.0f);
		overlayLine(canvas, { at.x, at.y - span }, { at.x, at.y + span }, 1.0f);
	}
}

void ExploreStage::drawStar(visage::Canvas& canvas) {
	const StageDisk d = disk();
	// AT THE SOUND'S OWN IDENTITY, both coordinates: home up the chart, its own drift across it. The
	// performance term the engine adds — Weather x (puck - anchor) — is not drawn on this surface at
	// all, by design: Weather does not move things around in Explore.
	//
	// AND "IS IT ON THE CHART" IS ASKED OF THE SHEET, NOT OF THE GLASS — the bubble saturates
	// everything past the world's edge onto the rim, so the projected point of a sound in the next
	// country is ON the disk to the last decimal. Asking `contains` about it would take the off-chart
	// branch below out of service entirely and draw a full glyph on the limb: the chart saying a sound
	// is here when it is nowhere near, which is the one thing this function refuses to do.
	//
	// **THE STAR STAYS WHERE THE SOUND WAS PUT, AND A SECOND ONE SHOWS WHERE IT IS PLAYED** (2026-09-21,
	// replacing a build that moved the star itself and left a faint ring behind: a secondary, displaced
	// puck keeps the primary location visible). The place is what a drag
	// moves and what the orbit turns about, so it keeps the full glyph and the label; the played place
	// gets a smaller star in the same hue, tied back by a faint line so which sound it belongs to is
	// never a guess. ONLY WHEN THE TWO DIFFER — a sound with no Radius on it is one thing, drawn once.
	const visage::Point sheet = sheetAtWorld(selectedDrift(), selectedDriftY());
	const bool onChart = d.contains(sheet);
	const uint32_t playedHue = kSeedHues[hueIndexOf(bridge_.soundHues(), selection_.slot())];
	const AnchorPoint played = playedPlace();
	playedMarkDrawn_ = false;
	vectorDrawn_ = false;
	if (played.x != selectedDrift() || played.y != selectedDriftY()) {
		const visage::Point playedSheet = sheetAtWorld(played.x, played.y);
		// Asked of the SHEET, for the reason above; a played place off the chart draws nothing rather
		// than a second rim mark — the knobs' own marks carry it there.
		if (d.contains(playedSheet)) {
			const visage::Point mark = Bubble::toGlass(d, playedSheet);
			const float small = 4.5f * d.glyphScale();
			// **THE VECTOR, FROM THE PLACE TO WHERE IT IS PLAYED**, from the centre to the offset puck,
			// in its own colour. In the MODULATION
			// colour rather than the star's: this is the one thing on the chart that is an offset and
			// not a place, and it is the quantity the Radius and Theta rings are drawn in. It stops
			// short of the small star so the head is not under the glyph, and the head is two strokes.
			if (onChart) {
				const visage::Point from = Bubble::toGlass(d, sheet);
				const float dx = mark.x - from.x;
				const float dy = mark.y - from.y;
				const float length = std::hypot(dx, dy);
				const float gap = small * 1.4f;
				if (length > gap * 2.0f) {
					const float ux = dx / length;
					const float uy = dy / length;
					const visage::Point tip { mark.x - ux * gap, mark.y - uy * gap };
					const float head = std::min(6.0f * d.glyphScale(), length * 0.3f);
					canvas.setColor(withAlpha(tokens::kMod, 0.9f));
					canvas.segment(from.x, from.y, tip.x, tip.y, 1.5f, true);
					// The head: back along the shaft and out to each side.
					canvas.segment(tip.x, tip.y, tip.x - ux * head - uy * head * 0.5f,
					               tip.y - uy * head + ux * head * 0.5f, 1.5f, true);
					canvas.segment(tip.x, tip.y, tip.x - ux * head + uy * head * 0.5f,
					               tip.y - uy * head - ux * head * 0.5f, 1.5f, true);
					overlayInk_.add(from, tip, 1.5f + head * 0.5f);
					vectorFrom_ = from;
					vectorTip_ = tip;
					vectorDrawn_ = true;
				}
			}
			starGlyph(canvas, mark.x, mark.y, small, playedHue, 0.85f);
			overlayInk_.add(mark, small * 3.0f);
			playedMarkAt_ = mark;
			playedMarkDrawn_ = true;
		}
	}
	const visage::Point at = Bubble::toGlass(d, sheet);
	const uint32_t hue = kSeedHues[hueIndexOf(bridge_.soundHues(), selection_.slot())];
	const float glyph = 7.0f * d.glyphScale();
	const visage::Font& face = font(kKnobReadoutSize);

	char printed[64] = {};
	const SoundIdentity id = identity();
	std::snprintf(printed, sizeof(printed), "%c  z%.1f", static_cast<char>('A' + selection_.slot()),
	              id.zone);

	if (onChart) {
		// The same glyph the Perform circle draws a sound with, so one sound reads as one object in both
		// views.
		starGlyph(canvas, at.x, at.y, glyph, hue, 1.0f);
		// RECORDED WHERE IT IS DRAWN, so a case can assert the DRAWING rather than recompute it — see
		// `starDrawnAt()` for the known-bad that showed why the difference matters.
		starDrawnAt_ = at;
		starDrawnOnChart_ = true;
		overlayInk_.add(at, glyph * 3.0f);
		// The label sits to whichever side has room for it. It used to sit unconditionally to the right,
		// which puts it over the rim and off the chart for any sound in the right-hand third — the same
		// fault as the star's, one step smaller.
		const bool toTheLeft = at.x + kStarLabelGap + kStarLabelWidth > d.centre().x + d.radius();
		const float textX = toTheLeft ? at.x - kStarLabelGap - kStarLabelWidth : at.x + kStarLabelGap;
		canvas.setColor(withAlpha(tokens::kStar, 0.85f));
		canvas.text(printed, face, toTheLeft ? visage::Font::kRight : visage::Font::kLeft, textX,
		            at.y - 6.0f, kStarLabelWidth, 12.0f);
		overlayInk_.add(textX, at.y - 6.0f, kStarLabelWidth, 12.0f);
		return;
	}

	// OFF THE CHART, AND SAID SO RATHER THAN DRAWN THERE. A sound's drift is identity, not view, so
	// at a tight zoom the sound this surface is editing can sit well outside the disk — a ×40
	// screenshot has it, and its label, out over the sound cards. Drawing the glyph where the maths
	// puts it is drawing on another panel; drawing it at the rim as though it were there would be the
	// chart lying about a position, which is the one thing this file refuses to do. So a dimmer,
	// smaller mark ON the rim, in the direction the sound lies, and a label that says outright that it
	// is not on the chart. The travel readout beside it is the number.
	// THE DIRECTION IS THE SHEET'S, for the same reason: every off-chart sound projects to the same
	// saturated radius, and only the flat point still knows which way it lies.
	const float dx = sheet.x - d.centre().x;
	const float dy = sheet.y - d.centre().y;
	const float reach = std::hypot(dx, dy);
	if (!std::isfinite(reach) || reach <= 0.0f) {
		return;
	}
	const float rimX = d.centre().x + dx / reach * (d.radius() - kOffChartInset);
	const float rimY = d.centre().y + dy / reach * (d.radius() - kOffChartInset);
	// OFF CHART: the rim mark is not the star, and saying so is what stops a case reading this as the
	// star's position and quietly asserting about the wrong glyph.
	starDrawnOnChart_ = false;
	starGlyph(canvas, rimX, rimY, glyph * 0.5f, hue, 0.45f);
	overlayInk_.add({ rimX, rimY }, glyph * 1.5f);

	std::snprintf(printed + std::strlen(printed), sizeof(printed) - std::strlen(printed),
	              "  off chart");
	// Anchored back toward the centre, so the label is always the side of the mark that has chart to
	// stand on.
	const bool toTheLeft = dx > 0.0f;
	const float textX = toTheLeft ? rimX - kStarLabelGap - kStarLabelWidth : rimX + kStarLabelGap;
	canvas.setColor(withAlpha(tokens::kStar, 0.65f));
	canvas.text(printed, face, toTheLeft ? visage::Font::kRight : visage::Font::kLeft, textX,
	            rimY - 6.0f, kStarLabelWidth, 12.0f);
	overlayInk_.add(textX, rimY - 6.0f, kStarLabelWidth, 12.0f);
}

void ExploreStage::drawReadouts(visage::Canvas& canvas) const {
	const StageDisk d = disk();
	const float radius = d.radius();
	const visage::Font& face = font(kKnobReadoutSize);
	const float left = d.centre().x - radius * 0.1f;
	float y = d.centre().y - radius * 0.9f;

	char printed[64] = {};
	formatZoom(printed, sizeof(printed), zoom_);
	canvas.setColor(withAlpha(tokens::kStar, 0.70f));
	canvas.text(printed, face, visage::Font::kLeft, left, y, 140.0f, 12.0f);

	y += 13.0f;
	std::snprintf(printed, sizeof(printed), "mutation +/-%.2fz", mutationZones());
	canvas.setColor(withAlpha(tokens::kGrid, 0.55f));
	canvas.text(printed, face, visage::Font::kLeft, left, y, 180.0f, 12.0f);

	y += 13.0f;
	// THE SOUND'S OWN DRIFT, which is the one number that says why the star is not on the rail — and
	// since the GY refactor that is the whole of the answer, so it is the number that belongs here.
	//
	// It used to read the ENGINE's travel, drift plus the puck's Weather-scaled contribution. That was
	// a number nobody could act on from this surface, and worse, it changed every frame of any
	// automation: a readout re-rasterising three digits at LFO rates on a surface whose glyph atlas
	// this project has already watched corrupt under text churn. Where the puck has the sound is
	// Perform's to show.
	std::snprintf(printed, sizeof(printed), "drift %+.3f", selectedDrift());
	canvas.setColor(withAlpha(tokens::kGrid, 0.55f));
	canvas.text(printed, face, visage::Font::kLeft, left, y, 180.0f, 12.0f);
}

namespace {

/// One corner encoder, drawn the way the tray's knobs are drawn — the same face, the same column, the
/// same type — because they are the same control in a different place.
void drawEncoder(visage::Canvas& canvas, const visage::Bounds& bounds, const char* label,
                 const std::string& reading, float value, bool enabled, bool hasValue,
                 float diameter = ExploreStage::kCornerDiameter) {
	drawKnobFace(canvas, bounds.x() + bounds.width() * 0.5f, bounds.y() + diameter * 0.5f, diameter,
	             value, enabled, hasValue);

	const visage::Font& labelFace = font(kKnobLabelSize);
	const float captionWidth = trackedWidth(labelFace, label, kKnobLabelTracking);
	canvas.setColor(enabled ? kMuted : kDisabled);
	drawTracked(canvas, label, labelFace, kKnobLabelTracking,
	            bounds.x() + (bounds.width() - captionWidth) * 0.5f,
	            bounds.y() + diameter + Knob::kGap, Knob::kLabelHeight);
	canvas.setColor(enabled ? kMuted : kDisabled);
	canvas.text(reading, font(kKnobReadoutSize), visage::Font::kCenter, bounds.x(),
	            bounds.y() + diameter + Knob::kGap + Knob::kLabelHeight + Knob::kGap, bounds.width(),
	            Knob::kReadoutHeight);
}

void drawButton(visage::Canvas& canvas, const visage::Bounds& bounds, const std::string& caption,
                bool enabled, bool accent) {
	canvas.setColor(enabled ? (accent ? kBorderOn : kLine) : withAlpha(kLine, 0.5f));
	canvas.roundedRectangleBorder(bounds.x(), bounds.y(), bounds.width(), bounds.height(),
	                              kButtonRounding, 1.0f);
	const visage::Font& face = font(kControlSize);
	const float captionWidth = trackedWidth(face, caption, kButtonTracking);
	canvas.setColor(enabled ? (accent ? kAccent : kInk) : kDisabled);
	drawTracked(canvas, caption, face, kButtonTracking,
	            bounds.x() + (bounds.width() - captionWidth) * 0.5f,
	            bounds.y() + (bounds.height() - kControlSize) * 0.5f - 1.0f, kControlSize + 2.0f);
}

/// The heart's button: the row's border, and a heart — in the likes' colour when the star stands on a like, in the muted
/// ink when it does not.
void drawLikeButton(visage::Canvas& canvas, const visage::Bounds& bounds, bool liked) {
	canvas.setColor(liked ? kLikeHue : kLine);
	canvas.roundedRectangleBorder(bounds.x(), bounds.y(), bounds.width(), bounds.height(), kButtonRounding, 1.0f);
	canvas.setColor(liked ? kLikeHue : kMuted);
	drawHeart(canvas, bounds.x() + bounds.width() * 0.5f, bounds.y() + bounds.height() * 0.5f, bounds.height() * 0.5f);
}

} // namespace

void ExploreStage::drawCorners(visage::Canvas& canvas) {
	// GAMMA, top left, and it is REAL: the selected sound's own gamma, which is a shift along the home
	// axis — §7.1b's "zone gives you a neighbourhood, gamma gives you a country". The mock puts a
	// gamma in this corner and this is the one that belongs to what Explore edits.
	//
	// THE READOUT IS THE WHOLE OF IT — the base the corner carries PLUS the vernier parameter beside
	// it — because what the number has to answer is "where is this sound along its line", and that is
	// the sum. A readout of the base alone would sit still while a host automated the vernier under
	// it, and a readout of the vernier alone is the one that used to stop at 4 while the sound went
	// on moving. The face is a compass for the same reason the rotate face is: an unbounded travel has
	// no end to point at.
	//
	// AND IT MOVES UNDER A CHART DRAG NOW, which is the decision this surface overturned. The comment
	// that used to sit in homeToCoord — "a scrub that quietly moved it would make that encoder's readout
	// a lie" — was right that a silently-moving number is the danger and wrong that holding gamma still
	// was the cure. Under the one-value design every vertical drag moves this number, constantly and
	// visibly, so the readout is not a lie; it is the drag's own feedback. What it costs is that the
	// reading has to be PROVABLY on the glass rather than merely computed, hence the recording below.
	const double gammaTotal =
	    bridge_.value(paramOf(SoundField::Gamma)) + selectedGammaBase();
	std::snprintf(gammaReading_, sizeof(gammaReading_), "%+.2f", gammaTotal);
	gammaDial_ = gammaDialPosition(gammaTotal);
	drawEncoder(canvas, partBounds(Part::Gamma), "Gamma", gammaReading_, gammaDial_, true, true);

	// ZOOM, top right. Not a parameter and never will be — it is a view scale that happens also to be
	// the mutation scale, which is a UI decision rather than something a patch stores.
	char zoomText[24] = {};
	formatZoom(zoomText, sizeof(zoomText), zoom_);
	const float zoomValue =
	    static_cast<float>(std::log(zoom_ / minZoom()) / std::log(kMaxZoom / minZoom()));
	drawEncoder(canvas, partBounds(Part::Zoom), "Zoom", zoomText, zoomValue, true, true);

	// ROTATE and DRIFT, drawn and labelled and switched off. Their engine API is being built; a corner
	// that answered the mouse before it could reach the engine would be the honesty rule's own
	// counter-example.
	// ROTATE, bottom left, and live: the selected sound's own plane, re-aimed. Its readout is in
	// degrees because that is what a turn is, and it reads the plane the sound is ACTUALLY on — its own
	// pin, the global one, or the plane its home gives it — so an unpinned sound shows where it already
	// points instead of a stale zero. In DEGREES OF THE STORED TURN (`rotateReadout`): this printed
	// `radians * 180 / kRotateSpan`, which was degrees only while the gearing was pi — backlog GF cut it to
	// pi/4 and the text read four times the turn ("+360 deg" for a quarter), the slip `rotateDialPosition`
	// below was already corrected for.
	const std::string rotateText = rotateReadout();
	const float rotateValue = rotateDialPosition(selectedRotation());
	drawEncoder(canvas, partBounds(Part::Rotate), "Rotate", rotateText.c_str(), rotateValue, kRotateLive, true);

	// RADIUS AND THETA ARE NOT DRAWN HERE ANY MORE — they are child `Knob`s over a `TravelControl`
	// (see `radiusKnobs_`), which is what lets them glow, take a dropped source and show a ring like
	// every other destination on this surface. Drawing them here was the defect.

	drawLikeButton(canvas, partBounds(Part::Like), likeUnderStar().has_value());
	drawButton(canvas, partBounds(Part::Mutate), "MUTATE", true, true);
	drawButton(canvas, partBounds(Part::Undo), "UNDO", canUndo(), false);
	drawButton(canvas, partBounds(Part::Redo), "REDO", canRedo(), false);

	// WHOSE WIDTH THE KNOB IS — the selected sound's letter in its own seed hue (V15), above the
	// face. The knob draws itself as a child; this chip is the stage's because the hue and the
	// letter belong to the selection, which the stage owns and the stock Knob deliberately does
	// not. Unconditional since V16: the column is always on stage.
	const visage::Bounds widthAt = widthKnobBounds();
	// Unconditional since V16 (the column is always on stage), through the hue the sound is actually
	// called by (the hues arc): the letter keeps the slot, the colour follows the remap.
	const char letter[2] = { static_cast<char>('A' + selection_.slot()), '\0' };
	canvas.setColor(kSeedHues[hueIndexOf(bridge_.soundHues(), selection_.slot())]);
	canvas.text(letter, font(kKnobLabelSize), visage::Font::kCenter, widthAt.x(),
	            widthAt.y() - Knob::kLabelHeight - Knob::kGap, widthAt.width(), Knob::kLabelHeight);
}

bool ExploreStage::drumKitShown() const {
	return oscTypeFromValue(bridge_.value(static_cast<clap_id>(ParamId::OscType))) == OscType::Drum
	       && steppedIndex(bridge_.value(static_cast<clap_id>(ParamId::DrumMode)), kDrumModeNames.size())
	              == static_cast<size_t>(DrumMode::Kit);
}

bool ExploreStage::padInView(size_t slot) const {
	// THE VIEW AS STORED (not `viewCentre()`, which answers for the new selection once it differs), and the pad's star where
	// its own chart will draw it: at its travel. Inside the disk and clear of its edge by kPadJumpMargin of the radius.
	const AnchorPoint at = bridge_.soundTravel().offset[std::min(slot, kMaxSounds - 1)];
	const double dx = (at.x - travelCentre_) / travelSpan();
	const double dy = (at.y - viewCentre_) / (homeSpanZones() / 1023.0);
	return std::hypot(dx, dy) <= 0.5 * (1.0 - kPadJumpMargin);
}

void ExploreStage::followSelection() {
	// THE SELECTION JUMP: in a Kit, a pad already in view leaves the view exactly where it is (no
	// auto-zoom, nothing glides); one off it or near its edge makes the view JUMP onto it, with no animation. Every other
	// engine, and the first frame, centre as they always have.
	if (everCentred_ && drumKitShown() && padInView(selection_.slot())) {
		settleOn(viewCentre_, travelCentre_);
		return;
	}
	recentre();
}

void ExploreStage::drawPadMarks(visage::Canvas& canvas) {
	padMarkCount_ = 0;
	if (!drumKitShown()) {
		return;
	}
	const size_t selected = std::min(selection_.slot(), kMaxSounds - 1);
	const TerrainWindow here = terrainWindowNow();
	const StageDisk d = disk();
	const auto kindOf = [this](size_t slot) {
		return steppedIndex(bridge_.value(static_cast<clap_id>(drumParam(slot, DrumControl::Kind))), kDrumKindNames.size());
	};
	const size_t kind = kindOf(selected);
	const double walkX = bridge_.value(static_cast<clap_id>(ParamId::DrumWalkX));
	const double walkY = bridge_.value(static_cast<clap_id>(ParamId::DrumWalkY));
	const SoundTravel travel = bridge_.soundTravel();
	const visage::Font& face = font(kKnobReadoutSize);
	for (size_t slot = 0; slot < kMaxSounds; ++slot) {
		if (slot == selected) {
			continue; // the star is the selected pad's mark, and its played mark carries the Walk
		}
		// ITS PLACE AND WHERE IT PLAYS, each decoded on its own plane and projected onto this one (the likes' road).
		const TerrainWindow own = terrainWindowFor(slot);
		const AnchorPoint at = travel.offset[slot];
		const auto onHere = [&](double x, double y) {
			std::array<double, kTerrainLikePoint> point {};
			const size_t n = terrainPlacePoint(own, x, y, point);
			return terrainPlaceOn(here, std::span<const double>(point.data(), n), x, y);
		};
		const PlanePlace place = onHere(at.x, at.y);
		const visage::Point flat = sheetAtWorld(place.x, place.y);
		if (!std::isfinite(flat.x) || !std::isfinite(flat.y) || !d.contains(flat)) {
			continue;
		}
		PadMark& mark = padMarks_[padMarkCount_++];
		mark = PadMark {};
		mark.slot = slot;
		mark.at = Bubble::toGlass(d, flat);
		mark.hollow = kindOf(slot) != kind;
		const uint32_t hue = kSeedHues[hueIndexOf(bridge_.soundHues(), slot)];
		const float r = 4.5f * d.glyphScale();
		// THE WALK, a faint arrow from the place to where the pad plays.
		if (walkX != 0.0 || walkY != 0.0) {
			const PlanePlace played = onHere(at.x + walkX, at.y + walkY);
			const visage::Point pf = sheetAtWorld(played.x, played.y);
			if (std::isfinite(pf.x) && std::isfinite(pf.y) && d.contains(pf)) {
				const visage::Point tip = Bubble::toGlass(d, pf);
				canvas.setColor(withAlpha(tokens::kMod, 0.45f));
				canvas.segment(mark.at.x, mark.at.y, tip.x, tip.y, 1.0f, true);
				canvas.circle(tip.x - 1.5f, tip.y - 1.5f, 3.0f);
				mark.arrow = true;
				mark.tip = tip;
			}
		}
		if (mark.hollow) {
			canvas.setColor(withAlpha(hue, 0.85f));
			canvas.ring(mark.at.x - r, mark.at.y - r, 2.0f * r, 1.5f);
		}
		else {
			canvas.setColor(withAlpha(hue, 0.9f));
			canvas.circle(mark.at.x - r, mark.at.y - r, 2.0f * r);
		}
		// ITS LETTER, and on a hollow mark its kind: the colour under it means nothing for that kind, so the label says what
		// it is instead of a tooltip the stage has no road for.
		char text[24] = {};
		if (mark.hollow) {
			std::snprintf(text, sizeof(text), "%c %s", static_cast<char>('A' + slot), kDrumKindNames[kindOf(slot)]);
		}
		else {
			std::snprintf(text, sizeof(text), "%c", static_cast<char>('A' + slot));
		}
		canvas.setColor(withAlpha(tokens::kStar, mark.hollow ? 0.6f : 0.8f));
		canvas.text(text, face, visage::Font::kLeft, mark.at.x + r + 3.0f, mark.at.y - 7.0f, 90.0f, 14.0f);
		overlayInk_.add(mark.at, r * 3.0f);
	}
}

void ExploreStage::drawLikes(visage::Canvas& canvas) {
	likeMarksDrawn_ = 0;
	const std::vector<TerrainLike>& likes = bridge_.terrainLikes().likes;
	if (likes.empty()) {
		return;
	}
	// EVERY LIKE AT ITS STORED POINT'S PROJECTION onto the plane being drawn (a mark that stayed put
	// under a rotate named a place that no longer sounded like the like) — wherever that lands, NO FADE:
	// a gamma move pans it off by the plane's alignment with the home, a turn may move it anywhere, and off the chart it is clipped.
	const TerrainWindow window = terrainWindowNow();
	const StageDisk d = disk();
	for (const TerrainLike& like : likes) {
		if (!likeApplies(like)) {
			continue;
		}
		const PlanePlace place = terrainLikePlaceOn(window, like);
		const visage::Point flat = sheetAtWorld(place.x, place.y);
		if (!std::isfinite(flat.x) || !std::isfinite(flat.y) || !d.contains(flat)) {
			continue;
		}
		// A GLYPH, NOT A PLACE — on the glass at the like's projected position, upright, as the crumbs' marks are.
		const visage::Point at = Bubble::toGlass(d, flat);
		canvas.setColor(withAlpha(kLikeHue, 0.92f));
		drawHeart(canvas, at.x, at.y, kLikeMarkSize);
		++likeMarksDrawn_;
	}
}

void ExploreStage::draw(visage::Canvas& canvas) {
	++stagePaints_;
	// THE LIKES' PHASE WARPS, every sound's, kept current with its likes.
	refreshLikeWarps();
	// AND THE LIKE-SET LOG: a snapshot if the set is new to it, and the walk once a stance settles (likes_log.h).
	logTick();
	// A new selection is a new chart, so the view follows it — here rather than through a callback out
	// of the selection, because a redraw is exactly what a selection change already causes.
	if (!everCentred_ || centredOnSlot_ != selection_.slot()) {
		followSelection();
	}
	// Every reason the map could be out of date is caught here at once — a zoom, a gamma, an engine
	// change, a new selection — because request() is idempotent and cheap: it compares a window and
	// returns.
	//
	// ASKED EVERY FRAME, GESTURE OR NOT — ONE RULE FOR THE WHOLE SURFACE. It used to be gated on no
	// gesture being in flight, because a request per frame used to ABANDON the build in progress and
	// the map never finished. The builder coalesces now: it runs each map to completion and then goes
	// again at wherever the view is by then, so asking every frame costs one rebuild per build rather
	// than a rebuild per frame.
	//
	// That gate was a workaround, and it had a cost: rotate only redrew the terrain when the hand
	// let go of the knob. The same was true of gamma and zoom, so all three get live feedback from
	// removing it rather than rotate getting a special case — which is the rule the surface should
	// have had.
	const TerrainWindow asked = askForTerrain();

	const StageDisk d = disk();
	// THE GROUND GOES WHERE THE BUBBLE CAN REACH IT. With a `TerrainGround` in front of this frame the
	// picture is already on the glass, bent, by the time this runs; without one — every widget case,
	// which has no frame tree — this paints it itself, unbent, so the surface is never blank.
	if (ground_ == nullptr) {
		drawGround(canvas);
	}

	drawTerrain(canvas);
	drawGrid(canvas);

	// THE OVERLAY IS CLIPPED TWICE, and the two clips answer different questions. The arithmetic in
	// overlayLine decides what is worth submitting — a leg trimmed to the chart is a few hundred
	// pixels of geometry instead of tens of thousands, and it is one quad either way in a batch with a
	// hard ceiling. The clamp here is what makes the guarantee unconditional: a mark this file adds
	// later and forgets to clip cannot reach the rest of the editor, it can only be missing from the
	// chart, which is the failure a person can see and report.
	overlayInk_ = InkBox {};
	const InkBox chart = chartBox();
	canvas.saveState();
	canvas.trimClampBounds(chart.left, chart.top, chart.right - chart.left,
	                       chart.bottom - chart.top);
	drawLikes(canvas);
	drawPadMarks(canvas);
	drawCrumbs(canvas);
	drawStar(canvas);
	canvas.restoreState();

	drawReadouts(canvas);

	// The rim, which is the same hairline Perform's circle carries.
	canvas.setColor(kLine);
	canvas.ring(d.centre().x - d.radius(), d.centre().y - d.radius(), d.radius() * 2.0f, 1.5f);

	drawCorners(canvas);

	// CLEAR OF THE COLLAPSE TOGGLE in the top-right corner (backlog NF, `StageRail::captionLeft`); a stage too
	// narrow for the whole line beside it says just its noun.
	const visage::Font& label = font(kBylineSize);
	const std::string* caption = &stageCaption();
	float captionX = StageRail::captionLeft(width(), trackedWidth(label, *caption, kStageLabelTracking));
	if (captionX < StageRail::kCaptionMinLeft) {
		static const std::string noun = "TERRAIN";
		caption = &noun;
		captionX = StageRail::captionLeft(width(), trackedWidth(label, noun, kStageLabelTracking));
	}
	canvas.setColor(kDisabled);
	drawTracked(canvas, *caption, label, kStageLabelTracking, captionX, kStageLabelTop, 12.0f);

	// KEEP THE PAINT LOOP ALIVE UNTIL THE MAP ON SCREEN IS THE MAP THIS VIEW ASKED FOR.
	// `askForTerrain()` above put the build request in flight, but the snapshot arrives
	// asynchronously and the view stops painting the moment this frame leaves the stale list.
	// Without a follow-up redraw the worker's answer is never painted, and `update()` — which
	// is the thing that notices a new snapshot — only runs as often as the editor is ticked.
	//
	// THE CONDITION IS "NOT YET THE WINDOW I ASKED FOR", NOT "NO MAP AT ALL". The first spelling
	// of this guard was `terrainQuadCount() == 0`, which is true only before the FIRST snapshot
	// ever lands. It fixed blank-on-load and left the next reported case untouched: after a
	// pan or a zoom there is still a map on screen, so the count is one, so nothing re-armed and
	// the newly revealed ground waited for the next mouse event. A snapshot carries the window it
	// was built for, so the honest question is whether that window is this one.
	//
	// `workerStarted()` is the guard against spinning forever should the worker never come up;
	// `request()` coalesces, so asking every frame while one is in flight costs a comparison.
	drawWarpModeBox(canvas);
	drawModelBox(canvas);

	if (terrain_.workerStarted() && !(terrain_.snapshot()->window == asked)) {
		repaint();
	}
}

/// THE MODE BOXES — a name, a chevron, and a border. A box is not a Knob and does not pretend to be:
/// a mode is a choice, and drawing it as a dial would say a sweep exists where a menu does.
///
/// NOT DRAWN WHERE IT IS DEAD, which overturns this box's old "greyed rather than
/// hidden": only switching the engine could make it live, so a greyed box is a control with no way back
/// on this face. The slot table says which box, if any, stands in each box slot on this engine (the warp mode on V5
/// and Basic Shapes, the bow mode on Resonate, the additive Shape on Additive; the Resonate model under the right
/// flank), and the hit test asks the same table.
bool ExploreStage::drawSlotBox(visage::Canvas& canvas, Slot slot) {
	const visage::Bounds box = slotBounds(slot);
	if (box.width() <= 1.0f) {
		return false;
	}
	const SurroundControl control = currentSlotRow()[static_cast<size_t>(slot)];
	if (control == SurroundControl::None) {
		return false;
	}
	canvas.setColor(withAlpha(kSurfaceRaised, 0.9f));
	canvas.roundedRectangle(box.x(), box.y(), box.width(), box.height(), 3.0f);
	canvas.setColor(withAlpha(kLine, 0.6f));
	canvas.roundedRectangleBorder(box.x(), box.y(), box.width(), box.height(), 3.0f, 1.0f);

	canvas.setColor(kInk);
	canvas.text(boxLabel(control), font(kKnobLabelSize), visage::Font::kLeft, box.x() + 6.0f, box.y(),
	            box.width() - 18.0f, box.height());

	// THE CHEVRON, as geometry rather than a glyph — the standing rule in this repo: the font has no
	// guaranteed arrow and a missing glyph is an invisible affordance.
	const float cx = box.right() - 9.0f;
	const float cy = box.y() + box.height() * 0.5f;
	canvas.setColor(kMuted);
	canvas.segment(cx - 3.5f, cy - 1.5f, cx, cy + 2.0f, 1.3f, true);
	canvas.segment(cx, cy + 2.0f, cx + 3.5f, cy - 1.5f, 1.3f, true);
	return true;
}

/// The box under the left stack's Warp slot: the warp, bow or additive-shape mode.
bool ExploreStage::drawWarpModeBox(visage::Canvas& canvas) {
	return drawSlotBox(canvas, Slot::LeftBox);
}

/// The box under the right stack: the Resonate model, and on no other engine (by design: the model is what
/// Resonate's string is, so a greyed box would be a control on the other engines' face that belongs to none of them).
bool ExploreStage::drawModelBox(visage::Canvas& canvas) {
	return drawSlotBox(canvas, Slot::RightBox);
}
} // namespace feathers::ui
