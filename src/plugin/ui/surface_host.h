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

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace visage {
class Frame;
}

// A SURFACE IS SOMETHING THE HOST DRAWS ON A GUEST'S BEHALF — a browser, an expanded view, a picker.
//
// WHY A GUEST CANNOT JUST DRAW ONE. The web named this: `<dialog>.showModal()` and the Popover API
// both promote an element to a **top layer** because a panel inside a clipping, scrolling container
// cannot escape it — `z-index: 9999` inside `overflow: hidden` still clips, and there is no value
// settable from the inside that helps. A rack nested in Feathers is exactly that container. **Only
// the outermost owner can grant a top layer**, so asking is the only road that survives nesting.
//
// AND WHAT `present` IS, ALSO NAMED: a PORTAL. React's `createPortal` renders a child into a node
// outside its parent's hierarchy while it stays inside the component tree, so its state, context and
// events behave as though it were where it logically belongs. That is this exactly — the browser is
// the guest's (its catalogue, its callbacks, its `ModState`) and is painted where the guest does not
// own. `expanded_overlay.h`'s *"arming is STATE, not geometry"* is the same insight reached from the
// other end, which is the evidence it is right.
//
// THE FOUR KINDS ARE ORDERED BY HOW FAR THE CONTENT IS FROM THE EDITOR, and the order is not
// cosmetic — see `SurfaceNeed`.

namespace feathers::ui {

/// HOW A SURFACE IS PRESENTED. `Inline` is the guest drawing it itself in the space it already has,
/// and is therefore also the answer meaning **declined**.
enum class SurfaceKind : uint8_t {
	Inline,   ///< The host will not present. The guest falls back to its own in-place affordance.
	Takeover, ///< Same window, replacing the editor. Costs no resize and no event loop.
	/// DOCKED BESIDE THE CHAIN, in room the editor asks the host for — neither replacing the editor
	/// nor floating over it, which is why it sits between those two here. The editor stays visible and
	/// reachable, so it serves a spanning gesture; and keys and focus behave exactly as the editor's
	/// do, which no second window of any kind can promise on every platform. Offered only while the host's canvas is SHOWING; a
	/// collapsed canvas hands the gesture to the overlay. Since 2026-10-03 the panel is on the
	/// same overlay either way, ARRIVING in the band and free to float anywhere over band and editor.
	Canvas,
	Overlay,  ///< Floating above the editor, which stays visible and reachable beneath.
	Window,   ///< A separate top-level window — `window.open()`, with its own event loop.
};

/// WHAT THE CONTENT REQUIRES, which is the thing the GUEST knows and the host does not.
///
/// THE INVERSION IS DELIBERATE AND WAS A CORRECTION. The first draft had the guest name a KIND and
/// ask whether it could have it — but then the guest is choosing on window size, which is the one
/// input that does not determine the answer. Material Design has the shape right: you declare
/// "dialog" and the framework presents it as a floating panel or a full-screen view depending on the
/// surface. **The guest owns the requirement; the host owns the policy.**
enum class SurfaceNeed : uint8_t {
	/// OPENS, RETURNS A VALUE, CLOSES. Nothing outside it is needed while it is up, so any kind works
	/// and the cheapest available one is right.
	SelfContained,

