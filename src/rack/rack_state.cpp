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

#include "rack/rack_state.h"

#include "backplane/axes.h"
#include "rack/rack_device.h"
#include "rack/rack_engine.h"
#include "rack/inner_chain_bundle.h"
#include "rack/site_chains.h"
#include "state/json.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace feathers::rack {

namespace json = state::json;

namespace {

// **THE BINARY FORMAT ENDED AT ITS v10 (2026-09-25, backlog JF step 1c).** Its history — v5 one format for
// every host, v6 no lane, v7 view flags, v8 the canvas, v9 the eight-byte tag, v10 macro polarity — was a
// history of the whole stream refused at its first byte for every change, and of a device's row read by
// COLUMN, so a control inserted mid-row re-pointed every later value without a word (the plan's §1.5: on
// this road nothing even refused). The document that replaced it names every value by key; a stream in
// the old format is refused (its first byte is not `{`) and reported as `NotLoaded`. No reader for it is
// kept: a clean break, 2026-09-24.
constexpr const char* kFormat = "feathers.rack";

/// A CEILING ON WHAT A FILE CAN MAKE THIS BUILD DO — every count read from a document is checked before it
/// is used as a length.
constexpr size_t kMaxCount = 1u << 20;

/// THE WHOLE DOCUMENT'S CEILING: the largest body, as base64, and room for everything around it.
constexpr size_t kMaxDocumentBytes = kMaxBodyBytes / 3 * 4 + (size_t { 8 } << 20);

/// HOW DEEP A DOCUMENT MAY NEST: a rack spliced into a rack's body adds about six levels (chains, a chain,
/// a device, its body, then the inner document's own), and `kMaxDepth` racks may nest — so the reader's
/// bound is set by the rack format's, with room, rather than by the JSON default.
constexpr size_t kMaxJsonDepth = kMaxDepth * 8 + 16;

json::Limits documentLimits() {
	json::Limits limits;
	limits.maxBytes = kMaxDocumentBytes;
	limits.maxDepth = kMaxJsonDepth;
	return limits;
}

/// **A BODY THAT IS ITSELF A DOCUMENT IS WRITTEN AS ONE** (state format §3.2, §3.7): a rack inside a rack's body is the rack document, spliced in as that object, so the file stays
/// readable and a nest costs what its contents cost. Any other body — an IrRef, an ICB1 bundle, anything
/// that is not one JSON object — is the device's own bytes in a `$bin`. **The framing tells them apart,
/// not a sniff of the bytes on the way back**: a `$bin` object is opaque, any other object is the document
/// form, and the device is handed that object's text either way.
///
/// MEASURED before this (2026-09-25, `test_rack_nesting`): with every body in a `$bin`, one OWLTT at the
/// bottom of twenty nested racks saved as 926,992 bytes, because each level's base64 is 4/3 of the level
/// inside it; the binary format it replaced wrote 2,960.
bool bodyIsDocument(const std::vector<uint8_t>& body, json::Value* out) {
	if (body.empty() || body.front() != '{') {
		return false;
	}
	const std::string_view text(reinterpret_cast<const char*>(body.data()), body.size());
	return json::parse(text, out, documentLimits()) && out->isObject() && !json::isBin(*out);
}

//------------------------------------------------------------------------------------------------//
// KEYS                                                                                           //
//------------------------------------------------------------------------------------------------//

constexpr size_t kNoIndex = static_cast<size_t>(-1);

size_t paramIndexOf(const device::Descriptor& d, std::string_view key) {
	for (size_t i = 0; i < d.params.size(); ++i) {
		if (d.params[i].key != nullptr && key == d.params[i].key) {
			return i;
		}
	}
	return kNoIndex;
}

size_t outputIndexOf(const device::Descriptor& d, std::string_view key) {
	for (size_t i = 0; i < d.outputs.size(); ++i) {
		if (d.outputs[i].key != nullptr && key == d.outputs[i].key) {
			return i;
		}
	}
	return kNoIndex;
}

const device::RetiredKey* retiredOf(const device::Descriptor& d, std::string_view key) {
	for (const device::RetiredKey& r : d.traits.retiredKeys) {
		if (r.key != nullptr && key == r.key) {
			return &r;
		}
	}
	return nullptr;
}

/// A CHOICE WHOSE VALUE IS STORED AS ITS LABEL — one with labels. A switch or a count with no labels
/// stays a number (§3.4).
bool storedByLabel(const device::ParamDesc& p) {
	return p.unit == device::Unit::Choice && !p.names.empty();
}

/// The label a Choice's value lands on — the nearest step, as every reader of a stepped control takes it.
const char* labelOf(const device::ParamDesc& p, float value) {
	const double v = std::isfinite(value) ? static_cast<double>(value) : p.def;
	const long long idx = std::llround(v - p.min);
	const long long last = static_cast<long long>(p.names.size()) - 1;
	return p.names[static_cast<size_t>(std::clamp(idx, 0LL, last))];
}

/// A label back to its value: the label itself, or one the control was renamed from.
bool valueOfLabel(const device::ParamDesc& p, std::string_view label, float* out) {
	std::string_view want = label;
	for (const device::ChoiceAlias& a : p.choiceAliases) {
		if (a.from != nullptr && a.to != nullptr && label == a.from) {
			want = a.to;
			break;
		}
	}
	for (size_t i = 0; i < p.names.size(); ++i) {
		if (p.names[i] != nullptr && want == p.names[i]) {
			*out = static_cast<float>(p.min + static_cast<double>(i));
			return true;
		}
	}
	return false;
}

//------------------------------------------------------------------------------------------------//
// VALUES AS TEXT                                                                                 //
//------------------------------------------------------------------------------------------------//

void writeValue(json::Writer& w, const RackPatch::Value& v) {
	switch (v.kind) {
	case RackPatch::Value::Kind::Number: w.number(v.number); break; // non-finite is written `null`
	case RackPatch::Value::Kind::Label: w.string(v.text); break;
	case RackPatch::Value::Kind::Null: w.null(); break;
	case RackPatch::Value::Kind::Raw: {
		json::Value parsed;
		if (json::parse(v.text, &parsed)) {
			w.value(parsed);
		}
		else {
			w.null();
		}
		break;
	}
	}
}

RackPatch::Value valueOf(std::string key, const json::Value& node) {
	RackPatch::Value v;
	v.key = std::move(key);
	if (node.isNumber()) {
		// A NUMBER THAT DOES NOT FIT A FLOAT (1e999) IS NOT INFINITY: it is a value that did not survive,
		// and it takes the non-finite road.
		v.kind = node.asFloat(&v.number) ? RackPatch::Value::Kind::Number : RackPatch::Value::Kind::Null;
	}
	else if (node.isString()) {
		v.kind = RackPatch::Value::Kind::Label;
		v.text = node.text;
	}
	else if (node.isNull()) {
		v.kind = RackPatch::Value::Kind::Null;
	}
	else {
		v.kind = RackPatch::Value::Kind::Raw;
		v.text = json::toCompactText(node);
	}
	return v;
}

/// **WHICH PRESET, AS JSON** (backlog QC): `{"name": …, "category": …}`, the folder only when there is one.
void writePresetIdentity(json::Writer& w, const PresetIdentity& identity) {
	w.beginObject();
	w.key("name");
	w.string(identity.name.substr(0, kPresetIdentityMaxBytes));
	if (!identity.category.empty()) {
		w.key("category");
		w.string(identity.category.substr(0, kPresetIdentityMaxBytes));
	}
	w.endObject();
}

/// Read it — absent, not an object, or with no string name reads as NONE (a name is not worth refusing a patch over).
bool readPresetIdentity(const json::Value* node, PresetIdentity* out) {
	*out = {};
	if (node == nullptr || !node->isObject()) {
		return false;
	}
	const json::Value* name = node->find("name");
	if (name == nullptr || !name->isString() || name->text.size() > kPresetIdentityMaxBytes) {
		return false;
	}
	out->name = name->text;
	if (const json::Value* category = node->find("category");
	    category != nullptr && category->isString() && category->text.size() <= kPresetIdentityMaxBytes) {
		out->category = category->text;
	}
	return true;
}

void writeValues(json::Writer& w, const std::vector<RackPatch::Value>& values) {
	w.beginObject();
	for (const RackPatch::Value& v : values) {
		w.key(v.key);
		writeValue(w, v);
	}
	w.endObject();
}

bool readValues(const json::Value* node, std::vector<RackPatch::Value>* out) {
	out->clear();
	if (node == nullptr || node->isNull()) {
		return true;
	}
	if (!node->isObject() || node->members.size() > kMaxCount) {
		return false;
	}
	out->reserve(node->members.size());
	for (const json::Member& m : node->members) {
		out->push_back(valueOf(m.key, m.value));
	}
	return true;
}

/// **WHAT A RE-SAVE MUST WRITE BACK, AS `ContainerBodies::carry` KEEPS IT** — the leftovers of one device
/// (its unknown values and extra state, and for a placeholder its schema) as one compact object.
std::string carriedText(const RackPatch::Device& leftovers) {
	if (leftovers.values.empty() && leftovers.extra.empty() && leftovers.labels.empty() && leftovers.schema == 0) {
		return {};
	}
	std::string out;
	json::Writer w(&out, false);
	w.beginObject();
	w.key("values");
	writeValues(w, leftovers.values);
	w.key("extra");
	writeValues(w, leftovers.extra);
	w.key("labels");
	writeValues(w, leftovers.labels);
	w.key("schema");
	w.number(leftovers.schema);
	w.endObject();
	return out;
}

void readCarried(const std::string* text, RackPatch::Device* into) {
	json::Value doc;
	if (text == nullptr || !json::parse(*text, &doc) || !doc.isObject()) {
		return;
	}
	// **NEVER A KEY TWICE**: a device replaced under its own id keeps the old one's leftovers until a load
	// clears them, and a repeated key is a document the reader refuses outright. What the row holds wins.
	const auto append = [](std::vector<RackPatch::Value>* to, const std::vector<RackPatch::Value>& from) {
		for (const RackPatch::Value& v : from) {
			const bool held = std::any_of(to->begin(), to->end(), [&](const RackPatch::Value& h) { return h.key == v.key; });
			if (!held) {
				to->push_back(v);
			}
		}
	};
	std::vector<RackPatch::Value> values;
	std::vector<RackPatch::Value> extra;
	std::vector<RackPatch::Value> labels;
	if (readValues(doc.find("values"), &values)) {
		append(&into->values, values);
	}
	if (readValues(doc.find("extra"), &extra)) {
		append(&into->extra, extra);
	}
	if (readValues(doc.find("labels"), &labels)) {
		append(&into->labels, labels);
	}
	uint32_t schema = 0;
	if (into->schema == 0 && doc.find("schema") != nullptr && doc.find("schema")->asU32(&schema)) {
		into->schema = schema;
	}
}

//------------------------------------------------------------------------------------------------//
// ONE ROUTE, NAMED AND PLACED — the same two functions for an engine and for a record              //
//------------------------------------------------------------------------------------------------//

/// THE DEVICE AN ENGINE OR A RECORD HOLDS UNDER `id`, and what it is — for the file's two roads. **BY ID,
/// SINCE KILL-LANES §2.4b**, and since backlog JF a route's device end is that id and a KEY, so the
/// descriptor is what turns the key into the device's own index and back.
struct Holder {
	const void* host = nullptr;
	const device::Descriptor* (*descOf)(const void* host, DeviceInstanceId id) = nullptr;
	/// **FOR SAYING A REFUSAL, THE DEVICE THE FILE MEANT** — when no device here wears the id a route named,
	/// the patch still says what that device was (its tag), and the host's catalogue says what that tag
	/// declares, so a refused route's key can still be reported as the control the file asked for.
	const RackPatch* patch = nullptr;
	const device::Descriptor* (*byTag)(const void* host, const char* tag) = nullptr;

