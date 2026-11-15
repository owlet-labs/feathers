/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even
 * the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

#pragma once

#include "device/modulatable.h"
#include "dsp/drum_voice.h"
#include "plugin/ui/frame_watchers.h"
#include "plugin/ui/edit_history.h"
#include "plugin/ui/fine_adjust.h"
#include "plugin/ui/mod_handle.h"
#include "plugin/ui/tooltip.h"

#include <array>
#include <cstdint>
#include <string>
#include <visage/ui.h>

// THE DRUM HIT PICTURE, SHARED: the drum panel (the synth's Drum engine) and the Drum device's card and window draw the SAME picture of a hit
// over `device::Modulatable` controls, so neither names a parameter id or a device index.

namespace feathers::ui {

/// The picture of a hit, read from the same laws the engine plays (drum::lawsFor, drum::modeRatio, drum::metalRatio): each
/// mode or oscillator a line at its frequency fading over its T60, each band a filled strip fading over its own.
/// **THE HIT PICTURE, AND THE CONTROLS ON IT** (the standing rule: compose around a visualiser, the controls on the plot).
/// The pad's hit as the engine plays it at the pad's real place: across the top the frequencies over time (a Kick's sweep
/// as a bending trace, a Membrane's and Metal's modes as lines fading over their own T60, the noise as bands), and under it
/// the amplitude envelope, its knee shaped by Drive. The handles: TUNE on the resting pitch (drag up and down), SWEEP T on
/// the sweep's landing (across), DECAY at the body's T60 (across), LEVEL at the envelope's peak (up and down) and DRIVE at
/// its knee (up and down), each drawn only where the kind reads it.
class HitPicture : public visage::Frame, public TooltipSource {
public:
	/// THE HANDLE'S OWN CONTROL'S TOOLTIP where the pointer is over one, the picture's otherwise.
	const char* tooltipAt(visage::Point local) const override;
	/// Every control a shown handle writes (and each two-way handle's second), as a gesture on the plot.
	void offeredControls(std::vector<Offered>& out) const override;
	enum class Handle : uint8_t {
		Tune, SweepTime, Decay, Level, Drive, WireDecay, ClickDecay, BellStart, BellEnd, Bell2, Hold, Dip, ClickSweep,
		/// Onto the picture by the consolidation (drum proposal §14 lever 3): a Kick's Start Phase (up and down), a Clap's Spread
		/// (across) and its band (up and down: its High Pass corner; across: its Q).
		StartPhase, Spread, ClapBand, kCount
	};
	static constexpr size_t kHandleCount = static_cast<size_t>(Handle::kCount);
	/// The controls the handles write, the selected pad's (null: not drawn).
	struct Controls {
		device::Modulatable* tune = nullptr;
		device::Modulatable* sweepTime = nullptr;
		device::Modulatable* decay = nullptr;
		device::Modulatable* level = nullptr;
		device::Modulatable* drive = nullptr;
		device::Modulatable* driveTone = nullptr; ///< the Drive handle's ACROSS (§14 lever 3)
		device::Modulatable* startPhase = nullptr;
		device::Modulatable* spread = nullptr;
		device::Modulatable* clapHighPass = nullptr;
		device::Modulatable* clapQ = nullptr;
		/// A Snare's: the wires' and the click's decays, and the moving bell (its end handle sets End up and down, Time across).
		device::Modulatable* wireDecay = nullptr;
		device::Modulatable* clickDecay = nullptr;
		device::Modulatable* bellStart = nullptr;
		device::Modulatable* bellEnd = nullptr;
		device::Modulatable* bellTime = nullptr;
		/// The second bell: up and down sets its Hz while Free (Linked, its ratio is the place's Metal, §14 lever 1, so up and
		/// down writes nothing); across, its ring (Bell 2 Time).
		device::Modulatable* bell2 = nullptr;
		device::Modulatable* bell2Time = nullptr;
		/// A Kick's: Hold (across), the fishtail (across: its time; up and down: its depth), the click sweep (up and down: its
		/// octaves; across: its time).
		device::Modulatable* hold = nullptr;
		device::Modulatable* dip = nullptr;
		device::Modulatable* dipTime = nullptr;
		device::Modulatable* clickSweep = nullptr;
		device::Modulatable* clickSweepTime = nullptr;
		friend bool operator==(const Controls&, const Controls&) = default;
	};

	HitPicture();
	/// **EVERY HANDLE A MODULATION TARGET** (`ui::ModHandle`, as OWLTT's plot handles are): where a handle's control is a destination it wears an
	/// amount arc along its axis, a live dot where modulation has put it, and takes the sources menu, an armed press or Alt as a route gesture,
	/// and a value drag otherwise. Off by default (the synth's panel keeps its own pills); the Drum device's window turns it on.
	void setModHandles(bool on) { modHandles_ = on; redraw(); }
	[[nodiscard]] bool modHandles() const noexcept { return modHandles_; }
	/// The handle's modulation plan (its axes' arcs and dots), for the cases; empty axes where the control is no destination.
	[[nodiscard]] ModHandlePlan modPlanOf(Handle h) const { return mods_[static_cast<size_t>(h)].plan(); }
	/// A COMPACT PICTURE (a card's) draws its handles without their readouts, which would crowd a small plot; the tooltip still names them.
	void setLabelsShown(bool shown) { if (shown != labels_) { labels_ = shown; redraw(); } }
	[[nodiscard]] bool labelsShown() const noexcept { return labels_; }
	void draw(visage::Canvas& canvas) override;
	/// What to draw and which handles to offer. `decaySeconds` is the pad's Decay control (the laws' T60 is it times Length).
	/// Compared on set, so a frame where nothing moved does not repaint.
	void show(drum::Kind kind, const drum::Laws& laws, double decaySeconds, double levelDb, const Controls& controls);

