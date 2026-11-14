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

#include "fx/sklitter/sklitter_types.h"
#include "fx/eq/eq_types.h"
#include "dsp/slice_basis.h"
#include "dsp/phase_warp.h"
#include "plugin/device_registry.h"
#include "plugin/audition.h"
#include "plugin/audition_tuning.h"
#include "fx/filter/filter_types.h"
#include "plugin/device_row.h"
#include "device/ears.h"
#include "device/extra_state.h"
#include "backplane/chunked_pool.h"
#include "backplane/container_bodies.h"
#include "backplane/device_host.h"
#include "plugin/master_meter.h"
#include "plugin/pad_hits.h"
#include "plugin/feathers_chains.h"
#include "rack/inner_chain.h"
#include "rack/chain_record.h"
#include "rack/held_falls.h"
#include "rack/fragment.h"
#include "rack/published_chain.h"
#include "plugin/fx_chain.h"
#include "plugin/mod_rack.h"
#include "plugin/params.h"
#include "plugin/preview.h"
#include "plugin/sound_orientation.h"
#include "plugin/sound_residue.h"
#include "plugin/sound_gamma.h"
#include "plugin/sound_hues.h"
#include "plugin/macro_modes.h"
#include "plugin/spectral_plot.h"
#include "plugin/spectral_state.h"
#include "plugin/terrain_likes.h"
#include "device/subscribers.h"
#include "hostslots/slot_bus.h"
#include "hostslots/slot_pool.h"
#include "rack/slot_owner.h"
#include "plugin/sound_travel.h"
#include "plugin/sound_variations.h"
#include "plugin/sound_weights.h"

#include "clap/id.h"

#include <array>
#include <atomic>
#include <bit>
#include <cstdint>
#include <unordered_map>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The road between the editor and the audio thread, in both directions, with no window and no host
// anywhere in it — which is why it is a file of its own. Everything here is testable by calling it.
//
// The two directions are not symmetric, because the problems are not.
//
// EDITOR TO ENGINE is a queue. A knob drag cannot write a parameter where the audio thread is
// reading it, and it cannot take a lock to do so either, so it posts an event and returns. The audio
// thread drains the queue at the top of its next block — or, when the plugin is inactive and there
// is no next block, clap_plugin_params.flush drains it on the main thread after the editor asks the
// host for one. Each event also has to reach the HOST, not just the ParamSet, or the move the user
// just made would be invisible to automation recording and to the host's own display of the
// parameter; the plugin binding does that as it drains.
//
// ENGINE TO EDITOR is a snapshot. Automation, a preset load and host modulation all move values
// underneath the editor, and the editor needs to notice without polling anything expensive. The
// audio thread publishes both readings of every parameter into atomics; the editor compares them
// against what it last drew and gets back the list of what moved.
//
// Why the modulated reading is published rather than read directly: ParamSet's modulation offsets
// are deliberately NOT atomic — see the threading note in params.h, which turns on their being
// touched from one thread at a time — so reading them from the editor's thread would be exactly the
// race that design avoids. Publishing costs the audio thread a handful of relaxed stores per block
// and keeps that invariant intact.

namespace feathers::ui {
class EditHistory;
}

namespace feathers {

// The published snapshot is an array of atomic doubles read by the editor and written by the audio
// thread, and the whole arrangement assumes those writes are just stores. On a platform where a
// 64-bit atomic is emulated with a lock, they would be a mutex acquisition per parameter per block
// on the audio thread — a priority-inversion stall, and exactly the kind of thing that is invisible
// until it is a dropout on somebody's machine. Every platform this targets satisfies it; the
// assertion is here so a port that does not fails to build rather than fails to play.
static_assert(std::atomic<double>::is_always_lock_free,
              "the published snapshot would take a lock on the audio thread");

/// A parameter move made in the editor, on its way to the audio thread.
///
/// Gestures bracket a drag. They carry no value of their own: the host uses them to decide that
/// everything between one and the other is a single undoable edit rather than several hundred.
struct UiParamEvent {
	enum class Kind : uint8_t { GestureBegin, Value, GestureEnd };

	Kind kind = Kind::Value;
	clap_id id = 0;
	double value = 0.0;
};

/// Lock-free single-producer single-consumer ring buffer.
///
/// One producer (the editor's thread) and one consumer (whichever thread is draining, which is the
/// audio thread while active and the main thread while not — never both, because an inactive plugin
/// has no audio thread). That restriction is what allows two plain indices and no locks.
///
/// empty() is the one reader outside those two roles: hasPendingWork() calls it from the editor's
/// thread, which therefore reads the index the CONSUMER writes. That is benign rather than
/// overlooked. Both indices are atomic, so the read is well defined and can only be out of date —
/// and its single caller uses it to decide whether to ask the host for a flush, where being a frame
/// late costs a frame and being a frame early costs a request the host is free to ignore.
class UiParamQueue {
public:
	/// Power of two, so the wrap is a mask. Far larger than a drag needs: a gesture at frame rate
	/// posts tens of events a second and the consumer empties the queue every block.
	static constexpr size_t kCapacity = 1024;

	/// Producer side. False when the queue is full, and the event is dropped rather than blocking —
	/// see droppedCount().
	bool push(const UiParamEvent& event) noexcept;

	/// Consumer side. False when there is nothing waiting.
	bool pop(UiParamEvent& out) noexcept;

	bool empty() const noexcept;

	/// How many events have ever been dropped for want of space.
	///
	/// Not decoration: a queue that overflows silently loses a knob move, and the editor and the
	/// engine then disagree about a value with nothing to say so. Nothing consumes this yet beyond the
	/// tests, and it is here so that when a drop happens there is something to look at.
	uint32_t droppedCount() const noexcept;

private:
	std::array<UiParamEvent, kCapacity> ring_ {};
	std::atomic<size_t> write_ { 0 };
	std::atomic<size_t> read_ { 0 };
	std::atomic<uint32_t> dropped_ { 0 };
};

/// What the editor's controls read and write. One per plugin instance.
class ParamBridge final : public rack::FragmentSite {
public:
	ParamBridge();

	//------------------//
	// The editor's side //
	//------------------//

	/// All three ignore ids this build does not declare, and setValue also ignores values that are not
	/// finite. Nothing unaddressable reaches the queue, which is what lets every consumer downstream
	/// assume an id it can look up — including the binding's list of moves the host still owes an
	/// event for, where an unrecognised id would sit forever.
	void beginGesture(clap_id id) noexcept;
	void setValue(clap_id id, double value) noexcept;
	void endGesture(clap_id id) noexcept;

	/// The base value, as of the last refresh() — what automation and the patch own.
	double value(clap_id id) const noexcept;

	/// Base plus host modulation, as of the last refresh() — what the engine is actually running on.
	/// Equal to value() whenever nothing is modulating the parameter.
	double modulated(clap_id id) const noexcept;

	/// **THE RIGHT EAR OF A `stereo` PARAMETER** (backlog KU, 2026-10-05), as of the last refresh(): true and `right` set
	/// while a pair routed into it splits the ears (`ParamSet::resolvedRight`); false — mono, draw the one ring — otherwise.
	bool modulatedRight(clap_id id, double& right) const noexcept;

	/// Take a new snapshot, and return the table indices whose readings moved since the previous one.
	///
	/// The return is the batch a caller redraws: on a quiet frame it is empty and nothing is touched.
	/// The first call reports every parameter, so a freshly opened editor draws itself from one code
	/// path rather than needing a separate initial pass.
	std::span<const uint32_t> refresh() noexcept;

	/// TEST ONLY. How many value gestures the editor has opened and not closed. Zero is the only
	/// correct answer once a hand has let go of everything — see openGestures_.
	int openGestureCountForTest() const noexcept { return openGestures_; }

	/// Whether the editor has posted anything the engine has not taken yet. The binding uses this to
	/// decide whether asking the host for a flush is worth the call. AND, WHILE THE SPECTRUM DRAWER IS OPEN, A DRAWER
	/// EDIT THE PLOT HAS NOT SHOWN (D2): the master switch and the curve are not params, so on a sleeping plugin nothing
	/// else would ask for the flush that hands them to the engine and publishes the plot they make.
	bool hasPendingWork() const noexcept {
		return !queue_.empty() || (plotWanted() && spectralMoved_.load(std::memory_order_relaxed));
	}
	/// THE PUBLISHING THREAD, about to hand the drawer's stored state to the engine (and publish its plot).
	///
	/// NO EDIT'S WAKEUP IS LOST. Each edit stores its values, then the flag with RELEASE; this takes the flag with an
	/// acq_rel EXCHANGE before the caller reads the state. Every edit falls on one side of the exchange in the flag's
	/// modification order: an edit whose `true` the exchange read synchronises with it, so its values are visible to the
	/// read that follows; an edit whose `true` lands after the exchange leaves the flag set, so hasPendingWork asks for
	/// another flush. (A relaxed store of false could erase an edit's true while the read after it still saw that edit's old
	/// values: nothing ordered the values before the clear, and the editor would wait for an unrelated flush.)
	void clearSpectralMoved() noexcept { (void)spectralMoved_.exchange(false, std::memory_order_acq_rel); }

	/// **THE EDITOR'S ONE UNDO HISTORY, AS THE PARAMETER ROAD SEES IT** (backlog JB, 2026-10-11). A hand on a
	/// parameter records its step here, whoever built the control: `ParamControl`'s value gesture, and every
	/// one-shot write through `writeParamAsEdit` (a menu pick, a stepper, a sound's switch). Held here rather
	/// than handed to each control because the parameter road has exactly one editor and one history, and the
	/// sites that wrote a parameter while recording nothing were controls nobody had handed one — a control
	/// that cannot be given the wrong history, or none, is the fix for that shape.
	///
	/// Set by `Gui` for its lifetime and cleared by it; null with no editor, and then nothing records.
	void setEditHistory(ui::EditHistory* history) noexcept { editHistory_ = history; }
	[[nodiscard]] ui::EditHistory* editHistory() const noexcept { return editHistory_; }

	//----------------------//
	// The engine's side //
	//----------------------//

	/// Publish both readings of every parameter for the editor to pick up. Audio thread while active,
	/// main thread while not — the same rule, and the same reason, as ParamSet's own.
	void publish(const ParamSet& params) noexcept;

	/// publish()'s inner half, taking the readings directly. Exists so the bridge can be tested
	/// without standing up a ParamSet, and so publish() has one place to write them.
	/// **A TEST SEAM, AND NAMED AS ONE (BH).** It was called `publishValues` and had NO CALLER IN
	/// `src/` at all — the production road is `publish(const ParamSet&)` above, which reads the
	/// same two numbers off a live ParamSet. A public method that only tests call reads as
	/// something the plugin uses, and one reviewer already recorded it as dead code on that basis.
	///
	/// **IT IS NOT DEAD AND MUST NOT BE DELETED.** Four cases in test_gui_bridge.cpp use it to
	/// publish ARBITRARY base/modulated pairs so they can exercise `refresh()`'s diffing — which
	/// values moved, and that only those are reported. Reaching those exact readings through a
	/// ParamSet would be testing a different thing. The seam is the point; the name was the bug.
	void publishValuesForTest(std::span<const double> base, std::span<const double> modulated) noexcept;
	/// TEST ONLY: put an fx device's control at a modulated value the way the audio thread's publish would, so a case
	/// can show a live position without running an engine. Widens the published row to cover `control`.
	void setFxModulatedForTest(size_t slot, size_t control, float value) noexcept;

	/// THE RATE THE ENGINE IS ACTUALLY RUNNING AT, so a surface that draws something rate-dependent
	/// draws the device the musician has rather than the one the developer had.
	///
	/// The filter card drew its curve at a hardcoded 48000, with a comment arguing that "a response
	/// plotted from 20 Hz to 20 kHz is the same picture at any rate a host would pick". That was true
	/// of every type the device had at the time — the SVFs are bilinear-warped and their shape in Hz
	/// barely moves — and stopped being true the day the Camp B models landed, because their corner is
	/// clamped at 0.3 x samplerate: 14.4 kHz at 48k, 13.2 kHz at 44.1k. MEASURED at up to 1.67 dB of
	/// disagreement between the drawn curve and the filter that plays, on an ordinary 44.1k session.
	///
	/// Set from activate(), which CLAP puts on the main thread; read by the editor on the same thread.
	/// Atomic anyway, because everything else that crosses this object is and a lone plain double
	/// here would be the exception somebody has to notice.
	void setSampleRate(double rate) noexcept {
		if (rate > 0.0) {
			sampleRate_.store(rate, std::memory_order_relaxed);
		}
	}
	/// 48000 until the host says otherwise — the same number the hardcode used, so an editor open
	/// before activate draws exactly what it drew before rather than something arbitrary.
	double sampleRate() const noexcept { return sampleRate_.load(std::memory_order_relaxed); }

	/// Hand every queued editor event to `apply`, oldest first, emptying the queue.
	template <typename Apply>
	void drain(Apply&& apply) noexcept {
		UiParamEvent event;
		while (queue_.pop(event)) {
			apply(event);
		}
	}

	const UiParamQueue& queue() const noexcept { return queue_; }

	//---------------------------------//
	// The audition orientation channel //
	//---------------------------------//

	/// THE EXPERIMENTAL ORIENTATION PIN, WHICH IS NOT A PARAMETER AND MUST NOT BECOME ONE.
	///
	/// A sound's plane is normally derived from its home, so nothing here is needed to play. This
	/// channel carries a plane held fixed BY HAND — the debug panel's sliders, or
	/// FEATHERS_AXIS_ROTATION — for hearing one deliberately. It stays by decision, and what it may not
	/// become is decided too: not CLAP params, though the controls stay in the UI to pin values
	/// experimentally, which also keeps rotate non-automatable exactly as the design always had it.
	///
	/// Plain relaxed atomics, eight of them, written by whoever holds the audition handle and read by
	/// the audio thread once a block. A read that straddles a write sees a mix of two orientations for
	/// one block, which during a live sweep is a plane between the two — indistinguishable from a
	/// frame of the sweep itself. Sequencing them would buy nothing an ear could hear and would cost
	/// the audio thread a fence.
	///
	/// Unpinned by default, which is a build that plays exactly as it would with none of this here.
	void setOrientation(const SliceOrientationPin& pin) noexcept;

	/// The same write, WITHOUT saying the patch changed — for a state load and for the environment
	/// seed at construction. A load is not an edit: marking dirty there tells the host its project was
	/// modified the instant it finished opening it, and the user gets a save prompt for having done
	/// nothing. The travel channel splits these two for the same reason; this is the orientation
	/// catching up.
	void restoreOrientation(const SliceOrientationPin& pin) noexcept;

	SliceOrientationPin orientation() const noexcept;

	/// THE REST OF THE AUDITION HANDLES, on the same channel and under the same rule — see
	/// audition_tuning.h. Same relaxed atomics for the same reason: a read that straddles a write sees
	/// one handle's old value beside another's new one, which during a sweep is a frame of the sweep.
	///
	/// `tuning()` is what the engine and the editor both read, so neither can be looking at a different
	/// audition from the other.
	void setFalloffPower(double power) noexcept;
	void setBlueShiftScales(double overshoot, double doppler) noexcept;
	/// The parity reference — see audition_tuning.h. Environment only, and deliberately no control.
	void setV1Crossfade(bool on) noexcept;
	AuditionTuning tuning() const noexcept;

	//----------------------------------//
	// The sounds' own travel — DRIFT     //
	//----------------------------------//

	/// Where a sound has drifted to, and how a gesture writes it. PATCH STATE rather than an audition
	/// handle: it is saved, it is part of what a sound is, and it outlives every question this strip
	/// exists to ask — see sound_travel.h.
	///
	/// THE BRACKET IS NOT DECORATION, and it is not the parameters' bracket either. A CLAP gesture
	/// tells the HOST that a run of parameter writes is one undoable edit, and there is no parameter
	/// here for it to say that about. What the bracket does instead is mark the plugin's STATE dirty
	/// when the drag ends, so the host knows the project changed and will ask for the patch again.
	/// Without it a drifted sound is a sound that silently fails to be saved — and the orientation pin
	/// has had exactly that hole since it landed, which this closes as well.
	void beginSoundTravelGesture() noexcept;
	void setSoundTravel(size_t slot, AnchorPoint offset) noexcept;
	/// The sound's polar offset from that place (`PolarOffset`, sound_travel.h) — the Radius and Theta
	/// corners. Same bracket, same dirtying, and read back as part of `soundTravel()`.
	void setSoundOrbit(size_t slot, PolarOffset orbit) noexcept;
	void endSoundTravelGesture() noexcept;

	/// The whole set, as the engine and the editor both read it once a block.
	SoundTravel soundTravel() const noexcept;

	//------------------------------------//
	// Which plane each sound is on — ROTATE //
	//------------------------------------//

