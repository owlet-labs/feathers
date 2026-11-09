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
#include "test_heap.h"
#include "doctest.h"
#include "portable_aligned_alloc.h"

#include "dsp/character_bank.h"
#include "dsp/osc_voice.h"
#include "dsp/vox_bake.h"
#include "vox_pin_sweep.h"

#include <array>
#include <algorithm>
#include <set>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <type_traits>
#include <vector>

// VOX THROUGH THE VOICE — the half of the engine a position cannot answer, and the pin that says
// the rest of the terrain did not notice it arrive.
//
// The claims that carry the weight, in the order they matter:
//
//   - THE ZERO-PRESENCE IDENTITY, as a SWEEP rather than a point. Fifty-four positions the species
//     field calls vowel-free, rendered through the whole voice path, hashed — and the hashes below
//     were CAPTURED AT HEAD 1ba1529, the commit before the blend existed, by this same harness.
//     That is what makes it a before-and-after rather than a tautology: nothing in this file could
//     have produced these numbers from the code it is testing.
//   - THE BLEND IS BEFORE THE OPERATORS. Constructed so that blend-after would pass and the real
//     placement fails it — the operators are nonlinear, so the blended-then-operated cycle is NOT
//     the mix of the two separately-operated ones, and the case measures exactly that gap.
//   - AUDIBILITY AND MONOTONICITY: a vowel is heard, and heard more as the terrain grows one.
//   - MONO COLLAPSE: at width 0 both ears stay bit-identical through vowel country.

using namespace feathers;

namespace {

/// Every allocation this process makes, counted — the test_clap_render trick, brought to the DSP
/// binary because the vox road is the newest thing that could reach for the heap on the audio
/// thread. The per-voice bake takes a kilobyte of VoxCycle and a recipe copy on the STACK for
/// exactly this reason; the counter is what says so rather than the comment.
std::atomic<long> gAllocations { 0 };

} // namespace

// GCC MISREADS A CORRECT REPLACEMENT ALLOCATOR, 53 TIMES ACROSS TWO FILES.
//
// `operator new` here calls std::malloc/std::aligned_alloc and `operator delete` calls std::free,
// which is the pairing the standard requires of a replacement pair and is what makes the counting
// below possible at all. -Wmismatched-new-delete sees `free()` inside a function named
// `operator delete` and reports the pair it is looking at as mismatched — it reasons about the
// NAMES rather than about which allocator actually produced the pointer, and a replacement pair is
// exactly the case where those come apart.
//
// SUPPRESSED RATHER THAN WORKED AROUND, and scoped to these functions rather than to the file,
// because there is nothing to fix: any rewrite that silences it would break the pairing. It is
// suppressed AT ALL because 53 false positives are not untidiness — they were the noise floor that
// hid a real dangling-pointer warning in env_card.cpp for a day, in the fix for a bug that had been
// reported by name. A warning nobody reads is worse than no warning.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif
void* operator new(size_t size) {
	gAllocations.fetch_add(1, std::memory_order_relaxed);
	void* p = feathers::test::heapAlloc(size == 0 ? 1 : size);
	if (p == nullptr) {
		throw std::bad_alloc();
	}
	return p;
}

void* operator new[](size_t size) {
	return ::operator new(size);
}

void* operator new(size_t size, std::align_val_t alignment) {
	gAllocations.fetch_add(1, std::memory_order_relaxed);
	// An aligned request may ask for less than a pointer: see portable_aligned_alloc.h.
	void* p = feathers::test::portableAlignedAlloc(size, static_cast<size_t>(alignment));
	if (p == nullptr) {
		throw std::bad_alloc();
	}
	return p;
}

void* operator new[](size_t size, std::align_val_t alignment) {
	return ::operator new(size, alignment);
}

void operator delete(void* p) noexcept {
	feathers::test::heapFree(p);
}
void operator delete[](void* p) noexcept {
	feathers::test::heapFree(p);
}
void operator delete(void* p, size_t) noexcept {
	feathers::test::heapFree(p);
}
void operator delete[](void* p, size_t) noexcept {
	feathers::test::heapFree(p);
}
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
void operator delete(void* p, std::align_val_t) noexcept {
	feathers::test::heapFree(p);
}
void operator delete[](void* p, std::align_val_t) noexcept {
	feathers::test::heapFree(p);
}
void operator delete(void* p, size_t, std::align_val_t) noexcept {
	feathers::test::heapFree(p);
}
void operator delete[](void* p, size_t, std::align_val_t) noexcept {
	feathers::test::heapFree(p);
}
#include "nothrow_new_forwarding.h"

namespace {

struct Spot {
	uint16_t zone;
	float offset;
	double gamma;
	double travelX;
	double travelY;
	int sounds;
};

Character characterFor(const Spot& s) {
	Character c;
	c.engine = OscEngine::Morph;
	c.flagship = true;
	c.count = static_cast<uint8_t>(s.sounds);
	c.sounds[0].zone = s.zone;
	c.sounds[0].phaseOffset = s.offset;
	c.sounds[0].gammaBase = s.gamma;
	c.sounds[0].travelX = s.travelX;
	c.sounds[0].travelY = s.travelY;
	c.sounds[0].weight = s.sounds == 1 ? 1.0 : 0.6;
	if (s.sounds > 1) {
		c.sounds[1] = c.sounds[0];
		c.sounds[1].zone = static_cast<uint16_t>((s.zone + 137) % 1024);
		c.sounds[1].weight = 0.4;
	}
	return c;
}

/// The species field at one sound's coordinate, spelled out here rather than taken from buildVoxFor.
///
/// A SECOND, INDEPENDENT SPELLING ON PURPOSE. Selecting vowel-free country by asking the engine
/// would make the pin agree with itself by construction: a build that read the species at the wrong
/// coordinate would move the selection to match, and every hash would still be whatever that build
/// produced. Restating it here means the check stands still while the engine moves.
///
/// PINNED-ELSE-CHARACTER, which is the house spelling (flagship.cpp's blend loop) and NOT the
/// sound's own pin alone. The two agree whenever the character carries no global pin, which is
/// every case in this file — so the wrong one was silent here and would have diverged the first
/// time a character carried a global pin with an unpinned sound in it.
double presenceOfSound(const SliceOrientationPin& fallback, const SoundCoord& s) {
	const SliceOrientationPin& pin = s.orientation.pinned ? s.orientation : fallback;
	const double home = sliceHome(s.zone, s.phaseOffset, s.gammaBase);
	const MorphBasis morphFrame =
	    sliceFrameFor<kMorphTriangles>(rotateFor(pin), home, s.residue.morph);
	return static_cast<double>(voxPresenceAt(morphFrame.phaseFor(0, home, s.travelX, s.travelY)));
}

/// THE COORDINATE IS THE INDEPENDENT PART; THE ACCUMULATION DELIBERATELY IS NOT. The sum runs in
/// float and takes the weight through float, exactly as buildVoxFor's loop does, so the equality
/// this file asserts against the engine is EXACT rather than approximate. Restating it in double
/// would part from the engine in the last bit at every position with a real vowel, which would
/// leave the check reading `Approx` — and an approximate agreement between two spellings of a
/// coordinate is the thing it was written to rule out.
float presenceOf(const Character& c) {
	float total = 0.0f;
	for (size_t i = 0; i < c.count; ++i) {
		total += static_cast<float>(c.sounds[i].weight)
		         * static_cast<float>(presenceOfSound(c.orientation, c.sounds[i]));
	}
	return total;
}

constexpr int kRenderSamples = 512;
constexpr double kRate = 48000.0;

using Buffer = std::array<float, kRenderSamples>;

Buffer renderOf(const Character& c, double noteHz) {
	CharacterBank bank;
	bank.setSampleRate(kRate);
	bank.setCharacter(c);
	OscVoice voice;
	voice.setCharacterBank(&bank);
	voice.setSampleRate(kRate);
	voice.setFrequency(noteHz);
	Buffer out {};
	voice.render(out.data(), kRenderSamples);
	bank.advance(kRenderSamples);
	return out;
}


/// CAPTURED AT HEAD 1ba1529 — "the species field: how much vowel is in a place" — by running this
/// exact sweep and this exact render through a tree where the blend, the per-voice bake and
/// VoxCharacter did not exist at all. Two notes per spot, 55 Hz and 1200 Hz, so both ends of the
/// mip ladder are in the pin.
///
/// A regeneration of these numbers is a statement that the VOWEL-FREE terrain has deliberately
/// changed character, and it deserves the standing of a golden regen rather than a test fix.
/// CAPTURED AT e7f1690, and the revision matters more than it looks.
///
/// e7f1690 is the docs-only parent of the first vox commit — the LAST revision with no vox code in
/// any shared file. An earlier draft of this pin captured at 1ba1529, which is before the BLEND but
/// not before VOX, and that is a materially weaker claim: underneath it sit three vox commits, and
/// 4ca67fb among them did not merely append. It rewrote existing code in slice_basis.h —
/// planeResidueValue's ternary became a branching form, 2*kMorphTriangles was replaced by
/// kMorphResidueValues throughout, and PlaneResidue grew a third member, a struct carried inside
/// SoundCoord and inside Character, of which the bank holds four copies. None of that SHOULD move a
/// float. But "should not move a float" is exactly the class of claim this pin exists to test
/// rather than assume, and captured at 1ba1529 it could not see any of it.
///
/// The old build cannot run this harness — voxPresenceAt does not exist there — which is why the
/// spots are frozen literals rather than a search. With coordinates rather than a predicate, a
/// small standalone program at e7f1690 compiles vox_pin_sweep.h unchanged and renders the list.
///
/// REGENERATING THESE IS A STATEMENT that the vowel-free terrain has deliberately changed
/// character, and it deserves the standing of a golden regen rather than a test fix. Nothing about
/// vox may move them: a species retune changes which places HAVE vowels, and these places have
/// none.
// ALL THREE TABLES RE-CAPTURED 2026-08-22, on a DELIBERATE character change: every
// flagship cycle is now DC-removed and peak-normalised, which was already this engine's stated
// contract ("normalised so full scale is ±1.0") and was not true. 45 of 60 sampled cycles were
// outside a healthy band before it, worst DC +1.41 and worst peak 2.21, with several not crossing
// zero at all; after, 0 of 60.
//
// SO THESE PINS MOVED FOR A REASON THAT IS NOT DRIFT, and the reason is written here rather than in
// a commit message that outlives nothing. 165 of kSweepHashes' 228 entries changed, all 59 smooth
// and all 40 extra. What did NOT move is golden-character, which is the firmware-parity pin — this
// is a plugin-side change to the flagship, and the reference's own goldens are untouched by it.
//
// ALL THREE TABLES RE-CAPTURED AGAIN 2026-09-24, for the licence migration: the voice's 4x decimator
// (src/dsp/halfband.h) was replaced — a vendored GPL halfband out, our own polyphase-allpass halfband
// in. EVERY render in this file passes through the decimator, so every hash moved: 228 of 228 here,
// 59 of 59 in kSmoothHashes, 40 of 40 in kSmoothExtraHashes. That is the expected count, not a
// finding; a partial move would have been the finding.
//
// THE CHAIN OF EVIDENCE, since these can no longer be compared with e7f1690 directly. At 40ebf335,
// the parent of the swap, all 169 plain entries still matched their e7f1690 values (this case was
// green there). The swap changed no float code upstream of the decimator — the diff touches
// halfband.h/.cpp and the one line in OscVoice::beginStereoStream that copies the decimator — so
// "vowel-free terrain renders what it rendered before vox" now holds as: e7f1690 == 40ebf335
// through the old decimator, and this table == 40ebf335's pre-decimation stream through the new
// one. The two decimators were measured side by side (halfband.h's table): passband magnitude equal
// to better than 0.001 dB, images at or below -103.9 dB (old) and -107.9 dB (new), group delay 2.23
// vs 2.44 samples at DC. test_aliasing's figures are unchanged to the hundredth of a dB. The sample
// values move by the small phase difference between the two filters, which is what a hash sees.
//
// All 228 entries were taken from the new build, including the 59 smooth-country indices this table
// never checks (kSmoothHashes does); they now equal those, rather than being stale e7f1690 values.
constexpr uint64_t kSweepHashes[] = {
	14233800729161824517ull, 13908510010539884084ull, 3474281271465659457ull, 157251139022237164ull,
	18065705592069142306ull, 15261475023631204187ull, 4794900634043703387ull, 10951566124120445372ull,
	12656357445341487604ull, 5429335429097019131ull, 621997189685934721ull, 4006578105639519916ull,
	15564547277022226230ull, 16351987561365813507ull, 3591578800006734472ull, 4052703029366736027ull,
	16924873541693059210ull, 6481021635522448507ull, 6330724175256679157ull, 5891670214660910219ull,
	9455905278852504736ull, 9425364467810033979ull, 2114646906505176970ull, 16083434311395777388ull,
	12237977225738595077ull, 5314209994795418738ull, 494361971774837105ull, 7973006261257942307ull,
	5217947776039958865ull, 7707677957127789935ull, 2056661258117380272ull, 11391565481158861681ull,
	2619740130035546003ull, 15283956936632527285ull, 7968362067421308982ull, 1243961432052504549ull,
	11070684164873133880ull, 3076244508069721337ull, 1278785664908161438ull, 6931857543770247533ull,
	1829671932183992607ull, 12962807041097726639ull, 10551199560629799255ull, 168743061940671489ull,
	8051637973445128369ull, 6449578014970832304ull, 4578547330024796588ull, 14751711771674556559ull,
	8094949507218847563ull, 16672596330167753706ull, 851157288416781804ull, 2411068626977485195ull,
	15323670961291355366ull, 5341180807067392563ull, 11148016660787520799ull, 13230959864822036871ull,
	10072650081865703021ull, 13759015556496673420ull, 12669514799570760374ull, 5654112404879854151ull,
	9844827989989455259ull, 7792400463569193112ull, 3175203356109115914ull, 15888104398765427881ull,
	12711573606583532735ull, 1527926076418842589ull, 2727252327670091956ull, 14719559558876399268ull,
	3996517076346710910ull, 4982323929185901772ull, 13264113524316849299ull, 15880445007618604835ull,
	9462682271765187570ull, 9606298260057918290ull, 7599107914276530924ull, 5493121900121663836ull,
	14022122974499308877ull, 16344077082316899152ull, 12665128727221990041ull, 9060784252907218401ull,
	15032072546241466696ull, 17559613699695116831ull, 16874984883879895444ull, 12802549535110437954ull,
	9301924220438415693ull, 2655098100984645486ull, 7091262262253145354ull, 17877635863426735049ull,
	141017585954295306ull, 16782769875529354010ull, 17546463170279835213ull, 14625652316009327064ull,
	12577500401310432615ull, 15748219089448405307ull, 4894931927363038467ull, 2068274511727670211ull,
	11036460268507517916ull, 2941452002297732583ull, 1792583212219765511ull, 12993233278176146419ull,
	5146286487909249503ull, 4971238188752322233ull, 8241384818619294937ull, 7968295932389926124ull,
	8988135746058979798ull, 15987319407281402350ull, 95162690462995081ull, 15501676258887419937ull,
	18406985345193791844ull, 6834977080818875531ull, 3661816947177417953ull, 4124529585419760313ull,
	18002251700682555665ull, 11367084616826441149ull, 4491522790477142885ull, 8313044419412336621ull,
	9171869383919415959ull, 12326805508877772199ull, 5399214299154065331ull, 13211367318119324132ull,
	14695583003409337761ull, 500709268869855795ull, 6600324389833687165ull, 11249314269529090049ull,
	4366970595080745916ull, 10282650924745224134ull, 6188863963019359094ull, 4037927462810478896ull,
	315281893023196627ull, 9058039708149442302ull, 12798323894309464834ull, 9841140973089055173ull,
	403054457633711617ull, 11411040815669148341ull, 7449956242861995098ull, 3598462089681977709ull,
	4136908100005844192ull, 9338788025943492006ull, 1521024149138615809ull, 8057893701785401364ull,
	13652265490501878899ull, 14607176865037273941ull, 1217330270182558164ull, 17263558766951623005ull,
	14356397957200393277ull, 16534712852302910051ull, 6381827904169907610ull, 13809369832870477060ull,
	5163073214022622425ull, 18219071936068799466ull, 10675716503293591239ull, 7549948414977449010ull,
	7227674925247861580ull, 17685823617097830352ull, 7661366541958747699ull, 15985395935622579817ull,
	9938521015669973724ull, 12038260435642555473ull, 17881252543628529380ull, 4154160470220782441ull,
	17145288293987911381ull, 6631561927013796191ull, 6848698524636875012ull, 17323880300479164877ull,
	12940873615376867650ull, 11436202385222400138ull, 10900396583766599992ull, 12616630816615177043ull,
	4367100925652905337ull, 16212911772367480198ull, 11988014426078421887ull, 16799342330749993ull,
	2761100768599274809ull, 10177230862376317438ull, 6539548352788462222ull, 14856790410983557100ull,
	10574953765919522075ull, 12458987684066174005ull, 10978796664587638377ull, 10885507566127691750ull,
	16358558983839681507ull, 2431203884147230780ull, 1854081454120070973ull, 5539563816058627227ull,
	13789345597799316944ull, 6869739574998246889ull, 10189642565798321261ull, 12654234596302105245ull,
	11541042604225071339ull, 7170367229934116149ull, 18411532298604037381ull, 13251113733348859116ull,
	15482844057414469121ull, 11900108352301667050ull, 7201668154172881656ull, 6740281571474321221ull,
	11496537726459612887ull, 16723432525768077446ull, 11905457520120879078ull, 3239987514143673363ull,
	15347764570851566408ull, 11383550189325316012ull, 17560879272337210628ull, 1442378504006522343ull,
	16217576954690148311ull, 5284777584853368238ull, 8692452175698814375ull, 15266227425412283448ull,
	18150436443236378088ull, 6728191378069016618ull, 12580082109812152449ull, 5774947778644593282ull,
	6386525663377715321ull, 1515543323795631400ull, 6921476754134946611ull, 16067177190819792199ull,
	12526360089738236952ull, 15401530598547868070ull, 14500359111129471148ull, 10595835858657768136ull,
	12449067334409665368ull, 1159767079109730882ull, 1993398231607427843ull, 17111215651828344823ull,
	3547124477962123199ull, 17086059975583238505ull, 10091542051196891079ull, 17595549855287634335ull
};
/// THE SMOOTH ROAD'S OWN PIN — the 59 hashes for the fourteen spots the smoothness field reaches,
/// plus the single stereo ear each of three straddling pairs contributes.
///
/// A WEAKER CLAIM THAN kSweepHashes, DELIBERATELY, and the difference matters. Those numbers came
/// from e7f1690, a build that had never heard of a vowel, so they are evidence about the PAST.
/// These were recorded the day the smooth source landed, so they can only say that these positions
/// render deterministically from here on. Recording them is not a re-capture of the pin: not one
/// e7f1690 number was touched, and 169 of the 228 still stand exactly as they were.
///
/// THE PARTITION IS EXACT AND THAT WAS VERIFIED, not assumed. Every one of these 59 renders
/// genuinely differs from its e7f1690 hash, and every one of the other 169 is bit-identical to it.
/// Zero on either side of that line is in the wrong set — which is what makes the split a statement
/// about where the field reaches rather than a convenient place to draw it. (That verification was
/// made against the e7f1690 values; the partition itself is unchanged by the 2026-09-24 decimator
/// re-capture, which moved all 228 renders and so cannot speak to it either way.)
constexpr uint64_t kSmoothHashes[] = {
	12656357445341487604ull, 5429335429097019131ull, 621997189685934721ull, 4006578105639519916ull,
	1829671932183992607ull, 12962807041097726639ull, 10551199560629799255ull, 168743061940671489ull,
	8094949507218847563ull, 16672596330167753706ull, 851157288416781804ull, 2411068626977485195ull,
	12711573606583532735ull, 1527926076418842589ull, 2727252327670091956ull, 14719559558876399268ull,
	9462682271765187570ull, 9606298260057918290ull, 7599107914276530924ull, 5493121900121663836ull,
	12577500401310432615ull, 15748219089448405307ull, 4894931927363038467ull, 2068274511727670211ull,
	4366970595080745916ull, 10282650924745224134ull, 6188863963019359094ull, 4037927462810478896ull,
	315281893023196627ull, 9058039708149442302ull, 12798323894309464834ull, 9841140973089055173ull,
	13789345597799316944ull, 6869739574998246889ull, 10189642565798321261ull, 12654234596302105245ull,
	15482844057414469121ull, 11900108352301667050ull, 7201668154172881656ull, 6740281571474321221ull,
	11496537726459612887ull, 16723432525768077446ull, 11905457520120879078ull, 3239987514143673363ull,
	15347764570851566408ull, 11383550189325316012ull, 17560879272337210628ull, 1442378504006522343ull,
	18150436443236378088ull, 6728191378069016618ull, 12580082109812152449ull, 5774947778644593282ull,
	6386525663377715321ull, 1515543323795631400ull, 6921476754134946611ull, 16067177190819792199ull,
	12449067334409665368ull, 17086059975583238505ull, 17595549855287634335ull
};


/// Scan a zone's offsets for a position whose vowel satisfies `want`, at zero travel.
template <typename Want>
float findVoxOffset(uint16_t zone, Want&& want) {
	for (int i = 0; i < 400000; ++i) {
		const float offset = static_cast<float>(i) * 0.0137f;
		const Character c = characterFor({ zone, offset, 0.0, 0.0, 0.0, 1 });
		if (want(buildVoxFor(c), c)) {
			return offset;
		}
	}
	REQUIRE_MESSAGE(false, "no offset in range has the asked-for vowel — the species field changed");
	return 0.0f;
}

float maxAbsDiff(const Buffer& a, const Buffer& b) {
	float worst = 0.0f;
	for (size_t i = 0; i < a.size(); ++i) {
		worst = std::max(worst, std::abs(a[i] - b[i]));
	}
	return worst;
}

using Grid = std::array<float, kFlagshipSlots + 1>;

/// One grid, baked the way a voice bakes it: the recipe re-prepared against the source, then the
/// single-mip bake. The route under test, called by hand.
Grid bakeThrough(const FlagshipRecipe& recipe, const FlagshipSource& source, int mip = 0) {
	Grid grid {};
	FlagshipRecipe r = recipe;
	prepareFlagshipRecipe(r, source);
	bakeFlagshipGridAtMip(r, source, mip, grid.data());
	return grid;
}


/// What a voice OUGHT to have baked for `c` at `noteHz` — the recipe, the vowel for that note, the
/// staircase re-derived against the blend, and the one mip the note plays. Built here from the
/// public pieces rather than read off the voice, so the two are independent statements of the same
/// thing.
Grid expectedVoiceGrid(const Character& c, double noteHz) {
	const VoxCharacter vox = buildVoxFor(c);
	VoxCycle cycle;
	bakeVoxCycle(vox.params, noteHz, cycle);
	const FlagshipSource source =
	    cycle.voiced ? FlagshipSource { &cycle, vox.presence } : FlagshipSource {};
	return bakeThrough(buildFlagshipFor(c), source, stairMipForFrequency(noteHz));
}

} // namespace

