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

#include "backplane/container_bodies.h"
#include "fx/delay/delay_types.h"
#include "fx/featherverb/featherverb_types.h"
#include "fx/filter/filter_types.h"
#include "fx/phaser/phaser.h"
#include "fx/sklitter/sklitter_types.h"
#include "plugin/audition.h"
#include "plugin/bridge_device_host.h"
#include "plugin/device_preset.h"
#include "plugin/device_registry.h"
#include "plugin/feathers_chains.h"
#include "plugin/gui_bridge.h"
#include "rack/rack_bridge.h"
#include "rack/rack_engine.h"
#include "rack/rack_axes.h"
#include "rack/rack_state.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <bit>
#include <cmath>
#include <cstring>
#include <random>
#include <string>
#include <vector>

// **THE PROPERTY THE STATE FORMAT EXISTS FOR** (backlog JF): a patch saved by one build and loaded by a build whose DEVICE HAS CHANGED gives
// every surviving control its value, a new control its default, a Choice its same LABEL wherever that label
// now stands, and a load report naming exactly what did not load as saved.
//
// THE EDIT IS SIMULATED, NOT WAITED FOR: the saved bytes are real (a SKLITTER device preset through the
// preset road, a Feathers chains block holding Featherverb through the state road), and the build that
// reads them is the same device's descriptor with a control inserted mid-row, one removed, a Choice
// reordered and a Choice label renamed with an alias. Only the reading descriptor is synthetic — which is
// exactly what a later build's is.
//
// Known-bads (the house protocol, `run-cases.sh --each`): key by position again, and these go red; drop
// the alias table, and the renamed label goes red.

using namespace feathers;
namespace sc = feathers::fx::sklitter;
namespace fv = feathers::fx::featherverb;

namespace {

bool labelled(const device::ParamDesc& p) { return p.unit == device::Unit::Choice && !p.names.empty(); }

size_t labelIndex(const device::ParamDesc& p, float v) {
	const long long i = std::llround(static_cast<double>(v) - p.min);
	return static_cast<size_t>(std::clamp(i, 0LL, static_cast<long long>(p.names.size()) - 1));
}

size_t indexOfKey(std::span<const device::ParamDesc> params, std::string_view key) {
	for (size_t i = 0; i < params.size(); ++i) {
		if (key == params[i].key) {
			return i;
		}
	}
	return params.size();
}

/// EVERY CONTROL OFF ITS DEFAULT, each by the rule a hand's edit takes — so a value that fell to its
/// default is visible as one, and no control's survival can be a coincidence.
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

//------------------------------------------------------------------------------------------------//
// A LATER BUILD'S DEVICE                                                                         //
//------------------------------------------------------------------------------------------------//

/// **THE SAME DEVICE, EDITED THE FOUR WAYS THAT USED TO COST A `kStateVersion` BUMP EACH.** Owns every
/// span its descriptor points at.
struct EditedDevice {
	static constexpr const char* kInsertedKey = "jf_inserted_mid";
	static constexpr const char* kRenamedSuffix = " (renamed)";

	std::vector<device::ParamDesc> params;
	std::vector<std::vector<const char*>> names; ///< per new control, when its labels were changed
	std::vector<std::string> strings;            ///< backing for renamed labels
	std::vector<device::ChoiceAlias> alias;
	device::Descriptor desc {};

	size_t insertedAt = 0; ///< in the NEW order
	std::string removedKey;
	std::string reorderedKey;
	std::string renamedKey;
	std::string renamedFrom;
	std::string renamedTo;

