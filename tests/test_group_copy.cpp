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

#include "backplane/block_context.h"
#include "composite/composite_types.h"
#include "fx/delay/delay_types.h"
#include "fx/filter/filter_types.h"
#include "hostslots/slot_pool.h"
#include "mod/lfo/lfo_types.h"
#include "mod/macros/macros_types.h"
#include "plugin/chain_preset.h"
#include "plugin/device_preset.h"
#include "plugin/device_registry.h"
#include "plugin/editor_lanes.h"
#include "plugin/gui_bridge.h"
#include "plugin/synth.h"
#include "plugin/ui/card_selection.h"
#include "plugin/ui/edit_history.h"
#include "plugin/ui/fragment_verbs.h"
#include "rack/fragment.h"
#include "rack/inner_chain.h"
#include "rack/parallel_rack.h"
#include "rack/rack_axes.h"
#include "rack/rack_bridge.h"
#include "rack/rack_device.h"
#include "rack/rack_engine.h"
#include "rack/rack_state.h"
#include "rack/site_chains.h"

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// BACKLOG OA — multi-select, copy/paste/duplicate, remove-many, Group into Chain and Group into Rack, by VALUE:
// every claim below reads the record, a route's amount, a label, a host slot's path — never a rendered buffer.
// Design decisions D1-D6 (2026-10-19).

using namespace feathers;
using backplane::Destination;
using backplane::Source;
using rack::RackEngine;
using Status = device::HostSlotStatus::State;

namespace {

size_t keyIndex(const device::Descriptor& d, std::string_view key) {
	for (size_t i = 0; i < d.params.size(); ++i) {
		if (d.params[i].key != nullptr && key == d.params[i].key) {
			return i;
		}
	}
	FAIL("no control keyed " << key);
	return 0;
}

const size_t kCutoff = keyIndex(fx::filter::kDescriptor, "filter_cutoff_wide");
const size_t kRes = keyIndex(fx::filter::kDescriptor, "filter_res");
const size_t kDelayMix = keyIndex(fx::delay::kDescriptor, "delay_mix");

/// THE STANDALONE CHAIN PLUGIN'S SHAPE: a top bridge owning its host slot pool, an engine a block runs on.
struct Rig {
	RackEngine engine { rack::kRackAxes, hostCatalogue() };
	std::unique_ptr<rack::RackBridge> top;
	ui::EditHistory history;
	std::unique_ptr<ui::FragmentVerbs> verbs;
	std::vector<float> l = std::vector<float>(64, 0.0f);
	std::vector<float> r = std::vector<float>(64, 0.0f);

	Rig() {
		engine.prepare(48000.0, 4096);
		top = std::make_unique<rack::RackBridge>(engine, nullptr, rack::RackBridge::HostSlots::OwnPool);
		verbs = std::make_unique<ui::FragmentVerbs>(*top, history, nullptr, [this] { return order(); });
		rack::clearPasteboard();
	}
	DeviceInstanceId add(const char* tag) {
		const DeviceInstanceId id = top->addDevice(tag);
		REQUIRE(id != kNoInstance);
		block();
		return id;
	}
	std::vector<DeviceInstanceId> order() const {
		std::vector<DeviceInstanceId> out;
		for (size_t p = 0; p < top->chainLength(); ++p) {
			out.push_back(top->deviceAtPosition(p));
		}
		return out;
	}
	rack::RackBridge& inner(DeviceInstanceId chain) {
		rack::RackBridge* b = rack::bridgeOf(top->containerSurfaceOf(chain));
		REQUIRE(b != nullptr);
		return *b;
	}
	void block() {
		top->drain();
		std::fill(l.begin(), l.end(), 0.1f);
		std::fill(r.begin(), r.end(), 0.1f);
		engine.processStereo(l.data(), r.data(), 64, backplane::unknownBlock());
		top->publish(engine);
		top->reapRetired();
	}
	void settle() {
		for (int i = 0; i < 3; ++i) {
			(void)top->serviceHostSlots();
			block();
		}
	}
	const hostslots::SlotPool& pool() { return top->hostSlots()->pool(); }
};

Source lfoOut(DeviceInstanceId lfo) { return Source::output(lfo, 0); }

} // namespace

//------------------------------------------------------------------------------------------------//
// STAGE 1 — THE SELECTION                                                                          //
//------------------------------------------------------------------------------------------------//

TEST_CASE("OA D1: a plain press selects one card, Shift selects the run from the anchor in chain order, Ctrl toggles") {
	ui::CardSelection s;
	const std::vector<DeviceInstanceId> order { 11, 12, 13, 14, 15 };
	s.press(12, ui::CardSelection::Press::Plain, order);
	CHECK(s.ids() == std::vector<DeviceInstanceId> { 12 });
	CHECK(s.anchor() == 12u);
	// SHIFT: the run from the anchor to the card, whichever side it is on, and the anchor stays.
	s.press(14, ui::CardSelection::Press::Range, order);
	CHECK(s.inOrder(order) == std::vector<DeviceInstanceId> { 12, 13, 14 });
	CHECK(s.anchor() == 12u);
	s.press(11, ui::CardSelection::Press::Range, order);
	CHECK(s.inOrder(order) == std::vector<DeviceInstanceId> { 11, 12 });
	// CTRL: one card in or out; an added card becomes the anchor.
	s.press(15, ui::CardSelection::Press::Toggle, order);
	CHECK(s.inOrder(order) == std::vector<DeviceInstanceId> { 11, 12, 15 });
	CHECK(s.anchor() == 15u);
	s.press(12, ui::CardSelection::Press::Toggle, order);
	CHECK(s.inOrder(order) == std::vector<DeviceInstanceId> { 11, 15 });
	// A KNOB ON A SELECTED CARD KEEPS THE SELECTION; on an unselected one, that card alone.
	s.touch(15);
	CHECK(s.inOrder(order) == std::vector<DeviceInstanceId> { 11, 15 });
	s.touch(13);
	CHECK(s.ids() == std::vector<DeviceInstanceId> { 13 });
	// A LASSO replaces, or with Shift adds; a device the chain no longer holds is pruned.
	s.lasso(std::vector<DeviceInstanceId> { 14, 15 }, true);
	CHECK(s.inOrder(order) == std::vector<DeviceInstanceId> { 13, 14, 15 });
	CHECK(s.prune(std::vector<DeviceInstanceId> { 11, 13, 15 }));
	CHECK(s.inOrder(order) == std::vector<DeviceInstanceId> { 13, 15 });
}

//------------------------------------------------------------------------------------------------//
// STAGE 2 — COPY, PASTE, DUPLICATE, REMOVE                                                         //
//------------------------------------------------------------------------------------------------//

TEST_CASE("OA: Duplicate copies state, labels and the routes AMONG the copies re-pointed; keeps inbound in the same record; leaves outbound behind; one undo step") {
	Rig rig;
	const DeviceInstanceId lfo = rig.add(mod::lfo::kTraits.tag);
	const DeviceInstanceId filter = rig.add(fx::filter::kTraits.tag);
	const DeviceInstanceId delay = rig.add(fx::delay::kTraits.tag);
	rig.top->setField(filter, kRes, 0.73f);
	rig.top->setOutputLabel(lfo, 0, "Wobble");
	rig.top->setRouteAmount(lfoOut(lfo), Destination::control(filter, kCutoff), 0.5f);      // among
	rig.top->setRouteAmount(Source::core(0), Destination::control(filter, kCutoff), 0.25f); // inbound: a macro
	rig.top->setRouteAmount(lfoOut(lfo), Destination::control(delay, kDelayMix), 0.4f);    // outbound
	rig.block();
	(void)rig.top->takeStateDirty();
	const size_t steps = rig.history.length();

	const std::vector<DeviceInstanceId> made = rig.verbs->duplicate(std::vector<DeviceInstanceId> { lfo, filter });
	REQUIRE(made.size() == 2);
	const DeviceInstanceId lfo2 = made[0];
	const DeviceInstanceId filter2 = made[1];
	CHECK(lfo2 != lfo);
	CHECK(filter2 != filter);
	// RIGHT AFTER THE LAST SELECTED DEVICE, in order: LFO, Filter, LFO', Filter', Delay.
	CHECK(rig.order() == std::vector<DeviceInstanceId> { lfo, filter, lfo2, filter2, delay });
	CHECK(rig.top->fieldAt(filter2, kRes) == doctest::Approx(0.73f));
	CHECK(rig.top->outputLabel(lfo2, 0) == "Wobble");
	CHECK(rig.top->routeAmount(lfoOut(lfo2), Destination::control(filter2, kCutoff)) == doctest::Approx(0.5f));
	CHECK(rig.top->routeAmount(lfoOut(lfo), Destination::control(filter2, kCutoff)) == 0.0f);
	CHECK(rig.top->routeAmount(Source::core(0), Destination::control(filter2, kCutoff)) == doctest::Approx(0.25f));
	CHECK(rig.top->routeAmount(lfoOut(lfo2), Destination::control(delay, kDelayMix)) == 0.0f);
	// THE ORIGINALS UNTOUCHED.
	CHECK(rig.top->routeAmount(lfoOut(lfo), Destination::control(filter, kCutoff)) == doctest::Approx(0.5f));
	CHECK(rig.top->routeAmount(lfoOut(lfo), Destination::control(delay, kDelayMix)) == doctest::Approx(0.4f));
	CHECK(rig.top->takeStateDirty());
	CHECK(rig.history.length() == steps + 1);

	rig.history.undo();
	CHECK(rig.order() == std::vector<DeviceInstanceId> { lfo, filter, delay });
	rig.history.redo();
	CHECK(rig.order() == std::vector<DeviceInstanceId> { lfo, filter, lfo2, filter2, delay });
	CHECK(rig.top->routeAmount(lfoOut(lfo2), Destination::control(filter2, kCutoff)) == doctest::Approx(0.5f));
	CHECK(rig.top->outputLabel(lfo2, 0) == "Wobble");
}

TEST_CASE("OA: Copy then Paste into ANOTHER chain carries the routes among the copies and drops the inbound ones, and says so") {
	Rig rig;
	const DeviceInstanceId lfo = rig.add(mod::lfo::kTraits.tag);
	const DeviceInstanceId filter = rig.add(fx::filter::kTraits.tag);
	const DeviceInstanceId chain = rig.add(composite::kTraits.tag);
	rig.top->setRouteAmount(lfoOut(lfo), Destination::control(filter, kCutoff), 0.5f);
	rig.top->setRouteAmount(Source::core(1), Destination::control(filter, kCutoff), 0.25f);
	rig.block();
	rig.verbs->copy(std::vector<DeviceInstanceId> { lfo, filter });
	REQUIRE(ui::FragmentVerbs::canPaste());

	rack::RackBridge& inner = rig.inner(chain);
	ui::EditHistory innerHistory;
	ui::FragmentVerbs innerVerbs(inner, innerHistory, nullptr, [&inner] {
		std::vector<DeviceInstanceId> out;
		for (size_t p = 0; p < inner.chainLength(); ++p) {
			out.push_back(inner.deviceAtPosition(p));
		}
		return out;
	});
	const std::vector<DeviceInstanceId> pasted = innerVerbs.paste(kNoInstance);
	REQUIRE(pasted.size() == 2);
	CHECK(inner.routeAmount(lfoOut(pasted[0]), Destination::control(pasted[1], kCutoff)) == doctest::Approx(0.5f));
	CHECK(inner.routeAmount(Source::core(1), Destination::control(pasted[1], kCutoff)) == 0.0f);
	CHECK(innerVerbs.lastSaidForTest().find("1 route") != std::string::npos);
	// AND A PASTE BACK INTO THE RECORD IT CAME FROM keeps the macro's route.
	const std::vector<DeviceInstanceId> back = rig.verbs->paste(kNoInstance);
	REQUIRE(back.size() == 2);
	CHECK(rig.top->routeAmount(Source::core(1), Destination::control(back[1], kCutoff)) == doctest::Approx(0.25f));
}

