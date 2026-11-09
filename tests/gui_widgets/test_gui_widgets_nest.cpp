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

// **NESTING PAST ONE LEVEL — backlog HG.** SKLITTER, a chain in its fx lane, a SKLITTER in that
// chain. Built exactly that way by hand on 2026-09-20, it took the whole plugin down on closing a
// window; the cases here are about the three levels rather than about any one widget, which is why
// they are their own binary. Everything at ONE level of nesting is pinned in
// `test_gui_widgets_devices.cpp`.

#include "fixtures.h"

#include "composite/composite_types.h"
#include "fx/sklitter/sklitter.h"
#include "fx/sklitter/sklitter_types.h"
#include "plugin/ui/chain_column.h"
#include "plugin/ui/expanded_views.h"
#include "plugin/ui/sklitter_expanded.h"
#include "plugin/ui/surface_layers.h"
#include "rack/chain_expanded.h"
#include "rack/inner_chain.h"
#include "rack/rack_bridge.h"
#include "rack/rack_device.h"
#include "rack/rack_editor.h"
#include "fx/filter/filter_types.h"
#include "fx/owltt/owltt_types.h"
#include "fx/pitch/pitch_types.h"
#include "fx/clip/clip_types.h"
#include "fx/drawn/drawn_types.h"
#include "fx/harm/harm_types.h"
#include "fx/noise/noise_types.h"
#include "fx/eq/eq_types.h"
#include "fx/disperser/disperser_types.h"
#include "fx/waypoint/waypoint_types.h"
#include "fx/busutil/busutil_types.h"
#include "mod/lfo/lfo_types.h"
#include "plugin/device_preset.h"
#include "plugin/preset_store.h"
#include "plugin/ui/preset_browser.h"
#include "rack/rack_axes.h"
#include "rack/rack_engine.h"
#include "rack/site_chains.h"
#include "rack/site_expanded.h"
#include "rack/parallel_rack.h"
#include "plugin/ui/parallel_rack_expanded.h"
#include "plugin/ui/route_list.h"
#include "plugin/ui/generic_card.h"
#include "plugin/ui/device_control.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

using namespace feathers;
namespace sc = feathers::fx::sklitter;
namespace cmp = feathers::composite;

namespace {

/// **A LIVE SKLITTER AND ITS ADOPTED CHAINS** — copied from `test_gui_widgets_devices.cpp`'s
/// `HeldChain` because that file's is in an anonymous namespace and this is a different binary. The
/// `adopt` is the whole of it: a container built and not adopted holds an empty catalogue and offers
/// nothing, with no error (bolt-on-chains trap T5).
struct HeldChain {
	sc::Sklitter device;
	HeldChain() {
		REQUIRE(sc::kDescriptor.container != nullptr);
		sc::kDescriptor.container->adopt(&device, feathers::hostCatalogue());
	}
	void* surface() { return &device; }
	rack::RackBridge& bridge() { return *device.postChain()->bridge(); }
};

/// How many frames of type T hang under `root` — the question "can a hand see it", asked of the
/// tree rather than of a flag. A panel presented onto layers nobody parented is unreachable from the
/// editor's root and this is what says so.
template <typename T>
size_t countInTree(visage::Frame* root) {
	size_t n = dynamic_cast<T*>(root) != nullptr ? 1u : 0u;
	for (visage::Frame* child : root->children()) {
		n += countInTree<T>(child);
	}
	return n;
}

/// **THE THREE LEVELS, BUILT THE WAY A HAND BUILDS THEM.** Declaration order is the fixture's
/// contract and not tidiness: `outer` must outlive everything presented onto it (the case in
/// `test_gui_widgets_devices.cpp` that gets this backwards SEGFAULTS, and says so).
struct NestedThree {
	HeldChain held; ///< the outer SKLITTER's own post chain — level 2
	ModFixture f;
	ui::WidgetRegistry registry;
	std::unique_ptr<ui::SurfaceLayers> outer; ///< L1: the editor's layers
	ui::SklitterExpanded* sklitterPanel = nullptr;
	ui::SurfaceId sklitterId = ui::kNoSurface;
	ui::ChainColumn* column = nullptr;   ///< the post rail's column — owns L2
	DeviceInstanceId chainDevice = kNoInstance;
	rack::ChainExpanded* chainPanel = nullptr;
	rack::RackEditor* innerEditor = nullptr; ///< owns L3
	DeviceInstanceId innerSklitter = kNoInstance;

	NestedThree() {
		rack::registerChainExpandedView();
		outer = std::make_unique<ui::SurfaceLayers>();
		outer->setBounds(0, 0, 1400, 900);

		// LEVEL 1 — a SKLITTER in Feathers' own fx lane, its panel presented on the editor's layers.
		const size_t slot = f.bridge.addFxDevice(sc::kTraits.tag);
		const DeviceInstanceId device = f.bridge.fxDeviceAt(slot);
		f.bridge.fxBodies().attach(device, held.surface());
		std::unique_ptr<ui::ExpandedView> view =
		    ui::makeExpandedView(f.deviceHost, &f.bridge, registry, device, sc::kDescriptor);
		REQUIRE(view != nullptr);
		sklitterPanel = dynamic_cast<ui::SklitterExpanded*>(view.get());
		REQUIRE(sklitterPanel != nullptr);
		sklitterPanel->postRailForTest().setCollapsed(false);
		sklitterId = outer->present(std::move(view), ui::SurfaceNeed::SpansGesture, "SKLITTER", {});
		REQUIRE(sklitterId != ui::kNoSurface);
		(void)outer->updateContent(); // the tick is what binds the rail to the live chain
		column = sklitterPanel->postRailForTest().columnForTest();
		REQUIRE(column != nullptr);
		REQUIRE(column->presentedOnForTest() == outer.get());

		// LEVEL 2 — a CHAIN device inside that SKLITTER, expanded. It escalates to the editor's
		// layers, which is the whole design: two panels on one host.
		chainDevice = column->addDevice(cmp::kTraits.tag);
		REQUIRE(chainDevice != kNoInstance);
		column->expandDevice(chainDevice);
		REQUIRE(column->expandedCountForTest() == 1);
		REQUIRE(outer->overlay().panelCount() == 2);
		chainPanel = findInTree<rack::ChainExpanded>(outer.get());
		REQUIRE(chainPanel != nullptr);
		(void)outer->updateContent(); // ChainExpanded::follow() builds the inner RackEditor here
		REQUIRE(chainPanel->liveForTest());
		innerEditor = chainPanel->editorForTest();
		REQUIRE(innerEditor != nullptr);

		// LEVEL 3 — a SKLITTER inside the chain. The crashing configuration exactly.
		innerSklitter = innerEditor->addDeviceByTag(sc::kTraits.tag);
		REQUIRE(innerSklitter != kNoInstance);
		innerEditor->syncCards();
	}
};

} // namespace

TEST_CASE("backlog HQ: SKLITTER in a chain in a SKLITTER — the third level's panel escalates all the "
          "way out, so a hand can see it") {
	// **A SKLITTER inside a rack inside a SKLITTER could not open its windowed editor.** The press
	// was not lost and the panel was not refused: it was BUILT, and mounted on a
	// `SurfaceLayers` that is deliberately not a frame.
	//
	// **THE MECHANISM, AND IT WAS ONE LINE OUT OF STEP WITH ITS OWN DOCUMENTATION.**
	// `SurfaceLayers::setOuterLayers` says a nested host *"presents its panels on the OUTERMOST
	// layers", and `kindAvailable` really does recurse (`outer_->kindFor(need)`). `presentImpl` did
	// not: it called `outer_->presentMounted(...)`, which mounts on the IMMEDIATE outer's own overlay
	// and never asked whether THAT host was itself nested. At one level the immediate outer IS the
	// outermost, so it was right everywhere it had been exercised. At two it was not, and the layer it
	// landed on was `ChainColumn`'s — whose constructor says in as many words *"NOT A CHILD FRAME. Its
	// two layers stay empty for this column's whole life — every present is forwarded".
	//
	// **THIS CASE USED TO PIN THE DEFECT** (same fixture, same press, the two assertions below
	// asserting that NOTHING changed) and was green while a hand could not open the panel. The fix
	// moves the forward into `presentMounted`, which is the road every hop takes, so escalation is the
	// same recursion `kindAvailable` already was.
	//
	// **ASSERTED AS A VALUE AND AS A TREE, because either alone is vacuous**: the editor says a panel
	// is up (so the press worked and this is not a refusal), and the tree under the OUTERMOST host has
	// gained that panel (so a hand can reach it). The tree walk is the half that matters — a panel
	// mounted on an unparented host would satisfy the count on some host somewhere.
	NestedThree n;
	const size_t panelsBefore = n.outer->overlay().panelCount();
	const size_t sklittersBefore = countInTree<ui::SklitterExpanded>(n.outer.get());
	REQUIRE(panelsBefore == 2);
	REQUIRE(sklittersBefore == 1); // the outer SKLITTER's own panel, and only it

	ui::FxCard* card = n.innerEditor->cardOfDeviceForTest(n.innerSklitter);
	REQUIRE(card != nullptr);
	// THE MARK IS OFFERED — the inner editor has a presenter, so this is a control that says it works.
	CHECK(card->expandBounds().width() > 0.0f);
	REQUIRE(card->onExpand != nullptr);
	card->onExpand();

	// THE PRESS TOOK: the inner editor holds a surface id and believes a panel is up.
	CHECK(n.innerEditor->expandedCountForTest() == 1);
	// AND IT REACHED THE OUTERMOST HOST — the two lines now agree, and the second is the one a hand
	// sees.
	CHECK(n.outer->overlay().panelCount() == panelsBefore + 1);
	CHECK(countInTree<ui::SklitterExpanded>(n.outer.get()) == sklittersBefore + 1);
	// NOT ON THE INTERMEDIATE HOST, which is the thing that was wrong: the column's own layers stay
	// empty for its whole life, as its constructor promises.
	CHECK(n.column->presentedOnForTest() == n.outer.get());

	// AND THE WAY BACK DOWN WORKS AT TWO HOPS TOO — the id the inner editor holds is one the OUTERMOST
	// host issued, so a dismissal has to walk out through every level that forwarded it.
	const ui::SurfaceId inner = n.innerEditor->expandedIdForTest();
	REQUIRE(inner != ui::kNoSurface);
	REQUIRE(n.innerEditor->surfaceLayersForTest().holds(inner));
	n.innerEditor->surfaceLayersForTest().dismiss(inner);
	CHECK(n.outer->overlay().panelCount() == panelsBefore);
	CHECK(countInTree<ui::SklitterExpanded>(n.outer.get()) == sklittersBefore);
}

TEST_CASE("expand toggle, nested: a chain column's and a nested rack editor's expand marks close their "
          "device's panel on the outermost host, leave the others up, and reopen it where it was left") {
	// Clicking a card's window-expand mark while its window is already expanded closes it. Both
	// nested hosts escalate their panels to the OUTERMOST layers, so the close has to
	// walk out the same way — and has to match on the CHAIN as well as the id, because ids repeat
	// across chains: a press inside the chain must never close the outer namesake.
	NestedThree n;
	// ROOM FOR A FOURTH PANEL CLEAR OF THE OTHERS, so the placement half below is not decided by the dodge.
	n.outer->setBounds(0, 0, 3200, 2000);
	ui::ExpandedOverlay& overlay = n.outer->overlay();
	REQUIRE(overlay.width() == doctest::Approx(3200.0f));
	const auto panelOf = [&overlay](DeviceInstanceId device, ui::DeviceNest nest) {
		for (const ui::ExpandedOverlay::PanelId id : overlay.panelIdsForTest()) {
			const ui::ExpandedView* content = overlay.contentForTest(id);
			if (content != nullptr && content->watchedDevice() == device && content->nest() == nest) {
				return id;
			}
		}
		return ui::ExpandedOverlay::kNoPanel;
	};
	const auto open = [&](DeviceInstanceId device, ui::DeviceNest nest) {
		return panelOf(device, nest) != ui::ExpandedOverlay::kNoPanel;
	};
	const ui::DeviceNest columnNest = n.column->nest();
	const ui::DeviceNest editorNest = n.innerEditor->nest();
	REQUIRE(overlay.panelCount() == 2); // the outer SKLITTER's and the chain's
	REQUIRE(open(n.chainDevice, columnNest));

	// LEVEL 3, THE NESTED RACK EDITOR: open, close, open.
	ui::FxCard* card = n.innerEditor->cardOfDeviceForTest(n.innerSklitter);
	REQUIRE(card != nullptr);
	REQUIRE(card->onExpand != nullptr);
	card->onExpand();
	CHECK(open(n.innerSklitter, editorNest));
	CHECK(overlay.panelCount() == 3);
	card->onExpand();
	CHECK_FALSE(open(n.innerSklitter, editorNest));
	CHECK(n.innerEditor->expandedCountForTest() == 0);
	CHECK(overlay.panelCount() == 2);
	CHECK(open(n.chainDevice, columnNest)); // the others stay up
	(void)n.outer->updateContent();
	card->onExpand();
	CHECK(open(n.innerSklitter, editorNest));

	// AND ITS PLACEMENT IS KEPT across the mark's close.
	{
		const ui::ExpandedOverlay::PanelId first = panelOf(n.innerSklitter, editorNest);
		REQUIRE(first == overlay.frontPanelIdForTest()); // the drag below moves the front panel
		const visage::Bounds opened = overlay.panelBounds(first);
		// INTO THE FAR CORNER, CLEAR OF THE OTHER TWO PANELS: a remembered place that covers another
		// panel is dodged off it on reopen (`show`), which is the layer's rule and not this case's.
		overlay.beginPanelDrag();
		overlay.dragPanelBy({ 9000.0f, 9000.0f });
		const visage::Bounds moved = overlay.panelBounds(first);
		REQUIRE((moved.x() != opened.x() || moved.y() != opened.y()));
		for (const ui::ExpandedOverlay::PanelId other : overlay.panelIdsForTest()) {
			if (other != first) {
				REQUIRE_FALSE(overlay.panelBounds(other).overlaps(moved));
			}
		}
		card->onExpand();
		REQUIRE_FALSE(open(n.innerSklitter, editorNest));
		(void)n.outer->updateContent();
		card->onExpand();
		const ui::ExpandedOverlay::PanelId again = panelOf(n.innerSklitter, editorNest);
		REQUIRE(again != ui::ExpandedOverlay::kNoPanel);
		CHECK(overlay.panelBounds(again).x() == doctest::Approx(moved.x()));
		CHECK(overlay.panelBounds(again).y() == doctest::Approx(moved.y()));
	}
	card->onExpand();
	REQUIRE(overlay.panelCount() == 2);
	(void)n.outer->updateContent();

	// LEVEL 2, THE CHAIN COLUMN: a second device beside the chain, open, close, open.
	const DeviceInstanceId filter = n.column->addDevice(feathers::fx::filter::kTraits.tag);
	REQUIRE(filter != kNoInstance);
	n.column->expandDevice(filter);
	CHECK(open(filter, columnNest));
	CHECK(n.column->expandedCountForTest() == 2);
	n.column->expandDevice(filter);
	CHECK_FALSE(open(filter, columnNest));
	CHECK(n.column->expandedCountForTest() == 1);
	CHECK(open(n.chainDevice, columnNest)); // the chain's panel stays up
	CHECK(overlay.panelCount() == 2);
	(void)n.outer->updateContent();
	n.column->expandDevice(filter);
	CHECK(open(filter, columnNest));
	CHECK(overlay.panelCount() == 3);

	// AND THE CHAIN DEVICE'S OWN MARK CLOSES THE CHAIN'S PANEL, leaving the filter's.
	n.chainPanel = nullptr;
	n.innerEditor = nullptr;
	n.column->expandDevice(n.chainDevice);
	CHECK_FALSE(open(n.chainDevice, columnNest));
	CHECK(open(filter, columnNest));
	CHECK(overlay.panelCount() == 2);
	(void)n.outer->updateContent();
}

TEST_CASE("backlog HG: closing the OUTER SKLITTER panel with a chain panel open under it tears the "
          "hosts down inside out") {
	// **THE CRASH: closing a SKLITTER with a rack in its fx lane and a SKLITTER in the rack took down
	// the whole plugin.** This case drives the ✕ on the outermost
	// panel and then the tick that frees what it retired — the editor's own two steps, nothing more.
	//
	// **WHY THE FREE IS A SECOND STEP AND WHY THAT IS THE WHOLE BUG.** `ExpandedOverlay` retires a
	// dismissed panel rather than destroying it (*"this runs from a mouse handler ... deleting a frame
	// out from under that walk is how a SIGSEGV lands"), and frees it at the next `draw`, `show` or
	// tick. So closing the SKLITTER panel destroys its `ChainColumn` — and with it the column's
	// `SurfaceLayers` — DURING a `retired_.clear()`, while that same clear() is the thing that will
	// next have to free the chain panel the column just asked to be taken down.
	//
	// **THIS IS NOT BACKLOG GR'S FAULT, AND THE DIFFERENCE MATTERS.** GR filed a `SurfaceLayers`
	// destroyed before the panel it hosts, and said the editor could not reach it. This crash gets
	// nowhere near that: it is `retired_`'s own vector, corrupted by a `push_back` that happens
	// INSIDE its `clear()` — see the minimal case below, which has no chain, no third level, and no
	// second host at all. GR's order may still be reachable at three levels, but it cannot be
	// observed until this is fixed, because this fires first in the same call.
	//
	// **THE ASSERTION IS SURVIVAL, WHICH IS WEAK, SO THE SANITIZER GATE IS THE OTHER HALF** — reading
	// freed memory does not fault, and `ctest` structurally cannot see this class (CLAUDE.md).
	NestedThree n;
	ui::FxCard* card = n.innerEditor->cardOfDeviceForTest(n.innerSklitter);
	REQUIRE(card != nullptr);
	REQUIRE(card->onExpand != nullptr);
	card->onExpand(); // the third level's panel, up on the column's own layers
	REQUIRE(n.innerEditor->expandedCountForTest() == 1);

	// THE ✕ ON THE OUTERMOST PANEL — the one a hand presses, by its own id.
	const ui::ExpandedOverlay::PanelId outermost = n.outer->overlay().panelIdsForTest().front();
	n.outer->overlay().dismiss(outermost);
	// AND THE TICK THAT FREES IT. Everything below happens inside this one call.
	(void)n.outer->updateContent();
	(void)n.outer->updateContent();

	n.sklitterPanel = nullptr;
	n.column = nullptr;
	n.chainPanel = nullptr;
	n.innerEditor = nullptr;
	CHECK(n.outer->overlay().panelCount() <= 1);
}

