/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program.
 * If not, see <https://www.gnu.org/licenses/>.
 */

#include "plugin/ui/perform_pad.h"

#include "plugin/ui/celestial.h" // performSky — Feathers' own 110 stars, not a second table
#include "plugin/ui/edit_history.h"
#include "plugin/ui/theme.h"

#include "visage_utils/time_utils.h"

#include <embedded/feathers_shaders.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace feathers::ui {

namespace sk = fx::sklitter;

PerformPad::PerformPad(device::Modulatable& x, device::Modulatable& y) :
    x_(x), y_(y),
    // **TWO SHADER OBJECTS, ONE PROGRAM.** A `Shader*` is the batch id, so this is what makes the two
    // inks two batches with two uniform sets; `ProgramCache` keys on the files, so the pair is compiled
    // once. `BlendMode::Alpha` because a spark fades out rather than adding light — and the fragment
    // stage therefore writes NON-premultiplied colour (see `fs_spark.sc`).
    sparkShader_(resources::shaders::vs_spark, resources::shaders::fs_spark, visage::BlendMode::Alpha),
    sparkLaneShader_(resources::shaders::vs_spark, resources::shaders::fs_spark,
                     visage::BlendMode::Alpha),
    onsetShaders_ { visage::Shader(resources::shaders::vs_spark, resources::shaders::fs_spark,
                                   visage::BlendMode::Alpha),
	                visage::Shader(resources::shaders::vs_spark, resources::shaders::fs_spark,
	                               visage::BlendMode::Alpha),
	                visage::Shader(resources::shaders::vs_spark, resources::shaders::fs_spark,
	                               visage::BlendMode::Alpha),
	                visage::Shader(resources::shaders::vs_spark, resources::shaders::fs_spark,
	                               visage::BlendMode::Alpha) },
    repeatShader_(resources::shaders::vs_spark, resources::shaders::fs_spark, visage::BlendMode::Alpha) {
	// **RESERVED ONCE, SO THE STEADY STATE NEVER REALLOCATES.** See the member.
	sparks_.reserve(kMaxSparks);
	visible_.reserve(kMaxSparks);
	// **THE SAME INJECTABLE-CLOCK SHAPE `PerformStage` AND `ExploreStage` USE**, and for their reason:
	// an animation whose time nobody can pin cannot be asserted headlessly at all. A case sets its own
	// and drives the pad tick by tick; production takes the wall clock.
	clock_ = [] { return static_cast<int64_t>(visage::time::milliseconds()); };

	// **A PRESS IS THE FIRST MOVE OF THE GESTURE, NOT AN ANCHOR FOR IT.** The shaper's pad learned this
	// the hard way — a click anywhere moves the puck — and the reason is sharper
	// here than there: this puck is a POSITION, not a displacement, so "where you pressed" IS the value.
	// Anchoring at the click would drop the whole distance from wherever the puck was to wherever the
	// hand landed.
	onMouseDown() = [this](const visage::MouseEvent& e) {
		// **A DOUBLE-CLICK CENTRES, AND IT IS ASKED BEFORE THE HIT TEST.** Anywhere on the pad, not only
		// on the puck: the puck is where the hand is NOT aiming when it wants it back, and the first
		// click of the pair has already put it under the cursor anyway. No drag is armed — `held_` stays
		// false — so a motion before the release cannot drag the puck straight back off the rest point
		// (the OWLTT bars' bug, paid for once already). The first click did what a click does: it moved
		// the puck and recorded that step, because nothing can know yet that a second one is coming —
		// so the centring AMENDS that step rather than adding one (see `centre`).
		if (e.repeatClickCount() >= 2) {
			(void)centre();
			return;
		}
		// EVERY OTHER PRESS FORGETS THE LAST STEP, including one that misses the disk and moves nothing:
		// otherwise a double-click whose first click fell outside would fold the centring into whatever
		// drag happened to be the tip, and one undo would go back further than the double-click.
		lastMove_ = 0;
		if (!perform_pad::hits(disk(), e.position.x, e.position.y, grabSlack())) {
			return;
		}
		held_ = true;
		beginMove(0);
		const auto [ax, ay] = perform_pad::axesAt(disk(), e.position.x, e.position.y);
		writeAxes(ax, ay);
	};

	onMouseDrag() = [this](const visage::MouseEvent& e) {
		if (!held_) {
			return;
		}
		// **NO HIT TEST HERE, AND THAT IS THE POINT OF `held_`.** Once a drag owns the puck it keeps it
		// past the rim: `axesAt` clamps to the disk, so dragging away from the pad slides the puck round
		// the rim rather than dropping the gesture in the middle of it.
		const auto [ax, ay] = perform_pad::axesAt(disk(), e.position.x, e.position.y);
		writeAxes(ax, ay);
	};

	onMouseUp() = [this](const visage::MouseEvent&) {
		if (!held_) {
			return;
		}
		held_ = false;
		endMove();
	};
}

PerformPad::~PerformPad() {
	if (held_) {
		endMove();
	}
}

void PerformPad::setHistory(EditHistory* history) {
	// NOT SWAPPED MID-GESTURE: the group a held drag opened must close on the history that opened it.
	if (held_) {
		endMove();
		held_ = false;
	}
	history_ = history;
	lastMove_ = 0; // a serial means nothing to a different history
}

