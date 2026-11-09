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

#include "device/endpoint.h"
#include "plugin/ui/frame_watchers.h"

#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <functional>
#include <string>
#include <vector>

// WHICH SOURCE THE HAND IS POINTING, and whether it is pointing at all.
//
// The mock's mapping mode is one flag and one focused unit: arm the arrow on a modulator's card, and
// every knob that source may reach lights up until you disarm it. It is STICKY on purpose — *"mapping
// stays STICKY — route several in one session"* — so the flag outlives a single route.
//
// Editor state, not a parameter and not patch state: which knob is glowing is not part of what a patch
// sounds like, and a saved session that reopened in mapping mode would be remembering a gesture rather
// than a sound. Same tier as SoundSelection and StageMode, and the same watch/notify shape, so a
// surface that draws differently while armed simply redraws when the flag moves.
//
// THE SOURCE COUNT IS GIVEN, NOT KNOWN. This class used to bound `focus()` with `kModSourceCount`, a
// constant of Feathers' parameter space — which put `plugin/params.h`, and the CLAP SDK behind it,
// into every widget that only needed to know which source was armed. Measured with a link probe: it
// is one of four such chokepoints in the card floor.
//
// A HOST HANDS ITS OWN COUNT IN, and there is no default. A defaulted bound is the failure this whole
// arrangement exists to prevent: a second host would silently inherit Feathers' number, and being
// wrong about how many sources exist is not a thing that announces itself — it is a source that
// cannot be armed, or one that can be armed and is not there. `RouteEdit` moved to `route_edit.h`
// with the bridge it writes through; this file no longer knows what a parameter is.

namespace feathers::ui {

class ModState {
public:
	/// Which source the strip is showing and the knobs are drawing amounts for. Always a real source —
	/// there is no "no source" state, because the rings have to have something to be about.
	///
	/// NOT ONLY A MACRO SINCE TASK 49. The per-sound weights are sources too, armed from the arrow grid
	/// in the Engine rail rather than from a card in the strip, and every part of the modulation surface
	/// — the glow, the rings, the route list — was already written against "the focused SOURCE" rather
	/// than "the focused card". So the bound here is kModSourceCount, and the strip goes on drawing its
	/// four cards without needing to know that something else can hold the focus.
	///
	/// **A SOURCE BY ENDPOINT SINCE KILL-LANES STEP 2** — a device's output by the device's id, or one of
	/// the host's own sources — so the focus follows a modulator wherever it moves and never lands on
	/// whatever took its old index. There is no source COUNT any more because there is no flat index to
	/// bound: a source that names nothing is refused where it is used, by the routing authority.
	/// **FOLLOWING THE PARENT WHILE IT IS ARMED** (rack roadmap §7): a chain's inner state answers the
	/// outer arm with the outer source spelled as an inner control would read it — `asOuter()` by
	/// default, or whatever `setOuterSpelling` says, which is not the same thing and backlog HM is why
	/// — so every inner control volunteers through the road it already has. This state's own arm wins.
	device::Source source() const {
		if (!armed_ && parent_ != nullptr && parent_->armed()) {
			return spellOuter(parent_->source());
		}
		return source_;
	}

	ModState() noexcept = default;
	~ModState() {
		setParent(nullptr);
		for (ModState* child : children_) {
			child->parent_ = nullptr;
		}
	}
	ModState(const ModState&) = delete;
	ModState& operator=(const ModState&) = delete;

	/// **THE STATE THIS ONE FOLLOWS** — rack roadmap §7: in assign mode the source's address is
	/// published, and destinations volunteer to read the source. A
	/// chain's inner state follows the outer: while the OUTER is armed and this one is not, `armed()`,
	/// `source()` and `bypassed()` are the outer's, and every arm, focus or bypass on the outer repaints
	/// this one's watchers too. The inner rack's own arms still arm this state locally, and a local arm
	/// takes precedence. Null unfollows.
	/// The state this one follows, or null — the route menu climbs it to arm an outer source where it lives.
	[[nodiscard]] ModState* parent() const noexcept { return parent_; }
	void setParent(ModState* parent) {
		if (parent == parent_) {
			return;
		}
		if (parent_ != nullptr) {
			std::erase(parent_->children_, this);
		}
		parent_ = parent;
		if (parent_ != nullptr) {
			parent_->children_.push_back(this);
		}
		watchers_.notify();
	}
	[[nodiscard]] const ModState* parentForTest() const noexcept { return parent_; }
	/// **HOW MANY STATES FOLLOW THIS ONE** — the other half of `parentForTest`, and it exists because
	/// exclusivity (see `arm`) walks the whole tree, so "does that change reach the main editor?" is a
	/// question about the tree's SHAPE and a case has to be able to read it. A state with no parent and
	/// no children cannot clear anybody and cannot be cleared, which is the claim the ordinary Feathers
	/// editor rests on — asserted rather than inferred from "there is only one root".
	[[nodiscard]] size_t childCountForTest() const noexcept { return children_.size(); }

