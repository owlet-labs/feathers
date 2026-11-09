/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even
 * the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
 * Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

// PHOTOGRAPH AN EXPANDED PANEL — the venue for looking at a widget that lives only on one.
//
//   build/widget-shot OWLTT out/panel.png
//   build/widget-shot SHPR out/shaper.png
//
// THE GAP THIS FILLS, and it is narrow on purpose. `guihost` opens a real window but holds the
// plugin across the CLAP boundary, so the only input it has is the PARAMETER API — and a panel is
// not a parameter. `test_gui_widgets` reaches the tree and asserts geometry but opens no window, so
// nothing is rasterised. Neither can answer "does this widget LOOK right", which is the only
// question a visual change actually raises.
//
// **AND THE OBVIOUS BRIDGE IS BANNED.** X11 pointer synthesis would let guihost click the card's
// expand mark, and `windowed-probe.cpp` says plainly why it must not: *"X11 pointer synthesis would
// bridge the two and is BANNED on this display, which is the real desktop."* It was done anyway
// before that line was read — five runs of `xdotool click` on :1, moving the real pointer — and
// this tool exists so that nobody, me included, has a reason to reach for it again.
//
// So the panel is opened IN-PROCESS, through the road the card's mark actually takes:
// `Gui::fxLaneForTest()->onExpandDevice(slot)`, the hook `gui.h` already exposes for exactly this.
// No synthetic input, no pointer moved, nothing touched outside this process's own window.

#include "fx/filter/filter_types.h"
#include "fx/owltt/owltt_types.h"
#include "fx/automod/automod_types.h"
#include "mod/lfo/lfo_types.h"
#include "plugin/gui.h"
#include "plugin/ui/explore_stage.h"
#include "plugin/ui/rail.h"
#include "plugin/ui/expanded_overlay.h"
#include "host_common.h"
#include "plugin/gui_bridge.h"
#include "plugin/params.h"
#include "plugin/ui/automod_card.h"
#include "fx/busutil/busutil_meter.h"
#include "plugin/ui/fx_lane.h"
#include "plugin/ui/chrome_hue.h"
#include "plugin/ui/header_bar.h"
#include "plugin/ui/stereo_tool_card.h" // `--stereo-tab`, `--stereo-sweep` (backlog KU)
#include "plugin/ui/twinfield_card.h"   // `--twinfield-tab`
#include "mod/twinfield/twinfield.h"       // `--twinfield-play`
#include "plugin/ui/sample_hold_card.h" // `--snh-sweep` (backlog MW)
#include "mod/samplehold/samplehold.h"
#include "mod/arp/arp.h" // `--arp-play`
#include "plugin/ui/pattern_card.h"     // `--pattern-focus`
#include "plugin/ui/pattern_expanded.h" // `--pattern-like`, `--pattern-focus`
#include "plugin/ui/mod_panel.h"
#include "plugin/ui/route_list.h"
#include "plugin/ui/chain_column.h"
#include "plugin/ui/sklitter_expanded.h"
#include "plugin/ui/resonate_expanded.h"
#include "fx/resonate/resonate_lanes.h"
#include "fx/resonate/resonate_types.h"
#include "fx/sklitter/sklitter.h"
#include "fx/sklitter/perform.h" // waypointAxis — `--sklitter-play`'s puck
#include "fx/sklitter/sklitter_types.h" // kDescriptor — `adopt` fans out to BOTH lanes
#include "rack/inner_chain.h"
#include "rack/chain_record.h"
#include "rack/site_chains.h"
#include "rack/site_expanded.h"
#include "rack/parallel_rack.h" // `--rack-chains` (backlog OC)
#include "plugin/ui/parallel_rack_expanded.h"
#include "rack/chain_expanded.h" // `--chain-window`
#include "rack/rack_device.h"
#include "rack/rack_editor.h"
#include "plugin/device_registry.h"
#include "plugin/ui/expanded_views.h" // `--device-list`
#include "plugin/ui/zone_coord_cell.h"
#include "plugin/ui/value_box.h"
#include "plugin/ui/fader_pill.h" // `--hover` reaches a pill (backlog IL)
#include "plugin/ui/knob.h"
#include "plugin/ui/switch_chip.h"
#include "plugin/ui/trig_pad.h" // `--trig-press` (backlog MV)
#include "plugin/ui/env_card.h" // `--env-peak-hover` (backlog MN)
#include "plugin/ui/stepseq_card.h" // `--stepseq-randomise` (backlog MX)
#include "plugin/ui/tooltip.h"
#include "plugin/ui/menu_anchor.h"
#include "plugin/ui/owltt_expanded.h"
#include "plugin/ui/engine_rail.h"
#include "plugin/ui/windowed_expanded.h"
#include "mod/stereo/stereo_types.h" // `--arm-pair`: the Stereo Tool armed over the shot device (backlog KU)

#include <X11/Xlib.h>

#include <array>
#include <algorithm>
#include <cmath>
#include <limits>
#include <chrono>
#include <numbers>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <utility>
#include <vector>
#include <unistd.h>

using namespace feathers;


namespace {

struct ShotGuiHost : GuiHost {
	void guiRequestFlush() noexcept override {}
	void guiRequestResize(uint32_t, uint32_t) noexcept override {}
	void guiCaptureState(clap_ostream_t*) noexcept override {}
	bool guiLoadState(clap_istream_t*) noexcept override { return false; }
};

/// THE WINDOW WHOSE PIXELS ARE THE EDITOR'S, which is the CHILD the plugin created inside the
/// container — not the container. `guihost` and `windowed-probe` both resolve it this way, and
/// capturing the parent instead is how a grab comes back as the desktop.
Window captureTarget(Display* display, Window parent) {
	Window root = 0;
	Window above = 0;
	Window* kids = nullptr;
	unsigned int count = 0;
	Window target = parent;
	if (XQueryTree(display, parent, &root, &above, &kids, &count) != 0 && count > 0) {
		target = kids[0];
	}
	if (kids != nullptr) {
		XFree(kids);
	}
	return target;
}

/// The first frame of type `T` anywhere under `root`, or null. Depth-first over the real tree, which
/// is the only thing that cannot be wrong about which panel is on screen — see `--rails` for why the
/// overlay's own accessor was not enough.
template <typename T>
T* findInTree(visage::Frame* root) {
	if (root == nullptr) {
		return nullptr;
	}
	if (auto* hit = dynamic_cast<T*>(root)) {
		return hit;
	}
	for (visage::Frame* child : root->children()) {
		if (T* hit = findInTree<T>(child)) {
			return hit;
		}
	}
	return nullptr;
}

template<typename T>
void allInTree(visage::Frame* root, std::vector<T*>& out) {
	if (root == nullptr) {
		return;
	}
	if (auto* hit = dynamic_cast<T*>(root)) {
		out.push_back(hit);
	}
	for (visage::Frame* child : root->children()) {
		allInTree<T>(child, out);
	}
}

} // namespace

