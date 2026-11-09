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

#include "plugin/ui/sklitter_card.h"

#include "fx/sklitter/sklitter_types.h"
#include "plugin/ui/theme.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace feathers::ui {

namespace sc = feathers::fx::sklitter;

namespace {

/// **ONTO THE DEVICE PIXEL GRID** (ribbon truth). A sixty-four-step band puts every cell edge at a fraction of
/// a pixel, and a hollow (dry) cell drawn there is a one-pixel border beside a half-covered column of mixed
/// colour — PHOTOGRAPHED as an empty cell with a shaded stripe down one side, which is how the partially
/// shaded empty cells looked. Edges snapped to whole device pixels draw every border whole.
float snapToPixel(visage::Canvas& canvas, float v) {
	const float scale = canvas.dpiScale() > 0.0f ? canvas.dpiScale() : 1.0f;
	return std::round(v * scale) / scale;
}

/// One whole device pixel, at least one logical pixel wide — a hollow cell's border.
float wholePixel(visage::Canvas& canvas) {
	const float scale = canvas.dpiScale() > 0.0f ? canvas.dpiScale() : 1.0f;
	return std::max(1.0f, std::round(scale)) / scale;
}

} // namespace


uint32_t sklitterRamp(float t) {
	// THE STOPS, in the order the bar walks them. See the header for why these four.
	// NOT `static`: the stops are live chrome roles (backlog LG), and a static copy would keep the first frame's hue.
	const uint32_t kStops[] = { kMuted, kAccent, kModulation, kValue };
	constexpr size_t kCount = sizeof(kStops) / sizeof(kStops[0]);

	const float x = std::clamp(t, 0.0f, 1.0f) * static_cast<float>(kCount - 1);
	const auto lo = static_cast<size_t>(x);
	const size_t hi = std::min(lo + 1u, kCount - 1u);
	const float f = x - static_cast<float>(lo);

	// **MIXED PER CHANNEL BETWEEN TWO NEIGHBOURING STOPS**, which walks the hue because the stops do.
	// A single lerp between the two ENDS would pass through grey in the middle, which is the
	// desaturation the first version had and the reason it read as brightness.
	const auto mix = [&](int shift) {
		const auto a = static_cast<float>((kStops[lo] >> shift) & 0xFFu);
		const auto b = static_cast<float>((kStops[hi] >> shift) & 0xFFu);
		return static_cast<uint32_t>(std::lround(a + (b - a) * f)) & 0xFFu;
	};
	return 0xFF000000u | (mix(16) << 16) | (mix(8) << 8) | mix(0);
}

float sklitterStepPhase(const backplane::DeviceHost& host, DeviceInstanceId device) {
	if (host.readingCountAt(device) < static_cast<size_t>(sc::Reading::kHeaderCount)) {
		return 0.0f;
	}
	return std::clamp(host.readingAt(device, static_cast<size_t>(sc::Reading::StepPhase)), 0.0f, 1.0f);
}

std::vector<SklitterChunk> sklitterChunks(const backplane::DeviceHost& host, DeviceInstanceId device) {
	std::vector<SklitterChunk> out;
	const size_t count = host.readingCountAt(device);
	if (count < static_cast<size_t>(sc::Reading::kHeaderCount)) {
		return out; // nothing published yet — the card draws an empty band, not a guess
	}
	const auto steps = static_cast<size_t>(
	    std::lround(host.readingAt(device, static_cast<size_t>(sc::Reading::Steps))));
	const size_t have = (count - static_cast<size_t>(sc::Reading::kHeaderCount)) / sc::kStepStride;
	out.reserve(std::min(steps, have));
	for (size_t i = 0; i < steps && i < have; ++i) {
		const size_t base = sc::stepReading(i);
		SklitterChunk c;
		c.source = host.readingAt(device, base);
		const auto flags = static_cast<uint32_t>(std::lround(host.readingAt(device, base + 1u)));
		c.reversed = (flags & static_cast<uint32_t>(sc::StepFlag::Reversed)) != 0u;
		c.pitchedUp = (flags & static_cast<uint32_t>(sc::StepFlag::PitchedUp)) != 0u;
		c.dry = (flags & static_cast<uint32_t>(sc::StepFlag::Dry)) != 0u;
		c.known = (flags & static_cast<uint32_t>(sc::StepFlag::Known)) != 0u;
		c.covered = (flags & static_cast<uint32_t>(sc::StepFlag::Covered)) != 0u;
		c.span = 1 + static_cast<int>((flags >> sc::kStepSpanShift) & sc::kStepSpanMask);
		c.ratchet = 1 + static_cast<int>((flags >> sc::kStepRatchetShift) & sc::kStepCountMask);
		const float piece = host.readingAt(device, base + 2u);
		c.piece = c.ratchet > 1 && std::isfinite(piece) ? std::clamp(piece, 0.0f, 1.0f) : 0.0f;
		out.push_back(c);
	}
	return out;
}