	/// `savedRow` is what was saved, in the ORIGINAL order — the renamed label is the one it chose, so the
	/// alias is the only road that value has.
	EditedDevice(const device::Descriptor& original, const std::vector<float>& savedRow, bool withAlias = true) {
		const std::span<const device::ParamDesc> was = original.params;
		size_t reorder = was.size();
		size_t rename = was.size();
		size_t remove = was.size();
		for (size_t i = 0; i < was.size(); ++i) {
			if (labelled(was[i]) && was[i].names.size() >= 3 && reorder == was.size()) {
				reorder = i;
			}
		}
		for (size_t i = 0; i < was.size(); ++i) {
			if (labelled(was[i]) && i != reorder && rename == was.size()) {
				rename = i;
			}
		}
		// THE REMOVED ONE: a plain number from the first half, so everything after it would shift under a
		// positional read.
		for (size_t i = 1; i < was.size() / 2; ++i) {
			if (!labelled(was[i]) && i != reorder && i != rename) {
				remove = i;
			}
		}
		REQUIRE(reorder < was.size());
		REQUIRE(rename < was.size());
		REQUIRE(remove < was.size());
		removedKey = was[remove].key;
		reorderedKey = was[reorder].key;
		renamedKey = was[rename].key;
		renamedFrom = was[rename].names[labelIndex(was[rename], savedRow[rename])];
		renamedTo = renamedFrom + kRenamedSuffix;

		strings.reserve(1); // `renamedTo` below points into it; one element, never reallocated
		strings.push_back(renamedTo);
		std::vector<size_t> origin;
		for (size_t i = 0; i < was.size(); ++i) {
			if (i != remove) {
				origin.push_back(i);
			}
		}
		insertedAt = origin.size() / 2;
		names.resize(origin.size() + 1);
		for (size_t j = 0; j < origin.size(); ++j) {
			const size_t newIndex = j < insertedAt ? j : j + 1;
			device::ParamDesc p = was[origin[j]];
			if (origin[j] == reorder) {
				names[newIndex].assign(p.names.rbegin(), p.names.rend()); // the same labels, backwards
				p.names = names[newIndex];
			}
			if (origin[j] == rename) {
				names[newIndex].assign(p.names.begin(), p.names.end());
				for (const char*& label : names[newIndex]) {
					if (renamedFrom == label) {
						label = strings[0].c_str();
					}
				}
				p.names = names[newIndex];
				if (withAlias) {
					alias.push_back(device::ChoiceAlias { was[rename].names[labelIndex(was[rename], savedRow[rename])],
						                                  strings[0].c_str() });
					p.choiceAliases = alias;
				}
			}
			params.push_back(p);
		}
		device::ParamDesc inserted { .key = kInsertedKey,
			                         .name = "Inserted",
			                         .min = 0.0,
			                         .max = 1.0,
			                         .def = 0.375,
			                         .stepped = false,
			                         .unit = device::Unit::Percent,
			                         .names = {} };
		params.insert(params.begin() + static_cast<std::ptrdiff_t>(insertedAt), inserted);
		// `names` was laid out in the new order before the insert shifted `params`; re-point every span.
		for (size_t j = 0; j < params.size(); ++j) {
			if (!names[j].empty()) {
				params[j].names = names[j];
			}
		}
		if (!alias.empty()) {
			params[indexOfKey(params, renamedKey)].choiceAliases = alias;
		}
		desc = original;
		desc.params = params;
		// THE INDEX-ADDRESSED TRAITS NO LONGER LINE UP, and nothing on the decode road reads them.
		desc.traits.enableControl = -1;
		desc.traits.amountControl = -1;
		desc.traits.perVoiceControl = -1;
		desc.traits.faceOrder = {};
		desc.traits.sideColumn = {};
		desc.traits.faceSections = {};
		desc.selfRouting = nullptr;
	}
};

/// **THE CLAIMS, FOR ONE DEVICE:** `decoded` is `edit.desc`'s row as the load gave it, `saved` the original
/// device's row as it was saved.
void checkSurvived(const device::Descriptor& original, const std::vector<float>& saved, const EditedDevice& edit,
                   const std::vector<float>& decoded, const rack::LoadReport& report) {
	REQUIRE(decoded.size() >= edit.params.size());
	for (size_t j = 0; j < edit.params.size(); ++j) {
		const device::ParamDesc& p = edit.params[j];
		INFO("control " << j << " '" << p.key << "'");
		if (std::string_view(p.key) == EditedDevice::kInsertedKey) {
			CHECK(decoded[j] == static_cast<float>(p.def)); // NEW: its default
			continue;
		}
		const size_t i = indexOfKey(original.params, p.key);
		REQUIRE(i < original.params.size());
		if (labelled(p)) {
			// A CHOICE BY ITS LABEL — the one saved, wherever that label now stands, or its new name.
			const std::string savedLabel = original.params[i].names[labelIndex(original.params[i], saved[i])];
			const std::string loadedLabel = p.names[labelIndex(p, decoded[j])];
			CHECK(loadedLabel == (p.key == edit.renamedKey && savedLabel == edit.renamedFrom ? edit.renamedTo : savedLabel));
		}
		else {
			// EVERY OTHER SURVIVOR TO THE BIT.
			CHECK(std::bit_cast<uint32_t>(decoded[j]) == std::bit_cast<uint32_t>(saved[i]));
		}
	}
	// THE REORDERED CHOICE IS NOT VACUOUS: its saved label is somewhere else now.
	const size_t r0 = indexOfKey(original.params, edit.reorderedKey);
	const size_t r1 = indexOfKey(edit.params, edit.reorderedKey);
	CHECK(labelIndex(original.params[r0], saved[r0]) != labelIndex(edit.params[r1], decoded[r1]));

	// **THE REPORT, BY VALUE, AND EXACTLY:** one new control at its default, one value this build has no
	// control for, and nothing else — no label unknown (the rename had its alias), nothing corrected.
	using Kind = rack::LoadReport::Kind;
	CHECK(report.countOf(Kind::KeyMissing) == 1);
	CHECK(report.firstOf(Kind::KeyMissing).what == EditedDevice::kInsertedKey);
	CHECK(report.countOf(Kind::KeyUnknown) == 1);
	CHECK(report.firstOf(Kind::KeyUnknown).what == edit.removedKey);
	CHECK(report.countOf(Kind::LabelUnknown) == 0);
	uint32_t total = 0;
	for (const uint32_t c : report.count) {
		total += c;
	}
	CHECK(total == 2);
}

//------------------------------------------------------------------------------------------------//
// STREAMS                                                                                        //
//------------------------------------------------------------------------------------------------//

struct Bytes {
	std::vector<uint8_t> data;
	size_t at = 0;
	static int64_t CLAP_ABI write(const clap_ostream* s, const void* buffer, uint64_t size) {
		auto* self = static_cast<Bytes*>(s->ctx);
		const auto* p = static_cast<const uint8_t*>(buffer);
		self->data.insert(self->data.end(), p, p + size);
		return static_cast<int64_t>(size);
	}
	static int64_t CLAP_ABI read(const clap_istream* s, void* buffer, uint64_t size) {
		auto* self = static_cast<Bytes*>(s->ctx);
		const size_t n = std::min<size_t>(static_cast<size_t>(size), self->data.size() - self->at);
		std::memcpy(buffer, self->data.data() + self->at, n);
		self->at += n;
		return static_cast<int64_t>(n);
	}
	clap_ostream out() { return clap_ostream { this, &Bytes::write }; }
	clap_istream in() {
		at = 0;
		return clap_istream { this, &Bytes::read };
	}
};

/// A catalogue that answers `edited` for its tag and the registry for everything else — a later build.
struct LaterBuild {
	const device::Descriptor* edited = nullptr;
	static const device::Descriptor* find(void* host, const char* tag) {
		const auto* self = static_cast<LaterBuild*>(host);
		if (device::sameTag(tag, self->edited->traits.tag)) {
			return self->edited;
		}
		return deviceForTag(tag);
	}
	device::Catalogue catalogue() { return device::Catalogue { this, &LaterBuild::find }; }
};

} // namespace

