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

#include "plugin/ui/slot_edit.h"
#include "plugin/gui_bridge.h"
#include "plugin/ui/drop_gap.h"
#include "plugin/ui/edit_history.h"
#include "plugin/ui/notify_tray.h"
#include "plugin/ui/sound_identity.h"
#include "plugin/ui/ui_state.h"
#include "plugin/ui/widget_registry.h"
#include "plugin/ui/fader_pill.h"
#include "plugin/ui/param_control.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>
#include <visage/ui.h>

// The mock's `#poolList`: the constellation as a list, one card per sound.
//
// WHAT IT IS FOR. The circle says where a sound is; this says what a sound IS. Every card carries the
// sound's own identity — the zone, offset and gamma that decide its character — and its placement,
// which is the other half of the two-level model the design is built on (a sound is an identity, an
// anchor is a placement). Clicking a card SELECTS it, and the selection is the context Explore edits,
// which is the reason this exists before Explore does.
//
// WHY IT IS A ONE-LEVEL TREE, and this is a narrowing of the mock rather than a simplification of it.
// The mock's tree has children: a sound may hold several stored placements with exactly one active,
// so its rows are `sound → variations`. The engine has ONE placement per sound — `PlaceX`, `PlaceY`,
// and nothing that could hold a second — so a variation list here would be a control with no state
// behind it. The placement is therefore drawn as the mock draws its ACTIVE child, in the same
// indented 9px row, and the level appears the day the engine has variations to put in it.
//
// EVERY ROW IS REAL. The dot is the hue the Perform circle draws that sound's star in; the switch is
// `Enable`, the one control here that writes a parameter; the readouts are `valueToText` on the
// parameter table, so a card and the host's own display of the same parameter cannot disagree about
// what it says. Nothing on a card is a label over an empty value.
//
// IT SCROLLS. Eight sounds at this card height overflow the rail on a short editor, and the answer to
// that is never to make the window taller — the editor's logical size is fixed and everything else
// fits inside it. Content taller than the rail scrolls under the wheel and is clipped by the frame.

namespace feathers::ui {

class SoundStack : public visage::Frame {
public:
	/// `previewWatchers` is who repaints when the preview moves. REQUIRED, not defaulted: preview is
	/// not a parameter, so the registry cannot redraw anybody for it, and the surfaces that draw what
	/// is audible — the blend circle, the scope — only repaint when told. A caller that forgot would
	/// get a working preview whose picture went stale, which is the exact failure this feature is not
	/// allowed to have.
	/// `history` is the editor's one undo stack — see edit_history.h. Required, because the verb this
	/// widget owns is the one that made the history shared: a clone writes a whole sound into a slot
	/// and switches it on, and an editor where that cannot be walked back is an editor where the
	/// cheapest way to lose a sound is to press the button next to it.
	SoundStack(ParamBridge& bridge, WidgetRegistry& registry, SoundSelection& selection,
	           EditHistory& history, FrameWatchers& previewWatchers, Notifier* notifier = nullptr);

	void draw(visage::Canvas& canvas) override;
	bool mouseWheel(const visage::MouseEvent& e) override;

	/// The rail's width in logical pixels, from the mock's own grid: `grid-template-columns: auto auto
	/// 206px minmax(0,1fr) auto`, where the 206 is this column. One of the few widths the mock states
	/// outright rather than leaving to its contents.
	static constexpr float kWidth = 206.0f;

