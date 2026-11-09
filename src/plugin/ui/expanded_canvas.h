/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

#pragma once

#include "plugin/ui/expanded_overlay.h"
#include "plugin/ui/frame_watchers.h"

#include <visage_ui/frame.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>

// THE CANVAS — A BAND BESIDE THE CHAIN, IN THE WINDOW THE DAW ALREADY OWNS, WHERE AN EXPANDED VIEW
// FLOATS.
//
// THE CANVAS, collapsible on the chain. The third host for an `ExpandedView`, after the overlay and
// the OS window, and the one that fails neither of the two ergonomic tests: the overlay covers things
// you might want to see, and an OS window steals focus from the DAW — which no second window of any
// kind avoids on every platform. This is neither: the EDITOR grows, the content sits in the
// new room, and keys and focus behave exactly as they do today because nothing but the size changed.
//
// THE BAND HOLDS NO PANELS — IT IS MORE OF THE SURFACE THEY FLOAT ON. Windows are not constrained to
// live only here while it is open: it is an extension of the UI where windows float over and out of
// the way of the main synth, and while it is open they still prefer to go here. Until then the band owned an `ExpandedOverlay` of its own and fenced its
// cards inside itself. Now every panel lives on the editor's one overlay (`SurfaceLayers`), which
// covers the editor AND this band; the band's floor (`contentBounds`) is only where a new panel
// PREFERS to land, handed to the overlay by the layers before each present.
//
// WHAT IT IS: a backdrop, an EDGE, a COLLAPSE in the closed-rail idiom, and a PARTITION a hand can drag.

namespace feathers::ui {

class ModState;

/// WHAT AN EMPTY CANVAS DRAWS. A seam with one implementation, NOT a setting — it must be easy to swap
/// or disable in code, not in config. A future screensaver is a different class.
class CanvasBackdrop {
public:
	virtual ~CanvasBackdrop() = default;
	/// Draw into `area`. `phase` advances once per tick while the canvas is showing, and stands still
	/// under reduced motion.
	virtual void draw(visage::Canvas& canvas, visage::Bounds area, double phase) = 0;
	/// Whether a standing canvas asks for frames. False is a backdrop that costs nothing at rest.
	virtual bool animates() const { return false; }
};

/// THE STARFIELD: the same recipe as Feathers' perform surface, rectangular rather than round.
/// `performSky()`'s 110 stars spread over the rectangle, twinkling on the perform surface's
/// own phase rule. Their colour there comes from the character field, which a canvas has no bridge to
/// sample, so the luminance is held constant — the recipe with that one input pinned.
class StarfieldBackdrop final : public CanvasBackdrop {
public:
	void draw(visage::Canvas& canvas, visage::Bounds area, double phase) override;
	bool animates() const override { return true; }
};

class ExpandedCanvas : public visage::Frame {
public:
	/// WHICH SIDE OF THE CHAIN. After an audition of the side column (2026-09-13) the layout felt wrong
	/// with the canvas opening to the side, so in both it is horizontal: Feathers' at the bottom (below
	/// Attack/Release), the rack's on top, above the macros. So Feathers' is a band along the BOTTOM under the knob tray, the rack's a band along
	/// the TOP above the macro row, and both extents are heights. `Right` stays built. Only the
	/// collapsed strip, the inboard rule and the chevrons' direction differ between edges.
	enum class Edge : uint8_t { Right, Bottom, Top };

	explicit ExpandedCanvas(Edge edge);
	~ExpandedCanvas() override;

	void draw(visage::Canvas& canvas) override;
	void resized() override;

	Edge edge() const noexcept { return edge_; }

	/// Advance the backdrop while the canvas is showing, and say whether it asked for a frame. Not a
	/// device frame — the OWLTT meter cases count those to the frame — so a host keeps it apart.
	bool animateBackdrop();

	//------------------------------------------------------------------------------------------//
	// The collapse.                                                                             //
	//------------------------------------------------------------------------------------------//

