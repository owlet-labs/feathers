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

#include "backplane/container_bodies.h"
#include "backplane/graph.h"
#include "rack/chain_record.h"
#include "rack/rack_state.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// **SOME OF A CHAIN'S DEVICES, LIFTED OUT AND PUT BACK** — backlog OA.
//
// Copy, cut, paste, duplicate, remove-many, Group into Chain and Group into Rack are each one LIFT and one PLACE
// of a FRAGMENT: a `RackPatch` holding some devices of one chain (rows by key, labels, view, body, site, keyed
// blocks) and every route that touches them. It is the patch format both bridges already save, so a fragment is
// what a chain's own body is, and the verbs below are written once for Feathers' fx lane and every chain the rack
// code holds (the Chain plugin, a Chain or Rack window, a site lane, SKLITTER's and Resonate's rails).
//
// **IDS.** A device's id is its address for every route and every host slot. A paste or a duplicate takes FRESH
// ids from the target's counter; an undo puts a device back under the id it HAD (`PlaceIds::Keep`), which is what
// lets the routes, labels and host slots that named it name it again. Ids are per holder (`InnerChain`, gap G3),
// so a container's body is its own id space and is copied verbatim — except where it names THIS record's ids,
// which is the `Outer` road (rack roadmap §7): those references are re-pointed with the devices or dropped and
// counted, never left to name whatever wears the id at the far end.
//
// **ROUTES.** Among the fragment's devices: kept and re-pointed. Into them from a source outside (`Inbound`): kept
// on a paste only into the record they were lifted from, and only where the record's gate still admits them.
// Out of them to a device outside (`Outbound`): not copied — a duplicated LFO does not double-modulate its old
// target (D3) — but an undo puts them back. A group keeps what the gates allow and COUNTS what they do not.

namespace feathers::rack {

/// A FRESH SERIAL NAMING ONE HOST, for "was this lifted from the record I am pasting into" — never an address
/// (CLAUDE.md: an address is not an identity). Process-wide, so two bridges never share one.
[[nodiscard]] uint64_t mintFragmentOrigin() noexcept;

/// **WHAT A BRIDGE HANDS THE FRAGMENT VERBS.** Both bridges implement it over the same five things they already
/// save (`patchOfRecord`): the record, its bodies, its counter, its views and its labels.
class FragmentSite {
public:
	virtual ~FragmentSite() = default;
	[[nodiscard]] virtual ChainRecord& fragmentRecord() = 0;
	/// WHICH OF THE RECORD'S CHAINS a hand edits here (Feathers' fx lane is chain 1; every rack bridge has one).
	[[nodiscard]] virtual size_t fragmentChain() const = 0;
	[[nodiscard]] virtual ContainerBodies& fragmentBodies() = 0;
	[[nodiscard]] virtual backplane::InstanceCounter& fragmentIds() = 0;
	[[nodiscard]] virtual CoreCodec fragmentCodec() const = 0;
	[[nodiscard]] virtual DeviceViews* fragmentViews() = 0;
	[[nodiscard]] virtual DeviceLabels* fragmentLabels() = 0;
	/// THE RECORD CAUGHT UP with what its devices changed themselves (backlog MX), before a lift reads it.
	virtual void fragmentCatchUp() = 0;
	/// HOST SLOTS under the device's path: freed (with a note), restored from the note, or moved one level down
	/// into `container` (or back up out of it, `into == false`). A bridge with no slot road answers nothing.
	virtual void fragmentFreeSlots(DeviceInstanceId device) = 0;
	virtual void fragmentRestoreSlots(DeviceInstanceId device) = 0;
	virtual bool fragmentRebaseSlots(DeviceInstanceId device, DeviceInstanceId container, bool into) = 0;
	/// AFTER A VERB THAT CHANGED SOMETHING: publish, mark the host dirty, and tell every listener whose output
	/// label moved since `labelsBefore`.
	virtual void fragmentChanged(const DeviceLabels& labelsBefore) = 0;
	[[nodiscard]] virtual uint64_t fragmentOrigin() const = 0;
	/// EXPIRES WITH THE BRIDGE — an undo record holds this, so a step over a chain that has gone does nothing.
	[[nodiscard]] virtual std::weak_ptr<const void> fragmentLife() const = 0;
	/// WHETHER THIS CHAIN RUNS THE HOST'S VOICES — a Group into Rack here reports the Poly devices it moves.
	[[nodiscard]] virtual bool fragmentHostsVoices() const { return false; }
	/// **A COPY HERE KEEPS ITS OWN ROUTES OUT** (backlog PN, Feathers' modulator strip; TO BE CONFIRMED): a paste or a
	/// duplicate placed in this chain, from this record, carries the routes leading OUT of the copied devices to
	/// whatever they reached, re-pointed to the copies — so a duplicated LFO modulates what its original does. Off
	/// everywhere else, where D3 holds (a copy does not reach what the original reached). A modulator's routes out are
	/// the whole of what it does; a copy without them is a card that moves nothing.
	[[nodiscard]] virtual bool fragmentCopiesOutbound() const { return false; }
	/// WHETHER GROUP INTO CHAIN / RACK IS OFFERED HERE. The modulator strip holds no containers: it says no.
	[[nodiscard]] virtual bool fragmentGroups() const { return true; }
};

/// **THE DEVICES, IN CHAIN ORDER, AND EVERY ROUTE THAT TOUCHES ANY OF THEM.** `positions[i]` is where
/// `patch.device[i]` stood when lifted; `origin` is the site it was lifted from.
struct Fragment {
	RackPatch patch;
	std::vector<size_t> positions;
	uint64_t origin = 0;

