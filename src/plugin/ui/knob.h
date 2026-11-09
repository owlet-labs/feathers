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

#pragma once

#include "device/modulatable.h"
#include "plugin/ui/edit_history.h"
#include "plugin/ui/macro_menu.h"
#include "plugin/ui/mod_state.h"
#include "plugin/ui/tooltip.h"
#include "plugin/ui/type_in.h"
#include "plugin/ui/value_box.h"

#include <functional>
#include "plugin/ui/notify_tray.h"
#include "plugin/ui/theme.h" // `FittedName`, `kKnobLabelSize`: the caption fit

#include <memory>
#include <optional>
#include <vector>
#include <visage/ui.h>

// The mock's knob, and the footer row of them.
//
// A knob is a 270° arc from the mock's `drawKnob`: a track, a value arc over it, a pointer, a
// tracked label under it and a percentage readout under that. It answers a vertical drag, the
// convention every hardware-panel plugin uses and the one the mock chose, with a fine modifier that
// the mock does not have and a real editor needs.
//
// WHAT IS LIVE AND WHAT IS NOT, which is the honesty rule made concrete rather than a decoration.
// The tray shows the mock's nine knobs plus the master-scale Width — the fixed nine revised in the
// mock itself, V17 — and all but one move sound unconditionally: Path, Pos X, Pos Y,
// Gamma, Weather, Attack, Release and Level are read by the engine always, Width whenever the
// patch is flagship. The exceptions:
//
//   - RELIEF is a parameter the host can see, automate and save, and the engine does not read it: the
//     complexity field it belongs to is still ahead. It draws its real value and refuses the mouse.
//   - WIDTH is live exactly while the engine is V5 (by design, V11c) — on the frozen species it changes
//     nothing at any value, so its knob follows the published engine through the V12c disabled
//     idiom rather than a build-time flag. See KnobTray::update.
//   - Pos X, Pos Y and Weather were in this tier for the whole of v1 and have LEFT it. The first two
//     carry the puck off the tour, and Weather is how much of the terrain you get; Weather was not
//     even a parameter then, and drew a label over an empty track.
//
// Both are disabled and both look disabled, but the second cannot even show a value, and a knob
// that pretended otherwise would be inventing state. Hence a nullable ParamDesc rather than two
// classes: the difference is one of binding, not of behaviour.

namespace feathers::ui {

/// A CONTROL A KNOB MAY OWN — `device::Modulatable`, plus the two gestures an owned control performs
/// for itself.
///
/// IT EXISTS BECAUSE THE OWNED ROAD AND THE BORROWED ROAD DIFFER IN EXACTLY TWO PLACES. A knob built
/// over somebody else's control was handed its modulation state by whoever built it, and drops a
/// route through `Modulatable::dropRoute`; a knob that OWNS its control has to pass that state down,
/// and the owned control drops a route with a larger gesture of its own.
///
/// `Knob` used to hold a `ParamControl` and call those two directly, which is what made it know what
/// a host parameter is. Declaring them here instead keeps the knowledge on the contract: the host's
/// `ParamControl` overrides both, a device control need not exist on this road at all, and `Knob`
/// stays ignorant of which it is driving — the same split `buildMouse` already achieved for the
/// mouse handlers.
///
/// BOTH DEFAULT TO DOING NOTHING, because an owned control that has no modulation state to pass on
/// and no larger gesture to perform is a complete implementation rather than an incomplete one.
class OwnedControl : public device::Modulatable {
public:
	virtual void setModulation(ModState* state, EditHistory* history) {
		(void)state;
		(void)history;
	}
	virtual void unroute() {}

	/// IS THERE ACTUALLY SOMETHING BEHIND THIS CONTROL — false for one built over nothing.
	///
	/// IT EXISTS BECAUSE "OWNED" AND "BOUND" ARE DIFFERENT FACTS AND A KNOB NEEDS BOTH. A parameter
	/// knob built with a null descriptor still OWNS a control — that control is what answers "unbound"
	/// to everything — but it must not be exposed as this knob's control_, because every
	/// `control_ == nullptr` guard means "there is nothing behind this knob" and draws a bare track.
	///
	/// FOUND BY A SEGFAULT rather than by reading, when the two were collapsed. Making the owned road
	/// skip building a control for a null descriptor left `owned_` AND `control_` both null, so the
	/// unroute path took its `control_->dropRoute()` branch on a null pointer. The old code could not
	/// have that bug because it always built the control; this restores that, and puts the
	/// distinction on the contract where a reader can see it.
	virtual bool bound() const noexcept { return true; }

	/// **A FACE THAT IS NOT "THE VALUE ALONG THE DECLARED RANGE"** — where the pointer sits, 0..1, for
	/// a given value; `nullopt` (every ordinary control) means the declared range IS the face.
	///
	/// WHO NEEDS IT: a control over an UNBOUNDED quantity. Explore's Radius has no ceiling and its
	/// Theta is periodic, so neither has an end of travel for a bounded sweep to point at — a clamped
	/// face stops being about the sound the moment the value leaves the range, which is the dial
	/// miscalibration seen on the corner these replace (2026-09-21). Radius answers a VERNIER
	/// (one revolution per base travel, a real zero at the bottom) and Theta a COMPASS.
	///
	/// A MAPPING RATHER THAN A POSITION, because a knob draws two things with it — the pointer at the
	/// base value and the displacement mark at the modulated one — and handing back one position would
	/// have let the mark go on using the clamped arithmetic while the pointer wrapped.
	///
	/// These two hooks were chosen (option C of three) over rebuilding the modulation UI inside
	/// Explore, which would have been a second implementation of the routable gate — and `Knob::
	/// mappable` records how that gate came to have two answers the last time it had two homes.
	virtual std::optional<float> faceAt(double value) const {
		(void)value;
		return std::nullopt;
	}

