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

#include "fx/delay/delay_lanes.h"
#include "fx/delay/delay_types.h"
#include "plugin/device_preset.h"
#include "plugin/device_registry.h"
#include "plugin/factory_presets.h"
#include "plugin/preset_store.h"
#include "rack/inner_chain.h"
#include "rack/rack_axes.h"
#include "rack/rack_bridge.h"
#include "rack/rack_engine.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

// THE DELAY'S FACTORY PRESETS (stage S5) — the effects Cluster Delay ships as
// fixed slots, made here as RECIPES in the FEEDBACK FX lane: diffusion, shimmer, wobble, lo-fi, dub. PROVISIONAL, for
// audition by ear: the numbers below are a starting place, and once the files are edited they become the masters (Resonate's
// arrangement, `test_resonate_presets.cpp`), at which point the writer below must never be pointed at `presets/factory`.
//
// What is asserted of what ships: each file is a Delay preset tagged `factory` in its folder; loaded into a FRESH
// Delay, its lane holds exactly the recipe's devices (by tag, in order) and the recipe's values (by key); and every one
// stays under 0 dBFS at a typical input while being heard.

namespace dl = feathers::fx::delay;
namespace fs = std::filesystem;
using namespace feathers;

namespace {

constexpr const char* kBankPrefix = "Devices/DELAY/";
/// A FIXED SAVE TIME, so rewriting the files from an unchanged table changes no byte (2026-11-04 00:00 UTC).
constexpr int64_t kSavedAt = 1793750400;
constexpr double kRate = 48000.0;

/// One control, by its descriptor NAME: a number, or a Choice by its LABEL.
struct Set {
	const char* name;
	double value = 0.0;
	const char* label = nullptr;
};

struct LaneDevice {
	const char* tag;
	std::vector<Set> sets;
};

struct Recipe {
	const char* category;
	const char* name;
	std::vector<Set> delay;
	std::vector<LaneDevice> lane;
};

/// THE FACTORY SET — PROVISIONAL (see the banner).
const std::vector<Recipe>& recipes() {
	static const std::vector<Recipe> set {
		{ "Space", "Diffused Echoes",
		  { { "Delay Sync", 0.0, "1/8." }, { "Delay Feedback", 0.62 }, { "Delay Mix", 0.35 }, { "Delay Tone", -0.2 } },
		  { { "DISPERSE", {} } } },
		{ "Space", "Shimmer Steps",
		  { { "Delay Sync", 0.0, "1/4" }, { "Delay Feedback", 0.55 }, { "Delay Mix", 0.3 }, { "Delay Tone", 0.15 } },
		  { { "PITCH", { { "Pitch Shift", 12.0 } } } } },
		{ "Character", "Wobbled",
		  { { "Delay Sync", 0.0, "1/8" }, { "Delay Feedback", 0.5 }, { "Delay Mix", 0.3 } },
		  { { "PHFL", { { "Phaser Mode", 0.0, "Flanger" } } } } },
		{ "Character", "Lo-Fi Loop",
		  { { "Delay Sync", 0.0, "1/8." }, { "Delay Feedback", 0.6 }, { "Delay Mix", 0.35 } },
		  { { "SHPR", {} }, { "flt1", { { "Filter Cutoff", 0.55 } } } } },
		{ "Dub", "Dub Send",
		  { { "Delay Sync", 0.0, "1/4." }, { "Delay Feedback", 0.8 }, { "Delay Mix", 0.5 }, { "Delay Dub", 0.0, "On" },
		    { "Delay Ping-Pong", 0.0, "Stereo" } },
		  { { "flt1", { { "Filter Cutoff", 0.5 } } } } },
	};
	return set;
}

std::string pathOf(const Recipe& r) { return std::string(kBankPrefix) + r.category + "/" + r.name + ".fpreset"; }

size_t controlNamed(const device::Descriptor& desc, std::string_view name) {
	for (size_t i = 0; i < desc.params.size(); ++i) {
		if (name == desc.params[i].name) {
			return i;
		}
	}
	FAIL("no control " << name << " on " << desc.traits.name);
	return 0;
}

/// A SET'S FIELD VALUE: a number as given, a label as its index in the control's names.
float valueOf(const device::Descriptor& desc, const Set& s) {
	const device::ParamDesc& p = desc.params[controlNamed(desc, s.name)];
	if (s.label == nullptr) {
		return static_cast<float>(s.value);
	}
	for (size_t i = 0; i < p.names.size(); ++i) {
		if (std::string_view(p.names[i]) == s.label) {
			return static_cast<float>(i);
		}
	}
	FAIL("no choice " << s.label << " on " << s.name);
	return 0.0f;
}

/// A DELAY IN A RACK.
struct Rig {
	rack::RackEngine engine { rack::kRackAxes, hostCatalogue() };
	rack::RackBridge bridge { engine };
	DeviceInstanceId id = kNoInstance;
	Rig() {
		engine.prepare(kRate, 4096);
		bridge.prepare(kRate, 4096);
		id = bridge.addDevice(dl::kTraits.tag);
		REQUIRE(id != kNoInstance);
		bridge.drain();
		bridge.publish(engine);
	}
	rack::InnerChain* lane() const { return dl::feedbackLaneOf(bridge.containerSurfaceOf(id)); }
	/// -6 dBFS noise bursts, 120 ms on and 380 off, for `seconds`; the peak out.
	float peak(double seconds) {
		std::vector<float> l(256), r(256);
		uint32_t seed = 5;
		float peak = 0.0f;
		const int total = static_cast<int>(seconds * kRate);
		for (int done = 0; done < total; done += 256) {
			for (int i = 0; i < 256; ++i) {
				seed = seed * 1664525u + 1013904223u;
				const bool on = ((done + i) % 24000) < 5760;
				l[static_cast<size_t>(i)] = on ? 0.5f * (static_cast<float>(seed >> 8) / 8388608.0f - 1.0f) : 0.0f;
				r[static_cast<size_t>(i)] = l[static_cast<size_t>(i)];
			}
			bridge.drain();
			engine.processStereo(l.data(), r.data(), 256, backplane::unknownBlock());
			bridge.publish(engine);
			for (int i = 0; i < 256; ++i) {
				peak = std::max({ peak, std::fabs(l[static_cast<size_t>(i)]), std::fabs(r[static_cast<size_t>(i)]) });
			}
		}
		return peak;
	}
};

/// THE RECIPE BUILT IN A LIVE DELAY, captured as preset bytes.
std::vector<uint8_t> capture(const Recipe& r) {
	Rig rig;
	for (const Set& s : r.delay) {
		rig.bridge.setField(rig.id, controlNamed(dl::kDescriptor, s.name), valueOf(dl::kDescriptor, s));
	}
	rack::InnerChain* lane = dl::ensureFeedbackLane(rig.bridge.containerSurfaceOf(rig.id));
	REQUIRE(lane != nullptr);
	for (const LaneDevice& d : r.lane) {
		const device::Descriptor* desc = deviceForTag(d.tag);
		REQUIRE(desc != nullptr);
		const DeviceInstanceId inner = lane->bridge()->addDevice(d.tag);
		REQUIRE(inner != kNoInstance);
		for (const Set& s : d.sets) {
			lane->bridge()->setField(inner, controlNamed(*desc, s.name), valueOf(*desc, s));
		}
	}
	std::vector<uint8_t> bytes;
	REQUIRE(captureDevicePreset(rig.bridge, dl::kDescriptor, rig.id, &bytes));
	return bytes;
}

std::vector<FactoryPresetFile> shipped() {
	std::vector<FactoryPresetFile> out;
	for (const FactoryPresetFile& f : factoryPresetFiles()) {
		if (f.path.starts_with(kBankPrefix)) {
			out.push_back(f);
		}
	}
	return out;
}

std::vector<uint8_t> payloadOf(const FactoryPresetFile& f, PresetMeta* meta) {
	std::vector<uint8_t> payload;
	REQUIRE(PresetStore::decode(std::vector<uint8_t>(f.bytes.begin(), f.bytes.end()), meta, &payload));
	return payload;
}

} // namespace

