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

#include "plugin/gui_bridge.h"
#include "plugin/rotate_sound.h"
#include "plugin/sound_residue.h"
#include "plugin/ui/celestial.h"
#include "plugin/ui/edit_history.h"
#include "plugin/ui/knob.h"
#include "plugin/ui/notify_tray.h"
#include "plugin/ui/sound_identity.h"
#include "plugin/ui/terrain.h"
#include "plugin/ui/ui_state.h"
#include "plugin/ui/widget_registry.h"

#include <embedded/feathers_shaders.h>
#include "visage_graphics/shader.h"

#include <algorithm>
#include <array>
#include <memory>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>
#include <visage/ui.h>

// The mock's terrain view: the selected sound's own character space, and the verbs for walking it.
//
// THE SAME APERTURE AS PERFORM, deliberately — the same disk, the same star glyph, the same dashed
// vocabulary — because the two surfaces are two views of one instrument and a second circle would
// read as a second instrument.
//
// WHAT THE AXES ARE, and this is the whole honesty question. Vertical is HOME, the one coordinate a
// sound's IDENTITY has: zone, offset, gamma and global gamma all address `zone/1023 + offset + gamma`
// and nothing else, which is the v1 finding that deferred this task until the engine had a plane.
// Horizontal is DRIFT along the sound's own slice plane — where the sound has walked out into its own
// plane. `SoundTravel` is patch state, saved, part of what the sound IS (sound_travel.h). So:
//
//   - A SCRUB MOVES HOME up the chart, committing through Zone and Offset, bracketed as one gesture —
//     exactly as the Perform drag commits through Path.
//   - AND IT MOVES DRIFT across it, 1:1 with what is drawn. A breadcrumb, a dice roll and an undo all
//     carry both coordinates.
//
// EXPLORE IS THE IDENTITY SURFACE, AND ONLY THAT. The decision of 2026-08-13: Weather does not move
// things around in Explore. The engine has this sound at `drift + Weather × (puck − anchor)`,
// and the second term is a PERFORMANCE coordinate no identity can store — so it is not on this chart
// at all. Turning Weather up, flying the puck or automating either moves what you HEAR and moves
// nothing you see here; Perform is the surface that owns that half and draws it. The consequence
// worth stating plainly, because it is the one a player will notice: the star can sit still while the
// sound changes. That is the same cut A24 makes for gamma — local is identity, global is the frame —
// and the same one A26 makes for what a rotation banks.
//
// What it buys, beyond the decision itself: nothing this surface draws is a function of any performance
// parameter, so a host automating them cannot repaint it, cannot churn its text, and cannot throw a
// mark at the rim. The chart is a picture of the patch.
//
// §5-table's rule is what decides the rest: a dimension the engine does not have is a dimension the
// surface must not offer. The engine has both axes of identity, so the surface offers both. It did
// not have the second one when this task was deferred, which is the whole reason it was deferred.
//
// THE WORLD MOVES ONLY WHEN YOU MOVE, which is the mock's own phrase for it. The view holds still
// while a scrub is in flight and re-centres on release, rather than following the star frame by
// frame: a terrain that re-centred continuously would rebuild continuously, and at 31 ms a build the
// map would never finish and you would be walking across an empty chart.

namespace feathers::ui {

class ExploreStage;
class SpectrumTab;
class DrumTab;

/// THE GROUND, AND THE ONLY REASON IT IS A FRAME OF ITS OWN: a post effect belongs to a frame and
/// applies to everything that frame paints. The bubble must bend the terrain and NOTHING else — not
/// the sphere's own lighting, which is already in screen coordinates, and certainly not the labels —
/// so the picture needs a surface nobody else paints on.
///
/// AND IT IS A SIBLING BENEATH THE STAGE, not a child of it, because visage composites a frame's own
/// painting FIRST and its children after: a child could only ever be on TOP of the grid and the stars,
/// which is the one place the ground cannot be. Sitting in front of the stage in the app's own child
/// list puts it exactly where it belongs and leaves the stage's painting — every line of it — where it
/// was. It is bounded to the stage's rectangle, so the two share coordinates and neither has to know
/// about the other's geometry.
class TerrainGround : public visage::Frame {
public:
	TerrainGround();

	/// Whose ground this is. Set once, after both frames exist.
	void setStage(ExploreStage* stage) { stage_ = stage; }

	/// The chart's radius as a fraction of this frame's width — the one thing the warp needs that the
	/// shader cannot see. Told by the stage as it paints, because the chart yields room to the width
	/// column and so the share changes with the frame's shape, not only with its size.
	void setBubbleRadiusShare(float share) {
		share_ = share;
		bubble_.setUniformValue("u_bubble", share, 0.0f, 0.0f, 0.0f);
	}



	[[nodiscard]] const visage::ShaderPostEffect& bubbleForTest() const { return bubble_; }
	/// TEST ONLY: what the line above was last handed. Upstream's `ShaderPostEffect` keeps its uniforms
	/// private and our fork's readback is on `Shader`, not on this class — so the chain is observable
	/// up to here and the last hop is one library call. Adding a second patch to visage to see that
	/// call would cost more than it buys.
	[[nodiscard]] float shareForTest() const noexcept { return share_; }

private:
	ExploreStage* stage_ = nullptr;
	float share_ = 0.0f;
	visage::ShaderPostEffect bubble_ { resources::shaders::vs_bubble, resources::shaders::fs_bubble };
};

/// AN OBSERVER OF THE EDITOR'S HISTORY, as well as a surface. The history is shared now — the sound
/// cards record into the same one — so this surface no longer owns the stack it walks. What it still
/// owns is what happens to the VIEW when an entry is walked: the slot is selected and the chart
/// re-centres on it, which is this surface's business and nobody else's. See edit_history.h for why
/// those two halves are split.
class ExploreStage : public visage::Frame, public EditObserver {
public:
	using Clock = std::function<int64_t()>;
	ExploreStage(ParamBridge& bridge, WidgetRegistry& registry, SoundSelection& selection,
	             EditHistory& history, TerrainMap& terrain, Notifier* notifier = nullptr,
	             Clock clock = {});
	~ExploreStage() override;

	void draw(visage::Canvas& canvas) override;

	/// Zoom on the wheel, which the surface did not answer at all before.
	///
	/// WHAT ARRIVES HERE, ON THIS PLATFORM, measured rather than assumed. visage's X11 backend reads
	/// scroll from CORE X11 buttons 4-7 only (windowing_x11.cpp) and converts them to a delta of
	/// exactly ±1; it never opens XInput2, so there are no high-resolution axes and NO GESTURE EVENTS
	/// OF ANY KIND. A touchpad's two-finger scroll still reaches us, because libinput emulates the
	/// legacy scroll buttons for it. A PINCH does not and cannot: libinput delivers pinch only through
	/// XI2 gesture events, and nothing here selects them.
	///
	/// So this handles scroll, and a pinch would need a change in visage's windowing layer rather than
	/// here. `FEATHERS_TRACE_WHEEL` prints what actually arrives, so that is a question the hardware
	/// can answer rather than a thing to argue about.
	bool mouseWheel(const visage::MouseEvent& e) override;

	/// One notch of scroll, as a zoom factor — A QUARTER OF THE MOCK'S `setZoom(S.zoom*1.6)` STEP, 1.6^(1/4) (four
	/// notches make the old one, so a wheel moves the view in small steps).
	static constexpr double kZoomStep = 1.1246826503806981;
	/// **THE TERRAIN WAITS FOR THE ZOOM TO REST.** A zoom scales the picture already on screen at once; the map is
	/// rebuilt at the new zoom only after it has held still this long, so a run of notches is one rebuild rather
	/// than a rebuild per notch — every one of which re-sampled a new grid and popped.
	static constexpr int64_t kZoomDwellMs = 250;
	/// **OR ONCE IT HAS MOVED THIS FAR FROM THE MAP ON SCREEN**, as a ratio either way, resting or not — a large
	/// enough zoom displacement redraws even without a dwell — so a long scroll refreshes
	/// as it goes rather than stretching one map to mush. √2: about three notches, a map at most 1.41× stretched.
	static constexpr double kZoomRebuildRatio = 1.4142135623730951;
	/// The zoom the terrain is built for: `zoom()` once it has rested `kZoomDwellMs`. TEST SEAM and truth for the draw.
	[[nodiscard]] double terrainZoom() const noexcept { return terrainZoom_; }

	/// Advance ambient motion and the terrain fill, and say whether the screen is out of date. Same
	/// contract as PerformStage::update and NotifyTray::update.
	bool update();

	/// THE MAP ITSELF, so an instrument can ask how many rebuilds a gesture actually caused.
	///
	/// A frame-time arm that drags the gamma corner is CLAIMING the terrain is rebuilding, and a
	/// gamma value that moved is not that claim — it is a difference check standing in for a value,
	/// which is the shape CLAUDE.md names. `requests()` and `builds()` are the value: a hold that
	/// leaves both where it found them measured a map sitting still, whatever the corner did.
	const TerrainMap& terrainForTest() const { return terrain_; }
	/// TEST ONLY: what the planet's two shaders were last handed (backlog FE). A claim about a VALUE cannot be read out of
	/// a rendered disk; the shader's own geometry is answered by a photograph instead.
	[[nodiscard]] const visage::Shader& planetShadeForTest() const { return planetShade_; }
	[[nodiscard]] const visage::Shader& planetAirForTest() const { return planetAir_; }

	/// THE GROUND, PAINTED WHEREVER IT IS ASKED FOR: the void the chart sits on and the terrain picture,
	/// and nothing that must stay flat. `TerrainGround` calls it so the bubble can bend it; a surface
	/// with no ground frame — every widget case, which has no frame tree at all — gets it from `draw`,
	/// so what is painted is the same either way and only the layer it lands in differs.
	void drawGround(visage::Canvas& canvas);

	/// Told once, by whoever built both. A stage that has one leaves the ground to it.
	void setGround(TerrainGround* ground) { ground_ = ground; }

	/// TEST ONLY: how many times the ground has been painted. The claim it answers is "exactly one
	/// surface paints the picture" — which cannot be read off a headless canvas, and which matters
	/// because the failure is a FLAT copy of the terrain sitting under the warped one, visible only as
	/// a ghost wherever the warp moved something, i.e. everywhere but the middle.
	[[nodiscard]] int groundPaintsForTest() const noexcept { return groundPaints_; }
	/// TEST ONLY: how many times the stage itself has been painted — backlog NF's "a closed Explore costs nothing":
	/// a repaint is not observable headlessly, so the windowed case counts the paints the hidden stage did not do.
	[[nodiscard]] int stagePaintsForTest() const noexcept { return stagePaints_; }

	/// TEST ONLY: a released drag still coasting to where the hand asked. It is a state a case cannot
	/// otherwise see and has to wait for — the crumb and the undo entry are planted when the star
	/// ARRIVES, not when the button comes up — and one it should assert EXISTS before asserting it
	/// ends, or "the world came to a stop" passes on a build where it never started.
	[[nodiscard]] bool settlingScrubForTest() const noexcept { return settlingScrub_; }

	/// TEST ONLY: the deepest bow, in pixels, of any zone line the last draw put on the glass —
	/// measured against that line's own chord. A straight grid scores zero however busy the chart is,
	/// which is what makes this a value and not a difference against a rendered buffer.
	[[nodiscard]] float gridBowForTest() const noexcept { return gridBow_; }

	/// TEST ONLY: the disc's radius as a share of the frame's width, as last handed to the warp. The
	/// shader's own arithmetic is not reachable from here — nothing headless runs it — so this is the
	/// half of that contract a case CAN hold: get it wrong and the ground is warped about an ellipse
	/// the size of the whole stage, which the rim mask hides so completely that no photograph and no
	/// difference against one can tell. A number is the only instrument left.
	[[nodiscard]] float bubbleRadiusShareForTest() const noexcept { return bubbleRadiusShare_; }

	//----------------//
	// The width knob //
	//----------------//

