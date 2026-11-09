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

#include "backplane/endpoint.h"
#include "plugin/ui/widget_registry.h"
#include "plugin/device_instance.h"
#include "plugin/device_registry.h"
#include "plugin/ui/drop_gap.h"
#include "plugin/ui/env_card.h"
#include "plugin/ui/mseg_card.h"
#include "plugin/ui/rack_card.h"
#include "plugin/ui/knob.h"
#include "plugin/ui/lane_selection.h"
#include "plugin/ui/macro_polarity.h"
#include "plugin/ui/macro_slot.h"
#include "plugin/ui/param_control.h"
#include "plugin/ui/relative_macro.h"
#include "plugin/ui/mod_state.h"
#include "plugin/ui/source_arm.h"
#include "plugin/ui/rail.h"
#include "plugin/ui/route_list.h"

#include <array>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <visage/ui.h>

// THE MODULATOR STRIP — the mock's `#modStrip`, and the surface #41 is about.
//
// WHERE IT SITS is the mock's answer, not a preference: `main{grid-template-columns: auto auto 206px
// minmax(0,1fr) auto}` is Engine rail, MODULATORS, sounds rail, stage, post-FX — a column on the left,
// outboard of the sound tree, which is where the design doc's own iteration landed it too.
//
// IT COSTS THE STAGE NOTHING, which is measured rather than hoped. StageDisk takes `min(width,
// height)` (celestial.h), so the circle and the Explore chart are bound by the stage's HEIGHT of 534
// while the stage is 866 wide — 332 px of it has never been reachable. This strip spends 236 of them:
// the disk keeps its radius, the chart keeps its width, and no corner encoder moves. What does change
// is that the disk re-centres, which is why the strip is PERMANENT and has no minimise button — the
// mock's `#stripMin` exists because its rig is resizable and ours is not, and a control that shifts
// the instrument sideways to buy screen nothing else can use is worse than the space it saves. The
// mock-parity audit deferred this exact question with a condition attached ("revisit if the FX lane
// and the modulator strip both land and the middle gets tight"); the condition has now been measured
// and it does not bite.
//
// FOUR CARDS RATHER THAN ONE, and that is a reading of the mock rather than a departure from it. The
// mock has one modulator, whose card carries one arrow that arms one source. Four macros are four
// sources, so they are four cards with four arrows — the card is the unit of "a source you can point
// at", and collapsing them into one card with four knobs would have left the arrow ambiguous about
// which of them it was arming.

namespace feathers::ui {

// (`NoEditorBody` STOOD HERE — a body saying "no editor yet" under a head the strip used to draw. The
// strip stopped drawing heads when cards took their own frames, so an unknown device in the strip had no
// head at all. A device this build does not have is `MissingDeviceCard` now (`generic_card.h`), the one
// card every host draws for it. The dispatch it guarded — `if (isMseg) MsegCard else EnvCard`, which
// drew a third device as a DAHDSR editing the wrong fields — is still closed: `cardKindOf` falls to
// `Generic` or `Missing`, never to another device's card.)


/// THE FOCUSED SOURCE'S ROUTINGS, ENUMERATED — `RouteList`, now in `route_list.h` over the host contract
/// so the rack's editor holds one too (rack roadmap §7f); implemented in `mod_panel.cpp`.
///
/// WHAT TO CALL A SOURCE ON SCREEN — a macro by its name (`ParamBridge::macroName`: the player's, or
/// "Macro 1".."Macro 4"), then "Sound A weight".."Sound H weight".
///
/// One vocabulary in one place, because three surfaces now name the same thing: the strip's cards, the
/// Engine rail's arrow grid, and the route list's heading. Two of them naming it differently would leave
/// a player unable to tell that the arrow they armed is the source the list is about.
///
/// The sounds are named by the letter the Perform circle labels their stars with, so the name, the
/// star and the arrow's own letter are all the same mark.
///
/// **BY ENDPOINT (kill-lanes step 2)**: a device source is named after the device its id names, asked
/// of the bridge's record, and numbered by where that device stands in its chain.
std::string modSourceName(const ParamBridge& bridge, backplane::Source source);

/// WHAT TO CALL A DESTINATION ON SCREEN, whichever kind it is — a parameter's own name, or a rack
/// device's name for one of its controls ("Env Attack"), qualified by slot once there is more than
/// one device to tell apart.
///
/// One vocabulary in one place, for the reason modSourceName has one: the route list, the
/// notification tray and the Engine rail all name destinations, and three surfaces disagreeing about
/// what a routing points at is three surfaces a player has to reconcile.
/// TAKES THE BRIDGE because a device destination's name is its DEVICE's name for that control, and
/// which device an id names is a runtime fact read from the record.
std::string modDestinationName(const ParamBridge& bridge, backplane::Destination destination);

/// **FEATHERS' OWN SOURCES AS ENDPOINTS** — a macro and a sound's weight are host ends, at the index
/// the axes give them. One spelling each, so no surface computes a core index by hand.
inline backplane::Source macroSourceAt(size_t macro) noexcept { return backplane::Source::core(macro); }
/// The Stack Index (backlog KE step 3): the host end after the weights.
inline backplane::Source stackIndexSource() noexcept { return backplane::Source::core(kStackIndexSource); }
inline backplane::Source weightSourceAt(size_t slot) noexcept {
	return backplane::Source::core(weightSourceOf(slot));
}
/// Whether `source` is one of the four macros — the header row's sources.
inline bool isMacroSource(backplane::Source source) noexcept {
	return !source.isDevice() && source.index < kMacroCount;
}

/// **EVERY SOURCE FEATHERS HAS, WALKED FROM ITS DEVICES** — the macros, the sound weights, then every
/// output of every device in either chain that declares one. What the panel hands `ModState` as its
/// source list (`ModState::setSourceLister`).
std::vector<backplane::Source> feathersSources(const ParamBridge& bridge);

class ModPanel : public Rail, public TooltipHost {
public:
	/// TAKES THE DEVICE SEAM ALONGSIDE THE PARAMETER ROAD. The strip still needs the bridge for
	/// Feathers' own furniture; its CARDS take the host, which is what lets the same card classes be
	/// built by the rack. The host is passed in rather than constructed here on purpose: `Gui::Impl`
	/// owns one already, declared above this panel so it outlives the cards that hold a reference to
	/// it. A `BridgeDeviceHost` member here would put that ordering back in play.
	ModPanel(ParamBridge& bridge, backplane::DeviceHost& host, WidgetRegistry& registry, ModState& mod, EditHistory& history,
	         Notifier* notifier = nullptr);

