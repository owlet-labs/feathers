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

#include "backplane/device_host.h"
#include "mod/arp/pattern.h"
#include "mod/arp/pattern_device.h"
#include "mod/arp/pattern_likes.h"
#include "mod/arp/pattern_midi.h"
#include "mod/arp/pattern_types.h"
#include "plugin/ui/device_control.h"
#include "plugin/ui/fader_pill.h"
#include "plugin/ui/expanded_overlay.h"
#include "plugin/ui/file_dialog.h"
#include "plugin/ui/source_arm.h"
#include "plugin/ui/mode_value_cell.h"
#include "plugin/ui/gamma_box.h"
#include "plugin/ui/paged_controls.h"
#include "plugin/ui/pattern_card.h"
#include "plugin/ui/tooltip.h"

#include <visage/ui.h>

#include <array>
#include <chrono>
#include <filesystem>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// THE PATTERN DEVICE'S WINDOW: the tabs, the focused pattern's TERRAIN
// and its lane above `GenericExpanded`'s grid of every control, so the window ⊇ the card at every focus by construction.
//
//     [ 1  E S ][ 2  E S ][ 3  E S ][ 4  E S ]
//     ┌──────────────────────┐ ┌───────────────────────────────┐
//     │   the terrain        │ │  the focused pattern's lane   │
//     │   (its place: puck)  │ └───────────────────────────────┘
//     └──────────────────────┘
//     every control, every pattern (the grid)

namespace feathers::ui {

/// **WHAT A PATTERN IS, IN FOUR NUMBERS** (design §14) — a reduction of the decode the device plays, per Mode family:
/// two palette axes, a glow and a grain. A pure function of the cycle.
struct PatternFeatures {
	float axisA = 0.0f; ///< Melody: register; Chords: harmonic rhythm; TB: the octave-up share
	float axisB = 0.0f; ///< density: notes (or hits) per step
	float glow = 0.0f;  ///< Melody: motion (mean interval); Chords: tension (tones); TB: accent + slide share
};
[[nodiscard]] PatternFeatures featuresOf(const mod::arp::pattern::PatternCycle& cycle, mod::arp::genes::Mode mode) noexcept;
/// **THE 2-D PALETTE, ONE PER MODE FAMILY** (design §14, the Feathers map's shape): the two axes pick a colour between
/// four corners — cool for the line modes, warm for Chords, acid for TB — and the glow lifts it. 0xRRGGBB.
[[nodiscard]] uint32_t paletteOf(mod::arp::genes::Mode mode, const PatternFeatures& f) noexcept;

/// **THE PATTERN TERRAIN** — the focused pattern's plane around its place: each cell is the pattern decoded THERE
/// (`PatternDevice::placeOf` at the cell's place, `pattern::build`) and painted by `paletteOf`. The puck is the place;
/// a press or drag moves it (Place X/Y, one gesture); the wheel zooms the view. The map is rebuilt only when what it was
/// built from moved (its key: the focused pattern's controls and the zoom), never per paint.
class PatternTerrain final : public visage::Frame, public TooltipSource {
public:
	static constexpr size_t kWide = 32;
	static constexpr size_t kHigh = 24;
	/// The plane's half-extent at zoom 1, in travel units.
	static constexpr double kHalfSpan = 0.5;

