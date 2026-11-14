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

// THE MODULATION PANEL, THE FX LANE'S FIRST TENANT, AND THE SOURCE GRID. Lines 11733–16674 of the one file, moved verbatim 2026-09-03; the seven mod-ring cases went to test_gui_widgets_mod_ring.cpp.

#include "fixtures.h"
#include "composite/composite_types.h"
#include "mod/lfo/lfo_types.h"
#include "plugin/ui/expanded_canvas.h"
#include "plugin/ui/menu_anchor.h"
#include "plugin/ui/spectrum_panel.h"

#include <array>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <string>
#include <unistd.h>

//------------------------//
// The modulation panel   //
//------------------------//


TEST_CASE("the strip costs the stage nothing, which is the measurement the design rests on") {
	// THE CLAIM THAT PUT THE PANEL HERE: the circle is the largest square that fits the stage, so it is
	// bound by the stage's HEIGHT, and the stage was 332px wider than the circle it held. If that is
	// true, spending 236 of them cannot move the disk — and if it is false, this feature has quietly
	// shrunk the instrument. Asserted against the assembled editor rather than against the arithmetic
	// that motivated it.
	ModFixture f;
	auto* perform = findInTree<ui::PerformStage>(f.root);
	REQUIRE(perform != nullptr);

	// WITH THE FX LANE CLOSED, which is the condition this claim was always about and which stopped
	// being the default the day the lane got a device (2026-08-18). The claim itself has not moved an
	// inch: spending 236 on the MODULATOR strip does not cost the disk. What changed is that a second
	// rail on the other edge now also asks for width by default, and measuring the strip's cost with
	// somebody else's lane open would be measuring the wrong thing. What the OPEN lane costs is
	// measured, in full, in the fx lane's own case.
	auto* fxLane = findInTree<ui::FxLane>(f.root);
	REQUIRE(fxLane != nullptr);
	fxLane->setCollapsed(true);

	// THE WINDOW LESS THE HEADER AND THE TRAY — **written as the arithmetic its own comment already
	// spelled out, rather than as the number that arithmetic came to.** It read `534 = 700 − header 52
	// − tray 114`, and the tray is `Knob::kHeight` plus its padding, so tightening the knob's line
	// spacing (line and knob/text spacing were too generous) moved a constant
	// this case had hard-coded. A test that states the relationship survives that; one that states the
	// result has to be edited every time a font or a gap moves, and each edit is a chance to write
	// down whatever the code now does.
	//
	// The epsilon stays: a physical-to-logical round trip on a fractional display scale costs a
	// fraction of a pixel.
	CHECK(perform->height()
	      == doctest::Approx(700.0f - ui::HeaderBar::kHeight - ui::KnobTray::kHeight).epsilon(0.005));
	// THE CLAIM, and not the tautology it can be mistaken for: the disk is half the stage's HEIGHT,
	// which is only true while the stage is the wider of the two. Had the strip squeezed the stage past
	// square, StageDisk's own min() would have taken the WIDTH instead and this would fail by exactly
	// what the strip cost.
	CHECK(perform->disk().radius()
	      == doctest::Approx(perform->height() * 0.5f - ui::StageDisk::kRimInset));
	CHECK(perform->width() > perform->height());
	CHECK(f.panel->isVisible());
	CHECK(f.panel->width() == doctest::Approx(ui::ModPanel::kWidth));

	// WHERE THE OLD "AND WHERE IT CANNOT BE AFFORDED IT IS NOT THERE AT ALL" SUBCASE WENT. It drove the
	// canvas to 708x400 logical and required the strip to vanish. Both halves are gone on purpose: that
	// canvas is not reachable any more (the floor IS the design canvas since syncContentScale stopped
	// waiting for a window), and the auto-hide it defended has been deleted in favour of the mock's own
	// minimise. What replaces it is the case below — the strip cannot leave at all — and #48's own
	// cases for the rail it becomes instead.
}

TEST_CASE("the modulator strip is never gone, at any size a host can ask for") {
	// THE DECISION, made into the property that replaces the auto-hide: if the macros hide when the
	// canvas is small, they hide to a minimised state, never to gone.
	//
	// The old rule took the whole strip away on a canvas it judged too small, which put four macro
	// knobs and the routing arm behind a resize with nothing on screen to say so. Now the only thing
	// that closes the strip is a hand on its own toggle, so every size a host can ask for — starting
	// at the floor, which is where the old rule used to fire — has the strip in its column.
	ModFixture f;
	auto* stack = findInTree<ui::SoundStack>(f.root);
	REQUIRE(stack != nullptr);

	const std::array<std::pair<float, float>, 5> canvases { { { 1240.0f, 700.0f },
		                                                      { 1240.0f, 800.0f },
		                                                      { 1400.0f, 900.0f },
		                                                      { 1920.0f, 1080.0f },
		                                                      { 2560.0f, 1400.0f } } };
	for (const auto& [logicalWidth, logicalHeight] : canvases) {
		INFO("logical canvas " << logicalWidth << "x" << logicalHeight);
		REQUIRE(f.gui.setSize(static_cast<uint32_t>(logicalWidth * f.scale),
		                      static_cast<uint32_t>(logicalHeight * f.scale)));
		CHECK(f.panel->isVisible());
		CHECK_FALSE(f.panel->collapsed());
		CHECK(f.panel->width() == doctest::Approx(ui::ModPanel::kWidth));
		// And in its column rather than merely on screen, which is the shape the old bug took.
		CHECK(stack->x() == doctest::Approx(ui::EngineRail::kWidth + ui::ModPanel::kWidth));
	}
}

TEST_CASE("closing the rail gives its width to the stage, and opening it gives it back") {
	// THE MECHANISM, through the real toggle at the real place. A closed rail is 34px of labelled
	// strip — the mock's `.min` width — and every pixel it gives up goes to the stage, because the
	// editor's column arithmetic is one sum over what each rail says it is currently worth.
	ModFixture f;
	auto* perform = findInTree<ui::PerformStage>(f.root);
	auto* stack = findInTree<ui::SoundStack>(f.root);
	REQUIRE(perform != nullptr);
	REQUIRE(stack != nullptr);

	const float openStage = perform->width();
	REQUIRE_FALSE(f.panel->collapsed());

	// The button the hand finds, not a rectangle this case restated.
	const auto clickToggle = [&] {
		const visage::Bounds toggle = f.panel->toggleBounds();
		const visage::Point at = f.panel->positionInWindow()
		                         + visage::Point { toggle.x() + toggle.width() * 0.5f,
			                                       toggle.y() + toggle.height() * 0.5f };
		dragThroughRoot(f.root, at, at);
	};

	clickToggle();
	CHECK(f.panel->collapsed());
	CHECK(f.panel->width() == doctest::Approx(ui::Rail::kCollapsedWidth));
	// THE WIDTH WENT SOMEWHERE, and this is where: the stage is wider by exactly what the rail gave
	// up, and the rail behind it has slid over by the same amount. A collapse that only hid the cards
	// would leave both of these unchanged.
	constexpr float kGiven = ui::ModPanel::kWidth - ui::Rail::kCollapsedWidth;
	CHECK(perform->width() == doctest::Approx(openStage + kGiven));
	CHECK(stack->x() == doctest::Approx(ui::EngineRail::kWidth + ui::Rail::kCollapsedWidth));
	// Still there, still labelled, still carrying the way back — the whole of "minimised, not gone".
	CHECK(f.panel->isVisible());
	CHECK(f.panel->hitAt({ f.panel->toggleBounds().x() + 2.0f, f.panel->toggleBounds().y() + 2.0f })
	          .part
	      == ui::ModPanel::Part::Collapse);

	clickToggle();
	CHECK_FALSE(f.panel->collapsed());
	CHECK(f.panel->width() == doctest::Approx(ui::ModPanel::kWidth));
	CHECK(perform->width() == doctest::Approx(openStage));
	CHECK(stack->x() == doctest::Approx(ui::EngineRail::kWidth + ui::ModPanel::kWidth));
}

TEST_CASE("a rail the user closed is still closed in the NEXT run of the host") {
	// The rail's state persists. The rail already survives the host closing and reopening
	// the editor window, because the editor object outlives its window. This is the other half, and it
	// is a different question with a different answer: surviving the PROCESS, which no object can do
	// by living longer.
	//
	// SO THE CASE THROWS THE EDITOR AWAY ENTIRELY. Not gui.destroy() — that is the reopen path, pinned
	// separately — but the whole Gui, its Impl, its panels and their bools, gone with the scope. What
	// the second editor knows, it can only have read off the disk.
	//
	// The file it reads is this binary's own — see main(), which points XDG_CONFIG_HOME at a temporary
	// directory. A case that wrote to the real one would be editing the machine it runs on.
	const std::filesystem::path file = EditorPrefs::path();
	REQUIRE_FALSE(file.empty());
	std::error_code code;
	std::filesystem::remove(file, code);

	{
		QuietGuiHost host;
		ParamBridge bridge;
	/// THE SAME BRIDGE AS THE DEVICE SEAM. Cards and panels are converging onto `backplane::DeviceHost`
	/// so the rack can build them; `BridgeDeviceHost` is what keeps Feathers handing them the parameter
	/// road underneath, and a fixture needs one for the same reason `Gui::Impl` does.
	feathers::BridgeDeviceHost deviceHost { bridge };
		bridge.refresh();
		Gui gui(host, bridge);
		REQUIRE(gui.create(CLAP_WINDOW_API_X11, false));
		REQUIRE(gui.setSize(Gui::kLogicalWidth, Gui::kLogicalHeight));
		auto* strip = findInTree<ui::ModPanel>(gui.root());
		REQUIRE(strip != nullptr);
		REQUIRE_FALSE(strip->collapsed());
		REQUIRE(strip->setCollapsed(true));
	}

	// It is on disk before anything reads it back, and it is a file a person could fix by hand.
	CHECK(std::filesystem::exists(file));

	{
		QuietGuiHost host;
		ParamBridge bridge;
	/// THE SAME BRIDGE AS THE DEVICE SEAM. Cards and panels are converging onto `backplane::DeviceHost`
	/// so the rack can build them; `BridgeDeviceHost` is what keeps Feathers handing them the parameter
	/// road underneath, and a fixture needs one for the same reason `Gui::Impl` does.
	feathers::BridgeDeviceHost deviceHost { bridge };
		bridge.refresh();
		Gui gui(host, bridge);
		REQUIRE(gui.create(CLAP_WINDOW_API_X11, false));
		REQUIRE(gui.setSize(Gui::kLogicalWidth, Gui::kLogicalHeight));
		auto* strip = findInTree<ui::ModPanel>(gui.root());
		REQUIRE(strip != nullptr);
		// THE CLAIM: a new process opens the editor the way the last one left it.
		CHECK(strip->collapsed());
		CHECK(strip->width() == doctest::Approx(ui::Rail::kCollapsedWidth));
		// And the columns were laid out FROM the remembered state rather than from the default and
		// then corrected — a rail placed at 236 and redrawn at 34 would leave the stack somewhere else.
		auto* stack = findInTree<ui::SoundStack>(gui.root());
		REQUIRE(stack != nullptr);
		CHECK(stack->x() == doctest::Approx(ui::EngineRail::kWidth + ui::Rail::kCollapsedWidth));
	}

	SUBCASE("and a file that is missing or nonsense is simply no preferences") {
		// PREFERENCES ARE NEVER WORTH AN ERROR. Whatever is in this file the editor opens, so the case
		// writes the worst thing it can think of and requires the defaults, silently.
		std::filesystem::create_directories(file.parent_path(), code);
		std::ofstream(file, std::ios::trunc) << "\x01\x02 not a config at all\n== = =\n\n";

		QuietGuiHost host;
		ParamBridge bridge;
	/// THE SAME BRIDGE AS THE DEVICE SEAM. Cards and panels are converging onto `backplane::DeviceHost`
	/// so the rack can build them; `BridgeDeviceHost` is what keeps Feathers handing them the parameter
	/// road underneath, and a fixture needs one for the same reason `Gui::Impl` does.
	feathers::BridgeDeviceHost deviceHost { bridge };
		bridge.refresh();
		Gui gui(host, bridge);
		REQUIRE(gui.create(CLAP_WINDOW_API_X11, false));
		auto* strip = findInTree<ui::ModPanel>(gui.root());
		REQUIRE(strip != nullptr);
		CHECK_FALSE(strip->collapsed());
	}
}

TEST_CASE("a rail the user closed stays closed through every resize") {
	// USER INTENT IS NOT A LAYOUT CONSEQUENCE. The auto-hide could not tell a panel the user had put
	// away from one the arithmetic had taken away, so any resize re-decided both. With it gone the
	// state has exactly one writer, and this is the case that says so: drag the editor through every
	// size in the sweep and the rail the hand closed is still closed, at the same width, in the same
	// column.
	ModFixture f;
	REQUIRE(f.panel->setCollapsed(true));

	const std::array<std::pair<float, float>, 6> canvases { { { 1240.0f, 700.0f },
		                                                      { 1240.0f, 900.0f },
		                                                      { 1600.0f, 1000.0f },
		                                                      { 1920.0f, 1080.0f },
		                                                      { 1300.0f, 760.0f },
		                                                      { 1240.0f, 700.0f } } };
	auto* stack = findInTree<ui::SoundStack>(f.root);
	REQUIRE(stack != nullptr);
	bool cycled = false;
	for (const auto& [logicalWidth, logicalHeight] : canvases) {
		INFO("logical canvas " << logicalWidth << "x" << logicalHeight);
		REQUIRE(f.gui.setSize(static_cast<uint32_t>(logicalWidth * f.scale),
		                      static_cast<uint32_t>(logicalHeight * f.scale)));
		CHECK(f.panel->collapsed());
		CHECK(f.panel->width() == doctest::Approx(ui::Rail::kCollapsedWidth));
		CHECK(stack->x() == doctest::Approx(ui::EngineRail::kWidth + ui::Rail::kCollapsedWidth));

		if (!cycled) {
			cycled = true;
			// THE HOST CLOSES THE EDITOR AND OPENS IT AGAIN, mid-sweep, because that is the shape the
			// persistence promise takes for a person: not a saved setting, but a window that comes back
			// the way it was left. It rides the editor object outliving its window — the one destroy()
			// deliberately does not tear down, see the Mesa swapchain note in create() — which is also
			// why the state may live in the panel and must never reach the patch.
			f.gui.destroy();
			REQUIRE(f.gui.create(CLAP_WINDOW_API_X11, false));
			// The SAME panel, not a rebuilt one: if create() ever starts building a fresh editor this
			// pointer is stale, and the case says so rather than reading a new object's default.
			REQUIRE(findInTree<ui::ModPanel>(f.gui.root()) == f.panel);
			CHECK(f.panel->collapsed());
			CHECK(f.panel->width() == doctest::Approx(ui::Rail::kCollapsedWidth));
		}
	}
}

TEST_CASE("the post-FX lane is a rail against the OTHER edge") {
	// The fx lane: right of the panel, collapsible. The mock's `#fxStrip`,
	// which is where the effect devices will dock when there are any — this is the LANE and not the
	// effects, so nothing here touches a parameter.
	//
	// THE THIRD HEIR, and the one that decides whether Rail was a mechanism or two panels sharing
	// code. Everything the left rails get for free has to work mirrored: the toggle on the lane's
	// INBOARD edge, which is its left; the chevrons pointing the other way; the width coming off the
	// stage's right rather than its left.
	ModFixture f;
	auto* lane = findInTree<ui::FxLane>(f.root);
	REQUIRE(lane != nullptr);
	auto* perform = findInTree<ui::PerformStage>(f.root);
	REQUIRE(perform != nullptr);

	// AGAINST THE RIGHT EDGE, full height between the header and the tray — the mock's last column.
	CHECK(lane->right() == doctest::Approx(f.root->width()));
	CHECK(lane->y() == doctest::Approx(ui::HeaderBar::kHeight));
	// ABOVE THE TRAY, WHICH IS ABOVE THE CANVAS BAND since 2026-09-13.
	CHECK(lane->bottom()
	      == doctest::Approx(f.root->height() - f.gui.canvasForTest()->height() - ui::KnobTray::kHeight));
	// And the stage stops where the lane starts rather than running under it.
	CHECK(perform->right() <= lane->x() + 0.5f);

	// IT ARRIVES OPEN, as of 2026-08-18, and the numbers below are the price of that word.
	//
	// It used to arrive closed, and this case used to pin that "so that nobody flips the default
	// without meeting the cost first". The cost has now been met rather than avoided: the lane holds
	// the multimode filter — a real card, over real parameters, drawing a curve computed from the
	// same evaluator the audio path uses — so the width it takes is buying something, which is the
	// exact condition gui.cpp's note set for this flip.
	//
	// WHAT IT COSTS, MEASURED AND PINNED so it cannot grow quietly: 374 of stage against 534 of
	// height at the design canvas, so StageDisk's min() takes the WIDTH and the circle is no longer
	// bound by the height. The disk's radius goes from 259 to 179. That is a real loss and it is
	// written down here as one, not smoothed over — and it is one toggle away from being undone by
	// any user who would rather have the circle.
	CHECK_FALSE(lane->collapsed());
	CHECK(lane->width() == doctest::Approx(ui::FxLane::kWidth));
	CHECK(perform->width() < perform->height());
	CHECK(perform->disk().radius()
	      == doctest::Approx(perform->width() * 0.5f - ui::StageDisk::kRimInset));
	// And the device is there — a lane that shipped open holding nothing would be the worst of both.
	REQUIRE(lane->filterCard() != nullptr);
	CHECK(lane->filterCard()->isVisible());

	// THE TOGGLE IS INBOARD, which for this rail means its LEFT half — the mirror of the modulator
	// strip's, and the whole of what Side::Right has to mean.
	const visage::Bounds toggle = lane->toggleBounds();
	CHECK(toggle.x() < lane->width() * 0.5f);
	CHECK(f.panel->toggleBounds().x() > f.panel->width() * 0.5f);

	SUBCASE("and its name is not written through its own toggle") {
		// FOUND BY THE CAMERA, pinned here so it stays found. On a rail against the right edge the
		// toggle sits at the INBOARD end — the same end a heading starts from — and the first render
		// of this lane drew "POST-FX" straight through the chevron: "P⟩ST-FX". Every geometry case
		// passed, because none of them had asked whether two rectangles the panel hands out overlap.
		REQUIRE_FALSE(lane->collapsed()); // it ships open now; this is the state the fault appeared in
		const visage::Bounds heading = lane->headingBounds();
		const visage::Bounds toggle = lane->toggleBounds();
		CHECK(heading.x() >= toggle.right());
		CHECK(heading.width() > 0.0f);
		CHECK(heading.right() <= lane->width() + 0.5f);
	}

	SUBCASE("and closing it gives that width back, like any other rail in reverse") {
		const float openStage = perform->width();
		REQUIRE(lane->pressCollapse({ toggle.x() + 2.0f, toggle.y() + 2.0f }));
		CHECK(lane->collapsed());
		CHECK(lane->width() == doctest::Approx(ui::Rail::kCollapsedWidth));
		CHECK(lane->right() == doctest::Approx(f.root->width()));
		constexpr float kGave = ui::FxLane::kWidth - ui::Rail::kCollapsedWidth;
		CHECK(perform->width() == doctest::Approx(openStage + kGave));
		// The left rails did not move: a lane closing on the right must not shuffle the left column.
		CHECK(f.panel->x() == doctest::Approx(ui::EngineRail::kWidth));

		// AND THE DISK COMES BACK, which is the other half of the price being a price rather than a
		// one-way door: closed, the stage is wider than it is tall again and StageDisk's min() takes
		// the height, exactly as it did before this lane existed.
		CHECK(perform->width() > perform->height());
		CHECK(perform->disk().radius()
		      == doctest::Approx(perform->height() * 0.5f - ui::StageDisk::kRimInset));
	}

	SUBCASE("and it is remembered like the others, with one key and no new mechanism") {
		REQUIRE(lane->setCollapsed(true));
		const std::filesystem::path file = EditorPrefs::path();
		REQUIRE(std::filesystem::exists(file));

		QuietGuiHost host;
		ParamBridge bridge;
	/// THE SAME BRIDGE AS THE DEVICE SEAM. Cards and panels are converging onto `backplane::DeviceHost`
	/// so the rack can build them; `BridgeDeviceHost` is what keeps Feathers handing them the parameter
	/// road underneath, and a fixture needs one for the same reason `Gui::Impl` does.
	feathers::BridgeDeviceHost deviceHost { bridge };
		bridge.refresh();
		Gui gui(host, bridge);
		REQUIRE(gui.create(CLAP_WINDOW_API_X11, false));
		REQUIRE(gui.setSize(Gui::kLogicalWidth, Gui::kLogicalHeight));
		auto* next = findInTree<ui::FxLane>(gui.root());
		REQUIRE(next != nullptr);
		// CLOSED here, so a stored `true` has to beat the now-OPEN default — which is the half of the
		// store a case can only exercise on a key whose default is not the stored value. The direction
		// flipped with the default in 2026-08-18; the claim did not.
		CHECK(next->collapsed());
	}
}

TEST_CASE("the Engine rail is a rail too, and the two of them are one mechanism") {
	// THE MECHANISM IS GENERIC OR IT IS THREE COPIES. The Engine rail inherits the same base, so this
	// asks it the same questions — and then closes BOTH, which is the arrangement that would catch a
	// column sum written as "the Engine rail's width plus whatever the strip is doing".
	ModFixture f;
	auto* engineRail = findInTree<ui::EngineRail>(f.root);
	auto* stack = findInTree<ui::SoundStack>(f.root);
	auto* perform = findInTree<ui::PerformStage>(f.root);
	REQUIRE(engineRail != nullptr);
	REQUIRE(stack != nullptr);
	REQUIRE(perform != nullptr);

	const float openStage = perform->width();
	REQUIRE(engineRail->pressCollapse(
	    { engineRail->toggleBounds().x() + 2.0f, engineRail->toggleBounds().y() + 2.0f }));
	CHECK(engineRail->collapsed());
	CHECK(engineRail->width() == doctest::Approx(ui::Rail::kCollapsedWidth));
	// Everything outboard of it moved by what it gave up, and nothing else did.
	constexpr float kEngineRailGave = ui::EngineRail::kWidth - ui::Rail::kCollapsedWidth;
	CHECK(f.panel->x() == doctest::Approx(ui::Rail::kCollapsedWidth));
	CHECK(stack->x() == doctest::Approx(ui::Rail::kCollapsedWidth + ui::ModPanel::kWidth));
	CHECK(perform->width() == doctest::Approx(openStage + kEngineRailGave));

	SUBCASE("and with both rails closed the stage has both of their widths") {
		REQUIRE(f.panel->setCollapsed(true));
		constexpr float kBothGave = kEngineRailGave + ui::ModPanel::kWidth - ui::Rail::kCollapsedWidth;
		CHECK(perform->width() == doctest::Approx(openStage + kBothGave));
		CHECK(stack->x() == doctest::Approx(2.0f * ui::Rail::kCollapsedWidth));
		// Both still on screen, both still one click from coming back.
		CHECK(engineRail->isVisible());
		CHECK(f.panel->isVisible());
	}
}

TEST_CASE("a closed rail reshapes the stage, and Explore's flanks follow without being told") {
	// V16'S AFFORDANCE, and the reason this is a case rather than a comment claiming composition:
	// ExploreStage reserves a full-height column beside its chart square for the width knob and banks
	// its mirror, all derived from the frame the stage is given. gui.cpp decides where the stage IS;
	// explore decides what happens inside it. Closing a rail moves the first, and this asks whether
	// the second followed — the one-geometry-authority claim, measured rather than asserted.
	ModFixture f;
	auto* explore = findInTree<ui::ExploreStage>(f.root);
	REQUIRE(explore != nullptr);

	const auto flanksAreSane = [&](const char* when) {
		INFO(when);
		const visage::Bounds left = explore->flankColumn(ui::ExploreStage::Flank::Left);
		const visage::Bounds right = explore->flankColumn(ui::ExploreStage::Flank::Right);
		// Inside the stage it was handed, both of them, with the mirror actually mirrored.
		CHECK(left.x() >= -0.5f);
		CHECK(right.right() <= explore->width() + 0.5f);
		CHECK(left.width() == doctest::Approx(right.width()));
		CHECK(left.height() == doctest::Approx(explore->height()));
		// And the knob that lives in the left column keeps a real on-stage rectangle — V16's own
		// promise, and what would break if the stage moved and the column did not follow.
		const visage::Bounds knob = explore->widthKnobBounds();
		CHECK(knob.width() > 0.0f);
		CHECK(knob.x() >= -0.5f);
		CHECK(knob.right() <= explore->width() + 0.5f);
	};

	flanksAreSane("both rails open");
	REQUIRE(f.panel->setCollapsed(true));
	flanksAreSane("modulator rail closed");
	auto* engineRail = findInTree<ui::EngineRail>(f.root);
	REQUIRE(engineRail != nullptr);
	REQUIRE(engineRail->setCollapsed(true));
	flanksAreSane("both left rails closed");

	// AND NOW THE OTHER SIDE, which is what the fx lane added to this question: the stage yields
	// width from BOTH edges, so its rectangle can move and narrow at the same time. Explore's flanks
	// are derived from the frame it is handed and know nothing about either — which is the claim.
	auto* lane = findInTree<ui::FxLane>(f.root);
	REQUIRE(lane != nullptr);
	REQUIRE_FALSE(lane->collapsed()); // it ships open, so this arrangement is already the live one
	flanksAreSane("left rails closed, fx lane open");
	REQUIRE(f.panel->setCollapsed(false));
	REQUIRE(engineRail->setCollapsed(false));
	flanksAreSane("everything open at once — the narrowest stage there is");
	REQUIRE(lane->setCollapsed(true));
	flanksAreSane("every rail but the fx lane, which is the widest stage a user can ask for");
	REQUIRE(lane->setCollapsed(false));
	flanksAreSane("back to the shipped arrangement");
}

TEST_CASE("a canvas that GREW cannot lose the strip") {
	// THE BUG: dragging the editor larger made the MODULATORS strip disappear, with the sound
	// rail sliding left into the column it had been in.
	//
	// The yield rule asked whether the strip cost the disk anything — stage width minus the strip
	// against stage height — and that is a question about the canvas's SHAPE, not its size. Every
	// canvas taller than about 5:3 fails it however big it is, and the content scale makes taller the
	// normal way to grow: it pins the binding dimension at the design size and lets the other one
	// run, so dragging the bottom edge down holds the logical width at 1240 and pushes the logical
	// height past what the rule allows. Measured at 1240x800 logical, where the old rule left the
	// stage 629 wide against a stage 633 tall and took the strip away by four pixels.
	//
	// So the case is the property rather than the one canvas that broke: from the design canvas, every
	// direction a hand can drag is at least as big in both dimensions, and the strip survives all of
	// them.
	ModFixture f;
	REQUIRE(f.panel->isVisible());
	auto* stack = findInTree<ui::SoundStack>(f.root);
	REQUIRE(stack != nullptr);

	// Taller-only first, because that is the drag that reported the bug, then wider-and-taller in the
	// proportions of the displays this editor actually opens on.
	const std::array<std::pair<float, float>, 6> grown { { { 1240.0f, 800.0f },
		                                                   { 1300.0f, 900.0f },
		                                                   { 1400.0f, 1050.0f },
		                                                   { 1600.0f, 1000.0f },
		                                                   { 1920.0f, 1080.0f },
		                                                   { 2560.0f, 1400.0f } } };
	for (const auto& [logicalWidth, logicalHeight] : grown) {
		INFO("logical canvas " << logicalWidth << "x" << logicalHeight);
		REQUIRE(f.gui.setSize(static_cast<uint32_t>(logicalWidth * f.scale),
		                      static_cast<uint32_t>(logicalHeight * f.scale)));
		CHECK(f.panel->isVisible());
		// AND IT IS IN ITS COLUMN, not merely visible: the symptom in the photograph is the rail's
		// position, so that is what the case reads.
		CHECK(stack->x() == doctest::Approx(ui::EngineRail::kWidth + ui::ModPanel::kWidth));
	}
}

TEST_CASE("a folded card is its head row, and what is under it comes up to meet it") {
	// Per-card minimise. The mock's finer collapse — `.mod-card.min > *:not(.head)` —
	// driven through the real button in the real head row.
	//
	// A FOLD IS A DEVICE CARD'S VERB, and since 2026-08-22 it is ONLY a device card's: the macros
	// became a permanent header row, because they are the mod-IN rather than something the rack
	// holds, and a header row has no fold button. So this case moved from macro 0 to the envelope,
	// which is the thing that still has a card.
	//
	// WHAT IS BELOW IT MOVES, which is the half a fold could plausibly get wrong: a card whose height
	// changed while everything under it stayed put would leave a hole, and a hit test that still
	// believed the old geometry would answer for the wrong thing.
	ModFixture f;
	const backplane::Source envelope = backplane::Source::output(f.bridge.rackDeviceAt(0), 0);
	const float openHeight = f.panel->sourceHeight(envelope);
	const float openContent = f.panel->contentHeight();
	const visage::Bounds cvBefore = f.panel->cvCardBounds();
	REQUIRE_FALSE(f.panel->cardMinimised(envelope));
	REQUIRE(openHeight > ui::ModPanel::kHeadHeight);

	// **BY DOUBLE-CLICKING THE HEAD, BECAUSE THE `(-)` MARK IS GONE** — with a double-click, the mark is
	// redundant. This drove a press at `foldBounds`, which now answers an
	// empty rectangle — a mark that is not drawn must not be hit-testable either, or it is an
	// invisible control. The VERB is unchanged and that is what the assertions below still check.
	const visage::Bounds head = f.panel->sourceBounds(envelope);
	const visage::Point at = f.panel->positionInWindow()
	                         + visage::Point { head.x() + head.width() * 0.5f,
		                                       head.y() + ui::ModPanel::kHeadHeight * 0.5f };
	doubleClickHead(f.root, at);

	CHECK(f.panel->cardMinimised(envelope));
	CHECK(f.panel->sourceHeight(envelope) == doctest::Approx(ui::ModPanel::kHeadHeight));
	// The CV card below came up by exactly what the folded one gave up.
	CHECK(f.panel->cvCardBounds().y()
	      == doctest::Approx(cvBefore.y() - (openHeight - ui::ModPanel::kHeadHeight)));
	// AND THE SCROLL FOLLOWS, which is the interaction that would otherwise let the strip scroll into
	// empty rail: contentHeight is the number maxScroll is measured against.
	CHECK(f.panel->contentHeight()
	      == doctest::Approx(openContent - (openHeight - ui::ModPanel::kHeadHeight)));
	// And the card is still a card — its head answers, and folding it again brings it back. THE
	// ROUND TRIP is what the old `Part::Fold` hit test was really asserting, and it survives the mark:
	// a verb reachable only one way is a verb one refactor from being unreachable.
	// **THE POINT IS RECOMPUTED, because folding MOVED the head.** `at` was taken from the open card;
	// once minimised the strip re-lays out and everything below comes up to meet it — which is what the
	// assertions above just checked. Re-using the stale point aims at whatever now occupies that y.
	const auto headCentre = [&] {
		const visage::Bounds b = f.panel->sourceBounds(envelope);
		return f.panel->positionInWindow()
		       + visage::Point { b.x() + b.width() * 0.5f, b.y() + ui::ModPanel::kHeadHeight * 0.5f };
	};
	doubleClickHead(f.root, headCentre());
	CHECK_FALSE(f.panel->cardMinimised(envelope));
	CHECK(f.panel->sourceHeight(envelope) == doctest::Approx(openHeight));
	doubleClickHead(f.root, headCentre());
	CHECK(f.panel->cardMinimised(envelope));

	// **BUT NOT THE ARROW, AND THAT IS THE POINT OF THE MOVE RATHER THAN A LOSS.** This used to check
	// that "both of its buttons still answer", which was true while the arm lived in the HEAD — the
	// one part a fold keeps. It moved out: the mod-assign arrow left the head (as in the Feathers synth)
	// and the mod outputs sit, by convention, at the bottom of the card. The outputs strip is BODY, so it folds away with the device's own surface, and
	// `sourceHeight`'s note already said what that means — *"an arrow that survived a fold would be
	// the head carrying an output again by the back door."
	CHECK(f.panel->arrowBounds(envelope).width() == doctest::Approx(0.0f));
	// AND A MACRO HAS NO FOLD AT ALL, which is the other half of the decision: the header row is not a
	// card, so asking it to fold is not a gesture the strip offers.
	CHECK(f.panel->foldBounds(macroSourceAt(0)).width() == doctest::Approx(0.0f));
	CHECK_FALSE(f.panel->cardMinimised(macroSourceAt(0)));
	CHECK_FALSE(f.panel->setCardMinimised(macroSourceAt(0), true));

	SUBCASE("and a folded card's body is not on screen to be touched — but its head is") {
		// **THIS ASSERTED `CHECK_FALSE(card->isVisible())`, WHICH WAS THE BUG WRITTEN DOWN AS THE
		// RULE (backlog CZ).** The strip hid a minimised card whole, and since the card frame draws
		// the head, a folded modulator was a blank band — MEASURED on the installed build. A folded
		// card is FOLDED now, the fx lane's way: visible at head height, its boxes hidden, so the
		// body is not on screen to be touched and the head is there to be double-clicked back open.
		auto* card = findInTree<ui::EnvCard>(f.panel);
		REQUIRE(card != nullptr);
		// THROUGH THE FRAME, NOT THE CARD: `EnvCard::height()` is a static constant (the body's
		// 144) that shadows `visage::Frame::height()` on an `EnvCard*` — MEASURED, it read 144 on a
		// frame that `laneHeight()` and the strip both put at 24.
		visage::Frame* frame = card;
		CHECK(frame->isVisible());
		CHECK(card->folded());
		CHECK(static_cast<float>(frame->height()) == doctest::Approx(ui::kCardHeadHeight));
		CHECK(frame->width() > 0.0f);
		for (visage::Frame* child : card->children()) {
			CHECK_FALSE(child->isVisible());
		}
	}

	SUBCASE("and the macro row is never hidden by a fold, because it is not a card") {
		// THE HEADER ROW IS PERMANENT. It is the mod-IN — the only modulation a host with no
		// modulation lane of its own can give this plugin — so no per-card verb may take it away.
		ui::Knob* dial = f.knobFor(static_cast<ParamId>(macroParam(0)));
		REQUIRE(dial != nullptr);
		CHECK(dial->isVisible());
	}

	SUBCASE("and folding arms nothing, which is the mock's stopPropagation") {
		// A hand tidying a card away has said nothing about which macro it is pointing, so no knob
		// anywhere may become mappable.
		std::vector<ui::Knob*> knobs;
		collectKnobs(f.root, &knobs);
		REQUIRE_FALSE(knobs.empty());
		for (ui::Knob* knob : knobs) {
			CHECK_FALSE(knob->mappable());
		}
	}
}

TEST_CASE("folds and the rail are remembered separately — the rail on the DESK, the folds in the PATCH") {
	// The two collapses compose rather than compete: the rail is "put this panel away", a fold is "I
	// am done with this macro". **THEY ARE ALSO FACTS ABOUT DIFFERENT THINGS, and this case used to say
	// otherwise.** It asserted that BOTH survived the process through `EditorPrefs`, which is the desk
	// file — and a desk outlives every patch, so the only key it could give a fold was the card's
	// POSITION in the strip. A reorder then handed the fold to whichever card arrived at that number.
	//
	// **DECIDED, 2026-09-12: a fold lives in the patch.** A rail is state about a WINDOW and stays on
	// the desk; a fold is state about a DEVICE and travels with it (`rack_state.h` argues the line). So
	// the claim splits, and the half that changed is asserted in both directions here: the rail still
	// comes back from the file, and the folds do NOT — they come back with the patch, or not at all.
	const std::filesystem::path file = EditorPrefs::path();
	std::error_code code;
	std::filesystem::remove(file, code);

	// A FULL RACK, BECAUSE A FOLD BELONGS TO A CARD AND A CARD EXISTS ONLY WHERE A DEVICE IS. This
	// used to fold slots 1 and 3 of a rack holding ONE device: the panel converted a source index to
	// a slot by arithmetic, so it happily remembered a fold for two cards that were not there. It now
	// asks the rack which slot holds the device that source names, and a device nothing holds answers
	// nothing — so the case has to put the devices in before it can fold their cards.
	const auto fillRack = [](ParamBridge& bridge) {
		while (bridge.modRack().count < feathers::kMaxRackSlots) {
			REQUIRE(bridge.addRackDevice(feathers::mod::dahdsr::kTraits.tag)
			        < feathers::kMaxRackSlots);
		}
	};
	/// The card a rack slot draws, named the way the strip names it: by the SOURCE its device
	/// publishes, which is addressed by the device's id rather than by its place in the
	/// rack.
	const auto cardOf = [](const ParamBridge& bridge, size_t slot) {
		return backplane::Source::output(bridge.rackDeviceAt(slot), 0);
	};

	// THE PATCH THE FIRST PROCESS SAVES, carried across by hand because that is what a host does with
	// it: the desk file crosses on its own, the patch crosses only if somebody saves and loads it.
	feathers::rack::RackPatch saved;

	{
		QuietGuiHost host;
		ParamBridge bridge;
	/// THE SAME BRIDGE AS THE DEVICE SEAM. Cards and panels are converging onto `backplane::DeviceHost`
	/// so the rack can build them; `BridgeDeviceHost` is what keeps Feathers handing them the parameter
	/// road underneath, and a fixture needs one for the same reason `Gui::Impl` does.
	feathers::BridgeDeviceHost deviceHost { bridge };
		fillRack(bridge);
		bridge.refresh();
		Gui gui(host, bridge);
		REQUIRE(gui.create(CLAP_WINDOW_API_X11, false));
		auto* strip = findInTree<ui::ModPanel>(gui.root());
		REQUIRE(strip != nullptr);
		REQUIRE(strip->setCardMinimised(cardOf(bridge, 1), true));
		REQUIRE(strip->setCardMinimised(cardOf(bridge, 3), true));
		// THE RAIL, PUT AWAY — the half that IS a desk fact, and the half this case still asserts
		// crosses the process on its own.
		strip->setCollapsed(true);
		REQUIRE(strip->collapsed());
		saved = bridge.chainsPatch();
	}

	{
		QuietGuiHost host;
		ParamBridge bridge;
	feathers::BridgeDeviceHost deviceHost { bridge };
		fillRack(bridge);
		bridge.refresh();
		Gui gui(host, bridge);
		REQUIRE(gui.create(CLAP_WINDOW_API_X11, false));
		auto* strip = findInTree<ui::ModPanel>(gui.root());
		REQUIRE(strip != nullptr);

		// **THE RAIL CAME BACK FROM THE FILE.** Nothing in this process asked for it.
		CHECK(strip->collapsed());

		// **AND NO FOLD DID.** This is the deletion, asserted rather than assumed: a fresh instance that
		// has loaded no patch has no folds, whatever the desk remembers. It is also why keying the desk
		// by device id would have been worse than keying it by position — these devices wear the same
		// ids the first process's did, so a desk keyed that way would fold cards 1 and 3 right here.
		for (size_t slot = 0; slot < feathers::kMaxRackSlots; ++slot) {
			CAPTURE(slot);
			CHECK_FALSE(strip->cardMinimised(cardOf(bridge, slot)));
		}

		// **THE FOLDS ARRIVE WITH THE PATCH, PER CARD** — which is the claim this case has always been
		// about; only the road changed. A single "some cards are folded" flag would bring the wrong
		// ones back, so all eight slots are asked and two of them answer yes.
		REQUIRE(bridge.restoreChains(saved).routes.count == 0);
		bridge.refresh();
		strip->resized();
		int folded = 0;
		for (size_t slot = 0; slot < feathers::kMaxRackSlots; ++slot) {
			CAPTURE(slot);
			const bool expected = slot == 1 || slot == 3;
			CHECK(strip->cardMinimised(cardOf(bridge, slot)) == expected);
			folded += strip->cardMinimised(cardOf(bridge, slot)) ? 1 : 0;
		}
		CHECK(folded == 2);
		// The rail is still the rail: loading a patch is not a statement about the window.
		CHECK(strip->collapsed());
	}
}

TEST_CASE("the strip scrolls rather than leaving its last card off the end") {
	// THE STRIP FITS AT ITS NATURAL SIZE AGAIN, which is worth saying out loud because this case used
	// to open by asserting it did not. Four macro CARDS were 408px of a 534px rail and the CV card
	// hung off the end by 14; the header row is under a hundred, so the whole strip — row, envelope
	// card and CV placeholder — now comes to about 375 and there is nothing to scroll.
	//
	// The contract still has to hold for the day there is: a second device, or a shorter rail. So the
	// case makes the content overflow deliberately rather than relying on it doing so by accident,
	// which is also the more honest test — it says what condition it is about.
	ModFixture f;
	REQUIRE(f.panel->cvCardBounds().bottom() <= f.panel->height());
	f.panel->setBounds(f.panel->x(), f.panel->y(), ui::ModPanel::kWidth, 200.0f);
	f.panel->setScroll(0.0f);
	REQUIRE(f.panel->cvCardBounds().bottom() > f.panel->height());
	const float cvBeforeScrolling = f.panel->cvCardBounds().bottom();

	visage::MouseEvent wheel = at(ui::ModPanel::kWidth * 0.5f, f.panel->height() * 0.5f);
	wheel.wheel_delta_y = -1.0f;
	wheel.precise_wheel_delta_y = -1.0f;
	CHECK(f.panel->mouseWheel(wheel));
	// ONE NOTCH MOVES IT, which is the claim — it used to be that one notch was enough to bring the
	// CV card fully in, and that was only true because the overflow was 14px. It is a strip taller
	// than its rail now, so what a notch owes is progress rather than arrival.
	CHECK(f.panel->scroll() > 0.0f);
	const float afterOneNotch = f.panel->cvCardBounds().bottom();
	CHECK(afterOneNotch < cvBeforeScrolling);
	// And the end of the strip IS reachable, which is the contract the notch is only a step toward.
	f.panel->setScroll(f.panel->maxScroll());
	CHECK(f.panel->cvCardBounds().bottom() <= f.panel->height());

	SUBCASE("and the contract is the sound rail's — both ends stop") {
		f.panel->setScroll(-50.0f);
		CHECK(f.panel->scroll() == doctest::Approx(0.0f));
		f.panel->setScroll(10000.0f);
		CHECK(f.panel->scroll() == doctest::Approx(f.panel->maxScroll()));
		// Scrolled to the end, the strip ends with the padding it starts with rather than with the CV
		// card's border against the frame.
		CHECK(f.panel->cvCardBounds().bottom()
		      == doctest::Approx(f.panel->height() - ui::ModPanel::kPadTop));
	}

	SUBCASE("a short strip still reaches its last card, and a hit test follows the scroll") {
		// Deliberately shorter than anything a window can produce: the contract is about content that
		// cannot fit, and 14px of overflow is too small a margin to be sure of what a case proves.
		f.panel->setBounds(f.panel->x(), f.panel->y(), ui::ModPanel::kWidth, 200.0f);
		f.panel->setScroll(0.0f);
		REQUIRE(f.panel->maxScroll() > 0.0f);

		// THE MACRO ROW IS THE FIRST THING IN THE STRIP since it stopped being four cards, so the
		// scroll's effect is measured on it — a column's own coordinate, and the dial that lives in it.
		const visage::Bounds firstColumn = f.panel->macroColumnBounds(0);
		const visage::Point onFirst { firstColumn.x() + firstColumn.width() * 0.5f,
			                          firstColumn.y() + firstColumn.height() - 4.0f };
		REQUIRE(f.panel->hitAt(onFirst).source == macroSourceAt(0));

		std::vector<ui::Knob*> dials;
		collectKnobs(f.panel, &dials);
		REQUIRE(dials.size() == kMacroCount);
		// IN THE PANEL'S COORDINATES: the dial sits inside its macro's `MacroSlot` (A7), which is the
		// frame the panel places, so its own y is the slot's offset and never moves.
		const auto dialY = [&f](const visage::Frame* dial) {
			float y = 0.0f;
			for (const visage::Frame* frame = dial; frame != nullptr && frame != f.panel; frame = frame->parent()) {
				y += frame->y();
			}
			return y;
		};
		const float dialBefore = dialY(dials[0]);

		const float pitch = ui::ModPanel::kMacroRowHeight + ui::ModPanel::kCardGap;
		f.panel->setScroll(pitch);
		// THE DIAL WENT WITH ITS ROW, which is the half a list of CHILD FRAMES gets wrong: the knobs
		// are placed rather than drawn, so a strip that scrolled its own painting and left its
		// children behind would look right until you tried to turn one.
		CHECK(dialY(dials[0]) == doctest::Approx(dialBefore - pitch));
		// And the geometry moved with it, so a hit test follows without being told.
		CHECK(f.panel->macroColumnBounds(0).y() == doctest::Approx(firstColumn.y() - pitch));

		f.panel->setScroll(f.panel->maxScroll());
		CHECK(f.panel->cvCardBounds().bottom() <= f.panel->height());

		// NOTHING IS UNREACHABLE — "every card reachable from SOME scroll position", which is the
		// contract as stated, and the scan has to be over scroll positions to match it.
		//
		// IT USED TO SCAN AT MAX SCROLL ALONE and that happened to work because the overflow was 14px,
		// so everything was on screen at the bottom. It is a real strip now: at max scroll the device
		// card's head row has gone off the TOP, which is correct behaviour and would have read as an
		// unreachable card.
		bool foundCard = false;
		for (float scrollTo = 0.0f; scrollTo <= f.panel->maxScroll() && !foundCard; scrollTo += 8.0f) {
			f.panel->setScroll(scrollTo);
			for (float y = 0.0f; y < 200.0f; y += 2.0f) {
				const ui::ModPanel::Hit hit = f.panel->hitAt({ onFirst.x, y });
				// `Part::Fold` IS WHAT THE HEAD ANSWERS NOW. It was `Part::Card` — the head rectangle
				// arming the source — until the 2026-08-24 decision moved arming onto the output and gave
				// the head to the fold outright. The CLAIM is unchanged: a device card's head is
				// reachable from some scroll position.
				if (hit.part == ui::ModPanel::Part::Fold && hit.source.isDevice()) {
					foundCard = true;
					break;
				}
			}
		}
		CHECK(foundCard);
	}

	SUBCASE("and a strip with room for everything is not left scrolled") {
		f.panel->setBounds(f.panel->x(), f.panel->y(), ui::ModPanel::kWidth, 200.0f);
		f.panel->setScroll(f.panel->maxScroll());
		REQUIRE(f.panel->scroll() > 0.0f);

		// Growing the editor is exactly what puts the rail back — see modPanelFits — and a scroll left
		// where it was would hold the heading off the top of a strip that now fits whole.
		f.panel->setBounds(f.panel->x(), f.panel->y(), ui::ModPanel::kWidth, 1000.0f);
		CHECK(f.panel->maxScroll() == doctest::Approx(0.0f));
		CHECK(f.panel->scroll() == doctest::Approx(0.0f));
		CHECK(f.panel->macroRowBounds().y()
		      == doctest::Approx(ui::ModPanel::kPadTop + ui::ModPanel::kHeadingHeight
		                         + ui::ModPanel::kCardGap));
		CHECK_FALSE(f.panel->mouseWheel(wheel));
	}
}

TEST_CASE("arming a card lights every destination and nothing else") {
	ModFixture f;
	std::vector<ui::Knob*> all;
	collectKnobs(f.root, &all);
	REQUIRE(all.size() >= 11);

	// THE KNOBS THAT ARE PARAMETERS, which is what this case has always been about and now has to say
	// out loud. It walks `kModDestinations` — a list of ParamIds — and reaches each one's widget
	// through the parameter registry, so a knob with no clap_id behind it can be neither found by that
	// walk nor counted against `kParamDestinationCount`.
	//
	// The filter card's six became exactly that in this commit: still destinations, still lighting,
	// but addressed by their index in the device's own declarations instead of by a host id. Counting
	// them here would compare a total that includes them against a list that cannot.
	//
	// ASKED OF THE CONTROL RATHER THAN OF THE WIDGET'S PARENT, because "is there a host parameter
	// behind this" is a question about what it addresses, not about where it is drawn.
	std::vector<ui::Knob*> knobs;
	for (ui::Knob* knob : all) {
		if (dynamic_cast<ui::ParamControl*>(knob->control()) != nullptr) {
			knobs.push_back(knob);
		}
	}
	REQUIRE(knobs.size() >= 11);
	// AND SOME WERE EXCLUDED, or the filter above is a line that does nothing and this case would go
	// on passing if a device's knobs quietly went back to being parameters.
	CHECK(knobs.size() < all.size());

	// Nothing is armed until a hand arms it, so nothing is mappable.
	for (ui::Knob* knob : knobs) {
		CHECK_FALSE(knob->mappable());
	}

	f.arm(macroSourceAt(0));

	size_t lit = 0;
	size_t dimLit = 0;
	for (ui::Knob* knob : knobs) {
		// THE HONESTY RULE APPLIED TO A VERB: a knob glows if and only if a drag on it would do
		// something. Anything else teaches the hand that the glow means nothing.
		// GREYED IS STILL A TARGET (backlog QK, by design greyed controls glow dimmer): a disabled or
		// inactive destination glows too, at the dim halo, so the hand can still find it; only brightness tells them apart.
		CHECK(knob->mappable() == knob->isModDestination());
		if (knob->mappable()) {
			const ui::Knob::Halo h = knob->haloForTest();
			CHECK(h.lit);
			CHECK(h.dim == !(knob->enabled() && knob->active()));
			dimLit += h.dim ? 1 : 0;
		}
		// THE COUNT BELOW IS OF ENABLED DESTINATIONS, as it always was: the disabled ones are the named list further down.
		lit += (knob->mappable() && knob->enabled()) ? 1 : 0;
	}
	// AND SOME GLOW DIM — Glide at least, disabled in Poly — or the dim branch above was never asked.
	CHECK(dimLit > 0);
	// ALL BUT ONE, and the missing one is the honesty rule doing its job rather than a gap: Glide
	// is disabled in Poly because glideSecondsNow() hands every voice zero there, so a route to it
	// would move nothing. SINCE 2026-11-08 (QK, by design) a switched-off knob IS routable and glows DIM: the route
	// takes effect the moment the knob is live again, and the dim glow says "a target, not live now". The FULL-brightness
	// count is still all but these, which is what `lit` counts.
	//
	// COUNTED RATHER THAN LISTED, which is what let this case follow the destination list when the fx
	// lane's filter widened it from six to eleven: it went red the moment the filter's parameters
	// became destinations and the card's knobs had not yet been handed the panel's state, because a
	// knob that IS a destination and does not light is precisely the equality above failing.
	size_t reachable = 0;
	for (const ParamId id : kModDestinations) {
		const std::span<visage::Frame* const> watchers = f.gui.widgets()->watchers(static_cast<clap_id>(id));
		for (visage::Frame* watcher : watchers) {
			if (auto* knob = dynamic_cast<ui::Knob*>(watcher); knob != nullptr && knob->enabled()) {
				++reachable;
				break;
			}
		}
	}
	CHECK(lit == reachable);
	// ALL BUT ONE AGAIN. It read `- 2` for exactly as long as Spread was a destination with no control
	// on the card: promoting it from a macro took its knob away, and the card layout gave it one back in
	// the macro row's first cell. The one that is still missing is Glide, which is the honesty rule
	// doing its job rather than a gap.
	// kParamDestinationCount since 2026-08-22: the rack's own controls became destinations too, and
	// they have no knob in this editor to light — they live on a device card, addressed by index
	// rather than by ParamId. This case is about the knobs that ARE parameters, which is what the
	// loop above walks.
	// AND THE SUBTRACTION THAT STOOD HERE IS GONE WITH THE IDS. For one commit the filter's controls
	// were declared as parameters AND addressed as a device's, so this count had to say "minus the
	// filter's" out loud. They are not declared any more: kParamDestinationCount no longer counts
	// them, the walk above no longer finds them, and the two sides agree again without help.
	// **NAMED, NOT SUBTRACTED** (2026-09-21). This read `kParamDestinationCount - 1`, the one being
	// Glide — and it stays named rather than subtracted. A count says "one more than expected" and
	// leaves the reader to work out WHICH; not noticing which is how `FilterPoly` shipped, and the
	// comment above records being glad to lose an earlier literal of exactly this kind.
	//
	// So the claim is stated as the LIST: these destinations have no control in this editor, and here
	// is why each one does not. Anything else appearing here is a destination whose knob failed to
	// light, which is the defect this case exists for.
	std::vector<std::string> unlit;
	for (const ParamId id : kModDestinations) {
		const std::span<visage::Frame* const> watchers = f.gui.widgets()->watchers(static_cast<clap_id>(id));
		bool reachableHere = false;
		for (visage::Frame* watcher : watchers) {
			if (auto* knob = dynamic_cast<ui::Knob*>(watcher); knob != nullptr && knob->enabled()) {
				reachableHere = true;
				break;
			}
		}
		if (!reachableHere) {
			const ParamDesc* desc = findParam(static_cast<clap_id>(id));
			REQUIRE(desc != nullptr);
			unlit.push_back(std::string(desc->name));
		}
	}
	// GLIDE: the honesty rule doing its job rather than a gap — see the paragraph above.
	// STACK DETUNE, the same rule for the same reason (backlog KE): the default patch stacks ONE oscillator, whose
	// fan position is 0, so the detune multiplies nothing and its knob is disabled until Stack is raised. STACK SPREAD
	// likewise (step 2): one part at fan 0 has pan 0 whatever Spread says.
	// AND THE STACK INDEX'S AMOUNTS, FREE PHASE AND CURVE (step 3): they reach the sound only through a route from the
	// Stack Index, and a stack of one has one part at index 0, so all four are disabled in the default patch. Three of
	// them are value boxes inside a zone knob rather than knobs, which this walk does not count either way.
	// AND RESONATE'S EIGHT (2026-09-26): its surround knobs are live only on Resonate, and the default patch is V5,
	// so on it they are disabled (the honesty rule; the Resonate surround case pins them live on Resonate).
	// AND ITS SPEED (2026-10-03), the same rule: a Resonate surround knob, disabled off Resonate.
	// AND BASIC SHAPES' FOUR (2026-10-03), the same rule: live only on Basic Shapes (test_gui_widgets_dahdsr's
	// "basic shapes: the right flank is the shape mix" pins them live there).
	// AND THE ADDITIVE SHAPE AMOUNT (2026-10-10), the same rule: the selected sound's knob is live only on Additive
	// (test_gui_widgets_dahdsr's "explore: on every engine a control only another engine could wake is not drawn" pins
	// it live there). Sound A's alone, as for every per-sound knob: A is the selected sound in the default patch.
	// AND THE SIX ADDITIVE BIAS KNOBS (2026-10-10), the same rule and the same case pins them live on Additive.
	// AND THE SPECTRAL DRAWER'S CONTINUOUS CONTROLS (2026-10-10; the later five operators' joined 2026-10-12, all listed by
	// kSpectralContinuousControls below rather than by name), the same rule and, being per sound like the bias
	// knobs, the same shape: the selected sound's (A's) set is enabled on Additive alone (design R9); the Additive half below
	// pins every one of them LIT there.
	std::vector<std::string> expectedUnlit { "Glide Time", "Stack Detune", "Stack Spread", "Stack Shape Amount",
		                                     "Stack Free Phase", "Stack Mapping Amount", "Stack Curve",
		                                     "Resonate Width", "Resonate Bow", "Resonate Shimmer", "Resonate Stiffness",
		                                     "Resonate Coupling", "Resonate Decay", "Resonate Pluck", "Resonate Travel",
		                                     "Resonate Speed",
		                                     "Shapes Sine", "Shapes Triangle", "Shapes Saw", "Shapes Square",
		                                     "A Shape Amount",
		                                     "A Rolloff", "A Formant", "A Odd/Even", "A Comb", "A Detail", "A Off-grid",
		                                     // PLUCK (2026-10-31): its six engine-wide knobs and Sound A's five scalers, live only on
		                                     // Pluck (the surround case pins them live there).
		                                     "Pluck Decay", "Pluck Decay Track", "Pluck Release", "Pluck Hardness", "Pluck Pickup",
		                                     "Pluck Width", "Pluck Velocity", "A Pluck Brightness", "A Pluck Sustain", "A Pluck Pick", "A Pluck Noise",
		                                     "A Pluck Stretch",
		                                     // A-PH (2026-10-18): Sub priority's two, in the spectrum panel's strip, not on the main
		                                     // face (the panel is a window the tab opens), and live there only under Sub priority.
		                                     "Additive Sub", "Additive Ceiling",
		                                     // OE (2026-10-26): the engine vocoder's analysis, in the spectrum panel's Vocode section,
		                                     // and live there only while some sound vocodes.
		                                     "Additive Vocode Bands", "Additive Vocode Attack", "Additive Vocode Release",
		                                     // OE2: Sibilance, the Vocode section's knob, greyed until some sound vocodes.
		                                     "Additive Vocode Sibilance" };
	// AND EVERY SOUND'S IDENTITY (backlog PQ): edit-only destinations, reached by a Relative macro through the tray's
	// Gamma box and never by a knob that lights for an ordinary source — A32's rule that nothing modulates identity.
	for (size_t slot = 0; slot < feathers::kMaxSounds; ++slot) {
		for (const feathers::SoundField field : feathers::kEditOnlyFields) {
			expectedUnlit.emplace_back(findParam(static_cast<clap_id>(feathers::soundParam(slot, field)))->name);
		}
	}
	// THE OTHER SEVEN SOUNDS' SPECTRAL CONTROLS ARE NOT ON ANY FACE until the spectrum panel is opened (it is a floating
	// window the tab opens, 2026-10-12, not a drawer whose hidden sets stood in the tree), and A's are unlit off Additive.
	for (size_t slot = 0; slot < feathers::kMaxSounds; ++slot) {
		for (const feathers::SpectralControl c : feathers::kSpectralContinuousControls) {
			expectedUnlit.emplace_back(findParam(static_cast<clap_id>(feathers::spectralParam(slot, c)))->name);
		}
	}
	// AND THE DRUM (2026-11-05): every pad's amounts that are not field scalers live in the kit panel, a floating window the
	// Drum tab opens, so they are on no face until it is; the Kit Walk and Sound A's seven scalers are on the Explore surround,
	// live on Drum alone (the surround case pins them there). B..H's scalers are hidden with their sound and not greyed by
	// the surround, which only greys the selected sound's, so they are not in this list.
	for (size_t slot = 0; slot < feathers::kMaxSounds; ++slot) {
		for (const feathers::DrumControl c : feathers::kDrumContinuousControls) {
			const bool scaler = c >= feathers::DrumControl::FieldPitch && c <= feathers::DrumControl::FieldMetal;
			if (!scaler || slot == 0) {
				expectedUnlit.emplace_back(findParam(static_cast<clap_id>(feathers::drumParam(slot, c)))->name);
			}
		}
	}
	expectedUnlit.emplace_back("Drum Walk X");
	expectedUnlit.emplace_back("Drum Walk Y");
	// AND THE KIT-WIDE LAYER (2026-11-06): on the Drum surround's Kit pages, live on a Drum Kit alone.
	for (const char* name : { "Drum Kit Gamma", "Drum Kit Zone", "Drum Kit Offset", "Drum Kit Pitch", "Drum Kit Length",
	                          "Drum Kit Sweep", "Drum Kit Tone", "Drum Kit Noise", "Drum Kit Snap", "Drum Kit Metal" }) {
		expectedUnlit.emplace_back(name);
	}
	std::sort(unlit.begin(), unlit.end());
	std::sort(expectedUnlit.begin(), expectedUnlit.end());
	std::string surprising;
	for (const std::string& name : unlit) {
		if (std::find(expectedUnlit.begin(), expectedUnlit.end(), name) == expectedUnlit.end()) {
			surprising += name + "; ";
		}
	}
	CAPTURE(surprising);
	CHECK(surprising.empty());
	// AND THE COUNT STILL TIES THE TWO SIDES TOGETHER, so a destination that quietly vanished from
	// the table cannot make the list above pass by being absent from both.
	CHECK(lit + unlit.size() == kParamDestinationCount);

	// AND ON ADDITIVE (2026-10-10): the engine those seven unlit A-knobs wait for. Switched by the published road, the
	// same equality holds, and Shape Amount and the six bias knobs are now LIT — so they are destinations in practice
	// (each has a ModState, ExploreStage::setModulation), not only in kModDestinations.
	ParamSet additive;
	additive.setBase(static_cast<clap_id>(ParamId::OscType), static_cast<double>(OscType::Additive));
	f.bridge.publish(additive);
	f.bridge.refresh();
	(void)f.gui.tickForTest();
	// The tick updates Explore only while it is the surface shown, and this fixture shows Perform; the stage's own frame
	// is what the tick would run, so run it.
	auto* explore = findInTree<ui::ExploreStage>(f.root);
	REQUIRE(explore != nullptr);
	(void)explore->update();
	size_t litAdditive = 0;
	for (ui::Knob* knob : knobs) {
		CHECK(knob->mappable() == knob->isModDestination()); // greyed glows dim (QK, decided 2026-11-08)
		litAdditive += (knob->mappable() && knob->enabled()) ? 1 : 0;
	}
	size_t reachableAdditive = 0;
	for (const ParamId id : kModDestinations) {
		for (visage::Frame* watcher : f.gui.widgets()->watchers(static_cast<clap_id>(id))) {
			if (auto* knob = dynamic_cast<ui::Knob*>(watcher); knob != nullptr && knob->enabled()) {
				++reachableAdditive;
				break;
			}
		}
	}
	CHECK(litAdditive == reachableAdditive);
	// AND EVERY ONE OF THE SPECTRUM PANEL'S, once the tab has opened it (a floating window, 2026-10-12): its knobs are
	// built armed into the same ModState, so a source armed before the panel opened lights them too.
	auto* spectrumTab = findInTree<ui::SpectrumTab>(f.root);
	REQUIRE(spectrumTab != nullptr);
	spectrumTab->clickLabelForTest();
	(void)f.gui.tickForTest();
	REQUIRE(f.gui.spectrumPanelForTest() != nullptr);
	for (const feathers::SpectralControl c : feathers::kSpectralContinuousControls) {
		const ParamId id = feathers::spectralParam(0, c);
		CAPTURE(std::string(findParam(static_cast<clap_id>(id))->name));
		bool litHere = false;
		for (visage::Frame* watcher : f.gui.widgets()->watchers(static_cast<clap_id>(id))) {
			if (auto* knob = dynamic_cast<ui::Knob*>(watcher); knob != nullptr) {
				litHere = litHere || knob->mappable();
			}
			// THE PARTIALS COUNT IS ITS MODE CELL'S BOX (backlog OH), mappable as a knob is.
			if (auto* box = dynamic_cast<ui::ValueBox*>(watcher); box != nullptr) {
				litHere = litHere || box->mappable(false);
			}
			// AND A PILL'S AMOUNT (the dense drawer, backlog OF): an amount routed on a pill feels like one routed on a knob.
			if (auto* pill = dynamic_cast<ui::FaderPill*>(watcher); pill != nullptr) {
				litHere = litHere || pill->mappableForTest();
			}
		}
		CHECK(litHere);
	}
	for (const ParamId id : { feathers::shapeAmountParam(0), feathers::additiveRolloffParam(0),
	                          feathers::additiveFormantParam(0), feathers::additiveOddEvenParam(0),
	                          feathers::additiveCombParam(0), feathers::additiveDetailParam(0),
	                          feathers::additiveOffGridParam(0) }) {
		const ParamDesc* desc = findParam(static_cast<clap_id>(id));
		REQUIRE(desc != nullptr);
		CAPTURE(std::string(desc->name));
		bool litHere = false;
		for (visage::Frame* watcher : f.gui.widgets()->watchers(static_cast<clap_id>(id))) {
			if (auto* knob = dynamic_cast<ui::Knob*>(watcher); knob != nullptr && knob->isVisible()) {
				litHere = litHere || knob->mappable();
			}
		}
		CHECK(litHere);
	}
}

TEST_CASE("holding Alt edits the parameter underneath without disarming") {
	// Bitwig's own behaviour, and the mock's: mapping is sticky, so without a bypass every value edit
	// during a mapping session would cost two extra clicks somewhere else.
	ModFixture f;
	f.arm(macroSourceAt(0));
	ui::Knob* path = f.knobFor(ParamId::Path);
	REQUIRE(path != nullptr);

	const double before = f.bridge.value(static_cast<clap_id>(ParamId::Path));
	const visage::Point centre = f.faceCentre(path);

	visage::MouseEvent down;
	down.window_position = centre;
	down.position = centre - path->positionInWindow();
	down.event_frame = path;
	down.button_id = visage::kMouseButtonLeft;
	down.button_state = visage::kMouseButtonLeft;
	down.is_down = true;
	down.repeat_click_count = 1;
	down.modifiers = visage::kModifierAlt;
	path->processMouseDown(down);

	visage::MouseEvent drag = down;
	drag.window_position = { centre.x, centre.y - 40.0f };
	drag.position = drag.window_position - path->positionInWindow();
	path->processMouseDrag(drag);

	visage::MouseEvent up = drag;
	up.is_down = false;
	up.button_state = visage::kMouseButtonNone;
	path->processMouseUp(up);

	CHECK(f.bridge.value(static_cast<clap_id>(ParamId::Path)) != before);
	CHECK(f.bridge.routeList().count() == 0);
	// Still armed afterwards — the bypass is for one gesture, not a mode change.
	CHECK(path->mappable());
}

TEST_CASE("the route list enumerates the focused macro, and only that one") {
	ModFixture f;
	auto* list = findInTree<ui::RouteList>(f.root);
	REQUIRE(list != nullptr);

	// Nothing routed: the heading stays and says so, rather than the section vanishing and leaving a
	// hand to wonder whether it clicked the wrong card.
	CHECK(list->rows().empty());
	CHECK(list->preferredHeight() > 0.0f);

	// Route Path from macro 0 and Level from macro 2, through the real knobs.
	f.arm(macroSourceAt(0));
	ui::Knob* path = f.knobFor(ParamId::Path);
	REQUIRE(path != nullptr);
	const visage::Point pathCentre = f.faceCentre(path);
	dragThroughRoot(f.root, pathCentre, { pathCentre.x, pathCentre.y - 35.0f });

	f.arm(macroSourceAt(2));
	ui::Knob* level = f.knobFor(ParamId::VoiceLevel);
	REQUIRE(level != nullptr);
	const visage::Point levelCentre = f.faceCentre(level);
	dragThroughRoot(f.root, levelCentre, { levelCentre.x, levelCentre.y - 20.0f });

	// THE CLAIM: the list belongs to the focused source. Macro 2 is focused, so Level is its one row and
	// macro 0's Path is not — a list that read the whole table would show two and be lying about which
	// macro does what, which is the only question this surface exists to answer.
	const std::vector<backplane::Destination> shown = list->rows();
	REQUIRE(shown.size() == 1);
	CHECK_FALSE(shown.front().isDevice());
	CHECK(kModDestinations[shown.front().index] == ParamId::VoiceLevel);

	SUBCASE("and switching focus switches what it enumerates") {
		f.arm(macroSourceAt(0));
		const std::vector<backplane::Destination> back = list->rows();
		REQUIRE(back.size() == 1);
		CHECK(kModDestinations[back.front().index] == ParamId::Path);
	}

	SUBCASE("the × takes a routing away, and undo brings it back") {
		const backplane::Destination destination = shown.front();
		const float before = f.bridge.routeAmount(macroSourceAt(2), destination);
		REQUIRE(before != 0.0f);

		const visage::Bounds row = list->rowBounds(0);
		const visage::Point cross = list->positionInWindow()
		                            + visage::Point { row.right() - ui::RouteList::kRemoveWidth * 0.5f,
			                                          row.y() + row.height() * 0.5f };
		// Through the real tree, at the place the list itself says the × is.
		REQUIRE(list->hitAt(cross - list->positionInWindow()).part == ui::RouteList::Part::Remove);
		dragThroughRoot(f.root, cross, cross);

		CHECK(f.bridge.routeAmount(macroSourceAt(2), destination) == 0.0f);
		CHECK(list->rows().empty());
		REQUIRE(f.gui.history()->canUndo());
		f.gui.history()->undo();
		CHECK(f.bridge.routeAmount(macroSourceAt(2), destination) == doctest::Approx(before));
		CHECK(list->rows().size() == 1);
	}

	SUBCASE("dragging a row's amount moves it, and leaves the destination's own value alone") {
		const backplane::Destination destination = shown.front();
		const double valueBefore = f.bridge.value(static_cast<clap_id>(ParamId::VoiceLevel));
		const visage::Bounds row = list->rowBounds(0);
		const visage::Point at = list->positionInWindow()
		                         + visage::Point { row.x() + row.width() * 0.5f,
			                                       row.y() + row.height() * 0.5f };
		const float before = f.bridge.routeAmount(macroSourceAt(2), destination);
		dragThroughRoot(f.root, at, { at.x, at.y - 40.0f });

		CHECK(f.bridge.routeAmount(macroSourceAt(2), destination)
		      == doctest::Approx(before + 40.0f / ui::RouteList::kDragRange).epsilon(0.02));
		CHECK(f.bridge.value(static_cast<clap_id>(ParamId::VoiceLevel)) == valueBefore);
	}
}

TEST_CASE("transfer chips: the chip opens a menu of every curve, applies nothing until a pick, and never covers the amount") {
	// The reverted attempt drew its glyph over the amount and
	// took its drag; this holds the geometry, the press, the menu and the pick apart, each by value.
	ModFixture f;
	auto* list = findInTree<ui::RouteList>(f.root);
	REQUIRE(list != nullptr);
	f.arm(macroSourceAt(2));
	ui::Knob* level = f.knobFor(ParamId::VoiceLevel);
	REQUIRE(level != nullptr);
	const visage::Point levelCentre = f.faceCentre(level);
	dragThroughRoot(f.root, levelCentre, { levelCentre.x, levelCentre.y - 20.0f });
	const std::vector<backplane::Destination> shown = list->rows();
	REQUIRE(shown.size() == 1);
	const backplane::Destination dest = shown.front();
	const backplane::Source source = macroSourceAt(2);
	const float amountBefore = f.bridge.routeAmount(source, dest);
	REQUIRE(amountBefore != 0.0f);
	REQUIRE(f.bridge.routeTransfer(source, dest) == backplane::Transfer::Lin);

	// THE GEOMETRY: the chip and the amount's text are disjoint, the amount ends where it always ended, and
	// every pixel of the amount's text is still the amount's — MEASURED, at the widest amounts there are.
	const visage::Bounds row = list->rowBounds(0);
	const visage::Bounds amount = list->amountBounds(0);
	const visage::Bounds chip = list->chipBounds(0);
	MESSAGE("route list width " << row.width() << ", chip x " << chip.x() << ".." << chip.right() << ", amount x "
	                            << amount.x() << ".." << amount.right());
	CHECK(chip.right() <= amount.x());
	CHECK(amount.right() == doctest::Approx(row.right() - ui::RouteList::kRemoveWidth - 4.0f));
	CHECK(chip.x() > 0.0f);
	for (const char32_t* widest : { U"+100%", U"-100%", U"+200%" }) {
		CHECK(ui::font(ui::kKnobReadoutSize).stringWidth(std::u32string(widest)) <= amount.width());
	}
	for (float x = amount.x(); x < amount.right(); x += 1.0f) {
		for (float y = amount.y(); y < amount.bottom(); y += 3.0f) {
			REQUIRE(list->hitAt({ x, y }).part == ui::RouteList::Part::Amount);
		}
	}
	const visage::Point chipCentre { chip.x() + chip.width() * 0.5f, chip.y() + chip.height() * 0.5f };
	REQUIRE(list->hitAt(chipCentre).part == ui::RouteList::Part::Transfer);

	// THE PRESS OPENS THE MENU AND MOVES NOTHING: the curve and the amount are what they were.
	dragThroughRoot(f.root, list->positionInWindow() + chipCentre, list->positionInWindow() + chipCentre);
	CHECK(ui::menuOpenFor(*list));
	CHECK(f.bridge.routeTransfer(source, dest) == backplane::Transfer::Lin);
	CHECK(f.bridge.routeAmount(source, dest) == amountBefore);
	ui::dismissMenuOf(*list);

	// THE MENU: every curve, named, the current one marked; a unipolar macro greys the three rectifiers,
	// which would change nothing it can say.
	const ui::RouteList::Row r { kNoInstance, dest };
	visage::PopupMenu menu = list->transferMenu(r);
	// THE FIXED CURVES AND THE φ ROW AFTER THEM (backlog PY).
	REQUIRE(menu.options().size() == device::kTransferMenuRows);
	CHECK(menu.options()[device::kTransferMenuPhiRow].name().toUtf8().find(device::kPhiTransferName)
	      != std::string::npos);
	CHECK(menu.options()[device::kTransferMenuPhiRow].enabled());
	REQUIRE_FALSE(feathers::sourceIsBipolarNow(f.bridge, source));
	for (size_t i = 0; i < device::kTransferCount; ++i) {
		const auto t = static_cast<device::Transfer>(i);
		CAPTURE(device::kTransferNames[i]);
		CHECK(menu.options()[i].name().toUtf8().find(device::kTransferNames[i]) != std::string::npos);
		CHECK(menu.options()[i].enabled() == !device::transferNeedsBipolar(t));
	}
	CHECK(menu.options()[0].name().toUtf8().rfind("X", 0) == 0);

	// A GREYED ROW IS NOT A PICK, whoever calls it.
	menu.onSelection().callback(static_cast<int>(device::Transfer::Abs));
	CHECK(f.bridge.routeTransfer(source, dest) == backplane::Transfer::Lin);

	// THE PICK IS THE EDIT: applied, one undo step, and the amount untouched.
	menu.onSelection().callback(static_cast<int>(device::Transfer::Exp));
	CHECK(f.bridge.routeTransfer(source, dest) == backplane::Transfer::Exp);
	CHECK(f.bridge.routeAmount(source, dest) == amountBefore);
	REQUIRE(f.gui.history()->canUndo());
	f.gui.history()->undo();
	CHECK(f.bridge.routeTransfer(source, dest) == backplane::Transfer::Lin);
	f.gui.history()->redo();
	CHECK(f.bridge.routeTransfer(source, dest) == backplane::Transfer::Exp);

	// AND THE × WITH ITS UNDO PUTS THE CURVE BACK WITH THE ROUTE.
	const visage::Point cross = list->positionInWindow()
	                            + visage::Point { row.right() - ui::RouteList::kRemoveWidth * 0.5f,
		                                          row.y() + row.height() * 0.5f };
	dragThroughRoot(f.root, cross, cross);
	REQUIRE(f.bridge.routeAmount(source, dest) == 0.0f);
	f.gui.history()->undo();
	CHECK(f.bridge.routeAmount(source, dest) == amountBefore);
	CHECK(f.bridge.routeTransfer(source, dest) == backplane::Transfer::Exp);
}

TEST_CASE("PY: a vertical drag on a φ chip sets its periods, a step a pixel, with a readout while it moves, one undo step; a click still opens the menu") {
	ModFixture f;
	auto* list = findInTree<ui::RouteList>(f.root);
	REQUIRE(list != nullptr);
	f.arm(macroSourceAt(2));
	ui::Knob* level = f.knobFor(ParamId::VoiceLevel);
	REQUIRE(level != nullptr);
	const visage::Point levelCentre = f.faceCentre(level);
	dragThroughRoot(f.root, levelCentre, { levelCentre.x, levelCentre.y - 20.0f });
	REQUIRE(list->rows().size() == 1);
	const backplane::Destination dest = list->rows().front();
	const backplane::Source source = macroSourceAt(2);
	const ui::RouteList::Row r { kNoInstance, dest };

	// THE φ ROW OF THE MENU IS THE PICK: the default periods, one undo step.
	visage::PopupMenu menu = list->transferMenu(r);
	menu.onSelection().callback(static_cast<int>(device::kTransferMenuPhiRow));
	const device::Transfer start = device::phiTransfer(device::kPhiOctavesDefault);
	REQUIRE(f.bridge.routeTransfer(source, dest) == start);
	const float amountBefore = f.bridge.routeAmount(source, dest);

	const visage::Bounds chip = list->chipBounds(0);
	const visage::Point centre = list->positionInWindow()
	                             + visage::Point { chip.x() + chip.width() * 0.5f, chip.y() + chip.height() * 0.5f };
	visage::Frame* down = f.root->frameAtPoint(centre);
	REQUIRE(down == list);
	const auto event = [&](visage::Point at, bool isDown) {
		visage::MouseEvent e;
		e.window_position = at;
		e.position = at - down->positionInWindow();
		e.event_frame = down;
		e.button_id = visage::kMouseButtonLeft;
		e.button_state = isDown ? visage::kMouseButtonLeft : visage::kMouseButtonNone;
		e.is_down = isDown;
		e.repeat_click_count = 1;
		return e;
	};

	// UP 32 PX IS +32 STEPS — ONE OCTAVE, 16 TO 32 PERIODS — and the readout says so WHILE it moves.
	down->processMouseDown(event(centre, true));
	CHECK_FALSE(list->chipDragReadout(0).has_value()); // a press alone is not a drag
	down->processMouseDrag(event({ centre.x, centre.y - 2.0f }, true));
	CHECK(f.bridge.routeTransfer(source, dest) == start); // under the threshold: nothing moved
	down->processMouseDrag(event({ centre.x, centre.y - 32.0f }, true));
	CHECK(device::phiStep(f.bridge.routeTransfer(source, dest)) == device::phiStep(start) + 32);
	CHECK(device::phiPeriods(f.bridge.routeTransfer(source, dest)) == doctest::Approx(32.0f));
	const std::optional<std::string> readout = list->chipDragReadout(0);
	REQUIRE(readout.has_value());
	CHECK(*readout == "Phi 32 per swing");
	down->processMouseUp(event({ centre.x, centre.y - 32.0f }, false));
	CHECK_FALSE(list->chipDragReadout(0).has_value());
	CHECK_FALSE(ui::menuOpenFor(*list)); // a drag is not a click
	CHECK(f.bridge.routeAmount(source, dest) == amountBefore); // the amount is not the chip's
	// THE WHOLE DRAG IS ONE STEP BACK TO WHERE IT STARTED.
	f.gui.history()->undo();
	CHECK(f.bridge.routeTransfer(source, dest) == start);
	f.gui.history()->redo();
	CHECK(device::phiPeriods(f.bridge.routeTransfer(source, dest)) == doctest::Approx(32.0f));

	// DOWN PAST THE BOTTOM CLAMPS AT ONE PERIOD.
	down->processMouseDown(event(centre, true));
	down->processMouseDrag(event({ centre.x, centre.y + 600.0f }, true));
	down->processMouseUp(event({ centre.x, centre.y + 600.0f }, false));
	CHECK(f.bridge.routeTransfer(source, dest) == device::phiTransferAtStep(0));

	// A CLICK THAT DOES NOT MOVE OPENS THE MENU, and moves nothing.
	dragThroughRoot(f.root, centre, centre);
	CHECK(ui::menuOpenFor(*list));
	CHECK(f.bridge.routeTransfer(source, dest) == device::phiTransferAtStep(0));
	ui::dismissMenuOf(*list);
	// AND LOG AND EXP: their EXPONENT, two pixels a step — up 32 px is +16 steps, one octave, 2 to 4 — and the
	// readout names it. Then a curve with NO coefficient ignores the drag: Linear's press opens the menu.
	for (const device::Transfer kind : { device::Transfer::Log, device::Transfer::Exp }) {
		CAPTURE(static_cast<int>(kind));
		visage::PopupMenu pick = list->transferMenu(r);
		pick.onSelection().callback(static_cast<int>(kind));
		REQUIRE(f.bridge.routeTransfer(source, dest) == kind);
		down->processMouseDown(event(centre, true));
		down->processMouseDrag(event({ centre.x, centre.y - 32.0f }, true));
		CHECK(device::transferExponent(f.bridge.routeTransfer(source, dest)) == doctest::Approx(4.0f));
		const std::optional<std::string> said = list->chipDragReadout(0);
		REQUIRE(said.has_value());
		CHECK(*said == std::string(device::transferName(kind)) + " exponent 4.00");
		down->processMouseUp(event({ centre.x, centre.y - 32.0f }, false));
		CHECK(device::transferKind(f.bridge.routeTransfer(source, dest)) == kind);
		CHECK(device::transferStep(f.bridge.routeTransfer(source, dest)) == 16);
		// DOWN PAST THE BOTTOM: exponent 1, a straight line.
		down->processMouseDown(event(centre, true));
		down->processMouseDrag(event({ centre.x, centre.y + 600.0f }, true));
		down->processMouseUp(event({ centre.x, centre.y + 600.0f }, false));
		CHECK(device::transferExponent(f.bridge.routeTransfer(source, dest)) == 1.0f);
	}
	visage::PopupMenu again = list->transferMenu(r);
	again.onSelection().callback(static_cast<int>(device::Transfer::Lin));
	REQUIRE(f.bridge.routeTransfer(source, dest) == device::Transfer::Lin);
	down->processMouseDown(event(centre, true));
	CHECK(ui::menuOpenFor(*list)); // the press is the menu, as it always was
	ui::dismissMenuOf(*list);
	down->processMouseDrag(event({ centre.x, centre.y - 40.0f }, true));
	down->processMouseUp(event({ centre.x, centre.y - 40.0f }, false));
	CHECK(f.bridge.routeTransfer(source, dest) == device::Transfer::Lin);
	CHECK_FALSE(list->chipDragReadout(0).has_value());
}

TEST_CASE("PY: the chip draws the curve the engine evaluates — every kind, at several coefficients, and it redraws as a drag moves the coefficient") {
	ModFixture f;
	auto* list = findInTree<ui::RouteList>(f.root);
	REQUIRE(list != nullptr);
	f.arm(macroSourceAt(2));
	ui::Knob* level = f.knobFor(ParamId::VoiceLevel);
	REQUIRE(level != nullptr);
	const visage::Point levelCentre = f.faceCentre(level);
	dragThroughRoot(f.root, levelCentre, { levelCentre.x, levelCentre.y - 20.0f });
	REQUIRE(list->rows().size() == 1);
	const backplane::Destination dest = list->rows().front();
	const backplane::Source source = macroSourceAt(2);
	REQUIRE_FALSE(feathers::sourceIsBipolarNow(f.bridge, source)); // so the chip plots 0..1
	const visage::Bounds chip = list->chipBounds(0);
	const float half = chip.height() * 0.5f - 1.0f;
	const float cy = chip.y() + chip.height() * 0.5f;

	// THE POINTS, RESTATED: x spans the chip, y is the centre less `shapeTransfer` (the engine's function) times
	// the half-height, at the source's reading for that x. And the drawn y for a bent curve differs from Lin's,
	// so the comparison is not vacuous.
	const auto holds = [&](device::Transfer t) {
		CAPTURE(std::string(device::transferName(t)));
		CAPTURE(device::transferStep(t));
		REQUIRE(f.bridge.setRouteTransfer(source, dest, t));
		const std::vector<visage::Point> points = list->chipPointsForTest(0);
		REQUIRE(points.size() >= 13u);
		CHECK(points.front().x == doctest::Approx(chip.x()));
		CHECK(points.back().x == doctest::Approx(chip.right()));
		for (const visage::Point& p : points) {
			const float reading = (p.x - chip.x()) / chip.width();
			CHECK(p.y == doctest::Approx(cy - device::shapeTransfer(t, reading) * half).epsilon(1e-4));
		}
		return points;
	};
	for (const device::Transfer t :
	     { device::Transfer::Lin, device::Transfer::Log, device::Transfer::Exp, device::Transfer::Abs,
	       device::transferWithStep(device::Transfer::Log, device::kExpStepMax),
	       device::transferWithStep(device::Transfer::Exp, 8), device::phiTransferAtStep(0),
	       device::phiTransfer(device::kPhiOctavesDefault), device::phiTransfer(device::kPhiOctavesMax) }) {
		(void)holds(t);
	}
	// A φ CURVE IS DRAWN TWO POINTS A PIXEL, so its busy shape is not aliased into a few strokes.
	REQUIRE(f.bridge.setRouteTransfer(source, dest, device::phiTransfer(device::kPhiOctavesMax)));
	CHECK(list->chipPointsForTest(0).size() == static_cast<size_t>(chip.width() * 2.0f) + 1u);

	// AND THE DRAG REDRAWS IT: mid-drag the points are the NEW curve's, at the new exponent.
	REQUIRE(f.bridge.setRouteTransfer(source, dest, device::Transfer::Exp));
	const std::vector<visage::Point> before = list->chipPointsForTest(0);
	const visage::Point centre = list->positionInWindow()
	                             + visage::Point { chip.x() + chip.width() * 0.5f, chip.y() + chip.height() * 0.5f };
	visage::Frame* down = f.root->frameAtPoint(centre);
	REQUIRE(down == list);
	const auto event = [&](visage::Point at, bool isDown) {
		visage::MouseEvent e;
		e.window_position = at;
		e.position = at - down->positionInWindow();
		e.event_frame = down;
		e.button_id = visage::kMouseButtonLeft;
		e.button_state = isDown ? visage::kMouseButtonLeft : visage::kMouseButtonNone;
		e.is_down = isDown;
		e.repeat_click_count = 1;
		return e;
	};
	down->processMouseDown(event(centre, true));
	down->processMouseDrag(event({ centre.x, centre.y - 48.0f }, true));
	const device::Transfer mid = f.bridge.routeTransfer(source, dest);
	REQUIRE(device::transferStep(mid) == 24);
	const std::vector<visage::Point> during = list->chipPointsForTest(0);
	REQUIRE(during.size() == before.size());
	for (size_t i = 0; i < during.size(); ++i) {
		const float reading = (during[i].x - chip.x()) / chip.width();
		CHECK(during[i].y == doctest::Approx(cy - device::shapeTransfer(mid, reading) * half).epsilon(1e-4));
	}
	CHECK(during[6].y != doctest::Approx(before[6].y)); // a steeper Exp sits lower at the middle: it moved
	down->processMouseUp(event({ centre.x, centre.y - 48.0f }, false));
}

TEST_CASE("the mark that moves and the mark that stays are never the same colour") {
	// R3, and the half of it that is easy to lose: a moving star in the modulated-glow colour IN
	// ADDITION TO the fixed one — so if both went periwinkle the two-state
	// display would stop distinguishing the two states, which is the whole of what it is for.
	//
	// WHAT THIS CASE IS AND IS NOT: it tests the DECISION, which is a pure function with one definition
	// per mark, and it is honest about that. Whether periwinkle reads well on the venue is for an eye at
	// the install, and no case here can stand in for it. What a case CAN do is make the pair
	// unforgeable: one definition each, and a known-bad that recolours the authored mark goes red here
	// rather than on the reporting screen.
	CHECK(ui::PerformStage::movingMarkColour(true) == ui::kModulation);
	CHECK(ui::PerformStage::movingMarkColour(false) == ui::kAccent);

	// Conditional, not permanent: periwinkle is modulation and nothing else in this editor, so a puck
	// nobody is modulating is drawn as the puck it is.
	CHECK(ui::PerformStage::movingMarkColour(false) != ui::kModulation);

	// And the authored mark is the accent in BOTH states — this is the assertion the known-bad fires on.
	CHECK(ui::PerformStage::authoredMarkColour() == ui::kAccent);
	CHECK(ui::PerformStage::authoredMarkColour() != ui::PerformStage::movingMarkColour(true));
	CHECK(ui::PerformStage::authoredMarkColour() == ui::PerformStage::movingMarkColour(false));

	SUBCASE("and the puck is a mark modulation can actually reach") {
		// The premise under all of it, checked against the destination table rather than remembered:
		// Path, Pos X and Pos Y are modulable, which is why the puck moves at all — and no placement or
		// identity parameter is, which is why no STAR on this circle can, and why the puck is the only
		// mark the rule can honestly be about.
		CHECK(modDestinationIndexOf(ParamId::Path) >= 0);
		CHECK(modDestinationIndexOf(ParamId::PosX) >= 0);
		CHECK(modDestinationIndexOf(ParamId::PosY) >= 0);
		for (size_t slot = 0; slot < kMaxSounds; ++slot) {
			CAPTURE(slot);
			CHECK(modDestinationIndexOf(soundParam(slot, SoundField::PlaceX)) < 0);
			CHECK(modDestinationIndexOf(soundParam(slot, SoundField::PlaceY)) < 0);
			// IDENTITY IS EDIT-ONLY SINCE BACKLOG PQ: a Relative macro may EDIT Zone, but the engine never compiles a route
			// there, so nothing MODULATES a star.
			const int zone = modDestinationIndexOf(soundParam(slot, SoundField::Zone));
			CHECK((zone < 0 || feathers::isEditOnlyDestination(static_cast<size_t>(zone))));
		}
	}
}

TEST_CASE("a region's marks cannot escape its own bounds") {
	// THE PROPERTY EVERY "WHICH WIDGET DREW THAT?" ARGUMENT RESTS ON, rendered rather than reasoned.
	// Reading vs_complex_shape.sc says the quad is clamped to the region and v_coordinates is
	// compensated so the shape's own SDF stays exact; that is an argument about the source. This
	// submits the geometry and counts pixels, because an artifact photographed ACROSS two panels can
	// only be blamed on a widget if a widget is able to paint outside itself — and it cannot.
	//
	// Kept after the hunt it was written for: if a visage merge ever loosens the clamp, the next
	// person chasing a stray mark would otherwise re-derive this from the shaders.
	visage::Canvas canvas;
	markedWindowless(canvas, 400, 300);
	// A GREY GROUND, NOT BLACK (backlog FB, 2026-09-16). This case went red under load with "red inside
	// = 0, red OUTSIDE = 0" — a frame with no red anywhere — and a black ground could not say which of
	// two failures that was: a read-back that returned NOTHING (black) or a frame the segments were
	// never drawn into (the ground). With the ground at 0x202020 the census below tells them apart,
	// and the red threshold (0x40) still clears it.
	constexpr uint32_t kGround = 0xff202020;
	canvas.setColor(kGround);
	canvas.fill(0.0f, 0.0f, static_cast<float>(canvas.width()), static_cast<float>(canvas.height()));

	visage::Region region;
	region.setBounds(200, 150, 150, 120);
	canvas.addRegion(&region);
	canvas.beginRegion(&region);
	canvas.setColor(0xffff0000);
	// Fully inside, so a screenshot with NO red anywhere fails as a broken harness rather than
	// passing as a working clamp.
	canvas.segment(10.0f, 10.0f, 100.0f, 100.0f, 3.0f, false);
	// And the escape attempt: from inside the region back to the WINDOW's origin, which in this
	// region's local coordinates is exactly minus its own position — the arithmetic the fork's
	// ORIGIN tripwire describes.
	canvas.segment(10.0f, 10.0f, -200.0f, -150.0f, 3.0f, false);
	canvas.endRegion();

	// MARKED (backlog IT): the census below names the failure it sees, but only if the frame is this
	// one; a read-back of someone else's frame could hand it red in the wrong place.
	const visage::Screenshot shot = takeMarkedCapture(canvas, 400, 300);
	REQUIRE(shot.width() == 400);
	int redInside = 0;
	int redOutside = 0;
	// THE CENSUS THAT NAMES THE FAILURE: how many pixels are the ground, how many are black (a read-back
	// that returned nothing), how many are something else.
	int ground = 0;
	int black = 0;
	int other = 0;
	// AND THE ALPHA, which is what separates a read-back that returned NOTHING (all four channels 0)
	// from a frame that was cleared and never drawn into (black, alpha 255).
	int alphaZero = 0;
	int alphaFull = 0;
	for (int y = 0; y < shot.height(); ++y) {
		for (int x = 0; x < shot.width(); ++x) {
			const visage::Color c = shot.sample(x, y);
			const int r = c.hexRed();
			const int g = c.hexGreen();
			const int b = c.hexBlue();
			const unsigned a = c.hexAlpha();
			if (a == 0) {
				++alphaZero;
			}
			else if (a == 0xff) {
				++alphaFull;
			}
			if (r == 0x20 && g == 0x20 && b == 0x20) {
				++ground;
			}
			else if (r == 0 && g == 0 && b == 0) {
				++black;
			}
			else {
				++other;
			}
			if (r < 0x40) {
				continue;
			}
			const bool inside = x >= 200 && x < 350 && y >= 150 && y < 270;
			if (inside) {
				++redInside;
			}
			else {
				++redOutside;
			}
		}
	}
	// Reported either way: "nothing outside" is only worth reading next to a count that says the
	// mark was drawn at all.
	MESSAGE("red inside region = " << redInside << ", red OUTSIDE region = " << redOutside
	        << "; census: ground = " << ground << ", black = " << black << ", other = " << other
	        << ", alpha 0 = " << alphaZero << ", alpha 255 = " << alphaFull);
	// THE PHOTOGRAPH, WHEN ASKED FOR: with FEATHERS_FRAME_DUMP_DIR set, a frame that failed the claim is
	// written there as a PPM named by the process, so the picture that read wrong can be looked at
	// rather than reasoned about.
	if (redInside == 0) {
		if (const char* dir = std::getenv("FEATHERS_FRAME_DUMP_DIR"); dir != nullptr) {
			const std::string path = std::string(dir) + "/fb-marks-" + std::to_string(getpid()) + ".ppm";
			std::ofstream out(path, std::ios::binary);
			out << "P6\n" << shot.width() << " " << shot.height() << "\n255\n";
			for (int y = 0; y < shot.height(); ++y) {
				for (int x = 0; x < shot.width(); ++x) {
					const visage::Color c = shot.sample(x, y);
					out.put(static_cast<char>(c.hexRed()));
					out.put(static_cast<char>(c.hexGreen()));
					out.put(static_cast<char>(c.hexBlue()));
				}
			}
			MESSAGE("frame written to " << path);
		}
	}
	CHECK(redInside > 0);
	CHECK(redOutside == 0);
}

TEST_CASE("a press on a stored stance recalls it, through the tree a window delivers to") {
	// THE DEFECT: a variant could not be selected — found after the drag
	// gesture landed on these cards, so the question this asks is the one the direct-callback cases
	// cannot: does a press at the coordinate a stance row OCCUPIES still reach that row, through the
	// tree gui.cpp assembles and the routing a window performs? A handler can be perfect and
	// unreachable, which is exactly what "I can no longer edit params" turned out to be.
	QuietGuiHost host;
	ParamBridge bridge;
	/// THE SAME BRIDGE AS THE DEVICE SEAM. Cards and panels are converging onto `backplane::DeviceHost`
	/// so the rack can build them; `BridgeDeviceHost` is what keeps Feathers handing them the parameter
	/// road underneath, and a fixture needs one for the same reason `Gui::Impl` does.
	feathers::BridgeDeviceHost deviceHost { bridge };
	bridge.refresh();
	Gui gui(host, bridge);
	REQUIRE(Gui::isAvailable());
	REQUIRE(gui.create(CLAP_WINDOW_API_X11, false));
	REQUIRE(gui.setSize(Gui::kLogicalWidth, Gui::kLogicalHeight));
	visage::Frame* root = gui.root();
	REQUIRE(root != nullptr);
	ui::SoundStack* stack = findInTree<ui::SoundStack>(root);
	REQUIRE(stack != nullptr);

	// A sound that remembers two explorations, and editing a third. (Ported twice on this side:
	// V19 made the light activeStance; V20 made the record the EXPLORE BOX, so the stored numbers
	// are zone moves and the recall claims below assert zone — the click-path claims themselves
	// are unchanged.)
	SoundVariations variations = bridge.soundVariations();
	REQUIRE(storeVariation(variations, 0, { 0.0, 300.0 }));
	REQUIRE(storeVariation(variations, 0, { 0.0, 700.0 }));
	bridge.beginSoundVariationsGesture();
	bridge.setSoundVariations(0, variations.sound[0]);
	bridge.endSoundVariationsGesture();
	// Storing into an empty list materialises the implicit record too, so two stores make three
	// rows — asked of the widget rather than counted here. Row 1 is the first stance stored above,
	// and it is not the active record.
	REQUIRE(stack->variationRows(0) == 3);
	REQUIRE(stack->variationValuesAt(0, 1).zone == doctest::Approx(300.0));
	REQUIRE_FALSE(stack->activeStance(0, 1));

	// The row's own coordinate, asked of the widget and converted the way a window would.
	const visage::Bounds row = stack->variationBoundsForTest(0, 1);
	const visage::Point inRow { row.x() + 30.0f, row.y() + row.height() * 0.5f };
	REQUIRE(stack->hitAt(inRow).part == SoundStack::Part::Variation);
	REQUIRE(stack->hitAt(inRow).index == 1);

	const visage::Point at = stack->positionInWindow();
	const visage::Point inWindow { at.x + inRow.x, at.y + inRow.y };
	visage::Frame* down = root->frameAtPoint(inWindow);
	REQUIRE(down != nullptr);
	// THE FIRST CLAIM: the press lands on the rail rather than on something drawn over it.
	CHECK(isWithin(down, stack));

	const auto eventAt = [&](bool isDown) {
		visage::MouseEvent e;
		e.window_position = inWindow;
		e.position = inWindow - down->positionInWindow();
		e.event_frame = down;
		e.button_id = visage::kMouseButtonLeft;
		e.button_state = isDown ? visage::kMouseButtonLeft : visage::kMouseButtonNone;
		e.is_down = isDown;
		e.repeat_click_count = 1;
		return e;
	};
	down->processMouseDown(eventAt(true));
	// A REAL HAND MOVES A LITTLE WHILE IT CLICKS, and this rail now watches drags — so the gesture is
	// delivered the way a window delivers one, jitter included, rather than as a press with no travel.
	visage::MouseEvent jitter = eventAt(true);
	jitter.window_position = { inWindow.x + 2.0f, inWindow.y + 1.0f };
	jitter.position = jitter.window_position - down->positionInWindow();
	down->processMouseDrag(jitter);
	down->processMouseUp(eventAt(false));

	// AND THE SECOND CLAIM: the stance is recalled — the exploration lands and the light moves onto
	// it. The place params stay exactly where they were: place left the stance (V20).
	CHECK(bridge.value(static_cast<clap_id>(soundParam(0, SoundField::Zone)))
	      == doctest::Approx(300.0));
	CHECK(stack->activeStance(0, 1));
	CHECK_FALSE(stack->activeStance(0, 2));
}


TEST_CASE("a stance row is outside its card's rect, so a press on one is never a press on the other") {
	// THE MECHANISM PROPOSED FOR THE "I cant select a variant?": that the drag now arms on a card
	// press and the rows live INSIDE the card rect, so a row press arms a drag instead of recalling.
	// Tested rather than argued, because the whole question is geometric and geometry can be asked.
	//
	// A card is `kCardHeight` tall and its rows begin a `kRowGap` BELOW its bottom edge — the rows are
	// siblings of the card, not children of it. Pinned here so that a future card that grew to cover
	// its own rows would fail this rather than silently swallow every press on them.
	StackFixture f;
	f.consume(0, 300.0);
	f.consume(1, 500.0);
	SoundVariations variations = f.bridge.soundVariations();
	REQUIRE(storeVariation(variations, 0, { 0.0, 300.0 }));
	REQUIRE(storeVariation(variations, 0, { 0.0, 700.0 }));
	f.bridge.beginSoundVariationsGesture();
	f.bridge.setSoundVariations(0, variations.sound[0]);
	f.bridge.endSoundVariationsGesture();
	f.settle();
	REQUIRE(f.stack.variationRows(0) == 3);

	const visage::Bounds card = f.stack.cardBoundsForTest(0);
	for (size_t index = 0; index < f.stack.variationRows(0); ++index) {
		CAPTURE(index);
		const visage::Bounds row = f.stack.variationBoundsForTest(0, index);
		// Disjoint, and the row is BELOW: no pixel of a row is a pixel of the card.
		CHECK(row.y() >= card.bottom());
		// And every pixel across the row's width answers the row rather than the card — the two verbs
		// on it included, since those are the row's own.
		for (float x = row.x(); x < row.right(); x += 1.0f) {
			const SoundStack::Hit hit = f.stack.hitAt({ x, row.y() + row.height() * 0.5f });
			if (hit.part == SoundStack::Part::Card) {
				FAIL("a press inside a stance row answered as the card at x=" << x);
			}
		}
	}
}

TEST_CASE("a sloppy press on a stance row still recalls, and never becomes a drag") {
	// THE OTHER HALF OF THE SAME WORRY. A press that travels is what turns a card press into a
	// reorder, so the question is whether a row press that travels turns into one too. It cannot: the
	// gesture arms only from the card body, and a row is not the card body. Asked with travel well
	// past the threshold, which is more than any click has.
	StackFixture f;
	f.consume(0, 300.0);
	f.consume(1, 500.0);
	SoundVariations variations = f.bridge.soundVariations();
	REQUIRE(storeVariation(variations, 0, { 0.0, 250.0 }));
	f.bridge.beginSoundVariationsGesture();
	f.bridge.setSoundVariations(0, variations.sound[0]);
	f.bridge.endSoundVariationsGesture();
	f.settle();
	f.set(ParamId::AZone, 900.0);
	REQUIRE_FALSE(f.stack.activeStance(0, 0));
	const double stanceZone = f.stack.variationValuesAt(0, 0).zone;

	const visage::Point at = f.variationPoint(0, 0);
	f.press(at);
	f.dragTo({ at.x + 3.0f, at.y + 4.0f * SoundStack::kDragThreshold });
	f.release({ at.x + 3.0f, at.y + 4.0f * SoundStack::kDragThreshold });
	f.settle();

	CHECK_FALSE(f.stack.dragSlot().has_value());
	CHECK(f.zoneOf(0) == doctest::Approx(stanceZone));
	CHECK(f.stack.activeStance(0, 0));
	// And the list did not reorder itself under the sloppiness.
	CHECK(f.zoneOf(1) == doctest::Approx(500.0));
}

TEST_CASE("a stance row still answers its own press once the rail has been scrolled") {
	// The offset code the drag already had to respect, asked of the ROW hit test as well: a rail that
	// has scrolled is where a press and the thing under it most easily come apart.
	StackFixture f;
	for (size_t slot = 0; slot < kMaxSounds; ++slot) {
		f.consume(slot, 100.0 + 50.0 * static_cast<double>(slot));
	}
	SoundVariations variations = f.bridge.soundVariations();
	REQUIRE(storeVariation(variations, 2, { 0.0, 440.0 }));
	f.bridge.beginSoundVariationsGesture();
	f.bridge.setSoundVariations(2, variations.sound[2]);
	f.bridge.endSoundVariationsGesture();
	f.settle();

	f.stack.setBounds(0.0f, 0.0f, SoundStack::kWidth, 200.0f);
	f.stack.setScroll(SoundStack::kSlotHeight + SoundStack::kRowGap);
	REQUIRE(f.stack.scroll() > 0.0f);
	f.set(static_cast<ParamId>(soundParam(2, SoundField::Zone)), 900.0);
	REQUIRE_FALSE(f.stack.activeStance(2, 0));
	const double stanceZone = f.stack.variationValuesAt(2, 0).zone;

	const visage::Point at = f.variationPoint(2, 0);
	REQUIRE(f.stack.hitAt(at).part == SoundStack::Part::Variation);
	REQUIRE(f.stack.hitAt(at).slot == 2);
	f.press(at);
	f.release(at);
	f.settle();
	CHECK(f.read(static_cast<ParamId>(soundParam(2, SoundField::Zone)))
	      == doctest::Approx(stanceZone));
	CHECK(f.stack.activeStance(2, 0));
}



//----------------------------------------------------------------------------------------------//
// THE FX LANE'S FIRST TENANT                                                                     //
//----------------------------------------------------------------------------------------------//


TEST_CASE("the filter card docks in the lane at the lane's own width") {
	FilterFixture f;

	// WIDTH RE-DERIVED FROM THIS LANE, not copied from the modulator strip's 216 — which is the same
	// arithmetic on a different rail and would be right by coincidence until one of them changed.
	CHECK(f.card->width() == doctest::Approx(ui::FxLane::kWidth - 2.0f * ui::FxLane::kPadX));
	CHECK(f.card->x() == doctest::Approx(ui::FxLane::kPadX));
	// Inside the lane, not over its hairline or under its heading.
	CHECK(f.card->y() > ui::FxLane::kHeadingHeight);
	CHECK(f.card->right() <= f.lane->width() - ui::FxLane::kPadX + 0.5f);
	CHECK(f.card->bottom() <= f.lane->height() + 0.5f);

	// AND IT GOES AWAY WITH THE LANE, which is Rail's doing rather than the lane's: a card left
	// visible in a 34px strip would draw across the hairline and answer clicks meant for the stage.
	REQUIRE(f.lane->setCollapsed(true));
	CHECK_FALSE(f.card->isVisible());
	REQUIRE(f.lane->setCollapsed(false));
	CHECK(f.card->isVisible());
}

TEST_CASE("the card folds to its head, and a folded card answers nothing below it") {
	FilterFixture f;
	const float open = f.card->height();
	CHECK(open == doctest::Approx(ui::FilterCard::openHeight()));

	// The head's own verb. Pressed at its left, away from the enable switch at the other end.
	f.foldPress({20.0f, ui::FilterCard::kHeadHeight * 0.5f});
	CHECK(f.card->folded());
	f.lane->resized();
	CHECK(f.card->height() == doctest::Approx(ui::FilterCard::foldedHeight()));
	CHECK(f.card->height() < open);

	// Everything it holds is hidden, for the reason a closed rail hides its children.
	CHECK_FALSE(f.card->partAt({f.card->width() * 0.5f, open - 20.0f}) != ui::FilterCard::Part::None);

	// AND THE TYPE IS STILL REACHABLE THROUGH A DROPDOWN THAT IS NOT THERE — it must not be. A click
	// where the menu used to be lands on nothing rather than on a control that is no longer drawn.
	const int before = static_cast<int>(f.param(fx::filter::Param::Type));
	f.press(f.centreOf(f.card->typeMenuBounds()));
	CHECK(static_cast<int>(f.param(fx::filter::Param::Type)) == before);

	f.foldPress({20.0f, ui::FilterCard::kHeadHeight * 0.5f});
	CHECK_FALSE(f.card->folded());
}

TEST_CASE("the enable switch is inside the head and is still reachable") {
	// A HIT-TEST ORDER BUG WITH A PLAUSIBLE SYMPTOM: the switch sits inside the head's rectangle, and
	// the head's own verb is fold. Tested in the wrong order the card would fold when you tried to
	// switch the filter on, and the report would read "the toggle does nothing".
	FilterFixture f;
	CHECK(f.card->partAt(f.centreOf(f.card->enableBounds())) == ui::FilterCard::Part::Enable);
	CHECK(f.card->partAt({20.0f, ui::FilterCard::kHeadHeight * 0.5f}) == ui::FilterCard::Part::Fold);

	CHECK(f.param(fx::filter::Param::Enabled) == doctest::Approx(0.0)); // off by default, always
	f.press(f.centreOf(f.card->enableBounds()));
	CHECK(f.param(fx::filter::Param::Enabled) == doctest::Approx(1.0));
	CHECK_FALSE(f.card->folded()); // and it did NOT fold on the way
	f.press(f.centreOf(f.card->enableBounds()));
	CHECK(f.param(fx::filter::Param::Enabled) == doctest::Approx(0.0));
}

TEST_CASE("the arrows walk the dropdown and skip what cannot be chosen") {
	FilterFixture f;
	const auto entries = ui::FilterCard::typeEntries();

	// The menu is every real type and then the families that are coming, marked and unselectable —
	// the header bar's "· soon" idiom, which is what gives the arrows something honest to skip.
	REQUIRE(entries.size() > static_cast<size_t>(fx::filter::Type::kCount));
	size_t live = 0;
	for (const auto& e : entries) {
		if (e.enabled) {
			live++;
			CHECK(e.type >= 0);
		}
		else {
			CHECK(e.type == -1); // a disabled row must not carry a type somebody could select
		}
	}
	CHECK(live == static_cast<size_t>(fx::filter::Type::kCount));

	// FROM THE LAST REAL TYPE, forward, there is nowhere to go — the arrow does not stop ON a "soon"
	// row and it does not wrap. That is the case that fails if skipping were written as "step one".
	const int last = static_cast<int>(fx::filter::Type::kCount) - 1;
	CHECK(f.card->typeAfter(last, 1) == -1);
	CHECK(f.card->typeAfter(last, -1) == last - 1);
	CHECK(f.card->typeAfter(0, -1) == -1);
	CHECK(f.card->typeAfter(0, 1) == 1);

	// And the arrows move the parameter, which is what makes them controls rather than decoration.
	CHECK(f.param(fx::filter::Param::Type) == doctest::Approx(0.0));
	f.press(f.centreOf(f.card->arrowBounds(1)));
	CHECK(f.param(fx::filter::Param::Type) == doctest::Approx(1.0));
	f.press(f.centreOf(f.card->arrowBounds(-1)));
	CHECK(f.param(fx::filter::Param::Type) == doctest::Approx(0.0));
	// At the end of the row it does nothing rather than something wrong.
	f.press(f.centreOf(f.card->arrowBounds(-1)));
	CHECK(f.param(fx::filter::Param::Type) == doctest::Approx(0.0));
}

TEST_CASE("the macro labels follow the type and the ids never move") {
	// THE DESIGN — one or two macros which affect the filter and follow the chosen type.
	// What follows is the MEANING and the caption; what must not follow is the id, because a host's
	// automation lane addresses a knob by id and a lane that re-pointed itself when the dropdown
	// moved would be a patch that plays differently every time it is opened.
	FilterFixture f;

	std::set<std::string> firstLabels;
	for (int type = 0; type < static_cast<int>(fx::filter::Type::kCount); type++) {
		CAPTURE(std::string(fx::filter::kTypeNames[type]));
		// setValue and NOT refresh: the bridge publishes the move immediately so the editor never lags
		// its own hand, while refresh() pulls the ENGINE's snapshot back over it — and this fixture has
		// no engine, so a refresh here would quietly restore the default and the case would be reading
		// a card that never moved. (It was written with the refresh first, and read one label for every
		// type.)
		f.set(fx::filter::Param::Type, static_cast<double>(type));

		const char* one = f.card->macroLabel(0);
		const char* two = f.card->macroLabel(1);
		REQUIRE(one != nullptr);
		REQUIRE(two != nullptr);
		CHECK(std::string(one) != std::string(two));
		firstLabels.insert(one);

		// A GAIN TYPE PUTS ITS GAIN ON MACRO 1, which is the mapping's own rule and the reason a
		// shelf's most useful control is the first knob a hand reaches.
		const bool gainType = type == static_cast<int>(fx::filter::Type::LowShelf)
		                      || type == static_cast<int>(fx::filter::Type::HighShelf)
		                      || type == static_cast<int>(fx::filter::Type::Bell);
		CHECK((std::string(one) == "Gain") == gainType);
	}
	// The captions really do differ across the dropdown — a mapping that returned one word for
	// everything would pass every check above and mean nothing.
	CHECK(firstLabels.size() > 1);
}

TEST_CASE("the curve is the filter, and no setting can send a point where it cannot be drawn") {
	// WAVE DISPLAY'S SECOND HARD RULE, applied at the source: these coordinates are COMPUTED from an
	// evaluated response rather than placed from a clamped parameter, which is exactly the family the
	// finite check exists for. Asked with the settings a hand can actually reach, at both ends.
	FilterFixture f;
	// setValue alone, for the reason the label case states: refresh() would pull the engine's snapshot
	// back over the move, and this fixture has no engine behind it.
	// THROUGH THE FIXTURE, which puts the value where the card reads it. This used to write the host
	// parameter directly, which the card no longer looks at.
	const auto sweep = [&](fx::filter::Param p, double value) { f.set(p, value); };

	for (int type = 0; type < static_cast<int>(fx::filter::Type::kCount); type++) {
		for (double res : {0.0, 0.5, 1.0}) {
			for (double macro : {-1.0, 0.0, 1.0}) {
				for (double cutoff : {fx::filter::kCutoffMinLog2, fx::filter::kCutoffMaxLog2}) {
					sweep(fx::filter::Param::Type, static_cast<double>(type));
					sweep(fx::filter::Param::Resonance, res);
					sweep(fx::filter::Param::Macro1, macro);
					sweep(fx::filter::Param::Macro2, -macro);
					sweep(fx::filter::Param::Cutoff, cutoff);
					CAPTURE(type);
					CAPTURE(res);
					CAPTURE(macro);
					CAPTURE(cutoff);
					std::array<float, ui::FilterCard::kMaxCurvePoints> gains{};
					const size_t points = f.card->curveGains(gains.data(), gains.size());
					REQUIRE(points >= 2);
					for (size_t i = 0; i < points; i++) {
						REQUIRE(std::isfinite(gains[i]));
						REQUIRE(gains[i] >= 0.0f);
					}
				}
			}
		}
	}

	// AND IT FOLLOWS THE KNOBS. A curve that did not would be a picture of a filter that used to be
	// playing — measured at a fixed high frequency, which a lowpass moving down has to quieten.
	sweep(fx::filter::Param::Type, static_cast<double>(fx::filter::Type::Lowpass));
	sweep(fx::filter::Param::Resonance, 0.3);
	sweep(fx::filter::Param::Macro1, 0.0);
	sweep(fx::filter::Param::Macro2, 0.0);
	sweep(fx::filter::Param::Cutoff, std::log2(12000.0));
	std::array<float, ui::FilterCard::kMaxCurvePoints> wide{};
	const size_t points = f.card->curveGains(wide.data(), wide.size());
	sweep(fx::filter::Param::Cutoff, std::log2(200.0));
	std::array<float, ui::FilterCard::kMaxCurvePoints> narrow{};
	REQUIRE(f.card->curveGains(narrow.data(), narrow.size()) == points);
	const size_t high = points * 3 / 4; // well up the log-spaced sweep
	CAPTURE(wide[high]);
	CAPTURE(narrow[high]);
	CHECK(narrow[high] < wide[high] * 0.1f);
}

TEST_CASE("the drawn curve follows spread, which stopped being a macro") {
	// THE SIBLING OF THE SLOPE CASE, and it exists because the thing it guards actually happened.
	// Spread was a MACRO until it was promoted to a param of its own; the card kept reading macro 1
	// and macro 2 and was never taught to read the new id, so the curve drew spread at zero forever
	// while the engine read the real value. it was seen it in one sentence — "spread is no longer
	// changing the presented curve" — and no case here could, because every existing curve case
	// leaves spread neutral, where a control that is not read and a control at zero are the same
	// picture.
	//
	// THE GENERAL SHAPE, worth more than this instance: A CONTROL THAT MOVES HOUSE STOPS BEING READ
	// BY EVERYTHING THAT READ IT BY ITS OLD NAME, and no test that only ever exercises it at its
	// neutral value will notice. The card assembles from published values, so anything the engine's
	// own assembly reads and this one does not is a curve of a filter nobody is playing.
	FilterFixture f;
	// THROUGH THE FIXTURE, which puts the value where the card reads it. This used to write the host
	// parameter directly, which the card no longer looks at.
	const auto sweep = [&](fx::filter::Param p, double value) { f.set(p, value); };
	sweep(fx::filter::Param::Type, static_cast<double>(fx::filter::Type::Lowpass));
	sweep(fx::filter::Param::Resonance, 0.0);
	sweep(fx::filter::Param::Macro1, 0.0);
	sweep(fx::filter::Param::Macro2, 0.0);
	sweep(fx::filter::Param::Slope, static_cast<double>(fx::filter::Slope::db12));
	sweep(fx::filter::Param::Cutoff, std::log2(500.0));

	// Measured an octave above the corner, where spread's second stage has moved the knee and the
	// difference is largest — not in the far field, where a blend below full weight converges back
	// toward one stage's asymptote and both settings would agree.
	const auto gainAtOneKhz = [&]() {
		std::array<float, ui::FilterCard::kMaxCurvePoints> gains{};
		const size_t points = f.card->curveGains(gains.data(), gains.size());
		REQUIRE(points >= 2);
		const double where = std::log2(1000.0 / fx::filter::kProfileLoHz)
		                     / std::log2(fx::filter::kProfileHiHz / fx::filter::kProfileLoHz);
		return static_cast<double>(gains[static_cast<size_t>(where * static_cast<double>(points - 1))]);
	};

	sweep(fx::filter::Param::Spread, 0.0);
	const double neutral = gainAtOneKhz();
	sweep(fx::filter::Param::Spread, 1.0);
	const double spreadUp = gainAtOneKhz();
	sweep(fx::filter::Param::Spread, -1.0);
	const double spreadDown = gainAtOneKhz();

	CAPTURE(neutral);
	CAPTURE(spreadUp);
	CAPTURE(spreadDown);
	// RELATIVE, because the profile's units are the profile's business. The first draft of this case
	// asserted an absolute difference of 0.5, having assumed decibels; these are linear gains, so the
	// threshold was larger than the whole range and the case failed against a WORKING fix. The claim
	// worth making is unit-independent anyway: the drawn curve MOVES, and by a share of itself rather
	// than by a number that has to be re-derived if the profile ever changes what it returns.
	REQUIRE(neutral > 0.0);
	CHECK(std::abs(spreadUp - neutral) / neutral > 0.1);
	CHECK(std::abs(spreadDown - neutral) / neutral > 0.1);
	// And the two directions are not the same picture: a second corner two octaves BELOW is already
	// well past by 1 kHz, where one two octaves above has barely begun.
	CHECK(spreadDown < spreadUp);

}

TEST_CASE("the drawn curve follows the slope and not only the knobs") {
	// THE CARD ASSEMBLES ITS CURVE FROM THE PUBLISHED VALUES, and a control it forgets to read is a
	// picture of a filter nobody is playing. Slope is the one most able to be forgotten: it is
	// reachable from a host's parameter list today whether or not this card has grown a control for
	// it, so a curve that ignored it would disagree with the sound for a reason the user could see
	// no cause for.
	//
	// Measured two octaves above the corner, where a pole count is a straight line and each pole is
	// another 6 dB down — the same place the module's own slope case measures.
	FilterFixture f;
	// THROUGH THE FIXTURE, which puts the value where the card reads it. This used to write the host
	// parameter directly, which the card no longer looks at.
	const auto sweep = [&](fx::filter::Param p, double value) { f.set(p, value); };
	sweep(fx::filter::Param::Type, static_cast<double>(fx::filter::Type::Lowpass));
	sweep(fx::filter::Param::Resonance, 0.0);
	sweep(fx::filter::Param::Macro1, 0.0);
	sweep(fx::filter::Param::Macro2, 0.0);
	sweep(fx::filter::Param::Cutoff, std::log2(500.0));

	std::array<float, ui::FilterCard::kMaxCurvePoints> gains{};
	std::array<double, 4> atTwoKilohertz{};
	size_t points = 0;
	for (int i = 0; i < static_cast<int>(fx::filter::Slope::kCount); i++) {
		sweep(fx::filter::Param::Slope, static_cast<double>(i));
		const size_t drawn = f.card->curveGains(gains.data(), gains.size());
		REQUIRE(drawn >= 2);
		if (points == 0) {
			points = drawn;
		}
		REQUIRE(drawn == points);
		// The profile is log-spaced from 20 Hz to 20 kHz, so 2 kHz sits at a fixed fraction of it.
		const double where = std::log2(2000.0 / fx::filter::kProfileLoHz)
		                     / std::log2(fx::filter::kProfileHiHz / fx::filter::kProfileLoHz);
		const auto at = static_cast<size_t>(where * static_cast<double>(points - 1));
		atTwoKilohertz[static_cast<size_t>(i)] = gains[at];
		CAPTURE(i);
		CAPTURE(gains[at]);
	}

	// Every step down the list is a quieter point up the skirt, and by a real margin rather than a
	// rounding — a pole is a factor of four in amplitude two octaves out.
	for (size_t i = 1; i < atTwoKilohertz.size(); i++) {
		CAPTURE(i);
		CAPTURE(atTwoKilohertz[i - 1]);
		CAPTURE(atTwoKilohertz[i]);
		CHECK(atTwoKilohertz[i] < atTwoKilohertz[i - 1] * 0.6);
	}
}

TEST_CASE("the slope box is on the type row and a hand can reach it") {
	// A HANDLER CAN BE PERFECT AND UNREACHABLE — this file's own lesson, and the reason the click here
	// goes THROUGH THE TREE at the coordinate the layout actually put the box, rather than calling the
	// card's handler with a rectangle the test restated.
	FilterFixture f;
	visage::Canvas canvas;
	f.card->draw(canvas);

	const visage::Bounds box = f.card->slopeBoxBounds();
	// On the type row, right of the forward arrow, inside the card.
	CHECK(box.y() >= ui::FilterCard::kHeadHeight);
	CHECK(box.bottom() <= f.card->curveBounds().y());
	CHECK(box.x() >= f.card->arrowBounds(1).right());
	CHECK(box.right() <= f.card->width());

	// A CLICK ON IT IS THE SLOPE MENU'S, not the fold's and not the type's.
	CHECK(f.card->partAt(f.centreOf(box)) == ui::FilterCard::Part::SlopeMenu);
	CHECK(f.card->partAt(f.centreOf(f.card->typeMenuBounds())) == ui::FilterCard::Part::TypeMenu);

	SUBCASE("and a type that declares one slope does not answer at all") {
		// GREYED RATHER THAN GONE, and unclickable: the box still says 12 dB, because that is the true
		// thing about a bell, and it does not move the row by disappearing.
		f.set(fx::filter::Param::Type, static_cast<double>(fx::filter::Type::Bell));
		f.card->draw(canvas);
		CHECK_FALSE(f.card->slopeIsLive());
		CHECK(f.card->partAt(f.centreOf(box)) == ui::FilterCard::Part::None);
		// The box is still exactly where it was, which is the anti-reflow claim.
		CHECK(f.card->slopeBoxBounds().x() == doctest::Approx(box.x()));
		CHECK(f.card->slopeBoxBounds().width() == doctest::Approx(box.width()));
	}

	SUBCASE("and the row does not reflow when the type changes") {
		// A row that rearranged itself per type would move the dropdown under a pointer that was
		// about to click it. Every rectangle on this row is a function of the card's width alone.
		const visage::Bounds menu = f.card->typeMenuBounds();
		const visage::Bounds back = f.card->arrowBounds(-1);
		const visage::Bounds forward = f.card->arrowBounds(1);
		for (int type = 0; type < static_cast<int>(fx::filter::Type::kCount); ++type) {
			CAPTURE(type);
			f.set(fx::filter::Param::Type, static_cast<double>(type));
			f.card->draw(canvas);
			CHECK(f.card->typeMenuBounds().x() == doctest::Approx(menu.x()));
			CHECK(f.card->typeMenuBounds().width() == doctest::Approx(menu.width()));
			CHECK(f.card->arrowBounds(-1).x() == doctest::Approx(back.x()));
			CHECK(f.card->arrowBounds(1).x() == doctest::Approx(forward.x()));
			CHECK(f.card->slopeBoxBounds().x() == doctest::Approx(box.x()));
		}
	}
}

TEST_CASE("the head and the type row have room for the words they draw") {
	// WHAT A PHOTOGRAPH WOULD HAVE SHOWN, asked of the font instead — and asked with the SAME
	// measurement the drawing uses, so this cannot pass while the glass disagrees. Moving keytrack
	// into the head and a slope box onto the type row put four pieces of text into corners that had
	// never held any, and text that does not fit is the defect a geometry case with only rectangles
	// in it cannot see.
	FilterFixture f;
	const visage::Font& control = ui::font(ui::kControlSize);
	const visage::Font& readout = ui::font(ui::kKnobReadoutSize);

	// THE CHIPS' OWN WORDS, centred in the chip's width as `SwitchChip::draw` prints them.
	for (const ui::SwitchChip* chip : {f.card->polyForTest(), f.card->keytrackForTest()}) {
		const float wide = ui::trackedWidth(ui::font(ui::kKnobLabelSize), chip->label().c_str(), 0.0f);
		CAPTURE(chip->label());
		CAPTURE(wide);
		CHECK(wide < static_cast<float>(chip->width()) - 4.0f); // a margin, not a squeeze against the wall
	}

	// EVERY SLOPE NAME, because the box is one width and the longest entry is what has to fit in it.
	for (int i = 0; i < static_cast<int>(fx::filter::Slope::kCount); ++i) {
		CAPTURE(i);
		const float wide = ui::trackedWidth(readout, fx::filter::kSlopeNames[i], 0.0f);
		CAPTURE(wide);
		CHECK(wide < f.card->slopeBoxBounds().width() - 4.0f);
	}

	// AND THE CARD'S NAME DOES NOT REACH THE ENABLE SQUARE. The head is back to a title and one
	// control, which is what it was before the switch spent a night up there.
	const float title = ui::FilterCard::kPad
	                    + ui::trackedWidth(control, fx::filter::kTraits.name, ui::kControlTracking);
	CAPTURE(title);
	CHECK(title < f.card->enableBounds().x());
}

TEST_CASE("the greyed slope box says what will run, not what is stored") {
	// FOUND BY PHOTOGRAPHING THE CARD, and by nothing else: every geometry case and every text-fit
	// case passed a bell whose greyed box read "24 dB" while the engine ran one section. A disabled
	// control may say "you cannot change this"; it may not say a number the DSP is not using.
	FilterFixture f;
	visage::Canvas canvas;

	f.set(fx::filter::Param::Slope, static_cast<double>(fx::filter::Slope::db24));
	f.set(fx::filter::Param::Type, static_cast<double>(fx::filter::Type::Lowpass));
	f.card->draw(canvas);
	// A lowpass wears all four, so stored and effective agree and the box says 24.
	CHECK(fx::filter::effectiveSlope(fx::filter::Type::Lowpass, fx::filter::Slope::db24)
	      == fx::filter::Slope::db24);

	// A BELL RUNS ONE SECTION whatever is stored, so the box has to read 12.
	CHECK(fx::filter::effectiveSlope(fx::filter::Type::Bell, fx::filter::Slope::db24)
	      == fx::filter::Slope::db12);
	CHECK(fx::filter::effectiveSlope(fx::filter::Type::Bell, fx::filter::Slope::db6)
	      == fx::filter::Slope::db12);
	// And a bandpass, which has a skirt but no one-pole: 18 rounds up to the 24 it can wear.
	CHECK(fx::filter::effectiveSlope(fx::filter::Type::Bandpass, fx::filter::Slope::db18)
	      == fx::filter::Slope::db24);
	CHECK(fx::filter::effectiveSlope(fx::filter::Type::Bandpass, fx::filter::Slope::db6)
	      == fx::filter::Slope::db12);

	// THE ONE THAT MATTERS: what the box reads is what the DSP will do, for every pairing. The
	// stage plan is the DSP's own answer, so this compares the card's reading against it rather
	// than against a second copy of the rule.
	for (int type = 0; type < static_cast<int>(fx::filter::Type::kCount); ++type) {
		for (int slope = 0; slope < static_cast<int>(fx::filter::Slope::kCount); ++slope) {
			CAPTURE(type);
			CAPTURE(slope);
			const auto t = static_cast<fx::filter::Type>(type);
			const auto asked = static_cast<fx::filter::Slope>(slope);
			const auto shown = fx::filter::effectiveSlope(t, asked);
			CHECK(fx::filter::slopeSupported(fx::filter::traitsOf(t), shown));
			CHECK(fx::filter::stagePlanOf(t, asked) == fx::filter::stagePlanOf(t, shown));
		}
	}
}

TEST_CASE("the camera writes the colours the editor drew") {
	// AN INSTRUMENT THAT LIES ABOUT COLOUR IS WORSE THAN NO INSTRUMENT, because it answers
	// confidently. This one is one commit old and its first output inverted red and blue — the frames
	// were faithful, the reading of them was not — so the byte order is pinned here rather than
	// described in a comment somebody has to find.
	//
	// ASSERTED AS AN ORDERING rather than as three exact bytes: the renderer is free to blend and
	// gamma-correct, and none of that can turn the accent's blue into its red. kAccent is 0xff7FA8E8,
	// where blue is the largest channel and red the smallest; a swapped writer reverses exactly that.
	FilterFixture f;
	auto* editor = dynamic_cast<visage::ApplicationEditor*>(f.root);
	REQUIRE(editor != nullptr);

	f.set(fx::filter::Param::Enabled, 1.0);
	f.settle();

	const int w = static_cast<int>(f.root->width());
	const int h = static_cast<int>(f.root->height());
	editor->setWindowless(w, h);
	// Asked AFTER setWindowless: supported() answers about an initialised renderer, and asking first
	// reads false with an empty message — which would make this case skip itself silently forever.
	if (!visage::Renderer::instance().supported()) {
		MESSAGE("SKIP no renderer: " << visage::Renderer::instance().errorMessage());
		return;
	}

	f.root->redrawAll();
	editor->drawWindow();
	editor->drawWindow();
	const visage::Screenshot& shot = editor->takeScreenshot();
	REQUIRE(shot.width() > 0);

	// The enable square, which is the one thing on this card that never dims — filled with kAccent
	// while the device is in the signal.
	const visage::Bounds box = f.card->enableBounds();
	const visage::Point at = f.card->positionInWindow()
	                         + visage::Point{box.x() + box.width() * 0.5f,
	                                         box.y() + box.height() * 0.5f};
	const int x = static_cast<int>(at.x);
	const int y = static_cast<int>(at.y);
	REQUIRE(x > 0);
	REQUIRE(y > 0);
	REQUIRE(x < shot.width());
	REQUIRE(y < shot.height());

	const uint8_t* pixel = shot.data() + (static_cast<size_t>(y) * shot.width() + x) * 4;
	const int red = pixel[0];
	const int green = pixel[1];
	const int blue = pixel[2];
	CAPTURE(red);
	CAPTURE(green);
	CAPTURE(blue);
	CHECK(blue > green);
	CHECK(green > red);
	// A wide margin, because the swap this exists to catch reverses the ends rather than nudging them.
	CHECK(blue - red > 40);
}

TEST_CASE("a route the rules refused on load is said in the tray") {
	// THE OTHER HALF OF THE SILENT REFUSAL: the loader now counts what it dropped, and this is the
	// road from that count to something a person can read. Drained on a FRAME rather than fired from
	// the loader, because a patch can be loaded with no editor open at all — a host restoring a
	// project loads state long before anybody opens the window — and a notification fired then is one
	// nobody could ever have seen.
	ModFixture f;
	ui::NotifyTray* tray = findInTree<ui::NotifyTray>(f.root);
	REQUIRE(tray != nullptr);
	REQUIRE(tray->text().empty());

	// THE LATCH IS WHAT THE EDITOR READS, so the case puts the report where a load would and then
	// asks the editor for a frame — which is exactly the order those two things happen in.
	// **THE ENDS ARE ENDPOINTS** (kill-lanes step 3b) — they were coordinates `0` and `0`, which named
	// macro 0 and destination 0 only because the two assertions below happened to decode the same zeros.
	// The latch is now set with THE VERY EXPRESSIONS those assertions name, so the case says "the end I put
	// in is the end the tray said" rather than resting on 0 == 0.
	feathers::RouteRefusals refused;
	refused.count = 1;
	refused.firstSource = macroSourceAt(0);
	refused.firstDestination = backplane::Destination::core(0);
	f.bridge.noteRefusedRoutes(refused);
	f.gui.tickForTest();

	CAPTURE(tray->text());
	CHECK_FALSE(tray->text().empty());
	// NAMED SPECIFICALLY ENOUGH TO ACT ON — which source and which destination, in the vocabulary the
	// route list already uses, so "my modulation stopped working" can become "the build told me why".
	CHECK(tray->text().find(ui::modSourceName(f.bridge, macroSourceAt(0))) != std::string::npos);
	const ParamDesc* desc = findParam(static_cast<clap_id>(feathers::kModDestinations[0]));
	REQUIRE(desc != nullptr);
	CHECK(tray->text().find(desc->name) != std::string::npos);

	SUBCASE("and it is said once, not once per frame") {
		// The take clears the latch. A message re-said every frame would pin the tray open forever
		// and would make the fade meaningless.
		const std::string first = tray->text();
		f.gui.tickForTest();
		CHECK(tray->text() == first); // unchanged because nothing new was said
		// Proven by saying something else and finding it stays said.
		tray->say("something else");
		f.gui.tickForTest();
		CHECK(tray->text() == "something else");
	}

	SUBCASE("and several refusals are one message with a count, not a queue") {
		// The tray is a single line and a later say() replaces an earlier one, so a message per route
		// would show only the last of them. One report, the first named in full and the rest counted.
		tray->say("");
		feathers::RouteRefusals several;
		several.count = 3;
		several.firstSource = macroSourceAt(0);
		several.firstDestination = backplane::Destination::core(0);
		f.bridge.noteRefusedRoutes(several);
		f.gui.tickForTest();
		CAPTURE(tray->text());
		CHECK(tray->text().find("3") != std::string::npos);
		CHECK(tray->text().find("+2") != std::string::npos);
	}

	SUBCASE("and an ordinary load says nothing at all") {
		// A message that appears on every load teaches nobody anything. A clean patch must leave the
		// tray exactly as it was.
		tray->say("");
		feathers::RouteRefusals none;
		f.bridge.noteRefusedRoutes(none);
		f.gui.tickForTest();
		CHECK(tray->text().empty());
	}

	SUBCASE("and a route naming a device this patch no longer has says SO, not whatever sits at index zero") {
		// **THE CLAIM THE COORDINATE SHAPE COULD NOT MAKE** (kill-lanes step 3b), and the reason it is worth
		// a case: a refusal used to arrive as a pair of indices, so an end naming a device that is gone had
		// already lost the identity by the time the tray saw it — the best it could do was print whatever the
		// index decoded to. The ends are endpoints now and are what the FILE asked for, so `gui.cpp` can ask
		// the bridge whether it still holds that device and say the honest thing.
		//
		// NOT A CHECK ON THE WORDING, which is `gui.cpp`'s to choose: the claim is that the tray does NOT
		// name macro 0 or destination 0 — which is precisely what an index-decoding reader would have
		// printed for the zeros in these two endpoints' index halves.
		tray->say("");
		constexpr DeviceInstanceId kAbsent = 9999;
		REQUIRE(f.bridge.descriptorOf(kAbsent) == nullptr); // the case is not vacuous: nothing wears this id
		feathers::RouteRefusals gone;
		gone.count = 1;
		gone.firstSource = backplane::Source::output(kAbsent, 0);
		gone.firstDestination = backplane::Destination::control(kAbsent, 0);
		f.bridge.noteRefusedRoutes(gone);
		f.gui.tickForTest();
		CAPTURE(tray->text());
		CHECK_FALSE(tray->text().empty()); // it still says something
		CHECK(tray->text().find(ui::modSourceName(f.bridge, macroSourceAt(0))) == std::string::npos);
		const ParamDesc* zeroth = findParam(static_cast<clap_id>(feathers::kModDestinations[0]));
		REQUIRE(zeroth != nullptr);
		CHECK(tray->text().find(zeroth->name) == std::string::npos);
	}
}

TEST_CASE("the card draws the filter the engine plays, field for field") {
	// THE SEAM THAT BROKE, AND THE GUARD THAT WOULD HAVE CAUGHT IT. The card assembles its own
	// Settings from the published values and the engine assembles one from the params; the card's
	// whole promise is that those two are the same filter. Every case before this one sampled the
	// promise a FIELD AT A TIME — the curve follows the cutoff, the curve follows the slope — so a
	// field nobody thought to sample was a field nobody checked, and spread was exactly that: it was
	// promoted from a macro to a param and the card was never taught to read it.
	//
	// MEASURED with the card frozen at spread 0 while the engine moved: at a 400 Hz corner, 1600 Hz,
	// the drawn point read 0.0618 at spread 0 AND at spread 0.9, where the engine read 0.0609 then
	// 0.0289 — 6.5 dB the curve never showed. The same at 24 dB: 0.00388 against 0.00377 then
	// 0.00179. it was seen it before any test did.
	//
	// SO THIS COMPARES THE WHOLE OBJECT rather than a field. A control added to the device from now
	// on is covered the day it exists, without anybody remembering to come here.
	FilterFixture f;
	const auto set = [&](fx::filter::Param p, double value) { f.set(p, value); };

	// Every continuous control off its default and every discrete one moved, so no field can agree by
	// sitting where it started.
	set(fx::filter::Param::Enabled, 1.0);
	set(fx::filter::Param::Type, static_cast<double>(fx::filter::Type::Highpass));
	set(fx::filter::Param::Cutoff, std::log2(640.0));
	set(fx::filter::Param::Resonance, 0.42);
	set(fx::filter::Param::Drive, 0.3);
	set(fx::filter::Param::Macro1, 0.6);
	set(fx::filter::Param::Macro2, -0.35);
	set(fx::filter::Param::Spread, 0.8);
	set(fx::filter::Param::Slope, static_cast<double>(fx::filter::Slope::db24));

	// AGAINST THE ROW, WHICH IS NOW THE ENGINE'S OWN SOURCE. This read `f.params.filterSettings()`
	// while the engine assembled the filter out of eleven published parameters; that function is gone
	// and so is the assembly, because `Synth::modulatedFilterState` starts from `fxFilterAt` — the
	// same decode this line performs.
	//
	// THE CLAIM SURVIVES THE CHANGE AND IS WORTH RESTATING, since the two sides now share a source and
	// a shared source can make a comparison look stronger than it is. What is still being asserted is
	// that the CARD does not transform what it draws: it decodes the same row the engine decodes and
	// changes exactly one field. Before the swallow the risk was a card whose reassembly forgot a
	// field — which shipped twice; now the risk is a card that reads the row and then adjusts
	// something on the way to the curve, and this catches that just as well.
	fx::filter::Settings engine = fxFilterAt(f.bridge.fxChain(), 0).settings;
	// The card draws the filter whether or not it is switched into the signal, which is the one field
	// the two are entitled to disagree about — see FilterCard::settings.
	engine.enabled = true;
	CHECK(f.card->settingsForTest() == engine);

	// AND THE ROW REALLY HOLDS WHAT THE CASE SET, or the comparison above is two reads of a pair of
	// defaults agreeing with each other. The values are the ones written at the top, restated rather
	// than read back through the same helper.
	CHECK(engine.cutoffLog2Hz == doctest::Approx(std::log2(640.0)).epsilon(1e-4));
	CHECK(engine.resonance == doctest::Approx(0.42f).epsilon(1e-4));
	CHECK(engine.type == fx::filter::Type::Highpass);

	SUBCASE("and at every type, since the macro table changes what a macro means") {
		for (int type = 0; type < static_cast<int>(fx::filter::Type::kCount); ++type) {
			CAPTURE(type);
			set(fx::filter::Param::Type, static_cast<double>(type));
			fx::filter::Settings at = fxFilterAt(f.bridge.fxChain(), 0).settings;
			at.enabled = true;
			CHECK(f.card->settingsForTest() == at);
			CHECK(at.type == static_cast<fx::filter::Type>(type));
		}
	}
}

TEST_CASE("the drawn curve moves when spread moves, and moves the way the filter does") {
	// THE DEFECT: spread stopped changing the presented curve. The case above pins the assembly; this
	// one pins the CONSEQUENCE, end to end, and against the filter rather than against arithmetic —
	// the drawn point at a frequency spread genuinely moves, compared with the same filter's MEASURED
	// impulse response, at more than one slope, because slope cascades stages and "spread's stage is
	// missing from the drawn cascade above 12 dB" would be a different bug wearing the same symptom.
	FilterFixture f;
	const auto set = [&](fx::filter::Param p, double value) { f.set(p, value); };
	set(fx::filter::Param::Enabled, 1.0);
	set(fx::filter::Param::Type, static_cast<double>(fx::filter::Type::Lowpass));
	set(fx::filter::Param::Cutoff, std::log2(400.0));
	set(fx::filter::Param::Resonance, 0.2);

	constexpr double kProbeHz = 1600.0;
	constexpr size_t kIrLength = 16384;
	std::array<float, ui::FilterCard::kMaxCurvePoints> gains{};

	/// The drawn value at kProbeHz, off the card's own profile.
	const auto drawnAtProbe = [&]() {
		const size_t points = f.card->curveGains(gains.data(), gains.size());
		REQUIRE(points > 2);
		const double where = std::log2(kProbeHz / fx::filter::kProfileLoHz)
		                     / std::log2(fx::filter::kProfileHiHz / fx::filter::kProfileLoHz);
		return static_cast<double>(gains[static_cast<size_t>(where * static_cast<double>(points - 1))]);
	};

	/// What the FILTER does at kProbeHz, measured off an impulse rather than evaluated.
	const auto measuredAtProbe = [&]() {
		fx::filter::MultimodeFilter filter;
		filter.prepare(48000.0);
		// FROM THE ROW, which is where the settings live and what the engine itself hands the filter.
		// This measures the DEVICE against the card's drawing, so it has to be configured from the
		// same place the device is.
		filter.setTarget(fxFilterAt(f.bridge.fxChain(), 0).settings);
		std::vector<float> ir(kIrLength, 0.0f);
		ir[0] = 1.0f;
		filter.processMono(ir.data(), static_cast<int>(ir.size()));
		double re = 0.0;
		double im = 0.0;
		for (size_t i = 0; i < ir.size(); ++i) {
			const double angle = 2.0 * std::numbers::pi * std::fmod(kProbeHz / 48000.0 * static_cast<double>(i), 1.0);
			re += static_cast<double>(ir[i]) * std::cos(angle);
			im -= static_cast<double>(ir[i]) * std::sin(angle);
		}
		return std::sqrt(re * re + im * im);
	};

	for (fx::filter::Slope slope : {fx::filter::Slope::db12, fx::filter::Slope::db24}) {
		CAPTURE(static_cast<int>(slope));
		set(fx::filter::Param::Slope, static_cast<double>(slope));

		set(fx::filter::Param::Spread, 0.0);
		const double drawnDry = drawnAtProbe();
		const double heardDry = measuredAtProbe();

		set(fx::filter::Param::Spread, 0.9);
		const double drawnWet = drawnAtProbe();
		const double heardWet = measuredAtProbe();

		CAPTURE(drawnDry);
		CAPTURE(drawnWet);
		CAPTURE(heardDry);
		CAPTURE(heardWet);

		// IT MOVED, and by a margin no rounding reaches: measured at a factor of about 2.1, which is
		// 6.5 dB.
		CHECK(heardWet < heardDry * 0.75);
		CHECK(drawnWet < drawnDry * 0.75);
		// AND IT MOVED BY THE SAME AMOUNT THE FILTER DID. Ratios rather than absolutes, because the
		// card's profile samples the response at its own grid rather than exactly at kProbeHz.
		const double drawnRatio = drawnWet / std::max(drawnDry, 1e-12);
		const double heardRatio = heardWet / std::max(heardDry, 1e-12);
		CAPTURE(drawnRatio);
		CAPTURE(heardRatio);
		CHECK(std::abs(drawnRatio - heardRatio) < 0.08);
	}
}

TEST_CASE("a macro slot a type does not spend gets no knob at all") {
	// THE HONESTY RULE, IN ITS STRICTEST FORM: not drawn-and-disabled, not drawn-without-a-label —
	// ABSENT. A knob under the hand that does nothing teaches the hand that knobs on this card might
	// do nothing, and that lesson is worse than a gap in a row.
	//
	// Five types have an empty macro 1 since Spread became a param of its own, and Bell has an empty
	// macro 2 because it cannot be a morph partner in either direction.
	FilterFixture f;
	visage::Canvas canvas;
	const auto selectType = [&](fx::filter::Type type) {
		f.set(fx::filter::Param::Type, static_cast<double>(type));
		// The card reads the type where it reads its macro labels, which is in the draw.
		f.card->draw(canvas);
	};

	// Row B is Spread, Macro 1, Macro 2 — so the macro slots are the last two, and Spread in front of
	// them is the knob that never goes away.
	REQUIRE(f.card->knobsForTest().size() >= 6);
	const ui::Knob* spread = f.card->knobsForTest()[3].get();
	const ui::Knob* first = f.card->knobsForTest()[4].get();
	const ui::Knob* second = f.card->knobsForTest()[5].get();

	for (fx::filter::Type type : {fx::filter::Type::Lowpass, fx::filter::Type::Highpass,
	                              fx::filter::Type::Bandpass, fx::filter::Type::Notch,
	                              fx::filter::Type::Peak}) {
		CAPTURE(static_cast<int>(type));
		selectType(type);
		CHECK_FALSE(first->isVisible()); // Spread's old home, now empty on purpose
		CHECK(second->isVisible());      // Morph, which these five still spend
		CHECK(spread->isVisible());      // and Spread itself, in a cell of its own, for every type
	}

	// The gain shapes keep macro 1 and gained a macro 2 they did not have — except Bell.
	selectType(fx::filter::Type::LowShelf);
	CHECK(first->isVisible());
	CHECK(second->isVisible());
	selectType(fx::filter::Type::Bell);
	CHECK(first->isVisible());
	CHECK_FALSE(second->isVisible());
}

TEST_CASE("the keytrack switch sits in a strip of its own along the bottom") {
	// ITS THIRD HOME (the KT button moved to the bottom), and each move fixed
	// what the last one taught. The macro row's third cell stopped being free when Spread became a
	// param and the row needed three knobs. The head was clear of every other rectangle but sat
	// INSIDE the head's own, whose verb is fold — a switch that folds the card if the hit test
	// reaches it first, which is a hazard this case had to manage. At the bottom it is in nobody's
	// rectangle, so the hazard is removed rather than guarded.
	FilterFixture f;
	const visage::Bounds sw = f.card->keytrackBounds();
	const auto overlaps = [](const visage::Bounds& a, const visage::Bounds& b) {
		return a.x() < b.right() && b.x() < a.right() && a.y() < b.bottom() && b.y() < a.bottom();
	};

	// BELOW EVERYTHING, and inside the card. Clear of the head entirely, which is the whole point.
	CHECK(sw.y() > ui::FilterCard::kHeadHeight);
	CHECK(sw.bottom() <= f.card->height());
	for (const auto& knob : f.card->knobsForTest()) {
		CAPTURE(knob->y());
		CHECK(sw.y() >= knob->y() + knob->height() - 1.0f);
	}

	// The chip is inside the card and off the poly chip above it.
	CHECK_FALSE(overlaps(sw, f.card->polyBounds()));
	CHECK(sw.right() <= f.card->width());
	CHECK_FALSE(overlaps(sw, f.card->enableBounds()));

	SUBCASE("and the card still fits the lane, which does not scroll") {
		// THE CARD GREW by the strip, and the lane places it at a fixed top and gives it its whole
		// open height — there is no scrolling to catch an overflow, so the fit is the thing that has
		// to hold. Asked of the real lane rather than of arithmetic.
		const visage::Bounds card{f.card->x(), f.card->y(), f.card->width(), f.card->height()};
		CAPTURE(card.bottom());
		CAPTURE(f.lane->height());
		CHECK(card.bottom() <= f.lane->height());
		// And the strip is genuinely inside it rather than hanging off the end.
		CHECK(ui::FilterCard::openHeight() >= f.card->keytrackRowTop() + ui::FilterCard::kSwitchHeight);
	}

	SUBCASE("and a click on it flips keytrack rather than folding the card") {
		// Kept from when the switch was in the head, where this was the hazard. It is cheap, and it
		// is the assertion that would notice if the switch ever moved back into somebody else's
		// rectangle.
		ui::SwitchChip* keytrack = f.card->keytrackForTest();
		REQUIRE(keytrack != nullptr);
		REQUIRE(f.param(fx::filter::Param::Keytrack) < 0.5);
		REQUIRE_FALSE(f.card->folded());

		const visage::Point onSegment =
		    keytrack->positionInWindow()
		    + visage::Point{keytrack->width() * 0.5f, keytrack->height() * 0.5f};
		REQUIRE(f.root->frameAtPoint(onSegment) == keytrack);
		f.clickAt(onSegment);
		f.settle();

		CHECK(f.param(fx::filter::Param::Keytrack) >= 0.5);
		CHECK_FALSE(f.card->folded());
	}

	SUBCASE("and it hides with the knobs when the card folds") {
		// THE OPPOSITE OF WHAT THIS CASE USED TO ASSERT, and the change is a decision rather than a
		// regression: the switch survived a fold while it lived in the head, because the head is
		// exactly what a folded card still is. Below the fold it is an ordinary control. The enable
		// square remains the one thing a fold leaves behind — it is the only one you need to get the
		// device back.
		f.card->setFolded(true);
		CHECK_FALSE(f.card->keytrackForTest()->isVisible());
		for (const auto& knob : f.card->knobsForTest()) {
			CHECK_FALSE(knob->isVisible());
		}
		CHECK(f.card->enableBounds().bottom() <= ui::FilterCard::foldedHeight());
	}
}

TEST_CASE("the drawn curve resolves a resonant peak instead of stepping over it") {
	// A review remark that the filter curve's waypoints might not be enough was about the
	// VERIFICATION set, and it was right — but it also led to the drawn resolution, and the card was
	// under-resolved in a way that mattered more.
	//
	// IT SHIPPED AT 96 POINTS. Ninety-six log-spaced points from 20 Hz to 20 kHz step 7.5% in
	// frequency each. A resonance at the SVF's own 0.98 clamp is a Q of 25, whose peak is 4% wide —
	// NARROWER THAN THE STEP — so the peak fell between two samples and the drawn curve missed it by
	// 1.97 dB. The knob said maximum resonance and the picture showed a gentle bump.
	//
	// The fix is not a bigger number picked by hand: it is WaveDisplay's own convention, two points
	// per logical pixel times the display scale, on a floor that exists because the thing needing the
	// points is the peak and the peak's width has nothing to do with how wide the card is.
	FilterFixture f;
	f.set(fx::filter::Param::Type, static_cast<double>(fx::filter::Type::Lowpass));
	f.set(fx::filter::Param::Cutoff, std::log2(1000.0));
	f.set(fx::filter::Param::Resonance, 1.0); // clamps to 0.98 in the module

	CHECK(f.card->curvePointCount() >= ui::FilterCard::kMinCurvePoints);
	CHECK(f.card->curvePointCount() <= ui::FilterCard::kMaxCurvePoints);
	// The house convention, not a second opinion about it.
	CHECK(ui::FilterCard::kPointsPerPixel == ui::WaveDisplay::kPointsPerPixel);

	std::array<float, ui::FilterCard::kMaxCurvePoints> drawn{};
	const size_t points = f.card->curveGains(drawn.data(), drawn.size());
	REQUIRE(points >= 2);
	float peak = 0.0f;
	for (size_t i = 0; i < points; i++) {
		peak = std::max(peak, drawn[i]);
	}

	// THE TRUE PEAK, from the module's own evaluator swept far finer than anything is drawn at. The
	// curve is measured against the FILTER rather than against a number somebody recorded, so this
	// case keeps meaning the same thing if the resonance mapping is ever retuned.
	fx::filter::Settings s;
	s.enabled = true;
	s.type = fx::filter::Type::Lowpass;
	s.cutoffLog2Hz = static_cast<float>(std::log2(1000.0));
	s.resonance = 1.0f;
	double truePeak = 0.0;
	for (int i = 0; i < 200000; i++) {
		const double hz = 20.0 * std::pow(1000.0, i / 199999.0);
		truePeak = std::max(truePeak, fx::filter::gainAt(s, 48000.0, hz));
	}

	const double missedDb = 20.0 * std::log10(truePeak / std::max<double>(peak, 1e-9));
	CAPTURE(points);
	CAPTURE(20.0 * std::log10(truePeak));
	CAPTURE(missedDb);
	// Measured at 0.08 dB here, against 1.97 dB at the 96 points this shipped with. A tenth of a
	// decibel is well inside the width of the stroke that draws it.
	CHECK(missedDb < 0.25);

	// KNOWN-BAD, run rather than asserted: the same curve at the old resolution DOES miss the peak,
	// so the case above is passing because of the fix and not because the peak was easy to hit.
	std::array<float, 96> coarseFreq{};
	std::array<float, 96> coarseGain{};
	fx::filter::gainProfile(s, 48000.0, coarseFreq.data(), coarseGain.data(), coarseGain.size());
	const float coarsePeak = *std::max_element(coarseGain.begin(), coarseGain.end());
	const double coarseMissed = 20.0 * std::log10(truePeak / std::max<double>(coarsePeak, 1e-9));
	CAPTURE(coarseMissed);
	CHECK(coarseMissed > 1.0);
}

TEST_CASE("an idle update of the lane copies the chain zero times, and the count it reads is the chain's own") {
	// BACKLOG T. FxLane::deviceCount() read `bridge_.fxChain().count` — a ~4 KB copy built from about
	// a thousand relaxed loads, to read one field of it — and updateCards() re-evaluated it on every
	// iteration while cardAt() asked again: three whole-chain copies per card per idle frame. The fix
	// reads a count. THIS CASE ASSERTS THE NUMBER OF COPIES rather than inferring it from a timing,
	// through the bridge's fxChainCopiesForTest() counter, which fxChain() itself increments.
	//
	// THE CARDS ARE HIDDEN FIRST, DELIBERATELY. updateCards() skips an invisible card before calling
	// its update(), and a card's update still reads its row through BridgeDeviceHost::fieldAt — which
	// copies the chain per FIELD (backlog BL, ~12 copies per card, hosting's to move onto the readers
	// pinned below). Hiding the cards leaves exactly the lane's own reads on the counter, which is
	// what this case is about; a total that included the seam's copies would be red for the seam's
	// reason and green again for the seam's fix, saying nothing about the lane either way.
	//
	// KNOWN-BAD (2026-09-03, both red, then green on restore): deviceCount() put back to
	// `bridge_.fxChain().count` fails `1 == 0` at both sizes (the hoisted read in updateCards makes it
	// one copy per update, not the N+1 of the original loop) and `3 == 0` for count + two cardAt; and
	// fxFieldAt pointed one field off fails the reader pins with the neighbouring values named.
	FilterFixture f;
	const auto hideCards = [&] {
		for (size_t slot = 0; slot < kMaxFxSlots; ++slot) {
			if (ui::FxCard* card = f.lane->cardAt(slot); card != nullptr) {
				card->setVisible(false);
			}
		}
	};
	const auto copiesDuringUpdate = [&] {
		const size_t before = f.bridge.fxChainCopiesForTest();
		f.lane->updateCards();
		return f.bridge.fxChainCopiesForTest() - before;
	};

	SUBCASE("one card") {
		REQUIRE(f.lane->deviceCount() == 1);
		hideCards();
		CHECK(copiesDuringUpdate() == 0);
		// And asking the count, or a card, copies nothing either.
		const size_t before = f.bridge.fxChainCopiesForTest();
		CHECK(f.lane->deviceCount() == 1);
		CHECK(f.lane->cardAt(0) != nullptr);
		CHECK(f.lane->cardAt(1) == nullptr);
		CHECK(f.bridge.fxChainCopiesForTest() - before == 0);
	}

	SUBCASE("a full lane of eight") {
		for (size_t i = 1; i < kMaxFxSlots; ++i) {
			REQUIRE(f.bridge.addFxDevice("flt1") < kMaxFxSlots);
		}
		f.bridge.refresh();
		f.lane->resized(); // the lane builds its cards from the chain on resize
		REQUIRE(f.lane->deviceCount() == kMaxFxSlots);
		hideCards();
		CHECK(copiesDuringUpdate() == 0);
	}

	SUBCASE("the count read without the copy is the count the copy carries, at every size") {
		// The accessor is the clamped load fxChain() starts with; this pins that they cannot drift.
		CHECK(f.bridge.fxDeviceCount() == f.bridge.fxChain().count);
		CHECK(f.lane->deviceCount() == f.bridge.fxChain().count);
		for (size_t i = 1; i < kMaxFxSlots; ++i) {
			REQUIRE(f.bridge.addFxDevice("flt1") < kMaxFxSlots);
			CHECK(f.bridge.fxDeviceCount() == f.bridge.fxChain().count);
			CHECK(f.bridge.fxDeviceCount() == i + 1);
		}
	}

	SUBCASE("the per-field readers answer what a copy's row answers, in range and out") {
		// BL's readers are consumed by nothing yet; this is the contract hosting's swap stands on. A
		// non-default value in every present field, so a reader pointed one slot or one field off
		// cannot agree by accident.
		fx::filter::State state = f.filter();
		fx::filter::setValueOf(&state, fx::filter::Param::Resonance, 0.73f);
		fx::filter::setValueOf(&state, fx::filter::Param::Drive, 0.41f);
		f.bridge.setFilterSettings(0, state);
		const FxChain chain = f.bridge.fxChain();
		const DeviceRow& row = fxDeviceAtPosition(chain, 0).row;
		REQUIRE(row.fieldsPresent > 0);
		CHECK(f.bridge.fxFieldsPresentAt(0) == row.fieldsPresent);
		for (size_t c = 0; c < row.fieldsPresent; ++c) {
			CAPTURE(c);
			CHECK(f.bridge.fxFieldAt(0, c) == row.field[c]);
		}
		// Past the present fields the copy carries what the atomics hold, and so does the reader.
		CHECK(f.bridge.fxFieldAt(0, kMaxDeviceFields - 1) == row.field[kMaxDeviceFields - 1]);
		// Out of range is zero on both roads: the copy has no row there, and the reader says so.
		CHECK(f.bridge.fxFieldAt(kMaxFxDevices, 0) == 0.0f);
		CHECK(f.bridge.fxFieldAt(0, kMaxDeviceFields) == 0.0f);
		CHECK(f.bridge.fxFieldsPresentAt(kMaxFxDevices) == 0);
		// An empty slot's present count is zero through both.
		CHECK(f.bridge.fxFieldsPresentAt(1) == fxDeviceAtPosition(chain, 1).row.fieldsPresent);
		CHECK(f.bridge.fxFieldsPresentAt(1) == 0);
	}
}

//----------------------------------------------------------------//
// THE SOURCE GRID — task 49's arrows, and the legality they honour //
//----------------------------------------------------------------//

TEST_CASE("the weight arrows are eight, in two rows of four, in slot order") {
	// THE DESIGNED SHAPE, asked of the real tree rather than of the class: a grid that laid itself out
	// correctly in isolation and landed somewhere else in the Engine rail would pass a geometry case and
	// fail the only thing the geometry is for.
	ModFixture f;
	auto* grid = findInTree<ui::SourceGrid>(f.root);
	REQUIRE(grid != nullptr);
	REQUIRE(grid->width() > 0.0f);

	for (size_t slot = 0; slot < kMaxSounds; ++slot) {
		CAPTURE(slot);
		CHECK(grid->cellBounds(slot).width() > 0.0f);
		CHECK(grid->cellBounds(slot).height() > 0.0f);
	}
	// Two rows of four: the first four share a top, the second four share a lower one, and reading
	// order runs left to right.
	for (size_t slot = 1; slot < ui::SourceGrid::kColumns; ++slot) {
		CAPTURE(slot);
		CHECK(grid->cellBounds(slot).y() == doctest::Approx(grid->cellBounds(0).y()));
		CHECK(grid->cellBounds(slot).x() > grid->cellBounds(slot - 1).x());
	}
	CHECK(grid->cellBounds(ui::SourceGrid::kColumns).y() > grid->cellBounds(0).y());
	CHECK(grid->cellBounds(ui::SourceGrid::kColumns).x()
	      == doctest::Approx(grid->cellBounds(0).x()));

	// NO CELL OVERLAPS ITS NEIGHBOUR, which is what makes a click land on the sound the hand aimed at.
	for (size_t slot = 0; slot + 1 < ui::SourceGrid::kColumns; ++slot) {
		CAPTURE(slot);
		CHECK(grid->cellBounds(slot).right() <= grid->cellBounds(slot + 1).x());
	}

	// And the whole grid fits the rail it was placed in.
	CHECK(grid->cellBounds(kMaxSounds - 1).right() <= grid->width() + 0.5f);
	CHECK(grid->cellBounds(kMaxSounds - 1).bottom() <= grid->height() + 0.5f);
}

TEST_CASE("the grid sits above the routes it makes") {
	// It sits there for a reason — the arrows are what makes a route and the list is what the route
	// became — so the order is part of the design rather than a layout detail.
	ModFixture f;
	auto* grid = findInTree<ui::SourceGrid>(f.root);
	auto* routes = findInTree<ui::RouteList>(f.root);
	REQUIRE(grid != nullptr);
	REQUIRE(routes != nullptr);
	CHECK(grid->positionInWindow().y + grid->height() <= routes->positionInWindow().y + 0.5f);
}

TEST_CASE("arming a weight arrow lights the destinations it may reach, and only those") {
	// THE UI-TRUTH CASE the legality seam was written for. maySourceRouteTo refuses a weight source the
	// three destinations the puck is made of, and `mappable` is both the glow and the gate — so an
	// armed weight arrow must leave Path, Pos X and Pos Y dark while lighting the rest.
	ModFixture f;
	auto* grid = findInTree<ui::SourceGrid>(f.root);
	REQUIRE(grid != nullptr);

	// A REAL CLICK ON A REAL ARROW, through the real tree — the same road a hand takes.
	const visage::Bounds cell = grid->cellBounds(0);
	const visage::Point at = grid->positionInWindow()
	                         + visage::Point { cell.x() + cell.width() * 0.5f,
		                                       cell.y() + cell.height() * 0.5f };
	dragThroughRoot(f.root, at, at);
	REQUIRE(grid->slotArmed(0));

	std::vector<ui::Knob*> knobs;
	collectKnobs(f.root, &knobs);
	size_t lit = 0;
	size_t offered = 0; // every enabled destination knob on the surface, device cards included
	for (ui::Knob* knob : knobs) {
		if (!knob->isModDestination() || !knob->enabled()) {
			continue;
		}
		++offered;
		lit += knob->mappable() ? 1 : 0;
	}

	ui::Knob* path = f.knobFor(ParamId::Path);
	ui::Knob* posX = f.knobFor(ParamId::PosX);
	ui::Knob* posY = f.knobFor(ParamId::PosY);
	REQUIRE(path != nullptr);
	REQUIRE(posX != nullptr);
	REQUIRE(posY != nullptr);
	CHECK_FALSE(path->mappable());
	CHECK_FALSE(posX->mappable());
	CHECK_FALSE(posY->mappable());

	// AND THE CONTROL, which is what stops this passing on an arm that never happened: a macro armed
	// the same way DOES light Path.
	f.arm(macroSourceAt(0));
	CHECK(path->mappable());
	// AND NOT EVERY DESTINATION LIT — the puck stayed dark — so the count above is not vacuous. This
	// compared against the old destination TABLE's size; the knobs on the surface are the population
	// now (a device card's controls are destinations too, and 82 knobs lit against 80 parameters).
	CHECK(lit < offered);
}

TEST_CASE("a switched-off sound's arrow greys out and refuses to be armed") {
	// Arrows grey out when their sound is disabled. A disabled sound takes no place in the blend,
	// so its weight is absent and there is nothing to route FROM — arming it would light every legal
	// destination for a source that reaches none of them.
	ModFixture f;
	auto* grid = findInTree<ui::SourceGrid>(f.root);
	REQUIRE(grid != nullptr);

	// Slot 2 is switched off in the shipped defaults, and slot 0 is on — the two states without
	// touching a parameter, so this case is about the grid rather than about a write reaching it.
	REQUIRE(grid->slotEnabled(0));
	REQUIRE_FALSE(grid->slotEnabled(2));

	const visage::Bounds cell = grid->cellBounds(2);
	const visage::Point at = grid->positionInWindow()
	                         + visage::Point { cell.x() + cell.width() * 0.5f,
		                                       cell.y() + cell.height() * 0.5f };
	dragThroughRoot(f.root, at, at);
	CHECK_FALSE(grid->slotArmed(2));

	// AND NOTHING ELSE WAS ARMED EITHER — a refusal that armed the previous source instead would be
	// worse than one that did nothing.
	std::vector<ui::Knob*> knobs;
	collectKnobs(f.root, &knobs);
	for (ui::Knob* knob : knobs) {
		CHECK_FALSE(knob->mappable());
	}

	// The control: switch that sound ON and the same click now arms it. The write goes the whole way
	// round — posted to the queue, drained into a ParamSet, published back — because there is no audio
	// thread here to do it, and a setValue on its own would leave the editor reading the snapshot it
	// already had. Same round trip the segmented-control case makes, and it is the difference between
	// testing this grid and testing a value that never arrived.
	f.bridge.setValue(static_cast<clap_id>(soundParam(2, SoundField::Enable)), 1.0);
	ParamSet params;
	f.bridge.drain([&](const UiParamEvent& event) {
		if (event.kind == UiParamEvent::Kind::Value) {
			params.setBase(event.id, event.value);
		}
	});
	f.bridge.publish(params);
	f.bridge.refresh();
	REQUIRE(grid->slotEnabled(2));
	dragThroughRoot(f.root, at, at);
	CHECK(grid->slotArmed(2));
}

TEST_CASE("a weight source can be routed, and the list says whose route it is") {
	// THE WHOLE ROAD, end to end and through real gestures: arm an arrow, drag a legal destination,
	// and find the route in the list under the name of the source that made it.
	ModFixture f;
	auto* grid = findInTree<ui::SourceGrid>(f.root);
	auto* routes = findInTree<ui::RouteList>(f.root);
	REQUIRE(grid != nullptr);
	REQUIRE(routes != nullptr);

	const visage::Bounds cell = grid->cellBounds(1);
	const visage::Point at = grid->positionInWindow()
	                         + visage::Point { cell.x() + cell.width() * 0.5f,
		                                       cell.y() + cell.height() * 0.5f };
	dragThroughRoot(f.root, at, at);
	REQUIRE(grid->slotArmed(1));

	// GLIDE IS A LEGAL DESTINATION and it lives in this very rail, so the drag is a short one — but it
	// is disabled in Poly, which is the shipped mode, so this uses Fine instead: a knob that is
	// switched off is not routable, which is the honesty rule one level up.
	ui::Knob* fine = f.knobFor(ParamId::FineTune);
	REQUIRE(fine != nullptr);
	REQUIRE(fine->mappable());
	const visage::Point centre = f.faceCentre(fine);
	dragThroughRoot(f.root, centre, { centre.x, centre.y - 35.0f });

	const int destination = modDestinationIndexOf(ParamId::FineTune);
	REQUIRE(destination >= 0);
	const backplane::Destination fineTune = backplane::Destination::core(static_cast<size_t>(destination));
	CHECK(f.bridge.routeAmount(weightSourceAt(1), fineTune) != 0.0f);

	// The list is about the focused source, so the route the arrow just made is the row it shows.
	const std::vector<backplane::Destination> rows = routes->rows();
	REQUIRE(rows.size() == 1);
	CHECK(rows[0] == fineTune);

	// AND IT HAS A LEGIBLE NAME — the vocabulary the strip, the grid and this list all share.
	CHECK(ui::modSourceName(f.bridge, weightSourceAt(1)) == "Sound B weight");
	CHECK(ui::modSourceName(f.bridge, macroSourceAt(0)) == "Macro 1");
}

TEST_CASE("a renamed macro names its source everywhere") {
	// ONE NAMING AUTHORITY, so a rename reaches every surface that says a macro's name in prose — the
	// route list's heading, the tray, the Engine rail all ask `modSourceName`. Asserted as the literal
	// the hand typed, not as "whatever modSourceName says", so a surface that still spelled the default
	// could not pass by agreeing with a namer that also still spelled it.
	ModFixture f;
	f.bridge.setMacroName(0, "Filter Sweep");
	CHECK(ui::modSourceName(f.bridge, macroSourceAt(0)) == "Filter Sweep");
	CHECK(ui::modSourceName(f.bridge, macroSourceAt(1)) == "Macro 2"); // only the one renamed

	// THE TRAY, reached the way a load reaches it — the refused-route report names its source.
	ui::NotifyTray* tray = findInTree<ui::NotifyTray>(f.root);
	REQUIRE(tray != nullptr);
	feathers::RouteRefusals refused;
	refused.count = 1;
	refused.firstSource = macroSourceAt(0);
	refused.firstDestination = backplane::Destination::core(0);
	f.bridge.noteRefusedRoutes(refused);
	f.gui.tickForTest();
	CAPTURE(tray->text());
	CHECK(tray->text().find("Filter Sweep") != std::string::npos);
	CHECK(tray->text().find("Macro 1") == std::string::npos);

	// CLEARED, the default comes back; the default spelled out is not a rename.
	f.bridge.setMacroName(0, "");
	CHECK(ui::modSourceName(f.bridge, macroSourceAt(0)) == "Macro 1");
	CHECK_FALSE(f.bridge.macroRenamed(0));
	f.bridge.setMacroName(0, "Macro 1");
	CHECK_FALSE(f.bridge.macroRenamed(0));
	CHECK(ui::modSourceName(f.bridge, macroSourceAt(0)) == "Macro 1");
}

TEST_CASE("a relabelled MACROS output names its source in the tray") {
	// THE SAME ONE AUTHORITY FOR A DEVICE'S OUTPUT: a MACROS card's four outputs each read "Macros N · <label>",
	// the label being what this instance calls it (`ParamBridge::outputLabel`). Asserted as the literal
	// text, not as "whatever modSourceName says", for the renamed macro's reason.
	ModFixture f;
	const size_t slot = f.bridge.addRackDevice("MACROS");
	REQUIRE(slot < kMaxRackSlots);
	const DeviceInstanceId macros = f.bridge.rackDeviceAt(slot);
	REQUIRE(macros != kNoInstance);
	const std::string head = "Macros " + std::to_string(f.bridge.rackSlotOf(macros) + 1);
	const backplane::Source second = backplane::Source::output(macros, 1);
	// BEFORE: the declared name, so each of the four is told apart even unlabelled.
	CHECK(ui::modSourceName(f.bridge, second) == head + " \xc2\xb7 Macro 2");

	f.bridge.setOutputLabel(macros, 1, "Wobble");
	CHECK(ui::modSourceName(f.bridge, second) == head + " \xc2\xb7 Wobble");
	CHECK(ui::modSourceName(f.bridge, backplane::Source::output(macros, 0)) == head + " \xc2\xb7 Macro 1");

	// THE TRAY, reached the way a load reaches it — the refused-route report names its source.
	ui::NotifyTray* tray = findInTree<ui::NotifyTray>(f.root);
	REQUIRE(tray != nullptr);
	feathers::RouteRefusals refused;
	refused.count = 1;
	refused.firstSource = second;
	refused.firstDestination = backplane::Destination::core(0);
	f.bridge.noteRefusedRoutes(refused);
	f.gui.tickForTest();
	CAPTURE(tray->text());
	CHECK(tray->text().find(head + " \xc2\xb7 Wobble") != std::string::npos);

	// A ONE-OUTPUT DEVICE IS UNCHANGED: the LFO's name already says which output there is.
	const size_t lfoSlot = f.bridge.addRackDevice("LFO");
	REQUIRE(lfoSlot < kMaxRackSlots);
	const DeviceInstanceId lfo = f.bridge.rackDeviceAt(lfoSlot);
	CHECK(ui::modSourceName(f.bridge, backplane::Source::output(lfo, 0))
	      == "LFO " + std::to_string(f.bridge.rackSlotOf(lfo) + 1));
}

TEST_CASE("the fx lane's output arm picks up a relabel") {
	// **NOT MACROS: IT CANNOT SIT HERE.** The fx lane holds devices with an audio half
	// (`maySitInAudioChain`), and MACROS has none. The label road is the device-agnostic one
	// (`ParamBridge::setOutputLabel` takes any declared output), so SKLITTER — audio AND seven outputs —
	// is the device that puts it in this lane. The lane REBUILDS an arm on a relabel (the label is part of
	// its identity), so the claim is about the arm at that place, whichever object it is.
	ModFixture f;
	auto* lane = findInTree<ui::FxLane>(f.root);
	REQUIRE(lane != nullptr);
	lane->setCollapsed(false);
	const size_t slot = f.bridge.addFxDevice("SKLITTER");
	REQUIRE(slot < feathers::kMaxFxSlots);
	const DeviceInstanceId device = f.bridge.fxDeviceAt(slot);
	REQUIRE(device != kNoInstance);
	lane->resized();
	lane->scrollCardIntoView(slot);
	lane->resized();
	f.gui.tickForTest();
	const ui::SourceArm* before = lane->outputArmForTest(slot, 1);
	REQUIRE(before != nullptr);
	CHECK(before->label() == "Perf Y");

	f.bridge.setOutputLabel(device, 1, "Wobble sweep");
	const ui::SourceArm* after = lane->outputArmForTest(slot, 1);
	REQUIRE(after != nullptr);
	CHECK(after->label() == "Wobble sweep");
	CHECK(lane->outputArmBoundsForTest(slot, 1).width()
	      == doctest::Approx(ui::SourceArm::widthFor("Wobble sweep")));
	// NOT VACUOUS: the two words measure differently.
	CHECK(ui::SourceArm::widthFor("Wobble sweep") != doctest::Approx(ui::SourceArm::widthFor("Perf Y")));
	// ITS NEIGHBOURS KEEP THEIR DECLARED NAMES.
	REQUIRE(lane->outputArmForTest(slot, 0) != nullptr);
	CHECK(lane->outputArmForTest(slot, 0)->label() == "Perf X");
}

TEST_CASE("every Engine rail section fits inside the rail it is drawn in") {
	// THE REGRESSION THIS ARC CAUSED AND THIS CASE EXISTS TO STOP RECURRING. The source grid is a new
	// section in a rail whose height is fixed by the editor's own logical size, and adding it pushed
	// the routings list off the bottom — where its rows are still laid out, still hit-testable by
	// arithmetic, and unreachable by a hand. Four cases failed and none of them was about the grid.
	//
	// A rail cannot scroll (unlike the modulator strip beside it), so "fits" is a real constraint
	// rather than a preference, and the next section added here — the voice-stack modulators are
	// already planned for this very grid — should find out from a test rather than from a hand.
	ModFixture f;
	auto* engineRail = findInTree<ui::EngineRail>(f.root);
	auto* grid = findInTree<ui::SourceGrid>(f.root);
	auto* routes = findInTree<ui::RouteList>(f.root);
	REQUIRE(engineRail != nullptr);
	REQUIRE(grid != nullptr);
	REQUIRE(routes != nullptr);
	REQUIRE(engineRail->height() > 0.0f);

	const float bottom = engineRail->positionInWindow().y + engineRail->height();
	CHECK(grid->positionInWindow().y + grid->height() <= bottom);
	// The list is measured at the height it ASKS for rather than the one it happens to have been given,
	// because that is the height it will draw its rows at the moment a route exists.
	CHECK(routes->positionInWindow().y + routes->preferredHeight() <= bottom);
	// AND NOTHING IS SCROLLED AWAY AT THE DESIGN CANVAS. That is the state every other case in this
	// file is written against, and the one a wheel must not be able to disturb.
	CHECK(engineRail->maxScroll() == doctest::Approx(0.0f));
}
TEST_CASE("a source with several routes keeps every row reachable") {
	// THE REGRESSION THE GRID CAUSED, found by measuring the rail rather than by looking at it. The
	// routings list grows a row per route, and before the source grid existed there was room above the
	// tray for all of them; the grid spent that headroom, so a source with more than one route now has
	// rows past the bottom of a rail that cannot grow. A row that is laid out and unreachable is the
	// worst kind of control — it hit-tests by arithmetic and answers nothing to a hand.
	//
	// The claim is REACHABILITY rather than fit: a rail shorter than its contents is allowed, and a
	// row a hand cannot get to is not.
	ModFixture f;
	auto* engineRail = findInTree<ui::EngineRail>(f.root);
	auto* routes = findInTree<ui::RouteList>(f.root);
	REQUIRE(engineRail != nullptr);
	REQUIRE(routes != nullptr);

	// Every destination routed from one macro, which is the tallest this list can ever be — EVERY
	// DESTINATION THE MACRO MAY REACH, asked of the routing rule. The dense store kept a route to an empty
	// lane and this counted all 464 cells; the bridge's record keeps only routes something can hear
	// (chain-as-a-device 5b C5), so the tallest list is the legal one.
	//
	// **WALKED AS ENDPOINTS (kill-lanes step 5), which is a walk over what EXISTS rather than over a
	// table.** It used to walk the flat destination axis through `route_coords.h`, and a coordinate walk
	// spends most of itself on columns no device stands in. **THE LEGAL SET IS THE SAME ONE EITHER WAY —
	// THE OFFERED SET IS NOT, AND THE DIFFERENCE IS THE POINT OF THE LAST OFFER BELOW.** What is walked
	// here is the host's own parameter table plus every control of every device the two chains HOLD; a
	// device end names the DEVICE, so this cannot drift when a chain is reordered. What the coordinate
	// sweep also handed over, and an enumeration over live devices cannot reach by construction, is a
	// pair naming a device that is not there: its EMPTY-SLOT columns.
	//
	// EVERY ONE OF THEM IS OFFERED AND ONLY THE LEGAL ONES ARE COUNTED, which is what makes the row
	// count below a claim about the record's gate rather than about this loop: the load is handed the
	// illegal pairs too, and the list must have exactly the legal ones in it.
	//
	// **AND BOTH NUMBERS COME OFF THE SAME OFFERED SET, SO NARROWING IT MOVES THEM TOGETHER AND THIS CASE
	// STAYS GREEN.** A green here cannot report that its own coverage shrank, which is why what is offered
	// is spelled out rather than left to whatever the loops happen to reach: each class of illegal pair
	// the gate must refuse is offered ON PURPOSE — a control that is not modulatable (every control of a
	// live device is offered, not every modulatable one), and a device the record does not hold.
	std::vector<backplane::Route> full;
	size_t reachable = 0;
	const backplane::Source macro = macroSourceAt(0);
	const auto offer = [&](backplane::Destination d) {
		full.push_back({ macro, d, 0.5f });
		// A SOUND'S IDENTITY (backlog PQ) is reached by a Relative macro only, but the RECORD keeps such a route
		// whatever the macro's type when it is loaded — a type gate at load would depend on whether the macro-modes
		// block had been restored yet — so it is a row here, inert in the engine (`ParamSet::rebuildCoreInputs`).
		const bool editOnly = !d.isDevice() && feathers::isEditOnlyDestination(d.index);
		reachable += f.bridge.mayRoute(macro, d) || editOnly ? 1 : 0;
	};
	for (size_t parameter = 0; parameter < feathers::kParamDestinationCount; ++parameter) {
		offer(backplane::Destination::core(parameter));
	}
	const auto offerControlsOf = [&](DeviceInstanceId id) {
		const device::Descriptor* desc = id != kNoInstance ? f.bridge.descriptorOf(id) : nullptr;
		if (desc == nullptr) {
			return;
		}
		for (size_t control = 0; control < desc->params.size(); ++control) {
			offer(backplane::Destination::control(id, control));
		}
	};
	for (size_t slot = 0; slot < feathers::kMaxFxSlots; ++slot) {
		offerControlsOf(f.bridge.fxDeviceAt(slot));
	}
	for (size_t slot = 0; slot < feathers::kMaxRackEnvelopes; ++slot) {
		offerControlsOf(f.bridge.rackDeviceAt(slot));
	}
	// **A DEVICE THE RECORD DOES NOT HOLD — the class the empty-slot columns used to cover.** Ids come
	// from a counter that starts at `kFirstInstance` and never reuses, so one far past anything this
	// fixture could have minted names nothing; the constant is not load-bearing because the REQUIRE is
	// what establishes it, and `descriptorOf` answering null IS "the record does not hold this".
	const DeviceInstanceId unheld = static_cast<DeviceInstanceId>(1u << 24);
	REQUIRE(f.bridge.descriptorOf(unheld) == nullptr);
	offer(backplane::Destination::control(unheld, 0));

	f.bridge.loadRoutesForTest(full);

	// **AND THE UNHELD ONE WAS REFUSED, ASKED OF THE RECORD RATHER THAN INFERRED FROM THE ROW COUNT.**
	// The count below notices it only if the list also draws a row for a destination it cannot name,
	// which is a second thing that has to be true; this reads the value the claim is about.
	CHECK(f.bridge.routeAmount(macro, backplane::Destination::control(unheld, 0)) == 0.0f);

	f.arm(macroSourceAt(0));
	REQUIRE(reachable > 40); // tall enough to overflow the rail, or the case asks nothing
	REQUIRE(routes->rows().size() == reachable);
	engineRail->resized();

	// It no longer fits, which is the known-bad this case exists for.
	REQUIRE(engineRail->maxScroll() > 0.0f);

	const float bottom = engineRail->positionInWindow().y + engineRail->height();
	engineRail->setScroll(engineRail->maxScroll());
	CHECK(routes->positionInWindow().y + routes->preferredHeight() <= bottom + 0.5f);

	// And the way back is the whole way: scrolled home, the panel starts where it always started.
	engineRail->setScroll(0.0f);
	CHECK(engineRail->scroll() == doctest::Approx(0.0f));

	// Clamped rather than free — a rail that scrolled past its own end would leave a hand looking at
	// empty rail wondering where its controls went.
	// PAST THE END BY A MARGIN, not at a fixed 10000: the drum pads' destinations (2026-11-05) took the full list past 10000.
	engineRail->setScroll(engineRail->maxScroll() + 10000.0f);
	CHECK(engineRail->scroll() == doctest::Approx(engineRail->maxScroll()));
}

namespace {

/// **THE ENGINE RAIL'S ROUTE LIST, ROUTED THROUGH THE KNOBS AND SEEN ONLY THROUGH THE EDITOR'S TICK** (backlog LV).
///
/// THE DEFECT: the Engine rail's route list showed only one route, not all of them. The Engine rail
/// placed the list at `preferredHeight()` only inside `layOutInto(true)`, which ran on a resize and a scroll — and an
/// empty list asks for ONE row, so a list laid out before anything was routed kept a one-row frame for every route
/// made after it, and visage clipped the rest. "a source with several routes keeps every row reachable" above calls
/// `Engine rail->resized()` by hand after routing, which is the step a hand never takes; these take only the hand's
/// steps and `tickForTest`.
struct RouteRowsFixture : ModFixture {
	ui::EngineRail* engineRail = nullptr;
	ui::RouteList* routes = nullptr;

	RouteRowsFixture() {
		engineRail = findInTree<ui::EngineRail>(root);
		routes = findInTree<ui::RouteList>(root);
		REQUIRE(engineRail != nullptr);
		REQUIRE(routes != nullptr);
		REQUIRE(routes->allRows().empty());
	}

	/// A route from the armed source, by dragging the knob's face up.
	void route(ParamId id, float rise) {
		ui::Knob* knob = knobFor(id);
		REQUIRE(knob != nullptr);
		const visage::Point centre = faceCentre(knob);
		dragThroughRoot(root, centre, { centre.x, centre.y - rise });
	}

	/// Three routes from macro 0, and the Engine rail laid out for them by an explicit resize — a known starting state
	/// for the cases about what happens AFTER, so their red is about their own step.
	void threeRoutesLaidOut() {
		arm(macroSourceAt(0));
		route(ParamId::Path, 35.0f);
		route(ParamId::VoiceLevel, 20.0f);
		route(ParamId::FineTune, 20.0f);
		REQUIRE(routes->allRows().size() == 3);
		engineRail->resized();
	}

	static float heightFor(size_t rows) {
		return ui::RouteList::kHeadingHeight
		       + static_cast<float>(std::max<size_t>(rows, 1)) * ui::RouteList::kRowHeight;
	}

	/// THE CLAIM, BY VALUE: the frame is the height of the rows it has, in logical AND native pixels (CLAUDE.md's
	/// scale rule), every row lies inside it, and the panel's content ends where the list does — so the scroll
	/// range is the list's too.
	void holdsEveryRow(size_t rows) {
		REQUIRE(routes->allRows().size() == rows);
		INFO("rows " << rows << ", frame height " << routes->height() << ", preferred " << routes->preferredHeight());
		CHECK(routes->height() == doctest::Approx(heightFor(rows)));
		CHECK(routes->nativeHeight() == static_cast<int>(std::lround(heightFor(rows) * routes->dpiScale())));
		for (size_t index = 0; index < rows; ++index) {
			CHECK(routes->rowBounds(index).bottom() <= routes->height() + 0.5f);
		}
		const float listTop = routes->positionInWindow().y - engineRail->positionInWindow().y + engineRail->scroll();
		CHECK(engineRail->contentHeight() == doctest::Approx(listTop + heightFor(rows)));
	}

	/// Where row `index`'s × is, in window coordinates.
	visage::Point crossOf(size_t index) const {
		const visage::Bounds row = routes->rowBounds(index);
		return routes->positionInWindow()
		       + visage::Point { row.right() - ui::RouteList::kRemoveWidth * 0.5f, row.y() + row.height() * 0.5f };
	}
};

} // namespace

TEST_CASE("the route list grows a row for each route made, with no resize, and the last row answers its ×") {
	RouteRowsFixture f;
	// ONE ROUTE FIRST — laid out, ticked, and right, which is the state the broken list was stuck in.
	f.arm(macroSourceAt(0));
	f.route(ParamId::Path, 35.0f);
	f.gui.tickForTest();
	f.holdsEveryRow(1);

	// TWO MORE FROM THE SAME SOURCE, through the knobs, and nothing but the editor's tick after them.
	f.route(ParamId::VoiceLevel, 20.0f);
	f.route(ParamId::FineTune, 20.0f);
	f.gui.tickForTest();
	f.holdsEveryRow(3);

	// AND THE LAST ROW ANSWERS A HAND WHERE IT IS DRAWN: its × through the real tree, scrolled into the rail first
	// (the scroll is the hand's own step; the press is what is asked). The removal is read from the bridge.
	const std::vector<ui::RouteList::Row> before = f.routes->allRows();
	f.engineRail->setScroll(f.engineRail->maxScroll());
	const visage::Point cross = f.crossOf(2);
	REQUIRE(cross.y < f.engineRail->positionInWindow().y + f.engineRail->height());
	REQUIRE(f.bridge.routeAmount(macroSourceAt(0), before[2].destination) != 0.0f);
	dragThroughRoot(f.root, cross, cross);
	CHECK(f.bridge.routeAmount(macroSourceAt(0), before[2].destination) == 0.0f);
	f.gui.tickForTest();
	f.holdsEveryRow(2);
}

TEST_CASE("the route list shrinks when a route is removed, and the Engine rail's content and scroll follow it") {
	RouteRowsFixture f;
	f.threeRoutesLaidOut();
	f.holdsEveryRow(3);
	// Scrolled to the end, so a shorter list must also bring the scroll back inside the content.
	f.engineRail->setScroll(f.engineRail->maxScroll());
	dragThroughRoot(f.root, f.crossOf(0), f.crossOf(0));
	f.gui.tickForTest();
	f.holdsEveryRow(2);
	CHECK(f.engineRail->scroll() <= f.engineRail->maxScroll() + 0.001f);
}

TEST_CASE("the route list is re-laid for the armed source's own count when the arm moves between sources") {
	RouteRowsFixture f;
	f.arm(macroSourceAt(2));
	f.route(ParamId::PosX, 20.0f);
	f.threeRoutesLaidOut();
	f.holdsEveryRow(3);
	f.arm(macroSourceAt(2));
	f.gui.tickForTest();
	f.holdsEveryRow(1);
	f.arm(macroSourceAt(0));
	f.gui.tickForTest();
	f.holdsEveryRow(3);
}

TEST_CASE("a tick with the routes unchanged lays the Engine rail out again never") {
	// THE TRIGGER IS THE LIST'S HEIGHT, NOT THE TICK: the editor ticks every frame and animates, and a re-layout on
	// every tick is a cost paid for nothing. A re-layout to the same bounds moves nothing a case could read, so the
	// panel counts its own placing walks.
	RouteRowsFixture f;
	f.threeRoutesLaidOut();
	f.gui.tickForTest();
	const size_t before = f.engineRail->contentRelayoutsForTest();
	f.gui.tickForTest();
	f.gui.tickForTest();
	CHECK(f.engineRail->contentRelayoutsForTest() == before);
	// AND IT IS NOT VACUOUS: one more route, and the next tick does lay out.
	f.route(ParamId::PosX, 20.0f);
	f.gui.tickForTest();
	CHECK(f.engineRail->contentRelayoutsForTest() > before);
}

TEST_CASE("switching a sound off does not disarm the arrow already pointing from it") {
	// THE ASYMMETRY, DECIDED AND PINNED. A greyed arrow refuses to BE armed, because a switched-off
	// sound is absent from the blend and there would be nothing to route from. But a sound switched
	// off while its arrow is already armed keeps the arming: refusing to start something meaningless
	// and destroying a gesture under way because an unrelated switch moved are different acts. A route
	// made from a source that is currently dark is legitimate — it reads zero until the sound comes
	// back — and that beats making a hand arm the same thing twice.
	//
	// Written because the two halves look inconsistent to anybody who meets only one of them, and a
	// deliberate behaviour with no case is a behaviour somebody later "fixes".
	ModFixture f;
	auto* grid = findInTree<ui::SourceGrid>(f.root);
	REQUIRE(grid != nullptr);
	REQUIRE(grid->slotEnabled(0));

	const visage::Bounds cell = grid->cellBounds(0);
	const visage::Point at = grid->positionInWindow()
	                         + visage::Point { cell.x() + cell.width() * 0.5f,
		                                       cell.y() + cell.height() * 0.5f };
	dragThroughRoot(f.root, at, at);
	REQUIRE(grid->slotArmed(0));

	// Switch that sound off the whole way round — posted, drained, published — because there is no
	// audio thread here to carry it.
	f.bridge.setValue(static_cast<clap_id>(soundParam(0, SoundField::Enable)), 0.0);
	ParamSet params;
	for (const ParamDesc& desc : paramDescs()) {
		params.setBase(static_cast<clap_id>(desc.id), f.bridge.value(static_cast<clap_id>(desc.id)));
	}
	f.bridge.drain([&](const UiParamEvent& event) {
		if (event.kind == UiParamEvent::Kind::Value) {
			params.setBase(event.id, event.value);
		}
	});
	f.bridge.publish(params);
	f.bridge.refresh();

	// The arrow is greyed now — and still armed.
	CHECK_FALSE(grid->slotEnabled(0));
	CHECK(grid->slotArmed(0));

	// And the arming still MEANS something: a legal destination is still mappable, so the route the
	// hand was about to make can still be made.
	ui::Knob* fine = f.knobFor(ParamId::FineTune);
	REQUIRE(fine != nullptr);
	CHECK(fine->mappable());
	// The puck stays refused, because that is about the source's CLASS rather than its state.
	ui::Knob* path = f.knobFor(ParamId::Path);
	REQUIRE(path != nullptr);
	CHECK_FALSE(path->mappable());
}

TEST_CASE("the filter card's own controls answer a click delivered the way a window delivers one") {
	// WHY THIS CASE EXISTS, and it is not because the controls were broken. Every other FilterCard
	// case drives `card->onMouseDown().callback(...)`, which invokes the handler directly and proves
	// nothing about whether a click can REACH the card — the gap this file's own header warns about
	// in the words "a knob can pass every test it has and still be unreachable where the layout puts
	// it". A day was spent on a report that the card was dead to real clicks; it was not, and the
	// only reason nobody could say so quickly is that no case asked this question. So this one asks
	// it through the root, at the real window coordinates, for every control the card owns.
	//
	// AND IT DRAINS. The editor writes onto a ring the audio thread drains; a value read back
	// without draining is the OLD value no matter what the click did, which is exactly the reading
	// that made a working card look dead in a host that never drains.
	ModFixture f;
	auto* card = findInTree<ui::FilterCard>(f.root);
	REQUIRE(card != nullptr);
	REQUIRE(card->isVisible());

	const auto windowPointIn = [&](visage::Bounds b) {
		return card->positionInWindow()
		       + visage::Point { b.x() + b.width() * 0.5f, b.y() + b.height() * 0.5f };
	};
	const auto settle = [&](ParamSet* params) {
		f.bridge.drain([&](const UiParamEvent& event) {
			if (event.kind == UiParamEvent::Kind::Value) {
				params->setBase(event.id, event.value);
			}
		});
		f.bridge.publish(*params);
		f.bridge.refresh();
	};

	ParamSet params;
	// READ FROM THE ROW, NOT FROM THE PARAMETER TABLE. The enable switch writes the filter's chain row
	// now; a read of the old clap id would answer with whatever the table was left at and would have
	// gone on passing while the click reached nothing at all.
	const auto enabledNow = [&] {
		return fxFilterAt(f.bridge.fxChain(), 0).settings.enabled;
	};

	// THE ONE THAT MATTERS MOST: the enable switch is the only editor affordance for putting the
	// device in the signal, so a click that cannot reach it is a device nobody can turn on.
	REQUIRE_FALSE(enabledNow());
	const visage::Point enableAt = windowPointIn(card->enableBounds());
	REQUIRE(f.root->frameAtPoint(enableAt) == card); // the click lands on the card, not through it
	dragThroughRoot(f.root, enableAt, enableAt);
	settle(&params);
	CHECK(enabledNow());

	dragThroughRoot(f.root, enableAt, enableAt);
	settle(&params);
	CHECK_FALSE(enabledNow()); // and it is a toggle, not a one-way switch

	// THE FOLD, which is the card's own state rather than a parameter.
	const bool foldedBefore = card->folded();
	const visage::Point foldAt = windowPointIn(card->headBounds());
	REQUIRE(f.root->frameAtPoint(foldAt) == card);
	doubleClickHead(f.root, foldAt);
	CHECK(card->folded() != foldedBefore);
	doubleClickHead(f.root, foldAt);
	CHECK(card->folded() == foldedBefore);

	// A KNOB IS THE POSITIVE CONTROL. If the routing were broken in general this would fail too, so
	// its passing is what makes the assertions above mean "the card answers" rather than "clicks work".
	REQUIRE_FALSE(card->knobsForTest().empty());
	ui::Knob* knob = card->knobsForTest().front().get();
	REQUIRE(knob->positionInWindow().x > 0.0f);
	CHECK(f.root->frameAtPoint(knob->positionInWindow()
	                           + visage::Point { knob->width() * 0.5f, knob->height() * 0.5f })
	      == knob);
}

TEST_CASE("a filter that is switched out of the signal reads as switched off") {
	// A disabled device's UI is grey. The device defaults OFF, so
	// this is the state every fresh instance opens in, and the card drew it at full strength — a
	// picture of a filter in the signal when there is no filter in the signal.
	//
	// ASKED OF THE CONTROLS, not of a photograph. Each of them already knows how to draw itself
	// switched off (Knob::setEnabled, SegmentedControl::setEnabled), so what this checks is that the
	// card TELLS them, which is a question the widgets can answer without a camera.
	FilterFixture f;
	visage::Canvas canvas;

	REQUIRE(f.param(fx::filter::Param::Enabled) < 0.5); // off by default, always
	f.card->draw(canvas);

	REQUIRE_FALSE(f.card->knobsForTest().empty());
	for (const auto& knob : f.card->knobsForTest()) {
		CHECK_FALSE(knob->active());
		// AND STILL LIVE TO THE HAND — the other half of the decision, which the case below has the teeth
		// for. Greying is a statement about the signal path, not about what you are allowed to touch.
		CHECK(knob->enabled());
	}
	// POLY AND KEYTRACK ARE CHIPS, which have no greyed-but-pointable look: usable whatever the device's state.
	REQUIRE(f.card->keytrackForTest() != nullptr);
	CHECK(f.card->keytrackForTest()->usable());
	CHECK(f.card->polyForTest()->usable());

	// SWITCHED IN THROUGH THE ROUTED PATH, the way a window delivers a click, and the card comes up.
	f.click(f.card->enableBounds());
	f.settle();
	REQUIRE(f.param(fx::filter::Param::Enabled) >= 0.5);
	f.card->draw(canvas);
	for (const auto& knob : f.card->knobsForTest()) {
		CHECK(knob->active());
	}
	// POLY AND KEYTRACK STAY USABLE WITH THE CARD, whatever Poly says: keytrack changes sound on the bus too,
	// so the honesty rule wants it live in both Poly states.
	CHECK(f.card->polyForTest()->usable());
	CHECK(f.card->keytrackForTest()->usable());
	f.set(fx::filter::Param::Poly, 1.0);
	f.settle();
	f.card->draw(canvas);
	CHECK(f.card->keytrackForTest()->usable());
	f.set(fx::filter::Param::Poly, 0.0);
	f.settle();
	f.card->draw(canvas);
	CHECK(f.card->keytrackForTest()->usable());

	// AND THE SQUARE IS THE WAY BACK. It is the one control on a switched-out card that is still doing
	// anything, which is why it is the one thing the greying does not touch: every other control can
	// afford to recede because none of them can undo this.
	f.click(f.card->enableBounds());
	f.settle();
	REQUIRE(f.param(fx::filter::Param::Enabled) < 0.5);
	f.card->draw(canvas);
	CHECK_FALSE(f.card->knobsForTest().front()->active());
}

TEST_CASE("a switched-out filter can still be dialled in") {
	// WHAT GREYING DOES NOT CARRY: a grey card is still an editable one. You may set a sound up
	// before you switch it on — the mod panel lets you draw a route from a source sitting at zero and
	// the rail lets you edit a sound you cannot currently hear, and this is the same case. The known-bad
	// is the obvious implementation: calling Knob::setEnabled(false) to get the colours, which also
	// takes the mouse away and makes a fresh instance a card you can look at and not touch.
	FilterFixture f;
	visage::Canvas canvas;
	REQUIRE(f.param(fx::filter::Param::Enabled) < 0.5);
	f.card->draw(canvas); // the greying is applied here, so everything below happens to a GREY card

	// A KNOB, dragged where the layout actually put it.
	ui::Knob* cutoff = f.card->knobsForTest().front().get();
	const double before = f.param(fx::filter::Param::Cutoff);
	const visage::Point centre = f.faceCentre(cutoff);
	REQUIRE(f.root->frameAtPoint(centre) == cutoff);
	dragThroughRoot(f.root, centre, { centre.x, centre.y - 20.0f });
	f.settle();
	CHECK(f.param(fx::filter::Param::Cutoff) > before);

	// THE TYPE ROW, which is drawn by the card itself rather than by a child widget and so takes its
	// dimming from a different piece of code — worth its own assertion for that reason alone.
	const int type = static_cast<int>(f.param(fx::filter::Param::Type));
	f.click(f.card->arrowBounds(1));
	f.settle();
	CHECK(static_cast<int>(f.param(fx::filter::Param::Type)) == type + 1);

	// AND THE KEYTRACK SWITCH, whose off/on is an architecture rather than a level and is exactly the
	// kind of thing a hand sets up before switching the device in.
	ui::SwitchChip* keytrack = f.card->keytrackForTest();
	REQUIRE(keytrack != nullptr);
	REQUIRE(f.param(fx::filter::Param::Keytrack) < 0.5);
	const visage::Point onSegment =
	    keytrack->positionInWindow()
	    + visage::Point { keytrack->width() * 0.5f, keytrack->height() * 0.5f };
	REQUIRE(f.root->frameAtPoint(onSegment) == keytrack);
	f.clickAt(onSegment);
	f.settle();
	CHECK(f.param(fx::filter::Param::Keytrack) >= 0.5);

	// AND NONE OF THAT SWITCHED THE DEVICE ON BY ACCIDENT: the card is still grey, and still off.
	CHECK(f.param(fx::filter::Param::Enabled) < 0.5);
	f.card->draw(canvas);
	CHECK_FALSE(cutoff->active());
}

TEST_CASE("the response curve is drawn in greys rather than in a blue-to-orange ramp") {
	// The filter curve is grey by design. It shipped as a banded
	// ramp from kAccent (#7FA8E8) to kValue (#E5A97A) — two of the palette's three semantic colours at
	// full chroma, spent on a line that is neither an interaction nor a value readout.
	//
	// MEASURED OFF A RENDER rather than read off a constant, because a constant is not what a hand
	// sees: the colour is quantised into bands, the stroke is antialiased, and the card's ground shows
	// through it. Windowless — nothing here opens a window.
	//
	// CHROMA IS THE CLAIM, stated as the widest gap between a pixel's channels. The known-bad ramp
	// measures 106 of 255 at its ends and fails this; a grey line cannot, whatever brightness it is
	// drawn at, which is what makes chroma the right thing to bound rather than "is it kMuted".
	FilterFixture f;
	// SWITCHED IN, so the curve draws at full strength. A switched-out card fades everything by
	// kOffAlpha, and a faded colour over the ground measures a chroma nobody chose.
	f.click(f.card->enableBounds());
	f.settle();
	REQUIRE(f.param(fx::filter::Param::Enabled) >= 0.5);

	const int width = static_cast<int>(std::ceil(f.card->width()));
	const int height = static_cast<int>(std::ceil(f.card->height()));
	REQUIRE(width > 0);
	REQUIRE(height > 0);

	visage::Canvas canvas;
	markedWindowless(canvas, width, height);
	canvas.setColor(0xff000000);
	canvas.fill(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height));

	visage::Region region;
	region.setBounds(0, 0, width, height);
	canvas.addRegion(&region);
	canvas.beginRegion(&region);
	f.card->draw(canvas);
	canvas.endRegion();

	// MARKED, AND THIS IS THE CASE THAT NEEDED IT MOST: its chroma bound read 172-191 against 40 on
	// frames that were not this card's (backlog CW, GK, IT) — a plausible number for a wrong picture.
	// A capture of anything but this frame now stops at the marker and names the venue.
	const visage::Screenshot shot = takeMarkedCapture(canvas, width, height);
	REQUIRE(shot.width() == width);

	// ONLY THE CURVE'S OWN PIXELS. Inside the curve box the card also draws its void ground and the
	// 0 dB line (kLine at 0.7), and the brightest channel either of those reaches is 63 — so a floor
	// of 90 separates the stroke from its furniture without knowing where the stroke went.
	const visage::Bounds box = f.card->curveBounds();
	struct Sample {
		int red = 0;
		int green = 0;
		int blue = 0;
		int peak = 0;
	};
	const auto sampleAt = [&](int x, int y) {
		Sample s;
		s.red = static_cast<int>(shot.sample(x, y).hexRed());
		s.green = static_cast<int>(shot.sample(x, y).hexGreen());
		s.blue = static_cast<int>(shot.sample(x, y).hexBlue());
		s.peak = std::max({ s.red, s.green, s.blue });
		return s;
	};
	const int left = static_cast<int>(box.x());
	const int right = std::min(static_cast<int>(box.right()), width);
	const int top = static_cast<int>(box.y());
	const int bottom = std::min(static_cast<int>(box.bottom()), height);
	REQUIRE(right > left);
	REQUIRE(bottom > top);

	int lit = 0;
	int widestChroma = 0;
	Sample brightestLow;
	Sample brightestHigh;
	// The ends, as fifths of the box: enough pixels to have a brightest one, narrow enough that each
	// is honestly "the low end" and "the high end" of a curve drawn left to right.
	const int lowEnd = left + (right - left) / 5;
	const int highEnd = right - (right - left) / 5;
	for (int y = top; y < bottom; ++y) {
		for (int x = left; x < right; ++x) {
			const Sample s = sampleAt(x, y);
			if (s.peak < 90) {
				continue;
			}
			++lit;
			widestChroma = std::max(widestChroma, s.peak - std::min({ s.red, s.green, s.blue }));
			if (x < lowEnd && s.peak > brightestLow.peak) {
				brightestLow = s;
			}
			if (x >= highEnd && s.peak > brightestHigh.peak) {
				brightestHigh = s;
			}
		}
	}

	// Reported either way: a chroma of zero is only worth reading next to a count that says the curve
	// was drawn at all.
	MESSAGE("curve pixels = " << lit << ", widest channel spread = " << widestChroma);
	MESSAGE("low end  = " << brightestLow.red << "," << brightestLow.green << "," << brightestLow.blue);
	MESSAGE("high end = " << brightestHigh.red << "," << brightestHigh.green << ","
	                      << brightestHigh.blue);
	REQUIRE(lit > 0);
	CHECK(widestChroma <= 40);

	// AND THE ORIENTATION CUE SURVIVES THE DESATURATION: the low end still leans cool and the high end
	// still leans warm, which is what makes it a hint rather than a decoration. Read off the brightest
	// pixel at each end, so partial coverage scales nothing that is being compared.
	REQUIRE(brightestLow.peak > 0);
	REQUIRE(brightestHigh.peak > 0);
	CHECK(brightestLow.blue > brightestLow.red);
	CHECK(brightestHigh.red > brightestHigh.blue);
}

TEST_CASE("the surfaces that draw from non-parameter channels are subscribed to them") {
	// THE GAP THAT SHOWED AS A STUCK PICTURE. Every one of these surfaces derives what it draws from
	// channels that are not parameters — travel, gamma, per-sound orientation, residue, the hues, the
	// variation rows — and watched only PARAMETERS. So a card, a wave or a map showed a placement that
	// had moved, and only caught up when something unrelated happened to invalidate the frame.
	//
	// ASSERTED AS A SUBSCRIPTION RATHER THAN AS A REDRAW, and that is a limit of the venue rather
	// than a softening of the claim: visage's redraw() does nothing on a frame that is not being
	// drawn, so a headless test cannot watch a repaint happen. What it can do is check that the wire
	// exists — and the other half, that the wire is pulled when a channel moves, is the bridge
	// suite's flag contract.
	QuietGuiHost host;
	ParamBridge bridge;
	/// THE SAME BRIDGE AS THE DEVICE SEAM. Cards and panels are converging onto `backplane::DeviceHost`
	/// so the rack can build them; `BridgeDeviceHost` is what keeps Feathers handing them the parameter
	/// road underneath, and a fixture needs one for the same reason `Gui::Impl` does.
	feathers::BridgeDeviceHost deviceHost { bridge };
	bridge.refresh();
	Gui gui(host, bridge);
	REQUIRE(Gui::isAvailable());
	REQUIRE(gui.create(CLAP_WINDOW_API_X11, false));

	const ui::FrameWatchers* watchers = gui.stateWatchers();
	REQUIRE(watchers != nullptr);

	visage::Frame* root = gui.root();
	REQUIRE(root != nullptr);

	// Found by name through the tree rather than by rebuilding the layout, so this keeps working when
	// the layout moves.
	// Identified by TYPE rather than by a name, because these frames carry none — and the same RTTI
	// walk this file already uses to say which frame a press landed on.
	int subscribed = 0;
	int surfaces = 0;
	std::function<void(visage::Frame*)> walk = [&](visage::Frame* frame) {
		for (visage::Frame* child : frame->children()) {
			const bool isSurface = dynamic_cast<ui::WaveDisplay*>(child) != nullptr
			                       || dynamic_cast<ui::SoundStack*>(child) != nullptr
			                       || dynamic_cast<ui::ExploreStage*>(child) != nullptr
			                       || dynamic_cast<ui::PerformStage*>(child) != nullptr
			                       // THE FIFTH, ADDED BEFORE IT NEEDS TO BE. The filter card draws its
			                       // curve from the filter's settings and today reaches them through
			                       // eight watched clap ids. Those ids are being swallowed into the
			                       // device's chain row, which is not a parameter, and on the day that
			                       // lands every one of those watches has nothing to attach to.
			                       //
			                       // A card that stops repainting fails SILENTLY — redraw() is a no-op
			                       // on a frame nobody is drawing and Frame exposes no staleness — so
			                       // there is no suite that would go red for it. The subscription is
			                       // the thing a headless venue can hold, so it goes in first.
			                       || dynamic_cast<ui::FilterCard*>(child) != nullptr;
			if (isSurface) {
				++surfaces;
				INFO("surface " << typeid(*child).name());
				CHECK(watchers->watches(child));
				subscribed += watchers->watches(child) ? 1 : 0;
			}
			walk(child);
		}
	};
	walk(root);
	// All five named surfaces were found — otherwise the loop above would pass by checking nothing,
	// which is how this kind of tree walk usually fails.
	CHECK(surfaces == 5);
	CHECK(subscribed == 5);

	// AND THE WIRE IS PULLED, which the two halves above do not say between them: the bit can rise
	// and the surfaces can be subscribed while nothing ever connects them. A mutation that simply
	// stopped tick() draining the bit survived both of them, which is how this check came to exist.
	//
	// Draining is the observable: after a channel moves the bit is up, and after a frame it is down
	// BECAUSE THE TICK TOOK IT — nothing else in the editor reads it.
	bridge.takeRepaintDirty();
	bridge.setSoundHue(3, 5);
	CHECK(bridge.takeRepaintDirty());

	bridge.setSoundHue(3, 6);
	gui.tickForTest();
	CHECK_FALSE(bridge.takeRepaintDirty());

	// And a frame with nothing to say leaves it alone rather than notifying on every tick.
	gui.tickForTest();
	CHECK_FALSE(bridge.takeRepaintDirty());
}

TEST_CASE("a click on the preset browser reaches the preset browser") {
	// WHO GETS THE CLICK — asked of the real tree rather than of the widget in isolation, which is the
	// only way this could be asked at all. Every existing browser case builds one at (0,0) and calls
	// hitsPrev/hitsName directly, so all of them pass on a browser nothing can click.
	//
	// The browser is a child of the top frame added AFTER the header, so it is on top, and
	// Frame::frameAtPoint walks children in reverse and stops at the first that contains the point and
	// does not ignore the mouse. That is the browser. It had no mouse handlers of its own, so the
	// click was swallowed — and the header's forwarding, which is where step() and the dropdown were
	// actually wired, could only ever fire for points the browser's box does NOT cover.
	//
	// Between that and a hit test translated in x but not in y, what remained live was a 13 px strip
	// of empty header ABOVE the box, which is presumably why it read as working.
	QuietGuiHost host;
	ParamBridge bridge;
	/// THE SAME BRIDGE AS THE DEVICE SEAM. Cards and panels are converging onto `backplane::DeviceHost`
	/// so the rack can build them; `BridgeDeviceHost` is what keeps Feathers handing them the parameter
	/// road underneath, and a fixture needs one for the same reason `Gui::Impl` does.
	feathers::BridgeDeviceHost deviceHost { bridge };
	Gui gui(host, bridge);
	REQUIRE(Gui::isAvailable());
	REQUIRE(gui.create(CLAP_WINDOW_API_X11, false));
	visage::Frame* root = gui.root();
	REQUIRE(root != nullptr);

	// The centre of each of the three pieces, in the top frame's coordinates, derived from where the
	// layout PUTS the browser rather than from a copy of those numbers.
	const float y = PresetBrowser::kTopOffset + PresetBrowser::kHeight * 0.5f;
	const float prevX = PresetBrowser::kLeftOffset + PresetBrowser::kArrowWidth * 0.5f;
	const float nameX = PresetBrowser::kLeftOffset + PresetBrowser::kArrowWidth
	                    + PresetBrowser::kNameWidth * 0.5f;
	const float nextX = PresetBrowser::kLeftOffset + PresetBrowser::kArrowWidth
	                    + PresetBrowser::kNameWidth + PresetBrowser::kArrowWidth * 0.5f;

	for (float x : {prevX, nameX, nextX}) {
		CAPTURE(x);
		visage::Frame* hit = root->frameAtPoint({x, y});
		REQUIRE(hit != nullptr);
		// The browser itself, not the header underneath it — which is what makes the header's
		// forwarding unreachable and the browser's own handlers the only ones that can run.
		CHECK(hit == static_cast<visage::Frame*>(gui.presetBrowserForTest()));
	}

	// AND THE BOTTOM OF THE BOX IS PART OF THE BOX. A point one pixel above its lower edge is inside
	// the control a user can see; the old hit test put the live region 13 px higher than the ink.
	const float lowerEdge = PresetBrowser::kTopOffset + PresetBrowser::kHeight - 1.0f;
	CHECK(root->frameAtPoint({nameX, lowerEdge})
	      == static_cast<visage::Frame*>(gui.presetBrowserForTest()));
}

TEST_CASE("a hand's move survives the very next refresh, and still tells the watchers") {
	// THE FLICKER UNDER EVERY KNOB. setValue writes the value the hand asked for straight into the
	// reading the widgets draw, so a move shows at once rather than a block later. refresh() then used
	// to diff the ENGINE's published values against that same reading — and so read the editor's own
	// optimism as a disagreement, and "corrected" it back to the value the engine had not been told
	// about yet. Every parameter move snapped to its old position for at least one frame:
	//
	//     setValue(cutoff, 10.97)   ->  editor shows 10.97
	//     refresh()                 ->  editor shows  9.97   <- the old value
	//     engine echoes             ->  editor shows 10.97
	//
	// Reported as "filters draw the wrong curve sometimes", and SOMETIMES is the tell: whether you see
	// it depends on where the frame lands against the flush round trip. Fixing the tick made it more
	// visible rather than less, by making the frames regular.
	// ON A PARAMETER THAT IS STILL ONE. This case was written on the filter's cutoff because that is
	// where the bug was REPORTED — "filters draw the wrong curve sometimes" — but nothing in the claim
	// is about the filter: it is about the editor's optimistic reading surviving a refresh with no
	// echo yet, which is true of every host parameter. The filter's controls are not host parameters
	// any more, so the case runs on Path and keeps saying exactly what it said.
	ParamBridge bridge;
	/// THE SAME BRIDGE AS THE DEVICE SEAM. Cards and panels are converging onto `backplane::DeviceHost`
	/// so the rack can build them; `BridgeDeviceHost` is what keeps Feathers handing them the parameter
	/// road underneath, and a fixture needs one for the same reason `Gui::Impl` does.
	feathers::BridgeDeviceHost deviceHost { bridge };
	bridge.refresh();
	const clap_id moved = static_cast<clap_id>(ParamId::Path);
	const double rest = bridge.value(moved);

	bridge.setValue(moved, rest + 0.5);
	REQUIRE(bridge.value(moved) == doctest::Approx(rest + 0.5)); // shown at once

	// THE CLAIM: one refresh with no echo yet must not move it. This is the assertion that fails on
	// the old diff, and it is a VALUE rather than "something is drawn".
	const std::span<const uint32_t> reported = bridge.refresh();
	CHECK(bridge.value(moved) == doctest::Approx(rest + 0.5));

	// AND THE WATCHERS STILL HEAR ABOUT IT, which is the half that must not be lost in fixing the
	// other. A widget that merely draws a value is redrawn by this notification and by nothing else,
	// so a move the diff kept quiet about would leave it a whole flush round trip behind.
	bool told = false;
	for (const uint32_t index : reported) {
		told = told || paramDescs()[index].id == ParamId::Path;
	}
	CHECK(told);

	// AND THE ENGINE'S WORD STILL WINS WHEN IT SPEAKS — automation, a preset load, or the echo landing
	// somewhere other than where the hand asked. Otherwise this fix would be a different bug: an
	// editor that stops following its own engine.
	ParamSet params;
	params.setBase(moved, rest + 0.25);
	bridge.publish(params);
	bridge.refresh();
	CHECK(bridge.value(moved) == doctest::Approx(rest + 0.25));

	// A quiet frame after all that tells nobody, so the touched flag is cleared rather than latched.
	CHECK(bridge.refresh().empty());
}

TEST_CASE("the filter card draws the rate the engine is running, not a fixed 48k") {
	// THE CARD'S PROMISE IS THAT THE CURVE IS THE FILTER THAT PLAYS, and the rate is part of which
	// filter that is. The evaluator was called with a hardcoded 48000 on the argument that "a response
	// plotted from 20 Hz to 20 kHz is the same picture at any rate a host would pick" — which was never
	// true near the top: the bilinear warp puts a 16 kHz corner in a different place relative to
	// Nyquist at 44.1k than at 48k, and a (since-dropped) model clamped at 0.3 x samplerate made it plain.
	//
	// MEASURED at up to 1.67 dB between the drawn curve and the filter that plays on a 44.1k session,
	// on that model. Small, but it is the one thing this card exists not to do.
	QuietGuiHost host;
	ParamBridge bridge;
	/// THE SAME BRIDGE AS THE DEVICE SEAM. Cards and panels are converging onto `backplane::DeviceHost`
	/// so the rack can build them; `BridgeDeviceHost` is what keeps Feathers handing them the parameter
	/// road underneath, and a fixture needs one for the same reason `Gui::Impl` does.
	feathers::BridgeDeviceHost deviceHost { bridge };
	CHECK(bridge.sampleRate() == doctest::Approx(48000.0)); // the old hardcode, until a host speaks

	WidgetRegistry registry;
	FilterCard card(deviceHost, registry, bridge.fxDeviceAt(0), nullptr);
	card.setBounds(0.0f, 0.0f, 300.0f, 400.0f);
	// SET IN THE ROW, which is where the card reads them. There is no host parameter to set any more,
	// and a case that went on writing one would be configuring nothing and then drawing the defaults.
	fx::filter::State state = fxFilterAt(bridge.fxChain(), 0);
	state.settings.enabled = true;
	state.settings.type = fx::filter::Type::Lowpass;
	state.settings.resonance = 0.7f;
	// Up near the top, where the warp makes the two rates disagree.
	state.settings.cutoffLog2Hz = static_cast<float>(std::log2(16000.0));
	bridge.setFilterSettings(0, state);
	REQUIRE(fxFilterAt(bridge.fxChain(), 0).settings.type == fx::filter::Type::Lowpass);

	std::array<float, 64> at48{};
	const size_t points = card.curveGains(at48.data(), at48.size());
	REQUIRE(points > 4);

	bridge.setSampleRate(44100.0);
	CHECK(bridge.sampleRate() == doctest::Approx(44100.0));
	std::array<float, 64> at441{};
	REQUIRE(card.curveGains(at441.data(), at441.size()) == points);

	// THE CURVE FOLLOWED THE RATE. Asserted as the VALUE the module's own evaluator gives at the new
	// rate, not merely as "the picture changed" — a card that read some other number, or that
	// recomputed at a third rate, would pass a difference check just as well.
	fx::filter::Settings s = card.settingsForTest();
	std::array<float, 64> freq{}, expected{};
	fx::filter::gainProfile(s, 44100.0, freq.data(), expected.data(), points);
	for (size_t i = 0; i < points; i++) {
		CAPTURE(freq[i]);
		CHECK(at441[i] == doctest::Approx(expected[i]).epsilon(1e-4));
	}

	// And the second assertion, which is what proves the first is not vacuous: the two rates really do
	// draw different curves for this type, so "it followed" is a claim with something behind it.
	bool differs = false;
	for (size_t i = 0; i < points; i++) {
		differs = differs || std::abs(at48[i] - at441[i]) > 1e-4f;
	}
	CHECK(differs);
}

TEST_CASE("the editor's heartbeat survives a close and reopen") {
	// THE SECOND OPEN IS A DIFFERENT CODE PATH from the first, and it is the one a musician takes
	// every time they hide and show the window. create() returns early on a reopen — the editor is
	// built once and kept for the life of the process, because destroying the window deadlocks on
	// Mesa's WSI threads — so anything create() does at its END happens exactly once, ever.
	//
	// destroy() stops the timer, so without a start on the early-return path the tick would be alive
	// on the first open and dead on every one after: the filter curve would follow the engine until
	// you closed the editor, and never again. setParent also starts it and in a real host always
	// follows create, but that makes it correct by convention rather than by construction, and this
	// harness has no window to parent to — which is exactly why the gap would have gone unnoticed.
	QuietGuiHost host;
	ParamBridge bridge;
	/// THE SAME BRIDGE AS THE DEVICE SEAM. Cards and panels are converging onto `backplane::DeviceHost`
	/// so the rack can build them; `BridgeDeviceHost` is what keeps Feathers handing them the parameter
	/// road underneath, and a fixture needs one for the same reason `Gui::Impl` does.
	feathers::BridgeDeviceHost deviceHost { bridge };
	Gui gui(host, bridge);
	REQUIRE(Gui::isAvailable());
	REQUIRE(gui.create(CLAP_WINDOW_API_X11, false));

	const auto pump = [] {
		for (int i = 0; i < 40; ++i) {
			visage::EventManager::instance().checkEventTimers();
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		}
	};
	pump();
	gui.takeDriveReportForTest();

	// Closed and opened again, the way a host does it.
	gui.destroy();
	REQUIRE(gui.create(CLAP_WINDOW_API_X11, false));

	bridge.setValue(static_cast<clap_id>(ParamId::AZone), 400.0);
	pump();
	// The same count registry.notify() makes, from a tick nobody called by hand — on the SECOND open.
	CHECK(gui.takeDriveReportForTest().paramFrames > 0);
}

TEST_CASE("no drag anywhere in the editor leaves a gesture open") {
	// AN UNMATCHED GESTURE IS HOST-FACING, and it is the one class of editor bug where the plugin
	// looks perfect and the DAW is wrong. beginGesture pushes a real event and the host is told a
	// parameter is being written; endGesture tells it the write finished. Drop the end and Bitwig is
	// left believing the gesture never stopped — an automation lane stuck in write, a parameter that
	// quietly stops answering automation — and NOTHING else here would notice, because every case we
	// own checks what the BRIDGE holds and never what the host was TOLD around it.
	//
	// THE EXISTING COVERAGE IS PER-PARAMETER AND THIS IS PER-WIDGET, which is the difference that
	// matters. There is already a case that drags the footer's knobs and checks a Begin and an End
	// appear in the queue for each; it enumerates parameters, so a widget that opens a gesture and
	// never closes it is caught only if that widget happens to sit on one of the ids it lists. This
	// drags EVERY knob the editor builds, wherever it lives, and asks the balance question instead.
	//
	// DELIBERATELY NOT THE ROUTE GESTURES. beginRouteGesture is an empty function and endRouteGesture
	// only sets a flag setRouteAmount already sets on every write, so a counter on those would be an
	// instrument measuring nothing — which in a test suite is its own kind of lie. Checked by reading
	// gui_bridge.cpp rather than inferred from the call pattern, because inferring it from the call
	// pattern is exactly how it was first reported as a live regression when it is not one.
	QuietGuiHost host;
	ParamBridge bridge;
	/// THE SAME BRIDGE AS THE DEVICE SEAM. Cards and panels are converging onto `backplane::DeviceHost`
	/// so the rack can build them; `BridgeDeviceHost` is what keeps Feathers handing them the parameter
	/// road underneath, and a fixture needs one for the same reason `Gui::Impl` does.
	feathers::BridgeDeviceHost deviceHost { bridge };
	Gui gui(host, bridge);
	REQUIRE(Gui::isAvailable());
	REQUIRE(gui.create(CLAP_WINDOW_API_X11, false));
	visage::Frame* root = gui.root();
	REQUIRE(root != nullptr);
	REQUIRE(bridge.openGestureCountForTest() == 0);

	std::vector<Knob*> knobs;
	const auto walk = [&](visage::Frame* frame, auto&& self) -> void {
		if (auto* knob = dynamic_cast<Knob*>(frame); knob != nullptr && knob->isVisible()) {
			knobs.push_back(knob);
		}
		for (visage::Frame* child : frame->children()) {
			self(child, self);
		}
	};
	walk(root, walk);
	REQUIRE(knobs.size() > 10); // a walk that found nothing would pass vacuously

	for (Knob* knob : knobs) {
		const visage::Point centre = centreInRoot(knob);
		// Both directions, so a control already sitting against one end of its range still moves —
		// a drag into a clamp posts nothing and would be read here as a widget that opens no gesture.
		dragThroughRoot(root, centre, { centre.x, centre.y - 40.0f });
		dragThroughRoot(root, centre, { centre.x, centre.y + 40.0f });
	}

	// THE CLAIM: every hand let go, so nothing is still being written.
	CHECK(bridge.openGestureCountForTest() == 0);
}

TEST_CASE("the editor drives its own tick, so a host with no timer is not a dead editor") {
	// WHO CALLS tick() — the question the case above does not ask, and the one two shipping paths get
	// wrong.
	//
	// Everything the engine tells the editor arrives through tick(): the parameter diff that moves the
	// filter curve, the state channel, and the terrain snapshot ExploreStage::update() watches for.
	// Until this heartbeat existed the only caller was the host, through posix-fd or
	// clap_host_timer_support, and:
	//
	//   - a macOS host offering neither never ticked at all (gui.cpp's guiCreate gate says so in
	//     as many words: "does not follow parameter changes");
	//   - clap-wrapper's VST3 shim starts its CFRunLoop idle timer inside setActive(true) only, and
	//     schedules the plugin's timers off ::clock() — CPU time, not wall time. On an idle main
	//     thread a 30 ms timer fires whenever the process happens to be busy, which is why the
	//     symptom read as "it redraws when I move the mouse".
	//
	// Both are invisible to the case above, which calls tickForTest() itself and therefore proves
	// only that the tick WORKS, never that anything runs it.
	//
	// SO THIS ONE NEVER CALLS IT. The editor rides visage's own frame loop — EventManager's timers are
	// checked from Window::drawCallback on every backend — and the only number read here is the one
	// registry.notify() produced INSIDE that tick. Delete startTicking() and it stays zero however
	// many times the loop is pumped; there is no way to fake a count the notification makes.
	QuietGuiHost host;
	ParamBridge bridge;
	/// THE SAME BRIDGE AS THE DEVICE SEAM. Cards and panels are converging onto `backplane::DeviceHost`
	/// so the rack can build them; `BridgeDeviceHost` is what keeps Feathers handing them the parameter
	/// road underneath, and a fixture needs one for the same reason `Gui::Impl` does.
	feathers::BridgeDeviceHost deviceHost { bridge };
	bridge.refresh();
	Gui gui(host, bridge);
	REQUIRE(Gui::isAvailable());
	REQUIRE(gui.create(CLAP_WINDOW_API_X11, false));

	// visage's frame loop, stood in for by the one call it makes. Pumped until the timer's period has
	// certainly elapsed, then drained, so what follows is this case's and not the build's.
	const auto pumpFrameLoop = [] {
		for (int i = 0; i < 40; ++i) {
			visage::EventManager::instance().checkEventTimers();
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		}
	};
	pumpFrameLoop();
	gui.takeDriveReportForTest();

	// THE PARAMETER ROAD, WITH NOBODY DRIVING BUT THE FRAME LOOP. Zone is watched by several surfaces.
	bridge.setValue(static_cast<clap_id>(ParamId::AZone), 400.0);
	pumpFrameLoop();
	const Gui::TickReport driven = gui.takeDriveReportForTest();
	CHECK(driven.paramFrames > 0);

	// AND THE CHANNEL ROAD, which is the terrain's road: a snapshot landing is noticed by a surface's
	// update(), and update() is only ever reached from tick().
	bridge.setSoundHue(2, 4);
	pumpFrameLoop();
	// EVERY SUBSCRIBED SURFACE AND EVERYTHING UNDER IT — the count is produced BY the notification
	// and is still the only evidence a repaint was delivered, but it counts FRAMES now rather than
	// watchers.
	//
	// IT USED TO SAY FIVE, and that number was the bug wearing a passing test. A watcher subscribes
	// because its DRAWING depends on this state; when one of them grew a child that draws from the
	// same state -- OWLTT's plot, moved out of the card into a frame of its own -- the notify reached
	// the parent and stopped. The card repainted, the picture did not, and five was still five.
	// Compared against the subtree rather than a literal so it cannot go stale the same way twice.
	CHECK(gui.takeDriveReportForTest().stateFrames == expectedStateFrames(gui));

	// AND READING IT CLEARS IT, so a quiet stretch reports quiet rather than the last total again.
	CHECK(gui.takeDriveReportForTest().paramFrames == 0);
}

TEST_CASE("the tick JOINS both roads to their widgets, and a deleted notify is not invisible") {
	// THE ONE LINE WHOSE REMOVAL WAS INVISIBLE TO THE WHOLE SUITE — twice over, because the editor
	// has two roads and neither join was pinned.
	//
	//   for (index : bridge.refresh()) registry.notify(index);   ← parameters, unpinned since it
	//                                                              was written
	//   if (takeRepaintDirty()) stateWatchers.notify();          ← the channels
	//
	// Delete either and every control stops responding to automation, or every surface stops
	// following the state — and 21 suites stay green, because the thing that is missing is a REPAINT
	// and visage's redraw() does nothing to a frame that is not being drawn. No headless venue can
	// watch one happen.
	//
	// SO THE NOTIFICATION REPORTS WHAT IT DELIVERED, and the tick adds those up. A count produced BY
	// the notification cannot be had without it — which is the difference between this and a counter
	// standing beside the call, where deleting the call leaves the counter happily counting.
	QuietGuiHost host;
	ParamBridge bridge;
	/// THE SAME BRIDGE AS THE DEVICE SEAM. Cards and panels are converging onto `backplane::DeviceHost`
	/// so the rack can build them; `BridgeDeviceHost` is what keeps Feathers handing them the parameter
	/// road underneath, and a fixture needs one for the same reason `Gui::Impl` does.
	feathers::BridgeDeviceHost deviceHost { bridge };
	bridge.refresh();
	Gui gui(host, bridge);
	REQUIRE(Gui::isAvailable());
	REQUIRE(gui.create(CLAP_WINDOW_API_X11, false));

	// A settled editor: whatever the build queued has been delivered, so what follows is this test's.
	for (int i = 0; i < 4; ++i) {
		gui.tickForTest();
	}
	const Gui::TickReport quiet = gui.tickForTest();
	CHECK(quiet.paramFrames == 0);
	CHECK(quiet.stateFrames == 0);

	// THE PARAMETER ROAD. Zone is watched by several surfaces, so a move must reach at least one.
	bridge.setValue(static_cast<clap_id>(ParamId::AZone), 400.0);
	const Gui::TickReport afterParam = gui.tickForTest();
	CHECK(afterParam.paramFrames > 0);
	CHECK(afterParam.stateFrames == 0); // a parameter is not a channel

	// THE CHANNEL ROAD. Five surfaces are subscribed, and one payload-free signal reaches all of them
	// — the fifth is the filter card, which joined ahead of its clap ids being swallowed into the
	// device row. An exact count rather than `> 0` on purpose: this is the number the notification
	// itself returns, so a surface that quietly stopped being subscribed shows up here as 4.
	bridge.setSoundHue(2, 4);
	const Gui::TickReport afterChannel = gui.tickForTest();
	CHECK(afterChannel.stateFrames == expectedStateFrames(gui));
	CHECK(afterChannel.paramFrames == 0); // a channel is not a parameter

	// AND A QUIET FRAME TELLS NOBODY, which is the other half of the contract: the tick is not an
	// unconditional redraw of everything, it is a delivery of what actually moved.
	const Gui::TickReport idle = gui.tickForTest();
	CHECK(idle.paramFrames == 0);
	CHECK(idle.stateFrames == 0);
}

TEST_CASE("the terrain lens, calibrated against waves everyone already knows") {
	// WHAT "SINEY" MEANS, MEASURED THROUGH THE ENGINE'S OWN REDUCTION rather than quoted from
	// anywhere. This case exists because a calibration table for exactly these landmarks arrived in
	// the WRONG UNITS — un-normalised, so every number was eleven times too big — and the threshold
	// derived from it would have called nine tenths of the terrain pure. The error was caught by
	// reading kTerrainHarmonics - 1 out of the reduction, and this is what makes it uncatchable
	// twice: the landmarks are now asserted, in the units the engine actually returns.
	//
	// The centroid is divided by kTerrainHarmonics - 1, so a sine is 0 and the twelfth harmonic
	// alone would be 1. A threshold stated in any other units is stated in the wrong ones.
	const auto features = [](auto&& shape) {
		std::array<float, kTerrainCycleSamples> cycle {};
		for (int i = 0; i < kTerrainCycleSamples; ++i) {
			const double phase = static_cast<double>(i) / static_cast<double>(kTerrainCycleSamples);
			cycle[static_cast<size_t>(i)] = static_cast<float>(shape(phase));
		}
		return terrainFeaturesOfCycle(cycle);
	};

	const TerrainFeatures sine = features([](double p) { return std::sin(2.0 * M_PI * p); });
	const TerrainFeatures triangle =
	    features([](double p) { return 4.0 * std::abs(p - std::floor(p + 0.5)) - 1.0; });
	const TerrainFeatures square = features([](double p) { return p < 0.5 ? 1.0 : -1.0; });
	const TerrainFeatures saw = features([](double p) { return 2.0 * p - 1.0; });

	std::printf("[terrain lens] centroid / roughness / oddShare\n"
	            "   sine     %.4f  %.4f  %.4f\n"
	            "   triangle %.4f  %.4f  %.4f\n"
	            "   square   %.4f  %.4f  %.4f\n"
	            "   saw      %.4f  %.4f  %.4f\n",
	            static_cast<double>(sine.centroid), static_cast<double>(sine.roughness),
	            static_cast<double>(sine.parity), static_cast<double>(triangle.centroid),
	            static_cast<double>(triangle.roughness), static_cast<double>(triangle.parity),
	            static_cast<double>(square.centroid), static_cast<double>(square.roughness),
	            static_cast<double>(square.parity), static_cast<double>(saw.centroid),
	            static_cast<double>(saw.roughness), static_cast<double>(saw.parity));

	// A PURE SINE IS THE ORIGIN OF THIS LENS. Everything else is measured against it.
	CHECK(sine.centroid == doctest::Approx(0.0).epsilon(1e-4));
	CHECK(sine.roughness == doctest::Approx(0.0).epsilon(1e-4));

	// THE ORDER IS THE WHOLE POINT: sine, then triangle, then square, then saw. If a threshold ever
	// stops sitting where it is meant to relative to these, this ordering is what says so.
	CHECK(sine.centroid < triangle.centroid);
	CHECK(triangle.centroid < square.centroid);
	CHECK(square.centroid < saw.centroid);
	CHECK(sine.roughness < triangle.roughness);
	CHECK(triangle.roughness < square.roughness);

	// AND THE ODD SHARE IS THE CONFOUND, which is why it is reported and never thresholded on.
	// MEASURED, and it is a sharper demonstration than the one this case was first written with: a
	// SINE, a TRIANGLE and a SQUARE all have an odd share of exactly 1. Three waves spanning the
	// entire range from the purest thing there is to very nearly the least pure, and the channel
	// cannot tell them apart at all. "Odd" means HOLLOW, not pure, and a criterion that rewarded it
	// would be selecting for something else and calling it simplicity.
	//
	// A first draft asserted square.parity > triangle.parity and failed, because a triangle is odd-
	// symmetric too. The failure was the finding.
	CHECK(sine.parity == doctest::Approx(1.0).epsilon(1e-3));
	CHECK(triangle.parity == doctest::Approx(1.0).epsilon(1e-3));
	CHECK(square.parity == doctest::Approx(1.0).epsilon(1e-3));
	// The sawtooth is the one with even harmonics, and it is the LEAST siney of the four — so a
	// lower odd share goes with a higher centroid here, which is the opposite of the direction a
	// simplicity criterion would want to read it in.
	CHECK(saw.parity < 0.7f);
	CHECK(saw.centroid > square.centroid);
	CHECK(square.centroid > triangle.centroid);
}

TEST_CASE("MEASURE: how much of the terrain is genuinely siney, and which field moves it") {
	// THE SWEEP THAT ANSWERS "there are not enough siney harmonic sounds" WITH WAVEFORMS rather than
	// with field values. Every earlier number in this arc was arithmetic on the four simplifying
	// fields — a PROXY — and the proxy had never met a cycle. This measures the sound.
	//
	// THE CRITERION, in the engine's own units and calibrated by the case above: siney = centroid
	// below 0.05 AND roughness below 0.03. Both bars sit BELOW A TRIANGLE, which measures 0.0599 and
	// 0.0432 — so this is not "triangle or purer", it is strictly purer than a triangle on both
	// axes. Worth stating plainly, because "just under a triangle" reads as including one.
	//
	// ODD SHARE IS REPORTED AND NEVER THRESHOLDED ON. Sine, triangle and square all measure exactly
	// 1, so it cannot separate pure from hollow — it is the confound that let an odd-symmetry field
	// into the conjunction in the first place.
	constexpr float kSineyCentroid = 0.05f;
	constexpr float kSineyRoughness = 0.03f;
	constexpr double kSpan = 40.0;
	constexpr int kSteps = 40000; // one sample per thousandth of a home unit
	const SliceOrientationPin kNoPin {};

	int siney = 0;
	int firstAt = -1;
	int runs = 0;
	int longestRun = 0;
	int currentRun = 0;
	double oddInSiney = 0.0;
	double oddOverall = 0.0;
	for (int i = 0; i < kSteps; ++i) {
		const double home = kSpan * static_cast<double>(i) / static_cast<double>(kSteps);
		// Home is zone/1023 + offset + gammaBase; the sweep walks it through the offset with zone and
		// gamma at rest, which is the axis a hand actually turns.
		const TerrainFeatures f =
		    terrainFeaturesAt(OscType::V5, kNoPin, 0, static_cast<float>(home), 0.0, 0.0);
		oddOverall += static_cast<double>(f.parity);
		const bool isSiney = f.centroid < kSineyCentroid && f.roughness < kSineyRoughness;
		if (isSiney) {
			++siney;
			oddInSiney += static_cast<double>(f.parity);
			if (firstAt < 0) {
				firstAt = i;
			}
			if (currentRun == 0) {
				++runs;
			}
			++currentRun;
			longestRun = std::max(longestRun, currentRun);
		}
		else {
			currentRun = 0;
		}
	}
	const double fraction = 100.0 * static_cast<double>(siney) / static_cast<double>(kSteps);
	const double unitsPerStep = kSpan / static_cast<double>(kSteps);
	std::printf("[siney sweep] V5 over home 0..%.0f, %d samples, zero travel and zero residue\n"
	            "   genuinely siney: %.3f%% of terrain (%d of %d)\n"
	            "   first at home %.4f\n"
	            "   %d separate runs, longest %.4f home units (%.1f zones)\n"
	            "   mean odd share: %.3f inside siney country, %.3f overall\n",
	            kSpan, kSteps, fraction,
	            siney, kSteps, firstAt < 0 ? -1.0 : static_cast<double>(firstAt) * unitsPerStep, runs,
	            static_cast<double>(longestRun) * unitsPerStep,
	            static_cast<double>(longestRun) * unitsPerStep * 1023.0,
	            siney > 0 ? oddInSiney / static_cast<double>(siney) : 0.0,
	            oddOverall / static_cast<double>(kSteps));

	// HOW MUCH THE ANSWER DEPENDS ON WHERE THE BAR SITS, because a single percentage is exactly what
	// has misled this arc twice already. Reported at four bars and for each channel alone, so the
	// conjunction's cost is visible: the two criteria are close to independent, so meeting BOTH is
	// far rarer than meeting either, which is most of why a field-value proxy said 7.6%.
	std::printf("   sensitivity (percent of terrain), V5 and Morph:\n");
	struct Bar {
		const char* name;
		float centroid;
		float roughness;
	};
	const Bar bars[] = {
		{ "strictly purer than a triangle (0.050/0.030)", 0.05f, 0.03f },
		{ "about a triangle              (0.060/0.045)", 0.060f, 0.045f },
		{ "halfway to a square           (0.140/0.120)", 0.140f, 0.120f },
		{ "anything below a square       (0.210/0.200)", 0.210f, 0.200f },
	};
	for (const OscType engine : { OscType::V5, OscType::Morph }) {
		for (const Bar& bar : bars) {
			int both = 0;
			int centroidOnly = 0;
			int roughOnly = 0;
			for (int i = 0; i < kSteps; ++i) {
				const double home = kSpan * static_cast<double>(i) / static_cast<double>(kSteps);
				const TerrainFeatures f =
				    terrainFeaturesAt(engine, kNoPin, 0, static_cast<float>(home), 0.0, 0.0);
				const bool c = f.centroid < bar.centroid;
				const bool r = f.roughness < bar.roughness;
				centroidOnly += c ? 1 : 0;
				roughOnly += r ? 1 : 0;
				both += (c && r) ? 1 : 0;
			}
			const double n = static_cast<double>(kSteps);
			std::printf("     %-6s %s  centroid %6.3f%%  rough %6.3f%%  BOTH %6.3f%%\n",
			            engine == OscType::V5 ? "V5" : "Morph", bar.name, 100.0 * centroidOnly / n,
			            100.0 * roughOnly / n, 100.0 * both / n);
		}
	}

	// STABLE CLAIMS ONLY. The exact percentage is a property of the terrain and will move whenever a
	// field is retuned; what must stay true is that the criterion selects a real MINORITY rather
	// than either nothing or most of the map — which is the shape of error that produced the two
	// wrong answers this arc has already had. A criterion calling nine tenths of terrain pure and a
	// criterion calling none of it pure are both useless, and both would pass a looser assertion.
	CHECK(fraction > 0.0);
	CHECK(fraction < 25.0);
	CHECK(firstAt >= 0);
}

TEST_CASE("MEASURE: which simplifying field actually moves the centroid") {
	// THE NUMBER THAT DECIDES WHETHER A SIMPLICITY FIELD SHOULD BIAS FOUR, TWO OR ONE. The four
	// fields are read at the same coordinate the character is built at, so each swept position
	// carries its own four values and its own measured centroid — and what they are worth can be
	// compared instead of argued.
	//
	// OBSERVATIONAL, NOT INTERVENTIONAL, and the difference is worth stating: there is no way to
	// hold one of these fixed and sweep the others without reaching into the character path, which
	// this arc is not allowed to touch. So this reports each field's MARGINAL association with a low
	// centroid — the mean centroid where the field is high against where it is low — which answers
	// "is it worth biasing" without pretending to be an experiment.
	constexpr double kSpan = 40.0;
	constexpr int kSteps = 20000;
	const SliceOrientationPin kNoPin {};

	struct Field {
		const char* name;
		size_t slot;
		owlet::dsp::phi::PhiTriConfig cfg;
		double lowSum = 0.0;
		int lowN = 0;
		double highSum = 0.0;
		int highN = 0;
	};
	Field fields[] = {
		{ "sineBlend ", morph_slot::kSineBlend, owlet::dsp::kPhiMorphSineBlendBank[0], 0, 0, 0, 0 },
		{ "oddSym    ", morph_slot::kOddSym, owlet::dsp::kPhiMorphOddSymBank[0], 0, 0, 0, 0 },
		{ "window    ", morph_slot::kWindow, owlet::dsp::kPhiMorphWindowBank[0], 0, 0, 0, 0 },
		{ "slopeMatch", morph_slot::kSlopeMatch, owlet::dsp::kPhiMorphSlopeMatchBank[0], 0, 0, 0, 0 },
	};

	for (int i = 0; i < kSteps; ++i) {
		const double home = kSpan * static_cast<double>(i) / static_cast<double>(kSteps);
		const TerrainFeatures f =
		    terrainFeaturesAt(OscType::V5, kNoPin, 0, static_cast<float>(home), 0.0, 0.0);
		const MorphBasis frame = sliceFrameFor<kMorphTriangles>(0.0, home, {});
		for (Field& field : fields) {
			const float value = owlet::dsp::phi::evalTriangle(frame.phaseFor(field.slot, home, 0.0, 0.0),
			                                                   frame.rateFor(field.slot, 0.0, 0.0),
			                                                   field.cfg);
			if (value > 0.5f) {
				field.highSum += static_cast<double>(f.centroid);
				++field.highN;
			}
			else {
				field.lowSum += static_cast<double>(f.centroid);
				++field.lowN;
			}
		}
	}

	std::printf("[siney fields] mean centroid when each field is high vs low, %d samples\n", kSteps);
	for (const Field& field : fields) {
		const double high = field.highN > 0 ? field.highSum / field.highN : 0.0;
		const double low = field.lowN > 0 ? field.lowSum / field.lowN : 0.0;
		std::printf("   %s  high %.4f (%5.1f%% of terrain)   low %.4f   effect %+.4f\n", field.name,
		            high, 100.0 * field.highN / static_cast<double>(kSteps), low, high - low);
	}

	// AND THE LIFT: of the positions that are GENUINELY SINEY, what fraction has each field high?
	// This is the question a bias would actually be answering — a field worth biasing is one the
	// siney places already agree about, and a field whose siney share matches its base rate carries
	// no information at all.
	int sineyTotal = 0;
	std::array<int, 4> sineyWithField {};
	for (int i = 0; i < kSteps; ++i) {
		const double home = kSpan * static_cast<double>(i) / static_cast<double>(kSteps);
		const TerrainFeatures f =
		    terrainFeaturesAt(OscType::V5, kNoPin, 0, static_cast<float>(home), 0.0, 0.0);
		// The looser of the calibrated bars, because the strict one yields too few positions here to
		// say anything about — which is itself the arc's headline.
		if (!(f.centroid < 0.060f && f.roughness < 0.045f)) {
			continue;
		}
		++sineyTotal;
		const MorphBasis frame = sliceFrameFor<kMorphTriangles>(0.0, home, {});
		for (size_t k = 0; k < 4; ++k) {
			const float value = owlet::dsp::phi::evalTriangle(
			    frame.phaseFor(fields[k].slot, home, 0.0, 0.0), frame.rateFor(fields[k].slot, 0.0, 0.0),
			    fields[k].cfg);
			sineyWithField[k] += value > 0.5f ? 1 : 0;
		}
	}
	std::printf("   of the %d siney positions (bar 0.060/0.045), how many have each field high:\n",
	            sineyTotal);
	for (size_t k = 0; k < 4; ++k) {
		const double share = sineyTotal > 0
		                         ? 100.0 * sineyWithField[k] / static_cast<double>(sineyTotal)
		                         : 0.0;
		const double base = 100.0 * fields[k].highN / static_cast<double>(kSteps);
		std::printf("     %s  %5.1f%% of siney vs %5.1f%% of terrain   lift %+.2fx\n", fields[k].name,
		            share, base, base > 0.0 ? share / base : 0.0);
	}

	// STABLE CLAIM: a field that simplifies must LOWER the centroid where it is high. slopeMatch is
	// the one with a mechanism for it — removing slope discontinuities takes harmonic decay from
	// 1/k^2 to 1/k^3, the largest single reduction available — so if any field earns a bias it is
	// that one, and this is the assertion that says so rather than the prose.
	const Field& slope = fields[3];
	CHECK(slope.highN > 0);
	CHECK(slope.lowN > 0);
	CHECK(slope.highSum / slope.highN < slope.lowSum / slope.lowN);
}

TEST_CASE("SHOT the filter card") {
	// NOT A CASE — A CAMERA, and the only windowless one this repo has. Gated on FEATHERS_SHOT_DIR so
	// it costs nothing in an ordinary run; set that to a directory and it writes one BGRA frame per
	// state, which any image tool will convert. It exists because the geometry cases, the text-fit
	// case and the whole suite passed a bell whose greyed box read "24 dB" over an engine running
	// 12, and no assertion in this file was ever going to see it.
	//
	// THE FRAMES MUST DIFFER, and here they did: three distinct hashes, with the lowpass one
	// UNCHANGED across the fix that moved the bell's — which is what says the camera is reading the
	// state rather than the clock.
	//
	// THE BYTES ARE R, G, B, A IN THAT ORDER — not BGRA, whatever the frame BUFFER holds. The vendored
	// tools/raw-image-check.cpp settles it: it reads a screenshot back as (pixel[0] << 16) | (pixel[1]
	// << 8) | pixel[2] and compares that to 0xff0000 for an image it drew red, so byte zero is RED.
	// Getting this wrong costs nothing in geometry and everything in colour — the first conversion of
	// these frames treated them as BGRA and rendered the editor's dark blue ground as warm brown,
	// which is a confidently wrong answer to every question a picture is usually asked. The case
	// below pins the order so the camera cannot silently invert.
	const char* dir = std::getenv("FEATHERS_SHOT_DIR");
	if (dir == nullptr) {
		return;
	}
	FilterFixture f;
	auto* editor = dynamic_cast<visage::ApplicationEditor*>(f.root);
	REQUIRE(editor != nullptr);
	const int w = static_cast<int>(f.root->width());
	const int h = static_cast<int>(f.root->height());
	// The renderer is asked AFTER the canvas is made windowless, which is the order the vendored
	// tools use: supported() answers about an initialised renderer, and asking first reads empty.
	editor->setWindowless(w, h);
	if (!visage::Renderer::instance().supported()) {
		MESSAGE("SKIP no renderer: " << visage::Renderer::instance().errorMessage());
		return;
	}

	const auto shoot = [&](const char* name) {
		f.settle();
		f.root->redrawAll();
		editor->drawWindow();
		editor->drawWindow();
		editor->drawWindow();
		const visage::Screenshot& shot = editor->takeScreenshot();
		MESSAGE(name << " shot " << shot.width() << "x" << shot.height());
		if (shot.width() == 0) {
			return;
		}
		const std::string path = std::string(dir) + "/" + name + ".raw";
		std::ofstream out(path, std::ios::binary);
		out.write(reinterpret_cast<const char*>(shot.data()),
		          static_cast<std::streamsize>(shot.width()) * shot.height() * 4);
		MESSAGE("  wrote " << path);
		const visage::Point at = f.card->positionInWindow();
		MESSAGE("  card at " << at.x << "," << at.y << " size " << f.card->width() << "x"
		                     << f.card->height() << " canvas " << shot.width() << "x" << shot.height());
	};

	// THROUGH THE ROW, then published, because that is the road the card draws from. Writing the
	// parameter table here would have configured nothing and photographed a card at its defaults.
	const auto set = [&](fx::filter::Param p, double value) {
		f.set(p, value);
		f.bridge.publish(f.params);
		f.bridge.refresh();
	};

	set(fx::filter::Param::Enabled, 1.0);
	set(fx::filter::Param::Cutoff, std::log2(900.0));
	set(fx::filter::Param::Resonance, 0.55);
	set(fx::filter::Param::Spread, 0.35);
	set(fx::filter::Param::Macro2, -0.4);
	set(fx::filter::Param::Slope, static_cast<double>(fx::filter::Slope::db24));
	set(fx::filter::Param::Type, static_cast<double>(fx::filter::Type::Lowpass));
	set(fx::filter::Param::Keytrack, 1.0);
	shoot("card_lowpass_24");

	set(fx::filter::Param::Type, static_cast<double>(fx::filter::Type::Bell));
	set(fx::filter::Param::Macro1, 0.5);
	shoot("card_bell_greyed");

	set(fx::filter::Param::Type, static_cast<double>(fx::filter::Type::Lowpass));
	f.card->setFolded(true);
	f.lane->resized(); // the LANE owns the card's height; setFolded alone leaves it full size
	shoot("card_folded");
}

TEST_CASE("MEASURE: what sineBlend and slopeMatch reach together, and whether they are one mechanism") {
	// THE NUMBER A DECISION TURNS ON, measured through the same instrument the four known waves
	// calibrated — terrainFeaturesAt by way of terrainFeaturesOfCycle — rather than through a model
	// of the segment maths. The model in question restated the firmware with UNIFORM-RANDOM waypoint
	// phases instead of the real phi distribution and put the joint result at 0.0455 against a
	// triangle's 0.052: just inside pure, and close enough that the real distribution could put it
	// either side. Which side it falls on is the whole decision.
	//
	// OBSERVATIONAL, AND DELIBERATELY SO. The four fields are locals inside buildMorphSliced,
	// consumed where they are computed; no lower entry takes them as arguments, so forcing one to 1
	// means editing the character path. That is surgery this arc may not do — and it turned out not
	// to be needed, because the terrain offers essentially the corner on its own.
	//
	// BOTH ENGINES, THROUGH ONE PASS AND ONE METHOD. V5 is the DEFAULT engine, so most patches are
	// the V5 population and a Morph-only figure describes an engine most sounds are not on.
	// But the operator tier is not a small perturbation: quantize alone has a duty of 0.85, so most
	// of V5 carries some staircase, which is a roughening by construction. Whether the enrichment
	// survives that is the question — and the two engines have to be measured the same way or the
	// comparison is between methods rather than between engines.
	constexpr double kSpan = 400.0; // ten times the earlier sweep: joint extremes are rare
	constexpr int kSteps = 200000;
	const SliceOrientationPin kNoPin {};
	const auto sineCfg = owlet::dsp::kPhiMorphSineBlendBank[0];
	const auto slopeCfg = owlet::dsp::kPhiMorphSlopeMatchBank[0];

	struct Cell {
		double sum = 0.0;
		double rough = 0.0;
		int n = 0;
		float best = 1.0f;
		int sineyStrict = 0;
		int sineyTriangle = 0;
	};

	for (const OscType engine : { OscType::Morph, OscType::V5 }) {
		Cell cells[4];
		int allSineyStrict = 0;
		float bestJoint = 0.0f;
		double bestJointCentroid = 0.0;
		double bestJointRough = 0.0;
		float bestJointSine = 0.0f;
		float bestJointSlope = 0.0f;

		for (int i = 0; i < kSteps; ++i) {
			const double home = kSpan * static_cast<double>(i) / static_cast<double>(kSteps);
			const MorphBasis frame = sliceFrameFor<kMorphTriangles>(0.0, home, {});
			const float sine = owlet::dsp::phi::evalTriangle(
			    frame.phaseFor(morph_slot::kSineBlend, home, 0.0, 0.0),
			    frame.rateFor(morph_slot::kSineBlend, 0.0, 0.0), sineCfg);
			const float slope = owlet::dsp::phi::evalTriangle(
			    frame.phaseFor(morph_slot::kSlopeMatch, home, 0.0, 0.0),
			    frame.rateFor(morph_slot::kSlopeMatch, 0.0, 0.0), slopeCfg);
			const TerrainFeatures f =
			    terrainFeaturesAt(engine, kNoPin, 0, static_cast<float>(home), 0.0, 0.0);
			const bool strict = f.centroid < 0.05f && f.roughness < 0.03f;
			const bool triangleBar = f.centroid < 0.060f && f.roughness < 0.045f;
			allSineyStrict += strict ? 1 : 0;

			Cell& cell = cells[(sine > 0.5f ? 1u : 0u) | (slope > 0.5f ? 2u : 0u)];
			cell.sum += static_cast<double>(f.centroid);
			cell.rough += static_cast<double>(f.roughness);
			++cell.n;
			cell.best = std::min(cell.best, f.centroid);
			cell.sineyStrict += strict ? 1 : 0;
			cell.sineyTriangle += triangleBar ? 1 : 0;

			const float joint = std::min(sine, slope);
			if (joint > bestJoint) {
				bestJoint = joint;
				bestJointCentroid = static_cast<double>(f.centroid);
				bestJointRough = static_cast<double>(f.roughness);
				bestJointSine = sine;
				bestJointSlope = slope;
			}
		}

		const char* names[4] = { "both low   ", "sineBlend  ", "slopeMatch ", "BOTH high  " };
		std::printf("[siney joint] %s, home 0..%.0f, %d samples\n",
		            engine == OscType::V5 ? "V5   " : "Morph", kSpan, kSteps);
		for (size_t k = 0; k < 4; ++k) {
			std::printf("   %s  n=%6d (%5.1f%%)  centroid %.4f  rough %.4f  lowest %.4f\n", names[k],
			            cells[k].n, 100.0 * cells[k].n / static_cast<double>(kSteps),
			            cells[k].n > 0 ? cells[k].sum / cells[k].n : 0.0,
			            cells[k].n > 0 ? cells[k].rough / cells[k].n : 0.0,
			            static_cast<double>(cells[k].best));
		}
		std::printf("   highest joint pair offered: sineBlend %.3f, slopeMatch %.3f"
		            " -> centroid %.4f, rough %.4f\n",
		            static_cast<double>(bestJointSine), static_cast<double>(bestJointSlope),
		            bestJointCentroid, bestJointRough);

		const double base = static_cast<double>(allSineyStrict) / kSteps;
		const double inCell = cells[3].n > 0
		                          ? static_cast<double>(cells[3].sineyStrict) / cells[3].n
		                          : 0.0;
		std::printf("   if BOTH biased high: siney %.2f%% of that country vs %.3f%% overall"
		            "  -> %.1fx enrichment  (triangle bar %.2f%%)\n",
		            100.0 * inCell, 100.0 * base, base > 0.0 ? inCell / base : 0.0,
		            cells[3].n > 0 ? 100.0 * cells[3].sineyTriangle / cells[3].n : 0.0);

		const double bothLow = cells[0].sum / cells[0].n;
		const double sineOnly = cells[1].sum / cells[1].n;
		const double slopeOnly = cells[2].sum / cells[2].n;
		const double bothHigh = cells[3].sum / cells[3].n;
		// AND FOR V5, THE QUESTION THE V5 NUMBERS RAISE: is it the OPERATORS eating the benefit? At
		// the same position where Morph reaches the corner, V5 is three times worse — and quantize
		// alone has a duty of 0.85, so most of V5 carries a staircase, which roughens exactly what
		// these two fields smooth. So: inside both-high character country, split by whether the
		// operator tier is also at rest, and see whether V5 comes back to Morph's answer.
		if (engine == OscType::V5) {
			double quietSum = 0.0;
			int quietN = 0;
			int quietSiney = 0;
			double loudSum = 0.0;
			int loudN = 0;
			for (int i = 0; i < kSteps; ++i) {
				const double home = kSpan * static_cast<double>(i) / static_cast<double>(kSteps);
				const MorphBasis frame = sliceFrameFor<kMorphTriangles>(0.0, home, {});
				const float sine = owlet::dsp::phi::evalTriangle(
				    frame.phaseFor(morph_slot::kSineBlend, home, 0.0, 0.0),
				    frame.rateFor(morph_slot::kSineBlend, 0.0, 0.0), sineCfg);
				const float slope = owlet::dsp::phi::evalTriangle(
				    frame.phaseFor(morph_slot::kSlopeMatch, home, 0.0, 0.0),
				    frame.rateFor(morph_slot::kSlopeMatch, 0.0, 0.0), slopeCfg);
				if (!(sine > 0.5f && slope > 0.5f)) {
					continue;
				}
				const FlagshipFields ops =
				    flagshipFieldsAt(flagshipLeadCoordinate(frame, home, 0.0, 0.0));
				const TerrainFeatures f =
				    terrainFeaturesAt(engine, kNoPin, 0, static_cast<float>(home), 0.0, 0.0);
				if (ops.quantize == 0.0f && ops.sync == 0.0f && ops.feedback == 0.0f) {
					quietSum += static_cast<double>(f.centroid);
					++quietN;
					quietSiney += (f.centroid < 0.05f && f.roughness < 0.03f) ? 1 : 0;
				}
				else {
					loudSum += static_cast<double>(f.centroid);
					++loudN;
				}
			}
			std::printf("   V5 inside both-high country, split by the operator tier:\n"
			            "     operators AT REST  n=%5d  centroid %.4f  siney %.2f%%\n"
			            "     operators awake    n=%5d  centroid %.4f\n",
			            quietN, quietN > 0 ? quietSum / quietN : 0.0,
			            quietN > 0 ? 100.0 * quietSiney / quietN : 0.0, loudN,
			            loudN > 0 ? loudSum / loudN : 0.0);
		}

		const double singles = (sineOnly - bothLow) + (slopeOnly - bothLow);
		std::printf("   interaction: singles sum %+.4f, joint %+.4f -> %.2fx the sum\n\n", singles,
		            bothHigh - bothLow, (bothHigh - bothLow) / singles);

		// STABLE CLAIMS. The exact means move whenever a field is retuned; the ORDERING is the
		// finding and must not. If the two fields were independent contributions, both-high would
		// sit near the sum of the singles' effects; if they are one mechanism, it sits past it.
		CAPTURE(engine == OscType::V5);
		REQUIRE(cells[3].n > 0);
		CHECK(bothHigh < sineOnly);
		CHECK(bothHigh < slopeOnly);
		CHECK(sineOnly < bothLow);
		CHECK(slopeOnly < bothLow);
		// And the mechanism claim itself: the joint effect exceeds the sum of the singles.
		CHECK((bothHigh - bothLow) < singles);
	}
}

TEST_CASE("MEASURE: truncation is not the mechanism — the INTERPOLANT is") {
	// THE CORRECTION, TESTED. Waypoint truncation should not roughen a waveform, and a model saying
	// otherwise was thinking of it as PWM — because the specified operation is truncate AND
	// renormalise the time axis, with the wrap handled. The modelled 0.180 for truncate-and-
	// renormalise KEPT THE PHI-AUTHORED CURVATURES on the survivors, and truncation does nothing
	// whatever to what the wave does BETWEEN its control points. So the number was measuring "three
	// points with phi bulges between them", which nobody would call simplification.
	//
	// THE STRUCTURAL FACT FIRST, because it decides what can even be built. A morph segment is
	//     value = startAmp + ampDelta*frac + curvature * 4*frac*(1-frac)
	// — a line plus a SYMMETRIC quadratic bump vanishing at both ends. Differentiate it: the entry
	// slope is ampDelta + 4*curvature and the exit slope is ampDelta - 4*curvature, so THEIR SUM IS
	// 2*ampDelta WHATEVER THE CURVATURE IS. One free parameter, and the endpoints fix the sum. A
	// Catmull-Rom is a cubic with entry and exit slopes independently settable, so IT IS NOT
	// EXPRESSIBLE IN THIS SEGMENT FORMAT AT ALL — the proposal needs a new segment type or a new
	// evaluator, not a different curvature value. That is the same one-parameter-two-conditions
	// argument that explains why slopeMatch averages rather than achieving C1, and it is why this
	// case samples the interpolant directly rather than trying to build a wavetable for it.
	constexpr int kPositions = 400;
	constexpr double kSpan = 400.0;

	// Sample a wrapped Catmull-Rom through the control points. WRAPPED, so the seam joins by
	// construction rather than by a fix bolted on afterwards, which is the half the specification calls out.
	const auto catmullValue = [](const std::vector<float>& phases, const std::vector<float>& amps,
	                             double t) {
		const int n = static_cast<int>(phases.size());
		int k = n - 1;
		for (int j = 0; j < n; ++j) {
			const double next = (j + 1 < n) ? phases[static_cast<size_t>(j + 1)] : phases[0] + 1.0;
			if (t < next) {
				k = j;
				break;
			}
		}
		const auto at = [&](int idx) {
			const int m = ((idx % n) + n) % n;
			return static_cast<double>(amps[static_cast<size_t>(m)]);
		};
		const double p0 = static_cast<double>(phases[static_cast<size_t>(k)]);
		const double p1 = (k + 1 < n) ? static_cast<double>(phases[static_cast<size_t>(k + 1)])
		                              : static_cast<double>(phases[0]) + 1.0;
		double local = t - p0;
		if (local < 0.0) {
			local += 1.0;
		}
		const double u = std::clamp(local / std::max(1e-6, p1 - p0), 0.0, 1.0);
		const double a = at(k - 1);
		const double b = at(k);
		const double c = at(k + 1);
		const double d = at(k + 2);
		return static_cast<float>(0.5
		                          * ((2.0 * b) + (-a + c) * u + (2.0 * a - 5.0 * b + 4.0 * c - d) * u * u
		                             + (-a + 3.0 * b - 3.0 * c + d) * u * u * u));
	};

	const auto catmullCycle = [&catmullValue](const std::vector<float>& phases,
	                                          const std::vector<float>& amps) {
		std::array<float, kTerrainCycleSamples> cycle {};
		for (int i = 0; i < kTerrainCycleSamples; ++i) {
			cycle[static_cast<size_t>(i)] = catmullValue(
			    phases, amps, static_cast<double>(i) / static_cast<double>(kTerrainCycleSamples));
		}
		return cycle;
	};

	// The SAME control points through the engine's own segment rule, which is the control that
	// isolates the interpolant: one line, restated from evalMorphTable above.
	const auto phiCycle = [](const std::vector<float>& phases, const std::vector<float>& amps,
	                         const std::vector<float>& curvatures) {
		std::array<float, kTerrainCycleSamples> cycle {};
		const int n = static_cast<int>(phases.size());
		for (int i = 0; i < kTerrainCycleSamples; ++i) {
			const double t = static_cast<double>(i) / static_cast<double>(kTerrainCycleSamples);
			int k = n - 1;
			for (int j = 0; j < n; ++j) {
				const double next = (j + 1 < n) ? phases[static_cast<size_t>(j + 1)]
				                                : phases[0] + 1.0;
				if (t < next) {
					k = j;
					break;
				}
			}
			const double p0 = static_cast<double>(phases[static_cast<size_t>(k)]);
			const double p1 = (k + 1 < n) ? static_cast<double>(phases[static_cast<size_t>(k + 1)])
			                              : static_cast<double>(phases[0]) + 1.0;
			double local = t - p0;
			if (local < 0.0) {
				local += 1.0;
			}
			const double u = std::clamp(local / std::max(1e-6, p1 - p0), 0.0, 1.0);
			const double b = static_cast<double>(amps[static_cast<size_t>(k)]);
			const double c = static_cast<double>(amps[static_cast<size_t>((k + 1) % n)]);
			const double curve = static_cast<double>(curvatures[static_cast<size_t>(k)]);
			cycle[static_cast<size_t>(i)] =
			    static_cast<float>(b + (c - b) * u + curve * u * (1.0 - u) * 4.0);
		}
		return cycle;
	};

	std::printf("[truncation] %d positions, home 0..%.0f. A triangle is 0.0599 / 0.0432.\n",
	            kPositions, kSpan);
	for (const int K : { 2, 3, 4, 5, 8 }) {
		double catCentroid = 0.0;
		double catRough = 0.0;
		double phiCentroid = 0.0;
		double phiRough = 0.0;
		int catSiney = 0;
		int n = 0;
		for (int i = 0; i < kPositions; ++i) {
			const double home = kSpan * static_cast<double>(i) / static_cast<double>(kPositions);
			const owlet::dsp::PhiMorphWavetable table = buildMorphSliced(
			    0, static_cast<float>(home), 0.0, 0.0, sliceFrameFor<kMorphTriangles>(0.0, home, {}),
			    0.0);
			// The control points this position actually authored: the endpoint plus its movable
			// waypoints, in order, stopping where the builder's own truncation stopped.
			std::vector<float> phases { 0.0f };
			std::vector<float> amps { table.endpointAmp };
			for (int w = 0; w < owlet::dsp::kPhiMorphMovableWaypoints; ++w) {
				if (w > 0 && !(table.phases[w] > table.phases[w - 1])) {
					break;
				}
				phases.push_back(table.phases[w]);
				amps.push_back(table.amplitudes[w]);
			}
			if (static_cast<int>(phases.size()) < K) {
				continue;
			}
			// TRUNCATE to K, then RENORMALISE the phases across the full cycle — the specified operation,
			// both halves. Even spacing, because the surviving points no longer describe the
			// original spans and keeping them would be truncation without the renormalise.
			std::vector<float> tp(phases.begin(), phases.begin() + K);
			std::vector<float> ta(amps.begin(), amps.begin() + K);
			for (int j = 0; j < K; ++j) {
				tp[static_cast<size_t>(j)] = static_cast<float>(j) / static_cast<float>(K);
			}
			std::vector<float> tc(static_cast<size_t>(K), 0.0f);
			for (int j = 0; j < K && j < owlet::dsp::kPhiMorphMaxSegments; ++j) {
				tc[static_cast<size_t>(j)] = table.curvatures[j];
			}
			const TerrainFeatures cat = terrainFeaturesOfCycle(catmullCycle(tp, ta));
			const TerrainFeatures phi = terrainFeaturesOfCycle(phiCycle(tp, ta, tc));
			catCentroid += static_cast<double>(cat.centroid);
			catRough += static_cast<double>(cat.roughness);
			phiCentroid += static_cast<double>(phi.centroid);
			phiRough += static_cast<double>(phi.roughness);
			catSiney += (cat.centroid < 0.05f && cat.roughness < 0.03f) ? 1 : 0;
			++n;
		}
		REQUIRE(n > 0);
		std::printf("   K=%d (n=%3d)  CATMULL centroid %.4f rough %.4f  siney %5.1f%%   |"
		            "  phi-curvature centroid %.4f rough %.4f\n",
		            K, n, catCentroid / n, catRough / n, 100.0 * catSiney / n, phiCentroid / n,
		            phiRough / n);
	}

	// AND THE QUESTION THAT DECIDES THE ARC: does it survive the OPERATORS? Today's other finding was
	// that the operator tier masks everything the character fields buy — V5 with the operators at
	// rest IS Morph, and awake it adds about 0.07 to the centroid. Field-biasing died on that. A
	// smooth interpolant starts from so much further down that it may not.
	//
	// PUSHED THROUGH THE REAL OPERATOR CHAIN, not a model of it, using a seam that already exists:
	// FlagshipSource takes a cycle and a presence, and at presence 1.0 the source IS that cycle. So
	// the operators bake on the interpolated wave exactly as they would on any other, which is what
	// V22c's "one decode" means. No new machinery, and nothing in the character path touched.
	{
		double bareSum = 0.0;
		double operatedSum = 0.0;
		double operatedRough = 0.0;
		int operatedSiney = 0;
		int awake = 0;
		int n = 0;
		for (int i = 0; i < kPositions; ++i) {
			const double home = kSpan * static_cast<double>(i) / static_cast<double>(kPositions);
			const MorphBasis frame = sliceFrameFor<kMorphTriangles>(0.0, home, {});
			const owlet::dsp::PhiMorphWavetable table =
			    buildMorphSliced(0, static_cast<float>(home), 0.0, 0.0, frame, 0.0);
			std::vector<float> phases { 0.0f };
			std::vector<float> amps { table.endpointAmp };
			for (int w = 0; w < owlet::dsp::kPhiMorphMovableWaypoints; ++w) {
				if (w > 0 && !(table.phases[w] > table.phases[w - 1])) {
					break;
				}
				phases.push_back(table.phases[w]);
				amps.push_back(table.amplitudes[w]);
			}
			if (phases.size() < 3) {
				continue;
			}
			std::vector<float> tp(phases.begin(), phases.begin() + 3);
			std::vector<float> ta(amps.begin(), amps.begin() + 3);
			for (int j = 0; j < 3; ++j) {
				tp[static_cast<size_t>(j)] = static_cast<float>(j) / 3.0f;
			}
			// The same interpolated wave at the vox grid's resolution, so the source is the cycle
			// rather than a resampling of a resampling.
			VoxCycle carrier;
			carrier.voiced = true;
			for (int slot = 0; slot <= kVoxSlots; ++slot) {
				const int at = slot == kVoxSlots ? 0 : slot;
				carrier.slots[static_cast<size_t>(slot)] =
				    catmullValue(tp, ta, static_cast<double>(at) / static_cast<double>(kVoxSlots));
			}
			FlagshipRecipe recipe = buildFlagshipSliced(0, static_cast<float>(home), 0.0, 0.0, frame,
			                                            sliceFrameFor<kStairTriangles>(0.0, home, {}),
			                                            0.0);
			const FlagshipSource source { &carrier, 1.0f };
			prepareFlagshipRecipe(recipe, source);
			std::array<float, kFlagshipSlots + 1> grid {};
			bakeFlagshipGridAtMip(recipe, source, 0, grid.data());
			std::array<float, kTerrainCycleSamples> out {};
			for (int j = 0; j < kTerrainCycleSamples; ++j) {
				out[static_cast<size_t>(j)] =
				    grid[static_cast<size_t>(j * kFlagshipSlots / kTerrainCycleSamples)];
			}
			const TerrainFeatures operated = terrainFeaturesOfCycle(out);
			const TerrainFeatures bare = terrainFeaturesOfCycle(catmullCycle(tp, ta));
			bareSum += static_cast<double>(bare.centroid);
			operatedSum += static_cast<double>(operated.centroid);
			operatedRough += static_cast<double>(operated.roughness);
			operatedSiney += (operated.centroid < 0.05f && operated.roughness < 0.03f) ? 1 : 0;
			awake += recipe.operated ? 1 : 0;
			++n;
		}
		REQUIRE(n > 0);
		std::printf("   K=3 through the REAL operator chain (%d positions, %d with operators awake):\n"
		            "     bare %.4f  ->  operated centroid %.4f rough %.4f   siney %5.1f%%\n",
		            n, awake, bareSum / n, operatedSum / n, operatedRough / n,
		            100.0 * operatedSiney / n);
	}

	CHECK(true);
}

//----------------------------------------------------------------------------------------------//
// THE ASSIGN GLOW ARRIVES WITH THE ARM, NOT ONE EDIT LATER                                     //
//----------------------------------------------------------------------------------------------//

TEST_CASE("a widget re-handed the same watch list is STILL on it, which is where the assign glow went") {
	// THE DEFECT: an effect's assign glow did not appear right away when mod was mapped to an effect
	// param that should have been glowing. Right in content,
	// never delivered: the knobs were off `ModState`'s watcher list, so arming repainted everything
	// else and not them, and the glow appeared on the first later repaint the lane got for its own
	// reasons — which is what completing a mapping does through `DeviceControl::setAmount`.
	//
	// THE MECHANISM, HERE RATHER THAN AT THE KNOB, BECAUSE IT IS NOT ABOUT KNOBS. `watch_ = list.watch(f)`
	// evaluates the RIGHT side first: `watch()` finds `f` already on the list, takes its duplicate
	// branch and returns a token WITHOUT pushing, so the list still holds one entry. The move-assignment
	// then released the OLD token — same frame — and took that one entry off. **A second subscribe
	// unsubscribed.**
	//
	// ASSERTED AS THE SUBSCRIPTION AND AS THE COUNT NOTIFY REPORTS, not as pixels: a repaint is
	// unobservable headlessly, so the two halves are "is the wire there" and "does pulling it reach
	// something".
	ui::FrameWatchers list;
	visage::Frame frame;

	ui::Watch token = list.watch(&frame);
	REQUIRE(list.watches(&frame));
	REQUIRE(list.count() == 1);
	REQUIRE(list.notify() == 1);

	// THE SECOND SUBSCRIBE — the one `Knob::setModulation` makes every time a card is told twice.
	token = list.watch(&frame);
	CHECK(list.watches(&frame));
	// AND EXACTLY ONE ENTRY, so the fix cannot be "push again and leave a duplicate": a frame on the
	// list twice would be redrawn twice and would need unwatching twice.
	CHECK(list.count() == 1);
	// THE DELIVERY, PRODUCED BY THE NOTIFICATION rather than by counting entries — the same reason
	// `notify()` returns anything at all.
	CHECK(list.notify() == 1);

	// A THIRD, because the failing version was not idempotent either: it left the frame off, and the
	// call after that put it back on and then took it off again.
	token = list.watch(&frame);
	CHECK(list.watches(&frame));
	CHECK(list.notify() == 1);

	// AND RELEASING STILL ENDS IT. The guard must not turn the token into a subscription that cannot
	// be given up, which is the way a fix for this could quietly reintroduce the dangling entry the
	// token exists to prevent.
	token.release();
	CHECK_FALSE(list.watches(&frame));
	CHECK(list.count() == 0);
	CHECK(list.notify() == 0);
}

TEST_CASE("a token re-pointed at the same frame on a DIFFERENT list comes off the first one") {
	// THE HALF THE SAME-FRAME GUARD COULD EAT. Skipping the release when the frames match is only
	// right while the LIST also matches — otherwise the old list keeps a raw pointer nobody will ever
	// remove, which is the dangling read `Watch` was written to end (five owners forgot, a sixth did
	// not, and was hit it as a crash on the arrow).
	ui::FrameWatchers first;
	ui::FrameWatchers second;
	visage::Frame frame;

	ui::Watch token = first.watch(&frame);
	REQUIRE(first.watches(&frame));

	token = second.watch(&frame);
	CHECK_FALSE(first.watches(&frame));
	CHECK(first.count() == 0);
	CHECK(second.watches(&frame));
	CHECK(second.count() == 1);
}

TEST_CASE("arming reaches the fx lane's knobs, so the glow is there before anything is routed") {
	// THE SAME CLAIM IN THE ASSEMBLED EDITOR, which is where the two-calls-per-card condition actually
	// arises: `FxLane::setModulation` tells the cards that already exist, and `ensureCard` tells each
	// card again as it is built. A tray or panel knob is told once and was never affected — which is
	// why it was seen this on EFFECT knobs and nowhere else, and why a count over the whole editor would
	// have looked healthy.
	ModFixture f;
	auto* lane = findInTree<ui::FxLane>(f.root);
	REQUIRE(lane != nullptr);
	lane->setCollapsed(false);
	f.gui.tickForTest();

	ui::ModState* mod = f.gui.modForTest();
	REQUIRE(mod != nullptr);

	std::vector<ui::Knob*> knobs;
	collectKnobs(lane, &knobs);
	// NOT VACUOUS: the lane really does carry knobs, so the loop below is asking about something.
	REQUIRE(knobs.size() >= 4);

	for (ui::Knob* knob : knobs) {
		INFO("knob at " << knob->x() << "," << knob->y());
		CHECK(mod->watches(knob));
	}

	// AND THE WIRE IS PULLED, ATTRIBUTABLY. `arm()` returns what `FrameWatchers::notify` returned —
	// a number produced BY the delivery — and the DIFFERENCE across taking the lane's knobs off the
	// list is exactly what those knobs contribute. A bare "told more than zero" would have passed on
	// the broken build, because 87 other knobs were still subscribed.
	const size_t withKnobs = mod->arm(true);
	REQUIRE(mod->arm(false) > 0);
	for (ui::Knob* knob : knobs) {
		mod->watchers().unwatch(knob);
	}
	const size_t withoutKnobs = mod->arm(true);
	CHECK(withKnobs > withoutKnobs);
	// `notify()` redraws each watcher's whole SUBTREE, so a knob is worth at least one frame and may
	// be worth more. At least one apiece is the floor that cannot be met by accident.
	CHECK(withKnobs - withoutKnobs >= knobs.size());
}

TEST_CASE("arming a source that is already armed tells nobody") {
	// THE TWIN, AND IT IS WHAT STOPS THE FIX BEING "REPAINT ALWAYS". A card that asked for a frame on
	// every tick would pass the case above and cost a repaint per block — the cost `publishModulated`
	// refuses the state channel to avoid. `ModState::arm` reports zero for a flag that did not move,
	// so the two cases together say the repaint follows the STATE rather than the clock.
	ModFixture f;
	auto* lane = findInTree<ui::FxLane>(f.root);
	REQUIRE(lane != nullptr);
	lane->setCollapsed(false);
	f.gui.tickForTest();

	ui::ModState* mod = f.gui.modForTest();
	REQUIRE(mod != nullptr);

	REQUIRE(mod->arm(true) > 0);
	CHECK(mod->arm(true) == 0);
	REQUIRE(mod->arm(false) > 0);
	CHECK(mod->arm(false) == 0);
}

TEST_CASE("EVERY device the browser offers glows for arm once it is in the lane") {
	// **THE SWEEP THE EXISTING ARM CASE IS NOT.** That one collects the knobs the DEFAULT chain
	// happens to hold, which is a fixed handful — so a device added to the registry later is outside
	// its subject set entirely, and ships with a dark card while the case stays green.
	//
	// **BUS UTILITY DID EXACTLY THAT** — it had no glow. Its
	// `setModulation` forwarded to the card's CONTROLS and not to its KNOBS — enough to read and write
	// a modulated value, not enough to be seen, because the arm highlight is drawn by the widget. One
	// missing line, invisible to every other guard: the reachability table was satisfied (the controls
	// are reached), the modulation route worked end to end (the value moves), and the card drew
	// perfectly. Only the GLOW was absent, and nothing asked about it.
	//
	// So this asks about it for every device that can reach the lane, which is what makes the next
	// card's version of this line impossible to omit quietly.
	ModFixture f;
	auto* lane = findInTree<ui::FxLane>(f.root);
	REQUIRE(lane != nullptr);
	lane->setCollapsed(false);
	f.gui.tickForTest();

	ui::ModState* mod = f.gui.modForTest();
	REQUIRE(mod != nullptr);

	const std::vector<const feathers::device::Descriptor*> offered = lane->addableForTest();
	REQUIRE(offered.size() >= 4);

	size_t devicesChecked = 0;
	size_t knobsChecked = 0;
	for (const feathers::device::Descriptor* desc : offered) {
		// A tag is a NUL-terminated literal of ANY length up to kTagBytes ("eq" is three bytes with its NUL),
		// so a fixed four-byte read runs past a short one: ASan's global-buffer-overflow, 2026-09-26.
		const std::string tag(desc->traits.tag);
		CAPTURE(tag);
		// A FIXTURE PER DEVICE: a lane's layout is a function of what is in it, and a card built into
		// a crowded lane may be scrolled out of view and never laid out.
		ModFixture each;
		auto* eachLane = findInTree<ui::FxLane>(each.root);
		REQUIRE(eachLane != nullptr);
		eachLane->setCollapsed(false);
		const size_t slot = each.bridge.addFxDevice(desc->traits.tag);
		REQUIRE(slot < feathers::kMaxFxSlots);
		eachLane->resized();
		eachLane->scrollCardIntoView(slot);
		eachLane->resized();
		each.gui.tickForTest();

		ui::ModState* eachMod = each.gui.modForTest();
		REQUIRE(eachMod != nullptr);
		ui::FxCard* card = eachLane->cardAt(slot);
		REQUIRE(card != nullptr);

		std::vector<ui::Knob*> knobs;
		collectKnobs(card, &knobs);
		for (ui::Knob* knob : knobs) {
			INFO("device " << tag << " knob at " << knob->x() << "," << knob->y());
			// **THE SUBSCRIPTION IS THE GLOW.** A knob the panel does not watch is one it cannot tell
			// to light, so this is the state the highlight is drawn from rather than a proxy for it.
			CHECK(eachMod->watches(knob));
			++knobsChecked;
		}
		++devicesChecked;
	}

	// NOT VACUOUS, TWO WAYS: every offered device was visited, and between them they really did build
	// knobs — a card that built none would satisfy the loop above by never entering it.
	CHECK(devicesChecked == offered.size());
	CHECK(knobsChecked > 10);
}

TEST_CASE("backlog FL: EVERY output of EVERY device the fx lane can hold has an arm, and it is "
          "inside its own card — at the lane's width and at a narrower one") {
	// **THE FX LANE DREW NO OUTPUT CHIPS AT ALL.** `SourceArm` appeared in `mod_panel.*` and
	// `rack_editor.*` and nowhere else, so a device that published a modulation source could sit in
	// this lane and be unroutable from it — the capability present at every layer except the one a
	// hand touches, which is the sentence the rack's own arms were added under.
	//
	// **NOTHING NOTICED BECAUSE NO FX DEVICE PUBLISHED ANYTHING UNTIL SKLITTER.** Every `src/fx/*`
	// descriptor said `.outputs = {}`; the lane's silence was correct for as long as there was nothing
	// to say. All of a device's outputs belong on its card.
	//
	// **AT TWO LANE WIDTHS.** The lane is a fixed-width rail, so `kWidth` is the only width the EDITOR
	// produces and the first pass is the real one. The second is narrower than the host can currently
	// make it, and is here because a wrap that is nearly good enough at the card width THIS lane
	// produces (`kWidth` less a `kPadX` each side) is where this defect lives:
	// the walk must put a chip on a new row rather than off the left edge, and a card wide enough for
	// every chip would never ask it to.
	//
	// **EVERY OUTPUT IS REQUIRED TO BE PRESENT HERE, AND THAT DOES NOT CONTRADICT THE HIDING RULE**
	// (`layoutOutputChips`, 2026-09-19: an output that cannot be drawn inside its card is hidden). A
	// card in THIS lane grows to hold its own foot — `laneExtent` reserves the rows — so there is no
	// width either of these passes can produce at which a chip does not fit, and "all seven are drawn"
	// is therefore the correct assertion rather than a lenient one. The lane that CAN run out of room
	// is the rack's, whose card height is granted rather than asked for, and its own case covers it.
	for (const float laneWidth : { ui::FxLane::kWidth, 190.0f }) {
		CAPTURE(laneWidth);
		size_t publishing = 0;
		size_t outputsChecked = 0;
		size_t multiOutputDevices = 0;

		ModFixture probe;
		auto* probeLane = findInTree<ui::FxLane>(probe.root);
		REQUIRE(probeLane != nullptr);
		const std::vector<const feathers::device::Descriptor*> offered = probeLane->addableForTest();
		REQUIRE(offered.size() >= 4);

		for (const feathers::device::Descriptor* desc : offered) {
			if (desc->outputCount() == 0) {
				continue; // an effect publishes nothing and gets no strip — the other half of the rule
			}
			++publishing;
			const std::string tag(desc->traits.tag);
			CAPTURE(tag);
			// A FIXTURE PER DEVICE: a card built into a crowded lane may be scrolled out of view, and
			// an arm follows its card's visibility.
			ModFixture each;
			auto* lane = findInTree<ui::FxLane>(each.root);
			REQUIRE(lane != nullptr);
			lane->setCollapsed(false);
			lane->setBounds(lane->x(), lane->y(), static_cast<int>(laneWidth), lane->height());
			const size_t slot = each.bridge.addFxDevice(desc->traits.tag);
			REQUIRE(slot < feathers::kMaxFxSlots);
			lane->resized();
			lane->scrollCardIntoView(slot);
			lane->resized();
			each.gui.tickForTest();

			ui::FxCard* card = lane->cardAt(slot);
			REQUIRE(card != nullptr);
			REQUIRE(card->isVisible());
			REQUIRE_FALSE(card->folded());
			const visage::Bounds box = card->bounds();
			REQUIRE(box.width() > 0.0f);
			// **THE CARD RESERVED THE ROWS ITS CHIPS NEED** — and this is asked of the FRAME, which is
			// the only side of it that can answer.
			//
			// **IT USED TO ASK `outputStripHeight()` AND THAT ASSERTION COULD NOT FAIL.** That accessor
			// is DEFINED as `kCardOutputStripHeight * outputRowsAt(width())` (`fx_card.h`), and
			// `outputRows()` is the same `outputRowsAt(width())` — so the line read `22*r == 22*r` while
			// its comment claimed it would catch a card that reserved one row for seven chips. It could
			// not catch anything. **A tautology wearing a value assertion's words is worse than no
			// assertion**: the next reader counts it as covered.
			//
			// `ownFrame()` goes through `ui::cardFrameOf`, so this compares the rectangle the card will
			// actually clip, veil and lay arms into against the row count — and it fails if the frame
			// and the count disagree, OR if the strip was CLAMPED because the card is too short to hold
			// its own foot. A card in this lane grows, so the clamp must not be firing here; that is a
			// claim with a false side, which is what makes it worth writing.
			CHECK(card->ownFrame().outputs.height()
			      == doctest::Approx(ui::kCardOutputStripHeight
			                         * static_cast<float>(card->outputRows())));
			// AND THE RESERVATION REACHES THE CHIPS: the strip is tall enough for the row the LAST chip
			// sits on. This is the half that ties "reserved" to "what the chips need" rather than to
			// what the card says it needs.
			{
				// THE LAST CHIP THE CARD SHOWS (`DeviceHost::outputsShown`: Pattern shows its open tab's).
				size_t lastShown = desc->outputCount() - 1;
				while (lastShown > 0 && ((card->outputsShown() >> lastShown) & 1u) == 0u) {
					--lastShown;
				}
				const visage::Bounds last = lane->outputArmBoundsForTest(slot, lastShown);
				REQUIRE(last.width() > 0.0f);
				const visage::Bounds strip = card->ownFrame().outputs;
				CHECK(last.bottom() - (strip.y() + static_cast<float>(box.y()))
				      <= strip.height() + 0.5f);
			}

			float previousRight = -1.0f;
			float previousTop = -1.0f;
			for (size_t output = 0; output < desc->outputCount(); ++output) {
				CAPTURE(output);
				CAPTURE(nameOrUnnamed(desc->outputs[output].name));
				const visage::Bounds chip = lane->outputArmBoundsForTest(slot, output);
				if (((card->outputsShown() >> output) & 1u) == 0u) {
					CHECK(chip.width() == 0.0f); // NOT ON THE CARD (`DeviceHost::outputsShown`)
					continue;
				}
				REQUIRE(chip.width() > 0.0f);
				// INSIDE ITS CARD, ALL FOUR EDGES — `visage::Bounds` has `contains(Point)` and not
				// `contains(Bounds)`, so the four inequalities are written out.
				CHECK(chip.x() >= box.x() - 0.5f);
				CHECK(chip.right() <= box.right() + 0.5f);
				CHECK(chip.y() >= box.y() - 0.5f);
				CHECK(chip.bottom() <= box.bottom() + 0.5f);
				// **AND AFTER THE ONE BEFORE IT IN READING ORDER: further RIGHT on the same row, or on a
				// row below.** This read `chip.right() <= previousRight` until 2026-09-20 — further LEFT —
				// because `layoutOutputChips` packed from the right, which drew SKLITTER's seven outputs
				// with `Weight 1` to the right of `Weight 2`: the mod outputs on the card were
				// badly formatted. The assertion is the same claim in the same shape; what changed is
				// which direction counts as forward, and it is now the one a reader reads in.
				const bool sameRow = std::abs(chip.y() - previousTop) < 0.5f;
				if (output > 0 && sameRow) {
					CHECK(chip.x() >= previousRight);
				}
				else if (output > 0) {
					CHECK(chip.y() > previousTop);
				}
				previousRight = chip.right();
				previousTop = chip.y();
				++outputsChecked;
			}
			if (desc->outputCount() > 1) {
				++multiOutputDevices;
			}
		}
		// NOT VACUOUS, AND THE THREE COUNTERS EACH COUNT SOMETHING THE LOOP COULD HAVE SKIPPED. A
		// fourth stood here — `devicesChecked == offered.size()` — and it was trivially true, because
		// it was incremented before the only `continue` in the loop. **The breadth is carried by
		// `REQUIRE(offered.size() >= 4)` above**, which is the claim that was really being made; a
		// counter that cannot fail says nothing and reads as though it can.
		CHECK(publishing >= 1);
		CHECK(outputsChecked >= 7);
		CHECK(multiOutputDevices >= 1);
	}
}

TEST_CASE("an fx output arm ARMS ITS OWN SOURCE, and goes when its device does") {
	// **THE GEOMETRY CASE ABOVE IS SATISFIED BY A DECORATIVE CHIP.** An arm wired to the wrong
	// endpoint sits in exactly the right rectangle and looks identical on screen; the rack's own arm
	// case makes the same distinction and for the same reason. So this presses the thing and reads
	// back WHICH source took the focus.
	ModFixture f;
	auto* lane = findInTree<ui::FxLane>(f.root);
	REQUIRE(lane != nullptr);
	lane->setCollapsed(false);

	const feathers::device::Descriptor* publisher = nullptr;
	for (const feathers::device::Descriptor* desc : lane->addableForTest()) {
		if (desc->outputCount() > 0) {
			publisher = desc;
			break;
		}
	}
	REQUIRE(publisher != nullptr);

	const size_t slot = f.bridge.addFxDevice(publisher->traits.tag);
	REQUIRE(slot < feathers::kMaxFxSlots);
	lane->resized();
	lane->scrollCardIntoView(slot);
	lane->resized();
	f.gui.tickForTest();

	ui::ModState* mod = f.gui.modForTest();
	REQUIRE(mod != nullptr);
	const DeviceInstanceId device = f.bridge.fxDeviceAt(slot);
	REQUIRE(device != feathers::kNoInstance);

	// **THE LAST OUTPUT, NOT THE FIRST.** A lane that built one arm and laid the rest on top of it
	// would pass a press on output 0; the endpoint that distinguishes a real per-output arm from a
	// single shared one is the last declared output.
	//
	// **PRESSED THROUGH THE REAL TREE**, at the rectangle the lane says the chip occupies, so this is
	// also the claim that the chip is REACHABLE there — a chip drawn under its own card is pressed by
	// the card, which is the defect `SourceArm::setOnTop` exists for.
	const size_t lastOutput = publisher->outputCount() - 1;
	const visage::Bounds chip = lane->outputArmBoundsForTest(slot, lastOutput);
	REQUIRE(chip.width() > 0.0f);
	const visage::Point inLane { chip.x() + chip.width() * 0.5f, chip.y() + chip.height() * 0.5f };
	const visage::Point inWindow = lane->positionInWindow() + inLane;
	dragThroughRoot(f.root, inWindow, inWindow);
	CHECK(mod->armed());
	CHECK(mod->source() == backplane::Source::output(device, lastOutput));
	CHECK(mod->source().isDevice());

	// AND THE ARMS GO WHEN THE DEVICE DOES, rather than staying aimed at a slot nothing holds.
	REQUIRE(lane->outputArmCountForTest() == publisher->outputCount());
	f.bridge.removeFxDevice(slot);
	lane->resized();
	CHECK(lane->outputArmCountForTest() == 0);
}

TEST_CASE("backlog DB: the fx lane SCROLLS while a dragged card is held at its edge — a steady step per editor tick, the drop gap following the hand, back the other way at the top, still in the middle, cancelled outside, and nothing once the card is dropped") {
	ModFixture f;
	auto* lane = findInTree<ui::FxLane>(f.root);
	REQUIRE(lane != nullptr);
	for (size_t i = 0; i < 6; ++i) {
		REQUIRE(f.bridge.addFxDevice("flt1") < kMaxFxSlots);
	}
	f.bridge.refresh();
	lane->resized();
	// A LANE SHORTER THAN ITS CARDS, so the case does not depend on a card's height — the strip's scroll
	// case takes the same road.
	lane->setBounds(lane->x(), lane->y(), lane->width(), 240.0f);
	lane->resized();
	REQUIRE(lane->maxScroll() > 0.0f);
	lane->setScroll(0.0f);
	CHECK_FALSE(lane->tickDrag()); // nothing held: nothing scrolls

	// THE CARD REPORTS IN ITS OWN COORDINATES; the case wants a point in the LANE's, so it converts back
	// through the card's current place — which moves with the scroll, and is re-read each time.
	const auto inCardFor = [&](float laneY) {
		return visage::Point { 10.0f, laneY - lane->cardBounds(0).y() };
	};
	lane->cardDragForTest(0, ui::FxCard::DragPhase::Begin, { 10.0f, 10.0f });
	lane->cardDragForTest(0, ui::FxCard::DragPhase::Move, inCardFor(lane->height() - 4.0f)); // held at the bottom
	REQUIRE(lane->dropGap().has_value());
	const size_t gapBefore = *lane->dropGap();

	// A STEP PER TICK, TO THE END, THE GAP FOLLOWING.
	CHECK(lane->tickDrag());
	CHECK(lane->scroll() == doctest::Approx(ui::FxLane::kEdgeScrollStep));
	float last = lane->scroll();
	int ticks = 0;
	while (lane->tickDrag() && ticks < 10000) {
		CHECK(lane->scroll() > last);
		last = lane->scroll();
		++ticks;
	}
	CHECK(ticks > 0);
	CHECK(lane->scroll() == doctest::Approx(lane->maxScroll()));
	REQUIRE(lane->dropGap().has_value());
	CHECK(*lane->dropGap() > gapBefore);

	// BACK THE OTHER WAY AT THE TOP.
	lane->cardDragForTest(0, ui::FxCard::DragPhase::Move, inCardFor(4.0f));
	CHECK(lane->tickDrag());
	CHECK(lane->scroll() == doctest::Approx(lane->maxScroll() - ui::FxLane::kEdgeScrollStep));

	// STILL IN THE MIDDLE.
	lane->cardDragForTest(0, ui::FxCard::DragPhase::Move, inCardFor(lane->height() * 0.5f));
	const float held = lane->scroll();
	CHECK_FALSE(lane->tickDrag());
	CHECK(lane->scroll() == doctest::Approx(held));

	// OUTSIDE THE LANE IS THE CANCEL, NOT A SCROLL.
	lane->cardDragForTest(0, ui::FxCard::DragPhase::Move, inCardFor(lane->height() + 20.0f));
	CHECK_FALSE(lane->dropGap().has_value());
	CHECK_FALSE(lane->tickDrag());
	CHECK(lane->scroll() == doctest::Approx(held));

	// AND THE EDITOR'S TICK IS WHAT DRIVES IT — the one clock a still pointer has.
	lane->setScroll(0.0f);
	lane->cardDragForTest(0, ui::FxCard::DragPhase::Move, inCardFor(lane->height() - 4.0f));
	(void)f.gui.tickForTest();
	CHECK(lane->scroll() == doctest::Approx(ui::FxLane::kEdgeScrollStep));

	// DROPPED: nothing more.
	lane->cardDragForTest(0, ui::FxCard::DragPhase::End, inCardFor(lane->height() - 4.0f));
	CHECK_FALSE(lane->dropGap().has_value());
	CHECK_FALSE(lane->tickDrag());
}

//--------------------------------------------------------------------------------------------------//
// A LOAD REPLACES THE CHAINS UNDER BOTH LANES                                                      //
//--------------------------------------------------------------------------------------------------//

namespace {

/// A PATCH WHOSE CHAINS DIFFER FROM A FRESH EDITOR'S IN EVERY WAY A LANE KEYS A CARD ON: the fx lane's
/// first slot changes KIND (the default FILTER gone, HARM in its place) and grows a second (OWLTT); the
/// strip keeps its default DAHDSR at slot 0 and grows an MSEG and an LFO behind it. Made by a second
/// bridge, which is what a saved patch is: another instance's record.
feathers::rack::RackPatch patchWithOtherChains() {
	ParamBridge other;
	REQUIRE(other.removeFxDevice(0));
	REQUIRE(other.addFxDevice(feathers::fx::harm::kTraits.tag) < feathers::kMaxFxSlots);
	REQUIRE(other.addFxDevice(feathers::fx::owltt::kTraits.tag) < feathers::kMaxFxSlots);
	REQUIRE(other.addRackDevice(feathers::mod::mseg::kTraits.tag) < feathers::kMaxRackSlots);
	REQUIRE(other.addRackDevice(feathers::mod::lfo::kTraits.tag) < feathers::kMaxRackSlots);
	return other.chainsPatch();
}

/// THE LANE NAMES THE LOADED DEVICES — by tag and by id, card by card, which is the value the report is
/// about ("effects dont populate"). A pixel difference would pass on a lane that drew SOMETHING new.
void checkLaneShowsHarmThenOwltt(const ParamBridge& bridge, const ui::FxLane& lane) {
	REQUIRE(bridge.fxDeviceCount() == 2);
	CHECK(lane.deviceCount() == 2);
	const std::array<const char*, 2> expected { feathers::fx::harm::kTraits.tag, feathers::fx::owltt::kTraits.tag };
	for (size_t slot = 0; slot < expected.size(); ++slot) {
		CAPTURE(slot);
		const ui::FxCard* card = lane.cardAt(slot);
		REQUIRE(card != nullptr);
		REQUIRE(card->descriptor() != nullptr);
		CHECK(feathers::device::sameTag(card->descriptor()->traits.tag, expected[slot]));
		CHECK(card->device() == bridge.fxDeviceAt(slot));
	}
	// AND LAID OUT, NOT MERELY BUILT: the first card has a size and is shown — which is what the collapse
	// toggle used to be needed for.
	const ui::FxCard* first = lane.cardAt(0);
	CHECK(first->isVisible());
	CHECK(first->width() > 0.0f);
	CHECK(first->height() > 0.0f);
}

/// A HOST WHOSE STATE CARRIES CHAINS. `StatefulGuiHost` round-trips the parameters only; the plugin's
/// `guiLoadState` also walks the tagged blocks, whose chains block ends in `ParamBridge::restoreChains`.
/// That last call is what this adds, with the patch the "preset" holds — the editor's half of the road
/// is the same whichever bytes carried it.
struct ChainLoadingHost : StatefulGuiHost {
	std::optional<feathers::rack::RackPatch> chainsOnLoad;
	bool guiLoadState(clap_istream_t* stream) noexcept override {
		if (!StatefulGuiHost::guiLoadState(stream)) {
			return false;
		}
		if (chainsOnLoad.has_value() && bridge != nullptr) {
			(void)bridge->restoreChains(*chainsOnLoad);
		}
		return true;
	}
};

} // namespace

TEST_CASE("a host state load puts the loaded devices on the fx lane's cards, without touching the fold") {
	// THE DEFECT: after a preset load, effects did not appear until the fx pane was closed and reopened.
	// `stateLoad` restores the chains through the bridge and asks the host for a flush; the
	// editor's next tick is the whole of what the editor gets.
	ModFixture f;
	auto* lane = findInTree<ui::FxLane>(f.root);
	REQUIRE(lane != nullptr);
	lane->setCollapsed(false);
	(void)f.gui.tickForTest();
	// NOT VACUOUS: the lane starts on the default FILTER, which the patch replaces.
	REQUIRE(lane->deviceCount() == 1);
	REQUIRE(lane->cardAt(0) != nullptr);
	REQUIRE(lane->cardAt(0)->descriptor() != nullptr);
	REQUIRE(feathers::device::sameTag(lane->cardAt(0)->descriptor()->traits.tag, feathers::fx::filter::kTraits.tag));

	(void)f.bridge.restoreChains(patchWithOtherChains());
	(void)f.gui.tickForTest();
	checkLaneShowsHarmThenOwltt(f.bridge, *lane);
	CHECK_FALSE(lane->collapsed());
}

TEST_CASE("a host state load puts the loaded modulators on the strip's cards") {
	// THE SAME ROAD INTO THE OTHER LANE. The strip keeps a dormant card in every slot, so before the fix
	// slot 1 went on being a hidden envelope card with no device behind it.
	ModFixture f;
	f.panel->setCollapsed(false);
	(void)f.gui.tickForTest();
	REQUIRE(f.bridge.modRack().count == 1);

	(void)f.bridge.restoreChains(patchWithOtherChains());
	(void)f.gui.tickForTest();
	REQUIRE(f.bridge.modRack().count == 3);
	const auto* mseg = dynamic_cast<const ui::MsegCard*>(f.panel->cardFrameForTest(1));
	REQUIRE(mseg != nullptr);
	CHECK(mseg->device() == f.bridge.rackDeviceAt(1));
	const auto* lfo = dynamic_cast<const ui::FxCard*>(f.panel->cardFrameForTest(2));
	REQUIRE(lfo != nullptr);
	REQUIRE(lfo->descriptor() != nullptr);
	CHECK(feathers::device::sameTag(lfo->descriptor()->traits.tag, feathers::mod::lfo::kTraits.tag));
	CHECK(lfo->device() == f.bridge.rackDeviceAt(2));
	CHECK(mseg->isVisible());
	CHECK(mseg->height() > 0.0f);
}

TEST_CASE("a route-amount drag on Path survives every rebuild that can land between two of its moves (backlog 19)") {
	// THE CRASH: adjusting a modulation amount to Path — SIGSEGV in
	// `ParamControl::setAmount` under `Knob::commitRoute` under `handleMouseMove`, so mid-drag, after at
	// least one good move of the same gesture. For the Path knob
	// `setAmount` reaches three things: the bridge, a static descriptor, and `mod_->touched()`, which
	// redraws every frame on the modulation watcher list. The first two outlive the editor, so the fault
	// was that list holding a frame some earlier rebuild had freed (f7dc28f0 found the lane leaving its
	// cards on it, 25282ded the overlay). `ui::Watch` (2026-09-01) now takes a frame off whatever list it
	// joined when it dies, and `forgetFrameTree` takes a retired subtree off before it goes.
	//
	// SO THIS CASE IS THE GESTURE ITSELF: ONE PRESS, MOVES BETWEEN EVERY REBUILD THAT CAN FREE A FRAME,
	// AND A TICK AFTER EACH so the retired cards and overlay content are actually freed before the next
	// move walks the list. Under ASan (scripts/sanitize-gate.sh) a freed frame left on the list is a
	// heap-use-after-free on the very next move. In an ordinary build that read does not fault, so the
	// list's SIZE is asserted too: after the add/remove legs it must be back at its baseline.
	ModFixture f;
	auto* lane = findInTree<ui::FxLane>(f.root);
	REQUIRE(lane != nullptr);
	lane->setCollapsed(false);
	f.panel->setCollapsed(false);
	// THE FLOATING PANEL, so the overlay leg opens in the editor and its dismissal retires frames here.
	(void)f.gui.canvasForTest()->setCollapsed(true);
	const auto settle = [&] {
		lane->resized();
		f.panel->resized();
		(void)f.gui.tickForTest();
	};
	settle();

	const backplane::Source source = backplane::Source::core(0);
	f.arm(source);
	ui::ModState* modState = f.gui.modForTest();
	REQUIRE(modState != nullptr);
	REQUIRE(modState->armed());
	ui::Knob* path = f.knobFor(ParamId::Path);
	REQUIRE(path != nullptr);
	const int destination = modDestinationIndexOf(ParamId::Path);
	REQUIRE(destination >= 0);
	const backplane::Destination dest = backplane::Destination::core(static_cast<size_t>(destination));
	REQUIRE(f.bridge.routeAmount(source, dest) == 0.0f);

	const Gui::WatcherCounts base = f.gui.watcherCountsForTest();
	REQUIRE(base.modulation > 0);

	// THE EVENTS VISAGE'S HANDLER BUILDS FOR THE FRAME IT HOLDS AS `mouse_down_frame_` — its position
	// re-read on every move, as `handleMouseMove` does, because a rebuild may move the knob.
	const visage::Point centre = f.faceCentre(path);
	REQUIRE(f.root->frameAtPoint(centre) == path);
	const auto eventAt = [&](visage::Point window, bool isDown) {
		visage::MouseEvent e;
		e.window_position = window;
		e.position = window - path->positionInWindow();
		e.event_frame = path;
		e.button_id = visage::kMouseButtonLeft;
		e.button_state = isDown ? visage::kMouseButtonLeft : visage::kMouseButtonNone;
		e.is_down = isDown;
		e.repeat_click_count = 1;
		return e;
	};
	const visage::MouseEvent press = eventAt(centre, true);
	path->processMouseDown(press);
	// THE PRESS SEEDED THE ROUTE — the drag really is on the amount road, not the value road.
	REQUIRE(std::abs(f.bridge.routeAmount(source, dest) - ui::Knob::kRouteSeedAmount) < 1e-6f);

	// EACH MOVE IS CHECKED BY VALUE against Knob's own rule restated here (anchor plus travel over the
	// range, clamped), and by the notification the crash was inside: `touched()` must have told frames.
	float y = centre.y;
	size_t moves = 0;
	const auto move = [&](const char* after) {
		CAPTURE(after);
		// THE KNOB UNDER THE HAND SURVIVED. If a rebuild replaced it, visage's handler would have dropped
		// the drag (`giveUpFocus`), and delivering to it here would be a gesture no host can make.
		REQUIRE(f.knobFor(ParamId::Path) == path);
		y -= 3.0f;
		const size_t told = modState->framesToldForTest();
		const visage::MouseEvent e = eventAt({ centre.x, y }, true);
		path->processMouseDrag(e);
		const float expected = std::clamp(ui::Knob::kRouteSeedAmount
		                                      + (press.position.y - e.position.y) / ui::Knob::kRouteDragRange,
		                                  -device::kMaxAmount, device::kMaxAmount);
		CHECK(std::abs(f.bridge.routeAmount(source, dest) - expected) < 1e-5f);
		CHECK(modState->framesToldForTest() > told);
		++moves;
	};
	move("the press");

	// 1. AN FX CARD BUILT AND RETIRED — FxLane::ensureCard, the site f7dc28f0 fixed.
	const size_t fxSlot = f.bridge.addFxDevice(fx::harm::kTraits.tag);
	REQUIRE(fxSlot < kMaxFxSlots);
	settle();
	move("an fx device added");
	REQUIRE(f.bridge.removeFxDevice(fxSlot));
	settle();
	move("that fx device removed");

	// 2. A STRIP CARD — a rack card puts the card AND each of its boxes on the modulation list.
	const size_t rackSlot = f.bridge.addRackDevice(mod::mseg::kTraits.tag);
	REQUIRE(rackSlot < kMaxRackSlots);
	settle();
	// THE STRIP REALLY REBUILT: the count MOVES (an MSEG card replaces a dormant one, so it need not rise).
	REQUIRE(f.gui.watcherCountsForTest().modulation != base.modulation);
	move("a modulator added");
	REQUIRE(f.bridge.removeRackDevice(rackSlot));
	settle();
	move("that modulator removed");

	// 3. THE EXPANDED OVERLAY OPENED AND DISMISSED MID-GESTURE — the entry's second suspect (25282ded).
	// HARM, because it declares a window; its knobs join the modulation list one by one.
	const size_t windowed = f.bridge.addFxDevice(fx::harm::kTraits.tag);
	REQUIRE(windowed < kMaxFxSlots);
	settle();
	const size_t beforeOpen = f.gui.watcherCountsForTest().modulation;
	REQUIRE(lane->onExpandDevice != nullptr);
	lane->onExpandDevice(windowed);
	REQUIRE(f.gui.expandedForTest()->open());
	settle();
	// THE PANEL'S KNOBS REALLY JOINED THE LIST, so its dismissal has something to leave behind.
	REQUIRE(f.gui.watcherCountsForTest().modulation > beforeOpen);
	move("an expanded view opened");
	f.gui.expandedForTest()->dismiss();
	REQUIRE_FALSE(f.gui.expandedForTest()->open());
	settle();
	move("that expanded view dismissed");
	REQUIRE(f.bridge.removeFxDevice(windowed));
	settle();
	move("its device removed");

	// EVERY LIST BACK WHERE IT STARTED: a frame freed by those three legs and left behind would be counted
	// here, which is the ordinary build's reading of what ASan reads on the move above.
	const Gui::WatcherCounts cycled = f.gui.watcherCountsForTest();
	CHECK(cycled.modulation == base.modulation);
	CHECK(cycled.state == base.state);
	CHECK(cycled.registry == base.registry);

	// 4. A HOST STATE LOAD — the entry's third suspect: both strips rebuilt from another record at once.
	(void)f.bridge.restoreChains(patchWithOtherChains());
	settle();
	REQUIRE(f.bridge.fxDeviceCount() == 2);
	REQUIRE(f.bridge.modRack().count == 3);
	move("a host state load");
	REQUIRE(f.bridge.removeRackDevice(1));
	REQUIRE(f.bridge.removeFxDevice(0));
	settle();
	move("the loaded devices removed");

	path->processMouseUp(eventAt({ centre.x, y }, false));
	REQUIRE(moves == 10);
	// ONE GESTURE, ONE UNDO STEP — the drag ended as a drag, not as something a rebuild cut short.
	REQUIRE(f.gui.history() != nullptr);
	CHECK(f.gui.history()->canUndo());
	CHECK(f.bridge.routeAmount(source, dest) > ui::Knob::kRouteSeedAmount);
}

TEST_CASE("a preset picked in the editor's browser puts the loaded devices on the fx lane's cards") {
	// THE PRESET BROWSER'S ROAD: `Gui::Impl::loadPresetBytes` -> `GuiHost::guiLoadState`, the road the defect
	// was on. Everything after the load is the editor's.
	const ScopedPresetLibrary library("lane-refresh");
	ChainLoadingHost host;
	ParamBridge bridge;
	feathers::BridgeDeviceHost deviceHost { bridge };
	host.bridge = &bridge;
	Gui gui(host, bridge);
	REQUIRE(gui.create(CLAP_WINDOW_API_X11, false));
	gui.setPopoutWindowForTest(false);
	REQUIRE(gui.setSize(Gui::kLogicalWidth, Gui::kLogicalHeight));
	auto* lane = findInTree<ui::FxLane>(gui.root());
	REQUIRE(lane != nullptr);
	lane->setCollapsed(false);
	(void)gui.tickForTest();
	ui::PresetBrowser* browser = gui.presetBrowserForTest();
	REQUIRE(browser != nullptr);
	saveThroughPrompt(*browser);
	const int saved = browser->currentIndex();
	REQUIRE(saved >= 0);
	REQUIRE(lane->deviceCount() == 1);

	host.chainsOnLoad = patchWithOtherChains();
	browser->selectIndex(saved);
	(void)gui.tickForTest();
	checkLaneShowsHarmThenOwltt(bridge, *lane);
	CHECK_FALSE(lane->collapsed());
}

TEST_CASE("a load that changes only a fold re-lays the strip, on the card it kept") {
	// THE SAME DEVICES, THE SAME IDS, SO NO CARD IS REBUILT — and the fold lives in the patch
	// (`ParamBridge::deviceFolded`) but reaches the card only through a layout.
	ModFixture f;
	f.panel->setCollapsed(false);
	(void)f.gui.tickForTest();
	const backplane::Source head = backplane::Source::output(f.bridge.rackDeviceAt(0), 0);
	const auto* card = dynamic_cast<const ui::FxCard*>(f.panel->cardFrameForTest(0));
	REQUIRE(card != nullptr);
	REQUIRE_FALSE(card->folded());
	const float openHeight = card->height();
	const feathers::rack::RackPatch open = f.bridge.chainsPatch();

	REQUIRE(f.panel->setCardMinimised(head, true));
	(void)f.gui.tickForTest();
	// NOT VACUOUS: folding moved both the card's state and its height.
	REQUIRE(card->folded());
	REQUIRE(card->height() < openHeight);

	(void)f.bridge.restoreChains(open);
	(void)f.gui.tickForTest();
	// THE SAME CARD — nothing was rebuilt, which is the condition this case is about.
	CHECK(f.panel->cardFrameForTest(0) == card);
	CHECK_FALSE(f.panel->cardMinimised(head));
	CHECK_FALSE(card->folded());
	CHECK(card->height() == doctest::Approx(openHeight));
}

//------------------------------------------------------------------------------------------------//
// VOICE STACKING STEP 3 — THE STACK INDEX'S ARROW AND THE ENGINE RAIL'S STACK PAGE (backlog KE)      //
//------------------------------------------------------------------------------------------------//

namespace {

/// Stack set the whole way round — posted, published, refreshed — because there is no audio thread here.
void publishStackCount(ModFixture& f, double count) {
	ParamSet params;
	for (const ParamDesc& desc : paramDescs()) {
		params.setBase(static_cast<clap_id>(desc.id), f.bridge.value(static_cast<clap_id>(desc.id)));
	}
	params.setBase(static_cast<clap_id>(ParamId::StackCount), count);
	f.bridge.publish(params);
	f.bridge.refresh();
	// And the Engine rail told, as its draw would: its enablement follows Stack.
	if (auto* engineRail = findInTree<ui::EngineRail>(f.root); engineRail != nullptr) {
		engineRail->refreshEnablement();
	}
}

visage::Point centreInWindow(const visage::Frame* frame, const visage::Bounds& local) {
	return frame->positionInWindow() + visage::Point { local.x() + local.width() * 0.5f, local.y() + local.height() * 0.5f };
}

backplane::Destination coreParam(ParamId id) {
	return backplane::Destination::core(static_cast<size_t>(modDestinationIndexOf(id)));
}

} // namespace

TEST_CASE("the Stack Index's arrow is greyed at a stack of one, and armed it lights Fine, Level and the stand only") {
	ModFixture f;
	// THE ARROW LIVES ON THE STACK PAGE (backlog MO), so the page is turned for a hand to reach it.
	auto* engineRail = findInTree<ui::EngineRail>(f.root);
	REQUIRE(engineRail != nullptr);
	engineRail->showPage(ui::EngineRail::Page::Stack);
	ui::StackIndexArrow* arrow = engineRail->stackArrowForTest();
	REQUIRE(arrow != nullptr);
	REQUIRE(arrow->isVisible());
	const backplane::Source stack = ui::stackIndexSource();

	// THE ROUTING RULE: a per-part value reaches only what a part has of its own (voice-stacking doc §3.3).
	CHECK(f.bridge.mayRoute(stack, coreParam(ParamId::FineTune)));
	CHECK(f.bridge.mayRoute(stack, coreParam(ParamId::VoiceLevel)));
	// AND THE PART'S STAND (step 4): Pos X and Pos Y displace each part from the puck.
	CHECK(f.bridge.mayRoute(stack, coreParam(ParamId::PosX)));
	CHECK(f.bridge.mayRoute(stack, coreParam(ParamId::PosY)));
	for (const ParamId shared : { ParamId::Path, ParamId::GlideTime, ParamId::StackDetune,
		                          ParamId::StackSpread, ParamId::Width }) {
		CAPTURE(static_cast<int>(shared));
		CHECK_FALSE(f.bridge.mayRoute(stack, coreParam(shared)));
	}
	CHECK(ui::modSourceName(f.bridge, stack) == "Stack Index");

	// A STACK OF ONE: the cell is there, greyed, and a click on it arms nothing.
	const visage::Point at = centreInWindow(arrow, visage::Bounds { 0.0f, 0.0f, arrow->width(), arrow->height() });
	CHECK_FALSE(arrow->stackEnabled());
	dragThroughRoot(f.root, at, at);
	CHECK_FALSE(arrow->stackArmed());

	// A STACK OF THREE: the same click arms it, and only Fine lights among the Engine rail's knobs.
	publishStackCount(f, 3.0);
	CHECK(arrow->stackEnabled());
	dragThroughRoot(f.root, at, at);
	REQUIRE(arrow->stackArmed());
	ui::Knob* fine = f.knobFor(ParamId::FineTune);
	ui::Knob* level = f.knobFor(ParamId::VoiceLevel);
	ui::Knob* path = f.knobFor(ParamId::Path);
	ui::Knob* detune = f.knobFor(ParamId::StackDetune);
	REQUIRE(fine != nullptr);
	REQUIRE(level != nullptr);
	REQUIRE(path != nullptr);
	REQUIRE(detune != nullptr);
	CHECK(fine->mappable());
	CHECK(level->mappable());
	CHECK_FALSE(path->mappable());
	CHECK_FALSE(detune->mappable()); // live at a stack of three, and still a destination the parts share
	ui::Knob* posX = f.knobFor(ParamId::PosX);
	REQUIRE(posX != nullptr);
	CHECK(posX->mappable()); // the stand
	CHECK(detune->enabled());
}

TEST_CASE("the Engine rail turns its page: STACK shows the zone knobs, PITCH the steppers, and the rail still fits") {
	ModFixture f;
	auto* engineRail = findInTree<ui::EngineRail>(f.root);
	REQUIRE(engineRail != nullptr);
	ui::Knob* fineKnob = f.knobFor(ParamId::FineTune);
	REQUIRE(fineKnob != nullptr);
	REQUIRE(engineRail->stackShapeCellForTest() != nullptr);

	// A FRESH EDITOR SHOWS PITCH.
	CHECK(engineRail->page() == ui::EngineRail::Page::Pitch);
	CHECK(fineKnob->isVisible());
	CHECK_FALSE(engineRail->stackShapeCellForTest()->isVisible());

	// A CLICK ON "STACK" TURNS IT, through the real tree.
	dragThroughRoot(f.root, centreInWindow(engineRail, engineRail->pageTabBounds(ui::EngineRail::Page::Stack)),
	                centreInWindow(engineRail, engineRail->pageTabBounds(ui::EngineRail::Page::Stack)));
	CHECK(engineRail->page() == ui::EngineRail::Page::Stack);
	CHECK_FALSE(fineKnob->isVisible());
	CHECK(engineRail->stackShapeCellForTest()->isVisible());
	CHECK(engineRail->stackMapCellForTest()->isVisible());
	CHECK(engineRail->stackCurveForTest()->isVisible());
	// THE STACK PAGE FITS WHERE THE PITCH PAGE DID: nothing scrolled away, and the cells sit inside the rail.
	CHECK(engineRail->maxScroll() == doctest::Approx(0.0f));
	const float bottom = engineRail->positionInWindow().y + engineRail->height();
	CHECK(engineRail->stackCurveForTest()->positionInWindow().y + engineRail->stackCurveForTest()->height() <= bottom);
	// AND THE INDEX'S ARROW, which turns with the page (backlog MO).
	REQUIRE(engineRail->stackArrowForTest() != nullptr);
	CHECK(engineRail->stackArrowForTest()->isVisible());
	CHECK(engineRail->stackArrowForTest()->positionInWindow().y + engineRail->stackArrowForTest()->height() <= bottom);
	// EACH ZONE KNOB CAN PRINT ITS WIDEST MODE in the room it has ("Random Fresh", "Center-out").
	CHECK(engineRail->stackShapeCellForTest()->minimumWidth() <= engineRail->stackShapeCellForTest()->width());
	CHECK(engineRail->stackMapCellForTest()->minimumWidth() <= engineRail->stackMapCellForTest()->width());

	// AND BACK.
	dragThroughRoot(f.root, centreInWindow(engineRail, engineRail->pageTabBounds(ui::EngineRail::Page::Pitch)),
	                centreInWindow(engineRail, engineRail->pageTabBounds(ui::EngineRail::Page::Pitch)));
	CHECK(engineRail->page() == ui::EngineRail::Page::Pitch);
	CHECK(fineKnob->isVisible());
	CHECK_FALSE(engineRail->stackCurveForTest()->isVisible());
	CHECK_FALSE(engineRail->stackArrowForTest()->isVisible());

	SUBCASE("and closing and reopening the rail keeps the page it was on") {
		engineRail->showPage(ui::EngineRail::Page::Stack);
		engineRail->setCollapsed(true);
		CHECK_FALSE(engineRail->stackShapeCellForTest()->isVisible());
		CHECK_FALSE(engineRail->stackArrowForTest()->isVisible());
		engineRail->setCollapsed(false);
		CHECK(engineRail->stackShapeCellForTest()->isVisible());
		CHECK(engineRail->stackArrowForTest()->isVisible());
		CHECK_FALSE(fineKnob->isVisible());
	}
}

TEST_CASE("the stack page is live before any route carries the Stack Index, and greyed only at a stack of one") {
	// BACKLOG MO: not greyed with no mappings. This case used to assert the
	// opposite — greyed until a route existed — and that reading was judged confusing: a control that acts the moment
	// the arrow beside it is used is set up first, not dead. A stack of one is what can never act from here (one part,
	// at index 0), so that still greys, as Detune and Spread do.
	ModFixture f;
	auto* engineRail = findInTree<ui::EngineRail>(f.root);
	REQUIRE(engineRail != nullptr);
	engineRail->showPage(ui::EngineRail::Page::Stack);
	const auto all = [&](bool want) {
		CHECK(engineRail->stackShapeCellForTest()->enabled() == want);
		CHECK(engineRail->stackMapCellForTest()->enabled() == want);
		CHECK(engineRail->stackCurveForTest()->enabled() == want);
		CHECK(engineRail->stackPlotForTest()->enabled() == want);
	};
	// NO ROUTE ANYWHERE FROM THE INDEX: the precondition the old greying turned on, asked of the bridge.
	const backplane::Source source = ui::stackIndexSource();
	for (const ParamId id : { ParamId::FineTune, ParamId::VoiceLevel, ParamId::PosX, ParamId::PosY, ParamId::OscPhase }) {
		CAPTURE(static_cast<int>(id));
		CHECK(f.bridge.routeAmount(source, coreParam(id)) == 0.0f);
	}
	publishStackCount(f, 4.0);
	engineRail->refreshEnablement();
	all(true);

	// AND ITS OWN TERMS STILL TAKE A HAND: the Mapping's mode moves the parameter with no route made.
	ui::ZoneSelect& select = engineRail->stackMapCellForTest()->select();
	select.selectZone(kStackMapNames.size() - 1);
	CHECK(steppedIndex(f.bridge.value(static_cast<clap_id>(ParamId::StackMap)), kStackMapNames.size())
	      == kStackMapNames.size() - 1);

	// A ROUTE CHANGES NOTHING ABOUT IT.
	f.bridge.setRouteAmount(source, coreParam(ParamId::FineTune), 0.1f);
	engineRail->refreshEnablement();
	all(true);

	// A STACK OF ONE takes it away, route or no route.
	publishStackCount(f, 1.0);
	engineRail->refreshEnablement();
	all(false);
	f.bridge.setRouteAmount(source, coreParam(ParamId::FineTune), 0.0f);
	engineRail->refreshEnablement();
	all(false);
}

TEST_CASE("the Stack Index's arrow sits under the stack page's Mapping and Curve, the rail's width, and left the source grid") {
	// BACKLOG MO ("stack index mod to move by the stack controls"): a LAYOUT change. Asserted as
	// rectangles: the arrow's top is one row gap under the lower of Mapping and Curve, its left is Mapping's, its right
	// is Curve's — so it spans exactly the controls above it — and the grid no longer has a row for it.
	ModFixture f;
	auto* engineRail = findInTree<ui::EngineRail>(f.root);
	REQUIRE(engineRail != nullptr);
	engineRail->showPage(ui::EngineRail::Page::Stack);
	ui::StackIndexArrow* arrow = engineRail->stackArrowForTest();
	REQUIRE(arrow != nullptr);
	ui::ModeValueCell* map = engineRail->stackMapCellForTest();
	ui::Knob* curve = engineRail->stackCurveForTest();
	const visage::Point a = arrow->positionInWindow();
	const visage::Point m = map->positionInWindow();
	const visage::Point c = curve->positionInWindow();
	const float rowGap = 6.0f; // the Engine rail's kRowGap
	const float aboveBottom = std::max(m.y + map->height(), c.y + curve->height());
	CHECK(std::abs(a.y - (aboveBottom + rowGap)) < 0.01f);
	CHECK(std::abs(a.x - m.x) < 0.01f);
	CHECK(std::abs((a.x + arrow->width()) - (c.x + curve->width())) < 0.01f);
	CHECK(std::abs(arrow->height() - ui::SourceGrid::kCellHeight) < 0.01f);

	// THE GRID IS ONE FAMILY AGAIN: its heading and the sounds' two rows, nothing below, and the arrow outside it.
	auto* grid = findInTree<ui::SourceGrid>(f.root);
	REQUIRE(grid != nullptr);
	const float rows = static_cast<float>(ui::SourceGrid::kRows);
	CHECK(std::abs(grid->preferredHeight()
	               - (ui::SourceGrid::kHeadingHeight + rows * (ui::SourceGrid::kCellHeight + ui::SourceGrid::kCellGap)))
	      < 0.01f);
	// Where the old row was, nothing answers.
	const float oldRow = ui::SourceGrid::kHeadingHeight + rows * (ui::SourceGrid::kCellHeight + ui::SourceGrid::kCellGap)
	                     + ui::SourceGrid::kCellHeight * 0.5f;
	CHECK(grid->hitAt({ grid->width() * 0.5f, oldRow }).part == ui::SourceGrid::Part::None);
	const visage::Point g = grid->positionInWindow();
	CHECK(a.y + arrow->height() <= g.y + 0.01f);
}

TEST_CASE("the Stack page's Shape and Mapping names open a list of every mode, and nothing moves until one is picked") {
	// CLAUDE.md's MULTIVALUE-TOGGLE RULE: a control shows what you are choosing BEFORE it is applied. The ‹ › arrows
	// step and apply on the way past (Random Fresh included), so the centre of each zone knob must be the other road —
	// the whole list, on demand — and it is `ZoneSelect`'s caption verb, the Menu.
	ModFixture f;
	auto* engineRail = findInTree<ui::EngineRail>(f.root);
	REQUIRE(engineRail != nullptr);
	engineRail->showPage(ui::EngineRail::Page::Stack);
	publishStackCount(f, 4.0);
	f.bridge.setRouteAmount(ui::stackIndexSource(), coreParam(ParamId::FineTune), 0.1f);
	engineRail->refreshEnablement();

	const auto check = [&](ui::ModeValueCell* cell, ParamId mode, std::span<const char* const> names) {
		REQUIRE(cell != nullptr);
		REQUIRE(cell->enabled());
		ui::ZoneSelect& select = cell->select();
		const visage::Bounds caption = select.captionBounds();
		const visage::Point centre { caption.x() + caption.width() * 0.5f, caption.y() + caption.height() * 0.5f };
		// THE CENTRE IS THE LIST, the edges are the steps.
		CHECK(select.verbAt(centre) == ui::ZoneSelect::Verb::Menu);
		CHECK(select.verbAt({ select.prevBounds().x() + 1.0f, centre.y }) == ui::ZoneSelect::Verb::Prev);
		// EVERY MODE IS A ROW, in the parameter's own order and by its own name.
		for (size_t i = 0; i < names.size(); ++i) {
			CAPTURE(i);
			CHECK(select.captionOf(i) == names[i]);
		}
		CHECK(select.captionOf(names.size()).empty());
		// ASKING WHAT A PRESS MEANS APPLIES NOTHING: the mode is where it was.
		const double before = f.bridge.value(static_cast<clap_id>(mode));
		(void)select.verbAt(centre);
		CHECK(f.bridge.value(static_cast<clap_id>(mode)) == before);
		// AND A PICKED ROW IS THE MODE: the menu's callback is `selectZone`.
		select.selectZone(names.size() - 1);
		CHECK(steppedIndex(f.bridge.value(static_cast<clap_id>(mode)), names.size()) == names.size() - 1);
	};
	check(engineRail->stackShapeCellForTest(), ParamId::StackShape, kStackShapeNames);
	check(engineRail->stackMapCellForTest(), ParamId::StackMap, kStackMapNames);
}

TEST_CASE("the stack page draws the patch's index beside the Shape: one dot per part, where the index puts it") {
	ModFixture f;
	auto* engineRail = findInTree<ui::EngineRail>(f.root);
	REQUIRE(engineRail != nullptr);
	engineRail->showPage(ui::EngineRail::Page::Stack);
	ui::StackIndexPlot* plot = engineRail->stackPlotForTest();
	REQUIRE(plot != nullptr);
	CHECK(plot->isVisible());
	// A TRIANGLE PEAKING AT THREE QUARTERS over five parts, restated from its rule (1 − 2·|t − ¾| / ¾):
	// −1, −⅓, +⅓, +1, +⅓.
	ParamSet params;
	for (const ParamDesc& desc : paramDescs()) {
		params.setBase(static_cast<clap_id>(desc.id), f.bridge.value(static_cast<clap_id>(desc.id)));
	}
	params.setBase(static_cast<clap_id>(ParamId::StackCount), 5.0);
	params.setBase(static_cast<clap_id>(ParamId::StackShape), static_cast<double>(StackShape::Triangle));
	params.setBase(static_cast<clap_id>(ParamId::StackShapeAmount), 0.75);
	f.bridge.publish(params);
	f.bridge.refresh();
	const std::vector<float> drawn = plot->indicesForTest();
	const std::array<float, 5> want { -1.0f, -1.0f / 3.0f, 1.0f / 3.0f, 1.0f, 1.0f / 3.0f };
	REQUIRE(drawn.size() == want.size());
	for (size_t part = 0; part < want.size(); ++part) {
		CAPTURE(part);
		CHECK(std::abs(drawn[part] - want[part]) < 1e-5f);
	}
	// AND WHERE THE DOTS SIT: left to right in part order, the peak (part 3) highest, part 0 lowest, inside the frame.
	for (int part = 1; part < 5; ++part) {
		CHECK(plot->dotForTest(part).x > plot->dotForTest(part - 1).x);
	}
	for (int part = 0; part < 5; ++part) {
		CHECK(plot->dotForTest(3).y <= plot->dotForTest(part).y);
		CHECK(plot->dotForTest(0).y >= plot->dotForTest(part).y);
		CHECK(plot->dotForTest(part).y >= 0.0f);
		CHECK(plot->dotForTest(part).y <= plot->height());
	}
	// BESIDE THE SHAPE, in the same row, and the Shape still wide enough for its widest mode.
	CHECK(plot->positionInWindow().y == doctest::Approx(engineRail->stackShapeCellForTest()->positionInWindow().y));
	CHECK(plot->positionInWindow().x >= engineRail->stackShapeCellForTest()->positionInWindow().x
	                                       + engineRail->stackShapeCellForTest()->width() - 0.5f);
	CHECK(engineRail->stackShapeCellForTest()->minimumWidth() <= engineRail->stackShapeCellForTest()->width());
	// AND OFF THE PITCH PAGE.
	engineRail->showPage(ui::EngineRail::Page::Pitch);
	CHECK_FALSE(plot->isVisible());
}

TEST_CASE("lane-mods: a modulator in the fx lane — named apart from the strip's, its fold kept by the patch, and armed from anywhere it focuses its card") {
	// The fx lane takes modulators since 2026-10-04 (every mod device is in scope in the Feathers fx
	// rack). Three things the strip did for its modulators that the lane had no reason to do until then.
	FilterFixture f;
	auto* lane = f.lane;
	REQUIRE(lane != nullptr);
	const char* lfoTag = feathers::mod::lfo::kTraits.tag;
	const size_t stripLfoSlot = f.bridge.addRackDevice(lfoTag);
	REQUIRE(stripLfoSlot != feathers::kNoPosition);
	const size_t laneLfoAt = lane->addDevice(lfoTag);
	REQUIRE(laneLfoAt == 1); // after the default filter
	const feathers::DeviceInstanceId stripLfo = f.bridge.rackDeviceAt(stripLfoSlot);
	const feathers::DeviceInstanceId laneLfo = f.bridge.fxDeviceAt(laneLfoAt);
	REQUIRE(laneLfo != feathers::kNoInstance);
	(void)f.panel->syncCards();
	(void)lane->syncCards();
	REQUIRE(lane->cardAt(laneLfoAt) != nullptr);

	SUBCASE("named by its chain: the strip's LFO and the lane's are two names in the route list") {
		const std::string inStrip = ui::modSourceName(f.bridge, backplane::Source::output(stripLfo, 0));
		const std::string inLane = ui::modSourceName(f.bridge, backplane::Source::output(laneLfo, 0));
		CHECK(inStrip == "LFO " + std::to_string(stripLfoSlot + 1));
		CHECK(inLane == "LFO (FX 2)");
		CHECK(inStrip != inLane);
	}

	SUBCASE("its fold is the bridge's — what the patch saves — and a card built for it reads it back") {
		ui::FxCard* card = lane->cardAt(laneLfoAt);
		REQUIRE(card->onHeadFold);
		CHECK_FALSE(card->folded());
		card->onHeadFold();
		CHECK(card->folded());
		CHECK(f.bridge.deviceFolded(laneLfo));
		// A CARD BUILT FRESH for a device the patch left folded comes up folded: a second LFO, folded on the
		// bridge before the lane has built anything for it.
		const size_t secondAt = f.bridge.addFxDevice(lfoTag);
		REQUIRE(secondAt == 2);
		f.bridge.setDeviceFolded(f.bridge.fxDeviceAt(secondAt), true);
		(void)lane->syncCards();
		REQUIRE(lane->cardAt(secondAt) != nullptr);
		CHECK(lane->cardAt(secondAt)->folded());
	}

	SUBCASE("armed from anywhere, it focuses its card in the lane — the strip's listener hands it over") {
		ui::ModState* mod = f.gui.modForTest();
		REQUIRE(mod != nullptr);
		REQUIRE(lane->focusedSlotForTest() != laneLfoAt);
		mod->focus(backplane::Source::output(laneLfo, 0));
		mod->arm(true);
		CHECK(lane->focusedSlotForTest() == laneLfoAt);
		mod->arm(false);
	}
}

TEST_CASE("OA: in Feathers' fx lane a Shift-press selects a run of cards, the dots menu groups it into one Chain in place, and the editor's undo puts the cards back") {
	ModFixture f;
	ui::FxLane* lane = f.gui.fxLaneForTest();
	REQUIRE(lane != nullptr);
	lane->setCollapsed(false);
	const size_t s1 = f.bridge.addFxDevice("flt1");
	const size_t s2 = f.bridge.addFxDevice("flt1");
	REQUIRE(s1 < feathers::kMaxFxSlots);
	REQUIRE(s2 == s1 + 1);
	const DeviceInstanceId d1 = f.bridge.fxDeviceAt(s1);
	const DeviceInstanceId d2 = f.bridge.fxDeviceAt(s2);
	lane->resized();
	f.gui.tickForTest();
	const size_t before = f.bridge.chainRecord().length(feathers::kLaneChain);

	// THE FOCUS ROAD, THEN THE CARD'S OWN PRESS WITH THE MODIFIER — as visage delivers a press.
	const auto press = [&](size_t slot, int modifiers) {
		ui::FxCard* card = lane->cardAt(slot);
		REQUIRE(card != nullptr);
		card->onCardPressed();
		visage::MouseEvent e {};
		e.position = { static_cast<float>(card->width()) * 0.5f, static_cast<float>(card->height()) * 0.5f };
		e.modifiers = modifiers;
		e.repeat_click_count = 1;
		card->noteCardPress(e);
		card->noteCardRelease(e);
	};
	press(s1, 0);
	press(s2, visage::kModifierShift);
	CHECK(lane->selectionForTest().selection().inOrder(std::vector<DeviceInstanceId> { d1, d2 })
	      == std::vector<DeviceInstanceId> { d1, d2 });
	CHECK(lane->cardAt(s1)->headFocused());
	CHECK(lane->cardAt(s2)->headFocused());
	CHECK(lane->selectionForTest().escapeHeldForTest()); // one entry on the editor's Esc stack

	REQUIRE(lane->slotVerbForTest(s1, ui::FragmentVerb::GroupChain));
	const rack::ChainRecord& record = f.bridge.chainRecord();
	REQUIRE(record.length(feathers::kLaneChain) == before - 1);
	const DeviceInstanceId chain = record.deviceAt(feathers::kLaneChain, s1);
	CHECK(device::sameTag(record.descriptorOf(chain)->traits.tag, composite::kTraits.tag));
	CHECK_FALSE(record.holds(d1));
	CHECK_FALSE(record.holds(d2));
	CHECK(lane->selectionForTest().selection().ids() == std::vector<DeviceInstanceId> { chain });
	CHECK(f.bridge.takeStateDirty());

	// THE EDITOR'S OWN UNDO KEY walks the lane's step back.
	CHECK(f.root->onKeyPress().callback(visage::KeyEvent { visage::KeyCode::Z, visage::kModifierRegCtrl, true }));
	REQUIRE(record.length(feathers::kLaneChain) == before);
	CHECK(record.deviceAt(feathers::kLaneChain, s1) == d1);
	CHECK(record.deviceAt(feathers::kLaneChain, s2) == d2);
}

TEST_CASE("PN: in Feathers' modulator strip, Shift selects a run and Ctrl toggles; Ctrl+D duplicates the selection, Delete "
          "removes several, the dots menu copies and pastes, a dragged selected card carries the block; the editor's undo "
          "walks each back") {
	// THE FX LANE'S GESTURES ON THE STRIP (backlog PN), driven through the real panel's roads and read off the record.
	// KNOWN-BADS, one per road: the card's `onCardSelectPress` left unbound (Shift selects one card, not three); the
	// panel's `selection_->key` line deleted (Ctrl+D duplicates nothing); `dropCard`'s block branch deleted (only the
	// dragged card moves).
	ModFixture f;
	ui::ModPanel* panel = f.panel;
	const size_t s1 = f.bridge.addRackDevice(mod::lfo::kTraits.tag);
	const size_t s2 = f.bridge.addRackDevice(mod::lfo::kTraits.tag);
	const size_t s3 = f.bridge.addRackDevice(mod::lfo::kTraits.tag);
	REQUIRE(s2 == s1 + 1);
	REQUIRE(s3 == s2 + 1);
	f.bridge.refresh();
	panel->resized();
	f.gui.tickForTest();
	const rack::ChainRecord& record = f.bridge.chainRecord();
	const auto strip = [&] {
		std::vector<DeviceInstanceId> out;
		for (size_t p = 0; p < record.length(feathers::kStripChain); ++p) {
			out.push_back(record.deviceAt(feathers::kStripChain, p));
		}
		return out;
	};
	const std::vector<DeviceInstanceId> before = strip();
	const DeviceInstanceId d1 = before[s1];
	const DeviceInstanceId d2 = before[s2];
	const DeviceInstanceId d3 = before[s3];

	// THE FOCUS ROAD, THEN THE CARD'S OWN PRESS WITH THE MODIFIER — as visage delivers a press.
	const auto press = [&](size_t slot, int modifiers) {
		ui::FxCard* card = panel->cardForTest(slot);
		REQUIRE(card != nullptr);
		card->onCardPressed();
		visage::MouseEvent e {};
		e.position = { card->width() * 0.5f, card->height() * 0.5f };
		e.modifiers = modifiers;
		e.repeat_click_count = 1;
		card->noteCardPress(e);
		card->noteCardRelease(e);
	};
	const auto selected = [&] { return panel->selectionForTest().selection().inOrder(strip()); };
	press(s1, 0);
	press(s3, visage::kModifierShift);
	CHECK(selected() == std::vector<DeviceInstanceId> { d1, d2, d3 });
	CHECK(panel->cardForTest(s2)->headFocused());
	CHECK(panel->selectionForTest().escapeHeldForTest()); // one entry on the editor's Esc stack
	press(s2, visage::kModifierRegCtrl);
	CHECK(selected() == std::vector<DeviceInstanceId> { d1, d3 });
	CHECK_FALSE(panel->cardForTest(s2)->headFocused());

	// CTRL+D: the two copies land after the last selected, and become the selection.
	REQUIRE(panel->onKeyPress().callback(visage::KeyEvent { visage::KeyCode::D, visage::kModifierRegCtrl, true }));
	const std::vector<DeviceInstanceId> dup = strip();
	REQUIRE(dup.size() == before.size() + 2);
	const DeviceInstanceId c1 = dup[s3 + 1];
	const DeviceInstanceId c3 = dup[s3 + 2];
	CHECK(selected() == std::vector<DeviceInstanceId> { c1, c3 });
	CHECK(panel->cardForTest(s3 + 2) != nullptr); // the panel built a card for the copy

	// DELETE OVER TWO: both copies go, one step.
	REQUIRE(panel->onKeyPress().callback(visage::KeyEvent { visage::KeyCode::Delete, 0, true }));
	CHECK(strip() == before);
	// THE EDITOR'S UNDO puts them back, then takes the duplicate away.
	CHECK(f.root->onKeyPress().callback(visage::KeyEvent { visage::KeyCode::Z, visage::kModifierRegCtrl, true }));
	CHECK(strip() == dup);
	CHECK(f.root->onKeyPress().callback(visage::KeyEvent { visage::KeyCode::Z, visage::kModifierRegCtrl, true }));
	CHECK(strip() == before);
	f.gui.tickForTest();
	panel->resized();

	// THE DOTS MENU'S VERBS: Copy on d1 (alone — the selection was pruned to what remains), Paste after it.
	press(s1, 0);
	REQUIRE(panel->slotVerbForTest(s1, ui::FragmentVerb::Copy));
	REQUIRE(panel->slotVerbForTest(s1, ui::FragmentVerb::Paste));
	const std::vector<DeviceInstanceId> pasted = strip();
	REQUIRE(pasted.size() == before.size() + 1);
	CHECK(pasted[s1] == d1);
	CHECK(pasted[s1 + 1] != d2); // a fresh copy stands between d1 and d2
	CHECK(pasted[s1 + 2] == d2);
	CHECK(f.root->onKeyPress().callback(visage::KeyEvent { visage::KeyCode::Z, visage::kModifierRegCtrl, true }));
	CHECK(strip() == before);
	f.gui.tickForTest();
	panel->resized();

	// THE BLOCK DRAG: d1 and d3 selected, d3's card dragged to the gap before the strip's first card — both move.
	press(s1, 0);
	press(s3, visage::kModifierRegCtrl);
	REQUIRE(selected() == std::vector<DeviceInstanceId> { d1, d3 });
	ui::FxCard* dragged = panel->cardForTest(s3);
	// THE CARD REPORTS IN ITS OWN COORDINATES, and the panel adds where it drew the card's source.
	const visage::Bounds box = panel->sourceBounds(backplane::Source::output(d3, 0));
	const visage::Bounds gap = panel->gapBounds(0);
	const visage::Point target { gap.x() + gap.width() * 0.5f - box.x(), gap.y() + gap.height() * 0.5f - box.y() };
	dragged->onCardDrag(ui::FxCard::DragPhase::Begin, { 4.0f, 4.0f });
	dragged->onCardDrag(ui::FxCard::DragPhase::Move, target);
	dragged->onCardDrag(ui::FxCard::DragPhase::End, target);
	const std::vector<DeviceInstanceId> moved = strip();
	REQUIRE(moved.size() == before.size());
	CHECK(moved[0] == d1);
	CHECK(moved[1] == d3);
	CHECK(f.root->onKeyPress().callback(visage::KeyEvent { visage::KeyCode::Z, visage::kModifierRegCtrl, true }));
	CHECK(strip() == before);
}

TEST_CASE("PN teardown: a strip selection of several outlives neither its devices nor its editor — a removal under it, "
          "an editor closed and reopened over it, and Esc afterwards all reach only living objects") {
	// THE LIFETIME HALF OF PN, FOR THE ASan GATE (`-L sanitize` runs this binary one process per case). The strip's
	// `LaneSelection` holds an entry on the editor's Esc stack while two or more are selected, and that entry's
	// close captures the selection; the selection holds a raw pointer to the stack. The steps: a selected device
	// removed by a road that is not the selection's own; the editor closed and reopened with the selection live
	// (MEASURED: a reopen keeps the tree — `Gui::create` returns early when nothing was built — so panel and stack
	// survive it together); Esc on the reopened editor; and the editor torn down by the fixture with a selection of
	// two live, where `Gui::Impl` destroys the panel before `surfaces` and the ticket is weak, so the stack never
	// calls into a dead selection. A green here under ASan is the claim; the value checks pin that the selection
	// was live at each step, so the case is not vacuous.
	ModFixture f;
	const size_t s1 = f.bridge.addRackDevice(mod::lfo::kTraits.tag);
	const size_t s2 = f.bridge.addRackDevice(mod::lfo::kTraits.tag);
	const size_t s3 = f.bridge.addRackDevice(mod::lfo::kTraits.tag);
	REQUIRE(s3 == s1 + 2);
	f.bridge.refresh();
	f.panel->resized();
	f.gui.tickForTest();

	const auto press = [](ui::ModPanel* panel, size_t slot, int modifiers) {
		ui::FxCard* card = panel->cardForTest(slot);
		REQUIRE(card != nullptr);
		card->onCardPressed();
		visage::MouseEvent e {};
		e.position = { card->width() * 0.5f, card->height() * 0.5f };
		e.modifiers = modifiers;
		e.repeat_click_count = 1;
		card->noteCardPress(e);
		card->noteCardRelease(e);
	};
	press(f.panel, s1, 0);
	press(f.panel, s3, visage::kModifierShift);
	REQUIRE(f.panel->selectionForTest().selection().size() == 3);
	REQUIRE(f.panel->selectionForTest().escapeHeldForTest());

	// A SELECTED DEVICE REMOVED UNDER THE SELECTION, by the bridge rather than the selection's verbs.
	REQUIRE(f.bridge.removeRackDevice(s2));
	f.bridge.refresh();
	f.gui.tickForTest();
	f.panel->resized();
	f.gui.tickForTest();
	CHECK(f.panel->selectionForTest().selection().size() == 2);
	CHECK(f.panel->selectionForTest().escapeHeldForTest());

	// THE EDITOR CLOSED WITH THE SELECTION LIVE, AND REOPENED (the same panel and stack come back).
	f.gui.destroy();
	REQUIRE(f.gui.create(CLAP_WINDOW_API_X11, false));
	f.gui.setPopoutWindowForTest(false);
	REQUIRE(f.gui.setSize(static_cast<uint32_t>(Gui::kLogicalWidth * f.scale),
	                      static_cast<uint32_t>(Gui::kLogicalHeight * f.scale)));
	visage::Frame* root = f.gui.root();
	REQUIRE(root != nullptr);
	ui::ModPanel* panel = findInTree<ui::ModPanel>(root);
	REQUIRE(panel != nullptr);
	CHECK(panel == f.panel); // the reopen kept the tree (see the banner)
	f.gui.tickForTest();
	panel->resized();
	f.gui.tickForTest();

	// A FRESH SELECTION OF TWO, THEN ESC ON THE REOPENED EDITOR: it closes that entry and nothing else.
	press(panel, s1, 0);
	press(panel, s1 + 1, visage::kModifierShift);
	REQUIRE(panel->selectionForTest().selection().size() == 2);
	REQUIRE(panel->selectionForTest().escapeHeldForTest());
	CHECK(root->onKeyPress().callback(visage::KeyEvent { visage::KeyCode::Escape, 0, true }));
	CHECK(panel->selectionForTest().selection().size() == 1);
	CHECK_FALSE(panel->selectionForTest().escapeHeldForTest());

	// AND CLOSED ONCE MORE WITH A SELECTION LIVE, LEFT FOR THE FIXTURE'S OWN TEARDOWN.
	press(panel, s1, 0);
	press(panel, s1 + 1, visage::kModifierShift);
	REQUIRE(panel->selectionForTest().escapeHeldForTest());
	f.gui.destroy();
}

TEST_CASE("sound cards on a drum kit (audition): all eight present, no add, clone or delete anywhere; the switch mutes the pad; the card's fader is the pad's Level") {
	StackFixture f;
	f.set(ParamId::OscType, static_cast<double>(OscType::Drum));
	f.set(ParamId::DrumMode, 1.0);
	REQUIRE(f.stack.kitMode());
	CHECK(f.stack.visibleCount() == feathers::kMaxSounds);
	// NO MAKE OR REMOVE GESTURE, anywhere on the rail: every point of every card, and below.
	int forbidden = 0;
	for (float y = 0.0f; y < f.stack.height(); y += 2.0f) {
		for (float x = 0.0f; x < f.stack.width(); x += 2.0f) {
			const SoundStack::Part part = f.stack.hitAt({ x, y }).part;
			forbidden += part == SoundStack::Part::NewSound || part == SoundStack::Part::Clone || part == SoundStack::Part::Delete
			                 ? 1
			                 : 0;
		}
	}
	CHECK(forbidden == 0);
	// THE SWITCH IS THE PAD'S MUTE (and leaves the sound's Enable alone).
	const double enable = f.read(static_cast<ParamId>(soundParam(2, SoundField::Enable)));
	f.press(f.switchPoint(2));
	f.settle();
	CHECK(f.read(static_cast<ParamId>(feathers::drumParam(2, DrumControl::Mute))) == 1.0);
	CHECK(f.read(static_cast<ParamId>(soundParam(2, SoundField::Enable))) == enable);
	f.press(f.switchPoint(2));
	f.settle();
	CHECK(f.read(static_cast<ParamId>(feathers::drumParam(2, DrumControl::Mute))) == 0.0);
	// THE CARD'S LEVEL FADER IS THE PAD'S LEVEL, the same param as the kit window's mirror and the plot's handle.
	(void)f.stack.updatePulses(0); // lays the faders out
	FaderPill* fader = f.stack.levelFaderForTest(4);
	REQUIRE(fader != nullptr);
	CHECK(fader->isVisible());
	fader->amountForTest().beginGesture();
	fader->amountForTest().setValue(-12.0);
	fader->amountForTest().endGesture();
	f.settle();
	CHECK(f.read(static_cast<ParamId>(feathers::drumParam(4, DrumControl::Level))) == doctest::Approx(-12.0));
	// PITCHED: the ordinary rail again (the faders hidden, a consumed card's delete back).
	f.set(ParamId::DrumMode, 0.0);
	(void)f.stack.updatePulses(0);
	CHECK_FALSE(fader->isVisible());
	CHECK(f.stack.visibleCount() < feathers::kMaxSounds);
	const size_t first = f.stack.visibleSlot(0);
	CHECK(f.stack.hitAt(f.deletePoint(first)).part == SoundStack::Part::Delete);
}

TEST_CASE("sound cards on a drum kit (audition): a pad's hit pulses its card alone, at its velocity, gone after 150 ms") {
	// THE FADE IS THE CLAIM, so reduced motion must be OFF whatever the caller's environment says: the
	// integrator round runs the GUI suites under FEATHERS_REDUCED_MOTION=1, where a pulse lights flat.
	const ScopedEnv motion("FEATHERS_REDUCED_MOTION", "0");
	StackFixture f;
	f.set(ParamId::OscType, static_cast<double>(OscType::Drum));
	f.set(ParamId::DrumMode, 1.0);
	(void)f.stack.updatePulses(1000);
	for (size_t pad = 0; pad < feathers::kMaxSounds; ++pad) {
		CHECK(f.stack.pulseOf(pad) == 0.0f);
	}
	feathers::PadHits hits;
	hits.count[3] = 1;
	hits.velocity[3] = 0.8f;
	f.bridge.publishPadHits(hits);
	CHECK(f.stack.updatePulses(1000));
	CHECK(f.stack.pulseOf(3) == doctest::Approx(0.8f));
	for (size_t pad = 0; pad < feathers::kMaxSounds; ++pad) {
		if (pad != 3) {
			CHECK(f.stack.pulseOf(pad) == 0.0f);
		}
	}
	(void)f.stack.updatePulses(1075);
	CHECK(f.stack.pulseOf(3) < 0.8f);
	CHECK(f.stack.pulseOf(3) > 0.0f);
	(void)f.stack.updatePulses(1150);
	CHECK(f.stack.pulseOf(3) == 0.0f);
	CHECK_FALSE(f.stack.updatePulses(1200)); // spent: no frame owed
}

TEST_CASE("sound cards on a drum kit (audition): under reduced motion a hit holds its card lit, flat, then goes out") {
	StackFixture f;
	f.set(ParamId::OscType, static_cast<double>(OscType::Drum));
	f.set(ParamId::DrumMode, 1.0);
	::setenv("FEATHERS_REDUCED_MOTION", "1", 1);
	feathers::PadHits hits;
	hits.count[1] = 1;
	hits.velocity[1] = 0.6f;
	f.bridge.publishPadHits(hits);
	(void)f.stack.updatePulses(500);
	(void)f.stack.updatePulses(600);
	const float held = f.stack.pulseOf(1);
	(void)f.stack.updatePulses(660);
	const float out = f.stack.pulseOf(1);
	::unsetenv("FEATHERS_REDUCED_MOTION");
	CHECK(held == doctest::Approx(0.6f)); // still at 100 ms: no fade
	CHECK(out == 0.0f);                   // and out after 150 ms
}
