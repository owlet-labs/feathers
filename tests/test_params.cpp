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

// What a saved patch depends on.
//
// Param ids are permanent API: a session saved by any build of feathers has to load in every later
// one, and the failure mode of a changed id is not a crash but somebody's patch quietly sounding
// wrong. Nothing in a compiler stops an id from being renumbered by a careless edit, so the whole
// table is pinned by a fingerprint here, and the state format is round-tripped against the kinds of
// stream a future version will hand it.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "plugin/device_registry.h"
#include "dsp/partial_spectrum.h"
#include "dsp/spectral_shaper.h"
#include "fx/spectral/spectral_types.h"
#include "plugin/params.h"
#include "param_routes.h"
#include "plugin/state_block_order.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {

using feathers::findParam;
using feathers::kMaxSounds;
using feathers::paramIndexOf;
using feathers::soundParam;
using feathers::SoundField;
using feathers::ParamDesc;
using feathers::paramDescs;
using feathers::ParamId;
using feathers::paramIdFingerprint;
using feathers::paramRangeFingerprint;
using feathers::ParamSet;

/// A clap_ostream/clap_istream pair over a byte vector — all a host's stream contract really is.
struct MemoryStream {
	std::vector<uint8_t> bytes;
	size_t readPos{0};

	/// Bytes handed over per call. Hosts are allowed to satisfy a read or write partially, and a
	/// serializer that assumes otherwise works everywhere until it meets the one host that doesn't.
	size_t chunk{SIZE_MAX};

	static int64_t CLAP_ABI writeFn(const clap_ostream* stream, const void* buffer, uint64_t size) {
		auto* self = static_cast<MemoryStream*>(stream->ctx);
		const size_t n = std::min<size_t>(size, self->chunk);
		const auto* src = static_cast<const uint8_t*>(buffer);
		self->bytes.insert(self->bytes.end(), src, src + n);
		return static_cast<int64_t>(n);
	}

	static int64_t CLAP_ABI readFn(const clap_istream* stream, void* buffer, uint64_t size) {
		auto* self = static_cast<MemoryStream*>(stream->ctx);
		const size_t available = self->bytes.size() - self->readPos;
		const size_t n = std::min<size_t>(std::min<size_t>(size, self->chunk), available);
		std::memcpy(buffer, self->bytes.data() + self->readPos, n);
		self->readPos += n;
		return static_cast<int64_t>(n);
	}

	clap_ostream out() { return clap_ostream{this, writeFn}; }
	clap_istream in() {
		readPos = 0;
		return clap_istream{this, readFn};
	}
};

/// Build a state stream by hand, so a test can express a patch this build has never written —
/// including one carrying params it does not know.
/// **THE DEFAULT IS `kStateVersion` ITSELF NOW, NOT A COPY OF ITS VALUE.** This said *"the number
/// moves here whenever it moves there"* and carried a literal `2`; when the byte went to 3 on
/// 2026-09-10 three cases in this file went red for no reason but the duplicate. These cases are about
/// the PARAMETER half of the stream and do not care which version it is, only that it is the CURRENT
/// one — which is a thing the code can say, so it says it. The refusal subcase below asks for
/// `kStateVersion + 1` on the same principle.
std::vector<uint8_t> makeState(const std::vector<std::pair<uint32_t, double>>& entries,
                               uint8_t version = feathers::kStateVersion) {
	std::vector<uint8_t> bytes;
	const auto pushU32 = [&bytes](uint32_t v) {
		for (int i = 0; i < 4; ++i) {
			bytes.push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xffU));
		}
	};
	bytes.push_back(version);
	pushU32(static_cast<uint32_t>(entries.size()));
	for (const auto& [id, value] : entries) {
		pushU32(id);
		uint64_t bits = 0;
		std::memcpy(&bits, &value, sizeof(bits));
		for (int i = 0; i < 8; ++i) {
			bytes.push_back(static_cast<uint8_t>((bits >> (i * 8)) & 0xffU));
		}
	}
	return bytes;
}

/// A value inside a param's range that is not its default, so "did this survive?" can be answered
/// without the default masking a failure.
double distinctFrom(const ParamDesc& desc) {
	const double quarter = desc.min + 0.25 * (desc.max - desc.min);
	const double threeQuarters = desc.min + 0.75 * (desc.max - desc.min);
	const double candidate = std::abs(quarter - desc.def) > std::abs(threeQuarters - desc.def)
	                             ? quarter
	                             : threeQuarters;
	// Stepped params only store integers; a fractional base would come back rounded by the host and
	// the round-trip claim would be about the wrong thing.
	return (desc.flags & CLAP_PARAM_IS_STEPPED) != 0 ? std::round(candidate) : candidate;
}

} // namespace

TEST_CASE("the param table is what it was") {
	// THESE CONSTANTS ARE PROMISES, and there are two of them so that a failure says which promise
	// broke. If either fails and you did not mean to touch the table, you have broken every patch
	// anyone has saved — find the edit and revert it. If you DID mean to, read what the failing
	// subcase is telling you, then paste the value it prints below.

	SUBCASE("ids and names") {
		// FNV-1a over the ordered (id, name) list: changes if a param is added, removed, renamed,
		// renumbered or reordered. This is how a saved patch and a host's automation lane address a
		// param, so a change here lands an old patch on the wrong knob or on none. Appending a param
		// is the one ordinary reason to update this — check that no existing id moved first.
		// Moved 2026-08-18 (second move that day): PER-SOUND WIDTH, decided — V13: "Eight new ids in the
		// free +3 placement slots (0x0203, 0x0213 … 0x0273, SoundField::Width), each 0..1 default 0 =
		// that sound mono." Eight new (id, name) pairs — "A Width" … "H Width", each in its own sound's
		// module — seated in the table beside each sound's placement rows. New ids only; no existing
		// pair moved, and the distinctness and id-block cases below still say so mechanically.
		// Moved 2026-08-18 (fourth move that day): THE FX LANE'S FIRST TENANT, the multimode filter,
		// appended as eight rows at 0x0300..0x0307 — Filter Enabled, Type, Cutoff, Res, Drive,
		// Keytrack, Macro 1 and Macro 2, all in a module called "Filter". Eight new (id, name) pairs
		// at the END of the table; nothing existing moved, and the distinctness and id-block cases
		// below still say so mechanically.
		//
		// Two things about this block are unlike every other append here. Its rows are BUILT from the
		// device's own descriptors under src/fx rather than written out (see filterRow() in
		// params.cpp), so the names in this fingerprint are the DEVICE's names — reword one there and
		// this moves. And those names qualify themselves ("Filter Macro 1"), which is not decoration:
		// the bare spelling collided with the four global "Macro n" rows, and the distinctness case
		// below is what found it.
		// Recomputed from the failing subcase's printout, per this test's own protocol — never picked,
		// and recomputed again if another session's ids land before this commit does.
		// Moved 2026-08-19: 0x0064 RENAMED "Phase Catchup" -> "Osc Mode", widened from a two-state
		// toggle to the three-way V24 decided — a dropdown for Osc Mode with Digi, Analog and Phase
		// Catchup. THE ID DID NOT MOVE and none was added or removed; what changed is
		// the NAME in the pair, which this fingerprint covers on purpose, because a host's automation
		// lane shows the name and a patch's stored value now means one of three things rather than
		// one of two. The rename was free to make and only briefly so: 0x0064 has never been in an
		// installed build, so no saved patch carries a value under the old reading — after the first
		// install that stops being true forever. Recomputed from the failing subcase's printout, per
		// this test's own protocol. (The move before it was that param's own arrival, the day before.)
		// MERGED 2026-08-19, and the value below is neither side's: two sessions moved this hash in
		// parallel — the fx lane appended eight filter rows, and 0x0064 was renamed for V24 — so the
		// merge recomputes from the merged table, which is the protocol's whole point. Picking either
		// side's constant here would have pinned a table that never existed.
		// Moved 2026-08-19 (again, same session): FILTER SPREAD appended at 0x0309, alongside the Slope
		// row below. Two new pairs in this block now, and Spread's arrival is a PROMOTION rather than a
		// new control — it was macro 1 for five of the eight types and is now a param of its own,
		// because a macro is for a meaning that changes with the type and Spread's never did. Nothing
		// existing moved. What DID change under a stored value: macro 1 no longer means Spread for
		// those five types, so a patch saved with that macro turned up comes back with the macro at
		// its stored number and no meaning attached to it. That cost was accepted, as there were no saved
		// patches to protect. Recomputed from the failing subcase's printout, per this test's protocol.
		// Moved 2026-08-19: FILTER SLOPE appended at 0x0308 — one new (id, name) pair, "Filter Slope"
		// in the "Filter" module, at the END of the block. Nothing existing moved. Worth noting WHY
		// this id was free to take: the comment in params.h used to promise 0x0308 to a Mix control
		// that does not exist, and no comment can hold a slot — the rows are built from the device's
		// descriptor list by position, so whatever the device appends next takes the next id. Mix is
		// now first in line for 0x0309. Recomputed from the failing subcase's printout, per this
		// test's own protocol.
// MOVED 2026-08-23: ParamId::FilterPoly appended at 0x030A.
//
// A DELIBERATE CHANGE, AND THE PROTOCOL THIS FILE STATES IS TO ADD A LINE RATHER THAN SWAP THE
// NUMBER. Poly and keytrack were one control: keytrack ON gave one filter per voice, keytrack OFF
// gave one on the summed bus, so the plugin shipped two of the four combinations. Poly and keytrack are
// two different things.
//
// Keytrack is TONAL — does a voice's corner follow its note. Poly is ARCHITECTURAL — how many
// filters there are. Keytrack implies poly (a corner cannot follow "the note" on a bus carrying six
// of them) and poly does not imply keytrack, and THAT missing combination is the interesting one:
// this filter is nonlinear, so six voices through six filters at one corner is a different sound
// from six voices through one filter at that corner. Saturation of a sum is not the sum of
// saturations.
//
// BOTH fingerprints moved because the id list grew AND a range came with it; neither existing
// control's id or range changed, which is what appending is for.
// REMOVED 2026-08-23: the filter's eleven ids, 0x0300..0x030A, and their ranges with them.
//
// THE PROTOCOL THIS FILE STATES IS TO ADD A LINE RATHER THAN SWAP THE NUMBER, and this is the first
// entry that RETIRES ids rather than appending them — so it is worth being exact about what moved.
// Every remaining id and every remaining range is unchanged; both fingerprints moved because eleven
// entries left the ordered list, not because anything surviving was renumbered or re-ranged.
//
// A DELIBERATE CHANGE, AND A BREAKING ONE. A docked effect's controls are not on Feathers' CLAP surface:
// they are swallowed and reached by internal modulation. A docked
// device's controls are its own state now, carried in its chain row and reached by the route table.
// What it costs is stated at ParamId: automation lanes written against those ids stop resolving, and
// every modulation destination that sat after them shifts index. Both are sanctioned by the project's
// patch-compatibility policy — no saved patches, no other users — and both stop being free the day
// that policy expires.
//
// THE BLOCK IS NOT RESERVED. A future tenant may have 0x0300; holding it open to honour ids nothing
// can reach would be exactly the wart that policy says to refuse.
// ADDED 2026-08-25: forty ids — the five FIELD MODIFIER knobs at +6..+A in each of the eight
// sound blocks. AQuantize..HQuantize (0x0206 stride 0x10), then Sync (+7), Feedback (+8),
// Points (+9), Grit (+A): field modifier knobs on the Explore view, as mod targets.
//
// A PURE APPEND INSIDE EACH BLOCK, which is what makes this the boring kind of entry: every
// pre-existing id keeps its number, nothing is renumbered, and the blocks had ten free offsets
// apiece (six of sixteen were in use). BOTH fingerprints moved because forty entries joined the
// ordered list and each brought a range.
//
// WHAT DID SHIFT, stated because it is invisible from here: MODULATION DESTINATION INDICES.
// kPerSoundDestinationsPerSlot went 3 -> 8, and the per-sound run is slot-major, so every per-sound
// destination above slot 0 moved. Sanctioned by the project's patch-compatibility policy — no saved
// patches, no other users — and it stops being free the day that policy expires. The fx lane's
// destinations are unaffected: the per-sound run sits after them, deliberately.
		//
		// ADDED 2026-08-28: NINE ids — one ENGINE-WIDE `WarpMode` (0x0002, beside OscType) and the eight
		// per-sound `AWarp`..`HWarp` depth knobs at +B in each sound block. The other warp modes are ported
		// with a dropdown near the knob and no phi field: Warp is a plain modifier knob, as in Serum.
		// See dsp/warp.h.
		//
		// A PURE APPEND AGAIN: every pre-existing id keeps its number, and the sound blocks had five free
		// offsets apiece (+B..+F) after the 2026-08-25 five landed at +6..+A.
		//
		// THE SPLIT IS FORCED, NOT CHOSEN — the depth is per sound because amounts blend, and the mode is
		// engine-wide because AN ENUM CANNOT BE BLENDED: the flagship renders one cycle from the blended
		// recipe, so a per-sound mode would have to be resolved by dominant weight and would step
		// mid-crossfade.
		//
		// AND THE DEFAULT IS 0, NOT kFieldNeutral — which is why the RANGE fingerprint moves by more than
		// nine entries' worth of ids would suggest. Warp is an AMOUNT with no phi field behind it, so 0
		// means "none"; the five knobs beside it are BIASES over a field, where 0.5 means "as authored".
		//
		// WHAT SHIFTED: kPerSoundDestinationsPerSlot went 8 -> 9, so every per-sound modulation destination
		// above slot 0 moved again. Sanctioned by the project's patch-compatibility policy, and it stops being
		// free the day that policy expires. The MODE is not a destination — it is stepped, and stepped
		// controls are excluded from that table by rule.
		//
		// ADDED 2026-09-25: TWO ids, `StackCount` (0x0065) and `StackDetune` (0x0066), in the voice block's free
		// offsets after OscMode — voice stacking, backlog KE. A pure
		// append: no existing id moves. Stack Detune joins the core destinations at the END of that list, so
		// every per-sound destination index moves up by one (the same sanctioned kind of shift as above).
		// AND A THIRD, `StackSpread` (0x0067, step 2), and one more core destination after Stack Detune.
		//
		// ADDED 2026-09-25: ONE id, `ResonateModel` (0x0003, beside OscType and WarpMode) — the Resonate
		// engine's physics, engine-wide because there is ONE string. A pure append: nothing renumbered.
		// ALL TOGETHER (merged 2026-09-26): Stack, Stack Detune, Stack Spread and ResonateModel.
		// ADDED 2026-09-26: SIX ids, 0x0068-0x006D, the Stack Index's Shape, Shape Amount, Free Phase, Mapping, Mapping
		// Amount and Curve (backlog KE step 3). A pure append; the four continuous ones join the core destinations at
		// the end of that list, so every per-sound destination index moves up by four.
		// ADDED 2026-09-26: NINE ids, 0x0004-0x000C, Resonate's shaping (Width, Bow, Bow Mode, Shimmer, Stiffness,
		// Coupling, Decay, Pluck, Travel). A pure append in the engine-wide block; the eight continuous ones join the
		// core destinations at the end of that list, so every per-sound destination index moves up by eight.
		// 2026-10-03: Attack (0x0050) and Release (0x0051) DELETED, Level (0x0052) renamed Voice Level, a new Level fader at
		// 0x0054, Phase at 0x006E and Phase Width at 0x006F. Ids are free.
		// ADDED 2026-10-03: ONE id, 0x000D, Resonate Speed (the string's tick rate). A pure append in the engine-wide
		// block; it joins the core destinations at the end, so every per-sound destination index moves up by one.
		// BOTH, merged 2026-10-03.
		// ADDED 2026-10-03: FOUR ids, 0x0044-0x0047, the Basic Shapes mix (Sine, Triangle, Saw, Square) after the
		// Resonate block; all four continuous, so they join the core destinations at the end. Also Osc Type's
		// choice list grows to five ("Basic Shapes"). Merged with voice-level 2026-10-03.
		// ADDED 2026-10-03: ONE id, 0x0074, Coarse (continuous pitch in semitones) after Bend Range. A pure append in the
		// pitch block; continuous, so it joins the core destinations at the end.
		// ADDED 2026-10-10: NINE ids, the additive Shape: 0x0048 Additive Shape (a Choice, so not a destination) and the
		// per-sound Shape Amount at +C in each sound block (0x020C..0x027C), which joins each slot's destination run at its end.
		// Osc Type's choice list grows to six ("Additive").
		// ADDED 2026-10-10: FORTY-EIGHT ids, the six additive bias knobs (Rolloff, Formant, Odd/Even, Comb, Detail, Off-grid)
		// in a second per-sound block, 0x0280 + slot·0x10 + 0..5; all continuous, so each slot's destination run grows by six.
		// ADDED 2026-10-10: 152 ids, the spectral drawer PER SOUND, 0x0400 + slot·0x20 + 0..18 (it was engine-wide at 0x0380 for one
		// commit, never merged); each slot's twelve continuous controls join that slot's destination run (16 -> 28), the ons,
		// Filter Mode and Link (stepped) do not.
		// ADDED 2026-10-12: TWENTY-FOUR ids, the vowel Formant per sound at +0x13..+0x15 of each spectral block (On, Vowel,
		// Depth); Vowel and Depth join each slot's destination run (28 -> 30), the On does not.
		// ADDED 2026-10-12: SIXTEEN ids, Partials per sound at +0x16..+0x17 (On, Count); Count joins the run (30 -> 31).
		// ADDED 2026-10-12: TWENTY-FOUR ids, Damp per sound at +0x18..+0x1A (On, Time, Tilt); Time and Tilt join the run
		// (31 -> 33).
		// ADDED 2026-10-12: SIXTEEN ids, Stretch per sound at +0x1B..+0x1C (On, Amount); Amount joins the run (33 -> 34).
		// ADDED 2026-10-12: SIXTEEN ids, Shift per sound at +0x1D..+0x1E (On, Hz); Hz joins the run (34 -> 35).
		// ADDED 2026-10-18: THREE ids, A-PH: 0x0049 Additive Phases (a Choice), 0x004A Additive Sub and 0x004B Additive Ceiling
		// (continuous, appended to the core destinations).
		// ADDED 2026-10-19: SEVENTY-TWO ids, F2 per sound in a SECOND spectral block, 0x0500 + slot·0x10 + 0..8 (Filter Gain, Filter 2
		// On, Mode, Cutoff, Resonance, Slope, Key Track, Gain, Routing); the six continuous join each slot's destination run at its
		// end (35 -> 41). No id of the first block moved.
		// ADDED 2026-10-19: EIGHT ids, OH's Partials Mode per sound at +9 of the second block (stepped: no destination).
		// ADDED 2026-10-19: EIGHT ids, OI's Partials Set per sound at +10 of the second block (continuous: each slot's run 41 -> 42).
		// ADDED 2026-10-26: EIGHT ids, OF's Partials Depth per sound at +11 of the second block (continuous: each slot's run 42 -> 43).
		// ADDED 2026-10-26: FIFTY-FOUR ids, OE (the engine vocoder, additive design §9.4): the analysis engine-wide at 0x004C..0x004F
		// (Resolution, a Choice; Bands, Attack, Release, continuous, appended to the core destinations) and 0x0055..0x0056 (Freeze, Key
		// Track; 0x0050/0x0051 stay unanswered), and per sound a THIRD spectral block, 0x0580 + slot·0x10 + 0..5 (On; Depth, Flatten,
		// Gate, Low, High, continuous: each slot's run 43 -> 48).
		// ADDED 2026-10-27: FORTY-THREE ids, OE2: Additive Vocode Spacing, Widths and Stereo (0x0057..0x0059, Choices), and per sound
		// the third spectral block's +6..+10 (Bandwidth, Spacing Amount, Spacing Free, Widths Amount, Widths Free; continuous: each
		// slot's run 48 -> 53).
		// AND 0x005A, Additive Vocode Sibilance (continuous, appended to the core destinations), the same day.
		// ADDED 2026-10-31: FORTY-EIGHT ids, the Karplus-Strong engine (Pluck): 0x00C0..0x00C7 engine-wide (Decay, Decay Track,
		// Release, Hardness, Pickup, Width and Velocity continuous and appended to the core destinations; Seed stepped), and per sound the
		// second per-sound block's +6..+A (Brightness, Sustain, Pick, Noise, Stretch; continuous: each slot's run 53 -> 58).
		// ADDED 2026-11-05: 181 ids, the drum engine: 0x00C8..0x00CC engine-wide (Mode, Pitched Kind and Seed stepped; Walk X and Y
		// continuous, appended to the core destinations), and per sound a block of 22 at 0x0600 + slot·0x20 (sixteen continuous, in
		// each slot's run after the spectral drawer's: 58 -> 74).
		// AND 2026-11-06: eighty more, each pad's snare envelopes and moving bell (+0x16..+0x1F of its block; continuous: each
		// slot's run 74 -> 84).
		// AND 2026-11-06 (S9): forty-eight more, each pad's kick shape (Hold, Dip, Dip Time, Click Sweep, Click Sweep Time,
		// continuous; Start Phase continuous) in a SECOND per-pad block, 0x0700 + slot·0x10 + 0..5 (each slot's run 84 -> 90).
		// AND 2026-11-06 (S10): thirty-two more, each pad's Drive Curve (stepped), Drive Tone, Clap Q and Clap High Pass
		// (continuous) at 0x0700 + slot·0x10 + 6..9 (each slot's run 90 -> 93).
		// AND 2026-11-06 (S11): forty-eight more, each pad's second bell (Link stepped; Ratio, Hz, Time, Q, Level continuous) at
		// +10..+15, and the second block's stride widened to 0x20 (32 a slot), so every second-block id above slot 0 moved
		// (each slot's run 93 -> 98).
		// AND 2026-11-06 (T1): sixteen more, each pad's Velocity Noise and Velocity Tension at +16..+17 (continuous; each
		// slot's run 98 -> 100).
		// AND 2026-11-06 (T3): thirty-two more, each pad's Wire Contour (stepped), Width, Buzz and Buzz Threshold (continuous)
		// at +18..+21 (each slot's run 100 -> 103).
		// AND 2026-11-06 (the kit-wide layer): ten engine-wide, 0x00CD..0x00D6 (Kit Gamma, Zone, Offset and seven field trims;
		// continuous, appended to the core destinations).
		// AND 2026-11-06 (audition): eight more, each pad's Mute (stepped) at +22 of the second block.
		// AND 2026-11-07 (the consolidation): each pad's fourteen absorbed or yielded controls GONE (retired in
		// kCoreRetiredKeys) and three zone coordinates added (Velocity, Wires, Bells), so each pad's controls 52 -> 44 and the
		// blocks re-packed (ids are free: nobody keeps patches).
		// AND 2026-11-07: each pad's Bend (continuous) at the end of the second block, 44 -> 45.
		// AND 2026-11-07 (stage (c), the layer slot): seven more per pad at the end of the second block, 45 -> 52.
		constexpr uint64_t kRecordedIds =  0x7c7af389de9ab129ull;
		CHECK(paramIdFingerprint() == kRecordedIds);
	}

	SUBCASE("ranges and defaults") {
		// FNV-1a over the ordered (id, min, max, default) list. A narrowed range is the quieter
		// breakage and the reason this is pinned separately: the old patch still loads and its stored
		// value is still in the file, but it comes back clamped to something that does not sound the
		// same, with nothing to tell the user it happened. Widening a range or moving a default is
		// legitimate — it is just a decision rather than a refactor, which is what this makes you say
		// out loud.
		// Moved 2026-08-18 (third move that day): the global Width 0x0043's DEFAULT moved 0 → 1, the
		// deliberate half of V13b — "the global Width 0x0043 becomes the MASTER SCALE — effective
		// separation for sound k = global × w_k — and the global's DEFAULT MOVES 0 → 1 (deliberate
		// fingerprint change; the default patch stays mono because the per-sound defaults are 0)."
		// A scale resting at zero would make every per-sound width knob a knob that does nothing
		// until a second knob is found. One default only: no range narrowed, no id moved. The move
		// before it (same day) appended the eight per-sound rows, 0..1 default 0, nothing existing
		// touched.
		// Moved 2026-08-18 (fourth move that day): the filter's eight rows, appended. Ranges and
		// defaults come across from the device's descriptors, and two of them are worth saying out
		// loud because they are tapers rather than plain ranges. FILTER CUTOFF IS STORED AS log2(Hz),
		// log2(20)..log2(20000), default log2(1000) — the same trick the amp edges play with
		// log2(seconds), because a CLAP param has no display curve and a control linear in hertz
		// spends nine tenths of its travel above 2 kHz. And FILTER DRIVE IS AN AMOUNT, 0..1 default 0,
		// not a gain: the shaper is mixed in by that number, so zero is exactly the untouched signal.
		// Nothing existing narrowed or moved. Recomputed from the failing subcase's printout, per this
		// test's own protocol.
		// Moved 2026-08-19: 0x0064's range WIDENED 0..1 -> 0..2 and its default moved 1 -> 2 (V24).
		// A widening, which is the safe direction — no stored value becomes unreachable — and the
		// default move is a deliberate decision rather than a drift: index 1 was "Catchup On" under
		// the old two-state reading and is "Analog" under the new one, so leaving the default alone
		// would have silently changed what every patch means. It follows the mode instead: Phase
		// Catchup keeps the default it was given (V21d, "de-clicked retriggers are the point"),
		// which is now index 2. Nothing else narrowed or moved. Recomputed from the failing
		// subcase's printout, per this test's own protocol.
		// MERGED 2026-08-19: recomputed from the merged table for the reason the ids subcase gives —
		// the filter's eight rows and 0x0064's widening both moved this, and neither side's constant
		// describes the table that now exists.
		// Moved 2026-08-19 (again, same session): Filter Spread's row, appended — bipolar -1..1,
		// DEFAULT 0, which is the neutral this mechanism has always rested at: zero spread runs one
		// stage and is skipped entirely, so a patch that never touches it renders what it always did.
		// Modulatable, unlike Slope beside it, because it is a continuous offset that is musical to
		// sweep — which is the same argument that says it is not a macro.
		// Moved 2026-08-19: Filter Slope's row, appended — a stepped choice 0..3, DEFAULT 1, which is
		// the "12 dB" entry and therefore the filter this device has always been. That default is the
		// load-bearing part of the number: a patch saved before this control existed omits the id and
		// keeps the default (ParamSet::load), so it comes back as the two-pole it was written as. A
		// default of 0 or 3 here would silently re-voice every patch ever saved. Nothing existing
		// narrowed or moved. Recomputed from the failing subcase's printout, per this test's own
		// protocol.
		// Moved 2026-08-22: the eight per-sound Smoothness rows changed DEFAULT from 1.0 to 0.5, and
		// that default is the load-bearing part of this number rather than an incidental. The knob
		// stopped being a MULTIPLIER on the flagship smoothness field and became a bias whose NEUTRAL
		// is the middle of its travel — because the field is alive over one interval of the first
		// eight offset steps of home and is exactly zero at the default coordinate, so a multiplier
		// was a control that did nothing wherever anybody actually stands (found by turning it). 0.5 is
		// the fixed point of fieldWithOverride, so a patch saved before this existed
		// omits the id, takes the default, and renders what it always did — which is the same
		// argument Slope's row makes above, and the reason a default of 1.0 could not stay: under the
		// new shape 1.0 means FULL smoothness, and every such patch would have been re-voiced. The
		// eight FM Depth rows are untouched: their ids exist and no engine reads them yet.
		// Moved 2026-08-22 (again): the eight FM Depth rows changed DEFAULT from 1.0 to 0.0, which is
		// what the FM design doc specified all along ("All eight are 0.0..1.0, default 0.0") and what
		// its identity guarantee rests on — "absent-in-state defaults to 0.0 (off), so no patch ever
		// changes its sound". They were registered at 1.0, which was harmless for exactly as long as
		// nothing read them: the moment the FM engine landed, every existing patch turned FM on at
		// FULL depth. Caught by the mono pin (`left == right`) going red across four synth cases,
		// because a full-depth default also switched the stereo polarity flip on.
		// Moved 2026-08-25 by the forty field-modifier rows, all of them 0.0..1.0 default 0.5.
		// THE DEFAULT IS THE POINT AND IT IS NOT A COMPATIBILITY CONCESSION: 0.5 is kFieldNeutral,
		// the fixed point of fieldWithOverride, so a sound nobody has touched renders the field
		// exactly as the terrain authored it. A default of 1.0 would mean every new patch silently
		// asks for every field lifted to FULL — which is the bug flagship.h records beside that
		// constant, found once already when the smoothness semantics moved from multiply to bias
		// without the defaults following.
		// Moved by the same nine ids — see the note on kRecordedIds, including why Warp's default is 0.
		// And by the stack's three (2026-09-25): Stack 1..8 default 1, Stack Detune 0..50 cents default 10, Stack Spread
		// 0..1 default 0. Moved 2026-09-25 by ResonateModel: 0..4, default 0 (Ring, the firmware's own model).
		// All four rows together, as merged 2026-09-26.
		// And by the Stack Index's six (2026-09-26): Shape 0..9 default Linear, its Amount 0..1 default 0, Free Phase
		// 0..1 default 0, Mapping 0..8 default As shaped, its Amount 0..1 default 0, Curve -1..+1 default 0.
		// And Resonate's nine (2026-09-26): Width 0..1 default 0; Bow, Pluck, Travel x0..x4 default x1; Bow Mode
		// 0..5 default Laws; Shimmer, Stiffness, Coupling, Decay log2 -2..+2 default 0 (x1).
		// And 2026-10-03: the two edges gone, Voice Level 0..1 default 0, Level 0..1 default 1, Phase and Phase Width 0..1 default 0.
		// And Resonate Speed (2026-10-03): log2 0..3 (x1..x8), default 1 (x2, by design).
		// BOTH, merged 2026-10-03.
		// And the Basic Shapes mix (2026-10-03): four 0..1, Sine default 1 and the rest 0; Osc Type 0..4.
		// And Coarse (2026-10-03): -48..+48 semitones, default 0; and Bend Range's default index 1 -> 3 (±2 -> ±12 st).
		// And the additive Shape (2026-10-10): Additive Shape 0..3 default 2 (Saw); eight Shape Amounts 0..1 default 0; Osc Type 0..5.
		// And the additive bias knobs (2026-10-10): forty-eight 0..1, default 0.5 (kFieldNeutral, the terrain as authored).
		// And the spectral drawer (2026-10-10): nineteen per sound, ranges and defaults as params.cpp states them (ons off, Link on).
		// And the vowel Formant (2026-10-12): On 0..1 default off, Vowel 0..7 default 3 (Open), Depth 0..1 default 0.7.
		// And Partials (2026-10-12): On 0..1 default off, Count 8..320 default 64.
		// And Damp (2026-10-12): On 0..1 default off, Time log2 seconds -4.32..4.32 (50 ms..20 s) default 1 (2 s), Tilt 0..3
		// default 1.
		// And Stretch (2026-10-12): On 0..1 default off, Amount 0..1 default 0.3.
		// And Shift (2026-10-12): On 0..1 default off, Hz -1000..+1000 default +50 (ParamUnit::Hertz, new).
		// And A-PH (2026-10-18): Additive Phases 0..2 default 0 (Natural), Sub 0..24 dB default 0, Ceiling -24..0 dBFS default -6.
		// And A-PH3 (2026-10-18): Sub became a level, -24..+12 dB default -6.
		// And F2 (2026-10-19): Filter Gain and Filter 2 Gain -24..+24 dB default 0, Filter 2 as filter 1 but its cutoff default
		// log2(500), Routing 0..1 default Series; both Filter Modes 0..4 (Bell).
		// And OH (2026-10-19): Partial Count's floor 8 -> 1 (default 64 unchanged); Partials Mode 0..1 default 1 (Harmonics).
		// And OI (2026-10-19): Partials Mode 0..2 (Centers); Partials Set 0..16 default 0.
		// And OF (2026-10-26): Partials Depth 0..1 default 1 (100%: the cull as it was).
		// And OE (2026-10-26): Resolution 0..2 default 1 (Balanced), Bands 8..128 default 24, Attack 1..500 ms default 10, Release
		// 5..2000 ms default 80, Freeze and Key Track 0..1 off; per sound Vocode off, Depth 0..2 default 1, Flatten 0..1 default 1, Gate
		// -96..0 dB default -96 (off), Low and High log2 20 Hz..20 kHz at their ends.
		// And OE2 (2026-10-27): Spacing 0..6, Widths 0..5, Stereo 0..2, all default 0; per sound Bandwidth 0..1 default 1, the
		// amounts 0..1 default 0.5, the Free coordinates -64..64 default 0.5.
		// And Sibilance 0..1 default 0.3 (the Spectral effect's).
		// And Pluck (2026-10-31): Decay log2 0.05..30 s default 3 s, Decay Track 0..1 default 0.5, Release log2 0.02..10 s default
		// 0.2 s, Hardness 0..1 default 0.6, Pickup 0.02..0.5 default 0.12, Width 0..0.3 default 0.1, Seed 0..999 default 0, Velocity
		// 0..1 default 0.5; per
		// sound the five scalers 0..1 default 0.5.
		// And the drum engine (2026-11-05): Mode 0..1 default Kit, Pitched Kind 0..5 default Membrane, Walk X/Y -1..1 default 0,
		// Seed 0..999; per pad the kit's defaults (kDrumPadDefaults: Pattern Drum's lanes).
		// And the kick shape (S9): Hold 0..200 ms, Dip 0..1, Dip Time 2..80 ms default 14, Click Sweep 0..6 oct, Click Sweep Time
		// log2 0.5..20 ms default 3 ms, Start Phase 0..90 deg; all default 0 except the two times.
		// And S10: Drive Curve 0..2 default Tanh, Drive Tone 0..1 default 0.5, Clap Q 0.5..8 default 1.6, Clap High Pass log2
		// 20 Hz..2048 Hz default 20 Hz (off).
		// And S11: Bell 2 Link default Linked, Ratio 1.3..2.5 default 1.6, Hz log2 250..2500 default 500, Time log2 3..80 ms
		// default 30 ms, Q 0.7..8 default 2, Level 0..1 default 0 (off).
		// And T1: Velocity Noise -1..1 default 0, Velocity Tension 0..1 default 1 (the tension law in full).
		// And T3: Wire Contour 0..2 default Exp, Width 0..1 default 0, Buzz 0..1 default 0, Buzz Threshold 0..1 default 0.5.
		// And the kit layer: Kit Gamma -4..4, Kit Zone -512..512, Kit Offset -2..2, all default 0; seven trims 0..1 default 0.5.
		// And Mute 0..1 default 0 (every pad sounds).
		// And the zones: Velocity -8..8 default 2/3 (Tension), Wires and Bells -8..8 default 0 (Tight, Off).
		// And each pad's Bend 0..1 default 1 (the whole Bend range).
		// And the layer: Kind 0..5 default Off, Level -60..6 dB default 0, Delay 0..10 ms default 0, High Pass log2 20 Hz..2048 Hz
		// default 20 Hz, Lock 0..1 default 1, Offset X and Y -1..1 default 0.
		constexpr uint64_t kRecordedRanges =  0x6ad7d3e75b1f667ull;
		CHECK(paramRangeFingerprint() == kRecordedRanges);
	}
}

