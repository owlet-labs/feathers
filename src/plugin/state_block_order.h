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

#include <array>
#include <cstdint>

namespace feathers {

/// THE ONE ORDER OF THE TAGGED STATE BLOCKS, and the only place it is written down.
///
/// The stream is positional: after the parameters, the blocks follow in exactly this sequence, and a
/// loader reaching block N expects block N's tag. The order is therefore a CONTRACT with every patch
/// ever saved — the tag-refusal cases prove a reader arriving at the wrong block refuses rather than
/// consuming what it found, but they each construct their own stream, so they cannot notice a block
/// INSERTED between two others. This constant and the case that pins it are what notice.
///
/// APPEND ONLY, WITH ONE DELIBERATE EXCEPTION ALREADY TAKEN. A new block takes the last position,
/// here and in feathers_clap.cpp's table (whose entries draw their names from this array
/// positionally, so the two cannot drift apart in size or spelling without a compile error or a red
/// pin). Moving a block mid-sequence makes every existing patch load one block's bytes as another's,
/// silently — the corruption class this table was built to end.
///
/// ROUTINGS MOVED FROM POSITION 6 TO LAST ON 2026-08-22, and the reasoning is the template for any
/// future move rather than a licence to make one casually.
///
///   - IT WAS NECESSARY, not tidier. A routing now names its device by INSTANCE ID rather than by
///     slot, so the loader must resolve those ids against the chains — and at position 6 it ran
///     BEFORE `fxchain` and `modrack` had been read. There is no ordering of a single stream in
///     which routings can both precede the chains and resolve against them.
///   - THE SILENT-CORRUPTION HAZARD WAS PAID FOR, not accepted. `kStateVersion` went to 2 in the
///     same change, so a patch written under the old order refuses at the FIRST BYTE of the stream
///     rather than reaching this table and reading `hues` where it expected `routings`. The
///     block-order rule's whole fear is a silent misread; a version bump converts it into a clean
///     refusal.
///   - IT WAS ALLOWED. Compatibility was relaxed on 2026-08-22 (CLAUDE.md): take the clean design
///     and break the format, naming this file's APPEND ONLY specifically and answering its "you
///     are actually writing a stream-format migration" with permission to write one.
///
/// So the bar for moving a block is: the order is WRONG rather than merely awkward, the version byte
/// moves in the same commit, and someone has said the format may break. Absent any of the three,
/// append.
///
/// **`fxchain`, `modrack` AND `routings` BECAME ONE `chains` BLOCK ON 2026-09-11** (chain-as-a-device
/// item 6) — both chains and every route in the rack's own format, one
/// device record rather than two. All three bars met: two formats for one idea is wrong, `kStateVersion`
/// went to 4 in the same commit, and the format may break (CLAUDE.md, 2026-08-22).
///
/// **`macros` APPENDED 2026-09-15 (backlog EG)** — which way each structural macro swings. Appended,
/// not inserted, and the version byte did not move: a patch from before it ends before the tag, which
/// reads as every macro unipolar, which is what every such patch was.
///
/// **`spectral` APPENDED 2026-10-10 (additive design §3.1)** — the spectral drawer's master switch and drawn curve,
/// keyed. Since 50 a stream must carry every block (`loadWalk`), so an append is a change to the state's SHAPE and the
/// version byte moved to 51 — but NOT as a clean break: a 50 stream is still read, and a block it predates
/// (`kStateBlockSince`) is not looked for and takes its default (the drawer on, the curve flat), which is exactly what
/// a patch from before the drawer was.
/// **`likes` APPENDED 2026-10-26 (backlog ON)** — the terrain's likes, keyed, on exactly the spectral block's terms: the
/// version byte moved to 52, and a 51, 50 or 49 stream is still read with the block taking its default (no likes).
/// **`preset` APPENDED 2026-11-06 (backlog QC)** — which preset the patch came from (preset_identity.h), keyed text, so an engine
/// restart or a reopened project names the preset the sound is. Version 53; a 52 or older stream reads with no preset named.
/// **`drumlanes` APPENDED 2026-11-07 (drum proposal §17 P4), after `preset`** — each kit pad's fx lane, keyed by pad, on the
/// same terms: the version byte moved to 54, and a 53 or older stream is still read with every pad lane empty.
inline constexpr std::array<const char*, 13> kStateBlockOrder = {
	"orientation", "drift", "planes", "residue", "gamma", "variations", "hues", "chains", "macros", "spectral", "likes", "preset",
	"drumlanes",
};

/// **THE STREAM'S VERSION BYTE.** Lived in `params.cpp`'s anonymous namespace until 2026-09-10, which
/// meant `tests/test_params.cpp` carried its own copy of the number with a comment saying *"the number
/// moves here whenever it moves there"* — a hand-maintained duplicate, and it went red the first time
/// the byte actually moved. It lives beside the rules it enforces now, and the test reads it.
///
/// See `params.cpp` for what each version changed and, more importantly, for WHEN TO MOVE IT.
// **BUMPED TO 5 ON 2026-09-14 BECAUSE THE FORMAT CHANGED, not merely a meaning.** `device::kTagBytes`
// widened from four to eight, so every serialised device tag is four bytes longer. This is the case
// the device-ranges INFO describes as needing the version rather than only the constant.
// **6 (2026-09-15): SKLITTER's Macro Cfg Phase is DELETED**, so every column after it in that device's
// row moved down one — Gamma, Pitch Scale and Stereo Width. This is the case `kStateVersion` exists
// for: a saved row would read Gamma out of the phase's old column. Nothing is saved (patch
// compatibility is not yet a constraint), so there is no migration, only the version bump that
// makes an old blob be rejected rather than misread.
// **7 (2026-09-15): AUTOMOD's Rate changed MEANING, not place** — it was 0..2 read as a percentage and
// is ±2 OCTAVES read as a multiple (backlog DT): the same column, and a saved 1.0 that meant "1X" would
// read as "2X". The rarest case the rule names ("a stored NUMBER changes meaning") and the one the
// fingerprint CANNOT see: the digest eats tags, control names and Choice tables — not ranges or units
// (MEASURED: it did not move for this change; an earlier note below claiming it eats ranges and units
// was wrong) — so the version byte is the only thing standing between a v6 row and a silent 2X.
// **8 (2026-09-15): THE MSEG's Amount is INSERTED before its segment block** (backlog DH — every
// modulator gains an output Amount; the other three append theirs, which moves nothing). The MSEG's
// global block must hold it (`mseg::Param::Enabled`'s note: the segment arithmetic is tied to
// `kFirstSegmentParam`), so every segment column moved down one — a saved row would read a segment's
// level out of its time's column. The version byte refuses it instead.
// 9 -> 10 ON 2026-09-16 (backlog EL): OWLTT gained a `Soft Clip` switch, APPENDED to its row, default On —
// the behaviour that shipped, so a device that loses the field sounds as it did. The shape of the row
// changed, which the stored-meaning fingerprint sees; the version byte says a v9 OWLTT row is one number
// short of what a v10 reader expects.
// **12 (2026-09-17): THE MSEG's Depth is DELETED** (backlog FH — Amount is its one depth, as on the DAHDSR), so every
// column after it in that device's row moved up one. No migration (nothing is saved); the version refuses an old row.
// **13 (2026-09-18): THE DAHDSR's AND THE MSEG's Loop / SingleTrigger / Trigger COLLAPSED INTO ONE `Mode` DROPDOWN**
// (five choices), with a hidden SingleTrigger field kept behind it for the synth's legato. Three columns became one
// and the rest of both rows moved up two, so a v12 row read by a v13 reader would take a time out of a level's column.
//
// THE BUMP WAS WRITTEN DOWN AND NOT MADE. The fingerprint's own note below said "MOVED 2026-09-17 WITH kStateVersion
// 13" while this constant still read 12 — the guard described in the past tense and never armed. That is the one
// failure this pair cannot catch for itself: the fingerprint moves whenever the meaning moves, but whether a move
// DESERVES a version byte is a judgement, and the SKLITTER entry below is a real case of one that does not. So the
// two constants are deliberately not mechanically linked, and the cost of that is exactly this — it has to be read.
// **14 (2026-09-19): THE LFO's `Gamma` IS DELETED**, so every column after it in that device's row moved up one. It was
// modelled as a distribution exponent, `pow(h, gamma)`, over the random walk; the project's decision is that the idea
// was only ever a seed — gamma is just a random seed for the track-phase deterministic random — and the device
// already carries one. Two keys into one deterministic sequence is one more than the idea has, so the control went
// rather than being repaired. No migration (nothing is saved); the version refuses an old row.
//
// AND THIS ONE WAS CAUGHT BY THE GUARD RATHER THAN BY A READING, which is the pair working as intended for once:
// `device-ranges` went red on the fingerprint the moment the column left, and named the remedy in its own failure.
// **16 (2026-09-19): AND THIS ONE IS THE MERGE ITSELF, WHICH IS WHY IT IS NOT 15.** The two entries
// below were written on separate branches ON THE SAME DAY and each correctly took the byte from 14 to
// 15 against a registry that did not yet contain the other's change. So TWO DIFFERENT FORMATS BOTH
// CALL THEMSELVES 15, and the merged tree — which removes columns from four devices rather than from
// one or three — is a third. Keeping 15 would leave a number that does not identify a layout, which
// is the one job this byte has. The rule the two entries below both cite (insert, remove or reorder →
// bump) applies to their UNION as much as to either half.
//
// **THE SHAPE OF THIS IS THE FINGERPRINT'S, AND IT IS WORTH RECOGNISING TWICE IN ONE FILE.** The note
// above `kStoredMeaningFingerprint` says a merge's digest cannot be resolved by choosing a side
// because neither branch ever saw the union. A version byte contended by two branches is the same
// fact wearing a smaller number: also not resolvable by choosing a side, and for exactly the reason
// that makes the digest unresolvable.
//
// **15 (2026-09-19): SKLITTER's `Macro Cfg` AND `Macro` ARE DELETED**, and this is a REMOVAL FROM THE MIDDLE — they
// were columns 2 and 3 of a row that runs to 27, so every column after them moved up TWO. A v14 SKLITTER row read by
// a v15 reader would take its Macro Cfg into P Write and its Macro into Density, and go on mis-reading to the end of
// the row; nothing about the bytes says so, which is the whole reason this byte exists. The perform circle's centre
// became a constant zero push (the center macro is to be dropped and held at zero), so the knob pair the blend used to fall back to has nothing left to be, and a configuration with no macro
// to configure goes with it.
//
// **THE CONTRAST WITH THE ENTRY BELOW IS THE POINT OF WRITING BOTH DOWN.** The same device APPENDED eleven perform
// controls two days earlier and the version did NOT move; it REMOVES two now and the version does. Append, no;
// insert, remove or reorder, yes — that is the rule, stated in this file and demonstrated twice by one device inside
// one week. No migration (nothing is saved); the version refuses an old row.
//
// **15 (2026-09-19): `Smoothing On` IS DELETED FROM ALL THREE MODULATORS** — the DAHDSR, the MSEG and
// the LFO — because the bottom of each device's Smoothing travel already carried "off" and a switch
// beside it was a second spelling of a position the knob had. Three
// rows each lost a column, and every column AFTER Smoothing in each of them moved up one — the Mode,
// the phase, the bipolar flag, the amount. This is squarely the case the version byte exists for: a v14
// row read by a v15 reader would take a mode out of a switch's old column. No migration (nothing is
// saved, CLAUDE.md 2026-08-22); the version refuses an old row rather than misreading it.
//
// AND THE DEFAULT MOVED WITH IT, which is the half a fingerprint alone would not have made anyone think
// about: `Smoothing` defaulted to 20 ms beside a switch that defaulted to off, so the pair meant "off"
// and the number was advice for whoever flipped the switch. With the switch gone the number IS the
// state, so the default is the bottom of the travel. The RIGHT default rather than the one that keeps
// an old row sounding the same.
// **17 (2026-09-19): THE LFO'S THREE SKEWS ARE RESCALED FROM 0..100 TO 0..1, AND THE FINGERPRINT DID
// NOT FIRE.** Read that second clause first, because it is the interesting one.
//
// The controls declared `{ 0, 100, 50 }` with `Unit::Percent` while a Percent in this project STORES
// A FRACTION — so the boxes read 5000%. The range moved to
// `{ 0, 1, 0.5 }`. No control was inserted, removed or reordered; no name changed; no Choice table
// moved. So `kStoredMeaningFingerprint` is UNCHANGED and `test_device_ranges` stayed green through
// the whole change.
//
// **AND A STORED 50.0 WENT FROM "NEUTRAL" TO A HARD EXTREME.** It clamps to 1.0 now — the same bytes,
// the opposite end of the control. That is squarely "what a saved float means", which is the thing
// the version byte exists for, and nothing mechanical said so.
//
// **THE CARVE-OUT THE GUARD IS BUILT ON HAS A HOLE IN IT, and it is worth naming rather than
// patching.** `kStoredMeaningFingerprint`'s own note says it ignores *"ranges, defaults or
// tooltips… those change what a value may BE without changing what it MEANS"*. True of a WIDENING —
// a cutoff ceiling going from 20k to 22k leaves every stored number reading as it did. False of a
// RESCALING, where the same number lands somewhere else entirely. The digest cannot tell the two
// apart, and making it fire on every range edit would make it cry often enough to be ignored, which
// is the failure its own note is written to avoid. So the mechanism stays and the JUDGEMENT is
// written down here: **a range change needs this byte when the domain is rescaled, and does not when
// it is merely widened.**
//
// No migration (nothing is saved); the version refuses an old row rather than playing it at the
// wrong end of three controls.
//
// **18 (2026-09-19): SKLITTER GAINS `Send Override`, AND IT IS AN INSERT RATHER THAN AN APPEND.** The knob
// forces the per-slice delay send to always or never (backlog EW), and it went in at column 17, directly
// after `Mix` — so the seven perform-circle columns after it each moved DOWN one and a v16 SKLITTER row read
// by a v17 reader would take its `Cfg N` out of the override's column and go on mis-reading to the end.
//
// **THE APPEND WOULD HAVE BEEN FREE AND WAS STILL WRONG.** Column 24 was empty and costs no version byte,
// which is precisely the trade the 2026-08-22 format policy exists to refuse: there are no saved rows to
// protect, so the only thing an append would have bought is a knob sitting on the far side of the perform
// circle, in a place chosen by the format rather than by what it is. The circle is a BLOCK — a card lays it
// out as one — and a knob belongs with the knobs. Insert, then; and this file's own rule says an insert
// moves the byte, so it does.
//
// **AND IT IS 18 BECAUSE TWO BRANCHES BOTH TOOK 17, THE SAME DAY, FOR DIFFERENT FORMAT CHANGES.** The LFO
// skew rescale above and this insert were written independently and each correctly bumped 16 → 17. **Git
// merged the CONSTANT without a conflict — both sides said `17` — and flagged only the prose.** So the one
// artefact that would have carried two incompatible formats under one number was the artefact that merged
// cleanly, and the comments, which cannot be read by any program, were the only thing that objected.
//
// This is the SECOND such collision in one day: `kStateVersion` 15 was taken twice for different column
// removals a few hours earlier, and backlog letters collided the same way. **A shared namespace with no
// lock is not protected by everyone being careful**, because each side is individually correct and the
// defect exists only in the pair. The integrator is the only one positioned to see it, and only if the
// merge shows them something — which here it did not. Until a mechanism exists, the rule is: on ANY merge
// touching this file, read the VALUE rather than trusting the absence of a conflict.
// **19, 2026-09-21, SKLITTER's `Transport Locked` (backlog GT).** Inserted at column 15, beside the two
// other loop settings, rather than appended after the perform circle — CLAUDE.md's compatibility
// section rules that ids are free and that a control goes where it belongs. Everything from `Min Env`
// up shifted one column (`Min Env` 15→16, `Mix` 16→17, `Send Override` 17→18, the four waypoints
// 18..21→19..22, the perform trio 22..24→23..25), so every stored number in SKLITTER's row past column
// 14 changes meaning. That is precisely what this byte is for.
//
// **TAKEN AS 19 AFTER READING THE VALUE RATHER THAN THE ABSENCE OF A CONFLICT**, which is the rule two
// paragraphs up and the trap that caught two branches in one week. If this arrives in a merge beside
// another branch that also moved it, the number is re-read from the MERGED tree and so is the
// fingerprint below.
// **20, 2026-09-21, SKLITTER's feedback circuit (backlog GU/GV) — THREE columns inserted at 19, 20 and
// 21.** `Dry Write`, `Feedback` and `FB Tap`: the two continuous writers (only the wet signal comes back), and the switch for the one thing not yet decided (whether the loop returns post grain fx or post master fx). They go directly after `Send Override`
// because they are knobs and the knobs run to there; the seven perform-circle columns below are a
// BLOCK a card lays out as one. Everything from `Cfg 1` up shifted THREE columns (19..22→22..25 for
// the waypoints, 23..25→26..28 for the perform trio), so every stored number in SKLITTER's row past
// column 18 changes meaning.
//
// **READ FROM THE MERGED TREE RATHER THAN INFERRED FROM THE EDIT**, which is the rule two paragraphs
// up and the trap that caught two branches in one week: 19 was in the tree when this branch started
// and 20 is the next. If this arrives in a merge beside another branch that also moved it, the number
// is re-read from the MERGED tree and so is the fingerprint below.
// **21, 2026-09-21, the delay's `Mode` gaining `Xfade`**. A `Choice` table GREW, which the rule below lists, so the byte moves.
//
// **AND IT IS A JUDGEMENT THE DIGEST CANNOT MAKE, so it is written down rather than assumed.** The
// entry is APPENDED — `Clean` is still 0 and `Granular` still 1 — so by the rule's own stated test
// ("whenever a stored NUMBER changes meaning") nothing has changed meaning and no bump is owed. The
// rule says "grows" because a table usually grows IN PLACE, as `kDivisionBeats` did when it went
// from nine entries to twenty-seven and moved every saved MSEG shape onto a different division.
// This one does not. **Bumped anyway**: the rule as written says grows, nothing is saved to
// protect, and a byte costs nothing — so the cheap side of an uncertain call is the one that cannot
// be wrong in the direction that matters.
// **22, 2026-09-21, the delay's `Mode` gaining `Tape`** and four controls appended to its row
// (`Wow`, `Flutter`, `Saturation`, `HF Loss`).
//
// **OWED FOR THE REASON WRITTEN INTO THE RULE BELOW AN HOUR EARLIER, and this is its first use.**
// Nothing here changes a stored number's MEANING — every existing `Mode` value and every existing
// column is where it was — so the older test would have said no bump. The set of REPRESENTABLE
// VALUES grew: a row can now hold `Mode = 3`, which a build without Tape cannot represent and
// would clamp or misread SILENTLY. The version is what lets that build refuse instead of guess.
// **23, 2026-09-21, the delay's `Mode` gaining `Multi-Tap`** and four controls appended to its row
// (`Taps`, `Tap Shape`, `Tap Slope`, `Tap Width`). Owed under the forward-direction rule below, as
// 22 was: no stored number changes meaning, but a row can now hold `Mode = 4`, which a build without
// the multi-tap cannot represent.
//
// **24, 2026-09-21, SKLITTER's `Grain Order` — AN AUDITION CONTROL, INSERTED AT COLUMN 22** (backlog
// HU). It goes after `FB Tap` because it belongs with the two writers whose order it chooses, so the
// four waypoints and the perform trio each moved down one: `Cfg 1` 22→23 through `Perf Depth` 28→29.
// Every stored number in SKLITTER's row past column 21 changes meaning, which is the ordinary insert
// case this byte exists for.
//
// **AND IT IS OWED AGAIN WHEN THE CONTROL IS DELETED, which is unusual enough to say out loud.**
// `Param::GrainOrder` is SCAFFOLD: it exists so the choice between two blend orders can be made by ear
// (no strong opinion yet about A, B without hearing them), and the plan is to delete
// it and the losing order together once it is made. That removal moves these columns back UP one and
// costs another version byte. Both edits are correct; neither is a mistake the other repairs.
//
// **27, 2026-09-22, the shaper's five of backlog IE** — Bloom, Sag, Loop, Cheby and Stair, appended
// after `Deadzone` at columns 17..21. The shaper's travel block follows its params, so every travel
// column moves down five: the ordinary insert case.
// **28, 2026-09-22, SKLITTER's `Groove` INSERTED AT COLUMN 23** (backlog IG), before the perform block
// for `Send Override`'s reason: `Cfg 1` 23→24 through `Perf Depth` 29→30, so every stored number in
// SKLITTER's row past column 22 changes meaning. Written as 27 on its branch and RE-READ at the merge,
// where the shaper's five had taken 27 — the two-branches-one-digit hazard this file already records.
// **29, 2026-09-23 — the shaper's extras became field scalers and its travel grew to 18 (backlog II).**
// **30, 2026-09-23 — the shaper's six extras each gained a power switch, appended (backlog IK).**
// **31, 2026-09-23 — SKLITTER's `Groove` SPLIT into `Groove Mode` (23, a Choice of ten) and `Groove Amount`
// (24), backlog IO.** Column 23 changes meaning (a zoned 0..1 → a mode index) and the perform block moves
// down one more: `Cfg 1` 24→25 through `Perf Depth` 30→31.
// **32, 2026-09-23 — SKLITTER's `Free Phase` INSERTED at column 25 (backlog IO)**: Free's unbounded position,
// its own control beside `Groove Amount`. The perform block moves down one more: `Cfg 1` 25→26 through
// `Perf Depth` 31→32.
// **33, 2026-09-23 — Automod gained `Width` after `Rotate` (backlog IS)**, so its folded-travel blob, which starts at
// `Param::kCount`, moves down one column.
// **34, 2026-09-23 — Automod gained `Keytrack` after `Width` (backlog IS.5)**; its blob moves down one more.
// **35, 2026-09-23 — SKLITTER's `Free Phase` CHANGED MEANING (backlog IO addendum 2)**: no column moved, but a
// stored phase now names a different pattern — every wave starts at 0 (no `j·φ` start) and the off-beat floor is
// gone, so phase 0 is straight. **`kStoredMeaningFingerprint` DID NOT MOVE AND CANNOT**: it digests names and
// Choice tables, and this is the judgement case its own note leaves to a hand bump, like the LFO skews' rescale.
// **36, 2026-09-23 — the Shaper gained `Colour Mode` and `Inflate` (backlog IW)**: Inflate is a MODE, no longer a band of the
// gamma-zero line, so a stored Colour means the field alone and the blob moves down two columns.
// **37, 2026-09-23 — OWLTT gained `Character Mode` and `Vibe Mode` (backlog IW)**, appended: a stored Character or Vibe
// now means an in-mode value (amount and lap) unless its mode is Free.
// **41, 2026-09-24 — Featherverb appended `Conv Predelay` and `Conv Feedback` (backlog JD)**; its IR (a filename,
// the markers, stretch and the envelope) is a BODY (`device::BodyOps`), not columns.
// **42, 2026-09-24 — Featherverb's Conv Mode gained `Exciter`** (a fifth Choice entry; backlog JD).
// **43, 2026-09-24 — Featherverb's Size Mode gained `Aurora` before Vast** (a stored 6 is now Aurora, 7 Vast, 8 Owl).
// **44, 2026-09-24 — the Filter's `Type` dropped its four GPL-derived models (licence migration)**: Diode Ladder,
// OB-Xd, K35 LP and K35 HP are gone and Comb moved from 12 to 8, so a stored Type of 8 or more means something else.
// (Built as 42 on the licence branch; renumbered 44 when it merged after 42 and 43 above.)
// **45, 2026-09-25 — Featherverb gained `Oversample` after `Character` (backlog JE)**: every Conv column moves down one.
// (Built as 44 on conv-load; renumbered 45 when it merged after the licence migration's 44.)
// **46, 2026-09-25 — Featherverb gained `Owl Comp` after `Oversample` (an audition switch)**: the Conv columns move down one.
// **47, 2026-09-25 — the macros block (`FSMB`) gained each macro's type (Knob/Latch/Momentary) and name**, after the
// polarity floats: a stream can now hold a Latch or a name, which a build without them would misread as the next block.
// Per-device output labels ride in the embedded rack patch. (Built as 41 on macro-mod; renumbered 47 when it merged
// after 41..46 above.)
// **48, 2026-09-25 — Featherverb dropped `Owl Comp`** (the compressor is always on in Owl): the Conv columns move up one.
// (Built as 47 on conv-load; renumbered 48 when it merged after the macros block's 47.)
// **49, 2026-09-25 — THE CHAINS BLOCK BECAME A KEYED JSON DOCUMENT (backlog JF step 1c)**: `FSC1`, then the
// document's length, then the rack document (`rack::rackToText`) — every device's values by `ParamDesc::key`,
// Choices by label, routes by key, output labels by output key. The block's SHAPE changed, which is what this
// byte is for; it is the last bump a device edit will ever cause (see the policy below it, JF step 1d) — 48
// above is the last device edit that moved it. A clean break: no reader for 48. (Built as 47 on jf-step1;
// renumbered 48 after macro-mod's 47, then 49 after conv-load's 48.)
// **50, 2026-10-03 — THE STREAM ENDS IN A TRAILER (`state/state_trailer.h`)**: `FEND`, the payload's length and
// its CRC-32, after the last block. A load requires it before committing anything, so a stream cut anywhere —
// at a block boundary, which the loaders read as an older patch and filled with defaults — or with a flipped
// byte is refused whole.. The save is atomic too: serialised into one buffer, then written. NOT A CLEAN BREAK: projects may
// now be kept, so 49 is still read (`kStateVersionUntrailered`, below).
// **51, 2026-10-10 — THE `spectral` BLOCK IS APPENDED (additive design §3.1)**: the drawer's master switch and drawn
// curve, keyed. A 51 writer writes ten blocks; 50 and 49 are still read, and the block they predate is not looked for
// (`kStateBlockSince`) — so the bump refuses nothing that loaded before it.
// **52, 2026-10-26 — THE `likes` BLOCK IS APPENDED (backlog ON)**: the terrain's likes, keyed. 51 and older are still read.
// **53, 2026-11-06 — THE `preset` BLOCK IS APPENDED (backlog QC)**: which preset the patch came from, keyed. 52 and older are still read.
// **54, 2026-11-07 — THE `drumlanes` BLOCK IS APPENDED after it (drum proposal §17 P4)**: each kit pad's fx lane, keyed by pad. 53
// and older are still read. (Built as 53 on drum-engine; renumbered 54 when it merged after the preset block's 53.)
inline constexpr uint8_t kStateVersion = 54;

/// **50, READ AND NEVER WRITTEN** — the first version with the trailer, and the last before the spectral block.
/// Every version from here to `kStateVersion` carries the end marker.
inline constexpr uint8_t kStateVersionTrailered = 50;

/// **49, READ AND NEVER WRITTEN** — the last version without the trailer. A 49 stream has no end marker to
/// check, so it is held to the next best thing: every block a 49 writer knew must be present, because every 49 writer
/// wrote all of them, and a 49 stream missing one is a truncated one.
inline constexpr uint8_t kStateVersionUntrailered = 49;

/// **THE FIRST VERSION THAT WROTE EACH BLOCK**, positionally beside `kStateBlockOrder`. A stream older than a block is
/// not asked for it — the block takes its absent default — and a stream that IS that new must carry it. 0 = every
/// version this build reads.
inline constexpr std::array<uint8_t, kStateBlockOrder.size()> kStateBlockSince = {
	0, 0, 0, 0, 0, 0, 0, 0, 0, 51, 52, 53, 54,
};

/// Whether this build reads a stream of version `v`.
constexpr bool readableStateVersion(uint8_t v) noexcept {
	return v >= kStateVersionUntrailered && v <= kStateVersion;
}

//------------------------------------------------------------------------------------------------//
// **THE POLICY FROM HERE ON: A DEVICE EDIT DOES NOT MOVE `kStateVersion`** (backlog JF step 1d,      //
// 2026-09-25). Every entry above from 6 to 46 but one (the tag widening, 5) was a device's row moving //
// under a patch that read it by COLUMN — 35 of the 37 bumps since 2026-09-14 by the design's count.  //
//------------------------------------------------------------------------------------------------//
//
// **WHY IT NO LONGER HAS TO.** Since 49 a patch names every stored value by KEY (`device::ParamDesc::key`,
// `SourceDesc::key`, `Traits::extraStateKey`) and a Choice by its label, so a control inserted, removed,
// reordered or appended, a Choice table grown or reordered, a device added, a control renamed — each is a
// non-event for every saved patch: survivors keep their values, a new control takes its default, and the
// load report says so. The version byte would only refuse patches that now load correctly.
//
// **WHAT GUARDS A DEVICE EDIT INSTEAD: THE SCHEMA LEDGER**, `tests/state_schema.ledger`, checked by
// `test_state_ledger` — one line per key with its unit, range, steps and labels. A new key is a line to
// paste; a widened range or a gained label is a line to refresh; a change of MEANING (a rescale, a new unit,
// a label gone without a `ChoiceAlias`, a key gone without being retired) is refused until the key changes
// (the old one retired in `Traits::retiredKeys`, with its reason) or the device's `Traits::schema` moves with
// a migration. It sees the rescale the fingerprint it replaces could not (v17, v35 went through that one
// green), and two branches' lines merge as text where two digests merged into a number neither computed.
//
// **`kStoredMeaningFingerprint` (backlog DR, 2026-09-10) IS DELETED WITH THIS.** It did its job — the MSEG
// division table that grew in place, and v14 — and its history is in git (`git log -S
// kStoredMeaningFingerprint`), not here, because nothing reads it any more.
//
// **WHAT STILL MOVES THIS BYTE:** a change to the SHAPE of the state itself — a core block inserted, moved
// or changed, the chains block's framing, the envelope. Not a device's controls, and not a device's body
// (which versions itself: `conv::IrRef`, the ICB1 bundle).

} // namespace feathers
