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

// PIECE 7 OF 18 of the devices suite: its cases in their original order, with the helpers only they use. See devices_common.h.

#include "devices/devices_common.h"

TEST_CASE("the two fx rails say which lane they are, and neither promises a lane that is not there") {
	// **ITS WORDS ARE TRUE TO WHAT IS THERE, and this case exists because they once were not.** The
	// grain line read *"per-slice sends — not built yet, backlog EW" while the grain chain had been a
	// real `rack::InnerChain`, fed grain by grain, for a day. **A caption is a claim about what is on
	// the other side of it**, so the assertion below is a NEGATIVE one on the promise words, not only a
	// positive one on the name.
	const std::string grain = ui::FxLaneRail::kGrainLine;
	const std::string post = ui::FxLaneRail::kPostLine;
	for (const std::string& line : { grain, post }) {
		CAPTURE(line);
		CHECK(line.find("not built") == std::string::npos);
		CHECK(line.find("backlog") == std::string::npos);
		CHECK(line.find("EW") == std::string::npos);
		CHECK(line.find("yet") == std::string::npos);
	}

	// **THE LABELS NAME THE LANES; THE CAPTIONS SAY WHAT THE LAYOUT CANNOT.**
	//
	// The stacked pair had to state the signal order in words — *"before MASTER", *"after the grain
	// lane" — because POST was drawn ABOVE GRAIN while the signal runs the other way. Left to right,
	// GRAIN FX now sits before POST FX, so those clauses are the layout said twice and they are gone;
	// this asserts they are gone rather than leaving it to a reader to notice.
	CHECK(grain.find("before POST") == std::string::npos);
	CHECK(post.find("after the grain") == std::string::npos);

	// **WHAT ADJACENCY STILL CANNOT SAY, AND EACH CAPTION THEREFORE KEEPS.** Two rails side by side
	// read just as easily as two PARALLEL lanes, and the grain lane is a DIVERSION that rejoins (by
	// design) — a chosen grain LEAVES the wet, so an empty grain lane is
	// inaudible rather than a doubling. That is the one fact the arrangement cannot carry.
	CHECK(grain.find("rejoins") != std::string::npos);
	CHECK(post.find("grain lane") != std::string::npos);

	CHECK(std::string(ui::FxLaneRail::kGrainLabel).find("GRAIN") != std::string::npos);
	CHECK(std::string(ui::FxLaneRail::kPostLabel).find("POST") != std::string::npos);
	CHECK(grain != post);
}

namespace {
/// The tag of the first device this chain will take that the build can ALSO open a panel for. Named
/// rather than hard-coded, and distinct from `firstAddableTag`: the expand cases are about a device
/// with somewhere to go, and picking the first addable one would make them vacuous the day that
/// device's panel is the one without an entry.
const char* firstExpandableTag(feathers::ui::ChainColumn& column) {
	for (const device::Descriptor* d : column.addable()) {
		if (ui::hasExpandedView(*d)) {
			return d->traits.tag;
		}
	}
	return nullptr;
}
} // namespace

