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

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "plugin/device_registry.h"
#include "plugin/params.h"
#include "plugin/spectral_state.h"
#include "plugin/terrain_likes.h"
#include "backplane/preset_identity.h"
#include "state/json.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

// THE SCHEMA LEDGER (backlog JF step 1b): one line per stored
// key, checked in as `tests/state_schema.ledger`, and this suite comparing it with what the registry
// declares now. It is what guards a device edit instead of `kStateVersion`.
//
// WHAT EACH DIFFERENCE MEANS, AND WHAT IT ASKS OF YOU:
//   - A NEW KEY (a control, an output, a device appended): harmless. Red until its line is in the ledger;
//     the failure prints the line.
//   - A HARMLESS CHANGE — a range WIDENED in the same unit, a Choice that only GAINED labels, a retirement
//     recorded: red until the line is refreshed, and `FEATHERS_LEDGER_WRITE=1` refreshes it for you.
//   - A MEANING CHANGE — a different unit, a range narrowed or moved (a rescale), a step or zone count
//     changed, a Choice label gone with no alias, a key gone without being retired, a retired key reused:
//     **the stored numbers would now mean something else.** Change the key (retire the old one, and old
//     patches fall to the default with the reason said) or bump the device's `Traits::schema` with a
//     migration. `FEATHERS_LEDGER_WRITE=1` REFUSES to write while one of these stands, so the ledger cannot
//     be regenerated past the very edit it exists to catch.
//
// **WHY A LEDGER AND NOT A DIGEST** (the digest it replaces is `kStoredMeaningFingerprint`): it fires on the
// edits that change meaning and not on renames; it catches a rescale, which the digest left out on purpose;
// and two branches that each add a line MERGE as text, where two digests merge into a number neither
// branch computed.

namespace {

using feathers::device::Descriptor;
using feathers::device::ParamDesc;
using feathers::device::Unit;

const char* unitName(Unit u) {
	switch (u) {
	case Unit::Number: return "number";
	case Unit::Percent: return "percent";
	case Unit::LogHertz: return "log_hertz";
	case Unit::LogSeconds: return "log_seconds";
	case Unit::LogMultiple: return "log_multiple";
	case Unit::Choice: return "choice";
	case Unit::Note: return "note";
	case Unit::Decibel: return "decibel";
	case Unit::Fader: return "fader";
	case Unit::Degrees: return "degrees";
	}
	return "unknown";
}

std::string num(double v) {
	std::string s;
	feathers::state::json::appendNumber(&s, v);
	return s;
}

bool isLabelled(const ParamDesc& p) { return p.unit == Unit::Choice && !p.names.empty(); }

//------------------------------------------------------------------------------------------------//
// ONE LINE PER KEY                                                                               //
//------------------------------------------------------------------------------------------------//

/// One ledger line, split: `kind tag key attrs…`. For `retired`, `attrs` is `date reason…`.
struct Line {
	std::string kind; ///< device, param, extra, block, out, retired
	std::string tag;
	std::string key;
	std::map<std::string, std::string> attrs;
	std::string text;