void PerformPad::beginMove(uint64_t amends) {
	// BOTH GESTURES, BEFORE EITHER WRITE, INSIDE ONE GROUP. `DeviceControl` records its own undo step
	// at each `endGesture` — a docked control has no DAW behind it to own one — so nesting the two
	// gestures nests nothing and a move of both axes was TWO steps, one of which could be undone alone.
	// The group is what makes the point one step.
	if (history_ != nullptr) {
		history_->beginGroup(amends != 0 ? "Centre" : "Move", amends);
	}
	x_.beginGesture();
	y_.beginGesture();
}

void PerformPad::endMove() {
	x_.endGesture();
	y_.endGesture();
	if (history_ == nullptr) {
		return;
	}
	// WHICH STEP THIS MOVE MADE, by the serial the history stamped on it — or none, if it moved
	// nothing and the tip is still whatever it was.
	const uint64_t before = history_->lastSerial();
	history_->endGroup();
	const uint64_t after = history_->lastSerial();
	lastMove_ = after != before ? after : 0;
}

bool PerformPad::centre() {
	// THE DESCRIPTOR'S DEFAULTS, NOT 0. Both happen to be 0 today (`sklitter_types.h`), and a rest
	// point that was hard-coded here would stop being the rest point the day the declaration moved.
	const double dx = x_.desc().def;
	const double dy = y_.desc().def;
	if (x_.value() == dx && y_.value() == dy) {
		return false;
	}
	// **FOLDED INTO THE FIRST CLICK'S STEP, when that step is still the tip** — so one undo puts the
	// puck back where it was BEFORE the double-click, not at the point the first click moved it to.
	// The history checks the identity and refuses if anything was recorded or undone in between, and
	// then this is a step of its own.
	beginMove(lastMove_);
	writeAxes(static_cast<float>(dx), static_cast<float>(dy));
	endMove();
	redraw();
	return true;
}

float PerformPad::scale() const {
	// **THE SHORTER SIDE, because the instrument is a disk and a disk is sized by the side that binds.**
	// `CircleRail` lays this pad out square, so the two agree there; a caller that laid it out oblong
	// gets the circle that fits rather than an ellipse of furniture.
	const float side = std::min(static_cast<float>(width()), static_cast<float>(height()));
	return std::max(1.0f, side / kReferenceDiameter);
}

perform_pad::Disk PerformPad::disk() const {
	// **THE INSET SCALES WITH THE MARKS IT IS RESERVING ROOM FOR**, which is what keeps the drawn rim
	// and the playable radius one fact at every size. See `rimInset()`.
	return perform_pad::diskIn(0.0f, 0.0f, static_cast<float>(width()), static_cast<float>(height()),
	                           rimInset());
}

sk::PerformWeights PerformPad::read() const {
	// **THE ONE READ, AND EVERY OTHER MEMBER FUNCTION GOES THROUGH IT.** `performWeightsOf` de-NaNs and
	// clamps and hands the sanitised axes back beside the weights, which is exactly so a surface does not
	// need a second copy of that rule — `perform.h` states it at the fields. So the puck drawn at a NaN
	// axis sits at the centre, which is what that axis SOUNDS like.
	return sk::performWeightsOf(static_cast<float>(x_.value()), static_cast<float>(y_.value()));
}

std::pair<float, float> PerformPad::puckPointForTest() const {
	const sk::PerformWeights w = read();
	return perform_pad::pointOf(disk(), w.x, w.y);
}

sk::PerformWeights PerformPad::weightsForTest() const { return read(); }

void PerformPad::writeAxes(float ax, float ay) {
	// **BOTH AXES, ALWAYS BOTH.** A drag writes a point, and a point is two numbers; writing one would
	// slide the puck along a line the hand did not move along — which is the known-bad this pair exists
	// to fail.
	x_.setValue(static_cast<double>(ax));
	y_.setValue(static_cast<double>(ay));
}

void PerformPad::setSparkSeedForTest(uint32_t seed) noexcept {
	// **NEVER ZERO.** xorshift32's zero state is a fixed point: it would answer 0 for ever, which reads
	// as "the random process stopped" and would be indistinguishable from a broken emitter.
	rng_ = seed != 0u ? seed : kSparkSeed;
}

void PerformPad::setClockForTest(Clock clock) {
	if (clock) {
		clock_ = std::move(clock);
		// **AND THE PREVIOUS SAMPLE IS DROPPED WITH THE CLOCK THAT TOOK IT.** A timestamp from the wall
		// clock differenced against one from a case's counter is a gap of decades; the first tick after
		// a clock change must be a position, not a motion.
		havePuck_ = false;
	}
}

float PerformPad::nextUnit() noexcept {
	// xorshift32, Marsaglia's triple. The top 24 bits over 2^24 — the low bits of this generator are
	// the weaker ones, and a spark's direction is drawn from exactly this.
	rng_ ^= rng_ << 13u;
	rng_ ^= rng_ >> 17u;
	rng_ ^= rng_ << 5u;
	return static_cast<float>(rng_ >> 8u) / 16777216.0f;
}

size_t PerformPad::poissonDraw(float mean) noexcept {
	if (!(mean > 0.0f)) {
		return 0; // also the NaN road: a rate that is not a number emits nothing rather than everything
	}
	// **KNUTH'S METHOD, AND THE CAP IS THE REASON IT IS SAFE TO WRITE AS A LOOP.** Multiply uniforms
	// until the product falls under `exp(-mean)`. It is exact, and it costs `mean + 1` draws on
	// average — about two at 60 fps. The loop terminates with probability 1 and NOT with certainty, so
	// it is bounded by `kMaxSparks` as well: a mean that had somehow become enormous would otherwise
	// spin the message thread, and the cap is where the vector would have stopped growing anyway.
	const float limit = std::exp(-mean);
	float product = 1.0f;
	size_t k = 0;
	while (k < kMaxSparks) {
		product *= nextUnit();
		if (product <= limit) {
			break;
		}
		++k;
	}
	return k;
}