std::vector<float> sklitterRatchetStarts(const SklitterChunk& c) {
	std::vector<float> starts;
	if (c.ratchet <= 1 || c.piece <= 0.0f) {
		return starts;
	}
	// EVERY RESTART THAT BEGINS INSIDE THE SLOT. A floored piece times the count can overrun the slot, and
	// the device's `whichSub` never reaches the ones that would start past it.
	for (int j = 0; j < c.ratchet; ++j) {
		const float at = static_cast<float>(j) * c.piece;
		if (at >= 1.0f) {
			break;
		}
		starts.push_back(at);
	}
	return starts;
}

int sklitterCountNumeral(const SklitterChunk& c, float cellWidth, float dpiScale) {
	if (!c.known || c.covered || c.dry || c.ratchet <= 1 || c.piece <= 0.0f) {
		return 0;
	}
	if (c.piece * cellWidth >= kSklitterMinPiece) {
		return 0; // the ticks resolve, and they ARE the count
	}
	const float scale = dpiScale > 0.0f ? dpiScale : 1.0f;
	return cellWidth * scale >= kSklitterCountMinCell ? c.ratchet : 0;
}

std::string sklitterHeadRatchetLabel(const std::vector<SklitterChunk>& picture, int playing) {
	if (playing < 0 || static_cast<size_t>(playing) >= picture.size()) {
		return {};
	}
	const SklitterChunk& c = picture[static_cast<size_t>(playing)];
	if (!c.known || c.covered || c.dry || c.ratchet <= 1) {
		return {};
	}
	return "x" + std::to_string(c.ratchet);
}

void drawSklitterHeadLabel(visage::Canvas& canvas, const visage::Font& font, const std::string& label, float headX,
                           float left, float wide, float top, float height) {
	if (label.empty()) {
		return;
	}
	// A TAG ON THE HEAD, in the bed's colour so it reads as a label and not as a cell — beside the line, and on
	// its left when the right would run off the band.
	const std::u32string wide32(label.begin(), label.end()); // ASCII by construction
	const float textW = font.stringWidth(wide32) + 4.0f;
	float x = headX + 3.0f;
	if (x + textW > left + wide) {
		x = headX - 1.5f - textW;
	}
	canvas.setColor(kSurfaceRaised);
	canvas.roundedRectangle(x, top, textW, height, 1.5f);
	canvas.setColor(kInk);
	canvas.text(label.c_str(), font, visage::Font::kCenter, x, top, textW, height);
}