	[[nodiscard]] std::string id() const { return kind + " " + tag + " " + key; }
	[[nodiscard]] std::string attr(const std::string& name) const {
		const auto it = attrs.find(name);
		return it == attrs.end() ? std::string {} : it->second;
	}
};

std::vector<std::string> linesOf(const Descriptor& d) {
	std::vector<std::string> out;
	const std::string tag = d.traits.tag;
	out.push_back("device " + tag + " - schema=" + std::to_string(d.traits.schema));
	for (const ParamDesc& p : d.params) {
		std::string line = "param " + tag + " " + p.key + " unit=" + unitName(p.unit);
		if (isLabelled(p)) {
			// A LABELLED CHOICE IS STORED BY ITS LABEL, so its index range means nothing on disk: the labels
			// are the whole of what it promises. Last on the line, because a label may contain a space.
			line += " choice=";
			for (size_t i = 0; i < p.names.size(); ++i) {
				line += (i == 0 ? "" : "|") + std::string(p.names[i] != nullptr ? p.names[i] : "");
			}
		}
		else {
			line += " min=" + num(p.min) + " max=" + num(p.max) + " stepped=" + (p.stepped ? "1" : "0");
			if (p.zones != 0) {
				line += " zones=" + std::to_string(p.zones);
			}
		}
		out.push_back(line);
	}
	if (d.traits.extraStateKey != nullptr) {
		out.push_back("extra " + tag + " " + d.traits.extraStateKey + " floats=" + std::to_string(d.traits.extraState));
	}
	// THE KEYED BLOCKS (backlog NM): a block's length is its layout, so a change of it is a meaning change, as `extra`.
	for (size_t b = 0; d.state != nullptr && b < d.state->blocks.size(); ++b) {
		out.push_back("block " + tag + " " + d.state->blocks[b].key + " floats=" + std::to_string(d.state->blocks[b].floats));
	}
	for (const feathers::device::SourceDesc& s : d.outputs) {
		out.push_back("out " + tag + " " + s.key + " bipolar=" + (s.bipolar ? "1" : "0"));
	}
	for (const feathers::device::RetiredKey& r : d.traits.retiredKeys) {
		out.push_back("retired " + tag + " " + r.key + " " + r.date + " " + r.reason);
	}
	return out;
}

std::vector<std::string> generate(std::span<const Descriptor> devices) {
	std::vector<std::string> out;
	for (const Descriptor& d : devices) {
		for (std::string& l : linesOf(d)) {
			out.push_back(std::move(l));
		}
	}
	return out;
}

const char* coreUnitName(feathers::ParamUnit u) {
	using feathers::ParamUnit;
	switch (u) {
	case ParamUnit::Number: return "number";
	case ParamUnit::Integer: return "integer";
	case ParamUnit::Zone: return "zone";
	case ParamUnit::Percent: return "percent";
	case ParamUnit::Cents: return "cents";
	case ParamUnit::Semitones: return "semitones";
	case ParamUnit::Octaves: return "octaves";
	case ParamUnit::Decibels: return "decibel";
	case ParamUnit::Gain: return "gain";
	case ParamUnit::LogHertz: return "log_hertz";
	case ParamUnit::LogSeconds: return "log_seconds";
	case ParamUnit::GlideSeconds: return "glide_seconds";
	case ParamUnit::BendChoice: return "bend_choice";
	case ParamUnit::Choice: return "choice";
	case ParamUnit::Factor: return "factor";
	case ParamUnit::LogFactor: return "log_factor";
	case ParamUnit::Hertz: return "hertz";
	case ParamUnit::Vowel: return "vowel";
	}
	return "unknown";
}

/// **THE CORE PARAMETERS THAT DECLARE A KEY** (backlog KE, ahead of JF step 2), as a device named `core` — one
/// line each in the device lines' own shape, so the same comparison guards a rescale of Stack Detune exactly as
/// it guards a device's. A core row without a key is still stored by CLAP id and is not in the ledger yet.
std::vector<std::string> coreLines() {
	std::vector<std::string> out;
	out.push_back("device core - schema=1");
	for (const feathers::ParamDesc& p : feathers::paramDescs()) {
		if (p.key == nullptr) {
			continue;
		}
		std::string line = std::string("param core ") + p.key + " unit=" + coreUnitName(p.unit);
		if (p.unit == feathers::ParamUnit::Choice && !p.names.empty()) {
			line += " choice=";
			for (size_t i = 0; i < p.names.size(); ++i) {
				line += (i == 0 ? "" : "|") + std::string(p.names[i] != nullptr ? p.names[i] : "");
			}
		}
		else {
			const bool stepped = (p.flags & CLAP_PARAM_IS_STEPPED) != 0;
			line += " min=" + num(p.min) + " max=" + num(p.max) + " stepped=" + (stepped ? "1" : "0");
		}
		out.push_back(line);
	}
	// AND THE CORE'S KEYED EXTRA STATE (the `spectral` block, kStateVersion 51): the spectral drawer's master switch and
	// each sound's drawn curve, in a device's extra-state shape, so a change of length is refused as a meaning change exactly as a
	// device's is.
	out.push_back(std::string("extra core ") + feathers::kSpectralOnKey + " floats=" + std::to_string(feathers::kSpectralOnFloats));
	for (size_t slot = 0; slot < feathers::kMaxSounds; ++slot) {
		out.push_back(std::string("extra core ") + feathers::kSpectralCurveOnKeys[slot]
		              + " floats=" + std::to_string(feathers::kSpectralCurveOnFloats));
		out.push_back(std::string("extra core ") + feathers::kSpectralCurveGainKeys[slot]
		              + " floats=" + std::to_string(feathers::kSpectralCurveGainFloats));
	}
	// OE (2026-10-26): AND THE ENGINE VOCODER'S HELD ENVELOPE in the same block, its words a fixed layout (additive design §9.3).
	out.push_back(std::string("extra core ") + feathers::kVocodeFreezeKey + " floats=" + std::to_string(feathers::kVocodeFreezeFloats));
	// ON (2026-10-26): AND THE TERRAIN'S LIKES, in their own `likes` block — each like's place and its partial envelope, a fixed layout.
	out.push_back(std::string("extra core ") + feathers::kTerrainLikesKey + " floats=" + std::to_string(feathers::kTerrainLikesFloats));
	// QC (2026-11-06): AND WHICH PRESET THE PATCH CAME FROM, in its own `preset` block — two keyed TEXT values, not floats.
	out.push_back(std::string("extra core ") + feathers::kPresetNameKey + " text");
	out.push_back(std::string("extra core ") + feathers::kPresetCategoryKey + " text");
	// AND THE CORE'S RETIRED KEYS (A-PH3, 2026-10-18), in a device's retired-line shape: a key is never reused.
	for (const feathers::device::RetiredKey& r : feathers::kCoreRetiredKeys) {
		out.push_back(std::string("retired core ") + r.key + " " + r.date + " " + r.reason);
	}
	return out;
}

/// Everything a patch stores a value under: the registry's devices, then the keyed core parameters.
std::vector<std::string> generateAll() {
	std::vector<std::string> out = generate(feathers::kDevices);
	for (std::string& l : coreLines()) {
		out.push_back(std::move(l));
	}
	return out;
}

Line parseLine(const std::string& text) {
	Line l;
	l.text = text;
	std::istringstream in(text);
	in >> l.kind >> l.tag >> l.key;
	std::string rest;
	std::getline(in, rest);
	if (!rest.empty() && rest[0] == ' ') {
		rest.erase(0, 1);
	}
	if (l.kind == "retired") {
		l.attrs["why"] = rest;
		return l;
	}
	// `choice=` runs to the end of the line; every other attribute is one token.
	const size_t choice = rest.find("choice=");
	if (choice != std::string::npos) {
		l.attrs["choice"] = rest.substr(choice + 7);
		rest.erase(choice);
	}
	std::istringstream attrs(rest);
	std::string token;
	while (attrs >> token) {
		const size_t eq = token.find('=');
		if (eq != std::string::npos) {
			l.attrs[token.substr(0, eq)] = token.substr(eq + 1);
		}
	}
	return l;
}

std::vector<std::string> splitLabels(const std::string& s) {
	std::vector<std::string> out;
	size_t from = 0;
	for (;;) {
		const size_t bar = s.find('|', from);
		out.push_back(s.substr(from, bar == std::string::npos ? std::string::npos : bar - from));
		if (bar == std::string::npos) {
			return out;
		}
		from = bar + 1;
	}
}

//------------------------------------------------------------------------------------------------//
// THE COMPARISON                                                                                 //
//------------------------------------------------------------------------------------------------//

struct Finding {
	bool meaning = false; ///< false: harmless (paste or refresh); true: the stored numbers would change meaning
	std::string text;
};

/// Where a device's aliases live, so a Choice label that went can be forgiven when it was renamed.
using AliasLookup = bool (*)(const std::string& tag, const std::string& key, const std::string& from,
                             std::string* to);

/// THE RULES, as one pure function over two ledgers, so the cases below can hold it to them with
/// synthetic lines and the known-bad can break it.
std::vector<Finding> compare(const std::vector<std::string>& ledgerText, const std::vector<std::string>& nowText,
                             AliasLookup aliasOf) {
	std::map<std::string, Line> ledger;
	std::map<std::string, Line> now;
	for (const std::string& t : ledgerText) {
		if (t.empty() || t[0] == '#') {
			continue;
		}
		const Line l = parseLine(t);
		ledger[l.id()] = l;
	}
	for (const std::string& t : nowText) {
		const Line l = parseLine(t);
		now[l.id()] = l;
	}
	const auto schemaMoved = [&](const std::string& tag) {
		const auto was = ledger.find("device " + tag + " -");
		const auto is = now.find("device " + tag + " -");
		return was != ledger.end() && is != now.end()
		       && std::atoll(is->second.attr("schema").c_str()) > std::atoll(was->second.attr("schema").c_str());
	};
	std::vector<Finding> out;
	const auto meaning = [&](const Line& l, const std::string& why) {
		if (schemaMoved(l.tag)) {
			out.push_back({ false, "MIGRATED (" + l.tag + "'s schema moved): " + why + "\n    now: " + l.text });
		}
		else {
			out.push_back({ true, "MEANING CHANGED — " + why + ". This key's stored numbers would now mean something "
			                          "else: change the key (retire the old one in Traits::retiredKeys, and old "
			                          "values fall to the default) or bump the device's Traits::schema with a "
			                          "migration.\n    was: "
			                          + ledger[l.id()].text + "\n    now: " + l.text });
		}
	};

	for (const auto& [id, cur] : now) {
		if (cur.kind == "retired") {
			if (ledger.find(id) == ledger.end()) {
				out.push_back({ false, "NEW retirement, paste: " + cur.text });
			}
			continue;
		}
		// A RETIRED KEY NAMES NOTHING AGAIN — whatever kind of thing it now is.
		if (cur.kind != "device" && ledger.find("retired " + cur.tag + " " + cur.key) != ledger.end()) {
			out.push_back({ true, "RETIRED KEY REUSED — " + cur.tag + " " + cur.key
			                          + " was retired and names something again; never reuse a key.\n    now: "
			                          + cur.text });
			continue;
		}
		const auto was = ledger.find(id);
		if (was == ledger.end()) {
			out.push_back({ false, "NEW, paste: " + cur.text });
			continue;
		}
		const Line& old = was->second;
		if (old.text == cur.text) {
			continue;
		}
		if (cur.kind == "device") {
			out.push_back({ false, "REFRESH (schema): " + cur.text });
			continue;
		}
		if (cur.kind == "extra" || cur.kind == "block") {
			meaning(cur, "the extra state changed length");
			continue;
		}
		if (cur.kind == "out") {
			meaning(cur, "the output's polarity changed");
			continue;
		}
		// A CONTROL.
		if (old.attr("unit") != cur.attr("unit")) {
			meaning(cur, "the unit changed (" + old.attr("unit") + " -> " + cur.attr("unit") + ")");
			continue;
		}
		if (!old.attr("choice").empty() || !cur.attr("choice").empty()) {
			const std::vector<std::string> was = splitLabels(old.attr("choice"));
			const std::vector<std::string> is = splitLabels(cur.attr("choice"));
			std::string gone;
			for (const std::string& label : was) {
				bool live = false;
				for (const std::string& l : is) {
					live = live || l == label;
				}
				std::string to;
				if (!live && !(aliasOf != nullptr && aliasOf(cur.tag, cur.key, label, &to))) {
					gone += (gone.empty() ? "'" : ", '") + label + "'";
				}
			}
			if (!gone.empty()) {
				meaning(cur, "Choice label " + gone + " is gone with no ChoiceAlias");
			}
			else {
				out.push_back({ false, "REFRESH (labels gained, reordered or aliased — stored by label, harmless): "
				                           + cur.text });
			}
			continue;
		}
		const double oldMin = std::atof(old.attr("min").c_str());
		const double oldMax = std::atof(old.attr("max").c_str());
		const double curMin = std::atof(cur.attr("min").c_str());
		const double curMax = std::atof(cur.attr("max").c_str());
		if (old.attr("stepped") != cur.attr("stepped")) {
			meaning(cur, "stepped changed");
		}
		else if (old.attr("zones") != cur.attr("zones")) {
			meaning(cur, "the zone count changed");
		}
		else if (curMin > oldMin || curMax < oldMax) {
			meaning(cur, "the range narrowed or moved (a rescale)");
		}
		else {
			out.push_back({ false, "REFRESH (range widened, same unit — harmless): " + cur.text });
		}
	}
	for (const auto& [id, old] : ledger) {
		if (now.find(id) != now.end() || old.kind == "retired") {
			continue;
		}
		if (old.kind == "device") {
			out.push_back({ false, "REMOVE (device gone): " + old.text });
			continue;
		}
		if (now.find("retired " + old.tag + " " + old.key) != now.end()) {
			out.push_back({ false, "REMOVE (retired): " + old.text });
			continue;
		}
		if (now.find("device " + old.tag + " -") == now.end()) {
			out.push_back({ false, "REMOVE (device gone): " + old.text });
			continue;
		}
		out.push_back({ true, "KEY GONE WITHOUT RETIREMENT — " + old.tag + " " + old.key
		                          + " no longer exists. If the control was removed, add it to that device's "
		                          "Traits::retiredKeys with a date and a reason (and remove this line); if it "
		                          "was renamed, give it back its key.\n    was: "
		                          + old.text });
	}
	return out;
}

bool registryAlias(const std::string& tag, const std::string& key, const std::string& from, std::string* to) {
	for (const Descriptor& d : feathers::kDevices) {
		if (tag != d.traits.tag) {
			continue;
		}
		for (const ParamDesc& p : d.params) {
			if (key != p.key) {
				continue;
			}
			for (const feathers::device::ChoiceAlias& a : p.choiceAliases) {
				if (from == a.from) {
					*to = a.to;
					return true;
				}
			}
		}
	}
	return false;
}

std::string kHeaderFor(const std::string& tag) {
	return "# Schema ledger for " + tag + " (backlog JF, split per device by NY.1). Checked by tests/test_state_ledger.cpp, whose banner says what each\n"
	       "# difference means: a new line is pasted, a harmless change is refreshed (FEATHERS_LEDGER_WRITE=1), a MEANING change is a new key or a schema bump.\n";
}

/// Every `*.ledger` file under `dir`, in file-name order. The ledger is one file PER DEVICE (plus `core`), named by
/// the tag its lines carry (backlog NY.1: two branches adding two devices no longer append to the same file).
std::vector<std::string> readLedger(const char* dir) {
	std::vector<std::filesystem::path> files;
	for (const auto& e : std::filesystem::directory_iterator(dir)) {
		if (e.path().extension() == ".ledger") {
			files.push_back(e.path());
		}
	}
	std::sort(files.begin(), files.end());
	std::vector<std::string> out;
	for (const auto& p : files) {
		std::ifstream in(p);
		std::string line;
		while (std::getline(in, line)) {
			out.push_back(line);
		}
	}
	return out;
}

/// Writes `lines` back as one file per tag, and removes the files of tags no longer present. A tag becomes a file
/// name, so it must be one (`[A-Za-z0-9_]`); the registry's tags are, and a tag that is not fails the write.
bool writeLedger(const char* dir, const std::vector<std::string>& lines) {
	std::map<std::string, std::vector<std::string>> byTag;
	for (const std::string& l : lines) {
		const Line p = parseLine(l);
		for (const char c : p.tag) {
			if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') {
				return false;
			}
		}
		byTag[p.tag].push_back(l);
	}
	for (const auto& [tag, ls] : byTag) {
		std::ofstream out(std::filesystem::path(dir) / (tag + ".ledger"), std::ios::trunc);
		out << kHeaderFor(tag);
		for (const std::string& l : ls) {
			out << l << "\n";
		}
	}
	for (const auto& e : std::filesystem::directory_iterator(dir)) {
		if (e.path().extension() == ".ledger" && byTag.find(e.path().stem().string()) == byTag.end()) {
			std::filesystem::remove(e.path());
		}
	}
	return true;
}

} // namespace

