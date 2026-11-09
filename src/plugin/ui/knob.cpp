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

#include "plugin/ui/knob.h"

#include "plugin/ui/menu_anchor.h"
#include "plugin/ui/mod_route_menu.h"

#include "plugin/ui/value_box.h"

#include "plugin/ui/theme.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>

namespace feathers::ui {

namespace {

constexpr float kPi = 3.14159265358979323846f;

/// The mock's arc: 270° of travel with the gap at the bottom, so the middle of the track points
/// straight up and the ends sit at seven and five o'clock.
constexpr float kHalfSweep = 0.75f * kPi;

/// Straight up, in visage's arc angles — VERIFIED BY RENDERING ONE rather than reasoned about, and
/// worth the trouble: the first guess put the gap on the left. visage describes an arc as a centre
/// angle and a half-extent, and its angles are ordinary screen angles — zero points at three
/// o'clock, positive runs clockwise, y grows downward. The middle of a knob's travel, which the
/// mock puts at twelve, is therefore a quarter turn back from zero. The pointer below is in that
/// same convention, which is what lets the two agree at every value, not only at the midpoint.
constexpr float kArcUp = -0.5f * kPi;

/// The knob's own geometry, from the mock's 52px face: a pointer between these radii.
constexpr float kPointerInner = 9.0f;
constexpr float kPointerOuter = 15.0f;
constexpr float kPointerWidth = 2.0f;

/// The mock's `ctx.lineWidth=5` for the amount, against the track's 3: fatter than what it sits on, so
/// it is legible where it overlaps the value arc rather than hiding under it.
constexpr float kModRingWidth = 5.0f;

} // namespace

Knob::Knob(std::unique_ptr<OwnedControl> control, const char* label, Notifier* notifier)
    : owned_(std::move(control)), label_(label), notifier_(notifier) {
	// A NULL CONTROL STAYS A REAL STATE, and it is the caller's to express. Handing over nothing
	// leaves control_ null, so every `control_ == nullptr` guard below reads as "there is nothing
	// behind this knob", which the face draws as a bare track. The parameter road used to spell this
	// as a null descriptor; it now spells it as no control at all, which is the same fact without
	// this file having to know what a descriptor is.
	// OWNED IS NOT BOUND — see OwnedControl::bound. A knob over a null descriptor owns a control that
	// answers "unbound" and must still draw as a bare track, so control_ stays null while owned_ does
	// not.
	control_ = (owned_ != nullptr && owned_->bound()) ? owned_.get() : nullptr;

	// NOTHING IS SUBSCRIBED HERE ANY MORE. Redraw subscription was keyed by clap_id and is the
	// caller's, beside the control it built.
	buildMouse();
}

Knob::Knob(device::Modulatable& control, const char* label, Notifier* notifier)
    : control_(&control), label_(label), notifier_(notifier) {
	// NO OWNED CONTROL. A device's control outlives its card's widgets, and whoever built the device
	// repaints them through the same ModState this knob is later handed.
	buildMouse();
}

void Knob::buildMouse() {
	// SHARED BY BOTH ROADS. Every one of these handlers now speaks only to control_, so there is
	// nothing in them that knows whether it is driving a host parameter or a device's control — which
	// is the entire point of the split and the reason they could be lifted out of one constructor
	// unchanged.
	// A press opens nothing on its own. Unlike the debug row, where a click jumps the parameter to
	// where it landed, a knob's press is only the anchor a drag is measured from — clicking a knob
	// and not moving it must leave the sound exactly as it was, which is also what makes the
	// double-click below safe to spend a click on.
	// THE GLOW FOLLOWS THE MODIFIER BY WAY OF THE HAND'S OWN MOVEMENT. visage stamps every mouse
	// event with the modifier state the server holds at that moment, and on X11 it translates no
	// modifier KEY at all — so motion over a knob is the only road a held Alt has to the draw path.
	// See ModState::bypassed for the whole of it, and haloLit() for what reads the flag.
	onMouseEnter() = [this](const visage::MouseEvent& e) { feedBypass(e); };
	onMouseMove() = [this](const visage::MouseEvent& e) { feedBypass(e); };
	onMouseExit() = [this](const visage::MouseEvent& e) { feedBypass(e); };

	onMouseDown() = [this](const visage::MouseEvent& e) {
		// THE PRESS FEEDS THE FLAG TOO, before its own gate is asked: a press is a motion event that
		// may not have had a move before it, and the glow should agree with the road the press takes
		// in the same frame.
		feedBypass(e);
		// A GREYED KNOB TAKES ONLY A ROUTE (backlog QK, by design: greyed controls glow, dimmer) — it glows for an
		// armed source, so the press that glow promises is honoured; its VALUE still takes no hand.
		if (control_ == nullptr || (!usable() && !mappable(e.isAltDown()))) {
			return;
		}
		// A PRESS ON THE KNOB WHILE ITS FIELD IS OPEN is the house's click-elsewhere: the field gives the
		// value back (`ui/type_in.h`), and the press goes on to be whatever it is.
		const bool closedField = typeIn_.closeIfOpen();
		const int clicks = e.repeatClickCount();
		// **THE INBOUND-ROUTE MENU, TESTED FIRST BECAUSE IT IS A DIFFERENT GESTURE ENTIRELY.** A
		// right-click is not a quiet variant of a drag: it must not begin one, must not move a value,
		// and must not be swallowed by the arm road below. Backlog DI: a right-click on a modulated
		// param shows its sources, and selecting a source there is the same as entering mod mode for
		// that source, so the amount can be fine-tuned from the destination.
		// ONE MENU FOR THE ONE GESTURE (host slots T7): the routes or the macro's own rows, and after them
		// the host rows — so a control with nothing routed still answers a right-click with "Expose to host".
		if (e.shouldTriggerPopup() && showContextMenu()) {
			return;
		}
		// **A TRIPLE-CLICK TYPES** (backlog LS: double-click resets, triple-click types). visage
		// counts the clicks, so this third press follows a second that has already reset or unrouted; the
		// field opens on what that second click replaced. Nothing else happens on a third click.
		if (clicks >= 3) {
			if (!closedField) {
				(void)typeIn_.open(typeInBounds());
			}
			return;
		}
		if (clicks != 2) {
			typeIn_.disarm();
		}
		// ARMED, AND THE OVERLAY NOT BYPASSED: this drag is about how far a source reaches, not about
		// where the value sits. The mock's Opt/Alt is what lets a hand edit the parameter underneath
		// without disarming first, which is Bitwig's own behaviour and worth having for the same reason:
		// mapping is sticky, so without a bypass every value edit in a mapping session costs two clicks
		// somewhere else.
		if (mappable(e.isAltDown())) {
			if (clicks == 2) {
				typeIn_.beginReset(*control_, true);
				unroute();
				typeIn_.endReset();
				return;
			}
			beginRouteDrag(e);
			return;
		}
		if (clicks == 2) {
			typeIn_.beginReset(*control_, false);
			resetToDefault();
			typeIn_.endReset();
			return;
		}
		anchor(e);
		dragging_ = true;
	};

	onMouseDrag() = [this](const visage::MouseEvent& e) {
		if (routeDragging_) {
			dragRouteTo(e);
			return;
		}
		if (dragging_) {
			dragTo(e);
		}
	};

	onMouseUp() = [this](const visage::MouseEvent&) {
		if (routeDragging_) {
			routeDragging_ = false;
			// THE MOCK'S OWN RULE: an amount dragged back to nothing is not a route at 0 %, it is gone.
			// A route that stayed would keep drawing a ring and keep a row in the list for a connection
			// the hand has just taken back.
			const float amount = std::abs(routeAmount()) < device::kAmountUnroutedBelow ? 0.0f : routeAmount();
			if (amount != routeAmount()) {
				commitRoute(amount);
			}
			// The control closes the gesture AND writes the undo step — see ParamControl's pair. The
			// widget no longer knows how a route is addressed, which is what lets it hold a device's
			// control instead of a parameter.
			control_->endAmountGesture();
			return;
		}
		dragging_ = false;
		if (gestureOpen_) {
			control_->endGesture();
			gestureOpen_ = false;
		}
	};
	// LAST, after every `=` above — see `installTooltipHover`.
	installTooltipHover(*this, *this);
}

void Knob::setFaceDiameter(float diameter) {
	if (diameter > 0.0f && faceDiameter_ != diameter) {
		faceDiameter_ = diameter;
		redraw();
	}
}

visage::Bounds Knob::typeInBounds() const {
	// OVER THE READING, which is the number being replaced — the face stays visible above it in a column.
	if (placement_ == LabelPlacement::Right) {
		const float textX = faceDiameter_ + kGap;
		const float block = kLabelHeight + kLineGap + kReadoutHeight;
		const float top = (static_cast<float>(height()) - block) * 0.5f + kLabelHeight + kLineGap;
		return { textX, top, std::max(0.0f, static_cast<float>(width()) - textX), kReadoutHeight };
	}
	return { 0.0f, faceDiameter_ + kGap + kLabelHeight + kLineGap, static_cast<float>(width()), kReadoutHeight };
}

void Knob::resized() {
	// UNCONDITIONAL, for the scale rule — `place` does nothing visible before the first triple-click.
	typeIn_.place(typeInBounds());
}

void Knob::setLabel(const char* label) {
	if (label == nullptr || label_ == label) {
		return;
	}
	label_ = label;
	redraw();
}

void Knob::setEnabled(bool enabled) {
	if (enabled_ != enabled) {
		enabled_ = enabled;
		redraw();
	}
}

void Knob::setActive(bool active) {
	if (active_ != active) {
		active_ = active;
		redraw();
	}
}

float Knob::normalized() const {
	// THE INTERFACE'S OWN ARITHMETIC. Modulatable::normalized is non-virtual for exactly this reason:
	// there is one right way to put a value on a 0..1 sweep and no reason for two of them.
	if (control_ == nullptr) {
		return 0.0f;
	}
	// A CONTROL WITH A FACE OF ITS OWN SAYS WHERE ITS POINTER SITS — see `OwnedControl::faceAt`.
	if (owned_ != nullptr) {
		if (const std::optional<float> face = owned_->faceAt(control_->value())) {
			return *face;
		}
	}
	return static_cast<float>(control_->normalized());
}

float Knob::modulatedFace() const {
	// THE SAME MAPPING FOR THE DISPLACEMENT MARK, or the mark would go on using the clamped sweep
	// while the pointer wrapped — two readings of one face.
	if (owned_ != nullptr) {
		if (const std::optional<float> face = owned_->faceAt(control_->display().modulated)) {
			return *face;
		}
	}
	return static_cast<float>(control_->normalizedModulated());
}

float Knob::modulatedFaceR() const {
	if (owned_ != nullptr) {
		if (const std::optional<float> face = owned_->faceAt(control_->display().modulatedR)) {
			return *face;
		}
	}
	return static_cast<float>(control_->normalizedModulatedR());
}

void Knob::anchor(const visage::MouseEvent& e) {
	anchorY_ = e.position.y;
	anchorValue_ = normalized();
	// AND THE VALUE ITSELF, for a control geared in its own units (`OwnedControl::dragSpan`): a
	// wrapping face's 0..1 position cannot be dragged from, because it is not monotone in the value.
	anchorRaw_ = control_ != nullptr ? control_->value() : 0.0;
	anchorFine_ = e.isShiftDown();
}

void Knob::dragTo(const visage::MouseEvent& e) {
	// Taking the modifier up or down mid-drag re-anchors rather than rescaling what came before.
	// Without this the value jumps the moment Shift is pressed, because the travel already covered
	// would suddenly be worth a fifth of what it was.
	if (e.isShiftDown() != anchorFine_) {
		anchor(e);
	}

	const float scale = anchorFine_ ? kFineFactor : 1.0f;
	const float travel = (anchorY_ - e.position.y) / kDragRange * scale;
	if (owned_ != nullptr) {
		// A CURVED FACE IS DRAGGED ALONG THE FACE — see `OwnedControl::valueAtFace`.
		if (const std::optional<double> curved =
		        owned_->valueAtFace(std::clamp(anchorValue_ + travel, 0.0f, 1.0f))) {
			commit(*curved);
			return;
		}
		if (const std::optional<double> span = owned_->dragSpan()) {
			// GEARED IN THE VALUE'S OWN UNITS AND NOT CLAMPED HERE — the control knows what is really
			// a limit. Absolute from the grab, like the ordinary road, so no rounding compounds.
			commit(anchorRaw_ + static_cast<double>(travel) * *span);
			return;
		}
	}
	const float t = std::clamp(anchorValue_ + travel, 0.0f, 1.0f);
	const device::ParamDesc& desc = control_->desc();
	commit(desc.min + static_cast<double>(t) * (desc.max - desc.min));
}

void Knob::resetToDefault() {
	// THE GESTURE ITSELF NOW LIVES ON THE CONTRACT, so the OWLTT graph's bars and every future
	// draggable thing perform the identical edit — same default, same bracketing, same free no-op —
	// instead of each reimplementing it beside its own drag. What stays here is what is a KNOB'S
	// business rather than the gesture's: saying so, and repainting.
	//
	// It reports whether it changed anything, which is what keeps the announcement honest without
	// asking the same question twice. The old form tested `value() == def` here and would have gone
	// on announcing "reset" if the shared rule ever grew a second reason to decline.
	if (control_ == nullptr || !control_->resetToDefault()) {
		return;
	}
	announce("reset");
	redraw();
}

void Knob::commit(double value) {
	if (control_ == nullptr || control_->value() == value) {
		return;
	}
	// The gesture opens on the first value that actually moves, not on the press, so a click that
	// does not turn the knob spends no undo step at all.
	if (!gestureOpen_) {
		control_->beginGesture();
		gestureOpen_ = true;
	}
	control_->setValue(value);
	announce();
	redraw();
}

// `~Knob` IS GONE, AND ITS ABSENCE IS THE POINT. It existed to call `mod_->unwatch(this)` — the rule
// this file taught the rest of the editor. `Watch` now holds that subscription and releases it, so
// there is nothing left for a destructor to remember. See `ui::Watch`.

std::vector<device::Source> Knob::inboundSources() const {
	// **DELEGATED, NOT DUPLICATED.** This is generic over anything that implements modulation:
	// `mod_route_menu.h`. What is left here is the widget's own API surface, kept
	// so a case can drive the gesture through the thing a hand touches.
	return control_ != nullptr ? ui::inboundSources(*control_) : std::vector<device::Source> {};
}

void Knob::chooseInbound(device::Source source) {
	if (control_ != nullptr) {
		ui::chooseInbound(*control_, source);
	}
}

size_t Knob::removeAllInbound() {
	return control_ != nullptr ? ui::removeAllInbound(*control_) : 0;
}

bool Knob::showRouteMenu() {
	return control_ != nullptr && ui::showRouteMenu(*this, *control_);
}

void Knob::setMacroMenu(MacroMenuHooks hooks) {
	macroMenu_ = std::move(hooks);
}

bool Knob::showMacroMenu() {
	// THE MENU IS `macro_menu.h`'s; the knob only anchors it. A knob that is not a macro holds
	// incomplete hooks and is refused there, so the press goes on to its ordinary road. Placement and
	// the re-click toggle are `showMenuAt`'s, reached from there.
	return ui::showMacroMenu(*this, macroMenu_);
}

std::optional<visage::PopupMenu> Knob::contextMenu() {
	if (control_ == nullptr) {
		return std::nullopt;
	}
	// AFTER A HOST VERB THAT MOVED SOMETHING: the corner mark follows at once, and a slot taken from a used
	// Recent is SAID (design §2: the last resort is announced) — whatever lane the host kept on it now
	// drives this control. Reached only while `alive_` holds, so `this` is safe here.
	HostMenuChanged changed = [this](const std::string& displaced) {
		if (!displaced.empty() && notifier_ != nullptr) {
			const device::HostSlotStatus now = control_->hostSlot();
			notifier_->say("Host slot " + std::to_string(now.slot + 1) + " taken from " + displaced);
		}
		redraw();
	};
	// THE ROUTES FIRST, AS BEFORE: a macro is never a destination, so the two cannot compete for one press.
	if (!ui::inboundSources(*control_).empty() || !macroMenu_.complete()) {
		return ui::controlMenu(*control_, alive_, std::move(changed));
	}
	visage::PopupMenu menu = ui::macroMenu(macroMenu_);
	// A MACROS CARD'S KNOB IS A DEVICE CONTROL, so it can be exposed like any other; a header macro is
	// already a host parameter, answers `Unavailable`, and gets its macro rows alone.
	// With no host rows the macro pick stays unwrapped: it holds the hooks by value and reaches no widget.
	(void)ui::appendHostRows(menu, *control_, alive_, std::move(changed));
	return menu;
}

bool Knob::showContextMenu() {
	std::optional<visage::PopupMenu> menu = contextMenu();
	if (!menu.has_value()) {
		return false;
	}
	// THE ONE ROAD EVERY DROPDOWN OPENS BY (`menu_anchor.h`); a re-click on an open menu closes it, and the
	// press is the menu's either way.
	(void)showMenuAt(std::move(*menu), *this, { 0.0f, 0.0f, width(), height() });
	return true;
}

bool Knob::hostMarked() const {
	if (control_ == nullptr) {
		return false;
	}
	// CACHED ON THE POOL'S GENERATION (T7 review): `hostSlot()` walks every slot, and this runs on every paint.
	// The generation moves on every mapping and state change, so an unchanged one is an unchanged answer.
	const uint64_t generation = control_->hostSlotGeneration();
	if (!hostMarkCache_.valid || hostMarkCache_.generation != generation || hostMarkCache_.control != control_) {
		const device::HostSlotStatus::State state = control_->hostSlot().state;
		hostMarkCache_ = { generation, control_,
			               state == device::HostSlotStatus::State::Pinned
			                   || state == device::HostSlotStatus::State::Anchored,
			               true };
	}
	return hostMarkCache_.marked;
}

void Knob::setModulation(ModState* state, EditHistory* history) {
	// RE-POINTED, NOT ADDED TWICE. A knob handed a second state would otherwise stay on the first
	// one's list for ever — registered with something nobody will ever unregister it from, which is
	// the same dangling pointer arriving by a different door.
	// NO EXPLICIT UNWATCH ON A RE-POINT EITHER: assigning the new token below releases the old
	// subscription. The comment this replaces warned that a knob handed a second state "would
	// otherwise stay on the first one's list for ever" — now it cannot.
	mod_ = state;
	// NOT STORED, ONLY PASSED ON — see the header. The reader this used to have moved into the
	// adapter, and a field that looks wired and is not tells the next reader that undo already
	// reaches this class, which is the opposite of true.
	// The owned control needs it too — it is what answers armed, the amount and the routable gate.
	// A knob built over somebody else's Modulatable does not: that control was handed its own state
	// by whoever built it, which is the point of it being somebody else's.
	if (owned_ != nullptr) {
		owned_->setModulation(state, history);
	}
	if (mod_ != nullptr) {
		// The glow IS the map-mode signal, so it has to arrive in the frame the flag moves in — the
		// mock repaints immediately for exactly this reason.
		//
		// **ASSIGNING THE TOKEN RELEASES THE PREVIOUS ONE**, which is the re-point above expressed as
		// ownership instead of as a remembered pair of calls. The explicit `unwatch` that used to sit
		// at the top of this function is gone: move-assignment does it, and cannot be forgotten.
		watch_ = mod_->watch(this);
	}
}

bool Knob::isModDestination() const {
	return control_ != nullptr && control_->display().isDestination;
}

bool Knob::mappable(bool altDown) const {
	// GREYED (disabled or not in the sound) IS STILL A TARGET (QK, by design): only a control that changes nothing WHERE
	// IT STANDS (`unavailable`, a Poly switch inside a Rack) is not one.
	if (control_ == nullptr || altDown || control_->unavailable() != nullptr) {
		return false;
	}
	// ARMED, A DESTINATION, AND THE ARMED SOURCE ALLOWED TO REACH IT — one question, asked once, of
	// the control. This used to be three tests here plus a maySourceRouteTo call, and the value box
	// asked only the first two, which is how the same gate came to have two answers. ModDisplay::
	// routable is now the single answer both read; see the note there.
	return control_->display().routable;
}

bool Knob::haloLit() const {
	// SWITCHED OFF FOR EITHER REASON, drawn the one way — see draw(). And the BYPASS read from the
	// shared state rather than from an event, because this is asked from the draw path, which has
	// none: the flag is the last modifier any knob's hand carried, fed by feedBypass.
	// GREYED OR NOT, LIT WHEN THE PRESS WOULD ROUTE (QK, by design: greyed controls glow dimmer); `halo` says
	// how bright. It went dark on a greyed knob, which hid a real, routable destination from the hand.
	const bool bypassed = mod_ != nullptr && mod_->bypassed();
	return mappable(bypassed);
}

Knob::Halo Knob::halo() const {
	if (!haloLit()) {
		return {};
	}
	return { true, control_->display().stereo, !(usable() && active_) };
}

void Knob::feedBypass(const visage::MouseEvent& e) {
	if (mod_ != nullptr) {
		// The return — how many frames were told — is the repaint's only witness, and it is what the
		// bypass case reads. Nothing here needs it: a flag that did not move tells nobody, by design.
		mod_->setBypass(e.isAltDown());
	}
}

float Knob::routeAmount() const {
	return control_ == nullptr ? 0.0f : control_->display().amount;
}

Knob::ModRing Knob::modRing() const {
	ModRing ring;
	if (control_ == nullptr) {
		return ring;
	}
	const device::ModDisplay state = control_->display();

	// TWO DIFFERENT QUESTIONS, AND ONLY ONE OF THEM IS ABOUT ROUTING. This used to open by refusing
	// any parameter that is not an internal-routing DESTINATION, which is right for the reach arcs
	// below — they are drawn from a route's amount, and a param with no destination index has no route
	// to draw — and wrong for the live reading, which is simply "where is this parameter actually
	// sitting", and is a fair question about anything a host can modulate.
	//
	// A MACRO IS A SOURCE, NOT A DESTINATION, so it fell at that first line and could not draw a
	// modulation arc AT ALL, whatever the host did to it: with a modulated signal routed into one of
	// the internal macros, the value arc on the macro knob did not move. It could not:
	// the widget returned an empty ring before reading a single value. The engine had it right all
	// along — ParamSet::sourceValue reads resolved(macro), so a host modulating a macro really does
	// reach that macro's destinations — which is what made this look like a routing bug rather than a
	// drawing one.
	// ASKED OF THE CONTROL, NOT OF THIS WIDGET'S OWN ModState POINTER. It used to read
	// `mod_ != nullptr && isModDestination()`, which was true enough while every knob's routing state
	// arrived through setModulation — and wrong the moment a knob could be built over somebody else's
	// control, because a DEVICE's control carries its own state and the widget's pointer stays null.
	// A knob over a rack control drew no ring at all, whatever was routed to it.
	//
	// Nothing changes on the parameter road: with no ModState anywhere, the control reports armed
	// false and amount zero, so this falls through to the live reading exactly as before — which is
	// the reading a macro knob needs, and the one that was missing.
	const bool destination = state.isDestination;

	// THE RING IS DRAWN IN THE FACE'S OWN FRAME, which is not always the parameter's. See
	// `faceZoneSpan()`: a zone knob sweeps one zone per rotation, so its ring has to be measured in
	// zones-of-one rather than in the whole range, or the arc drifts away from the pointer.
	const float base = normalized();
	// A ZONED DESTINATION'S AMOUNT IS STATED IN ZONE WIDTHS, so the arc has to be scaled the same way
	// the engine scales the offset — `ParamSet::rackOffsetAt`, the other reader of
	// `device::ParamDesc::zones`. `params.cpp` says the invariant this bends out loud: the
	// destination's range is the unit an amount is stated in, *"which is what lets the ring on the
	// face be drawn from the amount without asking what the parameter is."* A zone control is the one
	// case that has to ask — and a ring that did not would draw a full-range reach over a one-zone
	// modulation, which is a picture disagreeing with the sound rather than a cosmetic error.
	//
	// ZERO FOR EVERY OTHER CONTROL, so this is the identity everywhere else: host parameters reach
	// this through an adapter whose descriptor declares no zones.
	// AMOUNT IS IN ZONE WIDTHS; THE FACE IS IN FACE UNITS. The conversion is one over however many
	// zones the face spans — `zones` for a face showing the whole range, ONE for a zone knob's, and
	// one for a control with no zones at all, where the amount is already in full ranges.
	//
	// The rule this implements: for zone knobs, any modulation is relative, and the valid range for
	// a modulation is approximately one zone, not the entire knob range. On a zone-local face that
	// makes a full-amount route a full sweep of the
	// dial, which is the honest picture of "one zone".
	const float amount = destination ? state.amount / faceZoneSpan() : 0.0f;
	// **A BIPOLAR SOURCE REACHES BOTH WAYS FROM THE REST POSITION.** `SourceDesc::bipolar` says a
	// source swings either side of centre — the DAHDSR and the MSEG both declare it — and until now
	// NOTHING read it: `SourceView::isBipolarAt` was its only reader, nothing called it, and nothing in
	// the repo constructed a `SourceView` at all. `sources.h` states the flag's purpose as *"so a
	// surface can draw its rest position"*; no surface did, so every ring drew a one-sided reach as
	// though every source were unipolar.
	//
	// **READ FROM THE DISPLAY, BESIDE `amount`**, which is where a fact about this control's modulation
	// belongs — not from the mod state, which knows the focused source and nothing about what it is.
	// ON A CURVED FACE THE REACH IS BETWEEN ITS TWO END VALUES, each put on the face by the control
	// (`OwnedControl::valueAtFace`) — the same swing is a long arc at the small end of an exponential
	// sweep and a short one at the large, and an arc of fixed length would be wrong at both.
	const bool curvedFace = owned_ != nullptr && owned_->valueAtFace(0.0f).has_value();
	const auto faceOfReach = [&](float sign) {
		const double swing = static_cast<double>(state.amount) * device::routeSpanOf(control_->desc());
		return std::clamp(owned_->faceAt(control_->value() + sign * swing).value_or(base), 0.0f, 1.0f);
	};
	const auto reach = [&](float glow) {
		// THE TWO EDGES ARE THE ROUTE'S REACH, `ModDisplay::reachLo`/`reachHi` times the amount — for a Lin
		// route the arc that has always been drawn (one-sided from the value for a unipolar source,
		// `base ± amount` for a bipolar one); for a rectified one, the half it can actually reach.
		const float hiK = state.reachHi();
		const float loK = state.reachLo();
		const auto edge = [&](float k) {
			if (k == 0.0f) {
				return base;
			}
			// A ROUTE THAT MOVES ALONG THE FACE (`OwnedControl::routeFace`) IS DRAWN BY THE CONTROL'S OWN MAPPING.
			if (owned_ != nullptr && destination) {
				if (const std::optional<float> face = owned_->routeFace(
				        static_cast<double>(k) * static_cast<double>(state.amount) * device::routeSpanOf(control_->desc()))) {
					return std::clamp(*face, 0.0f, 1.0f);
				}
			}
			return curvedFace ? faceOfReach(k) : std::clamp(base + k * amount, 0.0f, 1.0f);
		};
		const float up = edge(hiK);
		const float down = edge(loK);
		ring.lo = std::min(down, up);
		ring.hi = std::max(down, up);
		ring.glow = glow;
		ring.drawn = true;
	};

	// EACH EAR'S MARK (backlog KU): left in `mark`, right in `markR`, both always — equal ears coincide.
	const auto marksPerEar = [&] {
		ring.stereo = true;
		ring.mark = modulatedFace();
		ring.hasMark = true;
		ring.markR = modulatedFaceR();
		ring.hasMarkR = true;
		ring.drawn = true;
	};

	if (destination && state.armed) {
		// Armed and unrouted draws no ring at all — the face glow is what says "you may point here",
		// and a zero-length arc under it would be a mark with nothing to say.
		if (amount != 0.0f) {
			reach(1.0f);
		}
		// **AND WHERE THE VALUE ACTUALLY IS, WHICH THIS BRANCH USED TO RETURN WITHOUT READING.**
		// BF: the amount arc masked the displacement arc — it did, by this `return`. The
		// armed state drew the reach and left, so the live position was never computed while
		// assigning, which is exactly when a hand wants to see it. The same 0.001 threshold the
		// running branch uses, and taken from the same place, so the two states agree about what
		// counts as movement.
		const float assigning = modulatedFace();
		if (std::abs(assigning - base) > 0.001f) {
			ring.mark = assigning;
			ring.hasMark = true;
			// THE MARK IMPLIES A RING EVEN WITH NO REACH TO DRAW. An amount of zero with a source
			// moving underneath it is a real state — the route is being pointed and the value is
			// already responding — and a mark with `drawn == false` would be discarded by every
			// caller.
			ring.drawn = true;
		}
		if (state.stereoLive) {
			marksPerEar();
		}
		return ring;
	}

	// **A STEREO ROUTE REACHES IT, NOT MAPPING** (backlog KU: 2026-10-04): the as-modulated arc splits into an
	// ear each — two half-thickness arcs, each from the base to where that ear is.
	if (state.stereoLive) {
		const float liveL = modulatedFace();
		const float liveR = modulatedFaceR();
		if (std::abs(liveL - base) > 0.001f || std::abs(liveR - base) > 0.001f) {
			ring.split = true;
			ring.left = { std::min(base, liveL), std::max(base, liveL) };
			ring.right = { std::min(base, liveR), std::max(base, liveR) };
			ring.lo = std::min(ring.left.lo, ring.right.lo);
			ring.hi = std::max(ring.left.hi, ring.right.hi);
			ring.glow = 0.9f;
			ring.drawn = true;
			return ring;
		}
	}

	// A THOUSANDTH OF THE RANGE, which is PerformStage::showsModulation's own threshold and taken from
	// there rather than invented: a host that publishes a modulated reading equal to the base one in
	// all but floating-point noise must not leave a permanent second state drawn on top of the first.
	const float live = modulatedFace();
	if (std::abs(live - base) > 0.001f) {
		ring.lo = std::min(base, live);
		ring.hi = std::max(base, live);
		ring.glow = 0.9f;
		ring.drawn = true;
		return ring;
	}
	if (amount != 0.0f) {
		reach(0.28f);
	}
	if (destination && !(ring.hi > ring.lo) && (amount != 0.0f || state.routedElsewhere)) {
		ring.routedDot = true;
		ring.drawn = true;
	}
	return ring;
}

void Knob::beginRouteDrag(const visage::MouseEvent& e) {
	routeDragging_ = true;
	routeAnchorY_ = e.position.y;
	routeBefore_ = routeAmount();
	// The mock's `routeAmt(id)||.4`, committed on the press so the ring exists before the hand has
	// moved: an arm that showed nothing until the drag was under way would read as an arm that missed.
	routeAnchorAmount_ = routeBefore_ != 0.0f ? routeBefore_ : kRouteSeedAmount;
	control_->beginAmountGesture();
	commitRoute(routeAnchorAmount_);
}

void Knob::dragRouteTo(const visage::MouseEvent& e) {
	const float travel = (routeAnchorY_ - e.position.y) / kRouteDragRange;
	commitRoute(std::clamp(routeAnchorAmount_ + travel, -device::kMaxAmount, device::kMaxAmount));
}

void Knob::commitRoute(float amount) {
	if (control_ == nullptr || !control_->display().isDestination) {
		return;
	}
	// setAmount notifies the panel itself — see ParamControl::setAmount and RackControl::setAmount,
	// which both call touched() for the same reason.
	control_->setAmount(amount);
	if (notifier_ != nullptr) {
		char printed[48] = {};
		std::snprintf(printed, sizeof(printed), "%s amount %+d%%", label_,
		              static_cast<int>(std::lround(amount * 100.0f)));
		notifier_->say(printed);
	}
	redraw();
}

void Knob::unroute() {
	if (control_ == nullptr || routeAmount() == 0.0f) {
		return;
	}
	// THE CONTROL DROPS IT AND RECORDS IT. Knob used to reach past its control to the bridge and the
	// route table to do this; a device control has neither, so the whole gesture belongs to whoever
	// knows how the route is addressed.
	if (owned_ != nullptr) {
		owned_->unroute();
	}
	else {
		// `dropRoute`, not a fourth transcription of the same three lines — and NOT named `unroute`
		// on the contract, because `ParamControl::unroute` above is a different, larger gesture and
		// a shared name would have been shadowed rather than overridden. See modulatable.h.
		control_->dropRoute();
	}
	if (notifier_ != nullptr) {
		std::string message = label_;
		message += " unrouted";
		notifier_->say(message);
	}
	redraw();
}

void Knob::announce(const char* verb) {
	if (notifier_ == nullptr || control_ == nullptr) {
		return;
	}
	std::string message = label_;
	if (verb != nullptr) {
		message += ' ';
		message += verb;
		notifier_->say(message);
		return;
	}
	// THROUGH THE CONTROL, which knows how its own road prints — the host's formatter for a
	// parameter, the declared unit for a device control. See Modulatable::text.
	const std::string printed = control_->text();
	if (!printed.empty()) {
		message += ' ';
		message += printed;
		notifier_->say(message);
	}
}

void drawKnobFace(visage::Canvas& canvas, float centreX, float centreY, float diameter, float value,
                  bool enabled, bool hasValue, float rest) {
	// The mock's 52px face carries a 19px track radius; the ratio is what travels to other sizes.
	const float trackRadius = diameter * (19.0f / Knob::kDiameter);
	const float box = trackRadius * 2.0f;
	const float left = centreX - trackRadius;
	const float top = centreY - trackRadius;

	// The track: the whole 270°, always drawn, so a knob at zero still reads as a knob.
	canvas.setColor(kLine);
	canvas.arc(left, top, box, Knob::kTrackWidth, kArcUp, kHalfSweep, true);

	// The value over it. A knob with nothing behind it has no value to draw — see the header — and a
	// disabled one draws its value in the switched-off colour rather than hiding it.
	// FROM THE REST TO THE VALUE, whichever way round: an ordinary knob's rest is the bottom of the
	// sweep, a centred one's the middle (backlog EG), and the arc is the distance between them.
	const float lo = std::min(rest, value);
	const float hi = std::max(rest, value);
	if (hasValue && hi > lo) {
		canvas.setColor(enabled ? kValue : kDisabled);
		const float half = kHalfSweep * (hi - lo);
		canvas.arc(left, top, box, Knob::kTrackWidth, kArcUp - kHalfSweep + 2.0f * kHalfSweep * lo + half, half,
		           true);
	}

	// The pointer, in ordinary screen trigonometry: angle zero at three o'clock, positive clockwise
	// because y grows downward. The arc above is in visage's centre-and-extent terms instead, which
	// is why this converts rather than reusing those numbers.
	if (hasValue) {
		const float angle = -0.5f * kPi - kHalfSweep + 2.0f * kHalfSweep * value;
		const float cosA = std::cos(angle);
		const float sinA = std::sin(angle);
		const float inner = diameter * (kPointerInner / Knob::kDiameter);
		const float outer = diameter * (kPointerOuter / Knob::kDiameter);
		canvas.setColor(enabled ? kPointer : kDisabled);
		canvas.segment(centreX + cosA * inner, centreY + sinA * inner, centreX + cosA * outer,
		               centreY + sinA * outer, kPointerWidth, true);
	}
}

void drawModulationRing(visage::Canvas& canvas, float centreX, float centreY, float diameter, float lo,
                        float hi, float glow) {
	// THE SAME TRACK, WIDER: the mock draws the amount at the value arc's own radius with a fatter
	// stroke and a shadow, so the two read as one control rather than as a second dial around the
	// first. The colour is the only thing that distinguishes them, which is why periwinkle is spent on
	// modulation and on nothing else in this editor.
	const float trackRadius = diameter * (19.0f / Knob::kDiameter);
	const float box = trackRadius * 2.0f;
	const float left = centreX - trackRadius;
	const float top = centreY - trackRadius;
	const float from = kArcUp - kHalfSweep + 2.0f * kHalfSweep * lo;
	const float to = kArcUp - kHalfSweep + 2.0f * kHalfSweep * hi;

	canvas.setColor(withAlpha(kModulation, 0.6f * glow));
	canvas.arc(left, top, box, diameter * (kModRingWidth / Knob::kDiameter), (from + to) * 0.5f,
	           (to - from) * 0.5f, true);
}

void drawModulationMark(visage::Canvas& canvas, float centreX, float centreY, float diameter,
                        float at) {
	drawModulationMark(canvas, centreX, centreY, diameter, at, kModulation);
}

void drawModulationMark(visage::Canvas& canvas, float centreX, float centreY, float diameter,
                        float at, uint32_t colour) {
	// **THE DISPLACEMENT, AS A MARK ACROSS THE TRACK RATHER THAN AS A SECOND ARC ALONG IT.**
	//
	// BF: the amount arc masked the displacement arc. They were the same [lo, hi] arc in the
	// same colour at the same radius — ONE visual channel — so drawing one after the other could only
	// ever replace it. No z-order fixes two facts sharing a channel; they need two channels. The
	// amount keeps the arc, which is the right shape for a REACH, and the displacement takes a tick,
	// which is the right shape for a POSITION.
	//
	// **BRIGHTER THAN THE SPAN AND CROSSING IT**, because the span is context and the mark is the
	// live thing: it is the one the eye should find while a modulator is moving, which is the whole
	// complaint. Radially longer than the track is wide so it reads as a crossing rather than as a
	// bright spot on the arc.
	const float trackRadius = diameter * (19.0f / Knob::kDiameter);
	const float width = diameter * (kModRingWidth / Knob::kDiameter);
	const float angle = kArcUp - kHalfSweep + 2.0f * kHalfSweep * std::clamp(at, 0.0f, 1.0f);
	// visage's arcs are centre-and-half-sweep; a mark is an arc of (almost) no sweep, so it is drawn
	// as the same primitive rather than as a line the geometry would have to place by hand.
	canvas.setColor(withAlpha(colour, 0.95f));
	canvas.arc(centreX - trackRadius, centreY - trackRadius, trackRadius * 2.0f, width * 1.6f, angle,
	           0.035f, true);
}

RingEarGeometry ringEarGeometry(float diameter) {
	// THE MONO RING'S FOOTPRINT, as `drawModulationRing` strokes it: a box of radius 19/kDiameter, stroked inward by
	// the ring's width — so its centre line is half a width inside the box.
	RingEarGeometry g;
	const float box = diameter * (19.0f / Knob::kDiameter);
	g.trackWidth = diameter * (kModRingWidth / Knob::kDiameter);
	g.trackRadius = box - g.trackWidth * 0.5f;
	g.outer = { g.trackRadius + g.trackWidth * 0.25f, g.trackWidth * 0.5f };
	g.inner = { g.trackRadius - g.trackWidth * 0.25f, g.trackWidth * 0.5f };
	return g;
}

void drawModulationRingEars(visage::Canvas& canvas, float centreX, float centreY, float diameter, float loL, float hiL,
                            float loR, float hiR, float glow) {
	// TWO HALF-THICKNESS ARCS IN THE ONE FOOTPRINT — never N (there are not enough pixels in the rings). In the
	// contrasting ear colours, opaque enough to read over the marigold value arc beneath.
	const RingEarGeometry g = ringEarGeometry(diameter);
	const auto arc = [&](const RingEarGeometry::Arc& a, float lo, float hi, uint32_t colour) {
		if (hi <= lo) {
			return;
		}
		const float from = kArcUp - kHalfSweep + 2.0f * kHalfSweep * lo;
		const float to = kArcUp - kHalfSweep + 2.0f * kHalfSweep * hi;
		const float box = a.radius + a.width * 0.5f; // visage strokes inward from the box
		canvas.setColor(withAlpha(colour, std::min(1.0f, 0.95f * glow / 0.9f)));
		canvas.arc(centreX - box, centreY - box, box * 2.0f, a.width, (from + to) * 0.5f, (to - from) * 0.5f, true);
	};
	arc(g.outer, loL, hiL, kModulationL);
	arc(g.inner, loR, hiR, kModulationR);
}

float Knob::captionWidth() const {
	return trackedWidth(font(kKnobLabelSize), captionIsName_ ? std::string(label_) : upper(label_), kKnobLabelTracking);
}

void Knob::setCaptionIsName(bool isName) {
	if (captionIsName_ != isName) {
		captionIsName_ = isName;
		redraw();
	}
}

FittedName Knob::columnName(const char* label, float width) {
	// THE SAME ROOM AS `columnCaption`: less a margin each side, so two neighbours never touch.
	const float room = std::max(0.0f, width - 2.0f * kCaptionMargin);
	return fitName(label != nullptr ? label : "", room, kKnobLabelSize, kNameFitFloorSize, kKnobLabelTracking);
}

std::string Knob::columnCaption(const char* label, float width) {
	// LESS A MARGIN EACH SIDE, so two cut neighbours never touch — see `kCaptionMargin`.
	const float room = std::max(0.0f, width - 2.0f * kCaptionMargin);
	return fitTracked(upper(label), font(kKnobLabelSize), kKnobLabelTracking, room);
}

std::string Knob::drawnCaption() const {
	if (captionIsName_) {
		return columnName(label_, static_cast<float>(width())).text;
	}
	return columnCaption(label_, static_cast<float>(width()));
}

std::string Knob::fullName() const {
	if (!fullName_.empty()) {
		return fullName_;
	}
	if (control_ != nullptr && control_->desc().name != nullptr) {
		return control_->desc().name;
	}
	return label_ != nullptr ? label_ : "";
}

bool Knob::captionCut() const {
	const std::string whole = captionIsName_ ? std::string(label_ != nullptr ? label_ : "") : upper(label_);
	if (whole.empty()) {
		return false;
	}
	// A LIST ROW DRAWS THE CAPTION UNFITTED beside the face and its box clips it — `draw`'s own arithmetic.
	if (placement_ == LabelPlacement::Right) {
		const float room = std::max(0.0f, static_cast<float>(width()) - faceDiameter_ - kGap);
		return trackedWidth(font(kKnobLabelSize), whole, kKnobLabelTracking) > room;
	}
	return drawnCaption() != whole;
}

std::optional<LabelFace> Knob::labelFaceAt(visage::Point) const {
	if (label_ == nullptr || label_[0] == '\0') {
		return std::nullopt;
	}
	return LabelFace { .label = label_, .full = fullName(), .cut = captionCut() };
}

float Knob::drawnCaptionSize() const {
	return captionIsName_ ? columnName(label_, static_cast<float>(width())).size : kKnobLabelSize;
}

void Knob::drawColumnCaption(visage::Canvas& canvas, const std::string& fitted, float width, float top, float size) {
	// FITTED ALREADY, so it is always centred: the cut happened in the text, with its "...", rather than
	// at the column's edge. The clamp stays as the floor under a caller that hands over unfitted text.
	const visage::Font& face = font(size);
	const float captionWidth = trackedWidth(face, fitted, kKnobLabelTracking);
	const float captionX = std::max(0.0f, (width - captionWidth) * 0.5f);
	canvas.saveState();
	canvas.setClampBounds(0, 0, width, top + kLabelHeight);
	drawTracked(canvas, fitted, face, kKnobLabelTracking, captionX, top, kLabelHeight);
	canvas.restoreState();
}

void Knob::draw(visage::Canvas& canvas) {
	// **THE FACE IS CENTRED IN A COLUMN AND LEFT-ALIGNED IN A ROW.** See `LabelPlacement`: the same
	// widget, arranged for a grid or for a list, and the arrangement lives here rather than in
	// whichever card happens to be holding it.
	const bool listRow = placement_ == LabelPlacement::Right;
	const float centreX = listRow ? faceDiameter_ * 0.5f : width() * 0.5f;
	const float centreY = listRow ? height() * 0.5f : faceDiameter_ * 0.5f;

	// SWITCHED OFF FOR EITHER REASON, drawn the one way: a knob the hand may not move and a knob whose
	// device is out of the signal both read as off, and the difference between them is behaviour rather
	// than colour. See setActive.
	const bool lit = usable() && active_;

	// UNDER THE ARCS, not over them: the mock fills the face before it strokes the value, so the glow
	// says "this one may be pointed at" without washing out what the knob currently reads.
	//
	// AND IT GOES GREY WITH THE REST OF THE CARD — a decision, not a tidying, and the two halves of it
	// pull in opposite directions on purpose. `mappable()` keys off enabled_ and NOT active_, so the
	// AFFORDANCE survives the greying: pointing a source at a filter you have not switched on yet is
	// legitimate for the same reason dialling its cutoff is, and the drag still writes an amount. The
	// GLOW does not survive it, because a full-strength periwinkle halo sitting on an otherwise grey
	// card reads as a rendering fault rather than as an invitation — and the filter ships switched OFF,
	// so that is exactly what a default patch showed the first time anyone armed a macro.
	//
	// AND IT GOES DARK WHILE THE BYPASS IS HELD. This used to ask `mappable()` with the default — no
	// event to hand it — so an armed knob glowed "this press will route" while Alt made the press
	// edit the value (backlog CU). haloLit() reads the bypass off ModState, which the hand's own
	// motion keeps current; what the halo says and what the press does are one predicate now.
	if (const Halo h = halo(); h.lit) {
		// A STEREO-CAPABLE DESTINATION GLOWS IN ITS OWN TINT (backlog KU) — the capability, advertised for any source.
		const uint32_t tint = h.stereo ? kModulationStereo : kModulation;
		const float halo = faceDiameter_ * 0.5f;
		// A GREYED KNOB'S HALO IS DIMMER (QK): plainly lit, and plainly not the active knob beside it.
		canvas.setColor(visage::Brush::radial(
		    visage::Gradient(visage::Color(withAlpha(tint, h.dim ? kGreyedHaloAlpha : 0.55f)),
		                     visage::Color(withAlpha(tint, 0.0f))),
		    { centreX, centreY }, halo));
		canvas.circle(centreX - halo, centreY - halo, halo * 2.0f);
	}

	drawKnobFace(canvas, centreX, centreY, faceDiameter_, normalized(), lit, control_ != nullptr,
	             control_ != nullptr ? static_cast<float>(control_->restNormalized()) : 0.0f);

	if (const ModRing ring = modRing(); ring.drawn) {
		if (ring.split) {
			// THE RUNNING STEREO RING (backlog KU): an ear each, outer left, inner right.
			drawModulationRingEars(canvas, centreX, centreY, faceDiameter_, ring.left.lo, ring.left.hi, ring.right.lo,
			                       ring.right.hi, ring.glow);
		} else if (ring.hi > ring.lo) {
			drawModulationRing(canvas, centreX, centreY, faceDiameter_, ring.lo, ring.hi, ring.glow);
		}
		// THE MARK LAST, so it sits ON the span rather than under it — and the span is skipped entirely
		// when there is no reach to draw, which is the armed-with-zero-amount state where the value is
		// moving and the reach is nothing.
		if (ring.hasMark) {
			// A STEREO-LIVE CONTROL'S MARKS IN THE EAR COLOURS (backlog KU), left then right.
			drawModulationMark(canvas, centreX, centreY, faceDiameter_, ring.mark,
			                   ring.stereo ? kModulationL : kModulation);
		}
		if (ring.hasMarkR) {
			drawModulationMark(canvas, centreX, centreY, faceDiameter_, ring.markR, kModulationR);
		}
		if (ring.routedDot) {
			// LOWER LEFT, opposite the host-slot dot: dim, a footnote saying "a route reaches this".
			const float scale = faceDiameter_ / kDiameter;
			const float dot = 2.0f * scale;
			const float r = faceDiameter_ * 0.5f - dot;
			canvas.setColor(withAlpha(kModulation, 0.6f));
			canvas.circle(centreX - r * 0.70710678f - dot, centreY + r * 0.70710678f - dot, dot * 2.0f);
		}
	}

	// **KEPT ON A HOST SLOT: A DOT AT THE FACE'S UPPER RIGHT** (host slots T7). A mark, not a colour on the
	// arcs: value and modulation already own those, and a third meaning on the same stroke would be the
	// channel-sharing `drawModulationMark` exists to undo. Outside the modulation ring's reach, so nothing
	// drawn by a route covers it, and small, because it is a fact to find when looking rather than a thing
	// to look at. Pinned and Anchored share it — both stay on the host until a hand frees them; which of
	// the two is in the right-click's slot line. The accent, the mock's colour for "a hand did this".
	if (hostMarked()) {
		const float scale = faceDiameter_ / kDiameter;
		const float dot = 2.25f * scale;
		const float reach = faceDiameter_ * 0.5f - dot; // on the face's own circle, clear of the 21.5px ring
		const float x = centreX + reach * 0.70710678f;
		const float y = centreY - reach * 0.70710678f;
		canvas.setColor(lit ? kAccent : kDisabled);
		canvas.circle(x - dot, y - dot, dot * 2.0f);
	}

	// The label, and under it what the parameter actually reads. The mock prints a knob's position
	// as a percentage of its travel, which is true of a mock where every knob is 0..1 and false
	// here: Attack is a time and Gamma is signed. So this asks the parameter table, exactly as the
	// row and the stepper do, and shows the reading a host would show.
	const visage::Font& labelFace = font(kKnobLabelSize);
	canvas.setColor(lit ? kMuted : kDisabled);
	const std::string caption = captionIsName_ ? std::string(label_) : upper(label_);

	// THE TWO LINES BESIDE THE FACE, LEFT-JUSTIFIED, NAME ON TOP — the list row. They are stacked
	// against the ROW's centre rather than under the face, so a row reads as one thing whatever the
	// face's diameter happens to be.
	if (listRow) {
		const float textX = faceDiameter_ + kGap;
		const float textW = std::max(0.0f, static_cast<float>(width()) - textX);
		const float block = kLabelHeight + kLineGap + kReadoutHeight;
		const float top = (static_cast<float>(height()) - block) * 0.5f;
		drawTracked(canvas, caption, labelFace, kKnobLabelTracking, textX, top, kLabelHeight);
		if (control_ != nullptr) {
			const std::string printed = control_->text();
			if (!printed.empty()) {
				canvas.setColor(lit ? kMuted : kDisabled);
				canvas.text(printed, font(kKnobReadoutSize), visage::Font::kLeft, textX,
				            top + kLabelHeight + kLineGap, textW, kReadoutHeight);
			}
		}
		return;
	}

	// **A CAPTION THAT DOES NOT FIT IS CUT, NOT SPILLED.** Waypoint's row of three at one unit gave
	// each knob about 57px and drew `MACRO 1` at its natural width from a centred origin, so the label
	// ran into its neighbour's and the two read as one string — `MACRO 1 1MACRO 2 1` in the capture.
	// Centring is what makes the overflow symmetrical and therefore worst: it loses the START of the
	// word, which is the part that identifies it.
	//
	// SO: the start of the word is kept and the cut is SAID — the longest start that fits, then "...",
	// centred in this knob's own column (`columnCaption`). It was a bare cut at the column's edge, which
	// kept the start but read as a whole word that happened to be short; a trailing "..." says a piece is
	// missing. The real fix for a specific card is a wider cell or fewer per row — this is the floor that
	// stops any card running one caption into the next silently.
	drawColumnCaption(canvas, drawnCaption(), static_cast<float>(width()), faceDiameter_ + kGap, drawnCaptionSize());

	if (control_ == nullptr) {
		return;
	}
	// THE CONTROL PRINTS ITSELF — the host's formatter for a parameter, the declared unit for a
	// device control, and this widget does not know or care which it is holding.
	const std::string printed = control_->text();
	if (!printed.empty()) {
		canvas.setColor(lit ? kMuted : kDisabled);
		canvas.text(printed, font(kKnobReadoutSize), visage::Font::kCenter, 0.0f,
		            faceDiameter_ + kGap + kLabelHeight + kLineGap, width(), kReadoutHeight);
	}
}

//----------//
// KnobTray //
//----------//

} // namespace feathers::ui