	/// THE ONE WIDTH KNOB, for the sound being explored — V15: "in explore, we should only have the
	/// width knob for the sound we are exploring." Eight stock Knobs stand behind it, one bound to
	/// each slot's Width parameter at construction, and exactly one is ever visible: rebinding on a
	/// selection change is a visibility flip rather than a rebindable control, so the Knob widget
	/// stays exactly what the tray and the Engine rail already trust. Disabled (V12c honesty idiom)
	/// while the patch is not V5 or the selected sound is parked — in both states the parameter
	/// changes nothing, and the control must say so. NEVER hidden for want of room (V16) — the
	/// chart square yields instead; see disk().
	/// HAND THE FLAGSHIP STACK THE EDITOR'S MODULATION STATE, the same way the tray, the Engine rail and
	/// the filter card are handed it in Gui::create.
	///
	/// THIS SURFACE WAS THE ONE THAT NEVER WAS, which is why the width, FM and smooth controls did not
	/// glow in mod mode: with no ModState a Knob's mod_ is null, and modRing() returns
	/// before it reads anything — no arm glow, no reach arc, and no drag can point a source at them.
	/// Every other surface carrying a Knob has had this since the mod panel landed.
	void setModulation(ModState* state, EditHistory* history);

	Knob& widthKnobForTest(size_t slot) { return *widthKnobs_[std::min(slot, kMaxSounds - 1)]; }
	/// Resonate's surround, in the order its slots are named (see resonateKnobBounds).
	enum class ResonateKnob : uint8_t { Width, Bow, Shimmer, Stiffness, Coupling, Decay, Pluck, Travel, Speed };
	static constexpr size_t kResonateKnobCount = 9;
	Knob& resonateKnobForTest(ResonateKnob which) { return *resonateKnobs_[static_cast<size_t>(which)]; }
	/// The slot a Resonate knob takes: the per-sound knob it replaces on the surround.
	[[nodiscard]] visage::Bounds resonateKnobBounds(ResonateKnob which) const;
	/// BASIC SHAPES' FOUR (2026-10-03), top to bottom on the right flank.
	enum class ShapesKnob : uint8_t { Sine, Triangle, Saw, Square };
	static constexpr size_t kShapesKnobCount = 4;
	Knob& shapesKnobForTest(ShapesKnob which) { return *shapesKnobs_[static_cast<size_t>(which)]; }
	/// The slot a shape knob takes: the right flank's Quantize / Sync / Feedback / Points, the four field
	/// modifiers not yet wired on Basic Shapes.
	[[nodiscard]] visage::Bounds shapesKnobBounds(ShapesKnob which) const;
	/// THE MODE BOX ON RESONATE IS THE BOW MODE (the warp mode everywhere else): live on Resonate, every choice
	/// on its menu before any is applied.
	[[nodiscard]] bool bowModeLive() const;
	[[nodiscard]] const char* bowModeLabel() const;
	void chooseBowMode(size_t index);
	void showBowModeMenu();
	/// THE MODE BOX ON ADDITIVE IS THE ADDITIVE SHAPE (design §4.1: "one knob+a dropdown", Warp's pattern) — the target each
	/// sound's Shape Amount pulls toward. Live on Additive only; every choice on its menu before any is applied.
	[[nodiscard]] bool additiveShapeLive() const;
	[[nodiscard]] const char* additiveShapeLabel() const;
	void chooseAdditiveShape(size_t index);
	void showAdditiveShapeMenu();
	/// THE ADDITIVE SHAPE AMOUNT KNOB, per sound, in Warp's slot (Warp is not drawn on Additive).
	Knob& shapeAmountKnobForTest(size_t slot) { return *shapeAmountKnobs_[std::min(slot, kMaxSounds - 1)]; }
	/// THE SIX ADDITIVE BIAS KNOBS (design §4.1), per sound, shown on Additive only: Rolloff in FM Depth's slot on the
	/// left flank (FM Depth is not drawn on Additive), the other five in the right flank's five field-modifier slots,
	/// top to bottom (none of which Additive draws).
	enum class AdditiveBiasKind : uint8_t { Rolloff, Formant, OddEven, Comb, Detail, OffGrid };
	static constexpr size_t kAdditiveBiasCount = 6;
	[[nodiscard]] visage::Bounds additiveBiasBounds(AdditiveBiasKind kind) const;
	Knob& additiveBiasKnobForTest(AdditiveBiasKind kind, size_t slot) {
		return *additiveBiasKnobs_[static_cast<size_t>(kind)][std::min(slot, kMaxSounds - 1)];
	}
	/// **PLUCK'S SURROUND** (2026-10-31): the string's five field scalers, per sound, on the right flank; and
	/// its engine-wide controls on the left, on two PAGES (the box in the left box slot names the page and offers both before
	/// either applies): the String page (Width, Decay, Release, Smoothness) and the Pluck page (Seed, Decay Track, Hardness,
	/// Pickup). VIEW STATE, like the shapes page. The placement is provisional.
	enum class StringKind : uint8_t { Brightness, Sustain, Pick, Noise, Stretch };
	static constexpr size_t kStringKindCount = 5;
	Knob& stringKnobForTest(StringKind kind, size_t slot) {
		return *stringKnobs_[static_cast<size_t>(kind)][std::min(slot, kMaxSounds - 1)];
	}
	/// **THE DRUM'S SURROUND** (2026-11-05): the seven field scalers of the selected sound (in Kit mode the
	/// selected PAD), each drawn only where its kind reads it (drum::kindReads), and the Kit Walk, drawn in Kit mode alone.
	enum class DrumField : uint8_t { Pitch, Length, Sweep, Tone, Noise, Snap, Metal };
	static constexpr size_t kDrumFieldCount = 7;
	Knob& drumFieldKnobForTest(DrumField f, size_t slot) {
		return *drumFieldKnobs_[static_cast<size_t>(f)][std::min(slot, kMaxSounds - 1)];
	}
	Knob& drumWalkKnobForTest(size_t axis) { return *drumWalkKnobs_[std::min<size_t>(axis, 1)]; }
	/// THE KIT-WIDE LAYER'S TEN KNOBS (2026-11-06), in SurroundControl's order from DrumKitGamma, and the Drum surround's pages:
	/// Pad (each pad's seven scalers and the Walk), Kit Fields (the seven kit trims in the scalers' slots, and the Walk) and Kit
	/// Place (Kit Zone, Offset and Gamma, and the Walk). The pages box stands in the left box slot in Kit mode only.
	static constexpr size_t kDrumKitKnobCount = 10;
	Knob& drumKitKnobForTest(size_t i) { return *drumKitKnobs_[std::min(i, kDrumKitKnobCount - 1)]; }
	enum class DrumPage : uint8_t { Pad, KitFields, KitPlace };
	void chooseDrumPage(DrumPage page);
	[[nodiscard]] DrumPage drumPageForTest() const noexcept { return drumPage_; }
	enum class PluckKnob : uint8_t { Width, Decay, Release, Seed, DecayTrack, Hardness, Pickup, Velocity };
	static constexpr size_t kPluckKnobCount = 8;
	Knob& pluckKnobForTest(PluckKnob which) { return *pluckKnobs_[static_cast<size_t>(which)]; }
	/// AND A THIRD, Velocity (its depth on Hardness).
	enum class PluckPage : uint8_t { String, Pluck, Velocity };
	void choosePluckPage(PluckPage page);
	[[nodiscard]] PluckPage pluckPageForTest() const noexcept { return pluckPage_; }
	/// Whether the page box stands on the face (Pluck only), as the drawing and the hit test ask.
	[[nodiscard]] bool pluckPageLive() const { return slotOf(SurroundControl::KsPage).has_value(); }
	/// Whether the Drum's page box (Pad, Kit Fields, Kit Place) is drawn: a Drum Kit's, in the left box slot.
	[[nodiscard]] bool drumPageLive() const { return slotOf(SurroundControl::DrumPage).has_value(); }

	/// **THE SPECTRUM TAB** (additive design §4.2, spectrum_panel.h) — on Additive only, on the stage's bottom edge under the
	/// right flank (the bottom centre is Mutate's row). Its chip is the master switch; its label asks the editor to open or
	/// close the spectrum PANEL (`onSpectrumTab`), which the editor presents and owns — Explore holds no panel, and is told
	/// whether one is open (`setSpectrumOpen`) only to light the tab's caret.
	std::function<void()> onSpectrumTab;
	[[nodiscard]] bool spectrumOpen() const noexcept { return spectrumOpen_; }
	void setSpectrumOpen(bool open);
	/// THE DRUM TAB (drum proposal §6), Drum's alone, in the spectrum tab's place: a press asks the editor for the kit panel.
	std::function<void()> onDrumTab;
	void setDrumOpen(bool open);
	[[nodiscard]] DrumTab& drumTabForTest() { return *drumTab_; }
	[[nodiscard]] SpectrumTab& spectrumTabForTest() { return *spectrumTab_; }
	[[nodiscard]] visage::Bounds spectrumTabBounds() const;

	/// THE WINDOW THIS VIEW WOULD ASK FOR, exposed so a case can pin WHICH ROAD it is assembled from.
	///
	/// The terrain window is built from BASE values while the scope beside it is built from MODULATED
	/// ones, deliberately and for opposite reasons — see askForTerrain's own note. That distinction
	/// lives in two files and is invisible from either alone, so it needs a case, and a case needs to
	/// be able to ask.
	TerrainWindow askForTerrainForTest() { return askForTerrain(); }
	/// Where the visible knob stands, for tests and the hue chip. Always a real on-stage rectangle
	/// (V16): disk() reserves this column before sizing the chart square.
	visage::Bounds widthKnobBounds() const;

	/// THE PER-SOUND FLAGSHIP KNOBS, stacked vertically in the left column (V16's flank): Width, then
	/// FM Depth, then Smoothness, top to bottom. One VISIBLE trio per selection, eight hidden behind
	/// them (one per slot). Each is a placement into the same left flank strip the width knob already
	/// used, so the chart reserve is unchanged and the square never overlaps them. The stack is centred
	/// on the disk; see knobStackBounds().
	///
	/// WHY A STACK rather than the old single centred width knob: the flagship now has three per-sound
	/// continuous controls (Width, the FM depth of doc §10, the smoothness override of doc §11) and a
	/// vertical column reads as "the flagship's dials" the way the filter card's do, without asking the
	/// chart to yield any more room than the width knob already reserved.
	/// WARP SITS NEXT TO FM DEPTH, 2026-08-28, on the left flank.
	///
	/// AND THE ORDER IS THE GROUPING. FM Depth and Warp are AMOUNTS — `scales.fmDepth` and
	/// `scales.warpDepth` go straight through, with no field behind them and a default of 0.
	/// Smoothness is a BIAS over a phi field, where 0.5 means "as the terrain authored it", which is
	/// what every knob on the right flank is. Putting the two amounts adjacent is what makes the two
	/// stacks read as two ideas rather than as seven knobs split by where there was room.
	enum class FlagshipKind : uint8_t { Width, FmDepth, Warp, Smoothness };
	static constexpr size_t kFlagshipKindCount = 4;
	visage::Bounds flagshipKindBounds(FlagshipKind kind) const;

	//------------------------------//
	// The five field modifier knobs //
	//------------------------------//

