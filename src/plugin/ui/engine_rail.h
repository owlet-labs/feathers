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

#include "plugin/bridge_device_host.h"
#include "plugin/gui_bridge.h"
#include "plugin/ui/controls.h"
#include "plugin/ui/knob.h"
#include "plugin/ui/rail.h"
#include "plugin/ui/mod_panel.h"
#include "plugin/ui/mode_value_cell.h"
#include "plugin/ui/param_control.h"
#include "plugin/ui/notify_tray.h"
#include "plugin/ui/source_grid.h"
#include "plugin/ui/stack_index_plot.h"
#include "plugin/ui/tooltip.h"
#include "plugin/ui/widget_registry.h"

#include <memory>
#include <vector>
#include <visage/ui.h>

// The mock's Engine rail: the settings that belong to the instrument rather than to a sound.
//
// Every control here is real — voice mode, voice count, note priority, octave, semitone, bend
// range, glide and fine tune are all parameters the engine acts on today, so the panel is live.
// Glide and Fine are knobs rather than steppers because the mock makes them knobs, and it hosts
// them HERE rather than in the footer tray: they belong to the instrument, not to a sound.
//
// Enablement is decided by the ENGINE, not by the mock, and the difference has bitten once. A voice
// count means nothing to a monophonic synth, so it greys out unless the mode is Poly. Glide greys
// out IN Poly, because Synth::glideSecondsNow() hands the voices zero there whatever the parameter
// says. Note priority does NOT grey out anywhere: the mock greyed it in Poly and the mock was
// wrong — the engine feeds it to the voice manager's stealing priority in every mode, so in Poly it
// chooses which held note the user loses when a new one arrives. The rule is that a control is
// disabled iff moving it changes no sound, and the only way to know that is to read the engine.

namespace feathers::ui {

class EngineRail : public Rail, public TooltipHost {
public:
	EngineRail(ParamBridge& bridge, WidgetRegistry& registry, Notifier* notifier = nullptr);

	void draw(visage::Canvas& canvas) override;

	/// The rail's open width — see kWidth, which is what these contents need at our type sizes.
	float expandedWidth() const override { return kWidth; }

	/// The toggle is the only part of a closed rail, so it is tested before anything else the panel
	/// owns. Returns whether the press was the rail's own verb rather than a control's.
	bool pressCollapse(visage::Point pixels);

	/// GLIDE AND FINE ARE DESTINATIONS TOO — the mock's list is `['path','posx','posy','level','glide',
	/// 'fine']` and its last two knobs are hosted here rather than in the footer row. A panel that only
	/// reached the tray would have left two of the six unroutable with nothing on screen to say why.
	void setModulation(ModState* state, EditHistory* history);
	void resized() override;

	/// Re-read the voice mode and enable or disable what depends on it. Called on every frame the
	/// bridge reports a change, which is how it tracks automation as well as its own clicks.
	void refreshEnablement();

	/// Whether the two knobs are live, so a test can ask rather than read pixels.
	bool glideEnabled() const noexcept;
	bool fineEnabled() const noexcept;
	[[nodiscard]] Knob* phaseForTest() const noexcept { return phase_.get(); }
	/// Phase Width (drawn on every engine since backlog OZ) and Osc Mode (hidden on Additive, backlog LY), for the case
	/// that asks whether they are drawn.
	[[nodiscard]] visage::Frame* phaseWidthForTest() const noexcept { return phaseWidth_.get(); }
	/// **THE ENGINE CHOICE, AT THE TOP OF THE RAIL (backlog QH: 2026-11-07).** It lived in the header bar; the rail
	/// that is named for it holds it now. A `ValueBox` over `ParamId::OscType`, whose press opens the descriptor's choice
	/// list (every engine visible before one is applied). `selectEngine` is what a pick does, named so a case can exercise
	/// the effect without a popup; `engineDisplayName` is what the collapsed rail's label wears.
	[[nodiscard]] ValueBox* engineBoxForTest() const noexcept { return engineBox_.get(); }
	void selectEngine(int index);
	static std::string engineDisplayName(int index);
	[[nodiscard]] visage::Frame* oscModeForTest() const noexcept { return oscMode_.get(); }
	/// The voice section's controls, for the drum audit's visibility cases (drum proposal §15).
	[[nodiscard]] visage::Frame* voiceModeForTest() const noexcept { return voiceMode_.get(); }
	[[nodiscard]] visage::Frame* voiceCountForTest() const noexcept { return voiceCount_.get(); }
	[[nodiscard]] visage::Frame* notePriorityForTest() const noexcept { return notePriority_.get(); }
	[[nodiscard]] visage::Frame* glideForTest() const noexcept { return glide_.get(); }
	[[nodiscard]] visage::Frame* transposeOctForTest() const noexcept { return transposeOct_.get(); }
	[[nodiscard]] visage::Frame* bendRangeForTest() const noexcept { return bendRange_.get(); }
	[[nodiscard]] visage::Frame* stackCountForTest() const noexcept { return stackCount_.get(); }