TEST_CASE("every param is distinctly identified and sanely bounded") {
	std::set<uint32_t> ids;
	std::set<std::string> names;

	for (const ParamDesc& desc : paramDescs()) {
		const auto id = static_cast<uint32_t>(desc.id);
		CAPTURE(std::string(desc.name));

		CHECK(ids.insert(id).second);       // a duplicate id makes one param unaddressable
		CHECK(names.insert(desc.name).second); // and a duplicate name makes it unfindable by a user

		CHECK(desc.min < desc.max);
		// min <= def <= max rather than a strict interior: an endpoint default is meaningful for
		// several of these (Level sits at unity, Path at all-A, the enums at their first entry).
		CHECK(desc.def >= desc.min);
		CHECK(desc.def <= desc.max);

		// A Choice param with no names would index an empty list. Nothing does today; the check is
		// here so that the first one that forgets fails a test instead of reading off the end.
		if (desc.unit == feathers::ParamUnit::Choice) {
			CHECK_FALSE(desc.names.empty());
			CHECK(desc.max == doctest::Approx(static_cast<double>(desc.names.size() - 1)));
		}

		CHECK(findParam(id) == &desc);
	}
	CHECK(ids.size() == feathers::kParamCount);
}

TEST_CASE("a value that is not a number never reaches a param") {
	// std::clamp compares, and every comparison against NaN is false, so clamping is not a filter —
	// a NaN gets through it untouched. One that reached base_ would print as "nan", multiply every
	// sample it touched into silence, and be written to the patch on the next save.
	//
	// Five doors, and the count is the point: an earlier version of this comment claimed there were
	// three and that there was no fourth, which is exactly how the modulation lane went unguarded.
	// It then said "if a fifth way to write a value ever appears, it belongs in this list" — and a
	// fifth arrived and did not join it. `ParamSet::setDeviceSource` is the reading a rack device
	// publishes, and it multiplies into every destination that source is routed to, so a NaN there
	// reaches resolved() by exactly the road a NaN on the modulation lane does. The GATE was
	// written (`std::isfinite(value) ? value : 0.0f`, params.cpp) and nothing checked it: deleting
	// it left all 34 suites green. If a SIXTH way appears, it belongs here too.
	const auto path = static_cast<clap_id>(ParamId::Path);
	const double nan = std::numeric_limits<double>::quiet_NaN();

	SUBCASE("through a host param event") {
		ParamSet params;
		params.setBase(path, 0.4);
		params.setBase(path, nan);
		CHECK(params.base(path) == doctest::Approx(0.4)); // the last good value, not the NaN
		params.setBase(path, std::numeric_limits<double>::infinity());
		CHECK(params.base(path) == doctest::Approx(0.4));
	}

	SUBCASE("through a host text field") {
		ParamSet params;
		double value = 0.0;
		CHECK_FALSE(params.textToValue(path, "nan", &value));
		CHECK_FALSE(params.textToValue(path, "inf", &value));
		// The one negative infinity that IS a setting still parses: it is what a silent fader prints.
		CHECK(params.textToValue(static_cast<clap_id>(ParamId::MasterGain), "-inf dB", &value));
		CHECK(value == doctest::Approx(findParam(static_cast<clap_id>(ParamId::MasterGain))->min));
	}

	SUBCASE("through a state file written by some other build") {
		MemoryStream stream;
		stream.bytes = makeState({{static_cast<uint32_t>(path), nan}});

		ParamSet params;
		clap_istream in = stream.in();
		REQUIRE(params.load(&in)); // the rest of the patch is still good, so the load stands
		CHECK(params.base(path) == doctest::Approx(findParam(path)->def));
		CHECK_FALSE(std::isnan(params.base(path)));
	}

	SUBCASE("through the modulation lane") {
		// The quietest of the four: a mod offset never reaches a file, so nothing is corrupted and
		// nothing is visible in the patch. But resolved() is the per-block reading the voice layer
		// runs on, and a NaN there travels into attackSeconds, masterGain and the rest — a voice goes
		// silent and the param it came from still reads correctly in the host.
		ParamSet params;
		params.setBase(path, 0.4);
		params.setModOffset(path, nan);

		CHECK_FALSE(std::isnan(params.resolved(path)));
		CHECK(params.resolved(path) == doctest::Approx(0.4)); // the base, as though nothing modulated
		params.setModOffset(path, std::numeric_limits<double>::infinity());
		CHECK(params.resolved(path) == doctest::Approx(0.4));

		// A good offset after a bad one still works — the bad one is dropped, not latched.
		params.setModOffset(path, 0.25);
		CHECK(params.resolved(path) == doctest::Approx(0.65));

		// Every derived reading stays finite, since those are what the voice layer actually calls.
		ParamSet derived;
		for (const ParamDesc& desc : paramDescs()) {
			derived.setModOffset(static_cast<clap_id>(desc.id), nan);
		}
		CHECK(std::isfinite(derived.glideTimeSeconds()));
		CHECK(std::isfinite(derived.masterGain()));
	}

	SUBCASE("through a device's published output") {
		// THE FIFTH DOOR, and the quietest of the five. A rack device's reading is not a parameter, is
		// never written to a file and never reaches the host — but it is multiplied by every route
		// leading out of that source, so one NaN becomes an offset on every destination the device
		// reaches at once. The gate's own comment draws the distinction that matters: "the envelope's
		// own sanitize() covers the settings it was given; this covers the number that came OUT, which
		// is a different claim".
		ParamSet params;
		const auto fine = static_cast<clap_id>(ParamId::FineTune);
		params.setBase(fine, -50.0);
		// **A DEVICE SOURCE IS A CELL THE NODE OWNS, AND THE RESOLVE READS IT THROUGH A POINTER (kill-lanes
		// §2.4b).** This case wrote a value into `ParamSet`'s source table by coordinate
		// (`setDeviceSource`) and read the destination; the table is gone, so the case owns the cell and
		// hands it in as the device's output (`oneCell`). What it proves is unchanged: the number in the
		// cell is what moves the destination, live, with no copy in between.
		constexpr feathers::DeviceInstanceId kDevice = 7;
		float cell = 0.0f;
		feathers::test::Routes routes;
		const int at = feathers::modDestinationIndexOf(ParamId::FineTune);
		REQUIRE(at >= 0);
		routes.set(feathers::backplane::Source::output(kDevice, 0),
		           feathers::backplane::Destination::core(static_cast<size_t>(at)), 0.5f);
		routes.publish(params, feathers::test::oneCell(&cell));

		// THE ROAD IS LIVE, SAID FIRST. A source that reached nowhere would satisfy every check below
		// while proving nothing — the reading has to actually move the destination.
		cell = 1.0f;
		REQUIRE(params.resolved(fine) == doctest::Approx(50.0));

		// **THE FINITE GATE MOVED TO THE WRITER.** `setDeviceSource` refused a NaN or an infinity here; the
		// cell is the node's now and the gate is the publish's (`RackEngine::advanceModulators`,
		// `writeOutputForVoice`), where every reading enters. A pointer read cannot filter and must not
		// pretend to, so this case no longer asserts it; the rack suites own that claim.
		cell = 0.5f;
		CHECK(params.resolved(fine) == doctest::Approx(0.0));
	}

	SUBCASE("a param the host was never offered modulation on does not get modulated") {
		// Not a NaN case, but the same shape of question: a value arriving on a lane the plugin never
		// advertised. Every stepped param is in that set, and a mod offset on one would have resolved()
		// rounding a voice count mid-block.
		ParamSet params;
		for (const ParamDesc& desc : paramDescs()) {
			if ((desc.flags & CLAP_PARAM_IS_MODULATABLE) != 0) {
				continue;
			}
			CAPTURE(std::string(desc.name));
			const auto id = static_cast<clap_id>(desc.id);
			const double before = params.resolved(id);
			params.setModOffset(id, 1.0);
			CHECK(params.resolved(id) == doctest::Approx(before));
		}
	}

	SUBCASE("a negative offset below a param's minimum is still legitimate") {
		// The guard is finiteness only, deliberately not the clamping the base gets: an offset is a
		// distance, and resolved() is where the sum gets clamped.
		ParamSet params;
		params.setBase(path, 0.5);
		params.setModOffset(path, -0.9); // Path's minimum is 0, but the offset is not a setting
		CHECK(params.resolved(path) == doctest::Approx(0.0));
		params.setModOffset(path, -0.2);
		CHECK(params.resolved(path) == doctest::Approx(0.3));
	}
}

