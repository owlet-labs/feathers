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

// SKLITTER ON THE POISONED HEAP, THROUGH THE PLUGIN A HOST LOADS — the rack's CLAP entry, every one of
// its buffers (the engine, the lanes, the plugin's own) on a heap that hands out 0.5f. Replacing
// the whole plugin instance still left the crush audible, so the device-level case alone
// (`test_poison_heap.cpp`) cannot speak for the plugin around it. See `poison_heap.h`.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "plugin/feathers_test_chain.h"
#include "rack/rack_clap.h"

#include <clap/clap.h>

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "poison_heap.h"

using namespace feathers;

namespace {

constexpr double kRate = 48000.0;

clap_host makeHost() {
	clap_host h {};
	h.clap_version = CLAP_VERSION_INIT;
	h.name = "poison-venue";
	h.vendor = "Owlet";
	h.url = "";
	h.version = "1";
	h.get_extension = [](const clap_host*, const char*) -> const void* { return nullptr; };
	h.request_restart = [](const clap_host*) {};
	h.request_process = [](const clap_host*) {};
	h.request_callback = [](const clap_host*) {};
	return h;
}

/// The rack plugin, created, initialised and activated on the poisoned heap, playing silence.
struct Venue {
	clap_host host = makeHost();
	const clap_plugin* plugin = nullptr;
	const clap_plugin_feathers_test_chain* chain = nullptr;
	int block;
	std::vector<float> l, r, inL, inR;
	float* outPtr[2] {};
	float* inPtr[2] {};
	clap_audio_buffer out {}, in {};
	double beats = 0.0;

	explicit Venue(int frames) : block(frames), l(frames), r(frames), inL(frames), inR(frames) {
		plugin = rack::create(&host);
		REQUIRE(plugin != nullptr);
		REQUIRE(plugin->init(plugin));
		REQUIRE(plugin->activate(plugin, kRate, 1, static_cast<uint32_t>(frames)));
		REQUIRE(plugin->start_processing(plugin));
		chain = static_cast<const clap_plugin_feathers_test_chain*>(
		    plugin->get_extension(plugin, CLAP_EXT_FEATHERS_TEST_CHAIN));
		REQUIRE(chain != nullptr);
		outPtr[0] = l.data();
		outPtr[1] = r.data();
		inPtr[0] = inL.data();
		inPtr[1] = inR.data();
		out.data32 = outPtr;
		out.channel_count = 2;
		in.data32 = inPtr;
		in.channel_count = 2;
	}
	~Venue() {
		plugin->stop_processing(plugin);
		plugin->deactivate(plugin);
		plugin->destroy(plugin);
	}
	Venue(const Venue&) = delete;
	Venue& operator=(const Venue&) = delete;

	struct Heard {
		size_t nonzero = 0;
		size_t first = SIZE_MAX;
		float peak = 0.0f;
	};

