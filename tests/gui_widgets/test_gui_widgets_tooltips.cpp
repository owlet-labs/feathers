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

// HOVER TOOLTIPS ON EVERY DEVICE CONTROL — see `src/plugin/ui/tooltip.h`.
//
// 2026-09-25, starting from COMP's "SC HPF" box: every control, everywhere, gets a hover tooltip.
// Every control already declared its text; nothing
// showed it. These cases hold the showing: the text a hovered control puts up, where the box goes,
// the delay, what a press does to it, and — the case that cannot be outgrown — that every hoverable
// leaf of every card and panel in every host says something, asked of the host-BUILT tree rather than
// of a widget built bare, because a hook dropped by a later `onMouseDown() = ...` is only visible there.

#include "fixtures.h"

#include "composite/composite_types.h"
#include "fx/comp/comp_types.h"
#include "fx/sklitter/sklitter.h"
#include "fx/sklitter/perform.h"
#include "fx/sklitter/sklitter_types.h"
#include "plugin/device_registry.h"
#include "plugin/ui/delay_card.h"
#include "plugin/ui/paged_controls.h"
#include "plugin/ui/chain_column.h"
#include "plugin/ui/controls.h"
#include "plugin/ui/engine_rail.h"
#include "plugin/ui/source_grid.h"
#include "plugin/ui/expanded_views.h"
#include "plugin/ui/fader_pill.h"
#include "plugin/ui/macros_card.h"
#include "mod/macros/macros_types.h"
#include "plugin/ui/drum_panel.h"
#include "plugin/ui/sound_stack.h"
#include "plugin/ui/parallel_rack_card.h"
#include "rack/parallel_rack.h"
#include "plugin/ui/harmonic_row.h"
#include "plugin/ui/smooth_card.h"
#include "plugin/ui/owltt_graph.h"
#include "plugin/ui/sklitter_expanded.h"
#include "plugin/ui/surface_layers.h"
#include "plugin/ui/switch_chip.h"
#include "plugin/ui/text_field.h"
#include "plugin/ui/edit_history.h"
#include "plugin/ui/encoder_face.h"
#include "plugin/ui/macro_slot.h"
#include "plugin/ui/tooltip.h"
#include "plugin/ui/tooltip_prefs.h"
#include "plugin/ui/value_box.h"
#include "plugin/ui/zone_select.h"
#include "plugin/ui/zone_coord_cell.h"
#include "plugin/ui/mode_value_cell.h"
#include "plugin/ui/fx_lane.h"
#include "plugin/ui/mod_panel.h"
#include "plugin/ui/env_card.h"
#include "plugin/ui/mseg_card.h"
#include "plugin/ui/lfo_card.h"
#include "fx/busutil/busutil_types.h"
#include "fx/filter/filter_types.h"

#include <functional>
#include <cstdio>
#include <map>
#include "rack/chain_expanded.h"
#include "rack/rack_editor.h"
#include "rack/rack_axes.h"
#include "rack/rack_engine.h"

#include <cxxabi.h>

#include <memory>
#include <set>
#include <string>
#include <typeinfo>
#include <vector>

using namespace feathers;
namespace sc = feathers::fx::sklitter;
namespace cmp = feathers::composite;

namespace {

/// A CLOCK A CASE MOVES BY HAND. The layer's delay is measured against it, so "499 ms is not enough and
/// 501 is" is a statement about the delay rather than about how busy the host was.
struct HandClock {
	double now = 10'000.0;
	void attach(ui::TooltipLayer& layer) {
		layer.setClock([this] { return now; });
	}
};

/// THE TOOLTIP DETAIL FOR ONE CASE, put back after it — the setting is process-wide, and a case that
/// left it at Full would change what every later case in the binary sees.
struct DetailScope {
	ui::TooltipDetail was = ui::tooltipDetail();
	explicit DetailScope(ui::TooltipDetail detail) { (void)ui::setTooltipDetail(detail); }
	~DetailScope() { (void)ui::setTooltipDetail(was); }
	DetailScope(const DetailScope&) = delete;
	DetailScope& operator=(const DetailScope&) = delete;
};

std::string demangled(const visage::Frame& f) {
	int status = 0;
	char* name = abi::__cxa_demangle(typeid(f).name(), nullptr, nullptr, &status);
	std::string out = status == 0 && name != nullptr ? name : typeid(f).name();
	std::free(name);
	return out;
}

/// A MOUSE EVENT AT `local` IN `frame`, WITH NO BUTTON — a hover.
visage::MouseEvent hoverAt(visage::Frame* frame, visage::Point local) {
	visage::MouseEvent e;
	e.position = local;
	e.event_frame = frame;
	e.button_id = visage::kMouseButtonNone;
	e.button_state = visage::kMouseButtonNone;
	e.is_down = false;
	return e;
}

visage::MouseEvent pressAt(visage::Frame* frame, visage::Point local) {
	visage::MouseEvent e = at(local.x, local.y);
	e.event_frame = frame;
	return e;
}

/// Advance whichever kind of host this is — both carry the delay on their per-frame road.
bool updateHost(visage::Frame* host) {
	if (auto* card = dynamic_cast<ui::FxCard*>(host)) {
		return card->update();
	}
	if (auto* view = dynamic_cast<ui::ExpandedView*>(host)) {
		return view->update();
	}
	// AND THE ENGINE RAIL, which hosts its own since voice stacking step 5.
	if (auto* panel = dynamic_cast<ui::ModPanel*>(host)) {
		return panel->updateTooltips();
	}
	if (auto* engineRail = dynamic_cast<ui::EngineRail*>(host)) {
		return engineRail->updateTooltips();
	}
	return false;
}

visage::Point centreOf(const visage::Frame& f) { return { f.width() * 0.5f, f.height() * 0.5f }; }

/// `local` in `frame`, carried up to `ancestor`'s coordinates.
visage::Point inAncestor(const visage::Frame* frame, const visage::Frame* ancestor, visage::Point local) {
	for (const visage::Frame* f = frame; f != nullptr && f != ancestor; f = f->parent()) {
		local.x += f->x();
		local.y += f->y();
	}
	return local;
}

bool drawnVisible(const visage::Frame* f, const visage::Frame* root) {
	for (; f != nullptr; f = f->parent()) {
		if (!f->isVisible()) {
			return false;
		}
		if (f == root) {
			return true;
		}
	}
	return true;
}

/// EVERY FRAME A HAND CAN REACH IN `host`: for each drawn descendant, what the host's own hit test
/// returns at its centre. The HIT TEST decides, not a list of widget types, so a control that is
/// covered, a container that passes the pointer through, and a frame that ignores the mouse are all
/// treated the way the window would treat them.
std::vector<visage::Frame*> reachableLeaves(visage::Frame* host) {
	std::vector<visage::Frame*> out;
	std::set<visage::Frame*> seen;
	std::vector<visage::Frame*> stack(host->children().begin(), host->children().end());
	while (!stack.empty()) {
		visage::Frame* f = stack.back();
		stack.pop_back();
		if (f == nullptr || !drawnVisible(f, host) || f->width() <= 0.0f || f->height() <= 0.0f) {
			continue;
		}
		for (visage::Frame* c : f->children()) {
			stack.push_back(c);
		}
		// **CENTRE-ONLY, AND THAT IS A LIMIT.** One point per drawn frame: a widget whose centre is
		// covered by a sibling but whose edge is not would be reached by a hand and missed here, and so
		// would a second region inside one widget (FaderPill's power symbol — checked by its own case).
		// Parts a CARD draws itself are not frames at all; "the parts a card draws itself" samples the
		// card's surface on a grid instead.
		const visage::Point c = inAncestor(f, host, centreOf(*f));
		if (c.x < 0.0f || c.y < 0.0f || c.x >= host->width() || c.y >= host->height()) {
			continue;
		}
		visage::Frame* hit = host->frameAtPoint(c);
		if (hit == nullptr || hit == host || !seen.insert(hit).second) {
			continue;
		}
		out.push_back(hit);
	}
	return out;
}

/// **FRAMES A HAND REACHES THAT ARE NOT A DEVICE CONTROL, AND WHY.** Each is a picture or an editor of
/// something that is not one control; a class that is not here and does not answer `TooltipSource` is
/// a control that forgot its hook, and the sweep names it.
bool allowListed(const visage::Frame& f, std::string* reason) {
	const std::string name = demangled(f);
	struct Entry {
		const char* cls;
		const char* why;
	};
	static const Entry kAllowed[] = {
		// DRAWS ITS OWN TOOLTIP, and the layer must stay hidden over it (a case below) — two boxes at
		// once would be the double tooltip this entry exists to prevent.
		{ "feathers::ui::OwlttGraph", "draws its own hover tooltip" },
		{ "feathers::ui::EqGraph", "a plot over eight bands; its crosshair readout is its own hover text" },
		// PLANES AND PICTURES: each edits or shows SEVERAL controls at once, so no one control's text
		// describes the whole of it. An open design question — a plane could name its axes — and named here so
		// the choice is visible rather than an omission.
		{ "feathers::ui::AutomodPad", "a plane over two coordinates, not one control" },
		{ "feathers::ui::ShaperPad", "a plane over two coordinates, not one control" },
		{ "feathers::ui::PhaserPad", "a plane through thirty phases, not one control" },
		{ "feathers::ui::PerformPad", "a plane over several controls, not one control" },
		{ "feathers::ui::HarmScope", "a picture of the notch and its harmonic, dragged as several controls" },
		{ "feathers::ui::AutomodStrip", "a picture of the modulation, not a control" },
		{ "feathers::ui::ArpLane", "a picture of the steps the arp played, not a control" },
		{ "feathers::ui::IrView", "the impulse's own editor — markers and stretch are body operations, not controls" },
		// CHROME, NOT DEVICE CONTROLS.
		{ "feathers::ui::FeatherverbExpanded::Strip", "the IR drawer's toggle strip" },
		{ "feathers::ui::FxLaneRail", "a rail's own chrome around the cards it holds" },
		{ "feathers::ui::ItemStepper", "the preset browser, which is not a device control" },
	};
	for (const Entry& e : kAllowed) {
		if (name == e.cls) {
			*reason = e.why;
			return true;
		}
	}
	return false;
}

/// THE CONTROL A WIDGET IS BOUND TO, asked through the widget's own accessor rather than through the
/// tooltip road — so the expected text is an independent restatement, not the answer read back.
const device::Modulatable* boundControl(visage::Frame* f) {
	if (auto* k = dynamic_cast<ui::Knob*>(f)) {
		return k->control();
	}
	if (auto* b = dynamic_cast<ui::ValueBox*>(f)) {
		return &b->control();
	}
	if (auto* s = dynamic_cast<ui::SwitchChip*>(f)) {
		return &s->control();
	}
	if (auto* s = dynamic_cast<ui::SegmentedControl*>(f)) {
		return s->control();
	}
	if (auto* z = dynamic_cast<ui::ZoneCoordCell*>(f)) {
		return &z->box().control();
	}
	if (auto* m = dynamic_cast<ui::ModeValueCell*>(f)) {
		return &m->select().control();
	}
	if (auto* z = dynamic_cast<ui::ZoneSelect*>(f)) {
		return &z->control();
	}
	return nullptr;
}

std::string declaredText(const device::ParamDesc& p) {
	return p.tooltip != nullptr && p.tooltip[0] != '\0' ? p.tooltip : (p.name != nullptr ? p.name : "");
}

/// **THE TWO WIDGETS THAT SPEAK A LINE OF THEIR OWN, AND WHAT THAT LINE MUST BE** — restated here, not read
/// back. Resonate's plot drags two controls at once, so it names both; each Model pictogram names its own
/// model before saying what it does (the review's ask, 2026-09-26). Everything else speaks a declaration.
bool speaksItsOwnLine(const visage::Frame& f, const std::string& text) {
	const std::string cls = demangled(f);
	// A PREFIX AND A NON-EMPTY REST \u2014 a line that is only its prefix says nothing.
	const auto prefixed = [&](const std::string& prefix) {
		return text.size() > prefix.size() && text.rfind(prefix, 0) == 0;
	};
	// THE DELAY CARD'S FB FX CHIP (delay-love S2): not a control — it opens the window, and says so.
	if (cls == "feathers::ui::DelayCard::LaneChip") {
		return text == ui::DelayCard::kLaneChipTip;
	}
	// THE PATTERN CARD'S TABS: the focus is the card's UI state, not a device control, so the numbers say what they do.
	// THE MIDI GRIP on each Pattern tab (design §7.9): a drag source, not a control.
	if (cls == "feathers::ui::MidiGrip") {
		return text == "Drag this pattern into your DAW as a MIDI clip (a bar-aligned loop at the song's tempo)";
	}
	if (cls == "feathers::ui::LikeButton") {
		return text == "Like this pattern: remember its place and how it sounds, so the terrain shows more like it";
	}
	if (cls == "feathers::ui::PatternTerrain") {
		return text == "The pattern terrain: each place is a pattern. Drag the puck to move this pattern there; the wheel zooms";
	}
	// THE PATTERN WINDOW'S LANE names its edit gestures ("add a gesture tooltip to the pattern lane"),
	// restated here; test_gui_widgets_devices drives every gesture each line names.
	if (cls == "feathers::ui::PatternLane") {
		return text
		           == "Edit by hand: click a step to select it and set it in the step inspector beside the lane; double-click "
		              "to add a note at the pitch under the pointer, or to delete the note there; drag a note up or down for "
		              "its pitch, snapped to the scale, or its right end for its length. Edits clear when the place (the "
		              "puck, Zone, Zone Offset, Gamma) or the Mode changes; Rotate keeps them."
		       || text
		              == "Edit by hand: click a cell to select it and set it in the step inspector beside the grid; "
		                 "double-click to turn that lane's hit on that step on or off. Edits clear when the place (the puck, "
		                 "Zone, Zone Offset, Gamma) or the Mode changes; Rotate keeps them.";
	}
	// THE STEP INSPECTOR beside the window's lane ("show the settings for the active step").
	if (cls == "feathers::ui::StepInspector") {
		return text == "The selected step's settings: they edit that step alone, and clear with the hand's other edits";
	}
	if (cls == "feathers::ui::PatternTabs") {
		return text == "A pattern's tab: press its number to edit it here. E puts it in range; S plays it alone (and edits it) — "
		               "Ctrl/Cmd-S adds it to the soloed ones, S again on the only solo clears it";
	}
	if (cls == "feathers::ui::ResonateRingView") {
		return text == "Drag: Character across, Morph up" || prefixed("A head sounding ");
	}
	// THE PHASER'S RESPONSE PLOT drags the centre control across and Feedback up, so it names both.
	if (cls == "feathers::ui::PhaserPlot") {
		return text == "Drag: Center across (the offset from the note while Key Track has one), Feedback up"
		       || text
		              == "Drag: Delay across, shorter to the right (the offset from the note while Key Track has one), "
		                 "Feedback up";
	}
	// THE SPECTRAL WINDOW'S FLATTEN, on the plot: an action on the drawn curve, not a device control, so it names what it does.
	if (cls == "feathers::ui::SpectrumFlattenButton") {
		return text == "Flatten: every band of the drawn curve back to 0 dB, as one edit.";
	}
	// THE STEP SEQUENCER'S GRID (backlog MX): its steps are row state, not controls, so each band names what it does;
	// and its Randomise is an act, not a control, so the button says what the act does.
	if (cls == "feathers::ui::StepGrid") {
		return text == ui::StepGrid::kTabsTip || text == ui::StepGrid::kLockTip || text == ui::StepGrid::kGateTip
		       || text == ui::StepGrid::kBarsTip;
	}
	if (cls == "feathers::ui::ActButton") {
		const char* own = dynamic_cast<const ui::ActButton&>(f).tooltipAt({});
		return own != nullptr && text == own && !text.empty();
	}
	if (cls == "feathers::ui::ResonateModelPicker") {
		for (const char* name : feathers::fx::resonate::kModelNames) {
			if (prefixed(std::string(name) + " \u2014 ")) {
				return true;
			}
		}
	}
	return false;
}

// ---- THE FULL NAME BEHIND A SHORTENED LABEL — restated here, not read back. -----------------

std::string lowerAlnum(const std::string& s) {
	std::string out;
	for (const char c : s) {
		if (std::isalnum(static_cast<unsigned char>(c)) != 0) {
			out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		}
	}
	return out;
}

/// "Len" of "Length": fewer letters, all of them found in order, and at least one word that is not a whole word of
/// the full name (so "Room" under FEATHERVERB, a dropped qualifier, is not one).
bool shortens(const std::string& label, const std::string& full) {
	const std::string l = lowerAlnum(label);
	const std::string f = lowerAlnum(full);
	if (l.empty() || l.size() >= f.size()) {
		return false;
	}
	size_t j = 0;
	for (size_t i = 0; i < f.size() && j < l.size(); ++i) {
		j += f[i] == l[j] ? 1 : 0;
	}
	if (j != l.size()) {
		return false;
	}
	std::set<std::string> fullWords;
	std::string w;
	const auto words = [](const std::string& s) {
		std::vector<std::string> out;
		std::string cur;
		for (const char c : s + " ") {
			if (std::isalnum(static_cast<unsigned char>(c)) != 0) {
				cur += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			}
			else if (!cur.empty()) {
				out.push_back(cur);
				cur.clear();
			}
		}
		return out;
	};
	for (const std::string& fw : words(full)) {
		fullWords.insert(fw);
	}
	for (const std::string& lw : words(label)) {
		if (fullWords.count(lw) == 0) {
			return true;
		}
	}
	return false;
}

bool endsInDots(const std::string& s) { return s.empty() || (s.size() >= 3 && s.compare(s.size() - 3, 3, "...") == 0); }

/// **THE NAME A HOVER ON `leaf` MUST SAY, OR EMPTY** — from what the widget DRAWS (its fitted caption, or its
/// caption against the room it is drawn in) and the control's declared name, or a macro slot's own name.
std::string expectedFullName(const visage::Frame* leaf) {
	std::string label;
	std::string full;
	bool cut = false;
	if (const auto* k = dynamic_cast<const ui::Knob*>(leaf)) {
		label = k->label() != nullptr ? k->label() : "";
		if (const auto* slot = dynamic_cast<const ui::MacroSlot*>(leaf->parent()); slot != nullptr && slot->naming().name) {
			full = slot->naming().name();
		}
		else if (k->control() != nullptr) {
			full = k->control()->desc().name;
		}
		if (k->labelPlacement() == ui::Knob::LabelPlacement::Right) {
			const std::string whole = k->captionIsName() ? label : ui::upper(label.c_str());
			cut = ui::trackedWidth(ui::font(ui::kKnobLabelSize), whole, ui::kKnobLabelTracking)
			      > k->width() - k->faceDiameter() - ui::Knob::kGap;
		}
		else {
			cut = !label.empty() && endsInDots(k->drawnCaption());
		}
	}
	else if (const auto* s = dynamic_cast<const ui::SwitchChip*>(leaf)) {
		label = s->label();
		if (const auto* slot = dynamic_cast<const ui::MacroSlot*>(leaf->parent()); slot != nullptr && slot->naming().name) {
			full = slot->naming().name();
		}
		else {
			full = s->control().desc().name;
		}
		if (s->presentation() == ui::SwitchChip::Presentation::Pad) {
			cut = !label.empty() && endsInDots(s->drawnCaption());
		}
		else if (device::isOffOnChoice(s->control().desc())) {
			cut = ui::textWidth(label, ui::font(ui::kKnobLabelSize)) > s->width();
		}
		else {
			return {}; // it prints its value, not its label
		}
	}
	else if (const auto* enc = dynamic_cast<const ui::EncoderFace*>(leaf)) {
		// A RELATIVE MACRO'S FACE (backlog PQ): its slot's caption, its slot's name.
		const auto* slot = dynamic_cast<const ui::MacroSlot*>(leaf->parent());
		if (slot == nullptr) {
			return {};
		}
		label = slot->caption();
		full = slot->naming().name ? slot->naming().name() : label;
		cut = !label.empty() && endsInDots(ui::Knob::columnName(label.c_str(), enc->width()).text);
	}
	else if (const auto* b = dynamic_cast<const ui::ValueBox*>(leaf)) {
		label = b->label();
		full = b->control().desc().name;
		const float wide = ui::textWidth(label, ui::font(ui::kKnobLabelSize));
		cut = b->isCompact() ? wide > b->width() - ui::ValueBox::kCompactPadX * 3.0f
		                                  - ui::textWidth(b->readout(), ui::font(ui::kKnobReadoutSize))
		                     : wide > b->captionBox().width;
	}
	else if (const auto* p = dynamic_cast<const ui::FaderPill*>(leaf)) {
		label = p->caption();
		full = p->amountForTest().desc().name;
		cut = ui::textWidth(label, ui::font(ui::kKnobLabelSize)) > (p->readout().empty() ? p->width() : p->width() * 0.5f - 7.0f);
	}
	if (label.empty()) {
		return {};
	}
	if (shortens(label, full)) {
		return full;
	}
	return cut ? label : std::string {};
}

bool startsWithNoCase(const std::string& text, const std::string& prefix) {
	return text.size() >= prefix.size() && lowerAlnum(text.substr(0, prefix.size())) == lowerAlnum(prefix)
	       && std::equal(prefix.begin(), prefix.end(), text.begin(),
	                     [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); });
}

/// THE TEXT A HOVER SHOWS, given the full name and the tooltip: "Full — tip", unless the tip already starts with it.
std::string expectedWithFull(const std::string& full, const std::string& tip) {
	if (full.empty()) {
		return tip;
	}
	if (tip.empty()) {
		return full;
	}
	return startsWithNoCase(tip, full) ? tip : full + " — " + tip;
}

struct SweepTally {
	size_t sources = 0;
	size_t allowed = 0;
	size_t pressed = 0;
	/// Labels a hover found shortened or cut, and so had to name in full.
	size_t named = 0;
};