	/// **HOW A SOURCE OF THE HOST OUTSIDE IS SPELLED IN HERE — AND `asOuter()` IS NOT ALWAYS THE ANSWER.**
	///
	/// Backlog HM: the modulators on Perform reached only inner modulation destinations, and the fx
	/// params in the grain fx did not glow. The perform pane's chips arm
	/// SKLITTER's OWN output, and a following state spelled that `outer(output(sklitter, n))` — a source
	/// the inner gate MUST refuse, because `backplane::mayRoute` sends an outer source to
	/// `RackEngine::upstreamOf(self, output(self, n))` and *"a device is not upstream of itself"* (trap
	/// T1, pinned in `tests/test_rack_nesting.cpp`). So `armed()` was true, the repaint was delivered,
	/// and every inner control computed `routable == false`. A flag that arrives is not an effect that
	/// happens.
	///
	/// **THE SAME SIGNAL IS REACHABLE INSIDE UNDER ANOTHER NAME**, which is what makes this a spelling
	/// rather than a missing feature: a holder may publish its outputs into its chain as that chain's
	/// CORE sources, and SKLITTER does (`kPostChainAxes.coreSources == kPerformOutputs`). Translating at
	/// the boundary fixes the glow and the WRITE together — a knob routes through `source()` too — so a
	/// control cannot light for an endpoint the drag would then be refused.
	///
	/// **IT IS DECLARED, NEVER INFERRED, AND THE DEFAULT IS THE OLD BEHAVIOUR.** Nothing here can derive
	/// the mapping: a nested rack's chain also declares core sources and they are its MACROS, so reading
	/// it off `axes().coreSources` would map a rack's output 0 onto Macro 1. Only the surface that knows
	/// the holder may say, and only one does — `FxLaneRail`, which is SKLITTER's own rail. Unset means
	/// `asOuter()`, which is what every other chain still gets.
	void setOuterSpelling(std::function<device::Source(device::Source)> spell) {
		spell_ = std::move(spell);
		watchers_.notify();
		tellChildren();
	}

	void focus(device::Source source) {
		if (source == source_) {
			return;
		}
		source_ = source;
		watchers_.notify();
		tellChildren();
	}

	/// **WHOSE ARM THIS IS, AND IT IS NOT A STYLE ARGUMENT.** A HAND's arm is a gesture and clears every
	/// other arm in the follow-tree, because two glowing surfaces for one gesture is incoherent — the
	/// next press routes from ONE source and nothing on screen says which (backlog HM). A BORROWED one
	/// is the editor arming a state for a moment so that a write lands on the right source and putting
	/// it straight back; `removeAllInbound` is the only such caller and its own comment says why it must
	/// arm at all. A borrow that cleared the hand's arm would delete a mode the player is in as a side
	/// effect of a menu row — which an existing case in `test_rack_editor.cpp` caught the moment
	/// exclusivity was added, rather than anybody predicting it.
	enum class Arm : uint8_t { Hand, Borrowed };

	/// Whether the next drag on a destination edits its AMOUNT rather than its value.
	bool armed() const noexcept { return armed_ || (parent_ != nullptr && parent_->armed()); }
	/// This state's OWN arm, the parent's not counted — what an inner rack's arms toggle.
	bool armedLocally() const noexcept { return armed_; }

	/// **EVERY ARM IN THIS FOLLOW-TREE OFF — what an editor CLOSING does** (backlog KS, 2026-09-28:
	/// after closing and reopening the UI with Macro assign mode active, the entire UI froze). The editor is built once and kept for the process (`PluginWindow`),
	/// so this state outlives every close, and an arm left standing came back on the next open as an editor
	/// whose knobs route instead of turn. An arm is a gesture of the hand at an open editor; the hand left
	/// when the editor did. From the ROOT, so a nested chain's local arm goes too — `arm(false)` alone
	/// cascades nothing, by design. Each state through `arm(false)`, so every surface is told.
	/// **HOW MANY STATES IT DISARMED** — zero when nothing in the tree was armed, which is how the rack's
	/// Esc (backlog NI) knows whether the key was its to take.
	size_t disarmTree() { return root()->disarmOthers(nullptr); }

