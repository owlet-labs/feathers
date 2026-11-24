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

// THE DAHDSR CARD. Lines 16675–30293 of the one file, moved verbatim 2026-09-03.

// SHARED BY EVERY PIECE OF THE dahdsr SUITE: the includes, and the helpers that more than one piece uses (each piece also holds the helpers only it uses).
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

// THE DAHDSR CARD. Lines 16675–30293 of the one file, moved verbatim 2026-09-03.

#include "fx/eq/eq_types.h"
#include "fx/owltt/owltt.h"
#include "plugin/ui/owltt_level_meter.h"
#include "plugin/ui/param_control.h"
#include "fx/drum/drum_types.h"
#include "plugin/ui/drum_device_card.h"
#include "plugin/ui/drum_device_expanded.h"
#include "plugin/ui/tooltip.h"
#include <cstdio>
#include <cstdlib>
#include "plugin/ui/delay_expanded.h"
#include "plugin/ui/eq_card.h"
#include "plugin/ui/noise_card.h"
#include "plugin/ui/eq_expanded.h"
#include "fx/featherverb/featherverb_types.h"
#include "fx/resonate/resonate_types.h"
#include "plugin/ui/menu_anchor.h"
#include "plugin/ui/settings_screen.h"
#include "plugin/ui/generic_expanded.h"
#include "mod/arp/pattern_types.h"
#include "plugin/ui/pattern_card.h"
#include "plugin/ui/eroder_card.h"
#include "plugin/ui/eroder_expanded.h"
#include "plugin/ui/balance_card.h"
#include "plugin/ui/balance_expanded.h"
#include "plugin/ui/smooth_card.h"
#include "plugin/ui/spectral_card.h"
#include "plugin/ui/spectral_expanded.h"
#include "plugin/ui/smooth_expanded.h"
#include "fx/inject/inject_types.h"
#include "plugin/ui/inject_card.h"
#include "fixtures.h"
#include "mod/lfo/lfo_types.h"
#include "mod/twinfield/twinfield_types.h"
#include "plugin/ui/twinfield_card.h"
#include "plugin/ui/lfo_card.h"
#include "plugin/ui/sklitter_card.h"
#include "plugin/ui/sklitter_expanded.h"
#include "plugin/ui/trig_pad.h"
#include "plugin/ui/mod_route_menu.h"
#include "mod/mseg/mseg_types.h"
#include "mod/rtrig/rtrig_types.h"
#include "mod/samplehold/samplehold_types.h"
#include "mod/stepseq/stepseq_types.h"
#include "plugin/ui/expanded_canvas.h"
#include "plugin/ui/spectrum_panel.h"
#include "rack/rack_axes.h"
#include "rack/rack_engine.h"
#include "param_routes.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-const-variable"

//------------------------------------------------------------------------------//
// The DAHDSR card — the layout it was asked for, and the curve that is the envelope //
//------------------------------------------------------------------------------//

/// **A SOURCE THAT MAY REACH ANY DEVICE CONTROL — a macro.**
///
/// A macro is the plugin's rather than a device's, so nothing can be reordered past it and it is
/// upstream of everything (the routing rule, 2026-09-05). Named because several cases below arm a
/// source only to prove something about a WIDGET — a bar's geometry, a repaint, an undo step, the Alt
/// gate — and used to arm the rack's own envelope at its own control, which the rule now refuses. The
/// SUBJECT of each of those cases is unchanged by which legal source is armed; a case that is about a
/// device source reaching another device belongs in test_mod_rack.cpp, where one now is.
inline backplane::Source armingSource() {
	return backplane::Source::core(1);
}

namespace {
/// **EVERY ROUTE, BY IDENTITY, IN ONE ORDER** — the whole-table comparison, over endpoints rather than
/// over a dense table's cells: two lists equal here are the same routes between the same devices.
std::vector<backplane::Route> routeListOf(const ParamBridge& bridge) {
	std::vector<backplane::Route> out;
	bridge.forEachRoute([&](const backplane::Route& r) { out.push_back(r); });
	std::sort(out.begin(), out.end(), [](const backplane::Route& a, const backplane::Route& b) {
		return a.source != b.source ? a.source < b.source : a.destination < b.destination;
	});
	return out;
}
/// HARM IN THE LANE, WITH ITS CARD — the fixture the cases below share.
///
/// IT LANDS IN SLOT 1 AND THAT IS THE POINT. The default chain already holds the filter, so every
/// assertion here is about a card that is not the first one — and HARM's controls are addressed by
/// (slot, control index), so a card that assumed slot 0 would silently edit the FILTER's row. A
/// single-tenant fixture cannot see that bug; this one fails on it.
struct HarmFixture : ModFixture {
	ui::FxLane* lane = nullptr;
	ui::HarmCard* card = nullptr;
	size_t slot = 0;
	/// HARM'S ID — what the card, the panel and every control address; `slot` is where the lane draws it.
	DeviceInstanceId device = kNoInstance;