TEST_CASE("THE PIN: vowel-free terrain renders the bits it rendered before vox existed") {
	// THE CLAIM, stated exactly, because an earlier draft of this comment overstated it and a
	// careful reader would have taken that apart:
	//
	//   NO NEW FLOAT CODE RUNS AT PRESENCE 0, AND THE FLOAT CODE THAT RUNS IS THE ORIGINAL SOURCE
	//   TEXT. It is NOT "no new code runs" — nextSample now decrements a bake countdown on every
	//   sample of every position, and updateVoxGrids tests four bools per interval. That work is
	//   integer and bool bookkeeping the float path never reads, and refreshGrids falls through to
	//   the two lines it always had, verbatim. The invariant that keeps this true is narrow and
	//   worth naming: updateVoxGrids must never write a float the sample loop reads, and must never
	//   set gridsDirty_, on its vowel-free branch.
	//
	// The evidence is 228 renders of 54 frozen positions, hashed, against numbers captured at
	// e7f1690 by a build that had never heard of a vowel. See kSweepHashes for why that revision.
	std::vector<uint64_t> got;
	std::vector<float> peaks;
	got.reserve(pin::kPinHashCount);
	peaks.reserve(pin::kPinHashCount);
	pin::pinSweep([&got, &peaks](const pin::PinBuffer& b) {
		got.push_back(pin::pinHash(b.data(), sizeof(b)));
		float peak = 0.0f;
		for (const float v : b) {
			peak = std::max(peak, std::abs(v));
		}
		peaks.push_back(peak);
	});
	REQUIRE(got.size() == pin::kPinHashCount);
	REQUIRE(std::size(kSweepHashes) == pin::kPinHashCount);

	// TWO CLAIMS OVER ONE SWEEP, split by pin::hashIsSmooth and nothing else. Outside smooth country
	// the e7f1690 numbers stood untouched until 2026-09-24, when the decimator swap moved every hash
	// in the file at once and both tables were re-taken together — see the note above kSweepHashes
	// for why that is a re-baseline of the output filter and not of the terrain.
	// Inside it, the smooth road's own recorded numbers. NEITHER TABLE WAS REGENERATED TO MAKE THE
	// OTHER PASS, which is the whole reason this is a partition and not a re-capture.
	//
	// kSmoothHashes RE-CAPTURED 2026-08-22, on a DELIBERATE character change: the
	// smoothness crossfade moved from the source to after quantize. The partition is what made that
	// a decision rather than a discovery — it reported ZERO of the plain hashes changed and 39 of the
	// smooth ones, which is the scope claim measured rather than asserted, and is why only this table
	// was re-taken. If a change ever moves a PLAIN hash, it is not a smooth-country change and the
	// numbers above are not the ones to regenerate.
	size_t plain = 0;
	size_t smooth = 0;
	for (size_t i = 0; i < got.size(); ++i) {
		CAPTURE(i);
		if (pin::hashIsSmooth(i)) {
			REQUIRE(smooth < std::size(kSmoothHashes));
			REQUIRE(got[i] == kSmoothHashes[smooth++]);
		}
		else {
			REQUIRE(got[i] == kSweepHashes[i]);
			++plain;
		}
	}
	// THE PARTITION ITSELF IS PINNED. A partition that quietly re-selected itself would let the
	// field move while the pin went on passing — the exact failure freezing the coordinates prevents
	// one level down. If the smoothness field's rate, duty or offset changes, THIS fails and says
	// so, and re-deriving the split becomes a deliberate act rather than a silent one.
	REQUIRE(smooth == std::size(kSmoothHashes));
	CHECK(smooth == 59);
	CHECK(plain == 169);

	// SILENCE HASHES AS RELIABLY AS ANYTHING ELSE, so a pin made only of hashes cannot tell a
	// correct render from a dead one. Every buffer has to be a waveform.
	for (size_t i = 0; i < peaks.size(); ++i) {
		CAPTURE(i);
		REQUIRE(peaks[i] > 0.01f);
	}

	// AND ALL 228 MUST BE DIFFERENT. Without this the pin cannot tell "54 places rendering
	// correctly" from "54 places that have become the same place", which is exactly what a
	// mis-plumbed blend or an early return would produce — and every hash would still match if the
	// baseline had been captured from the same mistake.
	std::vector<uint64_t> sorted = got;
	std::sort(sorted.begin(), sorted.end());
	CHECK(std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end());

	// The positions really are the vowel-free country they claim to be — checked against the
	// engine, and against this file's own independent spelling of the coordinate.
	int operated = 0;
	int corners = 0;
	int operatorCorners = 0;
	int twoSound = 0;
	int withGamma = 0;
	int withTravel = 0;
	for (size_t i = 0; i < pin::kSpotCount; ++i) {
		CAPTURE(i);
		const Character c = pin::characterOf(pin::kSpots[i]);
		const VoxCharacter vox = buildVoxFor(c);
		REQUIRE(vox.presence == presenceOf(c));
		REQUIRE(vox.presence == 0.0f);
		REQUIRE_FALSE(vox.audible);
		const FlagshipRecipe recipe = buildFlagshipFor(c);
		recipe.operated ? ++operated : ++corners;
		// THE OPERATOR TIER AT REST, asked separately from `operated` because those two questions
		// came apart when the smooth source landed: `operated` now means "not the plain morph path"
		// and a fourth field can make it true with every operator depth still exactly zero.
		if (recipe.quantize == 0.0f && recipe.syncRatio == 1.0f && recipe.feedback == 0.0f) {
			++operatorCorners;
		}
		twoSound += pin::kSpots[i].sounds > 1 ? 1 : 0;
		withGamma += pin::kSpots[i].gamma != 0.0 ? 1 : 0;
		withTravel += (pin::kSpots[i].travelX != 0.0 || pin::kSpots[i].travelY != 0.0) ? 1 : 0;
	}
	// COVERAGE, stated rather than hoped for. A pin that had quietly become all-one-kind would pass
	// every hash above and be evidence for about half as much.
	CHECK(operated >= 40); // the baked-grid road
	// THE OPERATOR TIER AT REST — six spots, and the smooth source did not move one of them, because
	// it is a different field and the operator depths are what they always were.
	CHECK(operatorCorners >= 5);
	// AND OF THOSE SIX, HOW MANY STILL TAKE THE CONTINUOUS MORPH TABLE: one. This number was six
	// until the smooth source landed, and it is recorded at its true value rather than the old
	// threshold being kept alive by loosening what it counts. Five of the six operator-corner spots
	// sit in the low-home cluster (indices 48..53) that was added to cover exactly this road, and
	// the smoothness field's territory falls across them — so they now render through the baked
	// grid, still pinned, just no longer pinning decision V8's continuous half.
	//
	// ONE WITNESS IS THIN AND IS MEANT TO READ AS THIN. If the smoothness offset ever moves — it is
	// a musical question that was open when this landed — this number should be revisited, and a
	// spot deliberately chosen outside smooth country would be the honest way to restore it.
	CHECK(corners >= 1);
	CHECK(twoSound >= 20);  // the blend road, not just single sounds
	CHECK(withGamma >= 30); // gamma's unbounded half, so the sweep leaves the first home units
	CHECK(withTravel >= 20);
	// The mip ladder: the three parked notes land on all three rungs, and 2600 Hz is the one no
	// earlier draft reached — flagshipMipSlots' dullest branch was unpinned without it.
	CHECK(stairMipForFrequency(55.0) == 0);
	CHECK(stairMipForFrequency(1200.0) == 1);
	CHECK(stairMipForFrequency(2600.0) == 2);
}