TEST_CASE("OA: the clip is text that reads back as the same devices and routes") {
	Rig rig;
	const DeviceInstanceId lfo = rig.add(mod::lfo::kTraits.tag);
	const DeviceInstanceId filter = rig.add(fx::filter::kTraits.tag);
	rig.top->setRouteAmount(lfoOut(lfo), Destination::control(filter, kCutoff), 0.5f);
	rig.top->setField(filter, kRes, 0.61f);
	const rack::Fragment f = rack::liftFragment(*rig.top, std::vector<DeviceInstanceId> { filter, lfo });
	REQUIRE(f.size() == 2);
	CHECK(f.patch.device[0].instanceId == lfo); // chain order, whatever order the hand picked
	const std::string text = rack::clipText(f);
	CHECK(text.rfind("{\"feathersClip\":1,\"patch\":", 0) == 0);
	const std::optional<rack::Fragment> back = rack::fragmentOfClipText(text);
	REQUIRE(back.has_value());
	CHECK(back->patch.device == f.patch.device);
	CHECK(back->patch.route == f.patch.route);
	CHECK(back->origin == 0u); // from nowhere
	CHECK_FALSE(rack::fragmentOfClipText("{\"something\":1}").has_value());
}

namespace {
/// A STAND-IN FOR THE SYSTEM CLIPBOARD (headless cases cannot reach X11): the seam `rack::setOsClipboard` takes.
struct FakeOsClipboard {
	std::string text;
	int reads = 0;
	FakeOsClipboard() {
		rack::setOsClipboard({ [this] {
			                      ++reads;
			                      return text;
		                      },
		                       [this](const std::string& t) { text = t; } });
	}
	~FakeOsClipboard() { rack::clearOsClipboard(); }
};
} // namespace

TEST_CASE("OA clipboard: a copy puts the clip on the OS clipboard as text with the clip header, and a paste with an "
          "EMPTY in-process board reads it back as the same devices and routes") {
	Rig rig;
	FakeOsClipboard os;
	const DeviceInstanceId lfo = rig.add(mod::lfo::kTraits.tag);
	const DeviceInstanceId filter = rig.add(fx::filter::kTraits.tag);
	rig.top->setRouteAmount(lfoOut(lfo), Destination::control(filter, kCutoff), 0.5f);
	rig.top->setField(filter, kRes, 0.61f);
	const std::vector<DeviceInstanceId> both { lfo, filter };
	rig.verbs->copy(both);
	REQUIRE(rack::isClipText(os.text));
	CHECK(os.text.rfind("{\"feathersClip\":1,\"patch\":", 0) == 0);
	const rack::Fragment lifted = rack::liftFragment(*rig.top, both);
	CHECK(os.text == rack::clipText(lifted));

	// ANOTHER PROCESS'S PASTE: no in-process board, only the system's text.
	rack::clearPasteboard();
	CHECK(rack::pasteboardHolds());
	const std::optional<rack::Fragment> back = rack::pasteboard();
	REQUIRE(back.has_value());
	CHECK(back->patch.device == lifted.patch.device);
	CHECK(back->patch.route == lifted.patch.route);
	CHECK(back->origin == 0u);
	const std::vector<DeviceInstanceId> pasted = rig.verbs->paste(filter);
	CHECK(pasted.size() == 2);
	CHECK(rig.top->chainLength() == 4);
}

TEST_CASE("OA clipboard: foreign text on the OS clipboard is refused cleanly; our own copy coming back keeps its origin; "
          "a different clip of ours, newer than the board, wins") {
	Rig rig;
	FakeOsClipboard os;
	const DeviceInstanceId lfo = rig.add(mod::lfo::kTraits.tag);
	const DeviceInstanceId filter = rig.add(fx::filter::kTraits.tag);

	// FOREIGN TEXT, with nothing in process: no paste, nothing placed, no throw.
	for (const char* foreign : { "hello", "{\"something\":1}", "{\"feathersClip\":1,\"patch\":", "{\"feathersClip\":1,\"patch\":{nonsense}}", "" }) {
		CAPTURE(foreign);
		os.text = foreign;
		rack::clearPasteboard();
		CHECK_FALSE(rack::pasteboard().has_value());
		CHECK(rig.verbs->paste(filter).empty());
		CHECK(rig.top->chainLength() == 2);
	}
	os.text = "hello";
	CHECK_FALSE(rack::pasteboardHolds());

	// OUR OWN COPY COMES BACK: the board answers and keeps the origin it was lifted from.
	rig.verbs->copy(std::vector<DeviceInstanceId> { filter });
	const std::optional<rack::Fragment> own = rack::pasteboard();
	REQUIRE(own.has_value());
	CHECK(own->origin != 0u);

	// FOREIGN TEXT NEWER THAN THE BOARD does not displace it (it is no clip).
	const std::string ours = os.text;
	os.text = "hello";
	const std::optional<rack::Fragment> still = rack::pasteboard();
	REQUIRE(still.has_value());
	CHECK(still->origin == own->origin);

	// A DIFFERENT CLIP OF OURS, NEWER: another instance copied the LFO after we copied the filter.
	os.text = rack::clipText(rack::liftFragment(*rig.top, std::vector<DeviceInstanceId> { lfo }));
	REQUIRE(os.text != ours);
	const std::optional<rack::Fragment> newer = rack::pasteboard();
	REQUIRE(newer.has_value());
	REQUIRE(newer->size() == 1);
	CHECK(newer->patch.device[0].instanceId == lfo);
	CHECK(newer->origin == 0u);
}

TEST_CASE("OA: Remove of several is ONE undo step, and the undo puts each device back under its old id with its routes, labels, fold and host slot") {
	Rig rig;
	const DeviceInstanceId lfo = rig.add(mod::lfo::kTraits.tag);
	const DeviceInstanceId filter = rig.add(fx::filter::kTraits.tag);
	const DeviceInstanceId delay = rig.add(fx::delay::kTraits.tag);
	rig.top->setRouteAmount(lfoOut(lfo), Destination::control(filter, kCutoff), 0.5f);
	rig.top->setRouteAmount(lfoOut(lfo), Destination::control(delay, kDelayMix), 0.4f);
	rig.top->setRouteAmount(Source::core(0), Destination::control(filter, kCutoff), 0.25f);
	rig.top->setOutputLabel(lfo, 0, "Wobble");
	rig.top->setDeviceFolded(filter, true);
	REQUIRE(rig.top->exposeToHost(filter, kCutoff, nullptr));
	rig.settle();
	const device::HostSlotStatus pinned = rig.top->hostSlotOf(filter, kCutoff);
	REQUIRE(pinned.state == Status::Pinned);
	const size_t steps = rig.history.length();

	CHECK(rig.verbs->remove(std::vector<DeviceInstanceId> { lfo, filter }) == 2);
	CHECK(rig.history.length() == steps + 1);
	CHECK(rig.order() == std::vector<DeviceInstanceId> { delay });
	CHECK(rig.pool().state(pinned.slot) == hostslots::SlotState::Empty);
	CHECK(rig.top->outputLabel(lfo, 0) != "Wobble");

	rig.history.undo();
	rig.settle();
	CHECK(rig.order() == std::vector<DeviceInstanceId> { lfo, filter, delay });
	CHECK(rig.top->routeAmount(lfoOut(lfo), Destination::control(filter, kCutoff)) == doctest::Approx(0.5f));
	CHECK(rig.top->routeAmount(lfoOut(lfo), Destination::control(delay, kDelayMix)) == doctest::Approx(0.4f));
	CHECK(rig.top->routeAmount(Source::core(0), Destination::control(filter, kCutoff)) == doctest::Approx(0.25f));
	CHECK(rig.top->outputLabel(lfo, 0) == "Wobble");
	CHECK(rig.top->deviceFolded(filter));
	const device::HostSlotStatus back = rig.top->hostSlotOf(filter, kCutoff);
	CHECK(back.state == Status::Pinned);
	CHECK(back.slot == pinned.slot);
}

TEST_CASE("OA: a copied Chain's inner route from the outer record follows a copied source, stays on the original in the same record, and is dropped elsewhere") {
	Rig rig;
	const DeviceInstanceId lfo = rig.add(mod::lfo::kTraits.tag);
	const DeviceInstanceId chain = rig.add(composite::kTraits.tag);
	rack::RackBridge& inner = rig.inner(chain);
	const DeviceInstanceId filter = inner.addDevice(fx::filter::kTraits.tag);
	REQUIRE(filter != kNoInstance);
	rig.block();
	// §7: the outer LFO reaches the filter inside the chain — an `Outer` route in the chain's own record.
	REQUIRE(rig.top->setInnerRouteAmount(lfoOut(lfo), chain, Destination::control(filter, kCutoff), 0.5f));
	rig.block();
	REQUIRE(inner.routeAmount(lfoOut(lfo).asOuter(), Destination::control(filter, kCutoff)) == doctest::Approx(0.5f));

	SUBCASE("the LFO and the chain duplicated together: the copy's inner route names the copied LFO") {
		const std::vector<DeviceInstanceId> made = rig.verbs->duplicate(std::vector<DeviceInstanceId> { lfo, chain });
		REQUIRE(made.size() == 2);
		rig.block();
		rack::RackBridge& copy = rig.inner(made[1]);
		REQUIRE(copy.chainLength() == 1);
		const DeviceInstanceId innerFilter = copy.deviceAtPosition(0);
		CHECK(copy.routeAmount(lfoOut(made[0]).asOuter(), Destination::control(innerFilter, kCutoff)) == doctest::Approx(0.5f));
		CHECK(copy.routeAmount(lfoOut(lfo).asOuter(), Destination::control(innerFilter, kCutoff)) == 0.0f);
	}
	SUBCASE("the chain alone, in the same record: the copy's inner route still names the original LFO") {
		const std::vector<DeviceInstanceId> made = rig.verbs->duplicate(std::vector<DeviceInstanceId> { chain });
		REQUIRE(made.size() == 1);
		rig.block();
		rack::RackBridge& copy = rig.inner(made[0]);
		CHECK(copy.routeAmount(lfoOut(lfo).asOuter(), Destination::control(copy.deviceAtPosition(0), kCutoff))
		      == doctest::Approx(0.5f));
	}
	SUBCASE("the chain alone, pasted into another record: its inner route is dropped and counted, never re-pointed") {
		const rack::Fragment f = rack::liftFragment(*rig.top, std::vector<DeviceInstanceId> { chain });
		RackEngine otherEngine { rack::kRackAxes, hostCatalogue() };
		otherEngine.prepare(48000.0, 4096);
		rack::RackBridge other { otherEngine };
		// AN LFO WEARING THE SAME ID over there, so a reference left as it was would silently name it.
		while (true) {
			const DeviceInstanceId id = other.addDevice(mod::lfo::kTraits.tag);
			REQUIRE(id != kNoInstance);
			if (id >= lfo) {
				break;
			}
		}
		const rack::PlaceResult placed = rack::placeFragment(other, f, other.chainLength(), rack::PlaceIds::Fresh, false);
		REQUIRE(placed.placed.size() == 1);
		CHECK(placed.innerRoutesDropped == 1);
		other.drain();
		otherEngine.processStereo(rig.l.data(), rig.r.data(), 64, backplane::unknownBlock());
		other.publish(otherEngine);
		rack::RackBridge* copy = rack::bridgeOf(other.containerSurfaceOf(placed.placed[0]));
		REQUIRE(copy != nullptr);
		size_t routes = 0;
		copy->forEachRoute([&](const backplane::Route&) { ++routes; });
		CHECK(routes == 0);
	}
}

