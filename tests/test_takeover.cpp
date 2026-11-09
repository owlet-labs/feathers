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

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "plugin/ui/expanded_canvas.h"
#include "plugin/ui/mod_state.h"
#include "plugin/ui/surface_layers.h"
#include "plugin/ui/takeover_stack.h"
#include "plugin/ui/theme.h"

#include <cmath>
#include <set>
#include <string>
#include <vector>

// THE NON-WINDOWING SURFACE HOST.
//
// NOTHING HERE CLAIMS ANYTHING ABOUT PIXELS — a repaint cannot be observed headlessly, which this
// repo has a rule about. What it claims is the part that is a CONTRACT rather than a picture: which
// kind a need may be answered with, what a stack does when a level below the top is dismissed, and
// that an id retired by a pop is never handed to anyone again.

using namespace feathers;

namespace {

/// A CONTENT FRAME THAT SAYS WHEN IT DIES, so a case can tell "taken down" from "leaked".
struct Probe : visage::Frame {
	explicit Probe(int& deaths) : deaths_(deaths) {}
	~Probe() override { ++deaths_; }
	/// A CALLBACK THE CONTENT OWNS, which is what makes the self-dismissal case faithful: the crash
	/// needed the `std::function` to be a MEMBER of the thing being destroyed, not a local.
	std::function<void()> onAct;
	int& deaths_;
};

/// A HOST THAT ANSWERS THE FORBIDDEN THING. It offers a takeover for a gesture that spans surfaces,
/// which is the mistake `SurfaceHost::kindFor` is non-virtual in order to make unreachable.
struct WrongHost : ui::SurfaceHost {
	void dismiss(ui::SurfaceId) override {}
	size_t presented = 0;

protected:
	ui::SurfaceKind kindAvailable(ui::SurfaceNeed) const override { return ui::SurfaceKind::Takeover; }
	ui::SurfaceId presentImpl(std::unique_ptr<visage::Frame>, ui::SurfaceNeed, std::string,
	                          std::function<void()>) override {
		++presented;
		return 1;
	}
};

std::unique_ptr<visage::Frame> plainFrame() {
	return std::make_unique<visage::Frame>();
}

const visage::KeyEvent kEscape { visage::KeyCode::Escape, 0, true };
const visage::KeyEvent kOtherKey { visage::KeyCode::A, 0, true };

} // namespace

TEST_CASE("an empty stack is invisible and presenting one makes it visible") {
	ui::TakeoverStack stack;
	stack.setBounds(0, 0, 720, 560);
	REQUIRE(stack.empty());
	REQUIRE_FALSE(stack.isVisible());

	const ui::SurfaceId id = stack.present(plainFrame(), ui::SurfaceNeed::SelfContained, "ADD DEVICE",
	                                       nullptr);
	CHECK(id != ui::kNoSurface);
	CHECK(stack.depth() == 1);
	CHECK(stack.isVisible());
	CHECK(stack.titleForTest() == "ADD DEVICE");
}

TEST_CASE("the content is placed below the bar and fills the rest") {
	ui::TakeoverStack stack;
	stack.setBounds(0, 0, 720, 560);
	stack.present(plainFrame(), ui::SurfaceNeed::SelfContained, "T", nullptr);

	const visage::Frame* top = stack.topForTest();
	REQUIRE(top != nullptr);
	CHECK(top->y() == static_cast<int>(ui::TakeoverStack::kBarHeight));
	CHECK(top->width() == 720);
	CHECK(top->height() == 560 - static_cast<int>(ui::TakeoverStack::kBarHeight));
}

TEST_CASE("A SPANNING GESTURE IS NEVER ANSWERED WITH A TAKEOVER — the editor beneath is gone") {
	ui::TakeoverStack stack;
	stack.setBounds(0, 0, 720, 560);

	CHECK(stack.kindFor(ui::SurfaceNeed::SelfContained) == ui::SurfaceKind::Takeover);
	CHECK(stack.kindFor(ui::SurfaceNeed::SpansGesture) == ui::SurfaceKind::Inline);

	// AND THE REFUSAL IS ACTED ON, not merely reported. A host that says "Inline" and then presents
	// anyway would have told the truth and done the other thing.
	const ui::SurfaceId id = stack.present(plainFrame(), ui::SurfaceNeed::SpansGesture, "ARM", nullptr);
	CHECK(id == ui::kNoSurface);
	CHECK(stack.depth() == 0);
	CHECK_FALSE(stack.isVisible());
}

TEST_CASE("a host that offers the forbidden kind is overruled by the contract, not trusted") {
	WrongHost host;
	// Its own answer is the wrong one...
	CHECK(host.kindFor(ui::SurfaceNeed::SelfContained) == ui::SurfaceKind::Takeover);
	// ...and the non-virtual road corrects it before anything is presented.
	CHECK(host.kindFor(ui::SurfaceNeed::SpansGesture) == ui::SurfaceKind::Inline);

	CHECK(host.present(plainFrame(), ui::SurfaceNeed::SpansGesture, "X", nullptr) == ui::kNoSurface);
	CHECK(host.presented == 0);

	// The kind it CAN satisfy still reaches it.
	CHECK(host.present(plainFrame(), ui::SurfaceNeed::SelfContained, "X", nullptr) != ui::kNoSurface);
	CHECK(host.presented == 1);
}

TEST_CASE("presenting nothing is declined rather than pushing an empty level") {
	ui::TakeoverStack stack;
	CHECK(stack.present(nullptr, ui::SurfaceNeed::SelfContained, "T", nullptr) == ui::kNoSurface);
	CHECK(stack.depth() == 0);
}

TEST_CASE("back pops one level and tells only that one") {
	ui::TakeoverStack stack;
	stack.setBounds(0, 0, 720, 560);
	int outer = 0;
	int inner = 0;
	stack.present(plainFrame(), ui::SurfaceNeed::SelfContained, "OUTER", [&] { ++outer; });
	const visage::Frame* outerFrame = stack.topForTest();
	stack.present(plainFrame(), ui::SurfaceNeed::SelfContained, "INNER", [&] { ++inner; });
	REQUIRE(stack.depth() == 2);
	CHECK(stack.titleForTest() == "INNER");

	// THE ONE UNDERNEATH STOPPED DRAWING. A takeover REPLACES: leaving the lower level visible would
	// paint two surfaces through each other, and the lower one would still be taking presses.
	CHECK_FALSE(outerFrame->isVisible());
	CHECK(stack.topForTest()->isVisible());

	stack.back();
	CHECK(stack.depth() == 1);
	CHECK(inner == 1);
	// THE ONE BELOW WAS NOT TOLD. Esc means "back", and a stack that closed everything on one press
	// is not a back stack.
	CHECK(outer == 0);
	CHECK(stack.titleForTest() == "OUTER");
	CHECK(stack.isVisible());
	CHECK(outerFrame->isVisible());

	stack.back();
	CHECK(stack.depth() == 0);
	CHECK(outer == 1);
	CHECK_FALSE(stack.isVisible());
}

TEST_CASE("dismissing a level takes everything above it, top-down") {
	ui::TakeoverStack stack;
	stack.setBounds(0, 0, 720, 560);
	std::vector<std::string> told;
	const ui::SurfaceId outer = stack.present(plainFrame(), ui::SurfaceNeed::SelfContained, "OUTER",
	                                          [&] { told.push_back("outer"); });
	stack.present(plainFrame(), ui::SurfaceNeed::SelfContained, "MID", [&] { told.push_back("mid"); });
	stack.present(plainFrame(), ui::SurfaceNeed::SelfContained, "TOP", [&] { told.push_back("top"); });
	REQUIRE(stack.depth() == 3);

	stack.dismiss(outer);
	CHECK(stack.depth() == 0);
	// A LEVEL PUSHED FROM INSIDE ANOTHER CANNOT OUTLIVE IT, and the order is the one a hand would
	// see: the thing in front goes first.
	REQUIRE(told.size() == 3);
	CHECK(told[0] == "top");
	CHECK(told[1] == "mid");
	CHECK(told[2] == "outer");
}

