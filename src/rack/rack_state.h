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

#include "backplane/graph.h"
#include "backplane/route_list.h"
#include "clap/stream.h"
#include "backplane/container_bodies.h"
#include "backplane/preset_identity.h"
#include "rack/chain_record.h"
#include "rack/rack_engine.h"

#include "device/macro_type.h"
#include "device/registry.h"
#include "hostslots/slot_pool.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// THE PATCH — ONE FORMAT FOR THE LANE, THE STANDALONE RACK AND A RACK IN A RACK (chain-as-a-device,
// item 6). What devices there are, in which chain and order, what each is set to, and what is routed
// where. Chosen 2026-09-11 between the two formats that existed: Feathers' encoding.
//
// THE FOUR PROPERTIES THAT MATTER, each paid for in `audition.cpp` before this file shared them:
//   - EVERY COUNT IS WRITTEN AS DATA, never assumed from this build's constants.
//   - A DEVICE RECORD IS LENGTH-PREFIXED, so a reader walks past a device it does not know.
//   - A REFUSAL IS SAID, not swallowed: a route the rules drop is counted and the first is named.
//   - A DEVICE THIS BUILD CANNOT RUN IS KEPT, in its place, and saved back out.
//
//------------------------------------------------------------------------------------------------//
// A ROUTE NAMES WHAT IT JOINS BY IDENTITY, NOT BY WHERE IT STANDS                                  //
//------------------------------------------------------------------------------------------------//
//
// Feathers' encoding, and the reason it was the one kept: a device end is its INSTANCE ID and its
// output or CONTROL INDEX; a host end is a kind and an index the host itself names — a macro, a weight,
// a parameter's stable id. Each survives what the rack's old (lane, ordinal, raw index) did not: a device
// loading at another lane, a control's modulatable flag changing (which renumbers ordinals), a host's
// parameter list being reordered. A route naming something that is not there is DROPPED AND COUNTED,
// never moved onto a neighbour.
//
// A HOST'S OWN COORDINATES ARE THE HOST'S TO NAME (`CoreCodec`): the rack names its macros by index;
// Feathers names macros, weights and parameters (by `ParamId`). Device ends are named here, the same for
// every host, by the record or engine that holds the devices.

namespace feathers::rack {

class RackEngine;

/// WHAT KIND OF THING A ROUTE'S SOURCE IS — Feathers' kinds, fixed for every host.
/// `Outer` (rack roadmap §7): a source of the host OUTSIDE this rack, as an inner route names it — `a` is
/// the outer device id (`kNoInstance` for one of the outer host's own), `b` the index WITH the scope bit,
/// exactly as `backplane::Source` holds it, so the file and the record spell it the same way.
/// `Stack` (backlog KE step 3): Feathers' Stack Index, a per-part source of a stacked voice; `a` is which (0).
/// `Export` (backlog NZ): a source in a device's PRE chain, as the host names it — `a` the holder's id, `b` the row of
/// its site's export table WITH the scope bit, as `backplane::Source::exported` holds it.
/// `ContainerExport` (backlog NZ (b); its own kind by design): a source INSIDE a Chain or a Rack, as
/// its host names it — `a` the container's id, `b` the row of the container's export table WITH its own scope bit, as
/// `backplane::Source::containerExported` holds it. A Rack may hold a site too, so the two tables' rows are two kinds.
enum class SourceKind : uint32_t { Macro = 0, Weight = 1, Device = 2, Outer = 3, Stack = 4, Export = 5, ContainerExport = 6 };
/// Either export kind — a holder's id in `a` that a placement translates as it does a device's.
[[nodiscard]] constexpr bool isExportKind(uint32_t kind) noexcept {
	return kind == static_cast<uint32_t>(SourceKind::Export) || kind == static_cast<uint32_t>(SourceKind::ContainerExport);
}
/// And its destination.
///
/// `Internal` (2026-09-21) IS A HOST'S OWN DESTINATION THAT IS NOT A PARAMETER — Feathers' core grew a
/// second region of its destination table (a sound's travel Radius and Theta, `plugin/params.h`) that
/// spends no CLAP id. It is a HOST end like `Param`: `a` is whatever stable id the host's `CoreCodec`
/// gives it and `b` is zero. A host that has none — the rack — refuses it exactly as it refuses any
/// kind it does not know, which is the codec's existing rule and needs no change here.
enum class DestKind : uint32_t { Param = 0, Device = 1, Internal = 2 };

/// ONE END OF A ROUTE, AS THE FILE HOLDS IT. `kind` is a `SourceKind` or `DestKind`. **A DEVICE END IS ITS
/// INSTANCE ID (`a`) AND A KEY** — the output's `SourceDesc::key` or the control's `ParamDesc::key` (backlog
/// JF §3.8): never an index, because an index is a position and a control inserted above it would re-point
/// the route. `b` is unused for a device end. For a host end, `a` is the index or id the host's
/// `CoreCodec` gave it and `b` is zero; for an `Outer` source, `a` and `b` are the outer host's id and
/// scoped index, as `backplane::Source` holds them.
struct PatchCoord {
	uint32_t kind = 0;
	uint32_t a = 0;
	uint32_t b = 0;
	std::string key;