TEST_CASE("OA: Duplicate of a device with a site carries the site, and its PRE export routes — into the device and from POST — follow the copy") {
	Rig rig;
	const DeviceInstanceId filter = rig.add(fx::filter::kTraits.tag);
	REQUIRE(rig.top->openSite(filter));
	rig.block();
	auto* site = static_cast<rack::SiteChains*>(rig.top->containerSurfaceOf(siteKeyOf(filter)));
	REQUIRE(site != nullptr);
	const DeviceInstanceId lfo = site->pre().bridge()->addDevice(mod::lfo::kTraits.tag);
	const DeviceInstanceId post = site->post().bridge()->addDevice(fx::filter::kTraits.tag);
	REQUIRE(lfo != kNoInstance);
	REQUIRE(post != kNoInstance);
	rig.block();
	const Source out = rig.top->siteExport(filter, lfoOut(lfo));
	REQUIRE(out.isExport());
	rig.top->setRouteAmount(out, Destination::control(filter, kCutoff), 0.5f);
	site->post().bridge()->setRouteAmount(out.asOuter(), Destination::control(post, kCutoff), 0.25f);
	rig.block();
	REQUIRE(rig.top->routeAmount(out, Destination::control(filter, kCutoff)) == doctest::Approx(0.5f));

	const std::vector<DeviceInstanceId> made = rig.verbs->duplicate(std::vector<DeviceInstanceId> { filter });
	REQUIRE(made.size() == 1);
	rig.block();
	rig.block();
	const DeviceInstanceId copy = made[0];
	CHECK(rig.top->hasSite(copy));
	const Source copied = Source::exported(copy, out.exportRow());
	CHECK(rig.top->routeAmount(copied, Destination::control(copy, kCutoff)) == doctest::Approx(0.5f));
	CHECK(rig.top->routeAmount(out, Destination::control(copy, kCutoff)) == 0.0f);
	auto* copySite = static_cast<rack::SiteChains*>(rig.top->containerSurfaceOf(siteKeyOf(copy)));
	REQUIRE(copySite != nullptr);
	rack::RackBridge* copyPost = copySite->post().bridge();
	REQUIRE(copyPost != nullptr);
	REQUIRE(copyPost->chainLength() == 1);
	const DeviceInstanceId postCopy = copyPost->deviceAtPosition(0);
	CHECK(copyPost->routeAmount(copied.asOuter(), Destination::control(postCopy, kCutoff)) == doctest::Approx(0.25f));
	CHECK(copyPost->routeAmount(out.asOuter(), Destination::control(postCopy, kCutoff)) == 0.0f);
}

TEST_CASE("OA: a block move lands the selection contiguous at the gap, in order, and its undo gives back the order and the routes the move deleted") {
	Rig rig;
	const DeviceInstanceId a = rig.add(fx::filter::kTraits.tag);
	const DeviceInstanceId b = rig.add(fx::delay::kTraits.tag);
	const DeviceInstanceId c = rig.add(fx::filter::kTraits.tag);
	const DeviceInstanceId lfo = rig.add(mod::lfo::kTraits.tag);
	const DeviceInstanceId d = rig.add(fx::delay::kTraits.tag);
	rig.top->setRouteAmount(lfoOut(lfo), Destination::control(c, kCutoff), 0.3f);
	CHECK(rig.verbs->moveBlock(std::vector<DeviceInstanceId> { a, c }, 5));
	CHECK(rig.order() == std::vector<DeviceInstanceId> { b, lfo, d, a, c });
	rig.history.undo();
	CHECK(rig.order() == std::vector<DeviceInstanceId> { a, b, c, lfo, d });
	CHECK(rig.top->routeAmount(lfoOut(lfo), Destination::control(c, kCutoff)) == doctest::Approx(0.3f));
	rig.history.redo();
	CHECK(rig.order() == std::vector<DeviceInstanceId> { b, lfo, d, a, c });
}

//------------------------------------------------------------------------------------------------//
// STAGE 3 — GROUP INTO CHAIN                                                                       //
//------------------------------------------------------------------------------------------------//

TEST_CASE("OA D4: a selection split by an audio device cannot be grouped, and says by which; one split by a modulator can") {
	Rig rig;
	const DeviceInstanceId a = rig.add(fx::filter::kTraits.tag);
	const DeviceInstanceId lfo = rig.add(mod::lfo::kTraits.tag);
	const DeviceInstanceId delay = rig.add(fx::delay::kTraits.tag);
	const DeviceInstanceId c = rig.add(fx::filter::kTraits.tag);
	CHECK(rig.verbs->groupRefusal(std::vector<DeviceInstanceId> { a, c }).find("Delay") != std::string::npos);
	CHECK(rig.verbs->group(std::vector<DeviceInstanceId> { a, c }, rack::GroupKind::Chain).empty());
	CHECK(rig.order() == std::vector<DeviceInstanceId> { a, lfo, delay, c });
	CHECK(rig.verbs->groupRefusal(std::vector<DeviceInstanceId> { a, delay }).empty());
	CHECK(rig.verbs->groupRefusal(std::vector<DeviceInstanceId> {}).size() > 0);
}

TEST_CASE("OA: Group into Chain puts the selection inside one Chain in place, in order, routes among kept, an upstream LFO's route kept through the outer gate, an outbound one kept through the group's export; one undo restores everything") {
	Rig rig;
	const DeviceInstanceId lfo = rig.add(mod::lfo::kTraits.tag);
	const DeviceInstanceId lfo2 = rig.add(mod::lfo::kTraits.tag);
	const DeviceInstanceId filter = rig.add(fx::filter::kTraits.tag);
	const DeviceInstanceId delay = rig.add(fx::delay::kTraits.tag);
	const DeviceInstanceId tail = rig.add(fx::filter::kTraits.tag);
	rig.top->setField(filter, kRes, 0.66f);
	rig.top->setOutputLabel(lfo2, 0, "Inner");
	rig.top->setRouteAmount(lfoOut(lfo2), Destination::control(filter, kCutoff), 0.5f);   // among
	rig.top->setRouteAmount(lfoOut(lfo), Destination::control(delay, kDelayMix), 0.3f);   // inbound, upstream
	rig.top->setRouteAmount(Source::core(2), Destination::control(filter, kRes), 0.2f);   // inbound, a macro
	rig.top->setRouteAmount(lfoOut(lfo2), Destination::control(tail, kCutoff), 0.7f);     // outbound
	REQUIRE(rig.top->exposeToHost(filter, kCutoff, nullptr));
	rig.settle();
	const hostslots::SlotIndex slot = rig.top->hostSlotOf(filter, kCutoff).slot;
	(void)rig.top->takeStateDirty();
	const size_t steps = rig.history.length();

	const std::vector<DeviceInstanceId> made =
	    rig.verbs->group(std::vector<DeviceInstanceId> { lfo2, filter, delay }, rack::GroupKind::Chain);
	REQUIRE(made.size() == 1);
	const DeviceInstanceId chain = made[0];
	rig.settle();
	CHECK(rig.history.length() == steps + 1);
	CHECK(rig.top->takeStateDirty());
	// IN PLACE: where the first moved device stood.
	CHECK(rig.order() == std::vector<DeviceInstanceId> { lfo, chain, tail });
	CHECK(device::sameTag(rig.top->descriptorOf(chain)->traits.tag, composite::kTraits.tag));
	rack::RackBridge& inner = rig.inner(chain);
	REQUIRE(inner.chainLength() == 3);
	CHECK(inner.deviceAtPosition(0) == lfo2);
	CHECK(inner.deviceAtPosition(1) == filter);
	CHECK(inner.deviceAtPosition(2) == delay);
	CHECK(inner.fieldAt(filter, kRes) == doctest::Approx(0.66f));
	CHECK(inner.outputLabel(lfo2, 0) == "Inner");
	CHECK(inner.routeAmount(lfoOut(lfo2), Destination::control(filter, kCutoff)) == doctest::Approx(0.5f));
	CHECK(inner.routeAmount(lfoOut(lfo).asOuter(), Destination::control(delay, kDelayMix)) == doctest::Approx(0.3f));
	CHECK(inner.routeAmount(Source::outerCore(2), Destination::control(filter, kRes)) == doctest::Approx(0.2f));
	// THE ROUTE OUT OF THE GROUP (backlog NZ (b)): the group exports its LFO, and the device left outside reads the
	// export — the same row a hand would mint, at the same amount; nothing reported lost.
	CHECK(rig.top->routeAmount(lfoOut(lfo2), Destination::control(tail, kCutoff)) == 0.0f);
	const Source exported = rig.top->containerExport(chain, lfoOut(lfo2));
	REQUIRE(exported.isExport());
	CHECK(exported.device == chain);
	CHECK(rig.top->routeAmount(exported, Destination::control(tail, kCutoff)) == doctest::Approx(0.7f));
	CHECK(rig.verbs->lastSaidForTest().find("could not cross") == std::string::npos);
	// THE HOST SLOT WENT DOWN WITH ITS DEVICE: the same slot, the path through the chain.
	CHECK(rig.pool().target(slot).path == std::vector<DeviceInstanceId> { chain, filter });
	CHECK(rig.pool().state(slot) == hostslots::SlotState::Pinned);

	rig.history.undo();
	rig.settle();
	CHECK(rig.order() == std::vector<DeviceInstanceId> { lfo, lfo2, filter, delay, tail });
	CHECK(rig.top->fieldAt(filter, kRes) == doctest::Approx(0.66f));
	CHECK(rig.top->outputLabel(lfo2, 0) == "Inner");
	CHECK(rig.top->routeAmount(lfoOut(lfo2), Destination::control(filter, kCutoff)) == doctest::Approx(0.5f));
	CHECK(rig.top->routeAmount(lfoOut(lfo), Destination::control(delay, kDelayMix)) == doctest::Approx(0.3f));
	CHECK(rig.top->routeAmount(Source::core(2), Destination::control(filter, kRes)) == doctest::Approx(0.2f));
	CHECK(rig.top->routeAmount(lfoOut(lfo2), Destination::control(tail, kCutoff)) == doctest::Approx(0.7f));
	CHECK(rig.top->routeAmount(exported, Destination::control(tail, kCutoff)) == 0.0f); // went with the group
	CHECK(rig.pool().target(slot).path == std::vector<DeviceInstanceId> { filter });
	CHECK(rig.top->hostSlotOf(filter, kCutoff).state == Status::Pinned);

	rig.history.redo();
	rig.settle();
	CHECK(rig.order() == std::vector<DeviceInstanceId> { lfo, chain, tail });
	CHECK(rig.inner(chain).chainLength() == 3);
	CHECK(rig.top->routeAmount(exported, Destination::control(tail, kCutoff)) == doctest::Approx(0.7f));
}

TEST_CASE("OA: grouping ONE device keeps a host macro's route into it, now read through the group's outer gate") {
	Rig rig;
	const DeviceInstanceId filter = rig.add(fx::filter::kTraits.tag);
	rig.top->setRouteAmount(Source::core(0), Destination::control(filter, kCutoff), 0.4f);
	const std::vector<DeviceInstanceId> made = rig.verbs->group(std::vector<DeviceInstanceId> { filter }, rack::GroupKind::Chain);
	REQUIRE(made.size() == 1);
	rig.settle();
	CHECK(rig.inner(made[0]).routeAmount(Source::outerCore(0), Destination::control(filter, kCutoff)) == doctest::Approx(0.4f));
}

//------------------------------------------------------------------------------------------------//
// STAGE 4 — GROUP INTO RACK                                                                        //
//------------------------------------------------------------------------------------------------//

TEST_CASE("OA D2: Group into Rack makes a Rack whose ONE chain holds the selection in order, at unity, with the routes among kept; undo restores") {
	Rig rig;
	const DeviceInstanceId lfo = rig.add(mod::lfo::kTraits.tag);
	const DeviceInstanceId filter = rig.add(fx::filter::kTraits.tag);
	const DeviceInstanceId delay = rig.add(fx::delay::kTraits.tag);
	rig.top->setRouteAmount(lfoOut(lfo), Destination::control(filter, kCutoff), 0.5f);
	const std::vector<DeviceInstanceId> made =
	    rig.verbs->group(std::vector<DeviceInstanceId> { lfo, filter, delay }, rack::GroupKind::Rack);
	REQUIRE(made.size() == 1);
	rig.settle();
	CHECK(rig.order() == made);
	CHECK(device::sameTag(rig.top->descriptorOf(made[0])->traits.tag, rack::parallel::kDescriptor.traits.tag));
	rack::parallel::Rack* r = rack::parallel::rackOf(rig.top->containerSurfaceOf(made[0]));
	REQUIRE(r != nullptr);
	REQUIRE(r->chainCount() == 1);
	REQUIRE(r->chainAt(0) != nullptr);
	rack::RackBridge* chain = r->chainAt(0)->bridge();
	REQUIRE(chain != nullptr);
	REQUIRE(chain->chainLength() == 3);
	CHECK(chain->deviceAtPosition(0) == lfo);
	CHECK(chain->deviceAtPosition(1) == filter);
	CHECK(chain->deviceAtPosition(2) == delay);
	CHECK(chain->routeAmount(lfoOut(lfo), Destination::control(filter, kCutoff)) == doctest::Approx(0.5f));
	CHECK(r->level(0) == doctest::Approx(static_cast<float>(composite::kFaderUnity)));
	rig.history.undo();
	rig.settle();
	CHECK(rig.order() == std::vector<DeviceInstanceId> { lfo, filter, delay });
	CHECK(rig.top->routeAmount(lfoOut(lfo), Destination::control(filter, kCutoff)) == doctest::Approx(0.5f));
}