	/// `seconds` of silence in — or of a tone at `amp` — with a playing transport at 120 BPM. Counts
	/// what came out that was not zero.
	Heard silence(double seconds, float amp = 0.0f) {
		Heard h;
		clap_input_events none {};
		none.size = [](const clap_input_events*) -> uint32_t { return 0; };
		none.get = [](const clap_input_events*, uint32_t) -> const clap_event_header* { return nullptr; };
		clap_output_events sink {};
		sink.try_push = [](const clap_output_events*, const clap_event_header*) { return true; };
		const auto total = static_cast<size_t>(seconds * kRate);
		for (size_t n = 0; n < total; n += static_cast<size_t>(block)) {
			for (size_t i = 0; i < inL.size(); ++i) {
				inL[i] = inR[i] = amp * std::sin(0.037f * static_cast<float>((n + i) % 100000));
			}
			clap_event_transport tp {};
			tp.header.size = sizeof(tp);
			tp.header.type = CLAP_EVENT_TRANSPORT;
			tp.flags = CLAP_TRANSPORT_HAS_TEMPO | CLAP_TRANSPORT_HAS_BEATS_TIMELINE
			           | CLAP_TRANSPORT_HAS_SECONDS_TIMELINE | CLAP_TRANSPORT_IS_PLAYING;
			tp.tempo = 120.0;
			tp.song_pos_beats = static_cast<clap_beattime>(std::llround(beats * CLAP_BEATTIME_FACTOR));
			tp.song_pos_seconds = static_cast<clap_sectime>(std::llround(beats * 0.5 * CLAP_SECTIME_FACTOR));
			tp.tsig_num = 4;
			tp.tsig_denom = 4;
			clap_process p {};
			p.frames_count = static_cast<uint32_t>(block);
			p.transport = &tp;
			p.audio_inputs = &in;
			p.audio_inputs_count = 1;
			p.audio_outputs = &out;
			p.audio_outputs_count = 1;
			p.in_events = &none;
			p.out_events = &sink;
			plugin->process(plugin, &p);
			beats += block / (kRate * 0.5);
			for (int i = 0; i < block; ++i) {
				const float v = std::max(std::fabs(l[static_cast<size_t>(i)]), std::fabs(r[static_cast<size_t>(i)]));
				if (!(v == 0.0f)) {
					++h.nonzero;
					h.first = std::min(h.first, n + static_cast<size_t>(i));
					h.peak = std::isfinite(v) ? std::max(h.peak, v) : 1e30f;
				}
			}
		}
		return h;
	}
};

std::string said(const Venue::Heard& h) {
	return "nonzero=" + std::to_string(h.nonzero) + " first=" + std::to_string(h.first) + " peak=" + std::to_string(h.peak);
}

} // namespace

TEST_CASE("POISONED HEAP, THE PLUGIN: a fresh rack plugin with SKLITTER captures silence and plays silence, every mode, several host blocks") {
	for (const int block : { 128, 512, 1024 }) {
		for (int m = 0; m < 8; ++m) {
			CAPTURE(block);
			CAPTURE(m);
			Venue v(block);
			const uint32_t slot = v.chain->add_device(v.plugin, "SKLITTER");
			REQUIRE(slot != UINT32_MAX);
			REQUIRE(v.chain->set_control(v.plugin, slot, "Mode", static_cast<double>(m)));
			const Venue::Heard idle = v.silence(2.0);
			CAPTURE(said(idle));
			CHECK(idle.nonzero == 0u);
			REQUIRE(v.chain->set_control(v.plugin, slot, "Engage", 1.0));
			const Venue::Heard engaged = v.silence(6.0);
			CAPTURE(said(engaged));
			CHECK(engaged.nonzero == 0u);
			// A Buffer Length change and a switch into Grain, still on silence.
			REQUIRE(v.chain->set_control(v.plugin, slot, "Buffer Length", 4.0));
			REQUIRE(v.chain->set_control(v.plugin, slot, "Mode", 5.0));
			const Venue::Heard moved = v.silence(4.0);
			CAPTURE(said(moved));
			CHECK(moved.nonzero == 0u);
		}
	}
}

TEST_CASE("POISONED HEAP, THE PLUGIN: a SECOND plugin made after the first is destroyed plays silence — the host's reuse, on purpose") {
	// The first instance plays LOUD audio through SKLITTER for a while and is destroyed, so the
	// allocator's free lists hold its audio rather than the poison; the second is built from them.
	{
		Venue first(512);
		const uint32_t slot = first.chain->add_device(first.plugin, "SKLITTER");
		REQUIRE(slot != UINT32_MAX);
		REQUIRE(first.chain->set_control(first.plugin, slot, "Engage", 1.0));
		REQUIRE(first.chain->set_control(first.plugin, slot, "Mode", 5.0));
		const Venue::Heard loud = first.silence(6.0, 0.9f);
		REQUIRE(loud.nonzero > 100000u); // NOT VACUOUS: the first instance really played audio
	}
	Venue second(512);
	const uint32_t slot = second.chain->add_device(second.plugin, "SKLITTER");
	REQUIRE(slot != UINT32_MAX);
	REQUIRE(second.chain->set_control(second.plugin, slot, "Engage", 1.0));
	const Venue::Heard heard = second.silence(8.0);
	CAPTURE(said(heard));
	CHECK(heard.nonzero == 0u);
}

