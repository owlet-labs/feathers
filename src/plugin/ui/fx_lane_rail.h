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

#include "plugin/ui/chain_column.h"
#include "plugin/ui/fx_lane.h" // FxLane::kWidth — THE standard vertical column width (the mock)
#include "plugin/ui/rail.h"

#include <functional>
#include <span>
#include <memory>
#include <string>

namespace feathers::rack {
class InnerChain;
}

// **ONE FX LANE INSIDE A DEVICE, AS A RAIL ON ITS PANEL** — SKLITTER's GRAIN FX and POST FX rails, lifted out of
// `sklitter_expanded.h` (2026-10-05) so Resonate's DRONE FX and WET FX are the same widget rather than a copy
// (backlog LC: "extract the post fx pattern"). What a rail edits is now a `Spec` — its upright label, its caption,
// and the cast from the host's handle to the chain — and SKLITTER's two lanes are two `Spec`s (`Lane`), so
// nothing it does changed. The banner below is SKLITTER's own history of the widget and is kept as it was written.

namespace feathers::ui {

/// **RAILS 2 AND 3 — ONE LANE EACH. `GRAIN FX` and `POST FX`, side by side (2026-09-20).**
///
/// **A DESIGN DECISION, AND IT REPLACES THE STACKED PAIR THIS CLASS HELD FOR AN AFTERNOON**: a second
/// vertical tray, grain-fx to the left of post-fx. So the panel reads
/// `grid | PERFORM | GRAIN FX | POST FX`, and this is ONE class INSTANTIATED TWICE rather than one
/// class holding two columns.
///
/// **"TRAY" IS LOOSE AND IT MEANS `Rail`.** The fold-down `Tray` class was deleted when the circle
/// landed, because it did not match the trays in Feathers at all; nothing here revives it. Two
/// `Rail`s is also what makes the three properties below fall out for free rather than be engineered.
///
/// **WHAT THE SPLIT BUYS, MEASURED RATHER THAN ASSUMED:**
///  - **`kOpenMinimumHeight` IS 231, DOWN FROM 422** — one column's floor plus one caption, not two of
///    each. It was 255 before the grain lane had any column at all, so this is BELOW the number that
///    stood all the way back to the placeholder, because a rail that holds one lane needs one caption
///    rather than a caption and a foot line. **And the zero-margin problem goes with it**: 422 happened
///    to equal `CircleRail::kOpenMinimumHeight` exactly, leaving the *"opening the fx rail added no
///    height"* case passing at equality with nothing to spare. At 231 it clears by 191.
///  - **EACH LANE GETS THE FULL PANEL HEIGHT**, less its own chrome, instead of half. The equal-split
///    argument that `laneRoom()` carried — that 60/40 would need 25% more panel height to keep the
///    smaller lane above `ChainColumn::kMinimumUsefulHeight` — is **moot, and the function is gone**.
///  - **EACH RAIL COLLAPSES INDEPENDENTLY**, because `collapsed_` is per-`Rail` and `railWidth()` is
///    what `railsWidth()` sums. A player who wants only the grain lane pays 34px for the other.
///
/// **AND IT RETIRES "MASTER ON TOP READS UPSIDE DOWN".** The stacked version drew MASTER above GRAIN
/// while the signal runs grain → master, so the rail read backwards as a diagram and the captions had
/// to say the order in words. Left to right, GRAIN FX now sits before POST FX, which IS signal order —
/// see `kGrainLine`/`kPostLine` for what the captions kept and what the layout took over.
///
/// SKLITTER has held a real post-effects chain since `b6e312c3` — a `rack::InnerChain` on its summed
/// output, with a full `device::ContainerOps` table — and **nobody could put anything in it**, because
/// there was no editor anywhere that fits in a rail. That is bolt-on-chains gap **G5**: the plumbing
/// from a container's storage to an editor is generic (`DeviceHost::containerSurfaceOf`), and the only
/// editor over the far end, `RackEditor`, is 720×560 and runs its lane sideways. This rail holds a
/// `ChainColumn` instead — see `chain_column.h` for why that is a new widget rather than a refactor of
/// either existing lane.
///
/// **WHAT A PLAYER CAN DO HERE**: add a device to the chain (the (+) and the card menu's "Add
/// before…"), remove one, reorder it, fold its card, and turn its controls. The chain runs on the
/// summed output after SKLITTER's own dry/wet blend, it takes MODULATORS as well as effects (the
/// engine sorts by capability, not by chain — `2026-09-19-bolt-on-chains.md` §2), and it rides in the
/// patch, because `ContainerOps::saveState` was already wired and `patchOfRecord` already asks it.
///
/// **AND THE SECOND LANE IS A RAIL, NOT A SENTENCE.** EW is TWO lanes (2026-09-17: one for the grains
/// that get routed, and one master). The grain lane has been a real
/// `rack::InnerChain` in the engine since the kill-lanes work — fed grain by grain, and rejoining the
/// wet BEFORE the master lane sees it — while the fx rail still carried the line *"not built yet,
/// backlog EW"* — there because there was one rail with one column.
///
/// **WHAT FEEDS IT, EXACTLY, BECAUSE "all modes" IS NOT TRUE AND THE RAIL DOES NOT SAY SO.** There
/// are TWO divert sites and they are different decisions: `playLooper` diverts PER SLICE through
/// `grainSend_` (the six looper modes), and `playGrain` diverts PER VOICE through
/// `grainLaneA_`/`grainLaneB_`. **`playBurst` writes `sendL_`/`sendR_` nowhere** —
/// it has no plan to hang a per-grain decision on (backlog EW) — so in Burst the lane still RUNS and
/// is handed SILENCE. A device a player puts there is inaudible in one of the seven modes, and nothing
/// on this rail says which. **That is a known gap in what this surface
/// promises, recorded here rather than left to be discovered**; the same is true, for a different
/// reason, of `Send Override` at −100%, which hands the lane silence in every mode.
///
/// **THE TWO GAPS THE OLD LINE NAMED ARE BOTH CLOSED, AND THAT IS WHAT MADE THIS CHEAP.** **G2** (one
/// `saveState` for N bodies) is `rack::writeChainBundle`/`readChainBundle` over `lanesOf()`, with
/// `maxBodyBytes = rack::bundleMaxBytes(2)` — both lanes ride in one body, named by stable lane id.
/// **G3** (no lane argument on the route verbs) is `Sklitter::laneIds_`, ONE `InstanceCounter` shared
/// by both chains, so `laneHolding` can answer "which lane holds this id" from the id alone. So this
/// rail did not have to invent anything: it asks the host for the same handle, casts it twice, and
/// puts a `ChainColumn` over each.
///
/// **ONE `DeviceNest` FOR BOTH RAILS, AND THAT IS CORRECT RATHER THAN A SHORTCUT.** `innerNest()` is
/// `DeviceNest::under(nest(), watchedDevice())` — a function of the HOLDER, and both lanes have the
/// same holder, so there is exactly one answer to give. It is safe because `ExpandedOverlay::keyOf` is
/// `title#path:device` and the two lanes share G3's counter: no id exists in both, so no two panels
/// can key alike. **Had the counters been per-lane, this would be the SKLITTER-inside-SKLITTER bug
/// again** — opening a panel in one lane retiring a panel about a different device in the other.
/// **The split into two rails does not change this and must not be read as changing it**: two frames
/// is not two nests, and the key is still (chain, device) with one chain path for both.
///
/// **AND EACH RAIL OWNS ITS `ModState`, WHICH THE SPLIT MAKES STRUCTURAL RATHER THAN CAREFUL.** An arm
/// inside a chain is an offer to route, and a route runs through one `InnerChain`'s authority: a
/// post-lane control cannot read a grain-lane modulator's output, because the two chains are two
/// routing graphs. The stacked version had to hold two states side by side and remember to parent both;
/// one rail per lane means each has its own by construction, and the only shared thing is the OUTER
/// state both follow — which is the one source a control in either chain can genuinely route from.
/// **The `EditHistory` is now per-rail too**, and that IS a change: with two rails an undo belongs to
/// the surface you are working in, and the two surfaces are two rails.
///
/// **AND THE HOLDER'S OWN SEVEN OUTPUTS ARE ROUTABLE INSIDE THE CHAIN WITH NO WIDGET TO ARM THEM.**
/// `kPostChainAxes.coreSources = kPerformOutputs`, so `Perf X` and its six siblings reach every inner
/// control through the engine; nothing on this rail arms one. Nothing draws an arm either, so nothing
/// is promised — but it is the next thing this surface owes.
class FxLaneRail final : public Rail {
public:
	/// **WHICH OF SKLITTER'S TWO LANES THIS RAIL EDITS.** The only thing that differs between the two
	/// instances, and it decides three things together — the cast (`grainChainOf` vs `postChainOf`),
	/// the upright label, and the caption. Keeping them on ONE enum rather than on three constructor
	/// arguments is what stops a rail being built that says GRAIN FX over the post chain.
	enum class Lane : uint8_t { Grain, Post };

