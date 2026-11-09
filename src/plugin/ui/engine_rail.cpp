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

#include "plugin/ui/widget_registry.h"
#include "plugin/ui/param_knob.h"
#include "plugin/ui/engine_rail.h"

#include "plugin/ui/param_edit.h"
#include "plugin/ui/theme.h"
#include "plugin/ui/value_box.h"

#include <algorithm>
#include <cmath>

namespace feathers::ui {

namespace {

constexpr float kPad = 12.0f;
constexpr float kRowHeight = 24.0f;
constexpr float kRowGap = 6.0f;
constexpr float kSectionGap = 11.0f; // was 18 before the engine box took the VOICE caption's row and a half more (QH)
constexpr float kLabelHeight = 16.0f;
/// THE PITCH KNOBS' FACE, AS LIST ROWS (2026-10-03, when Coarse joined): four knobs — Coarse and Fine, then Glide
/// and Phase — in two rows of two, each face with its caption and reading beside it. A column knob's row could not
/// take a fourth (a quarter of the rail cut every caption and ran Coarse's reading into Fine's, photographed), and
/// the rail had no height for a second column row (4 px spare, measured); two list rows at the text's own height
/// are shorter than the one column row they replace. The face is a little under the text's 26 px so the readings
/// keep the width they need beside it.
constexpr float kPitchRowDiameter = 22.0f;

const ParamDesc& descOf(ParamId id) {
	// Every id here is a declared parameter, so this cannot fail; if the table were ever edited out
	// from under it, a null dereference at construction is a better failure than a silent dead panel.
	return *findParam(static_cast<clap_id>(id));
}

} // namespace

EngineRail::EngineRail(ParamBridge& bridge, WidgetRegistry& registry, Notifier* notifier)
    : Rail("ENGINE", Rail::Side::Left), bridge_(bridge), host_(bridge), notifier_(notifier) {
	// THE PANEL'S OWN VERBS: every other control here is a child frame that answers for itself, so this handler
	// exists so the rail can be closed and reopened, and (step 3) so the section under Voice can turn its page.
	onMouseDown() = [this](const visage::MouseEvent& e) {
		if (pressCollapse(e.position)) {
			return;
		}
		// AND THE PAGE NAMES in the heading under Voice (step 3), which are the panel's, not a child's.
		if (!collapsed()) {
			for (const Page page : { Page::Pitch, Page::Stack }) {
				if (pageTabBounds(page).contains(e.position)) {
					showPage(page);
				}
			}
		}
	};
	// The panel itself watches the voice mode, because the mode decides what else is available.
	watch_ = registry.watchToken(static_cast<clap_id>(ParamId::VoiceMode), this);
	stackWatch_ = registry.watchToken(static_cast<clap_id>(ParamId::StackCount), this);
	oscTypeWatch_ = registry.watchToken(static_cast<clap_id>(ParamId::OscType), this);
	drumModeWatch_ = registry.watchToken(static_cast<clap_id>(ParamId::DrumMode), this); // a Kit hides the voice controls
	// THE TOOLTIP LAYER, over the whole rail (step 5) — `FxCard`'s arrangement.
	addChild(&tips_);
	tips_.watch(*this);

	voiceMode_ = std::make_unique<SegmentedControl>(bridge, registry, descOf(ParamId::VoiceMode),
	                                                kVoiceModeNames, notifier);
	voiceCount_ =
	    std::make_unique<Stepper>(bridge, registry, descOf(ParamId::VoiceCount), 1.0, notifier);
	notePriority_ = std::make_unique<SegmentedControl>(bridge, registry, descOf(ParamId::NotePriority),
	                                                   kNotePriorityNames, notifier);
	oscMode_ = std::make_unique<SegmentedControl>(bridge, registry, descOf(ParamId::OscMode),
	                                              kOscModeNames, notifier);
	transposeOct_ =
	    std::make_unique<Stepper>(bridge, registry, descOf(ParamId::TransposeOct), 1.0, notifier);
	transposeSemi_ =
	    std::make_unique<Stepper>(bridge, registry, descOf(ParamId::TransposeSemi), 1.0, notifier);
	bendRange_ =
	    std::make_unique<Stepper>(bridge, registry, descOf(ParamId::BendRange), 1.0, notifier);

	// The pitch knobs, as list rows at the text's height (`kPitchRowDiameter` says why).
	glide_ = paramKnob(bridge, registry, &descOf(ParamId::GlideTime), "Glide", notifier);
	fine_ = paramKnob(bridge, registry, &descOf(ParamId::FineTune), "Fine", notifier);
	coarse_ = paramKnob(bridge, registry, &descOf(ParamId::Coarse), "Coarse", notifier);
	// PHASE AND PHASE WIDTH (in the Engine rail, by Osc Mode): Phase the pitch knobs' fourth,
	// Phase Width a box beside Bend (see the layout for why).
	// PHASE IS A STEREO DESTINATION ON EVERY ENGINE (backlog OZ, 2026-10-29): on Additive the right ear reads its own
	// rotators while the ears' phases differ (PartialBank::setEarPhase), so a stereo route's ear difference is heard there
	// as on the others. (From 2026-10-10 to OZ, backlog LY, the two ears shared one rotator and the glow was withheld.)
	phase_ = paramKnob(bridge, registry, &descOf(ParamId::OscPhase), "Phase", notifier);
	for (Knob* knob : { glide_.get(), coarse_.get(), fine_.get(), phase_.get() }) {
		knob->setFaceDiameter(kPitchRowDiameter);
		knob->setLabelPlacement(Knob::LabelPlacement::Right);
	}
	engineControl_ = std::make_unique<ParamControl>(bridge, &descOf(ParamId::OscType));
	engineBox_ = std::make_unique<ValueBox>(*engineControl_, "");
	stackIndexWatches_.push_back(registry.watchToken(static_cast<clap_id>(ParamId::OscType), engineBox_.get()));
	phaseWidthControl_ = std::make_unique<ParamControl>(bridge, &descOf(ParamId::OscPhaseWidth));
	phaseWidth_ = std::make_unique<ValueBox>(*phaseWidthControl_, "P.Width");
	// THE STACK (backlog KE): how many, how far apart in pitch, how wide in stereo — one row of three knobs.
	stackCount_ = paramKnob(bridge, registry, &descOf(ParamId::StackCount), "Stack", notifier);
	stackCount_->setFaceDiameter(Knob::kEngineRailDiameter);
	stackDetune_ = paramKnob(bridge, registry, &descOf(ParamId::StackDetune), "Detune", notifier);
	stackDetune_->setFaceDiameter(Knob::kEngineRailDiameter);
	stackSpread_ = paramKnob(bridge, registry, &descOf(ParamId::StackSpread), "Spread", notifier);
	stackSpread_->setFaceDiameter(Knob::kEngineRailDiameter);

	// THE STACK INDEX'S ZONE KNOBS (step 3; voice-stacking doc §3.8): a mode and the amount that mode reads, in the
	// Groove cell's shape (`ModeValueCell`), over the core parameters through `ParamControl` — the adapter the doc
	// named as the risk, and the host's own control already is one. Shape's box is its Amount in every mode but
	// Free, where it is Free's phase; Mapping's box is its Amount, and As shaped has none.
	auto names = [](std::span<const char* const> labels) {
		std::vector<ZoneSelect::ZoneName> out;
		for (const char* label : labels) {
			out.push_back({ label, {} });
		}
		return out;
	};
	stackShapeMode_ = std::make_unique<ParamControl>(bridge, &descOf(ParamId::StackShape));
	stackShapeAmount_ = std::make_unique<ParamControl>(bridge, &descOf(ParamId::StackShapeAmount));
	stackFreePhase_ = std::make_unique<ParamControl>(bridge, &descOf(ParamId::StackFreePhase));
	stackMapMode_ = std::make_unique<ParamControl>(bridge, &descOf(ParamId::StackMap));
	stackMapAmount_ = std::make_unique<ParamControl>(bridge, &descOf(ParamId::StackMapAmount));
	const auto shapeIs = [&bridge](StackShape shape) {
		return steppedIndex(bridge.value(static_cast<clap_id>(ParamId::StackShape)), kStackShapeNames.size())
		       == static_cast<size_t>(shape);
	};
	stackShape_ = std::make_unique<ModeValueCell>(
	    *stackShapeMode_, "Shape", names(kStackShapeNames), [shapeIs](size_t param) {
		    return param == 0 ? !shapeIs(StackShape::Free) : shapeIs(StackShape::Free);
	    });
	ValueBox& shapeAmountBox = stackShape_->addFollower(0, std::make_unique<ValueBox>(*stackShapeAmount_, ""));
	ValueBox& freePhaseBox = stackShape_->addFollower(1, std::make_unique<ValueBox>(*stackFreePhase_, ""));
	stackMap_ = std::make_unique<ModeValueCell>(
	    *stackMapMode_, "Mapping", names(kStackMapNames), [&bridge](size_t) {
		    return steppedIndex(bridge.value(static_cast<clap_id>(ParamId::StackMap)), kStackMapNames.size())
		           != static_cast<size_t>(StackMap::AsShaped);
	    });
	ValueBox& mapAmountBox = stackMap_->addFollower(0, std::make_unique<ValueBox>(*stackMapAmount_, ""));
	stackShape_->setFaceDiameter(Knob::kEngineRailDiameter);
	stackMap_->setFaceDiameter(Knob::kEngineRailDiameter);
	stackCurve_ = paramKnob(bridge, registry, &descOf(ParamId::StackCurve), "Curve", notifier);
	stackCurve_->setFaceDiameter(Knob::kEngineRailDiameter);
	stackPlot_ = std::make_unique<StackIndexPlot>(bridge, registry);
	// EACH PARAMETER WATCHED BY THE CONTROL THAT SHOWS IT, as `paramKnob` does for a knob: the registry is what a
	// host's automation redraws through, and what "every declared parameter has a control" asks.
	stackIndexWatches_.push_back(registry.watchToken(static_cast<clap_id>(ParamId::StackShape), stackShape_.get()));
	stackIndexWatches_.push_back(registry.watchToken(static_cast<clap_id>(ParamId::StackMap), stackMap_.get()));
	stackIndexWatches_.push_back(registry.watchToken(static_cast<clap_id>(ParamId::StackShapeAmount), &shapeAmountBox));
	stackIndexWatches_.push_back(registry.watchToken(static_cast<clap_id>(ParamId::StackFreePhase), &freePhaseBox));
	stackIndexWatches_.push_back(registry.watchToken(static_cast<clap_id>(ParamId::StackMapAmount), &mapAmountBox));
	stackIndexWatches_.push_back(registry.watchToken(static_cast<clap_id>(ParamId::OscPhaseWidth), phaseWidth_.get()));
	// And the panel, for the mode choices: a Shape or Mapping change decides which box each cell shows.
	stackIndexWatches_.push_back(registry.watchToken(static_cast<clap_id>(ParamId::StackShape), this));
	stackIndexWatches_.push_back(registry.watchToken(static_cast<clap_id>(ParamId::StackMap), this));

	// The mock stacks the Engine rail's segments and puts the noun in the bend row, where "2 st" alone
	// would read as another transpose.
	// VOICE MODE LIES FLAT NOW (backlog KE step 2, provisional — voice-stacking doc §7, assumption 16). It was
	// stacked as the section's heading act; the stack's knob row needed the height, and Poly/Mono/Legato are three
	// short words that fit across exactly as Last/Low/High do.
	voiceMode_->setVertical(false);
	// NOTE PRIORITY LINES UP, and it is the row that pays for Osc Mode. Two arcs added to this rail
	// independently — the fx lane's source grid and this branch's retrigger row — and each fit on its
	// own while together they overflowed the rail by 26 px, which is what "every Engine rail section
	// fits inside the rail it is drawn in" exists to catch. Laying these three short words on one row
	// recovers 48 px, which absorbs both and leaves headroom rather than landing exactly on the line.
	// Same reasoning as Osc Mode's row directly below: three options fit across when their names are
	// short, and Last/Low/High are shorter than Digi/Analog/Catchup. (Voice Mode stayed stacked, as the
	// section's heading act, until the stack's knob row needed its height — see above.)
	notePriority_->setVertical(false);
	// ONE FULL-WIDTH ROW OF THREE, deliberately NOT the stack its two neighbours use — and the
	// reason is height rather than taste. Stacking was tried first, for consistency with Voice Mode
	// and Note Priority, and it cost two extra rows that pushed the routing section down the rail
	// far enough to break its own tests: the rail carries the modulation routes underneath, so
	// vertical space here is spent on something. Bitwig's Poly Grid Engine rail — the reference
	// layout — gives its three-option Retrigger control exactly this shape, one full-width row
	// of three, for what is presumably the same reason. Short names are the price and they pay it:
	// the row is bounded by the rail's width, and "Phase Catchup" would not fit beside two others.

	bendRange_->setRowLabel("bend");

	addChild(engineBox_.get());
	addChild(voiceMode_.get());
	addChild(voiceCount_.get());
	addChild(stackCount_.get());
	addChild(notePriority_.get());
	addChild(oscMode_.get());
	addChild(transposeOct_.get());
	addChild(transposeSemi_.get());
	addChild(bendRange_.get());
	addChild(glide_.get());
	addChild(coarse_.get());
	addChild(fine_.get());
	addChild(phase_.get());
	addChild(phaseWidth_.get());
	addChild(stackDetune_.get());
	addChild(stackSpread_.get());
	addChild(stackShape_.get());
	addChild(stackMap_.get());
	addChild(stackCurve_.get());
	addChild(stackPlot_.get());

	refreshEnablement();
}

void EngineRail::refreshEnablement() {
	// The parameter table's own stepped rule. A rounding of our own would be a fourth copy of it, and
	// params.h says by name what that costs: a control that decides for itself which option is
	// selected can disagree with the engine about which one is playing.
	const auto mode = static_cast<VoiceMode>(
	    steppedIndex(bridge_.value(static_cast<clap_id>(ParamId::VoiceMode)), kVoiceModeNames.size()));
	const bool poly = mode == VoiceMode::Poly;
	// PHASE MOVES THE OSCILLATOR'S READ, and Resonate reads a string with no cycle to move (OscVoice skips the offset
	// on a Weave side), so it greys there — the honesty rule, read from the engine's own skip.
	// AND PLUCK READS NONE EITHER: its string has no cycle to read at an offset (the String road never reads the phase).
	const OscType engine = oscTypeFromValue(bridge_.value(static_cast<clap_id>(ParamId::OscType)));
	// AND NOR DOES DRUM: a hit is struck, not read from a cycle (dsp/drum_voice.h).
	const bool phaseReads = engine != OscType::Resonate && engine != OscType::Pluck && engine != OscType::Drum;
	phase_->setEnabled(phaseReads);
	// PHASE WIDTH MIRRORS WHAT THE ROUTES ADD (Synth's `phaseSpreadLive_`): with nothing routed into Phase there is
	// nothing to mirror, and it changes nothing.
	phaseWidth_->setEnabled(phaseReads && phaseRouted());
	// AND ON ADDITIVE OSC MODE IS NOT DRAWN (2026-10-10, backlog LY; only an engine switch would wake
	// it, so hidden, not greyed): a note-on resets the bank to its spectrum-weighted phases whatever the mode, and legato
	// or a retrigger keeps them running (OscVoice::partialsPrimed_), so Digi, Analog and Catchup all sound the same.
	// Phase and Phase Width are live there: the bank turns each partial by its ratio times the offset, and since backlog
	// OZ (2026-10-29) the right ear by its own (PartialBank::setEarPhase).
	if (!collapsed()) {
		applyEngineVisibility();
	}
	// A voice count is meaningless to a monophonic synth, so it greys out there.
	voiceCount_->setEnabled(poly);
	// AND IT IS HELD UNDER THE STACK'S CPU GUARD (backlog KE): a stack of N sounds at most ⌊B / N⌋ notes, so the
	// counts above that are the same sound and the stepper stops there. The ceiling comes from the one function
	// the engine hands the voice manager, `stackVoiceLimit`, asked for the highest count there is.
	const int stack = static_cast<int>(
	    std::clamp(std::lround(bridge_.value(static_cast<clap_id>(ParamId::StackCount))), 1L,
	               static_cast<long>(kMaxStack)));
	voiceCount_->setCeiling(static_cast<double>(stackVoiceLimit(kMaxVoices, stack)));
	// A FAN OF ONE PART IS NO FAN: with Stack at 1 the engine seats one oscillator at fan position 0, where the
	// detune multiplies nothing (`Synth::partTargetFor`), so the knob changes no sound and greys.
	stackDetune_->setEnabled(stack > 1);
	// And Spread for the same reason: one part sits at fan 0, where its pan is 0 whatever Spread says.
	stackSpread_->setEnabled(stack > 1);
	// THE STACK INDEX'S CONTROLS ARE LIVE BEFORE ANY ROUTE CARRIES THE INDEX (backlog MO: "Stack
	// mapping controls are confusingly inactive with no mappings. they shouldnt be greyed out with no mappings."). The
	// synth reads the index only through the amounts routed from it (`Synth::partTargetFor`, `renderStackParts`), so
	// with no route they change no sound YET — but a route is one arrow away, beside them, and a control that will act
	// the moment its own page's arrow is used is set up first, not dead. Step 3 greyed them until a route existed, and
	// that read as broken. What still greys them is what no route can cure from here: a stack of one has one part, at
	// index 0, which is the same reading that greys Detune, Spread and the arrow itself.
	const bool canAct = stack > 1;
	stackShape_->setEnabled(canAct);
	stackMap_->setEnabled(canAct);
	stackCurve_->setEnabled(canAct);
	stackPlot_->setEnabled(canAct);
	// Which box each zone knob shows follows its mode, which automation can move as well as a hand.
	const bool shapeMoved = stackShape_->refresh();
	const bool mapMoved = stackMap_->refresh();
	if (shapeMoved || mapMoved) {
		stackShape_->resized();
		stackMap_->resized();
	}
	// AND THE STACK INDEX'S ARROW, which greys at a stack of one: Stack is not its parameter, so it is told when the
	// panel is.
	if (stackArrow_ != nullptr) {
		stackArrow_->redraw();
	}
	// NOTE PRIORITY IS LIVE IN EVERY MODE, and the mock said otherwise. Synth::applyModes() feeds it
	// to setStealingPriorityMode unconditionally, so in Poly it chooses which voice DIES to make
	// room for a new note — tests/test_synth.cpp's "poly steals the note the priority is not
	// protecting" pins exactly that, with Low and High shedding different notes. Greying it in Poly
	// told the user a control was dead when it decides which of their held notes they lose. The
	// engine is the authority here, not the mock, and the mock has been corrected to match.
	notePriority_->setEnabled(true);
	// Glide is the same question asked the same way and answered the other way: glideSecondsNow()
	// hands the voices 0 in Poly whatever the parameter says, so a glide time set there really does
	// change nothing. Fine tune has no such condition — it is in the pitch sum always, and Coarse beside it.
	glide_->setEnabled(!poly);
}

bool EngineRail::phaseRouted() const {
	const int at = modDestinationIndexOf(ParamId::OscPhase);
	if (at < 0) {
		return false;
	}
	bool routed = false;
	bridge_.forEachRoute([&](const backplane::Route& r) {
		if (r.destination.device == backplane::Destination::core(static_cast<size_t>(at)).device
		    && r.destination.index == static_cast<uint32_t>(at) && r.amount != 0.0f) {
			routed = true;
		}
	});
	return routed;
}

bool EngineRail::additiveEngine() const {
	return oscTypeFromValue(bridge_.value(static_cast<clap_id>(ParamId::OscType))) == OscType::Additive;
}

std::string EngineRail::engineDisplayName(int index) {
	if (index < 0 || index >= static_cast<int>(kOscTypeNames.size())) {
		return "\xe2\x80\x94";
	}
	// THE PHI ENGINES WEAR THE PREFIX; the others are not phi tables (Basic Shapes' are Fourier series, Additive is a
	// partial bank, Pluck a Karplus-Strong string, Drum a kit), so they keep the bare name.
	const char* name = kOscTypeNames[static_cast<size_t>(index)];
	const bool bare = index == static_cast<int>(OscType::BasicShapes) || index == static_cast<int>(OscType::Additive)
	                  || index == static_cast<int>(OscType::Pluck) || index == static_cast<int>(OscType::Drum);
	return bare ? std::string(name) : std::string("Phi") + name;
}

void EngineRail::selectEngine(int index) {
	if (index < 0 || index >= static_cast<int>(kOscTypeNames.size())) {
		return;
	}
	// A pick is a complete edit, bracketed on its own so the host and the editor's history each record one step.
	(void)writeParamAsEdit(bridge_, static_cast<clap_id>(ParamId::OscType), static_cast<double>(index), "Engine");
	if (notifier_ != nullptr) {
		notifier_->say(std::string("Engine ") + engineDisplayName(index));
	}
	redraw();
}

void EngineRail::applyEngineVisibility() {
	// Osc Mode and Phase Width ask only for a string (Pluck), since backlog LY and OZ gave Additive's
	// right ear its own phase. Guarded: it runs per frame.
	const auto show = [](visage::Frame* frame, bool shown) {
		if (frame->isVisible() != shown) {
			frame->setVisible(shown);
		}
	};
	// PLUCK DRAWS NEITHER OSC MODE NOR PHASE NOR PHASE WIDTH (KS design §3): a string has no phase for a mode to reset or an
	// offset to move, so only an engine switch could wake them (hidden, not greyed).
	// NOR DOES DRUM: a hit has no read phase and no oscillator a mode could reset (drum proposal §4.5).
	const OscType engineNow = oscTypeFromValue(bridge_.value(static_cast<clap_id>(ParamId::OscType)));
	const bool pluck = engineNow == OscType::Pluck || engineNow == OscType::Drum;
	show(phaseWidth_.get(), page_ == Page::Pitch && !pluck);
	show(phase_.get(), page_ == Page::Pitch && !pluck);
	// OSC MODE IS DRAWN ON ADDITIVE AGAIN (backlog LY: phase drift in detune): the bank now starts each
	// note at the accumulator's phase and follows its Catchup bend, Digi restart and Analog re-lock (OscVoice::controlPartials).
	show(oscMode_.get(), !pluck);
	// A DRUM KIT'S VOICES ARE ITS PADS' (what the rail's voice controls do in the Drum engine was unclear). A kit note is a voice in its PAD'S own polyphony group, played Poly with the pad's own
	// Voices limit (Synth::syncVoiceManagerSettings), so the voice mode, the voice count and the note priority, which all
	// set the ordinary group, never reach a kit's hit; nor does Glide, which a Poly group hands 0. Hidden, not greyed: only
	// leaving Kit mode would wake them. Pitched mode plays the ordinary group, so they all reach it there.
	const bool kit = engineNow == OscType::Drum
	                 && steppedIndex(bridge_.value(static_cast<clap_id>(ParamId::DrumMode)), kDrumModeNames.size())
	                        == static_cast<size_t>(DrumMode::Kit);
	show(voiceMode_.get(), !kit);
	show(voiceCount_.get(), !kit);
	show(notePriority_.get(), !kit);
	show(glide_.get(), page_ == Page::Pitch && !kit);
}

void EngineRail::applyPageVisibility() {
	const bool pitchPage = page_ == Page::Pitch;
	for (visage::Frame* frame : { static_cast<visage::Frame*>(transposeOct_.get()),
	                              static_cast<visage::Frame*>(transposeSemi_.get()), static_cast<visage::Frame*>(glide_.get()),
	                              static_cast<visage::Frame*>(coarse_.get()), static_cast<visage::Frame*>(fine_.get()),
	                              static_cast<visage::Frame*>(phase_.get()),
	                              static_cast<visage::Frame*>(bendRange_.get()) }) {
		frame->setVisible(pitchPage);
	}
	applyEngineVisibility();
	for (visage::Frame* frame : { static_cast<visage::Frame*>(stackShape_.get()),
	                              static_cast<visage::Frame*>(stackMap_.get()),
	                              static_cast<visage::Frame*>(stackCurve_.get()),
	                              static_cast<visage::Frame*>(stackPlot_.get()) }) {
		frame->setVisible(!pitchPage);
	}
	if (stackArrow_ != nullptr) {
		stackArrow_->setVisible(!pitchPage);
	}
}

void EngineRail::collapsedChanged() {
	Rail::collapsedChanged();
	// The rail shows or hides every child, the tooltip layer included; the layer is shown only by a hover that rests.
	tips_.clear();
	tips_.setVisible(false);
	// REOPENED, the rail has shown every child — both pages at once — so the one not turned to is hidden again.
	if (!collapsed()) {
		applyPageVisibility();
	}
}

void EngineRail::showPage(Page page) {
	if (page == page_) {
		return;
	}
	page_ = page;
	layOutInto(true);
	redraw();
}

visage::Bounds EngineRail::pageTabBounds(Page page) const {
	// THE TWO NAMES IN THE HEADING ROW, each as wide as it is drawn, with a heading's own gap between.
	const visage::Font& label = font(kBylineSize);
	const float pitchWidth = trackedWidth(label, "PITCH", kBylineTracking);
	const float stackWidth = trackedWidth(label, "STACK", kBylineTracking);
	const float gap = 12.0f;
	return page == Page::Pitch ? visage::Bounds { kPad, pageHeadingY_, pitchWidth, kLabelHeight }
	                           : visage::Bounds { kPad + pitchWidth + gap, pageHeadingY_, stackWidth, kLabelHeight };
}

bool EngineRail::glideEnabled() const noexcept {
	return glide_->enabled();
}

bool EngineRail::fineEnabled() const noexcept {
	return fine_->enabled();
}

float EngineRail::layOutInto(bool place) {
	// ONE walk down the panel, used twice: once to position the controls and once to find where the
	// section headings go. They used to be two pieces of arithmetic that had to agree, with a comment
	// asking the next person to keep them agreeing — and they stopped agreeing the moment the
	// segments went vertical and the pitch rows were reordered.
	const float w = width() - 2.0f * kPad;
	const float knobHeight = Knob::columnHeight(Knob::kEngineRailDiameter);
	// THE SCROLL IS AN OFFSET ON THE WALK and nothing else, which is what keeps it out of every other
	// line in this function: at the design canvas it is zero and every bound below is the one it always
	// was. See the note on EngineRail::contentHeight for the capture that made it necessary.
	float y = kPad - scroll_;
	if (place) {
		++placements_;
	}

	auto row = [&](visage::Frame* frame, float height) {
		if (place) {
			frame->setBounds(kPad, y, w, height);
		}
		y += height + kRowGap;
	};
	// A stacked segment is as tall as it has options; a lined-up one is a single row.
	auto segment = [&](SegmentedControl* seg, int options) {
		row(seg, seg->vertical() ? kRowHeight * static_cast<float>(options) : kRowHeight);
	};

	// THE ENGINE CHOICE, FIRST (QH): in the heading row, left of the rail's own toggle, where the "VOICE" caption was (the
	// box heads the section by being the first thing in it; a separate row would have cost the rail 30 px it does not have).
	if (place) {
		const visage::Bounds toggle = toggleBounds();
		engineBox_->setBounds(kPad, y, std::max(40.0f, toggle.x() - kPad - kRowGap), kRowHeight);
	}
	y += kRowHeight + kRowGap;
	segment(voiceMode_.get(), static_cast<int>(kVoiceModeNames.size()));
	row(voiceCount_.get(), kRowHeight);
	// THE STACK AS ONE ROW OF THREE KNOBS, under Voice Count (step 2): how many, how far apart in pitch, how wide
	// in the stereo field. ONE row because this rail has no height to spare — a stepper row plus a knob pair
	// overflowed it by 71 px and pushed the routings out of reach, which "every Engine rail section fits inside the
	// rail it is drawn in" caught. So Stack is a stepped knob here, and Voice Mode lies flat (see its note above)
	// to pay for the row (voice-stacking doc §7, assumption 16).
	if (place) {
		const float third = w / 3.0f;
		stackCount_->setBounds(kPad, y, third, knobHeight);
		stackDetune_->setBounds(kPad + third, y, third, knobHeight);
		stackSpread_->setBounds(kPad + 2.0f * third, y, third, knobHeight);
	}
	y += knobHeight + kRowGap;
	segment(notePriority_.get(), static_cast<int>(kNotePriorityNames.size()));
	// What a note-on does to the running phase, beside the retrig options it modifies (V21d/V24).
	// ITS ROW IS RESERVED EVEN WHERE IT IS NOT DRAWN (Additive, backlog LY): hidden, it leaves a gap rather than pulling the
	// pitch page up, so switching engine never moves a control under the hand.
	segment(oscMode_.get(), static_cast<int>(kOscModeNames.size()));

	const float pitchHeadingY = y + kSectionGap;
	y += kSectionGap + kLabelHeight; // "Pitch | Stack"
	pageHeadingY_ = pitchHeadingY;
	const float pageTop = y;

	// THE PITCH PAGE. Octave and semitone share a row now (step 3): the row they gave up is what the stack's cell in
	// the source grid costs (voice-stacking doc §7, assumption 18).
	if (place) {
		const float half = (w - kRowGap) * 0.5f;
		transposeOct_->setBounds(kPad, y, half, kRowHeight);
		transposeSemi_->setBounds(kPad + half + kRowGap, y, half, kRowHeight);
	}
	y += kRowHeight + kRowGap;

	// Octave, semitone, the knobs, then bend — the mock's order, with the knobs between the
	// steppers rather than after them.
	// PHASE IS A PITCH KNOB and PHASE WIDTH shares the bend row as a value box: a knob row of their own cost
	// 56 px the rail does not have ("every Engine rail section fits inside the rail it is drawn in" caught it).
	// AND COARSE BESIDE FINE (2026-10-03): two list rows of two, Coarse | Fine over Glide | Phase. The RIGHT column is
	// the wider one because Fine's reading is the widest ("-100.0 cents"): at half the row it was cut to "-100.0 cen"
	// (photographed), and Coarse's "-48.00 st" has room to give.
	const float listRow = Knob::rowHeight(kPitchRowDiameter);
	if (place) {
		const float left = (w - kRowGap) * 0.47f;
		const float right = w - kRowGap - left;
		coarse_->setBounds(kPad, y, left, listRow);
		fine_->setBounds(kPad + left + kRowGap, y, right, listRow);
		glide_->setBounds(kPad, y + listRow + kRowGap, left, listRow);
		phase_->setBounds(kPad + left + kRowGap, y + listRow + kRowGap, right, listRow);
	}
	y += 2.0f * listRow + 2.0f * kRowGap;

	// BEND TAKES THE WIDER SHARE since its default became 12 (2026-10-03): "bend 12 st" is two characters longer
	// than "bend 2 st" and half the row cut it to "end 12 s" (photographed). P.Width's two short lines need less.
	if (place) {
		const float bendWidth = (w - kRowGap) * 0.64f;
		bendRange_->setBounds(kPad, y, bendWidth, kRowHeight);
		phaseWidth_->setBounds(kPad + bendWidth + kRowGap, y, w - kRowGap - bendWidth, kRowHeight);
	}
	y += kRowHeight + kRowGap;
	const float pitchBottom = y;

	// THE STACK PAGE, in the same place (steps 3 and 5): Shape beside the picture of the index it makes, then Mapping and
	// the Curve. Mapping's widest mode, "Center-out", fits two thirds of the rail.
	y = pageTop;
	if (place) {
		// SHAPE AND THE PICTURE OF WHAT IT MAKES: the cell as narrow as its widest mode ("Random Fresh") allows, and the
		// plot in what is left of the row.
		const float shapeWidth = std::min(w, std::max(stackShape_->minimumWidth(), w * 2.0f / 3.0f));
		stackShape_->setBounds(kPad, y, shapeWidth, knobHeight);
		stackPlot_->setBounds(kPad + shapeWidth, y, w - shapeWidth, knobHeight);
	}
	y += knobHeight + kRowGap;
	if (place) {
		stackMap_->setBounds(kPad, y, w * 2.0f / 3.0f, knobHeight);
		stackCurve_->setBounds(kPad + w * 2.0f / 3.0f, y, w / 3.0f, knobHeight);
	}
	y += knobHeight + kRowGap;
	// AND THE INDEX THOSE THREE MAKE, AS A SOURCE (backlog MO): its arrow the full width of the rail directly under them,
	// so it reads as the stack's own output rather than as a ninth sound. It left the source grid for this row.
	if (stackArrow_ != nullptr) {
		if (place) {
			stackArrow_->setBounds(kPad, y, w, StackIndexArrow::kHeight);
		}
		y += StackIndexArrow::kHeight + kRowGap;
	}
	// THE TALLER PAGE SETS THE SECTION, so turning the page moves nothing below it.
	y = std::max(y, pitchBottom);
	// WHICH PAGE IS ON SCREEN — never while the rail is closed, which hides everything (`Rail::collapsedChanged`).
	if (place && !collapsed()) {
		applyPageVisibility();
	}

	// THE SOURCE GRID, DIRECTLY ABOVE THE ROUTINGS — deliberately, and the adjacency is the whole
	// of it: the arrows are what MAKES a route and the list below is what the route became, so a hand
	// that arms an arrow sees its work appear an inch under its own finger.
	if (sources_ != nullptr) {
		// A ROW GAP RATHER THAN A SECTION GAP, and the grid's own heading is what separates it — the
		// rail has no room to spend twelve pixels on air, for the reason spelled out on kCellHeight.
		y += kRowGap;
		if (place) {
			sources_->setBounds(kPad, y, w, sources_->preferredHeight());
		}
		y += sources_->preferredHeight() + kRowGap;
	}

	// And the routings under everything, which is where the mock's own section sits. Its height follows
	// what there is to show, so a macro with four routes pushes nothing off the rail that was not
	// already at the bottom of it.
	if (routes_ != nullptr) {
		y += kSectionGap;
		const float routesHeight = routes_->preferredHeight();
		if (place) {
			routes_->setBounds(kPad, y, w, routesHeight);
			placedRoutesHeight_ = routesHeight;
		}
		y += routesHeight + kRowGap;
	}
	// WHERE THE LAST SECTION ENDS, which is not where the walk ends: every branch above leaves a row
	// gap behind it for the next one, and counting that trailing gap would make the panel scrollable
	// by a few pixels at the design canvas — a wheel able to nudge the whole rail for no reason, in
	// the one size everything else is written against. Measured from the top of the CONTENT rather
	// than of the frame, so the clamp and the placement are the same arithmetic.
	contentHeight_ = y + scroll_ - kRowGap;
	return pitchHeadingY;
}

float EngineRail::contentHeight() const {
	if (contentHeight_ <= 0.0f) {
		// Nothing has been laid out yet, which is a panel that has not been given a size.
		const_cast<EngineRail*>(this)->layOutInto(false);
	}
	return contentHeight_;
}

float EngineRail::maxScroll() const {
	return std::max(0.0f, contentHeight() - height());
}

void EngineRail::setScroll(float y) {
	const float clamped = std::clamp(y, 0.0f, maxScroll());
	if (clamped == scroll_) {
		return;
	}
	scroll_ = clamped;
	layOutInto(true);
	redraw();
}

bool EngineRail::mouseWheel(const visage::MouseEvent& e) {
	if (maxScroll() <= 0.0f) {
		// AT THE DESIGN CANVAS THIS IS EVERY WHEEL EVENT, and returning false is what lets one reach
		// whatever is behind us instead of being swallowed by a rail with nowhere to go.
		return false;
	}
	// One notch moves a row, which is the unit this panel is built out of — the strip moves a card for
	// the same reason: it is what the eye is tracking.
	setScroll(scroll_ - e.precise_wheel_delta_y * (kRowHeight + kRowGap));
	return true;
}

void EngineRail::resized() {
	// A rail that got taller has room for more of its contents, and a scroll left where it was would
	// hold the top of the panel off a frame that no longer needs it held anywhere. Clamped here rather
	// than through setScroll, which lays out and would recurse.
	layOutInto(true);
	scroll_ = std::clamp(scroll_, 0.0f, maxScroll());
	layOutInto(true);
}

bool EngineRail::updateLayout() {
	// THE HEIGHT IS THE TRIGGER, NOT THE TICK: the editor ticks every frame, and a route whose amount moved has
	// changed nothing here. Every road that changes the rows — a knob's drag, the ×, undo, a load, an arm — ends
	// in a different height on the next tick, so none of them has to remember to call this.
	if (routes_ == nullptr || routes_->preferredHeight() == placedRoutesHeight_) {
		return false;
	}
	// AS A RESIZE DOES IT: a list that shrank can leave the scroll past the content's new end.
	resized();
	redraw();
	return true;
}

void EngineRail::setModulation(ModState* state, EditHistory* history) {
	glide_->setModulation(state, history);
	coarse_->setModulation(state, history);
	fine_->setModulation(state, history);
	phase_->setModulation(state, history);
	phaseWidthControl_->setModulation(state, history);
	engineControl_->setModulation(state, history);
	stackDetune_->setModulation(state, history);
	stackSpread_->setModulation(state, history);
	// Stack is not a destination (it is stepped), but every knob in the editor hangs off the modulation state so
	// it can say so rather than be skipped ("every knob in the editor is wired to the modulation state").
	stackCount_->setModulation(state, history);
	// THE STACK PAGE'S DESTINATIONS (step 3): the amounts and Free's phase through their cells' boxes, and the Curve.
	stackShapeMode_->setModulation(state, history);
	stackShapeAmount_->setModulation(state, history);
	stackFreePhase_->setModulation(state, history);
	stackMapMode_->setModulation(state, history);
	stackMapAmount_->setModulation(state, history);
	stackCurve_->setModulation(state, history);
	if (state != nullptr) {
		modWatch_ = state->watch(this);
	}
	if (state != nullptr && history != nullptr && routes_ == nullptr) {
		// THE GRID FIRST, so the child order matches the reading order — it is drawn above the list and
		// it is where a routing session starts.
		sources_ = std::make_unique<SourceGrid>(bridge_, *state, notifier_);
		addChild(sources_.get());
		stackArrow_ = std::make_unique<StackIndexArrow>(bridge_, *state, notifier_);
		addChild(stackArrow_.get());
		stackArrow_->setVisible(page_ == Page::Stack && !collapsed());
		routes_ = std::make_unique<RouteList>(host_, *state, *history, notifier_);
		addChild(routes_.get());
		layOutInto(true);
	}
}

bool EngineRail::pressCollapse(visage::Point pixels) {
	if (!toggleHit(pixels)) {
		return false;
	}
	if (toggleCollapsed() && notifier_ != nullptr) {
		// THE CLOSED STRIP HAS NO ROOM FOR THE ENGINE'S NAME (the longest label any rail carries is 160 px, `Rail::kCollapsedLabelRun`;
		// "ENGINE \xc2\xb7 PhiV5" alone is 185), so the notice that closes it says which engine is sounding (QH).
		notifier_->say(collapsed() ? std::string("Engine minimised \xc2\xb7 ")
		                                 + engineDisplayName(static_cast<int>(std::lround(
		                                     bridge_.value(static_cast<clap_id>(ParamId::OscType)))))
		                           : "Engine shown");
	}
	return true;
}

void EngineRail::draw(visage::Canvas& canvas) {
	// The enablement is re-read here rather than only on click, so the panel follows automation and
	// preset loads as well as its own controls — the registry redraws it whenever VoiceMode moves.
	refreshEnablement();

	const float w = width();
	const float h = height();

	canvas.setColor(kSurface);
	canvas.fill(0.0f, 0.0f, w, h);
	// The mock separates the Engine rail from the surface with a single hairline.
	canvas.setColor(kLine);
	canvas.fill(w - 1.0f, 0.0f, 1.0f, h);

	// CLOSED IS A WHOLE DRAWING, not the open one with pieces skipped — the same rule the modulator
	// strip follows. The fill and the hairline above are what a rail is; everything below is what
	// this particular rail holds.
	if (collapsed()) {
		drawRailChrome(canvas);
		return;
	}

	const visage::Font& label = font(kBylineSize);
	canvas.setColor(kMuted);

	// The headings come from the same walk that positions the controls, so a spacing or ordering
	// change moves both by construction rather than by remembering to.
	layOutInto(false);
	// THE SECTION'S TWO PAGES, both named (step 3): the shown one bright and underlined, the other in the headings'
	// muted ink, a click away — so the choice is on the face before it is made.
	layOutInto(false);
	for (const Page tab : { Page::Pitch, Page::Stack }) {
		const visage::Bounds at = pageTabBounds(tab);
		const bool shown = tab == page_;
		canvas.setColor(shown ? kInk : kMuted);
		drawTracked(canvas, tab == Page::Pitch ? "PITCH" : "STACK", label, kBylineTracking, at.x(), at.y(),
		            kLabelHeight);
		if (shown) {
			canvas.setColor(kAccent);
			canvas.fill(at.x(), at.bottom() - 1.0f, at.width(), 1.0f);
		}
	}
	canvas.setColor(kMuted);

	// THE SCROLL BAR, when there is something to scroll (the rail scrolled with no visible bar).
	// The cards' own bar, on the rail's inner edge, drawn by the rule
	// its header states: nothing to scroll, nothing to say.
	drawBodyScrollBar(canvas, { 0.0f, 0.0f, w - 1.0f, h }, scroll_, contentHeight());

	// AND THE WAY OUT, which the open rail needs quite as much as the closed one does. Drawn last so
	// it sits over the panel rather than under it. Missing this on the open state is a defect a test
	// cannot see — pressCollapse answers at the right rectangle whether or not anything is painted
	// there — and the windowed capture is what caught it: an Engine rail that could be closed only by
	// somebody who already knew where the button was.
	drawRailChrome(canvas);
}

} // namespace feathers::ui