void PerformPad::emitFrom(float level, double dt, float nx, float ny, bool fromLane) {
	// **THE EMISSION. Poisson, rate ∝ level^gamma, drawn over `dt` — NOT one per N frames.** What is
	// wanted is a process, and the difference is visible: a per-frame count makes an even shower, a
	// Poisson process clumps and gaps the way a real spray does.
	//
	// **AND `dt` COMES FROM THE CLOCK, NOT FROM A FRAME COUNT**, so the rate is sparks per SECOND at
	// any repaint rate the host happens to run at.
	if (!(dt > 0.0) || !std::isfinite(level) || !(level > 0.0f)) {
		return;
	}
	// **CLAMPED AT 1 FOR THE RATE AND NOT FOR THE READING.** The levels may exceed full scale (a chain
	// with gain in it) and `sklitterLevels` deliberately does not clamp, because a meter that did would
	// call a player's loudest moment and their second loudest the same. A RATE has a ceiling though,
	// and this is where that drawing decision is made.
	const float lv = std::min(level, 1.0f);
	const float rate = kSparkRateMax * std::pow(lv, kSparkLevelGamma);
	const size_t born = poissonDraw(rate * static_cast<float>(dt));
	for (size_t i = 0; i < born; ++i) {
		if (!spawn(nx, ny, fromLane, 0)) {
			break;
		}
	}
}

bool PerformPad::spawn(float nx, float ny, bool fromLane, uint8_t onset, bool repeat, float repeatPeriod) {
	if (liveSparks() >= kMaxSparks) {
		return false;
	}
	{
		Spark s;
		s.x = nx;
		s.y = ny;
		s.fromLane = fromLane;
		s.onset = onset;
		s.repeat = repeat;
		// **THE MOMENTUM: this tick's puck velocity, shared down, plus a kick.** Read from
		// `puckVx_`/`puckVy_` computed by the caller rather than re-differenced here, so every spark
		// born on one tick — of EITHER colour — carries the SAME inherited velocity, which is what "the
		// velocity at the time they are emitted" means. The two emitters differ in rate and hue and in
		// nothing else about how a spark flies; see `kSparkLaneSizeScale`.
		const float angle = nextUnit() * 6.28318531f;
		// **`sqrt(u)`, NOT `u` — and PHOTOGRAPHED, which is the only reason it was found.** A uniform
		// draw for the SPEED makes a fountain whose density piles up at its own origin: the sparks are
		// spread over an area that grows as r², so drawing r uniformly puts most of them in the small
		// disk near the puck. The first capture of the feel pass showed exactly that — a saturated
		// blob at the puck with a thin scatter round it, rather than a spray. `sqrt(u)` is the standard
		// correction and makes the density uniform in AREA. It changes no constant, only the shape of
		// the draw, so `kSparkSpread` still means what it says: the fastest kick a spark can get.
		const float kick = std::sqrt(nextUnit()) * kSparkSpread;
		s.vx = puckVx_ * kSparkInherit + std::cos(angle) * kick;
		s.vy = puckVy_ * kSparkInherit + std::sin(angle) * kick;
		s.life = kSparkLife * (1.0f - kSparkLifeJitter + 2.0f * kSparkLifeJitter * nextUnit());
		// **STAMPED, NOT ZEROED.** This is the field that turned a spark from state into a constant.
		s.birth = nowSeconds_;
		// **DRAWN LAST, so adding it did not renumber the stream** — every earlier draw keeps its
		// meaning and the seeded cases keep their counts.
		s.seed = nextUnit();
		// **THE SPLIT, FROM THE SEED — no draw of its own**, so adding it renumbered nothing. See
		// `sparkSplitFor` for why the CPU decides it at all. A REPEAT spark always splits, paced by the
		// repeat's own period (`sparkRepeatKick`); any other takes the random chance, off by default.
		s.split = repeat ? sparkRepeatKick(s.seed, repeatPeriod) : sparkSplitFor(s.seed, onset != 0);
		sparks_.push_back(s);
		++emitted_;
		if (fromLane) {
			++emittedLane_;
		}
		if (onset != 0) {
			++emittedOnset_[onset - 1u];
		}
		if (repeat) {
			++emittedRepeat_;
		}
		if (s.split != 0.0f) {
			++emittedSplit_;
		}
	}
	return true;
}

uint32_t PerformPad::eventsSince(uint32_t now, uint32_t last) noexcept {
	constexpr uint32_t kWrap = fx::sklitter::kGrainOnsetWrap;
	// MOD THE DEVICE'S OWN WRAP, so a counter crossing 2^24 is one more event rather than minus sixteen
	// million.
	const uint32_t fired = (now % kWrap + kWrap - last % kWrap) % kWrap;
	// **A COUNTER THAT WENT BACKWARDS IS A RESET, NOT A RHYTHM.** A device rebuilt or swapped counts again
	// from zero, which mod 2^24 reads as a forward jump of nearly the whole range; more than half of it is
	// taken as backwards and throws nothing.
	if (fired > kWrap / 2u) {
		return 0;
	}
	// **A SLOW TICK IS CAPPED, NOT DROPPED** — `kMaxOnsetsPerTick`. A UI running at a few frames a second
	// still sees its events; a stall of seconds throws a bounded burst rather than all of them.
	return std::min(fired, kMaxOnsetsPerTick);
}