/// HOVER, WAIT, READ; then PRESS, READ — for every reachable leaf of `host`. `desc` is the device the
/// host shows, and every text shown must be one of ITS declared tooltips.
void sweepHost(visage::Frame* host, const device::Descriptor& desc, SweepTally& tally, const std::string& where) {
	CAPTURE(where);
	std::set<std::string> declared;
	for (const device::ParamDesc& p : desc.params) {
		declared.insert(declaredText(p));
	}

	std::vector<visage::Frame*> leaves = reachableLeaves(host);
	for (visage::Frame* leaf : leaves) {
		const std::string cls = demangled(*leaf);
		CAPTURE(cls);
		auto* source = dynamic_cast<ui::TooltipSource*>(leaf);
		if (source == nullptr) {
			std::string why;
			CHECK_MESSAGE(allowListed(*leaf, &why), "a reachable frame with no tooltip and no allow-list entry");
			tally.allowed += allowListed(*leaf, &why) ? 1 : 0;
			continue;
		}
		const visage::Point c = centreOf(*leaf);
		const char* answer = source->tooltipAt(c);
		const std::string full = expectedFullName(leaf);
		if (answer == nullptr && full.empty()) {
			// A SOURCE THAT HAS NOTHING TO SAY AT ITS CENTRE — a bare knob track. Nothing to show.
			continue;
		}
		visage::Frame* hostFrame = nullptr;
		ui::TooltipHost* th = ui::tooltipHostOf(*leaf, &hostFrame);
		REQUIRE(th != nullptr);
		ui::TooltipLayer& layer = th->tooltipLayer();
		HandClock clock;
		clock.attach(layer);
		layer.clear();

		leaf->processMouseEnter(hoverAt(leaf, c));
		CHECK(layer.state() == ui::TooltipLayer::State::Pending);
		clock.now += ui::TooltipLayer::kDelayMs + 1.0;
		(void)updateHost(hostFrame);
		REQUIRE(layer.shown());
		CHECK(layer.isVisible());
		// THE TEXT IS THE CONTROL'S DECLARATION — exactly, where the widget names its control; one of
		// the device's own declarations otherwise (a pill answers for two).
		// (A PANEL MAY OWN CONTROLS OF ITS OWN — Featherverb's IR drawer declares its stretch beside the
		// panel, not in the device — so the device set is asked only where the widget names no control.)
		if (const device::Modulatable* control = boundControl(leaf)) {
			// A SWITCHED-OFF BOX SAYS WHY INSTEAD (ValueBox::setDisabledReason): the declaration is for a control that acts.
			// OR, FOR A CONTROL THAT CHANGES NOTHING WHERE IT STANDS, WHY (`ui::tooltipOf`): the Follower's Poly in Control.
			const auto* vb = dynamic_cast<const ui::ValueBox*>(leaf);
			const bool reasoned = vb != nullptr && !vb->enabled() && vb->disabledReason() != nullptr;
			const char* why = control->unavailable();
			// AND A SHORTENED OR CUT LABEL'S FULL NAME AT ITS START.
			CHECK(layer.text() == expectedWithFull(full, why != nullptr       ? std::string(why)
			                                             : reasoned           ? std::string(vb->disabledReason())
			                                                                  : declaredText(control->desc())));
		}
		else {
			// THE REST AFTER A FULL-NAME PREFIX is the declaration (a pill answers for two).
			std::string rest = layer.text();
			const std::string prefix = full + " — ";
			if (!full.empty() && rest.rfind(prefix, 0) == 0) {
				rest = rest.substr(prefix.size());
			}
			CHECK((declared.count(rest) == 1 || startsWithNoCase(rest, full) || speaksItsOwnLine(*leaf, rest)));
		}
		// **THE FULL NAME, IN THE FIRST SENTENCE** — so Brief shows it too — and once.
		if (!full.empty()) {
			CAPTURE(full);
			CAPTURE(layer.text());
			const std::string brief = ui::briefTooltip(layer.text());
			CHECK(lowerAlnum(brief).find(lowerAlnum(full)) != std::string::npos);
			CHECK(layer.text().find(full + " — " + full) == std::string::npos);
			++tally.named;
		}
		CHECK_FALSE(layer.lines().empty());
		// AND INSIDE THE HOST, whatever the host's size.
		const visage::Bounds box = layer.box();
		CHECK(box.x() >= 0.0f);
		CHECK(box.y() >= 0.0f);
		CHECK(box.right() <= hostFrame->width() + 0.01f);
		CHECK(box.bottom() <= hostFrame->height() + 0.01f);
		leaf->processMouseExit(hoverAt(leaf, c));
		CHECK(layer.state() == ui::TooltipLayer::State::Idle);
		CHECK_FALSE(layer.isVisible());
		++tally.sources;
	}

	// THE PRESS PASS, re-collecting after every press: a press is a real edit, and a card that rebuilds
	// its widgets on it would leave a list collected before it pointing at freed frames.
	for (size_t i = 0;; ++i) {
		leaves = reachableLeaves(host);
		if (i >= leaves.size()) {
			break;
		}
		visage::Frame* leaf = leaves[i];
		auto* source = dynamic_cast<ui::TooltipSource*>(leaf);
		const visage::Point c = centreOf(*leaf);
		if (source == nullptr || source->tooltipAt(c) == nullptr) {
			continue;
		}
		const std::string cls = demangled(*leaf);
		CAPTURE(cls);
		visage::Frame* hostFrame = nullptr;
		ui::TooltipLayer& layer = ui::tooltipHostOf(*leaf, &hostFrame)->tooltipLayer();
		HandClock clock;
		clock.attach(layer);
		layer.clear();
		leaf->processMouseEnter(hoverAt(leaf, c));
		clock.now += ui::TooltipLayer::kDelayMs + 1.0;
		(void)updateHost(hostFrame);
		REQUIRE(layer.shown());
		// **THE PRESS HIDES IT** — which only happens if the widget's press road still carries the hook
		// `installTooltipHover` added. A later `onMouseDown() = ...` would leave this reading Shown.
		leaf->processMouseDown(pressAt(leaf, c));
		CHECK(layer.state() == ui::TooltipLayer::State::Suppressed);
		CHECK_FALSE(layer.isVisible());
		// AND A DRAG, which is the rest of what a press becomes — still hidden at every step. Small, so a
		// knob or box moves a little rather than across its range.
		for (int step = 1; step <= 3; ++step) {
			leaf->processMouseDrag(pressAt(leaf, { c.x, c.y - static_cast<float>(step) }));
			clock.now += ui::TooltipLayer::kDelayMs;
			(void)updateHost(hostFrame);
			CHECK_FALSE(layer.shown());
		}
		leaf->processMouseUp(pressAt(leaf, c));
		// STILL HIDDEN AFTER THE RELEASE, however long the hand rests: a tooltip that came back under a
		// hand that just pressed would be describing a control the player is already using.
		clock.now += 10.0 * ui::TooltipLayer::kDelayMs;
		(void)updateHost(hostFrame);
		CHECK_FALSE(layer.shown());
		leaf->processMouseExit(hoverAt(leaf, c));
		CHECK(layer.state() == ui::TooltipLayer::State::Idle);
		++tally.pressed;
	}
}

/// THE CARD FOR `device` IN THE ASSEMBLED EDITOR, wherever the editor put it.
ui::FxCard* cardInTree(visage::Frame* root, DeviceInstanceId device) {
	if (auto* card = dynamic_cast<ui::FxCard*>(root); card != nullptr && card->device() == device) {
		return card;
	}
	for (visage::Frame* child : root->children()) {
		if (ui::FxCard* found = cardInTree(child, device)) {
			return found;
		}
	}
	return nullptr;
}

/// A DEVICE, PUT WHERE A HAND WOULD PUT IT — the fx lane for anything with audio, the strip otherwise —
/// and its card as the editor built it.
struct Placed {
	DeviceInstanceId device = kNoInstance;
	ui::FxCard* card = nullptr;
};

Placed place(ModFixture& f, const device::Descriptor& desc) {
	Placed out;
	if (maySitInAudioChain(desc)) {
		const size_t slot = f.bridge.addFxDevice(desc.traits.tag);
		REQUIRE(slot < feathers::kMaxFxSlots);
		out.device = f.bridge.fxDeviceAt(slot);
		auto* lane = findInTree<ui::FxLane>(f.root);
		REQUIRE(lane != nullptr);
		lane->setCollapsed(false);
		lane->resized();
		lane->scrollCardIntoView(slot);
		lane->resized();
	}
	else {
		const size_t slot = f.bridge.addRackDevice(desc.traits.tag);
		REQUIRE(slot < kMaxRackSlots);
		out.device = f.bridge.rackDeviceAt(slot);
		// `resized()` IS WHAT REBUILDS THE STRIP'S CARDS — see the MSEG walk case in the dahdsr suite.
		f.panel->setCollapsed(false);
		f.panel->resized();
		f.gui.tickForTest();
		// THE STRIP'S CARD BY ITS SLOT: a strip card's `device()` is not the question the strip asks
		// (mod_panel.h says why), so it is found where the strip keeps it.
		out.card = dynamic_cast<ui::FxCard*>(const_cast<visage::Frame*>(f.panel->cardFrameForTest(f.bridge.rackSlotOf(out.device))));
		return out;
	}
	f.gui.tickForTest();
	out.card = cardInTree(f.root, out.device);
	return out;
}

/// The first reachable leaf of `host` bound to the control named `name`.
visage::Frame* widgetFor(visage::Frame* host, const char* name) {
	for (visage::Frame* leaf : reachableLeaves(host)) {
		const device::Modulatable* c = boundControl(leaf);
		if (c != nullptr && std::string(c->desc().name) == name) {
			return leaf;
		}
	}
	return nullptr;
}

const device::Descriptor& descriptorByTag(const char* tag) {
	for (const device::Descriptor& d : kDevices) {
		if (std::string(d.traits.tag) == tag) {
			return d;
		}
	}
	FAIL("no device with tag " << tag);
	return kDevices[0];
}

/// Hover `leaf` at `local`, rest, and hand back its host's layer.
ui::TooltipLayer& restOn(visage::Frame* leaf, visage::Point local, HandClock& clock, visage::Frame** hostOut = nullptr) {
	visage::Frame* hostFrame = nullptr;
	ui::TooltipHost* host = ui::tooltipHostOf(*leaf, &hostFrame);
	REQUIRE(host != nullptr);
	ui::TooltipLayer& layer = host->tooltipLayer();
	clock.attach(layer);
	layer.clear();
	leaf->processMouseEnter(hoverAt(leaf, local));
	clock.now += ui::TooltipLayer::kDelayMs + 1.0;
	(void)updateHost(hostFrame);
	if (hostOut != nullptr) {
		*hostOut = hostFrame;
	}
	return layer;
}

struct HeldChain {
	sc::Sklitter device;
	HeldChain() {
		REQUIRE(sc::kDescriptor.container != nullptr);
		sc::kDescriptor.container->adopt(&device, feathers::hostCatalogue());
	}
	void* surface() { return &device; }
};

} // namespace

TEST_CASE("a hovered control shows its declared tooltip after the delay, on six kinds of card") {
	// ONE NAMED CONTROL PER CARD KIND, and the text asserted as the DECLARATION's — the generic card
	// (DELAY), the three hand-written dynamics cards the request started from, the filter, and a
	// modulator (DAHDSR, in the strip).
	struct Want {
		const char* tag;
		const char* control;
	};
	const Want wants[] = {
		{ "DELAY", nullptr }, { "busu", nullptr }, { "comp", "Comp SC HPF" },
		{ "clip", nullptr },  { "flt1", nullptr }, { "DHDS", nullptr },
	};
	for (const Want& want : wants) {
		CAPTURE(std::string(want.tag));
		ModFixture f;
		const device::Descriptor& desc = descriptorByTag(want.tag);
		const Placed placed = place(f, desc);
		REQUIRE(placed.card != nullptr);
		visage::Frame* leaf = nullptr;
		if (want.control != nullptr) {
			leaf = widgetFor(placed.card, want.control);
		}
		else {
			// THE FIRST BOUND CONTROL THE CARD SEATS — any will do; the claim is that it speaks.
			for (visage::Frame* l : reachableLeaves(placed.card)) {
				if (boundControl(l) != nullptr) {
					leaf = l;
					break;
				}
			}
		}
		REQUIRE(leaf != nullptr);
		const device::ParamDesc& p = boundControl(leaf)->desc();
		CAPTURE(std::string(p.name));

		HandClock clock;
		visage::Frame* hostFrame = nullptr;
		ui::TooltipLayer& layer = placed.card->tooltipLayer();
		clock.attach(layer);
		layer.clear();
		const visage::Point c = centreOf(*leaf);
		leaf->processMouseEnter(hoverAt(leaf, c));
		(void)ui::tooltipHostOf(*leaf, &hostFrame);
		REQUIRE(hostFrame == placed.card);

		// NOT YET: 499 ms is a hand passing over, not resting.
		clock.now += ui::TooltipLayer::kDelayMs - 1.0;
		(void)placed.card->update();
		CHECK_FALSE(layer.shown());
		CHECK_FALSE(layer.isVisible());
		// NOW.
		clock.now += 2.0;
		CHECK(placed.card->update());
		REQUIRE(layer.shown());
		CHECK(layer.isVisible());
		CHECK(layer.text() == declaredText(p));
		if (want.control != nullptr) {
			// THE BOX THE REQUEST STARTED FROM, by its own words: a filter on what the detector hears.
			CHECK(layer.text().find("detector") != std::string::npos);
		}

		// THE BOX SITS ABOVE THE POINTER (or below it where there is no room), inside the card, and the
		// lines are the text — nothing dropped by the wrap.
		const visage::Point anchor = inAncestor(leaf, placed.card, c);
		CHECK(layer.anchor().x == doctest::Approx(anchor.x));
		CHECK(layer.anchor().y == doctest::Approx(anchor.y));
		const visage::Bounds box = layer.box();
		const bool above = box.bottom() <= anchor.y - ui::TooltipLayer::kGap + 0.01f;
		const bool below = box.y() >= anchor.y + ui::TooltipLayer::kGap - 0.01f;
		CHECK((above || below));
		CHECK(box.x() >= ui::TooltipLayer::kMargin - 0.01f);
		CHECK(box.right() <= placed.card->width() - ui::TooltipLayer::kMargin + 0.01f);
		CHECK(box.width() <= ui::TooltipLayer::kMaxWidth + 0.01f);
		std::string joined;
		for (const std::string& line : layer.lines()) {
			joined += (joined.empty() ? "" : " ") + line;
		}
		// (AT THE DEFAULT DETAIL — FULL — what is shown is the whole declaration; the brief cases below
		// set Brief explicitly.)
		CHECK(joined == layer.displayed());
		CHECK(layer.displayed() == layer.text());
	}
}

TEST_CASE("a long tooltip wraps with visage's own measurement at the layer's scale, and every line fits") {
	// AT 1 AND AT 1.75 — the development display. The renderer draws with the face at the canvas scale, so a wrap
	// measured at 1 would disagree with the glyphs exactly where a line nearly fits.
	// FULL DETAIL (the default, pinned anyway): the wrap is about a text too long for one line, which the
	// whole declaration is.
	const DetailScope full { ui::TooltipDetail::Full };
	for (const float scale : { 1.0f, 1.75f }) {
		CAPTURE(scale);
		ModFixture f;
		const Placed placed = place(f, descriptorByTag("comp"));
		REQUIRE(placed.card != nullptr);
		placed.card->setDpiScale(scale);
		visage::Frame* leaf = widgetFor(placed.card, "Comp SC HPF");
		REQUIRE(leaf != nullptr);
		HandClock clock;
		ui::TooltipLayer& layer = restOn(leaf, centreOf(*leaf), clock);
		REQUIRE(layer.shown());
		REQUIRE(layer.dpiScale() == doctest::Approx(scale));
		// THIS ONE IS TWO SENTENCES, which is why it was picked — it cannot fit on one line of a card.
		REQUIRE(layer.text().size() > 60);
		CHECK(layer.lines().size() >= 2);
		// AN INDEPENDENT READING OF THE SAME METRIC: the renderer's face at the layer's scale.
		const visage::Font face = ui::font(ui::kKnobReadoutSize).withDpiScale(scale);
		const float inner = std::min(ui::TooltipLayer::kMaxWidth, placed.card->width() - 2.0f * ui::TooltipLayer::kMargin)
		                    - 2.0f * ui::TooltipLayer::kPadX;
		const auto width = [&](const std::string& s) { return face.stringWidth(visage::String::convertToUtf32(s)); };
		const std::vector<std::string>& lines = layer.lines();
		for (size_t i = 0; i < lines.size(); ++i) {
			CAPTURE(lines[i]);
			// NOT TOO LATE: every line fits the wrap width and the box drawn around it.
			CHECK(width(lines[i]) <= inner + 0.01f);
			CHECK(width(lines[i]) <= layer.box().width() - 2.0f * ui::TooltipLayer::kPadX + 0.01f);
			// NOT TOO EARLY: the next line's first word would not have fitted on this one.
			if (i + 1 < lines.size()) {
				const std::string next = lines[i + 1].substr(0, lines[i + 1].find(' '));
				CHECK(width(lines[i] + " " + next) > inner - 0.01f);
			}
		}
		// AND THE BOX IS AS TALL AS ITS LINES.
		CHECK(layer.box().height()
		      == doctest::Approx(face.lineHeight() * static_cast<float>(lines.size()) + 2.0f * ui::TooltipLayer::kPadY));
	}
}

TEST_CASE("a tooltip is clamped inside its card at both edges and flips below at the top") {
	ModFixture f;
	const Placed placed = place(f, descriptorByTag("comp"));
	REQUIRE(placed.card != nullptr);
	visage::Frame* leaf = widgetFor(placed.card, "Comp SC HPF");
	REQUIRE(leaf != nullptr);
	ui::TooltipLayer& layer = placed.card->tooltipLayer();
	const float w = placed.card->width();
	const float h = placed.card->height();
	REQUIRE(w > 0.0f);

	HandClock clock;
	const auto showAt = [&](visage::Point hostPoint) {
		clock.attach(layer);
		layer.clear();
		layer.hover(hostPoint, "a tooltip long enough that its box is wider than the distance to either edge");
		clock.now += ui::TooltipLayer::kDelayMs + 1.0;
		REQUIRE(layer.advance());
		REQUIRE(layer.shown());
		return layer.box();
	};

	// THE LEFT EDGE: centred on x = 1 the box would start far outside; it starts at the margin.
	visage::Bounds box = showAt({ 1.0f, h * 0.5f });
	CHECK(box.x() == doctest::Approx(ui::TooltipLayer::kMargin));
	// THE RIGHT EDGE, the mirror.
	box = showAt({ w - 1.0f, h * 0.5f });
	CHECK(box.right() == doctest::Approx(w - ui::TooltipLayer::kMargin));
	// THE TOP: no room above, so it goes below the pointer rather than off the card.
	box = showAt({ w * 0.5f, 4.0f });
	CHECK(box.y() >= 4.0f + ui::TooltipLayer::kGap);
	// AND ABOVE when there is room — the pointer is not covered by what describes its target.
	box = showAt({ w * 0.5f, h - 4.0f });
	CHECK(box.bottom() <= h - 4.0f - ui::TooltipLayer::kGap + 0.01f);
	CHECK(box.y() >= 0.0f);
}

TEST_CASE("a press hides the tooltip, a drag keeps it hidden, and only leaving re-arms it") {
	ModFixture f;
	const Placed placed = place(f, descriptorByTag("DELAY"));
	REQUIRE(placed.card != nullptr);
	ui::Knob* knob = nullptr;
	for (visage::Frame* l : reachableLeaves(placed.card)) {
		if ((knob = dynamic_cast<ui::Knob*>(l)) != nullptr && knob->control() != nullptr) {
			break;
		}
		knob = nullptr;
	}
	REQUIRE(knob != nullptr);
	HandClock clock;
	const visage::Point c = centreOf(*knob);

	SUBCASE("a press while pending stops it from ever appearing") {
		ui::TooltipLayer& layer = placed.card->tooltipLayer();
		clock.attach(layer);
		layer.clear();
		knob->processMouseEnter(hoverAt(knob, c));
		REQUIRE(layer.state() == ui::TooltipLayer::State::Pending);
		knob->processMouseDown(pressAt(knob, c));
		clock.now += 5.0 * ui::TooltipLayer::kDelayMs;
		(void)placed.card->update();
		CHECK_FALSE(layer.shown());
		knob->processMouseUp(pressAt(knob, c));
	}
	SUBCASE("a drag after a shown tooltip hides it and holds it hidden") {
		ui::TooltipLayer& layer = restOn(knob, c, clock);
		REQUIRE(layer.shown());
		knob->processMouseDown(pressAt(knob, c));
		for (int i = 1; i <= 5; ++i) {
			knob->processMouseDrag(pressAt(knob, { c.x, c.y - 4.0f * static_cast<float>(i) }));
			clock.now += ui::TooltipLayer::kDelayMs;
			(void)placed.card->update();
			CHECK_FALSE(layer.shown());
			CHECK_FALSE(layer.isVisible());
		}
		knob->processMouseUp(pressAt(knob, c));
		// A MOVE AFTER THE RELEASE, still on the knob, does not bring it back...
		knob->processMouseMove(hoverAt(knob, c));
		clock.now += 2.0 * ui::TooltipLayer::kDelayMs;
		(void)placed.card->update();
		CHECK_FALSE(layer.shown());
		// ...leaving and coming back does.
		knob->processMouseExit(hoverAt(knob, c));
		knob->processMouseEnter(hoverAt(knob, c));
		clock.now += ui::TooltipLayer::kDelayMs + 1.0;
		CHECK(placed.card->update());
		CHECK(layer.shown());
	}
}

TEST_CASE("moving between two controls restarts the delay and the text follows the control") {
	ModFixture f;
	const Placed placed = place(f, descriptorByTag("DELAY"));
	REQUIRE(placed.card != nullptr);
	std::vector<visage::Frame*> bound;
	for (visage::Frame* l : reachableLeaves(placed.card)) {
		if (boundControl(l) != nullptr) {
			bound.push_back(l);
		}
	}
	// TWO CONTROLS WITH DIFFERENT TEXT, so a text that did not follow could not pass by coincidence.
	visage::Frame* a = nullptr;
	visage::Frame* b = nullptr;
	for (visage::Frame* x : bound) {
		for (visage::Frame* y : bound) {
			if (declaredText(boundControl(x)->desc()) != declaredText(boundControl(y)->desc())) {
				a = x;
				b = y;
			}
		}
	}
	REQUIRE(a != nullptr);
	HandClock clock;
	ui::TooltipLayer& layer = restOn(a, centreOf(*a), clock);
	REQUIRE(layer.shown());
	CHECK(layer.text() == declaredText(boundControl(a)->desc()));
	a->processMouseExit(hoverAt(a, centreOf(*a)));
	CHECK_FALSE(layer.isVisible());
	b->processMouseEnter(hoverAt(b, centreOf(*b)));
	clock.now += ui::TooltipLayer::kDelayMs - 1.0;
	(void)placed.card->update();
	CHECK_FALSE(layer.shown());
	clock.now += 2.0;
	(void)placed.card->update();
	REQUIRE(layer.shown());
	CHECK(layer.text() == declaredText(boundControl(b)->desc()));
}

TEST_CASE("OWLTT's graph keeps its own tooltip and the card's layer stays hidden over it") {
	// NO DOUBLE TOOLTIPS. The graph draws its own and is on the sweep's allow-list for that reason;
	// this is the other half: hovering it after a knob leaves the card's layer hidden however long the
	// hand rests there.
	ModFixture f;
	const Placed placed = place(f, descriptorByTag("OWLTT"));
	REQUIRE(placed.card != nullptr);
	ui::OwlttGraph* graph = findInTree<ui::OwlttGraph>(placed.card);
	REQUIRE(graph != nullptr);
	CHECK(dynamic_cast<ui::TooltipSource*>(graph) == nullptr);
	ui::Knob* knob = findInTree<ui::Knob>(placed.card);
	REQUIRE(knob != nullptr);
	HandClock clock;
	ui::TooltipLayer& layer = restOn(knob, centreOf(*knob), clock);
	REQUIRE(layer.shown());
	knob->processMouseExit(hoverAt(knob, centreOf(*knob)));
	graph->processMouseEnter(hoverAt(graph, centreOf(*graph)));
	graph->processMouseMove(hoverAt(graph, centreOf(*graph)));
	clock.now += 4.0 * ui::TooltipLayer::kDelayMs;
	(void)placed.card->update();
	CHECK_FALSE(layer.shown());
	CHECK_FALSE(layer.isVisible());
}

