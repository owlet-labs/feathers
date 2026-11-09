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

#include "plugin/ui/param_edit.h"
#include "plugin/ui/widget_registry.h"
#include "plugin/ui/stage_rail.h"
#include "plugin/ui/perform_stage.h"

#include "dsp/constellation.h"

#include "plugin/ui/celestial.h"
#include "plugin/ui/theme.h"

#include <visage/graphics.h>
#include <visage/utils.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>
#include <cstdlib>
#include <string>

namespace feathers::ui {

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kTau = 2.0f * kPi;

/// What the blend-space field says about a point: the colour of the character you would hear there,
/// and how far that point is from any one sound (the mock's `excite`).
///
/// THE FIELD IS NOT BUILT YET, and this is where it will arrive. Sampling it needs the interpolated
/// character of an arbitrary point in the disk — the same interpolation phase 2's N-sound blend is
/// built around — so today this returns the mock's own out-of-field fallback, `{242,244,250,0}`,
/// which is what its sky is drawn against wherever the field has nothing to say. Every formula below
/// is therefore the mock's verbatim, and the day fieldAt answers for real the sky becomes aware of
/// the sound with no other line changed.
struct FieldSample {
	float r = 242.0f;
	float g = 244.0f;
	float b = 250.0f;
	float excite = 0.0f;
};

FieldSample fieldAt(BlendPoint) {
	return {};
}

float distance(BlendPoint a, BlendPoint b) {
	return std::hypot(a.x - b.x, a.y - b.y);
}

/// OUR caption, and the one place in this file where the mock is NOT the authority.
///
/// Its static HTML reads `blend — mixing geometry`, but drawStage() replaces that on the first paint
/// with `perform — its own space · position is relative by design` — and that second half describes
/// precisely the Pos X/Y residual this surface deliberately does not have. Copying the spec verbatim
/// would have the stage promise, in words, the one behaviour the honesty rule made us drop. So the
/// words are chosen to be true of v1: they name what the circle IS and claim nothing about how a
/// position may be moved. It becomes the mock's line again when the residual becomes real.
///
/// Built once because draw() runs every frame under ambient motion, and at 25 bytes this string is
/// past the small-string limit — one heap allocation per frame for a constant.
const std::string& stageCaption() {
	static const std::string caption = "BLEND \xe2\x80\x94 MIXING GEOMETRY";
	return caption;
}

/// The mock's `.stagelbl` geometry, which IS ours to copy: its own tracking (`.3em`), not the
/// byline's, and its `top:16px`.
constexpr float kStageLabelTracking = 0.30f;
constexpr float kStageLabelTop = 16.0f;

} // namespace

//-------------//
// Constructor //
//-------------//

PerformStage::PerformStage(ParamBridge& bridge, WidgetRegistry& registry, Notifier* notifier,
                           Clock clock)
    : bridge_(bridge), notifier_(notifier), clock_(std::move(clock)) {
	if (!clock_) {
		clock_ = [] { return static_cast<int64_t>(visage::time::milliseconds()); };
	}
	reduced_ = reducedMotionRequested();
	// A pinned ambient clock, for looking at a particular moment of the breathing on purpose. The
	// glow's trough is the one that needed proving and it lasts a fraction of a second, so catching it
	// by running repeatedly is not a method. Reduced motion freezes the clock at zero, which is a
	// different phase, not this one.
	if (const char* at = std::getenv("FEATHERS_SHIMMER_AT")) {
		shimmer_ = std::atof(at);
		pinnedShimmer_ = shimmer_ > 0.0;
	}

	// The puck is derived from Path, so anything that moves Path — this drag, a knob, automation, the
	// host's own panel — has to redraw the circle. Pos X/Y move it off the tour, and every sound's
	// placement and enable moves the constellation the tour runs through, so all of them redraw it
	// too: a surface that showed the patch only until somebody changed the patch would be worse than
	// one that never showed it, because it would look right.
	watches_.push_back(registry.watchToken(pathId(), this));
	watches_.push_back(registry.watchToken(static_cast<clap_id>(ParamId::PosX), this));
	watches_.push_back(registry.watchToken(static_cast<clap_id>(ParamId::PosY), this));
	for (size_t slot = 0; slot < kMaxSounds; ++slot) {
		for (const SoundField field : { SoundField::PlaceX, SoundField::PlaceY, SoundField::Enable }) {
			watches_.push_back(registry.watchToken(static_cast<clap_id>(soundParam(slot, field)), this));
		}
	}

	onMouseDown() = [this](const visage::MouseEvent& e) {
		// THE CIRCLE IS THE CONTROL, not the rectangle it is centred in. The stage frame is far wider
		// than the disk — the mock's stage column is nearly square and ours has a whole editor's width
		// to spare — so a press in that dead space would move the sound from somewhere that does not
		// look like a control at all. Once a drag has started it tracks anywhere, which is the ordinary
		// grammar: the hand may leave the control without the gesture ending.
		// A STAR FIRST, THE PUCK OTHERWISE. Grabbing a sound is how the constellation gets arranged by
		// hand, and it has to win the press it lands on — the puck can be reached anywhere else in the
		// disk, a star only where it is. Decided once, at the press: re-deciding mid-drag would let a
		// star that happened to pass under the pointer steal a gesture aimed at the puck.
		draggingStar_ = starAt(e.position);
		if (draggingStar_.has_value()) {
			dragging_ = true;
			// GRABBED, NOT PLACED — by design, and see `starGrabOffset_`. The press latches how far
			// the star is from the pointer and moves nothing; the sound goes nowhere until the hand
			// does. Calling `dragStarTo` here is what used to spend an undo step on a press.
			// THE STAR'S OWN POSITION, READ THE WAY `starAt` JUST READ IT — off `stars()`, which
			// takes the MODULATED place, so the offset is measured against the star that was actually
			// drawn and pressed rather than against the unmodulated parameter behind it.
			for (const Star& star : stars()) {
				if (star.slot == *draggingStar_) {
					const visage::Point at = toPixels(star.at);
					starGrabOffset_ = visage::Point { at.x - e.position.x, at.y - e.position.y };
					break;
				}
			}
			return;
		}
		// THE CIRCLE IS THE CONTROL, not the rectangle it is centred in — see below. A star sitting
		// outside the disk is still grabbable, because it is still a thing you can see and place.
		if (!hitsDisk(e.position)) {
			return;
		}
		dragging_ = true;
		dragTo(e.position);
	};
	onMouseDrag() = [this](const visage::MouseEvent& e) {
		if (!dragging_) {
			return;
		}
		if (draggingStar_.has_value()) {
			// THE POINTER PLUS THE GRAB OFFSET — see `starGrabOffset_`.
			dragStarTo(*draggingStar_,
			           { e.position.x + starGrabOffset_.x, e.position.y + starGrabOffset_.y });
			return;
		}
		// AND THE PUCK IS UNCHANGED, deliberately: it goes where it is clicked.
		dragTo(e.position);
	};
	onMouseUp() = [this](const visage::MouseEvent&) {
		dragging_ = false;
		draggingStar_.reset();
		starGrabOffset_ = {};
		endGesture();
	};
}