	/// **IT UNREGISTERS ITSELF, BECAUSE IT REGISTERED ITSELF** — see `Knob::~Knob`. The panel puts
	/// itself on `ModState`'s watcher list so the strip repaints when the focus moves; nothing took it
	/// off, and it survived only because a `ModPanel` happens to outlive the state it watches. That is
	/// a fact about member order in one file, which is not a guarantee.
	~ModPanel() override;

	void draw(visage::Canvas& canvas) override;
	void resized() override;
	/// The rail shows every child on reopening, the tooltip layer included; it is shown only by a hover that rests.
	void collapsedChanged() override;
	bool mouseWheel(const visage::MouseEvent& e) override;

	/// The strip's open width — see kWidth, which is the mock's card plus its padding.
	float expandedWidth() const override { return kWidth; }

	//--------//
	// Scroll //
	//--------//

	/// IT SCROLLS, for the sound rail's reason and by the sound rail's contract (sound_stack.h): five
	/// cards and a heading are 548px of content and the rail is 534 at the canvas the layout is
	/// written for, so the CV card's bottom edge was drawn off the end and clipped. The answer to
	/// content taller than its frame is never a taller window — the logical canvas has a floor and
	/// everything is written to fit inside it — so it scrolls under the wheel, and nothing is
	/// unreachable.
	///
	/// All three are readable because that contract is a statement about them: never scrolled past
	/// the end, never scrolled off the top, and every card reachable from some scroll position.
	float contentHeight() const;
	float scroll() const noexcept { return scroll_; }
	float maxScroll() const;
	void setScroll(float y);

	/// The mock's `--card-w:236px` for the card, plus the strip's own `padding:12px 10px`.
	static constexpr float kPadX = 10.0f;
	static constexpr float kPadTop = 12.0f;
	/// ONE NUMBER FOR ONE DESIGN — see `ui::kModCardWidth`, which the rack's lane reads too.
	static constexpr float kCardWidth = kModCardWidth;
	static constexpr float kWidth = kCardWidth + 2.0f * kPadX;
	static constexpr float kCardGap = 10.0f;

	/// The mock's `--card-head-h:24px`.
	static constexpr float kHeadHeight = 24.0f;

	//-------------------------------------------------------------//
	// THE MACRO HEADER ROW — four knobs, permanent, never a device. //
	//-------------------------------------------------------------//

	// The 4 macros live in the modulators header, not in a device: unlike effect/modulator cards,
	// the macros are reachable from outside Feathers as the mod-in mechanism.
	//
	// IT IS A TIER, NOT A LAYOUT. A macro is a real CLAP parameter — a host can automate it, and in a
	// host with no modulation of its own the macros are the ONLY modulation this plugin can be given.
	// That is what makes them the mod-IN, and it is a different kind of thing from a device the rack
	// holds: a device's controls are deliberately not parameters at all (mod_rack.h). Four cards said
	// they were four devices. A header row says what they are.
	//
	// AND IT BUYS THE ROOM THE CARDS NEED. Four macro cards were four times
	// (24 + columnHeight(44)) = 408px of a 534px rail, which left nowhere for a device to go. The row
	// is under a hundred, and the difference is exactly what the envelope card sits in.
	static constexpr size_t kMacroColumns = kMacroCount;
	/// Smaller than the 44px the cards carried, because four across 216px is 54px a column and a dial
	/// has to leave room for its own caption.
	static constexpr float kMacroDiameter = 36.0f;
	/// The arrow sits UNDER its knob rather than beside it — a column is 54px wide and a row of
	/// knob-then-arrow would leave the dial 34px of it.
	static constexpr float kMacroArrowHeight = 14.0f;
	static constexpr float kMacroRowHeight =
	    Knob::columnHeight(kMacroDiameter) + 3.0f + kMacroArrowHeight;

	/// The strip's own heading, `Modulators`.
	static constexpr float kHeadingHeight = 16.0f;

	/// The `(+)` row: one line of type in a box the width of a card, which is the mock's own
	/// `.slot-empty` shape at a single line rather than four.
	static constexpr float kAddHeight = 22.0f;

	/// The CV card: a head row and one line of explanation under it, and nothing else because there is
	/// nothing behind it yet.
	static constexpr float kCvCardHeight = kHeadHeight + 26.0f;

	/// The arrow: the mock's `➤` drawn as GEOMETRY rather than set as a glyph, which is A16's lesson —
	/// this font has no such character and a missing glyph is an invisible control.
	static constexpr float kArrowSize = 13.0f;

	/// Collapse is a part like any other, and it is tested FIRST because it is the one verb that still
	/// answers when the rail is closed and every card is gone. Fold is its per-card cousin.
	/// `Card` IS GONE — the head no longer arms. See the 2026-08-24 decision: arming attaches to an
	/// OUTPUT, outputs moved to the body, so the head stops being an arming surface and fold owns it
	/// outright. The same conflict showed from the other end before the arrow question
	/// existed: modulator devices could not be collapsed the way audio effects could.
	/// `Macro` IS WHAT `Card` WAS, AND ONLY FOR MACROS. The old name covered two unrelated things: a
	/// macro COLUMN, which focuses, and a device's HEAD, which armed. The head half is gone (see
	/// above); the macro half stays and gets a name that says which it is.
	enum class Part { None, Collapse, Fold, Arrow, Add, SlotMenu, Macro };

	struct Hit {
		Part part = Part::None;
		backplane::Source source {};
	};

	/// Public so a case can drive the real widget at the real place rather than at a rectangle it
	/// restated — the pattern ExploreStage::partBounds established.
	Hit hitAt(visage::Point pixels) const;

	/// WHERE A SOURCE LIVES IN THE STRIP — a macro's column in the header row, or a device's card.
	///
	/// One accessor over two kinds of thing, because every caller that has ever used it asks the same
	/// question ("what rectangle is this source") and none of them cares which kind it got. It used to
	/// be `cardBounds` and mean only a macro card; the rename is the point rather than tidiness, since
	/// a macro no longer has a card and a name that said otherwise would be the panel lying about its
	/// own layout.
	visage::Bounds sourceBounds(backplane::Source source) const;
	visage::Bounds arrowBounds(backplane::Source source) const;