	/// **WHAT ONE FULL DRAG TRAVEL IS WORTH, IN THE VALUE'S OWN UNITS** — and with it, a drag that is
	/// NOT clamped to the declared range; `nullopt` means a full travel is the declared range, clamped,
	/// as for every ordinary control. The control clamps for itself whatever really is a limit (a
	/// radius stops at zero). Asked at every drag event rather than at the grab, so a gearing that
	/// follows something live — Explore's follows the zoom — is current.
	virtual std::optional<double> dragSpan() const { return std::nullopt; }

	/// **HOW MUCH VALUE ONE FULL SWEEP OF THE FACE COVERS**, for a control whose face is its own
	/// (`faceAt`); `nullopt` means the declared range, as for every ordinary control. The modulation
	/// ring needs it: an amount is a fraction of the route's span, the ring is drawn in FACE units, and
	/// for a face that wraps the declared range says nothing about how big a sweep is.
	virtual std::optional<double> faceSweep() const { return std::nullopt; }

	/// **THE INVERSE OF `faceAt`, FOR A FACE THAT IS MONOTONE BUT NOT LINEAR** — an exponential sweep.
	/// Offering it says three things at once, which is why it is one hook: a drag moves the FACE
	/// linearly and the value follows through this (so the curve is felt under the hand, which is the
	/// point of having one); the drag is bounded by the face's two ends; and the modulation ring is
	/// drawn between `faceAt` of its two END VALUES rather than as a fixed fraction of the sweep,
	/// because on a curved face the same swing is a long arc at the small end and a short one at the
	/// large. A WRAPPING face must NOT offer it — it has no inverse — and keeps `dragSpan`/`faceSweep`.
	/// `Knob` itself still has no taper (device.h says why); the curve belongs to the control.
	virtual std::optional<double> valueAtFace(float face) const {
		(void)face;
		return std::nullopt;
	}

	/// **WHERE A ROUTE ADDING `offset` (in route-span units, `device::routeSpanOf`) PUTS THIS FACE, 0..1** — for a
	/// control whose routes move ALONG ITS FACE rather than along its value (the Radius knob, radius-mod-jump
	/// 2026-11-07). Offering it makes the modulation ring's edges this, so the control answers with the same
	/// function its engine plays the route through; `nullopt` keeps the value-space ring.
	virtual std::optional<float> routeFace(double offset) const {
		(void)offset;
		return std::nullopt;
	}
};

/// The mock's knob FACE — track, value arc, pointer — drawn at a centre, for a value 0..1.
///
/// Free rather than a member because Explore's corner encoders are knobs that are not bound to one
/// parameter: the gamma encoder follows whichever sound is selected, and zoom is not a parameter at
/// all. They must not be a second drawing of the same control. The angles below are visage's, which
/// are ordinary screen angles and were verified by RENDERING one — see the note in knob.cpp, and the
/// first guess put the gap on the left.
///
/// `hasValue` distinguishes a knob reading zero from a knob with nothing behind it: the first draws
/// its pointer at the bottom of the sweep, the second draws a bare track.
///
/// `rest` is where the value arc STARTS on the sweep — the bottom for an ordinary control, the middle
/// for a centred one (`Modulatable::restNormalized`, backlog EG): the arc runs between `rest` and
/// `value` whichever way round they fall, so a bipolar knob reads its sign as a direction from the notch.
void drawKnobFace(visage::Canvas& canvas, float centreX, float centreY, float diameter, float value,
                  bool enabled, bool hasValue = true, float rest = 0.0f);

/// The modulation arc over that face: `lo` and `hi` are positions along the same 0..1 sweep the value
/// uses, and `glow` is how loudly it is stated — the mock's three states differ in this and in their
/// endpoints, in nothing else. Free for the same reason drawKnobFace is: the panel's own small knobs
/// are not bound to a destination and must not be a second drawing of the same arc.
void drawModulationRing(visage::Canvas& canvas, float centreX, float centreY, float diameter, float lo,
                        float hi, float glow);

/// THE DISPLACEMENT MARK — where the value actually is, drawn across the amount span. See the
/// definition for why a mark and not a second arc.
void drawModulationMark(visage::Canvas& canvas, float centreX, float centreY, float diameter,
                        float at);
/// **WHERE EACH EAR'S ARC SITS** (backlog KU): the ring's own footprint cut in two along its radius — the left ear
/// outside, the right inside, each half the width, touching. Free and returned as numbers so a case can hold the
/// geometry to the rule without a picture. Every `radius` here is a stroke's CENTRE LINE: visage's `Canvas::arc`
/// strokes INWARD from its box (`shader_utils.sh` `arc`: centre at `width − thickness`, half-pixel units), so the
/// drawing adds half a width back to find each box.
struct RingEarGeometry {
	float trackRadius = 0.0f;
	float trackWidth = 0.0f;
	struct Arc {
		float radius = 0.0f;
		float width = 0.0f;
	};
	Arc outer;
	Arc inner;
};
[[nodiscard]] RingEarGeometry ringEarGeometry(float diameter);
/// THE RUNNING STEREO RING: an ear's as-modulated arc each, left outer in `kModulationL`, right inner in `kModulationR`.
void drawModulationRingEars(visage::Canvas& canvas, float centreX, float centreY, float diameter, float loL, float hiL,
                            float loR, float hiR, float glow);

/// The same mark in a given colour (backlog KU): a stereo-live control marks each ear in its own
/// (`kModulationL`/`kModulationR`).
void drawModulationMark(visage::Canvas& canvas, float centreX, float centreY, float diameter,
                        float at, uint32_t colour);

/// One knob: an arc, a label, a readout, bound to at most one parameter.
class Knob : public visage::Frame, public TooltipSource {
public:
	/// The control's declared tooltip, anywhere on the knob — see `ui/tooltip.h`.
	const char* tooltipAt(visage::Point) const override { return tooltipOf(control_); }
	/// **THE CAPTION AND THE NAME IT STANDS FOR** — `fullName()`; cut when the caption as drawn at this width (the
	/// column's fit, or a list row's clip) does not hold the whole label. See `ui::LabelFace`.
	[[nodiscard]] std::optional<LabelFace> labelFaceAt(visage::Point) const override;
	void offeredControls(std::vector<Offered>& out) const override {
		if (control_ != nullptr) {
			out.push_back({ control_, Road::Face });
		}
	}
	/// THE HALO AND THE RING (`haloLit`, `modRing`), for the glow-honesty sweep (`ui::ControlFace::modLookFor`).
	[[nodiscard]] ModLook modLookFor(const device::Modulatable& c) const override {
		if (&c != control_ || !isModDestination()) {
			return {};
		}
		const ModRing ring = modRing();
		return { true, haloLit(), ring.split || ring.hi > ring.lo || ring.hasMark || ring.routedDot };
	}