	/// **WHAT ANY HOLDER'S RAIL IS** (2026-10-05): the upright label, the one-line caption, and the cast from the
	/// handle the host files (`DeviceHost::containerSurfaceOf`) to this lane's chain — null when there is none.
	/// The holder's own outputs, published inward as core sources, are named by `coreSourceName` over the first
	/// `coreSources` of them (SKLITTER's seven `Perf` outputs); a holder that publishes none leaves both empty.
	struct Spec {
		const char* label = "";
		const char* caption = "";
		rack::InnerChain* (*chainOf)(void* surface) = nullptr;
		size_t coreSources = 0;
		std::string (*coreSourceName)(size_t index) = nullptr;
	};

	/// `registry` is what the cards are built through; `notifier` is where a refusal is said, and may be
	/// null (nothing here refuses silently — the (+) goes dim instead).
	FxLaneRail(const Spec& spec, WidgetRegistry& registry, Notifier* notifier) noexcept;
	/// SKLITTER'S TWO LANES, by name — `specFor(lane)`.
	FxLaneRail(Lane lane, WidgetRegistry& registry, Notifier* notifier) noexcept;
	[[nodiscard]] static Spec specFor(Lane lane) noexcept;
	~FxLaneRail() override;

	/// SKLITTER's lane, for a rail built from one (`Lane`); a rail built from a `Spec` has none and says Post.
	[[nodiscard]] Lane lane() const noexcept { return lane_; }
	[[nodiscard]] const Spec& spec() const noexcept { return spec_; }

