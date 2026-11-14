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
#include "mod/lfo/lfo_types.h"
#include "mod/stepseq/stepseq_types.h"
#include "hostslots/slot_bus.h"
#include "plugin/chain_preset.h"
#include "plugin/device_preset.h"
#include "plugin/device_registry.h"
#include "plugin/preset_store.h"
#include "plugin/ui/container_presets.h"
#include "rack/inner_chain.h"
#include "rack/parallel_rack.h"
#include "rack/rack_axes.h"
#include "rack/rack_bridge.h"
#include "rack/rack_device.h"
#include "rack/rack_engine.h"
#include "rack/rack_state.h"
#include "rack/site_chains.h"
#include "rack/fragment.h"
#include "fx/busutil/busutil_types.h"
#include "mod/macros/macros_types.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// BACKLOG OU — presets for Chains and Racks, and for the rack plugin, BY VALUE: every claim reads a row, a route's
// amount, a chain's devices or a browser's entries, never a rendered buffer.

using namespace feathers;
using backplane::Destination;
using backplane::Source;
using rack::RackEngine;
namespace pr = rack::parallel;
namespace sqn = mod::stepseq;
namespace fs = std::filesystem;

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
const size_t kChainMix = keyIndex(chainPresetDescriptor(), "chain_mix");
const size_t kChainMacro1 = keyIndex(chainPresetDescriptor(), "macro_1");
const size_t kRackMode = static_cast<size_t>(pr::Param::Mode);
const size_t kRackMacro1 = static_cast<size_t>(pr::Param::Macro1);
const char* const kChainTag = composite::kTraits.tag;
const char* const kRackTag = pr::kTraits.tag;

/// THE STANDALONE CHAIN PLUGIN'S SHAPE (test_group_copy's): a top bridge owning its host slot pool, an engine a block
/// runs on. A Chain or a Rack sits in it as a device.
struct Rig {
	RackEngine engine { rack::kRackAxes, hostCatalogue() };
	std::unique_ptr<rack::RackBridge> top;
	std::vector<float> l = std::vector<float>(64, 0.0f);
	std::vector<float> r = std::vector<float>(64, 0.0f);

	Rig() {
		engine.prepare(48000.0, 4096);
		top = std::make_unique<rack::RackBridge>(engine, nullptr, rack::RackBridge::HostSlots::OwnPool);
	}
	DeviceInstanceId add(const char* tag) {
		const DeviceInstanceId id = top->addDevice(tag);
		REQUIRE(id != kNoInstance);
		block();
		return id;
	}
	rack::RackBridge& inner(DeviceInstanceId chain) {
		rack::RackBridge* b = rack::bridgeOf(top->containerSurfaceOf(chain));
		REQUIRE(b != nullptr);
		return *b;
	}
	pr::Rack& rackOf(DeviceInstanceId id) {
		pr::Rack* rk = pr::rackOf(top->containerSurfaceOf(id));
		REQUIRE(rk != nullptr);
		return *rk;
	}
	rack::RackBridge& chainOf(DeviceInstanceId rackId, size_t index) {
		rack::InnerChain* c = rackOf(rackId).chainAt(index);
		REQUIRE(c != nullptr);
		REQUIRE(c->bridge() != nullptr);
		return *c->bridge();
	}
	void block() {
		top->drain();
		std::fill(l.begin(), l.end(), 0.1f);
		std::fill(r.begin(), r.end(), 0.1f);
		engine.processStereo(l.data(), r.data(), 64, backplane::unknownBlock());
		top->publish(engine);
		top->reapRetired();
	}
	void settle(int blocks = 4) {
		for (int b = 0; b < blocks; ++b) {
			block();
		}
	}
};

/// THE TAGS OF A CHAIN'S DEVICES, in chain order.
std::vector<std::string> tagsOf(const rack::RackBridge& b) {
	std::vector<std::string> out;
	for (size_t p = 0; p < b.chainLength(); ++p) {
		const device::Descriptor* d = b.descriptorOf(b.deviceAtPosition(p));
		out.emplace_back(d != nullptr ? d->traits.tag : "?");
	}
	return out;
}

/// The chain's document inside a preset, parsed — what was SAVED, not what a load made of it.
rack::RackPatch bodyOfPreset(const std::vector<uint8_t>& bytes) {
	DevicePresetContents contents;
	REQUIRE(readDevicePreset(bytes, &contents));
	rack::RackPatch doc;
	REQUIRE(rack::parseRackText(std::string_view(reinterpret_cast<const char*>(contents.body.data()), contents.body.size()),
	                            &doc));
	return doc;
}

/// THE SAMPLE CHAIN: an LFO into a Filter's cutoff, a macro into its resonance, a named macro, the Chain at 60% Mix with
/// Macro 1 at 0.7. Returns (lfo, filter).
std::pair<DeviceInstanceId, DeviceInstanceId> fillChain(Rig& rig, DeviceInstanceId chain) {
	rack::RackBridge& in = rig.inner(chain);
	const DeviceInstanceId lfo = in.addDevice(mod::lfo::kTraits.tag);
	const DeviceInstanceId filter = in.addDevice(fx::filter::kTraits.tag);
	REQUIRE(lfo != kNoInstance);
	REQUIRE(filter != kNoInstance);
	in.setField(filter, kRes, 0.73f);
	in.setField(filter, kCutoff, 0.41f);
	in.setRouteAmount(Source::output(lfo, 0), Destination::control(filter, kCutoff), 0.5f);
	in.setRouteAmount(Source::core(0), Destination::control(filter, kRes), 0.25f);
	in.setMacroName(0, "Wobble");
	in.setOutputLabel(lfo, 0, "Swing");
	rig.top->setField(chain, kChainMix, 0.6f);
	rig.top->setField(chain, kChainMacro1, 0.7f);
	rig.settle();
	return { lfo, filter };
}

