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
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <visage/ui.h>

#include "dsp/character_bank.h"
#include "plugin/ui/controls.h"
#include "plugin/ui/fader_pill.h"
#include "plugin/ui/fx_lane_rail.h"
#include "plugin/ui/knob.h"
#include "plugin/ui/param_control.h"
#include "plugin/ui/drum_vector_pad.h"
#include "plugin/ui/switch_chip.h"
#include "plugin/ui/tooltip.h"
#include "plugin/ui/type_in.h"
#include "plugin/ui/zone_coord_cell.h"
#include "plugin/ui/value_box.h"

// THE DRUM KIT PANEL (the drum engine design, and its S0 sketch): a floating window the Drum tab on Explore opens, as the Spectrum tab opens
// the spectrum panel. Composed around the HIT PICTURE — the shown layer's response drawn as its modes or bands fading over
// time — with the pads beside it and the pad's controls under it.
//
// A PAD IS TWO LAYERS (kit stance redesign), each a full hit of its own kind: two tabs under the pad-level controls (Note, Choke,
// Voices, Trigger, Pan, Bend, Release), the SAME layout on both, each addressing its layer's own parameters through
// `drumLayerParam`. A tab holds its Kind (layer two's has Off, then its Level, Delay and High Pass), its Lock, a small terrain pad
// for its vector, its terrain pitch mode and settled-pitch readout, its seven field scalers and every control its kind reads.
// The Explore surround holds only the kit trims. Lock is an editor flag: the kit's bulk edit skips a locked layer's vector,
// and the engine never reads it.
//
// PER PAD: eight sets of controls, each bound at construction to its own pad's params, one shown per selection (the spectrum
// panel's pattern: a rebindable control is a control that can be pointing somewhere its label does not say).
//
// THE HONESTY RULE: only what the shown layer's kind reads is drawn (drum::kindControls, drum::kindReads), Release only on a
// Gate pad, the pads, the tabs and each pad's Note, Choke and Voices only in Kit mode, the engine's Kind only in Pitched.

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
	/// TEST ONLY: how many times the placement has been computed rather than served from the cache.
	[[nodiscard]] size_t labelBuildsForTest() const { return labelBuilds_; }
	[[nodiscard]] std::string readoutOf(Handle h) const;
	/// TEST ONLY: a hand's drag of handle `h` by (dx, dy) pixels: press on it, move, release.
	void dragForTest(Handle h, float dx, float dy, bool fine = false);
	/// The handle a press at `p` takes (the nearest within the hit slop), or kCount: the press's own choice.
	[[nodiscard]] Handle handleUnder(visage::Point p) const;
	/// The second control a two-way handle writes (Drive's tone, a bell's time, the clap band's Q), or null.
	[[nodiscard]] ParamControl* secondOf(Handle h) const;
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
	ParamControl* controlOf(Handle h) const;
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
	ParamControl* heldControl_ = nullptr;
	ParamControl* heldSecond_ = nullptr;
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
	bool fine_ = false;     ///< Shift is held (the Tune handle moves whole semitones, or with Shift, finely)
	bool tuneFine_ = false; ///< the gearing the Tune handle's anchor was taken at
	float slack_ = 0.0f; ///< how far the hand is past where a time handle's control stops following it (px, signed)
};

/// THE PITCH A HIT SETTLES ON, AS TEXT AND BACK (the layer's settled-pitch readout, and the type-in that sets Tune from it).
namespace drum_pitch {
/// "55.0 Hz  A1 +0c", with a leading "~" when the pitch is approximate; empty when it is not defined.
[[nodiscard]] std::string readoutText(const drum::SettledPitch& pitch);
/// The nearest note's name and octave ("A1", "C#4"; octave numbering where A4 is 440 Hz) and the cents from it (-50..50).
struct NoteReading {
	std::string name;
	int cents = 0;
};
[[nodiscard]] NoteReading noteOf(double hz);
/// What was typed, as a frequency: a note name with an optional octave and cents ("a1", "C#4", "Bb2 -12c", "A1 +0c"), or a number
/// of Hz ("55", "55 Hz", "55.0 Hz  A1 +0c" reads its first number). Nothing for text that is neither.
[[nodiscard]] std::optional<double> parseHz(std::string_view text);
} // namespace drum_pitch

