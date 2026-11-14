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

#include "dsp/drum_voice.h"
#include "plugin/gui_bridge.h"
#include "plugin/params.h"
#include "plugin/ui/expanded_overlay.h"
#include "plugin/ui/frame_watchers.h"
#include "plugin/ui/ui_state.h"
#include "plugin/ui/widget_registry.h"

#include <array>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <visage/ui.h>

#include "dsp/character_bank.h"
#include "plugin/ui/controls.h"
#include "plugin/ui/fader_pill.h"
#include "plugin/ui/fx_lane_rail.h"
#include "plugin/ui/knob.h"
#include "plugin/ui/param_control.h"
#include "plugin/ui/switch_chip.h"
#include "plugin/ui/zone_coord_cell.h"
#include "plugin/ui/value_box.h"

// THE DRUM KIT PANEL (the drum engine design, and its S0 sketch): a floating window the Drum tab on Explore opens, as the Spectrum tab opens
// the spectrum panel. Composed around the HIT PICTURE — the selected pad's response drawn as its modes or bands fading over
// time — with the pads beside it and the pad's controls under it. The seven field scalers and the Kit Walk are on the Explore
// surround (the selected sound is the selected pad), so they are not repeated here.
//
// PER PAD: eight sets of controls, each bound at construction to its own pad's params, one shown per selection (the spectrum
// panel's pattern: a rebindable control is a control that can be pointing somewhere its label does not say).
//
// THE HONESTY RULE: only what the selected pad's kind reads is drawn (drum::kindControls), Release only on a Gate pad, the pads
// and each pad's Kind, Note, Choke and Voices only in Kit mode, the engine's Kind only in Pitched.

namespace feathers::ui {

class Notifier;

/// The tab: Explore's bottom edge, the word KIT; a click opens or closes the panel. Drawn on the Drum engine alone.
class DrumTab : public visage::Frame {
public:
	static constexpr float kWidth = 84.0f;
	static constexpr float kHeight = 22.0f;
	explicit DrumTab(std::function<void()> toggle);
	void draw(visage::Canvas& canvas) override;
	void setOpen(bool open);
	[[nodiscard]] bool caretLitForTest() const { return open_; }
	void clickForTest() { toggle_(); }

private:
	std::function<void()> toggle_;
	bool open_ = false;
};

/// The picture of a hit, read from the same laws the engine plays (drum::lawsFor, drum::modeRatio, drum::metalRatio): each
/// mode or oscillator a line at its frequency fading over its T60, each band a filled strip fading over its own.
/// **THE HIT PICTURE, AND THE CONTROLS ON IT** (the standing rule: compose around a visualiser, the controls on the plot).
/// The pad's hit as the engine plays it at the pad's real place: across the top the frequencies over time (a Kick's sweep
/// as a bending trace, a Membrane's and Metal's modes as lines fading over their own T60, the noise as bands), and under it
/// the amplitude envelope, its knee shaped by Drive. The handles: TUNE on the resting pitch (drag up and down), SWEEP T on
/// the sweep's landing (across), DECAY at the body's T60 (across), LEVEL at the envelope's peak (up and down) and DRIVE at
/// its knee (up and down), each drawn only where the kind reads it.
class HitPicture : public visage::Frame {
public:
	enum class Handle : uint8_t {
		Tune, SweepTime, Decay, Level, Drive, WireDecay, ClickDecay, BellStart, BellEnd, Bell2, Hold, Dip, ClickSweep,
		/// Onto the picture by the consolidation (drum proposal §14 lever 3): a Kick's Start Phase (up and down), a Clap's Spread
		/// (across) and its band (up and down: its High Pass corner; across: its Q).
		StartPhase, Spread, ClapBand, kCount
	};
	static constexpr size_t kHandleCount = static_cast<size_t>(Handle::kCount);
	/// The controls the handles write, the selected pad's (null: not drawn).
	struct Controls {
		ParamControl* tune = nullptr;
		ParamControl* sweepTime = nullptr;
		ParamControl* decay = nullptr;
		ParamControl* level = nullptr;
		ParamControl* drive = nullptr;
		ParamControl* driveTone = nullptr; ///< the Drive handle's ACROSS (§14 lever 3)
		ParamControl* startPhase = nullptr;
		ParamControl* spread = nullptr;
		ParamControl* clapHighPass = nullptr;
		ParamControl* clapQ = nullptr;
		/// A Snare's: the wires' and the click's decays, and the moving bell (its end handle sets End up and down, Time across).
		ParamControl* wireDecay = nullptr;
		ParamControl* clickDecay = nullptr;
		ParamControl* bellStart = nullptr;
		ParamControl* bellEnd = nullptr;
		ParamControl* bellTime = nullptr;
		/// The second bell: up and down sets its Hz while Free (Linked, its ratio is the place's Metal, §14 lever 1, so up and
		/// down writes nothing); across, its ring (Bell 2 Time).
		ParamControl* bell2 = nullptr;
		ParamControl* bell2Time = nullptr;
		/// A Kick's: Hold (across), the fishtail (across: its time; up and down: its depth), the click sweep (up and down: its
		/// octaves; across: its time).
		ParamControl* hold = nullptr;
		ParamControl* dip = nullptr;
		ParamControl* dipTime = nullptr;
		ParamControl* clickSweep = nullptr;
		ParamControl* clickSweepTime = nullptr;
		friend bool operator==(const Controls&, const Controls&) = default;
	};