TEST_CASE("stepped params only ever hold whole values") {
	ParamSet params;
	const auto voices = static_cast<clap_id>(ParamId::VoiceCount);

	// CLAP declares these as stepped, so a fractional value is not a setting the host was offered.
	double value = 0.0;
	REQUIRE(params.textToValue(voices, "3.7", &value));
	CHECK(value == doctest::Approx(4.0));

	params.setBase(voices, 3.7);
	CHECK(params.base(voices) == doctest::Approx(4.0));

	for (const ParamDesc& desc : paramDescs()) {
		if ((desc.flags & CLAP_PARAM_IS_STEPPED) == 0) {
			continue;
		}
		CAPTURE(std::string(desc.name));
		const auto id = static_cast<clap_id>(desc.id);
		const double midFraction = desc.min + 0.5 * (desc.max - desc.min) + 0.3;
		params.setBase(id, midFraction);
		CHECK(params.base(id) == doctest::Approx(std::round(params.base(id))));
	}
}

TEST_CASE("continuous params are automatable and modulatable, stepped ones stepped") {
	// EXACTLY ONE PARAMETER IS NOT AUTOMATABLE, AND IT IS NAMED HERE RATHER THAN EXEMPTED BY A RULE.
	//
	// The engine is deliberately not modulatable. The engine selects which
	// renderer builds the character tables, so a host sweeping it rebuilds the bank under a sounding
	// note -- measured at 1.951x the waveform's own steepest step, against 1.000x for a comparable
	// parameter move.
	//
	// A NAMED EXCEPTION RATHER THAN A PREDICATE, deliberately. Writing this as "stepped choice
	// parameters are not automatable" would have quietly taken the lane off the eight sound enables,
	// Voice Mode, the transposes and the bend range as well, and nobody asked for that. It would also
	// mean the next parameter to lose automation does so SILENTLY. This way a second one fails here
	//
	// THE SECOND, ARGUED 2026-09-25: the Resonate Model. A switch reseats the ONE string at the new model's
	// rest pose, discarding its motion, and costs 6-30 us at the tick, so a host sweeping it would throw
	// the string's motion away on every step. Stepped, so not modulatable either (the rule below).
	// and has to be argued for.
	for (const ParamDesc& desc : paramDescs()) {
		CAPTURE(std::string(desc.name));
		const bool engine = desc.id == ParamId::OscType || desc.id == ParamId::ResonateModel;
		CHECK(((desc.flags & CLAP_PARAM_IS_AUTOMATABLE) != 0) == !engine);

		const bool stepped = (desc.flags & CLAP_PARAM_IS_STEPPED) != 0;
		const bool modulatable = (desc.flags & CLAP_PARAM_IS_MODULATABLE) != 0;
		// Stepped params take whole values, and a modulator sweeping one would emit a stream of
		// discontinuities rather than a gesture. The two flags are deliberately exclusive here.
		CHECK(stepped != modulatable);
	}

	// Level is the one amplitude a host can address per voice, which is what makes MPE-style
	// expression reach the synth at all.
	const ParamDesc* level = findParam(static_cast<clap_id>(ParamId::VoiceLevel));
	REQUIRE(level != nullptr);
	CHECK((level->flags & CLAP_PARAM_IS_MODULATABLE_PER_NOTE_ID) != 0);
}

TEST_CASE("modulation offsets resolve on top of the base without moving it") {
	ParamSet params;
	const auto path = static_cast<clap_id>(ParamId::Path);

	params.setBase(path, 0.4);
	params.setModOffset(path, 0.25);

	CHECK(params.base(path) == doctest::Approx(0.4));   // automation's value, untouched
	CHECK(params.resolved(path) == doctest::Approx(0.65));

	// Resolution clamps to the declared range: a modulator pushing past the end is not an excuse to
	// hand the DSP a value it was never told to expect.
	params.setModOffset(path, 5.0);
	CHECK(params.resolved(path) == doctest::Approx(1.0));

	// Mod events carry an absolute offset from the base, not an increment, so each one replaces the
	// last rather than accumulating.
	params.setModOffset(path, 0.1);
	CHECK(params.resolved(path) == doctest::Approx(0.5));

	params.clearModulation();
	CHECK(params.resolved(path) == doctest::Approx(0.4));
}

TEST_CASE("state round-trips through a stream that only moves a few bytes at a time") {
	ParamSet saved;
	std::vector<std::pair<clap_id, double>> expected;
	for (const ParamDesc& desc : paramDescs()) {
		const auto id = static_cast<clap_id>(desc.id);
		const double value = distinctFrom(desc);
		saved.setBase(id, value);
		expected.emplace_back(id, value);
	}
	// Modulation is host state, not patch state, and must not ride along into the file.
	saved.setModOffset(static_cast<clap_id>(ParamId::Path), 0.3);

	MemoryStream stream;
	stream.chunk = 3; // an awkward size that lands mid-double, to force the write loop to loop
	clap_ostream out = stream.out();
	REQUIRE(saved.save(&out));

	ParamSet loaded;
	loaded.setBase(static_cast<clap_id>(ParamId::AZone), 999.0); // perturb before loading
	loaded.setModOffset(static_cast<clap_id>(ParamId::Path), -0.1);

	stream.chunk = 5;
	clap_istream in = stream.in();
	REQUIRE(loaded.load(&in));

	for (const auto& [id, value] : expected) {
		CAPTURE(id);
		CHECK(loaded.base(id) == value); // exact: the format stores the bit pattern, not a rounding of it
	}

	// The live modulation survives the load, and resolved() is the new base with the old offset still
	// on top. That is the contract on purpose: base_ is the plugin's state and mod_ is the host's, so
	// a preset load has no business clearing a modulator the host is still driving. Wiping it would
	// have dropped the value to unmodulated for exactly one block, until the host re-sent what was
	// already true — and would have meant the main thread writing a lane the audio thread owns.
	CHECK(loaded.resolved(ParamId::Path)
	      == doctest::Approx(loaded.base(ParamId::Path) - 0.1));
}

TEST_CASE("a global-width-era patch keeps its master scale and comes back mono") {
	// V13b, DOCUMENTED RATHER THAN HIDDEN: "a patch saved in the brief global-width-era window with
	// Width > 0 and no per-sound values comes back mono — pre-release, single-user patches, accepted."
	// The stream below is exactly such a patch: it carries 0x0043 and none of the per-sound width
	// ids, because they did not exist when it was saved. Two claims, and the first is the lead's
	// addition on review: the LOADED VALUE SURVIVES — the loader must not silently rewrite the
	// master scale, only the zero per-sound widths gate it — and the per-sound widths land at their
	// default of 0, which is what makes the render mono (the render half is pinned in
	// tests/test_terrain_stereo.cpp, the "no wide sound is mono" case's era subclaim).
	MemoryStream stream;
	stream.bytes = makeState({ { static_cast<uint32_t>(ParamId::Width), 0.6 } });

	ParamSet params;
	clap_istream in = stream.in();
	REQUIRE(params.load(&in));
	CHECK(params.base(static_cast<clap_id>(ParamId::Width)) == 0.6);
	for (size_t slot = 0; slot < kMaxSounds; ++slot) {
		CAPTURE(slot);
		CHECK(params.base(static_cast<clap_id>(soundParam(slot, SoundField::Width))) == 0.0);
	}
}

TEST_CASE("a patch from another version still loads") {
	const auto zone = static_cast<clap_id>(ParamId::AZone);
	const auto fine = static_cast<clap_id>(ParamId::FineTune);

	SUBCASE("ids this build does not know are skipped") {
		MemoryStream stream;
		stream.bytes = makeState({{static_cast<uint32_t>(zone), 300.0},
		                          {0xbeef, 1.0}, // a param from some later version
		                          {static_cast<uint32_t>(fine), -25.0}});

		ParamSet params;
		clap_istream in = stream.in();
		REQUIRE(params.load(&in));
		CHECK(params.base(zone) == doctest::Approx(300.0));
		CHECK(params.base(fine) == doctest::Approx(-25.0)); // and the entry after it still lands
	}

	SUBCASE("ids the patch omits keep their default") {
		MemoryStream stream;
		stream.bytes = makeState({{static_cast<uint32_t>(zone), 300.0}});

		ParamSet params;
		params.setBase(fine, 50.0);
		clap_istream in = stream.in();
		REQUIRE(params.load(&in));
		CHECK(params.base(fine) == doctest::Approx(findParam(fine)->def));
	}

	SUBCASE("out-of-range values are clamped rather than trusted") {
		MemoryStream stream;
		stream.bytes = makeState({{static_cast<uint32_t>(zone), 1e9}});

		ParamSet params;
		clap_istream in = stream.in();
		REQUIRE(params.load(&in));
		CHECK(params.base(zone) == doctest::Approx(findParam(zone)->max));
	}

	// A refused load must cost the user nothing. This is the case where a corrupt project file turns
	// into a lost patch: if load() cleared the params before discovering the stream was bad, the
	// session would come back as factory defaults with the real patch nowhere. So the params are set
	// to known values first, and what is checked is that they are still there afterwards.
	SUBCASE("a version byte this build cannot read is refused without touching the patch") {
		MemoryStream stream;
		// ONE PAST THE CURRENT VERSION, DERIVED RATHER THAN SPELLED. The point of the case is "a byte
		// this build cannot read", never the literal number — so it is computed from the byte instead
		// of tracking it by hand, which is what this file used to do and what broke it.
		stream.bytes = makeState({{static_cast<uint32_t>(zone), 300.0}},
		                         static_cast<uint8_t>(feathers::kStateVersion + 1));

		ParamSet params;
		params.setBase(zone, 700.0);
		params.setBase(fine, 40.0);

		clap_istream in = stream.in();
		CHECK_FALSE(params.load(&in));
		CHECK(params.base(zone) == doctest::Approx(700.0));
		CHECK(params.base(fine) == doctest::Approx(40.0));
	}

	SUBCASE("a truncated stream is refused without touching the patch") {
		MemoryStream stream;
		stream.bytes = makeState({{static_cast<uint32_t>(zone), 300.0},
		                          {static_cast<uint32_t>(fine), -25.0}});
		stream.bytes.resize(stream.bytes.size() - 3); // the second pair now runs off the end

		ParamSet params;
		params.setBase(zone, 700.0);
		params.setBase(fine, 40.0);

		clap_istream in = stream.in();
		CHECK_FALSE(params.load(&in));
		// Including the param the truncated stream had already read successfully: a partial load is
		// not a load.
		CHECK(params.base(zone) == doctest::Approx(700.0));
		CHECK(params.base(fine) == doctest::Approx(40.0));
	}
}

TEST_CASE("text conversion round-trips every param") {
	ParamSet params;
	for (const ParamDesc& desc : paramDescs()) {
		const auto id = static_cast<clap_id>(desc.id);
		CAPTURE(std::string(desc.name));

		// Both ends of the table's travel and the default. The default matters on its own: it is
		// where the special displays live — "Off" for a glide of zero, "-inf dB" at the bottom of a
		// fader — and a host that types back what the plugin showed it must land where it started.
		for (double value : {desc.min, desc.def, distinctFrom(desc), desc.max}) {
			CAPTURE(value);
			char text[CLAP_NAME_SIZE] = {};
			REQUIRE(params.valueToText(id, value, text, sizeof(text)));
			CAPTURE(text);

			double back = 0.0;
			REQUIRE(params.textToValue(id, text, &back));
			// Loose: the display is rounded for a human to read, so this asks that parsing what we
			// printed lands on the same setting, not that it recovers the bits.
			CHECK(back == doctest::Approx(value).epsilon(0.02));
		}
	}
}

TEST_CASE("the enum params print and parse their entry names") {
	ParamSet params;
	char text[CLAP_NAME_SIZE] = {};

	REQUIRE(params.valueToText(static_cast<clap_id>(ParamId::OscType), 1.0, text, sizeof(text)));
	CHECK(std::string(text) == "Stair");
	REQUIRE(params.valueToText(static_cast<clap_id>(ParamId::OscType), 3.0, text, sizeof(text)));
	CHECK(std::string(text) == "Resonate");

	double value = 0.0;
	REQUIRE(params.textToValue(static_cast<clap_id>(ParamId::OscType), "morph", &value));
	CHECK(value == doctest::Approx(0.0));

	REQUIRE(params.valueToText(static_cast<clap_id>(ParamId::VoiceMode), 2.0, text, sizeof(text)));
	CHECK(std::string(text) == "Legato");

	REQUIRE(params.valueToText(static_cast<clap_id>(ParamId::NotePriority), 1.0, text, sizeof(text)));
	CHECK(std::string(text) == "Low");

	// Bend range is an index into a list of non-contiguous choices, so its text is the semitone count
	// and parsing "12" has to find the choice rather than read an index.
	REQUIRE(params.valueToText(static_cast<clap_id>(ParamId::BendRange), 3.0, text, sizeof(text)));
	CHECK(std::string(text) == "12 st");
	REQUIRE(params.textToValue(static_cast<clap_id>(ParamId::BendRange), "12 st", &value));
	CHECK(value == doctest::Approx(3.0));

	// Coarse is continuous semitones, so its text keeps the hundredths Transpose Semi's whole steps do not have.
	REQUIRE(params.valueToText(static_cast<clap_id>(ParamId::Coarse), 5.25, text, sizeof(text)));
	CHECK(std::string(text) == "+5.25 st");
	REQUIRE(params.valueToText(static_cast<clap_id>(ParamId::TransposeSemi), 5.0, text, sizeof(text)));
	CHECK(std::string(text) == "+5 st");
	REQUIRE(params.textToValue(static_cast<clap_id>(ParamId::Coarse), "-7.5 st", &value));
	CHECK(value == doctest::Approx(-7.5));
}

TEST_CASE("the tapered params hit the values the design asked for") {
	ParamSet params;


	// Glide defaults to off, and off means exactly zero — the reason its taper is cubic rather than
	// the logarithm the amp edges use.
	CHECK(params.glideTimeSeconds() == 0.0);
	params.setBase(static_cast<clap_id>(ParamId::GlideTime), 1.0);
	CHECK(params.glideTimeSeconds() == doctest::Approx(feathers::kGlideMaxSeconds));

	// Master gain is unity at its 0 dB default and silent at the bottom of its travel.
	CHECK(params.masterGain() == doctest::Approx(1.0));
	params.setBase(static_cast<clap_id>(ParamId::MasterGain),
	               findParam(static_cast<clap_id>(ParamId::MasterGain))->min);
	CHECK(params.masterGain() == 0.0);

	// THE DEFAULT IS AN OCTAVE EITHER WAY, read through the accessor the synth bends by.
	CHECK(params.bendRangeSemitones() == 12);
	params.setBase(static_cast<clap_id>(ParamId::BendRange), 5.0);
	CHECK(params.bendRangeSemitones() == 48);
}

TEST_CASE("the stepped params read back as the enums they mean") {
	// The voice layer must never spell out `static_cast<VoiceMode>(std::lround(...))` for itself.
	// These accessors are the only mapping from index to meaning, so this is where that mapping is
	// pinned — including each one's default, which is what an untouched plugin plays. THE ENGINE
	// DEFAULT IS THE FLAGSHIP (decided 2026-08-18): a fresh instance is a V5
	// instance, and this line is where that decision is pinned rather than hoped.
	ParamSet params;
	CHECK(params.oscType() == feathers::OscType::V5);
	CHECK(params.voiceMode() == feathers::VoiceMode::Poly);
	CHECK(params.notePriority() == feathers::NotePriority::Last);
	CHECK(params.voiceCount() == 8);

	params.setBase(static_cast<clap_id>(ParamId::OscType), 0.0);
	CHECK(params.oscType() == feathers::OscType::Morph);
	params.setBase(static_cast<clap_id>(ParamId::OscType), 1.0);
	params.setBase(static_cast<clap_id>(ParamId::VoiceMode), 2.0);
	params.setBase(static_cast<clap_id>(ParamId::NotePriority), 1.0);
	params.setBase(static_cast<clap_id>(ParamId::VoiceCount), 16.0);
	CHECK(params.oscType() == feathers::OscType::Stair);
	CHECK(params.voiceMode() == feathers::VoiceMode::Legato);
	params.setBase(static_cast<clap_id>(ParamId::OscType), 3.0);
	CHECK(params.oscType() == feathers::OscType::Resonate);
	params.setBase(static_cast<clap_id>(ParamId::OscType), 5.0);
	CHECK(params.oscType() == feathers::OscType::Additive);
	params.setBase(static_cast<clap_id>(ParamId::OscType), 1.0);
	CHECK(params.notePriority() == feathers::NotePriority::Low);
	CHECK(params.voiceCount() == feathers::kMaxVoices);

	// Out of range from every direction. setBase clamps, so getting here at all takes a value the
	// door would have rejected — which is the point: the accessor is the second door, and the voice
	// layer switches exhaustively on what comes out of it.
	params.setBase(static_cast<clap_id>(ParamId::VoiceMode), 99.0);
	CHECK(params.voiceMode() == feathers::VoiceMode::Legato);
	params.setBase(static_cast<clap_id>(ParamId::VoiceMode), -99.0);
	CHECK(params.voiceMode() == feathers::VoiceMode::Poly);
}

TEST_CASE("every choice entry has a name, and every name selects its entry") {
	// The static_asserts beside each enum tie the counts together at compile time; this ties the
	// CONTENTS together, which they cannot. A stepped param's declared range has to cover exactly the
	// entries that exist — one short and the last engine is unreachable, one long and the host offers
	// a setting that prints the last name twice.
	for (const ParamDesc& desc : paramDescs()) {
		if (desc.unit != feathers::ParamUnit::Choice) {
			continue;
		}
		INFO("param: ", std::string(desc.name));
		CHECK(desc.min == 0.0);
		CHECK(desc.max == static_cast<double>(desc.names.size()) - 1.0);

		ParamSet params;
		for (size_t i = 0; i < desc.names.size(); ++i) {
			char text[64] = {};
			REQUIRE(params.valueToText(static_cast<clap_id>(desc.id), static_cast<double>(i), text,
			                           sizeof(text)));
			CHECK(std::string(text) == std::string(desc.names[i]));

			double back = -1.0;
			REQUIRE(params.textToValue(static_cast<clap_id>(desc.id), desc.names[i], &back));
			CHECK(back == static_cast<double>(i));
		}
	}
}

