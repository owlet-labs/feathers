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

#pragma once

#include "backplane/route_probe.h"
#include "device/device.h"
#include "device/endpoint.h"
#include "device/transfer.h"

#include <functional>
#include <optional>
#include <string>

namespace feathers::backplane {
class DeviceHost;
}

// **THE AS-MODULATED READOUT** (backlog PU: while modulation is being set on a target, a little box hovers near
// the destination reading out the as-modulated value — moving from Major to Minor with a toggle as the source, it
// shows "Minor").
//
// While a route's amount is being set — a ring drag, a value box's amount drag, a pill, the route list's amount, an
// armed assign — a small box floats beside the destination's face and prints what the device RECEIVES: the source's
// present reading times the amount, plus the base and every other route, landed by the engine's own rule, in the
// destination's own words ("Minor", "1.20 kHz", "64"). Under it, where the source can move, the range the route
// sweeps ("Major … Minor").
//
// **THE ENGINE'S ARITHMETIC, NOT A RESTATEMENT OF IT.** `asModulated` is `device::resolveRouted` over
// `before + device::routeTerm(amount, …) + after` — the two functions `RackEngine::resolveEar` calls — and every
// number but the amount comes from the engine (`backplane::RouteProbe`: the source's cell, the other inputs, the
// host's modulation, the span from the engine's table). So the readout equals what plays by construction, with the
// amount under the hand a block before the engine adopts it.
//
// THREE PIECES: this header (the arithmetic, the subject a control implements, and which route is live), the layer
// that draws the box (`mod_readout_layer.h`, one per card and per window, beside the tooltip layer), and the
// hooks — `DeviceControl`'s amount road and the route list's write — which tell `LiveAmount`. The layer finds the
// destination's face by asking the widgets (`ControlFace`), so no face carries a hook of its own.

namespace feathers::ui {

/// WHAT THE BOX SAYS, and the numbers behind it (the cases read the numbers).
struct ModReadout {
	float amount = 0.0f; ///< the route's amount the readout was made at
	double value = 0.0; ///< what the device receives now, in the control's own units
	double lo = 0.0;    ///< the lowest the route can carry it to, over what the source can say
	double hi = 0.0;    ///< and the highest
	std::string text;   ///< `value` in the destination's words
	std::string loText;
	std::string hiText;
	/// Whether the range says anything — false when the source cannot move the value (every reading lands it on
	/// one step, or the range stops it at an end both ways).
	[[nodiscard]] bool sweeps() const noexcept { return loText != hiText; }
};

/// **THE VALUE THE DEVICE RECEIVES**: `resolveRouted(base, before + routeTerm(amount, transfer, source, span) +
/// after)` — the engine's sum, in the engine's order, with this amount.
[[nodiscard]] double asModulated(const device::ParamDesc& p, double base, const backplane::RouteProbe& probe,
                                 float amount, device::Transfer transfer, float source) noexcept;

/// THE WHOLE READOUT: the value at the probe's source reading, and the range over what the source can say
/// (−1..1 bipolar, 0..1 unipolar — the curve's extremes lie at an end or at zero, so those three readings bound
/// it). `format` prints a value in the destination's words.
[[nodiscard]] ModReadout readoutFor(const device::ParamDesc& p, double base, const backplane::RouteProbe& probe,
                                    float amount, device::Transfer transfer, bool bipolar,
                                    const std::function<std::string(double)>& format);

/// **THE SAME FOR ONE OF FEATHERS' CORE PARAMETERS**, whose resolve is `ParamSet`'s, not the rack engine's: each
/// reading is `land(base + baseOffset + before + term + after)`, where `term` and `land` are the two functions
/// `ParamSet` calls (`feathers::coreRouteTerm`, `feathers::landCore`). Passed in rather than named here, so this
/// floor header stays free of the plugin's parameter table; `ParamControl` passes those two.
using CoreTerm = double (*)(float amount, device::Transfer transfer, double reading, bool macro, double span);
using CoreLand = std::function<double(double sum)>;
[[nodiscard]] ModReadout coreReadoutFor(double base, const backplane::RouteProbe& probe, float amount,
                                        device::Transfer transfer, bool bipolar, CoreTerm term, const CoreLand& land,
                                        const std::function<std::string(double)>& format);

/// **A CONTROL THAT CAN BE A ROUTE'S DESTINATION AND CAN SAY WHAT IT WOULD RECEIVE** — `DeviceControl` and `ParamControl`. The
/// layer matches a face to the live route by `readoutIdentity()` and `readoutDestination()`, and asks `routeReadout`.
class RouteReadoutSubject {
public:
	virtual ~RouteReadoutSubject() = default;
	/// Whose routing table the route is in (`DeviceHost::routeIdentity`).
	[[nodiscard]] virtual const void* readoutIdentity() const noexcept = 0;
	[[nodiscard]] virtual device::Destination readoutDestination() const noexcept = 0;
	/// The readout for the route from `source` into this control, or none — no route (amount 0), not a
	/// destination, or a host that cannot yet say what its engine reads (`DeviceHost::probeRoute`).
	[[nodiscard]] virtual std::optional<ModReadout> routeReadout(device::Source source) = 0;
	/// The box has gone: stop asking the engine.
	virtual void readoutEnded() {}
};

/// WHICH ROUTE IS BEING SET, OR WAS A MOMENT AGO. One for the process, on the UI thread: one hand sets one amount.
/// BY ENDPOINT AND HOST, NEVER BY A WIDGET POINTER — a control destroyed mid-linger leaves nothing dangling, and a
/// layer only ever dereferences a control it found in its own live tree.
struct LiveRoute {
	const void* host = nullptr; ///< the routing table's identity (`DeviceHost::routeIdentity`)
	device::Source source {};
	device::Destination destination {};
	bool held = false; ///< a gesture is open (between begin and end)
};

class LiveAmount {
public:
	/// How long the box stays after the hand lets go (or after a write with no gesture round it).
	static constexpr double kLingerMs = 1200.0;

	static void begin(const void* host, device::Source source, device::Destination destination);
	/// An amount was written — inside a gesture or not (the route list writes without one).
	static void touch(const void* host, device::Source source, device::Destination destination);
	static void end(const void* host, device::Source source, device::Destination destination);
	/// The live route, if a gesture is open or the last write was within `kLingerMs`.
	[[nodiscard]] static std::optional<LiveRoute> current();
	/// Forget it (a host going away; the cases).
	static void reset();
	/// The clock in milliseconds; steady by default, a case sets its own (null restores the default).
	static void setClockForTest(std::function<double()> clock);
	[[nodiscard]] static double now();
};

} // namespace feathers::ui