bool PerformStage::reducedMotionRequested() {
	// One contract, one reading of it — see celestial.h. Kept as a member because it is what every
	// caller and every test already names, and because a surface's own answer is what they are asking
	// for.
	return ui::reducedMotionRequested();
}

//----------//
// Geometry //
//----------//

StageDisk PerformStage::disk() const {
	return { width(), height() };
}

float PerformStage::diskRadius() const {
	return disk().radius();
}

visage::Point PerformStage::diskCentre() const {
	return disk().centre();
}

visage::Point PerformStage::toPixels(BlendPoint p) const {
	return disk().toPixels(p.x, p.y);
}

bool PerformStage::hitsDisk(visage::Point pixels) const {
	return disk().contains(pixels);
}

BlendPoint PerformStage::fromPixels(visage::Point pixels) const {
	const visage::Point normalized = disk().toNormalized(pixels);
	return { normalized.x, normalized.y };
}

//----------//
// The tour //
//----------//

std::vector<PerformStage::Star> PerformStage::stars() const {
	std::vector<Star> live;
	live.reserve(kMaxSounds);
	for (size_t slot = 0; slot < kMaxSounds; ++slot) {
		const double enable = bridge_.modulated(static_cast<clap_id>(soundParam(slot, SoundField::Enable)));
		if (steppedIndex(enable, kEnableNames.size()) == 0) {
			continue;
		}
		Star star;
		star.at = { static_cast<float>(bridge_.modulated(
			            static_cast<clap_id>(soundParam(slot, SoundField::PlaceX)))),
			        static_cast<float>(bridge_.modulated(
			            static_cast<clap_id>(soundParam(slot, SoundField::PlaceY)))) };
		// The colour the sound wears, not the slot's — the same read the rail does, so a star and
		// its card cannot disagree about which sound they are (sound_hues.h).
		star.hue = kSeedHues[hueIndexOf(bridge_.soundHues(), slot)];
		star.slot = slot;
		star.label = static_cast<char>('A' + slot);
		live.push_back(star);
	}
	return live;
}

float PerformStage::heardAlpha(size_t slot) const {
	const Preview preview = bridge_.preview();
	if (!preview.active()) {
		return 1.0f;
	}
	return preview.slot == slot ? 1.0f : kUnheardAlpha;
}

std::optional<PerformStage::Star> PerformStage::unpatchedPreviewStar() const {
	const Preview preview = bridge_.preview();
	if (!preview.active()) {
		return std::nullopt;
	}
	const auto enable =
	    static_cast<clap_id>(soundParam(preview.slot, SoundField::Enable));
	if (steppedIndex(bridge_.modulated(enable), kEnableNames.size()) != 0) {
		// Already in the sky, drawn by stars() like any other member of the patch.
		return std::nullopt;
	}
	Star star;
	star.at = { static_cast<float>(bridge_.modulated(
		            static_cast<clap_id>(soundParam(preview.slot, SoundField::PlaceX)))),
		        static_cast<float>(bridge_.modulated(
		            static_cast<clap_id>(soundParam(preview.slot, SoundField::PlaceY)))) };
	star.hue = kSeedHues[hueIndexOf(bridge_.soundHues(), preview.slot)];
	star.slot = preview.slot;
	star.label = static_cast<char>('A' + preview.slot);
	return star;
}

BlendPoint PerformStage::pathPos(double t) const {
	// THE ENGINE'S OWN TOUR, called rather than restated. tourPoint walks the polyline at equal
	// PARAMETER per leg — so adding a third sound does not squeeze the first two into part of the knob
	// — and a second implementation of that rule here is a line on screen that could disagree with the
	// route Path actually walks.
	const std::vector<Star> live = stars();
	std::vector<AnchorPoint> anchors;
	anchors.reserve(live.size());
	for (const Star& star : live) {
		anchors.push_back({ star.at.x, star.at.y });
	}
	const AnchorPoint at = tourPoint(anchors, t);
	return { static_cast<float>(at.x), static_cast<float>(at.y) };
}

