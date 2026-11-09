/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

#pragma once

#include "fx/sklitter/groove.h" // GrooveFeel — the band is ruled on the feel the device publishes
#include "fx/sklitter/sklitter_types.h" // kGrainOnsetClasses — the onset counters' width
#include "plugin/ui/generic_card.h"

// **THE BAND ABOVE SKLITTER'S CONTROLS, AND WHY IT IS THE ARRANGEMENT RATHER THAN THE AUDIO.**
//
// SKLITTER is a flagship effect, so its picture gets extra effort: the buffer rendered as a gradient
// that re-orders the chunks into their shuffled order, with the modifiers of the other modes shown.
//
// A WAVEFORM WOULD SAY WHAT WAS RECORDED. What this device DOES is read that recording out of order,
// backwards, doubled, shortened, or not at all — so the picture is the READING, not the recording. Each
// chunk is drawn at its position in PLAYBACK order and coloured by where in the bar it reads FROM, which
// is exactly what a gradient encodes: a bar played straight is a smooth ramp, and every reordering is
// visible as a break in it. The modifiers ride on top of the same chunk.
//
// **IT DERIVES FROM `GenericCard` RATHER THAN REPLACING IT.** The list below the band is the same
// one-column, scrolling, mode-aware list every generic card draws, and SKLITTER gained nothing by having
// a second copy of it. `bodyTopInset` and `drawBodyExtras` are the whole extension surface.