	/// THE RIGHT FLANK'S STACK, 2026-08-25 — field modifier knobs on the explore view, as mod
	/// targets. Quantize, Sync, Feedback, Points, Grit, top to bottom, each scaling the terrain field
	/// that authors its quantity through fieldWithOverride.
	///
	/// WHY THE RIGHT STRIP. It was banked at V16 as a general affordance — geometry only, no
	/// speculative widgets — so the space could be used for other things later.
	/// This is that. Putting them here rather than lengthening the left stack keeps the left flank's
	/// V16 geometry and the cases that pin it untouched, and it groups the two tiers a player would
	/// name differently: what this sound plays like on the left, what the terrain is allowed to say
	/// about it on the right.
	///
	/// ONE ARRAY INDEXED BY KIND, unlike the three parallel arrays behind the left stack. That older
	/// shape is not converted here: it works, its accessors are pinned by cases, and rewriting it
	/// would be churn this task does not need — but a new stack of five would be five more members
	/// and five more lines in every loop that touches them, so the new one is the better shape rather
	/// than the matching one.
	/// THE RIGHT FLANK IS THE FIVE FIELD MODIFIERS AND NOTHING ELSE. Warp briefly lived here as a
	/// sixth and moved to the left flank, beside FM Depth, on 2026-08-28, which
	/// is also the better line: every knob here BIASES a phi field, and warp has none to bias.
	enum class FieldKind : uint8_t { Quantize, Sync, Feedback, Points, Grit };
	static constexpr size_t kFieldKindCount = 5;
	/// The dropdown is a short box, not a knob column — it needs a line of text and nothing else.
	///
	/// A FULL KNOB SLOT FOR IT DOES NOT FIT, measured rather than reasoned: when the box took a
	/// column of its own the run overflowed its flank and the top knob landed at y = -28, which
	/// `test_gui_widgets.cpp`'s "each is reachable" case caught at once. It takes the gap under the
	/// Warp knob instead.
	static constexpr float kWarpModeBoxHeight = 20.0f;

	/// THE WARP MODE DROPDOWN — one per ENGINE, not per sound, so there is one of it and no array.
	/// Sits directly under the Warp knob, which is what "a drop down near the knob" asks for.
	[[nodiscard]] visage::Bounds warpModeBounds() const;
	/// What the box currently reads. Exposed because a case asserting the dropdown says what the
	/// engine is doing needs the string, and a render cannot be asked.
	[[nodiscard]] const char* warpModeLabel() const;
	void showWarpModeMenu();
	/// IS THE WARP MODE BOX A LIVE CONTROL RIGHT NOW — V5 and Basic Shapes, exactly like the Warp knob
	/// above it, and asked in one place so the drawing and the hit test cannot disagree about whether a
	/// box nobody can see opens its menu.
	[[nodiscard]] bool warpModeLive() const;
	/// NOT DRAWN AT ALL where neither the warp mode nor the bow mode is live (Morph, Stair) — since
	/// 2026-10-04: only switching the engine could ungrey it. Returns whether it drew, so a case can ask
	/// the draw itself rather than the predicate it should obey.
	bool drawWarpModeBox(visage::Canvas& canvas);
	bool drawWarpModeBoxForTest(visage::Canvas& canvas) { return drawWarpModeBox(canvas); }
	void chooseWarpMode(size_t index);
	/// THE RESONATE MODEL DROPDOWN — what the ONE string is (Ring, Bar, Tube, Membrane, ...), engine-wide like
	/// the warp mode, so one box and no array. Under the right flank's last knob, in a row fieldKindBounds
	/// reserves for it. A ValueBox-style name + chevron that opens every choice before any is applied — the
	/// multivalue-toggle rule — and NOT DRAWN AT ALL off Resonate (a greyed box naming a string
	/// that no voice scans read as part of the other engines' face). Its hit test is gated on the same answer.
	static constexpr float kModelBoxHeight = 20.0f;
	[[nodiscard]] visage::Bounds modelBoxBounds() const;
	[[nodiscard]] const char* modelLabel() const;
	[[nodiscard]] bool modelBoxLive() const;
	void showModelMenu();
	/// Returns whether it drew anything, so a case can ask the draw itself rather than the predicate it should obey.
	bool drawModelBox(visage::Canvas& canvas);
	bool drawModelBoxForTest(visage::Canvas& canvas) { return drawModelBox(canvas); }
	void chooseModel(size_t index);
	/// **BASIC SHAPES' SECOND PAGE (backlog KV).** The right flank holds the shape mix OR the operator knobs (Quantize,
	/// Sync, Feedback, Points), which V5 seats there; Basic Shapes has eleven slots for fourteen controls. A box in the
	/// right box slot names the page and opens both choices before either is applied (the multivalue rule). VIEW STATE,
	/// not a parameter: the mix is the default page. The placement is provisional (the alternative is a ring round the
	/// disk).
	enum class ShapesPage : uint8_t { Mix, Operators };
	void chooseShapesPage(ShapesPage page);
	[[nodiscard]] ShapesPage shapesPageForTest() const noexcept { return shapesPage_; }
	/// Whether the page box stands on the face (Basic Shapes only), as the drawing and the hit test ask.
	[[nodiscard]] bool shapesPageLive() const { return slotOf(SurroundControl::ShapesPage).has_value(); }
	visage::Bounds fieldKindBounds(FieldKind kind) const;
	Knob& fieldKnobForTest(FieldKind kind, size_t slot) {
		return *fieldKnobs_[static_cast<size_t>(kind)][std::min(slot, kMaxSounds - 1)];
	}

	Knob& fmDepthKnobForTest(size_t slot) { return *fmDepthKnobs_[std::min(slot, kMaxSounds - 1)]; }
	Knob& radiusKnobForTest(size_t slot) { return *radiusKnobs_[std::min(slot, kMaxSounds - 1)]; }
	Knob& thetaKnobForTest(size_t slot) { return *thetaKnobs_[std::min(slot, kMaxSounds - 1)]; }
	Knob& warpKnobForTest(size_t slot) { return *warpKnobs_[std::min(slot, kMaxSounds - 1)]; }
	Knob& smoothnessKnobForTest(size_t slot) {
		return *smoothnessKnobs_[std::min(slot, kMaxSounds - 1)];
	}

	/// THE SIDE COLUMNS the chart yields to — V16 settled symmetric, as a GENERAL AFFORDANCE, so
	/// the space can be used for other things later: each flank is
	/// a full-height strip beside the chart square, and a flank control is a placement INTO its
	/// strip rather than a re-derivation of chart geometry. Left holds the width knob; Right is
	/// banked — geometry only, no speculative widgets. When the reserve binds, Left starts at
	/// x = 0 and the square sits one kFlankGap in from each strip.
	enum class Flank : uint8_t { Left, Right };
	visage::Bounds flankColumn(Flank side) const;
	/// ONE KNOB'S RECTANGLE IN A FLANK STACK — the single definition both stacks ask, because two
	/// copies of "column height, gap, centre the run on the disk" is two things that must agree, and
	/// this file has already paid for that shape twice this week in its sibling.
	/// @param insertAfter  index after which `insertHeight` of extra room is reserved, or `count` for
	///                     none. The left stack uses it for the Warp dropdown, which needs real space
	///                     in the run rather than borrowing the gap under its knob.
	visage::Bounds stackSlotBounds(Flank side, size_t index, size_t count, size_t insertAfter,
	                               float insertHeight) const;
	visage::Bounds stackSlotBounds(Flank side, size_t index, size_t count) const {
		return stackSlotBounds(side, index, count, count, 0.0f);
	}

	//----------------//
	// The slot table //
	//----------------//

	/// **THE SURROUND'S SLOTS (backlog LJ)** — the places on the two flanks a control can stand, top to bottom: the left
	/// stack's four knobs with the mode box's row after the third, and the right stack's five with the model box's row
	/// after the last. Their rectangles are `flagshipKindBounds`, `warpModeBounds`, `fieldKindBounds` and
	/// `modelBoxBounds`; a slot is a place, and what stands in it is the table's answer.
	enum class Slot : uint8_t { Left0, Left1, Left2, LeftBox, Left3, Right0, Right1, Right2, Right3, Right4, RightBox, kCount };
	static constexpr size_t kSlotCount = static_cast<size_t>(Slot::kCount);
	/// Every control the surround can seat. The per-sound ones (eight knobs behind each, one per sound) come first, then
	/// the engine-wide knobs (one each), then the boxes, which seat only in a box slot.
	enum class SurroundControl : uint8_t {
		None,
		Width, FmDepth, Warp, Smoothness, Quantize, Sync, Feedback, Points, Grit, ShapeAmount,
		Rolloff, Formant, OddEven, Comb, Detail, OffGrid,
		KsBrightness, KsSustain, KsPick, KsNoise, KsStretch,
		DrumPitch, DrumLength, DrumSweep, DrumTone, DrumNoise, DrumSnap, DrumMetal,
		ResonateWidth, Bow, Shimmer, Stiffness, Coupling, Decay, Pluck, Travel, Speed,
		Sine, Triangle, Saw, Square,
		KsWidth, KsDecay, KsRelease, KsSeed, KsDecayTrack, KsHardness, KsPickup, KsVelocity,
		DrumWalkX, DrumWalkY,
		DrumKitGamma, DrumKitZone, DrumKitOffset, DrumKitPitch, DrumKitLength, DrumKitSweep, DrumKitTone, DrumKitNoise,
		DrumKitSnap, DrumKitMetal,
		WarpMode, BowMode, AdditiveShape, Model, ShapesPage, KsPage, DrumPage,
		kCount
	};
	static constexpr size_t kSurroundControlCount = static_cast<size_t>(SurroundControl::kCount);
	/// **ENGINE × SLOT → CONTROL, OR EMPTY.** Nothing reflows, but a different engine's knob can be placed in a
	/// gap deliberately. Layout, visibility, enablement and the
	/// box hit test all read this one table, so putting a knob in another engine's gap is a one-cell edit with no layout
	/// code, and an empty cell is an empty slot: nothing moves up to fill it.
	using SlotRow = std::array<SurroundControl, kSlotCount>;
	using SlotTable = std::array<SlotRow, static_cast<size_t>(OscType::kCount)>;
	/// Whether `control` stands in a slot of the row the surround draws now (the page and the honesty rule applied).
	[[nodiscard]] bool seatedNowForTest(SurroundControl control) const { return slotOf(control).has_value(); }
	[[nodiscard]] visage::Bounds slotBounds(Slot slot) const;
	[[nodiscard]] const SlotTable& slotTable() const noexcept { return slots_; }
	/// TEST ONLY: seat a different table (it must be valid: a box only in a box slot, a knob only in a knob slot, no
	/// control twice in one engine's row, and every control in some row, which is where it waits while hidden).
	/// The product table is held to the same rules at compile time.
	void setSlotTableForTest(const SlotTable& table);

	//----------//
	// Geometry //
	//----------//

	StageDisk disk() const;
	bool hitsDisk(visage::Point pixels) const;

	/// The chart's own coordinates. Vertical is home and grows UPWARD — drift flies you up over the
	/// landscape, which is the mock's axis convention — and horizontal is travel, right-positive.
	/// THESE FOUR ARE THE FLAT PLANE, and they stayed flat when the bubble arrived. A radial map is not
	/// separable — where a home lands across the glass depends on its travel and the other way about —
	/// so an axis-at-a-time answer cannot describe the drawn surface, and pretending otherwise would
	/// have put a plausible wrong number in twenty call sites. What they describe is the SHEET: the
	/// terrain's own window, the map builder's columns and rows, and the arithmetic that decides what
	/// country is in view. Everything that puts something ON THE GLASS, or reads something off it, goes
	/// through the pair below.
	double travelYAtScreenY(float y) const;
	float yAtTravelY(double home) const;
	double travelXAtScreenX(float x) const;
	float xAtTravelX(double travel) const;

	/// Where the bubble draws a place, and what place is drawn under a pixel. The projection in one
	/// pair: `Bubble` is the law, these two are the chart's own coordinates carried through it.
	visage::Point glassAt(double travelY, double travelX) const;
	visage::Point flatAt(visage::Point glass) const;