/// The fresh spots' recorded numbers — current behaviour, dated the day the smooth source landed.
/// RE-CAPTURED 2026-09-24 with the decimator swap; see the note above kSweepHashes (40 of 40 moved).
constexpr uint64_t kSmoothExtraHashes[] = {
	15482844057414469121ull, 11900108352301667050ull, 7201668154172881656ull, 6740281571474321221ull,
	953549664182581783ull, 16453200861556170514ull, 3600982691658264600ull, 457433940676719715ull,
	10564734531699925665ull, 2655703689095785817ull, 3919931598135699406ull, 4220605540817701450ull,
	3439483175252184988ull, 391043936330373641ull, 4058908728901225285ull, 13804481884923722513ull,
	15074509220056427994ull, 9946065363729979121ull, 14318132481157397786ull, 16442603755334877140ull,
	614902538345785652ull, 18428655567842899591ull, 4123344046771473515ull, 13872778889977074607ull,
	10768766723064621349ull, 1267112991336009422ull, 1101412693496116610ull, 15491582623130700403ull,
	15213309234504939359ull, 14148075935287661395ull, 229542621454858994ull, 281293066753266470ull,
	14947351400311777987ull, 13557958568027155994ull, 13747789785670339586ull, 12888940487050002535ull,
	17023255165163859085ull, 13929901647278086313ull, 15747060673271854749ull, 1807578372726161648ull
};

TEST_CASE("the smooth road's own pin, and it covers the corner the partition thinned") {
	// COVERAGE FIRST, BEFORE A SINGLE RENDER, and the order is the point. A known-bad that
	// swapped one fresh spot for a place with the operators awake died on the HASH check —
	// correctly, but it aborted the case before the coverage assertion below ever ran, which
	// means the guard that actually protects the repair was never the thing doing the work.
	// Worse, hashes are the one part a future editor can regenerate: change a spot, re-record,
	// green suite, silently degraded pin. The claim about WHICH PLACES these are does not
	// depend on what they render, so it is checked where nothing can abort ahead of it.
	// WHAT THESE SPOTS ARE FOR — asserted, because a coverage repair that quietly stopped covering
	// the thing it repaired would be worse than not having attempted it. Every one of them must be
	// in smooth country AND have the operator tier at rest: that combination is what the partition
	// left with a single witness, and it is restored here at eight rather than six.
	int corners = 0;
	int twoSound = 0;
	int withGamma = 0;
	int withTravel = 0;
	std::set<uint16_t> zones;
	float minSmooth = 1.0f;
	float maxSmooth = 0.0f;
	for (size_t i = 0; i < pin::kSmoothExtraCount; ++i) {
		CAPTURE(i);
		const Character c = pin::characterOf(pin::kSmoothExtraSpots[i]);
		const FlagshipRecipe r = buildFlagshipFor(c);
		// Vowel-free, like every spot in this file — the cubic and the vowel are never asked about
		// in the same breath.
		REQUIRE(buildVoxFor(c).presence == 0.0f);
		REQUIRE(r.smoothness > 0.0f);
		minSmooth = std::min(minSmooth, r.smoothness);
		maxSmooth = std::max(maxSmooth, r.smoothness);
		if (r.quantize == 0.0f && r.syncRatio == 1.0f && r.feedback == 0.0f) {
			++corners;
		}
		zones.insert(pin::kSmoothExtraSpots[i].zone);
		twoSound += pin::kSmoothExtraSpots[i].sounds > 1 ? 1 : 0;
		withGamma += pin::kSmoothExtraSpots[i].gamma != 0.0 ? 1 : 0;
		withTravel += (pin::kSmoothExtraSpots[i].travelX != 0.0
		               || pin::kSmoothExtraSpots[i].travelY != 0.0)
		                  ? 1
		                  : 0;
	}
	// FULL STRENGTH OR BETTER: the whole file had six operator-corner spots before the partition.
	CHECK(corners == 8);
	CHECK(zones.size() == 6);
	CHECK(twoSound >= 3);
	CHECK(withGamma >= 4);
	CHECK(withTravel >= 4);
	// And they SPAN smooth country rather than sitting in one neighbourhood of it, which is the
	// difference between eight spots and one spot measured eight times.
	CHECK(maxSmooth - minSmooth > 0.2f);
	CHECK(pin::kSmoothExtraPairs >= 4); // whole stereo pairs, not ears split across two claims

	std::vector<uint64_t> got;
	std::vector<float> peaks;
	pin::smoothExtraSweep([&got, &peaks](const pin::PinBuffer& b) {
		got.push_back(pin::pinHash(b.data(), sizeof(b)));
		float peak = 0.0f;
		for (const float v : b) {
			peak = std::max(peak, std::abs(v));
		}
		peaks.push_back(peak);
	});
	REQUIRE(got.size() == pin::kSmoothExtraHashCount);
	REQUIRE(std::size(kSmoothExtraHashes) == pin::kSmoothExtraHashCount);
	for (size_t i = 0; i < got.size(); ++i) {
		CAPTURE(i);
		REQUIRE(got[i] == kSmoothExtraHashes[i]);
	}
	// The same two guards the frozen pin carries, for the same reasons: silence hashes as reliably
	// as anything else, and a set of identical renders would hash consistently while proving nothing.
	for (size_t i = 0; i < peaks.size(); ++i) {
		CAPTURE(i);
		REQUIRE(peaks[i] > 0.01f);
	}
	std::vector<uint64_t> sorted = got;
	std::sort(sorted.begin(), sorted.end());
	CHECK(std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end());

}

TEST_CASE("the pin has teeth: a vowel forced into vowel-free country breaks every position") {
	// THE KNOWN-BAD. The hashes above are only evidence if a blend that reached this country would
	// be SEEN. Presence is not settable from outside — it is terrain — so the counterfactual is
	// built at the level the voice consumes: the same recipe, the same mip, blended against a real
	// vox cycle borrowed from a place that has one. If this matched, the pin would be measuring
	// nothing at all.
	const float vowelOffset = findVoxOffset(512, [](const VoxCharacter& v, const Character&) {
		return v.audible && v.presence > 0.5f;
	});
	const VoxCharacter borrowed = buildVoxFor(characterFor({ 512, vowelOffset, 0.0, 0.0, 0.0, 1 }));
	VoxCycle cycle;
	bakeVoxCycle(borrowed.params, 220.0, cycle);
	REQUIRE(cycle.voiced);

	int broken = 0;
	for (const pin::PinSpot& s : pin::kSpots) {
		const FlagshipRecipe recipe = buildFlagshipFor(pin::characterOf(s));
		const Grid clean = bakeThrough(recipe, {});
		const Grid tainted = bakeThrough(recipe, FlagshipSource { &cycle, borrowed.presence });
		broken += (clean != tainted) ? 1 : 0;
	}
	CHECK(broken == static_cast<int>(pin::kSpotCount));
}

TEST_CASE("a vowel is heard, and heard more as the terrain grows one") {
	// AUDIBILITY at a real position rather than a forced one, and the counterfactual is the SAME
	// place with the vowel taken out at the source seam. Morph table, operators, note and mip are
	// held identical, so the difference is the vowel and nothing else.
	const float offset = findVoxOffset(640, [](const VoxCharacter& v, const Character&) {
		return v.audible && v.presence > 0.6f;
	});
	const Character c = characterFor({ 640, offset, 0.0, 0.0, 0.0, 1 });
	const VoxCharacter vox = buildVoxFor(c);
	REQUIRE(vox.audible);

	VoxCycle cycle;
	bakeVoxCycle(vox.params, 220.0, cycle);
	REQUIRE(cycle.voiced);

	const FlagshipRecipe recipe = buildFlagshipFor(c);
	const Grid dry = bakeThrough(recipe, {});
	auto distanceFromDry = [&dry](const Grid& wet) {
		double sum = 0.0;
		for (size_t i = 0; i < dry.size(); ++i) {
			sum += std::abs(static_cast<double>(wet[i] - dry[i]));
		}
		return sum;
	};

	// PRESENCE ZERO THROUGH THE BLEND IS THE DRY CYCLE EXACTLY — morph + 0·(vox − morph) is morph
	// for any finite operand — so the ramp below starts where the engine's own vowel-free road
	// already is. That is what makes what follows a growth from the identity rather than from a
	// step somewhere near it.
	CHECK(bakeThrough(recipe, FlagshipSource { &cycle, 0.0f }) == dry);

	double previous = 0.0;
	for (const float presence : { 0.2f, 0.4f, 0.6f, 0.8f, 1.0f }) {
		const double here = distanceFromDry(bakeThrough(recipe, FlagshipSource { &cycle, presence }));
		CHECK(here > previous);
		previous = here;
	}
	CHECK(previous > 1.0);

	// And through the WHOLE VOICE, which is the claim that matters: this position rendered by the
	// plain morph builder — no operators, no vowel — against the flagship's own render of it.
	Character plainCharacter = c;
	plainCharacter.flagship = false;
	CHECK(maxAbsDiff(renderOf(c, 220.0), renderOf(plainCharacter, 220.0)) > 0.05f);
}

TEST_CASE("the blend is BEFORE the operator chain, and feedback is what makes it matter") {
	// THE CASE THAT WOULD PASS UNDER BLEND-AFTER AND FAILS UNDER IT — and, on the way, the
	// correction to which operator is doing the work.
	//
	// READ flagship.h's FlagshipSource FIRST for why the placement is right: it is ONE DECODE
	// (V21c), plus this feedback country, plus not needing a second bake and a crossfade. It is NOT
	// "the chain is nonlinear", which is the claim this case exists to bound.
	//
	// If the vowel were mixed in downstream, what came out would by definition be
	//     (1−p)·operate(morph) + p·operate(vox)
	// — a linear combination of two separately-operated cycles. V22c put the blend upstream, so what
	// is actually played is
	//     operate((1−p)·morph + p·vox).
	// Those differ exactly to the extent that `operate` is nonlinear IN ITS SOURCE.
	//
	// AND ONLY ONE OF THE THREE OPERATORS IS, WHICH WAS NOT THE EXPECTATION. Quantize looks like the
	// obvious nonlinearity and is not one: it samples the chain at step centres decided by PHASE
	// alone and holds, and a sample-and-hold at signal-independent instants is a linear map. Sync is
	// a phase warp, also linear in the source. FEEDBACK is the genuine nonlinearity, because the
	// source is evaluated at a phase that depends on its own output — y ← C(p + depth·y) — so which
	// waveform was in front of the chain changes where the chain looks next. MEASURED: at a
	// quantize-0.65, sync-1.0, feedback-0 position the two orders agree to 3.7e-08, and the case
	// below is at a position where feedback is up.
	// EVERY PROPERTY THE CASE NEEDS IS IN THE PREDICATE, not asserted after the search. The species
	// duty moved from 0.5 to 0.25, which halved how much of the terrain has a vowel — and a search
	// that asked for less than it needed then landed on the first vocal position without a
	// staircase, and failed on an assertion it had never asked the terrain for.
	const float offset = findVoxOffset(777, [](const VoxCharacter& v, const Character& c) {
		const FlagshipRecipe r = buildFlagshipFor(c);
		return v.audible && v.presence > 0.4f && r.feedback > 0.3f && r.quantize > 0.0f;
	});
	const Character c = characterFor({ 777, offset, 0.0, 0.0, 0.0, 1 });
	const VoxCharacter vox = buildVoxFor(c);
	REQUIRE(vox.audible);
	const FlagshipRecipe recipe = buildFlagshipFor(c);
	REQUIRE(recipe.feedback > 0.3f);
	REQUIRE(recipe.quantize > 0.0f); // the position carries a staircase too, so the chain is whole

	VoxCycle cycle;
	bakeVoxCycle(vox.params, 220.0, cycle);
	REQUIRE(cycle.voiced);

	// The gap between the two placements, built through the SAME operator machinery on both sides so
	// that WHERE the mix happens is the only difference between them.
	// WITHOUT THE NORMALISER, and that is not a dodge — it is what makes this a measurement of the
	// CHAIN. bakeThrough re-prepares per source, which is right for the engine (each rendered sound is
	// normalised to its own peak) and wrong for this question: the three bakes below then carry three
	// different gains, and an affine transform only commutes with a crossfade when its coefficients
	// are the SAME on both sides. Leaving them in would have this case reporting the normaliser's
	// per-source gain as if it were the operator chain's nonlinearity, which is the opposite of what
	// its name claims.
	auto raw = [](Grid g, const FlagshipRecipe& r) {
		for (float& v : g) {
			v = static_cast<float>(static_cast<double>(v) / std::max(1e-12, static_cast<double>(r.gain))
			                       + static_cast<double>(r.dcOffset));
		}
		return g;
	};
	auto bakeRaw = [&](const FlagshipRecipe& r, const FlagshipSource& source) {
		FlagshipRecipe prepared = r;
		prepareFlagshipRecipe(prepared, source);
		return raw(bakeThrough(r, source), prepared);
	};
	auto placementGap = [&](const FlagshipRecipe& r) {
		const Grid before = bakeRaw(r, FlagshipSource { &cycle, vox.presence });
		const Grid morphOperated = bakeRaw(r, {});
		const Grid voxOperated = bakeRaw(r, FlagshipSource { &cycle, 1.0f });
		double gap = 0.0;
		double scale = 0.0;
		for (size_t i = 0; i < before.size(); ++i) {
			const double after = static_cast<double>(morphOperated[i])
			                     + static_cast<double>(vox.presence)
			                           * static_cast<double>(voxOperated[i] - morphOperated[i]);
			gap = std::max(gap, std::abs(static_cast<double>(before[i]) - after));
			scale = std::max(scale, std::abs(static_cast<double>(before[i])));
		}
		return std::pair<double, double> { gap, scale };
	};

	const auto [gap, scale] = placementGap(recipe);
	// Not a rounding difference: a real fraction of the waveform's own swing. Blending after the
	// chain would be a different sound at this position, which is the decision's whole content.
	CHECK(gap > 0.05 * scale);

	// THE CONTROL, and the correction stated as a measurement: the same position with FEEDBACK ALONE
	// removed — sync and quantize left exactly as the terrain authored them — and the two placements
	// agree. So the nonlinearity that makes V22c's placement audible is feedback's, and a reading of
	// this file that credited quantize would be wrong about where the seam earns its keep.
	FlagshipRecipe noFeedback = recipe;
	noFeedback.feedback = 0.0f;
	prepareFlagshipRecipe(noFeedback, {});
	REQUIRE(noFeedback.operated);
	REQUIRE(noFeedback.steps > 0);
	const auto [linearGap, linearScale] = placementGap(noFeedback);
	CHECK(linearScale > 0.1); // there is a real waveform here to have been changed
	CHECK(linearGap < 1e-6);
}