TEST_CASE("backlog HG: the outermost host dies holding all three levels") {
	// THE OTHER ORDER, AND THE ONE `test_gui_widgets_devices.cpp` PINS AT ONE LEVEL — the editor going
	// away with everything open. Here with three levels rather than two.
	NestedThree n;
	ui::FxCard* card = n.innerEditor->cardOfDeviceForTest(n.innerSklitter);
	REQUIRE(card != nullptr);
	REQUIRE(card->onExpand != nullptr);
	card->onExpand();
	n.sklitterPanel = nullptr;
	n.column = nullptr;
	n.chainPanel = nullptr;
	n.innerEditor = nullptr;
	n.outer.reset(); // ...and the layers go, holding the lot
	CHECK(true);
}

TEST_CASE("backlog HG: the same close with only TWO levels up crashes as well — the third level is "
          "NOT what the crash needs") {
	// **THIS CASE WAS WRITTEN AS A CONTROL AND IT CAME BACK RED, WHICH IS THE FINDING.** It was meant
	// to prove the three-level configuration was load-bearing: same fixture, same ✕, same tick, with
	// the inner SKLITTER's panel never opened. It crashes identically, so the brief's hypothesis —
	// that the reported SKLITTER → chain → SKLITTER is what reaches this — is false. The nesting only led
	// to a *second* panel open at once, and a second panel is the whole requirement.
	//
	// It is kept rather than deleted because a control that fails is evidence, and because it is the
	// rung between the three-level case above and the minimal one below.
	NestedThree n;
	REQUIRE(n.innerEditor->expandedCountForTest() == 0);
	const ui::ExpandedOverlay::PanelId outermost = n.outer->overlay().panelIdsForTest().front();
	n.outer->overlay().dismiss(outermost);
	(void)n.outer->updateContent();
	(void)n.outer->updateContent();
	n.sklitterPanel = nullptr;
	n.column = nullptr;
	n.chainPanel = nullptr;
	n.innerEditor = nullptr;
	CHECK(n.outer->overlay().panelCount() <= 1);
}

namespace {

/// **THE SMALLEST THING THAT CRASHES** — one SKLITTER panel and one ORDINARY device's panel opened
/// from inside it. No chain, no third level. Built separately from `NestedThree` so the minimal case
/// cannot accidentally inherit anything the three-level one arranges.
struct TwoLevels {
	HeldChain held;
	ModFixture f;
	ui::WidgetRegistry registry;
	std::unique_ptr<ui::SurfaceLayers> outer;
	ui::SklitterExpanded* sklitterPanel = nullptr;
	ui::ChainColumn* column = nullptr;

	explicit TwoLevels(const char* innerTag) {
		rack::registerChainExpandedView();
		outer = std::make_unique<ui::SurfaceLayers>();
		outer->setBounds(0, 0, 1400, 900);
		const size_t slot = f.bridge.addFxDevice(sc::kTraits.tag);
		const DeviceInstanceId device = f.bridge.fxDeviceAt(slot);
		f.bridge.fxBodies().attach(device, held.surface());
		std::unique_ptr<ui::ExpandedView> view =
		    ui::makeExpandedView(f.deviceHost, &f.bridge, registry, device, sc::kDescriptor);
		REQUIRE(view != nullptr);
		sklitterPanel = dynamic_cast<ui::SklitterExpanded*>(view.get());
		REQUIRE(sklitterPanel != nullptr);
		sklitterPanel->postRailForTest().setCollapsed(false);
		REQUIRE(outer->present(std::move(view), ui::SurfaceNeed::SpansGesture, "SKLITTER", {})
		        != ui::kNoSurface);
		(void)outer->updateContent();
		column = sklitterPanel->postRailForTest().columnForTest();
		REQUIRE(column != nullptr);
		if (innerTag != nullptr) {
			const DeviceInstanceId inner = column->addDevice(innerTag);
			REQUIRE(inner != kNoInstance);
			column->expandDevice(inner);
			REQUIRE(outer->overlay().panelCount() == 2);
		}
	}

	/// THE ✕ ON THE OUTERMOST PANEL, THEN THE TICK THAT FREES WHAT IT RETIRED — the editor's own two
	/// steps and nothing else.
	void closeOutermostAndTick() {
		const ui::ExpandedOverlay::PanelId outermost = outer->overlay().panelIdsForTest().front();
		outer->overlay().dismiss(outermost);
		sklitterPanel = nullptr;
		column = nullptr;
		(void)outer->updateContent();
		(void)outer->updateContent();
	}
};

} // namespace

TEST_CASE("backlog HG control: closing a SKLITTER panel with NOTHING open from inside it is safe") {
	// The base of the ladder: the panel goes down and is freed, and nothing nested is involved.
	TwoLevels t { nullptr };
	REQUIRE(t.outer->overlay().panelCount() == 1);
	t.closeOutermostAndTick();
	CHECK(t.outer->overlay().panelCount() == 0);
}

TEST_CASE("backlog HG: closing a SKLITTER panel while a panel it opened is still up — the MINIMAL "
          "crash, with no chain and no third level") {
	// **THIS IS THE DEFECT, AND IT IS TWO LEVELS RATHER THAN THREE.** The brief's hypothesis was that
	// the reported SKLITTER → chain → SKLITTER was needed; it is not. Any device whose panel was opened from
	// inside the SKLITTER panel does it, because what matters is the ORDER the two are freed in and
	// not what they are.
	//
	// **THE ROAD.** `ExpandedOverlay::updateContent` opens with `retired_.clear()`. That clear destroys
	// the retired SKLITTER panel → its `FxLaneRail` → its `ChainColumn`, whose destructor dismisses the
	// nested panel it presented — which lands back in `ExpandedOverlay::dismissBy` on the SAME overlay
	// and does `retired_.push_back(...)` TWICE, into the vector that `clear()` is at that moment part
	// way through destroying. libstdc++'s `clear()` destroys [begin, finish) with `_M_finish` still at
	// the old end, so a `push_back` inside that walk either reallocates the buffer the walk is reading
	// or writes past the elements it is about to skip.
	//
	// **THE ASSERTION IS SURVIVAL, WHICH IS WEAK — the sanitizer gate is the other half.**
	const char* tag = nullptr;
	{
		TwoLevels probe { nullptr };
		for (const device::Descriptor* d : probe.column->addable()) {
			if (ui::hasExpandedView(*d) && !device::sameTag(d->traits.tag, cmp::kTraits.tag)) {
				tag = d->traits.tag;
				break;
			}
		}
	}
	REQUIRE(tag != nullptr);
	// NOT A CONTAINER, so nothing here is about nesting a chain: an ordinary device, one panel deep.
	TwoLevels t { tag };
	t.closeOutermostAndTick();
	CHECK(t.outer->overlay().panelCount() <= 1);
}

TEST_CASE("backlog HG control: closing the INNER panel first and the outer second is safe — the "
          "order is the whole of it") {
	// **THE SAME TWO PANELS, THE OTHER WAY ROUND.** This is what localises the fault to the column's
	// destructor re-entering the overlay it is being freed BY: close the nested panel while the column
	// is alive and the dismissal runs at an ordinary moment, so by the time the SKLITTER panel is
	// freed its column has nothing left to take down.
	//
	// It is also the workaround a hand has today.
	const char* tag = nullptr;
	{
		TwoLevels probe { nullptr };
		for (const device::Descriptor* d : probe.column->addable()) {
			if (ui::hasExpandedView(*d) && !device::sameTag(d->traits.tag, cmp::kTraits.tag)) {
				tag = d->traits.tag;
				break;
			}
		}
	}
	REQUIRE(tag != nullptr);
	TwoLevels t { tag };
	const std::vector<ui::ExpandedOverlay::PanelId> ids = t.outer->overlay().panelIdsForTest();
	REQUIRE(ids.size() == 2);
	t.outer->overlay().dismiss(ids.back()); // the nested one
	(void)t.outer->updateContent();
	CHECK(t.column->expandedCountForTest() == 0);
	t.closeOutermostAndTick();
	CHECK(t.outer->overlay().panelCount() == 0);
}

TEST_CASE("backlog HL: the editor closes with a panel already retired and not yet reaped — the "
          "DESTRUCTOR road, which the three explicit reaps do not cover") {
	// **A SECOND INSTANCE OF HG'S BUG, IN THE SAME VECTOR, FOUND WHILE FIXING THE FIRST.** The three
	// reaps that HG's road runs through — `show`, `updateContent`, `draw` — are not the only places
	// `retired_` is emptied. `ExpandedOverlay` HAD NO DESTRUCTOR, so the fourth emptying was
	// `~vector`'s own, during member teardown, and `~vector` has the identical property `clear()` has:
	// it destroys `[_M_start, _M_finish)` with `_M_finish` at the old end.
	//
	// **WHY THE EXISTING CASE ABOVE DOES NOT SEE IT, AND THE REASON IS LUCK RATHER THAN SAFETY.**
	// *"the outermost host dies holding all three levels" reaches the same teardown and is green,
	// because nothing was dismissed first: `retired_` is EMPTY there, and an empty vector's destructor
	// deallocates nothing, so the re-entrant `push_back` merely allocates a buffer nobody frees. A
	// leak, invisible under `detect_leaks=0`. Park one frame first and the same road is a
	// use-after-free — so the difference between the two cases is one `dismiss`, and the green one
	// says nothing about the red one.
	//
	// **THE SEQUENCE IS ONE A HAND MAKES**: press the ✕ on a panel and close the plugin window before
	// the next frame is drawn. There is no tick between them here for exactly that reason.
	const char* tag = nullptr;
	{
		TwoLevels probe { nullptr };
		for (const device::Descriptor* d : probe.column->addable()) {
			if (ui::hasExpandedView(*d) && !device::sameTag(d->traits.tag, cmp::kTraits.tag)) {
				tag = d->traits.tag;
				break;
			}
		}
	}
	REQUIRE(tag != nullptr);
	TwoLevels t { tag };
	REQUIRE(t.outer->overlay().panelCount() == 2);

	// THE ✕ ON THE OUTERMOST PANEL. Its two frames are now PARKED — not destroyed, and not yet reaped.
	const ui::ExpandedOverlay::PanelId outermost = t.outer->overlay().panelIdsForTest().front();
	t.outer->overlay().dismiss(outermost);
	t.sklitterPanel = nullptr;
	t.column = nullptr;

	// AND THE EDITOR GOES, WITH NO TICK IN BETWEEN. Everything below happens inside this one line:
	// the park list destroys the SKLITTER panel, whose `ChainColumn` dismisses the nested panel it
	// presented, which lands back in `dismissBy` and pushes into the vector being destroyed.
	t.outer.reset();
	// THE ASSERTION IS SURVIVAL, WHICH IS WEAK, SO THE SANITIZER GATE IS THE OTHER HALF — the same
	// caveat HG's cases carry, and for the same reason: reading freed memory does not fault.
	CHECK(true);
}

TEST_CASE("backlog HQ: an outer host that DIES holding a nested host's forwarded mount unlists it "
          "first — asserted as a value, because survival is not one") {
	// **THE THIRD OF THE THREE REPRODUCTIONS, REDUCED TO THE TWO OBJECTS IT IS ABOUT.** The ASan
	// trace on the HG case above reads:
	//
	//     heap-use-after-free  widget_registry.h:240  forgetFrameTree
	//     from                 surface_layers.cpp     dismiss(), the forwarded_ branch
	//     via                  ~RackEditor -> ~ChainExpanded -> ~MountedFrame
	//
	// freed by `~ExpandedOverlay` -> `~vector<Slot>` inside `~SurfaceLayers` of the `ChainColumn`'s
	// host. **THAT TEARDOWN DESTROYS AND DOES NOT DISMISS**, so `onDismissPanel` -> `panelWentDown` —
	// the only thing that unlists a `forwarded_` entry — structurally cannot run, and the inner host
	// is left holding a pointer to a frame that is about to be freed. One tick later its own
	// destructor walks that frame's children.
	//
	// **WHY THIS CASE EXISTS BESIDE THE THREE-LEVEL ONE.** Those assert survival, which passes for a
	// host that merely got lucky about allocation order — and this repo's rule is that a claim about a
	// value is asserted as a value. Here the inner host is still alive when the outer has gone, so
	// both numbers can be read: the forwarded entry is DROPPED and the outer pointer is NULLED. The
	// second is not bookkeeping — `dismiss` follows a forwarded id with `outer_->dismiss(id)`, which
	// is a second use-after-free on the same road.
	//
	// KNOWN-BAD: delete the `outerLayersGoingAway()` loop from `~SurfaceLayers` and this reads 1 and
	// non-null; ASan then reports the free on the three-level case above.
	ui::SurfaceLayers inner;
	{
		ui::SurfaceLayers outer;
		outer.setBounds(0, 0, 1400, 900);
		inner.setBounds(0, 0, 1400, 900);
		inner.setOuterLayers(&outer);
		CHECK(outer.innerCountForTest() == 1);

		// A SPANNING PRESENT ON THE INNER HOST, WHICH FORWARDS IT. The content is a bare frame: this
		// case is about the mount's lifetime and not about what was mounted.
		const ui::SurfaceId id =
		    inner.present(std::make_unique<visage::Frame>(), ui::SurfaceNeed::SpansGesture, "NESTED", {});
		REQUIRE(id != ui::kNoSurface);
		REQUIRE(inner.forwardedCountForTest() == 1);
		REQUIRE(outer.overlay().panelCount() == 1);
		REQUIRE(inner.holds(id));
		// ...AND NOTHING WAS DISMISSED, so the ordinary road is not what clears this.
		REQUIRE(inner.presentedCountForTest() == 0);
	}
	// THE OUTER IS GONE. Both readings are of the inner host, which is alive.
	CHECK(inner.forwardedCountForTest() == 0);
	CHECK(inner.outerLayers() == nullptr);
}

TEST_CASE("backlog HQ control: the ORDINARY dismissal road still unlists a forwarded entry — the new "
          "road is an addition and not a replacement") {
	// **THE LOAD-BEARING REDUNDANCY, PINNED.** A peer went 7 green to 4 tonight by giving the
	// overlay's slots a swap-and-destroy that made `dismissBy` a no-op, which skipped
	// `onDismissPanel` -> `panelWentDown` — the road this case is about. It looks redundant beside
	// `outerLayersGoingAway()` and it is not: it is what runs when a panel is taken DOWN while both
	// hosts live, and nothing in HQ's fix can reach that moment.
	ui::SurfaceLayers outer;
	ui::SurfaceLayers inner;
	outer.setBounds(0, 0, 1400, 900);
	inner.setBounds(0, 0, 1400, 900);
	inner.setOuterLayers(&outer);
	const ui::SurfaceId id =
	    inner.present(std::make_unique<visage::Frame>(), ui::SurfaceNeed::SpansGesture, "NESTED", {});
	REQUIRE(id != ui::kNoSurface);
	REQUIRE(inner.forwardedCountForTest() == 1);

	// THE ✕ ON THE OUTER'S PANEL, with both hosts alive — `dismissBy`'s road, not the destructor's.
	outer.overlay().dismiss(outer.overlay().panelIdsForTest().front());
	CHECK(inner.forwardedCountForTest() == 0);
	CHECK_FALSE(inner.holds(id));
	// AND THE OUTER IS STILL THE OUTER: a dismissal is not a divorce.
	CHECK(inner.outerLayers() == &outer);
	CHECK(outer.innerCountForTest() == 1);
}

//--------------------------------------------------------------------------------------------------//
// SKLITTER PRESETS — backlog FO. In this binary because the claim is "wherever SKLITTER lives": in      //
// Feathers' fx lane, in a rack, and in a chain inside a SKLITTER — the three hosts this file builds.  //
//--------------------------------------------------------------------------------------------------//