TEST_CASE("every sound's ids are where they will always be") {
	// FIFTY-SIX PERMANENT PROMISES, written out rather than derived, because deriving them here from
	// the same rule the code uses would prove only that the rule is self-consistent. A wrong id is not
	// a compile error — it is somebody's patch loading onto the wrong knob — so the expectation has to
	// come from somewhere other than the thing it is checking.
	//
	// A and B keep exactly the ids v1 gave them. That is the whole reason the block for C..H starts at
	// 0x0100 instead of continuing a stride: 0x0030 was already Blend, and ids never move.
	CHECK(soundParam(0, SoundField::Zone) == ParamId::AZone);
	CHECK(soundParam(0, SoundField::Offset) == ParamId::AOffset);
	CHECK(soundParam(0, SoundField::Gamma) == ParamId::AGamma);
	CHECK(soundParam(1, SoundField::Zone) == ParamId::BZone);
	CHECK(soundParam(1, SoundField::Offset) == ParamId::BOffset);
	CHECK(soundParam(1, SoundField::Gamma) == ParamId::BGamma);

	constexpr ParamId kIdentity[kMaxSounds][3] = {
	    { ParamId::AZone, ParamId::AOffset, ParamId::AGamma },
	    { ParamId::BZone, ParamId::BOffset, ParamId::BGamma },
	    { ParamId::CZone, ParamId::COffset, ParamId::CGamma },
	    { ParamId::DZone, ParamId::DOffset, ParamId::DGamma },
	    { ParamId::EZone, ParamId::EOffset, ParamId::EGamma },
	    { ParamId::FZone, ParamId::FOffset, ParamId::FGamma },
	    { ParamId::GZone, ParamId::GOffset, ParamId::GGamma },
	    { ParamId::HZone, ParamId::HOffset, ParamId::HGamma },
	};
	constexpr ParamId kPlacement[kMaxSounds][4] = {
	    { ParamId::APlaceX, ParamId::APlaceY, ParamId::AEnable, ParamId::AWidth },
	    { ParamId::BPlaceX, ParamId::BPlaceY, ParamId::BEnable, ParamId::BWidth },
	    { ParamId::CPlaceX, ParamId::CPlaceY, ParamId::CEnable, ParamId::CWidth },
	    { ParamId::DPlaceX, ParamId::DPlaceY, ParamId::DEnable, ParamId::DWidth },
	    { ParamId::EPlaceX, ParamId::EPlaceY, ParamId::EEnable, ParamId::EWidth },
	    { ParamId::FPlaceX, ParamId::FPlaceY, ParamId::FEnable, ParamId::FWidth },
	    { ParamId::GPlaceX, ParamId::GPlaceY, ParamId::GEnable, ParamId::GWidth },
	    { ParamId::HPlaceX, ParamId::HPlaceY, ParamId::HEnable, ParamId::HWidth },
	};

	for (size_t slot = 0; slot < kMaxSounds; ++slot) {
		CAPTURE(slot);
		CHECK(soundParam(slot, SoundField::Zone) == kIdentity[slot][0]);
		CHECK(soundParam(slot, SoundField::Offset) == kIdentity[slot][1]);
		CHECK(soundParam(slot, SoundField::Gamma) == kIdentity[slot][2]);
		CHECK(soundParam(slot, SoundField::PlaceX) == kPlacement[slot][0]);
		CHECK(soundParam(slot, SoundField::PlaceY) == kPlacement[slot][1]);
		CHECK(soundParam(slot, SoundField::Enable) == kPlacement[slot][2]);
		// The +3 slot each placement stride kept free is exactly where V13 put the per-sound width —
		// 0x0203, 0x0213 … 0x0273 — so the one-rule mapping grows a field without growing a case.
		CHECK(soundParam(slot, SoundField::Width) == kPlacement[slot][3]);
		// THE ADDITIVE SHAPE AMOUNT (2026-10-10) at +C, written out: 0x020C, 0x021C … 0x027C.
		CHECK(static_cast<uint32_t>(feathers::shapeAmountParam(slot)) == 0x020Cu + 0x10u * static_cast<uint32_t>(slot));
		CHECK(paramIndexOf(static_cast<clap_id>(feathers::shapeAmountParam(slot))) >= 0);
		// THE SIX ADDITIVE BIAS KNOBS (2026-10-10) in the second per-sound block, +0..+5: 0x0280 .. 0x02F5. Each in the
		// table at the neutral default, continuous, 0..1, and a modulation destination.
		const ParamId bias[] = { feathers::additiveRolloffParam(slot), feathers::additiveFormantParam(slot),
			                     feathers::additiveOddEvenParam(slot), feathers::additiveCombParam(slot),
			                     feathers::additiveDetailParam(slot), feathers::additiveOffGridParam(slot) };
		for (uint32_t k = 0; k < std::size(bias); ++k) {
			CAPTURE(k);
			CHECK(static_cast<uint32_t>(bias[k]) == 0x0280u + 0x10u * static_cast<uint32_t>(slot) + k);
			const int at = paramIndexOf(static_cast<clap_id>(bias[k]));
			REQUIRE(at >= 0);
			const ParamDesc& d = paramDescs()[static_cast<size_t>(at)];
			CHECK(d.def == 0.5);
			CHECK(d.min == 0.0);
			CHECK(d.max == 1.0);
			CHECK(feathers::modDestinationIndexOf(bias[k]) >= 0);
		}
		CHECK(bias[0] == (std::array { ParamId::ARolloff, ParamId::BRolloff, ParamId::CRolloff, ParamId::DRolloff,
		                               ParamId::ERolloff, ParamId::FRolloff, ParamId::GRolloff, ParamId::HRolloff })[slot]);

		// And every one of them is really in the table, with a name and a range. An id the mapping can
		// produce but the table has never heard of would be a param the host cannot see and the state
		// stream silently drops.
		for (SoundField field : { SoundField::Zone, SoundField::Offset, SoundField::Gamma,
		                          SoundField::PlaceX, SoundField::PlaceY, SoundField::Enable,
		                          SoundField::Width }) {
			CHECK(paramIndexOf(static_cast<clap_id>(soundParam(slot, field))) >= 0);
		}
	}
}

TEST_CASE("the constellation a patch gets when it says nothing is the shape v1 had") {
	// Two sounds on, six waiting, and the two that are on sitting either side of the origin — which is
	// the A→B path v1 had, restated spatially rather than replaced. A default that switched six more
	// sounds on would change every existing patch into something else. THE SHAPE, not the sound: the
	// default ENGINE is the flagship now (since 2026-08-18), so a fresh instance plays V5 on this
	// constellation — the geometry claims below are engine-independent and that is all this case pins.
	ParamSet params;
	CHECK(params.base(static_cast<clap_id>(ParamId::AEnable)) == 1.0);
	CHECK(params.base(static_cast<clap_id>(ParamId::BEnable)) == 1.0);
	for (size_t slot = 2; slot < kMaxSounds; ++slot) {
		CAPTURE(slot);
		CHECK(params.base(static_cast<clap_id>(soundParam(slot, SoundField::Enable))) == 0.0);
	}

	CHECK(params.base(static_cast<clap_id>(ParamId::APlaceX)) < 0.0);
	CHECK(params.base(static_cast<clap_id>(ParamId::BPlaceX)) > 0.0);
	CHECK(params.base(static_cast<clap_id>(ParamId::APlaceY)) == 0.0);
	CHECK(params.base(static_cast<clap_id>(ParamId::BPlaceY)) == 0.0);

	// No two sounds share a placement. Coincident anchors are a division by zero in any
	// inverse-distance weighting, and a default that contained one would be a crash waiting for the
	// day somebody switches both on.
	for (size_t a = 0; a < kMaxSounds; ++a) {
		for (size_t b = a + 1; b < kMaxSounds; ++b) {
			CAPTURE(a);
			CAPTURE(b);
			const double dx = params.base(static_cast<clap_id>(soundParam(a, SoundField::PlaceX)))
			                  - params.base(static_cast<clap_id>(soundParam(b, SoundField::PlaceX)));
			const double dy = params.base(static_cast<clap_id>(soundParam(a, SoundField::PlaceY)))
			                  - params.base(static_cast<clap_id>(soundParam(b, SoundField::PlaceY)));
			CHECK(dx * dx + dy * dy > 0.01);
		}
	}
}

//---------------------//
// The modulation bus  //
//---------------------//

namespace {

/// A routing table with one amount in it, addressed the way a hand would: source index, destination
/// parameter, amount in the destination's own range.
/// A route from a macro to a parameter, BY IDENTITY (kill-lanes §2.4b) — it filled a dense matrix by flat
/// coordinate; a host source is its index, a host destination its place in the destination list.
feathers::test::Routes routed(size_t source, ParamId destination, float amount) {
	feathers::test::Routes routes;
	const int at = feathers::modDestinationIndexOf(destination);
	REQUIRE(at >= 0);
	routes.set(feathers::test::macro(source), feathers::backplane::Destination::core(static_cast<size_t>(at)), amount);
	return routes;
}

} // namespace

TEST_CASE("the destination list is the host's own then the lane's tenants, and it is ORDERED") {
	// THE MOCK'S OWN LINE, pinned here rather than trusted to memory:
	//     const MOD_TARGETS=['path','posx','posy','level','glide','fine'];
	// Pinned literally for the same reason the forty-eight sound ids are: this list is what a hand can
	// reach, and widening it by accident would let a modulator into the identity half of the patch,
	// where A32 says nothing may move under a host.
	//
	// TWO DELIBERATE ACTS HAVE WIDENED IT SINCE, and this case is where both are recorded.
	//
	//   - THE TENANTS. An fx device declares which of its own params are destinations and the list is
	//     BUILT from those declarations, so the filter's five continuous controls are reachable.
	//     (Modulation could not be assigned to the filter's params before this.)
	//   - WEATHER AND WIDTH, brought in on 2026-08-19 — and the decision is the general principle
	//     rather than the two entries: EVERY CONTINUOUS PERFORMANCE PARAMETER IS MODULATABLE. Neither
	//     was a parameter when the mock was drawn, which is the only reason the mock omits them.
	//
	// THE MOCK'S SIX STILL COME FIRST, AND THAT PART IS PERMANENT. A destination's INDEX is what a
	// saved patch stores — the routing block is a table of amounts, not a list of names — so every
	// patch ever written means these six by the positions they hold here. Everything appends; nothing
	// inserts, which is why Weather and Width sit at 6 and 7 rather than beside the puck they belong
	// with.
	const std::vector<ParamId> expected {ParamId::Path,      ParamId::PosX,     ParamId::PosY,
	                                     ParamId::VoiceLevel,     ParamId::GlideTime, ParamId::FineTune,
	                                     ParamId::Weather,   ParamId::Width};
	REQUIRE(feathers::kModDestinations.size() >= expected.size());
	for (size_t i = 0; i < expected.size(); ++i) {
		CAPTURE(i);
		CHECK(feathers::kModDestinations[i] == expected[i]);
		CHECK(feathers::modDestinationIndexOf(expected[i]) == static_cast<int>(i));
	}

	SUBCASE("Weather and Width are reachable, and the ids are the ones the design names") {
		// CONFIRMED AGAINST THE TABLE rather than taken on trust: the decision arrived with ids attached
		// (0x0042 and 0x0043) and an id quoted from memory is exactly the kind of thing that lands a
		// route on the wrong knob.
		CHECK(static_cast<uint32_t>(ParamId::Weather) == 0x0042u);
		CHECK(static_cast<uint32_t>(ParamId::Width) == 0x0043u);
		CHECK(feathers::modDestinationIndexOf(ParamId::Weather) >= 0);
		CHECK(feathers::modDestinationIndexOf(ParamId::Width) >= 0);

		// AND THEIR NEIGHBOURS IN THE SAME ID BLOCK ARE NOT SWEPT IN WITH THEM. Relief is dormant — the
		// complexity field it reads does not exist yet, so a route to it would move nothing and the
		// honesty rule refuses it — and Global Gamma is IDENTITY, which is the half of the patch A32
		// says nothing may move under a host. Two entries were added, not a block.
		CHECK(feathers::modDestinationIndexOf(ParamId::Relief) < 0);
		CHECK(feathers::modDestinationIndexOf(ParamId::GlobalGamma) < 0);
	}

	// TWO SUBCASES ABOUT THE FILTER'S DESTINATIONS STOOD HERE. Both asked this table which of the
	// filter's controls a modulator may be pointed at; neither question is this table's any more,
	// because a swallowed device's controls are DEVICE destinations addressed by slot and ordinal.
	//
	// The claims survive at that address rather than at this one. Which controls are eligible is
	// asserted in tests/test_synth.cpp — the cutoff reachable, Keytrack, Poly and Enabled not — and
	// the DERIVATION they were really about, that the answer comes from the device's own descriptor
	// and never from a second list kept in src/plugin, is now structural: deviceDestinationOfControl
	// walks modulatableCount() and there is no plugin-side list left to disagree with it.


	SUBCASE("every destination is one the engine will accept an offset on") {
		// The honesty rule, checked against the table rather than against a list somebody typed twice:
		// a stepped param refuses modulation in setModOffset, so a stepped destination would draw a
		// ring that moves nothing.
		for (const ParamId id : feathers::kModDestinations) {
			const ParamDesc* desc = findParam(static_cast<clap_id>(id));
			REQUIRE(desc != nullptr);
			CAPTURE(std::string(desc->name));
			CHECK((desc->flags & CLAP_PARAM_IS_MODULATABLE) != 0);
			CHECK((desc->flags & CLAP_PARAM_IS_STEPPED) == 0);
		}
	}

	SUBCASE("the macros' own ids are a block apiece, and pinned") {
		// FOUR PERMANENT PROMISES, in the shape the forty-eight sound ids are pinned in. Each macro is a
		// CARD in the strip, so each is a 0x10 block rather than a field inside one — the shape and the
		// surface agreeing about what a source is. A φ LFO card lands at 0x00C0 and nothing here moves.
		CHECK(feathers::macroParam(0) == ParamId::Macro1);
		CHECK(feathers::macroParam(1) == ParamId::Macro2);
		CHECK(feathers::macroParam(2) == ParamId::Macro3);
		CHECK(feathers::macroParam(3) == ParamId::Macro4);
		CHECK(static_cast<uint32_t>(ParamId::Macro1) == 0x0080u);
		CHECK(static_cast<uint32_t>(ParamId::Macro2) == 0x0090u);
		CHECK(static_cast<uint32_t>(ParamId::Macro3) == 0x00A0u);
		CHECK(static_cast<uint32_t>(ParamId::Macro4) == 0x00B0u);

		// UNIPOLAR, RESTING AT THE BOTTOM: the direction a macro pushes lives in the route's signed
		// amount, and a source that could also be negative would be a second way to say it. Resting at
		// the minimum is what makes "routed but untouched" and "silent" the same state.
		for (size_t i = 0; i < feathers::kMacroCount; ++i) {
			const ParamDesc* desc = findParam(static_cast<clap_id>(feathers::macroParam(i)));
			REQUIRE(desc != nullptr);
			CAPTURE(std::string(desc->name));
			CHECK(desc->min == 0.0);
			CHECK(desc->max == 1.0);
			CHECK(desc->def == 0.0);
			CHECK((desc->flags & CLAP_PARAM_IS_AUTOMATABLE) != 0);
			CHECK((desc->flags & CLAP_PARAM_IS_MODULATABLE) != 0);
		}
	}

	SUBCASE("a macro is never its own destination") {
		// What makes resolved() terminate rather than recurse, and the reference's own rule about a source
		// patched to itself.
		for (size_t i = 0; i < feathers::kMacroCount; ++i) {
			CAPTURE(i);
			CHECK(feathers::modDestinationIndexOf(feathers::macroParam(i)) < 0);
		}
	}
}

// FOUR CASES ABOUT THE FILTER LIVED HERE AND HAVE MOVED, because each tested the seam between this
// table and the device — a plugin param row built from the module's descriptor, arriving at the DSP
// as a Settings field — and the swallowing removed that seam rather than changing it. None of the
// claims were dropped; each went where it is still decidable:
//
//   "a macro routed to the filter reaches the DSP's own settings"  -> tests/test_synth.cpp, against
//       the filter the ENGINE runs, reached through the route table, which is a swallowed device's
//       only road. It reads busFilterTargetForTest rather than a param table.
//   "a knob's range stops modulation except where the device says otherwise"  -> the general half is
//       below; the cutoff's exception is now setModulatedValueOf's, in tests/test_fx_filter.cpp.
//   "the slope the plugin publishes is the slope the DSP runs"  -> tests/test_fx_filter.cpp's
//       descriptor contract, which asserts the same mapping without a plugin id in the middle.
//   "spread is a param of its own and reaches the DSP as one"  -> the same place, for the same
//       reason: spread is a control of the device now, and its descriptor is the whole claim.

TEST_CASE("a knob's range stops modulation, and it is the range rather than the base that stops it") {
	// THE DEFAULT, AND IT IS UNCHANGED: for a param the declared range is the whole truth — a level
	// above its top or a voice count above the pool are not settings that exist — and resolved()
	// clamping base-plus-offset is what makes an offset safe to accept from a host.
	//
	// THE EXCEPTION NO LONGER HAS A PARAM TO SIT ON. `offsetsMayLeaveRange` is still read here and
	// still means what it meant; the one control that ever asked for it was the filter's cutoff, which
	// is a device control now. That is why this case tests the RULE and test_fx_filter tests the
	// exception — and why the assertion below walks the table rather than naming a param, so a future
	// param that asks for it cannot arrive unnoticed.
	feathers::ParamSet params;
	const feathers::ParamDesc* level = findParam(static_cast<clap_id>(ParamId::VoiceLevel));
	REQUIRE(level != nullptr);
	REQUIRE_FALSE(level->offsetsMayLeaveRange);

	params.setBase(static_cast<clap_id>(ParamId::VoiceLevel), level->max);
	params.setModOffset(static_cast<clap_id>(ParamId::VoiceLevel), 2.0);
	CHECK(params.resolved(ParamId::VoiceLevel) == doctest::Approx(level->max));
	params.setBase(static_cast<clap_id>(ParamId::VoiceLevel), level->min);
	params.setModOffset(static_cast<clap_id>(ParamId::VoiceLevel), -2.0);
	CHECK(params.resolved(ParamId::VoiceLevel) == doctest::Approx(level->min));

	// AND NO PARAM IN THE TABLE ASKS FOR THE EXCEPTION TODAY. Stated as a walk so that the day one
	// does, this case says so rather than quietly continuing to pass.
	// ONE DOES SINCE 2026-10-03, AND ONLY ONE: Phase, because phase WRAPS — a clamp would pin a modulated read phase
	// at an end, a hard nonlinearity rather than phase modulation. And it is honoured: a route past the top arrives.
	for (const feathers::ParamDesc& d : feathers::paramDescs()) {
		CAPTURE(std::string(d.name));
		if (d.id == ParamId::OscPhase) {
			CHECK(d.offsetsMayLeaveRange);
			continue;
		}
		CHECK_FALSE(d.offsetsMayLeaveRange);
	}
	params.setBase(static_cast<clap_id>(ParamId::OscPhase), 0.5);
	params.setModOffset(static_cast<clap_id>(ParamId::OscPhase), 0.75);
	CHECK(params.resolved(ParamId::OscPhase) == doctest::Approx(1.25));
}

TEST_CASE("a patch with nothing routed sounds exactly like a patch from before the bus existed") {
	// THE ZERO-EFFECT CLAIM. It is the crown of this feature and it is worth stating what would make it
	// vacuous: a bus that always returned zero would pass this and fail the case below it, which is why
	// the two are written as a pair and neither is allowed to stand alone.
	feathers::ParamSet params;
	params.setBase(static_cast<clap_id>(ParamId::Path), 0.4);
	params.setBase(static_cast<clap_id>(ParamId::FineTune), -25.0);
	params.setModOffset(static_cast<clap_id>(ParamId::Path), 0.1);

	SUBCASE("with the macros at rest") {
		for (const ParamDesc& desc : paramDescs()) {
			CAPTURE(std::string(desc.name));
			CHECK(params.resolved(desc.id) == params.base(desc.id) + (desc.id == ParamId::Path ? 0.1 : 0.0));
		}
	}

	SUBCASE("and with every macro pushed to the top") {
		// A macro that has been turned up but routed nowhere reaches nowhere. This is the case a user
		// meets first — four new knobs that do nothing until they are pointed at something — and the one
		// that would be embarrassing to get wrong.
		for (size_t i = 0; i < feathers::kMacroCount; ++i) {
			params.setBase(static_cast<clap_id>(feathers::macroParam(i)), 1.0);
		}
		CHECK(params.resolved(ParamId::Path) == doctest::Approx(0.5));
		CHECK(params.resolved(ParamId::FineTune) == doctest::Approx(-25.0));
		// Level's declared base is ZERO since backlog EF — the default patch's envelope carries it, over
		// a route the bridge makes and this bare table does not have — so nothing routed resolves to 0.
		CHECK(params.resolved(ParamId::VoiceLevel) == doctest::Approx(0.0));
	}
}

TEST_CASE("an amount is a fraction of the destination's whole range, whatever that range is") {
	feathers::ParamSet params;
	params.setBase(static_cast<clap_id>(feathers::macroParam(0)), 1.0);

	SUBCASE("half of Path's range is half of one") {
		params.setBase(static_cast<clap_id>(ParamId::Path), 0.25);
		routed(0, ParamId::Path, 0.5f).publish(params);
		CHECK(params.resolved(ParamId::Path) == doctest::Approx(0.75));
	}

	SUBCASE("half of Fine Tune's range is a hundred cents") {
		// The same amount on a parameter whose range is two hundred units wide moves it a hundred, which
		// is what lets one ring on one face be drawn from the amount without asking what it is attached
		// to.
		params.setBase(static_cast<clap_id>(ParamId::FineTune), -50.0);
		routed(0, ParamId::FineTune, 0.5f).publish(params);
		CHECK(params.resolved(ParamId::FineTune) == doctest::Approx(50.0));
	}

	SUBCASE("the source's position scales it, and zero reaches nowhere") {
		params.setBase(static_cast<clap_id>(ParamId::Path), 0.0);
		routed(0, ParamId::Path, 1.0f).publish(params);
		params.setBase(static_cast<clap_id>(feathers::macroParam(0)), 0.5);
		CHECK(params.resolved(ParamId::Path) == doctest::Approx(0.5));
		params.setBase(static_cast<clap_id>(feathers::macroParam(0)), 0.0);
		CHECK(params.resolved(ParamId::Path) == doctest::Approx(0.0));
	}

	SUBCASE("the sum still lands inside the declared range") {
		// An amount reaches twice the range on purpose, so the clamp is not an edge case — it is where
		// half the useful settings live.
		params.setBase(static_cast<clap_id>(ParamId::Path), 0.8);
		routed(0, ParamId::Path, feathers::kMaxRouteAmount).publish(params);
		CHECK(params.resolved(ParamId::Path) == doctest::Approx(1.0));
	}

	SUBCASE("four sources onto one destination add up") {
		params.setBase(static_cast<clap_id>(ParamId::Path), 0.0);
		feathers::test::Routes routes;
		const int at = feathers::modDestinationIndexOf(ParamId::Path);
		REQUIRE(at >= 0);
		for (size_t source = 0; source < feathers::kMacroCount; ++source) {
			routes.set(feathers::test::macro(source), feathers::backplane::Destination::core(static_cast<size_t>(at)), 0.1f);
			params.setBase(static_cast<clap_id>(feathers::macroParam(source)), 1.0);
		}
		routes.publish(params);
		CHECK(params.resolved(ParamId::Path) == doctest::Approx(0.4));
	}
}

