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

#include "plugin/device_registry.h"
#include "plugin/audition.h"

#include "plugin/fx_chain.h"
#include "plugin/mod_rack.h"
#include "rack/rack_device.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <string_view>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <locale.h>

namespace feathers {

namespace {

bool isSeparator(char c) {
	return c == ',' || c == ' ' || c == '\t' || c == ';';
}

/// Locale-independent, correctly-rounded number parse (strtod_l with the "C"
/// locale). Replaces std::from_chars(FP), which the macOS 26 SDK marks as
/// introduced in macOS 26.0 and so cannot be used when targeting a 15.x host.
/// Reproduces from_chars' contract: consumes the longest valid prefix of `s`,
/// reports the chars consumed to `consumed` (0 if nothing parsed), and leaves
/// the caller to reject junk instead of reading it as zero — a comma-decimal
/// locale can never reinterpret '.' as a grouping separator. `s` must be a
/// sub-view of a NUL-terminated buffer (every caller passes getenv text), so
/// strtod_l cannot overrun the view. Env parsing is init-time, not the audio
/// thread, so the locale allocation is acceptable.
bool parseNumber(std::string_view s, size_t& consumed, double& out) {
	const char* first = s.data();
	char* after = nullptr;
#if defined(_WIN32)
	// The UCRT spells the same thing _create_locale / _strtod_l / _free_locale.
	const _locale_t loc = _create_locale(LC_NUMERIC, "C");
	const double value = loc != nullptr ? _strtod_l(first, &after, loc) : std::strtod(first, &after);
	if (loc != nullptr) {
		_free_locale(loc);
	}
#else
	locale_t loc = newlocale(LC_NUMERIC_MASK, "C", static_cast<locale_t>(0));
	if (loc == static_cast<locale_t>(0)) {
		loc = LC_GLOBAL_LOCALE;
	}
	const double value = strtod_l(first, &after, loc);
	if (loc != LC_GLOBAL_LOCALE) {
		freelocale(loc);
	}
#endif
	if (after == first) {
		consumed = 0;
		return false;
	}
	consumed = static_cast<size_t>(after - first);
	out = value;
	return true;
}

bool parseNumber(std::string_view s, double& out) {
	size_t consumed = 0;
	return parseNumber(s, consumed, out);
}

} // namespace

SliceGenerator parseOrientation(std::string_view text) {
	// The variable's legacy format was "aperture,angle0,angle1,…" — seven numbers for a decode that
	// read one. Under M the pin is one turn. ONE number is the turn; TWO OR MORE is read as the legacy
	// layout, whose turn is the SECOND field (angle 0 — the aperture rides first and is unread). The
	// env var was declared non-migrating in writing, so this reading is a courtesy, not a contract — and
	// the announcement below says what was actually read either way.
	float values[2] = { 0.0f, 0.0f };
	size_t fields = 0;

	size_t at = 0;
	while (at < text.size() && fields < 2) {
		while (at < text.size() && isSeparator(text[at])) {
			++at;
		}
		if (at >= text.size()) {
			break;
		}
		float value = 0.0f;
		// Locale-independent C-locale parse (parseNumber/strtod_l): a comma-decimal machine cannot
		// reinterpret the '.' as a grouping separator and read one number where several were meant,
		// and a trailing unit or typo ends the parse instead of quietly becoming a zero — the same
		// invariants from_chars gave us (it is unavailable for floating point before macOS 26 in the
		// current SDK, so we target a 15.x host).
		size_t consumed = 0;
		double parsed = 0.0;
		if (!parseNumber(std::string_view(text.data() + at, text.size() - at), consumed, parsed)) {
			break;
		}
		at += consumed;
		value = static_cast<float>(parsed);
		values[fields++] = value;
	}

	SliceGenerator generator;
	generator.rotate = fields >= 2 ? values[1] : values[0];
	return generator;
}

SliceOrientationPin orientationFromEnvironment() {
	const char* text = std::getenv("FEATHERS_AXIS_ROTATION");
	if (text == nullptr || *text == '\0') {
		// Said out loud rather than passed over. "Per-sound" and "the variable did not cross the
		// flatpak boundary" look identical from the outside unless the ordinary case announces itself
		// too, and that boundary is the single most likely thing to go wrong with this handle.
		std::fprintf(stderr, "[feathers] slice orientation: per sound"
		                     " (FEATHERS_AXIS_ROTATION unset)\n");
		return {};
	}
	const SliceGenerator generator = parseOrientation(text);
	std::fprintf(stderr, "[feathers] slice orientation: PINNED, rotate %g (from \"%s\")\n",
	             static_cast<double>(generator.rotate), text);
	return { true, generator };
}

double falloffPowerFromEnvironment() {
	const char* text = std::getenv("FEATHERS_FALLOFF_POWER");
	if (text == nullptr || *text == '\0') {
		std::fprintf(stderr, "[feathers] blend falloff: power %g (FEATHERS_FALLOFF_POWER unset)\n",
		             kFalloffPowerA);
		return kFalloffPowerA;
	}

	// parseNumber rather than atof, for the same reason parseOrientation uses it: junk should be
	// REJECTED rather than read as zero, and a zero exponent is a constellation with no geometry left
	// in it — every weight 1 whatever the distance.
	double value = 0.0;
	const std::string_view view { text };
	if (!parseNumber(view, value) || !(value > 0.0) || !std::isfinite(value)) {
		std::fprintf(stderr,
		             "[feathers] blend falloff: power %g — could not read \"%s\" as a positive number\n",
		             kFalloffPowerA, text);
		return kFalloffPowerA;
	}
	// §7.1d SAID this was inaudible below three sounds, and it was right for the build it was written
	// for: Path governed two sounds then, and the exponent reached nothing that was played. §2c made the
	// rule uniform, so two sounds are weighted like every other count and the exponent is audible there
	// too — only parity mode still goes by Path alone. The line said otherwise until a review caught
	// it, which is what a stale comment does when it is also a printed string: it was telling somebody
	// the control they were reaching for does nothing.
	std::fprintf(stderr,
	             "[feathers] blend falloff: power %g (from \"%s\") — audible at every sound count,"
	             " except in parity mode\n",
	             value, text);
	return value;
}

namespace {

/// One non-negative number from the environment, or `fallback` with a line saying why.
///
/// Shared by both scales rather than written twice, because two readers of the same shape are two
/// chances to spell the announcement differently and one chance to forget a guard.
double scaleFromEnvironment(const char* name, double fallback, bool* wasSet) {
	const char* text = std::getenv(name);
	if (text == nullptr || *text == '\0') {
		return fallback;
	}
	// Set-but-refused is not the same as unset, and the announcement below has to be able to tell them
	// apart: a line reading "unset" to somebody who has just exported the variable is the log telling
	// them their export failed when what failed was their value.
	*wasSet = true;
	double value = 0.0;
	const std::string_view view { text };
	if (!parseNumber(view, value) || !(value >= 0.0) || !std::isfinite(value)) {
		std::fprintf(stderr, "[feathers] %s: ignored — \"%s\" is not a non-negative number\n", name, text);
		return fallback;
	}
	return value;
}

} // namespace

void blueShiftScalesFromEnvironment(double* overshoot, double* doppler) {
	bool anySet = false;
	const double o = scaleFromEnvironment("FEATHERS_BLUE_OVERSHOOT", kBlueShiftScaleOn, &anySet);
	const double d = scaleFromEnvironment("FEATHERS_BLUE_DOPPLER", kBlueShiftScaleOn, &anySet);
	if (overshoot != nullptr) {
		*overshoot = o;
	}
	if (doppler != nullptr) {
		*doppler = d;
	}
	// Announced whichever way it went, like the orientation: "off" and "the variable did not cross the
	// flatpak boundary" are indistinguishable from outside unless the ordinary case says so too. And
	// the caveat is worth printing, because a listener who turns these up with two sounds will hear
	// nothing and reasonably conclude the feature is broken.
	if (o == 0.0 && d == 0.0) {
		std::fprintf(stderr, "[feathers] blue shift: OFF by request — the engine is the one without it\n");
		return;
	}
	// The caveat is printed with the values because it is the difference between "nothing happens" and
	// "nothing happens YET": at rest every term is zero by construction, so a patch nobody is moving
	// sounds exactly as it did before any of this existed.
	//
	// The "three sounds or more" caveat this line used to carry is GONE, and its going is the point of
	// §2c: one rule applies at every count, so blue shift reaches two sounds like it reaches eight.
	// Left as a note rather than deleted silently, because an audition instruction written against the
	// old build would now be telling somebody to do unnecessary work — and the sentence above this one
	// said the superseded thing for a commit longer than the note did, which is how a comment gets to
	// contradict its own next paragraph.
	std::fprintf(stderr,
	             "[feathers] blue shift: overshoot x%g, doppler x%g%s — heard while the puck MOVES,"
	             " at every sound count\n",
	             o, d, anySet ? "" : " (default)");
}

bool v1CrossfadeFromEnvironment() {
	const char* text = std::getenv("FEATHERS_V1_CROSSFADE");
	const bool on = text != nullptr && *text != '\0' && std::strcmp(text, "0") != 0;
	if (on) {
		// Announced loudly, because it is the one setting here that makes the plugin deliberately play
		// an OLDER engine: two sounds take the verbatim v1 crossfade, blue shift never reaches them, and
		// a listener who has forgotten it is set would be comparing against the wrong thing.
		std::fprintf(stderr, "[feathers] PARITY MODE: two sounds take the v1 crossfade"
		                     " (FEATHERS_V1_CROSSFADE set) — a reference, not a feature\n");
	}
	return on;
}

namespace {

/// A tag rather than a length, so a stream that ends where the parameters ended reads as "no
/// orientation here" rather than as four bytes of something else.
constexpr uint8_t kOrientationTag[4] = { 'F', 'S', 'L', '1' };

/// The drift block's own tag, for the same reason the orientation has one: a reader that finds
/// something else here has been handed a stream it does not understand, and should say so rather
/// than interpret whatever comes next as coordinates.
// 'FST2' SINCE 2026-09-21: each slot carries its polar offset beside its place, and the block says HOW
// MANY doubles a slot has, so the next member costs no tag. 'FST1' — two doubles a slot, no count — is
// still READ, because it is three lines and the alternative is an open project refusing to load.
constexpr uint8_t kTravelTag[4] = { 'F', 'S', 'T', '2' };
constexpr uint8_t kTravelTagV1[4] = { 'F', 'S', 'T', '1' };
constexpr size_t kTravelValuesPerSlot = 4; // x, y, orbit radius, orbit theta

/// The per-sound plane block's tag.
constexpr uint8_t kPlanesTag[4] = { 'F', 'S', 'P', '1' };

/// The banked-residue block's tag. Absent from every patch that has never rotated a drifted sound,
/// which is most of them and all of the old ones.
///
/// FSR2 SINCE V-C2, AND THE BUMP WAS FORCED RATHER THAN CHOSEN. This block's payload is split by the
/// counts in its header — one per engine — so a third engine's rows need a third count, and the
/// number of counts is exactly what a tag fixes. An FSR1 reader handed a four-count header would take
/// the vox count for the first half of a double and walk the rest of the stream at the wrong offset,
/// which is the silent misparse this codebase refuses on principle: it would corrupt not only the
/// residue but every block after it, since each one reads where the last one stopped.
///
/// SO THE TRADE IS STATED PLAINLY. Builds shipped before this refuse an FSR2 patch WHOLE and
/// deterministically — the tag compare fails, the load returns false, and the host keeps the patch it
/// had rather than a wrong one. That is the V2 posture ratified for the stance block and it
/// carries the same one-way door: install this build before saving with it. Reading OLD patches is
/// forever — the loader below takes FSR1's three counts and gives vox the zero rows that are the
/// honest reading of "this sound never rotated a vox character", because until V-C2 it could not.
///
/// FSR3 (2026-09-25) is the same bump for the same reason: the Resonate engine's rows joined the fold,
/// so the header carries a FIFTH count, and an FSR2 patch reads with the zero weave rows that are what
/// it sounded like.
///
/// FSR4 (2026-10-08), once more: the additive engine's partial rows joined the fold, a SIXTH count, and
/// an FSR3 patch reads with the zero partial rows that are what it sounded like.
constexpr uint8_t kResidueTag[4] = { 'F', 'S', 'R', '4' };

/// The tag this block wore before the partial rows joined. Still read.
constexpr uint8_t kResidueTagV3[4] = { 'F', 'S', 'R', '3' };

/// The tag this block wore before the weave rows joined. Still read, forever.
constexpr uint8_t kResidueTagV2[4] = { 'F', 'S', 'R', '2' };

/// The tag this block wore before vox joined the fold. Still read, forever.
constexpr uint8_t kResidueTagV1[4] = { 'F', 'S', 'R', '1' };

/// Gamma's unbounded half, the block after the residue. Same A9 shape as every one above it: a patch
/// written before the vernier existed ends before this tag, reads as absent, and keeps the zero base
/// that reproduces it exactly.
constexpr uint8_t kGammaTag[4] = { 'F', 'S', 'G', '1' };

/// The stances each sound remembers — the sixth and last block, after the gamma one.
constexpr uint8_t kVariationsTag[4] = { 'F', 'S', 'V', '1' }; // legacy: places only, read forever
constexpr uint8_t kVariationsTag2[4] = { 'F', 'S', 'V', '2' }; // V18: stances carry ride-along values

/// Which colour each sound wears. The eighth block, after the routings — see sound_hues.h.
constexpr uint8_t kHuesTag[4] = { 'F', 'S', 'H', '1' };
/// THE MACROS' POLARITY (backlog EG) — the last block, after the chains.
constexpr uint8_t kMacroModesTag[4] = { 'F', 'S', 'M', 'B' };
/// THE SPECTRAL DRAWER'S STORED STATE (kStateVersion 51) — the last block, after the macros; keyed (saveSpectralState).
constexpr uint8_t kSpectralTag[4] = { 'F', 'S', 'S', 'D' };
/// THE TERRAIN'S LIKES (kStateVersion 52, backlog ON) — the last block, after the spectral one; keyed (saveTerrainLikes).
constexpr uint8_t kTerrainLikesTag[4] = { 'F', 'S', 'L', 'K' };
/// WHICH PRESET THE PATCH CAME FROM (kStateVersion 53, backlog QC) — the last block, after the likes; keyed text.
constexpr uint8_t kPresetIdentityTag[4] = { 'F', 'S', 'P', 'N' };

/// BOTH CHAINS AND EVERY ROUTE — the last block, after the hues: the tag, then the rack's own file
/// (`rack/rack_state.h`). It replaces the three blocks the lane, the strip and the routings each had.
constexpr uint8_t kChainsTag[4] = { 'F', 'S', 'C', '1' };

bool writeAll(const clap_ostream* stream, const void* data, size_t size) {
	const auto* at = static_cast<const uint8_t*>(data);
	size_t left = size;
	while (left > 0) {
		const int64_t wrote = stream->write(stream, at, left);
		if (wrote <= 0) {
			return false;
		}
		at += wrote;
		left -= static_cast<size_t>(wrote);
	}
	return true;
}

/// Reads exactly `size` bytes, and distinguishes a clean end of stream from a short read. Both are
/// failures to READ, but only the first is a patch that simply predates the block.
bool readAll(const clap_istream* stream, void* data, size_t size, bool* endedCleanly) {
	auto* at = static_cast<uint8_t*>(data);
	size_t left = size;
	*endedCleanly = false;
	while (left > 0) {
		const int64_t got = stream->read(stream, at, left);
		if (got == 0) {
			*endedCleanly = (left == size);
			return false;
		}
		if (got < 0) {
			return false;
		}
		at += got;
		left -= static_cast<size_t>(got);
	}
	return true;
}

bool writeFloat(const clap_ostream* stream, float value) {
	// Written as the exact bits rather than as text, for the same reason the parameter stream writes
	// doubles that way: a patch saved on one machine has to load bit-identically on another, and an
	// orientation that shifted in the last place would move every sound in the patch a little.
	uint32_t bits = 0;
	std::memcpy(&bits, &value, sizeof(bits));
	uint8_t bytes[4];
	for (int i = 0; i < 4; ++i) {
		bytes[i] = static_cast<uint8_t>((bits >> (i * 8)) & 0xffU);
	}
	return writeAll(stream, bytes, sizeof(bytes));
}

/// Eight bytes, exact bits, little-endian — the same contract writeFloat has, at the width the value
/// actually is.
///
/// DRIFT IS STORED AS A DOUBLE AND HAS TO COME BACK AS ONE. Through a float it does not: 0.37 returns
/// as 0.37000000476837158, so the character built after a reload is not the character that was saved,
/// and A11's promise that a rewind is a stored number RESTORED stops being true the moment the number
/// goes through a file. The orientation keeps writeFloat because its generator is float all the way
/// down; this is not.
bool writeDouble(const clap_ostream* stream, double value) {
	uint64_t bits = 0;
	std::memcpy(&bits, &value, sizeof(bits));
	uint8_t bytes[8];
	for (int i = 0; i < 8; ++i) {
		bytes[i] = static_cast<uint8_t>((bits >> (i * 8)) & 0xffU);
	}
	return writeAll(stream, bytes, sizeof(bytes));
}

bool readDouble(const clap_istream* stream, double* value) {
	uint8_t bytes[8];
	bool ended = false;
	if (!readAll(stream, bytes, sizeof(bytes), &ended)) {
		return false;
	}
	uint64_t bits = 0;
	for (int i = 0; i < 8; ++i) {
		bits |= static_cast<uint64_t>(bytes[i]) << (i * 8);
	}
	std::memcpy(value, &bits, sizeof(*value));
	return true;
}

bool readFloat(const clap_istream* stream, float* value) {
	uint8_t bytes[4];
	bool ended = false;
	if (!readAll(stream, bytes, sizeof(bytes), &ended)) {
		return false;
	}
	uint32_t bits = 0;
	for (int i = 0; i < 4; ++i) {
		bits |= static_cast<uint32_t>(bytes[i]) << (i * 8);
	}
	std::memcpy(value, &bits, sizeof(*value));
	return true;
}

} // namespace

bool saveOrientation(const clap_ostream* stream, const SliceOrientationPin& pin) {
	if (stream == nullptr) {
		return false;
	}
	if (!writeAll(stream, kOrientationTag, sizeof(kOrientationTag))) {
		return false;
	}
	// Whether the plane was held by hand, written before the numbers it qualifies. An unpinned patch
	// records that it is unpinned rather than recording a plane, so a derived sound keeps deriving
	// instead of being frozen to whatever the derivation said on the day it was saved.
	if (!writeFloat(stream, pin.pinned ? 1.0f : 0.0f)) {
		return false;
	}
	// THE STREAM LAYOUT IS THE LEGACY LAYOUT, COUNT ONE: an angle count, an aperture slot, then the
	// angles. The count is what lets every build on either side of the shrink read every other's
	// patches without guessing — an old build reads count 1 and leaves its extra angles at zero
	// (which its decode never read), and this build reads an old count of 6 and keeps angle 0, which
	// IS the turn (ratio[0] = 1, the specified migration). The aperture slot is written as 0.0 and read
	// into nothing: a dead field in the format is cheaper forever than a second format.
	if (!writeFloat(stream, 1.0f)) {
		return false;
	}
	if (!writeFloat(stream, 0.0f)) { // the aperture slot, unread by M on any build that writes count 1
		return false;
	}
	if (!writeFloat(stream, pin.generator.rotate)) {
		return false;
	}
	return true;
}

bool loadOrientation(const clap_istream* stream, SliceOrientationPin* out, bool* present) {
	if (stream == nullptr || out == nullptr || present == nullptr) {
		return false;
	}
	*present = false;

	uint8_t tag[4];
	bool ended = false;
	if (!readAll(stream, tag, sizeof(tag), &ended)) {
		// A patch written before this block existed ends exactly here, which is not an error.
		return ended;
	}
	if (std::memcmp(tag, kOrientationTag, sizeof(tag)) != 0) {
		return false;
	}

	float pinned = 0.0f;
	if (!readFloat(stream, &pinned)) {
		return false;
	}

	float count = 0.0f;
	if (!readFloat(stream, &count)) {
		return false;
	}
	// VALIDATED BEFORE THE CAST, not after it. Converting a float that does not fit is undefined, and
	// on this compiler 1e30 lands on 0 — which then sails through a `> 64` test and is ACCEPTED, so the
	// loader reads no angles and leaves the stream positioned in the middle of them. Everything after
	// it is then read out of the wrong bytes. Measured, not assumed.
	if (!(count >= 0.0f) || count > 64.0f) {
		return false;
	}
	const auto written = static_cast<size_t>(count);

	SliceOrientationPin pin;
	pin.pinned = pinned != 0.0f;
	// The aperture slot: read so the stream position is right, dropped because M never reads it.
	float aperture = 0.0f;
	if (!readFloat(stream, &aperture)) {
		return false;
	}
	for (size_t k = 0; k < written; ++k) {
		float angle = 0.0f;
		if (!readFloat(stream, &angle)) {
			return false;
		}
		// Every angle is read so the stream position is right whatever the count; only angle 0 is
		// kept, because angle 0 IS the turn — the corner always spread its scalar with ratio[0] = 1,
		// so a legacy six-angle block's first angle is exactly what its hand produced. The raw-byte
		// migration cases in the bridge suite pin this reading.
		if (k == 0) {
			// UNBOUNDED IS NOT NON-FINITE, and this is the value's ONLY door — the guard on
			// `setSoundOrientation` is on the SINGULAR, and a patch takes the plural. Substituted
			// per value, not per block, so a bad angle cannot cost a good slot its turn.
			pin.generator.rotate =
			    std::isfinite(angle) ? angle : SliceOrientationPin {}.generator.rotate;
		}
	}

	*out = pin;
	*present = true;
	return true;
}

bool saveSoundTravel(const clap_ostream* stream, const SoundTravel& travel) {
	if (stream == nullptr) {
		return false;
	}
	if (!writeAll(stream, kTravelTag, sizeof(kTravelTag))) {
		return false;
	}
	// The count, written though it is a constant, so a build with more slots can read a patch from a
	// build with fewer without either of them guessing — the orientation block's own idiom.
	if (!writeFloat(stream, static_cast<float>(kMaxSounds))
	    || !writeFloat(stream, static_cast<float>(kTravelValuesPerSlot))) {
		return false;
	}
	for (size_t slot = 0; slot < kMaxSounds; ++slot) {
		if (!writeDouble(stream, travel.offset[slot].x) || !writeDouble(stream, travel.offset[slot].y)
		    || !writeDouble(stream, travel.orbit[slot].radius)
		    || !writeDouble(stream, travel.orbit[slot].theta)) {
			return false;
		}
	}
	return true;
}

bool loadSoundTravel(const clap_istream* stream, SoundTravel* out, bool* present) {
	if (stream == nullptr || out == nullptr || present == nullptr) {
		return false;
	}
	*present = false;

	uint8_t tag[4];
	bool ended = false;
	if (!readAll(stream, tag, sizeof(tag), &ended)) {
		// A patch written before drift existed ends exactly here. Not an error, and the caller keeps
		// the zero offsets that reproduce it.
		return ended;
	}
	const bool v1 = std::memcmp(tag, kTravelTagV1, sizeof(tag)) == 0;
	if (!v1 && std::memcmp(tag, kTravelTag, sizeof(tag)) != 0) {
		return false;
	}

	float count = 0.0f;
	if (!readFloat(stream, &count)) {
		return false;
	}
	// HOW MANY DOUBLES A SLOT CARRIES. 'FST1' never said, because it was always two.
	float perSlot = 2.0f;
	if (!v1 && !readFloat(stream, &perSlot)) {
		return false;
	}
	if (!(perSlot >= 2.0f) || perSlot > 64.0f) {
		return false;
	}
	const auto values = static_cast<size_t>(perSlot);
	// Validated before the cast, for the reason spelled out in loadOrientation: an out-of-range float
	// converts to whatever the hardware feels like, and the value it happens to land on can pass a
	// range check written against the RESULT.
	if (!(count >= 0.0f) || count > 64.0f) {
		return false;
	}
	const auto written = static_cast<size_t>(count);

	SoundTravel travel;
	for (size_t slot = 0; slot < written; ++slot) {
		double x = 0.0;
		double y = 0.0;
		if (!readDouble(stream, &x) || !readDouble(stream, &y)) {
			return false;
		}
		// Slots this build does not have are read and dropped rather than skipped blindly, so the
		// stream position is right whatever the counts are.
		if (slot < kMaxSounds) {
			// THE PAIR OR NEITHER, AND THE OTHER ROAD ALREADY SETTLED IT. The first version kept the
			// finite coordinate, reasoning that a bad half should not cost a good half — hunt-state
			// pointed at the singular setter, which refuses both or neither:
			//
			//     if (slot >= kMaxSounds || !isfinite(offset.x) || !isfinite(offset.y)) { return; }
			//
			// So per-component here would make the two roads answer one question differently: a
			// half-finite drift refused whole when a hand sends it and half-kept when a FILE does.
			// AND THE HALF-KEPT ANSWER IS THE WORSE ONE — (0.25, 0) is a place the sound has never
			// been, which is the argument `setSoundResidue` already makes at 88 coordinates and a
			// drift is the same argument at two. A point is one fact, not two.
			//
			// PER-VALUE IS STILL RIGHT PER SLOT, which is not a contradiction: one bad slot must not
			// cost another slot its drift, because those ARE two facts.
			const bool whole = std::isfinite(x) && std::isfinite(y);
			travel.offset[slot] = whole ? AnchorPoint { x, y } : AnchorPoint {};
		}
		// THE REST OF THE SLOT, count-driven: members this build knows are kept, members it does not
		// are read and dropped, members the writer did not have stay at their zero.
		double rest[2] = { 0.0, 0.0 };
		for (size_t member = 2; member < values; ++member) {
			double value = 0.0;
			if (!readDouble(stream, &value)) {
				return false;
			}
			if (member < 4) {
				rest[member - 2] = value;
			}
		}
		if (slot < kMaxSounds) {
			// The pair or neither, again — and a negative radius is not a radius.
			const bool whole = std::isfinite(rest[0]) && std::isfinite(rest[1]) && rest[0] >= 0.0;
			travel.orbit[slot] = whole ? PolarOffset { rest[0], rest[1] } : PolarOffset {};
		}
	}

	*out = travel;
	*present = true;
	return true;
}

bool saveSoundGamma(const clap_ostream* stream, const SoundGamma& gamma) {
	if (stream == nullptr) {
		return false;
	}
	if (!writeAll(stream, kGammaTag, sizeof(kGammaTag))) {
		return false;
	}
	// The count, written though it is a constant, so a build with more slots can read a patch from a
	// build with fewer without either of them guessing — the idiom every block above this one uses.
	if (!writeFloat(stream, static_cast<float>(kMaxSounds))) {
		return false;
	}
	for (size_t slot = 0; slot < kMaxSounds; ++slot) {
		// DOUBLE, and the width is the feature rather than a habit: this is the unbounded coordinate,
		// and a float would quantise it at about eight zones per ULP by the time it reached 1e5.
		if (!writeDouble(stream, gamma.base[slot])) {
			return false;
		}
	}
	return true;
}

bool loadSoundGamma(const clap_istream* stream, SoundGamma* out, bool* present) {
	if (stream == nullptr || out == nullptr || present == nullptr) {
		return false;
	}
	*present = false;

	uint8_t tag[4];
	bool ended = false;
	if (!readAll(stream, tag, sizeof(tag), &ended)) {
		// A patch written before the vernier existed ends exactly here. Not an error, and the caller
		// keeps the zero base that reproduces it to the bit.
		return ended;
	}
	if (std::memcmp(tag, kGammaTag, sizeof(tag)) != 0) {
		return false;
	}

	float count = 0.0f;
	if (!readFloat(stream, &count)) {
		return false;
	}
	// Validated BEFORE the cast, for the reason loadOrientation spells out: an out-of-range float
	// converts to whatever the hardware feels like, and the value it lands on can pass a range check
	// written against the RESULT.
	if (!(count >= 0.0f) || count > 64.0f) {
		return false;
	}
	const auto written = static_cast<size_t>(count);

	SoundGamma gamma;
	for (size_t slot = 0; slot < written; ++slot) {
		double base = 0.0;
		if (!readDouble(stream, &base)) {
			return false;
		}
		// Slots this build does not have are read and dropped rather than skipped blindly, so the
		// stream position is right whatever the counts are.
		//
		// AND A NON-FINITE BASE IS DROPPED WITH THEM, which is not the same as clamping one. This
		// quantity is UNBOUNDED BY DESIGN — a sound carried far along its line is the feature, and
		// the case that covers it says so — but infinity and NaN are not far along the line, they are
		// off it. The count above is already validated with the idiom that catches a NaN
		// (`!(count >= 0.0f)`, false for a NaN); the value was not.
		//
		// IT MATTERS HERE RATHER THAN AT THE SETTER because a patch arrives through
		// `restoreSoundGamma`, which writes the whole set straight into the atomics and never passes
		// `setSoundGamma`. So this is the only door that road has.
		if (slot < kMaxSounds && std::isfinite(base)) {
			gamma.base[slot] = base;
		}
	}

	*out = gamma;
	*present = true;
	return true;
}

bool saveSoundHues(const clap_ostream* stream, const SoundHues& hues) {
	if (stream == nullptr) {
		return false;
	}
	if (!writeAll(stream, kHuesTag, sizeof(kHuesTag))) {
		return false;
	}
	// The count, written though it is a constant — the idiom every block here uses, so a build with
	// more slots can read a patch from a build with fewer without either of them guessing.
	if (!writeFloat(stream, static_cast<float>(kMaxSounds))) {
		return false;
	}
	for (size_t slot = 0; slot < kMaxSounds; ++slot) {
		// A FLOAT FOR A NUMBER 0..7, which looks wasteful and is the house shape: every scalar in this
		// stream is a float or a double, the readers are written against that, and a lone byte here
		// would be the one field whose width somebody has to remember.
		if (!writeFloat(stream, static_cast<float>(hues.index[slot]))) {
			return false;
		}
	}
	return true;
}

bool loadSoundHues(const clap_istream* stream, SoundHues* out, bool* present) {
	if (stream == nullptr || out == nullptr || present == nullptr) {
		return false;
	}
	*present = false;

	uint8_t tag[4];
	bool ended = false;
	if (!readAll(stream, tag, sizeof(tag), &ended)) {
		// A patch written before colours could travel ends exactly here. Not an error, and the caller
		// keeps the identity that reproduces what it drew.
		return ended;
	}
	if (std::memcmp(tag, kHuesTag, sizeof(tag)) != 0) {
		return false;
	}

	float count = 0.0f;
	if (!readFloat(stream, &count)) {
		return false;
	}
	// Validated BEFORE the cast, for the reason loadOrientation spells out: an out-of-range float
	// converts to whatever the hardware feels like, and the value it lands on can pass a range check
	// written against the RESULT.
	if (!(count >= 0.0f) || count > 64.0f) {
		return false;
	}
	const auto written = static_cast<size_t>(count);

	SoundHues hues;
	for (size_t slot = 0; slot < written; ++slot) {
		float index = 0.0f;
		if (!readFloat(stream, &index)) {
			return false;
		}
		// Range-checked before the cast and folded after it, both on purpose: a patch is a file on
		// somebody's disk, and a colour index off the end of an eight-entry palette is the one way a
		// name can become a crash. Slots this build does not have are read and dropped rather than
		// skipped blindly, so the stream position is right whatever the counts are.
		if (slot < kMaxSounds) {
			const bool sane = index >= 0.0f && index < 64.0f;
			hues.index[slot] = sane ? static_cast<uint8_t>(static_cast<size_t>(index) % kMaxSounds)
			                        : static_cast<uint8_t>(slot);
		}
	}

	*out = hues;
	*present = true;
	return true;
}

bool saveMacroModes(const clap_ostream* stream, const MacroModes& modes) {
	if (stream == nullptr) {
		return false;
	}
	if (!writeAll(stream, kMacroModesTag, sizeof(kMacroModesTag))) {
		return false;
	}
	// The count, then one float per macro holding 0 or 1 — the house shape (see the hues block): every
	// scalar in this stream is a float or a double, and a build with more macros reads a patch from a
	// build with fewer without either guessing.
	if (!writeFloat(stream, static_cast<float>(kMacroCount))) {
		return false;
	}
	for (size_t i = 0; i < kMacroCount; ++i) {
		if (!writeFloat(stream, modes.bipolar[i] != 0 ? 1.0f : 0.0f)) {
			return false;
		}
	}
	// SINCE V41, per macro: its type as a float, then its name as a byte count (a float) and the raw
	// UTF-8 bytes. The writer clamps, so no name can produce a stream this build's reader refuses.
	for (size_t i = 0; i < kMacroCount; ++i) {
		if (!writeFloat(stream, static_cast<float>(static_cast<uint8_t>(modes.type[i])))) {
			return false;
		}
		const std::string name = device::clampMacroName(modes.name[i]);
		if (!writeFloat(stream, static_cast<float>(name.size()))) {
			return false;
		}
		if (!name.empty() && !writeAll(stream, name.data(), name.size())) {
			return false;
		}
	}
	return true;
}

bool loadMacroModes(const clap_istream* stream, MacroModes* out, bool* present) {
	if (stream == nullptr || out == nullptr || present == nullptr) {
		return false;
	}
	*present = false;

	uint8_t tag[4];
	bool ended = false;
	if (!readAll(stream, tag, sizeof(tag), &ended)) {
		// A patch written before a macro could swing both ways ends exactly here. Not an error, and
		// the caller keeps every macro unipolar, which is what that patch was.
		return ended;
	}
	if (std::memcmp(tag, kMacroModesTag, sizeof(tag)) != 0) {
		return false;
	}

	float count = 0.0f;
	if (!readFloat(stream, &count)) {
		return false;
	}
	if (!(count >= 0.0f) || count > 64.0f) {
		return false;
	}
	const auto written = static_cast<size_t>(count);

	MacroModes modes;
	for (size_t i = 0; i < written; ++i) {
		float flag = 0.0f;
		if (!readFloat(stream, &flag)) {
			return false;
		}
		// Macros this build does not have are read and dropped, so the stream position is right
		// whatever the counts are; anything that is not exactly 1 is unipolar, the safe reading.
		if (i < kMacroCount) {
			modes.bipolar[i] = flag == 1.0f ? 1 : 0;
		}
	}

	// SINCE V41: type and name per macro, for the same `written` macros. A stream that ENDS here is
	// MALFORMED, not an old patch: `ParamSet::load` refuses any version but this one before a block is
	// read, so every stream that reaches this reader was written with these fields, and a short read
	// mid-block is a truncation like any other in this file.
	for (size_t i = 0; i < written; ++i) {
		float type = 0.0f;
		if (!readFloat(stream, &type)) {
			return false;
		}
		float length = 0.0f;
		if (!readFloat(stream, &length)) {
			return false;
		}
		// Validated as a float BEFORE the cast (see loadOrientation): negative, non-integral, NaN or
		// past the cap is a malformed stream, because a length is the one field whose misreading
		// desynchronises every block after it.
		if (!(length >= 0.0f) || length > static_cast<float>(device::kMacroNameMax) ||
		    length != std::floor(length)) {
			return false;
		}
		const auto bytes = static_cast<size_t>(length);
		char name[device::kMacroNameMax] = {};
		if (bytes > 0) {
			bool nameEnded = false;
			if (!readAll(stream, name, bytes, &nameEnded)) {
				return false;
			}
		}
		if (i < kMacroCount) {
			// A type is clamped, not refused: NaN reads as Knob (what every macro was), anything past
			// the last type as the last — a knob that behaves wrongly is recoverable, a lost patch is not.
			constexpr float kLast = static_cast<float>(static_cast<uint8_t>(device::MacroType::kCount) - 1);
			const float t = (type >= 0.0f) ? std::min(std::floor(type), kLast) : 0.0f;
			modes.type[i] = static_cast<device::MacroType>(static_cast<uint8_t>(t));
			modes.name[i] = device::clampMacroName(std::string_view(name, bytes));
		}
	}

	*out = modes;
	*present = true;
	return true;
}

namespace {

/// The longest key and the most floats one spectral entry may declare. Past either, the stream is malformed: a count is
/// the one field whose misreading desynchronises every block after it, so it is bounded before it is believed.
constexpr float kSpectralKeyMax = 64.0f;
constexpr float kSpectralFloatsMax = 4096.0f;

bool writeSpectralEntry(const clap_ostream* stream, const char* key, const float* values, size_t count) {
	const size_t length = std::strlen(key);
	if (!writeFloat(stream, static_cast<float>(length)) || !writeAll(stream, key, length)
	    || !writeFloat(stream, static_cast<float>(count))) {
		return false;
	}
	for (size_t i = 0; i < count; ++i) {
		if (!writeFloat(stream, values[i])) {
			return false;
		}
	}
	return true;
}

bool validCount(float v, float max) {
	return v >= 0.0f && v <= max && v == std::floor(v);
}

} // namespace

bool saveSpectralState(const clap_ostream* stream, const SpectralState& state, const float* vocodeFreeze) {
	if (stream == nullptr || !writeAll(stream, kSpectralTag, sizeof(kSpectralTag))) {
		return false;
	}
	// THE MASTER SWITCH, THEN EACH SOUND'S CURVE: its switch and its gains, under its letter.
	if (!writeFloat(stream, static_cast<float>(1 + 2 * kMaxSounds + (vocodeFreeze != nullptr ? 1 : 0)))) {
		return false;
	}
	const float on = state.on ? 1.0f : 0.0f;
	if (!writeSpectralEntry(stream, kSpectralOnKey, &on, kSpectralOnFloats)) {
		return false;
	}
	for (size_t slot = 0; slot < kMaxSounds; ++slot) {
		const spectral::DrawnCurve& curve = state.curve[slot];
		const float curveOn = curve.on ? 1.0f : 0.0f;
		if (!writeSpectralEntry(stream, kSpectralCurveOnKeys[slot], &curveOn, kSpectralCurveOnFloats)
		    || !writeSpectralEntry(stream, kSpectralCurveGainKeys[slot], curve.gain.data(), kSpectralCurveGainFloats)) {
			return false;
		}
	}
	// OE: THE ENGINE VOCODER'S HELD ENVELOPE, as the synth last published it (additive design §9.3).
	if (vocodeFreeze != nullptr && !writeSpectralEntry(stream, kVocodeFreezeKey, vocodeFreeze, kVocodeFreezeFloats)) {
		return false;
	}
	return true;
}

bool loadSpectralState(const clap_istream* stream, SpectralState* out, bool* present) {
	if (stream == nullptr || out == nullptr || present == nullptr) {
		return false;
	}
	*present = false;
	uint8_t tag[4];
	bool ended = false;
	if (!readAll(stream, tag, sizeof(tag), &ended)) {
		return ended;
	}
	if (std::memcmp(tag, kSpectralTag, sizeof(tag)) != 0) {
		return false;
	}
	float entries = 0.0f;
	if (!readFloat(stream, &entries) || !validCount(entries, 256.0f)) {
		return false;
	}
	SpectralState state; // every key the stream does not carry keeps its default
	std::array<float, static_cast<size_t>(kSpectralFloatsMax)> values {};
	for (int e = 0; e < static_cast<int>(entries); ++e) {
		float length = 0.0f;
		if (!readFloat(stream, &length) || !validCount(length, kSpectralKeyMax)) {
			return false;
		}
		char key[static_cast<size_t>(kSpectralKeyMax) + 1] {};
		const auto bytes = static_cast<size_t>(length);
		if (bytes > 0 && !readAll(stream, key, bytes, &ended)) {
			return false;
		}
		float count = 0.0f;
		if (!readFloat(stream, &count) || !validCount(count, kSpectralFloatsMax)) {
			return false;
		}
		const auto n = static_cast<size_t>(count);
		for (size_t i = 0; i < n; ++i) {
			if (!readFloat(stream, &values[i])) {
				return false;
			}
		}
		const std::string_view k(key, bytes);
		if (k == kVocodeFreezeKey && n == kVocodeFreezeFloats) {
			std::copy_n(values.begin(), n, state.vocodeFreeze.begin()); // validated where it is primed (readVocodeFreezeBlock)
		}
		if (k == kSpectralOnKey && n == kSpectralOnFloats) {
			state.on = values[0] == 1.0f;
		}
		for (size_t slot = 0; slot < kMaxSounds; ++slot) {
			if (k == kSpectralCurveOnKeys[slot] && n == kSpectralCurveOnFloats) {
				state.curve[slot].on = values[0] == 1.0f;
			}
			else if (k == kSpectralCurveGainKeys[slot] && n == kSpectralCurveGainFloats) {
				for (size_t i = 0; i < n; ++i) {
					// A band outside [0, kSpectralCurveGainMax] (spectral_state.h) is pinned, a non-finite one is unity — one band of a
					// drawing, not the shape of the stream, so it is not a refusal.
					const float g = values[i];
					state.curve[slot].gain[i] = std::isfinite(g) ? std::clamp(g, 0.0f, kSpectralCurveGainMax) : 1.0f;
				}
			}
		}
		// Anything else: a key from another build, read past and dropped.
	}
	*out = state;
	*present = true;
	return true;
}

bool saveTerrainLikes(const clap_ostream* stream, const TerrainLikes& likes) {
	// NO LIKES, NO TABLE: the tag and an entry count of 0, which every loader of this block already reads as "present,
	// no likes". The keyed entry is kTerrainLikesFloats words whatever the like count (~360 KB), so writing it for an
	// empty set made every saved patch ~370 KB against ~10 KB (the perf survey, 2026-10-29: 81 test presets embedded as a
	// 154 MB generated .cpp, 446 s to compile). The block itself stays, because a 52 stream without it is refused as cut.
	if (likes.likes.empty()) {
		return stream != nullptr && writeAll(stream, kTerrainLikesTag, sizeof(kTerrainLikesTag)) && writeFloat(stream, 0.0f);
	}
	if (stream == nullptr || !writeAll(stream, kTerrainLikesTag, sizeof(kTerrainLikesTag)) || !writeFloat(stream, 1.0f)) {
		return false;
	}
	// ONLY THE SLOTS IN USE (backlog OQ): the full table is ~360 KB whatever the count.
	std::vector<float> words(terrainLikesFloatsFor(likes.likes.size()), 0.0f); // main thread: a save may allocate
	writeTerrainLikes(likes, words.data(), words.size());
	return writeSpectralEntry(stream, kTerrainLikesKey, words.data(), words.size());
}

bool loadTerrainLikes(const clap_istream* stream, TerrainLikes* out, bool* present) {
	if (stream == nullptr || out == nullptr || present == nullptr) {
		return false;
	}
	*present = false;
	uint8_t tag[4];
	bool ended = false;
	if (!readAll(stream, tag, sizeof(tag), &ended)) {
		return ended;
	}
	if (std::memcmp(tag, kTerrainLikesTag, sizeof(tag)) != 0) {
		return false;
	}
	float entries = 0.0f;
	if (!readFloat(stream, &entries) || !validCount(entries, 256.0f)) {
		return false;
	}
	TerrainLikes likes; // a key the stream does not carry keeps no likes
	std::vector<float> values;
	for (int e = 0; e < static_cast<int>(entries); ++e) {
		float length = 0.0f;
		if (!readFloat(stream, &length) || !validCount(length, kSpectralKeyMax)) {
			return false;
		}
		char key[static_cast<size_t>(kSpectralKeyMax) + 1] {};
		const auto bytes = static_cast<size_t>(length);
		if (bytes > 0 && !readAll(stream, key, bytes, &ended)) {
			return false;
		}
		float count = 0.0f;
		if (!readFloat(stream, &count) || !validCount(count, 1048576.0f) /* any key's length, read past if unknown (the v5 likes are 85057) */) {
			return false;
		}
		values.assign(static_cast<size_t>(count), 0.0f);
		for (float& v : values) {
			if (!readFloat(stream, &v)) {
				return false;
			}
		}
		// THE KEY AT ITS OWN LENGTH, or nothing: a block that is not this layout's, or holds a non-finite like, is refused
		// whole rather than read in part — no likes, never some of them.
		if (std::string_view(key, bytes) == kTerrainLikesKey) {
			TerrainLikes read;
			if (readTerrainLikes(values.data(), values.size(), &read)) {
				likes = std::move(read);
			}
		}
	}
	*out = std::move(likes);
	*present = true;
	return true;
}

bool savePresetIdentity(const clap_ostream* stream, const PresetIdentity& identity) {
	if (stream == nullptr || !writeAll(stream, kPresetIdentityTag, sizeof(kPresetIdentityTag))) {
		return false;
	}
	const auto text = [stream](std::string_view s) {
		return writeFloat(stream, static_cast<float>(s.size())) && (s.empty() || writeAll(stream, s.data(), s.size()));
	};
	// BOTH KEYS ALWAYS, each a length and its bytes: the key, then the value. Small, and the same shape a later key takes.
	return writeFloat(stream, 2.0f) && text(kPresetNameKey) && text(identity.name.substr(0, kPresetIdentityMaxBytes))
	       && text(kPresetCategoryKey) && text(identity.category.substr(0, kPresetIdentityMaxBytes));
}

bool loadPresetIdentity(const clap_istream* stream, PresetIdentity* out, bool* present) {
	if (stream == nullptr || out == nullptr || present == nullptr) {
		return false;
	}
	*present = false;
	uint8_t tag[4];
	bool ended = false;
	if (!readAll(stream, tag, sizeof(tag), &ended)) {
		return ended;
	}
	if (std::memcmp(tag, kPresetIdentityTag, sizeof(tag)) != 0) {
		return false;
	}
	float entries = 0.0f;
	if (!readFloat(stream, &entries) || !validCount(entries, 64.0f)) {
		return false;
	}
	const auto text = [stream](std::string* s) {
		float length = 0.0f;
		if (!readFloat(stream, &length) || !validCount(length, static_cast<float>(kPresetIdentityMaxBytes))) {
			return false;
		}
		s->assign(static_cast<size_t>(length), '\0');
		bool cut = false;
		return s->empty() || readAll(stream, s->data(), s->size(), &cut);
	};
	PresetIdentity identity; // a key the stream does not carry stays empty
	for (int e = 0; e < static_cast<int>(entries); ++e) {
		std::string key;
		std::string value;
		if (!text(&key) || !text(&value)) {
			return false;
		}
		if (key == kPresetNameKey) {
			identity.name = std::move(value);
		}
		else if (key == kPresetCategoryKey) {
			identity.category = std::move(value);
		}
	}
	*out = std::move(identity);
	*present = true;
	return true;
}

namespace {
constexpr char kDrumLanesTag[4] = { 'F', 'D', 'L', '1' };
constexpr float kDrumLaneBytesMax = 67108864.0f; // 64 MiB, a chain body's own ceiling and then some
} // namespace

bool saveDrumLanes(const clap_ostream* stream, const DrumLanes& lanes) {
	// THE TAG, AN ENTRY COUNT, then per pad that has a lane holding anything: its key ("pad.a".."pad.h") and its chain's
	// body. A pad with no lane, or an empty one, writes nothing, so a kit without pad lanes is the tag and a 0.
	if (stream == nullptr || !writeAll(stream, kDrumLanesTag, sizeof(kDrumLanesTag))) {
		return false;
	}
	size_t entries = 0;
	for (const auto& body : lanes) {
		entries += body.empty() ? 0u : 1u;
	}
	if (!writeFloat(stream, static_cast<float>(entries))) {
		return false;
	}
	for (size_t pad = 0; pad < lanes.size(); ++pad) {
		const std::vector<uint8_t>& body = lanes[pad];
		if (body.empty()) {
			continue;
		}
		const std::string key = std::string("pad.") + static_cast<char>('a' + pad);
		if (!writeFloat(stream, static_cast<float>(key.size())) || !writeAll(stream, key.data(), key.size())
		    || !writeFloat(stream, static_cast<float>(body.size())) || !writeAll(stream, body.data(), body.size())) {
			return false;
		}
	}
	return true;
}

bool loadDrumLanes(const clap_istream* stream, DrumLanes* out, bool* present) {
	if (stream == nullptr || out == nullptr || present == nullptr) {
		return false;
	}
	*present = false;
	uint8_t tag[4];
	bool ended = false;
	if (!readAll(stream, tag, sizeof(tag), &ended)) {
		return ended;
	}
	if (std::memcmp(tag, kDrumLanesTag, sizeof(tag)) != 0) {
		return false;
	}
	float entries = 0.0f;
	if (!readFloat(stream, &entries) || !validCount(entries, 64.0f)) {
		return false;
	}
	DrumLanes lanes;
	for (int e = 0; e < static_cast<int>(entries); ++e) {
		float length = 0.0f;
		if (!readFloat(stream, &length) || !validCount(length, kSpectralKeyMax)) {
			return false;
		}
		char key[static_cast<size_t>(kSpectralKeyMax) + 1] {};
		const auto keyBytes = static_cast<size_t>(length);
		if (keyBytes > 0 && !readAll(stream, key, keyBytes, &ended)) {
			return false;
		}
		float count = 0.0f;
		if (!readFloat(stream, &count) || !validCount(count, kDrumLaneBytesMax)) {
			return false;
		}
		std::vector<uint8_t> body(static_cast<size_t>(count));
		if (!body.empty() && !readAll(stream, body.data(), body.size(), &ended)) {
			return false;
		}
		// A KEY THIS BUILD KNOWS lands on its pad; anything else is read past and dropped.
		const std::string_view k(key, keyBytes);
		if (k.size() == 5 && k.substr(0, 4) == "pad." && k[4] >= 'a' && k[4] < static_cast<char>('a' + lanes.size())) {
			lanes[static_cast<size_t>(k[4] - 'a')] = std::move(body);
		}
	}
	*out = std::move(lanes);
	*present = true;
	return true;
}

bool saveSoundVariations(const clap_ostream* stream, const SoundVariations& variations,
                         const std::array<AnchorPoint, kMaxSounds>& livePlaces) {
	if (stream == nullptr) {
		return false;
	}
	// FSV2 SINCE V18: each stored stance carries its ride-along values behind a third header count.
	// The tag had to move — the stream is positional with no lengths, so an FSV1 reader meeting
	// grown stances would hand the extra bytes to the next block's tag read, a silent misparse; the
	// bumped tag turns that into a deterministic whole-load refusal on FSV1-era builds. The value
	// count below is what retires further bumps — with the caveat the review put on the record:
	// only from builds carrying the raised header cap onward. The one FSV2-aware build shipped
	// before V20 capped the count at 64 and refuses this record whole at the header, which is the
	// same honest refusal by a different door.
	if (!writeAll(stream, kVariationsTag2, sizeof(kVariationsTag2))) {
		return false;
	}
	// All three counts, written though all are constants — the idiom every block here uses, so a
	// build with more slots, a longer list or MORE VALUE MEMBERS can read a patch from a build with
	// fewer without either guessing.
	if (!writeFloat(stream, static_cast<float>(kMaxSounds))
	    || !writeFloat(stream, static_cast<float>(kMaxVariations))
	    || !writeFloat(stream, static_cast<float>(kRideAlongWireValues))) {
		return false;
	}
	for (size_t slot = 0; slot < kMaxSounds; ++slot) {
		const SoundVariations::List& list = variations.sound[slot];
		if (!writeFloat(stream, static_cast<float>(list.count))
		    || !writeFloat(stream, static_cast<float>(list.active))) {
			return false;
		}
		// ONLY WHAT IS STORED, not the whole array: the entries past the count are value-initialised
		// padding, and writing them would put numbers in the file that mean nothing and would have to be
		// ignored on the way back in.
		for (size_t at = 0; at < list.count; ++at) {
			// THE DEAD PLACE SLOTS carry THE SLOT'S LIVE PLACE — as DEFENSE-IN-DEPTH, and the
			// review corrected this comment's original claim: NO SHIPPED BUILD READS THESE BYTES.
			// Place left the stance (V20) so this build's loader drops them, and every pre-V20
			// loader refuses the whole block at the header (its per-stance cap is 64, this record
			// is 96) — a patch saved here fails to open in earlier builds as a deterministic
			// whole-load refusal, the V2 posture. The live place is simply the honest value if a
			// future reader ever walks these bytes, and deterministic bytes keep resave-identical
			// exact.
			if (!writeFloat(stream, static_cast<float>(livePlaces[slot].x))
			    || !writeFloat(stream, static_cast<float>(livePlaces[slot].y))) {
				return false;
			}
			// The ride-along value members through the ONE spelled order (rideAlongWireValue).
			// DOUBLES, unlike the dead floats: these are parameters and state read at double
			// precision and a recall must restore the exact values the stance stored — and one
			// uniform member size is also what lets a shorter build DROP members it does not know
			// without guessing their widths.
			for (size_t member = 0; member < kRideAlongWireValues; ++member) {
				if (!writeDouble(stream, rideAlongWireValue(list.values[at], member))) {
					return false;
				}
			}
		}
	}
	return true;
}

bool loadSoundVariations(const clap_istream* stream, SoundVariations* out, bool* present,
                         size_t* valuesRead) {
	if (stream == nullptr || out == nullptr || present == nullptr) {
		return false;
	}
	*present = false;
	if (valuesRead != nullptr) {
		*valuesRead = 0;
	}

	uint8_t tag[4];
	bool ended = false;
	if (!readAll(stream, tag, sizeof(tag), &ended)) {
		// A patch written before variations existed ends exactly here. Not an error: the caller keeps
		// empty lists, which is every sound having the one placement its parameters hold.
		return ended;
	}
	// TWO GENERATIONS, ONE LOADER. FSV1 is places-only and read forever; FSV2 carries a third
	// header count and per-stance ride-along values behind it. Anything else is refused.
	const bool legacy = std::memcmp(tag, kVariationsTag, sizeof(tag)) == 0;
	if (!legacy && std::memcmp(tag, kVariationsTag2, sizeof(tag)) != 0) {
		return false;
	}

	float slots = 0.0f;
	float perSound = 0.0f;
	float perStance = 0.0f;
	if (!readFloat(stream, &slots) || !readFloat(stream, &perSound)) {
		return false;
	}
	if (!legacy && !readFloat(stream, &perStance)) {
		return false;
	}
	// Validated BEFORE the cast, for the reason loadOrientation spells out: an out-of-range float
	// converts to whatever the hardware feels like, and the value it lands on can pass a range check
	// written against the RESULT.
	// The member count's ceiling is generous where the slot counts' are tight: the explore box is
	// 96 members today and future members append, so the sanity bound guards against garbage (a
	// float that is not a small count) rather than against growth.
	if (!(slots >= 0.0f) || slots > 64.0f || !(perSound >= 0.0f) || perSound > 64.0f
	    || !(perStance >= 0.0f) || perStance > 4096.0f) {
		return false;
	}
	const auto writtenSlots = static_cast<size_t>(slots);
	const auto writtenPer = static_cast<size_t>(perSound);
	// How many ride-along floats each stored stance carries in THIS stream: zero in the legacy
	// block, kRideAlongWireValues in one we wrote, possibly more in a stream from a build with
	// members we have not heard of — those are read and dropped, which is what keeps the stream
	// position right and this tag the last one the block ever needs.
	const size_t writtenValues = legacy ? 0 : static_cast<size_t>(perStance);

	SoundVariations variations;
	for (size_t slot = 0; slot < writtenSlots; ++slot) {
		float count = 0.0f;
		float active = 0.0f;
		if (!readFloat(stream, &count) || !readFloat(stream, &active)) {
			return false;
		}
		if (!(count >= 0.0f) || count > static_cast<float>(writtenPer) || !(active >= 0.0f)
		    || active > 64.0f) {
			return false;
		}
		const auto stored = static_cast<size_t>(count);
		for (size_t at = 0; at < stored; ++at) {
			float x = 0.0f;
			float y = 0.0f;
			if (!readFloat(stream, &x) || !readFloat(stream, &y)) {
				return false;
			}
			// Entries this build has no room for are read and dropped rather than skipped blindly, so the
			// stream position is right whatever the counts are. The x,y just read are the DEAD place
			// slots (or a legacy record's real places) — dropped either way: place left the stance.
			(void)x;
			(void)y;
			const bool keep = slot < kMaxSounds && at < kMaxVariations;
			// The ride-along value members through the ONE spelled order; members past what this
			// build knows are read and dropped like the entries above.
			for (size_t member = 0; member < writtenValues; ++member) {
				double value = 0.0;
				if (!readDouble(stream, &value)) {
					return false;
				}
				if (keep && member < kRideAlongWireValues) {
					setRideAlongWireValue(variations.sound[slot].values[at], member, value);
				}
			}
		}
		if (slot < kMaxSounds) {
			SoundVariations::List& list = variations.sound[slot];
			list.count = static_cast<uint8_t>(std::min(stored, kMaxVariations));
			// An active index a shorter build cannot address lands on its last entry rather than out of
			// bounds — the patch still says which stance was live, and this build says the nearest thing
			// it can hold.
			const size_t wanted = static_cast<size_t>(active);
			list.active = list.count == 0
			                  ? 0
			                  : static_cast<uint8_t>(std::min(wanted, static_cast<size_t>(list.count - 1)));
		}
	}

	*out = variations;
	*present = true;
	if (valuesRead != nullptr) {
		*valuesRead = writtenValues;
	}
	return true;
}

void fillAbsentStanceValues(SoundVariations& variations,
                            const std::array<RideAlongValues, kMaxSounds>& live,
                            size_t firstAbsent) {
	// See the header: an absent member means THE SLOT'S LIVE VALUE, so recalling a stance from a
	// patch that predates that member is a no-op on that axis — exactly the behaviour the stance
	// was stored under, never a silent zeroing. Member-granular: an FSV2 file from the width-only
	// era fills everything past width; an FSV1 file fills everything.
	for (size_t slot = 0; slot < kMaxSounds; ++slot) {
		SoundVariations::List& list = variations.sound[slot];
		for (size_t at = 0; at < list.count; ++at) {
			for (size_t member = firstAbsent; member < kRideAlongWireValues; ++member) {
				setRideAlongWireValue(list.values[at], member,
				                      rideAlongWireValue(live[slot], member));
			}
		}
	}
}

bool saveSoundOrientations(const clap_ostream* stream, const SoundOrientations& planes) {
	if (stream == nullptr) {
		return false;
	}
	if (!writeAll(stream, kPlanesTag, sizeof(kPlanesTag))) {
		return false;
	}
	// Both counts, written though both are constants: a build with more slots or more angles can read
	// a patch from a build with fewer without either of them guessing.
	// Count ONE angle per slot — the legacy layout at the shrunk count, exactly as the global pin's
	// block writes it; see saveOrientation for why the dead aperture slot stays in the format.
	if (!writeFloat(stream, static_cast<float>(kMaxSounds)) || !writeFloat(stream, 1.0f)) {
		return false;
	}
	for (size_t slot = 0; slot < kMaxSounds; ++slot) {
		// Whether this sound's turn was held by hand, written before the numbers it qualifies — an
		// unpinned sound records that it is unpinned rather than freezing a turn nobody gave it.
		if (!writeFloat(stream, planes.pin[slot].pinned ? 1.0f : 0.0f)
		    || !writeFloat(stream, 0.0f) // the aperture slot, unread by M
		    || !writeFloat(stream, planes.pin[slot].generator.rotate)) {
			return false;
		}
	}
	return true;
}

bool loadSoundOrientations(const clap_istream* stream, SoundOrientations* out, bool* present) {
	if (stream == nullptr || out == nullptr || present == nullptr) {
		return false;
	}
	*present = false;

	uint8_t tag[4];
	bool ended = false;
	if (!readAll(stream, tag, sizeof(tag), &ended)) {
		// A patch written before per-sound planes existed ends here. Not an error, and the caller keeps
		// the unpinned entries that reproduce it exactly.
		return ended;
	}
	if (std::memcmp(tag, kPlanesTag, sizeof(tag)) != 0) {
		return false;
	}

	float slots = 0.0f;
	float angles = 0.0f;
	if (!readFloat(stream, &slots) || !readFloat(stream, &angles)) {
		return false;
	}
	// Validated BEFORE the cast, both of them — converting an out-of-range float is undefined, and the
	// value it lands on can pass a range check written against the result. See loadOrientation.
	if (!(slots >= 0.0f) || slots > 64.0f || !(angles >= 0.0f) || angles > 64.0f) {
		return false;
	}
	const auto writtenSlots = static_cast<size_t>(slots);
	const auto writtenAngles = static_cast<size_t>(angles);

	SoundOrientations planes;
	for (size_t slot = 0; slot < writtenSlots; ++slot) {
		float pinned = 0.0f;
		float aperture = 0.0f;
		if (!readFloat(stream, &pinned) || !readFloat(stream, &aperture)) {
			return false;
		}
		SliceOrientationPin pin;
		pin.pinned = pinned != 0.0f;
		// The aperture was read above so the stream position is right; M never reads it, so it drops.
		static_cast<void>(aperture);
		for (size_t k = 0; k < writtenAngles; ++k) {
			float angle = 0.0f;
			if (!readFloat(stream, &angle)) {
				return false;
			}
			// Every angle is read so the stream position is right whatever the count; angle 0 is the
			// turn (ratio[0] = 1 — the specified migration), the rest drop. The raw-byte migration cases in
			// the bridge suite pin this reading.
			if (k == 0) {
				// UNBOUNDED IS NOT NON-FINITE, and this is the value's ONLY door — the guard on
				// `setSoundOrientation` is on the SINGULAR, and a patch takes the plural. Substituted
				// per value, not per block, so a bad angle cannot cost a good slot its turn.
				pin.generator.rotate =
				    std::isfinite(angle) ? angle : SliceOrientationPin {}.generator.rotate;
			}
		}
		if (slot < kMaxSounds) {
			planes.pin[slot] = pin;
		}
	}

	*out = planes;
	*present = true;
	return true;
}

bool saveSoundResidue(const clap_ostream* stream, const SoundResidue& residue) {
	if (stream == nullptr) {
		return false;
	}
	if (!writeAll(stream, kResidueTag, sizeof(kResidueTag))) {
		return false;
	}
	// NOTHING BANKED, ALMOST NOTHING WRITTEN — a header with a slot count of zero rather than the 88
	// doubles a slot this block otherwise costs. It used to write NO BLOCK AT ALL in this case, which
	// was correct while it was the last thing in the stream and became a bug the moment gamma's block
	// was appended after it: an absent middle block leaves the next block's tag sitting where THIS
	// loader reads, and the residue loader then rejects a perfectly good patch. Found by the
	// round-trip through the plugin's real state road, which is the only test that walks the whole
	// tail in order.
	//
	// A zero count reads back as an all-zero residue, which is what an absent block always meant. Old
	// patches, written before this block existed at all, still end early and still read as absent —
	// the loaders treat a clean end of stream as "not present" and always have.
	if (!residue.anyBanked()) {
		return writeFloat(stream, 0.0f) && writeFloat(stream, static_cast<float>(kMorphResidueValues))
		       && writeFloat(stream, static_cast<float>(kStairResidueValues))
		       && writeFloat(stream, static_cast<float>(kVoxResidueValues))
		       && writeFloat(stream, static_cast<float>(kWeaveResidueValues))
		       && writeFloat(stream, static_cast<float>(kPartialResidueValues));
	}
	// SIX COUNTS, because the payload has five parts. Slots, then how many numbers the morph engine
	// contributed, how many the stair engine did, how many vox did, how many weave did and how many the
	// partials did — a single total would be
	// ambiguous the day any bank grows a triangle, and the reader would split the parts in the wrong
	// place with no way to know it had. The count-per-engine idiom is why an FSR1 patch can still be
	// placed correctly by the loader below: its two counts say where its two parts end, whatever this
	// build's own banks have since become.
	if (!writeFloat(stream, static_cast<float>(kMaxSounds))
	    || !writeFloat(stream, static_cast<float>(kMorphResidueValues))
	    || !writeFloat(stream, static_cast<float>(kStairResidueValues))
	    || !writeFloat(stream, static_cast<float>(kVoxResidueValues))
	    || !writeFloat(stream, static_cast<float>(kWeaveResidueValues))
	    || !writeFloat(stream, static_cast<float>(kPartialResidueValues))) {
		return false;
	}
	for (size_t slot = 0; slot < kMaxSounds; ++slot) {
		for (size_t i = 0; i < kPlaneResidueValues; ++i) {
			if (!writeDouble(stream, planeResidueValue(residue.banked[slot], i))) {
				return false;
			}
		}
	}
	return true;
}

bool loadSoundResidue(const clap_istream* stream, SoundResidue* out, bool* present) {
	if (stream == nullptr || out == nullptr || present == nullptr) {
		return false;
	}
	*present = false;

	uint8_t tag[4];
	bool ended = false;
	if (!readAll(stream, tag, sizeof(tag), &ended)) {
		// A patch written before the residue was saved — or any patch that never rotated a drifted sound — ends
		// here. Not an error, and the caller keeps the empty residue that reproduces it exactly.
		return ended;
	}
	// FOUR TAGS ACCEPTED, AND THE TAG IS WHAT SAYS HOW MANY COUNTS FOLLOW — the one thing a reader
	// cannot discover from the payload, which is why the bump existed. FSR1 carries three counts and no
	// vox rows; FSR2 carries four and no weave rows; FSR3 carries five and no partial rows; FSR4 six.
	const bool hasPartialCount = std::memcmp(tag, kResidueTag, sizeof(tag)) == 0;
	const bool hasWeaveCount = hasPartialCount || std::memcmp(tag, kResidueTagV3, sizeof(tag)) == 0;
	const bool hasVoxCount = hasWeaveCount || std::memcmp(tag, kResidueTagV2, sizeof(tag)) == 0;
	if (!hasVoxCount && std::memcmp(tag, kResidueTagV1, sizeof(tag)) != 0) {
		return false;
	}

	float slots = 0.0f;
	float morphValues = 0.0f;
	float stairValues = 0.0f;
	float voxValues = 0.0f;
	float weaveValues = 0.0f;
	float partialValues = 0.0f;
	if (!readFloat(stream, &slots) || !readFloat(stream, &morphValues)
	    || !readFloat(stream, &stairValues)) {
		return false;
	}
	// ABSENT MEANS ZERO ROWS, not "as many as this build has". An FSR1 patch was written by a build
	// whose fold had never heard of vox, so it banked no vox difference and there are none of its
	// numbers in the stream to read — the zero rows below are what that patch sounded like and
	// therefore what it must load as. An FSR2 patch and the weave rows, the same; an FSR3 patch and the
	// partial rows, the same.
	if (hasVoxCount && !readFloat(stream, &voxValues)) {
		return false;
	}
	if (hasWeaveCount && !readFloat(stream, &weaveValues)) {
		return false;
	}
	if (hasPartialCount && !readFloat(stream, &partialValues)) {
		return false;
	}
	// Validated BEFORE the cast, all of them — converting an out-of-range float is undefined, and the
	// value it lands on can pass a range check written against the result. See loadOrientation, where
	// 1e30 became 0 and sailed through a `> 64`. The ceiling is generous rather than exact: it is here
	// to refuse nonsense, not to police a future bank's size.
	const auto sane = [](float v) { return v >= 0.0f && v <= 4096.0f; };
	if (!sane(slots) || !sane(morphValues) || !sane(stairValues) || !sane(voxValues) || !sane(weaveValues)
	    || !sane(partialValues)) {
		return false;
	}
	const auto writtenSlots = static_cast<size_t>(slots);
	// THE PARTS, IN THE FLAT ORDER: how many numbers the stream WROTE for each, and how many this build
	// HOLDS. One walk places every part the same way — its first min(written, held) numbers land at the
	// part's start in this build's order, the rest are read and dropped — so the next engine to append a
	// part adds one entry to each array rather than one more nested subtraction (the class of bug where a
	// part's numbers were bounded by this build's count instead of the stream's, and lent to its neighbour).
	const std::array<size_t, 5> written { static_cast<size_t>(morphValues), static_cast<size_t>(stairValues),
		                                  static_cast<size_t>(voxValues), static_cast<size_t>(weaveValues),
		                                  static_cast<size_t>(partialValues) };
	constexpr std::array<size_t, 5> held { kMorphResidueValues, kStairResidueValues, kVoxResidueValues,
		                                   kWeaveResidueValues, kPartialResidueValues };
	static_assert(held[0] + held[1] + held[2] + held[3] + held[4] == kPlaneResidueValues,
	              "every part of the flat order has its entry here");
	// AND IN THE FLAT ORDER'S ORDER: the sum above passes with two entries swapped, which would land one
	// part's numbers at another's start. The two boundaries the flat order names pin the sequence.
	static_assert(held[0] + held[1] + held[2] == kPreWeaveResidueValues, "morph, stair, vox, then weave");
	static_assert(held[0] + held[1] + held[2] + held[3] == kPrePartialResidueValues, "weave, then the partials");

	SoundResidue residue;
	for (size_t slot = 0; slot < writtenSlots; ++slot) {
		// ALL OR NOTHING PER SLOT, which is the rule `ParamBridge::setSoundResidue(slot, banked)`
		// already states and gives the reason for: *"a residue is one banked difference spread across
		// 88 coordinates, and storing the finite half of a spoilt fold would leave the sound standing
		// at a point that is neither where it was nor where the turn would have put it."* The
		// per-slot setter enforces it; the WHOLE-SET one that a patch load takes
		// (`setSoundResidue(const SoundResidue&)`) does not, so this loader is the door.
		bool spoilt = false;
		size_t start = 0; // where this part begins in THIS build's flat order
		for (size_t part = 0; part < written.size(); ++part) {
			for (size_t j = 0; j < written[part]; ++j) {
				double value = 0.0;
				if (!readDouble(stream, &value)) {
					return false;
				}
				// Read and dropped rather than skipped blindly, so the stream position is right whatever
				// the counts are — the rule every block here follows. The parts are placed by the counts
				// that were WRITTEN, so a patch from a build with a smaller morph bank still lands its
				// stair numbers in the stair rows rather than four triangles to the left.
				if (!std::isfinite(value)) {
					// The rest of this slot is still READ, and then discarded below.
					spoilt = true;
				}
				if (slot < kMaxSounds && j < held[part]) {
					setPlaneResidueValue(residue.banked[slot], start + j, value);
				}
			}
			start += held[part];
		}
		// AND THE SPOILT SLOT GOES BACK TO NO FOLD AT ALL, which is the state that reproduces the
		// sound exactly — see the empty-residue note at the top of this function.
		if (spoilt && slot < kMaxSounds) {
			residue.banked[slot] = PlaneResidue {};
		}
	}

	*out = residue;
	*present = true;
	return true;
}

// THE CHAINS BLOCK — both chains, every device's row and body, and every route, in the ONE format
// (`rack/rack_state.h`, chain-as-a-device item 6). What this file's routings block established — a route
// names its device by instance id and its control by index, a parameter by its ParamId, and a reference
// that does not resolve is DROPPED LOUDLY — is that format's own rule now, stated there. The swap that
// made it a rule, kept here because it is the example that convinces: swap slot 1's DAHDSR for an MSEG
// and a route addressed to "slot 1, control 3" silently becomes a route to a different device's third
// control. By identity, it is refused and said.

//
// **THE PATCH IS A JSON DOCUMENT SINCE BACKLOG JF** (`rack::rackToText`), and a document read from a stream
// runs to the stream's end — but this block has blocks after it. So the framing here is the tag, the
// document's length in bytes, then the text: the one place the envelope still frames by length, until the
// envelope itself becomes JSON (the plan's step 2).

bool saveChainsBlock(const clap_ostream* stream, const rack::RackPatch& patch) {
	std::string text;
	if (stream == nullptr || !rack::rackToText(patch, &text) || text.size() > UINT32_MAX) {
		return false;
	}
	const auto bytes = static_cast<uint32_t>(text.size());
	return writeAll(stream, kChainsTag, sizeof(kChainsTag)) && writeAll(stream, &bytes, sizeof(bytes))
	       && writeAll(stream, text.data(), text.size());
}

bool loadChainsBlock(const clap_istream* stream, rack::RackPatch* out, bool* present, rack::LoadReport* report) {
	if (stream == nullptr || out == nullptr || present == nullptr) {
		return false;
	}
	*present = false;
	*out = rack::RackPatch {};
	uint8_t tag[4];
	bool ended = false;
	if (!readAll(stream, tag, sizeof(tag), &ended)) {
		return ended; // a stream that ends here carries no chains: the caller puts a fresh instance's
	}
	if (std::memcmp(tag, kChainsTag, sizeof(tag)) != 0) {
		return false;
	}
	// A TAG WITH NOTHING AFTER IT is a truncated block, not an absent one. The length is bounded before
	// a byte is allocated for it: the rack's own ceiling on a body, and room around it.
	uint32_t bytes = 0;
	if (!readAll(stream, &bytes, sizeof(bytes), &ended) || bytes == 0 || bytes > rack::kMaxBodyBytes * 2) {
		return false;
	}
	std::string text(bytes, '\0');
	if (!readAll(stream, text.data(), bytes, &ended)) {
		return false;
	}
	if (!rack::parseRackText(text, out, report) || !out->present) {
		return false;
	}
	*present = true;
	return true;
}

FxChain defaultFxChain() {
	FxChain chain;
	// The tag comes from the device's own declared traits rather than being spelled out here, so a
	// tenant and its record cannot disagree about what the tenant is called.
	const char* tag = fx::filter::kTraits.tag;
	static_assert(kFxTagBytes == 4);
	for (size_t i = 0; i < kFxTagBytes; ++i) {
		chain.device[0].row.tag[i] = tag[i];
	}
	chain.count = 1;
	// THE FIRST DEVICE GETS THE FIRST SERIAL. A default lane whose one device had kNoInstance would
	// be a device nothing could route to, which is a worse starting point than an empty lane.
	// ITS PLACE IN THE ONE SEQUENCE — the rack default holds the one before it. See
	// plugin/device_instance.h for why "the first serial" on both chains was a collision every
	// session started with.
	chain.device[0].row.instanceId = kDefaultFxInstance;
	// **THERE IS NO LANE AND NO ORDER TO STAMP ANY MORE (kill-lanes step 3c).** Two lines stood here:
	// one giving the row a routing lane — because a device the live table could not address was the same
	// "nothing could route to it" the serial above prevents on the other axis — and one writing
	// `chain.order[0]`, because this road builds its device by writing the row rather than by calling
	// `addFxDevice` and so was one of the two places that had to state the position-to-lane fact itself.
	// A route names the instance id now and the subscript IS the position, so `device[0]` with
	// `count == 1` says everything both lines said. The serial stamp above still has to be here, and it
	// still has to be BEFORE the settings below, because `setFxFilterAt` rebuilds the row and carries the
	// identity it finds there across.
	// AND ITS SETTINGS, not just its tag. The row is where a swallowed device's controls live, so a
	// lane whose record carried only a tag would be a filter with no settings at all — every field
	// reading as absent. stateOf answers that with the DECLARED defaults, which is the right answer,
	// but only because the count says the fields are missing rather than zero. Stamping them makes the
	// default lane SAY what it holds instead of relying on that inference.
	//
	// **AND ITS SWITCH IS NAMED HERE, OFF, RATHER THAN TAKEN FROM THE DEVICE'S DEFAULT.** The
	// 2026-09-04 decision (all effects default to on when they are added) turned every fx
	// device's declared enable to 1, and it is a decision about ADDING: the press on (+) is the hand
	// asking for the device. THIS FILTER WAS NEVER ADDED. It is the factory patch, pinned in slot 0
	// of every instance that has not loaded a state, and its declared cutoff is 1 kHz — so taking the
	// device default here would low-pass the whole plugin before anybody had touched it.
	//
	// ONE LINE, AND IT IS THE ONLY PLACE THE FACTORY PATCH DISAGREES WITH THE DEVICE. Delete it and
	// the pinned filter follows that default like everything else; that is a decision about what a
	// fresh Feathers should SOUND like, which is authored content and not this function's.
	fx::filter::State pinned {};
	pinned.settings.enabled = false;
	setFxFilterAt(&chain, 0, pinned);
	chain.nextInstanceId = kFirstFreeInstance;
	return chain;
}

bool fxChainHas(const FxChain& chain, const char* tag) {
	if (tag == nullptr) {
		return false;
	}
	for (size_t i = 0; i < chain.count && i < chain.device.size(); ++i) {
		if (std::memcmp(fxDeviceAtPosition(chain, i).tag().data(), tag, kFxTagBytes) == 0) {
			return true;
		}
	}
	return false;
}

size_t slotOfFxInstance(const FxChain& chain, DeviceInstanceId id) {
	if (id == kNoInstance) {
		return kNoPosition;
	}
	const size_t at = fxPositionOfInstance(chain, id);
	return at;
}

DeviceInstanceId instanceOfFxSlot(const FxChain& chain, size_t slot) {
	if (slot >= fxChainLength(chain)) {
		return kNoInstance;
	}
	return fxDeviceAtPosition(chain, slot).instanceId();
}

// defaultModRack, dahdsrRow and dahdsrSettings live in mod_rack.cpp — see the note at the top of
// that file for why the rack's data functions are not in here.

} // namespace feathers