namespace feathers::ui {

/// **ONE CHUNK OF THE ARRANGEMENT, DECODED.** A picture is not recoverable from a rendered frame —
/// this repo's rule — and "the strip changed" is true of any bug that alters it, so the band's input
/// is a value a case can assert.
struct SklitterChunk {
	float source = 0.0f; ///< where in the bar it reads from, 0..1
	bool reversed = false;
	bool pitchedUp = false;
	bool dry = false;
	bool known = false; ///< it has played at least once, so the rest of this means something
	/// **THE LONG GRAIN (backlog IX): how many steps this slice plays across.** 1 for an ordinary slice; the
	/// steps after a long one are `covered` and draw nothing of their own.
	int span = 1;
	bool covered = false;
	/// **THE RATCHET: how many times the slice restarts inside its slot**, 1 for none, and how long the piece
	/// that restarts is, as a fraction of the slot (floored at 24 ms by the device, so not `1 / ratchet`).
	int ratchet = 1;
	float piece = 0.0f;
};

/// **WHERE A RATCHETING CELL'S PIECES BEGIN, AS FRACTIONS OF THE CELL** — every restart that starts inside
/// the slot. Empty for a slice that does not ratchet. Shared by the card and the panel.
[[nodiscard]] std::vector<float> sklitterRatchetStarts(const SklitterChunk& c);

/// A split cell's two halves are this far apart, and a restart's tick is this wide — both in the bed's colour.
inline constexpr float kSklitterSplitGap = 1.0f;
inline constexpr float kSklitterTickWidth = 1.0f;
/// The narrowest piece a restart tick is drawn for, in pixels: a tick and two of fill.
inline constexpr float kSklitterMinPiece = 3.0f;
/// The narrowest cell, in DEVICE pixels, that carries its ratchet count as a numeral — so a 1.75x display
/// gets numerals where a 1x one does not.
inline constexpr float kSklitterCountMinCell = 8.0f;
/// The height of the head's "xN" tag.
inline constexpr float kHeadLabelHeight = 10.0f;

/// **THE RATCHET COUNT A CELL SHOWS AS A NUMERAL, or 0 for none**. Where the restart
/// ticks resolve they ARE the count, and a numeral would sit on them; where they do not, a cell at least
/// `kSklitterCountMinCell` device pixels wide prints its count on its solid top half. Below that the split alone
/// says "it restarts", and the head's tag names the count of the cell that plays. `cellWidth` is logical.
[[nodiscard]] int sklitterCountNumeral(const SklitterChunk& c, float cellWidth, float dpiScale);

/// **THE HEAD'S TAG — "xN" for the PLAYING cell when it ratchets, empty otherwise**.
/// The count is the one the device published for the head's step, which is the HEARD count of the slot playing
/// (`Sklitter::heardRatchet`), not the plan's. ASCII `x`: this atlas has no glyph for U+00D7 (`mod_panel.cpp`
/// draws its cross as strokes for that reason).
[[nodiscard]] std::string sklitterHeadRatchetLabel(const std::vector<SklitterChunk>& picture, int playing);

/// Draw the head's tag beside the head line at `headX` — on its left when the right would leave the band.
void drawSklitterHeadLabel(visage::Canvas& canvas, const visage::Font& font, const std::string& label, float headX,
                           float left, float wide, float top, float height);

/// **DRAW ONE CELL OF THE CHUNK FACE** at `x, top, w, h` — the card's band and the panel's strip share it.
/// A ratcheting cell is SPLIT HORIZONTALLY (a simple solid bar on the bottom, subdivisions on
/// top): both halves solid in the slice's colour, the top one cut by a tick at each restart. Every other
/// cell is the block it always was, pixel for pixel. With `numeralFont`, a ratcheting cell too narrow to tick
/// prints its count (`sklitterCountNumeral`).
void drawSklitterCell(visage::Canvas& canvas, const SklitterChunk& c, float x, float top, float w, float h,
                      float pitchTickTop, const visage::Font* numeralFont = nullptr);

/// **DRAW THE CHUNK FACE — every slot of the arrangement at the band's edges**, shared by the card and the
/// panel so the two cannot disagree about which steps own a cell. A long grain is ONE cell across every step
/// it plays; the steps under it (`covered`) draw NOTHING of their own. A step not yet known draws an outline
/// when `outlineUnknown` (the card at rest), nothing otherwise.
void drawSklitterChunkFace(visage::Canvas& canvas, const std::vector<SklitterChunk>& picture,
                           const std::vector<float>& edges, float left, float top, float wide, float height,
                           float pitchTickTop, bool outlineUnknown, const visage::Font* numeralFont = nullptr);

/// **THE PICTURE, READ OFF THE DEVICE'S READINGS.** Free rather than a member because the card and the
/// panel draw the same arrangement two ways and a second copy of the decode is a second place for the
/// flag bits to be read off the wrong half of a pair. Empty when nothing has been published.
[[nodiscard]] std::vector<SklitterChunk> sklitterChunks(const backplane::DeviceHost& host,
                                                      DeviceInstanceId device);

/// WHICH STEP IS PLAYING, or -1 when the device has published nothing.
[[nodiscard]] int sklitterPlayingStep(const backplane::DeviceHost& host, DeviceInstanceId device);

/// HOW FAR THROUGH THE PLAYING STEP, 0..1 — what makes the head sweep rather than jump once a slice.
[[nodiscard]] float sklitterStepPhase(const backplane::DeviceHost& host, DeviceInstanceId device);

/// **THE TWO LEVELS THE PERFORM CIRCLE'S SPARKS ARE EMITTED FROM, 0..1+ LINEAR, AND THEY PARTITION
/// THE SOUND** — `Reading::WetLevel` and `Reading::GrainLevel`, which the enumerators argue at length.
/// `wet` is what did NOT take the grain lane, `grain` is the lane's output, and their sum is the whole
/// of what this device is contributing.
///
/// **RETURNED AS A PAIR RATHER THAN BY TWO FUNCTIONS, AND THAT IS THE PARTITION MADE STRUCTURAL.** The
/// two are only a partition while they came from the same publish; two calls could straddle one, and
/// then "how much went each way" would be a ratio between two different moments — the fault the shared
/// hold window exists to prevent, reintroduced one layer up. One read, one pair.
struct SklitterLevels {
	float wet = 0.0f;
	float grain = 0.0f;
	/// **THE GRAIN ONSET COUNTERS, one per gate class** (`Reading::GrainOnsets`, backlog HR round 2) —
	/// monotone mod `kGrainOnsetWrap`, so the reader differences two reads. Read in the same pass as the
	/// levels because the pad gates onset sparks on them. `haveOnsets` is false when the device published
	/// nothing, which the pad must tell apart from "zero grains so far".
	std::array<uint32_t, fx::sklitter::kGrainOnsetClasses> onsets {};
	/// REPEAT'S RESTART COUNTER and the last repeat's period, seconds (`Reading::RepeatCount`/`Period`).
	uint32_t repeats = 0;
	float repeatPeriod = 0.0f;
	bool haveOnsets = false;
};

/// Zero on both when nothing has been published — which is also what a bypassed, unengaged or Mix-0
/// device really reads, so the two are indistinguishable ON PURPOSE: both mean "this instrument is
/// making no sound", and the one consumer wants the same answer for both.
///
/// **NOT CLAMPED ABOVE 1.** A device driven into a chain with gain in it can exceed full scale, and a
/// surface that clamped would tell the player their loudest moment and their second loudest were the
/// same. `PerformPad` clamps at the point it turns a number into a RATE, which is a drawing decision,
/// exactly as `busutil_meter.h` argues for its own peaks.
[[nodiscard]] SklitterLevels sklitterLevels(const backplane::DeviceHost& host,
                                            DeviceInstanceId device);

/// **HOW MANY BARS THE BAND SPANS**, from the device's own `Reading::SeqBars`. One when nothing has
/// been published, because a band ruled into zero bars divides by zero and a band ruled into one is
/// the honest picture of a one-bar sequence.
[[nodiscard]] int sklitterSeqBars(const backplane::DeviceHost& host, DeviceInstanceId device);

/// **THE RECORD LIGHT — backlog MJ.** The device's own `Reading::Recording`: whether a write landed in the buffer
/// during its last block. Off when nothing has been published. Read, never inferred from the settings — the card
/// cannot know whether the Record Gate is open, and only the device can say that a write actually landed.
[[nodiscard]] bool sklitterRecording(const backplane::DeviceHost& host, DeviceInstanceId device);
/// The light itself, a dot of radius `kSklitterRecordLightRadius` centred on (`cx`, `cy`): the record red while
/// `on`, an unlit ring while not, so the place is there whether or not it is recording. Card and panel share it.
inline constexpr float kSklitterRecordLightRadius = 3.0f;
void drawSklitterRecordLight(visage::Canvas& canvas, float cx, float cy, bool on);

/// **THE GROOVE THE BAND IS RULED ON** — `Reading::Groove` and `Reading::GrooveAmount`, the feel where it
/// acts and Straight where it does not (backlog IG; two readings since IO). Straight when nothing has been
/// published.
[[nodiscard]] fx::sklitter::GrooveFeel sklitterGroove(const backplane::DeviceHost& host, DeviceInstanceId device);

/// **WHERE EACH STEP OF THE BAND BEGINS, AS A FRACTION OF THE BAND — the SWUNG grid (backlog IG).**
/// `steps + 1` entries, the last 1. Each bar of the band takes an equal share and the steps inside it are
/// placed by `grooveWarp` of their straight phase, which is the device's own slice-boundary warp — so the
/// chunks are as wide as the slots that play them. At Straight this is `i / steps`. Shared by the card
/// and the panel, for `drawSklitterBarRules`' reason.
[[nodiscard]] std::vector<float> sklitterStepEdges(const fx::sklitter::GrooveFeel& groove, size_t steps,
                                                   int seqBars);

/// **HOW LONG ONE REPEAT READS, AS A FRACTION OF THE BUFFER** — `Reading::RepeatLength`, the scale the
/// chunks' `source` is on. Zero when nothing has been published and in every mode but Repeat, which is
/// what selects the chunk face over the repeat face.
[[nodiscard]] float sklitterRepeatLength(const backplane::DeviceHost& host, DeviceInstanceId device);

/// **REPEAT'S FACE, AS GEOMETRY A CASE CAN ASSERT** (a bar split horizontally: the bottom is the
/// repeating duration, the subdivisions on top).
///
/// THE TOP ROW IS THE SUBDIVISIONS: one cell per repeat, at the band's own (swung) edges and coloured by
/// where that repeat reads from — so its count is the step count, which is the `1/N` the Rate readout
/// prints. THE BOTTOM ROW IS THE REPEATING DURATION, in the ratchet's vocabulary (a
/// simple solid bar on the bottom, subdivisions on top): ONE SOLID BAR under each run of consecutive
/// repeats that read one place, exactly as wide as those repeats, never across a bar line. A repeat that
/// reads the same place every time draws one bar per bar of the band; one that wanders (Zone A's offset)
/// draws a bar per place. No faint remainder and no empty track — which is what the first face was.
///
/// All positions are fractions of the band, 0..1.
struct SklitterRepeatFace {
	struct Span {
		float from = 0.0f;    ///< `edges[first]`
		float to = 0.0f;      ///< `edges[last + 1]`
		size_t first = 0;     ///< the run's first step
		size_t last = 0;      ///< and its last
		int bar = 0;          ///< which bar of the band it belongs to
		bool playing = false; ///< the head is on one of its steps
		bool dry = false;     ///< the run plays the input, hollowed as every dry cell is
		float source = 0.0f;  ///< where in the capture it reads, 0..1 — its colour
	};
	std::vector<float> edges; ///< the top row's cells: `steps + 1` entries, the same edges the chunks use
	std::vector<Span> spans;  ///< the bottom row, one per run
	int seqBars = 1;
};

/// Built from the decoded picture and the one extra number; empty when `repeatLength` is zero.
[[nodiscard]] SklitterRepeatFace sklitterRepeatFace(const std::vector<SklitterChunk>& picture,
                                                    const fx::sklitter::GrooveFeel& groove, int seqBars,
                                                    float repeatLength, int playing);

/// **DRAW REPEAT'S FACE** in the band's rectangle — card and panel share it for `drawSklitterBarRules`'
/// reason. The head is the caller's, across both rows.
void drawSklitterRepeatFace(visage::Canvas& canvas, float left, float top, float wide, float height,
                            const std::vector<SklitterChunk>& picture, const SklitterRepeatFace& face,
                            const visage::Font* numeralFont = nullptr);

/// **RULE THE BAND INTO ITS BARS.** Shared by the card and the panel for the same reason the decode
/// is: two copies of "where does bar three start" is two places to get it wrong. Draws nothing at one
/// bar. See the .cpp for why the count comes from the device rather than from dividing `steps`.
void drawSklitterBarRules(visage::Canvas& canvas, float left, float top, float wide, float height,
                         size_t steps, int seqBars);

/// **THE GRADIENT, ACROSS HUE AND THROUGH THE PALETTE'S OWN CENTRES.**
///
/// The gradient runs over HUE, touching at least three colour centres, not just
/// opacity/saturation. The first version mixed `kMute` into `kAccent` — two blues a few degrees
/// apart — so position in the bar read as "dimmer or brighter" and two chunks a third of a bar apart
/// were hard to tell at a glance. That is a ramp you cannot ORDER by eye, which is the one thing this
/// picture has to support.
///
/// FOUR STOPS, ALL OF THEM TOKENS THIS EDITOR ALREADY USES, walked in hue rather than invented:
/// `kMute` (grey-blue) -> `kAccent` (cool blue) -> `kMod` (violet) -> `kValue` (warm amber). Cool to
/// warm across the whole bar, with two distinct centres in between, so a chunk's colour says roughly
/// WHERE it came from before the eye has compared it to anything.
[[nodiscard]] uint32_t sklitterRamp(float t);

class SklitterCard final : public GenericCard {
public:
	using GenericCard::GenericCard;