TEST_CASE("the host's modulation lane and the macros' lane do not delete each other") {
	// C5, and the reason the two are separate arrays at all. A CLAP mod event OVERWRITES its param's
	// offset and reset() clears the whole lane — both correct for a lane the host owns, and both
	// destructive if a macro's contribution were living in it.
	feathers::ParamSet params;
	params.setBase(static_cast<clap_id>(ParamId::Path), 0.2);
	params.setBase(static_cast<clap_id>(feathers::macroParam(1)), 1.0);
	routed(1, ParamId::Path, 0.5f).publish(params);
	params.setModOffset(static_cast<clap_id>(ParamId::Path), 0.1);

	CHECK(params.resolved(ParamId::Path) == doctest::Approx(0.8));

	SUBCASE("a host clearing its modulation leaves the patch's own") {
		params.clearModulation();
		CHECK(params.resolved(ParamId::Path) == doctest::Approx(0.7));
	}

	SUBCASE("a host event overwriting its offset leaves the patch's own") {
		params.setModOffset(static_cast<clap_id>(ParamId::Path), 0.0);
		CHECK(params.resolved(ParamId::Path) == doctest::Approx(0.7));
	}

	SUBCASE("unrouting leaves the host's") {
		feathers::test::Routes {}.publish(params);
		CHECK(params.resolved(ParamId::Path) == doctest::Approx(0.3));
	}

	SUBCASE("a host modulating a MACRO reaches everything that macro feeds") {
		// The macro's own reading is resolved(), not base(), which is what makes a macro a legitimate
		// target for a host modulator: one lane in the host, six destinations in here.
		params.setBase(static_cast<clap_id>(feathers::macroParam(1)), 0.0);
		CHECK(params.resolved(ParamId::Path) == doctest::Approx(0.3));
		params.setModOffset(static_cast<clap_id>(feathers::macroParam(1)), 1.0);
		CHECK(params.resolved(ParamId::Path) == doctest::Approx(0.8));
	}
}

TEST_CASE("the host cannot automate the engine") {
	// Engine modulation is not supported, by design. Backlog 21.
	//
	// TWO ROADS REACHED THIS PARAMETER AND ONLY ONE WAS EVER CLOSED. Internal modulation never
	// reached it -- `kCoreModDestinations` is eight continuous params and stepped controls are
	// excluded by rule -- and that was sometimes offered as the reason the engine was safe, which is
	// a right answer from a wrong mechanism. The other road was open in every DAW: `kStepped` carries
	// `CLAP_PARAM_IS_AUTOMATABLE`, so a host could draw a lane on the engine and switch it under a
	// sounding note.
	//
	// BOTH ARE ASSERTED, because closing one while believing the other was already shut is exactly
	// how this stayed open. A revert of either fails here.
	const ParamDesc* engine = findParam(static_cast<clap_id>(ParamId::OscType));
	REQUIRE(engine != nullptr);

	CHECK((engine->flags & CLAP_PARAM_IS_AUTOMATABLE) == 0);
	CHECK((engine->flags & CLAP_PARAM_IS_MODULATABLE) == 0);
	CHECK(feathers::modDestinationIndexOf(ParamId::OscType) < 0);

	// AND IT IS STILL A PARAMETER. Not automatable is not the same as not present: the editor writes
	// it, a patch stores it, and the host lists it. `CLAP_PARAM_IS_READONLY` would have said the
	// value cannot be changed at all rather than that the HOST may not change it, which is a
	// different and wrong claim.
	CHECK((engine->flags & CLAP_PARAM_IS_STEPPED) != 0);
	CHECK((engine->flags & CLAP_PARAM_IS_READONLY) == 0);
	CHECK(engine->max > engine->min);
}

TEST_CASE("every modulation destination is a param the host lane would also accept") {
	// TWO LANES REACH THE SAME `resolved()` AND ONLY ONE OF THEM CHECKS ANYTHING.
	//
	//     resolved() = base_ + mod_ + sourceOffsetAt()
	//                          ^^^^   ^^^^^^^^^^^^^^^^
	//                    host lane    internal routes
	//
	// `setModOffset` — the HOST lane — refuses a param that is not declared MODULATABLE, and says
	// why: "a mod offset on one of them would be meaningless anyway — resolved() would round a voice
	// count mid-block". `sourceOffsetAt` — the ROUTE lane, which is where a macro, an envelope or a
	// weight arrives — checks no such thing. It adds an offset to whatever destination the table
	// names.
	//
	// AND `buildModDestinations` NEVER CONSULTS THE FLAG. The table is written by hand: eight core
	// ids, then eight per-sound knobs per slot. Nothing stops a stepped param being added to
	// `kCoreModDestinations`, and if one were, the editor would draw a ring on it, a drag would write
	// an amount, the engine would apply it, and the same modulation arriving from the host would be
	// refused at the door. That is the honesty rule failing in the direction that is hardest to see:
	// not a control that does nothing, but a control that does something its own plugin says is
	// meaningless.
	//
	// MEASURED TODAY: 72 destinations, 0 not-modulatable, 0 stepped. The invariant holds and nothing
	// was holding it.
	//
	// AND THE KNOWN-BAD SHOWED THE ASYMMETRY AS A VALUE RATHER THAN AS A FLAG, which is the part
	// worth knowing: with `ParamId::Path` swapped for the stepped `ParamId::VoiceCount` in
	// `kCoreModDestinations`, the two flag checks below fail AND the host-lane check fails — while
	// the ROUTE-lane check at the end still PASSES. The route really did move a voice count; the
	// host lane really did refuse the same move. One `resolved()`, two lanes, two answers.
	REQUIRE(feathers::kParamDestinationCount > 0);
	size_t checked = 0;
	for (size_t i = 0; i < feathers::kParamDestinationCount; ++i) {
		const feathers::ParamId id = feathers::kModDestinations[i];
		const feathers::ParamDesc* desc = findParam(static_cast<clap_id>(id));
		// EVERY ENTRY NAMES A REAL PARAM. `buildModDestinations` hands back an all-zero array if its
		// count does not fill the list, and zero is not a param id — the static_assert beside it
		// catches that at build time, and this catches the same thing where a failure can say so.
		REQUIRE(desc != nullptr);
		CAPTURE(std::string(desc->name));

		// THE CLAIM: the route lane cannot reach a param the host lane would refuse.
		CHECK((desc->flags & CLAP_PARAM_IS_MODULATABLE) != 0);
		// AND THE REASON THE HOST LANE REFUSES THEM, stated separately because it is a different
		// fact: a stepped destination is one `resolved()` would round mid-block.
		CHECK((desc->flags & CLAP_PARAM_IS_STEPPED) == 0);
		++checked;
	}
	// THE SWEEP REALLY WALKED THE TABLE. A destination count that had drifted to zero would satisfy
	// every assertion above by asking nothing — the empty-filter shape, arriving in a loop bound.
	CHECK(checked == feathers::kParamDestinationCount);

	// AND THE TWO LANES ARE ASKED THE SAME QUESTION THROUGH THEIR OWN DOORS, rather than compared as
	// flags: set an offset from each and require both to land. A flag comparison would agree with
	// itself if `setModOffset`'s gate ever changed which flag it reads.
	feathers::ParamSet params;
	const int at = feathers::modDestinationIndexOf(feathers::kModDestinations[0]);
	REQUIRE(at >= 0);
	const auto id = static_cast<clap_id>(feathers::kModDestinations[0]);
	const feathers::ParamDesc* first = findParam(id);
	REQUIRE(first != nullptr);
	const double mid = 0.5 * (first->min + first->max);
	params.setBase(id, mid);

	// The host lane.
	params.setModOffset(id, 0.0);
	const double unmodulated = params.resolved(id);
	params.setModOffset(id, (first->max - first->min) * 0.25);
	CHECK(params.resolved(id) > unmodulated);

	// The route lane, through a macro at full travel.
	params.setModOffset(id, 0.0);
	feathers::test::Routes routes;
	routes.set(feathers::test::macro(0), feathers::backplane::Destination::core(static_cast<size_t>(at)), 0.25f);
	routes.publish(params);
	params.setBase(static_cast<clap_id>(feathers::macroParam(0)), 1.0);
	CHECK(params.resolved(id) > unmodulated);
}

TEST_CASE("every display unit the formatter handles has a param that exercises it, or is recorded as having none") {
	// A UNIT WITH NO PARAM IS A FORMATTER NOTHING CAN TEST, AND IT ROTS.
	//
	// MEASURED: `ParamUnit::LogHertz` has ZERO users. Its only one was the filter's cutoff, which is
	// a device control now — the same fate `offsetsMayLeaveRange` met, and that flag has a case
	// saying so while this had none. The consequence is not hypothetical: moving `valueToText`'s kHz
	// band from 1 kHz to 100 kHz — a hundredfold error in what a musician would read — leaves all 23
	// cases in this suite GREEN, because no param can reach the branch.
	//
	// SO THIS CASE IS WRITTEN THE WAY THE `offsetsMayLeaveRange` ONE IS, and for its stated reason:
	// to be TOLD rather than to prevent. Adding a LogHertz param is a good thing and this case going
	// red is how the next author learns that the three display bands behind it have been unexercised
	// since the swallowing and should be checked before they are trusted.
	std::array<size_t, 18> users {};
	for (const feathers::ParamDesc& d : feathers::paramDescs()) {
		const auto unit = static_cast<size_t>(d.unit);
		REQUIRE(unit < users.size()); // a unit past the end of this table is a table that went stale
		users[unit]++;
	}

	// THE SWEEP REALLY WALKED THE TABLE. Without this a paramDescs() that had drifted to empty would
	// satisfy the zero-user assertion below and nothing else would notice.
	size_t total = 0;
	for (const size_t n : users) {
		total += n;
	}
	CHECK(total == feathers::kParamCount);

	const auto count = [&](feathers::ParamUnit u) { return users[static_cast<size_t>(u)]; };

	// EVERY OTHER UNIT HAS AT LEAST ONE PARAM, so the round-trip sweep in `text conversion
	// round-trips every param` really does reach each formatter branch. Named one at a time rather
	// than looped, because "which units are live" is the fact being pinned and a loop over an
	// enum would restate the enum rather than the fact.
	CHECK(count(feathers::ParamUnit::Number) > 0);
	CHECK(count(feathers::ParamUnit::Integer) > 0);
	CHECK(count(feathers::ParamUnit::Zone) > 0);
	CHECK(count(feathers::ParamUnit::Percent) > 0);
	CHECK(count(feathers::ParamUnit::Cents) > 0);
	CHECK(count(feathers::ParamUnit::Semitones) > 0);
	CHECK(count(feathers::ParamUnit::Octaves) > 0);
	CHECK(count(feathers::ParamUnit::Decibels) > 0);
	CHECK(count(feathers::ParamUnit::Gain) > 0);
	// USERS AGAIN SINCE 2026-10-12 (none from 2026-10-03, when Attack and Release went): the spectral drawer's Damp Time.
	// Its two display bands are checked by value at the end of this case.
	CHECK(count(feathers::ParamUnit::LogSeconds) > 0);
	CHECK(count(feathers::ParamUnit::GlideSeconds) > 0);
	CHECK(count(feathers::ParamUnit::BendChoice) > 0);
	CHECK(count(feathers::ParamUnit::Choice) > 0);
	// The multipliers (2026-09-26, Resonate's shaping): x on the value, and x on 2^value.
	CHECK(count(feathers::ParamUnit::Factor) > 0);
	CHECK(count(feathers::ParamUnit::LogFactor) > 0);
	// Linear Hz, signed (2026-10-12): the spectral Shift. Its band is checked by value at the end of this case.
	CHECK(count(feathers::ParamUnit::Hertz) > 0);
	// A vowel position (2026-10-12): the spectral Vowel. Its names are checked by value at the end of this case.
	CHECK(count(feathers::ParamUnit::Vowel) > 0);

	// LOGHERTZ HAS USERS AGAIN (2026-10-10): the spectral drawer's Cutoff and Formant Shift crossover. This line read `== 0`
	// and asked that the three display bands be checked before it went; they are, below, each band by its own value and
	// each read back through `textToValue` (the round-trip case beside this one sweeps the whole travel as well).
	CHECK(count(feathers::ParamUnit::LogHertz) > 0);
	feathers::ParamSet params;
	const auto cutoff = static_cast<clap_id>(feathers::spectralParam(0, feathers::SpectralControl::FilterCutoff));
	for (const auto& [hz, want] : { std::pair { 2000.0, "2.00 kHz" }, std::pair { 440.0, "440 Hz" },
	                                std::pair { 50.0, "50.0 Hz" } }) {
		CAPTURE(hz);
		char text[64] {};
		REQUIRE(params.valueToText(cutoff, std::log2(hz), text, sizeof(text)));
		CHECK(std::string(text) == want);
		double back = 0.0;
		REQUIRE(params.textToValue(cutoff, text, &back));
		CHECK(std::exp2(back) == doctest::Approx(hz).epsilon(0.005));
	}
	// AND LOGSECONDS' TWO BANDS, through Damp Time: milliseconds below a second, seconds from it, each read back.
	const auto damp = static_cast<clap_id>(feathers::spectralParam(0, feathers::SpectralControl::DampTime));
	for (const auto& [s, want] : { std::pair { 2.0, "2.000 s" }, std::pair { 0.25, "250.00 ms" }, std::pair { 0.05, "50.00 ms" } }) {
		CAPTURE(s);
		char text[64] {};
		REQUIRE(params.valueToText(damp, std::log2(s), text, sizeof(text)));
		CHECK(std::string(text) == want);
		double back = 0.0;
		REQUIRE(params.textToValue(damp, text, &back));
		CHECK(std::exp2(back) == doctest::Approx(s).epsilon(1e-4));
	}
	// AND HERTZ, signed and linear, through Shift Hz: each read back, a kHz spelling too.
	const auto shift = static_cast<clap_id>(feathers::spectralParam(0, feathers::SpectralControl::ShiftHz));
	for (const auto& [hz, want] : { std::pair { 50.0, "+50.0 Hz" }, std::pair { -1000.0, "-1000.0 Hz" }, std::pair { 0.0, "+0.0 Hz" } }) {
		CAPTURE(hz);
		char text[64] {};
		REQUIRE(params.valueToText(shift, hz, text, sizeof(text)));
		CHECK(std::string(text) == want);
		double back = 1.0e9;
		REQUIRE(params.textToValue(shift, text, &back));
		CHECK(back == doctest::Approx(hz));
	}
	double kilo = 0.0;
	REQUIRE(params.textToValue(shift, "-0.5 kHz", &kilo));
	CHECK(kilo == doctest::Approx(-500.0));
	// AND VOWEL, through the spectral Vowel: the nearest anchor's name and the position, each read back; a name alone is its
	// anchor.
	const auto vowel = static_cast<clap_id>(feathers::spectralParam(0, feathers::SpectralControl::FormantVowel));
	for (const auto& [pos, want] : { std::pair { 3.0, "Open 3.00" }, std::pair { 5.4, "Nasal 5.40" }, std::pair { 0.2, "Breath 0.20" } }) {
		CAPTURE(pos);
		char text[64] {};
		REQUIRE(params.valueToText(vowel, pos, text, sizeof(text)));
		CHECK(std::string(text) == want);
		double back = -1.0;
		REQUIRE(params.textToValue(vowel, text, &back));
		CHECK(back == doctest::Approx(pos));
	}
	double named = -1.0;
	REQUIRE(params.textToValue(vowel, "rasp", &named));
	CHECK(named == 7.0);
}

TEST_CASE("a continuous param's display survives a round trip across its whole range") {
	// THE SIBLING SWEEP SAMPLES FOUR VALUES PER PARAM. That is enough to prove the special displays
	// work — "Off", "-inf dB", the enum names all live at an endpoint or the default — and it is not
	// enough to prove the display carries the resolution a musician needs, because a lost digit only
	// shows up between the samples.
	//
	// MEASURED, both directions, at 65 samples per continuous param:
	//   as shipped                     worst 0.468% of span  (Level, '-0.9 dB' -> 0.901571)
	//   with Gain printed %+.0f not %+.1f   worst 4.75% of span  (Level, '-1 dB'  -> 0.891251)
	// and the four-value sweep stays GREEN through that mutation. It is the SAMPLING that misses it,
	// not the tolerance: none of min/def/max/distinct happens to land where a dropped digit hurts.
	//
	// THE BOUND IS TAKEN FROM THE INSTRUMENT, not chosen: 1% of span sits twice above the worst the
	// code actually produces and nearly five times below what losing one digit produces. If a display
	// is deliberately made coarser, this reddens — which is the point, because that is a change a
	// player would see in a DAW's automation readout and never think to report.
	//
	// AGAINST THE SPAN RATHER THAN AGAINST THE VALUE. doctest's Approx(v).epsilon() is relative to v,
	// which collapses to nothing near zero and goes slack for large values — the wrong shape for
	// "can I see the difference when I move the knob", which is a question about the travel.
	//
	// STEPPED PARAMS ARE EXCLUDED and that is not a dodge: a stepped param has no legal value between
	// its steps, `sanitizeValue` rounds one away at the door, and sampling one at a half-step asks it
	// to preserve a setting it cannot hold. The sibling sweep covers them at values they can.
	ParamSet params;
	constexpr double kMaxDrift = 0.01; // of span — see the measurement above
	size_t sampled = 0;
	size_t continuous = 0;
	double worst = 0.0;

	for (const ParamDesc& desc : paramDescs()) {
		if ((desc.flags & CLAP_PARAM_IS_STEPPED) != 0) {
			continue;
		}
		++continuous;
		const auto id = static_cast<clap_id>(desc.id);
		const double span = desc.max - desc.min;
		REQUIRE(span > 0.0);
		CAPTURE(std::string(desc.name));

		for (int k = 0; k <= 64; ++k) {
			const double value = desc.min + span * (static_cast<double>(k) / 64.0);
			CAPTURE(value);
			char text[CLAP_NAME_SIZE] = {};
			// A REFUSAL IS A HARD FAILURE, not a tolerance question: a host that cannot be told what a
			// value is called shows the user nothing. Measured 0 refusals over the whole table.
			REQUIRE(params.valueToText(id, value, text, sizeof(text)));
			CAPTURE(text);
			double back = 0.0;
			REQUIRE(params.textToValue(id, text, &back));

			const double drift = std::abs(back - value) / span;
			if (drift > worst) {
				worst = drift;
			}
			CHECK(drift <= kMaxDrift);
			++sampled;
		}
	}

	// THE SWEEP REALLY RAN. A table that had drifted to all-stepped, or a loop bound that stopped
	// early, would satisfy every CHECK above by making none of them — the empty-filter shape again.
	CHECK(continuous > 20);
	CHECK(sampled == continuous * 65);
	// AND THE HEADROOM IS REPORTED, so the next person changing a display can see how much of the
	// bound they are spending rather than only whether they broke it.
	MESSAGE("worst continuous round trip: " << (worst * 100.0) << "% of span, bound "
	                                        << (kMaxDrift * 100.0) << "%");
}

//------------------------------------------------------------------------------------------------//
// THE EDGE STORE — what replaced the 27,840-cell matrix, and the bound it added                    //
//------------------------------------------------------------------------------------------------//