	/// ARM OR DISARM, AND REPORT HOW MANY FRAMES THAT TOLD.
	///
	/// **THE COUNT IS THE ONLY WAY TO ASK WHETHER ENTERING ASSIGN MODE REACHED A KNOB.** Arming
	/// changes no value, so nothing downstream of it can be read back: the glow is not a number a
	/// widget holds, it is a branch inside `draw()`. A repaint is unobservable headlessly, so the line
	/// that DELIVERS one has to say that it did — the same reason `touched()` hands on
	/// `FrameWatchers::notify`'s return, and the same reason that return exists at all.
	///
	/// **IT IS DELIBERATELY NOT ADDED TO `framesToldForTest()`.** That tally counts `touched()` alone
	/// so a case can tell "the routings changed" from "the hand armed something", and folding this
	/// into it would erase exactly the distinction its own comment says it exists for. A case that
	/// wants this number takes it from here.
	///
	/// ZERO FOR A FLAG THAT DID NOT MOVE, which is not a failure and is the twin half of any case
	/// written against this: arming while already armed must tell nobody, or the fix for a missing
	/// repaint is "repaint always" and it would pass.
	///
	/// **AND `by` SAYS WHOSE ARM IT IS — see `Arm`.** The default is the hand's, which is exclusive.
	size_t arm(bool armed, Arm by = Arm::Hand) {
		if (armed == armed_) {
			// A HAND ARMING WHAT IS ARMED — another source's arrow pressed while in the mode — tells nobody
			// (nothing to repaint), but it is the newest thing the hand did, so the Esc stack hears it (NN).
			if (armed && by == Arm::Hand) {
				if (ModState* top = root(); top->onTreeArm_) {
					top->onTreeArm_(true);
				}
			}
			return 0;
		}
		armed_ = armed;
		// **ONE HAND, ONE ARM — AN ARM ANYWHERE IN THIS TREE CLEARS EVERY OTHER** (backlog HM,
		// 2026-09-21: a modulator in the fx lane and one on Perform could be assigning at once). Two glowing surfaces for one gesture is incoherent to a player: the
		// next press routes from ONE source, and nothing on screen says which.
		//
		// **IT DOES NOT UNDO THE PER-LANE STATE, AND THAT DISTINCTION IS THE WHOLE DESIGN.** Each state
		// keeps its own source, its own flag and its own watchers, because an arm is an offer to route
		// and a route runs inside one `InnerChain`'s authority — a shared state would light controls
		// volunteering for a route the engine refuses (`chain_column.h` says so at length, and calls
		// merging them the honesty rule failing where it is expensive to notice). What becomes unique is
		// WHO IS ARMING, which is a fact about the hand rather than about a lane.
		//
		// **DISARMING CASCADES NOTHING**, deliberately: taking your own arm off is not an instruction to
		// anybody else, and a cascade there would make `arm(false)` a way to silently clear a surface
		// the hand never touched.
		if (armed_ && by == Arm::Hand) {
			root()->disarmOthers(this);
		}
		// **ARMING SELECTS — TOLD FROM HERE, THE ONE PLACE EVERY ROAD PASSES.** By design the lit (armed)
		// card is the one Delete takes. The
		// strip's Delete resolves its own selection, so the two must be one card, and the way to make
		// that true of every road that arms — the card's chip, the Engine rail's grid, anything written
		// tomorrow — is to say it where arming happens rather than at each caller. The listener is the
		// strip's (`ModPanel` installs it) and it decides whether the source is a card it holds; a
		// macro or a sound weight armed here selects nothing, and disarming says nothing at all.
		if (armed && onArmed_) {
			onArmed_(source_);
		}
		const size_t told = watchers_.notify();
		tellChildren();
		// **AND THE ROOT'S EDITOR IS TOLD, FOR ITS ESC STACK** (backlog NN) — from any state in the tree, so an
		// arm in a nested chain is the editor's one arm too. `byHand` false for a disarm or a borrow.
		if (ModState* top = root(); top->onTreeArm_) {
			top->onTreeArm_(armed && by == Arm::Hand);
		}
		return told;
	}