	/// **A DRUM KIT'S CARDS ARE ITS PADS**: all eight present, no add, clone or delete (hidden),
	/// the switch is the pad's Mute, the S its solo, the second line its kind and note, a Level fader its pad's Level, and
	/// the card pulses when its pad is struck.
	[[nodiscard]] bool kitMode() const;
	[[nodiscard]] bool padMuted(size_t slot) const;
	void togglePadMute(size_t slot);
	/// Read the published hits, start a pulse on each pad whose count moved, and answer whether anything is lit.
	bool updatePulses(int64_t nowMs);
	/// Seat each kit card's Level fader on its card (and hide them all off a kit).
	void layoutLevelFaders();
	/// How lit card `slot` is now, 0..1 (its last hit's velocity, falling over kPulseMs; held flat under reduced motion).
	[[nodiscard]] float pulseOf(size_t slot) const;
	static constexpr float kPulseMs = 150.0f;
	/// The pad Level fader on card `slot` (Kit mode), for a case to drive.
	[[nodiscard]] FaderPill* levelFaderForTest(size_t slot) const { return levelFaders_[slot].get(); }

	//----------//
	// Geometry //
	//----------//

	/// What a click at a point would hit. Public so a test can ask what is under a coordinate rather
	/// than click somewhere and assert that something happened.
	/// A press lands on a sound (the card and its verbs) or on one of that sound's remembered stances
	/// (a variation row and its two). `index` means anything only for the three variation parts.
	enum class Part {
		None,
		/// The "+ New sound" row.
		NewSound,
		Card,
		Enable,
		Clone,
		Preview,
		/// Free the slot back to empty. The one destructive verb on the card.
		Delete,
		/// The row body: recall this stance, which is the mock's radio light.
		Variation,
		/// Remember where the sound is standing now, as a new stance beside this one.
		VariationStore,
		/// Forget this one. Never the last: a sound is never unplaced.
		VariationForget,
	};
	struct Hit {
		Part part = Part::None;
		size_t slot = 0;
		size_t index = 0;
	};
	Hit hitAt(visage::Point position) const;

	/// THE MOCK'S ⧉, opened at last. A4 left clone out on the grounds that eight fixed slots have
	/// nothing to create or destroy — which is true of the SLOTS and false of the SOUNDS, and the
	/// sounds are what was missing: the tree view had no clone. It
	/// copies a sound's whole identity into the first slot that is switched off, and switches that slot
	/// on, because a clone you cannot hear is not a clone.
	///
	/// WHAT TRAVELS AND WHAT DOES NOT. Everything in `SoundIdentity` — zone, offset, gamma, the drift
	/// out along the plane, the plane itself, and what its rotations banked — because that is what the
	/// sound IS, and because leaving any of it behind is the bug this project has already had twice
	/// (A5's drift, A26's residue). `PlaceX`/`PlaceY` do NOT travel: where a sound sits in the blend
	/// circle is where you play it FROM, and identity-versus-performance is the line this whole arc is
	/// drawn along. The clone therefore arrives sounding like its source and standing in its own slot's
	/// place — which also makes the pair audibly and visibly separable the moment it exists, where two
	/// stars on one anchor would be a zero-length leg of the tour and two glyphs nobody could tell
	/// apart.
	///
	/// WHETHER THERE IS ROOM AT ALL, and which empty slot the next clone would spend. Readable so the
	/// button can say whether it will work before it is pressed, rather than being a control that
	/// sometimes does nothing for reasons the surface knows and the hand does not. It is NOT where the
	/// copy appears — see `cloneDestination`, and the note on `clone` for why the two came apart.
	std::optional<size_t> cloneTarget() const;
	/// WHERE A COPY OF `slot` WOULD APPEAR: immediately after its source in the list. Nothing when the
	/// rail is full.
	std::optional<size_t> cloneDestination(size_t slot) const;
	/// Copy `slot`'s identity and switch it on, IMMEDIATELY AFTER ITS SOURCE — by design, and the
	/// end of a clone that arrived at the far end of a list of eight.
	///
	/// IT IS A SPLICE, NOT A FILL, because slot order IS list order IS the Path's tour order: a copy
	/// that landed in whatever slot happened to be free would be a copy the tour visits somewhere else
	/// entirely, which is the one thing a clone must not be. So the empty slot nearest the source is
	/// spent, every sound between it and the source shifts one place to make the gap fall where it is
	/// wanted, and the copy is written into that gap. Sounds that shift move WHOLE — identity, switch,
	/// stances and placement — because they are existing sounds changing slots rather than new ones
	/// being made, and a shift that left a placement behind would teleport half the rail.
	///
	/// The letters re-badge, because a letter is the name of a SLOT: a clone of D lands as E and what
	/// was E becomes F. One undo step puts every slot the splice touched back.
	void clone(size_t slot);