	/// **WHICH RACK SLOT THIS SOURCE BELONGS TO, OR `kNoPosition` WHEN IT IS NOT THE RACK'S.**
	///
	/// THIS REPLACED `isRackSource(source)` AT SEVEN SITES, AND THE REPLACEMENT WAS A FIX RATHER THAN A
	/// RESPELLING: `isRackSource` tested a lane's NUMBER, which answered YES for a rack lane holding
	/// NOTHING. This asks the RACK whether it holds the source's device — `ParamBridge::rackSlotOf`, by
	/// the id the endpoint names (kill-lanes step 2) — so the question and the answer are the same lookup
	/// and there is nothing to get out of step. A macro or a weight names no device and belongs to no slot.
	size_t rackSlotOfSource(backplane::Source source) const;

	//---------------------------------------------//
	// The outputs row — one arrow per declared output //
	//---------------------------------------------//

	/// **WHAT THE DEVICE IN `slot` IS, OR NULL** — past the rack's count, past the array, or a tag this
	/// build cannot name, all answered by one lookup so a caller cannot remember two of the three.
	const device::Descriptor* descriptorOf(size_t slot) const;

	/// HOW MANY MODULATION OUTPUTS THE DEVICE IN `slot` PUBLISHES, asked of its descriptor. Zero for a
	/// device that publishes none, which is the case a head-mounted arrow could not express at all.
	size_t outputCountOf(size_t slot) const;

	/// THE ROW THE ARROWS LIVE IN, between the head and the device's own body — and ZERO HIGH for a
	/// device with no outputs, so it costs nothing to a device that has none.
	///
	/// IT WAS IN THE HEAD, AND THAT WAS RIGHT ONLY BY COINCIDENCE OF N=1. Both rack devices declare
	/// exactly one output, so a single head-mounted arrow happened to fit; nothing in the code could
	/// express two, or none. A row grows with the count and vanishes without one.
	float outputRowHeight(size_t slot) const;
	/// **HOW MANY ROWS OF CHIPS THIS SLOT'S DEVICE NEEDS.** One for every device that declares one
	/// output, which was every device until the Note Expressions modulator declared seven.
	size_t outputRowsOf(size_t slot) const;

	visage::Bounds outputRowBounds(size_t slot) const;

	/// WHAT THIS INSTANCE CALLS THIS OUTPUT — `ParamBridge::outputLabel`: the player's label, else the
	/// device's declared `SourceDesc::name`; empty where there is no such output. The strip does not
	/// name a device's outputs any more than it names its params — the device declares, the patch labels.
	std::string outputName(size_t slot, size_t output) const;
	/// Every output's name, in order — what the chip walk measures (`outputChipRowsFor`).
	std::vector<std::string> outputNamesOf(size_t slot) const;

	/// ONE OUTPUT'S WHOLE CHIP — the arrow AND the name beside it — which is what a press hits.
	///
	/// THE LABEL IS PART OF THE CONTROL, not a caption near one. Two outputs draw two identical
	/// glyphs and only the name says which is which; a name that could be read but not pressed would
	/// put the readable half and the pressable half in different places.
	visage::Bounds outputChipBounds(backplane::Source source) const;

	/// The chip's width, which is the arrow's box plus whatever the device's name for the output
	/// measures. MEASURED rather than stepped: the names belong to the devices, so any fixed step is
	/// a guess that is wrong for somebody.
	///
	/// **NOTHING IN THIS PANEL CALLS IT ANY MORE — IT IS A TEST SEAM WEARING AN ACCESSOR'S NAME.** The
	/// layout used to walk it twice (a row count and a placement); both walks are
	/// `ui::outputChipRowsFor` / `outputChipBoundsFor` now, which measure through `SourceArm::widthFor`
	/// directly. It is kept because the case that proves a chip is as wide as its NAME rather than a
	/// fixed step needs the per-output width to compare against, and recovering one chip's width from
	/// a rectangle it was already used to compute would be that case checking itself.
	float outputChipWidth(size_t slot, size_t output) const;

	/// HOW TALL ONE SOURCE'S WHOLE ROW IS — head, outputs row and body together. For the cases: a
	/// fold's effect on a card's height is the claim, and it is not readable any other way.
	float sourceHeightForTest(backplane::Source source) const { return sourceHeight(source); }
	/// The card rectangle a source occupies — for cases that need to say "inside the card" without
	/// restating how the card is laid out.
	visage::Bounds sourceBoundsForTest(backplane::Source source) const { return sourceBounds(source); }

	static constexpr float kOutputRowHeight = 20.0f;

	/// The whole header row, and one macro's column within it.
	visage::Bounds macroRowBounds() const;
	visage::Bounds macroColumnBounds(size_t macro) const;

	/// One device card. `slot` is a POSITION in the rack — the order the strip draws — never an address:
	/// the device standing there is `ParamBridge::rackDeviceAt(slot)`.
	visage::Bounds envCardBounds(size_t slot) const;

	/// How many device cards the strip is drawing, which is the rack's count.
	size_t deviceCount() const;

	/// WHICH KIND OF CARD A SLOT WANTS — the ONE dispatch, asked of the rack.
	///
	/// IT IS AN ENUM AND NOT A CHAIN OF `if`s IN `ensureCard` BECAUSE TWO PLACES NEED THE ANSWER: the
	/// card gets built from it, and `sourceHeight` needs the height before the card exists. Two
	/// chains would be two places to forget the same device.
	/// **WHAT THE STRIP BUILDS FOR A SLOT.**
	///
	/// `Generic` is the fallback arm, and `Missing` is for a device this build does not have — the
	/// shared `MissingDeviceCard` ("device missing"). The tracker is on the generic card for now, its own card later.
	/// Before that decision the strip had two hand-written cards and no generic arm, so registering any
	/// third modulator meant writing a card first — which is the hold-out the tracker sat in, and the
	/// reason both note lanes had no end-to-end coverage.
	///
	/// THE CHOICE IS STRUCTURAL, WHICH IS THE FX LANE'S RULE (vision D3, option A): a device gets a
	/// bespoke card if the strip has one for its tag and the generic grid otherwise, so a device
	/// arriving in the catalogue needs no maintenance here.
	enum class CardKind { Dormant, Envelope, Mseg, Generic, Missing };
	CardKind cardKindOf(size_t slot) const;

