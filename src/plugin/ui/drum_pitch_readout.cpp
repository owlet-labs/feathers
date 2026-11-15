/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even
 * the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

#include "plugin/ui/drum_pitch_readout.h"

#include "plugin/ui/fine_adjust.h"
#include "plugin/ui/text_field.h"
#include "plugin/ui/theme.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace feathers::ui {

namespace {
uint32_t alphaOf(uint32_t colour, float alpha) {
	const auto a = static_cast<uint32_t>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f);
	return (colour & 0x00ffffffu) | (a << 24);
}
} // namespace

namespace drum_pitch {

namespace {
constexpr std::array<const char*, 12> kNoteNames { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
}

NoteReading noteOf(double hz) {
	NoteReading out;
	if (!(hz > 0.0) || !std::isfinite(hz)) {
		return out;
	}
	const double midi = 69.0 + 12.0 * std::log2(hz / 440.0);
	const long n = std::lround(midi);
	out.cents = static_cast<int>(std::lround((midi - static_cast<double>(n)) * 100.0));
	const long within = ((n % 12) + 12) % 12;
	const long octave = (n - within) / 12 - 1;
	out.name = std::string(kNoteNames[static_cast<size_t>(within)]) + std::to_string(octave);
	return out;
}

std::string readoutText(const drum::SettledPitch& pitch) {
	if (!pitch.defined) {
		return {};
	}
	const NoteReading note = noteOf(pitch.hz);
	char text[64] = {};
	std::snprintf(text, sizeof(text), "%s%.1f Hz  %s %+dc", pitch.approximate ? "~" : "", pitch.hz, note.name.c_str(), note.cents);
	return text;
}

std::optional<double> parseHz(std::string_view text) {
	std::string t;
	for (const char c : text) {
		t += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	const auto first = t.find_first_not_of(" \t");
	if (first == std::string::npos) {
		return std::nullopt;
	}
	t = t.substr(first);
	// A NOTE NAME: a letter A..G, an optional # or b, the octave, then optional cents ("+12", "-7c").
	if (t[0] >= 'a' && t[0] <= 'g') {
		static constexpr std::array<int, 7> kSemitone { 9, 11, 0, 2, 4, 5, 7 }; // a b c d e f g
		int semitone = kSemitone[static_cast<size_t>(t[0] - 'a')];
		size_t at = 1;
		if (at < t.size() && (t[at] == '#' || t[at] == 'b')) {
			semitone += t[at] == '#' ? 1 : -1;
			++at;
		}
		const char* begin = t.c_str() + at;
		char* end = nullptr;
		const long octave = std::strtol(begin, &end, 10);
		if (end == begin) {
			return std::nullopt; // no octave: not a note
		}
		double cents = 0.0;
		const char* rest = end;
		while (*rest == ' ') {
			++rest;
		}
		if (*rest == '+' || *rest == '-') {
			char* centsEnd = nullptr;
			cents = std::strtod(rest, &centsEnd);
			if (centsEnd == rest) {
				return std::nullopt;
			}
			rest = centsEnd;
			if (*rest == 'c') {
				++rest;
			}
		}
		while (*rest == ' ') {
			++rest;
		}
		if (*rest != '\0') {
			return std::nullopt;
		}
		const double midi = 12.0 * static_cast<double>(octave + 1) + static_cast<double>(semitone) + cents / 100.0;
		return 440.0 * std::exp2((midi - 69.0) / 12.0);
	}
	// A FREQUENCY: a number, then nothing or "hz" (and whatever follows it: the readout's own note text reads back).
	const char* begin = t.c_str();
	char* end = nullptr;
	const double hz = std::strtod(begin, &end);
	if (end == begin || !std::isfinite(hz) || hz <= 0.0) {
		return std::nullopt;
	}
	const char* rest = end;
	while (*rest == ' ') {
		++rest;
	}
	if (*rest == '\0' || (rest[0] == 'h' && rest[1] == 'z')) {
		return hz;
	}
	return std::nullopt;
}

} // namespace drum_pitch

std::optional<std::string> SettledPitchControl::editText() const {
	const drum::SettledPitch pitch = settled_();
	if (!pitch.defined || pitch.approximate) {
		return std::nullopt; // no one pitch to land: refuse rather than take a number for something else
	}
	return drum_pitch::readoutText(pitch);
}

std::optional<double> SettledPitchControl::parseText(std::string_view text, double shown) const {
	const drum::SettledPitch pitch = settled_();
	const std::optional<double> hz = drum_pitch::parseHz(text);
	if (!hz || !pitch.defined || pitch.approximate || !(pitch.hz > 0.0)) {
		return std::nullopt;
	}
	// TUNE IS A LOG OF THE BASE PITCH and every kind's settled pitch is proportional to the base, so the Tune that lands it on
	// `hz` is where Tune stands now plus the semitones between the two.
	const device::ParamDesc& d = tune_.desc();
	return std::clamp(shown + 12.0 * std::log2(*hz / pitch.hz), d.min, d.max);
}

PitchReadout::PitchReadout(SettledPitchControl& control) : control_(control) {
	onMouseDown() = [this](const visage::MouseEvent& e) { beginHand(e.position.y, e.isShiftDown()); };
	onMouseDrag() = [this](const visage::MouseEvent& e) { moveHand(e.position.y, e.isShiftDown()); };
	onMouseUp() = [this](const visage::MouseEvent&) { endHand(); };
	installTooltipHover(*this, *this);
}

void PitchReadout::resized() { typeIn_.place(typeInBounds()); }

const char* PitchReadout::tooltipAt(visage::Point) const {
	return "The pitch this hit settles on, at the pad's resting pitch, its Tune and the terrain's. Click to type a note (A1, C#4) "
	       "or a frequency; drag to move Tune a semitone at a time, Shift for fine.";
}

void PitchReadout::refresh(const std::string& text) {
	if (!isVisible()) {
		drawn_ = text; // nothing is drawn while it is hidden, so nothing is owed a repaint
		return;
	}
	if (text != drawn_) {
		redraw();
	}
}

void PitchReadout::draw(visage::Canvas& canvas) {
	drawn_ = control_.text();
	canvas.setColor(kSurfaceRaised);
	canvas.roundedRectangle(0.0f, 0.0f, width(), height(), 4.0f);
	canvas.setColor(alphaOf(kLine, 0.8f));
	canvas.roundedRectangleBorder(0.0f, 0.0f, width(), height(), 4.0f, 1.0f);
	canvas.setColor(kInk);
	canvas.text(drawn_, font(kKnobReadoutSize), visage::Font::kCenter, 0.0f, 0.0f, width(), height());
}

void PitchReadout::beginHand(float y, bool fine) {
	const bool closedField = typeIn_.closeIfOpen();
	pressed_ = !closedField;
	dragged_ = false;
	fine_ = fine;
	anchorY_ = y;
	anchorValue_ = control_.value();
}

void PitchReadout::moveHand(float y, bool fine) {
	if (!pressed_) {
		return;
	}
	if (!dragged_) {
		if (std::fabs(anchorY_ - y) < 3.0f) {
			return; // a click that wandered a pixel is still a click
		}
		dragged_ = true;
		control_.beginGesture();
	}
	if (fine != fine_) {
		// SHIFT CHANGING MID-DRAG RE-ANCHORS (the knob's rule), so nothing jumps.
		fine_ = fine;
		anchorY_ = y;
		anchorValue_ = control_.value();
	}
	const double semitones = static_cast<double>(anchorY_ - y) / static_cast<double>(kPixelsPerSemitone);
	control_.setValue(fine_ ? anchorValue_ + semitones * static_cast<double>(kFineAdjust) : std::round(anchorValue_ + semitones));
	redraw();
}

void PitchReadout::endHand() {
	if (!pressed_) {
		return;
	}
	pressed_ = false;
	if (dragged_) {
		control_.endGesture();
		dragged_ = false;
		return;
	}
	// A CLICK TYPES: the field opens on the readout's own text, a note or a frequency lands the settled pitch there.
	typeIn_.beginReset(control_, false, true);
	(void)typeIn_.open(typeInBounds());
}

void PitchReadout::clickForTest() {
	beginHand(10.0f, false);
	endHand();
}

void PitchReadout::dragForTest(float dy, bool fine) {
	beginHand(100.0f, fine);
	moveHand(100.0f - dy, fine);
	endHand();
}

} // namespace feathers::ui