	/// **READ THE HOST'S HANDLE FOR THE HOLDING DEVICE AND (RE)BUILD THE COLUMN OVER IT**, or drop it
	/// when there is none. Returns whether anything changed.
	///
	/// **THE HANDLE IS THE SAME FOR BOTH RAILS AND THE CHAIN IS NOT.** `containerSurfaceOf` files one
	/// pointer per INSTANCE, so each rail is handed that same pointer and casts it through its own
	/// lane's accessor. Two rails asking the host separately is still one handle; what makes them two
	/// editors is `chainFor()`, below.
	///
	/// **RE-ASKED EVERY TICK, AND THAT IS CLAUDE.md's ABA RULE RATHER THAN CAUTION.** The pointer a host
	/// files for a container is a CONTEMPORANEOUS HANDLE, not an identity: the converger replaces a
	/// container's whole device instance when a body arrives (bolt-on-chains trap **T9**), so an editor
	/// that remembered the pointer would be drawing a bridge inside a destroyed device.
	bool follow(void* surface);
	/// Advance the cards and say how many asked for a frame — the count produced BY the delivery.
	size_t updateCards();
	/// This lane's own modulation state, parented to the panel's. An outer source armed is what every
	/// inner control reads; a LOCAL arm stays in this lane, because a route cannot cross lanes.
	///
	/// **`holder` IS WHOSE OUTPUTS THIS CHAIN PUBLISHES INWARD, and it is why this takes a third
	/// argument** (backlog HM). The holder's own output `n` is not reachable in here by the outer road —
	/// `mayRoute` asks whether it is upstream of the holder, and a device is not upstream of itself
	/// (trap T1) — but it IS this chain's core source `n`, because SKLITTER declares
	/// `kPostChainAxes.coreSources == kPerformOutputs` and fills them with `publishCoreSource(n,
	/// performOutput(n))`. The rail states that mapping through `ModState::setOuterSpelling`; asked live
	/// rather than captured, for `setOutputSources`' reason — the holder's instance is replaced whenever
	/// a body arrives (trap T9).
	void setOuterModulation(ModState* mod, EditHistory* history, std::function<DeviceInstanceId()> holder);