	/// **THE NAME THE CAPTION STANDS FOR** — what a hover says when the caption is shortened or cut. The control's
	/// declared name unless set: a macro's slot sets the macro's effective name ("Macro 1", or the user's own).
	void setFullName(std::string name) { fullName_ = std::move(name); }
	[[nodiscard]] std::string fullName() const;
	/// Whether the caption as drawn now does not hold the whole label (see `labelFaceAt`).
	[[nodiscard]] bool captionCut() const;

	/// `desc` may be null — see the note above about Weather. `label` is the mock's caption, which is
	/// not always the parameter's name: the mock calls GlobalGamma "Gamma" because the row has no
	/// other gamma in it.
	/// `notifier` may be null — a knob that has nowhere to announce itself still turns.
	///
	/// THE HOST-PARAMETER CONSTRUCTOR, and it is unchanged on purpose: seventeen call sites and
	/// fourteen fixtures use it. It builds a ParamControl internally and hands it to the one below.
	/// THE OWNED ROAD — the knob takes the control with it.
	///
	/// IT REPLACED A `ParamBridge` CONSTRUCTOR, and the difference is the whole of this file's half of
	/// the extraction. That one built a `ParamControl` internally, which meant `Knob` had to know what
	/// a host parameter is; this takes any `device::Modulatable` already built. A host that wants the
	/// old behaviour builds its own `ParamControl` and passes it, which is one line at five call
	/// sites and is where the knowledge belonged.
	///
	/// AND THE REGISTRY IS GONE FROM BOTH ROADS. It was keyed by clap_id, so only the parameter road
	/// could ever use it — the device road took it and wrote `(void)registry;`. Subscribing is now the
	/// caller's, beside the `ParamControl` it already owns: `registry.watch(id, knob)`.
	Knob(std::unique_ptr<OwnedControl> control, const char* label,
	     Notifier* notifier = nullptr);

	/// THE OTHER ROAD: a knob over ANY device::Modulatable, which is what a control inside a dockable
	/// device is. The knob does not own it and does not care which kind it is.
	///
	/// This is the whole reason for the split. device.h promises that a device may define its own
	/// interface and still join the shared modulation surface, and that promise was empty while the
	/// editor's most-used control could only be built over a host parameter: a swallowed filter's
	/// cutoff has no clap_id and never will. RackControl already implemented the interface, so the
	/// proof it was sufficient existed — what was missing was Knob taking it.
	/// THE BORROWED ROAD — the control outlives the knob, which is every device card.
	Knob(device::Modulatable& control, const char* label,
	     Notifier* notifier = nullptr);

	void draw(visage::Canvas& canvas) override;

	/// Rename the caption. Almost every knob in this editor is captioned once and forever; the fx
	/// filter's two macro knobs are not, because what they DO follows the type in the dropdown while
	/// their ids never move. The label is a caption and nothing else — it does not reach the parameter
	/// or the host, which still see the name the table declares.
	void setLabel(const char* label);

	/// WHAT THIS KNOB SAYS ON THE GLASS. Read-only, and it exists so a case can assert the CAPTION
	/// rather than photograph it: HARM's card carries a hard rule that no control of it may be
	/// labelled "HPF" — the firmware's unfinished rename — and a rule about words on a card is only
	/// enforceable if the words are readable from a test.
	const char* label() const noexcept { return label_; }

	/// **HOW WIDE THIS KNOB'S NAME DRAWS**, in the face and tracking it is actually drawn with — so a
	/// card sizing a cell to hold its labels asks the thing that draws them rather than estimating.
	/// The caption is upper-cased before measuring, because that is what appears.
	float captionWidth() const;

	/// **A COLUMN'S CAPTION, AS DRAWN** — `label` upper-cased and, when it is wider than `width` less
	/// `kCaptionMargin` each side, cut to the longest start that fits with "..." after it
	/// (`fitTracked`). The ONE spelling of the caption a knob column prints, so a pad standing in a knob's place (`SwitchChip`'s Pad) cuts at the same
	/// letter. Three full stops rather than U+2026, for `fitText`'s reason: this font is not promised
	/// the glyph, and a missing glyph draws nothing.
	static std::string columnCaption(const char* label, float width);
	/// **A COLUMN'S CAPTION THAT IS A NAME THE USER TYPED** (backlog OT) — as typed, no capitals forced, and shrunk a
	/// pixel at a time to `kNameFitFloorSize` before it is cut (`fitName`). The macro's caption; its text and the size
	/// it is drawn at.
	static FittedName columnName(const char* label, float width);
	/// **THIS CAPTION IS A NAME** (`columnName`), not a fixed word. A macro's slot sets it; every other knob keeps the
	/// upper-cased `columnCaption`.
	void setCaptionIsName(bool isName);
	[[nodiscard]] bool captionIsName() const noexcept { return captionIsName_; }
	/// This knob's caption at its own width — the text `draw` puts under the face (`columnName` for a name, else
	/// `columnCaption`).
	[[nodiscard]] std::string drawnCaption() const;
	/// The size `draw` sets that text in: `kKnobLabelSize`, or less for a name that was shrunk to fit.
	[[nodiscard]] float drawnCaptionSize() const;
	/// **DRAW A FITTED CAPTION CENTRED IN A COLUMN `width` WIDE** at `top`, in the knob's caption face (at `size`) and
	/// tracking, in the canvas's current colour. Both the knob and the pad draw theirs through this.
	static void drawColumnCaption(visage::Canvas& canvas, const std::string& fitted, float width, float top,
	                              float size = kKnobLabelSize);