	HitPicture();
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
	[[nodiscard]] std::string readoutOf(Handle h) const;
	/// TEST ONLY: a hand's drag of handle `h` by (dx, dy) pixels: press on it, move, release.
	void dragForTest(Handle h, float dx, float dy);
	/// The handle a press at `p` takes (the nearest within the hit slop), or kCount: the press's own choice.
	[[nodiscard]] Handle handleUnder(visage::Point p) const;
	/// The second control a two-way handle writes (Drive's tone, a bell's time, the clap band's Q), or null.
	[[nodiscard]] ParamControl* secondOf(Handle h) const;
	/// Where `seconds` is drawn across the plot (square-root time), and the time under `x`.
	[[nodiscard]] float xOf(double seconds) const;
	[[nodiscard]] double secondsAt(float x) const;
	/// The time a time handle dragged to `x` sets: `secondsAt` inside the plot, and past its right edge the span doubling every
	/// kDragOctavePixels, so the handle can always reach its control's whole range.
	[[nodiscard]] double dragSeconds(float x) const;
	static constexpr double kDragOctavePixels = 40.0;
	/// Where `hz` is drawn up the plot (log frequency, 20 Hz to 20 kHz over its top 58%).
	[[nodiscard]] float yOfHz(double hz) const;

private:
	ParamControl* controlOf(Handle h) const;
	float plotLeft() const { return 10.0f; }
	float plotRight() const { return width() - 10.0f; }
	float specTop() const { return 8.0f; }
	float specBottom() const { return height() * 0.58f; }
	float envTop() const { return height() * 0.64f; }
	float envBottom() const { return height() - 8.0f; }
	double hzAt(float y) const;
	float yOfLevel(double db) const;
	double span() const { return dragging_ ? frozenSpan_ : naturalSpan(); }
	double naturalSpan() const;
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
	ParamControl* heldControl_ = nullptr;
	ParamControl* heldSecond_ = nullptr;
	visage::Point pressAt_ {};
	double startValue_ = 0.0;
	double startSecond_ = 0.0; ///< the second control's value at the press (Drive Tone, Clap Q)
	float startX_ = 0.0f;
	float startY_ = 0.0f;
	double startMultiplier_ = 1.0;
	double frozenSpan_ = 1.0;
};

class DrumPanel : public ExpandedView {
public:
	static constexpr float kMinimumWidth = 642.0f; // ONE ROW OF EIGHT PADS: 2·12 + 8·72 + 7·6, locked
	static constexpr float kMinimumHeight = 572.0f; // under the one row: the zone row, the pills and the Layer row (measured 564 at its fullest)

