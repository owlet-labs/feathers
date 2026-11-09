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

#include "backplane/sources.h"
#include "fx/filter/filter_types.h"
#include "mod/dahdsr/dahdsr_types.h"

#include <array>
#include <cstring>

// SOURCE ENUMERATION — one endpoint, one answer, whether the source is the host's or a device's.
//
// THE CASES ARE MOSTLY ABOUT nullptr, and that is the right emphasis rather than a thin test. Three
// genuinely different situations answer "nothing publishes this" — a host index nobody described, a
// device the host does not hold, and an output its device does not declare — and every one of them is a
// row a surface would otherwise draw as a source that cannot be routed from. That is the honesty rule in
// the source axis, and it is the same failure the destination axis's slack columns were.
//
// **BY ENDPOINT SINCE KILL-LANES STEP 2.** These cases asked by flat index over a lane table; a source
// names a device by its id now, so the host half and the device half cannot be confused by an index
// running past one into the other — the case that guarded exactly that is replaced below by the one that
// says why it cannot happen.

using namespace feathers::backplane;
namespace filter = feathers::fx::filter;
namespace dahdsr = feathers::mod::dahdsr;
using feathers::DeviceInstanceId;
using feathers::device::SourceDesc;

namespace {

/// FOUR DESCRIBED CORE SOURCES. A host may reserve more rows than it describes — Feathers' weight rows —
/// and the rows past the span are the normal case rather than a broken one.
constexpr std::array<SourceDesc, 4> kCore {
	SourceDesc { "macro_1", "Macro 1", false },
	SourceDesc { "macro_2", "Macro 2", false },
	SourceDesc { "macro_3", "Macro 3", true },
	SourceDesc { "macro_4", "Macro 4", false },
};

/// Device 11 is a filter (publishes no outputs); device 12 a DAHDSR (publishes some); 13 is nobody.
constexpr DeviceInstanceId kFilter = 11;
constexpr DeviceInstanceId kEnvelope = 12;
constexpr DeviceInstanceId kNobody = 13;
struct Devices {
	DeviceView view() const {
		return { this,
			     [](const void*, DeviceInstanceId id) -> const feathers::device::Descriptor* {
				     return id == kFilter ? &filter::kDescriptor : id == kEnvelope ? &dahdsr::kDescriptor : nullptr;
			     },
			     // THE ENVELOPE IS DOWNSTREAM OF THE FILTER, so a route BACK from the envelope to the filter
			     // would close a cycle and one forward would not. Stated as the relation itself since
			     // kill-lanes step 4b, where the rule stopped being a comparison of two ranks.
			     [](const void*, Source s, Destination d) {
				     return s.isDevice() && d.isDevice() && !(s.device == kFilter && d.device == kEnvelope);
			     } };
	}
};

} // namespace

TEST_CASE("a described core source answers its own descriptor") {
	const Devices devices;
	const SourceView sources(kCore, devices.view());

	for (size_t i = 0; i < kCore.size(); ++i) {
		CAPTURE(i);
		const Source s = Source::core(i);
		REQUIRE(sources.descOf(s) == &kCore[i]);
		REQUIRE(sources.isLive(s));
		REQUIRE(std::strcmp(sources.nameOf(s), kCore[i].name) == 0);
		REQUIRE(sources.isBipolar(s) == kCore[i].bipolar);
	}
}

TEST_CASE("a core row the host reserved but never described is not a source") {
	// Feathers' weight rows are exactly this: routable host rows with no descriptor published for them.
	const Devices devices;
	const SourceView sources(kCore, devices.view());
	for (size_t i = kCore.size(); i < kCore.size() + 3; ++i) {
		CAPTURE(i);
		const Source s = Source::core(i);
		REQUIRE(sources.descOf(s) == nullptr);
		REQUIRE_FALSE(sources.isLive(s));
		REQUIRE(sources.nameOf(s) == nullptr);
		REQUIRE_FALSE(sources.isBipolar(s));
	}
}

TEST_CASE("a device output answers the device's own declaration, and an output it lacks does not") {
	const Devices devices;
	const SourceView sources(kCore, devices.view());
	const size_t declared = dahdsr::kDescriptor.outputCount();
	REQUIRE(declared > 0);

	for (size_t output = 0; output < declared + 2; ++output) {
		CAPTURE(output);
		const Source s = Source::output(kEnvelope, output);
		if (output < declared) {
			REQUIRE(sources.descOf(s) == &dahdsr::kDescriptor.outputs[output]);
			REQUIRE(sources.nameOf(s) != nullptr);
		} else {
			REQUIRE(sources.descOf(s) == nullptr);
		}
		REQUIRE(sources.isLive(s) == (output < declared));
	}
}

TEST_CASE("a device that publishes nothing contributes no sources at all") {
	const Devices devices;
	const SourceView sources(kCore, devices.view());
	REQUIRE(filter::kDescriptor.outputCount() == 0);
	for (size_t output = 0; output < 4; ++output) {
		REQUIRE_FALSE(sources.isLive(Source::output(kFilter, output)));
	}
}

TEST_CASE("a device the host does not hold has no sources") {
	const Devices devices;
	const SourceView sources(kCore, devices.view());
	for (size_t output = 0; output < 4; ++output) {
		CAPTURE(output);
		REQUIRE_FALSE(sources.isLive(Source::output(kNobody, output)));
		REQUIRE(sources.nameOf(Source::output(kNobody, output)) == nullptr);
	}
}

TEST_CASE("a device's output never reads the host's rows, whatever its index") {
	// THE CASE THIS REPLACED guarded a flat index running off the end of the axis into the core span — a
	// device source falling to the core branch and answering a macro's descriptor. An endpoint says which
	// half it is, so the same numbers ask two different questions: host source 0 is Macro 1, and output 0
	// of a device that is not there is nothing.
	const Devices devices;
	const SourceView sources(kCore, devices.view());
	REQUIRE(sources.isLive(Source::core(0))); // the premise: index 0 of the core span answers
	REQUIRE(sources.descOf(Source::output(kNobody, 0)) == nullptr);
	REQUIRE(sources.descOf(Source::output(kFilter, 0)) == nullptr);
	// And a host endpoint never asks the devices: with no devices at all the core still answers.
	const SourceView bare(kCore, DeviceView {});
	REQUIRE(bare.isLive(Source::core(3)));
	REQUIRE_FALSE(bare.isLive(Source::output(kEnvelope, 0)));
}

TEST_CASE("isLive cannot disagree with descOf, because it is derived from it") {
	const Devices devices;
	const SourceView sources(kCore, devices.view());
	for (size_t i = 0; i < kCore.size() + 4; ++i) {
		CAPTURE(i);
		for (const Source s : { Source::core(i), Source::output(kFilter, i), Source::output(kEnvelope, i),
		                        Source::output(kNobody, i) }) {
			REQUIRE(sources.isLive(s) == (sources.descOf(s) != nullptr));
		}
	}
}

TEST_CASE("a default-constructed view publishes nothing and answers safely") {
	const SourceView none;
	for (size_t i = 0; i < 8; ++i) {
		CAPTURE(i);
		REQUIRE(none.descOf(Source::core(i)) == nullptr);
		REQUIRE_FALSE(none.isLive(Source::core(i)));
		REQUIRE_FALSE(none.isLive(Source::output(kEnvelope, i)));
	}
}