namespace {
/// One sound's vox recipe, through the house pin fallback — a second spelling of what buildVoxFor's
/// pass two does, so a dropped sound, a mis-weighting or a wrong frame is caught here rather than
/// left to look like a plausible vowel.
VoxParams oneSoundParams(const Character& c, size_t i) {
	const SoundCoord& s = c.sounds[i];
	const SliceOrientationPin& pin = s.orientation.pinned ? s.orientation : c.orientation;
	const double home = sliceHome(s.zone, s.phaseOffset, s.gammaBase);
	return buildVoxSliced(s.zone, s.phaseOffset, s.travelX, s.travelY,
	                      sliceFrameFor<kVoxTriangles>(rotateFor(pin), home, s.residue.vox),
	                      s.gammaBase);
}

/// A two-sound character at two chosen zones, weighted `wA` / `1 - wA`.
Character pairOf(uint16_t zoneA, float offsetA, uint16_t zoneB, float offsetB, double wA) {
	Character c;
	c.engine = OscEngine::Morph;
	c.flagship = true;
	c.count = 2;
	c.sounds[0].zone = zoneA;
	c.sounds[0].phaseOffset = offsetA;
	c.sounds[0].weight = wA;
	c.sounds[1] = c.sounds[0];
	c.sounds[1].zone = zoneB;
	c.sounds[1].phaseOffset = offsetB;
	c.sounds[1].weight = 1.0 - wA;
	return c;
}

} // namespace

TEST_CASE("a single sound is its own vowel, to the bit") {
	// POSITION PURITY, which the log-space blend is one careless spelling away from losing: a round
	// trip through exp2(log2(x)) lands within a ULP of x rather than on it, and normalising by a
	// reciprocal does not give exactly 1 even when the weight IS the total. Both are fixed by
	// construction — the logs are anchored at the first contributing sound, so a lone sound's term
	// is log2(ref/ref) which is exactly zero, and the weight is a division, so it is exactly one.
	// This case is what says so.
	const float offset = findVoxOffset(512, [](const VoxCharacter& v, const Character&) {
		return v.audible && v.presence > 0.5f;
	});
	const Character solo = characterFor({ 512, offset, 0.0, 0.0, 0.0, 1 });
	REQUIRE(solo.count == 1);
	REQUIRE(solo.sounds[0].weight == 1.0);
	CHECK(buildVoxFor(solo).params == oneSoundParams(solo, 0));

	// And it holds at a gamma and a travel, where the frames are doing real work.
	Character moved = solo;
	moved.sounds[0].gammaBase = 41.0;
	moved.sounds[0].travelX = 0.35;
	moved.sounds[0].travelY = -0.11;
	if (buildVoxFor(moved).audible) {
		CHECK(buildVoxFor(moved).params == oneSoundParams(moved, 0));
	}
}

TEST_CASE("a vowel-free neighbour makes the vowel QUIETER, never a different vowel") {
	// THE SEPARATION THE BLEND TURNS ON: presence dilutes, identity does not.
	//
	// buildVoxSliced answers every position, including the three quarters of the terrain with no
	// vowel in it, where its formant numbers mean nothing at all. Weighting the recipe by w alone
	// would let one of those places drag a real vowel's formants halfway toward arbitrary values —
	// so standing between an "ah" and a plain morph sound would give you neither an "ah" nor a
	// fade, but a third, arbitrary vowel. This is the direct known-bad for that, and it fails
	// loudly on the w-alone spelling.
	const float vocal = findVoxOffset(512, [](const VoxCharacter& v, const Character&) {
		return v.audible && v.presence > 0.6f;
	});
	const float silent = findVoxOffset(512, [](const VoxCharacter& v, const Character&) {
		return v.presence == 0.0f;
	});
	const Character alone = characterFor({ 512, vocal, 0.0, 0.0, 0.0, 1 });
	const VoxCharacter aloneVox = buildVoxFor(alone);
	REQUIRE(aloneVox.audible);

	// The vowel-free neighbour's own recipe is real, arbitrary and NOT the vowel's — which is what
	// makes the identity check below mean something.
	const Character neighbourAlone = characterFor({ 512, silent, 0.0, 0.0, 0.0, 1 });
	REQUIRE(buildVoxFor(neighbourAlone).presence == 0.0f);
	REQUIRE_FALSE(oneSoundParams(neighbourAlone, 0) == oneSoundParams(alone, 0));

	for (const double weight : { 0.75, 0.5, 0.25 }) {
		CAPTURE(weight);
		const Character mixed = pairOf(512, vocal, 512, silent, weight);
		const VoxCharacter blended = buildVoxFor(mixed);
		REQUIRE(blended.audible);
		// THE IDENTITY IS UNMOVED — the vowel-free sound contributes nothing to what the vowel is,
		// at any weight, because its identity weight is w x 0.
		CHECK(blended.params == aloneVox.params);
		// AND THE PRESENCE HAS FALLEN, by exactly the neighbour's share, because presence keeps its
		// plain w weighting and is deliberately not normalised.
		CHECK(blended.presence
		      == doctest::Approx(static_cast<double>(aloneVox.presence) * weight).epsilon(1e-6));
		CHECK(blended.presence < aloneVox.presence);
	}
}

TEST_CASE("two vowels make a DIPHTHONG — one tract moving, not two singers") {
	// THE DESIGN DECISION, and the reason an earlier draft of this blend was wrong twice over. The
	// intermediate between "ah" and "ee" is not a phantom value to be avoided; it is what a real
	// vocal tract produces on the way between two shapes, and it is the point of the engine.
	//
	// A CROSSFADE AND A TRACT ARE TRIVIALLY DISTINGUISHABLE, which is what makes this a strong
	// known-bad rather than a restatement: crossfading two baked cycles leaves BOTH formant pairs
	// present at half strength — two singers — while blending the formants leaves ONE pair, in
	// between. The case below pins the second and shows the first would fail it.
	const float offsetA = findVoxOffset(300, [](const VoxCharacter& v, const Character&) {
		return v.audible && v.presence > 0.6f;
	});
	const float offsetB = findVoxOffset(900, [](const VoxCharacter& v, const Character&) {
		return v.audible && v.presence > 0.6f;
	});
	const VoxParams vowelA = oneSoundParams(characterFor({ 300, offsetA, 0.0, 0.0, 0.0, 1 }), 0);
	const VoxParams vowelB = oneSoundParams(characterFor({ 900, offsetB, 0.0, 0.0, 0.0, 1 }), 0);
	// Two genuinely distinct vowels, or the sweep below would be a fade from a thing to itself.
	REQUIRE(std::abs(vowelA.formant[0].frequencyHz - vowelB.formant[0].frequencyHz) > 20.0f);

	// THE TRAJECTORY IS CONTINUOUS AND MONOTONIC, and it passes through the GEOMETRIC middle — the
	// tract's own arithmetic, not the arithmetic mean. Sweeping the weight from all-A to all-B, F1
	// and F2 must walk from one vowel's to the other's without ever leaving the interval between.
	float previous1 = vowelA.formant[0].frequencyHz;
	const bool rising = vowelB.formant[0].frequencyHz > vowelA.formant[0].frequencyHz;
	for (int step = 0; step <= 8; ++step) {
		CAPTURE(step);
		const double wA = 1.0 - static_cast<double>(step) / 8.0;
		const VoxCharacter mid = buildVoxFor(pairOf(300, offsetA, 900, offsetB, wA));
		REQUIRE(mid.audible);
		for (size_t f = 0; f < kVoxNumFormants; ++f) {
			const float lo = std::min(vowelA.formant[f].frequencyHz, vowelB.formant[f].frequencyHz);
			const float hi = std::max(vowelA.formant[f].frequencyHz, vowelB.formant[f].frequencyHz);
			// BOUNDED BY WHAT THE NEIGHBOURS ACTUALLY ARE — never outside, at any weight.
			REQUIRE(mid.params.formant[f].frequencyHz >= lo * 0.999f);
			REQUIRE(mid.params.formant[f].frequencyHz <= hi * 1.001f);
		}
		const float here = mid.params.formant[0].frequencyHz;
		if (step > 0) {
			CHECK((rising ? here > previous1 : here < previous1));
		}
		previous1 = here;
	}

	// THE MIDPOINT IS A WEIGHTED GEOMETRIC MEAN — geometric because a tract moving 300 to 900 Hz
	// passes through 520 and not 600, and WEIGHTED BY w x presence rather than by w, because the two
	// places do not have equally strong vowels. Both halves of that are the decision, and the
	// expectation here is built from the engine's two inputs rather than from the engine's answer.
	const Character mid = pairOf(300, offsetA, 900, offsetB, 0.5);
	const VoxCharacter half = buildVoxFor(mid);
	const double idA = mid.sounds[0].weight * presenceOfSound(mid.orientation, mid.sounds[0]);
	const double idB = mid.sounds[1].weight * presenceOfSound(mid.orientation, mid.sounds[1]);
	REQUIRE(idA > 0.0);
	REQUIRE(idB > 0.0);
	const double uA = idA / (idA + idB);
	const double uB = idB / (idA + idB);
	for (size_t f = 0; f < kVoxNumFormants; ++f) {
		CAPTURE(f);
		const double a = static_cast<double>(vowelA.formant[f].frequencyHz);
		const double b = static_cast<double>(vowelB.formant[f].frequencyHz);
		const double weightedGeometric = std::pow(a, uA) * std::pow(b, uB);
		CHECK(static_cast<double>(half.params.formant[f].frequencyHz)
		      == doctest::Approx(weightedGeometric).epsilon(1e-4));
		// THE KNOWN-BAD FOR LINEAR-Hz BLENDING: the arithmetic mean at the same weights is a
		// different number wherever the two vowels are far enough apart, and the case fails if the
		// blend ever goes back to it.
		const double weightedArithmetic = uA * a + uB * b;
		if (std::abs(weightedArithmetic - weightedGeometric) > 1.0) {
			CHECK(static_cast<double>(half.params.formant[f].frequencyHz)
			      != doctest::Approx(weightedArithmetic).epsilon(1e-4));
		}
	}

	// AND IT IS ONE TRACT, NOT TWO SINGERS. A crossfade of the two BAKED CYCLES is a different
	// waveform from the bake of the blended params — that is the whole decision — and the difference
	// is a real fraction of the swing rather than a rounding.
	VoxCycle cycleA;
	VoxCycle cycleB;
	VoxCycle cycleMid;
	bakeVoxCycle(vowelA, 130.81, cycleA);
	bakeVoxCycle(vowelB, 130.81, cycleB);
	bakeVoxCycle(half.params, 130.81, cycleMid);
	REQUIRE(cycleA.voiced);
	REQUIRE(cycleB.voiced);
	REQUIRE(cycleMid.voiced);
	double gap = 0.0;
	double scale = 0.0;
	for (size_t i = 0; i < cycleMid.slots.size(); ++i) {
		const double crossfaded =
		    0.5 * (static_cast<double>(cycleA.slots[i]) + static_cast<double>(cycleB.slots[i]));
		gap = std::max(gap, std::abs(static_cast<double>(cycleMid.slots[i]) - crossfaded));
		scale = std::max(scale, std::abs(static_cast<double>(cycleMid.slots[i])));
	}
	CHECK(gap > 0.1 * scale);
}


TEST_CASE("the voice bakes for its own note, and the bank keeps out of it") {
	// Decision V22a in one case: the vowel's cycle depends on the NOTE, so the same character at two
	// pitches is two different waveforms — a difference no position-pure build could produce, since
	// the bank's tables are identical for both.
	const float offset = findVoxOffset(900, [](const VoxCharacter& v, const Character&) {
		return v.audible && v.presence > 0.5f;
	});
	const Character c = characterFor({ 900, offset, 0.0, 0.0, 0.0, 1 });
	const VoxCharacter vox = buildVoxFor(c);
	REQUIRE(vox.audible);

	// The bake is a function of formantHz/fundamentalHz, so two notes an octave apart fit different
	// numbers of formant periods into a cycle.
	VoxCycle low;
	VoxCycle high;
	bakeVoxCycle(vox.params, 110.0, low);
	bakeVoxCycle(vox.params, 220.0, high);
	REQUIRE(low.voiced);
	REQUIRE(high.voiced);
	CHECK_FALSE(low.slots == high.slots);
	CHECK(maxAbsDiff(renderOf(c, 110.0), renderOf(c, 220.0)) > 0.0f);

	// And the bank, correctly, has nothing to say about which: it offers no grid at all for a vowel build (gridFor is
	// null at every rung), so no voice can read one. It no longer BAKES the vox-free grid there either (backlog PB) —
	// what that costs nothing is held by "the bank's grid is baked the moment a voice can read it" below.
	CharacterBank bank;
	bank.setSampleRate(kRate);
	bank.setCharacter(c);
	for (int mip = 0; mip < 3; ++mip) {
		CHECK(bank.current().gridFor(mip) == nullptr);
	}
	CHECK(bank.current().readsGrid());
	CHECK(bank.current().vox.audible);
	CHECK(bank.current().vox.presence == vox.presence);
}