TEST_CASE("bipolar-macro-core: a bipolar macro reaches a CORE destination both ways — 0 is -depth, the notch nothing, 1 is +depth, on every road") {
	// The defect: a macro switched bipolar published 2v - 1 into its cell (backlog EG), and the rack/device
	// road reads the cell — but every CORE route from a macro read `macroReading`, the raw 0..1 parameter, so on Pitch,
	// Fine Tune, Radius, Theta … a bipolar macro played UNIPOLAR while its ring drew both ways. BY VALUE through ParamSet,
	// on every road a macro reaches: the block resolve (`resolved`), an internal destination (`internalOffset`), the
	// per-voice resolve (`internalOffset` at a voice), the per-voice window plan (`buildWindowPlan`), and the readout's
	// probe (`probeRoute`). And a UNIPOLAR macro's reading is bit-identical to the expression it always was.
	//
	// KNOWN-BAD: `macroReading` without the bipolar mask (the old body) — every bipolar check goes red, unipolar stays green.
	namespace bp = feathers::backplane;
	ParamSet params;
	const auto fine = static_cast<clap_id>(ParamId::FineTune);
	const double fineSpan = 2.0 * feathers::kFineTuneMaxCents;
	const int at = feathers::modDestinationIndexOf(ParamId::FineTune);
	REQUIRE(at >= 0);
	const bp::Destination fineDest = bp::Destination::core(static_cast<size_t>(at));
	const size_t theta = feathers::travelInternalIndex(1, feathers::TravelAxis::Theta);
	const double turn = 2.0 * 3.14159265358979323846;
	constexpr float kFine = 0.25f;
	constexpr float kTheta = 0.5f;
	feathers::test::Routes routes;
	routes.set(feathers::test::macro(0), fineDest, kFine);
	routes.set(feathers::test::macro(0), bp::Destination::core(feathers::travelDestinationIndex(1, feathers::TravelAxis::Theta)),
	           kTheta);
	REQUIRE(routes.publish(params));

	for (const bool bipolar : { false, true }) {
		CAPTURE(bipolar);
		params.setMacroBipolarMask(bipolar ? 1u : 0u);
		for (const double v : { 0.0, 0.2, 0.5, 0.8, 1.0 }) {
			CAPTURE(v);
			params.setBase(static_cast<clap_id>(feathers::macroParam(0)), v);
			params.publishCoreSources();
			const double source = bipolar ? 2.0 * v - 1.0 : v;
			// THE BLOCK RESOLVE: Fine Tune sits at 0, so what it reads IS the route's term.
			const double fineOffset = params.resolved(fine);
			// THE INTERNAL DESTINATION, at no voice and at a voice.
			const double thetaPull = params.internalOffset(theta);
			const double thetaVoice = params.internalOffset(theta, 3);
			// THE PER-VOICE WINDOW PLAN's scalar half, where a macro input lands.
			ParamSet::WindowPlan plan;
			params.buildWindowPlan(ParamId::FineTune, plan);
			// THE READOUT'S PROBE: the source reading the editor's as-modulated readout is computed from.
			bp::RouteProbe probe;
			REQUIRE(params.probeRoute(fineDest, feathers::test::macro(0), probe));
			if (!bipolar) {
				// BIT-IDENTICAL to the expression a unipolar macro always was.
				CHECK(fineOffset == static_cast<double>(kFine) * v * fineSpan);
				CHECK(thetaPull == static_cast<double>(kTheta) * v * turn);
				CHECK(probe.source == v);
			}
			CHECK(fineOffset == doctest::Approx(static_cast<double>(kFine) * source * fineSpan));
			CHECK(thetaPull == doctest::Approx(static_cast<double>(kTheta) * source * turn));
			CHECK(thetaVoice == thetaPull);
			CHECK(plan.scalar == doctest::Approx(static_cast<double>(kFine) * source * fineSpan));
			CHECK(probe.source == doctest::Approx(source));
		}
		// THE NOTCH IS THE REST: half way up a bipolar macro adds exactly nothing.
		if (bipolar) {
			params.setBase(static_cast<clap_id>(feathers::macroParam(0)), 0.5);
			params.publishCoreSources();
			CHECK(params.resolved(fine) == 0.0);
			CHECK(params.internalOffset(theta) == 0.0);
		}
	}
	// A RELATIVE MACRO STILL RESTS AT 0, whatever its polarity says (backlog PQ).
	params.setMacroBipolarMask(1u);
	params.setMacroRelativeMask(1u);
	params.setBase(static_cast<clap_id>(feathers::macroParam(0)), 0.0);
	params.publishCoreSources();
	CHECK(params.resolved(fine) == 0.0);
	CHECK(params.internalOffset(theta) == 0.0);
}

TEST_CASE("backlog EG: a macro switched bipolar publishes 2v - 1 of the same 0..1 parameter into its cell, and the parameter itself is untouched") {
	// THE PUBLISHED CELL IS WHAT EVERY ROUTE FROM A MACRO READS, so this is the engine half of the
	// mode: asserted BY VALUE against the arithmetic the mode declares, for both modes, and the base
	// underneath read back unchanged — a mode that re-ranged the parameter would re-point the DAW's
	// automation lane, which is the one thing it must not do.
	ParamSet params;
	params.setBase(static_cast<clap_id>(feathers::macroParam(0)), 0.25);
	params.setBase(static_cast<clap_id>(feathers::macroParam(1)), 0.25);
	CHECK(params.macroBipolarMask() == 0u);
	params.publishCoreSources();
	CHECK(params.coreSourceCells()[0] == doctest::Approx(0.25f));
	CHECK(params.coreSourceCells()[1] == doctest::Approx(0.25f));

	params.setMacroBipolarMask(1u << 1);
	CHECK(params.macroBipolarMask() == (1u << 1));
	params.publishCoreSources();
	CHECK(params.coreSourceCells()[0] == doctest::Approx(0.25f));  // untouched: not its bit
	CHECK(params.coreSourceCells()[1] == doctest::Approx(-0.5f));  // 2 x 0.25 - 1
	CHECK(params.base(feathers::macroParam(1)) == doctest::Approx(0.25)); // the parameter is what it was

	// THE CENTRE IS THE REST: half way up the parameter publishes nothing at all, and the ends the ends.
	params.setBase(static_cast<clap_id>(feathers::macroParam(1)), 0.5);
	params.publishCoreSources();
	CHECK(params.coreSourceCells()[1] == doctest::Approx(0.0f));
	params.setBase(static_cast<clap_id>(feathers::macroParam(1)), 1.0);
	params.publishCoreSources();
	CHECK(params.coreSourceCells()[1] == doctest::Approx(1.0f));
	params.setBase(static_cast<clap_id>(feathers::macroParam(1)), 0.0);
	params.publishCoreSources();
	CHECK(params.coreSourceCells()[1] == doctest::Approx(-1.0f));

	// AND BACK: clearing the bit publishes the parameter as it is.
	params.setMacroBipolarMask(0u);
	params.publishCoreSources();
	CHECK(params.coreSourceCells()[1] == doctest::Approx(0.0f));
}

TEST_CASE("relative macro: the engine reads a Relative macro as resting at 0, and an Absolute one exactly as before") {
	// BACKLOG PQ: a Relative macro's routes are EDITS of their destinations' bases, made on the message thread,
	// so the engine must add NOTHING for them per block — its cell publishes 0 and the live macro road reads 0.
	// And with the bit clear the reading is the one every Absolute macro always had: asserted by value against
	// an independent restatement of the route (base + amount x macro x span), not against another run of the
	// same code.
	const auto id = static_cast<clap_id>(ParamId::VoiceLevel);
	const ParamDesc* desc = findParam(id);
	REQUIRE(desc != nullptr);
	ParamSet params;
	const double base = desc->min + 0.25 * (desc->max - desc->min);
	const double macro = 0.6;
	const float amount = 0.3f;
	params.setBase(id, base);
	params.setBase(static_cast<clap_id>(feathers::macroParam(0)), macro);
	feathers::test::Routes routes;
	routes.set(feathers::test::macro(0), feathers::test::param(ParamId::VoiceLevel), amount);
	REQUIRE(routes.publish(params));

	const double expected =
	    std::clamp(base + static_cast<double>(amount) * macro * (desc->max - desc->min), desc->min, desc->max);
	REQUIRE(expected != base); // the route is live, so the relative case below is not vacuous
	CHECK(params.macroRelativeMask() == 0u);
	CHECK(params.resolved(id) == doctest::Approx(expected).epsilon(1e-12));
	params.publishCoreSources();
	CHECK(params.coreSourceCells()[0] == static_cast<float>(macro));

	params.setMacroRelativeMask(1u << 0);
	CHECK(params.resolved(id) == base); // exactly: nothing is added
	CHECK(params.macroReading(0) == 0.0);
	params.setMacroBipolarMask(1u << 0); // a polarity does not wake it: there is no value to read either way
	params.publishCoreSources();
	CHECK(params.coreSourceCells()[0] == 0.0f);
	CHECK(params.base(static_cast<clap_id>(feathers::macroParam(0))) == macro); // the parameter is untouched

	// AND BACK: clearing the bit is the Absolute macro again.
	params.setMacroBipolarMask(0u);
	params.setMacroRelativeMask(0u);
	CHECK(params.resolved(id) == doctest::Approx(expected).epsilon(1e-12));
}

TEST_CASE("relative macro: a route into a sound's identity is never compiled, so even an Absolute one leaves Gamma where its base is") {
	// A32: nothing MODULATES Zone, Offset or Gamma. A route there exists only as a Relative macro's edit road, and one
	// left behind by a macro switched back to Absolute must stay inert. Asserted with an Absolute macro at full, so a
	// route the engine DID compile would move Gamma by 0.5 x 1 x 8 = 4.
	const ParamId gamma = soundParam(0, SoundField::Gamma);
	const auto id = static_cast<clap_id>(gamma);
	REQUIRE(feathers::isEditOnlyDestination(static_cast<size_t>(feathers::modDestinationIndexOf(gamma))));
	ParamSet params;
	params.setBase(id, 1.0);
	params.setBase(static_cast<clap_id>(feathers::macroParam(0)), 1.0);
	feathers::test::Routes routes;
	routes.set(feathers::test::macro(0), feathers::test::param(gamma), 0.5f);
	(void)routes.publish(params);
	CHECK(params.resolved(id) == 1.0);
	CHECK_FALSE(params.isRouted(gamma));
	// THE CONTROL: the same route into a parameter that IS modulated moves it, so the publish above did land.
	routes.set(feathers::test::macro(0), feathers::test::param(ParamId::VoiceLevel), 0.5f);
	params.setBase(static_cast<clap_id>(ParamId::VoiceLevel), 0.0);
	(void)routes.publish(params);
	CHECK(params.resolved(static_cast<clap_id>(ParamId::VoiceLevel)) > 0.0);
	CHECK(params.resolved(id) == 1.0);
}

TEST_CASE("the engine's route store is an edge list, and it is smaller than the matrix by two orders") {
	// **THE HEADLINE CLAIM OF THE ARC, MEASURED RATHER THAN COMPUTED.** The old matrix measured 27,840
	// cells and 108.8 KB, of which a real patch uses about ten. This is the same number taken from the
	// type that replaced it, so the saving is a fact about the build rather than about an estimate.
	// **THE WHOLE `ParamSet` AGAINST WHAT ITS ROUTE TABLE ALONE USED TO BE**, which is the strongest
	// form of the claim available without reaching inside: the entire object — parameters, modulation
	// lane, device sources, weights and all — is now a fraction of what one member of it cost.
	const size_t matrixBytes = (feathers::kMacroCount + feathers::kWeightSourceCount
	                            + feathers::kMaxDeviceSlots * feathers::device::kMaxDeviceOutputs)
	                           * (feathers::kParamDestinationCount + feathers::kMaxDeviceSlots * 32) // 32: the old destination stride
	                           * sizeof(float); // what the dense table WAS, written out — no constant names it now
	MESSAGE("the matrix alone was " << matrixBytes << " bytes; the whole ParamSet is now "
	                                << sizeof(feathers::ParamSet));
	CHECK(matrixBytes > 100000); // the premise, so the ratio below cannot pass on a shrunken axis
	// A RATIO RATHER THAN A BYTE COUNT, so widening a stride does not turn a real property into a
	// brittle number that has to be edited whenever an axis grows.
	CHECK(sizeof(feathers::ParamSet) * 10 < matrixBytes);
}

TEST_CASE("backlog OJ: a list past the params' room is refused WHOLE and counted, and once room is made every route lands") {
	// **NO BOUND IS LEFT TO DROP AT** (backlog OJ): the published list grows without one, and the params' stores
	// grow on the message thread (`reserveRoutes`) when the bridge's list tells them to (`wireRouteGrowth`). A
	// params NOT told — an unwired host — keeps its starting room, and a list bigger than that must be refused
	// WHOLE and counted rather than truncated: a truncated copy is the silent drop the old bound existed to count.
	feathers::ParamSet params;
	constexpr size_t kHostSources = feathers::kMacroCount + feathers::kWeightSourceCount;
	feathers::test::Routes routes;
	const size_t want = feathers::kInitialRouteRoom + 7;
	for (size_t source = 0; source < kHostSources && routes.pending.size() < want; ++source) {
		for (size_t d = 0; d < feathers::kParamDestinationCount && routes.pending.size() < want; ++d) {
			routes.set(feathers::test::macro(source), feathers::backplane::Destination::core(d),
			           0.001f * static_cast<float>(1 + routes.pending.size()));
		}
	}
	REQUIRE(routes.pending.size() == want);
	REQUIRE(params.routeRoom() < want);

	// UNWIRED: refused, counted, nothing adopted.
	CHECK_FALSE(routes.publish(params));
	CHECK(params.routeRoomRefusals() == 1);
	CHECK(params.stagedRoutes().size() == 0);
	CHECK_FALSE(params.anyRoute());

	// ROOM MADE, AS THE LIST'S GROWTH HOOK WOULD: the same list lands whole, every amount by value.
	params.reserveRoutes(routes.list->capacity());
	REQUIRE(params.routeRoom() >= want);
	CHECK(routes.publish(params));
	CHECK(params.routeRoomRefusals() == 1);
	CHECK(params.stagedRoutes().size() == want);
	for (const feathers::backplane::Route& r : routes.pending) {
		CHECK(feathers::test::adoptedAmount(params, r.source, r.destination) == r.amount);
	}
}

// **FOUR MEASUREMENT CASES STOOD HERE AND ARE DELETED WITH THE THINGS THEY MEASURED (kill-lanes §2.4b,
// 2026-09-13):** the dense PUBLISH against the edge store's (the dense table is gone); the bus FILL and
// what per-voice resolution would multiply (backlog DU's gate — there is no bus to fill); the RESOLVE as
// edges against the sixty-source scan (neither exists: a node walks its own inputs); and the resolve's
// flatness in patch size (an input list is O(routes into this control) by construction). Their numbers
// live in the plan docs they were written for. A measurement of a mechanism that no longer runs is not
// a measurement.

TEST_CASE("the source axis admits every note expression CLAP defines") {
	// **THE CONSTANT THAT STOPPED THE NOTE-EXPRESSION MODULATOR EXISTING.** `kMaxDeviceOutputs` was 4;
	// `clap/events.h` defines seven expressions (VOLUME, PAN, TUNING, VIBRATO, EXPRESSION, BRIGHTNESS,
	// PRESSURE — ids 0..6), so a device that speaks them wants seven outputs and could not declare
	// them. The edges doc §6 named this constant as exactly that blocker.
	//
	// **PINNED AGAINST CLAP'S OWN NUMBER, not against 7.** A literal here would be a second place the
	// count lives, free to drift from the header that defines it; this reads the highest id CLAP
	// declares and asserts a device may publish one output per expression. If CLAP ever adds an
	// eighth, this fails and says so rather than a device silently losing its last output.
	constexpr size_t clapExpressionCount = CLAP_NOTE_EXPRESSION_PRESSURE + 1;
	static_assert(clapExpressionCount == 7, "clap/events.h changed its expression set");
	CHECK(feathers::device::kMaxDeviceOutputs >= clapExpressionCount);
}

TEST_CASE("backlog FI: the id lookups are tables, and they answer exactly what a scan of the lists answers — for every id") {
	// `paramIndexOf` and `modDestinationIndexOf` scanned their lists on every `resolved()`, dozens of times a control
	// tick. They are tables now; this restates the scan independently and compares every id the tables span and past.
	const std::span<const feathers::ParamDesc> descs = feathers::paramDescs();
	for (uint32_t id = 0; id < feathers::kParamIdSpan + 64; ++id) {
		int scanned = -1;
		for (size_t i = 0; i < descs.size(); ++i) {
			if (static_cast<uint32_t>(descs[i].id) == id) {
				scanned = static_cast<int>(i);
				break;
			}
		}
		int destination = -1;
		for (size_t i = 0; i < feathers::kModDestinations.size(); ++i) {
			if (static_cast<uint32_t>(feathers::kModDestinations[i]) == id) {
				destination = static_cast<int>(i);
				break;
			}
		}
		CAPTURE(id);
		CHECK(feathers::paramIndexOf(id) == scanned);
		CHECK(feathers::modDestinationIndexOf(static_cast<feathers::ParamId>(id)) == destination);
	}
	CHECK(feathers::paramIndexOf(static_cast<uint32_t>(feathers::ParamId::VoiceLevel)) >= 0); // not vacuous
}

//---------------------------------------------------------------------------------------------//
// THE CORE'S INTERNAL DESTINATIONS — modulatable, and not parameters (2026-09-21)               //
// They use the internal param type so as not to spend CLAP parameters.                        //
//---------------------------------------------------------------------------------------------//

TEST_CASE("an internal destination resolves in its own swing, and spends no CLAP parameter") {
	// A FULL SWING ON RADIUS IS ONE WHOLE FACE OF ITS KNOB (radius-mod-jump, 2026-11-07: routes move along the face;
	// `playedRadius` turns the face distance into a radius, whose top is the whole plane, backlog PO) — restated here
	// as a number rather than read back from the descriptor, so a change to either has to be a change to both.
	feathers::ParamSet params;
	const size_t radius = feathers::travelInternalIndex(2, feathers::TravelAxis::Radius);
	const size_t theta = feathers::travelInternalIndex(2, feathers::TravelAxis::Theta);

	feathers::test::Routes routes;
	routes.set(feathers::test::macro(0),
	           feathers::backplane::Destination::core(feathers::travelDestinationIndex(2, feathers::TravelAxis::Radius)),
	           1.0f);
	routes.set(feathers::test::macro(1),
	           feathers::backplane::Destination::core(feathers::travelDestinationIndex(2, feathers::TravelAxis::Theta)),
	           1.0f);
	REQUIRE(routes.publish(params));

	// NOTHING MOVES UNTIL A SOURCE DOES — the zero that keeps an unmodulated patch bit-exact.
	CHECK(params.internalOffset(radius) == 0.0);
	CHECK(params.internalOffset(theta) == 0.0);

	params.setBase(static_cast<clap_id>(feathers::macroParam(0)), 1.0);
	params.setBase(static_cast<clap_id>(feathers::macroParam(1)), 1.0);
	CHECK(params.internalOffset(radius) == doctest::Approx(1.0));
	// ONE WHOLE TURN, so a ramp into Theta is exactly one orbit.
	CHECK(params.internalOffset(theta) == doctest::Approx(2.0 * 3.14159265358979323846));

	// AND ONLY SLOT 2's. A neighbour's pair reads nothing, or the index arithmetic is wrong.
	CHECK(params.internalOffset(feathers::travelInternalIndex(1, feathers::TravelAxis::Radius)) == 0.0);
	CHECK(params.internalOffset(feathers::travelInternalIndex(3, feathers::TravelAxis::Theta)) == 0.0);

	// **AND NO PARAMETER WAS SPENT**, which is the design: the parameter table is exactly as long as it
	// was, and nothing in it is called Radius or Theta.
	for (const feathers::ParamDesc& d : feathers::paramDescs()) {
		const std::string name(d.name);
		CAPTURE(name);
		CHECK(name.find("Radius") == std::string::npos);
		CHECK(name.find("Theta") == std::string::npos);
	}
}

TEST_CASE("an internal destination's saved id survives the parameter table growing") {
	// A ROUTE ON DISK SPELLS AN INTERNAL DESTINATION BY A STABLE ID, NOT BY TABLE POSITION — because
	// the position is `kParamDestinationCount + n`, and that moves every time a parameter destination
	// is added ahead of it. This pins the ids themselves, which is the promise, and the round trip.
	CHECK(feathers::internalDestinationId(feathers::travelInternalIndex(0, feathers::TravelAxis::Radius)) == 0x0100u);
	CHECK(feathers::internalDestinationId(feathers::travelInternalIndex(0, feathers::TravelAxis::Theta)) == 0x0101u);
	CHECK(feathers::internalDestinationId(feathers::travelInternalIndex(7, feathers::TravelAxis::Theta)) == 0x0171u);
	for (size_t i = 0; i < feathers::kInternalDestinationCount; ++i) {
		CAPTURE(i);
		CHECK(feathers::internalIndexOfId(feathers::internalDestinationId(i)) == static_cast<int>(i));
	}
	// AND WHAT IT REFUSES: a parameter id, a slot past the last, an axis nobody has declared.
	CHECK(feathers::internalIndexOfId(0x0043u) == -1);
	CHECK(feathers::internalIndexOfId(0x0180u) == -1);
	CHECK(feathers::internalIndexOfId(0x0102u) == -1);
	// THE TWO REGIONS DO NOT OVERLAP, and together they are what the host declares.
	CHECK(feathers::feathersAxes().coreDestinations == feathers::kCoreDestinationCount);
	CHECK_FALSE(feathers::isInternalDestination(feathers::kParamDestinationCount - 1));
	CHECK(feathers::isInternalDestination(feathers::kParamDestinationCount));
	CHECK_FALSE(feathers::isInternalDestination(feathers::kCoreDestinationCount));
}

TEST_CASE("the Resonate Model is stepped, and neither automatable nor modulatable — OscType's flags") {
	// A model switch reseats the string and discards its motion (weave_string.h setModel), so it is a
	// patch choice: the same flags as the engine selector, whatever they become.
	const ParamDesc* model = findParam(static_cast<clap_id>(ParamId::ResonateModel));
	const ParamDesc* engine = findParam(static_cast<clap_id>(ParamId::OscType));
	REQUIRE(model != nullptr);
	REQUIRE(engine != nullptr);
	CHECK((model->flags & CLAP_PARAM_IS_STEPPED) != 0);
	CHECK((model->flags & CLAP_PARAM_IS_AUTOMATABLE) == 0);
	CHECK((model->flags & CLAP_PARAM_IS_MODULATABLE) == 0);
	CHECK((model->flags & CLAP_PARAM_IS_MODULATABLE_PER_NOTE_ID) == 0);
	CHECK(model->flags == engine->flags);
	CHECK(feathers::modDestinationIndexOf(ParamId::ResonateModel) < 0);
}

