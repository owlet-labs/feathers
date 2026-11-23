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

// EVERY TRIG PAD STRIKES ON EVERY CLICK, HOWEVER SHORT. A hand's writes reach the engine as record snapshots adopted one per block, so a press and
// release inside one block, written as plain values, leave a device that acts on the Trig's RISE nothing to read. The pad lets go through the
// release road (a fall waits until a block has read the rise), and this holds it for every device that has a pad: through the pad itself, over a
// real bridge and engine with blocks running, counted by the device's own fire seam (`ModOps::trigFires`).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "fx/noise/noise_types.h"
#include "mod/dahdsr/dahdsr_types.h"
#include "mod/lfo/lfo_types.h"
#include "mod/mseg/mseg_types.h"
#include "mod/rtrig/rtrig_types.h"
#include "mod/samplehold/samplehold_types.h"
#include "mod/stepseq/stepseq_types.h"
#include "persist_rig.h"
#include "plugin/device_registry.h"
#include "plugin/ui/device_control.h"
#include "plugin/ui/trig_pad.h"
#include "rack_endpoints.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

using namespace feathers;

namespace {

struct Row {
	const char* tag;
	size_t trig;
	/// How many times the device says the pad's Trig fired, out of its fire word.
	uint32_t (*fires)(uint32_t word);
	/// A SETTING WITHOUT WHICH THE TRIG IS NOT READ (Noise fires its Trig only under Impulses): control index and value, -1 for none.
	int setting = -1;
	float settingValue = 0.0f;
};

uint32_t whole(uint32_t word) { return word; }
uint32_t advanceByte(uint32_t word) { return (word >> mod::stepseq::kFireAdvanceShift) & mod::stepseq::kFireAdvanceMask; }

std::vector<Row> rows() {
	return {
		{ mod::dahdsr::kTraits.tag, static_cast<size_t>(mod::dahdsr::Param::Trig), whole },
		{ mod::mseg::kTraits.tag, static_cast<size_t>(mod::mseg::Param::Trig), whole },
		{ mod::lfo::kTraits.tag, static_cast<size_t>(mod::lfo::Param::Trig), whole },
		{ mod::samplehold::kTraits.tag, static_cast<size_t>(mod::samplehold::Param::Trig), whole },
		{ mod::rtrig::kTraits.tag, static_cast<size_t>(mod::rtrig::Param::Trig), whole },
		{ fx::noise::kTraits.tag, static_cast<size_t>(fx::noise::Param::Trig), whole, static_cast<int>(fx::noise::Param::Source),
		  static_cast<float>(fx::noise::Source::Impulses) },
		{ mod::stepseq::kTraits.tag, static_cast<size_t>(mod::stepseq::Param::Trig), advanceByte },
	};
}

struct Bench {
	persist::Rig rig;
	const device::Descriptor* desc = nullptr;
	explicit Bench(const Row& row) {
		desc = deviceForTag(row.tag);
		REQUIRE(desc != nullptr);
		std::vector<float> defaults;
		for (const device::ParamDesc& p : desc->params) {
			defaults.push_back(static_cast<float>(p.def));
		}
		if (row.setting >= 0) {
			defaults[static_cast<size_t>(row.setting)] = row.settingValue;
		}
		rig.seat(row.tag, defaults);
		rig.block();
	}
	uint32_t fires(const Row& row) {
		REQUIRE(desc->mod != nullptr);
		REQUIRE(desc->mod->trigFires != nullptr);
		return row.fires(desc->mod->trigFires(rig.storage()));
	}
	/// The main thread's visit (a held fall lands once the engine has adopted its rise) between blocks.
	void settle(int rounds = 8) {
		for (int i = 0; i < rounds; ++i) {
			(void)rig.bridge->serviceHostSlots();
			rig.block();
		}
	}
};

} // namespace

TEST_CASE("every Trig pad: a click quicker than a block fires exactly once, a slow click once, and a routed trigger is unchanged") {
	for (const Row& row : rows()) {
		CAPTURE(std::string(row.tag));
		for (const int between : { 0, 1, 20 }) {
			CAPTURE(between);
			Bench b(row);
			ui::DeviceControl control(*b.rig.bridge, b.rig.id, *b.desc, row.trig);
			ui::TrigPad pad(control, [](uint32_t&) { return false; });
			const uint32_t before = b.fires(row);
			pad.press();
			for (int i = 0; i < between; ++i) {
				b.rig.block();
			}
			pad.release();
			b.settle();
			CHECK(b.fires(row) - before == 1u);
			// AND THE TRIG ENDS AT REST, the record's and the pad's.
			CHECK(b.rig.bridge->fieldAt(b.rig.id, row.trig) == 0.0f);
			CHECK_FALSE(pad.lit());
		}
		// A BURST OF FIVE FAST TAPS, a main-thread visit and a block between taps: five fires.
		{
			Bench b(row);
			ui::DeviceControl control(*b.rig.bridge, b.rig.id, *b.desc, row.trig);
			ui::TrigPad pad(control, [](uint32_t&) { return false; });
			const uint32_t before = b.fires(row);
			for (uint32_t tap = 1; tap <= 5; ++tap) {
				pad.press();
				pad.release();
				b.settle(4);
				CHECK(b.fires(row) - before == tap);
			}
		}
		// A ROUTED TRIGGER INTO THE SAME PARAM IS UNCHANGED: a macro through a route, one fire per rise (a bare engine, as the other route cases
		// build it: a bridge's drain does not carry a route set on the engine).
		{
			rack::RackEngine engine(persist::Rig::kAxes, hostCatalogue());
			const size_t lane = engine.addDevice(row.tag);
			REQUIRE(lane != rack::RackEngine::kNoSlot);
			engine.prepare(48000.0, 4096);
			if (row.setting >= 0) {
				engine.setField(lane, static_cast<size_t>(row.setting), row.settingValue);
			}
			engine.setRouteAmount(test::macro(0), test::controlOf(engine, lane, row.trig), 1.0f);
			const device::Descriptor* desc = deviceForTag(row.tag);
			const auto fires = [&] { return row.fires(desc->mod->trigFires(engine.slotAtLane(lane).storage.get())); };
			std::array<float, 64> l {};
			std::array<float, 64> r {};
			const auto run = [&](float macro) {
				engine.setMacro(0, macro);
				engine.processStereo(l.data(), r.data(), 64, backplane::unknownBlock());
			};
			run(0.0f);
			const uint32_t before = fires();
			for (uint32_t rise = 1; rise <= 3; ++rise) {
				run(1.0f);
				run(1.0f);
				run(0.0f);
				run(0.0f);
				CHECK(fires() - before == rise);
			}
		}
	}
}