double PerformStage::nearestPathT(BlendPoint p) const {
	const std::vector<Star> live = stars();
	if (live.size() < 2) {
		return 0.0;
	}
	// Projected onto EVERY LEG, keeping the best. The mock searches a windowed hundred samples of the
	// tour because a free search snaps between segments once there are three or more anchors; the legs
	// are the exact version of what that window approximates, and there are at most seven of them.
	const double legs = static_cast<double>(live.size() - 1);
	double bestT = 0.0;
	float bestDistance = std::numeric_limits<float>::max();
	for (size_t leg = 0; leg + 1 < live.size(); ++leg) {
		const BlendPoint from = live[leg].at;
		const BlendPoint to = live[leg + 1].at;
		const float dx = to.x - from.x;
		const float dy = to.y - from.y;
		const float lengthSquared = dx * dx + dy * dy;
		double local = 0.0;
		if (lengthSquared > 0.0f) {
			local = std::clamp(static_cast<double>(((p.x - from.x) * dx + (p.y - from.y) * dy)
			                                       / lengthSquared),
			                   0.0, 1.0);
		}
		const BlendPoint on { from.x + dx * static_cast<float>(local),
			                  from.y + dy * static_cast<float>(local) };
		const float reach = distance(on, p);
		if (reach < bestDistance) {
			bestDistance = reach;
			bestT = (static_cast<double>(leg) + local) / legs;
		}
	}
	return bestT;
}

clap_id PerformStage::pathId() const noexcept {
	return static_cast<clap_id>(ParamId::Path);
}

double PerformStage::pathValue() const {
	return bridge_.value(pathId());
}

double PerformStage::pathModulated() const {
	return bridge_.modulated(pathId());
}

BlendPoint PerformStage::puck() const {
	const BlendPoint on = pathPos(pathModulated());
	return { on.x + static_cast<float>(bridge_.modulated(static_cast<clap_id>(ParamId::PosX))),
		     on.y + static_cast<float>(bridge_.modulated(static_cast<clap_id>(ParamId::PosY))) };
}

BlendPoint PerformStage::puckBase() const {
	const BlendPoint on = pathPos(pathValue());
	return { on.x + static_cast<float>(bridge_.value(static_cast<clap_id>(ParamId::PosX))),
		     on.y + static_cast<float>(bridge_.value(static_cast<clap_id>(ParamId::PosY))) };
}

bool PerformStage::showsModulation() const {
	// A whole thousandth of the parameter's range, so a host that publishes a modulated reading equal
	// to the base one in all but floating-point noise does not permanently draw a second state on top
	// of the first.
	return std::abs(pathModulated() - pathValue()) > 0.001;
}

//------------//
// The gesture //
//------------//

std::optional<size_t> PerformStage::starAt(visage::Point pixels) const {
	// THE PUCK WINS A TIE, and this is not a detail. The puck sits ON a sound whenever Path is parked
	// at one end of the tour, so a press there is genuinely ambiguous — and the two readings fail in
	// very different ways. Guessing "star" moves somebody's sound when they meant to move where they
	// are standing, silently changing the patch; guessing "puck" means a star under the puck cannot be
	// grabbed until Path is moved off it, which is a thing you notice and can undo by moving.
	const visage::Point puckAt = toPixels(puck());
	if (std::hypot(puckAt.x - pixels.x, puckAt.y - pixels.y) <= kStarGrabPixels) {
		return std::nullopt;
	}

	// Otherwise the NEAREST star within reach, not the first — two sounds dragged close together would
	// hand the press to whichever sits earlier in the patch rather than to the one under the pointer,
	// and "the one I clicked" is the only rule a hand believes.
	std::optional<size_t> best;
	float bestReach = kStarGrabPixels;
	for (const Star& star : stars()) {
		const visage::Point at = toPixels(star.at);
		const float reach = std::hypot(at.x - pixels.x, at.y - pixels.y);
		if (reach <= bestReach) {
			bestReach = reach;
			best = star.slot;
		}
	}
	return best;
}

void PerformStage::dragStarTo(size_t slot, visage::Point pixels) {
	// Clamped into the disk like the puck: a sound outside the circle is a sound you cannot see, and
	// the tour would run off the edge of the surface to reach it.
	BlendPoint at = fromPixels(pixels);
	const float reach = std::hypot(at.x, at.y);
	if (reach > 1.0f) {
		at = { at.x / reach, at.y / reach };
	}
	commit(static_cast<clap_id>(soundParam(slot, SoundField::PlaceX)), static_cast<double>(at.x));
	commit(static_cast<clap_id>(soundParam(slot, SoundField::PlaceY)), static_cast<double>(at.y));

	if (notifier_ != nullptr) {
		char printed[48] = {};
		std::snprintf(printed, sizeof(printed), "Sound %c placed", static_cast<char>('A' + slot));
		notifier_->say(printed);
	}
	redraw();
}