	/// See `arm`. One listener, because one strip holds the cards; a second strip would be a second
	/// design question, not a vector.
	void setArmListener(std::function<void(device::Source)> listener) { onArmed_ = std::move(listener); }

	/// **EVERY ARM CHANGE ANYWHERE IN THIS FOLLOW-TREE, told to the ROOT's observer** — `true` when a hand
	/// armed something (it is then the most recently opened thing, backlog NN), `false` for any disarm or a
	/// borrowed arm. Set on the root; one observer, the editor's `ArmEscape`.
	void setTreeArmObserver(std::function<void(bool byHand)> observer) { onTreeArm_ = std::move(observer); }

	/// Whether ANY state in this follow-tree is armed, root and every nest below it.
	[[nodiscard]] bool armedInTree() { return root()->anyArmedBelow(); }

	/// **WHAT TO CALL A SOURCE, FOR A WIDGET THAT HAS NO WAY TO KNOW.**
	///
	/// `modSourceName` needs the host's `DeviceSlots`; a `Knob` has a control and this state and
	/// nothing else. Backlog DI puts a list of a destination's inbound sources under the pointer, and
	/// a menu of indices would be unusable — so the namer is installed once by whoever assembles the
	/// editor, which is the only layer that holds both.
	///
	/// **A FUNCTION RATHER THAN A TABLE, because a source's name is not constant**: a device source is
	/// named after the device in its slot, and that changes when a rack slot is filled or moved. A
	/// table captured at install time would go stale exactly when the rack is edited, which is when
	/// somebody is most likely to be reading it.
	///
	/// UNSET IS NOT AN ERROR: a surface with no namer falls back to an ordinal, which is worse to read
	/// and still correct.
	void setSourceNamer(std::function<std::string(device::Source)> namer) { namer_ = std::move(namer); }
	[[nodiscard]] std::string sourceName(device::Source source) const {
		// AN OUTER SOURCE IS NAMED BY THE HOST OUTSIDE (rack roadmap §7), marked as from outside: this
		// state's namer knows only this rack's sources.
		if (source.isOuter() && parent_ != nullptr) {
			// "outer Macro 1", in WORDS: this was "↑ Macro 1", and the editor's one typeface (Lato) has no U+2191 — measured
			// with fc-query, the same gap that swallowed the voice-count stepper's "→" (backlog KE step 2) — so it drew as
			// a blank. Any glyph outside the embedded font's charset draws as nothing.
			return "outer " + parent_->sourceName(source.inner());
		}
		if (namer_) {
			return namer_(source);
		}
		return source.isDevice() ? "Output " + std::to_string(source.index + 1)
		                         : "Source " + std::to_string(source.index + 1);
	}

	/// **EVERY SOURCE A HAND COULD ARM, FOR A SURFACE THAT HAS TO ENUMERATE THEM** — the inbound-route
	/// menu (backlog DI) walks these and keeps the ones reaching its control. It walked `0..sourceCount`
	/// while a source was a flat index; an endpoint has no count, so whoever knows what sources exist —
	/// the panel that walks the devices — installs the list, beside the namer and for the namer's
	/// reasons: asked live, because the rack's devices change under it.
	///
	/// UNSET IS AN EMPTY LIST, which offers no menu rather than a guessed one.
	void setSourceLister(std::function<std::vector<device::Source>()> lister) { lister_ = std::move(lister); }
	[[nodiscard]] std::vector<device::Source> sources() const {
		std::vector<device::Source> out = lister_ ? lister_() : std::vector<device::Source> {};
		// AND THE HOST OUTSIDE'S, spelled as this rack reads them (rack roadmap §7): the inbound-route
		// menu keeps the ones that may reach its control, and the gate answers that per source.
		//
		// **THROUGH THE SAME SPELLING `source()` USES, AND DEDUPED AGAINST WHAT THE LISTER ALREADY GAVE.**
		// A holder's output that is spelled as this chain's own core source is the SAME endpoint the
		// lister named, so listing it twice would put one route in the menu twice under two names.
		if (parent_ != nullptr) {
			for (const device::Source s : parent_->sources()) {
				const device::Source spelled = spellOuter(s);
				if (std::find(out.begin(), out.end(), spelled) == out.end()) {
					out.push_back(spelled);
				}
			}
		}
		return out;
	}