/// A PRESET ROOT OF THIS CASE'S OWN, never a developer's library.
struct PresetRoot {
	fs::path dir;
	std::string was;
	bool had = false;
	PresetRoot() {
		dir = fs::temp_directory_path() / ("feathers-ou-" + std::to_string(::getpid()) + "-"
		                                   + std::to_string(reinterpret_cast<uintptr_t>(this)));
		fs::remove_all(dir);
		fs::create_directories(dir);
		if (const char* existing = std::getenv("FEATHERS_PRESET_ROOT"); existing != nullptr) {
			had = true;
			was = existing;
		}
		::setenv("FEATHERS_PRESET_ROOT", dir.c_str(), 1);
	}
	~PresetRoot() {
		// PUT BACK WHAT CTEST GAVE THE PROCESS, so no later case reaches a developer's real library.
		if (had) {
			::setenv("FEATHERS_PRESET_ROOT", was.c_str(), 1);
		}
		else {
			::unsetenv("FEATHERS_PRESET_ROOT");
		}
		std::error_code ec;
		fs::remove_all(dir, ec);
	}
};

std::vector<std::string> namesOf(const ui::PresetBrowser& b) {
	std::vector<std::string> out;
	for (size_t i = 0; i < b.count(); ++i) {
		out.push_back(b.entry(i).name);
	}
	std::sort(out.begin(), out.end());
	return out;
}

} // namespace

TEST_CASE("OU: a chain preset saved and loaded into a fresh Chain gives the same devices, states, inner routes and "
          "macros, value by value") {
	Rig a;
	const DeviceInstanceId chain = a.add(kChainTag);
	const auto [lfo, filter] = fillChain(a, chain);
	std::vector<uint8_t> bytes;
	REQUIRE(captureContainerPreset(*a.top, chainPresetDescriptor(), chain, &bytes));
	CHECK(isPresetOf(chainPresetDescriptor(), bytes));
	CHECK_FALSE(isPresetOf(rackPresetDescriptor(), bytes));

	Rig b;
	const DeviceInstanceId fresh = b.add(kChainTag);
	REQUIRE(applyDevicePreset(*b.top, chainPresetDescriptor(), fresh, bytes) == DevicePresetLoad::Loaded);
	b.settle();
	rack::RackBridge& in = b.inner(fresh);
	REQUIRE(tagsOf(in) == std::vector<std::string> { mod::lfo::kTraits.tag, fx::filter::kTraits.tag });
	const DeviceInstanceId lfo2 = in.deviceAtPosition(0);
	const DeviceInstanceId filter2 = in.deviceAtPosition(1);
	// EVERY CONTROL OF EVERY DEVICE, as the source held it.
	for (size_t c = 0; c < fx::filter::kDescriptor.params.size(); ++c) {
		CAPTURE(c);
		CHECK(in.fieldAt(filter2, c) == a.inner(chain).fieldAt(filter, c));
	}
	for (size_t c = 0; c < mod::lfo::kDescriptor.params.size(); ++c) {
		CAPTURE(c);
		CHECK(in.fieldAt(lfo2, c) == a.inner(chain).fieldAt(lfo, c));
	}
	CHECK(in.fieldAt(filter2, kRes) == doctest::Approx(0.73f));
	// THE INNER ROUTES, from a device and from a macro.
	CHECK(in.routeAmount(Source::output(lfo2, 0), Destination::control(filter2, kCutoff)) == doctest::Approx(0.5f));
	CHECK(in.routeAmount(Source::core(0), Destination::control(filter2, kRes)) == doctest::Approx(0.25f));
	// THE MACROS: the value and the Mix are the Chain's row; the name is the chain's.
	CHECK(b.top->fieldAt(fresh, kChainMacro1) == doctest::Approx(0.7f));
	CHECK(b.top->fieldAt(fresh, kChainMix) == doctest::Approx(0.6f));
	CHECK(in.macroName(0) == "Wobble");
	CHECK(in.outputLabel(lfo2, 0) == "Swing");
	// THE BYPASS IS THE SESSION'S, as for every device preset.
	CHECK(b.top->fieldAt(fresh, static_cast<size_t>(composite::Param::Enabled)) == 1.0f);
}

TEST_CASE("OU: a rack preset round-trips its mode, roles and chains") {
	Rig a;
	const DeviceInstanceId rackId = a.add(kRackTag);
	pr::Rack& rk = a.rackOf(rackId);
	REQUIRE(rk.addChain() == 1);
	a.settle();
	const DeviceInstanceId filter = a.chainOf(rackId, 0).addDevice(fx::filter::kTraits.tag);
	const DeviceInstanceId delay = a.chainOf(rackId, 1).addDevice(fx::delay::kTraits.tag);
	REQUIRE(filter != kNoInstance);
	REQUIRE(delay != kNoInstance);
	a.chainOf(rackId, 0).setField(filter, kRes, 0.2f);
	a.chainOf(rackId, 1).setField(delay, kDelayMix, 0.8f);
	rk.setLevel(1, 0.3f);
	rk.setMuted(0, true);
	a.top->setField(rackId, kRackMode, static_cast<float>(pr::Mode::MidSide));
	a.top->setField(rackId, kRackMacro1, 0.45f);
	a.settle();
	std::vector<uint8_t> bytes;
	REQUIRE(captureContainerPreset(*a.top, rackPresetDescriptor(), rackId, &bytes));
	CHECK(isPresetOf(rackPresetDescriptor(), bytes));

	Rig b;
	const DeviceInstanceId fresh = b.add(kRackTag);
	REQUIRE(b.rackOf(fresh).chainCount() == 1); // a fresh rack's identity, before the load
	REQUIRE(applyDevicePreset(*b.top, rackPresetDescriptor(), fresh, bytes) == DevicePresetLoad::Loaded);
	b.settle();
	pr::Rack& got = b.rackOf(fresh);
	// THE MODE, AND THE ROLES IT GIVES THE CHAINS IN THEIR ORDER: Mid is chain 1 (the Filter), Side chain 2 (the Delay).
	CHECK(b.top->fieldAt(fresh, kRackMode) == static_cast<float>(pr::Mode::MidSide));
	CHECK(b.top->fieldAt(fresh, kRackMacro1) == doctest::Approx(0.45f));
	REQUIRE(got.chainCount() == 2);
	CHECK(pr::roleCount(pr::Mode::MidSide, got.chainCount()) == 2);
	CHECK(got.chainIdAt(0) == rk.chainIdAt(0));
	CHECK(got.chainIdAt(1) == rk.chainIdAt(1));
	CHECK(tagsOf(b.chainOf(fresh, 0)) == std::vector<std::string> { fx::filter::kTraits.tag });
	CHECK(tagsOf(b.chainOf(fresh, 1)) == std::vector<std::string> { fx::delay::kTraits.tag });
	CHECK(b.chainOf(fresh, 0).fieldAt(b.chainOf(fresh, 0).deviceAtPosition(0), kRes) == doctest::Approx(0.2f));
	CHECK(b.chainOf(fresh, 1).fieldAt(b.chainOf(fresh, 1).deviceAtPosition(0), kDelayMix) == doctest::Approx(0.8f));
	// EACH CHAIN'S OWN STRIP.
	CHECK(got.level(1) == doctest::Approx(0.3f));
	CHECK(got.muted(0));
	CHECK_FALSE(got.muted(1));
	// AND THE AUDIO THREAD RUNS THE NEW LINEUP, not the fresh rack's one chain.
	CHECK(got.liveChainsForTest() == 2);
}