	/// A disabled knob still draws its value — it is switched off, not blank — and stops answering
	/// the mouse. Same contract as SegmentedControl and Stepper.
	void setEnabled(bool enabled);
	bool enabled() const noexcept { return enabled_; }
	/// ENABLED HERE AND IN ITS PLACE: a control that answers `unavailable()` (a Poly switch inside a Rack) greys too.
	[[nodiscard]] bool usable() const { return enabled_ && (control_ == nullptr || control_->unavailable() == nullptr); }

	/// Whether what this knob sets is reaching the sound RIGHT NOW — false while the DEVICE it belongs
	/// to is switched out of the signal, which is the fx filter's card with its Enabled off.
	///
	/// IT DRAWS EXACTLY AS A DISABLED KNOB DRAWS and it goes on answering the mouse, and that split is
	/// the whole reason it is not the flag above. `enabled_` refuses the hand because MOVING the knob
	/// would be a lie — Relief's field does not exist yet, the frozen engine ignores Width — and
	/// nothing about a filter's cutoff is a lie while the filter is off: it is a setting the host saves
	/// and the engine will read the moment the device is switched in. So the colour is shared and the
	/// behaviour is not: grey says "not in the signal", not "hands off".
	void setActive(bool active);
	bool active() const noexcept { return active_; }

	//------------//
	// Modulation //
	//------------//

	/// Give this knob the modulation panel's state, so it can draw what reaches it and be routed.
	///
	/// SET AFTER CONSTRUCTION rather than taken in the constructor, because most knobs in this editor
	/// are built by a tray or a panel that has no business knowing about modulation, and widening a
	/// constructor that four call sites and a dozen fixtures already use would be a change to all of
	/// them to say one thing about one of them. A knob never given either pointer behaves exactly as it
	/// did before this existed.
	///
	/// AND `history` IS FORWARDED, NOT STORED — this widget keeps no `EditHistory*` of its own. It
	/// used to: `a3049d3` gave it one and recorded the amount drag's `RouteEdit` here, and `c009573`
	/// moved that down into the control adapter when a knob started addressing a
	/// `device::Modulatable`. The field outlived both its readers by three months. What the pointer
	/// is still for is the FORWARD to `owned_` — that is the road an `FxControl` gets its history on,
	/// and therefore the road the undo step for a swallowed device's control exists on at all
	/// (backlog 18). **The parameter is load-bearing; the member it used to feed was not.**
	/// **IT UNREGISTERS ITSELF, BECAUSE IT REGISTERED ITSELF.**
	///
	/// `setModulation` puts this knob on `ModState`'s watcher list — a list of RAW FRAME POINTERS that
	/// `notify()` walks. Until now nothing here took it off again: a distant OWNER had to remember
	/// `forgetFrameTree`, and five of them did while the rack did not. The result surfaced as a crash
	/// on clicking an arrow, because arming is simply the first thing that reads the list after a card
	/// has died.
	///
	/// **A THING THAT OPTS IN BY ITSELF AND OPTS OUT ONLY IF SOMEBODY ELSE REMEMBERS IS A DANGLING
	/// POINTER WITH EXTRA STEPS.** `forgetFrameTree`'s own comment says why no owner can be trusted —
	/// *"that list is joined by CHILDREN THE OWNER NEVER SEES"* — which is an argument that the owner
	/// was never the right place to put the duty.
	///
	/// THIS IS SAFE BECAUSE THE STATE OUTLIVES ITS WATCHERS, AND THAT IS BY MEMBER ORDER RATHER THAN BY
	/// CONSTRUCTION: `RackGui` declares `mod_` before `editor_`, and `Gui::Impl` declares `mod` before
	/// `modPanel` and `fxLane`. Members destruct in reverse, so widgets always go first. **Reordering
	/// either declaration would break this quietly**, which is why it is written here rather than left
	/// to be inferred.

	void setModulation(ModState* state, EditHistory* history);
	/// HOW MANY ZONES THE FACE'S SWEEP SPANS — and it is the modulation ring that needs the answer.
	///
	/// **THE BODY IS LOAD-BEARING; THE VIRTUAL WAS NOT, AND THOSE ARE DIFFERENT CLAIMS.** Its sibling
	/// `faceValue()` went with `ZoneKnob` because the FUNCTION had no purpose left. This one keeps its
	/// purpose — a device may declare `zones` and still be drawn by a PLAIN `Knob`, which is what
	/// `GenericCard` does for any device it has no card for, and the ring's scaling has to be right
	/// there too — and lost only its `virtual`, because `Knob` has no subclasses at all.
	///
	/// A ZONED DESTINATION'S ROUTE AMOUNT IS STATED IN ZONE WIDTHS (`device::ParamDesc::zones`), so
	/// drawing that amount on a face means converting zone widths into face units, and the conversion
	/// depends on how much of the range the face is showing. A face that sweeps the whole range spans
	/// every zone; a zone knob's face sweeps ONE.
	///
	/// The default answers for a face that shows everything, which is now every face this widget
	/// draws — so a control declaring zones keeps the scaling it had.
	[[nodiscard]] float faceZoneSpan() const {
		if (control_ == nullptr) {
			return 1.0f;
		}
		const device::ParamDesc& d = control_->desc();
		// **A FACE OF ITS OWN IS MEASURED IN ITS OWN SWEEPS** (`OwnedControl::faceSweep`): the ring is
		// drawn on the face, so what a full amount covers is the route's span over one sweep of it.
		if (owned_ != nullptr) {
			if (const std::optional<double> sweep = owned_->faceSweep()) {
				const double span = device::routeSpanOf(d);
				if (*sweep > 0.0 && span > 0.0) {
					return static_cast<float>(*sweep / span);
				}
			}
		}
		// **A DECLARED SPAN WINS, AND THE RING IS THE SECOND READER OF IT.** `rackOffsetAt` is the
		// first; if only one of them read `modSpan` the arc on the face would draw a different reach
		// from the one the route actually applies, which is the exact disagreement the zones comment
		// below was written about. See `device::ParamDesc::modSpan`.
		const double range = d.max - d.min;
		if (d.modSpan > 0.0 && range > 0.0) {
			return static_cast<float>(range / d.modSpan);
		}
		return d.zones > 0 ? static_cast<float>(d.zones) : 1.0f;
	}