//------------------------------------------------------------------------------------------------//
// THE PROOF                                                                                      //
//------------------------------------------------------------------------------------------------//

TEST_CASE("JF proof: a SKLITTER device preset survives a device edit — insert, remove, reorder, rename") {
	rack::RackEngine engine { rack::kRackAxes, hostCatalogue() };
	engine.prepare(48000.0, 4096);
	rack::RackBridge bridge(engine);
	bridge.prepare(48000.0, 4096);
	const DeviceInstanceId id = bridge.addDevice(sc::kTraits.tag);
	REQUIRE(id != kNoInstance);
	setDistinctRow(bridge, id, sc::kDescriptor);
	const std::vector<float> saved = rowOf(bridge, id, sc::kDescriptor);

	std::vector<uint8_t> bytes;
	REQUIRE(captureDevicePreset(bridge, sc::kDescriptor, id, &bytes));
	// A DOCUMENT, READABLE: the preset names its values, and a Choice by its label.
	const std::string text(bytes.begin(), bytes.end());
	CHECK(text.find("\"format\": \"feathers.rack\"") != std::string::npos);
	CHECK(text.find(std::string("\"") + sc::kDescriptor.params[0].key + "\":") != std::string::npos);

	const EditedDevice edit(sc::kDescriptor, saved);
	DevicePresetContents contents;
	rack::LoadReport report;
	REQUIRE(readDevicePreset(bytes, &contents, &edit.desc, &report));
	checkSurvived(sc::kDescriptor, saved, edit, contents.field, report);

	// AND SAID: the tray line a load of this preset would show.
	const std::string line = rack::loadReportLine(report, {});
	CHECK(line.find(edit.removedKey) != std::string::npos);
	CHECK(line.find("new control") != std::string::npos);
}