	/// Whether the hand is HOLDING THE BYPASS — Alt on Linux and Windows, Option on macOS — so that a
	/// press on an armed destination would edit its VALUE rather than route to it.
	///
	/// THIS IS THE GLOW'S HALF OF THE BYPASS, AND IT WAS MISSING (backlog CU). The PRESS has always
	/// read the modifier off its own event — `Knob::mappable(e.isAltDown())` — so with Alt held a
	/// drag edited the value. But the halo is painted in `draw()`, which has no event to ask, and it
	/// asked `mappable()` with the default: an armed knob went on glowing "this press will route"
	/// while the press in fact edited. The honesty rule is that a control's look matches what a press
	/// does, so the flag the draw path reads has to live somewhere every knob can see, and this is it.
	///
	/// FED BY MOTION, NOT BY A KEY. visage's X11 road translates no modifier keysym at all
	/// (`translateKeyCode` has no `XK_Alt_*`, MEASURED 2026-09-06), so no frame is ever told Alt went
	/// down — but every mouse event it delivers is stamped with the modifier state the server holds
	/// at that moment, so the hand's own movement over a knob is what carries the fact. A held Alt
	/// with a perfectly still pointer is not seen until the next motion; on a real hand that is the
	/// same instant. A polled road (visage exposing its `modifierState()`) would close that gap and
	/// is filed in the fork patch plan.
	bool bypassed() const noexcept { return bypass_ || (parent_ != nullptr && parent_->bypassed()); }

	/// SET THE BYPASS, AND REPORT HOW MANY FRAMES THAT TOLD — the same contract as `arm`, for the
	/// same reason: what changes is a branch inside `draw()`, and only the delivery count can say the
	/// repaint reached anybody. Zero for a flag that did not move, which is what lets every motion
	/// event feed this without repainting the editor on every pixel of travel.
	size_t setBypass(bool held) {
		if (held == bypass_) {
			return 0;
		}
		bypass_ = held;
		const size_t told = watchers_.notify();
		tellChildren();
		return told;
	}

	/// Tell everyone watching that the ROUTINGS have changed, not the flag.
	///
	/// The amounts live on the bridge, which has no watcher list of its own — the parameter registry
	/// only knows about parameters, and an amount is deliberately not one. So the surfaces that draw
	/// routings share this one notification: a knob that has just written an amount says so here, and
	/// the strip and the route list redraw in the same frame rather than one gesture late.
	///
	/// IT RETURNS HOW MANY FRAMES IT TOLD, and that number is the only way to test any of this. A
	/// repaint is unobservable headlessly, so the line that DELIVERS one has to report that it did —
	/// `FrameWatchers::notify` returns a count for exactly this reason and this hands it on.
	size_t touched() {
		const size_t told = watchers_.notify();
		framesTold_ += told;
		return told;
	}

	/// HOW MANY FRAMES THIS STATE HAS TOLD, EVER — the tally a case reads across a road it cannot
	/// see inside.
	///
	/// IT ACCUMULATES THE NOTIFICATION'S OWN RETURN, NOT A COUNT OF CALLS, and the difference is the
	/// whole reason it is trustworthy. A counter standing beside `notify()` would rise for a
	/// `touched()` that told nobody, so it would report success on an empty watcher list — which is
	/// the state a torn-down surface leaves behind. This rises only when a frame was really told, so
	/// a case has to put a frame on the list before it can observe anything, which is the half that
	/// proves delivery rather than intent.
	///
	/// AND IT COUNTS `touched()` ALONE, not `focus` or `arm`. Those notify the same list about a
	/// different fact, and a tally that moved for all three could not tell "the routings changed"
	/// from "the hand armed something" — which is exactly the distinction both of its cases rest on.
	size_t framesToldForTest() const noexcept { return framesTold_; }

	/// HOW MANY FRAMES ARE ON THIS LIST — the same accessor `SoundSelection` and `StageMode` carry,
	/// read by the case that requires every watcher list to return to its baseline across a card
	/// cycle.
	///
	/// IT EXISTS SO THAT `watchers()` BELOW KEEPS HAVING ONE PURPOSE. That accessor is exposed for
	/// `forgetFrameTree` and says so; a census reading `watchers().count()` would be a second reason
	/// to hand out the whole list, and the comment on it would quietly stop being true. This is the
	/// narrow road for the narrow question.
	size_t watcherCountForTest() const noexcept { return watchers_.count(); }