void PerformPad::emitEvents(const SparkEvents& events, float nx, float ny, bool sounding) {
	// **THE FIRST READING IS A BASELINE.** A device that has been running for an hour has counted every
	// grain and repeat of it; the panel opening is not the moment they all fired.
	if (!haveOnsets_) {
		lastOnsets_ = events.onsets;
		lastRepeats_ = events.repeats;
		haveOnsets_ = true;
		return;
	}
	// EVERY BASELINE ADVANCES THIS TICK, whatever is emitted — so a later tick counts from here.
	std::array<uint32_t, fx::sklitter::kGrainOnsetClasses> fired {};
	for (size_t c = 0; c < fired.size(); ++c) {
		fired[c] = eventsSince(events.onsets[c], lastOnsets_[c]);
		lastOnsets_[c] = events.onsets[c];
	}
	const uint32_t repeats = eventsSince(events.repeats, lastRepeats_);
	lastRepeats_ = events.repeats;

	// **AND AN EVENT NOBODY HEARS THROWS NOTHING.** The counters run whatever the Mix; the levels are the
	// device's own statement of whether any of it is audible. The baselines still advanced, so turning
	// the Mix up later does not release a backlog.
	if (!sounding) {
		return;
	}
	for (size_t c = 0; c < fired.size(); ++c) {
		for (uint32_t i = 0; i < fired[c]; ++i) {
			if (!spawn(nx, ny, /*fromLane=*/false, static_cast<uint8_t>(c + 1u))) {
				return; // the cap: nothing further fits this tick
			}
		}
	}
	for (uint32_t i = 0; i < repeats; ++i) {
		if (!spawn(nx, ny, /*fromLane=*/false, /*onset=*/0, /*repeat=*/true, events.repeatPeriod)) {
			return;
		}
	}
}

bool PerformPad::updateSparks(float wetLevel, float grainLevel, const SparkEvents* events) {
	const int64_t now = clock_();
	// **THE PUCK AND THE INSTANT, SAMPLED TOGETHER — the contemporaneity note on the members.** One
	// read of the parameters, through the same `read()` every other member goes through, paired with
	// the timestamp that will measure the gap to the next one.
	const sk::PerformWeights w = read();
	const float nx = w.x;
	const float ny = w.y;

	// **THE PAD'S OWN CLOCK, IN SECONDS SINCE ITS OWN EPOCH — and the epoch is why.** `Spark::birth`
	// reaches the shader as a `float`; milliseconds-since-some-distant-origin in a float has tens of
	// milliseconds of quantum, which is most of a frame, so a spark's age would step rather than run.
	if (!haveEpoch_) {
		epochMs_ = now;
		haveEpoch_ = true;
	}
	nowSeconds_ = static_cast<float>(static_cast<double>(now - epochMs_) / 1000.0);

	double dt = 0.0;
	if (havePuck_) {
		// `PerformStage::update`'s clamp, for its reason: a stalled host hands back seconds, and a
		// spark integrated over seconds teleports.
		dt = std::min(kSparkMaxDt, static_cast<double>(now - lastTickMs_) / 1000.0);
		dt = std::max(0.0, dt);
	}
	const bool hadPuck = havePuck_;
	lastTickMs_ = now;
	havePuck_ = true;

	if (hadPuck && dt > 0.0) {
		const auto inv = static_cast<float>(1.0 / dt);
		float vx = (nx - puckX_) * inv;
		float vy = (ny - puckY_) * inv;
		// **A JUMP IS NOT A VELOCITY — see `kSparkSpeedMax`.** The DIRECTION of a jump is real (the
		// puck did go that way) and its magnitude is an artefact of the sampling, so the clamp keeps
		// the first and discards the second.
		const float speed = std::hypot(vx, vy);
		if (speed > kSparkSpeedMax) {
			const float k = kSparkSpeedMax / speed;
			vx *= k;
			vy *= k;
		}
		puckVx_ = vx;
		puckVy_ = vy;
	}
	else if (!hadPuck) {
		puckVx_ = 0.0f;
		puckVy_ = 0.0f;
	}
	puckX_ = nx;
	puckY_ = ny;

	const bool hadSparks = liveSparks() > 0;

	// **NOTHING IS MOVED HERE ANY MORE — backlog HR's GPU pass, and this is the whole of what changed
	// on the CPU side.** A spark's position is a closed form of its birth data and the clock
	// (`sparkOffsetAt`), evaluated in `vs_spark.sc`. There is no integrator, so there is nothing to
	// step, and the tick's cost stops being a function of how many sparks are alive.
	//
	// **RETIREMENT IS ADVANCING AN INDEX, NOT A SCAN.** Sparks are appended in birth order, so if the
	// test is "born longer ago than the LONGEST life a spark can have" then the dead are always a
	// PREFIX — and retiring them touches only the dead ones. The jittered life is what forces the
	// worst-case bound here; the exact life is enforced where it can be exact and free, by the
	// fragment shader's `discard`, and by the CPU reference path's own clamp.
	//
	// **THE APERTURE MOVED TO THE SHADER TOO** (`u_spark_disk`), for the same reason: testing it here
	// would mean evaluating every live spark's position every tick, which is the walk this pass exists
	// to delete.
	const float longest = kSparkLife * (1.0f + kSparkLifeJitter);
	while (head_ < sparks_.size() && nowSeconds_ - sparks_[head_].birth >= longest) {
		++head_;
	}
	// **COMPACTED ONLY WHEN THE DEAD OUTNUMBER THE LIVE**, so the memmove is amortised over
	// `kMaxSparks/2` births rather than paid every tick. An `erase` per tick would have put an O(alive)
	// cost back in by the back door.
	if (head_ > 0 && (head_ == sparks_.size() || head_ >= kMaxSparks / 2)) {
		sparks_.erase(sparks_.begin(), sparks_.begin() + static_cast<ptrdiff_t>(head_));
		head_ = 0;
	}

	// **THE EMISSION. Poisson, rate ∝ level^gamma, drawn over `dt` — NOT one per N frames.** What is
	// wanted is a process, and the difference is visible: a per-frame count makes an even shower, a
	// Poisson process clumps and gaps the way a real spray does.
	//
	// **AND `dt` COMES FROM THE CLOCK, NOT FROM A FRAME COUNT**, so the rate is sparks per SECOND at
	// any repaint rate the host happens to run at.
	// **TWO EMITTERS, ONE FUNCTION, CALLED TWICE — which is the second colour's whole implementation.**
	// A second copy of the birth arithmetic is the shape this repo keeps paying for (`sklitter_expanded.h`
	// lists four instances in this device alone); written once, the two populations cannot drift in
	// anything but the level that drives them and the flag they carry.
	//
	// **THE STRAIGHT ROAD FIRST, THE LANE SECOND, AND THE ORDER IS PART OF THE SEED'S MEANING.** They
	// share one random stream, so swapping them changes every draw — which is fine and is why a case
	// pins the order by pinning the counts.
	const size_t emittedBefore = emitted_;
	emitFrom(wetLevel, dt, nx, ny, /*fromLane=*/false);
	emitFrom(grainLevel, dt, nx, ny, /*fromLane=*/true);
	// **THE EVENT EMITTERS, LAST, so the two level emitters' draws are exactly what they were** — every
	// seeded count in the suites keeps its value. `sounding` is the same "is this instrument audible"
	// the level emitters answer by emitting at all.
	if (events != nullptr) {
		const bool sounding = (std::isfinite(wetLevel) && wetLevel > 0.0f)
		                      || (std::isfinite(grainLevel) && grainLevel > 0.0f);
		emitEvents(*events, nx, ny, sounding);
	}

	// **THE ANSWER, AND IT IS THE WHOLE CONTRACT (class header).** A frame is asked for when something
	// MOVED or DIED: sparks alive this tick, or sparks that were alive last tick and are not now — the
	// second is what paints the last one out. Silence with nothing alive answers `false`, asks for
	// nothing, and leaves this pad byte-identical to the one that had no clock.
	// **THE BIRTHS ARE COUNTED AS PUSHES, NOT AS DRAWS.** `poissonDraw` can answer more than the cap
	// will take; a tick whose every event was refused made no picture and must not claim a frame.
	// **ONE COMPARISON, NO PARTICLE TOUCHED — and that is the zero-repaint promise made CHEAPER by the
	// GPU pass rather than harder.** "Is anything alive" is now "did the retire index reach the end",
	// because retirement is by worst-case life and therefore a prefix. While something IS alive the pad
	// must still ask every frame: the picture moves because `u_spark_now` moves, and nothing else would
	// dirty the frame.
	const bool dirty = liveSparks() > 0 || hadSparks || emitted_ > emittedBefore;
	if (dirty) {
		redraw();
	}
	return dirty;
}