void drawSklitterCell(visage::Canvas& canvas, const SklitterChunk& c, float x, float top, float w, float h,
                      float pitchTickTop, const visage::Font* numeralFont) {
	// **THE COLOUR IS WHERE IT READS FROM.** That is the whole idea: playback order along the band, source
	// position in the colour, so a straight bar is a smooth ramp and every reordering is a break in it.
	const std::vector<float> starts = sklitterRatchetStarts(c);
	if (starts.empty()) {
		canvas.setColor(sklitterRamp(c.source));
		canvas.roundedRectangle(x, top, w, h, 1.5f);
	}
	else {
		// **A RATCHET SPLITS THE CELL HORIZONTALLY, AND BOTH HALVES ARE SOLID** (by design, a simple solid
		// bar on the bottom and subdivisions on top). The bottom half is the slot,
		// one bar in the slice's own colour across the whole cell. The top half is the same colour cut at every
		// restart by a tick in the bed's colour — N pieces, N − 1 ticks, each where that restart really begins,
		// the last piece running to the cell's end. It drew the repeating piece at its real length on the bottom
		// with the rest of the slot faint, and the pieces on top with gaps where the audible length ended:
		// partially filled, partially greyed.
		const float half = std::max(1.0f, (h - kSklitterSplitGap) * 0.5f);
		const float lowTop = top + half + kSklitterSplitGap;
		canvas.setColor(sklitterRamp(c.source));
		canvas.roundedRectangle(x, top, w, half, 1.0f);
		canvas.roundedRectangle(x, lowTop, w, half, 1.0f);
		// **TICKS ONLY WHERE THEY RESOLVE.** Nine pieces in a six-pixel cell are eight ticks with no fill
		// between them — PHOTOGRAPHED as a dark top with coloured edges, the very "partially shaded empty
		// cell" this replaces. Where a piece is narrower than `kSklitterMinPiece` the top stays solid: the
		// split still says "it restarts", and the count reads wherever the cell is wide enough to hold it.
		const float narrowest = c.piece * w;
		if (narrowest >= kSklitterMinPiece) {
			canvas.setColor(kSurfaceRaised);
			for (size_t j = 1; j < starts.size(); ++j) {
				canvas.fill(snapToPixel(canvas, x + starts[j] * w), top, kSklitterTickWidth, half);
			}
		}
		// **AND WHERE THEY DO NOT, THE COUNT AS A NUMERAL** (item (a)) — on the solid top half the
		// ticks would have cut, in ink. See `sklitterCountNumeral` for when.
		if (const int n = numeralFont != nullptr ? sklitterCountNumeral(c, w, canvas.dpiScale()) : 0; n > 0) {
			const std::string count = std::to_string(n);
			canvas.setColor(kInk);
			canvas.text(count.c_str(), *numeralFont, visage::Font::kCenter, x, top, w, half);
		}
	}

	// **THE MODIFIERS RIDE ON THE CHUNK THEY BELONG TO.** A legend somewhere else would be a second thing
	// to read; these are marks on the block itself.
	if (c.reversed) {
		// A BACKWARD NOTCH: the chunk's own colour taken out of its leading edge, so a reversed block reads
		// as pointing the other way.
		canvas.setColor(kSurfaceRaised);
		canvas.triangleLeft(x + 1.0f, top + h * 0.5f - 3.0f, 6.0f);
	}
	if (c.pitchedUp) {
		// A TICK ABOVE: an octave is a step up, drawn as one.
		canvas.setColor(kInk);
		canvas.fill(x + w * 0.5f - 1.0f, pitchTickTop, 2.0f, 2.0f);
	}
	if (c.dry) {
		// DRY IS THE INPUT, NOT THE BUFFER, so the block is hollowed: a border where the fill was — one whole
		// device pixel, so it is a line and not a shade.
		const float border = wholePixel(canvas);
		canvas.setColor(kSurfaceRaised);
		canvas.roundedRectangle(x + border, top + border, std::max(0.0f, w - 2.0f * border), h - 2.0f * border, 1.0f);
	}
}

void drawSklitterChunkFace(visage::Canvas& canvas, const std::vector<SklitterChunk>& picture,
                           const std::vector<float>& edges, float left, float top, float wide, float height,
                           float pitchTickTop, bool outlineUnknown, const visage::Font* numeralFont) {
	if (edges.size() != picture.size() + 1u) {
		return;
	}
	for (size_t i = 0; i < picture.size(); ++i) {
		const SklitterChunk& c = picture[i];
		// **A STEP UNDER A LONG GRAIN DRAWS NOTHING OF ITS OWN** — the grain's cell already spans it.
		if (c.covered) {
			continue;
		}
		const size_t end = std::min(picture.size(), i + static_cast<size_t>(std::max(1, c.span)));
		// a hairline between chunks, so the cuts read — and a long grain is ONE cell across every step it
		// plays (backlog IX), as wide as its slot really is. On whole device pixels: see `snapToPixel`.
		const float x = snapToPixel(canvas, left + edges[i] * wide);
		const float w = std::max(wholePixel(canvas), snapToPixel(canvas, left + edges[end] * wide) - x - wholePixel(canvas));
		if (!c.known) {
			// **AN EMPTY SLOT, NOT A GAP.** A step that has not played yet has nothing true to say about where
			// it reads from — but the CUT is known, and showing it is the whole point of publishing the
			// arrangement at rest: the player can see the bar divided before engaging. An outline, so it reads
			// as "a place for a chunk" rather than as a chunk.
			if (outlineUnknown) {
				canvas.setColor(withAlpha(kLine, 0.35f));
				canvas.roundedRectangleBorder(x, top, w, height, 1.5f, 1.0f);
			}
			continue;
		}
		drawSklitterCell(canvas, c, x, top, w, height, pitchTickTop, numeralFont);
	}
}

int sklitterPlayingStep(const backplane::DeviceHost& host, DeviceInstanceId device) {
	if (host.readingCountAt(device) < static_cast<size_t>(sc::Reading::kHeaderCount)) {
		return -1;
	}
	return static_cast<int>(
	    std::lround(host.readingAt(device, static_cast<size_t>(sc::Reading::Step))));
}

