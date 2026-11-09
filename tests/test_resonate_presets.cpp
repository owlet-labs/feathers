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

#include "fx/resonate/resonate_dsp.h"
#include "fx/resonate/resonate_types.h"
#include "plugin/device_preset.h"
#include "plugin/device_registry.h"
#include "plugin/factory_presets.h"
#include "plugin/preset_store.h"
#include "rack/rack_bridge.h"
#include "rack/rack_engine.h"
#include "rack/rack_axes.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// RESONATE'S FACTORY PRESETS.
//
// **THE FILES UNDER `presets/factory/Devices/RESONATE/` ARE THE MASTERS (2026-10-06), AND THE TABLE FOLLOWS THEM.**
// All five were edited by ear and collected at b8480ed5, lane bodies and all. The table below is a
// transcription of their control values; it was the other way round until then (the files were written from it).
// Three things are asserted of what ships:
// it is exactly the table (by VALUE, every control, through `readDevicePreset`), it is tagged `factory` in the
// folder it sits in, and every preset stays under 0 dBFS at a typical input.

namespace rsn = feathers::fx::resonate;
namespace fs = std::filesystem;
using namespace feathers;

namespace {

constexpr const char* kBankPrefix = "Devices/RESONATE/";
/// A FIXED SAVE TIME, so rewriting the files from an unchanged table changes no byte (2026-10-05 00:00 UTC).
constexpr int64_t kSavedAt = 1791158400;

/// One control as a preset sets it, BY ITS KEY (as the file stores it): a number, or a Choice by its LABEL.
struct Set {
	const char* key;
	double value = 0.0;
	const char* label = nullptr;
};

struct FactoryPreset {
	const char* category;
	const char* name;
	std::vector<Set> sets;
};

/// THE FACTORY SET, AS IT SHIPS (2026-10-06). **TRANSCRIBED FROM THE COMMITTED FILES, which are now the masters:** all
/// five were edited by ear and collected at b8480ed5 (`presets/factory/Devices/RESONATE/`). Every
/// control value each file holds is here, by KEY, a Choice by its label; a control a file does not name takes its
/// default. To change a factory preset, change the FILE (save it from the plugin, collect it) and then this table to
/// match — never the other way round (see the writer case).
const std::vector<FactoryPreset>& factorySet() {
	static const std::vector<FactoryPreset> set {
		{ "Drones", "Tanpura",
		  {
		      { "resonate_enabled", 0, "On" },
		      { "resonate_model", 0, "Ring" },
		      { "resonate_character", 0.5 },
		      { "resonate_morph", 0 },
		      { "resonate_bow", 0 },
		      { "resonate_drive", 0.5 },
		      { "resonate_pluck_strength", 0.5 },
		      { "resonate_voicing", 0, "Poly" },
		      { "resonate_porta", -11 },
		      { "resonate_octave", 0 },
		      { "resonate_semitone", 0 },
		      { "resonate_fine", 0 },
		      { "resonate_drone_out", 0.5 },
		      { "resonate_drone_excite", 0.3 },
		      { "resonate_drone_attack", -4 },
		      { "resonate_drone_decay", 3.5849626 },
		      { "resonate_drone_chord_choice", 0, "Tanpura" },
		      { "resonate_drone_character", 0.5 },
		      { "resonate_drone_width", 0.7 },
		      { "resonate_root", 42.51429 },
		      { "resonate_size_taps", 64 },
		      { "resonate_width", 0.3 },
		      { "resonate_blend", 0.5 },
		      { "resonate_level", 0.4 },
		      { "resonate_mix", 1 },
		      { "resonate_sustain", 0 },
		      { "resonate_jawari", 0 },
		      { "resonate_bridge_curve", 0.6 },
		      { "resonate_sympathy_notes", 0.5425954 },
		      { "resonate_drone_voice", 0.07142857 },
		      { "resonate_drone_stiffness", 0.18095246 },
		      { "resonate_drone_body", 0.6 },
		      { "resonate_drone_twin", 0 },
		      { "resonate_drone_cycle", 0.5 },
		      { "resonate_drone_cycle_sync", 0, "1/2" },
		      { "resonate_head_release", 0, "Ring" },
		      { "resonate_drone_tuning", 0, "Just" },
		      { "resonate_ring_decay", 0 },
		      { "resonate_drone_cycle_level", 0.5 },
		      { "resonate_bounce", 1 },
		      { "resonate_tension", -2.309524 },
		      { "resonate_drone_input", 0.5 },
		      { "resonate_string", 0.5 },
		      { "resonate_string_excite", 0.5 },
		      { "resonate_drone_cycle_transport", 0, "Off" },
		      { "resonate_pickup", 0 },
		      { "resonate_bridge_fixed", 0 },
		      { "resonate_drone_pluck", 0.6380953 },
		      { "resonate_drone_jawari", 0.6809523 },
		      { "resonate_drone_bounce", 0.70952386 },
		      { "resonate_drone_sympathy", 0.8512668 },
		      { "resonate_drone_to_wet_fx", 0, "On" },
		      { "resonate_drone_thread", 0.023 },
		      { "resonate_drone_follow", 0, "On" },
		  } },
		{ "Drones", "Rudra Veena",
		  {
		      { "resonate_enabled", 0, "On" },
		      { "resonate_model", 0, "Ring" },
		      { "resonate_character", 0.49537045 },
		      { "resonate_morph", 0.7015099 },
		      { "resonate_bow", 0 },
		      { "resonate_drive", 0.36190465 },
		      { "resonate_pluck_strength", 0.5 },
		      { "resonate_voicing", 0, "Poly" },
		      { "resonate_porta", -11 },
		      { "resonate_octave", 0 },
		      { "resonate_semitone", 0 },
		      { "resonate_fine", 0 },
		      { "resonate_drone_out", 0.2814 },
		      { "resonate_drone_excite", 0 },
		      { "resonate_drone_attack", -2 },
		      { "resonate_drone_decay", 4 },
		      { "resonate_drone_chord_choice", 0, "Fifth + Octave" },
		      { "resonate_drone_character", 0.5 },
		      { "resonate_drone_width", 0.5 },
		      { "resonate_root", 36 },
		      { "resonate_size_taps", 64 },
		      { "resonate_width", 0.3 },
		      { "resonate_blend", 0 },
		      { "resonate_level", 0.268 },
		      { "resonate_mix", 1 },
		      { "resonate_sustain", 0 },
		      { "resonate_jawari", 0.62380946 },
		      { "resonate_bridge_curve", 0.19761917 },
		      { "resonate_sympathy_notes", 1 },
		      { "resonate_drone_voice", 0.2 },
		      { "resonate_drone_stiffness", 0.15 },
		      { "resonate_drone_body", 0.8 },
		      { "resonate_drone_twin", 0.7 },
		      { "resonate_drone_cycle", 0 },
		      { "resonate_drone_cycle_sync", 0, "1/4" },
		      { "resonate_head_release", 0, "Ring" },
		      { "resonate_drone_tuning", 0, "Just" },
		      { "resonate_ring_decay", 0 },
		      { "resonate_drone_cycle_level", 0.5 },
		      { "resonate_bounce", 0.71904755 },
		      { "resonate_tension", -2 },
		      { "resonate_drone_input", 0 },
		      { "resonate_string", 1 },
		      { "resonate_string_excite", 0 },
		      { "resonate_drone_cycle_transport", 0, "Off" },
		      { "resonate_pickup", 0.58095247 },
		      { "resonate_bridge_fixed", 0 },
		      { "resonate_drone_pluck", 0.5 },
		      { "resonate_drone_jawari", 0.55 },
		      { "resonate_drone_bounce", 0.71904755 },
		      { "resonate_drone_sympathy", 0.66555256 },
		      { "resonate_drone_to_wet_fx", 0, "On" },
		      { "resonate_drone_thread", 0.025333328 },
		      { "resonate_drone_follow", 0, "On" },
		  } },
		{ "Played", "Sitar Sympathy",
		  {
		      { "resonate_enabled", 0, "On" },
		      { "resonate_model", 0, "Stiffening" },
		      { "resonate_character", 0.15806887 },
		      { "resonate_morph", 0.42569253 },
		      { "resonate_bow", 0 },
		      { "resonate_drive", 0.5 },
		      { "resonate_pluck_strength", 0.6 },
		      { "resonate_voicing", 0, "Poly" },
		      { "resonate_porta", -11 },
		      { "resonate_octave", 0 },
		      { "resonate_semitone", 0 },
		      { "resonate_fine", 0 },
		      { "resonate_drone_out", 0.4 },
		      { "resonate_drone_excite", 0.15 },
		      { "resonate_drone_attack", -4 },
		      { "resonate_drone_decay", 3.5849626 },
		      { "resonate_drone_chord_choice", 0, "Sitar" },
		      { "resonate_drone_character", 0.5 },
		      { "resonate_drone_width", 0.5 },
		      { "resonate_root", 50 },
		      { "resonate_size_taps", 64 },
		      { "resonate_width", 0.3 },
		      { "resonate_blend", 0.5 },
		      { "resonate_level", 0.42 },
		      { "resonate_mix", 1 },
		      { "resonate_sustain", 0.25 },
		      { "resonate_jawari", 0.17380956 },
		      { "resonate_bridge_curve", 0.55 },
		      { "resonate_sympathy_notes", 0.5425954 },
		      { "resonate_drone_voice", 0.2 },
		      { "resonate_drone_stiffness", 0 },
		      { "resonate_drone_body", 0.5 },
		      { "resonate_drone_twin", 0 },
		      { "resonate_drone_cycle", 0 },
		      { "resonate_drone_cycle_sync", 0, "1/8" },
		      { "resonate_head_release", 0, "Ring" },
		      { "resonate_drone_tuning", 0, "Just" },
		      { "resonate_ring_decay", 0 },
		      { "resonate_drone_cycle_level", 0.5 },
		      { "resonate_bounce", 0.64761895 },
		      { "resonate_tension", -1.5 },
		      { "resonate_drone_input", 0.5 },
		      { "resonate_string", 1 },
		      { "resonate_string_excite", 0 },
		      { "resonate_drone_cycle_transport", 0, "Off" },
		      { "resonate_pickup", 0.6999999 },
		      { "resonate_bridge_fixed", 0 },
		      { "resonate_drone_pluck", 0.5 },
		      { "resonate_drone_jawari", 0.3 },
		      { "resonate_drone_bounce", 0.71428573 },
		      { "resonate_drone_sympathy", 0.66555256 },
		      { "resonate_drone_to_wet_fx", 0, "On" },
		      { "resonate_drone_thread", 0.03 },
		      { "resonate_drone_follow", 0, "On" },
		  } },
		{ "Filters", "Clean Resonator",
		  {
		      { "resonate_enabled", 0, "On" },
		      { "resonate_model", 0, "Ring" },
		      { "resonate_character", 0.5 },
		      { "resonate_morph", 0 },
		      { "resonate_bow", 0 },
		      { "resonate_drive", 0.35 },
		      { "resonate_pluck_strength", 0.5 },
		      { "resonate_voicing", 0, "Poly" },
		      { "resonate_porta", -11 },
		      { "resonate_octave", 0 },
		      { "resonate_semitone", 0 },
		      { "resonate_fine", 0 },
		      { "resonate_drone_out", 0 },
		      { "resonate_drone_excite", 0.3 },
		      { "resonate_drone_attack", -4 },
		      { "resonate_drone_decay", 3.5849626 },
		      { "resonate_drone_chord_choice", 0, "Unison" },
		      { "resonate_drone_character", 0.5 },
		      { "resonate_drone_width", 0.5 },
		      { "resonate_root", 48 },
		      { "resonate_size_taps", 96 },
		      { "resonate_width", 0.4 },
		      { "resonate_blend", 1 },
		      { "resonate_level", 0.45 },
		      { "resonate_mix", 0.6 },
		      { "resonate_sustain", 0 },
		      { "resonate_jawari", 0 },
		      { "resonate_bridge_curve", 0.5 },
		      // (the file also holds the RETIRED `resonate_sympathy_bridge_wide` 0, which a load ignores: Sympathy takes its default)
		      { "resonate_drone_voice", 0.2 },
		      { "resonate_drone_stiffness", 0 },
		      { "resonate_drone_body", 0 },
		      { "resonate_drone_twin", 0 },
		      { "resonate_drone_cycle", 0 },
		      { "resonate_drone_cycle_sync", 0, "Off" },
		      { "resonate_head_release", 0, "Gate" },
		      { "resonate_drone_tuning", 0, "Just" },
		      { "resonate_ring_decay", 0 },
		      { "resonate_drone_cycle_level", 0.5 },
		      { "resonate_bounce", 1 },
		      { "resonate_tension", -1.5 },
		      { "resonate_drone_input", 0.5 },
		      { "resonate_string", 0.5 },
		      { "resonate_string_excite", 0.5 },
		      { "resonate_drone_cycle_transport", 0, "Off" },
		      { "resonate_pickup", 0 },
		      { "resonate_bridge_fixed", 0 },
		      { "resonate_drone_pluck", 0.5 },
		      { "resonate_drone_jawari", 0 },
		      { "resonate_drone_to_wet_fx", 0, "On" },
		  } },
		{ "Pads", "Bowed Shimmer",
		  {
		      { "resonate_enabled", 0, "On" },
		      { "resonate_model", 0, "Ring" },
		      { "resonate_character", 0.5 },
		      { "resonate_morph", 0.3 },
		      { "resonate_bow", 0.45 },
		      { "resonate_drive", 0.5 },
		      { "resonate_pluck_strength", 0.5 },
		      { "resonate_voicing", 0, "Poly" },
		      { "resonate_porta", -11 },
		      { "resonate_octave", 0 },
		      { "resonate_semitone", 0 },
		      { "resonate_fine", 0 },
		      { "resonate_drone_out", 0.38 },
		      { "resonate_drone_excite", 0.3 },
		      { "resonate_drone_attack", -1 },
		      { "resonate_drone_decay", 3.5849626 },
		      { "resonate_drone_chord_choice", 0, "Fifth + Octave" },
		      { "resonate_drone_character", 0.5 },
		      { "resonate_drone_width", 0.9 },
		      { "resonate_root", 60 },
		      { "resonate_size_taps", 64 },
		      { "resonate_width", 0.7 },
		      { "resonate_blend", 0.2 },
		      { "resonate_level", 0.38 },
		      { "resonate_mix", 0.6 },
		      { "resonate_sustain", 0.8 },
		      { "resonate_jawari", 0 },
		      { "resonate_bridge_curve", 0.5 },
		      // (the file also holds the RETIRED `resonate_sympathy_bridge_wide` 0.6656, which a load ignores: Sympathy takes its default)
		      { "resonate_drone_voice", 0.6 },
		      { "resonate_drone_stiffness", 0 },
		      { "resonate_drone_body", 0.5 },
		      { "resonate_drone_twin", 0 },
		      { "resonate_drone_cycle", 0 },
		      { "resonate_drone_cycle_sync", 0, "Off" },
		      { "resonate_head_release", 0, "Gate" },
		      { "resonate_drone_tuning", 0, "Just" },
		      { "resonate_ring_decay", 0 },
		      { "resonate_drone_cycle_level", 0.5 },
		      { "resonate_bounce", 1 },
		      { "resonate_tension", -1.5 },
		      { "resonate_drone_input", 0.5 },
		      { "resonate_string", 0.5 },
		      { "resonate_string_excite", 0.5 },
		      { "resonate_drone_cycle_transport", 0, "Off" },
		      { "resonate_pickup", 0.3 },
		      { "resonate_bridge_fixed", 0 },
		      { "resonate_drone_pluck", 0.5 },
		      { "resonate_drone_jawari", 0 },
		      { "resonate_drone_to_wet_fx", 0, "On" },
		  } },
	};
	return set;
}

/// A Choice's index by its label, or -1.
int labelIndex(const device::ParamDesc& d, const char* label) {
	for (size_t i = 0; i < d.names.size(); ++i) {
		if (std::string_view(d.names[i]) == label) {
			return static_cast<int>(i);
		}
	}
	return -1;
}

/// THE ROW A PRESET MEANS: every control's declared default, then the preset's own.
std::vector<float> rowOf(const FactoryPreset& preset) {
	std::vector<float> row(rsn::kDescriptor.params.size());
	for (size_t i = 0; i < row.size(); ++i) {
		row[i] = static_cast<float>(rsn::kDescriptor.params[i].def);
	}
	for (const Set& s : preset.sets) {
		size_t i = 0;
		while (i < rsn::kDescriptor.params.size() && std::string_view(rsn::kDescriptor.params[i].key) != s.key) {
			++i;
		}
		// A KEY THE DEVICE NO LONGER DECLARES is a file that has drifted from the device: said, not skipped.
		REQUIRE_MESSAGE(i < rsn::kDescriptor.params.size(), preset.name << ": no control keyed " << s.key);
		const device::ParamDesc& d = rsn::kDescriptor.params[i];
		double v = s.value;
		if (s.label != nullptr) {
			const int at = labelIndex(d, s.label);
			REQUIRE_MESSAGE(at >= 0, preset.name << ": " << d.name << " has no choice " << s.label);
			v = at;
		}
		REQUIRE_MESSAGE((v >= d.min && v <= d.max), preset.name << ": " << d.name << " " << v << " out of range");
		row[i] = static_cast<float>(v);
	}
	return row;
}

/// THE PRESET AS A PERSON'S SAVE WRITES IT: a host holding the device, its controls set, captured.
std::vector<uint8_t> capture(const std::vector<float>& row) {
	rack::RackEngine engine { rack::kRackAxes, hostCatalogue() };
	engine.prepare(48000.0, 4096);
	rack::RackBridge bridge(engine);
	bridge.prepare(48000.0, 4096);
	const DeviceInstanceId id = bridge.addDevice(rsn::kTraits.tag);
	REQUIRE(id != kNoInstance);
	bridge.drain();
	bridge.publish(engine);
	for (size_t i = 0; i < row.size(); ++i) {
		bridge.setField(id, i, row[i]);
	}
	std::vector<uint8_t> bytes;
	REQUIRE(captureDevicePreset(bridge, rsn::kDescriptor, id, &bytes));
	return bytes;
}

std::string pathOf(const FactoryPreset& p) {
	return std::string(kBankPrefix) + p.category + "/" + p.name + ".fpreset";
}

//------------------------------------------------------------------------------------------------//
// THE RENDER: the device's own ops, at a typical input.                                          //
//------------------------------------------------------------------------------------------------//

constexpr double kRate = 48000.0;
constexpr int kBlock = 256;

/// A TYPICAL INPUT, as Resonate meets one in Feathers' fx lane: a phrase of plucked notes, each a sawtooth at the
/// note's pitch falling over 0.3 s from -6 dBFS, with the same notes on the note road (the synth's voices play what
/// leaves the lane, and every note reaches the device). Six notes over the drone's octave and a fifth, then silence
/// for the rings to fall.
struct Phrase {
	std::vector<float> input;
	struct Note {
		int key;
		int on;
		int off;
	};
	std::vector<Note> notes;
};

Phrase typicalPhrase() {
	Phrase p;
	const int total = static_cast<int>(12.0 * kRate);
	p.input.assign(static_cast<size_t>(total), 0.0f);
	const int keys[] = { 48, 55, 60, 62, 64, 67, 60, 55 };
	const int spacing = static_cast<int>(0.75 * kRate);
	for (int n = 0; n < 8; ++n) {
		const int on = n * spacing;
		p.notes.push_back({ keys[n], on, on + static_cast<int>(0.5 * kRate) });
		const double hz = 440.0 * std::exp2((keys[n] - 69) / 12.0);
		for (int i = 0; i < spacing && on + i < total; ++i) {
			const double t = i / kRate;
			const double saw = 2.0 * (t * hz - std::floor(t * hz + 0.5));
			p.input[static_cast<size_t>(on + i)] += static_cast<float>(0.5 * saw * std::exp(-t / 0.3));
		}
	}
	return p;
}

/// THE PEAK OF BOTH CHANNELS out of the device's own ops, the row set, the phrase played.
float renderPeak(const std::vector<float>& row, const Phrase& phrase) {
	const device::AudioOps& audio = *rsn::kDescriptor.audio;
	const device::NoteOps& notes = *rsn::kDescriptor.notes;
	void* storage = ::operator new(audio.stateBytes, std::align_val_t { audio.stateAlign });
	audio.construct(storage);
	audio.prepare(storage, kRate, 4096);
	audio.setFields(storage, row.data(), row.size());
	std::vector<float> left(kBlock);
	std::vector<float> right(kBlock);
	float peak = 0.0f;
	const int total = static_cast<int>(phrase.input.size());
	std::vector<device::NoteEvent> in;
	std::vector<device::NoteEvent> out(64);
	for (int at = 0; at < total; at += kBlock) {
		const int n = std::min(kBlock, total - at);
		in.clear();
		for (const Phrase::Note& note : phrase.notes) {
			for (const auto& [when, kind] : { std::pair { note.on, device::NoteEvent::Kind::On },
			                                  std::pair { note.off, device::NoteEvent::Kind::Off } }) {
				if (when >= at && when < at + n) {
					device::NoteEvent e;
					e.kind = kind;
					e.key = static_cast<int16_t>(note.key);
					e.noteId = note.key;
					e.velocity = kind == device::NoteEvent::Kind::On ? 0.8 : 0.0;
					in.push_back(e);
				}
			}
		}
		size_t outCount = 0;
		notes.processNotes(storage, in.data(), in.size(), out.data(), out.size(), &outCount);
		std::copy_n(phrase.input.begin() + at, n, left.begin());
		std::copy_n(phrase.input.begin() + at, n, right.begin());
		audio.processStereo(storage, left.data(), right.data(), n);
		for (int i = 0; i < n; ++i) {
			peak = std::max({ peak, std::fabs(left[static_cast<size_t>(i)]), std::fabs(right[static_cast<size_t>(i)]) });
		}
	}
	audio.destruct(storage);
	::operator delete(storage, std::align_val_t { audio.stateAlign });
	return peak;
}

/// The shipped Resonate files, by path.
std::vector<FactoryPresetFile> shippedResonate() {
	std::vector<FactoryPresetFile> out;
	for (const FactoryPresetFile& f : factoryPresetFiles()) {
		if (f.path.starts_with(kBankPrefix)) {
			out.push_back(f);
		}
	}
	return out;
}

} // namespace