TEST_CASE("DELAY factory presets: writes the files from the recipes (only with FEATHERS_DELAY_PRESETS_OUT)") {
	const char* outDir = std::getenv("FEATHERS_DELAY_PRESETS_OUT");
	if (outDir == nullptr || *outDir == '\0') {
		MESSAGE("FEATHERS_DELAY_PRESETS_OUT unset: nothing written");
		return;
	}
	for (const Recipe& r : recipes()) {
		PresetMeta meta;
		meta.tags = { "factory" };
		meta.saved = kSavedAt;
		meta.category = r.category;
		const std::vector<uint8_t> file = PresetStore::encode(meta, capture(r));
		const fs::path path = fs::path(outDir) / pathOf(r);
		fs::create_directories(path.parent_path());
		std::ofstream os(path, std::ios::binary | std::ios::trunc);
		os.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
		REQUIRE(os.good());
		MESSAGE("wrote " << path.string());
	}
}

TEST_CASE("DELAY factory presets: each loads into a FRESH Delay with its lane's devices and values, by value") {
	const std::vector<FactoryPresetFile> files = shipped();
	REQUIRE(files.size() == recipes().size());
	for (const Recipe& r : recipes()) {
		const std::string name = r.name;
		CAPTURE(name);
		const auto file = std::find_if(files.begin(), files.end(), [&](const FactoryPresetFile& f) { return f.path == pathOf(r); });
		REQUIRE(file != files.end());
		PresetMeta meta;
		const std::vector<uint8_t> payload = payloadOf(*file, &meta);
		CHECK(std::find(meta.tags.begin(), meta.tags.end(), "factory") != meta.tags.end());
		CHECK(meta.category == r.category);

		Rig rig;
		REQUIRE(applyDevicePreset(rig.bridge, dl::kDescriptor, rig.id, payload, {}) == DevicePresetLoad::Loaded);
		(void)rig.peak(0.05); // a block or two, so the lane's edits drain
		for (const Set& s : r.delay) {
			CAPTURE(s.name);
			CHECK(rig.bridge.fieldAt(rig.id, controlNamed(dl::kDescriptor, s.name)) == valueOf(dl::kDescriptor, s));
		}
		rack::InnerChain* lane = rig.lane();
		REQUIRE(lane != nullptr);
		rack::RackBridge& b = *lane->bridge();
		REQUIRE(b.chainLength() == r.lane.size());
		for (size_t k = 0; k < r.lane.size(); ++k) {
			const DeviceInstanceId inner = b.deviceAtPosition(k);
			const device::Descriptor* desc = b.descriptorOf(inner);
			REQUIRE(desc != nullptr);
			CHECK(device::sameTag(desc->traits.tag, r.lane[k].tag));
			for (const Set& s : r.lane[k].sets) {
				CAPTURE(s.name);
				CHECK(b.fieldAt(inner, controlNamed(*desc, s.name)) == valueOf(*desc, s));
			}
		}
	}
}