	friend bool operator==(const PatchCoord&, const PatchCoord&) = default;
};

/// **WHAT A LOAD REFUSED** — the count, and the first refusal's two ends BY IDENTITY (kill-lanes step 3b):
/// a device end is the device and its own output or CONTROL index, a host end the host's own index. A
/// summary rather than a list: a tray is one line.
///
/// **IT HELD COORDINATES AND THAT IS WHAT CHANGED**. A
/// coordinate carries a POOL SLOT, so every reader of a refusal had to invert a host's private layout to
/// say what it was about — two such inversions grew independently in one day, one here and one in
/// `ParamBridge`, which is the signal that the thing itself should move rather than that each reader
/// should work around it. A reader now asks the record it already has: `holds(id)` says whether the device
/// is still here, `positionOf(id)` where it stands, and the control index is the device's own name for the
/// control rather than the modulatable ordinal a column packed.
///
/// **THESE ENDS ARE WHAT THE FILE ASKED FOR, NOT WHAT THE LOOKUP FOUND** — by design, and it is the
/// half that makes the shape work. A coordinate had a natural "nothing": one past the axis. AN ENDPOINT
/// HAS NONE — `Destination {}` is `{kNoInstance, 0}`, which is a REAL host destination (the host's first
/// parameter), so a default-constructed end is not "nowhere" but "core 0", and no sentinel may be invented
/// from that space. So a device end is reported as the patch named it,
/// `Destination::control(theIdTheFileNamed, theControlTheFileNamed)`, whether or not it resolved: a reader
/// can then say *"this route named device X, which is not here"* instead of *"this route named nothing"*,
/// which is both more use and the honest claim — the truth about the file, rather than the truth about our
/// lookup. **WHERE THE LOOKUP SUCCEEDS THE TWO IDS ARE THE SAME ANYWAY**: a device is renamed only when its
/// file id was already taken, which is exactly the duplicate that makes the id resolve to nothing, so a
/// resolving id is never a renamed one.
///
/// THE ONE END THAT CANNOT BE SPELLED AS AN ENDPOINT AT ALL is a host end whose kind the codec refuses
/// outright — there is no index to name — and a device end the file wrote under `kNoInstance`, which names
/// no device and whose endpoint would read as a host end. Both are `nobody()`, the value that already
/// exists for "an end no route can name".
struct RouteRefusals {
	size_t count = 0;
	backplane::Source firstSource = backplane::Source::nobody();
	backplane::Destination firstDestination = backplane::Destination::nobody();

	friend bool operator==(const RouteRefusals&, const RouteRefusals&) = default;
};

//------------------------------------------------------------------------------------------------//
// THE LOAD REPORT (backlog JF §3.13 —)                              //
//------------------------------------------------------------------------------------------------//
//
// **WHAT A LOAD DID NOT DO AS SAVED, SAID.** It generalises `RouteRefusals` and rides exactly its road:
// built during the load on the message thread, latched on the bridge until an editor exists, and said as
// ONE tray line naming the most serious kind in full and counting the rest (`loadReportLine`). A later load
// replaces it: it describes the patch now in front of the user. The details panel is later work (§5 step 4).
//
// **A VALUE, NOT A LOG**: a count and the first example of each kind, so a case can assert it and a tray
// can say it without either inferring anything.

struct LoadReport {
	/// IN ORDER OF SERIOUSNESS — the headline is the first kind with a count.
	enum class Kind : uint8_t {
		NotLoaded,     ///< malformed, a wrong format or major, the old binary format: nothing changed
		RouteDropped,  ///< `routes` below, as `RouteRefusals` always said it
		/// A `hostSlots` member that could not be read: dropped whole, the rest of the patch loads. SECOND ON
		/// PURPOSE, above every device-level kind: the slot map is invisible — no card shows it, and nothing
		/// sounds different when it is lost — while its loss re-points the host's lanes. So it must be said
		/// loudly, or it is not said at all.
		SlotsDropped,
		DeviceUnknown, ///< a device this build lacks: a placeholder keeps its place and its values
		BodyRefused,   ///< a body the device would not take
		NewerSchema,   ///< a device saved by a newer build: known values applied, the rest carried
		KeyUnknown,    ///< a value this build has no control for: carried, not applied
		KeyRetired,    ///< a value whose meaning changed: dropped, with the retirement's reason
		LabelUnknown,  ///< a Choice label this build lacks: that control at its default
		ValueCoerced,  ///< null, not a number, out of range, or extra state of the wrong length
		KeyMissing,    ///< the patch predates a control: at its default
	};
	static constexpr size_t kKinds = static_cast<size_t>(Kind::KeyMissing) + 1;

	struct First {
		std::string where; ///< the device's card name and its place ("Featherverb, chain 2 #1"), or "patch"
		std::string what;  ///< the key, label or tag
		std::string why;   ///< the retirement reason, the schema numbers, the refusal

		friend bool operator==(const First&, const First&) = default;
	};

	std::array<uint32_t, kKinds> count {};
	std::array<First, kKinds> first {};
	/// THE DROPPED ROUTES BY IDENTITY, which a host names in its own vocabulary — `count[RouteDropped]` is
	/// `routes.count`.
	RouteRefusals routes;

	void note(Kind kind, std::string where, std::string what, std::string why = {}) {
		const size_t k = static_cast<size_t>(kind);
		if (count[k]++ == 0) {
			first[k] = First { std::move(where), std::move(what), std::move(why) };
		}
	}
	void noteRoute(backplane::Source source, backplane::Destination destination) {
		if (routes.count == 0) {
			routes.firstSource = source;
			routes.firstDestination = destination;
		}
		++routes.count;
		++count[static_cast<size_t>(Kind::RouteDropped)];
	}
	/// Another report's items after this one's — a nested load, or route drops a service found.
	void merge(const LoadReport& other);
	void mergeRoutes(const RouteRefusals& other);