	//-------------------------//
	// THE PITCH | STACK PAGES //
	//-------------------------//

	/// **THE SECTION UNDER VOICE HAS TWO PAGES, PITCH AND STACK** (backlog KE step 3; voice-stacking doc §7,
	/// assumption 18): the Stack Index's Shape, Mapping and Curve need a knob row and a half more than this rail
	/// has, so they share the pitch section's place, and the heading names both pages — the one shown bright, the
	/// other a click away. Both names are on the face, so the choice is seen before it is made. Pitch is the page a
	/// fresh editor shows.
	enum class Page { Pitch, Stack };
	[[nodiscard]] Page page() const noexcept { return page_; }
	void showPage(Page page);
	/// Reopening the rail shows every child; the page not turned to is hidden again.
	void collapsedChanged() override;

	/// **THE ENGINE RAIL HOSTS ITS OWN TOOLTIPS** (voice stacking step 5): every control on it declares its text, and a
	/// card was the only thing that could show one. A layer on top of the rail, as `FxCard` has; `updateTooltips` is
	/// its per-frame clock, driven from the editor's tick. Public so a case can read what it shows.
	TooltipLayer& tooltipLayer() override { return tips_; }
	bool updateTooltips() { return tips_.advance(); }
	/// **THE ROUTE LIST'S HEIGHT, FOLLOWED (backlog LV).** The list grows a row per route, and nothing about a route
	/// being made, removed or a source being armed reaches this panel's layout — so the list kept whatever height
	/// the last resize gave it and visage clipped every row past it, so only one route was listed. Per frame from the
	/// editor's tick, `GenericCard::updateContent`'s shape: re-lays out only when the
	/// height the list asks for differs from the one it was placed at. True when it re-laid out.
	bool updateLayout();
	/// How many times a walk has PLACED the controls — so a case can say a tick with nothing changed placed nothing.
	[[nodiscard]] size_t contentRelayoutsForTest() const noexcept { return placements_; }
	/// Where each page's name sits in the heading row, so a test can click it rather than aim.
	[[nodiscard]] visage::Bounds pageTabBounds(Page page) const;
	/// The stack page's controls, so a test can ask about them rather than read pixels.
	[[nodiscard]] ModeValueCell* stackShapeCellForTest() const noexcept { return stackShape_.get(); }
	[[nodiscard]] ModeValueCell* stackMapCellForTest() const noexcept { return stackMap_.get(); }
	[[nodiscard]] Knob* stackCurveForTest() const noexcept { return stackCurve_.get(); }
	[[nodiscard]] StackIndexPlot* stackPlotForTest() const noexcept { return stackPlot_.get(); }
	/// The Stack Index's arrow on the stack page (backlog MO); null until setModulation, as the grid is.
	[[nodiscard]] StackIndexArrow* stackArrowForTest() const noexcept { return stackArrow_.get(); }

	//--------//
	// SCROLL //
	//--------//