	/// HOW TALL THE BAND IS. A strip rather than a panel: on the card this is a glance, and the
	/// expanded view is where it is worth looking at.
	static constexpr float kBandHeight = 26.0f;
	/// The gap between the band and the first row.
	static constexpr float kBandGap = 6.0f;

	/// TEST SEAM — the band's input, decoded. See `sklitterChunks`.
	using Chunk = SklitterChunk;
	[[nodiscard]] std::vector<Chunk> chunksForTest() const { return chunks(); }
	/// TEST SEAM — the head's "xN" tag this card draws, off its own readings.
	[[nodiscard]] std::string headLabelForTest() const {
		return sklitterHeadRatchetLabel(chunks(), sklitterPlayingStep(ownHost(), deviceId()));
	}
	[[nodiscard]] int playingStepForTest() const;
	/// TEST SEAM — the Repeat face this card draws, off its own readings; empty edges mean the chunk face.
	[[nodiscard]] SklitterRepeatFace repeatFaceForTest() const { return repeatFace(chunks()); }
	/// TEST SEAM — whether the card would ask for a frame right now. The REPORT; the delivery is the
	/// `redraw()` inside, which `visage::Frame` gives no way to observe.
	bool updateContentForTest() { return updateContent(); }
	/// TEST SEAM — backlog MJ: whether the record light this surface draws is lit, off its own readings.
	[[nodiscard]] bool recordLightForTest() const { return sklitterRecording(ownHost(), deviceId()); }

protected:
	float bodyTopInset() const override { return kBandHeight + kBandGap; }
	void drawBodyExtras(visage::Canvas& canvas) override;