	/// **WHERE A DEVICE IN THIS CHAIN OPENS ITS PANEL, AND WHICH CHAIN ITS CARDS BELONG TO.** Both are
	/// the PANEL's facts, not the rail's, so both arrive from `SklitterExpanded` — and both are
	/// remembered here rather than only pushed down, because `follow` rebuilds the column whenever the
	/// holding instance is replaced and a column built after the fact would otherwise come back with
	/// no way in and the host's own tint. That is trap **T9** wearing a different hat.
	void setPresentedOn(SurfaceLayers* host);
	/// A DROP OUTSIDE THIS LANE'S COLUMN, offered to the owner (`ChainColumn::setOnDropOutside`, backlog NQ) — handed to
	/// every column this rail builds, so a rebuild (trap T9) keeps it.
	void setOnDropOutside(std::function<bool(std::span<const DeviceInstanceId>, visage::Point)> f);
	/// **THE SAME NEST REACHES BOTH RAILS, and that is not a bug** — see the banner. The nest is the
	/// HOLDER's and the holder is one device; `keyOf` stays unique because the two lanes share one
	/// instance counter.
	void setNest(DeviceNest nest);

	/// Whether the rail currently holds a column over a running instance.
	[[nodiscard]] bool liveForTest() const noexcept { return column_ != nullptr; }
	/// TEST ONLY: the handle this rail follows (what `follow` was last handed), so a holder's case can say WHICH chain.
	[[nodiscard]] void* surfaceForTest() const noexcept { return surface_; }
	[[nodiscard]] ChainColumn* columnForTest() noexcept { return column_.get(); }
	/// **A PHANTOM RAIL** (rack stage 2, §2a): no chain behind it, but drawn as an EMPTY chain's — the (+) and what passes
	/// (`passes`) where the column would be — and its (+) calls `onAdd`, which makes the chain. A real empty chain and a
	/// phantom are then the same rail, collapsed or open.
	void setPhantom(std::string passes, std::function<void()> onAdd);
	[[nodiscard]] bool phantom() const noexcept { return static_cast<bool>(onPhantomAdd_); }
	[[nodiscard]] const std::string& phantomPassesForTest() const noexcept { return phantomPasses_; }
	/// Where a phantom's (+) is, in the rail.
	[[nodiscard]] visage::Bounds phantomAddBounds() const;
	/// The column over the running chain, or null — what a holder opens the chain's add menu on (rack stage 2's phantom
	/// rails, §2a: the zone that makes a chain then offers its devices).
	[[nodiscard]] ChainColumn* column() noexcept { return column_.get(); }
	[[nodiscard]] rack::RackBridge* bridgeForTest() noexcept { return bridge_; }
	/// **THIS LANE'S MODULATION STATE, SO A CASE CAN ASSERT THE LANES ARE SEPARATE.** The claim — an arm
	/// in one lane does not light the other — is about STATE and cannot be read off a drawing, which is
	/// CLAUDE.md's oldest rule.
	[[nodiscard]] ModState& modForTest() noexcept { return mod_; }
	/// The same state, for a holder that relays an arm made here out to its own host (a Rack's panel, backlog NZ (b)).
	[[nodiscard]] const ModState& modulation() const noexcept { return mod_; }

	void draw(visage::Canvas& canvas) override;
	void resized() override;

	/// **THE STANDARD VERTICAL COLUMN WIDTH, READ FROM THE LANE THAT DEFINES IT RATHER THAN RESTATED.**
	/// Open lanes are the standard vertical column width, as in Feathers. That standard is
	/// `FxLane::kWidth`, whose own comment traces it to the spec — *"The mock's `#fxStrip{width:256px}`"*, and `mock/feathers-ui-mock.html` is THE
	/// UI SPEC. The card inside it is `kCardUnitWidth` (216); the 40 between them is the lane's 10 a
	/// side plus chip room, so the two numbers are consistent.
	///
	/// **IT REPLACES A 300 THAT WAS NEVER DERIVED FROM ANYTHING.** That constant's own comment conceded
	/// it was *"chosen rather than derived ... not load-bearing"* — sized to the longest CAPTION, back
	/// when a caption had to spell out the signal order the layout now carries. A number sized to a
	/// sentence is what a number that should have come from the spec looks like in hindsight.
	///
	/// **AND IT IS AN ALIAS, NOT A COPY, SO IT CANNOT DRIFT FROM THE MOCK.** Restating `256.0f` here
	/// would be a second opinion about a generated token, which is the shape this repo has paid for
	/// repeatedly.
	///
	/// **MEASURED BEFORE IT WAS SET, because a narrower column is a narrower CARD and these rails hold
	/// cards through `ChainColumn` rather than through `FxLane`.** SKLITTER's own card is the worst
	/// case in the build — seven outputs, so the most chips to place. At 300 the card is 280 wide and
	/// its chips take 3 rows; at 256 it is 236 wide and they take 4, strip height 66 → 88. **Nothing is
	/// hidden at either width** (`outputChipBoundsFor` returns no zero-width chip), so the standard
	/// costs 22px of card height for a seven-output device and loses no control.
	static constexpr float kWidth = FxLane::kWidth;
	float expandedWidth() const override { return kWidth; }