TEST_CASE("OU: an outside route is dropped on save") {
	Rig a;
	const DeviceInstanceId outer = a.add(mod::lfo::kTraits.tag);
	const DeviceInstanceId chain = a.add(kChainTag);
	const DeviceInstanceId tail = a.add(fx::filter::kTraits.tag);
	const auto [lfo, filter] = fillChain(a, chain);
	// IN FROM OUTSIDE: the top's LFO into the chain's Filter. OUT TO OUTSIDE: the chain's LFO, exported, into the tail.
	REQUIRE(a.top->setInnerRouteAmount(Source::output(outer, 0), chain, Destination::control(filter, kRes), 0.4f));
	const Source exported = a.top->containerExport(chain, Source::output(lfo, 0));
	REQUIRE(exported.isExport());
	a.top->setRouteAmount(exported, Destination::control(tail, kCutoff), 0.6f);
	a.settle();
	// NOT VACUOUS: the live chain holds both halves before the save.
	REQUIRE(a.top->innerRouteAmount(Source::output(outer, 0), chain, Destination::control(filter, kRes))
	        == doctest::Approx(0.4f));
	REQUIRE(a.top->routeAmount(exported, Destination::control(tail, kCutoff)) == doctest::Approx(0.6f));

	std::vector<uint8_t> bytes;
	size_t dropped = 0;
	REQUIRE(captureContainerPreset(*a.top, chainPresetDescriptor(), chain, &bytes, &dropped));
	CHECK(dropped == 2); // the inbound route and the export row
	const rack::RackPatch saved = bodyOfPreset(bytes);
	CHECK(saved.exports.empty());
	size_t outerRoutes = 0;
	for (const rack::RackPatch::Route& route : saved.route) {
		outerRoutes += route.source.kind == static_cast<uint32_t>(rack::SourceKind::Outer) ? 1 : 0;
	}
	CHECK(outerRoutes == 0);
	CHECK(saved.route.size() == 2); // the two routes AMONG the chain's own stay

	// THE SAVE TOOK NOTHING FROM THE LIVE CHAIN.
	CHECK(a.top->innerRouteAmount(Source::output(outer, 0), chain, Destination::control(filter, kRes))
	      == doctest::Approx(0.4f));
	CHECK(a.top->routeAmount(exported, Destination::control(tail, kCutoff)) == doctest::Approx(0.6f));

	// AND A LOAD ELSEWHERE BRINGS NONE OF IT: a fresh rig with an LFO at the same id reads no route from it.
	Rig b;
	const DeviceInstanceId outer2 = b.add(mod::lfo::kTraits.tag);
	REQUIRE(outer2 == outer);
	const DeviceInstanceId fresh = b.add(kChainTag);
	REQUIRE(applyDevicePreset(*b.top, chainPresetDescriptor(), fresh, bytes) == DevicePresetLoad::Loaded);
	b.settle();
	CHECK(b.top->innerRouteAmount(Source::output(outer2, 0), fresh,
	                              Destination::control(b.inner(fresh).deviceAtPosition(1), kRes))
	      == 0.0f);
}

TEST_CASE("OU: a chain preset loaded through a rack's chain fills only that chain and leaves the rack's mode and roles alone") {
	// THE INTENDED ROAD (2026-10-29: add a Chain and use ITS browser): a rail has no browser of its own. A Chain
	// device added to one of the Rack's chains takes the preset through ITS browser, the one every Chain has.
	PresetRoot root;
	Rig source;
	const DeviceInstanceId chain = source.add(kChainTag);
	(void)fillChain(source, chain);
	std::vector<uint8_t> bytes;
	REQUIRE(captureContainerPreset(*source.top, chainPresetDescriptor(), chain, &bytes));
	REQUIRE(PresetStore::save("Lead Chain", bytes, kChainPresetBank));

	Rig rig;
	const DeviceInstanceId rackId = rig.add(kRackTag);
	pr::Rack& rk = rig.rackOf(rackId);
	REQUIRE(rk.addChain() == 1);
	rig.settle();
	const DeviceInstanceId delay = rig.chainOf(rackId, 0).addDevice(fx::delay::kTraits.tag);
	rig.chainOf(rackId, 0).setField(delay, kDelayMix, 0.8f);
	rk.setLevel(1, 0.3f);
	rig.top->setField(rackId, kRackMode, static_cast<float>(pr::Mode::MidSide));
	rig.top->setField(rackId, kRackMacro1, 0.2f);
	rack::RackBridge& side = rig.chainOf(rackId, 1);
	const DeviceInstanceId added = side.addDevice(kChainTag); // the Side role's chain gets a Chain device
	REQUIRE(added != kNoInstance);
	rig.settle();
	const uint16_t first = rk.chainIdAt(0);
	const uint16_t second = rk.chainIdAt(1);

	// ITS OWN BROWSER — the Chain device's, over the Chains bank — and a pick in it.
	ui::ContainerPresets presets = ui::makeChainDevicePresets(side, added, nullptr);
	const int at = presets.browser->indexOf("Lead Chain");
	REQUIRE(at >= 0);
	REQUIRE(presets.browser->selectIndex(at));
	rig.settle();

	// THAT CHAIN HOLDS THE PRESET: the Chain device it was given, and inside it the devices, states and routes.
	REQUIRE(side.chainLength() == 1);
	REQUIRE(side.deviceAtPosition(0) == added);
	rack::RackBridge* inner = rack::bridgeOf(side.containerSurfaceOf(added));
	REQUIRE(inner != nullptr);
	REQUIRE(tagsOf(*inner) == std::vector<std::string> { mod::lfo::kTraits.tag, fx::filter::kTraits.tag });
	const DeviceInstanceId lfo2 = inner->deviceAtPosition(0);
	const DeviceInstanceId filter2 = inner->deviceAtPosition(1);
	CHECK(inner->fieldAt(filter2, kRes) == doctest::Approx(0.73f));
	CHECK(inner->routeAmount(Source::output(lfo2, 0), Destination::control(filter2, kCutoff)) == doctest::Approx(0.5f));
	CHECK(side.fieldAt(added, kChainMacro1) == doctest::Approx(0.7f));
	// AND NOTHING ELSE MOVED: the Rack's mode and the roles it gives, its macro, the chains, their order, the other chain.
	CHECK(rig.top->fieldAt(rackId, kRackMode) == static_cast<float>(pr::Mode::MidSide));
	CHECK(rig.top->fieldAt(rackId, kRackMacro1) == doctest::Approx(0.2f));
	REQUIRE(rk.chainCount() == 2);
	CHECK(pr::roleCount(pr::Mode::MidSide, rk.chainCount()) == 2);
	CHECK(rk.chainIdAt(0) == first);
	CHECK(rk.chainIdAt(1) == second);
	CHECK(rk.level(1) == doctest::Approx(0.3f));
	CHECK(tagsOf(rig.chainOf(rackId, 0)) == std::vector<std::string> { fx::delay::kTraits.tag });
	CHECK(rig.chainOf(rackId, 0).fieldAt(delay, kDelayMix) == doctest::Approx(0.8f));
	const device::Descriptor* still = rig.top->descriptorOf(rackId);
	REQUIRE(still != nullptr);
	CHECK(device::sameTag(still->traits.tag, kRackTag));
}

