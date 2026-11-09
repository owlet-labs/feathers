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

#include "plugin/synth.h"

#include <cmath>
#include <memory>
#include <vector>

// Terrain stereo at the synth — the per-sound form (§5.5, decisions V11/V12/V13): a V5 patch with a
// wide sound renders the character twice, each ear seeing sound k's ANCHOR displaced by
// ±(global·w_k)/2 on the disk's X. The global Width 0x0043 is the MASTER SCALE; the per-sound
// widths 0x0203…0x0273 are what it scales.
//
// The load-bearing claims are the identity ones. No wide sound is TODAY'S RENDER: the mono path is
// the untouched code and these tests hold it to the bit — including at global > 0 with every
// per-sound width at zero, which is how a patch from the brief global-width era reads (V13b,
// documented rather than hidden; the loader half lives in test_params.cpp). The frozen engines are
// mono AT EVERY WIDTH — V5 only, by design, with an engine switch mid-performance dropping to mono as
// an accepted consequence on the record. And an ear is not a pan law: in the uniform case it is the
// render the puck itself would make standing where the ear stands, which the Pos X equivalence pins
// bit-for-bit because anchor displacement is relatively identical to puck displacement and the
// fixture stays on the binary grid.

namespace {

using feathers::OscType;
using feathers::ParamId;
using feathers::ParamSet;
using feathers::soundParam;
using feathers::SoundField;
using feathers::Synth;

constexpr double kSampleRate = 48000.0;
constexpr int16_t kKeyC4 = 60;

/// What CharacterBank makes of kCharacterFadeSeconds at this rate, plus the decimator slack the
/// bank's own guard adds. A seam is fully drained this many samples after its build lands; asserts
/// below leave a margin on top rather than cutting it fine.
constexpr int kSeamDrain = static_cast<int>(feathers::kCharacterFadeSeconds * kSampleRate + 0.5) + 8;

/// A parameter moving mid-performance, which is what automation is.
struct Event {
	uint32_t frame;
	ParamId param;
	double value;
};

/// Both channels of a performance, because this suite exists for the one feature where they part.
struct Stereo {
	std::vector<float> left;
	std::vector<float> right;
};

/// A synth with its params and an explicit two-sound character, so a change to what the plugin
/// loads with cannot quietly change what these tests measure. The zones straddle the bank, Path
/// sits off both anchors, and Pos X stays ON THE BINARY GRID — every fixture displacement here is a
/// power of two, so (tour + posX) + ear and tour + (posX + ear) are the same float and the
/// equivalence cases compare sounds rather than association order.
struct Rig {
	ParamSet params;
	Synth synth{params};

	explicit Rig(OscType type) {
		synth.activate(kSampleRate, 4096);
		set(ParamId::OscType, static_cast<double>(type));
		set(ParamId::AZone, 17.0);
		set(ParamId::BZone, 800.0);
		set(ParamId::Path, 0.25);
		set(ParamId::PosX, 0.125);
		// A FIXED LEVEL: the default patch's Level base is zero, carried by the strip's envelope through
		// a route only the bridge makes (backlog EF); a bare Synth needs the old fixed level said.
		set(ParamId::VoiceLevel, 1.0);
	}

	void set(ParamId id, double value) { params.setBase(static_cast<clap_id>(id), value); }

	void width(size_t slot, double w) {
		params.setBase(static_cast<clap_id>(soundParam(slot, SoundField::Width)), w);
	}

	/// The uniform stereo stance most cases use: the master scale full up and both sounds equally
	/// wide, so the effective separation (V13b: global × w_k) is exactly `w` and the ears stand at
	/// ±w/2 — the shipped global-width geometry, reached through the per-sound algebra.
	void widen(double w) {
		set(ParamId::Width, 1.0);
		width(0, w);
		width(1, w);
	}