void PerformStage::dragTo(visage::Point pixels) {
	// THE MOCK'S OWN DECOMPOSITION, restored now that it is honest: the nearest point ON the tour is
	// Path, and the rest of the way is the Pos X/Y residual. v1 dropped the residual because the
	// engine did not read those parameters — a puck sliding off the line would have shown a mixture
	// nobody was hearing. Phase 2 reads them, so keeping the drag on the line would now be the lie:
	// the hand would go one way and the sound another.
	// CLAMPED INTO THE DISK FIRST. A drag that started on the circle keeps tracking when the hand runs
	// off it — that is the ordinary grammar — but the puck itself must stay inside the control it
	// belongs to. Without this the residual is whatever the mouse coordinate says: a drag to the corner
	// of a large window posted Pos X of 32, against a parameter declared -1..1, which the engine would
	// clamp and the picture would not. Found by the range check on the off-circle drag.
	BlendPoint at = fromPixels(pixels);
	const float reach = std::hypot(at.x, at.y);
	if (reach > 1.0f) {
		at = { at.x / reach, at.y / reach };
	}
	const double t = nearestPathT(at);
	const BlendPoint on = pathPos(t);

	commit(pathId(), t);
	commit(static_cast<clap_id>(ParamId::PosX), static_cast<double>(at.x - on.x));
	commit(static_cast<clap_id>(ParamId::PosY), static_cast<double>(at.y - on.y));

	if (notifier_ != nullptr) {
		char printed[32] = {};
		if (valueToText(pathId(), t, printed, sizeof(printed))) {
			notifier_->say(std::string("Path ") + printed);
		}
	}
	redraw();
}

void PerformStage::commit(clap_id id, double value) {
	if (bridge_.value(id) == value) {
		return;
	}
	// Same grammar as the knobs: a gesture opens on the first value that actually moves, so a click
	// landing where the puck already is spends no undo step. Tracked per id, because a drag moves
	// three parameters and each has to be closed on the id it was opened on.
	if (std::find(gesturesOpen_.begin(), gesturesOpen_.end(), id) == gesturesOpen_.end()) {
		valuesBefore_.push_back(bridge_.value(id));
		bridge_.beginGesture(id);
		gesturesOpen_.push_back(id);
	}
	bridge_.setValue(id, value);
}

void PerformStage::endGesture() {
	for (const clap_id id : gesturesOpen_) {
		bridge_.endGesture(id);
	}
	// **THE DRAG IS ONE UNDO STEP** (backlog JB): up to three parameters moved by one hand, grouped, so one
	// Ctrl+Z puts the puck back where the press found it. A parameter the drag left where it started adds
	// nothing, and a drag that moved nothing records nothing (an empty group records no step).
	if (ui::EditHistory* history = bridge_.editHistory(); history != nullptr && !gesturesOpen_.empty()) {
		history->beginGroup("Perform");
		for (size_t i = 0; i < gesturesOpen_.size(); ++i) {
			const double after = bridge_.value(gesturesOpen_[i]);
			if (after != valuesBefore_[i]) {
				history->record(std::make_unique<ParamValueEdit>(bridge_, gesturesOpen_[i], valuesBefore_[i], after));
			}
		}
		history->endGroup();
	}
	gesturesOpen_.clear();
	valuesBefore_.clear();
}

//--------//
// Motion //
//--------//

bool PerformStage::update() {
	const int64_t now = clock_();
	if (!everTicked_) {
		everTicked_ = true;
		lastTick_ = now;
		lastPuck_ = puck();
	}
	// The mock's own clamp: a frame that took longer than 50ms — a stalled host, a window being
	// dragged — advances the ambient world by 50ms rather than by however long the freeze was.
	const double dt = std::min(0.05, static_cast<double>(now - lastTick_) / 1000.0);
	lastTick_ = now;

	bool dirty = false;
	if (pinnedShimmer_) {
		// Held where it was put: the sky still redraws, so the surface is live, but the phase does not
		// move under the capture.
		redraw();
		return true;
	}
	if (!reduced_) {
		// AMBIENT MOTION IS ALWAYS ON while the editor is open (decided 2026-08-12). The sky turns
		// whether or not anyone is playing, which is the whole difference between Perform's space and
		// Explore's: this one is alive, that one moves only when you do.
		shimmer_ += dt;
		dirty = true;
	}

	const BlendPoint now_puck = puck();
	if (showsModulation() && !reduced_) {
		trail_.push_back(now_puck);
		if (trail_.size() > kTrailLength) {
			trail_.pop_front();
		}
		dirty = true;
	}
	else if (!trail_.empty()) {
		// The mock drains it a point per frame when the modulation stops, so the tail retracts instead
		// of blinking out.
		trail_.pop_front();
		dirty = true;
	}

	// Ripples that have finished are dropped rather than left to be skipped by every later draw. They
	// are bounded anyway, so this is not about memory: an expired ripple that lingers is state the
	// surface still holds and can still be asked about, and rippleCount() would report a wake that is
	// no longer on screen.
	std::erase_if(ripples_, [this](const Ripple& ripple) {
		return shimmer_ - ripple.born >= kRippleLife;
	});

	// The comet's wake. On the shimmer clock, which is why reduced motion has no ripples without a
	// rule of its own saying so: a frozen clock never reaches the next one's spacing.
	if (distance(now_puck, lastPuck_) > kRippleTrigger && shimmer_ - lastRipple_ > kRippleSpacing) {
		ripples_.push_back({ now_puck, shimmer_ });
		if (ripples_.size() > kMaxRipples) {
			ripples_.erase(ripples_.begin());
		}
		lastRipple_ = shimmer_;
		dirty = true;
	}
	lastPuck_ = now_puck;

	if (dirty) {
		redraw();
	}
	return dirty;
}

//---------//
// Drawing //
//---------//