TEST_CASE("dismissed content leaves the tree at once and is destroyed later, never inside a callback") {
	// **THIS CASE USED TO ASSERT THE DEFECT.** It required `deaths == 1` immediately after `back()` —
	// "the content is destroyed when its level goes, not held" — and that is precisely the behaviour
	// that crashed the audio engine: `BrowserView::onPick` calls `dismiss()`, so destroying the
	// content inside the pop frees the `std::function` that is mid-execution and the `this` it
	// captured. ASan: `heap-use-after-free ... rack_editor.cpp:148 in operator()`.
	//
	// A TEST CAN PIN A BUG AS EASILY AS A CONTRACT, and this one did, in its own title. The contract it
	// should have been asserting is the one `ExpandedOverlay::retired_` had already written down:
	// leaving the TREE is immediate, being DESTROYED is deferred to a moment when nothing is running
	// inside it.
	int deaths = 0;
	{
		ui::TakeoverStack stack;
		stack.setBounds(0, 0, 720, 560);
		stack.present(std::make_unique<Probe>(deaths), ui::SurfaceNeed::SelfContained, "T", nullptr);
		REQUIRE(deaths == 0);

		stack.back();
		// OUT OF THE TREE IMMEDIATELY — the stack is empty and hidden, which is everything a dismissal
		// owes the player: it stops drawing and stops taking presses at once.
		CHECK(stack.empty());
		CHECK_FALSE(stack.isVisible());
		// AND NOT YET DESTROYED. If this is ever 1 again, the crash is back.
		CHECK(deaths == 0);

		// THE NEXT PRESENT IS A SAFE MOMENT: no dismissal callback can still be on the stack.
		stack.present(std::make_unique<visage::Frame>(), ui::SurfaceNeed::SelfContained, "U", nullptr);
		CHECK(deaths == 1);
	}
	CHECK(deaths == 1);
}

TEST_CASE("content that dismisses itself from its own callback survives the call") {
	// THE CRASH, IN MINIATURE AND WITHOUT THE RACK. A guest's handler does `dismiss(id)` and then keeps
	// working — which is what `BrowserView::onPick` does, because the pick has to take the surface down
	// before it says anything through the notifier behind it.
	ui::TakeoverStack stack;
	stack.setBounds(0, 0, 720, 560);
	int deaths = 0;
	int after = 0;
	ui::SurfaceId id = ui::kNoSurface;

	auto probe = std::make_unique<Probe>(deaths);
	Probe* raw = probe.get();
	id = stack.present(std::move(probe), ui::SurfaceNeed::SelfContained, "T", nullptr);
	REQUIRE(id != ui::kNoSurface);

	// THE CONTENT'S OWN CALLBACK, holding a pointer to itself exactly as a lambda captures `this`.
	raw->onAct = [&] {
		stack.dismiss(id);
		// IF THE CONTENT WERE DESTROYED INSIDE `dismiss`, THIS LINE RUNS ON FREED MEMORY. Under ASan
		// that is a heap-use-after-free; without it, it is a value nobody can predict.
		++after;
		CHECK(deaths == 0);
	};
	raw->onAct();

	CHECK(after == 1);
	CHECK(stack.empty());
}

TEST_CASE("a stack destroyed with surfaces up takes them down and tells nobody") {
	int deaths = 0;
	int told = 0;
	{
		ui::TakeoverStack stack;
		stack.setBounds(0, 0, 720, 560);
		stack.present(std::make_unique<Probe>(deaths), ui::SurfaceNeed::SelfContained, "T",
		              [&] { ++told; });
	}
	CHECK(deaths == 1);
	// AN EDITOR BEING TORN DOWN IS NOT "THE PLAYER CLOSED YOUR SURFACE", and a guest mid-destruction
	// is the worst possible moment to call back into.
	CHECK(told == 0);
}

TEST_CASE("AN ID IS NEVER REUSED, so a stale dismiss cannot land on somebody else's surface") {
	ui::TakeoverStack stack;
	stack.setBounds(0, 0, 720, 560);
	const ui::SurfaceId first = stack.present(plainFrame(), ui::SurfaceNeed::SelfContained, "A", nullptr);
	stack.back();
	int secondDismissals = 0;
	const ui::SurfaceId second = stack.present(plainFrame(), ui::SurfaceNeed::SelfContained, "B",
	                                           [&] { ++secondDismissals; });

	// THE ABA RULE, IN THE SMALL. A depth index would make these two equal, and the stale dismiss
	// below would take down a surface the caller never opened.
	CHECK(first != second);
	stack.dismiss(first);
	CHECK(stack.depth() == 1);
	CHECK(secondDismissals == 0);
	CHECK(stack.titleForTest() == "B");
}

TEST_CASE("dismissing nothing, or an id that was never issued, is silence") {
	ui::TakeoverStack stack;
	stack.setBounds(0, 0, 720, 560);
	stack.present(plainFrame(), ui::SurfaceNeed::SelfContained, "A", nullptr);
	stack.dismiss(ui::kNoSurface);
	stack.dismiss(99999);
	CHECK(stack.depth() == 1);
	CHECK(stack.dismissalsForTest() == 0);
}

TEST_CASE("a dismissal handler that presents again is safe, and its level survives the pop") {
	ui::TakeoverStack stack;
	stack.setBounds(0, 0, 720, 560);
	bool again = true;
	stack.present(plainFrame(), ui::SurfaceNeed::SelfContained, "FIRST", [&] {
		if (!again) {
			return;
		}
		again = false;
		// A BROWSER WHOSE PICK OPENS THE THING IT PICKED does exactly this, and the entry must
		// already be out of the vector or this pushes onto one being mutated.
		stack.present(plainFrame(), ui::SurfaceNeed::SelfContained, "SECOND", nullptr);
	});

	stack.back();
	CHECK(stack.depth() == 1);
	CHECK(stack.titleForTest() == "SECOND");
	CHECK(stack.isVisible());
}

TEST_CASE("escape pops one level, and with nothing up it is offered onward rather than eaten") {
	ui::TakeoverStack stack;
	stack.setBounds(0, 0, 720, 560);
	int offered = 0;
	stack.onEscapeWhenEmpty = [&] {
		++offered;
		return true;
	};

	stack.present(plainFrame(), ui::SurfaceNeed::SelfContained, "OUTER", nullptr);
	stack.present(plainFrame(), ui::SurfaceNeed::SelfContained, "INNER", nullptr);

	CHECK(stack.onKeyPress().callback(kEscape));
	CHECK(stack.depth() == 1);
	CHECK(offered == 0);

	CHECK(stack.onKeyPress().callback(kEscape));
	CHECK(stack.depth() == 0);
	CHECK(offered == 0);

	// EMPTY: THE KEY BELONGS TO WHOEVER ELSE WANTS IT — today that is leaving assign mode.
	CHECK(stack.onKeyPress().callback(kEscape));
	CHECK(offered == 1);

	// And a key nobody asked about is not swallowed either.
	CHECK_FALSE(stack.onKeyPress().callback(kOtherKey));
}

TEST_CASE("the back chevron pops and the rest of the bar does not") {
	ui::TakeoverStack stack;
	stack.setBounds(0, 0, 720, 560);
	stack.present(plainFrame(), ui::SurfaceNeed::SelfContained, "T", nullptr);

	CHECK_FALSE(stack.pressAt(400.0f, 8.0f));
	CHECK(stack.depth() == 1);

	// AND BELOW THE BAR IS THE CONTENT'S — a miss must not close a full-window surface, because Back
	// is the only exit and most of the surface is empty space.
	CHECK_FALSE(stack.pressAt(300.0f, 400.0f));
	CHECK(stack.depth() == 1);

	CHECK(stack.pressAt(10.0f, 8.0f));
	CHECK(stack.depth() == 0);
}

TEST_CASE("the wheel is swallowed while a surface is up and passed on when none is") {
	ui::TakeoverStack stack;
	stack.setBounds(0, 0, 720, 560);
	const visage::MouseEvent wheel {};

	CHECK_FALSE(stack.onMouseWheel().callback(wheel));
	stack.present(plainFrame(), ui::SurfaceNeed::SelfContained, "T", nullptr);
	// OTHERWISE IT SCROLLS A RACK THE PLAYER CANNOT SEE, and they come back to a surface that moved.
	CHECK(stack.onMouseWheel().callback(wheel));
}

TEST_CASE("a mounted panel reports its OWN size, so the overlay does not shrink it") {
	// The popout cards were not expanding to their full size: they left an invisible scroll
	// function instead. A panel is shown at its full size.
	//
	// `ExpandedOverlay` sizes its panel from `minimumSize()`. `MountedFrame` answered a constant, so
	// every popout came out the same size and the real panel was scrolled inside it. It now asks.
	struct SizedView : ui::ExpandedView {
		visage::Point minimumSize() const override { return { 900.0f, 640.0f }; }
		std::string title() const override { return "BIG"; }
	};

	auto view = std::make_unique<SizedView>();
	ui::MountedFrame mount { std::move(view), "BIG", visage::Point { 420.0f, 300.0f } };

	// THE PANEL'S NUMBER, NOT THE FLOOR'S — and the floor is deliberately smaller than both, so a
	// mount that returned it would be visibly wrong rather than coincidentally right.
	CHECK(mount.minimumSize().x == doctest::Approx(900.0f));
	CHECK(mount.minimumSize().y == doctest::Approx(640.0f));
}