	/// `FxLane`'s furniture, as SKLITTER's `CircleRail` has it — `sklitter_expanded.h` asserts the two agree.
	static constexpr float kPadX = 10.0f;
	static constexpr float kHeadingHeight = 16.0f;
	static constexpr float kGap = 10.0f;
	static constexpr float kLineHeight = 14.0f;
	/// The knob caption's size, not the rail's 20px upright one: a closed rail says ONE word in a 34px
	/// column and has the whole width for it, while this is a line of running text.
	static constexpr float kLineSize = 11.0f;

	/// **THE UPRIGHT LABELS, PUBLIC BECAUSE `Rail::kCollapsedLabelRun` HAS TO HOLD THE LONGEST ONE.**
	/// The two rails' names. Eight and seven glyphs, well inside the 160 that
	/// `MODULATORS` (140px at `kCollapsedLabelSize`) forced — but the case that walks every live rail
	/// and measures it through the font is what says so, not this comment.
	static constexpr const char* kGrainLabel = "GRAIN FX";
	static constexpr const char* kPostLabel = "POST FX";

	/// **ONE CAPTION PER RAIL, AND BOTH GOT SHORTER WHEN THE LAYOUT STARTED CARRYING THE ORDER.**
	///
	/// The stacked pair had to say where each lane sat in the signal *in words* — *"before MASTER"*,
	/// *"after the grain lane"* — because MASTER was drawn ABOVE GRAIN while the signal runs the other
	/// way. **Left to right, GRAIN FX now sits before POST FX, which IS the signal order**, so those
	/// clauses would be the layout said twice and they are gone.
	///
	/// **WHAT THE LAYOUT STILL CANNOT SAY, AND THE CAPTIONS THEREFORE KEEP:** adjacency is not flow. Two
	/// rails side by side read just as easily as two PARALLEL lanes, and the grain lane is a DIVERSION
	/// that rejoins (deliberately) — a chosen grain LEAVES the wet
	/// and comes back as the lane's output, so an empty grain lane is inaudible rather than a doubling.
	/// That is the one fact a player cannot get from the arrangement, and it is what each line now says.
	///
	/// `kNotRunning` is the ChainExpanded message, for the window between a panel opening and the device
	/// being provisioned.
	static constexpr const char* kGrainLine = "diverted per slice — rejoins the wet here";
	static constexpr const char* kPostLine = "the summed wet, grain lane included";
	static constexpr const char* kNotRunning = "not running here — close and reopen this panel";

	/// **THE SHORTEST THE PANEL MAY BE WHILE THIS RAIL IS OPEN** — ONE column floor
	/// (`ChainColumn::kMinimumUsefulHeight`) plus this rail's chrome: the heading, the caption, their
	/// gaps and the padding at both ends. Written as the vertical run in the order it is drawn, because
	/// it is the same arithmetic `columnBounds()` does in reverse.
	///
	/// **A FLOOR RATHER THAN A SIZE, AND ONLY WHILE OPEN**, exactly as `CircleRail::kOpenMinimumHeight`
	/// is: a rail is as tall as the panel, so this is what `SklitterExpanded::minimumSize` maxes
	/// against. A floor that applied while the rail was SHUT would make every SKLITTER panel taller for
	/// a rail nobody opened — a 34px strip holds no card.
	///
	/// **231, AND THE TWO NUMBERS IT REPLACES ARE BOTH WORTH KEEPING IN VIEW.** It was 422 with two
	/// stacked columns in one rail, and 255 before the grain lane had a column at all — so splitting
	/// into two rails lands BELOW the placeholder's own figure, because a rail with one lane needs one
	/// caption where the old one needed a caption and a foot line. **The margin against
	/// `CircleRail::kOpenMinimumHeight` (422) is what this buys**: the stacked version passed the
	/// *"opening the fx rail added no height"* case at EXACT equality, by coincidence of two unrelated
	/// sums, with nothing to spare for the next thing added. 191 is spare.
	static constexpr float kOpenMinimumHeight = kPadTop
	                                            + kHeadingHeight + kGap
	                                            + kLineHeight + kGap
	                                            + ChainColumn::kMinimumUsefulHeight
	                                            + kPadTop;