	//----------//
	// Reorder  //
	//----------//

	/// MOVE A SOUND TO ANOTHER PLACE IN THE LIST — the mock's draggable rows, and the affordance whose
	/// absence changed what the instrument could be played into: `tourPoint` walks the live anchors in
	/// SLOT order, so the order of this list is the order the Path knob sweeps.
	///
	/// MOVE-TO-INDEX, NOT SWAP — the mock's own `reorderSound()` splices, and a swap is a different
	/// verb that happens to agree with it only for adjacent rows. Everything between the two places
	/// shifts by one, empty slots included: an empty slot shifted is still empty and still hidden, and
	/// carrying them along is what makes the visible order come out as a plain splice without this
	/// having to know where the gaps are.
	///
	/// NO ENGINE CHANGE, and that is the point of doing it this way. The engine tours enabled slots in
	/// slot order and goes on doing exactly that; moving the CONTENTS through the parameter road
	/// reorders the tour as a consequence rather than as a second mechanism that could disagree with
	/// it.
	void reorder(size_t from, size_t to);

	/// WHAT THE HAND IS HOLDING, and WHICH GAP it would drop into. Both empty unless a drag is in
	/// flight. Readable because a gesture that has no state a test can read can only be tested by its
	/// result.
	///
	/// THE SECOND ONE USED TO BE A SLOT AND THAT WAS THE BUG. The destination highlighted a
	/// different sound — it should instead highlight the bumper before a sound where it actually
	/// goes."* A row index differs from where the sound lands by one at every position, and cannot
	/// express "before the first" or "after the last" at all. It is a GAP now, 0..visibleCount()
	/// inclusive — see drop_gap.h.
	std::optional<size_t> dragSlot() const noexcept { return dragging_ ? armed_ : std::nullopt; }
	std::optional<size_t> dropGap() const noexcept { return dragging_ ? drop_ : std::nullopt; }

	/// WHERE THE BUMPER IS DRAWN for a gap, in the rail's own coordinates — a thin horizontal band
	/// between two cards, above the first, or below the last. Public so a case can ask the geometry
	/// rather than photograph it.
	visage::Bounds gapBounds(size_t gap) const;

	/// How far the pointer must travel before a press on a card becomes a drag rather than a click.
	/// Small, because the rows are 38px tall and the verbs on them are 9px: a threshold big enough to
	/// be comfortable would be a threshold that swallows the click.
	static constexpr float kDragThreshold = 4.0f;

	//---------------------------//
	// Which slots hold a sound   //
	//---------------------------//

	/// WHETHER A SLOT HAS BEEN CONSUMED — clone was confusing when all 8 sounds were pre-seeded, so
	/// slots stay empty until they are consumed.
	///
	/// EMPTY IS DERIVED, NOT STORED. A slot is empty when nothing about it has been authored: it is
	/// switched off, its identity is what the parameter table declares, its placement is the default
	/// one its slot was born with, and it remembers no stances. Nothing new goes in the patch, nothing
	/// migrates, and an existing eight-slot patch reads as "the sounds somebody made, plus room" the
	/// first time it is opened — which is the intended state rather than a conversion of it.
	///
	/// A PARKED SOUND IS NOT EMPTY. Switching a sound off leaves its identity in the slot, so it stays
	/// in the rail and a clone will not land on top of it. That is the whole of the confusing case: a
	/// clone now always goes somewhere nothing was, and "cloned over" cannot happen.
	///
	/// EMPTINESS IS PRESENTATION, said plainly because the alternative reading would be wrong: the
	/// engine still has eight slots, the host still has eight sets of automation lanes, and a sound
	/// still sounds only when it is enabled. This decides what the rail DRAWS and where a clone LANDS.
	bool isEmpty(size_t slot) const;
	/// The slots the rail draws, in slot order — the consumed ones. Public because "what is on screen"
	/// is the claim every geometry case here needs to make.
	size_t visibleCount() const;
	size_t visibleSlot(size_t position) const;