	/// HOW TALL THAT SLOT'S BODY IS. The card's own answer when it has been built, and the kind's
	/// otherwise — which is the same number, because each card's extent at `kCardWidth` is the static
	/// this reads.
	///
	/// **ASKED AT `kCardWidth` IN BOTH ARMS, which is what makes them the same number.** A generic
	/// card's height depends on its width — the outputs foot is as many rows as the chips need — so
	/// an arm that asked the live card about the width it currently HAS would answer differently on
	/// the layout that built it. See the note on the live arm.
	float bodyHeightOf(size_t slot) const;

	/// WHAT THE HEAD ROW OF A SLOT'S CARD SAYS, asked of the device in the slot.
	///
	/// The strip draws every card's head — that division is stated where the head is drawn — so the
	/// strip is what has to know which device it is heading. It named `mod::dahdsr::kTraits.name`
	/// unconditionally, which was true while a rack slot could hold only an envelope and became a
	/// card labelled DAHDSR over an MSEG's editor the moment a slot could hold either. It showed up
	/// within minutes of the (+) existing, and read as the wrong device having been added.
	const char* deviceName(size_t slot) const;

	//------------------------//
	// Adding a device: the (+) //
	//------------------------//

	/// THE `(+)` UNDER THE LAST CARD, or under the macro row when the rack is empty.
	///
	/// AT THE END BECAUSE THE OPERATION IS AN APPEND. addRackDevice puts a device in the next free
	/// slot and nowhere else — the one mutation that cannot move an existing device, and therefore
	/// cannot move a live routing column. A `(+)` drawn BETWEEN two cards would be offering an
	/// insertion, which is a different operation with a different cost, and the button would be
	/// promising something the road under it does not do.
	visage::Bounds addBounds() const;

	/// The devices this strip may be asked to add, in the table's own order — every registered device
	/// that declares a modulation output AND tags itself a modulator. DERIVED, NEVER LISTED, in both
	/// halves: `maySitInModRack` reads the device's own declarations and `isTaggedModulator` reads its
	/// own `kTags` (device_row.h), so a device gains a seat here by what it declares rather than by
	/// anyone remembering to add it to a menu.
	///
	/// THE TWO HALVES ANSWER DIFFERENT QUESTIONS, which is why both are here. The output is the
	/// CAPABILITY — the rack will take it, and `addRackDevice` still asks for nothing more. The tag is
	/// what the device CALLS ITSELF, and it is what keeps an fx device that happens to publish outputs
	/// out of the picker. Backlog FM is the browser that makes this a CLEARABLE
	/// default rather than a filter baked into one menu.
	std::vector<const device::Descriptor*> addableForTest() const;

	/// Open the picker, as a press on the `(+)` does. Public so a case can drive the gesture without a
	/// window: `visage::PopupMenu::show` needs one, and what this case is about is which devices are
	/// offered and what choosing one does — see addChoiceForTest.
	///
	/// WITH `before` NAMING A SLOT it is the "Add before" road (backlog CR): the same picker, opened at
	/// that card, and the pick inserts before THAT CARD — its lane is captured when the picker opens.
	/// With `after`, the "Add after" road: the same picker at the same card, the pick going BEHIND it.
	void showAddMenu(size_t before = kNoPosition, bool after = false);

	/// THE MENU `showAddMenu` WOULD SHOW, built and not shown — its rows, their enables and its
	/// `onSelection`, so a case can read the shape and pick a row without a window.
	visage::PopupMenu addMenuForTest(size_t before = kNoPosition, bool after = false);

	/// Choose the nth offered device, as clicking that row of the menu would. Returns the slot it
	/// landed in, or kNoPosition if the rack refused it.
	size_t addChoiceForTest(size_t index);

	/// "Add before" chosen for `slot`, then the nth offered device picked — the whole gesture without
	/// a window. Returns the position the new device landed at, or kNoPosition if refused.
	size_t insertChoiceForTest(size_t slot, size_t index);

private:
	/// **THE (+)'S PICK** — `appendDevice`, except that a note-EMITTING device (an arp) goes in front of the first device
	/// while "Arps sent to front of chain" is on (`emitters_front.h`). The menu's (+) and `addChoiceForTest` come here.
	size_t addDevice(const char* tag);
	/// Append one device by tag, announce it, and repaint. The one place an append meets the bridge, so there is a
	/// single road to test and a single place a refusal is worded. An "Add after" the last card lands here directly:
	/// it names its place.
	size_t appendDevice(const char* tag);
	/// Insert a new device of `tag` before the device `anchor`; one undo step.
	size_t insertDeviceBefore(DeviceInstanceId anchor, const char* tag);
	/// "ADD AFTER" IS "ADD BEFORE THE NEXT ONE" — or an append behind the last — through the two roads
	/// above, so a full rack is refused in the same words. `anchor` is found at the pick.
	size_t insertDeviceAfter(DeviceInstanceId anchor, const char* tag);

public:

	//----------------------//
	// Folding a single card //
	//----------------------//

	/// THE MOCK'S SECOND, FINER COLLAPSE — `.mod-card.min > *:not(.head){display:none}`, a card kept
	/// as its head row alone, minimised per card. It composes with the rail rather than
	/// competing with it: the rail is "put this whole panel away", a fold is "I am done with this
	/// macro but I want to see that it is there".
	/// A FOLD IS A DEVICE CARD'S VERB AND NOT A MACRO'S, since the macros stopped being cards. A macro
	/// source answers false and refuses the setter, which is the honesty rule applied to a verb: the
	/// header row has no fold button to press.
	bool cardMinimised(backplane::Source source) const;
	/// Returns whether this changed anything, so a caller can announce a real fold and stay quiet
	/// about a redundant one.
	bool setCardMinimised(backplane::Source source, bool minimised);

	/// Where the fold button is — in the head row, inboard of the arrow. Public for the same reason
	/// arrowBounds is: a case drives the real button rather than a rectangle it restated.
	visage::Bounds foldBounds(backplane::Source source) const;

	//---------------------------------------//
	// Shrinking and reordering the rack      //
	//---------------------------------------//

	/// THE THREE DOTS, in the head row this strip DRAWS ITSELF — which is why the rack's verbs can
	/// live in the strip while the fx lane's had to be pushed into the cards. fx_card.h says the two
	/// chains divide the head differently; this is the first place that difference paid rather than
	/// cost. An fx card fills its lane and would have swallowed a lane-drawn target; a rack card is
	/// handed only its BODY, and the head above it is the strip's own rectangle.
	visage::Bounds slotMenuBounds(backplane::Source source) const;

	/// Open it for `slot`, as pressing the dots does.
	void showSlotMenu(size_t slot);