TEST_CASE("removing the device mid-hover leaves nothing dangling") {
	// THE LAYER HOLDS COPIES. A card torn down while its tooltip is pending or shown takes its layer with
	// it, and the next card's layer starts clean. Run under the sanitizer gate this is also the
	// lifetime case: nothing the hooks captured may be touched after the frames are gone.
	for (const bool shown : { false, true }) {
		CAPTURE(shown);
		ModFixture f;
		const Placed placed = place(f, descriptorByTag("comp"));
		REQUIRE(placed.card != nullptr);
		visage::Frame* leaf = widgetFor(placed.card, "Comp SC HPF");
		REQUIRE(leaf != nullptr);
		HandClock clock;
		ui::TooltipLayer& layer = placed.card->tooltipLayer();
		clock.attach(layer);
		layer.clear();
		leaf->processMouseEnter(hoverAt(leaf, centreOf(*leaf)));
		if (shown) {
			clock.now += ui::TooltipLayer::kDelayMs + 1.0;
			REQUIRE(placed.card->update());
			REQUIRE(layer.shown());
		}
		const size_t slot = f.bridge.fxSlotOf(placed.device);
		REQUIRE(f.bridge.removeFxDevice(slot));
		// THE LANE RETIRES THE CARD ON ITS LAYOUT AND FREES IT AT THE NEXT SAFE POINT — both driven here,
		// so the card, its widgets and its layer are all really destroyed before anything is read.
		auto* lane = findInTree<ui::FxLane>(f.root);
		REQUIRE(lane != nullptr);
		lane->resized();
		lane->releaseRetiredCards();
		CHECK(lane->retiredCountForTest() == 0);
		f.gui.tickForTest();
		CHECK(cardInTree(f.root, placed.device) == nullptr);
		// A NEW DEVICE IN THE SAME PLACE STARTS WITH NOTHING TO SHOW.
		const Placed again = place(f, descriptorByTag("comp"));
		REQUIRE(again.card != nullptr);
		CHECK(again.card->tooltipLayer().state() == ui::TooltipLayer::State::Idle);
		CHECK_FALSE(again.card->tooltipLayer().isVisible());
	}
}

namespace {

/// A HOST WITH NOTHING ELSE IN IT, so a case can destroy the hovered widget outright — which no card
/// lets a case do to its own widgets.
struct ProbeHost : visage::Frame, ui::TooltipHost {
	ui::TooltipLayer layer;
	ProbeHost() {
		addChild(&layer);
		layer.watch(*this);
	}
	ui::TooltipLayer& tooltipLayer() override { return layer; }
};

struct Probe : visage::Frame, ui::TooltipSource {
	Probe() { ui::installTooltipHover(*this, *this); }
	const char* tooltipAt(visage::Point) const override { return "a probe"; }
	void offeredControls(std::vector<Offered>&) const override {}
};

} // namespace

TEST_CASE("a hovered widget that is destroyed, removed or hidden takes its tooltip with it") {
	// **visage DELIVERS NO EXIT FOR ANY OF THESE** (`window_event_handler.cpp` drops a removed hovered
	// frame silently), so without the liveness check a card that rebuilt its widgets under a resting hand
	// — automation, an undo, a keyboard edit — would go on showing a box for a control that is gone.
	SUBCASE("destroyed") {
		for (const bool shown : { false, true }) {
			CAPTURE(shown);
			ProbeHost host;
			host.setBounds(0.0f, 0.0f, 300.0f, 200.0f);
			auto probe = std::make_unique<Probe>();
			host.addChild(probe.get());
			probe->setBounds(100.0f, 100.0f, 40.0f, 20.0f);
			HandClock clock;
			clock.attach(host.layer);
			probe->processMouseEnter(hoverAt(probe.get(), { 20.0f, 10.0f }));
			if (shown) {
				clock.now += ui::TooltipLayer::kDelayMs + 1.0;
				REQUIRE(host.layer.advance());
				REQUIRE(host.layer.shown());
			}
			probe.reset();
			clock.now += 2.0 * ui::TooltipLayer::kDelayMs;
			// THE ADVANCE THAT NOTICES REPORTS A REPAINT iff there was a box to take down.
			CHECK(host.layer.advance() == shown);
			CHECK_FALSE(host.layer.shown());
			CHECK_FALSE(host.layer.isVisible());
			CHECK(host.layer.state() == ui::TooltipLayer::State::Idle);
		}
	}
	SUBCASE("removed from its card, and hidden") {
		for (const bool shown : { false, true }) {
			for (const bool remove : { true, false }) {
				CAPTURE(shown);
				CAPTURE(remove);
				ModFixture f;
				const Placed placed = place(f, descriptorByTag("comp"));
				REQUIRE(placed.card != nullptr);
				visage::Frame* leaf = widgetFor(placed.card, "Comp SC HPF");
				REQUIRE(leaf != nullptr);
				visage::Frame* parent = leaf->parent();
				REQUIRE(parent != nullptr);
				HandClock clock;
				ui::TooltipLayer& layer = placed.card->tooltipLayer();
				clock.attach(layer);
				layer.clear();
				leaf->processMouseEnter(hoverAt(leaf, centreOf(*leaf)));
				if (shown) {
					clock.now += ui::TooltipLayer::kDelayMs + 1.0;
					REQUIRE(placed.card->update());
					REQUIRE(layer.shown());
				}
				if (remove) {
					parent->removeChild(leaf);
				}
				else {
					leaf->setVisible(false);
				}
				clock.now += 2.0 * ui::TooltipLayer::kDelayMs;
				(void)placed.card->update();
				CHECK_FALSE(layer.shown());
				CHECK_FALSE(layer.isVisible());
				CHECK(layer.state() == ui::TooltipLayer::State::Idle);
				// PUT BACK, so the card tears down the tree it built.
				if (remove) {
					parent->addChild(leaf);
				}
				else {
					leaf->setVisible(true);
				}
			}
		}
	}
}

TEST_CASE("a card taken out of its host and put back, hidden, or re-scaled starts Idle") {
	// NONE OF THESE RESIZES THE CARD — a card re-added at the same size fires no resize — so the layer's
	// resize hook alone would bring the old box back with it.
	for (const int how : { 0, 1, 2 }) {
		CAPTURE(how); // 0 re-added, 1 hidden, 2 re-scaled
		ModFixture f;
		const Placed placed = place(f, descriptorByTag("comp"));
		REQUIRE(placed.card != nullptr);
		visage::Frame* leaf = widgetFor(placed.card, "Comp SC HPF");
		REQUIRE(leaf != nullptr);
		HandClock clock;
		ui::TooltipLayer& layer = restOn(leaf, centreOf(*leaf), clock);
		REQUIRE(layer.shown());
		visage::Frame* parent = placed.card->parent();
		REQUIRE(parent != nullptr);
		if (how == 0) {
			parent->removeChild(placed.card);
			parent->addChild(placed.card);
		}
		else if (how == 1) {
			placed.card->setVisible(false);
		}
		else {
			placed.card->setDpiScale(placed.card->dpiScale() == 1.75f ? 1.0f : 1.75f);
		}
		CHECK(layer.state() == ui::TooltipLayer::State::Idle);
		CHECK_FALSE(layer.isVisible());
		CHECK(layer.lines().empty());
		if (how == 1) {
			placed.card->setVisible(true);
		}
	}
}

TEST_CASE("leave and press report whether they hid a box, and a move re-places without re-measuring") {
	ModFixture f;
	const Placed placed = place(f, descriptorByTag("comp"));
	REQUIRE(placed.card != nullptr);
	visage::Frame* leaf = widgetFor(placed.card, "Comp SC HPF");
	REQUIRE(leaf != nullptr);
	HandClock clock;
	ui::TooltipLayer& layer = restOn(leaf, centreOf(*leaf), clock);
	REQUIRE(layer.shown());

	// A MOVE OVER A SHOWN BOX: the box follows, the lines are not measured again.
	const size_t measured = layer.measuresForTest();
	const float x0 = layer.box().x();
	for (int i = 1; i <= 5; ++i) {
		leaf->processMouseMove(hoverAt(leaf, { centreOf(*leaf).x - 3.0f * static_cast<float>(i), centreOf(*leaf).y }));
	}
	CHECK(layer.measuresForTest() == measured);
	CHECK(layer.shown());
	CHECK(layer.box().x() <= x0);

	// THE REPAINT REPORTS: true only when there was a box to take down.
	CHECK(layer.leave());
	CHECK_FALSE(layer.leave());
	(void)restOn(leaf, centreOf(*leaf), clock);
	REQUIRE(layer.shown());
	CHECK(layer.press());
	CHECK_FALSE(layer.press());
	CHECK_FALSE(layer.leave()); // suppressed, so nothing was shown to hide
}

TEST_CASE("a box is never wider than its host, however narrow the host") {
	// MEASURED, and it is why this case is on a 24px host rather than a card: visage's `lineBreaks`
	// splits an unbreakable word mid-word once it overflows, so on a card-sized host no line can outgrow
	// the wrap width and a 300-character word never reached the clamp (the known-bad that deleted it went
	// GREEN there). What CAN outgrow the host is the wrap width's own 20px floor, on a host narrower
	// than it plus the padding — so that is the host this case builds.
	for (const float hostW : { 24.0f, 236.0f }) {
		CAPTURE(hostW);
		ProbeHost host;
		host.setBounds(0.0f, 0.0f, hostW, 200.0f);
		HandClock clock;
		clock.attach(host.layer);
		const std::string word(300, 'W');
		host.layer.hover({ hostW * 0.5f, 150.0f }, word.c_str());
		clock.now += ui::TooltipLayer::kDelayMs + 1.0;
		REQUIRE(host.layer.advance());
		CHECK(host.layer.box().x() >= ui::TooltipLayer::kMargin - 0.01f);
		CHECK(host.layer.box().right() <= hostW - ui::TooltipLayer::kMargin + 0.01f);
	}
}

TEST_CASE("the enable square in a card's head shows the device's enable tooltip") {
	ModFixture f;
	const device::Descriptor& desc = descriptorByTag("comp");
	const Placed placed = place(f, desc);
	REQUIRE(placed.card != nullptr);
	const visage::Bounds sq = placed.card->enableBounds();
	REQUIRE(sq.width() > 0.0f);
	(void)placed.card->update(); // the card's own hooks go on at its first frame
	HandClock clock;
	ui::TooltipLayer& layer = restOn(placed.card, { sq.x() + sq.width() * 0.5f, sq.y() + sq.height() * 0.5f }, clock);
	REQUIRE(layer.shown());
	CHECK(layer.text() == declaredText(desc.params[static_cast<size_t>(desc.traits.enableControl)]));
	// AND THE CARD'S BARE BODY SAYS NOTHING.
	placed.card->processMouseMove(hoverAt(placed.card, { 1.0f, placed.card->height() - 1.0f }));
	CHECK(layer.state() == ui::TooltipLayer::State::Idle);
}

TEST_CASE("the parts a card draws itself answer with their controls' tooltips") {
	// THE SWEEP CANNOT SEE THESE: a chip or a type selector drawn by the card is not a child frame, so
	// the hit test returns the card. So the card's surface is sampled on a grid and the set of texts it
	// answers must contain each hand-drawn control's declaration — named here by the card's OWN list of
	// what it draws (the chips) or by the control the press road writes.
	struct Want {
		const char* tag;
		std::vector<size_t> controls;
	};
	std::vector<Want> wants {
		{ "flt1", { static_cast<size_t>(fx::filter::Param::Type), static_cast<size_t>(fx::filter::Param::Slope) } },
		{ "busu", { static_cast<size_t>(fx::busutil::Param::InvertL), static_cast<size_t>(fx::busutil::Param::InvertR) } },
		{ "DHDS", {} },
		{ "MSEG", {} },
		{ "LFO", {} },
	};
	for (const auto& chip : ui::EnvCard::chips()) {
		wants[2].controls.push_back(static_cast<size_t>(chip.param));
	}
	for (const auto& chip : ui::MsegCard::chips()) {
		wants[3].controls.push_back(static_cast<size_t>(chip.param));
	}
	for (const auto& chip : ui::LfoCard::chips()) {
		wants[4].controls.push_back(static_cast<size_t>(chip.param));
	}
	for (const Want& want : wants) {
		CAPTURE(std::string(want.tag));
		ModFixture f;
		const device::Descriptor& desc = descriptorByTag(want.tag);
		const Placed placed = place(f, desc);
		REQUIRE(placed.card != nullptr);
		std::map<std::string, visage::Point> said;
		for (float y = 0.5f; y < placed.card->height(); y += 2.0f) {
			for (float x = 0.5f; x < placed.card->width(); x += 2.0f) {
				if (const char* t = placed.card->tooltipAt({ x, y })) {
					said.emplace(t, visage::Point { x, y });
				}
			}
		}
		REQUIRE_FALSE(want.controls.empty());
		for (const size_t c : want.controls) {
			const std::string text = declaredText(desc.params[c]);
			CAPTURE(text);
			REQUIRE(said.count(text) == 1);
			// AND THROUGH THE EVENT ROAD at that point, not only the question: the card's own hooks
			// (installed on its first frame) carry it to the layer.
			(void)placed.card->update();
			HandClock clock;
			ui::TooltipLayer& layer = restOn(placed.card, said.at(text), clock);
			REQUIRE(layer.shown());
			CHECK(layer.text() == text);
			placed.card->processMouseExit(hoverAt(placed.card, said.at(text)));
		}
	}
}

TEST_CASE("the tooltip layer covers its card at every scale, and the box is in logical pixels") {
	// CLAUDE.md's setBounds/DPI rule: a scale change moves nothing logical, so a layer that re-set its
	// bounds only on a logical change would keep the old scale's native size. Asserted on the NATIVE
	// width, where that failure shows.
	ModFixture f;
	const Placed placed = place(f, descriptorByTag("comp"));
	REQUIRE(placed.card != nullptr);
	ui::TooltipLayer& layer = placed.card->tooltipLayer();
	for (const float s : { 1.0f, 1.75f, 1.0f }) {
		CAPTURE(s);
		// THE SCALE ALONE, with no layout after it — the card's own logical rectangle does not move, so
		// only the layer's DPI hook can put its native size right. (The card's own native rectangle is
		// its parent's to re-set; the layer's is the layer's.)
		placed.card->setDpiScale(s);
		CHECK(layer.width() == doctest::Approx(placed.card->width()));
		CHECK(layer.height() == doctest::Approx(placed.card->height()));
		CHECK(layer.nativeWidth() == static_cast<int>(std::lround(placed.card->width() * s)));
		CHECK(layer.nativeHeight() == static_cast<int>(std::lround(placed.card->height() * s)));
	}
}

TEST_CASE("every hoverable control on every card shows its tooltip, and a press hides it") {
	// **THE CASE THAT CANNOT BE OUTGROWN.** It walks `kDevices`, puts each device where a hand would —
	// its lane — lets the EDITOR build the card, and hovers and presses every frame the card's own hit
	// test reaches. A widget class that forgot `installTooltipHover`, or whose press road was
	// reassigned after it, fails here by name; a new device is enrolled by being registered.
	SweepTally tally;
	size_t devices = 0;
	// **DEVICES WITH NO CARD IN THIS BUILD, BY NAME — and today there are none.** Every registered device
	// gets a card in one lane or the other; a tag added here is a stated exemption, and a device missing
	// a card while not listed fails rather than being skipped.
	const std::set<std::string> kNoCard {};
	for (const device::Descriptor& desc : kDevices) {
		const std::string tag = desc.traits.tag;
		CAPTURE(tag);
		ModFixture f;
		const Placed placed = place(f, desc);
		if (placed.card == nullptr) {
			CHECK_MESSAGE(kNoCard.count(tag) == 1, "a registered device the editor built no card for");
			continue;
		}
		CHECK(kNoCard.count(tag) == 0);
		++devices;
		const size_t before = tally.sources;
		sweepHost(placed.card, desc, tally, "card " + tag);
		// EVERY CARD HAS SOMETHING TO HOVER — at least its controls, or the sweep saw nothing.
		CHECK(tally.sources > before);
	}
	CHECK(devices == kDeviceCount - kNoCard.size());
	MESSAGE("cards: " << devices << " devices, " << tally.sources << " hovered, " << tally.pressed << " pressed, " << tally.allowed << " allow-listed, " << tally.named << " named in full");
	CHECK(tally.sources > 100);
	CHECK(tally.pressed > 50);
}

TEST_CASE("every hoverable control on every expanded view shows its tooltip, and a press hides it") {
	rack::registerChainExpandedView();
	SweepTally tally;
	size_t views = 0;
	for (const device::Descriptor& desc : kDevices) {
		const std::string tag = desc.traits.tag;
		CAPTURE(tag);
		if (!ui::hasExpandedView(desc)) {
			continue;
		}
		ModFixture f;
		ui::WidgetRegistry registry;
		const Placed placed = place(f, desc);
		if (placed.device == kNoInstance) {
			continue;
		}
		// SKLITTER's panel binds its chains to a live device body — see the nest suite.
		HeldChain held;
		if (tag == sc::kTraits.tag) {
			f.bridge.fxBodies().attach(placed.device, held.surface());
		}
		std::unique_ptr<ui::ExpandedView> view =
		    ui::makeExpandedView(f.deviceHost, &f.bridge, registry, placed.device, desc);
		if (view == nullptr) {
			MESSAGE("no expanded view built for " << tag);
			continue;
		}
		++views;
		const visage::Point min = view->minimumSize();
		view->setBounds(0.0f, 0.0f, std::max(min.x, 900.0f), std::max(min.y, 600.0f));
		(void)view->update();
		view->resized();
		sweepHost(view.get(), desc, tally, "expanded " + tag);
	}
	CHECK(views >= 6);
	MESSAGE("panels: " << views << " views, " << tally.sources << " hovered, " << tally.pressed << " pressed, " << tally.allowed << " allow-listed, " << tally.named << " named in full");
	CHECK(tally.sources > 30);
}

namespace {

/// WHAT A SURFACE OFFERS, by the device's own control index: what has a face of its own there, and what a picture
/// writes as a side road (`ui/control_face.h`).
struct Offers {
	std::set<size_t> face;
	std::set<size_t> gesture;
};

/// The index of `control` in `desc`, or npos — asked by ADDRESS of the declaration it reads, so a control of
/// another device (SKLITTER's inner chain, a chain's inner rack) or a core parameter is not counted as this one's.
size_t indexIn(const device::Descriptor& desc, const device::Modulatable& control) {
	const device::ParamDesc* p = &control.desc();
	for (size_t i = 0; i < desc.params.size(); ++i) {
		if (&desc.params[i] == p) {
			return i;
		}
	}
	return static_cast<size_t>(-1);
}

std::string paramName(const device::Descriptor& desc, size_t i) {
	return desc.params[i].name != nullptr ? desc.params[i].name : std::to_string(i);
}

/// EVERY FRAME UNDER `root`, asked what it offers. `visibleOnly`: only frames EFFECTIVELY visible — the frame and every
/// ancestor up to `root` shown — so a box the mode has hidden is not "offered" in that state.
void collectOffers(const visage::Frame* root, const device::Descriptor& desc, Offers& into, bool visibleOnly) {
	std::vector<ui::ControlFace::Offered> list;
	std::vector<const visage::Frame*> stack { root };
	while (!stack.empty()) {
		const visage::Frame* f = stack.back();
		stack.pop_back();
		if (visibleOnly && !f->isVisible()) {
			continue; // and nothing under it
		}
		if (const auto* face = dynamic_cast<const ui::ControlFace*>(f)) {
			list.clear();
			face->offeredControls(list);
			for (const ui::ControlFace::Offered& o : list) {
				const size_t i = o.control != nullptr ? indexIn(desc, *o.control) : static_cast<size_t>(-1);
				if (i < desc.params.size()) {
					(o.road == ui::ControlFace::Road::Face ? into.face : into.gesture).insert(i);
				}
			}
		}
		for (const visage::Frame* c : f->children()) {
			stack.push_back(c);
		}
	}
}

/// WHAT `card` OFFERS THAT `window` DOES NOT, one line a control: a card face needs a window face; a card-only side
/// road may be met by either. `skip` (the enable) is not asked about.
std::string missingFrom(const Offers& card, const Offers& window, const device::Descriptor& desc, size_t skip,
                        size_t* compared) {
	std::string missing;
	for (const size_t p : card.face) {
		if (p == skip) {
			continue;
		}
		++*compared;
		if (window.face.count(p) == 0) {
			missing += "\n    " + paramName(desc, p) + (window.gesture.count(p) != 0 ? "  (only as a plot drag in the window)" : "");
		}
	}
	for (const size_t p : card.gesture) {
		if (p == skip || card.face.count(p) != 0) {
			continue;
		}
		++*compared;
		if (window.face.count(p) == 0 && window.gesture.count(p) == 0) {
			missing += "\n    " + paramName(desc, p) + "  (a drag on the card)";
		}
	}
	return missing;
}

/// THE VALUES A GATE CONTROL TAKES — every one for a Choice or a short integer range, the ends and the default
/// otherwise. A card or window that builds a widget only in some mode is asked in that mode.
std::vector<float> gateValues(const device::ParamDesc& g) {
	std::vector<float> out;
	if (g.unit == device::Unit::Choice) {
		for (size_t v = 0; v < g.names.size(); ++v) {
			out.push_back(static_cast<float>(v));
		}
	}
	else if (g.max - g.min <= 32.0) {
		for (double v = g.min; v <= g.max; v += 1.0) {
			out.push_back(static_cast<float>(v));
		}
	}
	else {
		out = { static_cast<float>(g.min), static_cast<float>(g.def), static_cast<float>(g.max) };
	}
	return out;
}

} // namespace