	const device::Descriptor* declaredOf(DeviceInstanceId id) const {
		if (const device::Descriptor* d = descOf(host, id); d != nullptr) {
			return d;
		}
		if (patch != nullptr && byTag != nullptr) {
			for (const RackPatch::Device& named : patch->device) {
				if (named.instanceId == id) {
					return byTag(host, named.tag.data());
				}
			}
		}
		return nullptr;
	}
};

/// A ROUTE AS THE FILE HOLDS IT, or false when it cannot be — a device end this engine does not run, a
/// control or output the device does not declare, a host end the codec cannot name. BY IDENTITY AND KEY:
/// the same road `patchOfRecord` takes, so an engine-saved and a record-saved patch spell a route
/// identically.
bool describe(const CoreCodec& codec, const Holder& holder, const backplane::Route& route, RackPatch::Route* out) {
	*out = RackPatch::Route {};
	out->amount = route.amount;
	out->transfer = route.transfer;
	if (route.source.isOuter()) {
		// A SOURCE OUTSIDE THIS RACK (rack roadmap §7): the outer id and the index with its scope bit, whole.
		// Numbered, not keyed: the outer device's descriptor is the outer host's, and not in reach here.
		out->source = PatchCoord { static_cast<uint32_t>(SourceKind::Outer), route.source.device, route.source.index, {} };
	}
	else if (route.source.isExport()) {
		// A SOURCE IN THE DEVICE'S PRE CHAIN (backlog NZ), or INSIDE A CONTAINER (NZ (b)), each its own kind: the holder's
		// id, and the row of that table with its scope bit, whole. The row is the table's own name, saved in its body.
		if (holder.descOf(holder.host, route.source.device) == nullptr) {
			return false;
		}
		const SourceKind kind = route.source.isSiteExport() ? SourceKind::Export : SourceKind::ContainerExport;
		out->source = PatchCoord { static_cast<uint32_t>(kind), route.source.device, route.source.index, {} };
	}
	else if (route.source.isDevice()) {
		const device::Descriptor* desc = holder.descOf(holder.host, route.source.device);
		if (desc == nullptr || route.source.index >= desc->outputs.size()) {
			return false;
		}
		out->source = PatchCoord { static_cast<uint32_t>(SourceKind::Device), route.source.device, 0,
			                       desc->outputs[route.source.index].key };
	}
	else {
		uint32_t kind = 0;
		uint32_t a = 0;
		if (codec.nameSource == nullptr || !codec.nameSource(codec.host, route.source.index, &kind, &a)) {
			return false;
		}
		out->source = PatchCoord { kind, a, 0, {} };
	}
	if (route.destination.isDevice()) {
		const device::Descriptor* desc = holder.descOf(holder.host, route.destination.device);
		if (desc == nullptr || route.destination.index >= desc->params.size()) {
			return false;
		}
		out->destination = PatchCoord { static_cast<uint32_t>(DestKind::Device), route.destination.device, 0,
			                            desc->params[route.destination.index].key };
	}
	else {
		uint32_t kind = 0;
		uint32_t a = 0;
		if (codec.nameDestination == nullptr
		    || !codec.nameDestination(codec.host, route.destination.index, &kind, &a)) {
			return false;
		}
		out->destination = PatchCoord { kind, a, 0, {} };
	}
	return true;
}

/// AN ID A FILE MAY NAME: one the counter can issue. `kNoInstance` names nothing and the top of the id space
/// is the host's own node, so a file naming either is read as naming no id at all — the device takes a fresh
/// one, exactly as it does when the id is missing or already held.
DeviceInstanceId namedBy(DeviceInstanceId fileId) {
	return fileId >= backplane::InstanceCounter::kCeiling ? kNoInstance : fileId;
}

/// THE WAY BACK FOR AN ENGINE: which id a device the patch named by `named` WEARS in the engine — the
/// same id when the engine could grant it, a fresh one when it could not (`adoptDevice` hands a wanted id
/// back on collision and the loader tries again without it). **AN ID TWO DEVICES WORE RESOLVES NOWHERE**
/// — refused rather than arbitrated, exactly as `WornIds` below refuses for a record. A file id of
/// `kNoInstance` is recorded for nothing: it names no device, so no route may name it either.
struct Placement {
	std::vector<std::pair<DeviceInstanceId, DeviceInstanceId>> wornForNamed;

