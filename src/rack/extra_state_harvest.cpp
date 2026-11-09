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

#include "rack/extra_state_harvest.h"

#include <bit>
#include <cstdint>

namespace feathers::rack {

namespace {

/// The device's declared basis, as a row index, or `kNone` for a device that publishes nothing.
constexpr size_t kNone = static_cast<size_t>(-1);

size_t basisField(const device::Descriptor* d) noexcept {
	if (d == nullptr || d->mod == nullptr || d->mod->publishExtraState == nullptr || d->mod->extraStateBasis < 0
	    || static_cast<size_t>(d->mod->extraStateBasis) >= d->traits.extraState) {
		return kNone;
	}
	return d->params.size() + static_cast<size_t>(d->mod->extraStateBasis);
}

float recordFloat(const DeviceState& row, size_t field) noexcept {
	return field < row.fieldsPresent && field < row.field.size() ? row.field[field] : 0.0f;
}

} // namespace

void stampExtraStateBases(ChainRecord* record) {
	if (record == nullptr) {
		return;
	}
	for (size_t chain = 0; chain < record->chainCount(); ++chain) {
		for (size_t position = 0; position < record->length(chain); ++position) {
			const DeviceInstanceId id = record->deviceAt(chain, position);
			const size_t field = basisField(record->descriptorOf(id));
			if (id != kNoInstance && field != kNone) {
				(void)record->setField(id, field, device::freshExtraStateBasis());
			}
		}
	}
}

bool harvestExtraState(ChainRecord* record, const device::ExtraStateSnapshot::Read& read) {
	if (record == nullptr || read.serial == 0 || read.device == kNoInstance || !record->holds(read.device)) {
		return false;
	}
	const device::Descriptor* d = record->descriptorOf(read.device);
	const size_t basis = basisField(d);
	if (basis == kNone || read.count != d->traits.extraState) {
		return false;
	}
	const DeviceState& row = record->rowOf(read.device);
	const size_t first = d->params.size();
	// A SNAPSHOT TAKEN BEFORE A HAND'S EDIT OR A LOAD carries the basis it replaced, and must not land over it.
	if (std::bit_cast<uint32_t>(recordFloat(row, basis)) != std::bit_cast<uint32_t>(read.values[basis - first])) {
		return false;
	}
	bool changed = false;
	for (size_t k = 0; k < read.count; ++k) {
		const float v = read.values[k];
		if (std::bit_cast<uint32_t>(recordFloat(row, first + k)) != std::bit_cast<uint32_t>(v)) {
			changed = record->setField(read.device, first + k, v) || changed;
		}
	}
	return changed;
}

} // namespace feathers::rack