TEST_CASE("a device's window offers every control its card offers") {
	// **A DEVICE'S WINDOW IS STRICTLY A SUPERSET OF ITS CARD** (after the Phaser/Flanger's window
	// left out most of what its card had). Opening the window must never cost a control the card had a moment ago.
	//
	// ASKED OF THE WIDGETS, NOT OF A LIST. Each card and each window is built the way the editor builds it, every
	// frame in its tree is asked what it offers (`ui::ControlFace`, which every hoverable widget answers — it does not
	// compile otherwise), and the answer is mapped to the device's own control index by the declaration it reads. So
	// a new device is in this case by being registered, and a new card or window built from the existing widgets by
	// being built: there is no per-device list here and no exemption.
	//
	// TWO ROADS. A control with a FACE on the card (a box, a chip, a segmented control, a pill) must have a face in
	// the window: a plot's axis drag is not the same offer as Center's box. A control the card offers only as a side
	// road (a picture's drag) may be met by either.
	//
	// PER STATE, AS A HAND SEES IT. A gate hides Stages in the Flanger and shows it in the Phaser, so the comparison is
	// made in EACH state — the default patch, then every control that gates another swept through its values one at a
	// time — between what is EFFECTIVELY visible on the card (the frame and every ancestor shown) and what is
	// effectively visible in the window in that same state. A window that shows a control only in some other mode
	// fails: in this mode, opening the window would cost it. The union over all states is kept only as a diagnostic.
	//
	// WHO ANSWERS. Every hoverable widget (it does not compile otherwise), and every frame that writes a control without
	// a tooltip of its own (`ui/control_face.h` names them). EXTRA STATE IS OUTSIDE THE GUARD: a pad's folded travel and
	// the EQ's band selection are not params.
	//
	// THE ENABLE IS NOT ASKED ABOUT: the window's enable is the overlay panel header's chip (`window-enable`), which is
	// chrome around the view and outside the view tree this walks; the card's is its head's square.
	//
	// IT REPLACES "STRUCTURAL: an expanded window is a SUPERSET of its card's controls" (the devices suite), which put
	// every device in one fx lane capped at eight slots and skipped the rest — the Phaser/Flanger among them.
	rack::registerChainExpandedView();
	size_t views = 0;
	size_t compared = 0;
	size_t windowed = 0;
	size_t states = 0;
	std::vector<std::string> failures;
	std::vector<std::string> unionGaps;
	for (const device::Descriptor& desc : kDevices) {
		const std::string tag = desc.traits.tag;
		CAPTURE(tag);
		if (!ui::hasExpandedView(desc)) {
			continue;
		}
		++windowed;
		// A CONTAINER'S WINDOW EDITS ITS LIVE BODY (SKLITTER's chains, a chain's inner rack), and a fixture with no
		// engine holds none — so every container gets one, built where a rack engine builds it. Declared before the
		// fixture: the surfaces are presented over it and must go first.
		std::unique_ptr<rack::RackEngine> bodies;
		void* body = nullptr;
		if (desc.container != nullptr) {
			bodies = std::make_unique<rack::RackEngine>(rack::kRackAxes, feathers::hostCatalogue());
			bodies->prepare(48000.0, 4096);
			const size_t lane = bodies->addDevice(desc.traits.tag);
			REQUIRE(lane != rack::RackEngine::kNoSlot);
			body = bodies->slotAtLane(lane).storage.get();
			REQUIRE(body != nullptr);
		}
		ModFixture f;
		ui::WidgetRegistry registry;
		const Placed placed = place(f, desc);
		REQUIRE(placed.device != kNoInstance);
		REQUIRE(placed.card != nullptr);
		if (body != nullptr) {
			f.bridge.fxBodies().attach(placed.device, body);
		}
		std::unique_ptr<ui::ExpandedView> view =
		    ui::makeExpandedView(f.deviceHost, &f.bridge, registry, placed.device, desc);
		REQUIRE_MESSAGE(view != nullptr, "a device that declares a window and gets none");
		++views;
		const visage::Point min = view->minimumSize();
		view->setBounds(0.0f, 0.0f, std::max(min.x, 900.0f), std::max(min.y, 600.0f));

		const size_t enable = device::enableControlIn(desc.traits, desc.params);
		Offers cardUnion;
		Offers windowUnion;
		std::map<std::string, std::string> missingByState; // state -> lines
		bool cardOffersAnything = false;
		const auto compareNow = [&](const std::string& state) {
			f.gui.tickForTest();
			placed.card->resized();
			(void)placed.card->update();
			(void)view->update();
			view->resized();
			(void)view->update();
			Offers card;
			Offers window;
			collectOffers(placed.card, desc, card, true);
			collectOffers(view.get(), desc, window, true);
			collectOffers(placed.card, desc, cardUnion, false);
			collectOffers(view.get(), desc, windowUnion, false);
			++states;
			for (const size_t p : card.face) {
				cardOffersAnything = cardOffersAnything || p != enable;
			}
			const std::string missing = missingFrom(card, window, desc, enable, &compared);
			if (!missing.empty()) {
				missingByState[state] = missing;
			}
		};
		std::set<size_t> gates;
		for (const device::ParamDesc& p : desc.params) {
			if (p.gateControl >= 0 && static_cast<size_t>(p.gateControl) < desc.params.size()) {
				gates.insert(static_cast<size_t>(p.gateControl));
			}
		}
		const auto sweep = [&](const std::string& page) {
			compareNow(page + "default");
			for (const size_t g : gates) {
				const float was = f.deviceHost.fieldAt(placed.device, g);
				for (const float v : gateValues(desc.params[g])) {
					f.deviceHost.setField(placed.device, g, v);
					char value[32];
					std::snprintf(value, sizeof value, "%g", static_cast<double>(v));
					compareNow(page + paramName(desc, g) + " = " + value);
				}
				f.deviceHost.setField(placed.device, g, was);
			}
		};
		// A CARD THAT SHOWS ONE PAGE AT A TIME (`ui::PagedControls`: the Pattern's tabs) is compared PAGE BY PAGE, the
		// window turned to the same page — the page is UI state, which no gate sweep reaches.
		auto* pagedCard = dynamic_cast<ui::PagedControls*>(placed.card);
		auto* pagedView = dynamic_cast<ui::PagedControls*>(view.get());
		CHECK_MESSAGE((pagedCard == nullptr) == (pagedView == nullptr), "a card and its window page together or not at all");
		if (pagedCard != nullptr && pagedView != nullptr) {
			REQUIRE(pagedCard->pageCount() == pagedView->pageCount());
			for (size_t page = 0; page < pagedCard->pageCount(); ++page) {
				pagedCard->showPage(page);
				pagedView->showPage(page);
				sweep("page " + std::to_string(page + 1) + ", ");
			}
		}
		else {
			sweep("");
		}
		for (const auto& [state, missing] : missingByState) {
			failures.push_back(tag + " (" + desc.traits.name + "), at " + state + " — the card shows, the window does not:" + missing);
		}
		size_t ignored = 0;
		if (const std::string gap = missingFrom(cardUnion, windowUnion, desc, enable, &ignored); !gap.empty()) {
			unionGaps.push_back(tag + ": over all states, hidden included:" + gap);
		}
		// NOT VACUOUS: a CARD that answered nothing but its enable would pass silently, so each must offer more.
		CHECK_MESSAGE(cardOffersAnything, "a card the walk found no controls on but its enable");
		CHECK_MESSAGE(!windowUnion.face.empty(), "a window the walk found no controls in");
	}
	for (const std::string& gap : unionGaps) {
		MESSAGE("(diagnostic) " << gap);
	}
	for (const std::string& failure : failures) {
		MESSAGE(failure);
	}
	CHECK_MESSAGE(failures.empty(), failures.size() << " device state(s) where the window drops a control its card shows (above)");
	CHECK(views == windowed);
	CHECK(views >= 14);
	MESSAGE("windows: " << views << ", states: " << states << ", card controls compared: " << compared);
	CHECK(compared > 300);
}

namespace {

/// Every knob and every value box under `root`, with whether it is EFFECTIVELY visible (it and every
/// ancestor up to `root` shown) — a hidden one is checked as a control but not clicked.
void collectTypeIns(visage::Frame* root, std::vector<std::pair<visage::Frame*, bool>>& out, bool shown = true) {
	shown = shown && root->isVisible();
	if (dynamic_cast<ui::Knob*>(root) != nullptr || dynamic_cast<ui::ValueBox*>(root) != nullptr) {
		out.emplace_back(root, shown);
	}
	for (visage::Frame* child : root->children()) {
		collectTypeIns(child, out, shown);
	}
}

} // namespace

TEST_CASE("type-in: every knob and value box on every device opens its field on a text that reads back to its value, or refuses") {
	// **THE GUARD BACKLOG LS ASKED FOR**: for every device control, the text a triple-click
	// opens the field on parses back to the same value — or the field REFUSES to open. Never a silently wrong
	// value: a control a device prints its own way (`TextOps`) and that the unit parser cannot read back is
	// refused, and a refusal anywhere else is a failure.
	//
	// ASKED OF THE WIDGETS, as "a device's window offers every control its card offers" asks: every card and
	// every window is built the way the editor builds it, and every knob and value box found in it is asked
	// through its own control — at the ends, the default and a sweep between, as floats, because that is what
	// a device row stores. Then each one EFFECTIVELY visible is triple-clicked for real: the field opens
	// exactly when the control does not refuse, and Escape leaves the value and the undo stack as they were.
	rack::registerChainExpandedView();
	size_t widgets = 0;
	size_t values = 0;
	size_t clicked = 0;
	std::set<std::string> refusals;
	std::vector<std::string> failures;
	for (const device::Descriptor& desc : kDevices) {
		const std::string tag = desc.traits.tag;
		CAPTURE(tag);
		std::unique_ptr<rack::RackEngine> bodies;
		void* body = nullptr;
		if (desc.container != nullptr) {
			bodies = std::make_unique<rack::RackEngine>(rack::kRackAxes, feathers::hostCatalogue());
			bodies->prepare(48000.0, 4096);
			const size_t lane = bodies->addDevice(desc.traits.tag);
			REQUIRE(lane != rack::RackEngine::kNoSlot);
			body = bodies->slotAtLane(lane).storage.get();
		}
		ModFixture f;
		ui::WidgetRegistry registry;
		const Placed placed = place(f, desc);
		REQUIRE(placed.device != kNoInstance);
		REQUIRE(placed.card != nullptr);
		if (body != nullptr) {
			f.bridge.fxBodies().attach(placed.device, body);
		}
		ui::EditHistory* history = f.gui.history();
		REQUIRE(history != nullptr);
		std::unique_ptr<ui::ExpandedView> view;
		if (ui::hasExpandedView(desc)) {
			view = ui::makeExpandedView(f.deviceHost, &f.bridge, registry, placed.device, desc);
			if (view != nullptr) {
				const visage::Point min = view->minimumSize();
				view->setBounds(0.0f, 0.0f, std::max(min.x, 900.0f), std::max(min.y, 600.0f));
				// IN THE TREE, as the canvas seats it, so the field it opens can take the keyboard.
				f.root->addChild(view.get());
			}
		}
		f.gui.tickForTest();
		std::vector<std::pair<visage::Frame*, bool>> found;
		collectTypeIns(placed.card, found);
		if (view != nullptr) {
			collectTypeIns(view.get(), found);
		}
		for (const auto& [frame, shown] : found) {
			auto* knob = dynamic_cast<ui::Knob*>(frame);
			auto* box = dynamic_cast<ui::ValueBox*>(frame);
			device::Modulatable* control = knob != nullptr ? knob->control() : &box->control();
			if (control == nullptr || indexIn(desc, *control) >= desc.params.size()) {
				continue; // a core parameter, or another device's control — asked where it lives
			}
			// A BOX WHOSE PRESS OPENS A LIST (a picker, a Choice) never sees a triple-click: every choice
			// is already on its menu.
			if (box != nullptr && box->opensMenu()) {
				continue;
			}
			++widgets;
			const device::ParamDesc& p = control->desc();
			const std::string where = tag + " / " + (p.key != nullptr ? p.key : "?") + " (" + demangled(*frame) + ")";
			const double span = p.max - p.min;
			std::vector<double> samples { p.min, p.def, p.max };
			for (int k = 1; k < 16 && span > 0.0; ++k) {
				const double v = p.min + span * static_cast<double>(k) / 16.0;
				samples.push_back(p.stepped ? std::round(v) : v);
			}
			const double was = control->value();
			bool refused = false;
			for (const double sample : samples) {
				control->setValue(sample);
				const double v = control->value();
				const std::optional<std::string> opening = control->editText();
				if (!opening) {
					refused = true;
					continue;
				}
				++values;
				const std::optional<double> back = control->parseText(*opening, v);
				if (!back || static_cast<float>(*back) != static_cast<float>(v)) {
					char line[256];
					std::snprintf(line, sizeof line, " at %.9g opens on \"%s\", which reads back as %s", v,
					              opening->c_str(), back ? std::to_string(*back).c_str() : "no value");
					failures.push_back(where + line);
				}
				// AND WHAT THE CONTROL SHOWS IS IN THE SAME VOCABULARY. The opening text reads back by
				// construction when both halves are the declared unit; the trap is a control SHOWING some
				// other unit, where a hand types what it sees. So the reading on the glass must parse back to
				// a value that reads the same — the refusal's own criterion, asked independently here. Only
				// where the reading is NOT the declared unit's own print: that one is the parser's vocabulary
				// by definition, and may merely be coarse (a density of 0.3 Hz prints "0").
				const std::string shown = control->text();
				if (shown == device::formatValue(p, v)) {
					continue;
				}
				const std::optional<double> fromShown = control->parseText(shown, v);
				bool same = false;
				if (fromShown) {
					control->setValue(*fromShown);
					same = control->text() == shown;
					control->setValue(v);
				}
				if (!same) {
					failures.push_back(where + " shows \"" + shown + "\", which does not read back as itself — it should refuse");
				}
			}
			control->setValue(was);
			if (refused) {
				refusals.insert(tag + " / " + (p.key != nullptr ? p.key : "?"));
				// ONLY A CONTROL ITS DEVICE PRINTS ITS OWN WAY MAY REFUSE.
				if (desc.text == nullptr || desc.text->format == nullptr) {
					failures.push_back(where + " refuses, and its device declares no TextOps");
				}
			}

			// AND THROUGH THE WIDGET ITSELF, once, where a hand could reach it.
			const bool enabled = knob != nullptr ? knob->enabled() : box->enabled();
			if (!shown || !enabled || frame->width() <= 0.0f || frame->height() <= 0.0f) {
				continue;
			}
			const double before = control->value();
			const size_t steps = history->length();
			const bool expectOpen = control->editText().has_value();
			const visage::Point hit { frame->width() * 0.5f,
				                     knob != nullptr && knob->labelPlacement() == ui::Knob::LabelPlacement::Below
				                         ? knob->faceDiameter() * 0.5f
				                         : frame->height() * 0.5f };
			frame->processMouseDown(pressAt(frame, hit));
			frame->processMouseUp(pressAt(frame, hit));
			frame->processMouseDown(doubleClickAt(hit.x, hit.y));
			frame->processMouseUp(pressAt(frame, hit));
			frame->processMouseDown(tripleClickAt(hit.x, hit.y));
			frame->processMouseUp(pressAt(frame, hit));
			ui::TextField* field = knob != nullptr ? knob->typeInField() : box->typeInField();
			const bool opened = field != nullptr && field->editing();
			++clicked;
			if (opened != expectOpen) {
				failures.push_back(where + (expectOpen ? " did not open its field" : " opened a field it refuses"));
			}
			if (opened) {
				field->processTextInput("1");
				field->processKeyPress(visage::KeyEvent { visage::KeyCode::Escape, 0, true });
			}
			else {
				// A REFUSAL STILL RESET THE VALUE ON THE DOUBLE-CLICK — the designed gesture; put it back.
				control->beginGesture();
				control->setValue(before);
				control->endGesture();
				while (history->length() > steps && history->canUndo()) {
					history->undo();
				}
			}
			if (static_cast<float>(control->value()) != static_cast<float>(before) || (opened && history->length() != steps)) {
				failures.push_back(where + ": a triple-click then Escape changed something");
			}
		}
		if (view != nullptr) {
			f.root->removeChild(view.get());
		}
	}
	for (const std::string& r : refusals) {
		MESSAGE("refuses (prints through its device's own TextOps, which the unit parser cannot read back): " << r);
	}
	for (const std::string& failure : failures) {
		MESSAGE(failure);
	}
	CHECK_MESSAGE(failures.empty(), failures.size() << " type-in failure(s) (above)");
	MESSAGE("widgets: " << widgets << ", values read back: " << values << ", triple-clicked: " << clicked
	                    << ", refusing controls: " << refusals.size());
	// NOT VACUOUS: a walk that found a handful would pass on nothing.
	CHECK(widgets > 300);
	CHECK(clicked > 150);
	CHECK(values > 3000);
}
TEST_CASE("the chain column's cards and the rack editor's cards show tooltips too") {
	// THE OTHER TWO HOSTS: a ChainColumn (SKLITTER's post rail) and a RackEditor (a chain device's
	// panel). Built the way the nest suite builds them, which is how the editor does.
	rack::registerChainExpandedView();
	HeldChain held;
	ModFixture f;
	ui::WidgetRegistry registry;
	auto outer = std::make_unique<ui::SurfaceLayers>();
	outer->setBounds(0, 0, 1400, 900);
	const size_t slot = f.bridge.addFxDevice(sc::kTraits.tag);
	const DeviceInstanceId device = f.bridge.fxDeviceAt(slot);
	f.bridge.fxBodies().attach(device, held.surface());
	std::unique_ptr<ui::ExpandedView> view =
	    ui::makeExpandedView(f.deviceHost, &f.bridge, registry, device, sc::kDescriptor);
	REQUIRE(view != nullptr);
	auto* panel = dynamic_cast<ui::SklitterExpanded*>(view.get());
	REQUIRE(panel != nullptr);
	panel->postRailForTest().setCollapsed(false);
	REQUIRE(outer->present(std::move(view), ui::SurfaceNeed::SpansGesture, "SKLITTER", {}) != ui::kNoSurface);
	(void)outer->updateContent();
	ui::ChainColumn* column = panel->postRailForTest().columnForTest();
	REQUIRE(column != nullptr);

	SweepTally tally;
	// THE COLUMN: a COMP, the card the request started from.
	const DeviceInstanceId comp = column->addDevice(fx::comp::kTraits.tag);
	REQUIRE(comp != kNoInstance);
	(void)outer->updateContent();
	ui::FxCard* compCard = column->cardOf(comp);
	REQUIRE(compCard != nullptr);
	sweepHost(compCard, descriptorByTag("comp"), tally, "column comp");
	const size_t afterColumn = tally.sources;
	CHECK(afterColumn > 0);

	// THE RACK EDITOR: a chain device in the column, expanded, and a COMP inside it.
	const DeviceInstanceId chain = column->addDevice(cmp::kTraits.tag);
	REQUIRE(chain != kNoInstance);
	column->expandDevice(chain);
	(void)outer->updateContent();
	auto* chainPanel = findInTree<rack::ChainExpanded>(outer.get());
	REQUIRE(chainPanel != nullptr);
	(void)outer->updateContent();
	rack::RackEditor* editor = chainPanel->editorForTest();
	REQUIRE(editor != nullptr);
	const DeviceInstanceId inner = editor->addDeviceByTag(fx::comp::kTraits.tag);
	REQUIRE(inner != kNoInstance);
	editor->syncCards();
	ui::FxCard* innerCard = editor->cardOfDeviceForTest(inner);
	REQUIRE(innerCard != nullptr);
	sweepHost(innerCard, descriptorByTag("comp"), tally, "rack editor comp");
	MESSAGE("column+rack: " << tally.sources << " hovered, " << tally.pressed << " pressed");
	CHECK(tally.sources > afterColumn);
}

namespace {

/// HOVER `leaf`, THEN DRIVE THE HOST'S OWN TICK — never the layer's `advance` or the card's `update`
/// directly — past the delay, and then for a long while after. `tick` is whatever the real host runs per
/// frame. A host that re-adds its cards every tick clears the layer every tick (clear-on-re-add is
/// deliberate), so the tooltip either never appears or appears and vanishes; both fail here.
void showsAndStaysThroughHostTick(visage::Frame* leaf, const std::function<void()>& tick, const char* where) {
	CAPTURE(std::string(where));
	REQUIRE(leaf != nullptr);
	visage::Frame* hostFrame = nullptr;
	ui::TooltipHost* host = ui::tooltipHostOf(*leaf, &hostFrame);
	REQUIRE(host != nullptr);
	ui::TooltipLayer& layer = host->tooltipLayer();
	HandClock clock;
	clock.attach(layer);
	// SETTLE FIRST, so a host that does its one-time work on the first ticks has done it.
	for (int i = 0; i < 3; ++i) {
		tick();
	}
	(void)layer.clear();
	leaf->processMouseEnter(hoverAt(leaf, centreOf(*leaf)));
	REQUIRE(layer.state() == ui::TooltipLayer::State::Pending);
	// 60 Hz FOR 0.6 s OF INJECTED TIME — past the delay by the host's own road alone.
	for (int i = 0; i < 36; ++i) {
		clock.now += 1000.0 / 60.0;
		tick();
	}
	CHECK(layer.shown());
	CHECK(layer.isVisible());
	// AND IT STAYS: two more seconds of frames with the hand still resting.
	for (int i = 0; i < 120; ++i) {
		clock.now += 1000.0 / 60.0;
		tick();
	}
	CHECK(layer.shown());
	CHECK(layer.isVisible());
	CHECK(layer.state() == ui::TooltipLayer::State::Shown);
	leaf->processMouseExit(hoverAt(leaf, centreOf(*leaf)));
}

/// The first control-bound widget a card seats.
visage::Frame* firstBound(visage::Frame* card) {
	for (visage::Frame* l : reachableLeaves(card)) {
		if (boundControl(l) != nullptr) {
			return l;
		}
	}
	return nullptr;
}

} // namespace

TEST_CASE("a tooltip appears and stays through each real host's own tick") {
	SUBCASE("the Feathers fx lane, through the editor's tick") {
		ModFixture f;
		const Placed placed = place(f, descriptorByTag("comp"));
		REQUIRE(placed.card != nullptr);
		showsAndStaysThroughHostTick(widgetFor(placed.card, "Comp SC HPF"), [&] { (void)f.gui.tickForTest(); },
		                             "fx lane");
	}
	SUBCASE("the modulator strip, through the editor's tick") {
		ModFixture f;
		const Placed placed = place(f, descriptorByTag("DHDS"));
		REQUIRE(placed.card != nullptr);
		showsAndStaysThroughHostTick(firstBound(placed.card), [&] { (void)f.gui.tickForTest(); }, "mod strip");
	}
	SUBCASE("the Engine rail, through the editor's tick") {
		// THE ENGINE RAIL'S LAYER IS ADVANCED BY THE EDITOR'S TICK (voice stacking step 5) — the road a hand gets.
		ModFixture f;
		auto* engineRail = findInTree<ui::EngineRail>(f.root);
		REQUIRE(engineRail != nullptr);
		ui::Knob* fine = f.knobFor(ParamId::FineTune);
		REQUIRE(fine != nullptr);
		showsAndStaysThroughHostTick(fine, [&] { (void)f.gui.tickForTest(); }, "Engine rail");
	}
	SUBCASE("a ChainColumn and a RackEditor, through the layers' tick") {
		// SKLITTER's post rail (a ChainColumn) and a chain device's panel (a RackEditor), presented on
		// `SurfaceLayers` exactly as the nest suite builds them; `updateContent()` is the layers' per-frame
		// road, which runs `FxLaneRail::updateCards` (the column's `syncCards` + `updateCards`) and
		// `ChainExpanded::updateContent` (the editor's `syncCards` + `updateCards`).
		rack::registerChainExpandedView();
		HeldChain held;
		ModFixture f;
		ui::WidgetRegistry registry;
		auto outer = std::make_unique<ui::SurfaceLayers>();
		outer->setBounds(0, 0, 1400, 900);
		const size_t slot = f.bridge.addFxDevice(sc::kTraits.tag);
		const DeviceInstanceId device = f.bridge.fxDeviceAt(slot);
		f.bridge.fxBodies().attach(device, held.surface());
		std::unique_ptr<ui::ExpandedView> view =
		    ui::makeExpandedView(f.deviceHost, &f.bridge, registry, device, sc::kDescriptor);
		REQUIRE(view != nullptr);
		auto* panel = dynamic_cast<ui::SklitterExpanded*>(view.get());
		REQUIRE(panel != nullptr);
		panel->postRailForTest().setCollapsed(false);
		REQUIRE(outer->present(std::move(view), ui::SurfaceNeed::SpansGesture, "SKLITTER", {}) != ui::kNoSurface);
		const auto tick = [&] { (void)outer->updateContent(); };
		tick();
		ui::ChainColumn* column = panel->postRailForTest().columnForTest();
		REQUIRE(column != nullptr);
		const DeviceInstanceId comp = column->addDevice(fx::comp::kTraits.tag);
		REQUIRE(comp != kNoInstance);
		tick();
		ui::FxCard* compCard = column->cardOf(comp);
		REQUIRE(compCard != nullptr);
		showsAndStaysThroughHostTick(widgetFor(compCard, "Comp SC HPF"), tick, "chain column");

		const DeviceInstanceId chain = column->addDevice(cmp::kTraits.tag);
		REQUIRE(chain != kNoInstance);
		column->expandDevice(chain);
		tick();
		auto* chainPanel = findInTree<rack::ChainExpanded>(outer.get());
		REQUIRE(chainPanel != nullptr);
		tick();
		rack::RackEditor* editor = chainPanel->editorForTest();
		REQUIRE(editor != nullptr);
		const DeviceInstanceId inner = editor->addDeviceByTag(fx::comp::kTraits.tag);
		REQUIRE(inner != kNoInstance);
		tick();
		ui::FxCard* innerCard = editor->cardOfDeviceForTest(inner);
		REQUIRE(innerCard != nullptr);
		showsAndStaysThroughHostTick(widgetFor(innerCard, "Comp SC HPF"), tick, "rack editor");
	}
}