TEST_CASE("content that is not a panel reports the bounds it was laid out to") {
	// THE GENERIC FALLBACK: a plain frame has no `minimumSize`, so the mount uses what the guest sized
	// it to — and the floor is a FLOOR, applied only when the content is smaller than it.
	auto small = std::make_unique<visage::Frame>();
	small->setBounds(0, 0, 200, 150);
	ui::MountedFrame tiny { std::move(small), "T", visage::Point { 420.0f, 300.0f } };
	CHECK(tiny.minimumSize().x == doctest::Approx(420.0f));
	CHECK(tiny.minimumSize().y == doctest::Approx(300.0f));

	auto big = std::make_unique<visage::Frame>();
	big->setBounds(0, 0, 700, 500);
	ui::MountedFrame large { std::move(big), "T", visage::Point { 420.0f, 300.0f } };
	CHECK(large.minimumSize().x == doctest::Approx(700.0f));
	CHECK(large.minimumSize().y == doctest::Approx(500.0f));
}

// ---------------------------------------------------------------------------------------------
// THE CANVAS. The fourth kind, docked beside the chain in room the
// editor asks the host for. What is claimed here is the ROUTING contract and the lifetimes, never a
// picture: which kind a spanning gesture gets while the canvas shows and once it is collapsed, that
// an id dismisses what it named, that a nested host escalates to the outer one, and that a
// replacement issued from INSIDE the panel being replaced does not free the frame under the handler.
// ---------------------------------------------------------------------------------------------

namespace {

/// A HOST WITH A CANVAS, laid out the way an editor lays one out: the instrument keeps its `kBase`
/// columns, the canvas is a column outboard of them, and the layers cover BOTH — so a fold shrinks the
/// layers, exactly as an editor's logical size shrinks (`fold` re-lays out, as the editor's
/// `onCollapsedChanged` does).
struct CanvasHost {
	static constexpr int kBase = 520;
	ui::SurfaceLayers layers;
	ui::ExpandedCanvas canvas { ui::ExpandedCanvas::Edge::Right };
	int told = 0;
	CanvasHost() {
		// OPENED: a fresh canvas is folded (minimised is the original behaviour), and
		// these cases are about the routing while it shows.
		REQUIRE(canvas.setCollapsed(false));
		layers.setCanvas(&canvas);
		layout();
	}
	void layout() {
		const int extent = static_cast<int>(canvas.extent());
		layers.setBounds(0, 0, kBase + extent, 600);
		canvas.setBounds(kBase, 0, extent, 600);
	}
	void fold(bool folded) {
		REQUIRE(canvas.setCollapsed(folded));
		layout();
	}
	/// The layer's whole surface — the editor plus the band.
	visage::Bounds surface() const { return { 0.0f, 0.0f, layers.width(), layers.height() }; }
	static bool inside(const visage::Bounds& inner, const visage::Bounds& outer) {
		return inner.x() >= outer.x() - 0.5f && inner.y() >= outer.y() - 0.5f && inner.right() <= outer.right() + 0.5f
		       && inner.bottom() <= outer.bottom() + 0.5f;
	}
	ui::SurfaceId present(std::unique_ptr<visage::Frame> content = plainFrame()) {
		return layers.present(std::move(content), ui::SurfaceNeed::SpansGesture, "X", [this] { ++told; });
	}
};

/// AN EXPANDED VIEW THAT COUNTS ITS OWN ADVANCES, so a case can prove the tick reached it THROUGH the
/// layers rather than inferring it from a picture that animates on its own.
struct TickedView : ui::ExpandedView {
	int ticks = 0;
	int& deaths;
	explicit TickedView(int& d) : deaths(d) {}
	~TickedView() override { ++deaths; }
	visage::Point minimumSize() const override { return { 300.0f, 200.0f }; }
	std::string title() const override { return "TICKED"; }
	bool updateContent() override {
		++ticks;
		return true;
	}
};

/// AN EXPANDED VIEW KEYED BY DEVICE, so two of them are two panels and two of the same are one.
struct KeyedView : ui::ExpandedView {
	DeviceInstanceId id;
	visage::Point minimum;
	explicit KeyedView(DeviceInstanceId device, visage::Point size = { 200.0f, 120.0f }) : id(device), minimum(size) {}
	visage::Point minimumSize() const override { return minimum; }
	std::string title() const override { return "K" + std::to_string(id); }
	DeviceInstanceId watchedDevice() const override { return id; }
};

} // namespace

TEST_CASE("a spanning gesture ARRIVES in the canvas while it shows and centred once it is folded — on the one overlay either way") {
	CanvasHost h;
	REQUIRE(h.canvas.showing());
	CHECK(h.layers.kindFor(ui::SurfaceNeed::SpansGesture) == ui::SurfaceKind::Canvas);
	// AND A SELF-CONTAINED SURFACE IS STILL THE TAKEOVER: the canvas serves gestures, not browsers.
	CHECK(h.layers.kindFor(ui::SurfaceNeed::SelfContained) == ui::SurfaceKind::Takeover);
	// THE REGION IS THE BAND'S FLOOR, in the overlay's pixels.
	const auto region = h.layers.canvasRegion();
	REQUIRE(region.has_value());
	CHECK(region->x() == doctest::Approx(CanvasHost::kBase + ui::ExpandedCanvas::kPad));
	CHECK(region->y() == doctest::Approx(ui::ExpandedCanvas::kTitleHeight));

	const ui::SurfaceId id = h.present();
	REQUIRE(id != ui::kNoSurface);
	REQUIRE(h.layers.overlay().open());
	// AT THE REGION'S TOP-LEFT — the band's full-left rule, now a preference on the editor's overlay.
	CHECK(h.layers.overlay().panelBounds().x() == doctest::Approx(region->x()));
	CHECK(h.layers.overlay().panelBounds().y() == doctest::Approx(region->y()));
	// THE ID NAMES IT. Dismissing by id takes it down, tells the presenter once, and a second dismiss
	// of the same id is silence rather than a second telling.
	h.layers.dismiss(id);
	CHECK_FALSE(h.layers.overlay().open());
	CHECK(h.told == 1);
	h.layers.dismiss(id);
	CHECK(h.told == 1);

	// FOLDED: no region, and a new panel is centred over the editor, as before the canvas existed.
	h.fold(true);
	CHECK(h.layers.kindFor(ui::SurfaceNeed::SpansGesture) == ui::SurfaceKind::Overlay);
	CHECK_FALSE(h.layers.canvasRegion().has_value());
	// ANOTHER DEVICE: the first one would reopen where it was left, which is the overlay's memory.
	const ui::SurfaceId second = h.present(std::make_unique<KeyedView>(2));
	REQUIRE(second != ui::kNoSurface);
	REQUIRE(h.layers.overlay().open());
	const visage::Bounds centred = h.layers.overlay().panelBounds();
	CHECK(centred.x() == doctest::Approx(std::round((h.layers.width() - centred.width()) * 0.5f)));
	h.layers.dismiss(second);
	CHECK_FALSE(h.layers.overlay().open());
	CHECK(h.told == 2);
}