	void note(DeviceInstanceId named, DeviceInstanceId worn) {
		if (named != kNoInstance) {
			wornForNamed.emplace_back(named, worn);
		}
	}
	DeviceInstanceId wornFor(DeviceInstanceId named) const {
		DeviceInstanceId found = kNoInstance;
		for (const auto& [i, worn] : wornForNamed) {
			if (i == named) {
				if (found != kNoInstance) {
					return kNoInstance;
				}
				found = worn;
			}
		}
		return found;
	}
};

/// THE SAME MAP FOR A RECORD'S ROAD — there a device is its id and nothing else, and what the two roads
/// share is exactly this: which id the file named, which one was granted, refused when two wore one.
using WornIds = Placement;

/// **ONE END OF A ROUTE AS THE FILE ASKED FOR IT** (decided 2026-09-12; `RouteRefusals` carries the whole
/// reasoning). Both roads use these, and the record road uses them for the route it is about to MAKE as
/// well as for the report of one it refuses — the two are the same expression, because an id that resolves
/// is never a renamed one.
///
/// A DEVICE END IS THE ID THE PATCH WROTE, VERBATIM, and the index its KEY names on the device that id holds
/// now (`descOf`). Resolving the id belongs to whoever holds the devices, and its refusal is the honest
/// answer. **A KEY THE DEVICE DOES NOT DECLARE NAMES NO CONTROL**, so that end is `nobody()` — an index
/// invented for it would name some other control, which is the silent re-point keys exist to end — and the
/// report carries the key in words instead.
///
/// A HOST END IS WHEREVER THE HOST'S CODEC PLACES IT. `nobody()` for the ends that cannot be spelled as an
/// endpoint at all: a host kind the codec refuses outright, and a device end the file wrote under
/// `kNoInstance` — `output(kNoInstance, b)` IS `core(b)`, so spelling that one would report a device end as
/// a host end rather than as the nothing it is.
backplane::Source sourceOfPatchCoord(const PatchCoord& coord, const CoreCodec& codec, const Holder& holder) {
	if (isExportKind(coord.kind)) {
		const backplane::Source s { coord.a, coord.b }; // the holder and its row, as the record holds them
		const bool site = coord.kind == static_cast<uint32_t>(SourceKind::Export);
		return (site ? s.isSiteExport() && !s.isContainerExport() : s.isContainerExport() && !s.isSiteExport())
		           ? s
		           : backplane::Source::nobody(); // a kind and a bit that disagree name nothing
	}
	if (coord.kind == static_cast<uint32_t>(SourceKind::Outer)) {
		return backplane::Source { coord.a, coord.b }; // spelled as the record holds it — see `SourceKind::Outer`
	}
	if (coord.kind == static_cast<uint32_t>(SourceKind::Device)) {
		if (coord.a == kNoInstance) {
			return backplane::Source::nobody();
		}
		const device::Descriptor* desc = holder.declaredOf(coord.a);
		const size_t index = desc != nullptr ? outputIndexOf(*desc, coord.key) : kNoIndex;
		if (desc != nullptr && index == kNoIndex) {
			return backplane::Source::nobody();
		}
		// A DEVICE THAT IS NOT HERE: its id is still the honest report; its output index is unknowable, so 0.
		return backplane::Source::output(coord.a, index == kNoIndex ? 0 : index);
	}
	size_t core = 0;
	if (codec.placeSource != nullptr && codec.placeSource(codec.host, coord.kind, coord.a, &core)) {
		return backplane::Source::core(core);
	}
	return backplane::Source::nobody();
}

backplane::Destination destinationOfPatchCoord(const PatchCoord& coord, const CoreCodec& codec, const Holder& holder) {
	if (coord.kind == static_cast<uint32_t>(DestKind::Device)) {
		if (coord.a == kNoInstance) {
			return backplane::Destination::nobody();
		}
		const device::Descriptor* desc = holder.declaredOf(coord.a);
		const size_t index = desc != nullptr ? paramIndexOf(*desc, coord.key) : kNoIndex;
		if (desc != nullptr && index == kNoIndex) {
			return backplane::Destination::nobody();
		}
		return backplane::Destination::control(coord.a, index == kNoIndex ? 0 : index);
	}
	size_t core = 0;
	if (codec.placeDestination != nullptr && codec.placeDestination(codec.host, coord.kind, coord.a, &core)) {
		return backplane::Destination::core(core);
	}
	return backplane::Destination::nobody();
}

/// A FILE ROUTE AS ENDPOINTS THIS ENGINE CAN TAKE, or false — a device end the engine did not build, a
/// key it does not declare, a host end the codec cannot place. Only the ID is translated, from the one the
/// file named to the one the engine granted; the key is turned into the device's own index.
bool resolve(const CoreCodec& codec, const Placement& placed, const Holder& holder, const RackPatch::Route& in,
             backplane::Source* source, backplane::Destination* destination) {
	*source = backplane::Source::nobody();
	*destination = backplane::Destination::nobody();
	bool ok = true;
	if (in.source.kind == static_cast<uint32_t>(SourceKind::Outer)) {
		// A SOURCE OUTSIDE THIS RACK (rack roadmap §7): spelled the OUTER host's way, and NOT remapped by
		// this rack's placement — the id is the outer host's. The record's gate, asked next, decides
		// whether the host outside still has it upstream; a source gone from outside is refused there.
		*source = backplane::Source { in.source.a, in.source.b };
		if (!source->isOuter()) {
			ok = false; // an `Outer` coordinate must carry the scope bit; a file that lost it names nothing
		}
	}
	else if (isExportKind(in.source.kind)) {
		// A HOLDER'S PRE EXPORT (backlog NZ) or a CONTAINER'S (NZ (b)): the holder's id translated like any device end,
		// the row kept whole, and the kind and the bit must agree.
		const DeviceInstanceId worn = placed.wornFor(in.source.a);
		*source = backplane::Source { worn, in.source.b };
		const bool site = in.source.kind == static_cast<uint32_t>(SourceKind::Export);
		if (worn == kNoInstance || source->isOuter()
		    || !(site ? source->isSiteExport() && !source->isContainerExport()
		              : source->isContainerExport() && !source->isSiteExport())) {
			ok = false;
		}
	}
	else if (in.source.kind == static_cast<uint32_t>(SourceKind::Device)) {
		const DeviceInstanceId worn = placed.wornFor(in.source.a);
		const device::Descriptor* desc = worn != kNoInstance ? holder.descOf(holder.host, worn) : nullptr;
		const size_t index = desc != nullptr ? outputIndexOf(*desc, in.source.key) : kNoIndex;
		if (index == kNoIndex) {
			ok = false;
		}
		else {
			*source = backplane::Source::output(worn, index);
		}
	}
	else {
		size_t index = 0;
		if (codec.placeSource == nullptr || !codec.placeSource(codec.host, in.source.kind, in.source.a, &index)) {
			ok = false;
		}
		else {
			*source = backplane::Source::core(index);
		}
	}
	if (in.destination.kind == static_cast<uint32_t>(DestKind::Device)) {
		const DeviceInstanceId worn = placed.wornFor(in.destination.a);
		const device::Descriptor* desc = worn != kNoInstance ? holder.descOf(holder.host, worn) : nullptr;
		const size_t index = desc != nullptr ? paramIndexOf(*desc, in.destination.key) : kNoIndex;
		if (index == kNoIndex || !desc->params[index].modulatable) {
			ok = false;
		}
		else {
			*destination = backplane::Destination::control(worn, index);
		}
	}
	else {
		size_t index = 0;
		if (codec.placeDestination == nullptr
		    || !codec.placeDestination(codec.host, in.destination.kind, in.destination.a, &index)) {
			ok = false;
		}
		else {
			*destination = backplane::Destination::core(index);
		}
	}
	return ok;
}

/// **THE FIRST REFUSAL IN FULL, BY IDENTITY; THE REST COUNTED** — and, since the ends became keys, the
/// route's two ends IN WORDS as the file spelled them, so a key that names nothing can still be said.
void noteRefusal(LoadReport* report, const RackPatch::Route& route, backplane::Source source,
                 backplane::Destination destination) {
	const auto spell = [](const PatchCoord& c) {
		if (!c.key.empty()) {
			return "device " + std::to_string(c.a) + " '" + c.key + "'";
		}
		return "kind " + std::to_string(c.kind) + " #" + std::to_string(c.a);
	};
	if (report->routes.count == 0) {
		report->first[static_cast<size_t>(LoadReport::Kind::RouteDropped)] =
		    LoadReport::First { "patch", spell(route.source) + " -> " + spell(route.destination), {} };
	}
	report->noteRoute(source, destination);
}

void copyTag(const char* from, std::array<char, device::kTagBytes + 1>* to) {
	*to = {};
	for (size_t c = 0; c < device::kTagBytes && from != nullptr && from[c] != '\0'; ++c) {
		(*to)[c] = from[c];
	}
}

// THE RACK'S CODEC — macros by index; core destinations (none today) by index.
bool rackNameSource(const void* host, size_t source, uint32_t* kind, uint32_t* a) {
	const auto* axes = static_cast<const backplane::Axes*>(host);
	if (source >= axes->coreSources) {
		return false;
	}
	*kind = static_cast<uint32_t>(SourceKind::Macro);
	*a = static_cast<uint32_t>(source);
	return true;
}
bool rackPlaceSource(const void* host, uint32_t kind, uint32_t a, size_t* source) {
	const auto* axes = static_cast<const backplane::Axes*>(host);
	if (kind != static_cast<uint32_t>(SourceKind::Macro) || a >= axes->coreSources) {
		return false;
	}
	*source = a;
	return true;
}
bool rackNameDestination(const void* host, size_t destination, uint32_t* kind, uint32_t* a) {
	const auto* axes = static_cast<const backplane::Axes*>(host);
	if (destination >= axes->coreDestinations) {
		return false;
	}
	*kind = static_cast<uint32_t>(DestKind::Param);
	*a = static_cast<uint32_t>(destination);
	return true;
}
bool rackPlaceDestination(const void* host, uint32_t kind, uint32_t a, size_t* destination) {
	const auto* axes = static_cast<const backplane::Axes*>(host);
	if (kind != static_cast<uint32_t>(DestKind::Param) || a >= axes->coreDestinations) {
		return false;
	}
	*destination = a;
	return true;
}

//------------------------------------------------------------------------------------------------//
// ROUTE ENDS AS TEXT                                                                             //
//------------------------------------------------------------------------------------------------//

bool writeSourceEnd(json::Writer& w, const PatchCoord& c) {
	w.beginObject();
	switch (static_cast<SourceKind>(c.kind)) {
	case SourceKind::Macro:
		w.key("macro");
		w.number(c.a);
		break;
	case SourceKind::Weight:
		w.key("weight");
		w.number(c.a);
		break;
	case SourceKind::Device:
		if (c.key.empty()) {
			return false; // a device output named by position is exactly what this format refuses to write
		}
		w.key("device");
		w.number(c.a);
		w.key("out");
		w.string(c.key);
		break;
	case SourceKind::Stack:
		w.key("stack");
		w.number(c.a);
		break;
	case SourceKind::Export:
		w.key("export");
		w.number(c.a);
		w.key("index");
		w.number(c.b);
		break;
	case SourceKind::ContainerExport:
		w.key("inside");
		w.number(c.a);
		w.key("index");
		w.number(c.b);
		break;
	case SourceKind::Outer: {
		// THE HOP COUNT IS ITS OWN KEY (2026-10-31), so the index carries no field whose position the encoding may move:
		// the count widened from bits 24–27 to 23–27 and a file that spelled it inside `index` would have read wrong.
		const uint32_t hops = (c.b & backplane::Source::kOuterDepthMask) >> backplane::Source::kOuterDepthShift;
		w.key("outer");
		w.number(c.a);
		w.key("index");
		w.number(c.b & ~backplane::Source::kOuterDepthMask);
		if (hops != 0) {
			w.key("hops");
			w.number(hops);
		}
		break;
	}
	default:
		w.key("kind");
		w.number(c.kind);
		w.key("a");
		w.number(c.a);
		break;
	}
	w.endObject();
	return true;
}

bool writeDestinationEnd(json::Writer& w, const PatchCoord& c) {
	w.beginObject();
	switch (static_cast<DestKind>(c.kind)) {
	case DestKind::Param:
		w.key("param");
		w.number(c.a);
		break;
	case DestKind::Internal:
		w.key("internal");
		w.number(c.a);
		break;
	case DestKind::Device:
		if (c.key.empty()) {
			return false;
		}
		w.key("device");
		w.number(c.a);
		w.key("control");
		w.string(c.key);
		break;
	default:
		w.key("kind");
		w.number(c.kind);
		w.key("a");
		w.number(c.a);
		break;
	}
	w.endObject();
	return true;
}

bool readU32(const json::Value& o, const char* name, uint32_t* out) {
	const json::Value* v = o.find(name);
	return v != nullptr && v->asU32(out);
}

bool readSourceEnd(const json::Value* node, PatchCoord* out) {
	*out = PatchCoord {};
	if (node == nullptr || !node->isObject()) {
		return false;
	}
	if (readU32(*node, "macro", &out->a)) {
		out->kind = static_cast<uint32_t>(SourceKind::Macro);
		return true;
	}
	if (readU32(*node, "weight", &out->a)) {
		out->kind = static_cast<uint32_t>(SourceKind::Weight);
		return true;
	}
	if (readU32(*node, "stack", &out->a)) {
		out->kind = static_cast<uint32_t>(SourceKind::Stack);
		return true;
	}
	if (readU32(*node, "export", &out->a)) {
		out->kind = static_cast<uint32_t>(SourceKind::Export);
		return readU32(*node, "index", &out->b);
	}
	if (readU32(*node, "inside", &out->a)) {
		out->kind = static_cast<uint32_t>(SourceKind::ContainerExport);
		return readU32(*node, "index", &out->b);
	}
	if (readU32(*node, "outer", &out->a)) {
		out->kind = static_cast<uint32_t>(SourceKind::Outer);
		if (!readU32(*node, "index", &out->b)) {
			return false;
		}
		// A file from before the `hops` key spelled the count in bits 24–27 of `index` (four bits, at most 15 more hosts);
		// a file written since has those bits clear, so the two spellings cannot be mistaken for each other.
		constexpr uint32_t kLegacyShift = 24;
		constexpr uint32_t kLegacyMask = 0xFu << kLegacyShift;
		uint32_t hops = (out->b & kLegacyMask) >> kLegacyShift;
		readU32(*node, "hops", &hops);
		if (hops >= backplane::Source::kMaxOuterHops) {
			return false;
		}
		out->b = (out->b & ~backplane::Source::kOuterDepthMask) | (hops << backplane::Source::kOuterDepthShift);
		return true;
	}
	if (readU32(*node, "device", &out->a)) {
		const json::Value* key = node->find("out");
		out->kind = static_cast<uint32_t>(SourceKind::Device);
		if (key == nullptr || !key->isString()) {
			return false;
		}
		out->key = key->text;
		return true;
	}
	return readU32(*node, "kind", &out->kind) && readU32(*node, "a", &out->a);
}

bool readDestinationEnd(const json::Value* node, PatchCoord* out) {
	*out = PatchCoord {};
	if (node == nullptr || !node->isObject()) {
		return false;
	}
	if (readU32(*node, "param", &out->a)) {
		out->kind = static_cast<uint32_t>(DestKind::Param);
		return true;
	}
	if (readU32(*node, "internal", &out->a)) {
		out->kind = static_cast<uint32_t>(DestKind::Internal);
		return true;
	}
	if (readU32(*node, "device", &out->a)) {
		const json::Value* key = node->find("control");
		out->kind = static_cast<uint32_t>(DestKind::Device);
		if (key == nullptr || !key->isString()) {
			return false;
		}
		out->key = key->text;
		return true;
	}
	return readU32(*node, "kind", &out->kind) && readU32(*node, "a", &out->a);
}

//------------------------------------------------------------------------------------------------//
// THE HOST SLOT POOL (host-slots plan §2, T4)                                                    //
//------------------------------------------------------------------------------------------------//

/// A SLOT'S STATE AS A WORD, never its enum's number: the enum is T1's to reorder, the file is not.
const char* slotStateName(hostslots::SlotState s) {
	switch (s) {
	case hostslots::SlotState::Empty: return "empty";
	case hostslots::SlotState::Default: return "default";
	case hostslots::SlotState::Recent: return "recent";
	case hostslots::SlotState::Pinned: return "pinned";
	case hostslots::SlotState::Anchored: return "anchored";
	}
	return "";
}

/// The word back. "empty" reads (it names a state) and `restore` refuses it, as it would from any source.
bool slotStateOf(const json::Value* node, hostslots::SlotState* out) {
	if (node == nullptr || !node->isString()) {
		return false;
	}
	for (const hostslots::SlotState s : { hostslots::SlotState::Empty, hostslots::SlotState::Default,
	                                      hostslots::SlotState::Recent, hostslots::SlotState::Pinned,
	                                      hostslots::SlotState::Anchored }) {
		if (node->text == slotStateName(s)) {
			*out = s;
			return true;
		}
	}
	return false;
}

void writeHostSlots(json::Writer& w, const hostslots::PoolSnapshot& snap) {
	w.beginObject();
	w.key("capacity");
	w.number(snap.capacity);
	w.key("clock");
	w.number(snap.clock);
	w.key("slots");
	w.beginArray();
	for (const hostslots::SlotRecord& r : snap.slots) {
		w.beginObject();
		w.key("n");
		w.number(r.index);
		w.key("state");
		w.string(slotStateName(r.state));
		w.key("path");
		w.beginArray();
		for (const DeviceInstanceId id : r.target.path) {
			w.number(id);
		}
		w.endArray();
		w.key("tag");
		w.string(r.target.tag);
		w.key("key");
		w.string(r.target.key);
		w.key("label");
		w.string(r.target.label);
		w.key("recent");
		w.number(r.recency);
		w.key("level");
		w.number(int64_t { r.level });
		w.key("rank");
		w.number(int64_t { r.rank });
		w.key("chainOrder");
		w.number(int64_t { r.chainOrder });
		// WRITTEN FOR EVERY SLOT, not only an anchored one: a released slot keeps the value it released to,
		// and a round trip that dropped it would not give back the snapshot it was handed.
		w.key("beforeAnchor");
		w.string(slotStateName(r.beforeAnchor));
		w.endObject();
	}
	w.endArray();
	// THE HOST MARKS (2026-09-26), only when there are any, so a pool the host never touched writes exactly
	// what it did before the mark existed. An absent member reads as none.
	if (!snap.hostMarked.empty()) {
		w.key("hostMarked");
		w.beginArray();
		for (const hostslots::SlotIndex m : snap.hostMarked) {
			w.number(m);
		}
		w.endArray();
	}
	w.endObject();
}

bool readI32(const json::Value& o, const char* name, int32_t* out) {
	const json::Value* v = o.find(name);
	int64_t n = 0;
	if (v == nullptr || !v->asI64(&n) || n < INT32_MIN || n > INT32_MAX) {
		return false;
	}
	*out = static_cast<int32_t>(n);
	return true;
}

bool readString(const json::Value& o, const char* name, std::string* out) {
	const json::Value* v = o.find(name);
	if (v == nullptr || !v->isString()) {
		return false;
	}
	*out = v->text;
	return true;
}

/// **THE POOL, OR WHY NOT.** Every field is required: a Default without its tiers is taken over in the wrong
/// order, an Anchored slot without `beforeAnchor` releases to the wrong state, and a slot without its label
/// shows the host no name (plan §2). Then `restore`'s own validation, on a scratch pool, so a snapshot this
/// hands back is one a pool will take.
bool readHostSlots(const json::Value& node, hostslots::PoolSnapshot* out, std::string* why) {
	if (!node.isObject()) {
		*why = "not an object";
		return false;
	}
	hostslots::PoolSnapshot snap;
	const json::Value* clock = node.find("clock");
	if (!readU32(node, "capacity", &snap.capacity) || clock == nullptr || !clock->asU64(&snap.clock)) {
		*why = "no capacity or clock";
		return false;
	}
	// THE POOL'S OWN LIMIT (`hostslots::kMaxHostSlotCapacity`), checked before the list is read, so a file
	// cannot make this build read four billion slots only to have `whyInvalid` refuse them.
	if (snap.capacity > hostslots::kMaxHostSlotCapacity) {
		*why = hostslots::SlotPool::whyInvalid(snap);
		return false;
	}
	const json::Value* slots = node.find("slots");
	if (slots == nullptr || !slots->isArray() || slots->items.size() > hostslots::kMaxHostSlotCapacity) {
		*why = "the slots are not a list";
		return false;
	}
	for (const json::Value& s : slots->items) {
		hostslots::SlotRecord r;
		const json::Value* path = s.find("path");
		const json::Value* recent = s.find("recent");
		if (!s.isObject() || !readU32(s, "n", &r.index) || !slotStateOf(s.find("state"), &r.state)
		    || path == nullptr || !path->isArray() || path->items.size() > kMaxCount
		    || !readString(s, "tag", &r.target.tag) || !readString(s, "key", &r.target.key)
		    || !readString(s, "label", &r.target.label) || recent == nullptr || !recent->asU64(&r.recency)
		    || !readI32(s, "level", &r.level) || !readI32(s, "rank", &r.rank)
		    || !readI32(s, "chainOrder", &r.chainOrder) || !slotStateOf(s.find("beforeAnchor"), &r.beforeAnchor)) {
			// BY ITS PLACE IN THE LIST, not "slot N": its index may be the field that did not read, and "slot N"
			// is `whyInvalid`'s name for an index.
			*why = "entry " + std::to_string(snap.slots.size() + 1) + " of the slot list could not be read";
			return false;
		}
		for (const json::Value& id : path->items) {
			DeviceInstanceId v = kNoInstance;
			if (!id.asU32(&v)) {
				// asU32: a whole number with no sign, no fraction and no exponent that fits 32 bits, so -1,
				// 3.0 and 2^32 are all refused here rather than wrapped or truncated into some other device.
				*why = "slot " + std::to_string(r.index) + " has a path that is not device ids";
				return false;
			}
			r.target.path.push_back(v);
		}
		snap.slots.push_back(std::move(r));
	}
	// THE HOST MARKS: OPTIONAL (absent is none), and when present a list of slot indices — `whyInvalid` below
	// checks their range and order.
	if (const json::Value* marked = node.find("hostMarked"); marked != nullptr) {
		if (!marked->isArray() || marked->items.size() > hostslots::kMaxHostSlotCapacity) {
			*why = "the host marks are not a list";
			return false;
		}
		for (const json::Value& m : marked->items) {
			hostslots::SlotIndex n = 0;
			if (!m.asU32(&n)) {
				*why = "a host mark is not a slot index";
				return false;
			}
			snap.hostMarked.push_back(n);
		}
	}
	// `restore`'s OWN VALIDATION, so a snapshot this hands back is one a pool takes — and its reason, so the
	// report says which check failed.
	if (std::string invalid = hostslots::SlotPool::whyInvalid(snap); !invalid.empty()) {
		*why = std::move(invalid);
		return false;
	}
	*out = std::move(snap);
	return true;
}

bool notLoaded(LoadReport* report, const std::string& why) {
	if (report != nullptr) {
		report->note(LoadReport::Kind::NotLoaded, "patch", "", why);
	}
	return false;
}

} // namespace

