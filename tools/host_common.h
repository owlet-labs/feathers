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

#include "plugin/feathers_test_chain.h"

#include <clap/clap.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>
#include <cstdlib>
#include <filesystem>
#include <system_error>
#include <dlfcn.h>
#include <unistd.h>

// WHAT ALL THREE TEST HOSTS DO BEFORE THEY DIFFER.
//
// `guihost`, `cyclehost` and `bitwighost` each dlopen a `.clap`, find `clap_entry`, walk to the
// factory and create the first plugin. Three copies of one dance, and they had already drifted — not
// in behaviour but in what they TELL YOU when it fails, which is the half that costs a morning.
//
// THE GRANULAR REPORTING IS GUIHOST'S AND IT IS THE ONE WORTH KEEPING. Its own comment says why:
// folding the steps together "saves three lines and costs the next person the difference between
// 'that file is not a CLAP plugin' and 'it is, and it refused to start' — which are not the same
// morning's work". `cyclehost` had folded two pairs and `bitwighost` a third; consolidating here
// gives the other two the better of the two behaviours rather than the average of them.
//
// AND ONE DIFFERENCE THAT LOOKS REAL AND IS NOT, recorded so nobody restores it: `bitwighost` opened
// with `RTLD_LOCAL | RTLD_NOW` and the other two with `RTLD_NOW` alone. Under glibc the default when
// neither LOCAL nor GLOBAL is given IS local, so the two spellings are the same call. Written out
// here because "the DAW-shaped host passes an extra flag" reads like something load-bearing.

namespace feathers::toolhost {

/// **KEEP A TEST HOST OUT OF THE USER'S REAL CONFIG AND PRESET LIBRARY.**
///
/// These tools open a REAL editor, and the editor persists: preferences to
/// `$XDG_CONFIG_HOME/feathers/editor.conf` and presets to `$XDG_DATA_HOME/feathers`. Run bare, they
/// write the files the installed plugin reads.
///
/// THIS ALREADY COST SOMETHING. `widget-shot` calls `setPopoutWindowForTest(false)` on its expand
/// path, and a stretch of builds opened expanded panels INLINE instead of as floating windows —
/// The behaviour was reported and nobody connected it, because nobody imagined a capture tool could
/// reach the user's config. `venue-xvfb.sh` mitigates it, and nothing enforces the venue: a tool invoked
/// directly is unguarded, which is a guard that depends on remembering to use it.
///
/// SO THE REDIRECT LIVES WHERE IT CANNOT BE BYPASSED — in the tools themselves, before anything
/// loads. **It defers to a caller who has already said where their configuration lives**: an existing
/// `XDG_CONFIG_HOME` is left alone, which is what makes `venue-xvfb.sh` still work and what lets a
/// developer point a tool at a prepared directory.
///
/// AND THERE IS AN ESCAPE HATCH, because "reproduce the bug with my real preferences" is a real
/// reason: set `FEATHERS_REAL_CONFIG=1` and this does nothing.
inline void isolateUserConfig(const char* toolName) {
	if (const char* real = std::getenv("FEATHERS_REAL_CONFIG"); real != nullptr && real[0] == '1') {
		std::printf("config:   REAL — $FEATHERS_REAL_CONFIG=1, so %s may write your editor.conf\n",
		            toolName);
		return;
	}
	const auto redirect = [&](const char* variable, const char* kind) {
		// A CALLER WHO SET IT HAS SAID WHERE THEIR CONFIGURATION LIVES — believed as given, exactly as
		// `EditorPrefs::path` believes it. This is what keeps `venue-xvfb.sh`'s scratch dirs in force.
		if (const char* existing = std::getenv(variable);
		    existing != nullptr && existing[0] != '\0') {
			return;
		}
		std::error_code code;
		const std::filesystem::path scratch =
		    std::filesystem::temp_directory_path()
		    / (std::string("feathers-tool-") + kind + "-"
		       + std::to_string(static_cast<long>(::getpid())));
		std::filesystem::create_directories(scratch, code);
		if (code) {
			// COULD NOT MAKE ONE — say so rather than fall through silently to the real directory.
			std::printf("config:   WARNING — no scratch %s dir (%s); %s may write your real one\n",
			            kind, code.message().c_str(), toolName);
			return;
		}
		::setenv(variable, scratch.c_str(), 1);
	};
	redirect("XDG_CONFIG_HOME", "config");
	// AND THE DATA DIR TOO. Presets are XDG DATA, not config — redirecting one and not the other is
	// the exact half-guard `venue-xvfb.sh` shipped with.
	redirect("XDG_DATA_HOME", "data");
	// AND THE PRESET ROOT, which since backlog FO phase 2 is ~/Documents/Owlet/Feathers/Presets rather
	// than anything under XDG — neither redirect above reaches it.
	redirect("FEATHERS_PRESET_ROOT", "presets");
}

/// A loaded plugin and everything needed to shut it down again. Falsy when the load failed — every
/// failure has already been reported by then, on its own line, saying which step refused.
struct Loaded {
	void* lib = nullptr;
	const clap_plugin_entry* entry = nullptr;
	/// KEPT BECAUSE ONE HOST CREATES TWICE. `cyclehost`'s FEATHERS_CYCLE_RECREATE_PLUGIN throws the
	/// whole instance away between rounds — *"which is what removing a device and adding it again
	/// does"* — so the factory has to outlive the first create rather than being a local of it.
	const clap_plugin_factory* factory = nullptr;
	const clap_plugin* plugin = nullptr;
	const clap_plugin_descriptor* descriptor = nullptr;

