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

#include "plugin/gui_bridge.h"
#include "plugin/ui/celestial.h"
#include "plugin/ui/notify_tray.h"
#include "plugin/ui/theme.h"
#include "plugin/ui/widget_registry.h"

#include <cstdint>
#include <optional>
#include <deque>
#include <functional>
#include <vector>
#include <visage/ui.h>

// The mock's Perform surface: the blend circle, the sounds placed in it, and the puck you play it
// with.
//
// WHAT MAKES THIS HONEST IN v1, which decides nearly every line below. The engine has exactly two
// sounds — A and B, each a zone, an offset and a gamma — and `Path` is the crossfade between them
// (`Character::xf`, synth.cpp). So the circle is not a picture of a future feature: the tour from A
// to B IS what the Path knob drives, and a puck riding that line is drawn at the mixture you are
// hearing. Three things follow, and they are the difference between this and a mock-up:
//
//   - THE SOUNDS MOVE, AND THEY ARE THE PATCH. Their places are parameters, one pair per slot, and
//     dragging a star writes them — the constellation on screen is the one the engine tours. They were
//     inert constants for the whole of v1 because nothing in the engine read a position; phase 2 reads
//     all of it, so drawing them from anywhere but the patch would be the honesty rule broken.
//   - THE PUCK IS DERIVED, NEVER STORED. It is the tour position of Path plus the residual, computed
//     at the moment it is asked, so
//     there is no second copy of the position to fall out of step with the parameter — the puck
//     cannot be right while the sound is wrong.
//   - A DRAG DECOMPOSES TO PATH PLUS THE RESIDUAL, which is the mock's own rule. v1 dropped the
//     residual because Pos X and Pos Y reached nothing, so a puck sliding off the line would have
//     shown a mixture nobody was hearing. They are live now: they move the weights and the travel, and
//     keeping the drag on the line would be the dishonest half of the same argument.
//
// WHAT IS DELIBERATELY NOT HERE: the mock's blend-space FIELD, the dim per-pixel wash of the
// character you would hear at each point, and the comet spotlight that reveals it. It needs the
// interpolated character of an arbitrary point in the disk, which is the same interpolation phase 2
// builds for the N-sound blend. Drawing an invented colormap in the meantime would be the one thing
// this surface must not do, so the sky is drawn against the mock's own neutral fallback (see
// fieldAt's note in the .cpp) and the field lights it up later without moving this code.
//
// THE COMET SPOTLIGHT COMES WITH IT, and deliberately not before (decided 2026-08-12: it waits
// for the field). The puck draws a reticle and leaves ripples, and that is all it should draw: the
// mock's own puck has no glow either, because its comet is a masked REVEAL of the field rather than
// a light of its own. With no field there is nothing to reveal, and a glow added here in the
// meantime would be a new design element wearing the spec's clothes.

namespace feathers::ui {

/// A point in the blend circle's own coordinates: the unit disk, centre at the origin, x to the
/// right and y DOWN, as in the mock and as in every screen space this editor draws into.
struct BlendPoint {
	float x = 0.0f;
	float y = 0.0f;
};

class PerformStage : public visage::Frame {
public:
	/// The clock is injectable for the same reason NotifyTray's is: ambient motion is a function of
	/// time, and a test should be able to say what time it is. Milliseconds, left empty for the real
	/// one.
	using Clock = std::function<int64_t()>;
	PerformStage(ParamBridge& bridge, WidgetRegistry& registry, Notifier* notifier = nullptr,
	             Clock clock = {});

	void draw(visage::Canvas& canvas) override;

	/// Advance ambient motion to now, and say whether the screen is out of date because of it. The
	/// same contract as NotifyTray::update, and for the same reason: the editor's frame callback is
	/// the only thing that knows time has passed.
	bool update();

	//----------//
	// Geometry //
	//----------//

	/// The blend circle: the largest disk that fits the frame, inset by the mock's 8px rim. Shared
	/// with Explore, which is drawn in the same aperture.
	StageDisk disk() const;
	float diskRadius() const;
	visage::Point diskCentre() const;

	/// Between blend coordinates and this frame's pixels. Public because every geometric test in this
	/// file is a statement about one of them.
	visage::Point toPixels(BlendPoint p) const;
	BlendPoint fromPixels(visage::Point pixels) const;