//------------------------------------------------------------------------------------------------//
// BRIEF TOOLTIPS — the tooltip up to its first '.', with a hook for a settings-page option.     //
// See `tooltip.h` (TooltipDetail) and `tooltip_prefs.h`.                                           //
//------------------------------------------------------------------------------------------------//

namespace {

/// The one declared tooltip under `key`, across the whole registry.
std::string declaredByKey(const char* key) {
	std::vector<std::string> found;
	for (const auto& d : feathers::kDevices) {
		for (const auto& p : d.params) {
			if (p.key != nullptr && std::string_view(p.key) == key) {
				found.push_back(declaredText(p));
			}
		}
	}
	REQUIRE(found.size() == 1);
	return found.front();
}

} // namespace

TEST_CASE("brief is the first sentence of a real declared tooltip, and never cuts inside a number") {
	struct Row {
		const char* key;
		const char* brief;
	};
	// THE EXPECTED TEXT IS WRITTEN OUT, not computed: each is the declaration read by eye and cut where a
	// reader would stop. Two carry a decimal BEFORE their first sentence ends (44.1, 66.7), one carries a
	// decimal and has no sentence end at all (0.5), one has no "." whatever.
	const Row rows[] = {
		{ "comp_sc_hpf", "A high-pass on what the detector hears, not on the audio — keeps bass from pumping the gain." },
		{ "featherverb_oversample",
		  "Runs the reverb at twice the rate: less aliasing grit up top at 44.1 and 48 kHz, for about two "
		  "and a half times the reverb's CPU." },
		{ "groove_amount",
		  "How much of the Groove Mode: the swing % (50% straight, 66.7% the triplet, 75% the MPC's heaviest, at "
		  "full), or the share of the groove template." },
		{ "shaper_hysteresis", "How much of the field's hysteresis you hear — 0.5 is the position's own, 0 removes it" },
		{ "comp_makeup", "Gain after compression, to win back the level it took" },
	};
	for (const Row& row : rows) {
		CAPTURE(row.key);
		const std::string declared = declaredByKey(row.key);
		CHECK(ui::briefTooltip(declared) == row.brief);
	}
}

TEST_CASE("brief: each rule of a sentence end, on its own") {
	struct Row {
		const char* text;
		const char* brief;
	};
	const Row rows[] = {
		// NO SENTENCE END: the whole text.
		{ "", "" },
		{ "No period at all", "No period at all" },
		{ "Version 1.2.3 is here", "Version 1.2.3 is here" },
		// A PERIOD AT THE END, and one with whitespace after it (trimmed).
		{ "Ends with one.", "Ends with one." },
		{ "Ends with one.   ", "Ends with one." },
		{ "trailing spaces   ", "trailing spaces" },
		// SEVERAL SENTENCES: the first, whatever whitespace follows it.
		{ "First. Second. Third.", "First." },
		{ "Two sentences.\nOn two lines.", "Two sentences." },
		{ "Tabbed.\tNext.", "Tabbed." },
		// DECIMALS: a point followed by a digit is not an end, at the start, inside, or at the end.
		{ "Set it to 0.5 dB. Or less.", "Set it to 0.5 dB." },
		{ "Between 1.0 and 2.5. Beyond is clipped.", "Between 1.0 and 2.5." },
		{ ".5 is half. Next.", ".5 is half." },
		// BUT A DIGIT BEFORE A REAL END IS STILL AN END.
		{ "Up to 4. Then more.", "Up to 4." },
		// ABBREVIATIONS, each of the list, any case, and after a bracket.
		{ "Rates, e.g. 1/4 or 1/8. More.", "Rates, e.g. 1/4 or 1/8." },
		{ "E.g. this one. More.", "E.g. this one." },
		{ "The tail, i.e. the late part. More.", "The tail, i.e. the late part." },
		{ "Bass, mids, etc. and more. Next.", "Bass, mids, etc. and more." },
		{ "Wet vs. dry. Next.", "Wet vs. dry." },
		{ "Close to the reference (cf. its reverb). Next.", "Close to the reference (cf. its reverb)." },
		{ "About approx. two. Next.", "About approx. two." },
		// NOT AN ABBREVIATION: a word that merely ENDS like one is a sentence end.
		{ "It is a pet. Next.", "It is a pet." },
		// AN ELLIPSIS trails off; it ends nothing.
		{ "Wait... then it fades. Next.", "Wait... then it fades." },
	};
	for (const Row& row : rows) {
		CAPTURE(row.text);
		CHECK(ui::briefTooltip(row.text) == row.brief);
	}
}

TEST_CASE("brief, over every declared tooltip: a prefix, cut only at a real sentence end") {
	size_t shortened = 0;
	size_t whole = 0;
	for (const auto& d : feathers::kDevices) {
		for (const auto& p : d.params) {
			const std::string declared = declaredText(p);
			CAPTURE(d.traits.tag);
			CAPTURE(declared);
			const std::string brief = ui::briefTooltip(declared);
			REQUIRE(declared.compare(0, brief.size(), brief) == 0);
			if (!declared.empty()) {
				CHECK_FALSE(brief.empty());
			}
			if (declared.find_first_not_of(" \t\n\r", brief.size()) == std::string::npos) {
				++whole;
				continue;
			}
			// CUT: the brief ends at a ".", and what follows it in the declaration is whitespace — never a
			// digit (a decimal point) and never more letters.
			++shortened;
			REQUIRE_FALSE(brief.empty());
			CHECK(brief.back() == '.');
			const char next = declared[brief.size()];
			CHECK((next == ' ' || next == '\n' || next == '\t' || next == '\r'));
		}
	}
	// NOT A VACUOUS SWEEP: the registry has both kinds, in numbers.
	CHECK(shortened >= 20);
	CHECK(whole >= 20);
}

TEST_CASE("the tooltip shown is brief or full by the setting, read when the box appears") {
	ModFixture f;
	// AFTER THE FIXTURE: it opens an editor, and an editor's create restores the detail from the desk
	// (full, since this desk has no line) — which is the real road, so the case sets Brief after it.
	const DetailScope brief { ui::TooltipDetail::Brief };
	const Placed placed = place(f, descriptorByTag("comp"));
	REQUIRE(placed.card != nullptr);
	visage::Frame* leaf = widgetFor(placed.card, "Comp SC HPF");
	REQUIRE(leaf != nullptr);
	const std::string full = "A high-pass on what the detector hears, not on the audio — keeps bass from pumping the "
	                         "gain. Off is a true bypass";
	const std::string first = "A high-pass on what the detector hears, not on the audio — keeps bass from pumping the gain.";
	const auto joined = [](const ui::TooltipLayer& layer) {
		std::string out;
		for (const std::string& line : layer.lines()) {
			out += (out.empty() ? "" : " ") + line;
		}
		return out;
	};

	// BRIEF: the first sentence is what is measured and drawn; the hovered text is still the whole one.
	HandClock clock;
	ui::TooltipLayer& layer = restOn(leaf, centreOf(*leaf), clock);
	REQUIRE(layer.shown());
	CHECK(layer.text() == full);
	CHECK(layer.displayed() == first);
	CHECK(joined(layer) == first);

	// CHANGED WHILE SHOWN: the box already up keeps what it was shown with...
	(void)ui::setTooltipDetail(ui::TooltipDetail::Full);
	CHECK(ui::tooltipDetail() == ui::TooltipDetail::Full);
	clock.now += 1000.0;
	(void)placed.card->update();
	CHECK(layer.displayed() == first);

	// ...AND THE NEXT HOVER SHOWS THE NEW DETAIL, with no restart of anything.
	leaf->processMouseExit(hoverAt(leaf, centreOf(*leaf)));
	CHECK_FALSE(layer.shown());
	CHECK(layer.displayed().empty());
	ui::TooltipLayer& again = restOn(leaf, centreOf(*leaf), clock);
	REQUIRE(again.shown());
	CHECK(again.displayed() == full);
	CHECK(joined(again) == full);

	// AND BACK.
	(void)ui::setTooltipDetail(ui::TooltipDetail::Brief);
	leaf->processMouseExit(hoverAt(leaf, centreOf(*leaf)));
	ui::TooltipLayer& third = restOn(leaf, centreOf(*leaf), clock);
	REQUIRE(third.shown());
	CHECK(third.displayed() == first);
}

TEST_CASE("the default tooltip detail is full, and a desk with no line, or a line it cannot read, says full") {
	// The default is verbose. THE PROCESS DEFAULT — every case that changes it puts it
	// back (DetailScope), and under `run-cases.sh --each` this case is its own process, where nothing
	// has touched it.
	CHECK(ui::tooltipDetail() == ui::TooltipDetail::Full);
	CHECK(ui::displayedTooltip("One. Two.") == "One. Two.");

	EditorPrefs desk;
	desk.load();
	desk.setText(ui::kTooltipDetailPrefKey, ""); // no line
	CHECK(desk.values().count(ui::kTooltipDetailPrefKey) == 0);
	CHECK(ui::tooltipDetailIn(desk) == ui::TooltipDetail::Full);
	desk.setText(ui::kTooltipDetailPrefKey, "terse");
	CHECK(ui::tooltipDetailIn(desk) == ui::TooltipDetail::Full);
	// AND THE ONE WORD THAT OPTS IN.
	desk.setText(ui::kTooltipDetailPrefKey, "brief");
	CHECK(ui::tooltipDetailIn(desk) == ui::TooltipDetail::Brief);
	desk.setText(ui::kTooltipDetailPrefKey, "");
}

TEST_CASE("the tooltip detail round-trips through the prefs, and every editor's prefs are told") {
	const DetailScope scope { ui::TooltipDetail::Brief };
	// TWO EDITORS' PREFS, as Feathers' editor and the rack's each hold one.
	EditorPrefs a;
	a.load();
	EditorPrefs b;
	b.load();
	{
		const std::unique_ptr<ui::TooltipDetailWatch> keepA = ui::keepTooltipDetailIn(a);
		const std::unique_ptr<ui::TooltipDetailWatch> keepB = ui::keepTooltipDetailIn(b);

		// THE SETTINGS PAGE'S HOOK: both told, both hold it, and the file says it.
		CHECK(ui::setTooltipDetail(ui::TooltipDetail::Full) == 2);
		CHECK(a.text(ui::kTooltipDetailPrefKey) == "full");
		CHECK(b.text(ui::kTooltipDetailPrefKey) == "full");
		{
			EditorPrefs fresh;
			fresh.load();
			CHECK(fresh.text(ui::kTooltipDetailPrefKey) == "full");
			CHECK(ui::tooltipDetailIn(fresh) == ui::TooltipDetail::Full);
		}
		// THE SAME VALUE AGAIN tells nobody.
		CHECK(ui::setTooltipDetail(ui::TooltipDetail::Full) == 0);

		CHECK(ui::setTooltipDetail(ui::TooltipDetail::Brief) == 2);
		EditorPrefs fresh;
		fresh.load();
		CHECK(fresh.text(ui::kTooltipDetailPrefKey) == "brief");
		CHECK(ui::tooltipDetailIn(fresh) == ui::TooltipDetail::Brief);
	}
	// GONE WITH ITS OWNER: a watch no longer alive is not told.
	CHECK(ui::setTooltipDetail(ui::TooltipDetail::Full) == 0);

	// A NEW PROCESS KNOWS ONLY THE PREFS: restore from a desk that says full.
	(void)ui::setTooltipDetail(ui::TooltipDetail::Brief);
	a.setText(ui::kTooltipDetailPrefKey, "full");
	{
		EditorPrefs desk;
		desk.load();
		ui::restoreTooltipDetail(desk);
	}
	CHECK(ui::tooltipDetail() == ui::TooltipDetail::Full);
	a.setText(ui::kTooltipDetailPrefKey, "");
}

TEST_CASE("Feathers' editor reads the tooltip detail from the desk at create and keeps it there") {
	// FROM THE DEFAULT (full) TO A DESK THAT OPTED INTO BRIEF.
	const DetailScope scope { ui::TooltipDetail::Full };
	{
		EditorPrefs desk;
		desk.load();
		desk.setText(ui::kTooltipDetailPrefKey, "brief");
	}
	{
		StatefulGuiHost host;
		ParamBridge bridge;
		feathers::BridgeDeviceHost deviceHost { bridge };
		host.bridge = &bridge;
		Gui gui(host, bridge);
		REQUIRE(ui::tooltipDetail() == ui::TooltipDetail::Full);
		REQUIRE(gui.create(CLAP_WINDOW_API_X11, false));
		CHECK(ui::tooltipDetail() == ui::TooltipDetail::Brief);
		// THE HOOK, WITH THIS EDITOR ALIVE: its prefs are told, so the desk says full.
		CHECK(ui::setTooltipDetail(ui::TooltipDetail::Full) == 1);
		EditorPrefs desk;
		desk.load();
		CHECK(desk.text(ui::kTooltipDetailPrefKey) == "full");
		gui.destroy();
	}
	EditorPrefs desk;
	desk.load();
	desk.setText(ui::kTooltipDetailPrefKey, "");
}

TEST_CASE("RESONATE's meter, plot, axes and pictograms speak through the hover road, on the card and the panel") {
	namespace rsn = feathers::fx::resonate;
	ModFixture f;
	const device::Descriptor& desc = descriptorByTag(rsn::kTraits.tag);
	const Placed placed = place(f, desc);
	auto* card = dynamic_cast<ui::ResonateCard*>(placed.card);
	REQUIRE(card != nullptr);
	(void)card->update();
	ui::WidgetRegistry registry;
	std::unique_ptr<ui::ExpandedView> view = ui::makeExpandedView(f.deviceHost, &f.bridge, registry, placed.device, desc);
	auto* panel = dynamic_cast<ui::ResonateExpanded*>(view.get());
	REQUIRE(panel != nullptr);
	const visage::Point min = panel->minimumSize();
	panel->setBounds(0.0f, 0.0f, min.x, min.y);
	(void)panel->update();
	panel->resized();

	const std::string pluck = declaredText(rsn::kParamDescs[static_cast<size_t>(rsn::Param::Pluck)]);
	const std::string morph = declaredText(rsn::kParamDescs[static_cast<size_t>(rsn::Param::Morph)]);
	const std::string character = declaredText(rsn::kParamDescs[static_cast<size_t>(rsn::Param::Character)]);
	for (const ui::ResonateFace* face : { &card->face(), &panel->face() }) {
		CAPTURE(face == &card->face() ? "card" : "panel");
		HandClock clock;
		// THE METER, AS A WIDGET OF ITS OWN: Pluck Sens's declaration, through the hover — the panel's only;
		// the simplified card has none.
		if (face == &card->face()) {
			CHECK(face->meter() == nullptr);
		}
		else {
			REQUIRE(face->meter() != nullptr);
			ui::TooltipLayer& meter = restOn(face->meter(), centreOf(*face->meter()), clock);
			REQUIRE(meter.shown());
			CHECK(meter.text() == pluck);
			face->meter()->processMouseExit(hoverAt(face->meter(), centreOf(*face->meter())));
		}
		// THE PLOT NAMES THE DRAG; EACH BAND ITS OWN CONTROL.
		ui::ResonateRingView* ring = face->ringView();
		const visage::Bounds plot = ring->plotBounds();
		ui::TooltipLayer& a = restOn(ring, { plot.x() + plot.width() * 0.5f, plot.y() + plot.height() * 0.5f }, clock);
		REQUIRE(a.shown());
		CHECK(a.text() == "Drag: Character across, Morph up");
		ring->processMouseExit(hoverAt(ring, { plot.x() + 1.0f, plot.y() + 1.0f }));
		// THE BANDS, THROUGH THE HIT TEST: what a hand over a band reaches is the box seated there — a later
		// sibling of the view — and the box speaks its own control's declaration.
		visage::Frame* top = face == &card->face() ? static_cast<visage::Frame*>(f.root) : panel;
		const visage::Point origin = ring->positionInWindow() - top->positionInWindow();
		for (const auto& [band, want] : { std::pair { ring->morphBand(), morph }, std::pair { ring->characterBand(), character } }) {
			const visage::Point at = origin + visage::Point { band.x() + band.width() * 0.5f, band.y() + band.height() * 0.5f };
			visage::Frame* hit = top->frameAtPoint(at);
			REQUIRE(hit != nullptr);
			CAPTURE(demangled(*hit));
			REQUIRE(dynamic_cast<ui::ValueBox*>(hit) != nullptr);
			const visage::Point local = at - (hit->positionInWindow() - top->positionInWindow());
			ui::TooltipLayer& t = restOn(hit, local, clock);
			REQUIRE(t.shown());
			CHECK(t.text() == want);
			hit->processMouseExit(hoverAt(hit, local));
		}
		// EACH PICTOGRAM NAMES ITS MODEL, then one line.
		ui::ResonateModelPicker* picker = face->modelPicker();
		for (size_t i = 0; i < ui::ResonateModelPicker::kCells; ++i) {
			const visage::Bounds cell = picker->cellBounds(i);
			const visage::Point mid { cell.x() + cell.width() * 0.5f, cell.y() + cell.height() * 0.5f };
			ui::TooltipLayer& p = restOn(picker, mid, clock);
			REQUIRE(p.shown());
			CAPTURE(i);
			CHECK(p.text().rfind(std::string(rsn::kModelNames[i]) + " — ", 0) == 0);
			CHECK(p.text().size() < 100);
			picker->processMouseExit(hoverAt(picker, mid));
		}
	}

	// A NOTE CHIP SAYS WHICH NOTE ITS HEAD SOUNDS — 60 is C3 by the reference's naming (`device::Unit::Note`).
	rsn::Reading r;
	r.headGains[0] = 1.0f;
	r.headNotes[0] = 60.0f;
	std::array<float, rsn::kReadingCount> flat {};
	rsn::packReading(r, flat);
	f.bridge.publishDeviceReadings(f.bridge.fxSlotOf(placed.device), flat.data(), flat.size());
	(void)card->update();
	ui::ResonateRingView* ring = card->face().ringView();
	const std::vector<ui::ResonateRingView::Chip> chips = ring->chips();
	REQUIRE(chips.size() == 1);
	HandClock clock;
	const visage::Point mid { chips[0].box.x() + chips[0].box.width() * 0.5f, chips[0].box.y() + chips[0].box.height() * 0.5f };
	ui::TooltipLayer& chip = restOn(ring, mid, clock);
	REQUIRE(chip.shown());
	CHECK(chip.text() == "A head sounding C3");
}

TEST_CASE("the Smooth Shaper's ceiling shows Drive's tooltip when the hand rests ON it, through the layer") {
	// THE SWEEP ABOVE PROBES EACH FRAME AT ITS CENTRE, and the curve's centre is picture, not the handle — so the
	// sweep cannot see whether the ceiling's hover reaches the layer at all. A first draft without the curve's
	// `installTooltipHover` passed every case; this one is where the hand actually rests.
	ModFixture f;
	const device::Descriptor& desc = descriptorByTag("smth");
	const Placed placed = place(f, desc);
	auto* card = dynamic_cast<ui::SmoothCard*>(placed.card);
	REQUIRE(card != nullptr);
	(void)card->updateContent();
	ui::SmoothCurveView* curve = card->curveForTest();
	const visage::Bounds plot = curve->plotBounds();
	const visage::Point onHandle { curve->ceilingX(), plot.y() + plot.height() * 0.5f };
	REQUIRE(curve->grabsDrive(onHandle));
	HandClock clock;
	visage::Frame* host = nullptr;
	ui::TooltipLayer& layer = restOn(curve, onHandle, clock, &host);
	REQUIRE(layer.shown());
	CHECK(layer.text() == std::string(desc.params[static_cast<size_t>(feathers::fx::smooth::Param::Drive)].tooltip));
}

TEST_CASE("every hoverable control on the Engine rail shows its tooltip, on both pages") {
	// THE ENGINE RAIL HAD NO TOOLTIP HOST (voice stacking step 5): every control on it declares its text — the stack's
	// own and Voice Count's in full, the older rows by name — and nothing showed any of it. Both pages, because the
	// Stack page's zone knobs, Curve and index picture are only reachable when it is turned to.
	ModFixture f;
	auto* engineRail = findInTree<ui::EngineRail>(f.root);
	REQUIRE(engineRail != nullptr);
	size_t shown = 0;
	for (const ui::EngineRail::Page page : { ui::EngineRail::Page::Pitch, ui::EngineRail::Page::Stack }) {
		engineRail->showPage(page);
		CAPTURE(static_cast<int>(page));
		for (visage::Frame* leaf : reachableLeaves(engineRail)) {
			const std::string cls = demangled(*leaf);
			CAPTURE(cls);
			auto* source = dynamic_cast<ui::TooltipSource*>(leaf);
			// EVERY FRAME A HAND CAN REACH ON THE RAIL SAYS WHAT IT IS — but the route list, whose rows are routes rather
			// than controls and already print their source and destination in full.
			if (cls == "feathers::ui::RouteList") {
				continue;
			}
			REQUIRE(source != nullptr);
			// THE GRID IS SEVERAL ARROWS, not one: asked at a sound's cell, never at the gap between cells its centre
			// happens to fall in. (The Stack index's arrow is a leaf of its own on the stack page, backlog MO.)
			std::vector<visage::Point> points { centreOf(*leaf) };
			if (auto* grid = dynamic_cast<ui::SourceGrid*>(leaf)) {
				const auto mid = [](visage::Bounds b) {
					return visage::Point { b.x() + b.width() * 0.5f, b.y() + b.height() * 0.5f };
				};
				points = { mid(grid->cellBounds(0)) };
			}
			for (const visage::Point c : points) {
				// AND EVERY ONE HAS SOMETHING TO SAY: the rail has no bare knob track, so a silent source here is a gap.
				REQUIRE(source->tooltipAt(c) != nullptr);
				visage::Frame* hostFrame = nullptr;
				ui::TooltipHost* th = ui::tooltipHostOf(*leaf, &hostFrame);
				REQUIRE(th != nullptr);
				CHECK(hostFrame == engineRail);
				ui::TooltipLayer& layer = th->tooltipLayer();
				HandClock clock;
				clock.attach(layer);
				layer.clear();
				leaf->processMouseEnter(hoverAt(leaf, c));
				clock.now += ui::TooltipLayer::kDelayMs + 1.0;
				(void)updateHost(hostFrame);
				REQUIRE(layer.shown());
				// THE SOURCE'S OWN TEXT — with a cut or shortened caption's full name at its start.
				CHECK(layer.text() == expectedWithFull(expectedFullName(leaf), source->tooltipAt(c)));
				const visage::Bounds box = layer.box();
				CHECK(box.x() >= 0.0f);
				CHECK(box.right() <= hostFrame->width() + 0.01f);
				CHECK(box.bottom() <= hostFrame->height() + 0.01f);
				leaf->processMouseExit(hoverAt(leaf, c));
				CHECK_FALSE(layer.isVisible());
				++shown;
			}
		}
	}
	// NOT VACUOUS: the rail's steppers, segments and knobs, the grid and the stack page's cells were all reached.
	CHECK(shown >= 15);
}