/// **THE TUNE CONTROL AS THE PITCH IT SETTLES ON**: reads and writes the layer's Tune, prints the settled pitch as text, and takes a
/// typed note or frequency back as the Tune that lands the settled pitch there (a shift of Tune by the ratio; Tune is a log of the
/// base pitch, so the settled pitch moves by exactly that ratio). Refuses to be typed into while the pitch is not defined (a Clap) or
/// approximate (a Metal), since there is no one pitch to land.
class SettledPitchControl final : public device::Modulatable {
public:
	SettledPitchControl(ParamControl& tune, std::function<drum::SettledPitch()> settled)
	    : tune_(tune), settled_(std::move(settled)) {}
	const device::ParamDesc& desc() const override { return tune_.desc(); }
	double value() const override { return tune_.value(); }
	void setValue(double value) override { tune_.setValue(value); }
	device::ModDisplay display() const override { return tune_.display(); }
	void setAmount(float amount) override { tune_.setAmount(amount); }
	void beginGesture() override { tune_.beginGesture(); }
	void endGesture() override { tune_.endGesture(); }
	std::string text() const override { return drum_pitch::readoutText(settled_()); }
	[[nodiscard]] std::optional<std::string> editText() const override;
	[[nodiscard]] std::optional<double> parseText(std::string_view text, double shown) const override;

private:
	ParamControl& tune_;
	std::function<drum::SettledPitch()> settled_;
};

/// **A PAD'S SOLO**: the editor's existing preview (the sound cards' S: this pad alone is heard), as a two-state control for a SwitchChip.
/// Not a parameter and not a modulation target; it reads and writes `ParamBridge::setPreview`/`clearPreview`, which the synth's `padAudible`
/// already honours, and nothing is stored with the patch.
class PadSoloControl final : public device::Modulatable {
public:
	PadSoloControl(ParamBridge& bridge, size_t slot, std::function<void()> changed)
	    : bridge_(bridge), slot_(slot), changed_(std::move(changed)) {}
	const device::ParamDesc& desc() const override;
	double value() const override { return bridge_.preview().active() && bridge_.preview().slot == slot_ ? 1.0 : 0.0; }
	void setValue(double value) override;
	device::ModDisplay display() const override { return {}; }
	void setAmount(float) override {}
	void beginGesture() override {}
	void endGesture() override {}
	std::string text() const override { return value() != 0.0 ? "On" : "Off"; }

private:
	ParamBridge& bridge_;
	size_t slot_;
	std::function<void()> changed_;
};

/// **A PAD'S PREVIEW BUTTON** (a play glyph): a press strikes the pad, the release lets it go, through the editor's note ring into the synth's own
/// note road (the pad's Note key, so choke groups, trigger modes, voices, the pad's fx lane and note devices behave as for a host note).
/// Not a parameter and not a modulation target; velocity is fixed at 100/127. Whatever ends a press (the mouse up, the pad being switched away,
/// the editor closing, the widget being destroyed) sends the off, so a note is never left stuck.
class PadPlayButton : public visage::Frame, public TooltipSource {
public:
	static constexpr float kVelocity = 100.0f / 127.0f;
	static constexpr float kWidth = 30.0f;
	static constexpr float kHeight = 22.0f;
	PadPlayButton(ParamBridge& bridge, size_t slot);
	~PadPlayButton() override;
	void draw(visage::Canvas& canvas) override;
	const char* tooltipAt(visage::Point) const override;
	void offeredControls(std::vector<Offered>&) const override {} // no control: a key, not a parameter
	/// A press: the pad's key on. A release: its off, for the key that was pressed (the Note may have moved since).
	void press();
	void release();
	[[nodiscard]] bool held() const noexcept { return held_; }
	[[nodiscard]] int heldKey() const noexcept { return heldKey_; }

private:
	ParamBridge& bridge_;
	size_t slot_;
	bool held_ = false;
	int heldKey_ = 0;
};