	/// The per-sound generator channel, in the shape the travel one has and with the same bracket
	/// semantics: no host gesture, because there is no parameter underneath, and mark_dirty on the way
	/// out because this is patch state the host cannot otherwise see.
	///
	/// The GLOBAL pin on this bridge is a different thing and stays: it is the audition's handle for
	/// hearing one plane deliberately across every sound. A sound's own pin wins over it.
	void beginSoundOrientationGesture() noexcept;
	void setSoundOrientation(size_t slot, const SliceOrientationPin& pin) noexcept;
	void endSoundOrientationGesture() noexcept;

	SoundOrientations soundOrientations() const noexcept;

	/// Replace every entry at once — what a patch load does, and not an edit.
	void setSoundOrientations(const SoundOrientations& planes) noexcept;

	/// WHAT THE TURNS BANKED, on the same channel and inside the same bracket as the plane they belong
	/// to. A rotation writes both or it writes a bug: the plane alone is the A10 behaviour that was
	/// overruled, and the residue alone is a sound standing somewhere its plane never was.
	///
	/// No gesture calls of its own — the plane's bracket is the turn's bracket, and opening a second one
	/// around the same encoder click would be two edits where the player made one.
	void setSoundResidue(size_t slot, const PlaneResidue& banked) noexcept;

	SoundResidue soundResidue() const noexcept;

	/// Replace every entry at once — a patch load, and not an edit.
	void setSoundResidue(const SoundResidue& residue) noexcept;

	/// Replace every offset at once — what a patch load does. Not a gesture: nobody's hand is on it.
	void setSoundTravel(const SoundTravel& travel) noexcept;

	//--------------------------------------------//
	// How far along its line each sound has gone  //
	//--------------------------------------------//

	/// GAMMA'S UNBOUNDED HALF, on the same road and with the same bracket semantics as drift: no host
	/// gesture, because the base has no parameter underneath it to make one about, and mark_dirty on
	/// the way out because it is patch state the host cannot otherwise see.
	///
	/// The per-sound Gamma PARAMETER is untouched and stays the host's to automate. This is the coarse
	/// half the Explore corner accumulates into — see sound_gamma.h for why the split exists and what
	/// it costs.
	void beginSoundGammaGesture() noexcept;
	void setSoundGamma(size_t slot, double base) noexcept;
	void endSoundGammaGesture() noexcept;

	SoundGamma soundGamma() const noexcept;

	/// Replace every base at once — a patch load, and NOT an edit.
	///
	/// THE RESTORE-VS-SET SPLIT MATTERS HERE. A load must not leave the state dirty: it would tell the
	/// host the project changed the instant a patch was opened, and the host would ask for it back and
	/// mark an untouched session unsaved. The per-slot setter above is the edit and marks dirty; this
	/// one is the restore and does not.
	void restoreSoundGamma(const SoundGamma& gamma) noexcept;

	//------------------------------------//
	// The stances each sound remembers    //
	//------------------------------------//

	/// VARIATIONS — the mock's child rows, the subsounds. See sound_variations.h.
	///
	/// PATCH STATE, and the only channel here the AUDIO THREAD NEVER READS. The engine's placement is
	/// still `PlaceX`/`PlaceY`; a variation is a stance the editor remembers and can recall INTO those
	/// parameters. So these are plain members rather than atomics, and that is a statement rather than
	/// an omission: everything that touches them — the cards that edit them, the state save that
	/// writes them, the load that restores them — is on the main thread, which is where CLAP puts both
	/// the editor and state. An atomic here would suggest a reader that does not exist.
	///
	/// The bracket is the drift channel's: no host gesture, because there is no parameter underneath,
	/// and mark_dirty on the way out because this is patch state the host cannot otherwise see.
	void beginSoundVariationsGesture() noexcept;
	void setSoundVariations(size_t slot, const SoundVariations::List& list) noexcept;
	void endSoundVariationsGesture() noexcept;

	const SoundVariations& soundVariations() const noexcept { return variations_; }

	/// Replace every list at once — what a patch load does, and NOT an edit.
	void restoreSoundVariations(const SoundVariations& variations) noexcept;

	//---------------------------------//
	// Which colour each sound wears    //
	//---------------------------------//

	/// HUES — a sound's NAME rather than its sound. See sound_hues.h for why it is stored rather than
	/// derived from the slot it happens to be in.
	///
	/// THE SAME SHAPE AS VARIATIONS, and for the same reason: the audio thread never reads a colour,
	/// so these are plain members rather than atomics. Everything that touches them — the rail that
	/// draws them, the verbs that assign them, the state save and load — is on the main thread. An
	/// atomic here would suggest a reader that does not exist.
	///
	/// The bracket is the variations channel's: no host gesture, because there is no parameter
	/// underneath, and mark_dirty on the way out because this is patch state the host cannot otherwise
	/// see. A reorder that was not saved would put the colours back the next time the patch opened,
	/// which is the bug this whole change exists to end, arriving a session later.
	void beginSoundHuesGesture() noexcept;
	void setSoundHue(size_t slot, uint8_t index) noexcept;
	void endSoundHuesGesture() noexcept;

	const SoundHues& soundHues() const noexcept { return hues_; }

	/// Replace every colour at once — what a patch load does, and NOT an edit.
	void restoreSoundHues(const SoundHues& hues) noexcept;

	//------------------------------------------------------------------------------------------//
	// THE MACROS' POLARITY (backlog EG) — patch state beside the parameters, on the same rules   //
	//------------------------------------------------------------------------------------------//

	/// Whether macro `index` swings -1..+1 (rest at the middle) rather than 0..1.
	[[nodiscard]] bool macroBipolar(size_t index) const noexcept;
	/// THE SWITCH — an edit: the patch is marked modified and the editor repaints. The parameter under
	/// the macro is untouched; what changes is how it is read, everywhere at once.
	void setMacroBipolar(size_t index, bool bipolar) noexcept;
	[[nodiscard]] const MacroModes& macroModes() const noexcept { return macroModes_; }
	/// Replace every mode at once — what a patch load does, and NOT an edit.
	/// Not noexcept: it builds names and calls the rename listener.
	void restoreMacroModes(const MacroModes& modes);
	/// The same modes as one word, bit `i` for macro `i` — what `publishEditorLanes` hands the engine.
	[[nodiscard]] uint32_t macroBipolarMask() const noexcept;
	/// Which macros are Relative (backlog PQ), bit `i` for macro `i` — the engine reads those as resting at 0.
	[[nodiscard]] uint32_t macroRelativeMask() const noexcept;

	//------------------------------------------------------------------------------------------//
	// THE SPECTRAL DRAWER'S STORED STATE (additive design §3.1) — patch state beside the params  //
	//------------------------------------------------------------------------------------------//

	/// The master switch and the drawn curve, as one value — what the save writes and process() hands the engine. Each
	/// field is its own relaxed atomic (the gammas' road), so a block that reads across a hand's stroke may see part of
	/// it; the next block sees the rest, and nothing is ever read torn within a float.
	[[nodiscard]] SpectralState spectralState() const noexcept;
	/// THE MASTER SWITCH — an edit (the patch is modified, the editor repaints). Every operator's setting is kept.
	void setSpectralOn(bool on) noexcept;
	[[nodiscard]] bool spectralOn() const noexcept { return spectralOn_.load(std::memory_order_relaxed); }
	/// A SOUND'S drawn curve, its switch, and its bands — edits on the master switch's rules.
	[[nodiscard]] spectral::DrawnCurve spectralCurve(size_t slot) const noexcept;
	void setSpectralCurveOn(size_t slot, bool on) noexcept;
	void setSpectralCurveBand(size_t slot, size_t band, float gain) noexcept;
	/// Every band of a sound's curve back to unity (the drawer's Flatten): ONE edit.
	void flattenSpectralCurve(size_t slot) noexcept;
	/// A sound's whole curve at once — what a clone, a reorder or an undo writes (ui::SlotState): an edit when it moved.
	void setSpectralCurve(size_t slot, const spectral::DrawnCurve& curve) noexcept;
	/// Replace it all at once — what a patch load does, and NOT an edit.
	void restoreSpectralState(const SpectralState& state) noexcept;

	//------------------------------------------------------------------------------------------//
	// THE TERRAIN'S LIKES (backlog ON) — patch state beside the params, the editor's alone        //
	//------------------------------------------------------------------------------------------//

	/// Every like the patch holds. Nothing on the audio thread reads them: they change what Explore DRAWS, never what plays.
	[[nodiscard]] const TerrainLikes& terrainLikes() const noexcept { return terrainLikes_; }
	/// THE EDIT — a like added or removed, or an undo of one: the patch is modified and the editor repaints. Capped at
	/// kMaxTerrainLikes (the stored layout's length); the excess is dropped.
	void setTerrainLikes(const TerrainLikes& likes);
	/// Replace them all at once — what a patch load does, and NOT an edit.
	void restoreTerrainLikes(const TerrainLikes& likes);

	//------------------------------------------------------------------------------------------//
	// WHICH PRESET THE PATCH CAME FROM (backlog QC) — patch state, so the editor's face survives  //
	// an engine restart, a reopened project and a rebuilt editor                                  //
	//------------------------------------------------------------------------------------------//

	[[nodiscard]] const PresetIdentity& presetIdentity() const noexcept { return presetIdentity_; }
	/// The editor's browser naming the preset it loaded or saved (or none). NOT an edit: naming changes no sound.
	void setPresetIdentity(const PresetIdentity& identity) {
		if (identity != presetIdentity_) {
			presetIdentity_ = identity;
			++presetIdentitySerial_;
		}
	}
	/// What a patch load does — the same, under its own name so a reader can tell the two roads apart.
	void restorePresetIdentity(const PresetIdentity& identity) { setPresetIdentity(identity); }
	/// Moves on every change, so an open editor can tell a host's state load renamed the patch under it.
	[[nodiscard]] uint64_t presetIdentitySerial() const noexcept { return presetIdentitySerial_; }

	/// **THE LIKES' PHASE WARPS** (backlog ON, dsp/phase_warp.h) — derived on the message thread from each sound's likes (the
	/// editor's Explore stage derives them, terrain.h `deriveLikeWarp`) and handed to the audio thread, which builds its
	/// characters through them (Synth::attachLikeWarps). A TRIPLE BUFFER: the writer fills a back set and swaps it into the
	/// mailbox; the reader swaps the mailbox into its front only when it is fresh. No allocation, no lock, and no pointer is
	/// ever an identity — a set carries its own serial.
	struct LikeWarpSet {
		uint64_t serial = 0;
		std::array<uint8_t, kMaxSounds> engine {};
		std::array<LikeWarp, kMaxSounds> warps {};
	};
	/// Main thread: one sound's warp, for an engine; the whole set is published at once.
	void publishLikeWarp(size_t slot, uint8_t engine, const LikeWarp& warp);
	/// Main thread: what was last published for a sound (the editor draws through the same value).
	[[nodiscard]] const LikeWarp& likeWarpOf(size_t slot) const noexcept { return likeMain_.warps[std::min(slot, kMaxSounds - 1)]; }
	[[nodiscard]] uint8_t likeEngineOf(size_t slot) const noexcept { return likeMain_.engine[std::min(slot, kMaxSounds - 1)]; }
	/// Main thread: the published set's serial (it moves on every change).
	[[nodiscard]] uint64_t likeSerial() const noexcept { return likeMain_.serial; }
	/// Audio thread: the newest published set.
	[[nodiscard]] const LikeWarpSet& takeLikeWarps() noexcept;

	//------------------------------------------------------------------------------------------//
	// THE SPECTRUM DRAWER'S PLOT (additive design §4.2, D2) — engine to editor, while it is open //
	//------------------------------------------------------------------------------------------//
	/// WHETHER THE EDITOR'S DRAWER IS OPEN, so the engine publishes its plot (publishEngineReadings asks this first, and
	/// builds nothing while it is false). The editor sets it on opening the drawer and clears it on closing the drawer, on
	/// leaving Explore, and on closing the editor (ui::ExploreStage) — a closed drawer costs the audio thread one load.
	///
	/// OPENING (false -> true) ASKS FOR A FRESH FRAME: a sleeping plugin runs no process(), so it marks the plot stale
	/// (`hasPendingWork`, which asks the host for the flush that publishes it), and the next publish goes out under a new
	/// serial even if it equals the last one — the editor builds a fresh panel on every open, whose plot takes no frame
	/// published before it (ui::SpectrumPlot), and must not wait for a change to see one again.
	void setPlotWanted(bool wanted) noexcept {
		if (!plotWanted_.exchange(wanted, std::memory_order_acq_rel) && wanted) {
			plotRepublish_.store(true, std::memory_order_release);
			spectralMoved_.store(true, std::memory_order_release);
		}
	}
	[[nodiscard]] bool plotWanted() const noexcept { return plotWanted_.load(std::memory_order_relaxed); }
	/// THE PUBLISHING THREAD (the audio thread, or a flush: never both at once, which makes this the single writer).
	/// Publishes `frame` under a new serial when it differs from the last one published; a frame equal to it publishes
	/// nothing, so a held note costs the editor no redraw — except the first publish after the drawer opens, which always goes
	/// out (`setPlotWanted`). Answers whether it published.
	bool publishPlot(const SpectralPlotFrame& frame) noexcept;
	/// THE EDITOR. The newest whole frame into `out`, and its serial; false (both untouched) when none has been published
	/// or the writer was mid-frame — the next poll takes it. Never a frame torn between two publishes (gui_bridge.cpp).
	bool readPlot(SpectralPlotFrame& out, uint32_t& serial) const noexcept;
	/// The serial of the newest frame begun, so a poll can skip the copy when nothing is new.
	[[nodiscard]] uint32_t plotSerial() const noexcept { return plotSerialB_.load(std::memory_order_relaxed); }

	/// How macro `index`'s control drives its parameter (`device/macro_type.h`); Knob out of range.
	[[nodiscard]] device::MacroType macroType(size_t index) const noexcept;
	/// THE TYPE SWITCH — an edit on setMacroBipolar's rules. A type past the last is refused, not clamped:
	/// the menu offers only real ones, so anything else is a caller's bug and moves nothing.
	void setMacroType(size_t index, device::MacroType type) noexcept;
	/// **WHAT A MACRO IS CALLED IN PROSE** — the name given to it, or "Macro N" when there is none. The one
	/// authority `modSourceName` asks, so the route list, the tray and the Engine rail cannot disagree.
	[[nodiscard]] std::string macroName(size_t index) const;
	/// Whether the macro carries a name of its own rather than the default.
	[[nodiscard]] bool macroRenamed(size_t index) const noexcept;
	/// THE RENAME — an edit on setMacroBipolar's rules. Clamped by `device::clampMacroName`; empty, or the
	/// default spelled out, clears the name, so "renamed" always means a name that differs from the default.
	void setMacroName(size_t index, std::string_view name);
	/// **TOLD WHEN A MACRO'S NAME MOVES** — by setMacroName when it changes and by restoreMacroModes for each
	/// macro whose effective name changed, on the main thread, after the bridge holds the new name. The
	/// host hangs its parameter-info rescan here (A9) and the editor its captions, so it is a LIST: each
	/// subscriber holds the token that ends it, and the editor closing ends only the editor's.
	[[nodiscard]] device::Subscription subscribeMacroRename(std::function<void(size_t)> listener) {
		return macroRenameSubscribers_.subscribe(std::move(listener));
	}

	/// **THE HOST'S OWN SOURCES, DECLARED** — one `SourceDesc` per macro and per sound weight, so a
	/// `SourceView` built over this span answers `isBipolar` for a macro the way it answers for a
	/// device's output. `bridge_device_host.h`'s note said the core span was empty because every core
	/// source was unipolar and warned this would go quietly wrong the day one was not; that day is EG.
	[[nodiscard]] std::span<const device::SourceDesc> coreSourceDescs() const noexcept { return coreSourceDescs_; }

	//----------------------//
	// What is in the fx lane //
	//----------------------//

	/// WHAT THE LANE HOLDS AND WHAT EACH DEVICE IS SET TO.
	///
	/// THE SETTINGS ARE HERE NOW, WHICH THEY WERE NOT, and the sentence that used to stand here said
	/// why they were not: "not their settings — every one of those is a parameter, and a copy of one
	/// here would be a second source of truth for it." That was true while the filter's controls were
	/// clap params. The swallowing takes them off the parameter list, at which point this is not a
	/// second source of truth, it is the ONLY one — exactly as the rack's already is.
	///
	/// **A VIEW OF THE RECORD, MESSAGE THREAD ONLY** (chain-as-a-device 5b, C4). It was a flattened
	/// table of relaxed atomics the audio thread read every block; the audio thread now takes the
	/// record's published state whole (`publishedChains()`) and never calls this.
	FxChain fxChain() const noexcept;