//------------------------------------------------------------------------------------------------//
// THE REGISTRY'S OWN KEYS                                                                        //
//------------------------------------------------------------------------------------------------//

TEST_CASE("state keys: every device's controls, outputs and extra state have valid, distinct keys") {
	// THE SAME RULE `device_registry.h` ASSERTS AT COMPILE TIME, asked again here so a failure NAMES the
	// device and the key rather than only refusing to build.
	for (const Descriptor& d : feathers::kDevices) {
		INFO("device " << d.traits.tag);
		for (size_t i = 0; i < d.params.size(); ++i) {
			INFO("control " << i << " (" << (d.params[i].name != nullptr ? d.params[i].name : "?") << ")");
			CHECK(feathers::device::validStateKey(d.params[i].key));
			for (size_t j = i + 1; j < d.params.size(); ++j) {
				INFO("against control " << j);
				CHECK_FALSE(feathers::device::sameKey(d.params[i].key, d.params[j].key));
			}
			if (d.traits.extraStateKey != nullptr) {
				CHECK_FALSE(feathers::device::sameKey(d.params[i].key, d.traits.extraStateKey));
			}
		}
		CHECK((d.traits.extraState != 0) == (d.traits.extraStateKey != nullptr));
		for (size_t i = 0; i < d.outputs.size(); ++i) {
			INFO("output " << i << " (" << (d.outputs[i].name != nullptr ? d.outputs[i].name : "?") << ")");
			CHECK(feathers::device::validStateKey(d.outputs[i].key));
			for (size_t j = i + 1; j < d.outputs.size(); ++j) {
				CHECK_FALSE(feathers::device::sameKey(d.outputs[i].key, d.outputs[j].key));
			}
		}
		CHECK(feathers::deviceKeysValid(d));
		CHECK(feathers::retiredKeysUnused(d));
		CHECK(feathers::choiceAliasesSound(d));
	}
}

