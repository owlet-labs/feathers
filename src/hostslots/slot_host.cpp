/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without
 * even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not,
 * see <https://www.gnu.org/licenses/>.
 */

#include "hostslots/slot_host.h"

#include "device/format_value.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace feathers::hostslots {

namespace {

/// A `TextView` over a `TextSite`: its fields, no readings (the DAW's text is not a meter's), and its surface.
device::TextView viewOf(const TextSite& site) noexcept {
	return device::TextView {
		.host = &site,
		.field = [](const void* host, size_t control) {
			const auto& s = *static_cast<const TextSite*>(host);
			return control < s.fields.size() ? s.fields[control] : 0.0f;
		},
		.readingCount = [](const void*) { return size_t { 0 }; },
		.reading = [](const void*, size_t) { return 0.0f; },
		.surface = site.surface,
	};
}

/// A HOOK THAT PRINTS AND READS BACK — only those reach the DAW, so the host never shows words it cannot type in.
bool speaksBoth(const TextSite& site) noexcept {
	return site.text != nullptr && site.text->format != nullptr && site.text->parse != nullptr;
}

} // namespace

bool SlotHost::resolve(SlotIndex n, const device::ParamDesc** desc, double* value) const noexcept {
	if (n >= pool_.capacity() || pool_.state(n) == SlotState::Empty) {
		return false;
	}
	return lookup_.lookup(pool_.target(n), desc, value) && *desc != nullptr;
}

bool SlotHost::info(SlotIndex n, clap_param_info* info) const noexcept {
	if (n >= kDeclaredSlots || info == nullptr) {
		return false;
	}
	info->id = slotId(n);
	info->flags = CLAP_PARAM_IS_AUTOMATABLE | CLAP_PARAM_IS_MODULATABLE;
	info->cookie = nullptr;
	const device::ParamDesc* desc = nullptr;
	double value = 0.0;
	// A SLOT WHOSE TARGET DOES NOT RESOLVE READS AS EMPTY. The pool is reconciled at every load (plan §0.7),
	// so this is the window between a removal and the next main-thread visit, and a name for a control that
	// is not there would be a name for nothing.
	if (resolve(n, &desc, &value) && !pool_.target(n).label.empty()) {
		std::snprintf(info->name, sizeof(info->name), "%s", pool_.target(n).label.c_str());
	}
	else {
		std::snprintf(info->name, sizeof(info->name), "Slot %u", static_cast<unsigned>(n + 1));
		info->flags |= CLAP_PARAM_IS_HIDDEN;
	}
	std::snprintf(info->module, sizeof(info->module), "%s", kSlotModule);
	info->min_value = 0.0;
	info->max_value = 1.0;
	info->default_value = 0.0;
	return true;
}

bool SlotHost::value(clap_id id, double* value) const noexcept {
	const SlotIndex n = slotOfId(id);
	if (n == kNoSlot || value == nullptr) {
		return false;
	}
	const device::ParamDesc* desc = nullptr;
	double current = 0.0;
	*value = resolve(n, &desc, &current) ? device::toNormal(*desc, current) : 0.0;
	return true;
}

bool SlotHost::valueToText(clap_id id, double value, char* display, uint32_t size) const noexcept {
	const SlotIndex n = slotOfId(id);
	if (n == kNoSlot || display == nullptr || size == 0) {
		return false;
	}
	const device::ParamDesc* desc = nullptr;
	double current = 0.0;
	if (!resolve(n, &desc, &current)) {
		std::snprintf(display, size, "%.3f", value);
		return true;
	}
	// formatValue builds a std::string; this is the main thread (value_to_text is [main-thread]).
	try {
		// THE DEVICE FIRST (rack stage 2, §3): the Rack's position says which chain it lands on.
		TextSite site;
		if (lookup_.textSite(pool_.target(n), &site) && speaksBoth(site)) {
			char own[64] = {};
			if (site.text->format(site.control, device::fromNormal(*desc, value), viewOf(site), own, sizeof(own))) {
				std::snprintf(display, size, "%s", own);
				return true;
			}
		}
		const std::string text = device::formatValue(*desc, device::fromNormal(*desc, value));
		std::snprintf(display, size, "%s", text.c_str());
	}
	catch (...) {
		return false;
	}
	return true;
}

bool SlotHost::textToValue(clap_id id, const char* display, double* value) const noexcept {
	const SlotIndex n = slotOfId(id);
	if (n == kNoSlot || display == nullptr || value == nullptr) {
		return false;
	}
	const device::ParamDesc* desc = nullptr;
	double current = 0.0;
	if (!resolve(n, &desc, &current)) {
		return false;
	}
	// `valueToText`'s INVERSE (backlog LS): the target's own unit, read as the DAW showed it — a bare number in
	// the unit the control is showing at `current` — then back to the slot's 0..1. Main thread, like its pair.
	try {
		TextSite site;
		if (lookup_.textSite(pool_.target(n), &site) && speaksBoth(site)) {
			double own = 0.0;
			if (site.text->parse(site.control, display, viewOf(site), &own)) {
				*value = device::toNormal(*desc, std::clamp(own, desc->min, desc->max));
				return true;
			}
		}
		const std::optional<double> parsed = device::parseValue(*desc, display, current);
		if (!parsed) {
			return false;
		}
		*value = device::toNormal(*desc, *parsed);
	}
	catch (...) {
		return false;
	}
	return true;
}

bool emitSlotEvent(const clap_output_events* out, SlotEventKind kind, uint16_t slot, float value01) noexcept {
	return emitParamEvent(out, kind, slotId(slot), value01);
}

bool emitParamEvent(const clap_output_events* out, SlotEventKind kind, clap_id id, float value01) noexcept {
	if (out == nullptr) {
		return false;
	}
	if (kind == SlotEventKind::Value) {
		clap_event_param_value event = {};
		event.header.size = sizeof(event);
		event.header.time = 0;
		event.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
		event.header.type = CLAP_EVENT_PARAM_VALUE;
		event.param_id = id;
		// Addressed to the whole plugin rather than to a voice: a device control is not per voice here.
		event.note_id = -1;
		event.port_index = -1;
		event.channel = -1;
		event.key = -1;
		event.value = static_cast<double>(value01);
		return out->try_push(out, &event.header);
	}
	clap_event_param_gesture event = {};
	event.header.size = sizeof(event);
	event.header.time = 0;
	event.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
	event.header.type = kind == SlotEventKind::Begin ? CLAP_EVENT_PARAM_GESTURE_BEGIN : CLAP_EVENT_PARAM_GESTURE_END;
	event.param_id = id;
	return out->try_push(out, &event.header);
}

void drainOutbox(SlotOutbox<>& outbox, const clap_output_events* out) noexcept {
	outbox.drain([out](SlotEventKind kind, uint16_t slot, float value01) noexcept {
		return emitSlotEvent(out, kind, slot, value01);
	});
}

clap_param_rescan_flags SlotHost::rescanOwed() {
	const std::vector<bool> renamed = pool_.takeRenamed();
	for (const bool r : renamed) {
		if (r) {
			// AND VALUES (backlog KO): a slot that changed hands now reads another control's value, and a host that
			// cached the slot while it was Empty holds 0.0 for it until it is told to read it again.
			return CLAP_PARAM_RESCAN_VALUES | CLAP_PARAM_RESCAN_INFO | CLAP_PARAM_RESCAN_TEXT;
		}
	}
	return 0;
}

} // namespace feathers::hostslots