	/// **WHAT THE AUDIO THREAD READS — the record's state, published after every change, taken whole
	/// or not at all.** `publishEditorLanes` snapshots it into the synth when its sequence moves.
	[[nodiscard]] const rack::PublishedChainState& publishedChains() const noexcept { return publishedChains_.current(); }
	/// **ROOM FOR AT LEAST `devices` IN `chain`** (`kLaneChain`, `kStripChain`) — MESSAGE THREAD, allocates
	/// (unbounded devices step 5). The record grows (`ChainRecord::growChain`) and the state is published in its
	/// new shape; the synth grows to match at its next provision and takes it at the drain after.
	bool reserveChain(size_t chain, size_t devices);
	/// How many devices `chain` holds before it must grow.
	[[nodiscard]] size_t chainCapacity(size_t chain) const noexcept { return chains_.capacity(chain); }
	/// THE RECORD ITSELF, for a case.
	[[nodiscard]] const rack::ChainRecord& chainRecordForTest() const noexcept { return chains_; }
	/// THE RECORD, FOR THE GATE A CHAIN IN THE LANE IS HANDED (rack roadmap §7) — the plugin entry
	/// point wires `Synth::setOuterGateMaker` over it, once, before provisioning.
	[[nodiscard]] const rack::ChainRecord& chainRecord() const noexcept { return chains_; }
	/// **THE LANE CONTAINERS' OWN CONTROLS, SERVICED** (backlog NO, `rack::serviceOwnControls`): released ones pruned, a mirror written
	/// — on the plugin's service beat, after the containers' own service. Returns how many routes went.
	size_t serviceOwnControls() {
		return rack::serviceOwnControls(chains_, fxBodies_, [this](backplane::Source s, backplane::Destination d) {
			setRouteAmount(s, d, 0.0f);
		});
	}
	/// **THE RECORD'S STATE, MESSAGE THREAD** — what provisioning builds from, beside the publish it went
	/// out as (`publishedChains().sequence()`).
	[[nodiscard]] const rack::ChainState& chainState() const noexcept { return chains_.state(); }

	/// HOW MANY DEVICES THE LANE HAS, without copying the lane. fxChain() builds a ~4 KB FxChain from
	/// about a thousand relaxed loads; a caller that only wants the count -- FxLane::deviceCount(), asked
	/// several times per card per idle frame -- was paying for all of them (backlog T). The same clamped
	/// read fxChain() starts with, so the two can never disagree about how many rows are live.
	size_t fxDeviceCount() const noexcept;

	/// ONE FIELD OF ONE DEVICE, AND HOW MANY FIELDS ITS ROW CARRIES -- one relaxed load each, with the
	/// clamps fxChain() applies, so a value read here is the value a copy of the chain would carry.
	///
	/// CONSUMED BY NOTHING YET (2026-09-03), ON PURPOSE. BridgeDeviceHost::fieldAt and fieldsPresentAt
	/// copy the WHOLE chain per field (backlog BL: MEASURED ~10 copies per card per idle frame, ~80 for
	/// a full lane, 5.85 us and 14-40 us respectively), and that seam header is hosting's and under
	/// edit for BK -- so the readers land here first and the two overrides move onto them in that
	/// commit, rather than two authors meeting in one header the same night. Out of range answers what
	/// the copy's row answers: zero.
	float fxFieldAt(size_t slot, size_t control) const noexcept;
	size_t fxFieldsPresentAt(size_t slot) const noexcept;

	/// **THE RACK'S THREE, THE SAME QUESTION ASKED OF THE OTHER CHAIN (BL, BK).**
	///
	/// The device seam used to answer `fieldAt` by copying a WHOLE chain per FIELD — `const ModRack
	/// rack = bridge_.modRack();` on every call — so one modulator card's idle update copied the rack
	/// once per control it draws. inside measured the fx twin of this and it is the same defect one
	/// host down; these are the readers that retire it on the rack side.
	///
	/// `rackModulatedFieldAt` is BK's: WHERE MODULATION LEFT the control, so a card can draw the
	/// moving part of its ring from the engine's own arithmetic instead of recomputing the offsets on
	/// the far side of a thread boundary.
	float rackFieldAt(size_t slot, size_t control) const noexcept;
	size_t rackFieldsPresentAt(size_t slot) const noexcept;
	float rackModulatedFieldAt(size_t slot, size_t control) const noexcept;

	/// HOW MANY TIMES fxChain() HAS BEEN COPIED, so a case can ASSERT the number of copies a road makes
	/// rather than infer it from a timing. UI-SIDE ONLY: fxChain() is never called from the audio
	/// thread (its own comment says why the audio thread reads the flattened atomics instead), so this
	/// is a plain counter and touches nothing the audio thread reads.
	size_t fxChainCopiesForTest() const noexcept { return fxChainCopies_; }

	/// WHAT ONE DEVICE IN THE LANE IS SET TO. The lane's half of setEnvelopeSettings, and the road a
	/// card's control takes.
	///
	/// TAKES A `State` RATHER THAN A `Settings`, and the difference is a silent data loss rather than
	/// a matter of taste. `Settings` is what the DSP reads and it deliberately omits Keytrack, because
	/// keytracking decides how many filters there are and where they sit — the owner's architecture
	/// rather than a number the sample loop reads. But Keytrack IS a declared control and IS patch
	/// state, so a channel that took `Settings` would drop that bit on every write, and it would read
	/// back as "the user had keytracking off", which is a plausible patch rather than a corrupt one.
	/// f7 flagged this before either of us wrote a line; see filter_types.h's State.
	void setFilterSettings(size_t slot, const fx::filter::State& state) noexcept;

	/// ONE CONTROL OF WHATEVER IS IN A SLOT, addressed by the device's own control index.
	///
	/// THE DEVICE-GENERIC ROAD, and the reason it had to exist: setFilterSettings above names a type,
	/// so the lane's second device could not be edited through it at all. A row is every device's own
	/// indexed view of itself in its own declared units, so writing one field of it needs the
	/// DESCRIPTOR and never the device's type — which is the same argument Synth::modulatedRowAt makes
	/// on the engine's side of the boundary.
	///
	/// COERCED HERE, by the control's own declaration, so a widget may hand over whatever its drag
	/// produced without knowing the range. A device this build cannot name is refused rather than
	/// written blind.
	///
	/// **ADDRESSED BY THE DEVICE'S ID, IN EITHER CHAIN (kill-lanes step 2).** It was two roads — one by
	/// fx LANE, one by mod-rack SLOT, two spaces that had to be kept apart by two hosts — and before that
	/// one by fx POSITION. An id has one space: where the record keeps the device is its own business,
	/// found inside, once.
	void setDeviceControl(DeviceInstanceId device, size_t control, float value) noexcept;
	/// **THE FALLS A QUICK PRESS LEFT WAITING, LANDED** once the synth has adopted the chains publish `adopted` that carried
	/// their rises (`rack::HeldFalls`). MESSAGE THREAD: the plugin's visit, with `Synth::chainsAdopted`.
	void landHeldControls(uint32_t adopted) noexcept;
	/// A MOMENTARY'S RELEASE (`DeviceHost::releaseField`): `setDeviceControl`, held until the synth has the rise.
	void releaseDeviceControl(DeviceInstanceId device, size_t control, float value) noexcept;

	/// ONE FLOAT OF A DEVICE'S OPAQUE STATE BLOB, by its offset WITHIN the blob. Deliberately not
	/// `setDeviceControl`: that road coerces against a declaration and this one has none to coerce
	/// against. See the definition for why widening the other guard would have been wrong.
	void setDeviceState(DeviceInstanceId device, size_t index, float value) noexcept;

	/// **A SLOT OR A POSITION IS NOT AN ID, AND A `size_t` WOULD NARROW INTO ONE WITHOUT A WORD** —
	/// `DeviceInstanceId` is a `uint32_t`. Every road on this bridge that names a device by id refuses a
	/// `size_t` in that place, so a caller still holding its old address fails to compile rather than
	/// editing whichever device wears that number (the same guard `backplane::DeviceHost` keeps). A case
	/// caught it the first hour: an "add before" handed the anchor's lane and inserted before nothing.
	void setDeviceControl(size_t, size_t, float) noexcept = delete;
	void setDeviceState(size_t, size_t, float) noexcept = delete;
	size_t insertFxDeviceBefore(size_t, const char*) noexcept = delete;
	size_t insertRackDeviceBefore(size_t, const char*) noexcept = delete;
	size_t fxSlotOf(size_t) const noexcept = delete;
	size_t rackSlotOf(size_t) const noexcept = delete;
	const device::Descriptor* descriptorOf(size_t) const noexcept = delete;
	std::vector<backplane::Route> routesOf(size_t) const = delete;

	/// Replace the lane at once — what a patch load does, and NOT an edit.
	void restoreFxChain(const FxChain& chain) noexcept;

	/// **WHAT EACH CONTAINER IN THE LANE HOLDS** — its body, keyed by the instance id its row carries.
	///
	/// MESSAGE THREAD ONLY, AND THAT IS WHY IT IS A PLAIN MEMBER BESIDE A TABLE OF ATOMICS. The lane's
	/// rows are atomics because the audio thread reads them every block; a body never crosses to the
	/// audio thread at all. It is saved, loaded and consumed at provisioning, and all three are main-
	/// thread roads — `provisionAndDrain`'s comment says so in as many words, because provisioning
	/// allocates. A body in the flattened channel would put variable-size bytes on the audio path.
	///
	/// A BODY FOR A DEVICE NO LONGER IN THE LANE IS HARMLESS AND IS NOT SAVED: `chainsPatch` carries a
	/// body only for a row that is present, and instance ids are never reissued, so a stale entry
	/// cannot come back attached to a different device. It costs memory until the next load replaces
	/// the set.
	[[nodiscard]] const ContainerBodies& fxBodies() const noexcept { return fxBodies_; }
	/// THE SAME, FOR THE PARTY THAT PROVISIONS: the converger files each running container's handle here
	/// (`ContainerBodies::attach`), on the message thread, beside the body it was built from.
	[[nodiscard]] ContainerBodies& fxBodies() noexcept { return fxBodies_; }

	/// Replace every container's contents at once — what a patch load does, beside `restoreFxChain`.
	void restoreFxBodies(ContainerBodies bodies) { fxBodies_ = std::move(bodies); }

	/// A HAND'S BLOCK was written (`DeviceHost::setBlock`): the patch is edited, as a control's write is.
	void markStateDirty() noexcept { stateDirty_.store(true, std::memory_order_relaxed); }

	/// **A CHAIN ACCESS SITE FOR DEVICE `device`** (backlog NR) — filed empty under `siteKeyOf(device)`, which is "it has
	/// one", and the next visit's provision builds it (`Converger`'s sites). False for a device that offers none: not
	/// an audio device, or one that declares none (SKLITTER and Resonate; D3 lifted for the Rack and the Chain, 2026-10-29) (`rack::siteLanesOf`'s rule).
	bool openFxSite(DeviceInstanceId device) {
		const device::Descriptor* d = descriptorOf(device);
		if (d == nullptr || d->audio == nullptr || d->traits.siteLanes == device::Traits::SiteLanes::None) {
			return false;
		}
		if (!fxBodies_.has(siteKeyOf(device))) {
			(void)fxBodies_.store(siteKeyOf(device), nullptr, 0);
			stateDirty_.store(true, std::memory_order_relaxed);
		}
		return true;
	}

	/// THE DEVICE'S SITE REPLACED WHOLE — `DeviceHost::loadSite` for the fx lane: the running site's `applyState`, or the
	/// bytes filed for the next visit's provision to build it from.
	bool loadFxSite(DeviceInstanceId device, std::span<const uint8_t> body);

	/// A SOURCE IN DEVICE `owner`'s PRE CHAIN AS THIS HOST NAMES IT (backlog NZ) — `DeviceHost::siteExport`.
	backplane::Source fxSiteExport(DeviceInstanceId owner, backplane::Source inner);
	/// A source inside the Chain or Rack `container` in the fx lane, as the lane names it (backlog NZ (b)).
	backplane::Source fxContainerExport(DeviceInstanceId container, backplane::Source inner);
	/// **RELEASE THE EXPORT ROWS NO ROUTE READS ANY MORE** (backlog NZ: Feathers' lane, a container's table and a site's
	/// own), MESSAGE THREAD, on the plugin's service beat — `ExportTable::sweep`'s rule. A row's readers: a route in this
	/// bridge reading it, and a route inside any container or site the lane holds. `drained` is the routes generation the
	/// audio thread last adopted (`ParamSet::routesGeneration`), against this bridge's `routesGeneration()`. Returns how
	/// many rows it freed.
	size_t sweepExports(uint32_t drained);

	/// **IS THIS DEVICE'S CARD FOLDED** — view state that travels in the patch (`rack_state.h` says why a fold could not stay on the desk while the rails do). THE BRIDGE IS
	/// WHERE IT LIVES, not the panel: a panel's own set would outlive a load, and ids are patch-scoped, so
	/// the incoming patch's device 1 would inherit the outgoing one's fold. Message thread; no engine reads
	/// it and nothing here is published.
	[[nodiscard]] bool deviceFolded(DeviceInstanceId device) const noexcept {
		return (cardViews_.flagsOf(device) & rack::kViewFolded) != 0;
	}

	/// **AND A FOLD IS AN EDIT**, because the patch now carries it: a host that asks is told the project is
	/// modified, exactly as a knob does. It is not a sound change and not a repaint the engine owes — the
	/// panel redraws itself — so it marks the state dirty and nothing else.
	void setDeviceFolded(DeviceInstanceId device, bool folded) noexcept;

	/// **WHAT A DEVICE'S OUTPUT IS CALLED ON THIS INSTANCE** — the player's label, else the name the device
	/// declares for it, else "Source" (no such device or output). Per instance, so two MACROS cards name
	/// their outputs apart; held here beside the folds and saved with them (`rack::DeviceLabels`). The one
	/// authority `modSourceName` asks. Message thread.
	[[nodiscard]] std::string outputLabel(DeviceInstanceId device, size_t output) const;
	/// Whether the output carries a label of its own rather than its declared name.
	[[nodiscard]] bool outputRelabelled(DeviceInstanceId device, size_t output) const noexcept {
		return !outputLabels_.labelOf(device, output).empty();
	}
	/// **THE RELABEL** — `setMacroName`'s rules: clamped (`device::clampMacroName`), and empty or the declared
	/// name spelled out clears it, so "relabelled" always means a label that differs. An output the device
	/// does not declare is ignored; an unchanged label moves nothing. Otherwise an edit — the patch is
	/// modified, and the editor repaints because every route list naming this source reads differently —
	/// and `subscribeOutputLabel` is told.
	void setOutputLabel(DeviceInstanceId device, size_t output, std::string_view label);
	/// WHICH PRESET A DEVICE'S PANEL LAST LOADED OR SAVED (backlog QC) — `rack::DeviceLabels::presetOf`, saved with the
	/// device in the chains block. Not an edit: naming changes no sound.
	[[nodiscard]] PresetIdentity devicePreset(DeviceInstanceId device) const {
		const PresetIdentity* p = outputLabels_.presetOf(device);
		return p != nullptr ? *p : PresetIdentity {};
	}
	void setDevicePreset(DeviceInstanceId device, const PresetIdentity& identity) {
		(void)outputLabels_.setPreset(device, identity);
	}
	/// **TOLD WHEN AN OUTPUT'S LABEL MOVES** — by `setOutputLabel` on a real change and by a patch load
	/// (`restoreChains`) once per (device, output) whose label differs from before, after the bridge holds
	/// the new one. A list, `subscribeMacroRename`'s shape and reason.
	[[nodiscard]] device::Subscription
	subscribeOutputLabel(std::function<void(DeviceInstanceId, size_t)> listener) {
		return outputLabelSubscribers_.subscribe(std::move(listener));
	}
	/// **WHICH OF A DEVICE'S OUTPUTS ITS CARD SHOWS** (`DeviceHost::outputsShown`; Pattern design §7.10) — view state, never
	/// saved: a card sets it whenever it is built. A change tells the output-label listeners once, so each host re-lays.
	[[nodiscard]] uint64_t outputsShown(DeviceInstanceId device) const noexcept {
		const auto it = outputsShown_.find(device);
		if (it != outputsShown_.end()) {
			return it->second;
		}
		const device::Descriptor* d = descriptorOf(device);
		return d != nullptr ? d->outputsShownByDefault() : ~uint64_t { 0 };
	}
	void setOutputsShown(DeviceInstanceId device, uint64_t mask) {
		const uint64_t was = outputsShown(device);
		if (was == mask) {
			return;
		}
		const device::Descriptor* d = descriptorOf(device);
		if (d != nullptr && mask == d->outputsShownByDefault()) {
			outputsShown_.erase(device);
		}
		else {
			outputsShown_[device] = mask;
		}
		outputLabelSubscribers_.notify(device, static_cast<size_t>(std::countr_zero(was ^ mask)));
	}

	/// **HOW THE CANVAS IS SHOWN** — its extent and its fold, in the patch like a card's fold (2026-09-13: the partition size and expanded state persist with the patch).
	/// An edit when it
	/// changes, as a fold is. Message thread.
	[[nodiscard]] rack::CanvasView canvasView() const noexcept { return canvasView_; }
	void setCanvasView(rack::CanvasView view) noexcept;