TEST_CASE("two columns over one chain: the expand mark is each column's own answer, and one column losing, releasing or being destroyed never takes it from a sibling that presents") {
	HeldChain held;
	ui::WidgetRegistry registry;
	ui::ModState mod;
	ui::EditHistory history;
	ui::SurfaceLayers outer;
	outer.setBounds(0, 0, 1200, 700);
	ui::ChainColumn a(held.bridge(), registry, mod, history, nullptr);
	a.setBounds(0, 0, static_cast<int>(ui::FxLaneRail::kWidth), 600);
	const char* tag = firstExpandableTag(a);
	REQUIRE(tag != nullptr);
	const DeviceInstanceId inner = a.addDevice(tag);
	REQUIRE(inner != feathers::kNoInstance);
	const auto markOf = [&](ui::ChainColumn& c) {
		ui::FxCard* card = c.cardOf(inner);
		REQUIRE(card != nullptr);
		return card->expandBounds().width();
	};
	a.setPresentedOn(&outer);
	CHECK(markOf(a) > 0.0f);
	// B IS BUILT WITHOUT A PRESENTER: its card has NO mark although A presents over the same chain (a mark that opens nothing is dead),
	// and A's mark is untouched by B's being built.
	{
		ui::ChainColumn b(held.bridge(), registry, mod, history, nullptr);
		b.setBounds(0, 0, static_cast<int>(ui::FxLaneRail::kWidth), 600);
		b.syncCards();
		CHECK(markOf(b) == 0.0f);
		CHECK(markOf(a) > 0.0f);
		// B GETS A PRESENTER, then loses it (nullptr): A keeps its mark through both.
		b.setPresentedOn(&outer);
		CHECK(markOf(b) > 0.0f);
		CHECK(markOf(a) > 0.0f);
		b.setPresentedOn(nullptr);
		CHECK(markOf(b) == 0.0f);
		CHECK(markOf(a) > 0.0f);
		// ...and B destroyed WHILE presenting: A keeps it.
		b.setPresentedOn(&outer);
		CHECK(markOf(b) > 0.0f);
	}
	CHECK(markOf(a) > 0.0f);
	CHECK(held.bridge().expandedPresentersForTest() == 1);
	// THE LAST PRESENTER GOING CLEARS THE ANSWER, so a column given one later still starts from nothing.
	a.setPresentedOn(nullptr);
	CHECK(markOf(a) == 0.0f);
	CHECK(held.bridge().expandedPresentersForTest() == 0);
}
TEST_CASE("a column outlives its bridge: with and without a presenter, destroying the bridge first and the column after touches nothing dead (clean under ASan)") {
	for (const bool presenting : { false, true }) {
		CAPTURE(presenting);
		ui::WidgetRegistry registry;
		ui::ModState mod;
		ui::EditHistory history;
		ui::SurfaceLayers outer;
		outer.setBounds(0, 0, 1200, 700);
		auto engine = std::make_unique<feathers::rack::RackEngine>(feathers::rack::kRackAxes, feathers::hostCatalogue());
		engine->prepare(48000.0, 4096);
		auto bridge = std::make_unique<feathers::rack::RackBridge>(*engine);
		bridge->prepare(48000.0, 4096);
		auto column = std::make_unique<ui::ChainColumn>(*bridge, registry, mod, history, nullptr);
		column->setBounds(0, 0, static_cast<int>(ui::FxLaneRail::kWidth), 600);
		if (presenting) {
			column->setPresentedOn(&outer);
			CHECK(bridge->expandedPresentersForTest() == 1);
		}
		// THE BRIDGE GOES FIRST (an inner chain's device freed while its panel still stands), then the column: its destructor releases the
		// presenter count and must not touch the bridge, and a late setPresentedOn(nullptr) likewise.
		bridge.reset();
		engine.reset();
		if (presenting) {
			column->setPresentedOn(nullptr);
		}
		column.reset();
		CHECK(true); // reaching here without a sanitizer report is the assertion
	}
}
TEST_CASE("a device in a held chain has a real way into a panel, and no mark at all when it has nowhere to go") {
	// **There was no expand button for a SKLITTER inside a SKLITTER.** The column's own comment defended that as the honesty rule — a mark
	// that opened nothing would be a control that does not work — and the defence was sound while the
	// premise held. This case pins BOTH halves, because a fix that only made the mark appear would have
	// turned a correct refusal into the thing it was refusing to be.
	//
	// **THE MECHANISM IS THE HOST'S ANSWER AND NOT A FLAG ON THE CARD**, which is why the mark can
	// come and go with no card rebuilt: `FxCard::expandBounds` is empty unless
	// `DeviceHost::hasExpandedViewFor` says yes, and `RackEngine` says no until somebody installs a
	// query. `setPresentedOn` is the one place that installs and clears it.
	HeldChain held;
	ui::WidgetRegistry registry;
	ui::ModState mod;
	ui::EditHistory history;
	ui::SurfaceLayers outer;
	outer.setBounds(0, 0, 1200, 700);

	const char* tag = nullptr;
	{
		ui::ChainColumn column(held.bridge(), registry, mod, history, nullptr);
		column.setBounds(0, 0, static_cast<int>(ui::FxLaneRail::kWidth), 600);
		tag = firstExpandableTag(column);
		REQUIRE(tag != nullptr);
		const DeviceInstanceId inner = column.addDevice(tag);
		REQUIRE(inner != feathers::kNoInstance);
		ui::FxCard* card = column.cardOf(inner);
		REQUIRE(card != nullptr);

		// **NOWHERE TO PRESENT: NO MARK, AND NO PANEL EVEN IF THE VERB IS DRIVEN DIRECTLY.** Two
		// assertions rather than one, because they fail for different reasons — the first is what a
		// hand sees, the second is what a hand could reach past it.
		CHECK(column.presentedOnForTest() == nullptr);
		CHECK(card->expandBounds().width() == 0.0f);
		column.expandDevice(inner);
		CHECK(column.expandedCountForTest() == 0);
		CHECK_FALSE(outer.overlay().open());

		// GIVEN ONE, THE MARK APPEARS ON THE CARD THAT WAS ALREADY BUILT — no rebuild, because the
		// card asks the host on every paint rather than caching the answer.
		column.setPresentedOn(&outer);
		CHECK(column.presentedOnForTest() == &outer);
		CHECK(card->expandBounds().width() > 0.0f);

		// **AND THE PANEL LANDS ON THE OUTER HOST, WHICH IS THE WHOLE POINT** — `2026-09-13-canvas.md`:
		// nesting escalates outward. A panel opened INTO this 300px column would be the clipping
		// container `surface_host.h` was written about.
		column.expandDevice(inner);
		CHECK(column.expandedCountForTest() == 1);
		CHECK(outer.overlay().open());
		CHECK(outer.overlay().panelCount() == 1);

		// THE PRESS ITSELF REACHES IT, not only the verb: the card takes head presses through
		// `noteCardPress`, and a mark drawn by one road and dispatched by another is a documented bug
		// in this very file's history (the spine's marks, painted down the left and pressed across
		// the top).
		const visage::Bounds mark = card->expandBounds();
		card->noteCardPress({ mark.x() + mark.width() * 0.5f, mark.y() + mark.height() * 0.5f }, 1);
		// A SECOND PRESS ON THE SAME CARD CLOSES ITS PANEL — and a third opens it
		// again, so the press is seen reaching the verb both ways rather than being lost.
		CHECK(column.expandedCountForTest() == 0);
		CHECK(outer.overlay().panelCount() == 0);
		card->noteCardPress({ mark.x() + mark.width() * 0.5f, mark.y() + mark.height() * 0.5f }, 1);
		CHECK(column.expandedCountForTest() == 1);
		CHECK(outer.overlay().panelCount() == 1);

		// **THE CARD AND ITS PANEL WEAR ONE COLOUR.** Not "both have a tint" — the same value, from
		// the same derivation, which is what makes the flare answer "which card opened this?".
		REQUIRE(outer.overlay().panelForTest() != nullptr);
		CHECK(outer.overlay().panelForTest()->tintForTest() == card->tintedHeadMarks().tint);
		CHECK(card->tintedHeadMarks().tint != 0);

		// TAKEN DOWN WITH THE PRESENTER, because a panel left on a host this column no longer answers
		// to is a panel nothing can dismiss.
		column.setPresentedOn(nullptr);
		CHECK(column.expandedCountForTest() == 0);
		CHECK_FALSE(outer.overlay().open());
		CHECK(card->expandBounds().width() == 0.0f);

		column.setPresentedOn(&outer);
		column.expandDevice(inner);
		REQUIRE(outer.overlay().open());
		// ...and the column now goes out of scope with a panel still up.
	}
	// **A PANEL OUTLIVES THE COLUMN UNLESS THE COLUMN TAKES IT DOWN, AND IT READS A BRIDGE THAT IS
	// GOING WITH IT.** This is the lifetime class `scripts/sanitize-gate.sh` exists for; the assertion
	// here is the cheap half, and the gate is the half that can see a read of freed memory.
	CHECK_FALSE(outer.overlay().open());
	CHECK(outer.presentedCountForTest() == 0);
}
TEST_CASE("a chain inside a panel tints its cards and their panels for ITS chain, because an instance id is not unique across nesting") {
	// **THE BUG UNDERNEATH THE COLOUR, AND IT IS NOT A COLOUR BUG.** `backplane/device_instance_id.h`
	// records that *"each of Feathers' chains counts separately from `kFirstInstance`", and
	// `rack::InnerChain` owns a whole `RackEngine` with an `InstanceCounter` of its own — so the first
	// device inside SKLITTER's post chain is instance 1, and so is the first device of some other
	// chain. `ExpandedOverlay::keyOf` was `title#device` and the layer keeps ONE PANEL PER KEY, so two
	// different devices could share a key and the second opened would RETIRE the first.
	//
	// **SO THE CLAIM HERE IS ABOUT VALUES, NOT ABOUT A DRAWING** (CLAUDE.md's rule, paid for four
	// times): the nested card's tint is computed from the nested chain's own nest, and it is NOT the
	// tint the same id would get at the top level. A case asserting only "the two differ from each
	// other" would pass with the nest wired to anything at all.
	// **THE OUTER HOST IS DECLARED FIRST AND THAT IS NOT TIDINESS.** Members and locals go in reverse,
	// and a host must outlive what it is hosting: the panel's chain takes its own panels down through
	// this object on the way out. Declared the other way round the case SEGFAULTS — which is a real
	// fact about the arrangement and is recorded in the report rather than hidden by the fix.
	ui::SurfaceLayers outer;
	outer.setBounds(0, 0, 1400, 900);
	PerformPanel p;
	REQUIRE(p.panel != nullptr);

	// THE PANEL IS TOLD WHERE IT IS, as `SurfaceLayers::mount` tells it in the editor. That one call is
	// the whole road: it reaches the rail, and the rail reaches the column.
	p.panel->setPresentedOn(&outer);
	setFolds(p, /*bodyOpen=*/true, /*circleOpen=*/false, /*fxOpen=*/true);

	HeldChain held;
	ui::FxLaneRail& rail = p.panel->postRailForTest();
	REQUIRE(rail.follow(held.surface()));
	ui::ChainColumn* column = rail.columnForTest();
	REQUIRE(column != nullptr);

	// **THE COLUMN'S NEST IS THE PANEL'S OWN, ONE LEVEL DEEPER** — computed from the SKLITTER instance
	// the panel is about, so two SKLITTERs' chains are two nests.
	CHECK(column->nest() == ui::DeviceNest::under({}, p.device));
	CHECK_FALSE(column->nest().top());

	const char* tag = firstExpandableTag(*column);
	REQUIRE(tag != nullptr);
	const DeviceInstanceId inner = column->addDevice(tag);
	REQUIRE(inner != feathers::kNoInstance);
	ui::FxCard* card = column->cardOf(inner);
	REQUIRE(card != nullptr);

	// **THE VALUE, COMPUTED BY AN INDEPENDENT RESTATEMENT OF THE INTENDED PATH** rather than read back
	// out of the thing under test.
	const uint32_t want = ui::instanceTintFor(inner, ui::DeviceNest::under({}, p.device));
	CHECK(card->tintedHeadMarks().tint == want);
	// AND THE SECOND ASSERTION THAT PROVES THE FIRST IS NOT VACUOUS: the nest actually MOVED the hue
	// off the one this id would wear in the host's own chain. Without it, `want` could be the default
	// and the check above would pass with the whole mechanism deleted.
	CHECK(card->tintedHeadMarks().tint != ui::instanceTintFor(inner));

	// **AND THE CHAIN NEVER WEARS ITS HOLDER'S OWN HUE** — the guarantee `DeviceNest::under` is built
	// to give, and the exact pair a hand has on screen when it opens a panel from inside a panel.
	CHECK(card->tintedHeadMarks().tint != ui::instanceTintFor(p.device));

	// THE PANEL IT OPENS CARRIES THE SAME PAIR OUT TO THE OUTER LAYER — through a `MountedFrame`, which
	// answers for the mounted content rather than for itself. That delegation is the line this case
	// would have missed: the card was right and the panel was wrong, one capture apart.
	column->expandDevice(inner);
	REQUIRE(outer.overlay().panelForTest() != nullptr);
	CHECK(outer.overlay().panelForTest()->tintForTest() == want);
}
TEST_CASE("the editor's own teardown, with a panel open from inside a panel: the outer host dies holding both") {
	// **THE ORDER THE EDITOR ACTUALLY DIES IN, WHICH IS NOT THE ORDER A FIXTURE FALLS INTO.** In the
	// plugin the SKLITTER panel is INSIDE the outer `SurfaceLayers` — a child of its overlay's slot —
	// so tearing the layers down destroys the panel, which destroys the chain column, which asks those
	// same layers to take its nested panel down. The call lands on a host that is mid-destruction.
	//
	// **THE CASE ABOVE PROVES THE OTHER ORDER IS FATAL AND THAT ORDER CANNOT HAPPEN HERE** — with the
	// outer declared after the panel it hosts, `~ChainColumn` dismisses through a pointer to a freed
	// host and segfaults (MEASURED: 20 assertions, then SIGSEGV). That is a fact about fixtures, not
	// about the editor, and this case is the one that asks the editor's question.
	//
	// **THE ASSERTION IS SURVIVAL, WHICH IS WEAK, SO THE SANITIZER GATE IS THE OTHER HALF**: reading
	// freed memory does not fault, and `ctest` structurally cannot see this class (CLAUDE.md).
	HeldChain held;
	ModFixture f;
	ui::WidgetRegistry registry;
	namespace sc = feathers::fx::sklitter;
	const size_t slot = f.bridge.addFxDevice(sc::kTraits.tag);
	const DeviceInstanceId device = f.bridge.fxDeviceAt(slot);
	// THE HANDLE A HOST WOULD FILE FOR THE RUNNING DEVICE — `widget-shot` supplies the same one for the
	// same reason: a fixture has no `Synth`, so nothing is ever provisioned and the rail would honestly
	// report "not running here".
	f.bridge.fxBodies().attach(device, held.surface());

	{
		ui::SurfaceLayers outer;
		outer.setBounds(0, 0, 1400, 900);
		std::unique_ptr<ui::ExpandedView> view =
		    ui::makeExpandedView(f.deviceHost, &f.bridge, registry, device, sc::kDescriptor);
		REQUIRE(view != nullptr);
		auto* panel = dynamic_cast<ui::SklitterExpanded*>(view.get());
		REQUIRE(panel != nullptr);
		panel->postRailForTest().setCollapsed(false);
		// PRESENTED, NOT HELD — this is what puts the panel inside the host, which is the whole point.
		REQUIRE(outer.present(std::move(view), ui::SurfaceNeed::SpansGesture, "SKLITTER", {})
		        != ui::kNoSurface);
		// THE TICK IS WHAT BINDS THE CHAIN, through the panel's own road.
		(void)outer.updateContent();
		ui::ChainColumn* column = panel->postRailForTest().columnForTest();
		REQUIRE(column != nullptr);
		CHECK(column->presentedOnForTest() != nullptr); // the mount told it where it is

		const char* tag = firstExpandableTag(*column);
		REQUIRE(tag != nullptr);
		const DeviceInstanceId inner = column->addDevice(tag);
		REQUIRE(inner != feathers::kNoInstance);
		column->expandDevice(inner);
		// **BOTH PANELS ON ONE HOST — the nested one did not replace the one it came from.** That is
		// the escalation working AND the key being (chain, device) rather than device alone.
		CHECK(outer.overlay().panelCount() == 2);
		CHECK(column->expandedCountForTest() == 1);
	}
}
TEST_CASE("the THREE rails open and close INDEPENDENTLY, keep their strip when closed, and sit "
          "grid | PERFORM | GRAIN FX | POST FX") {
	// **INDEPENDENTLY NEEDS NO NEW CONCEPT** — `collapsed_` is per-`Rail` and `railsWidth()` sums
	// `railWidth()`, so nothing had to be written for the third rail to inherit it. Asserted across
	// all EIGHT states rather than at the obvious ones; four of them did not exist an hour ago.
	//
	// **AND THE ORDER IS A SPEC RATHER THAN A PREFERENCE.** Grain fx sits to the left of post fx,
	// which puts the panel at grid | PERFORM | GRAIN FX | POST FX — and that is also the
	// order the signal runs in, since the grain lane rejoins the wet before the post lane sees it.
	PerformPanel p;
	REQUIRE(p.panel != nullptr);
	ui::CircleRail& circle = p.panel->circleRailForTest();
	ui::FxLaneRail& grain = p.panel->grainRailForTest();
	ui::FxLaneRail& post = p.panel->postRailForTest();

	for (const bool wantCircle : { false, true }) {
		for (const bool wantGrain : { false, true }) {
			for (const bool wantPost : { false, true }) {
				CAPTURE(wantCircle);
				CAPTURE(wantGrain);
				CAPTURE(wantPost);
				setLanes(p, wantCircle, wantGrain, wantPost);
				CHECK(circle.collapsed() == !wantCircle);
				CHECK(grain.collapsed() == !wantGrain);
				CHECK(post.collapsed() == !wantPost);

				// **MINIMISED, NEVER GONE** — `rail.h`'s rule: a closed rail still costs its 34px strip,
				// because the toggle that closed it has to be there to open it.
				CHECK(circle.railWidth()
				      == (wantCircle ? ui::CircleRail::kWidth : ui::Rail::kCollapsedWidth));
				CHECK(grain.railWidth()
				      == (wantGrain ? ui::FxLaneRail::kWidth : ui::Rail::kCollapsedWidth));
				CHECK(post.railWidth()
				      == (wantPost ? ui::FxLaneRail::kWidth : ui::Rail::kCollapsedWidth));
				// AND THE PANEL GAVE EACH ONE THAT WIDTH, which is the half `railWidth()` alone cannot
				// promise: a rail that announced 34 and was laid out at 0 would pass the lines above.
				CHECK(static_cast<float>(circle.width()) == doctest::Approx(circle.railWidth()));
				CHECK(static_cast<float>(grain.width()) == doctest::Approx(grain.railWidth()));
				CHECK(static_cast<float>(post.width()) == doctest::Approx(post.railWidth()));
				CHECK(static_cast<float>(circle.height()) == doctest::Approx(p.panel->height()));

				// THE ORDER, AND THAT NONE OF THE THREE OVERLAP: PERFORM inboard of GRAIN FX, GRAIN FX
				// inboard of POST FX, POST FX against the panel's right edge, nothing between any of
				// them. Three rails placed from one running edge, so a slip stacks one on another.
				CHECK(circle.x() + circle.width() == grain.x());
				CHECK(grain.x() + grain.width() == post.x());
				CHECK(post.x() + post.width() == p.panel->width());
				CHECK(p.panel->railsWidth()
				      == doctest::Approx(circle.railWidth() + grain.railWidth() + post.railWidth()));
			}
		}
	}

	// **A CLOSED RAIL'S CONTENTS ARE HIDDEN, NOT MERELY UNDRAWN.** `Rail::collapsedChanged` is what
	// does it and the reason is its own: a control left visible inside a 34px strip answers a click
	// meant for whatever is drawn over it now.
	setRails(p, false, false);
	for (visage::Frame* child : circle.children()) {
		CHECK_FALSE(child->isVisible());
	}
	setRails(p, true, false);
	bool anyVisible = false;
	for (visage::Frame* child : circle.children()) {
		anyVisible = anyVisible || child->isVisible();
	}
	CHECK(anyVisible); // not vacuous: the rail really does hold something
}
TEST_CASE("SKLITTER's seven perform outputs are REAL ARMS along the bottom of the pane, and they fit "
          "the rows it reserves at its narrowest") {
	// SKLITTER's mod outputs, once only on the card, are duplicated on the Perform pane, along the
	// bottom of the pane.
	//
	// **THE HONESTY RULE IS THE WHOLE OF THIS CASE.** A chip that is drawn must ARM and must ROUTE. So
	// nothing below reads a drawing: it reads the widgets the rail built, the `device::Source` each one
	// answers, and the `ModState` a press moves. `source_arm.h`'s own banner records that Feathers once
	// PAINTED chips into a canvas instead — two implementations of "point this source at something",
	// one of which vanished under a card — and this rail is the fourth host to use the real widget.
	namespace sc = feathers::fx::sklitter;
	PerformPanel p;
	REQUIRE(p.panel != nullptr);
	ui::CircleRail& circle = p.panel->circleRailForTest();

	// **NO STATE, NO ARMS** — the rail is handed its `ModState` by the panel, and until then a chip
	// would be an arrow over nothing. `PerformPanel` does not call `setModulation`, so this is the
	// honest starting state rather than one arranged for the case.
	for (const std::unique_ptr<ui::SourceArm>& arm : circle.outputArmsForTest()) {
		CHECK(arm == nullptr);
	}

	// **THROUGH THE FIXTURE'S OWN OUTER STATE** — see `PerformPanel`: the pair has to outlive the
	// panel, and a case that declares its own gets that wrong exactly as this one once did.
	p.armFrom();
	setRails(p, /*circleOpen=*/true, /*fxOpen=*/false);

	// SEVEN, ONE PER DECLARED OUTPUT, IN DECLARED ORDER.
	REQUIRE(circle.outputArmsForTest().size() == sc::kPerformOutputs);
	for (const std::unique_ptr<ui::SourceArm>& arm : circle.outputArmsForTest()) {
		REQUIRE(arm != nullptr);
	}

	// **THEY FIT THE ROWS THE PANE RESERVES AT ITS DESIGN WIDTH — asked of the FONT, not of a drawing.**
	// `kMinOutputRows` is a chosen constant with a stated job (`kCollapsedLabelRun`'s arrangement one
	// widget over), and this is the job: it is the design width's count, the floor on the computed
	// count and the one figure `kOpenMinimumHeight` is summed from. **EXACTLY, not at most (2026-09-22)**:
	// a floor above the real count would reserve an empty row at 392 and lower the disk for nothing.
	// (This used to read "at its narrowest" — `kWidth` stopped being the narrowest the day the circle
	// learned to yield, and the yielded widths are the sweep case's.)
	const size_t rows =
	    ui::outputChipRowsFor(sc::kDescriptor, ui::CircleRail::kWidth - 2.0f * ui::CircleRail::kPadX);
	MESSAGE("seven perform outputs need " << rows << " row(s) at the pane's design width "
	                                      << ui::CircleRail::kWidth);
	CHECK(rows >= 1u); // not vacuous: the font answered with something
	CHECK(rows == ui::CircleRail::kMinOutputRows);
	CHECK(ui::CircleRail::outputRowsFor(ui::CircleRail::kWidth) == ui::CircleRail::kMinOutputRows);

	// **AND EVERY CHIP IS INSIDE THE STRIP, read off the APPLIED rectangles.** A rail that computed a
	// strip and placed its arms somewhere else would pass the row count above.
	const visage::Bounds strip = circle.outputStripBounds();
	CHECK(strip.bottom() <= static_cast<float>(circle.height()));
	for (const std::unique_ptr<ui::SourceArm>& arm : circle.outputArmsForTest()) {
		CAPTURE(arm->x());
		CAPTURE(arm->y());
		CHECK(static_cast<float>(arm->x()) >= strip.x());
		CHECK(static_cast<float>(arm->x() + arm->width()) <= strip.right() + 0.5f);
		CHECK(static_cast<float>(arm->y()) >= strip.y());
		CHECK(static_cast<float>(arm->y() + arm->height()) <= strip.bottom() + 0.5f);
		CHECK(arm->width() > 0); // a zero-width arm is hidden by every host's own rule
	}

	// **THEY READ IN DECLARED ORDER, LEFT TO RIGHT AND THEN DOWN.** Not a nicety: the chips are told
	// apart only by their labels, so a strip that put `Perf X` at the right-hand end would be correct
	// chip by chip and read as a different device.
	//
	// **THIS ASSERTION WAS THE OTHER WAY ROUND FOR ONE AFTERNOON, AND THE STORY IS WORTH THE LINES.**
	// Written against a base before `76a6d068`, it MEASURED the chips running right to left (output 0
	// at x = 307.2, output 3 at x = 52.8) and pinned that as the shared walk's contract. It was not a
	// contract: it was the badly-formatted-mod-outputs defect, fixed on mainline
	// while this branch was in flight, and the pin would have asked the next reader to preserve it.
	// **A MEASUREMENT IS ABOUT THE TREE YOU ARE ON, AND SAYING "measured" DOES NOT MAKE IT ABOUT THE
	// TREE** — the number was right and the conclusion was about my base.
	//
	// The rows run DOWNWARD in declared order, which was never in doubt.
	const auto& arms = circle.outputArmsForTest();
	for (size_t n = 1; n < arms.size(); ++n) {
		CAPTURE(n);
		CHECK(arms[n]->y() >= arms[n - 1]->y());
		if (arms[n]->y() == arms[n - 1]->y()) {
			CHECK(arms[n]->x() > arms[n - 1]->x()); // left to right, since 76a6d068
		}
	}
	// AND THE SEVEN REALLY DID LAND ON TWO ROWS rather than all on one, which is what makes the
	// wrapping claim above more than arithmetic.
	CHECK(arms.back()->y() > arms.front()->y());

	// **AND THE STRIP DOES NOT SIT ON THE DISK.** `blockSideFor` takes `outputBandFor` out of the room
	// the block is sized from AND `resized()` takes it out of the room the block is centred in; miss
	// either and the circle rides down over the chips with every size assertion still green.
	CHECK(strip.y() >= static_cast<float>(circle.pad().y() + circle.pad().height()));
}
TEST_CASE("an output chip on the perform pane and the same one on the card are TWO VIEWS OF ONE ARM") {
	// **"DUPLICATE", NOT "MOVE" — and the question that answers is whether two surfaces can fight.**
	// They cannot, and it is structural rather than careful: `gui.cpp` hands ONE `ModState`
	// (`&impl_->mod`) to the fx lane and to every expanded view, and `ModState` holds ONE focused
	// source and ONE armed flag. So arming from either surface is the same arm.
	//
	// **ASSERTED AS THE SOURCE AND THE STATE, never as two drawings** (CLAUDE.md's oldest rule): that
	// two chips look alike is not evidence that they name the same endpoint.
	namespace sc = feathers::fx::sklitter;
	PerformPanel p;
	REQUIRE(p.panel != nullptr);
	ui::CircleRail& circle = p.panel->circleRailForTest();
	p.armFrom();
	setRails(p, /*circleOpen=*/true, /*fxOpen=*/false);

	// **THE CHIP IS PRESSED AND THE STATE IS READ — and a first draft of this case did NOT do that.**
	// It set `outer.focus(want)` itself and then checked `outer.source() == want`, which asserts that
	// `ModState` works and says NOTHING about the widget: every line passed with the arms built over
	// the wrong output, or over no output at all. That is the vacuous-assertion shape CLAUDE.md keeps
	// a rule for, arriving in a case written to check an honesty claim.
	//
	// Pressing is the road a hand takes (`SourceArm`'s own `onMouseDown`), and what it leaves in the
	// shared state is the arm's own answer to "which source am I".
	const auto press = [](ui::SourceArm& arm) {
		visage::MouseEvent e {};
		e.button_id = visage::kMouseButtonLeft;
		e.button_state = visage::kMouseButtonLeft;
		e.repeat_click_count = 1;
		e.is_down = true;
		e.position = { static_cast<float>(arm.width()) * 0.5f, static_cast<float>(arm.height()) * 0.5f };
		arm.onMouseDown().callback(e);
	};

	// **THE ENDPOINT IS THE CARD'S OWN**: `Source::output(device, n)` — the same construction
	// `FxLane::sourceOfOutput` makes for a chip on the card, over the panel's own device.
	for (size_t n = 0; n < sc::kPerformOutputs; ++n) {
		CAPTURE(n);
		ui::SourceArm* arm = circle.outputArmsForTest()[n].get();
		REQUIRE(arm != nullptr);
		press(*arm);
		// ARMED, AND POINTING AT THIS OUTPUT — read out of the state the card's chips share.
		CHECK(p.outer.armed());
		CHECK(p.outer.source() == backplane::Source::output(p.device, n));
	}

	// **AND PRESSING THE ARMED ONE AGAIN DISARMS** — `SourceArm`'s sticky-and-exclusive contract, which
	// is what makes two chips over one source two views rather than two switches. The last press above
	// left output 6 armed.
	ui::SourceArm* last = circle.outputArmsForTest()[sc::kPerformOutputs - 1].get();
	REQUIRE(last != nullptr);
	press(*last);
	CHECK_FALSE(p.outer.armed());
}
TEST_CASE("the CIRCLE yields the deficit and the GRID never does — a panel narrower than its minimum "
          "keeps every knob whole") {
	// **THE DEFECT THIS EXISTS FOR WAS PHOTOGRAPHED, AND IT IS WHY AN INSTALL WAS CANCELLED.** With
	// body + circle + both fx rails open at 1240, the grid's bottom row read
	// `SEND OVERRIDE · PERF X · PERF Y · PE…` — **Perf Depth sliced in half at the body's right edge**,
	// with a second control cut beside `Shuffle`. `resized()` placed the rails from the right edge at
	// full width and gave the body the remainder with NO FLOOR, so the whole shortfall came off the
	// grid (backlog HB: the last thing placed is the residual, and it absorbs every error in the sum).
	//
	// **THAT IS THE HONESTY RULE'S OWN CASE, NOT A COSMETIC CLIP.** Perf Depth exists, is enabled,
	// changes sound through a real param, and could not be read or reached. It is also the outcome
	// `minimumSize`'s own comment records as REJECTED when window-grows was chosen over
	// grid-compresses — "the grid keeps its knobs at full size throughout" — arriving by a road
	// nobody was watching.
	//
	// **THE DECISION IS THAT THE CIRCLE GIVES**: it shrinks when both lanes are open, and only if the
	// window would otherwise clip off the screen. Both halves are asserted here: the
	// grid keeps its ask, and the circle is what pays for it.
	PerformPanel p;
	REQUIRE(p.panel != nullptr);
	setRails(p, /*circleOpen=*/true, /*fxOpen=*/true);
	REQUIRE(p.panel->bodyRailForTest() != nullptr);

	// THE GRID'S OWN ASK, read as the panel's minimum less what the rails want. This is the number the
	// body must never be given less than.
	const float gridAsk = p.panel->minimumSize().x - p.panel->railsWidth();
	REQUIRE(gridAsk > 0.0f);
	const float ask = p.panel->minimumSize().x;

	// **STARVED BY 80px — the measured deficit of the all-open state at a 1240 layer.** Driven as a
	// layout rather than as a constant, so this is the road the overlay takes when it clamps.
	constexpr float kStarve = 80.0f;
	p.panel->setBounds(0, 0, static_cast<int>(ask - kStarve), static_cast<int>(p.panel->minimumSize().y));
	p.panel->resized();

	CHECK(p.panel->widthDeficit() == doctest::Approx(kStarve));
	CHECK(p.panel->surplusWidth() == doctest::Approx(0.0f)); // the twin: never both at once
	// **THE CIRCLE PAID ALL OF IT.**
	CHECK(p.panel->circleWidth() == doctest::Approx(ui::CircleRail::kWidth - kStarve));
	CHECK(static_cast<float>(p.panel->circleRailForTest().width())
	      == doctest::Approx(ui::CircleRail::kWidth - kStarve));
	// **AND THE GRID PAID NONE OF IT — the assertion the photograph is about.**
	CHECK(static_cast<float>(p.panel->bodyRailForTest()->width()) >= gridAsk);
	// AND THE OTHER TWO RAILS KEPT THEIR ASK: the circle is the elastic one, not "whoever is last".
	CHECK(static_cast<float>(p.panel->grainRailForTest().width())
	      == doctest::Approx(ui::FxLaneRail::kWidth));
	CHECK(static_cast<float>(p.panel->postRailForTest().width())
	      == doctest::Approx(ui::FxLaneRail::kWidth));
	// AND NOTHING OVERLAPS — four columns still tile the panel they were squeezed into.
	CHECK(static_cast<float>(p.panel->circleRailForTest().x())
	      == doctest::Approx(static_cast<float>(p.panel->bodyRailForTest()->width())));
	CHECK(static_cast<float>(p.panel->postRailForTest().x() + p.panel->postRailForTest().width())
	      == doctest::Approx(static_cast<float>(p.panel->width())));

	// **IT DOES NOT OSCILLATE, AND THAT IS THE WHOLE REASON IT IS A CLAMP AND NOT A PREDICATE.**
	// Written as "shrink if it clips" this would flip-flop: it clips, so the circle shrinks, so it no
	// longer clips, so it grows. Laying out twice at the SAME width must give the same answer, because
	// the deficit is computed from the rails' BASE widths and cannot see the yield it caused.
	const float once = p.panel->circleWidth();
	p.panel->resized();
	p.panel->resized();
	CHECK(p.panel->circleWidth() == doctest::Approx(once));

	// **AND THE SURPLUS DIRECTION IS UNTOUCHED** — the whole Perform circle scales as large as the
	// window allows. The yield refines it, it does
	// not reverse it, and a case that only drove the deficit would let the growth rot.
	p.panel->setBounds(0, 0, static_cast<int>(ask + 300.0f), static_cast<int>(p.panel->minimumSize().y));
	p.panel->resized();
	CHECK(p.panel->widthDeficit() == doctest::Approx(0.0f));
	CHECK(p.panel->circleWidth() == doctest::Approx(ui::CircleRail::kWidth + 300.0f));
}
TEST_CASE("at the yield floor the circle stops giving, and the panel clips again — reached on purpose") {
	// **A FLOOR NOBODY CAN REACH IS A FLOOR NOBODY HAS CHECKED.** The circle can give back exactly what
	// the scaling work gave it — down to `kYieldFloorWidth` (266 until backlog IG moved it to 220, the
	// room the default editor leaves with every panel open). Past that the deficit falls through to the
	// body and the grid clips, exactly as it did before any of this.
	//
	// **THAT IS A REAL STATE AND IT IS HONEST TO SAY SO.** The alternative — letting the circle
	// collapse to nothing to protect the grid — trades a clipped knob for a disk too small to play,
	// which is the instrument this device is FOR. So the floor holds and the failure stays visible.
	PerformPanel p;
	REQUIRE(p.panel != nullptr);
	setRails(p, /*circleOpen=*/true, /*fxOpen=*/true);
	const float ask = p.panel->minimumSize().x;
	const float capacity = ui::CircleRail::kWidth - ui::CircleRail::kYieldFloorWidth;
	REQUIRE(capacity > 0.0f);

	// STARVED PAST WHAT THE CIRCLE HAS TO GIVE.
	const float starve = capacity + 40.0f;
	p.panel->setBounds(0, 0, static_cast<int>(ask - starve), static_cast<int>(p.panel->minimumSize().y));
	p.panel->resized();

	CHECK(p.panel->widthDeficit() == doctest::Approx(starve));
	// THE CIRCLE STOPPED AT THE FLOOR rather than going below it or going negative.
	CHECK(p.panel->circleWidth() == doctest::Approx(ui::CircleRail::kYieldFloorWidth));
	CHECK(static_cast<float>(p.panel->circleRailForTest().width())
	      == doctest::Approx(ui::CircleRail::kYieldFloorWidth));

	// **AND THE DISK SHRANK WITH IT RATHER THAN SLIDING OFF THE RAIL, which is what `blockSideFor`'s
	// floor moving is for.** At `kBlockHeight` the block would have stayed 372 in a 246 band and the
	// disk would have hung off the left edge at [-22.2, 288.2] — MEASURED before the change. The disk
	// is now inside its own rail, which is the property that was never checked because nothing could
	// reach it.
	ui::PerformPad& pad = p.panel->padForTest();
	CHECK(static_cast<float>(pad.x()) >= 0.0f);
	CHECK(static_cast<float>(pad.x() + pad.width())
	      <= static_cast<float>(p.panel->circleRailForTest().width()) + 0.5f);
	// **THE FLOOR MOVED 266 → 220 (backlog IG, design option (A), 2026-09-22)** and stopped being the
	// pre-scaling disk (132.25). It is now the room the default editor leaves with every panel open once
	// SKLITTER's grid gained its side column. A SQUARE room that wide holds a disk of 67.2, which is what
	// the rail drew there until backlog IR — pinned so the arithmetic cannot drift.
	CHECK(ui::CircleRail::kYieldFloorWidth == doctest::Approx(220.0f));
	CHECK(ui::CircleRail::diskFor(ui::CircleRail::kBlockFloor) == doctest::Approx(67.196f).epsilon(0.001));
	// **SINCE IR THE ROOM IS NOT CUT SQUARE**, and a tall one lets the cells move up and down out of the
	// disk's way: the disk is the whole width inside the padding, 200 against 67.2.
	CHECK(static_cast<float>(pad.width())
	      == doctest::Approx(expectedOptimumDisk(static_cast<float>(p.panel->circleRailForTest().width()),
	                                             static_cast<float>(p.panel->circleRailForTest().height()))));
	CHECK(static_cast<float>(pad.width()) == doctest::Approx(ui::CircleRail::kBlockFloor));
}
TEST_CASE("backlog HX: the output strip grows by whole rows as the circle yields — all seven perform "
          "outputs stay placed at the yield floor, and the design width moves nothing") {
	// With every panel expanded, some of Perform's mod outputs were truncated.
	//
	// **THE MECHANISM WAS A CONSTANT WEARING THE CLOTHES OF A MEASUREMENT.** `kOutputRows = 2` was
	// measured — at 392, the only width the rail ever had when it was written. The circle then learned
	// to YIELD width (both fx lanes open and the window would clip), down to 266, where the same seven
	// chips wrap onto more rows; and `layoutOutputChips` HIDES a row past the strip's height, by the
	// card's rule. So the tail of the outputs — `Perf Speed` first, then the Weights — vanished, and
	// `layOutOutputArms`' own comment said "nothing here hides a chip that does not fit". True of the
	// field at 392, false of the effect at 266: CLAUDE.md's commonest wrong claim.
	//
	// **THE FIX IS THAT THE COUNT COMES FROM THE WIDTH THE RAIL HAS**, through the same walk that
	// places the chips (`outputChipRowsFor`), and the CIRCLE gives up the height — the strip is the
	// fixture of the pane and the circle the thing that scales. Everything below is read off the
	// APPLIED widgets after a layout driven down the road `widthDeficit()` takes, never off the
	// constants.
	namespace sc = feathers::fx::sklitter;
	PerformPanel p;
	REQUIRE(p.panel != nullptr);
	ui::CircleRail& circle = p.panel->circleRailForTest();
	p.armFrom();
	setRails(p, /*circleOpen=*/true, /*fxOpen=*/true);
	const auto& arms = circle.outputArmsForTest();
	REQUIRE(arms.size() == sc::kPerformOutputs);
	for (const std::unique_ptr<ui::SourceArm>& arm : arms) {
		REQUIRE(arm != nullptr);
	}

	const float ask = p.panel->minimumSize().x;
	const float tall = p.panel->minimumSize().y;
	// **DRIVEN AS A LAYOUT, THE WAY THE OVERLAY CLAMPS** — the panel is given less than its ask and
	// `circleWidth()` yields the difference; the rail's width is what falls out, not what is set.
	const auto layOutRailAt = [&](float railWidth) {
		const float starve = ui::CircleRail::kWidth - railWidth;
		p.panel->setBounds(0, 0, static_cast<int>(ask - starve), static_cast<int>(tall));
		p.panel->resized();
		REQUIRE(static_cast<float>(circle.width()) == doctest::Approx(railWidth));
	};

	/// EVERY ONE OF THE SEVEN: placed, inside the rail, inside the strip, over no other, and the strip
	/// as many rows tall as the walk's own count says this width needs.
	const auto checkAllSevenPlaced = [&] {
		const float railWide = static_cast<float>(circle.width());
		const visage::Bounds strip = circle.outputStripBounds();
		// **THE ROW COUNT IS THE WALK'S** — a count and a placement from one function cannot disagree,
		// which is the whole design; this pins that the rail really asked it rather than a constant.
		const size_t rows = ui::outputChipRowsFor(sc::kDescriptor, strip.width());
		CAPTURE(railWide);
		CAPTURE(rows);
		CHECK(ui::CircleRail::outputRowsFor(railWide) == std::max(rows, ui::CircleRail::kMinOutputRows));
		CHECK(strip.height()
		      == doctest::Approx(static_cast<float>(ui::CircleRail::outputRowsFor(railWide))
		                         * ui::kCardOutputStripHeight));
		CHECK(strip.bottom() <= static_cast<float>(circle.height()) + 0.5f);
		for (size_t n = 0; n < arms.size(); ++n) {
			CAPTURE(n);
			INFO("output := " << (sc::kOutputs[n].name != nullptr ? sc::kOutputs[n].name : ""));
			const ui::SourceArm& arm = *arms[n];
			// **PLACED** — an empty rectangle is how the walk says "hidden", and every host turns it
			// into `setVisible(false)`. This is the assertion the defect fails: `Perf Speed` at 266.
			CHECK(arm.width() > 0);
			CHECK(arm.height() > 0);
			CHECK(arm.isVisible());
			// INSIDE THE RAIL AND INSIDE THE STRIP.
			CHECK(static_cast<float>(arm.x()) >= 0.0f);
			CHECK(static_cast<float>(arm.x() + arm.width()) <= railWide + 0.5f);
			CHECK(static_cast<float>(arm.x()) >= strip.x() - 0.5f);
			CHECK(static_cast<float>(arm.x() + arm.width()) <= strip.right() + 0.5f);
			CHECK(static_cast<float>(arm.y()) >= strip.y() - 0.5f);
			CHECK(static_cast<float>(arm.y() + arm.height()) <= strip.bottom() + 0.5f);
			// OVER NO OTHER — two chips on one spot would read as one device with six outputs.
			for (size_t m = 0; m < n; ++m) {
				CAPTURE(m);
				const ui::SourceArm& other = *arms[m];
				const bool apart = arm.x() >= other.x() + other.width()
				                   || other.x() >= arm.x() + arm.width()
				                   || arm.y() >= other.y() + other.height()
				                   || other.y() >= arm.y() + arm.height();
				CHECK(apart);
			}
		}
		// AND THE STRIP DOES NOT SIT ON THE DISK — `resized()` centres the block in the room the TALLER
		// band leaves, or the circle rides down over the new row with every count above still right.
		CHECK(strip.y() >= static_cast<float>(circle.pad().y() + circle.pad().height()) - 0.5f);
	};

	// **THE DESIGN WIDTH FIRST: two rows, as HX found them.** The disk is backlog IR's: the rail at the
	// panel's minimum height is taller than it is wide, so the cells move up and down and the disk grows
	// into the middle. MEASURED 2026-09-22 before IR: 310.441 (a square block, cells at its corners).
	layOutRailAt(ui::CircleRail::kWidth);
	CHECK(ui::CircleRail::outputRowsFor(ui::CircleRail::kWidth) == 2u);
	CHECK(circle.outputStripBounds().height() == doctest::Approx(2.0f * ui::kCardOutputStripHeight));
	const float diskAtDesign = static_cast<float>(circle.pad().width());
	MESSAGE("disk at the design width " << ui::CircleRail::kWidth << ": " << diskAtDesign << ", room "
	                                    << circle.layout().across << "x" << circle.layout().down);
	CHECK(circle.layout().across == doctest::Approx(ui::CircleRail::kBlockHeight));
	CHECK(diskAtDesign
	      == doctest::Approx(expectedOptimumDisk(ui::CircleRail::kWidth, static_cast<float>(circle.height()))));
	CHECK(diskAtDesign > ui::CircleRail::diskFor(ui::CircleRail::kBlockHeight));
	checkAllSevenPlaced();

	// **THE YIELD FLOOR — where it was seen the tail go.** More rows than the floor, all seven placed.
	layOutRailAt(ui::CircleRail::kYieldFloorWidth);
	const size_t rowsAtFloor = ui::CircleRail::outputRowsFor(ui::CircleRail::kYieldFloorWidth);
	MESSAGE("rows at the yield floor " << ui::CircleRail::kYieldFloorWidth << ": " << rowsAtFloor
	                                   << "; disk " << circle.pad().width() << ", room "
	                                   << circle.layout().across << "x" << circle.layout().down);
	// NOT VACUOUS: the floor really does need more rows than the design width, or the case is about a
	// width where the defect could not occur (an instrument blind to the condition).
	CHECK(rowsAtFloor > ui::CircleRail::kMinOutputRows);
	checkAllSevenPlaced();
	// **AND THE ROWS CAME OUT OF THE SPARE HEIGHT, NOT OUT OF THE CIRCLE.** The disk at the floor is the
	// whole width inside the padding (backlog IR; 67.2 before it), so the taller band cost it nothing.
	CHECK(circle.layout().across == doctest::Approx(ui::CircleRail::kBlockFloor));
	CHECK(static_cast<float>(circle.pad().width()) == doctest::Approx(ui::CircleRail::kBlockFloor));

	// **EVERY WIDTH BETWEEN, because the row count STEPS and a step is where a placement breaks.** At
	// each width: all seven placed, and the disk is NOT bound by the height the taller band left — which
	// is the claim `kOpenMinimumHeight` rests on now the band varies (a panel at its minimum height never
	// needs to grow for the extra rows). Read off the layout at the panel's minimum height, which is the
	// tightest room the rows ever have.
	size_t previousRows = rowsAtFloor;
	for (float w = ui::CircleRail::kYieldFloorWidth; w <= ui::CircleRail::kWidth; w += 1.0f) {
		layOutRailAt(w);
		CAPTURE(w);
		const size_t rows = ui::CircleRail::outputRowsFor(w);
		if (rows != previousRows) {
			MESSAGE("row count steps " << previousRows << " -> " << rows << " at rail width " << w);
			previousRows = rows;
		}
		// MONOTONE: a wider rail never needs more rows.
		CHECK(rows <= previousRows);
		checkAllSevenPlaced();
		CHECK(static_cast<float>(circle.pad().width())
		      == doctest::Approx(expectedOptimumDisk(w, static_cast<float>(circle.height()))));
		// NOT HEIGHT-BOUND at the panel's minimum height, wherever the band is taller than the design's.
		if (rows > ui::CircleRail::kMinOutputRows) {
			CHECK(static_cast<float>(circle.pad().width()) < circle.layout().down);
		}
	}
	CHECK(previousRows == ui::CircleRail::kMinOutputRows); // the sweep ended back at the design count
}
TEST_CASE("the output strip takes its height from the PANEL and not from the disk — the circle is the "
          "same size it was, and the pane is taller by exactly the strip") {
	// **TWO OF THE DECISIONS COLLIDE HERE AND THIS IS THE RESOLUTION, ASSERTED.** The strip runs along
	// the bottom of the pane, which costs HEIGHT — and the disk is bounded by the SHORTER of its two
	// rooms, so height taken from the pane is height taken from the circle at every size where height
	// binds. Against that: the whole Perform circle scales as large as the window allows.
	//
	// **RESOLVED BY THE EXISTING PRECEDENT, the one `minimumSize` already records for the horizontal case**:
	// "a rail that opens makes the window wider ... the grid keeps its knobs at full size throughout"
	// — the panel grows rather than the content compressing. So the strip ADDS to
	// `kOpenMinimumHeight` and the disk is untouched at the floor.
	//
	// **THE NUMBERS ARE THE CLAIM**: `kMinOutputBand` is subtracted in `blockSideFor` (at 392) and added in
	// `kOpenMinimumHeight`, and if those are not the SAME term the disk shrinks by the difference with
	// nothing red anywhere.
	CHECK(ui::CircleRail::kOpenMinimumHeight
	      == doctest::Approx(2.0f * ui::CircleRail::kPadTop + ui::CircleRail::kHeadingHeight
	                         + ui::CircleRail::kGap + ui::CircleRail::kBlockHeight
	                         + ui::CircleRail::kMinOutputBand));
	// AT THE FLOOR THE DISK IS EXACTLY WHAT IT WAS — the block is `kBlockHeight`, which is the
	// property the whole resolution exists to keep.
	CHECK(std::min(ui::CircleRail::acrossFor(ui::CircleRail::kWidth), ui::CircleRail::downFor(ui::CircleRail::kWidth, ui::CircleRail::kOpenMinimumHeight))
	      == doctest::Approx(ui::CircleRail::kBlockHeight));

	// **AND THE FX RAIL'S FLOOR NOW CLEARS THE CIRCLE'S WITH REAL MARGIN.** This pair has been wrong
	// twice in one day: while both fx lanes were stacked in one rail the two constants were EQUAL (422
	// = 422, two unrelated sums) and the `<=` in the floor case was passing on a coincidence. The rail
	// split dropped the fx floor to 231 and this raises the circle's, so the gap is wide and the
	// comparison is strict.
	CHECK(ui::FxLaneRail::kOpenMinimumHeight < ui::CircleRail::kOpenMinimumHeight);

	// **AND THE NATIVE WIDTH, per CLAUDE.md's scale rule.** the development display is 1.75x and *"a scale
	// change moves nothing logical" — a strip laid out at the wrong scale is the defect that produced
	// that rule (a veil painted at 1/1.75 of its card). So the claim is about the NATIVE rectangle: at
	// 1.75 the strip's native width is 1.75x its logical one, and an arm inside it likewise.
	PerformPanel p;
	REQUIRE(p.panel != nullptr);
	ui::CircleRail& circle = p.panel->circleRailForTest();
	p.armFrom();
	setRails(p, /*circleOpen=*/true, /*fxOpen=*/false);
	const float logicalStrip = circle.outputStripBounds().width();
	REQUIRE(logicalStrip > 0.0f);

	constexpr float kScale = 1.75f;
	p.panel->setDpiScale(kScale);
	p.panel->resized();
	// **THE LOGICAL RECTANGLE IS UNCHANGED — that is what "a scale change moves nothing logical"
	// MEANS**, and it is why asserting the logical width here would pass on a build where the strip
	// never converted.
	CHECK(circle.outputStripBounds().width() == doctest::Approx(logicalStrip));

	// **A NATIVE RECTANGLE IS INTEGRAL, SO THE CLAIM IS "within a pixel", NOT "equal".** MEASURED: an
	// arm 65.8 logical is 115 native against 115.15 predicted — a rounding difference, and a first
	// draft of this case asserted exact equality and went red on it. Asserting the exact product would
	// be asserting that visage does not round, which is a different claim and a false one.
	const auto nativeMatchesScale = [&](const visage::Frame& f) {
		const float want = static_cast<float>(f.width()) * kScale;
		const auto got = static_cast<float>(f.nativeWidth());
		CAPTURE(want);
		CAPTURE(got);
		// THE SCALE REALLY REACHED IT — the half that a tolerance alone would let through on a frame
		// that never converted, since at 1:1 native and logical are equal and within any tolerance.
		CHECK(got > static_cast<float>(f.width()));
		CHECK(std::abs(got - want) <= 1.0f);
	};
	nativeMatchesScale(circle);
	ui::SourceArm* arm = circle.outputArmsForTest()[0].get();
	REQUIRE(arm != nullptr);
	nativeMatchesScale(*arm);
}
TEST_CASE("PERFORM is exactly as wide as the grid beside it, and the two columns share an edge") {
	// Perform is the same width as the main panel. Two facts, and they are
	// different ones: the rail DECLARES the grid's width, and the panel PUTS it where the grid stops.
	// A rail that declared 392 and was laid out somewhere else would satisfy the first and be the
	// change not working.
	CHECK(ui::CircleRail::kWidth == doctest::Approx(ui::GenericExpanded::kGridWidth));
	// **NOT VACUOUS, AND RESTATED RATHER THAN REPEATED**: the grid width is four columns and the
	// padding either side, computed here from the cell rather than read off the constant under test.
	CHECK(ui::GenericExpanded::kGridWidth == doctest::Approx(24.0f + 4.0f * 92.0f));

	PerformPanel p;
	REQUIRE(p.panel != nullptr);
	setFolds(p, true, true, false);
	const ui::CircleRail& circle = p.panel->circleRailForTest();
	// THE PANEL GAVE IT THAT WIDTH, and gave it to it at the grid's right-hand edge.
	CHECK(static_cast<float>(circle.width()) == doctest::Approx(ui::GenericExpanded::kGridWidth));
	// **THE EDGE IS WHERE THE GRID'S SIDE COLUMN ENDS (backlog IG).** SKLITTER's grid gained a fifth
	// column for Groove and the loop settings; the circle kept the FOUR-column width (the "same width
	// as the main panel" was decided when the grid was four), so the shared edge moved one cell right.
	CHECK(static_cast<float>(circle.x())
	      == doctest::Approx(ui::GenericExpanded::kGridWidth + ui::GenericExpanded::kSideColumnWidth));

	// **AND THE ROOM WENT INTO THE DISK, WHICH IS THE ANSWER TO "what is the extra 220px for".** The
	// pad is square at `kDisk`, and `kDisk` is derived from the corner tuck — the case above checks
	// that derivation against the cells' real positions, so this is the half that says the pad was
	// actually given it.
	// **AND SINCE BACKLOG IR, THE ROOM'S HEIGHT TOO:** the panel is taller than the circle's floor, so
	// the cells move up and down and the disk is at least `kDisk`, which is its size in a square room.
	const ui::PerformPad& pad = p.panel->padForTest();
	CHECK(static_cast<float>(pad.width())
	      == doctest::Approx(expectedOptimumDisk(static_cast<float>(circle.width()),
	                                             static_cast<float>(circle.height()))));
	CHECK(static_cast<float>(pad.height()) == doctest::Approx(static_cast<float>(pad.width())));
	CHECK(static_cast<float>(pad.width()) >= ui::CircleRail::kDisk);
	// A square room at the rail's ask holds exactly `kDisk`, which is what stops a wider rail from
	// spilling down the page.
	CHECK(ui::CircleRail::kDisk <= ui::CircleRail::kBlockHeight);
	// AND THE FLOOR IT PUTS UNDER THE PANEL IS STILL UNDER THE GRID'S OWN ANSWER, so the taller block
	// costs SKLITTER no height at all. DERIVED here from the panel rather than from the comment.
	CHECK(ui::CircleRail::kOpenMinimumHeight <= static_cast<float>(p.panel->minimumSize().y));
}
TEST_CASE("the disk's two bounds CROSS, and neither side of the crossing can be seen from the other") {
	// **THE DEFECT THIS EXISTS FOR WAS MINE, AND NO CASE COULD SEE IT.** `kDisk` was
	// `2·(offset·√2 − kCornerClear)` — the largest disk leaving air between the grab reach and a corner
	// cell, measured along the diagonal. Turning that constant into a function of the block exposed an
	// inequality it had been satisfying by luck: `offset·√2` grows FASTER than `offset`, so past a block
	// of about 520.6 the diagonal answer is WIDER than the block it is supposed to sit in and the circle
	// spills out of the band at its four cardinal points — clipped top and bottom, because the block is
	// what the rail's height allows. MEASURED at a 1240px panel: block 610, diagonal 647.
	//
	// **EVERY CASE IN THE COMMIT THAT INTRODUCED IT WAS GREEN. IT TOOK THE THIRD PHOTOGRAPH.** Not a
	// cleverer assertion — one more render than was needed. *"Render geometry before believing it"
	// (CLAUDE.md).
	//
	// **AND THE REASON A SUITE COULD NOT HAVE CAUGHT IT AT ONE SIZE IS STRUCTURAL.** Below the crossing
	// the diagonal is always the answer and the block bound is unreachable; above it, the reverse. So a
	// case laid out at 392 is blind to half of `diskFor` BY CONSTRUCTION. This one is a pure-arithmetic
	// sweep for exactly that reason: it is the only shape that can stand on both sides at once.
	const auto diagonalFor = [](float block) {
		// The intended path restated, not `CircleRail::diskFor` called — a case that called it would be
		// asserting a function agrees with itself.
		const float offset = block * 0.5f - ui::CircleRail::kCellSide;
		return 2.0f * (offset * std::sqrt(2.0f) - ui::CircleRail::kCornerClear);
	};

	size_t flips = 0;
	float crossing = 0.0f;
	size_t diagonalSide = 0;
	size_t blockSide = 0;
	bool previous = diagonalFor(ui::CircleRail::kBlockHeight) <= ui::CircleRail::kBlockHeight;
	for (float block = ui::CircleRail::kBlockHeight; block <= 1200.0f; block += 1.0f) {
		const float diagonal = diagonalFor(block);
		// **THE PRODUCTION FUNCTION IS THE SMALLER OF THE TWO AT EVERY SIZE**, which is the whole claim.
		CHECK(ui::CircleRail::diskFor(block) == doctest::Approx(std::min(block, diagonal)));
		const bool diagonalBites = diagonal <= block;
		(diagonalBites ? diagonalSide : blockSide)++;
		if (diagonalBites != previous) {
			++flips;
			crossing = block;
			previous = diagonalBites;
		}
	}
	// **IT SWAPS ONCE AND ONLY ONCE**, which is what makes "below" and "above" two regimes rather than a
	// ragged alternation, and is the property the two layout cases each lean on from one side.
	CHECK(flips == 1u);
	// AND WHERE: `b(√2 − 1) = 2·kCellSide·√2 + 2·kCornerClear` puts it at 520.6, derived here rather
	// than read off the sweep it is checking.
	const float derived = (2.0f * ui::CircleRail::kCellSide * std::sqrt(2.0f)
	                       + 2.0f * ui::CircleRail::kCornerClear)
	                      / (std::sqrt(2.0f) - 1.0f);
	CHECK(crossing == doctest::Approx(std::ceil(derived)).epsilon(0.01));
	// NOT VACUOUS: both regimes really occur in the sweep, so neither `CHECK` above is being made about
	// an empty half.
	CHECK(diagonalSide > 50u);
	CHECK(blockSide > 50u);

	// **AND BOTH SIDES ARE REACHABLE IN THE REAL LAYOUT, not only in this sweep.** The panel's own ask
	// is below the crossing and a dragged-out panel is above it — which is what the two cases that lay
	// the rail out actually stand on.
	CHECK(diagonalFor(ui::CircleRail::kBlockHeight) <= ui::CircleRail::kBlockHeight);
	CHECK(diagonalFor(700.0f) > 700.0f);
}
TEST_CASE("THE PERFORM CIRCLE GROWS WITH THE PANEL, driven through the real overlay's resize grip") {
	// When the window is enlarged, the whole Perform circle scales as large as the window allows.
	//
	// **THE CLAIM IS A RADIUS, WHICH IS A VALUE.** CLAUDE.md: a comparison between two renders is
	// evidence that SOMETHING changed, never that the right thing did — and the circle is exactly the
	// surface where that trap bites, because a pad drawn at any wrong size still draws differently from
	// one drawn at 310. So the disk is computed here from the room the rail was given, by an
	// independent restatement of the corner geometry (`expectedOptimumDisk`), and the "it got bigger"
	// comparison is kept only as the SECOND assertion that proves the first is not vacuous.
	//
	// **AND IT GOES THROUGH THE GRIP, which is the only way a hand makes a panel bigger.** The grip
	// writes `slot.sized`, `panelSizeOf` maxes it against the content's minimum, the panel is laid out
	// at the difference and `SklitterExpanded::resized` hands that difference to this rail. A case that
	// called `circle.setBounds()` itself would prove the rail's arithmetic and nothing about whether any
	// of that road exists.
	namespace sc = feathers::fx::sklitter;
	ModFixture f;
	auto* lane = findInTree<ui::FxLane>(f.root);
	auto* overlay = f.gui.expandedForTest();
	REQUIRE(lane != nullptr);
	REQUIRE(overlay != nullptr);
	const size_t slot = f.bridge.addFxDevice(sc::kTraits.tag);
	f.bridge.refresh();
	lane->resized();
	lane->scrollCardIntoView(slot);
	lane->resized();
	REQUIRE(lane->onExpandDevice != nullptr);
	(void)f.gui.canvasForTest()->setCollapsed(true);
	lane->onExpandDevice(slot);
	REQUIRE(overlay->open());

	auto* panel = findInTree<ui::SklitterExpanded>(f.root);
	REQUIRE(panel != nullptr);
	ui::CircleRail& circle = panel->circleRailForTest();
	ui::PerformPad& pad = panel->padForTest();
	REQUIRE_FALSE(circle.collapsed());

	// **THE STATE THE CASE IS ASKED IN, ESTABLISHED BEFORE IT ASKS ANYTHING.** A correct assertion put
	// to the wrong state passes for the same reason a right one does — so the floor is checked as a
	// floor here: at the smallest panel the rail is its ASK and the pad is the disk that room holds
	// (backlog IR: at least `kDisk`, more when the panel is taller than the circle's floor), which is
	// what makes every "it grew past this" below mean something.
	CHECK(static_cast<float>(circle.width()) == doctest::Approx(ui::CircleRail::kWidth));
	CHECK(static_cast<float>(pad.width())
	      == doctest::Approx(expectedOptimumDisk(ui::CircleRail::kWidth, static_cast<float>(circle.height()))));
	CHECK(static_cast<float>(pad.width()) >= ui::CircleRail::kDisk);
	CHECK(pad.scale() == doctest::Approx(static_cast<float>(pad.width()) / ui::PerformPad::kReferenceDiameter));
	CHECK(panel->surplusWidth() == doctest::Approx(0.0f));
	const float baseRadius = pad.disk().radius;
	const float baseCellSide = static_cast<float>(panel->cornerCellsForTest()[sc::kCfg1]->width());
	const float baseScale = pad.scale();
	REQUIRE(baseRadius > 0.0f);

	// **PLACED FULL-LEFT FIRST, BECAUSE THE GRIP IS CLAMPED FROM WHERE THE PANEL SITS.** A centred
	// panel can only grow into half this layer's headroom, and in a narrow editor that is none at all —
	// which would make every assertion below read the clamp rather than the growth.
	overlay->beginPanelDrag();
	overlay->dragPanelBy({ -5000.0f, -5000.0f });
	overlay->beginPanelResize();
	overlay->resizePanelBy({ 260.0f, 260.0f });

	// **NOT MEASURING A CLAMP, AND NOT MEASURING NOTHING.** If the layer refused the grip there is no
	// growth to assert anything about, and a case that carried on would be green about a state it never
	// reached.
	REQUIRE(panel->surplusWidth() > 0.0f);
	REQUIRE(pad.scale() > baseScale);

	const float railWide = static_cast<float>(circle.width());
	const float railTall = static_cast<float>(circle.height());
	// THE RAIL GOT ITS ASK PLUS THE SURPLUS, which is the one line that says where the room went.
	CHECK(railWide == doctest::Approx(ui::CircleRail::kWidth + panel->surplusWidth()));

	// AND THE INSTRUMENT IS THE SIZE THAT ROOM ALLOWS — restated, not called.
	const auto [across, down] = expectedRoomFor(railWide, railTall);
	const float disk = expectedOptimumDisk(railWide, railTall);
	CHECK(circle.layout().across == doctest::Approx(across));
	CHECK(circle.layout().down == doctest::Approx(down));
	CHECK(static_cast<float>(pad.width()) == doctest::Approx(disk));
	CHECK(static_cast<float>(pad.height()) == doctest::Approx(disk));
	// **THE PAD'S FRAME IS NOT THE PLAYABLE DISK**, and the radius is the number a drag is decomposed
	// against — so it is asserted in its own right, with the inset restated from the marks it reserves
	// room for rather than read off `rimInset()`.
	CHECK(pad.disk().radius
	      == doctest::Approx(disk * 0.5f - (ui::PerformPad::kMarkRadius + 2.0f) * pad.scale()));

	// **AND THE SECOND READING, WHICH PROVES THE FIRST IS NOT VACUOUS**: the circle is really bigger
	// than it was, not merely equal to an arithmetic that also did not move.
	CHECK(pad.disk().radius > baseRadius);
	CHECK(static_cast<float>(pad.width()) > ui::CircleRail::kDisk);

	// **THE CELLS DID NOT GROW WITH IT, AND THAT IS THE DESIGN RATHER THAN AN OVERSIGHT.** A square is
	// sized by the two rows it prints (`kCellSide`); what a bigger room buys is SPREAD. So they are
	// the same size, inside the room, and further apart.
	const auto& cells = panel->cornerCellsForTest();
	REQUIRE(cells[sc::kCfg1] != nullptr);
	CHECK(static_cast<float>(cells[sc::kCfg1]->width()) == doctest::Approx(baseCellSide));
	CHECK(static_cast<float>(cells[sc::kCfg2]->x() - cells[sc::kCfg1]->x())
	      <= across - ui::CircleRail::kCellSide + 0.01f);

	// **AND THE AIR BETWEEN A CELL AND THE GESTURE SURVIVED THE SCALING.** This is the property the
	// whole geometry is derived to hold, and the one a scale factor applied to some furniture and not
	// the rest would break silently: the mark, the rim inset and the grab reach all move together or a
	// corner cell ends up inside the disk a press is answered for.
	const float cx = static_cast<float>(pad.x()) + static_cast<float>(pad.width()) * 0.5f;
	const float cy = static_cast<float>(pad.y()) + static_cast<float>(pad.height()) * 0.5f;
	const float grabbed = pad.disk().radius + pad.grabSlack();
	CHECK(grabbed == doctest::Approx(disk * 0.5f)); // the identity `kCornerClear` leans on
	for (size_t i = 0; i < sc::kCorners; ++i) {
		CAPTURE(i);
		const auto* c = cells[i];
		const float nearX = std::clamp(cx, static_cast<float>(c->x()),
		                               static_cast<float>(c->x() + c->width()));
		const float nearY = std::clamp(cy, static_cast<float>(c->y()),
		                               static_cast<float>(c->y() + c->height()));
		const float clear = std::hypot(nearX - cx, nearY - cy) - grabbed;
		// **EXACTLY `kCornerClear`, IN EVERY REGIME SINCE BACKLOG IR.** Before it, past the crossover the
		// disk stopped at the block's side and the cells stayed at the block's corners, further away.
		// Now a cell is placed as close as the air allows, whichever bound stopped the disk.
		CHECK(clear == doctest::Approx(ui::CircleRail::kCornerClear));
	}

	// **AND THE DISK FITS THE ROOM, which is the bound the compile-time version never needed.** A disk
	// wider than the room spills out of the rail's band at the cardinal points: clipped top and bottom.
	CHECK(static_cast<float>(pad.width()) <= std::min(across, down) + 0.01f);
	CHECK(static_cast<float>(pad.y()) >= 0.0f);
	CHECK(static_cast<float>(pad.y() + pad.height()) <= static_cast<float>(circle.height()));
}
TEST_CASE("the surplus width is the CIRCLE's, and the grid and the fx rail keep exactly their ask") {
	// **THE DESIGN QUESTION THIS ANSWERS: where does extra panel width GO.** The panel is three
	// columns and only one of them can spend area. The grid is fixed cells four across by design, so
	// width past its own answer is dead air behind the knobs; the fx rail is a column of cards as wide
	// as the rail, and its own header says its width is *"chosen rather than derived ... not
	// load-bearing". The circle is the one thing here whose substance IS area. So it takes all of it.
	//
	// **AND THE RISK IS A REAL ONE: the body is laid out from `width() - bodyRightInset()`**, so a
	// surplus handed to the rail without being taken out of that inset puts the grid and the circle on
	// top of each other. That is what the "share an edge" line below is for.
	namespace sc = feathers::fx::sklitter;
	PerformPanel p;
	REQUIRE(p.panel != nullptr);
	setFolds(p, true, true, true);
	const float askedWide = static_cast<float>(p.panel->width());
	const float baseFx = static_cast<float>(p.panel->postRailForTest().width());
	const float baseCircleX = static_cast<float>(p.panel->circleRailForTest().x());
	/// EVERY GRID CELL'S WIDTH AT THE PANEL'S OWN ASK — the reference the stretch check is made against,
	/// read from the layout rather than from `kCellWidth`, which is protected and is also the number the
	/// grid would be accused of ignoring.
	std::vector<float> baseCellWidths;
	for (const visage::Frame* cell : p.panel->cellsForTest()) {
		baseCellWidths.push_back(cell == nullptr ? 0.0f : static_cast<float>(cell->width()));
	}

	// THE HOST HANDS IT MORE ROOM — which is all a host ever does to a panel.
	constexpr float kExtra = 300.0f;
	p.panel->setBounds(0, 0, static_cast<int>(askedWide + kExtra),
	                   static_cast<int>(p.panel->minimumSize().y + kExtra));
	p.panel->resized();

	ui::CircleRail& circle = p.panel->circleRailForTest();
	ui::FxLaneRail& grain = p.panel->grainRailForTest();
	ui::FxLaneRail& fx = p.panel->postRailForTest();
	CHECK(p.panel->surplusWidth() == doctest::Approx(kExtra));
	// ALL OF IT, to the circle.
	CHECK(static_cast<float>(circle.width()) == doctest::Approx(ui::CircleRail::kWidth + kExtra));
	// AND NONE OF IT ANYWHERE ELSE — **both** fx rails now, which is the line a surplus split three
	// ways rather than given whole would fail.
	CHECK(static_cast<float>(fx.width()) == doctest::Approx(baseFx));
	CHECK(static_cast<float>(fx.width()) == doctest::Approx(ui::FxLaneRail::kWidth));
	CHECK(static_cast<float>(grain.width()) == doctest::Approx(ui::FxLaneRail::kWidth));

	// **THE FOUR COLUMNS STILL TILE THE PANEL, which is the line the overlap would fail.** The body
	// stops where the circle starts, the circle where GRAIN FX starts, GRAIN FX where POST FX starts,
	// and POST FX at the panel's edge.
	const float wide = static_cast<float>(p.panel->width());
	CHECK(static_cast<float>(circle.x()) + static_cast<float>(circle.width())
	      == doctest::Approx(static_cast<float>(grain.x())));
	CHECK(static_cast<float>(grain.x()) + static_cast<float>(grain.width())
	      == doctest::Approx(static_cast<float>(fx.x())));
	CHECK(static_cast<float>(fx.x()) + static_cast<float>(fx.width()) == doctest::Approx(wide));
	// **AND THE BODY STOPS WHERE THE CIRCLE STARTS**, which is the overlap this could have introduced:
	// `GenericExpanded::resized` spans the body's own rail over `width() - bodyRightInset()`, so a
	// surplus given to the circle without being taken out of that inset draws the grid under it.
	REQUIRE(p.panel->bodyRailForTest() != nullptr);
	CHECK(static_cast<float>(p.panel->bodyRailForTest()->width())
	      == doctest::Approx(static_cast<float>(circle.x())));
	// **AND THE CIRCLE'S LEFT EDGE DID NOT MOVE**, which is the shape of "the grid keeps its ask": the
	// rail stayed against the grid and grew RIGHTWARD into the room, rather than the whole arrangement
	// sliding. A surplus shared with the grid would move this.
	CHECK(static_cast<float>(circle.x()) == doctest::Approx(baseCircleX));

	// **AND THE GRID IS UNTOUCHED — the half a width assertion cannot see.** The knobs keep their cell,
	// which is the design decision of 2026-09-19 arriving at a second size: the window grows, the grid
	// does not compress, and it does not stretch either. The reference is the size they were at the
	// panel's own ask, read before the room arrived.
	REQUIRE(!baseCellWidths.empty());
	for (size_t n = 0; n < p.panel->cellsForTest().size(); ++n) {
		const visage::Frame* cell = p.panel->cellsForTest()[n];
		if (cell == nullptr || !cell->isVisible()) {
			continue;
		}
		CAPTURE(n);
		CHECK(static_cast<float>(cell->width()) == doctest::Approx(baseCellWidths[n]));
	}
	(void)sc::kCorners;
}
TEST_CASE("the circle stays ROUND: it grows to the shorter of the two rooms and stops") {
	// **THE SECOND DESIGN QUESTION, ANSWERED RATHER THAN ASSUMED.** A disk in a rail that grows on one
	// axis either stops growing at the other or stops being a circle. It stops growing — so a panel
	// dragged wide and short puts its surplus into air either side of a centred circle, and a panel
	// dragged tall and narrow into air above and below it. Both directions are checked, because a
	// `min` written as a `max` passes one of them.
	//
	// **SINCE BACKLOG IR THE ROOM IS NOT CUT SQUARE FIRST.** The disk is still never wider than the
	// shorter room, but it reaches it: the cells move into the long axis' spare room instead of holding
	// the disk to a square block's inset.
	PerformPanel p;
	REQUIRE(p.panel != nullptr);
	setFolds(p, true, true, false);
	const float askedWide = static_cast<float>(p.panel->minimumSize().x);
	const float askedTall = static_cast<float>(p.panel->minimumSize().y);
	ui::CircleRail& circle = p.panel->circleRailForTest();
	ui::PerformPad& pad = p.panel->padForTest();

	const auto layOut = [&](float wide, float tall) {
		p.panel->setBounds(0, 0, static_cast<int>(wide), static_cast<int>(tall));
		p.panel->resized();
	};

	/// The column left under the heading — the DOWN bound, restated from the rail's own furniture.
	/// **LESS THE OUTPUT BAND SINCE 2026-09-20**: the seven perform outputs sit along the bottom of the
	/// pane, and that strip is height the disk never sees. Restated here rather than read off
	/// `downFor`, which is under test. **The MINIMUM band: every subcase here lays out at the asked
	/// width or wider, where the row count is floored at `kMinOutputRows`.**
	const auto downBound = [&] {
		return static_cast<float>(circle.height()) - 2.0f * ui::Rail::kPadTop
		       - ui::CircleRail::kHeadingHeight - ui::CircleRail::kGap - ui::CircleRail::kMinOutputBand;
	};
	/// The band inside the padding — the ACROSS bound.
	const auto acrossBound = [&] {
		return static_cast<float>(circle.width()) - 2.0f * ui::CircleRail::kPadX;
	};

	SUBCASE("wide and short: the HEIGHT binds, and more width past it changes nothing") {
		layOut(askedWide + 400.0f, askedTall);
		const float stopped = static_cast<float>(pad.width());
		CHECK(stopped == doctest::Approx(downBound()));
		// **AND IT IS REALLY THE HEIGHT DOING IT**: the width would allow far more. Without this the
		// case would pass on a build that had simply stopped growing altogether.
		CHECK(stopped < acrossBound());

		layOut(askedWide + 800.0f, askedTall);
		CHECK(static_cast<float>(pad.width()) == doctest::Approx(stopped));
		// NOT VACUOUS: the RAIL really did get the second 400px, it simply could not spend it.
		CHECK(static_cast<float>(circle.width()) == doctest::Approx(ui::CircleRail::kWidth + 800.0f));
		// AND THE DISK IS CENTRED IN THE AIR IT COULD NOT USE, rather than left against one edge.
		CHECK(static_cast<float>(pad.x()) + static_cast<float>(pad.width()) * 0.5f
		      == doctest::Approx(static_cast<float>(circle.width()) * 0.5f));
	}

	SUBCASE("tall and narrow: the WIDTH binds, and more height past it changes nothing") {
		layOut(askedWide, askedTall + 400.0f);
		const float stopped = static_cast<float>(pad.width());
		CHECK(static_cast<float>(circle.width()) == doctest::Approx(ui::CircleRail::kWidth));
		CHECK(stopped == doctest::Approx(acrossBound()));
		// At the panel's own width that bound is `kBlockHeight`, and the disk now reaches it: 372
		// against the 310 the square block's inset gave (backlog IR).
		CHECK(stopped == doctest::Approx(ui::CircleRail::kBlockHeight));
		CHECK(stopped > ui::CircleRail::kDisk);
		CHECK(stopped < downBound()); // really the width doing it

		layOut(askedWide, askedTall + 800.0f);
		CHECK(static_cast<float>(pad.width()) == doctest::Approx(stopped));
		// NOT VACUOUS: the rail really is 800px taller and the disk still did not grow.
		CHECK(static_cast<float>(circle.height()) == doctest::Approx(askedTall + 800.0f));
	}

	SUBCASE("both: it grows, and it is still round") {
		layOut(askedWide + 400.0f, askedTall + 400.0f);
		CHECK(static_cast<float>(pad.width()) == doctest::Approx(static_cast<float>(pad.height())));
		CHECK(static_cast<float>(pad.width())
		      == doctest::Approx(expectedOptimumDisk(static_cast<float>(circle.width()),
		                                             static_cast<float>(circle.height()))));
		CHECK(static_cast<float>(pad.width()) > ui::CircleRail::kDisk);
	}

	SUBCASE("past the crossover the ROOM is the bound, and the disk stays inside the rail") {
		// **THE REGIME THE FIRST DRAFT OF THIS GEOMETRY GOT WRONG.** `offset·√2` grows faster than
		// `offset`, so past a room of about 521 the corner-derived disk is WIDER than the room it is
		// supposed to sit in, and the circle runs out of the band at its four cardinal points. Every
		// case here was green and the picture at a 1240px panel showed it: block 610, pad 647.
		layOut(askedWide + 900.0f, askedTall + 900.0f);
		const float room = std::min(acrossBound(), downBound());
		// **THE STATE THE QUESTION IS ASKED IN.** Below the crossover this case would be re-asking the
		// one above it and would pass for the wrong reason.
		REQUIRE(room > 521.0f);
		CHECK(static_cast<float>(pad.width()) == doctest::Approx(room));
		CHECK(static_cast<float>(pad.x()) >= 0.0f);
		CHECK(static_cast<float>(pad.y()) >= 0.0f);
		CHECK(static_cast<float>(pad.x() + pad.width()) <= static_cast<float>(circle.width()) + 0.01f);
		CHECK(static_cast<float>(pad.y() + pad.height())
		      <= static_cast<float>(circle.height()) + 0.01f);
		// AND IT IS STILL A REAL INSTRUMENT, not a pad clamped to nothing.
		CHECK(static_cast<float>(pad.width()) > ui::CircleRail::kDisk);
	}
}
TEST_CASE("the pad's 1:1 furniture size is the disk a SQUARE room at the rail's ask holds") {
	// **A NUMBER THAT HAS TO AGREE WITH ANOTHER FILE'S, CHECKED RATHER THAN SHARED.**
	// `PerformPad::kReferenceDiameter` is what every piece of the pad's furniture is sized against, and
	// it has to be `CircleRail::kDisk` or the pad in the design's square room would draw at some scale
	// other than 1. It cannot BE that constant: `sklitter_expanded.h` includes `perform_pad.h`, so the
	// dependency runs one way only. `Rail::kCollapsedLabelRun` is the same arrangement one widget over.
	CHECK(ui::PerformPad::kReferenceDiameter == doctest::Approx(ui::CircleRail::kDisk));
	// **AND THE CONSEQUENCE, WHICH IS THE POINT OF THE NUMBER**: in that room the scale is exactly 1.
	// **IT WAS "AT THE SMALLEST PANEL" UNTIL BACKLOG IR**, when the smallest panel's room was cut square
	// from it. SKLITTER's grid makes the panel taller than the circle's floor, and since IR the disk uses
	// that height, so the default panel's pad is larger than `kDisk` and its furniture scales with it.
	PerformPanel p;
	REQUIRE(p.panel != nullptr);
	setFolds(p, true, true, false);
	ui::CircleRail& circle = p.panel->circleRailForTest();
	circle.setBounds(circle.x(), circle.y(), ui::CircleRail::kWidth, ui::CircleRail::kOpenMinimumHeight);
	circle.resized();
	CHECK(static_cast<float>(p.panel->padForTest().width()) == doctest::Approx(ui::CircleRail::kDisk));
	CHECK(p.panel->padForTest().scale() == doctest::Approx(1.0f));
	CHECK(p.panel->padForTest().markRadius() == doctest::Approx(ui::PerformPad::kMarkRadius));
	CHECK(p.panel->padForTest().puckRadius() == doctest::Approx(ui::PerformPad::kPuckRadius));
	// THE IDENTITY `CircleRail::kCornerClear` LEANS ON: the grab reach is exactly half the pad's frame,
	// which holds at every size only because these two are one number.
	CHECK(p.panel->padForTest().grabSlack() == doctest::Approx(p.panel->padForTest().rimInset()));
}
TEST_CASE("the ground BEHIND the circle is Feathers' perform ground, and it is the RAIL that paints it") {
	// The ground behind the circle is the same colour as Feathers' Perform background. Behind the circle — the surface the disk sits ON, not a fill inside it. The pad
	// still draws a RING and not a plate, which is its own long-standing decision and is checked below.
	//
	// **WHICH COLOUR THAT IS, READ RATHER THAN NAMED.** `PerformStage::draw` paints no ground of its
	// own — sounds, sky, wake, trail, tour, rim, reticle, label, and nothing under any of them — so
	// "Feathers' perform background" is whatever the editor paints behind it, which is
	// `Gui::Impl::drawBackground`'s `canvas.setColor(kVoid)`. `theme.h` has `kVoid =
	// tokens::kVoidPerform`, a GENERATED token, so the value is the mock's own `--void-perform`. The
	// mock agrees from the other end: `drawBlendView` fills that circle's aperture `TH.voidP`.
	const uint32_t expectedGround = ui::rotatedPalette(ui::kChromeBase, ui::chromeHueOffset(), ui::accentHueOffset()).voidGround;
	CHECK(ui::CircleRail::kGround == expectedGround);
	CHECK(expectedGround == ui::rotateHue(feathers::tokens::kVoidPerform, ui::chromeHueOffset()));
	// **NOT VACUOUS, AND IT IS THE WHOLE POINT OF THE CHANGE**: the rail no longer wears the surface
	// every other rail wears. A ground equal to `kSurfaceRaised` is this case passing on the build it
	// exists to reject.
	CHECK(ui::CircleRail::kGround != ui::kSurfaceRaised);

	PerformPanel p;
	REQUIRE(p.panel != nullptr);
	setFolds(p, true, true, false);
	ui::CircleRail& circle = p.panel->circleRailForTest();
	// THE STATE THE QUESTION IS ASKED IN: nothing has painted, so the seam answers nothing.
	REQUIRE(circle.groundForTest() == 0u);

	visage::Canvas canvas;
	circle.draw(canvas);
	CHECK(circle.groundForTest() == ui::CircleRail::kGround);

	// **AND THE PAD STILL DOES NOT PAINT A PLATE**, which is the half the correction to this task turned
	// on. `perform_pad.cpp`'s decision — *"a ring rather than a filled plate ... the plain one should not
	// claim there is anything there" — stands, and the dark inside the ring is this rail showing
	// through. **THAT HALF IS NOT ASSERTED HERE AND CANNOT BE**: a `Canvas` cannot be asked what it was
	// given, so "the pad paints no ground" has no seam in it by construction — which is the point, but
	// it means the evidence is the private display, where a pixel inside the ring and a pixel on the
	// rail beside it both sample `#010204`. A first draft asserted the absence with a `requires`
	// expression on a member that does not exist; that is ill-formed rather than false, and a case that
	// does not compile is not a case.
}
TEST_CASE("the starfield is FEATHERS' OWN SKY, warped by the puck, and STILL") {
	// A starfield like Feathers' Perform sky inside the circle, without the twinkle, its stars moving
	// as the puck position warps them.
	//
	// **THE CLAIM IS THE ABSENCE OF A CLOCK, AND THAT IS WHAT THE THREE COMPARISONS BELOW MEASURE.** A
	// twinkle is a function of TIME and dirties a frame forever with no hand on anything — backlog GJ,
	// which is the Explore chart repainting at rest for weeks and nobody noticing. A warp is a function
	// of the PUCK. So: same puck twice must be IDENTICAL (there is no clock), a different puck must
	// DIFFER (the warp is live), and coming BACK must be identical again (it is a mapping, not an
	// accumulation). The third is the one that cannot be had any other way: a field that integrated its
	// own displacement is correct in every single frame and drifts across a session.
	namespace sc = feathers::fx::sklitter;
	namespace pp = feathers::ui::perform_pad;
	PerformPanel p;
	REQUIRE(p.panel != nullptr);
	setFolds(p, true, true, false);
	ui::PerformPad& pad = p.panel->padForTest();
	const pp::Disk d = pad.disk();
	REQUIRE(d.valid());

	/// THE GESTURE'S OWN ROAD, so the puck is moved the way a hand moves it rather than by a seam.
	const auto press = [&](float px, float py) {
		visage::MouseEvent e;
		e.position = { px, py };
		pad.onMouseDown().callback(e);
		pad.onMouseUp().callback(e);
	};
	const auto drawn = [&] {
		visage::Canvas canvas;
		pad.draw(canvas);
		return pad.starsForTest();
	};
	const auto same = [](const std::vector<ui::PerformPad::DrawnStar>& a,
	                     const std::vector<ui::PerformPad::DrawnStar>& b) {
		if (a.size() != b.size()) {
			return false;
		}
		for (size_t i = 0; i < a.size(); ++i) {
			// BIT FOR BIT. "Still" does not mean "close": a field re-randomised or re-integrated per
			// draw is motionless in intent and flickers in fact, and an epsilon here would pass it.
			if (a[i].seed != b[i].seed || a[i].x != b[i].x || a[i].y != b[i].y) {
				return false;
			}
		}
		return true;
	};

	// THE STATE THE QUESTION IS ASKED IN: nothing has painted, so the record answers nothing.
	REQUIRE(pad.starsForTest().empty());

	press(d.cx, d.cy); // the centre, exactly
	REQUIRE(pad.weightsForTest().x == doctest::Approx(0.0f).epsilon(1e-5));
	const std::vector<ui::PerformPad::DrawnStar> centred = drawn();

	// **IT IS REALLY A FIELD**, not an empty record that every comparison below would agree about.
	const auto& sky = ui::performSky();
	REQUIRE(centred.size() > sky.size() / 2);
	CHECK(centred.size() <= sky.size());

	// **AND IT IS FEATHERS' SKY, BOUNDED — the line that says these are performSky()'s stars rather
	// than some other field that happens to be deterministic.** The pull is capped at 0.15 and its two
	// components are 1 and 0.45 of it, so no star can move further than 0.15·√(1+0.45²) ≈ 0.165 from
	// where the table put it. A field generated from a different table, or from a second seed, or
	// re-randomised, fails this by a mile.
	for (const ui::PerformPad::DrawnStar& star : centred) {
		CAPTURE(star.seed);
		REQUIRE(star.seed < sky.size());
		const float ux = (star.x - d.cx) / d.radius;
		const float uy = (star.y - d.cy) / d.radius;
		CHECK(std::hypot(ux - sky[star.seed].x, uy - sky[star.seed].y) <= 0.166f);
		// AND THE APERTURE CONTAINS THE SKY: a star the warp pushed past the rim is not drawn at all,
		// because this surface has no clip path and would otherwise paint it on the rail.
		CHECK(std::hypot(ux, uy) <= 1.0f + 1e-4f);
	}

	SUBCASE("the same puck, drawn twice, is identical bit for bit — there is no clock in here") {
		CHECK(same(centred, drawn()));
		CHECK(same(centred, drawn()));
	}

	SUBCASE("a different puck moves it, and in the direction the warp claims") {
		const float px = d.cx + d.radius * 0.55f;
		const float py = d.cy - d.radius * 0.35f;
		press(px, py);
		const std::vector<ui::PerformPad::DrawnStar> moved = drawn();
		CHECK_FALSE(same(centred, moved));

		// **THE DIRECTION, WHICH "IT DIFFERS" CANNOT SAY — AND THE FIRST STATEMENT OF IT WAS WRONG.**
		// The obvious claim is "the star nearest the puck ends up nearer to it", and it is FALSE for
		// this field: the pull has a radial term of `0.45·p` and a TANGENTIAL term of `p`, so a star
		// already close is mostly swirled around and its distance can rise. MEASURED on the nearest
		// star: seed 0.0949 from the puck, drawn 0.1523. The field is a swirl, not an attraction.
		//
		// **WHAT IS TRUE AT EVERY DISTANCE is that the displacement LEANS toward the puck**: the
		// component of `drawn − seed` along the unit vector from the seed to the puck is exactly
		// `0.45·p`, because the tangential term is perpendicular and contributes nothing to that dot
		// product. That is the claim a sign-inverted warp fails and a jitter-on-the-puck's-value fails.
		const fx::sklitter::PerformWeights w = pad.weightsForTest();
		REQUIRE(std::hypot(w.x, w.y) > 0.1f); // the puck really moved off the centre
		const auto leanOf = [&](size_t seed, const ui::PerformPad::DrawnStar& star) {
			const float ux = (star.x - d.cx) / d.radius;
			const float uy = (star.y - d.cy) / d.radius;
			const float tx = w.x - sky[seed].x;
			const float ty = w.y - sky[seed].y;
			const float len = std::hypot(tx, ty);
			return std::pair<float, float> { ((ux - sky[seed].x) * tx + (uy - sky[seed].y) * ty) / len,
				                             std::hypot(ux - sky[seed].x, uy - sky[seed].y) };
		};
		size_t nearest = 0;
		size_t farthest = 0;
		float best = 1e9f;
		float worst = -1.0f;
		for (size_t i = 0; i < sky.size(); ++i) {
			const float dist = std::hypot(sky[i].x - w.x, sky[i].y - w.y);
			if (dist < best) {
				best = dist;
				nearest = i;
			}
			if (dist > worst) {
				worst = dist;
				farthest = i;
			}
		}
		float nearLean = -1.0f;
		float nearMove = 0.0f;
		float farLean = -1.0f;
		for (const ui::PerformPad::DrawnStar& star : moved) {
			if (star.seed == nearest) {
				const auto [lean, move] = leanOf(nearest, star);
				nearLean = lean;
				nearMove = move;
			}
			if (star.seed == farthest) {
				farLean = leanOf(farthest, star).first;
			}
		}
		// NOT VACUOUS: both of those stars really were drawn, so the numbers below are about stars.
		REQUIRE(nearLean > -1.0f);
		REQUIRE(farLean > -1.0f);
		CHECK(nearLean > 0.0f);
		CHECK(farLean > 0.0f);
		// **AND IT FALLS OFF**, which "it leans" alone does not say: a uniform shove would pass both
		// lines above and would not be a warp around anything.
		CHECK(nearLean > farLean * 2.0f);
		// **AND IT REALLY IS A SWIRL**: the displacement is bigger than its lean, so there is a
		// tangential component. A pure attraction passes every line above this one.
		CHECK(nearMove > nearLean * 1.5f);
	}

	SUBCASE("and coming BACK lands on the same stars — a mapping, not an accumulation") {
		press(d.cx + d.radius * 0.55f, d.cy - d.radius * 0.35f);
		const std::vector<ui::PerformPad::DrawnStar> away = drawn();
		REQUIRE_FALSE(same(centred, away)); // the trip really happened
		press(d.cx, d.cy);
		CHECK(same(centred, drawn()));
		// AND ROUND AGAIN, because one round trip can be survived by a field that alternates.
		press(d.cx + d.radius * 0.55f, d.cy - d.radius * 0.35f);
		CHECK(same(away, drawn()));
		press(d.cx, d.cy);
		CHECK(same(centred, drawn()));
	}
}
TEST_CASE("the starfield SCALES with the circle: the same stars, in the same places, on a bigger disk") {
	// **THE FIELD IS A PROPERTY OF THE INSTRUMENT, NOT OF THE PIXELS.** The stars live in the unit disk
	// and are drawn at `centre + p·radius`, so a circle that grew has the same sky spread wider rather
	// than the same pixels with gaps between them. Asserted as a VALUE — each star's position converted
	// BACK through the disk it was drawn in — because "the picture differs at two sizes" is true of any
	// build at all.
	namespace pp = feathers::ui::perform_pad;
	PerformPanel p;
	REQUIRE(p.panel != nullptr);
	setFolds(p, true, true, false);
	ui::PerformPad& pad = p.panel->padForTest();

	const auto unitField = [&] {
		visage::Canvas canvas;
		pad.draw(canvas);
		const pp::Disk d = pad.disk();
		std::vector<std::pair<size_t, std::pair<float, float>>> out;
		for (const ui::PerformPad::DrawnStar& star : pad.starsForTest()) {
			out.push_back({ star.seed, { (star.x - d.cx) / d.radius, (star.y - d.cy) / d.radius } });
		}
		return out;
	};

	const auto small = unitField();
	const float smallRadius = pad.disk().radius;
	REQUIRE(!small.empty());

	p.panel->setBounds(0, 0, static_cast<int>(p.panel->minimumSize().x + 400.0f),
	                   static_cast<int>(p.panel->minimumSize().y + 400.0f));
	p.panel->resized();
	const auto large = unitField();
	// THE STATE THE QUESTION IS ASKED IN: the disk really is bigger, or the equality below is trivial.
	REQUIRE(pad.disk().radius > smallRadius * 1.2f);

	REQUIRE(large.size() == small.size());
	for (size_t i = 0; i < small.size(); ++i) {
		CAPTURE(i);
		CHECK(large[i].first == small[i].first);
		CHECK(large[i].second.first == doctest::Approx(small[i].second.first).epsilon(1e-4));
		CHECK(large[i].second.second == doctest::Approx(small[i].second.second).epsilon(1e-4));
	}
	// AND THE SECOND READING THAT PROVES THE FIRST IS NOT VACUOUS: in PIXELS they are somewhere else
	// entirely, so the equality above is about the field and not about nothing having happened.
	CHECK(pad.starsForTest().front().x != doctest::Approx(0.0f));
}
TEST_CASE("the puck's sparks ask for NOTHING at silence — the property they were required to keep") {
	// **THIS IS THE WHOLE RISK OF THE FEATURE, and it is a regression this repo has already paid to
	// remove once.** `perform_pad.h` used to say the pad had no clock, no timer and no accumulated
	// state, citing backlog GJ — the Explore chart dirtying a frame forever at rest for weeks. Sparks
	// need exactly what that refused. What survives is narrower and is asserted here: no sound means
	// no emission means no live spark means NO FRAME REQUESTED.
	//
	// **THE COUNT IS PRODUCED BY THE ASKING.** `updateSparks` returns whether it called `redraw()`, so
	// a build that had stopped calling it cannot answer `true`; a counter standing beside the call
	// could. `redraw()` itself is unobservable headlessly, which is why the line that delivers the
	// repaint is the line that reports it.
	SparkRig r;
	r.puckAt(0.0f, 0.0f);

	size_t asked = 0;
	for (int i = 0; i < 200; ++i) {
		if (r.tick(16, 0.0f)) {
			++asked;
		}
	}
	CHECK(asked == 0);
	CHECK(r.pad->sparksForTest().empty());
	CHECK(r.pad->sparksEmittedForTest() == 0);

	SUBCASE("and a moving puck at silence is still silent — the emission is LEVEL's, not the hand's") {
		// A drag dirties the frame by moving the PUCK, which is the parameter road and not this one;
		// what must not happen is a spark. Ticking while dragging across the disk at zero level emits
		// nothing at all.
		for (int i = 0; i < 40; ++i) {
			r.puckAt(-0.8f + 0.04f * static_cast<float>(i), 0.3f);
			r.tick(16, 0.0f);
		}
		CHECK(r.pad->sparksEmittedForTest() == 0);
		CHECK(r.pad->sparksForTest().empty());
	}

	SUBCASE("NOT VACUOUS: the same pad, the same ticks, with sound, asks nearly every time") {
		// **THE VENUE MUST BE SHOWN CAPABLE OF EXPRESSING THE OUTCOME BEFORE ITS SILENCE COUNTS.**
		// Without this line the zero above is also what a pad with `updateSparks` deleted would answer.
		size_t loud = 0;
		for (int i = 0; i < 200; ++i) {
			if (r.tick(16, 1.0f)) {
				++loud;
			}
		}
		CHECK(loud >= 190);
		CHECK(r.pad->sparksEmittedForTest() > 100);

		// **AND IT GOES QUIET AGAIN WHEN THE SOUND DOES**, which is the property in its live form: the
		// last spark is painted out and then nothing more is asked for, for ever.
		size_t after = 0;
		for (int i = 0; i < 200; ++i) {
			if (r.tick(16, 0.0f)) {
				++after;
			}
		}
		// The tail: the sparks alive when the sound stopped fly on until their lives run out. It is
		// bounded by `kSparkLife` at its longest, which is well under 60 frames.
		CHECK(after > 0);
		CHECK(after < 60);
		CHECK(r.pad->sparksForTest().empty());
	}
}
TEST_CASE("the emission rate is the LEVEL's, through a Poisson process — asserted as a count, not a picture") {
	// **The sparks are emitted as a Poisson process driven by the instantaneous level.** So the
	// claim is arithmetic: over `T` seconds at level `L` the expected count is
	// `kSparkRateMax · L^kSparkLevelGamma · T`. Asserted against an INDEPENDENT restatement of that
	// product rather than against what the code computed, and with a second reading — the ratio between
	// two levels — that a build with the right total and the wrong curve would fail.
	const auto expected = [](float level, double seconds) {
		return static_cast<double>(ui::PerformPad::kSparkRateMax)
		       * std::pow(static_cast<double>(level),
		                  static_cast<double>(ui::PerformPad::kSparkLevelGamma))
		       * seconds;
	};

	// Sixteen seconds of ticks at 60 fps. Long enough that three sigma is a few percent, and the puck
	// sits still at the centre so nothing escapes the aperture early.
	constexpr int kTicks = 1000;
	constexpr int kMs = 16;
	const double seconds = static_cast<double>(kTicks) * static_cast<double>(kMs) / 1000.0;

	const auto run = [&](float level) {
		SparkRig r;
		r.puckAt(0.0f, 0.0f);
		for (int i = 0; i < kTicks; ++i) {
			r.tick(kMs, level);
		}
		return static_cast<double>(r.pad->sparksEmittedForTest());
	};

	const double full = run(1.0f);
	const double half = run(0.5f);

	// **±4 SIGMA ON A POISSON COUNT**, which for 1440 is about 152 — the band a correct build sits
	// inside and a build whose rate is wrong by ten percent does not.
	const double wantFull = expected(1.0f, seconds);
	const double wantHalf = expected(0.5f, seconds);
	CAPTURE(full);
	CAPTURE(wantFull);
	CHECK(std::fabs(full - wantFull) < 4.0 * std::sqrt(wantFull));
	CAPTURE(half);
	CAPTURE(wantHalf);
	CHECK(std::fabs(half - wantHalf) < 4.0 * std::sqrt(wantHalf));

	// **THE CURVE, WHICH THE TWO ABSOLUTE CHECKS ALONE DO NOT PIN.** `kSparkLevelGamma` is 2, so half
	// the level is a QUARTER of the rate; a linear map would put this ratio at 2 and would still be
	// within a few sigma of one of the two bands if `kSparkRateMax` were retuned to suit.
	REQUIRE(half > 0.0);
	const double ratio = full / half;
	CAPTURE(ratio);
	CHECK(ratio == doctest::Approx(std::pow(2.0, ui::PerformPad::kSparkLevelGamma)).epsilon(0.15));

	SUBCASE("and it is a PROCESS, not one spark every N frames — by its VARIANCE") {
		// **THE DIFFERENCE IS WHAT ASKED FOR: a per-frame count emits the same number every tick,
		// a Poisson process clumps and gaps.** The discriminator is the VARIANCE of the per-tick birth
		// count: for a Poisson process it equals the mean, and for any deterministic `ceil(rate·dt)`
		// build it is exactly ZERO.
		//
		// **IT WAS "HOW MANY TICKS EMITTED NOTHING", AND THAT BROKE WHEN THE RATE ROSE** — which is
		// worth recording, because the test was not wrong so much as written against one operating
		// point. At the old 90/s the mean was 1.44 a frame and a quarter of ticks were empty; at 400/s
		// the mean is 6.4 and `exp(−6.4)` is 0.0017, so **fewer than one tick in four hundred is empty
		// and the old bar could not be met by a correct build.** Variance is the same claim without a
		// working point baked into it.
		SparkRig r;
		r.puckAt(0.0f, 0.0f);
		constexpr int kTicksHere = 600;
		std::vector<double> born;
		born.reserve(kTicksHere);
		size_t last = 0;
		for (int i = 0; i < kTicksHere; ++i) {
			r.tick(16, 1.0f);
			const size_t now = r.pad->sparksEmittedForTest();
			born.push_back(static_cast<double>(now - last));
			last = now;
		}
		double mean = 0.0;
		for (double b : born) {
			mean += b;
		}
		mean /= static_cast<double>(born.size());
		double var = 0.0;
		for (double b : born) {
			var += (b - mean) * (b - mean);
		}
		var /= static_cast<double>(born.size() - 1);
		CAPTURE(mean);
		CAPTURE(var);
		// The mean is the rate times the tick, which the absolute checks above already pinned; this is
		// here so the ratio below is a ratio of two live numbers.
		REQUIRE(mean > 1.0);
		// **VARIANCE OVER MEAN IS 1 FOR A POISSON PROCESS AND 0 FOR A COUNTER.** The band is generous
		// because a sample variance of 600 draws is itself noisy (its own relative sd is √(2/599) ≈ 6%),
		// and it excludes 0 by a mile, which is the build this case exists to reject.
		CHECK(var / mean == doctest::Approx(1.0).epsilon(0.25));
	}

	SUBCASE("and the same seed replays exactly — which is what makes every number above assertable") {
		CHECK(run(1.0f) == full);
		CHECK(run(0.5f) == half);
	}
}
TEST_CASE("the two colours divide the sparks the way the two levels divide the sound") {
	// **The wet output and the grain fx output emit two different colours.** The point
	// is not decoration: the grain lane is a DIVERSION, so a grain that goes down it LEAVES the wet and
	// nothing on the surface has ever said which grains went. Two colours in the two levels' ratio is
	// that fact made visible.
	//
	// **SO THE CLAIM IS ABOUT A RATIO, AND A TOTAL CANNOT SEE IT.** A build that emitted exactly the
	// right number of sparks and painted them all one colour passes every count in the case above.
	// That is why the pad counts the two populations separately.
	const auto expected = [](float level, double seconds) {
		return static_cast<double>(ui::PerformPad::kSparkRateMax)
		       * std::pow(static_cast<double>(level),
		                  static_cast<double>(ui::PerformPad::kSparkLevelGamma))
		       * seconds;
	};
	constexpr int kTicks = 1000;
	constexpr int kMs = 16;
	const double seconds = static_cast<double>(kTicks) * static_cast<double>(kMs) / 1000.0;

	const auto run = [&](float wet, float grain) {
		SparkRig r;
		r.puckAt(0.0f, 0.0f);
		for (int i = 0; i < kTicks; ++i) {
			r.tick(kMs, wet, grain);
		}
		const size_t lane = r.pad->laneSparksEmittedForTest();
		const size_t all = r.pad->sparksEmittedForTest();
		REQUIRE(all >= lane); // the lane's are a SUBSET of the total, or the counters disagree
		return std::pair<double, double> { static_cast<double>(all - lane), static_cast<double>(lane) };
	};

	SUBCASE("each colour's rate is ITS OWN level's, independently") {
		// **INDEPENDENT RATES, NOT A SHARED ONE SPLIT BY A HUE.** If the two shared a budget the total
		// would be constant and the colour would say "the sound moved sideways" rather than "more of it
		// went that way". Driven at levels in a 1:2 ratio, which the gamma turns into 1:4 sparks.
		const auto [straight, lane] = run(0.5f, 1.0f);
		const double wantStraight = expected(0.5f, seconds);
		const double wantLane = expected(1.0f, seconds);
		CAPTURE(straight);
		CAPTURE(wantStraight);
		CHECK(std::fabs(straight - wantStraight) < 4.0 * std::sqrt(wantStraight));
		CAPTURE(lane);
		CAPTURE(wantLane);
		CHECK(std::fabs(lane - wantLane) < 4.0 * std::sqrt(wantLane));
		// AND THE RATIO, which is the reading a player takes off the picture.
		CHECK(lane / straight == doctest::Approx(4.0).epsilon(0.15));
	}

	SUBCASE("a silent lane lights NO second colour, however loud the straight road is") {
		// **THE MODE CHECK, IN THE FORM THE PAD CAN ANSWER.** The grain lane is gated off entirely in
		// Burst, so the second colour will simply not appear there — and a colour that never
		// appears is indistinguishable from a colour that is broken. This pins the pad's half: handed a
		// zero lane level it emits exactly zero lane sparks, so an absent second colour in those modes
		// is the ENGINE saying nothing went down the lane and not the surface failing to draw it.
		const auto [straight, lane] = run(1.0f, 0.0f);
		CHECK(lane == 0.0);
		CHECK(straight > 100.0); // not vacuous: the first colour is pouring out
	}

	SUBCASE("and the reverse — everything down the lane leaves the first colour dark") {
		const auto [straight, lane] = run(0.0f, 1.0f);
		CHECK(straight == 0.0);
		CHECK(lane > 100.0);
	}

	SUBCASE("a spark carries WHICH ROAD it came from, and the draw reads it") {
		// **THE FLAG IS ON THE SPARK, not decided at paint time from something else** — which is what
		// makes a lane spark stay a lane spark for its whole flight, including after the level that
		// made it has moved on.
		SparkRig r;
		r.puckAt(0.0f, 0.0f);
		r.tick(16, 0.0f, 0.0f);
		r.tick(16, 1.0f, 1.0f);
		size_t lane = 0;
		size_t straight = 0;
		for (const ui::PerformPad::Spark& s : r.pad->sparksForTest()) {
			if (s.fromLane) {
				++lane;
			}
			else {
				++straight;
			}
		}
		REQUIRE(lane > 0);
		REQUIRE(straight > 0);
		CHECK(lane == r.pad->laneSparksEmittedForTest());
		CHECK(lane + straight == r.pad->sparksEmittedForTest());

		// **AND THE POPULATIONS DO NOT BLEED INTO EACH OTHER OVER TIME.** Driven with the lane silent
		// from here, the lane sparks age out and the count of lane-flagged sparks only ever falls — a
		// flag recomputed at paint time from the CURRENT level would flip them all to straight at once.
		const size_t laneBorn = r.pad->laneSparksEmittedForTest();
		for (int i = 0; i < 10; ++i) {
			r.tick(16, 1.0f, 0.0f);
		}
		CHECK(r.pad->laneSparksEmittedForTest() == laneBorn); // no new lane sparks: the lane is silent
		size_t stillLane = 0;
		for (const ui::PerformPad::Spark& s : r.pad->sparksForTest()) {
			if (s.fromLane) {
				++stillLane;
			}
		}
		CHECK(stillLane > 0);        // they are still flying, still flagged
		CHECK(stillLane <= laneBorn); // and none was invented
	}
}
TEST_CASE("backlog HR: a spark's path is a CLOSED FORM, which is what let it move to the GPU") {
	// **Spark translation and any glitter properties belong, ideally, purely on the GPU.** That is
	// a structural change rather than a port, and this case is the reason why: an INTEGRATOR cannot move
	// to a vertex shader, because a vertex shader has nowhere to keep the thing it is integrating. A
	// CLOSED FORM can, because it is a function of birth data and the clock and of nothing else.
	//
	// **THE FORMULA IS ASSERTED HERE AGAINST AN INDEPENDENT RESTATEMENT. THE SHADER IMPLEMENTING IT IS
	// NOT, AND CANNOT BE** — a fragment stage has no headless seam. `vs_spark.sc` agreeing with
	// `sparkOffsetAt` is shown by photographing the GPU path against the CPU reference path, which
	// evaluates this same function. That split is stated plainly because the second half is weaker
	// evidence than the first and should not be read as though it were the same.
	const float k = ui::PerformPad::kSparkDrag;

	SUBCASE("the displacement is v0·(1 − e^(−k·t))/k, restated rather than re-called") {
		for (const float t : { 0.0f, 0.01f, 0.1f, 0.25f, 0.55f, 2.0f }) {
			CAPTURE(t);
			// THE INDEPENDENT RESTATEMENT: the integral of `v0·e^(−k·s)` from 0 to t, written out.
			const double want = (1.0 - std::exp(-static_cast<double>(k) * t)) / static_cast<double>(k);
			const auto [dx, dy] = ui::PerformPad::sparkOffsetAt(1.0f, -2.0f, t, k);
			CHECK(dx == doctest::Approx(want).epsilon(1e-5));
			CHECK(dy == doctest::Approx(-2.0 * want).epsilon(1e-5));
		}
	}

	SUBCASE("it is bounded — a spark cannot travel further than v0/k however long it lives") {
		// The terminal displacement, which is what makes `kSparkSpread` and `kSparkDrag` together a
		// REACH rather than two numbers whose product nobody has looked at. At 0.8 and 2.2 that is
		// 0.36 of a radius, which is the figure backlog HR reports as the spray's reach.
		const auto [dx, dy] = ui::PerformPad::sparkOffsetAt(ui::PerformPad::kSparkSpread, 0.0f, 1.0e6f, k);
		CHECK(dx == doctest::Approx(ui::PerformPad::kSparkSpread / k).epsilon(1e-4));
		CHECK(dy == doctest::Approx(0.0f));
	}

	SUBCASE("MEASURE: how far the OLD per-tick integrator drifted from it") {
		// **THE AGREEMENT NUMBER, and it is a measurement rather than a bound.** Before this pass the
		// CPU stepped `p += v·dt; v *= e^(−k·dt)` every tick — an explicit Euler step whose error is
		// O(dt²) per step. The closed form is exact. So the two do NOT agree to float precision and it
		// would be dishonest to assert that they do; what is worth knowing is HOW FAR apart they are
		// over a spark's whole life, because that is the visible difference between the build already
		// seen and this one.
		constexpr float kDt = 1.0f / 60.0f;
		constexpr float kV0 = 0.8f;
		float p = 0.0f;
		float v = kV0;
		float t = 0.0f;
		float worst = 0.0f;
		for (int i = 0; i < 40; ++i) {
			p += v * kDt;
			v *= std::exp(-k * kDt);
			t += kDt;
			const auto [exact, unused] = ui::PerformPad::sparkOffsetAt(kV0, 0.0f, t, k);
			(void)unused;
			worst = std::max(worst, std::abs(p - exact));
		}
		// In AXIS units — radii — so it converts to pixels by the disk's radius.
		MESSAGE("old Euler integrator vs the closed form, worst over a spark's life: " << worst
		        << " radii (" << (worst * 150.0f) << " px on a 300px disk)");
		// **A BOUND LOOSE ENOUGH TO BE A FACT RATHER THAN A RESTATEMENT OF TODAY'S NUMBER**, and tight
		// enough to fail if somebody replaced the closed form with something that is not the same curve.
		CHECK(worst < 0.02f);
	}
}
TEST_CASE("backlog HR: the GPU path and the CPU reference submit the same sparks in the same places") {
	// **WHAT THIS CAN AND CANNOT PROVE, SAID FIRST, because the difference is the whole point.**
	//
	// It proves that the two paths agree about WHICH sparks are drawn and WHERE — same population,
	// same aperture cull, same positions to the bit, from the same `sparkPointAt`. That is everything
	// on the C++ side of the seam.
	//
	// **IT DOES NOT PROVE THAT `vs_spark.sc` IMPLEMENTS THE SAME FORMULA**, and nothing headless can:
	// a vertex stage has no seam. That half is evidenced by photographing the two paths against each
	// other (`widget-shot --spark-cpu`), which is weaker evidence and is labelled as such in backlog HR.
	//
	// **AND A WHOLE-IMAGE RMSE CANNOT DO IT EITHER — MEASURED, which is why it is not used.** Two runs
	// sample a random process at different instants, so a GPU frame against a CPU frame scored 0.0254
	// while a CPU frame against ANOTHER CPU FRAME scored 0.0250 — and a frame with NO SPARKS AT ALL
	// scored 0.0252 against one full of them. A metric that cannot separate "sparks" from "no sparks"
	// cannot separate two spark paths, and reporting the first number alone would have looked like
	// agreement.
	SparkRig r;
	r.puckAt(0.25f, -0.1f);
	r.tick(16, 0.0f, 0.0f);
	for (int i = 0; i < 30; ++i) {
		r.tick(16, 1.0f, 0.8f);
	}
	REQUIRE(r.pad->liveSparks() > 50); // not vacuous: a real population to disagree about

	visage::Canvas canvas;
	REQUIRE(r.pad->sparkGpuForTest()); // the GPU path is what ships, so it is what is measured first
	r.pad->draw(canvas);
	const std::vector<std::pair<float, float>> gpu(r.pad->drawnSparksForTest().begin(),
	                                               r.pad->drawnSparksForTest().end());

	// **THE SAME STATE, THE OTHER PATH — no tick between them**, so the clock has not moved and the
	// only variable is who computes the position.
	r.pad->setSparkGpuForTest(false);
	r.pad->draw(canvas);
	const std::vector<std::pair<float, float>>& cpu = r.pad->drawnSparksForTest();

	REQUIRE(gpu.size() == cpu.size());
	REQUIRE(!gpu.empty());
	for (size_t i = 0; i < gpu.size(); ++i) {
		CAPTURE(i);
		// **BIT FOR BIT.** Both went through one `sparkPointAt`; an epsilon here would pass a build in
		// which the two paths had quietly acquired two different formulas.
		CHECK(gpu[i].first == cpu[i].first);
		CHECK(gpu[i].second == cpu[i].second);
	}

	SUBCASE("and the record really is the moving position, not the birth position") {
		// **NOT VACUOUS.** Every equality above would also hold if both paths recorded where the spark
		// was BORN — which is exactly the bug the GPU path invites, because that is where it puts the
		// quad. So the drawn positions must have MOVED since birth.
		size_t moved = 0;
		const std::span<const ui::PerformPad::Spark> live = r.pad->sparksForTest();
		REQUIRE(live.size() == cpu.size());
		for (size_t i = 0; i < live.size(); ++i) {
			const auto [bx, by] = feathers::ui::perform_pad::pointOf(r.disk, live[i].x, live[i].y);
			if (std::hypot(cpu[i].first - bx, cpu[i].second - by) > 0.5f) {
				++moved;
			}
		}
		CAPTURE(moved);
		CHECK(moved > live.size() / 2);
	}
}
TEST_CASE("the sparks are legibly apart from the starfield and from each other — asserted as numbers") {
	// **THIS CASE EXISTS BECAUSE THE FIRST PAIR OF INKS WAS CHOSEN ON SEMANTICS AND WAS ILLEGIBLE.**
	// `kAccent` (the waypoint fill) and `kMod` ("something else acts on this") were each well argued and
	// were both pale cool blues 33/765 apart, in the starfield's own family. On the build the sparks
	// were hard to tell apart from the starfield background, so they need more distinct colouring and a
	// larger size.
	//
	// **THE POINT IS THE ORDER OF THE TEST: legibility is a NUMBER and it settles before meaning is
	// worth arguing.** Nothing about the old choice was checkable by any case we had, because every
	// case was about counts and positions — a picture can be perfectly correct and unreadable.
	const auto channels = [](uint32_t argb) {
		return std::array<int, 3> { static_cast<int>((argb >> 16) & 0xFFu),
			                        static_cast<int>((argb >> 8) & 0xFFu),
			                        static_cast<int>(argb & 0xFFu) };
	};
	const auto apart = [&](uint32_t a, uint32_t b) {
		const auto ca = channels(a);
		const auto cb = channels(b);
		return std::abs(ca[0] - cb[0]) + std::abs(ca[1] - cb[1]) + std::abs(ca[2] - cb[2]);
	};

	// **THE BAR IS 100 OF A POSSIBLE 765**, which is comfortably above the 33 that failed in the
	// hands and comfortably below what the seed palette offers, so it is a floor rather than a
	// restatement of the values chosen.
	constexpr int kApart = 100;

	SUBCASE("each spark ink is far from the STAR ink") {
		CAPTURE(apart(ui::PerformPad::kSparkInk, ui::PerformPad::kStarInk));
		CHECK(apart(ui::PerformPad::kSparkInk, ui::PerformPad::kStarInk) >= kApart);
		CAPTURE(apart(ui::PerformPad::kSparkLaneInk, ui::PerformPad::kStarInk));
		CHECK(apart(ui::PerformPad::kSparkLaneInk, ui::PerformPad::kStarInk) >= kApart);
	}

	SUBCASE("and the two spark inks are far from EACH OTHER") {
		// The half that actually failed: both were legible against the ground and identical to one
		// another, so the second colour carried nothing.
		CAPTURE(apart(ui::PerformPad::kSparkInk, ui::PerformPad::kSparkLaneInk));
		CHECK(apart(ui::PerformPad::kSparkInk, ui::PerformPad::kSparkLaneInk) >= kApart);
	}

	SUBCASE("and the OLD pair would fail this case, which is what makes it a real bar") {
		// **NOT VACUOUS.** Without this, a bar of 100 might simply be one the palette cannot help
		// passing. These are the two tokens that shipped and were reported illegible.
		CHECK(apart(feathers::tokens::kAccent, feathers::tokens::kMod) < kApart);
	}

	SUBCASE("a spark is LARGER than the brightest star for its whole life, and at its native size") {
		// **SIZE IS THE SECOND SEPARATION AND THE RAMP NEVER CROSSES THE FIELD.** A spark that shrank
		// into the star range would look like a star on its way out — the complaint arriving late.
		CHECK(ui::PerformPad::kSparkSizeDeath > ui::PerformPad::kBrightStar);
		CHECK(ui::PerformPad::kSparkSizeBirth > ui::PerformPad::kSparkSizeDeath);

		// **AND THE CLAIM IS ABOUT WHAT REACHES THE GLASS, SO THE ASSERTION IS ON THE NATIVE SIZE** —
		// CLAUDE.md's scale rule. A pad laid out larger than its reference draws larger furniture, and
		// a spark that did not scale with it would shrink back into the starfield on a big window.
		SparkRig r;
		r.puckAt(0.0f, 0.0f);
		r.tick(16, 0.0f, 0.0f);
		r.tick(16, 1.0f, 1.0f);
		REQUIRE(!r.pad->sparksForTest().empty());
		visage::Canvas canvas;
		r.pad->draw(canvas);
		const float small = r.pad->lastSparkNativeSizeForTest();
		REQUIRE(small > 0.0f);
		CHECK(small >= ui::PerformPad::kSparkSizeDeath);

		// THE SAME PAD, LAID OUT BIGGER: the native size grows with it.
		r.p.panel->setBounds(0, 0, static_cast<int>(r.p.panel->minimumSize().x + 400.0f),
		                     static_cast<int>(r.p.panel->minimumSize().y + 400.0f));
		r.p.panel->resized();
		REQUIRE(r.pad->scale() > 1.2f); // not vacuous: the pad really is bigger
		r.pad->draw(canvas);
		const float large = r.pad->lastSparkNativeSizeForTest();
		CAPTURE(small);
		CAPTURE(large);
		CHECK(large > small * 1.15f);
	}
}
TEST_CASE("a spark is born with the PUCK'S VELOCITY at the instant it was emitted") {
	// **Sparks carry momentum set by the puck's velocity vector at the moment they are emitted.** Two
	// halves, and both are values:
	//   1. the measured velocity is the difference quotient over the tick that measured it, and
	//   2. every spark born on that tick carries `kSparkInherit` of it, plus a kick bounded by
	//      `kSparkSpread`.
	// **THE CONTRAST IS THE FEATURE**, so the still case is checked beside the moving one: sparks
	// emitted while the hand is still must NOT drift, or the inheritance is decoration.
	SparkRig r;

	// A move of 0.05 radii over 50 ms is 1.0 radii a second — comfortably under `kSparkSpeedMax`, so
	// this case is about the inheritance and not about the clamp (which has its own case below).
	constexpr float kMove = 0.05f;
	constexpr int64_t kGapMs = 50;
	constexpr float kSpeed = kMove / (static_cast<float>(kGapMs) / 1000.0f);

	r.puckAt(0.0f, 0.0f);
	r.tick(16, 0.0f); // the first sample: a position, not a motion
	CHECK(r.pad->puckVelocityForTest().first == doctest::Approx(0.0f));
	r.drain();

	r.puckAt(kMove, 0.0f);
	REQUIRE(r.tick(kGapMs, 1.0f)); // it asked for a frame, so something was emitted

	const auto [vx, vy] = r.pad->puckVelocityForTest();
	CAPTURE(vx);
	CHECK(vx == doctest::Approx(kSpeed).epsilon(1e-3));
	CHECK(vy == doctest::Approx(0.0f).epsilon(1e-3));

	const std::span<const ui::PerformPad::Spark> born = r.pad->sparksForTest();
	REQUIRE(!born.empty()); // not vacuous: there are sparks to make a claim about
	float meanVx = 0.0f;
	for (const ui::PerformPad::Spark& s : born) {
		// **THE INHERITED PART IS EXACT AND THE KICK IS BOUNDED**, which together say the velocity is
		// the puck's and not merely correlated with it. A build that inherited half, or that inherited
		// the puck's POSITION scaled, fails the first line; a build that used a bigger spread to fake
		// the direction fails the second.
		CHECK(std::hypot(s.vx - kSpeed * ui::PerformPad::kSparkInherit, s.vy)
		      <= ui::PerformPad::kSparkSpread + 1e-4f);
		// AND IT REALLY WAS EMITTED AT THE PUCK, which is where the momentum is being claimed from.
		CHECK(s.x == doctest::Approx(kMove).epsilon(1e-4));
		CHECK(s.y == doctest::Approx(0.0f).epsilon(1e-4));
		meanVx += s.vx;
	}
	meanVx /= static_cast<float>(born.size());

	SUBCASE("while a STILL hand emits sparks that go nowhere in particular") {
		// **THE SECOND READING THAT MAKES THE FIRST NON-VACUOUS.** With no move, the only velocity a
		// spark can have is the kick — mean zero over enough of them. If this drifted, `kSparkInherit`
		// would be reading something that is not the puck's motion.
		SparkRig still;
		still.puckAt(0.0f, 0.0f);
		float sum = 0.0f;
		size_t seen = 0;
		for (int i = 0; i < 200; ++i) {
			const size_t before = still.pad->sparksEmittedForTest();
			still.tick(16, 1.0f);
			const std::span<const ui::PerformPad::Spark> live = still.pad->sparksForTest();
			// The sparks added by THIS tick are the tail of the vector — nothing reorders it, and the
			// ageing pass ran before the emission.
			const size_t added = still.pad->sparksEmittedForTest() - before;
			for (size_t k = live.size() - std::min(added, live.size()); k < live.size(); ++k) {
				sum += live[k].vx;
				++seen;
			}
		}
		REQUIRE(seen > 200);
		const float meanStill = sum / static_cast<float>(seen);
		CAPTURE(meanStill);
		// The kick is uniform in direction, so its mean falls off as 1/√n; at several hundred sparks
		// a tenth of the spread is many sigma away from any real drift.
		CHECK(std::fabs(meanStill) < ui::PerformPad::kSparkSpread * 0.1f);
		// **AND THE MOVING CASE IS CLEAR OF IT BY THE WHOLE INHERITANCE**, which is the intended contrast
		// stated as a number rather than as two pictures.
		CAPTURE(meanVx);
		CHECK(meanVx > kSpeed * ui::PerformPad::kSparkInherit * 0.5f);
	}

	SUBCASE("and they are DRAWN, at the pixels the disk maps them to") {
		// **THE SIMULATION BEING RIGHT IS NOT THE PICTURE BEING THERE.** Without this, `drawSparks`
		// could be deleted and every number above would stay green with nothing on screen — the shape
		// CLAUDE.md calls for where a repaint cannot be observed headlessly, and the same one
		// `starsForTest()` already uses one function over.
		visage::Canvas canvas;
		r.pad->draw(canvas);
		const std::vector<std::pair<float, float>>& ink = r.pad->drawnSparksForTest();
		REQUIRE(ink.size() == born.size());
		for (size_t i = 0; i < ink.size(); ++i) {
			CAPTURE(i);
			// **CONVERTED BACK THROUGH THE DISK**, so this is a claim about WHERE, not merely that
			// something was submitted. The pad's own `pointOf` maps axis units to pixels; a build that
			// drew every spark at the origin, or in axis units, fails here.
			const auto [wantX, wantY] =
			    feathers::ui::perform_pad::pointOf(r.disk, born[i].x, born[i].y);
			CHECK(ink[i].first == doctest::Approx(wantX).epsilon(1e-4));
			CHECK(ink[i].second == doctest::Approx(wantY).epsilon(1e-4));
		}
		// AND THE RECORD IS EMPTIED BY EACH DRAW rather than accumulated, or a still pad would grow a
		// longer and longer list of ink for the same picture.
		r.pad->draw(canvas);
		CHECK(r.pad->drawnSparksForTest().size() == born.size());
	}

	SUBCASE("a JUMP is not a velocity — the puck teleports, the sparks do not") {
		// **`Perf X`/`Perf Y` ARE MODULATION DESTINATIONS AND A CLICK LANDS THE PUCK ANYWHERE IN ONE
		// TICK.** The difference quotient for a click from the centre to the rim over 16 ms is 62
		// radii a second, for something nobody dragged. `kSparkSpeedMax` keeps the DIRECTION, which is
		// real, and discards the magnitude, which is an artefact of the sampling.
		SparkRig j;
		j.puckAt(0.0f, 0.0f);
		j.tick(16, 0.0f);
		j.puckAt(0.0f, -1.0f); // straight to the top of the disk, one tick
		j.tick(16, 1.0f);

		const auto [jx, jy] = j.pad->puckVelocityForTest();
		CAPTURE(jx);
		CAPTURE(jy);
		// CLAMPED TO EXACTLY THE CEILING — not merely "finite", which an unclamped build also is.
		CHECK(std::hypot(jx, jy) == doctest::Approx(ui::PerformPad::kSparkSpeedMax).epsilon(1e-3));
		// AND POINTING THE WAY THE PUCK WENT: up, which is negative y in every screen space here.
		CHECK(jy < 0.0f);
		CHECK(jx == doctest::Approx(0.0f).epsilon(1e-3));
	}
}
TEST_CASE("the panel's BODY folds like a rail: the grid goes, the strip stays, and the room comes back") {
	// The main body collapses in the window as well, and Perform is the same width as the main panel.
	// "As well" is the specification — the body JOINS the
	// two rails rather than replacing anything, so the panel has three independently foldable things
	// and eight legal states.
	//
	// **THE BODY IS NOT A RAIL THAT OWNS THE GRID**, and the split is what this case pins: the RAIL
	// holds the state and draws the strip, the PANEL reads that state as one more term in the
	// visibility line it already computes per cell. So the two things to check are that the bool moved
	// AND that the cells went — either alone passes on a build where the other is broken.
	PerformPanel p;
	REQUIRE(p.panel != nullptr);
	ui::BodyRail* body = p.panel->bodyRailForTest();
	REQUIRE(body != nullptr);

	// **IT OPENS BY DEFAULT**, for the reason the circle does: the grid is what the panel IS, and a
	// device panel that arrived folded would be an expand button that expands into a strip.
	CHECK_FALSE(body->collapsed());
	CHECK_FALSE(p.panel->bodyCollapsed());

	const float openWidth = p.panel->minimumSize().x;
	visage::Frame* live = nullptr;
	size_t liveSeen = 0;
	for (visage::Frame* c : p.panel->cellsForTest()) {
		if (c->isVisible()) {
			++liveSeen;
			live = c;
		}
	}
	REQUIRE(live != nullptr); // not vacuous: there is a grid to fold away

	setFolds(p, false, true, false);
	CHECK(body->collapsed());
	CHECK(p.panel->bodyCollapsed());
	// **MINIMISED, NEVER GONE** — `rail.h`'s rule, and the same 34px the other two keep.
	CHECK(body->railWidth() == ui::Rail::kCollapsedWidth);
	CHECK(static_cast<float>(body->width()) == doctest::Approx(ui::Rail::kCollapsedWidth));
	// **AND THE CONTROLS REALLY WENT.** Every cell, not the one sampled above: a fold that hid the
	// first row and left the rest would pass a single-cell check and be visibly wrong.
	for (visage::Frame* c : p.panel->cellsForTest()) {
		CHECK_FALSE(c->isVisible());
	}

	// **THE ROOM CAME BACK, WHICH IS THE HALF A VISIBILITY CHECK CANNOT SEE.** `minimumSize()` is read
	// live by the host on every layout pass, so this is what makes the window shrink rather than leave
	// 392 pixels of empty ground where the grid was.
	CHECK(p.panel->minimumSize().x
	      == doctest::Approx(ui::Rail::kCollapsedWidth + p.panel->railsWidth()));
	// THE GRID AND ITS SIDE COLUMN (backlog IG) — the whole body, which is what folded.
	CHECK(openWidth - p.panel->minimumSize().x
	      == doctest::Approx(ui::GenericExpanded::kGridWidth + ui::GenericExpanded::kSideColumnWidth
	                         - ui::Rail::kCollapsedWidth));

	// **AND IT COMES BACK.** A body that only ever folded would take the grid away for good, which is
	// the honesty rule failing on a feature; the half a one-direction case would miss.
	setFolds(p, true, true, false);
	CHECK(p.panel->minimumSize().x == doctest::Approx(openWidth));
	size_t liveAgain = 0;
	for (visage::Frame* c : p.panel->cellsForTest()) {
		liveAgain += c->isVisible() ? 1u : 0u;
	}
	CHECK(liveAgain == liveSeen);

	// **THE WAY BACK IS THE BUTTON THAT CLOSED IT**, real in both states and inside the strip it has to
	// be pressed in. `rail.h` promises this for every rail; nothing had ever asked a 34px one.
	setFolds(p, false, true, false);
	const visage::Bounds t = body->toggleBounds();
	CHECK(t.x() >= 0.0f);
	CHECK(t.right() <= static_cast<float>(body->width()));
	CHECK(t.bottom() <= static_cast<float>(body->height()));
	CHECK(body->toggleHit({ t.x() + t.width() * 0.5f, t.y() + t.height() * 0.5f }));
}
TEST_CASE("FOUR strips and nothing else is a LEGAL state, not a broken one") {
	// **THE STATE THE FOLDS MAKE REACHABLE, and the one nothing else in this file could reach.** With
	// the body, the circle and BOTH fx rails closed the panel holds no content at all — and every
	// arithmetic on it that used to be anchored by the grid is now anchored by nothing. That is exactly
	// where a width sums to zero or a height collapses to a clipped word.
	//
	// **IT WAS THREE STRIPS UNTIL THE FX LANES SPLIT INTO TWO RAILS (2026-09-20).** The number in the
	// title is the point of the case, so it is in the title.
	PerformPanel p;
	REQUIRE(p.panel != nullptr);
	ui::BodyRail* body = p.panel->bodyRailForTest();
	REQUIRE(body != nullptr);
	ui::CircleRail& circle = p.panel->circleRailForTest();
	ui::FxLaneRail& grain = p.panel->grainRailForTest();
	ui::FxLaneRail& fx = p.panel->postRailForTest();

	setFolds(p, false, false, false);

	// FOUR COLUMNS OF 34 AND NO MORE.
	CHECK(p.panel->minimumSize().x == doctest::Approx(4.0f * ui::Rail::kCollapsedWidth));
	// **AND A HEIGHT, BECAUSE NOTHING OPEN IS ASKING FOR ANY.** The grid's rows left with the grid and
	// the circle's block left with the circle; what is left to fit is an upright word per strip, which
	// is `Rail::kCollapsedMinimumHeight`. A panel that went on claiming the hidden grid's height would
	// be asserting a need it does not have, and one that claimed nothing would clip all three labels.
	CHECK(p.panel->minimumSize().y == doctest::Approx(ui::Rail::kCollapsedMinimumHeight));
	CHECK(p.panel->minimumSize().y > ui::Rail::kPadTop + ui::Rail::kToggleSize);

	// SIDE BY SIDE, IN ORDER, WITH NOTHING BETWEEN THEM AND NOTHING OVER THE EDGE.
	CHECK(static_cast<float>(body->x()) == doctest::Approx(0.0f));
	CHECK(static_cast<float>(body->x() + body->width()) == doctest::Approx(circle.x()));
	CHECK(static_cast<float>(circle.x() + circle.width()) == doctest::Approx(grain.x()));
	CHECK(static_cast<float>(grain.x() + grain.width()) == doctest::Approx(fx.x()));
	CHECK(static_cast<float>(fx.x() + fx.width()) == doctest::Approx(p.panel->width()));

	// **AND EACH ONE IS STILL A WAY BACK.** Four strips with an unreachable toggle is the broken
	// version of this state, and it looks identical in a photograph.
	for (ui::Rail* rail : { static_cast<ui::Rail*>(body), static_cast<ui::Rail*>(&circle),
		                    static_cast<ui::Rail*>(&grain), static_cast<ui::Rail*>(&fx) }) {
		REQUIRE(rail->collapsed());
		CHECK(static_cast<float>(rail->width()) == doctest::Approx(ui::Rail::kCollapsedWidth));
		CHECK(static_cast<float>(rail->height()) == doctest::Approx(p.panel->height()));
		const visage::Bounds t = rail->toggleBounds();
		CHECK(t.x() >= 0.0f);
		CHECK(t.right() <= static_cast<float>(rail->width()));
		CHECK(t.bottom() <= static_cast<float>(rail->height()));
		CHECK(rail->toggleHit({ t.x() + t.width() * 0.5f, t.y() + t.height() * 0.5f }));
	}

	// NOT VACUOUS: nothing is on screen behind them.
	for (visage::Frame* c : p.panel->cellsForTest()) {
		CHECK_FALSE(c->isVisible());
	}
	CHECK_FALSE(p.panel->padForTest().isVisible());

	// **AND EVERY ONE OF THE EIGHT STATES IS LEGAL.** The three are independent, so this is the walk
	// that says so rather than three pairs that each hold the third still.
	for (const bool wantBody : { false, true }) {
		for (const bool wantCircle : { false, true }) {
			for (const bool wantFx : { false, true }) {
				CAPTURE(wantBody);
				CAPTURE(wantCircle);
				CAPTURE(wantFx);
				setFolds(p, wantBody, wantCircle, wantFx);
				CHECK(p.panel->bodyCollapsed() == !wantBody);
				CHECK(circle.collapsed() == !wantCircle);
				CHECK(fx.collapsed() == !wantFx);
				// THE THREE COLUMNS STILL FILL THE PANEL EXACTLY, whatever the combination.
				CHECK(static_cast<float>(body->width()) + p.panel->railsWidth()
				      == doctest::Approx(static_cast<float>(p.panel->width())));
				CHECK(static_cast<float>(p.panel->width()) > 0.0f);
				CHECK(static_cast<float>(p.panel->height()) > 0.0f);
			}
		}
	}
}
TEST_CASE("a panel that did NOT ask to be collapsible is the panel it was before the body could fold") {
	// **THE OPT-OUT IS THE HALF NOBODY WOULD NOTICE BREAKING.** `GenericExpanded` builds its `BodyRail`
	// unconditionally — child order is fixed at construction and visage has no reorder, so the rail has
	// to exist before the first cell whether or not it is ever used — and only `setBodyCollapsible`
	// makes it visible. Every other device's panel derives from this base, so "a panel that does not ask
	// is byte-for-byte the panel it was" is a claim about SEVEN surfaces, and it was a reading until
	// this case. The next device to inherit a strip nobody asked for is what this stops.
	//
	// **A BARE `GenericExpanded`, BUILT HERE, BECAUSE NOTHING IN THE PRODUCT BUILDS ONE.** Every entry in
	// `expanded_views.cpp` names a subclass, so the opted-out panel has no instance to go and find —
	// which is exactly why it needs constructing rather than looking up. The FILTER descriptor is used
	// for no reason beyond being a real one that is not SKLITTER's.
	namespace flt = feathers::fx::filter;
	ModFixture f;
	ui::WidgetRegistry registry;
	const size_t slot = f.bridge.addFxDevice(flt::kTraits.tag);
	f.bridge.refresh();
	ui::GenericExpanded panel(f.deviceHost, registry, f.bridge.fxDeviceAt(slot), flt::kDescriptor);

	// THE RAIL IS THERE AND IT IS INVISIBLE, which is both halves: it had to be built, and it must not
	// be drawn or hit-tested. visage skips invisible children in `frameAtPoint` and in the draw walk, so
	// invisibility is the whole of the opt-out.
	REQUIRE(panel.bodyRailForTest() != nullptr);
	CHECK_FALSE(panel.bodyRailForTest()->isVisible());
	CHECK_FALSE(panel.bodyCollapsed());

	// **THE GEOMETRY IS THE PRE-CHANGE ARITHMETIC, RESTATED RATHER THAN READ OFF THE PANEL'S OWN
	// CONSTANTS.** 2·kPad + min(live, kPerRow)·kCellWidth, spelled out, so a change to any of the three
	// that moved this panel would be a red here instead of two expressions agreeing with each other.
	const visage::Point want = panel.minimumSize();
	const auto across = std::min(static_cast<float>(panel.liveCells()), 4.0f);
	CHECK(want.x == doctest::Approx(24.0f + std::max(1.0f, across) * 92.0f));
	panel.setBounds(0, 0, static_cast<int>(want.x), static_cast<int>(want.y));
	panel.resized();
	const auto visibleCells = [&] {
		size_t seen = 0;
		for (visage::Frame* c : panel.cellsForTest()) {
			seen += c->isVisible() ? 1u : 0u;
		}
		return seen;
	};
	CHECK(visibleCells() == panel.liveCells());
	CHECK(panel.liveCells() > 0u); // not vacuous: FILTER has controls

	// **AND THE OPT-OUT HOLDS EVEN IF SOMETHING REACHES THE RAIL AND FOLDS IT.** `bodyCollapsed()` is
	// `visible && collapsed`, deliberately — so a stray `setCollapsed(true)` on a rail the panel never
	// turned on changes the rail's own bool and NOTHING about the panel. The weaker design, keying off
	// the bool alone, would fold a panel that has no chevron to unfold it with.
	REQUIRE(panel.bodyRailForTest()->setCollapsed(true));
	CHECK_FALSE(panel.bodyCollapsed());
	panel.resized();
	CHECK(visibleCells() == panel.liveCells());
	CHECK(panel.minimumSize().x == doctest::Approx(want.x));
	CHECK(panel.minimumSize().y == doctest::Approx(want.y));
}
TEST_CASE("a closed rail's strip is tall enough for the longest label any rail carries") {
	// **`Rail::kCollapsedLabelRun` IS A CHOSEN NUMBER WITH A STATED JOB, AND THIS IS THE JOB.** It has
	// to be a compile-time constant — `GenericExpanded::minimumSize` returns it as a height floor — and
	// how long a word is at a size is a FONT measurement, so the arithmetic cannot be written in the
	// header. What CAN be done is what `ZoneCoordCell::minimumSquareSide` does one widget over: ask the
	// font at run time and require the constant to clear it.
	//
	// **THE FAILURE IT GUARDS IS SILENT AND LEAVES NO WRONG PIXEL.** `Canvas::text` draws inside the box
	// it is given and says nothing about what did not fit, so a strip one pixel too short for its word
	// is a rail called `CONTROL`, with no red anywhere and nothing in the picture that looks broken.
	//
	// **THE LABELS ARE ASKED OF THE RAILS, NOT RESTATED.** Six strings written into this case would pin
	// it to itself; `labelForTest()` pins it to the rails, so a seventh subclass or a renamed one is
	// covered by adding it to the walk rather than by remembering a number.
	namespace sc = feathers::fx::sklitter;
	ModFixture f;
	ui::WidgetRegistry registry;
	ui::ModState mod;
	ui::EditHistory history;
	const size_t slot = f.bridge.addFxDevice(sc::kTraits.tag);
	f.bridge.refresh();
	std::unique_ptr<ui::ExpandedView> sklitter = ui::makeExpandedView(
	    f.deviceHost, &f.bridge, registry, f.bridge.fxDeviceAt(slot), sc::kDescriptor);
	auto* sklitterPanel = dynamic_cast<ui::SklitterExpanded*>(sklitter.get());
	REQUIRE(sklitterPanel != nullptr);
	REQUIRE(sklitterPanel->bodyRailForTest() != nullptr);
	ui::EngineRail engineRail(f.bridge, registry);
	ui::ModPanel modPanel(f.bridge, f.deviceHost, registry, mod, history);
	ui::FxLane lane(f.bridge, registry);
	// THE STAGE'S STRIP (backlog NF) carries one of TWO words, as the header's switch says — both are measured.
	ui::StageRail stageExplore;
	ui::StageRail stagePerform;
	stagePerform.setRailLabel(ui::StageRail::kPerformLabel);

	const std::vector<ui::Rail*> rails { &engineRail,
		                                 &modPanel,
		                                 &lane,
		                                 &stageExplore,
		                                 &stagePerform,
		                                 &sklitterPanel->circleRailForTest(),
		                                 &sklitterPanel->grainRailForTest(),
		                                 &sklitterPanel->postRailForTest(),
		                                 sklitterPanel->bodyRailForTest() };
	// THE SAME HAND-MAINTAINED WALK THE `expandedWidth()` INVARIANT KEEPS, and the same admission: the
	// grep that finds them all is `class .* : public Rail`.
	//
	// **SEVEN SINCE THE FX LANES SPLIT INTO TWO RAILS (2026-09-20), AND THE SEVENTH WAS MISSED ON THE
	// FIRST PASS.** `FX LANES` became `GRAIN FX` and `POST FX`; both walks in this file had to gain an
	// entry and only the `expandedWidth()` one did. **A known-bad lengthening `kGrainLabel` to
	// `"GRAIN FX LANE DIVERSION"` — 322px at `kCollapsedLabelSize` against a 160px run — came back
	// GREEN**, which is exactly what CLAUDE.md says a green mutation is for: it checks the CASE, not
	// the code, and a case that cannot express its subject passes for the same reason a correct one
	// does. Nothing in this case's text looked wrong; it was measuring six of the seven labels.
	CHECK(rails.size() >= 9u);

	float widest = 0.0f;
	std::string widestLabel;
	for (ui::Rail* rail : rails) {
		REQUIRE(rail != nullptr);
		const std::string& label = rail->labelForTest();
		CAPTURE(label);
		CHECK_FALSE(label.empty()); // a closed rail says exactly one word, so it must have one
		const float run =
		    ui::font(ui::Rail::kCollapsedLabelSize).stringWidth(std::u32string(label.begin(), label.end()));
		if (run > widest) {
			widest = run;
			widestLabel = label;
		}
	}
	CAPTURE(widestLabel);
	CAPTURE(widest);
	// NOT VACUOUS: the font answered with a real number rather than a zero everything clears.
	CHECK(widest > static_cast<float>(ui::Rail::kCollapsedLabelSize));
	// **AND THE AIR, NOT JUST THE FIT.** `>=` alone passes where the longest word ends exactly at the
	// bottom of the strip — legal, and wrong to look at. The floor is a quarter of the label size, so a
	// font change costing a pixel or two is a warning in the capture and not a red here.
	CHECK(ui::Rail::kCollapsedLabelRun - widest >= ui::Rail::kCollapsedLabelSize * 0.25f);

	// **AND THE RUN REALLY IS WHAT THE STRIP LEAVES FOR IT.** `drawRailChrome` starts the label at
	// `toggle.bottom() + kPadTop`, with the toggle at `kPadTop/2` — so the room below the chrome is the
	// height floor less exactly that, and this is the line that says the constant is added to the right
	// things rather than merely being large. **RESTATED FROM `drawRailChrome`'s OWN ARITHMETIC**, not
	// from `kCollapsedChromeHeight`, which is the constant under test: the first draft of that constant
	// was `kPadTop + kToggleSize + kPadTop`, 6px larger than the chrome it claimed to be, and a check
	// that read it back would have agreed with the mistake.
	CHECK(ui::Rail::kCollapsedMinimumHeight
	          - (ui::Rail::kPadTop * 0.5f + ui::Rail::kToggleSize + ui::Rail::kPadTop)
	      >= widest);
}