SklitterLevels sklitterLevels(const backplane::DeviceHost& host, DeviceInstanceId device) {
	SklitterLevels out;
	if (host.readingCountAt(device) < static_cast<size_t>(sc::Reading::kHeaderCount)) {
		return out;
	}
	// **A NON-FINITE READS AS SILENCE.** The bridge scrubs at the publish and the rack's `atomic_ref`
	// road does not, and this is the last place before a number becomes an emission rate — see
	// `Sklitter::foldOutLevels` for why a NaN must not get that far.
	const auto clean = [](float v) { return std::isfinite(v) && v > 0.0f ? v : 0.0f; };
	// **BOTH READ HERE, BETWEEN NO OTHER CALL.** See `SklitterLevels` for why the pair is the unit.
	out.wet = clean(host.readingAt(device, static_cast<size_t>(sc::Reading::WetLevel)));
	out.grain = clean(host.readingAt(device, static_cast<size_t>(sc::Reading::GrainLevel)));
	// A counter that is not a finite non-negative integer below the wrap is not a counter; zero it and
	// let the pad's re-baseline absorb the jump rather than emit on it.
	const auto counter = [&](sc::Reading at, size_t offset) -> uint32_t {
		const float v = host.readingAt(device, static_cast<size_t>(at) + offset);
		return std::isfinite(v) && v >= 0.0f && v < static_cast<float>(sc::kGrainOnsetWrap) ? static_cast<uint32_t>(v)
		                                                                                   : 0u;
	};
	for (size_t c = 0; c < sc::kGrainOnsetClasses; ++c) {
		out.onsets[c] = counter(sc::Reading::GrainOnsets, c);
	}
	out.repeats = counter(sc::Reading::RepeatCount, 0);
	out.repeatPeriod = clean(host.readingAt(device, static_cast<size_t>(sc::Reading::RepeatPeriod)));
	out.haveOnsets = true;
	return out;
}

int sklitterSeqBars(const backplane::DeviceHost& host, DeviceInstanceId device) {
	if (host.readingCountAt(device) < static_cast<size_t>(sc::Reading::kHeaderCount)) {
		return 1;
	}
	const int bars = static_cast<int>(
	    std::lround(host.readingAt(device, static_cast<size_t>(sc::Reading::SeqBars))));
	// **ONE IS THE FLOOR, AND A DEVICE THAT HAS PUBLISHED NOTHING READS ZERO.** A band ruled into zero
	// bars would divide by it; a band ruled into one draws no interior line, which is the honest
	// picture of a one-bar sequence.
	return bars > 0 ? bars : 1;
}

bool sklitterRecording(const backplane::DeviceHost& host, DeviceInstanceId device) {
	if (host.readingCountAt(device) < static_cast<size_t>(sc::Reading::kHeaderCount)) {
		return false;
	}
	return host.readingAt(device, static_cast<size_t>(sc::Reading::Recording)) > 0.5f;
}

void drawSklitterRecordLight(visage::Canvas& canvas, float cx, float cy, bool on) {
	// THE RECORD RED — the master meter's and the bus utility's over-red, because a lit red dot already means
	// "recording" to anyone who has used a tape machine; a data colour, so outside the hue offset.
	constexpr uint32_t kRecordRed = 0xffD9483B;
	const float r = kSklitterRecordLightRadius;
	if (on) {
		canvas.setColor(kRecordRed);
		canvas.circle(cx - r, cy - r, 2.0f * r);
	}
	else {
		canvas.setColor(withAlpha(kLine, 0.9f));
		canvas.ring(cx - r, cy - r, 2.0f * r, 1.0f);
	}
}

sc::GrooveFeel sklitterGroove(const backplane::DeviceHost& host, DeviceInstanceId device) {
	if (host.readingCountAt(device) < static_cast<size_t>(sc::Reading::kHeaderCount)) {
		return {};
	}
	const float mode = host.readingAt(device, static_cast<size_t>(sc::Reading::Groove));
	const float amount = host.readingAt(device, static_cast<size_t>(sc::Reading::GrooveAmount));
	const sc::GrooveMode feel = sc::grooveModeOf(static_cast<double>(mode));
	if (!std::isfinite(amount)) {
		return { feel, 0.0 };
	}
	// **FREE'S AMOUNT IS ITS PHASE AND HAS NO ENDS (backlog IO addendum).** The 0..1 clamp is a DEPTH's rule,
	// and applied to Free it drew every phase past 1 as 1 and every one below 0 as 0: the band froze while
	// the phase moved, which read as a bounded control and as a ribbon that "does not update every time".
	// `Settings::grooveFeel` is the one decode — it clamps a depth and leaves a phase — and this follows it.
	return { feel, sc::isFree(feel) ? static_cast<double>(amount) : std::clamp(static_cast<double>(amount), 0.0, 1.0) };
}