	/// **BOTH CHAINS AND EVERY ROUTE, AS THE ONE FORMAT'S PATCH** (chain-as-a-device item 6) — the
	/// record's devices, rows and ids in each chain's order, each container's body, and every route this
	/// bridge published, named by identity (`feathersCoreCodec`). What the state road saves. Message thread.
	///
	/// **AND THE HOST SLOTS, WHEN `withSlots`** (plan §2): the pool's snapshot rides as `hostSlots`, written
	/// only when some slot is mapped. A PRESET is saved without them (plan §0.8: presets do not carry
	/// slots): the pool belongs to the project, whose host lanes are keyed on its ids.
	[[nodiscard]] rack::RackPatch chainsPatch(bool withSlots = true) const;

	/// WHAT A LOAD DOES TO THE HOST SLOT POOL (decided 2026-09-25). `Restore` is the host's
	/// `state.load` — a project opening — and puts the patch's pool in place (an absent one is an empty
	/// pool). `Keep` is a preset: the project's pool stays, and only the slots whose targets the new chains
	/// no longer hold are freed.
	enum class SlotsOnLoad : uint8_t { Restore, Keep };

	/// **AND A PATCH BACK INTO THE RECORD** — everything replaced at once: devices where the patch put them
	/// (by id, in each chain's order, as far as the chain reaches), bodies, routes through the record's
	/// gate. A patch
	/// with nothing in it (`present == false`: a stream from before this block) puts the chains a fresh
	/// instance has. Publishes; NOT an edit. Returns what did not load as saved (the load report, backlog
	/// JF), which the caller notes.
	///
	/// THE SLOTS AFTER THE CHAINS, never before: `applyPatchToRecord` may re-id a device (plan §0.7), so the
	/// pool is restored (or kept) against the record as it stands afterwards, and every slot whose target no
	/// longer resolves is freed (`reconcileSlots`).
	rack::LoadReport restoreChains(const rack::RackPatch& patch, SlotsOnLoad slots = SlotsOnLoad::Restore);

	//------------//
	// Host slots //
	//------------//

	// **THE POOL, ITS ROADS AND ITS SERVICE ARE `rack::SlotOwner`'S** (`rack/slot_owner.h`), over this
	// bridge's record — the one the standalone rack's bridge holds too (T10), so the two plugins keep one set
	// of rules. Everything below is that object's, reached from here; the contracts are written there once.
	// OWNED HERE (plan §3: "In Feathers the owner is `ParamBridge`"), beside the record whose devices its
	// targets name, because every road that removes, re-inserts or reloads a device runs through this class.
	// Message thread only, like the record, except where the owner says otherwise.

	[[nodiscard]] hostslots::SlotPool& slotPool() noexcept { return slots_.pool(); }
	[[nodiscard]] const hostslots::SlotPool& slotPool() const noexcept { return slots_.pool(); }

	/// THE LOCK-FREE CELLS BETWEEN THE HOST'S EVENTS AND THE ENGINE, whose address is fixed for the bridge's
	/// life — the plugin's event road and the engine's application (`setHostSlotBus`) both hold it.
	[[nodiscard]] hostslots::SlotValueBus<>& slotBus() noexcept { return slots_.bus(); }
	[[nodiscard]] const hostslots::SlotValueBus<>& slotBus() const noexcept { return slots_.bus(); }

	/// `SlotOwner::targetOf` — path `[device]`: Feathers' chains are the outermost level.
	[[nodiscard]] std::optional<hostslots::Target> slotTargetOf(DeviceInstanceId device, size_t control) const {
		return slots_.targetOf(device, control);
	}
	/// `SlotOwner::labelOf` — "Filter · Cutoff", numbered only beside a twin in either chain.
	[[nodiscard]] std::string slotLabelOf(DeviceInstanceId device, size_t control) const {
		return slots_.labelOf(device, control);
	}
	/// `SlotOwner::resolve` — a device in either chain, of the target's tag, declaring its key.
	bool resolveSlotTarget(const hostslots::Target& target, const device::ParamDesc** desc, double* value) const {
		return slots_.resolve(target, desc, value);
	}
	/// `SlotOwner::textSite`, with the instance this bridge files for the device (`fxBodies`) — where a slot's
	/// target prints its own words in the DAW (rack stage 2, §3).
	bool slotTextSite(const hostslots::Target& target, hostslots::TextSite* site) const {
		DeviceInstanceId device = kNoInstance;
		if (!slots_.textSite(target, &device, site)) {
			return false;
		}
		site->surface = fxBodies_.surfaceOf(device);
		return true;
	}
	/// `SlotOwner::reconcile`. From the main-thread service it catches the roads that change a device without
	/// removing it: `storeRowOf`'s replacement (a different tag under the same id) and `rebuildChain`'s
	/// `replaceChain` (which can drop devices).
	uint32_t reconcileSlots() { return slots_.reconcile(); }
	uint32_t reconcileSlotsIfMoved() { return slots_.reconcileIfMoved(); }

	//--------------------------------------------------------------------------------------------//
	// HOST SLOTS: THE WRITE ROADS (plan §1.2, task T6b) — `SlotOwner`'s banner has the four roads.  //
	// This bridge's share: `setDeviceControl` is the hand's road (`SlotOwner::onHandWrite`), and     //
	// `storeRowOf` (the filter, envelope and MSEG settings roads) and `rebuildChain` seed the bound  //
	// controls they CHANGED (`SlotOwner::captureBound` / `seedChangedBound`) before they publish.   //
	//--------------------------------------------------------------------------------------------//

	using SlotOutbox = hostslots::SlotOutbox<>;

	/// THE EDITOR-TO-HOST RING FOR SLOTS. The bridge produces (main thread); the plugin drains it in `process`
	/// and `params.flush` (`hostslots::drainOutbox`).
	[[nodiscard]] SlotOutbox& slotOutbox() noexcept { return slots_.outbox(); }

	/// `SlotOwner::descOf` — for the audio thread, lock-free.
	[[nodiscard]] const device::ParamDesc* slotDescOf(size_t n) const noexcept { return slots_.descOf(n); }

	/// `SlotOwner::setHostRecording` — written by the audio thread every block.
	void setHostRecording(bool recording) noexcept { slots_.setHostRecording(recording); }
	[[nodiscard]] bool hostRecording() const noexcept { return slots_.hostRecording(); }

	/// `SlotOwner::beginGesture` / `endGesture`, from `BridgeDeviceHost`. Main thread.
	///
	/// **A FIRST GRAB IS SERVICED HERE, NOT A FRAME LATER.** When the `touchControl` just before this took a NEW slot,
	/// the slot is bound and the host told at once (`setFirstGrabService`, the plugin's main-thread visit): the
	/// rebind queues the takeover's Value, then this hand's Begin, and the visit's rescan names the slot BEFORE the
	/// next block sends them. Left to the next visit, the Begin went out a frame late on a slot the host still knew
	/// as a hidden "Slot N", and a host's recently-touched list did not put the control on top. AFTER `beginGesture`,
	/// so the rebind sees the hand down and keeps its gesture open.
	void beginControlGesture(DeviceInstanceId device, size_t control) noexcept {
		slots_.beginGesture(device, control);
		if (std::exchange(firstGrab_, false) && firstGrabService_) {
			firstGrabService_();
		}
	}
	void endControlGesture(DeviceInstanceId device, size_t control) noexcept { slots_.endGesture(device, control); }
	/// **A HAND TOOK A CONTROL: THE RECENT RING** (T9) — `SlotOwner::touch`. A new mapping is a change to the
	/// saved pool, so it marks the patch dirty; a recency bump alone does not. Main thread.
	void touchControl(DeviceInstanceId device, size_t control) noexcept {
		firstGrab_ = markedIf(slots_.touch(device, control));
	}
	/// What `beginControlGesture` runs after a first grab: the owner's main-thread visit. Unset, a first grab waits
	/// for the next visit, as before. Main thread.
	void setFirstGrabService(std::function<void()> service) { firstGrabService_ = std::move(service); }
	/// **WHAT THE TRAY SHOULD SAY ABOUT THE POOL** (T9) — `SlotOwner::takeNotice`: a recycled Recent slot, once
	/// no hand is down. The editor's tick asks. Empty when there is nothing to say.
	[[nodiscard]] std::string takeSlotNotice() { return slots_.takeNotice(); }

	/// `SlotOwner::boundTo`.
	[[nodiscard]] hostslots::SlotIndex slotBoundTo(DeviceInstanceId device, size_t control) const noexcept {
		return slots_.boundTo(device, control);
	}

	/// **THE RIGHT-CLICK'S HOST ROWS** (T7) — `SlotOwner::statusOf`, `expose`, `unpin` (no release: 2026-09-26); the
	/// logic is the owner's, shared with the standalone rack. Each verb that changed the pool MARKS THE PATCH
	/// DIRTY here: the pool is saved with the project. The host hears a slot's new name at the next visit.
	[[nodiscard]] device::HostSlotStatus hostSlotStatusOf(DeviceInstanceId device, size_t control) const {
		return slots_.statusOf(device, control);
	}
	bool exposeToHost(DeviceInstanceId device, size_t control, std::string* displaced) {
		return markedIf(slots_.expose(device, control, displaced));
	}
	bool unpinFromHost(DeviceInstanceId device, size_t control) { return markedIf(slots_.unpin(device, control)); }
	/// The pool's generation — a knob's mark caches on it (`Knob::hostMarked`).
	[[nodiscard]] uint64_t hostSlotGeneration() const noexcept { return slots_.pool().generation(); }

	/// **THE GATE, FOR A CONTAINER IN THE LANE** — the record's answer (`rack::ChainRecord::gateOver`) and THIS
	/// bridge's dirty flag, so an edit inside a container (a Rack's chain added, its level moved; backlog NO and JV)
	/// tells the DAW Feathers' patch changed. The maker the plugin hands `Synth::setOuterGateMaker`. `bridge` is a
	/// `ParamBridge*`.
	static device::OuterGate outerGateOver(const void* bridge, DeviceInstanceId self) noexcept {
		const auto* b = static_cast<const ParamBridge*>(bridge);
		device::OuterGate gate = rack::ChainRecord::gateOver(&b->chains_, self);
		gate.dirtyHost = const_cast<ParamBridge*>(b); // a bridge is never const-defined
		gate.markDirty = [](void* host) { static_cast<ParamBridge*>(host)->stateDirty_.store(true, std::memory_order_relaxed); };
		// AND ITS OWN CONTROLS' MIRROR (backlog NO: a Rack's chain level, mute and solo), written as any control is.
		gate.setOwnField = [](void* host, DeviceInstanceId self, size_t control, float value) {
			auto* bridge = static_cast<ParamBridge*>(host);
			if (!bridge->chains_.storesField(self, control, value)) { // the value already there is no edit
				bridge->setDeviceControl(self, control, value);
			}
		};
		return gate;
	}

	/// **THE POOL, FOR A CHAIN IN THE LANE** (host slots T11) — the maker the plugin hands `Synth::setSlotLinkMaker`
	/// over this bridge, beside the gate: a control inside a rack in the lane takes its slot from THIS pool, by
	/// path, and gestures and values reach the host on it. `bridge` is a `ParamBridge*`.
	static device::HostSlotLink slotLinkOver(const void* bridge, DeviceInstanceId self) noexcept {
		return const_cast<ParamBridge*>(static_cast<const ParamBridge*>(bridge))->slots_.linkFor(self);
	}
	/// `SlotOwner` itself — the nested levels' road (T11) and a case's.
	[[nodiscard]] rack::SlotOwner& slotOwner() noexcept { return slots_; }

	/// **THE SLOTS' SHARE OF A MAIN-THREAD VISIT, AFTER `reconcileSlotsIfMoved`** — `SlotOwner::service`, and
	/// the record published when it changed. Returns how many slots it anchored. Main thread.
	uint32_t serviceSlotRoads() noexcept;

	/// APPEND A DEVICE TO THE LANE, returning the slot it took or `kMaxFxSlots` on refusal. The `(+)`'s
	/// only road, and an EDIT.
	///
	/// TAKES A TAG, WHICH IS THE DEVICE'S OWN NAME FOR ITSELF. Not a descriptor pointer and not an
	/// index into kDevices: an index is a position in a table that may be reordered, and a pointer is
	/// an address, which device.h's own ABA rule says is never an identity. The tag is what the saved
	/// row stores, so the thing the editor names is the thing the patch will name.
	///
	/// THE DECISION IS NOT MADE HERE. addFxDevice in fx_chain.h refuses an unknown tag, a device that
	/// cannot render audio, and a full lane, all from the device's own declarations. This function
	/// adds exactly two things that free function cannot know about: the atomics the audio thread
	/// reads, and that a hand did it — which is what makes the patch modified.
	size_t addFxDevice(const char* tag) noexcept;

	/// ADD A NEW DEVICE BEFORE `anchor` — backlog CR. Returns the position it landed at, or kMaxFxSlots
	/// if refused. The anchor is the card's DEVICE, resolved to a position here rather than remembered by
	/// the menu. An anchor that has left the chain appends.
	size_t insertFxDeviceBefore(DeviceInstanceId anchor, const char* tag) noexcept;

	/// REMOVE THE DEVICE AT POSITION `slot`, CLOSING THE GAP BEHIND IT. False if there is nothing there.
	///
	/// **ITS ROUTES GO WITH IT, AND NOTHING ELSE MOVES** — the record removes the node and every edge
	/// naming it (`ChainRecord::remove`). This used to be a road that REQUIRED the dense routing table,
	/// because the table was addressed by slot and a removal shifted every later device; a route names a
	/// device's id now, so there is nothing to shift and nothing a caller could forget.
	///
	/// AN EDIT RATHER THAN A PATCH ARRIVING, so it marks the state dirty where restoreFxChain does not.
	bool removeFxDevice(size_t slot) noexcept;

	/// MOVE THE DEVICE AT `from` TO `to`. The order of an audio chain is its sound — and a reorder that
	/// makes a route run backwards deletes it, by the record's own rule.
	bool moveFxDevice(size_t from, size_t to) noexcept;

	/// PUT A DEVICE BACK AT `slot` WITH ITS ROUTES — what an undo of a remove calls. `routes` are what
	/// `routesOf` answered before the removal; the device comes back under the same id, so they name it
	/// again as they are. A route whose other end has gone meanwhile is refused by the record's gate.
	bool insertFxDevice(size_t slot, const FxDevice& device, std::span<const backplane::Route> routes) noexcept;

	//--------------------------------------------------------------------------------------------//
	// SOME OF THE FX LANE AT ONCE — backlog OA (`rack/fragment.h`): copy, paste, duplicate, remove, group, over the //
	// same record the lane's single-device verbs edit. The strip is not offered.                   //
	//--------------------------------------------------------------------------------------------//
	rack::ChainRecord& fragmentRecord() override { return chains_; }
	size_t fragmentChain() const override { return kLaneChain; }
	ContainerBodies& fragmentBodies() override { return fxBodies_; }
	backplane::InstanceCounter& fragmentIds() override { return ids_; }
	rack::CoreCodec fragmentCodec() const override { return feathersCoreCodec(); }
	rack::DeviceViews* fragmentViews() override { return &cardViews_; }
	rack::DeviceLabels* fragmentLabels() override { return &outputLabels_; }
	void fragmentCatchUp() override { harvestExtraState(); }
	void fragmentFreeSlots(DeviceInstanceId device) override {
		(void)slots_.pool().freeUnder(std::span<const DeviceInstanceId>(&device, 1));
	}
	void fragmentRestoreSlots(DeviceInstanceId device) override {
		(void)slots_.restoreUnderPath(std::span<const DeviceInstanceId>(&device, 1));
	}
	bool fragmentRebaseSlots(DeviceInstanceId device, DeviceInstanceId container, bool into) override {
		const std::array<DeviceInstanceId, 1> flat { device };
		const std::array<DeviceInstanceId, 2> deep { container, device };
		return into ? slots_.rebasePath(flat, deep) : slots_.rebasePath(deep, flat);
	}
	void fragmentChanged(const rack::DeviceLabels& labelsBefore) override;
	uint64_t fragmentOrigin() const override { return fragmentOrigin_; }
	std::weak_ptr<const void> fragmentLife() const override { return life_; }
	/// THE FX LANE RUNS THE VOICES (a Poly device in it runs per voice), so a Group into Rack here says what it moves to the bus.
	bool fragmentHostsVoices() const override { return true; }