	/// Whether a point is on the blend circle — which is the control, as opposed to the much wider
	/// rectangle the circle is centred in. Public so a test can ask rather than aim.
	bool hitsDisk(visage::Point pixels) const;

	//----------//
	// The tour //
	//----------//

	/// One sound of the live constellation: where it sits, which hue it wears, and which slot it is.
	struct Star {
		BlendPoint at;
		uint32_t hue = 0;
		size_t slot = 0;
		char label = 'A';
	};

	/// THE CONSTELLATION AS THE PATCH HAS IT — every enabled slot, at its own placement, in slot
	/// order. Read from the parameters rather than declared here, which is the whole of the fix: the
	/// surface used to draw two sounds at two constants no matter what the patch said.
	///
	/// Slot order, because that is the order the ENGINE tours them in (see character_params.h). A
	/// surface that drew them in any other order would be drawing a different route from the one Path
	/// walks.
	std::vector<Star> stars() const;

	/// WHAT YOU CAN ACTUALLY HEAR OF A SOUND, as an alpha the circle draws it at: 1 ordinarily, and
	/// while a preview is latched, 1 for the sound being listened to and a fallback for the rest.
	///
	/// The circle is a picture of the blend, and a preview suspends the blend — so a Perform surface
	/// that kept drawing eight equal stars would be the surface most confidently lying about what is
	/// coming out of the plugin. See preview.h.
	float heardAlpha(size_t slot) const;
	/// The sound being previewed, as a star, even when its own switch is off — preview ignores the
	/// switch, so this is the one thing the circle draws that the patch does not contain. Nothing when
	/// no preview is latched, or when the previewed sound is already among the enabled stars.
	std::optional<Star> unpatchedPreviewStar() const;

	static constexpr float kUnheardAlpha = 0.35f;

	/// The tour, parameterised the way the Path parameter is: 0 at the first sound, 1 at the last.
	///
	/// Calls the engine's own tourPoint rather than restating it, so the dashed line on screen is the
	/// polyline Path actually walks — including its equal-parameter-per-leg rule, which is the part a
	/// second implementation would get subtly wrong.
	BlendPoint pathPos(double t) const;

	/// The Path that puts the puck as near as the tour allows to a point.
	///
	/// Projects onto every leg and keeps the best, which is what the mock's windowed search is an
	/// approximation of — with three or more anchors a free search snaps between segments, and legs
	/// are the exact version of the window that protects.
	double nearestPathT(BlendPoint p) const;

	/// Where the puck is: the tour position of the Path the ENGINE is running on, plus the Pos X/Y
	/// residual it is standing off the tour by.
	///
	/// THE RESIDUAL IS DRAWN NOW, and that is a consequence of the engine rather than a change of
	/// mind. v1 dropped it because Pos X/Y reached nothing — a puck sliding off the line would have
	/// been showing a mixture nobody was hearing. They are live in phase 2: they move the puck, which
	/// moves the weights and the travel. Dropping the residual would now be the dishonest choice.
	BlendPoint puck() const;

	/// Where the hand left it: the same position from the base values alone. Equal to puck() unless
	/// the host is modulating, which is exactly when the mock's two-state display appears.
	BlendPoint puckBase() const;

	/// Whether host modulation is currently moving Path far enough to be worth drawing as two states.
	bool showsModulation() const;

	//----------------------------------//
	// Which colour a mark is drawn in   //
	//----------------------------------//

	/// THE MARK THAT MOVES, and the mark that stays, are different colours — R3: a moving star in the
	/// 'modulated glow' colour in addition to the fixed star.
	///
	/// The puck is the only mark that rule can honestly be about. The destination list is the mock's
	/// six, `['path','posx','posy','level','glide','fine']`, plus whatever the fx lane's tenants
	/// declare — and NOTHING in either half is a sound's identity or its placement, so no star on this
	/// circle can move under a modulator. The puck can, and does: Path, Pos X and Pos Y are three of
	/// the six. A tenant's params live downstream of the blend entirely, so widening the list that way
	/// leaves this claim where it was.
	///
	/// TWO FUNCTIONS RATHER THAN A COLOUR AT EACH SITE, because the pair is the point. If both marks
	/// went periwinkle the two-state display would stop distinguishing the two states, which is the
	/// whole of what it is for — the moving star is IN ADDITION TO the fixed one, not
	/// instead of it. Stated here as one definition each so a later edit cannot drift them together.
	static constexpr uint32_t movingMarkColour(bool modulated) {
		// Conditional rather than permanent: periwinkle is modulation and nothing else in this editor,
		// and a puck nobody is modulating is just the puck.
		return modulated ? kModulation : kAccent;
	}