	/// Play one held note over `frames` samples with `script` applied at its frames, rendered in
	/// blocks of `block` and cut at every event exactly as the CLAP binding cuts.
	Stereo play(const std::vector<Event>& script, uint32_t frames, int block) {
		Stereo out;
		out.left.assign(frames, 0.0f);
		out.right.assign(frames, 0.0f);
		synth.noteOn(0, kKeyC4, -1, 1.0);

		size_t next = 0;
		uint32_t at = 0;
		while (at < frames) {
			const uint32_t blockEnd = std::min(frames, at + static_cast<uint32_t>(block));
			while (at < blockEnd) {
				while (next < script.size() && script[next].frame <= at) {
					set(script[next].param, script[next].value);
					next++;
				}
				uint32_t until = blockEnd;
				if (next < script.size()) {
					until = std::min(blockEnd, script[next].frame);
				}
				synth.render(out.left.data() + at, out.right.data() + at, static_cast<int>(until - at));
				at = until;
			}
		}
		return out;
	}
};

/// Bit equality over a window. The claims here are identity claims; a tolerance would let exactly
/// the drift they exist to catch through.
int bitDiffs(const std::vector<float>& a, const std::vector<float>& b, size_t from, size_t to) {
	REQUIRE(a.size() >= to);
	REQUIRE(b.size() >= to);
	int diffs = 0;
	for (size_t i = from; i < to; i++) {
		diffs += a[i] == b[i] ? 0 : 1;
	}
	return diffs;
}

float peakOf(const std::vector<float>& x, size_t from, size_t to) {
	float peak = 0.0f;
	for (size_t i = from; i < std::min(to, x.size()); i++) {
		peak = std::max(peak, std::abs(x[i]));
	}
	return peak;
}

bool allFinite(const std::vector<float>& x) {
	for (float v : x) {
		if (!std::isfinite(v)) {
			return false;
		}
	}
	return true;
}

} // namespace

TEST_CASE("no wide sound is mono, whichever factor of the algebra is zero") {
	// The mono guarantee is structural — the mono render path is the untouched code — and this is
	// its observable half, stated over V13b's whole algebra (separation = global × w_k): both
	// channels identical for a patch that never touched any width, for one with the master scale up
	// over all-zero per-sound widths, for one with wide sounds under a zero master scale, and for
	// the era reading — global up, per-sound values absent, which is every patch saved in the brief
	// global-width window (V13b: "comes back mono — pre-release, single-user patches, accepted"; the
	// loader half, 0.6 in 0.6 back, is pinned in test_params.cpp). The KNOWN-BAD is the same patch
	// with both factors up: the very comparison that certifies silence here reports a difference
	// there, so a width leak into any mono path could not pass unseen.
	constexpr uint32_t kFrames = 4096;

	auto silentOwned = std::make_unique<Rig>(OscType::V5); // on the heap: five Synths outgrow a default stack
	Rig& silent = *silentOwned;
	// THE DEFAULT PATCH IS THIS FIRST RIG, and since V13b the master scale DEFAULTS TO 1 — checked
	// here so a regression of that deliberate flip cannot hide inside "still mono". Mono-by-default
	// is carried entirely by the per-sound zeros.
	CHECK(silent.params.base(static_cast<clap_id>(ParamId::Width)) == 1.0);
	const Stereo atZero = silent.play({}, kFrames, 256);
	CHECK(bitDiffs(atZero.left, atZero.right, 0, kFrames) == 0);
	CHECK(peakOf(atZero.left, 256, kFrames) > 0.05f);

	auto scaleUpNothingWideOwned = std::make_unique<Rig>(OscType::V5); // on the heap: five Synths outgrow a default stack
	Rig& scaleUpNothingWide = *scaleUpNothingWideOwned;
	scaleUpNothingWide.set(ParamId::Width, 1.0);
	scaleUpNothingWide.width(0, 0.0);
	scaleUpNothingWide.width(1, 0.0);
	const Stereo scaleOnly = scaleUpNothingWide.play({}, kFrames, 256);
	CHECK(bitDiffs(atZero.left, scaleOnly.left, 0, kFrames) == 0);
	CHECK(bitDiffs(atZero.right, scaleOnly.right, 0, kFrames) == 0);

	auto wideUnderZeroScaleOwned = std::make_unique<Rig>(OscType::V5); // on the heap: five Synths outgrow a default stack
	Rig& wideUnderZeroScale = *wideUnderZeroScaleOwned;
	wideUnderZeroScale.set(ParamId::Width, 0.0);
	wideUnderZeroScale.width(0, 1.0);
	wideUnderZeroScale.width(1, 1.0);
	const Stereo gated = wideUnderZeroScale.play({}, kFrames, 256);
	CHECK(bitDiffs(atZero.left, gated.left, 0, kFrames) == 0);
	CHECK(bitDiffs(atZero.right, gated.right, 0, kFrames) == 0);

	auto eraPatchOwned = std::make_unique<Rig>(OscType::V5); // on the heap: five Synths outgrow a default stack
	Rig& eraPatch = *eraPatchOwned;
	eraPatch.set(ParamId::Width, 0.6);
	const Stereo era = eraPatch.play({}, kFrames, 256);
	CHECK(bitDiffs(atZero.left, era.left, 0, kFrames) == 0);
	CHECK(bitDiffs(atZero.right, era.right, 0, kFrames) == 0);

	auto wideOwned = std::make_unique<Rig>(OscType::V5); // on the heap: five Synths outgrow a default stack
	Rig& wide = *wideOwned;
	wide.widen(0.4);
	const Stereo apart = wide.play({}, kFrames, 256);
	CHECK(bitDiffs(apart.left, apart.right, 0, kFrames) > 0);
	CHECK(bitDiffs(apart.left, atZero.left, 0, kFrames) > 0);
	CHECK(peakOf(apart.left, 256, kFrames) > 0.05f);
	CHECK(peakOf(apart.right, 256, kFrames) > 0.05f);
}