int main(int argc, char** argv) {
	// BEFORE ANYTHING LOADS — see `toolhost::isolateUserConfig`. A test host must not write
	// the files the installed plugin reads.
	feathers::toolhost::isolateUserConfig("widget-shot");
	// `--device-list`: THE REGISTRY AS DATA, for `scripts/readme-device-list.sh` (the README's device list), and no
	// window. One tab-separated line per device in `kDevices` order — `device`, category rank (menu order) and heading, tag, name, whether
	// it has an expanded window, modulatable controls, outputs — then one `engine` line per oscillator type. Asked
	// of the same table and the same `ui::hasExpandedView` the editor uses, so the list cannot drift from the build.
	if (argc == 2 && std::strcmp(argv[1], "--device-list") == 0) {
		rack::registerChainExpandedView(); // as every GUI that hosts chains does at creation, so the Chain answers too
		for (const device::Descriptor& d : kDevices) {
			std::printf("device\t%zu\t%s\t%s\t%s\t%s\t%zu\t%zu\n", device::categoryRank(d.category),
			            device::categoryName(d.category), d.traits.tag,
			            d.traits.name, ui::hasExpandedView(d) ? "window" : "card", d.modulatableCount(),
			            d.outputs.size());
		}
		for (const char* engine : kOscTypeNames) {
			std::printf("engine\t%s\n", engine);
		}
		return 0;
	}
	if (argc < 3) {
		std::printf("usage: %s <device tag> <out.png> [\"<control name>=<value>\"]...\n"
		            "       [--card]\n"
		            "       [--hue <degrees>]  (the chrome hue, backlog LG, set once the editor is open)\n"
		            "       [--accent-hue <degrees>]  (the accent hue: the roles turn by chrome + accent)\n"
		            "       [--settings]  (open the settings screen through the header's Libra mark before the grab)\n"
		            "       [--host \"<parameter name>=<value>\"]...  (seed a HOST parameter — Stack, Stack Shape —\n"
		            "         before the editor opens; the unquoted form is the device's controls)\n"
		            "       [--stack-route <amount>]  (route the Stack Index into Fine Tune at that amount)\n"
		            "       [--engine-rail-page pitch|stack]  (turn the Engine rail's page before the grab)\n"
		            "       [--preview lfo|dual[+band]] [--grid uniform|compact] [--phase 0..1]\n"
		            "       [--envelope 0..1] [--readings] [--clip <seconds since over>]\n"
		            "       [--meter <r0>,<r1>,...]  (publish these as the device's readings, in order)\n"
		            "       [--output-peak <dBFS>]  (every card's courtesy output meter; others 6 dB down per step)\n"
		            "       [--fold]  (with --card: the card folded before the grab)\n"
		            "       [--modulated \"<control name>=<value>\"]...  (publish where modulation left a control)\n"
		            "       [--modulated-r \"<control name>=<value>\"]...  (and its RIGHT ear: a stereo route, KU)\n"
		            "       [--arm-pair]  (add a Stereo Tool to the strip and arm its Split output: the stereo-tinted glow)\n"
		            "       [--arm-macro]  (focus and arm Macro 1: the mapping glow over every destination it may reach)\n"
		            "       [--stereo-tab 0|1]  (the STEREO card's tab: Split, Width)\n"
		            "       [--twinfield-tab 0|1]  (the TFIELD card's tab: A, B)\n"
		            "       [--twinfield-play]  (run the TFIELD offline; its 2x4 output block shows values)\n"
		            "       [--stereo-sweep]  (publish 4 s of a stereo route on STEREO's In and Amount, a frame at a time)\n"
		            "       [--state <index>=<value>]  (one float of the device's extra state, by offset)\n"
		            "       [--sklitter-sparks <wet>:<grain>] [--spark-cpu]  (publish SKLITTER's two spark\n"
		            "         levels, so the perform circle has something to emit — it is silent without\n"
		            "         audio; --spark-cpu draws them through the CPU reference path instead of\n"
		            "         vs_spark.sc, which is the A/B that tests the shader)\n"
		            "       [--sklitter-onsets <grains/s>]  (with --sklitter-sparks: advance the grain onset\n"
		            "         counters through all four gate classes, for the larger onset sparks)\n"
		            "       [--sklitter-repeats <period s>]  (with --sklitter-sparks: one Repeat restart per\n"
		            "         period, published with it — the splitting repeat sparks)\n"
		            "       [--rack] [--playhead <ordinal>:<phase>] [--loop]\n"
		            "       [--arp-play]  (with ARP: run it offline at its row, a chord latched at 140 bpm, publishing its steps)\n"
		            "       [--pattern-like]  (with PAT's panel: record a like for the focused pattern, so the terrain lights)\n"
		            "       [--pattern-focus 1-4]  (with PAT: focus that tab on the card and, if open, the window)\n"
		            "       [--height <logical px>]  (a taller editor, so a tall card is photographed whole)\n"
		            "       [--pattern-edit <step>]...  (with PAT's panel: click that step of the window's lane — a hand edit)\n"
		            "       [--audio <seconds>]  (run the lane device's own DSP over a held, re-struck sawtooth chord for\n"
		            "         that long, its readings published as the plugin's lane does: live meters, no \"no signal\")\n"
		            "       [--mod-run <seconds>]  (with --rack: run the rack modulator's own ticks that long under a struck\n"
		            "         gate — and, for one that listens to audio, the same chord — publishing its playhead each frame)\n"
		            "       [--snh-sweep]  (with --rack SNH: run the device offline at its row, publishing its hold\n"
		            "         each frame, so the Sample & Hold card's scope draws a real staircase)\n"
		            "         NB --rack seeds BEFORE the editor opens, so the strip is laid out twice\n"
		            "         and any fault in the pass that BUILDS a card is repaired before the grab\n"
		            "       [--rack-add <TAG>]  (add via the strip's (+) AFTER the editor is open —\n"
		            "         the only venue that can photograph that pass; see the block in this file)\n"
		            "       [--rack-scroll <px>|max]  (scroll the modulator strip before the grab)\n"
		            "       [--rack-set \"<control name>=<value>\"]...  (on the --rack-add device, once added)\n"
		            "       [--rack-scroll-end]  (scroll the strip to its end, so the added card is whole)\n"
		            "       [--trig-press]  (hold every Trig pad, so the shot shows it lit)\n"
		            "       [--env-peak-hover]  (an Opt-hover on every DAHDSR card's peak: the bend's highlight, MN)\n"
		            "       [--stepseq-randomise <vgprs>]  (Randomise those lanes on every step sequencer card)\n"
		            "       [--rack-menu \"<control name>\"]  (open that strip box's Choice menu)\n"
		            "       [--owltt-mode-menu]  (open OWLTT's Mode menu on its panel)\n"
		            "       [--popout]  (the panel in its own window; the screen is saved as <out>-popout.png)\n"
		            "       [--rails none|circle|fx|grain|post|both]  (SKLITTER's three expansion rails;\n"
		            "         \"fx\" and \"both\" mean every fx lane, as they always have)\n"
		            "       [--body open|collapsed]  (the panel's own control grid, which folds too)\n"
		            "       [--chain-add <TAG>]...  (into SKLITTER's MASTER lane; repeatable)\n"
		            "       [--grain-add <TAG>]...  (into SKLITTER's GRAIN lane; repeatable)\n"
		            "       [--chain-live]  (bind SKLITTER's post chain, empty)\n"
		            "       [--chain-expand <position>]...  (open that chain device's panel; repeatable)\n"
		            "       [--chain-drag <position>]  (pick that master-lane card up by its head and hold\n"
		            "         it over the gap past the last card — the bumper, mid-drag)\n"
		            "       [--chain-at-wall]  (put the chain at the nesting limit and open its (+) menu)\n"
		            "       [--fx-add-menu <heading>]  (open the fx lane's (+) menu and hover that category\n"
		            "         heading open, 0-based; -1 hovers nothing)\n"
		            "       [--panel-grow <dx>x<dy>]  (drag the open panel's resize grip)\n"
		            "       [--then \"<control name>=<value>\"]...  (set it AFTER the panel is open, and\n"
		            "         let the editor's tick notice — a plain \"name=value\" seeds it BEFORE)\n"
		            "       [--macro <1..4>=<Knob|Latch|Momentary>[:<name>[:<value 0..1>]]]...  (seed a\n"
		            "         header macro's type, name and value BEFORE the editor opens — the MacroSlot)\n"
		            "       [--label <output 1..N>=<text>]...  (relabel the seeded device's output on this\n"
		            "         instance — the MACROS card's rename; \"name=value\" sets reach --rack devices too)\n"
		            "       [--expose \"<control name>\"]...  (pin that control of the seeded device to a host\n"
		            "         slot — the right-click's \"Expose to host\" — so its knob draws the host mark)\n",
		            argv[0]);
		return 2;
	}
	const char* tag = argv[1];
	const std::string out = argv[2];

	// A STATE TO PHOTOGRAPH IN, because a widget at its defaults is the one state that says least.
	// Every number this editor draws is zero out of the box, so a shot of the defaults cannot answer
	// "is `12 : 96` legible at 1.75x" — which is the question a coordinate readout actually raises.
	// Named rather than indexed, the way guihost's `--dset` is: a renumbered control then fails
	// visibly instead of quietly setting its neighbour.
	std::vector<std::pair<std::string, double>> wanted;
	/// `--host`: HOST parameters by their declared name, seeded before the editor opens (voice stacking, KE step 3).
	std::vector<std::pair<std::string, double>> hostWanted;
	/// `--stack-route`: the Stack Index into Fine Tune at this amount; 0 routes nothing.
	float stackRoute = 0.0f;
	/// `--route-curves`: Macro 1 routed to three parameters through three curves, armed, so the Engine rail's route
	/// list draws its transfer chips; `--route-curves-menu` also opens
	/// the first row's chip menu, through the road the chip's press takes. `--route-curves-bipolar` switches
	/// Macro 1 bipolar first, so the rectifiers are offered.
	bool routeCurves = false;
	bool routeCurvesMenu = false;
	bool routeCurvesBipolar = false;
	float routePhiOctaves = std::numeric_limits<float>::quiet_NaN();
	float routeExpExponent = std::numeric_limits<float>::quiet_NaN();
	float routeLogExponent = std::numeric_limits<float>::quiet_NaN();
	/// `--engine-rail-page`: which page the Engine rail shows under Voice; empty leaves the default.
	std::string engineRailPage;
	/// `--state <index>=<value>`: floats of the device's state blob past its params (its `Traits::extraState`),
	/// by offset within the blob — the EQ's band channels and analyser view have no control name to set.
	std::vector<std::pair<size_t, float>> stateWanted;
	/// `--control <index>=<value>`: one DECLARED control of the shot device, through the coercing road a card writes
	/// (`setDeviceControl`) — `--state` writes the opaque blob past the params instead (backlog NL's mode shots).
	std::vector<std::pair<size_t, float>> controlWanted;
	// **`--card` IS FOR A WIDGET THAT LIVES ON THE CARD RATHER THAN ON A PANEL**, and automod is the
	// first: its `hasExpanded` is false — declined deliberately — so `onExpandDevice` would either
	// reach nothing or photograph a surface the device does not have. The editor's own window, with
	// the lane holding the card, IS the venue for it.
	bool expand = true;
	bool spectrum = false;
	/// `--drum-kit`: the drum engine's kit panel, opened by its tab's road (seed `--host "Osc Type=7"`).
	bool drumKit = false;
	/// `--width W` (logical): the editor's width, as `--height` sets its height — a large canvas for the terrain.
	uint32_t shotWidth = 0;
	/// `--explore-zooms z1,z2,…` (terrain-render-bugs): the Explore surface, then one grab per zoom stop
	/// (`<out stem>-z<i>.png`), each with the terrain's placement, the disk and the ground frame's native size printed —
	/// the picture's aspect by value. Implies `--explore`. `--explore-pan x,y` moves the view by that many view spans
	/// first (the selected sound's place, not the view, is what is stored, so this drags the view like a hand).
	std::vector<double> exploreZooms;
	/// `--explore-zoom-pump ms`: how long each zoom stop is pumped before its grab (default 1500) — short catches a zoom
	/// IN FLIGHT, the picture of the old window scaled while the new one builds.
	int exploreZoomPump = 1500;
	/// `--explore-time N` (terrain-render-bugs): N zoom steps of x1.06 (in for N/2, out for N/2), one per 40 ms of wall
	/// time, timing the editor's own `processEvents` (where a frame is drawn) and counting ground and stage paints and
	/// terrain builds.
	int exploreTimeSteps = 0;
	/// `--resize-to WxH` + `--resize-steps n` (terrain-render-bugs): after the Explore surface is up, resize the editor
	/// from its starting size to WxH (logical) in n equal steps (1 = one jump, as a maximise delivers it), 30 ms apart,
	/// then settle and report the stage, the terrain's placement against the stage, and whether the snapshot's window is
	/// the one the stage would ask for now — the stale-bake question, by value. One grab, `<stem>-resized.png`.
	uint32_t resizeToW = 0;
	uint32_t resizeToH = 0;
	int resizeSteps = 1;
	/// `--collapse-rails`: after the Explore surface (and any resize), collapse every visible `Rail` one at a time,
	/// cumulatively, then expand them back, reporting the stage and whether the terrain overhangs it after each.
	bool collapseRails = false;
	/// `--radius-route <base>:<depth>[:bi]` (radius-mod-jump, 2026-11-07): sound 1's Radius at `base` travel units, a
	/// route from Macro 1 into it at `depth`, Macro 1 bipolar with `:bi`, the source armed — so the Radius knob's ring
	/// (which a route now draws ALONG THE FACE) can be looked at. Implies `--explore`.
	double radiusRouteBase = -1.0;
	float radiusRouteDepth = 0.0f;
	bool radiusRouteBipolar = false;
	/// `--explore`: the Explore surface, by the header switch's own road, before the grab.
	bool exploreSurface = false;
	/// `--likes N`: N likes made on the Explore stage by its own verbs (backlog ON) — a dice roll, then the heart — so the
	/// marks and the lit heart can be photographed.
	int likes = 0;
	std::string preview;
	std::string gridArg;
	double phaseArg = 0.0;
	/// THE ENVELOPE THE STRIP DRAWS ITS TRACE AT. Defaulted mid-scale rather than 0 so `--phase`
	/// alone photographs a visible trace; `--envelope` sets it explicitly.
	double envArg = 0.5;
	bool havePhase = false;
	/// BUS UTILITY'S METER, AND THE AGE ITS LED DRAWS FROM. See the publish block below.
	bool haveReadings = false;
	/// `--hue <degrees>`: the chrome hue (backlog LG), set once the editor is open, as the settings screen sets it.
	double hueArg = 0.0;
	/// `--accent-hue <degrees>`: the accent hue, the second axis, set beside `--hue`.
	double accentHueArg = 0.0;
	/// `--settings`: open the settings screen (backlog LG) through the header's door before the grab.
	bool settingsArg = false;
	/// **ANY DEVICE'S READINGS, VERBATIM** — `--meter <r0>,<r1>,...` publishes these as the device's readings
	/// in declared order (COMP's In/Out/GR, CLIP's In/Out/Over). `--readings` is Bus Utility's fixed set.
	std::vector<float> meterArg;
	/// `--output-peak <dBFS>`: every lane position's courtesy output meter (backlog MK) published at this level, the
	/// shot device's exactly and each other position 6 dB further down per step, so one shot shows several levels.
	/// `--fold`: the shot device's card folded, through the card's own verb, before the grab (`--card` only).
	double outputPeakDb = 0.0;
	bool haveOutputPeak = false;
	bool foldArg = false;
	/// **`--modulated "<control name>=<value>"` — WHERE MODULATION LEFT A CONTROL**, published on the bridge's modulated
	/// channel as the audio side would (`publishModulated`), so a surface drawing the MODULATED value — Resonate's
	/// puck — can be photographed modulated without an engine running. The base is left where it is.
	std::vector<std::pair<std::string, double>> modulatedArg;
	/// `--modulated-r`: the RIGHT ear of a stereo-live control (backlog KU), published with its mask — a split ring.
	std::vector<std::pair<std::string, double>> modulatedRArg;
	/// `--arm-pair`: a Stereo Tool in the strip, its Split output focused and armed — the stereo-tinted glow.
	bool armPair = false;
	/// `--arm-macro`: Macro 1 focused and armed — the plain (mono) mapping glow over every destination it may reach.
	bool armMacro = false;
	/// `--stereo-tab <0|1>`: turn the Stereo Tool card's tab (Split, Width) through the card's own verb.
	int stereoTab = -1;
	/// `--twinfield-tab <0|1>`: turn the Twin Field card's tab (A, B) through the card's own verb.
	int twinfieldTab = -1;
	/// `--twinfield-play`: run the TFIELD offline and publish its orbit phases, so the 2x4 output block shows values.
	bool twinfieldPlay = false;
	/// `--stereo-sweep`: four seconds of an engine-shaped stereo route — In's two ears in quadrature, Amount swaying —
	/// published a frame at a time, so the card's scroll scope has traces that move and differ.
	bool stereoSweep = false;
	bool snhSweep = false;
	/// `--audio <seconds>`: SIGNAL THROUGH THE SHOT DEVICE, for a surface that otherwise says "no signal". This tool has no
	/// `Synth` and no audio thread, so a meter, gain-reduction bar or spectrum has nothing to draw. The device's own DSP
	/// (the body a harness option already filed, else a fresh instance from its descriptor) is run at its lane row over a
	/// sawtooth chord struck every half second, with the note context a voice would hand it, and its readings go
	/// to the stores the plugin's lane fills (`feathers_clap.cpp`'s DSP connector: OWLTT's and automod's own, else the
	/// generic channel). A container's inner engine meters its own devices. Test-only; the fx lane only.
	double audioSeconds = 0.0;
	/// `--mod-run <seconds>`: the rack's twin of `--audio` — the shot modulator's own `ModOps` ticked at its block rate
	/// under the same struck gate, its playhead (position, level, trig fires, recent events) published as the rack does,
	/// so a scope or a timeline has a history. A modulator that reads audio (`ModOps::readsAudioOf`) hears the chord.
	double modRunSeconds = 0.0;
	/// `--arp-play`: the ARP in the lane, run offline at its row with a latched chord held at 140 bpm, its playhead and
	/// newest steps published each frame — so the card's step lane draws the steps it really plays (the S&H sweep's road).
	bool arpPlay = false;
	/// `--pattern-like`: in the Pattern device's window, record a like (the heart) for the focused pattern, so the terrain's
	/// "more like this" highlight shows in the shot.
	bool patternLike = false;
	/// `--pattern-focus <1-4>`: the Pattern card's (and window's) tab, through their own verb (`PagedControls`).
	int patternFocus = 0;
	/// `--height <px>`: the editor's logical height asked of the host road (adjustSize), for a tall card.
	uint32_t shotHeight = 0;
	/// `--pattern-edit <step>`: click that step of the Pattern window's lane, through the lane's own press and release.
	std::vector<int> patternEdits;
	bool haveSparks = false;
	std::string sklitterPlay; ///< `--sklitter-play`; empty when not asked

	bool sparkCpu = false;
	float sparkWet = 0.0f;
	float sparkGrain = 0.0f;
	/// GRAIN ONSETS PER SECOND to drive the pad's third emitter with (backlog HR round 2), 0 = none.
	double onsetRate = 0.0;
	/// REPEAT'S PERIOD, seconds, to drive the repeat counter with (backlog HR round 2), 0 = none.
	double repeatPeriod = 0.0;
	double clipArg = 0.0;
	/// **THE MOD RACK IS A DIFFERENT LANE AND `addFxDevice` CANNOT REACH IT.** DAHDSR and MSEG are
	/// modulation SOURCES; they are not on the fx chain at all, which is why asking for one by tag
	/// used to photograph an empty lane (see the refusal guard below).
	///
	/// **AND IT SEEDS BEFORE `Gui` EXISTS, WHICH MAKES IT BLIND TO A WHOLE CLASS OF FAULT — USE
	/// `--rack-add` FOR THOSE.** The card is then built during the editor's opening sequence and laid
	/// out AGAIN before the first frame is grabbed, so anything whose subject is *the pass that builds
	/// the card* is repaired before the shutter: on the second pass the card already has a width.
	/// MEASURED on the mod strip's one-row-foot defect — this flag's shot was BYTE-IDENTICAL across
	/// the broken and the fixed build over the whole strip column, which reads exactly like "fixed"
	/// and is not. `--rack-add` reaches the same state through the (+) on an open editor and differs
	/// by 6589 pixels. **A venue that cannot express the outcome is not evidence about it.**
	bool rackMode = false;
	long playheadOrdinal = -1;
	double playheadPhase = 0.0;
	bool loopArg = false;
	visage::Point dragBy { 0.0f, 0.0f };
	bool haveDrag = false;
	/// WHICH OF SKLITTER'S TWO RAILS TO OPEN — see the block below. Empty means leave the defaults.
	std::string railArg;
	std::string bodyArg;
	/// A MODULATOR TO ADD THROUGH THE STRIP'S (+) ONCE THE EDITOR IS OPEN — see the block below for
	/// why that is a different state from `--rack`, which seeds the bridge before the editor exists.
	std::string rackAdd;
	/// `--rack-scroll`: where to scroll the modulator strip before the grab — pixels, or "max" for the
	/// bottom. Negative when not asked. A card lower than the strip's height is otherwise unphotographable.
	float rackScroll = -1.0f;
	bool rackScrollMax = false;
	/// **UNBOUNDED DEVICES (step 8): A LONG LANE AND A LONG STRIP.** `--fill-fx N` adds filters until the lane holds N,
	/// `--fill-rack N` adds LFOs until the strip does, and `--lane-scroll <px|max>` scrolls the lane before the grab —
	/// the venue for looking at a chain past the size it used to stop at.
	size_t fillFx = 0;
	size_t fillRack = 0;
	/// `--select-fx N` (backlog OA): the first N lane cards selected; `--select-menu`: and the first one's dots menu open.
	size_t selectFx = 0;
	bool selectMenu = false;
	/// `--chain-select N` with `--chain-window`: N more devices in the window's chain, the first N selected.
	size_t chainSelect = 0;
	/// `--select-verb group-chain|group-rack|duplicate` with `--select-fx`: the verb applied to the selection.
	std::string selectVerb;
	float laneScroll = -1.0f;
	bool laneScrollMax = false;
	/// **CONTROLS OF THE `--rack-add` DEVICE, SET AFTER IT LANDS** — by declared name, refused when
	/// the name is not the device's. The modulator cards' faces (backlog GG: the widest Mode name in
	/// its cell) are otherwise only photographable at their defaults.
	std::vector<std::pair<std::string, double>> rackSet;
	/// **SCROLL THE STRIP TO ITS END** after the add, so a card that landed below the fold is in
	/// the frame whole rather than cut off at the window's bottom edge.
	bool rackScrollEnd = false;
	bool trigPress = false; // `--trig-press`: the hand on every Trig pad, so the shot shows it lit (backlog MV)
	bool envPeakHover = false; // `--env-peak-hover`: an Opt-hover on every DAHDSR card's peak, lit (backlog MN)
	/// `--stepseq-randomise <lanes>`: every step sequencer card runs Randomise on each lane named (letters of "vgprs":
	/// value, gate, prob, ratchet, slew), so the grid is photographed holding a pattern (backlog MX).
	std::string stepseqRandomise;
	/// **OPEN THE MENU OF THE STRIP BOX OVER THIS CONTROL** (by declared name), so a Choice dropdown
	/// can be photographed open — its size against the card it hangs from is the question.
	std::string rackMenu;
	/// **HOVER A CONTROL AND LET ITS TOOLTIP APPEAR** — the first widget in the editor bound to the
	/// control of this declared name gets a mouse enter at its centre (or at `--hover-at`'s offset from
	/// its top-left, in logical pixels), delivered in-process as a hand's would be; the host's tooltip
	/// clock is then moved past the delay, and the editor's own tick shows it. No X11 pointer is moved.
	std::string hover;
	visage::Point hoverAt { -1.0f, -1.0f };
	/// `--tooltip-detail brief|full` — the tooltip detail for `--hover`, set after the editor has read the
	/// desk's (`tooltip_prefs.h`), so a photograph names what it shows. Unset keeps the desk's.
	std::string tooltipDetail;
	/// **THREE GRABS AROUND THE HOVER** — `<out>-before.png` (nothing hovered), `<out>-shown.png` (the
	/// tooltip up) and `<out>-hidden.png` (after the pointer left). Meant to be run WITHOUT
	/// `VISAGE_FULL_REDRAW`, so the only repaints are the ones the layer asks for: before and hidden
	/// matching while shown differs is the proof the show AND the hide are delivered.
	bool hoverSequence = false;
	/// OPEN OWLTT'S MODE MENU once its panel is up — see the block that does it.
	bool owlttModeMenu = false;
	/// **OPEN THE PANEL IN ITS OWN WINDOW — the default a player gets**, and photograph the whole screen
	/// as well, since that window is not the editor's. `-popout.png` beside the requested file.
	bool popout = false;
	/// **WHAT TO PUT IN SKLITTER'S MASTER LANE, so the fx rail's upper column has something to
	/// photograph.** Repeatable, applied in order, and refused loudly when the chain will not take a
	/// tag.
	std::vector<std::string> chainAdd;
	/// **RESONATE'S TWO FX LANES, OPEN, with these devices in them** (2026-10-05): `--resonate-drone <tag>` and
	/// `--resonate-wet <tag>`, each repeatable. Either one binds both lanes over a harness-built instance (as
	/// `--chain-add` does for SKLITTER) and opens both rails.
	std::vector<std::string> resonateDrone;
	/// `--site`: THE DEVICE'S PRE/POST MICROCHAIN (backlog NZ), opened by its card's chip; `--site-pre`/`--site-post` seed its
	/// lanes (repeatable) and `--site-arm` arms PRE's first modulator output, so the glow down the chain is in the shot.
	bool site = false;
	std::vector<std::string> sitePre;
	std::vector<std::string> sitePost;
	bool siteArm = false;
	/// **`--rack-chains N[:open]` (backlog OC): THE RACK WINDOW OVER N RUNNING CHAINS** — a Rack body built and filed as a
	/// host would, N chains with a level each, audio run through it so the headers' meters read, and `:open` opens the
	/// first rail (the others stay collapsed, the state a fresh rack starts in). 0 means not asked.
	int rackChains = 0;
	bool rackChainsOpen = false;
	/// `--chain-window` (backlog OC): the CHAIN device's window over a running body with a filter and a delay in it.
	bool chainWindow = false;
	/// `--chain-rack <mode>:<chains>:<band count index>` with `--chain-window`: a RACK beside the Filter in the window's
	/// horizontal chain, in that mode with that many chains — the h-chain venue for the Rack card's height.
	std::string chainRack;
	/// `--chain-with <TAG>[:<control>=<value>]` with `--chain-window`: that device beside the Filter in the window's
	/// horizontal chain, one control set — the h-chain venue for any card (the Pattern's, the Arp's).
	std::vector<std::string> chainWith;
	std::vector<std::string> resonateWet;
	/// **AND THE SAME FOR THE GRAIN LANE, which is a SECOND column since 2026-09-20.** A separate
	/// option rather than a lane argument on `--chain-add`, because every existing invocation of that
	/// flag means the master lane and should keep meaning it.
	std::vector<std::string> grainAdd;
	/// **BIND THE CHAINS AND ADD NOTHING** — the state a player meets first, and the one `--chain-add`
	/// can never photograph: an EMPTY chain with a live (+) in it. Implied by either add flag.
	bool chainLive = false;
	/// **WHICH DEVICES IN THAT CHAIN TO OPEN A PANEL FOR** — by chain position, so a case names a place
	/// rather than an instance id it cannot know. This is the ONLY way to photograph two panels at once
	/// (the SKLITTER panel and one opened from inside its chain), which is the state backlog GR's tint
	/// is about and the one a whole-window capture otherwise cannot reach.
	std::vector<size_t> chainExpand;
	/// **A CARD IN THE MASTER LANE HELD MID-DRAG** (backlog JA) — the one state a drop indicator exists
	/// in, and one no tick reaches by itself. -1 is none.
	int chainDrag = -1;
	/// **PUT THE CHAIN COLUMN AT THE NESTING WALL AND OPEN ITS (+).** The only way to photograph the
	/// congratulation, which lives as a disabled row in that menu: reaching the wall for real means
	/// nesting a device inside a device twenty times, which no harness is going to do. The NEST is what
	/// the bound is read from, so setting it is setting the real state rather than faking the drawing.
	bool chainAtWall = false;
	/// **OPEN THE FX LANE'S (+) MENU, AND HOVER ONE CATEGORY HEADING OPEN.** The add menu is filed by
	/// category, so the picture worth taking is the headings with one submenu out. The
	/// hover is delivered IN-PROCESS to the popup's own list frame, as a mouse move a hand would make —
	/// no X11 pointer is synthesised or moved. -1 opens the menu and hovers nothing.
	int fxAddMenu = -2;
	/// **HOW FAR TO DRAG THE PANEL'S RESIZE GRIP, so a claim about scaling can be made at more than one
	/// size.** A claim that something scales cannot be made from one picture, and every other state this
	/// tool photographs is a state a hand can put the editor in — so this takes the grip's own road
	/// (`beginPanelResize` then `resizePanelBy`), which is what `ExpandedPanel` calls on a real drag,
	/// rather than writing a size into the slot. Clamped by the overlay exactly as a hand would be, and
	/// the APPLIED number is printed rather than the asked-for one.
	visage::Point panelGrow { 0.0f, 0.0f };
	bool havePanelGrow = false;
	/// **A CONTROL SET AFTER THE PANEL IS OPEN, which is a different state from the same value set
	/// before it.** `"<name>=<value>"` seeds the bridge before the editor exists, so the panel OPENS at
	/// the size that value asks for; `--then` writes the same field under an open panel and lets the
	/// editor's own tick notice — the road a hand on the Mode picker takes. Backlog HY was exactly the
	/// difference: a panel opened in a sparse mode and switched to Grain kept the sparse mode's height, and no shot
	/// taken through the first road could have shown it.
	std::vector<std::pair<std::string, double>> then;
	/// **A HEADER MACRO'S TYPE, NAME AND VALUE** (A7's `MacroSlot`) — seeded on the bridge before the
	/// editor opens, the road a patch load takes. An empty name leaves the default.
	struct MacroSeed {
		size_t index = 0;
		device::MacroType type = device::MacroType::Knob;
		std::string name;
		double value = -1.0; ///< < 0: leave the parameter where it is
	};
	std::vector<MacroSeed> macroSeeds;
	/// `--label`: the seeded device's output (0-based here) and what this instance calls it.
	std::vector<std::pair<size_t, std::string>> labels;
	/// `--expose`: controls of the seeded device given a host slot and pinned, by name.
	std::vector<std::string> exposed;
	/// `--preset-prompt panel|header[:<name>[|<tags>]]` — open the save prompt of SKLITTER's panel browser
	/// or the editor header's, with `<name>` and `<tags>` typed in (the suggestion when absent).
	/// `--preset-seed` — before anything else, save a handful of presets, in categories and with tags, into
	/// the chosen browser's bank (a scratch root under the venue), so a list has something to show.
	/// `--preset-list panel|header:folders|tags` — open that browser's list in that view. Backlog FO.
	/// `--preset-manage` — and hover its Manage row open, the way `--fx-add-menu` hovers a heading.
	std::string presetPrompt;
	std::string presetList;
	bool presetSeed = false;
	bool presetManage = false;
	for (int i = 3; i < argc; ++i) {
		const std::string arg = argv[i];
		if (arg == "--host" && i + 1 < argc) {
			const std::string spec = argv[++i];
			const size_t at = spec.find('=');
			if (at == std::string::npos) {
				std::printf("--host takes \"<parameter name>=<value>\", got \"%s\"\n", spec.c_str());
				return 2;
			}
			hostWanted.emplace_back(spec.substr(0, at), std::strtod(spec.c_str() + at + 1, nullptr));
			continue;
		}
		// `--route-phi <octaves>`: `--route-curves` with the first row (Voice Level) through the φ curve at
		// 2^octaves periods (backlog PY) — the chip's picture at low and high periods.
		if (arg == "--route-phi" && i + 1 < argc) {
			routePhiOctaves = static_cast<float>(std::strtod(argv[++i], nullptr));
			routeCurves = true;
			continue;
		}
		// `--route-exp <k>` / `--route-log <k>`: the first row's Exp, or the third row's Log, at exponent k.
		if (arg == "--route-exp" && i + 1 < argc) {
			routeExpExponent = static_cast<float>(std::strtod(argv[++i], nullptr));
			routeCurves = true;
			continue;
		}
		if (arg == "--route-log" && i + 1 < argc) {
			routeLogExponent = static_cast<float>(std::strtod(argv[++i], nullptr));
			routeCurves = true;
			continue;
		}
		if (arg == "--route-curves" || arg == "--route-curves-menu" || arg == "--route-curves-bipolar") {
			routeCurves = true;
			routeCurvesMenu = routeCurvesMenu || arg == "--route-curves-menu";
			routeCurvesBipolar = routeCurvesBipolar || arg == "--route-curves-bipolar";
			continue;
		}
		if (arg == "--stack-route" && i + 1 < argc) {
			stackRoute = static_cast<float>(std::strtod(argv[++i], nullptr));
			continue;
		}
		if (arg == "--engine-rail-page" && i + 1 < argc) {
			engineRailPage = argv[++i];
			if (engineRailPage != "pitch" && engineRailPage != "stack") {
				std::printf("--engine-rail-page takes pitch or stack, got \"%s\"\n", engineRailPage.c_str());
				return 2;
			}
			continue;
		}
		if (arg == "--site") {
			site = true;
			expand = false;
			continue;
		}
		if (arg == "--site-pre" && i + 1 < argc) {
			sitePre.emplace_back(argv[++i]);
			continue;
		}
		if (arg == "--site-post" && i + 1 < argc) {
			sitePost.emplace_back(argv[++i]);
			continue;
		}
		if (arg == "--site-arm") {
			siteArm = true;
			continue;
		}
		if (arg == "--card") {
			expand = false;
			continue;
		}
		// `--spectrum`: THE ADDITIVE DRAWER'S SPECTRUM PANEL, opened by its own tab's road (ExploreStage::onSpectrumTab), for a
		// side-by-side with the Spectral device's window (2026-10-13). Seed Osc Type with `--host "Osc Type=<Additive's index>"`.
		if (arg == "--likes" && i + 1 < argc) {
			likes = std::atoi(argv[++i]);
			expand = false;
			continue;
		}
		if (arg == "--spectrum") {
			spectrum = true;
			expand = false;
			continue;
		}
		if (arg == "--explore") {
			exploreSurface = true;
			continue;
		}
		if (arg == "--resize-to" && i + 1 < argc) {
			if (std::sscanf(argv[++i], "%ux%u", &resizeToW, &resizeToH) != 2) {
				std::printf("--resize-to wants WxH\n");
				return 2;
			}
			exploreSurface = true;
			expand = false;
			continue;
		}
		if (arg == "--collapse-rails") {
			collapseRails = true;
			exploreSurface = true;
			expand = false;
			continue;
		}
		if (arg == "--resize-steps" && i + 1 < argc) {
			resizeSteps = std::max(1, std::atoi(argv[++i]));
			continue;
		}
		if (arg == "--explore-time" && i + 1 < argc) {
			exploreTimeSteps = std::max(2, std::atoi(argv[++i]));
			exploreSurface = true;
			expand = false;
			continue;
		}
		if (arg == "--explore-zoom-pump" && i + 1 < argc) {
			exploreZoomPump = std::max(4, std::atoi(argv[++i]));
			continue;
		}
		if (arg == "--width" && i + 1 < argc) {
			shotWidth = static_cast<uint32_t>(std::max(0, std::atoi(argv[++i])));
			continue;
		}
		if (arg == "--explore-zooms" && i + 1 < argc) {
			std::string list = argv[++i];
			for (size_t at = 0; at < list.size();) {
				const size_t comma = list.find(',', at);
				exploreZooms.push_back(std::atof(list.substr(at, comma - at).c_str()));
				at = comma == std::string::npos ? list.size() : comma + 1;
			}
			exploreSurface = true;
			expand = false;
			continue;
		}
		if (arg == "--radius-route" && i + 1 < argc) {
			const std::string spec = argv[++i];
			if (std::sscanf(spec.c_str(), "%lf:%f", &radiusRouteBase, &radiusRouteDepth) != 2 || radiusRouteBase < 0.0) {
				std::printf("--radius-route wants <base>:<depth>[:bi], got \"%s\"\n", spec.c_str());
				return 2;
			}
			radiusRouteBipolar = spec.find(":bi") != std::string::npos;
			exploreSurface = true;
			expand = false;
			continue;
		}
		if (arg == "--drum-kit") {
			drumKit = true;
			expand = false;
			continue;
		}
		if (arg == "--preset-prompt" && i + 1 < argc) {
			presetPrompt = argv[++i];
			continue;
		}
		if (arg == "--preset-list" && i + 1 < argc) {
			presetList = argv[++i];
			continue;
		}
		if (arg == "--preset-seed") {
			presetSeed = true;
			continue;
		}
		if (arg == "--preset-manage") {
			presetManage = true;
			continue;
		}
		if (arg == "--then" && i + 1 < argc) {
			const std::string spec = argv[++i];
			const size_t eq = spec.find('=');
			if (eq == std::string::npos) {
				std::printf("--then takes \"<control name>=<value>\", got \"%s\"\n", spec.c_str());
				return 2;
			}
			then.emplace_back(spec.substr(0, eq), std::strtod(spec.c_str() + eq + 1, nullptr));
			continue;
		}
		// DRAG THE OPEN PANEL BY ITS HEAD, in-process through the tree — press, move, release — the
		// way test_gui_widgets_devices' DD case does, and for the same reason no pointer is moved:
		// this is the desktop display. `--drag 200,150` moves it right and down; `--drag 5000,5000` is the
		// clamp, and the capture shows where the panel actually stopped.
		if (arg == "--drag" && i + 1 < argc) {
			const std::string spec = argv[++i];
			const size_t comma = spec.find(',');
			if (comma == std::string::npos) {
				std::printf("--drag takes dx,dy\n");
				return 2;
			}
			dragBy = visage::Point { static_cast<float>(std::atof(spec.substr(0, comma).c_str())),
				                     static_cast<float>(std::atof(spec.substr(comma + 1).c_str())) };
			haveDrag = true;
			continue;
		}
		// GROW THE OPEN PANEL BY ITS GRIP. `--panel-grow 260x180` is a hand dragging the far corner
		// that far; the overlay clamps it to its own edge, which is why the applied size is printed.
		if (arg == "--panel-grow" && i + 1 < argc) {
			const std::string spec = argv[++i];
			const size_t by = spec.find('x');
			if (by == std::string::npos) {
				std::printf("--panel-grow takes <dx>x<dy>\n");
				return 2;
			}
			panelGrow = visage::Point { static_cast<float>(std::atof(spec.substr(0, by).c_str())),
				                        static_cast<float>(std::atof(spec.substr(by + 1).c_str())) };
			havePanelGrow = true;
			continue;
		}
		if (arg == "--rails" && i + 1 < argc) {
			railArg = argv[++i];
			// **THREE RAILS SINCE 2026-09-20, SO `fx` MEANS BOTH FX LANES AND `grain`/`post` NAME ONE.**
			// `fx` and `both` keep exactly the meaning every existing invocation gave them — every fx
			// lane there is — which is why they were not redefined to mean the post rail alone.
			if (railArg != "none" && railArg != "circle" && railArg != "fx" && railArg != "grain"
			    && railArg != "post" && railArg != "both") {
				std::printf("--rails takes none, circle, fx, grain, post or both, got \"%s\"\n",
				            railArg.c_str());
				return 2;
			}
			continue;
		}
		if (arg == "--body" && i + 1 < argc) {
			// **A SEPARATE SWITCH FROM `--rails`, BECAUSE IT IS A SEPARATE FOLD.** The body joins the
			// two rails rather than becoming a third value of the same option: all eight combinations
			// are legal states and an option with four values could photograph only four of them.
			bodyArg = argv[++i];
			if (bodyArg != "open" && bodyArg != "collapsed") {
				std::printf("--body takes open or collapsed, got \"%s\"\n", bodyArg.c_str());
				return 2;
			}
			continue;
		}
		if (arg == "--preview" && i + 1 < argc) {
			preview = argv[++i];
			continue;
		}
		// WHICH PERFORMANCE GRID — 3/3/1 or 3/4. A looking question, so the tool photographs either
		// without a rebuild, as `--preview` already does.
		// **WHERE THE LFO IS, FOR A TOOL WITH NO AUDIO THREAD.** The strip's marker and its response
		// curve both read the phase the DSP publishes, and a headless shot has no DSP — so without
		// this every photograph is of a device sitting at rest, which is the ONE state that cannot
		// show whether the star moves. Published straight into the bridge, which is exactly where a
		// running plugin puts it.
		if (arg == "--control" && i + 1 < argc) {
			const std::string spec = argv[++i];
			const size_t eq = spec.find('=');
			if (eq == std::string::npos) {
				std::printf("--control takes <index>=<value>, got \"%s\"\n", spec.c_str());
				return 1;
			}
			controlWanted.emplace_back(static_cast<size_t>(std::atol(spec.substr(0, eq).c_str())),
			                           static_cast<float>(std::atof(spec.substr(eq + 1).c_str())));
			continue;
		}
		if (arg == "--state" && i + 1 < argc) {
			const std::string spec = argv[++i];
			const size_t eq = spec.find('=');
			if (eq == std::string::npos) {
				std::printf("--state takes <index>=<value>, got \"%s\"\n", spec.c_str());
				return 1;
			}
			stateWanted.emplace_back(static_cast<size_t>(std::atol(spec.substr(0, eq).c_str())),
			                         static_cast<float>(std::atof(spec.substr(eq + 1).c_str())));
			continue;
		}
		if (arg == "--phase" && i + 1 < argc) {
			phaseArg = std::atof(argv[++i]);
			havePhase = true;
			continue;
		}
		// AND WHERE THE ENVELOPE FOLLOWER IS, for the trace the strip now draws over the two curves.
		// Same reason as the phase directly above: a headless shot has no DSP, so without this the
		// trace photographs at rest — a picture of the feature switched off.
		if (arg == "--envelope" && i + 1 < argc) {
			envArg = std::atof(argv[++i]);
			havePhase = true;
			continue;
		}
		// **THE PERFORM CIRCLE'S SPARKS NEED A LEVEL AND A LEVEL ONLY COMES FROM AUDIO** (backlog HR).
		// Nothing in this tool renders a block, so the pad at rest is correctly and uselessly empty —
		// the one state a capture of the sparks cannot answer anything in. This publishes the two
		// levels the emitters read, exactly as an engaged device would, so the population settles
		// during the `pump` that follows and the frame shows both colours alive.
		// **SKLITTER'S BAND FROM A REAL DEVICE** (Repeat's face, 2026-09-23): `<mode index>:<rate>:<engaged
		// 0|1>[:<zone A>[:<groove mode>:<amount>[:<zone B>[:<puck config>[:<seq bars>]]]]]`. A device is built, driven with a ramp at 120 BPM —
		// engaged, a bar and a half past the press so it is Playing mid-bar — and ITS publish is what the
		// band draws, so the photograph is of the readings the engine computes rather than ones typed here.
		if (arg == "--sklitter-play" && i + 1 < argc) {
			sklitterPlay = argv[++i];
			continue;
		}
		if (arg == "--sklitter-sparks" && i + 1 < argc) {
			const std::string spec = argv[++i];
			const size_t colon = spec.find(':');
			sparkWet = static_cast<float>(std::atof(spec.substr(0, colon).c_str()));
			sparkGrain = colon == std::string::npos
			                 ? 0.0f
			                 : static_cast<float>(std::atof(spec.substr(colon + 1).c_str()));
			haveSparks = true;
			continue;
		}
		// **THE A/B FOR THE GPU PASS — backlog HR.** Both paths evaluate the same closed form
		// (`PerformPad::sparkPointAt`); the only difference is whether the VERTEX SHADER does it or the
		// CPU does. So two captures under this flag are a test of `vs_spark.sc` and of nothing else,
		// and it is the only test of that shader there is: a fragment stage has no headless seam.
		if (arg == "--spark-cpu") {
			sparkCpu = true;
			continue;
		}
		// **GRAIN ONSETS, BACKLOG HR ROUND 2.** The onset counters are EVENTS, so a single publish
		// before the editor opens shows nothing — the pad takes the first reading as a baseline. This
		// advances them during every `pump`, cycling the four gate classes, and republishes through
		// the same channel. Needs `--sklitter-sparks` (the pad throws no onset while both levels read 0).
		if (arg == "--sklitter-onsets" && i + 1 < argc) {
			onsetRate = std::atof(argv[++i]);
			continue;
		}
		// AND REPEAT'S RESTARTS, one every `<period>` seconds with that period published beside them —
		// the splitting repeat sparks. Same road, same need for `--sklitter-sparks`.
		if (arg == "--sklitter-repeats" && i + 1 < argc) {
			repeatPeriod = std::atof(argv[++i]);
			continue;
		}
		if (arg == "--accent-hue" && i + 1 < argc) {
			accentHueArg = std::atof(argv[++i]);
			continue;
		}
		if (arg == "--hue" && i + 1 < argc) {
			hueArg = std::atof(argv[++i]);
			continue;
		}
		if (arg == "--settings") {
			settingsArg = true;
			continue;
		}
		if (arg == "--readings") {
			haveReadings = true;
			continue;
		}
		if (arg == "--twinfield-play") {
			twinfieldPlay = true;
			continue;
		}
		if (arg == "--twinfield-tab" && i + 1 < argc) {
			twinfieldTab = std::atoi(argv[++i]);
			continue;
		}
		if (arg == "--stereo-tab" && i + 1 < argc) {
			stereoTab = std::atoi(argv[++i]);
			continue;
		}
		if (arg == "--arp-play") {
			arpPlay = true;
			continue;
		}
		if (arg == "--pattern-edit" && i + 1 < argc) {
			patternEdits.push_back(std::atoi(argv[++i]));
			continue;
		}
		if (arg == "--height" && i + 1 < argc) {
			shotHeight = static_cast<uint32_t>(std::max(0, std::atoi(argv[++i])));
			continue;
		}
		if (arg == "--pattern-focus" && i + 1 < argc) {
			patternFocus = std::atoi(argv[++i]);
			continue;
		}
		if (arg == "--pattern-like") {
			patternLike = true;
			continue;
		}
		if (arg == "--mod-run" && i + 1 < argc) {
			modRunSeconds = std::max(0.0, std::atof(argv[++i]));
			continue;
		}
		if (arg == "--audio" && i + 1 < argc) {
			audioSeconds = std::max(0.0, std::atof(argv[++i]));
			continue;
		}
		if (arg == "--snh-sweep") {
			snhSweep = true;
			continue;
		}
		if (arg == "--stereo-sweep") {
			stereoSweep = true;
			continue;
		}
		if (arg == "--arm-macro") {
			armMacro = true;
			continue;
		}
		if (arg == "--arm-pair") {
			armPair = true;
			continue;
		}
		if (arg == "--modulated-r" && i + 1 < argc) {
			const std::string spec = argv[++i];
			const size_t eq = spec.rfind('=');
			if (eq == std::string::npos) {
				std::printf("--modulated-r wants \"<control name>=<value>\", got \"%s\"\n", spec.c_str());
				return 1;
			}
			modulatedRArg.emplace_back(spec.substr(0, eq), std::atof(spec.substr(eq + 1).c_str()));
			continue;
		}
		if (arg == "--modulated" && i + 1 < argc) {
			const std::string spec = argv[++i];
			const size_t eq = spec.rfind('=');
			if (eq == std::string::npos) {
				std::printf("--modulated wants \"<control name>=<value>\", got \"%s\"\n", spec.c_str());
				return 1;
			}
			modulatedArg.emplace_back(spec.substr(0, eq), std::atof(spec.substr(eq + 1).c_str()));
			continue;
		}
		if (arg == "--output-peak" && i + 1 < argc) {
			outputPeakDb = std::atof(argv[++i]);
			haveOutputPeak = true;
			continue;
		}
		if (arg == "--fold") {
			foldArg = true;
			continue;
		}
		if (arg == "--meter" && i + 1 < argc) {
			const std::string list = argv[++i];
			for (size_t at = 0; at <= list.size();) {
				const size_t comma = std::min(list.find(',', at), list.size());
				meterArg.push_back(static_cast<float>(std::atof(list.substr(at, comma - at).c_str())));
				at = comma + 1;
			}
			continue;
		}
		if (arg == "--clip" && i + 1 < argc) {
			clipArg = std::atof(argv[++i]);
			haveReadings = true;
			continue;
		}
		if (arg == "--rack") {
			rackMode = true;
			continue;
		}
		if (arg == "--fill-fx" && i + 1 < argc) {
			fillFx = static_cast<size_t>(std::atol(argv[++i]));
			continue;
		}
		if (arg == "--fill-rack" && i + 1 < argc) {
			fillRack = static_cast<size_t>(std::atol(argv[++i]));
			continue;
		}
		if (arg == "--select-fx" && i + 1 < argc) {
			selectFx = static_cast<size_t>(std::atol(argv[++i]));
			continue;
		}
		if (arg == "--select-verb" && i + 1 < argc) {
			selectVerb = argv[++i];
			continue;
		}
		if (arg == "--chain-select" && i + 1 < argc) {
			chainSelect = static_cast<size_t>(std::atol(argv[++i]));
			continue;
		}
		if (arg == "--select-menu") {
			selectMenu = true;
			continue;
		}
		if (arg == "--lane-scroll" && i + 1 < argc) {
			const std::string at = argv[++i];
			laneScrollMax = at == "max";
			laneScroll = laneScrollMax ? 0.0f : static_cast<float>(std::atof(at.c_str()));
			continue;
		}
		if (arg == "--rack-add" && i + 1 < argc) {
			rackAdd = argv[++i];
			continue;
		}
		if (arg == "--rack-scroll" && i + 1 < argc) {
			const std::string at = argv[++i];
			rackScrollMax = at == "max";
			rackScroll = rackScrollMax ? 0.0f : static_cast<float>(std::atof(at.c_str()));
			continue;
		}
		if (arg == "--rack-set" && i + 1 < argc) {
			const std::string spec = argv[++i];
			const size_t eq = spec.find('=');
			if (eq == std::string::npos) {
				std::printf("--rack-set expects \"<control name>=<value>\", got \"%s\"\n", spec.c_str());
				return 2;
			}
			rackSet.emplace_back(spec.substr(0, eq), std::strtod(spec.c_str() + eq + 1, nullptr));
			continue;
		}
		if (arg == "--stepseq-randomise" && i + 1 < argc) {
			stepseqRandomise = argv[++i];
			continue;
		}
		if (arg == "--trig-press") {
			trigPress = true;
			continue;
		}
		if (arg == "--env-peak-hover") {
			envPeakHover = true;
			continue;
		}
		if (arg == "--rack-scroll-end") {
			rackScrollEnd = true;
			continue;
		}
		if (arg == "--popout") {
			popout = true;
			continue;
		}
		if (arg == "--owltt-mode-menu") {
			owlttModeMenu = true;
			continue;
		}
		if (arg == "--rack-menu" && i + 1 < argc) {
			rackMenu = argv[++i];
			continue;
		}
		if (arg == "--chain-with" && i + 1 < argc) {
			chainWith.emplace_back(argv[++i]);
			continue;
		}
		if (arg == "--chain-rack" && i + 1 < argc) {
			chainRack = argv[++i];
			continue;
		}
		if (arg == "--chain-window") {
			chainWindow = true;
			continue;
		}
		if (arg == "--rack-chains" && i + 1 < argc) {
			const std::string spec = argv[++i];
			rackChains = std::atoi(spec.c_str());
			rackChainsOpen = spec.find(":open") != std::string::npos;
			continue;
		}
		if (arg == "--resonate-drone" && i + 1 < argc) {
			resonateDrone.emplace_back(argv[++i]);
			continue;
		}
		if (arg == "--resonate-wet" && i + 1 < argc) {
			resonateWet.emplace_back(argv[++i]);
			continue;
		}
		if (arg == "--chain-live") {
			chainLive = true;
			continue;
		}
		if (arg == "--chain-add" && i + 1 < argc) {
			chainAdd.emplace_back(argv[++i]);
			continue;
		}
		if (arg == "--grain-add" && i + 1 < argc) {
			grainAdd.emplace_back(argv[++i]);
			continue;
		}
		if (arg == "--chain-at-wall") {
			chainAtWall = true;
			continue;
		}
		if (arg == "--fx-add-menu" && i + 1 < argc) {
			fxAddMenu = std::atoi(argv[++i]);
			continue;
		}
		if (arg == "--chain-expand" && i + 1 < argc) {
			chainExpand.push_back(static_cast<size_t>(std::atoi(argv[++i])));
			continue;
		}
		if (arg == "--chain-drag" && i + 1 < argc) {
			chainDrag = std::atoi(argv[++i]);
			continue;
		}
		// WHERE A RACK MODULATOR STANDS, for a tool with no audio thread — the same reason `--phase`
		// exists for the automod. Written `<ordinal>:<phase>`; the ordinal is a `Stage` for a DAHDSR
		// and a segment index for an MSEG, which is the device's own vocabulary rather than a second
		// one this tool would have to keep in step.
		if (arg == "--playhead" && i + 1 < argc) {
			const std::string spec = argv[++i];
			const size_t colon = spec.find(':');
			if (colon == std::string::npos) {
				std::printf("--playhead takes <ordinal>:<phase>, got \"%s\"\n", spec.c_str());
				return 2;
			}
			playheadOrdinal = std::strtol(spec.c_str(), nullptr, 10);
			playheadPhase = std::strtod(spec.c_str() + colon + 1, nullptr);
			continue;
		}
		if (arg == "--loop") {
			loopArg = true;
			continue;
		}
		if (arg == "--grid" && i + 1 < argc) {
			gridArg = argv[++i];
			if (gridArg != "uniform" && gridArg != "compact") {
				std::printf("--grid takes uniform or compact, got \"%s\"\n", gridArg.c_str());
				return 2;
			}
			continue;
		}
		if (arg == "--macro" && i + 1 < argc) {
			// <1..4>=<Type>[:<name>[:<value>]] — REFUSED rather than guessed at, like every other flag.
			const std::string spec = argv[++i];
			const size_t eq = spec.find('=');
			MacroSeed seed;
			const long index = eq == std::string::npos ? 0 : std::atol(spec.substr(0, eq).c_str());
			std::string rest = eq == std::string::npos ? std::string {} : spec.substr(eq + 1);
			std::string typeName = rest.substr(0, rest.find(':'));
			rest = rest.find(':') == std::string::npos ? std::string {} : rest.substr(rest.find(':') + 1);
			seed.name = rest.substr(0, rest.find(':'));
			if (rest.find(':') != std::string::npos) {
				seed.value = std::atof(rest.substr(rest.find(':') + 1).c_str());
			}
			bool known = true;
			if (typeName == "Knob") {
				seed.type = device::MacroType::Knob;
			}
			else if (typeName == "Latch") {
				seed.type = device::MacroType::Latch;
			}
			else if (typeName == "Momentary") {
				seed.type = device::MacroType::Momentary;
			}
			else {
				known = false;
			}
			if (index < 1 || index > static_cast<long>(kMacroCount) || !known) {
				std::printf("--macro takes <1..4>=<Knob|Latch|Momentary>[:<name>[:<value>]], got \"%s\"\n",
				            spec.c_str());
				return 2;
			}
			seed.index = static_cast<size_t>(index - 1);
			macroSeeds.push_back(seed);
			continue;
		}
		if (arg == "--hover" && i + 1 < argc) {
			hover = argv[++i];
			continue;
		}
		if (arg == "--hover-sequence") {
			hoverSequence = true;
			continue;
		}
		if (arg == "--tooltip-detail" && i + 1 < argc) {
			tooltipDetail = argv[++i];
			if (tooltipDetail != "brief" && tooltipDetail != "full") {
				std::printf("--tooltip-detail takes brief or full, got \"%s\"\n", tooltipDetail.c_str());
				return 2;
			}
			continue;
		}
		if (arg == "--hover-at" && i + 1 < argc) {
			// <x>,<y> from the hovered widget's top-left, in logical pixels.
			const std::string spec = argv[++i];
			const size_t comma = spec.find(',');
			if (comma == std::string::npos) {
				std::printf("--hover-at takes <x>,<y>, got \"%s\"\n", spec.c_str());
				return 2;
			}
			hoverAt = { std::strtof(spec.substr(0, comma).c_str(), nullptr),
			            std::strtof(spec.substr(comma + 1).c_str(), nullptr) };
			continue;
		}
		if (arg == "--expose" && i + 1 < argc) {
			// "<control name>" — THE SEEDED DEVICE'S CONTROL, EXPOSED TO THE HOST (host slots T7): the verb the
			// right-click's "Expose to host" runs, so the knob draws its host mark.
			exposed.emplace_back(argv[++i]);
			continue;
		}
		if (arg == "--label" && i + 1 < argc) {
			// <1..N>=<text> — THE SEEDED DEVICE'S OUTPUT n, relabelled on this instance (`setOutputLabel`),
			// the road the MACROS card's rename takes. Refused rather than guessed at, like every flag.
			const std::string spec = argv[++i];
			const size_t eq = spec.find('=');
			const long index = eq == std::string::npos ? 0 : std::atol(spec.substr(0, eq).c_str());
			if (index < 1) {
				std::printf("--label takes <output 1..N>=<text>, got \"%s\"\n", spec.c_str());
				return 2;
			}
			labels.emplace_back(static_cast<size_t>(index - 1), spec.substr(eq + 1));
			continue;
		}
		const size_t eq = arg.find('=');
		if (eq == std::string::npos) {
			std::printf("expected \"<control name>=<value>\", got \"%s\"\n", arg.c_str());
			return 2;
		}
		wanted.emplace_back(arg.substr(0, eq), std::strtod(arg.c_str() + eq + 1, nullptr));
	}

	XInitThreads();
	Display* display = XOpenDisplay(nullptr);
	if (display == nullptr) {
		std::printf("XOpenDisplay failed — is DISPLAY set?\n");
		return 2;
	}

	ShotGuiHost host;
	ParamBridge bridge;
	ParamSet params;
	for (const MacroSeed& seed : macroSeeds) {
		if (seed.value >= 0.0) {
			params.setBase(static_cast<clap_id>(macroParam(seed.index)), seed.value);
		}
	}
	for (const auto& [name, value] : hostWanted) {
		const ParamDesc* found = nullptr;
		for (const ParamDesc& d : paramDescs()) {
			if (name == d.name) {
				found = &d;
			}
		}
		if (found == nullptr) {
			// REFUSING RATHER THAN PHOTOGRAPHING THE WRONG STATE — the device controls' own rule, below.
			std::printf("no host parameter named \"%s\" — refusing rather than setting nothing\n", name.c_str());
			return 1;
		}
		params.setBase(static_cast<clap_id>(found->id), value);
		std::printf("host:     %s = %g\n", name.c_str(), value);
	}
	bridge.publish(params);
	bridge.refresh();
	if (stackRoute != 0.0f) {
		bridge.setRouteAmount(backplane::Source::core(kStackIndexSource),
		                      backplane::Destination::core(static_cast<size_t>(modDestinationIndexOf(ParamId::FineTune))),
		                      stackRoute);
		bridge.refresh();
		std::printf("route:    Stack Index -> Fine Tune at %g\n", static_cast<double>(stackRoute));
	}
	for (const MacroSeed& seed : macroSeeds) {
		bridge.setMacroType(seed.index, seed.type);
		bridge.setMacroName(seed.index, seed.name);
		std::printf("macro:    %zu type %d name \"%s\" (renamed %d) value %.3f\n", seed.index + 1,
		            static_cast<int>(bridge.macroType(seed.index)), bridge.macroName(seed.index).c_str(),
		            bridge.macroRenamed(seed.index) ? 1 : 0,
		            bridge.value(static_cast<clap_id>(macroParam(seed.index))));
	}

	// **THE REFUSAL GUARD COMPARED AGAINST A VALUE THIS FUNCTION NEVER RETURNS.** `addFxDevice`
	// answers `kNoPosition` when it refuses — its own declaration says so — and this checked for
	// `size_t(-1)`. So the guard was DEAD: `widget-shot DSR ...` printed `seeded: DSR -> slot 8`,
	// photographed an empty lane, and exited 0. That is precisely the failure the message below
	// claims to prevent, and the tool has been able to do it since the guard was written.
	//
	// FOUND BY ASKING FOR A DEVICE THAT IS NOT IN THE FX CHAIN — the rack's DAHDSR — and reading the
	// slot number instead of the reassuring word beside it. A venue that reports success for a state
	// it never reached is worth less than no venue.
	size_t rackSlot = kNoPosition;
	if (rackMode) {
		rackSlot = bridge.addRackDevice(tag);
		if (rackSlot == kNoPosition) {
			std::printf("no modulator tagged \"%s\" for the rack — refusing rather than photographing "
			            "an empty rack (addRackDevice answered %zu, which is its refusal)\n",
			            tag, rackSlot);
			return 1;
		}
		bridge.refresh();
		std::printf("seeded:   %s -> RACK slot %zu\n", tag, rackSlot);
		if (loopArg) {
			feathers::ModRack rack = bridge.modRack();
			mod::dahdsr::Settings env = rack.dahdsrAt(rackSlot);
			env.mode = mod::Mode::Loop;
			rack.setDahdsrAt(rackSlot, env);
			bridge.restoreModRack(rack);
			std::printf("loop:     on\n");
		}
		if (playheadOrdinal >= 0) {
			bridge.publishRackPlayhead(
			    rackSlot, feathers::mod::Playhead { static_cast<uint32_t>(playheadOrdinal),
				                                   static_cast<float>(playheadPhase) });
			std::printf("playhead: ordinal %ld phase %.4f (rack slot %zu)\n", playheadOrdinal,
			            playheadPhase, rackSlot);
		}
		else {
			// **SAID OUT LOUD RATHER THAN LEFT TO THE PICTURE.** A shot with no playhead published is
			// the CONTROL for every shot that has one, and a control is only a control if the log says
			// which one it was.
			std::printf("playhead: none published (control shot — nothing should be marked)\n");
		}
		bridge.refresh();
	}

	// THE FX LANE'S OWN SEEDING, SKIPPED ENTIRELY IN RACK MODE. A rack modulator is not in this lane
	// and asking for it here is what used to photograph an empty one.
	const size_t slot = rackMode ? 0 : bridge.addFxDevice(tag);
	if (!rackMode && slot == kNoPosition) {
		std::printf("no device tagged \"%s\" in the fx lane — refusing rather than photographing an "
		            "empty lane (addFxDevice answered %zu, which is its refusal)\n",
		            tag, slot);
		return 1;
	}
	if (!rackMode) {
		bridge.refresh();
		std::printf("seeded:   %s -> slot %zu\n", tag, slot);
		// --playhead IN THE LANE TOO (a note device's window, e.g. Pattern's playhead), on the road the engine takes.
		if (playheadOrdinal >= 0) {
			bridge.publishFxPlayhead(slot, feathers::mod::Playhead { static_cast<uint32_t>(playheadOrdinal),
			                                                         static_cast<float>(playheadPhase) });
			bridge.refresh();
			std::printf("playhead: ordinal %ld phase %.4f (lane slot %zu)\n", playheadOrdinal, playheadPhase, slot);
		}
	}
	// --fill-fx / --fill-rack: PAST THE OLD CAPS, through the same add verbs a hand uses, each refusal said.
	while (bridge.fxChain().count < fillFx) {
		if (bridge.addFxDevice(feathers::fx::filter::kTraits.tag) == kNoPosition) {
			std::printf("--fill-fx: the lane REFUSED at %zu\n", bridge.fxChain().count);
			return 1;
		}
	}
	while (bridge.modRack().count < fillRack) {
		if (bridge.addRackDevice(feathers::mod::lfo::kTraits.tag) == kNoPosition) {
			std::printf("--fill-rack: the strip REFUSED at %zu\n", bridge.modRack().count);
			return 1;
		}
	}
	if (fillFx > 0 || fillRack > 0) {
		bridge.refresh();
		std::printf("filled:   lane %zu, strip %zu\n", bridge.fxChain().count, bridge.modRack().count);
	}

	const device::Descriptor* desc = deviceForTag(tag);
	for (const auto& [name, value] : wanted) {
		size_t at = device::kNoControl;
		for (size_t i = 0; desc != nullptr && i < desc->params.size(); ++i) {
			if (name == desc->params[i].name) {
				at = i;
				break;
			}
		}
		if (at == device::kNoControl) {
			// REFUSING RATHER THAN PHOTOGRAPHING THE WRONG STATE. A shot taken with a control silently
			// unset looks exactly like a shot taken with it set — which is the whole family of failure
			// this venue keeps producing.
			std::printf("no control named \"%s\" on %s — refusing rather than setting nothing\n",
			            name.c_str(), tag);
			return 1;
		}
		if (rackMode) {
			// THE RACK DEVICE BY ITS ID, the road `setDeviceControl` takes for either chain.
			bridge.setDeviceControl(bridge.rackDeviceAt(rackSlot), at, static_cast<float>(value));
			std::printf("set:      %-24s -> %g (rack slot %zu)\n", name.c_str(), value, rackSlot);
			continue;
		}
		// **THE DEVICE, NOT THE POSITION `addFxDevice` HANDED BACK** (kill-lanes step 2). The road took a
		// lane before and a position before that, and each rename was so this line could not compile
		// unchanged: on this tool's append-only chain the numbers coincide, so a silent substitution
		// would have been right here and wrong the first time a chain was reordered. It takes an id now,
		// and refuses a `size_t`. `build-install-on` has `FEATHERS_BUILD_TOOLS` off and never sees this;
		// the shared `build` dir does, which is the whole reason both are built.
		bridge.setDeviceControl(bridge.fxDeviceAt(slot), at, static_cast<float>(value));
		std::printf("set:      %-24s -> %g\n", name.c_str(), value);
	}
	for (const auto& [index, value] : controlWanted) {
		const DeviceInstanceId seeded = rackMode ? bridge.rackDeviceAt(rackSlot) : bridge.fxDeviceAt(slot);
		bridge.setDeviceControl(seeded, index, value);
		std::printf("control:  [%zu] -> %g\n", index, static_cast<double>(value));
	}
	for (const auto& [index, value] : stateWanted) {
		const DeviceInstanceId seeded = rackMode ? bridge.rackDeviceAt(rackSlot) : bridge.fxDeviceAt(slot);
		bridge.setDeviceState(seeded, index, value);
		std::printf("state:    [%zu] -> %g\n", index, static_cast<double>(value));
	}
	for (const auto& [output, text] : labels) {
		const DeviceInstanceId seeded = rackMode ? bridge.rackDeviceAt(rackSlot) : bridge.fxDeviceAt(slot);
		bridge.setOutputLabel(seeded, output, text);
		// READ BACK, so the log says what the bridge holds rather than what was asked (a clamp, a refusal).
		std::printf("label:    output %zu -> \"%s\" (relabelled %d)\n", output + 1,
		            bridge.outputLabel(seeded, output).c_str(), bridge.outputRelabelled(seeded, output) ? 1 : 0);
	}
	for (const std::string& name : exposed) {
		const DeviceInstanceId seeded = rackMode ? bridge.rackDeviceAt(rackSlot) : bridge.fxDeviceAt(slot);
		size_t at = device::kNoControl;
		for (size_t i = 0; desc != nullptr && i < desc->params.size(); ++i) {
			if (name == desc->params[i].name) {
				at = i;
			}
		}
		// READ BACK FROM THE POOL, and refused rather than photographed unexposed (see the set loop above).
		const bool pinned = at != device::kNoControl && bridge.exposeToHost(seeded, at, nullptr);
		const device::HostSlotStatus status = at != device::kNoControl ? bridge.hostSlotStatusOf(seeded, at)
		                                                               : device::HostSlotStatus {};
		std::printf("expose:   %-24s -> %s (slot %u)\n", name.c_str(), pinned ? "pinned" : "REFUSED", status.slot + 1);
		if (!pinned) {
			return 1;
		}
	}
	bridge.refresh();

	// THE PHASE, BEFORE THE EDITOR EXISTS, so the first frame it draws already has it — the same
	// order a loaded patch arrives in.
	if (havePhase) {
		// AND AN ENVELOPE ALONGSIDE IT, because the strip now draws both and a capture that published
		// only the phase would photograph the trace at rest — a picture of the feature switched off,
		// which is the least useful frame to inspect it in. Driven from the same flag: a capture asked
		// for a phase is a capture that wants the strip live.
		bridge.publishAutomodReadings(slot, static_cast<float>(phaseArg),
		                              static_cast<float>(envArg));
		std::printf("phase:    %.4f  envelope: %.4f (slot %zu)\n", phaseArg, envArg, slot);
	}

	// **A METER WITH NOTHING PUBLISHED PHOTOGRAPHS AS "no signal", WHICH IS THE ONE STATE THAT SAYS
	// LEAST ABOUT ITS LAYOUT.** Same argument as `--phase` above, one device along: Bus Utility's card
	// is mostly meter, and a capture of it at rest cannot answer whether the numbers are legible,
	// which is exactly what a capture of it is FOR. `--clip <seconds>` sets the age the LED draws
	// from, so the core, the glow and the dark ring are all photographable states.
	if (haveReadings) {
		std::array<float, static_cast<size_t>(fx::busutil::Reading::kCount)> readings {};
		readings[static_cast<size_t>(fx::busutil::Reading::PeakL)] = 0.72f;
		readings[static_cast<size_t>(fx::busutil::Reading::PeakR)] = 0.51f;
		readings[static_cast<size_t>(fx::busutil::Reading::Momentary)] = -14.2f;
		readings[static_cast<size_t>(fx::busutil::Reading::ShortTerm)] = -16.8f;
		readings[static_cast<size_t>(fx::busutil::Reading::Integrated)] = -18.3f;
		readings[static_cast<size_t>(fx::busutil::Reading::Correlation)] = 0.62f;
		readings[static_cast<size_t>(fx::busutil::Reading::ClipAge)] = static_cast<float>(clipArg);
		readings[static_cast<size_t>(fx::busutil::Reading::RawPeak)] = 0.836f;
		bridge.publishDeviceReadings(slot, readings.data(), readings.size());
		std::printf("readings: peak %.2f/%.2f  LUFS %.1f/%.1f/%.1f  corr %.2f  clip age %.2fs\n",
		            0.72, 0.51, -14.2, -16.8, -18.3, 0.62, clipArg);
	}

	if (!modulatedArg.empty() && !rackMode) {
		FxChain moved = bridge.fxChain();
		for (const auto& [name, value] : modulatedArg) {
			size_t at = device::kNoControl;
			for (size_t i = 0; desc != nullptr && i < desc->params.size(); ++i) {
				if (name == desc->params[i].name) {
					at = i;
				}
			}
			if (at == device::kNoControl) {
				std::printf("no control named \"%s\" on %s — refusing rather than modulating nothing\n", name.c_str(), tag);
				return 1;
			}
			moved.device[slot].row.field[at] = static_cast<float>(value);
			std::printf("modulated: %-24s -> %g\n", name.c_str(), value);
		}
		bridge.publishModulated(bridge.modRack(), moved, RackPlayheads {}, mod::TransportClock::unknown());
	}
	if (!modulatedRArg.empty() && !rackMode) {
		// THE RIGHT EAR (backlog KU): the values and the stereo-earMask mask, as `publishEngineReadings` publishes them.
		std::array<float, kMaxDeviceFields> right {};
		// X11's `ControlMask` (a modifier bit, X.h) is a MACRO; the device mask shares the spelling.
#pragma push_macro("ControlMask")
#undef ControlMask
		feathers::device::ControlMask earMask {};
#pragma pop_macro("ControlMask")
		for (const auto& [name, value] : modulatedRArg) {
			size_t at = device::kNoControl;
			for (size_t i = 0; desc != nullptr && i < desc->params.size(); ++i) {
				if (name == desc->params[i].name) {
					at = i;
				}
			}
			if (at == device::kNoControl || at >= right.size()) {
				std::printf("no control named \"%s\" on %s — refusing rather than modulating nothing\n", name.c_str(), tag);
				return 1;
			}
			right[at] = static_cast<float>(value);
			earMask.set(at);
			std::printf("right ear: %-24s -> %g\n", name.c_str(), value);
		}
		bridge.publishFxRightEar(slot, earMask, right);
	}

	if (haveOutputPeak) {
		for (size_t position = 0; position < bridge.fxChain().count; ++position) {
			const double steps = std::fabs(static_cast<double>(position) - static_cast<double>(slot));
			const double db = outputPeakDb - 6.0 * steps;
			bridge.publishFxOutputPeak(position, static_cast<float>(std::pow(10.0, db / 20.0)));
			std::printf("output peak: position %zu at %+.1f dBFS\n", position, db);
		}
	}

	if (!meterArg.empty()) {
		bridge.publishDeviceReadings(slot, meterArg.data(), meterArg.size());
		// OWLTT'S READINGS RIDE THE LANE'S OWN ROAD (`publishOwlttMeters`), not the generic one: the same list, laid out
		// in `Owltt::kReadingCount`'s order — including Key Track's note at 17 — so a shot can show a tracked note.
		if (device::sameTag(tag, fx::owltt::kTraits.tag) && meterArg.size() > 17) {
			const auto three = [&](size_t at) { return std::array<float, 3> { meterArg[at], meterArg[at + 1], meterArg[at + 2] }; };
			const std::array<float, 3> second = three(14);
			bridge.publishOwlttMeters(slot, three(0), three(3), three(6), meterArg[9] >= 0.5f,
			                          { meterArg[10] >= 0.5f, meterArg[11] >= 0.5f, meterArg[12] >= 0.5f }, meterArg[13], &second,
			                          meterArg[17]);
		}
		std::printf("meter:   ");
		for (const float v : meterArg) {
			std::printf(" %g", static_cast<double>(v));
		}
		std::printf(" (slot %zu)\n", slot);
	}

	// **THE TWO SPARK LEVELS, THROUGH THE GENERIC READINGS CHANNEL — the same road the audio thread
	// takes.** Published before the editor opens so the very first tick already has a level; the
	// `pump` after the rails are set is what lets the population settle to `rate × life`.
	std::array<float, fx::sklitter::kReadingCount> sk {};
	if (haveSparks) {
		sk[static_cast<size_t>(fx::sklitter::Reading::WetLevel)] = sparkWet;
		sk[static_cast<size_t>(fx::sklitter::Reading::GrainLevel)] = sparkGrain;
		bridge.publishDeviceReadings(slot, sk.data(), sk.size());
		std::printf("sklitter sparks: wet %.2f  grain %.2f\n", static_cast<double>(sparkWet),
		            static_cast<double>(sparkGrain));
	}

	if (!sklitterPlay.empty()) {
		namespace sk = fx::sklitter;
		std::vector<double> parts;
		for (size_t at = 0; at <= sklitterPlay.size();) {
			const size_t colon = std::min(sklitterPlay.find(':', at), sklitterPlay.size());
			parts.push_back(std::atof(sklitterPlay.substr(at, colon - at).c_str()));
			at = colon + 1;
		}
		static sk::Sklitter played;
		played.prepare(48000.0, 4096);
		sk::Settings cfg;
		cfg.mode = static_cast<sk::Mode>(static_cast<int>(parts.size() > 0 ? parts[0] : 0.0));
		cfg.enabled = true;
		cfg.rate = static_cast<float>(parts.size() > 1 ? parts[1] : 0.5);
		cfg.engage = parts.size() > 2 && parts[2] != 0.0;
		cfg.density = 1.0f;
		cfg.pWrite = 0.0f;
		cfg.zoneA = static_cast<float>(parts.size() > 3 ? parts[3] : 0.0);
		if (parts.size() > 5) {
			cfg.grooveMode = static_cast<float>(parts[4]);
			cfg.grooveAmount = static_cast<float>(parts[5]);
		}
		// `:<zone B>:<puck config>` — the puck on the second waypoint, whole, at that configuration (the
		// ratchet needs the puck: backlog IX). A configuration below zero leaves the puck centred.
		if (parts.size() > 6) {
			cfg.zoneB = static_cast<float>(parts[6]);
		}
		if (parts.size() > 7 && parts[7] >= 0.0) {
			cfg.cornerCfg[sk::kCfg2] = static_cast<float>(parts[7]);
			const auto [ax, ay] = sk::waypointAxis(sk::kCfg2);
			cfg.perfX = ax;
			cfg.perfY = ay;
		}
		// `:<seq bars>` — a one-bar sequence draws each cell four times as wide as the default four bars do.
		if (parts.size() > 8 && parts[8] > 0.0) {
			cfg.seqBars = static_cast<int32_t>(parts[8]);
		}
		played.setTarget(cfg);
		std::array<float, sk::kReadingCount> held {};
		played.setReadingsSink(device::ReadingsSink { &held, [](void* ctx, const float* v, size_t n) {
			                                              auto* out = static_cast<std::array<float, sk::kReadingCount>*>(ctx);
			                                              std::copy(v, v + std::min(n, out->size()), out->begin());
		                                              } });
		// A BAR AND A HALF PAST THE PRESS when engaged (the capture is a bar, then a beat boundary); one block
		// at rest.
		const int frames = cfg.engage ? 96000 + 96000 + 60000 : 480;
		std::vector<float> l(480);
		std::vector<float> r(480);
		double beats = 0.0;
		for (int done = 0; done < frames; done += 480) {
			for (size_t k = 0; k < l.size(); ++k) {
				l[k] = std::sin(static_cast<float>(done + static_cast<int>(k)) * 0.01f) * 0.5f;
				r[k] = l[k];
			}
			device::TransportClock clock = device::TransportClock::fromSecondsPerBeat(0.5);
			clock.songPositionBeats = beats;
			clock.playing = true;
			played.setTransportClock(clock);
			played.processStereo(l.data(), r.data(), 480);
			beats += 480.0 / 24000.0;
		}
		played.publishReadings();
		bridge.publishDeviceReadings(slot, held.data(), held.size());
		std::printf("sklitter play: mode %d rate %.3f %s  status %.0f steps %.0f step %.0f seqBars %.0f "
		            "rateN %.2f repeatLength %.4f\n",
		            static_cast<int>(cfg.mode), static_cast<double>(cfg.rate), cfg.engage ? "engaged" : "at rest",
		            static_cast<double>(held[0] /* Reading::Status — X11 defines `Status` */),
		            static_cast<double>(held[static_cast<size_t>(sk::Reading::Steps)]),
		            static_cast<double>(held[static_cast<size_t>(sk::Reading::Step)]),
		            static_cast<double>(held[static_cast<size_t>(sk::Reading::SeqBars)]),
		            static_cast<double>(held[static_cast<size_t>(sk::Reading::RateDivision)]),
		            static_cast<double>(held[static_cast<size_t>(sk::Reading::RepeatLength)]));
		for (size_t k = 0; k < static_cast<size_t>(held[static_cast<size_t>(sk::Reading::Steps)]); ++k) {
			const auto flags = static_cast<uint32_t>(held[sk::stepReading(k) + 1]);
			std::printf("  step %zu source %.4f flags %u span %u ratchet %u piece %.4f%s\n", k,
			            static_cast<double>(held[sk::stepReading(k)]), flags,
			            1u + ((flags >> sk::kStepSpanShift) & sk::kStepCountMask),
			            1u + ((flags >> sk::kStepRatchetShift) & sk::kStepCountMask),
			            static_cast<double>(held[sk::stepReading(k) + 2]),
			            (flags & static_cast<uint32_t>(sk::StepFlag::Covered)) != 0u ? " covered" : "");
		}
	}

	Gui gui(host, bridge);
	if (!Gui::isAvailable() || !gui.create(CLAP_WINDOW_API_X11, false)) {
		std::printf("gui.create failed — no X11 backend in this build\n");
		return 2;
	}
	// **THE CONTAINER IS AS BIG AS THE PLUGIN'S OWN FLOOR, THROUGH THE ROAD A DAW TAKES.** `setSize`
	// runs the asked size through `adjustSize` — the design at the scale in force, in native pixels
	// (backlog EJ) — so under `FEATHERS_UI_SCALE=1.75` the editor believed it was 2170x1225 while this
	// X window stayed at the logical 1240x700 and cut it to a 708x400-logical corner. A host asks
	// `adjust_size` before it makes the window, and so does this. MEASURED: the floor is 1240x734 at
	// 1.0 (the 700 plus the collapsed canvas strip) and 1860x1101 at 1.5. NB the xvfb venue's screen
	// is 1920x1080, so a 1.5x editor already overhangs it by 21 rows and a 1.75x one (2170x1285) by
	// far more; the capture comes back cut at the screen, not at the window.
	uint32_t width = shotWidth > 0 ? shotWidth : Gui::kLogicalWidth;
	uint32_t height = shotHeight > 0 ? shotHeight : Gui::kLogicalHeight;
	(void)gui.adjustSize(&width, &height);
	gui.setSize(width, height);
	std::printf("window:   %ux%u native for %ux%u logical\n", width, height, Gui::kLogicalWidth,
	            Gui::kLogicalHeight);

	// The container, left UNMAPPED across set_parent the way a real host leaves it — guihost spent a
	// release cycle mapping it early and excusing an editor that never opened in Bitwig.
	const int screen = DefaultScreen(display);
	Window parent = XCreateSimpleWindow(display, RootWindow(display, screen), 0, 0, width, height, 0,
	                                    BlackPixel(display, screen), BlackPixel(display, screen));
	XStoreName(display, parent, "feathers widget-shot");
	XFlush(display);

	clap_window window {};
	window.api = CLAP_WINDOW_API_X11;
	window.x11 = parent;
	if (!gui.setParent(&window)) {
		std::printf("gui.setParent failed\n");
		return 2;
	}
	XMapWindow(display, parent);
	XRaiseWindow(display, parent);
	XFlush(display);

	// **DRAINING X IS NOT DRAWING A FRAME**, and the first version of this loop did only the former.
	// Every capture came back at ONE COLOUR — a window that had never composited — because nothing
	// called into the editor. `guihost` gets this for free: it holds the plugin across CLAP and calls
	// `on_fd`, which lands on `Gui::processEvents()`. Reaching `Gui` directly means reaching that
	// method directly, and forgetting it produces a window that maps, sizes and stays black.
	double onsetDue = 0.0;
	double repeatDue = 0.0;
	size_t onsetNext = 0;
	auto onsetClock = std::chrono::steady_clock::now();
	const auto pump = [&](int milliseconds) {
		for (int i = 0; i < milliseconds / 4; ++i) {
			// THE ONSET COUNTERS ADVANCE WITH THE CLOCK, one class after another — see `--sklitter-onsets`.
			// **THE WALL CLOCK, NOT THE LOOP'S NOMINAL 4 ms** — MEASURED on xvfb, an iteration takes about
			// three times that, so crediting 4 ms each ran the grains at a third of the asked rate.
			if (haveSparks && (onsetRate > 0.0 || repeatPeriod > 0.0)) {
				const auto now = std::chrono::steady_clock::now();
				const double elapsed = std::chrono::duration<double>(now - onsetClock).count();
				onsetDue += onsetRate * elapsed;
				onsetClock = now;
				bool moved = false;
				if (repeatPeriod > 0.0) {
					repeatDue += elapsed / repeatPeriod;
					sk[static_cast<size_t>(fx::sklitter::Reading::RepeatPeriod)] = static_cast<float>(repeatPeriod);
					while (repeatDue >= 1.0) {
						repeatDue -= 1.0;
						sk[static_cast<size_t>(fx::sklitter::Reading::RepeatCount)] += 1.0f;
						moved = true;
					}
				}
				while (onsetDue >= 1.0) {
					onsetDue -= 1.0;
					sk[static_cast<size_t>(fx::sklitter::Reading::GrainOnsets) + onsetNext] += 1.0f;
					onsetNext = (onsetNext + 1) % fx::sklitter::kGrainOnsetClasses;
					moved = true;
				}
				if (moved) {
					bridge.publishDeviceReadings(slot, sk.data(), sk.size());
				}
			}
			gui.processEvents();
			while (XPending(display) > 0) {
				XEvent event;
				XNextEvent(display, &event);
			}
			usleep(4000);
		}
	};
	pump(1500);

	// THE ROAD THE CARD'S MARK TAKES, driven directly. `gui.h` exposes the lane for this and says
	// why: *"so a case can drive `onExpandDevice` — the road the card's expand mark actually takes —
	// instead of a seam invented for the test."* A panel opened through a bespoke hook would be a
	// picture of something no hand can reach.
	ui::FxLane* lane = gui.fxLaneForTest();
	if (lane == nullptr) {
		std::printf("no fx lane on this editor\n");
		return 2;
	}
	if (!expand) {
		// **AND THE CARD IS SCROLLED INTO VIEW, WHICH IS NOT COSMETIC.** `fx_lane.h`: *"the filter
		// card alone is 356, so ANY second card needs scrolling to be used at all"* — so a card
		// seeded through the bridge before this editor existed sits below the fold, and the first
		// shot of this strip came back with the bottom row cut off. The lane does exactly this when
		// a card is added by hand; the tool is taking the same road rather than a new one.
		lane->scrollCardIntoView(slot);
		std::printf("venue:    the card in the lane (--card), scrolled into view, no panel opened\n");
		if (foldArg) {
			if (ui::FxCard* card = lane->cardAt(slot); card != nullptr) {
				card->setFolded(true);
				lane->resized();
				std::printf("fold:     the card folded\n");
			}
		}
	}
	// THE ENGINE RAIL'S PAGE, turned through the panel's own verb — the call a click on the page's name makes.
	if (!engineRailPage.empty()) {
		ui::EngineRail* engineRail = findInTree<ui::EngineRail>(gui.root());
		if (engineRail == nullptr) {
			std::printf("no Engine rail on this editor\n");
			return 2;
		}
		engineRail->showPage(engineRailPage == "stack" ? ui::EngineRail::Page::Stack
		                                             : ui::EngineRail::Page::Pitch);
		pump(300);
		std::printf("Engine rail: the %s page\n", engineRailPage.c_str());
	}
	if (armMacro) {
		ui::ModState* mod = gui.modForTest();
		if (mod == nullptr) {
			std::printf("no mod state on this editor\n");
			return 2;
		}
		mod->focus(backplane::Source::core(0));
		(void)mod->arm(true);
		pump(400);
		std::printf("armed:    Macro 1\n");
	}
	if (armPair) {
		// THE STEREO-TINTED POTENTIAL-DESTINATION GLOW (backlog KU): a source armed over a stereo control.
		const size_t toolSlot = bridge.addRackDevice(mod::stereo::kTraits.tag);
		const DeviceInstanceId tool = toolSlot < kMaxRackSlots ? bridge.rackDeviceAt(toolSlot) : kNoInstance;
		if (tool == kNoInstance) {
			std::printf("--arm-pair: the Stereo Tool was refused — refusing rather than shooting\n");
			return 1;
		}
		bridge.refresh();
		ui::ModState* mod = gui.modForTest();
		if (mod == nullptr) {
			std::printf("no mod state on this editor\n");
			return 2;
		}
		mod->focus(backplane::Source::output(tool, static_cast<size_t>(mod::stereo::Output::Split)));
		(void)mod->arm(true);
		pump(400);
		std::printf("armed:    the Stereo Tool's Split (a pair)\n");
	}
	if (routeCurves) {
		// THE BRIDGE'S OWN VERBS — the ones the chip's pick and a ring drag take — then Macro 1 focused and
		// armed through the editor's state, so the route list shows its rows. REFUSED rather than shot if
		// any write is not taken: a picture of three Lin rows would look like a picture of the feature.
		const backplane::Source macro1 = backplane::Source::core(0);
		if (routeCurvesBipolar) {
			bridge.setMacroBipolar(0, true);
		}
		struct Seed {
			ParamId id;
			float amount;
			backplane::Transfer curve;
		};
		// THE PATH ROW IS THE ONE THE ENGINE RAIL SHOWS FIRST, so the coefficient flags curve it (Phi, then Exp,
		// then Log, whichever is given first in that order).
		const backplane::Transfer first = backplane::Transfer::Exp;
		const backplane::Transfer third =
		    routeCurvesBipolar               ? backplane::Transfer::Abs
		    : !std::isnan(routePhiOctaves)   ? device::phiTransfer(routePhiOctaves)
		    : !std::isnan(routeExpExponent)  ? device::exponentTransfer(backplane::Transfer::Exp, routeExpExponent)
		    : !std::isnan(routeLogExponent)  ? device::exponentTransfer(backplane::Transfer::Log, routeLogExponent)
		                                     : backplane::Transfer::Log;
		const Seed seeds[] = { { ParamId::VoiceLevel, 0.4f, first },
			                   { ParamId::FineTune, -0.3f, backplane::Transfer::Lin },
			                   { ParamId::Path, 0.6f, third } };
		for (const Seed& s : seeds) {
			const backplane::Destination d =
			    backplane::Destination::core(static_cast<size_t>(modDestinationIndexOf(s.id)));
			bridge.setRouteAmount(macro1, d, s.amount);
			if (bridge.routeAmount(macro1, d) == 0.0f
			    || (s.curve != backplane::Transfer::Lin && !bridge.setRouteTransfer(macro1, d, s.curve))) {
				std::printf("--route-curves: a route or curve was refused — refusing rather than shooting\n");
				return 1;
			}
			std::printf("route:    Macro 1 -> %s at %g through %s\n", findParam(static_cast<clap_id>(s.id))->name,
			            static_cast<double>(s.amount), device::transferName(s.curve));
		}
		bridge.refresh();
		ui::ModState* mod = gui.modForTest();
		if (mod == nullptr) {
			std::printf("no mod state on this editor\n");
			return 2;
		}
		mod->focus(macro1);
		(void)mod->arm(true);
		pump(400);
		if (routeCurvesMenu) {
			ui::RouteList* list = findInTree<ui::RouteList>(gui.root());
			const std::vector<ui::RouteList::Row> rows = list != nullptr ? list->allRows() : std::vector<ui::RouteList::Row> {};
			if (rows.empty()) {
				std::printf("--route-curves-menu: no route list rows to open a chip on\n");
				return 1;
			}
			(void)ui::showMenuAt(list->transferMenu(rows.front()), *list, list->chipBounds(0));
			std::printf("menu:     the first row's chip menu, opened against its chip\n");
		}
		pump(600);
	}
	if (expand && !lane->onExpandDevice) {
		// A REAL FAILURE, NOT A MISSING TEST HOOK. The lane installs this per card as it is built; an
		// empty callback here means the card's expand mark is dead in the shipping editor too.
		std::printf("onExpandDevice is EMPTY — the card's expand mark reaches nothing\n");
		return 1;
	}
	// **THE PANEL HAS TWO HOSTS AND ONE OF THEM IS A SEPARATE X WINDOW.** `kPopoutWindowKey` defaults
	// TRUE, so a plain call to `onExpandDevice` opens a floating window and the editor's own capture
	// comes back showing no panel at all — which is exactly what the first working frame of this tool
	// showed, a perfectly drawn editor with nothing opened on it. The panel was open the whole time,
	// in a window this process was not photographing.
	//
	// THE OVERLAY IS THE ONE TO PHOTOGRAPH, because it puts the widget in its CONTEXT: a panel inside
	// the editor is what a reader needs to judge whether it sits right among everything else. The
	// content is identical either way — that is the property `expanded_overlay.h` kept deliberately —
	// so choosing the host changes where the pixels land and nothing about what is drawn.
	if (expand && !rackMode) {
		gui.setPopoutWindowForTest(popout);
		lane->onExpandDevice(slot);
	}
	// **--site: THE MICROCHAIN, OPENED BY THE CARD'S OWN CHIP** (backlog NZ). This tool has no `Synth`, so nothing builds
	// the site; one is built here as the converger builds it — the real catalogue, the bridge's own gate — and filed
	// under the site's key, where the panel finds it by its per-tick road. Leaked on purpose: the panel holds bridges
	// into it.
	if (site && !rackMode) {
		const DeviceInstanceId id = bridge.fxDeviceAt(slot);
		if (!bridge.openFxSite(id)) {
			std::printf("--site: \"%s\" offers no site\n", tag);
			return 1;
		}
		auto* held = new rack::SiteChains(id, hostCatalogue(), 48000.0, 4096);
		held->setOuterGate(rack::ChainRecord::gateOver(&bridge.chainRecord(), id));
		DeviceInstanceId firstModulator = kNoInstance;
		for (const auto& [lanePart, tags] : { std::pair { &held->pre(), &sitePre }, std::pair { &held->post(), &sitePost } }) {
			for (const std::string& t : *tags) {
				const DeviceInstanceId added = lanePart->bridge()->addDevice(t.c_str());
				if (added == kNoInstance) {
					std::printf("--site: \"%s\" was refused\n", t.c_str());
					return 1;
				}
				const device::Descriptor* d = lanePart->bridge()->descriptorOf(added);
				if (lanePart == &held->pre() && firstModulator == kNoInstance && d != nullptr && d->mod != nullptr) {
					firstModulator = added;
				}
			}
		}
		bridge.fxBodies().attach(siteKeyOf(id), held);
		gui.setPopoutWindowForTest(popout);
		ui::FxCard* card = lane->cardAt(slot);
		if (card == nullptr || !card->onSite) {
			std::printf("--site: the card at slot %zu has no site chip\n", slot);
			return 1;
		}
		card->onSite();
		pump(400);
		auto* panel = findInTree<rack::SiteExpanded>(gui.root());
		if (panel == nullptr) {
			std::printf("--site: no site panel opened\n");
			return 1;
		}
		if (siteArm && firstModulator != kNoInstance) {
			panel->preModForTest().focus(backplane::Source::output(firstModulator, 0));
			panel->preModForTest().arm(true);
		}
		std::printf("site:     PRE %zu, POST %zu device(s); %s\n", held->deviceCount(rack::SiteChains::kPreLane),
		            held->deviceCount(rack::SiteChains::kPostLane),
		            siteArm ? "PRE's first modulator armed" : "nothing armed");
	}
	if (likes > 0) {
		ui::ExploreStage* explore = findInTree<ui::ExploreStage>(gui.root());
		if (explore == nullptr) {
			std::printf("--likes: no Explore stage in the tree\n");
			return 1;
		}
		// THE STAGE'S OWN VERBS: a roll moves the sound, the heart likes where it lands; the last stays under the star.
		for (int i = 0; i < likes; ++i) {
			explore->mutate();
			pump(60);
			explore->toggleLike();
			pump(60);
		}
		std::printf("venue:    Explore with %d like(s), the last under the star\n", likes);
	}
	if (spectrum) {
		gui.setPopoutWindowForTest(popout);
		ui::ExploreStage* explore = findInTree<ui::ExploreStage>(gui.root());
		if (explore == nullptr || !explore->onSpectrumTab) {
			std::printf("--spectrum: no Explore stage with a spectrum tab in the tree\n");
			return 1;
		}
		explore->onSpectrumTab();
		std::printf("venue:    the Additive spectrum panel, opened by its tab's road\n");
	}
	if (exploreSurface) {
		ui::HeaderBar* header = findInTree<ui::HeaderBar>(gui.root());
		if (header == nullptr) {
			std::printf("--explore: no header in the tree\n");
			return 1;
		}
		header->showSurfaceForTest(ui::StageMode::Surface::Explore);
		pump(200);
		std::printf("venue:    the Explore surface, by the header switch's road\n");
	}
	if (resizeToW > 0 && resizeToH > 0) {
		ui::ExploreStage* explore = findInTree<ui::ExploreStage>(gui.root());
		if (explore == nullptr) {
			std::printf("--resize-to: no Explore stage in the tree\n");
			return 1;
		}
		const double w0 = static_cast<double>(shotWidth > 0 ? shotWidth : Gui::kLogicalWidth);
		const double h0 = static_cast<double>(shotHeight > 0 ? shotHeight : Gui::kLogicalHeight);
		for (int k = 1; k <= resizeSteps; ++k) {
			const double t = static_cast<double>(k) / resizeSteps;
			uint32_t w = static_cast<uint32_t>(std::lround(w0 + (resizeToW - w0) * t));
			uint32_t h = static_cast<uint32_t>(std::lround(h0 + (resizeToH - h0) * t));
			(void)gui.adjustSize(&w, &h);
			XResizeWindow(display, parent, w, h);
			XFlush(display);
			gui.setSize(w, h);
			pump(32);
		}
		pump(2500);
		const visage::Bounds where = explore->terrainPlacement();
		const ui::StageDisk d = explore->disk();
		const std::shared_ptr<const ui::TerrainSnapshot> snap = explore->terrainForTest().snapshot();
		const ui::TerrainWindow asked = explore->askForTerrainForTest();
		const bool overhangs = where.x() < 0.0f || where.y() < 0.0f || where.right() > explore->width()
		                       || where.bottom() > explore->height();
		std::printf("resized:  %d step(s) to %ux%u: stage %.0fx%.0f; disk c %.1f,%.1f r %.1f; terrain at %.1f,%.1f %.1fx%.1f "
		            "(%s the stage); snapshot spans %.6g x %.6g centre %.6g,%.6g, asked %.6g x %.6g centre %.6g,%.6g (%s)\n",
		            resizeSteps, resizeToW, resizeToH, explore->width(), explore->height(), d.centre().x, d.centre().y,
		            d.radius(), where.x(), where.y(), where.width(), where.height(), overhangs ? "OVERHANGS" : "inside",
		            snap->window.travelXSpan, snap->window.travelYSpan, snap->window.travelXCentre,
		            snap->window.travelYCentre, asked.travelXSpan, asked.travelYSpan, asked.travelXCentre,
		            asked.travelYCentre,
		            snap->window.travelXSpan == asked.travelXSpan && snap->window.travelYSpan == asked.travelYSpan
		                    && snap->window.travelXCentre == asked.travelXCentre
		                    && snap->window.travelYCentre == asked.travelYCentre
		                ? "CURRENT"
		                : "STALE");
		const std::string stem = out.size() > 4 ? out.substr(0, out.size() - 4) : out;
		const std::string path = stem + "-resized.png";
		char command[1024];
		std::snprintf(command, sizeof(command), "import -depth 8 -window 0x%lx %s",
		              static_cast<unsigned long>(captureTarget(display, parent)), path.c_str());
		if (std::system(command) != 0) {
			return 1;
		}
		std::printf("grab:     %s\n", path.c_str());
	}
	if (collapseRails) {
		ui::ExploreStage* explore = findInTree<ui::ExploreStage>(gui.root());
		std::vector<ui::Rail*> rails;
		allInTree<ui::Rail>(gui.root(), rails);
		if (explore == nullptr) {
			return 1;
		}
		const auto report = [&](const char* what) {
			pump(1200);
			const visage::Bounds where = explore->terrainPlacement();
			const ui::StageDisk d = explore->disk();
			const bool overhangs = where.x() < 0.0f || where.y() < 0.0f || where.right() > explore->width()
			                       || where.bottom() > explore->height();
			std::printf("rails:    %-28s stage %.0fx%.0f disk r %.1f terrain %.1fx%.1f at %.1f,%.1f — %s\n", what,
			            explore->width(), explore->height(), d.radius(), where.width(), where.height(), where.x(),
			            where.y(), overhangs ? "OVERHANGS" : "inside");
		};
		report("start");
		std::vector<ui::Rail*> collapsed;
		for (ui::Rail* rail : rails) {
			if (!rail->isVisible() || !rail->setCollapsed(true)) {
				continue;
			}
			collapsed.push_back(rail);
			char what[64];
			std::snprintf(what, sizeof(what), "collapsed rail %zu", collapsed.size());
			report(what);
		}
		for (auto it = collapsed.rbegin(); it != collapsed.rend(); ++it) {
			(void)(*it)->setCollapsed(false);
		}
		report("all expanded again");
	}
	if (exploreTimeSteps > 0) {
		ui::ExploreStage* explore = findInTree<ui::ExploreStage>(gui.root());
		if (explore == nullptr) {
			std::printf("--explore-time: no Explore stage in the tree\n");
			return 1;
		}
		pump(1500);
		const auto measure = [&](int steps, bool zooming) {
			const int ground0 = explore->groundPaintsForTest();
			const int stage0 = explore->stagePaintsForTest();
			const uint64_t builds0 = explore->terrainForTest().builds();
			double inside = 0.0;
			double worst = 0.0;
			double cpu = 0.0;
			const auto threadCpuMs = [] {
				timespec ts {};
				clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
				return static_cast<double>(ts.tv_sec) * 1e3 + static_cast<double>(ts.tv_nsec) * 1e-6;
			};
			const auto wall0 = std::chrono::steady_clock::now();
			for (int k = 0; k < steps; ++k) {
				if (zooming) {
					explore->setZoom(explore->zoom() * (k < steps / 2 ? 1.06 : 1.0 / 1.06));
				}
				// PACED BY THE WALL CLOCK, as a hand is: one zoom notch every 40 ms however long a frame takes. (Paced by
				// frames instead, a slow frame stretched each notch past the zoom's rest time and every notch rebuilt —
				// a harness artifact, not the editor's behaviour.)
				const auto stepEnd = std::chrono::steady_clock::now() + std::chrono::milliseconds(40);
				for (int t = 0; t == 0 || std::chrono::steady_clock::now() < stepEnd; ++t) {
					const auto a = std::chrono::steady_clock::now();
					const double c0 = threadCpuMs();
					gui.processEvents();
					cpu += threadCpuMs() - c0;
					const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count();
					inside += ms;
					worst = std::max(worst, ms);
					while (XPending(display) > 0) {
						XEvent event;
						XNextEvent(display, &event);
					}
					usleep(4000);
				}
			}
			const double wall = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - wall0).count();
			const int grounds = explore->groundPaintsForTest() - ground0;
			const int stages = explore->stagePaintsForTest() - stage0;
			std::printf("time:     %s %d steps: wall %.0f ms, in processEvents %.0f ms (this thread's CPU %.0f ms, worst call "
			            "%.1f ms); ground paints %d, stage paints %d, terrain builds %llu; per stage paint %.2f ms wall, %.2f ms "
			            "CPU\n",
			            zooming ? "ZOOMING" : "AT REST", steps, wall, inside, cpu, worst, grounds, stages,
			            static_cast<unsigned long long>(explore->terrainForTest().builds() - builds0),
			            stages > 0 ? inside / stages : 0.0, stages > 0 ? cpu / stages : 0.0);
		};
		measure(exploreTimeSteps, false);
		measure(exploreTimeSteps, true);
	}
	if (!exploreZooms.empty()) {
		ui::ExploreStage* explore = findInTree<ui::ExploreStage>(gui.root());
		if (explore == nullptr) {
			std::printf("--explore-zooms: no Explore stage in the tree\n");
			return 1;
		}
		const std::string stem = out.size() > 4 ? out.substr(0, out.size() - 4) : out;
		for (size_t z = 0; z < exploreZooms.size(); ++z) {
			explore->setZoom(exploreZooms[z]);
			pump(exploreZoomPump);
			const visage::Bounds where = explore->terrainPlacement();
			const ui::StageDisk d = explore->disk();
			std::printf("zoom:     %g (asked %g): stage %.0fx%.0f native %dx%d dpi %.2f; disk c %.1f,%.1f r %.1f; "
			            "terrain at %.1f,%.1f %.1fx%.1f aspect %.4f\n",
			            explore->zoom(), exploreZooms[z], explore->width(), explore->height(), explore->nativeWidth(),
			            explore->nativeHeight(), static_cast<double>(explore->dpiScale()), d.centre().x, d.centre().y,
			            d.radius(), where.x(), where.y(), where.width(), where.height(),
			            where.height() > 0.0f ? where.width() / where.height() : 0.0f);
			char suffix[32];
			std::snprintf(suffix, sizeof(suffix), "-z%zu.png", z);
			const std::string path = stem + suffix;
			char command[1024];
			std::snprintf(command, sizeof(command), "import -depth 8 -window 0x%lx %s",
			              static_cast<unsigned long>(captureTarget(display, parent)), path.c_str());
			if (std::system(command) != 0) {
				std::printf("--explore-zooms: `import` failed for %s\n", path.c_str());
				return 1;
			}
			std::printf("grab:     %s\n", path.c_str());
		}
	}
	if (radiusRouteBase >= 0.0) {
		const backplane::Source macro1 = backplane::Source::core(0);
		const backplane::Destination d = backplane::Destination::core(travelDestinationIndex(0, TravelAxis::Radius));
		bridge.setSoundOrbit(0, { radiusRouteBase, 0.0 });
		bridge.setMacroBipolar(0, radiusRouteBipolar);
		bridge.setRouteAmount(macro1, d, radiusRouteDepth);
		bridge.refresh();
		ui::ModState* mod = gui.modForTest();
		ui::ExploreStage* explore = findInTree<ui::ExploreStage>(gui.root());
		if (mod == nullptr || explore == nullptr || bridge.routeAmount(macro1, d) != radiusRouteDepth) {
			std::printf("--radius-route: no mod state, no Explore stage, or the route was refused\n");
			return 1;
		}
		mod->focus(macro1);
		(void)mod->arm(true);
		pump(400);
		const ui::Knob::ModRing ring = explore->radiusKnobForTest(0).modRingForTest();
		std::printf("route:    Macro 1%s -> A Radius at %g, base %g (face %.4f); ring %.4f..%.4f drawn %d\n",
		            radiusRouteBipolar ? " (bipolar)" : "", static_cast<double>(radiusRouteDepth), radiusRouteBase,
		            radiusFace(radiusRouteBase), static_cast<double>(ring.lo), static_cast<double>(ring.hi),
		            ring.drawn ? 1 : 0);
	}
	if (drumKit) {
		gui.setPopoutWindowForTest(popout);
		ui::ExploreStage* explore = findInTree<ui::ExploreStage>(gui.root());
		if (explore == nullptr || !explore->onDrumTab) {
			std::printf("--drum-kit: no Explore stage with a drum tab in the tree\n");
			return 1;
		}
		explore->onDrumTab();
		std::printf("venue:    the drum kit panel, opened by its tab's road\n");
	}
	pump(2000);

	// **--rails PUTS SKLITTER'S TWO EXPANSION RAILS IN A STATED STATE, so each one can be
	// photographed.** They are editor-local bools with no parameter behind them — deliberately, since
	// which panels you like open is a fact about your desk and not about the sound (`rail.h`) — so
	// there is no `--dset` that can reach them and the camera would otherwise only ever see the
	// defaults. Driven through the rail's own `setCollapsed`, which is the road the chevron click
	// takes, rather than by synthesising a press: `windowed-probe.cpp` bans pointer synthesis on this
	// display and the ban is not conditional on which display happens to be up.
	if (!railArg.empty() || !bodyArg.empty()) {
		// **FOUND BY WALKING THE FRAME TREE, NOT BY ASKING THE OVERLAY FOR ITS CONTENT.** The obvious
		// road — `ExpandedOverlay::contentForTest()` — returns the LAST slot's content, and a
		// `dynamic_cast` on it came back null with the panel plainly on screen: the overlay holds more
		// than one slot here, so "the last one" is not "the one I seeded". The tree cannot be wrong
		// about which frames exist, so it is asked instead.
		ui::ExpandedOverlay* overlay = gui.expandedForTest();
		ui::SklitterExpanded* panel = findInTree<ui::SklitterExpanded>(gui.root());
		if (panel == nullptr || overlay == nullptr) {
			std::printf("--rails/--body asked for and no SKLITTER panel is in the tree\n");
			return 1;
		}
		// **THE DEFAULTS ARE THE PANEL'S OWN**, so `--body` alone photographs the folded grid against
		// the rails as they open, and `--rails` alone keeps its meaning unchanged.
		const bool circleOpen = railArg.empty()
		                            ? !panel->circleRailForTest().collapsed()
		                            : (railArg == "circle" || railArg == "both");
		const bool grainOpen = railArg.empty()
		                           ? !panel->grainRailForTest().collapsed()
		                           : (railArg == "fx" || railArg == "both" || railArg == "grain");
		const bool postOpen = railArg.empty()
		                          ? !panel->postRailForTest().collapsed()
		                          : (railArg == "fx" || railArg == "both" || railArg == "post");
		const bool bodyOpen = bodyArg != "collapsed";
		// **THE APPLIED RECTANGLE, NOT THE ONE THE LAYER WOULD COMPUTE IF ASKED.** `panelBounds()` goes
		// through `panelSizeOf`, which reads `minimumSize()` live — it would print the new number on a
		// build where nothing was ever laid out. The panel frame's own width is what a layout pass
		// wrote. (The same trap caught the case for this in `test_gui_widgets_devices`.)
		const auto appliedWidth = [&] {
			return overlay->panelForTest() != nullptr
			           ? static_cast<float>(overlay->panelForTest()->width())
			           : 0.0f;
		};
		const float before = appliedWidth();
		if (panel->bodyRailForTest() != nullptr) {
			panel->bodyRailForTest()->setCollapsed(!bodyOpen);
		}
		panel->circleRailForTest().setCollapsed(!circleOpen);
		panel->grainRailForTest().setCollapsed(!grainOpen);
		panel->postRailForTest().setCollapsed(!postOpen);
		// **AND NOTHING IS DONE TO THE OVERLAY HERE, WHICH IS THE POINT.** This block used to call
		// `overlay->resized()` by hand, *"otherwise a tray that just opened is drawn inside the old
		// rectangle and clipped"* — a workaround in the CAMERA for a road the editor did not have. The
		// rail announces its new width, the panel re-derives and tells its host its minimum moved
		// (`ExpandedView::onMinimumSizeChanged`), and the host fits it. Photographing that road rather
		// than around it is what makes the capture evidence about the editor.
		// **AND THE HEADROOM, BECAUSE THE PANEL IS CLAMPED TO THE LAYER.** `panelSizeOf` bounds a
		// panel by the overlay's width and `expanded_overlay.h` is deliberate about what happens then:
		// a content that does not fit "is clipped rather than squashed". So the number that decides
		// whether an open rail is honest or cut off is the content's own minimum against the layer,
		// and it is printed rather than left to be worked out from a picture.
		std::printf("rails:    body %s, circle %s, grain %s, post %s — panel %.0f -> %.0f px wide; content "
		            "minimum %.0f + chrome %.0f vs layer %.0f (%s)\n",
		            bodyOpen ? "OPEN" : "closed", circleOpen ? "OPEN" : "closed",
		            grainOpen ? "OPEN" : "closed", postOpen ? "OPEN" : "closed", before, appliedWidth(),
		            panel->minimumSize().x, 2.0f * ui::ExpandedOverlay::kPanelPad,
		            static_cast<float>(overlay->width()),
		            panel->minimumSize().x + 2.0f * ui::ExpandedOverlay::kPanelPad
		                    <= static_cast<float>(overlay->width())
		                ? "FITS"
		                : "CLIPPED");
		// **THE SPARK PATH, SET BEFORE THE SETTLING PUMP** so the population that lands in the frame was
		// drawn by the path under test for its whole life rather than for the last few frames.
		if (sparkCpu) {
			panel->padForTest().setSparkGpuForTest(false);
			std::printf("sparks:   CPU reference path (vs_spark.sc bypassed)\n");
		}
		pump(1200);
	}

	// **--chain-window: THE CHAIN DEVICE'S WINDOW OVER A RUNNING BODY** (backlog OC) — `--rack-chains`' gap for the Chain.
	if (chainWindow) {
		rack::ChainExpanded* panel = findInTree<rack::ChainExpanded>(gui.root());
		if (panel == nullptr) {
			std::printf("--chain-window asked for and no Chain window is in the tree\n");
			return 1;
		}
		const device::AudioOps& audio = *rack::kDeviceDescriptor.audio;
		void* held = ::operator new(audio.stateBytes, std::align_val_t { audio.stateAlign });
		audio.construct(held);
		rack::kDeviceDescriptor.container->adopt(held, hostCatalogue());
		audio.prepare(held, 48000.0, 4096);
		bridge.fxBodies().attach(bridge.fxDeviceAt(slot), held);
		(void)rack::bridgeOf(held)->addDevice(fx::filter::kTraits.tag);
		// --chain-rack: a Rack beside the Filter, the Rack card's h-chain venue.
		if (!chainRack.empty()) {
			namespace pr = rack::parallel;
			int mode = 0;
			int chains = 1;
			int bands = 0;
			(void)std::sscanf(chainRack.c_str(), "%d:%d:%d", &mode, &chains, &bands);
			rack::RackBridge& inner = *rack::bridgeOf(held);
			const DeviceInstanceId rackId = inner.addDevice(pr::kTraits.tag);
			inner.setField(rackId, static_cast<size_t>(pr::Param::Mode), static_cast<float>(mode));
			inner.setField(rackId, static_cast<size_t>(pr::Param::BandCount), static_cast<float>(bands));
			inner.drain();
			if (pr::Rack* rk = pr::rackOf(inner.containerSurfaceOf(rackId)); rk != nullptr) {
				while (rk->chainCount() < static_cast<size_t>(chains)) {
					(void)rk->addChain();
				}
			}
			std::printf("chain-rack: mode %d, %d chain(s), bands index %d\n", mode, chains, bands);
		}
		// --chain-with: any device beside the Filter, one control set by its declared name.
		for (const std::string& spec : chainWith) {
			const size_t colon = spec.find(':');
			const std::string tagWanted = spec.substr(0, colon);
			rack::RackBridge& inner = *rack::bridgeOf(held);
			const DeviceInstanceId added = inner.addDevice(tagWanted.c_str());
			if (added == kNoInstance) {
				std::printf("--chain-with: \"%s\" was refused\n", tagWanted.c_str());
				return 1;
			}
			if (colon != std::string::npos) {
				const std::string set = spec.substr(colon + 1);
				const size_t eq = set.find('=');
				const device::Descriptor* d = inner.descriptorOf(added);
				bool found = false;
				for (size_t c = 0; d != nullptr && eq != std::string::npos && c < d->params.size(); ++c) {
					if (set.substr(0, eq) == d->params[c].name) {
						inner.setField(added, c, std::stof(set.substr(eq + 1)));
						found = true;
					}
				}
				if (!found) {
					std::printf("--chain-with: no control \"%s\" on %s\n", set.c_str(), tagWanted.c_str());
					return 1;
				}
			}
			inner.drain();
			std::printf("chain-with: %s\n", spec.c_str());
		}
		// --chain-select N (backlog OA): N more devices in the window's chain, the first N selected in its editor.
		for (size_t n = 0; n < chainSelect; ++n) {
			(void)rack::bridgeOf(held)->addDevice(n % 2 == 0 ? mod::lfo::kTraits.tag : fx::filter::kTraits.tag);
		}
		(void)panel->updateContent();
		pump(600);
		(void)panel->updateContent();
		if (chainSelect > 0 && panel->editorForTest() != nullptr) {
			rack::RackBridge& inner = *rack::bridgeOf(held);
			std::vector<DeviceInstanceId> ids;
			for (size_t p = 0; p < chainSelect && p < inner.chainLength(); ++p) {
				ids.push_back(inner.deviceAtPosition(p));
			}
			panel->editorForTest()->syncCards();
			panel->editorForTest()->selectionForTest().select(ids);
			std::printf("chain-select: %zu card(s) selected in the Chain window\n", ids.size());
			if (selectMenu && !ids.empty()) {
				panel->editorForTest()->showSlotMenu(ids.front());
				std::printf("select-menu: the first selected card's dots menu opened\n");
			}
		}
		pump(1200);
		std::printf("chain:    window minimum %.0f x %.0f, macro column needs %.0f (with Mix/Gain), %.0f (without)\n",
		            panel->minimumSize().x, panel->minimumSize().y, rack::RackEditor::columnNeeded(true, false),
		            rack::RackEditor::columnNeeded(false, false));
	}

	// **--rack-chains: THE RACK'S WINDOW OVER RUNNING CHAINS** (backlog OC). Same gap as `--chain-add`: no `Synth`, so
	// nothing files a body and the window draws "0 chains". A real Rack is built, adopts the real catalogue, is filed under
	// the device's id, gets N chains (a filter in the first, a level apiece) and a few blocks of signal so every header's
	// meter has a peak to read; the panel then follows it through its own per-tick road.
	if (rackChains > 0) {
		ui::ParallelRackExpanded* panel = findInTree<ui::ParallelRackExpanded>(gui.root());
		// WITH --card THERE IS NO WINDOW, and the CARD follows the rack by the same road (rack stage 2, step 4).
		if (panel == nullptr && expand) {
			std::printf("--rack-chains asked for and no Rack window is in the tree\n");
			return 1;
		}
		namespace pr = rack::parallel;
		const device::AudioOps& audio = pr::kAudioOps;
		void* held = ::operator new(audio.stateBytes, std::align_val_t { audio.stateAlign });
		audio.construct(held);
		pr::kContainerOps.adopt(held, hostCatalogue());
		audio.prepare(held, 48000.0, 4096);
		bridge.fxBodies().attach(bridge.fxDeviceAt(slot), held);
		pr::Rack& rack = *pr::rackOf(held);
		while (rack.chainCount() < static_cast<size_t>(rackChains)) {
			(void)rack.addChain();
		}
		(void)rack.chainAt(0)->bridge()->addDevice(fx::filter::kTraits.tag);
		const float levels[] = { 0.75f, 0.5f, 0.25f, 0.9f };
		for (size_t c = 0; c < rack.chainCount(); ++c) {
			rack.setLevel(c, levels[c % 4]);
		}
		if (rack.chainCount() > 2) {
			rack.setMuted(2, true);
		}
		if (rack.chainCount() > 1) {
			rack.setSoloed(1, true);
			rack.setSoloed(0, true);
		}
		std::vector<float> l(256);
		std::vector<float> r(256);
		for (int b = 0; b < 8; ++b) {
			for (size_t i = 0; i < l.size(); ++i) {
				l[i] = r[i] = 0.4f * std::sin(0.05f * static_cast<float>(i + 256 * static_cast<size_t>(b)));
			}
			audio.processStereo(held, l.data(), r.data(), 256);
		}
		if (panel == nullptr) {
			pump(600);
			pump(1200);
			std::printf("rack:     %zu chain(s), card only\n", rack.chainCount());
		}
		else {
			(void)panel->updateContent();
			if (rackChainsOpen) {
				panel->railForTest(0).setCollapsed(false);
			}
			panel->setBounds(0.0f, 0.0f, std::max(static_cast<float>(panel->width()), panel->minimumSize().x),
			                 std::max(static_cast<float>(panel->height()), panel->minimumSize().y));
			pump(600);
			(void)panel->updateContent();
			pump(1200);
			std::printf("rack:     %zu chain(s), panel minimum %.0f x %.0f\n", rack.chainCount(), panel->minimumSize().x,
			            panel->minimumSize().y);
		}
	}

	// **--resonate-drone / --resonate-wet: RESONATE'S LANES, BOUND AND OPEN.** The same gap `--chain-add` fills for
	// SKLITTER: this tool has no `Synth`, so nothing files a running instance and the rails would say "not running
	// here". One is built — the device's own ops, adopting the real catalogue — and filed under the device's id; the
	// rails then find it by the panel's own per-tick road. Leaked on purpose: the rails hold bridges into it.
	if (!resonateDrone.empty() || !resonateWet.empty()) {
		ui::ResonateExpanded* panel = findInTree<ui::ResonateExpanded>(gui.root());
		if (panel == nullptr) {
			std::printf("--resonate-* asked for and no Resonate panel is in the tree\n");
			return 1;
		}
		const device::AudioOps& audio = *fx::resonate::kDescriptor.audio;
		void* held = ::operator new(audio.stateBytes, std::align_val_t { audio.stateAlign });
		audio.construct(held);
		fx::resonate::kDescriptor.container->adopt(held, hostCatalogue());
		audio.prepare(held, 48000.0, 4096);
		bridge.fxBodies().attach(bridge.fxDeviceAt(slot), held);
		(void)panel->updateContent();
		panel->droneRailForTest().setCollapsed(false);
		panel->wetRailForTest().setCollapsed(false);
		ui::ChainColumn* drone = panel->droneRailForTest().columnForTest();
		ui::ChainColumn* wet = panel->wetRailForTest().columnForTest();
		if (drone == nullptr || wet == nullptr) {
			std::printf("--resonate-*: the rails did not bind both lanes\n");
			return 1;
		}
		for (const std::string& tag : resonateDrone) {
			if (drone->addDevice(tag.c_str()) == kNoInstance) {
				std::printf("--resonate-drone \"%s\": the lane refused it\n", tag.c_str());
				return 1;
			}
		}
		for (const std::string& tag : resonateWet) {
			if (wet->addDevice(tag.c_str()) == kNoInstance) {
				std::printf("--resonate-wet \"%s\": the lane refused it\n", tag.c_str());
				return 1;
			}
		}
		pump(600);
		(void)panel->updateContent();
		pump(1200);
		ui::ExpandedOverlay* overlay = gui.expandedForTest();
		std::printf("resonate: drone lane %zu device(s), wet lane %zu; switch %s; panel minimum %.0f x %.0f, layer %.0f wide\n",
		            drone->deviceCount(), wet->deviceCount(),
		            panel->droneRailForTest().accessoryShownForTest() ? "SHOWN" : "hidden", panel->minimumSize().x,
		            panel->minimumSize().y, overlay != nullptr ? static_cast<float>(overlay->width()) : 0.0f);
	}

	// **--chain-add PUTS DEVICES IN SKLITTER'S POST CHAIN, AND THE HARNESS HAS TO SUPPLY THE INSTANCE
	// ITSELF.** In the plugin the chain is reached through `DeviceHost::containerSurfaceOf`, which
	// answers with the storage a host filed for the running device (`ContainerBodies::attach`, from
	// `Synth::provisionDevices`). **This tool has no `Synth`** — it seeds the bridge by hand and opens
	// the editor over it — so nothing is ever attached and the rail correctly draws "not running
	// here". That is an accurate report about this harness and a useless picture.
	//
	// So one is built here: a real `fx::sklitter::Sklitter`, adopting the real catalogue, filed under
	// the real device id. **The pointer is exactly what a host would file** — the device IS the
	// storage — so what the column then draws is the genuine road and not a stand-in for it. What is
	// simulated is the PROVISIONING, which this tool does not have; what is photographed is the editor.
	//
	// It outlives the shot by being a `static`: the rail holds a `RackBridge*` into it until the panel
	// closes, and a local would take the chain out from under the capture.
	if (!chainAdd.empty() || !grainAdd.empty() || chainLive) {
		ui::SklitterExpanded* panel = findInTree<ui::SklitterExpanded>(gui.root());
		if (panel == nullptr) {
			std::printf("--chain-add asked for and no SKLITTER panel is in the tree\n");
			return 1;
		}
		static fx::sklitter::Sklitter held;
		// **BOTH LANES ADOPT, THROUGH THE OPS TABLE RATHER THAN BY HAND.** Adopting `postChain()`
		// alone left the grain lane with no catalogue and no bridge, so the rail's second column was
		// never built and the picture showed one lane while the engine had two — trap T5, arriving in
		// the harness rather than in the code. `ContainerOps::adopt` is the call that fans out.
		fx::sklitter::kDescriptor.container->adopt(&held, hostCatalogue());
		bridge.fxBodies().attach(bridge.fxDeviceAt(slot), &held);
		// THE RAIL RE-ASKS BY ID EVERY TICK, so this is the tick that finds it — driven through the
		// panel's own road rather than by calling `follow` here, so what the picture shows is the road
		// the editor takes.
		(void)panel->updateContentForTest();
		ui::ChainColumn* column = panel->postRailForTest().columnForTest();
		ui::ChainColumn* grainColumn = panel->grainRailForTest().columnForTest();
		if (column == nullptr || grainColumn == nullptr) {
			std::printf("--chain-add: the rail did not bind both lanes (master=%p grain=%p)\n",
			            static_cast<const void*>(column), static_cast<const void*>(grainColumn));
			return 1;
		}
		for (const std::string& tag : chainAdd) {
			if (column->addDevice(tag.c_str()) == kNoInstance) {
				std::printf("--chain-add \"%s\": the chain refused it (unknown tag, or full?)\n",
				            tag.c_str());
				return 1;
			}
			std::printf("chain-add: %s -> master lane position %zu\n", tag.c_str(),
			            column->deviceCount() - 1);
		}
		for (const std::string& tag : grainAdd) {
			if (grainColumn->addDevice(tag.c_str()) == kNoInstance) {
				std::printf("--grain-add \"%s\": the chain refused it (unknown tag, or full?)\n",
				            tag.c_str());
				return 1;
			}
			std::printf("grain-add: %s -> grain lane position %zu\n", tag.c_str(),
			            grainColumn->deviceCount() - 1);
		}
		pump(1200);

		// **--chain-expand OPENS A PANEL FROM INSIDE THE CHAIN, WHICH IS THE POINT OF THE WHOLE
		// ARRANGEMENT AND THE ONE STATE A SINGLE-PANEL CAPTURE CANNOT REACH.** It drives the column's
		// own verb rather than hit-testing the card's mark, for the reason every seam in this tool
		// exists: a rectangle restated here would be this tool checking its own arithmetic. That the
		// MARK is drawn at all is a separate claim, and it is the card's `expandBounds()` — reported
		// below so a capture says whether the way in exists as well as where the panel went.
		for (const size_t position : chainExpand) {
			if (position >= column->deviceCount()) {
				std::printf("--chain-expand %zu: the chain holds %zu device(s)\n", position,
				            column->deviceCount());
				return 1;
			}
			const DeviceInstanceId device = column->deviceAt(position);
			const ui::FxCard* card = column->cardOf(device);
			const bool wayIn = card != nullptr && card->expandBounds().width() > 0.0f;
			const size_t before = column->expandedCountForTest();
			column->expandDevice(device);
			const size_t after = column->expandedCountForTest();
			std::printf("chain-expand: position %zu (instance %u) — mark %s, panels %zu -> %zu\n",
			            position, static_cast<unsigned>(device), wayIn ? "DRAWN" : "ABSENT", before,
			            after);
			if (after == before) {
				std::printf("--chain-expand %zu: nothing opened\n", position);
				return 1;
			}
			pump(600);
		}

		// **--chain-drag: THE BUMPER, PHOTOGRAPHED MID-DRAG.** The press goes to the card itself at its
		// head and the drag carries it over the gap past the last card, through the card's own handlers
		// — the same events a window delivers to the frame that took the press. No release: the drop
		// indicator only exists while the hand is still down.
		if (chainDrag >= 0) {
			const size_t position = static_cast<size_t>(chainDrag);
			if (position >= column->deviceCount()) {
				std::printf("--chain-drag %zu: the chain holds %zu device(s)\n", position,
				            column->deviceCount());
				return 1;
			}
			ui::FxCard* card = column->cardOf(column->deviceAt(position));
			if (card == nullptr) {
				std::printf("--chain-drag %zu: no card\n", position);
				return 1;
			}
			const visage::Point pressAt { card->width() * 0.3f, ui::FxCard::kSlotHeadHeight * 0.5f };
			const size_t gap = position + 1 < column->deviceCount() ? column->deviceCount() : 0;
			const visage::Bounds bumper = column->gapBounds(gap);
			const visage::Point target = column->positionInWindow()
			                             + visage::Point { card->x() + pressAt.x,
				                                           bumper.y() + bumper.height() * 0.5f };
			const auto eventAt = [&](visage::Point inCard, bool isDown) {
				visage::MouseEvent e {};
				e.event_frame = card;
				e.position = inCard;
				e.window_position = card->positionInWindow() + inCard;
				e.button_id = visage::kMouseButtonLeft;
				e.button_state = isDown ? visage::kMouseButtonLeft : visage::kMouseButtonNone;
				e.is_down = isDown;
				e.repeat_click_count = 1;
				return e;
			};
			card->processMouseDown(eventAt(pressAt, true));
			card->processMouseDrag(eventAt(target - card->positionInWindow(), true));
			const std::optional<size_t> held = column->dropGap();
			std::printf("chain-drag: position %zu held over gap %s (asked for %zu), dragging %s\n",
			            position, held.has_value() ? std::to_string(*held).c_str() : "NONE", gap,
			            column->dragDevice().has_value() ? "YES" : "no");
			if (!held.has_value() || *held != gap) {
				return 1;
			}
			pump(600);
		}

		// **THE WALL, PHOTOGRAPHED.** `nestingFull()` is read from the column's nest, so walking the nest
		// down to `kMaxUsefulDepth` puts the column in exactly the state a twenty-deep player would be
		// in — no drawing is faked and no guard is bypassed. Then the (+) menu is opened, which is where
		// the congratulation lives.
		if (chainAtWall) {
			ui::DeviceNest deep;
			while (static_cast<size_t>(deep.depth) < rack::kMaxUsefulDepth) {
				deep = ui::DeviceNest::under(deep, 1);
			}
			column->setNest(deep);
			std::printf("chain-at-wall: nest depth %u, nestingFull %s\n",
			            static_cast<unsigned>(column->nest().depth),
			            column->nestingFull() ? "YES" : "no");
			column->showAddMenu();
			pump(600);
		}
	}

	if (haveDrag) {
		ui::ExpandedOverlay* overlay = gui.expandedForTest();
		ui::ExpandedPanel* panel = overlay != nullptr ? overlay->panelForTest() : nullptr;
		if (panel == nullptr || !overlay->open()) {
			std::printf("--drag asked for and no panel is open\n");
			return 1;
		}
		const visage::Bounds before = overlay->panelBounds();
		// THE HEAD, in window pixels: the middle of the title row, clear of the ✕.
		const visage::Point head = panel->positionInWindow()
		                           + visage::Point { panel->width() * 0.5f,
			                                         ui::ExpandedOverlay::kTitleHeight * 0.5f };
		visage::Frame* down = gui.root()->frameAtPoint(head);
		if (down == nullptr) {
			std::printf("--drag: nothing under the panel's head at (%.0f, %.0f)\n", head.x, head.y);
			return 1;
		}
		const auto eventAt = [&](visage::Point windowPosition, bool isDown) {
			visage::MouseEvent e;
			e.window_position = windowPosition;
			e.position = windowPosition - down->positionInWindow();
			e.event_frame = down;
			e.button_id = visage::kMouseButtonLeft;
			e.button_state = isDown ? visage::kMouseButtonLeft : visage::kMouseButtonNone;
			e.is_down = isDown;
			e.repeat_click_count = 1;
			return e;
		};
		down->processMouseDown(eventAt(head, true));
		down->processMouseDrag(eventAt(head + dragBy, true));
		down->processMouseUp(eventAt(head + dragBy, false));
		pump(800);
		const visage::Bounds after = overlay->panelBounds();
		std::printf("drag:     asked (%.0f, %.0f); panel (%.0f, %.0f) -> (%.0f, %.0f) in a %.0fx%.0f layer\n",
		            dragBy.x, dragBy.y, before.x(), before.y(), after.x(), after.y(),
		            overlay->width(), overlay->height());
	}

	// **--panel-grow DRAGS THE GRIP, WHICH IS THE ONLY WAY A HAND MAKES A PANEL BIGGER.** The grip
	// writes `slot.sized`, `panelSizeOf` maxes it against the content's minimum, and the content is
	// given the difference — so this is the road the window enlargement takes and not a seam beside it.
	//
	// **AFTER `--drag`, BECAUSE THE GRIP IS CLAMPED FROM WHERE THE PANEL SITS.** `resizePanelBy` bounds
	// the far corner by this layer's edge, so a centred panel can only grow into half the headroom; a
	// hand that wants it big moves it left first, and the tool takes the same two steps in the same
	// order.
	//
	// **AND THE NUMBERS ARE PRINTED, BECAUSE A CLAIM THAT SOMETHING SCALES CANNOT BE MADE FROM A
	// PICTURE.** Two renders differing is evidence that something changed, never that the right thing
	// did (CLAUDE.md). So the disk's own radius comes out here, at whatever size the grip reached, and
	// the picture is the second reading rather than the only one.
	// **--then WRITES A CONTROL UNDER THE OPEN PANEL AND LETS THE TICK NOTICE.** The panel's height is
	// printed before and after, because the claim this exists to photograph — "the panel re-fits when
	// its grid grows" — is a claim about a number, and the picture is the second reading of it.
	if (!then.empty()) {
		if (rackMode || !expand) {
			std::printf("--then is for a device panel opened in the overlay (not --card, not --rack)\n");
			return 1;
		}
		ui::ExpandedOverlay* overlay = gui.expandedForTest();
		if (overlay == nullptr || overlay->panelForTest() == nullptr) {
			std::printf("--then asked for and no panel is open\n");
			return 1;
		}
		const float heightBefore = static_cast<float>(overlay->panelForTest()->height());
		for (const auto& [name, value] : then) {
			size_t at = device::kNoControl;
			for (size_t i = 0; desc != nullptr && i < desc->params.size(); ++i) {
				if (name == desc->params[i].name) {
					at = i;
					break;
				}
			}
			if (at == device::kNoControl) {
				std::printf("--then: no control named \"%s\" on %s — refusing rather than setting nothing\n",
				            name.c_str(), tag);
				return 1;
			}
			bridge.setDeviceControl(bridge.fxDeviceAt(slot), at, static_cast<float>(value));
			std::printf("then:     %-24s -> %g (under the open panel)\n", name.c_str(), value);
		}
		pump(600);
		std::printf("panel:    %.0f -> %.0f px tall after --then\n", heightBefore,
		            static_cast<float>(overlay->panelForTest()->height()));
	}

	// THE BROWSER A --preset-* OPTION NAMES: the editor header's, or the open SKLITTER panel's.
	const auto browserFor = [&](const std::string& where) -> ui::PresetBrowser* {
		if (where == "header") {
			return gui.presetBrowserForTest();
		}
		if (ui::SklitterExpanded* sk = findInTree<ui::SklitterExpanded>(gui.root()); sk != nullptr) {
			return &sk->presetBrowserForTest();
		}
		return nullptr;
	};
	const std::string presetWhere = !presetList.empty()     ? presetList.substr(0, presetList.find(':'))
	                                : !presetPrompt.empty() ? presetPrompt.substr(0, presetPrompt.find(':'))
	                                                        : std::string("panel");
	if (presetSeed) {
		ui::PresetBrowser* browser = browserFor(presetWhere);
		if (browser == nullptr) {
			std::printf("--preset-seed: no %s browser\n", presetWhere.c_str());
			return 1;
		}
		const std::pair<const char*, const char*> seeds[] = {
			{ "Tape Chop", "tape, dark" },          { "Glass Stutter", "bright, glitch" },
			{ "Stutters/Half Time", "glitch" },     { "Stutters/Ratchet Rain", "glitch, rhythmic" },
			{ "Textures/Dust Cloud", "dark, ambient" }, { "Textures/Shimmer Bed", "ambient, bright" },
		};
		for (const auto& [name, tags] : seeds) {
			if (!browser->saveAs(name, parseTagList(tags))) {
				std::printf("--preset-seed: \"%s\" was not saved\n", name);
			}
		}
		std::printf("seed:     %zu presets in %s\n", browser->count(), PresetStore::directory(browser->bank()).c_str());
		pump(300);
	}

	if (!presetPrompt.empty()) {
		const size_t colon = presetPrompt.find(':');
		ui::PresetBrowser* browser = browserFor(presetWhere);
		if (browser == nullptr) {
			std::printf("--preset-prompt: no %s browser to open\n", presetWhere.c_str());
			return 1;
		}
		browser->chooseFromMenu(ui::PresetBrowser::kMenuSave);
		if (colon != std::string::npos) {
			const std::string typed = presetPrompt.substr(colon + 1);
			const size_t bar = typed.find('|');
			browser->typeForTest(typed.substr(0, bar));
			if (bar != std::string::npos) {
				browser->typeTagsForTest(typed.substr(bar + 1));
			}
		}
		pump(600);
		std::printf("prompt:   %s browser, name \"%s\", tags \"%s\"\n", presetWhere.c_str(),
		            browser->promptTextForTest().c_str(), browser->tagsTextForTest().c_str());
		// THE GEOMETRY, AS NUMBERS: the panel and its name field, logical and native, beside the window's
		// width — a claim about a scale is a claim about the NATIVE rectangle (CLAUDE.md).
		const visage::Frame* p = browser->promptFrameForTest();
		const visage::Frame* f = browser->nameFieldForTest();
		std::printf("prompt:   panel x %.0f w %.0f (native x %d w %d, dpi %.2f); field w %.0f native %d; "
		            "window %.0f wide (native %d)\n",
		            p->x(), p->width(), p->nativeX(), p->nativeWidth(), p->dpiScale(), f->width(), f->nativeWidth(),
		            gui.root()->width(), gui.root()->nativeWidth());
	}

	if (!presetList.empty()) {
		ui::PresetBrowser* browser = browserFor(presetWhere);
		if (browser == nullptr) {
			std::printf("--preset-list: no %s browser\n", presetWhere.c_str());
			return 1;
		}
		const bool tags = presetList.find(":tags") != std::string::npos;
		// THE VIEW ROW ALONE: choosing it reopens the list in that view on the next loop turn, which is the
		// road a hand takes — no second `showPresetMenu`, which would stack a list on the reopened one.
		browser->chooseFromMenu(tags ? ui::PresetBrowser::kMenuByTag : ui::PresetBrowser::kMenuByFolder);
		pump(600);
		std::printf("list:     %s browser, by %s, %zu rows\n", presetWhere.c_str(), tags ? "tag" : "folder",
		            browser->listRows().size());
		if (presetManage) {
			// THE SUBMENU OPENED BY A HOVER DELIVERED IN-PROCESS to the popup's own list — the road
			// `--fx-add-menu` takes; no X11 pointer is synthesised or moved.
			visage::Frame* top = browser;
			while (top->parent() != nullptr) {
				top = top->parent();
			}
			visage::PopupList* list = findInTree<visage::PopupList>(top);
			int row = -1;
			for (int r = 0; list != nullptr && r < list->numOptions(); ++r) {
				if (list->option(r).name().toUtf8() == ui::PresetBrowser::kManageMenu) {
					row = r;
				}
			}
			if (row < 0) {
				std::printf("--preset-manage: no Manage row in the open list\n");
				return 1;
			}
			const float rowY = 0.5f * static_cast<float>(list->yForIndex(row) + list->yForIndex(row + 1));
			visage::MouseEvent e {};
			e.event_frame = list;
			e.position = { list->width() * 0.5f, rowY };
			e.window_position = list->positionInWindow() + e.position;
			list->processMouseMove(e);
			std::printf("list:     hovered \"%s\"\n", list->option(row).name().toUtf8().c_str());
			pump(1200);
		}
	}

	if (owlttModeMenu) {
		// OWLTT'S MODE MENU, OPENED THROUGH THE PANEL'S OWN `showModeMenu` — a report of a dropdown
		// that did not appear beside its box named this panel. Prints where the rows landed against the
		// box, in the popup's own frame, so the photograph has a number beside it.
		visage::Frame* host = popout && gui.expandedWindowForTest() != nullptr
		                          ? static_cast<visage::Frame*>(gui.expandedWindowForTest()->windowForTest())
		                          : gui.root();
		ui::OwlttExpanded* panel = findInTree<ui::OwlttExpanded>(host);
		if (panel == nullptr) {
			std::printf("--owltt-mode-menu asked for and no OWLTT panel is open\n");
			return 1;
		}
		panel->showModeMenu();
		pump(600);
		const ui::MenuPlacement placed = ui::lastMenuPlacementForTest();
		std::printf("menu:     box %.0f,%.0f %.0fx%.0f — rows %.0f,%.0f %.0fx%.0f — window %.0fx%.0f\n",
		            placed.anchor.x(), placed.anchor.y(), placed.anchor.width(), placed.anchor.height(),
		            placed.list.x(), placed.list.y(), placed.list.width(), placed.list.height(),
		            placed.windowWidth, placed.windowHeight);
		if (popout) {
			// THE POPOUT IS ANOTHER X WINDOW, so the whole screen is taken — this venue's screen is a
			// private Xvfb, so there is nothing on it that is not this process's.
			const std::string screen = out.substr(0, out.rfind('.')) + "-popout.png";
			const std::string command = "import -depth 8 -window root " + screen;
			if (std::system(command.c_str()) != 0) {
				std::printf("capture of the popout failed\n");
				return 1;
			}
			std::printf("popout:   %s\n", screen.c_str());
		}
	}

	if (havePanelGrow) {
		ui::ExpandedOverlay* overlay = gui.expandedForTest();
		ui::SklitterExpanded* panel = findInTree<ui::SklitterExpanded>(gui.root());
		if (overlay == nullptr || overlay->panelForTest() == nullptr) {
			std::printf("--panel-grow asked for and no panel is open\n");
			return 1;
		}
		const float beforeWide = static_cast<float>(overlay->panelForTest()->width());
		overlay->beginPanelResize();
		overlay->resizePanelBy(panelGrow);
		pump(600);
		std::printf("grow:     grip +%.0fx%.0f — panel %.0f -> %.0f px wide\n", panelGrow.x, panelGrow.y,
		            beforeWide, static_cast<float>(overlay->panelForTest()->width()));
		if (panel != nullptr) {
			const ui::CircleRail& circle = panel->circleRailForTest();
			const ui::PerformPad& pad = panel->padForTest();
			const auto disk = pad.disk();
			const ui::CircleRail::Layout at = circle.layout();
			std::printf("circle:   rail %.0fx%.0f (ask %.0f, surplus %.0f) — room %.0fx%.0f, cells at "
			            "±%.0f,±%.0f, pad %.0fx%.0f, disk radius %.1f, scale %.2f\n",
			            static_cast<float>(circle.width()), static_cast<float>(circle.height()),
			            ui::CircleRail::kWidth, panel->surplusWidth(), at.across, at.down, at.dx, at.dy,
			            static_cast<float>(pad.width()), static_cast<float>(pad.height()), disk.radius,
			            pad.scale());
		}
	}

	// WHICH LFO PICTURE THE AUTOMOD CARD DRAWS, reached through the tree exactly as `--preview`
	// is, and for the same stated reason: which of two shapes is right is a LOOKING question, so the
	// tool photographs either without a rebuild.
	if (!preview.empty()) {
		// THE CANDIDATES, BY NAME. `lfo` is the waveform alone; `stacked` and `side` put a response
		// curve below it or beside it; a `+band` suffix shades the corner's travel across the sweep.
		const bool band = preview.size() > 5 && preview.compare(preview.size() - 5, 5, "+band") == 0;
		const std::string layout = band ? preview.substr(0, preview.size() - 5) : preview;
		if (layout != "lfo" && layout != "dual") {
			std::printf("--preview takes lfo or dual (optionally +band), got \"%s\"\n",
			            preview.c_str());
			return 2;
		}
		size_t switched = 0;
		std::function<void(visage::Frame*)> walkPreview = [&](visage::Frame* frame) {
			if (auto* card = dynamic_cast<ui::AutomodCard*>(frame)) {
				card->setPicture(layout == "dual" ? ui::AutomodCard::Picture::Dual
				                                  : ui::AutomodCard::Picture::LfoOnly);
				card->setResponseStyle(band ? ui::AutomodCard::ResponseStyle::Band
				                            : ui::AutomodCard::ResponseStyle::Plain);
				if (!gridArg.empty()) {
					card->setLayout(gridArg == "uniform" ? ui::AutomodCard::Layout::Uniform
					                                     : ui::AutomodCard::Layout::Compact);
				}
				++switched;
			}
			for (visage::Frame* child : frame->children()) {
				walkPreview(child);
			}
		};
		walkPreview(gui.root());
		std::printf("preview:  %s (%zu card(s))\n", preview.c_str(), switched);
		if (switched == 0) {
			// REFUSING RATHER THAN PHOTOGRAPHING THE OTHER VARIANT. A shot taken with the preview
			// silently unset looks exactly like a shot taken with it set, which is this tool's whole
			// stated failure mode.
			std::printf("no AutomodCard found — the preview was asked for and reached nothing\n");
			return 1;
		}
		pump(800);
	}

	// **--rack-add ADDS A MODULATOR TO AN EDITOR THAT IS ALREADY OPEN, WHICH IS A DIFFERENT STATE FROM
	// `--rack` AND THE ONLY ONE SOME DEFECTS LIVE IN.**
	//
	// `--rack` seeds the bridge BEFORE `Gui` exists, so the strip's card is built during the editor's
	// opening sequence and is laid out again before the first frame is grabbed. A layout that follows
	// a layout hides every fault whose subject is *the pass that builds the card*: on the second pass
	// the card already has a width, so anything that was read off a zero width comes out right.
	// MEASURED, on the mod strip's one-row-foot defect: the `--rack` shot was BYTE-IDENTICAL across
	// the broken and fixed builds over the whole strip column, while the same state reached through
	// this flag differs.
	//
	// **SO IT TAKES THE ROAD THE PLAYER'S HAND TAKES.** `ModPanel::addChoiceForTest` is the (+)
	// picker's own entry into `addDevice`, which ends in exactly ONE `resized()` and then a `redraw()`
	// — the sequence behind the report that the UI did not render until it was perturbed. No pointer is
	// synthesised; `windowed-probe.cpp`'s ban is not conditional on which display is up.
	if (!rackAdd.empty()) {
		ui::ModPanel* strip = findInTree<ui::ModPanel>(gui.root());
		if (strip == nullptr) {
			std::printf("--rack-add asked for and no ModPanel is in the tree\n");
			return 1;
		}
		// BY TAG, AGAINST WHAT THE PICKER ACTUALLY OFFERS — not by index, so a device joining or
		// leaving the menu moves nothing here and a tag the menu does not carry is refused rather than
		// silently photographing its neighbour. This tool's rule at every other door.
		const std::vector<const device::Descriptor*> offered = strip->addableForTest();
		size_t choice = offered.size();
		for (size_t i = 0; i < offered.size(); ++i) {
			if (device::sameTag(offered[i]->traits.tag, rackAdd.c_str())) {
				choice = i;
			}
		}
		if (choice >= offered.size()) {
			std::printf("--rack-add \"%s\": the strip's (+) does not offer that tag — refusing rather "
			            "than photographing a strip it never joined\n",
			            rackAdd.c_str());
			return 1;
		}
		const size_t landed = strip->addChoiceForTest(choice);
		if (landed == kNoPosition) {
			std::printf("--rack-add \"%s\": the rack refused it (full?)\n", rackAdd.c_str());
			return 1;
		}
		std::printf("rack-add: %s -> RACK slot %zu, through the (+) on an open editor\n",
		            rackAdd.c_str(), landed);
		const device::Descriptor& added = *offered[choice];
		for (const auto& [name, value] : rackSet) {
			size_t at = added.params.size();
			for (size_t p = 0; p < added.params.size(); ++p) {
				if (name == added.params[p].name) {
					at = p;
				}
			}
			if (at >= added.params.size()) {
				std::printf("--rack-set: no control named \"%s\" on %s — refusing rather than setting "
				            "nothing\n",
				            name.c_str(), rackAdd.c_str());
				return 1;
			}
			bridge.setDeviceControl(bridge.rackDeviceAt(landed), at, static_cast<float>(value));
			std::printf("rack-set: %-24s -> %g\n", name.c_str(), value);
		}
		if (rackScrollEnd) {
			strip->setScroll(strip->maxScroll());
		}
		if (!rackMenu.empty()) {
			// THE BOX'S OWN `showChoiceMenu`, called in-process — the road its press takes, with no
			// pointer synthesised. Found by the control's declared name among the card's value boxes.
			pump(300);
			ui::ValueBox* opened = nullptr;
			std::function<void(visage::Frame*)> walk = [&](visage::Frame* f) {
				if (auto* box = dynamic_cast<ui::ValueBox*>(f);
				    box != nullptr && rackMenu == box->control().desc().name) {
					opened = box;
				}
				for (visage::Frame* child : f->children()) {
					walk(child);
				}
			};
			walk(strip);
			if (opened == nullptr || !opened->showChoiceMenu()) {
				std::printf("--rack-menu \"%s\": no value box with a menu for that control — refusing\n",
				            rackMenu.c_str());
				return 1;
			}
			std::printf("menu:     %s opened\n", rackMenu.c_str());
		}
		// **REDRAW ONLY — NO SECOND LAYOUT.** A `resized()` here would be the very thing this flag
		// exists to avoid, and so would anything that changes the window's size. `pump` runs frames;
		// a frame is not a layout.
		pump(1200);
	}

	// **--rack-scroll: THE STRIP SCROLLED, so a card below its fold can be photographed.** Through
	// `ModPanel::setScroll`, the wheel's own road; the value it landed on is printed, since the panel
	// clamps it to its range.
	if (rackScroll >= 0.0f) {
		ui::ModPanel* strip = findInTree<ui::ModPanel>(gui.root());
		if (strip == nullptr) {
			std::printf("--rack-scroll asked for and no ModPanel is in the tree\n");
			return 1;
		}
		strip->setScroll(rackScrollMax ? strip->maxScroll() : rackScroll);
		std::printf("rack-scroll: %.1f of %.1f\n", static_cast<double>(strip->scroll()),
		            static_cast<double>(strip->maxScroll()));
		pump(800);
	}
	// **--stepseq-randomise: THE GRID HOLDING A PATTERN** (backlog MX), through the card's own Randomise — the act a
	// hand's press runs — once per lane letter, the grid left showing the last lane named.
	if (!stepseqRandomise.empty()) {
		std::vector<ui::StepSeqCard*> cards;
		const std::function<void(visage::Frame*)> walk = [&](visage::Frame* frame) {
			if (auto* card = dynamic_cast<ui::StepSeqCard*>(frame); card != nullptr) {
				cards.push_back(card);
			}
			for (visage::Frame* child : frame->children()) {
				walk(child);
			}
		};
		walk(gui.root());
		for (ui::StepSeqCard* card : cards) {
			for (const char c : stepseqRandomise) {
				const std::string lanes = "vgprs";
				const size_t at = lanes.find(c);
				if (at == std::string::npos) {
					std::printf("--stepseq-randomise: '%c' is not one of vgprs\n", c);
					return 2;
				}
				card->grid().setLane(static_cast<mod::stepseq::Lane>(at));
				(void)card->randomise();
			}
		}
		std::printf("stepseq-randomise: %zu card(s), lanes %s\n", cards.size(), stepseqRandomise.c_str());
		pump(400);
	}
	// **--trig-press: A HAND ON EVERY TRIG PAD** (backlog MV), so the shot shows the lit jack. Through `TrigPad::press`,
	// the mouse-down's own road; nothing is armed, so each press is a fire by hand, released before the editor closes.
	std::vector<ui::TrigPad*> trigPads;
	if (trigPress) {
		const std::function<void(visage::Frame*)> walk = [&](visage::Frame* frame) {
			if (auto* pad = dynamic_cast<ui::TrigPad*>(frame); pad != nullptr) {
				trigPads.push_back(pad);
			}
			for (visage::Frame* child : frame->children()) {
				walk(child);
			}
		};
		walk(gui.root());
		for (ui::TrigPad* pad : trigPads) {
			pad->press();
		}
		std::printf("trig-press: %zu pad(s) held\n", trigPads.size());
		pump(400);
	}
	// --env-peak-hover: AN OPT-HOVER ON EVERY DAHDSR CARD'S PEAK (backlog MN), through the card's own hover handler —
	// in process, no pointer moved, the `--trig-press` road — so the shot shows the bend's highlight. What each card
	// caught is printed, so a shot of nothing lit says why.
	if (envPeakHover) {
		std::vector<ui::EnvCard*> envCards;
		const std::function<void(visage::Frame*)> walk = [&](visage::Frame* frame) {
			if (auto* card = dynamic_cast<ui::EnvCard*>(frame); card != nullptr) {
				envCards.push_back(card);
			}
			for (visage::Frame* child : frame->children()) {
				walk(child);
			}
		};
		walk(gui.root());
		for (ui::EnvCard* card : envCards) {
			const visage::Bounds box = card->curveBounds();
			visage::MouseEvent e;
			e.position = { box.x() + card->stageEndX(mod::dahdsr::Stage::Attack) * box.width(), box.y() + 2.0f };
			e.modifiers = visage::kModifierAlt;
			card->onMouseMove().callback(e);
			const ui::EnvCard::BendTarget lit = card->highlightForTest();
			std::printf("env-peak-hover: kind %d stages 0x%02x\n", static_cast<int>(lit.kind), lit.stages);
		}
		std::printf("env-peak-hover: %zu card(s)\n", envCards.size());
		pump(400);
	}
	// --select-fx N: THE FIRST N CARDS OF THE FX LANE SELECTED (backlog OA), through the lane's own selection — what a
	// Shift-press makes — so every selected card's frame and the "N selected" pill are photographed. --select-menu then
	// opens the first selected card's dots menu, the verbs in it.
	if (selectFx > 0) {
		std::vector<DeviceInstanceId> ids;
		for (size_t p = 0; p < selectFx && p < bridge.chainRecord().length(feathers::kLaneChain); ++p) {
			ids.push_back(bridge.chainRecord().deviceAt(feathers::kLaneChain, p));
		}
		lane->selectionForTest().select(ids);
		std::printf("select-fx: %zu card(s) selected\n", ids.size());
		pump(600);
		if (selectMenu && !ids.empty()) {
			lane->showSlotMenu(bridge.fxSlotOf(ids.front()));
			std::printf("select-menu: the first selected card's dots menu opened\n");
			pump(600);
		}
		// --select-verb: one of the menu's verbs, picked on the first selected card — the menu's own road.
		if (!selectVerb.empty() && !ids.empty()) {
			const ui::FragmentVerb verb = selectVerb == "group-rack" ? ui::FragmentVerb::GroupRack
			                              : selectVerb == "duplicate" ? ui::FragmentVerb::Duplicate
			                                                          : ui::FragmentVerb::GroupChain;
			const bool done = lane->slotVerbForTest(bridge.fxSlotOf(ids.front()), verb);
			std::printf("select-verb: %s %s; the lane holds %zu\n", selectVerb.c_str(), done ? "done" : "REFUSED",
			            bridge.chainRecord().length(feathers::kLaneChain));
			pump(800);
		}
	}
	// --lane-scroll: THE FX LANE SCROLLED, by `FxLane::setScroll` (the wheel's road), the landing printed.
	if (laneScroll >= 0.0f) {
		lane->setScroll(laneScrollMax ? lane->maxScroll() : laneScroll);
		std::printf("lane-scroll: %.1f of %.1f\n", static_cast<double>(lane->scroll()),
		            static_cast<double>(lane->maxScroll()));
		pump(800);
	}

	if (fxAddMenu >= -1) {
		lane->showAddMenu();
		pump(600);
		// THE POPUP'S FIRST LIST — the top level, whose rows are the category headings.
		// Hung on the lane's TOPMOST ancestor, which is above `gui.root()`.
		visage::Frame* top = lane;
		while (top->parent() != nullptr) {
			top = top->parent();
		}
		visage::PopupList* list = findInTree<visage::PopupList>(top);
		if (list == nullptr) {
			std::printf("--fx-add-menu: no popup list opened\n");
			return 1;
		}
		std::printf("fx-add-menu: %d top-level rows\n", list->numOptions());
		if (fxAddMenu >= 0) {
			if (fxAddMenu >= list->numOptions()) {
				std::printf("--fx-add-menu %d: the menu has %d rows\n", fxAddMenu, list->numOptions());
				return 1;
			}
			// THE ROW'S MIDDLE, from the list's own row pitch (its option height is a theme value private
			// to popup_menu.cpp).
			const float rowY = 0.5f * static_cast<float>(list->yForIndex(fxAddMenu) + list->yForIndex(fxAddMenu + 1));
			visage::MouseEvent e {};
			e.event_frame = list;
			e.position = { list->width() * 0.5f, rowY };
			e.window_position = list->positionInWindow() + e.position;
			list->processMouseMove(e);
			std::printf("fx-add-menu: hovered \"%s\"\n", list->option(fxAddMenu).name().toUtf8().c_str());
			pump(1200);
		}
	}

	if (!hover.empty()) {
		if (!tooltipDetail.empty()) {
			(void)ui::setTooltipDetail(tooltipDetail == "full" ? ui::TooltipDetail::Full : ui::TooltipDetail::Brief);
		}
		// THE FIRST WIDGET BOUND TO THAT CONTROL, found by the control's declared name — through the
		// widgets' own accessors, so what is hovered is what a hand would hover.
		visage::Frame* found = nullptr;
		std::function<void(visage::Frame*)> walk = [&](visage::Frame* f) {
			if (found != nullptr || f == nullptr || !f->isVisible()) {
				return;
			}
			const device::Modulatable* c = nullptr;
			if (auto* k = dynamic_cast<ui::Knob*>(f)) {
				c = k->control();
			}
			else if (auto* b = dynamic_cast<ui::ValueBox*>(f)) {
				c = &b->control();
			}
			else if (auto* s = dynamic_cast<ui::SwitchChip*>(f)) {
				c = &s->control();
			}
			else if (auto* p = dynamic_cast<ui::FaderPill*>(f)) {
				// A PILL BY ITS AMOUNT — the first control it offers (backlog IL: a stack's readout follows hover).
				std::vector<ui::ControlFace::Offered> offered;
				p->offeredControls(offered);
				c = offered.empty() ? nullptr : offered.front().control;
			}
			if (c != nullptr && f->width() > 0.0f && hover == c->desc().name) {
				found = f;
				return;
			}
			for (visage::Frame* child : f->children()) {
				walk(child);
			}
		};
		walk(gui.root());
		if (found == nullptr) {
			std::printf("--hover \"%s\": no visible knob, box, chip or pill bound to that control — refusing\n",
			            hover.c_str());
			return 1;
		}
		visage::Frame* hostFrame = nullptr;
		ui::TooltipHost* host = ui::tooltipHostOf(*found, &hostFrame);
		if (host == nullptr) {
			std::printf("--hover \"%s\": the widget has no tooltip host above it\n", hover.c_str());
			return 1;
		}
		static double fakeNow = 0.0;
		host->tooltipLayer().setClock([] { return fakeNow; });
		const visage::Point local = hoverAt.x >= 0.0f ? hoverAt
		                                              : visage::Point { found->width() * 0.5f, found->height() * 0.5f };
		visage::MouseEvent e {};
		e.event_frame = found;
		e.position = local;
		e.window_position = found->positionInWindow() + local;
		// ONE `import` OF THE PLUGIN WINDOW, for --hover-sequence. Not the colour-counting loop below:
		// these three are compared with each other by hash, which is a stronger check than "drawn".
		const std::string stem = out.size() > 4 ? out.substr(0, out.size() - 4) : out;
		const auto grab = [&](const char* suffix) {
			const std::string path = stem + suffix;
			char command[1024];
			std::snprintf(command, sizeof(command), "import -depth 8 -window 0x%lx %s",
			              static_cast<unsigned long>(captureTarget(display, parent)), path.c_str());
			if (std::system(command) != 0) {
				std::printf("--hover-sequence: `import` failed for %s\n", path.c_str());
				return false;
			}
			std::printf("grab:     %s\n", path.c_str());
			return true;
		};
		if (hoverSequence) {
			pump(900);
			if (!grab("-before.png")) {
				return 1;
			}
		}
		found->processMouseEnter(e);
		fakeNow += 2.0 * ui::TooltipLayer::kDelayMs;
		pump(900);
		if (hoverSequence && !grab("-shown.png")) {
			return 1;
		}
		const ui::TooltipLayer& layer = host->tooltipLayer();
		const visage::Bounds box = layer.box();
		std::printf("hover:    \"%s\" %s — box %.1f,%.1f %.1fx%.1f in a %.0fx%.0f host (scale %.2f), %zu line(s)\n",
		            hover.c_str(), layer.shown() ? "SHOWN" : "NOT SHOWN", box.x(), box.y(), box.width(),
		            box.height(), hostFrame->width(), hostFrame->height(), static_cast<double>(layer.dpiScale()),
		            layer.lines().size());
		std::printf("detail:   %s\n", ui::tooltipDetail() == ui::TooltipDetail::Full ? "full" : "brief");
		for (const std::string& line : layer.lines()) {
			std::printf("          | %s\n", line.c_str());
		}
		if (!layer.shown()) {
			return 1;
		}
		if (hoverSequence) {
			found->processMouseExit(e);
			pump(900);
			std::printf("hover:    after exit %s\n", layer.shown() ? "STILL SHOWN" : "hidden");
			if (!grab("-hidden.png")) {
				return 1;
			}
		}
	}

	// **AN ALL-BLACK GRAB IS `import` SUCCEEDING**, and it is the documented failure of this venue
	// rather than a possibility being guarded against in the abstract: `windowed-probe.cpp` measured
	// 12 runs producing 1 blank frame — *"deviation exactly 0, 260 bytes"* — against ~140 KB for a
	// real one. The very first run of this tool produced exactly that, 260 bytes and two colours, and
	// reported "shot:" as if it had photographed something.
	//
	// SO THE FRAME MUST EARN THE WORD. A window that has not finished its first compositing pass
	// hands back one colour; a panel that drew hands back thousands. Counting distinct colours is the
	// cheapest thing that separates them, and it CANNOT pass on a blank — which is the property a
	// file-size threshold or an exit code does not have.
	// **THE STEREO TOOL'S SCOPE IS PHOTOGRAPHED MID-SWEEP** — last before the grab, because a scope scrolls with time:
	// any settling pump after the sweep would scroll held values across the whole plot and flatten the traces.
	// `--twinfield-tab`: THE TWIN FIELD CARD'S TAB, turned through the card's own verb, then a pump so the pad draws it.
	if (twinfieldTab >= 0) {
		ui::TwinFieldCard* card = findInTree<ui::TwinFieldCard>(gui.root());
		if (card == nullptr) {
			std::printf("--twinfield-tab: no Twin Field card in this editor\n");
			return 1;
		}
		card->show(static_cast<size_t>(std::clamp(twinfieldTab, 0, 1)));
		std::printf("twinfield: tab %d shown\n", static_cast<int>(card->shown()));
		pump(300);
	}
	// `--twinfield-play`: RUN THE DEVICE OFFLINE AT ITS ROW and publish its two orbit phases on the playhead channel the
	// card's 2x4 block reads — `--arp-play`'s road, since this tool runs no audio. In the fx lane only.
	if (twinfieldPlay) {
		namespace tfd = mod::twinfield;
		const FxChain chain = bridge.fxChain();
		if (rackMode || slot >= chain.count || !device::sameTag(chain.device[slot].row.tag.data(), tfd::kTraits.tag)) {
			std::printf("--twinfield-play: the shot device is not a TFIELD in the lane\n");
			return 1;
		}
		std::array<float, tfd::at(tfd::Param::kCount)> fields {};
		for (size_t i = 0; i < fields.size(); ++i) {
			fields[i] = chain.device[slot].row.field[i];
		}
		tfd::TwinField field;
		field.prepare(48000.0);
		field.setFields(fields.data(), fields.size());
		device::ModTickContext context {};
		for (int t = 0; t < 2000; ++t) {
			field.tick(context);
		}
		mod::Playhead where {};
		where.ordinal = 0;
		where.phase = static_cast<float>(field.phaseRead(0));
		where.level = static_cast<float>(field.phaseRead(1));
		where.haveLevel = true;
		bridge.publishFxPlayhead(slot, where);
		bridge.refresh();
		pump(300);
		std::printf("twinfield: published A %.3f, B %.3f; Mod 1 %.3f, Mod 8 %.3f\n", static_cast<double>(where.phase),
		            static_cast<double>(where.level), static_cast<double>(field.outputAt(0)),
		            static_cast<double>(field.outputAt(7)));
	}
	if (stereoTab >= 0 || stereoSweep) {
		ui::StereoToolCard* card = findInTree<ui::StereoToolCard>(gui.root());
		if (card == nullptr) {
			std::printf("--stereo-tab/--stereo-sweep: no Stereo Tool card in this editor\n");
			return 1;
		}
		if (stereoTab >= 0) {
			card->show(static_cast<mod::stereo::Output>(std::clamp(stereoTab, 0, 1)));
			std::printf("stereo:   tab %d shown\n", static_cast<int>(card->shown()));
		}
		if (stereoSweep) {
			const auto amount = static_cast<size_t>(mod::stereo::Param::Amount);
			const auto in = static_cast<size_t>(mod::stereo::Param::In);
#pragma push_macro("ControlMask")
#undef ControlMask
			feathers::device::ControlMask earMask {};
#pragma pop_macro("ControlMask")
			earMask.set(in);
			std::array<float, kMaxDeviceFields> right {};
			const auto started = std::chrono::steady_clock::now();
			for (int frame = 0; frame < 400; ++frame) {
				const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
				const double w = 2.0 * std::numbers::pi * 0.3 * t;
				FxChain lane = bridge.fxChain();
				ModRack strip = bridge.modRack();
				DeviceRow& row = rackMode ? strip.device[rackSlot] : lane.device[slot].row;
				row.field[amount] = static_cast<float>(1.0 + 0.8 * std::sin(2.0 * std::numbers::pi * 0.25 * t));
				row.field[in] = static_cast<float>(0.5 * std::sin(w));
				bridge.publishModulated(strip, lane, RackPlayheads {}, mod::TransportClock::unknown());
				right[in] = static_cast<float>(0.5 * std::cos(w));
				if (rackMode) {
					bridge.publishRackRightEar(rackSlot, earMask, right);
				} else {
					bridge.publishFxRightEar(slot, earMask, right);
				}
				pump(12);
			}
			for (const size_t back : { size_t { 0 }, size_t { 20 }, size_t { 60 }, size_t { 120 }, size_t { 200 } }) {
				const device::EarPair p = card->scopeAtForTest(back);
				std::printf("stereo:   %zu frames back: L %.3f R %.3f\n", back, static_cast<double>(p.left),
				            static_cast<double>(p.right));
			}
			const device::EarPair last = card->scopeLastForTest();
			std::printf("stereo:   swept %zu frames; newest pair L %.3f R %.3f\n", card->scopeCountForTest(),
			            static_cast<double>(last.left), static_cast<double>(last.right));
		}
	}
	// **THE SAMPLE & HOLD'S SCOPE, FROM THE DEVICE ITSELF** (backlog MW): the device runs here, offline, at the row the
	// strip holds, nine ticks a 12 ms frame (750 a second), and its own playhead is published each frame — the road the
	// engine takes. Last before the grab, for the stereo sweep's reason: a scope scrolls with time.
	// **THE ARP'S STEP LANE, FROM THE DEVICE ITSELF**: the real `Arp` runs here, offline, at
	// the row the lane holds, with C–E–G held and let go (so pass "Latch=1" to keep it playing), on a 140 bpm transport,
	// asked at every 32-sample chunk as `Synth::render` asks it — and its playhead and newest steps are published each
	// 12 ms frame on the road the engine takes (`publishFxPlayhead`), the S&H sweep's shape.
	if (patternFocus > 0) {
		ui::PatternCard* card = findInTree<ui::PatternCard>(gui.root());
		ui::PatternExpanded* window = findInTree<ui::PatternExpanded>(gui.root());
		if (card == nullptr && window == nullptr) {
			std::printf("--pattern-focus: no Pattern card or window in this editor\n");
			return 1;
		}
		const auto page = static_cast<size_t>(std::clamp(patternFocus, 1, 4) - 1);
		if (card != nullptr) {
			card->showPage(page);
		}
		if (window != nullptr) {
			window->showPage(page);
		}
		std::printf("pattern:  tab %zu focused\n", page + 1);
		pump(300);
	}
	if (!patternEdits.empty()) {
		ui::PatternExpanded* window = findInTree<ui::PatternExpanded>(gui.root());
		if (window == nullptr) {
			std::printf("--pattern-edit: no Pattern window is open (shoot PAT with its panel)\n");
			return 1;
		}
		ui::PatternLane& lane = window->lane();
		for (const int step : patternEdits) {
			visage::MouseEvent e;
			e.position = { lane.xOfStep(static_cast<size_t>(std::max(0, step))) + 2.0f, lane.height() * 0.35f };
			lane.onMouseDown().callback(e);
			lane.onMouseUp().callback(e);
		}
		std::printf("edits:    %zu step(s) clicked on the lane\n", patternEdits.size());
		pump(300);
	}
	if (patternLike) {
		ui::PatternExpanded* window = findInTree<ui::PatternExpanded>(gui.root());
		if (window == nullptr) {
			std::printf("--pattern-like: no Pattern window is open (shoot PAT with its panel)\n");
			return 1;
		}
		const bool liked = window->terrain().like();
		std::printf("like:     %s\n", liked ? "recorded for the focused pattern" : "REFUSED");
		pump(300);
	}
	if (arpPlay) {
		namespace ap = mod::arp;
		const FxChain chain = bridge.fxChain();
		if (rackMode || slot >= chain.count || !device::sameTag(chain.device[slot].row.tag.data(), ap::kTraits.tag)) {
			std::printf("--arp-play: the shot device is not an ARP in the lane\n");
			return 1;
		}
		ap::Row fields {};
		for (size_t i = 0; i < fields.size(); ++i) {
			fields[i] = chain.device[slot].row.field[i];
		}
		ap::Arp arp;
		arp.prepare(48000.0);
		arp.setFields(fields.data(), fields.size());
		std::array<device::NoteEvent, 8> scratch {};
		size_t none = 0;
		for (const int16_t key : { int16_t { 60 }, int16_t { 64 }, int16_t { 67 } }) {
			device::NoteEvent on {};
			on.key = key;
			on.velocity = 0.8;
			arp.processNotes(&on, 1, scratch.data(), scratch.size(), &none);
		}
		for (const int16_t key : { int16_t { 60 }, int16_t { 64 }, int16_t { 67 } }) {
			device::NoteEvent off {};
			off.kind = device::NoteEvent::Kind::Off;
			off.key = key;
			arp.processNotes(&off, 1, scratch.data(), scratch.size(), &none);
		}
		constexpr double kBpm = 140.0;
		uint64_t now = 0;
		std::array<device::NoteEvent, 64> out {};
		for (int frame = 0; frame < 150; ++frame) {
			const uint64_t end = now + 576; // 12 ms
			while (now < end) {
				device::TransportClock clock = device::TransportClock::fromSecondsPerBeat(60.0 / kBpm);
				clock.songPositionSeconds = static_cast<double>(now) / 48000.0;
				clock.songPositionBeats = clock.songPositionSeconds * kBpm / 60.0;
				clock.playing = true;
				uint32_t next = 1;
				(void)arp.emit(clock, now, out.data(), out.size(), &next);
				now += std::min<uint64_t>({ next, 32, end - now });
			}
			arp.tick();
			mod::Playhead where {};
			arp.playhead(&where.ordinal, &where.phase);
			(void)arp.recentEvents(where.recent.data(), where.recent.size());
			bridge.publishFxPlayhead(slot, where);
			bridge.refresh();
			pump(12);
		}
		uint32_t newest = 0;
		float phase = 0.0f;
		arp.playhead(&newest, &phase);
		std::printf("arp:      ran %.2f s at %g bpm; newest step slot %u; %zu chord keys\n",
		            static_cast<double>(now) / 48000.0, kBpm, newest, arp.chordForTest().size());
	}
	if (snhSweep) {
		ui::SampleHoldCard* card = findInTree<ui::SampleHoldCard>(gui.root());
		// THE CARD'S OWN SLOT, found by its device — so `--rack SNH` and `--rack-add SNH` (with `--rack-set`) both serve.
		size_t snhSlot = kMaxRackSlots;
		for (size_t s = 0; card != nullptr && s < kMaxRackSlots; ++s) {
			if (bridge.rackDeviceAt(s) == card->device()) {
				snhSlot = s;
			}
		}
		if (snhSlot >= kMaxRackSlots) {
			std::printf("--snh-sweep: no Sample & Hold card in the rack (add one with --rack SNH or --rack-add SNH)\n");
			return 1;
		}
		namespace snh = mod::samplehold;
		const ModRack strip = bridge.modRack();
		const DeviceRow& row = strip.device[snhSlot];
		snh::Row fields {};
		for (size_t i = 0; i < fields.size(); ++i) {
			fields[i] = row.field[i];
		}
		snh::Device dev;
		dev.prepare(48000.0);
		dev.settings = snh::settingsOf(fields, fields.size());
		device::ModTickContext ctx {};
		for (int frame = 0; frame < 300; ++frame) {
			for (int t = 0; t < 9; ++t) {
				dev.tick(ctx);
			}
			bridge.publishRackPlayhead(snhSlot, dev.sharedShape().playhead());
			bridge.refresh();
			pump(12);
		}
		std::printf("snh:      swept %zu frames; %u samples taken; newest hold %.3f\n", card->scopeCountForTest(),
		            dev.sharedShape().taken(), static_cast<double>(dev.sharedShape().held()));
	}
	if (modRunSeconds > 0.0) {
		if (!rackMode) {
			std::printf("--mod-run: with --rack only\n");
			return 1;
		}
		const device::Descriptor* desc = nullptr;
		for (const device::Descriptor& d : kDevices) {
			if (device::sameTag(d.traits.tag, tag)) {
				desc = &d;
			}
		}
		if (desc == nullptr || desc->mod == nullptr) {
			std::printf("--mod-run: \"%s\" has no modulator ops\n", tag);
			return 1;
		}
		const device::ModOps& ops = *desc->mod;
		void* held = ::operator new(ops.stateBytes, std::align_val_t { ops.stateAlign });
		ops.construct(held);
		constexpr double kRate = 48000.0;
		constexpr int kBlock = 256;
		ops.prepare(held, kRate);
		const ModRack strip = bridge.modRack();
		const DeviceRow& row = strip.device[rackSlot];
		// A STRIP ROW MAY NOT COUNT ITS FIELDS (`fieldsPresent` 0): the whole row then, as `--snh-sweep` hands it.
		const size_t present = row.fieldsPresent > 0 ? row.fieldsPresent : row.field.size();
		std::printf("mod-run:  row of %zu field(s) (fieldsPresent %zu)\n", present, row.fieldsPresent);
		const bool hears = desc->audio != nullptr && ops.readsAudioOf != nullptr
		                   && ops.readsAudioOf(row.field.data(), present);
		if (hears) {
			desc->audio->prepare(held, kRate, 4096);
		}
		const double freqs[] = { 110.0, 164.81, 220.0, 277.18 };
		double phase[4] = {};
		std::vector<float> l(kBlock);
		std::vector<float> r(kBlock);
		const size_t period = ops.blockSamples > 0 ? ops.blockSamples : 1;
		size_t accum = 0;
		bool wasGate = false;
		const auto blocks = static_cast<long>(modRunSeconds * kRate / kBlock);
		long frame = 0;
		for (long b = 0; b < blocks; ++b) {
			const double t = static_cast<double>(frame) / kRate;
			const bool gate = std::fmod(t, 0.5) < 0.4 || b + 8 >= blocks;
			ops.setFields(held, row.field.data(), present);
			if (hears) {
				for (int i = 0; i < kBlock; ++i) {
					const double at = std::fmod(static_cast<double>(frame + i) / kRate, 0.5);
					const double env = (gate ? 1.0 : 0.0) * std::min(1.0, at / 0.005) * std::exp(-at * 1.5);
					double sum = 0.0;
					for (int v = 0; v < 4; ++v) {
						phase[v] += freqs[v] / kRate;
						phase[v] -= std::floor(phase[v]);
						sum += 2.0 * phase[v] - 1.0;
					}
					l[static_cast<size_t>(i)] = r[static_cast<size_t>(i)] = static_cast<float>(0.25 * env * sum);
				}
				desc->audio->setFields(held, row.field.data(), present);
				desc->audio->processStereo(held, l.data(), r.data(), kBlock);
			}
			accum += kBlock;
			while (accum >= period) {
				accum -= period;
				device::ModTickContext ctx {};
				ctx.gate = gate;
				ctx.retrigger = gate && !wasGate;
				wasGate = gate;
				ctx.clock = device::TransportClock::fromSecondsPerBeat(0.5);
				ctx.clock.songPositionSeconds = t;
				ctx.clock.songPositionBeats = t * 2.0;
				ctx.clock.playing = true;
				ops.tick(held, ctx);
			}
			frame += kBlock;
			mod::Playhead where {};
			if (ops.playhead != nullptr) {
				ops.playhead(held, &where.ordinal, &where.phase);
			}
			if (ops.playheadLevel != nullptr) {
				where.level = ops.playheadLevel(held);
				where.haveLevel = true;
			}
			if (ops.trigFires != nullptr) {
				where.trigFires = ops.trigFires(held);
				where.haveTrigFires = true;
			}
			if (ops.recentEvents != nullptr) {
				(void)ops.recentEvents(held, where.recent.data(), where.recent.size());
			}
			bridge.publishRackPlayhead(rackSlot, where);
			// EVERY BLOCK IS SEEN BY A FRAME: a card that keeps a history (Random Trigger's timeline, S&H's scope) samples
			// the published playhead once per editor tick, so a publish no tick saw is a hit the picture lost.
			bridge.refresh();
			pump(8);
		}
		// NO SETTLING PUMP AFTER: a scope keeps a frame-count ring, and idle frames would scroll the run out of it.
		bridge.refresh();
		{
			const mod::Playhead back = bridge.rackPlayhead(rackSlot);
			std::printf("mod-run:  slot %zu (card device %u), published ordinal %u phase %.3f level %.3f (have %d)\n", rackSlot,
			            static_cast<unsigned>(bridge.rackDeviceAt(rackSlot)), back.ordinal, static_cast<double>(back.phase),
			            static_cast<double>(back.level), back.haveLevel ? 1 : 0);
		}
		std::printf("mod-run:  %.2f s of %s ticks at %zu samples%s; trig fires %u, level %.3f, output 0 %.3f\n",
		            static_cast<double>(frame) / kRate, tag, period, hears ? ", hearing the chord" : "",
		            ops.trigFires != nullptr ? ops.trigFires(held) : 0u,
		            static_cast<double>(ops.playheadLevel != nullptr ? ops.playheadLevel(held) : -1.0f),
		            static_cast<double>(ops.outputAt != nullptr ? ops.outputAt(held, 0) : 0.0f));
	}
	if (audioSeconds > 0.0) {
		if (rackMode) {
			std::printf("--audio: the fx lane only (a rack modulator takes no audio here)\n");
			return 1;
		}
		const DeviceInstanceId id = bridge.fxDeviceAt(slot);
		const device::Descriptor* desc = nullptr;
		for (const device::Descriptor& d : kDevices) {
			if (device::sameTag(d.traits.tag, tag)) {
				desc = &d;
			}
		}
		if (desc == nullptr || desc->audio == nullptr) {
			std::printf("--audio: \"%s\" has no audio ops\n", tag);
			return 1;
		}
		const device::AudioOps& audio = *desc->audio;
		// THE BODY A HARNESS OPTION ALREADY FILED (--chain-window, --rack-chains, --resonate-*, --chain-add), so the
		// devices inside it are the ones that run; else a fresh one. Kept alive past the shot: a surface may hold it.
		void* held = bridge.fxBodies().surfaceOf(id);
		const bool fresh = held == nullptr;
		if (fresh) {
			held = ::operator new(audio.stateBytes, std::align_val_t { audio.stateAlign });
			audio.construct(held);
			if (desc->container != nullptr) {
				desc->container->adopt(held, hostCatalogue());
			}
			// FILED WHERE THE PLUGIN FILES IT — a device with stored state (a container, Featherverb's IR) is found by its
			// editor through `containerSurfaceOf`, as `rack/converger.cpp` attaches it.
			if (desc->hasStoredState()) {
				bridge.fxBodies().attach(id, held);
			}
		}
		constexpr int kBlock = 256;
		constexpr double kRate = 48000.0;
		audio.prepare(held, kRate, 4096);
		// THE STORES THE PLUGIN'S LANE FILLS, one per reading road — see the DSP connector in `feathers_clap.cpp`.
		struct Hop {
			ParamBridge* bridge;
			size_t slot;
			int road; // 0 generic, 1 OWLTT, 2 automod
		};
		static Hop hop {};
		hop = Hop { &bridge, slot,
			        device::sameTag(tag, fx::owltt::kTraits.tag) ? 1 : device::sameTag(tag, fx::automod::kTraits.tag) ? 2 : 0 };
		if (audio.setReadingsSink != nullptr) {
			audio.setReadingsSink(held, device::ReadingsSink { &hop, [](void* context, const float* v, size_t n) {
				                                                  auto* h = static_cast<Hop*>(context);
				                                                  if (h->road == 1 && n >= 18) {
					                                                  const auto three = [&](size_t at) {
						                                                  return std::array<float, 3> { v[at], v[at + 1], v[at + 2] };
					                                                  };
					                                                  const std::array<float, 3> second = three(14);
					                                                  h->bridge->publishOwlttMeters(
					                                                      h->slot, three(0), three(3), three(6), v[9] >= 0.5f,
					                                                      { v[10] >= 0.5f, v[11] >= 0.5f, v[12] >= 0.5f }, v[13],
					                                                      &second, v[17]);
				                                                  }
				                                                  else if (h->road == 2 && n >= 2) {
					                                                  h->bridge->publishAutomodReadings(h->slot, v[0], v[1]);
				                                                  }
				                                                  else {
					                                                  h->bridge->publishDeviceReadings(h->slot, v, n);
				                                                  }
			                                                  } });
		}
		const FxChain chain = bridge.fxChain();
		const DeviceRow& row = chain.device[slot].row;
		const size_t present = row.fieldsPresent > 0 ? row.fieldsPresent : row.field.size();
		// A CHORD STRUCK EVERY HALF SECOND AND HELD FOR 0.4 OF IT — A2, E3, A3, C#4 — and the shot lands while it sounds:
		// sustained levels for the meters, onsets and gaps for the envelopes, the delays and the tails.
		const double freqs[] = { 110.0, 164.81, 220.0, 277.18 };
		double phase[4] = {};
		std::vector<float> l(kBlock);
		std::vector<float> r(kBlock);
		std::vector<float> sideL(kBlock);
		std::vector<float> sideR(kBlock);
		const auto blocks = static_cast<long>(audioSeconds * kRate / kBlock);
		long frame = 0;
		for (long b = 0; b < blocks; ++b) {
			const double t = static_cast<double>(frame) / kRate;
			const double inBar = std::fmod(t, 0.5);
			const bool gate = inBar < 0.4 || b + 8 >= blocks;
			for (int i = 0; i < kBlock; ++i) {
				const double at = std::fmod(static_cast<double>(frame + i) / kRate, 0.5);
				const double env = (gate ? 1.0 : 0.0) * std::min(1.0, at / 0.005) * std::exp(-at * 1.5);
				double sl = 0.0;
				double sr = 0.0;
				for (int v = 0; v < 4; ++v) {
					phase[v] += freqs[v] / kRate;
					phase[v] -= std::floor(phase[v]);
					const double saw = 2.0 * phase[v] - 1.0;
					(v % 2 == 0 ? sl : sr) += 0.7 * saw;
					(v % 2 == 0 ? sr : sl) += 0.3 * saw;
				}
				l[static_cast<size_t>(i)] = static_cast<float>(0.25 * env * sl);
				r[static_cast<size_t>(i)] = static_cast<float>(0.25 * env * sr);
			}
			audio.setFields(held, row.field.data(), present);
			if (audio.setTransportClock != nullptr) {
				device::TransportClock clock = device::TransportClock::fromSecondsPerBeat(60.0 / 120.0);
				clock.songPositionSeconds = t;
				clock.songPositionBeats = t * 2.0;
				clock.playing = true;
				audio.setTransportClock(held, clock);
			}
			if (audio.setNoteContext != nullptr) {
				device::NoteContext note {};
				note.pitchSemitones = 45.0f;
				note.gate = gate;
				audio.setNoteContext(held, note);
			}
			if (audio.beginPerVoiceBlock != nullptr) {
				(void)audio.beginPerVoiceBlock(held);
			}
			// THE SAME CHORD AT THE SIDECHAIN INPUT, so Sidechain Inject and a keyed compressor have a key too.
			sideL.assign(l.begin(), l.end());
			sideR.assign(r.begin(), r.end());
			if (audio.setSidechain != nullptr) {
				audio.setSidechain(held, sideL.data(), sideR.data(), kBlock, gate);
			}
			audio.processStereo(held, l.data(), r.data(), kBlock);
			frame += kBlock;
			if (b % 4 == 3) {
				bridge.refresh();
				pump(8);
			}
		}
		// A SHORT SETTLE, NOT A LONG ONE: the chord is still sounding when the shutter falls, so a meter's ballistics and
		// the perform circle's sparks show a signal in progress rather than one that stopped half a second ago.
		bridge.refresh();
		pump(48);
		std::printf("audio:    %.2f s through %s (%s body), %ld blocks of %d\n", static_cast<double>(frame) / kRate, tag,
		            fresh ? "a fresh" : "the filed", blocks, kBlock);
	}
	// THE CHROME HUE AND THE SETTINGS SCREEN (backlog LG), last before the grab so nothing after them repaints
	// over either: the hue through the process-global the screen's strip sets, the screen through the header's
	// own door. The editor's watch writes the hue to THIS run's scratch prefs (`isolateUserConfig`).
	if (hueArg != 0.0) {
		(void)ui::setChromeHueOffset(hueArg);
		std::printf("hue:      %+g degrees\n", ui::chromeHueOffset());
		pump(400);
	}
	if (accentHueArg != 0.0) {
		(void)ui::setAccentHueOffset(accentHueArg);
		std::printf("accent:   %+g degrees\n", ui::accentHueOffset());
		pump(400);
	}
	if (settingsArg) {
		ui::HeaderBar* header = gui.headerForTest();
		if (header == nullptr || !header->onSettings) {
			std::printf("--settings: the header has no door to the settings screen\n");
			return 1;
		}
		header->onSettings();
		pump(400);
	}
	const Window target = captureTarget(display, parent);
	const auto colourCount = [&]() -> long {
		char probe[1024];
		std::snprintf(probe, sizeof(probe), "identify -format '%%k' %s 2>/dev/null", out.c_str());
		FILE* pipe = popen(probe, "r");
		if (pipe == nullptr) {
			return 0;
		}
		char buffer[64] = {0};
		if (std::fgets(buffer, sizeof(buffer), pipe) == nullptr) {
			pclose(pipe);
			return 0;
		}
		pclose(pipe);
		return std::strtol(buffer, nullptr, 10);
	};

	long colours = 0;
	for (int attempt = 1; attempt <= 6; ++attempt) {
		char command[1024];
		std::snprintf(command, sizeof(command), "import -depth 8 -window 0x%lx %s",
		              static_cast<unsigned long>(target), out.c_str());
		if (std::system(command) != 0) {
			std::printf("capture failed — `import` exited nonzero on attempt %d\n", attempt);
			return 1;
		}
		colours = colourCount();
		if (colours > 64) {
			std::printf("shot:     %s (window 0x%lx, %ux%u, %ld colours, attempt %d)\n", out.c_str(),
			            static_cast<unsigned long>(target), width, height, colours, attempt);
			break;
		}
		std::printf("          attempt %d gave %ld colours — not a drawn frame, waiting\n", attempt,
		            colours);
		pump(1500);
	}
	if (colours <= 64) {
		// REFUSING RATHER THAN WRITING A FILE THAT LOOKS LIKE EVIDENCE. A blank PNG on disk with a
		// plausible name is worse than no PNG at all.
		std::printf("REFUSED: never got a drawn frame (%ld colours) — the file is not evidence\n",
		            colours);
		std::remove(out.c_str());
		return 1;
	}

	gui.destroy();
	XDestroyWindow(display, parent);
	XCloseDisplay(display);
	return 0;
}