namespace {

/// **EVERY CONTROL SOMEWHERE ITS DEFAULT IS NOT, AND NO TWO ALIKE WHERE THE RANGE ALLOWS.** A round trip
/// of defaults is a round trip of nothing: a codec that wrote no fields at all would pass it.
void setDistinctRow(backplane::DeviceHost& host, DeviceInstanceId id, const device::Descriptor& d) {
	for (size_t i = 0; i < d.params.size(); ++i) {
		const device::ParamDesc& p = d.params[i];
		const double span = p.max - p.min;
		double v;
		if (p.stepped) {
			v = p.def + 1.0 <= p.max ? p.def + 1.0 : p.def - 1.0;
		}
		else {
			v = p.min + span * (0.29 + 0.06 * static_cast<double>(i % 7));
			if (std::abs(v - p.def) < span * 0.01) {
				v = p.min + span * 0.81;
			}
		}
		host.setField(id, i, static_cast<float>(v));
	}
}

std::vector<float> rowOf(const backplane::DeviceHost& host, DeviceInstanceId id, const device::Descriptor& d) {
	std::vector<float> row(d.params.size());
	for (size_t i = 0; i < row.size(); ++i) {
		row[i] = host.fieldAt(id, i);
	}
	return row;
}

/// ONE LANE, AS VALUES: which devices, in which order, set to what.
struct LaneShot {
	std::vector<std::string> tags;
	std::vector<std::vector<float>> fields;
	friend bool operator==(const LaneShot&, const LaneShot&) = default;
};

LaneShot shotOf(const rack::RackBridge& lane) {
	LaneShot shot;
	for (size_t pos = 0; pos < lane.chainLength(0); ++pos) {
		const DeviceInstanceId member = lane.deviceAtPosition(pos);
		const device::Descriptor* d = lane.descriptorOf(member);
		REQUIRE(d != nullptr);
		shot.tags.emplace_back(d->traits.tag);
		shot.fields.push_back(rowOf(lane, member, *d));
	}
	return shot;
}

bool kept(size_t control) {
	return control == static_cast<size_t>(sc::Param::Enabled) || control == static_cast<size_t>(sc::Param::Engage);
}

/// **THE ROUND TRIP, ASKED OF WHATEVER HOST HOLDS THE DEVICE.** Fill both lanes, set every control, save;
/// load Init and see it all go; load the save and see it all come back — every assertion a value read off
/// the host or a lane's bridge, never a render.
void roundTripIn(backplane::DeviceHost& host, DeviceInstanceId id) {
	const device::Descriptor& d = sc::kDescriptor;
	void* surface = host.containerSurfaceOf(id);
	REQUIRE(surface != nullptr);
	REQUIRE(sc::postChainOf(surface) != nullptr);
	REQUIRE(sc::grainChainOf(surface) != nullptr);
	rack::RackBridge* post = sc::postChainOf(surface)->bridge();
	rack::RackBridge* grain = sc::grainChainOf(surface)->bridge();
	REQUIRE(post != nullptr);
	REQUIRE(grain != nullptr);

	// BOTH LANES HOLD SOMETHING SET AWAY FROM ITS DEFAULT — two devices in the post lane, so ORDER is part
	// of the claim, and one in the grain lane, so the grain lane is.
	const DeviceInstanceId p1 = post->addDevice(fx::filter::kTraits.tag);
	const DeviceInstanceId p2 = post->addDevice(fx::owltt::kTraits.tag);
	const DeviceInstanceId g1 = grain->addDevice(fx::filter::kTraits.tag);
	REQUIRE(p1 != kNoInstance);
	REQUIRE(p2 != kNoInstance);
	REQUIRE(g1 != kNoInstance);
	setDistinctRow(*post, p1, *post->descriptorOf(p1));
	setDistinctRow(*post, p2, *post->descriptorOf(p2));
	setDistinctRow(*grain, g1, *grain->descriptorOf(g1));
	setDistinctRow(host, id, d);

	const std::vector<float> saved = rowOf(host, id, d);
	const LaneShot postSaved = shotOf(*post);
	const LaneShot grainSaved = shotOf(*grain);
	REQUIRE(postSaved.tags.size() == 2);
	REQUIRE(grainSaved.tags.size() == 1);
	size_t moved = 0;
	for (size_t i = 0; i < d.params.size(); ++i) {
		moved += saved[i] != static_cast<float>(d.params[i].def) ? 1u : 0u;
	}
	CHECK(moved == d.params.size()); // not vacuous: every control is off its default

	std::vector<uint8_t> bytes;
	REQUIRE(captureDevicePreset(host, d, id, &bytes));
	// WHAT WAS SAVED, AS VALUES — the row exactly, and a body.
	DevicePresetContents contents;
	rack::LoadReport report;
	REQUIRE(readDevicePreset(bytes, &contents, &d, &report));
	CHECK(report.empty()); // saved and read by one build: nothing missing, unknown or corrected
	CHECK(contents.tag == d.traits.tag);
	REQUIRE(contents.field.size() >= d.params.size());
	for (size_t i = 0; i < d.params.size(); ++i) {
		CHECK(contents.field[i] == saved[i]);
	}
	CHECK_FALSE(contents.body.empty());

	// INIT: every control that is not kept goes to its default and both lanes empty; the two kept ones
	// stay exactly where the hand left them.
	REQUIRE(applyDevicePreset(host, d, id, initDevicePreset(d), ui::SklitterExpanded::kPresetKeeps)
	        == DevicePresetLoad::Loaded);
	for (size_t i = 0; i < d.params.size(); ++i) {
		if (kept(i)) {
			CHECK(host.fieldAt(id, i) == saved[i]);
		}
		else {
			CHECK(host.fieldAt(id, i) == device::coerceControl(d.params[i], static_cast<float>(d.params[i].def)));
		}
	}
	CHECK(post->chainLength(0) == 0);
	CHECK(grain->chainLength(0) == 0);

	// AND BACK: every control, both lanes, device for device and field for field.
	REQUIRE(applyDevicePreset(host, d, id, bytes, ui::SklitterExpanded::kPresetKeeps) == DevicePresetLoad::Loaded);
	for (size_t i = 0; i < d.params.size(); ++i) {
		INFO("control " << i << " (" << d.params[i].name << ")");
		CHECK(host.fieldAt(id, i) == saved[i]);
	}
	CHECK(shotOf(*post) == postSaved);
	CHECK(shotOf(*grain) == grainSaved);
}

} // namespace

TEST_CASE("backlog FO: a SKLITTER preset round-trips every control and both lanes, in a RACK") {
	rack::RackEngine engine { rack::kRackAxes, hostCatalogue() };
	engine.prepare(48000.0, 4096);
	rack::RackBridge bridge(engine);
	bridge.prepare(48000.0, 4096);
	const DeviceInstanceId id = bridge.addDevice(sc::kTraits.tag);
	REQUIRE(id != kNoInstance);
	roundTripIn(bridge, id);
}

TEST_CASE("backlog FO: a SKLITTER preset round-trips every control and both lanes, in FEATHERS' fx lane") {
	HeldChain held;
	ModFixture f;
	const size_t slot = f.bridge.addFxDevice(sc::kTraits.tag);
	const DeviceInstanceId id = f.bridge.fxDeviceAt(slot);
	f.bridge.fxBodies().attach(id, held.surface());
	roundTripIn(f.deviceHost, id);
}

TEST_CASE("backlog FO: a SKLITTER preset round-trips every control and both lanes, NESTED — in a chain "
          "inside a SKLITTER") {
	NestedThree n;
	rack::RackBridge* inner = rack::bridgeOf(n.held.bridge().containerSurfaceOf(n.chainDevice));
	REQUIRE(inner != nullptr);
	roundTripIn(*inner, n.innerSklitter);
}

TEST_CASE("backlog FO: a preset of another device, or bytes that are not one, are refused and change "
          "nothing") {
	rack::RackEngine engine { rack::kRackAxes, hostCatalogue() };
	engine.prepare(48000.0, 4096);
	rack::RackBridge bridge(engine);
	bridge.prepare(48000.0, 4096);
	const DeviceInstanceId id = bridge.addDevice(sc::kTraits.tag);
	REQUIRE(id != kNoInstance);
	setDistinctRow(bridge, id, sc::kDescriptor);
	const std::vector<float> before = rowOf(bridge, id, sc::kDescriptor);

	CHECK(applyDevicePreset(bridge, sc::kDescriptor, id, initDevicePreset(fx::filter::kDescriptor))
	      == DevicePresetLoad::OtherDevice);
	const std::vector<uint8_t> junk { 1, 2, 3, 4, 5 };
	CHECK(applyDevicePreset(bridge, sc::kDescriptor, id, junk) == DevicePresetLoad::Malformed);
	CHECK(applyDevicePreset(bridge, sc::kDescriptor, id, {}) == DevicePresetLoad::Malformed);
	CHECK(rowOf(bridge, id, sc::kDescriptor) == before);
}

TEST_CASE("backlog FO: SKLITTER's presets live in their own bank, and the plugin's own bank does not list "
          "them") {
	ScopedPresetLibrary library("fo-bank");
	const std::string bank = devicePresetBank(sc::kDescriptor);
	CHECK(bank == "Devices/SKLITTER");
	REQUIRE(PresetStore::save("Crunch", initDevicePreset(sc::kDescriptor), bank));
	CHECK(PresetStore::pathFor("Crunch", bank).parent_path() == PresetStore::root() / "Devices" / "SKLITTER");
	const std::vector<StoredPreset> device = PresetStore::loadAll(bank);
	REQUIRE(device.size() == 1);
	CHECK(device.front().name == "Crunch");
	CHECK(PresetStore::loadAll(kPluginBank).empty()); // Feathers/ and Devices/ are side by side
}

TEST_CASE("backlog FO: the SKLITTER panel carries Feathers' preset browser in its TITLE ROW, clear of the "
          "title, and a save then a pick through it loads the save") {
	ScopedPresetLibrary library("fo-panel");
	HeldChain held;
	ModFixture f;
	ui::WidgetRegistry registry;
	ui::SurfaceLayers layers;
	layers.setBounds(0, 0, 1400, 900);
	const size_t slot = f.bridge.addFxDevice(sc::kTraits.tag);
	const DeviceInstanceId device = f.bridge.fxDeviceAt(slot);
	f.bridge.fxBodies().attach(device, held.surface());
	std::unique_ptr<ui::ExpandedView> view =
	    ui::makeExpandedView(f.deviceHost, &f.bridge, registry, device, sc::kDescriptor);
	auto* panelView = dynamic_cast<ui::SklitterExpanded*>(view.get());
	REQUIRE(panelView != nullptr);
	ui::PresetBrowser* browser = &panelView->presetBrowserForTest();
	REQUIRE(layers.present(std::move(view), ui::SurfaceNeed::SpansGesture, "SKLITTER", {}) != ui::kNoSurface);
	ui::ExpandedPanel* panel = layers.overlay().panelForTest();
	REQUIRE(panel != nullptr);

	// IT IS THE SAME CLASS THE EDITOR'S HEADER HOLDS, parented into THIS panel's title row.
	CHECK(panel->accessory() == browser);
	CHECK(browser->parent() == panel);
	CHECK(browser->isVisible());
	CHECK(browser->bank() == "Devices/SKLITTER");
	// INSIDE THE TITLE ROW, clear of the fold chevron and of the title text.
	const visage::Bounds row = panel->titleBounds();
	CHECK(static_cast<float>(browser->y()) >= row.y());
	CHECK(static_cast<float>(browser->y() + browser->height()) <= row.bottom());
	CHECK(static_cast<float>(browser->x() + browser->width()) <= panel->foldBounds().x());
	CHECK(static_cast<float>(browser->x()) >= ui::ExpandedOverlay::kPanelPad
	                                              + ui::trackedWidth(ui::font(ui::kControlSize), "SKLITTER",
	                                                                  ui::kControlTracking));
	CHECK(browser->width() == doctest::Approx(browser->barWidth()));
	// A PRESS THERE REACHES THE BROWSER, not the head's drag: the browser is the frame under the point.
	const visage::Point middle { static_cast<float>(browser->x()) + browser->width() * 0.5f,
		                         static_cast<float>(browser->y()) + browser->height() * 0.5f };
	CHECK(panel->frameAtPoint(middle) == static_cast<visage::Frame*>(browser));
	MESSAGE("panel width " << panel->width() << ", browser at x " << browser->x() << " w " << browser->width()
	                       << ", fold at " << panel->foldBounds().x() << ", slack left of browser "
	                       << (browser->x() - ui::ExpandedOverlay::kPanelPad
	                           - ui::trackedWidth(ui::font(ui::kControlSize), "SKLITTER", ui::kControlTracking)));

	// AND AT THE NARROWEST THIS PANEL GETS — every rail folded, so the panel is its minimum — it still
	// fits: the browser is shown, not hidden by `accessoryBounds` for want of room.
	panelView->circleRailForTest().setCollapsed(true);
	panelView->grainRailForTest().setCollapsed(true);
	panelView->postRailForTest().setCollapsed(true);
	layers.overlay().resized();
	CHECK(panel->width() == doctest::Approx(panelView->minimumSize().x + 2.0f * ui::ExpandedOverlay::kPanelPad));
	CHECK(panel->accessoryBounds().width() > 0.0f);
	CHECK(browser->isVisible());
	CHECK(static_cast<float>(browser->x() + browser->width()) <= panel->foldBounds().x());
	MESSAGE("minimum panel width " << panel->width() << ", browser at x " << browser->x() << ", slack left of browser "
	                               << (browser->x() - ui::ExpandedOverlay::kPanelPad
	                                   - ui::trackedWidth(ui::font(ui::kControlSize), "SKLITTER",
	                                                      ui::kControlTracking)));

	// A SAVE THROUGH THE BROWSER, then a change, then the save picked again: the value comes back.
	const size_t mix = static_cast<size_t>(sc::Param::Mix);
	f.deviceHost.setField(device, mix, 0.25f);
	const float savedMix = f.deviceHost.fieldAt(device, mix);
	saveThroughPrompt(*browser);
	REQUIRE(browser->count() == 2); // Init (a factory FILE now) and the save
	const int savedAt = browser->currentIndex();
	CHECK(PresetStore::loadAll("Devices/SKLITTER").size() == 2); // on disk, in the device's bank
	f.deviceHost.setField(device, mix, 0.9f);
	REQUIRE(f.deviceHost.fieldAt(device, mix) != savedMix);
	browser->selectIndex(savedAt);
	CHECK(f.deviceHost.fieldAt(device, mix) == savedMix);
	// AND INIT IS THE DEFAULT.
	browser->selectIndex(0);
	CHECK(f.deviceHost.fieldAt(device, mix)
	      == device::coerceControl(sc::kDescriptor.params[mix], static_cast<float>(sc::kDescriptor.params[mix].def)));

	// DISMISSED: the panel lets go of the browser before either is retired.
	layers.overlay().dismiss();
	CHECK(panel->accessory() == nullptr);
}

TEST_CASE("the SKLITTER panel's browse-by-tag reopens its list over the panel, filed by tag") {
	ScopedPresetLibrary library("fo-panel-view");
	HeldChain held;
	ModFixture f;
	ui::WidgetRegistry registry;
	ui::SurfaceLayers layers;
	layers.setBounds(0, 0, 1400, 900);
	const size_t slot = f.bridge.addFxDevice(sc::kTraits.tag);
	const DeviceInstanceId device = f.bridge.fxDeviceAt(slot);
	f.bridge.fxBodies().attach(device, held.surface());
	std::unique_ptr<ui::ExpandedView> view =
	    ui::makeExpandedView(f.deviceHost, &f.bridge, registry, device, sc::kDescriptor);
	auto* panelView = dynamic_cast<ui::SklitterExpanded*>(view.get());
	REQUIRE(panelView != nullptr);
	ui::PresetBrowser* browser = &panelView->presetBrowserForTest();
	REQUIRE(layers.present(std::move(view), ui::SurfaceNeed::SpansGesture, "SKLITTER", {}) != ui::kNoSurface);
	REQUIRE(browser->parent() != nullptr);

	// THE MENUS visage has parented on the browser's top frame — what the showing put there.
	const auto menus = [&] {
		size_t n = 0;
		for (visage::Frame* child : browser->topParentFrame()->children()) {
			n += dynamic_cast<visage::PopupMenuFrame*>(child) != nullptr ? 1 : 0;
		}
		return n;
	};
	const size_t before = menus();
	browser->chooseFromMenu(ui::PresetBrowser::kMenuByTag);
	CHECK(menus() == before); // deferred past the closing menu's callback
	for (int i = 0; i < 250 && menus() == before; ++i) {
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
		visage::EventManager::instance().checkEventTimers();
	}
	CHECK(menus() == before + 1);
	CHECK(browser->view() == ui::PresetBrowser::View::Tags);
	// THIS BANK HAS NO TAGS, and the reopened list says so first.
	REQUIRE_FALSE(browser->listRows().empty());
	CHECK(browser->listRows().front().id == ui::ItemStepper::kNoteId);

	// DISMISSED WITH A REOPEN WAITING: nothing opens onto a panel that has gone.
	browser->chooseFromMenu(ui::PresetBrowser::kMenuByFolder);
	REQUIRE(browser->reopenPending());
	layers.overlay().dismiss();
	CHECK_FALSE(browser->reopenPending());
}

TEST_CASE("window header: the CONTAINERS' windows carry the enable chip too — SKLITTER's and the chain's, "
          "each over its own device in its own chain") {
	// **THE TWO WINDOWS THE FX-LANE SWEEP CANNOT OPEN** (`test_gui_widgets_devices.cpp`, "every device's
	// window carries ONE enable chip"): the chain container's panel is the rack module's, opened from a
	// column inside SKLITTER, and it escalates to the editor's layers — so its chip must write the
	// device in the INNER chain, through that chain's bridge and that column's history, not the host's.
	NestedThree n;

	// WHICH PANEL IS WHOSE: the overlay holds the mounts, so each is asked what it wraps.
	ui::ExpandedPanel* chainChrome = nullptr;
	ui::ExpandedPanel* sklitterChrome = nullptr;
	for (const ui::ExpandedOverlay::PanelId id : n.outer->overlay().panelIdsForTest()) {
		const auto* mount = dynamic_cast<const ui::MountedFrame*>(n.outer->overlay().contentForTest(id));
		REQUIRE(mount != nullptr);
		if (mount->mounted() == n.chainPanel) {
			chainChrome = n.outer->overlay().panelForTest(id);
		}
		if (mount->mounted() == n.sklitterPanel) {
			sklitterChrome = n.outer->overlay().panelForTest(id);
		}
	}
	REQUIRE(chainChrome != nullptr);
	REQUIRE(sklitterChrome != nullptr);
	const auto press = [](ui::ExpandedPanel& chrome) {
		const visage::Bounds sw = chrome.enableBounds();
		chrome.onMouseDown().callback(at(sw.x() + sw.width() * 0.5f, sw.y() + sw.height() * 0.5f));
		chrome.onMouseUp().callback(at(sw.x() + sw.width() * 0.5f, sw.y() + sw.height() * 0.5f));
	};

	SUBCASE("the chain's window writes the chain device's enable in the INNER chain, and its card agrees") {
		REQUIRE(cmp::kTraits.enableControl >= 0);
		const auto idx = static_cast<size_t>(cmp::kTraits.enableControl);
		rack::RackBridge& inner = n.held.bridge();
		REQUIRE(chainChrome->enableBounds().width() > 0.0f);
		REQUIRE(inner.fieldAt(n.chainDevice, idx) == 1.0f);
		ui::FxCard* card = nullptr;
		for (ui::FxCard* c : n.column->cardsForTest()) {
			if (c->device() == n.chainDevice) {
				card = c;
			}
		}
		REQUIRE(card != nullptr);
		const size_t steps = n.column->historyForTest().length();
		press(*chainChrome);
		CHECK(inner.fieldAt(n.chainDevice, idx) == 0.0f);
		CHECK_FALSE(card->headEnabled());
		CHECK(n.column->historyForTest().length() == steps + 1);
		(void)n.outer->updateContent();
		CHECK_FALSE(chainChrome->enableShownForTest());
		// AND THE HOST'S OWN SKLITTER, which shares no id space with the chain's device, is untouched.
		CHECK(n.f.deviceHost.fieldAt(n.sklitterPanel->watchedDevice(),
		                             static_cast<size_t>(sc::kTraits.enableControl))
		      == 1.0f);
	}

	SUBCASE("SKLITTER's window writes SKLITTER's enable in the host's chain") {
		const auto idx = static_cast<size_t>(sc::kTraits.enableControl);
		const DeviceInstanceId device = n.sklitterPanel->watchedDevice();
		REQUIRE(sklitterChrome->enableBounds().width() > 0.0f);
		REQUIRE(n.f.deviceHost.fieldAt(device, idx) == 1.0f);
		press(*sklitterChrome);
		CHECK(n.f.deviceHost.fieldAt(device, idx) == 0.0f);
		(void)n.outer->updateContent();
		CHECK_FALSE(sklitterChrome->enableShownForTest());
		// AND THE PRESET BROWSER BESIDE IT KEPT ITS ROOM: the accessory sits inboard of the chip.
		if (visage::Frame* accessory = sklitterChrome->accessory(); accessory != nullptr && accessory->isVisible()) {
			CHECK(static_cast<float>(accessory->x() + accessory->width()) <= sklitterChrome->enableBounds().x());
		}
	}
}