TEST_CASE("RESONATE factory presets: writes the factory files from the table (only with FEATHERS_RESONATE_PRESETS_OUT)") {
	// **DO NOT RUN THIS OVER `presets/factory`.** Since 2026-10-06 the shipped files are hand-edited and
	// carry LANE BODIES (devices in DRONE FX / WET FX) that this table cannot hold: a run here would rewrite every
	// file from the table with EMPTY lanes and silently drop their lane devices and save metadata. The files are the
	// masters; this case is kept only to write a fresh set into a scratch folder.
	const char* outDir = std::getenv("FEATHERS_RESONATE_PRESETS_OUT");
	if (outDir == nullptr || *outDir == '\0') {
		MESSAGE("FEATHERS_RESONATE_PRESETS_OUT unset: nothing written");
		return;
	}
	for (const FactoryPreset& p : factorySet()) {
		PresetMeta meta;
		meta.tags = { "factory" };
		meta.saved = kSavedAt;
		meta.category = p.category;
		const std::vector<uint8_t> file = PresetStore::encode(meta, capture(rowOf(p)));
		const fs::path path = fs::path(outDir) / pathOf(p);
		fs::create_directories(path.parent_path());
		std::ofstream os(path, std::ios::binary | std::ios::trunc);
		os.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
		REQUIRE(os.good());
		MESSAGE("wrote " << path.string());
	}
}