/// The settled pitch of a layer, drawn as text, with a hand on it: a click types a note or Hz into it, a drag moves Tune in whole
/// semitones (Shift: fine, `kFineAdjust` of the travel, not rounded).
class PitchReadout : public visage::Frame, public TooltipSource {
public:
	explicit PitchReadout(SettledPitchControl& control);
	void draw(visage::Canvas& canvas) override;
	void resized() override;
	const char* tooltipAt(visage::Point) const override;
	void offeredControls(std::vector<Offered>& out) const override { out.push_back({ &control_, Road::Face }); }
	/// The text drawn, and the field a click opens (null until it has).
	[[nodiscard]] std::string textForTest() const { return control_.text(); }
	/// Repaint if what it says has changed since it last drew (the panel asks every tick).
	void refresh(const std::string& text);
	[[nodiscard]] TextField* typeInField() const noexcept { return typeIn_.field(); }
	/// TEST ONLY: a hand's click (press and release on the spot), and a drag of `dy` pixels up (positive), `fine` for Shift.
	void clickForTest();
	void dragForTest(float dy, bool fine);
	/// How many pixels of vertical travel are worth one semitone.
	static constexpr float kPixelsPerSemitone = 6.0f;

private:
	[[nodiscard]] visage::Bounds typeInBounds() const { return { 0.0f, 0.0f, width(), height() }; }
	void beginHand(float y, bool fine);
	void moveHand(float y, bool fine);
	void endHand();
	SettledPitchControl& control_;
	TypeIn typeIn_ { *this };
	std::string drawn_;
	bool pressed_ = false;
	bool dragged_ = false;
	bool fine_ = false;
	float anchorY_ = 0.0f;
	double anchorValue_ = 0.0;
};

/// THE LAYER TABS: both choices on the face ("Layer 1", "Layer 2"), the shown one lit. An editor flag, not a parameter.
class LayerTabs : public visage::Frame {
public:
	explicit LayerTabs(std::function<void(size_t)> choose);
	void draw(visage::Canvas& canvas) override;
	void setShown(size_t layer);
	[[nodiscard]] size_t shown() const noexcept { return shown_; }
	/// Which tab a point falls in, or -1.
	[[nodiscard]] int tabAt(visage::Point p) const;
	void clickForTest(size_t layer) { choose_(layer); }
	static constexpr float kTabWidth = 88.0f;
	static constexpr float kHeight = 24.0f;

private:
	std::function<void(size_t)> choose_;
	size_t shown_ = 0;
};

class DrumPanel : public ExpandedView {
public:
	static constexpr float kMinimumWidth = 642.0f; // ONE ROW OF EIGHT PADS: 2·12 + 8·72 + 7·6, locked
	static constexpr float kMinimumHeight = 704.0f; // the picture, the pad row, the pad's controls, the layer tabs and a tab's body

