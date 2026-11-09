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

#include "dsp/character_bank.h"
#include "dsp/osc_voice.h"

// One voice and the character bank it plays from, wired together and driven in the order the synth
// drives them: render, then tell the bank how far time moved.
//
// The two are separate in the plugin because a synth has one bank and sixteen voices. A test almost
// always wants one of each, and wants the pair to behave exactly as the synth makes them behave —
// including advance(), without which a deferred character change would never land.

namespace feathers::test {

struct BankedVoice {
	CharacterBank bank;
	OscVoice voice;

	BankedVoice() { voice.setCharacterBank(&bank); }

	void setSampleRate(double hostSampleRate) {
		bank.setSampleRate(hostSampleRate);
		voice.setSampleRate(hostSampleRate);
	}

	void setCharacter(const Character& c) { bank.setCharacter(c); }
	void setCharacterStereo(const Character& l, const Character& r) { bank.setCharacterStereo(l, r); }
	void setFrequency(double hz) { voice.setFrequency(hz); }
	void setNoiseSeed(uint32_t seed) { voice.setNoiseSeed(seed); }
	void reset() { voice.reset(); }
	void clearModifiersForTest() { bank.clearModifiersForTest(); }

	void render(float* out, int n) {
		voice.render(out, n);
		bank.advance(n);
	}

	void renderStereo(float* outL, float* outR, int n) {
		voice.renderStereo(outL, outR, n);
		bank.advance(n);
	}

	void renderWithoutOversamplingForTest(float* out, int n) {
		voice.renderWithoutOversamplingForTest(out, n);
		bank.advance(n);
	}
};

/// ONE SOUND, DECLARED TWICE, which is what a test asking for "zone N at offset x" wants to hear.
///
/// Two entries rather than one on purpose: two sounds is the case the plugin ships in and the case
/// that still takes the v1 crossfade, so a test built this way renders the path a user renders. A
/// single-sound character would take the N-way blend instead — the same numbers by different
/// arithmetic, and a difference none of these tests asked to be sensitive to.
///
/// `xf` at 0 puts the crossfade on the first of the two, so the excitation is zero and what comes out
/// is that zone exactly, which is what every caller of this compares against.
inline Character characterAt(OscEngine engine, uint16_t zone, float offset = 0.0f) {
	Character c;
	c.engine = engine;
	c.count = 2;
	c.sounds[0].zone = zone;
	c.sounds[0].phaseOffset = offset;
	c.sounds[0].weight = 1.0;
	c.sounds[1] = c.sounds[0];
	c.sounds[1].weight = 0.0;
	c.xf = 0.0f;
	return c;
}

} // namespace feathers::test