	[[nodiscard]] visage::Bounds headingBounds() const;
	/// Where this lane's caption is written, and where its column sits. Public for the reason
	/// `headingBounds` is: a case that restates a rectangle is checking its own arithmetic.
	[[nodiscard]] visage::Bounds captionBounds() const;
	[[nodiscard]] visage::Bounds columnBounds() const;

	/// **A ROW OF THE HOLDER'S OWN UNDER THE CAPTION** (2026-10-05, Resonate's `Drone to Wet FX` switch): `frame` is
	/// borrowed and becomes this rail's child, `height` tall, across the rail's padded width; the column starts
	/// below it. Null removes it.
	void setAccessory(visage::Frame* frame, float height);
	/// **THE HOLDER'S GATE ON IT** (the honesty rule): false hides it and gives its row back to the column. The rail's
	/// own collapse is separate and wins. Returns whether it changed.
	bool setAccessoryGate(bool shown);
	/// **THE ACCESSORY STAYS WHEN THE RAIL IS CLOSED** (backlog OC: the Rack's chain header is a channel strip that works
	/// collapsed): it then fills the closed column under the toggle (`accessoryBounds`), and the rail stops drawing its
	/// upright label — the accessory says the name. An EMPTY `Spec::caption` gives its row back, open or closed.
	void setAccessoryWhenCollapsed(bool shown);
	static constexpr float kCollapsedStripPadX = 3.0f;
	static constexpr float kCollapsedStripGap = 6.0f;
	[[nodiscard]] bool accessoryShownForTest() const { return accessoryShown(); }
	[[nodiscard]] visage::Bounds accessoryBounds() const;
	/// `kOpenMinimumHeight` plus the accessory's row while it shows — what the holder's minimum maxes against.
	[[nodiscard]] float openMinimumHeight() const;

	/// The caption this rail draws, from its `Spec`. Public so a case reads the rail's own answer
	/// rather than restating the mapping it is checking.
	[[nodiscard]] const char* captionForTest() const noexcept;

private:
	/// **THE CAST, BY SPEC.** The one place a lane's `chainOf` is called (SKLITTER's `grainChainOf` or `postChainOf`), so a
	/// rail cannot be built that draws one lane's cards under the other's label.
	[[nodiscard]] rack::RackBridge* bridgeIn(void* surface) const;

	Spec spec_;
	Lane lane_ = Lane::Post;
	WidgetRegistry& registry_;
	Notifier* notifier_ = nullptr;
	/// **THIS LANE'S OWN MODULATION AND UNDO.** A chain's assignments are the inner chain's, not
	/// SKLITTER's — `ChainExpanded` holds the same pair for the same reason — and `setParent` is what
	/// makes an OUTER source armed on the panel light an inner control.
	ModState mod_;
	EditHistory history_;
	/// The holder's history this lane's forwards to (backlog JB), and the binding.
	EditHistory* outerHistory_ = nullptr;
	void bindHistory();
	/// The handle the host last gave for the holding device, remembered ONLY so `follow` can tell a
	/// changed answer from an unchanged one. Never dereferenced across a tick — see `follow`.
	void* surface_ = nullptr;
	rack::RackBridge* bridge_ = nullptr;
	/// THE TWO FACTS A REBUILT COLUMN HAS TO BE TOLD AGAIN — see `setPresentedOn`.
	SurfaceLayers* presenter_ = nullptr;
	std::function<bool(std::span<const DeviceInstanceId>, visage::Point)> onDropOutside_;
	DeviceNest nest_ {};
	std::unique_ptr<ChainColumn> column_;
	visage::Frame* accessory_ = nullptr;
	float accessoryHeight_ = 0.0f;
	bool accessoryGate_ = true;
	bool accessoryWhenCollapsed_ = false;
	/// The y below the heading where the next row starts: under the caption, or under the heading when there is none.
	[[nodiscard]] float belowHeading() const;
	void collapsedChanged() override;
	/// Whether the accessory takes a row now: set, and not hidden by its holder.
	[[nodiscard]] bool accessoryShown() const;
	std::string phantomPasses_;
	std::function<void()> onPhantomAdd_;
	void dropColumn();
};

} // namespace feathers::ui