	/// **THE MODULATOR STRIP AS A FRAGMENT SITE** (backlog PN) — the same record, bodies, ids, views, labels and
	/// origin as the fx lane above (this bridge's own answers, forwarded), but chain `kStripChain`. So the strip's
	/// selection runs the fx lane's verbs, written once, over the strip. Two differences, both the strip's nature: a
	/// copy keeps its routes out (`fragmentCopiesOutbound`, TO BE CONFIRMED), and nothing groups.
	class StripSite final : public rack::FragmentSite {
	public:
		explicit StripSite(ParamBridge& bridge) : b_(bridge) {}
		rack::ChainRecord& fragmentRecord() override { return b_.fragmentRecord(); }
		size_t fragmentChain() const override { return kStripChain; }
		ContainerBodies& fragmentBodies() override { return b_.fragmentBodies(); }
		backplane::InstanceCounter& fragmentIds() override { return b_.fragmentIds(); }
		rack::CoreCodec fragmentCodec() const override { return b_.fragmentCodec(); }
		rack::DeviceViews* fragmentViews() override { return b_.fragmentViews(); }
		rack::DeviceLabels* fragmentLabels() override { return b_.fragmentLabels(); }
		void fragmentCatchUp() override { b_.fragmentCatchUp(); }
		void fragmentFreeSlots(DeviceInstanceId device) override { b_.fragmentFreeSlots(device); }
		void fragmentRestoreSlots(DeviceInstanceId device) override { b_.fragmentRestoreSlots(device); }
		bool fragmentRebaseSlots(DeviceInstanceId device, DeviceInstanceId container, bool into) override {
			return b_.fragmentRebaseSlots(device, container, into);
		}
		void fragmentChanged(const rack::DeviceLabels& labelsBefore) override { b_.fragmentChanged(labelsBefore); }
		uint64_t fragmentOrigin() const override { return b_.fragmentOrigin(); }
		std::weak_ptr<const void> fragmentLife() const override { return b_.fragmentLife(); }
		bool fragmentHostsVoices() const override { return false; }
		bool fragmentCopiesOutbound() const override { return true; }
		bool fragmentGroups() const override { return false; }

	private:
		ParamBridge& b_;
	};
	[[nodiscard]] rack::FragmentSite& stripFragmentSite() noexcept { return stripSite_; }

	//---------------------------//
	// What is in the modulator rack //
	//---------------------------//

	/// WHAT THE RACK HOLDS AND WHAT EACH OF THEM IS SET TO — and unlike the fx lane above, the
	/// settings really are here.
	///
	/// A VIEW OF THE RECORD'S STRIP, MESSAGE THREAD ONLY — see `fxChain()`.
	ModRack modRack() const noexcept;

	/// One envelope's settings, from the editor. An EDIT — it marks the patch modified, the way every
	/// other setter on this side does.
	void setEnvelopeSettings(size_t slot, const mod::dahdsr::Settings& settings) noexcept;

	/// THE SAME ROAD FOR AN MSEG, and its existence beside the one above is the point of the rack
	/// holding devices rather than envelopes.
	///
	/// TWO SETTERS RATHER THAN ONE TAKING A DeviceRow, deliberately. A card should hand over the
	/// device's OWN settings type and have the tag and the field count stamped for it from that
	/// device's declarations — a caller assembling a row itself is a caller who can stamp the wrong
	/// tag onto the right numbers, and nothing downstream would notice until a patch reloaded as the
	/// other device. The row is an implementation of the channel, not its interface.
	void setMsegSettings(size_t slot, const mod::mseg::Settings& settings) noexcept;

	/// Store one rack slot's row, in the ordering the channel requires. Private-ish but declared here
	/// because restoreModRack and setEnvelopeSettings must not each spell that ordering out.
	void storeRackRow(size_t slot, const DeviceRow& row) noexcept;

	/// WHAT THE SLOT'S DEVICE IS CALLED — the serial, read out of the record.
	///
	/// EVERY SETTINGS WRITE ON THIS BRIDGE GOES THROUGH ONE OF THESE, and that is what they are for.
	/// A settings edit rebuilds a whole DeviceRow, so it has to carry the identity across; while
	/// these call sites read `fxInstance_[slot]` on their own, the ADDRESS was a field each of them had
	/// to remember to fetch beside the serial, and forgetting would have silently unrouted the device
	/// the moment a knob moved.
	///
	/// **THERE IS NO SECOND HALF ANY MORE (kill-lanes step 3c).** `DeviceIdentity` carried a `routeLane`
	/// beside the serial; step 3b stopped anything answering for it and 3c deleted it. These still return
	/// the struct rather than a bare id because the road they feed (`toDeviceRow` → `storeRowOf`) is the
	/// one that must not lose a device's name on the way, and that is what the type is for.
	DeviceIdentity fxIdentityAt(size_t slot) const noexcept;
	DeviceIdentity rackIdentityAt(size_t slot) const noexcept;

	/// **WHICH DEVICE STANDS AT A POSITION, AND WHERE A DEVICE STANDS** — the two crossings between the
	/// order a player sees and the id every route and control names (kill-lanes step 2). They replaced the
	/// lane lookups: a widget holds its device's id, and asks for a position only to draw or drop.
	///
	/// ON THE BRIDGE RATHER THAN ON THE CHAINS because a widget asking `bridge_.fxChain()` to answer one
	/// question rebuilds the whole flattened lane, and these are called from layout walks per card per
	/// frame. `kNoInstance` for a position holding nothing; kMaxFxDevices / kMaxRackEnvelopes for an id
	/// that chain does not hold — what every caller already tests against for "no such slot".
	DeviceInstanceId fxDeviceAt(size_t slot) const noexcept { return fxIdentityAt(slot).instanceId; }
	DeviceInstanceId rackDeviceAt(size_t slot) const noexcept { return rackIdentityAt(slot).instanceId; }
	size_t fxSlotOf(DeviceInstanceId device) const noexcept;
	size_t rackSlotOf(DeviceInstanceId device) const noexcept;

	/// **WHAT EACH DEVICE IS AND WHERE IT STANDS, BY ID** — the record's view, for the routing authority
	/// and for a surface that asks what a source or destination belongs to. Valid while this bridge is.
	[[nodiscard]] backplane::DeviceView devices() const noexcept { return chains_.devices(); }
	/// The device `id`, in either chain, or null.
	[[nodiscard]] const device::Descriptor* descriptorOf(DeviceInstanceId id) const noexcept;

	/// Replace the whole rack at once — what a patch load does, and NOT an edit.
	void restoreModRack(const ModRack& rack) noexcept;

	/// THE DEFAULT PATCH'S ONE ROUTE (backlog EF): the strip's envelope into Level at full amount. Called
	/// where the default rack is installed — a fresh instance, and a restore with no patch — never on a
	/// patch that carries its own routes.
	void routeDefaultEnvelope();

	/// APPEND A MODULATOR TO THE RACK, returning the slot it took or `kMaxRackSlots` on refusal. The
	/// rack's half of addFxDevice above, and everything that one says applies here — including which
	/// capability is asked for, which is the device's rather than the lane's.
	size_t addRackDevice(const char* tag) noexcept;

	/// The rack's "add before" — see `insertFxDeviceBefore`; the anchor is a DEVICE, the answer a
	/// position.
	size_t insertRackDeviceBefore(DeviceInstanceId anchor, const char* tag) noexcept;

	/// THE RACK'S OTHER THREE — the fx lane's roads for the strip, by the same rules: a removal takes the
	/// device's routes (as a SOURCE as well as a destination — a modulator is both), a reorder deletes
	/// what it makes backwards, and `insertRackDevice` is the inverse an undo needs.
	bool removeRackDevice(size_t slot) noexcept;
	bool moveRackDevice(size_t from, size_t to) noexcept;
	bool insertRackDevice(size_t slot, const DeviceRow& row, std::span<const backplane::Route> routes) noexcept;

	/// WHERE MODULATION HAS ACTUALLY LEFT EACH CONTROL, published once a block by the engine — the
	/// rack's counterpart to `modulated(clap_id)` for parameters, and read for the same purpose: a
	/// widget draws the moving part of its indicator from this while the printed number stays at the
	/// base.
	///
	/// The engine's own arithmetic rather than the editor's. See Synth::modulatedRack.
	/// **AND WHERE EACH OF THEM STANDS, ON THE SAME CALL RATHER THAN A SECOND ONE.**
	///
	/// A REQUIRED PARAMETER BEATS A REMEMBERED CONVENTION, which is this repo's rule for exactly this
	/// hazard — and the hazard is documented three lines from one of the call sites: *"sites that
	/// must agree is exactly how setModRack came to exist at one of them and not the other."* There
	/// are three places the engine hands the editor its snapshot. Adding a second function would make
	/// it three places to remember; adding a parameter makes it a compile error to forget.
	/// **AND THE FX LANE ON THE SAME CALL, for the reason this function already gives about the
	/// playheads: "A REQUIRED PARAMETER BEATS A REMEMBERED CONVENTION."** There are three places the
	/// engine hands the editor its snapshot. A separate `publishModulatedFxChain` would have been
	/// three places to remember; a parameter makes it a compile error to forget.
	///
	/// **RENAMED FROM `publishModulatedRack`**, which was narrower than what it does: it carries the
	/// fx lane too. The old name was kept for exactly one commit, deliberately, because renaming
	/// reaches comment references in two other sessions' files and that commit landed inside an active
	/// merge window — the deferral was written here rather than left for someone to notice.
	/// **AND THE CLOCK, WHICH IS WHY THIS TAKES ONE MORE ARGUMENT RATHER THAN GAINING A SIBLING.** The
	/// note above argues a parameter over a separate `publishModulatedFxChain` — "three places to
	/// remember" against "a compile error to forget" — and a tempo the editor needs once a block is
	/// the same kind of thing. Placed BEFORE the defaulted `drivePeaks` deliberately: given a default
	/// it could be forgotten at exactly the site that matters, which is the argument's own point.
	void publishModulated(const ModRack& rack, const FxChain& fx,
	                          const RackPlayheads& playheads, mod::TransportClock clock,
	                          const FxDrivePeaks& drivePeaks = noFxDrivePeaks(),
	                          const RackPlayheads& fxPlayheads = noFxPlayheads()) noexcept;

	/// WHERE MODULATION LEFT ONE OF THE FX LANE'S CONTROLS — `rackModulatedFieldAt`'s twin, and the
	/// seam reads it the same way.
	///
	/// **NO TAG BESIDE THIS ONE, deliberately, and the asymmetry with the rack is the point.** The
	/// rack channel needs `envModulatedTag_` because `modulatedEnvelope` DECODES a typed row out of
	/// it, and a type taken from one publish against fields from another is how an MSEG gets read
	/// through an envelope's layout. Nothing decodes this channel: the seam asks for one field by
	/// index and gets a float. A tag here would be storage nothing interrogates.
	float fxModulatedFieldAt(size_t slot, size_t control) const noexcept;
	mod::dahdsr::Settings modulatedEnvelope(size_t slot) const noexcept;

	/// **THE RIGHT EAR (backlog KU), PER POSITION AND PER SLOT.** `publish*RightEar` stores the values whose bit is
	/// set in `mask`, then the mask; `*ModulatedFieldRAt` answers true and the value only while that bit is set — so a
	/// mono control publishes nothing new and reads as mono. Audio thread writes, editor reads, both relaxed.
	void publishFxRightEar(size_t position, const device::ControlMask& mask,
	                       const std::array<float, kMaxDeviceFields>& values) noexcept;
	void publishRackRightEar(size_t slot, const device::ControlMask& mask,
	                         const std::array<float, kMaxDeviceFields>& values) noexcept;
	bool fxModulatedFieldRAt(size_t slot, size_t control, float& right) const noexcept;
	bool rackModulatedFieldRAt(size_t slot, size_t control, float& right) const noexcept;

	/// WHERE A RACK MODULATOR STANDS — the ordinal and the phase the card draws its playhead at.
	///
	/// **ONE ATOMIC, BECAUSE TWO WOULD BE ABLE TO STRADDLE.** The automod's phase is a single float
	/// and is therefore STRUCTURALLY INCAPABLE of handing back half of one reading beside half of
	/// another; its header says so. A pair of relaxed atomics gives that property away, and what it
	/// buys is the worst kind of bug this project knows — a marker jumping a whole segment for one
	/// frame, unreproducible, invisible to every test, and indistinguishable from the engine having
	/// done it. So the pair is PACKED: ordinal in the high word, the phase's bits in the low one.
	///
	/// **IT DOES NOT RAISE `repaintDirty_`**, for the reason `publishOwlttMeters` and
	/// `publishAutomodPhase` refuse it: this is written every block whether anything moved or not,
	/// and the flag's blast radius is the whole watched tree. The card that draws the marker watches
	/// this value itself.
	void publishRackPlayhead(size_t slot, mod::Playhead where) noexcept;
	[[nodiscard]] mod::Playhead rackPlayhead(size_t slot) const noexcept;

	/// **HOW LONG A BEAT IS, or zero when the host has not said** — see `DeviceHost::secondsPerBeat`,
	/// which is where the meaning of the zero is written down. Published by `publishModulated` from
	/// the clock the engine was handed, read by the editor when it paints.
	[[nodiscard]] double rackSecondsPerBeat() const noexcept {
		return rackSecondsPerBeat_.load(std::memory_order_relaxed);
	}

	//---------------------------------------------//
	// How hard an fx device is being driven       //
	//---------------------------------------------//

	/// **THE DRIVE METER'S CHANNEL — audio thread writes, UI thread reads.**
	///
	/// A PER-SLOT ATOMIC, exactly like `rackPlayhead_`, and for the same reason: the value crosses a
	/// thread boundary into a drawing. Published only for slots whose device offers
	/// `AudioOps::drivePeak`; the rest keep the sentinel and every surface then draws no meter rather
	/// than a meter reading silence.
	///
	/// **NEGATIVE IS THE SENTINEL, NOT ZERO.** Zero is a real level — a device being driven by nothing
	/// — and a channel that could not tell it from "nobody publishes here" would make an idle shaper
	/// and an unmetered one draw identically. `DeviceHost::drivePeakAt` returns false on the sentinel.
	void publishFxDrivePeak(size_t slot, float peak) noexcept;
	[[nodiscard]] bool fxDrivePeak(size_t slot, float& peak) const noexcept;
	/// **EACH FX POSITION'S COURTESY OUTPUT METER** (backlog MK) — the engine's held peak of what the device put out
	/// (`RackEngine::outputPeakAt`), linear and NOT clamped to 1, since the over is what the card's line draws from its
	/// other end. Negative is the sentinel, as for the drive peak; `fxOutputPeak` declines on it. Written with every
	/// engine-readings publish (`publishEngineReadings`), never raising `repaintDirty_`: the card's own meter frame
	/// asks for its frame.
	void publishFxOutputPeaks(const FxDrivePeaks& peaks) noexcept;
	void publishFxOutputPeak(size_t slot, float peak) noexcept;
	[[nodiscard]] bool fxOutputPeak(size_t slot, float& peak) const noexcept;

	/// **WHERE A MODULATOR IN THE FX LANE STANDS** — `rackPlayhead`'s twin, by lane position, packed the same
	/// way and for the same reason. The lane holds modulators since 2026-10-04; idle for any other position.
	void publishFxPlayhead(size_t position, mod::Playhead where) noexcept;

	/// **THE EXTRA STATE A DEVICE CHANGED ITSELF, ENGINE TO RECORD** (backlog MX, `device/extra_state.h`): the audio
	/// thread stores a strip slot's or a lane position's snapshot here, and `harvestExtraState` — the main thread, at
	/// every `refresh` and before every save — writes it into the record under the basis guard. Not an edit: nothing is
	/// marked dirty and no undo step is made.
	void publishRackExtraState(size_t slot, DeviceInstanceId device, uint64_t serial, const float* values, size_t count) noexcept;
	void publishFxExtraState(size_t position, DeviceInstanceId device, uint64_t serial, const float* values,
	                         size_t count) noexcept;
	void harvestExtraState() noexcept;
	[[nodiscard]] mod::Playhead fxPlayhead(size_t position) const noexcept;

	/// **THE MASTER METER'S READINGS** (backlog DN), published by the plugin beside the drive peaks on
	/// every process, read by the tray's meter on the editor's tick. Three floats, each its own atomic:
	/// a frame that reads a loudness from one block and a clip age from the next draws nothing wrong,
	/// since each is a fact on its own. Non-finite is dropped before it crosses, for the peaks' reason.
	void publishMasterMeter(const MasterMeterReadings& readings) noexcept;
	[[nodiscard]] MasterMeterReadings masterMeter() const noexcept;
	/// **THE KIT PADS' HITS** (pad_hits.h), published beside the master meter on every process and read by the kit panel's
	/// pads and the sound cards on the editor's tick, which turn a count that moved into a pulse.
	void publishPadHits(const PadHits& hits) noexcept;
	[[nodiscard]] PadHits padHits() const noexcept;
	/// **THE KIT PADS' FX LANES** (drum proposal §17 P3): the Synth's, through a function the plugin installs once (the bridge
	/// owns no synth). `make` builds the pad's lane when it has none (MESSAGE THREAD: it allocates). Null without a source, or
	/// for a pad with no lane and `make` false. The pointer is the Synth's for its whole life (never freed), so it is not an
	/// ABA handle; a panel still re-asks every tick, the house habit.
	using PadLaneSource = std::function<rack::InnerChain*(size_t pad, bool make)>;
	void setPadLaneSource(PadLaneSource source) { padLaneSource_ = std::move(source); }
	[[nodiscard]] rack::InnerChain* drumPadLane(size_t pad, bool make = false) const {
		return padLaneSource_ ? padLaneSource_(pad, make) : nullptr;
	}

	//---------------------------------------------//
	// Which sound is being listened to on its own //
	//---------------------------------------------//