//------------------------------------------------------------------------------------------------//
// CHAIN ACCESS SITES (backlog NR) — the card's chip and the panel it opens                         //
//------------------------------------------------------------------------------------------------//

namespace {

/// A RACK HOST — an engine and its bridge, prepared — as the rack's own editor has one.
struct SiteHost {
	rack::RackEngine engine { rack::kRackAxes, hostCatalogue() };
	rack::RackBridge bridge { engine };
	SiteHost() {
		engine.prepare(48000.0, 4096);
		bridge.prepare(48000.0, 4096);
	}
};

visage::Point centreOf(const visage::Bounds& b) { return { b.x() + b.width() * 0.5f, b.y() + b.height() * 0.5f }; }

bool overlaps(const visage::Bounds& a, const visage::Bounds& b) {
	return a.width() > 0.0f && b.width() > 0.0f && a.x() < b.right() && b.x() < a.right() && a.y() < b.bottom()
	       && b.y() < a.bottom();
}

} // namespace

TEST_CASE("NR: a card draws the site chip exactly when pressing it opens a site — the host binds the verb, the device "
          "offers one (a Rack and a Chain too) — and a press on it is the verb") {
	SiteHost h;
	const DeviceInstanceId id = h.bridge.addDevice(fx::filter::kTraits.tag);
	REQUIRE(id != kNoInstance);
	const device::Descriptor* desc = h.bridge.descriptorOf(id);
	REQUIRE(desc != nullptr);
	ui::GenericCard card { h.bridge, id, *desc, nullptr };
	card.setBounds(0, 0, 320, 200);
	CHECK(card.siteBounds().width() == 0.0f); // no verb bound: no chip
	int pressed = 0;
	card.onSite = [&] { ++pressed; };
	const visage::Bounds chip = card.siteBounds();
	REQUIRE(chip.width() > 0.0f);
	// IN THE HEAD, AND ON NOTHING ELSE THERE.
	CHECK(chip.bottom() <= ui::kCardHeadHeight);
	CHECK_FALSE(overlaps(chip, card.expandBounds()));
	CHECK_FALSE(overlaps(chip, card.enableBounds()));
	CHECK_FALSE(overlaps(chip, card.slotMenuBounds()));
	card.noteCardPress(centreOf(chip), 1);
	CHECK(pressed == 1);

	// A RACK AND A CHAIN OFFER ONE TOO, ON THEIR OWN CARDS (D3 lifted,); a container whose lanes ARE its
	// sound (SKLITTER) still offers none, whatever the host binds.
	ui::WidgetRegistry registry;
	const DeviceInstanceId chain = h.bridge.addDevice(cmp::kTraits.tag);
	const DeviceInstanceId rackId = h.bridge.addDevice(rack::parallel::kTraits.tag);
	const DeviceInstanceId sklitter = h.bridge.addDevice(fx::sklitter::kTraits.tag);
	REQUIRE(chain != kNoInstance);
	REQUIRE(rackId != kNoInstance);
	REQUIRE(sklitter != kNoInstance);
	ui::ChainCard chainCard { h.bridge, registry, chain };
	ui::ParallelRackCard rackCard { h.bridge, registry, rackId };
	for (ui::FxCard* container : { static_cast<ui::FxCard*>(&chainCard), static_cast<ui::FxCard*>(&rackCard) }) {
		container->setBounds(0, 0, 320, 200);
		CHECK(container->siteBounds().width() == 0.0f); // no verb bound: no chip
		const int before = pressed;
		container->onSite = [&] { ++pressed; };
		const visage::Bounds own = container->siteBounds();
		REQUIRE(own.width() > 0.0f);
		CHECK(own.bottom() <= ui::kCardHeadHeight);
		CHECK_FALSE(overlaps(own, container->expandBounds()));
		CHECK_FALSE(overlaps(own, container->enableBounds()));
		container->noteCardPress(centreOf(own), 1);
		CHECK(pressed == before + 1);
	}
	CHECK(h.bridge.openSite(chain));
	CHECK(h.bridge.openSite(rackId));
	ui::GenericCard grains { h.bridge, sklitter, *h.bridge.descriptorOf(sklitter), nullptr };
	grains.setBounds(0, 0, 320, 200);
	grains.onSite = [&] { ++pressed; };
	CHECK(grains.siteBounds().width() == 0.0f);
	CHECK_FALSE(h.bridge.openSite(sklitter));
}

TEST_CASE("NZ: the site panel is ONE chain — PRE's cards, a partition, the device itself outlined, a partition, POST's "
          "cards — and its centre is the device through the host it runs in, not a copy") {
	rack::registerChainExpandedView(); // the site's panel is filed by the same hook
	SiteHost h;
	const DeviceInstanceId id = h.bridge.addDevice(fx::filter::kTraits.tag);
	REQUIRE(id != kNoInstance);
	REQUIRE(h.bridge.openSite(id));
	auto* site = static_cast<rack::SiteChains*>(h.bridge.containerSurfaceOf(siteKeyOf(id)));
	REQUIRE(site != nullptr);
	const DeviceInstanceId before = site->pre().bridge()->addDevice(fx::filter::kTraits.tag);
	const DeviceInstanceId after = site->post().bridge()->addDevice(fx::filter::kTraits.tag);
	REQUIRE(before != kNoInstance);
	REQUIRE(after != kNoInstance);
	ui::WidgetRegistry registry;
	std::unique_ptr<ui::ExpandedView> view =
	    ui::makeExpandedView(h.bridge, nullptr, registry, siteKeyOf(id), ui::siteViewDescriptor());
	auto* panel = dynamic_cast<rack::SiteExpanded*>(view.get());
	REQUIRE(panel != nullptr);
	const visage::Point size = panel->minimumSize();
	panel->setBounds(0, 0, size.x, size.y);
	(void)panel->updateContent();
	REQUIRE(panel->liveForTest());
	CHECK(panel->offersPreForTest());
	CHECK(panel->bridgeForTest() == site->pre().bridge());
	rack::RackEditor* editor = panel->editorForTest();
	REQUIRE(editor != nullptr);

	ui::FxCard* pre = editor->cardOfDeviceForTest(before);
	ui::FxCard* centre = editor->centreCardForTest();
	const std::vector<ui::FxCard*> post = editor->postCardsForTest();
	REQUIRE(pre != nullptr);
	REQUIRE(centre != nullptr);
	REQUIRE(post.size() == 1u);
	// THE CENTRE IS THE DEVICE, OVER THE HOST IT RUNS IN — the same instance its lane card addresses.
	CHECK(centre->device() == id);
	CHECK(centre->deviceHost() == &h.bridge);
	CHECK(post[0]->device() == after);
	CHECK(post[0]->deviceHost() == site->post().bridge());
	// LEFT TO RIGHT IN SIGNAL ORDER, a partition each side of the centre.
	CHECK(pre->x() + pre->width() <= editor->partitionForTest(0));
	CHECK(editor->partitionForTest(0) < centre->x());
	CHECK(centre->x() + centre->width() <= editor->partitionForTest(1));
	CHECK(editor->partitionForTest(1) < post[0]->x());
	// POST GROWS THROUGH ITS OWN SEGMENT: an "Add before" on a POST card lands in POST.
	REQUIRE(editor->insertDeviceBeforeForTest(after, fx::filter::kTraits.tag) != kNoInstance);
	CHECK(site->post().bridge()->chainLength(0) == 2u);
	CHECK(site->pre().bridge()->chainLength(0) == 1u);
	CHECK(editor->postAddRowForTest() != nullptr);
}

TEST_CASE("NR: an edit inside a site's panel records into the panel's own history, and its undo puts the lane's value back") {
	rack::registerChainExpandedView();
	SiteHost h;
	const DeviceInstanceId id = h.bridge.addDevice(fx::filter::kTraits.tag);
	REQUIRE(h.bridge.openSite(id));
	auto* site = static_cast<rack::SiteChains*>(h.bridge.containerSurfaceOf(siteKeyOf(id)));
	REQUIRE(site != nullptr);
	rack::RackBridge* pre = site->pre().bridge();
	const DeviceInstanceId inner = pre->addDevice(fx::filter::kTraits.tag);
	REQUIRE(inner != kNoInstance);
	ui::WidgetRegistry registry;
	std::unique_ptr<ui::ExpandedView> view =
	    ui::makeExpandedView(h.bridge, nullptr, registry, siteKeyOf(id), ui::siteViewDescriptor());
	auto* panel = dynamic_cast<rack::SiteExpanded*>(view.get());
	REQUIRE(panel != nullptr);
	REQUIRE(panel->editorForTest() != nullptr);
	// THE PANEL'S EDITOR RECORDS INTO THE PANEL'S HISTORY — the inner rack's own, as a chain device's panel's is.
	CHECK(&panel->editorForTest()->historyForTest() == &panel->historyForTest());
	const auto cutoff = static_cast<size_t>(fx::filter::Param::Cutoff);
	const float before = pre->fieldAt(inner, cutoff);
	ui::DeviceControl knob { *pre, inner, fx::filter::kDescriptor, cutoff, nullptr, &panel->historyForTest() };
	knob.beginGesture();
	knob.setValue(static_cast<double>(before) * 0.5);
	knob.endGesture();
	REQUIRE(panel->historyForTest().canUndo());
	panel->historyForTest().undo();
	CHECK(pre->fieldAt(inner, cutoff) == before);
}

TEST_CASE("NR: PRE is veiled and refused while the device runs in the voices (D4), and offered again when it does not") {
	rack::registerChainExpandedView();
	SiteHost h;
	h.engine.setVoices(2);
	const DeviceInstanceId id = h.bridge.addDevice(fx::filter::kTraits.tag);
	const auto poly = static_cast<size_t>(fx::filter::Param::Poly);
	h.bridge.setField(id, poly, 1.0f);
	REQUIRE(h.bridge.runsPerVoice(id));
	REQUIRE(h.bridge.openSite(id));
	ui::WidgetRegistry registry;
	std::unique_ptr<ui::ExpandedView> view =
	    ui::makeExpandedView(h.bridge, nullptr, registry, siteKeyOf(id), ui::siteViewDescriptor());
	auto* panel = dynamic_cast<rack::SiteExpanded*>(view.get());
	REQUIRE(panel != nullptr);
	REQUIRE(panel->editorForTest() != nullptr);
	CHECK(panel->offersPreForTest());
	CHECK_FALSE(panel->preAvailableForTest());
	CHECK_FALSE(panel->editorForTest()->preAvailableForTest());
	h.bridge.setField(id, poly, 0.0f);
	(void)panel->updateContent();
	CHECK(panel->preAvailableForTest());
	CHECK(panel->editorForTest()->preAvailableForTest());
	// AND A PANEL SHOWING PRE VEILS IT when the device goes into the voices.
	h.bridge.setField(id, poly, 1.0f);
	(void)panel->updateContent();
	CHECK_FALSE(panel->editorForTest()->preAvailableForTest());
}

namespace {

/// The PRE veil of `id`'s site panel, laid out at its minimum size: rows as drawn and the room they are drawn in.
struct VeilRead {
	std::vector<std::string> rows;
	float room = 0.0f;
	bool visible = false;
	bool offersPre = false;
};

VeilRead readPreVeil(SiteHost& h, DeviceInstanceId id) {
	VeilRead out;
	REQUIRE(h.bridge.openSite(id));
	ui::WidgetRegistry registry;
	std::unique_ptr<ui::ExpandedView> view =
	    ui::makeExpandedView(h.bridge, nullptr, registry, siteKeyOf(id), ui::siteViewDescriptor());
	auto* panel = dynamic_cast<rack::SiteExpanded*>(view.get());
	REQUIRE(panel != nullptr);
	const visage::Point size = panel->minimumSize();
	panel->setBounds(0, 0, size.x, size.y);
	(void)panel->updateContent();
	rack::RackEditor* editor = panel->editorForTest();
	REQUIRE(editor != nullptr);
	out.rows = editor->preVeilLinesForTest();
	out.room = editor->preVeilBoundsForTest().width() - 16.0f;
	out.visible = editor->preVeilVisibleForTest();
	out.offersPre = panel->offersPreForTest();
	return out;
}

} // namespace

TEST_CASE("NR: Harm offers PRE — its notch and high-pass process the passing signal — and PRE is veiled "
          "only under Poly, with the reason wrapped to fit the veil") {
	rack::registerChainExpandedView();
	SiteHost h;
	h.engine.setVoices(2);
	const DeviceInstanceId id = h.bridge.addDevice(fx::harm::kTraits.tag);
	REQUIRE(id != kNoInstance);
	{
		const VeilRead mono = readPreVeil(h, id);
		CHECK(mono.offersPre);
		CHECK_FALSE(mono.visible);
	}
	h.bridge.setField(id, static_cast<size_t>(fx::harm::Param::Poly), 1.0f);
	REQUIRE(h.bridge.runsPerVoice(id));
	const VeilRead poly = readPreVeil(h, id);
	CHECK(poly.offersPre);
	CHECK(poly.visible);
	REQUIRE(poly.rows.size() >= 2u); // it does not fit on one row, so the wrap is what is under test
	const visage::Font face = ui::font(12.0f);
	for (const std::string& row : poly.rows) {
		INFO(row);
		CHECK(ui::textWidth(row, face) <= poly.room);
	}
	std::string joined;
	for (const std::string& row : poly.rows) {
		joined += (joined.empty() ? "" : " ") + row;
	}
	CHECK(joined.find("per voice") != std::string::npos);
	CHECK(joined.find("no PRE") == std::string::npos);
}

TEST_CASE("NR: a POST-only device's PRE veil says why, and every row of it fits the veil") {
	rack::registerChainExpandedView();
	SiteHost h;
	const DeviceInstanceId id = h.bridge.addDevice(fx::noise::kTraits.tag);
	REQUIRE(id != kNoInstance);
	const VeilRead noise = readPreVeil(h, id);
	CHECK_FALSE(noise.offersPre);
	CHECK(noise.visible);
	REQUIRE(noise.rows.size() >= 2u);
	const visage::Font face = ui::font(12.0f);
	std::string joined;
	for (const std::string& row : noise.rows) {
		INFO(row);
		CHECK(ui::textWidth(row, face) <= noise.room);
		joined += (joined.empty() ? "" : " ") + row;
	}
	CHECK(joined == "This device has no PRE: nothing comes before it to process");
}

TEST_CASE("NZ: in the site panel a PRE modulator lights the device and POST, and a POST modulator lights only POST — "
          "the screen's order is the reach") {
	rack::registerChainExpandedView();
	SiteHost h;
	const DeviceInstanceId id = h.bridge.addDevice(fx::filter::kTraits.tag);
	REQUIRE(h.bridge.openSite(id));
	auto* site = static_cast<rack::SiteChains*>(h.bridge.containerSurfaceOf(siteKeyOf(id)));
	REQUIRE(site != nullptr);
	rack::RackBridge* pre = site->pre().bridge();
	rack::RackBridge* post = site->post().bridge();
	const DeviceInstanceId preLfo = pre->addDevice(mod::lfo::kTraits.tag);
	const DeviceInstanceId preFilter = pre->addDevice(fx::filter::kTraits.tag);
	const DeviceInstanceId postLfo = post->addDevice(mod::lfo::kTraits.tag);
	const DeviceInstanceId postFilter = post->addDevice(fx::filter::kTraits.tag);
	REQUIRE(preLfo != kNoInstance);
	REQUIRE(postFilter != kNoInstance);
	ui::WidgetRegistry registry;
	std::unique_ptr<ui::ExpandedView> view =
	    ui::makeExpandedView(h.bridge, nullptr, registry, siteKeyOf(id), ui::siteViewDescriptor());
	auto* panel = dynamic_cast<rack::SiteExpanded*>(view.get());
	REQUIRE(panel != nullptr);
	ui::ModState outer;
	ui::EditHistory history;
	panel->modulationBound(&outer, &history);
	// THE THREE SEGMENTS' CUTOFFS, each read as the editor reads it: through its own host and its segment's state.
	const auto cutoff = static_cast<size_t>(fx::filter::Param::Cutoff);
	ui::DeviceControl preKnob { *pre, preFilter, fx::filter::kDescriptor, cutoff, &panel->preModForTest(), nullptr };
	ui::DeviceControl centreKnob { h.bridge, id, fx::filter::kDescriptor, cutoff, &panel->centreModForTest(), nullptr };
	ui::DeviceControl postKnob { *post, postFilter, fx::filter::kDescriptor, cutoff, &panel->postModForTest(), nullptr };

	// ARMED IN PRE: the rest of PRE, the device and POST all volunteer.
	panel->preModForTest().focus(backplane::Source::output(preLfo, 0));
	panel->preModForTest().arm(true);
	const backplane::Source exported = panel->centreModForTest().source();
	CHECK(exported.isExport());
	CHECK(exported == h.bridge.siteExport(id, backplane::Source::output(preLfo, 0)));
	CHECK(panel->postModForTest().source() == exported.asOuter());
	CHECK(preKnob.display().routable);
	CHECK(centreKnob.display().routable);
	CHECK(postKnob.display().routable);

	// ARMED IN POST: only POST (decision O3). The device's own state is not armed at all.
	panel->postModForTest().focus(backplane::Source::output(postLfo, 0));
	panel->postModForTest().arm(true);
	CHECK(postKnob.display().routable);
	CHECK_FALSE(centreKnob.display().routable);
	CHECK_FALSE(preKnob.display().routable);

	// AND A WRITE FROM THE CENTRE LANDS IN THE HOST, as the device's own route from its PRE: the drag's road, by value.
	panel->postModForTest().arm(false);
	panel->preModForTest().arm(true);
	h.bridge.setRouteAmount(panel->centreModForTest().source(), backplane::Destination::control(id, cutoff), 0.5f);
	CHECK(h.bridge.routeAmount(exported, backplane::Destination::control(id, cutoff)) == 0.5f);
}