	/// **THE LIKES' WARP — VIEW ONLY** (backlog ON). The chart draws the WARPED world: a liked region is given room. These
	/// carry a REAL place (the sound's, a crumb's, a like's — what the patch stores and the engine plays) to where it is
	/// drawn, and a drawn place back to the real one a press means. Through the warp of the map being drawn, so a mark
	/// always sits on the ground built for it; the identity, exactly, without likes. Nothing here writes a coordinate:
	/// automation and modulation of the place stay in real coordinates.
	[[nodiscard]] WarpPoint viewOf(double travelX, double travelY) const;
	[[nodiscard]] WarpPoint worldOf(double travelX, double travelY) const;
	/// Where the selected sound is DRAWN, in the view's world: its place through the warp.
	[[nodiscard]] double viewDrift() const;
	[[nodiscard]] double viewDriftY() const;
	/// A real place on the sheet, and on the glass.
	[[nodiscard]] visage::Point sheetAtWorld(double travelX, double travelY) const;
	[[nodiscard]] visage::Point glassAtWorld(double travelY, double travelX) const;
	/// What the likes made of the map being drawn — its model and its warp — or null.
	[[nodiscard]] std::shared_ptr<const TerrainLikeState> likesDrawn() const;

	/// The middle of the view, in home. Not the selected sound's home: the view holds still under a
	/// scrub and re-centres when the gesture ends.
	///
	/// Answers before the first frame, and before the first frame after the selection moves, by working
	/// out where the view WOULD settle. A view that only existed once something had drawn it would make
	/// a press arriving first — which a test does and a host can — measure against a centre of zero.
	double viewCentre() const;

	/// How much space the chart shows, at the current zoom. Quoted in zones because that is the unit
	/// every measurement of this engine is quoted in.
	double homeSpanZones() const;
	double travelSpan() const;

	/// The view and mutation scale. One number does both jobs, which is the design's: a closer look
	/// and a smaller step are the same intention, at every zoom (see `mutate`).
	double zoom() const noexcept { return zoom_; }
	void setZoom(double zoom);

	/// THE WIDEST THE VIEW GOES, and it is a LEGIBILITY limit now rather than an addressing one.
	///
	/// The zoom needed more range outward. It stopped at ×1 — 64 zones, one neighbourhood, a keyhole. Then at the whole band,
	/// 3069 zones, on the reasoning that everything past it was nowhere. There is no nowhere any more,
	/// and since span averaging a texel reports the MEAN of what it covers, so the picture no longer
	/// comes apart as the view widens — it summarises. The number, and the measurements that chose it,
	/// are at the definition.
	static double minZoom();
	/// How much home the widest stop shows, in zones — 16× the band this ladder used to end at.
	/// Measured rather than chosen; the reasoning is in minZoom.
	static constexpr double kWidestSpanZones = 49152.0;
	static constexpr double kMaxZoom = 64.0;
	/// What ×1 means, kept as a name because it is no longer the end of the ladder.
	static constexpr double kUnitZoom = 1.0;
	/// The chart's span at ×1, in zones and in travel. §7.1b measured 64 zones apart as 7° of plane
	/// angle — clearly one neighbourhood — which is what makes it the right window to open at.
	static constexpr double kBaseZones = 64.0;
	/// And the travel span at ×1, which is the SAME SPAN IN THE SAME UNITS. Not a taste: the basis's
	/// phase coefficients are of order one, so a unit of travel moves each triangle's phase about as
	/// far as a unit of home does — and a chart whose two axes were scaled differently would draw a
	/// step right and a step up as the same distance when they are not. Matching them makes the picture
	/// isotropic in the coordinate the engine actually builds at.
	///
	/// The first two attempts are worth keeping because the photograph is what settled it: at a span of
	/// 2.0 every column was past the point where characters stop being related (the plan measures an
	/// offset step of 0.05 as already saturating) and the chart was vertical mush; at 0.5 it still read
	/// as stripes, because 0.5 of travel is some four times the whole 64-zone height.
	static constexpr double kBaseTravel = kBaseZones / 1023.0;
	/// **AND params.cpp SPELLS THE SAME NUMBER**, because the Radius parameter's modulation swing is
	/// stated in these units and the engine must not include this header. They agree by arithmetic
	/// rather than by inclusion, so this is what notices if either side moves.
	static_assert(kBaseTravel == 64.0 / 1023.0,
	              "params.cpp's kBaseTravel must match: Radius's modSpan is half of it");

	/// **HOW MUCH WIDER THAN THE VIEW THE MAP IS BUILT — the speculative margin** (2026-09-19:
	/// undrawn canvas still entered the viewport on a drag; ideally the margin is biased in the direction of
	/// the scroll).
	///
	/// A MAP BUILT TO COVER EXACTLY THE VIEW HAS NOTHING LEFT WHEN THE VIEW MOVES, and that is the
	/// whole of the defect. The picture on screen belongs to a window one build old — placed by that
	/// window's own coordinates (`terrainPlacement`) and carried into it from the one before — so any
	/// pan leaves a strip at the leading edge that no previous window covered and `carry()` therefore
	/// cannot fill. Its own comment says as much: *"it is what a strip filling in at the leading edge
	/// of a pan looks like"*.
	///
	/// MEASURED BEFORE THIS EXISTED, 866x534, a one-second fling: worst 25% of the chart with nothing
	/// drawn on it, mean 9.5%, and zero at rest. The offset that produces it is about 15.5% of a view
	/// each way, so a map covering `1 + 2 x 0.155` of the view is the bound the measurement asks for
	/// and 1.35 is that with a little room for a slower build.
	///
	/// WHAT IT COSTS, SAID PLAINLY: the texel count does not change, so a wider map is a COARSER one —
	/// 128 texels now span 1.35 views, which is 95 across the view where there were 128. That is a real
	/// loss of detail and it is paid everywhere, which is why backlog GB (denser sampling toward the
	/// centre, the original) is the sequel rather than an optional extra: it recovers the
	/// budget from rim texels the bubble was already squeezing out of sight.
	static constexpr double kTerrainMargin = 1.35;

	/// The span a map is BUILT at, given the span the view shows. One arithmetic, so a test that waits
	/// for "the snapshot that matches this view" and the builder that makes it cannot drift apart —
	/// three cases compared the built span against the view's own and would silently start waiting
	/// forever otherwise.
	/// **AND `FEATHERS_TERRAIN_MARGIN=<x>` OVERRIDES IT, for one reason: an A/B on a SHARED HOST has to
	/// take both arms under the same load or it is a measurement about other people's compiles.** The
	/// void is proportional to build time and build time swings with the box — the same committed code
	/// measured 0.0% worst at load 7 and 14%-86% at load 38, which is the whole range of the defect
	/// arriving from outside this repo. Rebuilding between arms puts minutes and a different load
	/// between them; a channel lets them be interleaved. Read once, so a process is one arm.
	static double terrainMargin();
	static double builtSpan(double viewSpan) { return viewSpan * terrainMargin(); }

	/// HOW FAR INSIDE THE EDGE AN AUTOMATED FOLLOW LEAVES THE STAR, as a fraction of the view.
	///
	/// THE ONE CONSTANT THAT DECIDES A DESIGN QUESTION, and 0.5 is not a coincidence: at half a view the inset
	/// below is exactly zero, so the follow settles on the star itself and this IS "centre it" — the
	/// shipped behaviour, arithmetically identical to `recentre()` rather than merely similar to it,
	/// which is why the case for it can assert equality instead of approximate agreement.
	///
	/// 0.15 IS THE ALTERNATIVE, kept alive rather than described: a follow that moves only far enough to
	/// bring the star back on-chart uncovers about what it moves, so ~15% of the view instead of the
	/// ~50% that centring costs by construction (a star one pixel outside is half a view from the
	/// centre). ui-10 measured that strip at 38% morph / 51% stair. The trade is the strip against
	/// where the star comes to rest, which is a question for eyes and not for arithmetic — the design
	/// asks for the view CENTRED on the active point, so centring keeps the default and this flips it.
	///
	/// Deliberate moves — a finished gesture, dice, clone, undo, redo, a new selection — do not come
	/// through here at all: they centre unconditionally, and that is not in question.
	static constexpr double kFollowMargin = 0.5;

	/// Where the view settles when a star is carried off the chart, as pure arithmetic on the numbers
	/// that decide it. Public and static so both margins can be measured without a window, and so the
	/// alternative above is proven to work rather than merely proven to compile.
	///
	/// `margin` of 0.5 returns `starHome` exactly — the centring answer — for every span.
	static double followCentreFor(double starHome, double span, bool throughTop, double margin);

	/// HOW CLOSE TO THE GLASS A DRAGGED POINT GETS before the view starts moving with it.
	///
	/// Even after the centring fix a drag could still reach the edge of the plane: dragging the star, the
	/// terrain stopped and the point could move no further. The VALUE was
	/// never stuck: `travelXAtScreenX` extrapolates past the window, and one measured drag wrote drift out to
	/// 3.5 visible widths. The PICTURE was. The view is held for the whole grab, so the star and the
	/// terrain under it leave the chart and the surface stops answering the hand — which reads as the
	/// edge of the world. Releasing re-centred and let you drag again: a ratchet, not a chart.
	static constexpr double kScrubEdgeMargin = 0.15;
	/// THE SELECTION JUMP'S EDGE: a Kit pad whose star is within this share of the radius from the rim is "near the edge" and
	/// the view jumps to it, as it does to one off the chart (by design).
	static constexpr double kPadJumpMargin = 0.15;

	/// **HOW FAST A DRAG MAY CARRY THE WORLD, IN VIEWS PER SECOND** (the pan rate is throttled so
	/// that unrendered terrain does not come into the viewport). An ordinary drag is
	/// under the cap and the star lands exactly where the pointer is, which is what pressing on the chart MEANS; a fling
	/// is not, and the star follows at this speed while the hand gets ahead of it, catching up as the hand slows. In VIEWS
	/// rather than zones because what the builder owes is a fraction of a map — the same demand at every zoom.
	static constexpr double kViewsPerSecond = 1.2;

	/// The view centre that keeps `star` at least `margin` of a span inside the view — and the SAME
	/// centre when it already is. Pure, static and public for the same reasons as followCentreFor.
	///
	/// ONLY WHEN NEEDED, which is what keeps A15 true: a view that tracked the point continuously would
	/// ask for a new terrain every frame, and at 31ms a build the map would never finish. In the middle
	/// of the chart this returns the centre unchanged and the ground stays still under the hand.
	static double keepInsideView(double star, double centre, double span, double margin);

	//----------//
	// The sound //
	//----------//

	/// The selected sound's identity, and where that puts it on the chart.
	SoundIdentity identity() const;
	double selectedHome() const;
	/// Where the ENGINE actually has this sound in its plane: its own drift plus
	/// `Weather × (puck − anchor)`. Read and never written here — the second term belongs to whoever
	/// is playing the Perform circle — and exactly zero at Weather 0, which is the parameter's default
	/// and the whole of v1's behaviour.
	///
	/// DELIBERATELY NOT WHAT THIS CHART DRAWS WITH, by design; see the note at the top of
	/// this file. It is kept because it is the honest answer to "where is this sound now", and because
	/// the case that pins the decision needs to be able to watch it move while the chart holds still —
	/// a stationary chart and a stationary formula are different claims, and only one of them is the
	/// one being made.
	double selectedTravel() const;
	/// The sound's own drift along the plane's first axis: what a scrub writes, and the horizontal
	/// coordinate everything on this chart is placed from.
	double selectedDrift() const;
	/// **THE SOUND'S PLACE IN THE PLANE, IN POLAR**: drift became polar in the plane, with a Theta
	/// knob added and drift renamed Radius.
	///
	/// A VIEW ONTO THE STATE, NOT THE STATE. `SoundTravel::offset` stays the cartesian pair the engine
	/// decodes; these are that pair read as a magnitude and an angle. The device pad already has this
	/// shape — its two numbers are a VIEW onto the state through the basis, not the state of record —
	/// and storing polar instead would put a discontinuity at the origin into the patch.
	///
	/// AT THE ORIGIN THE ANGLE IS UNDEFINED and `selectedTheta()` answers 0. That is honest rather than
	/// convenient: a point with no radius has no direction, so the Theta knob has nothing to turn until
	/// there is a radius to turn. Said out loud because the alternative — remembering a hidden angle —
	/// would make the knob move a number the patch does not contain.
	double selectedRadius() const;
	double selectedTheta() const;
	/// The vertical half of the sound's place in the decoded plane — see the definition (backlog GY).
	double selectedDriftY() const;
	/// Where the selected sound is PLAYED: authored place plus the engine's live polar pull. Drawing only.
	[[nodiscard]] AnchorPoint playedPlace() const;
	/// Where the rotate encoder reads, in radians: the selected sound's own turn, else the global
	/// audition pin's, else 0 — the turn every unturned sound genuinely plays at.
	double selectedRotation() const;
	/// What the Rotate corner prints: `selectedRotation` in degrees, `Unit::Degrees`'s own reading.
	[[nodiscard]] std::string rotateReadout() const;
	/// The selected sound's COARSE gamma: the unbounded base the corner accumulates into, beside the
	/// ±4 vernier the parameter still holds. Home is the two added plus the global frame.
	double selectedGammaBase() const;
	/// How far one full encoder travel carries the base. The vernier's whole range, so the corner
	/// moves gamma at exactly the rate it always did — what changed is that it no longer stops.
	static constexpr double kGammaTurn = 8.0;
	/// Where the gamma dial sits for a given gamma, 0..1 around its travel. A compass rather than a
	/// slider, because the base is unbounded — the same wrap, and the same Euclidean-remainder lesson,
	/// as rotateDialPosition.
	static float gammaDialPosition(double gamma);