	/// **FREE THE CARDS A REBUILD RETIRED — CALLED ONLY WHERE NOTHING IS WALKING THE TREE.**
	///
	/// `FxLane::releaseRetiredCards` verbatim, for the reason it gives, and this panel needed it the
	/// moment `ensureCard` started rebuilding on a lane change. `ensureCard` used to free last
	/// round's cards itself, which is safe on the road it was written for — a state-channel notify —
	/// and NOT safe on a drag: `FxCard::noteCardRelease` fires `onCardDrag(DragPhase::End)`, which
	/// reaches `ModPanel::moveDevice` and then `resized()`, and a reorder moves at least two lanes,
	/// so the second slot's `clear()` would free the card the first slot retired — the one whose
	/// `onMouseUp` is still on the stack. That crashed on every reorder the FX chain and
	/// this panel was one rebuild away from the same thing.
	///
	/// **THE CALLER DECIDES BECAUSE ONLY THE CALLER KNOWS.** `Gui::Impl::tickOnce()` runs before the
	/// pump drains an X event, which is the one place in the editor that can promise it.
	void releaseRetiredCards() { retiredCards_.clear(); }

	/// HOW MANY CARDS ARE WAITING TO BE FREED. For cases, and for the reason `FxLane` states: "a
	/// reorder does not destroy the card that is dispatching" is a claim about a DEFERRAL, and a case
	/// that only checks the editor survived would pass on a build that freed it and got lucky.
	[[nodiscard]] size_t retiredCountForTest() const { return retiredCards_.size(); }

	/// **WHICH SLOT THE LAST `showSlotMenu` WAS ASKED ABOUT — the value, not a consequence of it.**
	///
	/// A case that removes through the menu and counts the survivors is green whichever device went;
	/// a case that reads the popup's contents cannot tell two identical modulators apart. The claim
	/// under test is "the press on THIS card opened the menu for THIS slot", and the only honest way
	/// to hold a claim about a value is to read the value — CLAUDE.md's rule, and this is five lines
	/// of it. Recorded before the range check, so a refusal is visible too.
	size_t lastSlotMenuForTest() const noexcept { return lastSlotMenu_; }

	/// WHAT THAT MENU OFFERS for a given slot — the ends cannot move further out, and the rows are
	/// DISABLED rather than absent so Remove does not land under a different finger depending on
	/// which card you pressed.
	enum class SlotVerb { AddBefore, AddAfter, MoveUp, MoveDown, Remove };
	struct SlotOption {
		SlotVerb verb;
		bool enabled;
	};
	std::vector<SlotOption> slotOptionsForTest(size_t slot) const;
	bool slotChoiceForTest(size_t slot, SlotVerb verb);

	/// The strip's verbs on a rack slot. Public because a case drives them; the menu and the drag are
	/// the only things in the editor that call them.
	bool removeDeviceAtLane(size_t slot);
	bool moveDevice(size_t from, size_t to);

	//------------------------//
	// Selecting a card        //
	//------------------------//

	/// **WHICH CARD A KEY ACTS ON — SELECTED BY A PRESS ANYWHERE ON ITS FACE.** A mod card is
	/// highlighted/outlined when selected by a click anywhere on its face.
	///
	/// THE ROAD IS `FxCard::onCardPressed`, WHICH EXISTED AND THIS PANEL NEVER BOUND. The strip
	/// subscribed to `onCardDrag`, `onHeadFold` and `onHeadMenu` and not to the one hook that says
	/// "the hand arrived". The fx lane and the rack bind it and select on a press anywhere including
	/// a knob, because `FxCard` accepts keystrokes and its `focusChanged(is_focused, was_clicked)`
	/// fires the hook when a press on a CHILD moves the keyboard focus up to the card (landed at
	/// `2522511`). So a press on a value box or a knob selects the card it sits in without the strip
	/// hearing the press itself — which is the point: the other way to hear it,
	/// `setReceiveChildMouseEvents`, delivers the CHILD's coordinates and would be read as the card's.
	///
	/// **HELD BY DEVICE INSTANCE ID, NOT BY POSITION** — the fold set's key, for the fold set's
	/// reason, and for the week's: `removeRackDevice` slides every survivor up one, so a position
	/// remembered across a removal names a DIFFERENT DEVICE, and a held Delete would walk the rack.
	/// The fx lane, keyed by position, has to reset its focus before every removal to stay safe; an
	/// id cannot go stale in that direction — after a removal it either names a survivor, wherever
	/// that survivor now sits, or names nothing. `selectedSlot()` resolves it to a position at the
	/// moment it is asked, and every reader asks.
	///
	/// `kNoFocus` is `kNoPosition`, the same "none" `addChoiceForTest` answers — a position no live
	/// slot can have, whatever the count grows to.
	static constexpr size_t kNoFocus = kNoPosition;
	/// Select the device at `slot`, as a press on its card does. A slot the rack does not hold selects
	/// nothing.
	void selectSlot(size_t slot);
	void clearSelection();
	/// The selected device's CURRENT position, or `kNoFocus`. Resolved now, never remembered.
	size_t selectedSlot() const;
	size_t selectedSlotForTest() const { return selectedSlot(); }
	/// THE MACRO ROW'S KNOB FOR MACRO `index`, or null — built over its polarity view (backlog EG).
	/// Shown only while the macro's type is Knob; it is built either way (`MacroSlot`).
	[[nodiscard]] Knob* macroKnobForTest(size_t index) const {
		return index < slots_.size() ? &slots_[index]->knob() : nullptr;
	}
	/// THE WHOLE SLOT — the knob, the chip and the rename field one macro's column holds.
	[[nodiscard]] MacroSlot* macroSlotForTest(size_t index) const {
		return index < slots_.size() ? slots_[index].get() : nullptr;
	}
	/// THE ANCHOR OF THE SELECTION — the card a single Delete acts on (the old single focus).
	DeviceInstanceId selectedInstanceForTest() const noexcept { return selection_->selection().anchor(); }