CoreCodec rackCoreCodec(const backplane::Axes& axes) {
	return CoreCodec { &axes, &rackNameSource, &rackPlaceSource, &rackNameDestination, &rackPlaceDestination };
}

//------------------------------------------------------------------------------------------------//
// THE LOAD REPORT                                                                                //
//------------------------------------------------------------------------------------------------//

void LoadReport::merge(const LoadReport& other) {
	for (size_t k = 0; k < kKinds; ++k) {
		if (count[k] == 0 && other.count[k] != 0) {
			first[k] = other.first[k];
		}
		count[k] += other.count[k];
	}
	if (routes.count == 0) {
		routes.firstSource = other.routes.firstSource;
		routes.firstDestination = other.routes.firstDestination;
	}
	routes.count += other.routes.count;
}

void LoadReport::mergeRoutes(const RouteRefusals& other) {
	if (other.count == 0) {
		return;
	}
	if (routes.count == 0) {
		routes.firstSource = other.firstSource;
		routes.firstDestination = other.firstDestination;
	}
	routes.count += other.count;
	count[static_cast<size_t>(Kind::RouteDropped)] += static_cast<uint32_t>(other.count);
}

std::string deviceWhere(const char* name, size_t chain, size_t position) {
	return std::string(name != nullptr ? name : "?") + " (chain " + std::to_string(chain + 1) + ", #"
	       + std::to_string(position + 1) + ")";
}

std::string loadReportLine(const LoadReport& report, const std::string& routePhrase) {
	using Kind = LoadReport::Kind;
	if (report.empty()) {
		return {};
	}
	if (report.countOf(Kind::NotLoaded) != 0) {
		const LoadReport::First& f = report.firstOf(Kind::NotLoaded);
		return "Not loaded: " + (f.why.empty() ? std::string("the patch could not be read") : f.why)
		       + ". Nothing changed.";
	}
	// THE HEADLINE: the most serious kind, its first example in full.
	const auto headline = [&](Kind k) -> std::string {
		const LoadReport::First& f = report.firstOf(k);
		const uint32_t n = report.countOf(k);
		switch (k) {
		case Kind::RouteDropped:
			return "dropped " + std::string(n == 1 ? "route: " : std::to_string(n) + " routes: ")
			       + (routePhrase.empty() ? f.what : routePhrase)
			       + (n > 1 ? ", +" + std::to_string(n - 1) + " more" : std::string {});
		case Kind::SlotsDropped:
			return "the host slot map could not be read (" + f.why + "); the host's lanes may drive other controls";
		case Kind::DeviceUnknown: return "'" + f.what + "' is not a device this build has (kept in its place)";
		case Kind::BodyRefused: return f.where + " would not take its contents" + (f.why.empty() ? "" : " (" + f.why + ")");
		case Kind::NewerSchema: return f.where + " was saved by a newer build" + (f.why.empty() ? "" : " (" + f.why + ")");
		case Kind::KeyUnknown: return f.where + ": '" + f.what + "' is not a control this build has (kept)";
		case Kind::KeyRetired: return f.where + ": '" + f.what + "' was retired — " + f.why;
		case Kind::LabelUnknown: return f.where + ": '" + f.why + "' is not a choice this build has for '" + f.what + "' (at its default)";
		case Kind::ValueCoerced: return f.where + ": '" + f.what + "' " + (f.why.empty() ? "was out of range" : f.why);
		case Kind::KeyMissing:
			return n == 1 ? f.where + ": new control '" + f.what + "' at its default"
			              : std::to_string(n) + " new controls at their defaults (first " + f.where + " '" + f.what + "')";
		case Kind::NotLoaded: break;
		}
		return {};
	};
	const auto counted = [](Kind k, uint32_t n) -> std::string {
		const std::string c = std::to_string(n);
		switch (k) {
		case Kind::RouteDropped: return c + (n == 1 ? " route dropped" : " routes dropped");
		case Kind::SlotsDropped: return "the host slot map dropped";
		case Kind::DeviceUnknown: return c + (n == 1 ? " unknown device" : " unknown devices");
		case Kind::BodyRefused: return c + (n == 1 ? " device's contents refused" : " devices' contents refused");
		case Kind::NewerSchema: return c + (n == 1 ? " device from a newer build" : " devices from a newer build");
		case Kind::KeyUnknown: return c + (n == 1 ? " value from a newer build" : " values from a newer build");
		case Kind::KeyRetired: return c + (n == 1 ? " retired value" : " retired values");
		case Kind::LabelUnknown: return c + (n == 1 ? " unknown choice" : " unknown choices");
		case Kind::ValueCoerced: return c + (n == 1 ? " value corrected" : " values corrected");
		case Kind::KeyMissing: return c + (n == 1 ? " new control at its default" : " new controls at their defaults");
		case Kind::NotLoaded: break;
		}
		return {};
	};
	std::string line;
	std::string rest;
	for (size_t k = static_cast<size_t>(Kind::RouteDropped); k < LoadReport::kKinds; ++k) {
		const auto kind = static_cast<Kind>(k);
		const uint32_t n = report.countOf(kind);
		if (n == 0) {
			continue;
		}
		if (line.empty()) {
			line = headline(kind);
		}
		else {
			rest += (rest.empty() ? "" : ", ") + counted(kind, n);
		}
	}
	// ROUTES ALONE READ AS THEY ALWAYS HAVE — a patch with nothing else to say is not "loaded with changes".
	if (rest.empty() && report.countOf(Kind::RouteDropped) != 0) {
		line[0] = 'D'; // "dropped …" as a sentence of its own
		return line;
	}
	return "Loaded with changes: " + line + (rest.empty() ? "" : " (+ " + rest + ")");
}

std::string loadReportDetail(const LoadReport& report) {
	static constexpr std::array<const char*, LoadReport::kKinds> kNames = {
		"not-loaded", "route-dropped", "slots-dropped", "device-unknown", "body-refused", "newer-schema",
		"key-unknown", "key-retired", "label-unknown", "value-coerced", "key-missing",
	};
	std::string out;
	for (size_t k = 0; k < LoadReport::kKinds; ++k) {
		const uint32_t n = report.count[k];
		if (n == 0) {
			continue;
		}
		const LoadReport::First& f = report.first[k];
		out += (out.empty() ? "" : " ") + std::string(kNames[k]) + "=" + std::to_string(n) + " [" + f.where + " | "
		       + f.what + (f.why.empty() ? "" : " | " + f.why) + "]";
	}
	return out;
}

//------------------------------------------------------------------------------------------------//
// KEYS ↔ A ROW                                                                                   //
//------------------------------------------------------------------------------------------------//

void valuesOfRow(const device::Descriptor& d, std::span<const float> fields, size_t fieldsPresent,
                 RackPatch::Device* out) {
	out->values.clear();
	out->extra.clear();
	out->schema = d.traits.schema;
	const size_t present = std::min(fieldsPresent, fields.size());
	out->values.reserve(d.params.size());
	for (size_t i = 0; i < d.params.size(); ++i) {
		const device::ParamDesc& p = d.params[i];
		// A CONTROL PAST WHAT THE ROW HOLDS IS WHAT THE DEVICE READS FOR IT: its declared default.
		const float v = i < present ? fields[i] : static_cast<float>(p.def);
		if (storedByLabel(p)) {
			out->values.push_back(RackPatch::Value::label(p.key, labelOf(p, v)));
		}
		else {
			out->values.push_back(RackPatch::Value::of(p.key, v));
		}
	}
	if (d.traits.extraStateKey != nullptr && present > d.params.size()) {
		const size_t n = std::min(present - d.params.size(), d.traits.extraState);
		std::string text;
		json::Writer w(&text, false);
		w.beginArray();
		for (size_t k = 0; k < n; ++k) {
			w.number(fields[d.params.size() + k]);
		}
		w.endArray();
		out->extra.push_back(RackPatch::Value { d.traits.extraStateKey, RackPatch::Value::Kind::Raw, 0.0f, text });
	}
}

//------------------------------------------------------------------------------------------------//
// THE KEYED BLOCKS (backlog NM, `device/state_blocks.h`)                                         //
//------------------------------------------------------------------------------------------------//

namespace {

/// THE INDEX OF THE BLOCK `key` NAMES IN `d`, or `kNoIndex`.
size_t blockIndexOf(const device::Descriptor& d, std::string_view key) {
	const size_t i = device::blockIndexOf(d.state, key);
	return d.state != nullptr && i < d.state->blocks.size() ? i : kNoIndex;
}

/// ONE BLOCK AS THE DOCUMENT STORES IT: a `Raw` JSON array under its key, the row's extra state's shape.
RackPatch::Value blockValue(const char* key, std::span<const float> floats) {
	std::string text;
	json::Writer w(&text, false);
	w.beginArray();
	for (const float f : floats) {
		w.number(f);
	}
	w.endArray();
	return RackPatch::Value { key, RackPatch::Value::Kind::Raw, 0.0f, text };
}

/// **EVERY BLOCK THE DEVICE HAS SOMETHING TO SAY IN, appended to `out->extra`**: what the running instance last
/// published (`surface`, read under the block's seqlock), else what the store holds (a load not built yet, or an
/// instance that has not ticked since). A block that is neither is not written: the device is as freshly built.
void writeBlocks(const device::Descriptor& d, const void* surface, const ContainerBodies* bodies, DeviceInstanceId id,
                 RackPatch::Device* out) {
	if (d.state == nullptr) {
		return;
	}
	const std::vector<ContainerBodies::Block>* stored = bodies != nullptr ? bodies->blocksOf(id) : nullptr;
	for (size_t k = 0; k < d.state->blocks.size(); ++k) {
		const device::StateBlockDesc& b = d.state->blocks[k];
		std::vector<float> floats(b.floats);
		if (surface != nullptr && d.state->read != nullptr && d.state->read(surface, k, floats.data(), floats.size()) != 0) {
			out->extra.push_back(blockValue(b.key, floats));
			continue;
		}
		for (size_t s = 0; stored != nullptr && s < stored->size(); ++s) {
			if ((*stored)[s].index == k && (*stored)[s].floats.size() == b.floats) {
				out->extra.push_back(blockValue(b.key, (*stored)[s].floats));
				break;
			}
		}
	}
}

/// **A PATCH DEVICE'S BLOCKS, PARSED** — those under a key `d` declares and of the length it declares. Anything else
/// under a block key is said and dropped: a block is a device's own state, and one of another length is from a layout
/// this build does not read (which would have been given a new key).
std::vector<ContainerBodies::Block> blocksOfPatch(const device::Descriptor& d, const RackPatch::Device& in,
                                                  LoadReport* report, const std::string& where) {
	std::vector<ContainerBodies::Block> out;
	for (const RackPatch::Value& e : in.extra) {
		const size_t k = blockIndexOf(d, e.key);
		if (k == kNoIndex) {
			continue;
		}
		const device::StateBlockDesc& b = d.state->blocks[k];
		json::Value array;
		std::vector<float> floats;
		bool whole = e.kind == RackPatch::Value::Kind::Raw && json::parse(e.text, &array) && array.isArray()
		             && array.items.size() == b.floats;
		for (size_t i = 0; whole && i < array.items.size(); ++i) {
			float f = 0.0f;
			whole = array.items[i].asFloat(&f) && std::isfinite(f);
			floats.push_back(f);
		}
		if (!whole) {
			if (report != nullptr) {
				report->note(LoadReport::Kind::ValueCoerced, where, e.key,
				             "was not " + std::to_string(b.floats) + " numbers (left out: the device starts fresh)");
			}
			continue;
		}
		out.push_back({ k, std::move(floats) });
	}
	return out;
}

/// THE SAME BLOCKS, STRAIGHT INTO AN INSTANCE NOTHING RUNS YET (`applyRack`'s road, which has no store).
void restoreBlocksOfPatch(const device::Descriptor& d, void* storage, const RackPatch::Device& in, LoadReport* report,
                          const std::string& where) {
	if (d.state == nullptr || d.state->restore == nullptr) {
		return;
	}
	for (const ContainerBodies::Block& b : blocksOfPatch(d, in, report, where)) {
		(void)d.state->restore(storage, b.index, b.floats.data(), b.floats.size());
	}
}

} // namespace