TEST_CASE("with the canvas open a panel arrives in the band, drags anywhere over the editor, and is never lost off the surface") {
	// THE DESIGN: the canvas is an extension of the UI where windows float over and out of the way of the
	// main synth; while it is open, windows still prefer to go there.
	CanvasHost h;
	const visage::Bounds region = *h.layers.canvasRegion();
	REQUIRE(h.present(std::make_unique<KeyedView>(1)) != ui::kNoSurface);
	ui::ExpandedOverlay& overlay = h.layers.overlay();
	CHECK(CanvasHost::inside(overlay.panelBounds(), region));

	// OVER THE INSTRUMENT: dragged left out of the band, it sits wholly over the editor's own columns —
	// which the band's own floor could never allow.
	overlay.beginPanelDrag();
	overlay.dragPanelBy({ -(region.x() - 40.0f), 100.0f });
	const visage::Bounds over = overlay.panelBounds();
	CHECK(over.x() == doctest::Approx(40.0f));
	CHECK(over.right() <= static_cast<float>(CanvasHost::kBase));
	CHECK(over.y() == doctest::Approx(region.y() + 100.0f));

	// AND NEVER OFF THE SURFACE, whichever way a hand throws it: clamped to the editor PLUS the band.
	for (const visage::Point throwTo : { visage::Point { -5000.0f, -5000.0f }, visage::Point { 5000.0f, 5000.0f } }) {
		overlay.beginPanelDrag();
		overlay.dragPanelBy(throwTo);
		CHECK(CanvasHost::inside(overlay.panelBounds(), h.surface()));
	}
	// THE FAR CORNER OF THE SURFACE IS THE BAND'S, so the clamp is the whole surface and not the editor.
	CHECK(overlay.panelBounds().right() == doctest::Approx(h.layers.width()));
	CHECK(overlay.panelBounds().x() > static_cast<float>(CanvasHost::kBase));
}

TEST_CASE("folding the canvas dismisses nothing — a panel in the band is pulled back onto the editor, and returns when the band opens") {
	CanvasHost h;
	REQUIRE(h.present(std::make_unique<KeyedView>(1)) != ui::kNoSurface);
	ui::ExpandedOverlay& overlay = h.layers.overlay();
	// PUT IT DEEP IN THE BAND: its right edge on the surface's right edge.
	overlay.beginPanelDrag();
	overlay.dragPanelBy({ 5000.0f, 0.0f });
	const visage::Bounds inBand = overlay.panelBounds();
	REQUIRE(inBand.x() > static_cast<float>(CanvasHost::kBase));

	h.fold(true);
	CHECK(h.told == 0);
	REQUIRE(overlay.panelCount() == 1);
	const visage::Bounds pulled = overlay.panelBounds();
	CHECK(CanvasHost::inside(pulled, h.surface()));
	CHECK(pulled.right() == doctest::Approx(h.layers.width()));
	// THE PANEL ITSELF MOVED, not only the number: the frame a hand sees is where the bounds say.
	REQUIRE(overlay.panelForTest() != nullptr);
	CHECK(overlay.panelForTest()->x() == doctest::Approx(pulled.x()));

	// OPEN AGAIN: kept as chosen and clamped when read, so it goes back where the hand left it.
	h.fold(false);
	CHECK(overlay.panelBounds().x() == doctest::Approx(inBand.x()));
	CHECK(overlay.panelForTest()->x() == doctest::Approx(inBand.x()));
}

TEST_CASE("the overlay holds SEVERAL panels, one per device, whether they arrived in the band or not, and a fold dismisses none") {
	// Several windows can be open, in the canvas or floating. A second DEVICE is a second panel, the same device again replaces its own. Since
	// 2026-10-03 the band holds no panels of its own: one stack over the editor and the band together.
	CanvasHost h;
	h.fold(true);
	const ui::SurfaceId floatingA = h.present(std::make_unique<KeyedView>(1));
	const ui::SurfaceId floatingB = h.present(std::make_unique<KeyedView>(2));
	REQUIRE(floatingA != ui::kNoSurface);
	REQUIRE(floatingB != ui::kNoSurface);
	CHECK(h.layers.overlay().panelCount() == 2);
	CHECK(h.told == 0);
	// THE SAME DEVICE AGAIN: its panel is replaced, its presenter told, the count unchanged.
	const ui::SurfaceId floatingA2 = h.present(std::make_unique<KeyedView>(1));
	CHECK(floatingA2 != floatingA);
	CHECK(h.layers.overlay().panelCount() == 2);
	CHECK(h.told == 1);
	CHECK_FALSE(h.layers.holds(floatingA));
	CHECK(h.layers.holds(floatingA2));

	// THEN THE CANVAS OPENS AND THE NEXT TWO ARRIVE IN IT, on the same overlay. The floating ones stay.
	h.fold(false);
	const visage::Bounds region = *h.layers.canvasRegion();
	const ui::SurfaceId dockedC = h.present(std::make_unique<KeyedView>(3));
	const ui::SurfaceId dockedD = h.present(std::make_unique<KeyedView>(4));
	REQUIRE(dockedC != ui::kNoSurface);
	REQUIRE(dockedD != ui::kNoSurface);
	CHECK(h.layers.overlay().panelCount() == 4);
	CHECK(h.layers.presentedCountForTest() == 4);
	const auto ids = h.layers.overlay().panelIdsForTest();
	CHECK(CanvasHost::inside(h.layers.overlay().panelBounds(ids[2]), region));
	CHECK(CanvasHost::inside(h.layers.overlay().panelBounds(ids[3]), region));

	// ONE BY ID, and only that one.
	h.layers.dismiss(dockedC);
	CHECK(h.layers.overlay().panelCount() == 3);
	CHECK(h.layers.holds(dockedD));
	CHECK_FALSE(h.layers.holds(dockedC));
	CHECK(h.told == 2);

	// FOLDING DISMISSES NOTHING AND TELLS NOBODY: every panel stays, on the surface that is left.
	h.fold(true);
	CHECK(h.told == 2);
	CHECK(h.layers.overlay().panelCount() == 3);
	CHECK(h.layers.holds(dockedD));
	for (const auto id : h.layers.overlay().panelIdsForTest()) {
		CHECK(CanvasHost::inside(h.layers.overlay().panelBounds(id), h.surface()));
	}

	// AND EVERYTHING PRESENTED, AT ONCE.
	h.layers.dismissPresented();
	CHECK(h.layers.overlay().panelCount() == 0);
	CHECK(h.layers.presentedCountForTest() == 0);
	CHECK(h.told == 5);
}

TEST_CASE("a new panel DODGES the open ones — beside them where it fits, over them only when the layer is too full") {
	// New panels dodge the open ones so they do not overlap where possible, and overlap only when they must.
	ui::ExpandedOverlay overlay;
	overlay.setBounds(0, 0, 1000, 600);
	const auto rect = [&](ui::ExpandedOverlay::PanelId id) { return overlay.panelBounds(id); };
	const ui::ExpandedOverlay::PanelId a = overlay.show(std::make_unique<KeyedView>(1));
	const ui::ExpandedOverlay::PanelId b = overlay.show(std::make_unique<KeyedView>(2));
	const ui::ExpandedOverlay::PanelId c = overlay.show(std::make_unique<KeyedView>(3));
	REQUIRE(overlay.panelCount() == 3);
	// THE FIRST IS CENTRED — an editor's overlay, where a corner panel would cover the rails — and the
	// next two sit clear of it and of each other, inside the layer, ON ITS ROW: rows first, so panels
	// fill sideways before they wrap (so that windows appearing rightward have headroom).
	CHECK(rect(a).x() == doctest::Approx(std::round((1000.0f - rect(a).width()) * 0.5f)));
	CHECK(rect(b).y() == doctest::Approx(rect(a).y()));
	CHECK(rect(b).x() >= rect(a).right()); // beside it, to the right first
	CHECK(rect(c).y() == doctest::Approx(rect(a).y()));
	CHECK_FALSE(rect(a).overlaps(rect(b)));
	CHECK_FALSE(rect(a).overlaps(rect(c)));
	CHECK_FALSE(rect(b).overlaps(rect(c)));
	for (const auto id : { a, b, c }) {
		CHECK(rect(id).x() >= 0.0f);
		CHECK(rect(id).y() >= 0.0f);
		CHECK(rect(id).right() <= 1000.0f);
		CHECK(rect(id).bottom() <= 600.0f);
	}
	// THE FRONT IS THE LAST OPENED, and Escape closes it alone.
	CHECK(overlay.frontPanelIdForTest() == c);
	CHECK(overlay.onKeyPress().callback(kEscape));
	CHECK(overlay.panelCount() == 2);
	CHECK(overlay.panelBounds(c).width() == doctest::Approx(0.0f)); // gone: an unknown id's bounds are empty
	CHECK(overlay.frontPanelIdForTest() == b);

	// TOO FULL: panels as big as the layer cannot avoid one another, and the second one still opens —
	// over the first — rather than being refused.
	ui::ExpandedOverlay small;
	small.setBounds(0, 0, 300, 300);
	const auto big = small.show(std::make_unique<KeyedView>(7, visage::Point { 260.0f, 140.0f }));
	const auto bigger = small.show(std::make_unique<KeyedView>(8, visage::Point { 260.0f, 140.0f }));
	REQUIRE(small.panelCount() == 2);
	CHECK(small.panelBounds(big).overlaps(small.panelBounds(bigger)));
	// AND CASCADED, not stacked exactly: the covered panel's head stays out from under the new one, one
	// title row down and a step right, so a hand can still reach it to raise it.
	CHECK(small.panelBounds(bigger).y() >= small.panelBounds(big).y() + ui::ExpandedOverlay::kTitleHeight - 1.0f);
	CHECK(small.panelBounds(bigger).x() > small.panelBounds(big).x());
	// EACH WEARS ITS INSTANCE'S TINT, and two instances wear two.
	REQUIRE(small.panelForTest(big) != nullptr);
	REQUIRE(small.panelForTest(bigger) != nullptr);
	CHECK(small.panelForTest(big)->tintForTest() == ui::instanceTintFor(7));
	CHECK(small.panelForTest(big)->tintForTest() != 0);
	CHECK(small.panelForTest(big)->tintForTest() != small.panelForTest(bigger)->tintForTest());
}