std::vector<float> sklitterStepEdges(const sc::GrooveFeel& feel, size_t steps, int seqBars) {
	std::vector<float> edges(steps + 1, 1.0f);
	if (steps == 0) {
		return edges;
	}
	const size_t bars = seqBars > 0 ? static_cast<size_t>(seqBars) : 1u;
	// THE DEVICE CUTS WHOLE BARS INTO THE BAND (`publishReadings` drops a partial one), so the steps divide
	// into the bars exactly; a band that somehow did not falls back to the straight grid it would be.
	const size_t perBar = steps % bars == 0 ? steps / bars : steps;
	const size_t drawnBars = steps % bars == 0 ? bars : 1u;
	for (size_t i = 0; i < steps; ++i) {
		const size_t bar = i / perBar;
		const double at = static_cast<double>(i % perBar) / static_cast<double>(perBar);
		edges[i] = static_cast<float>((static_cast<double>(bar) + sc::grooveWarp(feel, at))
		                              / static_cast<double>(drawnBars));
	}
	return edges;
}

float sklitterRepeatLength(const backplane::DeviceHost& host, DeviceInstanceId device) {
	if (host.readingCountAt(device) < static_cast<size_t>(sc::Reading::kHeaderCount)) {
		return 0.0f;
	}
	const float v = host.readingAt(device, static_cast<size_t>(sc::Reading::RepeatLength));
	return std::isfinite(v) ? std::clamp(v, 0.0f, 1.0f) : 0.0f;
}

SklitterRepeatFace sklitterRepeatFace(const std::vector<SklitterChunk>& picture, const sc::GrooveFeel& groove,
                                      int seqBars, float repeatLength, int playing) {
	SklitterRepeatFace face;
	if (repeatLength <= 0.0f || picture.empty()) {
		return face;
	}
	const size_t steps = picture.size();
	face.edges = sklitterStepEdges(groove, steps, seqBars);
	// THE BARS THE EDGES WERE CUT INTO, by `sklitterStepEdges`' own rule, so a span never crosses a bar line.
	const size_t asked = seqBars > 0 ? static_cast<size_t>(seqBars) : 1u;
	const size_t bars = steps % asked == 0 ? asked : 1u;
	const size_t perBar = steps / bars;
	face.seqBars = static_cast<int>(bars);
	// **ONE SOLID BAR PER RUN OF REPEATS THAT READ ONE PLACE — the ratchet's vocabulary** (a simple
	// solid bar on the bottom, subdivisions on top). The bar is as wide as the repeats that make
	// it, at the band's own edges, so the top row's cells sit exactly on it. It was the capture drawn as an
	// empty track per bar with the read span shaded inside it — the partially shaded empty cells.
	for (size_t i = 0; i < steps; ++i) {
		const SklitterChunk& c = picture[i];
		if (!c.known || c.covered) {
			continue;
		}
		const auto bar = static_cast<int>(i / perBar);
		const bool isPlaying = playing >= 0 && static_cast<size_t>(playing) == i;
		SklitterRepeatFace::Span* last = face.spans.empty() ? nullptr : &face.spans.back();
		if (last != nullptr && last->bar == bar && last->last + 1 == i && last->source == c.source && last->dry == c.dry) {
			last->last = i;
			last->to = face.edges[i + 1];
			last->playing = last->playing || isPlaying;
			continue;
		}
		SklitterRepeatFace::Span s;
		s.bar = bar;
		s.first = i;
		s.last = i;
		s.from = face.edges[i];
		s.to = face.edges[i + 1];
		s.source = c.source;
		s.dry = c.dry;
		s.playing = isPlaying;
		face.spans.push_back(s);
	}
	return face;
}

void drawSklitterRepeatFace(visage::Canvas& canvas, float left, float top, float wide, float height,
                            const std::vector<SklitterChunk>& picture, const SklitterRepeatFace& face,
                            const visage::Font* numeralFont) {
	if (face.edges.size() != picture.size() + 1u) {
		return;
	}
	// **SPLIT HORIZONTALLY, AS ASKED: the subdivisions on top, the repeating duration beneath.**
	constexpr float kRowGap = 2.0f;
	const float rowH = std::max(1.0f, (height - kRowGap) * 0.5f);
	const float lowTop = top + rowH + kRowGap;

	// THE TOP ROW — one cell per repeat, where it plays and coloured by where it reads, marks and all: THE SAME
	// FACE the chunks draw, so a Repeat slot that ratchets splits here too (backlog IX).
	drawSklitterChunkFace(canvas, picture, face.edges, left, top, wide, rowH, top - 3.0f, true, numeralFont);

	// THE BOTTOM ROW — one solid bar under each run of repeats that read one place, in that place's colour,
	// every one at full strength: which run is playing is the head's to say, not a fade's.
	const float px = wholePixel(canvas);
	for (const SklitterRepeatFace::Span& s : face.spans) {
		const float x = snapToPixel(canvas, left + s.from * wide);
		const float w = std::max(px, snapToPixel(canvas, left + s.to * wide) - x - px);
		canvas.setColor(sklitterRamp(s.source));
		canvas.roundedRectangle(x, lowTop, w, rowH, 1.5f);
		if (s.dry) {
			// DRY IS THE INPUT, hollowed as every dry cell is — see `drawSklitterCell`.
			canvas.setColor(kSurfaceRaised);
			canvas.roundedRectangle(x + px, lowTop + px, std::max(0.0f, w - 2.0f * px), rowH - 2.0f * px, 1.0f);
		}
	}
}