	/// **THE STRIP'S MULTI-SELECTION AND ITS VERBS** (backlog PN) — the fx lane's own `LaneSelection` (backlog OA),
	/// over the bridge's strip site (`ParamBridge::stripFragmentSite`): click, Shift range, Ctrl/Cmd toggle, the lasso
	/// on the strip's bare background, Ctrl/Cmd + A C X V D, Delete over two or more, the dots menu's verbs, the block
	/// drag, the OS clipboard and one undo step each. Nothing here is a second copy of any of it.
	[[nodiscard]] LaneSelection& selectionForTest() noexcept { return *selection_; }
	/// The editor's Esc stack: a selection of two or more is one entry on it.
	void setEscapeStack(EscapeStack* stack) { selection_->setEscapeStack(stack); }
	/// THE DOTS MENU'S VERB, as picking it would — over the selection when it holds the card at `slot`.
	bool slotVerbForTest(size_t slot, FragmentVerb verb);
	/// The menu's verb ids start here, past the slot verbs.
	static constexpr int kVerbIds = 100;
	/// THE MACRO ROW'S TOOLTIP LAYER (`tips_`); `updateTooltips` is its delay, advanced on the editor's tick.
	TooltipLayer& tooltipLayer() override { return tips_; }
	bool updateTooltips() { return tips_.advance(); }
	/// THE ARM FOR ONE OUTPUT OF THE CARD AT `slot`, or null where none is built — what its chip prints.
	[[nodiscard]] const SourceArm* outputArmForTest(size_t slot, size_t output) const {
		for (const OutputArm& entry : outputArms_) {
			if (entry.slot == slot && entry.output == output) {
				return entry.arm.get();
			}
		}
		return nullptr;
	}

	//------------------------//
	// Dragging a card         //
	//------------------------//

	/// Reorder by drag rather than by "move earlier". The menu entries stay
	/// beside it — they cost nothing and are the only reorder a small window can reach.
	///
	/// THE WHOLE GESTURE IS THE STRIP'S HERE, unlike the fx lane's. The strip draws every head row
	/// itself and hands a device card only its BODY, so a press on the head — the natural grab
	/// handle — lands on the strip and never reaches the card. The fx lane had to push the same
	/// gesture down into its cards because a card there fills the lane. Second time that difference
	/// has paid rather than cost.
	std::optional<size_t> dragSlot() const noexcept { return dragging_ ? dragSlot_ : std::nullopt; }
	std::optional<size_t> dropGap() const noexcept { return dragging_ ? dropGap_ : std::nullopt; }

	/// Which gap a point in the strip's coordinates is asking for, or nullopt outside it.
	std::optional<size_t> dropGapAt(visage::Point position) const;
	/// Where the bumper is drawn for a gap — a band between two cards, above the first, below the
	/// last. Public so a case can ask the geometry rather than photograph it.
	visage::Bounds gapBounds(size_t gap) const;

	static constexpr float kDragThreshold = 4.0f;

	/// How tall a SOURCE's rectangle currently is — the header row's height for a macro, and for a
	/// device its card's height, or just its head row when folded.
	float sourceHeight(backplane::Source source) const;

	/// Told when a fold changes, so the editor can remember it — the same shape as the rail's
	/// onRailWidthChanged, and for the same reason: the panel knows what happened, the editor knows
	/// what is worth writing down. The strip itself knows nothing about files.
	void onCardFoldChanged(std::function<void(backplane::Source)> callback) {
		foldChanged_ = std::move(callback);
	}

	/// **THE MODULATOR STRIP'S WINDOW ROADS, the fx lane's twins** (the window
	/// behaviour applies to mod devices as well as effects). Installed by the editor, which owns the
	/// layers; empty outside one. `onExpandDevice` opens a device's window from its card's expand chip
	/// (folded or not); `onFocusDeviceWindow` brings an open one to the front on a press on the card's
	/// head, and returns whether one was open. A modulator that declares no window never offers the
	/// chip, so the first is reached only by a device that has one.
	std::function<void(DeviceInstanceId)> onExpandDevice;
	std::function<bool(DeviceInstanceId, DeviceNest)> onFocusDeviceWindow;
	/// **A SOURCE ARMED THAT THIS STRIP DOES NOT HOLD** — a modulator in the fx lane (2026-10-04). `ModState` keeps
	/// ONE arm listener and this panel installs it, so the strip selects its own card and hands anything else
	/// here; the editor points it at the fx lane, which focuses the card the source belongs to.
	std::function<void(backplane::Source)> onArmedElsewhere;

	/// Where the disabled CV card sits. It is drawn because the mock draws it — a placeholder that says
	/// what the empty slot is for — and it answers nothing, because there is nothing behind it yet.
	visage::Bounds cvCardBounds() const;

private:
	void focusOrArm(const Hit& hit);

	ParamBridge& bridge_;
	/// THE DEVICE SEAM THE CARDS ARE BUILT ON. Declared here, ABOVE `cards_`, because every card
	/// holds a reference to it and members are destroyed in reverse — the ordering rule that cost a
	/// heap-use-after-free once already this week.
	backplane::DeviceHost& host_;
	/// HELD, NOT JUST PASSED THROUGH. The panel was handed the registry to build knobs with and did
	/// not keep it; retiring a card now has to take its whole subtree OFF the registry as well as off
	/// the state list, and that happens long after construction. See `forgetFrameTree`.
	WidgetRegistry& registry_;
	ModState& mod_;
	EditHistory& history_;
	Notifier* notifier_ = nullptr;
	/// THE MACROS' CONTROLS, in two layers (backlog EG): the parameter, and the polarity view over it
	/// that the slot's knob and chip are both built on. Declared before `slots_` so they outlive the
	/// slots — a held momentary lets go through the view as its chip is destroyed.
	std::vector<std::unique_ptr<ParamControl>> macroParams_;
	std::vector<std::unique_ptr<BipolarMacro>> macroViews_;
	/// **THE TURN IN FLIGHT ON A RELATIVE MACRO (backlog PQ)** — one at a time, since one hand turns one encoder.
	/// Declared before `slots_`, whose encoders call into it, so it outlives them.
	std::unique_ptr<RelativeMacroGesture> relativeTurn_;
	std::vector<std::unique_ptr<MacroSlot>> slots_;
	/// THE PANEL'S EAR ON MACRO RENAMES — it refreshes `slots_`, so it is DECLARED AFTER them and dies
	/// first (and the destructor ends it earlier still).
	device::Subscription renameSubscription_;
	/// **THE MACRO ROW'S TOOLTIPS** (a long macro name cut in its column, read by hovering it) — a
	/// layer over the whole rail, `EngineRail`'s arrangement. The strip's cards host their own, and the NEAREST host
	/// wins, so this one only ever speaks for what sits on the rail itself: the macro slots.
	TooltipLayer tips_;
	/// **AND ON OUTPUT RELABELS** — a chip prints its output's label and is as wide as it, so a relabel
	/// (the MACROS card's rename, or a patch load) lays the strip out again. Ended in the destructor.
	device::Subscription labelSubscription_;
	/// One card per rack slot, built for the pool rather than for the count — a slot the rack does not
	/// currently hold keeps a real rectangle and is simply not shown, which is the dormant-widget
	/// discipline the Explore width knobs already follow. It is also what lets a browser turn one on
	/// without constructing anything.
	/// THE CARDS, WHICHEVER DEVICE EACH SLOT HOLDS.
	///
	/// This used to be `std::array<std::unique_ptr<EnvCard>, N>` — the STRIP declaring the type of its
	/// contents, which is exactly the shape the rack itself had before a slot could hold a device. A
	/// rack that can hold an MSEG and a strip that can only draw envelopes is half-unpinned, and the
	/// half left over is the one a hand can see.
	/// BY POSITION, AS LONG AS THE STRIP — grown by `reserveCards` (unbounded devices step 6).
	std::vector<std::unique_ptr<FxCard>> cards_ = std::vector<std::unique_ptr<FxCard>>(kMaxRackSlots);
	/// Grow every per-position array to at least `count`. UI thread.
	void reserveCards(size_t count);