TEST_CASE("state ledger: the checked-in ledger agrees with the registry") {
	const std::vector<std::string> ledger = readLedger(FEATHERS_LEDGER_DIR);
	const std::vector<std::string> now = generateAll();
	const std::vector<Finding> findings = compare(ledger, now, &registryAlias);
	size_t meanings = 0;
	std::string report;
	for (const Finding& f : findings) {
		meanings += f.meaning ? 1 : 0;
		report += (f.meaning ? "  !! " : "  -- ") + f.text + "\n";
	}
	const char* write = std::getenv("FEATHERS_LEDGER_WRITE");
	if (write != nullptr && std::string(write) == "1" && !findings.empty()) {
		if (meanings == 0) {
			REQUIRE(writeLedger(FEATHERS_LEDGER_DIR, now));
			MESSAGE("FEATHERS_LEDGER_WRITE=1: the ledger was refreshed (" << findings.size()
			                                                               << " harmless differences):\n"
			                                                               << report);
			return;
		}
		MESSAGE("FEATHERS_LEDGER_WRITE=1 REFUSED: a meaning change stands, and the ledger may not be regenerated "
		        "past it.");
	}
	INFO("tests/ledger/*.ledger differs from the registry in " << findings.size() << " line(s), " << meanings
	                                                                 << " of them MEANING CHANGES:\n"
	                                                                 << report);
	CHECK(findings.empty());
}