	PatternTerrain(backplane::DeviceHost& host, DeviceInstanceId id, ModState* mod, EditHistory* history);
	void setFocus(size_t pattern);
	void setZoom(double zoom);
	[[nodiscard]] double zoom() const { return zoom_; }
	/// Rebuilds the map if its key moved; true when it did.
	bool poll();
	void draw(visage::Canvas& canvas) override;
	bool mouseWheel(const visage::MouseEvent& e) override;
	const char* tooltipAt(visage::Point) const override {
		return "The pattern terrain: each place is a pattern. Drag the puck to move this pattern there; the wheel zooms";
	}
	void offeredControls(std::vector<Offered>& out) const override;
	/// **THE PUCK IS PLACE X AND Y'S FACE FOR MODULATION** (the puck glows when assignable and shows a route
	/// ring once routed). Armed and routable, a press ON the puck routes instead of moving it: horizontal travel sets X's
	/// amount, vertical Y's, each axis routed only once the drag moves along it; a double-click unroutes both; a press off
	/// the puck, or with Alt, moves it as ever.
	[[nodiscard]] ModLook modLookFor(const device::Modulatable& c) const override;
	/// Hands the map its modulation state (the window's `modulationBound`): the puck's controls are rebuilt over it.
	void setModulation(ModState* mod, EditHistory* history);
	/// Where the puck is drawn, in this frame's pixels; a press within `kPuckReach` of it is on it.
	[[nodiscard]] visage::Point puckPosition() const;
	static constexpr float kPuckReach = 10.0f;
	static constexpr float kRouteDragRange = 70.0f;
	static constexpr float kRouteSeedAmount = 0.4f;
	static constexpr float kRouteDeadZone = 3.0f;
	[[nodiscard]] DeviceControl& placeXForTest() const { return *placeX_; }
	[[nodiscard]] DeviceControl& placeYForTest() const { return *placeY_; }

	/// The place a unit point of the map) (x right, y up, 0..1) stands for.
	[[nodiscard]] std::array<double, 2> placeAt(double ux, double uy) const;
	/// Press-and-drag to unit point (ux, uy): writes Place X/Y.
	void moveTo(double ux, double uy);
	/// **THE EDGE PAN** — `edge_pan`'s law, the ExplorePad's: a
	/// DRAGGING hand at or past an edge scrolls the view on time, and the puck stays under the hand, so the place walks.
	/// One frame of it, `seconds` long (what `poll` does every frame while a hand is on the map); true if the view moved.
	bool edgeScroll(double seconds);
	/// The view's centre, in travel units (where the map is drawn about).
	[[nodiscard]] std::array<double, 2> viewCentreForTest() const { return { centreX_, centreY_ }; }
	/// **LIKE WHAT PLAYS HERE** (design §14-bis): the focused pattern's place and the fingerprint of its cycle there,
	/// appended to its slot's likes (the oldest gives way past `likes::kMaxLikes`), one block write.
	bool like();
	/// The focused slot's likes as the block holds them.
	[[nodiscard]] mod::arp::likes::SlotLikes likesNow() const;
	/// The focused pattern's slice of the hand's block (its banked turns, its pin switches).
	[[nodiscard]] mod::arp::pat::HandSlot handNow() const;
	/// "More like this" at a unit point of the map: the score of the pattern decoded there.
	[[nodiscard]] double scoreAt(double ux, double uy) const;
	[[nodiscard]] uint32_t cellForTest(size_t x, size_t y) const { return cells_[y * kWide + x]; }
	/// The colour a cell has before the likes brighten it.
	[[nodiscard]] uint32_t baseForTest(size_t x, size_t y) const { return base_[y * kWide + x]; }
	/// **THE LIKES' MARKS** (design §14-bis): each of the focused slot's likes at its stored point projected onto the plane
	/// drawn now (`pat::markOn`), in travel units — so a turn of Rotate, a gamma or a zone moves a mark to where that
	/// pattern now is nearest, and a like whose projection is off the map is not drawn.
	[[nodiscard]] std::vector<mod::arp::pat::PlaneMark> likeMarks() const;
	/// How many hearts the last paint drew.
	[[nodiscard]] size_t likeMarksDrawnForTest() const { return likeMarksDrawn_; }
	[[nodiscard]] size_t buildsForTest() const { return builds_; }
	[[nodiscard]] double lastBuildMsForTest() const { return lastBuildMs_; }

private:
	/// THE ROW AS THE CONTROLS HOLD IT, in a member buffer refilled in place (valid until the next call).
	[[nodiscard]] const std::vector<float>& rowNow() const;
	/// `recentre`: about the place (every rebuild a key change asks for); false keeps the view where a pan put it.
	void build(const std::vector<float>& row, bool recentre = true);
	/// The fingerprint of the focused pattern decoded at `place` (16 steps over a held C4: the picture's own road).
	[[nodiscard]] mod::arp::likes::Fingerprint fingerprintAt(const mod::arp::pat::Settings& s, double x, double y) const;