TEST_CASE("an Engine rail control shows its core row's declared tooltip, not only its name") {
	// FOUND BY A PHOTOGRAPH, NOT BY THE SWEEP ABOVE: `ParamControl` copied a core row's name and range and dropped its
	// tooltip, so every Engine rail knob and box fell back to its name — and a sweep comparing the layer to the widget's
	// own answer could not tell, because both answered the name. The reference here is the TABLE's text.
	ModFixture f;
	auto* engineRail = findInTree<ui::EngineRail>(f.root);
	REQUIRE(engineRail != nullptr);
	engineRail->showPage(ui::EngineRail::Page::Stack);
	const ParamDesc* curve = findParam(static_cast<clap_id>(ParamId::StackCurve));
	REQUIRE(curve != nullptr);
	REQUIRE(curve->tooltip != nullptr);
	ui::Knob* knob = engineRail->stackCurveForTest();
	REQUIRE(knob != nullptr);
	CHECK(std::string(knob->tooltipAt(centreOf(*knob))) == curve->tooltip);
	// AND THROUGH THE LAYER, as a hand gets it.
	ui::TooltipLayer& layer = engineRail->tooltipLayer();
	HandClock clock;
	clock.attach(layer);
	layer.clear();
	knob->processMouseEnter(hoverAt(knob, centreOf(*knob)));
	clock.now += ui::TooltipLayer::kDelayMs + 1.0;
	(void)engineRail->updateTooltips();
	REQUIRE(layer.shown());
	CHECK(layer.text() == curve->tooltip);
}

TEST_CASE("backlog MK: every audio device's card carries its output meter in the head, so it still shows when the card is folded") {
	// A tiny 1px meter in the header, so it is still visible when collapsed. SWEPT over every
	// device that sits in the fx lane (`place` puts it there), so a card added tomorrow is in it: published 6 dB over,
	// folded, and the line must be visible, inside the folded card, with a level and an over to draw.
	size_t swept = 0;
	for (const device::Descriptor& desc : kDevices) {
		if (!maySitInAudioChain(desc)) {
			continue;
		}
		const std::string tag = desc.traits.tag;
		CAPTURE(tag);
		ModFixture f;
		const Placed placed = place(f, desc);
		REQUIRE(placed.card != nullptr);
		auto* lane = findInTree<ui::FxLane>(f.root);
		REQUIRE(lane != nullptr);
		const size_t slot = f.bridge.fxSlotOf(placed.device);
		REQUIRE(slot < feathers::kMaxFxSlots);
		f.bridge.publishFxOutputPeak(slot, 2.0f); // +6 dBFS
		placed.card->setFolded(true);
		lane->resized();
		f.gui.tickForTest();
		ui::FxCard* card = lane->cardAt(slot);
		REQUIRE(card != nullptr);
		REQUIRE(card->folded());
		(void)card->update();
		const ui::CardOutputMeter& meter = card->outputMeterForTest();
		CHECK(meter.isVisible());
		CHECK(card->outputMeterPeakForTest() == 2.0f);
		// INSIDE THE FOLDED CARD, which is its head.
		CHECK(meter.x() >= 0.0f);
		CHECK(meter.y() >= 0.0f);
		CHECK(meter.x() + meter.width() <= static_cast<float>(card->width()));
		CHECK(meter.y() + meter.height() <= static_cast<float>(card->height()));
		const ui::CardMeterMarks& m = meter.marks();
		CHECK(m.level.width() * m.level.height() > 0.0f);
		CHECK(m.over.width() * m.over.height() > 0.0f);
		++swept;
	}
	CHECK(swept > 20); // not vacuous: the lane's whole catalogue
}

TEST_CASE("HT: the PERFORM rail's ANCHOR button bakes the hand's puck into the patch, centres it, dims, and one undo puts both back") {
	rack::registerChainExpandedView();
	HeldChain held;
	ModFixture f;
	ui::WidgetRegistry registry;
	const size_t slot = f.bridge.addFxDevice(sc::kTraits.tag);
	const DeviceInstanceId device = f.bridge.fxDeviceAt(slot);
	f.bridge.fxBodies().attach(device, held.surface());
	std::unique_ptr<ui::ExpandedView> view =
	    ui::makeExpandedView(f.deviceHost, &f.bridge, registry, device, sc::kDescriptor);
	REQUIRE(view != nullptr);
	auto* panel = dynamic_cast<ui::SklitterExpanded*>(view.get());
	REQUIRE(panel != nullptr);
	ui::EditHistory history;
	panel->modulationBound(nullptr, &history);
	view->setBounds(0, 0, 1400, 900);
	view->resized();
	ui::CircleRail& rail = panel->circleRailForTest();
	const auto field = [&](size_t i) { return f.deviceHost.fieldAt(device, i); };

	// AT THE CENTRE A PRESS WOULD CHANGE NOTHING, so the button is dimmed and a press does nothing.
	REQUIRE(field(static_cast<size_t>(sc::Param::PerfX)) == 0.0f);
	CHECK_FALSE(rail.anchorEnabled());

	// THE HAND PUTS THE PUCK SOMEWHERE.
	f.deviceHost.setField(device, static_cast<size_t>(sc::Param::PerfX), 0.6f);
	f.deviceHost.setField(device, static_cast<size_t>(sc::Param::PerfY), -0.35f);
	CHECK(rail.anchorEnabled());
	sc::Row row {};
	for (size_t i = 0; i < row.size(); ++i) {
		row[i] = field(i);
	}
	const sc::AnchorPress want = sc::anchorPressOf(sc::stateOf(row));

	// A PRESS WHERE THE BUTTON IS.
	const visage::Bounds b = rail.anchorBounds();
	visage::MouseEvent e;
	e.position = { b.x() + b.width() * 0.5f, b.y() + b.height() * 0.5f };
	e.button_id = visage::kMouseButtonLeft;
	e.button_state = visage::kMouseButtonLeft;
	rail.onMouseDown().callback(e);

	CHECK(field(static_cast<size_t>(sc::Param::PerfX)) == 0.0f);
	CHECK(field(static_cast<size_t>(sc::Param::PerfY)) == 0.0f);
	for (size_t i = 0; i < sc::kAnchorFloats; ++i) {
		CAPTURE(i);
		CHECK(field(sc::kAnchorFirst + i) == want.anchor[i]);
	}
	CHECK(field(sc::kAnchorFirst + sc::kAnchorFloats - 1) == 1.0f);
	CHECK_FALSE(rail.anchorEnabled());
	CHECK(history.length() == 1);

	// ONE UNDO: the puck back where the hand had it and no anchor.
	history.undo();
	CHECK(field(static_cast<size_t>(sc::Param::PerfX)) == 0.6f);
	CHECK(field(static_cast<size_t>(sc::Param::PerfY)) == -0.35f);
	for (size_t i = 0; i < sc::kAnchorFloats; ++i) {
		CHECK(field(sc::kAnchorFirst + i) == 0.0f);
	}
}

// ---- THE FULL NAME BEHIND A SHORTENED OR CUT LABEL ("hovering over a macro name, or really any knob
// name should show the full parameter name if for some reason abbreviated or truncated, or if that would normally show a
// tooltip, show it at the start of the tooltip. I hit this with long macro names in the main feathers macros.") ------

TEST_CASE("full name: a long macro name in Feathers' main macros shows whole on hover, in Brief too") {
	ModFixture f;
	ui::MacroSlot* slot = f.panel->macroSlotForTest(0);
	REQUIRE(slot != nullptr);
	REQUIRE(slot->naming().rename);
	// 24 CHARACTERS: the bridge clamps a macro name to that, so this is the longest a hand can type.
	const std::string longName = "Humanize Amount Over Tim";
	REQUIRE(longName.size() == 24);
	slot->naming().rename(longName);
	slot->refresh();
	f.panel->resized();
	ui::Knob& knob = slot->knob();
	REQUIRE(knob.isVisible());
	REQUIRE(std::string(knob.label()) == longName);
	// THE PRECONDITION, read off the glass: the column cut it.
	REQUIRE(endsInDots(knob.drawnCaption()));
	CHECK(knob.drawnCaption() != longName);

	// THE MAIN MACRO ROW IS A TOOLTIP HOST NOW (it had none: the knob's hover went nowhere).
	visage::Frame* hostFrame = nullptr;
	REQUIRE(ui::tooltipHostOf(knob, &hostFrame) != nullptr);
	CHECK(hostFrame == f.panel);
	for (const ui::TooltipDetail detail : { ui::TooltipDetail::Full, ui::TooltipDetail::Brief }) {
		const DetailScope scope(detail);
		HandClock clock;
		ui::TooltipLayer& layer = restOn(&knob, centreOf(knob), clock);
		REQUIRE(layer.shown());
		CAPTURE(layer.text());
		CHECK(layer.text().rfind(longName, 0) == 0);
		CHECK(layer.displayed().rfind(longName, 0) == 0);
		knob.processMouseExit(hoverAt(&knob, centreOf(knob)));
	}

	// AND THE UNRENAMED "M2" SAYS ITS NAME — the host's "Macro 2" — at the start.
	ui::MacroSlot* other = f.panel->macroSlotForTest(1);
	REQUIRE(other != nullptr);
	REQUIRE(std::string(other->knob().label()) == "M2");
	const std::string m2 = other->naming().name();
	CHECK(m2 == "Macro 2");
	CHECK(ui::hoverText(other->knob(), centreOf(other->knob())).rfind(m2, 0) == 0);
}

TEST_CASE("full name: an elided knob caption's tooltip starts with the full name") {
	ModFixture f;
	const Placed placed = place(f, descriptorByTag("ARP"));
	REQUIRE(placed.card != nullptr);
	auto* knob = dynamic_cast<ui::Knob*>(widgetFor(placed.card, "Gate"));
	REQUIRE(knob != nullptr);
	const char* declared = knob->control()->desc().tooltip;
	REQUIRE(declared != nullptr);
	// A LONG CAPTION IN ITS OWN COLUMN: cut with dots.
	static const char* const kLong = "Gate Length Of Each Step";
	knob->setLabel(kLong);
	REQUIRE(endsInDots(knob->drawnCaption()));
	const std::string text = ui::hoverText(*knob, centreOf(*knob));
	// NOT AN ABBREVIATION OF "Gate" — its own (longer) words — so the label itself, whole, then the tooltip.
	CHECK(text == std::string(kLong) + " — " + declared);
	CHECK(ui::briefTooltip(text).rfind(kLong, 0) == 0);
}

TEST_CASE("full name: \"Len\" shows Length, \"Oct\" shows Octaves") {
	ModFixture f;
	const Placed arp = place(f, descriptorByTag("ARP"));
	REQUIRE(arp.card != nullptr);
	auto* oct = dynamic_cast<ui::Knob*>(widgetFor(arp.card, "Octaves"));
	REQUIRE(oct != nullptr);
	REQUIRE(std::string(oct->label()) == "Oct");
	REQUIRE_FALSE(endsInDots(oct->drawnCaption())); // shown whole: the abbreviation alone is the reason
	const std::string octText = ui::hoverText(*oct, centreOf(*oct));
	CAPTURE(octText);
	CHECK(octText.rfind("Octaves", 0) == 0);
	CHECK(ui::briefTooltip(octText).find("Octaves") != std::string::npos);

	// THE PATTERN CARD IN A HORIZONTAL CHAIN captions its Length knob "Len" (`PatternCard::resized`).
	const Placed pat = place(f, descriptorByTag("PAT"));
	REQUIRE(pat.card != nullptr);
	pat.card->setLaneAxis(ui::FxCard::LaneAxis::Horizontal);
	pat.card->setBounds(0.0f, 0.0f, pat.card->laneExtent(ui::kCardUnitHeight), ui::kCardUnitHeight);
	pat.card->resized();
	ui::Knob* len = nullptr;
	for (visage::Frame* leaf : reachableLeaves(pat.card)) {
		if (auto* k = dynamic_cast<ui::Knob*>(leaf); k != nullptr && k->label() != nullptr && std::string(k->label()) == "Len") {
			len = k;
		}
	}
	REQUIRE(len != nullptr);
	const std::string lenName = len->control()->desc().name;
	CHECK(lenName.find("Length") != std::string::npos);
	const std::string lenText = ui::hoverText(*len, centreOf(*len));
	CAPTURE(lenText);
	CHECK(lenText.rfind(lenName, 0) == 0);
}

TEST_CASE("full name: a whole label with no tooltip gets none, and one with a tooltip keeps it unchanged") {
	ModFixture f;
	const Placed arp = place(f, descriptorByTag("ARP"));
	REQUIRE(arp.card != nullptr);
	auto* gate = dynamic_cast<ui::Knob*>(widgetFor(arp.card, "Gate"));
	REQUIRE(gate != nullptr);
	REQUIRE_FALSE(endsInDots(gate->drawnCaption()));
	// A WHOLE, UNABBREVIATED LABEL: the tooltip exactly as declared.
	CHECK(ui::hoverText(*gate, centreOf(*gate)) == declaredText(gate->control()->desc()));

	// NO TOOLTIP AT ALL, AND A WHOLE LABEL: still none.
	struct Bare : visage::Frame, ui::TooltipSource {
		const char* tooltipAt(visage::Point) const override { return nullptr; }
		std::optional<ui::LabelFace> labelFaceAt(visage::Point) const override {
			return ui::LabelFace { .label = "Mix", .full = "Mix", .cut = false };
		}
		void offeredControls(std::vector<Offered>&) const override {}
	} bare;
	CHECK(ui::hoverText(bare, {}).empty());
	// THE RULE'S EDGES, by value.
	CHECK(ui::fullNameToShow({ .label = "Room", .full = "Featherverb Room", .cut = false }).empty());
	CHECK(ui::fullNameToShow({ .label = "Bandwidth", .full = "Filter Macro 1", .cut = false }).empty());
	CHECK(ui::fullNameToShow({ .label = "LENGTH", .full = "Length", .cut = false }).empty());
	CHECK(ui::fullNameToShow({ .label = "Len", .full = "Length", .cut = false }) == "Length");
	CHECK(ui::fullNameToShow({ .label = "M1", .full = "Macro 1", .cut = false }) == "Macro 1");
	CHECK(ui::fullNameToShow({ .label = "Bandwidth", .full = "Filter Macro 1", .cut = true }) == "Bandwidth");
}

TEST_CASE("full name: never doubled — a tooltip that already starts with the name is left alone") {
	CHECK(ui::withFullName("Humanize Amount", "Humanize Amount: how much of the feel") == "Humanize Amount: how much of the feel");
	CHECK(ui::withFullName("Humanize Amount", "humanize amount sets the feel") == "humanize amount sets the feel");
	CHECK(ui::withFullName("Humanize Amount", "How much of the feel. More.") == "Humanize Amount — How much of the feel. More.");
	CHECK(ui::withFullName("Length", nullptr) == "Length");
	CHECK(ui::withFullName("", "As declared.") == "As declared.");
	// A hover's text, composed twice over, still names it once — the layer is handed the composition, never its own output.
	DetailScope scope(ui::TooltipDetail::Brief);
	CHECK(ui::displayedTooltip(ui::withFullName("Humanize Amount", "How much of the feel. More.")) == "Humanize Amount — How much of the feel.");
}

TEST_CASE("full name: a card head's elided name shows whole on hover") {
	ModFixture f;
	const device::Descriptor& desc = descriptorByTag("comp");
	const Placed placed = place(f, desc);
	REQUIRE(placed.card != nullptr);
	(void)placed.card->update();
	// NARROW ENOUGH THAT THE NAME IS CUT against the head's marks, and wide enough that some of it is still drawn.
	const std::string whole = placed.card->headName();
	REQUIRE_FALSE(whole.empty());
	ui::FxCard::HeadLayout head;
	for (float w = placed.card->width(); w > 20.0f; w -= 2.0f) {
		placed.card->setBounds(placed.card->x(), placed.card->y(), w, placed.card->height());
		head = placed.card->headLayout();
		if (head.name != whole) {
			break;
		}
	}
	REQUIRE(head.name != whole);
	REQUIRE(endsInDots(head.name));
	REQUIRE(head.name.size() > 3);
	const visage::Point at { head.nameBox.x() + 2.0f, head.nameBox.y() + head.nameBox.height() * 0.5f };
	CAPTURE(whole);
	CAPTURE(head.name);
	CAPTURE(ui::hoverText(*placed.card, at));
	CHECK(ui::hoverText(*placed.card, at).rfind(whole, 0) == 0);
	// AND THROUGH THE CARD'S OWN HOVER ROAD.
	HandClock clock;
	ui::TooltipLayer& layer = restOn(placed.card, at, clock);
	REQUIRE(layer.shown());
	CHECK(layer.text() == whole);
}

TEST_CASE("full name: every shortened or cut label on every card in a HORIZONTAL chain names itself in its first sentence") {
	// THE H-CHAIN'S COMPACT CARDS — laid out as the chain window lays them (`RackEditor::placeLane`): horizontal, one
	// unit tall — through the same sweep the lane's cards go through, which asserts the full name for every label that
	// is drawn cut or abbreviated.
	SweepTally tally;
	size_t devices = 0;
	for (const device::Descriptor& desc : kDevices) {
		const std::string tag = desc.traits.tag;
		CAPTURE(tag);
		ModFixture f;
		const Placed placed = place(f, desc);
		if (placed.card == nullptr) {
			continue;
		}
		placed.card->setLaneAxis(ui::FxCard::LaneAxis::Horizontal);
		placed.card->setBounds(0.0f, 0.0f, placed.card->laneExtent(ui::kCardUnitHeight), ui::kCardUnitHeight);
		placed.card->resized();
		(void)placed.card->update();
		++devices;
		sweepHost(placed.card, desc, tally, "h-chain " + tag);
	}
	MESSAGE("h-chain: " << devices << " devices, " << tally.sources << " hovered, " << tally.named << " named in full");
	CHECK(devices > 20);
	CHECK(tally.named > 0);
}

TEST_CASE("full name: the route list's heading names a cut source in full on hover, and a whole one says nothing") {
	// THE PAINTED HEADING (the follow-up): "ROUTES" and the focused source's name, fitted to the room
	// after it (`RouteList::headingName`). Not a widget — so the list answers `labelFaceAt` over the name's room.
	ModFixture f;
	auto* list = findInTree<ui::RouteList>(f.root);
	REQUIRE(list != nullptr);
	REQUIRE(list->width() > 0.0f);
	ui::MacroSlot* slot = f.panel->macroSlotForTest(0);
	REQUIRE(slot != nullptr);
	HandClock clock;

	// A WHOLE NAME: no tooltip at all, over the name or anywhere on the list.
	f.arm(ui::macroSourceAt(0));
	{
		const ui::RouteList::HeadingName name = list->headingName();
		REQUIRE_FALSE(name.whole.empty());
		REQUIRE(name.fitted.text == name.whole);
		const visage::Point at { name.room.right() - 2.0f, name.room.y() + name.room.height() * 0.5f };
		CHECK(ui::hoverText(*list, at).empty());
		ui::TooltipLayer& layer = restOn(list, at, clock);
		CHECK_FALSE(layer.shown());
		list->processMouseExit(hoverAt(list, at));
	}

	// THE LONGEST NAME A HAND CAN TYPE (24), in a list narrowed to 110 px
	// (the Engine rail's own width holds it whole, shrunk): the room cuts it.
	const std::string longName = "Humanize Amount Over Tim";
	slot->naming().rename(longName);
	slot->refresh();
	f.arm(ui::macroSourceAt(0));
	list->setBounds(list->x(), list->y(), 110.0f, list->height());
	const ui::RouteList::HeadingName name = list->headingName();
	REQUIRE(name.whole == longName);
	CAPTURE(name.fitted.text);
	REQUIRE(name.fitted.text != longName);
	REQUIRE(name.fitted.elided);
	const visage::Point at { name.room.right() - 2.0f, name.room.y() + name.room.height() * 0.5f };
	for (const ui::TooltipDetail detail : { ui::TooltipDetail::Full, ui::TooltipDetail::Brief }) {
		const DetailScope scope(detail);
		ui::TooltipLayer& layer = restOn(list, at, clock);
		REQUIRE(layer.shown());
		CHECK(layer.text() == longName);
		CHECK(layer.displayed() == longName);
		list->processMouseExit(hoverAt(list, at));
	}
	// AND BELOW THE HEADING, over a row's room, nothing.
	CHECK(ui::hoverText(*list, { 4.0f, ui::RouteList::kHeadingHeight + 4.0f }).empty());
}

TEST_CASE("full name: every macro TYPE in Feathers' main macros shows its name on hover, the relative encoder included") {
	// The defect: no hover tooltip on a relative/encoder macro — the encoder face (backlog PQ) had no hover
	// at all. So EVERY type is walked, at the short caption ("M1" → "Macro 1") and at a typed name cut to its column.
	ModFixture f;
	ui::MacroSlot* slot = f.panel->macroSlotForTest(0);
	REQUIRE(slot != nullptr);
	const std::string longName = "Humanize Amount Over Tim"; // 24: the longest a hand can type
	size_t types = 0;
	for (int t = 0; t < static_cast<int>(device::MacroType::kCount); ++t) {
		CAPTURE(std::string(device::kMacroTypeNames[t]));
		ui::chooseMacroMenuEntry(slot->menuHooks(), t);
		REQUIRE(static_cast<int>(slot->type()) == t);
		f.panel->resized();
		// THE FACE ON SHOW for this type, found by the hit test at the column's centre, as a hand finds it.
		for (const bool renamed : { false, true }) {
			CAPTURE(renamed);
			if (renamed) {
				slot->naming().rename(longName);
				slot->refresh();
			}
			visage::Frame* face = slot->frameAtPoint({ slot->width() * 0.5f, slot->height() * 0.5f });
			REQUIRE(face != nullptr);
			CAPTURE(demangled(*face));
			REQUIRE(dynamic_cast<ui::TooltipSource*>(face) != nullptr);
			const std::string full = expectedFullName(face);
			// THE NAME IS SAID IN BOTH STATES: "M1" abbreviates "Macro 1", and the typed name is cut.
			CHECK(full == (renamed ? longName : std::string("Macro 1")));
			for (const ui::TooltipDetail detail : { ui::TooltipDetail::Full, ui::TooltipDetail::Brief }) {
				const DetailScope scope(detail);
				HandClock clock;
				visage::Frame* hostFrame = nullptr;
				ui::TooltipLayer& layer = restOn(face, centreOf(*face), clock, &hostFrame);
				CHECK(hostFrame == f.panel);
				REQUIRE(layer.shown());
				CAPTURE(layer.text());
				const char* own = dynamic_cast<ui::TooltipSource*>(face)->tooltipAt(centreOf(*face));
				CHECK(layer.text() == expectedWithFull(full, own != nullptr ? own : ""));
				CHECK(layer.text().rfind(full, 0) == 0);
				CHECK(layer.displayed().rfind(full, 0) == 0);
				// AND ITS OWN TOOLTIP AFTER THE NAME: the encoder says what a turn does.
				if (dynamic_cast<ui::EncoderFace*>(face) != nullptr) {
					CHECK(layer.text() == full + " — " + ui::EncoderFace::kTooltip);
				}
				face->processMouseExit(hoverAt(face, centreOf(*face)));
			}
			if (renamed) {
				slot->naming().rename("");
				slot->refresh();
			}
		}
		++types;
	}
	CHECK(types == static_cast<size_t>(device::MacroType::kCount));
}