	[[nodiscard]] uint32_t countOf(Kind kind) const noexcept { return count[static_cast<size_t>(kind)]; }
	[[nodiscard]] const First& firstOf(Kind kind) const noexcept { return first[static_cast<size_t>(kind)]; }
	[[nodiscard]] bool empty() const noexcept {
		for (const uint32_t c : count) {
			if (c != 0) {
				return false;
			}
		}
		return true;
	}

	friend bool operator==(const LoadReport&, const LoadReport&) = default;
};

/// **THE TRAY LINE** — the most serious kind in full, the rest counted. `routePhrase` is the host's own
/// naming of `routes`' first refusal ("LFO -> Filter Cutoff"), used when routes are the headline. Empty for
/// an empty report.
std::string loadReportLine(const LoadReport& report, const std::string& routePhrase);

/// **EVERY KIND, COUNTED, WITH ITS FIRST EXAMPLE** — for the event log (plugin/event_log.h), which has room the
/// tray does not: `key-missing=3 [Featherverb, chain 2 #1 | size]`. Empty for an empty report.
std::string loadReportDetail(const LoadReport& report);

//------------------------------------------------------------------------------------------------//
// VIEW STATE TRAVELS WITH THE DEVICE            //
//------------------------------------------------------------------------------------------------//
//
// **A FOLD IS THE ONE PIECE OF VIEW STATE THAT COULD NOT LIVE ON THE DESK, AND THE REASON IS IDENTITY.**
// `editor_prefs.h` keeps how somebody's desk is arranged — which rails are closed — and its banner says
// none of it may reach a patch, because a preset that rearranged the loader's editor would be rude. A
// FOLD is not that kind of fact: it belongs to a DEVICE, and a device only exists inside a patch. The
// desk file could key it by position (a reorder then moves the fold onto another card, which is the
// defect this replaces) and cannot key it by instance id, because ids are patch-scoped and two patches
// both hold a device 1 — a desk-level key would share one patch's fold with another's.
//
// So the fold rides with its device, and the rails stay on the desk. **That is the line: state ABOUT A
// DEVICE goes in the patch; state about the window does not.**
//
// A BIT FIELD RATHER THAN A BOOL, so a second view fact — a card's chosen height, a pinned control — is
// a bit rather than a format bump. One flag is defined; the rest are zero and are written as zero.

/// A CARD KEPT AS ITS HEAD ROW ALONE (`FxCard::setFolded`, `ModPanel::cardMinimised`).
inline constexpr uint32_t kViewFolded = 1u << 0;

/// **VIEW STATE BY DEVICE, AS THE PATCH CARRIES IT** — the same shape and the same thread rule as
/// `ContainerBodies`: keyed by instance id, filled by a load, read by a save, MESSAGE THREAD ONLY, and
/// never published to the audio thread. It is emphatically NOT part of `ChainState`, which crosses to the
/// engine every edit: a fold changes no sound and must not cost a publish the block has to read.
///
/// A VECTOR OF PAIRS BECAUSE IT IS AS LONG AS THE CHAINS ARE — four to sixteen entries, scanned linearly,
/// which is free at that size and allocates once. `flagsOf` answers zero for a device nobody has folded,
/// so "not stored" and "stored unfolded" are the same answer, which is what a caller wants here: the
/// question is never "did somebody fold this once" but "is it folded".
class DeviceViews {
public:
	[[nodiscard]] uint32_t flagsOf(DeviceInstanceId device) const noexcept {
		for (const auto& [id, flags] : held_) {
			if (id == device) {
				return flags;
			}
		}
		return 0;
	}

	/// Zero ERASES rather than storing a zero, so a patch carries no entry for a card nobody folded and
	/// the map cannot grow with every device that has ever been unfolded.
	void set(DeviceInstanceId device, uint32_t flags) {
		if (device == kNoInstance) {
			return; // addresses nothing
		}
		for (auto it = held_.begin(); it != held_.end(); ++it) {
			if (it->first == device) {
				if (flags == 0) {
					held_.erase(it);
				}
				else {
					it->second = flags;
				}
				return;
			}
		}
		if (flags != 0) {
			held_.emplace_back(device, flags);
		}
	}

	void clear() noexcept { held_.clear(); }
	[[nodiscard]] size_t size() const noexcept { return held_.size(); }

	friend bool operator==(const DeviceViews&, const DeviceViews&) = default;

private:
	std::vector<std::pair<DeviceInstanceId, uint32_t>> held_;
};

/// The name `d` DECLARES for output `output` — empty for no device, no such output, or one declared unnamed.
[[nodiscard]] inline std::string_view declaredOutputName(const device::Descriptor* d, size_t output) noexcept {
	if (d == nullptr || output >= d->outputs.size() || d->outputs[output].name == nullptr) {
		return {};
	}
	return d->outputs[output].name;
}

/// **WHAT A PLAYER CALLS EACH OF A DEVICE'S OUTPUTS** — per instance, so two MACROS cards can call their
/// first output "Wobble" and "Swell". State about a DEVICE, so it rides in the patch beside the view flags
/// and on `DeviceViews`' rules: keyed by instance id, filled by a load, read by a save, MESSAGE THREAD ONLY,
/// never published (a label is a `std::string`, and no block reads one).
///
/// AN EMPTY LABEL IS "THE DECLARED NAME", so "not stored" and "stored empty" are the same answer, and a
/// device whose every label is empty has no entry at all. Each entry's list is kept with no trailing empty,
/// so two maps that name the same labels compare equal however they got there. The one thing this does NOT
/// know is the declared name — it holds no descriptors — so "a label equal to the declared name clears" is
/// the bridge's rule (`setOutputLabel`) and the loader's (`applyPatchToRecord`), not this class's.
class DeviceLabels {
public:
	/// Output `output`'s label, EMPTY when it carries none. Valid until the next `set` or `clear`.
	[[nodiscard]] std::string_view labelOf(DeviceInstanceId device, size_t output) const noexcept {
		const std::vector<std::string>* labels = labelsOf(device);
		return labels != nullptr && output < labels->size() ? std::string_view((*labels)[output])
		                                                    : std::string_view {};
	}