	//---------//
	// The verbs //
	//---------//

	/// What a press at a point would take hold of.
	/// The three `Debug*` switches are DIAGNOSTIC-ONLY and are deliberately last, so every existing
	/// value keeps its number and nothing that persists a Part is disturbed. They exist only where
	/// FEATHERS_DIAGNOSTIC is defined — which is every install build and no shipping one.
	enum class Part { None, Chart, Gamma, Zoom, Rotate, Radius, Theta, Mutate, Undo, Redo, Like };
	Part hitAt(visage::Point position) const;
	visage::Bounds partBounds(Part part) const;


	/// Put the selected sound's own drift here — the coordinate the chart draws it at, so a drag is
	/// 1:1 with what is under the pointer.
	///
	/// THIS USED TO SUBTRACT THE PERFORMANCE TERM, because the star sat at the sum, and that term had
	/// to be captured at the press and held for the whole gesture: read fresh, a host automating
	/// Weather, Path or Pos X DURING a drag moved it under the hand and every frame wrote the inverse
	/// of that movement into the sound's stored drift — a silent, permanent edit made by the
	/// automation rather than by the user. Since that decision there is no term and nothing to hold, so
	/// that leak is gone by construction rather than by a captured value. Its case is kept, asserting
	/// the stronger property.
	void driftTo(double drift);
	/// Commit both halves of the sound's place in one gesture — see the definition (backlog GY).
	void driftToXY(double driftX, double driftY);

	/// Carry the selected sound further along its own line, cumulatively: the coarse half of gamma,
	/// which is state rather than a parameter and so has a bracket of its own — the same shape the
	/// drift channel has. Public for the same reason `driftTo` is: it is a verb, and the corner is one
	/// way of asking for it rather than the only one.
	void nudgeSelectedGamma(double delta);

	/// The mock's 🎲: a random step of up to a quarter of the visible span, so ZOOM IS THE MUTATION
	/// SCALE — closer in means smaller jumps, which is the design's reason for the one control doing
	/// both jobs.
	///
	/// **AT EVERY ZOOM, ZOOMED OUT INCLUDED — NO CAP** (reversing 8560e420). That
	/// commit held the roll at ×1's ±16 zones on the way out, on the reasoning that a quarter of a wide
	/// view is not a mutation but a different sound. Once the ladder reached ×1/768 the cap made a roll
	/// 0.03% of the view at the widest stop, which sounded like tiny moves that did not scale with zoom.
	/// The decision: a roll is always a quarter of what is on screen, and the protection is dropped —
	/// zoomed out, the dice is a long jump by intent. The re-centre after a roll stays.
	void mutate();
	/// **THE HEART BESIDE MUTATE** (backlog ON: "more like this"). Likes the selected sound where it
	/// stands — its place, the home of the plane, and the partial envelope the terrain measures there, captured now — or,
	/// when the sound already stands on a like of this plane, removes that one. One undo step, and the patch is modified.
	void toggleLike();
	/// Which like the selected sound stands on, if any: one on the plane being drawn, within `kLikeTouchPixels` of the
	/// star. What the heart reads as its state and what a second press removes.
	[[nodiscard]] std::optional<size_t> likeUnderStar() const;
	/// **THE HEART'S CONTEXT MENU**: "Clear all likes on <engine>, sound <letter>", this terrain's only, greyed with none. Built here so a case can read it
	/// and pick from it; a popup trigger on the heart opens it.
	[[nodiscard]] visage::PopupMenu likesMenu();
	static constexpr int kClearLikes = 1;
	/// **THE LIKES' STRENGTH** (backlog ON): "Like strength ×1/×2/×4" on the heart's menu, ids kLikeStrength + s. Weighs
	/// every like as s likes in every sound's phase warp; stage state, not saved, ×1 on every open.
	static constexpr int kLikeStrength = 100;
	[[nodiscard]] double likeStrength() const { return likeStrength_; }
	/// Every like OF THIS ENGINE AND SOUND gone in ONE undo step; the patch is modified, and with no likes the warp is the identity again.
	void clearLikes();
	/// **THE LIKE-SET LOG'S TICK** (plugin/ui/likes_log.h), every frame the stage draws: a `snapshot` line when the patch's
	/// like set is not the one the log has described (the first frame, a patch load), and a `walk` line when the selected
	/// sound's stance differs from the last one logged and has been held still `likeslog::kWalkDwellSeconds` — so a drag
	/// writes nothing until it settles. Message thread; nothing is measured while the log is off.
	void logTick();
	/// TEST ONLY: the clock the log reads (unix seconds); the system clock by default.
	void setLogClockForTest(std::function<double()> clock) { logClock_ = std::move(clock); }
	/// The selected sound's stance as a like stores it: its place, the plane's home, the envelope, the full point, the angles.
	[[nodiscard]] TerrainLike stanceNow() const;
	/// Whether a like belongs to the terrain being drawn: the selected engine's and the selected sound's.
	[[nodiscard]] bool likeApplies(const TerrainLike& like) const;
	static constexpr float kLikeTouchPixels = 6.0f;
	/// TEST ONLY: how many like marks the last frame drew on the chart.
	[[nodiscard]] int likeMarksDrawn() const noexcept { return likeMarksDrawn_; }
	/// Where like `index` of the patch lands on the plane being drawn — its stored point projected (`terrainLikePlaceOn`).
	[[nodiscard]] PlanePlace likePlace(size_t index) const;
	/// And where the selected sound's crumb `index` lands: its identity's own point, projected.
	[[nodiscard]] PlanePlace crumbPlace(size_t index) const;
	/// The window the terrain would be asked for now — the selected sound's whole world — without asking for it.
	[[nodiscard]] TerrainWindow terrainWindowNow() const;
	/// Any sound's window (its world, at the current view), and the likes' phase warp it carries.
	[[nodiscard]] TerrainWindow terrainWindowFor(size_t slot) const;
	/// Every sound's likes' phase warp re-derived where its likes changed, and published to the synth.
	void refreshLikeWarps() const;
	/// How far a roll can go, on each axis, at this zoom — readable because the surface prints it and
	/// the two have to be the same number.
	double mutationZones() const;
	/// WHAT THE POLAR KNOBS TELL THE STAGE AROUND A GESTURE — it owns the history of a sound's identity.
	void polarGestureBegan();
	void polarGestureEnded(TravelAxis axis);
	double mutationTravel() const;

	/// Undo and redo over the EDITOR'S history, which this surface walks but no longer owns. Every
	/// committed gesture is one entry, which is the same unit the host's own undo sees — and so is
	/// every clone, which is what made the history shared in the first place: the verb that needed
	/// recording lived on the cards, and the stack lived in here where it could not reach.
	bool canUndo() const noexcept { return history_.canUndo(); }
	bool canRedo() const noexcept { return history_.canRedo(); }
	void undo() { history_.undo(); }
	void redo() { history_.redo(); }
	size_t historyLength() const noexcept { return history_.length(); }

	/// Breadcrumbs for a sound, oldest first. Full identities — see SoundIdentity.
	std::vector<SoundIdentity> crumbs(size_t slot) const;

	/// How many quads the terrain submits: one when there is a map, none before the first rows land.
	///
	/// It used to be thousands — a quad per texel — and the number was readable because the ceiling it
	/// had to stay under is a renderer limit that fails silently. The map is a picture and is drawn as
	/// one now, so the answer is one, and the count stays readable because "one" is the claim worth
	/// keeping true: it is what took the terrain out of the arena census entirely.
	int terrainQuadCount() const;

	/// WHERE ON SCREEN the terrain picture lands this frame, for the map currently in hand.
	///
	/// THE ONE COPY OF THAT ARITHMETIC, and it is a seam rather than a convenience: `drawTerrain` calls
	/// this, so a test reading it is reading what the frame draws. The published map being complete says
	/// nothing about whether any of it is ON the chart — a snapshot is placed by the window it was BUILT
	/// for, and a view that has moved since leaves it somewhere else. That gap is exactly what let a
	/// "0.0% void" measurement coexist with a visibly flickering terrain, so the number a case needs
	/// is this one, not the buffer's.
	visage::Bounds terrainPlacement() const;
	/// THE RIM MASK — void outside the disk, nothing inside it. A statement about the DISK alone, which
	/// is why it takes one and reads no terrain: it has to be drawn on every frame the ground paints a
	/// picture, including the frames where this surface's own snapshot read comes back empty.
	void drawRimMask(visage::Canvas& canvas, const StageDisk& d) const;




	/// TEST ONLY: whether the view is part way through a pan. The journey is the thing under test and
	/// it is invisible in any single frame — a case that only looked at the destination would pass
	/// against the teleport this replaced.
	[[nodiscard]] bool viewGlidingForTest() const noexcept { return glidingView_; }

	/// Where the selected sound's crumb `index` sits on screen.
	///
	/// A named function rather than a line inside draw() for the reason the rotate dial is one: it has
	/// to place each crumb from that crumb's OWN identity, and the only way to check that is to be able
	/// to ask. Both coordinates, which is where it went wrong — the horizontal came from the live sound
	/// and every crumb landed in the same column.
	visage::Point crumbAt(size_t index) const;

	/// Whether crumb `index` is on the chart at all, and how many are.
	///
	/// A crumb's place is worked out from its own home, and at a tight zoom a crumb left elsewhere
	/// projects THOUSANDS of pixels away. Its mark is therefore drawn only when the chart contains it;
	/// the LINE to it is a separate question, because a leg between two off-chart crumbs can still
	/// cross the disk and has to be drawn where it does — see clipSegmentToDisk.
	bool crumbIsVisible(size_t index) const;
	size_t visibleCrumbCount() const;

	static constexpr size_t kMaxCrumbs = 12; ///< the mock's `if(crumbs.length>12)crumbs.shift()`

	/// The rectangle every overlay mark of the last frame fell inside — the trail, the star, and the
	/// star's own label. Empty until something has been drawn.
	///
	/// THE TYPE ITSELF MOVED TO celestial.h, and this name is kept as an alias so that every case
	/// saying `ExploreStage::InkBox` still says it. A photograph showed the selected star and its label
	/// drawn beyond the disk at ×40, which is why this surface records its ink; Perform's puck now
	/// answers the same question since A30 was reversed, and one definition of "inside" is what
	/// keeps two surfaces agreeing about where their own rim is.
	using InkBox = ui::InkBox;
	InkBox overlayInk() const noexcept { return overlayInk_; }