	/// FOLDED BY DEFAULT — decided after living with it: a minimised canvas is the original
	/// behaviour. A fresh patch shows the editor exactly as it was before the canvas existed, with the
	/// strip one chevron away; the floating panel over the editor stays the everyday host. On a
	/// 1920×1080 display the band can only be paid for by shrinking everything else, and that is the
	/// trade a hand should make, not a default.
	static constexpr bool kDefaultCollapsed = true;
	[[nodiscard]] bool collapsed() const noexcept { return collapsed_; }
	/// Returns whether this changed anything. COLLAPSING DISMISSES NOTHING (since 2026-10-03): no panel
	/// lives here, and the host's re-layout shrinks the overlay, which pulls every panel that was over
	/// the band back onto the instrument — and lets it return when the band opens again.
	bool setCollapsed(bool collapsed);
	bool toggleCollapsed() { return setCollapsed(!collapsed_); }
	/// A present can land here — open, not collapsed, and not hidden by a host that has no canvas.
	[[nodiscard]] bool showing() const noexcept { return !collapsed_ && isVisible(); }
	/// TOLD, NOT ASSUMED — a rail's rule (`Rail::onRailWidthChanged`): the extent is one term in an
	/// arithmetic that belongs to the editor, so the canvas announces and the editor re-derives.
	std::function<void()> onCollapsedChanged;

	/// HOW MUCH OF THE EDITOR THIS TAKES ALONG ITS DOCK AXIS — width for `Right`, height for `Bottom`
	/// and `Top`. The one number the host's layout and the window's logical size both read, so the two
	/// cannot disagree about how much room was asked for.
	[[nodiscard]] float extent() const noexcept;
	/// THE EXTENT WHEN OPEN — the partition's place. The partition moves, to give different balances,
	/// so the canvas size varies. A hand drags the grip
	/// along the inboard edge; a patch carries the number (`rack::CanvasView`). Clamped to the two
	/// constants below; `kExtent` is the default for a patch that never set one.
	[[nodiscard]] float openExtent() const noexcept { return openExtent_; }
	/// Returns whether anything changed. Tells nobody — this is the road a patch load takes, and the
	/// host is the one applying it.
	bool setOpenExtent(float extent);

	/// FIT THE OPEN BAND TO THE ROOM A WINDOW ALREADY HAS (2026-09-17: "opening/closing the canvas
	/// reproportions the window ... fits inside the original window provided its large enough"). `room`
	/// is the rows left under or over the instrument at the scale in force. With at least `kMinExtent` of
	/// them, a saved extent taller than the room shrinks to it, so the host is not asked for more; with
	/// less, nothing changes and the window has to grow. Never grows the band. Returns whether it moved.
	bool fitOpenExtentTo(float room) {
		if (room < kMinExtent || room >= openExtent_) {
			return false;
		}
		return setOpenExtent(std::floor(room));
	}
	static constexpr float kExtent = 480.0f;
	static constexpr float kMinExtent = 120.0f;
	/// THE ABSOLUTE CEILING, and `kMaxShare` the one that binds: a host caps the extent at its design
	/// size times this, so the canvas is never more than the instrument beside it. On some resize paths
	/// that compressed the synth, the canvas blew up, took over the entire view and could not shrink back
	/// to normal — the partition rule, taken to the pointer at the very
	/// top, let the canvas reach 1400 rows against 700, the scale fell to a half, and the grip became a
	/// two-pixel line nobody could take hold of again.
	static constexpr float kMaxExtent = 1400.0f;
	static constexpr float kMaxShare = 1.0f;
	/// THIS CANVAS'S CEILING, set by its host to its design size times `kMaxShare`. Every road that
	/// sets the extent — the drag, a patch load, the reset — goes through `setOpenExtent`, which clamps
	/// to it, so a patch saved past the cap by an earlier build opens at the cap.
	///
	/// A BAND ALREADY PAST THE NEW CAP IS BROUGHT DOWN TO IT, so a fresh band opens where its reset puts it
	/// (`defaultExtent`): the rack's lane is 466 rows and `kExtent` 480 (backlog LX).
	void setMaxExtent(float extent) {
		maxExtent_ = std::clamp(extent, kMinExtent, kMaxExtent);
		openExtent_ = std::min(openExtent_, maxExtent_);
	}
	[[nodiscard]] float maxExtent() const noexcept { return maxExtent_; }
	/// WHERE A FRESH BAND OPENS AND A DOUBLE-CLICK ON THE GRIP PUTS IT BACK: `kExtent`, held under this canvas's cap.
	[[nodiscard]] float defaultExtent() const noexcept { return std::min(kExtent, maxExtent_); }