TEST_CASE("OU: each browser lists only its own kind") {
	PresetRoot root;
	Rig rig;
	const DeviceInstanceId chain = rig.add(kChainTag);
	const DeviceInstanceId rackId = rig.add(kRackTag);
	(void)fillChain(rig, chain);
	std::vector<uint8_t> chainBytes;
	std::vector<uint8_t> rackBytes;
	REQUIRE(captureContainerPreset(*rig.top, chainPresetDescriptor(), chain, &chainBytes));
	REQUIRE(captureContainerPreset(*rig.top, rackPresetDescriptor(), rackId, &rackBytes));
	REQUIRE(PresetStore::save("Lead Chain", chainBytes, kChainPresetBank));
	REQUIRE(PresetStore::save("Wide Rack", rackBytes, kRackPresetBank));
	// ANOTHER KIND'S FILE IN EACH FOLDER, as a hand moving files would leave it.
	REQUIRE(PresetStore::save("Stray Rack", rackBytes, kChainPresetBank));
	REQUIRE(PresetStore::save("Stray Chain", chainBytes, kRackPresetBank));

	ui::ContainerPresets chainDevice = ui::makeChainDevicePresets(*rig.top, chain, nullptr);
	ui::ContainerPresets rackDevice = ui::makeRackDevicePresets(*rig.top, rackId, nullptr);
	ui::ContainerPresets top = ui::makeTopPresets(*rig.top, nullptr);
	const std::vector<std::string> chains { "Init", "Lead Chain" };
	const std::vector<std::string> racks { "Init", "Wide Rack" };
	CHECK(chainDevice.browser->bank() == kChainPresetBank);
	CHECK(rackDevice.browser->bank() == kRackPresetBank);
	CHECK(namesOf(*chainDevice.browser) == chains);
	CHECK(namesOf(*top.browser) == chains);
	CHECK(namesOf(*rackDevice.browser) == racks);
	// EVERY ENTRY OFFERED IS ITS BROWSER'S KIND — Init included.
	for (const ui::ContainerPresets* p : { &chainDevice, &top }) {
		for (size_t i = 0; i < p->browser->count(); ++i) {
			CHECK(isPresetOf(chainPresetDescriptor(), p->browser->entry(i).data.bytes));
		}
	}
	for (size_t i = 0; i < rackDevice.browser->count(); ++i) {
		CHECK(isPresetOf(rackPresetDescriptor(), rackDevice.browser->entry(i).data.bytes));
	}
	// AND A FILE OF THE OTHER KIND, handed over anyway, is refused on every road: a Rack never turns into a Chain.
	CHECK(applyDevicePreset(*rig.top, rackPresetDescriptor(), rackId, chainBytes) == DevicePresetLoad::OtherDevice);
	CHECK(applyDevicePreset(*rig.top, chainPresetDescriptor(), chain, rackBytes) == DevicePresetLoad::OtherDevice);
	CHECK(applyTopPreset(*rig.top, rackBytes) == DevicePresetLoad::OtherDevice);
	const device::Descriptor* still = rig.top->descriptorOf(rackId);
	REQUIRE(still != nullptr);
	CHECK(device::sameTag(still->traits.tag, kRackTag));
}

namespace {

/// THE INNER SEQUENCER, as test_rack_nesting's MX case seats it: free, 8 steps a second, Turing at Lock 0, a ramp.
void seatTuring(rack::RackBridge& inner, DeviceInstanceId id) {
	inner.setField(id, sqn::at(sqn::Param::Retrig), 0.0f);
	inner.setField(id, sqn::at(sqn::Param::TimeBase), 0.0f);
	inner.setField(id, sqn::at(sqn::Param::Rate), 3.0f);
	inner.setField(id, sqn::at(sqn::Param::Turing), 1.0f);
	inner.setField(id, sqn::at(sqn::Param::TuringLock), 0.0f);
	for (size_t s = 0; s < 16; ++s) {
		inner.setField(id, sqn::valueIndex(s), static_cast<float>(s) / 16.0f);
	}
}

/// The running inner device's register — what it would publish now, step values only.
std::vector<float> registerOf(rack::RackEngine& innerEngine, DeviceInstanceId id) {
	std::array<float, device::kMaxExtraStateFloats> extra {};
	size_t n = 0;
	(void)innerEngine.extraStateAt(innerEngine.slotOf(id), extra.data(), extra.size(), &n);
	std::vector<float> out;
	for (size_t s = 0; s < sqn::kMaxSteps && s * sqn::kStepFloats < n; ++s) {
		out.push_back(extra[s * sqn::kStepFloats]);
	}
	return out;
}

std::vector<float> recordedSteps(const rack::RackBridge& b, DeviceInstanceId id) {
	std::vector<float> out;
	for (size_t s = 0; s < sqn::kMaxSteps; ++s) {
		out.push_back(b.fieldAt(id, sqn::valueIndex(s)));
	}
	return out;
}

} // namespace

