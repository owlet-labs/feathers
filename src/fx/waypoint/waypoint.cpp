/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

#include "fx/filter/multimode_filter.h"
#include "fx/waypoint/waypoint_types.h"

#include <algorithm>
#include <new>

// THE WAYPOINT FILTER'S RENDER PATH — the device's phase 2.
//
// THERE IS NO NEW DSP HERE AND THAT IS THE WHOLE POINT. `settingsAtMorph` returns the FILTER's own
// `Settings`, so this file hands an existing `MultimodeFilter` an existing settings type. The device
// is its interpolation; the audio is somebody else's, already written and already tested.

namespace feathers::fx::waypoint {

namespace {

struct OpsWaypoint {
	fx::filter::MultimodeFilter dsp;
	Settings state {};
	device::NoteContext note {};

	/// LATCH THE WHOLE STATE, THEN RECOMPUTE — the order-independence `registry.h` requires of any
	/// device with more than one input road. `setFields` is the only road here today, but the rule is
	/// the contract's and not this device's to relax.
	void refresh() {
		// THE MORPH POSITION ARRIVES IN THE ROW, ALREADY MODULATED. `Synth` applies a destination's
		// offset to the row BEFORE calling setFields (synth.cpp:531), so the value read here is the
		// resolved one — which is why this device needs no modulation road of its own and why Morph
		// being the sole modulatable control costs nothing at render time.
		fx::filter::Settings s = settingsAtMorph(state, static_cast<double>(state.morph));
		// KEYTRACK IS AN OFFSET AND NOTHING ELSE — just track offset? The filter adds
		// `keyOffsetSemitones / 12` to `cutoffLog2Hz`, the same log domain the waypoints live in, so
		// applying it AFTER the interpolation shifts every waypoint by the same interval. **The shape
		// a hand drew in the plane transposes rather than distorting**, which is the reason this is
		// applied here and not folded into each waypoint before interpolating: doing it per waypoint
		// would give the same answer only because the transform is affine, and would stop being true
		// the day anything about the traversal is not.
		//
		// **ON THE BUS IT FOLLOWS THE HOST'S ONE NOTE** — the filter's rule (`OpsFilter::refresh`), restated
		// because it is a property of what keytrack MEANS rather than of that device: per voice, the
		// voice's note; on a summed bus, the host's collapsed note or an upstream Tracker's. It used to require Poly.
		s.keyOffsetSemitones =
		    state.keytrack
		        ? static_cast<float>(static_cast<double>(note.pitchSemitones)
		                             - static_cast<double>(fx::filter::kKeytrackReferenceKey))
		        : 0.0f;
		dsp.setTarget(s);
	}
};

OpsWaypoint& asWaypoint(void* storage) { return *static_cast<OpsWaypoint*>(storage); }

} // namespace

const fx::filter::Settings& targetForTest(const void* storage) {
	return static_cast<const OpsWaypoint*>(storage)->dsp.target();
}

const device::AudioOps kAudioOps {
    .stateBytes = sizeof(OpsWaypoint),
    .stateAlign = alignof(OpsWaypoint),
    // CONSTRUCTED INTO STORAGE THE HOST OWNS, because the host is the one under
    // no-allocation-after-activate. The device says how big it is; where it lives is not its business.
    .construct = [](void* storage) { new (storage) OpsWaypoint(); },
    .destruct = [](void* storage) { asWaypoint(storage).~OpsWaypoint(); },
    .prepare = [](void* storage,
                  double sampleRate, int) { asWaypoint(storage).dsp.prepare(sampleRate); },
    // RESTART, NOT reset — `AudioOps::reset` means "as if just switched on", and MultimodeFilter
    // draws the distinction itself: reset() forgets the delay line, restart() also forgets the device
    // was ACTIVE, so the next block snaps its coefficients instead of ramping in from wherever the
    // morph last left them. The filter device learned this the hard way; taking the weaker meaning
    // here would be re-earning its lesson.
    .reset = [](void* storage) { asWaypoint(storage).dsp.restart(); },
    .setFields =
        [](void* storage, const float* fields, size_t fieldsPresent) {
	        // THE DEVICE DECODES ITS OWN ROW, which is what puts absent fields at their DECLARED
	        // defaults rather than at zero — device_row.h's rule, arriving as a bare pointer and a
	        // count because the device vocabulary cannot see the host's DeviceRow.
	        Row row {};
	        const size_t take = std::min(fieldsPresent, row.size());
	        for (size_t i = 0; i < take; ++i) {
		        row[i] = fields[i];
	        }
	        asWaypoint(storage).state = stateOf(row, take);
	        asWaypoint(storage).refresh();
        },
    // THE NOTE ROAD, because this device DOES keytrack. Latch and recompute rather than assuming an
    // order: `registry.h` is explicit that fields and notes may arrive in either order, and that
    // fields arrive far less often — so each road latches its own input and re-derives the target.
    .setNoteContext =
        [](void* storage, const device::NoteContext& note) {
	        asWaypoint(storage).note = note;
	        asWaypoint(storage).refresh();
        },
    .processStereo =
        [](void* storage, float* left, float* right, int frames) {
	        asWaypoint(storage).dsp.processStereo(left, right, frames);
        },
    // THE MONO ROAD, DECLARED RATHER THAN LEFT TO CONVENTION. `registry.h:183`: a host holding a mono
    // buffer for a device with no mono road knows to give it two channels instead of hoping — and
    // MultimodeFilter DEREFERENCES a null right channel, so the alternative here is not degraded
    // audio, it is a crash on the per-voice path.
    .processMono =
        [](void* storage, float* mono, int frames) {
	        asWaypoint(storage).dsp.processMono(mono, frames);
        },
    .holdsTail = [](const void* storage) { return static_cast<const OpsWaypoint*>(storage)->dsp.holdsTail(); },
};

} // namespace feathers::fx::waypoint