TEST_CASE("OA + NZ rung 5: grouping a device a PRE export reaches — into a Chain, or into a Rack — keeps the route, now "
          "read through the group's outer gate, by value; one undo restores") {
	for (const rack::GroupKind kind : { rack::GroupKind::Chain, rack::GroupKind::Rack }) {
		CAPTURE(static_cast<int>(kind));
		Rig rig;
		const DeviceInstanceId holder = rig.add(fx::filter::kTraits.tag);
		const DeviceInstanceId after = rig.add(fx::filter::kTraits.tag);
		REQUIRE(rig.top->openSite(holder));
		rig.block();
		auto* site = static_cast<rack::SiteChains*>(rig.top->containerSurfaceOf(siteKeyOf(holder)));
		REQUIRE(site != nullptr);
		const DeviceInstanceId macros = site->pre().bridge()->addDevice(mod::macros::kTraits.tag);
		REQUIRE(macros != kNoInstance);
		site->pre().bridge()->setField(macros, 0, 0.5f);
		rig.block();
		const Source out = rig.top->siteExport(holder, Source::output(macros, 0));
		REQUIRE(out.isExport());
		const Destination dest = Destination::control(after, kCutoff);
		rig.top->setRouteAmount(out, dest, 0.25f);
		rig.settle();
		REQUIRE(rig.top->routeAmount(out, dest) == doctest::Approx(0.25f));
		const float base = rig.top->fieldAt(after, kCutoff);
		const auto& p = fx::filter::kDescriptor.params[kCutoff];
		const double want = device::withRouteOffset(p, base, 0.25 * 0.5 * device::routeSpanOf(p));
		CHECK(want != doctest::Approx(base)); // the claim below is about a value that moved

		const std::vector<DeviceInstanceId> made = rig.verbs->group(std::vector<DeviceInstanceId> { after }, kind);
		REQUIRE(made.size() == 1);
		rig.settle();
		CHECK(rig.order() == std::vector<DeviceInstanceId> { holder, made[0] });
		rack::RackBridge* inner = nullptr;
		const RackEngine* engine = nullptr;
		if (kind == rack::GroupKind::Chain) {
			inner = &rig.inner(made[0]);
			engine = rack::engineOf(rig.top->containerSurfaceOf(made[0]));
		}
		else {
			rack::parallel::Rack* r = rack::parallel::rackOf(rig.top->containerSurfaceOf(made[0]));
			REQUIRE(r != nullptr);
			REQUIRE(r->chainAt(0) != nullptr);
			inner = r->chainAt(0)->bridge();
			engine = &r->chainAt(0)->engine();
		}
		REQUIRE(inner != nullptr);
		REQUIRE(engine != nullptr);
		// THE ROUTE CROSSED: in the group's record, spelled as the host outside names it — and nothing was reported lost.
		CHECK(inner->routeAmount(out.asOuter(), dest) == doctest::Approx(0.25f));
		CHECK(rig.verbs->lastSaidForTest().find("could not cross") == std::string::npos);
		// AND IT ARRIVES: the grouped filter's cutoff as the engine inside resolved it.
		const size_t at = engine->slotOf(after);
		REQUIRE(at != RackEngine::kNoSlot);
		CHECK(engine->resolvedFieldForTest(at, kCutoff) == doctest::Approx(want).epsilon(1e-5));

		rig.history.undo();
		rig.settle();
		CHECK(rig.order() == std::vector<DeviceInstanceId> { holder, after });
		CHECK(rig.top->routeAmount(out, dest) == doctest::Approx(0.25f));
	}
}


TEST_CASE("OA + NZ (b): grouping a modulator with an effect — into a Chain, or into a Rack — keeps its route to a device "
          "left outside AFTER the group through the group's export, by value; before the group and the group's own "
          "controls are refused; one undo restores") {
	for (const rack::GroupKind kind : { rack::GroupKind::Chain, rack::GroupKind::Rack }) {
		CAPTURE(static_cast<int>(kind));
		Rig rig;
		const DeviceInstanceId head = rig.add(fx::filter::kTraits.tag);
		const DeviceInstanceId macros = rig.add(mod::macros::kTraits.tag);
		const DeviceInstanceId filter = rig.add(fx::filter::kTraits.tag);
		const DeviceInstanceId tail = rig.add(fx::filter::kTraits.tag);
		rig.top->setField(macros, 0, 0.5f);
		const Destination dest = Destination::control(tail, kCutoff);
		rig.top->setRouteAmount(lfoOut(macros), dest, 0.25f);
		rig.settle();
		const float base = rig.top->fieldAt(tail, kCutoff);
		const auto& p = fx::filter::kDescriptor.params[kCutoff];
		const double want = device::withRouteOffset(p, base, 0.25 * 0.5 * device::routeSpanOf(p));
		REQUIRE(want != doctest::Approx(base));
		const auto resolved = [&] {
			const size_t at = rig.engine.slotOf(tail);
			REQUIRE(at != RackEngine::kNoSlot);
			return rig.engine.resolvedFieldForTest(at, kCutoff);
		};
		REQUIRE(resolved() == doctest::Approx(want).epsilon(1e-5)); // the control: the route works before the group

		const std::vector<DeviceInstanceId> made = rig.verbs->group(std::vector<DeviceInstanceId> { macros, filter }, kind);
		REQUIRE(made.size() == 1);
		const DeviceInstanceId group = made[0];
		rig.settle();
		rig.settle();
		CHECK(rig.order() == std::vector<DeviceInstanceId> { head, group, tail });
		const Source out = rig.top->containerExport(group, lfoOut(macros));
		REQUIRE(out.isExport());
		CHECK(rig.top->routeAmount(out, dest) == doctest::Approx(0.25f));
		CHECK(rig.verbs->lastSaidForTest().find("could not cross") == std::string::npos);
		// THE VALUE ARRIVES through the group's export: the macro inside, the cutoff outside.
		CHECK(resolved() == doctest::Approx(want).epsilon(1e-5));
		// NOT UPSTREAM, AND NOT ITSELF: the device before the group, and the group's own controls.
		CHECK_FALSE(rig.top->mayRoute(out, Destination::control(head, kCutoff)));
		const device::Descriptor* gd = rig.top->descriptorOf(group);
		REQUIRE(gd != nullptr);
		size_t own = 0;
		while (own < gd->params.size() && (!gd->params[own].modulatable || static_cast<int>(own) == gd->traits.enableControl)) {
			++own;
		}
		REQUIRE(own < gd->params.size()); // a control a route COULD reach, so the refusal below is the rule's
		CHECK(rig.top->mayRoute(Source::core(0), Destination::control(group, own))); // a host macro reaches it
		CHECK_FALSE(rig.top->mayRoute(out, Destination::control(group, own)));
		// A SOURCE INSIDE THAT IS NO MODULATOR exports nothing.
		CHECK(rig.top->containerExport(group, Source::output(filter, 0)) == Source::nobody());

		rig.history.undo();
		rig.settle();
		CHECK(rig.order() == std::vector<DeviceInstanceId> { head, macros, filter, tail });
		CHECK(rig.top->routeAmount(lfoOut(macros), dest) == doctest::Approx(0.25f));
		CHECK(resolved() == doctest::Approx(want).epsilon(1e-5));
	}
}

TEST_CASE("NZ (b): a Chain's and a Rack's export tables save in their bodies — a copy of the patch, loaded fresh, reads "
          "the export at the device after, by value") {
	for (const rack::GroupKind kind : { rack::GroupKind::Chain, rack::GroupKind::Rack }) {
		CAPTURE(static_cast<int>(kind));
		rack::RackPatch saved;
		double want = 0.0;
		{
			Rig rig;
			const DeviceInstanceId macros = rig.add(mod::macros::kTraits.tag);
			const DeviceInstanceId tail = rig.add(fx::filter::kTraits.tag);
			rig.top->setField(macros, 0, 0.5f);
			rig.top->setRouteAmount(lfoOut(macros), Destination::control(tail, kCutoff), -0.25f);
			const std::vector<DeviceInstanceId> made = rig.verbs->group(std::vector<DeviceInstanceId> { macros }, kind);
			REQUIRE(made.size() == 1);
			rig.settle();
			const auto& p = fx::filter::kDescriptor.params[kCutoff];
			want = device::withRouteOffset(p, rig.top->fieldAt(tail, kCutoff), -0.25 * 0.5 * device::routeSpanOf(p));
			saved = rig.top->patch();
		}
		Rig fresh;
		(void)fresh.top->loadPatch(saved);
		fresh.settle();
		fresh.settle();
		REQUIRE(fresh.top->chainLength() == 2);
		const DeviceInstanceId tail = fresh.top->deviceAtPosition(1);
		const size_t at = fresh.engine.slotOf(tail);
		REQUIRE(at != RackEngine::kNoSlot);
		CHECK(fresh.engine.resolvedFieldForTest(at, kCutoff) == doctest::Approx(want).epsilon(1e-5));
	}
}
TEST_CASE("NZ release: a container's export row is freed when its last route goes, and reused by the next mint; a live "
          "route keeps its row") {
	for (const rack::GroupKind kind : { rack::GroupKind::Chain, rack::GroupKind::Rack }) {
		CAPTURE(static_cast<int>(kind));
		Rig rig;
		const DeviceInstanceId macros = rig.add(mod::macros::kTraits.tag);
		const DeviceInstanceId filter = rig.add(fx::filter::kTraits.tag);
		const DeviceInstanceId tail = rig.add(fx::filter::kTraits.tag);
		const Destination dest = Destination::control(tail, kCutoff);
		rig.top->setRouteAmount(lfoOut(macros), dest, 0.25f);
		const std::vector<DeviceInstanceId> made = rig.verbs->group(std::vector<DeviceInstanceId> { macros, filter }, kind);
		REQUIRE(made.size() == 1);
		const DeviceInstanceId group = made[0];
		rig.settle();
		const Source out = rig.top->containerExport(group, lfoOut(macros));
		REQUIRE(out.isExport());
		const size_t row = out.exportRow();
		const device::Descriptor* gd = rig.top->descriptorOf(group);
		void* surface = rig.top->containerSurfaceOf(group);
		REQUIRE((gd != nullptr && surface != nullptr));
		const auto named = [&] { return gd->container->exportedSource(surface, row); };
		REQUIRE(rig.top->routeAmount(out, dest) == doctest::Approx(0.25f));

		// A LIVE ROUTE KEEPS ITS ROW, however many blocks and services go by.
		for (int i = 0; i < 4; ++i) {
			rig.settle();
		}
		CHECK(named() == lfoOut(macros));

		// THE LAST ROUTE GOES; the row is free once the audio thread has drained the publish that dropped it.
		rig.top->setRouteAmount(out, dest, 0.0f);
		REQUIRE(rig.top->routeAmount(out, dest) == 0.0f);
		for (int i = 0; i < 4; ++i) {
			rig.settle();
		}
		CHECK(named() == Source::nobody());

		// AND THE NEXT MINT TAKES IT.
		const Source again = rig.top->containerExport(group, lfoOut(macros));
		REQUIRE(again.isExport());
		CHECK(again.exportRow() == row);
		CHECK(named() == lfoOut(macros));
	}
}

TEST_CASE("NZ release: a row minted for a gesture and not yet routed is not swept, and a freed row waits for the audio "
          "thread to drain the publish that dropped its route") {
	rack::ExportTable table;
	const backplane::Source a = Source::output(7, 0);
	const size_t row = table.mint(a);
	REQUIRE(row != rack::ExportTable::kNoRow);
	size_t used = 0;
	const auto readBy = [&](bool live) {
		return [live](size_t) { return live; };
	};
	// MINTED, NEVER ROUTED: arming mints before the route exists.
	for (int i = 0; i < 4; ++i) {
		used += table.sweep(readBy(false), 10 + static_cast<uint32_t>(i), 100);
	}
	CHECK(used == 0);
	CHECK(table.source(row) == a);
	// ROUTED, THEN UNROUTED: condemned at publish 20; freed only once `drained` reaches 20.
	CHECK(table.sweep(readBy(true), 19, 19) == 0);
	CHECK(table.sweep(readBy(false), 20, 19) == 0);
	CHECK(table.sweep(readBy(false), 21, 19) == 0);
	CHECK(table.source(row) == a);
	CHECK(table.sweep(readBy(false), 21, 20) == 1);
	CHECK(table.source(row) == Source::nobody());
	// A ROUTE THAT COMES BACK BEFORE THE DRAIN CATCHES UP SPARES IT.
	const size_t again = table.mint(a);
	CHECK(table.sweep(readBy(true), 30, 30) == 0);
	CHECK(table.sweep(readBy(false), 31, 30) == 0);
	CHECK(table.sweep(readBy(true), 32, 30) == 0);
	CHECK(table.sweep(readBy(false), 33, 99) == 0); // condemned afresh at 33, not freed by the older stamp
	CHECK(table.source(again) == a);
}