TEST_CASE("OU: a random or captured state inside a chain survives the round trip") {
	// A TURING REWRITE — the register a machine reached by chance, published from the audio thread (backlog MX) — inside
	// a Chain, saved as a chain preset with nothing harvested by hand, loaded into a fresh Chain. An LFO outside routes
	// into the chain, so the capture REWRITES the chain's document (the route is dropped) and the register must survive
	// that too, not only a copy of the bytes.
	Rig a;
	const DeviceInstanceId outer = a.add(mod::lfo::kTraits.tag);
	const DeviceInstanceId chain = a.add(kChainTag);
	rack::RackBridge& in = a.inner(chain);
	RackEngine* innerEngine = rack::engineOf(a.top->containerSurfaceOf(chain));
	REQUIRE(innerEngine != nullptr);
	const DeviceInstanceId seq = in.addDevice(sqn::kTraits.tag);
	REQUIRE(seq != kNoInstance);
	seatTuring(in, seq);
	REQUIRE(a.top->setInnerRouteAmount(Source::output(outer, 0), chain,
	                                   Destination::control(seq, sqn::at(sqn::Param::TuringLock)), 0.01f));
	a.settle(600);
	const std::vector<float> heard = registerOf(*innerEngine, seq);
	bool rewrote = false;
	for (size_t s = 0; s < 16; ++s) {
		rewrote = rewrote || heard[s] != static_cast<float>(s) / 16.0f;
	}
	REQUIRE(rewrote); // NOT VACUOUS: the machine has moved off the hand's ramp

	std::vector<uint8_t> bytes;
	size_t dropped = 0;
	REQUIRE(captureContainerPreset(*a.top, chainPresetDescriptor(), chain, &bytes, &dropped));
	REQUIRE(dropped == 1); // the document was rewritten
	Rig b;
	const DeviceInstanceId fresh = b.add(kChainTag);
	REQUIRE(applyDevicePreset(*b.top, chainPresetDescriptor(), fresh, bytes) == DevicePresetLoad::Loaded);
	b.block();
	rack::RackBridge& got = b.inner(fresh);
	REQUIRE(got.chainLength() == 1);
	CHECK(recordedSteps(got, got.deviceAtPosition(0)) == heard);
}

TEST_CASE("OU: the rack plugin's top level takes a chain preset as its whole state, and gives one") {
	// THE PLUGIN IS A CHAIN AND EIGHT MACROS: a Chain device's preset replaces everything at the top.
	Rig source;
	const DeviceInstanceId chain = source.add(kChainTag);
	(void)fillChain(source, chain);
	std::vector<uint8_t> bytes;
	REQUIRE(captureContainerPreset(*source.top, chainPresetDescriptor(), chain, &bytes));

	Rig plugin;
	(void)plugin.add(fx::delay::kTraits.tag); // replaced by the load
	(void)plugin.top->takeStateDirty();
	REQUIRE(applyTopPreset(*plugin.top, bytes) == DevicePresetLoad::Loaded);
	plugin.settle();
	REQUIRE(tagsOf(*plugin.top) == std::vector<std::string> { mod::lfo::kTraits.tag, fx::filter::kTraits.tag });
	const DeviceInstanceId lfo2 = plugin.top->deviceAtPosition(0);
	const DeviceInstanceId filter2 = plugin.top->deviceAtPosition(1);
	CHECK(plugin.top->fieldAt(filter2, kRes) == doctest::Approx(0.73f));
	CHECK(plugin.top->routeAmount(Source::output(lfo2, 0), Destination::control(filter2, kCutoff)) == doctest::Approx(0.5f));
	CHECK(plugin.top->macro(0) == doctest::Approx(0.7f)); // the Chain's Macro 1 is the plugin's
	CHECK(plugin.top->macroName(0) == "Wobble");
	CHECK(plugin.top->takeStateDirty()); // a preset picked by hand is an edit of the project
	// THE MACRO WENT TO THE HOST AS A VALUE, as a hand's turn of the knob does.
	REQUIRE(plugin.top->macroOutbox() != nullptr);
	float told = -1.0f;
	plugin.top->macroOutbox()->drain([&](hostslots::SlotEventKind kind, uint16_t slot, float value01) {
		if (kind == hostslots::SlotEventKind::Value && slot == 0) {
			told = value01;
		}
		return true;
	});
	CHECK(told == doctest::Approx(0.7f));

	// AND BACK OUT: the plugin's state as a chain preset, into a Chain device.
	std::vector<uint8_t> out;
	REQUIRE(captureTopPreset(*plugin.top, &out));
	CHECK(isPresetOf(chainPresetDescriptor(), out));
	CHECK_FALSE(bodyOfPreset(out).hostSlots.has_value());
	Rig back;
	const DeviceInstanceId into = back.add(kChainTag);
	REQUIRE(applyDevicePreset(*back.top, chainPresetDescriptor(), into, out) == DevicePresetLoad::Loaded);
	back.settle();
	CHECK(tagsOf(back.inner(into)) == std::vector<std::string> { mod::lfo::kTraits.tag, fx::filter::kTraits.tag });
	CHECK(back.top->fieldAt(into, kChainMacro1) == doctest::Approx(0.7f));
}

//------------------------------------------------------------------------------------------------//
// A DEVICE PRESET CARRIES ITS SITE — its PRE and POST chains, as a Rack or a Chain preset carries its                //
// inner chains: the site's bytes, and PRE's routes into the device's own controls. Every claim a value.                  //
//------------------------------------------------------------------------------------------------//