	/// Whether this knob's parameter is one a source may reach — kModDestinations, which is the mock's
	/// own list. A knob that is not a destination never glows and never takes a route, which is the
	/// honesty rule applied to a verb rather than to a value.
	bool isModDestination() const;

	/// Whether a drag right now would edit an AMOUNT rather than the value: armed, a destination, and
	/// the Alt bypass not held.
	///
	/// THE PRESS PASSES ITS OWN EVENT'S MODIFIER; THE DRAW PATH MUST NOT USE THE DEFAULT. The default
	/// is what let the halo ignore Alt for a month (backlog CU): `draw()` has no event and asked
	/// `mappable()`, so the glow said "will route" while the press edited. `haloLit()` below is what
	/// the draw path asks now; the default stays for the cases and tools that ask about the gate
	/// without a hand on it.
	bool mappable(bool altDown = false) const;

	/// WHAT THE HALO READS — the one predicate behind the periwinkle glow, exposed so a case can
	/// assert the value the glass shows rather than infer it from a render. Armed, reachable, the
	/// bypass not held (`ModState::bypassed`), and the knob lit; a knob with no `ModState` never
	/// glows, which is the absent-feature rule every other reader of `mod_` follows.
	bool haloLit() const;

	/// WHAT THE RING DRAWS, exposed so a case can read what reached the glass rather than infer it from
	/// the values behind it — the same reason Explore records its gamma reading.
	///
	/// `lo` and `hi` are positions along the knob's own 0..1 sweep, so a case can compare them against
	/// normalized() without knowing the parameter's units.
	struct ModRing {
		float lo = 0.0f;
		float hi = 0.0f;
		float glow = 0.0f;
		bool drawn = false;

		/// **WHERE THE VALUE ACTUALLY IS INSIDE THE SPAN — the displacement, as a MARK rather than as a
		/// second arc.**
		///
		/// Backlog BF: in modulation mode it was hard to see what the modulation was doing, because the
		/// amount arc masked the displacement arc. It did, and by one `return`: the
		/// armed branch of `modRing` drew the amount and left before it read the live value at all, so
		/// while you were assigning, the thing you were assigning was invisible.
		///
		/// **LAYERING WOULD NOT HAVE FIXED IT.** Both were the same [lo, hi] arc — one visual channel,
		/// one place — so whichever was drawn second simply replaced the other. Two facts need two
		/// channels: the amount is the SPAN the route can reach, the displacement is a MARK at where it
		/// has reached. Read as a value, so a case can assert both without a capture.
		float mark = 0.0f;
		bool hasMark = false;

		/// **TWO EARS WHILE MAPPING** (backlog KU: 2026-10-04): an armed stereo-live control keeps the ONE reach
		/// arc above — the route's strength is not stereo — and marks where each ear is: `mark` the left in
		/// `kModulationL`, `markR` the right in `kModulationR`. Equal ears draw coincident marks.
		bool stereo = false;
		float markR = 0.0f;
		bool hasMarkR = false;

		/// **TWO EARS WHILE RUNNING** (not mapping): the as-modulated arc SPLITS into two half-thickness arcs, each
		/// from the base to its own ear — `left` outer, `right` inner (`ringEarGeometry`).
		struct EarArc {
			float lo = 0.0f;
			float hi = 0.0f;
		};
		bool split = false;
		EarArc left;
		EarArc right;

		/// **A ROUTE NO ARC CAN SHOW** (2026-11-07, the glow-honesty sweep): one from a source that is not focused and is
		/// still, or the focused one's pushing the value past its end (the reach clamps to nothing). `ValueBox`'s dot,
		/// for the same reason — a control must not stay silent about what is being done to it.
		bool routedDot = false;
	};

	/// **THE POTENTIAL-DESTINATION GLOW** — lit while an armed source may route here; `stereo` tints it
	/// (`kModulationStereo`) on a destination that takes a value per ear, whatever source is armed (backlog KU).
	struct Halo {
		bool lit = false;
		bool stereo = false;
		/// GREYED (disabled, or not in the sound): drawn at `kGreyedHaloAlpha` rather than the full glow (QK).
		bool dim = false;
	};
	/// **HOW BRIGHT A GREYED KNOB'S HALO IS** against the active one's 0.55 (QK, decided).
	static constexpr float kGreyedHaloAlpha = 0.28f;
	Halo halo() const;

	/// The mock's three states, in its own order (paint(), :1922-1938):
	///
	///   * ARMED — the AMOUNT, a static reach from the base, at full glow, beside the face's own glow.
	///   * RUNNING — the modulated RESULT, base → where the value actually is now, at 0.9.
	///   * AT REST — a route that exists while its source sits at zero, as a faint hint at 0.28.
	///
	/// In every one of them the POINTER AND THE PRINTED NUMBER STAY AT THE BASE. That is the mock's
	/// rule (":1171 — Never the modulated result: the value indicator stays at the base") and it is
	/// also ours for a second reason it could not have known: a readout that chased a modulator would
	/// re-rasterise its text every frame, and text churn is the family this project traced its atlas
	/// corruption to.
	ModRing modRing() const;