	DrumPanel(ParamBridge& bridge, WidgetRegistry& registry, SoundSelection& selection, Notifier* notifier);
	~DrumPanel() override;

	void draw(visage::Canvas& canvas) override;
	void resized() override;
	/// THE PAD ROW'S WIDTH PLUS THE PAD FX RAIL'S (drum proposal §17 P3), and an open rail's height as a floor (SKLITTER's rule).
	visage::Point minimumSize() const override;
	/// THE WIDTH IS LOCKED to the pads' row; only the height grows.
	visage::Point maximumSize() const override { return { minimumSize().x, kNoCap }; }
	void setPresentedOn(SurfaceLayers* host) override;
	/// TEST ONLY: the pad fx rail (null never: it is built with the panel).
	[[nodiscard]] FxLaneRail& laneRailForTest() const { return *lane_; }
	/// TEST ONLY: whether pad `pad`'s tile wears the FX dot (its lane holds a device).
	[[nodiscard]] bool padShowsFxForTest(size_t pad) const { return padHasFx(pad); }
	std::string title() const override { return "Drum Kit"; }
	std::string headerTitle() const override;
	bool updateContent() override;

	/// The pad a hand clicks: selects that sound (the selection Explore and this panel share).
	void clickPadForTest(size_t pad);
	/// TEST ONLY: where pad `pad`'s tile is drawn.
	[[nodiscard]] visage::Bounds padBoundsForTest(size_t pad) const { return padBounds(pad); }
	/// TEST ONLY: a pad's box, segmented control or pill by DrumControl; null for a control drawn as a handle on the
	/// picture (Tune, Decay, Sweep Time, Drive, Level: `handleShownForTest`) or one on the Explore surround (the scalers).
	[[nodiscard]] visage::Frame* controlForTest(size_t pad, DrumControl c) const;
	/// Whether pad `pad`'s control `c` is drawn and live, wherever it lives (a box, a pill, or a handle on the picture).
	[[nodiscard]] bool drawnForTest(size_t pad, DrumControl c) const;
	[[nodiscard]] const HitPicture& pictureForTest() const { return picture_; }
	[[nodiscard]] HitPicture& pictureForTest() { return picture_; }
	[[nodiscard]] bool padsShownForTest() const { return padsShown_; }
	/// THE HIT PULSE: read the published hits, light each pad whose count moved, and answer
	/// whether any is lit; and how lit pad `pad` is now (its hit's velocity, falling over kPulseMs; flat under reduced motion).
	bool updatePulses(int64_t nowMs);
	[[nodiscard]] float pulseOf(size_t pad) const;
	static constexpr float kPulseMs = 150.0f;
	/// The colour pad `pad` wears: its sound's (the card's dot, the star).
	[[nodiscard]] uint32_t padHue(size_t pad) const;

protected:
	void modulationBound(ModState* mod, EditHistory* history) override;

private:
	struct PadSet {
		std::unique_ptr<ParamControl> kindControl, noteControl, chokeControl, countControl;
		std::unique_ptr<ValueBox> kind, note, choke, count;
		std::unique_ptr<SegmentedControl> voices, trigger, driveCurve;
		/// The handles' controls (the picture writes them for the selected pad).
		std::unique_ptr<ParamControl> tune, sweepTime, decay, level, drive, driveTone;
		std::unique_ptr<ParamControl> wireDecay, clickDecay, bellStart, bellEnd, bellTime, bell2Hz, bell2Time;
		std::unique_ptr<ParamControl> hold, dip, dipTime, clickSweep, clickSweepTime, startPhase;
		std::unique_ptr<ParamControl> spread, clapQ, clapHighPass;
		/// THE ZONE CONTROLS (drum proposal §14): Velocity (every kind), Wires and Bells (a Snare's).
		std::unique_ptr<ParamControl> velocityZoneControl, wiresControl, bellsControl;
		std::unique_ptr<ZoneCoordCell> velocityZone, wires, bells;
		/// The pills: Level and Tune (mirroring their handles), Pan, Release (a Gate pad's), Velocity Noise (kinds that read
		/// Noise; its own control by design), Width (a Snare's and a Clap's), Bend (Kit mode alone; decision 8).
		std::unique_ptr<ParamControl> levelMirrorControl, tuneMirrorControl, panControl, releaseControl, velocityNoiseControl,
		    widthControl, bendControl;
		std::unique_ptr<FaderPill> levelMirror, tuneMirror, pan, release, velocityNoise, width, bend;
		/// THE LAYER ROW (drum proposal §12, stage (c)), Kit mode alone: its kind, and while it is on its Level, Delay, High
		/// Pass, Lock and the offset vector from the pad.
		std::unique_ptr<ParamControl> layerKindControl, layerLevelControl, layerDelayControl, layerHighPassControl,
		    layerLockControl, layerOffsetXControl, layerOffsetYControl;
		std::unique_ptr<ValueBox> layerKind;
		std::unique_ptr<FaderPill> layerLevel, layerDelay, layerHighPass, layerOffsetX, layerOffsetY;
		std::unique_ptr<SwitchChip> layerLock;
	};