namespace {

constexpr float kPreValue = 0.5f;
constexpr float kRouteAmount = 0.25f;
const size_t kInvertL = static_cast<size_t>(fx::busutil::Param::InvertL);

rack::SiteChains& siteOf(Rig& rig, DeviceInstanceId id) {
	auto* site = static_cast<rack::SiteChains*>(rig.top->containerSurfaceOf(siteKeyOf(id)));
	REQUIRE(site != nullptr);
	return *site;
}

float resolved(const Rig& rig, DeviceInstanceId id, size_t control) {
	const size_t at = rig.engine.slotOf(id);
	REQUIRE(at != RackEngine::kNoSlot);
	return rig.engine.resolvedFieldForTest(at, control);
}

/// A DEVICE WITH A SITE: a Macros at `kPreValue` in PRE routed into `control` of the device at `kRouteAmount`, and a Bus
/// Util inverting the left in POST. The PRE Macros's id back through `macros`.
void furnish(Rig& rig, DeviceInstanceId id, size_t control, DeviceInstanceId* macros = nullptr) {
	REQUIRE(rig.top->openSite(id));
	rig.settle(2);
	rack::SiteChains& site = siteOf(rig, id);
	const DeviceInstanceId m = site.pre().bridge()->addDevice(mod::macros::kTraits.tag);
	const DeviceInstanceId bus = site.post().bridge()->addDevice(fx::busutil::kTraits.tag);
	REQUIRE(m != kNoInstance);
	REQUIRE(bus != kNoInstance);
	site.pre().bridge()->setField(m, 0, kPreValue);
	site.post().bridge()->setField(bus, kInvertL, 1.0f);
	rig.settle(2);
	const Source out = rig.top->siteExport(id, Source::output(m, 0));
	REQUIRE(out.isSiteExport());
	REQUIRE(rig.top->mayRoute(out, Destination::control(id, control)));
	rig.top->setRouteAmount(out, Destination::control(id, control), kRouteAmount);
	rig.settle(4);
	if (macros != nullptr) {
		*macros = m;
	}
}

/// What the device's PRE and POST hold, read off the running site: PRE's Macros value and POST's Bus Util Invert L.
void checkFurnished(Rig& rig, DeviceInstanceId id) {
	rack::SiteChains& site = siteOf(rig, id);
	REQUIRE(site.pre().bridge()->chainLength() == 1);
	REQUIRE(site.post().bridge()->chainLength() == 1);
	const DeviceInstanceId m = site.pre().bridge()->deviceAtPosition(0);
	const DeviceInstanceId bus = site.post().bridge()->deviceAtPosition(0);
	CHECK(device::sameTag(site.pre().bridge()->descriptorOf(m)->traits.tag, mod::macros::kTraits.tag));
	CHECK(device::sameTag(site.post().bridge()->descriptorOf(bus)->traits.tag, fx::busutil::kTraits.tag));
	CHECK(site.pre().bridge()->fieldAt(m, 0) == kPreValue);
	CHECK(site.post().bridge()->fieldAt(bus, kInvertL) == 1.0f);
}

} // namespace

TEST_CASE("Site presets: a Filter's preset carries its PRE and POST; loaded into a fresh Filter in a fresh host the PRE "
          "modulator still moves its cutoff, by value") {
	std::vector<uint8_t> bytes;
	double want = 0.0;
	{
		Rig rig;
		const DeviceInstanceId filter = rig.add(fx::filter::kTraits.tag);
		const float base = resolved(rig, filter, kCutoff);
		furnish(rig, filter, kCutoff);
		const auto& p = fx::filter::kDescriptor.params[kCutoff];
		want = device::withRouteOffset(p, base, kRouteAmount * kPreValue * device::routeSpanOf(p));
		CHECK(want != doctest::Approx(base));
		CHECK(resolved(rig, filter, kCutoff) == doctest::Approx(want).epsilon(1e-5));
		REQUIRE(captureDevicePreset(*rig.top, fx::filter::kDescriptor, filter, &bytes));
	}
	DevicePresetContents contents;
	REQUIRE(readDevicePreset(bytes, &contents, &fx::filter::kDescriptor));
	REQUIRE(contents.site.has_value());
	REQUIRE(contents.site->routes.size() == 1);
	CHECK(contents.site->routes[0].key == std::string(fx::filter::kDescriptor.params[kCutoff].key));
	CHECK(contents.site->routes[0].amount == kRouteAmount);

	Rig fresh;
	(void)fresh.add(fx::delay::kTraits.tag); // a different id for the Filter than the one it was saved from
	const DeviceInstanceId target = fresh.add(fx::filter::kTraits.tag);
	CHECK_FALSE(fresh.top->hasSite(target));
	REQUIRE(applyDevicePreset(*fresh.top, fx::filter::kDescriptor, target, bytes) == DevicePresetLoad::Loaded);
	fresh.settle(6);
	CHECK(fresh.top->hasSite(target));
	checkFurnished(fresh, target);
	CHECK(resolved(fresh, target, kCutoff) == doctest::Approx(want).epsilon(1e-5));
	// AND THE PRE MODULATOR IS WHAT MOVES IT: its value to 0 returns the cutoff to the preset's own.
	rack::SiteChains& site = siteOf(fresh, target);
	site.pre().bridge()->setField(site.pre().bridge()->deviceAtPosition(0), 0, 0.0f);
	fresh.settle(4);
	CHECK(resolved(fresh, target, kCutoff) == doctest::Approx(fresh.top->fieldAt(target, kCutoff)).epsilon(1e-5));
}

TEST_CASE("Site presets: a preset with NO site leaves the device's site, its chains and its PRE route as they were") {
	Rig rig;
	const DeviceInstanceId filter = rig.add(fx::filter::kTraits.tag);
	const DeviceInstanceId plain = rig.add(fx::filter::kTraits.tag);
	rig.top->setField(plain, kRes, 0.7f);
	std::vector<uint8_t> old;
	REQUIRE(captureDevicePreset(*rig.top, fx::filter::kDescriptor, plain, &old)); // no site: as every older preset
	DevicePresetContents contents;
	REQUIRE(readDevicePreset(old, &contents, &fx::filter::kDescriptor));
	CHECK_FALSE(contents.site.has_value());
	DeviceInstanceId macros = kNoInstance;
	furnish(rig, filter, kCutoff, &macros);
	const Source out = rig.top->siteExport(filter, Source::output(macros, 0));
	REQUIRE(applyDevicePreset(*rig.top, fx::filter::kDescriptor, filter, old) == DevicePresetLoad::Loaded);
	rig.settle(4);
	CHECK(rig.top->fieldAt(filter, kRes) == doctest::Approx(0.7f)); // the preset loaded
	CHECK(rig.top->hasSite(filter));
	checkFurnished(rig, filter);
	CHECK(rig.top->routeAmount(out, Destination::control(filter, kCutoff)) == kRouteAmount);
}