TEST_CASE("the partition follows the pointer and the other side keeps its design size — the scale pays when the window cannot") {
	// THE DEFECTS: scaling the partition to extreme values repositioned the knobs until they clipped off,
	// so the synth side must not over-shrink; and in the rack, clipping vertically offered no scroll. `partitionExtent` is the rule both hosts apply
	// on every drag step; asserted here as VALUES, since the picture cannot be observed headlessly.
	using ui::ExpandedCanvas;
	// SCALE-LIMITED: a 1080-pixel window, a 700-row instrument, the pointer 380 pixels up from the foot.
	// The instrument gets 700 pixels at scale 1.0 and the canvas the 380 that are left: 380 rows.
	CHECK(ExpandedCanvas::partitionExtent(700.0f, 1080.0f, 380.0f, 1.75f) == doctest::Approx(380.0f));
	// Further up: the instrument shrinks to fit its 700 rows in 600 pixels (scale 0.857), and the 480
	// canvas pixels at that scale are 560 rows.
	CHECK(ExpandedCanvas::partitionExtent(700.0f, 1080.0f, 480.0f, 1.75f) == doctest::Approx(560.0f));
	// The other side is NEVER below its design size: at scale s = (span - canvas) / base the instrument
	// has exactly `base` rows whatever the pointer does.
	// FREE: a 2000-pixel window at scale 1.0 has room; the extent is the pointer's rows, capped so the
	// instrument keeps 700.
	CHECK(ExpandedCanvas::partitionExtent(700.0f, 2000.0f, 400.0f, 1.0f) == doctest::Approx(400.0f));
	CHECK(ExpandedCanvas::partitionExtent(700.0f, 2000.0f, 1000.0f, 1.0f) == doctest::Approx(1000.0f));
	// THE TWO REGIMES MEET WITHOUT A STEP: at exactly 700 pixels left for 700 rows both give 1300 rows.
	CHECK(ExpandedCanvas::partitionExtent(700.0f, 2000.0f, 1300.0f, 1.0f) == doctest::Approx(1300.0f));
	// PAST IT the instrument is scaled down rather than losing rows: 1500 canvas pixels leave 500 for 700
	// rows (scale 0.714), so the canvas has 2100 rows — clamped to the ceiling the host passes, which is
	// its own design size (`kMaxShare`): the canvas is never more than the instrument beside it.
	CHECK(ExpandedCanvas::partitionExtent(700.0f, 2000.0f, 1500.0f, 1.0f, ExpandedCanvas::kMinExtent, 700.0f)
	      == doctest::Approx(700.0f));
	// CLAMPED to the canvas's own floor and ceiling.
	CHECK(ExpandedCanvas::partitionExtent(700.0f, 2000.0f, 10.0f, 1.0f) == doctest::Approx(ExpandedCanvas::kMinExtent));
	CHECK(ExpandedCanvas::partitionExtent(560.0f, 4000.0f, 3900.0f, 1.0f) == doctest::Approx(ExpandedCanvas::kMaxExtent));
	// AND THE TOP-EDGE BAND (the rack) is the same arithmetic with the pointer's distance from the top.
	CHECK(ExpandedCanvas::partitionExtent(560.0f, 1080.0f, 520.0f, 1.75f) == doctest::Approx(520.0f));
}

TEST_CASE("panels arriving in the band dodge one another WITHIN it first, and a raised panel comes to the front") {
	CanvasHost h;
	const visage::Bounds region = *h.layers.canvasRegion();
	const ui::SurfaceId one = h.present(std::make_unique<KeyedView>(1));
	const ui::SurfaceId two = h.present(std::make_unique<KeyedView>(2));
	REQUIRE(one != ui::kNoSurface);
	REQUIRE(two != ui::kNoSurface);
	ui::ExpandedOverlay& floor = h.layers.overlay();
	REQUIRE(floor.panelCount() == 2);
	const auto ids = floor.panelIdsForTest();
	const auto other = ids[0];
	const auto first = ids[1];
	REQUIRE(floor.frontPanelIdForTest() == first);
	// TWO PANELS, CLEAR OF EACH OTHER, INSIDE THE BAND'S FLOOR.
	CHECK_FALSE(floor.panelBounds(first).overlaps(floor.panelBounds(other)));
	CHECK(CanvasHost::inside(floor.panelBounds(first), region));
	CHECK(CanvasHost::inside(floor.panelBounds(other), region));
	// THE FIRST IS FULL LEFT AT THE TOP OF THE FLOOR, and the second beside it to the right.
	CHECK(floor.panelBounds(other).x() == doctest::Approx(region.x()));
	CHECK(floor.panelBounds(other).y() == doctest::Approx(region.y()));
	CHECK(floor.panelBounds(first).x() >= floor.panelBounds(other).right());
	CHECK(floor.panelBounds(first).y() == doctest::Approx(region.y()));

	// A THIRD THAT CANNOT SIT BESIDE THEM WRAPS INSIDE THE BAND rather than spilling over the editor
	// while the band still has room.
	REQUIRE(h.present(std::make_unique<KeyedView>(3)) != ui::kNoSurface);
	CHECK(CanvasHost::inside(floor.panelBounds(floor.frontPanelIdForTest()), region));
	// A RELEASE ON THE BACK CARD BRINGS IT TO THE FRONT — the panel's own road.
	ui::ExpandedPanel* back = floor.panelForTest(other);
	REQUIRE(back != nullptr);
	visage::MouseEvent up;
	up.position = { 5.0f, 5.0f };
	back->onMouseUp().callback(up);
	CHECK(floor.frontPanelIdForTest() == other);
}

TEST_CASE("a hidden canvas has no extent and offers nothing, which is what a nested editor gets") {
	CanvasHost h;
	CHECK(h.canvas.extent() == doctest::Approx(ui::ExpandedCanvas::kExtent));
	// AN EMPTY CANVAS IS NOT A DEVICE FRAME: its sky moves on `animateBackdrop`, which the host runs
	// uncounted, and never through the content tick the meter cases count.
	CHECK_FALSE(h.layers.updateContent());
	REQUIRE(h.canvas.setCollapsed(true));
	// THE CLOSED STRIP IS THE RAIL'S, so the two folds read as one idiom.
	CHECK(h.canvas.extent() == doctest::Approx(34.0f));
	REQUIRE(h.canvas.setCollapsed(false));
	h.canvas.setVisible(false);
	CHECK(h.canvas.extent() == doctest::Approx(0.0f));
	CHECK_FALSE(h.canvas.showing());
	CHECK(h.layers.kindFor(ui::SurfaceNeed::SpansGesture) == ui::SurfaceKind::Overlay);
	// AND A HIDDEN BAND IS NO PREFERENCE: a present is centred, as on a host with no canvas.
	CHECK_FALSE(h.layers.canvasRegion().has_value());
}