//--------------------------------------------------------------------------------------------------//
// IN-PLACE AND ALIASED BUFFERS. A CLAP host may hand the same buffer for input and output (the rack  //
// declares `in_place_pair`), and a mono track may hand one buffer for both channels. A path that     //
// reads the input after writing the output would record SKLITTER's own wet as "input" — a capture of //
// silence would then keep the wet's residue, which is the reported symptom.                              //
//--------------------------------------------------------------------------------------------------//

namespace {

enum class Layout { Separate, SeparateStaleOut, InPlace, MonoInPlace, CrossedInPlace };

/// A fresh rack plugin holding SKLITTER (engaged from the start, in `mode`), driven by `signal` for
/// `seconds` in the given buffer layout. Answers the LEFT output, block after block.
std::vector<float> render(Layout layout, int mode, double seconds, float (*signal)(size_t)) {
	constexpr int kBlock = 256;
	clap_host host = makeHost();
	const clap_plugin* plugin = rack::create(&host);
	REQUIRE(plugin != nullptr);
	REQUIRE(plugin->init(plugin));
	REQUIRE(plugin->activate(plugin, kRate, 1, kBlock));
	REQUIRE(plugin->start_processing(plugin));
	const auto* chain = static_cast<const clap_plugin_feathers_test_chain*>(
	    plugin->get_extension(plugin, CLAP_EXT_FEATHERS_TEST_CHAIN));
	REQUIRE(chain != nullptr);
	const uint32_t slot = chain->add_device(plugin, "SKLITTER");
	REQUIRE(slot != UINT32_MAX);
	REQUIRE(chain->set_control(plugin, slot, "Mode", static_cast<double>(mode)));
	REQUIRE(chain->set_control(plugin, slot, "Engage", 1.0));

	std::vector<float> a(kBlock), b(kBlock), c(kBlock), d(kBlock);
	float* inPtr[2] {};
	float* outPtr[2] {};
	clap_audio_buffer in {}, out {};
	in.channel_count = 2;
	out.channel_count = 2;
	in.data32 = inPtr;
	out.data32 = outPtr;
	clap_input_events none {};
	none.size = [](const clap_input_events*) -> uint32_t { return 0; };
	none.get = [](const clap_input_events*, uint32_t) -> const clap_event_header* { return nullptr; };
	clap_output_events sink {};
	sink.try_push = [](const clap_output_events*, const clap_event_header*) { return true; };

	std::vector<float> heard;
	double beats = 0.0;
	const auto total = static_cast<size_t>(seconds * kRate);
	for (size_t n = 0; n < total; n += kBlock) {
		switch (layout) {
		case Layout::Separate:
		case Layout::SeparateStaleOut:
			inPtr[0] = a.data(), inPtr[1] = b.data(), outPtr[0] = c.data(), outPtr[1] = d.data();
			break;
		case Layout::InPlace:
			inPtr[0] = outPtr[0] = a.data(), inPtr[1] = outPtr[1] = b.data();
			break;
		case Layout::MonoInPlace: // one buffer for everything, as a mono track might
			inPtr[0] = inPtr[1] = outPtr[0] = outPtr[1] = a.data();
			break;
		case Layout::CrossedInPlace: // in L is out R, in R is out L
			inPtr[0] = outPtr[1] = a.data(), inPtr[1] = outPtr[0] = b.data();
			break;
		}
		for (int i = 0; i < kBlock; ++i) {
			const float v = signal(n + static_cast<size_t>(i));
			inPtr[0][i] = v;
			inPtr[1][i] = v;
		}
		if (layout == Layout::SeparateStaleOut) {
			// The host's output still holds something LOUD from before; the plugin must overwrite it.
			for (int i = 0; i < kBlock; ++i) {
				c[static_cast<size_t>(i)] = d[static_cast<size_t>(i)] = 0.75f;
			}
		}
		clap_event_transport tp {};
		tp.header.size = sizeof(tp);
		tp.header.type = CLAP_EVENT_TRANSPORT;
		tp.flags = CLAP_TRANSPORT_HAS_TEMPO | CLAP_TRANSPORT_HAS_BEATS_TIMELINE | CLAP_TRANSPORT_HAS_SECONDS_TIMELINE
		           | CLAP_TRANSPORT_IS_PLAYING;
		tp.tempo = 120.0;
		tp.song_pos_beats = static_cast<clap_beattime>(std::llround(beats * CLAP_BEATTIME_FACTOR));
		tp.song_pos_seconds = static_cast<clap_sectime>(std::llround(beats * 0.5 * CLAP_SECTIME_FACTOR));
		clap_process p {};
		p.frames_count = kBlock;
		p.transport = &tp;
		p.audio_inputs = &in;
		p.audio_inputs_count = 1;
		p.audio_outputs = &out;
		p.audio_outputs_count = 1;
		p.in_events = &none;
		p.out_events = &sink;
		// A FRESH CAPTURE, as a player makes one: disengage at 3 s (the tone stops there too), re-engage
		// at 5 s on silence. Engaged, P Write 0 freezes the ring, so only a new press records anew.
		if (n == static_cast<size_t>(3.0 * kRate) / kBlock * kBlock) {
			REQUIRE(chain->set_control(plugin, slot, "Engage", 0.0));
		}
		if (n == static_cast<size_t>(5.0 * kRate) / kBlock * kBlock) {
			REQUIRE(chain->set_control(plugin, slot, "Engage", 1.0));
		}
		plugin->process(plugin, &p);
		beats += kBlock / (kRate * 0.5);
		// THE CROSSED LAYOUT'S LEFT OUTPUT IS BUFFER b; every other layout's is outPtr[0].
		heard.insert(heard.end(), outPtr[0], outPtr[0] + kBlock);
	}
	plugin->stop_processing(plugin);
	plugin->deactivate(plugin);
	plugin->destroy(plugin);
	return heard;
}

float toneThenSilence(size_t n) {
	// 3 s of a tone (SKLITTER records and plays it), then silence, which the press at 5 s records.
	return n < static_cast<size_t>(3.0 * kRate) ? 0.8f * std::sin(0.0581f * static_cast<float>(n)) : 0.0f;
}

} // namespace