	/// IT SCROLLS NOW, on the modulator strip's own terms and for the reason the strip has them: this
	/// rail's contents can be taller than the rail. The editor is resizable with no constraint of its
	/// own ("whatever the host offers is a size it can be drawn at", feathers_clap.cpp), so the height
	/// is the host's to choose, and the rail cannot answer a short window by making the instrument
	/// smaller.
	///
	/// FOUND BY A CAPTURE RATHER THAN BY A CASE, which is the part worth keeping. The arrow grid fitted
	/// at the design canvas and every case passed; at 1200x660 — a size a host reaches by dragging an
	/// edge, and the one guihost happens to leave behind — the routings list was off the bottom of the
	/// rail, still laid out, still hit-testable by arithmetic, and unreachable by a hand. The section
	/// was new but the weakness was not: a full route list overflowed this rail before the grid existed.
	///
	/// At the design canvas maxScroll() is zero and nothing here does anything, which is the state
	/// every existing case is written against.
	float contentHeight() const;
	float scroll() const noexcept { return scroll_; }
	float maxScroll() const;
	void setScroll(float y);
	bool mouseWheel(const visage::MouseEvent& e) override;

	/// The panel's width in logical pixels. OURS, not the mock's: the mock's rail is `auto` in a
	/// `grid-template-columns: auto auto 206px minmax(0,1fr) auto`, so it has no fixed width to
	/// copy — it is as wide as its contents make it. This is the width those contents need at our
	/// type sizes, and it is a number to revisit when the panel's contents change rather than a
	/// measurement to defend.
	static constexpr float kWidth = 168.0f;

private:
	/// **THE SUBSCRIPTION, HELD** — see `ui::Watch`. Declared LAST so it is destroyed FIRST, which
	/// releases into a list that is still alive.
	Watch watch_;
	/// AND STACK, because the stack's CPU guard caps what Voice Count can do (backlog KE) — the stepper's
	/// ceiling is redrawn whenever Stack moves, by automation and preset loads as much as by a hand.
	Watch stackWatch_;
	/// AND THE ENGINE, because Phase greys on Resonate.
	Watch oscTypeWatch_;
	Watch drumModeWatch_;
	/// THE STACK PAGE'S PARAMETERS, each watched by the control that shows it (step 3).
	std::vector<Watch> stackIndexWatches_;
	/// AND THE MODULATION STATE, because Phase Width greys while no route reaches Phase.
	Watch modWatch_;
	/// Walk the panel top to bottom. With `place`, it positions every control; either way it returns
	/// the y the "Pitch" heading belongs at, so the headings and the controls cannot drift apart.
	float layOutInto(bool place);

	ParamBridge& bridge_;
	/// THE BRIDGE BEHIND THE HOST CONTRACT, for the list (rack roadmap §7f: one `RouteList` over
	/// `backplane::DeviceHost` in both hosts). Stateless over `bridge_`, so a second adapter beside the
	/// editor's own costs nothing. Declared here, before the list, so it outlives it.
	BridgeDeviceHost host_;
	/// Kept so the route list can be given a voice when it is created, which happens after construction.
	Notifier* notifier_ = nullptr;
	/// Zero at the design canvas, and zero is the whole editor as it was.
	float scroll_ = 0.0f;
	/// What the last walk measured the contents at, so contentHeight() answers without laying out
	/// again. Written by layOutInto, which is the one place that knows how tall anything is.
	mutable float contentHeight_ = 0.0f;
	/// Walks that placed the controls; `contentRelayoutsForTest`.
	size_t placements_ = 0;
	/// The height the route list was last PLACED at, which `updateLayout` compares against what it asks for now.
	float placedRoutesHeight_ = 0.0f;