	backplane::DeviceHost& host_;
	DeviceInstanceId device_ = kNoInstance;
	ModState* mod_ = nullptr;
	EditHistory* history_ = nullptr;
	size_t focus_ = 0;
	double zoom_ = 1.0;
	std::vector<float> key_;
	mutable std::vector<float> rowBuf_; ///< `rowNow`'s buffer
	std::vector<float> keyBuf_;         ///< `poll`'s working key, kept so a tick at rest allocates nothing
	std::vector<float> likedBuf_;       ///< and its likes-and-hand key
	double keyZoom_ = -1.0;
	size_t keyFocus_ = mod::arp::pat::kPatterns;
	std::array<uint32_t, kWide * kHigh> cells_ {};
	std::array<uint32_t, kWide * kHigh> base_ {};
	std::vector<float> likesKey_;
	double centreX_ = 0.0;
	double centreY_ = 0.0;
	size_t builds_ = 0;
	double lastBuildMs_ = 0.0;
	std::unique_ptr<DeviceControl> placeX_;
	std::unique_ptr<DeviceControl> placeY_;
	bool dragging_ = false;
	/// The hand's last position on the map, and whether it has moved since the press (only a DRAG pans).
	visage::Point pointer_ {};
	bool dragged_ = false;
	// THE ROUTE GESTURE (`modLookFor`): which axes may route, which have, and where each started.
	bool routing_ = false;
	visage::Point routeFrom_ {};
	bool routeMayX_ = false;
	bool routeMayY_ = false;
	bool routeX_ = false;
	bool routeY_ = false;
	float anchorX_ = 0.0f;
	float anchorY_ = 0.0f;
	Watch watch_;
	const ModState* watched_ = nullptr;
	void refreshWatch();
	std::chrono::steady_clock::time_point edgeSteppedAt_ {};
	/// Writes Place X/Y for the pointer, held `edge_pan::kStarInsetPixels` inside the map.
	void moveToPointer();
	std::array<double, 2> puckKey_ {};
	size_t likeMarksDrawn_ = 0;
};

/// **THE HEART** — likes what the focused pattern plays at its place (design §14-bis). A press, not a control.
class LikeButton final : public visage::Frame, public TooltipSource {
public:
	LikeButton();
	std::function<void()> onLike;
	void draw(visage::Canvas& canvas) override;
	const char* tooltipAt(visage::Point) const override {
		return "Like this pattern: remember its place and how it sounds, so the terrain shows more like it";
	}
	void offeredControls(std::vector<Offered>&) const override {}
};

/// **A PIN'S SWITCH** (design §4): the On/Off half of a pin's `FaderPill`. Not a row control — the switches live in the
/// hand's block (`pattern_hand`), because Off and amount 0 are the same sound and the switch only remembers an amount —
/// so this reads and writes that block, one whole write per press.
class PinSwitch final : public device::Modulatable {
public:
	PinSwitch(backplane::DeviceHost& host, DeviceInstanceId id, size_t pattern, size_t pin);
	const device::ParamDesc& desc() const override;
	double value() const override;
	void setValue(double value) override;
	/// THE SWITCH AS IT STANDS, base and modulated alike (it is no destination). Empty, it read as 0 — and a pill draws its
	/// power from `modulated`, so every pin looked switched off while the engine played it on (the field
	/// scaler sliders all looked inactive even when they were not).
	device::ModDisplay display() const override {
		device::ModDisplay d;
		d.base = value();
		d.modulated = d.base;
		d.modulatedR = d.base;
		return d;
	}
	void setAmount(float) override {}
	void beginGesture() override {}
	void endGesture() override {}
	std::string text() const override { return value() >= 0.5 ? "On" : "Off"; }

private:
	backplane::DeviceHost& host_;
	DeviceInstanceId device_;
	size_t pattern_;
	size_t pin_;
};

/// **THE PATTERN'S WINDOW — COMPOSED AROUND ITS PICTURES**: compact controls (no single toggle holding a whole cell,
/// slider banks where knobs would sprawl), and Rotate and the other terrain controls close to the terrain, even lying in
/// it as in the other effects. Not
/// GenericExpanded's grid: the house's composed windows (the Disperser's, the Phaser's, the Shaper's) are the pattern.
///
///   - THE TAB STRIP, every tab's E and S on it; under it THE FOCUSED PATTERN'S PANEL:
///       - the TERRAIN, its own controls lying in it as the Shaper's and the Phaser's do — Rotate in its top-right corner
///         (a `GammaBox`, their seating), the heart top-left, and along its foot Zone, Offset, Gamma (a `GammaBox`),
///         Radius and Theta; the wheel zooms, and the zoom is read out in the corner;
///       - beside it the LANE, and under the lane the pattern's own: its choices as compact boxes (the honesty rule
///         hides what its Mode does not read), its switch as a chip, its continuous controls as a SLIDER BANK of
///         FaderPills (Rate, Length, Gate, Accent, Glide);
///       - under both, the PINS' bank.
///   - THE DEVICE'S OWN, under the panel: the selector (a knob, which reads better), Humanize as SKLITTER's zone cell
///     (`ModeValueCell`), the switches as one chip row, the rest as boxes and a slider bank.
/// Every control the card offers is here at every focus (`a device's window offers every control its card offers`).
/// **ONE SETTING OF THE ACTIVE STEP, AS A CONTROL** (the active step's settings sit near the sequence view): the
/// house widgets (a chip, a FaderPill, a box) draw it, and every write goes through the lane
/// into the hand's edits (`pattern.edits.v3`) — one undo step a gesture. Not a device control: the step is the hand's.
class StepControl final : public device::Modulatable {
public:
	enum class What { Note, Pitch, Velocity, Gate, Accent, Slide, Tie, Hit, HitAccent, CellVelocity, CellValue, kCount };
	StepControl(PatternLane& lane, What what) : lane_(lane), what_(what) {}
	const device::ParamDesc& desc() const override;
	double value() const override;
	void setValue(double value) override;
	device::ModDisplay display() const override {
		device::ModDisplay d;
		d.base = value();
		d.modulated = d.base;
		d.modulatedR = d.base;
		return d;
	}
	void setAmount(float) override {}
	void beginGesture() override { lane_.beginStepEdit(); }
	void endGesture() override { lane_.endStepEdit(); }
	std::string text() const override;

private:
	PatternLane& lane_;
	What what_;
};

/// **THE STEP INSPECTOR** — beside the window's lane: the selected step's settings. A pitched step: Note (on/off: a rest
/// made a note, a note made a rest), Tie, and while it is a note its Pitch (in the scale), Velocity, Gate, Accent and Slide;
/// a Drum cell: its Hit and Accent. With nothing selected it says how to select.
class StepInspector final : public visage::Frame, public TooltipSource {
public:
	static constexpr float kHeight = 50.0f;
	static constexpr const char* kHint = "Click a step to set it here; double-click to add or delete a note";
	explicit StepInspector(PatternLane& lane);
	/// Re-reads the selection: which widgets show, and what they read.
	void refresh();
	void resized() override;
	void draw(visage::Canvas& canvas) override;
	const char* tooltipAt(visage::Point) const override {
		return "The selected step's settings: they edit that step alone, and clear with the hand's other edits";
	}
	void offeredControls(std::vector<Offered>&) const override {}
	[[nodiscard]] StepControl& controlForTest(StepControl::What w) const { return *controls_[static_cast<size_t>(w)]; }
	/// The widget drawing `w`, and whether it is shown now.
	[[nodiscard]] visage::Frame* widgetForTest(StepControl::What w) const { return widgets_[static_cast<size_t>(w)]; }
	[[nodiscard]] std::string labelForTest() const { return label_; }

private:
	void layOut();
	PatternLane& lane_;
	std::array<std::unique_ptr<StepControl>, static_cast<size_t>(StepControl::What::kCount)> controls_ {};
	std::array<visage::Frame*, static_cast<size_t>(StepControl::What::kCount)> widgets_ {};
	std::vector<std::unique_ptr<visage::Frame>> owned_;
	std::string label_;
};

/// **THE PATCH AS THE MIDI EXPORT READS IT** (design §7.9): every field and the hand's, the edits' and the play blocks.
[[nodiscard]] mod::arp::pat::MidiSource midiSourceOf(const backplane::DeviceHost& host, DeviceInstanceId id);
/// The song's tempo for the export: the host's, or 120 when it has none.
[[nodiscard]] double midiTempoOf(const backplane::DeviceHost& host);

/// **THE MIDI GRIP** (drag a pattern's MIDI off its tab and into the DAW): on each tab;
/// a press starts an OUTGOING FILE DRAG of that pattern as a .mid (visage's drag source: XDND, NSDraggingSession, OLE),
/// the file written at the press into `directory()` — inside the DAW's own sandbox, since the plugin runs in it.
class MidiGrip final : public visage::Frame, public TooltipSource {
public:
	MidiGrip(backplane::DeviceHost& host, DeviceInstanceId id, size_t pattern);
	bool isDragDropSource() override { return true; }
	std::string startDragDropSource() override;
	void cleanupDragDropSource() override {}
	void draw(visage::Canvas& canvas) override;
	const char* tooltipAt(visage::Point) const override {
		return "Drag this pattern into your DAW as a MIDI clip (a bar-aligned loop at the song's tempo)";
	}
	void offeredControls(std::vector<Offered>&) const override {}
	/// Where the files go: on macOS ~/Library/Caches/Feathers/MIDI (the platform's cache); elsewhere
	/// $XDG_CACHE_HOME/feathers/midi, else ~/.cache/feathers/midi. The temp dir as a last resort on either.
	[[nodiscard]] static std::filesystem::path directory();
	/// `directory()`'s rule as a pure function of what it reads: `XDG_CACHE_HOME`, `HOME` (either may be null) and
	/// whether this is macOS. So the macOS answer can be asked on any host.
	[[nodiscard]] static std::filesystem::path directoryFor(const char* xdgCache, const char* home, bool apple);
	[[nodiscard]] const std::string& lastPathForTest() const { return lastPath_; }

private:
	backplane::DeviceHost& host_;
	DeviceInstanceId device_;
	size_t pattern_;
	std::string lastPath_;
};

class PatternExpanded final : public ExpandedView, public PagedControls {
public:
	PatternExpanded(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id, Notifier* notifier = nullptr);
	~PatternExpanded() override;