//------------------------------------------------------------------------------------------------//
// THE RULES, AGAINST SYNTHETIC LINES — so a rule that stopped firing is seen without a real device      //
// having to change first. Known-bad (d): make `compare` ignore ranges, and the rescale case goes green.  //
//------------------------------------------------------------------------------------------------//

namespace {

std::vector<Finding> diff(std::vector<std::string> was, std::vector<std::string> is) {
	return compare(was, is, [](const std::string& tag, const std::string& key, const std::string& from,
	                           std::string* to) {
		if (tag == "TST" && key == "mode" && from == "Hall") {
			*to = "Chamber";
			return true;
		}
		return false;
	});
}

size_t countMeaning(const std::vector<Finding>& f) {
	size_t n = 0;
	for (const Finding& x : f) {
		n += x.meaning ? 1 : 0;
	}
	return n;
}

const std::string kDevice = "device TST - schema=1";

} // namespace

TEST_CASE("state ledger rules: a rescale, a narrowing, a new unit, a step or zone change are MEANING changes") {
	const std::string was = "param TST skew unit=percent min=0 max=100 stepped=0";
	CHECK(countMeaning(diff({ kDevice, was }, { kDevice, "param TST skew unit=percent min=0 max=1 stepped=0" })) == 1);
	CHECK(countMeaning(diff({ kDevice, was }, { kDevice, "param TST skew unit=percent min=10 max=100 stepped=0" }))
	      == 1);
	CHECK(countMeaning(diff({ kDevice, was }, { kDevice, "param TST skew unit=number min=0 max=100 stepped=0" }))
	      == 1);
	CHECK(countMeaning(diff({ kDevice, was }, { kDevice, "param TST skew unit=percent min=0 max=100 stepped=1" }))
	      == 1);
	CHECK(countMeaning(
	          diff({ kDevice, was }, { kDevice, "param TST skew unit=percent min=0 max=100 stepped=0 zones=8" }))
	      == 1);
	CHECK(countMeaning(diff({ kDevice, "extra TST travel floats=18" }, { kDevice, "extra TST travel floats=19" }))
	      == 1);
}