TEST_CASE("the frozen engines are mono at every width") {
	// V5 ONLY, by design (V11): Morph and Stair stay strictly mono, so Width on their patches changes
	// nothing — not the blend, not the channels, not a bit. The KNOWN-BAD is the identical sweep on
	// a V5 patch, which the same comparisons see immediately: the test can tell a width leak from a
	// no-op, so a frozen engine routed through the ear displacement could not pass.
	constexpr uint32_t kFrames = 4096;

	// Resonate too: its voices scan one string, mono by construction (the stereo dual-tap is backlog).
	for (OscType type : { OscType::Morph, OscType::Stair, OscType::Resonate }) {
		CAPTURE(static_cast<int>(type));
		auto referenceOwned = std::make_unique<Rig>(type); // on the heap: five Synths outgrow a default stack
		Rig& reference = *referenceOwned;
		const Stereo atZero = reference.play({}, kFrames, 256);
		CHECK(bitDiffs(atZero.left, atZero.right, 0, kFrames) == 0);

		for (double width : { 0.3, 1.0 }) {
			CAPTURE(width);
			auto sweptOwned = std::make_unique<Rig>(type); // on the heap: five Synths outgrow a default stack
			Rig& swept = *sweptOwned;
			swept.widen(width);
			const Stereo out = swept.play({}, kFrames, 256);
			CHECK(bitDiffs(out.left, atZero.left, 0, kFrames) == 0);
			CHECK(bitDiffs(out.right, atZero.right, 0, kFrames) == 0);
		}
	}

	auto flagshipOwned = std::make_unique<Rig>(OscType::V5); // on the heap: five Synths outgrow a default stack
	Rig& flagship = *flagshipOwned;
	const Stereo v5AtZero = flagship.play({}, kFrames, 256);
	auto flagshipWideOwned = std::make_unique<Rig>(OscType::V5); // on the heap: five Synths outgrow a default stack
	Rig& flagshipWide = *flagshipWideOwned;
	flagshipWide.widen(0.3);
	const Stereo v5Wide = flagshipWide.play({}, kFrames, 256);
	CHECK(bitDiffs(v5Wide.left, v5AtZero.left, 0, kFrames) > 0);
}