TEST_CASE("OA + NZ rung 5: a group made BEFORE the holder cannot read its PRE export, while an upstream LFO's route into it is kept") {
	Rig rig;
	const DeviceInstanceId lfo = rig.add(mod::lfo::kTraits.tag);
	const DeviceInstanceId before = rig.add(fx::filter::kTraits.tag);
	const DeviceInstanceId holder = rig.add(fx::filter::kTraits.tag);
	REQUIRE(rig.top->openSite(holder));
	rig.block();
	auto* site = static_cast<rack::SiteChains*>(rig.top->containerSurfaceOf(siteKeyOf(holder)));
	REQUIRE(site != nullptr);
	const DeviceInstanceId macros = site->pre().bridge()->addDevice(mod::macros::kTraits.tag);
	REQUIRE(macros != kNoInstance);
	rig.block();
	const Source out = rig.top->siteExport(holder, Source::output(macros, 0));
	REQUIRE(out.isExport());
	// The only export route a hand could make here goes INTO the holder; the device before it is refused outright.
	CHECK_FALSE(rig.top->mayRoute(out, Destination::control(before, kCutoff)));
	// AN UPSTREAM LFO's ROUTE INTO THE SAME DEVICE is the control: grouping keeps it, so the gate is answering.
	rig.top->setRouteAmount(lfoOut(lfo), Destination::control(before, kCutoff), 0.5f);
	const std::vector<DeviceInstanceId> made = rig.verbs->group(std::vector<DeviceInstanceId> { before }, rack::GroupKind::Chain);
	REQUIRE(made.size() == 1);
	rig.settle();
	rack::RackBridge& inner = rig.inner(made[0]);
	CHECK(inner.routeAmount(lfoOut(lfo).asOuter(), Destination::control(before, kCutoff)) == doctest::Approx(0.5f));
	CHECK_FALSE(inner.mayRoute(out.asOuter(), Destination::control(before, kCutoff)));
}

//------------------------------------------------------------------------------------------------//
// FEATHERS' FX LANE — the same verbs over `ParamBridge`                                            //
//------------------------------------------------------------------------------------------------//

namespace {

/// FEATHERS' SHAPE: the bridge (strip + fx lane in one record, owning the pool), the synth that runs the lane.
struct FeathersRig {
	ParamSet params;
	Synth synth { params };
	ParamBridge bridge;
	ui::EditHistory history;
	std::unique_ptr<ui::FragmentVerbs> verbs;
	std::vector<float> l = std::vector<float>(64, 0.0f);
	std::vector<float> r = std::vector<float>(64, 0.0f);

	FeathersRig() {
		synth.activate(48000.0, 4096);
		params.setBase(static_cast<clap_id>(ParamId::VoiceLevel), 1.0);
		synth.setCatalogue(hostCatalogue());
		synth.setHostSlotBus(&bridge.slotBus());
		synth.setOuterGateMaker(&rack::ChainRecord::gateOver, &bridge.chainRecord());
		synth.setSlotLinkMaker(&ParamBridge::slotLinkOver, &bridge);
		verbs = std::make_unique<ui::FragmentVerbs>(bridge, history, nullptr, [this] { return lane(); });
		rack::clearPasteboard();
	}
	std::vector<DeviceInstanceId> lane() const {
		std::vector<DeviceInstanceId> out;
		for (size_t p = 0; p < bridge.chainRecord().length(kLaneChain); ++p) {
			out.push_back(bridge.chainRecord().deviceAt(kLaneChain, p));
		}
		return out;
	}
	DeviceInstanceId addFx(const char* tag) {
		const size_t slot = bridge.addFxDevice(tag);
		REQUIRE(slot != kNoPosition);
		bridge.refresh();
		const DeviceInstanceId id = bridge.fxDeviceAt(slot);
		REQUIRE(id != kNoInstance);
		settle();
		return id;
	}
	DeviceInstanceId addStrip(const char* tag) {
		const size_t slot = bridge.addRackDevice(tag);
		REQUIRE(slot != kNoPosition);
		bridge.refresh();
		const DeviceInstanceId id = bridge.rackDeviceAt(slot);
		REQUIRE(id != kNoInstance);
		settle();
		return id;
	}
	rack::RackBridge& inner(DeviceInstanceId chain) {
		rack::RackBridge* b = rack::bridgeOf(bridge.fxBodies().surfaceOf(chain));
		REQUIRE(b != nullptr);
		return *b;
	}
	void settle() {
		for (int i = 0; i < 3; ++i) {
			publishEditorLanes(bridge, params, synth);
			(void)synth.provisionDevices(bridge.chainState(), bridge.publishedChains().sequence(), &bridge.fxBodies());
			(void)bridge.reconcileSlotsIfMoved();
			(void)bridge.serviceSlotRoads();
			synth.serviceContainers(bridge.chainState(), &bridge.fxBodies());
			synth.render(l.data(), r.data(), 64);
		}
	}
};

} // namespace

TEST_CASE("rack-teardown-uaf: in Feathers' shape, the plugin's unhook leaves no chain in the lane, nor any level below it, "
          "linked to the bridge that is about to go") {
	// THE PLUGIN'S ORDER: `FeathersPlugin` declares the synth before the bridge, so the bridge — the slot pool's owner
	// (`ParamBridge::slots_`) and every outer gate's host — dies FIRST, and the lane's engine, holding a Chain and the
	// racks nested in it, dies after. A nested rack dying services its sites, and a site's bridge pulls its bindings
	// through its link: a read of the destroyed bridge. ASan cannot see it here — the bridge is a member, destroyed but
	// not yet freed when the synth goes — so this asserts the cause BY VALUE: after `Synth::unhookHostLinks` (what
	// `~FeathersPlugin` calls first), no level holds a live link. (The standalone shape, where the owner IS freed, is
	// the ASan case: "Depth gate, a site ...", red before this branch.)
	// KNOWN-BAD: `LaneRack::unhookHostLinks` without `converger_.unhookSlotLinks()` — the two "after" checks go red.
	FeathersRig rig;
	const DeviceInstanceId chain = rig.addFx(composite::kTraits.tag);
	rack::RackBridge& in = rig.inner(chain);
	const DeviceInstanceId filter = in.addDevice(fx::filter::kTraits.tag);
	REQUIRE(filter != kNoInstance);
	rig.settle();
	REQUIRE(in.openSite(filter));
	rig.settle();
	auto* site = static_cast<rack::SiteChains*>(in.containerSurfaceOf(siteKeyOf(filter)));
	REQUIRE(site != nullptr);
	rack::RackBridge& pre = *site->pre().bridge();
	const DeviceInstanceId preFilter = pre.addDevice(fx::filter::kTraits.tag);
	REQUIRE(preFilter != kNoInstance);
	rig.settle();
	// BEFORE: the Chain's level and the site's PRE inside it are both linked to the plugin's pool.
	REQUIRE(in.slotLinkFor(filter).live());
	REQUIRE(pre.slotLinkFor(preFilter).live());
	rig.synth.unhookHostLinks();
	// AFTER: neither — each level handed its children none, all the way down.
	CHECK_FALSE(in.slotLinkFor(filter).live());
	CHECK_FALSE(pre.slotLinkFor(preFilter).live());
}

TEST_CASE("OA: in Feathers' fx lane, Group into Chain keeps a strip LFO's route into a grouped filter through the chain's outer gate, and Duplicate copies with routes among; both undo") {
	FeathersRig rig;
	const std::vector<DeviceInstanceId> before = rig.lane();
	const DeviceInstanceId lfo = rig.addStrip(mod::lfo::kTraits.tag);
	const DeviceInstanceId filter = rig.addFx(fx::filter::kTraits.tag);
	const DeviceInstanceId delay = rig.addFx(fx::delay::kTraits.tag);
	rig.bridge.setRouteAmount(lfoOut(lfo), Destination::control(filter, kCutoff), 0.5f);
	(void)rig.bridge.takeStateDirty();

	const std::vector<DeviceInstanceId> dup = rig.verbs->duplicate(std::vector<DeviceInstanceId> { filter });
	REQUIRE(dup.size() == 1);
	CHECK(rig.bridge.routeAmount(lfoOut(lfo), Destination::control(dup[0], kCutoff)) == doctest::Approx(0.5f));
	CHECK(rig.bridge.takeStateDirty());
	rig.history.undo();
	CHECK(rig.bridge.chainRecord().holds(dup[0]) == false);

	const std::vector<DeviceInstanceId> made =
	    rig.verbs->group(std::vector<DeviceInstanceId> { filter, delay }, rack::GroupKind::Chain);
	REQUIRE(made.size() == 1);
	rig.settle();
	rack::RackBridge& inner = rig.inner(made[0]);
	REQUIRE(inner.chainLength() == 2);
	CHECK(inner.deviceAtPosition(0) == filter);
	CHECK(inner.routeAmount(lfoOut(lfo).asOuter(), Destination::control(filter, kCutoff)) == doctest::Approx(0.5f));
	std::vector<DeviceInstanceId> expected = before;
	expected.push_back(made[0]);
	CHECK(rig.lane() == expected);

	rig.history.undo();
	rig.settle();
	expected = before;
	expected.push_back(filter);
	expected.push_back(delay);
	CHECK(rig.lane() == expected);
	CHECK(rig.bridge.routeAmount(lfoOut(lfo), Destination::control(filter, kCutoff)) == doctest::Approx(0.5f));
}


//------------------------------------------------------------------------------------------------//
// FEATHERS' MODULATOR STRIP — the same verbs over the bridge's strip site (backlog PN)            //
//------------------------------------------------------------------------------------------------//

namespace {

/// THE STRIP'S ORDER, and a `FragmentVerbs` over `ParamBridge::stripFragmentSite` — what `ModPanel` owns.
struct StripRig : FeathersRig {
	std::unique_ptr<ui::FragmentVerbs> strip;
	StripRig() {
		strip = std::make_unique<ui::FragmentVerbs>(bridge.stripFragmentSite(), history, nullptr,
		                                            [this] { return stripOrder(); });
	}
	std::vector<DeviceInstanceId> stripOrder() const {
		std::vector<DeviceInstanceId> out;
		for (size_t p = 0; p < bridge.chainRecord().length(kStripChain); ++p) {
			out.push_back(bridge.chainRecord().deviceAt(kStripChain, p));
		}
		return out;
	}
};

const size_t kLfoTimeBase = keyIndex(mod::lfo::kDescriptor, "time_base");
Destination radiusOf(size_t slot) { return Destination::core(travelDestinationIndex(slot, TravelAxis::Radius)); }

} // namespace