	[[nodiscard]] drum::Kind kindForTest() const { return kind_; }
	[[nodiscard]] const drum::Laws& lawsForTest() const { return laws_; }
	[[nodiscard]] bool handleShown(Handle h) const;
	/// Where a handle is drawn, in this frame's coordinates.
	[[nodiscard]] visage::Point handleAt(Handle h) const;
	/// Where each shown handle's readout is drawn (collision-avoiding), and its text.
	[[nodiscard]] std::array<visage::Bounds, kHandleCount> labelRects() const;
	/// TEST ONLY: how many times the placement has been computed rather than served from the cache.
	[[nodiscard]] size_t labelBuildsForTest() const { return labelBuilds_; }
	[[nodiscard]] std::string readoutOf(Handle h) const;
	/// TEST ONLY: a hand's drag of handle `h` by (dx, dy) pixels: press on it, move, release.
	void dragForTest(Handle h, float dx, float dy, bool fine = false);
	/// The handle a press at `p` takes (the nearest within the hit slop), or kCount: the press's own choice.
	[[nodiscard]] Handle handleUnder(visage::Point p) const;
	/// The second control a two-way handle writes (Drive's tone, a bell's time, the clap band's Q), or null.
	[[nodiscard]] device::Modulatable* secondOf(Handle h) const;
	/// Where `seconds` is drawn across the plot (soft-knee log time, see kAxisKnee), and the time under `x`.
	[[nodiscard]] float xOf(double seconds) const;
	[[nodiscard]] double secondsAt(float x) const;
	/// The time axis: soft-knee logarithmic and fixed (see `xOf`): linear below kAxisKnee, so 0 stands on the plot's left edge,
	/// logarithmic above it up to kAxisMax, the longest Decay a pad can have (10 s at Length x4).
	static constexpr double kAxisKnee = 0.001;
	static constexpr double kAxisMax = 40.0;
	/// Where a handle's HIT POINT and dot stand: `handleAt` (its value's place) nudged vertically, in a fixed order, clear of
	/// every handle before it, so handles at the same time each stay grabbable. `handleUnder` takes marks.
	[[nodiscard]] visage::Point markAt(Handle h) const;
	/// Where `hz` is drawn up the plot (log frequency, 20 Hz to 20 kHz over its top 58%).
	[[nodiscard]] float yOfHz(double hz) const;

private:
	device::Modulatable* controlOf(Handle h) const;
	float plotLeft() const { return 10.0f; }
	float plotRight() const { return width() - 10.0f; }
	float specTop() const { return 8.0f; }
	float specBottom() const { return height() * 0.58f; }
	float envTop() const { return height() * 0.64f; }
	float envBottom() const { return height() - 8.0f; }
	double hzAt(float y) const;
	float yOfLevel(double db) const;
	[[nodiscard]] std::array<visage::Point, kHandleCount> marks() const;
	[[nodiscard]] std::array<visage::Bounds, kHandleCount> placeLabels() const;
	void press(visage::Point p);
	void drag(visage::Point p);
	void release();

	drum::Kind kind_ { drum::Kind::Membrane };
	drum::Laws laws_ {};
	double decaySeconds_ = 0.5;
	double levelDb_ = 0.0;
	Controls controls_ {};
	bool dragging_ = false;
	Handle held_ = Handle::kCount;
	/// THE CONTROLS THE PRESS TOOK, held for the life of the drag: `show` rebuilds `controls_` from the pad's current Kind
	/// every tick, so a Kind or pad change mid-drag would answer another set (or none) to a re-resolve. The objects are
	/// the panel's own per-pad ones and outlive the picture; only which of them `controls_` names moves.
	device::Modulatable* heldControl_ = nullptr;
	device::Modulatable* heldSecond_ = nullptr;
	EditHistory* groupHistory_ = nullptr; ///< the history a two-way handle's one undo step is grouped in
	/// THE LABEL PLACEMENT, kept with everything it is a function of (bounds, scale, what is shown and its numbers):
	/// a paint that nothing changed serves it without placing eight labels again.
	struct LabelKey {
		float w = -1.0f, h = -1.0f, dpi = -1.0f;
		drum::Kind kind {};
		drum::Laws laws {};
		double decay = 0.0, level = 0.0;
		Controls controls {};
		bool dragging = false;
		friend bool operator==(const LabelKey&, const LabelKey&) = default;
	};
	mutable LabelKey labelKey_ {};
	mutable std::array<visage::Bounds, kHandleCount> labelCache_ {};
	mutable bool labelCached_ = false;
	mutable size_t labelBuilds_ = 0;
	visage::Point pressAt_ {};
	double startValue_ = 0.0;
	double startSecond_ = 0.0; ///< the second control's value at the press (Drive Tone, Clap Q)
	float startX_ = 0.0f;
	float startY_ = 0.0f;
	double startMultiplier_ = 1.0;
	bool labels_ = true;
	bool modHandles_ = false;
	std::array<ModHandle, kHandleCount> mods_ {};
	Handle routeHandle_ = Handle::kCount; ///< the handle whose route gesture is open
	void rebuildMods();
	[[nodiscard]] std::vector<ModHandleAxis> modAxesFor(Handle h);
	bool fine_ = false;     ///< Shift is held (the Tune handle moves whole semitones, or with Shift, finely)
	bool tuneFine_ = false; ///< the gearing the Tune handle's anchor was taken at
	float slack_ = 0.0f; ///< how far the hand is past where a time handle's control stops following it (px, signed)
};


} // namespace feathers::ui