TEST_CASE("the uniform case is the render Pos X would make standing there") {
	// V13's REDUCTION CASE, the spec as a test: with every enabled sound at the same width w and the
	// global at g, displacing each anchor by +s per ear L (s = g·w/2) is RELATIVELY identical to
	// displacing the puck by −s — everything downstream of the geometry consumes (puck − anchor) —
	// so the left channel must be the MONO render of the same patch with Pos X moved by −s, the same
	// build road end to end, bit for bit, and the right likewise at +s. This is exactly the shipped
	// puck-displacement behaviour at separation g·w, which is what the design requires the uniform
	// case to reduce to. It also carries the symmetry claim: swapping the ears IS mirroring the
	// displacement, because each channel is pinned to its own displaced mono render.
	//
	// TWO FACTORISATIONS OF THE SAME SEPARATION, because the algebra is the claim: g=1.0 over
	// w=0.25 and g=0.5 over w=0.5 must land on the SAME pair of mono references — separation is
	// global × w_k and nothing else.
	//
	// THE FIXTURE MUST STAY OFF THE RIM: Pos X clamps at ±1, so this only compares sounds while
	// PosX ± s lies inside the declared range. The rig's 0.125 with s = 0.125 leaves the whole
	// disk of slack — an edit that moved the fixture near ±1 would silently turn the spec test into
	// a clamp test. Every value is a power of two, per the rig note: anchors at ∓0.5, and both
	// g·w products and their halves are exact.
	constexpr uint32_t kFrames = 4096;
	constexpr double kSeparation = 0.25;

	auto leftStandOwned = std::make_unique<Rig>(OscType::V5); // on the heap: five Synths outgrow a default stack
	Rig& leftStand = *leftStandOwned;
	leftStand.set(ParamId::PosX, 0.125 - kSeparation / 2.0);
	const Stereo atLeft = leftStand.play({}, kFrames, 256);

	auto rightStandOwned = std::make_unique<Rig>(OscType::V5); // on the heap: five Synths outgrow a default stack
	Rig& rightStand = *rightStandOwned;
	rightStand.set(ParamId::PosX, 0.125 + kSeparation / 2.0);
	const Stereo atRight = rightStand.play({}, kFrames, 256);

	struct Factoring {
		double global;
		double perSound;
	};
	for (const Factoring f : { Factoring { 1.0, 0.25 }, Factoring { 0.5, 0.5 } }) {
		CAPTURE(f.global);
		auto wideOwned = std::make_unique<Rig>(OscType::V5); // on the heap: five Synths outgrow a default stack
		Rig& wide = *wideOwned;
		wide.set(ParamId::Width, f.global);
		wide.width(0, f.perSound);
		wide.width(1, f.perSound);
		const Stereo ears = wide.play({}, kFrames, 256);

		CHECK(bitDiffs(ears.left, atLeft.left, 0, kFrames) == 0);
		CHECK(bitDiffs(ears.right, atRight.left, 0, kFrames) == 0);
	}
	// And the two standing points genuinely sound different, or the runs above just agreed on
	// nothing.
	CHECK(bitDiffs(atLeft.left, atRight.left, 0, kFrames) > 0);
}

TEST_CASE("width is per sound: the wide sound decorrelates and the mono sound stays mono") {
	// THE POINT OF V13, isolated honestly: each sound rendered ALONE under identical width settings,
	// because the blend is nonlinear and per-sound contributions cannot be subtracted out of a mix.
	// Weather is up so the displacement reaches the travel term — a single sound's blend weight is 1
	// wherever the ears stand, so at Weather 0 a lone sound's width is inaudible (true of the
	// shipped global width too: only relative geometry was ever heard). The KNOWN-BADs are built in:
	// the same lone sound with its width up decorrelates under the very comparison that certifies
	// mono, and a PARKED sound's width must not open the stereo window at all — a disabled sound is
	// not in the character.
	constexpr uint32_t kFrames = 4096;

	const auto solo = [](double widthA) {
		auto rigOwned = std::make_unique<Rig>(OscType::V5); // on the heap: five Synths outgrow a default stack
		Rig& rig = *rigOwned;
		rig.set(ParamId::Weather, 0.5);
		rig.set(ParamId::BEnable, 0.0);
		rig.set(ParamId::Width, 1.0);
		rig.width(0, widthA);
		return rig.play({}, kFrames, 256);
	};

	const Stereo wide = solo(0.5);
	CHECK(bitDiffs(wide.left, wide.right, 0, kFrames) > 0);
	CHECK(peakOf(wide.left, 256, kFrames) > 0.05f);

	const Stereo mono = solo(0.0);
	CHECK(bitDiffs(mono.left, mono.right, 0, kFrames) == 0);
	CHECK(peakOf(mono.left, 256, kFrames) > 0.05f);

	// The parked sound: B is out of the constellation, so its width buys nothing and the render
	// stays the untouched mono path — bit-identical to the lone-A render above.
	auto parkedOwned = std::make_unique<Rig>(OscType::V5); // on the heap: five Synths outgrow a default stack
	Rig& parked = *parkedOwned;
	parked.set(ParamId::Weather, 0.5);
	parked.set(ParamId::BEnable, 0.0);
	parked.set(ParamId::Width, 1.0);
	parked.width(1, 0.9);
	const Stereo idle = parked.play({}, kFrames, 256);
	CHECK(bitDiffs(idle.left, mono.left, 0, kFrames) == 0);
	CHECK(bitDiffs(idle.right, mono.right, 0, kFrames) == 0);
}