TEST_CASE("JF proof: a Feathers state with Featherverb in the fx lane survives a device edit") {
	ParamBridge bridge;
	BridgeDeviceHost host { bridge };
	const size_t slot = bridge.addFxDevice(fv::kTraits.tag);
	REQUIRE(slot < kMaxFxSlots);
	const DeviceInstanceId id = bridge.fxDeviceAt(slot);
	setDistinctRow(host, id, fv::kDescriptor);
	const std::vector<float> saved = rowOf(host, id, fv::kDescriptor);

	// THE STATE ROAD: Feathers' chains block, written and read as a host would.
	Bytes stream;
	clap_ostream out = stream.out();
	REQUIRE(saveChainsBlock(&out, bridge.chainsPatch()));
	clap_istream in = stream.in();
	rack::RackPatch patch;
	bool present = false;
	REQUIRE(loadChainsBlock(&in, &patch, &present));
	REQUIRE(present);

	// A LATER BUILD, whose Featherverb has changed, loads it.
	const EditedDevice edit(fv::kDescriptor, saved);
	LaterBuild later { &edit.desc };
	backplane::InstanceCounter ids;
	ContainerBodies bodies;
	rack::ChainRecord record { feathersAxes(), kFeathersChainCapacities, later.catalogue(), ids, &backplane::permitAll };
	const rack::LoadReport report = rack::applyPatchToRecord(patch, &record, &bodies, &ids, feathersCoreCodec());
	REQUIRE(record.holds(id));
	REQUIRE(record.descriptorOf(id) == &edit.desc);
	const auto& row = record.rowOf(id);
	const std::vector<float> decoded(row.field.begin(), row.field.begin() + static_cast<std::ptrdiff_t>(row.fieldsPresent));
	checkSurvived(fv::kDescriptor, saved, edit, decoded, report);

	// AND THE VALUE ONLY THE OLD BUILD KNEW IS CARRIED, NOT LOST: saved back out by the later build, and a
	// build that has the control again finds it.
	backplane::RouteList routes;
	record.publishRoutes(routes);
	const rack::RackPatch resaved = rack::patchOfRecord(record, &bodies, {}, routes, feathersCoreCodec());
	const auto it = std::find_if(resaved.device.begin(), resaved.device.end(),
	                             [&](const rack::RackPatch::Device& d) { return d.instanceId == id; });
	REQUIRE(it != resaved.device.end());
	const size_t removed = indexOfKey(fv::kDescriptor.params, edit.removedKey);
	std::vector<float> back(fv::kDescriptor.params.size() + fv::kDescriptor.traits.extraState);
	rack::LoadReport again;
	back.resize(rack::rowOfValues(fv::kDescriptor, *it, back, &again, "Featherverb", nullptr));
	CHECK(back[removed] == saved[removed]);
}

TEST_CASE("JF: without its alias, a renamed Choice label is reported and that control defaults") {
	// THE ALIAS TABLE'S OWN CASE: the same edit with no alias must say so, not guess.
	rack::RackEngine engine { rack::kRackAxes, hostCatalogue() };
	engine.prepare(48000.0, 4096);
	rack::RackBridge bridge(engine);
	bridge.prepare(48000.0, 4096);
	const DeviceInstanceId id = bridge.addDevice(sc::kTraits.tag);
	setDistinctRow(bridge, id, sc::kDescriptor);
	const std::vector<float> saved = rowOf(bridge, id, sc::kDescriptor);
	std::vector<uint8_t> bytes;
	REQUIRE(captureDevicePreset(bridge, sc::kDescriptor, id, &bytes));

	const EditedDevice edit(sc::kDescriptor, saved, false);
	DevicePresetContents contents;
	rack::LoadReport report;
	REQUIRE(readDevicePreset(bytes, &contents, &edit.desc, &report));
	CHECK(report.countOf(rack::LoadReport::Kind::LabelUnknown) == 1);
	CHECK(report.firstOf(rack::LoadReport::Kind::LabelUnknown).what == edit.renamedKey);
	CHECK(report.firstOf(rack::LoadReport::Kind::LabelUnknown).why == edit.renamedFrom);
	const size_t j = indexOfKey(edit.params, edit.renamedKey);
	CHECK(contents.field[j] == static_cast<float>(edit.params[j].def));
}