TEST_CASE("Site presets: a Rack preset and a Chain preset carry the container's OWN site — loaded into fresh ones, the PRE "
          "modulator moves Macro 1, by value") {
	struct Kind {
		const device::Descriptor* desc;
		size_t macro;
	};
	for (const Kind k : { Kind { &rackPresetDescriptor(), kRackMacro1 }, Kind { &chainPresetDescriptor(), kChainMacro1 } }) {
		INFO(k.desc->traits.name);
		std::vector<uint8_t> bytes;
		double want = 0.0;
		{
			Rig rig;
			const DeviceInstanceId id = rig.add(k.desc->traits.tag);
			const float base = resolved(rig, id, k.macro);
			furnish(rig, id, k.macro);
			const auto& p = k.desc->params[k.macro];
			want = device::withRouteOffset(p, base, kRouteAmount * kPreValue * device::routeSpanOf(p));
			CHECK(want != doctest::Approx(base));
			CHECK(resolved(rig, id, k.macro) == doctest::Approx(want).epsilon(1e-6));
			REQUIRE(captureContainerPreset(*rig.top, *k.desc, id, &bytes));
		}
		Rig fresh;
		(void)fresh.add(fx::delay::kTraits.tag);
		const DeviceInstanceId target = fresh.add(k.desc->traits.tag);
		REQUIRE(applyDevicePreset(*fresh.top, *k.desc, target, bytes) == DevicePresetLoad::Loaded);
		fresh.settle(6);
		checkFurnished(fresh, target);
		CHECK(resolved(fresh, target, k.macro) == doctest::Approx(want).epsilon(1e-6));
	}
}

TEST_CASE("Site presets: routes across the site's edge are dropped — an outside LFO into PRE, and PRE into a later device "
          "— while PRE into its own device and POST reading PRE travel") {
	Rig rig;
	const DeviceInstanceId lfo = rig.add(mod::lfo::kTraits.tag);
	const DeviceInstanceId filter = rig.add(fx::filter::kTraits.tag);
	const DeviceInstanceId later = rig.add(fx::filter::kTraits.tag);
	DeviceInstanceId macros = kNoInstance;
	furnish(rig, filter, kCutoff, &macros);
	rack::SiteChains& site = siteOf(rig, filter);
	const DeviceInstanceId preFilter = site.pre().bridge()->addDevice(fx::filter::kTraits.tag);
	const DeviceInstanceId postFilter = site.post().bridge()->addDevice(fx::filter::kTraits.tag);
	REQUIRE(preFilter != kNoInstance);
	REQUIRE(postFilter != kNoInstance);
	rig.settle(2);
	const Source out = rig.top->siteExport(filter, Source::output(macros, 0));
	// THE FOUR ROUTES: outside → PRE, PRE → a later device, PRE → its own device (made by `furnish`), POST ← PRE.
	REQUIRE(site.pre().bridge()->mayRoute(Source::outerOutput(lfo, 0), Destination::control(preFilter, kCutoff)));
	site.pre().bridge()->setRouteAmount(Source::outerOutput(lfo, 0), Destination::control(preFilter, kCutoff), 0.5f);
	REQUIRE(rig.top->mayRoute(out, Destination::control(later, kCutoff)));
	rig.top->setRouteAmount(out, Destination::control(later, kCutoff), 0.5f);
	site.post().bridge()->setRouteAmount(out.asOuter(), Destination::control(postFilter, kCutoff), 0.5f);
	rig.settle(4);
	REQUIRE(site.pre().bridge()->routeAmount(Source::outerOutput(lfo, 0), Destination::control(preFilter, kCutoff)) == 0.5f);
	REQUIRE(site.post().bridge()->routeAmount(out.asOuter(), Destination::control(postFilter, kCutoff)) == 0.5f);
	std::vector<uint8_t> bytes;
	REQUIRE(captureDevicePreset(*rig.top, fx::filter::kDescriptor, filter, &bytes));
	DevicePresetContents contents;
	REQUIRE(readDevicePreset(bytes, &contents, &fx::filter::kDescriptor));
	REQUIRE(contents.site.has_value());
	REQUIRE(contents.site->routes.size() == 1); // PRE → its own cutoff; PRE → the later Filter is not the preset's
	// IN THE SAVED BYTES THEMSELVES — not inferred from a load, whose own gate would refuse a dangling id anyway: the one
	// `Outer` source left is POST's read of PRE, named by the preset's own device.
	std::vector<Source> outers;
	REQUIRE(rack::remapOuterSources(
	            contents.site->body,
	            [](void* ctx, Source s, uint32_t) -> std::optional<Source> {
		            static_cast<std::vector<Source>*>(ctx)->push_back(s);
		            return s;
	            },
	            &outers, nullptr)
	            .has_value());
	REQUIRE(outers.size() == 1);
	CHECK(outers[0].device == kFirstInstance);
	CHECK(outers[0].inner().isSiteExport());

	Rig fresh;
	const DeviceInstanceId target = fresh.add(fx::filter::kTraits.tag);
	REQUIRE(applyDevicePreset(*fresh.top, fx::filter::kDescriptor, target, bytes) == DevicePresetLoad::Loaded);
	fresh.settle(6);
	rack::SiteChains& loaded = siteOf(fresh, target);
	const auto outerRoutes = [](const rack::RackBridge& b) {
		size_t n = 0;
		for (const auto& r : b.patch().route) {
			n += r.source.kind == static_cast<uint32_t>(rack::SourceKind::Outer) ? 1 : 0;
		}
		return n;
	};
	CHECK(outerRoutes(*loaded.pre().bridge()) == 0); // the outside LFO's route went
	REQUIRE(outerRoutes(*loaded.post().bridge()) == 1); // POST reading PRE stayed…
	for (const auto& r : loaded.post().bridge()->patch().route) {
		if (r.source.kind == static_cast<uint32_t>(rack::SourceKind::Outer)) {
			CHECK(r.source.a == target); // …re-pointed at the device it now belongs to
			CHECK(Source { r.source.a, r.source.b }.inner().isSiteExport());
		}
	}
}