	HarmFixture() {
		lane = findInTree<ui::FxLane>(root);
		REQUIRE(lane != nullptr);
		slot = bridge.addFxDevice(fx::harm::kTraits.tag);
		REQUIRE(slot == 1); // behind the filter the default chain already holds
		device = bridge.fxDeviceAt(slot);
		lane->resized();
		card = dynamic_cast<ui::HarmCard*>(lane->cardAt(slot));
		REQUIRE(card != nullptr);
	}

	/// WHAT THE DSP WILL SEE, THROUGH THE DSP'S OWN DECODER.
	///
	/// `fx::harm::stateOf` is literally the function `kAudioOps.setFields` calls on the audio thread,
	/// so a case reading through here cannot be green while the sample loop would receive something
	/// else. Restating the row's field order in the test would have been a second copy of exactly the
	/// mapping most likely to be wrong.
	fx::harm::State state(size_t which) const {
		const feathers::FxChain chain = bridge.fxChain();
		REQUIRE(which < chain.device.size());
		fx::harm::Row row {};
		const size_t fields = fromDeviceRow(fxDeviceAtPosition(chain, which).row, row);
		return fx::harm::stateOf(row, fields);
	}
	fx::harm::State state() const { return state(slot); }

	double param(fx::harm::Param p) const {
		return static_cast<double>(fx::harm::valueOf(state(), p));
	}

	static visage::Point centreOf(visage::Bounds b) {
		return { b.x() + b.width() * 0.5f, b.y() + b.height() * 0.5f };
	}
	/// The card's own handler, invoked directly — what the filter card's cases call `press`.
	void press(visage::Bounds part) {
		const visage::Point p = centreOf(part);
		card->onMouseDown().callback(at(p.x, p.y));
	}

	/// **THE FOLD GESTURE: count 1 then count 2.** `FxCard::foldOnHeadPress` acts only on the SECOND,
	/// so one press on a head asserts the ABSENCE of a fold — which is what stops a double-click
	/// toggling twice and landing where it started.
	void foldPress(visage::Bounds part) {
		const visage::Point p = centreOf(part);
		card->onMouseDown().callback(at(p.x, p.y));
		card->onMouseDown().callback(doubleAt(p.x, p.y));
		// AND THE RELEASE — see the sibling helper. The fold is on mouse-up now.
		card->onMouseUp().callback(doubleAt(p.x, p.y));
	}

	/// PRESS THE HARMONIC ROW, IN ITS OWN COORDINATES. The row is a CHILD FRAME since it became a
	/// widget both surfaces build, so its presses go to it directly and never reach the card's hit
	/// test. These cases used to press the card at the arrows' card-space rectangles; they press the
	/// same control at the same place, one frame down.
	void pressRow(visage::Bounds partInRow) {
		ui::HarmonicRow* row = expanded().harmonicRow();
		REQUIRE(row != nullptr);
		const visage::Point p = centreOf(partInRow);
		row->onMouseDown().callback(at(p.x, p.y));
	}
	visage::Bounds rowPrev() { return expanded().harmonicRow()->prevArrowBounds(); }
	visage::Bounds rowNext() { return expanded().harmonicRow()->nextArrowBounds(); }

	/// A KNOB ON THE CARD, by label.
	ui::Knob* cardKnob(const std::string& label) const {
		for (const auto& k : card->knobsForTest()) {
			if (k->label() != nullptr && std::string(k->label()) == label) {
				return k.get();
			}
		}
		return nullptr;
	}

	/// HARM'S EXPANDED VIEW, built on demand and kept, so a case can drive the panel's controls
	/// without opening the overlay. Built over the same bridge and the same device as the card.
	ui::HarmExpanded& expanded() {
		if (panel == nullptr) {
			panel = std::make_unique<ui::HarmExpanded>(deviceHost, panelRegistry, device);
			panel->setBounds(0, 0, panel->minimumSize().x, panel->minimumSize().y);
		}
		return *panel;
	}