TEST_CASE("a nested host escalates a spanning gesture to the OUTER's overlay — arriving in its band while that shows, centred otherwise — and its dismiss follows") {
	// BACKLOG EB, decided 2026-09-14 ("beside it"): a device inside a chain opens beside the chain's panel
	// in the outer band; with the band folded it floats over the editor, as it always did. The inner
	// host asks nothing of its own layers either way.
	CanvasHost outer; // the canvas open
	ui::SurfaceLayers inner;
	inner.setBounds(0, 0, 400, 300);
	inner.setOuterLayers(&outer.layers);
	CHECK(inner.kindFor(ui::SurfaceNeed::SpansGesture) == ui::SurfaceKind::Canvas);
	CHECK(inner.kindFor(ui::SurfaceNeed::SelfContained) == ui::SurfaceKind::Takeover);

	int told = 0;
	const ui::SurfaceId id = inner.present(plainFrame(), ui::SurfaceNeed::SpansGesture, "IN", [&] { ++told; });
	REQUIRE(id != ui::kNoSurface);
	REQUIRE(outer.layers.overlay().open());
	CHECK(outer.layers.overlay().panelBounds().x() == doctest::Approx(outer.layers.canvasRegion()->x()));
	CHECK_FALSE(inner.overlay().open());
	inner.dismiss(id);
	CHECK_FALSE(outer.layers.overlay().open());
	CHECK(told == 1);

	// FOLDED, CENTRED ON THE OUTER'S OVERLAY: the same press, no preference.
	outer.fold(true);
	CHECK(inner.kindFor(ui::SurfaceNeed::SpansGesture) == ui::SurfaceKind::Overlay);
	const ui::SurfaceId over = inner.present(plainFrame(), ui::SurfaceNeed::SpansGesture, "IN", [&] { ++told; });
	REQUIRE(over != ui::kNoSurface);
	CHECK(outer.layers.overlay().open());
	CHECK_FALSE(inner.overlay().open());
	inner.dismiss(over);
	CHECK_FALSE(outer.layers.overlay().open());
	CHECK(told == 2);
}

TEST_CASE("a forwarded panel comes off the INNER host's modulation list when it goes, whichever end closes it") {
	// FOUND BY THE SANITIZER GATE: a nested rack's panel registers its knobs on the INNER `ModState`
	// (the editor that built it), is retired on the OUTER overlay, and the inner `ModState` can die
	// first — a chain view dropping its editor — with the knobs still on it. `~FrameWatchers` asserts
	// exactly that. So the inner host forgets the mount on its own lists when the forwarded surface
	// goes, whether the inner asked (its editor's destructor) or the outer did (its ✕).
	//
	// **THE COUNTERS ARE DECLARED BEFORE THE HOSTS, SO THEY OUTLIVE THEM.** A dismissed panel is
	// PARKED, not freed — the overlay reaps it later so a handler inside it never runs on a freed
	// frame — and the last reap is the outer host's destructor. Declared inside each subcase, the
	// probe's `deaths` was out of scope by then (ASan, 2026-10-03: stack-use-after-scope in ~Probe).
	int deaths = 0;
	int told = 0;
	CanvasHost outer;
	ui::SurfaceLayers inner;
	ui::ModState innerMod;
	inner.setModulationState(&innerMod);
	inner.setOuterLayers(&outer.layers);

	SUBCASE("the inner dismisses") {
		auto probe = std::make_unique<Probe>(deaths);
		Probe* raw = probe.get();
		const ui::SurfaceId id =
		    inner.present(std::move(probe), ui::SurfaceNeed::SpansGesture, "IN", [&] { ++told; });
		REQUIRE(id != ui::kNoSurface);
		// A KNOB STANDS IN: something inside the panel on the inner modulation list.
		ui::Watch knob = innerMod.watchers().watch(raw);
		REQUIRE(innerMod.watchers().watches(raw));
		inner.dismiss(id);
		CHECK_FALSE(innerMod.watchers().watches(raw));
		CHECK(told == 1);
		CHECK_FALSE(outer.layers.overlay().open());
	}

	SUBCASE("the outer dismisses") {
		auto probe = std::make_unique<Probe>(deaths);
		Probe* raw = probe.get();
		REQUIRE(inner.present(std::move(probe), ui::SurfaceNeed::SpansGesture, "IN", [&] { ++told; })
		        != ui::kNoSurface);
		ui::Watch knob = innerMod.watchers().watch(raw);
		REQUIRE(innerMod.watchers().watches(raw));
		outer.layers.dismissPresented(); // its overlay holds it, arrived in the band since that shows
		CHECK_FALSE(innerMod.watchers().watches(raw));
		CHECK(told == 1);
	}
}

TEST_CASE("a mounted panel is advanced through the layers' tick, with the band open and folded alike") {
	// THE RACK USED TO KEEP ITS OWN THUNK BESIDE THE ID, and Feathers had no mount at all — two roads
	// for one panel. The mount carries the tick now; this proves it reaches the view either way.
	CanvasHost h;
	int deaths = 0;
	auto view = std::make_unique<TickedView>(deaths);
	TickedView* raw = view.get();
	const ui::SurfaceId id = h.present(std::move(view));
	REQUIRE(id != ui::kNoSurface);
	REQUIRE(h.layers.overlay().open());
	CHECK(h.layers.updateContent());
	CHECK(raw->ticks == 1);

	// FOLDED, STILL UP AND STILL TICKED: a fold no longer takes a panel down.
	h.fold(true);
	CHECK(h.layers.updateContent());
	CHECK(raw->ticks == 2);
	h.layers.dismiss(id);
	// FREED BY THE TICK, which is outside every dispatch — the case's proof that dismissal retires
	// rather than deletes, and that the retiree does not leak.
	(void)h.layers.updateContent();
	CHECK(deaths == 1);

	auto second = std::make_unique<TickedView>(deaths);
	TickedView* rawSecond = second.get();
	REQUIRE(h.present(std::move(second)) != ui::kNoSurface);
	REQUIRE(h.layers.overlay().open());
	CHECK(h.layers.updateContent());
	CHECK(rawSecond->ticks == 1);
}

TEST_CASE("a replacement issued from INSIDE the panel being replaced does not free it under the handler") {
	// THE DEFECT FOUND WHILE DESIGNING THE NESTING: the overlay
	// freed the retired frame AFTER retiring it in `show`, on the grounds that no dispatch was in
	// flight — true for a press on a card in the editor, false for a press on a card INSIDE the panel
	// (a nested rack presents through the outer host). Here the content itself issues the replacement,
	// which is that shape exactly; under ASan a use-after-free here is a report, and without ASan the
	// death count says whether the frame outlived the call it was made from.
	CanvasHost h;
	int deaths = 0;
	auto first = std::make_unique<Probe>(deaths);
	Probe* raw = first.get();
	REQUIRE(h.present(std::move(first)) != ui::kNoSurface);
	raw->onAct = [&h] { (void)h.present(); };
	// THE PRESS, FROM INSIDE: the first frame's own callback presents the second.
	raw->onAct();
	// STILL ALIVE — retired, not freed — until a draw or a tick, which is outside the handler.
	CHECK(deaths == 0);
	CHECK(raw->onAct != nullptr); // reading the retired frame: fine while it lives, a report if it did not
	(void)h.layers.updateContent();
	CHECK(deaths == 1);
	CHECK(h.layers.overlay().open());

	// AND WITH THE BAND FOLDED, THE SAME PROMISE.
	h.fold(true);
	(void)h.layers.updateContent();
	int overlayDeaths = 0;
	auto floating = std::make_unique<Probe>(overlayDeaths);
	Probe* rawFloating = floating.get();
	REQUIRE(h.present(std::move(floating)) != ui::kNoSurface);
	REQUIRE(h.layers.overlay().open());
	rawFloating->onAct = [&h] { (void)h.present(); };
	rawFloating->onAct();
	CHECK(overlayDeaths == 0);
	(void)h.layers.updateContent();
	CHECK(overlayDeaths == 1);
	CHECK(h.layers.overlay().open());
}

TEST_CASE("the canvas's geometry: a toggle row, a floor under it where a panel ARRIVES at its own size, and a strip when collapsed") {
	// The card floats over the window instead of stretching it. The panel is
	// the editor's overlay's, at the content's minimum, arriving on the floor (2026-10-03).
	CanvasHost h;
	const float w = h.canvas.extent();
	const visage::Bounds content = h.canvas.contentBounds();
	CHECK(content.y() == doctest::Approx(ui::ExpandedCanvas::kTitleHeight));
	CHECK(content.x() == doctest::Approx(ui::ExpandedCanvas::kPad));
	CHECK(content.width() == doctest::Approx(w - 2.0f * ui::ExpandedCanvas::kPad));
	CHECK(content.height() == doctest::Approx(600.0f - ui::ExpandedCanvas::kTitleHeight - ui::ExpandedCanvas::kPad));
	CHECK(h.canvas.toggleBounds().bottom() <= ui::ExpandedCanvas::kTitleHeight);

	// A MOUNTED VIEW KEEPS ITS OWN SIZE — NOT the floor's — and sits inside it.
	int deaths = 0;
	auto view = std::make_unique<TickedView>(deaths);
	REQUIRE(h.present(std::move(view)) != ui::kNoSurface);
	const ui::ExpandedView* mounted = h.layers.overlay().contentForTest();
	REQUIRE(mounted != nullptr);
	CHECK(mounted->width() == doctest::Approx(300.0f));
	// ITS OWN HEIGHT (the panel's content row, which is the minimum plus the panel's pad), and NOT the
	// floor's 560.
	CHECK(mounted->height() >= 200.0f);
	CHECK(mounted->height() < content.height() * 0.5f);
	const ui::ExpandedPanel* panel = h.layers.overlay().panelForTest();
	REQUIRE(panel != nullptr);
	CHECK(panel->isVisible());
	CHECK(CanvasHost::inside({ panel->x(), panel->y(), panel->width(), panel->height() }, *h.layers.canvasRegion()));

	// COLLAPSED: no floor at all, and the panel stays up.
	h.fold(true);
	CHECK(h.canvas.contentBounds().width() == doctest::Approx(0.0f));
	CHECK(h.layers.overlay().open());
	CHECK(h.told == 0);
}