	/// MINT A SOUND IN THE FIRST EMPTY SLOT — the mock's "+ New sound" (its `addBtn`), which is how a
	/// sound comes into being when there is no original to copy. Rolled zone and offset, placed at the
	/// emptiest part of the circle, switched on and selected, one undo step. Does nothing when all
	/// eight are consumed, and says so.
	void newSound();
	/// Where a new sound would land, or nothing when the rail is full. Readable so the affordance can
	/// say whether it will work before it is pressed.
	std::optional<size_t> newSoundTarget() const;

	/// DELETE — free a slot back to EMPTY, which is the way back the empty-until-consumed rail
	/// otherwise lacks: switching a sound off leaves it consumed (that is what protects a parked sound
	/// from a clone), so without this verb a patch that reached eight sounds could never make room
	/// again except by undo. The design round hit exactly that wall, and the verb was added.
	///
	/// DELETED IS NEVER-USED — ONE STATE, NOT TWO THAT LOOK ALIKE. Deleting writes the parameter
	/// table's own defaults for the slot: the same values `isEmpty` reads. There is no tombstone and
	/// no third state; the slot after a delete is bit-for-bit the slot nobody ever touched, so the
	/// "+ New sound" row returns by the same derivation that hid it.
	///
	/// ONE UNDO STEP RESTORES EVERYTHING — identity, switch, stances, placement — because the record
	/// is the whole `SlotState`, which was grown for exactly this class of verb. And it is the ONE
	/// verb allowed to touch a consumed slot; a clone still cannot, and only an explicit press lands
	/// here. Deleting the last sound is legal: an empty patch is a valid patch, and the engine blends
	/// an empty constellation to silence by design (character_bank.cpp) — asserted on rendered audio,
	/// not on that comment, by the zero-sounds case.
	void deleteSound(size_t slot);

	/// PREVIEW — listen to one sound on its own, with the puck wherever it happens to be. See
	/// preview.h for what it does to the character and why it is not a parameter.
	///
	/// LATCHED, AND IT FOLLOWS THE SELECTION. Momentary was the alternative and it does
	/// not survive the use it exists for: you preview a sound in order to EDIT it, both hands' worth of
	/// attention is on the Explore chart being dragged, and a button that has to be held is a button
	/// that cannot be held while you work. A key would have been the other way to hold one, and this
	/// editor takes the keyboard only while a name is being typed (keyboard_claim.h) — at rest it holds
	/// nothing, so the DAW keeps its bindings, and CLAP has no key-focus extension to ask a host through.
	///
	/// Pressing it on a card both SELECTS that sound and listens to it, because those are one
	/// intention. Pressing it again on the sound being previewed turns it off.
	void togglePreview(size_t slot);
	/// Which sound is being previewed, straight off the bridge — so the rail cannot draw a preview
	/// state that differs from the one the engine is playing.
	bool previewing(size_t slot) const;
	bool anyPreviewing() const;

	//------------//
	// Variations //
	//------------//