	[[nodiscard]] bool empty() const noexcept { return patch.device.empty(); }
	[[nodiscard]] size_t size() const noexcept { return patch.device.size(); }
	[[nodiscard]] bool names(DeviceInstanceId id) const noexcept;
	[[nodiscard]] std::vector<DeviceInstanceId> ids() const;
};

/// WHERE A ROUTE STANDS AGAINST A FRAGMENT (D3).
enum class RouteClass : uint8_t { Among, Inbound, Outbound };
[[nodiscard]] RouteClass classify(const Fragment& f, const RackPatch::Route& route) noexcept;

/// **LIFT** — `ids` that the site's chain holds, in CHAIN order (an id it does not hold is skipped).
[[nodiscard]] Fragment liftFragment(FragmentSite& site, std::span<const DeviceInstanceId> ids);

/// THE PASTEBOARD'S TEXT (D5, stage 1 holds it in process): `{"feathersClip":1,…}` around the patch — keys, not
/// columns, so it reads back in another build. The origin is NOT in it: a clip read from text is from nowhere,
/// so its inbound routes are dropped wherever it lands.
[[nodiscard]] std::string clipText(const Fragment& f);
[[nodiscard]] std::optional<Fragment> fragmentOfClipText(std::string_view text);

/// **THE PASTEBOARD — ONE PER LOADED BINARY** (D5, stage 1): every editor in this plugin binary shares it. It holds the
/// clip as TEXT and, beside it, the origin it was lifted from, so a paste back into that record keeps its inbound
/// routes. Message thread.
void setPasteboard(const Fragment& f);
[[nodiscard]] std::optional<Fragment> pasteboard();
[[nodiscard]] bool pasteboardHolds() noexcept;
void clearPasteboard() noexcept;

/// **THE OS CLIPBOARD, BEHIND A SEAM** (backlog OA). The pasteboard above is in process; this is the road out of it.
/// `write` puts the clip's text on the operating system's clipboard when a copy or cut is made; `read` answers what the
/// system's clipboard holds now. Both are installed by the editor that owns a window (`ui::bindOsClipboard`) and are
/// empty otherwise — a headless case, a host with no display — in which case the pasteboard is exactly what it was.
/// Message thread. `read` may block briefly (the X11 owner is asked), so it is called only on a paste and when the
/// in-process board is empty and a menu asks whether paste is offered.
struct OsClipboard {
	std::function<std::string()> read;
	std::function<void(const std::string&)> write;
};
void setOsClipboard(OsClipboard port);
void clearOsClipboard() noexcept;
/// Whether `text` is a clip of ours (the header alone; `fragmentOfClipText` is the full parse).
[[nodiscard]] bool isClipText(std::string_view text) noexcept;

enum class PlaceIds : uint8_t {
	/// Under the ids the fragment names — an undo's put-back. A device whose id the record already holds is refused.
	Keep,
	/// Under fresh ids from the site's counter — a paste or a duplicate.
	Fresh,
};

struct PlaceResult {
	/// What landed, in chain order, under the ids they wear.
	std::vector<DeviceInstanceId> placed;
	/// (the id the fragment named, the id it wears).
	std::vector<std::pair<DeviceInstanceId, DeviceInstanceId>> worn;
	/// Devices the chain refused (full, unknown chain, or an id it holds under `Keep`).
	size_t devicesRefused = 0;
	/// Routes not carried that a hand would expect to be: inbound into another record, and what the gate refused.
	size_t routesDropped = 0;
	/// Outbound routes left behind on a paste — by design (D3), so not a loss to report.
	size_t outboundLeft = 0;
	/// References inside a body or a site to the source record's devices that could not be re-pointed.
	size_t innerRoutesDropped = 0;
	/// REFUSED WHOLE BY THE DEPTH GATE (`fitsNesting`): the fragment stands too tall for this chain; nothing placed.
	bool tooDeep = false;
	LoadReport report;
};

/// `position` for "where each stood" — `Fragment::positions`, in ascending order (an undo's put-back).
inline constexpr size_t kOriginalPositions = static_cast<size_t>(-1);

/// **WHETHER THE FRAGMENT FITS THIS CHAIN'S DEPTH** — the depth gate (`fitsNesting`) as `placeFragment` asks it, for
/// a caller that must know before it changes anything (a drag lifts from one chain before it places in another).
[[nodiscard]] bool fragmentFits(FragmentSite& site, const Fragment& f);

/// **PLACE** the fragment at `position` in the site's chain (clamped to its end), or at `kOriginalPositions`.
/// `outbound` carries the routes out of the fragment (an undo); a paste passes false. Publishes and marks dirty.
PlaceResult placeFragment(FragmentSite& site, const Fragment& f, size_t position, PlaceIds ids, bool outbound);

/// **REMOVE** — every device of `ids` the chain holds, with its body, site, labels, view and routes; its host slots
/// freed with a note, so `placeFragment(Keep)` of a fragment lifted first gets them back. Returns how many went.
size_t removeFragmentDevices(FragmentSite& site, std::span<const DeviceInstanceId> ids);

//------------------------------------------------------------------------------------------------//
// GROUP                                                                                            //
//------------------------------------------------------------------------------------------------//

enum class GroupKind : uint8_t { Chain, Rack };

/// WHY THIS SELECTION CANNOT BE GROUPED, in words for a tooltip — empty when it can. Refused: nothing selected, or
/// an audio device the selection does not hold standing between two it does (D4: grouping would move it).
[[nodiscard]] std::string groupRefusal(FragmentSite& site, std::span<const DeviceInstanceId> ids);

/// THE GROUP, AS DATA: what replaces the selection. Built once (`planGroup`) and applied by the verb and its redo,
/// so a redo puts back the same container under the same id with the same body.
struct GroupPlan {
	GroupKind kind = GroupKind::Chain;
	/// The container's id, taken from the site's counter when planned — so a redo puts it back under the same one.
	DeviceInstanceId containerId = kNoInstance;
	/// Where it stands: where the first moved device stood.
	size_t position = 0;
	/// The selection, in chain order, keeping its ids inside (the group is a new id space).
	std::vector<DeviceInstanceId> moved;
	/// The group's chain: the moved devices and the routes among them, their bodies re-pointed.
	RackPatch inner;
	/// Routes from a source that stays outside, spelled as the group's `Outer` source — kept by `applyGroup` only
	/// where the gate admits them with the group in place.
	std::vector<std::pair<backplane::Source, RackPatch::Route>> inbound;
	/// Routes from a moved modulator to a device that stays outside, spelled as the group's EXPORT (backlog NZ (b):
	/// `inner.exports` names the rows) — made by `applyGroup` only where the gate admits them with the group in place.
	std::vector<backplane::Route> outbound;
	/// What the selection was, every route with it: the undo's put-back.
	Fragment before;
	size_t routesLost = 0;  ///< routes no boundary can carry (outbound; two levels out), found by the plan
	size_t inboundLost = 0; ///< inbound and outbound routes the gate refused, found by the apply
	std::string firstLost;  ///< the first loss, in words
	size_t polyOnBus = 0;   ///< Group into Rack in a voice chain: moved devices with Poly on (they run on the bus)
	size_t slotsLost = 0;   ///< host slots that could not move down with their device