	std::unique_ptr<SegmentedControl> voiceMode_;
	std::unique_ptr<Stepper> voiceCount_;
	/// HOW MANY OSCILLATORS A NOTE STACKS, how far apart in pitch, how wide in stereo (backlog KE): one row of three
	/// knobs under Voice Count, because the count and Voice Count trade against each other through the CPU guard.
	/// The zone knobs join them in a later step (voice-stacking doc §5).
	std::unique_ptr<Knob> stackCount_;
	std::unique_ptr<Knob> stackDetune_;
	std::unique_ptr<Knob> stackSpread_;
	std::unique_ptr<SegmentedControl> notePriority_;
	/// V21d: "enabling phase catchup is an Engine rail option: near voice retrig options." Always
	/// enabled — it changes what a retrigger sounds like in every mode, poly steals included.
	std::unique_ptr<SegmentedControl> oscMode_;
	std::unique_ptr<Stepper> transposeOct_;
	std::unique_ptr<Stepper> transposeSemi_;
	std::unique_ptr<Stepper> bendRange_;
	std::unique_ptr<Knob> glide_;
	/// THE SOURCES THAT ARE NOT CARDS — the per-sound weight arrows, in a section directly above the
	/// routings, which is the chosen placement and the one that dissolves the perform-versus-explore
	/// question by belonging to neither stage. Null until setModulation, on the same terms as the list.
	std::unique_ptr<SourceGrid> sources_;
	/// THE FOCUSED SOURCE'S ROUTINGS, at the foot of the rail — the mock puts `#inspRoutes` here rather
	/// than in the modulator strip, and it is the right place for the same reason the strip is on the
	/// left: the list is about the source the strip has focused, and the two sit side by side. Null
	/// until setModulation is called, so an Engine rail in an editor with no panel is the one it was.
	std::unique_ptr<RouteList> routes_;
	std::unique_ptr<Knob> fine_;
	/// Coarse (ParamId::Coarse): Fine Tune's road in semitones, beside it in the pitch row.
	std::unique_ptr<Knob> coarse_;
	/// The oscillator's read phase (ParamId::OscPhase) — greyed on Resonate, whose string has no cycle to move.
	std::unique_ptr<Knob> phase_;
	/// Phase Width (Q10): it mirrors Phase's MODULATION, so it greys while no route reaches Phase — and on Resonate.
	std::unique_ptr<ParamControl> engineControl_;
	std::unique_ptr<ValueBox> engineBox_;
	std::unique_ptr<ParamControl> phaseWidthControl_;
	std::unique_ptr<ValueBox> phaseWidth_;
	bool phaseRouted() const;

	/// THE STACK PAGE (step 3): the Shape and Mapping zone knobs, each a mode and the amount that mode reads, and the
	/// Curve. The cells borrow their controls, so the controls are owned here and declared before the cells.
	Page page_ = Page::Pitch;
	std::unique_ptr<ParamControl> stackShapeMode_;
	std::unique_ptr<ParamControl> stackShapeAmount_;
	std::unique_ptr<ParamControl> stackFreePhase_;
	std::unique_ptr<ParamControl> stackMapMode_;
	std::unique_ptr<ParamControl> stackMapAmount_;
	std::unique_ptr<ModeValueCell> stackShape_;
	std::unique_ptr<ModeValueCell> stackMap_;
	std::unique_ptr<Knob> stackCurve_;
	/// The index those three make, drawn beside the Shape (step 5).
	std::unique_ptr<StackIndexPlot> stackPlot_;
	/// **THE INDEX'S OWN ARROW, UNDER THE CONTROLS THAT MAKE IT** (backlog MO: "stack index mod to move
	/// by the stack controls"). It was the source grid's last row; here it reads as part of the stack. Null until
	/// setModulation, on the grid's terms.
	std::unique_ptr<StackIndexArrow> stackArrow_;
	/// Show the page turned to and hide the other.
	void applyPageVisibility();
	/// Osc Mode is not drawn on Additive (backlog LY): the page decides Phase Width, the engine and the page Osc Mode.
	void applyEngineVisibility();
	[[nodiscard]] bool additiveEngine() const;
	/// The y the pages' heading row sits at, from the last walk.
	float pageHeadingY_ = 0.0f;
	/// THE TOOLTIP LAYER, a plain member as the card's is; on top of every child by its own flag.
	TooltipLayer tips_;
};

} // namespace feathers::ui