	/// ONE ARM PER DEVICE OUTPUT, AS A WIDGET RATHER THAN A PAINTED CHIP.
	///
	/// **THIS PANEL USED TO DRAW ITS OWN CHIPS AND HIT-TEST ITS OWN RECTANGLES**, while the rack built
	/// `SourceArm` frames for the same verb — two implementations of "point this source at something".
	/// It broke the day a card grew its own frame: a panel draws BENEATH its children, so the painted
	/// chips vanished under the card and the rack's widgets did not. A unified API for mod-assign
	/// widgets is what resolves it.
	///
	/// SIBLINGS OF THE CARD, so they are above it BY CONSTRUCTION rather than by draw order. Rebuilt
	/// with the cards, and each unwatches itself in its own destructor.
	struct OutputArm {
		std::unique_ptr<SourceArm> arm;
		size_t slot = 0;
		size_t output = 0;
		/// PART OF THE IDENTITY, not decoration — a slot whose device changed keeps its number, and an
		/// arm kept across that swap would name the old device's output beside the new one's arrow.
		std::string label;
		/// **THE SOURCE IS NOT HERE, AND THAT IS DELIBERATE.** It is derived from `slot` — through the
		/// DEVICE standing at the slot, which changes when the rack is shortened or reordered — so
		/// remembering it would be remembering which device used to be here. The arm asks instead; see
		/// `ModPanel::sourceOfOutput` and `SourceArm::setSourceProvider`.
	};
	std::vector<OutputArm> outputArms_;

	/// THE ENDPOINT A SLOT'S n-th OUTPUT PUBLISHES ON — the output of the device standing at the slot,
	/// read at the moment it is asked. Every arm reaches this through its provider rather than holding an
	/// answer, because which device stands at a slot moves whenever the rack is shortened or reordered.
	backplane::Source sourceOfOutput(size_t slot, size_t output) const;

	/// The arm for one output, made if it does not exist yet. Null when the slot holds nothing.
	SourceArm* ensureOutputArm(size_t slot, size_t output);

	/// Drop arms whose output is gone or whose device changed under them.
	void pruneOutputArms();

	/// **DISARM IF THE ARMED SOURCE'S DEVICE HAS LEFT.** `ModState` never revalidates its source, so
	/// deleting the armed modulator otherwise leaves the strip claiming a gesture that
	/// the routing authority will refuse. Disarms rather than re-points — see the definition for why
	/// choosing a replacement source would be worse than choosing none.
	void disarmIfArmedSourceIsGone();

	/// RETIRED CARDS, freed by `releaseRetiredCards` and by nothing else. Same reason as
	/// `FxLane::retiredCards_`: a card destroyed while the editor is mid-draw leaves visage painting
	/// a dead widget.
	std::vector<std::unique_ptr<FxCard>> retiredCards_;

	/// What each card was BUILT FOR, so a slot whose device changed gets a new card rather than the
	/// old one showing the wrong controls. Compared against the rack's tag rather than trusted: the
	/// device in a slot can change under the panel — a patch load does it today and the browser will
	/// do it deliberately.
	std::vector<device::Tag> cardTag_ = std::vector<device::Tag>(kMaxRackSlots);

	/// **AND WHICH DEVICE IT WAS BUILT FOR, WHICH THE TAG CANNOT STAND IN FOR.**
	///
	/// `cards_` is keyed by POSITION and a card holds its DEVICE'S ID (kill-lanes step 2; a lane before
	/// that, a position before BX step 4). Those two are joined only at the moment the card is made, so
	/// the cache key has to name both or a reused card keeps a device that has moved out from under it —
	/// MEASURED when the card held a lane: three DAHDSRs, remove position 1, and the card still at
	/// position 1 went on addressing the device that had been there. It draws and edits the DELETED
	/// device, which is what "remove removed a different modulator" looks like from the hand's side.
	///
	/// THE TAG CANNOT CATCH IT AND NEITHER CAN THE KIND: two DAHDSRs are the same kind AND the same
	/// tag, so the key hits and `ensureCard` returns early. This is not a stronger version of the
	/// same test, it is the OTHER half of the address.
	///
	/// **NOT ASKED OF THE CARD.** `FxCard::device()` would answer for a real card and answer
	/// `kNoInstance` for `MissingDeviceCard`, whose base takes the default — so an unknown slot would
	/// compare unequal on every layout and rebuild its card every frame. The panel records what it built
	/// with, which is a fact it already has in hand.
	std::vector<DeviceInstanceId> cardDevice_ = std::vector<DeviceInstanceId>(kMaxRackSlots);

	/// Build the card `slot` needs if it has not got it. Called before anything asks a card its size
	/// or draws it. True when it built one.
	bool ensureCard(size_t slot);

public:
	/// THE HEIGHT THE STRIP GAVE A SLOT'S CARD, and the frame itself. Exposed because "the strip draws
	/// the card the device asks for" is a claim about which TYPE got built, and the height is the one
	/// thing the two cards genuinely disagree about — a case can read it without a canvas.
	/// ADVANCE EVERY CARD'S LIVE ELEMENT, RETURNING HOW MANY ASKED FOR A FRAME.
	///
	/// `FxLane::updateCards()` line for line, and the count is a count produced BY the work — the
	/// only thing that cannot be had without doing it, which is what makes it worth returning at all.
	size_t updateCards();