	/// **WHERE THE STAR GLYPH WAS ACTUALLY DRAWN**, in glass coordinates, as of the last `draw()`.
	///
	/// A VALUE SEAM, and it exists because a case that recomputes the position proves nothing about the
	/// drawing. MEASURED 2026-09-21: the case named *"the puck is DRAWN where the press put it"* — whose
	/// own comment says *"the claim is about PIXELS, not about the number behind them"* and *"nothing
	/// that reads state alone can stand in for it"* — asserted on `glassAt(selectedDriftY(), ...)`,
	/// which is the same recomputation the draw does rather than the draw's own answer. A known-bad
	/// that moved a draw site left it green.
	///
	/// `overlayInk()` cannot do this job: it is a bounding box over the glyph AND its label AND the
	/// crumbs, so it is wide by a label and its centre is not the star. This is the star.
	///
	/// `{0, 0}` before the first draw, and when the sound is off chart (the rim mark is a different
	/// thing and is asserted separately) — so a case must establish that the star IS on chart before
	/// reading it, which the `starOnChartForTest` flag beside it answers.
	[[nodiscard]] visage::Point starDrawnAt() const noexcept { return starDrawnAt_; }
	/// Where the played-place star was drawn on the last paint, and whether one was — `starDrawnAt`'s twin.
	[[nodiscard]] visage::Point playedMarkAt() const noexcept { return playedMarkAt_; }
	[[nodiscard]] bool playedMarkDrawn() const noexcept { return playedMarkDrawn_; }
	[[nodiscard]] bool vectorDrawn() const noexcept { return vectorDrawn_; }
	[[nodiscard]] visage::Point vectorFrom() const noexcept { return vectorFrom_; }
	[[nodiscard]] visage::Point vectorTip() const noexcept { return vectorTip_; }
	[[nodiscard]] bool starDrawnOnChart() const noexcept { return starDrawnOnChart_; }
	/// ONE OTHER PAD AS THE LAST PAINT DREW IT: where, hollow or filled, and its Walk arrow's tip if it drew one.
	struct PadMark {
		size_t slot = 0;
		visage::Point at {};
		bool hollow = false;
		bool arrow = false;
		visage::Point tip {};
	};
	[[nodiscard]] std::span<const PadMark> padMarksForTest() const { return std::span(padMarks_).first(padMarkCount_); }
	/// The view's two centres as stored, whatever the selection now (the jump's cases read them before and after).
	[[nodiscard]] double storedViewCentreForTest() const noexcept { return viewCentre_; }
	[[nodiscard]] double storedTravelCentreForTest() const noexcept { return travelCentre_; }
	/// The chart's two spans in travel (a chart's diameter), so a case can put a pad a known share of the radius out.
	[[nodiscard]] double travelSpanForTest() const { return travelSpan(); }
	[[nodiscard]] double homeSpanForTest() const { return homeSpanZones() / 1023.0; }

	/// How many zone labels the last frame wrote, and the two steps that decide it.
	///
	/// Readable because the bug was a COUNT: at a wide view every gridline carried a label and the
	/// chart was papered in them — illegible, and a few hundred novel strings per view at a large
	/// gamma base, which is the heaviest glyph churn this plugin can make. A picture cannot assert
	/// "about six"; this can.
	int gridLabelCount() const noexcept { return gridLabels_; }
	static double gridLineStep(double spanInZones);
	static double gridLabelStep(double spanInZones);
	/// About this many labels, at every zoom and every base — the axis-tick target.
	static constexpr double kWantedGridLabels = 6.0;

	/// The sky of the last frame: how many stars reached the chart, and the box they fell in.
	///
	/// Readable for the reason the terrain's quad count is: the failure is a DENSITY and a SPREAD,
	/// neither of which a picture asserts and both of which a number does. The sky's cell grows with

	/// THE NUMBER THE GAMMA CORNER LAST DREW, and where its compass was pointing when it drew it.
	///
	/// Readable because of a decision this surface OVERTURNED. homeToCoord used to say, in as many
	/// words, that a scrub must not move gamma "because a scrub that quietly moved it would make that
	/// encoder's readout a lie" — and the one-value decision makes every vertical drag move exactly
	/// that number. The old worry was right about the danger and wrong about the cure: the answer is
	/// not to hold gamma still, it is to make sure the corner is SEEN to move, or this becomes the next
	/// "rotate seems to not work at all" — a verb that works and looks dead.
	///
	/// So a case has to be able to say "the reading changed, and it changed to the sound's own number".
	/// A picture cannot say either half; these two can. Recorded at the DRAW rather than computed
	/// beside it, because a reading computed twice is two readings and only one of them is on screen.
	const char* gammaReadingDrawn() const noexcept { return gammaReading_; }
	float gammaDialDrawn() const noexcept { return gammaDial_; }


	/// The box the overlay is allowed to mark: the disk's own square. The chart is a circle and the
	/// clipping is to the circle, so this is the weaker of the two statements — but it is the one a
	/// rectangle can hold, and it is the statement the bug broke.
	InkBox chartBox() const;

	/// How far past the chart a star's halo is permitted to bleed. The glyph is drawn AT a point rather
	/// than inside a shape, so a sound sitting on the rim spreads its halo over the rim: that is a
	/// glyph having a size, not a mark in the wrong place, and the tolerance says so in one number
	/// rather than by exempting the star from the check.
	static constexpr float kGlyphBleed = 24.0f;

	/// Whether the two corners whose engine API is still being built answer the mouse. They are drawn
	/// and labelled either way — scaffolding is not scope, and a corner that vanished until the engine
	/// arrived would make the surface look like it had fewer verbs than the design gives it.
	/// BOTH CORNERS ARE LIVE. Rotate re-aims the selected sound's own plane through
	/// `rotateSoundBy`'s deriving overload, which works out the plane the sound is CURRENTLY on rather
	/// than making this surface guess — a guess here would be a default-constructed generator, which is
	/// the degenerate plane, and the sound would jump to it on the first click of the encoder.
	static constexpr bool kRotateLive = true;
	/// The sound's place in the plane, read in polar. Its state of record is `SoundTravel` — the same
	/// pair the chart's two axes write — so both corners are real encoders over the same numbers the
	/// gesture moves: the gesture for a hand, the encoders for a number.
	static constexpr bool kRadiusLive = true;
	static constexpr bool kThetaLive = true;

	/// **HOW FAR ONE FULL ENCODER TRAVEL SWINGS THE SOUND AROUND THE ORIGIN, in radians.** A gearing,
	/// like `kRotateSpan` and for the same reason — theta has no ends, so there is nothing for a range
	/// to be. A quarter turn per full travel: four drags to go all the way round, which is slow enough
	/// that a twitch does not fling the sound to the other side of the plane and fast enough that
	/// getting to the far side is not a chore.
	///
	/// **NOT `kRotateSpan`, though it is a multiple of it**, and they must not be merged: rotate turns
	/// the PLANE the sound is decoded onto (everything moves) and theta walks the sound around WITHIN
	/// that plane (only this sound moves). Backlog GF slowed the first one; nothing about that decision
	/// says anything about this one.
	static constexpr double kThetaSpan = 3.14159265358979323846 / 2.0;
	static_assert(kThetaSpan > 0.0, "the corner's gearing is a rate and must be positive");

	/// **HOW FAR ONE FULL ENCODER TRAVEL TURNS THE PLANE, in radians — a GEARING, not a range.**
	/// Unbounded: rotate is a re-aiming rather than an amount, so it has no ends, and the turn
	/// accumulates across as many drags as it takes. The mock's own knob is unbounded too.
	///
	/// **SLOWED FOURFOLD, 2026-09-20** (backlog GF: the rotate control was too
	/// sensitive per unit of knob rotation). It was pi — a full 140px
	/// travel turning the plane a half turn, which is **1.29 degrees per pixel**, so a ten-pixel twitch
	/// re-aimed it by thirteen degrees. At pi/4 a full travel is 45 degrees and a pixel is 0.32.
	///
	/// THE RANGE IS NOT WHAT CHANGED, and the backlog entry is explicit that narrowing it would be the
	/// wrong fix: the reachable set of planes is a statement about the instrument, the gearing is a
	/// statement about the hand, and only the second one was wrong. Nothing has become unreachable —
	/// the turn accumulates, so a half turn is now two drags instead of one.
	///
	/// AND IT IS NOT `OrientationRow`'s CONSTANT OF THE SAME NAME. That one is a slider's absolute
	/// THROW (+-half a turn, a position), and its own comment says the Explore corner carries turns
	/// far beyond it. Two different quantities that were numerically equal; only this one is a rate.
	///
	/// **THE NUMBER IS AN EAR'S, NOT A MEASUREMENT'S.** There is no instrument for how fast a re-aiming
	/// should feel. Fourfold is a first proposal chosen to be felt rather than argued over; pi/2 is
	/// half as much of a change and pi/6 is half again as much, and either is one edit.
	static constexpr double kRotateSpan = 3.14159265358979323846 / 4.0;
	/// NOT THE DIAL'S PERIOD. `rotateDialPosition` wraps on a full turn (2*pi) and says so in its own
	/// arithmetic; it used to wrap on `2 * kRotateSpan`, which was the same number only while the
	/// gearing was pi. Slowing the corner is what would have made the compass lie.
	///
	/// AND NOT A CONVERSION FACTOR EITHER: a case borrowed this constant as the mathematical pi for
	/// degrees-to-radians, and would have started expressing degrees in quarter-radians. Both were
	/// found by changing the number and reading what broke, which is the argument for changing it in
	/// one place and letting the suite speak.
	static_assert(kRotateSpan > 0.0, "the corner's gearing is a rate and must be positive");

	/// Where the rotate dial sits for a given turn, 0..1 around its travel.
	///
	/// A FUNCTION RATHER THAN A LINE INSIDE draw(), because it has to agree with the degree text beside
	/// it and the only way to check that is to be able to ask it. It wraps every 2π — rotate is
	/// unbounded, so the dial is a compass rather than a slider — and it wraps in BOTH directions,
	/// which `std::fmod` does not: fmod keeps its argument's sign, so every turn past −180° came back
	/// negative and clamped the dial to zero while the text went on counting. Untouched sounds already
	/// sit anywhere in ±π, so that was one drag away from anybody.
	static float rotateDialPosition(double radians);

	/// The mock's corner knob, at the face size this surface uses.
	static constexpr float kCornerDiameter = 46.0f;
	static constexpr float kCornerWidth = 74.0f;
	/// **THE POLAR PAIR IS TWO KNOBS IN ONE CORNER'S WIDTH, so its faces are smaller than the other
	/// three corners'** (smaller control elements are acceptable to make room for Theta).
	/// They stack upward from the bottom-right rather than sharing
	/// the corner's width side by side, because the readout under each one is a signed number with
	/// three decimals and half of 74px cannot hold it — a control whose value is unreadable is a
	/// control you turn by guessing.
	static constexpr float kPolarDiameter = 34.0f;
	static constexpr float kPolarGap = 4.0f;
	/// The breathing room between the width column and the chart square — part of the reserve
	/// disk() takes off the frame before sizing the square (V16), so the two can never touch.
	static constexpr float kFlankGap = 12.0f;
	static constexpr float kButtonHeight = 26.0f;
	static constexpr float kButtonGap = 6.0f;

	void resized() override;

private:
	/// **THE SUBSCRIPTIONS, HELD** — see `ui::Watch`. Declared last so they are released FIRST, into
	/// lists that are still alive.
	std::vector<Watch> watches_;
	clap_id paramOf(SoundField field) const;
	double gammaSum() const;
	/// Keep the width knob's visibility, bounds and enablement in step with the selection, the
	/// engine and the chart geometry. Cheap and idempotent; runs from resized() and update().
	/// Renamed syncFlagshipKnobs() to drive all three per-sound controls.
	void syncWidthKnob();