	/// Every label the device carries, by output, no trailing empty — null for a device with none.
	[[nodiscard]] const std::vector<std::string>* labelsOf(DeviceInstanceId device) const noexcept {
		for (const auto& [id, labels] : held_) {
			if (id == device) {
				return &labels;
			}
		}
		return nullptr;
	}

	/// CLAMPED to `device::kMacroNameMax` bytes (`clampMacroName`, which never splits a code point). Empty
	/// ERASES, and the device's entry goes with its last label — `DeviceViews::set`'s rule, so the map cannot
	/// grow with every output that was ever relabelled and cleared.
	void set(DeviceInstanceId device, size_t output, std::string_view label) {
		if (device == kNoInstance || output >= device::kMaxDeviceOutputs) {
			return; // addresses nothing
		}
		std::string clamped = device::clampMacroName(label);
		auto it = std::find_if(held_.begin(), held_.end(), [device](const auto& e) { return e.first == device; });
		if (it == held_.end()) {
			if (clamped.empty()) {
				return;
			}
			it = held_.insert(held_.end(), { device, {} });
		}
		std::vector<std::string>& labels = it->second;
		if (output >= labels.size()) {
			if (clamped.empty()) {
				return;
			}
			labels.resize(output + 1);
		}
		labels[output] = std::move(clamped);
		while (!labels.empty() && labels.back().empty()) {
			labels.pop_back();
		}
		if (labels.empty()) {
			held_.erase(it);
		}
	}

	/// Every label of one device gone — what a removal does.
	void erase(DeviceInstanceId device) {
		std::erase_if(held_, [device](const auto& e) { return e.first == device; });
		std::erase_if(presets_, [device](const auto& e) { return e.first == device; });
	}

	void clear() noexcept {
		held_.clear();
		presets_.clear();
	}

	// **AND WHICH PRESET EACH DEVICE'S PANEL LAST LOADED OR SAVED** (backlog QC: an engine restart used to
	// forget which preset it had loaded). Also what a device instance is CALLED, so it lives here, rides in the patch
	// beside its labels (a device's "preset" member) and on the same rules: by instance id, message thread, an empty
	// name ERASES. A device panel's browser lives only while its window is open; this is what it opens on.

	/// The preset device `device` came from, or null.
	[[nodiscard]] const PresetIdentity* presetOf(DeviceInstanceId device) const noexcept {
		for (const auto& [id, identity] : presets_) {
			if (id == device) {
				return &identity;
			}
		}
		return nullptr;
	}
	/// Name it (an empty name forgets it). True when that changed anything.
	bool setPreset(DeviceInstanceId device, const PresetIdentity& identity) {
		if (device == kNoInstance) {
			return false;
		}
		auto it = std::find_if(presets_.begin(), presets_.end(), [device](const auto& e) { return e.first == device; });
		if (identity.empty()) {
			if (it == presets_.end()) {
				return false;
			}
			presets_.erase(it);
			return true;
		}
		if (it != presets_.end()) {
			if (it->second == identity) {
				return false;
			}
			it->second = identity;
			return true;
		}
		presets_.emplace_back(device, identity);
		return true;
	}
	[[nodiscard]] size_t size() const noexcept { return held_.size(); }

	/// **EACH (device, output) WHOSE LABEL DIFFERS BETWEEN `before` AND `after`**, called once apiece — what a
	/// load tells its subscribers. Ids are patch-scoped, so this compares what each id's outputs are CALLED,
	/// not whether they are the same device: an incoming device 1 that loses the outgoing one's "Wobble" is a
	/// label that moved, and the surface reading it must redraw either way.
	template <typename Fn>
	static void forEachDifference(const DeviceLabels& before, const DeviceLabels& after, Fn&& fn) {
		const auto walk = [&](const DeviceLabels& one, const DeviceLabels& other, bool skipShared) {
			for (const auto& [id, labels] : one.held_) {
				if (skipShared && other.labelsOf(id) != nullptr) {
					continue; // reported by the first walk
				}
				const std::vector<std::string>* theirs = other.labelsOf(id);
				const size_t n = std::max(labels.size(), theirs != nullptr ? theirs->size() : size_t { 0 });
				for (size_t output = 0; output < n; ++output) {
					if (one.labelOf(id, output) != other.labelOf(id, output)) {
						fn(id, output);
					}
				}
			}
		};
		walk(before, after, false);
		walk(after, before, true);
	}

	friend bool operator==(const DeviceLabels&, const DeviceLabels&) = default;

private:
	std::vector<std::pair<DeviceInstanceId, std::vector<std::string>>> held_;
	std::vector<std::pair<DeviceInstanceId, PresetIdentity>> presets_;
};

/// **WHAT A PATCH SAYS, AS A VALUE — the stream's content and nothing about any engine or record.**
/// Parsed anywhere (`parseRack` touches no host state); applied by whoever holds the devices.
/// HOW THE CANVAS IS SHOWN — its size along its dock axis and whether it is folded. IN THE PATCH, like a
/// card's fold. Not
/// per device — the canvas is the editor's one band — so it rides beside the chains rather than in a row.
/// `extent` 0 is "never set": the editor uses its default.
struct CanvasView {
	float extent = 0.0f;
	bool collapsed = false;