void PerformStage::draw(visage::Canvas& canvas) {
	const float radius = diskRadius();
	if (radius <= 0.0f) {
		return;
	}
	// Every glyph size in the mock is quoted against its 520px canvas, so one ratio carries all of
	// them to whatever size the editor is at.
	const float scale = disk().glyphScale();

	drawSounds(canvas);
	drawSky(canvas);
	drawWake(canvas);
	drawTrail(canvas);
	drawTour(canvas, scale);

	// The rim last of the circle's own furniture: the mock's 1.5px line, which is what makes the
	// space read as an instrument's aperture rather than as a gradient that faded out.
	const visage::Point centre = diskCentre();
	canvas.setColor(kLine);
	canvas.ring(centre.x - radius, centre.y - radius, radius * 2.0f, 1.5f);

	drawReticle(canvas, scale);
	// Last, so a star drifting under it never eats a letter: it is an overlay in the mock too.
	drawLabel(canvas);
}

void PerformStage::drawLabel(visage::Canvas& canvas) const {
	// A dark disk with two stars in it is a picture without a noun; this is the noun. See
	// stageCaption() for why the words are ours rather than the mock's.
	const visage::Font& face = font(kBylineSize);
	// CLEAR OF THE COLLAPSE TOGGLE in the top-right corner (backlog NF, `StageRail::captionLeft`); a stage too
	// narrow for the whole line beside it says just its noun.
	const std::string* caption = &stageCaption();
	float captionX = StageRail::captionLeft(width(), trackedWidth(face, *caption, kStageLabelTracking));
	if (captionX < StageRail::kCaptionMinLeft) {
		static const std::string noun = "BLEND";
		caption = &noun;
		captionX = StageRail::captionLeft(width(), trackedWidth(face, noun, kStageLabelTracking));
	}
	canvas.setColor(kMuted);
	drawTracked(canvas, *caption, face, kStageLabelTracking, captionX, kStageLabelTop, kBylineSize * 1.6f);
}

void PerformStage::drawSounds(visage::Canvas& canvas) const {
	const visage::Point centre = diskCentre();
	const float radius = diskRadius();

	// Each sound's glow, breathing on its own phase. Added rather than blended, which is the mock's
	// `globalCompositeOperation='lighter'`: two sounds close together should brighten where they
	// overlap, because that is where you would hear both.
	canvas.setBlendMode(visage::BlendMode::Add);
	int index = 0;
	for (const Star& sound : stars()) {
		const visage::Point at = toPixels(sound.at);
		const float phase = static_cast<float>(shimmer_);
		const float breath = 1.0f + 0.14f * std::sin(phase * 0.7f + static_cast<float>(index) * 2.1f);
		const float inner = std::min(width(), height()) * 0.012f;
		const float outer = radius * 0.8f * breath;
		// The mock's numbers, and they SUPERSEDE its originals (.11 +/- .04 peak, .035 mid), changed on
		// 2026-08-12 after seeing them built: too faint, and the pulse dipped far enough
		// that the glow read as fully transparent at its trough. The base is roughly doubled and the
		// swing cut from +/-36% of base to +/-14%, so it still breathes and never goes out. Changed in
		// mock/feathers-ui-mock.html first, so the spec stays the single source and stays true to what
		// ships. The radius breathing above is untouched — size was the part of the pulse that worked.
		//
		// Raised again to .28/.09 when the seeds were saturated (same day): a more
		// saturated colour of the same value sums to less light, and this puts the presence back where
		// it had been approved. Compensation for the chroma change, not a second opinion about how bright
		// the glow should be.
		// AND EVERYTHING FALLS BACK EXCEPT WHAT IS SOUNDING, while a preview is latched — the glow is
		// the circle's loudest statement about where the sound is coming from, so it is the first thing
		// that would be lying.
		const float heard = heardAlpha(sound.slot);
		const float peak =
		    (0.28f + 0.04f * std::sin(phase * 0.9f + static_cast<float>(index) * 1.7f)) * heard;
		// THE FOCAL RADIUS IS A FRACTION OF THE RADIUS, NOT A LENGTH, and nothing in the signature says
		// so — it sits between two pixel radii called radius_x and radius_y. visage's own SVG loader is
		// the only in-tree caller that passes one, and it divides first: `focal_radius / radius`,
		// visage_graphics/svg.h:86.
		//
		// Handing it the mock's inner radius in pixels (~6 against an outer of ~200) put a 6 where at
		// most a 1 belongs, and the gradient came out INSIDE OUT: alpha 0 at the sound, ramping up
		// toward the rim. The halos were not missing, they were inverted, and at these alphas that
		// reads as a faint wash near the edge of the disk and nothing around the stars at all.
		//
		// Three stops at 0, ½ and 1, evenly spaced — which is the mock's gradient exactly.
		canvas.setColor(visage::Brush::radial(
		    visage::Gradient(visage::Color(withAlpha(sound.hue, peak)),
		                     visage::Color(withAlpha(sound.hue, 0.09f * heard)),
		                     visage::Color(withAlpha(sound.hue, 0.0f))),
		    { at.x, at.y }, outer, outer, { at.x, at.y }, inner / outer));
		canvas.circle(centre.x - radius, centre.y - radius, radius * 2.0f);
		++index;
	}
	canvas.setBlendMode(visage::BlendMode::Alpha);
}

