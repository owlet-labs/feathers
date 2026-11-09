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

// THE RESONATE BENCHMARK — what one Resonate costs per host block at its heaviest ordinary load: eight
// heads held, Size 256 (the FIR's longest), Bow up so the string never parks, a noise input, 48 kHz,
// 256-sample blocks. NOT A TEST: it asserts nothing and ctest never runs it (bench_render's posture).
//
// It runs the Ring and the Membrane (the dearest string) at Size 64 and 256, alternating, three
// repetitions, because this is a shared host whose load swings: take the medians and re-run rather than
// quoting a number from another day. "% of a core" is the block's time over the block's own duration.

#include "fx/resonate/resonate_dsp.h"
#include "fx/resonate/resonate_types.h"

#include <new>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <iterator>
#include <memory>
#include <vector>

namespace {

namespace resonate = feathers::fx::resonate;

volatile float g_sink = 0.0f;

double usPerBlock(int model, int size, float blend, float width, int chord) {
	constexpr int kBlock = 256;
	constexpr int kBlocks = 48000 * 20 / kBlock; // 20 s
	auto d = std::make_unique<resonate::ResonateDsp>();
	resonate::Settings s;
	s.model = model;
	s.size = size;
	s.bow = 0.5f;
	s.blend = blend;
	s.width = width;
	s.droneChord = chord;
	d->prepare(48000.0);
	d->setSettings(s);
	for (int k = 0; k < 8; ++k) {
		feathers::device::NoteEvent e;
		e.key = static_cast<int16_t>(48 + 3 * k);
		e.velocity = 0.8;
		d->queueNotes(&e, 1);
	}
	std::vector<float> l(kBlock);
	std::vector<float> r(kBlock);
	uint32_t seed = 1;
	auto fill = [&]() {
		for (int i = 0; i < kBlock; ++i) {
			seed = seed * 1664525u + 1013904223u;
			l[static_cast<size_t>(i)] = r[static_cast<size_t>(i)] = 0.2f * (static_cast<float>(seed >> 8) / 8388608.0f - 1.0f);
		}
	};
	for (int b = 0; b < 200; ++b) { // warm: heads open, string moving
		fill();
		d->process(l.data(), r.data(), kBlock);
	}
	double total = 0.0;
	for (int b = 0; b < kBlocks; ++b) {
		fill();
		const auto t0 = std::chrono::steady_clock::now();
		d->process(l.data(), r.data(), kBlock);
		total += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
	}
	g_sink = l[0];
	return total / kBlocks;
}


/// THE SAME LOAD THROUGH THE DEVICE'S OWN OPS (2026-10-05) — what a host calls since the fx lanes: the DSP with its lane
/// tap, the two lanes (EMPTY here, so each is a pass) and the ops' own mix, chunked at 256. Against `usPerBlock` (the
/// DSP alone, with no tap — what the ops were before the lanes) it is the lanes' cost when nobody uses them.
double usPerBlockOps(int model, int size, float blend, float width, int chord) {
	constexpr int kBlock = 256;
	constexpr int kBlocks = 48000 * 20 / kBlock;
	resonate::Settings s;
	s.model = model;
	s.size = size;
	s.bow = 0.5f;
	s.blend = blend;
	s.width = width;
	s.droneChord = chord;
	const resonate::Row row = resonate::rowOf(s);
	const feathers::device::AudioOps& audio = resonate::kAudioOps;
	void* mem = ::operator new(audio.stateBytes, std::align_val_t { audio.stateAlign });
	audio.construct(mem);
	audio.prepare(mem, 48000.0, 4096);
	audio.setFields(mem, row.data(), row.size());
	std::vector<feathers::device::NoteEvent> notes(8);
	std::vector<feathers::device::NoteEvent> passed(8);
	for (int k = 0; k < 8; ++k) {
		notes[static_cast<size_t>(k)].key = static_cast<int16_t>(48 + 3 * k);
		notes[static_cast<size_t>(k)].velocity = 0.8;
	}
	size_t count = 0;
	resonate::kNoteOps.processNotes(mem, notes.data(), notes.size(), passed.data(), passed.size(), &count);
	std::vector<float> l(kBlock);
	std::vector<float> r(kBlock);
	uint32_t seed = 1;
	auto fill = [&]() {
		for (int i = 0; i < kBlock; ++i) {
			seed = seed * 1664525u + 1013904223u;
			l[static_cast<size_t>(i)] = r[static_cast<size_t>(i)] = 0.2f * (static_cast<float>(seed >> 8) / 8388608.0f - 1.0f);
		}
	};
	for (int b = 0; b < 200; ++b) {
		fill();
		audio.processStereo(mem, l.data(), r.data(), kBlock);
	}
	double total = 0.0;
	for (int b = 0; b < kBlocks; ++b) {
		fill();
		const auto t0 = std::chrono::steady_clock::now();
		audio.processStereo(mem, l.data(), r.data(), kBlock);
		total += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
	}
	g_sink = l[0];
	audio.destruct(mem);
	::operator delete(mem, std::align_val_t { audio.stateAlign });
	return total / kBlocks;
}
} // namespace