void drawSklitterBarRules(visage::Canvas& canvas, float left, float top, float wide, float height,
                         size_t steps, int seqBars) {
	// **THE BAR LINES, WHICH ARE WHAT MAKE A CRAMMED SEQUENCE READABLE.** Cram was chosen over page
	// (it makes varying sequence lengths more naturally expressible), and the thing that makes
	// a 64-chunk band legible as FOUR BARS rather than as 64 chunks is the ruling.
	//
	// **DRAWN FROM THE DEVICE'S OWN `SeqBars`, NOT INFERRED.** Dividing `steps` by a slice count the
	// card would have to guess at is a second opinion about the cut, and the two would disagree the
	// first time a rate change landed between two publishes.
	//
	// NO INTERIOR LINE AT ONE BAR — there is nothing to separate, and a rule at the far edge would
	// read as a chunk boundary that is not there.
	if (seqBars <= 1 || steps == 0) {
		return;
	}
	const auto perBar = static_cast<float>(steps) / static_cast<float>(seqBars);
	const float step = wide / static_cast<float>(steps);
	canvas.setColor(withAlpha(kLine, 0.55f));
	for (int b = 1; b < seqBars; ++b) {
		const float x = left + perBar * static_cast<float>(b) * step;
		canvas.fill(x - 0.5f, top, 1.0f, height);
	}
}

int SklitterCard::playingStepForTest() const {
	return sklitterPlayingStep(ownHost(), deviceId());
}

bool SklitterCard::updateContent() {
	// THE LIST'S OWN ANSWER FIRST — a mode change re-lays the rows out and that is a repaint too.
	bool moved = GenericCard::updateContent();

	// **WHAT THE BAND DRAWS, COMPARED RATHER THAN ASSUMED.** A card that redrew every tick would
	// repaint at the host's timer rate whether or not anything had changed; `busutil_card.cpp` states
	// that rule and automod's strip states it from the other side.
	//
	// THE PHASE IS IN THE COMPARISON, and it is what makes the head SWEEP: without it the picture
	// changes only when the slice does, so the head would jump a few times a bar and sit still
	// between — which is most of what "only updating sometimes" looked like.
	const int step = sklitterPlayingStep(ownHost(), deviceId());
	const float phase = sklitterStepPhase(ownHost(), deviceId());
	// **THE FILL IS IN THE COMPARISON TOO**, or the capture bar would creep without anyone asking for
	// the frames that draw it — the same omission that made the band static, one layer down.
	const float fill = ownHost().readingAt(deviceId(), static_cast<size_t>(sc::Reading::Fill));
	// **AND THE BAR COUNT, because the RULING can move while the chunks do not.** At a fine cut both
	// a two-bar and a four-bar sequence clamp to the same 64 steps, so the chunk comparison alone
	// could report "nothing moved" for a band that must be ruled differently.
	const int bars = sklitterSeqBars(ownHost(), deviceId());
	const sc::GrooveFeel groove = sklitterGroove(ownHost(), deviceId());
	const float repeat = sklitterRepeatLength(ownHost(), deviceId()); // Repeat's face can move alone too
	// backlog MJ: the record light can change while nothing else on the band does.
	const int recording = sklitterRecording(ownHost(), deviceId()) ? 1 : 0;
	const std::vector<SklitterChunk> now = chunks();
	if (step != drawnStep_ || phase != drawnPhase_ || fill != drawnFill_ || bars != drawnSeqBars_
	    || groove != drawnGroove_ || repeat != drawnRepeat_ || recording != drawnRecording_
	    || now.size() != drawn_.size()) {
		moved = true;
	}
	else {
		for (size_t i = 0; i < now.size(); ++i) {
			if (now[i].source != drawn_[i].source || now[i].known != drawn_[i].known
			    || now[i].reversed != drawn_[i].reversed || now[i].pitchedUp != drawn_[i].pitchedUp
			    || now[i].dry != drawn_[i].dry || now[i].span != drawn_[i].span
			    || now[i].covered != drawn_[i].covered || now[i].ratchet != drawn_[i].ratchet
			    || now[i].piece != drawn_[i].piece) {
				moved = true;
				break;
			}
		}
	}
	if (!moved) {
		return false;
	}
	drawnStep_ = step;
	drawnPhase_ = phase;
	drawnFill_ = fill;
	drawnSeqBars_ = bars;
	drawnGroove_ = groove;
	drawnRepeat_ = repeat;
	drawnRecording_ = recording;
	drawn_ = now;
	// **THE DELIVERY.** Returning true is the report; this is the line that actually puts a frame on
	// the screen, and the absence of it is the whole of the reported defect.
	redraw();
	return true;
}