TEST_CASE("JF: the Delay's Ping-Pong saved as On loads as Stereo, through its alias, and Off stays Off") {
	// **THE CONTROL WAS OFF/ON UNTIL 2026-10-11 AND IS OFF/STEREO/CLASSIC NOW** (`delay_types.h`), stored by label. A patch
	// from before holds the word `On`; this is that patch, made by saving one today and putting the old word back.
	namespace dly = feathers::fx::delay;
	const size_t pp = static_cast<size_t>(dly::Param::PingPong);
	for (const float saved : { 0.0f, 1.0f }) {
		CAPTURE(saved);
		rack::RackEngine engine { rack::kRackAxes, hostCatalogue() };
		engine.prepare(48000.0, 4096);
		rack::RackBridge bridge(engine);
		bridge.prepare(48000.0, 4096);
		const DeviceInstanceId id = bridge.addDevice(dly::kTraits.tag);
		REQUIRE(id != kNoInstance);
		bridge.setField(id, pp, saved);
		std::vector<uint8_t> bytes;
		REQUIRE(captureDevicePreset(bridge, dly::kDescriptor, id, &bytes));
		std::string text(bytes.begin(), bytes.end());
		const std::string now = std::string("\"") + dly::kParamDescs[pp].key + "\": \"" + (saved > 0.5f ? "Stereo" : "Off") + "\"";
		const size_t at = text.find(now);
		REQUIRE(at != std::string::npos); // the document really does store the label
		const std::string old = std::string("\"") + dly::kParamDescs[pp].key + "\": \"" + (saved > 0.5f ? "On" : "Off") + "\"";
		text.replace(at, now.size(), old);
		const std::vector<uint8_t> oldBytes(text.begin(), text.end());
		DevicePresetContents contents;
		rack::LoadReport report;
		REQUIRE(readDevicePreset(oldBytes, &contents, &dly::kDescriptor, &report));
		CHECK(report.countOf(rack::LoadReport::Kind::LabelUnknown) == 0);
		CHECK(contents.field[pp] == saved); // On -> 1 (Stereo), Off -> 0
	}
}

//------------------------------------------------------------------------------------------------//
// §3.12 CASE 1 AND 2 — EVERY DEVICE, BY VALUE, AND BYTE-STABLE                                    //
//------------------------------------------------------------------------------------------------//

TEST_CASE("JF: every device's every control round-trips to the bit through the document, and re-saves byte for byte") {
	std::mt19937 rng(20260925);
	rack::RackPatch patch;
	patch.present = true;
	std::vector<std::vector<float>> rows;
	for (size_t k = 0; k < kDeviceCount; ++k) {
		const device::Descriptor& d = kDevices[k];
		std::vector<float> row(d.params.size() + d.traits.extraState);
		for (size_t i = 0; i < d.params.size(); ++i) {
			const device::ParamDesc& p = d.params[i];
			std::uniform_real_distribution<double> in(p.min, p.max);
			double v = in(rng);
			if (p.stepped) {
				v = std::round(v); // a stepped control is read at its step; a label names only a step
			}
			row[i] = device::coerceControl(p, static_cast<float>(v));
		}
		std::uniform_real_distribution<float> blob(-4.0f, 4.0f);
		for (size_t e = 0; e < d.traits.extraState; ++e) {
			row[d.params.size() + e] = blob(rng);
		}
		rack::RackPatch::Device device;
		device.tag = rack::patchTagOf(d.traits.tag);
		device.instanceId = static_cast<DeviceInstanceId>(kFirstInstance + k);
		rack::valuesOfRow(d, row, row.size(), &device);
		patch.device.push_back(device);
		rows.push_back(row);
	}
	std::string text;
	REQUIRE(rack::rackToText(patch, &text));
	rack::RackPatch parsed;
	rack::LoadReport report;
	REQUIRE(rack::parseRackText(text, &parsed, &report));
	CHECK(report.empty());
	CHECK(parsed == patch);
	std::string again;
	REQUIRE(rack::rackToText(parsed, &again));
	CHECK(again == text); // §3.12 case 2: save → load → save, the same bytes

	for (size_t k = 0; k < kDeviceCount; ++k) {
		const device::Descriptor& d = kDevices[k];
		INFO("device " << d.traits.tag);
		std::vector<float> back(d.params.size() + d.traits.extraState);
		rack::LoadReport r;
		back.resize(rack::rowOfValues(d, parsed.device[k], back, &r, d.traits.name, nullptr));
		CHECK(r.empty());
		REQUIRE(back.size() == rows[k].size());
		for (size_t i = 0; i < back.size(); ++i) {
			INFO("field " << i);
			CHECK(std::bit_cast<uint32_t>(back[i]) == std::bit_cast<uint32_t>(rows[k][i]));
		}
	}
}

//------------------------------------------------------------------------------------------------//
// §3.12 CASE 5 AND 6 — FORWARD, AND THE REPORT BY VALUE                                            //
//------------------------------------------------------------------------------------------------//