TEST_CASE("NZ: the site panel carries the Chain's furniture — the device's own Mix and wet Gain as the head row's two "
          "knobs, the same controls as on its card") {
	rack::registerChainExpandedView();
	SiteHost h;
	// PITCH DECLARES BOTH ROLES (backlog NR: its mix is the one the slot blends); Noise declares neither (Harm
	// took them 2026-10-29, the Clipper 2026-10-28).
	const DeviceInstanceId pitch = h.bridge.addDevice(fx::pitch::kTraits.tag);
	const DeviceInstanceId filter = h.bridge.addDevice(fx::noise::kTraits.tag);
	REQUIRE(pitch != kNoInstance);
	REQUIRE(filter != kNoInstance);
	REQUIRE(h.bridge.openSite(pitch));
	REQUIRE(h.bridge.openSite(filter));
	ui::WidgetRegistry registry;
	const auto open = [&](DeviceInstanceId id) {
		return ui::makeExpandedView(h.bridge, nullptr, registry, siteKeyOf(id), ui::siteViewDescriptor());
	};
	const device::Descriptor& desc = *h.bridge.descriptorOf(pitch);
	const size_t declared = (desc.traits.mixControl >= 0 ? 1u : 0u) + (desc.traits.wetGainControl >= 0 ? 1u : 0u);
	REQUIRE(declared > 0u);
	std::unique_ptr<ui::ExpandedView> withMix = open(pitch);
	auto* panel = dynamic_cast<rack::SiteExpanded*>(withMix.get());
	REQUIRE(panel != nullptr);
	REQUIRE(panel->editorForTest() != nullptr);
	CHECK(panel->editorForTest()->furnitureKnobCountForTest() == declared);
	std::unique_ptr<ui::ExpandedView> without = open(filter);
	auto* bare = dynamic_cast<rack::SiteExpanded*>(without.get());
	REQUIRE(bare != nullptr);
	REQUIRE(bare->editorForTest() != nullptr);
	CHECK(bare->editorForTest()->furnitureKnobCountForTest() == 0u);
}

TEST_CASE("NR: the eight devices that got the furniture show Mix and wet Gain as the site panel's head row, "
          "and their cards are unchanged — Clip and Drawn among them") {
	rack::registerChainExpandedView();
	SiteHost h;
	ui::WidgetRegistry registry;
	const char* tags[] = { fx::eq::kTraits.tag, fx::disperser::kTraits.tag, fx::filter::kTraits.tag,
	                       fx::waypoint::kTraits.tag, fx::busutil::kTraits.tag, fx::clip::kTraits.tag,
	                       fx::drawn::kTraits.tag, fx::harm::kTraits.tag };
	for (const char* tag : tags) {
		const DeviceInstanceId id = h.bridge.addDevice(tag);
		REQUIRE(id != kNoInstance);
		INFO(tag);
		REQUIRE(h.bridge.openSite(id));
		const device::Descriptor& desc = *h.bridge.descriptorOf(id);
		CHECK(desc.traits.mixControl >= 0);
		CHECK(desc.traits.wetGainControl >= 0);
		std::unique_ptr<ui::ExpandedView> view =
		    ui::makeExpandedView(h.bridge, nullptr, registry, siteKeyOf(id), ui::siteViewDescriptor());
		auto* panel = dynamic_cast<rack::SiteExpanded*>(view.get());
		REQUIRE(panel != nullptr);
		REQUIRE(panel->editorForTest() != nullptr);
		CHECK(panel->editorForTest()->furnitureKnobCountForTest() == 2u);
	}
}

TEST_CASE("NZ: in the assembled editor, the default patch's fx-lane card draws the site chip, and a click on it "
          "gives that device its site and opens the site's panel") {
	// THE CARD THE EDITOR OPENED ON. The lane builds its cards in its constructor and the editor installs the site
	// verb afterwards, so the cards of the patch it opened on were never told it and drew no chip.
	FilterFixture f;
	const DeviceInstanceId id = f.card->device();
	REQUIRE(id != kNoInstance);
	REQUIRE_FALSE(f.deviceHost.hasSite(id));
	const visage::Bounds chip = f.card->siteBounds();
	REQUIRE(chip.width() > 0.0f);
	// THROUGH THE ROOT, where a hand clicks.
	f.click(chip);
	CHECK(f.deviceHost.hasSite(id));
	ui::ExpandedOverlay* overlay = f.gui.expandedForTest();
	REQUIRE(overlay != nullptr);
	REQUIRE(overlay->open());
	// THE LAYERS MOUNT WHAT THEY PRESENT, so the panel is the mount's content.
	const auto* mount = dynamic_cast<const ui::MountedFrame*>(overlay->contentForTest());
	REQUIRE(mount != nullptr);
	const auto* panel = dynamic_cast<const rack::SiteExpanded*>(mount->mounted());
	REQUIRE(panel != nullptr);
	CHECK(panel->ownerForTest() == id);
}

TEST_CASE("NZ rung 4: a PRE modulator armed in the site panel lights the lane's device AFTER the holder, out the back, "
          "and not the one before it; disarming puts the editor's own state back") {
	// THE ASSEMBLED EDITOR, the default patch's Filter as the device BEFORE, then the holder and a device AFTER.
	FilterFixture f;
	const DeviceInstanceId before = f.card->device();
	const size_t holderSlot = f.bridge.addFxDevice(fx::filter::kTraits.tag);
	const size_t afterSlot = f.bridge.addFxDevice(fx::filter::kTraits.tag);
	REQUIRE(holderSlot < kMaxFxSlots);
	REQUIRE(afterSlot < kMaxFxSlots);
	f.bridge.refresh();
	f.lane->resized();
	const DeviceInstanceId holder = f.bridge.fxDeviceAt(holderSlot);
	const DeviceInstanceId after = f.bridge.fxDeviceAt(afterSlot);
	REQUIRE(f.bridge.openFxSite(holder));
	// NO SYNTH IN THIS FIXTURE, so the site is built as the converger builds it and filed where the panel finds it.
	auto site = std::make_unique<rack::SiteChains>(holder, hostCatalogue(), 48000.0, 4096);
	site->setOuterGate(rack::ChainRecord::gateOver(&f.bridge.chainRecord(), holder));
	const DeviceInstanceId lfo = site->pre().bridge()->addDevice(mod::lfo::kTraits.tag);
	REQUIRE(lfo != kNoInstance);
	f.bridge.fxBodies().attach(siteKeyOf(holder), site.get());
	ui::FxCard* card = f.lane->cardAt(holderSlot);
	REQUIRE(card != nullptr);
	REQUIRE(card->onSite);
	card->onSite();
	auto* panel = findInTree<rack::SiteExpanded>(f.root);
	REQUIRE(panel != nullptr);

	ui::ModState* editorMod = f.gui.modForTest();
	REQUIRE(editorMod != nullptr);
	const device::Source was = editorMod->source();
	const auto cutoff = static_cast<size_t>(fx::filter::Param::Cutoff);
	ui::DeviceControl afterKnob { f.deviceHost, after, fx::filter::kDescriptor, cutoff, editorMod, nullptr };
	ui::DeviceControl beforeKnob { f.deviceHost, before, fx::filter::kDescriptor, cutoff, editorMod, nullptr };
	CHECK_FALSE(afterKnob.display().routable);

	// ARMED IN PRE, AND THE PANEL'S TICK RELAYS IT: the device after lights, the one before does not.
	panel->preModForTest().focus(backplane::Source::output(lfo, 0));
	panel->preModForTest().arm(true);
	CHECK(panel->relayArmForTest());
	CHECK(editorMod->source() == f.deviceHost.siteExport(holder, backplane::Source::output(lfo, 0)));
	CHECK(afterKnob.display().routable);
	CHECK_FALSE(beforeKnob.display().routable);

	// DISARMED IN PRE: the editor's state is put back as it was found.
	panel->preModForTest().arm(false);
	CHECK(panel->relayArmForTest());
	CHECK_FALSE(editorMod->armed());
	CHECK(editorMod->source() == was);
	CHECK_FALSE(afterKnob.display().routable);
	f.gui.expandedForTest()->dismiss();
	f.bridge.fxBodies().attach(siteKeyOf(holder), nullptr);
}

TEST_CASE("OA: a rail's column (SKLITTER's post chain here; a Rack's chains are the same widget) selects with Ctrl, duplicates the selection through its dots menu, and holds one Esc entry") {
	NestedThree n;
	ui::ChainColumn& column = *n.column;
	const DeviceInstanceId a = column.addDevice("flt1");
	const DeviceInstanceId b = column.addDevice("flt1");
	REQUIRE(a != kNoInstance);
	REQUIRE(b != kNoInstance);
	column.syncCards();
	column.setBounds(0, 0, 260, 1200);
	const auto press = [&](DeviceInstanceId d, int modifiers) {
		ui::FxCard* card = column.cardOf(d);
		REQUIRE(card != nullptr);
		card->onCardPressed();
		visage::MouseEvent e {};
		e.position = { static_cast<float>(card->width()) * 0.5f, static_cast<float>(card->height()) * 0.5f };
		e.modifiers = modifiers;
		e.repeat_click_count = 1;
		card->noteCardPress(e);
		card->noteCardRelease(e);
	};
	press(a, 0);
	press(b, visage::kModifierRegCtrl);
	CHECK(column.cardOf(a)->headFocused());
	CHECK(column.cardOf(b)->headFocused());
	CHECK(column.selectionForTest().escapeHeldForTest());
	const size_t before = column.deviceCount();

	REQUIRE(column.slotVerbForTest(a, ui::FragmentVerb::Duplicate));
	CHECK(column.deviceCount() == before + 2);
	// THE COPIES ARE THE SELECTION NOW, and they sit right after the last selected device, in order.
	const std::vector<DeviceInstanceId> copies = column.selectionForTest().selection().ids();
	REQUIRE(copies.size() == 2);
	CHECK(copies[0] != a);
	CHECK(copies[1] != b);
	CHECK(column.deviceAt(column.deviceCount() - 2) == copies[0]);
	CHECK(column.deviceAt(column.deviceCount() - 1) == copies[1]);
	column.historyForTest().undo();
	column.syncCards();
	CHECK(column.deviceCount() == before);
}

//------------------------------------------------------------------------------------------------//
// NZ: POST CARDS DRAG — within POST, and across the centre between PRE and POST, one undo step each  //
//------------------------------------------------------------------------------------------------//

namespace {

/// THE SITE PANEL OVER A FILTER: one Filter in PRE, three in POST, the editor laid out wide enough to show them all.
struct SitePanel {
	SiteHost h;
	DeviceInstanceId id = kNoInstance;
	rack::SiteChains* site = nullptr;
	DeviceInstanceId pre1 = kNoInstance;
	std::vector<DeviceInstanceId> post;
	ui::WidgetRegistry registry;
	std::unique_ptr<ui::ExpandedView> view;
	rack::SiteExpanded* panel = nullptr;
	rack::RackEditor* editor = nullptr;

	SitePanel() {
		rack::registerChainExpandedView();
		id = h.bridge.addDevice(fx::filter::kTraits.tag);
		REQUIRE(id != kNoInstance);
		REQUIRE(h.bridge.openSite(id));
		site = static_cast<rack::SiteChains*>(h.bridge.containerSurfaceOf(siteKeyOf(id)));
		REQUIRE(site != nullptr);
		pre1 = site->pre().bridge()->addDevice(fx::filter::kTraits.tag);
		REQUIRE(pre1 != kNoInstance);
		for (int i = 0; i < 3; ++i) {
			post.push_back(site->post().bridge()->addDevice(fx::filter::kTraits.tag));
			REQUIRE(post.back() != kNoInstance);
		}
		view = ui::makeExpandedView(h.bridge, nullptr, registry, siteKeyOf(id), ui::siteViewDescriptor());
		panel = dynamic_cast<rack::SiteExpanded*>(view.get());
		REQUIRE(panel != nullptr);
		const visage::Point size = panel->minimumSize();
		panel->setBounds(0, 0, std::max(size.x, 4000.0f), size.y);
		(void)panel->updateContent();
		editor = panel->editorForTest();
		REQUIRE(editor != nullptr);
		editor->setBounds(0, 0, 4000, editor->height());
	}
	std::vector<DeviceInstanceId> order(rack::RackBridge* lane) const {
		std::vector<DeviceInstanceId> out;
		for (size_t p = 0; p < lane->chainLength(0); ++p) {
			out.push_back(lane->deviceAtPosition(p));
		}
		return out;
	}
	/// A DRAG BY HAND: `device`'s card taken at its centre, moved to `to` (editor coordinates), and let go.
	void drag(DeviceInstanceId device, visage::Point to, bool release = true) {
		const visage::Bounds card = editor->cardBoundsForTest(device);
		REQUIRE(card.width() > 0.0f);
		const visage::Point grip { card.width() * 0.5f, card.height() * 0.5f };
		editor->cardDragForTest(device, ui::FxCard::DragPhase::Begin, grip);
		editor->cardDragForTest(device, ui::FxCard::DragPhase::Move, { to.x - card.x(), to.y - card.y() });
		if (release) {
			editor->cardDragForTest(device, ui::FxCard::DragPhase::End, { to.x - card.x(), to.y - card.y() });
		}
	}
	/// A point at the LEFT edge of `device`'s card — the gap before it.
	visage::Point before(DeviceInstanceId device) const {
		const visage::Bounds b = editor->cardBoundsForTest(device);
		return { b.x() + 2.0f, b.y() + b.height() * 0.5f };
	}
};

} // namespace

TEST_CASE("NZ: a POST card drags within POST — the drop names a POST gap, the release reorders POST, one undo step, and "
          "undo puts the order back") {
	SitePanel s;
	rack::RackBridge* post = s.site->post().bridge();
	const std::vector<ui::FxCard*> cards = s.editor->postCardsForTest();
	REQUIRE(cards.size() == 3u);
	CHECK(static_cast<bool>(cards[2]->onCardDrag)); // the card reports its gesture to the lane
	const size_t steps = s.panel->historyForTest().length();

	// HELD OVER THE FIRST POST CARD: a POST gap, the first.
	s.drag(s.post[2], s.before(s.post[0]), false);
	CHECK(s.editor->dropIntoPostForTest());
	REQUIRE(s.editor->dropGapForTest().has_value());
	CHECK(*s.editor->dropGapForTest() == 0u);
	s.editor->cardDragForTest(s.post[2], ui::FxCard::DragPhase::End,
	                          { s.before(s.post[0]).x - s.editor->cardBoundsForTest(s.post[2]).x(),
	                            s.before(s.post[0]).y - s.editor->cardBoundsForTest(s.post[2]).y() });
	CHECK(s.order(post) == std::vector<DeviceInstanceId> { s.post[2], s.post[0], s.post[1] });
	CHECK(s.order(s.site->pre().bridge()) == std::vector<DeviceInstanceId> { s.pre1 }); // PRE untouched
	CHECK(s.panel->historyForTest().length() == steps + 1);
	s.panel->historyForTest().undo();
	CHECK(s.order(post) == std::vector<DeviceInstanceId> { s.post[0], s.post[1], s.post[2] });
}