void SklitterCard::drawBodyExtras(visage::Canvas& canvas) {
	const float left = bodyLeft() + kPad;
	const float top = kHeadHeight + kRowGap - bodyScroll();
	const float wide = std::max(0.0f, static_cast<float>(width()) - (left - bodyLeft()) - 2.0f * kPad);
	if (wide <= 0.0f || folded()) {
		return;
	}

	// THE BED. Drawn whatever the device is doing, so the band is a place rather than something that
	// appears and vanishes — a strip that came and went as the gesture did would make the card jump.
	canvas.setColor(kSurfaceRaised);
	canvas.roundedRectangle(left, top, wide, kBandHeight, 3.0f);

	// **THE RECORD LIGHT (backlog MJ), IN THE BAND'S TOP-RIGHT CORNER AND DRAWN LAST** — over the chunks, so a busy
	// band cannot bury it. Every road out of this function below the bed goes through it.
	const auto recordLight = [&] {
		drawSklitterRecordLight(canvas, left + wide - kSklitterRecordLightRadius - 2.0f,
		                        top + kSklitterRecordLightRadius + 2.0f, sklitterRecording(ownHost(), deviceId()));
	};

	const std::vector<SklitterChunk> picture = chunks();

	// **WHILE IT IS CAPTURING, SHOW THE CAPTURE — or the device looks dead for two seconds.**
	//
	// MEASURED in an instrumented run of the real lane: from the moment Engage goes high, SKLITTER
	// records a whole BAR and then waits for the next beat boundary, so at 120 BPM it is **four to
	// five beats — over two seconds — before it plays a note**. Nothing in the band moved during that,
	// because there is nothing to move: no step is playing and no chunk is known. Press, watch, see
	// nothing, conclude it is broken. That is most of the "nothing populates until engage" report and
	// all of the pause after it.
	//
	// `Reading::Fill` was already published and nothing drew it. A bar creeping across the band says
	// "recording, wait" without a word of text.
	const auto status = static_cast<int>(std::lround(
	    ownHost().readingAt(deviceId(), static_cast<size_t>(sc::Reading::Status))));
	if (status == 1 || status == 2) { // Recording or Standby — see engage.h
		const float fill = std::clamp(
		    ownHost().readingAt(deviceId(), static_cast<size_t>(sc::Reading::Fill)), 0.0f, 1.0f);
		canvas.setColor(withAlpha(kAccent, 0.35f));
		canvas.roundedRectangle(left, top + kBandHeight - 4.0f, wide * fill, 2.0f, 1.0f);
	}


	if (picture.empty()) {
		recordLight();
		return;
	}
	const int playing = playingStepForTest();
	// **THE SWUNG GRID, NOT AN EVEN ONE (backlog IG)** — each chunk as wide as the slot that plays it.
	const std::vector<float> edges = sklitterStepEdges(sklitterGroove(ownHost(), deviceId()),
	                                                   picture.size(), sklitterSeqBars(ownHost(), deviceId()));

	// **RULED BEFORE THE CHUNKS, so a line reads as the band's grid rather than as a mark on a block.**
	// Above the bed and under everything else is the only place it can sit without competing.
	drawSklitterBarRules(canvas, left, top + 2.0f, wide, kBandHeight - 4.0f, picture.size(),
	                    sklitterSeqBars(ownHost(), deviceId()));

	// **REPEAT HAS ITS OWN FACE** — the subdivisions over the repeating duration; see `SklitterRepeatFace`.
	// The head crosses both rows exactly as it crosses the chunks.
	if (const SklitterRepeatFace face = repeatFace(picture); !face.edges.empty()) {
		const float phase = sklitterStepPhase(ownHost(), deviceId());
		drawSklitterRepeatFace(canvas, left, top + 4.0f, wide, kBandHeight - 8.0f, picture, face,
		                       &font(kKnobLabelSize));
		if (playing >= 0 && static_cast<size_t>(playing) < picture.size()) {
			canvas.setColor(kInk);
			const auto p = static_cast<size_t>(playing);
			const float at = (edges[p] + phase * (edges[p + 1] - edges[p])) * wide;
			canvas.fill(left + std::min(at, wide - 1.5f), top + 2.0f, 1.5f, kBandHeight - 4.0f);
			drawSklitterHeadLabel(canvas, font(kKnobLabelSize), sklitterHeadRatchetLabel(picture, playing),
			                      left + std::min(at, wide - 1.5f), left, wide, top + 2.0f, kHeadLabelHeight);
		}
		recordLight();
		return;
	}

	drawSklitterChunkFace(canvas, picture, edges, left, top + 4.0f, wide, kBandHeight - 8.0f, top + 1.0f, true,
	                      &font(kKnobLabelSize));

	// **WHERE THE HEAD IS NOW, TO THE SAMPLE.** Drawn last so nothing paints over it, and as a line
	// rather than a block: it marks a position and must not be mistaken for a chunk.
	//
	// **PLUS THE PHASE THROUGH THE STEP, which is what makes it a playhead rather than a marker.**
	// At step granularity it moves a few times a bar and sits still between — which read as a playhead
	// that moved only when params changed, the movement it did have being invisible against
	// a card that was not repainting either.
	if (playing >= 0 && static_cast<size_t>(playing) < picture.size()) {
		canvas.setColor(kInk);
		const auto p = static_cast<size_t>(playing);
		// THE PHASE RUNS THE WHOLE SLOT, which for a long grain is every step it spans (backlog IX).
		const size_t pEnd = std::min(picture.size(), p + static_cast<size_t>(std::max(1, picture[p].span)));
		const float at = (edges[p] + sklitterStepPhase(ownHost(), deviceId()) * (edges[pEnd] - edges[p])) * wide;
		canvas.fill(left + std::min(at, wide - 1.5f), top + 2.0f, 1.5f, kBandHeight - 4.0f);
		// **AND WHAT THE PLAYING CELL RATCHETS, AS A NUMBER** (item (b)) — the one count that is
		// always legible, whatever the cell's width: the heard count the device published for the head's step.
		drawSklitterHeadLabel(canvas, font(kKnobLabelSize), sklitterHeadRatchetLabel(picture, playing),
		                      left + std::min(at, wide - 1.5f), left, wide, top + 2.0f, kHeadLabelHeight);
	}

#ifdef FEATHERS_DIAGNOSTIC
	// **WHAT THE BAND WAS ACTUALLY HANDED, DRAWN LAST SO THE CHUNKS DO NOT BURY IT.** It was drawn
	// before them, so engaging the effect overwrote the diagnostic output.
	//
	// **ORIGINALLY:** on a build where the picture was still not moving, the UI did not populate until
	// the effect was engaged and then updated only when params moved. Every road reachable from a test is sound — the readings arrive, the
	// chunks decode, `FxLane::updateCards` reports movement on a phase change and silence when still —
	// so the next thing to establish is which of those is untrue in the plugin, and that is a
	// measurement a test cannot take.
	//
	// FIVE NUMBERS, IN THE ONE PLACE ALREADY BEING LOOKED AT: `c` is how many readings reached this card
	// at all (0 means the publish never arrives and everything downstream is moot), `S` the engage
	// status, `N` the step count, `i` the playing step and `p` the phase through it. A `p` that never
	// changes says the device is not advancing; a `c` of 0 says the wiring is; an `N` of 0 with a
	// healthy `c` says the device is publishing but not yet cut into slices.
	//
	// DIAGNOSTIC BUILDS ONLY, which is the ship flag the development installs already carry.
	{
		char line[64] = {};
		std::snprintf(line, sizeof(line), "c%zu S%d N%zu i%d p%.2f",
		              ownHost().readingCountAt(deviceId()),
		              static_cast<int>(std::lround(
		                  ownHost().readingAt(deviceId(), static_cast<size_t>(sc::Reading::Status)))),
		              picture.size(), playingStepForTest(),
		              static_cast<double>(sklitterStepPhase(ownHost(), deviceId())));
		canvas.setColor(kDisabled);
		canvas.text(line, font(kKnobReadoutSize), visage::Font::kLeft, left + 3.0f, top + 2.0f,
		            wide - 6.0f, kBandHeight - 4.0f);
	}
#endif
	recordLight();
}

} // namespace feathers::ui