TEST_CASE("preview suspends the stereo window") {
	// V12a at the synth: the mapping already suspends the displacement while a preview is latched
	// (pinned in the blend suite); this pins the synth's own half — a previewing patch never opens
	// the stereo window, so both channels are the one previewed sound, bit-identical. The KNOWN-BAD
	// is the same patch with the preview released: the same comparison sees stereo immediately.
	constexpr uint32_t kFrames = 4096;

	auto listeningOwned = std::make_unique<Rig>(OscType::V5); // on the heap: five Synths outgrow a default stack
	Rig& listening = *listeningOwned;
	listening.widen(0.5);
	feathers::Preview preview;
	preview.slot = 0;
	listening.synth.setPreview(preview);
	const Stereo held = listening.play({}, kFrames, 256);
	CHECK(bitDiffs(held.left, held.right, 0, kFrames) == 0);
	CHECK(peakOf(held.left, 256, kFrames) > 0.05f);

	auto releasedOwned = std::make_unique<Rig>(OscType::V5); // on the heap: five Synths outgrow a default stack
	Rig& released = *releasedOwned;
	released.widen(0.5);
	const Stereo open = released.play({}, kFrames, 256);
	CHECK(bitDiffs(open.left, open.right, 0, kFrames) > 0);
}

TEST_CASE("block size does not change the samples while width automates") {
	// The invariance rule, held through the one feature that doubles the render: the same script
	// lands its builds at the same samples whatever the block size, because the render never
	// crosses samplesUntilPendingBuild. This is also the determinism claim with teeth: a
	// nondeterministic render could not match itself across two different choppings.
	constexpr uint32_t kFrames = 4096;

	// Both rigs stand their sounds fully wide, so the automated GLOBAL is the whole separation —
	// the master scale exercised as the live control it now is (V13b).
	const auto invariant = [](const std::vector<Event>& script) {
		auto oneOwned = std::make_unique<Rig>(OscType::V5); // on the heap: five Synths outgrow a default stack
		Rig& one = *oneOwned;
		one.width(0, 1.0);
		one.width(1, 1.0);
		const Stereo whole = one.play(script, kFrames, 4096);

		auto otherOwned = std::make_unique<Rig>(OscType::V5); // on the heap: five Synths outgrow a default stack
		Rig& other = *otherOwned;
		other.width(0, 1.0);
		other.width(1, 1.0);
		const Stereo chopped = other.play(script, kFrames, 61);

		CHECK(bitDiffs(whole.left, chopped.left, 0, kFrames) == 0);
		CHECK(bitDiffs(whole.right, chopped.right, 0, kFrames) == 0);
		CHECK(bitDiffs(whole.left, whole.right, 900, 1800) > 0); // the width really was up in between
	};

	SUBCASE("moves an idle guard apart") {
		invariant({ { 700, ParamId::Width, 0.6 }, { 2000, ParamId::Width, 0.15 } });
	}

	SUBCASE("two moves inside one guard window — a stereo build DEFERS end to end") {
		// The move at 700 builds on the spot and arms the ~100-sample guard; the move at 750 lands
		// inside it, so this build is HELD and released only when the guard drains — the one path
		// where samplesUntilPendingBuild actually bounds a render chunk. The whole-buffer run has no
		// block boundary anywhere near the landing sample, so it must cut its own chunk there; a
		// renderer that crossed the guard mid-chunk would land the build at a host-chosen sample and
		// part company with the chopped run right here.
		invariant({ { 700, ParamId::Width, 0.6 }, { 750, ParamId::Width, 0.9 } });
	}

	SUBCASE("a per-sound width automates the same everywhere") {
		// The new params through the same machinery: the global stands at 1 from before the note and
		// ONE SOUND's width moves mid-performance — per-sound automation lands its builds at the same
		// samples whatever the chopping.
		invariant({ { 0, ParamId::Width, 1.0 },
		            { 700, soundParam(0, SoundField::Width), 0.6 },
		            { 2000, soundParam(0, SoundField::Width), 0.15 } });
	}
}