	[[nodiscard]] size_t lost() const noexcept { return routesLost + inboundLost; }
};

/// **PLAN** — refused (nullopt) where `groupRefusal` says so. Changes nothing but the container's id, taken here.
[[nodiscard]] std::optional<GroupPlan> planGroup(FragmentSite& site, std::span<const DeviceInstanceId> ids, GroupKind kind);

/// **APPLY** — the moved devices leave, the container arrives where the first of them stood, the inbound routes the
/// gate admits go inside it, their host slots move down under it. Returns the container's id, or `kNoInstance`
/// (nothing changed: the record is not the one the plan was made over). Fills the plan's apply-time counts.
DeviceInstanceId applyGroup(FragmentSite& site, GroupPlan& plan);

/// **THE UNDO OF A GROUP** — the slots move back up, the container goes, and `before` (lifted before the group) is
/// put back where it stood, under its own ids, every route with it.
bool undoGroup(FragmentSite& site, DeviceInstanceId container, const Fragment& before);

//------------------------------------------------------------------------------------------------//
// MOVE AS A BLOCK                                                                                  //
//------------------------------------------------------------------------------------------------//

/// **THE SELECTION MOVES TO `gap`** (0 = before the first device, length = after the last), in order and
/// contiguous. The routes the new order forbids go. False when nothing moved.
bool moveBlock(FragmentSite& site, std::span<const DeviceInstanceId> ids, size_t gap);

/// THE CHAIN PUT BACK IN THIS ORDER (an undo of `moveBlock`), and the routes `routes` reapplied through the gate.
bool restoreOrder(FragmentSite& site, std::span<const DeviceInstanceId> order, std::span<const backplane::Route> routes);

/// EVERY ROUTE TOUCHING `ids`, as the record holds them — what a block move may delete and its undo puts back.
[[nodiscard]] std::vector<backplane::Route> routesTouching(FragmentSite& site, std::span<const DeviceInstanceId> ids);

//------------------------------------------------------------------------------------------------//
// A BODY'S REFERENCES TO ITS HOLDER'S RECORD                                                       //
//------------------------------------------------------------------------------------------------//

/// What becomes of one `Outer` source a body names: the same, another, or nothing (dropped). `level` is how many hosts in
/// from the body's holder the route's own chain is — 1 for the body's own chains, 2 inside a container one of them holds,
/// and so on — so the source reaches the holder's record exactly when `outer.outerHops() == level`, stays inside the
/// body when fewer, and reaches past the holder when more (sources at any depth).
using OuterRemap = std::optional<backplane::Source> (*)(void* ctx, backplane::Source outer, uint32_t level);

/// **EVERY `Outer` SOURCE IN A CONTAINER'S BODY OR A SITE, PASSED THROUGH `remap`, AT EVERY DEPTH** — the four framings
/// the rack code writes (a chain's document, a bundle of lanes, a site, a Rack's chains), and the bodies and sites of the
/// devices inside them, recursively, each at its own `level`. `dropped` counts what `remap` refused. Returns the bytes
/// unchanged when nothing changed, and nullopt for bytes no framing here recognises (a nested body no framing knows —
/// a leaf's own body — is left as it is).
[[nodiscard]] std::optional<std::vector<uint8_t>> remapOuterSources(std::span<const uint8_t> body, OuterRemap remap,
                                                                    void* ctx, size_t* dropped);

/// **A CONTAINER'S BODY AS A PRESET HOLDS IT** (backlog OU) — a
/// chain's document, or a Rack's body chain by chain, with every route from an `Outer` source and every EXPORT row
/// gone: the two halves of a route across the saved container's edge. AT EVERY DEPTH: a nested body's `Outer` route stays
/// while it names something inside the preset (fewer hops than its depth) and goes when it reaches past the preset. `dropped` adds what went (routes and rows). The
/// bytes unchanged when nothing went; nullopt for bytes that are neither framing.
[[nodiscard]] std::optional<std::vector<uint8_t>> withoutOutsideRoutes(std::span<const uint8_t> body, size_t* dropped);

} // namespace feathers::rack
