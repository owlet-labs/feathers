/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even
 * the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
 * Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

#include "rack/device_publishers.h"

#include "device/registry.h"
#include "fx/automod/automod.h"
#include "fx/busutil/busutil_types.h"
#include "fx/owltt/owltt.h"
#include "backplane/axes.h"
#include "backplane/chunked_pool.h"
#include "rack/rack_engine.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <vector>

namespace feathers::rack {

void installDevicePublishers(RackEngine& engine) {
	// THE ENGINE IS CAPTURED AS AN ADDRESS, NOT AS A REFERENCE PARAMETER. `[&engine]` would capture
	// the local reference, which dies when this function returns while the connector outlives it —
	// a dangling capture, and the same lifetime class as the watcher bug fixed in b36e197.
	RackEngine* const target = &engine;

	/// **BUS UTILITY'S SEAM IS A FUNCTION POINTER AND A CONTEXT, SO THE SLOT CANNOT RIDE IN A
	/// CLOSURE** the way it does for the two devices below. It rides in here, and the array is owned
	/// by a `shared_ptr` the CONNECTOR captures — so it lives exactly as long as the thing that can
	/// still install callbacks pointing into it.
	///
	/// A LOCAL ARRAY WOULD BE THE DANGLING CAPTURE THIS FUNCTION'S OWN FIRST LINE WARNS ABOUT, one
	/// indirection along: `[&engine]` was that bug (b36e197), and a context pointing at a dead stack
	/// frame is the same mistake wearing a raw pointer. Sized once and never resized, so no install's
	/// address can be invalidated by a later one.
	struct ReadingsHop {
		RackEngine* engine = nullptr;
		size_t slot = 0;
	};
	// SIZED FROM THE ENGINE'S OWN AXES, which is what `readings_` is sized from — so a slot the
	// engine will accept a publish for always has a hop, and one it will not is refused above rather
	// than indexing past the end.
	// **A POOL, GROWN WITH THE ENGINE** (unbounded devices step 4): the engine tells its listeners on the main
	// thread each time it makes room, and every hop stays where it was — a device holds a pointer to its own.
	auto hops = std::make_shared<backplane::ChunkedPool<ReadingsHop>>(engine.requestedCapacity());
	engine.addGrowthListener([hops](size_t capacity) { hops->grow(capacity); });

	engine.setDspConnector([target, hops](const char* tag, size_t slot, void* dspStorage) {
		if (dspStorage == nullptr) {
			return;
		}
		if (device::sameTag(tag, fx::busutil::kTraits.tag)) {
			if (hops->at(slot) == nullptr) {
				return;
			}
			(*hops)[slot] = ReadingsHop { target, slot };
			fx::busutil::setReadingsCallback(
			    dspStorage,
			    [](void* context, const float* values, size_t count) {
				    auto* hop = static_cast<ReadingsHop*>(context);
				    hop->engine->publishReadings(hop->slot, values, count);
			    },
			    &(*hops)[slot]);
			return;
		}
		// AUTOMOD PUBLISHES ITS LFO PHASE, AND ITS ABSENCE HERE WAS A REAL BUG. while OWLTT, one arm below, animated correctly the
		// moment the rack grew a tick road. **THE TICK WAS NECESSARY AND NOT SUFFICIENT**:
		// `AutomodStrip::update()` redraws only when the phase it reads DIFFERS from the one it
		// drew, and with nothing published `readingCountAt` answered 0, so `phase()` returned a
		// constant 0.0f and the card was correct to report that nothing had changed.
		//
		// **THAT IS WHY THE TWO DEVICES FAILED DIFFERENTLY AND THE DIFFERENCE WAS THE EVIDENCE.**
		// `BridgeDeviceHost` has answered for both devices since automod was written; this
		// connector answered for one, and nothing made the two disagree out loud.
		if (device::sameTag(tag, fx::automod::kTraits.tag)) {
			auto* automod = static_cast<fx::automod::Automod*>(dspStorage);
			automod->setReadingsCallback([target, slot](float phase, float envelope) {
				// WRAPPED AND NaN-GUARDED AT THE PUBLICATION POINT, exactly as
				// `ParamBridge::publishAutomodPhase` does and for its stated reason: the value
				// crosses a thread boundary into a drawing that MULTIPLIES IT BY A WIDTH. The
				// guard is here rather than in `publishReadings` because that road is generic and
				// must not wrap OWLTT's decibels — a phase is a position on a circle and a dB is
				// not.
				// TWO READINGS: [0] the LFO phase, [1] the envelope follower's level.
				//
				// EACH GUARDED IN ITS OWN TERMS, which is why they are not guarded in a loop. A phase
				// is a position on a circle and WRAPS; a level is a magnitude and CLAMPS. Treating
				// them alike would either wrap a level of 1.2 to 0.2 — a full meter reading as nearly
				// empty — or clamp a phase and stick the marker at the end of its travel.
				const std::array<float, 2> safe {
				    std::isfinite(phase) ? (phase - std::floor(phase)) : 0.0f,
				    std::isfinite(envelope) ? std::clamp(envelope, 0.0f, 1.0f) : 0.0f,
				};
				target->publishReadings(slot, safe.data(), safe.size());
			});
			return;
		}
		if (!device::sameTag(tag, fx::owltt::kTraits.tag)) {
			return;
		}
		auto* owltt = static_cast<fx::owltt::Owltt*>(dspStorage);
		owltt->setMeterCallback([target, slot](size_t, const std::array<float, 3>& grDB,
		                                    const std::array<float, 3>& inputLevelDB,
		                                    const std::array<float, 3>& outputLevelDB,
		                                    bool clipping, const fx::owltt::Owltt::MeterExtras& extras) {
			// FLATTENED IN THE ORDER `OwlttGraph::meters` READS BACK, and those two are the only
			// places that know it — see `DeviceHost::readingAt`. The clip flag rides as a float
			// because the seam carries floats, and 0/1 says exactly as much as a bool. Fourteen since
			// backlog EL: a light per band (10..12) and the combined gain reduction in dB (13).
			const std::array<float, fx::owltt::Owltt::kReadingCount> values {
			    grDB[0],          grDB[1],          grDB[2],
			    inputLevelDB[0],  inputLevelDB[1],  inputLevelDB[2],
			    outputLevelDB[0], outputLevelDB[1], outputLevelDB[2],
			    clipping ? 1.0f : 0.0f,
			    extras.bandSaturating[0] ? 1.0f : 0.0f, extras.bandSaturating[1] ? 1.0f : 0.0f,
			    extras.bandSaturating[2] ? 1.0f : 0.0f, extras.combinedGrDb,
			    extras.secondChannelGr[0], extras.secondChannelGr[1], extras.secondChannelGr[2],
			    extras.noteHz }; // backlog LQ's three (14..16), and Key Track's note (17)
			target->publishReadings(slot, values.data(), values.size());
		});
	});
}

} // namespace feathers::rack
