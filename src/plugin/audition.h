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

#pragma once

#include "plugin/fx_chain.h"
#include "plugin/mod_rack.h"
#include "plugin/audition_tuning.h"
#include "plugin/sound_orientation.h"
#include "plugin/sound_residue.h"
#include "plugin/sound_gamma.h"
#include "plugin/sound_hues.h"
#include "plugin/macro_modes.h"
#include "plugin/spectral_state.h"
#include "plugin/terrain_likes.h"
#include "backplane/preset_identity.h"
#include "plugin/sound_travel.h"
#include "plugin/sound_variations.h"
#include "rack/rack_state.h"

#include "clap/stream.h"

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

// The slice's orientation: where a session's starting one comes from, and how a patch keeps it.
//
// THE HANDLE IS TEMPORARY, THE STORAGE IS NOT. Stage 1 is auditioning a geometry — whether moving in
// the plane sounds like going somewhere new — and nothing can be asked about that without a way to
// aim the plane. Spending a parameter id to ask would be spending it forever, so the handle is an
// environment variable and three debug sliders, and both go away when a designed control arrives.
//
// What stays is the STREAM FORMAT. A pinned plane is part of what a patch sounds like, so a patch has
// to carry it, and it is written EXPLICITLY rather than left to anything's default — the versioning
// obligation. An UNPINNED patch carries that fact too, so that a sound whose plane is
// derived keeps deriving it rather than being frozen to whatever the derivation said on the day it
// was saved.
//
// The host matters here. the Bitwig is the flatpak, and a plain shell export does not cross the
// sandbox: it takes `flatpak run --env=FEATHERS_AXIS_ROTATION=… com.bitwig.BitwigStudio`, or a
// `flatpak override --user --env=…` to set it once. Which is exactly why the reader announces what it
// read — one line in the host's log turns "did the variable reach the plugin" from a guess into a
// fact, and a silent default is indistinguishable from a plugin that never loaded.