	void resized() override;
	void draw(visage::Canvas& canvas) override;
	[[nodiscard]] visage::Point minimumSize() const override;
	[[nodiscard]] std::string title() const override;
	[[nodiscard]] bool updateContent() override;
	void setFocus(size_t pattern);
	[[nodiscard]] size_t focus() const { return focus_; }
	[[nodiscard]] PatternTerrain& terrain() const { return *terrain_; }
	[[nodiscard]] PatternLane& lane() const { return *lane_; }
	[[nodiscard]] StepInspector& inspector() const { return *inspector_; }
	//-- PagedControls: the tabs ------------------------------------------------------------------------------------//
	[[nodiscard]] size_t pageCount() const override { return mod::arp::pat::kPatterns; }
	void showPage(size_t page) override { setFocus(page); }

	static constexpr float kPad = 12.0f;
	static constexpr float kGap = 6.0f;
	/// The rows the sources' strip is given at the minimum width (43 handles: design §7.10).
	static constexpr float kSourceRows = 6.0f;
	static constexpr float kTabsHeight = 24.0f;
	static constexpr float kTerrainWidth = 380.0f;
	static constexpr float kTerrainHeight = 280.0f;
	static constexpr float kLaneHeight = 110.0f;
	static constexpr float kPillHeight = 26.0f;
	static constexpr float kPillGap = 4.0f;
	static constexpr size_t kPillsPerRow = 4;
	static constexpr float kOverlayInset = 6.0f;
	static constexpr float kOverlayBoxWidth = 74.0f;
	static constexpr float kRotateWidth = 110.0f;
	/// THE FOCUSED PATTERN'S PINS (design §4), the ones its Mode reads shown.
	[[nodiscard]] const std::vector<std::unique_ptr<FaderPill>>& pills() const { return pills_; }
	/// The window's handle for `output` (null until a modulation state is bound).
	[[nodiscard]] const SourceArm* armForTest(size_t output) const { return output < arms_.size() ? arms_[output].get() : nullptr; }
	/// Drum's (and Modulation's) lane chip `l` — its caption is the lane's name in the focused tab's Mode.
	[[nodiscard]] const SwitchChip* drumChipForTest(size_t l) const { return l < drumChips_.size() ? drumChips_[l].get() : nullptr; }
	/// The widget seated for a device control (null: none here — the enable, which is the header's, or Place X/Y, which
	/// are the puck), and whether it is shown now.
	[[nodiscard]] visage::Frame* widgetForTest(size_t param) const;
	/// Tab `p`'s cycle cell.
	[[nodiscard]] const ValueBox& cycleRepeatsForTest(size_t p) const { return *cycleRepeats_[p]; }
	[[nodiscard]] const ValueBox& cycleNextForTest(size_t p) const { return *cycleNext_[p]; }
	[[nodiscard]] const ValueBox& cycleSpanForTest(size_t p) const { return *cycleSpan_[p]; }
	[[nodiscard]] SwitchChip& fillChipForTest(size_t p) const { return *fillChips_[p]; }
	[[nodiscard]] SwitchChip& goChipForTest(size_t p) const { return *goChips_[p]; }
	[[nodiscard]] MidiGrip& midiGripForTest(size_t p) const { return *midiGrips_[p]; }
	/// **EXPORT MIDI…** (the tab menu's item): asks where, then writes the same bytes the grip drags. True if a dialog opened.
	bool exportMidi(size_t pattern);
	[[nodiscard]] FileDialog& exportDialogForTest() { return exportDialog_; }
	[[nodiscard]] const PatternTabs& tabsForTest() const { return *tabs_; }
	[[nodiscard]] const Knob& selectorForTest() const { return *selector_; }
	/// THE CYCLE ROW'S HEIGHT in the strip, when it is drawn.
	static constexpr float kCycleRow = ValueBox::kCompactHeight + 6.0f;

protected:
	void modulationBound(ModState* mod, EditHistory* history) override;

private:
	/// ONE SEATED CONTROL: a device param and the widget that drives it.
	struct Seat {
		size_t param = 0;
		visage::Frame* widget = nullptr;
		float height = 0.0f;
	};
	[[nodiscard]] DeviceControl& control(size_t index) const;
	[[nodiscard]] bool live(size_t index) const;
	void buildPatternWidgets();
	void buildPills();
	void layOut();
	/// THE HONESTY RULE'S STATE (every control's gate, and the focused Mode): a change re-lays the window.
	[[nodiscard]] std::vector<uint8_t> liveSet() const;
	/// Lays `seats` (the live ones) left to right in rows of `cell`-wide cells from (x, y) across `w` — stretched to fill
	/// each row, or `cell` exactly when `stretch` is false; returns the y below them.
	float flow(std::vector<Seat>& seats, float x, float y, float w, float cell, bool stretch = true);
	void refreshReadouts();