size_t restoreBlocks(const device::Descriptor& d, void* storage, const ContainerBodies& bodies, DeviceInstanceId id) {
	const std::vector<ContainerBodies::Block>* stored = bodies.blocksOf(id);
	if (d.state == nullptr || d.state->restore == nullptr || storage == nullptr || stored == nullptr) {
		return 0;
	}
	size_t taken = 0;
	for (const ContainerBodies::Block& b : *stored) {
		if (b.index < d.state->blocks.size() && b.floats.size() == d.state->blocks[b.index].floats
		    && d.state->restore(storage, b.index, b.floats.data(), b.floats.size())) {
			++taken;
		}
	}
	return taken;
}

size_t refreshBlocks(const device::Descriptor& d, const void* storage, ContainerBodies* bodies, DeviceInstanceId id,
                     bool onTickOnly) {
	if (d.state == nullptr || d.state->read == nullptr || storage == nullptr || bodies == nullptr) {
		return 0;
	}
	size_t moved = 0;
	for (size_t k = 0; k < d.state->blocks.size(); ++k) {
		const device::StateBlockDesc& b = d.state->blocks[k];
		if (onTickOnly && !b.harvestOnTick) {
			continue;
		}
		std::vector<float> floats(b.floats);
		const uint64_t serial = d.state->read(storage, k, floats.data(), floats.size());
		if (serial != 0 && bodies->refreshBlock(id, k, floats, serial)) {
			++moved;
		}
	}
	return moved;
}

size_t harvestBlocks(const ChainRecord& record, ContainerBodies* bodies) {
	if (bodies == nullptr) {
		return 0;
	}
	size_t moved = 0;
	for (size_t chain = 0; chain < record.chainCount(); ++chain) {
		for (const DeviceState& row : record.state().chain(chain)) {
			const device::Descriptor* d = record.descriptorOf(row.instanceId);
			if (d == nullptr || d->state == nullptr) {
				continue;
			}
			if (const void* surface = bodies->surfaceOf(row.instanceId); surface != nullptr) {
				moved += refreshBlocks(*d, surface, bodies, row.instanceId, /*onTickOnly=*/true);
			}
		}
	}
	return moved;
}

size_t rowOfValues(const device::Descriptor& d, const RackPatch::Device& in, std::span<float> fields,
                   LoadReport* report, const std::string& where, RackPatch::Device* carried) {
	using Kind = LoadReport::Kind;
	LoadReport scratch;
	LoadReport& r = report != nullptr ? *report : scratch;
	const size_t params = std::min(d.params.size(), fields.size());
	for (size_t i = 0; i < params; ++i) {
		fields[i] = static_cast<float>(d.params[i].def);
	}
	if (in.schema > d.traits.schema) {
		r.note(Kind::NewerSchema, where, d.traits.tag,
		       "schema " + std::to_string(in.schema) + ", this build reads " + std::to_string(d.traits.schema));
	}
	std::vector<bool> seen(params, false);
	const auto carry = [&](const RackPatch::Value& v, bool extra) {
		if (carried != nullptr) {
			(extra ? carried->extra : carried->values).push_back(v);
		}
	};
	// **A RETIRED KEY MAY CARRY ITS VALUE INTO ITS SUCCESSOR** (`RetiredKey::carriedInto`): read after every value the patch
	// names, under the new key, and only where the patch did not name that key itself.
	std::vector<RackPatch::Value> carriedLate;
	const auto take = [&](const RackPatch::Value& v) {
		const size_t i = paramIndexOf(d, v.key);
		if (i == kNoIndex || i >= params) {
			if (const device::RetiredKey* retired = retiredOf(d, v.key); retired != nullptr) {
				r.note(Kind::KeyRetired, where, v.key,
				       std::string(retired->reason != nullptr ? retired->reason : "") + " ("
				           + (retired->date != nullptr ? retired->date : "") + ")");
				if (retired->carriedInto != nullptr) {
					RackPatch::Value moved = v; // its meaning lives on under a new key: read there (`carriedLate`)
					moved.key = retired->carriedInto;
					carriedLate.push_back(std::move(moved));
				}
				return; // otherwise dropped: its meaning is gone, so carrying it would only carry it wrong
			}
			r.note(Kind::KeyUnknown, where, v.key);
			carry(v, false);
			return;
		}
		if (seen[i]) {
			return;
		}
		seen[i] = true;
		const device::ParamDesc& p = d.params[i];
		const auto defaulted = [&](const char* why) {
			r.note(Kind::ValueCoerced, where, v.key, why);
			fields[i] = static_cast<float>(p.def);
		};
		if (storedByLabel(p) && v.kind == RackPatch::Value::Kind::Label) {
			float value = 0.0f;
			if (valueOfLabel(p, v.text, &value)) {
				fields[i] = device::coerceControl(p, value);
			}
			else {
				r.note(Kind::LabelUnknown, where, v.key, v.text);
				fields[i] = static_cast<float>(p.def);
			}
			return;
		}
		switch (v.kind) {
		case RackPatch::Value::Kind::Number: {
			if (!std::isfinite(v.number)) {
				defaulted("was not a finite number (at its default)");
				break;
			}
			const float coerced = device::coerceControl(p, v.number);
			if (coerced != v.number) {
				r.note(Kind::ValueCoerced, where, v.key, "was outside its range (clamped)");
			}
			fields[i] = coerced;
			break;
		}
		case RackPatch::Value::Kind::Null: defaulted("was not a finite number (at its default)"); break;
		case RackPatch::Value::Kind::Label: defaulted("was a label where a number belongs (at its default)"); break;
		case RackPatch::Value::Kind::Raw: defaulted("was not a number (at its default)"); break;
		}
	};
	for (const RackPatch::Value& v : in.values) {
		take(v);
	}
	for (const RackPatch::Value& v : carriedLate) {
		if (const size_t i = paramIndexOf(d, v.key); i < params && !seen[i]) {
			take(v);
		}
	}
	for (size_t i = 0; i < params; ++i) {
		if (!seen[i]) {
			r.note(Kind::KeyMissing, where, d.params[i].key);
		}
	}
	size_t present = params;
	for (const RackPatch::Value& e : in.extra) {
		const bool ours = d.traits.extraStateKey != nullptr && e.key == d.traits.extraStateKey;
		// A KEYED BLOCK IS NOT THE ROW'S, and is read by its own road (`blocksOfPatch`).
		if (!ours && blockIndexOf(d, e.key) != kNoIndex) {
			continue;
		}
		if (!ours) {
			if (const device::RetiredKey* retired = retiredOf(d, e.key); retired != nullptr) {
				r.note(Kind::KeyRetired, where, e.key, retired->reason != nullptr ? retired->reason : "");
				continue;
			}
			r.note(Kind::KeyUnknown, where, e.key);
			carry(e, true);
			continue;
		}
		json::Value array;
		if (e.kind != RackPatch::Value::Kind::Raw || !json::parse(e.text, &array) || !array.isArray()) {
			r.note(Kind::ValueCoerced, where, e.key, "was not a list of numbers (left out)");
			continue;
		}
		// WHAT FITS, AS THE ROW RULE ALWAYS SAID (`Traits::extraState`): the device gets what the file
		// carried up to its own length, and a non-finite float becomes zero — a blob float has no default.
		const size_t room = std::min(d.traits.extraState, fields.size() > params ? fields.size() - params : 0);
		const size_t n = std::min(array.items.size(), room);
		for (size_t k = 0; k < n; ++k) {
			float f = 0.0f;
			fields[params + k] = array.items[k].asFloat(&f) && std::isfinite(f) ? f : 0.0f;
		}
		// A SHORTER BLOB FROM AN OLDER SCHEMA IS THE MIGRATION, NOT A COERCION: a device that grew its extra state
		// bumps `Traits::schema` (the ledger demands it), and what an older file lacks loads as 0 by the rule above —
		// which the device declares is its home (the Phaser/Flanger's travel, 30 → 34 floats, 2026-10-03). Anything
		// else that does not fit is still said.
		const bool olderAndShorter = in.schema != 0 && in.schema < d.traits.schema
		                             && array.items.size() < d.traits.extraState;
		if (array.items.size() != d.traits.extraState && !olderAndShorter) {
			r.note(Kind::ValueCoerced, where, e.key,
			       "held " + std::to_string(array.items.size()) + " numbers where this build keeps "
			           + std::to_string(d.traits.extraState));
		}
		present = params + n;
	}
	return present;
}

//------------------------------------------------------------------------------------------------//
// THE WIRE                                                                                       //
//------------------------------------------------------------------------------------------------//

bool rackToText(const RackPatch& patch, std::string* out, bool indented) {
	out->clear();
	json::Writer w(out, indented);
	w.beginObject();
	w.key("format");
	w.string(kFormat);
	w.key("major");
	w.number(kRackMajor);
	// THE CHAINS, EACH AN ARRAY IN ITS ORDER — order is the meaning here (a device's place in its chain),
	// so this is the one place the document is positional, and it positions DEVICES, never values.
	uint32_t chains = 0;
	for (const RackPatch::Device& device : patch.device) {
		chains = std::max(chains, device.chain + 1);
	}
	w.key("chains");
	w.beginArray();
	for (uint32_t chain = 0; chain < chains; ++chain) {
		w.beginArray();
		for (const RackPatch::Device& device : patch.device) {
			if (device.chain != chain) {
				continue;
			}
			w.beginObject();
			w.key("tag");
			w.string(std::string_view(device.tag.data(), strnlen(device.tag.data(), device::kTagBytes)));
			w.key("id");
			w.number(device.instanceId);
			w.key("schema");
			w.number(device.schema);
			// THE VIEW FLAGS WHOLE — a bit this build does not know is written back out as it came.
			w.key("view");
			w.number(device.view);
			w.key("values");
			writeValues(w, device.values);
			if (!device.extra.empty()) {
				w.key("extra");
				writeValues(w, device.extra);
			}
			// WHAT ITS OUTPUTS ARE CALLED HERE, by output key — only those that carry a name.
			if (!device.labels.empty()) {
				w.key("labels");
				writeValues(w, device.labels);
			}
			// AND WHICH PRESET ITS PANEL LAST LOADED OR SAVED (backlog QC) — only when it names one.
			if (!device.preset.empty()) {
				w.key("preset");
				writePresetIdentity(w, device.preset);
			}
			// THE BODY: a document spliced in whole, or the device's own bytes in a `$bin` (IrRef, ICB1) —
			// see `bodyIsDocument`.
			w.key("body");
			json::Value document;
			if (device.body.empty()) {
				w.null();
			}
			else if (bodyIsDocument(device.body, &document)) {
				w.value(document);
			}
			else {
				w.bytes(device.body);
			}
			// ITS SITE (backlog NR), only when it has one: absent is "none", bytes (maybe none) are the two chains.
			if (device.hasSite) {
				w.key("site");
				w.bytes(device.site);
			}
			w.endObject();
		}
		w.endArray();
	}
	w.endArray();
	w.key("routes");
	w.beginArray();
	for (const RackPatch::Route& r : patch.route) {
		w.beginObject();
		w.key("src");
		if (!writeSourceEnd(w, r.source)) {
			return false;
		}
		w.key("dst");
		if (!writeDestinationEnd(w, r.destination)) {
			return false;
		}
		w.key("amount");
		w.number(r.amount);
		// THE CURVE BY LABEL, AND ONLY WHEN THERE IS ONE — a Lin route writes exactly what it always wrote.
		if (r.transfer != device::Transfer::Lin) {
			if (const char* label = device::transferLabel(r.transfer); label != nullptr) {
				w.key("transfer");
				w.string(label);
			}
			// THE CURVE'S COEFFICIENT, KEYED BESIDE IT, AND ONLY OFF ITS DEFAULT (backlog PY) — a key per MEANING:
			// Log's and Exp's exponent, Phi's octaves (log2 of its periods, as MATH's Periods stores them). A
			// default Log or Exp writes exactly what it always wrote.
			if (const char* key = device::transferCoefficientKey(r.transfer);
			    key != nullptr && r.transfer != device::transferDefaultOf(r.transfer)) {
				w.key(key);
				w.number(device::transferCoefficient(r.transfer));
			}
		}
		w.endObject();
	}
	w.endArray();
	// THE EXPORT TABLE (backlog NZ (b)), only when there is one: [row, inner id (0 for a macro), index] each.
	if (!patch.exports.empty()) {
		w.key("exports");
		w.beginArray();
		for (const RackPatch::Export& e : patch.exports) {
			w.beginArray();
			w.number(uint64_t { e.row });
			w.number(uint64_t { e.device == kNoInstance ? 0u : e.device });
			w.number(uint64_t { e.index });
			w.endArray();
		}
		w.endArray();
	}
	w.key("macros");
	w.beginArray();
	for (const float m : patch.macro) {
		w.number(m);
	}
	w.endArray();
	w.key("bipolar");
	w.beginArray();
	for (const uint8_t b : patch.macroBipolar) {
		w.number(uint64_t { b != 0 ? 1u : 0u });
	}
	w.endArray();
	// EACH MACRO'S TYPE (a `device::MacroType`) AND NAME (empty for "Macro N"), beside its polarity.
	w.key("macroTypes");
	w.beginArray();
	for (const uint8_t t : patch.macroType) {
		w.number(uint64_t { t });
	}
	w.endArray();
	w.key("macroNames");
	w.beginArray();
	for (const std::string& name : patch.macroName) {
		w.string(device::clampMacroName(name)); // clamped here, so no name a caller hands in reads back longer
	}
	w.endArray();
	// WHICH PRESET THE RACK PLUGIN'S BROWSER CAME FROM (backlog QC) — only when it names one.
	if (!patch.preset.empty()) {
		w.key("preset");
		writePresetIdentity(w, patch.preset);
	}
	w.key("canvas");
	w.beginObject();
	w.key("extent");
	w.number(patch.canvas.extent);
	w.key("collapsed");
	w.boolean(patch.canvas.collapsed);
	w.endObject();
	// THE MACROS COLUMN'S FOLD (backlog QN), only when folded: absent reads as open.
	if (patch.macrosCollapsed) {
		w.key("macrosColumn");
		w.beginObject();
		w.key("collapsed");
		w.boolean(true);
		w.endObject();
	}
	// THE HOST SLOT POOL, only when this patch owns one (the outermost plugin's project state).
	if (patch.hostSlots.has_value()) {
		w.key("hostSlots");
		writeHostSlots(w, *patch.hostSlots);
	}
	w.endObject();
	if (indented) {
		out->push_back('\n'); // a file ends its last line; a compact body is exactly one value
	}
	return w.ok();
}