TEST_CASE("JF: a newer build's values load what this one knows, report the rest, and re-save with them intact") {
	using Value = rack::RackPatch::Value;
	using Kind = rack::LoadReport::Kind;
	const device::Descriptor& f = fx::filter::kDescriptor;
	rack::RackPatch patch;
	patch.present = true;
	rack::RackPatch::Device d;
	d.tag = rack::patchTagOf(f.traits.tag);
	d.chain = 1; // Feathers' fx lane
	d.instanceId = 5;
	rack::valuesOfRow(f, std::vector<float>(f.params.size(), 0.0f), 0, &d);
	d.schema = f.traits.schema + 1; // a newer build
	d.values.push_back(Value::of("filter_future", 0.5f));
	d.values.push_back(Value { "filter_shape", Value::Kind::Raw, 0.0f, "{\"points\":[1,2]}" });
	for (Value& v : d.values) {
		if (v.key == "filter_type") {
			v = Value::label("filter_type", "Hyperpass");
		}
	}
	patch.device.push_back(d);
	// AND A DEVICE THIS BUILD HAS NEVER HEARD OF.
	rack::RackPatch::Device alien;
	alien.tag = rack::patchTagOf("ALIEN");
	alien.chain = 1;
	alien.instanceId = 6;
	alien.values = { Value::of("warp", 3.0f) };
	patch.device.push_back(alien);

	std::string text;
	REQUIRE(rack::rackToText(patch, &text));
	rack::RackPatch parsed;
	REQUIRE(rack::parseRackText(text, &parsed));

	backplane::InstanceCounter ids;
	ContainerBodies bodies;
	rack::ChainRecord record { feathersAxes(), kFeathersChainCapacities, hostCatalogue(), ids, &backplane::permitAll };
	const rack::LoadReport report = rack::applyPatchToRecord(parsed, &record, &bodies, &ids, feathersCoreCodec());
	CHECK(report.countOf(Kind::NewerSchema) == 1);
	CHECK(report.countOf(Kind::KeyUnknown) == 2);
	CHECK(report.firstOf(Kind::KeyUnknown).what == "filter_future");
	CHECK(report.countOf(Kind::LabelUnknown) == 1);
	CHECK(report.firstOf(Kind::LabelUnknown).what == "filter_type");
	CHECK(report.firstOf(Kind::LabelUnknown).why == "Hyperpass");
	CHECK(report.countOf(Kind::DeviceUnknown) == 1);
	CHECK(report.firstOf(Kind::DeviceUnknown).what == "ALIEN");
	CHECK(report.countOf(Kind::KeyMissing) == 0);
	CHECK(report.countOf(Kind::RouteDropped) == 0);
	// THE LABEL THIS BUILD LACKS PUT ITS CONTROL AT THE DEFAULT.
	const size_t type = indexOfKey(f.params, "filter_type");
	CHECK(record.rowOf(5).field[type] == static_cast<float>(f.params[type].def));

	// RE-SAVED WITH THE UNKNOWNS INTACT — byte for byte where they are the newer build's.
	backplane::RouteList routes;
	record.publishRoutes(routes);
	const rack::RackPatch resaved = rack::patchOfRecord(record, &bodies, {}, routes, feathersCoreCodec());
	REQUIRE(resaved.device.size() == 2);
	const auto has = [](const rack::RackPatch::Device& dev, const Value& v) {
		return std::find(dev.values.begin(), dev.values.end(), v) != dev.values.end();
	};
	CHECK(has(resaved.device[0], Value::of("filter_future", 0.5f)));
	CHECK(has(resaved.device[0], Value { "filter_shape", Value::Kind::Raw, 0.0f, "{\"points\":[1,2]}" }));
	CHECK(resaved.device[1].values == alien.values);
	CHECK(resaved.device[1].tag == alien.tag);
	std::string out;
	REQUIRE(rack::rackToText(resaved, &out));
	CHECK(out.find("\"filter_shape\": {") != std::string::npos);
}

TEST_CASE("JF: a retired key is dropped with its reason, not carried") {
	using Kind = rack::LoadReport::Kind;
	const device::Descriptor& f = fx::filter::kDescriptor;
	static constexpr device::RetiredKey kRetired[] = { { "filter_old_drive", "2026-09-25", "rescaled to 0..1" } };
	device::Descriptor later = f;
	later.traits.retiredKeys = kRetired;
	rack::RackPatch::Device d;
	rack::valuesOfRow(f, std::vector<float>(f.params.size(), 0.0f), 0, &d);
	d.values.push_back(rack::RackPatch::Value::of("filter_old_drive", 70.0f));
	std::vector<float> row(f.params.size());
	rack::LoadReport report;
	rack::RackPatch::Device carried;
	(void)rack::rowOfValues(later, d, row, &report, "Filter", &carried);
	CHECK(report.countOf(Kind::KeyRetired) == 1);
	CHECK(report.firstOf(Kind::KeyRetired).what == "filter_old_drive");
	CHECK(report.firstOf(Kind::KeyRetired).why.find("rescaled to 0..1") != std::string::npos);
	CHECK(report.countOf(Kind::KeyUnknown) == 0);
	CHECK(carried.values.empty());
}