TEST_CASE("state ledger rules: a widening, a new key, gained or aliased labels are harmless") {
	const std::string was = "param TST skew unit=percent min=0 max=1 stepped=0";
	std::vector<Finding> f = diff({ kDevice, was }, { kDevice, "param TST skew unit=percent min=-1 max=2 stepped=0" });
	CHECK(f.size() == 1);
	CHECK(countMeaning(f) == 0);

	f = diff({ kDevice, was }, { kDevice, was, "param TST fresh unit=number min=0 max=4 stepped=1" });
	REQUIRE(f.size() == 1);
	CHECK(countMeaning(f) == 0);
	CHECK(f[0].text.find("NEW, paste: param TST fresh") != std::string::npos);

	const std::string labels = "param TST mode unit=choice choice=Room|Hall|Plate";
	// gained in the middle, and reordered: stored by label, so nothing moves
	f = diff({ kDevice, labels }, { kDevice, "param TST mode unit=choice choice=Plate|Room|Spring|Hall" });
	CHECK(f.size() == 1);
	CHECK(countMeaning(f) == 0);
	// renamed WITH an alias
	f = diff({ kDevice, labels }, { kDevice, "param TST mode unit=choice choice=Room|Chamber|Plate" });
	CHECK(countMeaning(f) == 0);
	// renamed WITHOUT one: this lookup aliases Hall and nothing else, so a Plate renamed to Slab is lost
	f = diff({ kDevice, labels }, { kDevice, "param TST mode unit=choice choice=Room|Chamber|Slab" });
	CHECK(countMeaning(f) == 1);
}