TEST_CASE("two performances of the same script are the same performance") {
	constexpr uint32_t kFrames = 2048;
	// A per-sound width is the moving part, under a standing master scale — determinism claimed on
	// the new automation lane itself.
	const std::vector<Event> script = { { 300, soundParam(0, SoundField::Width), 0.5 } };

	auto firstOwned = std::make_unique<Rig>(OscType::V5); // on the heap: five Synths outgrow a default stack
	Rig& first = *firstOwned;
	first.set(ParamId::Width, 1.0);
	first.width(1, 0.25);
	const Stereo a = first.play(script, kFrames, 128);
	auto secondOwned = std::make_unique<Rig>(OscType::V5); // on the heap: five Synths outgrow a default stack
	Rig& second = *secondOwned;
	second.set(ParamId::Width, 1.0);
	second.width(1, 0.25);
	const Stereo b = second.play(script, kFrames, 128);

	// NOT SILENCE (backlog MA): two silent performances agree whatever the code does, so both ears must sound.
	REQUIRE(peakOf(a.left, 0, kFrames) > 0.05f);
	REQUIRE(peakOf(a.right, 0, kFrames) > 0.05f);
	CHECK(bitDiffs(a.left, b.left, 0, kFrames) == 0);
	CHECK(bitDiffs(a.right, b.right, 0, kFrames) == 0);
}

TEST_CASE("the seams: widening mid-note, collapsing back, and the accepted engine drop") {
	// Width moved during a held note, both directions, and the accepted consequence of switching to a
	// frozen engine at width: mono, on the record (V11). Each seam is a bank rebuild masked by the
	// ordinary crossfade; what this pins is the state machinery around it — everything finite, the
	// channels apart exactly while a stereo build is live or draining, and BIT-identical again once
	// the collapse's fade and carry have drained and the mono path has the render back.
	constexpr uint32_t kFrames = 4096;
	constexpr uint32_t kWidenAt = 512;
	constexpr uint32_t kCollapseAt = 2048;

	SUBCASE("width up, then back to zero") {
		auto rigOwned = std::make_unique<Rig>(OscType::V5); // on the heap: five Synths outgrow a default stack
		Rig& rig = *rigOwned;
		// The master scale starts CLOSED — explicitly, since its default moved to 1 (V13b) — so the
		// note opens mono and the script's move at kWidenAt is the seam under test.
		rig.set(ParamId::Width, 0.0);
		rig.width(0, 1.0);
		rig.width(1, 1.0);
		const Stereo out = rig.play(
		    { { kWidenAt, ParamId::Width, 0.5 }, { kCollapseAt, ParamId::Width, 0.0 } }, kFrames, 256);

		CHECK(allFinite(out.left));
		CHECK(allFinite(out.right));
		// Mono before the widening, to the bit.
		CHECK(bitDiffs(out.left, out.right, 0, kWidenAt) == 0);
		// Apart while wide — measured after the widening's own fade so the window is honestly stereo.
		CHECK(bitDiffs(out.left, out.right, kWidenAt + static_cast<uint32_t>(kSeamDrain), kCollapseAt)
		      > 0);
		// And mono again — bit-identical, not merely converged — once the collapse drains: the bank's
		// window closes, the synth returns to the mono path, and the right channel is the left one.
		CHECK(bitDiffs(out.left, out.right, kCollapseAt + 2u * static_cast<uint32_t>(kSeamDrain),
		               kFrames)
		      == 0);
		CHECK(peakOf(out.left, kFrames - 1024, kFrames) > 0.05f);
	}

	SUBCASE("the engine switch drops to mono") {
		auto rigOwned = std::make_unique<Rig>(OscType::V5); // on the heap: five Synths outgrow a default stack
		Rig& rig = *rigOwned;
		rig.set(ParamId::Width, 0.0); // closed at the note, per the subcase above
		rig.width(0, 1.0);
		rig.width(1, 1.0);
		const Stereo out = rig.play({ { kWidenAt, ParamId::Width, 0.5 },
		                              { kCollapseAt, ParamId::OscType,
		                                static_cast<double>(OscType::Morph) } },
		                            kFrames, 256);

		CHECK(allFinite(out.left));
		CHECK(allFinite(out.right));
		CHECK(bitDiffs(out.left, out.right, kWidenAt + static_cast<uint32_t>(kSeamDrain), kCollapseAt)
		      > 0);
		CHECK(bitDiffs(out.left, out.right, kCollapseAt + 2u * static_cast<uint32_t>(kSeamDrain),
		               kFrames)
		      == 0);
		CHECK(peakOf(out.left, kFrames - 1024, kFrames) > 0.05f);
	}
}

