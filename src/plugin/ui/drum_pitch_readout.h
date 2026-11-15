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

#pragma once

#include "device/modulatable.h"
#include "dsp/drum_voice.h"
#include "plugin/ui/tooltip.h"
#include "plugin/ui/type_in.h"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <visage/ui.h>

// THE SETTLED-PITCH READOUT, SHARED between the drum panel and the Drum device's window: the text, its parse, the Tune adapter and the hand on it.

namespace feathers::ui {

/// THE PITCH A HIT SETTLES ON, AS TEXT AND BACK (the layer's settled-pitch readout, and the type-in that sets Tune from it).
namespace drum_pitch {
/// "55.0 Hz  A1 +0c", with a leading "~" when the pitch is approximate; empty when it is not defined.
[[nodiscard]] std::string readoutText(const drum::SettledPitch& pitch);
/// The nearest note's name and octave ("A1", "C#4"; octave numbering where A4 is 440 Hz) and the cents from it (-50..50).
struct NoteReading {
	std::string name;
	int cents = 0;
};
[[nodiscard]] NoteReading noteOf(double hz);
/// What was typed, as a frequency: a note name with an optional octave and cents ("a1", "C#4", "Bb2 -12c", "A1 +0c"), or a number
/// of Hz ("55", "55 Hz", "55.0 Hz  A1 +0c" reads its first number). Nothing for text that is neither.
[[nodiscard]] std::optional<double> parseHz(std::string_view text);
} // namespace drum_pitch

/// **THE TUNE CONTROL AS THE PITCH IT SETTLES ON**: reads and writes the layer's Tune, prints the settled pitch as text, and takes a
/// typed note or frequency back as the Tune that lands the settled pitch there (a shift of Tune by the ratio; Tune is a log of the
/// base pitch, so the settled pitch moves by exactly that ratio). Refuses to be typed into while the pitch is not defined (a Clap) or
/// approximate (a Metal), since there is no one pitch to land.
class SettledPitchControl final : public device::Modulatable {
public:
	SettledPitchControl(device::Modulatable& tune, std::function<drum::SettledPitch()> settled)
	    : tune_(tune), settled_(std::move(settled)) {}
	const device::ParamDesc& desc() const override { return tune_.desc(); }
	double value() const override { return tune_.value(); }
	void setValue(double value) override { tune_.setValue(value); }
	device::ModDisplay display() const override { return tune_.display(); }
	void setAmount(float amount) override { tune_.setAmount(amount); }
	void beginGesture() override { tune_.beginGesture(); }
	void endGesture() override { tune_.endGesture(); }
	std::string text() const override { return drum_pitch::readoutText(settled_()); }
	/// The Tune it reads and writes (a typed note or a drag sets it).
	[[nodiscard]] device::Modulatable& tune() const { return tune_; }
	[[nodiscard]] std::optional<std::string> editText() const override;
	[[nodiscard]] std::optional<double> parseText(std::string_view text, double shown) const override;

private:
	device::Modulatable& tune_;
	std::function<drum::SettledPitch()> settled_;
};

/// The settled pitch of a layer, drawn as text, with a hand on it: a click types a note or Hz into it, a drag moves Tune in whole
/// semitones (Shift: fine, `kFineAdjust` of the travel, not rounded).
class PitchReadout : public visage::Frame, public TooltipSource {
public:
	explicit PitchReadout(SettledPitchControl& control);
	void draw(visage::Canvas& canvas) override;
	void resized() override;
	const char* tooltipAt(visage::Point) const override;
	/// IT DECLARES TUNE, which a typed note or a drag writes, as a gesture (not a face: the pill is Tune's face and the one that glows).
	void offeredControls(std::vector<Offered>& out) const override { out.push_back({ &control_.tune(), Road::Gesture }); }
	/// The text drawn, and the field a click opens (null until it has).
	[[nodiscard]] std::string textForTest() const { return control_.text(); }
	/// Repaint if what it says has changed since it last drew (the panel asks every tick).
	void refresh(const std::string& text);
	[[nodiscard]] TextField* typeInField() const noexcept { return typeIn_.field(); }
	/// TEST ONLY: a hand's click (press and release on the spot), and a drag of `dy` pixels up (positive), `fine` for Shift.
	void clickForTest();
	void dragForTest(float dy, bool fine);
	/// How many pixels of vertical travel are worth one semitone.
	static constexpr float kPixelsPerSemitone = 6.0f;

private:
	[[nodiscard]] visage::Bounds typeInBounds() const { return { 0.0f, 0.0f, width(), height() }; }
	void beginHand(float y, bool fine);
	void moveHand(float y, bool fine);
	void endHand();
	SettledPitchControl& control_;
	TypeIn typeIn_ { *this };
	std::string drawn_;
	bool pressed_ = false;
	bool dragged_ = false;
	bool fine_ = false;
	float anchorY_ = 0.0f;
	double anchorValue_ = 0.0;
};


} // namespace feathers::ui