	/// THE MOCK'S CHILD ROWS AT LAST — the subsounds. See sound_variations.h for what one is and
	/// for V19's write-through model: the ACTIVE stance is the live record — edits mutate it, the
	/// banked siblings stand, and a recall switches which record you occupy.
	///
	/// HOW MANY ROWS A SOUND DRAWS: one per stored stance, or ONE for a sound that has stored none,
	/// which is the implicit record its live explore state holds (V20). So the rail looks exactly as
	/// it did until somebody stores something, and the level appears where the mock always drew it.
	size_t variationRows(size_t slot) const;
	/// The stance's stored ride-along values — the whole explore box since V20 — with the
	/// empty-list rule: a sound that has stored nothing has the one implicit record its live state
	/// holds, and the ACTIVE index reads the live values (V19's mirror).
	RideAlongValues variationValuesAt(size_t slot, size_t index) const;
	/// Where that stance lives on the Explore axis — the row's label, in home units: its stored
	/// zone, offset and gamma base plus the live vernier and global halves the box does not carry.
	double variationHome(size_t slot, size_t index) const;
	/// The live slot's ride-along values, off the parameters — the family's value half.
	RideAlongValues liveRideAlong(size_t slot) const;
	/// WHETHER THAT ROW IS THE ACTIVE STANCE — the record your edits are mutating (V19: stances are
	/// write-through, the mock's model; there is no stepped-off state for a comparison to be honest
	/// about). For a sound that has stored nothing, the one drawn row is the implicit active record
	/// its parameters hold.
	bool activeStance(size_t slot, size_t index) const;

	/// Switch to a stance: the outgoing active record BANKS the whole live box, and the incoming
	/// one's stored box — zone, offset, gamma base, drift, plane with residue, width — comes into
	/// the parameters and state channels. It NEVER writes place (V20: "place leaves the stance" —
	/// the star stands while the sound travels), and it DOES move the identity by design: a
	/// variation is a remembered exploration of the sound, and switching to one goes there. One
	/// undo step, chart following both directions.
	void activateVariation(size_t slot, size_t index);
	/// Remember the sound's current exploration as a new stance. The mock's "clone variation": it
	/// banks the current record and makes the copy live.
	void storeVariationAt(size_t slot);
	/// Forget one. Refuses the last, and says why.
	void forgetVariationAt(size_t slot, size_t index);

	/// Where a variation row and its verbs are, so a case can press one rather than clicking somewhere
	/// and hoping. Public for the reason hitAt is: a test that cannot ask where a control is can only
	/// assert that something happened.
	float slotHeightForTest(size_t slot) const { return slotHeight(slot); }
	visage::Bounds cardBoundsForTest(size_t slot) const { return cardBounds(slot); }
	visage::Bounds switchBoundsForTest(size_t slot) const { return switchBounds(slot); }
	visage::Bounds cloneBoundsForTest(size_t slot) const { return cloneBounds(slot); }
	visage::Bounds previewBoundsForTest(size_t slot) const { return previewBounds(slot); }
	visage::Bounds deleteBoundsForTest(size_t slot) const { return deleteBounds(slot); }
	visage::Bounds newRowBoundsForTest() const { return newRowBounds(); }
	visage::Bounds variationBoundsForTest(size_t slot, size_t index) const {
		return variationBounds(slot, index);
	}
	visage::Bounds variationStoreBoundsForTest(size_t slot, size_t index) const {
		return variationStoreBounds(slot, index);
	}
	visage::Bounds variationForgetBoundsForTest(size_t slot, size_t index) const {
		return variationForgetBounds(slot, index);
	}
	bool slotOnScreenForTest(size_t slot) const { return slotOnScreen(slot); }
	uint32_t seedHueForTest(size_t slot) const { return seedHue(slot); }

	/// How tall the whole list is, and how far it is scrolled. Both readable because the scroll
	/// contract — never taller than the rail, never scrolled past the end — is a statement about these
	/// two numbers and nothing else.
	float contentHeight() const;
	float scroll() const noexcept { return scroll_; }
	float maxScroll() const;
	void setScroll(float y);

	/// The "+ New sound" row under the last card — the mock's own affordance, in the rail rather than
	/// in a header because that is where the mock puts it and where the thing it creates appears.
	static constexpr float kNewRowHeight = 20.0f;