	/// PREVIEW — see preview.h for what it is and what it is not.
	///
	/// ON THIS CHANNEL RATHER THAN THE PARAMETER QUEUE, and the difference is the whole point: nothing
	/// here reaches a parameter, a gesture, the host or a patch. One relaxed atomic, written by the
	/// editor and read by the audio thread once a block, exactly like the travel and plane channels
	/// beside it — but unlike those, it does NOT mark the state dirty, because listening to a sound is
	/// not a change to the project.
	///
	/// `clearPreview()` is the same write and exists to be called by name from the two places that must
	/// not forget: the editor closing, and the plugin deactivating. A latched preview with no editor on
	/// screen is a plugin playing one sound for a reason nobody can see.
	void setPreview(size_t slot) noexcept;
	void clearPreview() noexcept;
	Preview preview() const noexcept;

	/// Whether non-parameter state has changed since this was last asked, and clears the flag.
	///
	/// The plugin drains it on the main thread and tells the host. Kept here rather than in the plugin
	/// because the editor is what moves this state and the bridge is the only road it has.
	bool takeStateDirty() noexcept;

	/// WHETHER ANYTHING THE EDITOR DRAWS HAS MOVED SINCE THIS WAS LAST ASKED, and clears the flag.
	///
	/// A SECOND FLAG RATHER THAN A SECOND READER OF THE FIRST, and the two genuinely differ. `state
	/// dirty` means "the host should come and collect our state", so a LOAD deliberately does not
	/// raise it — the host just gave us that state and does not need telling about it. Repaint is the
	/// opposite case: a load is precisely when the screen is most wrong, because every channel moved
	/// at once and nothing on screen knows. So the restores raise this and not that.
	///
	/// PAYLOAD-FREE AND COALESCED. It says THAT something moved, never what: a drag posts tens of
	/// values a second and every widget that reads any of these channels already re-derives what it
	/// draws from scratch, so a per-channel signal would buy nothing and cost a subscription matrix.
	/// One bit, drained once a frame.
	///
	/// The engine→editor publishes are deliberately NOT here — see publishPuckVelocity.
	bool takeRepaintDirty() noexcept;

	/// WHAT THE LAST LOAD REFUSED, LATCHED UNTIL SOMEBODY READS IT, and cleared by the reading.
	///
	/// A LATCH RATHER THAN A CALL, because a patch can be loaded with NO EDITOR OPEN — a host restoring
	/// a project, or a preset chosen from its own browser — and a notification fired at load time in
	/// that case is a notification nobody could ever have seen. Held here until an editor exists and
	/// drains it on a frame, which is the same road takeRepaintDirty travels and for the same reason.
	///
	/// **NOT AN ATOMIC ANY MORE, AND THAT IS A DELIBERATE POSTURE CHANGE (kill-lanes step 3b).** This was
	/// one `std::atomic<uint64_t>` PACKED as count << 32 | source << 16 | destination, "so the take is a
	/// single exchange: a second load cannot interleave a count from one patch with a source index from
	/// another". A refusal names ENDPOINTS now, whose device half is a 32-bit instance id off a counter
	/// that only goes up — truncating one to sixteen bits would report a route as belonging to a
	/// DIFFERENT device, quietly, so the packing cannot survive the new type.
	///
	/// AND THE ATOMIC IT WOULD HAVE BECOME DOES NOT EXIST HERE. MEASURED, g++-14 on this host:
	/// `std::atomic<RouteRefusals>` is 24 bytes and links to `__atomic_store` / `__atomic_exchange`,
	/// undefined without `-latomic` — with or without `-mcx16`. Buying it would mean a new runtime
	/// dependency, which the static-link posture forbids.
	///
	/// WHAT MAKES A PLAIN MEMBER CORRECT is that there was never a second thread: `noteRefusedRoutes` is
	/// reached only from `state.load`, which CLAP declares `[main-thread]` (see `feathers_clap.cpp`'s
	/// chains block, which argues that thread rule at length), and `takeLoadReport` only from the
	/// editor's frame, also the main thread. Two loads racing one take was never possible, so the
	/// exchange was sequencing something one thread already sequenced.
	///
	/// **A LOAD REPORT SINCE BACKLOG JF** — the refused routes are one kind of what a load can say, and the
	/// report rides the same latch (`rack::LoadReport`). `noteLoadReport` REPLACES what is held (it
	/// describes the patch now in front of the user), except that an EMPTY report clears nothing, for the reason
	/// `noteRefusedRoutes` always gave; `noteRefusedRoutes` ADDS a service's drops to it.
	void noteRefusedRoutes(const RouteRefusals& refused);
	void noteLoadReport(const rack::LoadReport& report);
	rack::LoadReport takeLoadReport();

	/// THE SMOOTHED PUCK VELOCITY, and this one travels the OTHER way — the engine measures it and the
	/// editor reads it. It is not a handle: nobody sets it, it is what the puck is doing.
	///
	/// IT DOES NOT RAISE THE REPAINT FLAG, deliberately. This is written from the AUDIO THREAD once
	/// per block, so flagging it would mark the editor dirty at block rate and turn a coalesced
	/// once-a-frame signal into an unconditional redraw of everything, forever, whether or not the
	/// puck is moving. The surfaces that care about velocity already run per-frame updates of their
	/// own; a channel the editor never writes is not what this flag is for.
	///
	/// Published so the scope can draw the sound being made rather than the one that would be made
	/// standing still. Two relaxed stores a block, and a reader that catches them mid-write sees one
	/// axis of a gesture beside the other axis of the same gesture a block earlier, which is a frame of
	/// the gesture.
	void publishPuckVelocity(AnchorPoint velocity) noexcept;
	AnchorPoint puckVelocity() const noexcept;

	/// WHAT MODULATION IS ADDING TO EACH SOUND'S PLACE (`TravelPulls`, sound_travel.h), on the same road
	/// as the velocity above and for the same reason: measured by the engine, not set by anyone. The
	/// Explore chart adds it to the authored place through `pulledPlace` — the engine's own function —
	/// so the star sits where the sound is played. **IT DOES NOT RAISE `repaintDirty_`**: it is written
	/// every block whether anything moved or not; the chart compares it once a frame and repaints only
	/// when it changed, which is what keeps Explore quiet at rest. A reader mid-write sees one sound's
	/// radius from this block beside its angle from the last, which is a frame of the orbit.
	/// NEVER PUBLISHED reads as all zero — the authored place — so a bridge with no engine draws true.
	void publishTravelPulls(const TravelPulls& pulls) noexcept;
	TravelPulls travelPulls() const noexcept;

	/// OWLTT MULTIBAND COMPRESSOR METERS — per-band gain reduction and input level, published by the
	/// DSP and read by the OwlttCard. Same road as puckVelocity: relaxed stores a block, reader that
	/// straddles a write sees one band's reading beside another's from a frame earlier, which during
	/// audio is a frame of the signal. Keyed by slot so two OWLTTs cannot collide. The bridge owns the
	/// atomics, so they outlive every editor open/close cycle — no use-after-free when the window closes
	/// with sound playing.
	///
	/// @param slot  Slot index (0..kMaxFxSlots-1)
	/// @param grDB  Per-band gain reduction, normalized to ±1.0 (negative = attenuation, positive = boost), 3 bands
	/// @param inputLevelDB  Per-band input level, normalized to 0..1 (dBFS-linear scale), 3 bands
	/// @param outputLevelDB  Per-band output level, normalized to 0..1 (dBFS-linear scale), 3 bands
	/// @param clipping  True if output is clipping (with hold timer)
	/// @param bandSaturating  A light per band while its clipper is working (backlog EL); defaulted so
	///                        a caller with only the original five still compiles — and publishes dark.
	/// @param combinedGrDb  The mean |gain change| across the bands, in dB (backlog EL).
	/// @param secondChannelGr  Each band's SECOND channel's gain reduction (backlog LQ), scaled as `grDB` is. Null
	///                        is a publisher with one linked gain, so the second channel reads `grDB`.
	void publishOwlttMeters(size_t slot, const std::array<float, 3>& grDB,
	                       const std::array<float, 3>& inputLevelDB,
	                       const std::array<float, 3>& outputLevelDB, bool clipping,
	                       const std::array<bool, 3>& bandSaturating = { false, false, false },
	                       float combinedGrDb = 0.0f,
	                       const std::array<float, 3>* secondChannelGr = nullptr, float noteHz = 0.0f) noexcept;
	struct OwlttMeterReading {
		std::array<float, 3> grDB {0.0f, 0.0f, 0.0f};
		std::array<float, 3> inputLevelDB {0.0f, 0.0f, 0.0f};
		std::array<float, 3> outputLevelDB {0.0f, 0.0f, 0.0f};
		bool clipping {false};
		std::array<bool, 3> bandSaturating { false, false, false };
		float combinedGrDb = 0.0f;
		/// Each band's SECOND channel's gain reduction (backlog LQ); `grDB` is the first's.
		std::array<float, 3> grSecond {0.0f, 0.0f, 0.0f};
		/// KEY TRACK's note, Hz, 0 while not tracking (reading 17).
		float noteHz = 0.0f;
	};
	[[nodiscard]] OwlttMeterReading owlttMeters(size_t slot) const noexcept;

	/// WHERE THE AUTOMODULATOR'S LFO IS, 0..1 — published by the DSP once per tick and read by the
	/// card's strip, which draws a marker on the waveform at it.
	///
	/// **ONE RELAXED FLOAT, AND THAT IS THE WHOLE CONTRACT.** Staleness is fine — a marker a frame
	/// behind is a marker a millisecond behind — and tearing is not, which is why it is an atomic
	/// rather than a plain float. A single `float` cannot straddle a write the way `OwlttMeterReading`
	/// can, so unlike the meters there is no multi-field reading to be coherent about.
	///
	/// **IT DOES NOT RAISE `repaintDirty_`**, and this is the most important line here.
	/// `publishOwlttMeters` refuses the same flag for the measured reason recorded beside it: the flag
	/// is coalesced once a frame but its BLAST RADIUS is not, so one publish told 108 frames of which
	/// 85 draw nothing of the sort, and taking a block-rate publisher off the whole-tree channel
	/// measured at -53%. The frame is asked for by the widget that draws the reading — `AutomodStrip`
	/// watches this value itself, exactly as `OwlttGraph::update()` watches the meters.
	///
	/// Keyed by slot, so two automodulators cannot collide. The bridge owns the atomics, so they
	/// outlive every editor open/close cycle.
	/// BOTH OF AUTOMOD'S READINGS AT ONCE — the LFO phase and the envelope follower's level.
	///
	/// ONE CALL RATHER THAN TWO, because they are published together from one callback on the audio
	/// thread and a surface that read them from separate publishes could draw a phase from this block
	/// beside an envelope from the last.
	void publishAutomodReadings(size_t slot, float phase, float envelope) noexcept;
	[[nodiscard]] float automodPhase(size_t slot) const noexcept;
	/// **HOW MANY POSITIONS THE ENGINE->EDITOR READINGS REACH** — the fx lane's and the strip's. A publish
	/// past it is refused and a read past it answers the empty reading; the pool grows with the chain
	/// (unbounded devices step 2), so this is a capacity and not a cap.
	[[nodiscard]] size_t fxTelemetryCapacity() const noexcept { return fxTelemetry_.capacity(); }
	[[nodiscard]] size_t rackTelemetryCapacity() const noexcept { return rackTelemetry_.capacity(); }
	/// THE ENVELOPE FOLLOWER'S LEVEL, post-trim and bounded to 0..1 — what the steering actually uses.
	[[nodiscard]] float automodEnvelope(size_t slot) const noexcept;

	/// **A GENERIC PER-SLOT READINGS CHANNEL — ANONYMOUS FLOATS, THE WAY `DeviceHost::readingAt`
	/// ALREADY CARRIES THEM.**
	///
	/// The two channels above are DEVICE-NAMED: `automodPhase`, `owlttMeters`. That works while a
	/// device publishes two or three numbers somebody can name in a getter, and Bus Utility's meter
	/// publishes seven — `Reading::kCount` — which would be seven getters on this class describing
	/// one device's card.
	///
	/// **SO THIS IS THE SHAPE THE RACK ALREADY USES**: `RackEngine::publishReadings(slot, values,
	/// count)`, indexed, unnamed, with the device's own UI knowing what the indices mean.
	/// `device_host.h` argues that case at length — *"the host carries numbers it does not understand
	/// and the DEVICE'S OWN UI knows what the indices mean"* — and Feathers simply had no such road
	/// until now. owltt and automod are NOT migrated onto it here: that is a separate change to two
	/// working devices, and doing it as a side effect of adding a third is how a refactor arrives
	/// unreviewed. Backlog DO.
	///
	/// AUDIO-THREAD-WRITTEN, UI-READ, relaxed — the same terms as every other reading on this bridge.
	/// A torn float is one frame of a meter.
	void publishDeviceReadings(size_t slot, const float* values, size_t count) noexcept;
	/// HOW MANY THIS SLOT HAS EVER PUBLISHED — zero for a device that publishes none, and zero for a
	/// slot nothing has run in yet. Both draw as no meter, which is right: `device_host.h` says a
	/// caller cannot tell those apart and should not need to.
	[[nodiscard]] size_t deviceReadingCount(size_t slot) const noexcept;
	[[nodiscard]] float deviceReading(size_t slot, size_t index) const noexcept;

	/// HOW MUCH OF EACH SOUND IS BEING HEARD — the activation weights, travelling the same way the
	/// puck velocity does and for the same reason: the engine computes them, nobody sets them, and a
	/// surface that wants to draw them must not go reading the character off the audio thread to find
	/// out.
	///
	/// Nine relaxed stores a block. A reader that straddles them sees one sound's share beside
	/// another's from a block earlier — which is a frame of the same gesture, since the only thing that
	/// moves them is the puck moving. The mask is stored LAST on the write and read FIRST on the read,
	/// so a straddling reader can see a share whose slot has just gone dark but never a lit slot with
	/// nothing behind it: absent is the state that must never be invented.
	///
	/// DERIVED, NEVER SAVED — see sound_weights.h. This channel has no gesture bracket and never marks
	/// the state dirty, because nothing here is a change to the patch: it is a reading OF one.
	void publishSoundWeights(const SoundWeights& weights) noexcept;
	SoundWeights soundWeights() const noexcept;

	/// THE MODULATION ROUTINGS — how far each macro reaches into each destination.
	///
	/// The same road the drift offsets and the gamma bases take, and for the same two reasons: there is
	/// no parameter underneath an amount for a host gesture to be about, so the bracket's job is to mark
	/// the project dirty; and there are twenty-four of them, edited by dragging a knob rather than by an
	/// automation lane, which is why these are not CLAP params.
	///
	/// An amount is in units of the destination's own range — see ModRoutes in params.h.
	///
	/// **BY ENDPOINT (kill-lanes step 2)**: a host end is Feathers' own macro or parameter index, a device
	/// end the device's id and its own output or control index. Refused, with nothing changed, for a pair
	/// `mayRoute` refuses or an amount that is not finite; zero removes.
	void beginRouteGesture() noexcept;
	void setRouteAmount(backplane::Source source, backplane::Destination destination, float amount) noexcept;
	void endRouteGesture() noexcept;

	/// WHETHER `source` MAY REACH `destination` — the record's authority (the routing rule: a live source
	/// upstream of a live modulatable destination, and Feathers' own weight-to-puck rule), the one gate a
	/// gesture's glow and its write both ask.
	[[nodiscard]] bool mayRoute(backplane::Source source, backplane::Destination destination) const noexcept {
		return chains_.mayRoute(source, destination) && mayReachEditOnly(source, destination);
	}
	/// **A SOUND'S IDENTITY IS REACHED BY A RELATIVE MACRO OR NOT AT ALL (backlog PQ)** — true for every destination
	/// that is not edit-only (`isEditOnlyDestination`), and for an edit-only one only from a header macro whose type is
	/// Relative: its route is an edit of the base, never a modulation of it (A32).
	[[nodiscard]] bool mayReachEditOnly(backplane::Source source, backplane::Destination destination) const noexcept {
		if (destination.isDevice() || !isEditOnlyDestination(destination.index)) {
			return true;
		}
		return !source.isDevice() && source.index < kMacroCount
		       && macroModes_.type[source.index] == device::MacroType::Relative;
	}

	/// HOW FAR ONE SOURCE REACHES ONE DESTINATION, or 0 — one read of the record's graph.
	float routeAmount(backplane::Source source, backplane::Destination destination) const noexcept;

	/// **THE AS-MODULATED READOUT'S CHANNEL** (backlog PU; `backplane/route_probe.h`): the editor names a pair, the
	/// plugin's block answers it from the lane engine (`publishEngineReadings`).
	[[nodiscard]] backplane::RouteProbeChannel& routeProbe() const noexcept { return routeProbe_; }