	/// Where the hand left it. Never modulation's colour, in either state.
	static uint32_t authoredMarkColour() { return kAccent; }

	//--------//
	// Motion //
	//--------//

	/// The mock's `prefers-reduced-motion`, which a plugin window has no way to be asked about — so it
	/// is an environment variable, read once when the editor's stage is built.
	///
	/// The contract is the mock's, exactly and no wider: no ambient repaint while idle, and no
	/// modulation trail. Everything else still moves, because everything else moves only when the
	/// user or the host moves it. (The wake ripples disappear too, and that is the same rule rather
	/// than a fourth one: they age on the shimmer clock, and the shimmer clock is what stops.)
	static bool reducedMotionRequested();
	bool reducedMotion() const noexcept { return reduced_; }

	/// The ambient clock, in seconds. Frozen under reduced motion, and holdable at a chosen value with
	/// FEATHERS_SHIMMER_AT so a particular moment of the breathing can be captured deliberately. Readable so a test can watch it
	/// rather than watch pixels.
	double shimmer() const noexcept { return shimmer_; }

	/// How many points of modulation trail are being drawn.
	size_t trailLength() const noexcept { return trail_.size(); }

	/// How many wake ripples are alive. Expired ones are dropped as they expire, so this counts what
	/// is on screen rather than what has ever been left behind.
	size_t rippleCount() const noexcept { return ripples_.size(); }

	/// THE APERTURE CONTAINS THE PUCK, overriding A30: the puck is clipped.
	///
	/// A30 measured the geometry and judged the spill cosmetic and honest — the puck outside the disk is
	/// showing a residual pushed past the edge of the blend space, and clipping it hides where the hand
	/// is. Watching it in use settled it the other way:
	/// the mock clips everything to the circle (the sky already does, and so do the ripples), and a
	/// reticle drawn over the sound cards reads as a bug however true it is.
	///
	/// So every mark the puck makes is clipped to the disk, and this is where the last frame's ink went
	/// — the same recorder Explore uses, for the same reason: the bound has to be checked at the site
	/// that submits the geometry.
	InkBox puckInk() const noexcept { return puckInk_; }
	/// The box those marks are allowed to fall inside: the disk's own square.
	InkBox diskSquare() const;
	/// How far past the rim a glyph is allowed to bleed, for the same reason Explore allows it: a mark
	/// sitting ON the rim spreads its own size over the rim, which is a glyph having a size rather than
	/// a mark in the wrong place.
	static constexpr float kGlyphBleed = 6.0f;

	/// The mock's own numbers, kept where they can be read against it.
	static constexpr size_t kTrailLength = 46; ///< `if(trail.length>46)trail.shift()`
	static constexpr size_t kMaxRipples = 8;
	static constexpr double kRippleLife = 1.4;    ///< seconds, `age/1.4`
	static constexpr double kRippleSpacing = 0.14; ///< seconds between wake rings
	static constexpr float kRippleTrigger = 0.004f; ///< how far the puck must move to leave one
	/// The mock's frame and rim, which live on the disk both surfaces share.
	static constexpr float kMockSize = StageDisk::kMockSize;
	static constexpr float kRimInset = StageDisk::kRimInset;

private:
	/// **THE SUBSCRIPTIONS, HELD** — see `ui::Watch`. Declared last so they are released FIRST.
	std::vector<Watch> watches_;
	clap_id pathId() const noexcept;
	double pathValue() const;     ///< base
	double pathModulated() const; ///< base + host modulation

	/// Which star is under a point, or none. The grab radius is generous against the glyph, because a
	/// star is small on purpose and a control you have to hit exactly is a control you fight.
	static constexpr float kStarGrabPixels = 16.0f;
	std::optional<size_t> starAt(visage::Point pixels) const;