	explicit operator bool() const noexcept { return plugin != nullptr; }
};

/// Create ANOTHER instance from an already-loaded bundle, reporting its own failure.
///
/// The second half of the same duplication: `cyclehost` had create-and-init-and-report written twice,
/// once in the load and once in the recreate, and only the first of them said which step refused.
inline const clap_plugin* createAnother(const Loaded& loaded, const clap_host* host) {
	if (loaded.factory == nullptr || loaded.descriptor == nullptr) {
		std::printf("createAnother: nothing loaded to create from\n");
		return nullptr;
	}
	const clap_plugin* made =
	    loaded.factory->create_plugin(loaded.factory, host, loaded.descriptor->id);
	if (made == nullptr) {
		std::printf("create_plugin returned nothing for %s\n", loaded.descriptor->id);
		return nullptr;
	}
	if (!made->init(made)) {
		std::printf("plugin->init refused for %s\n", loaded.descriptor->id);
		return nullptr;
	}
	return made;
}

/// Open `path`, create its FIRST plugin against `host`, and return both. Reports its own failure.
///
/// THE FIRST PLUGIN, NOT A NAMED ONE, because this bundle ships exactly one and a host that had to
/// name it would need updating the day the id changed — which is the coupling these tools exist to
/// avoid rather than to demonstrate.
/// IS THIS CAPTURE A SINGLE FLAT COLOUR? A photograph of an editor is not one colour, and `import`
/// returns 0 for an all-black grab of a window that was not ready.
///
/// SHARED, BECAUSE ALL THREE HOSTS PHOTOGRAPH AND ALL THREE WERE EXPOSED. A blank frame passes every
/// other check any of them makes: valid PNG, zero exit status, and a DISTINCT md5 -- distinct
/// precisely BECAUSE it is blank, so hashing frames to prove they differ positively approves one.
/// The hash rule catches a window that is ASLEEP and returns IDENTICAL frames; only this catches one
/// that returns a DIFFERENT frame which happens to be empty. Opposite directions, both needed.
///
/// MEASURED, NOT CHOSEN. On this venue a blank frame is 260 bytes with a deviation of exactly 0,
/// against ~140 KB and 0.0684 / 0.0695 / 0.0731 for real captures of three different devices; 12
/// fresh-window runs produced 1 blank. The threshold sits two orders of magnitude below the real
/// floor, so it answers "uniform" and not "boring".
inline bool captureIsUniform(const std::string& path) {
	char command[640];
	std::snprintf(command, sizeof(command),
	              "test \"$(convert %s -colorspace Gray -format '%%[fx:standard_deviation>0.001]'"
	              " info: 2>/dev/null)\" = 1",
	              path.c_str());
	// Non-zero means the deviation was NOT above the threshold -- uniform, or `convert` could not
	// answer at all. Neither is a photograph of an editor.
	return std::system(command) != 0;
}

//----------------------------------------------------------------------------------------------//
// SEEDING THE FX LANE -- shared, because all three hosts want it for the same reason: no device //
// surface can be photographed until a device is IN the lane, and device controls are swallowed  //
// rather than CLAP parameters, so clap_plugin_params cannot reach one.                          //
//----------------------------------------------------------------------------------------------//

/// The extension, or null if this plugin does not offer it (an older .clap, say).
inline const clap_plugin_feathers_test_chain* testChain(const clap_plugin* plugin) {
	if (plugin == nullptr || plugin->get_extension == nullptr) {
		return nullptr;
	}
	return static_cast<const clap_plugin_feathers_test_chain*>(
	    plugin->get_extension(plugin, CLAP_EXT_FEATHERS_TEST_CHAIN));
}

/// EVERY DEVICE THIS PLUGIN WILL LET US PLACE, in the plugin's own order. Empty if it does not
/// offer the extension.
///
/// ASKED, NOT HARD-CODED. The first six surfaces were photographed with the tags read out of
/// `*_types.h` by hand, which makes the INVOKER the thing somebody must edit when a device is added
/// -- the hardcoded-list failure one step out from the host. This is what `--set` already does for
/// host parameters, and the hosts still link nothing of ours: the tags come from the plugin.
inline std::vector<std::string> availableDevices(const clap_plugin* plugin) {
	std::vector<std::string> tags;
	const clap_plugin_feathers_test_chain* chain = testChain(plugin);
	if (chain == nullptr) {
		return tags;
	}
	for (uint32_t i = 0;; ++i) {
		char tag[64] = {};
		if (!chain->available_device(plugin, i, tag, sizeof(tag))) {
			break;
		}
		tags.emplace_back(tag);
	}
	return tags;
}

/// One device to place, and what to set on it once it is there.
struct SeedRequest {
	std::string tag;
	std::vector<std::pair<std::string, double>> controls;
};

/// PLACE EVERY REQUESTED DEVICE AND APPLY ITS CONTROLS, reporting each step by name.
///
/// BY NAME AT EVERY DOOR, which is guihost's existing rule for `--set` and holds here for a sharper
/// reason: the host does not link our device registry -- see the tools' CMake comment -- so a tag or
/// a control name is resolved by the PLUGIN against its own tables. A name this build does not know
/// is reported and refused rather than quietly setting nothing, and there is no index that could
/// silently mean a different control in the two binaries.
///
/// FALSE IF ANYTHING WAS REFUSED, and the caller should treat that as fatal rather than photograph
/// anyway: a picture of a lane that is missing the device it was asked for is the failure this whole
/// seam exists to prevent, and it looks exactly like a success.
inline bool seedFxChain(const clap_plugin* plugin, const std::vector<SeedRequest>& seeds) {
	if (seeds.empty()) {
		return true;
	}
	const clap_plugin_feathers_test_chain* chain = testChain(plugin);
	if (chain == nullptr) {
		std::printf("seed:     plugin does not offer %s\n", CLAP_EXT_FEATHERS_TEST_CHAIN);
		return false;
	}

	// COUNTED BEFORE AND AFTER, because the lane is NOT EMPTY when we arrive -- a fresh plugin comes
	// up with a device already in slot 0, so the first run of this compared a TOTAL against a DELTA
	// and passed on arithmetic that happened to work. A guard that is satisfied by devices it did not
	// place is exactly the vacuous check it was written to prevent, one level up.
	const uint32_t before = chain->device_count(plugin);
	bool ok = true;
	for (const SeedRequest& seed : seeds) {
		const uint32_t slot = chain->add_device(plugin, seed.tag.c_str());
		if (slot == UINT32_MAX) {
			std::printf("seed:     device \"%s\" REFUSED (unknown tag, not an audio device,"
			            " or the lane is full)\n",
			            seed.tag.c_str());
			ok = false;
			continue;
		}
		std::printf("seed:     device \"%s\" -> slot %u\n", seed.tag.c_str(), slot);
		for (const auto& [name, value] : seed.controls) {
			if (chain->set_control(plugin, slot, name.c_str(), value)) {
				std::printf("            %-24s -> %g\n", name.c_str(), value);
			} else {
				std::printf("            %-24s REFUSED (no such control on this device)\n",
				            name.c_str());
				ok = false;
			}
		}
	}

	// THE NON-VACUITY GUARD, and the reason device_count is in the extension at all. Without it a run
	// that placed nothing reports exactly what a run that worked reports, and the photograph is of an
	// empty lane. Asked of the PLUGIN rather than counted here, so it is the lane's own answer.
	const uint32_t after = chain->device_count(plugin);
	const uint32_t added = after >= before ? after - before : 0;
	std::printf("seed:     lane holds %u device(s), %u added by this run (%zu asked for)\n", after,
	            added, seeds.size());
	if (added != seeds.size()) {
		std::printf("seed:     THE LANE IS NOT WHAT WAS ASKED FOR -- refusing to photograph it\n");
		ok = false;
	}
	return ok;
}

inline Loaded loadPlugin(const char* path, const clap_host* host) {
	Loaded out;

	out.lib = dlopen(path, RTLD_NOW);
	if (out.lib == nullptr) {
		std::printf("dlopen failed: %s\n", dlerror());
		return {};
	}
	out.entry = static_cast<const clap_plugin_entry*>(dlsym(out.lib, "clap_entry"));
	if (out.entry == nullptr) {
		std::printf("no clap_entry symbol in %s — not a CLAP plugin?\n", path);
		return {};
	}
	if (!out.entry->init(path)) {
		std::printf("clap_entry.init refused %s\n", path);
		return {};
	}
	out.factory =
	    static_cast<const clap_plugin_factory*>(out.entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
	const clap_plugin_factory* factory = out.factory;
	if (factory == nullptr) {
		std::printf("plugin offers no plugin factory\n");
		return {};
	}
	if (factory->get_plugin_count(factory) == 0) {
		std::printf("plugin factory is empty\n");
		return {};
	}
	out.descriptor = factory->get_plugin_descriptor(factory, 0);
	if (out.descriptor == nullptr) {
		std::printf("plugin factory returned no descriptor\n");
		return {};
	}
	out.plugin = factory->create_plugin(factory, host, out.descriptor->id);
	if (out.plugin == nullptr) {
		std::printf("create_plugin returned nothing for %s\n", out.descriptor->id);
		return {};
	}
	if (!out.plugin->init(out.plugin)) {
		std::printf("plugin->init refused for %s\n", out.descriptor->id);
		return {};
	}
	return out;
}

} // namespace feathers::toolhost