bool writeRack(const clap_ostream* stream, const RackPatch& patch, bool indented) {
	std::string text;
	if (stream == nullptr || !rackToText(patch, &text, indented)) {
		return false;
	}
	size_t done = 0;
	while (done < text.size()) {
		const int64_t n = stream->write(stream, text.data() + done, text.size() - done);
		if (n <= 0) {
			return false;
		}
		done += static_cast<size_t>(n);
	}
	return true;
}

bool parseRackText(std::string_view text, RackPatch* out, LoadReport* report) {
	if (out == nullptr) {
		return false;
	}
	*out = RackPatch {};
	if (text.empty()) {
		return true; // an empty stream is a fresh instance: `present` stays false
	}
	// THE OLD BINARY FORMAT OPENS WITH ITS VERSION BYTE (10, a newline) AND "MACR" — anything whose first
	// visible character is not `{` is not a document this build reads, and saying which is kinder.
	const size_t firstVisible = text.find_first_not_of(" \t\r\n");
	if (firstVisible == std::string_view::npos || text[firstVisible] != '{') {
		return notLoaded(report, "saved in an older format, which this build no longer reads");
	}
	json::Value doc;
	std::string error;
	if (!json::parse(text, &doc, documentLimits(), &error)) {
		return notLoaded(report, "not a readable patch (" + error + ")");
	}
	const json::Value* format = doc.find("format");
	if (format == nullptr || !format->isString() || format->text != kFormat) {
		return notLoaded(report, "not a rack patch");
	}
	uint32_t major = 0;
	if (!readU32(doc, "major", &major) || major == 0) {
		return notLoaded(report, "the patch names no format version");
	}
	if (major > kRackMajor) {
		return notLoaded(report, "saved by a newer format: saved by major " + std::to_string(major) + ", this build reads "
		                             + std::to_string(kRackMajor));
	}
	out->present = true;

	const json::Value* chains = doc.find("chains");
	if (chains == nullptr || !chains->isArray() || chains->items.size() > kMaxCount) {
		return notLoaded(report, "the patch has no chains");
	}
	for (size_t chain = 0; chain < chains->items.size(); ++chain) {
		const json::Value& devices = chains->items[chain];
		if (!devices.isArray()) {
			return notLoaded(report, "a chain is not a list");
		}
		for (const json::Value& node : devices.items) {
			RackPatch::Device device;
			device.chain = static_cast<uint32_t>(chain);
			const json::Value* tag = node.find("tag");
			// AN EMPTY TAG IS KEPT AS WHAT IT IS — a record naming no device, which a record may hold and the
			// binary format carried — and lands as a placeholder like any tag this build does not know.
			if (tag == nullptr || !tag->isString() || tag->text.size() > device::kTagBytes) {
				return notLoaded(report, "a device has no usable tag");
			}
			std::copy(tag->text.begin(), tag->text.end(), device.tag.begin());
			if (!readU32(node, "id", &device.instanceId)) {
				return notLoaded(report, "a device has no id");
			}
			(void)readU32(node, "schema", &device.schema);
			(void)readU32(node, "view", &device.view);
			if (!readValues(node.find("values"), &device.values) || !readValues(node.find("extra"), &device.extra)
			    || !readValues(node.find("labels"), &device.labels)) {
				return notLoaded(report, "a device's values are not an object");
			}
			(void)readPresetIdentity(node.find("preset"), &device.preset); // backlog QC; absent or odd: none
			// THE BODY, bounded before a byte of it is allocated for. One ceiling, the rack's: the parse knows
			// no devices, and the device that receives the body validates it (`loadState`).
			if (const json::Value* body = node.find("body"); body != nullptr && !body->isNull()) {
				if (json::isBin(*body)) {
					if (!json::binOf(*body, &device.body, kMaxBodyBytes)) {
						return notLoaded(report, "a device's contents could not be read");
					}
				}
				else if (body->isObject()) {
					// A DOCUMENT BODY: handed to its device as the document's own text.
					const std::string text = json::toCompactText(*body);
					if (text.size() > kMaxBodyBytes) {
						return notLoaded(report, "a device's contents are larger than any this build writes");
					}
					device.body.assign(text.begin(), text.end());
				}
				else {
					return notLoaded(report, "a device's contents are neither a document nor bytes");
				}
			}
			// ITS SITE (backlog NR), bounded as a two-lane bundle is: the site's own `loadState` validates the rest.
			if (const json::Value* site = node.find("site"); site != nullptr && !site->isNull()) {
				if (!json::isBin(*site) || !json::binOf(*site, &device.site, bundleMaxBytes(2))) {
					return notLoaded(report, "a device's chains could not be read");
				}
				device.hasSite = true;
			}
			out->device.push_back(std::move(device)); // a tag this build cannot run is still recorded
		}
	}
	if (const json::Value* routes = doc.find("routes"); routes != nullptr) {
		if (!routes->isArray() || routes->items.size() > kMaxCount) {
			return notLoaded(report, "the routes are not a list");
		}
		for (const json::Value& node : routes->items) {
			RackPatch::Route r;
			const json::Value* amount = node.find("amount");
			if (!readSourceEnd(node.find("src"), &r.source) || !readDestinationEnd(node.find("dst"), &r.destination)
			    || amount == nullptr || !(amount->isNull() || amount->asFloat(&r.amount))) {
				return notLoaded(report, "a route could not be read");
			}
			if (amount->isNull()) {
				r.amount = 0.0f; // not finite when saved: a route of no amount, which the applier drops
			}
			// THE CURVE, BY LABEL; absent is Lin. A label this build does not know is a later build's curve:
			// the route still loads, Lin, and the report says one was not understood rather than guessing.
			if (const json::Value* transfer = node.find("transfer"); transfer != nullptr) {
				std::optional<device::Transfer> t =
				    transfer->isString() ? device::transferFromLabel(transfer->text.data(), transfer->text.size())
				                         : std::nullopt;
				// ITS COEFFICIENT, under the key its kind names: absent or unreadable is the kind's default, never a
				// guess at something else.
				if (t.has_value()) {
					if (const char* key = device::transferCoefficientKey(*t); key != nullptr) {
						if (const json::Value* c = node.find(key); c != nullptr) {
							float read = 0.0f;
							if (c->asFloat(&read)) {
								t = device::transferWithCoefficient(*t, read);
							}
						}
					}
				}
				if (t.has_value()) {
					r.transfer = *t;
				}
				else if (report != nullptr) {
					report->note(LoadReport::Kind::LabelUnknown, "patch",
					             transfer->isString() ? transfer->text : std::string("transfer"),
					             "a route's curve this build lacks: read as Linear");
				}
			}
			out->route.push_back(std::move(r));
		}
	}
	if (const json::Value* exports = doc.find("exports"); exports != nullptr && exports->isArray()) {
		for (const json::Value& e : exports->items) {
			uint32_t row = 0;
			uint32_t id = 0;
			uint32_t index = 0;
			if (!e.isArray() || e.items.size() != 3 || !e.items[0].asU32(&row) || !e.items[1].asU32(&id)
			    || !e.items[2].asU32(&index) || row >= device::kSiteExportRows) {
				return false; // a table this build did not write
			}
			out->exports.push_back({ row, id == 0 ? kNoInstance : static_cast<DeviceInstanceId>(id), index });
		}
	}
	if (const json::Value* macros = doc.find("macros"); macros != nullptr && macros->isArray()) {
		for (const json::Value& m : macros->items) {
			float v = 0.0f;
			out->macro.push_back(m.asFloat(&v) ? v : 0.0f);
		}
	}
	if (const json::Value* bipolar = doc.find("bipolar"); bipolar != nullptr && bipolar->isArray()) {
		for (const json::Value& b : bipolar->items) {
			uint32_t v = 0;
			out->macroBipolar.push_back(b.asU32(&v) && v == 1 ? 1 : 0); // anything but exactly 1 is unipolar
		}
	}
	if (const json::Value* types = doc.find("macroTypes"); types != nullptr && types->isArray()) {
		for (const json::Value& t : types->items) {
			// CLAMPED, NOT REFUSED — FSMB's rule: anything unreadable is a Knob (what every macro was), past the
			// last is the last. A knob that behaves wrongly is recoverable; a lost patch is not.
			uint32_t v = 0;
			const uint32_t last = static_cast<uint32_t>(device::kAbsoluteMacroTypeCount) - 1; // the rack has no Relative (PQ)
			out->macroType.push_back(static_cast<uint8_t>(t.asU32(&v) ? std::min(v, last) : 0u));
		}
	}
	if (const json::Value* names = doc.find("macroNames"); names != nullptr && names->isArray()) {
		for (const json::Value& n : names->items) {
			out->macroName.push_back(n.isString() ? device::clampMacroName(n.text) : std::string {});
		}
	}
	(void)readPresetIdentity(doc.find("preset"), &out->preset); // backlog QC; absent or odd: none
	if (const json::Value* column = doc.find("macrosColumn"); column != nullptr && column->isObject()) {
		if (const json::Value* collapsed = column->find("collapsed"); collapsed != nullptr) {
			(void)collapsed->asBool(&out->macrosCollapsed); // backlog QN
		}
	}
	if (const json::Value* canvas = doc.find("canvas"); canvas != nullptr && canvas->isObject()) {
		if (const json::Value* extent = canvas->find("extent"); extent != nullptr) {
			(void)extent->asFloat(&out->canvas.extent);
		}
		if (const json::Value* collapsed = canvas->find("collapsed"); collapsed != nullptr) {
			(void)collapsed->asBool(&out->canvas.collapsed);
		}
	}
	// THE HOST SLOT POOL — DROPPED ALONE AND SAID, never a refusal of the patch (see the header's note).
	if (const json::Value* slots = doc.find("hostSlots"); slots != nullptr) {
		hostslots::PoolSnapshot snap;
		std::string why;
		if (readHostSlots(*slots, &snap, &why)) {
			out->hostSlots = std::move(snap);
		}
		else if (report != nullptr) {
			report->note(LoadReport::Kind::SlotsDropped, "patch", "hostSlots", why);
		}
	}
	return true;
}

bool parseRack(const clap_istream* stream, RackPatch* out, LoadReport* report) {
	if (stream == nullptr || out == nullptr) {
		return false;
	}
	// TO THE END OF THE STREAM, bounded: the document is all that is on it.
	std::string text;
	char buffer[16384];
	for (;;) {
		const int64_t n = stream->read(stream, buffer, sizeof(buffer));
		if (n < 0) {
			*out = RackPatch {};
			return notLoaded(report, "the stream could not be read");
		}
		if (n == 0) {
			break;
		}
		text.append(buffer, static_cast<size_t>(n));
		if (text.size() > kMaxDocumentBytes) {
			*out = RackPatch {};
			return notLoaded(report, "the patch is larger than any this build writes");
		}
	}
	return parseRackText(text, out, report);
}