void PerformStage::drawSky(visage::Canvas& canvas) const {
	const visage::Point centre = diskCentre();
	const float radius = diskRadius();
	const float phase = static_cast<float>(shimmer_);
	const BlendPoint puckAt = puck();

	for (const SkyStar& star : performSky()) {
		// `Math.hypot(st.x,st.y)||.01` — a substitute for ZERO, not a floor. std::max would also lift
		// every star inside 0.01 of the centre, which is a different rule; this sky has none (the
		// closest sits at 0.0895) so the two agree today, and only one of them still agrees if the
		// star table ever changes.
		const float measured = std::hypot(star.x, star.y);
		const float ring = measured == 0.0f ? 0.01f : measured;
		const FieldSample here = fieldAt({ star.x, star.y });
		// Differential rotation: the inner sky turns faster than the rim, which is what makes it read
		// as a galaxy rather than as a spinning picture.
		const float twist = ((here.r - here.b) / 255.0f) * 0.55f + ((here.g - here.r) / 255.0f) * 0.3f;
		const float angle = phase * 0.05f / (0.35f + ring) * (0.55f + 1.5f * here.excite) + twist;
		const float cosA = std::cos(angle);
		const float sinA = std::sin(angle);
		float x = star.x * cosA - star.y * sinA;
		float y = star.x * sinA + star.y * cosA;

		// And the puck pulls on what it passes: nearby stars swirl around it. The instrument's cursor
		// has mass in its own sky.
		const float dx = x - puckAt.x;
		const float dy = y - puckAt.y;
		const float d = std::hypot(dx, dy) + 1.0e-4f;
		const float pull = std::min(0.15f, 0.05f / (d * d + 0.055f));
		x += (-dx / d) * pull * 0.45f + (-dy / d) * pull;
		y += (-dy / d) * pull * 0.45f + (dx / d) * pull;

		// THE APERTURE CONTAINS THE SKY. The gravitation above can push a star near the rim outside
		// the disk by up to about a fifth of the radius, and the mock never shows that because it
		// clips everything to the circle — we have no clip, so a displaced star would paint on the
		// bare stage outside the aperture. Skipping it is what the clip does to it.
		if (std::hypot(x, y) > 1.0f) {
			continue;
		}

		const FieldSample there = fieldAt({ x, y });
		const float luminance = (there.r * 0.35f + there.g * 0.5f + there.b * 0.15f) / 255.0f;
		// Rounded, not truncated, because the mock rounds. Worth a lround today only in the sense that
		// a systematic half-LSB darkening is the kind of thing nobody finds later: once fieldAt()
		// returns real colour this runs over the whole palette rather than over one constant.
		const auto channel = [](float value) {
			return static_cast<uint32_t>(std::clamp(std::lround(value), 0L, 255L));
		};
		const uint32_t colour = 0xff000000u | (channel(there.r + (255.0f - there.r) * 0.55f) << 16)
		                        | (channel(there.g + (255.0f - there.g) * 0.55f) << 8)
		                        | channel(there.b + (255.0f - there.b) * 0.55f);
		const float twinkle = 0.18f * std::sin(phase * (1.2f + 2.5f * there.excite) + star.x * 9.0f
		                                       + star.y * 7.0f);
		const float alpha = star.magnitude * (0.14f + 0.55f * luminance + twinkle);
		const float size = (star.magnitude > 0.75f ? 1.4f : 0.9f) * (1.0f + luminance * 1.1f);
		canvas.setColor(withAlpha(colour, alpha));
		canvas.fill(centre.x + x * radius, centre.y + y * radius, size, size);
	}

	// The mock's three faint rings, each shimmering on its own phase. Not a grid: they are what gives
	// the disk depth, and they are the only thing in here drawn at a fixed radius.
	constexpr std::array<float, 3> kRings { 0.35f, 0.65f, 0.92f };
	for (size_t k = 0; k < kRings.size(); ++k) {
		const float alpha = 0.07f - 0.018f * static_cast<float>(k)
		                    + 0.02f * std::sin(phase * 0.5f + static_cast<float>(k) * 2.0f);
		const float r = radius * kRings[k];
		canvas.setColor(withAlpha(tokens::kGrid, alpha));
		canvas.ring(centre.x - r, centre.y - r, r * 2.0f, 1.0f);
	}
}

void PerformStage::drawWake(visage::Canvas& canvas) const {
	const float radius = diskRadius();

	for (const Ripple& ripple : ripples_) {
		const double age = shimmer_ - ripple.born;
		const double life = age / kRippleLife;
		if (life >= 1.0 || life < 0.0) {
			continue;
		}
		const visage::Point at = toPixels(ripple.at);
		const float r = static_cast<float>(age) * 0.5f * radius;
		if (r <= 0.0f) {
			continue;
		}
		canvas.setColor(withAlpha(kAccent, static_cast<float>(0.16 * (1.0 - life))));

		// The mock clips the whole circle to the disk; visage has no clip path, so the part of the ring
		// that has left the aperture is worked out and simply not drawn. That arithmetic was written
		// here first and now lives in clipRingToDisk, because the puck's marks need the same answer and
		// two copies of it would be two opinions about where this surface's rim is.
		const std::optional<RingArc> visible = clipRingToDisk(disk(), at, r);
		if (!visible.has_value()) {
			continue;
		}
		if (visible->whole) {
			canvas.ring(at.x - r, at.y - r, r * 2.0f, 1.0f);
			continue;
		}
		canvas.arc(at.x - r, at.y - r, r * 2.0f, 1.0f, visible->middle, visible->half, false);
	}
}