TEST_CASE("NZ: a card drags across the centre — POST into PRE and PRE into POST, under the same id, one undo step each; a "
          "host source's route travels, PRE's own export's does not and is said; the centre takes no drop") {
	SitePanel s;
	rack::RackBridge* pre = s.site->pre().bridge();
	rack::RackBridge* post = s.site->post().bridge();
	const auto cutoff = static_cast<size_t>(fx::filter::Param::Cutoff);
	// A POST FILTER READS A HOST MACRO and PRE's own export (an LFO in PRE).
	const DeviceInstanceId lfo = pre->addDevice(mod::lfo::kTraits.tag);
	REQUIRE(lfo != kNoInstance);
	(void)s.panel->updateContent();
	const backplane::Source out = s.h.bridge.siteExport(s.id, backplane::Source::output(lfo, 0));
	REQUIRE(out.isExport());
	const backplane::Destination moving = backplane::Destination::control(s.post[1], cutoff);
	REQUIRE(post->mayRoute(backplane::Source::outerCore(0), moving));
	post->setRouteAmount(backplane::Source::outerCore(0), moving, 0.3f);
	post->setRouteAmount(out.asOuter(), moving, 0.4f);
	REQUIRE(post->routeAmount(out.asOuter(), moving) == doctest::Approx(0.4f));
	(void)s.panel->updateContent();
	const size_t steps = s.panel->historyForTest().length();

	// OVER THE CENTRE: no gap, and a release there moves nothing.
	const visage::Bounds centre = s.editor->centreCardForTest()->bounds();
	s.drag(s.post[1], centreOf(centre), false);
	CHECK_FALSE(s.editor->dropGapForTest().has_value());
	s.editor->cardDragForTest(s.post[1], ui::FxCard::DragPhase::End,
	                          { centreOf(centre).x - s.editor->cardBoundsForTest(s.post[1]).x(),
	                            centreOf(centre).y - s.editor->cardBoundsForTest(s.post[1]).y() });
	CHECK(s.order(post) == std::vector<DeviceInstanceId> { s.post[0], s.post[1], s.post[2] });
	CHECK(s.panel->historyForTest().length() == steps);

	// POST INTO PRE, before PRE's Filter: the same id lands in PRE and leaves POST.
	s.drag(s.post[1], s.before(s.pre1), false);
	CHECK_FALSE(s.editor->dropIntoPostForTest());
	REQUIRE(s.editor->dropGapForTest().has_value());
	CHECK(*s.editor->dropGapForTest() == 0u);
	s.editor->cardDragForTest(s.post[1], ui::FxCard::DragPhase::End,
	                          { s.before(s.pre1).x - s.editor->cardBoundsForTest(s.post[1]).x(),
	                            s.before(s.pre1).y - s.editor->cardBoundsForTest(s.post[1]).y() });
	CHECK(s.order(pre) == std::vector<DeviceInstanceId> { s.post[1], s.pre1, lfo });
	CHECK(s.order(post) == std::vector<DeviceInstanceId> { s.post[0], s.post[2] });
	CHECK(s.panel->historyForTest().length() == steps + 1);
	// THE HOST'S MACRO STILL REACHES IT; PRE'S OWN EXPORT CANNOT (it would read itself), and that is SAID.
	CHECK(pre->routeAmount(backplane::Source::outerCore(0), moving) == doctest::Approx(0.3f));
	CHECK(pre->routeAmount(out.asOuter(), moving) == 0.0f);
	CHECK(post->routeAmount(out.asOuter(), moving) == 0.0f);
	CHECK(s.editor->notifyTrayForTest().text().find("1 route could not cross") != std::string::npos);
	(void)s.panel->updateContent();

	// ONE UNDO PUTS IT BACK, ROUTES AND ALL.
	s.panel->historyForTest().undo();
	(void)s.panel->updateContent();
	CHECK(s.order(pre) == std::vector<DeviceInstanceId> { s.pre1, lfo });
	CHECK(s.order(post) == std::vector<DeviceInstanceId> { s.post[0], s.post[1], s.post[2] });
	CHECK(post->routeAmount(backplane::Source::outerCore(0), moving) == doctest::Approx(0.3f));
	CHECK(post->routeAmount(out.asOuter(), moving) == doctest::Approx(0.4f));
	// AND REDO MOVES IT AGAIN.
	s.panel->historyForTest().redo();
	(void)s.panel->updateContent();
	CHECK(s.order(pre) == std::vector<DeviceInstanceId> { s.post[1], s.pre1, lfo });

	// PRE INTO POST, past POST's last card: the end of POST.
	s.panel->historyForTest().undo();
	(void)s.panel->updateContent();
	const visage::Bounds last = s.editor->cardBoundsForTest(s.post[2]);
	s.drag(s.pre1, { last.right() - 2.0f, last.y() + last.height() * 0.5f });
	CHECK(s.order(pre) == std::vector<DeviceInstanceId> { lfo });
	CHECK(s.order(post) == std::vector<DeviceInstanceId> { s.post[0], s.post[1], s.post[2], s.pre1 });
	s.panel->historyForTest().undo();
	CHECK(s.order(pre) == std::vector<DeviceInstanceId> { s.pre1, lfo });
	CHECK(s.order(post) == std::vector<DeviceInstanceId> { s.post[0], s.post[1], s.post[2] });
}

TEST_CASE("NZ: a veiled PRE takes no drop — a POST card held over it names no gap while the device runs in the voices") {
	SitePanel s;
	s.h.engine.setVoices(2);
	const auto poly = static_cast<size_t>(fx::filter::Param::Poly);
	s.h.bridge.setField(s.id, poly, 1.0f);
	REQUIRE(s.h.bridge.runsPerVoice(s.id));
	(void)s.panel->updateContent();
	REQUIRE_FALSE(s.editor->preAvailableForTest());
	const visage::Bounds b = s.editor->cardBoundsForTest(s.pre1);
	s.drag(s.post[0], { b.x() + 2.0f, b.y() + b.height() * 0.5f });
	CHECK(s.order(s.site->pre().bridge()) == std::vector<DeviceInstanceId> { s.pre1 });
	CHECK(s.order(s.site->post().bridge()).size() == 3u);
}

//------------------------------------------------------------------------------------------------//
// OA: A SITE'S POST SELECTS — press, Ctrl, Shift, the lasso, Group, Duplicate and the block drag     //
//------------------------------------------------------------------------------------------------//

namespace {

/// A HAND'S PRESS ON A CARD'S OWN FRAME: the focus road first, then the press with the modifier, then the release.
void pressSiteCard(rack::RackEditor& editor, DeviceInstanceId device, int modifiers) {
	ui::FxCard* card = editor.cardOfDeviceForTest(device);
	REQUIRE(card != nullptr);
	card->onCardPressed();
	visage::MouseEvent e {};
	e.position = { static_cast<float>(card->width()) * 0.5f, static_cast<float>(card->height()) * 0.5f };
	e.modifiers = modifiers;
	e.repeat_click_count = 1;
	card->noteCardPress(e);
	card->noteCardRelease(e);
}

} // namespace

TEST_CASE("OA: a site's POST cards select — Ctrl adds, Shift runs, each selected card draws the flag, and a selection in "
          "PRE or POST clears the other") {
	SitePanel s;
	rack::RackEditor& e = *s.editor;
	ui::LaneSelection* post = e.postSelectionForTest();
	REQUIRE(post != nullptr);
	const auto flag = [&e](DeviceInstanceId d) { return e.cardOfDeviceForTest(d)->headFocused(); };

	pressSiteCard(e, s.post[0], 0);
	pressSiteCard(e, s.post[2], visage::kModifierRegCtrl);
	CHECK(post->selection().inOrder(s.post) == std::vector<DeviceInstanceId> { s.post[0], s.post[2] });
	CHECK(flag(s.post[0]));
	CHECK_FALSE(flag(s.post[1]));
	CHECK(flag(s.post[2]));
	CHECK(post->escapeHeldForTest()); // two or more: one Esc entry
	pressSiteCard(e, s.post[0], 0);
	pressSiteCard(e, s.post[2], visage::kModifierShift);
	CHECK(post->selection().inOrder(s.post) == s.post);

	// A PRESS IN PRE TAKES THE SELECTION THERE, and POST's goes; and back again.
	pressSiteCard(e, s.pre1, 0);
	CHECK(e.selectionForTest().selection().ids() == std::vector<DeviceInstanceId> { s.pre1 });
	CHECK(post->selection().empty());
	CHECK(flag(s.pre1));
	CHECK_FALSE(flag(s.post[0]));
	pressSiteCard(e, s.post[1], 0);
	CHECK(post->selection().ids() == std::vector<DeviceInstanceId> { s.post[1] });
	CHECK(e.selectionForTest().selection().empty());
	CHECK_FALSE(flag(s.pre1));
	CHECK(e.focusedDeviceForTest() == s.post[1]);
}

TEST_CASE("OA: the lasso over a site's POST selects POST's cards it touches, and keeps to the chain it touched first") {
	SitePanel s;
	rack::RackEditor& e = *s.editor;
	ui::LaneSelection* post = e.postSelectionForTest();
	REQUIRE(post != nullptr);
	const visage::Bounds p0 = e.cardBoundsForTest(s.post[0]);
	const visage::Bounds p1 = e.cardBoundsForTest(s.post[1]);
	const visage::Bounds pre = e.cardBoundsForTest(s.pre1);
	const float y = p0.y() + p0.height() * 0.5f;
	const auto mouse = [](visage::Point at) {
		visage::MouseEvent m {};
		m.position = at;
		return m;
	};
	// FROM THE GAP BEFORE POST'S FIRST CARD INTO ITS SECOND.
	e.onMouseDown().callback(mouse({ p0.x() - 1.0f, y }));
	e.onMouseDrag().callback(mouse({ p1.x() + 4.0f, y }));
	e.onMouseUp().callback(mouse({ p1.x() + 4.0f, y }));
	CHECK(post->selection().inOrder(s.post) == std::vector<DeviceInstanceId> { s.post[0], s.post[1] });
	CHECK(e.selectionForTest().selection().empty());
	// A LASSO STARTED IN POST AND DRAWN BACK OVER PRE stays POST's: it touched POST first.
	e.onMouseDown().callback(mouse({ p1.x() + 4.0f, y }));
	e.onMouseDrag().callback(mouse({ p1.x() + 2.0f, y }));
	e.onMouseDrag().callback(mouse({ pre.x() + 4.0f, y }));
	e.onMouseUp().callback(mouse({ pre.x() + 4.0f, y }));
	CHECK(e.selectionForTest().selection().empty());
	CHECK(post->selection().contains(s.post[0]));
	CHECK(post->selection().contains(s.post[1]));
}

TEST_CASE("OA: a site's POST selection groups into a Chain in place, duplicates with Ctrl+D, and drags as one block — "
          "each one undo step in the panel's history") {
	SitePanel s;
	rack::RackEditor& e = *s.editor;
	rack::RackBridge* postLane = s.site->post().bridge();
	ui::LaneSelection* post = e.postSelectionForTest();
	REQUIRE(post != nullptr);
	ui::EditHistory& history = s.panel->historyForTest();
	REQUIRE_FALSE(history.canUndo()); // nothing recorded yet, so each verb below is exactly the one step it undoes

	// GROUP: post[0] and post[1] become one Chain at their place, and it is the selection.
	pressSiteCard(e, s.post[0], 0);
	pressSiteCard(e, s.post[1], visage::kModifierShift);
	REQUIRE(e.slotVerbForTest(s.post[0], ui::FragmentVerb::GroupChain));
	(void)s.panel->updateContent();
	REQUIRE(postLane->chainLength() == 2u);
	const DeviceInstanceId chain = postLane->deviceAtPosition(0);
	CHECK(device::sameTag(postLane->descriptorOf(chain)->traits.tag, composite::kTraits.tag));
	CHECK(postLane->deviceAtPosition(1) == s.post[2]);
	CHECK(post->selection().ids() == std::vector<DeviceInstanceId> { chain });
	CHECK(history.canUndo()); // ONE step: the single undo below empties it
	CHECK(s.order(s.site->pre().bridge()) == std::vector<DeviceInstanceId> { s.pre1 }); // PRE untouched
	history.undo();
	CHECK_FALSE(history.canUndo());
	(void)s.panel->updateContent();
	CHECK(s.order(postLane) == s.post);

	// DUPLICATE: Ctrl+D over POST's selection copies post[2] right after it.
	pressSiteCard(e, s.post[2], 0);
	CHECK(e.onKeyPress().callback(visage::KeyEvent { visage::KeyCode::D, visage::kModifierRegCtrl, true }));
	(void)s.panel->updateContent();
	REQUIRE(postLane->chainLength() == 4u);
	CHECK(s.order(postLane)[2] == s.post[2]);
	CHECK(std::find(s.post.begin(), s.post.end(), postLane->deviceAtPosition(3)) == s.post.end());
	CHECK(history.canUndo()); // ONE step: the single undo below empties it
	CHECK(s.order(s.site->pre().bridge()) == std::vector<DeviceInstanceId> { s.pre1 });
	history.undo();
	CHECK_FALSE(history.canUndo());
	(void)s.panel->updateContent();
	CHECK(s.order(postLane) == s.post);

	// THE BLOCK DRAG: post[1] and post[2] held by post[2] and let go before post[0] land there, in order.
	pressSiteCard(e, s.post[1], 0);
	pressSiteCard(e, s.post[2], visage::kModifierShift);
	s.drag(s.post[2], s.before(s.post[0]));
	CHECK(s.order(postLane) == std::vector<DeviceInstanceId> { s.post[1], s.post[2], s.post[0] });
	CHECK(history.canUndo()); // ONE step: the single undo below empties it
	history.undo();
	CHECK_FALSE(history.canUndo());
	CHECK(s.order(postLane) == s.post);
}

//------------------------------------------------------------------------------------------------//
// NZ (b): AN INNER MODULATOR ARMED IN A CHAIN'S OR A RACK'S PANEL LIGHTS WHAT FOLLOWS THE CONTAINER  //
//------------------------------------------------------------------------------------------------//

TEST_CASE("NZ (b): an LFO armed inside a Chain's panel lights the host's device AFTER the chain, not the one before, "
          "through a borrowed arm spelled as the chain's export, and disarming puts the host's state back") {
	rack::registerChainExpandedView();
	SiteHost h;
	const DeviceInstanceId before = h.bridge.addDevice(fx::filter::kTraits.tag);
	const DeviceInstanceId chain = h.bridge.addDevice(composite::kTraits.tag);
	const DeviceInstanceId after = h.bridge.addDevice(fx::filter::kTraits.tag);
	REQUIRE(after != kNoInstance);
	rack::RackBridge* inner = rack::bridgeOf(h.bridge.containerSurfaceOf(chain));
	REQUIRE(inner != nullptr);
	const DeviceInstanceId lfo = inner->addDevice(mod::lfo::kTraits.tag);
	REQUIRE(lfo != kNoInstance);
	// THE HOST'S STATE OUTLIVES THE PANEL: its widgets watch it, and unwatch as the panel goes.
	ui::ModState host;
	ui::EditHistory history;
	ui::WidgetRegistry registry;
	auto view = ui::makeExpandedView(h.bridge, nullptr, registry, chain, *h.bridge.descriptorOf(chain));
	auto* panel = dynamic_cast<rack::ChainExpanded*>(view.get());
	REQUIRE(panel != nullptr);
	panel->modulationBound(&host, &history);
	(void)panel->updateContent();
	REQUIRE(panel->liveForTest());

	const auto cutoff = static_cast<size_t>(fx::filter::Param::Cutoff);
	ui::DeviceControl afterKnob { h.bridge, after, fx::filter::kDescriptor, cutoff, &host, nullptr };
	ui::DeviceControl beforeKnob { h.bridge, before, fx::filter::kDescriptor, cutoff, &host, nullptr };
	const device::Source was = host.source();
	CHECK_FALSE(host.armed());
	CHECK_FALSE(afterKnob.display().routable);

	// ARMED IN THE CHAIN'S PANEL, AND THE TICK RELAYS IT as the chain's export: the device after lights, the one before
	// does not (the graph's "after").
	panel->modForTest().focus(backplane::Source::output(lfo, 0));
	panel->modForTest().arm(true);
	CHECK(panel->updateContent());
	const backplane::Source out = h.bridge.containerExport(chain, backplane::Source::output(lfo, 0));
	REQUIRE(out.isExport());
	CHECK(host.source() == out);
	CHECK(host.armed());
	CHECK(afterKnob.display().routable);
	CHECK_FALSE(beforeKnob.display().routable);
	CHECK_FALSE(panel->relayArmForTest()); // a second tick with nothing new moves nothing

	// DISARMED IN THE PANEL: the host's state is put back as it was found.
	panel->modForTest().arm(false);
	CHECK(panel->relayArmForTest());
	CHECK_FALSE(host.armed());
	CHECK(host.source() == was);
	CHECK_FALSE(afterKnob.display().routable);

	// AND THE PANEL TAKES ITS RELAY WITH IT when it goes while armed.
	panel->modForTest().focus(backplane::Source::output(lfo, 0));
	panel->modForTest().arm(true);
	CHECK(panel->relayArmForTest());
	REQUIRE(host.armed());
	view.reset();
	CHECK_FALSE(host.armed());
	CHECK(host.source() == was);
}

TEST_CASE("NZ (b): an LFO armed in one of a Rack's chain rails lights the host's device AFTER the rack, spelled as the "
          "rack's export, and disarming puts the host's state back") {
	SiteHost h;
	const DeviceInstanceId rackId = h.bridge.addDevice(rack::parallel::kTraits.tag);
	const DeviceInstanceId after = h.bridge.addDevice(fx::filter::kTraits.tag);
	REQUIRE(after != kNoInstance);
	auto* rk = rack::parallel::rackOf(h.bridge.containerSurfaceOf(rackId));
	REQUIRE(rk != nullptr);
	if (rk->chainCount() == 0) {
		(void)rk->addChain();
	}
	rack::RackBridge* inner = rk->chainAt(0)->bridge();
	REQUIRE(inner != nullptr);
	const DeviceInstanceId lfo = inner->addDevice(mod::lfo::kTraits.tag);
	REQUIRE(lfo != kNoInstance);
	// THE HOST'S STATE OUTLIVES THE PANEL: its widgets watch it, and unwatch as the panel goes.
	ui::ModState host;
	ui::EditHistory history;
	ui::WidgetRegistry registry;
	auto view = ui::makeExpandedView(h.bridge, nullptr, registry, rackId, *h.bridge.descriptorOf(rackId));
	auto* panel = dynamic_cast<ui::ParallelRackExpanded*>(view.get());
	REQUIRE(panel != nullptr);
	panel->modulationBound(&host, &history);
	(void)panel->updateContent();
	REQUIRE(panel->railCountForTest() >= 1);

	const auto cutoff = static_cast<size_t>(fx::filter::Param::Cutoff);
	ui::DeviceControl afterKnob { h.bridge, after, fx::filter::kDescriptor, cutoff, &host, nullptr };
	const device::Source was = host.source();
	CHECK_FALSE(afterKnob.display().routable);

	ui::ModState& rail = panel->railForTest(0).modForTest();
	rail.focus(backplane::Source::output(lfo, 0));
	rail.arm(true);
	CHECK(panel->relayArmForTest());
	const backplane::Source out = h.bridge.containerExport(rackId, backplane::Source::output(lfo, 0));
	REQUIRE(out.isExport());
	CHECK(host.source() == out);
	CHECK(host.armed());
	CHECK(afterKnob.display().routable);

	rail.arm(false);
	CHECK(panel->relayArmForTest());
	CHECK_FALSE(host.armed());
	CHECK(host.source() == was);
	CHECK_FALSE(afterKnob.display().routable);
}