	/// THE SLOT TABLE'S READERS (backlog LJ). The row for the engine the patch is on; where a control stands in it, if
	/// anywhere; and where it waits while hidden (its slot in the first row that seats it, so a hidden knob keeps a real
	/// rectangle, the one its own engine gives it).
	[[nodiscard]] const SlotRow& currentSlotRow() const;
	[[nodiscard]] std::optional<Slot> slotOf(SurroundControl control) const;
	[[nodiscard]] Slot homeSlotOf(SurroundControl control) const;
	/// The knob a control is for `sound` (engine-wide controls ignore it); null for a box.
	[[nodiscard]] Knob* surroundKnob(SurroundControl control, size_t sound) const;
	/// A box's label and menu, by the control it is.
	[[nodiscard]] const char* boxLabel(SurroundControl control) const;
	void showBoxMenu(SurroundControl control);
	/// Where a box opens its menu: the slot it stands in.
	[[nodiscard]] visage::Bounds boxBounds(SurroundControl control) const;
	/// Draws whatever box the table seats in `slot`; returns whether it drew.
	bool drawSlotBox(visage::Canvas& canvas, Slot slot);


	SoundIdentity identityOf(size_t slot) const;

	/// WHAT THIS SURFACE DOES AFTER THE HISTORY HAS MOVED, and deliberately nothing more: the values
	/// are already back — the record put them there through the bridge — so what is left is pointing
	/// the editor at the sound that changed. Select it, re-centre on it, say which way we walked.
	///
	/// An entry that is not about one sound (a future verb that changes something global) points
	/// nothing, which is the honest answer rather than a guess at a slot.
	void editApplied(const EditRecord& record, const char* verb) override;
	void editRecorded(const EditRecord& record) override;

	/// Post a value as part of the gesture in flight, opening one on the first value that moves — the
	/// same grammar as the knobs and the Perform drag.
	void commit(clap_id id, double value);
	/// The same, for the drift channel, which is state rather than a parameter and so has a bracket of
	/// its own — see ParamBridge::beginSoundTravelGesture.
	void commitDrift(size_t slot, double x, double y);
	/// And for the plane. Turns the sound's CURRENT plane by a delta rather than setting one, because
	/// the state of record is the generator and a turn has to accumulate in it — see rotateBy.
	void rotateSelectedBy(double radians);
	/// The pixels one encoder travel of the Rotate or Gamma corner spans for this event, RE-ANCHORING the
	/// drag when Shift has gone up or down since the last one — `Knob::dragTo`'s rule, for the same reason.
	float regearCorner(const visage::MouseEvent& e);
	void endGesture();

	/// Remember an edit in the editor's history, so undo and redo have something to walk. `verb` is
	/// what the tray says when it is walked — "scrub", "mutate" — because an undo that only says
	/// "Undo" makes a player check what moved instead of being told.
	void record(size_t slot, const SoundIdentity& before, const SoundIdentity& after, const char* verb);
	/// The one centering road both observer entry points share — see the definition.
	void followEdit(const EditRecord& record);
	void plantCrumb(size_t slot, const SoundIdentity& at);

	/// Ask for the map this view needs, and hand back the WINDOW that was asked for. The return value
	/// is what lets draw() tell "the map on screen is the one I asked for" from "the worker still owes
	/// me one" — a question `request()` cannot answer, because it coalesces and returns nothing.
	TerrainWindow askForTerrain();
	void recentre();
	/// WHERE THE VIEW IS PUT, with no opinion about whether it got there in one frame or forty.
	void placeView(double candidateCentre, double candidateTravel);
	/// **PAN THE VIEW ONTO THE SELECTED SOUND INSTEAD OF TELEPORTING ONTO IT** — `recentre()`'s
	/// destination, reached over the frames that follow at `kViewsPerSecond`. Used by the two gesture
	/// releases (a click and a jog) and by nothing else: a selection change, a first frame and a
	/// history walk are statements about where the view IS, and easing those would animate a chart
	/// into place that was never anywhere else.
	void glideToSelected();
	/// One frame of that pan. Called from `update()`, and a no-op when nothing is gliding.
	void stepViewGlide();
	/// The view catching up with a star that AUTOMATION carried off the chart, as opposed to a hand
	/// that finished a gesture. `throughTop` says which edge it left by, since the answer is not
	/// symmetric: the view has to move toward the star, and home increases upward.
	void followSelected(bool throughTop);
	/// One door for settling the view, so both axes are recorded in the same breath every time.
	///
	/// It used to also record a gamma baseline, because the view followed gamma; backlog GY removed
	/// the follow, and `centredOnGamma_`/`scrubbedGamma_`/`viewGamma()` went with it once `scrubTo`
	/// was deleted and the last thing that wrote gamma from a chart gesture was gone.
	void settleOn(double candidateCentre, double candidateTravel);
	/// **THE STAR FOLLOWS THE POINTER AT A BOUNDED SPEED** (`kViewsPerSecond`) — see the definition.
	void dragStarTo(double wantedHome, double wantedTravel);
	/// Close a released drag that has finished coasting: the undo entry, the crumb, the recentre and the
	/// terrain request a gesture gets, deferred until the star actually reached where the hand asked.
	void finishScrub();
	/// The middle of the view along TRAVEL, the horizontal twin of viewCentre(). Answers before the
	/// first frame the same way, by reporting where it would settle.
	double travelCentre() const;

	/// ONE STALE MARK FOR BOTH HALVES OF THE SURFACE. The ground is a separate frame now, and visage's
	/// `redraw()` marks one frame — so every reason this stage repaints is a reason the picture under it
	/// repaints too, and a stage that told only itself would leave the terrain a frame behind the star
	/// standing on it. Every `redraw()` in this file is this instead; the bare one would compile.
	void repaint() {
		redraw();
		if (ground_ != nullptr) {
			ground_->redraw();
		}
	}

	void drawTerrain(visage::Canvas& canvas);
	void drawTerrainPicture(visage::Canvas& canvas);
	/// Not const: it records how many labels it wrote — see gridLabelCount.
	void drawGrid(visage::Canvas& canvas);
	/// Not const: it records what it drew — see skyStarCount.
	void drawCrumbs(visage::Canvas& canvas);
	void drawStar(visage::Canvas& canvas);
	/// The likes on the plane being drawn, as small hearts at their places. Not const: it counts them (`likeMarksDrawn`).
	void drawLikes(visage::Canvas& canvas);
	/// THE KIT'S OTHER PADS (drum proposal §3.4): each at its place projected onto the selected pad's plane, a filled mark
	/// if it is the selected pad's kind and a HOLLOW one labelled with its kind if not (the map's colours mean nothing for it),
	/// and a faint arrow to where the Walk plays it.
	void drawPadMarks(visage::Canvas& canvas);
	/// Whether a Drum Kit is what the chart shows.
	[[nodiscard]] bool drumKitShown() const;
	/// THE SELECTION JUMP: whether `slot's star stands inside the view as it is now, clear of its edge, in
	/// which case selecting it moves nothing.
	[[nodiscard]] bool padInView(size_t slot) const;
	/// The one road a selection change takes: in a Kit, a pad already in view leaves the view where it is; otherwise (and on
	/// every other engine) the view JUMPS onto the selected sound.
	void followSelection();
	void drawReadouts(visage::Canvas& canvas) const;
	/// Not const: it records the gamma reading it wrote — see gammaReadingDrawn.
	void drawCorners(visage::Canvas& canvas);

	/// Every overlay line goes through here: clipped to the chart, drawn only if any of it is on the
	/// chart, and recorded. One door, so no future mark can be added that misses either half.
	void overlayLine(visage::Canvas& canvas, visage::Point from, visage::Point to, float thickness);

	/// A STRAIGHT LINE ON THE SHEET IS A CURVE ON THE GLASS, and every straight line this surface draws
	/// — the zone grid, the identity rail, the trail between two crumbs — is a line on the SHEET. Under
	/// the bubble they bow outward, hardest near the rim where the foreshortening is, and that bowing is
	/// most of what makes the disc read as a ball rather than as a lit circle.
	///
	/// So: walk the flat line, put every step on the glass, and hand the caller a polyline it can draw
	/// however it draws lines. The step count is fixed rather than adaptive because the worst case is
	/// bounded — a line all the way across the disc — and at that length the curve's deepest sag from a
	/// chord of its own length is under a pixel, which is the only number that matters here.
	static constexpr int kCurveSteps = 24;
	struct GlassCurve {
		visage::Point points[kCurveSteps + 1] {};
		int count = 0;
	};
	GlassCurve curveOnGlass(visage::Point flatFrom, visage::Point flatTo) const;

	/// The trail's legs, which are lines on the sheet and are clipped to the disk leg by leg.
	void overlayCurve(visage::Canvas& canvas, visage::Point flatFrom, visage::Point flatTo,
	                  float thickness);

	ParamBridge& bridge_;
	SoundSelection& selection_;
	/// THE EDITOR'S HISTORY, not this surface's. Held by reference and outliving every widget, which is
	/// what lets a record be walked without asking whether the surface that made it is still alive.
	EditHistory& history_;
	TerrainMap& terrain_;
	Notifier* notifier_ = nullptr;
	Clock clock_;

	double zoom_ = 1.0;
	/// See `terrainZoom()`, and when `zoom_` last moved (`clock_`).
	double terrainZoom_ = 1.0;
	int64_t zoomMovedAt_ = 0;
	double viewCentre_ = 0.0;
	/// The horizontal half of the view, which did not exist until a drag walked off the edge of it. Travel
	/// was pinned to drift 0, so one drift-corner drag — a full visible width at ×1 — took the star off
	/// the chart with nothing to bring it back, and the scrub could not fetch it because a scrub only
	/// addresses the window it can see. That is what "hitting the edge of the plane" was.
	double travelCentre_ = 0.0;
	bool everCentred_ = false;
	/// Which sound the view was last centred on. A new selection is a new chart, so the view follows
	/// it — but only once, and not while a gesture is in flight.
	size_t centredOnSlot_ = 0;

	/// THE VIEW'S OWN JOURNEY, separate from the star's (`dragWanted*`/`settlingScrub_`). The two move
	/// at the same capped rate and usually together, but they are not the same thing: the star is the
	/// SOUND being placed and the view is the window onto it, and a click moves the first instantly
	/// while the second catches up.
	/// **THE GAMMA THE MAP IS HELD AT WHILE THE CORNER IS TURNING** (backlog GY, option (ii) of
	/// three).
	///
	/// WHY IT EXISTS. Gamma sets the one home the whole plane is taken at, so it is in the terrain's
	/// content key and every step of a turn is a genuinely different map. Left alone that is a rebuild
	/// per mouse event, none of which can reuse the last — `TerrainSnapshot::computed` needs the two
	/// windows to agree on every field but their centres, and this is the field that differs. MEASURED
	/// over one sweep: 22 builds and the chart 43% blank on average, which is the "scroll into nothing"
	/// the gamma cases are named for, arriving by a new road.
	///
	/// SO THE MAP HOLDS THE BASE IT STARTED THE GESTURE AT and rebuilds once on release. The chart
	/// shows a stale plane while a hand is actively turning and the true one the moment it stops —
	/// the same bargain the drag cap already makes with the star, and for the same reason: what a hand
	/// needs mid-gesture is continuity, and what it needs at the end is the truth.
	///
	/// EMPTY MEANS "NOT TURNING", so the ordinary path is `selectedGammaBase()` with nothing held.
	std::optional<double> heldGammaBase_;

	bool glidingView_ = false;
	double glideWantedCentre_ = 0.0;
	double glideWantedTravel_ = 0.0;
	int64_t glideSteppedAt_ = 0;

	/// When the star was last stepped toward the pointer, and where the pointer last asked it to be — see `dragStarTo`.
	int64_t starSteppedAt_ = 0;
	/// A released drag still coasting toward `dragWanted*` at the capped rate — see `finishScrub`.
	bool settlingScrub_ = false;
	double dragWantedHome_ = 0.0;
	double dragWantedTravel_ = 0.0;
	int64_t lastTick_ = 0;
	bool everTicked_ = false;
	/// The last map this surface drew, held so `update()` can tell a new one has arrived. Holding it
	/// also keeps that map alive for as long as it might still be on screen, which is what makes the
	/// builder free to move on without waiting for anybody.
	std::shared_ptr<const TerrainSnapshot> lastSnapshot_;