//------------------------------------------------------------------------------------------------//
// A RECORD                                                                                       //
//------------------------------------------------------------------------------------------------//

RackPatch::Device patchDeviceOfRow(const ChainRecord& record, const ContainerBodies* bodies, const DeviceViews* views,
                                   const DeviceLabels* labels, const DeviceState& row, size_t chain) {
	RackPatch::Device device;
	device.chain = static_cast<uint32_t>(chain);
	std::copy(row.tag.begin(), row.tag.end(), device.tag.begin());
	device.instanceId = row.instanceId;
	// HOW ITS CARD IS SHOWN, under the id the record has it under — the same key its routes and its
	// body use, so the three cannot disagree about which device they are about.
	device.view = views != nullptr ? views->flagsOf(row.instanceId) : 0;
	// ITS VALUES BY KEY, and then whatever the load could not apply and a save must carry back out: a
	// known device's unknown keys after its own, a device this build cannot run as it came.
	const device::Descriptor* desc = record.descriptorFor(device.tag.data());
	if (desc != nullptr) {
		valuesOfRow(*desc, std::span<const float>(row.field.data(), row.field.size()), row.fieldsPresent,
		            &device);
		// AND WHAT ITS OUTPUTS ARE CALLED, under the same id for the same reason, BY OUTPUT KEY.
		for (size_t output = 0; labels != nullptr && output < desc->outputs.size(); ++output) {
			const std::string_view named = labels->labelOf(row.instanceId, output);
			if (!named.empty()) {
				device.labels.push_back(RackPatch::Value::label(desc->outputs[output].key, std::string(named)));
			}
		}
	}
	// AND WHICH PRESET ITS PANEL CAME FROM (backlog QC), under the same id.
	if (labels != nullptr) {
		if (const PresetIdentity* preset = labels->presetOf(row.instanceId); preset != nullptr) {
			device.preset = *preset;
		}
	}
	if (bodies != nullptr) {
		readCarried(bodies->carriedFor(row.instanceId), &device);
	}
	// THE BODY, FROM THE RUNNING INSTANCE WHEN THE CARRIER HAS ONE ON FILE (rack roadmap §6): a chain edited
	// through its own editor is current only there — `bodyOf` asks the device's `saveState`, which reads its
	// own record on this thread — and from the stored bytes otherwise (built but never edited, or never
	// built: a load that has not been provisioned yet).
	bool taken = false;
	if (bodies != nullptr) {
		if (const void* surface = bodies->surfaceOf(row.instanceId); surface != nullptr) {
			taken = bodyOf(record.descriptorOf(row.instanceId), surface, &device.body) && !device.body.empty();
		}
		if (!taken && bodies->has(row.instanceId)) {
			const std::span<const uint8_t> body = bodies->bodyFor(row.instanceId);
			device.body.assign(body.begin(), body.end());
		}
		// AND ITS SITE (backlog NR), by the same rule: the running site's chains when one is on file — edited
		// through its own editor, they are current only there — the stored bytes otherwise.
		const DeviceInstanceId siteKey = siteKeyOf(row.instanceId);
		if (bodies->has(siteKey)) {
			device.hasSite = true;
			const void* site = bodies->surfaceOf(siteKey);
			if (site == nullptr || !bodyOf(&siteDescriptor(), site, &device.site)) {
				const std::span<const uint8_t> bytes = bodies->bodyFor(siteKey);
				device.site.assign(bytes.begin(), bytes.end());
			}
		}
	}
	// **AND ITS KEYED BLOCKS, HARVESTED NOW** (backlog NM): from the running instance's own snapshot, else from
	// the store — the save is what catches them up, so a block that moves every tick costs nothing between saves.
	if (desc != nullptr) {
		writeBlocks(*desc, bodies != nullptr ? bodies->surfaceOf(row.instanceId) : nullptr, bodies, row.instanceId,
		            &device);
	}
	return device;
}

bool patchRouteOfRecord(const ChainRecord& record, const CoreCodec& codec, const backplane::Route& route,
                        RackPatch::Route* out) {
	const Holder holder { &record, [](const void* h, DeviceInstanceId id) {
		                     return static_cast<const ChainRecord*>(h)->descriptorOf(id);
	                     } };
	return describe(codec, holder, route, out);
}

RackPatch patchOfRecord(const ChainRecord& record, const ContainerBodies* bodies, std::span<const float> macros,
                        const backplane::RouteList& routes, const CoreCodec& codec, const DeviceViews* views,
                        std::span<const uint8_t> macroBipolar, const DeviceLabels* labels) {
	RackPatch out;
	out.present = true;
	out.macro.assign(macros.begin(), macros.end());
	out.macroBipolar.assign(macroBipolar.begin(), macroBipolar.end());
	for (size_t chain = 0; chain < record.chainCount(); ++chain) {
		// **THE CHAIN IS ITS ORDER** (kill-lanes step 3b) — the run of rows IS what a save walks, so there is
		// no second structure to agree with and no place left for the two to disagree.
		for (const DeviceState& row : record.state().chain(chain)) {
			out.device.push_back(patchDeviceOfRow(record, bodies, views, labels, row, chain));
		}
	}
	// **THE PUBLISHED ROUTES ARE ALREADY IN THE FILE'S TERMS** (kill-lanes step 1) but for one word: a device
	// end is its id and its output or control INDEX, and the file names that control by its KEY — so the
	// device's descriptor turns one into the other, and a host end asks the codec for its name.
	routes.forEach([&](const backplane::Route& route) {
		RackPatch::Route r;
		if (patchRouteOfRecord(record, codec, route, &r)) {
			out.route.push_back(std::move(r));
		}
	});
	return out;
}

LoadReport applyPatchToRecord(const RackPatch& patch, ChainRecord* record, ContainerBodies* bodies,
                              backplane::InstanceCounter* ids, const CoreCodec& codec, DeviceViews* views,
                              DeviceLabels* labels) {
	LoadReport report {};
	if (record == nullptr || ids == nullptr || !patch.present) {
		return report;
	}
	record->clear();
	if (bodies != nullptr) {
		bodies->clear();
	}
	// **EMPTIED WITH THE REST, AND THAT IS WHAT STOPS ONE PATCH'S FOLDS REACHING ANOTHER'S CARDS.** Ids are
	// patch-scoped, so two patches both hold a device 1; a map that survived a load would hand the incoming
	// device 1 the outgoing one's fold.
	if (views != nullptr) {
		views->clear();
	}
	if (labels != nullptr) {
		labels->clear(); // and one patch's labels never name another's outputs, for the same reason
	}
	// THE COUNTER PAST EVERY ID THE PATCH NAMES, first — so an id issued fresh below (to a device whose own
	// was taken) can never be one a later device in the same patch arrives wearing.
	//
	// **AND THIS LOOP IS LOAD-BEARING FOR THE ROUTES, not only for the ids** (2026-09-12). The route road
	// below reports and builds each device end from THE ID THE FILE NAMED, which is sound only because an id
	// that resolves through `WornIds` is never a renamed device's: a device is renamed exactly when its file
	// id was already held, and that is the same duplicate that makes `wornFor` answer `kNoInstance`. **THAT
	// EQUIVALENCE IS WHAT THIS LOOP BUYS.** Without it, a fresh id issued to an early device could collide
	// with a LATER device's file id; that later device would then be renamed for a reason that is not a
	// duplicate, `wornFor` would still answer with one pair — and the route would be wired to the wrong
	// device, silently. Move or drop this and the route road stops being correct, with nothing near it
	// failing.
	for (const RackPatch::Device& device : patch.device) {
		if (namedBy(device.instanceId) != kNoInstance) {
			ids->reserve(device.instanceId);
		}
	}
	PatchPlacement placement;
	placement.idsOrFresh = ids;
	(void)placePatchInRecord(patch, record, bodies, codec, views, labels, placement, &report);
	return report;
}

size_t placePatchInRecord(const RackPatch& patch, ChainRecord* record, ContainerBodies* bodies, const CoreCodec& codec,
                          DeviceViews* views, DeviceLabels* labels, const PatchPlacement& placement,
                          LoadReport* reportOut) {
	LoadReport& report = *reportOut;
	WornIds worn;
	size_t placedCount = 0;
	size_t nextAt = placement.position;
	for (size_t index = 0; index < patch.device.size(); ++index) {
		const RackPatch::Device& device = patch.device[index];
		const size_t chain = placement.chain != ChainRecord::kNoChain ? placement.chain : device.chain;
		if (chain >= record->chainCount()) {
			continue; // a chain this host does not have
		}
		// WHERE IT GOES: each at its own recorded place (an undo's put-back, ascending, so each lands where it
		// stood), else one after another from `position`, else at the end of its chain — a load.
		size_t at = record->length(chain);
		if (!placement.positions.empty()) {
			at = index < placement.positions.size() ? std::min(placement.positions[index], record->length(chain))
			                                         : record->length(chain);
		}
		else if (nextAt != ChainRecord::kNoPosition) {
			at = std::min(nextAt, record->length(chain));
		}
		// **NO SLOT IS CHOSEN HERE** (kill-lanes step 3b) — the file names none and neither does this: a
		// device goes AT THE END OF ITS CHAIN, and where the record parks it is the record's own business
		// (`ChainRecord::insertRow`). A full chain is what refuses it, not a search that came up empty.
		DeviceState row;
		std::copy(device.tag.begin(), device.tag.begin() + static_cast<std::ptrdiff_t>(row.tag.size()), row.tag.begin());
		// UNDER ITS OWN ID IF FREE — a fresh one if not; the routes that named it follow it either way. A placement
		// with no counter (an undo's put-back) takes the id it names or nothing.
		if (namedBy(device.instanceId) != kNoInstance && !record->holds(device.instanceId)) {
			row.instanceId = device.instanceId;
		}
		else if (placement.idsOrFresh != nullptr) {
			row.instanceId = placement.idsOrFresh->issue();
		}
		else {
			continue; // refused, and counted by the caller: the return is what landed
		}
		// **EVERY CONTROL BY ITS KEY** (backlog JF): wherever it stands in the file, missing at its default,
		// a Choice by its label. A device this build cannot run keeps nothing in its row — it has no
		// controls here — and ALL of what it came with is carried beside the body, so it saves back out as
		// it arrived.
		const device::Descriptor* d = record->descriptorFor(row.tag.data());
		const std::string where = deviceWhere(d != nullptr ? d->traits.name : device.tag.data(), chain, at);
		RackPatch::Device leftovers;
		if (d != nullptr) {
			row.fieldsPresent = rowOfValues(*d, device, std::span<float>(row.field.data(), row.field.size()), &report,
			                                where, &leftovers);
		}
		else {
			report.note(LoadReport::Kind::DeviceUnknown, where, device.tag.data());
			row.fieldsPresent = 0;
			leftovers.values = device.values;
			leftovers.extra = device.extra;
			leftovers.labels = device.labels; // its output names too, so it saves back out as it arrived
			leftovers.schema = device.schema;
		}
		if (!record->insertRow(chain, at, row)) {
			continue;
		}
		worn.note(namedBy(device.instanceId), row.instanceId);
		++placedCount;
		if (nextAt != ChainRecord::kNoPosition) {
			nextAt = at + 1;
		}
		if (placement.worn != nullptr) {
			placement.worn->emplace_back(namedBy(device.instanceId), row.instanceId);
		}
		// ITS OUTPUTS' NAMES, BY KEY, on the bridge's rule: one spelling out the declared name is none. A key
		// this device does not declare names no output here — carried, so a build that has it reads it back.
		for (const RackPatch::Value& label : d != nullptr ? device.labels : std::vector<RackPatch::Value> {}) {
			const size_t output = outputIndexOf(*d, label.key);
			if (output == kNoIndex || label.kind != RackPatch::Value::Kind::Label) {
				leftovers.labels.push_back(label);
				continue;
			}
			if (labels != nullptr && device::clampMacroName(label.text) != declaredOutputName(d, output)) {
				labels->set(row.instanceId, output, label.text);
			}
		}
		// WHICH PRESET ITS PANEL CAME FROM (backlog QC), under the id it ended up wearing.
		if (labels != nullptr) {
			(void)labels->setPreset(row.instanceId, device.preset);
		}
		if (bodies != nullptr && !device.body.empty()) {
			(void)bodies->store(row.instanceId, device.body.data(), device.body.size());
		}
		// ITS SITE UNDER THE ID IT ENDED UP WEARING (backlog NR) — filed even when empty, which is "it has one".
		if (bodies != nullptr && device.hasSite) {
			(void)bodies->store(siteKeyOf(row.instanceId), device.site.data(), device.site.size());
		}
		if (bodies != nullptr) {
			bodies->carry(row.instanceId, carriedText(leftovers));
		}
		// **ITS KEYED BLOCKS, EVEN NONE** (backlog NM): storing is what moves the device's stored generation, so a load
		// always builds a device that keeps blocks afresh — handed what the file holds, or nothing — rather than leaving
		// a running instance holding another patch's state under a reused id.
		if (bodies != nullptr && d != nullptr && d->state != nullptr) {
			bodies->storeBlocks(row.instanceId, blocksOfPatch(*d, device, &report, where));
		}
		// UNDER THE ID IT ENDED UP WEARING, not the one the file named — a device whose own id was taken gets
		// a fresh one, and its fold follows it there exactly as its body and its routes do.
		if (views != nullptr && device.view != 0) {
			views->set(row.instanceId, device.view);
		}
	}
	// **A ROUTE LANDS AS AN ENDPOINT, NOT AS A COORDINATE** (kill-lanes step 3b). The file holds a device end
	// as its instance id and the KEY of its output or control; a load swaps the id the FILE named for the id
	// the device ENDED UP WEARING, turns the key into the device's own index through its descriptor, and
	// hands the pair to the record's one gate.
	//
	// **THE RULES ARE UNCHANGED AND ARE STILL ASKED ONCE**: `ChainRecord::setRoute` gates every pair through
	// `backplane::mayRoute` — a live source with such an output, a live destination whose control is
	// MODULATABLE and inside the axes' stride, the host's policy, and the source upstream of the
	// destination — so every test `resolve` used to make on the way is made there, on the identities.
	const Holder holder { record,
		                  [](const void* h, DeviceInstanceId id) { return static_cast<const ChainRecord*>(h)->descriptorOf(id); },
		                  &patch,
		                  [](const void* h, const char* tag) { return static_cast<const ChainRecord*>(h)->descriptorFor(tag); } };
	for (const RackPatch::Route& route : patch.route) {
		const float amount = std::isfinite(route.amount) ? route.amount : 0.0f;
		if (amount == 0.0f) {
			continue;
		}
		// **EACH END AS THE FILE ASKED FOR IT** (decided 2026-09-12; `RouteRefusals` carries the reasoning).
		// A device end is the id the patch wrote — which, where the id resolves at all, IS the id the device
		// ended up wearing: a device is renamed only when its file id was already taken, and that is
		// precisely the duplicate that makes the id resolve to nothing. **THAT EQUIVALENCE IS BOUGHT BY THE
		// `ids->reserve` LOOP AT THE TOP OF THIS FUNCTION.**
		const backplane::Source s = sourceOfPatchCoord(route.source, codec, holder);
		const backplane::Destination d = destinationOfPatchCoord(route.destination, codec, holder);
		// **WHETHER AN END NAMES ANYTHING THIS RECORD HOLDS IS `setRoute`'S QUESTION, WITH TWO EXCEPTIONS.**
		// An id TWO of the patch's devices wore must be refused before the gate sees it: the record holds
		// exactly one device under that id — whichever kept it — so the gate would cheerfully wire the route
		// to that one. `wornFor` is the only thing that knows the file wore it twice. And a KEY the device
		// does not declare resolves to `nobody()`, which is refused here rather than handed on.
		//
		// **A FRAGMENT'S ROUTE MAY NAME A DEVICE THE PATCH DOES NOT CARRY** (backlog OA): an inbound route on an undo's
		// put-back, whose source stayed in the record. `outsideEnds` lets such an end through as the record's own id,
		// for the gate to answer; a load never sets it, and there an id the patch did not carry names nothing.
		const auto unnamed = [&](DeviceInstanceId id) {
			if (worn.wornFor(id) != kNoInstance) {
				return false;
			}
			if (!placement.outsideEnds) {
				return true;
			}
			for (const RackPatch::Device& named : patch.device) {
				if (named.instanceId == id) {
					return true; // the patch carried it and it did not land
				}
			}
			return false;
		};
		const bool namesNoOneDevice = (route.source.kind == static_cast<uint32_t>(SourceKind::Device)
		                               && (unnamed(route.source.a) || s == backplane::Source::nobody()))
		                              || (route.destination.kind == static_cast<uint32_t>(DestKind::Device)
		                                  && (unnamed(route.destination.a) || d == backplane::Destination::nobody()));
		if (namesNoOneDevice || !record->setRoute(s, d, amount)) {
			noteRefusal(&report, route, s, d);
		}
		else {
			(void)record->setRouteTransfer(s, d, route.transfer); // Lin too: a load says what the curve IS
		}
	}
	return placedCount;
}