TEST_CASE("Escape is the overlay's: the band takes no keys, and the panel's layer closes its front panel") {
	CanvasHost h;
	CHECK_FALSE(h.canvas.onKeyPress().callback(kEscape));
	REQUIRE(h.present() != ui::kNoSurface);
	CHECK_FALSE(h.canvas.onKeyPress().callback(kEscape));
	CHECK(h.layers.overlay().onKeyPress().callback(kEscape));
	CHECK_FALSE(h.layers.overlay().open());
	CHECK(h.told == 1);
}

TEST_CASE("a band's geometry: the strip is one rail tall, the grip lies along the edge that meets the chain") {
	ui::ExpandedCanvas bottom { ui::ExpandedCanvas::Edge::Bottom };
	// FOLDED BY DEFAULT, one strip tall; opened for the geometry below.
	CHECK(bottom.collapsed());
	CHECK(bottom.extent() == doctest::Approx(34.0f));
	REQUIRE(bottom.setCollapsed(false));
	bottom.setBounds(0, 0, 1000, static_cast<int>(bottom.extent()));
	CHECK(bottom.extent() == doctest::Approx(ui::ExpandedCanvas::kExtent));
	// THE INBOARD EDGE OF A BOTTOM BAND IS ITS TOP.
	CHECK(bottom.gripBounds().y() == doctest::Approx(0.0f));
	CHECK(bottom.gripBounds().width() == doctest::Approx(1000.0f));
	CHECK(bottom.contentBounds().y() == doctest::Approx(ui::ExpandedCanvas::kTitleHeight));
	REQUIRE(bottom.setCollapsed(true));
	CHECK(bottom.extent() == doctest::Approx(34.0f));
	bottom.setBounds(0, 0, 1000, 34);
	// THE TOGGLE SITS CENTRED IN THE STRIP, not hanging below it.
	CHECK(bottom.toggleBounds().bottom() <= 34.0f);

	ui::ExpandedCanvas top { ui::ExpandedCanvas::Edge::Top };
	REQUIRE(top.setCollapsed(false));
	top.setBounds(0, 0, 720, static_cast<int>(top.extent()));
	// THE INBOARD EDGE OF A TOP BAND IS ITS BOTTOM.
	CHECK(top.gripBounds().bottom() == doctest::Approx(top.extent()));
	CHECK(top.gripBounds().y() == doctest::Approx(top.extent() - ui::ExpandedCanvas::kGripSize));
}

TEST_CASE("the partition moves with the hand: towards the chain grows the canvas, clamped, told per step and once on release") {
	// The partition is movable, giving different balances and letting the canvas size vary. Driven through the frame's own handlers with window positions, which is what
	// a real drag delivers.
	ui::ExpandedCanvas canvas { ui::ExpandedCanvas::Edge::Bottom };
	REQUIRE(canvas.setCollapsed(false));
	canvas.setBounds(0, 700, 1000, static_cast<int>(canvas.extent()));
	int steps = 0;
	int commits = 0;
	canvas.onExtentDrag = [&](visage::Point) { ++steps; };
	canvas.onExtentCommit = [&] { ++commits; };
	const auto at = [](float x, float y, float wx, float wy) {
		visage::MouseEvent e;
		e.position = { x, y };
		e.window_position = { wx, wy };
		return e;
	};
	// A PRESS ON THE GRIP, then the hand moves UP by 100 — towards the chain — and the band grows.
	canvas.onMouseDown().callback(at(500.0f, 2.0f, 500.0f, 702.0f));
	REQUIRE(canvas.draggingExtentForTest());
	canvas.onMouseDrag().callback(at(500.0f, -98.0f, 500.0f, 602.0f));
	CHECK(canvas.openExtent() == doctest::Approx(ui::ExpandedCanvas::kExtent + 100.0f));
	CHECK(steps == 1);
	CHECK(commits == 0);
	// DOWN past the floor: clamped. The host is told every step regardless — it may put the partition
	// under the pointer itself (`partitionExtent`), so the logical clamp is not the last word.
	canvas.onMouseDrag().callback(at(500.0f, 0.0f, 500.0f, 3000.0f));
	CHECK(canvas.openExtent() == doctest::Approx(ui::ExpandedCanvas::kMinExtent));
	canvas.onMouseDrag().callback(at(500.0f, 0.0f, 500.0f, 3001.0f));
	CHECK(canvas.openExtent() == doctest::Approx(ui::ExpandedCanvas::kMinExtent));
	canvas.onMouseUp().callback(at(500.0f, 0.0f, 500.0f, 3001.0f));
	CHECK_FALSE(canvas.draggingExtentForTest());
	CHECK(commits == 1);
	// A PRESS OFF THE GRIP DRAGS NOTHING.
	canvas.onMouseDown().callback(at(500.0f, 200.0f, 500.0f, 900.0f));
	CHECK_FALSE(canvas.draggingExtentForTest());

	// THE HOST'S CEILING BINDS EVERY ROAD: a drag, a load, a number. THE DEFECT: the canvas blew up and
	// took over the entire view, unable to shrink back to normal.
	canvas.setMaxExtent(700.0f);
	CHECK(canvas.setOpenExtent(5000.0f));
	CHECK(canvas.openExtent() == doctest::Approx(700.0f));
	// AND A DOUBLE-CLICK ON THE GRIP IS THE WAY BACK: the default, committed at once.
	visage::MouseEvent twice = at(500.0f, 2.0f, 500.0f, 702.0f);
	twice.repeat_click_count = 1;
	const int committedBefore = commits;
	canvas.onMouseDown().callback(twice);
	CHECK_FALSE(canvas.draggingExtentForTest());
	CHECK(canvas.openExtent() == doctest::Approx(ui::ExpandedCanvas::kExtent));
	CHECK(commits == committedBefore + 1);
	// A TOP BAND GROWS THE OTHER WAY: the hand moves DOWN, towards the chain below it.
	ui::ExpandedCanvas top { ui::ExpandedCanvas::Edge::Top };
	REQUIRE(top.setCollapsed(false));
	top.setBounds(0, 0, 720, static_cast<int>(top.extent()));
	const float gripY = top.gripBounds().y() + 2.0f;
	top.onMouseDown().callback(at(300.0f, gripY, 300.0f, gripY));
	REQUIRE(top.draggingExtentForTest());
	top.onMouseDrag().callback(at(300.0f, gripY + 50.0f, 300.0f, gripY + 50.0f));
	CHECK(top.openExtent() == doctest::Approx(ui::ExpandedCanvas::kExtent + 50.0f));
}

TEST_CASE("the canvas tells its host when it folds, and only when something changed") {
	CanvasHost h;
	int changes = 0;
	h.canvas.onCollapsedChanged = [&] { ++changes; };
	CHECK_FALSE(h.canvas.setCollapsed(false));
	CHECK(changes == 0);
	CHECK(h.canvas.toggleCollapsed());
	CHECK(h.canvas.collapsed());
	CHECK(changes == 1);
	CHECK(h.canvas.toggleCollapsed());
	CHECK_FALSE(h.canvas.collapsed());
	CHECK(changes == 2);
}

//--------------------------------------------------------------------------------------------------//
// IDENTITY ACROSS NESTING — `ui::DeviceNest`.                                                        //
//--------------------------------------------------------------------------------------------------//

