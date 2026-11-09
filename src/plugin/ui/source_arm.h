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

// THE ONE WIDGET THAT ARMS A MODULATION SOURCE — the unified API for mod-assign UI widgets.
//
// **IT LIVED IN THE RACK AND FEATHERS DREW ITS OWN.** The rack built `SourceArm` frames; Feathers'
// modulator strip PAINTED chips into its own canvas and hit-tested rectangles it computed itself.
// Two implementations of "point this source at something", which is the shape this repo has paid for
// repeatedly — and it broke the moment a card grew its own frame, because a panel draws BENEATH its
// children and Feathers' painted chips vanished under the card while the rack's widgets did not.
//
// **A WIDGET IS ALSO THE ONLY VERSION THAT COMPOSES.** It sits in the card's outputs strip as a
// sibling, so it is above the card by construction rather than by anyone remembering draw order.

#pragma once

#include "device/registry.h"
#include "plugin/ui/mod_state.h"
#include "plugin/ui/theme.h"

#include <visage/ui.h>

#include <cstddef>
#include <functional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace feathers::ui {

/// Arms one modulation source. Sticky and exclusive: pressing the armed arm disarms, pressing another
/// moves the focus and stays armed, so a hand can route from one source to several controls.
class SourceArm : public visage::Frame {
public:
	SourceArm(ModState& mod, device::Source source, std::string label = {});
	~SourceArm() override;

	void draw(visage::Canvas& canvas) override;

	/// TALL ENOUGH TO CONTAIN ITS OWN ARROW. It was 14 while the glyph it draws is `kArmArrowSize + 6`
	/// = 19 — so the chip was SMALLER than the arrow inside it, and a case asking whether the chip
	/// contains the glyph's corners was right to refuse. Derived from the glyph rather than restated,
	/// so the two cannot drift.
	static constexpr float kHeight = kArmArrowSize + 6.0f;

	/// HOW FAR THE GLYPH SITS FROM A LABELLED CHIP'S LEFT EDGE. Public because a host laying the chip
	/// out needs the same number, and two opinions about it is exactly the fault this widget exists
	/// to end.
	static constexpr float kChipInset = 6.0f;

	/// HOW WIDE THIS ARM WANTS TO BE, label included. **THE NAME IS NOT DECORATION:** a device
	/// publishing two outputs draws two identical glyphs and nothing but the label says which is
	/// which — legible only while every device declared exactly one.
	float preferredWidth() const;

	/// HOW WIDE AN ARM WITH THIS LABEL WOULD BE, without one existing.
	///
	/// **A WIDTH MUST NOT REQUIRE A LAYOUT TO HAVE HAPPENED.** A host asking "how much room does this
	/// output need" may be asking BEFORE it has placed anything — that is what a layout pass is for —
	/// and an answer of zero would silently reserve nothing. Making it static keeps one computation
	/// without making the caller depend on the widget already being alive.
	static float widthFor(const std::string& label);

	/// THE WORD ON THE CHIP — what the host called this output when it last said. `setLabel` is for a
	/// host that keeps its arms across a relabel (`ChainColumn`, `RackEditor`); it repaints, and the host
	/// lays the chip out again, because a new word is a new width.
	[[nodiscard]] const std::string& label() const noexcept { return label_; }
	void setLabel(std::string label) {
		if (label != label_) {
			label_ = std::move(label);
			redraw();
		}
	}

	/// WHERE THE GLYPH ITSELF IS, in the PARENT's coordinates. The whole widget is the button — a
	/// press on the label arms too, which is what makes the word part of the control rather than a
	/// caption beside it — but a caller that needs the arrow specifically (a hit test proving a point
	/// is PAST the glyph, say) must not be handed the chip and told it is the arrow.
	visage::Bounds glyphBounds() const;

	/// **ASK FOR THE SOURCE AT USE RATHER THAN CARRYING THE ONE IT WAS BORN WITH.**
	///
	/// The constructor's `source` used to be an INDEX DERIVED FROM A LANE, and a lane is an address in a
	/// pool whose whole purpose is that the thing at it can be replaced — CLAUDE.md's ABA rule, and this
	/// widget was holding one for life. It is an ENDPOINT now (kill-lanes step 2): a device's output by
	/// the device's id, which no removal or reorder can re-point. The provider stays because an arm in
	/// Feathers' strip belongs to a SLOT, and which DEVICE stands at a slot changes when the rack is
	/// shortened or reordered — so the host hands over the way to ASK, and nothing is remembered.
	///
	/// A HOST THAT DOES NOT SET ONE KEEPS THE CONSTRUCTOR'S VALUE, which is the honest description of
	/// the rack: `RackEditor` builds an arm per device and rebuilds every arm wholesale when the chain
	/// changes, so its endpoint names the device it was built for and has nothing to ask.
	void setSourceProvider(std::function<device::Source()> provider) { source_ = std::move(provider); }