TEST_CASE("RESONATE factory presets: what ships is exactly the table, control by control, tagged factory in its folder") {
	const std::vector<FactoryPresetFile> shipped = shippedResonate();
	REQUIRE(shipped.size() == factorySet().size());
	for (const FactoryPreset& p : factorySet()) {
		const std::string name = p.name;
		CAPTURE(name);
		const auto file = std::find_if(shipped.begin(), shipped.end(),
		                               [&](const FactoryPresetFile& f) { return f.path == pathOf(p); });
		REQUIRE(file != shipped.end());
		PresetMeta meta;
		std::vector<uint8_t> payload;
		REQUIRE(PresetStore::decode(std::vector<uint8_t>(file->bytes.begin(), file->bytes.end()), &meta, &payload));
		CHECK(std::find(meta.tags.begin(), meta.tags.end(), "factory") != meta.tags.end());
		CHECK(meta.category == p.category);
		DevicePresetContents contents;
		REQUIRE(readDevicePreset(payload, &contents, &rsn::kDescriptor));
		CHECK(contents.tag == rsn::kTraits.tag);
		// CONTROL VALUES ONLY: the row, by key (`readDevicePreset`). The BODY — the preset's lane devices — is not compared;
		// the table has no way to hold it.
		const std::vector<float> want = rowOf(p);
		REQUIRE(contents.field.size() >= want.size());
		for (size_t i = 0; i < want.size(); ++i) {
			CAPTURE(rsn::kDescriptor.params[i].name);
			CHECK(contents.field[i] == want[i]);
		}
	}
}

