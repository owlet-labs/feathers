/*
 * Copyright © 2026 Owlet Records
 * SPDX-License-Identifier: MIT
 *
 * This file is part of the Feathers Device SDK, which is MIT-licensed (see LICENSE in the SDK's
 * root) so that devices of any licence, open or closed, may build against it. Feathers itself,
 * which vendors this SDK, is GPL-3.0-or-later.
 */

// **THE EXAMPLE DEVICE, DRIVEN THROUGH ITS DESCRIPTOR ALONE** — the way a host drives it: storage of the declared
// size and alignment, the ops table's pointers, nothing named by type. Built against the SDK only.

#include "sine_mod.h"

#include "device/format_value.h"

#include <cmath>
#include <cstdio>
#include <iterator>
#include <string>

int main() {
	int failures = 0;
	auto expect = [&](bool ok, const char* what) {
		if (!ok) {
			std::fprintf(stderr, "FAIL: %s\n", what);
			++failures;
		}
	};

	const feathers::device::Descriptor& d = sine_mod::kDescriptor;
	const feathers::device::ModOps& ops = *d.mod;
	expect(d.audio == nullptr && d.outputs.size() == 1 && d.params.size() == sine_mod::kCount,
	       "a modulator with one output and two controls");

	alignas(std::max_align_t) unsigned char storage[256];
	expect(ops.stateBytes <= sizeof(storage) && ops.stateAlign <= alignof(std::max_align_t), "its state fits");
	ops.construct(storage);
	ops.prepare(storage, 48000.0);
	const float row[] = { 0.0f }; // Rate = log2(1 Hz); Depth absent, so it takes its default of 1
	ops.setFields(storage, row, std::size(row));

	// 1 Hz at 48 kHz in 64-sample ticks: tick i publishes sin(2π · 64i / 48000). Tick 187 is 0.25 - 0.00067 of a cycle.
	const feathers::device::ModTickContext context {};
	for (int i = 0; i <= 187; ++i) {
		ops.tick(storage, context);
	}
	const float value = ops.outputAt(storage, 0);
	expect(std::fabs(value - 1.0f) < 1.0e-3f, "a 1 Hz sine at full default depth peaks a quarter cycle in");

	const std::string rate = feathers::device::formatValue(d.params[sine_mod::kRate], 1.0);
	const std::string depth = feathers::device::formatValue(d.params[sine_mod::kDepth], 0.5);
	std::printf("%s: Rate %s, Depth %s, peak %.4f\n", d.traits.name, rate.c_str(), depth.c_str(), value);
	expect(!rate.empty() && !depth.empty(), "the host's formatter prints the device's own declarations");

	ops.destruct(storage);
	return failures == 0 ? 0 : 1;
}