TEST_CASE("JF: the old binary format, and anything that is not a rack document, is refused whole and said") {
	using Kind = rack::LoadReport::Kind;
	// THE FIRST BYTES OF THE BINARY FORMAT THIS REPLACED: version 10, then "MACR".
	const std::string binary = std::string("\x0a" "MACR\x00\x00\x00\x00", 9);
	rack::RackPatch patch;
	rack::LoadReport report;
	CHECK_FALSE(rack::parseRackText(binary, &patch, &report));
	CHECK(report.countOf(Kind::NotLoaded) == 1);
	CHECK(report.firstOf(Kind::NotLoaded).why.find("older format") != std::string::npos);
	CHECK(rack::loadReportLine(report, {}).rfind("Not loaded:", 0) == 0);

	report = {};
	CHECK_FALSE(rack::parseRackText("{\"format\": \"feathers.rack\", \"major\": 2, \"chains\": []}", &patch, &report));
	CHECK(report.firstOf(Kind::NotLoaded).why.find("newer format") != std::string::npos);
	report = {};
	CHECK_FALSE(rack::parseRackText("{\"format\": \"something.else\", \"major\": 1}", &patch, &report));
	CHECK(report.countOf(Kind::NotLoaded) == 1);
	report = {};
	CHECK_FALSE(rack::parseRackText("{\"format\": \"feathers.rack\", \"major\": 1, \"chains\": [[{\"tag\": \"flt1\"", &patch, &report));
	CHECK(report.countOf(Kind::NotLoaded) == 1);
	// AN EMPTY STREAM IS STILL A FRESH INSTANCE, NOT A REFUSAL.
	report = {};
	CHECK(rack::parseRackText("", &patch, &report));
	CHECK_FALSE(patch.present);
	CHECK(report.empty());
}

TEST_CASE("JF: the tray line names the most serious thing in full and counts the rest") {
	using Kind = rack::LoadReport::Kind;
	rack::LoadReport r;
	CHECK(rack::loadReportLine(r, {}).empty());
	r.note(Kind::KeyMissing, "Featherverb (chain 2, #1)", "featherverb_new");
	r.note(Kind::KeyMissing, "Featherverb (chain 2, #1)", "featherverb_newer");
	r.note(Kind::KeyUnknown, "SKLITTER (chain 1, #1)", "groove_swing");
	const std::string line = rack::loadReportLine(r, {});
	CHECK(line.rfind("Loaded with changes: ", 0) == 0);
	CHECK(line.find("'groove_swing' is not a control this build has") != std::string::npos); // the headline
	CHECK(line.find("2 new controls at their defaults") != std::string::npos);                // counted
	// ROUTES ALONE READ AS THEY ALWAYS HAVE.
	rack::LoadReport routes;
	routes.noteRoute(backplane::Source::core(1), backplane::Destination::core(2));
	CHECK(rack::loadReportLine(routes, "Macro 2 -> Level") == "Dropped route: Macro 2 -> Level");
}