//------------------------------------------------------------------------------------------------//
// AN ENGINE                                                                                      //
//------------------------------------------------------------------------------------------------//

bool bodyOf(const device::Descriptor* d, const void* storage, std::vector<uint8_t>* out) {
	out->clear();
	if (d == nullptr || d->bodySaver() == nullptr || storage == nullptr) {
		return true; // nothing held — an empty body, and not a failure
	}
	// ONCE, INTO A BUFFER, because the record carries the length before the bytes; each level serialises
	// its child exactly once (see `ContainerOps::saveState`).
	const device::ByteSink into { out, [](void* ctx, const uint8_t* data, size_t n) {
		                             auto* v = static_cast<std::vector<uint8_t>*>(ctx);
		                             v->insert(v->end(), data, data + n);
	                             } };
	return d->bodySaver()(const_cast<void*>(storage), into);
}

namespace {

const device::Descriptor* engineDescOf(const void* h, DeviceInstanceId id) {
	const auto* e = static_cast<const RackEngine*>(h);
	const size_t slot = e->slotOf(id);
	return slot == RackEngine::kNoSlot ? nullptr : e->slotAtLane(slot).live_desc();
}

} // namespace

bool patchOf(const RackEngine& rack, RackPatch* out) {
	if (out == nullptr) {
		return false;
	}
	*out = RackPatch {};
	out->present = true;
	const backplane::Axes& axes = rack.axes();
	for (size_t i = 0; i < axes.coreSources; ++i) {
		out->macro.push_back(rack.macro(i));
		out->macroBipolar.push_back(rack.macroBipolar(i) ? 1 : 0);
		out->macroType.push_back(static_cast<uint8_t>(rack.macroType(i)));
		out->macroName.push_back(rack.macroRenamed(i) ? rack.macroName(i) : std::string {});
	}
	for (size_t position = 0; position < rack.chainLength(0); ++position) {
		const size_t lane = rack.deviceAtPosition(position);
		if (lane == RackEngine::kNoSlot || !rack.slotAtLane(lane).live()) {
			continue;
		}
		const Slot& slot = rack.slotAtLane(lane);
		RackPatch::Device device;
		copyTag(slot.live_desc()->traits.tag, &device.tag);
		device.instanceId = slot.id;
		valuesOfRow(*slot.live_desc(), std::span<const float>(slot.base.data(), slot.base.size()), slot.base.size(),
		            &device);
		if (!bodyOf(slot.live_desc(), slot.storage.get(), &device.body)) {
			return false;
		}
		writeBlocks(*slot.live_desc(), slot.storage.get(), nullptr, slot.id, &device);
		out->device.push_back(std::move(device));
	}
	// **THE ROUTES THE ENGINE HOLDS, BY IDENTITY — walked, not scanned.** This swept every cell of the
	// axes' `sources × destinations` asking `routeAmount` at each; the routes are edges and there is no
	// grid to sweep (kill-lanes §2.4b).
	const Holder holder { &rack, &engineDescOf };
	const CoreCodec codec = rackCoreCodec(axes);
	rack.forEachRoute([&](const backplane::Route& route) {
		RackPatch::Route r;
		if (route.amount != 0.0f && describe(codec, holder, route, &r)) {
			out->route.push_back(std::move(r));
		}
	});
	return true;
}

void applyRack(const RackPatch& patch, RackEngine* rack, LoadReport* report) {
	if (rack == nullptr || report == nullptr) {
		return;
	}
	*report = LoadReport {};
	if (!patch.present) {
		return;
	}
	const backplane::Axes& axes = rack->axes();
	for (size_t i = 0; i < rack->slotCount(); ++i) {
		rack->removeDeviceAtLane(i);
	}
	for (size_t i = 0; i < patch.macro.size(); ++i) {
		rack->setMacro(i, patch.macro[i]);
	}
	// EVERY MACRO'S POLARITY, as the bridge's load does: a patch that lists fewer leaves the rest unipolar.
	for (size_t i = 0; i < axes.coreSources; ++i) {
		rack->setMacroBipolar(i, i < patch.macroBipolar.size() && patch.macroBipolar[i] != 0);
	}
	// TYPE AND NAME, EVERY MACRO: a patch that lists fewer leaves the rest a Knob and unnamed.
	for (size_t i = 0; i < axes.coreSources; ++i) {
		rack->setMacroType(i, i < patch.macroType.size() ? static_cast<device::MacroType>(patch.macroType[i])
		                                                 : device::MacroType::Knob);
		rack->setMacroName(i, i < patch.macroName.size() ? std::string_view(patch.macroName[i]) : std::string_view {});
	}
	Placement placed;
	for (const RackPatch::Device& device : patch.device) {
		if (device.chain != 0) {
			continue; // an engine keeps one chain
		}
		// BUILT HERE AND ADOPTED — this road is for an engine no block is running on. A container's body
		// goes into the device before the adopt, where building members may allocate.
		// The engine's own slot for it — the file names none — and the FILE'S ID, so the engine and the patch
		// name the device alike; a malformed file naming one id twice gets a fresh one for the second.
		const std::string where = deviceWhere(device.tag.data(), 0, rack->chainLength(0));
		RackEngine::Provision p = rack->provision(device.tag.data());
		if (!p.live()) {
			report->note(LoadReport::Kind::DeviceUnknown, where, device.tag.data());
			continue; // a tag this build cannot run
		}
		if (!device.body.empty() && p.desc->bodyLoader() != nullptr
		    && !p.desc->bodyLoader()(p.storage.get(), device.body.data(), device.body.size())) {
			report->note(LoadReport::Kind::BodyRefused, where, device.tag.data());
		}
		restoreBlocksOfPatch(*p.desc, p.storage.get(), device, report, where);
		const device::Descriptor* desc = p.desc;
		size_t lane = rack->adoptDevice(rack->chainLength(0), std::move(p), namedBy(device.instanceId));
		if (lane == RackEngine::kNoSlot && namedBy(device.instanceId) != kNoInstance) {
			lane = rack->adoptDevice(rack->chainLength(0), std::move(p)); // handed back whole on refusal
		}
		if (lane == RackEngine::kNoSlot) {
			continue;
		}
		std::vector<float> fields(desc->params.size() + desc->traits.extraState);
		const size_t present = rowOfValues(*desc, device, fields, report, where, nullptr);
		for (size_t f = 0; f < present; ++f) {
			rack->setField(lane, f, fields[f]);
		}
		// THE ID THE FILE NAMED AND THE ONE THE ENGINE GRANTED — the same one unless it collided.
		placed.note(namedBy(device.instanceId), rack->slotAtLane(lane).id);
	}
	const Holder holder { rack, &engineDescOf, &patch, [](const void* h, const char* tag) {
		                     return static_cast<const RackEngine*>(h)->catalogue()(tag);
	                     } };
	const CoreCodec codec = rackCoreCodec(axes);
	for (const RackPatch::Route& route : patch.route) {
		backplane::Source source;
		backplane::Destination destination;
		if (!std::isfinite(route.amount) || route.amount == 0.0f) {
			continue;
		}
		// **BY IDENTITY END TO END (kill-lanes §2.4b)**, and by key since backlog JF.
		if (!resolve(codec, placed, holder, route, &source, &destination) || !rack->mayRoute(source, destination)) {
			noteRefusal(report, route, sourceOfPatchCoord(route.source, codec, holder),
			            destinationOfPatchCoord(route.destination, codec, holder));
			continue;
		}
		rack->setRouteAmount(source, destination, route.amount);
		(void)rack->setRouteTransfer(source, destination, route.transfer);
	}
}

// **BOTH OF THESE DROP VIEW STATE (folds, output labels)**: an engine holds neither. They are reached only
// before `RackEngine::provision` hands a container its catalogue, which builds the bridge that holds them
// and that every later save goes through (`rack_state.h` says the rest).
bool saveRack(const clap_ostream* stream, const RackEngine& rack, bool indented) {
	RackPatch patch;
	return patchOf(rack, &patch) && writeRack(stream, patch, indented);
}

bool loadRack(const clap_istream* stream, RackEngine* rack, LoadReport* report) {
	if (rack == nullptr || report == nullptr) {
		return false;
	}
	*report = LoadReport {};
	RackPatch patch;
	// NOTHING IS WRITTEN UNTIL THE WHOLE STREAM HAS PARSED — a truncated patch changes nothing.
	if (!parseRack(stream, &patch, report)) {
		return false;
	}
	applyRack(patch, rack, report);
	return true;
}

} // namespace feathers::rack