	/// **ASK FOR THE FRAME, AND ASKING MEANS CALLING `redraw()`.** The graph updated only sometimes,
	/// and the playhead moved only when a param changed. The band had no `updateContent` at all,
	/// so the only thing that ever repainted the card was something else doing it — a knob drag, a
	/// param arriving — and between those the picture simply sat there.
	///
	/// **THIS IS THE SECOND TIME THIS EXACT BUG HAS BEEN WRITTEN HERE.** `busutil_card.cpp` carries the
	/// first, at length: *"this card returned `true` and asked for nothing, so the meter computed
	/// correctly, published correctly, compared correctly, and never drew"* — and beside it, that the
	/// case which should have caught it asserted the RETURN VALUE, which is the report and not the
	/// delivery. `FxLane::updateCards` only COUNTS cards that report movement; it repaints none of
	/// them. So this compares, redraws, and then reports.
	bool updateContent() override;

private:
	/// WHAT WAS LAST DRAWN, so `updateContent` can ask whether the picture actually moved.
	std::vector<Chunk> drawn_;
	int drawnStep_ = -2;
	float drawnPhase_ = -1.0f;
	float drawnFill_ = -1.0f;
	int drawnSeqBars_ = -1;
	/// backlog IG — the grid can move while the chunks do not. `kCount` is a mode nothing publishes, so
	/// the first tick always draws.
	fx::sklitter::GrooveFeel drawnGroove_ { fx::sklitter::GrooveMode::kCount, -1.0 };
	/// `Reading::RepeatLength` as last drawn — Repeat's bottom row can change while nothing else does.
	float drawnRepeat_ = -1.0f;
	/// backlog MJ — the record light as last drawn (1 lit, 0 dark); -1 so the first tick draws it.
	int drawnRecording_ = -1;

	[[nodiscard]] std::vector<Chunk> chunks() const {
		return sklitterChunks(ownHost(), deviceId());
	}
	/// Repeat's face over `picture`, from this card's readings — the one builder the draw and the seam share.
	[[nodiscard]] SklitterRepeatFace repeatFace(const std::vector<Chunk>& picture) const {
		return sklitterRepeatFace(picture, sklitterGroove(ownHost(), deviceId()), sklitterSeqBars(ownHost(), deviceId()),
		                          sklitterRepeatLength(ownHost(), deviceId()), sklitterPlayingStep(ownHost(), deviceId()));
	}
};

} // namespace feathers::ui