TEST_CASE("IN PLACE: every buffer layout a host may use renders the separate-buffer output bit for bit, and a capture of silence after a tone plays exact zeros") {
	for (const int mode : { 4, 5, 3 }) { // Shuffle, Grain, Time
		CAPTURE(mode);
		const std::vector<float> ref = render(Layout::Separate, mode, 14.0, toneThenSilence);
		// NOT VACUOUS: the tone was heard, and SKLITTER made something other than the dry of it.
		size_t loud = 0;
		for (size_t i = 0; i < static_cast<size_t>(3.0 * kRate); ++i) {
			loud += std::fabs(ref[i]) > 0.1f ? 1u : 0u;
		}
		REQUIRE(loud > 10000u);
		// THE TAIL: from 10 s — a press at 5 s recorded silence and has played it since — the output is 0.
		size_t tail = 0;
		for (size_t i = static_cast<size_t>(10.0 * kRate); i < ref.size(); ++i) {
			tail += ref[i] != 0.0f ? 1u : 0u;
		}
		CAPTURE(tail);
		CHECK(tail == 0u);
		for (const Layout layout : { Layout::SeparateStaleOut, Layout::InPlace, Layout::MonoInPlace, Layout::CrossedInPlace }) {
			CAPTURE(static_cast<int>(layout));
			const std::vector<float> got = render(layout, mode, 14.0, toneThenSilence);
			size_t differ = 0;
			size_t firstDiff = SIZE_MAX;
			size_t gotTail = 0;
			for (size_t i = 0; i < ref.size(); ++i) {
				if (got[i] != ref[i]) {
					++differ;
					firstDiff = std::min(firstDiff, i);
				}
				gotTail += (i >= static_cast<size_t>(10.0 * kRate) && got[i] != 0.0f) ? 1u : 0u;
			}
			CAPTURE(differ);
			CAPTURE(firstDiff);
			CAPTURE(gotTail);
			// ONE BUFFER FOR BOTH CHANNELS CANNOT CARRY TWO: the right channel's blend lands last and is
			// what the shared buffer holds, so a mono layout is not compared with the stereo left. It
			// must still record the silence and play exact zeros.
			if (layout != Layout::MonoInPlace) {
				CHECK(differ == 0u);
			}
			CHECK(gotTail == 0u);
		}
	}
}