TEST_CASE("JF: a schema-1 Phaser/Flanger's thirty travel floats load as the first thirty, the four it lacks as home, and no coercion is reported") {
	// THE MIGRATION ROUND 2 DECLARED (`phaser_types.h`, schema 2): the travel grew 30 → 34 floats, APPENDED, so an older
	// patch's thirty keep their indices and meanings and the new four load as 0 — home. Through the whole text road: the
	// document written with "schema": 1 and a 30-element array, parsed, turned into a row, handed to a device whose
	// travel 30..33 was not zero before.
	namespace ph = fx::phaser;
	using Kind = rack::LoadReport::Kind;
	const device::Descriptor& p = ph::kDescriptor;
	REQUIRE(p.traits.schema == 2);
	REQUIRE(p.traits.extraState == 34);
	std::vector<float> defaults(p.params.size());
	for (size_t i = 0; i < defaults.size(); ++i) {
		defaults[i] = static_cast<float>(p.params[i].def);
	}
	std::array<float, 30> old {};
	std::string array = "[";
	for (size_t i = 0; i < old.size(); ++i) {
		old[i] = 0.013f + 0.029f * static_cast<float>(i);
		char buf[32];
		std::snprintf(buf, sizeof buf, "%.9g", static_cast<double>(old[i]));
		array += (i == 0 ? "" : ",") + std::string(buf);
	}
	array += "]";
	const auto load = [&](uint32_t schema, rack::LoadReport* report) {
		rack::RackPatch patch;
		patch.present = true;
		rack::RackPatch::Device d;
		d.tag = rack::patchTagOf(p.traits.tag);
		d.chain = 1;
		d.instanceId = 7;
		rack::valuesOfRow(p, defaults, defaults.size(), &d);
		d.extra = { rack::RackPatch::Value { p.traits.extraStateKey, rack::RackPatch::Value::Kind::Raw, 0.0f, array } };
		d.schema = schema;
		patch.device.push_back(d);
		std::string text;
		REQUIRE(rack::rackToText(patch, &text));
		CHECK(text.find("\"schema\": " + std::to_string(schema)) != std::string::npos);
		rack::RackPatch parsed;
		REQUIRE(rack::parseRackText(text, &parsed));
		REQUIRE(parsed.device.size() == 1);
		std::vector<float> fields(p.params.size() + p.traits.extraState, 9.0f);
		fields.resize(rack::rowOfValues(p, parsed.device[0], fields, report, "Phaser/Flanger", nullptr));
		return fields;
	};
	rack::LoadReport report;
	const std::vector<float> fields = load(1, &report);
	CHECK(report.countOf(Kind::ValueCoerced) == 0); // the expected migration is not a coercion
	CHECK(report.countOf(Kind::NewerSchema) == 0);
	REQUIRE(fields.size() == ph::kTravelFirst + 30);
	// INTO A DEVICE whose travel 30..33 was live: the first thirty bit for bit, the four absent ones exactly 0.
	alignas(ph::Phaser) unsigned char storage[sizeof(ph::Phaser)];
	ph::kAudioOps.construct(storage);
	ph::kAudioOps.prepare(storage, 48000.0, 4096);
	std::vector<float> live(ph::kTravelFirst + 34, 0.0f);
	std::copy(fields.begin(), fields.begin() + static_cast<long>(ph::kTravelFirst), live.begin());
	for (size_t i = 0; i < 34; ++i) {
		live[ph::kTravelFirst + i] = 0.4f;
	}
	ph::kAudioOps.setFields(storage, live.data(), live.size());
	ph::kAudioOps.setFields(storage, fields.data(), fields.size());
	const auto& dev = *reinterpret_cast<const ph::Phaser*>(storage);
	for (size_t i = 0; i < 30; ++i) {
		CAPTURE(i);
		CHECK(std::bit_cast<uint32_t>(dev.target().travel[i]) == std::bit_cast<uint32_t>(old[i]));
	}
	for (size_t i = 30; i < 34; ++i) {
		CAPTURE(i);
		CHECK(std::bit_cast<uint32_t>(dev.target().travel[i]) == 0u);
	}
	ph::kAudioOps.destruct(storage);
	// THE SAME SHORT BLOB CLAIMING THE CURRENT SCHEMA IS NOT A MIGRATION, and is still said.
	rack::LoadReport current;
	(void)load(2, &current);
	CHECK(current.countOf(Kind::ValueCoerced) == 1);
	// AND A FILE THAT DID NOT SAY ITS SCHEMA (0) is not assumed older.
	rack::LoadReport unsaid;
	(void)load(0, &unsaid);
	CHECK(unsaid.countOf(Kind::ValueCoerced) == 1);
}

TEST_CASE("Delay: a patch saved with Mode BBD loads as Buckets — the rename's ChoiceAlias, through the keyed load road") {
	using Kind = rack::LoadReport::Kind;
	namespace dl = fx::delay;
	const device::Descriptor& d = dl::kDescriptor;
	const size_t mode = indexOfKey(d.params, "delay_mode");
	REQUIRE(mode < d.params.size());
	rack::RackPatch::Device saved;
	rack::valuesOfRow(d, std::vector<float>(d.params.size(), 0.0f), 0, &saved);
	// AS AN OLDER BUILD WROTE IT: the label "BBD", which no longer names any choice.
	bool replaced = false;
	for (rack::RackPatch::Value& v : saved.values) {
		if (v.key == "delay_mode") {
			v = rack::RackPatch::Value::label("delay_mode", "BBD");
			replaced = true;
		}
	}
	REQUIRE(replaced);
	std::vector<float> row(d.params.size() + d.traits.extraState);
	rack::LoadReport report;
	(void)rack::rowOfValues(d, saved, row, &report, "Delay", nullptr);
	CHECK(row[mode] == static_cast<float>(static_cast<int>(dl::Mode::Bbd)));
	CHECK(std::string_view(d.params[mode].names[labelIndex(d.params[mode], row[mode])]) == "Buckets");
	CHECK(report.countOf(Kind::LabelUnknown) == 0);
}