	/// Pixels of vertical travel for the whole ±200 % of an amount, from the mock's `/70`. Half the
	/// value drag's, because an amount has twice the range and a hand should not have to cross the
	/// window twice to state it.
	static constexpr float kRouteDragRange = 70.0f;

	/// What a press on an unrouted destination creates before the drag has moved anywhere, from the
	/// mock's `routeAmt(id)||.4`. A route that began at zero would show nothing for the first pixels of
	/// the gesture, and the hand would think the arm had not taken.
	static constexpr float kRouteSeedAmount = 0.4f;

	/// Where the parameter sits in its own range, 0..1, which is what the arc sweeps and what a drag
	/// moves. Reads the published value, never a value the drag is holding on to. A knob with no
	/// parameter reads 0.
	float normalized() const;

	/// Pixels of vertical travel for the parameter's whole range, from the mock's `/140`.
	static constexpr float kDragRange = 140.0f;

	/// What the fine modifier multiplies that travel by. Shift is the modifier every DAW's own
	/// controls use for this, so it is the one a hand already knows.
	static constexpr float kFineFactor = 0.2f;

	/// The mock's knob face, `canvas width=52 height=52`, and the arc inside it. The Engine rail's are
	/// smaller — `.insp-krow .knob canvas` is 38px — so the face is a per-knob size with the footer's
	/// as the default, and the arc scales with it.
	static constexpr float kDiameter = 52.0f;
	static constexpr float kEngineRailDiameter = 38.0f;
	static constexpr float kTrackWidth = 3.0f;

	void setFaceDiameter(float diameter);

	/// WHERE THE NAME AND THE READING SIT RELATIVE TO THE FACE.
	///
	/// **`Below` IS THE COLUMN AND `Right` IS THE LIST ROW.** The generic card puts one knob per row: the
	/// knob on the left, the name and value to its right on a line each, so two lines of text associate
	/// with the knob, the name left-justified on top. A grid of columns and a list of rows want the same widget arranged two ways, and the
	/// widget is the only thing that should know how it draws itself — a card positioning the face and
	/// printing the text beside it would be a second opinion about what a knob looks like.
	enum class LabelPlacement : uint8_t {
		Below, ///< Caption centred under the face, reading under that. The knob-tray column.
		Right, ///< Face at the left; caption over reading to its right, both left-justified.
	};

	void setLabelPlacement(LabelPlacement placement) {
		if (placement_ == placement) {
			return;
		}
		placement_ = placement;
		redraw();
	}

	[[nodiscard]] LabelPlacement labelPlacement() const noexcept { return placement_; }

	/// HOW TALL ONE LIST ROW IS — the face, or the two lines of text, whichever is taller.
	static constexpr float rowHeight(float diameter) {
		const float text = kLabelHeight + kLineGap + kReadoutHeight;
		return diameter > text ? diameter : text;
	}
	float faceDiameter() const noexcept { return faceDiameter_; }

	/// The mock's `.knob` column: face, 5px gap, label, 5px gap, readout.
	/// **THE FACE-TO-TEXT GAP AND THE LINE-TO-LINE GAP, SEPARATELY.** They were one 5px constant used
	/// for both, and the text rows beneath the knob read as too generously spaced, both between lines
	/// and between knob and text. Two names because they are two decisions —
	/// the distance that binds a caption to its dial is not the distance that binds a caption to its
	/// reading, and one number could only ever be right for one of them.
	///
	/// `kGap` REMAINS AS THE FACE GAP because a dozen call sites outside this widget space things
	/// against it — `zone_coord_box.cpp` and `explore_stage.cpp` draw their own captions under their
	/// own faces and must stay in step with a knob beside them.
	static constexpr float kGap = 3.0f;
	/// Between the caption and the reading under it. Tighter than the face gap on purpose: two lines
	/// of text about one control should read as a block, not as two rows that happen to be adjacent.
	static constexpr float kLineGap = 2.0f;
	static constexpr float kLabelHeight = 11.0f;
	/// **THE AIR EACH SIDE OF A CUT CAPTION** — `columnCaption` fits to the column less this on both
	/// sides. Fitted to the full column, two long neighbours butted up with no space between them and
	/// read as one run, `RESONA...CUTOFF...`. No column or label padding existed to borrow, so it is
	/// the face gap's 3px: two neighbours then stand 6px apart, twice the distance that already binds a
	/// caption to its own dial, so a caption sits visibly closer to its knob than to the next caption.
	static constexpr float kCaptionMargin = kGap;
	static constexpr float kReadoutHeight = 13.0f;
	static constexpr float kHeight = kDiameter + kGap + kLabelHeight + kLineGap + kReadoutHeight;

	/// The same column at another face size — the Engine rail's knobs are smaller. Stated as a
	/// difference from the footer's column rather than as the sum again, so the stack of gaps exists
	/// in one place: two copies of it is precisely what would let the tray and the Engine rail disagree
	/// about how tall a knob is. (Not kHeight = columnHeight(kDiameter), which is the way round it
	/// wants to be read: a static member's initialiser is not a complete-class context, so it cannot
	/// call a member function the class has not finished defining.)
	static constexpr float columnHeight(float diameter) { return kHeight - kDiameter + diameter; }

	/// TEST ONLY — THE ARC THIS KNOB WOULD DRAW.
	///
	/// It exists for this file's oldest rule: a claim about a VALUE cannot be recovered from a
	/// rendered buffer. The zoned-amount rule has two readers — the engine's offset and this arc —
	/// and the arc is the one that would ship wrong unseen, because a full-range reach over a
	/// one-zone modulation still draws *something*. A difference check is blind exactly there.
	[[nodiscard]] ModRing modRingForTest() const { return modRing(); }
	[[nodiscard]] Halo haloForTest() const { return halo(); }