void PerformPad::drawSparks(visage::Canvas& canvas, const perform_pad::Disk& d) {
	drawnSparks_.clear();
	drawnChildren_.clear();
	drawnQuads_.clear();
	visible_.clear();
	const float k = scale();
	const float birthSize = kSparkSizeBirth * k;
	// **A SPARK'S SIZE SCALE BY ITS EMITTER** — 1 for the wet, `kSparkLaneSizeScale` for the lane,
	// `kSparkOnsetSizeScale` for a grain onset. One function, so the quad, the uniform and the CPU
	// reference cannot disagree about it.
	const auto sizeScaleOf = [](const Spark& s) {
		if (s.repeat) {
			return kSparkRepeatSizeScale;
		}
		return s.onset != 0 ? kSparkOnsetSizeScale : (s.fromLane ? kSparkLaneSizeScale : 1.0f);
	};

	// **THE UNIFORMS ARE THE SAME FOR EVERY SPARK OF ONE INK, WHICH IS WHY THEY ARE UNIFORMS.** Only
	// the clock, the drag and the two sizes vary between frames; everything per-spark rides the quad.
	// The SIZE is per ink since the onset emitter: the fragment stage draws the round shape at
	// `u_spark_now.z`, not at the quad's size, because a splitter's quad is larger than its spark.
	const auto arm = [&](visage::Shader& shader, uint32_t ink, float sizeScale, float hole, bool repeat) {
		shader.setUniformValue("u_spark_now", nowSeconds_, kSparkDrag, birthSize * sizeScale,
		                       kSparkSizeDeath * k * sizeScale);
		shader.setUniformValue("u_spark_look", kSparkAlpha, kSparkGlitterRate, hole, 0.0f);
		// **THE STREAK SPEED CONVERTS TO PIXELS HERE**, because the shader compares it against a
		// velocity it was handed in pixels. Converting in the shader instead would mean passing the
		// radius too, for a number that is constant across the whole batch.
		shader.setUniformValue("u_spark_shape", kSparkStreakMax, kSparkStreakAt,
		                       kSparkStreakSpeed * d.radius, kSparkStreakWaist);
		// THE SPLIT'S SHAPE — see `sparkChildOffsetAt` and `fs_spark.sc`. The kick itself is per quad. A
		// REPEAT spark splits at birth and grows its lobe faster (`sparkSplitAt`, `sparkGrowOf`), which is
		// why it is its own shader object: these are per-batch uniforms.
		Spark probe;
		probe.repeat = repeat;
		shader.setUniformValue("u_spark_split", kSparkSplitAngle, sparkGrowOf(probe), kSparkSplitChildScale,
		                       kSparkSplitNeck);
		shader.setUniformValue("u_spark_split_at", repeat ? 0.0f : kSparkSplitAtEarliest,
		                       repeat ? 0.0f : kSparkSplitAtLatest, 0.0f, 0.0f);
		const float r = static_cast<float>((ink >> 16) & 0xFFu) / 255.0f;
		const float g = static_cast<float>((ink >> 8) & 0xFFu) / 255.0f;
		const float b = static_cast<float>(ink & 0xFFu) / 255.0f;
		shader.setUniformValue("u_spark_ink", r, g, b, kSparkGlitter);
	};
	arm(sparkShader_, kSparkInk, 1.0f, 0.0f, false);
	arm(sparkLaneShader_, kSparkLaneInk, kSparkLaneSizeScale, 0.0f, false);
	for (size_t c = 0; c < onsetShaders_.size(); ++c) {
		// THROUGH `sparkInk`/`sparkHole`, on a spark of this class, so the uniform and the asserted
		// mapping are one function.
		Spark probe;
		probe.onset = static_cast<uint8_t>(c + 1u);
		arm(onsetShaders_[c], sparkInk(probe), kSparkOnsetSizeScale, sparkHole(probe), false);
	}
	{
		Spark probe;
		probe.repeat = true;
		arm(repeatShader_, sparkInk(probe), kSparkRepeatSizeScale, sparkHole(probe), true);
	}
	constexpr size_t kInks = 3u + fx::sklitter::kGrainOnsetClasses;
	const auto shaderOf = [this](const Spark& s) -> visage::Shader* {
		const size_t ink = inkOf(s);
		if (ink == kInks - 1u) {
			return &repeatShader_;
		}
		if (ink >= 2u) {
			return &onsetShaders_[ink - 2u];
		}
		return ink == 1u ? &sparkLaneShader_ : &sparkShader_;
	};

	// **PASS ONE: THE APERTURE AND THE RECORD, IN BIRTH ORDER.** Both paths share it, so they cannot
	// disagree about which sparks are drawn, where, or where their children are.
	for (size_t i = head_; i < sparks_.size(); ++i) {
		const Spark& s = sparks_[i];
		const float sz = birthSize * sizeScaleOf(s);
		lastSparkSize_ = sz;

		// **WHERE IT IS NOW — the closed form, evaluated once per spark per DRAW and shared by both
		// paths.** On the GPU path this is not what places the quad (the vertex shader does that from
		// the same formula); it is what decides whether the spark is inside the aperture at all, and
		// what the drawn record reports. See `fs_spark.sc` for why the aperture could not stay in the
		// shader.
		const auto [px, py] = sparkPointAt(s, nowSeconds_);
		// **THE APERTURE.** `drawSky` culls a displaced star at exactly `kSparkReach` because there is
		// no clip path and the pad's frame is wider than the disk by `rimInset()`; a spark past the rim
		// would paint on the rail outside the circle.
		if (std::hypot(px, py) > kSparkReach) {
			continue;
		}
		const auto [sx, sy] = perform_pad::pointOf(d, px, py);
		visible_.push_back(i);
		// RECORDED AS WHERE IT WILL BE, not where the quad was put — the picture's claim is about the
		// spark, and on the GPU path the quad's own rectangle is only the envelope.
		drawnSparks_.emplace_back(sx, sy);
		// **THE QUAD IS SIZED AT THE SHAPE'S LARGEST EXTENT — `sparkQuadSide`.** A vertex shader may
		// MOVE a quad out of its submitted rect (visage's clamp comes from the canvas's clip region, not
		// the shape's rectangle — measured, and it is what makes the translation work at all), but it
		// cannot make it BIGGER than the rectangle the rasteriser was handed. So the streak's room — and
		// a splitter's child lobe's — is reserved from birth and the fill is paid from frame one.
		drawnQuads_.push_back(sparkQuadSide(sz, s.split * d.radius));
		std::pair<float, float> child;
		if (sparkChildPointAt(s, nowSeconds_, child)) {
			drawnChildren_.push_back(perform_pad::pointOf(d, child.first, child.second));
		}
	}

	if (gpu_) {
		// **PASS TWO, ONE INK AT A TIME — and grouping is a batching decision, not a cosmetic one.**
		// visage's `autoBatchIndex` stops looking for a batch to join at the first later batch the new
		// shape OVERLAPS, and sparks overlap constantly near the puck; submitted interleaved in birth
		// order, the inks would fragment into a batch per run. Grouped, it is one batch per ink.
		for (size_t ink = 0; ink < kInks; ++ink) {
			for (size_t n = 0; n < visible_.size(); ++n) {
				const Spark& s = sparks_[visible_[n]];
				if (inkOf(s) != ink) {
					continue;
				}
				const float quad = drawnQuads_[n];
				const auto [bx, by] = perform_pad::pointOf(d, s.x, s.y);
				// **THE PER-QUAD BUDGET, NOW FULLY SPENT.** Six floats exist (the fork patch's
				// `kShaderQuadValues`), and a seventh is a fork change and a pin bump:
				//
				//     [0] vx   [1] vy   — PIXELS per second at birth. The shader works in canvas space.
				//     [2] birth          — seconds, on `nowSeconds_`'s clock
				//     [3] life           — seconds
				//     [4] seed           — 0..1, this spark's own random number
				//     [5] split          — the signed child kick, PIXELS per second; 0 = never splits
				//                          (backlog HR round 2 — `Spark::split`)
				//
				// **THE SEED IS A REAL FLOAT RATHER THAN A HASH OF THE VELOCITY.** Hashing `(vx, vy)` ties
				// every shape decision to the direction the spark happens to be flying, which is exactly
				// the correlation a random number is supposed to break — and the split's time now derives
				// from it.
				//
				// **THE SPLIT IS THE SPARE, SPENT — one float, and the rest derives.** The side is its
				// sign; the time is linear in `[4]`; the angle, growth, lobe size and neck are uniforms.
				// So a split costs no instance, no emission path and no seventh float.
				const float values[6] = { s.vx * d.radius, s.vy * d.radius, s.birth,
					                      s.life,          s.seed,          s.split * d.radius };
				canvas.shader(shaderOf(s), bx - quad * 0.5f, by - quad * 0.5f, quad, quad, values);
			}
		}
		return;
	}

	// **THE CPU REFERENCE — the same closed form, evaluated here instead of in the vertex stage.** It is
	// a known-good to photograph against rather than a fallback: the two differ only in WHO evaluates
	// `sparkPointAt`, so a capture of one against the other is a test of the shader. It draws squares
	// — no streak, no ring, no neck — because it is a reference for the TRAJECTORY, the parent's and
	// the child's, and not a pixel target.
	for (size_t n = 0; n < visible_.size(); ++n) {
		const Spark& s = sparks_[visible_[n]];
		const float age = std::min(std::max(0.0f, nowSeconds_ - s.birth), s.life);
		const float remain = s.life > 0.0f ? std::clamp(1.0f - age / s.life, 0.0f, 1.0f) : 0.0f;
		const float size = (kSparkSizeDeath + (kSparkSizeBirth - kSparkSizeDeath) * remain) * k;
		const float drawn = size * sizeScaleOf(s);
		const auto [sx, sy] = drawnSparks_[n];
		canvas.setColor(withAlpha(sparkInk(s), remain * kSparkAlpha));
		canvas.fill(sx - drawn * 0.5f, sy - drawn * 0.5f, drawn, drawn);
		std::pair<float, float> child;
		if (sparkChildPointAt(s, nowSeconds_, child)) {
			const auto [cx, cy] = perform_pad::pointOf(d, child.first, child.second);
			const float lobe = drawn * kSparkSplitChildScale * sparkChildGrowthAt(s, nowSeconds_ - s.birth);
			canvas.fill(cx - lobe * 0.5f, cy - lobe * 0.5f, lobe, lobe);
		}
	}
}