	/// Redraw `frame` whenever any of it moves — the knobs' glow is the map-mode signal, so it has to
	/// arrive in the same frame the flag does.
	[[nodiscard]] Watch watch(visage::Frame* frame) { return watchers_.watch(frame); }

	/// AND OFF IT AGAIN — the other half, which was missing.
	///
	/// `watch` has always been here and `unwatch` was reachable only through `watchers()`, which is
	/// exposed for `forgetFrameTree`. So a frame could put ITSELF on the list and could not take
	/// itself off without going round through an accessor documented for somebody else's use. **A
	/// register with no matching unregister beside it is an invitation to leave the duty to an
	/// owner**, and five owners took it while a sixth did not — which showed up as a crash.
	void unwatch(const visage::Frame* frame) { watchers_.unwatch(frame); }

	/// Whether `frame` will be repainted when this state moves. FrameWatchers explains why this is a
	/// public thing rather than an implementation detail: a repaint is unobservable headlessly, so a
	/// case that wants to assert "this widget follows the armed flag" can only check the
	/// SUBSCRIPTION. That is exactly the claim that was wrong here — see EnvCard's constructor.
	bool watches(const visage::Frame* frame) const { return watchers_.watches(frame); }
	void clearWatchers() { watchers_.clear(); }

	/// THE LIST ITSELF, for the one caller that must take a whole SUBTREE off it — `forgetFrameTree`,
	/// called when a rack card is retired. Exposed rather than wrapped because the alternative is a
	/// `unwatchTree` here that would have to know about `visage::Frame::children()`, and this class
	/// deliberately knows nothing about the tree. `watch`, `watches` and `clearWatchers` already
	/// front the same member; this adds reach, not a new coupling.
	FrameWatchers& watchers() noexcept { return watchers_; }

private:
	/// THE FIRST OF THE HOST'S OWN SOURCES until something is focused — there is no "no source" state.
	device::Source source_ = device::Source::core(0);
	bool armed_ = false;
	bool bypass_ = false;
	/// See `arm` and `setArmListener`.
	std::function<void(device::Source)> onArmed_;
	/// See `setTreeArmObserver`.
	std::function<void(bool)> onTreeArm_;
	[[nodiscard]] bool anyArmedBelow() const {
		if (armed_) {
			return true;
		}
		for (const ModState* child : children_) {
			if (child->anyArmedBelow()) {
				return true;
			}
		}
		return false;
	}
	std::function<std::string(device::Source)> namer_;
	std::function<std::vector<device::Source>()> lister_;
	FrameWatchers watchers_;
	size_t framesTold_ = 0;
	/// See `setParent`.
	ModState* parent_ = nullptr;
	std::vector<ModState*> children_;
	/// See `setOuterSpelling`.
	std::function<device::Source(device::Source)> spell_;
	/// **ONE PLACE, SO `source()` AND `sources()` CANNOT DISAGREE** — which is the bug this whole seam
	/// exists to close, arriving in a smaller form: a menu that offered an endpoint the glow was not
	/// about would be the honesty rule failing inside the fix for it.
	[[nodiscard]] device::Source spellOuter(device::Source source) const {
		return spell_ ? spell_(source) : source.asOuter();
	}
	/// **THE TOP OF THIS FOLLOW-TREE** — where `arm` starts the walk, so that "every other arm" means
	/// every one in the whole nest and not only the ones below the state a hand happened to touch.
	[[nodiscard]] ModState* root() noexcept {
		ModState* top = this;
		while (top->parent_ != nullptr) {
			top = top->parent_;
		}
		return top;
	}
	/// EVERY STATE IN THE TREE BUT ONE, DISARMED. Through `arm(false)`, so each one's own watchers and
	/// children are told exactly as they would be by a hand — a surface going dark is a repaint, and a
	/// flag cleared behind the notification's back is the class of bug `notify()` returns a count for.
	size_t disarmOthers(const ModState* keep) {
		size_t disarmed = 0;
		if (this != keep && armed_) {
			(void)arm(false);
			++disarmed;
		}
		for (ModState* child : children_) {
			disarmed += child->disarmOthers(keep);
		}
		return disarmed;
	}

	/// A CHANGE HERE IS A CHANGE FOR EVERY STATE FOLLOWING THIS ONE: their watchers are told too.
	void tellChildren() {
		for (ModState* child : children_) {
			child->watchers_.notify();
			child->tellChildren();
		}
	}
};

} // namespace feathers::ui