TEST_CASE("a device id is not an identity once chains nest: two panels for two devices that share an id, and two colours") {
	// **THE PREMISE IS A FACT ABOUT THIS BUILD, NOT A HYPOTHETICAL.**
	// `backplane/device_instance_id.h`: *"Today each of Feathers' chains counts separately from
	// `kFirstInstance`"*, and `rack::InnerChain` owns a whole `RackEngine` with an `InstanceCounter`
	// of its own — so the first device inside a held chain is instance 1, and so is the first device
	// of the host's own chain. Two different devices, one integer, one TITLE if they are the same kind.
	//
	// **WHAT THAT COST WAS NOT A COLOUR.** `ExpandedOverlay::keyOf` was `title#device` and the layer
	// keeps ONE PANEL PER KEY, so the second of the two opened RETIRED the first — which is exactly
	// what a hand opening SKLITTER-inside-SKLITTER would have watched happen to the panel it was
	// working in.
	CanvasHost h;
	REQUIRE(h.canvas.setCollapsed(true)); // everything floats, so one layer holds both

	const ui::DeviceNest inner = ui::DeviceNest::under({}, 4);
	auto outerView = std::make_unique<KeyedView>(1);
	auto innerView = std::make_unique<KeyedView>(1);
	// SAME TITLE, SAME DEVICE — the collision, stated rather than hoped for.
	REQUIRE(outerView->title() == innerView->title());
	REQUIRE(outerView->watchedDevice() == innerView->watchedDevice());
	innerView->setNest(inner);

	const uint32_t outerTint = ui::instanceTintFor(1);
	const uint32_t innerTint = ui::instanceTintFor(1, inner);

	const ui::SurfaceId a = h.present(std::move(outerView));
	REQUIRE(a != ui::kNoSurface);
	CHECK(h.layers.overlay().panelCount() == 1);
	const ui::SurfaceId b = h.present(std::move(innerView));
	REQUIRE(b != ui::kNoSurface);
	// **BOTH UP, AND NOBODY WAS TOLD THEIR PANEL WENT** — `told` counts the presenters a replacement
	// dismisses, so zero is the whole claim: the second did not stand on the first.
	CHECK(h.layers.overlay().panelCount() == 2);
	CHECK(h.told == 0);
	CHECK(h.layers.holds(a));
	CHECK(h.layers.holds(b));

	// **AND THEY WEAR DIFFERENT COLOURS, BY VALUE.** Computed from the derivation rather than read
	// back off the panels, so a wiring that tinted both from the same place would fail here.
	REQUIRE(h.layers.overlay().panelForTest() != nullptr);
	CHECK(h.layers.overlay().panelForTest()->tintForTest() == innerTint);
	CHECK(innerTint != outerTint);

	// THE SAME NEST AND THE SAME DEVICE IS STILL ONE PANEL — the rule the key exists to enforce did
	// not get weaker, it got correct. Without this the case above would pass with the key made unique
	// per `show`, which would open a twin for every press.
	auto again = std::make_unique<KeyedView>(1);
	again->setNest(inner);
	const ui::SurfaceId b2 = h.present(std::move(again));
	CHECK(h.layers.overlay().panelCount() == 2);
	CHECK(h.told == 1);
	CHECK_FALSE(h.layers.holds(b));
	CHECK(h.layers.holds(b2));
}

TEST_CASE("where a panel was left is remembered per CHAIN and device, so two panels that share an id do not fight over one place") {
	// The trap named in the brief for this work: the overlay remembers a placement per key, so if two
	// genuinely different devices share a key, dragging one moves the other's remembered place. It is
	// the same key defect as above wearing a smaller hat — and it is worth its own case because it
	// survives even when both panels happen to be open at once.
	CanvasHost h;
	REQUIRE(h.canvas.setCollapsed(true));
	const ui::DeviceNest inner = ui::DeviceNest::under({}, 4);

	auto nested = std::make_unique<KeyedView>(1);
	nested->setNest(inner);
	REQUIRE(h.present(std::move(nested)) != ui::kNoSurface);
	const visage::Bounds before = h.layers.overlay().panelBounds();
	h.layers.overlay().beginPanelDrag();
	h.layers.overlay().dragPanelBy({ 120.0f, 60.0f });
	const visage::Bounds moved = h.layers.overlay().panelBounds();
	REQUIRE(moved.x() != before.x()); // the drag is a real move, or the rest says nothing
	h.layers.overlay().dismiss();

	// THE HOST'S OWN DEVICE 1 OPENS WHERE IT WOULD HAVE OPENED, not where the nested one was dragged.
	REQUIRE(h.present(std::make_unique<KeyedView>(1)) != ui::kNoSurface);
	CHECK(h.layers.overlay().panelBounds().x() != moved.x());
	h.layers.overlay().dismiss();

	// AND THE NESTED ONE COMES BACK WHERE IT WAS LEFT — the other half, without which "they do not
	// share a place" could be satisfied by remembering nothing at all.
	auto again = std::make_unique<KeyedView>(1);
	again->setNest(inner);
	REQUIRE(h.present(std::move(again)) != ui::kNoSurface);
	CHECK(h.layers.overlay().panelBounds().x() == doctest::Approx(moved.x()));
	CHECK(h.layers.overlay().panelBounds().y() == doctest::Approx(moved.y()));
}

TEST_CASE("the nest's algebra: a chain never wears its holder's hue, and eight in a row are eight colours") {
	// **THE FLARE'S PROMISE, AS ARITHMETIC RATHER THAN AS A SCREENSHOT.** Eight hues cannot identify;
	// what they can do is make the pair a hand has on screen differ by construction. Both halves below
	// are the honest bound and neither is an approximation of "unique".
	CHECK(ui::DeviceNest {}.top());
	CHECK_FALSE(ui::DeviceNest::under({}, 1).top());

	// ONE CHAIN, EIGHT CONSECUTIVE IDS, EIGHT COLOURS — at the top level and at any depth.
	for (const ui::DeviceNest nest : { ui::DeviceNest {}, ui::DeviceNest::under({}, 3),
		                               ui::DeviceNest::under(ui::DeviceNest::under({}, 3), 7) }) {
		std::set<uint32_t> hues;
		for (uint32_t id = 1; id <= 8; ++id) {
			hues.insert(ui::instanceTintFor(id, nest));
		}
		CHECK(hues.size() == 8);
		// AND THE NINTH REPEATS THE FIRST, which is the bound `kInstanceTints` states out loud.
		CHECK(ui::instanceTintFor(9, nest) == ui::instanceTintFor(1, nest));
	}

	// **A CHAIN'S FIRST SEVEN DEVICES NEVER WEAR THE HUE OF THE DEVICE HOLDING THEM.** This is the
	// SKLITTER-inside-SKLITTER pair, and it is a guarantee rather than a 7-in-8 chance.
	//
	// **SEVEN AND NOT EIGHT, AND THE FIRST DRAFT OF THIS CASE SAID EIGHT AND WENT RED.** Eight hues
	// with one of them spent on the holder leaves seven; the EIGHTH device of a chain wears its
	// holder's colour exactly. That is the arithmetic and there is no arrangement of a fixed palette
	// that avoids it — which is why the bound is asserted here rather than described in a comment
	// nobody re-derives.
	for (uint32_t holder = 1; holder <= 20; ++holder) {
		const ui::DeviceNest nest = ui::DeviceNest::under({}, holder);
		for (uint32_t id = 1; id <= 7; ++id) {
			CHECK(ui::instanceTintFor(id, nest) != ui::instanceTintFor(holder));
		}
		CHECK(ui::instanceTintFor(8, nest) == ui::instanceTintFor(holder)); // the bound, stated
	}

	// AND THE PATH IS NEVER THE TOP-LEVEL PATH, whatever the holder — 0 names the host's own chain and
	// a hash landing there would make a nested device indistinguishable from an outer one.
	for (uint32_t holder = 0; holder < 5000; ++holder) {
		REQUIRE(ui::DeviceNest::under({}, holder).path != 0);
	}
	// DIFFERENT HOLDERS ARE DIFFERENT CHAINS, and nesting composes rather than collapsing.
	CHECK(ui::DeviceNest::under({}, 1).path != ui::DeviceNest::under({}, 2).path);
	CHECK(ui::DeviceNest::under(ui::DeviceNest::under({}, 1), 2).path
	      != ui::DeviceNest::under(ui::DeviceNest::under({}, 2), 1).path);
	CHECK(ui::DeviceNest::under(ui::DeviceNest::under({}, 1), 2).path != ui::DeviceNest::under({}, 2).path);
}