	/// TEST ONLY. Whether this knob has been handed the editor's modulation state at all.
	///
	/// It exists because the failure it catches is INVISIBLE from outside: a knob with no ModState
	/// draws exactly like a knob whose parameter is simply not a destination — modRing() returns an
	/// empty ring in both cases — so a whole surface can be left out of the wiring and look correct
	/// until somebody enters mod mode and finds three controls that will not light. That is
	/// exactly what happened on the Explore stage's flagship stack.
	bool hasModulationForTest() const { return mod_ != nullptr; }

	/// **THE INBOUND-ROUTE MENU — backlog DI.** Right-click a destination and every source routed
	/// into it is listed with its amount; picking one focuses and arms that source, so the ordinary
	/// drag on this same knob then tunes that route. Returns whether a menu was shown, so the press
	/// can fall through when there is nothing to list.
	///
	/// PUBLIC because a case must be able to open it without synthesising a right-click: this repo
	/// bans X11 pointer synthesis on the development display, and `widget-shot` exists because of that ban.
	bool showRouteMenu();

	/// **THE MACRO MENU ON A KNOB THAT HAS ONE — the structural macros.** It began as the polarity
	/// switch (backlog EG: a mode switch or context menu that turns them into bipolar knobs
	/// defaulting to 0) and now carries the type and Rename beside it: `macro_menu.h`. The
	/// hooks' writers are THE VERBS a case calls instead of the menu, for the reason `chooseInbound` is
	/// separate from `showRouteMenu`. Tried after the route menu, which a macro never has (it is not a
	/// destination), so the two cannot compete for one press.
	void setMacroMenu(MacroMenuHooks hooks);
	bool showMacroMenu();

	/// **WHAT A RIGHT-CLICK OPENS, BUILT AND NOT SHOWN** (host slots T7): the inbound routes when there are
	/// any, else the macro menu on a knob that has one — then, on either, the host rows (`appendHostRows`:
	/// "Expose to host", or the slot and its one verb) for a control whose host has a pool. Nullopt when
	/// there is nothing to list, so the press goes on to its drag. Every pick is guarded by this knob's
	/// `alive_`: a popup still open when the card is destroyed writes nothing. PUBLIC so a case can read
	/// the rows as a hand sees them and fire the popup's own pick, across the card's destruction included.
	[[nodiscard]] std::optional<visage::PopupMenu> contextMenu();
	/// Opens `contextMenu()` under the knob; false when there is nothing to list. The right-click's road.
	bool showContextMenu();

	/// **IS THIS CONTROL KEPT ON A HOST SLOT — Pinned or Anchored** — the one fact the knob's corner dot
	/// draws (see `draw`). Not Default or Recent: those can be taken over at any touch, and a mark that
	/// could vanish without a hand on this knob would promise a lane the host may not keep.
	[[nodiscard]] bool hostMarked() const;
	[[nodiscard]] bool hasMacroMenuForTest() const noexcept { return macroMenu_.complete(); }
	[[nodiscard]] const MacroMenuHooks& macroMenuForTest() const noexcept { return macroMenu_; }

	/// EVERY SOURCE WITH A LIVE ROUTE INTO THIS CONTROL, in source order. What the menu lists, and
	/// what a case reads instead of the menu.
	[[nodiscard]] std::vector<device::Source> inboundSources() const;

	/// **THE VERB BEHIND A MENU ENTRY** — pick a source to focus and arm it, or remove its route.
	/// Public for the reason `showRouteMenu` is: the popup's own selection callback cannot be reached
	/// from a case, so the behaviour lives here where it can.
	void chooseInbound(device::Source source);
	/// Clear every route into this control — the menu's one Remove row. Returns how many went.
	size_t removeAllInbound();

	/// THE CONTROL BEHIND THIS KNOB, whichever road it came in on. Null only for a knob built with a
	/// null descriptor — the footer's Weather, which is bound to nothing and draws a bare track.
	device::Modulatable* control() const noexcept { return control_; }

	/// See `held_`. Takes ownership; the subscription ends when this knob does.
	void holdWatch(Watch watch) { held_.push_back(std::move(watch)); }

	/// **THE TYPED-VALUE FIELD (backlog LS)** — a triple-click opens it over the readout, on the value from
	/// before the double-click's reset. Null until the first triple-click; see `ui/type_in.h`.
	[[nodiscard]] TextField* typeInField() const noexcept { return typeIn_.field(); }
	/// Where it opens: over the reading under the face in a column, beside the face in a list row.
	[[nodiscard]] visage::Bounds typeInBounds() const;
	/// Whether the control behind the last double-click refused to be typed into.
	[[nodiscard]] bool typeInRefused() const noexcept { return typeIn_.refused(); }
	void resized() override;

private:

	/// Install the mouse handlers. Called by both constructors — see the definition.
	void buildMouse();

	/// Hand the modifier this event carries to `ModState::setBypass`, so the glow follows Alt. Called
	/// from enter, move, exit and the press — every event visage stamps with the server's modifier
	/// state — and a no-op without a `ModState`.
	void feedBypass(const visage::MouseEvent& e);