TEST_CASE("PN: in Feathers' modulator strip, Duplicate places the copy after the original with its routes OUT duplicated "
          "to the same destinations, routes among re-pointed, the originals untouched; one undo step") {
	// THE ROUTE RULE FOR THE STRIP (PROVISIONAL): a copied modulator keeps its routes to destinations — a core
	// destination and an fx-lane control here — as new routes from the copy, at the same amounts. D3 (outbound not
	// copied) still holds on the fx lane; the strip's site says otherwise (`fragmentCopiesOutbound`).
	// KNOWN-BAD: `StripSite::fragmentCopiesOutbound` answering false, and the copy's routes out read 0.
	StripRig rig;
	const DeviceInstanceId a = rig.addStrip(mod::lfo::kTraits.tag);
	const DeviceInstanceId b = rig.addStrip(mod::lfo::kTraits.tag);
	const DeviceInstanceId filter = rig.addFx(fx::filter::kTraits.tag);
	rig.bridge.setRouteAmount(lfoOut(a), radiusOf(0), 0.5f);                                   // out, to the core
	rig.bridge.setRouteAmount(lfoOut(a), Destination::control(filter, kCutoff), 0.3f);         // out, to the lane
	rig.bridge.setRouteAmount(lfoOut(a), Destination::control(b, kLfoTimeBase), 0.2f);         // among {a, b}
	rig.bridge.setRouteAmount(Source::core(0), Destination::control(a, kLfoTimeBase), 0.25f); // in: a macro
	rig.settle();
	(void)rig.bridge.takeStateDirty();
	const std::vector<DeviceInstanceId> before = rig.stripOrder();
	const size_t steps = rig.history.length();

	const std::vector<DeviceInstanceId> made = rig.strip->duplicate(std::vector<DeviceInstanceId> { a, b });
	REQUIRE(made.size() == 2);
	const DeviceInstanceId a2 = made[0];
	const DeviceInstanceId b2 = made[1];
	CHECK(a2 != a);
	CHECK(b2 != b);
	// RIGHT AFTER THE LAST SELECTED, in order — and in the STRIP, not the lane.
	std::vector<DeviceInstanceId> expected = before;
	const auto afterB = std::find(expected.begin(), expected.end(), b) + 1;
	expected.insert(afterB, { a2, b2 });
	CHECK(rig.stripOrder() == expected);
	CHECK(rig.bridge.chainRecord().chainOf(a2) == kStripChain);
	// OUT: the copy reaches what the original reaches, at the same amounts.
	CHECK(rig.bridge.routeAmount(lfoOut(a2), radiusOf(0)) == doctest::Approx(0.5f));
	CHECK(rig.bridge.routeAmount(lfoOut(a2), Destination::control(filter, kCutoff)) == doctest::Approx(0.3f));
	// AMONG: re-pointed copy to copy, and the copy does not reach the ORIGINAL b.
	CHECK(rig.bridge.routeAmount(lfoOut(a2), Destination::control(b2, kLfoTimeBase)) == doctest::Approx(0.2f));
	CHECK(rig.bridge.routeAmount(lfoOut(a2), Destination::control(b, kLfoTimeBase)) == 0.0f);
	// IN: kept, in the same record.
	CHECK(rig.bridge.routeAmount(Source::core(0), Destination::control(a2, kLfoTimeBase)) == doctest::Approx(0.25f));
	// THE ORIGINALS UNTOUCHED.
	CHECK(rig.bridge.routeAmount(lfoOut(a), radiusOf(0)) == doctest::Approx(0.5f));
	CHECK(rig.bridge.routeAmount(lfoOut(a), Destination::control(filter, kCutoff)) == doctest::Approx(0.3f));
	CHECK(rig.bridge.routeAmount(lfoOut(a), Destination::control(b, kLfoTimeBase)) == doctest::Approx(0.2f));
	CHECK(rig.bridge.takeStateDirty());
	CHECK(rig.history.length() == steps + 1);

	rig.history.undo();
	CHECK(rig.stripOrder() == before);
	CHECK(rig.bridge.routeAmount(lfoOut(a2), radiusOf(0)) == 0.0f);
	rig.history.redo();
	CHECK(rig.stripOrder() == expected);
	CHECK(rig.bridge.routeAmount(lfoOut(a2), radiusOf(0)) == doctest::Approx(0.5f));
}

TEST_CASE("PN: in the strip, Cut takes the modulators and their routes out as one step, Paste puts copies back after the "
          "anchor with their routes out, and two undos restore the originals under their own ids") {
	// KNOWN-BAD: `fragmentChain` answering kLaneChain in `StripSite`, and the cut finds nothing in the strip (the lane
	// holds no LFO), so the order does not change.
	StripRig rig;
	const DeviceInstanceId a = rig.addStrip(mod::lfo::kTraits.tag);
	const DeviceInstanceId b = rig.addStrip(mod::lfo::kTraits.tag);
	rig.bridge.setRouteAmount(lfoOut(a), radiusOf(1), 0.75f);
	rig.settle();
	const std::vector<DeviceInstanceId> before = rig.stripOrder();
	const size_t laneBefore = rig.lane().size();

	(void)rig.strip->cut(std::vector<DeviceInstanceId> { a });
	CHECK_FALSE(rig.bridge.chainRecord().holds(a));
	CHECK(rig.bridge.routeAmount(lfoOut(a), radiusOf(1)) == 0.0f);
	CHECK(rig.stripOrder().size() == before.size() - 1);
	REQUIRE(ui::FragmentVerbs::canPaste());

	const std::vector<DeviceInstanceId> pasted = rig.strip->paste(b);
	REQUIRE(pasted.size() == 1);
	const DeviceInstanceId a2 = pasted[0];
	CHECK(a2 != a);
	// AFTER THE ANCHOR, in the strip; the lane untouched.
	const std::vector<DeviceInstanceId> now = rig.stripOrder();
	const auto at = std::find(now.begin(), now.end(), b);
	REQUIRE(at != now.end());
	REQUIRE(at + 1 != now.end());
	CHECK(*(at + 1) == a2);
	CHECK(rig.lane().size() == laneBefore);
	// ITS ROUTE OUT CAME WITH IT — the clip was lifted from this record (same origin).
	CHECK(rig.bridge.routeAmount(lfoOut(a2), radiusOf(1)) == doctest::Approx(0.75f));

	rig.history.undo(); // the paste
	CHECK_FALSE(rig.bridge.chainRecord().holds(a2));
	rig.history.undo(); // the cut
	CHECK(rig.stripOrder() == before);
	CHECK(rig.bridge.routeAmount(lfoOut(a), radiusOf(1)) == doctest::Approx(0.75f));
}

TEST_CASE("PN: in the strip, Remove of several and the block move are one undo step each, and the strip offers no Group") {
	StripRig rig;
	const DeviceInstanceId a = rig.addStrip(mod::lfo::kTraits.tag);
	const DeviceInstanceId b = rig.addStrip(mod::lfo::kTraits.tag);
	const DeviceInstanceId c = rig.addStrip(mod::lfo::kTraits.tag);
	rig.bridge.setRouteAmount(lfoOut(c), radiusOf(0), 0.4f);
	rig.settle();
	const std::vector<DeviceInstanceId> before = rig.stripOrder();

	// THE BLOCK MOVE: {a, c} to the strip's front, contiguous, in order.
	REQUIRE(rig.strip->moveBlock(std::vector<DeviceInstanceId> { a, c }, 0));
	const std::vector<DeviceInstanceId> moved = rig.stripOrder();
	REQUIRE(moved.size() >= 3);
	CHECK(moved[0] == a);
	CHECK(moved[1] == c);
	rig.history.undo();
	CHECK(rig.stripOrder() == before);

	// REMOVE {a, c}: one step; the undo puts both back under their ids with c's route.
	CHECK(rig.strip->remove(std::vector<DeviceInstanceId> { a, c }) == 2);
	CHECK_FALSE(rig.bridge.chainRecord().holds(a));
	CHECK_FALSE(rig.bridge.chainRecord().holds(c));
	CHECK(rig.bridge.chainRecord().holds(b));
	rig.history.undo();
	CHECK(rig.stripOrder() == before);
	CHECK(rig.bridge.routeAmount(lfoOut(c), radiusOf(0)) == doctest::Approx(0.4f));

	// NO GROUP: refused with words, and nothing changes. KNOWN-BAD: `StripSite::fragmentGroups` answering true.
	CHECK_FALSE(rig.strip->groupRefusal(std::vector<DeviceInstanceId> { a, b }).empty());
	CHECK(rig.strip->group(std::vector<DeviceInstanceId> { a, b }, rack::GroupKind::Chain).empty());
	CHECK(rig.stripOrder() == before);
	// AND THE FX LANE STILL GROUPS: the refusal is the strip's, not the verbs'.
	CHECK(rig.verbs->groupRefusal(std::vector<DeviceInstanceId> { rig.addFx(fx::filter::kTraits.tag) }).empty());
}

TEST_CASE("PN: D3 still holds on the fx lane — a duplicated lane LFO leaves its route out behind") {
	// THE STRIP'S RULE IS THE STRIP'S: the same verb over the lane's site does not copy routes out.
	// KNOWN-BAD: `placeFragment` carrying outbound on every fresh same-origin copy (dropping the site's say).
	StripRig rig;
	const DeviceInstanceId filter = rig.addFx(fx::filter::kTraits.tag);
	const DeviceInstanceId lfo = rig.addFx(mod::lfo::kTraits.tag);
	rig.bridge.setRouteAmount(lfoOut(lfo), radiusOf(0), 0.5f);
	(void)filter;
	rig.settle();
	const std::vector<DeviceInstanceId> made = rig.verbs->duplicate(std::vector<DeviceInstanceId> { lfo });
	REQUIRE(made.size() == 1);
	CHECK(rig.bridge.routeAmount(lfoOut(made[0]), radiusOf(0)) == 0.0f);
	CHECK(rig.bridge.routeAmount(lfoOut(lfo), radiusOf(0)) == doctest::Approx(0.5f));
}
TEST_CASE("NZ release: a SITE's own export row is freed when its last route goes — into the holder, a device after it, or the site's own POST — and reused by the next mint") {
	Rig rig;
	const DeviceInstanceId holder = rig.add(fx::filter::kTraits.tag);
	const DeviceInstanceId after = rig.add(fx::filter::kTraits.tag);
	REQUIRE(rig.top->openSite(holder));
	rig.block();
	auto* site = static_cast<rack::SiteChains*>(rig.top->containerSurfaceOf(siteKeyOf(holder)));
	REQUIRE(site != nullptr);
	const DeviceInstanceId macros = site->pre().bridge()->addDevice(mod::macros::kTraits.tag);
	REQUIRE(macros != kNoInstance);
	const DeviceInstanceId postFilter = site->post().bridge()->addDevice(fx::filter::kTraits.tag);
	REQUIRE(postFilter != kNoInstance);
	rig.settle();
	const Source inner = Source::output(macros, 0);
	const Source out = rig.top->siteExport(holder, inner);
	REQUIRE(out.isSiteExport());
	const size_t row = out.exportRow();
	const auto named = [&] { return site->exportedSource(row); };

	// THREE READERS IN TURN: each alone keeps the row through several services, and the row goes when none is left.
	const Destination intoHolder = Destination::control(holder, kCutoff);
	const Destination intoAfter = Destination::control(after, kCutoff);
	const Destination intoPost = Destination::control(postFilter, kCutoff);
	rig.top->setRouteAmount(out, intoHolder, 0.25f);
	rig.settle();
	CHECK(named() == inner);
	rig.top->setRouteAmount(out, intoAfter, 0.25f);
	rig.top->setRouteAmount(out, intoHolder, 0.0f);
	rig.settle();
	rig.settle();
	CHECK(named() == inner); // the device after the holder still reads it (rung 4)
	REQUIRE(site->post().bridge()->mayRoute(out.asOuter(), intoPost));
	site->post().bridge()->setRouteAmount(out.asOuter(), intoPost, 0.5f);
	rig.top->setRouteAmount(out, intoAfter, 0.0f);
	rig.settle();
	rig.settle();
	CHECK(named() == inner); // the site's own POST still reads it
	site->post().bridge()->setRouteAmount(out.asOuter(), intoPost, 0.0f);
	rig.settle();
	rig.settle();
	CHECK(named() == Source::nobody()); // no reader left: freed once the audio thread caught up
	CHECK(site->exportRowsUsed() == 0);

	// THE NEXT MINT TAKES IT.
	const Source again = rig.top->siteExport(holder, inner);
	REQUIRE(again.isSiteExport());
	CHECK(again.exportRow() == row);
	CHECK(named() == inner);
}

namespace {

/// A CHAIN HOLDING A MACROS AT 0.5, A FILTER AFTER IT, AND A ROUTE FROM THE CHAIN'S EXPORT OF THE MACROS INTO THE FILTER'S
/// CUTOFF — the setting every NZ any-depth move below starts from.
struct ExportedHolder {
	Rig rig;
	DeviceInstanceId holder = kNoInstance;
	DeviceInstanceId filter = kNoInstance;
	DeviceInstanceId macros = kNoInstance;
	float base = 0.0f;
	ExportedHolder() {
		holder = rig.add(composite::kTraits.tag);
		filter = rig.add(fx::filter::kTraits.tag);
		rig.settle();
		macros = rig.inner(holder).addDevice(mod::macros::kTraits.tag);
		REQUIRE(macros != kNoInstance);
		rig.inner(holder).setField(macros, 0, 0.5f);
		rig.settle();
		base = cutoff();
		const Source out = rig.top->containerExport(holder, Source::output(macros, 0));
		REQUIRE(out.isContainerExport());
		rig.top->setRouteAmount(out, Destination::control(filter, kCutoff), 0.25f);
		rig.settle();
		rig.settle();
	}
	[[nodiscard]] float cutoff() const {
		const size_t at = rig.engine.slotOf(filter);
		REQUIRE(at != RackEngine::kNoSlot);
		return rig.engine.resolvedFieldForTest(at, kCutoff);
	}
	[[nodiscard]] double want() const {
		const device::ParamDesc& p = fx::filter::kDescriptor.params[kCutoff];
		return device::withRouteOffset(p, base, 0.25 * 0.5 * device::routeSpanOf(p));
	}
	/// Every route into the filter's cutoff, as the top record has them.
	[[nodiscard]] std::vector<Source> sourcesIn() const {
		std::vector<Source> out;
		rig.top->forEachRoute([&](const backplane::Route& r) {
			if (r.destination == Destination::control(filter, kCutoff)) {
				out.push_back(r.source);
			}
		});
		return out;
	}
};

} // namespace