TEST_CASE("NZ (b): under a borrowed PRE arm a Chain's open panel after the holder lights its inner device — the PRE "
          "export reaches inside the chain, and the chain's state follows the host's") {
	rack::registerChainExpandedView();
	SiteHost h;
	const DeviceInstanceId holder = h.bridge.addDevice(fx::filter::kTraits.tag);
	const DeviceInstanceId chain = h.bridge.addDevice(composite::kTraits.tag);
	REQUIRE(h.bridge.openSite(holder));
	auto* site = static_cast<rack::SiteChains*>(h.bridge.containerSurfaceOf(siteKeyOf(holder)));
	REQUIRE(site != nullptr);
	const DeviceInstanceId lfo = site->pre().bridge()->addDevice(mod::lfo::kTraits.tag);
	REQUIRE(lfo != kNoInstance);
	rack::RackBridge* inner = rack::bridgeOf(h.bridge.containerSurfaceOf(chain));
	REQUIRE(inner != nullptr);
	const DeviceInstanceId innerFilter = inner->addDevice(fx::filter::kTraits.tag);
	REQUIRE(innerFilter != kNoInstance);

	ui::ModState host;
	ui::EditHistory history;
	ui::WidgetRegistry registry;
	auto siteView = ui::makeExpandedView(h.bridge, nullptr, registry, siteKeyOf(holder), ui::siteViewDescriptor());
	auto* sitePanel = dynamic_cast<rack::SiteExpanded*>(siteView.get());
	REQUIRE(sitePanel != nullptr);
	sitePanel->modulationBound(&host, &history);
	(void)sitePanel->updateContent();
	auto chainView = ui::makeExpandedView(h.bridge, nullptr, registry, chain, *h.bridge.descriptorOf(chain));
	auto* chainPanel = dynamic_cast<rack::ChainExpanded*>(chainView.get());
	REQUIRE(chainPanel != nullptr);
	chainPanel->modulationBound(&host, &history);
	(void)chainPanel->updateContent();
	REQUIRE(chainPanel->liveForTest());

	const auto cutoff = static_cast<size_t>(fx::filter::Param::Cutoff);
	ui::DeviceControl innerKnob { *inner, innerFilter, fx::filter::kDescriptor, cutoff, &chainPanel->modForTest(), nullptr };
	CHECK_FALSE(innerKnob.display().routable);

	// ARMED IN PRE: the host's state borrows the export, and the chain's state, a child of the host's, follows it.
	sitePanel->preModForTest().focus(backplane::Source::output(lfo, 0));
	sitePanel->preModForTest().arm(true);
	CHECK(sitePanel->relayArmForTest());
	const backplane::Source out = h.bridge.siteExport(holder, backplane::Source::output(lfo, 0));
	REQUIRE(out.isExport());
	CHECK(host.source() == out);
	CHECK(chainPanel->modForTest().source() == out.asOuter());
	CHECK(innerKnob.display().routable);
	// AND THE CHAIN'S OWN RELAY DOES NOT FIGHT IT: nothing is armed locally in the chain.
	CHECK_FALSE(chainPanel->relayArmForTest());
	CHECK(host.source() == out);

	sitePanel->preModForTest().arm(false);
	CHECK(sitePanel->relayArmForTest());
	CHECK_FALSE(host.armed());
	CHECK_FALSE(innerKnob.display().routable);
}

//------------------------------------------------------------------------------------------------//
// NR: THE HOST'S ROUTE LIST WALKS ITS DEVICES' SITES — a host source's routes into PRE and POST       //
//------------------------------------------------------------------------------------------------//

TEST_CASE("NR: the rack's route list walks a device's site — a host macro's routes into PRE and POST are listed, named "
          "by holder and lane, read and written through the site's own table") {
	SitePanel s;
	rack::RackBridge* pre = s.site->pre().bridge();
	rack::RackBridge* post = s.site->post().bridge();
	const auto cutoff = static_cast<size_t>(fx::filter::Param::Cutoff);
	const backplane::Destination intoPost = backplane::Destination::control(s.post[1], cutoff);
	const backplane::Destination intoPre = backplane::Destination::control(s.pre1, cutoff);
	post->setRouteAmount(backplane::Source::outerCore(0), intoPost, 0.3f);
	pre->setRouteAmount(backplane::Source::outerCore(0), intoPre, -0.2f);
	REQUIRE(post->routeAmount(backplane::Source::outerCore(0), intoPost) == doctest::Approx(0.3f));
	REQUIRE(pre->routeAmount(backplane::Source::outerCore(0), intoPre) == doctest::Approx(-0.2f));
	const DeviceInstanceId key = siteKeyOf(s.id);

	// LISTED, under the site's key, with their amounts.
	const std::vector<backplane::DeviceHost::InnerRoute> rows = s.h.bridge.innerRoutesFrom(backplane::Source::core(0));
	REQUIRE(rows.size() == 2u);
	const auto find = [&rows](backplane::Destination d) -> const backplane::DeviceHost::InnerRoute* {
		for (const auto& r : rows) {
			if (r.destination == d) {
				return &r;
			}
		}
		return nullptr;
	};
	REQUIRE(find(intoPost) != nullptr);
	REQUIRE(find(intoPre) != nullptr);
	CHECK(find(intoPost)->chain == key);
	CHECK(find(intoPost)->amount == doctest::Approx(0.3f));
	CHECK(find(intoPre)->amount == doctest::Approx(-0.2f));
	// A SOURCE THAT ROUTES NOWHERE IN THE SITE lists nothing.
	CHECK(s.h.bridge.innerRoutesFrom(backplane::Source::core(1)).empty());

	// READ AND WRITTEN through the host's verbs, landing in the lane.
	CHECK(s.h.bridge.innerRouteAmount(backplane::Source::core(0), key, intoPost) == doctest::Approx(0.3f));
	CHECK(s.h.bridge.setInnerRouteAmount(backplane::Source::core(0), key, intoPost, 0.5f));
	CHECK(post->routeAmount(backplane::Source::outerCore(0), intoPost) == doctest::Approx(0.5f));
	CHECK(s.h.bridge.setInnerRouteTransfer(backplane::Source::core(0), key, intoPost, backplane::Transfer::Exp));
	CHECK(s.h.bridge.innerRouteTransfer(backplane::Source::core(0), key, intoPost) == backplane::Transfer::Exp);

	// NAMED BY THE HOLDER, ITS PLACE AND THE LANE.
	const std::string control = fx::filter::kDescriptor.params[cutoff].name;
	CHECK(s.h.bridge.innerDestinationName(key, intoPost) == "Filter 1 POST · Filter · " + control);
	CHECK(s.h.bridge.innerDestinationName(key, intoPre) == "Filter 1 PRE · Filter · " + control);

	// AND THE LIST A HAND SEES — the rack's route list with macro 1 armed — draws both rows.
	ui::ModState mod;
	ui::EditHistory history;
	ui::RouteList list { s.h.bridge, mod, history };
	mod.focus(backplane::Source::core(0));
	const std::vector<ui::RouteList::Row> drawn = list.allRows();
	CHECK(std::find(drawn.begin(), drawn.end(), ui::RouteList::Row { key, intoPost }) != drawn.end());
	CHECK(std::find(drawn.begin(), drawn.end(), ui::RouteList::Row { key, intoPre }) != drawn.end());
	CHECK(list.nameOfForTest(ui::RouteList::Row { key, intoPost }) == "Filter 1 POST · Filter · " + control);
}

TEST_CASE("NR: Feathers' route list walks an fx-lane device's site — a host source's route into its POST is listed, "
          "read, written and named through the bridge") {
	FilterFixture f;
	const DeviceInstanceId holder = f.card->device();
	REQUIRE(f.bridge.openFxSite(holder));
	// NO SYNTH IN THIS FIXTURE, so the site is built as the converger builds it and filed where the bridge finds it.
	auto site = std::make_unique<rack::SiteChains>(holder, hostCatalogue(), 48000.0, 4096);
	site->setOuterGate(rack::ChainRecord::gateOver(&f.bridge.chainRecord(), holder));
	const DeviceInstanceId inPost = site->post().bridge()->addDevice(fx::filter::kTraits.tag);
	REQUIRE(inPost != kNoInstance);
	f.bridge.fxBodies().attach(siteKeyOf(holder), site.get());
	const auto cutoff = static_cast<size_t>(fx::filter::Param::Cutoff);
	const backplane::Destination d = backplane::Destination::control(inPost, cutoff);
	site->post().bridge()->setRouteAmount(backplane::Source::outerCore(0), d, 0.4f);
	REQUIRE(site->post().bridge()->routeAmount(backplane::Source::outerCore(0), d) == doctest::Approx(0.4f));
	const DeviceInstanceId key = siteKeyOf(holder);

	const std::vector<backplane::DeviceHost::InnerRoute> rows = f.bridge.innerRoutesFrom(backplane::Source::core(0));
	REQUIRE(rows.size() == 1u);
	CHECK(rows[0].chain == key);
	CHECK(rows[0].destination == d);
	CHECK(rows[0].amount == doctest::Approx(0.4f));
	CHECK(f.bridge.innerRouteAmount(backplane::Source::core(0), key, d) == doctest::Approx(0.4f));
	CHECK(f.bridge.setInnerRouteAmount(backplane::Source::core(0), key, d, -0.1f));
	CHECK(site->post().bridge()->routeAmount(backplane::Source::outerCore(0), d) == doctest::Approx(-0.1f));
	const size_t at = f.bridge.fxSlotOf(holder);
	REQUIRE(at != kNoPosition);
	CHECK(f.bridge.innerDestinationName(key, d)
	      == "Filter " + std::to_string(at + 1) + " POST · Filter · " + fx::filter::kDescriptor.params[cutoff].name);
	f.bridge.fxBodies().attach(key, nullptr);
}

// ---------------------------------------------------------------------------------------------
// THE AUDITION OF 2026-10-29 — THE GLOW AT EVERY DEPTH. The host here plays the ENCLOSING CHAIN's inside (a top
// RackBridge whose core 0 is its Macro 1): a Filter holding a site, a Rack and a Filter in PRE, a Filter in POST, and a
// Filter in the PRE Rack's first chain. Macro 1 armed in the Chain's editor state; each depth asked whether its control
// volunteers, read as the editor reads it — through its own host and the state its surface binds.
// ---------------------------------------------------------------------------------------------

TEST_CASE("Audition 2026-10-29: an enclosing Chain's macro armed lights the PRE Rack's Mix, Gain and macros, a PRE and a "
          "POST Filter, the holder, the PRE Rack's own window, and a Filter inside the PRE Rack's chain — two hosts out, "
          "lit exactly where a drag routes") {
	rack::registerChainExpandedView();
	SiteHost h;
	const DeviceInstanceId id = h.bridge.addDevice(fx::filter::kTraits.tag);
	REQUIRE(h.bridge.openSite(id));
	auto* site = static_cast<rack::SiteChains*>(h.bridge.containerSurfaceOf(siteKeyOf(id)));
	REQUIRE(site != nullptr);
	rack::RackBridge* pre = site->pre().bridge();
	rack::RackBridge* post = site->post().bridge();
	const DeviceInstanceId preRack = pre->addDevice(rack::parallel::kTraits.tag);
	const DeviceInstanceId preFilter = pre->addDevice(fx::filter::kTraits.tag);
	const DeviceInstanceId postFilter = post->addDevice(fx::filter::kTraits.tag);
	REQUIRE(preRack != kNoInstance);
	REQUIRE(preFilter != kNoInstance);
	REQUIRE(postFilter != kNoInstance);
	// THE STATE AND HISTORY OUTLIVE THE PANELS BOUND TO THEM (declared first): a panel's relay lets its borrowed arm go
	// at destruction, through the state it was handed.
	ui::ModState chainMod; // the enclosing Chain's editor state
	ui::EditHistory history;
	ui::WidgetRegistry registry;
	std::unique_ptr<ui::ExpandedView> view =
	    ui::makeExpandedView(h.bridge, nullptr, registry, siteKeyOf(id), ui::siteViewDescriptor());
	auto* panel = dynamic_cast<rack::SiteExpanded*>(view.get());
	REQUIRE(panel != nullptr);
	panel->modulationBound(&chainMod, &history);
	chainMod.focus(backplane::Source::core(0));
	chainMod.arm(true);

	const auto cutoff = static_cast<size_t>(fx::filter::Param::Cutoff);
	const auto& rackDesc = rack::parallel::kDescriptor;
	// DEPTH 1 — the site panel: the holder (centre), PRE's Rack and Filter, POST's Filter.
	ui::DeviceControl centre { h.bridge, id, fx::filter::kDescriptor, cutoff, &panel->centreModForTest(), nullptr };
	ui::DeviceControl preCut { *pre, preFilter, fx::filter::kDescriptor, cutoff, &panel->preModForTest(), nullptr };
	ui::DeviceControl postCut { *post, postFilter, fx::filter::kDescriptor, cutoff, &panel->postModForTest(), nullptr };
	CHECK(centre.display().routable);
	CHECK(preCut.display().routable);
	CHECK(postCut.display().routable);
	for (const auto p : { rack::parallel::Param::Mix, rack::parallel::Param::Gain, rack::parallel::Param::Macro1 }) {
		ui::DeviceControl c { *pre, preRack, rackDesc, static_cast<size_t>(p), &panel->preModForTest(), nullptr };
		INFO("PRE Rack control " << static_cast<size_t>(p));
		CHECK(c.display().routable);
	}

	// DEPTH 2 — the PRE Rack's own window, bound to PRE's state as its card's ⤢ binds it.
	std::unique_ptr<ui::ExpandedView> rackView = ui::makeExpandedView(*pre, nullptr, registry, preRack, rackDesc);
	auto* rackPanel = dynamic_cast<ui::ParallelRackExpanded*>(rackView.get());
	REQUIRE(rackPanel != nullptr);
	rackPanel->modulationBound(&panel->preModForTest(), &history);
	for (const auto p : { rack::parallel::Param::Mix, rack::parallel::Param::Gain, rack::parallel::Param::Macro1 }) {
		ui::DeviceControl c { *pre, preRack, rackDesc, static_cast<size_t>(p), &panel->preModForTest(), nullptr };
		INFO("PRE Rack window control " << static_cast<size_t>(p));
		CHECK(c.display().routable);
	}
	// DEPTH 3 — a Filter in the PRE Rack's own chain, read through that rail's own state. The Chain's Macro 1 is TWO hosts
	// out from there (the Rack's chain → PRE → the Chain), and since sources reach any depth it is
	// spelled so: lit, and the drag routes THAT macro — not PRE's own Macro 1, which the one-hop spelling named.
	auto* rk = rack::parallel::rackOf(pre->containerSurfaceOf(preRack));
	REQUIRE(rk != nullptr);
	if (rk->chainCount() == 0) {
		(void)rk->addChain();
	}
	rack::RackBridge* deep = rk->chainAt(0)->bridge();
	REQUIRE(deep != nullptr);
	const DeviceInstanceId deepFilter = deep->addDevice(fx::filter::kTraits.tag);
	REQUIRE(deepFilter != kNoInstance);
	rackPanel->setBounds(0, 0, 900, 500);
	(void)rackPanel->updateContent();
	REQUIRE(rackPanel->railCountForTest() >= 1);
	ui::FxLaneRail& rail = rackPanel->railForTest(0);
	const backplane::Source spelled = rail.modForTest().source();
	CHECK(spelled.outerHops() == 2);
	CHECK(spelled.inner().inner() == backplane::Source::core(0)); // the CHAIN's Macro 1
	ui::DeviceControl deepCut { *deep, deepFilter, fx::filter::kDescriptor, cutoff, &rail.modForTest(), nullptr };
	CHECK(deepCut.display().routable);
	// LIT EXACTLY WHERE A DRAG ROUTES: the same source, the same answer, and the route is made.
	const auto toDeep = backplane::Destination::control(deepFilter, cutoff);
	CHECK(deep->mayRoute(spelled, toDeep));
	deep->setRouteAmount(spelled, toDeep, 0.25f);
	CHECK(deep->routeAmount(spelled, toDeep) == 0.25f);
}

