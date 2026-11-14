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

#include "plugin/ui/filter_card.h"
#include "plugin/ui/menu_anchor.h"

#include "plugin/gui_bridge.h"
#include "plugin/params.h"
#include "plugin/ui/controls.h"
#include "plugin/ui/fx_lane.h"
#include "plugin/ui/notify_tray.h"
#include "plugin/ui/theme.h"
#include "plugin/ui/widget_registry.h"

#include <algorithm>
#include <cmath>
#include <span>
#include <string>

namespace feathers::ui {

namespace {

namespace flt = feathers::fx::filter;

// descOf() STOOD HERE AND IS GONE WITH THE PARAMETERS IT LOOKED UP. It turned a ParamId into the
// host table's descriptor so a widget could be built over it; every control on this card is now
// addressed by its index in the DEVICE's own declarations, which is device.h's permanent rule and the
// one place this card used to fall short of it. The compiler found this one — with -Werror on, a
// helper nobody calls is an error rather than a leftover.

/// The dropdown, in dropdown order: every type the module actually renders, then the non-SVF
/// families that are coming. Those are DISABLED "· soon" rows, which is the header bar's idiom
/// (header_bar.cpp:49) and the honesty rule applied to a menu — a name you can read and cannot
/// choose is a promise; a name you can choose that does nothing is a lie. They are also what gives
/// the arrows something to skip, which is the only reason skipping is worth writing.
///
/// "SOON" IS RESEARCH, NOT WIRING. This comment used to say the coming models all sat in
/// libs/sst-filters already. Under the licence migration (2026-09-24) that library is GPL code this
/// project is leaving, so nothing may be wired from it: a ladder, if one comes, is a clean-room model
/// written from published sources (e.g. Zavalishin, "The Art of VA Filter Design"). The four models
/// that had been transcribed from it were dropped rather than rewritten.
constexpr FilterCard::TypeEntry kSoonEntries[] = {
	{"Vintage Ladder · soon", -1, false},
};

/// Built once from the module's own name list plus the soon rows, so a type appended in src/fx
/// appears in this menu without anybody coming here to add it.
const std::vector<FilterCard::TypeEntry>& allTypeEntries() {
	static const std::vector<FilterCard::TypeEntry> entries = [] {
		std::vector<FilterCard::TypeEntry> out;
		for (int i = 0; i < static_cast<int>(flt::Type::kCount); ++i) {
			out.push_back({flt::kTypeNames[i], i, true});
		}
		for (const FilterCard::TypeEntry& soon : kSoonEntries) {
			out.push_back(soon);
		}
		return out;
	}();
	return entries;
}

/// HOW LOUDLY THE CARD STATES ITSELF WHILE THE DEVICE IS SWITCHED OUT — a disabled card reads
/// grey. The response curve shipped with this number and the type
/// row now shares it rather than picking a second one, so there is one answer to "how grey is off".
///
/// It applies only to what the CARD draws. The knobs and the keytrack switch have their own
/// switched-off colours and are told to use them instead — see showEnablement.
constexpr float kOffAlpha = 0.35f;

/// The curve's vertical range, in decibels. Wide enough for a resonant peak and a shelf at full
/// gain (±24 dB) with room left, narrow enough that a gentle roll-off is still a visible slope.
constexpr float kCurveTopDb = 30.0f;
constexpr float kCurveBottomDb = -42.0f;

/// The colour bands, copied from the wave display along with the reason: a per-point colour puts one
/// gradient-atlas entry per point on every redraw, and a redraw happens on every parameter change.
constexpr int kColourBands = 16;

/// HOW MUCH OF THE CURVE'S COLOUR IS TAKEN OUT — the shipped curve was too colourful. Three quarters of the chroma, which leaves the line reading as grey while the
/// last quarter still says which end is which.
///
/// The ends stay kAccent and kValue rather than becoming two new colours, and desaturate() is why
/// that costs nothing: it moves neither end's BRIGHTNESS, so the curve is drawn at exactly the
/// lightness it always was and only its colourfulness changes — which is the whole of the note. A
/// re-skin that moves either token moves this line with it, as it should.
constexpr float kCurveDesaturation = 0.75f;

float dbOf(float gain) {
	return 20.0f * std::log10(std::max(gain, 1.0e-6f));
}

} // namespace

DeviceControl& FilterCard::control(flt::Param param) const {
	const size_t index = static_cast<size_t>(param);
	// BUILT ON FIRST ASK rather than in the constructor's initialiser list, because an FxControl holds
	// a reference and eleven of them would otherwise need eleven lines that say the same thing. The
	// array is `mutable` for this and this alone: what it caches is an ADAPTER, not a value, so a
	// const read building one is not a const read changing anything the card reports.
	if (controls_[index] == nullptr) {
		controls_[index] = std::make_unique<DeviceControl>(host_, device_, flt::kDescriptor, index,
		                                                  mod_, history_);
	}
	return *controls_[index];
}

FilterCard::FilterCard(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id,
                       Notifier* notifier)
    : host_(host), notifier_(notifier), device_(id) {
	(void)registry; // the chips take none, as the knobs do not
	// NO PARAMETER WATCHES ANY MORE, and their absence is the whole of this change.
	//
	// This card used to watch eight of the filter's clap ids so the curve would follow the knobs. The
	// filter's controls are not host parameters now — a docked effect's controls are not on the
	// plugin's CLAP surface; they are swallowed and reached by internal modulation — so there is nothing left to watch. The card is subscribed to the STATE road
	// instead, in Gui::create, which is what carries a device row; that subscription went in ahead of
	// this commit precisely so there was never a build where neither road reached the card.

	const auto addKnob = [&](flt::Param param, const char* label) {
		auto knob = std::make_unique<Knob>(control(param), label, notifier);
		knob->setFaceDiameter(kKnobDiameter);
		addChild(knob.get());
		knobs_.push_back(std::move(knob));
		// RECORDED AS THE KNOB IS BUILT, in the one place that knows which control it was built for.
		// Knob addresses its value through device::Modulatable and does not publish which one, so
		// asking it afterwards is not possible — and a second hand-kept list is the shape this arc
		// keeps removing.
		knobParams_.push_back(param);
	};
	// Row A is the filter every filter has; row B is the two that follow the type. The macro labels
	// are set in draw rather than here, because they change with the dropdown while the ids do not.
	addKnob(flt::Param::Cutoff, "Cutoff");
	addKnob(flt::Param::Resonance, "Res");
	addKnob(flt::Param::Drive, "Drive");
	// ROW B, IN CELL ORDER: Spread first because it applies to every type and therefore never
	// vanishes, then the two macro slots, which do.
	addKnob(flt::Param::Spread, "Spread");
	addKnob(flt::Param::Macro1, macroLabel(0));
	addKnob(flt::Param::Macro2, macroLabel(1));

	// POLY GETS THE SWITCH WITH WORDS ON IT, because IT is the architecture: off is one filter on the
	// summed bus, on is one filter per voice. A control that changes what the signal path IS should
	// say so in more than a light.
	//
	// THAT SENTENCE USED TO SIT ON KEYTRACK AND WAS TRUE THEN. Until the 2026-08-23 split, keytrack
	// chose the arrangement AND the tracking; the engine now reads poly for the arrangement, so the
	// justification moved with the job. Leaving it where it was would have been a label describing a
	// control that no longer does what it says.
	auto poly = std::make_unique<SwitchChip>(control(flt::Param::Poly), "Poly");
	addChild(poly.get());
	poly_ = std::move(poly);

	// KEYTRACK IS TONAL: whether the corner follows the note — a voice's own with Poly on, the host's
	// one picked note on the bus. Under Poly, because Poly decides which note that is.
	auto keytrack = std::make_unique<SwitchChip>(control(flt::Param::Keytrack), "Keytrack");
	addChild(keytrack.get());
	keytrack_ = std::move(keytrack);

	onMouseDown() = [this](const visage::MouseEvent& e) {
		// A PRESS MAY BECOME A DRAG — recorded before the parts are dispatched, because the part that
		// wins here (fold, on the head) is the one a drag has to be able to override.
		noteCardPress(e);
		switch (partAt(e.position)) {
		case Part::Fold:
			// **THE FOLD HAPPENS ON RELEASE, NOT HERE** — see `FxCard::onMouseUp`. This arm exists so a
			// press on the head is not dispatched as a body press; it has nothing to do.
			break;
		case Part::Enable:
			toggleEnabled();
			break;
		case Part::TypeMenu:
			showTypeMenu();
			break;
		case Part::PrevType:
			stepType(-1);
			break;
		case Part::NextType:
			stepType(1);
			break;
		case Part::SlopeMenu:
			showSlopeMenu();
			break;
		case Part::SlotMenu:
			pressSlotMenu();
			break;
		case Part::None:
			break;
		}
	};
}

std::span<const FilterCard::TypeEntry> FilterCard::typeEntries() {
	return allTypeEntries();
}

float FilterCard::openHeight() {
	// DERIVED, so a row that grows moves the card instead of overflowing it.
	// The switch shares the macro row rather than taking one of its own, so the card is not as tall as
	// its parts listed end to end — the first version reserved a row for it and left a band of empty
	// card under the knobs, which the camera showed and the arithmetic did not.
	//
	// TWO SWITCH STRIPS SINCE 2026-08-23, not one: poly and keytrack are separate controls now. An
	// existing geometry case caught the omission the moment the second switch was added — it asserts
	// a switch's bottom is inside the card — which is what pinning geometry rather than trusting it
	// is for.
	return kHeadHeight + kRowGap + kTypeRowHeight + kRowGap + kCurveHeight + kRowGap
	       + Knob::columnHeight(kKnobDiameter) + kRowGap + Knob::columnHeight(kKnobDiameter) + kRowGap
	       + kSwitchHeight + 6.0f + kSwitchHeight + kPad;
}

void FilterCard::foldedChanged() {
	// A folded card hides what it holds, for the reason a closed rail does: a control drawn outside
	// its card would answer a click meant for whatever is now in that space.
	for (auto& knob : knobs_) {
		knob->setVisible(!folded_);
	}
	// The macro slots have their own answer, which is not simply "unfolded": a type that declares
	// nothing there keeps no knob whether the card is open or shut.
	syncMacroKnobs();
	// AND THE KEYTRACK SWITCH HIDES WITH THEM, now that it is in a strip at the BOTTOM rather than in
	// the head. It survived a fold while it lived up there, because the head is exactly what a folded
	// card still is; below the fold it is an ordinary control and goes with the rest. The enable
	// square remains the one thing a fold leaves behind, which is right — it is the only one you need
	// to get the device back.
	if (poly_ != nullptr) {
		poly_->setVisible(!folded_);
	}
	if (keytrack_ != nullptr) {
		keytrack_->setVisible(!folded_);
	}
	// AND THE LANE IS TOLD, because a fold changes this card's HEIGHT and the lane places cards by
	// asking for it. redraw() alone repaints the card at the size it already had — the controls vanish
	// but the card keeps its size.
}

visage::Bounds FilterCard::headBounds() const {
	// **THE FRAME'S HEAD, ON WHICHEVER EDGE IT IS** (backlog KR). This was the top 24px across the whole
	// width, written when the head was always on top. In the rack's horizontal lane the head is a left
	// spine and the body starts at y 0 (4128abe1 moved the type row to `bodyTopEdge() + kRowGap`), so the
	// arrows, the type box and the poles box all sat inside that stale strip: `partAt` answered Fold, the
	// fold is a release verb that the base's own spine test then refused, and the press did nothing.
	// Every hand-written card with a `headBounds` had the same stale strip and returns this now.
	return ownFrame().head;
}


visage::Bounds FilterCard::typeMenuBounds() const {
	const float left = bodyLeft() + kPad + kArrowWidth + 4.0f;
	// The slope box and the forward arrow both live to its right now.
	const float right = width() - kPad - kSlopeBoxWidth - 4.0f - kArrowWidth - 4.0f;
	return {left, bodyTopEdge() + kRowGap, right - left, kTypeRowHeight};
}

visage::Bounds FilterCard::arrowBounds(int direction) const {
	const float y = bodyTopEdge() + kRowGap;
	// **FROM THE BODY, LIKE ITS SIBLING ONE LINE UP.** The back arrow read `kPad` — the CARD's origin
	// — while `typeMenuBounds` beside it read `bodyLeft() + kPad`, so on a spine the arrow was drawn
	// and hit-tested underneath the header. A guard asymmetry inside one function.
	//
	// THE SPINE SWEEP DID NOT CATCH THIS AND COULD NOT: it walks CHILD FRAMES, and this arrow is
	// painted geometry with no frame of its own. Which is the limit of that case stated plainly — it
	// covers everything a card puts in the tree and nothing a card draws by hand.
	const float x = direction < 0 ? bodyLeft() + kPad
	                              : width() - kPad - kSlopeBoxWidth - 4.0f - kArrowWidth;
	return {x, y, kArrowWidth, kTypeRowHeight};
}

visage::Bounds FilterCard::slopeBoxBounds() const {
	return {width() - kPad - kSlopeBoxWidth, bodyTopEdge() + kRowGap, kSlopeBoxWidth, kTypeRowHeight};
}

bool FilterCard::slopeIsLive() const {
	return flt::slopeAppliesTo(static_cast<flt::Type>(selectedType()));
}

float FilterCard::polyRowTop() const {
	// Below both knob rows: the curve, then row A, then row B, then this. Derived rather than
	// written down, so a row that grows moves the strip instead of drawing over it.
	const float column = Knob::columnHeight(kKnobDiameter);
	return curveBounds().bottom() + kRowGap + column + kRowGap + column + kRowGap;
}

float FilterCard::keytrackRowTop() const {
	// UNDER POLY, AND THAT ORDER IS THE ARGUMENT. Poly decides whether there are per-voice filters at
	// all; keytrack only says what one of them does. Reading top to bottom is reading the dependency,
	// and the lower switch greys when the upper one is off.
	return polyRowTop() + kSwitchHeight + 6.0f;
}

visage::Bounds FilterCard::polyBounds() const {
	// A CHIP THAT NAMES ITSELF, at the strip's left: the chip prints its own word, so there is no caption beside it.
	return {bodyLeft() + kPad, polyRowTop(), kSwitchWidth, kSwitchHeight};
}

visage::Bounds FilterCard::keytrackBounds() const {
	// A STRIP OF ITS OWN along the bottom — nothing else is in this band, so there is no rectangle it can collide with.
	return {bodyLeft() + kPad, keytrackRowTop(), kSwitchWidth, kSwitchHeight};
}

visage::Bounds FilterCard::curveBounds() const {
	const float top = bodyTopEdge() + kRowGap + kTypeRowHeight + kRowGap;
	return {bodyLeft() + kPad, top, bodyWidth() - 2.0f * kPad, kCurveHeight};
}

FilterCard::Part FilterCard::partAt(visage::Point pixels) const {
	// THE SLOT MENU AND THE ENABLE ARE TESTED BEFORE THE HEAD THEY SIT IN, because they are inside the
	// head's rectangle and the head's own verb is fold — a hit test in the other order would make
	// both unclickable and the bug would read as "the toggle does nothing".
	if (slotMenuAt(pixels)) {
		return Part::SlotMenu;
	}
	if (enableBounds().contains(pixels)) {
		return Part::Enable;
	}
	if (headBounds().contains(pixels)) {
		return Part::Fold;
	}
	if (folded_) {
		// Nothing below the head exists while the card is folded, and a click that landed on where a
		// control used to be must not reach it.
		return Part::None;
	}
	if (arrowBounds(-1).contains(pixels)) {
		return Part::PrevType;
	}
	if (arrowBounds(1).contains(pixels)) {
		return Part::NextType;
	}
	if (typeMenuBounds().contains(pixels)) {
		return Part::TypeMenu;
	}
	// A type that declares one slope has a box that says so and does not answer — the honesty rule
	// applied to a hit test, not only to a colour.
	if (slopeIsLive() && slopeBoxBounds().contains(pixels)) {
		return Part::SlopeMenu;
	}
	return Part::None;
}

int FilterCard::selectedType() const {
	// steppedIndex rather than a cast, and the count from the MODULE's own enum: the editor and the
	// engine have to agree about which entry a value names, and both ask the same question of the same
	// list. A cast would also let a malformed patch name a type with no case in any switch.
	return static_cast<int>(
	    steppedIndex(control(flt::Param::Type).value(), static_cast<size_t>(flt::Type::kCount)));
}

bool FilterCard::updateContent() {
	// ONE READ, ONE COMPARISON, A `redraw()` ONLY WHEN THEY DIFFER — `OwlttGraph::update` is the shipped
	// version of this shape, and `AutomodStrip::update` argues for exact equality over a tolerance: the
	// question is "is the screen showing these settings", which is a bitwise question.
	if (!isVisible()) {
		return false; // `redraw()` is refused for an invisible frame, so a true here would be a lie.
	}
	// THE TRIPLE (backlog KU): whether an ear each is drawn, and each ear's settings — the mono picture's own pair
	// when nothing stereo reaches, so mono compares exactly as it always has.
	const bool stereo = stereoCurves();
	const fx::filter::Settings now = stereo ? settingsFor(false) : settings();
	const fx::filter::Settings nowR = stereo ? settingsFor(true) : now;
	if (haveDrawn_ && stereo == drawnStereo_ && now == drawnSettings_ && nowR == drawnSettingsR_) {
		return false;
	}
	drawnSettings_ = now;
	drawnSettingsR_ = nowR;
	drawnStereo_ = stereo;
	haveDrawn_ = true;
	redraw();
	return true;
}

fx::filter::Settings FilterCard::settings() const {
	// ONE READ OF THE DEVICE'S OWN STATE, which is what the swallow bought here and it is worth saying
	// what it replaced. This function used to REBUILD the settings field by field out of eight
	// published parameter values, and the failure mode of that shape is a field nobody remembers: it
	// shipped twice, once when Spread was promoted out of the macro table and once when Slope became
	// reachable, and both times the curve drew a filter the engine was not running. There is no list
	// to forget any more — the row holds the settings the engine reads, and this reads the row.
	// THE ROW, READ THROUGH THE SEAM — `ShaperCard::settings` records why at length. `fxFilterAt`
	// reaches into a `DeviceRow` through a type only the parameter road has; `fieldsPresentAt` and
	// `fieldAt` are the same read in the vocabulary both hosts share, over the same `DeviceRow`, so
	// Feathers' numbers are unchanged by construction rather than by testing.
	flt::Row row {};
	const size_t present = std::min(host_.fieldsPresentAt(device_), row.size());
	for (size_t i = 0; i < present; ++i) {
		row[i] = host_.fieldAt(device_, i);
	}
	flt::Settings s = flt::stateOf(row, present).settings;
	// THE CURVE DRAWS THE FILTER, whether or not it is switched into the signal. The one field this
	// overrides, and the reason the card cannot simply hand the state back untouched.
	s.enabled = true;
	// keyOffsetSemitones stays at its declared zero: the curve draws the filter at the knob's own
	// corner, which is what it is at middle C and what it is on the bus.
	return s;
}

bool FilterCard::stereoCurves() const {
	// THE CONTROLS THAT MOVE THE CURVE'S SHAPE — the four the plot is a picture of. Drive and Spread move the sound
	// and not this picture's magnitude, so a stereo route on them alone leaves one curve.
	for (const flt::Param p : { flt::Param::Cutoff, flt::Param::Resonance, flt::Param::Macro1, flt::Param::Macro2 }) {
		float right = 0.0f;
		if (host_.modulatedFieldRAt(device_, static_cast<size_t>(p), right)) {
			return true;
		}
	}
	return false;
}

fx::filter::Settings FilterCard::settingsFor(bool right) const {
	flt::Row row {};
	const size_t present = std::min(host_.fieldsPresentAt(device_), row.size());
	for (size_t i = 0; i < present; ++i) {
		float r = 0.0f;
		row[i] = right && host_.modulatedFieldRAt(device_, i, r) ? r : host_.modulatedFieldAt(device_, i);
	}
	flt::Settings s = flt::stateOf(row, present).settings;
	s.enabled = true; // as `settings()`: the curve draws the filter, switched in or not
	return s;
}

size_t FilterCard::curvePointCount() const {
	const float scale = std::max(1.0f, dpiScale());
	const float box = std::max(1.0f, curveBounds().width());
	const auto wanted = static_cast<size_t>(std::lround(box * scale * kPointsPerPixel));
	return std::min(kMaxCurvePoints, std::max(kMinCurvePoints, wanted));
}

size_t FilterCard::curveGains(float* out, size_t capacity, bool right) const {
	if (out == nullptr || capacity == 0) {
		return 0;
	}
	const size_t points = std::min(capacity, curvePointCount());
	// The frequencies are needed by the profile call and thrown away — the card places its points by
	// index, since the spacing is fixed and logarithmic. A stack buffer rather than a vector because
	// draw() runs on every frame of a knob drag and this would be an allocation on each of them.
	std::array<float, kMaxCurvePoints> freq{};
	// THE MODULE'S OWN EVALUATOR, checked against the measured impulse response of the real sample
	// loop, AT THE RATE THE ENGINE IS ACTUALLY RUNNING.
	//
	// This was a hardcoded 48000, on the argument that "a response plotted from 20 Hz to 20 kHz is the
	// same picture at any rate a host would pick, and the editor has no business reaching for the
	// audio thread's number to draw a shape". That was true of every type the device had when it was
	// written — the SVFs are bilinear-warped and their shape in Hz barely moves — and the Camp B
	// models ended it: their corner is clamped at 0.3 x samplerate, which is 14.4 kHz at 48k and
	// 13.2 kHz at 44.1k. MEASURED at up to 1.67 dB between the drawn curve and the filter that plays,
	// on an ordinary 44.1k session, worst at high cutoff where the clamp bites.
	//
	// The number is not the audio thread's: the bridge holds it, written once at activate.
	// AN EAR EACH while a stereo route reaches the curve's controls (backlog KU); the base otherwise, as always.
	flt::gainProfile(stereoCurves() ? settingsFor(right) : settings(), host_.sampleRate(), freq.data(), out, points);
	// **AND NOTHING NON-FINITE REACHES THE PLOT.** The rate above is the reason this could happen and
	// is fixed at its source, but a profile is arithmetic over host-supplied numbers and a NaN in it
	// draws as an invisible curve rather than as an error — the quietest possible failure. Clamping
	// here costs one pass and turns any future instance into a flat line, which is at least a line.
	for (size_t i = 0; i < points; ++i) {
		if (!std::isfinite(out[i])) {
			out[i] = 1.0f;
		}
	}
	return points;
}

const char* FilterCard::macroLabel(int which) const {
	const flt::TypeTraits& traits = flt::traitsOf(static_cast<flt::Type>(selectedType()));
	return which == 0 ? traits.macro1.label : traits.macro2.label;
}

int FilterCard::typeAfter(int type, int direction) const {
	const auto entries = typeEntries();
	// Find where this type sits in the MENU, because that is the order an arrow walks — the menu is
	// what the user is looking at, and the enum's order is only incidentally the same.
	size_t at = 0;
	for (size_t i = 0; i < entries.size(); ++i) {
		if (entries[i].enabled && entries[i].type == type) {
			at = i;
			break;
		}
	}
	// SKIPPING, not stopping: the disabled rows are a promise about what is coming, and an arrow that
	// halted on one would make the last real type unreachable from the other direction.
	for (size_t i = at; direction > 0 ? i + 1 < entries.size() : i > 0;) {
		i = direction > 0 ? i + 1 : i - 1;
		if (entries[i].enabled) {
			return entries[i].type;
		}
	}
	return -1;
}

void FilterCard::chooseType(int type) {
	if (type < 0 || type >= static_cast<int>(flt::Type::kCount)) {
		return;
	}
	// A complete edit rather than a drag, so it is bracketed on its own and the host records one
	// undoable step — the same shape the engine menu uses.
	DeviceControl& c = control(flt::Param::Type);
	c.beginGesture();
	c.setValue(static_cast<double>(type));
	c.endGesture();
	if (notifier_ != nullptr) {
		notifier_->say(flt::kTypeNames[static_cast<size_t>(type)]);
	}
	redraw();
}

void FilterCard::stepType(int direction) {
	const int next = typeAfter(selectedType(), direction);
	if (next >= 0) {
		chooseType(next);
	}
}

void FilterCard::showTypeMenu() {
	visage::PopupMenu menu;
	const auto entries = typeEntries();
	const int selected = selectedType();
	for (size_t i = 0; i < entries.size(); ++i) {
		auto& option = menu.addOption(entries[i].type, entries[i].name);
		option.enable(entries[i].enabled);
		option.select(entries[i].enabled && entries[i].type == selected);
	}
	menu.onSelection() = [this](int id) { chooseType(id); };
	showMenuAt(std::move(menu), *this, typeMenuBounds());
}

int FilterCard::selectedSlope() const {
	// steppedIndex and the module's own count, for the reason selectedType() gives: a published value
	// outside the list must not name an entry that does not exist.
	return static_cast<int>(
	    steppedIndex(control(flt::Param::Slope).value(), static_cast<size_t>(flt::Slope::kCount)));
}

void FilterCard::chooseSlope(int slope) {
	if (slope < 0 || slope >= static_cast<int>(flt::Slope::kCount)) {
		return;
	}
	DeviceControl& c = control(flt::Param::Slope);
	c.beginGesture();
	c.setValue(static_cast<double>(slope));
	c.endGesture();
	if (notifier_ != nullptr) {
		notifier_->say(flt::kSlopeNames[static_cast<size_t>(slope)]);
	}
	redraw();
}

void FilterCard::showSlopeMenu() {
	visage::PopupMenu menu;
	const auto type = static_cast<flt::Type>(selectedType());
	const int selected = selectedSlope();
	for (int i = 0; i < static_cast<int>(flt::Slope::kCount); ++i) {
		const auto slope = static_cast<flt::Slope>(i);
		auto& option = menu.addOption(i, flt::kSlopeNames[static_cast<size_t>(i)]);
		// EVERY ENTRY, WITH THE ONES THIS TYPE CANNOT WEAR DISABLED — the same shape the type menu
		// gives its "· soon" rows. A menu that simply omitted them would leave a player wondering
		// where 18 dB went on a bandpass; a greyed row says the type has no one-pole form to make an
		// odd order from.
		option.enable(flt::slopeSupported(flt::traitsOf(type), slope));
		// Ticked where the filter IS, which for an unsupported stored value is the entry it resolved
		// to — the same reading the box shows.
		option.select(slope == flt::effectiveSlope(type, static_cast<flt::Slope>(selected)));
	}
	menu.onSelection() = [this](int id) { chooseSlope(id); };
	showMenuAt(std::move(menu), *this, slopeBoxBounds());
}

const char* FilterCard::contentTooltipAt(visage::Point local) const {
	// THE PRESS ROAD'S OWN HIT TEST, so the tooltip describes what a press there would reach — the type
	// selector (both arrows and the name) and the slope box when it answers at all.
	switch (partAt(local)) {
		case Part::TypeMenu:
		case Part::PrevType:
		case Part::NextType:
			return controlTooltip(static_cast<size_t>(flt::Param::Type));
		case Part::SlopeMenu:
			return controlTooltip(static_cast<size_t>(flt::Param::Slope));
		default:
			return nullptr;
	}
}

device::Modulatable* FilterCard::enableModulatable() {
	// BY INDEX, from the descriptor — the same answer today and the one that keeps working for a
	// device whose enable is not its first control.
	return hasEnable() ? &control(static_cast<flt::Param>(enableControl())) : nullptr;
}

void FilterCard::toggleEnabled() {
	// THE TOGGLE, THE UNDO BRACKETING AND THE REDRAW ARE THE BASE'S — FxCard::toggleEnable. This
	// card reaches it from a `Part` switch rather than a position, which is why it calls that rather
	// than `enablePressAt`: the hit-test already decided.
	if (const auto nowOn = toggleEnable()) {
		if (notifier_ != nullptr) {
			notifier_->say(*nowOn ? "Filter on" : "Filter off");
		}
	}
}

void FilterCard::setModulation(ModState* state, EditHistory* history) {
	// KEPT FIRST, so an adapter built LATER by `control()` is born knowing. The loop below covers the
	// ones that already exist; this covers the ones that do not yet.
	mod_ = state;
	history_ = history;
	// EVERY KNOB ON THE CARD, which is not the same claim as "every destination on the card": a knob
	// whose parameter is not a destination takes this and stays exactly as it was (Knob::mappable asks
	// modDestinationIndexOf before anything else), so this does not have to know which of the five the
	// module declared modulatable. The keytrack switch is not a Knob and is not offered one.
	for (auto& knob : knobs_) {
		knob->setModulation(state, history);
	}
	// AND EVERY ADAPTER, INCLUDING THE ONES NO KNOB HOLDS. A Knob forwards to the control it was
	// given, so the six knobs' adapters are covered twice over; the switches' and the three bespoke
	// parts' are not covered at all otherwise, and an adapter that never learned the panel's state
	// reports itself unarmed forever. Cheap, total, and it cannot fall behind a widget list.
	for (size_t i = 0; i < controls_.size(); ++i) {
		control(static_cast<flt::Param>(i)).setModulation(state, history);
	}
}

void FilterCard::showEnablement(bool on) {
	// GREY BUT NOT DEAF. Each control is told it is INACTIVE rather than disabled, which in this editor
	// are two different things: inactive draws in the switched-off colours and still answers the hand.
	// A filter you have not switched on yet is still a filter you are allowed to dial in — the mod
	// panel lets a route be drawn from a source sitting at zero, and the rail lets a sound be edited
	// while nothing is audible. See Knob::setActive for why this is not setEnabled.
	for (auto& knob : knobs_) {
		knob->setActive(on);
	}
	// POLY AND KEYTRACK ARE CHIPS (two states, so a chip), and a chip has no "greyed but pointable" look: it
	// stays as lit as its value, which is the truth about it whether or not the device is in the signal.
}

std::vector<fx::filter::Param> FilterCard::addressedParamsForTest() const {
	std::vector<fx::filter::Param> out(knobParams_.begin(), knobParams_.end());
	if (poly_ != nullptr) {
		out.push_back(flt::Param::Poly);
	}
	if (keytrack_ != nullptr) {
		out.push_back(flt::Param::Keytrack);
	}
	return out;
}

void FilterCard::layOutKnobs() {
	const float column = Knob::columnHeight(kKnobDiameter);
	const float rowA = curveBounds().bottom() + kRowGap;
	const float rowB = rowA + column + kRowGap;
	const float inner = bodyWidth() - 2.0f * kPad;

	// `cells` is how many columns the row is divided into, which is NOT always how many knobs go in
	// it: the macro row has three cells and three knobs now, and the keytrack switch has left it for
	// the head. Deriving the cell width from the knob count instead is what once drew the switch over
	// a knob, and the rule outlives the arrangement that taught it.
	const auto place = [&](size_t first, size_t count, int cells, float top) {
		const float cell = inner / static_cast<float>(cells);
		for (size_t i = 0; i < count && first + i < knobs_.size(); ++i) {
			knobs_[first + i]->setBounds(bodyLeft() + kPad + cell * static_cast<float>(i), top, cell, column);
		}
	};
	place(0, 3, 3, rowA);               // cutoff, res, drive
	place(3, 3, kMacroRowCells, rowB);  // spread, macro 1, macro 2 — one cell each, always

	if (poly_ != nullptr) {
		const visage::Bounds box = polyBounds();
		poly_->setBounds(box.x(), box.y(), box.width(), box.height());
	}
	if (keytrack_ != nullptr) {
		const visage::Bounds box = keytrackBounds();
		keytrack_->setBounds(box.x(), box.y(), box.width(), box.height());
	}
}

void FilterCard::resized() {
	layOutKnobs();
}

void FilterCard::drawHeadExtras(visage::Canvas& canvas) {
	// **THE HEAD'S MARKS ARE DRAWN HERE, OUTSIDE THE BODY'S CLAMP.** `drawContent` is clamped to the
	// body — which is exactly what stops a card painting over its own head — and in a TOP-head lane
	// the head is outside that rectangle. So a slot menu drawn in `drawContent` is silently CLIPPED:
	// Feathers' Filter card lost its "..." entirely, and the vertical sanity check is what found it.
	drawSlotMenu(canvas, kMuted);
}

void FilterCard::drawContent(visage::Canvas& canvas) {
	// **NO SURFACE AND NO BORDER — `FxCard::draw` painted them.** This card drew its own for as
	// long as there were eight cards each drawing the same two shapes; see `ui::drawCardFrame`.

	const bool on = control(flt::Param::Enabled).value() >= 0.5;

	// THE CONTROLS FOLLOW THE SWITCH, and they are told here rather than in toggleEnabled because the
	// parameter moves from four places — the switch, automation, a preset load, the host's own generic
	// panel — and this is the one of them the card is guaranteed to see: it watches FilterEnabled, so
	// every one of those redraws it.
	showEnablement(on);

	// What a colour the CARD draws becomes while the device is switched out. Multiplies the alpha the
	// colour already carries rather than replacing it, so a stroke that was deliberately faint does
	// not come out LOUDER than it was when the filter was in the signal.
	const auto stated = [&](uint32_t colour) {
		if (on) {
			return colour;
		}
		const float alpha = static_cast<float>((colour >> 24) & 0xFFu) / 255.0f;
		return withAlpha(colour, alpha * kOffAlpha);
	};

	// The head: the device's own name, and the switch that puts it in the signal. The NAME takes one
	// step down the type ladder rather than the dimming above, because it is how you find this card in
	// a lane of cards and a name you have to hunt for is a worse card than a bright one.
	// THE LANE'S VERBS ON THIS SLOT — and like the enable square below, they do NOT dim with the
	// device: removing a filter you have switched off is exactly when you would want to.

	// THE ONE THING THAT NEVER DIMS. Everything else on a switched-out card can afford to recede
	// because none of it is doing anything; this square is still live and it is the only way back, so
	// it draws at full strength in both its states — accent when in the signal, and an empty socket
	// with a visible rim when not. It used to draw that socket in the hairline colour, which on a card
	// that is now grey all over left the way back as the dimmest thing on it.
	const visage::Bounds sw = enableBounds();
	canvas.setColor(on ? kAccent : kMuted);
	if (on) {
		canvas.roundedRectangle(sw.x(), sw.y(), sw.width(), sw.height(), 3.0f);
	}
	else {
		canvas.roundedRectangleBorder(sw.x(), sw.y(), sw.width(), sw.height(), 3.0f, 1.0f);
	}

	if (folded_) {
		// FOLDED IS A WHOLE DRAWING, the rule the rails follow: the head and nothing else, so what is
		// left says what the card is rather than showing a cut-off version of it.
		// CLEAR OF THE HEAD'S BUTTONS — the enable square is the leftmost of them, so the label's box
		// ends there. It used to run to `width() - kPad - 20`, which is to the RIGHT of the square's
		// left edge, so a right-aligned type name printed straight over it.
		canvas.setColor(kDisabled);
		const visage::Bounds label = foldedLabelBounds(enableBounds().x());
		canvas.text(flt::kTypeNames[static_cast<size_t>(selectedType())], font(kKnobReadoutSize),
		            visage::Font::kRight, label.x(), label.y(), label.width(), label.height());
		return;
	}

	// The type row: the dropdown box drawn by hand, the way the header bar draws its closed selector,
	// with an arrow at each end.
	const visage::Bounds menu = typeMenuBounds();
	canvas.setColor(stated(withAlpha(kLine, 0.9f)));
	canvas.roundedRectangleBorder(menu.x(), menu.y(), menu.width(), menu.height(), 3.0f, 1.0f);
	canvas.setColor(stated(kInk));
	canvas.text(flt::kTypeNames[static_cast<size_t>(selectedType())], font(kControlSize),
	            visage::Font::kCenter, menu.x(), menu.y(), menu.width(), menu.height());

	// THE SLOPE BOX, at the right of the type row because a slope is an attribute of the type. Drawn
	// the same way the dropdown is — a hand-drawn box with the value in it — and GREYED, not removed,
	// where the type declares only one: a box that vanished would move the row under the pointer as
	// the dropdown changed, and greyed says the true thing, which is that this type has one slope.
	{
		const visage::Bounds box = slopeBoxBounds();
		const bool live = slopeIsLive();
		canvas.setColor(stated(withAlpha(kLine, live ? 0.9f : 0.4f)));
		canvas.roundedRectangleBorder(box.x(), box.y(), box.width(), box.height(), 3.0f, 1.0f);
		canvas.setColor(stated(live ? kInk : kDisabled));
		// THE SLOPE THAT WILL RUN, not the one that is stored. A bell holding a stored 24 dB runs one
		// section, and a greyed box reading "24 dB" over a filter running 12 would be the card telling
		// a lie it cannot be caught in — the same fault as drawing the curve at a corner the engine is
		// not at. Found by photographing the card; every geometry and text case passed it.
		const auto effective = flt::effectiveSlope(static_cast<flt::Type>(selectedType()),
		                                           static_cast<flt::Slope>(selectedSlope()));
		canvas.text(flt::kSlopeNames[static_cast<size_t>(effective)], font(kKnobReadoutSize),
		            visage::Font::kCenter, box.x(), box.y(), box.width(), box.height());
	}

	for (int direction : {-1, 1}) {
		const visage::Bounds box = arrowBounds(direction);
		// GEOMETRY RATHER THAN A GLYPH, which is A16's lesson: this font has no such character and a
		// missing glyph is an invisible control. Dimmed when there is nothing that way, which is the
		// honesty rule — an arrow that cannot move should not look like one that can.
		const bool live = typeAfter(selectedType(), direction) >= 0;
		// Dimmed THROUGH the live/dead colour rather than instead of it, so a switched-out card still
		// says which way the arrows can walk: the two are one step apart at either brightness.
		canvas.setColor(stated(live ? kMuted : kDisabled));
		const float cx = box.x() + box.width() * 0.5f;
		const float cy = box.y() + box.height() * 0.5f;
		const float reach = 3.5f;
		const float tip = cx + reach * static_cast<float>(direction);
		const float back = cx - reach * static_cast<float>(direction);
		canvas.segment(tip, cy, back, cy - reach, 1.6f, true);
		canvas.segment(tip, cy, back, cy + reach, 1.6f, true);
	}

	// THE CURVE.
	const visage::Bounds box = curveBounds();
	canvas.setColor(kVoid);
	canvas.roundedRectangle(box.x(), box.y(), box.width(), box.height(), 3.0f);

	// The 0 dB line, so a shelf's boost and a lowpass's roll-off are read against something rather
	// than against the middle of a box.
	const float unity = box.y() + box.height() * (kCurveTopDb / (kCurveTopDb - kCurveBottomDb));
	canvas.setColor(stated(withAlpha(kLine, 0.7f)));
	canvas.fill(box.x(), unity, box.width(), 1.0f);

	// AND THE CARD SAYS WHEN THE CURVE IS A STAND-IN. This device's promise is that the curve IS the
	// filter that plays, and for a nonlinear model that cannot be true — there is no single magnitude
	// response to draw. Three such models landed declaring themselves drawable anyway, with a comment
	// asserting "the header says so" over a header that said nothing. It says so now: one word, in the
	// disabled ink, in the corner of the box it qualifies.
	if (!flt::traitsOf(static_cast<flt::Type>(selectedType())).drawable) {
		const visage::Font& micro = font(kKnobReadoutSize);
		canvas.setColor(stated(kDisabled));
		canvas.text("proxy", micro, visage::Font::kRight, box.x(), box.y() + 3.0f,
		            box.width() - 6.0f, 12.0f);
	}

	// A FIXED BUFFER RATHER THAN AN ALLOCATION PER FRAME. draw() runs on every parameter change,
	// which is every frame of a knob drag, and kMaxCurvePoints of float is 8 KB of stack.
	std::array<float, kMaxCurvePoints> gainStore{};
	// AN EAR EACH (backlog KU, Q1): while a stereo route reaches the curve's controls, two curves from the modulated
	// rows — left in the left ear's colour, right in the right's — drawn by the same loop as the mono one.
	const bool stereo = stereoCurves();
	bool tooFew = false;
	for (int ear = 0; ear < (stereo ? 2 : 1); ++ear) {
		const size_t points = curveGains(gainStore.data(), gainStore.size(), ear == 1);
		const std::span<const float> gains(gainStore.data(), points);
		if (points < 2) {
			tooFew = true;
			break;
		}
		const auto pointAt = [&](size_t i) {
			const float t = static_cast<float>(i) / static_cast<float>(points - 1);
			const float db = std::clamp(dbOf(gains[i]), kCurveBottomDb, kCurveTopDb);
			const float y = box.y() + box.height() * (kCurveTopDb - db) / (kCurveTopDb - kCurveBottomDb);
			return visage::Point{box.x() + box.width() * t, y};
		};

		// COLOUR QUANTISED INTO BANDS, and only pushed when the band changes — wave_display.cpp:305-308,
		// copied along with its reason: a per-point colour puts one gradient-atlas entry per point on
		// every redraw, and a redraw happens on every frame of a knob drag.
		int lastBand = -1;
		for (size_t i = 1; i < points; ++i) {
			const float t = static_cast<float>(i) / static_cast<float>(points - 1);
			const int band = static_cast<int>(std::lround(t * (kColourBands - 1)));
			if (stereo) {
				if (band != lastBand) {
					lastBand = band;
					canvas.setColor(stated(withAlpha(ear == 0 ? kModulationL : kModulationR, 0.9f)));
				}
			} else if (band != lastBand) {
				lastBand = band;
				const float banded = static_cast<float>(band) / static_cast<float>(kColourBands - 1);
				// Low frequencies at the accent, high at the value colour, BOTH WITH THREE QUARTERS OF THEIR
				// CHROMA REMOVED — so the line reads as grey and the cool-to-warm drift is left as a hint
				// about which end is which rather than as two semantic colours spent on a graph. And the
				// whole curve dimmed while the device is switched out: a picture of a filter that is not in
				// the signal should not look like one that is.
				const uint32_t hue = blendColour(desaturate(kAccent, kCurveDesaturation),
				                                 desaturate(kValue, kCurveDesaturation), banded);
				canvas.setColor(stated(hue));
			}
			const visage::Point from = pointAt(i - 1);
			const visage::Point to = pointAt(i);
			// EVERY COMPUTED POINT FINITE-CHECKED BEFORE THE RENDERER — wave_display.cpp:341-343. These
			// coordinates come from an evaluated response rather than from a clamped parameter, which is
			// exactly the case that rule exists for; a non-finite one would hand the renderer a garbage
			// quad and there is no loop condition here to stop it.
			if (!std::isfinite(from.y) || !std::isfinite(to.y)) {
				continue;
			}
			canvas.segment(from.x, from.y, to.x, to.y, 1.5f, true);
		}
	}
	if (tooFew) {
		return; // as the mono picture always has: no curve, and nothing drawn after it
	}

	// The macro labels follow the dropdown while their ids never move, so they are re-read here rather
	// than set once at construction — as does WHETHER THE KNOB IS THERE AT ALL.
	//
	// A TYPE THAT DECLARES NOTHING IN A MACRO SLOT GETS NO KNOB, rather than a knob that is drawn and
	// disabled or drawn without a label. The honesty rule wants an absent control, not a present and
	// inert one: a knob under the hand's cursor that does nothing teaches the hand that knobs on this
	// card might do nothing. Five types have an empty slot since Spread became a param of its own,
	// and Bell has one because it cannot be a morph partner in either direction.
	if (knobs_.size() >= 6) {
		syncMacroKnobs();
		knobs_[4]->setLabel(macroLabel(0));
		knobs_[5]->setLabel(macroLabel(1));
	}
}

void FilterCard::syncMacroKnobs() {
	if (knobs_.size() < 6) {
		return;
	}
	const flt::TypeTraits& traits = flt::traitsOf(static_cast<flt::Type>(selectedType()));
	const bool showFirst = !folded_ && traits.macro1.kind != flt::MacroKind::None;
	const bool showSecond = !folded_ && traits.macro2.kind != flt::MacroKind::None;
	knobs_[4]->setVisible(showFirst);
	knobs_[5]->setVisible(showSecond);
	// AND SPREAD VANISHES ON THE TYPES IT DOES NOT REACH — the same treatment the macro slots get, for
	// the same reason. Spread is a param rather than a macro, so it was written as the one row-B knob
	// that "applies to every type and therefore never vanishes"; that stopped being true the day the
	// Camp B models landed, none of which runs the cascade the spread stage mixes over. It was found
	// by turning it. Hidden rather than greyed because the row already hides its other two, so a gap
	// is the vocabulary this row speaks.
	knobs_[3]->setVisible(!folded_ && flt::spreadSupported(traits));
	// AND RESONANCE, on the one type whose shape is its design (backlog NL's Brickwall HP: the elliptic's Qs
	// are set by its ripple and stopband, so the knob would turn and change nothing).
	knobs_[1]->setVisible(!folded_ && flt::resonanceSupported(traits));
}

void FilterCard::offeredControls(std::vector<Offered>& out) const {
	FxCard::offeredControls(out);
	// THE TYPE AND SLOPE SELECTORS this card draws itself: each IS its control.
	out.push_back({ &control(flt::Param::Type), Road::Face });
	out.push_back({ &control(flt::Param::Slope), Road::Face });
}

} // namespace feathers::ui