TEST_CASE("DELAY factory presets: every one stays under 0 dBFS at a typical input, and is heard") {
	for (const FactoryPresetFile& f : shipped()) {
		CAPTURE(f.path);
		PresetMeta meta;
		const std::vector<uint8_t> payload = payloadOf(f, &meta);
		Rig rig;
		REQUIRE(applyDevicePreset(rig.bridge, dl::kDescriptor, rig.id, payload, {}) == DevicePresetLoad::Loaded);
		const float peak = rig.peak(8.0);
		MESSAGE(f.path << ": peak " << 20.0 * std::log10(std::max(1e-9f, peak)) << " dBFS (input -6 dBFS)");
		CHECK(peak < 1.0f);
		CHECK(peak > 0.05f);
	}
}

TEST_CASE("DELAY presets: Init is the declared defaults and an EMPTY lane — a lane that held devices is emptied") {
	Rig rig;
	rack::InnerChain* lane = dl::ensureFeedbackLane(rig.bridge.containerSurfaceOf(rig.id));
	REQUIRE(lane != nullptr);
	REQUIRE(lane->bridge()->addDevice("flt1") != kNoInstance);
	rig.bridge.setField(rig.id, controlNamed(dl::kDescriptor, "Delay Feedback"), 0.9f);
	REQUIRE(applyDevicePreset(rig.bridge, dl::kDescriptor, rig.id, initDevicePreset(dl::kDescriptor), {})
	        == DevicePresetLoad::Loaded);
	(void)rig.peak(0.05);
	CHECK(rig.bridge.fieldAt(rig.id, controlNamed(dl::kDescriptor, "Delay Feedback"))
	      == static_cast<float>(dl::kParamDescs[static_cast<size_t>(dl::Param::Feedback)].def));
	REQUIRE(rig.lane() != nullptr);
	CHECK(rig.lane()->bridge()->chainLength() == 0);
}