	/// **TOLD TO THE HOST BEFORE THE ARM DOES ANYTHING — the hand arrived on this chip.**
	///
	/// On the install of the strip's selection, a mod card could be highlight-outlined without really
	/// being selected, so Delete hit something else. This chip sits ON THE CARD'S FACE — it is
	/// the panel's widget, `setOnTop` of the card, and the thing a hand presses most on a modulator —
	/// and its press lit the card (armed, glowing) while telling the panel's selection nothing:
	/// `FxCard::onCardPressed` fires only for a press the CARD frame or its children take, and this
	/// is neither. The chip accepts no keystrokes either, so the focus fell to the panel and Delete
	/// took the card selected BEFORE — lit card, different card deleted. "Anywhere on their face"
	/// has to include the chip, so the host binds this to its selection. Fired before the arm's own
	/// verb so a host that redraws on selection sees the arm's state after, not before.
	std::function<void()> onPressed;

private:
	/// WHICH SOURCE THIS ARM IS FOR, RIGHT NOW. Every read goes through here — the press, the paint —
	/// so there is one answer and it is never older than the question.
	device::Source source() const { return source_ ? source_() : fixedSource_; }

	/// **THE SUBSCRIPTION, HELD** — see `ui::Watch`. Declared last so it is released FIRST.
	Watch watch_;
	ModState& mod_;
	/// What the constructor was told, used only while no provider has been set.
	device::Source fixedSource_ {};
	std::function<device::Source()> source_;
	std::string label_;
};

//------------------------------------------------------------------------------------------------//
// WHERE A DEVICE'S OUTPUT CHIPS GO — the answer all three hosts ask for.                           //
//------------------------------------------------------------------------------------------------//
//
// **EVERY OUTPUT OF A DEVICE IS ON ITS CARD, ON EVERY HOST THAT DRAWS ONE** — a design rule for
// every modulator. Three hosts draw a card — Feathers' modulator strip, Feathers' fx lane and
// the rack editor — and each had its own idea of the strip: the strip wrapped, the rack laid one
// non-wrapping row, and the fx lane drew no chips at all. A seven-output device is what made the
// difference visible (backlog FL).
//
// **THESE TWO ARE WHERE `theme`'s WALK MEETS A DEVICE.** `ui::layoutOutputChips` knows about widths
// and rectangles; `SourceArm::widthFor` knows what a chip for a NAME measures. A host has a
// descriptor and a rectangle, and should have to know neither.

/// HOW MANY ROWS THIS DEVICE'S CHIPS NEED IN A STRIP THIS WIDE — what a host reserves before it has a
/// strip to measure. Zero outputs is zero rows.
///
/// **`labels[i]`, WHERE GIVEN, IS WHAT OUTPUT `i` IS CALLED ON THIS INSTANCE** (`DeviceHost::outputLabel`)
/// and is measured instead of the declared name — a relabelled chip is as wide as its label. Outputs past
/// the end of `labels` are measured by their declared names, so an empty span is the declaration alone.
/// **`shown`: A BIT PER OUTPUT** (`DeviceHost::outputsShown`) — an output whose bit is clear takes no room and gets an
/// empty rectangle, so the chips after it close up and its arm hides (a route from it keeps reading).
size_t outputChipRowsFor(const device::Descriptor& desc, float stripWidth,
                         std::span<const std::string> labels = {}, uint64_t shown = ~uint64_t { 0 });

/// WHERE EACH OF THIS DEVICE'S CHIPS GOES INSIDE `strip`, one rectangle per declared output, in order.
///
/// **COMPUTED FROM THE DESCRIPTOR, NOT READ OFF THE WIDGETS** — `ModPanel::outputChipBounds`'s
/// contract, and it is the one that lets a host ask before it has built an arm, which is exactly when
/// a layout pass asks. `labels` as for `outputChipRowsFor`.
std::vector<visage::Bounds> outputChipBoundsFor(const device::Descriptor& desc, visage::Bounds strip,
                                                std::span<const std::string> labels = {},
                                                uint64_t shown = ~uint64_t { 0 });

} // namespace feathers::ui