void PerformPad::drawSky(visage::Canvas& canvas, const perform_pad::Disk& d, float px, float py) {
	drawnStars_.clear();

	// **`PerformStage::drawSky`'s PULL, WITH THE CLOCK TAKEN OUT.** There, a star is first rotated by an
	// angle built from the shimmer and then pulled by the puck; here the rotation is gone and the pull
	// is all that is left, which is exactly the half that is a function of STATE rather than of time.
	// The constants are that function's, unchanged — a second set of numbers for the same visual idea
	// is two skies that drift apart the first time either is tuned.
	//
	// **IT READS THE SEED EVERY FRAME AND WRITES NOTHING BACK**, which is what makes the field a
	// mapping: the puck returning to where it was puts every star exactly where it was, with no state
	// to have accumulated.
	const auto& sky = performSky();
	for (size_t i = 0; i < sky.size(); ++i) {
		const SkyStar& star = sky[i];
		const float dx = star.x - px;
		const float dy = star.y - py;
		// The `+1e-4` is a substitute for dividing by zero when the puck sits exactly on a star, not a
		// floor on the distance — the same substitution `drawSky` makes, for the same reason.
		const float dist = std::hypot(dx, dy) + 1.0e-4f;
		const float pull = std::min(0.15f, 0.05f / (dist * dist + 0.055f));
		// Toward the puck and around it at once: the radial term is what gathers, the tangential term
		// is what makes it read as a swirl rather than as a squeeze. **AND THE TANGENTIAL TERM IS THE
		// BIGGER OF THE TWO**, so a star already close to the puck ends up FURTHER from it, not nearer —
		// measured, and it is why the case for this asserts the displacement's LEAN toward the puck
		// rather than a distance.
		const float x = star.x + (-dx / dist) * pull * 0.45f + (-dy / dist) * pull;
		const float y = star.y + (-dy / dist) * pull * 0.45f + (dx / dist) * pull;

		// **THE APERTURE CONTAINS THE SKY**, and this is what the mock's clip path does to a star the
		// warp has pushed past the rim. We have no clip, so a displaced star would otherwise paint on
		// the rail outside the circle.
		if (std::hypot(x, y) > 1.0f) {
			continue;
		}

		const auto [sx, sy] = perform_pad::pointOf(d, x, y);
		const float size = (star.magnitude > kBrightAbove ? kBrightStar : kFaintStar) * scale();
		canvas.setColor(withAlpha(kStarInk, star.magnitude * kStarAlpha));
		canvas.fill(sx, sy, size, size);
		drawnStars_.push_back({ i, sx, sy });
	}
}