	backplane::DeviceHost& host_;
	DeviceInstanceId device_;
	Notifier* notifier_ = nullptr;
	ModState* mod_ = nullptr;
	EditHistory* history_ = nullptr;
	mutable std::vector<std::unique_ptr<DeviceControl>> controls_;
	size_t focus_ = 0;
	std::unique_ptr<PatternTabs> tabs_;
	std::array<std::unique_ptr<SwitchChip>, 2 * mod::arp::pat::kPatterns> chips_ {};
	/// **EVERY TAB'S CYCLE CELL, ON ITS TAB** (a next-action and repeats cell in each tab): its
	/// Repeats and its Next, in a second row of the strip, drawn in Cycle alone (the honesty rule).
	std::array<std::unique_ptr<ValueBox>, mod::arp::pat::kPatterns> cycleRepeats_ {};
	std::array<std::unique_ptr<ValueBox>, mod::arp::pat::kPatterns> cycleNext_ {};
	std::array<std::unique_ptr<ValueBox>, mod::arp::pat::kPatterns> cycleSpan_ {};
	/// **EACH TAB'S FILL SWITCH AND ITS TRIGGER** (design §7.8), on the tab beside E and S: F makes the tab a fill; Go (a
	/// momentary, shown on a fill) fires it.
	std::array<std::unique_ptr<SwitchChip>, mod::arp::pat::kPatterns> fillChips_ {};
	std::array<std::unique_ptr<SwitchChip>, mod::arp::pat::kPatterns> goChips_ {};
	/// EACH TAB'S MIDI GRIP (design §7.9), and the Export MIDI… dialog the tab menu opens.
	std::array<std::unique_ptr<MidiGrip>, mod::arp::pat::kPatterns> midiGrips_ {};
	FileDialog exportDialog_;
	std::unique_ptr<PatternTerrain> terrain_;
	std::unique_ptr<PatternLane> lane_;
	std::unique_ptr<StepInspector> inspector_;
	std::unique_ptr<LikeButton> heart_;
	// THE TERRAIN'S OWN, lying in it — rebuilt for the focused pattern.
	std::unique_ptr<GammaBox> rotate_;
	std::unique_ptr<ValueBox> zone_;
	std::unique_ptr<ValueBox> zoneOffset_;
	std::unique_ptr<GammaBox> gamma_;
	// THE FOCUSED PATTERN'S OWN, beside the terrain.
	std::vector<std::unique_ptr<visage::Frame>> patternWidgets_;
	std::vector<Seat> patternBoxes_;
	std::vector<Seat> patternChips_;
	std::vector<Seat> patternBank_;
	// DRUM'S LANE BANK (shown in Drum alone): a chip and a note box a lane.
	std::vector<std::unique_ptr<SwitchChip>> drumChips_;
	std::vector<std::unique_ptr<ValueBox>> drumNotes_;
	void buildDrumBank();
	// THE PINS' BANK.
	std::vector<std::unique_ptr<DeviceControl>> pinAmounts_;
	std::vector<std::unique_ptr<PinSwitch>> pinSwitches_;
	std::vector<std::unique_ptr<FaderPill>> pills_;
	/// **EVERY SOURCE'S HANDLE** (design §7.10: the card shows the open tab's, the window all 43), by output index — null
	/// until a modulation state is bound (`ArpExpanded`'s rule), laid by the walk every host uses.
	std::array<std::unique_ptr<SourceArm>, std::size(mod::arp::pat::kOutputs)> arms_ {};
	visage::Bounds sourceStrip_ {};
	/// The sources the window shows now (`pat::windowOutputsFor`), and what it reads from the row.
	uint64_t shownOutputs_ = ~uint64_t { 0 };
	[[nodiscard]] uint64_t windowShown() const;
	// THE DEVICE'S OWN.
	std::vector<std::unique_ptr<visage::Frame>> globalWidgets_;
	std::unique_ptr<Knob> selector_;
	std::unique_ptr<ModeValueCell> humanize_;
	std::vector<Seat> globalBoxes_;
	std::vector<Seat> globalChips_;
	/// WIDE GLOBALS (Run's segmented row): their own row, cells wide enough for every choice's name.
	std::vector<Seat> globalWide_;
	std::vector<Seat> globalBank_;
	/// The panel the focused tab opens onto, as laid out.
	visage::Bounds panel_ {};
	std::vector<uint8_t> laidLive_;
	size_t laidFocus_ = mod::arp::pat::kPatterns;
};

} // namespace feathers::ui