	/// **THE ROUTE'S CURVE** — Lin for no route. The write is
	/// refused for a pair with no route, and for a source whose consumer is linear by construction (the
	/// stack index: the synth gives part k `amount × index[k]` itself, `ParamSet::stackIndexAmount`).
	backplane::Transfer routeTransfer(backplane::Source source, backplane::Destination destination) const noexcept {
		return chains_.routeTransfer(source, destination);
	}
	bool setRouteTransfer(backplane::Source source, backplane::Destination destination, backplane::Transfer t) noexcept;
	/// Whether `t` may be chosen for `source` at all — the stack index is Lin only, whatever its polarity.
	[[nodiscard]] bool transferAllowedFor(backplane::Source source, backplane::Transfer t) const noexcept;

	/// Whether any source OTHER than `except` reaches `destination` — the "routed elsewhere" mark a
	/// control draws beside the focused source's ring.
	bool routedElsewhere(backplane::Destination destination, backplane::Source except) const noexcept;

	/// EVERY ROUTE: `f(const backplane::Route&)`, by identity, as published.
	template <typename F>
	void forEachRoute(F&& f) const {
		routeList_.forEach(std::forward<F>(f));
	}

	/// **A LOAD'S ROUTES WITHOUT A PATCH — TESTS ONLY.** Every route replaced through the record's gate the
	/// way `restoreChains` lands them: NOT an edit. A case that needs more routes than any patch this
	/// bridge could have saved has no other road to them.
	void loadRoutesForTest(std::span<const backplane::Route> routes) noexcept;

	/// **EVERY ROUTE NAMING `device`, AT EITHER END** — what an undo of a removal keeps, and hands back to
	/// `insertFxDevice` / `insertRackDevice`. It replaced a dense per-lane slice of the 111 KB table.
	[[nodiscard]] std::vector<backplane::Route> routesOf(DeviceInstanceId device) const;

	//--------------------------------------------------------------------------------------------//
	// THE ROUTES INSIDE A CHAIN, FROM ONE OF THIS HOST'S SOURCES — rack roadmap §7, hygiene's other   //
	// half: the Engine rail's route list shows them beside this host's own, and edits them from here. //
	//--------------------------------------------------------------------------------------------//

	/// THE HOST CONTRACT'S OWN ROW (`DeviceHost::InnerRoute`), so the adapter hands these through unchanged.
	using InnerRoute = backplane::DeviceHost::InnerRoute;
	/// Every route inside any chain in the lane whose source is `source` (spelled this host's way).
	[[nodiscard]] std::vector<InnerRoute> innerRoutesFrom(backplane::Source source) const;
	[[nodiscard]] float innerRouteAmount(backplane::Source source, DeviceInstanceId chain,
	                                     backplane::Destination inner) const;
	/// Through the chain's own gate; returns whether it was taken.
	bool setInnerRouteAmount(backplane::Source source, DeviceInstanceId chain, backplane::Destination inner,
	                         float amount);
	/// The inner route's curve, and its write through the chain's own gate.
	[[nodiscard]] backplane::Transfer innerRouteTransfer(backplane::Source source, DeviceInstanceId chain,
	                                                     backplane::Destination inner) const;
	bool setInnerRouteTransfer(backplane::Source source, DeviceInstanceId chain, backplane::Destination inner,
	                           backplane::Transfer t);
	/// "Chain 2 · Filter · Cutoff" — the chain by its place in the lane, the rest as the chain spells it.
	[[nodiscard]] std::string innerDestinationName(DeviceInstanceId chain, backplane::Destination inner) const;
	/// WHAT TO CALL ONE OF THIS HOST'S OWN DESTINATIONS ON SCREEN — a parameter's own name, or a device's
	/// name for one of its controls ("Env Attack"), numbered by where the device stands once there is more
	/// than one to tell apart. One vocabulary: the route list, the tray and the Engine rail all read this.
	[[nodiscard]] std::string destinationName(backplane::Destination destination) const;

	/// **THE ROUTES AS THEY ARE PUBLISHED — BY IDENTITY, the one store.** The editor's thread writes it
	/// (every road below republishes it whole); the audio thread snapshots it and compiles it into its own
	/// coordinates with `ParamSet::adoptRoutes` (kill-lanes step 1), so no lane crosses the boundary.
	///
	/// **EDGES, NOT A MATRIX, AND ONE STORE, NOT TWO.** This was 27,840 relaxed floats, of which a real
	/// patch uses about ten. The writer keeps no second list beside it: a mutation edits the record and
	/// publishes again.
	const backplane::RouteList& routeList() const noexcept { return routeList_; }

	/// **TOLD, ON THIS (THE MESSAGE) THREAD, HOW MANY ROUTES THE ENGINE'S STORES MUST HOLD — BEFORE THE PUBLISH THAT
	/// NEEDS IT** (backlog OJ). `routeList_` grows without bound; the engine's stores (`ParamSet::reserveRoutes`,
	/// `Synth::reserveRoutes`) are grown by this listener, which `wireRouteGrowth` installs. Told at once of the
	/// current capacity. One listener; a second replaces the first.
	void setRouteGrowthListener(std::function<void(size_t capacity)> listener);

	/// **HOW MANY TIMES THE ROUTINGS HAVE CHANGED — the number that lets the engine skip a publish.**
	///
	/// `publishEditorLanes` used to call `params.setRoutes(bridge.routes())` on EVERY audio block, and
	/// both halves of that are dense: `routes()` is 27,840 atomic loads returning a 111 KB value, and
	/// `setRoutes` then walks it. §14.3 measured the pair at 0.37 % of a core, on a table that changes
	/// only when a hand moves a ring.
	///
	/// **AND MEASURING THE EDGE STORE IS WHAT FORCED THIS RATHER THAN A PREFERENCE FOR IT.** The edge
	/// list made the RESOLVE walk ten edges instead of sixty sources, and made the PUBLISH slower —
	/// 16.55 µs against the dense loop's 9.85 µs, measured A/B in one process — because a branchy scan
	/// of 27,840 floats does not vectorise where 27,840 stores do. The saving was never going to come
	/// from publishing a sparse thing faster; it comes from not publishing at all.
	///
	/// This is the design's pointer swap — swap the routes on the audio thread when the running one
	/// is marked dirty — in the form this side of the boundary can take: a counter
	/// the engine compares against the last one it applied.
	///
	/// **IT IS THE PUBLISH'S OWN SEQUENCE NOW** (`RouteList::sequence`), not a second counter bumped
	/// beside the writes — so it cannot say "changed" without the change being there to copy, and it is
	/// odd exactly while a publish is being written. Never 0 once constructed: the constructor publishes
	/// the empty table, so an engine whose last-applied generation is 0 always copies once before it
	/// starts skipping (an engine that skipped its FIRST publish would run on its own table while the
	/// editor drew this one).
	uint32_t routesGeneration() const noexcept { return routeList_.sequence(); }

private:
	PadLaneSource padLaneSource_; ///< drumPadLane's: the plugin's Synth, or empty (a test's bridge)
	/// `restoreChains` without the host slots: the record, bodies, routes, views and labels.
	rack::LoadReport restoreChainRecord(const rack::RackPatch& patch);

	/// THE STRUCTURAL VERBS, ONCE, FOR EITHER CHAIN — `removeFxDevice` and its siblings are these with
	/// the chain named.
	bool removeDevice(size_t chain, size_t slot) noexcept;
	bool moveDevice(size_t chain, size_t from, size_t to) noexcept;
	bool insertDevice(size_t chain, size_t slot, const DeviceRow& row,
	                  std::span<const backplane::Route> routes) noexcept;

	UiParamQueue queue_;

	std::atomic<bool> orientationPinned_ { false };
	std::atomic<float> orientationRotate_ { 0.0f };
	std::atomic<double> falloffPower_ { kFalloffPowerA };
	std::atomic<double> blueOvershootScale_ { kBlueShiftScaleOn };
	std::atomic<double> blueDopplerScale_ { kBlueShiftScaleOn };
	std::atomic<bool> v1Crossfade_ { false };
	/// Two per slot. Relaxed like the rest of this channel: a reader that straddles a write sees one
	/// axis of a drag beside the other axis a block earlier, which is a frame of the drag.
	std::array<std::atomic<double>, kMaxSounds * 2> soundTravel_ {};
	/// `[slot * 2]` radius, `[slot * 2 + 1]` theta — `SoundTravel::orbit`, beside the offsets it is added to.
	std::array<std::atomic<double>, kMaxSounds * 2> soundOrbit_ {};
	/// One relaxed double per slot. Unbounded by design, so nothing here clamps it.
	std::array<std::atomic<double>, kMaxSounds> soundGamma_ {};
	/// Per slot: pinned, and the one turn the decode reads. Relaxed like the rest of this channel — a
	/// reader that straddles a write sees a turn from a frame earlier, which during a sweep is a frame
	/// of the sweep.
	std::array<std::atomic<bool>, kMaxSounds> soundPinned_ {};
	std::array<std::atomic<float>, kMaxSounds> soundPlane_ {};
	/// Both engines' banked residue per slot, doubles because the value IS a double difference — see
	/// SliceResidue for why floats would break the guarantee this exists to give. Relaxed like the rest
	/// of this channel; a reader that straddles a write sees one coordinate of a turn beside another
	/// from a frame earlier, which during a sweep is a frame of the sweep.
	std::array<std::atomic<double>, kMaxSounds * kPlaneResidueValues> soundResidue_ {};
	/// The stances each sound remembers. Main thread only — see the note on the setters.
	SoundVariations variations_ {};
	/// Which colour each sound wears. Main thread only, for the reason the variations beside it are.
	SoundHues hues_ {};
	MacroModes macroModes_ {};
	/// THE TERRAIN'S LIKES (backlog ON): patch state beside the parameters, main thread only, read by nothing on the audio thread.
	TerrainLikes terrainLikes_ {};
	PresetIdentity presetIdentity_ {};
	uint64_t presetIdentitySerial_ = 0;
	/// The likes' warps' triple buffer (`publishLikeWarp`): the main thread's own copy, three sets with the warps their
	/// entries point at (on the heap, about 1.6 MB a set), the mailbox's index with a fresh bit (4), and the audio thread's
	/// front index.
	LikeWarpSet likeMain_ {};
	std::array<LikeWarpSet, 3> likeSets_ {};
	std::unique_ptr<std::array<std::array<PhaseWarp, kMaxSounds>, 3>> likeStores_ =
	    std::make_unique<std::array<std::array<PhaseWarp, kMaxSounds>, 3>>();
	std::atomic<int> likeMailbox_ { 1 };
	int likeBack_ = 2;
	int likeFront_ = 0;
	/// THE SPECTRAL DRAWER'S STORED STATE (`spectralState`): the editor writes, process() reads, a field per atomic.
	std::atomic<bool> spectralOn_ { true };
	/// OE: THE ENGINE VOCODER'S HELD ENVELOPE AS THE LAST LOAD CARRIED IT, and how many loads there have been (SpectralState::
	/// vocodeFreeze, vocodeFreezeLoads): written by restoreSpectralState on the main thread, the words before the count, so a reader
	/// that sees a count sees at least its words.
	device::BlockSnapshot<kVocodeFreezeFloats> vocodeFreeze_;
	std::atomic<uint32_t> vocodeFreezeLoads_ { 0 };
	std::array<std::atomic<bool>, kMaxSounds> spectralCurveOn_ {};
	std::array<std::array<std::atomic<float>, spectral::kCurveBands>, kMaxSounds> spectralCurve_ {};
	/// THE SPECTRUM DRAWER'S PLOT (`publishPlot` / `readPlot`): the frame as flat floats, each its own atomic, between two
	/// serials (A written first, B last) — the house's readings-store pattern (eq_types.h), made a whole frame or none.
	std::atomic<bool> plotWanted_ { false };
	/// The drawer reopened: the next publish goes out even if it equals the last (`setPlotWanted`).
	std::atomic<bool> plotRepublish_ { false };
	/// A drawer edit since the last plot (`hasPendingWork`).
	std::atomic<bool> spectralMoved_ { false };
	std::atomic<uint32_t> plotSerialA_ { 0 };
	std::atomic<uint32_t> plotSerialB_ { 0 };
	std::array<std::atomic<float>, kSpectralPlotFloats> plot_ {};
	/// The publishing thread's own: the last frame published (what "unchanged" compares against) and its serial.
	SpectralPlotFrame plotLast_ {};
	uint32_t plotWriterSerial_ { 0 };
	/// Told when a macro's effective name moves (`subscribeMacroRename`). Main thread only.
	device::Subscribers<size_t> macroRenameSubscribers_;
	/// Names are string literals; `bipolar` follows `macroModes_` (see `setMacroBipolar`). The names here
	/// are the PARAMETER TABLE'S and stay so: prose asks `macroName`, which knows a rename, and nothing
	/// reads these for prose — a `const char*` into a string a rename can reallocate would dangle.
	std::array<device::SourceDesc, kCoreSourceCount> coreSourceDescs_ {};

	/// **ONE SEQUENCE FOR BOTH CHAINS (vision D1)** — the one counter every id either chain holds was
	/// drawn from. It was two once (`fxNextInstance_`, `envNextInstance_`), both minting device 1, and
	/// `backplane::Graph` keys its nodes on the id: two nodes under one id is two devices at one address
	/// (`tests/test_feathers_graph_order.cpp` found `Graph::restore` refusing the second). The blocks' own
	/// `nextInstanceId` fields are TRANSPORT: `fxChain()` and `modRack()` report this, a restore takes the
	/// MAXIMUM of what it is handed and what is here. Declared before the record, which holds a reference.
	backplane::InstanceCounter ids_;

	/// **THE RECORD — WHAT FEATHERS' CHAINS ARE** (chain-as-a-
	/// device 5b, C4). Two chains in rank order — the strip, then the fx lane (`feathers_chains.h`) — every
	/// device's identity, lane and row in one place. It replaced ~40 arrays of relaxed atomics that the
	/// audio thread read every block (`fxTag_`, `fxFields_`, `envSettings_`, ...), whose ordering rules
	/// (tag last, count after the rows) were defended "by comments on both sides and by being copied",
	/// because a single-threaded suite cannot straddle a write. The record is the MESSAGE THREAD's alone,
	/// and the audio thread takes what it publishes whole (`publishedChains()`), so there is no ordering
	/// to defend. `FxChain` and `ModRack` are views of it now.
	rack::ChainRecord chains_ { feathersAxes(), kFeathersChainCapacities, hostCatalogue(), ids_,
		                        &detail::maySourceRouteToIgnoringDevice };
	/// THE HOST SLOTS (`slotPool()`, `slotBus()`, ...) over the record above, which it names and so must
	/// follow. Empty until something maps a control: defaults are T8's.
	rack::SlotOwner slots_ { chains_ };
	/// WHAT THE AUDIO THREAD TAKES — the record's state, published after every change. See `publishedChains`.
	rack::PublishedChains publishedChains_ { kMaxDeviceSlots, 2 };
	/// See `landHeldControls`. Message thread.
	rack::HeldFalls heldFalls_;
	uint32_t chainsAdopted_ = 0;
	bool heldFallsLive_ = false;
	/// After every change to `chains_`: publish it, and say the editor has something to repaint.
	void publishChains() noexcept;
	/// REPLACE ONE CHAIN OF THE RECORD with these rows, IN THIS ORDER — a restore's and every structural
	/// edit's road. A row whose id is missing or already held by the other chain is given a fresh one; a
	/// row past the chain's width is not placed. Where each device is parked is the record's (kill-lanes
	/// step 3b), so the order is the whole of what crosses.
	void rebuildChain(size_t chain, std::span<const DeviceRow* const> rows) noexcept;
	/// ONE ROW INTO THE DEVICE `device` — its fields, and its tag if that changed (a replacement in the
	/// same place under the same id: `setFilterSettings`, `setEnvelopeSettings`, `storeRackRow` stamp a
	/// tag). By id rather than by lane since kill-lanes step 3b.
	void storeRowOf(DeviceInstanceId device, const DeviceRow& row) noexcept;
	/// See fxChainCopiesForTest(). Mutable because fxChain() is const and the count is bookkeeping about
	/// reads, not state; UI thread only.
	mutable size_t fxChainCopies_ = 0;

	/// THE CONTAINERS' BODIES. See `fxBodies`. Message thread only.
	ContainerBodies fxBodies_;
	/// HOW EACH DEVICE'S CARD IS SHOWN — see `deviceFolded`. Message thread only, like the bodies beside it,
	/// and for the same reason: it is written by a load and read by a save, both message-thread roads.
	rack::DeviceViews cardViews_;
	/// WHAT EACH DEVICE'S OUTPUTS ARE CALLED — see `outputLabel`. Beside the folds, on their road and rule.
	rack::DeviceLabels outputLabels_;
	/// Told when one moves (`subscribeOutputLabel`). Main thread only.
	device::Subscribers<DeviceInstanceId, size_t> outputLabelSubscribers_;
	/// WHICH OUTPUTS EACH DEVICE'S CARD SHOWS — see `outputsShown`. Absent: all.
	std::unordered_map<DeviceInstanceId, uint64_t> outputsShown_;
	/// See `fragmentOrigin` and `fragmentLife` (backlog OA): a serial naming this bridge, and the token its undo
	/// records hold the weak half of.
	const uint64_t fragmentOrigin_ = rack::mintFragmentOrigin();
	/// THE STRIP'S SITE (backlog PN): holds only a reference back to this bridge, so it lives exactly as long.
	StripSite stripSite_ { *this };
	std::shared_ptr<const void> life_ = std::make_shared<char>(0);
	rack::CanvasView canvasView_;