	/// WHERE A DRAG LANDS. **It was virtual for exactly one reason and that reason was retired with
	/// `ZoneKnob` on 2026-08-29** — that widget travelled through zones rather than mapping one
	/// rotation to the whole range, and it was the only thing that ever overrode this.
	///
	/// The argument for the seam was good and is worth keeping on the record: an override beat a
	/// zone-shaped `if` in the most shared widget in the editor, which would have made every knob pay
	/// attention to a case belonging to one subclass. **What retires it is not that the argument got
	/// worse but that `Knob` has no subclasses at all** — measured across `src/`, `tools/` and
	/// `tests/`, nothing derives from it, so there is nobody the seam could serve.
	void dragTo(const visage::MouseEvent& e);
	/// Where the displacement mark sits — `normalizedModulated`, through the control's own face if it
	/// has one (`OwnedControl::faceAt`).
	[[nodiscard]] float modulatedFace() const;
	/// The right ear's, by the same mapping (backlog KU).
	[[nodiscard]] float modulatedFaceR() const;

protected:
	/// THE DRAG GESTURE'S OWN STATE. Protected rather than private because `dragTo` was once virtual
	/// and an override needed to reuse the gesture; `Knob` has no subclasses now, so this is
	/// protected-by-history. Harmless, and named so nobody reads it as a live extension point. The
	/// anchor rather than keep a second one. A subclass with its own anchor would drift from this one
	/// the moment either learned something the other did not — the fine-modifier re-anchor below is
	/// exactly such a thing.
	[[nodiscard]] float anchorTravelY() const noexcept { return anchorY_; }
	[[nodiscard]] float anchorValueNorm() const noexcept { return anchorValue_; }
	[[nodiscard]] bool anchorIsFine() const noexcept { return anchorFine_; }

	/// Take the press as the anchor for everything that follows. A drag reads its value from where
	/// the press was, not from the previous event, so the parameter cannot creep from accumulated
	/// rounding and a drag that returns to where it started returns the value with it.
	///
	/// Protected with the rest of the gesture, for the reason above: an override had to RE-anchor when the fine
	/// modifier changes mid-drag, and a subclass that could not would have the value jump under it.
	void anchor(const visage::MouseEvent& e);

	/// Post a value only if it is a change, and bracket it as one gesture. The no-op case posts
	/// nothing at all: a gesture around a value the parameter already holds spends an undo step on
	/// silence. Protected for the same historical reason: an override had to land its value through the same
	/// bracket — a subclass that wrote the control directly would skip the undo step (backlog 18).
	void commit(double value);

private:
	/// **THE MODULATION SUBSCRIPTION, HELD** — see `ui::Watch`. Re-assigned by `setModulation`, which
	/// is what releases the previous state's entry.
	Watch watch_;

	/// **SUBSCRIPTIONS MADE ON THIS KNOB'S BEHALF.** `paramKnob` is a free function: it registers the
	/// knob with the parameter registry and has nowhere to keep the token, so the knob keeps it. The
	/// builder subscribes, the thing that lives holds — which is the whole rule, applied to a case
	/// where the two are not the same object.
	std::vector<Watch> held_;
	void resetToDefault();

	/// Say what this knob now reads, in the readout's lower-left line.
	void announce(const char* verb = nullptr);



	/// The amount the focused source currently sends here.
	float routeAmount() const;
	void beginRouteDrag(const visage::MouseEvent& e);
	void dragRouteTo(const visage::MouseEvent& e);
	void commitRoute(float amount);
	/// The mock's double-click in mapping mode: the route is gone, not zeroed.
	void unroute();

	/// OWNED ONLY WHEN THIS KNOB WAS BUILT OVER A HOST PARAMETER. The Modulatable constructor leaves
	/// it null and points control_ at somebody else's control — a device's, which outlives the widget
	/// because the device does.
	/// Held only on the owned road; null on the borrowed one.
	std::unique_ptr<OwnedControl> owned_;
	device::Modulatable* control_ = nullptr;
	const char* label_;
	/// `setFullName`; empty is the control's declared name.
	std::string fullName_;
	Notifier* notifier_ = nullptr;
	/// The macro menu's hooks, incomplete on every knob but a structural macro's.
	MacroMenuHooks macroMenu_;
	/// **THE KNOB, FOR A MENU THAT MAY OUTLIVE IT** — `contextMenu`'s picks hold it weakly and do nothing
	/// once it is gone (`MathCard::alive_`'s shape). The control a pick writes through outlives the knob
	/// (a card declares its controls before its widgets), so while this holds, so does the control.
	std::shared_ptr<void> alive_ = std::make_shared<char>(0);
	/// `hostMarked`'s answer, and the pool generation and control it was taken for.
	struct HostMarkCache {
		uint64_t generation = 0;
		const device::Modulatable* control = nullptr;
		bool marked = false;
		bool valid = false;
	};
	mutable HostMarkCache hostMarkCache_;
	float faceDiameter_ = kDiameter;
	LabelPlacement placement_ = LabelPlacement::Below;
	bool captionIsName_ = false;
	bool enabled_ = true;
	bool active_ = true;

	float anchorY_ = 0.0f;
	float anchorValue_ = 0.0f;
	double anchorRaw_ = 0.0; ///< the value at the grab, for a `dragSpan` control
	bool anchorFine_ = false;
	bool dragging_ = false;

	/// Whether a gesture has been opened for the drag in progress. It opens on the first value that
	/// actually moves rather than on the press, so a click that turns nothing posts nothing.
	bool gestureOpen_ = false;

	/// Null in every knob that has not been handed the panel's state, which is what makes this feature
	/// absent rather than merely inactive in the editors that do not want it.
	/// STILL HELD, AND FOR ONE THING ONLY: touched(), the shared redraw notification, and the watch
	/// subscription. Everything a knob used to ask it about routing — armed, source, the amount — now
	/// comes from control_->display(), which is what lets a device control answer the same questions
	/// without this widget knowing what it is.
	ModState* mod_ = nullptr;
	bool routeDragging_ = false;
	float routeAnchorY_ = 0.0f;
	float routeAnchorAmount_ = 0.0f;
	/// What the amount was when the press landed, so the undo record carries where the drag STARTED
	/// rather than where its last motion event happened to be.
	float routeBefore_ = 0.0f;

	/// LAST, so it — and the field it may own as a child — goes first.
	TypeIn typeIn_ { *this };
};

} // namespace feathers::ui