	/// Move the puck under a point: Path onto the tour, and Pos X/Y for the rest of the way.
	void dragTo(visage::Point pixels);

	/// Move one sound's placement under a point, which is how a constellation gets arranged by hand.
	void dragStarTo(size_t slot, visage::Point pixels);
	/// Write one parameter as part of the drag's single bracketed gesture. No-ops when the value has
	/// not moved, so a click that lands where the puck already is spends no undo step.
	void commit(clap_id id, double value);
	void endGesture();

	void drawSky(visage::Canvas& canvas) const;
	void drawSounds(visage::Canvas& canvas) const;
	void drawTour(visage::Canvas& canvas, float scale) const;
	void drawWake(visage::Canvas& canvas) const;
	void drawTrail(visage::Canvas& canvas) const;
	/// Not const, and that is the point: it is the one drawing pass that records where its ink went.
	void drawReticle(visage::Canvas& canvas, float scale);
	void drawLabel(visage::Canvas& canvas) const;

	/// THE THREE DOORS EVERY PUCK MARK GOES THROUGH — clipped to the disk, drawn only if any of it is
	/// inside, and recorded. One door per shape, so a mark added later cannot miss either half.
	void puckLine(visage::Canvas& canvas, visage::Point from, visage::Point to, float thickness);
	/// An arc of a ring centred on `at`. Both the reticle's four gapped arcs and the base ring's
	/// dashes are arcs, so they clip through one function rather than two.
	void puckArc(visage::Canvas& canvas, visage::Point at, float radius, float middle, float half,
	             float thickness);
	/// A filled dot draws whole or not at all: its centre is either inside the aperture or it is not,
	/// and a couple of pixels of its own size over the rim is what kGlyphBleed is for.
	void puckDot(visage::Canvas& canvas, visage::Point at, float radius);

	struct Ripple {
		BlendPoint at;
		double born = 0.0;
	};

	ParamBridge& bridge_;
	Notifier* notifier_ = nullptr;
	Clock clock_;
	bool reduced_ = false;

	double shimmer_ = 0.0;
	bool pinnedShimmer_ = false; ///< FEATHERS_SHIMMER_AT holds the ambient clock for a capture
	int64_t lastTick_ = 0;
	bool everTicked_ = false;

	std::deque<BlendPoint> trail_;
	std::vector<Ripple> ripples_;
	BlendPoint lastPuck_ {};
	InkBox puckInk_;
	/// The mock's `lastRip=0`, against a shimmer that also starts at 0 — so the first 140ms of an
	/// editor's life leave no wake, however far the puck moves in them. A large negative here would
	/// have been a ripple on the opening frame, which is not what the surface does.
	double lastRipple_ = 0.0;

	bool dragging_ = false;
	/// Which sound the hand has hold of, if it grabbed a star rather than the puck. A drag is one or
	/// the other for its whole life: deciding again mid-drag would let a star that passed under the
	/// pointer steal a gesture aimed at the puck.
	std::optional<size_t> draggingStar_;

	/// HOW FAR THE STAR IS FROM THE POINTER WHEN IT IS GRABBED, so a drag moves it BY the hand rather
	/// than TO the hand.
	///
	/// Clicking and holding a star grabs it. `starAt` takes a press
	/// anywhere within `kStarGrabPixels`, and without this the press itself placed the sound at the
	/// pointer — measured at 15 px of movement, and two full host gesture brackets, from a press with
	/// no drag at all. A sound's place is patch state, so that was an undo step in the host for a
	/// gesture nobody made.
	///
	/// THE PUCK HAS NO SUCH FIELD AND MUST NOT GROW ONE. Click-to-position is what the puck is for,
	/// which the press handler says in as many words; this decision was about the star.
	visage::Point starGrabOffset_ {};
	/// Which parameters this drag has opened a gesture on. A drag moves up to three, and each one has
	/// to be closed on the same ids it was opened on — a host given an unmatched begin holds an undo
	/// step open forever.
	std::vector<clap_id> gesturesOpen_;
	/// What each of those read when its gesture opened, beside it — the undo step's "before" (backlog JB).
	std::vector<double> valuesBefore_;
};

} // namespace feathers::ui