TEST_CASE("the bank's grid is baked the moment a voice can read it: leaving vowel country reads the right grid, never a slot's stale one (backlog PB)") {
	// A VOWEL BUILD SKIPS THE BANK'S VOX-FREE GRID (nothing can read it), so its slot keeps whatever grid it held
	// before — the shape of an ABA: the bank has two slots per ear, so a vowel build lands on the slot a vowel-free
	// build two rebuilds ago baked, and that build's grid is still sitting there. A bank that decided to bake by what
	// the slot held, or a voice that read it, would play a grid from a character the puck left long ago.
	//
	// So: vowel-free F0, then vowels V1 and V2 (V2 lands on F0's slot, which still holds F0's grid), then vowel-free F3
	// and F4. At every vowel-free build, the grid the bank hands out and the grid the voice is reading on its first
	// block after the build is, bit for bit, an independent bake of THAT character; and through the fade into it the
	// voice reads that grid too, never F0's.
	const auto vowelFree = [](const VoxCharacter& v, const Character& c) {
		return !v.audible && buildFlagshipFor(c).operated;
	};
	const auto vowel = [](const VoxCharacter& v, const Character& c) {
		return v.audible && v.presence > 0.3f && buildFlagshipFor(c).operated;
	};
	const Character f0 = characterFor({ 640, findVoxOffset(640, vowelFree), 0.0, 0.0, 0.0, 1 });
	const Character v1 = characterFor({ 300, findVoxOffset(300, vowel), 0.0, 0.0, 0.0, 1 });
	const Character v2 = characterFor({ 900, findVoxOffset(900, vowel), 0.0, 0.0, 0.0, 1 });
	const Character f3 = characterFor({ 120, findVoxOffset(120, vowelFree), 0.0, 0.0, 0.0, 1 });
	const Character f4 = characterFor({ 500, findVoxOffset(500, vowelFree), 0.0, 0.0, 0.0, 1 });
	const Character sequence[] = { f0, v1, v2, f3, f4 };

	constexpr double kNote = 220.0;
	CharacterBank bank;
	bank.setSampleRate(kRate);
	bank.setCharacter(f0);
	OscVoice voice;
	voice.setCharacterBank(&bank);
	voice.setSampleRate(kRate);
	voice.setFrequency(kNote);
	const int mip = stairMipForFrequency(kNote);
	FlagshipTable f0Grid {};
	bakeFlagshipTable(buildFlagshipFor(f0), f0Grid);
	for (size_t gen = 0; gen < std::size(sequence); ++gen) {
		CAPTURE(gen);
		const Character& c = sequence[gen];
		if (gen > 0) {
			bank.setCharacter(c);
		}
		const bool audible = buildVoxFor(c).audible;
		REQUIRE(bank.current().vox.audible == audible);
		// THE FIRST BLOCK AFTER THE BUILD — the voice syncs and starts its fade into it.
		Buffer out {};
		voice.render(out.data(), 32);
		bank.advance(32);
		if (audible) {
			REQUIRE(bank.current().gridFor(mip) == nullptr);
			Buffer rest {};
			voice.render(rest.data(), kRenderSamples); // past the guard, so the next request builds at once
			bank.advance(kRenderSamples);
			continue;
		}
		FlagshipTable want {};
		bakeFlagshipTable(buildFlagshipFor(c), want);
		REQUIRE(bank.current().flagship == want); // member-wise; see FlagshipTable::operator==
		const float* played = voice.freshGridForTest(0);
		REQUIRE(played == bank.current().gridFor(mip));
		const float* rung = flagshipMipSlots(want, mip);
		for (int j = 0; j <= kFlagshipSlots; ++j) {
			REQUIRE(played[j] == rung[j]);
		}
		if (gen >= 3) {
			// NOT VACUOUS: the stale grid this guards against differs from the right one.
			CHECK_FALSE(bank.current().flagship == f0Grid);
		}
		// And it is still that grid once the fade has run out.
		Buffer rest {};
		voice.render(rest.data(), kRenderSamples);
		bank.advance(kRenderSamples);
		REQUIRE_FALSE(voice.isFadingForTest());
		REQUIRE(voice.freshGridForTest(0) == bank.current().gridFor(mip));
		for (int j = 0; j <= kFlagshipSlots; ++j) {
			REQUIRE(voice.freshGridForTest(0)[j] == rung[j]);
		}
	}
}

TEST_CASE("the voice's grid is the bake its recipe, its vowel and its NOTE describe") {
	// The claim a rendered buffer cannot make. A staircase derived from the vowel-free cycle, or a
	// grid baked at the wrong mip, still produces a perfectly plausible sound — plausible enough
	// that every difference test in this file goes on passing. So the grid the sample loop actually
	// loads is compared against an independently built one, bit for bit.
	//
	// THREE NOTES, ONE PER RUNG of the mip ladder (the switch points are 700 and 1800 Hz), because
	// the whole reason a voice bakes for itself is that it knows which rung it is on.
	const float offset = findVoxOffset(640, [](const VoxCharacter& v, const Character& c) {
		const FlagshipRecipe r = buildFlagshipFor(c);
		return v.audible && v.presence > 0.5f && r.feedback > 0.2f && r.steps > 0;
	});
	const Character c = characterFor({ 640, offset, 0.0, 0.0, 0.0, 1 });
	REQUIRE(buildVoxFor(c).audible);
	REQUIRE(buildFlagshipFor(c).steps > 0); // there is a staircase here to get wrong

	for (const double noteHz : { 220.0, 1100.0, 2400.0 }) {
		CAPTURE(noteHz);
		CharacterBank bank;
		bank.setSampleRate(kRate);
		bank.setCharacter(c);
		OscVoice voice;
		voice.setCharacterBank(&bank);
		voice.setSampleRate(kRate);
		voice.setFrequency(noteHz);
		Buffer out {};
		voice.render(out.data(), 64);
		bank.advance(64);

		REQUIRE(voice.mipForTest() == stairMipForFrequency(noteHz));
		const float* played = voice.freshGridForTest(0);
		REQUIRE(played != nullptr);
		// Not the bank's grid: the bank's is vox-free and shared, and this note's is neither.
		REQUIRE(played != bank.current().gridFor(voice.mipForTest()));
		const Grid want = expectedVoiceGrid(c, noteHz);
		for (int j = 0; j <= kFlagshipSlots; ++j) {
			REQUIRE(played[j] == want[static_cast<size_t>(j)]);
		}
		// And the fade's other side is the same grid, because there is nothing to fade from yet.
		CHECK(voice.staleGridForTest(0) == played);
	}
	// The three notes really did land on three different rungs, or the loop above would be one
	// claim made three times.
	CHECK(stairMipForFrequency(220.0) == 0);
	CHECK(stairMipForFrequency(1100.0) == 1);
	CHECK(stairMipForFrequency(2400.0) == 2);
}

TEST_CASE("gridFor and eval agree about whose grid a character is on") {
	// TWO FUNCTIONS ON ONE STRUCT MUST NOT DISAGREE ABOUT WHAT "ON THE GRID ROAD" MEANS. They did:
	// eval asked "operated or a vowel" while gridFor asked "operated" alone, and it was safe only
	// because the voice tests vox.audible before it ever asks. The trap that leaves is the worst
	// kind — a future caller writing perfectly reasonable code is handed the STAIR grid for a morph
	// character and eval reads it as a flagship grid, which is a silent wrong sound rather than a
	// failure. So they share one predicate now, and this case is what holds them to it.
	auto check = [](const Character& c, const char* what) {
		CAPTURE(std::string(what));
		CharacterBank bank;
		bank.setSampleRate(kRate);
		bank.setCharacter(c);
		const PreparedCharacter& p = bank.current();
		// The predicate is the road eval takes.
		const bool grid = p.readsGrid();
		if (p.engine == OscEngine::Morph) {
			CHECK(grid == (p.flagship.operated || p.vox.audible));
		}
		else {
			CHECK(grid);
		}
		// And gridFor answers for exactly the same fact: it has a grid whenever the character is on
		// the grid road AND the bank is the one who owns it. A vowel is the one case where it is
		// not, and it says so with a null rather than the nearest plausible waveform.
		const float* g = p.gridFor(0);
		if (p.vox.audible) {
			CHECK(g == nullptr);
		}
		else {
			CHECK(g != nullptr);
		}
	};

	// A vocal position: on the grid road, and the grid is NOT the bank's.
	const float vocal = findVoxOffset(512, [](const VoxCharacter& v, const Character&) {
		return v.audible && v.presence > 0.5f;
	});
	check(characterFor({ 512, vocal, 0.0, 0.0, 0.0, 1 }), "vocal");

	// A vocal position with NO operators — the case that made the two predicates part company, and
	// the one where the old gridFor would have handed back a stair grid for a morph character.
	const float vocalCorner = findVoxOffset(300, [](const VoxCharacter& v, const Character& c) {
		return v.audible && v.presence > 0.2f && !buildFlagshipFor(c).operated;
	});
	const Character corner = characterFor({ 300, vocalCorner, 0.0, 0.0, 0.0, 1 });
	REQUIRE(buildVoxFor(corner).audible);
	REQUIRE_FALSE(buildFlagshipFor(corner).operated);
	check(corner, "vocal identity corner");

	// And the two roads that predate vox, unchanged: an operated position with no vowel, and a
	// stair character, which has no vox road at all.
	check(characterFor(pin::kSpots[0].zone == 0 ? Spot { 512, 0.0f, 0.0, 0.0, 0.0, 1 }
	                                            : Spot { pin::kSpots[0].zone, pin::kSpots[0].offset,
		                                                   pin::kSpots[0].gamma, 0.0, 0.0, 1 }),
	      "vowel-free");
	Character stair = characterFor({ 400, 0.0f, 0.0, 0.0, 0.0, 1 });
	stair.engine = OscEngine::Stair;
	stair.flagship = false;
	check(stair, "stair");
}

TEST_CASE("a voice in vowel-free country is on the bank's grid, not one of its own") {
	// The identity, said as a POINTER rather than as a hash. The sweep pins the bits; this pins the
	// mechanism that produces them — a voice standing where there is no throat reads the shared
	// grid the bank baked, which is what makes the whole vox road cost nothing there.
	for (const pin::PinSpot& spot : pin::kSpots) {
		const Character c = pin::characterOf(spot);
		REQUIRE_FALSE(buildVoxFor(c).audible);
		CharacterBank bank;
		bank.setSampleRate(kRate);
		bank.setCharacter(c);
		OscVoice voice;
		voice.setCharacterBank(&bank);
		voice.setSampleRate(kRate);
		voice.setFrequency(147.0);
		Buffer out {};
		voice.render(out.data(), 64);
		bank.advance(64);
		REQUIRE(voice.freshGridForTest(0) == bank.current().gridFor(voice.mipForTest()));
	}
}

TEST_CASE("mono collapse survives a vowel: at width 0 the two ears stay bit-identical") {
	// The width-0 promise is structural — the bank hands ear 1 the same table as ear 0, so the two
	// pointers are EQUAL and the voice skips the second bake entirely. That is exactly the road a
	// per-voice bake could break, so it is checked THROUGH vowel country rather than beside it.
	const float offset = findVoxOffset(120, [](const VoxCharacter& v, const Character&) {
		return v.audible && v.presence > 0.4f;
	});
	const Character c = characterFor({ 120, offset, 0.0, 0.0, 0.0, 1 });
	REQUIRE(buildVoxFor(c).audible);

	CharacterBank bank;
	bank.setSampleRate(kRate);
	bank.setCharacter(c); // a mono request: the bank answers ear 1 with ear 0's table
	OscVoice voice;
	voice.setCharacterBank(&bank);
	voice.setSampleRate(kRate);
	voice.setFrequency(196.0);
	Buffer left {};
	Buffer right {};
	voice.renderStereo(left.data(), right.data(), kRenderSamples);
	bank.advance(kRenderSamples);
	CHECK(std::memcmp(left.data(), right.data(), sizeof(left)) == 0);

	// And it is the same stream the mono loop produces — the pair loop and the single loop mirror
	// each other, and a per-voice bake is the newest way to make them disagree.
	const Buffer mono = renderOf(c, 196.0);
	CHECK(std::memcmp(left.data(), mono.data(), sizeof(left)) == 0);
}

TEST_CASE("the two ears bake their own vowels when the terrain really does differ") {
	// The other side of that equality test: two genuinely different characters, one per ear, both
	// in vowel country. Each ear must get its own bake — an ear quietly reading the other's grid
	// would come out identical, which is what this catches.
	const float leftOffset = findVoxOffset(300, [](const VoxCharacter& v, const Character&) {
		return v.audible && v.presence > 0.5f;
	});
	const float rightOffset = findVoxOffset(640, [](const VoxCharacter& v, const Character&) {
		return v.audible && v.presence > 0.5f;
	});
	const Character left = characterFor({ 300, leftOffset, 0.0, 0.0, 0.0, 1 });
	const Character right = characterFor({ 640, rightOffset, 0.0, 0.0, 0.0, 1 });
	REQUIRE(buildVoxFor(left).audible);
	REQUIRE(buildVoxFor(right).audible);

	CharacterBank bank;
	bank.setSampleRate(kRate);
	bank.setCharacterStereo(left, right);
	REQUIRE(&bank.current(0) != &bank.current(1));

	OscVoice voice;
	voice.setCharacterBank(&bank);
	voice.setSampleRate(kRate);
	voice.setFrequency(196.0);
	Buffer outL {};
	Buffer outR {};
	voice.renderStereo(outL.data(), outR.data(), kRenderSamples);
	bank.advance(kRenderSamples);
	CHECK(maxAbsDiff(outL, outR) > 0.05f);

	// Each ear is the mono render of its own character: the ears share the note, the phase and the
	// noise stream, and fork only at the table read.
	CHECK(std::memcmp(outL.data(), renderOf(left, 196.0).data(), sizeof(outL)) == 0);
	CHECK(std::memcmp(outR.data(), renderOf(right, 196.0).data(), sizeof(outR)) == 0);
}