	/// **RE-SEAT ANY MACRO WHOSE TYPE MOVED UNDER THE ROW** — a patch restore, which reports a type to
	/// nobody (`restoreMacroModes` fires the rename listener for NAMES only). Called by the editor's tick
	/// on the frame the bridge's repaint flag is taken, which every type write and every restore sets;
	/// a pick in the slot's own menu has re-seated it already and this finds nothing to do. Returns how
	/// many slots moved.
	size_t syncMacroSlots();

	/// **REBUILD AND RE-LAY THE STRIP IF THE RACK NO LONGER MATCHES ITS CARDS** — a patch load, which
	/// replaces the rack under the strip and tells the editor only by the repaint flag. `FxLane::syncCards`
	/// is the same road for the same defect; called by the editor's tick beside `syncMacroSlots`. A card
	/// whose fold no longer matches the patch's counts too, since only a layout applies it. Returns how
	/// many cards were stale.
	size_t syncCards();

	/// AT `kCardWidth`, THE ONE WIDTH THIS STRIP HAS — the same question `bodyHeightOf` asks, for the
	/// same reason: a generic card's height is a function of its width, so a case reading this before
	/// a layout has placed the card would otherwise be told about a card zero pixels wide.
	float cardHeightForTest(size_t slot) const {
		return slot < cards_.size() && cards_[slot] != nullptr ? cards_[slot]->laneExtent(kCardWidth)
		                                                       : 0.0f;
	}
	const visage::Frame* cardFrameForTest(size_t slot) const {
		return slot < cards_.size() ? cards_[slot].get() : nullptr;
	}
	/// THE CARD ITSELF, for a case that drives its selection roads (backlog PN).
	FxCard* cardForTest(size_t slot) const { return slot < cards_.size() ? cards_[slot].get() : nullptr; }

private:
	/// See `lastSlotMenuForTest`. Written by `showSlotMenu` and read by nothing else in `src/`.
	size_t lastSlotMenu_ = kNoPosition;
	/// THE CLICK COUNT OF THE LAST PRESS — read by the fold in `focusOrArm`, which acts only on the
	/// SECOND. A single click on a card's head must not collapse it now that the head IS the target.
	int foldClicks_ = 0;
	/// **THE SUBSCRIPTIONS, HELD** — see `ui::Watch`. Declared last so they are released FIRST.
	std::vector<Watch> watches_;
	float scroll_ = 0.0f;

	/// THE DRAG, in three pieces because a press is not yet a drag — the sound rail's shape and its
	/// reasons. Below the threshold the gesture is still a click, so the head's own verbs keep
	/// working.
	/// Which DEVICE a modulation source belongs to, by its instance id — `kNoInstance` for a macro,
	/// for an empty slot, or for a source whose device has gone. Private because it is the fold
	/// set's key and nothing outside needs it.
	DeviceInstanceId instanceOfSource(backplane::Source source) const;

	std::optional<size_t> dragSlot_;
	std::optional<size_t> dropGap_;
	std::optional<float> dragAnchorY_;
	bool dragging_ = false;

	/// THE SELECTION, BY THE IDS `mod_rack.h` MINTS WHERE A ROW IS BUILT — see `selectSlot` and `selectionForTest`.
	/// It holds ids, never positions, for the reason the single selected id did: a removal slides every survivor.
	std::unique_ptr<LaneSelection> selection_;
	/// Its lasso and "N selected" pill, drawn above the cards (kept the panel's last child).
	std::unique_ptr<SelectionOverlay> selectionOverlay_;
	[[nodiscard]] std::vector<DeviceInstanceId> stripOrder() const;
	/// After a verb: the cards follow the chain and the selection is pruned.
	void afterVerb();
	/// A DROP OF THE CARD AT `from` IN `gap`: the whole selection when the card is in one of two or more, else the
	/// card alone. Both drag roads (the card's own and a folded head's) end here.
	void dropCard(size_t from, size_t gap);
	/// The id of the device at a rack position, or `kNoInstance` for a slot the rack does not hold.
	DeviceInstanceId instanceAtSlot(size_t slot) const;
	/// Tell every card whether it is the selected one — the flag `FxCard::setFocused` draws the outline
	/// from. Run after every layout, because a layout is what follows every change of position.
	void applySelection();
	/// **WHICH DEVICE CARDS ARE FOLDED IS NOT HELD HERE ANY MORE — IT IS THE BRIDGE'S, AND IT TRAVELS IN THE
	/// PATCH** (`ParamBridge::deviceFolded`). The set that stood here
	/// was right about its KEY and wrong about its HOME: an id is patch-scoped, so a panel-local set
	/// outlived the patch that gave those ids meaning, and the next patch's device 1 inherited the last
	/// one's fold. The history below is kept because the key is still the lesson.
	///
	/// IT WAS INDEXED BY SLOT AND THAT IS A BUG A PLAYER SEES. `removeRackDevice` shifts every row
	/// above the hole down one, so a flag pinned to a slot stays where it is while the devices move
	/// past it: the card the hand folded springs open and an untouched one folds itself. MEASURED on
	/// three MSEGs with the middle one folded — remove the first, or simply reorder, and both halves
	/// go wrong at once. The prediction was already one line under the removal that causes it:
	/// *"Every card from the hole down is now the face of a different device."*
	///
	/// AN INSTANCE ID IS THE RIGHT KEY AND THE FILE ALREADY MINTS ONE. `mod_rack.h` stamps
	/// `nextInstanceId` into the row where the device is BUILT and never reissues it — *"an undo puts
	/// back the device that was there, id and all, and the counter only ever climbs"* — which is
	/// exactly CLAUDE.md's remedy for an ABA key: contents and key move together by construction. A
	/// lane would have travelled with the row too, but a lane is a POOL HANDLE and the pool reissues,
	/// so the next device to take a freed lane would inherit the fold of the one that left.
	///
	/// THE RAIL'S OWN COLLAPSE REMAINS EDITOR-LOCAL and persisted on the desk (`editor_prefs.h`): it is a
	/// fact about a window, not about a device, and a preset that closed somebody's rail would be rude. A
	/// fold is the other kind — it belongs to a device, and a device only exists inside a patch.
	std::function<void(backplane::Source)> foldChanged_;
};

} // namespace feathers::ui