	bool kitMode() const;
	drum::Kind kindOf(size_t pad) const;
	/// Show the selected pad's set, honestly, and hide every other; lay the shown controls out. Answers whether anything moved.
	bool sync();
	visage::Bounds padBounds(size_t pad) const;
	/// THE PAD FX RAIL (drum design §17 P3: it hangs off the kit window's RIGHT side, as SKLITTER's inner chain does,
	/// and shows the FOCUSED pad's lane). Kit mode alone; its width (collapsed or open) is outside the pad row's.
	std::unique_ptr<FxLaneRail> lane_;
	bool phantomShown_ = false;
	bool pendingAddMenu_ = false;
	/// The rail's width while it is shown (Kit mode), else 0; and the content's width, the pad row's.
	[[nodiscard]] float railWidthNow() const;
	/// Follow the selected pad's lane (a phantom (+) while it has none). Whether anything moved.
	bool followLane();
	[[nodiscard]] bool padHasFx(size_t pad) const;
	/// The pad's fields where it really stands (its place, its drift, the Walk, its likes' warp), by the bank's own road.
	drum::Fields fieldsOf(size_t pad) const;
	drum::Laws lawsOf(size_t pad) const;

	ParamBridge& bridge_;
	std::array<uint32_t, kMaxSounds> pulseSeen_ {};
	std::array<float, kMaxSounds> pulseLevel_ {};
	std::array<int64_t, kMaxSounds> pulseAt_ {};
	int64_t pulseNow_ = 0;
	SoundSelection& selection_;
	std::vector<Watch> watches_;
	Watch selectionWatch_;

	std::unique_ptr<SegmentedControl> mode_;
	std::unique_ptr<ParamControl> pitchedKindControl_, seedControl_;
	std::unique_ptr<ValueBox> pitchedKind_, seed_;
	std::array<PadSet, kMaxSounds> pads_ {};
	HitPicture picture_;
	/// The likes' warp per sound as the synth attaches them (WaveDisplay's), so the picture is of what plays.
	mutable std::array<LikeWarp, kMaxSounds> likeTable_ {};

	size_t shownPad_ = kMaxSounds;
	bool shownKit_ = false;
	drum::Kind shownKind_ = drum::Kind::kCount;
	bool shownGate_ = false;
	bool padsShown_ = false;
};

} // namespace feathers::ui