// ---- THE H FADERS' DOUBLE-CLICK ("some of the h faders which dont have toggle behavior dont re-default on
// double click"). An AMOUNT-ONLY pill resets its amount to the declared default, one undo step, as a knob and a vertical
// fader do; a pill WITH a power keeps its convention — every click toggles the power, so a double-click is two toggles and
// the amount is left alone. Swept over every card and every window, as a hand finds the pills. ----

namespace {

struct PillTally {
	size_t amountOnly = 0;
	size_t powered = 0;
	std::vector<std::string> broken;
};

/// A hand's double-click at the pill's centre, off the power symbol: press, release, press (2), release.
void doubleClickPill(ui::FaderPill& pill) {
	visage::Point c = centreOf(pill);
	if (pill.powerGlyphForTest().contains(c)) {
		c.x = pill.width() * 0.25f;
	}
	visage::MouseEvent first = at(c.x, c.y);
	first.event_frame = &pill;
	pill.processMouseDown(first);
	pill.processMouseUp(first);
	visage::MouseEvent second = doubleClickAt(c.x, c.y);
	second.event_frame = &pill;
	pill.processMouseDown(second);
	pill.processMouseUp(second);
}

void sweepPills(visage::Frame* host, PillTally& tally, const std::string& where) {
	for (visage::Frame* leaf : reachableLeaves(host)) {
		auto* pill = dynamic_cast<ui::FaderPill*>(leaf);
		if (pill == nullptr) {
			continue;
		}
		device::Modulatable& amount = pill->amountForTest();
		const device::ParamDesc& d = amount.desc();
		const std::string name = where + " / " + (d.name != nullptr ? d.name : "?");
		CAPTURE(name);
		// AWAY FROM ITS DEFAULT FIRST, so a reset is a value moving and not a value staying.
		const double away = std::fabs(d.max - d.def) > std::fabs(d.min - d.def) ? d.max : d.min;
		REQUIRE(away != d.def);
		amount.beginGesture();
		amount.setValue(away);
		amount.endGesture();
		REQUIRE(amount.value() == doctest::Approx(away));
		if (pill->amountOnly()) {
			doubleClickPill(*pill);
			// FLOAT STORAGE: a declared 0.3 reads back as 0.30000001, so the comparison is the stored precision's, not exact.
			const bool reset = std::fabs(amount.value() - d.def) <= 1e-6 * std::max(1.0, std::fabs(d.max - d.min));
			CHECK_MESSAGE(reset, "an amount-only pill a double-click did not put back to its default");
			if (!reset) {
				tally.broken.push_back(name);
			}
			++tally.amountOnly;
		}
		else {
			device::Modulatable* power = pill->powerForTest();
			REQUIRE(power != nullptr);
			const double before = power->value();
			doubleClickPill(*pill);
			CHECK(power->value() == doctest::Approx(before)); // two clicks, two toggles
			CHECK(amount.value() == doctest::Approx(away));   // and the amount left where the hand put it
			++tally.powered;
		}
	}
}

} // namespace

TEST_CASE("an amount-only h fader (FaderPill) resets to its default on a double-click, a powered one keeps its toggle — every "
          "card, every window, the Engine rail") {
	rack::registerChainExpandedView();
	PillTally tally;
	for (const device::Descriptor& desc : kDevices) {
		const std::string tag = desc.traits.tag;
		CAPTURE(tag);
		ModFixture f;
		ui::WidgetRegistry registry;
		const Placed placed = place(f, desc);
		if (placed.card != nullptr) {
			(void)placed.card->update();
			sweepPills(placed.card, tally, "card " + tag);
		}
		if (placed.device == kNoInstance || !ui::hasExpandedView(desc)) {
			continue;
		}
		HeldChain held;
		if (tag == sc::kTraits.tag) {
			f.bridge.fxBodies().attach(placed.device, held.surface());
		}
		std::unique_ptr<ui::ExpandedView> view = ui::makeExpandedView(f.deviceHost, &f.bridge, registry, placed.device, desc);
		if (view == nullptr) {
			continue;
		}
		const visage::Point min = view->minimumSize();
		view->setBounds(0.0f, 0.0f, std::max(min.x, 900.0f), std::max(min.y, 600.0f));
		(void)view->update();
		view->resized();
		sweepPills(view.get(), tally, "window " + tag);
	}
	// AND THE ENGINE RAIL'S SOUND STACK, whose per-sound Level faders are amount-only pills.
	{
		ModFixture f;
		auto* engineRail = findInTree<ui::EngineRail>(f.root);
		REQUIRE(engineRail != nullptr);
		for (const ui::EngineRail::Page page : { ui::EngineRail::Page::Pitch, ui::EngineRail::Page::Stack }) {
			engineRail->showPage(page);
			sweepPills(engineRail, tally, "Engine rail");
		}
	}
	std::string list;
	for (const std::string& b : tally.broken) {
		list += "\n  " + b;
	}
	MESSAGE("h faders: " << tally.amountOnly << " amount-only, " << tally.powered << " powered; broken:" << list);
	CHECK(tally.amountOnly > 0);
	CHECK(tally.powered > 0);
	CHECK(tally.broken.empty());
}

TEST_CASE("the h-fader double-click, on the three hosts the device sweep does not reach: the sound stack's Level pills, the "
          "Drum panel's pills and a parallel Rack card's chain pill") {
	// 1. THE SOUND STACK, as the editor builds it — each sound's Level, amount-only.
	{
		ModFixture f;
		// THE LEVEL PILLS ARE A KIT'S: the Drum engine in Kit mode.
		ParamSet params;
		params.setBase(static_cast<clap_id>(ParamId::OscType), static_cast<double>(OscType::Drum));
		params.setBase(static_cast<clap_id>(ParamId::DrumMode), 1.0);
		f.bridge.publish(params);
		f.bridge.refresh();
		f.gui.tickForTest();
		auto* stack = findInTree<ui::SoundStack>(f.root);
		REQUIRE(stack != nullptr);
		stack->resized();
		PillTally tally;
		sweepPills(stack, tally, "sound stack");
		CHECK(tally.amountOnly > 0);
		CHECK(tally.broken.empty());
	}
	// 2. THE DRUM PANEL, over the Drum engine — the selected pad's pills.
	{
		ExploreFixture f;
		f.set(ParamId::OscType, static_cast<double>(OscType::Drum));
		ui::DrumPanel panel(f.bridge, f.registry, f.selection, &f.tray);
		panel.setBounds(0.0f, 0.0f, ui::DrumPanel::kMinimumWidth, ui::DrumPanel::kMinimumHeight);
		(void)panel.update();
		PillTally tally;
		sweepPills(&panel, tally, "drum panel");
		CHECK(tally.amountOnly + tally.powered > 0);
		CHECK(tally.broken.empty());
	}
	// 3. A PARALLEL RACK ON THE LANE WITH A CHAIN — each chain strip's Level pill, amount-only.
	{
		namespace pr = feathers::rack::parallel;
		ModFixture f;
		feathers::rack::RackEngine bodies { feathers::rack::kRackAxes, feathers::hostCatalogue() };
		bodies.prepare(48000.0, 4096);
		const size_t at = bodies.addDevice(pr::kTraits.tag);
		REQUIRE(at != feathers::rack::RackEngine::kNoSlot);
		void* body = bodies.slotAtLane(at).storage.get();
		REQUIRE(body != nullptr);
		const size_t slot = f.bridge.addFxDevice(pr::kTraits.tag);
		REQUIRE(slot < feathers::kMaxFxSlots);
		const DeviceInstanceId id = f.bridge.fxDeviceAt(slot);
		f.bridge.fxBodies().attach(id, body);
		while (pr::rackOf(body)->chainCount() < 2) {
			(void)pr::rackOf(body)->addChain();
		}
		auto* lane = findInTree<ui::FxLane>(f.root);
		REQUIRE(lane != nullptr);
		lane->setCollapsed(false);
		lane->resized();
		lane->scrollCardIntoView(slot);
		lane->resized();
		auto* card = dynamic_cast<ui::ParallelRackCard*>(lane->cardAt(slot));
		REQUIRE(card != nullptr);
		(void)card->update();
		card->resized();
		PillTally tally;
		sweepPills(card, tally, "parallel rack card");
		CHECK(tally.amountOnly > 0);
		CHECK(tally.broken.empty());
		f.bridge.fxBodies().detach(id, body);
		(void)f.bridge.removeFxDevice(slot);
	}
}

TEST_CASE("every modulatable face glows for an armed source and shows a routed mark once routed (Pattern's Go \"doesnt glow\")") {
	// **THE GLOW-HONESTY RULE, ASKED OF THE WIDGETS** (`ui::ControlFace::modLookFor`), as the superset guard asks them
	// what they offer: on the card and the window, each EFFECTIVELY visible face over a device control the armed source
	// may route to must draw the glow; a face over one it may not route to must not. Disarmed, with a route in place,
	// each such face draws a routed mark. The enable is chrome (the card head's square), not asked, as the superset
	// guard does not ask it.
	//
	// ASSERTED FOR EVERY DEVICE (backlog QK, 2026-11-07: `SwitchChip` shows modulation by default, and the cells, the
	// vertical fader and the trig pad answer for what they draw). `kAllStates` are walked in every state the superset guard
	// visits (each page, each gate control swept through its values) — Pattern's Go shows only on a fill tab, so the default
	// state alone never met it; the rest in their default state.
	//
	// **`kGlowDeferred`: THE GAPS WITH A DIFFERENT CAUSE, by face, each with its reason** (listed in QK, not fixed here). A
	// gap on a face this names is LISTED and not asserted; any other gap fails.
	static const std::set<std::string> kAllStates { "PAT" };
	struct Deferred {
		const char* face; // "<tag> <surface> / <key>"
		const char* why;
	};
	static const std::vector<Deferred> kGlowDeferred {
		// (EMPTY since 2026-11-08: greyed faces glow, dimmer — QK, by design. The greyed entries became assertions.)
	};
	const auto deferred = [](const std::string& line) {
		for (const Deferred& d : kGlowDeferred) {
			if (line.rfind(d.face, 0) == 0) {
				return true;
			}
		}
		return false;
	};
	rack::registerChainExpandedView();
	const backplane::Source source = backplane::Source::core(0);
	size_t asserted = 0;
	std::set<std::string> failures;
	std::set<std::string> litAndMarked; // asserted devices: the keys seen lit when armed AND marked once routed
	std::vector<std::string> faceless;  // asserted devices: routable controls no state ever showed a face for
	std::map<std::string, std::set<std::string>> gapsByDevice;
	for (const device::Descriptor& desc : kDevices) {
		const std::string tag = desc.traits.tag;
		CAPTURE(tag);
		if (desc.container != nullptr) {
			continue; // a container's window edits a live body: the superset guard builds one, this does not
		}
		const bool assertHere = kAllStates.count(tag) > 0; // every state walked, and Go-by-name bookkeeping
		ModFixture f;
		ui::WidgetRegistry registry;
		const Placed placed = place(f, desc);
		REQUIRE(placed.device != kNoInstance);
		REQUIRE(placed.card != nullptr);
		std::unique_ptr<ui::ExpandedView> view;
		if (ui::hasExpandedView(desc)) {
			view = ui::makeExpandedView(f.deviceHost, &f.bridge, registry, placed.device, desc);
			if (view != nullptr) {
				const visage::Point min = view->minimumSize();
				view->setBounds(0.0f, 0.0f, std::max(min.x, 900.0f), std::max(min.y, 600.0f));
			}
		}
		ui::ModState* mod = f.gui.modForTest();
		REQUIRE(mod != nullptr);
		if (view != nullptr) {
			view->setModulation(mod, f.gui.history()); // as the canvas seats a window
		}
		std::set<size_t> seenFace;
		const size_t enable = device::enableControlIn(desc.traits, desc.params);
		struct Face {
			const ui::ControlFace* face;
			const device::Modulatable* control;
			std::string where;
			/// A PLOT'S DRAG (`Road::Gesture`): held to the rule only where it draws modulation at all (the puck, the markers);
			/// that those DO is asserted by name below.
			bool gesture = false;
		};
		const auto faces = [&] {
			std::vector<Face> out;
			std::vector<ui::ControlFace::Offered> list;
			for (const visage::Frame* root : { static_cast<const visage::Frame*>(placed.card),
			                                   static_cast<const visage::Frame*>(view.get()) }) {
				if (root == nullptr) {
					continue;
				}
				const char* surface = root == placed.card ? "card" : "window";
				std::vector<const visage::Frame*> stack { root };
				while (!stack.empty()) {
					const visage::Frame* fr = stack.back();
					stack.pop_back();
					if (!fr->isVisible()) {
						continue;
					}
					if (const auto* face = dynamic_cast<const ui::ControlFace*>(fr)) {
						list.clear();
						face->offeredControls(list);
						for (const ui::ControlFace::Offered& o : list) {
							const size_t i = o.control != nullptr ? indexIn(desc, *o.control) : static_cast<size_t>(-1);
							if (i >= desc.params.size() || i == enable) {
								continue;
							}
							const bool gesture = o.road != ui::ControlFace::Road::Face;
							if (gesture && !face->modLookFor(*o.control).drawn) {
								continue; // a plot drag that draws no modulation (most do not)
							}
							const char* key = o.control->desc().key != nullptr ? o.control->desc().key : "?";
							out.push_back({ face, o.control, tag + " " + surface + " / " + key + " (" + demangled(*fr) + ")", gesture });
						}
					}
					for (const visage::Frame* c : fr->children()) {
						stack.push_back(c);
					}
				}
			}
			return out;
		};
		// THE DESTINATION A FACE'S CONTROL IS, asked of the device's own index when the face holds a VIEW over the control
		// (the MACROS card's `BipolarMacro`) rather than the `DeviceControl` itself — a view read as "not routable" made the
		// sweep report its honest glow as a lie (backlog QK, 2026-11-08).
		const auto destOf = [&](const device::Modulatable* c) {
			if (const auto* road = dynamic_cast<const ui::DeviceControl*>(c)) {
				return road->destination();
			}
			return backplane::Destination::control(placed.device, static_cast<uint32_t>(indexIn(desc, *c)));
		};
		const auto routable = [&](const device::Modulatable* c) { return f.bridge.mayRoute(source, destOf(c)); };
		const auto refresh = [&] {
			f.gui.tickForTest();
			placed.card->resized();
			(void)placed.card->update();
			if (view != nullptr) {
				(void)view->update();
				view->resized();
				(void)view->update();
			}
		};
		const auto gap = [&](const std::string& line) {
			gapsByDevice[tag].insert(line);
			if (!deferred(line)) {
				failures.insert(line); // EVERY DEVICE (backlog QK)
			}
		};
		const auto checkNow = [&](const std::string& state) {
			// ARMED: glow iff routable.
			mod->focus(source);
			mod->arm(true);
			refresh();
			std::vector<backplane::Destination> routes;
			for (const Face& face : faces()) {
				const bool may = routable(face.control);
				const ui::ControlFace::ModLook look = face.face->modLookFor(*face.control);
				++asserted;
				if (may && !(look.drawn && look.armedGlow)) {
					gap(face.where + ": routable, no glow (" + state + ")");
				}
				if (!may && look.armedGlow) {
					gap(face.where + ": NOT routable, glows (" + state + ")");
				}
				if (may) {
					routes.push_back(destOf(face.control));
				}
			}
			// ROUTED, NOT ARMED: every routable face shows its route.
			mod->arm(false);
			for (const backplane::Destination& d : routes) {
				f.bridge.setRouteAmount(source, d, 0.5f);
			}
			refresh();
			for (const Face& face : faces()) {
				if (routable(face.control) && !face.face->modLookFor(*face.control).routedMark) {
					gap(face.where + ": routed, no mark (" + state + ")");
				}
				if (assertHere && routable(face.control) && face.face->modLookFor(*face.control).routedMark) {
					seenFace.insert(indexIn(desc, *face.control));
					litAndMarked.insert(face.control->desc().key != nullptr ? face.control->desc().key : "?");
				}
			}
			// AND WITH THE FOCUS ON ANOTHER SOURCE (an LFO's route seen while the macro is focused): `routedElsewhere`'s
			// road, which a mark keyed on the focused source's amount alone would miss.
			mod->focus(backplane::Source::core(1));
			refresh();
			for (const Face& face : faces()) {
				if (routable(face.control) && !face.face->modLookFor(*face.control).routedMark) {
					gap(face.where + ": routed from an unfocused source, no mark (" + state + ")");
				}
			}
			mod->focus(source);
			for (const backplane::Destination& d : routes) {
				f.bridge.setRouteAmount(source, d, 0.0f);
			}
			refresh();
		};
		if (!assertHere) {
			checkNow("default");
			continue;
		}
		// THE DECLARED GATES, AND EVERY SWITCH: a surface may gate a face on a switch no descriptor names as a gate (Fill
		// shows Go and the Span markers, and declares nothing).
		std::set<size_t> gates;
		for (size_t i = 0; i < desc.params.size(); ++i) {
			const device::ParamDesc& p = desc.params[i];
			if (p.gateControl >= 0 && static_cast<size_t>(p.gateControl) < desc.params.size()) {
				gates.insert(static_cast<size_t>(p.gateControl));
			}
			if (p.stepped && p.min == 0.0 && p.max == 1.0 && i != enable) {
				gates.insert(i);
			}
		}
		const auto sweep = [&](const std::string& page) {
			checkNow(page + "default");
			for (const size_t g : gates) {
				const float was = f.deviceHost.fieldAt(placed.device, g);
				for (const float v : gateValues(desc.params[g])) {
					f.deviceHost.setField(placed.device, g, v);
					char value[32];
					std::snprintf(value, sizeof value, "%g", static_cast<double>(v));
					checkNow(page + paramName(desc, g) + " = " + value);
				}
				f.deviceHost.setField(placed.device, g, was);
			}
		};
		auto* pagedCard = dynamic_cast<ui::PagedControls*>(placed.card);
		auto* pagedView = dynamic_cast<ui::PagedControls*>(view.get());
		if (pagedCard != nullptr && pagedView != nullptr) {
			for (size_t page = 0; page < pagedCard->pageCount(); ++page) {
				pagedCard->showPage(page);
				pagedView->showPage(page);
				sweep("page " + std::to_string(page + 1) + ", ");
			}
		}
		else {
			sweep("");
		}
		for (size_t i = 0; i < desc.params.size(); ++i) {
			if (i != enable && seenFace.count(i) == 0
			    && f.bridge.mayRoute(source, backplane::Destination::control(placed.device, static_cast<uint32_t>(i)))) {
				faceless.push_back(tag + " / " + (desc.params[i].key != nullptr ? desc.params[i].key : "?"));
			}
		}
	}
	for (const std::string& line : failures) {
		FAIL_CHECK(line);
	}
	std::string listed;
	for (const auto& [tag, lines] : gapsByDevice) {
		listed += " " + tag + ":" + std::to_string(lines.size());
	}
	MESSAGE("glow gaps by device (deferred ones listed, every other one a failure):" << listed);
	// AND EVERY DEFERRAL STILL NAMES A REAL GAP: one whose face was fixed is taken out, not left to excuse the next regression.
	for (const Deferred& d : kGlowDeferred) {
		bool still = false;
		for (const auto& [t, lines] : gapsByDevice) {
			for (const std::string& l : lines) {
				still = still || l.rfind(d.face, 0) == 0;
			}
		}
		CAPTURE(std::string(d.face));
		CHECK_MESSAGE(still, "a deferral that names no gap any more");
	}
	std::string none;
	for (const std::string& k : faceless) {
		none += " " + k;
	}
	MESSAGE("routable, but no face in any state (a plot drag at most):" << none);
	// GO, BY NAME (where the defect was seen): every tab's, lit for the armed source and marked once routed.
	for (const char* go : { "p1.fill_trigger", "p2.fill_trigger", "p3.fill_trigger", "p4.fill_trigger" }) {
		CHECK_MESSAGE(litAndMarked.count(go) == 1, go);
	}
	// THE PUCK AND THE MARKERS, BY NAME: plot drags that are faces for modulation — every tab's.
	for (const char* p : { "p1", "p2", "p3", "p4" }) {
		for (const char* k : { ".place_x", ".place_y", ".start", ".end" }) {
			const std::string key = std::string(p) + k;
			CHECK_MESSAGE(litAndMarked.count(key) == 1, key);
		}
	}
	CHECK(asserted > 100); // not vacuous: Pattern's card and window hold dozens of faces, in dozens of states
}