	/// `previewWatchers` (optional) is who the editor tells when a pad's Solo moves, so the sound cards' S marks follow it.
	DrumPanel(ParamBridge& bridge, WidgetRegistry& registry, SoundSelection& selection, Notifier* notifier,
	          FrameWatchers* previewWatchers = nullptr);
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
	/// THE LAYER TAB SHOWN (an editor flag): 0 is layer one (the pad's own block), 1 layer two. Pitched mode has layer one alone.
	[[nodiscard]] size_t layerShownForTest() const noexcept { return layerShown_; }
	[[nodiscard]] float contentBottomForTest() const noexcept { return contentBottom_; }
	void showLayerForTest(size_t layer) { chooseLayer(layer); }
	[[nodiscard]] LayerTabs& layerTabsForTest() const { return *tabs_; }
	/// TEST ONLY: a pad's box, segmented control or pill by DrumControl, `layer`'s own copy where the control has one (a pad-level
	/// control ignores `layer`); null for a control drawn as a handle on the picture (Tune, Decay, Sweep Time, Drive, Level:
	/// `handleShownForTest`) or one on the Explore surround (the kit trims).
	[[nodiscard]] visage::Frame* controlForTest(size_t pad, DrumControl c, size_t layer = 0) const;
	/// Whether pad `pad`'s control `c` of layer `layer` is drawn and live, wherever it lives (a box, a pill, or a handle on the picture).
	[[nodiscard]] bool drawnForTest(size_t pad, DrumControl c, size_t layer = 0) const;
	/// The layer's field scaler pill for field `field` (0 Pitch .. 6 Metal), its terrain pad, its settled-pitch readout.
	/// TEST ONLY: pad `pad`'s Mute and Solo chips.
	[[nodiscard]] SwitchChip& muteChipForTest(size_t pad) const { return *pads_[pad].mute; }
	[[nodiscard]] SwitchChip& soloChipForTest(size_t pad) const { return *pads_[pad].solo; }
	[[nodiscard]] PadPlayButton& playButtonForTest(size_t pad) const { return *pads_[pad].play; }
	[[nodiscard]] visage::Frame* fieldPillForTest(size_t pad, size_t layer, size_t field) const;
	[[nodiscard]] DrumVectorPad& vectorPadForTest(size_t pad, size_t layer) const { return *pads_[pad].layer[layer].vectorPad; }
	[[nodiscard]] PitchReadout& pitchReadoutForTest(size_t pad, size_t layer) const { return *pads_[pad].layer[layer].readout; }
	/// What the readout says for the shown pad's layer, by the engine's own settled-pitch function (the value behind its text).
	[[nodiscard]] drum::SettledPitch settledPitchForTest(size_t pad, size_t layer) const { return settledOf(pad, layer); }
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
	/// ONE LAYER'S CONTROLS (one pad has two): each bound at construction to the layer's own parameters (`drumLayerParam`). Layer one
	/// is the pad's own block (its Kind, Level), layer two the third block with LayerKind and the layer-only rows.
	struct LayerSet {
		/// The picture's handles' controls (it writes them for the shown layer): `level` is the pad's Level on layer one, the layer's
		/// own LayerLevel on layer two.
		std::unique_ptr<ParamControl> tune, sweepTime, decay, level, drive, driveTone;
		std::unique_ptr<ParamControl> wireDecay, clickDecay, bellStart, bellEnd, bellTime, bell2Hz, bell2Time;
		std::unique_ptr<ParamControl> hold, dip, dipTime, clickSweep, clickSweepTime, startPhase;
		std::unique_ptr<ParamControl> spread, clapQ, clapHighPass;
		std::unique_ptr<ParamControl> kindControl, countControl;
		std::unique_ptr<ValueBox> kind, count;
		std::unique_ptr<SegmentedControl> driveCurve, pitchMode;
		/// THE ZONE CONTROLS (drum proposal §14): Velocity (every kind), Wires and Bells (a Snare's).
		std::unique_ptr<ParamControl> velocityZoneControl, wiresControl, bellsControl;
		std::unique_ptr<ZoneCoordCell> velocityZone, wires, bells;
		/// The pills: Tune, Velocity Noise (kinds that read Noise), Width (a Snare's and a Clap's); layer two's Level, Delay and High Pass.
		std::unique_ptr<ParamControl> tuneControl, velocityNoiseControl, widthControl, levelPillControl, delayControl, highPassControl;
		std::unique_ptr<FaderPill> tunePill, velocityNoise, width, levelPill, delay, highPass;
		/// THE SEVEN FIELD SCALERS (Pitch, Length, Sweep, Tone, Noise, Snap, Metal), each drawn only where the kind reads it.
		std::array<std::unique_ptr<ParamControl>, 7> fieldControl;
		std::array<std::unique_ptr<FaderPill>, 7> field;
		/// THE LAYER'S VECTOR: its terrain pad (writes OffsetX and OffsetY), and Lock, an editor flag the kit's bulk edit reads.
		std::unique_ptr<ParamControl> offsetXControl, offsetYControl, lockControl;
		std::unique_ptr<SwitchChip> lock;
		std::unique_ptr<DrumVectorPad> vectorPad;
		DrumVectorContext vectorContext {};
		EditHistory* vectorGroup = nullptr;
		/// THE SETTLED PITCH, and the hand on it.
		std::unique_ptr<SettledPitchControl> settledControl;
		std::unique_ptr<PitchReadout> readout;
	};
	struct PadSet {
		std::unique_ptr<ParamControl> noteControl, chokeControl;
		std::unique_ptr<ValueBox> note, choke;
		std::unique_ptr<SegmentedControl> voices, trigger;
		/// The pills: Pan, Release (a Gate pad's), Bend (Kit mode alone; decision 8). The pad's Level is on its sound card and is the
		/// Level handle on layer one's picture.
		std::unique_ptr<ParamControl> panControl, releaseControl, bendControl, muteControl;
		std::unique_ptr<FaderPill> pan, release, bend;
		/// MUTE (the pad's own Mute) and SOLO (the preview of this pad alone), as chips on the pad row.
		std::unique_ptr<SwitchChip> mute, solo;
		std::unique_ptr<PadPlayButton> play;
		std::unique_ptr<PadSoloControl> soloControl;
		std::array<LayerSet, 2> layer {};
	};