TEST_CASE("RESONATE factory presets: every one stays under 0 dBFS at a typical input, and is heard") {
	const Phrase phrase = typicalPhrase();
	// **EVERY PRESET UNDER THE BOUND, NONE EXCEPTED.** The hand-edited Rudra Veena peaked at +3.95 dBFS here; the fix
	// (2026-10-06) trimmed its OUTPUT and nothing else: Level 0.4 -> 0.268 and Drone Out
	// 0.42 -> 0.2814, the same 0.67 on both faders ((2v)^2: -6.95 dB), so its tone/drone balance is kept. Level alone could
	// not reach it (with the tone silent the drone alone peaks at +1.5 dBFS). MEASURED after: -3.0 dBFS. The set below is
	// kept, and EMPTY, so a preset crossing the bound later is named rather than lost in one CHECK.
	const std::vector<std::string> overAwaitingRuling {};
	std::vector<std::string> over;
	for (const FactoryPreset& p : factorySet()) {
		const float peak = renderPeak(rowOf(p), phrase);
		const std::string name = p.name;
		CAPTURE(name);
		MESSAGE(name << ": peak " << 20.0 * std::log10(std::max(1e-9f, peak)) << " dBFS (input -6 dBFS)");
		if (peak >= 1.0f) {
			over.push_back(name);
		}
		CHECK(peak > 0.05f); // non-vacuity: the render reached the output
	}
	CHECK(over == overAwaitingRuling);
	// AND THE BOUND CAN GO RED: the same phrase through the loudest a preset could be — Level, Drone Out and Mix at
	// their tops, every exciter up — is over it. A ceiling nothing could cross would prove nothing.
	FactoryPreset loud { "x", "loud",
		                 { { "resonate_level", 1.0 }, { "resonate_drone_out", 1.0 }, { "resonate_mix", 1.0 },
		                   { "resonate_bow", 1.0 }, { "resonate_drive", 1.0 }, { "resonate_sustain", 1.0 },
		                   { "resonate_drone_input", 1.0 } } };
	CHECK(renderPeak(rowOf(loud), phrase) >= 1.0f);
}