TEST_CASE("the weight readout of a wide patch is the two ears' mean, and at width 0 it is the mono one") {
	// V13 leaves no head character to read when a patch is wide: there are two evaluations with every
	// anchor displaced by its own sound's width, and the middle is not one of them. So the readout
	// averages the ears, and the claim worth pinning is the boundary — a patch that is not wide must
	// read EXACTLY what it read before this existed, because the two ears are then the same character.
	auto monoOwned = std::make_unique<Rig>(OscType::V5); // on the heap: five Synths outgrow a default stack
	Rig& mono = *monoOwned;
	std::vector<float> l(64, 0.0f);
	std::vector<float> r(64, 0.0f);
	mono.synth.noteOn(0, kKeyC4, -1, 1.0);
	mono.synth.render(l.data(), r.data(), 64);
	const feathers::SoundWeights head = mono.synth.soundWeights();
	REQUIRE(head.live != 0);

	// The same patch with the width parameters up but the master scale at zero: still mono by the
	// algebra, so still the head's reading to the bit rather than to a tolerance.
	auto parkedOwned = std::make_unique<Rig>(OscType::V5); // on the heap: five Synths outgrow a default stack
	Rig& parked = *parkedOwned;
	parked.width(0, 0.8);
	parked.width(1, 0.8);
	parked.set(ParamId::Width, 0.0);
	parked.synth.noteOn(0, kKeyC4, -1, 1.0);
	parked.synth.render(l.data(), r.data(), 64);
	CHECK(parked.synth.soundWeights() == head);

	// AND WIDE IS DIFFERENT, which is what stops the equality above from being a readout that never
	// moves. The ears see different geometry, so their mean is not the head's blend.
	auto wideOwned = std::make_unique<Rig>(OscType::V5); // on the heap: five Synths outgrow a default stack
	Rig& wide = *wideOwned;
	wide.widen(0.8);
	wide.synth.noteOn(0, kKeyC4, -1, 1.0);
	wide.synth.render(l.data(), r.data(), 64);
	const feathers::SoundWeights mean = wide.synth.soundWeights();
	// Both sounds are still in the blend — width moves where they stand, not whether they are there.
	CHECK(mean.live == head.live);
	CHECK(mean.at(0) != head.at(0));
	// And it is still a set of shares: averaging two normalised readings of the same membership keeps
	// the total at one.
	double total = 0.0;
	for (size_t slot = 0; slot < feathers::kMaxSounds; ++slot) {
		total += mean.at(slot);
	}
	CHECK(total == doctest::Approx(1.0));
}

// **BACKLOG MA, CLOSED BY A GUARD RATHER THAN BY AN AUDIT** (2026-10-31). A bare-Synth rig that renders silence makes every
// "unchanged"/"differs" comparison on it vacuous: green whatever the code does. The audit of 2026-10-11 found none silent
// and put a REQUIRE in each comparing case; THIS asks the rig itself, so a rig edited to stop sounding (its Voice Level
// line deleted, its engine left silent) fails here by name, before any case that leans on it.
TEST_CASE("backlog MA: the bare rig's reference render SOUNDS, so a difference test on it can see a difference") {
	const auto peakOf = [](const std::vector<float>& x) { float p = 0.0f; for (const float v : x) { p = std::max(p, std::fabs(v)); } return p; };
	auto rigOwned = std::make_unique<Rig>(OscType::V5); // on the heap: five Synths outgrow a default stack
	Rig& rig = *rigOwned;
	const Stereo out = rig.play({}, 4096, 256);
	CAPTURE(peakOf(out.left));
	REQUIRE(peakOf(out.left) > 0.05f);
	REQUIRE(peakOf(out.right) > 0.05f);
}