//--------------------------------------------------------------------------------------------------//
// ROUTE TRANSFER CHIPS, on Feathers' own destinations.                                                //
//--------------------------------------------------------------------------------------------------//

namespace {

using feathers::device::Transfer;

/// THE CURVES RESTATED BY HAND at the readings below — independently of `shapeTransfer`.
double restatedCurve(Transfer t, double x) {
	const double c = std::clamp(x, -1.0, 1.0);
	switch (t) {
	case Transfer::Lin: return x;
	case Transfer::Log: return (c < 0 ? -1.0 : 1.0) * (1.0 - (1.0 - std::abs(c)) * (1.0 - std::abs(c)));
	case Transfer::Exp: return (c < 0 ? -1.0 : 1.0) * c * c;
	case Transfer::Abs: return std::abs(c);
	case Transfer::Pos: return std::max(c, 0.0);
	case Transfer::Neg: return std::min(c, 0.0);
	default: return 0.0;
	}
}

constexpr Transfer kEveryTransfer[] = { Transfer::Lin, Transfer::Log, Transfer::Exp,
	                                    Transfer::Abs, Transfer::Pos, Transfer::Neg };

/// A DEVICE SOURCE THE CASE OWNS, with a cell, a per-voice run and per-voice windows for device 7 — and a
/// block-rate window for device 8, which has no per-voice run (an LFO's shape on the bus).
struct TransferCells {
	static constexpr size_t kVoices = 2;
	float cell = 0.0f;
	std::array<float, kVoices> perVoice {};
	std::array<float, kVoices * feathers::device::kTickWindowStride> windows {};
	std::array<float, feathers::device::kTickWindowStride> block {};

	ParamSet::DeviceCells cells() const {
		ParamSet::DeviceCells c;
		c.host = this;
		c.cellOf = [](const void* h, feathers::DeviceInstanceId, size_t) { return &static_cast<const TransferCells*>(h)->cell; };
		c.perVoiceCellOf = [](const void* h, feathers::DeviceInstanceId id, size_t) -> const float* {
			return id == 7 ? static_cast<const TransferCells*>(h)->perVoice.data() : nullptr;
		};
		c.voices = kVoices;
		c.perVoiceWindowOf = [](const void* h, feathers::DeviceInstanceId id, size_t) -> const float* {
			return id == 7 ? static_cast<const TransferCells*>(h)->windows.data() : nullptr;
		};
		c.windowOf = [](const void* h, feathers::DeviceInstanceId id, size_t) -> const float* {
			return id == 8 ? static_cast<const TransferCells*>(h)->block.data() : nullptr;
		};
		return c;
	}
};

} // namespace

TEST_CASE("transfer chips: each curve's shape at chosen points, and its label round-trips") {
	using feathers::device::shapeTransfer;
	// BY HAND, not by the restatement: the numbers the design doc states.
	CHECK(shapeTransfer(Transfer::Lin, -0.5f) == -0.5f);
	CHECK(shapeTransfer(Transfer::Lin, 2.0f) == 2.0f); // Lin never clamps
	CHECK(shapeTransfer(Transfer::Log, 0.5f) == 0.75f);
	CHECK(shapeTransfer(Transfer::Log, -0.5f) == -0.75f);
	CHECK(shapeTransfer(Transfer::Exp, 0.5f) == 0.25f);
	CHECK(shapeTransfer(Transfer::Exp, -0.5f) == -0.25f);
	CHECK(shapeTransfer(Transfer::Exp, 2.0f) == 1.0f); // every other curve clamps first
	CHECK(shapeTransfer(Transfer::Abs, -0.5f) == 0.5f);
	CHECK(shapeTransfer(Transfer::Pos, -0.5f) == 0.0f);
	CHECK(shapeTransfer(Transfer::Pos, 0.5f) == 0.5f);
	CHECK(shapeTransfer(Transfer::Neg, 0.5f) == 0.0f);
	CHECK(shapeTransfer(Transfer::Neg, -0.5f) == -0.5f);
	for (const Transfer t : kEveryTransfer) {
		CAPTURE(static_cast<int>(t));
		CHECK(shapeTransfer(t, 0.0f) == 0.0f); // a rested source adds nothing, whatever the curve
		CHECK(shapeTransfer(t, 1.0f) == (t == Transfer::Neg ? 0.0f : 1.0f));
		const char* label = feathers::device::transferLabel(t);
		REQUIRE(label != nullptr);
		const auto back = feathers::device::transferFromLabel(label, std::strlen(label));
		REQUIRE(back.has_value());
		CHECK(*back == t);
	}
	CHECK_FALSE(feathers::device::transferFromLabel("sqrt", 4).has_value());
	// A ROUTE NOBODY CURVED IS LIN, which is what makes every patch before this feature read as it did.
	CHECK(feathers::backplane::Route {}.transfer == Transfer::Lin);
}

//--------------------------------------------------------------------------------------------------//
// BACKLOG PY — THE φ-TRIANGLE TRANSFER.                                                              //
//--------------------------------------------------------------------------------------------------//

namespace {
/// **THE CURVE RESTATED BY HAND** from its words — the house unipolar triangle (0 → 1 → 0 over a cycle) read at
/// P·u plus a warp of two centred house triangles at P·φ⁻¹·u and P·φ^−½·u, depths 0.2 and 0.12 — in doubles,
/// with φ from its definition. Not `phiTriangleCurve`, not `PhiTriContext`.
double phiTri(double p) {
	const double x = p - std::floor(p);
	return x < 0.5 ? 2.0 * x : 2.0 * (1.0 - x);
}
double phiCurveRestated(double u, double periods) {
	const double phi = (1.0 + std::sqrt(5.0)) / 2.0;
	const double warp = 0.2 * (2.0 * phiTri(periods / phi * u + 0.25) - 1.0)
	                    + 0.12 * (2.0 * phiTri(periods / std::sqrt(phi) * u + 0.25) - 1.0);
	return phiTri(periods * u + warp);
}
/// The steepest the phase can run, per unit of P: the carrier's 1 plus the warp's 4·(0.2/φ + 0.12/√φ).
double phiPhaseRateMax() {
	const double phi = (1.0 + std::sqrt(5.0)) / 2.0;
	return 1.0 + 4.0 * (0.2 / phi + 0.12 / std::sqrt(phi));
}
/// The steepest the curve can be, per unit of input: the triangle's slope 2 times the phase's.
double phiSlopeBound(double periods) { return 2.0 * periods * phiPhaseRateMax(); }
} // namespace

TEST_CASE("PY: the φ transfer is the house φ-triangle restated, at sample points, at every period count, sign kept") {
	using namespace feathers::device;
	size_t points = 0;
	for (int step = 0; step <= kPhiSteps; step += 7) {
		const Transfer t = phiTransferAtStep(step);
		REQUIRE(isPhiTransfer(t));
		const double periods = std::exp2(static_cast<double>(step) / kPhiStepsPerOctave);
		CHECK(static_cast<double>(phiPeriods(t)) == doctest::Approx(periods).epsilon(1e-6));
		for (int i = 0; i <= 64; ++i) {
			const double u = static_cast<double>(i) / 64.0 * 0.999 + 0.0005;
			const double want = phiCurveRestated(u, periods);
			CAPTURE(step);
			CAPTURE(u);
			// FLOAT PHASES AGAINST A DOUBLE RESTATEMENT AT UP TO 128 PERIODS: 2e-3 is a phase error of a few 1e-4
			// times the steepest slope there is, far under any one triangle's swing.
			CHECK(std::abs(static_cast<double>(shapeTransfer(t, static_cast<float>(u))) - want) <= 2e-3);
			CHECK(std::abs(static_cast<double>(shapeTransfer(t, static_cast<float>(-u))) + want) <= 2e-3);
			++points;
		}
	}
	MESSAGE(points << " points");
	// A RESTED SOURCE ADDS NOTHING — exactly, not nearly.
	CHECK(shapeTransfer(phiTransfer(kPhiOctavesDefault), 0.0f) == 0.0f);
	CHECK(shapeTransfer(phiTransfer(0.0f), 0.0f) == 0.0f);
	CHECK(shapeTransfer(phiTransfer(kPhiOctavesMax), 0.0f) == 0.0f);
	// CLAMPED like every other curve.
	CHECK(shapeTransfer(phiTransfer(3.0f), 5.0f) == shapeTransfer(phiTransfer(3.0f), 1.0f));
}

TEST_CASE("PY: Log and Exp at every exponent step are the curve restated — 1 − (1 − |x|)^k and |x|^k, k = 2·2^(step/16), sign kept") {
	using namespace feathers::device;
	for (const Transfer kind : { Transfer::Log, Transfer::Exp }) {
		for (int step = kExpStepMin; step <= kExpStepMax; step += 4) {
			const Transfer t = transferWithStep(kind, step);
			const double k = 2.0 * std::pow(2.0, step / 16.0);
			for (const double x : { 0.05, 0.3, 0.5, 0.71, 0.95, 1.0 }) {
				const double m = x;
				const double want = kind == Transfer::Log ? 1.0 - std::pow(1.0 - m, k) : std::pow(m, k);
				CAPTURE(static_cast<int>(kind));
				CAPTURE(step);
				CAPTURE(x);
				CHECK(std::abs(static_cast<double>(shapeTransfer(t, static_cast<float>(x))) - want) <= 1e-5);
				CHECK(std::abs(static_cast<double>(shapeTransfer(t, static_cast<float>(-x))) + want) <= 1e-5);
			}
			CHECK(shapeTransfer(t, 0.0f) == 0.0f);
		}
	}
	// EXPONENT 1 IS A LINE, both ways; and the default step keeps the old square to the bit.
	CHECK(std::abs(shapeTransfer(transferWithStep(Transfer::Exp, kExpStepMin), 0.37f) - 0.37f) <= 1e-6f);
	CHECK(std::abs(shapeTransfer(transferWithStep(Transfer::Log, kExpStepMin), 0.37f) - 0.37f) <= 1e-6f);
	CHECK(shapeTransfer(Transfer::Exp, 0.3f) == 0.3f * 0.3f);
}

TEST_CASE("PY: the φ transfer is continuous and stays inside 0..1 — no jump past the slope bound per small step") {
	using namespace feathers::device;
	for (const float octaves : { 0.0f, 2.0f, 4.0f, 6.0f, 7.0f }) {
		const Transfer t = phiTransfer(octaves);
		const double bound = phiSlopeBound(static_cast<double>(phiPeriods(t)));
		constexpr int kN = 100000;
		const double du = 1.0 / kN;
		double worst = 0.0;
		float previous = shapeTransfer(t, 0.0f);
		float lo = previous;
		float hi = previous;
		for (int i = 1; i <= kN; ++i) {
			const float y = shapeTransfer(t, static_cast<float>(i * du));
			worst = std::max(worst, std::abs(static_cast<double>(y - previous)));
			lo = std::min(lo, y);
			hi = std::max(hi, y);
			previous = y;
		}
		CAPTURE(octaves);
		MESSAGE("octaves " << octaves << ": worst step " << worst << " against " << bound * du << "; range " << lo
		                   << ".." << hi);
		// CONTINUOUS: no step larger than the steepest slope there is allows — 1% over it for the float input's own
		// rounding (6e-8 at u ≈ 1, times a slope of up to ~480) — when a jump would be tenths.
		CHECK(worst <= bound * du * 1.01 + 1e-6);
		CHECK(lo >= 0.0f);
		CHECK(hi <= 1.0f);
		// AND IT USES THE RANGE — the reason for a warped triangle over a plain sum, which huddles round ½.
		CHECK(hi >= 0.98f);
		CHECK(lo <= 0.02f);
	}
}

TEST_CASE("PY: the φ transfer's period count is its periods — P peaks within the warp's swing, the phase never reversing") {
	// THE COUNT: peaks — local maxima above 0.98 — over the input's 0..1. The phase only ever runs forward (the
	// warp's slope stays under the carrier's), so each pass through a half-integer is exactly one peak, and the
	// phase ends at P + warp(1), the warp within ±0.32: the count is P within ±(0.32 + 1).
	using namespace feathers::device;
	const auto peaks = [](Transfer t) {
		constexpr int kN = 400000;
		int count = 0;
		float a = shapeTransfer(t, 0.0f);
		float b = shapeTransfer(t, 1.0f / kN);
		for (int i = 2; i <= kN; ++i) {
			const float c = shapeTransfer(t, static_cast<float>(i) / kN);
			if (b > 0.98f && b >= a && b > c) {
				++count;
			}
			a = b;
			b = c;
		}
		return count;
	};
	CHECK(phiPhaseRateMax() - 1.0 < 1.0); // the premise: the warp never outruns the carrier
	int previous = 0;
	for (const float octaves : { 0.0f, 1.0f, 3.0f, 4.0f, 5.0f, 6.0f, 6.5f, 7.0f }) {
		const Transfer t = phiTransfer(octaves);
		const double periods = static_cast<double>(phiPeriods(t));
		const int n = peaks(t);
		CAPTURE(octaves);
		MESSAGE(periods << " periods: " << n << " peaks");
		CHECK(n >= static_cast<int>(std::floor(periods - 0.32)));
		CHECK(n <= static_cast<int>(std::ceil(periods + 0.32)));
		CHECK(n > previous); // more periods, more peaks
		previous = n;
	}
	// ONE PERIOD IS A SHAPE — one peak; 128 IS A LOOKUP — 128 of them, give or take the end.
	CHECK(peaks(phiTransfer(0.0f)) == 1);
	CHECK(peaks(phiTransfer(kPhiOctavesMax)) >= 127);
	// AND THE PERIODS ARE UNEQUAL — what makes it a lookup and not a triangle wave: at 16 periods the longest
	// rise-and-fall is more than twice the shortest. Measured between successive zeros (minima under 0.02).
	{
		const Transfer t = phiTransfer(4.0f);
		constexpr int kN = 400000;
		std::vector<double> zeros;
		float a = shapeTransfer(t, 0.0f);
		float b = shapeTransfer(t, 1.0f / kN);
		zeros.push_back(0.0);
		for (int i = 2; i <= kN; ++i) {
			const float c = shapeTransfer(t, static_cast<float>(i) / kN);
			if (b < 0.02f && b <= a && b < c) {
				zeros.push_back(static_cast<double>(i - 1) / kN);
			}
			a = b;
			b = c;
		}
		REQUIRE(zeros.size() >= 10);
		double shortest = 1.0;
		double longest = 0.0;
		for (size_t i = 1; i < zeros.size(); ++i) {
			shortest = std::min(shortest, zeros[i] - zeros[i - 1]);
			longest = std::max(longest, zeros[i] - zeros[i - 1]);
		}
		MESSAGE("16 periods: shortest " << shortest << ", longest " << longest);
		CHECK(longest > 2.0 * shortest);
	}
}

TEST_CASE("PY: every coefficient step of every kind is a valid curve whose label is its kind's and whose coefficient round-trips; nothing else is valid") {
	using namespace feathers::device;
	size_t valid = 0;
	for (const Transfer kind : { Transfer::Log, Transfer::Exp, Transfer::Phi }) {
		const auto [lo, hi] = transferStepRange(kind);
		CHECK(lo < hi);
		for (int step = lo; step <= hi; ++step) {
			const Transfer t = transferWithStep(kind, step);
			CAPTURE(static_cast<int>(kind));
			CAPTURE(step);
			REQUIRE(transferValid(t));
			CHECK(transferKind(t) == kind);
			CHECK(transferStep(t) == step);
			CHECK(std::string(transferLabel(t)) == kTransferLabels[static_cast<size_t>(kind)]);
			CHECK(std::string(transferName(t)) == kTransferNames[static_cast<size_t>(kind)]);
			// THE FILE'S ROAD: the label gives the kind at its default, the coefficient puts the step back.
			const auto back = transferFromLabel(transferLabel(t), std::strlen(transferLabel(t)));
			REQUIRE(back.has_value());
			CHECK(*back == transferDefaultOf(kind));
			CHECK(transferWithCoefficient(*back, transferCoefficient(t)) == t);
			CHECK_FALSE(transferNeedsBipolar(t));
			CHECK(hasCoefficient(t));
			++valid;
		}
	}
	// THE KINDS WITH NO COEFFICIENT ARE ONE CODE EACH, and the default Log and Exp ARE the old enumerators.
	for (const Transfer t : { Transfer::Lin, Transfer::Abs, Transfer::Pos, Transfer::Neg }) {
		CHECK_FALSE(hasCoefficient(t));
		CHECK(transferWithStep(t, 9) == t);
	}
	CHECK(transferDefaultOf(Transfer::Log) == Transfer::Log);
	CHECK(transferDefaultOf(Transfer::Exp) == Transfer::Exp);
	CHECK(transferExponent(Transfer::Exp) == 2.0f);
	CHECK(transferExponent(transferWithStep(Transfer::Exp, kExpStepMin)) == 1.0f);
	CHECK(transferExponent(transferWithStep(Transfer::Exp, kExpStepMax)) == 8.0f);
	CHECK(exponentTransfer(Transfer::Log, 4.0f) == transferWithStep(Transfer::Log, 16));
	// CLAMPED AT BOTH ENDS, and a NaN coefficient is the default.
	CHECK(phiTransfer(-3.0f) == phiTransferAtStep(0));
	CHECK(phiTransfer(99.0f) == phiTransferAtStep(kPhiSteps));
	CHECK(phiTransfer(std::nanf("")) == phiTransfer(kPhiOctavesDefault));
	CHECK(phiPeriods(phiTransfer(kPhiOctavesDefault)) == 16.0f);
	CHECK(exponentTransfer(Transfer::Exp, 0.1f) == transferWithStep(Transfer::Exp, kExpStepMin));
	CHECK(exponentTransfer(Transfer::Exp, std::nanf("")) == Transfer::Exp);
	// EVERY OTHER 16-BIT CODE IS NOT A CURVE: count the valid ones over the whole space.
	size_t everyValid = 0;
	for (uint32_t c = 0; c <= 0xffffu; ++c) {
		everyValid += transferValid(static_cast<Transfer>(c)) ? 1u : 0u;
	}
	CHECK(everyValid == valid + 4u); // the three kinds' steps, and Lin, Abs, Pos, Neg
	CHECK(shapeTransfer(static_cast<Transfer>(0x00ffu), 0.5f) == 0.0f); // not a curve: silence
	// THE RING'S REACH IS LIN'S: a φ curve peaks inside the range.
	ModDisplay d;
	d.bipolar = true;
	d.transfer = phiTransfer(4.0f);
	CHECK(d.reachLo() == -1.0f);
	CHECK(d.reachHi() == 1.0f);
	d.bipolar = false;
	CHECK(d.reachLo() == 0.0f);
	// AND THE READOUT'S PEAK READING IS WHERE IT PEAKS.
	CHECK(phiTriangleCurve(phiPeakReading(phiTransfer(5.0f)), 32.0f) >= 0.995f);
}

TEST_CASE("transfer chips: a core destination reads its source through the curve — the block resolve, each voice's offset, and the per-sample windows") {
	ParamSet params;
	const auto fine = static_cast<clap_id>(ParamId::FineTune);
	const ParamDesc* desc = findParam(fine);
	REQUIRE(desc != nullptr);
	const double span = desc->max - desc->min;
	params.setBase(fine, 0.0);
	constexpr float kAmount = 0.25f;
	TransferCells cells;
	feathers::test::Routes routes;
	const auto dest = feathers::test::param(ParamId::FineTune);

	SUBCASE("the block resolve and each voice's offset") {
		const auto src = feathers::backplane::Source::output(7, 0);
		routes.set(src, dest, kAmount);
		cells.cell = 2.0f; // past the range: Lin carries it, every curve clamps it
		cells.perVoice = { -0.5f, 0.75f };
		for (const Transfer t : kEveryTransfer) {
			CAPTURE(static_cast<int>(t));
			REQUIRE(routes.setTransfer(src, dest, t));
			routes.publish(params, cells.cells());
			const double block = std::clamp(kAmount * restatedCurve(t, 2.0) * span, desc->min, desc->max);
			CHECK(params.resolved(fine) == doctest::Approx(block));
			CHECK(params.perVoiceOffset(ParamId::FineTune, 0) == doctest::Approx(kAmount * restatedCurve(t, -0.5) * span));
			CHECK(params.perVoiceOffset(ParamId::FineTune, 1) == doctest::Approx(kAmount * restatedCurve(t, 0.75) * span));
		}
		// NOT VACUOUS: Lin reaches past Exp's clamp, and the rectifiers part ways on voice 0.
		CHECK(restatedCurve(Transfer::Lin, 2.0) != restatedCurve(Transfer::Exp, 2.0));
		CHECK(restatedCurve(Transfer::Abs, -0.5) != restatedCurve(Transfer::Neg, -0.5));

		// LIN, TO THE BIT: the old expression verbatim — `amount × cell × span` in double.
		REQUIRE(routes.setTransfer(src, dest, Transfer::Lin));
		cells.cell = 0.3f;
		routes.publish(params, cells.cells());
		const double old = static_cast<double>(kAmount) * static_cast<double>(cells.perVoice[0]) * span;
		CHECK(params.perVoiceOffset(ParamId::FineTune, 0) == old);
	}

	SUBCASE("the per-sample windows: a voice's own, and a block-rate one") {
		const auto voiced = feathers::backplane::Source::output(7, 0);
		const auto block = feathers::backplane::Source::output(8, 0);
		const auto stride = static_cast<size_t>(feathers::device::kTickWindowStride);
		feathers::device::fillTickWindowRamp(cells.windows.data(), -1.0f, 1.0f);          // voice 0
		feathers::device::fillTickWindowRamp(cells.windows.data() + stride, 0.5f, 0.5f);  // voice 1, flat
		feathers::device::fillTickWindowRamp(cells.block.data(), 1.0f, -1.0f);
		for (const Transfer t : kEveryTransfer) {
			CAPTURE(static_cast<int>(t));
			routes = {};
			routes.set(voiced, dest, kAmount);
			REQUIRE(routes.setTransfer(voiced, dest, t));
			routes.publish(params, cells.cells());
			std::array<float, feathers::device::kTickWindow> out {};
			REQUIRE(params.resolveWindow(ParamId::FineTune, 0, 0, feathers::device::kTickWindow, out.data()));
			for (int i = 0; i < feathers::device::kTickWindow; i += 7) {
				CAPTURE(i);
				CHECK(out[static_cast<size_t>(i)]
				      == doctest::Approx(kAmount * restatedCurve(t, cells.windows[static_cast<size_t>(i)]) * span).epsilon(1e-5));
			}
			// THE BUS ROAD: a block-rate window, the same run for every voice (voice -1 asks for it alone).
			routes = {};
			routes.set(block, dest, kAmount);
			REQUIRE(routes.setTransfer(block, dest, t));
			routes.publish(params, cells.cells());
			REQUIRE(params.resolveWindow(ParamId::FineTune, -1, 0, feathers::device::kTickWindow, out.data()));
			for (int i = 0; i < feathers::device::kTickWindow; i += 7) {
				CAPTURE(i);
				CHECK(out[static_cast<size_t>(i)]
				      == doctest::Approx(kAmount * restatedCurve(t, cells.block[static_cast<size_t>(i)]) * span).epsilon(1e-5));
			}
		}
	}
}