//------------------------------------------------------------------------------------------------//
// THE PERF PASS (backlog PB 2, 2026-10-29): the fast path is the path it replaced, to the bit.     //
//------------------------------------------------------------------------------------------------//

namespace {

/// BOTH CHANNELS, interleaved, out of the device's own ops: the row set, the phrase played, in host blocks that cycle through
/// `blocks` (so a window boundary, the FIR's look-ahead and the history's mirror end fall everywhere), the DSP on the
/// reference road or not (ResonateDsp::setReferenceForTest), and `later` set over the row at sample `laterAt` (a Blend
/// leaving 0, so the kernels are seated mid-window).
std::vector<float> renderAll(const std::vector<float>& row, const Phrase& phrase, bool reference, const std::vector<int>& blocks,
                             const std::vector<float>* later = nullptr, int laterAt = -1, std::vector<std::pair<double, int>>* droneNodes = nullptr) {
	const device::AudioOps& audio = *rsn::kDescriptor.audio;
	const device::NoteOps& notes = *rsn::kDescriptor.notes;
	void* storage = ::operator new(audio.stateBytes, std::align_val_t { audio.stateAlign });
	audio.construct(storage);
	audio.prepare(storage, kRate, 4096);
	rsn::dspForTest(storage).setReferenceForTest(reference);
	audio.setFields(storage, row.data(), row.size());
	const int total = static_cast<int>(phrase.input.size());
	std::vector<float> out;
	out.reserve(2 * static_cast<size_t>(total));
	std::vector<float> left(4096);
	std::vector<float> right(4096);
	std::vector<device::NoteEvent> in;
	std::vector<device::NoteEvent> passed(64);
	size_t b = 0;
	for (int at = 0; at < total;) {
		const int n = std::min(blocks[b++ % blocks.size()], total - at);
		if (later != nullptr && laterAt >= at && laterAt < at + n) {
			audio.setFields(storage, later->data(), later->size());
		}
		in.clear();
		for (const Phrase::Note& note : phrase.notes) {
			for (const auto& [when, kind] : { std::pair { note.on, device::NoteEvent::Kind::On },
			                                  std::pair { note.off, device::NoteEvent::Kind::Off } }) {
				if (when >= at && when < at + n) {
					device::NoteEvent e;
					e.kind = kind;
					e.key = static_cast<int16_t>(note.key);
					e.noteId = note.key;
					e.velocity = kind == device::NoteEvent::Kind::On ? 0.8 : 0.0;
					in.push_back(e);
				}
			}
		}
		size_t outCount = 0;
		notes.processNotes(storage, in.data(), in.size(), passed.data(), passed.size(), &outCount);
		std::copy_n(phrase.input.begin() + at, n, left.begin());
		std::copy_n(phrase.input.begin() + at, n, right.begin());
		audio.processStereo(storage, left.data(), right.data(), n);
		for (int i = 0; i < n; ++i) {
			out.push_back(left[static_cast<size_t>(i)]);
			out.push_back(right[static_cast<size_t>(i)]);
		}
		at += n;
	}
	if (droneNodes != nullptr) { // EACH ACTIVE DRONE STRING'S PITCH AND NODE COUNT at the end (Drone Detail, backlog PS)
		const rsn::ResonateDsp& dsp = rsn::dspForTest(storage);
		for (size_t k = 0; k < rsn::kMaxDroneStrings; ++k) {
			if (dsp.droneActiveForTest(k)) {
				droneNodes->emplace_back(dsp.droneNodeForTest(k).tunedHz(), dsp.droneNodeForTest(k).nodes());
			}
		}
	}
	audio.destruct(storage);
	::operator delete(storage, std::align_val_t { audio.stateAlign });
	return out;
}

/// How many samples differ in their BITS (a NaN against a NaN is the same bits only if it is the same NaN).
size_t bitsDiffering(const std::vector<float>& a, const std::vector<float>& b) {
	REQUIRE(a.size() == b.size());
	size_t n = 0;
	for (size_t i = 0; i < a.size(); ++i) {
		n += std::memcmp(&a[i], &b[i], sizeof(float)) != 0 ? 1 : 0;
	}
	return n;
}

} // namespace