	/// TAKE `candidate` IF IT IS AHEAD OF WHERE THE COUNTER STANDS. The restore paths' door: a state
	/// load hands the rack's serial and the lane's, and the counter must end up past BOTH.
	void adoptInstanceCounter(DeviceInstanceId candidate) noexcept {
		if (candidate > kNoInstance) {
			ids_.reserve(candidate - 1); // the next id issued is `candidate` or later
		}
	}

	/// Which sound is being listened to on its own, or kNone. One relaxed atomic: a read that straddles
	/// the write sees either the old sound or the new one, and a block of either is a block of a sound
	/// somebody asked to hear.
	std::atomic<size_t> preview_ { Preview::kNone };
	std::atomic<bool> stateDirty_ { false };
	/// `changed`, with the patch marked dirty when it is — the host-slot verbs' one line of bookkeeping.
	/// A touch that took a NEW slot, owed a service at the Begin that follows it (`beginControlGesture`).
	bool firstGrab_ = false;
	std::function<void()> firstGrabService_;
	bool markedIf(bool changed) noexcept {
		if (changed) {
			stateDirty_.store(true, std::memory_order_relaxed);
		}
		return changed;
	}
	/// The whole report — every kind, and the refused routes' endpoints. MAIN THREAD ONLY, and a plain
	/// member for that reason: see noteRefusedRoutes.
	rack::LoadReport loadReport_ {};
	/// Set by every editor-written channel, drained once a frame by the editor's tick. See
	/// takeRepaintDirty for why this is not the same bit as stateDirty_.
	std::atomic<bool> repaintDirty_ { false };
	std::atomic<double> puckVelocityX_ { 0.0 };
	std::atomic<double> puckVelocityY_ { 0.0 };
	/// `[slot * 2]` radius, `[slot * 2 + 1]` theta — see publishTravelPulls.
	std::array<std::atomic<double>, kMaxSounds * 2> travelPulls_ {};
	/// The shares and which slots are in the blend at all. Relaxed, like every other engine→editor
	/// reading on this bridge; the ordering that matters is stated on publishSoundWeights.
	std::array<std::atomic<double>, kMaxSounds> soundWeight_ {};
	std::atomic<uint8_t> soundWeightLive_ { 0 };
	/// EVERY ROUTE, BY IDENTITY, AS PUBLISHED FROM THE RECORD — see `routeList()`. The record (`chains_`)
	/// holds the routes since 5b C5; this is what it publishes for the engine and what the editor's reads
	/// walk. UNBOUNDED (backlog OJ): it grows, and tells `routeGrowth_` first.
	backplane::RouteList routeList_;
	std::function<void(size_t)> routeGrowth_;
	/// Republish `routeList_` from the record — after a route edit or a structural one, never a knob.
	void publishRoutes() noexcept;

	// Written by the engine, read by the editor.
	std::array<std::atomic<double>, kParamCount> publishedBase_;
	std::array<std::atomic<double>, kParamCount> publishedMod_;
	/// The right ear and whether it is live, for a `stereo` parameter (backlog KU) — see `modulatedRight`.
	std::array<std::atomic<double>, kParamCount> publishedModR_;
	std::array<std::atomic<bool>, kParamCount> publishedStereoLive_;

	/// THE GENERIC READINGS CHANNEL — see `publishDeviceReadings`. A fixed stride so a slot's values
	/// are contiguous and no device can write into its neighbour's.
	///
	/// **FORTY-EIGHT, AND IT WAS TWELVE UNTIL SKLITTER PUBLISHED A PICTURE.** A stride equal to the
	/// widest publisher is a stride with no headroom: the next reading added to any device silently
	/// truncates at the boundary, and `publishDeviceReadings` clamps rather than failing, so the
	/// symptom is a meter quietly reading zero. It was eight until Bus Utility published an eighth,
	/// then twelve — and SKLITTER's band needs `kReadingCount`, a header plus two floats for each of
	/// sixteen steps, which is 37. **The truncation showed up exactly as predicted here**: the card
	/// asked for four chunks and decoded three, because `(12 - 5) / 2` is 3.
	///
	/// FORTY-EIGHT rather than 37, for the same reason twelve was not eight: the next thing anyone
	/// publishes should not have to come back here. The cost is `kMaxFxSlots * 48` atomics.
	/// **160 SINCE BACKLOG HR ROUND 2** (2026-09-23): SKLITTER's spark events — four grain onset counters,
	/// the Repeat counter and its period — took the header to 17 and the count to 145, one past 144.
	/// **224 SINCE BACKLOG IX, the same day:** a step is three floats (`fx::sklitter::stepReading`) and the
	/// header is 18, so SKLITTER publishes 18 + 3 × 64 = 210.
	static constexpr size_t kMaxDeviceReadings = 224;
	/// **THE WIDEST PUBLISHER, PINNED AGAINST THE STRIDE.** The truncation this stride can cause is
	/// silent by construction — `publishDeviceReadings` clamps — so the guard cannot be a runtime
	/// check that nobody reads. SKLITTER is the widest; the day another device passes it, this fails to
	/// compile rather than drawing a short picture.
	static_assert(fx::sklitter::kReadingCount <= kMaxDeviceReadings,
	              "a device publishes more readings than the bridge's stride carries; raise the stride");
	// THE EQ IS THE SECOND PICTURE (its analyser frame, 195 floats), and it gets its own line for the same reason.
	static_assert(fx::eq::kReadingCount <= kMaxDeviceReadings,
	              "the EQ publishes more readings than the bridge's stride carries; raise the stride");

	/// **EVERYTHING THE ENGINE TELLS THE EDITOR ABOUT ONE FX POSITION, IN ONE PLACE** (unbounded devices
	/// step 2). These were thirteen arrays sized by `kMaxFxSlots`, which is a cap; one element per position
	/// in a pool that grows (`backplane::ChunkedPool`) is not. Every member is an atomic written by the
	/// audio thread and read by the editor, relaxed, as each array was — and the pool never moves an
	/// element, so a growth landing mid-block cannot pull a cell out from under its writer.
	///
	/// **STILL BY POSITION**, as every one of the arrays was: a reorder points a position at another
	/// device for the frame before the next publish. That is the design doc's R4, and it is a separate
	/// change from the cap.
	/// THE READOUT'S PAIR AND ITS ANSWER (backlog PU) — mutable: asking is a read of the bridge to every caller.
	mutable backplane::RouteProbeChannel routeProbe_;
	struct FxTelemetry {
		/// Where modulation has left each field — see fxModulatedFieldAt. Written once a block from
		/// `Synth::modulatedFxChain()`; the width after the fields, never before.
		std::array<std::atomic<float>, kMaxDeviceFields> modulated {};
		std::atomic<size_t> modulatedFieldsPresent { 0 };
		/// THE RIGHT EAR (backlog KU): the values, and which controls a stereo route reaches — see publishFxRightEar.
		std::array<std::atomic<float>, kMaxDeviceFields> modulatedR {};
		std::array<std::atomic<uint64_t>, device::kControlMaskWords> stereoLive {};
		/// OWLTT's meters — one set per position so two OWLTTs cannot collide.
		std::array<std::atomic<float>, 3> owlttGrDB {};
		std::array<std::atomic<float>, 3> owlttInputLevelDB {};
		std::array<std::atomic<float>, 3> owlttOutputLevelDB {};
		std::atomic<bool> owlttClipping { false };
		std::array<std::atomic<bool>, 3> owlttBandSaturating {};
		std::atomic<float> owlttCombinedGrDb { 0.0f };
		std::atomic<float> owlttNoteHz { 0.0f }; ///< Key Track's note, Hz (reading 17)
		/// Each band's SECOND channel's gain reduction (backlog LQ; R, or S in M/S) — `owlttGrDB` is the first's.
		std::array<std::atomic<float>, 3> owlttGrSecond {};
		/// The automodulator's LFO phase, 0..1, and its envelope.
		std::atomic<float> automodPhase { 0.0f };
		std::atomic<float> automodEnvelope { 0.0f };
		/// THE GENERIC READINGS CHANNEL (`publishDeviceReadings`), and **HOW MANY THAT POSITION PUBLISHED,
		/// STORED RATHER THAN ASSUMED** — a count taken from a neighbour is how OWLTT's bars froze.
		std::array<std::atomic<float>, kMaxDeviceReadings> readings {};
		std::atomic<uint32_t> readingCount { 0 };
		/// **THE DRIVE METER STARTS AT THE SENTINEL, NOT AT ZERO.** 0.0f is a REAL level; the negative says
		/// "nobody has published here", which is the true picture before the engine's first block — and a
		/// position the pool has only just grown must say the same. See `publishFxDrivePeak`.
		std::atomic<float> drivePeak { -1.0f };
		/// THE COURTESY OUTPUT METER (backlog MK), the same sentinel for the same reason — see `publishFxOutputPeak`.
		std::atomic<float> outputPeak { -1.0f };
		/// A lane modulator's playhead, packed as `RackTelemetry::playhead` is. See `publishFxPlayhead`.
		std::atomic<uint64_t> playhead { 0 };
		std::atomic<float> playheadLevel { -1.0f }; ///< as `RackTelemetry::playheadLevel`
		std::atomic<uint64_t> trigFires { 0 }; ///< as `RackTelemetry::trigFires`
		std::array<std::atomic<uint64_t>, device::kRecentEvents> recent {}; ///< as `RackTelemetry::recent`
		device::ExtraStateSnapshot extraState; ///< as `RackTelemetry::extraState`
	};

	/// **AND ONE RACK POSITION'S** — the modulated row, its tag and width, and the playhead.
	struct RackTelemetry {
		/// The rack's table holding where MODULATION has left each control — see publishModulated.
		std::array<std::atomic<float>, kMaxDeviceFields> modulated {};
		/// **THE TAG OF THE ROW BESIDE IT, AND THAT IS THE WHOLE POINT.** The modulated channel and the
		/// raw channel are two publishes, so reading the type from one while decoding fields from the
		/// other is a key taken from one snapshot and contents from another. Stamped by the same loop
		/// that writes the fields, so the two move together by construction (backlog BK).
		std::atomic<uint64_t> modulatedTag { 0 };
		/// How wide the modulated row is — its own cell rather than the raw width, because the two are
		/// written by different publishes.
		std::atomic<size_t> modulatedFieldsPresent { 0 };
		/// THE RIGHT EAR (backlog KU) — see publishRackRightEar.
		std::array<std::atomic<float>, kMaxDeviceFields> modulatedR {};
		std::array<std::atomic<uint64_t>, device::kControlMaskWords> stereoLive {};
		/// Where the modulator stands, packed: ordinal in the high word, the phase's bits in the low, so a
		/// reader cannot take an ordinal from one tick beside a phase from the next.
		std::atomic<uint64_t> playhead { 0 };
		/// THE LEVEL BESIDE IT (`mod::Playhead::level`), raw 0..1, or NEGATIVE for "not published" — a level is never
		/// below zero, so the sentinel cannot be a reading. Its own word, so it can be a tick apart from the playhead
		/// it stands beside: a star drawn one 1.3 ms tick off is not a picture anyone can see.
		std::atomic<float> playheadLevel { -1.0f };
		/// THE TRIG'S FIRE COUNT (`mod::Playhead::trigFires`, backlog MV): the count in the low word, bit 32 set when
		/// published. Its own word, a tick apart from the playhead at worst, which the pad's flash cannot show.
		std::atomic<uint64_t> trigFires { 0 };
		/// THE NEWEST EVENTS (`mod::Playhead::recent`), one word each (`device::packRecentEvent`), newest first. Each word is
		/// one event whole, so a reader may take them from different ticks and still pair every ordinal with its value.
		std::array<std::atomic<uint64_t>, device::kRecentEvents> recent {};
		/// THE EXTRA STATE THE DEVICE CHANGED ITSELF (backlog MX) — see `publishRackExtraState`.
		device::ExtraStateSnapshot extraState;
	};

	backplane::ChunkedPool<FxTelemetry> fxTelemetry_ { kMaxFxSlots };
	backplane::ChunkedPool<RackTelemetry> rackTelemetry_ { kMaxRackSlots };

	/// **SECONDS PER BEAT, OR ZERO FOR "NO TEMPO"** — one number for the whole rack, published with the
	/// playheads because it is the same kind of fact on the same tick. Not per-slot: every device in
	/// the rack runs against one transport, which is the argument `DeviceHost::secondsPerBeat` makes.
	///
	/// A PLAIN `atomic<double>` RATHER THAN A PACKED `uint64_t`: the playheads are packed because an
	/// ordinal and a phase are TWO fields that must not be taken from different ticks. This is one
	/// field, so there is no pairing to protect and nothing to unpack.
	std::atomic<double> rackSecondsPerBeat_ { 0.0 };
	/// THE MASTER METER — see `publishMasterMeter`. At rest: silence, no peak, no clip for as long as it counts.
	std::atomic<float> masterMomentary_ { fx::busutil::kSilenceLufs };
	/// The kit pads' hits (pad_hits.h): a count and a velocity per pad, each its own relaxed atomic.
	std::array<std::atomic<uint32_t>, kMaxSounds> padHitCount_ {};
	std::array<std::atomic<float>, kMaxSounds> padHitVelocity_ {};
	std::atomic<float> masterRawPeak_ { 0.0f };
	std::atomic<float> masterClipAge_ { fx::busutil::kClipAgeMax };

	// The editor's own copy of the last snapshot it saw. Not atomic and not shared: only refresh()
	// and the readers on its thread ever touch these.
	std::atomic<double> sampleRate_ { 48000.0 };
	std::array<double, kParamCount> seenBase_ {};
	std::array<double, kParamCount> seenMod_ {};
	/// WHAT THE ENGINE LAST SAID, as opposed to what the editor is currently SHOWING — and the
	/// distinction is the whole of a bug that made every knob flicker.
	///
	/// refresh() used to diff the published values against seenBase_/seenMod_, which are what the
	/// widgets draw. But setValue writes those optimistically, so that a hand sees its own move at
	/// once instead of a block later — and the very next refresh then read that optimism as a
	/// DISAGREEMENT with the engine and "corrected" it back to the value the engine had not yet been
	/// told about. Every parameter move therefore snapped back for at least one frame:
	///
	///     setValue(cutoff, 10.97)      editor shows 10.97
	///     refresh()                    editor shows  9.97   <- the old value, for a frame
	///     engine echoes                editor shows 10.97
	///
	/// The question refresh() has to ask is "what did the ENGINE change since I last looked", which is
	/// what these two answer. The editor's own optimism is then simply not its business: seenBase_ is
	/// left alone until a genuinely new published value arrives, and that value wins when it does —
	/// automation, a preset load and the engine's own echo all come down this road.
	std::array<double, kParamCount> lastPublishedBase_ {};
	std::array<double, kParamCount> lastPublishedMod_ {};
	std::array<double, kParamCount> lastPublishedModR_ {};
	std::array<bool, kParamCount> lastPublishedStereoLive_ {};
	std::array<double, kParamCount> seenModR_ {};
	std::array<bool, kParamCount> seenStereoLive_ {};
	/// WHICH PARAMETERS THE EDITOR ITSELF MOVED SINCE THE LAST REFRESH, so that the widgets which
	/// merely WATCH one still hear about it.
	///
	/// A knob redraws itself when a hand turns it, but the filter card's curve does not — it watches
	/// FilterCutoff and is redrawn by the notification. If refresh() reported only what the engine
	/// changed, the curve would wait a whole flush round trip behind the knob. So an editor-side write
	/// is REPORTED like any other change; what it must not do is drag seenBase_ back to the value the
	/// engine has not yet been told about, which is the bug this pair of arrays exists to separate.
	std::array<bool, kParamCount> editorTouched_ {};
	/// HOW MANY VALUE GESTURES ARE OPEN, and it exists because an unmatched one is HOST-FACING and
	/// invisible to everything else we have.
	///
	/// beginGesture pushes a real event into the queue and the host is told a parameter is being
	/// written; endGesture tells it the write finished. Drop the end and the DAW is left believing a
	/// gesture is still in progress — in Bitwig an automation lane stuck in write, a parameter that
	/// stops answering automation — and NOTHING in this repo would notice, because every case we own
	/// checks what the bridge HOLDS and never what the host was TOLD around it.
	///
	/// DELIBERATELY NOT THE ROUTE GESTURES. beginRouteGesture is an empty function and endRouteGesture
	/// only sets stateDirty_, which setRouteAmount already sets on every write — so counting those
	/// would be an instrument measuring nothing, which in a test suite is its own kind of lie. If a
	/// route bracket ever reaches the host, it earns a counter that day.
	int openGestures_ = 0;

	/// See `editHistory`. The editor's, never owned here.
	ui::EditHistory* editHistory_ = nullptr;

	std::vector<uint32_t> changed_;
	bool everRefreshed_ = false;
};

} // namespace feathers