TEST_CASE("Audition 2026-10-29, ASSEMBLED: the Chain's window, a Filter in it whose site's PRE holds a Rack — the Chain's "
          "Macro 1 armed in its window lights the PRE Rack's Mix, Gain and Macro 1, and a route from it moves the Mix") {
	rack::registerChainExpandedView();
	// THE CHAIN'S ENGINE OUTLIVES THE EDITOR (declared first): the editor's panels pin and unpin surfaces inside it.
	rack::RackEngine outer { rack::kRackAxes, hostCatalogue() };
	outer.prepare(48000.0, 4096);
	ModFixture f;
	auto* lane = findInTree<ui::FxLane>(f.root);
	REQUIRE(lane != nullptr);
	const size_t slot = f.bridge.addFxDevice(cmp::kTraits.tag);
	REQUIRE(slot < kMaxFxSlots);
	lane->resized();
	const DeviceInstanceId chain = f.bridge.fxDeviceAt(slot);
	const size_t lane2 = outer.addDevice(cmp::kTraits.tag);
	REQUIRE(lane2 != rack::RackEngine::kNoSlot);
	void* storage = outer.slotAtLane(lane2).storage.get();
	rack::RackBridge* inner = rack::bridgeOf(storage);
	REQUIRE(inner != nullptr);
	const DeviceInstanceId holder = inner->addDevice(fx::filter::kTraits.tag);
	REQUIRE(holder != kNoInstance);
	inner->drain();
	f.bridge.fxBodies().attach(chain, storage);

	ui::ExpandedCanvas* canvas = f.gui.canvasForTest();
	REQUIRE(canvas != nullptr);
	REQUIRE(canvas->setCollapsed(false));
	f.grantPreferredSize();
	lane->onExpandDevice(slot);
	auto* view = findInTree<rack::ChainExpanded>(f.root);
	REQUIRE(view != nullptr);
	(void)view->updateContent();
	rack::RackEditor* editor = view->editorForTest();
	REQUIRE(editor != nullptr);
	editor->syncCards();
	ui::FxCard* card = nullptr;
	for (ui::FxCard* c : editor->cardsForTest()) {
		card = c != nullptr && c->device() == holder ? c : card;
	}
	REQUIRE(card != nullptr);
	REQUIRE(card->onSite);
	card->onSite();
	auto* panel = findInTree<rack::SiteExpanded>(f.root);
	REQUIRE(panel != nullptr);
	(void)panel->updateContent();
	auto* site = static_cast<rack::SiteChains*>(inner->containerSurfaceOf(siteKeyOf(holder)));
	REQUIRE(site != nullptr);
	rack::RackBridge* pre = site->pre().bridge();
	const DeviceInstanceId preRack = pre->addDevice(rack::parallel::kTraits.tag);
	REQUIRE(preRack != kNoInstance);
	(void)panel->updateContent();

	// THE CHAIN'S MACRO 1, ARMED WHERE A HAND ARMS IT: its window's own macro arrow (`SourceArm(mod_, core(0))`).
	view->modForTest().focus(backplane::Source::core(0));
	view->modForTest().arm(true);
	(void)view->updateContent();
	(void)panel->updateContent();
	const auto& rackDesc = rack::parallel::kDescriptor;
	for (const auto p : { rack::parallel::Param::Mix, rack::parallel::Param::Gain, rack::parallel::Param::Macro1 }) {
		ui::DeviceControl c { *pre, preRack, rackDesc, static_cast<size_t>(p), &panel->preModForTest(), nullptr };
		INFO("PRE Rack control " << static_cast<size_t>(p) << " source " << panel->preModForTest().source().device << "/"
		                         << panel->preModForTest().source().index);
		CHECK(panel->preModForTest().armed());
		CHECK(c.display().routable);
	}
	// AND THE CARD ITSELF, AS DRAWN IN THE SITE PANEL: every control the PRE Rack's card offers volunteers.
	rack::RackEditor* siteEditor = panel->editorForTest();
	REQUIRE(siteEditor != nullptr);
	siteEditor->syncCards();
	ui::FxCard* rackCard = nullptr;
	for (ui::FxCard* c : siteEditor->cardsForTest()) {
		rackCard = c != nullptr && c->device() == preRack ? c : rackCard;
	}
	REQUIRE(rackCard != nullptr);
	std::vector<ui::ControlFace::Offered> offered;
	std::function<void(visage::Frame*)> walk = [&](visage::Frame* fr) {
		if (auto* face = dynamic_cast<const ui::ControlFace*>(fr)) {
			face->offeredControls(offered);
		}
		for (visage::Frame* child : fr->children()) {
			walk(child);
		}
	};
	walk(rackCard);
	size_t lit = 0;
	size_t modulatable = 0;
	for (const auto& o : offered) {
		if (o.control != nullptr && o.control->display().isDestination) {
			++modulatable;
			lit += o.control->display().routable ? 1 : 0;
		}
	}
	MESSAGE("PRE Rack card: " << lit << " of " << modulatable << " modulatable controls lit");
	CHECK(modulatable > 0);
	CHECK(lit == modulatable);
	view->modForTest().arm(false);
	f.gui.expandedForTest()->dismiss();
	f.bridge.fxBodies().attach(chain, nullptr);
}

namespace {

/// EVERY PANEL IN THE TREE watching `device` in `nest` — what a press of that device's ⤢ must leave standing.
size_t panelsFor(visage::Frame* root, DeviceInstanceId device, ui::DeviceNest nest) {
	size_t n = 0;
	if (auto* v = dynamic_cast<ui::ExpandedView*>(root); v != nullptr && v->watchedDevice() == device && v->nest() == nest) {
		++n;
	}
	for (visage::Frame* child : root->children()) {
		n += panelsFor(child, device, nest);
	}
	return n;
}

/// THE AUDITION'S SHAPE, ASSEMBLED: Feathers' lane → a Chain, its window open → a Filter in it, with a site per `mode`
/// (0 none, 1 empty, 2 a Rack in PRE), the site's panel open when it has one. Then the Filter's own ⤢.
size_t expandNestedHolder(int mode) {
	rack::registerChainExpandedView();
	rack::RackEngine outer { rack::kRackAxes, hostCatalogue() }; // outlives the editor, as above
	outer.prepare(48000.0, 4096);
	ModFixture f;
	auto* lane = findInTree<ui::FxLane>(f.root);
	REQUIRE(lane != nullptr);
	const size_t slot = f.bridge.addFxDevice(cmp::kTraits.tag);
	lane->resized();
	const DeviceInstanceId chain = f.bridge.fxDeviceAt(slot);
	void* storage = outer.slotAtLane(outer.addDevice(cmp::kTraits.tag)).storage.get();
	rack::RackBridge* inner = rack::bridgeOf(storage);
	REQUIRE(inner != nullptr);
	const DeviceInstanceId holder = inner->addDevice(fx::filter::kTraits.tag);
	inner->drain();
	f.bridge.fxBodies().attach(chain, storage);
	REQUIRE(f.gui.canvasForTest()->setCollapsed(false));
	f.grantPreferredSize();
	lane->onExpandDevice(slot);
	auto* view = findInTree<rack::ChainExpanded>(f.root);
	REQUIRE(view != nullptr);
	(void)view->updateContent();
	rack::RackEditor* editor = view->editorForTest();
	REQUIRE(editor != nullptr);
	editor->syncCards();
	ui::FxCard* card = nullptr;
	for (ui::FxCard* c : editor->cardsForTest()) {
		card = c != nullptr && c->device() == holder ? c : card;
	}
	REQUIRE(card != nullptr);
	if (mode > 0) {
		card->onSite();
		auto* panel = findInTree<rack::SiteExpanded>(f.root);
		REQUIRE(panel != nullptr);
		if (mode == 2) {
			auto* site = static_cast<rack::SiteChains*>(inner->containerSurfaceOf(siteKeyOf(holder)));
			REQUIRE(site != nullptr);
			REQUIRE(site->pre().bridge()->addDevice(rack::parallel::kTraits.tag) != kNoInstance);
		}
		(void)panel->updateContent();
		(void)f.gui.expandedForTest()->updateContent();
	}
	REQUIRE(card->onExpand);
	card->onExpand();
	(void)f.gui.expandedForTest()->updateContent();
	const size_t open = panelsFor(f.root, holder, view->innerNest());
	f.gui.expandedForTest()->dismiss();
	f.bridge.fxBodies().attach(chain, nullptr);
	return open;
}

} // namespace

TEST_CASE("Audition 2026-10-29: the ⤢ of a Filter inside a Chain opens the Filter's own window — with no site, an empty "
          "site, and a site whose PRE holds a Rack, each with the site's panel open") {
	for (int mode : { 0, 1, 2 }) {
		INFO("site mode " << mode);
		CHECK(expandNestedHolder(mode) == 1);
	}
}

TEST_CASE("JB: edits two levels in record into the plugin's ONE history — in a Chain inside a SKLITTER's post lane: an add, a value, a route, a move and a remove, each undone and redone by value") {
	// THE PLUGIN'S STATE ABOVE THE EDITOR IT IS HANDED TO, so it outlives every widget that watches it (frame_watchers.h's
	// ordering rule; the ASan gate's -UNDEBUG assert caught it the other way round, 2026-10-31).
	ui::ModState hostMod;
	ui::EditHistory history; // the plugin's one
	NestedThree n;
	n.sklitterPanel->setModulation(&hostMod, &history);
	(void)n.outer->updateContent();
	rack::RackEditor& ed = *n.chainPanel->editorForTest(); // two levels in: the lane's rail, then the Chain
	rack::RackBridge& inner = *n.chainPanel->bridgeForTest();
	// THE FORWARDING, ASKED: the Chain's window writes to the rail's history, and the rail's to the plugin's.
	REQUIRE(ed.historyForTest().forwardsTo() == &n.column->historyForTest());
	REQUIRE(n.column->historyForTest().forwardsTo() == &history);
	const auto order = [&] {
		std::vector<DeviceInstanceId> out;
		for (size_t p = 0; p < inner.chainLength(); ++p) {
			out.push_back(inner.deviceAtPosition(p));
		}
		return out;
	};
	const size_t start = history.length();
	const std::vector<DeviceInstanceId> before = order(); // the SKLITTER the fixture put there

	// ADD: one step in the plugin's history.
	const DeviceInstanceId filter = ed.addDeviceByTag(fx::filter::kTraits.tag);
	REQUIRE(filter != kNoInstance);
	CHECK(history.length() == start + 1);
	const std::vector<DeviceInstanceId> withFilter = order();
	REQUIRE(withFilter.size() == before.size() + 1);

	// A VALUE, through the control a card would hold, recording where the window records.
	const device::Descriptor& fd = fx::filter::kDescriptor;
	// THE FIRST CONTINUOUS DESTINATION — beside the enable, which is ordinal 0 since backlog KW and a stepped switch
	// (a 0.25 written to it is no value a hand could leave, and the record restores it as the switch it is).
	const size_t cutoff = fd.controlOfNthModulatableBesideEnable(0);
	REQUIRE(cutoff < fd.params.size());
	REQUIRE_FALSE(fd.params[cutoff].stepped);
	REQUIRE(std::string_view(fd.params[cutoff].key) == "filter_cutoff_wide"); // the cutoff, by its KEY
	ui::ModState innerMod;
	ui::DeviceControl control(inner, filter, fd, cutoff, &innerMod, &ed.historyForTest());
	const float base = inner.fieldAt(filter, cutoff);
	const float moved = static_cast<float>(fd.params[cutoff].min + 0.25 * (fd.params[cutoff].max - fd.params[cutoff].min));
	REQUIRE(moved != base);
	control.beginGesture();
	control.setValue(moved);
	control.endGesture();
	CHECK(inner.fieldAt(filter, cutoff) == doctest::Approx(moved));
	CHECK(history.length() == start + 2);

	// A ROUTE from the chain's Macro 1.
	const backplane::Source macro = backplane::Source::core(0);
	const backplane::Destination dest = backplane::Destination::control(filter, cutoff);
	innerMod.focus(macro);
	innerMod.arm(true);
	REQUIRE(inner.mayRoute(macro, dest));
	control.beginAmountGesture();
	control.setAmount(0.5f);
	control.endAmountGesture();
	CHECK(inner.routeAmount(macro, dest) == doctest::Approx(0.5f));
	CHECK(history.length() == start + 3);

	// A MOVE: the filter to the front.
	const DeviceInstanceId ids[] { filter };
	REQUIRE(ed.selectionForTest().verbs().moveBlock(ids, 0));
	REQUIRE(order().front() == filter);
	CHECK(history.length() == start + 4);

	// AND A NON-DEFAULT ENABLE (backlog KW), written to the record without a step: the remove's record must carry every
	// field by KEY, so the restore below is asked of all of them — the switch included.
	const auto enable = static_cast<size_t>(fd.traits.enableControl);
	inner.setField(filter, enable, 0.0f);
	REQUIRE(inner.fieldAt(filter, enable) == 0.0f);
	std::vector<float> fieldsBefore;
	for (size_t i = 0; i < fd.params.size(); ++i) {
		fieldsBefore.push_back(inner.fieldAt(filter, i));
	}

	// A REMOVE.
	ed.removeDevice(filter);
	REQUIRE(order() == before);
	CHECK(history.length() == start + 5);
	CHECK(ed.historyForTest().forwardsTo() != nullptr); // nothing stayed behind in the window's own

	// BACK, ONE STEP AT A TIME, BY VALUE.
	history.undo(); // the remove
	REQUIRE(order().size() == before.size() + 1);
	CHECK(order().front() == filter);
	CHECK(inner.fieldAt(filter, cutoff) == doctest::Approx(moved));
	CHECK(inner.routeAmount(macro, dest) == doctest::Approx(0.5f)); // it came back with its route
	for (size_t i = 0; i < fd.params.size(); ++i) {
		CAPTURE(std::string(fd.params[i].key));
		CHECK(inner.fieldAt(filter, i) == fieldsBefore[i]); // every field, by value, the enable's 0 among them
	}
	history.undo(); // the move
	CHECK(order() == withFilter);
	history.undo(); // the route
	CHECK(inner.routeAmount(macro, dest) == 0.0f);
	history.undo(); // the value
	CHECK(inner.fieldAt(filter, cutoff) == doctest::Approx(base));
	history.undo(); // the add
	CHECK(order() == before);

	// AND FORWARD AGAIN TO WHERE THE HAND LEFT IT.
	history.redo();
	CHECK(order() == withFilter);
	history.redo();
	CHECK(inner.fieldAt(filter, cutoff) == doctest::Approx(moved));
	history.redo();
	CHECK(inner.routeAmount(macro, dest) == doctest::Approx(0.5f));
	history.redo();
	CHECK(order().front() == filter);
	history.redo();
	CHECK(order() == before);
	CHECK_FALSE(history.canRedo());
}

TEST_CASE("JB: when the nested window that made them closes, its records in the plugin's history go dead — undo steps past them, and touches nothing") {
	ui::ModState hostMod; // above the editor it is handed to — see the case above
	ui::EditHistory history;
	NestedThree n;
	n.sklitterPanel->setModulation(&hostMod, &history);
	(void)n.outer->updateContent();
	rack::RackEditor& ed = *n.chainPanel->editorForTest();
	rack::RackBridge& inner = *n.chainPanel->bridgeForTest();
	const size_t start = history.length();
	const DeviceInstanceId filter = ed.addDeviceByTag(fx::filter::kTraits.tag);
	REQUIRE(filter != kNoInstance);
	REQUIRE(history.length() == start + 1);
	REQUIRE(history.canUndo());
	const size_t length = inner.chainLength();
	// THE CHAIN'S WINDOW CLOSES (its panel, and the state its records hold, go).
	n.outer->overlay().dismiss(); // the top panel: the Chain's
	(void)n.outer->updateContent();
	REQUIRE(findInTree<rack::ChainExpanded>(n.outer.get()) == nullptr);
	CHECK_FALSE(history.canUndo()); // the one record is dead
	history.undo();                 // and walking it does nothing
	CHECK(inner.chainLength() == length);
}

TEST_CASE("NZ any depth: an LFO armed in a Chain's panel INSIDE another Chain's panel relays out through both — the host's device after the outer chain lights for the re-exported spelling; disarming puts both states back") {
	rack::registerChainExpandedView();
	ui::ModState host; // above the fixture: the panels' widgets watch it
	ui::EditHistory history;
	SiteHost h;
	const DeviceInstanceId outer = h.bridge.addDevice(composite::kTraits.tag);
	const DeviceInstanceId after = h.bridge.addDevice(fx::filter::kTraits.tag);
	rack::RackBridge* in1 = rack::bridgeOf(h.bridge.containerSurfaceOf(outer));
	REQUIRE(in1 != nullptr);
	const DeviceInstanceId inner = in1->addDevice(composite::kTraits.tag);
	REQUIRE(inner != kNoInstance);
	rack::RackBridge* in2 = rack::bridgeOf(in1->containerSurfaceOf(inner));
	REQUIRE(in2 != nullptr);
	const DeviceInstanceId lfo = in2->addDevice(mod::lfo::kTraits.tag);
	REQUIRE(lfo != kNoInstance);
	ui::WidgetRegistry registry;
	auto outerView = ui::makeExpandedView(h.bridge, nullptr, registry, outer, *h.bridge.descriptorOf(outer));
	auto* p1 = dynamic_cast<rack::ChainExpanded*>(outerView.get());
	REQUIRE(p1 != nullptr);
	p1->modulationBound(&host, &history);
	(void)p1->updateContent();
	REQUIRE(p1->liveForTest());
	auto innerView = ui::makeExpandedView(*in1, nullptr, registry, inner, *in1->descriptorOf(inner));
	auto* p2 = dynamic_cast<rack::ChainExpanded*>(innerView.get());
	REQUIRE(p2 != nullptr);
	p2->modulationBound(&p1->modForTest(), &history); // the inner window answers to the outer one, as its editor's card does
	(void)p2->updateContent();
	REQUIRE(p2->liveForTest());
	const auto cutoff = static_cast<size_t>(fx::filter::Param::Cutoff);
	ui::DeviceControl afterKnob { h.bridge, after, fx::filter::kDescriptor, cutoff, &host, nullptr };
	CHECK_FALSE(afterKnob.display().routable);

	// ARMED TWO LEVELS IN: the inner panel relays to the outer one's state, and the outer one relays that on.
	p2->modForTest().focus(backplane::Source::output(lfo, 0));
	p2->modForTest().arm(true);
	CHECK(p2->relayArmForTest());
	CHECK(p1->relayArmForTest());
	const backplane::Source mid = in1->containerExport(inner, backplane::Source::output(lfo, 0));
	REQUIRE(mid.isContainerExport());
	CHECK(p1->modForTest().source() == mid);
	const backplane::Source out = h.bridge.containerExport(outer, mid);
	REQUIRE(out.isContainerExport());
	CHECK(host.source() == out);
	CHECK(host.armed());
	CHECK(afterKnob.display().routable);

	// DISARMED: the chain of borrowed arms lets go, inner first.
	p2->modForTest().arm(false);
	CHECK(p2->relayArmForTest());
	CHECK(p1->relayArmForTest());
	CHECK_FALSE(host.armed());
	p2->modulationBound(nullptr, nullptr);
	innerView.reset();
	outerView.reset();
}