/// A BUSY PHRASE: 32 short notes, 0.12 s apart, over eleven keys from two octaves below middle C — more notes than heads, so
/// heads are taken again while their strings still ring (a string retuned, regridded, and seated back on a moving bridge).
Phrase busyPhrase() {
	Phrase p;
	const int total = static_cast<int>(4.0 * kRate);
	p.input.assign(static_cast<size_t>(total), 0.0f);
	const int keys[] = { 36, 43, 48, 52, 55, 60, 64, 67, 72, 41, 46 };
	const int spacing = static_cast<int>(0.12 * kRate);
	for (int n = 0; n < 32; ++n) {
		const int key = keys[n % 11];
		const int on = n * spacing;
		p.notes.push_back({ key, on, on + static_cast<int>(0.08 * kRate) });
		const double hz = 440.0 * std::exp2((key - 69) / 12.0);
		for (int i = 0; i < spacing && on + i < total; ++i) {
			const double t = i / kRate;
			const double saw = 2.0 * (t * hz - std::floor(t * hz + 0.5));
			p.input[static_cast<size_t>(on + i)] += static_cast<float>(0.5 * saw * std::exp(-t / 0.05));
		}
	}
	return p;
}

TEST_CASE("RESONATE perf pass: the fast path renders the reference path's bits, every factory preset and the heavy loads") {
	// THE PHRASE'S FIRST 4 s (five notes, held and released, over the drone): every preset's strings are struck, ring and are
	// read, its drone is fed, and its FIR is heard where its Blend is up.
	Phrase phrase = typicalPhrase();
	phrase.input.resize(static_cast<size_t>(4.0 * kRate));
	const Phrase busy = busyPhrase();
	const std::vector<int> blocks { 256, 97, 1, 33, 512, 7 };
	struct Load {
		std::string name;
		std::vector<float> row;
		const Phrase* phrase = nullptr;
		std::vector<float> later {}; // set over the row at laterAt, if any
		int laterAt = -1;
	};
	std::vector<Load> loads;
	for (const FactoryPreset& p : factorySet()) {
		loads.push_back({ p.name, rowOf(p), &phrase });
	}
	// THE BENCH'S HEAVY ROWS (bench_resonate) and the roads the presets leave untried: the longest FIR at Width 0 and not,
	// the Membrane, eight strings, Bridge Fixed between 0 and 1 (pointHeight's full exponent), a stiff drone, the Bow.
	const auto with = [](std::vector<Set> sets) { return rowOf(FactoryPreset { "x", "load", std::move(sets) }); };
	loads.push_back({ "defaults", with({}), &phrase });
	loads.push_back({ "Size 256", with({ { "resonate_size_taps", 256.0 } }), &phrase });
	loads.push_back({ "Size 256, Width 0", with({ { "resonate_size_taps", 256.0 }, { "resonate_width", 0.0 } }), &phrase });
	loads.push_back({ "Membrane, Size 256, Bow", with({ { "resonate_model", 3.0 }, { "resonate_size_taps", 256.0 }, { "resonate_bow", 0.5 } }),
	                  &phrase });
	loads.push_back({ "Bilawal, Fixed 0.5, stiff",
	                  with({ { "resonate_drone_chord_choice", 0, "Bilawal (Ionian)" }, { "resonate_bridge_fixed", 0.5 },
	                         { "resonate_drone_stiffness", 1.0 }, { "resonate_jawari", 0.8 }, { "resonate_string", 1.0 } }),
	                  &phrase });
	// HEADS TAKEN AGAIN WHILE THEIR STRINGS RING, on a bridge the drone moves (both Sympathies up): NodeString's rows must be
	// dropped when a string is regridded or seated back on the bridge (rowValid_).
	const std::vector<Set> ringing { { "resonate_string", 1.0 }, { "resonate_sympathy_notes", 1.0 }, { "resonate_drone_sympathy", 1.0 },
		                             { "resonate_drone_chord_choice", 0, "Tanpura" }, { "resonate_drone_out", 0.6 } };
	loads.push_back({ "busy, ringing on the bridge", with(ringing), &busy });
	{
		std::vector<Set> mono = ringing;
		mono.push_back({ "resonate_voicing", 0, "Mono" });
		mono.push_back({ "resonate_porta", -3.0 });
		loads.push_back({ "busy, Mono gliding", with(mono), &busy });
	}
	// THE DRONE'S ROOT MOVED WHILE IT RINGS: its strings retune (and regrid) in place.
	{
		std::vector<Set> moved = ringing;
		moved.push_back({ "resonate_root", 55.0 });
		loads.push_back({ "Root up a fifth mid-ring", with(ringing), &phrase, with(moved), static_cast<int>(1.5 * kRate) + 5 });
	}
	// THE DRONE'S STRINGS JOINING A MOVING BRIDGE WHILE THEY RING (Drone Sympathy 0 -> 1, the notes' strings moving it): each is
	// seated where the bridge is, which writes its previous level's end.
	{
		std::vector<Set> off = ringing;
		off.push_back({ "resonate_drone_sympathy", 0.0 });
		loads.push_back({ "the drone joining a moving bridge", with(off), &busy, with(ringing), static_cast<int>(2.0 * kRate) + 3 });
	}
	// THE CHORD CHANGED WHILE IT RINGS: a string whose interval moves fades out, is cleared while still swinging, and starts again
	// — on a RIGID bridge (both Sympathies 0), where no seat follows the clear to drop the rows for it.
	{
		std::vector<Set> rigid = ringing;
		rigid.push_back({ "resonate_sympathy_notes", 0.0 });
		rigid.push_back({ "resonate_drone_sympathy", 0.0 });
		std::vector<Set> major = rigid;
		major.push_back({ "resonate_drone_chord_choice", 0, "Major" });
		loads.push_back({ "the chord changed mid-ring", with(rigid), &phrase, with(major), static_cast<int>(1.7 * kRate) + 9 });
	}
	// BLEND LEAVING 0 MID-RENDER: the kernels were left stale at the ticks meanwhile and are seated mid-window.
	const std::vector<float> blendOff = with({ { "resonate_blend", 0.0 }, { "resonate_size_taps", 200.0 } });
	loads.push_back({ "Blend leaving 0", blendOff, &phrase, with({ { "resonate_blend", 1.0 }, { "resonate_size_taps", 200.0 } }),
	                  static_cast<int>(1.3 * kRate) + 11 });
	for (const Load& load : loads) {
		CAPTURE(load.name);
		const std::vector<float>* later = load.later.empty() ? nullptr : &load.later;
		const std::vector<float> reference = renderAll(load.row, *load.phrase, true, blocks, later, load.laterAt);
		const std::vector<float> fast = renderAll(load.row, *load.phrase, false, blocks, later, load.laterAt);
		CHECK(bitsDiffering(reference, fast) == 0);
		// NON-VACUITY: the render reached the output.
		float peak = 0.0f;
		for (const float v : fast) {
			peak = std::max(peak, std::fabs(v));
		}
		CHECK(peak > 0.01f);
	}
	// AND THE CHANGES WERE HEARD: the Blend's and the Root's renders differ from ones that kept the first row.
	CHECK(bitsDiffering(renderAll(blendOff, phrase, false, blocks), renderAll(loads.back().row, phrase, false, blocks, &loads.back().later,
	                                                                          loads.back().laterAt))
	      > 0);
	const Load& root = *std::find_if(loads.begin(), loads.end(), [](const Load& l) { return l.name == "Root up a fifth mid-ring"; });
	CHECK(bitsDiffering(renderAll(root.row, phrase, false, blocks), renderAll(root.row, phrase, false, blocks, &root.later, root.laterAt)) > 0);
}