	ui::Knob* panelKnob(const std::string& label) {
		for (const auto& k : expanded().knobsForTest()) {
			if (k->label() != nullptr && std::string(k->label()) == label) {
				return k.get();
			}
		}
		return nullptr;
	}

	/// A KNOB ON EITHER SURFACE — the card first, then the panel.
	///
	/// THE UNION, AND IT IS WHAT MOST CASES WANT. A case about a CONTROL — does a drag on Width write
	/// the row the engine decodes — is asking about the control and not about which surface happens
	/// to carry it today. The card reduction moved six knobs and every one of those cases would
	/// otherwise have had to be edited, which is how a suite gets narrowed by a refactor.
	///
	/// A case about a SURFACE still asks that surface: `cardKnob` and `panelKnob` are both here, and
	/// the layout cases use them.
	ui::Knob* knob(const std::string& label) {
		if (ui::Knob* onCard = cardKnob(label)) {
			return onCard;
		}
		return panelKnob(label);
	}

	ui::WidgetRegistry panelRegistry;
	std::unique_ptr<ui::HarmExpanded> panel;

	/// SCROLL THE LANE UNTIL THE WHOLE CARD IS IN IT, and call this before driving anything through
	/// the ROOT.
	///
	/// IT EXISTS BECAUSE A CASE PASSED FOR THE WRONG REASON AND THAT IS WORTH THE FIVE LINES. HARM's
	/// card is 330 tall behind a 356-tall filter in a 534-tall lane, so at scroll zero its bottom
	/// strip — the Poly switch — is off the end of the lane and `frameAtPoint` never finds it. The
	/// case that asserted "a click on the dead switch does not move the value" was therefore asking a
	/// control no hand could reach, and it would have gone on passing after the switch came alive.
	/// A probe aimed where the right and wrong answers agree is not a probe.
	void reveal() {
		lane->setScroll(lane->maxScroll());
		const visage::Bounds box = lane->cardBounds(slot);
		REQUIRE(box.y() >= -1.0f);
		REQUIRE(box.bottom() <= lane->height() + 1.0f);
	}

	/// A press delivered through the ROOT at a widget's own place, with the lane scrolled so the
	/// widget is actually there to be hit.
	void clickWidget(visage::Frame* widget, float fx, float fy) {
		REQUIRE(widget != nullptr);
		reveal();
		const visage::Point at = widget->positionInWindow()
		                         + visage::Point { widget->width() * fx, widget->height() * fy };
		// NOT VACUOUS: the hit test must actually land on the widget, or this is a click into space
		// and every assertion after it is about nothing.
		REQUIRE(root->frameAtPoint(at) == widget);
		dragThroughRoot(root, at, at);
	}
};
} // namespace

// THE CHAIN FURNITURE (backlog OW, 2026-10-29): HARM's Mix and wet Gain are the site panel's head row, supplied by the
// slot (test_gui_widgets_nest, "NR: the eight devices ..."). Neither the card nor the window draws them, by the NR
// decision that the custom cards are not touched, so the HARM sweeps below skip exactly these two and nothing else.
[[maybe_unused]] static bool harmSlotFurniture(size_t i) {
	return i == static_cast<size_t>(fx::harm::Param::Mix) || i == static_cast<size_t>(fx::harm::Param::WetGain);
}

namespace {
/// A CONTENT THAT IS NOTHING BUT ITS TWO ANSWERS, so the layer's cases are about the LAYER. A stub
/// that drew controls would let a failure in either half look like a failure in the other.
class StubExpandedView final : public ui::ExpandedView {
public:
	StubExpandedView(visage::Point minimum, std::string name)
	    : minimum_(minimum), name_(std::move(name)) {}

	visage::Point minimumSize() const override { return minimum_; }
	std::string title() const override { return name_; }

	/// So a case can prove the LAYER destroyed it, which is the claim `show` makes about ownership.
	static int alive;

	~StubExpandedView() override { --alive; }
	StubExpandedView(const StubExpandedView&) = delete;
	StubExpandedView& operator=(const StubExpandedView&) = delete;

private:
	visage::Point minimum_;
	std::string name_;
};
int StubExpandedView::alive = 0;
std::unique_ptr<StubExpandedView> makeStub(visage::Point minimum, std::string name) {
	++StubExpandedView::alive;
	return std::make_unique<StubExpandedView>(minimum, std::move(name));
}
} // namespace


#pragma GCC diagnostic pop