TEST_CASE("NZ any depth + OA: a holder whose export is routed out, grouped into a Chain, then that group grouped again — the route reaches through every level by value; undo ungroups each step back to the original route, redo regroups") {
	ExportedHolder h;
	REQUIRE(h.cutoff() == doctest::Approx(h.want()).epsilon(1e-5));
	// GROUPED: the group re-exports the holder's row, and the route outside reads the group's export.
	const std::vector<DeviceInstanceId> g1 = h.rig.verbs->group(std::vector<DeviceInstanceId> { h.holder }, rack::GroupKind::Chain);
	REQUIRE(g1.size() == 1);
	h.rig.settle();
	h.rig.settle();
	REQUIRE(h.sourcesIn().size() == 1);
	CHECK(h.sourcesIn()[0].isContainerExport());
	CHECK(h.sourcesIn()[0].device == g1[0]);
	CHECK(h.cutoff() == doctest::Approx(h.want()).epsilon(1e-5));
	// AND AGAIN, A LEVEL DEEPER: three tables in a row now.
	const std::vector<DeviceInstanceId> g2 = h.rig.verbs->group(std::vector<DeviceInstanceId> { g1[0] }, rack::GroupKind::Rack);
	REQUIRE(g2.size() == 1);
	h.rig.settle();
	h.rig.settle();
	REQUIRE(h.sourcesIn().size() == 1);
	CHECK(h.sourcesIn()[0].device == g2[0]);
	// THE RACK THE GROUP MADE STANDS ENABLED: its row's first write (the chain-slot mirror) widens it with the declared
	// defaults, not zeros (it stood disabled until 2026-10-31).
	auto* made = rack::parallel::rackOf(h.rig.top->containerSurfaceOf(g2[0]));
	REQUIRE(made != nullptr);
	CHECK(made->fieldForTest(rack::parallel::Param::Enabled) == 1.0f);
	CHECK(h.cutoff() == doctest::Approx(h.want()).epsilon(1e-5));
	// UNDO, STEP BY STEP, BACK TO THE HOLDER'S OWN EXPORT.
	h.rig.history.undo();
	h.rig.settle();
	h.rig.settle();
	REQUIRE(h.sourcesIn().size() == 1);
	CHECK(h.sourcesIn()[0].device == g1[0]);
	CHECK(h.cutoff() == doctest::Approx(h.want()).epsilon(1e-5));
	h.rig.history.undo();
	h.rig.settle();
	h.rig.settle();
	REQUIRE(h.sourcesIn().size() == 1);
	CHECK(h.sourcesIn()[0].device == h.holder);
	CHECK(h.cutoff() == doctest::Approx(h.want()).epsilon(1e-5));
	// AND REDO.
	h.rig.history.redo();
	h.rig.settle();
	h.rig.settle();
	CHECK(h.cutoff() == doctest::Approx(h.want()).epsilon(1e-5));
}

TEST_CASE("NZ any depth + OA: duplicating a holder whose export is routed leaves the original route by value, and the copy's table returns to zero rows (nothing reads it)") {
	ExportedHolder h;
	const std::vector<DeviceInstanceId> copies = h.rig.verbs->duplicate(std::vector<DeviceInstanceId> { h.holder });
	REQUIRE(copies.size() == 1);
	for (int i = 0; i < 6; ++i) {
		h.rig.settle();
	}
	CHECK(h.cutoff() == doctest::Approx(h.want()).epsilon(1e-5));
	const device::Descriptor* d = h.rig.top->descriptorOf(copies[0]);
	void* surface = h.rig.top->containerSurfaceOf(copies[0]);
	REQUIRE((d != nullptr && surface != nullptr));
	size_t rows = 0;
	for (size_t r = 0; r < device::kSiteExportRows; ++r) {
		rows += d->container->exportedSource(surface, r) != Source::nobody() ? 1 : 0;
	}
	CHECK(rows == 0);
}


// ---------------------------------------------------------------------------------------------
// THE DEPTH GATE (2026-10-31): every road that deepens asks `rack::fitsNesting`, so none can build live what a reload
// gives back empty. Each road twice: where its deepest new chain would stand at 21 it is refused, said, and changes
// nothing (the acted-on chain's patch and the whole top's, byte for byte); where it would stand at 20 it is allowed,
// and a Filter's cutoff placed there comes back BY VALUE, at depth 20, from a save loaded into a fresh instance.
// ---------------------------------------------------------------------------------------------

namespace {

constexpr size_t kTop = rack::kMaxUsefulDepth; // 20, the nesting limit

bool isTag(const rack::RackBridge& b, DeviceInstanceId id, const char* tag) {
	const device::Descriptor* d = b.descriptorOf(id);
	return d != nullptr && device::sameTag(d->traits.tag, tag);
}

/// EVERY CHAIN BELOW `b`, with its depth: a Chain's, each of a Rack's, a site's PRE and POST.
template <typename F>
void forEachChainBelow(rack::RackBridge& b, size_t depth, F&& f) {
	f(b, depth);
	for (size_t p = 0; p < b.chainLength(); ++p) {
		const DeviceInstanceId id = b.deviceAtPosition(p);
		if (isTag(b, id, composite::kTraits.tag)) {
			if (rack::RackBridge* in = rack::bridgeOf(b.containerSurfaceOf(id)); in != nullptr) {
				forEachChainBelow(*in, depth + 1, f);
			}
		}
		else if (isTag(b, id, rack::parallel::kTraits.tag)) {
			if (void* s = b.containerSurfaceOf(id); s != nullptr) {
				rack::parallel::Rack* rk = rack::parallel::rackOf(s);
				for (size_t c = 0; c < rk->chainCount(); ++c) {
					if (rk->chainAt(c) != nullptr && rk->chainAt(c)->bridge() != nullptr) {
						forEachChainBelow(*rk->chainAt(c)->bridge(), depth + 1, f);
					}
				}
			}
		}
		if (b.hasSite(id)) {
			if (auto* site = static_cast<rack::SiteChains*>(b.containerSurfaceOf(siteKeyOf(id))); site != nullptr) {
				for (rack::InnerChain* lane : { &site->pre(), &site->post() }) {
					if (lane->bridge() != nullptr) {
						forEachChainBelow(*lane->bridge(), depth + 1, f);
					}
				}
			}
		}
	}
}

/// THE MARK: a Filter's cutoff no default lands on, so finding it is finding THIS Filter.
float markCutoff() {
	const auto& p = fx::filter::kDescriptor.params[kCutoff];
	return static_cast<float>(p.min + 0.3137 * (p.max - p.min));
}

/// THE DEPTHS AT WHICH A FILTER CARRIES THE MARK.
std::vector<size_t> markedDepths(rack::RackBridge& top) {
	std::vector<size_t> out;
	forEachChainBelow(top, 0, [&out](rack::RackBridge& b, size_t depth) {
		for (size_t p = 0; p < b.chainLength(); ++p) {
			const DeviceInstanceId id = b.deviceAtPosition(p);
			if (isTag(b, id, fx::filter::kTraits.tag) && b.fieldAt(id, kCutoff) == doctest::Approx(markCutoff())) {
				out.push_back(depth);
			}
		}
	});
	return out;
}

std::string patchText(const rack::RackBridge& b) {
	std::string text;
	REQUIRE(rack::rackToText(b.patch(), &text, false));
	return text;
}

/// A TOWER: the Chain plugin's top (`Rig`) and `levels` Chains, one inside the next, each holding a Filter first.
struct Tower {
	Rig rig;
	std::vector<rack::RackBridge*> at;         ///< at[d] is the chain at depth d; at[0] the top
	std::vector<DeviceInstanceId> filter;      ///< filter[d] in at[d], d >= 1
	std::vector<DeviceInstanceId> chain;       ///< chain[d] in at[d - 1] holds at[d]

	explicit Tower(size_t levels) {
		at.push_back(rig.top.get());
		filter.push_back(kNoInstance);
		chain.push_back(kNoInstance);
		for (size_t d = 1; d <= levels; ++d) {
			CAPTURE(d);
			const DeviceInstanceId c = at[d - 1]->addDevice(composite::kTraits.tag);
			REQUIRE(c != kNoInstance);
			rig.block();
			rig.block();
			rack::RackBridge* in = rack::bridgeOf(at[d - 1]->containerSurfaceOf(c));
			REQUIRE(in != nullptr);
			const DeviceInstanceId f = in->addDevice(fx::filter::kTraits.tag);
			REQUIRE(f != kNoInstance);
			rig.block();
			at.push_back(in);
			filter.push_back(f);
			chain.push_back(c);
		}
		rig.settle();
	}
	/// What a hand sees of the whole tree, and of the chain at `d` — the "nothing changed" readings.
	std::pair<std::string, std::string> snapshot(size_t d) {
		rig.settle();
		return { patchText(*rig.top), patchText(*at[d]) };
	}
	/// SAVED AS TEXT AND LOADED INTO A FRESH CHAIN PLUGIN (`rack_clap.cpp`'s road), then where the mark stands there.
	std::vector<size_t> markedAfterReload() {
		rig.settle();
		std::string text;
		REQUIRE(rack::rackToText(rig.top->patch(), &text, false));
		Rig fresh;
		rack::RackPatch patch;
		REQUIRE(rack::parseRackText(text, &patch));
		(void)fresh.top->loadPatch(patch);
		for (int i = 0; i < 4; ++i) {
			fresh.settle();
		}
		return markedDepths(*fresh.top);
	}
};

ui::FragmentVerbs verbsOn(rack::RackBridge& b, ui::EditHistory& history) {
	return ui::FragmentVerbs(b, history, nullptr, [&b] {
		std::vector<DeviceInstanceId> out;
		for (size_t p = 0; p < b.chainLength(); ++p) {
			out.push_back(b.deviceAtPosition(p));
		}
		return out;
	});
}

/// A CHAIN HOLDING `inner` MORE CHAINS, the innermost holding a marked Filter — added at the end of `b`.
DeviceInstanceId addMarkedChain(Rig& rig, rack::RackBridge& b, size_t inner) {
	const DeviceInstanceId outer = b.addDevice(composite::kTraits.tag);
	REQUIRE(outer != kNoInstance);
	rig.block();
	rig.block();
	rack::RackBridge* in = rack::bridgeOf(b.containerSurfaceOf(outer));
	REQUIRE(in != nullptr);
	for (size_t i = 0; i < inner; ++i) {
		const DeviceInstanceId c = in->addDevice(composite::kTraits.tag);
		REQUIRE(c != kNoInstance);
		rig.block();
		rig.block();
		in = rack::bridgeOf(in->containerSurfaceOf(c));
		REQUIRE(in != nullptr);
	}
	const DeviceInstanceId f = in->addDevice(fx::filter::kTraits.tag);
	REQUIRE(f != kNoInstance);
	rig.block();
	in->setField(f, kCutoff, markCutoff());
	rig.settle();
	return outer;
}

} // namespace