	bool kitMode() const;
	/// The shown pad: the selection in Kit mode, pad A in Pitched.
	[[nodiscard]] size_t shownPad() const;
	/// Layer `layer`'s kind of pad `pad`; nullopt for a layer two that is Off.
	std::optional<drum::Kind> layerKindOf(size_t pad, size_t layer) const;
	drum::Kind kindOf(size_t pad) const;
	/// Show the selected pad's set, honestly, and hide every other; lay the shown controls out. Answers whether anything moved.
	bool sync();
	void chooseLayer(size_t layer);
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
	/// The character the synth builds from the parameters now (WaveDisplay's road: modulated values, the played travel, the planes,
	/// residue, gamma and the likes' warp), and the bank's pads from it: `pads` are layer ones', `layers` layer twos'.
	[[nodiscard]] Character characterNow() const;
	/// The pad's fields where it really stands, layer `layer`'s, by the bank's own road.
	drum::Fields fieldsFrom(size_t pad, size_t layer, const DrumCharacter& built) const;
	drum::Laws lawsFrom(size_t pad, size_t layer, const DrumCharacter& built) const;
	/// The layer's controls as the voice resolves them (Synth::fillDrumKit's `fillControls`), and its Tune in semitones.
	drum::Controls controlsOf(size_t pad, size_t layer) const;
	/// What the layer settles on, by `drum::settledPitch` over the same fields, controls and resting pitch the voice plays.
	drum::SettledPitch settledOf(size_t pad, size_t layer) const;
	drum::SettledPitch settledFrom(size_t pad, size_t layer, const DrumCharacter& built) const;
	void refreshVectorContext(size_t pad, size_t layer, const Character& character);

	ParamBridge& bridge_;
	FrameWatchers* previewWatchers_ = nullptr;
	std::optional<size_t> soloSeen_; ///< whose Solo the chips last drew (the preview moves from the cards too)
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
	std::unique_ptr<LayerTabs> tabs_;
	HitPicture picture_;
	/// The terrain pads' "rotate": no turn (an unbound control reads 0).
	std::unique_ptr<ParamControl> noRotate_;
	/// Every control built, for the modulation state to be handed to.
	std::vector<ParamControl*> allControls_;
	/// The likes' warp per sound as the synth attaches them (WaveDisplay's), so the picture is of what plays.
	mutable std::array<LikeWarp, kMaxSounds> likeTable_ {};

	size_t layerShown_ = 0;
	/// Where the shown tab's content ends (the last control's bottom), for the panel's draw and the layout's own tests.
	float contentBottom_ = 0.0f;
	size_t shownPad_ = kMaxSounds;
	size_t shownLayer_ = 2;
	bool shownKit_ = false;
	drum::Kind shownKind_ = drum::Kind::kCount;
	bool shownLayerOn_ = false;
	bool shownSettled_ = false;
	bool shownGate_ = false;
	bool padsShown_ = false;
};

} // namespace feathers::ui