TEST_CASE("the double buffer: a rebuild between two vowels crosses without a step") {
	// The swap, exercised. A voice fading from one vocal character into another reads BOTH bakes at
	// once, and the outgoing one is the grid that was fresh last chunk rather than a second bake of
	// the same thing. What a test can see of that is the seam: had the swap banked the wrong slot,
	// the fade would mix in a grid belonging to neither character and the crossing would show a
	// jump far larger than the waveform's own slope.
	const float aOffset = findVoxOffset(512, [](const VoxCharacter& v, const Character&) {
		return v.audible && v.presence > 0.5f;
	});
	const float bOffset = findVoxOffset(777, [](const VoxCharacter& v, const Character&) {
		return v.audible && v.presence > 0.5f;
	});
	const Character a = characterFor({ 512, aOffset, 0.0, 0.0, 0.0, 1 });
	const Character b = characterFor({ 777, bOffset, 0.0, 0.0, 0.0, 1 });
	REQUIRE(buildVoxFor(a).audible);
	REQUIRE(buildVoxFor(b).audible);

	CharacterBank bank;
	bank.setSampleRate(kRate);
	bank.setCharacter(a);
	OscVoice voice;
	voice.setCharacterBank(&bank);
	voice.setSampleRate(kRate);
	voice.setFrequency(147.0);

	Buffer first {};
	voice.render(first.data(), kRenderSamples);
	bank.advance(kRenderSamples);

	bank.setCharacter(b);
	Buffer second {};
	voice.render(second.data(), kRenderSamples);
	bank.advance(kRenderSamples);
	CHECK_FALSE(voice.isFadingForTest()); // 512 samples is well past a 96-sample fade

	// THE SWAP ITSELF: the side the fade mixes OUT of has to be the bake that was fresh before the
	// rebuild — the outgoing character's, at the note this voice is playing. A buffer that never
	// swapped would hand back the incoming character's grid for both sides, which is a fade from a
	// sound to itself and is exactly what this catches.
	{
		CharacterBank two;
		two.setSampleRate(kRate);
		two.setCharacter(a);
		OscVoice v;
		v.setCharacterBank(&two);
		v.setSampleRate(kRate);
		v.setFrequency(147.0);
		Buffer scratch {};
		v.render(scratch.data(), 64);
		two.advance(64);
		two.setCharacter(b);
		// A short render, so the fade is still in flight and both sides are genuinely being read.
		v.render(scratch.data(), 8);
		two.advance(8);
		REQUIRE(v.isFadingForTest());
		const Grid wantFresh = expectedVoiceGrid(b, 147.0);
		const Grid wantStale = expectedVoiceGrid(a, 147.0);
		REQUIRE(v.freshGridForTest(0) != v.staleGridForTest(0));
		for (int j = 0; j <= kFlagshipSlots; ++j) {
			REQUIRE(v.freshGridForTest(0)[j] == wantFresh[static_cast<size_t>(j)]);
			REQUIRE(v.staleGridForTest(0)[j] == wantStale[static_cast<size_t>(j)]);
		}
	}

	// R2 — BOTH SIDES OF THE FADE MUST BE BAKED AT THE NOTE BEING PLAYED, and the swap alone does
	// not give you that. The swap banks the outgoing bake, which is right and free while the note
	// has not moved; move the note between the two characters and the banked grid is for a pitch
	// this voice has left. The fade would then mix a vowel at the old pitch out against a vowel at
	// the new one — an audible swell across every character change made during a glide, and a
	// mutation that simply never bakes the stale side survives every other case in this file.
	{
		CharacterBank moving;
		moving.setSampleRate(kRate);
		moving.setCharacter(a);
		OscVoice v;
		v.setCharacterBank(&moving);
		v.setSampleRate(kRate);
		v.setFrequency(147.0);
		Buffer scratch {};
		v.render(scratch.data(), 64);
		moving.advance(64);

		// A note far enough away to be a different key, so the banked bake is genuinely stale.
		constexpr double kMoved = 220.0;
		REQUIRE(voxBakeNoteKey(kMoved) != voxBakeNoteKey(147.0));
		v.setFrequency(kMoved);
		moving.setCharacter(b);
		v.render(scratch.data(), 8);
		moving.advance(8);
		REQUIRE(v.isFadingForTest());

		const Grid wantFreshMoved = expectedVoiceGrid(b, kMoved);
		const Grid wantStaleMoved = expectedVoiceGrid(a, kMoved);
		for (int j = 0; j <= kFlagshipSlots; ++j) {
			REQUIRE(v.freshGridForTest(0)[j] == wantFreshMoved[static_cast<size_t>(j)]);
			REQUIRE(v.staleGridForTest(0)[j] == wantStaleMoved[static_cast<size_t>(j)]);
		}
		// And it really was stale — the bake at the old note is a different grid, so the equality
		// above is a rebake rather than a coincidence.
		const Grid atOldNote = expectedVoiceGrid(a, 147.0);
		CHECK_FALSE(atOldNote == wantStaleMoved);
	}

	// The largest step across the join, against the largest step in a settled stretch of the same
	// stream. A fade reading a grid from nowhere would blow this out by orders of magnitude.
	auto biggestStep = [](const float* p, int n) {
		float worst = 0.0f;
		for (int i = 1; i < n; ++i) {
			worst = std::max(worst, std::abs(p[i] - p[i - 1]));
		}
		return worst;
	};
	CHECK(biggestStep(second.data(), 128)
	      < 4.0f * std::max(biggestStep(first.data() + 128, 256), 0.01f));

	// And the rebuild really happened rather than being dropped as equal-to-live.
	CHECK(maxAbsDiff(first, second) > 0.0f);
}

TEST_CASE("the vox road allocates nothing, and its per-voice state is fixed-size") {
	// DECISION V10b, checked rather than asserted in a comment. Everything vox adds to a voice is a
	// fixed array or a scalar; everything the per-block bake needs beyond that lives on the stack.
	static_assert(std::is_trivially_copyable_v<VoxGrid>, "a voice's bake is plain storage");
	static_assert(std::is_trivially_copyable_v<VoxCycle>, "the scratch cycle is plain storage");
	static_assert(std::is_trivially_copyable_v<VoxCharacter>, "the prepared vowel is plain storage");
	static_assert(sizeof(VoxGrid::slots) == 3 * sizeof(float) * (kFlagshipSlots + 1),
	              "three rungs of the mip ladder and nothing else — no length, no owner");
	static_assert(sizeof(VoxGrid) <= sizeof(VoxGrid::slots) + 32,
	              "the key is a pointer and an integer beside the slots, not a second allocation");
	static_assert(std::is_trivially_copyable_v<VoxBakeKey>, "the bake key is plain storage");

	const float offset = findVoxOffset(512, [](const VoxCharacter& v, const Character&) {
		return v.audible && v.presence > 0.5f;
	});
	const Character vocal = characterFor({ 512, offset, 0.0, 0.0, 0.0, 1 });
	const Character other = characterFor({ 777, offset, 0.0, 0.0, 0.0, 1 });
	REQUIRE(buildVoxFor(vocal).audible);

	CharacterBank bank;
	bank.setSampleRate(kRate);
	bank.setCharacter(vocal);
	OscVoice voice;
	voice.setCharacterBank(&bank);
	voice.setSampleRate(kRate);
	voice.setFrequency(147.0);
	Buffer out {};
	Buffer outR {};
	// The first render is where anything lazy would happen — the first sync, the first bake, the
	// first grid resolve — so it is inside the count rather than excused from it.
	const long before = gAllocations.load(std::memory_order_relaxed);
	for (int block = 0; block < 8; ++block) {
		// A character change every block, which is what an automated position does: the bank
		// rebuilds, the voice swaps its slot and bakes the incoming side, all from fixed storage.
		bank.setCharacter((block % 2) == 0 ? other : vocal);
		voice.render(out.data(), 64);
		bank.advance(64);
	}
	// And the stereo road, which bakes twice.
	bank.setCharacterStereo(vocal, other);
	for (int block = 0; block < 4; ++block) {
		voice.renderStereo(out.data(), outR.data(), 64);
		bank.advance(64);
	}
	// A glide, which is what dirties the grids every evaluation sample — the case decision V22c(e)
	// says must not turn into a per-sample bake.
	voice.setGlideSeconds(0.5);
	voice.glideToPitch(72.0);
	for (int block = 0; block < 4; ++block) {
		voice.renderStereo(out.data(), outR.data(), 64);
		bank.advance(64);
	}
	CHECK(gAllocations.load(std::memory_order_relaxed) - before == 0);

	// THE COUNTER IS CAPABLE OF COUNTING — a watch through an interposition that had quietly stopped
	// working would pass forever and mean nothing.
	const long beforeReal = gAllocations.load(std::memory_order_relaxed);
	{
		std::vector<float> deliberate(1024, 0.0f);
		CHECK(deliberate.size() == 1024);
	}
	CHECK(gAllocations.load(std::memory_order_relaxed) > beforeReal);
}

TEST_CASE("the bake is on a schedule the host cannot reach") {
	// THE VOICE HEADER'S OWN RULE, and the vox bake is the newest schedule that could break it:
	// "nothing here may depend on how the host chops its blocks". Every other schedule — the fade,
	// the glide, the mip switch, the noise hold — advances on GENERATED samples, and the bake had to
	// join them rather than ride the chunk boundary, which is where the host's buffer lengths fall.
	//
	// A GLIDE IS WHAT MAKES THE DIFFERENCE VISIBLE. The vowel's cycle is a function of the note, so
	// baking it at a chunk boundary means baking it at whatever pitch the glide happened to be
	// passing through when the host's buffer ran out. One 512-sample call and 512 one-sample calls
	// pass through different boundaries and would bake at different pitches — the same automation
	// rendering differently at different block sizes, which is the thing this codebase pays a guard
	// and a samplesUntilPendingBuild() to prevent elsewhere.
	const float offset = findVoxOffset(300, [](const VoxCharacter& v, const Character&) {
		return v.audible && v.presence > 0.5f;
	});
	const Character c = characterFor({ 300, offset, 0.0, 0.0, 0.0, 1 });
	REQUIRE(buildVoxFor(c).audible);

	auto glidedRender = [&c](int blockSize) {
		CharacterBank bank;
		bank.setSampleRate(kRate);
		bank.setCharacter(c);
		OscVoice voice;
		voice.setCharacterBank(&bank);
		voice.setSampleRate(kRate);
		voice.setPitch(40.0);
		voice.setGlideSeconds(0.25);
		voice.glideToPitch(76.0); // three octaves, so the bake's note really is moving
		Buffer out {};
		for (int done = 0; done < kRenderSamples; done += blockSize) {
			const int take = std::min(blockSize, kRenderSamples - done);
			voice.render(out.data() + done, take);
			bank.advance(take);
		}
		REQUIRE(voice.isGliding());
		return out;
	};

	const Buffer whole = glidedRender(kRenderSamples);
	for (const int blockSize : { 1, 3, 4, 7, 32, 33, 128 }) {
		CAPTURE(blockSize);
		const Buffer chopped = glidedRender(blockSize);
		REQUIRE(std::memcmp(whole.data(), chopped.data(), sizeof(whole)) == 0);
	}
	// And the glide really did move the vowel — otherwise the equality above would be the equality
	// of a note that never changed, which no cadence could get wrong.
	Buffer parked {};
	{
		CharacterBank bank;
		bank.setSampleRate(kRate);
		bank.setCharacter(c);
		OscVoice voice;
		voice.setCharacterBank(&bank);
		voice.setSampleRate(kRate);
		voice.setPitch(40.0);
		voice.render(parked.data(), kRenderSamples);
		bank.advance(kRenderSamples);
	}
	CHECK(maxAbsDiff(whole, parked) > 0.01f);
}