TEST_CASE("Depth gate, the add menu: a container in the chain at 20 is refused and nothing changes; at 19 it goes in, and what it holds comes back from a reload at 20") {
	{
		Tower t(kTop);
		const auto before = t.snapshot(kTop);
		CHECK(t.at[kTop]->addDevice(composite::kTraits.tag) == kNoInstance);
		CHECK(t.at[kTop]->addDevice(rack::parallel::kTraits.tag) == kNoInstance);
		CHECK(t.snapshot(kTop) == before);
		CHECK(t.at[kTop]->addDevice(fx::filter::kTraits.tag) != kNoInstance); // a leaf at the same depth: a nesting bound, not a dead chain
	}
	Tower t(kTop - 1);
	const DeviceInstanceId c = t.at[kTop - 1]->addDevice(composite::kTraits.tag);
	REQUIRE(c != kNoInstance);
	t.rig.block();
	t.rig.block();
	rack::RackBridge* in = rack::bridgeOf(t.at[kTop - 1]->containerSurfaceOf(c));
	REQUIRE(in != nullptr);
	CHECK(in->nestingDepth() == kTop);
	const DeviceInstanceId f = in->addDevice(fx::filter::kTraits.tag);
	REQUIRE(f != kNoInstance);
	t.rig.block();
	in->setField(f, kCutoff, markCutoff());
	CHECK(markedDepths(*t.rig.top) == std::vector<size_t> { kTop });
	CHECK(t.markedAfterReload() == std::vector<size_t> { kTop });
}

TEST_CASE("Depth gate, a site: opening one on the Filter at 20 is refused and nothing changes; at 19 its PRE takes a Filter that comes back from a reload at 20") {
	{
		Tower t(kTop);
		const auto before = t.snapshot(kTop);
		CHECK_FALSE(t.at[kTop]->openSite(t.filter[kTop]));
		CHECK_FALSE(t.at[kTop]->hasSite(t.filter[kTop]));
		CHECK(t.snapshot(kTop) == before);
	}
	Tower t(kTop - 1);
	REQUIRE(t.at[kTop - 1]->openSite(t.filter[kTop - 1]));
	t.rig.settle();
	auto* site = static_cast<rack::SiteChains*>(t.at[kTop - 1]->containerSurfaceOf(siteKeyOf(t.filter[kTop - 1])));
	REQUIRE(site != nullptr);
	rack::RackBridge& pre = *site->pre().bridge();
	CHECK(pre.nestingDepth() == kTop);
	const DeviceInstanceId f = pre.addDevice(fx::filter::kTraits.tag);
	REQUIRE(f != kNoInstance);
	t.rig.block();
	pre.setField(f, kCutoff, markCutoff());
	CHECK(t.markedAfterReload() == std::vector<size_t> { kTop });
}

TEST_CASE("Depth gate, Group into Chain and Group into Rack: refused at 20 with the nesting-full message and nothing changed; at 19 the group's Filter comes back from a reload at 20") {
	for (const rack::GroupKind kind : { rack::GroupKind::Chain, rack::GroupKind::Rack }) {
		CAPTURE(static_cast<int>(kind));
		{
			Tower t(kTop);
			ui::EditHistory history;
			ui::FragmentVerbs verbs = verbsOn(*t.at[kTop], history);
			const auto before = t.snapshot(kTop);
			CHECK(rack::groupRefusal(*t.at[kTop], std::vector<DeviceInstanceId> { t.filter[kTop] }) == rack::kNestingFullMessage);
			CHECK(verbs.group(std::vector<DeviceInstanceId> { t.filter[kTop] }, kind).empty());
			CHECK(verbs.lastSaidForTest() == rack::kNestingFullMessage);
			CHECK(t.snapshot(kTop) == before);
		}
		Tower t(kTop - 1);
		ui::EditHistory history;
		ui::FragmentVerbs verbs = verbsOn(*t.at[kTop - 1], history);
		t.at[kTop - 1]->setField(t.filter[kTop - 1], kCutoff, markCutoff());
		REQUIRE(verbs.group(std::vector<DeviceInstanceId> { t.filter[kTop - 1] }, kind).size() == 1);
		t.rig.settle();
		CHECK(markedDepths(*t.rig.top) == std::vector<size_t> { kTop });
		CHECK(t.markedAfterReload() == std::vector<size_t> { kTop });
	}
}

TEST_CASE("Depth gate, Paste: a Chain pasted into the chain at 20 is refused whole with the nesting-full message and nothing changes; at 19 it lands and its Filter comes back from a reload at 20") {
	{
		Tower t(kTop);
		const DeviceInstanceId marked = addMarkedChain(t.rig, *t.rig.top, 0);
		t.rig.verbs->copy(std::vector<DeviceInstanceId> { marked });
		ui::EditHistory history;
		ui::FragmentVerbs verbs = verbsOn(*t.at[kTop], history);
		const auto before = t.snapshot(kTop);
		CHECK(verbs.paste(kNoInstance).empty());
		CHECK(verbs.lastSaidForTest() == rack::kNestingFullMessage);
		CHECK(t.snapshot(kTop) == before);
		// THE UNDO'S ROAD, the put-back under the ids it names, asks the same gate.
		const rack::Fragment lifted = rack::liftFragment(*t.rig.top, std::vector<DeviceInstanceId> { marked });
		const rack::PlaceResult back = rack::placeFragment(*t.at[kTop], lifted, rack::kOriginalPositions, rack::PlaceIds::Keep, true);
		CHECK(back.tooDeep);
		CHECK(back.placed.empty());
		CHECK(t.snapshot(kTop) == before);
	}
	Tower t(kTop - 1);
	const DeviceInstanceId marked = addMarkedChain(t.rig, *t.rig.top, 0);
	t.rig.verbs->copy(std::vector<DeviceInstanceId> { marked });
	t.rig.verbs->remove(std::vector<DeviceInstanceId> { marked });
	t.rig.settle();
	ui::EditHistory history;
	ui::FragmentVerbs verbs = verbsOn(*t.at[kTop - 1], history);
	REQUIRE(verbs.paste(kNoInstance).size() == 1);
	t.rig.settle();
	CHECK(markedDepths(*t.rig.top) == std::vector<size_t> { kTop });
	// AND UNDO, THEN REDO, RE-PLACE IT THROUGH THE SAME GATE — and it still stands at 20.
	history.undo();
	t.rig.settle();
	CHECK(markedDepths(*t.rig.top).empty());
	history.redo();
	t.rig.settle();
	CHECK(markedDepths(*t.rig.top) == std::vector<size_t> { kTop });
	CHECK(t.markedAfterReload() == std::vector<size_t> { kTop });
}

TEST_CASE("Depth gate, a drag into another chain: a Chain dragged into the chain at 20 is refused before anything leaves, said, and nothing changes; into 19 it moves and comes back from a reload at 20") {
	{
		Tower t(kTop);
		const DeviceInstanceId marked = addMarkedChain(t.rig, *t.rig.top, 0);
		const auto before = t.snapshot(kTop);
		CHECK(t.rig.verbs->moveInto(*t.at[kTop], std::vector<DeviceInstanceId> { marked }, 0).empty());
		CHECK(t.rig.verbs->lastSaidForTest() == rack::kNestingFullMessage);
		CHECK(t.snapshot(kTop) == before);
		CHECK(markedDepths(*t.rig.top) == std::vector<size_t> { 1 });
	}
	Tower t(kTop - 1);
	const DeviceInstanceId marked = addMarkedChain(t.rig, *t.rig.top, 0);
	REQUIRE(t.rig.verbs->moveInto(*t.at[kTop - 1], std::vector<DeviceInstanceId> { marked }, 0).size() == 1);
	t.rig.settle();
	CHECK(markedDepths(*t.rig.top) == std::vector<size_t> { kTop });
	CHECK(t.markedAfterReload() == std::vector<size_t> { kTop });
}

TEST_CASE("Depth gate, presets: a device preset with a site, a Chain preset, a Rack preset and a chain loaded whole — each refused where its deepest chain would stand at 21, and nothing changes; each loaded where it would stand at 20, and comes back from a reload there") {
	// THE BYTES, captured at the top where there is room: a Filter whose site's PRE holds the marked Filter (1 tall),
	// a Chain holding a Chain holding it (2 tall), and a Rack whose chain holds that (2 tall).
	std::vector<uint8_t> sitePreset;
	std::vector<uint8_t> chainPreset;
	std::vector<uint8_t> rackPreset;
	{
		Rig src;
		const DeviceInstanceId withSite = src.add(fx::filter::kTraits.tag);
		REQUIRE(src.top->openSite(withSite));
		src.settle();
		auto* site = static_cast<rack::SiteChains*>(src.top->containerSurfaceOf(siteKeyOf(withSite)));
		REQUIRE(site != nullptr);
		const DeviceInstanceId f = site->pre().bridge()->addDevice(fx::filter::kTraits.tag);
		REQUIRE(f != kNoInstance);
		src.block();
		site->pre().bridge()->setField(f, kCutoff, markCutoff());
		src.settle();
		REQUIRE(captureDevicePreset(*src.top, fx::filter::kDescriptor, withSite, &sitePreset));
		const DeviceInstanceId twoTall = addMarkedChain(src, *src.top, 1);
		REQUIRE(captureContainerPreset(*src.top, chainPresetDescriptor(), twoTall, &chainPreset));
		const DeviceInstanceId rackId = src.add(rack::parallel::kTraits.tag);
		src.settle();
		rack::parallel::Rack* rk = rack::parallel::rackOf(src.top->containerSurfaceOf(rackId));
		REQUIRE(rk != nullptr);
		if (rk->chainCount() == 0) {
			(void)rk->addChain();
		}
		src.settle();
		(void)addMarkedChain(src, *rk->chainAt(0)->bridge(), 0);
		REQUIRE(captureContainerPreset(*src.top, rackPresetDescriptor(), rackId, &rackPreset));
	}

	SUBCASE("a device preset carrying a site") {
		{
			Tower t(kTop);
			const auto before = t.snapshot(kTop);
			CHECK(applyDevicePreset(*t.at[kTop], fx::filter::kDescriptor, t.filter[kTop], sitePreset) == DevicePresetLoad::TooDeep);
			CHECK(t.snapshot(kTop) == before);
		}
		Tower t(kTop - 1);
		REQUIRE(applyDevicePreset(*t.at[kTop - 1], fx::filter::kDescriptor, t.filter[kTop - 1], sitePreset) == DevicePresetLoad::Loaded);
		t.rig.settle();
		CHECK(t.markedAfterReload() == std::vector<size_t> { kTop });
	}
	SUBCASE("a Chain preset onto a Chain") {
		{
			Tower t(kTop);
			const auto before = t.snapshot(kTop - 1);
			CHECK(applyDevicePreset(*t.at[kTop - 1], chainPresetDescriptor(), t.chain[kTop], chainPreset) == DevicePresetLoad::TooDeep);
			CHECK(t.snapshot(kTop - 1) == before);
		}
		Tower t(kTop - 1);
		REQUIRE(applyDevicePreset(*t.at[kTop - 2], chainPresetDescriptor(), t.chain[kTop - 1], chainPreset) == DevicePresetLoad::Loaded);
		t.rig.settle();
		CHECK(t.markedAfterReload() == std::vector<size_t> { kTop });
	}
	SUBCASE("a Rack preset onto a Rack") {
		{
			Tower t(kTop);
			const DeviceInstanceId rackId = t.at[kTop - 1]->addDevice(rack::parallel::kTraits.tag);
			REQUIRE(rackId != kNoInstance);
			const auto before = t.snapshot(kTop - 1);
			CHECK(applyDevicePreset(*t.at[kTop - 1], rackPresetDescriptor(), rackId, rackPreset) == DevicePresetLoad::TooDeep);
			CHECK(t.snapshot(kTop - 1) == before);
		}
		Tower t(kTop - 1);
		const DeviceInstanceId rackId = t.at[kTop - 2]->addDevice(rack::parallel::kTraits.tag);
		REQUIRE(rackId != kNoInstance);
		t.rig.settle();
		REQUIRE(applyDevicePreset(*t.at[kTop - 2], rackPresetDescriptor(), rackId, rackPreset) == DevicePresetLoad::Loaded);
		t.rig.settle();
		CHECK(t.markedAfterReload() == std::vector<size_t> { kTop });
	}
	SUBCASE("a chain preset loaded whole into a Chain's own chain") {
		{
			Tower t(kTop);
			const auto before = t.snapshot(kTop);
			CHECK(applyTopPreset(*t.at[kTop], chainPreset) == DevicePresetLoad::TooDeep);
			CHECK(t.snapshot(kTop) == before);
		}
		Tower t(kTop - 1);
		REQUIRE(applyTopPreset(*t.at[kTop - 1], chainPreset) == DevicePresetLoad::Loaded);
		t.rig.settle();
		CHECK(t.markedAfterReload() == std::vector<size_t> { kTop });
	}
}