namespace feathers {

/// Parse an orientation from `text`. ONE number is the rotate, in radians. TWO OR MORE numbers is
/// the legacy "aperture,angle0,angle1,…" layout, whose turn is the SECOND field — the aperture rode
/// first and nothing reads it. Comma or whitespace separated.
///
/// Lenient about separators and trailing junk, strict about numbers: a field that is not a number
/// ends the parse, rather than being silently read as zero. An empty or absent string gives the
/// default orientation — rotate 0, the born-open plane every unturned sound plays.
SliceGenerator parseOrientation(std::string_view text);

/// The orientation pin this process should start with: FEATHERS_AXIS_ROTATION if it is set, and
/// UNPINNED otherwise — in which case every sound's plane comes from its own home, which is the
/// ordinary way the engine runs. Announces on stderr which of the two it got. Called once per plugin
/// instance.
///
/// The announcement is not decoration. the Bitwig is the flatpak, a plain shell export does not
/// cross the sandbox, and a silent fallback to the default is indistinguishable from a plugin that
/// never loaded — so the one line in the host's log is what turns "did the variable arrive" from a
/// guess into a fact.
SliceOrientationPin orientationFromEnvironment();

/// The blend falloff this process should start with: FEATHERS_FALLOFF_POWER if it parses as a
/// positive number, and power 1 otherwise — the value that leaves a two-sound patch exactly as it
/// was. Announces which of the two it got, and on which terms, for the same flatpak reason the
/// orientation does.
///
/// NOT SAVED INTO A PATCH, deliberately, and the asymmetry with the orientation is the point: a
/// pinned plane is a sound somebody chose and has to be able to reload, while this is a question
/// being asked of an ear. Storing it would put an unanswered question into everybody's patches, and
/// the answer — whichever it is — becomes a constant rather than a field.
double falloffPowerFromEnvironment();

/// The blue-shift scales this process should start with — FEATHERS_BLUE_OVERSHOOT and
/// FEATHERS_BLUE_DOPPLER, each a non-negative number, and 1 unless one of them says otherwise.
///
/// LIVE BY DEFAULT, by decision: the uniform blend rule is gated on blue shift so that it has directional
/// character to give back, and scales at zero would have made that a net loss. Setting either to 0 is
/// how the audition hears the engine WITHOUT it, which is the comparison the whole feature is judged
/// by — see the zero-scale rest case in tests/test_character_blend.cpp.
///
/// Not saved into a patch, for the same reason the falloff is not.
void blueShiftScalesFromEnvironment(double* overshoot, double* doppler);

/// Whether this process should take the FIRMWARE-PARITY REFERENCE at two sounds — FEATHERS_V1_CROSSFADE,
/// set to anything, and off otherwise.
///
/// §2c: the verbatim v1 crossfade stopped being the shipping rule when blue shift landed, and is kept
/// reachable so the bit-identity tests keep running against it and so there is a way back if the
/// character is redeployed to hardware. Documented as a reference, NOT as a user feature — which is
/// why it has an environment variable and no control: a control is how something becomes a feature.
bool v1CrossfadeFromEnvironment();

/// Append the orientation to a patch, after whatever the parameters wrote.
///
/// A tagged trailing block, so a stream written before this existed simply ends where the parameters
/// end and reads back as "absent" rather than as garbage.
bool saveOrientation(const clap_ostream* stream, const SliceOrientationPin& pin);

/// Read the orientation a patch carries, or report that it carries none.
///
/// Three outcomes, and they are deliberately distinguishable: read one (true, `*out` set), the patch
/// predates the block (true, `*out` untouched — the caller leaves the pin empty and every sound
/// derives its own plane), or the stream is malformed (false). Guessing on the third would load a
/// sound nobody saved.
bool loadOrientation(const clap_istream* stream, SliceOrientationPin* out, bool* present);

/// Append each sound's drift offset, after the orientation block.
///
/// A SECOND TAGGED BLOCK rather than a wider first one, which is the same argument that made the
/// orientation a trailing block in the first place: a patch saved before this existed ends where the
/// orientation ends, and reads back as "absent" rather than as a short read. Nothing has to be
/// versioned, because a block that is not there is a block whose fields keep their defaults — and the
/// default here is exactly the zero that reproduces every patch made before drift existed.
bool saveSoundTravel(const clap_ostream* stream, const SoundTravel& travel);

/// Read the drift a patch carries, or report that it carries none. Same three outcomes as the
/// orientation's loader, and for the same reasons.
bool loadSoundTravel(const clap_istream* stream, SoundTravel* out, bool* present);

/// Append each sound's own plane, after the drift block.
///
/// A THIRD TAGGED BLOCK, for the reason the second one was: a patch written before this ends where
/// the drift ends and reads back as absent, and absent means every sound derives its plane from its
/// home — which is the engine as it always was, bit for bit.
///
/// WRITTEN AS FLOATS, deliberately, and this is where the drift block's lesson gets applied rather
/// than copied. That lesson was "persist at the width the value actually IS": drift holds doubles and
/// lost them through float casts. A SliceGenerator holds floats all the way down — the basis maths is
/// float — so a float here is exact, and a double would be four bytes of implied precision the type
/// does not have.
bool saveSoundOrientations(const clap_ostream* stream, const SoundOrientations& planes);

/// Read the per-sound planes a patch carries, or report that it carries none.
bool loadSoundOrientations(const clap_istream* stream, SoundOrientations* out, bool* present);

/// Append what each sound's rotations banked, after the planes block.
///
/// A FOURTH TAGGED BLOCK, on the rule the second and third follow: a patch written before the
/// residue decision ends where the planes end and reads back as absent, and absent means every residue is
/// zero — which the decode adds inside the parenthesis the travel products already sat in, so it is the
/// engine as it always was, bit for bit.
///
/// AND IT IS NOT WRITTEN AT ALL when nothing has been banked, which the other three had no reason to
/// do. This one is 88 numbers per slot, and a patch that has never rotated a drifted sound would
/// otherwise carry five kilobytes of zeros for a feature it does not use. `anyBanked()` decides, and an
/// all-zero block and an absent one mean exactly the same thing to the loader.
///
/// WRITTEN AS DOUBLES, which is the drift block's lesson rather than the planes block's: persist at the
/// width the value actually IS. A residue is the difference of two travel contributions, both
/// accumulated in double by the decode, and the guarantee it exists to give — the standing sound comes
/// back where it was — is stated in doubles.
///
/// MEASURED, because the drift block's version of this argument came with a character change to show
/// for it and this one does not, quite: a float round trip loses about nine digits of each coordinate,
/// and at the magnitudes a real turn banks (~1e-1) the built table did NOT move in the known-bad. So
/// the honest reason is exactness rather than an audible difference — the residue is what makes a
/// rewind land on the sound it recorded rather than near it, and a value quantised at ~1e-8 in phase
/// sits close enough to the character banks' own float sensitivity, once their rate multipliers have
/// scaled it, that whether a given patch survives would depend on where in the bank it happens to sit.
/// That is not a thing to leave to a patch's luck for the sake of four bytes a coordinate.
bool saveSoundResidue(const clap_ostream* stream, const SoundResidue& residue);

/// Read what a patch banked, or report that it banked nothing.
bool loadSoundResidue(const clap_istream* stream, SoundResidue* out, bool* present);

/// GAMMA'S UNBOUNDED HALF. Absent in every patch written before the vernier existed, which reads as a
/// zero base — and a zero base builds the character those patches always built, to the bit.
bool saveSoundGamma(const clap_ostream* stream, const SoundGamma& gamma);

/// Reads it, or reports it absent at a clean end of stream.
bool loadSoundGamma(const clap_istream* stream, SoundGamma* out, bool* present);

/// WHICH COLOUR EACH SOUND WEARS — see sound_hues.h for why a name is stored rather than derived.
///
/// Absent in every patch written before colours could travel, and absence reads as the IDENTITY: slot
/// 0 wearing the first colour, slot 1 the second. That is exactly what those patches drew, because a
/// colour used to be its slot's index — so a patch from before this block opens looking the way it
/// looked when it was saved, which is what makes the block safe to append.
bool saveSoundHues(const clap_ostream* stream, const SoundHues& hues);

/// Reads them, or reports the block absent at a clean end of stream.
bool loadSoundHues(const clap_istream* stream, SoundHues* out, bool* present);

/// WHICH WAY EACH STRUCTURAL MACRO SWINGS (backlog EG) — see macro_modes.h. The last block; absent in
/// every patch written before it, and absence reads as every macro unipolar, which is what those
/// patches were.
bool saveMacroModes(const clap_ostream* stream, const MacroModes& modes);

/// Reads them, or reports the block absent at a clean end of stream.
bool loadMacroModes(const clap_istream* stream, MacroModes* out, bool* present);

/// THE SPECTRAL DRAWER'S STORED STATE (spectral_state.h) — the master switch and the drawn curve, as KEYED values:
/// the tag, an entry count, then per entry its key (a byte count and the bytes), a float count and the floats. A key
/// this build does not know is skipped and a known key whose count is not this build's is ignored (its default
/// stands), so a state added or retired later is a non-event; tests/state_schema.ledger guards the meanings.
/// `vocodeFreeze` (kVocodeFreezeFloats, or null for none): OE's held envelope, written under kVocodeFreezeKey when given — the
/// synth's own snapshot, not `state.vocodeFreeze`, which is only what the last load carried.
bool saveSpectralState(const clap_ostream* stream, const SpectralState& state, const float* vocodeFreeze = nullptr);

/// Reads it, or reports the block absent at a clean end of stream (what the loader of a pre-51 stream never asks).
bool loadSpectralState(const clap_istream* stream, SpectralState* out, bool* present);

/// THE TERRAIN'S LIKES (backlog ON, terrain_likes.h) — kStateVersion 52, the last block, after the spectral one; KEYED in the
/// spectral block's shape: the tag, an entry count, then per entry its key, a float count and the floats. One key today,
/// kTerrainLikesKey, at `terrainLikesFloatsFor(count)` (only the slots in use, backlog OQ) up to kTerrainLikesFloats; a key
/// this build does not know is skipped and a known key that is not a whole number of slots is ignored (no likes), so the
/// meaning is the ledger's to guard.
bool saveTerrainLikes(const clap_ostream* stream, const TerrainLikes& likes);

/// Reads them, or reports the block absent at a clean end of stream (what the loader of a pre-52 stream never asks).
bool loadTerrainLikes(const clap_istream* stream, TerrainLikes* out, bool* present);

/// **WHICH PRESET THE PATCH CAME FROM** (backlog QC, preset_identity.h) — kStateVersion 53, after the likes. KEYED TEXT: the
/// tag, an entry count, then per entry its key and its bytes, each behind a float length. Two keys today, kPresetNameKey
/// and kPresetCategoryKey; a key this build does not know is read past, and a missing one stays empty.
bool savePresetIdentity(const clap_ostream* stream, const PresetIdentity& identity);

/// Reads it, or reports the block absent at a clean end of stream (what the loader of a pre-53 stream never asks).
bool loadPresetIdentity(const clap_istream* stream, PresetIdentity* out, bool* present);

/// THE KIT PADS' FX LANES (drum proposal §17 P4) — kStateVersion 54, the last block, after the preset: the tag `FDL1`, an entry
/// count, then per pad whose lane holds anything its key ("pad.a".."pad.h"), the body's byte count and the body
/// (`rack::InnerChain::saveState`'s, a keyed chain bundle). A pad not named has no lane, or an empty one.
using DrumLanes = std::array<std::vector<uint8_t>, 8>;
bool saveDrumLanes(const clap_ostream* stream, const DrumLanes& lanes);
/// Reads them, or reports the block absent at a clean end of stream (what the loader of a pre-54 stream never asks).
bool loadDrumLanes(const clap_istream* stream, DrumLanes* out, bool* present);

/// Append the stances each sound remembers, after the gamma block.
///
/// A SIXTH TAGGED BLOCK, on the rule the four before it follow: a patch written before variations
/// existed ends where the gamma block ends and reads back as absent, and absent means every sound has
/// exactly the one implicit record its live state holds — the engine and the editor as they were.
///
/// WRITTEN EVEN WHEN NOTHING IS STORED, unlike the residue block, and the asymmetry is deliberate. The
/// residue's conditional write buys real bytes (88 numbers a slot) and cost a load-failure hunt to get
/// right; this block is eight counts and a handful of pairs, so the bytes are not worth a second code
/// path that only runs on some patches. Every slot carries its count, and a count of zero says "this
/// sound never stored one" in the file exactly as it does in memory.
///
/// FLOATS, because these are the record's DEAD x,y slots (place left the stance, V20) and the dead
/// layout keeps the float width a placement had when it was live.
/// `livePlaces` fills them with each slot's live placement rather than zeros — DEFENSE-IN-DEPTH,
/// not a compatibility promise, and the review corrected the record here: NO SHIPPED BUILD EVER
/// READS THESE BYTES. Every pre-V20 loader refuses the whole block at the HEADER — its per-stance
/// sanity cap is 64 and this writer emits 96 — so a patch saved by this build fails to open in any
/// earlier one as a deterministic whole-load refusal (the V2 posture: refusal beats misparse), and
/// the count-driven no-tag-bump benefit begins only at builds carrying the raised cap. The live
/// place is written because it is the honest value if some future reader ever does walk these
/// bytes, and because deterministic bytes keep the resave-identical claim exact.
bool saveSoundVariations(const clap_ostream* stream, const SoundVariations& variations,
                         const std::array<AnchorPoint, kMaxSounds>& livePlaces);

/// Read the variations a patch carries, or report that it carries none. Same three outcomes as every
/// loader above, and for the same reasons.
///
/// TWO TAGS ARE ACCEPTED, ONE IS WRITTEN. "FSV1" is the legacy block — places only, all dropped
/// now — and is read forever; "FSV2" (V18) adds the per-stance ride-along values behind a third
/// header count, so future value members need no further tag. `valuesRead`, when asked for, says
/// how many members the stream carried per stance; the caller owes every member past it the
/// migration fill — see fillAbsentStanceValues.
///
/// THE OLD-BUILD STORY, CORRECTED BY THE REVIEW: an FSV1-era build refuses a new patch at the TAG
/// compare; the one FSV2-aware pre-V20 build refuses it at the HEADER — its per-stance sanity cap
/// was 64 and this build writes 96 — so it never reaches a stance record at all. Both are
/// deterministic whole-load refusals (the V2 posture: refusal beats misparse, nothing shipped
/// outside this house), and the count-driven no-further-bump benefit begins only at builds
/// carrying the raised cap.
bool loadSoundVariations(const clap_istream* stream, SoundVariations* out, bool* present,
                         size_t* valuesRead = nullptr);

/// THE MIGRATION FILL for stance members a patch did not carry: absent means THE SLOT'S LIVE
/// VALUE, not zero. A stance stored before a member existed was stored under behaviour where a
/// recall never touched that axis, and the live fill reproduces exactly that for every old patch:
/// the recall is a no-op on the absent axes. A zero fill would silently zero the sound's own
/// settings. Member-granular via the wire order (`firstAbsent` = the loader's valuesRead), so a
/// width-only FSV2 file keeps its widths and fills the rest, and an FSV1 file fills everything.
/// The live values arrive as an array so this stays param-agnostic like the rest of the file; the
/// caller reads them from the state the stream loaded just before this block.
void fillAbsentStanceValues(SoundVariations& variations,
                            const std::array<RideAlongValues, kMaxSounds>& live,
                            size_t firstAbsent);
/// WHAT A LOAD REFUSED, so that a route dropped by the rules can be SAID rather than lost in silence —
/// the one format's summary (`rack/rack_state.h`), the same type in every host: the count, and the first
/// refusal's coordinates, one past the axis for an end that named something not here.
///
/// A summary rather than a list, and that is a decision about the tray rather than about the loader:
/// the tray is one line and a later say() replaces an earlier one, so N messages would show only the
/// last of them. The first refusal is named in full and the rest are counted, which is specific
/// enough to act on — "my modulation stopped working" becomes "the build told me which one".
using RouteRefusals = rack::RouteRefusals;

/// **BOTH CHAINS AND EVERY ROUTE — the last block, in the rack's own format** (chain-as-a-device item 6,
///). A tag, then `rack::writeRack`: the strip (chain 0) and the
/// fx lane (chain 1), each device with its instance id, lane, row and container body, and every route named
/// by identity — Feathers' ends through `feathersCoreCodec`. What is IN the patch comes from the bridge's
/// record (`ParamBridge::chainsPatch`); this is only the framing.
bool saveChainsBlock(const clap_ostream* stream, const rack::RackPatch& patch);

/// `present` comes back false when the stream ends where the block would start — a fresh instance's
/// chains are the caller's to put. A tag that is not this block's, a truncated block, or a rack stream
/// that does not parse refuses the load, and `report` (if given) says why.
bool loadChainsBlock(const clap_istream* stream, rack::RackPatch* out, bool* present,
                     rack::LoadReport* report = nullptr);

} // namespace feathers