void PerformPad::draw(visage::Canvas& canvas) {
	const perform_pad::Disk d = disk();
	if (!d.valid()) {
		return;
	}
	const sk::PerformWeights w = read();
	const float k = scale();

	// **THE SKY, FIRST, SO EVERYTHING ELSE IS DRAWN ON IT.** A function of the puck and of a fixed
	// table, with no clock anywhere in it — see the class header for why that distinction is the whole
	// design and what it cost to learn (backlog GJ).
	drawSky(canvas, d, w.x, w.y);

	// **THE SPARKS, OVER THE FIELD AND UNDER THE INSTRUMENT.** Backlog HR. `draw` only PAINTS them —
	// every spark's position was settled by `updateSparks`, which is the one road that touches the
	// clock and the one road that samples the puck for a velocity. A `draw` that advanced them would
	// make the animation a function of how often the frame happened to be painted, and would put a
	// second sampler on the puck (see the members' contemporaneity note).
	drawSparks(canvas, d);

	// THE DISK. **STILL A RING RATHER THAN A FILLED PLATE, AND THE OLD REASON STILL HOLDS.** It said
	// *"whatever the cool version paints inside it later, the plain one should not claim there is
	// anything there"*. The cool version has begun to arrive — there IS something in there now — and it
	// is still not a plate: the dark between the stars is the RAIL showing through (`CircleRail::draw`
	// paints `kVoid` since 2026-09-20, so the background behind the circle is the colour of Feathers'
	// perform background), so this surface claims exactly the stars it drew and nothing else.
	//
	// **AND THE RING WAS RE-MEASURED AGAINST THAT NEW GROUND rather than assumed to survive it**, because
	// the note below records this exact ring vanishing into a ground once. `kMuted` at 45% comes out
	// `#404350` over `#010204` against `#464D5F` over `#0E1420` before: darker in absolute value, and
	// SEPARATED FROM ITS GROUND BY MORE — per-channel (63,65,76) against (56,57,64), about 13% further
	// apart. So the alpha is unchanged, and that is a measurement rather than a look.
	//
	// **IT WAS `kSurfaceRaised` AND IT WAS INVISIBLE — PHOTOGRAPHED, not reasoned about.** On the
	// expanded panel's ground that token is close enough in value to vanish, so the first render of this
	// surface showed four marks and a puck floating in nothing, with no circle at all. The marks read
	// because they are `kMuted`; the ring is that at 45%, which puts it between the ground and them —
	// present as a surface, quieter than the things ON it. A geometry case cannot see this and did not:
	// every bound was right in a picture with no circle in it.
	canvas.setColor(withAlpha(kMuted, 0.45f));
	canvas.ring(d.cx - d.radius, d.cy - d.radius, d.radius * 2.0f, kRingWeight * k);

	// **THE CENTRE, MARKED, BECAUSE IT IS NOW A PLACE RATHER THAN A DEFAULT.** Since 2026-09-19 the
	// middle of this disk is the device with NO macro at all, which is a state a player aims at
	// deliberately — the circle's own bypass. A pad that drew nothing there would make the one position
	// with a name the only one with no mark.
	const float centre = kCentreRadius * k;
	canvas.setColor(kMuted);
	canvas.circle(d.cx - centre, d.cy - centre, centre * 2.0f);

	// **THE FOUR WAYPOINTS, FILLED BY THE WEIGHT THE ENGINE IS ACTUALLY USING.** `w.corner[i]` is the
	// share that waypoint has of the push right now — not a second computation of it (`perform.h`'s
	// contract), which is what keeps this picture and the sound the same numbers. A waypoint the puck
	// is facing reads full; one it has turned away from reads as an outline.
	const float mark = markRadius();
	for (size_t i = 0; i < sk::kCorners; ++i) {
		const auto [mx, my] = perform_pad::waypointPoint(d, static_cast<sk::Corner>(i));
		canvas.setColor(kMuted);
		canvas.ring(mx - mark, my - mark, mark * 2.0f, 1.0f * k);
		const float share = std::clamp(w.corner[i], 0.0f, 1.0f);
		if (share > 0.0f) {
			// The filled part is the share, as a radius — so a half-weighted waypoint is visibly half
			// taken rather than half transparent, which reads at this size and a fade does not.
			const float r = mark * share;
			canvas.setColor(kAccent);
			canvas.circle(mx - r, my - r, r * 2.0f);
		}
	}

	// **THE PUCK, WHERE THE PARAMETERS SAY — INCLUDING OUTSIDE THE RING.** `Perf X` and `Perf Y` are
	// modulation destinations and `performWeightsOf` clamps them to the SQUARE, so an automated pair at
	// (1, 1) is a real state that lies outside this disk by a factor of √2. Drawing it there is the
	// honest picture; moving it onto the rim would be the surface telling a story about a value it did
	// not have. A hand cannot reach it — `axesAt` clamps a drag to the disk — and loses nothing by that,
	// because every point beyond the centre region has a rim point with identical weights (see `axesAt`).
	const auto [px, py] = perform_pad::pointOf(d, w.x, w.y);
	const float puck = puckRadius();
	canvas.setColor(kInk);
	canvas.circle(px - puck, py - puck, puck * 2.0f);
}

void PerformPad::offeredControls(std::vector<Offered>& out) const {
	// THE PLANE'S TWO AXES, dragged together: side roads.
	out.push_back({ &x_, Road::Gesture });
	out.push_back({ &y_, Road::Gesture });
}

} // namespace feathers::ui