TEST_CASE("Pattern's Go and Humanize take a route as they glow: armed, a press-drag sets the amount and fires nothing; a double-click unroutes; Alt picks") {
	// THE GLOW'S PROMISE KEPT (Go "doesnt glow"): a lit face offers a drag that routes. Go is a chip
	// (`makeModulatedChip`), Humanize's selector a `ZoneSelect` that now takes the dropdown's road.
	namespace pt = mod::arp::pat;
	rack::registerChainExpandedView();
	const device::Descriptor& desc = pt::kDescriptor;
	ModFixture f;
	ui::WidgetRegistry registry;
	const Placed placed = place(f, desc);
	REQUIRE(placed.device != kNoInstance);
	f.deviceHost.setField(placed.device, pt::at(0, pt::Field::Fill), 1.0f); // tab 1 a fill: Go shows
	std::unique_ptr<ui::ExpandedView> view = ui::makeExpandedView(f.deviceHost, &f.bridge, registry, placed.device, desc);
	REQUIRE(view != nullptr);
	ui::ModState* mod = f.gui.modForTest();
	REQUIRE(mod != nullptr);
	view->setModulation(mod, f.gui.history());
	view->setBounds(0.0f, 0.0f, std::max(view->minimumSize().x, 900.0f), std::max(view->minimumSize().y, 600.0f));
	f.gui.tickForTest();
	(void)view->update();
	view->resized();
	const auto find = [&]<class W>(const char* key) -> W* {
		std::vector<visage::Frame*> stack { view.get() };
		while (!stack.empty()) {
			visage::Frame* fr = stack.back();
			stack.pop_back();
			if (auto* w = dynamic_cast<W*>(fr); w != nullptr && fr->isVisible() && w->control().desc().key != nullptr
			                                    && std::string(w->control().desc().key) == key) {
				return w;
			}
			for (visage::Frame* c : fr->children()) {
				stack.push_back(c);
			}
		}
		return nullptr;
	};
	ui::SwitchChip* go = find.template operator()<ui::SwitchChip>("p1.fill_trigger");
	ui::ZoneSelect* humanize = find.template operator()<ui::ZoneSelect>("humanize");
	REQUIRE(go != nullptr);
	REQUIRE(humanize != nullptr);
	const backplane::Source source = backplane::Source::core(0);
	const backplane::Destination goDest = dynamic_cast<const ui::DeviceControl&>(go->control()).destination();
	const backplane::Destination huDest = dynamic_cast<const ui::DeviceControl&>(humanize->control()).destination();
	CHECK_FALSE(go->modLookFor(go->control()).armedGlow); // nothing armed
	CHECK_FALSE(humanize->modLookFor(humanize->control()).armedGlow);
	mod->focus(source);
	mod->arm(true);
	CHECK(go->modLookFor(go->control()).armedGlow);
	CHECK(humanize->modLookFor(humanize->control()).armedGlow);
	const float seed = ui::SwitchChip::kRouteSeedAmount;
	const float range = ui::SwitchChip::kRouteDragRange;
	SUBCASE("Go: the press routes, the drag sets how far, and Go never fired") {
		const double before = go->control().value();
		go->onMouseDown().callback(at(0.0f, 0.0f));
		CHECK(f.bridge.routeAmount(source, goDest) == doctest::Approx(seed));
		const visage::MouseEvent drag = at(0.0f, -range * 0.5f);
		go->onMouseDrag().callback(drag);
		go->onMouseUp().callback(drag);
		CHECK(f.bridge.routeAmount(source, goDest) == doctest::Approx(seed + 0.5f));
		CHECK(go->control().value() == before);
		mod->arm(false);
		CHECK(go->modLookFor(go->control()).routedMark);
	}
	SUBCASE("Humanize: the press routes, the drag sets how far, the choice is untouched; a double-click unroutes") {
		const double before = humanize->control().value();
		humanize->onMouseDown().callback(at(1.0f, 1.0f));
		CHECK(f.bridge.routeAmount(source, huDest) == doctest::Approx(seed));
		const visage::MouseEvent drag = at(1.0f, 1.0f - range * 0.5f);
		humanize->onMouseDrag().callback(drag);
		humanize->onMouseUp().callback(drag);
		CHECK(f.bridge.routeAmount(source, huDest) == doctest::Approx(seed + 0.5f));
		CHECK(humanize->control().value() == before);
		humanize->onMouseDown().callback(doubleClickAt(1.0f, 1.0f));
		CHECK(f.bridge.routeAmount(source, huDest) == 0.0f);
	}
	SUBCASE("Humanize with Alt: a pick, never a route") {
		const double before = humanize->control().value();
		const visage::Bounds next = humanize->nextBounds();
		humanize->onMouseDown().callback(optAt(next.x() + next.width() * 0.5f, next.y() + next.height() * 0.5f));
		CHECK(f.bridge.routeAmount(source, huDest) == 0.0f);
		CHECK(humanize->control().value() != before); // stepped to the next zone
	}
}

TEST_CASE("QK: a MACROS card's macro knob glows exactly when the bridge may route the armed source to it — never where it refuses") {
	// THE GLOW-HONESTY RULE ON THE ONE FACE THAT WEARS A VIEW (`BipolarMacro`) over its `DeviceControl`: the knob's glow is
	// asked of the face (`modLookFor`), the answer of the bridge (`mayRoute`) — two independent roads, by value, for a source
	// that may reach it (Macro 1) and one that may not (the card's OWN output, a self-loop).
	ModFixture f;
	const Placed placed = place(f, descriptorByTag(feathers::mod::macros::kTraits.tag));
	auto* card = dynamic_cast<ui::MacrosCard*>(placed.card);
	REQUIRE(card != nullptr);
	ui::ModState* mod = f.gui.modForTest();
	REQUIRE(mod != nullptr);
	size_t mayCount = 0;
	size_t refusedCount = 0;
	for (const backplane::Source source : { backplane::Source::core(0), backplane::Source::output(placed.device, 0) }) {
		mod->focus(source);
		mod->arm(true);
		f.gui.tickForTest();
		(void)card->update();
		for (size_t n = 0; n < ui::MacrosCard::kSlotCount; ++n) {
			CAPTURE(n);
			ui::Knob& knob = card->slot(n).knob();
			REQUIRE(knob.control() != nullptr);
			const bool may = f.bridge.mayRoute(
			    source, backplane::Destination::control(placed.device, static_cast<uint32_t>(feathers::mod::macros::macroParam(n))));
			CHECK(knob.modLookFor(*knob.control()).armedGlow == may);
			(may ? mayCount : refusedCount) += 1;
		}
		mod->arm(false);
	}
	CHECK(mayCount > 0);     // Macro 1 reaches them
	CHECK(refusedCount > 0); // their own output does not: not vacuous in either direction
}

TEST_CASE("Pattern's puck and markers take a route as they glow: armed, on the puck a horizontal drag routes Place X only and a vertical one Place Y only; off it, or with Alt, the puck moves; a marker's handle routes by vertical travel; double-clicks unroute") {
	namespace pt = mod::arp::pat;
	rack::registerChainExpandedView();
	ModFixture f;
	ui::WidgetRegistry registry;
	const Placed placed = place(f, pt::kDescriptor);
	REQUIRE(placed.device != kNoInstance);
	std::unique_ptr<ui::ExpandedView> view =
	    ui::makeExpandedView(f.deviceHost, &f.bridge, registry, placed.device, pt::kDescriptor);
	REQUIRE(view != nullptr);
	ui::ModState* mod = f.gui.modForTest();
	REQUIRE(mod != nullptr);
	view->setModulation(mod, f.gui.history());
	view->setBounds(0.0f, 0.0f, std::max(view->minimumSize().x, 900.0f), std::max(view->minimumSize().y, 600.0f));
	f.gui.tickForTest();
	(void)view->update();
	view->resized();
	ui::PatternTerrain* terrain = findInTree<ui::PatternTerrain>(view.get());
	ui::PatternLane* lane = findInTree<ui::PatternLane>(view.get());
	REQUIRE(terrain != nullptr);
	REQUIRE(lane != nullptr);
	const backplane::Source source = backplane::Source::core(0);
	const auto destOf = [](const ui::DeviceControl& c) { return c.destination(); };
	mod->focus(source);
	mod->arm(true);
	const float seed = ui::PatternTerrain::kRouteSeedAmount;
	const float range = ui::PatternTerrain::kRouteDragRange;
	SUBCASE("the puck: one axis per drag, a double-click unroutes both") {
		ui::DeviceControl& x = terrain->placeXForTest();
		ui::DeviceControl& y = terrain->placeYForTest();
		const double x0 = x.value();
		const double y0 = y.value();
		const visage::Point p = terrain->puckPosition();
		terrain->onMouseDown().callback(at(p.x, p.y));
		terrain->onMouseDrag().callback(at(p.x + range * 0.5f, p.y));
		terrain->onMouseUp().callback(at(p.x + range * 0.5f, p.y));
		CHECK(f.bridge.routeAmount(source, destOf(x)) == doctest::Approx(seed + 0.5f));
		CHECK(f.bridge.routeAmount(source, destOf(y)) == 0.0f); // no vertical travel: Y untouched
		CHECK(x.value() == x0); // routing moved nothing
		CHECK(y.value() == y0);
		terrain->onMouseDown().callback(at(p.x, p.y));
		terrain->onMouseDrag().callback(at(p.x, p.y - range * 0.25f)); // up
		terrain->onMouseUp().callback(at(p.x, p.y - range * 0.25f));
		CHECK(f.bridge.routeAmount(source, destOf(y)) == doctest::Approx(seed + 0.25f));
		CHECK(f.bridge.routeAmount(source, destOf(x)) == doctest::Approx(seed + 0.5f)); // X kept
		CHECK(terrain->modLookFor(x).armedGlow);
		mod->arm(false);
		CHECK(terrain->modLookFor(x).routedMark);
		CHECK(terrain->modLookFor(y).routedMark);
		mod->arm(true);
		terrain->onMouseDown().callback(doubleClickAt(p.x, p.y));
		CHECK(f.bridge.routeAmount(source, destOf(x)) == 0.0f);
		CHECK(f.bridge.routeAmount(source, destOf(y)) == 0.0f);
	}
	SUBCASE("off the puck, and with Alt on it, the puck moves and nothing routes") {
		ui::DeviceControl& x = terrain->placeXForTest();
		const double x0 = x.value();
		const visage::Point p = terrain->puckPosition();
		const visage::Point off { p.x + 40.0f, p.y };
		terrain->onMouseDown().callback(at(off.x, off.y));
		terrain->onMouseUp().callback(at(off.x, off.y));
		CHECK(x.value() != x0);
		CHECK(f.bridge.routeAmount(source, destOf(x)) == 0.0f);
		const double x1 = x.value();
		const visage::Point q = terrain->puckPosition();
		terrain->onMouseDown().callback(optAt(q.x, q.y));
		terrain->onMouseDrag().callback(optAt(q.x + 30.0f, q.y));
		terrain->onMouseUp().callback(optAt(q.x + 30.0f, q.y));
		CHECK(x.value() != x1);
		CHECK(f.bridge.routeAmount(source, destOf(x)) == 0.0f);
	}
	SUBCASE("a marker's handle: vertical travel is its amount, the marker stays; a double-click unroutes; Alt drags it") {
		ui::DeviceControl* start = lane->startControlForTest();
		REQUIRE(start != nullptr);
		const double s0 = start->value();
		const float hx = lane->markX(lane->startMarkForTest());
		lane->onMouseDown().callback(at(hx, 3.0f));
		CHECK(f.bridge.routeAmount(source, destOf(*start)) == doctest::Approx(seed));
		lane->onMouseDrag().callback(at(hx, 3.0f - range * 0.5f));
		lane->onMouseUp().callback(at(hx, 3.0f - range * 0.5f));
		CHECK(f.bridge.routeAmount(source, destOf(*start)) == doctest::Approx(seed + 0.5f));
		CHECK(start->value() == s0);
		CHECK(lane->modLookFor(*start).armedGlow);
		lane->onMouseDown().callback(doubleClickAt(hx, 3.0f));
		CHECK(f.bridge.routeAmount(source, destOf(*start)) == 0.0f);
		const float column = lane->markX(1) - lane->markX(0);
		lane->onMouseDown().callback(optAt(hx, 3.0f));
		lane->onMouseDrag().callback(optAt(hx + 3.0f * column, 3.0f));
		lane->onMouseUp().callback(optAt(hx + 3.0f * column, 3.0f));
		CHECK(start->value() == doctest::Approx(s0 + 3.0));
		CHECK(f.bridge.routeAmount(source, destOf(*start)) == 0.0f);
	}
}

//--------------------------------------------------------------------------------------------------//
// A GATE'S RELAYOUT IS THE RELAYOUT A RESIZE DOES                                //
//--------------------------------------------------------------------------------------------------//
//
// THE DEFECT: layout issues on the Additive engine (at least) after enabling Vocode, until the window was resized. A gate that
// reveals or hides controls re-lays its card, window or panel on the per-frame road, never through `setBounds`. Two things a
// resize does can be missed there: the children's bounds (a gate road that lays out only part of the tree) and the owner's OWN
// picture — section headings and panel frames drawn at positions the layout computed, which visage repaints only on
// `redraw()`. The spectrum panel had the second: its Vocode analysis panel opened, the controls moved, the frames behind them
// stayed where the last paint put them. These cases sweep every gate and assert both, by value.

namespace {

/// Every descendant of `root` with its bounds and its own visibility, in tree order — BY VALUE, and NO POINTER. A snapshot
/// outlives the frames it describes: a gate's update can rebuild part of the tree (a parallel rack's phantom rails are
/// freed and made again), and a frame pointer kept across that is a read of a freed frame. Nor is an address an identity
/// to compare: a rebuilt frame can be allocated where the freed one was. So what a snapshot keeps is what the comparisons
/// need — the frame's type name and its depth below `root` (1 = a direct child), recorded while it was alive.
struct LaidOut {
	std::string type;
	int depth = 0;
	visage::Bounds bounds;
	bool visible = false;
};

void snapshotLayout(const visage::Frame& root, int depth, std::vector<LaidOut>& out) {
	for (const visage::Frame* child : root.children()) {
		const auto& frameRef = *child;
		out.push_back({ typeid(frameRef).name(), depth, child->bounds(), child->isVisible() });
		snapshotLayout(*child, depth + 1, out);
	}
}

std::vector<LaidOut> layoutOf(const visage::Frame& root) {
	std::vector<LaidOut> out;
	snapshotLayout(root, 1, out);
	return out;
}

/// The lines where `a` (what the gate road left) differs from `b` (what a resize lays out); empty when they agree.
std::string layoutDiff(const std::vector<LaidOut>& a, const std::vector<LaidOut>& b) {
	if (a.size() != b.size()) {
		return " the tree changed size across a resize (" + std::to_string(a.size()) + " vs " + std::to_string(b.size()) + ")";
	}
	std::string out;
	for (size_t i = 0; i < a.size(); ++i) {
		const bool same = a[i].visible == b[i].visible && (!a[i].visible || a[i].bounds == b[i].bounds);
		if (!same) {
			char line[200];
			std::snprintf(line, sizeof line, " [%s at %.1f,%.1f %.1fx%.1f%s, a resize: %.1f,%.1f %.1fx%.1f%s]", a[i].type.c_str(),
			              a[i].bounds.x(), a[i].bounds.y(), a[i].bounds.width(), a[i].bounds.height(), a[i].visible ? "" : " hidden",
			              b[i].bounds.x(), b[i].bounds.y(), b[i].bounds.width(), b[i].bounds.height(), b[i].visible ? "" : " hidden");
			out += line;
		}
	}
	return out;
}

/// WHETHER ANY DIRECT CHILD OF THE OWNER THAT IS SHOWN IN BOTH MOVED — the change an owner's own picture can depend on (a
/// heading over a row, a panel's frame around its cells). A child only shown or hidden moves nothing the owner drew around
/// it, and a grandchild that moved inside its cell is the cell's to repaint, not the owner's.
///
/// BY VALUE, BOTH SNAPSHOTS TAKEN BY `layoutOf(owner)`: a tree whose shape changed (a different count, or a different
/// type or depth at the same place in tree order) counts as moved, since a rebuild is a relayout; otherwise a direct
/// child (depth 1) shown in both whose bounds differ. No frame is dereferenced and no address compared, so a frame freed
/// and rebuilt between the two snapshots — even at the same address — is judged on its recorded geometry alone.
bool directChildMoved(const std::vector<LaidOut>& a, const std::vector<LaidOut>& b) {
	if (a.size() != b.size()) {
		return true;
	}
	for (size_t i = 0; i < a.size(); ++i) {
		if (a[i].type != b[i].type || a[i].depth != b[i].depth) {
			return true;
		}
		if (a[i].depth == 1 && a[i].visible && b[i].visible && !(a[i].bounds == b[i].bounds)) {
			return true;
		}
	}
	return false;
}

/// THE RELAYOUT A RESIZE DOES, forced: one pixel wider and back, so `setBounds` cannot dedup either step.
void forceResize(visage::Frame& frame) {
	const visage::Bounds b = frame.bounds();
	frame.setBounds(b.x(), b.y(), b.width() + 1.0f, b.height());
	frame.setBounds(b);
}

/// COUNTS THE REDRAWS `target` ITSELF ASKS FOR (children share the handler and are not counted) — the delivery, not the
/// report. `rearm()` clears visage's pending latch as a drawn frame would. The previous handler is put back on destruction.
struct OwnRedraws {
	visage::Frame* target = nullptr;
	visage::FrameEventHandler* previous = nullptr;
	visage::FrameEventHandler handler;
	int count = 0;

	explicit OwnRedraws(visage::Frame* frame) : target(frame), previous(frame->eventHandler()) {
		handler.request_redraw = [this](visage::Frame* asked) {
			if (asked == target) {
				++count;
			}
		};
		target->setEventHandler(&handler);
	}
	~OwnRedraws() { target->setEventHandler(previous); }
	OwnRedraws(const OwnRedraws&) = delete;
	OwnRedraws& operator=(const OwnRedraws&) = delete;
	void rearm() {
		target->setEventHandler(&handler);
		count = 0;
	}
};

} // namespace

TEST_CASE("a layout snapshot is by value: a child freed and rebuilt between two snapshots, even at the same address, is judged by "
          "its geometry, and nothing freed is read") {
	// The sweep below snapshots a card or window, lets a gate's update rebuild part of its tree, and snapshots again. The
	// first snapshot must hold no frame pointer (a freed frame read — the sanitizer gate's report on a parallel rack's
	// phantom rails), and the comparison must not take an equal address for the same frame (a rebuilt one can land where
	// the freed one was). KNOWN-BAD: a snapshot holding the frame pointer, with `directChildMoved` reading `parent()` through
	// it — the sweep below ("a gate's relayout is a resize's") reads a freed phantom rail, ASan red.
	struct Box : visage::Frame {};
	struct Other : visage::Frame {};
	visage::Frame owner;
	owner.setBounds(0.0f, 0.0f, 200.0f, 100.0f);
	auto child = std::make_unique<Box>();
	owner.addChild(child.get());
	child->setBounds(10.0f, 10.0f, 50.0f, 20.0f);
	const std::vector<LaidOut> before = layoutOf(owner);
	const void* was = child.get();
	// REBUILT AT THE SAME PLACE: freed (removed first, as the product does), a new one made — the allocator often hands back
	// the same address — and laid out where the old one was.
	owner.removeChild(child.get());
	child.reset();
	child = std::make_unique<Box>();
	owner.addChild(child.get());
	child->setBounds(10.0f, 10.0f, 50.0f, 20.0f);
	const bool sameAddress = static_cast<const void*>(child.get()) == was; // usually true; false under ASan's quarantine
	MESSAGE("rebuilt at the same address: " << std::string(sameAddress ? "yes" : "no"));
	CHECK_FALSE(directChildMoved(before, layoutOf(owner))); // same type, same place: nothing the owner drew around moved
	child->setBounds(12.0f, 10.0f, 50.0f, 20.0f);
	CHECK(directChildMoved(before, layoutOf(owner))); // the rebuilt child sits elsewhere: moved, whatever its address
	// A DIFFERENT KIND OF FRAME in its place is a different tree, moved by definition.
	owner.removeChild(child.get());
	child.reset();
	auto other = std::make_unique<Other>();
	owner.addChild(other.get());
	other->setBounds(10.0f, 10.0f, 50.0f, 20.0f);
	CHECK(directChildMoved(before, layoutOf(owner)));
	owner.removeChild(other.get());
}

TEST_CASE("a gate's relayout is a resize's: every card and window, every gate value, lays out as a resize does and repaints itself") {
	rack::registerChainExpandedView();
	size_t moves = 0;
	std::vector<std::string> failures;
	for (const device::Descriptor& desc : kDevices) {
		const std::string tag = desc.traits.tag;
		CAPTURE(tag);
		std::set<size_t> gates;
		for (const device::ParamDesc& p : desc.params) {
			if (p.gateControl >= 0 && static_cast<size_t>(p.gateControl) < desc.params.size()) {
				gates.insert(static_cast<size_t>(p.gateControl));
			}
		}
		if (gates.empty()) {
			continue;
		}
		std::unique_ptr<rack::RackEngine> bodies;
		void* body = nullptr;
		if (desc.container != nullptr) {
			bodies = std::make_unique<rack::RackEngine>(rack::kRackAxes, feathers::hostCatalogue());
			bodies->prepare(48000.0, 4096);
			const size_t lane = bodies->addDevice(desc.traits.tag);
			REQUIRE(lane != rack::RackEngine::kNoSlot);
			body = bodies->slotAtLane(lane).storage.get();
		}
		ModFixture f;
		ui::WidgetRegistry registry;
		const Placed placed = place(f, desc);
		REQUIRE(placed.device != kNoInstance);
		REQUIRE(placed.card != nullptr);
		if (body != nullptr) {
			f.bridge.fxBodies().attach(placed.device, body);
		}
		std::unique_ptr<ui::ExpandedView> view;
		if (ui::hasExpandedView(desc)) {
			view = ui::makeExpandedView(f.deviceHost, &f.bridge, registry, placed.device, desc);
			REQUIRE(view != nullptr);
			const visage::Point min = view->minimumSize();
			view->setBounds(0.0f, 0.0f, std::max(min.x, 900.0f), std::max(min.y, 600.0f));
		}
		const auto settle = [&] {
			for (int i = 0; i < 3; ++i) {
				f.gui.tickForTest();
				(void)placed.card->update();
				if (view != nullptr) {
					(void)view->update();
				}
			}
		};
		settle();
		// ONE OWNER AT A TIME: the gate moved on the per-frame road only (the tick and `update`, never a resize), then compared
		// against the same owner laid out by a forced resize.
		const auto check = [&](visage::Frame& owner, const char* which, const std::string& state, const std::vector<LaidOut>& before,
		                       int redraws) {
			const std::vector<LaidOut> gated = layoutOf(owner);
			forceResize(owner);
			const std::vector<LaidOut> resized = layoutOf(owner);
			if (const std::string diff = layoutDiff(gated, resized); !diff.empty()) {
				failures.push_back(tag + " " + which + " at " + state + ": the gate laid out what a resize does not:" + diff);
			}
			// THE OWNER'S OWN PICTURE: a gate that moved a control it draws around must have asked for its repaint.
			if (directChildMoved(before, gated) && redraws == 0) {
				failures.push_back(tag + " " + which + " at " + state + ": the gate moved its controls and the owner asked for no repaint");
			}
		};
		for (const size_t g : gates) {
			const float was = f.deviceHost.fieldAt(placed.device, g);
			for (const float v : gateValues(desc.params[g])) {
				char state[96];
				std::snprintf(state, sizeof state, "%s = %g", paramName(desc, g).c_str(), static_cast<double>(v));
				const std::vector<LaidOut> cardBefore = layoutOf(*placed.card);
				const std::vector<LaidOut> viewBefore = view != nullptr ? layoutOf(*view) : std::vector<LaidOut> {};
				int cardRedraws = 0;
				int viewRedraws = 0;
				{
					OwnRedraws cardProbe(placed.card);
					std::unique_ptr<OwnRedraws> viewProbe = view != nullptr ? std::make_unique<OwnRedraws>(view.get()) : nullptr;
					f.deviceHost.setField(placed.device, g, v);
					settle();
					cardRedraws = cardProbe.count;
					viewRedraws = viewProbe != nullptr ? viewProbe->count : 0;
				}
				++moves;
				check(*placed.card, "card", state, cardBefore, cardRedraws);
				if (view != nullptr) {
					check(*view, "window", state, viewBefore, viewRedraws);
				}
				settle();
			}
			f.deviceHost.setField(placed.device, g, was);
			settle();
		}
	}
	for (const std::string& failure : failures) {
		MESSAGE(failure);
	}
	CHECK_MESSAGE(failures.empty(), failures.size() << " gate move(s) whose relayout is not a resize's (above)");
	MESSAGE("gate moves: " << moves);
	CHECK(moves > 40);
}