	friend bool operator==(const CanvasView&, const CanvasView&) = default;
};

/// A PATCH TAG FROM A DECLARED ONE. Same reason as `device::tagOf`: `traits.tag` is a string
/// literal, so copying the field's full width off it reads past its end. Here it also guarantees the
/// NUL the patch side depends on.
[[nodiscard]] inline std::array<char, device::kTagBytes + 1> patchTagOf(const char* declared) {
	std::array<char, device::kTagBytes + 1> out {};
	if (declared == nullptr) {
		return out;
	}
	for (size_t i = 0; i < device::kTagBytes && declared[i] != '\0'; ++i) {
		out[i] = declared[i];
	}
	return out;
}

struct RackPatch {
	/// **ONE STORED VALUE, BY KEY** (backlog JF §3.3). What the file said, not yet what any device makes of
	/// it — a patch is parsed with no descriptor in reach. A control's number, a Choice's LABEL, `null` (a
	/// value that was not finite when saved), or anything else a newer build wrote, kept as its JSON text so
	/// it is carried back out unchanged.
	struct Value {
		enum class Kind : uint8_t { Number, Label, Null, Raw };
		std::string key;
		Kind kind = Kind::Number;
		float number = 0.0f;
		/// The label (`Label`), or the value's compact JSON (`Raw`).
		std::string text;

		static Value of(std::string key, float v) { return Value { std::move(key), Kind::Number, v, {} }; }
		static Value label(std::string key, std::string l) {
			return Value { std::move(key), Kind::Label, 0.0f, std::move(l) };
		}

		friend bool operator==(const Value&, const Value&) = default;
	};

	struct Device {
		/// WHICH OF THE HOST'S CHAINS — Feathers: 0 the strip, 1 the fx lane; the rack: 0.
		uint32_t chain = 0;
		/// **`kTagBytes` CHARACTERS AND A NUL, AND THE +1 IS LOAD-BEARING.** This is the PATCH's copy
		/// of the tag, not the record's `device::Tag`: it is handed to `descriptorFor` and
		/// `provision` as a C string, so it needs a terminator the record's exactly-sized field does
		/// not have. It was `std::array<char, 5>` while tags were four bytes, and the day the field
		/// widened this was what overran -- the writer put `kTagBytes` bytes out of a five-byte array
		/// and the reader put them back, which segfaulted `chain-record` rather than failing quietly.
		std::array<char, device::kTagBytes + 1> tag {};
		/// THE ID ROUTES NAME IT BY — its only address. A loader may give it another (one already in use, or
		/// none); the routes that named it follow it there. **NO LANE**: where
		/// a device sits is its chain and its place in the order of the records.
		DeviceInstanceId instanceId = kNoInstance;
		/// **ITS CONTROLS, BY KEY — NOT A ROW READ BY COLUMN** (backlog JF). Written in descriptor order (what
		/// the card shows), read in any order: a control inserted mid-row changes nothing for the others, a
		/// new one takes its default, and a key this build does not know is carried back out, not applied.
		/// `valuesOfRow` and `rowOfValues` are the only translation between this and a row.
		std::vector<Value> values;
		/// **STATE BEYOND THE CONTROLS** (`Traits::extraState`), under the device's `extraStateKey`: each a
		/// `Raw` JSON array of numbers. Usually none, or one.
		std::vector<Value> extra;
		/// The device's `Traits::schema` when written; 0 when the file did not say.
		uint32_t schema = 0;
		/// **HOW ITS CARD IS SHOWN** — a bit field (`kViewFolded`), zero for a card in its ordinary state.
		/// View state, not sound: no engine ever reads it, and a build that does not know a bit ignores it.
		uint32_t view = 0;
		/// **WHAT AN OUTPUT IS CALLED ON THIS INSTANCE** (`DeviceLabels`) — BY THE OUTPUT'S KEY (backlog JF),
		/// each a `Label` value, only for an output that carries one: at most `device::kMacroNameMax` bytes of
		/// UTF-8. View state like `view`: no engine reads it. Keyed rather than by index for the reason values
		/// are: an output inserted above another would hand it the other's name.
		std::vector<Value> labels;
		/// Which preset its panel last loaded or saved (backlog QC) — `DeviceLabels::presetOf`; empty: none.
		PresetIdentity preset;
		/// A CONTAINER'S CONTENTS as bytes (`ContainerOps::saveState`) — a rack in a slot carries the rack
		/// inside it here. Empty for every other device.
		std::vector<uint8_t> body;
		/// **ITS CHAIN ACCESS SITE** (backlog NR): whether it has one, and the two chains' bundle (`SiteChains::saveState`).
		/// `hasSite` with no bytes is a site holding nothing — still a site, so its chip stays as it was left.
		bool hasSite = false;
		std::vector<uint8_t> site;

		friend bool operator==(const Device&, const Device&) = default;
	};
	struct Route {
		PatchCoord source;
		PatchCoord destination;
		float amount = 0.0f;
		/// THE ROUTE'S CURVE — written as `"transfer": "<label>"` only when not Lin, so a Lin patch writes as it always
		/// did and a file with no key reads Lin.
		device::Transfer transfer = device::Transfer::Lin;

