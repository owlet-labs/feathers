/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even
 * the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
 * Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

#pragma once

#include "backplane/device_host.h"
#include "fx/harm/harm_envelope.h"
#include "fx/harm/harm_types.h"

#include <visage/ui.h>

#include <vector>

// HARM'S ENVELOPE, DRAWN — the shape of one note: delay, attack, hold, decay, sustain, release.
//
// IT LOOKS AND LAYS ITSELF OUT LIKE THE DAHDSR MODULATOR'S CURVE (env_card.cpp) because it is the same kind of picture of the
// same kind of envelope: a dark box, the curve in the value colour, a stage's width in proportion to its length in SECONDS
// with a floor so a millisecond attack beside a five-second decay is still an edge, and a sustain plateau that takes a fixed
// share because a level has no length. It is NOT that card's widget, and cannot be: the modulator's is welded to
// `dahdsr::Settings` and runs the modulator's own `Envelope` to get its points. HARM's attack, decay and release are
// one-pole time constants rather than fixed-length ramps (see harm_envelope.h), so this draws HARM's own stages, from
// `envelopeSegments`/`envelopeLevelAt` — the closed form of what `fx::harm::Envelope` plays, which a case holds to the
// sample-by-sample envelope.
//
// IT IS A PICTURE AND NOT A CONTROL. The six times and levels are the boxes beside it, and the modulator's own curve drag edits
// the stage SHAPES, which HARM's envelope does not have.

namespace feathers::ui {

class HarmEnvelopeView : public visage::Frame {
public:
	HarmEnvelopeView(backplane::DeviceHost& host, DeviceInstanceId id);
	HarmEnvelopeView(backplane::DeviceHost&, size_t) = delete; // a lane or a slot is not an id

	void draw(visage::Canvas& canvas) override;

	/// The share of the width a sustain plateau takes, as in the modulator's card.
	static constexpr float kSustainShare = 0.16f;
	/// The least a stage that happens may take, as a fraction of the whole — so a short stage is still an edge.
	static constexpr float kMinLiveShare = 0.04f;
	static constexpr float kPad = 6.0f;

	/// RE-READ THE DEVICE'S ENVELOPE and say whether the picture changed (the window polls this once a tick).
	bool refresh();

	/// ONE STAGE OF THE DRAWN SHAPE, on the 0..1 width of the plot.
	struct Span {
		fx::harm::Envelope::Stage stage = fx::harm::Envelope::Stage::Idle;
		float from = 0.0f;
		float to = 0.0f;
	};
	/// The shape: x 0..1 across, y the level 0..1, as drawn. Rebuilt when the settings move.
	const std::vector<visage::Point>& curvePointsForTest() const { return curve_; }
	const std::vector<Span>& spansForTest() const { return spans_; }
	/// The settings the picture was last built from.
	const fx::harm::EnvelopeTimes& timesForTest() const { return times_; }
	/// The box the curve is drawn in, in this frame's own coordinates.
	visage::Bounds plotBounds() const;

private:
	void rebuild();
	fx::harm::EnvelopeTimes readTimes() const;

	backplane::DeviceHost& host_;
	DeviceInstanceId device_ = kNoInstance;
	fx::harm::EnvelopeTimes times_ {};
	bool built_ = false;
	std::vector<visage::Point> curve_;
	std::vector<Span> spans_;
};

} // namespace feathers::ui