void PerformStage::drawTrail(visage::Canvas& canvas) const {
	// The trail the modulation leaves. trail_ holds oldest-first, and the alpha ramps up with the
	// index, so the oldest point is the faintest and the newest sits just behind the reticle.
	//
	// It is accumulated ONLY while something is modulating Path — a hand-driven puck leaves ripples
	// instead. Drawn here, before the tour and the sounds, so that it passes UNDER both: it is the
	// faintest thing on the surface and must not sit on top of the things it is a history of.
	const size_t count = trail_.size();
	for (size_t i = 0; i < count; ++i) {
		const visage::Point at = toPixels(trail_[i]);
		const float alpha = static_cast<float>(i) / static_cast<float>(count) * 0.45f;
		// Unconditional, and the comment above carries the guarantee: this is accumulated ONLY while
		// something is modulating Path, so every point of it is by construction a modulated one.
		canvas.setColor(withAlpha(movingMarkColour(true), alpha));
		canvas.circle(at.x - 2.2f, at.y - 2.2f, 4.4f);
	}
}

void PerformStage::drawTour(visage::Canvas& canvas, float scale) const {
	const std::vector<Star> live = stars();

	// The tour made visible: the route the Path knob drives along, dashed because it is a route rather
	// than a thing. One segment with two sounds, the mock's polyline with more — and it is drawn leg by
	// leg from the same anchors the engine tours, so a sound switched on appears in the line as well as
	// in the sky.
	canvas.setColor(withAlpha(kAccent, 0.30f));
	for (size_t leg = 0; leg + 1 < live.size(); ++leg) {
		dashedLine(canvas, toPixels(live[leg].at), toPixels(live[leg + 1].at), 2.0f, 5.0f, 1.0f);
	}

	for (const Star& sound : live) {
		const visage::Point at = toPixels(sound.at);
		const float heard = heardAlpha(sound.slot);
		starGlyph(canvas, at.x, at.y, 5.0f * scale, sound.hue, heard);
		if (heard >= 1.0f && bridge_.preview().active()) {
			// RINGED, because dimming the others says "quieter" and a hand needs to be told WHICH one is
			// the exception. The same mark the rail puts round the card, so the two surfaces name the
			// sound the same way.
			canvas.setColor(kAccent);
			// A RING, not a disc: `circle` fills, and a filled mark here would cover the star it is
			// pointing at.
			canvas.ring(at.x - 9.0f * scale, at.y - 9.0f * scale, 18.0f * scale, 1.5f);
		}
		// Which star is which. The mock names its sounds in a pool list beside the stage; there is no
		// such list here, and the sounds ARE "A".."H" everywhere else in the editor — the parameter
		// names, the Engine rail, the knob tray — so the letters are the same identification rather than
		// a new invention.
		const visage::Font& face = font(kKnobLabelSize);
		canvas.setColor(withAlpha(kMuted, heard));
		const char label[2] = { sound.label, '\0' };
		canvas.text(label, face, visage::Font::kCenter, at.x - 10.0f, at.y + 12.0f * scale, 20.0f,
		            kKnobLabelSize * 1.6f);
	}

	// AND THE ONE SOUND THAT IS BEING HEARD WITHOUT BEING IN THE PATCH. Preview ignores the Enable
	// switch on purpose — auditioning a sound before putting it in the patch is what it is for — so a
	// previewed slot can be switched off, and drawing nothing there would leave a circle of stars,
	// none of which you can hear. It is drawn OUTLINED rather than filled: it is sounding, and it is
	// still not a member of the constellation, and the picture has to say both.
	if (const std::optional<Star> unpatched = unpatchedPreviewStar()) {
		const visage::Point at = toPixels(unpatched->at);
		starGlyph(canvas, at.x, at.y, 5.0f * scale, unpatched->hue, 1.0f);
		canvas.setColor(kAccent);
		canvas.ring(at.x - 9.0f * scale, at.y - 9.0f * scale, 18.0f * scale, 1.5f);
		const visage::Font& face = font(kKnobLabelSize);
		canvas.setColor(kMuted);
		const char label[2] = { unpatched->label, '\0' };
		canvas.text(label, face, visage::Font::kCenter, at.x - 10.0f, at.y + 12.0f * scale, 20.0f,
		            kKnobLabelSize * 1.6f);
	}
}

void PerformStage::puckLine(visage::Canvas& canvas, visage::Point from, visage::Point to,
                            float thickness) {
	const std::optional<std::pair<visage::Point, visage::Point>> inside =
	    clipSegmentToDisk(disk(), from, to);
	if (!inside.has_value()) {
		return;
	}
	puckInk_.add(inside->first, inside->second, thickness * 0.5f);
	canvas.segment(inside->first.x, inside->first.y, inside->second.x, inside->second.y, thickness,
	               false);
}

void PerformStage::puckArc(visage::Canvas& canvas, visage::Point at, float radius, float middle,
                           float half, float thickness) {
	if (radius <= 0.0f || half <= 0.0f) {
		return;
	}
	const std::optional<RingArc> visible = clipRingToDisk(disk(), at, radius);
	if (!visible.has_value()) {
		return;
	}

	// THE PIECES OF THIS ARC THAT SURVIVE, which is an intersection of two spans around a circle and
	// so can be two pieces rather than one. Worked in the arc's own frame: the visible span sits at
	// `delta` from this arc's middle, and the same span also lives a turn either side of that — three
	// copies, of which at most two can overlap a span no wider than half the circle. Testing only the
	// copy at `delta` is the bug this shape invites: an arc straddling the wrap would lose the half of
	// itself that is written in the other copy, and it would lose it only at certain angles.
	const auto draw = [&](float low, float high) {
		if (high <= low) {
			return;
		}
		const float pieceMiddle = middle + (low + high) * 0.5f;
		const float pieceHalf = (high - low) * 0.5f;
		canvas.arc(at.x - radius, at.y - radius, radius * 2.0f, thickness, pieceMiddle, pieceHalf,
		           false);
		// The ends and the middle of what was drawn. An arc bulges away from its chord, so the middle
		// has to be in the record or the box would under-report by exactly the amount that matters
		// least here and most on a wider arc.
		for (const float angle : { pieceMiddle - pieceHalf, pieceMiddle, pieceMiddle + pieceHalf }) {
			puckInk_.add({ at.x + std::cos(angle) * radius, at.y + std::sin(angle) * radius },
			             thickness * 0.5f);
		}
	};

	if (visible->whole) {
		draw(-half, half);
		return;
	}
	float delta = visible->middle - middle;
	// Into (−π, π], so the three copies below are the three that can possibly reach this arc.
	delta -= kTau * std::floor((delta + kPi) / kTau);
	for (const float turn : { -kTau, 0.0f, kTau }) {
		const float low = delta + turn - visible->half;
		const float high = delta + turn + visible->half;
		draw(std::max(-half, low), std::min(half, high));
	}
}