	/// The mock's `.seed` card and its indented placement row, in logical pixels.
	static constexpr float kPadX = 14.0f;      ///< `.rail { padding: 16px 14px }`
	static constexpr float kPadTop = 16.0f;
	static constexpr float kCardPadX = 9.0f;   ///< `.seed { padding: 7px 9px }`
	static constexpr float kCardPadY = 7.0f;
	static constexpr float kCardHeight = 38.0f;
	static constexpr float kPlaceHeight = 14.0f; ///< `.anc-row`
	static constexpr float kRowGap = 3.0f;       ///< `.tree { gap: 3px }`
	/// What one slot occupies when the sound has stored no stances — a card and its single placement
	/// row, which is what every slot was before variations existed.
	static constexpr float kSlotHeight = kCardHeight + kPlaceHeight + kRowGap;
	/// The two verbs on a variation row, sized under the card's own so the column reads down the rail.
	static constexpr float kRowActSize = 9.0f;
	static constexpr float kRowActGap = 6.0f;
	static constexpr float kHeadingHeight = 16.0f;
	/// The mock's `.sw`: an 18x10 pill with a 6px knob.
	static constexpr float kSwitchWidth = 18.0f;
	static constexpr float kSwitchHeight = 10.0f;
	/// The clone button, sized to sit beside the switch on the same line. DRAWN AS GEOMETRY rather
	/// than as the mock's ⧉ character: A16 already found that this editor's font has no glyph for the
	/// mock's symbols, and a missing glyph is an invisible button — worse than a plain one. Two offset
	/// rounded outlines say "a copy of this" in any font.
	static constexpr float kCloneSize = 11.0f;
	static constexpr float kCloneGap = 8.0f; ///< between the button and the switch
	/// The preview button, third on the `.acts` line and sized to match its neighbours. Drawn as a
	/// filled play triangle for the reason the clone glyph is drawn rather than typed (A16): the font
	/// has no symbol for this, and a missing glyph is an invisible control.
	static constexpr float kPreviewSize = 11.0f;
	/// The S inside it. Sized to fill the box's cap height rather than to a step of the type scale:
	/// this is a letter used as a mark, and it should read as one at 11px.
	static constexpr float kSoloLetterSize = 9.0f;
	static constexpr float kPreviewGap = 8.0f; ///< between it and the clone button
	/// The delete verb, rightmost on the `.acts` line — past the switch, because the one control that
	/// destroys must not sit between two that are safe. The same ✕ the variation rows already use to
	/// forget, drawn as geometry for the reason every glyph here is (A16), and sized with the row
	/// verbs rather than the make-verbs: destruction should not be the biggest thing on the line.
	static constexpr float kDeleteSize = 9.0f;
	static constexpr float kDeleteGap = 8.0f; ///< between the switch and it
	/// How far the cards you are NOT hearing fall back while a preview is latched. Not decoration: a
	/// latched listening mode that looked like the ordinary rail would be a surface lying about what is
	/// coming out of the plugin, which is the one thing this feature may not do.
	static constexpr float kUnheardOpacity = 0.4f;

private:
	/// **THE SUBSCRIPTIONS, HELD** — see `ui::Watch`. Declared last so they are released FIRST.
	std::vector<Watch> watches_;
	/// Where slot `n`'s card sits, in the frame's own pixels — scroll included, so a hit test and a
	/// draw cannot disagree about where a card is.
	visage::Bounds cardBounds(size_t slot) const;
	visage::Bounds switchBounds(size_t slot) const;
	/// The delete ✕, hard against the card's right padding — the end of the line.
	visage::Bounds deleteBounds(size_t slot) const;
	/// The clone button's face, left of the switch — the mock's `.acts` row, which holds both.
	visage::Bounds cloneBounds(size_t slot) const;
	/// And the preview button's, left of the clone.
	visage::Bounds previewBounds(size_t slot) const;
	/// Where the "+ New sound" row sits — after the last card the rail draws.
	visage::Bounds newRowBounds() const;
	/// Where one variation row sits, and where its two verbs sit inside it.
	visage::Bounds variationBounds(size_t slot, size_t index) const;
	visage::Bounds variationStoreBounds(size_t slot, size_t index) const;
	visage::Bounds variationForgetBounds(size_t slot, size_t index) const;
	/// How tall slot `n` is, rows included — the number that made the rail's geometry stop being a
	/// multiplication. Every card position is a running sum of these now, so a sound that grows a
	/// stance moves the ones below it rather than drawing over them.
	float slotHeight(size_t slot) const;
	/// WHETHER ANY OF SLOT `n` IS IN THE RAIL, its stance rows included. The draw loop's cull, named
	/// because it has to measure the WHOLE slot: a card scrolled just off the top can still have four
	/// stance rows on screen, and a cull that only knew about the card dropped them mid-scroll.
	bool slotOnScreen(size_t slot) const;