TEST_CASE("state ledger rules: a key gone must be retired; a retired key may never come back") {
	const std::string was = "param TST skew unit=percent min=0 max=1 stepped=0";
	CHECK(countMeaning(diff({ kDevice, was }, { kDevice })) == 1);
	std::vector<Finding> f = diff({ kDevice, was }, { kDevice, "retired TST skew 2026-09-25 rescaled to 0..1" });
	CHECK(countMeaning(f) == 0);
	CHECK(f.size() == 2); // the retirement to paste, the old line to remove
	CHECK(countMeaning(diff({ kDevice, "retired TST skew 2026-09-25 rescaled" },
	                        { kDevice, "retired TST skew 2026-09-25 rescaled", was }))
	      == 1);
	// A whole device gone is not a meaning change: there is nobody left to misread it.
	CHECK(countMeaning(diff({ kDevice, was }, {})) == 0);
}

TEST_CASE("state ledger rules: a schema bump lets a meaning change through, as a migration") {
	const std::string was = "param TST skew unit=percent min=0 max=100 stepped=0";
	const std::string is = "param TST skew unit=percent min=0 max=1 stepped=0";
	CHECK(countMeaning(diff({ kDevice, was }, { kDevice, is })) == 1);
	CHECK(countMeaning(diff({ kDevice, was }, { "device TST - schema=2", is })) == 0);
}