void PerformStage::puckDot(visage::Canvas& canvas, visage::Point at, float radius) {
	if (radius <= 0.0f || !disk().contains(at)) {
		return;
	}
	puckInk_.add(at, radius);
	canvas.circle(at.x - radius, at.y - radius, radius * 2.0f);
}

InkBox PerformStage::diskSquare() const {
	return diskBox(disk());
}

void PerformStage::drawReticle(visage::Canvas& canvas, float scale) {
	const visage::Point at = toPixels(puck());
	puckInk_ = InkBox {};

	// The two-state display, when the host is modulating Path: the base value keeps a dashed anchor
	// ring where your hand left it, tethered to the reticle that has ridden away from it. Without
	// this a modulated editor shows one position and the automation lane shows another, and neither
	// says which is which.
	if (showsModulation()) {
		const visage::Point base = toPixels(puckBase());
		canvas.setColor(withAlpha(authoredMarkColour(), 0.25f));
		puckLine(canvas, base, at, 1.0f);
		canvas.setColor(withAlpha(authoredMarkColour(), 0.7f));
		// The dashed ring, dash by dash, so each one is clipped where it crosses rather than the whole
		// ring being kept or dropped together.
		// The same dashes dashedRing() would lay down — centre angle `i * step`, half-span a quarter of
		// the step — walked here rather than called, because each one has to go through the clip.
		constexpr int kDashes = 8;
		const float ringRadius = 7.0f * scale;
		const float step = kTau / static_cast<float>(kDashes);
		for (int i = 0; i < kDashes; ++i) {
			puckArc(canvas, base, ringRadius, static_cast<float>(i) * step, step * 0.25f, 1.0f);
		}
		canvas.setColor(withAlpha(authoredMarkColour(), 0.9f));
		puckDot(canvas, base, 1.6f * scale);
	}

	// The telescope reticle: a ring gapped at the cardinals, with ticks bridging the gaps.
	const float ring = 13.0f * scale;
	const float tick = 5.0f * scale;
	// The one colour the ring and its four ticks share — R3, and the only line that had to
	// change for it, because the tick loop below sets no colour of its own.
	canvas.setColor(withAlpha(movingMarkColour(showsModulation()), 0.85f));
	constexpr float kGap = 0.3f; // the mock's `k*PI/2+.3` … `(k+1)*PI/2-.3`
	for (int k = 0; k < 4; ++k) {
		const float centreAngle = static_cast<float>(k) * kPi * 0.5f + kPi * 0.25f;
		puckArc(canvas, at, ring, centreAngle, kPi * 0.25f - kGap, 1.0f);
	}
	for (int k = 0; k < 4; ++k) {
		const float angle = static_cast<float>(k) * kPi * 0.5f;
		const float cosA = std::cos(angle);
		const float sinA = std::sin(angle);
		puckLine(canvas, { at.x + cosA * (ring - tick * 0.55f), at.y + sinA * (ring - tick * 0.55f) },
		         { at.x + cosA * (ring + tick * 0.75f), at.y + sinA * (ring + tick * 0.75f) }, 1.0f);
	}
	const float core = 1.7f * scale;
	canvas.setColor(kPointer);
	puckDot(canvas, at, core);

	// AND THE PUCK IS NEVER LOST. Clipping alone would let the cursor vanish outright — the puck can
	// stand up to about two and a half radii out, since Pos X and Pos Y each reach ±1 on top of a tour
	// point that already lives in the unit disk — and A30's objection to clipping was exactly that it
	// hides where the hand is. The decision overrode the spill, not the hand: so when NOTHING of the reticle
	// is inside, a dim tick sits on the rim in the puck's direction, which is the vocabulary A27
	// already established on Explore for a thing that is out there. No label, because unlike Explore's
	// star this position is on two knobs in the tray and in the host's own automation lane.
	if (puckInk_.any) {
		return;
	}
	const StageDisk d = disk();
	const float dx = at.x - d.centre().x;
	const float dy = at.y - d.centre().y;
	const float reach = std::hypot(dx, dy);
	if (!std::isfinite(reach) || reach <= 0.0f) {
		return;
	}
	constexpr float kRimTickInset = 3.0f;
	const visage::Point rim { d.centre().x + dx / reach * (d.radius() - kRimTickInset),
		                      d.centre().y + dy / reach * (d.radius() - kRimTickInset) };
	canvas.setColor(withAlpha(kPointer, 0.45f));
	puckDot(canvas, rim, core);
}

} // namespace feathers::ui