	/// WHICH GAP a drop at `position` is asking for, 0..visibleCount() inclusive, or nullopt outside
	/// the rail — which is the cancel. A point above the first card or below the last is the END of
	/// the list rather than nothing: the hand aimed past the list because that is where it wanted to
	/// go, and a gap can say so where a row index could not.
	std::optional<size_t> dropGapAt(visage::Point position) const;
	/// The empty slot a splice at `slot` would spend: the nearest one, so the fewest sounds move.
	std::optional<size_t> spliceRoom(size_t slot) const;
	/// THE LANE A SPLICE MOVES SOUNDS ALONG: the slots the list occupies, in order. Gaps are NOT in
	/// it — see the note on `laneSteps` in the .cpp for why shifting an empty slot is not the free
	/// operation it looks like.
	std::vector<size_t> occupiedLane() const;

	/// THE COLOUR A SOUND WEARS — the hue its card's dot and its star in the circle are drawn in.
	/// One accessor rather than an array lookup at each site, because what it reads is about to
	/// change and the sites should not each have an opinion about where a colour comes from.
	uint32_t seedHue(size_t slot) const;

	bool enabled(size_t slot) const;
	/// Where the sound is standing right now: the two parameters, which are the live placement and the
	/// only placement the engine has ever read.
	AnchorPoint livePlacement(size_t slot) const;
	void toggleEnable(size_t slot);

	void drawCard(visage::Canvas& canvas, size_t slot) const;

	ParamBridge& bridge_;
	SoundSelection& selection_;
	EditHistory& history_;
	FrameWatchers& previewWatchers_;
	std::array<uint32_t, kMaxSounds> pulseSeen_ {};
	std::array<float, kMaxSounds> pulseLevel_ {};
	std::array<int64_t, kMaxSounds> pulseAt_ {};
	int64_t pulseNow_ = 0;
	std::array<std::unique_ptr<ParamControl>, kMaxSounds> levelControls_;
	std::array<std::unique_ptr<FaderPill>, kMaxSounds> levelFaders_;
	Notifier* notifier_ = nullptr;
	float scroll_ = 0.0f;

	/// THE DRAG, in three pieces because a press is not yet a drag. `armed_` is the card a press
	/// landed on, `anchorY_` is where it landed, and `dragging_` is whether the pointer has since
	/// travelled far enough to mean it. Until it has, the press is still a click and every verb on the
	/// card still works — which is the whole reason the threshold exists rather than a handle.
	std::optional<size_t> armed_;
	float anchorY_ = 0.0f;
	bool dragging_ = false;
	/// Where it would land. Cleared when the pointer leaves the rail, which is this editor's only
	/// cancel: Escape would need keyboard focus and nothing here has ever asked a host for it (see the
	/// preview note above), so dragging out is the gesture that means "no".
	std::optional<size_t> drop_;
};

} // namespace feathers::ui