//------------------------------------------------------------------------------------------------//
// DRONE DETAIL (backlog PS, 2026-11-05): a switch, so the two can be A/B compared                   //
//------------------------------------------------------------------------------------------------//

namespace {

/// |a - b| over |a|, in dB, both channels: how loud the change is against what it changes.
double residualDb(const std::vector<float>& a, const std::vector<float>& b) {
	REQUIRE(a.size() == b.size());
	double ea = 0.0;
	double ed = 0.0;
	for (size_t i = 0; i < a.size(); ++i) {
		ea += static_cast<double>(a[i]) * a[i];
		const double d = static_cast<double>(b[i]) - a[i];
		ed += d * d;
	}
	return 10.0 * std::log10(std::max(ed, 1e-300) / std::max(ea, 1e-300));
}

/// THE NODE COUNT A STRING TAKES, by its own law (NodeString::tune, an ideal string): as many as the Courant number allows, under the cap.
int nodesFor(double hz, int cap) {
	return std::clamp(static_cast<int>(rsn::NodeString::kCourant * kRate / (2.0 * hz)), 2, cap);
}

} // namespace

TEST_CASE("RESONATE Drone Detail: Full is the string as it was; each cap gives every drone string exactly that many nodes and changes the "
          "sound by its measured residual; a change while the strings ring regrids them without a jump") {
	Phrase phrase = typicalPhrase();
	phrase.input.resize(static_cast<size_t>(4.0 * kRate));
	const std::vector<int> blocks { 256, 97, 1, 33, 512, 7 };
	// WOOD ELVES' DRONE (its factory patch's Resonate): the eight strings of Bhairavi, 73-147 Hz, all at 128 nodes at Full.
	const std::vector<Set> elves { { "resonate_drone_chord_choice", 0, "Bhairavi (Phrygian)" }, { "resonate_root", 62.212986 },
		                           { "resonate_octave", -1.7142857 }, { "resonate_drone_out", 1.0 }, { "resonate_drone_decay", 3.321928 },
		                           { "resonate_drone_sympathy", 0.66555256 } };
	const auto with = [&](const char* detail) {
		std::vector<Set> sets = elves;
		if (detail != nullptr) {
			sets.push_back({ "resonate_drone_detail", 0, detail });
		}
		return rowOf(FactoryPreset { "x", "detail", std::move(sets) });
	};
	// FULL, STORED BY LABEL, IS THE ROW WITHOUT THE KEY to the bit (and the whole plugin's renders are the build before it to the bit:
	// bench_patch's phase hashes on Wood Elves and Pan Tanpura, in the commit).
	std::vector<std::pair<double, int>> fullNodes;
	const std::vector<float> full = renderAll(with("Full"), phrase, false, blocks, nullptr, -1, &fullNodes);
	CHECK(bitsDiffering(renderAll(with(nullptr), phrase, false, blocks), full) == 0);
	REQUIRE(fullNodes.size() == 8);
	for (const auto& [hz, n] : fullNodes) {
		CAPTURE(hz);
		CHECK(n == nodesFor(hz, rsn::NodeString::kMaxNodes));
		CHECK(n == 128); // every Bhairavi string here is under 187.5 Hz
	}
	// EACH CAP: every drone string at min(its own count, the cap), BY VALUE, and the sound moved by a little — MEASURED (the comment
	// beside each band): inaudibly small next to the drone itself, never nothing.
	struct Cap {
		const char* label;
		int nodes;
		double lowDb;
		double highDb;
	};
	// MEASURED 2026-11-05, this phrase into the device alone (the drone heard bare): -16.9, -12.3 and -8.8 dB, pinned +-3 dB. Inside
	// the factory patches (bench_patch, the whole plugin) the same switch is -69 / -69 / -66 dB on Wood Elves and -54 / -50 / -47 on
	// Pan Tanpura: there the drone's strings are a small part of what is heard.
	const Cap caps[] = { { "96", 96, -19.9, -13.9 }, { "64", 64, -15.3, -9.3 }, { "48", 48, -11.8, -5.8 } };
	for (const Cap& c : caps) {
		CAPTURE(c.label);
		std::vector<std::pair<double, int>> nodes;
		const std::vector<float> capped = renderAll(with(c.label), phrase, false, blocks, nullptr, -1, &nodes);
		REQUIRE(nodes.size() == 8);
		for (const auto& [hz, n] : nodes) {
			CAPTURE(hz);
			CHECK(n == nodesFor(hz, c.nodes));
			CHECK(n == c.nodes); // every string here is low enough to want more
		}
		const double r = residualDb(full, capped);
		MESSAGE("Drone Detail " << std::string(c.label) << ": residual " << r << " dB re Full");
		CHECK(r > c.lowDb);
		CHECK(r < c.highDb);
	}
	// A CHANGE WHILE THEY RING (Full -> 48 at 1.5 s, then back is the same road): the strings regrid in place. No sample-to-sample
	// step in the 20 ms after it is larger than twice the largest Full itself takes there.
	const int at = static_cast<int>(1.5 * kRate) + 5;
	const std::vector<float> to48 = with("48");
	const std::vector<float> moved = renderAll(with("Full"), phrase, false, blocks, &to48, at);
	const auto stepAfter = [&](const std::vector<float>& x) {
		float worst = 0.0f;
		for (int i = at; i < at + static_cast<int>(0.02 * kRate); ++i) {
			for (int ch = 0; ch < 2; ++ch) {
				const size_t j = 2 * static_cast<size_t>(i) + static_cast<size_t>(ch);
				CHECK(std::isfinite(x[j]));
				worst = std::max(worst, std::fabs(x[j] - x[j - 2]));
			}
		}
		return worst;
	};
	const float stepFull = stepAfter(full);
	const float stepMoved = stepAfter(moved);
	MESSAGE("largest step after the change: " << stepMoved << " (Full alone: " << stepFull << ")");
	CHECK(stepFull > 0.0f); // NON-VACUITY: the drone is sounding there
	CHECK(stepMoved <= 2.0f * stepFull);
	CHECK(bitsDiffering(full, moved) > 0); // and the change was heard
}
