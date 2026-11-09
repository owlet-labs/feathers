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

#include "plugin/ui/relative_macro.h"

#include "device/device.h"
#include "plugin/params.h"
#include "plugin/sound_gamma.h"
#include "plugin/sound_travel.h"
#include "plugin/ui/device_control.h" // DeviceValueEdit
#include "plugin/ui/param_edit.h"     // ParamValueEdit
#include "plugin/ui/stored_edit.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>

namespace feathers::ui {

namespace {

constexpr double kFullTurn = 2.0 * 3.14159265358979323846;

/// The sound whose Gamma PARAMETER this is, if it is one.
std::optional<size_t> gammaSlotOf(ParamId id) {
	for (size_t slot = 0; slot < kMaxSounds; ++slot) {
		if (soundParam(slot, SoundField::Gamma) == id) {
			return slot;
		}
	}
	return std::nullopt;
}

} // namespace

double relativeStep(double base, double step, double lo, double hi, RelativeRule rule) {
	const double v = base + step;
	switch (rule) {
		case RelativeRule::Unbounded: return v;
		case RelativeRule::Wrap: {
			const double span = hi - lo;
			if (!(span > 0.0)) {
				return lo;
			}
			double folded = std::fmod(v - lo, span);
			if (folded < 0.0) {
				folded += span;
			}
			return lo + folded;
		}
		case RelativeRule::Clamp:
		default: return std::clamp(v, lo, hi);
	}
}

RelativeMacroGesture::~RelativeMacroGesture() {
	// A GESTURE LEFT OPEN STILL CLOSES ITS HOST GESTURES — a DAW left believing a parameter is held is the
	// failure `ParamBridge`'s bracket comment names. It records nothing: there is no history to trust here.
	if (open_) {
		end(nullptr);
	}
}

void RelativeMacroGesture::collect(size_t macro) {
	targets_.clear();
	const backplane::Source source = backplane::Source::core(macro);
	struct Ctx {
		RelativeMacroGesture* self;
		backplane::Source source;
	} ctx { this, source };
	host_.forEachRouteFrom(source, &ctx, [](void* c, backplane::Destination d) {
		Ctx& x = *static_cast<Ctx*>(c);
		RelativeMacroGesture& self = *x.self;
		const double amount = static_cast<double>(self.host_.routeAmount(x.source, d));
		if (amount == 0.0) {
			return;
		}
		Target t;
		t.destination = d;
		if (d.isDevice()) {
			const device::Descriptor* desc = self.bridge_.descriptorOf(d.device);
			if (desc == nullptr || d.index >= desc->params.size()) {
				return;
			}
			const device::ParamDesc& p = desc->params[d.index];
			t.kind = Kind::Control;
			t.device = d.device;
			t.control = d.index;
			t.perUnit = amount * device::routeSpanOf(p);
			t.lo = p.min;
			t.hi = p.max;
			t.stepped = p.stepped;
			// AN ANGLE WRAPS; a drag window (±2^23) clamps only at an edge millions of units out, which is the
			// intended "just keeps going" without a second rule.
			t.rule = p.unit == device::Unit::Degrees && !p.stepped ? RelativeRule::Wrap : RelativeRule::Clamp;
		}
		else if (isInternalDestination(d.index)) {
			const size_t internal = d.index - kParamDestinationCount;
			const device::ParamDesc& p = internalDestinationDesc(internal);
			t.slot = internal / kInternalDestinationsPerSlot;
			const bool theta = internal % kInternalDestinationsPerSlot == static_cast<size_t>(TravelAxis::Theta);
			t.kind = theta ? Kind::Theta : Kind::Radius;
			t.perUnit = amount * device::routeSpanOf(p);
			// THETA WRAPS as its knob folds. RADIUS TURNS ALONG ITS FACE, 0..1 (radius-mod-jump, 2026-11-07) — the
			// same face a route into it moves along (`playedRadiusFace`), so `running` holds a FACE position for it
			// (`begin`/`turn`) and its floor and ceiling are the face's ends, which are its knob's.
			t.lo = theta ? -kFullTurn * 0.5 : 0.0;
			t.hi = theta ? kFullTurn * 0.5 : 1.0;
			t.rule = theta ? RelativeRule::Wrap : RelativeRule::Clamp;
		}
		else if (d.index < kParamDestinationCount) {
			const ParamId id = kModDestinations[d.index];
			const ParamDesc* p = findParam(static_cast<clap_id>(id));
			if (p == nullptr) {
				return;
			}
			// THE SAME SPAN THE ENGINE SCALES THE ROUTE BY (`ParamSet::sourceOffsetAt`): the declared range.
			t.perUnit = amount * (p->max - p->min);
			if (const std::optional<size_t> slot = gammaSlotOf(id)) {
				// GAMMA "JUST KEEPS GOING": carried into the unbounded coarse half, not the ±4 vernier,
				// so the sum the sound plays is not stopped by the parameter's range (to confirm).
				t.kind = Kind::GammaBase;
				t.slot = *slot;
				t.rule = RelativeRule::Unbounded;
			}
			else {
				t.kind = Kind::Param;
				t.id = static_cast<clap_id>(id);
				t.lo = p->min;
				t.hi = p->max;
				t.stepped = (p->flags & CLAP_PARAM_IS_STEPPED) != 0;
				// PHASE IS THE ONE PERIODIC CORE PARAMETER (its route already leaves the range, params.cpp).
				t.rule = id == ParamId::OscPhase ? RelativeRule::Wrap : RelativeRule::Clamp;
			}
		}
		else {
			return;
		}
		self.targets_.push_back(t);
	});
}

double RelativeMacroGesture::read(const Target& t) const {
	switch (t.kind) {
		case Kind::Param: return bridge_.value(t.id);
		case Kind::GammaBase: return bridge_.soundGamma().base[t.slot];
		case Kind::Radius: return bridge_.soundTravel().orbit[t.slot].radius;
		case Kind::Theta: return bridge_.soundTravel().orbit[t.slot].theta;
		case Kind::Control: return static_cast<double>(host_.fieldAt(t.device, t.control));
	}
	return 0.0;
}

void RelativeMacroGesture::write(const Target& t, double value) {
	switch (t.kind) {
		case Kind::Param: bridge_.setValue(t.id, value); return;
		case Kind::GammaBase: bridge_.setSoundGamma(t.slot, value); return;
		case Kind::Radius:
		case Kind::Theta: {
			PolarOffset orbit = bridge_.soundTravel().orbit[t.slot];
			(t.kind == Kind::Radius ? orbit.radius : orbit.theta) = value;
			bridge_.setSoundOrbit(t.slot, orbit);
			return;
		}
		case Kind::Control: host_.setField(t.device, t.control, static_cast<float>(value)); return;
	}
}

void RelativeMacroGesture::openHostGesture(const Target& t) {
	switch (t.kind) {
		case Kind::Param: bridge_.beginGesture(t.id); return;
		case Kind::GammaBase: bridge_.beginSoundGammaGesture(); return;
		case Kind::Radius:
		case Kind::Theta: bridge_.beginSoundTravelGesture(); return;
		case Kind::Control: host_.beginControlGesture(t.device, t.control); return;
	}
}

void RelativeMacroGesture::closeHostGesture(const Target& t) {
	switch (t.kind) {
		case Kind::Param: bridge_.endGesture(t.id); return;
		case Kind::GammaBase: bridge_.endSoundGammaGesture(); return;
		case Kind::Radius:
		case Kind::Theta: bridge_.endSoundTravelGesture(); return;
		case Kind::Control: host_.endControlGesture(t.device, t.control); return;
	}
}

void RelativeMacroGesture::begin(size_t macro) {
	if (open_) {
		end(nullptr);
	}
	collect(macro);
	for (Target& t : targets_) {
		t.before = read(t);
		t.running = t.kind == Kind::Radius ? radiusFace(t.before) : t.before;
		t.written = t.before;
		openHostGesture(t);
	}
	open_ = true;
}

void RelativeMacroGesture::turn(double delta) {
	if (!open_ || delta == 0.0 || !std::isfinite(delta)) {
		return;
	}
	for (Target& t : targets_) {
		t.running = relativeStep(t.running, t.perUnit * delta, t.lo, t.hi, t.rule);
		const double value = t.kind == Kind::Radius ? radiusAtFace(t.running)
		                     : t.stepped              ? std::round(t.running)
		                                              : t.running;
		if (value != t.written) {
			write(t, value);
			// READ BACK: the road may coerce (a parameter's sanitiser, a field's float), and the record and the
			// next comparison must hold what is stored, not what was asked for.
			t.written = read(t);
		}
	}
}

void RelativeMacroGesture::end(EditHistory* history) {
	if (!open_) {
		return;
	}
	open_ = false;
	for (const Target& t : targets_) {
		closeHostGesture(t);
	}
	if (history == nullptr) {
		targets_.clear();
		return;
	}
	// ONE STEP FOR THE WHOLE TURN, however many bases it moved — a group of one records as itself, a group
	// of none records nothing.
	history->beginGroup("Turn");
	for (const Target& t : targets_) {
		const double after = read(t);
		if (after == t.before) {
			continue;
		}
		switch (t.kind) {
			case Kind::Param:
				history->record(std::make_unique<ParamValueEdit>(bridge_, t.id, t.before, after, "Turn"));
				break;
			case Kind::Control:
				history->record(std::make_unique<DeviceValueEdit>(host_, t.device, t.control, t.before, after));
				break;
			case Kind::GammaBase: {
				ParamBridge& bridge = bridge_;
				const size_t slot = t.slot;
				history->record(std::make_unique<StoredEdit<double>>(
				    [&bridge, slot](const double& v) {
					    bridge.beginSoundGammaGesture();
					    bridge.setSoundGamma(slot, v);
					    bridge.endSoundGammaGesture();
				    },
				    t.before, after, "Turn"));
				break;
			}
			case Kind::Radius:
			case Kind::Theta: {
				ParamBridge& bridge = bridge_;
				const size_t slot = t.slot;
				const bool radius = t.kind == Kind::Radius;
				history->record(std::make_unique<StoredEdit<double>>(
				    [&bridge, slot, radius](const double& v) {
					    PolarOffset orbit = bridge.soundTravel().orbit[slot];
					    (radius ? orbit.radius : orbit.theta) = v;
					    bridge.beginSoundTravelGesture();
					    bridge.setSoundOrbit(slot, orbit);
					    bridge.endSoundTravelGesture();
				    },
				    t.before, after, "Turn"));
				break;
			}
		}
	}
	history->endGroup();
	targets_.clear();
}

} // namespace feathers::ui