int main() {
	struct Row {
		const char* name;
		int model;
		int size;
		float blend;
		float width;
		int chord;
	};
	// Blend 0.5 and Width 0.3 are the defaults; the last two rows are where the filter is unheard (Blend 0)
	// and where the two channels' kernels are one (Width 0).
	// THE DRONE: the default chord (Unison, one string), Sitar (six) and a raga (Bilawal, eight: every ear busy), mid-zone.
	constexpr int kUnison = 0;
	constexpr int kSitar = 7;
	constexpr int kBilawal = resonate::kFirstRaga; // Bilawal (Ionian)
	const Row rows[] = { { "Ring, Size 64", 0, 64, 0.5f, 0.3f, kUnison },          { "Ring, Size 256", 0, 256, 0.5f, 0.3f, kUnison },
		                 { "Membrane, Size 64", 3, 64, 0.5f, 0.3f, kUnison },      { "Membrane, Size 256", 3, 256, 0.5f, 0.3f, kUnison },
		                 { "Ring 256, Blend 0", 0, 256, 0.0f, 0.3f, kUnison }, { "Ring 256, Width 0", 0, 256, 0.5f, 0.0f, kUnison },
		                 { "Ring 256, Sitar", 0, 256, 0.5f, 0.3f, kSitar },
		                 { "Ring 256, Bilawal (8)", 0, 256, 0.5f, 0.3f, kBilawal } };
	constexpr int kRows = static_cast<int>(std::size(rows));
	constexpr int kReps = 3;
	double us[kReps][kRows] = {};
	for (int rep = 0; rep < kReps; ++rep) {
		for (int j = 0; j < kRows; ++j) {
			const int k = (j + rep) % kRows;
			us[rep][k] = usPerBlock(rows[k].model, rows[k].size, rows[k].blend, rows[k].width, rows[k].chord);
		}
	}
	const double blockUs = 256.0 / 48000.0 * 1e6;
	std::printf("eight heads held, Bow 0.5, noise in, 48 kHz, 256-sample blocks (%.0f us of audio each):\n", blockUs);
	for (int k = 0; k < kRows; ++k) {
		double v[kReps];
		for (int rep = 0; rep < kReps; ++rep) {
			v[rep] = us[rep][k];
		}
		std::sort(v, v + kReps);
		std::printf("%-20s us/block: %7.1f %7.1f %7.1f   median %7.1f   %.2f%% of a core\n", rows[k].name, us[0][k],
		            us[1][k], us[2][k], v[1], 100.0 * v[1] / blockUs);
	}
	// THE FX LANES' COST, A AGAINST B, ALTERNATED (a shared host): the DSP alone (the ops before the lanes) and the
	// device's own ops with both lanes empty, on the lightest and the heaviest rows.
	std::printf("\nthe fx lanes, empty (A = the DSP alone, B = the device's ops), medians of %d alternated:\n", kReps);
	for (const int k : { 0, kRows - 1 }) {
		double a[kReps];
		double b[kReps];
		for (int rep = 0; rep < kReps; ++rep) {
			const Row& w = rows[k];
			if (rep % 2 == 0) {
				a[rep] = usPerBlock(w.model, w.size, w.blend, w.width, w.chord);
				b[rep] = usPerBlockOps(w.model, w.size, w.blend, w.width, w.chord);
			}
			else {
				b[rep] = usPerBlockOps(w.model, w.size, w.blend, w.width, w.chord);
				a[rep] = usPerBlock(w.model, w.size, w.blend, w.width, w.chord);
			}
		}
		std::sort(a, a + kReps);
		std::sort(b, b + kReps);
		std::printf("%-22s A %7.1f us  B %7.1f us  B-A %+6.1f us (%+.1f%%)\n", rows[k].name, a[1], b[1], b[1] - a[1],
		            100.0 * (b[1] - a[1]) / a[1]);
	}
	return 0;
}