		friend bool operator==(const Route&, const Route&) = default;
	};

	/// WHETHER THERE WAS A PATCH AT ALL — an empty stream is a fresh instance, not an empty rack.
	bool present = false;
	/// THE HOST'S MACRO VALUES, when its macros are not parameters saved elsewhere (the rack's). Feathers
	/// writes none: its macros are CLAP parameters, on the parameter road.
	std::vector<float> macro;
	/// AND WHICH WAY EACH SWINGS (backlog EG): 1 for a macro read bipolar (-1..+1 of the same 0..1
	/// value), 0 for unipolar. Beside `macro`, sized like it; a shorter list leaves the rest unipolar.
	std::vector<uint8_t> macroBipolar;
	/// AND HOW EACH ONE'S CONTROL DRIVES IT — a `device::MacroType` per macro, beside `macroBipolar` and
	/// sized like it; a shorter list leaves the rest Knob.
	std::vector<uint8_t> macroType;
	/// AND WHAT EACH IS CALLED — at most `device::kMacroNameMax` bytes of UTF-8, EMPTY for the default
	/// "Macro N". A shorter list leaves the rest unnamed.
	std::vector<std::string> macroName;
	/// Chain by chain, each in its order.
	std::vector<Device> device;
	std::vector<Route> route;
	/// **WHAT THIS CHAIN EXPORTS TO THE HOST OUTSIDE** (backlog NZ (b)) — a Chain container's export table, row by row:
	/// the inner source each row names (`device` kNoInstance for one of this chain's macros, `index` its number). An
	/// outer route reads it as `Source::exported(the container, row)`. Empty everywhere else.
	struct Export {
		uint32_t row = 0;
		DeviceInstanceId device = kNoInstance;
		uint32_t index = 0;

		friend bool operator==(const Export&, const Export&) = default;
	};
	std::vector<Export> exports;
	CanvasView canvas;
	/// **WHETHER THE MACROS COLUMN IS FOLDED** (backlog QN) — in the patch, like the canvas's fold, so a rack or a chain reopens
	/// the way it was left in a fresh instance. `"macrosColumn": {"collapsed": true}`, written only when folded.
	bool macrosCollapsed = false;
	/// **THE HOST SLOT POOL** (backlog JU; host-slots plan §2, T4) — which host param names which control, in
	/// which state, as `SlotPool::snapshot` took it. ABSENT (nullopt) when the document has no `hostSlots`
	/// member, which is every patch but the OUTERMOST plugin's own: a nested rack never writes one, and a
	/// preset does not carry one (plan §0.8). `SlotPool::restore` is how a pool is made from it, and the
	/// parse has already run that validation once, so a snapshot that arrives here restores.
	std::optional<hostslots::PoolSnapshot> hostSlots {};
	/// **WHICH PRESET THE RACK PLUGIN'S OWN BROWSER LAST LOADED OR SAVED** (backlog QC) — the outermost patch's, as its
	/// host slots are; a preset file carries none (`captureTopPreset`). Empty: none.
	PresetIdentity preset;