	/// THE PARTITION DRAG. `onExtentDrag` fires on every step, AFTER the extent has been moved by the
	/// hand's logical delta, with the pointer's window position — so a host that knows its window can
	/// override the extent with `partitionExtent` (the partition then follows the pointer PHYSICALLY,
	/// the instrument's side scaling to fit rather than being squeezed) and re-lay out. `onExtentCommit`
	/// fires once on release, so the host asks the DAW for the size it now prefers and writes the number
	/// to the patch — one ask per gesture rather than one per pixel.
	std::function<void(visage::Point)> onExtentDrag;
	std::function<void()> onExtentCommit;

	/// **THE PARTITION FOLLOWS THE POINTER, AND THE INSTRUMENT IS NEVER SQUEEZED.** Dragged to extreme
	/// values, the partition made the knobs reposition and eventually clip off, so the synth side must not
	/// over-shrink; and in the rack, clipping vertically left no scroll. Both were the drag moving the extent inside a fixed
	/// logical size, so the other side lost rows. The rule now: the other side keeps its DESIGN size
	/// (`base` logical rows) whatever the extent, and the SCALE pays when the window cannot.
	///
	/// `nativeSpan` is the window's height in device pixels (width for a right column), `canvasNative`
	/// how many of them the pointer puts on the canvas's side of the partition, `preferredScale` the
	/// scale the editor would draw at given room. Two regimes, one answer:
	///   * SCALE-LIMITED — the window is smaller than `(base + extent) × preferred`: the whole editor
	///     draws at `nativeSpan / (base + extent)`, so the canvas's physical share is
	///     `extent / (base + extent)`, and the extent that puts the partition under the pointer is
	///     `base × canvas / (nativeSpan − canvas)`.
	///   * FREE — the window has room at the preferred scale: the layout spreads, the extent is the
	///     pointer's logical position, capped so the other side keeps `base` rows.
	/// Clamped to [`minExtent`, `maxExtent`] either way.
	static float partitionExtent(float base, float nativeSpan, float canvasNative, float preferredScale,
	                             float minExtent = kMinExtent, float maxExtent = kMaxExtent);
	/// The grip: a strip along the INBOARD edge (the edge that meets the chain), the whole length of it.
	/// TEN ROWS, not six: at the scales a big canvas drives the editor to, six rows were two pixels.
	/// A DOUBLE-CLICK ON IT PUTS THE PARTITION BACK AT THE DEFAULT — the way back to normal that has to
	/// exist whatever size the hand left it at.
	visage::Bounds gripBounds() const;
	static constexpr float kGripSize = 10.0f;
	/// WHETHER A HAND IS ON THE PARTITION. A host that syncs the frame from the patch on the beat must
	/// leave it alone while this is true: the patch holds the extent as it was at the last release, and
	/// applying it mid-drag snaps the partition back under the hand on every tick, which made compressing
	/// jumpy, erratic and hard to size down.
	[[nodiscard]] bool draggingExtent() const noexcept { return draggingExtent_; }
	[[nodiscard]] bool draggingExtentForTest() const noexcept { return draggingExtent_; }

	//------------------------------------------------------------------------------------------//
	// Geometry, in the canvas's own coordinates.                                                //
	//------------------------------------------------------------------------------------------//

	visage::Bounds toggleBounds() const;
	/// THE FLOOR — the band under its toggle row, inside its pads, where the backdrop draws and where a
	/// new panel PREFERS to land (`ExpandedOverlay::setPreferredRegion`). Nothing is clamped to it.
	visage::Bounds contentBounds() const;

	static constexpr float kTitleHeight = 28.0f;
	static constexpr float kPad = 12.0f;

	void setBackdrop(std::unique_ptr<CanvasBackdrop> backdrop);

	double backdropPhaseForTest() const noexcept { return phase_; }

private:
	void drawStrip(visage::Canvas& canvas);
	void drawChevrons(visage::Canvas& canvas) const;

	Edge edge_;
	bool collapsed_ = kDefaultCollapsed;
	float openExtent_ = kExtent;
	float maxExtent_ = kMaxExtent;
	bool draggingExtent_ = false;
	visage::Point dragOrigin_ {};
	float dragStartExtent_ = 0.0f;
	std::unique_ptr<CanvasBackdrop> backdrop_;
	double phase_ = 0.0;
	bool reducedMotion_ = false;
};

} // namespace feathers::ui