TEST_CASE("the vowel FOLLOWS the character across repeated rebuilds — no ABA in the bake cache") {
	// THE CASE THAT SHOULD HAVE CAUGHT A SHIPPED REGRESSION, and the reason it did not exist is
	// worth stating: the identity pin proves vowel-FREE terrain is unchanged, and the constellation
	// and diphthong cases prove the blend is right AT A FIXED CHARACTER. Nothing anywhere required
	// the vowel to follow a character that CHANGES under a held note — which is what moving the
	// puck does, and it is the entire way a performer meets this engine.
	//
	// THREE REBUILDS IS THE MINIMUM THAT REPRODUCES IT. The bank double-buffers into
	// PreparedCharacter tables_[2][2] and flips on every build, so only two addresses per ear ever
	// exist. A cache keyed on that address is ABA-vulnerable: build 3 lands in the slot build 1
	// used, the key compares equal to build 1's, and a voice keeps playing a vowel two rebuilds
	// old. Two changes cannot show it; three can.
	const float offsetA = findVoxOffset(300, [](const VoxCharacter& v, const Character&) {
		return v.audible && v.presence > 0.4f;
	});
	const float offsetB = findVoxOffset(640, [](const VoxCharacter& v, const Character&) {
		return v.audible && v.presence > 0.4f;
	});
	const float offsetC = findVoxOffset(900, [](const VoxCharacter& v, const Character&) {
		return v.audible && v.presence > 0.4f;
	});
	const Character characters[] = {
		characterFor({ 300, offsetA, 0.0, 0.0, 0.0, 1 }),
		characterFor({ 640, offsetB, 0.0, 0.0, 0.0, 1 }),
		characterFor({ 900, offsetC, 0.0, 0.0, 0.0, 1 }),
		characterFor({ 300, offsetA, 0.0, 0.0, 0.0, 1 }),
	};
	for (const Character& c : characters) {
		REQUIRE(buildVoxFor(c).audible);
	}

	constexpr double kNote = 130.81; // C3, held throughout — the note never changes
	CharacterBank bank;
	bank.setSampleRate(kRate);
	bank.setCharacter(characters[0]);
	OscVoice voice;
	voice.setCharacterBank(&bank);
	voice.setSampleRate(kRate);
	voice.setFrequency(kNote);

	std::vector<Buffer> renders;
	std::vector<Grid> grids;
	for (size_t gen = 0; gen < std::size(characters); ++gen) {
		CAPTURE(gen);
		if (gen > 0) {
			bank.setCharacter(characters[gen]);
		}
		// Well past the 2 ms crossfade and past the bank's deferral guard, so what is measured is
		// the new character alone rather than a mix of it with the old.
		Buffer out {};
		voice.render(out.data(), kRenderSamples);
		bank.advance(kRenderSamples);
		REQUIRE_FALSE(voice.isFadingForTest());

		// THE SHARPEST FORM OF THE CLAIM: the grid the sample loop is reading is the bake of THIS
		// character at this note. Under ABA the third generation reads the first's, and this fails
		// on the exact slot the reuse hands back.
		const Grid want = expectedVoiceGrid(characters[gen], kNote);
		const float* played = voice.freshGridForTest(0);
		REQUIRE(played != nullptr);
		for (int j = 0; j <= kFlagshipSlots; ++j) {
			REQUIRE(played[j] == want[static_cast<size_t>(j)]);
		}
		Grid snapshot {};
		std::copy(played, played + kFlagshipSlots + 1, snapshot.begin());
		grids.push_back(snapshot);
		renders.push_back(out);
	}

	// And the audible form of the same claim, which is what was heard to fail: the sound must EVOLVE.
	// Generations 0, 1 and 2 are three different places and must render three different sounds.
	for (size_t a = 0; a < 3; ++a) {
		for (size_t b = a + 1; b < 3; ++b) {
			CAPTURE(a);
			CAPTURE(b);
			CHECK(maxAbsDiff(renders[a], renders[b]) > 0.01f);
		}
	}
	// THE FOURTH RETURNS TO THE FIRST'S POSITION, so its WAVEFORM must be the first's again — which
	// says the differences above are the character and not merely the passage of time.
	//
	// Compared as GRIDS rather than as renders, and that distinction is the correction to a first
	// draft of this case: the two renders are four blocks apart, so the phase accumulator and the
	// noise stream have both moved on and the buffers differ by most of full scale while playing
	// the identical waveform. The grid is the waveform; the buffer is the waveform sampled from
	// wherever the voice happens to have got to.
	CHECK(grids[0] == grids[3]);
	CHECK_FALSE(grids[0] == grids[1]);
	CHECK_FALSE(grids[1] == grids[2]);
}

TEST_CASE("the shared smooth source: every voice's vowel bake is the bake that reads its own, to the bit (backlog PB)") {
	// THE BANK READS THE SMOOTH SOURCE ONCE PER BUILD and every voice baking that build's vowel reads it from the table
	// (FlagshipSlotSmooth). The claim is that this changes no bit: each voice's grid, as the character moves under held
	// notes, is the grid an independent bake computes reading the smooth source itself (expectedVoiceGrid: the recipe
	// re-prepared and evaluated slot by slot, no share anywhere on its road). Asserted on the grid the sample loop
	// loads, at two notes at once, through four rebuilds, and per ear.
	//
	// SMOOTH VOWEL COUNTRY, which is what Book of Dreams stands in: the vowel audible, the recipe operated and smoothed,
	// and feedback in it — the share is read only there, so a position without smoothness would make this vacuous.
	const auto smoothVowel = [](const VoxCharacter& v, const Character& c) {
		const FlagshipRecipe r = buildFlagshipFor(c);
		return v.audible && v.presence > 0.05f && r.operated && r.smoothness > 0.05f && r.feedback > 0.0f;
	};
	std::vector<Character> characters;
	for (const uint16_t zone : { 640, 300, 900, 120, 500, 777 }) {
		for (int i = 0; i < 400000 && characters.size() < 4; i += 1) {
			const float offset = static_cast<float>(i) * 0.0137f;
			const Character c = characterFor({ zone, offset, 0.0, 0.0, 0.0, 1 });
			if (smoothVowel(buildVoxFor(c), c)) {
				characters.push_back(c);
				i += 2000; // the next one somewhere else
			}
		}
	}
	REQUIRE(characters.size() == 4);

	// NOT VACUOUS: the share is live on every one, and it MOVES between them, so a voice reading a share from another
	// build (a remembered table, the other ear's, the stale side's) would read numbers that are not this build's.
	std::vector<std::array<double, kFlagshipSlots>> shares;
	for (const Character& c : characters) {
		CharacterBank bank;
		bank.setSampleRate(kRate);
		bank.setCharacter(c);
		REQUIRE(bank.current().vox.audible);
		REQUIRE(bank.current().smooth.live);
		shares.push_back(bank.current().smooth.at);
	}
	for (size_t a = 0; a + 1 < shares.size(); ++a) {
		CHECK_FALSE(shares[a] == shares[a + 1]);
	}

	const auto requireGrid = [](const float* played, const Grid& want) {
		REQUIRE(played != nullptr);
		for (int j = 0; j <= kFlagshipSlots; ++j) {
			CAPTURE(j);
			REQUIRE(played[j] == want[static_cast<size_t>(j)]);
		}
	};

	SUBCASE("mono: two held notes through four rebuilds") {
		CharacterBank bank;
		bank.setSampleRate(kRate);
		bank.setCharacter(characters[0]);
		constexpr double kNotes[] = { 130.81, 196.0 };
		OscVoice voices[2];
		for (int v = 0; v < 2; ++v) {
			voices[v].setCharacterBank(&bank);
			voices[v].setSampleRate(kRate);
			voices[v].setFrequency(kNotes[v]);
		}
		for (size_t gen = 0; gen < characters.size(); ++gen) {
			CAPTURE(gen);
			if (gen > 0) {
				bank.setCharacter(characters[gen]);
			}
			Buffer out {};
			for (OscVoice& voice : voices) {
				voice.render(out.data(), kRenderSamples);
			}
			bank.advance(kRenderSamples);
			for (int v = 0; v < 2; ++v) {
				CAPTURE(v);
				REQUIRE_FALSE(voices[v].isFadingForTest());
				requireGrid(voices[v].freshGridForTest(0), expectedVoiceGrid(characters[gen], kNotes[v]));
			}
		}
	}

	SUBCASE("stereo: each ear's bake reads its own ear's share") {
		CharacterBank bank;
		bank.setSampleRate(kRate);
		bank.setCharacterStereo(characters[1], characters[2]);
		REQUIRE(&bank.current(0) != &bank.current(1));
		OscVoice voice;
		voice.setCharacterBank(&bank);
		voice.setSampleRate(kRate);
		voice.setFrequency(196.0);
		Buffer outL {};
		Buffer outR {};
		voice.renderStereo(outL.data(), outR.data(), kRenderSamples);
		bank.advance(kRenderSamples);
		requireGrid(voice.freshGridForTest(0), expectedVoiceGrid(characters[1], 196.0));
		requireGrid(voice.freshGridForTest(1), expectedVoiceGrid(characters[2], 196.0));
	}
}

TEST_CASE("leaving vowel country drops the vox grid on the very first sample") {
	// THE SECOND HYPOTHESIS ABOUT THE SHIPPED REGRESSION, checked rather than reasoned about.
	//
	// updateVoxGrids' vowel-free branch sets voxLive_ = false and deliberately does NOT dirty the
	// grids — that is the invariant the identity pin leans on, since dirtying would make the
	// vowel-free path do work the original never did. The worry is what that leaves behind: a voice
	// LEAVING vowel country could in principle keep stairGrid_ pointing into its own voxGrids_
	// until something else happened to dirty them, which would latch a dead vowel across terrain
	// that has none and make the silent spans far larger than the species field's quarter.
	//
	// IT CANNOT, and the reason is an ordering rather than a coincidence, so it is pinned here:
	// voxLive_ changes only inside updateVoxGrids, driven by the four vox.audible flags, which are
	// properties of prepared characters and therefore change only when the bank rebuilds — and
	// syncToBank sets gridsDirty_ AND zeroes the bake countdown on every rebuild. So the first
	// sample after a rebuild runs updateVoxGrids and then refreshGrids, in that order, with the
	// fresh voxLive_ in hand.
	const float vocal = findVoxOffset(512, [](const VoxCharacter& v, const Character&) {
		return v.audible && v.presence > 0.5f;
	});
	const Character vocalCharacter = characterFor({ 512, vocal, 0.0, 0.0, 0.0, 1 });
	const Character freeCharacter = pin::characterOf(pin::kSpots[0]);
	REQUIRE(buildVoxFor(vocalCharacter).audible);
	REQUIRE_FALSE(buildVoxFor(freeCharacter).audible);

	CharacterBank bank;
	bank.setSampleRate(kRate);
	bank.setCharacter(vocalCharacter);
	OscVoice voice;
	voice.setCharacterBank(&bank);
	voice.setSampleRate(kRate);
	voice.setFrequency(130.81);
	Buffer out {};
	voice.render(out.data(), 256);
	bank.advance(256);
	// In vowel country the voice is on its OWN grid, not the bank's.
	REQUIRE(voice.freshGridForTest(0) != nullptr);
	REQUIRE(bank.current().gridFor(voice.mipForTest()) == nullptr);

	// Now leave it, and look after ONE host sample — the tightest window there is.
	bank.setCharacter(freeCharacter);
	voice.render(out.data(), 1);
	bank.advance(1);
	const float* bankGrid = bank.current().gridFor(voice.mipForTest());
	REQUIRE(bankGrid != nullptr);
	CHECK(voice.freshGridForTest(0) == bankGrid);

	// And it stays there — the vowel does not creep back when the countdown next fires.
	voice.render(out.data(), 512);
	bank.advance(512);
	CHECK(voice.freshGridForTest(0) == bank.current().gridFor(voice.mipForTest()));
}

TEST_CASE("a parked note bakes ONCE — the cache is what makes the vox road affordable") {
	// THE COST OF THE VOX ROAD IS ONE BAKE TIMES HOW OFTEN, and the second factor is the whole of
	// what the cache changed. Before it, a voice rebaked every 32 host samples forever: sixteen
	// voices in feedback country measured about three quarters of a chunk's budget, permanently,
	// for a note that was not moving. The key is (which prepared character, which note to 2.34
	// cents), so a standing note bakes when it arrives and never again.
	const float offset = findVoxOffset(512, [](const VoxCharacter& v, const Character&) {
		return v.audible && v.presence > 0.5f;
	});
	const Character c = characterFor({ 512, offset, 0.0, 0.0, 0.0, 1 });
	REQUIRE(buildVoxFor(c).audible);

	CharacterBank bank;
	bank.setSampleRate(kRate);
	bank.setCharacter(c);
	OscVoice voice;
	voice.setCharacterBank(&bank);
	voice.setSampleRate(kRate);
	voice.setFrequency(130.81); // C3, the firmware's own reference
	Buffer out {};
	for (int block = 0; block < 40; ++block) {
		voice.render(out.data(), 128);
		bank.advance(128);
	}
	// 5120 host samples — a hundred and sixty bake intervals, and one bake.
	CHECK(voice.voxBakesForTest() == 1);

	// A CHARACTER CHANGE COSTS A BAKE, and only one: the incoming side. The outgoing side was baked
	// while it was fresh and the swap banks it, so its key still matches and nothing recomputes it —
	// which is also what keeps both sides of the 2 ms crossfade genuinely vocal rather than making
	// every character change an audible vowel swell.
	const float other = findVoxOffset(777, [](const VoxCharacter& v, const Character&) {
		return v.audible && v.presence > 0.5f;
	});
	const uint32_t before = voice.voxBakesForTest();
	bank.setCharacter(characterFor({ 777, other, 0.0, 0.0, 0.0, 1 }));
	voice.render(out.data(), 128);
	bank.advance(128);
	CHECK(voice.voxBakesForTest() - before == 1);
	// And both sides of the fade are live bakes of their own characters, not one grid read twice.
	CHECK(voice.freshGridForTest(0) != voice.staleGridForTest(0));

	// A NOTE THAT MOVES BY LESS THAN THE KEY'S STEP IS THE SAME NOTE. 2.34 cents is chosen so that
	// no pulse edge can move more than half a grid slot, so there is nothing to rebake for.
	const uint32_t settled = voice.voxBakesForTest();
	for (int block = 0; block < 20; ++block) {
		voice.render(out.data(), 128);
		bank.advance(128);
	}
	CHECK(voice.voxBakesForTest() == settled);

	// A NOTE THAT QUANTISES TO THE SAME KEY IS THE SAME NOTE. The step is 2.34 cents, chosen so no
	// pulse edge can move more than half a grid slot within one — so there is nothing to rebake for.
	//
	// The frequency is picked BY ITS KEY rather than by a fraction of a step, and that distinction
	// is the correction to a first draft of this case: a rounded key has boundaries, so a move of
	// half a step lands in the next bucket about half the time. The contract is "same key, same
	// bake", not "small move, no bake", and testing the latter tests the position of the boundary.
	const double base = 130.81;
	REQUIRE(voxBakeNoteKey(base) == voxBakeNoteKey(voice.frequencyHz()));
	double sameKey = base;
	for (int i = 1; i < 64; ++i) {
		const double candidate = base * std::exp2(static_cast<double>(i) * 1e-4);
		if (voxBakeNoteKey(candidate) != voxBakeNoteKey(base)) {
			break;
		}
		sameKey = candidate;
	}
	REQUIRE(sameKey != base); // the bucket has width, or the loop below proves nothing
	voice.setFrequency(sameKey);
	voice.render(out.data(), 128);
	bank.advance(128);
	CHECK(voice.voxBakesForTest() == settled);

	// And a note in a different bucket is a different note.
	double otherKey = base;
	for (int i = 1; i < 4096; ++i) {
		otherKey = base * std::exp2(static_cast<double>(i) * 1e-4);
		if (voxBakeNoteKey(otherKey) != voxBakeNoteKey(base)) {
			break;
		}
	}
	REQUIRE(voxBakeNoteKey(otherKey) != voxBakeNoteKey(base));
	voice.setFrequency(otherKey);
	voice.render(out.data(), 128);
	bank.advance(128);
	CHECK(voice.voxBakesForTest() == settled + 1);
}