	/// THE EDITOR BENEATH MUST STAY REACHABLE — `Overlay` or `Window` only, NEVER `Takeover`.
	///
	/// THIS IS A CORRECTNESS CONSTRAINT AND IT COST A SHIPPED BUG. `expanded_overlay.h` records it:
	/// *"ROUTING MODULATION IS A GESTURE THAT SPANS SURFACES: arm a source in the mod panel, then
	/// click a destination on this one... The dismissal and the arming want the same press, and only
	/// one of them can have it."* The resolution was structural — the scrim draws and takes nothing,
	/// so a press anywhere else reaches the editor BENEATH and arms a source there.
	///
	/// **A TAKEOVER HAS NO BENEATH.** The mod panel is not dimmed, it is gone.
	///
	/// AND `expanded_overlay`'s "arming is state, not geometry" DOES NOT SAVE THIS, which is the
	/// subtle half: the armed source genuinely survives a takeover, because it is a flag in
	/// `ModState` and nothing about presentation touches it. What does not survive is the player's
	/// ability to SEE AND CLICK the destination. So the two kinds differ by whether the editor stays
	/// REACHABLE, and not by how much room there is — the constraint holds at every window size.
	SpansGesture,
};

/// A LIVE SURFACE. Monotonic and NEVER REUSED.
///
/// NOT AN INDEX AND NOT A POINTER, under this repo's own rule: *"an address is only a valid identity
/// while the thing at it cannot be replaced"*. A stack pops and pushes, so a depth index is an ABA
/// bug waiting for the second guest — dismiss(2) landing on whatever occupies level 2 now.
using SurfaceId = uint64_t;
inline constexpr SurfaceId kNoSurface = 0;

/// THE HOST'S SIDE OF THE SEAM. Feathers implements it; the rack implements it; a rack inside a rack
/// forwards to the one above and never learns how deep it is.
class SurfaceHost {
public:
	virtual ~SurfaceHost() = default;

	/// PRESENT CONTENT, or decline. Returns `kNoSurface` when declined, and the guest must then draw
	/// its own in-place affordance — declining is a first-class answer, not an error.
	///
	/// **NON-VIRTUAL, AND THAT IS THE POINT.** `SpansGesture` must never be answered with a takeover
	/// (see the enum), and a rule every implementor has to remember is a rule that gets forgotten by
	/// the third host. So the check lives here, once: a host's `presentImpl` is never called with a
	/// need it cannot satisfy, and no host can get this wrong even deliberately. `DeviceHost::mayRoute`
	/// is non-virtual for the same reason.
	///
	/// OWNERSHIP TRANSFERS, matching `ExpandedOverlay::show`. A borrowed frame would put a dangling
	/// pointer one early return away from a guest that is destroyed while presented.
	SurfaceId present(std::unique_ptr<visage::Frame> content, SurfaceNeed need, std::string title,
	                  std::function<void()> onDismiss) {
		if (content == nullptr || kindFor(need) == SurfaceKind::Inline) {
			return kNoSurface;
		}
		return presentImpl(std::move(content), need, std::move(title), std::move(onDismiss));
	}

	/// WHAT THIS HOST WOULD DO WITH SUCH CONTENT, with the correctness constraint already applied.
	/// A guest asks this to decide what to BUILD, and gets the same answer `present` will act on.
	SurfaceKind kindFor(SurfaceNeed need) const {
		const SurfaceKind kind = kindAvailable(need);
		// A HOST THAT OFFERS A TAKEOVER FOR A SPANNING GESTURE IS ANSWERING WRONG, and the safe
		// reading of a wrong answer is "cannot" rather than "will, badly": Inline keeps the editor
		// on screen, which is the whole thing the need was asking for.
		if (need == SurfaceNeed::SpansGesture && kind == SurfaceKind::Takeover) {
			return SurfaceKind::Inline;
		}
		return kind;
	}

	/// TAKE DOWN A SURFACE. Unknown or stale ids are ignored, so a guest holding an id across a
	/// dismissal it did not cause is safe rather than lucky.
	virtual void dismiss(SurfaceId id) = 0;

protected:
	/// WHAT THIS HOST CAN DO, before the contract's constraint is applied. Implementors answer for
	/// their own policy — window size, nesting depth, whether they do windows at all — and do not
	/// repeat the `SpansGesture` rule, which `kindFor` has already enforced above them.
	virtual SurfaceKind kindAvailable(SurfaceNeed need) const = 0;

	virtual SurfaceId presentImpl(std::unique_ptr<visage::Frame> content, SurfaceNeed need,
	                              std::string title, std::function<void()> onDismiss) = 0;
};

} // namespace feathers::ui