	/// The map as ONE buffer the atlas can recognise, and which snapshot last filled it.
	///
	/// visage keys an image on its data POINTER, so a caller whose pixels change has to overwrite one
	/// buffer rather than hand over a new one — and a snapshot is a fresh allocation every publish. So
	/// the pixels are copied in here when the snapshot changes, which is per BUILD rather than per
	/// frame.
	std::array<uint32_t, static_cast<size_t>(TerrainMap::kResolution) * TerrainMap::kResolution> terrainPixels_ {};
	/// **AND THE SETTLED MAP'S UPSAMPLED PICTURE** (`TerrainSnapshot::fine`, 2026-09-17): cubic over the
	/// MEASUREMENTS and coloured per pixel, built by the terrain's own thread. Copied here for the same reason the texels
	/// are — visage keys an image on its pointer. Empty until a map settles, and the texels are drawn until it does.
	std::shared_ptr<const TerrainSnapshot> terrainPixelsFrom_;
	/// `freshPixelSerial`, stamped where `terrainPixels_` and `terrainFine_` are copied in — so the atlas uploads the
	/// terrain once per publish rather than once per frame (visage `Image::serial`).
	uint64_t terrainPixelsSerial_ = 0;
	uint64_t terrainFineSerial_ = 0;
	/// THE CUBIC'S PICTURE when one exists, and which snapshot it came from. Its OWN `From` pointer
	/// rather than sharing the one above, because the fine buffer can appear or vanish while the
	/// snapshot pointer is unchanged — `TerrainMap::fineWanted` is live, so a map built before a flip
	/// carries no `fine` and the next one does. Sharing the guard would leave the old buffer on screen.

	std::vector<uint32_t> terrainFine_;
	std::shared_ptr<const TerrainSnapshot> terrainFineFrom_;

	/// What the hand has hold of, and what it had when it took hold. The departure point is what a
	/// breadcrumb records: the mock plants one where you LEFT, not where you arrived, so the trail is
	/// the places you have been rather than the place you are.
	Part grabbed_ = Part::None;
	std::optional<SoundIdentity> departure_;
	float grabY_ = 0.0f;
	double grabValue_ = 0.0;
	/// Whether the Rotate or Gamma corner's drag is geared fine (Shift) — see `regearCorner`.
	bool grabFine_ = false;
	/// Where the last corner drag event was, which is where a change of gearing re-anchors.
	float grabLastY_ = 0.0f;
	std::vector<clap_id> gesturesOpen_;
	bool driftGestureOpen_ = false;
	bool planeGestureOpen_ = false;
	bool gammaGestureOpen_ = false;

	/// **NOT SAVED WITH THE PATCH, AND WHY** (backlog NM): the crumbs (and undo) are session navigation, not sound — a
	/// Mutate's RESULT is saved, in the drift block. The dice live in the editor and start again at every open (deliberately
	/// not persisted).
	std::array<std::vector<SoundIdentity>, kMaxSounds> crumbs_;
	uint32_t diceState_ = 0x9E3779B9u;
	InkBox overlayInk_;
	/// **THE TERRAIN AS A PLANET** (backlog FE): its light, multiplied over the map, and its atmosphere, added over the
	/// map and the rim — see `drawTerrain` and `src/plugin/ui/shaders/`. One of each, so one batch of each per frame.
	TerrainGround* ground_ = nullptr;
	int groundPaints_ = 0;
	int stagePaints_ = 0;
	float bubbleRadiusShare_ = 0.0f;
	visage::Shader planetShade_ { resources::shaders::vs_planet, resources::shaders::fs_planet_shade, visage::BlendMode::Mult };
	visage::Shader planetAir_ { resources::shaders::vs_planet, resources::shaders::fs_planet_air, visage::BlendMode::Add };
	int likeMarksDrawn_ = 0;
	/// The likes as the terrain window carries them: shared and immutable, rebuilt only when the bridge's move (so a frame
	/// costs a comparison, not a copy).
	mutable std::shared_ptr<const std::vector<TerrainLike>> likesShared_;
	/// The likes' phase warp per sound and what each was derived from (`likeWarpFor`): the engine, the strength, the likes.
	struct LikeWarpKey {
		OscType engine = OscType::V5;
		double strength = 1.0;
		std::vector<TerrainLike> likes;
		friend bool operator==(const LikeWarpKey&, const LikeWarpKey&) = default;
	};
	mutable std::array<std::optional<LikeWarpKey>, kMaxSounds> likeKeys_ {};
	mutable std::array<feathers::LikeWarp, kMaxSounds> likeWarps_ {};
	double likeStrength_ = 1.0;
	/// THE LIKE-SET LOG (likes_log.h): an edit as its deltas (a snapshot first if the log describes another set).
	void logLikesEdit(const char* cause, const TerrainLikes& before, const TerrainLikes& after);
	/// A walk line for the stance being tracked, now.
	void logWalkNow(double now);
	/// The selected sound's stance as the walk tracks it: engine, sound, x, y.
	[[nodiscard]] std::array<double, 4> walkStance() const;
	std::function<double()> logClock_;
	/// The stance `logTick` tracks and since when it has been still; the one last logged, when it arrived there and when it
	/// left (unset while it is still there).
	std::optional<std::array<double, 4>> walkAt_;
	double walkSince_ = 0.0;
	std::optional<std::array<double, 4>> walkLogged_;
	double walkLoggedSince_ = 0.0;
	std::optional<double> walkLoggedLeft_;
	/// The like set the log has described (a snapshot plus its deltas); none until the first snapshot.
	std::optional<TerrainLikes> likesLogged_;
	[[nodiscard]] double logNow() const;
	[[nodiscard]] feathers::LikeWarp likeWarpFor(size_t slot, const TerrainWindow& window) const;
	int gridLabels_ = 0;
	float gridBow_ = 0.0f;
	/// What drawCorners last wrote in the Gamma corner, and where it left the compass — see
	/// gammaReadingDrawn. Sized like the buffer that formats it.
	char gammaReading_[32] = {};
	float gammaDial_ = 0.0f;
	visage::Point starDrawnAt_ {};
	/// The SECOND, smaller star: where the sound is PLAYED, drawn only when that is not where it was put
	/// — see drawStar. The main star (`starDrawnAt_`) stays on the place.
	visage::Point playedMarkAt_ {};
	bool playedMarkDrawn_ = false;
	/// The vector from the place to the played place — drawn only with both ends on the chart and far
	/// enough apart to have a shaft. Recorded where it is drawn, like the stars.
	visage::Point vectorFrom_ {};
	visage::Point vectorTip_ {};
	bool vectorDrawn_ = false;
	bool starDrawnOnChart_ = false;
	/// THE KIT'S OTHER PADS on the selected pad's chart (drum proposal §3.4), as the last paint drew them.
	std::array<PadMark, kMaxSounds> padMarks_ {};
	size_t padMarkCount_ = 0;

	/// The width knobs, one per slot — see widthKnobForTest above for why there are eight of them
	/// behind one visible control.
	std::array<std::unique_ptr<Knob>, kMaxSounds> widthKnobs_;
	/// The FM-depth knobs (doc §10), one per slot, in the same vertical stack as the width knobs.
	std::array<std::unique_ptr<Knob>, kMaxSounds> fmDepthKnobs_;
	/// **THE POLAR PAIR — STOCK KNOBS OVER A `TravelControl`, one per slot like every knob here**
	/// (2026-09-21). They were custom-drawn corners, and a custom-drawn corner answers no `display()`:
	/// it could not glow, take a dropped source, show a ring or open the routes menu — so Radius and
	/// Theta were destinations in the engine's table and in nothing a hand could reach (no mod glow
	/// appeared on them). Eight of each rather than one that follows the selection, because a
	/// destination that re-points itself under an armed modulator when the selection moves is a bug.
	std::array<std::unique_ptr<Knob>, kMaxSounds> radiusKnobs_;
	std::array<std::unique_ptr<Knob>, kMaxSounds> thetaKnobs_;
	/// Where the selected sound stood when the polar knobs were last told to repaint — they have no
	/// parameter id for the registry to notify them by, and a chart drag moves what they read.
	PolarOffset polarSeen_ {};
	/// The selected sound's pull as `update` last saw it — see the comparison there.
	double pullSeenRadius_ = 0.0;
	double pullSeenTheta_ = 0.0;
	/// The smoothness-override knobs (doc §11), one per slot, bottom of the flagship stack.
	std::array<std::unique_ptr<Knob>, kMaxSounds> warpKnobs_;
	std::array<std::unique_ptr<Knob>, kMaxSounds> smoothnessKnobs_;
	/// THE FIVE FIELD MODIFIER KNOBS, [kind][slot] — forty objects, of which five are ever visible.
	/// Same construction rule as the left stack: bound at construction, one per slot, selection is a
	/// visibility flip rather than a rebind, because a rebindable control is a control that can be
	/// pointing somewhere its label does not say.
	std::array<std::array<std::unique_ptr<Knob>, kMaxSounds>, kFieldKindCount> fieldKnobs_;
	/// RESONATE'S OWN SURROUND: on Resonate these take the slots of the per-sound knobs above
	/// (Width, FM Depth, Smoothness and the right flank's five), bound to the engine-wide Resonate params, so
	/// one of each rather than one per sound. Hidden on every other engine, where the per-sound knobs show.
	std::array<std::unique_ptr<Knob>, kResonateKnobCount> resonateKnobs_;
	/// BASIC SHAPES' MIX (2026-10-03): engine-wide, one of each, on the right flank's top four slots. Hidden on
	/// every other engine.
	std::array<std::unique_ptr<Knob>, kShapesKnobCount> shapesKnobs_;
	/// THE ADDITIVE SHAPE AMOUNT (2026-10-10), one per slot like Warp's, in Warp's slot; shown on Additive only.
	std::array<std::unique_ptr<Knob>, kMaxSounds> shapeAmountKnobs_;
	/// THE ADDITIVE BIAS KNOBS (2026-10-10), [kind][slot] like the field modifiers; shown on Additive only.
	std::array<std::array<std::unique_ptr<Knob>, kMaxSounds>, kAdditiveBiasCount> additiveBiasKnobs_;
	/// THE SLOT TABLE IN FORCE: the product's (`kSurroundSlots` in the .cpp), or a case's.
	SlotTable slots_;
	ShapesPage shapesPage_ = ShapesPage::Mix;
	/// Basic Shapes' row with the operator page up: the table's own row, the four mix knobs swapped for the four
	/// operators V5 seats in those slots. Rebuilt whenever the table is seated.
	SlotRow shapesOperatorsRow_ {};
	void rebuildShapesOperatorsRow();
	/// PLUCK'S STRINGS AND ENGINE-WIDE KNOBS, and its second page's row (the table's Pluck row, its left flank swapped).
	std::array<std::array<std::unique_ptr<Knob>, kMaxSounds>, kStringKindCount> stringKnobs_;
	std::array<std::unique_ptr<Knob>, kPluckKnobCount> pluckKnobs_;
	std::array<std::array<std::unique_ptr<Knob>, kMaxSounds>, kDrumFieldCount> drumFieldKnobs_;
	std::array<std::unique_ptr<Knob>, 2> drumWalkKnobs_;
	std::array<std::unique_ptr<Knob>, kDrumKitKnobCount> drumKitKnobs_;
	DrumPage drumPage_ = DrumPage::Pad;
	SlotRow drumKitFieldsRow_ {};
	SlotRow drumKitPlaceRow_ {};
	/// The Drum row as the mode and the selected kind leave it (currentSlotRow): the table's, less what they do not read.
	mutable SlotRow drumRow_ {};
	PluckPage pluckPage_ = PluckPage::String;
	SlotRow pluckPageTwoRow_ {};
	SlotRow pluckPageThreeRow_ {};
	/// THE SPECTRUM TAB, a child added LAST so it paints over (and takes the mouse before) every knob; whether the editor's
	/// panel is open (the caret).
	std::unique_ptr<SpectrumTab> spectrumTab_;
	std::unique_ptr<DrumTab> drumTab_;
	bool drumOpen_ = false;
	bool spectrumOpen_ = false;
};

} // namespace feathers::ui