TEST_CASE("MEASURE: how often a voice actually bakes, parked and gliding") {
	// The number the cost question turns on, reported rather than predicted. A glide is the case the
	// key alone cannot bound — three octaves in a quarter second crosses a 2.34-cent step every
	// eighth of a host sample — so the rate cap is what holds it, and this is where its effect is
	// visible.
	const float offset = findVoxOffset(777, [](const VoxCharacter& v, const Character& c) {
		const FlagshipRecipe r = buildFlagshipFor(c);
		return v.audible && v.presence > 0.4f && r.feedback > 0.3f && r.quantize > 0.0f;
	});
	const Character c = characterFor({ 777, offset, 0.0, 0.0, 0.0, 1 });
	REQUIRE(buildVoxFor(c).audible);
	constexpr int kBlocks = 24;
	constexpr int kBlockSize = 128;

	auto bakesFor = [&c](double glideSeconds, double target) {
		CharacterBank bank;
		bank.setSampleRate(kRate);
		bank.setCharacter(c);
		OscVoice voice;
		voice.setCharacterBank(&bank);
		voice.setSampleRate(kRate);
		voice.setPitch(40.0);
		voice.setGlideSeconds(glideSeconds);
		voice.glideToPitch(target);
		Buffer out {};
		for (int block = 0; block < kBlocks; ++block) {
			voice.render(out.data(), kBlockSize);
			bank.advance(kBlockSize);
		}
		return voice.voxBakesForTest();
	};

	const uint32_t parked = bakesFor(0.0, 40.0);
	const uint32_t slow = bakesFor(0.5, 44.0);
	const uint32_t fast = bakesFor(0.25, 76.0);
	const int samples = kBlocks * kBlockSize;
	const int intervals = samples * 4 / kVoxBakeIntervalSamples;
	std::printf("[vox bakes] over %d host samples (%d bake intervals): parked %u | slow glide %u"
	            " | three-octave glide %u\n",
	            samples, intervals, parked, slow, fast);

	// A PARKED NOTE BAKES ONCE, whatever the block count.
	CHECK(parked == 1);
	// A slow glide bakes when the note has genuinely moved, and less often than the cap allows.
	CHECK(slow > parked);
	CHECK(slow <= static_cast<uint32_t>(intervals));
	// AND THE CAP HOLDS THE FAST ONE. Without it the key would fire on almost every sample; with
	// it, a glide can never bake more often than one interval, whatever it is doing.
	CHECK(fast <= static_cast<uint32_t>(intervals));
}

TEST_CASE("MEASURE: what a voice pays per block to have a throat") {
	// REPORTED, and bounded only against pathology. The bake is once per chunk per voice (V22c(e)),
	// so the budget is a chunk: at 48 kHz, 32 host samples is 667 µs, and a sixteen-voice patch has
	// to fit sixteen of these inside it. The number below is what says whether that is a sentence
	// worth writing or a problem to bring back to the design.
	using clock = std::chrono::steady_clock;
	const float offset = findVoxOffset(777, [](const VoxCharacter& v, const Character& c) {
		const FlagshipRecipe r = buildFlagshipFor(c);
		return v.audible && v.presence > 0.4f && r.feedback > 0.3f && r.quantize > 0.0f;
	});
	const Character worst = characterFor({ 777, offset, 0.0, 0.0, 0.0, 1 });
	const float quietOffset = findVoxOffset(777, [](const VoxCharacter& v, const Character& c) {
		const FlagshipRecipe r = buildFlagshipFor(c);
		return v.audible && v.presence > 0.4f && r.feedback == 0.0f && r.quantize > 0.0f;
	});
	const Character typical = characterFor({ 777, quietOffset, 0.0, 0.0, 0.0, 1 });

	auto bakeCost = [](const Character& c, int mip) {
		const VoxCharacter vox = buildVoxFor(c);
		const FlagshipRecipe recipe = buildFlagshipFor(c);
		VoxCycle cycle;
		constexpr int kRounds = 300;
		const auto start = clock::now();
		double sink = 0.0;
		for (int i = 0; i < kRounds; ++i) {
			bakeVoxCycle(vox.params, 147.0 + static_cast<double>(i) * 1e-6, cycle);
			FlagshipRecipe r = recipe;
			const FlagshipSource source { &cycle, vox.presence };
			prepareFlagshipRecipe(r, source);
			Grid grid {};
			bakeFlagshipGridAtMip(r, source, mip, grid.data());
			sink += static_cast<double>(grid[7]);
		}
		const double us =
		    std::chrono::duration<double, std::micro>(clock::now() - start).count() / kRounds;
		CHECK(std::isfinite(sink));
		return us;
	};

	const double typicalUs = bakeCost(typical, 0);
	const double worstUs = bakeCost(worst, 0);
	std::printf("[vox bake] per voice per chunk: no feedback %.3f us | with feedback %.3f us"
	            " | chunk budget at 48k/32 = 667 us\n",
	            typicalUs, worstUs);
	std::printf("[vox bake] sixteen voices of the worst case: %.0f us of a 667 us chunk (%.0f%%)\n",
	            16.0 * worstUs, 100.0 * 16.0 * worstUs / 667.0);
	// REPORTED AND ESCALATED, not silently accepted. On a quiet machine the bake measures about
	// 12 us without feedback and 32 us with; sixteen voices of the worst case is then around 76% of
	// a chunk's budget, and the character is SHARED, so that is a place a whole patch can sit rather
	// than a corner one voice can reach. V22b(e) named the answer in advance — "bake every block
	// first: correctness and simplicity; slew-triggered rebake is a measured optimization if
	// profiling ever asks" — and profiling is asking. How to answer it is a design decision, not
	// this file's.
	//
	// THE CEILING IS THE HOUSE ONE: generous, so that it catches a structural regression rather than
	// a busy machine. These same bakes measure 58 and 136 us on a box under load average 13, which
	// is four times the quiet reading and says exactly how little a tight bound would be worth. A
	// number that blows this is a bug, not a tuning question — the same standing test_flagship's
	// per-build ceilings carry.
	CHECK(worstUs < 600.0);
}

TEST_CASE("MEASURE: what a moving puck in vowel country costs, at sixteen voices") {
	// THE COST THE BAKE KEY LEAVES ON THE TABLE, measured rather than predicted. The key is
	// {build serial, note}, so a PARKED note bakes once — but every character build mints a new
	// serial and therefore invalidates EVERY voice's bake at once. Gesturing is exactly the case
	// where the character rebuilds continuously, so it is the case the key does not help.
	//
	// WHAT BOUNDS IT, and why this is not unbounded: the bank refuses to rebuild while a fade guard
	// is running, so builds land at most once per (fade + carry) host samples — about 480 a second
	// at 48 kHz however fast the hand moves. The mouse cannot outrun that, which is what turns "a
	// drag" into a bounded rate rather than a race.
	const float vocal = findVoxOffset(777, [](const VoxCharacter& v, const Character& c) {
		const FlagshipRecipe r = buildFlagshipFor(c);
		return v.audible && v.presence > 0.4f && r.feedback > 0.3f && r.quantize > 0.0f;
	});
	const Character freeCharacter = pin::characterOf(pin::kSpots[0]);
	REQUIRE_FALSE(buildVoxFor(freeCharacter).audible);

	constexpr int kVoices = 16;
	constexpr int kBlock = 64;
	constexpr int kSeconds = 1;
	constexpr int kTotal = static_cast<int>(kRate) * kSeconds;

	// A DRAG, as the engine sees one: the character rebuilds every block at a slightly different
	// place, which is what a puck being moved produces. Two places alternating would let the bank's
	// dedup drop half of them, so the offset walks.
	auto drag = [&](bool vocalCountry) {
		CharacterBank bank;
		bank.setSampleRate(kRate);
		std::vector<OscVoice> voices(kVoices);
		for (int v = 0; v < kVoices; ++v) {
			voices[static_cast<size_t>(v)].setCharacterBank(&bank);
			voices[static_cast<size_t>(v)].setSampleRate(kRate);
			// Spread notes, so the voices are not one note sixteen times.
			voices[static_cast<size_t>(v)].setFrequency(110.0 * std::exp2(v / 12.0));
		}
		Buffer out {};
		const auto start = std::chrono::steady_clock::now();
		int done = 0;
		int step = 0;
		while (done < kTotal) {
			Character c = vocalCountry
			                  ? characterFor({ 777, vocal + static_cast<float>(step) * 1e-4f, 0.0,
				                                 0.0, 0.0, 1 })
			                  : freeCharacter;
			if (!vocalCountry) {
				c.sounds[0].travelX = static_cast<double>(step) * 1e-5;
			}
			bank.setCharacter(c);
			++step;
			for (int v = 0; v < kVoices; ++v) {
				voices[static_cast<size_t>(v)].render(out.data(), kBlock);
			}
			bank.advance(kBlock);
			done += kBlock;
		}
		const double micros =
		    std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start)
		        .count();
		uint32_t bakes = 0;
		for (int v = 0; v < kVoices; ++v) {
			bakes += voices[static_cast<size_t>(v)].voxBakesForTest();
		}
		return std::pair<uint32_t, double> { bakes, micros };
	};

	const auto [vocalBakes, vocalUs] = drag(true);
	const auto [freeBakes, freeUs] = drag(false);
	const double audioUs = 1e6 * static_cast<double>(kSeconds);
	std::printf("[vox drag] one second of audio, %d voices, character rebuilding every %d samples:\n"
	            "           vowel country  %u bakes  %.1f ms  (%.1f%% of a core)\n"
	            "           vowel-free     %u bakes  %.1f ms  (%.1f%% of a core)\n"
	            "           the vowel's share: %.1f%% of a core\n",
	            kVoices, kBlock, vocalBakes, vocalUs / 1000.0, 100.0 * vocalUs / audioUs, freeBakes,
	            freeUs / 1000.0, 100.0 * freeUs / audioUs, 100.0 * (vocalUs - freeUs) / audioUs);

	// VOWEL-FREE COUNTRY BAKES EXACTLY NOTHING, however hard the puck is dragged — updateVoxGrids
	// returns on four flag loads. That is the claim the whole identity rests on, restated as a cost.
	CHECK(freeBakes == 0);
	// And the rebuild rate really is guard-bounded rather than block-bounded: sixteen voices over one
	// second cannot exceed sixteen times the ~480 builds a second the guard allows.
	CHECK(vocalBakes > 0);
	CHECK(vocalBakes < static_cast<uint32_t>(kVoices) * 600u);
	// Pathology only, and generous for the same reason the per-bake ceiling is: this measures four
	// times slower on a loaded box than an idle one, so a tight bound here would be a flaky test
	// rather than a guard. A real regression blows this by an order of magnitude.
	CHECK(vocalUs < 4.0 * audioUs);
}

TEST_CASE("MEASURE: how often a rebuild leaves the vowel where it was") {
	// THE NUMBER BEFORE THE OPTIMISATION, not after it. The lever on the table is that a character
	// rebuild need not invalidate a voice whose note has not moved AND whose new character is
	// vox-identical at that note. Whether that is worth building depends entirely on how often a
	// moving puck actually leaves the vowel alone, and that is measurable without building anything.
	//
	// TWO DEFINITIONS, because they give very different answers and the difference IS the finding.
	// BIT-IDENTICAL is what a cheap guard could test — compare the new VoxCharacter against the one
	// the bake was made from and skip if equal. WITHIN TOLERANCE is what an optimisation would have
	// to test to be worth anything, and it needs a decision about how far a vowel may drift unheard,
	// which is a sound question rather than an implementation one.
	//
	// The tolerance below is the SAME reasoning the note key already uses: a formant frequency
	// moving by a relative 1/512 cannot shift any pulse edge more than half a grid slot, for any
	// formant and any note. Gains and presence are amplitudes into a ±1 waveform, so 1e-3 of one.
	const float vocal = findVoxOffset(777, [](const VoxCharacter& v, const Character&) {
		return v.audible && v.presence > 0.4f;
	});

	auto sameWithin = [](const VoxCharacter& a, const VoxCharacter& b) {
		if (a.audible != b.audible) {
			return false;
		}
		if (std::abs(a.presence - b.presence) > 1e-3f) {
			return false;
		}
		for (size_t f = 0; f < kVoxNumFormants; ++f) {
			const float lo = std::min(a.params.formant[f].frequencyHz, b.params.formant[f].frequencyHz);
			const float hi = std::max(a.params.formant[f].frequencyHz, b.params.formant[f].frequencyHz);
			if (lo <= 0.0f || hi / lo - 1.0f > 1.0f / 512.0f) {
				return false;
			}
			for (size_t k = 0; k < kVoxMaxPulses; ++k) {
				if (std::abs(a.params.formant[f].pulseGain[k] - b.params.formant[f].pulseGain[k])
				    > 1e-3f) {
					return false;
				}
			}
		}
		return true;
	};

	// Drag speeds, in home units per second, against a 750-rebuild-per-second tick. The reachable
	// band is three home units wide, so "a second to cross the chart" is about 3.0.
	std::printf("[vox drift] rebuilds that leave the vowel where it was, over 750 steps:\n");
	for (const double perSecond : { 0.1, 0.5, 3.0, 12.0 }) {
		const double step = perSecond / 750.0;
		int bitIdentical = 0;
		int withinTolerance = 0;
		int audible = 0;
		int comparable = 0;
		VoxCharacter previous {};
		bool havePrevious = false;
		for (int i = 0; i < 750; ++i) {
			const Character c = characterFor(
			    { 777, vocal + static_cast<float>(static_cast<double>(i) * step), 0.0, 0.0, 0.0, 1 });
			const VoxCharacter here = buildVoxFor(c);
			audible += here.audible ? 1 : 0;
			// ONLY AUDIBLE-TO-AUDIBLE TRANSITIONS COUNT, and a first draft of this case did not
			// restrict them — which made the number rise with drag speed, exactly backwards. The
			// reason is that a fast drag LEAVES vowel country, buildVoxFor returns a default
			// VoxCharacter there, and consecutive vowel-free steps are trivially bit-identical to
			// each other. That is not a rebake saved: a voice outside vowel country never bakes at
			// all. The question is only ever about steps where a bake would really happen.
			if (havePrevious && here.audible && previous.audible) {
				++comparable;
				bitIdentical += (here.params == previous.params && here.presence == previous.presence)
				                    ? 1
				                    : 0;
				withinTolerance += sameWithin(here, previous) ? 1 : 0;
			}
			previous = here;
			havePrevious = true;
		}
		REQUIRE(comparable > 0);
		std::printf("           %5.1f home units/sec:  bit-identical %3d%%   within tolerance %3d%%"
		            "   (%d of 750 steps in vowel country)\n",
		            perSecond, bitIdentical * 100 / comparable, withinTolerance * 100 / comparable,
		            audible);
	}
	CHECK(true);
}