TEST_CASE("transfer chips: a ring's reach is the half the curve can reach, and Lin draws the ring it always drew") {
	using feathers::device::ModDisplay;
	const auto reach = [](bool bipolar, Transfer t) {
		ModDisplay d;
		d.bipolar = bipolar;
		feathers::device::applyTransferToDisplay(d, t);
		return std::pair<float, float> { d.reachLo(), d.reachHi() };
	};
	CHECK(reach(true, Transfer::Lin) == std::pair<float, float> { -1.0f, 1.0f });
	CHECK(reach(false, Transfer::Lin) == std::pair<float, float> { 0.0f, 1.0f });
	CHECK(reach(true, Transfer::Log) == std::pair<float, float> { -1.0f, 1.0f });
	CHECK(reach(true, Transfer::Exp) == std::pair<float, float> { -1.0f, 1.0f });
	CHECK(reach(true, Transfer::Abs) == std::pair<float, float> { 0.0f, 1.0f });
	CHECK(reach(true, Transfer::Pos) == std::pair<float, float> { 0.0f, 1.0f });
	CHECK(reach(true, Transfer::Neg) == std::pair<float, float> { -1.0f, 0.0f });
	CHECK(reach(false, Transfer::Neg) == std::pair<float, float> { 0.0f, 0.0f });
	// A RECTIFIED BIPOLAR SOURCE READS AS A UNIPOLAR ONE to a widget that knows only `bipolar`.
	ModDisplay d;
	d.bipolar = true;
	feathers::device::applyTransferToDisplay(d, Transfer::Abs);
	CHECK_FALSE(d.bipolar);
}

TEST_CASE("the Additive Shape's labels are partials::ShapeKind's, entry by entry") {
	// The menu's index IS the ShapeKind the bank builds (character_params.h casts it), so a label out of order would
	// name one shape and play another. Written out, not derived from either table.
	using feathers::partials::ShapeKind;
	REQUIRE(feathers::kAdditiveShapeNames.size() == static_cast<size_t>(ShapeKind::kCount));
	CHECK(std::string(feathers::kAdditiveShapeNames[static_cast<size_t>(ShapeKind::Sine)]) == "Sine");
	CHECK(std::string(feathers::kAdditiveShapeNames[static_cast<size_t>(ShapeKind::Triangle)]) == "Triangle");
	CHECK(std::string(feathers::kAdditiveShapeNames[static_cast<size_t>(ShapeKind::Saw)]) == "Saw");
	CHECK(std::string(feathers::kAdditiveShapeNames[static_cast<size_t>(ShapeKind::Square)]) == "Square");
	// And the param's default is the Saw the design names.
	const feathers::ParamDesc* d = feathers::findParam(static_cast<clap_id>(ParamId::AdditiveShape));
	REQUIRE(d != nullptr);
	CHECK(d->def == static_cast<double>(ShapeKind::Saw));
}

TEST_CASE("the spectral drawer's params: one block per sound, unkeyed, the continuous ones in each slot's destination run, the S ones stereo") {
	// THE ID LAYOUT, written out: 0x0400 + slot·0x20 + the control's index; thirty-one controls per sound (the v1
	// operators' nineteen, then the vowel Formant's three at +0x13..+0x15, Partials' two at +0x16..+0x17, Damp's three at
	// +0x18..+0x1A, Stretch's two at +0x1B..+0x1C, Shift's two at +0x1D..+0x1E).
	// F2 (2026-10-19): nine more per sound in a SECOND block, 0x0500 + slot·0x10 + (the control's index − 31), so no id of the
	// first block moved; and KEYED, as every core row added since JF declares its key ("a.spectral.filter2.on" …).
	using feathers::SpectralControl;
	// OE (2026-10-26): six more per sound in a THIRD block, 0x0580 + slot·0x10 + (the control's index − 43), keyed likewise.
	// OE2 (2026-10-27): five more in the third block (+6..+10): Bandwidth, the Spacing and Widths amounts and Free coordinates.
	REQUIRE(feathers::kSpectralControlCount == 54);
	REQUIRE(feathers::kSpectralThirdBlockFirst == 43);
	REQUIRE(feathers::kSpectralFirstBlockCount == 31);
	for (size_t slot = 0; slot < feathers::kMaxSounds; ++slot) {
		for (size_t c = 0; c < feathers::kSpectralControlCount; ++c) {
			const auto control = static_cast<SpectralControl>(c);
			const ParamId id = feathers::spectralParam(slot, control);
			CAPTURE(slot);
			CAPTURE(c);
			const bool f2 = c >= 31;
			const bool oe = c >= 43;
			CHECK(static_cast<uint32_t>(id)
			      == (oe   ? 0x0580u + 0x10u * static_cast<uint32_t>(slot) + static_cast<uint32_t>(c - 43)
			          : f2 ? 0x0500u + 0x10u * static_cast<uint32_t>(slot) + static_cast<uint32_t>(c - 31)
			             : 0x0400u + 0x20u * static_cast<uint32_t>(slot) + static_cast<uint32_t>(c)));
			const feathers::SpectralAddress at = feathers::spectralAddressOf(id);
			CHECK(at.slot == slot);
			CHECK(at.control == control);
			const feathers::ParamDesc* desc = feathers::findParam(static_cast<clap_id>(id));
			REQUIRE(desc != nullptr);
			// UNKEYED, as every per-sound row is until JF step 2 (lead's call, 2026-10-10) — but F2's, which carry their sound's
			// letter: "a.spectral.…".
			if (f2) {
				REQUIRE(desc->key != nullptr);
				CHECK(std::string(desc->key).rfind(std::string(1, static_cast<char>('a' + slot)) + ".spectral.", 0) == 0);
			}
			else {
				CHECK(desc->key == nullptr);
			}
			CHECK(std::string(desc->name).rfind(std::string(1, static_cast<char>('A' + slot)) + " Spectral", 0) == 0);
			// A DESTINATION IFF CONTINUOUS, and in ITS OWN SLOT'S run; stereo IFF marked S.
			const bool stepped = (desc->flags & CLAP_PARAM_IS_STEPPED) != 0;
			const int destination = feathers::modDestinationIndexOf(id);
			CHECK((destination >= 0) == !stepped);
			if (destination >= 0) {
				const size_t run = feathers::kCoreModDestinations.size() + slot * feathers::kPerSoundDestinationsPerSlot;
				CHECK(static_cast<size_t>(destination) >= run);
				CHECK(static_cast<size_t>(destination) < run + feathers::kPerSoundDestinationsPerSlot);
			}
			CHECK(desc->stereo == feathers::spectralStereo(control));
			// EVERY SWITCH IS OFF BY DEFAULT except Link, so a fresh sound's drawer is neutral.
			if (desc->names.data() == feathers::kEnableNames.data()) {
				CHECK(desc->def == (control == SpectralControl::FormantShiftLink ? 1.0 : 0.0));
			}
		}
	}
	// NOTHING ELSE IS A SPECTRAL ID: the gaps in each block and the ids around it address nothing.
	CHECK(feathers::spectralAddressOf(static_cast<ParamId>(0x0400u + 31u)).slot == feathers::kMaxSounds);
	CHECK(feathers::spectralAddressOf(static_cast<ParamId>(0x03FFu)).slot == feathers::kMaxSounds);
	CHECK(feathers::spectralAddressOf(static_cast<ParamId>(0x0500u + 12u)).slot == feathers::kMaxSounds);
	CHECK(feathers::spectralAddressOf(static_cast<ParamId>(0x0580u + 11u)).slot == feathers::kMaxSounds);
	CHECK(feathers::spectralAddressOf(static_cast<ParamId>(0x0600u)).slot == feathers::kMaxSounds);
	// THE DRUM PADS TAKE 0x0600..0x06FF and 0x0700..0x07FF (2026-11-05/06; the second block 32 a slot since S11), so the span is 0x0800.
	CHECK(feathers::kParamIdSpan == 0x0800u);
	// THE MODE'S LABELS, in spectral::FilterMode's order (Synth::drawerFor casts the index).
	CHECK(std::string(feathers::kSpectralFilterModeNames[static_cast<size_t>(feathers::spectral::FilterMode::LowPass)]) == "Low-pass");
	CHECK(std::string(feathers::kSpectralFilterModeNames[static_cast<size_t>(feathers::spectral::FilterMode::HighPass)]) == "High-pass");
	CHECK(std::string(feathers::kSpectralFilterModeNames[static_cast<size_t>(feathers::spectral::FilterMode::BandPass)]) == "Band-pass");
	CHECK(std::string(feathers::kSpectralFilterModeNames[static_cast<size_t>(feathers::spectral::FilterMode::Notch)]) == "Notch");
	CHECK(std::string(feathers::kSpectralFilterModeNames[static_cast<size_t>(feathers::spectral::FilterMode::Bell)]) == "Bell");
	CHECK(feathers::kSpectralFilterModeNames.size() == static_cast<size_t>(feathers::spectral::kFilterModeCount));
	// F2'S DEFAULTS, so a sound that never touches them sounds as it did: filter 2 off, Series, every Gain 0 dB.
	CHECK(feathers::findParam(static_cast<clap_id>(feathers::spectralParam(0, SpectralControl::Filter2On)))->def == 0.0);
	CHECK(feathers::findParam(static_cast<clap_id>(feathers::spectralParam(0, SpectralControl::FilterRouting)))->def == 0.0);
	CHECK(feathers::findParam(static_cast<clap_id>(feathers::spectralParam(0, SpectralControl::FilterGain)))->def == 0.0);
	CHECK(feathers::findParam(static_cast<clap_id>(feathers::spectralParam(0, SpectralControl::Filter2Gain)))->def == 0.0);
	CHECK(feathers::findParam(static_cast<clap_id>(feathers::spectralParam(0, SpectralControl::Filter2Cutoff)))->def
	      == doctest::Approx(std::log2(500.0)).epsilon(1e-15));
	// OH: the count's floor is 1 (it was 8, the drawer's MINIMUM; its default stays 64), and the scheme defaults to Harmonics
	// (the operator as it was), its labels the Spectral device's, in spectral::PartialsScheme's order.
	CHECK(feathers::findParam(static_cast<clap_id>(feathers::spectralParam(0, SpectralControl::PartialsCount)))->min == 1.0);
	CHECK(feathers::findParam(static_cast<clap_id>(feathers::spectralParam(0, SpectralControl::PartialsCount)))->def == 64.0);
	CHECK(feathers::findParam(static_cast<clap_id>(feathers::spectralParam(0, SpectralControl::PartialsMode)))->def
	      == static_cast<double>(feathers::spectral::PartialsScheme::Harmonics));
	for (size_t i = 0; i < feathers::kSpectralPartialsModeNames.size(); ++i) {
		CHECK(std::string(feathers::kSpectralPartialsModeNames[i]) == feathers::fx::spectral::kPartialsModeNames[i]);
	}
	// OE2: THE ENGINE VOCODER'S ZONES AND STEREO wear the Spectral effect's labels, in its order (the layout casts the index).
	REQUIRE(feathers::kAdditiveVocodeSpacingNames.size() == std::size(feathers::fx::spectral::kZoneNames));
	for (size_t i = 0; i < feathers::kAdditiveVocodeSpacingNames.size(); ++i) {
		CHECK(std::string(feathers::kAdditiveVocodeSpacingNames[i]) == feathers::fx::spectral::kZoneNames[i]);
	}
	REQUIRE(feathers::kAdditiveVocodeWidthsNames.size() == std::size(feathers::fx::spectral::kWidthZoneNames));
	for (size_t i = 0; i < feathers::kAdditiveVocodeWidthsNames.size(); ++i) {
		CHECK(std::string(feathers::kAdditiveVocodeWidthsNames[i]) == feathers::fx::spectral::kWidthZoneNames[i]);
	}
	for (size_t i = 0; i < feathers::kAdditiveVocodeStereoNames.size(); ++i) {
		CHECK(std::string(feathers::kAdditiveVocodeStereoNames[i]) == feathers::fx::spectral::kVocodeStereoNames[i]);
	}
	CHECK(std::string(feathers::kSpectralPartialsModeNames[static_cast<size_t>(feathers::spectral::PartialsScheme::Peaks)]) == "Peaks");
	// THE LOG2 LITERALS params.cpp writes (std::log2 is not constexpr), against std::log2.
	const auto range = [](SpectralControl c) {
		return feathers::findParam(static_cast<clap_id>(feathers::spectralParam(0, c)));
	};
	CHECK(range(SpectralControl::FilterCutoff)->min == doctest::Approx(std::log2(20.0)).epsilon(1e-15));
	CHECK(range(SpectralControl::FilterCutoff)->max == doctest::Approx(std::log2(20000.0)).epsilon(1e-15));
	CHECK(range(SpectralControl::FilterCutoff)->def == doctest::Approx(std::log2(2000.0)).epsilon(1e-15));
	CHECK(range(SpectralControl::FormantShiftCrossover)->min == doctest::Approx(std::log2(100.0)).epsilon(1e-15));
	CHECK(range(SpectralControl::FormantShiftCrossover)->max == doctest::Approx(std::log2(8000.0)).epsilon(1e-15));
	CHECK(range(SpectralControl::FormantShiftCrossover)->def == doctest::Approx(std::log2(1000.0)).epsilon(1e-15));
	CHECK(range(SpectralControl::DampTime)->min == doctest::Approx(std::log2(0.05)).epsilon(1e-15));
	CHECK(range(SpectralControl::DampTime)->max == doctest::Approx(std::log2(20.0)).epsilon(1e-15));
	CHECK(range(SpectralControl::DampTime)->def == std::log2(2.0));
	// AND A SOUND'S KNOBS (what a clone copies) cover every one of its controls.
	for (size_t c = 0; c < feathers::kSpectralControlCount; ++c) {
		const ParamId id = feathers::spectralParam(3, static_cast<SpectralControl>(c));
		bool found = false;
		for (size_t k = 0; k < feathers::kPerSoundKnobCount; ++k) {
			found = found || feathers::perSoundKnobParam(3, k) == id;
		}
		CHECK(found);
	}
}

TEST_CASE("a sound's knob list is pinned: perSoundKnobParam's order is the stance wire's and the clone's, by param id") {
	// THE ORDER IS ON THE WIRE (RideAlongValues::knobs, written in this order into every stance) — so a knob inserted mid-list
	// would hand every saved stance's later knobs to the wrong control. Appending is the only safe edit; this case is what
	// says so out loud, by id, for every slot.
	// THE NO-COMPAT WAIVER WAS TAKEN ONCE (spectral-ops, 2026-10-12; under the policy that patch compatibility is not a
	// constraint yet): the later spectral operators' controls landed mid-list, the continuous ones before the stepped
	// run (each slot's kModDestinations run grew 28 -> 35), so a stance saved before that reads its later knobs shifted.
	// With saved patches to protect, that would have been an append.
	using feathers::SpectralControl;
	for (size_t slot = 0; slot < feathers::kMaxSounds; ++slot) {
		CAPTURE(slot);
		std::vector<ParamId> want {
			feathers::fmDepthParam(slot),         feathers::smoothnessParam(slot),     feathers::quantizeParam(slot),
			feathers::syncParam(slot),            feathers::feedbackParam(slot),       feathers::pointsParam(slot),
			feathers::gritParam(slot),            feathers::warpParam(slot),           feathers::shapeAmountParam(slot),
			feathers::additiveRolloffParam(slot), feathers::additiveFormantParam(slot), feathers::additiveOddEvenParam(slot),
			feathers::additiveCombParam(slot),    feathers::additiveDetailParam(slot), feathers::additiveOffGridParam(slot),
			// PLUCK'S FIVE FIELD SCALERS (2026-10-31), mid-list under the same waiver: no saved stance to protect.
			feathers::ksBrightnessParam(slot), feathers::ksSustainParam(slot), feathers::ksPickParam(slot),
			feathers::ksNoiseParam(slot), feathers::ksStretchParam(slot),
		};
		for (const SpectralControl c : feathers::kSpectralContinuousControls) {
			want.push_back(feathers::spectralParam(slot, c));
		}
		// THE DRUM PAD'S AMOUNTS (2026-11-05), in each slot's destination run after the spectral drawer's: mid-list, the same waiver.
		for (const feathers::DrumControl c : feathers::kDrumContinuousControls) {
			want.push_back(feathers::drumParam(slot, c));
		}
		for (const SpectralControl c : feathers::kSpectralSteppedControls) {
			want.push_back(feathers::spectralParam(slot, c));
		}
		// AND ITS STEPPED CONTROLS, appended after the spectral drawer's.
		for (const feathers::DrumControl c : feathers::kDrumSteppedControls) {
			want.push_back(feathers::drumParam(slot, c));
		}
		REQUIRE(want.size() == feathers::kPerSoundKnobCount);
		for (size_t k = 0; k < want.size(); ++k) {
			CAPTURE(k);
			CHECK(feathers::perSoundKnobParam(slot, k) == want[k]);
		}
	}
	// AND THE SPECTRAL ORDERS THE LIST IS BUILT FROM, by control.
	CHECK(feathers::kSpectralContinuousControls
	      == std::array<SpectralControl, 37> { SpectralControl::FilterCutoff, SpectralControl::FilterResonance,
	                                           SpectralControl::FilterSlope, SpectralControl::FilterKeyTrack,
	                                           SpectralControl::CombSpacing, SpectralControl::CombDepth,
	                                           SpectralControl::CombPhase, SpectralControl::FormantShiftLow,
	                                           SpectralControl::FormantShiftHigh, SpectralControl::FormantShiftCrossover,
	                                           SpectralControl::TiltAmount, SpectralControl::OddEvenBalance,
	                                           SpectralControl::FormantVowel, SpectralControl::FormantDepth,
	                                           SpectralControl::PartialsCount, SpectralControl::DampTime,
	                                           SpectralControl::DampTilt, SpectralControl::StretchAmount,
	                                           SpectralControl::ShiftHz, SpectralControl::FilterGain,
	                                           SpectralControl::Filter2Cutoff, SpectralControl::Filter2Resonance,
	                                           SpectralControl::Filter2Slope, SpectralControl::Filter2KeyTrack,
	                                           SpectralControl::Filter2Gain, SpectralControl::PartialsSet,
	                                           SpectralControl::PartialsDepth, SpectralControl::VocodeDepth,
	                                           SpectralControl::VocodeFlatten, SpectralControl::VocodeGate,
	                                           SpectralControl::VocodeLow, SpectralControl::VocodeHigh,
	                                           SpectralControl::VocodeBandwidth, SpectralControl::VocodeSpacingAmount,
	                                           SpectralControl::VocodeSpacingFree, SpectralControl::VocodeWidthsAmount,
	                                           SpectralControl::VocodeWidthsFree });
	CHECK(feathers::kSpectralSteppedControls
	      == std::array<SpectralControl, 17> { SpectralControl::FilterOn, SpectralControl::FilterMode,
	                                           SpectralControl::CombOn, SpectralControl::FormantShiftOn,
	                                           SpectralControl::FormantShiftLink, SpectralControl::TiltOn,
	                                           SpectralControl::OddEvenOn, SpectralControl::FormantOn,
	                                           SpectralControl::PartialsOn, SpectralControl::DampOn,
	                                           SpectralControl::StretchOn, SpectralControl::ShiftOn,
	                                           SpectralControl::Filter2On, SpectralControl::Filter2Mode,
	                                           SpectralControl::FilterRouting, SpectralControl::PartialsMode,
	                                           SpectralControl::VocodeOn });
}