TEST_CASE("Any depth, presets: a Chain preset keeps a nested route that names something inside it and drops one that "
          "reaches past it — a Filter two Chains in, reading the outer Chain's Macro 1 (kept) and the top's (dropped)") {
	std::vector<uint8_t> bytes;
	{
		Rig rig;
		const DeviceInstanceId outer = rig.add(kChainTag);
		rack::RackBridge& c = rig.inner(outer);
		const DeviceInstanceId d = c.addDevice(kChainTag);
		REQUIRE(d != kNoInstance);
		rig.settle(2);
		rack::RackBridge* inD = rack::bridgeOf(c.containerSurfaceOf(d));
		REQUIRE(inD != nullptr);
		const DeviceInstanceId filter = inD->addDevice(fx::filter::kTraits.tag);
		REQUIRE(filter != kNoInstance);
		rig.settle(2);
		const Destination dest = Destination::control(filter, kCutoff);
		const Source chainMacro = Source::core(0).asOuter();           // the saved Chain's own Macro 1: inside the preset
		const Source topMacro = Source::core(0).asOuter().asOuter();   // the host's: past the preset
		REQUIRE(inD->mayRoute(chainMacro, dest));
		REQUIRE(inD->mayRoute(topMacro, dest));
		inD->setRouteAmount(chainMacro, dest, 0.25f);
		inD->setRouteAmount(topMacro, dest, 0.5f);
		rig.settle(2);
		size_t dropped = 0;
		REQUIRE(captureContainerPreset(*rig.top, chainPresetDescriptor(), outer, &bytes, &dropped));
		CHECK(dropped == 1);
	}
	Rig fresh;
	const DeviceInstanceId target = fresh.add(kChainTag);
	REQUIRE(applyDevicePreset(*fresh.top, chainPresetDescriptor(), target, bytes) == DevicePresetLoad::Loaded);
	fresh.settle(6);
	rack::RackBridge& c = fresh.inner(target);
	REQUIRE(c.chainLength() == 1);
	rack::RackBridge* inD = rack::bridgeOf(c.containerSurfaceOf(c.deviceAtPosition(0)));
	REQUIRE(inD != nullptr);
	const DeviceInstanceId filter = inD->deviceAtPosition(0);
	const Destination dest = Destination::control(filter, kCutoff);
	CHECK(inD->routeAmount(Source::core(0).asOuter(), dest) == 0.25f);
	CHECK(inD->routeAmount(Source::core(0).asOuter().asOuter(), dest) == 0.0f);
}

namespace {

std::vector<uint8_t> bytesOf(const fs::path& file) {
	std::ifstream in(file, std::ios::binary);
	return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

} // namespace

TEST_CASE("hardening: a preset write that fails leaves the old file byte for byte, and a good write replaces it whole") {
	PresetRoot root;
	const std::vector<uint8_t> first(64, 0x11);
	const std::vector<uint8_t> second(4096, 0x22);
	REQUIRE(PresetStore::save("Keep", first, kChainPresetBank));
	const fs::path file = PresetStore::pathFor("Keep", kChainPresetBank);
	const std::vector<uint8_t> before = bytesOf(file);
	REQUIRE_FALSE(before.empty());

	// THE TEMP'S PLACE IS TAKEN BY A DIRECTORY, so the write cannot open it: the failure a full disk or a lost
	// permission would be, in a form a test can make by value.
	fs::path temp = file;
	temp += ".tmp";
	REQUIRE(fs::create_directory(temp));
	CHECK_FALSE(PresetStore::save("Keep", second, kChainPresetBank));
	CHECK(bytesOf(file) == before);
	REQUIRE(fs::remove(temp));

	// AND ONCE THE WAY IS CLEAR THE NEW BYTES TAKE THE PLACE, with no temp left beside them.
	REQUIRE(PresetStore::save("Keep", second, kChainPresetBank));
	CHECK(bytesOf(file) != before);
	CHECK_FALSE(fs::exists(temp));
	const std::vector<StoredPreset> all = PresetStore::loadAll(kChainPresetBank);
	REQUIRE(all.size() == 1);
	CHECK(all[0].bytes == second);
}

TEST_CASE("hardening: names differing only in an accented letter are two presets, and a non-ASCII name round-trips") {
	PresetRoot root;
	const std::string e = "Caf\xC3\xA9";   // Cafe with e-acute
	const std::string g = "Caf\xC3\xA8";   // Cafe with e-grave
	const std::string cjk = "\xE9\x9F\xB3\xE8\x89\xB2"; // two ideographs, nothing ASCII
	CHECK(PresetStore::sanitise(e) == e);
	CHECK(PresetStore::sanitise(e) != PresetStore::sanitise(g));
	CHECK(PresetStore::sanitise(cjk) == cjk);
	// EXISTING ASCII BEHAVIOUR IS UNCHANGED, and what must never reach a file name still cannot.
	CHECK(PresetStore::sanitise("a/b\\c:d*e?f\"g<h>i|j.k") == "a-b-c-d-e-f-g-h-i-j-k");
	CHECK(PresetStore::sanitise("  Lead 1_x-y  ") == "Lead 1_x-y");
	CHECK(PresetStore::sanitise("ctl\x01\x7F") == "ctl--");
	CHECK(PresetStore::sanitise(std::string("a\xC3") + "b") == "a-b");      // a truncated sequence is one stray byte
	CHECK(PresetStore::sanitise("a\xC0\xAF" "b") == "a--b");                // an overlong '/' is two
	CHECK(PresetStore::sanitise("a\xC2\x85" "b") == "a-" "-b");             // a C1 control (NEL) is not a name

	const std::vector<uint8_t> pe(8, 0x0E);
	const std::vector<uint8_t> pg(8, 0x0A);
	const std::vector<uint8_t> pc(8, 0x0C);
	REQUIRE(PresetStore::save(e, pe, kChainPresetBank));
	REQUIRE(PresetStore::save(g, pg, kChainPresetBank));
	REQUIRE(PresetStore::save(cjk, pc, kChainPresetBank));
	const std::vector<StoredPreset> all = PresetStore::loadAll(kChainPresetBank);
	REQUIRE(all.size() == 3);
	size_t found = 0;
	for (const StoredPreset& p : all) {
		if (p.name == e) {
			CHECK(p.bytes == pe);
			++found;
		}
		else if (p.name == g) {
			CHECK(p.bytes == pg);
			++found;
		}
		else if (p.name == cjk) {
			CHECK(p.bytes == pc);
			++found;
		}
	}
	CHECK(found == 3);
}