	friend bool operator==(const RackPatch&, const RackPatch&) = default;
};

/// **A HOST'S OWN COORDINATES, NAMED AND PLACED** — the half of a route no device holds. Each returns
/// false for a coordinate it cannot name or a name it cannot place. `host` is handed back to every call.
struct CoreCodec {
	const void* host = nullptr;
	bool (*nameSource)(const void* host, size_t source, uint32_t* kind, uint32_t* a) = nullptr;
	bool (*placeSource)(const void* host, uint32_t kind, uint32_t a, size_t* source) = nullptr;
	bool (*nameDestination)(const void* host, size_t destination, uint32_t* kind, uint32_t* a) = nullptr;
	bool (*placeDestination)(const void* host, uint32_t kind, uint32_t a, size_t* destination) = nullptr;
};

/// THE RACK'S CODEC — its core sources are macros, by index; its core destinations (none, today) by
/// index. `axes` must outlive every use.
CoreCodec rackCoreCodec(const backplane::Axes& axes);

//------------------------------------------------------------------------------------------------//
// THE WIRE                                                                                       //
//------------------------------------------------------------------------------------------------//

//
// **A JSON DOCUMENT SINCE BACKLOG JF**, and the one format the
// rack plugin's state, Feathers' `FSC1` block, SKLITTER's lanes and device presets all ride:
//
//     {"format": "feathers.rack", "major": 1,
//      "chains": [[{"tag", "id", "schema", "view", "values": {key: number | "label" | null},
//                   "extra": {key: [numbers]}, "body": {"$bin": …} | {a document} | null}, …], …],
//      "routes": [{"src": {"device": id, "out": key} | {"macro": n} | …,
//                  "dst": {"device": id, "control": key} | {"param": id} | …, "amount": x}],
//      "macros": [x…], "bipolar": [0|1…], "canvas": {"extent": x, "collapsed": bool},
//      "hostSlots": {"capacity": n, "clock": n, "slots": [{"n", "state", "path": [ids], "tag", "key", "label",
//                    "recent", "level", "rank", "chainOrder", "beforeAnchor"}, …]}}   (optional)
//
// **`hostSlots` IS THE ONE MEMBER THAT MAY BE DROPPED ALONE.** Everything else malformed refuses the whole
// document. A malformed pool does not, because what it guards is the host's lanes, not the sound: a patch
// whose devices, values and routes are all readable is worth loading without its slot map, and the report
// (`SlotsDropped`) says the map was lost. Refusing would lose the patch AND the map. It is dropped WHOLE,
// never slot by slot, because `SlotPool::restore` is all-or-nothing and the parse runs its validation (`whyInvalid`) to decide.
//
// **A CLEAN BREAK**: the binary format it replaced (rack `kVersion` 10) has no reader,
// and such a stream is refused and reported as `NotLoaded`. A body is the device's own bytes, unread by
// the host: in a `$bin` when they are opaque (IrRef, ICB1), and spliced in as the object itself when they
// are one JSON document — a rack inside a rack — so a nest is readable and costs what its contents cost.
// The framing, not a sniff, tells the two apart on the way back.

/// The document's `major`: the envelope's own version, which moves only when the SHAPE of this document
/// changes — never for a device edit, which keys absorb.
inline constexpr uint32_t kRackMajor = 1;

/// **A PATCH AS TEXT** — tab-indented, one member per line, deterministic; or compact. False only for a
/// patch that cannot be named in a file: a device route end with no key.
///
/// **A CONTAINER'S BODY IS WRITTEN COMPACT**, and that is what keeps it the device's own bytes: its parent
/// splices it in as an object (indented, as the parent is) and hands the object's COMPACT text back on
/// load — which is, byte for byte, what the container wrote.
bool rackToText(const RackPatch& patch, std::string* out, bool indented = true);

/// **WRITE A PATCH** — `rackToText`, onto the stream. The whole stream is the document.
bool writeRack(const clap_ostream* stream, const RackPatch& patch, bool indented = true);

/// **READ A PATCH INTO A VALUE. NO HOST, SO NO THREAD.** False: not a rack document — nothing is to be
/// applied, and `report` (if given) says `NotLoaded` and why. True with `present == false`: an empty text,
/// a fresh instance. A device this build cannot run is still parsed; whether it can run is the applier's
/// question.
bool parseRackText(std::string_view text, RackPatch* out, LoadReport* report = nullptr);

/// `parseRackText` over what is left of `stream` — READ TO ITS END, so the stream must hold the document
/// and nothing after it (a host framing one among other data frames it by length and hands the text).
bool parseRack(const clap_istream* stream, RackPatch* out, LoadReport* report = nullptr);

//------------------------------------------------------------------------------------------------//
// KEYS ↔ A ROW — the one translation, for every road that holds a device's fields                   //
//------------------------------------------------------------------------------------------------//

/// **A ROW AS KEYED VALUES.** Every control in descriptor order — a labelled Choice as its label — the
/// extra state under its key when the row carries it. A control past `fieldsPresent` is written as its
/// declared default, which is what the device reads for it.
void valuesOfRow(const device::Descriptor& d, std::span<const float> fields, size_t fieldsPresent,
                 RackPatch::Device* out);

/// **KEYED VALUES AS A ROW** — every control by its key, wherever it stands in the file: a missing key at
/// its declared default, a Choice by label (and its aliases), every value coerced by the one rule. Returns
/// `fieldsPresent` (the controls, then as much extra state as the file carried). Values this build does not
/// apply but must carry back out — unknown keys, and a newer build's extra state — are appended to
/// `carried`; retired ones are dropped. Every departure from "loaded as saved" is noted in `report` under
/// `where`. `fields` must hold `d.params.size() + d.traits.extraState`.
size_t rowOfValues(const device::Descriptor& d, const RackPatch::Device& in, std::span<float> fields,
                   LoadReport* report, const std::string& where, RackPatch::Device* carried);

/// How a report names a device: its card name and where it stands.
std::string deviceWhere(const char* name, size_t chain, size_t position);

//------------------------------------------------------------------------------------------------//
// A RECORD — what Feathers' bridge and the rack's bridge hold                                     //
//------------------------------------------------------------------------------------------------//

/// **A RECORD AS A PATCH** — every chain's devices in order (a tag this build cannot run included), each
/// row and container body, every route the record published (`routes`, the list its host publishes from
/// it), and `macros` as given. `views` is how each device's card is shown (null: none, every card
/// ordinary); `labels` what each device's outputs are called (null: every one its declared name).
/// Message thread.
RackPatch patchOfRecord(const ChainRecord& record, const ContainerBodies* bodies, std::span<const float> macros,
                        const backplane::RouteList& routes, const CoreCodec& codec,
                        const DeviceViews* views = nullptr, std::span<const uint8_t> macroBipolar = {},
                        const DeviceLabels* labels = nullptr);

/// **A PATCH INTO A RECORD, WHICH IS EMPTIED FIRST** — each device appended to its chain in the order of
/// the records (the file names no place but that order, and where the record parks a row is the record's
/// own business), under its id if free and a fresh one (from `ids`) if
/// not; each device's values into its row BY KEY (`rowOfValues`); bodies into `bodies`, and beside each
/// what the row cannot hold and a re-save must write back (`ContainerBodies::carry` — a placeholder's whole
/// values, a known device's unknown keys); routes resolved by id and key and placed through the record's
/// gate. Returns what did not load as saved. A patch with nothing in it (`present == false`) changes
/// nothing. Macros are left to the host. `views`, when given, is EMPTIED AND REFILLED under the id each
/// device ends up wearing — so a fold follows its device through a load that had to rename it, and one
/// patch's folds can never survive into another's. `labels`, when given, the same, and on the same rule — a
/// label spelling out the declared name is stored as none, and one naming no output of a known device is
/// dropped. Message thread.
LoadReport applyPatchToRecord(const RackPatch& patch, ChainRecord* record, ContainerBodies* bodies,
                              backplane::InstanceCounter* ids, const CoreCodec& codec, DeviceViews* views = nullptr,
                              DeviceLabels* labels = nullptr);

/// **ONE DEVICE OF A RECORD AS A PATCH HOLDS IT** — the row by key, its labels, view, body, site, carried values and
/// keyed blocks: what `patchOfRecord` writes for each device, and what a fragment lifts (backlog OA).
RackPatch::Device patchDeviceOfRow(const ChainRecord& record, const ContainerBodies* bodies, const DeviceViews* views,
                                   const DeviceLabels* labels, const DeviceState& row, size_t chain);
/// ONE ROUTE OF A RECORD AS A PATCH HOLDS IT — false for one the file cannot name (`patchOfRecord`'s rule).
bool patchRouteOfRecord(const ChainRecord& record, const CoreCodec& codec, const backplane::Route& route,
                        RackPatch::Route* out);

/// **WHERE `placePatchInRecord` PUTS A PATCH'S DEVICES.**
struct PatchPlacement {
	/// Every device into this chain — or, `kNoChain`, each into the chain the patch names.
	size_t chain = ChainRecord::kNoChain;
	/// The first device here and each next after it — or, `kNoPosition`, each at the end of its chain (a load).
	size_t position = ChainRecord::kNoPosition;
	/// Each device at its own place (an undo's put-back, ascending), overriding `position`.
	std::span<const size_t> positions {};
	/// Where an id the record already holds gets a fresh one. Null: such a device is refused.
	backplane::InstanceCounter* idsOrFresh = nullptr;
	/// A route end naming a device the patch does not carry is the record's own (a fragment's inbound route).
	bool outsideEnds = false;
	/// (named, worn) for every device placed.
	std::vector<std::pair<DeviceInstanceId, DeviceInstanceId>>* worn = nullptr;
};

/// **A PATCH INTO A RECORD, WHICH IS NOT EMPTIED** — `applyPatchToRecord`'s road with the emptying left to the caller:
/// each device placed as `placement` says, under the id it names where that is free, its body, site, labels, view and
/// blocks filed under the id it wears; then every route through the record's gate, refusals into `report`. Returns
/// how many devices landed. Message thread.
size_t placePatchInRecord(const RackPatch& patch, ChainRecord* record, ContainerBodies* bodies, const CoreCodec& codec,
                          DeviceViews* views, DeviceLabels* labels, const PatchPlacement& placement, LoadReport* report);

//------------------------------------------------------------------------------------------------//
// AN ENGINE — for a rack inside a rack, which has no record of its own yet                          //
//------------------------------------------------------------------------------------------------//

/// **AN ENGINE AS A PATCH.** False if a container could not say its body.
bool patchOf(const RackEngine& rack, RackPatch* out);

/// **A PATCH INTO AN ENGINE, WHICH IS EMPTIED FIRST.** Builds and adopts on the calling thread — for an
/// engine no block is running on (a rack device before adoption, a case). `report` is required. **AN
/// ENGINE CARRIES NOTHING**: it has no store beside its rows, so a key this build does not know is
/// reported and not kept on this road — the record's road keeps it.
void applyRack(const RackPatch& patch, RackEngine* rack, LoadReport* report);

/// A CONTAINER'S BODY, AS BYTES — empty (and true) for a device that holds nothing.
bool bodyOf(const device::Descriptor* d, const void* storage, std::vector<uint8_t>* out);

//------------------------------------------------------------------------------------------------//
// A DEVICE'S KEYED BLOCKS ON EVERY ROAD (backlog NM, `device/state_blocks.h`)                      //
//------------------------------------------------------------------------------------------------//

/// **HAND A FRESH INSTANCE THE BLOCKS STORED FOR IT** — message thread, before the instance is posted. A block of the
/// wrong length, or one the device refuses, is skipped: the device plays it as freshly built. Returns how many it took.
size_t restoreBlocks(const device::Descriptor& d, void* storage, const ContainerBodies& bodies, DeviceInstanceId id);

/// **THE STORE CAUGHT UP WITH WHAT A RUNNING INSTANCE PUBLISHED** — every block, or (`onTickOnly`) only those that
/// declare `harvestOnTick`. A block never published (serial 0) keeps what is stored. Returns how many moved. Allocates:
/// message thread.
size_t refreshBlocks(const device::Descriptor& d, const void* storage, ContainerBodies* bodies, DeviceInstanceId id,
                     bool onTickOnly);

/// **THE BRIDGES' TICK HARVEST**: `refreshBlocks(onTickOnly)` for every device in `record` whose running instance is
/// filed in `bodies`. Returns how many blocks moved.
size_t harvestBlocks(const ChainRecord& record, ContainerBodies* bodies);

/// `patchOf` then `writeRack`; `parseRack` then `applyRack`.
///
/// **THIS ROAD DROPS VIEW STATE — every card's fold and every output's label.** An engine holds neither;
/// they live on the bridge (`DeviceViews`, `DeviceLabels`). That is sound only because this road is the
/// FALLBACK for a rack inside a rack BEFORE `RackEngine::provision` hands the container its catalogue,
/// which is what builds the bridge. From then on a save goes through `bridge->patch()`, so a real project
/// save carries both. Anything that calls these on an engine that already has a bridge loses them.
bool saveRack(const clap_ostream* stream, const RackEngine& rack, bool indented = true);
bool loadRack(const clap_istream* stream, RackEngine* rack, LoadReport* report);

} // namespace feathers::rack
