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

// **PRESETS — the store, the shared stepper, the browser and its prompt** (backlog FO). Their own binary
// since phase 2 gathered what was spread over `controls` and `lane`: the library on disk
// (~/Documents/Owlet/Feathers/Presets, or a folder a person chose), factory files, the name-and-tags
// prompt, rename, delete, the list's Manage submenu, tag browsing and tag suggestions. Every case asserts VALUES — a path, a byte, a tag —
// read off the store or the browser; none infers them from a picture.

#include "fixtures.h"

#include "fx/sklitter/sklitter_types.h"
#include "plugin/device_preset.h"
#include "plugin/preset_tags.h"
#include "plugin/ui/edit_history.h"
#include "plugin/ui/item_stepper.h"
#include "plugin/ui/text_field.h"
#include "plugin/ui/expanded_canvas.h"
#include "plugin/ui/menu_anchor.h"
#include "plugin/ui/sklitter_expanded.h"
#include "plugin/ui/container_presets.h"
#include "composite/composite_types.h"
#include "../menu_placement_check.h"

#include <optional>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

namespace {

/// An environment variable set for one scope and put back after, including "was not set".
class EnvVar {
public:
	EnvVar(const char* name, const char* value) : name_(name) {
		if (const char* was = std::getenv(name); was != nullptr) {
			had_ = true;
			was_ = was;
		}
		if (value == nullptr) {
			::unsetenv(name);
		}
		else {
			::setenv(name, value, 1);
		}
	}
	~EnvVar() {
		if (had_) {
			::setenv(name_.c_str(), was_.c_str(), 1);
		}
		else {
			::unsetenv(name_.c_str());
		}
	}
	EnvVar(const EnvVar&) = delete;
	EnvVar& operator=(const EnvVar&) = delete;

private:
	std::string name_;
	bool had_ = false;
	std::string was_;
};

/// A browser over its own scratch library and a provider whose captures are counted and whose payload
/// a case can change. "Init" is its one factory preset.
struct Rig {
	ScopedPresetLibrary library;
	int captures = 0;
	uint8_t payload = 7;
	std::vector<std::vector<uint8_t>> applied;
	FunctionPresetProvider provider;
	PresetBrowser browser;

	explicit Rig(const char* label, const PresetBank& bank = "Test")
	    : library(label),
	      provider(
	          [this](PresetData& out) {
		          ++captures;
		          out.bytes = { payload };
		          return true;
	          },
	          [this](const PresetData& data) {
		          // A PAYLOAD OF 0 "DOES NOT LOAD" — how a case makes a preset fail at load time.
		          if (!data.bytes.empty() && data.bytes[0] == 0) {
			          return false;
		          }
		          applied.push_back(data.bytes);
		          return true;
	          }),
	      browser(std::vector<std::string> { "Init" }, std::vector<PresetData> { PresetData { { 1 } } }, bank,
	              PresetBrowser::kHeaderStyle) {
		browser.setBounds(0.0f, 0.0f, browser.totalWidth(), PresetBrowser::kHeight);
		browser.setProvider(&provider);
		browser.clock = [] { return int64_t { 1'800'000'000 }; };
		browser.loadBankFromDisk();
	}
	[[nodiscard]] std::vector<StoredPreset> disk() const { return PresetStore::loadAll(browser.bank()); }
	/// Save through the prompt the way a hand does: menu, name, tags, Enter.
	void save(const std::string& name, const std::string& tags = {}) {
		browser.chooseFromMenu(PresetBrowser::kMenuSave);
		REQUIRE(browser.prompt() == PresetBrowser::Prompt::Naming);
		browser.typeForTest(name);
		browser.typeTagsForTest(tags);
		browser.submitPrompt();
		REQUIRE(browser.prompt() == PresetBrowser::Prompt::Closed);
	}
};

void writeRaw(const std::filesystem::path& file, const std::vector<uint8_t>& bytes) {
	std::filesystem::create_directories(file.parent_path());
	std::ofstream out(file, std::ios::binary | std::ios::trunc);
	out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

/// A mock source for the shared stepper: `n` items, some unloadable, wrapping or not.
struct MockSource : ItemSource {
	std::vector<std::string> labels;
	std::vector<bool> loadable;
	std::vector<std::vector<std::string>> sections;
	bool wrap = false;
	int current = 0;
	std::vector<int> acted;
	size_t itemCount() const override { return labels.size(); }
	std::string itemLabel(size_t i) const override { return labels[i]; }
	int currentItem() const override { return current; }
	std::vector<bool> failsOnLoad; ///< discovered only by trying: `itemLoadable` still says yes
	std::vector<int> tried;
	bool selectItem(size_t i) override {
		tried.push_back(static_cast<int>(i));
		if (i < failsOnLoad.size() && failsOnLoad[i]) {
			return false;
		}
		current = static_cast<int>(i);
		return true;
	}
	bool itemLoadable(size_t i) const override { return i >= loadable.size() || loadable[i]; }
	bool itemsWrap() const override { return wrap; }
	std::vector<std::string> itemSections(size_t i) const override {
		return i < sections.size() ? sections[i] : std::vector<std::string> {};
	}
	std::vector<ItemAction> itemActions() const override { return { { 3, "Do it", true }, { 4, "Not now", false } }; }
	void itemAct(int id) override { acted.push_back(id); }
};

/// THE EVENT LOOP'S TIMERS, TURNED BY HAND — what a window's draw callback does every frame, and the road
/// the browser's deferred reopen travels. Until `done`, or about half a second.
bool pumpUntil(const std::function<bool()>& done) {
	for (int i = 0; i < 250 && !done(); ++i) {
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
		visage::EventManager::instance().checkEventTimers();
	}
	return done();
}

/// THE MENUS OPEN OVER `frame`: visage parents a popup on the source's top frame (`PopupMenuFrame::show`),
/// so this counts what the showing itself put there rather than anything the browser says about itself.
size_t openMenus(visage::Frame& frame) {
	size_t n = 0;
	for (visage::Frame* child : frame.topParentFrame()->children()) {
		n += dynamic_cast<visage::PopupMenuFrame*>(child) != nullptr ? 1 : 0;
	}
	return n;
}


} // namespace

//--------------------------------------------------------------------------------------------------//
// THE STORE                                                                                         //
//--------------------------------------------------------------------------------------------------//

TEST_CASE("the preset root is ~/Documents/Owlet/Feathers/Presets, overridable, with Feathers/ and Devices/ side by side") {
	const std::filesystem::path home = std::filesystem::temp_directory_path() / "feathers-fo-home";
	{
		EnvVar root("FEATHERS_PRESET_ROOT", nullptr);
		EnvVar h("HOME", home.c_str());
		CHECK(PresetStore::root() == home / "Documents" / "Owlet" / "Feathers" / "Presets");
		CHECK(PresetStore::directory() == home / "Documents" / "Owlet" / "Feathers" / "Presets" / "Feathers");
		CHECK(PresetStore::directory(devicePresetBank(fx::sklitter::kDescriptor))
		      == home / "Documents" / "Owlet" / "Feathers" / "Presets" / "Devices" / "SKLITTER");
		// NOT XDG DATA: a set XDG_DATA_HOME changes nothing.
		EnvVar xdg("XDG_DATA_HOME", "/somewhere/else");
		CHECK(PresetStore::root() == home / "Documents" / "Owlet" / "Feathers" / "Presets");
	}
	{
		EnvVar root("FEATHERS_PRESET_ROOT", "/tmp/elsewhere");
		CHECK(PresetStore::root() == std::filesystem::path("/tmp/elsewhere"));
	}
	{
		EnvVar root("FEATHERS_PRESET_ROOT", nullptr);
		EnvVar h("HOME", nullptr);
		CHECK(PresetStore::root().empty()); // no home, no library: presets live in memory
	}
}

TEST_CASE("a preset name is not a path, and a category is folders of names") {
	CHECK(PresetStore::sanitise("Warm Pad") == "Warm Pad");
	CHECK(PresetStore::sanitise("bass_01-a") == "bass_01-a");
	CHECK(PresetStore::sanitise("../../.bashrc").find("..") == std::string::npos);
	CHECK(PresetStore::sanitise("a/b").find('/') == std::string::npos);
	CHECK(PresetStore::sanitise(std::string("a\0b", 3)).find('\0') == std::string::npos);
	CHECK(PresetStore::sanitise("///").empty());
	CHECK(PresetStore::pathFor("///").empty());
	// "Pads/Glass" is the name Glass in the category Pads; traversal is inert in either half.
	CHECK(PresetStore::splitTyped("Pads/Glass") == std::pair<std::string, std::string> { "Glass", "Pads" });
	CHECK(PresetStore::splitTyped("Glass") == std::pair<std::string, std::string> { "Glass", "" });
	CHECK(PresetStore::splitTyped("../../x/Glass").second == "x");
	CHECK(PresetStore::sanitiseCategory("a//b/../c") == "a/b/c");
}

TEST_CASE("a preset file is a JSON metadata block then the payload — the block JF carries as \"meta\"") {
	const PresetMeta meta { { "dark", "tape \"chop\"" }, 1'800'000'000 };
	const std::vector<uint8_t> payload { 9, 8, 7 };
	CHECK(PresetStore::metaJson(meta) == R"({"saved": 1800000000, "tags": ["dark", "tape \"chop\""]})");
	const std::vector<uint8_t> file = PresetStore::encode(meta, payload);
	PresetMeta back;
	std::vector<uint8_t> body;
	REQUIRE(PresetStore::decode(file, &back, &body));
	CHECK(back == meta);
	CHECK(body == payload);
	// A FILE WITHOUT THE HEADER is a bare payload with no tags, not a refusal.
	REQUIRE(PresetStore::decode(payload, &back, &body));
	CHECK(back == PresetMeta {});
	CHECK(body == payload);
	// A KEY THIS BUILD DOES NOT KNOW IS SKIPPED WHOLE, nested or not — JF will add members.
	PresetMeta later;
	REQUIRE(PresetStore::parseMetaJson(R"({"author": {"n": [1, "x"]}, "tags": ["a"], "saved": 5, "x": null})", &later));
	CHECK(later.tags == std::vector<std::string> { "a" });
	CHECK(later.saved == 5);
	CHECK_FALSE(PresetStore::parseMetaJson(R"({"tags": ["a")", &later));
}

TEST_CASE("decode takes a null out-pointer for the half it is not asked for (backlog KA)") {
	const PresetMeta meta { { "dark" }, 1'800'000'000 };
	const std::vector<uint8_t> payload { 9, 8, 7 };
	const std::vector<uint8_t> file = PresetStore::encode(meta, payload);
	// ONLY THE TAGS: the meta comes back whole and nothing is written through the null payload.
	PresetMeta back;
	REQUIRE(PresetStore::decode(file, &back, nullptr));
	CHECK(back == meta);
	// ONLY THE SOUND, from a file with a header, one without, and one whose metadata does not parse.
	std::vector<uint8_t> body;
	REQUIRE(PresetStore::decode(file, nullptr, &body));
	CHECK(body == payload);
	body.clear();
	REQUIRE(PresetStore::decode(payload, nullptr, &body));
	CHECK(body == payload);
	std::vector<uint8_t> badMeta = file;
	badMeta[9] = 'x'; // the JSON's opening brace
	body.clear();
	REQUIRE(PresetStore::decode(badMeta, nullptr, &body));
	CHECK(body == payload);
	// NEITHER: still a verdict on the header, with nothing to write.
	CHECK(PresetStore::decode(file, nullptr, nullptr));
	CHECK(PresetStore::decode(payload, nullptr, nullptr));
}

TEST_CASE("the bank is read through every category folder, hidden ones skipped, sorted") {
	ScopedPresetLibrary library("fo-walk");
	REQUIRE(PresetStore::save("B", { 1 }, "Test"));
	REQUIRE(PresetStore::save("A", { 2 }, "Test", "Pads/Soft"));
	REQUIRE(PresetStore::save("C", { 3 }, "Test", "Bass"));
	writeRaw(PresetStore::directory("Test") / ".trash" / "D.fpreset", { 4 });
	const std::vector<StoredPreset> all = PresetStore::loadAll("Test");
	REQUIRE(all.size() == 3);
	CHECK(all[0].name == "B");
	CHECK(all[0].category.empty());
	CHECK(all[1].name == "C");
	CHECK(all[1].category == "Bass");
	CHECK(all[2].name == "A");
	CHECK(all[2].category == "Pads/Soft");
	CHECK(all[2].bytes == std::vector<uint8_t> { 2 });
}

//--------------------------------------------------------------------------------------------------//
// FACTORY PRESETS ARE FILES                                                                         //
//--------------------------------------------------------------------------------------------------//

TEST_CASE("factory presets are copied in when MISSING and never over a copy the user has edited") {
	Rig rig("fo-factory");
	const std::filesystem::path init = PresetStore::pathFor("Init", "Test");
	REQUIRE(std::filesystem::exists(init));
	REQUIRE(rig.browser.count() == 1);
	CHECK(rig.browser.entry(0).name == "Init");
	CHECK_FALSE(rig.browser.isReadOnly(0)); // a file: editable
	CHECK(rig.browser.entry(0).data.bytes == std::vector<uint8_t> { 1 });

	// THE USER EDITS IT (overwrite with the current sound, and a tag)...
	rig.payload = 55;
	REQUIRE(rig.browser.overwriteCurrent(0));
	REQUIRE(rig.browser.editEntry(0, "Init", { "mine" }));
	// ...AND THE NEXT OPEN LEAVES IT ALONE.
	rig.browser.loadBankFromDisk();
	REQUIRE(rig.browser.count() == 1);
	CHECK(rig.browser.entry(0).data.bytes == std::vector<uint8_t> { 55 });
	CHECK(rig.browser.entry(0).meta.tags == std::vector<std::string> { "mine" });

	// DELETED, IT IS COPIED BACK ON THE NEXT OPEN — "when missing" is the rule as designed.
	REQUIRE(PresetStore::erase(init));
	rig.browser.loadBankFromDisk();
	REQUIRE(rig.browser.count() == 1);
	CHECK(rig.browser.entry(0).data.bytes == std::vector<uint8_t> { 1 });
}

TEST_CASE("with no preset root, the factory preset is still offered — from memory, read-only") {
	EnvVar root("FEATHERS_PRESET_ROOT", nullptr);
	EnvVar home("HOME", nullptr);
	PresetBrowser browser(std::vector<std::string> { "Init" }, std::vector<PresetData> { PresetData { { 1 } } }, "Test",
	                      PresetBrowser::kHeaderStyle);
	browser.loadBankFromDisk();
	REQUIRE(browser.count() == 1);
	CHECK(browser.isReadOnly(0));
	CHECK_FALSE(browser.deleteEntry(0));
}

//--------------------------------------------------------------------------------------------------//
// THE FACTORY PRESETS THE PLUGIN CARRIES — presets/factory, embedded by cmake/EmbedPresets.cmake      //
//--------------------------------------------------------------------------------------------------//

namespace feathers {
// The same generator run over tests/fixtures/factory-presets (tests/CMakeLists.txt).
std::span<const FactoryPresetFile> testFactoryPresetFiles();
} // namespace feathers

namespace {

using FileSet = std::vector<std::pair<std::string, std::vector<uint8_t>>>;

/// The folder as the embedding is specified to see it, restated here rather than asked of the generator:
/// every `.fpreset`, no hidden file or folder, '/'-separated paths, sorted.
FileSet filesUnder(const std::filesystem::path& dir) {
	FileSet out;
	std::error_code ec;
	if (!std::filesystem::is_directory(dir, ec)) {
		return out;
	}
	for (const auto& e : std::filesystem::recursive_directory_iterator(dir)) {
		if (!e.is_regular_file() || e.path().extension() != ".fpreset") {
			continue;
		}
		const std::filesystem::path rel = std::filesystem::relative(e.path(), dir);
		bool hiddenPart = false;
		for (const auto& part : rel) {
			hiddenPart = hiddenPart || part.string().starts_with(".");
		}
		if (hiddenPart) {
			continue;
		}
		std::ifstream in(e.path(), std::ios::binary);
		out.emplace_back(rel.generic_string(),
		                 std::vector<uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()));
	}
	std::sort(out.begin(), out.end());
	return out;
}

FileSet embedded(std::span<const FactoryPresetFile> files) {
	FileSet out;
	for (const FactoryPresetFile& f : files) {
		out.emplace_back(std::string(f.path), std::vector<uint8_t>(f.bytes.begin(), f.bytes.end()));
	}
	return out;
}

std::vector<uint8_t> fileBytes(const std::filesystem::path& file) {
	std::ifstream in(file, std::ios::binary);
	return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::vector<uint8_t> bytesOf(const std::string& s) { return std::vector<uint8_t>(s.begin(), s.end()); }

} // namespace

TEST_CASE("the embedded factory presets are exactly the .fpreset files under their folder, by path and bytes") {
	// THE PLUGIN'S OWN SET against presets/factory — the ship check.
	CHECK(embedded(factoryPresetFiles()) == filesUnder(FEATHERS_FACTORY_PRESET_DIR));
	// AND THE SAME GENERATOR over a fixture folder that holds something, so the check above is not vacuous
	// while the real folder is empty: a category folder, a device bank, a space, a quote and a backslash in
	// the path's bytes, bytes past 0x7f — and a hidden file and a .txt that must NOT be embedded.
	const FileSet fixture = filesUnder(FEATHERS_TEST_FACTORY_PRESET_DIR);
	CHECK(embedded(testFactoryPresetFiles()) == fixture);
	REQUIRE(fixture.size() == 3);
	CHECK(fixture[0].first == "Devices/SKLITTER/Stutters/Tape Chop.fpreset");
	CHECK(fixture[1].first == "Feathers/Bright.fpreset");
	CHECK(fixture[2].first == "Feathers/Pads/Glass.fpreset");
	CHECK(fixture[1].second == bytesOf("fixture payload: Bright\n"));
}

TEST_CASE("first open on an empty preset root installs every factory file of the bank, byte for byte") {
	const ScopedPresetLibrary library("factory-install");
	const std::span<const FactoryPresetFile> files = testFactoryPresetFiles();
	REQUIRE(files.size() == 3);

	PresetBrowser synth(std::vector<std::string> { "Init" }, std::vector<PresetData> { PresetData { { 1 } } },
	                    kPluginBank, PresetBrowser::kHeaderStyle);
	synth.factoryFiles = files;
	synth.loadBankFromDisk();
	for (const FactoryPresetFile& f : files) {
		const bool mine = f.path.starts_with("Feathers/");
		CAPTURE(f.path);
		CHECK(std::filesystem::exists(library.path() / std::string(f.path)) == mine);
		if (mine) {
			CHECK(fileBytes(library.path() / std::string(f.path)) == std::vector<uint8_t>(f.bytes.begin(), f.bytes.end()));
		}
	}
	// LISTED, and the computed Init beside them exactly as before.
	REQUIRE(synth.count() == 3);
	const int init = synth.indexOf("Init");
	REQUIRE(init >= 0);
	CHECK(synth.entry(init).data.bytes == std::vector<uint8_t> { 1 });
	const int glass = synth.indexOf("Glass", "Pads");
	REQUIRE(glass >= 0);
	CHECK(synth.entry(glass).data.bytes == bytesOf("fixture payload: Glass — a bare payload, no FPRE header; bytes past 0x7f: é ü\n"));
	CHECK(synth.indexOf("Bright") >= 0);

	// A DEVICE'S BANK TAKES ITS OWN, under the folder its tag names.
	PresetBrowser device(std::vector<std::string> { "Init" }, std::vector<PresetData> { PresetData { { 2 } } },
	                     "Devices/SKLITTER", PresetBrowser::kPanelStyle);
	device.factoryFiles = files;
	device.loadBankFromDisk();
	const std::filesystem::path chop = library.path() / "Devices" / "SKLITTER" / "Stutters" / "Tape Chop.fpreset";
	REQUIRE(std::filesystem::exists(chop));
	CHECK(fileBytes(chop) == bytesOf("fixture payload: Tape Chop \"quoted\" \\ backslash\n"));
	CHECK(device.indexOf("Tape Chop", "Stutters") >= 0);
	CHECK(device.indexOf("Init") >= 0);
}

TEST_CASE("an existing edited factory file is never overwritten, and a missing one beside it still installs") {
	const ScopedPresetLibrary library("factory-keep");
	const std::filesystem::path bright = library.path() / "Feathers" / "Bright.fpreset";
	std::filesystem::create_directories(bright.parent_path());
	{
		std::ofstream out(bright, std::ios::binary);
		out << "the user's own Bright";
	}
	CHECK(PresetStore::installFactoryFiles(kPluginBank, testFactoryPresetFiles()) == 1); // Glass only
	CHECK(fileBytes(bright) == bytesOf("the user's own Bright"));
	CHECK(fileBytes(library.path() / "Feathers" / "Pads" / "Glass.fpreset")
	      == bytesOf("fixture payload: Glass — a bare payload, no FPRE header; bytes past 0x7f: é ü\n"));
	// AND AGAIN, THROUGH THE BROWSER: nothing is written, the user's bytes are what it lists.
	CHECK(PresetStore::installFactoryFiles(kPluginBank, testFactoryPresetFiles()) == 0);
	PresetBrowser synth(std::vector<std::string> { "Init" }, std::vector<PresetData> { PresetData { { 1 } } },
	                    kPluginBank, PresetBrowser::kHeaderStyle);
	synth.factoryFiles = testFactoryPresetFiles();
	synth.loadBankFromDisk();
	const int at = synth.indexOf("Bright");
	REQUIRE(at >= 0);
	CHECK(synth.entry(at).data.bytes == bytesOf("the user's own Bright"));
}

TEST_CASE("Init still loads beside the factory files, and an authored Feathers/Init.fpreset replaces the computed one") {
	{
		// NO FACTORY FILES AT ALL (the shipped folder today): the computed Init is written and listed.
		const ScopedPresetLibrary library("factory-init-none");
		PresetBrowser synth(std::vector<std::string> { "Init" }, std::vector<PresetData> { PresetData { { 1, 2 } } },
		                    kPluginBank, PresetBrowser::kHeaderStyle);
		synth.factoryFiles = {};
		synth.loadBankFromDisk();
		REQUIRE(synth.count() == 1);
		CHECK(synth.entry(0).name == "Init");
		CHECK(synth.entry(0).data.bytes == std::vector<uint8_t> { 1, 2 });
		CHECK(std::filesystem::exists(library.path() / "Feathers" / "Init.fpreset"));
	}
	{
		// AN AUTHORED Init SHIPS AS A FILE: it is installed first, so the computed one finds it there.
		const ScopedPresetLibrary library("factory-init-authored");
		static const uint8_t kAuthored[] = { 'a', 'u', 't', 'h' };
		const FactoryPresetFile files[] = { { "Feathers/Init.fpreset", std::span<const uint8_t> { kAuthored } } };
		PresetBrowser synth(std::vector<std::string> { "Init" }, std::vector<PresetData> { PresetData { { 1, 2 } } },
		                    kPluginBank, PresetBrowser::kHeaderStyle);
		synth.factoryFiles = files;
		synth.loadBankFromDisk();
		REQUIRE(synth.count() == 1);
		CHECK(synth.entry(0).data.bytes == bytesOf("auth"));
		CHECK(fileBytes(library.path() / "Feathers" / "Init.fpreset") == bytesOf("auth"));
	}
}

TEST_CASE("a factory file whose path would leave the root is refused") {
	const ScopedPresetLibrary library("factory-escape");
	static const uint8_t kBytes[] = { 'x' };
	const FactoryPresetFile bad[] = {
		{ "Feathers/../escaped.fpreset", std::span<const uint8_t> { kBytes } },
		{ "Feathers/./dot.fpreset", std::span<const uint8_t> { kBytes } },
	};
	CHECK(PresetStore::installFactoryFiles(kPluginBank, bad) == 0);
	CHECK_FALSE(std::filesystem::exists(library.path() / "escaped.fpreset"));
}

//--------------------------------------------------------------------------------------------------//
// THE SHARED STEPPER                                                                                //
//--------------------------------------------------------------------------------------------------//

TEST_CASE("ItemStepper steps over what will not load, wraps only if the source does, and lists sections and actions") {
	MockSource src;
	src.labels = { "a", "b", "c", "d" };
	src.loadable = { true, false, true, true };
	ItemStepper stepper(src, { 26.0f, 160.0f, 24.0f });
	stepper.setBounds(0.0f, 0.0f, stepper.totalWidth(), 26.0f);

	stepper.step(1);
	CHECK(src.current == 2); // b is skipped
	stepper.step(1);
	CHECK(src.current == 3);
	stepper.step(1);
	CHECK(src.current == 3); // no wrap: the end is the end
	src.wrap = true;
	stepper.step(1);
	CHECK(src.current == 0); // round the end
	stepper.step(-1);
	CHECK(src.current == 3); // and back the other way
	stepper.step(-1);
	CHECK(src.current == 2);
	stepper.step(-1);
	CHECK(src.current == 0); // b skipped going left too

	// NOTHING LOADS BUT THE CURRENT ONE: no step, no spin.
	src.loadable = { true, false, false, false };
	stepper.step(1);
	CHECK(src.current == 0);

	// THE LIST: top-level items first, the current one marked; then sections; then the actions.
	src.loadable.clear();
	src.sections = { {}, { "x" }, { "x", "y" }, {} };
	const std::vector<ItemStepper::Row> rows = stepper.listRows();
	REQUIRE(rows.size() == 7);
	CHECK(rows[0].label == "a");
	CHECK(rows[0].current);
	CHECK(rows[1].label == "d");
	CHECK(rows[2].section == "x");
	CHECK(rows[2].label == "b");
	CHECK(rows[3].section == "x");
	CHECK(rows[3].label == "c");
	CHECK(rows[4].section == "y");
	CHECK(rows[4].label == "c"); // an item under every section it names
	CHECK(rows[5].id == ItemStepper::kActionBase + 3);
	CHECK_FALSE(rows[6].enabled);
	stepper.choose(rows[5].id);
	CHECK(src.acted == std::vector<int> { 3 });
	stepper.choose(1);
	CHECK(src.current == 1);

	// OFF WHILE SOMETHING ELSE OWNS THE FACE.
	stepper.setInteractive(false);
	stepper.step(1);
	CHECK(src.current == 1);
}

TEST_CASE("ItemStepper: a load that FAILS is passed over like one known not to load — A, B fails, lands on C") {
	MockSource src;
	src.labels = { "A", "B", "C", "D" };
	src.failsOnLoad = { false, true, false, false };
	ItemStepper stepper(src, { 26.0f, 160.0f, 24.0f });
	stepper.setBounds(0.0f, 0.0f, stepper.totalWidth(), 26.0f);

	stepper.step(1);
	CHECK(src.tried == std::vector<int> { 1, 2 }); // tried B, it failed, went on to C
	CHECK(src.current == 2);
	CHECK(stepper.failed(1));
	CHECK_FALSE(stepper.failed(2));
	// REMEMBERED: going back does not try B again, and the list shows it dimmed and unpickable.
	src.tried.clear();
	stepper.step(-1);
	CHECK(src.tried == std::vector<int> { 0 });
	CHECK(src.current == 0);
	const std::vector<ItemStepper::Row> rows = stepper.listRows();
	CHECK_FALSE(rows[1].enabled);
	CHECK(rows[2].enabled);

	// STILL ONE LAP: with every other item failing too, a wrapping step tries each once and stays put.
	src.wrap = true;
	src.failsOnLoad = { false, true, true, true };
	stepper.forgetFailures();
	src.tried.clear();
	stepper.step(1);
	CHECK(src.tried == std::vector<int> { 1, 2, 3 });
	CHECK(src.current == 0);

	// THE LIST CHANGING (the count moves) forgets the failures on its own.
	src.labels.push_back("E");
	src.failsOnLoad.clear();
	CHECK_FALSE(stepper.failed(1));
}

TEST_CASE("a preset that fails to load does not become current, and a step moves on past it") {
	Rig rig("fo-failload");
	rig.save("A");
	rig.payload = 0; // "does not load"
	rig.save("B");
	rig.payload = 7;
	rig.save("C");
	REQUIRE(rig.browser.indexOf("B") == rig.browser.indexOf("A") + 1); // sorted: A, B, C, Init
	REQUIRE(rig.browser.indexOf("C") == rig.browser.indexOf("B") + 1);
	rig.browser.selectIndex(rig.browser.indexOf("A"));
	rig.applied.clear();
	CHECK_FALSE(rig.browser.selectIndex(rig.browser.indexOf("B")));
	CHECK(rig.browser.currentName() == "A"); // the face does not name a sound that is not playing
	rig.browser.step(1);
	CHECK(rig.browser.currentName() == "C");
	CHECK(rig.applied.size() == 1);
}

TEST_CASE("the preset browser's own click steps it, and it does not wrap") {
	Rig rig("fo-click");
	rig.save("Warm Pad");
	rig.browser.selectIndex(0);
	rig.applied.clear();
	visage::MouseEvent press {};
	press.position = { PresetBrowser::kArrowWidth + PresetBrowser::kNameWidth + PresetBrowser::kArrowWidth * 0.5f,
		               PresetBrowser::kHeight * 0.5f };
	rig.browser.onMouseDown().callback(press);
	CHECK(rig.browser.currentName() == "Warm Pad");
	CHECK(rig.applied.size() == 1); // a step LOADS
	rig.browser.onMouseDown().callback(press);
	CHECK(rig.browser.currentName() == "Warm Pad"); // at the end: Serum does not wrap
	press.position = { PresetBrowser::kArrowWidth * 0.5f, PresetBrowser::kHeight * 0.5f };
	rig.browser.onMouseDown().callback(press);
	CHECK(rig.browser.currentName() == "Init");
	// PICKING THE ONE YOU ARE ON IS A LOAD — you change something and pick it again to get it back.
	const size_t before = rig.applied.size();
	rig.browser.selectIndex(rig.browser.currentIndex());
	CHECK(rig.applied.size() == before + 1);
	rig.browser.selectIndex(99);
	CHECK(rig.applied.size() == before + 1);
}

//--------------------------------------------------------------------------------------------------//
// SAVE ASKS FOR A NAME AND TAGS                                                                     //
//--------------------------------------------------------------------------------------------------//

TEST_CASE("the menu's Save OPENS THE PROMPT and writes nothing until Enter — then the typed name, category and tags") {
	Rig rig("fo-save");
	rig.browser.chooseFromMenu(PresetBrowser::kMenuSave);
	CHECK(rig.browser.prompt() == PresetBrowser::Prompt::Naming);
	CHECK(rig.browser.promptFrameForTest()->parent() != nullptr); // shown
	CHECK(rig.browser.promptTextForTest() == "User 1");
	CHECK(rig.captures == 0);
	CHECK(rig.disk().size() == 1); // Init only

	rig.browser.typeForTest("Stutters/Tape Chop");
	rig.browser.typeTagsForTest("Dark, tape ,dark");
	rig.browser.submitPrompt();
	CHECK(rig.browser.prompt() == PresetBrowser::Prompt::Closed);
	CHECK(rig.browser.promptFrameForTest()->parent() == nullptr);
	CHECK(rig.captures == 1);
	CHECK(rig.browser.currentName() == "Tape Chop");
	CHECK(rig.applied.empty()); // saving selects without re-applying

	const std::vector<StoredPreset> disk = rig.disk();
	REQUIRE(disk.size() == 2);
	const StoredPreset& saved = disk[1];
	CHECK(saved.name == "Tape Chop");
	CHECK(saved.category == "Stutters");
	CHECK(saved.meta.tags == std::vector<std::string> { "dark", "tape" });
	CHECK(saved.meta.saved == 1'800'000'000);
	CHECK(saved.bytes == std::vector<uint8_t> { 7 });
	CHECK(saved.path == PresetStore::directory("Test") / "Stutters" / "Tape Chop.fpreset");
}

TEST_CASE("the preset prompt types Hello World with Shift and Space by the key road, and Enter or Esc on its panel "
          "saves or cancels") {
	// no capitals or spaces in preset names, keys reaching the DAW, and Enter should save
	// "whenever preset dialog is open even if cursor not in a text box, ie, after clicking a tag chip". The
	// road here is visage's: the key's TEXT to the focused text entry, then the KEY from the focus up through
	// keystroke-taking parents; a key no frame uses is what visage forwards to the host. The X half (the grab,
	// a host that takes the focus back, a forwarded key without Shift) is tools/key-probe's, ctest key-probe-x11.
	Rig rig("tk-hello");
	rig.browser.chooseFromMenu(PresetBrowser::kMenuSave);
	REQUIRE(rig.browser.prompt() == PresetBrowser::Prompt::Naming);
	visage::Frame* name = rig.browser.nameFieldForTest();
	REQUIRE(name != nullptr);
	struct Typed {
		const char* text;
		visage::KeyCode code;
		int modifiers;
	};
	const Typed keys[] = {
		{ "H", visage::KeyCode::H, visage::kModifierShift }, { "e", visage::KeyCode::E, 0 },
		{ "l", visage::KeyCode::L, 0 }, { "l", visage::KeyCode::L, 0 }, { "o", visage::KeyCode::O, 0 },
		{ " ", visage::KeyCode::Space, 0 }, { "W", visage::KeyCode::W, visage::kModifierShift },
		{ "o", visage::KeyCode::O, 0 }, { "r", visage::KeyCode::R, 0 }, { "l", visage::KeyCode::L, 0 },
		{ "d", visage::KeyCode::D, 0 },
	};
	for (const Typed& k : keys) {
		REQUIRE(name->receivesTextInput());
		name->processTextInput(k.text);
		CHECK(keyFromFocus(name, visage::KeyEvent(k.code, k.modifiers, true)));
		CHECK(keyUpFromFocus(name, visage::KeyEvent(k.code, k.modifiers, false)));
	}
	CHECK(rig.browser.promptTextForTest() == "Hello World");

	// THE PANEL HOLDS THE FOCUS (a chip or the gap between rows was clicked): every key is used — none goes to
	// the host — its text is taken (visage forwards an unnamed key unless a text entry is active), and Enter saves.
	visage::Frame* panel = rig.browser.promptFrameForTest();
	REQUIRE(panel != nullptr);
	CHECK(panel->receivesTextInput());
	CHECK(keyFromFocus(panel, visage::KeyEvent(visage::KeyCode::Space, 0, true)));
	CHECK(keyFromFocus(panel, visage::KeyEvent(visage::KeyCode::Return, 0, true)));
	CHECK(rig.browser.prompt() == PresetBrowser::Prompt::Closed);
	const std::vector<StoredPreset> disk = rig.disk();
	CHECK(std::any_of(disk.begin(), disk.end(), [](const StoredPreset& p) { return p.name == "Hello World"; }));

	// AND ESC FROM THE PANEL CANCELS, WRITING NOTHING.
	const size_t before = rig.disk().size();
	rig.browser.chooseFromMenu(PresetBrowser::kMenuSave);
	rig.browser.typeForTest("Never Saved");
	CHECK(keyFromFocus(panel, visage::KeyEvent(visage::KeyCode::Escape, 0, true)));
	CHECK(rig.browser.prompt() == PresetBrowser::Prompt::Closed);
	CHECK(rig.disk().size() == before);
}

TEST_CASE("Esc closes the prompt and writes NOTHING") {
	Rig rig("fo-esc");
	rig.browser.chooseFromMenu(PresetBrowser::kMenuSave);
	rig.browser.typeForTest("Never Saved");
	rig.browser.cancelPrompt();
	CHECK(rig.browser.prompt() == PresetBrowser::Prompt::Closed);
	CHECK(rig.captures == 0);
	CHECK(rig.disk().size() == 1);
	CHECK(rig.browser.count() == 1);
}

TEST_CASE("the prompt takes the keyboard while it is up and gives it back on cancel, save, removal and "
          "destruction") {
	// WHILE ITS NAME AND TAGS ARE TYPED INTO, a key that reached the DAW started its transport (found
	// 2026-09-26). The claim is the X focus on our own window; at rest the editor holds none.
	ClaimCounter claims;
	const ui::ScopedKeyboardClaimBackend scoped(claims);
	{
		Rig rig("fo-claim");
		CHECK(claims.takes == 0);
		rig.browser.chooseFromMenu(PresetBrowser::kMenuSave);
		CHECK(claims.outstanding() == 1);
		rig.browser.cancelPrompt();
		CHECK(claims.outstanding() == 0);

		rig.browser.chooseFromMenu(PresetBrowser::kMenuSave);
		rig.browser.typeForTest("Kept");
		rig.browser.submitPrompt();
		REQUIRE(rig.browser.prompt() == PresetBrowser::Prompt::Closed);
		CHECK(claims.takes == 2);
		CHECK(claims.outstanding() == 0);

		// TAKEN OUT OF THE TREE with the prompt up — the panel dismissed under it.
		visage::Frame panel;
		panel.addChild(&rig.browser);
		rig.browser.chooseFromMenu(PresetBrowser::kMenuSave);
		CHECK(claims.outstanding() == 1);
		panel.removeChild(&rig.browser);
		CHECK(rig.browser.prompt() == PresetBrowser::Prompt::Closed);
		CHECK(claims.outstanding() == 0);
	}
	// AND DESTROYED with it up.
	{
		auto rig = std::make_unique<Rig>("fo-claim-gone");
		rig->browser.chooseFromMenu(PresetBrowser::kMenuSave);
		CHECK(claims.outstanding() == 1);
		rig.reset();
	}
	CHECK(claims.takes == 4);
	CHECK(claims.outstanding() == 0);
}

/// THE WINDOW'S POINTER, AS VISAGE DRIVES IT (backlog MF). `set_cursor_style` is the function a frame's
/// `setCursorStyle` calls, and on X11 it defines the cursor of the WHOLE window, which keeps that cursor
/// until it is told another one. `remove_from_hierarchy` restates `WindowEventHandler::giveUpFocus`: a
/// hovered frame leaving the tree is forgotten with NO `mouseExit`, so nothing it set is put back.
struct CursorWindow {
	visage::FrameEventHandler handler;
	visage::MouseCursor last = visage::MouseCursor::Arrow;
	int sets = 0;
	visage::Frame* hovered = nullptr;

	CursorWindow() {
		handler.set_cursor_style = [this](visage::MouseCursor style) {
			last = style;
			++sets;
		};
		handler.remove_from_hierarchy = [this](visage::Frame* frame) {
			if (hovered == frame) {
				hovered = nullptr;
			}
		};
	}

	/// The pointer moves onto `frame`: the exit to the last one and the enter to this one, as
	/// `WindowEventHandler::handleMouseMove` delivers them.
	void hover(visage::Frame* frame) {
		if (hovered == frame) {
			return;
		}
		if (hovered != nullptr) {
			hovered->processMouseExit(visage::MouseEvent {});
		}
		hovered = frame;
		if (frame != nullptr) {
			frame->processMouseEnter(visage::MouseEvent {});
		}
	}
};

TEST_CASE("MF: a prompt closed under the pointer gives back the arrow — on Enter, Esc, removal and destruction") {
	// THE DEFECT: the text-selection cursor often appeared while using the plugin with no text being
	// selected. The prompt's fields are the only frames in the editor that set the
	// I-beam, and the prompt leaves the tree (it is not hidden) on every one of these roads.
	using visage::MouseCursor;
	CursorWindow window;
	visage::Frame panel;
	panel.setBounds(0.0f, 0.0f, 600.0f, 400.0f);
	panel.setEventHandler(&window.handler);
	{
		Rig rig("fo-ibeam");
		panel.addChild(&rig.browser);

		// THE VENUE CAN SHOW AN I-BEAM: hovered and left up, the field holds it; moved off, it lets go.
		rig.browser.chooseFromMenu(PresetBrowser::kMenuSave);
		window.hover(rig.browser.nameFieldForTest());
		CHECK(window.last == MouseCursor::IBeam);
		window.hover(rig.browser.tagsFieldForTest());
		CHECK(window.last == MouseCursor::IBeam);
		window.hover(&panel);
		CHECK(window.last == MouseCursor::Arrow);

		// ENTER saves with the pointer still on the name field.
		window.hover(rig.browser.nameFieldForTest());
		REQUIRE(window.last == MouseCursor::IBeam);
		rig.browser.typeForTest("Under The Pointer");
		rig.browser.submitPrompt();
		REQUIRE(rig.browser.prompt() == PresetBrowser::Prompt::Closed);
		CHECK(window.last == MouseCursor::Arrow);

		// ESC, from the tags field.
		rig.browser.chooseFromMenu(PresetBrowser::kMenuSave);
		window.hover(rig.browser.tagsFieldForTest());
		REQUIRE(window.last == MouseCursor::IBeam);
		rig.browser.cancelPrompt();
		CHECK(window.last == MouseCursor::Arrow);

		// A CLOSE WITH THE POINTER ELSEWHERE tells the window nothing: the arrow is not the prompt's to set.
		// Over a field and OFF it again first, so the field has had its exit and knows it is not hovered.
		rig.browser.chooseFromMenu(PresetBrowser::kMenuSave);
		window.hover(rig.browser.nameFieldForTest());
		window.hover(&panel);
		REQUIRE(window.last == MouseCursor::Arrow);
		const int before = window.sets;
		rig.browser.cancelPrompt();
		CHECK(window.sets == before);

		// TAKEN OUT OF THE TREE with the prompt up and hovered: the prompt lives in the panel, which stays.
		rig.browser.chooseFromMenu(PresetBrowser::kMenuSave);
		window.hover(rig.browser.nameFieldForTest());
		REQUIRE(window.last == MouseCursor::IBeam);
		panel.removeChild(&rig.browser);
		REQUIRE(rig.browser.prompt() == PresetBrowser::Prompt::Closed);
		CHECK(window.last == MouseCursor::Arrow);

		// DESTROYED with it up and hovered.
		panel.addChild(&rig.browser);
		rig.browser.chooseFromMenu(PresetBrowser::kMenuSave);
		window.hover(rig.browser.tagsFieldForTest());
		REQUIRE(window.last == MouseCursor::IBeam);
	}
	CHECK(window.last == MouseCursor::Arrow);
	CHECK(window.hovered == nullptr);
}

TEST_CASE("a name already in the bank ASKS before replacing: replace overwrites in place, the other answer goes back") {
	Rig rig("fo-collide");
	rig.save("Crunch", "old");
	rig.payload = 42;
	rig.browser.chooseFromMenu(PresetBrowser::kMenuSave);
	rig.browser.typeForTest("Crunch ");
	rig.browser.typeTagsForTest("new");
	rig.browser.submitPrompt();
	CHECK(rig.browser.prompt() == PresetBrowser::Prompt::ConfirmReplace);
	CHECK(rig.captures == 1); // asked, nothing done

	rig.browser.resolveReplace(false);
	CHECK(rig.browser.prompt() == PresetBrowser::Prompt::Naming);
	CHECK(rig.browser.promptTextForTest() == "Crunch ");
	CHECK(rig.captures == 1);

	rig.browser.submitPrompt();
	REQUIRE(rig.browser.prompt() == PresetBrowser::Prompt::ConfirmReplace);
	rig.browser.resolveReplace(true);
	CHECK(rig.browser.prompt() == PresetBrowser::Prompt::Closed);
	CHECK(rig.captures == 2);
	const std::vector<StoredPreset> disk = rig.disk();
	REQUIRE(disk.size() == 2);
	CHECK(disk[0].name == "Crunch");
	CHECK(disk[0].bytes == std::vector<uint8_t> { 42 });
	CHECK(disk[0].meta.tags == std::vector<std::string> { "new" });
}

TEST_CASE("an empty name is refused with the prompt left open, and the fields take EVERY key") {
	Rig rig("fo-refuse");
	rig.browser.chooseFromMenu(PresetBrowser::kMenuSave);
	rig.browser.typeForTest("  /// ");
	rig.browser.submitPrompt();
	CHECK(rig.browser.prompt() == PresetBrowser::Prompt::Naming);
	CHECK_FALSE(rig.browser.notice().empty());
	CHECK(rig.captures == 0);
	// visage bubbles a key to the parents exactly when processKeyPress returns false; +/- would step the
	// UI scale and Esc would close a panel. Both fields report every key as used, Tab included.
	for (visage::Frame* field : { rig.browser.nameFieldForTest(), rig.browser.tagsFieldForTest() }) {
		for (const visage::KeyCode key : { visage::KeyCode::Equals, visage::KeyCode::Minus, visage::KeyCode::A,
		                                   visage::KeyCode::Tab }) {
			CHECK(field->processKeyPress(visage::KeyEvent(key, 0, true)));
		}
	}
	// AND THE STEPPERS DO NOTHING WHILE IT IS UP.
	rig.browser.step(1);
	CHECK(rig.applied.empty());
	CHECK(rig.browser.nameFieldForTest()->processKeyPress(visage::KeyEvent(visage::KeyCode::Escape, 0, true)));
	CHECK(rig.browser.prompt() == PresetBrowser::Prompt::Closed);
	CHECK(rig.disk().size() == 1);
}

//--------------------------------------------------------------------------------------------------//
// RENAME, DELETE, SHOW IN FILE MANAGER                                                              //
//--------------------------------------------------------------------------------------------------//

TEST_CASE("Rename / tags moves the FILE, keeps its sound, retags it, and never overwrites another") {
	Rig rig("fo-rename");
	rig.save("Glass", "bright");
	rig.save("Other");
	const int glass = rig.browser.indexOf("Glass");
	REQUIRE(glass >= 0);
	const std::filesystem::path before = rig.browser.entry(static_cast<size_t>(glass)).path;
	rig.browser.selectIndex(glass);
	rig.browser.chooseFromMenu(PresetBrowser::kMenuEdit);
	REQUIRE(rig.browser.prompt() == PresetBrowser::Prompt::Naming);
	CHECK(rig.browser.promptKind() == PresetBrowser::PromptKind::Edit);
	CHECK(rig.browser.promptTextForTest() == "Glass");
	CHECK(rig.browser.tagsTextForTest() == "bright");

	// ONTO ANOTHER'S NAME: refused, the field stays, nothing moved.
	rig.browser.typeForTest("Other");
	rig.browser.submitPrompt();
	CHECK(rig.browser.prompt() == PresetBrowser::Prompt::Naming);
	CHECK(std::filesystem::exists(before));

	rig.browser.typeForTest("Pads/Crystal");
	rig.browser.typeTagsForTest("bright, airy");
	rig.browser.submitPrompt();
	CHECK(rig.browser.prompt() == PresetBrowser::Prompt::Closed);
	CHECK_FALSE(std::filesystem::exists(before));
	const std::filesystem::path after = PresetStore::directory("Test") / "Pads" / "Crystal.fpreset";
	REQUIRE(std::filesystem::exists(after));
	CHECK(rig.browser.currentName() == "Crystal"); // the selection followed it
	CHECK(rig.captures == 2); // an edit is not a save of the current sound
	const std::vector<StoredPreset> disk = rig.disk();
	const auto it = std::find_if(disk.begin(), disk.end(), [](const StoredPreset& p) { return p.name == "Crystal"; });
	REQUIRE(it != disk.end());
	CHECK(it->category == "Pads");
	CHECK(it->meta.tags == std::vector<std::string> { "bright", "airy" });
	CHECK(it->bytes == std::vector<uint8_t> { 7 });
}

TEST_CASE("Delete ASKS, and only a confirmed delete removes the file") {
	Rig rig("fo-delete");
	rig.save("Doomed");
	const int at = rig.browser.indexOf("Doomed");
	rig.browser.selectIndex(at);
	const std::filesystem::path file = rig.browser.entry(static_cast<size_t>(at)).path;
	rig.browser.chooseFromMenu(PresetBrowser::kMenuDelete);
	CHECK(rig.browser.pendingDelete() == at);
	rig.browser.resolveDelete(false);
	CHECK(std::filesystem::exists(file));
	CHECK(rig.browser.count() == 2);

	rig.browser.chooseFromMenu(PresetBrowser::kMenuDelete);
	rig.browser.resolveDelete(true);
	CHECK_FALSE(std::filesystem::exists(file));
	CHECK(rig.browser.count() == 1);
	CHECK(rig.browser.indexOf("Doomed") < 0);
	CHECK(rig.browser.currentIndex() == -1); // nothing current; nothing was applied
	CHECK(rig.disk().size() == 1);
}

//--------------------------------------------------------------------------------------------------//
// TAGS                                                                                              //
//--------------------------------------------------------------------------------------------------//

TEST_CASE("browse by tag lists a preset under EVERY tag it carries; by folder, under its category") {
	Rig rig("fo-tagview");
	rig.save("Bass/Sub", "dark, low");
	rig.save("Air", "bright");
	const auto rowsUnder = [&](const std::string& section, const std::vector<std::string>& inside = {}) {
		std::vector<std::string> out;
		for (const ItemStepper::Row& r : rig.browser.listRows()) {
			if (r.id < ItemStepper::kActionBase && r.section == section && r.subsections == inside) {
				out.push_back(r.label);
			}
		}
		return out;
	};
	CHECK(rowsUnder("Bass") == std::vector<std::string> { "Sub" });
	CHECK(rowsUnder("tag", { "dark (1)" }).empty());
	rig.browser.chooseFromMenu(PresetBrowser::kMenuByTag);
	REQUIRE(rig.browser.view() == PresetBrowser::View::Tags);
	// BARE TAGS ARE IN THE CATCH-ALL NAMESPACE: tag ▸ value (count) ▸ presets.
	CHECK(rowsUnder("tag", { "dark (1)" }) == std::vector<std::string> { "Sub" });
	CHECK(rowsUnder("tag", { "low (1)" }) == std::vector<std::string> { "Sub" });
	CHECK(rowsUnder("tag", { "bright (1)" }) == std::vector<std::string> { "Air" });
	CHECK(rowsUnder("Bass").empty());
	CHECK(rowsUnder("") == std::vector<std::string> { "Init" }); // untagged at the top
	rig.browser.chooseFromMenu(PresetBrowser::kMenuByFolder);
	CHECK(rig.browser.view() == PresetBrowser::View::Folders);
}

TEST_CASE("choosing a view REOPENS the list in that view, one event-loop turn later — not \"nothing happens\"") {
	// THE DEFECT: choosing "browse by tags" appeared to do nothing. The menu closed on the click and the face
	// looked the same until the list was opened again by hand.
	Rig rig("fo-view-reopen");
	rig.save("Bass/Sub", "dark, low");
	REQUIRE(openMenus(rig.browser) == 0);
	rig.browser.chooseFromMenu(PresetBrowser::kMenuByTag);
	CHECK(rig.browser.view() == PresetBrowser::View::Tags);
	// NOT INSIDE THE CHOICE: that arrives in the closing menu's selection callback, which tears the old
	// menu down after it returns.
	CHECK(openMenus(rig.browser) == 0);
	CHECK(rig.browser.reopenPending());
	// ONE TURN OF THE LOOP LATER, A LIST IS OPEN — counted off the frame tree, where visage put it.
	REQUIRE(pumpUntil([&] { return openMenus(rig.browser) == 1; }));
	CHECK_FALSE(rig.browser.reopenPending());
	// AND IT IS THE TAG VIEW: what the reopened list was built from.
	std::vector<std::string> sections;
	for (const ItemStepper::Row& r : rig.browser.listRows()) {
		if (r.id >= 0 && r.id < ItemStepper::kActionBase && !r.section.empty()) {
			REQUIRE(r.subsections.size() == 1);
			sections.push_back(r.section + " > " + r.subsections[0]);
		}
	}
	CHECK(sections == std::vector<std::string> { "tag > dark (1)", "tag > low (1)" });
	// ONLY ONCE: the timer is one shot, so the loop turning on opens nothing more.
	pumpUntil([] { return false; });
	CHECK(openMenus(rig.browser) == 1);
	// BACK TO FOLDERS REOPENS TOO, filed by category.
	rig.browser.chooseFromMenu(PresetBrowser::kMenuByFolder);
	REQUIRE(pumpUntil([&] { return openMenus(rig.browser) == 2; }));
	CHECK(rig.browser.view() == PresetBrowser::View::Folders);
}

TEST_CASE("the list shows which view it is in: both view rows, the current one marked") {
	Rig rig("fo-view-mark");
	// THE TWO VIEW ROWS, BY ID — so each row is the view it names, whichever is current, and not a toggle
	// to "the other one".
	const auto viewRows = [&] {
		std::vector<std::string> out;
		for (const ItemStepper::Row& r : rig.browser.listRows()) {
			if (r.id == PresetBrowser::kMenuByFolder || r.id == PresetBrowser::kMenuByTag) {
				out.push_back(r.label);
			}
		}
		return out;
	};
	CHECK(viewRows() == std::vector<std::string> { "\xe2\x80\xa2 Browse by folder", "Browse by tag" });
	rig.browser.chooseFromMenu(PresetBrowser::kMenuByTag);
	CHECK(viewRows() == std::vector<std::string> { "Browse by folder", "\xe2\x80\xa2 Browse by tag" });
}

TEST_CASE("a tag view over a bank with no tags says so in a dimmed first row, instead of looking like folders") {
	Rig rig("fo-view-notags");
	rig.save("Air");
	CHECK(rig.browser.listRows().front().id != ItemStepper::kNoteId); // folders: no note
	rig.browser.chooseFromMenu(PresetBrowser::kMenuByTag);
	const std::vector<ItemStepper::Row> rows = rig.browser.listRows();
	REQUIRE_FALSE(rows.empty());
	CHECK(rows.front().id == ItemStepper::kNoteId);
	CHECK(rows.front().label == "No tags yet \xe2\x80\x94 add tags when saving");
	CHECK_FALSE(rows.front().enabled);
	CHECK(rows.front().section.empty());
	// THE ROW IS NOT A PICK: choosing it selects nothing and acts on nothing.
	const int before = rig.browser.currentIndex();
	rig.browser.chooseFromMenu(ItemStepper::kNoteId);
	CHECK(rig.browser.currentIndex() == before);
	CHECK(rig.applied.empty());
	// ONE TAG ANYWHERE IN THE BANK AND THE NOTE GOES: the view has something to file under.
	rig.save("Glass", "bright");
	CHECK(rig.browser.listRows().front().id != ItemStepper::kNoteId);
}

TEST_CASE("the view is told to its owner when a hand changes it, and not when the owner puts it back") {
	Rig rig("fo-view-told");
	std::vector<PresetBrowser::View> told;
	rig.browser.onViewChanged = [&](PresetBrowser::View v) { told.push_back(v); };
	rig.browser.setView(PresetBrowser::View::Tags);
	CHECK(told.empty());
	rig.browser.chooseFromMenu(PresetBrowser::kMenuByTag); // already there: reopens, tells nobody
	CHECK(told.empty());
	rig.browser.chooseFromMenu(PresetBrowser::kMenuByFolder);
	CHECK(told == std::vector<PresetBrowser::View> { PresetBrowser::View::Folders });
}

TEST_CASE("tag ranking is recency-weighted frequency, by value") {
	constexpr int64_t kNow = 1'800'000'000;
	constexpr int64_t kDay = 86'400;
	// THE ARITHMETIC, stated: weight = 0.5 ^ (age / 30 days).
	//   "old":   on a 90-day and a 60-day preset     -> 0.125 + 0.25 = 0.375
	//   "mid":   on two 30-day presets              -> 0.5 + 0.5    = 1.0
	//   "new", "fresh": one preset saved now         -> 1.0 each: tie "mid" on score, lose on count, and
	//                                                   tie each other on everything but the alphabet
	//   "ghost": one preset with no save time        -> 0.5 ^ 12: counted, never ahead
	const std::vector<PresetMeta> metas {
		{ { "old" }, kNow - 90 * kDay }, { { "Old " }, kNow - 60 * kDay }, { { "mid" }, kNow - 30 * kDay },
		{ { "mid" }, kNow - 30 * kDay }, { { "new", "fresh" }, kNow },     { { "ghost" }, 0 },
	};
	const std::vector<TagStat> ranked = rankTags(metas, kNow);
	REQUIRE(ranked.size() == 5);
	CHECK(ranked[0].tag == "mid");
	CHECK(ranked[0].score == doctest::Approx(1.0).epsilon(1e-9));
	CHECK(ranked[0].count == 2);
	CHECK(ranked[1].tag == "fresh");
	CHECK(ranked[1].score == doctest::Approx(1.0).epsilon(1e-9));
	CHECK(ranked[2].tag == "new");
	CHECK(ranked[3].tag == "old"); // "Old " is the same tag: one spelling
	CHECK(ranked[3].count == 2);
	CHECK(ranked[3].score == doctest::Approx(0.375).epsilon(1e-9));
	CHECK(ranked[4].tag == "ghost");
	CHECK(ranked[4].score == doctest::Approx(std::pow(0.5, 12.0)).epsilon(1e-9));

	// COMPLETIONS for a prefix, best first, none already present; SUGGESTIONS the best five not present.
	CHECK(tagCompletions(ranked, "o", {}) == std::vector<std::string> { "old" });
	CHECK(tagCompletions(ranked, "", {}).empty());
	CHECK(tagSuggestions(ranked, { "fresh" }) == std::vector<std::string> { "mid", "new", "old", "ghost" });
	CHECK(tagSuggestions(ranked, {}, 2) == std::vector<std::string> { "mid", "fresh" });
}

TEST_CASE("the prompt's chips: the bank's best tags, then completions while a tag is typed; Tab takes one") {
	Rig rig("fo-chips");
	rig.save("A", "dark, tape");
	rig.save("B", "dark, drone");
	rig.browser.chooseFromMenu(PresetBrowser::kMenuSave);
	rig.browser.typeTagsForTest(""); // the field pre-fills B's tags (backlog PP); this case is about an empty one
	// NOTHING TYPED: the best tags — "dark" is on two presets.
	CHECK(rig.browser.chips() == std::vector<std::string> { "dark", "drone", "tape" });
	// TYPING "d": the completions of "d", best first.
	rig.browser.typeTagsForTest("tape, d");
	CHECK(rig.browser.chips() == std::vector<std::string> { "dark", "drone" });
	// TAB TAKES THE FIRST.
	CHECK(rig.browser.acceptCompletion());
	CHECK(parseTagList(rig.browser.tagsTextForTest()) == std::vector<std::string> { "tape", "dark" });
	// A CHIP ADDS ITS TAG, and one already present is not offered again.
	CHECK(rig.browser.chips() == std::vector<std::string> { "drone" });
	rig.browser.pressChip(0);
	CHECK(parseTagList(rig.browser.tagsTextForTest()) == std::vector<std::string> { "tape", "dark", "drone" });
	// A NEW TAG IS ALLOWED: nothing completes it, and it saves as typed.
	rig.browser.typeTagsForTest("tape, glassy");
	CHECK_FALSE(rig.browser.acceptCompletion());
	rig.browser.typeForTest("C");
	rig.browser.submitPrompt();
	const int c = rig.browser.indexOf("C");
	REQUIRE(c >= 0);
	CHECK(rig.browser.entry(static_cast<size_t>(c)).meta.tags == std::vector<std::string> { "tape", "glassy" });
}

TEST_CASE("tag suggestions are PER DEVICE TYPE: a SKLITTER browser never offers Feathers' tags") {
	ScopedPresetLibrary library("fo-types");
	REQUIRE(PresetStore::save("Pad", { 1 }, kPluginBank, {}, { { "lush" }, 1'800'000'000 }));
	REQUIRE(PresetStore::save("Chop", { 1 }, devicePresetBank(fx::sklitter::kDescriptor), {},
	                          { { "stutter" }, 1'800'000'000 }));
	const auto chipsOf = [](const PresetBank& bank) {
		FunctionPresetProvider provider([](PresetData& out) {
			out.bytes = { 1 };
			return true;
		}, nullptr);
		PresetBrowser browser(std::vector<std::string> {}, std::vector<PresetData> {}, bank, PresetBrowser::kPanelStyle);
		browser.setProvider(&provider);
		browser.clock = [] { return int64_t { 1'800'000'000 }; };
		browser.loadBankFromDisk();
		browser.chooseFromMenu(PresetBrowser::kMenuSave);
		std::vector<std::string> out = browser.chips();
		browser.cancelPrompt();
		return out;
	};
	CHECK(chipsOf(devicePresetBank(fx::sklitter::kDescriptor)) == std::vector<std::string> { "stutter" });
	CHECK(chipsOf(kPluginBank) == std::vector<std::string> { "lush" });
}

//--------------------------------------------------------------------------------------------------//
// FEATHERS' OWN EDITOR, END TO END                                                                  //
//--------------------------------------------------------------------------------------------------//

TEST_CASE("Feathers' editor installs its Init as a file in Feathers/, and a saved preset comes back by value") {
	const ScopedPresetLibrary library("fo-editor");
	StatefulGuiHost host;
	ParamBridge bridge;
	feathers::BridgeDeviceHost deviceHost { bridge };
	host.bridge = &bridge;
	Gui gui(host, bridge);
	REQUIRE(Gui::isAvailable());
	REQUIRE(gui.create(CLAP_WINDOW_API_X11, false));
	PresetBrowser* browser = gui.presetBrowserForTest();
	REQUIRE(browser != nullptr);
	CHECK(browser->bank() == kPluginBank);
	CHECK(std::filesystem::exists(library.path() / "Feathers" / "Init.fpreset"));
	CHECK(browser->currentName() == "Init");

	const clap_id zone = static_cast<clap_id>(ParamId::AZone);
	host.adopt(ParamId::AZone, 400.0);
	gui.tickForTest();
	saveThroughPrompt(*browser);
	const int saved = browser->currentIndex();
	REQUIRE(saved >= 0);
	CHECK(std::filesystem::exists(library.path() / "Feathers" / (browser->currentName() + ".fpreset")));
	host.adopt(ParamId::AZone, 800.0);
	gui.tickForTest();
	// RECALL WITHOUT LEAVING IT FIRST — you change something and pick it again to get it back.
	browser->selectIndex(saved);
	gui.tickForTest();
	CHECK(bridge.value(zone) == doctest::Approx(400.0));
	// OVERWRITE REPLACES THE BYTES.
	host.adopt(ParamId::AZone, 650.0);
	gui.tickForTest();
	REQUIRE(browser->overwriteCurrent(saved));
	host.adopt(ParamId::AZone, 100.0);
	gui.tickForTest();
	browser->selectIndex(saved);
	gui.tickForTest();
	CHECK(bridge.value(zone) == doctest::Approx(650.0));
}

TEST_CASE("a saved preset outlives the editor") {
	const ScopedPresetLibrary library("fo-outlives");
	const clap_id zone = static_cast<clap_id>(ParamId::AZone);
	std::string savedName;
	{
		StatefulGuiHost host;
		ParamBridge bridge;
		feathers::BridgeDeviceHost deviceHost { bridge };
		host.bridge = &bridge;
		Gui gui(host, bridge);
		REQUIRE(gui.create(CLAP_WINDOW_API_X11, false));
		PresetBrowser* browser = gui.presetBrowserForTest();
		REQUIRE(browser != nullptr);
		host.adopt(ParamId::AZone, 400.0);
		gui.tickForTest();
		saveThroughPrompt(*browser);
		savedName = browser->currentName();
		gui.destroy();
	}
	{
		StatefulGuiHost host;
		ParamBridge bridge;
		feathers::BridgeDeviceHost deviceHost { bridge };
		host.bridge = &bridge;
		Gui gui(host, bridge);
		REQUIRE(gui.create(CLAP_WINDOW_API_X11, false));
		PresetBrowser* browser = gui.presetBrowserForTest();
		REQUIRE(browser != nullptr);
		const int found = browser->indexOf(savedName);
		REQUIRE(found >= 0);
		host.adopt(ParamId::AZone, 900.0);
		gui.tickForTest();
		browser->selectIndex(found);
		gui.tickForTest();
		CHECK(bridge.value(zone) == doctest::Approx(400.0));
	}
}

TEST_CASE("a preset that will not load says so, instead of doing nothing") {
	const ScopedPresetLibrary library("fo-refused");
	StatefulGuiHost host;
	ParamBridge bridge;
	feathers::BridgeDeviceHost deviceHost { bridge };
	host.bridge = &bridge;
	Gui gui(host, bridge);
	REQUIRE(gui.create(CLAP_WINDOW_API_X11, false));
	PresetBrowser* browser = gui.presetBrowserForTest();
	REQUIRE(browser != nullptr);
	saveThroughPrompt(*browser);
	const int saved = browser->currentIndex();
	host.refuseLoads = true;
	CHECK(gui.notifyTextForTest().find("did not load") == std::string::npos);
	browser->selectIndex(saved);
	CHECK(gui.notifyTextForTest().find("did not load") != std::string::npos);
}

TEST_CASE("the header's browse-by-tag reopens its list in the editor, and the next editor opens filed by tag") {
	const ScopedPresetLibrary library("fo-view-editor");
	{
		StatefulGuiHost host;
		ParamBridge bridge;
		feathers::BridgeDeviceHost deviceHost { bridge };
		host.bridge = &bridge;
		Gui gui(host, bridge);
		REQUIRE(gui.create(CLAP_WINDOW_API_X11, false));
		PresetBrowser* browser = gui.presetBrowserForTest();
		REQUIRE(browser != nullptr);
		CHECK(browser->view() == PresetBrowser::View::Folders); // a clean desk
		const size_t before = openMenus(*browser);
		browser->chooseFromMenu(PresetBrowser::kMenuByTag);
		// IN THE EDITOR'S OWN TREE, where the window's loop services the timer.
		REQUIRE(pumpUntil([&] { return openMenus(*browser) == before + 1; }));
		CHECK(browser->view() == PresetBrowser::View::Tags);
		// WRITTEN THE MOMENT IT CHANGED, read back off the desk file.
		EditorPrefs desk;
		desk.load();
		CHECK(desk.flag("presets.view.tags"));
		gui.destroy();
	}
	{
		StatefulGuiHost host;
		ParamBridge bridge;
		feathers::BridgeDeviceHost deviceHost { bridge };
		host.bridge = &bridge;
		Gui gui(host, bridge);
		REQUIRE(gui.create(CLAP_WINDOW_API_X11, false));
		PresetBrowser* browser = gui.presetBrowserForTest();
		REQUIRE(browser != nullptr);
		CHECK(browser->view() == PresetBrowser::View::Tags);
	}
}

TEST_CASE("GG placement: the preset list, the Delete confirmation and the Replace question hang off their boxes, "
          "at the top of a window and at its foot") {
	// The menus presets-2 added (`ItemStepper::showList`, the two confirmations) go through
	// `ui::showMenuAt` like every other dropdown; checked against the box each one hangs off, in an
	// editor-sized frame, with the browser near the top and then at the foot where every menu must go UP.
	using menu_check::checkHangsOff;
	using menu_check::inTop;
	for (const float y : { 10.0f, 700.0f - PresetBrowser::kHeight - 4.0f }) {
		CAPTURE(y);
		Rig rig("gg-placement");
		rig.save("Doomed");
		visage::Frame window;
		window.setBounds(0.0f, 0.0f, 1240.0f, 700.0f);
		window.addChild(&rig.browser);
		rig.browser.setBounds(500.0f, y, rig.browser.totalWidth(), PresetBrowser::kHeight);

		rig.browser.showPresetMenu();
		checkHangsOff(ui::lastMenuPlacementForTest(), inTop(rig.browser, rig.browser.nameBounds()));
		ui::dismissMenuOf(rig.browser);

		rig.browser.selectIndex(rig.browser.indexOf("Doomed"));
		rig.browser.chooseFromMenu(PresetBrowser::kMenuDelete);
		checkHangsOff(ui::lastMenuPlacementForTest(), inTop(rig.browser, rig.browser.nameBounds()));
		rig.browser.resolveDelete(false);

		rig.browser.chooseFromMenu(PresetBrowser::kMenuSave);
		rig.browser.typeForTest("Doomed");
		rig.browser.submitPrompt();
		REQUIRE(rig.browser.prompt() == PresetBrowser::Prompt::ConfirmReplace);
		visage::Frame* field = rig.browser.nameFieldForTest();
		REQUIRE(field != nullptr);
		// THE PROMPT ITSELF OPENS UNDER THE BROWSER (`openPrompt`), so at the window's foot it is off the
		// window and so is anything hung off it — a prompt question, not a menu one, and out of this sweep.
		// Asked where the prompt is on screen.
		const bool onScreen = field->positionInWindow().y + field->height() <= window.height();
		CHECK((onScreen || y > 100.0f)); // at the top it IS asked
		if (onScreen) {
			checkHangsOff(ui::lastMenuPlacementForTest(), inTop(*field, { 0.0f, 0.0f, field->width(), field->height() }));
		}
		rig.browser.cancelPrompt();
		window.removeChild(&rig.browser);
	}
}

//------------------------------------------------------------------------------------------------//
// IMPORT… AND EXPORT…, THROUGH THE OS FILE DIALOG — driven by a fake backend, so no case ever opens   //
// a real dialog, and every file a case reads or writes is under its own scratch library.             //
//------------------------------------------------------------------------------------------------//

namespace {

/// The OS dialog as a case sees it: what was asked for, and an answer the case gives.
class FakeDialogs final : public ui::FileDialogBackend {
public:
	class FakeSession final : public Session {
	public:
		explicit FakeSession(FakeDialogs& owner) : owner_(owner) {}
		void cancel() override { ++owner_.cancels; }

	private:
		FakeDialogs& owner_;
	};
	std::unique_ptr<Session> start(const ui::FileDialogOptions& options,
	                               std::shared_ptr<ui::FileDialogChannel> channel) override {
		++starts;
		last = options;
		this->channel = std::move(channel);
		return std::make_unique<FakeSession>(*this);
	}
	/// The person chose `path`; the browser hears of it when its dialog is polled, as the timer would.
	void choose(PresetBrowser& browser, const std::filesystem::path& path) {
		REQUIRE(channel != nullptr);
		channel->complete({ ui::FileDialogResult::Status::Chosen, path, {} });
		CHECK(browser.fileDialogForTest().poll());
	}

	bool available() override { return canShow; }

	int starts = 0;
	int cancels = 0;
	bool canShow = true;
	ui::FileDialogOptions last;
	std::shared_ptr<ui::FileDialogChannel> channel;
};

/// A preset file somewhere that is NOT the bank, the way one arrives from a download or a friend.
std::filesystem::path presetFileElsewhere(const ScopedPresetLibrary& library, const std::string& file,
                                          const std::vector<uint8_t>& payload, const std::vector<std::string>& tags = {}) {
	const std::filesystem::path path = library.path() / "Downloads" / file;
	writeRaw(path, PresetStore::encode(PresetMeta { tags, 1'700'000'000 }, payload));
	return path;
}

/// What a notifier was told.
struct HeardNotices final : ui::Notifier {
	std::vector<std::string> said;
	void say(std::string_view text) override { said.emplace_back(text); }
};

} // namespace

TEST_CASE("Import… opens the OS file dialog for a .fpreset; the chosen file is loaded, copied into the bank and selected") {
	FakeDialogs dialogs; // FIRST, so it outlives the browser whose open dialog it would be asked to cancel
	Rig rig("io-import");
	ui::ScopedFileDialogBackend useFake(dialogs);
	const std::filesystem::path source = presetFileElsewhere(rig.library, "Big Pad.fpreset", { 55 }, { "wide", "pad" });

	rig.browser.chooseFromMenu(PresetBrowser::kMenuImport);
	REQUIRE(dialogs.starts == 1);
	CHECK(dialogs.last.mode == ui::FileDialogOptions::Mode::OpenFile);
	REQUIRE(dialogs.last.filters.size() == 1);
	CHECK(dialogs.last.filters[0].extensions == std::vector<std::string> { "fpreset" });
	// ASKED, NOTHING DONE: the dialog is up and the bank and the sound are as they were.
	CHECK(rig.applied.empty());
	CHECK(rig.disk().size() == 1);

	dialogs.choose(rig.browser, source);
	REQUIRE(rig.applied.size() == 1);
	CHECK(rig.applied[0] == std::vector<uint8_t> { 55 });
	const int at = rig.browser.indexOf("Big Pad");
	REQUIRE(at >= 0);
	CHECK(rig.browser.currentIndex() == at);
	CHECK_FALSE(rig.browser.isReadOnly(at));
	const std::vector<StoredPreset> disk = rig.disk();
	REQUIRE(disk.size() == 2);
	const auto stored = std::find_if(disk.begin(), disk.end(), [](const StoredPreset& p) { return p.name == "Big Pad"; });
	REQUIRE(stored != disk.end());
	CHECK(stored->bytes == std::vector<uint8_t> { 55 });
	CHECK(stored->meta.tags == std::vector<std::string> { "wide", "pad" }); // the file's tags came with it
	CHECK(stored->path.parent_path() == PresetStore::directory(rig.browser.bank()));
	CHECK(std::filesystem::exists(source)); // a COPY: the original is untouched
}

TEST_CASE("Import validates like a load: garbage, a missing file, a folder and a refused payload write NOTHING") {
	Rig rig("io-refuse");
	HeardNotices heard;
	rig.browser.setNotifier(&heard);
	using Outcome = PresetBrowser::ImportOutcome;

	// NOT A FILE, OR NOTHING IN IT.
	CHECK(rig.browser.importFile(rig.library.path() / "nowhere.fpreset") == Outcome::Unreadable);
	CHECK(rig.browser.importFile(rig.library.path()) == Outcome::Unreadable);
	writeRaw(rig.library.path() / "Downloads" / "Empty.fpreset", {});
	CHECK(rig.browser.importFile(rig.library.path() / "Downloads" / "Empty.fpreset") == Outcome::Unreadable);
	// A HEADER THAT LIES ABOUT ITS OWN LENGTH is not a preset file (`PresetStore::decode`).
	std::vector<uint8_t> lying = PresetStore::encode(PresetMeta { { "x" }, 1 }, { 9 });
	lying.resize(lying.size() / 2);
	writeRaw(rig.library.path() / "Downloads" / "Lying.fpreset", lying);
	CHECK(rig.browser.importFile(rig.library.path() / "Downloads" / "Lying.fpreset") == Outcome::Unreadable);
	// A PAYLOAD THE PROVIDER REFUSES — the Rig's "does not load" byte — is refused exactly as a pick of it
	// would be: the provider is asked, it says no, and nothing is copied.
	const std::filesystem::path refused = presetFileElsewhere(rig.library, "Broken.fpreset", { 0 });
	CHECK(rig.browser.importFile(refused) == Outcome::Refused);

	CHECK(rig.applied.empty());
	CHECK(rig.disk().size() == 1); // "Init", and nothing else
	CHECK(rig.browser.indexOf("Broken") < 0);
	CHECK(rig.browser.indexOf("Lying") < 0);
	CHECK(heard.said.size() == 4); // each unreadable one said so; the provider speaks for its own refusal
}

TEST_CASE("Import of a name already in the bank ASKS first: Cancel changes nothing, Keep both, Replace") {
	Rig rig("io-collide");
	rig.save("Crunch", "old");
	REQUIRE(rig.disk().size() == 2);
	const std::filesystem::path source = presetFileElsewhere(rig.library, "Crunch.fpreset", { 42 }, { "new" });
	using Outcome = PresetBrowser::ImportOutcome;
	using Choice = PresetBrowser::ImportChoice;

	// ASKED BEFORE THE SOUND CHANGES — so Cancel leaves everything exactly as it was.
	CHECK(rig.browser.importFile(source) == Outcome::Asking);
	CHECK(rig.browser.importPending());
	CHECK(rig.applied.empty());
	CHECK(rig.browser.resolveImport(Choice::Cancel) == Outcome::Refused);
	CHECK_FALSE(rig.browser.importPending());
	CHECK(rig.applied.empty());
	REQUIRE(rig.disk().size() == 2);

	// KEEP BOTH: a copy beside it, never over it.
	CHECK(rig.browser.importFile(source) == Outcome::Asking);
	CHECK(rig.browser.resolveImport(Choice::KeepBoth) == Outcome::Imported);
	CHECK(rig.applied.back() == std::vector<uint8_t> { 42 });
	CHECK(rig.browser.currentIndex() == rig.browser.indexOf("Crunch 2"));
	std::vector<StoredPreset> disk = rig.disk();
	REQUIRE(disk.size() == 3);
	CHECK(disk[0].name == "Crunch");
	CHECK(disk[0].bytes == std::vector<uint8_t> { 7 });
	CHECK(disk[1].name == "Crunch 2");
	CHECK(disk[1].bytes == std::vector<uint8_t> { 42 });

	// REPLACE: that preset's file now holds the imported one, in place.
	CHECK(rig.browser.importFile(source) == Outcome::Asking);
	CHECK(rig.browser.resolveImport(Choice::Replace) == Outcome::Imported);
	CHECK(rig.browser.currentIndex() == rig.browser.indexOf("Crunch"));
	disk = rig.disk();
	REQUIRE(disk.size() == 3);
	CHECK(disk[0].name == "Crunch");
	CHECK(disk[0].bytes == std::vector<uint8_t> { 42 });
	CHECK(disk[0].meta.tags == std::vector<std::string> { "new" });
}

TEST_CASE("Import over a READ-ONLY entry cannot replace it — only Keep both or Cancel") {
	EnvVar root("FEATHERS_PRESET_ROOT", nullptr);
	EnvVar home("HOME", nullptr); // no library at all: "Init" is in memory, read-only
	ScopedPresetLibrary scratch("io-readonly-src");
	::unsetenv("FEATHERS_PRESET_ROOT");
	FunctionPresetProvider provider([](PresetData& out) { out.bytes = { 1 }; return true; }, nullptr);
	PresetBrowser browser(std::vector<std::string> { "Init" }, std::vector<PresetData> { PresetData { { 1 } } }, "Test",
	                      PresetBrowser::kHeaderStyle);
	browser.setProvider(&provider);
	browser.loadBankFromDisk();
	REQUIRE(browser.isReadOnly(browser.indexOf("Init")));
	const std::filesystem::path source = presetFileElsewhere(scratch, "Init.fpreset", { 3 });
	CHECK(browser.importFile(source) == PresetBrowser::ImportOutcome::Asking);
	CHECK(browser.resolveImport(PresetBrowser::ImportChoice::Replace) == PresetBrowser::ImportOutcome::Refused);
	CHECK(browser.entry(static_cast<size_t>(browser.indexOf("Init"))).data.bytes == std::vector<uint8_t> { 1 });
}

TEST_CASE("Export… opens the OS save dialog named after the preset; the live sound is written there with its tags") {
	FakeDialogs dialogs; // FIRST, so it outlives the browser
	Rig rig("io-export");
	ui::ScopedFileDialogBackend useFake(dialogs);
	rig.save("Glass", "shiny, cold");
	rig.payload = 9; // the live sound has moved on since the save: EXPORT writes what is playing

	rig.browser.chooseFromMenu(PresetBrowser::kMenuExport);
	REQUIRE(dialogs.starts == 1);
	CHECK(dialogs.last.mode == ui::FileDialogOptions::Mode::SaveFile);
	CHECK(dialogs.last.defaultName == "Glass.fpreset");
	const int capturesBefore = rig.captures;

	const std::filesystem::path chosen = rig.library.path() / "Elsewhere" / "Glass.fpreset";
	std::filesystem::create_directories(chosen.parent_path());
	dialogs.choose(rig.browser, chosen);
	CHECK(rig.captures == capturesBefore + 1);
	REQUIRE(std::filesystem::exists(chosen));
	std::ifstream in(chosen, std::ios::binary);
	const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	PresetMeta meta;
	std::vector<uint8_t> payload;
	REQUIRE(PresetStore::decode(bytes, &meta, &payload));
	CHECK(payload == std::vector<uint8_t> { 9 });
	CHECK(meta.tags == std::vector<std::string> { "shiny", "cold" });
	// AND THE BANK IS UNTOUCHED: an export is a copy out, not a save.
	const std::vector<StoredPreset> disk = rig.disk();
	REQUIRE(disk.size() == 2);
	CHECK(disk[0].name == "Glass");
	CHECK(disk[0].bytes == std::vector<uint8_t> { 7 });
}

TEST_CASE("Export adds .fpreset to a name without one, and what it writes imports back to the same sound") {
	Rig rig("io-roundtrip");
	rig.payload = 23;
	const std::filesystem::path out = rig.library.path() / "Elsewhere" / "Mine";
	std::filesystem::create_directories(out.parent_path());
	REQUIRE(rig.browser.exportTo(out));
	std::filesystem::path written = out;
	written += ".fpreset";
	REQUIRE(std::filesystem::exists(written));
	CHECK_FALSE(std::filesystem::exists(out));
	CHECK(rig.browser.importFile(written) == PresetBrowser::ImportOutcome::Imported);
	CHECK(rig.applied.back() == std::vector<uint8_t> { 23 });
	CHECK(rig.browser.indexOf("Mine") >= 0);
}

TEST_CASE("a browser destroyed with its OS dialog open closes the dialog, and the late answer imports nothing") {
	ScopedPresetLibrary library("io-destroyed");
	FakeDialogs dialogs;
	ui::ScopedFileDialogBackend useFake(dialogs);
	int applied = 0;
	FunctionPresetProvider provider([](PresetData& out) { out.bytes = { 1 }; return true; },
	                                [&applied](const PresetData&) { ++applied; return true; });
	auto browser = std::make_unique<PresetBrowser>(std::vector<std::string> { "Init" },
	                                               std::vector<PresetData> { PresetData { { 1 } } }, "Test",
	                                               PresetBrowser::kHeaderStyle);
	browser->setProvider(&provider);
	browser->loadBankFromDisk();
	browser->chooseFromMenu(PresetBrowser::kMenuImport);
	REQUIRE(dialogs.starts == 1);
	const std::shared_ptr<ui::FileDialogChannel> channel = dialogs.channel;
	browser.reset();
	CHECK(dialogs.cancels == 1);
	CHECK(channel->abandoned());
	channel->complete({ ui::FileDialogResult::Status::Chosen, presetFileElsewhere(library, "Late.fpreset", { 5 }), {} });
	std::this_thread::sleep_for(std::chrono::milliseconds(2 * ui::FileDialog::kPollMs));
	visage::EventManager::instance().checkEventTimers();
	CHECK(applied == 0);
	CHECK(PresetStore::loadAll("Test").size() == 1);
}

TEST_CASE("GG cascade: the list's Delete row opens its confirmation from the same box, and Delete there removes the file") {
	// The confirmation is opened INSIDE the list's selection callback, from the same box and anchor —
	// the shape `showMenuAt`'s re-click toggle once read as a second press on an open menu, closing it
	// and leaving the Delete row doing nothing. Picked the way visage picks, through the live popups.
	using menu_check::newestPopup;
	using menu_check::pickRow;
	Rig rig("gg-cascade-delete");
	rig.save("Doomed");
	const int at = rig.browser.indexOf("Doomed");
	rig.browser.selectIndex(at);
	const std::filesystem::path file = rig.browser.entry(static_cast<size_t>(at)).path;

	rig.browser.showPresetMenu();
	visage::PopupMenuFrame* list = newestPopup(rig.browser);
	REQUIRE(list != nullptr);
	REQUIRE(pickRow(*list, "Delete \"Doomed\"…"));

	visage::PopupMenuFrame* confirm = newestPopup(rig.browser);
	CHECK(rig.browser.pendingDelete() == at);
	CHECK(ui::menuOpenFor(rig.browser));
	REQUIRE(confirm != nullptr);
	REQUIRE(confirm != list);
	REQUIRE(pickRow(*confirm, "Delete \"Doomed\""));
	CHECK_FALSE(std::filesystem::exists(file));
	CHECK(rig.browser.indexOf("Doomed") < 0);
}

TEST_CASE("GG: the save prompt opens INSIDE the window — below the browser at the top, above it at the foot") {
	// Asserted as geometry: the prompt's bounds, in the window's coordinates, lie within the 1240x700
	// window; and at the foot it sits wholly ABOVE the browser, so it does not cover the box it is about.
	for (const float y : { 10.0f, 700.0f - PresetBrowser::kHeight - 4.0f }) {
		CAPTURE(y);
		Rig rig("gg-prompt");
		visage::Frame window;
		window.setBounds(0.0f, 0.0f, 1240.0f, 700.0f);
		window.addChild(&rig.browser);
		rig.browser.setBounds(500.0f, y, rig.browser.totalWidth(), PresetBrowser::kHeight);
		rig.browser.chooseFromMenu(PresetBrowser::kMenuSave);
		REQUIRE(rig.browser.prompt() == PresetBrowser::Prompt::Naming);
		visage::Frame* prompt = rig.browser.promptFrameForTest();
		REQUIRE(prompt != nullptr);
		REQUIRE(prompt->parent() == &window);
		const visage::Bounds b = prompt->bounds();
		CHECK(b.x() >= 0.0f);
		CHECK(b.y() >= 0.0f);
		CHECK(b.right() <= 1240.0f);
		CHECK(b.bottom() <= 700.0f);
		if (y < 100.0f) {
			CHECK(b.y() == y + PresetBrowser::kHeight); // below, as before (whole numbers: exact)
		} else {
			CHECK(b.bottom() <= y); // above the browser, not over it
		}
		rig.browser.cancelPrompt();
		window.removeChild(&rig.browser);
	}
}

/// The palettes the prompt's text fields carry — the only frames in Feathers that set one.
static std::vector<const visage::Palette*> promptPalettes(visage::Frame* prompt) {
	std::vector<const visage::Palette*> out;
	for (visage::Frame* child : prompt->children()) {
		if (child->palette() != nullptr) {
			out.push_back(child->palette());
		}
	}
	return out;
}

TEST_CASE("the prompt's fields share ONE palette that outlives every browser, not one each that dies with it") {
	// **A CANVAS KEEPS THE LAST PALETTE IT WAS HANDED, AND NOTHING EVER TAKES IT BACK.** visage's
	// `Frame::drawToRegion` calls `canvas.setPalette` only for a frame that HAS one, so a frame without
	// one (every visage popup) reads whichever palette was drawn last. A palette owned by a browser is
	// therefore read after the browser is gone — the SEGV in `PopupList::draw` was hit in Bitwig.
	// Asserted as the value that makes it impossible: two live browsers hand their fields the SAME
	// palette, which no per-browser member can be (both alive at once, so no reused address can pass).
	Rig a("palette-share-a");
	Rig b("palette-share-b");
	const std::vector<const visage::Palette*> pa = promptPalettes(a.browser.promptFrameForTest());
	const std::vector<const visage::Palette*> pb = promptPalettes(b.browser.promptFrameForTest());
	REQUIRE(pa.size() == 2); // Name and Tags: not vacuous
	REQUIRE(pb.size() == 2);
	CHECK(pa[0] == pa[1]);
	CHECK(pa[0] == pb[0]);
	CHECK(pb[0] == pb[1]);
}

TEST_CASE("a popup drawn after a browser whose prompt was drawn is destroyed reads no freed palette "
          "(the Bitwig SEGV in PopupList::draw)") {
	// THE CRASH, DRIVEN THROUGH A REAL WINDOWLESS EDITOR: the prompt's fields draw (the canvas takes
	// their palette), the browser goes (a device panel closed), then any menu opens and paints. Before
	// the fix the popup's `canvas.color()` followed the canvas into the browser's freed palette; under
	// ASan this case is a heap-use-after-free, and without it a read of freed memory that usually
	// survives — which is how it reached Bitwig.
	visage::ApplicationEditor editor;
	editor.setBounds(0.0f, 0.0f, 1240.0f, 700.0f);
	editor.setWindowless(1240, 700);
	if (!visage::Renderer::instance().supported()) {
		MESSAGE("SKIP no renderer: " << visage::Renderer::instance().errorMessage());
		return;
	}
	visage::Frame anchor;
	editor.addChild(&anchor);
	anchor.setBounds(20.0f, 20.0f, 80.0f, 20.0f);
	{
		auto rig = std::make_unique<Rig>("palette-uaf");
		editor.addChild(&rig->browser);
		rig->browser.setBounds(500.0f, 10.0f, rig->browser.totalWidth(), PresetBrowser::kHeight);
		rig->browser.chooseFromMenu(PresetBrowser::kMenuSave);
		REQUIRE(rig->browser.prompt() == PresetBrowser::Prompt::Naming);
		REQUIRE(rig->browser.promptFrameForTest()->parent() != nullptr); // shown, in this editor's tree
		editor.redrawAll();
		editor.drawWindow();
		rig->browser.cancelPrompt();
		editor.removeChild(&rig->browser);
	}
	visage::PopupMenu menu;
	menu.addOption(0, "DAHSR");
	menu.addOption(1, "Math");
	REQUIRE(ui::showMenuAt(std::move(menu), anchor, { 0.0f, 0.0f, 80.0f, 20.0f }));
	REQUIRE(openMenus(editor) == 1);
	editor.redrawAll();
	editor.drawWindow();
	editor.drawWindow();
	CHECK(openMenus(editor) == 1);
	ui::dismissMenuOf(anchor);
	editor.removeChild(&anchor);
}

TEST_CASE("Import… and Export… are rows of the list, enabled only while an OS dialog can be shown") {
	FakeDialogs dialogs;
	Rig rig("io-rows");
	ui::ScopedFileDialogBackend useFake(dialogs);
	const auto row = [&rig](int id) {
		for (const ItemStepper::Row& r : rig.browser.listRows()) {
			if (r.id == id) {
				return std::optional<ItemStepper::Row>(r);
			}
		}
		return std::optional<ItemStepper::Row> {};
	};
	REQUIRE(row(PresetBrowser::kMenuImport).has_value());
	REQUIRE(row(PresetBrowser::kMenuExport).has_value());
	CHECK(row(PresetBrowser::kMenuImport)->label == "Import…");
	CHECK(row(PresetBrowser::kMenuImport)->enabled);
	CHECK(row(PresetBrowser::kMenuExport)->enabled);
	// NO PORTAL, NO ZENITY, NO KDIALOG: the rows stay, disabled — the menu keeps its shape.
	dialogs.canShow = false;
	CHECK_FALSE(row(PresetBrowser::kMenuImport)->enabled);
	CHECK_FALSE(row(PresetBrowser::kMenuExport)->enabled);
}

//------------------------------------------------------------------------------------------------//
// THE LIST'S SECOND LEVEL, AND WHERE THE LIBRARY IS ("the menu is clogged with     //
// things besides presets", "how am I supposed to select the folder for preset browsing?")            //
//------------------------------------------------------------------------------------------------//

namespace {

/// A menu as a case reads it: a row's label, "[name]" for a submenu, "---" for a separator. Read off
/// `buildList` — the menu `showList` shows — so it is where each row WENT, not where it asked to go.
std::vector<std::string> shape(const visage::PopupMenu& menu) {
	std::vector<std::string> out;
	for (const visage::PopupMenu& o : menu.options()) {
		if (o.isBreak()) {
			out.emplace_back("---");
		}
		else if (o.hasOptions()) {
			out.push_back("[" + o.name().toUtf8() + "]");
		}
		else {
			out.push_back(o.name().toUtf8());
		}
	}
	return out;
}

/// The submenu called `name` at the top of `menu`, or null.
const visage::PopupMenu* submenuOf(const visage::PopupMenu& menu, const std::string& name) {
	for (const visage::PopupMenu& o : menu.options()) {
		if (!o.isBreak() && o.hasOptions() && o.name().toUtf8() == name) {
			return &o;
		}
	}
	return nullptr;
}

/// **THE PROCESS'S CHOSEN ROOT AND THE DESK'S KEY, PUT BACK** — both outlive a case otherwise, and the
/// next case in this process would be browsing a folder this one chose.
struct ChosenRootGuard {
	~ChosenRootGuard() {
		(void)PresetBrowser::setPresetRoot({});
		EditorPrefs desk;
		desk.load();
		desk.setText(PresetBrowser::kRootPrefKey, {});
	}
};

/// The SKLITTER panel an editor opens from its fx lane, found in the editor's own tree — a device panel's
/// browser, which has no road to the editor's prefs.
ui::SklitterExpanded* openSklitterPanel(ModFixture& f) {
	auto* lane = findInTree<ui::FxLane>(f.root);
	REQUIRE(lane != nullptr);
	const size_t slot = f.bridge.addFxDevice(fx::sklitter::kTraits.tag);
	REQUIRE(slot < feathers::kMaxFxSlots);
	f.bridge.refresh();
	lane->resized();
	lane->scrollCardIntoView(slot);
	lane->resized();
	(void)f.gui.canvasForTest()->setCollapsed(true); // the floating panel
	REQUIRE(lane->onExpandDevice != nullptr);
	lane->onExpandDevice(slot);
	return findInTree<ui::SklitterExpanded>(f.root);
}

} // namespace

TEST_CASE("the preset list is presets, the three saves, and Manage — and Manage holds every other verb, grouped, in both views") {
	Rig rig("menu-shape");
	rig.save("Pads/Glass", "bright");
	rig.save("Air");
	rig.browser.selectIndex(rig.browser.indexOf("Air"));
	const std::vector<std::string> manage {
		"Overwrite \"Air\"", "Rename / tags…", "Delete \"Air\"…", "---", // Overwrite back in Manage
		"Import…", "Export…", "---",
		"\xe2\x80\xa2 Browse by folder", "Browse by tag", "---",
		"Choose preset folder…", "Reset preset folder",
	};
	// BY FOLDER: the top-level presets, the category submenu, then Save and Manage — nothing else.
	{
		const visage::PopupMenu menu = rig.browser.buildList();
		CHECK(shape(menu) == std::vector<std::string> { "Air", "Init", "[Pads]", "---", "Save \"Air\"", "Save as…", "Save as \"Air 2\"", "[Manage]" });
		const visage::PopupMenu* sub = submenuOf(menu, PresetBrowser::kManageMenu);
		REQUIRE(sub != nullptr);
		CHECK(shape(*sub) == manage);
		// EACH ROW IS STILL ITS VERB — the ids `choose` takes, in the submenu as they were at the foot.
		CHECK(sub->options()[0].id() == PresetBrowser::kMenuManageOverwrite);
		CHECK(sub->options()[1].id() == PresetBrowser::kMenuEdit);
		CHECK(sub->options()[10].id() == PresetBrowser::kMenuChooseRoot);
	}
	// BY TAG: the tag submenus instead of the categories; the foot is the same shape, the mark moved.
	rig.browser.chooseFromMenu(PresetBrowser::kMenuByTag);
	{
		const visage::PopupMenu menu = rig.browser.buildList();
		// Glass's category is NOT a tag here — the folder view is that view.
		CHECK(shape(menu) == std::vector<std::string> { "Air", "Init", "[tag]", "---", "Save \"Air\"", "Save as…", "Save as \"Air 2\"", "[Manage]" });
		const visage::PopupMenu* sub = submenuOf(menu, PresetBrowser::kManageMenu);
		REQUIRE(sub != nullptr);
		std::vector<std::string> byTag = manage;
		byTag[7] = "Browse by folder";
		byTag[8] = "\xe2\x80\xa2 Browse by tag";
		CHECK(shape(*sub) == byTag);
	}
	// AND THE ROWS SAY THE SAME: Save at the foot, everything else filed under Manage.
	std::vector<std::pair<int, std::string>> actions;
	for (const ItemStepper::Row& r : rig.browser.listRows()) {
		if (r.id >= ItemStepper::kActionBase) {
			actions.emplace_back(r.id, r.section);
		}
	}
	REQUIRE(actions.size() == 12);
	CHECK(actions[0] == std::pair<int, std::string> { PresetBrowser::kMenuOverwrite, "" });
	CHECK(actions[1] == std::pair<int, std::string> { PresetBrowser::kMenuSave, "" });
	CHECK(actions[2] == std::pair<int, std::string> { PresetBrowser::kMenuSaveVariant, "" });
	for (size_t i = 3; i < actions.size(); ++i) {
		CHECK(actions[i].second == PresetBrowser::kManageMenu);
	}
}

TEST_CASE("Show in file manager is gone — Choose preset folder… took its place") {
	Rig rig("menu-no-show");
	for (const ItemStepper::Row& r : rig.browser.listRows()) {
		CHECK(r.label.find("file manager") == std::string::npos);
		CHECK(r.label.find("Show in") == std::string::npos);
	}
	const visage::PopupMenu menu = rig.browser.buildList();
	const visage::PopupMenu* sub = submenuOf(menu, PresetBrowser::kManageMenu);
	REQUIRE(sub != nullptr);
	for (const visage::PopupMenu& o : sub->options()) {
		CHECK(o.name().toUtf8().find("file manager") == std::string::npos);
	}
}

TEST_CASE("ItemStepper: an action with a submenu goes IN it, at its first verb's place; separators group; "
          "an action without one stays at the foot") {
	struct Grouped : MockSource {
		std::vector<ItemAction> itemActions() const override {
			return {
				{ 1, "Top", true },
				{ 2, "A", true, "More" },
				{ 3, "B", false, "More" },
				{ 4, "C", true, "More", true },
				{ 5, "Bottom", true, {}, true },
			};
		}
	};
	Grouped source;
	source.labels = { "one", "two" };
	ItemStepper stepper(source, PresetBrowser::kPanelStyle);
	const visage::PopupMenu menu = stepper.buildList();
	CHECK(shape(menu) == std::vector<std::string> { "one", "two", "---", "Top", "[More]", "---", "Bottom" });
	const visage::PopupMenu* more = submenuOf(menu, "More");
	REQUIRE(more != nullptr);
	CHECK(shape(*more) == std::vector<std::string> { "A", "B", "---", "C" });
	CHECK_FALSE(more->options()[1].enabled());
	// A PICK IN THE SUBMENU IS THE SAME VERB: its id reaches the source.
	stepper.choose(more->options()[3].id());
	CHECK(source.acted == std::vector<int> { 4 });
}

TEST_CASE("Featherverb-shaped actions — no submenu, no separators — list exactly as before: at the foot, in order") {
	// The IR browser's `itemActions` is three-field braces; the fields added since default to "at the
	// foot, no separator". This is that shape through the stepper; the Featherverb cases in the devices
	// binary are the real browser.
	MockSource source;
	source.labels = { "a" };
	ItemStepper stepper(source, PresetBrowser::kPanelStyle);
	CHECK(shape(stepper.buildList()) == std::vector<std::string> { "a", "---", "Do it", "Not now" });
	for (const ItemStepper::Row& r : stepper.listRows()) {
		CHECK(r.section.empty());
		CHECK_FALSE(r.breakBefore);
	}
}

TEST_CASE("GG: the preset list with its Manage submenu opens through showMenuAt, and a re-click closes it") {
	using menu_check::checkHangsOff;
	using menu_check::inTop;
	Rig rig("menu-gg-manage");
	visage::Frame window;
	window.setBounds(0.0f, 0.0f, 1240.0f, 700.0f);
	window.addChild(&rig.browser);
	rig.browser.setBounds(500.0f, 10.0f, rig.browser.totalWidth(), PresetBrowser::kHeight);
	rig.browser.showPresetMenu();
	REQUIRE(ui::lastMenuPlacementForTest().shown);
	checkHangsOff(ui::lastMenuPlacementForTest(), inTop(rig.browser, rig.browser.nameBounds()));
	CHECK(ui::menuOpenFor(rig.browser));
	rig.browser.showPresetMenu(); // the re-click
	CHECK_FALSE(ui::lastMenuPlacementForTest().shown);
	window.removeChild(&rig.browser);
}

TEST_CASE("Choose preset folder… is enabled only while an OS folder picker can be shown and the root is not pinned; "
          "Reset only while a folder is chosen") {
	FakeDialogs dialogs;
	const ScopedPresetLibrary scratch("root-rows");
	const EnvVar home("HOME", (scratch.path() / "home").c_str());
	ChosenRootGuard guard;
	const auto row = [](PresetBrowser& b, int id) {
		for (const ItemStepper::Row& r : b.listRows()) {
			if (r.id == id) {
				return r;
			}
		}
		FAIL("no row " << id);
		return ItemStepper::Row {};
	};
	ui::ScopedFileDialogBackend useFake(dialogs);
	{
		const EnvVar unpinned("FEATHERS_PRESET_ROOT", nullptr);
		Rig rig("root-rows-rig");
		// The Rig's library pinned the root again; unpin it for the browser's life.
		const EnvVar unpinnedAgain("FEATHERS_PRESET_ROOT", nullptr);
		CHECK(row(rig.browser, PresetBrowser::kMenuChooseRoot).label == "Choose preset folder…");
		CHECK(row(rig.browser, PresetBrowser::kMenuChooseRoot).enabled);
		CHECK_FALSE(row(rig.browser, PresetBrowser::kMenuResetRoot).enabled); // at the default already
		// NO PICKER: the row stays, disabled, and choosing it opens nothing.
		dialogs.canShow = false;
		CHECK_FALSE(row(rig.browser, PresetBrowser::kMenuChooseRoot).enabled);
		dialogs.canShow = true;
		// A CHOSEN FOLDER: Reset has something to reset.
		(void)PresetBrowser::setPresetRoot(scratch.path() / "Chosen");
		CHECK(row(rig.browser, PresetBrowser::kMenuResetRoot).enabled);
	}
	// PINNED BY THE ENVIRONMENT (the tests; anybody who sets it): both would change nothing, so both are off.
	Rig pinned("root-rows-pinned");
	CHECK(PresetStore::rootOverridden());
	CHECK_FALSE(row(pinned.browser, PresetBrowser::kMenuChooseRoot).enabled);
	CHECK_FALSE(row(pinned.browser, PresetBrowser::kMenuResetRoot).enabled);
	pinned.browser.chooseFromMenu(PresetBrowser::kMenuChooseRoot);
	CHECK(dialogs.starts == 0);
}

TEST_CASE("Choose preset folder… moves EVERY browser: the header's and a SKLITTER panel's re-list from the chosen "
          "folder, factory presets land in it, the prefs keep it, the next editor opens on it, Reset goes back") {
	FakeDialogs dialogs; // FIRST, so it outlives the browsers whose dialogs it answers
	ui::ScopedFileDialogBackend useFake(dialogs);
	const ScopedPresetLibrary scratch("root-choose");
	// NEVER THE REAL ~/Documents: the default root is under a scratch HOME, and the pin is lifted so the
	// chosen root is the one that counts.
	const EnvVar home("HOME", (scratch.path() / "home").c_str());
	const EnvVar unpinned("FEATHERS_PRESET_ROOT", nullptr);
	ChosenRootGuard guard;
	const std::filesystem::path fallback = scratch.path() / "home" / "Documents" / "Owlet" / "Feathers" / "Presets";
	const std::filesystem::path chosen = scratch.path() / "Elsewhere";
	const PresetBank sklitterBank = devicePresetBank(fx::sklitter::kDescriptor);
	// A LIBRARY ALREADY IN THE CHOSEN FOLDER — one preset per bank, so a re-list is visible by value.
	writeRaw(chosen / kPluginBank / "Far Pad.fpreset", PresetStore::encode({ { "far" }, 1'700'000'000 }, { 9 }));
	writeRaw(chosen / sklitterBank / "Far Chop.fpreset", PresetStore::encode({}, { 9 }));
	{
		ModFixture f;
		PresetBrowser* header = f.gui.presetBrowserForTest();
		REQUIRE(header != nullptr);
		ui::SklitterExpanded* sklitter = openSklitterPanel(f);
		REQUIRE(sklitter != nullptr);
		PresetBrowser& panel = sklitter->presetBrowserForTest();
		CHECK(panel.bank() == sklitterBank);
		CHECK(PresetStore::root() == fallback);
		CHECK(header->indexOf("Far Pad") < 0);
		CHECK(panel.indexOf("Far Chop") < 0);
		CHECK_FALSE(std::filesystem::exists(chosen / sklitterBank / "Init.fpreset"));

		// FROM THE PANEL, which has no prefs: the OS folder picker, starting where the library is now.
		panel.chooseFromMenu(PresetBrowser::kMenuChooseRoot);
		REQUIRE(dialogs.starts == 1);
		CHECK(dialogs.last.mode == ui::FileDialogOptions::Mode::OpenFolder);
		CHECK(dialogs.last.startDir == fallback);
		CHECK(PresetStore::root() == fallback); // asked, nothing moved yet
		const size_t live = PresetBrowser::liveCountForTest();
		dialogs.choose(panel, chosen);

		// THE ROOT MOVED FOR BOTH, and both re-listed from it — by value, off each browser.
		CHECK(PresetStore::root() == chosen);
		CHECK(live >= 2);
		const int far = header->indexOf("Far Pad");
		REQUIRE(far >= 0);
		CHECK(header->entry(static_cast<size_t>(far)).path == chosen / kPluginBank / "Far Pad.fpreset");
		CHECK(header->entry(static_cast<size_t>(far)).meta.tags == std::vector<std::string> { "far" });
		const int chop = panel.indexOf("Far Chop");
		REQUIRE(chop >= 0);
		CHECK(panel.entry(static_cast<size_t>(chop)).path == chosen / sklitterBank / "Far Chop.fpreset");
		// FACTORY PRESETS COPIED IN WHEN MISSING — the panel's Init, as a file in the new root. (The
		// header's Init is its live capture, and this fixture's quiet host captures nothing, so the header
		// has no factory file here; the editor below, on a host with state, is where its Init is asked.)
		const int init = panel.indexOf("Init");
		REQUIRE(init >= 0);
		CHECK(panel.entry(static_cast<size_t>(init)).path == chosen / sklitterBank / "Init.fpreset");
		CHECK(std::filesystem::exists(chosen / sklitterBank / "Init.fpreset"));

		// KEPT IN THE EDITOR'S PREFS (a redirected desk: main() points XDG_CONFIG_HOME at a scratch dir).
		EditorPrefs desk;
		desk.load();
		CHECK(desk.text(PresetBrowser::kRootPrefKey) == chosen.string());
	}
	// A NEW PROCESS KNOWS ONLY THE PREFS: forget the process-wide root, then open an editor.
	PresetStore::setChosenRoot({});
	REQUIRE(PresetStore::root() == fallback);
	{
		StatefulGuiHost host;
		ParamBridge bridge;
		feathers::BridgeDeviceHost deviceHost { bridge };
		host.bridge = &bridge;
		Gui gui(host, bridge);
		REQUIRE(gui.create(CLAP_WINDOW_API_X11, false));
		PresetBrowser* browser = gui.presetBrowserForTest();
		REQUIRE(browser != nullptr);
		CHECK(PresetStore::root() == chosen);
		CHECK(browser->indexOf("Far Pad") >= 0);
		CHECK(std::filesystem::exists(chosen / kPluginBank / "Init.fpreset"));
		// A SAVE GOES TO THE CHOSEN FOLDER.
		saveThroughPrompt(*browser);
		CHECK(std::filesystem::exists(chosen / kPluginBank / (browser->currentName() + ".fpreset")));

		// RESET: back to the default, RE-LISTED from it — its Init installed there by the re-list, since
		// nothing ever wrote one (the first editor's quiet host had nothing to capture) — and the desk
		// forgets the folder.
		REQUIRE_FALSE(std::filesystem::exists(fallback / kPluginBank / "Init.fpreset"));
		browser->chooseFromMenu(PresetBrowser::kMenuResetRoot);
		CHECK(PresetStore::root() == fallback);
		CHECK(browser->indexOf("Far Pad") < 0);
		CHECK(std::filesystem::exists(fallback / kPluginBank / "Init.fpreset"));
		const int init = browser->indexOf("Init");
		REQUIRE(init >= 0);
		CHECK(browser->entry(static_cast<size_t>(init)).path.parent_path() == fallback / kPluginBank);
		EditorPrefs desk;
		desk.load();
		CHECK(desk.text(PresetBrowser::kRootPrefKey).empty());
		CHECK(desk.values().count(PresetBrowser::kRootPrefKey) == 0);
		// THE CHOSEN FOLDER IS LEFT AS IT WAS — a reset moves where we look, it deletes nothing.
		CHECK(std::filesystem::exists(chosen / kPluginBank / "Far Pad.fpreset"));
		gui.destroy();
	}
}

TEST_CASE("a preset folder with a space, a # and a % survives the prefs file — the line format would cut it") {
	const ScopedPresetLibrary scratch("root-escape");
	ChosenRootGuard guard;
	const std::filesystem::path odd = scratch.path() / " My #1 100% Presets ";
	EditorPrefs desk;
	desk.load();
	{
		const std::unique_ptr<ui::PresetRootWatch> watch = ui::keepPresetRootIn(desk);
		(void)PresetBrowser::setPresetRoot(odd);
	}
	EditorPrefs reread;
	reread.load();
	CHECK(reread.text(PresetBrowser::kRootPrefKey) == odd.string());
	PresetStore::setChosenRoot({});
	ui::restorePresetRoot(reread);
	CHECK(PresetStore::chosenRoot() == odd);
}

//------------------------------------------------------------------------------------------------//
// NAMESPACED TAGS, AND THE CATEGORY IN THE METADATA                               //
//------------------------------------------------------------------------------------------------//

TEST_CASE("a tag is namespace:value — each half normalised; bare is the catch-all `tag`; category is reserved") {
	CHECK(normaliseTag("Mood : Dark   Side") == "mood:dark side");
	CHECK(normaliseTag("Ge nre!:Techno") == "genre:techno");
	CHECK(normaliseTag("dark") == "dark");
	CHECK(normaliseTag("tag:Dark") == "dark"); // the catch-all is spelt bare
	CHECK(normaliseTag(":dark") == "dark");
	CHECK(normaliseTag("mood:") == "");        // a namespace with no value is no tag
	CHECK(normaliseTag("Category:Pads") == ""); // the folder says where a preset lives
	// THE 32-BYTE LIMIT IS PER VALUE: a long namespace does not eat the value's room.
	const std::string longValue(40, 'x');
	CHECK(normaliseTag("mood:" + longValue) == "mood:" + std::string(kMaxTagBytes, 'x'));
	CHECK(normaliseNamespace(std::string(40, 'n')).size() == kMaxTagBytes);
	CHECK(parseTagList("Type:Pad, mood:dark, dark, TAG:dark, category:x") ==
	      std::vector<std::string> { "type:pad", "mood:dark", "dark" });
	CHECK(splitTag("mood:dark").ns == "mood");
	CHECK(splitTag("mood:dark").value == "dark");
	CHECK(splitTag("dark").ns == kCatchAllNamespace);
	CHECK(splitTag("dark").value == "dark");
}

TEST_CASE("a save writes its folder into the metadata as category:<folder>; the top level writes none") {
	Rig rig("ns-category-save");
	rig.save("Pads/Warm/Glass", "mood:dark"); // "Glass" in the category "Pads/Warm"
	rig.save("Air");
	const std::vector<StoredPreset> disk = rig.disk();
	const auto find = [&](const std::string& name) {
		const auto it = std::find_if(disk.begin(), disk.end(), [&](const StoredPreset& p) { return p.name == name; });
		REQUIRE(it != disk.end());
		return *it;
	};
	// READ OFF THE FILE'S OWN BYTES, not off the listing (which the folder decides).
	const auto fileMeta = [](const std::filesystem::path& file) {
		std::ifstream in(file, std::ios::binary);
		const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
		PresetMeta meta;
		std::vector<uint8_t> payload;
		REQUIRE(PresetStore::decode(bytes, &meta, &payload));
		return meta;
	};
	const PresetMeta glass = fileMeta(find("Glass").path);
	CHECK(glass.category == "Pads/Warm");
	CHECK(glass.tags == std::vector<std::string> { "mood:dark" }); // the category is not a user tag
	CHECK(PresetStore::metaJson(glass).find("\"category:Pads/Warm\"") != std::string::npos);
	const PresetMeta air = fileMeta(find("Air").path);
	CHECK(air.category.empty());
	CHECK(PresetStore::metaJson(air).find("category") == std::string::npos);
}

TEST_CASE("the folder wins locally: a file moved by hand lists under its folder, and its next save rewrites the metadata") {
	Rig rig("ns-folder-wins");
	const std::filesystem::path moved = PresetStore::directory(rig.browser.bank()) / "Pads" / "Moved.fpreset";
	writeRaw(moved, PresetStore::encode(PresetMeta { { "mood:dark" }, 1'700'000'000, "Leads" }, { 5 }));
	const auto categoryOnDisk = [&] {
		std::ifstream in(moved, std::ios::binary);
		const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
		PresetMeta meta;
		std::vector<uint8_t> payload;
		REQUIRE(PresetStore::decode(bytes, &meta, &payload));
		return meta.category;
	};
	REQUIRE(categoryOnDisk() == "Leads");
	rig.browser.loadBankFromDisk();
	const int at = rig.browser.indexOf("Moved", "Pads");
	REQUIRE(at >= 0);
	CHECK(rig.browser.indexOf("Moved", "Leads") < 0);
	CHECK(rig.browser.entry(static_cast<size_t>(at)).meta.category == "Pads");
	CHECK(categoryOnDisk() == "Leads"); // reading writes nothing
	REQUIRE(rig.browser.overwriteCurrent(at));
	CHECK(categoryOnDisk() == "Pads");
	CHECK(std::filesystem::exists(moved)); // rewritten in place, not moved
}

TEST_CASE("Import files a preset into its metadata's category folder, creating it; without one, the top") {
	FakeDialogs dialogs;
	Rig rig("ns-import-category");
	ui::ScopedFileDialogBackend useFake(dialogs);
	const std::filesystem::path from = rig.library.path() / "Downloads" / "Soft Keys.fpreset";
	writeRaw(from, PresetStore::encode(PresetMeta { { "type:keys" }, 1'700'000'000, "Keys/Soft" }, { 44 }));
	const std::filesystem::path folder = PresetStore::directory(rig.browser.bank()) / "Keys" / "Soft";
	REQUIRE_FALSE(std::filesystem::exists(folder));
	CHECK(rig.browser.importFile(from) == PresetBrowser::ImportOutcome::Imported);
	const int at = rig.browser.indexOf("Soft Keys", "Keys/Soft");
	REQUIRE(at >= 0);
	CHECK(rig.browser.entry(static_cast<size_t>(at)).path == folder / "Soft Keys.fpreset");
	CHECK(std::filesystem::exists(folder / "Soft Keys.fpreset"));
	CHECK(rig.browser.entry(static_cast<size_t>(at)).meta.tags == std::vector<std::string> { "type:keys" });
	// A SECOND OF THE SAME NAME IN THAT FOLDER ASKS; Keep both lands beside it, in the same folder.
	CHECK(rig.browser.importFile(from) == PresetBrowser::ImportOutcome::Asking);
	CHECK(rig.browser.resolveImport(PresetBrowser::ImportChoice::KeepBoth) == PresetBrowser::ImportOutcome::Imported);
	CHECK(std::filesystem::exists(folder / "Soft Keys 2.fpreset"));
	// NO CATEGORY: the top of the bank, as before.
	const std::filesystem::path bare = rig.library.path() / "Downloads" / "Bare.fpreset";
	writeRaw(bare, PresetStore::encode(PresetMeta { {}, 1'700'000'000 }, { 45 }));
	CHECK(rig.browser.importFile(bare) == PresetBrowser::ImportOutcome::Imported);
	CHECK(rig.browser.indexOf("Bare") >= 0);
	// AND EXPORT CARRIES THE CATEGORY, so what goes out comes back to the same folder.
	rig.browser.selectIndex(rig.browser.indexOf("Soft Keys", "Keys/Soft"));
	const std::filesystem::path out = rig.library.path() / "Out" / "Sent.fpreset";
	std::filesystem::create_directories(out.parent_path());
	REQUIRE(rig.browser.exportTo(out));
	std::ifstream in(out, std::ios::binary);
	const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	PresetMeta meta;
	std::vector<uint8_t> payload;
	REQUIRE(PresetStore::decode(bytes, &meta, &payload));
	CHECK(meta.category == "Keys/Soft");
}

TEST_CASE("browse by tag is namespace ▸ value (count) ▸ presets: values by count, past twelve under More…, no category") {
	Rig rig("ns-tag-view");
	// type: a on 3 presets, b and c on 2, then twelve on one each (s00..s11) — fifteen values in all.
	int n = 0;
	const auto add = [&](const std::string& category, std::vector<std::string> tags) {
		REQUIRE(rig.browser.saveAs(category + "P" + std::to_string(n++), tags));
	};
	add("Pads/", { "type:a", "mood:dark" });
	add("", { "type:a", "type:b" });
	add("", { "type:a", "type:c", "dark" });
	add("", { "type:b" });
	add("", { "type:c" });
	for (int i = 0; i < 12; ++i) {
		char v[8];
		std::snprintf(v, sizeof v, "s%02d", i);
		add("", { std::string("type:") + v });
	}
	rig.browser.chooseFromMenu(PresetBrowser::kMenuByTag);
	const visage::PopupMenu menu = rig.browser.buildList();
	// NAMESPACES ALPHABETICAL; `category` is not among them though "Pads" is a folder.
	std::vector<std::string> top = shape(menu);
	top.erase(std::remove_if(top.begin(), top.end(), [](const std::string& s) { return s.empty() || s[0] != '['; }), top.end());
	CHECK(top == std::vector<std::string> { "[mood]", "[tag]", "[type]", "[Manage]" });
	const visage::PopupMenu* type = submenuOf(menu, "type");
	REQUIRE(type != nullptr);
	CHECK(shape(*type) == std::vector<std::string> { "[a (3)]", "[b (2)]", "[c (2)]", "[s00 (1)]", "[s01 (1)]",
	                                                 "[s02 (1)]", "[s03 (1)]", "[s04 (1)]", "[s05 (1)]", "[s06 (1)]",
	                                                 "[s07 (1)]", "[s08 (1)]", "[More…]" });
	const visage::PopupMenu* more = submenuOf(*type, PresetBrowser::kMoreSection);
	REQUIRE(more != nullptr);
	CHECK(shape(*more) == std::vector<std::string> { "[s09 (1)]", "[s10 (1)]", "[s11 (1)]" });
	// THE PRESETS AT THE BOTTOM, by value: a's three.
	const visage::PopupMenu* a = submenuOf(*type, "a (3)");
	REQUIRE(a != nullptr);
	CHECK(shape(*a) == std::vector<std::string> { "P1", "P2", "P0" }); // sorted by category then name: "" before "Pads"
	const visage::PopupMenu* tag = submenuOf(menu, "tag");
	REQUIRE(tag != nullptr);
	CHECK(shape(*tag) == std::vector<std::string> { "[dark (1)]" });
}

TEST_CASE("the save prompt completes namespaces: \"mo\" offers \"mood:\", then that namespace's values; a chip reads mood:dark") {
	Rig rig("ns-complete");
	REQUIRE(rig.browser.saveAs("A", { "mood:dark", "type:pad" }));
	REQUIRE(rig.browser.saveAs("B", { "mood:dark", "moody" }));
	REQUIRE(rig.browser.saveAs("C", { "mood:dreamy" }));
	rig.browser.chooseFromMenu(PresetBrowser::kMenuSave);
	REQUIRE(rig.browser.prompt() == PresetBrowser::Prompt::Naming);
	rig.browser.typeTagsForTest(""); // the field pre-fills C's tags (backlog PP); this case is about an empty one
	// NOTHING TYPED: the bank's best tags, namespaced as they are.
	const std::vector<std::string> best = rig.browser.chips();
	REQUIRE_FALSE(best.empty());
	CHECK(best.front() == "mood:dark");
	// "mo": the namespace first, then the bare tag it also starts.
	rig.browser.typeTagsForTest("mo");
	CHECK(rig.browser.chips() == std::vector<std::string> { "mood:", "moody" });
	// TAB TAKES "mood:" AND LEAVES IT OPEN for the value.
	REQUIRE(rig.browser.acceptCompletion());
	CHECK(rig.browser.tagsTextForTest() == "mood:");
	CHECK(rig.browser.chips() == std::vector<std::string> { "mood:dark", "mood:dreamy" });
	rig.browser.typeTagsForTest("mood:dr");
	CHECK(rig.browser.chips() == std::vector<std::string> { "mood:dreamy" });
	REQUIRE(rig.browser.acceptCompletion());
	CHECK(rig.browser.tagsTextForTest() == "mood:dreamy, ");
	// AND A CHIP PRESSED WHILE A NAMESPACE IS OPEN takes the whole tag.
	rig.browser.typeTagsForTest("mood:dreamy, type:");
	CHECK(rig.browser.chips() == std::vector<std::string> { "type:pad" });
	rig.browser.pressChip(0);
	CHECK(rig.browser.tagsTextForTest() == "mood:dreamy, type:pad, ");
	rig.browser.cancelPrompt();
}

//--------------------------------------------------------------------------------------------------//
// A PRESET LOAD CAN BE TAKEN BACK, AND SAYS WHAT IT LOADED (the load stays as it is, with an easy //
// way back and a "loaded <preset name>" message).                                                  //
//--------------------------------------------------------------------------------------------------//

namespace {

/// THE WHOLE PATCH AS BYTES, as the host would hand it to a save — every parameter and the block.
std::vector<uint8_t> capturedPatch(StatefulGuiHost& host) {
	struct Sink {
		std::vector<uint8_t> bytes;
		static int64_t CLAP_ABI write(const clap_ostream_t* self, const void* buffer, uint64_t size) {
			auto* sink = static_cast<Sink*>(self->ctx);
			const auto* b = static_cast<const uint8_t*>(buffer);
			sink->bytes.insert(sink->bytes.end(), b, b + size);
			return static_cast<int64_t>(size);
		}
	} sink;
	clap_ostream_t stream {};
	stream.ctx = &sink;
	stream.write = &Sink::write;
	host.guiCaptureState(&stream);
	return sink.bytes;
}

} // namespace

TEST_CASE("a preset load is ONE undo step that puts the whole patch back — every parameter and every block — "
          "and the browser's face back on the name; redo loads the preset again") {
	const ScopedPresetLibrary library("pt-undo");
	StatefulGuiHost host;
	host.withBlock = true;
	ParamBridge bridge;
	feathers::BridgeDeviceHost deviceHost { bridge };
	host.bridge = &bridge;
	Gui gui(host, bridge);
	REQUIRE(gui.create(CLAP_WINDOW_API_X11, false));
	PresetBrowser* browser = gui.presetBrowserForTest();
	REQUIRE(browser != nullptr);
	ui::EditHistory* history = gui.history();
	REQUIRE(history != nullptr);
	const clap_id zone = static_cast<clap_id>(ParamId::AZone);
	const clap_id level = static_cast<clap_id>(ParamId::VoiceLevel);

	// A SAVED PRESET, P: its own zone, level and block.
	host.adopt(ParamId::AZone, 400.0);
	host.adopt(ParamId::VoiceLevel, 0.25);
	host.block = { 1, 2, 3 };
	gui.tickForTest();
	saveThroughPrompt(*browser);
	const int saved = browser->currentIndex();
	REQUIRE(saved >= 0);
	const std::string savedName = browser->currentName();

	// THE PATCH PLAYING NOW — P, edited: every part of it moved from what P stores.
	host.adopt(ParamId::AZone, 800.0);
	host.adopt(ParamId::VoiceLevel, 0.75);
	host.block = { 9, 9, 9, 9 };
	gui.tickForTest();
	const std::vector<uint8_t> before = capturedPatch(host);
	REQUIRE_FALSE(before.empty());
	const size_t steps = history->length();

	// LOAD INIT BY THE BROWSER — the arrows' own road (`ItemStepper` → `selectIndex`).
	const int init = browser->indexOf("Init");
	REQUIRE(init >= 0);
	REQUIRE(init != saved);
	REQUIRE(browser->selectIndex(init));
	gui.tickForTest();
	const std::vector<uint8_t> loaded = capturedPatch(host);
	REQUIRE(loaded != before); // the load changed the patch, so the undo below has something to undo
	CHECK(host.block != std::vector<uint8_t>({ 9, 9, 9, 9 }));
	CHECK(browser->currentName() == "Init");

	// ONE STEP, and an undo puts back THE EXACT PATCH: every byte the host would save.
	CHECK(history->length() == steps + 1);
	REQUIRE(history->canUndo());
	history->undo();
	gui.tickForTest();
	CHECK(capturedPatch(host) == before);
	CHECK(bridge.value(zone) == doctest::Approx(800.0));
	CHECK(bridge.value(level) == doctest::Approx(0.75));
	CHECK(host.block == std::vector<uint8_t>({ 9, 9, 9, 9 }));
	// AND THE FACE NAMES WHAT IS PLAYING AGAIN — the entry that was current, not Init.
	CHECK(browser->currentName() == savedName);
	// THE UNDO IS NOT ITSELF A NEW STEP.
	CHECK(history->length() == steps + 1);

	// REDO LOADS INIT AGAIN, byte for byte.
	REQUIRE(history->canRedo());
	history->redo();
	gui.tickForTest();
	CHECK(capturedPatch(host) == loaded);
	CHECK(browser->currentName() == "Init");
	CHECK(history->length() == steps + 1);

	// A LOAD THAT FAILS RECORDS NOTHING: there is nothing to go back from.
	host.refuseLoads = true;
	CHECK_FALSE(browser->selectIndex(saved));
	CHECK(history->length() == steps + 1);
}

TEST_CASE("a preset load says \"Loaded: <name>\" — 100 ms at full strength, faded out over 600 ms, then gone; "
          "deaf to the mouse and never focused") {
	const ScopedPresetLibrary library("pt-toast");
	StatefulGuiHost host;
	ParamBridge bridge;
	feathers::BridgeDeviceHost deviceHost { bridge };
	host.bridge = &bridge;
	Gui gui(host, bridge);
	REQUIRE(gui.create(CLAP_WINDOW_API_X11, false));
	int64_t now = 50'000;
	gui.setPresetToastClockForTest([&now] { return now; });
	PresetBrowser* browser = gui.presetBrowserForTest();
	ui::NotifyTray* toast = gui.presetToastForTest();
	REQUIRE(browser != nullptr);
	REQUIRE(toast != nullptr);
	// NOTHING SAID ON OPENING: opening an editor loads nothing.
	CHECK_FALSE(toast->visible());
	CHECK(toast->ignoresMouseEvents());
	CHECK_FALSE(toast->acceptsKeystrokes());

	host.adopt(ParamId::AZone, 400.0);
	gui.tickForTest();
	saveThroughPrompt(*browser);
	const std::string savedName = browser->currentName();
	CHECK_FALSE(toast->visible()); // a SAVE is not a load
	const int init = browser->indexOf("Init");
	REQUIRE(browser->selectIndex(init));
	CHECK(toast->text() == "Loaded: Init");
	CHECK(toast->opacity() == 1.0f);
	// THE TIMING, BY VALUE: held 100 ms, half way through a 600 ms fade at 400 ms, gone at 700 ms.
	CHECK(NotifyTray::kToastTiming.holdMs == 100);
	CHECK(NotifyTray::kToastTiming.fadeMs == 600);
	now += 99;
	CHECK(toast->opacity() == 1.0f);
	now += 1 + 300;
	CHECK(toast->opacity() == doctest::Approx(0.5f).epsilon(0.001));
	now += 299;
	CHECK(toast->opacity() > 0.0f);
	now += 1;
	CHECK(toast->opacity() == 0.0f);
	CHECK_FALSE(toast->visible());
	// THE EDITOR'S OWN READOUT IS UNTOUCHED: the toast is its own widget at its own timing.
	CHECK(gui.notifyTextForTest().find("Loaded:") == std::string::npos);

	// AND AN UNDO OF THE LOAD SAYS WHAT IT WENT BACK TO.
	ui::EditHistory* history = gui.history();
	REQUIRE(history != nullptr);
	REQUIRE(history->canUndo());
	history->undo();
	CHECK(toast->text() == "Loaded: " + savedName);
	CHECK(toast->opacity() == 1.0f);
}

//--------------------------------------------------------------------------------------------------//
// CTRL+Z / CTRL+SHIFT+Z / CTRL+Y ON THE EDITOR'S HISTORY (the preset way back).     //
// Delivered the way visage delivers a key — from the focus up through the parents (`keyFromFocus`) —  //
// and the answer is whether the key was TAKEN, which is what decides whether the host gets it.       //
//--------------------------------------------------------------------------------------------------//

namespace {

visage::KeyEvent chord(visage::KeyCode code, int modifiers) {
	return visage::KeyEvent { code, modifiers, true };
}

/// An editor over a stateful host with a saved preset P loaded and then edited, and Init loaded over
/// that — so the history's tip is a preset load, and `before` is the patch it took away.
struct UndoKeysRig {
	ScopedPresetLibrary library { "pt-keys" };
	StatefulGuiHost host;
	ParamBridge bridge;
	feathers::BridgeDeviceHost deviceHost { bridge };
	std::unique_ptr<Gui> gui;
	PresetBrowser* browser = nullptr;
	ui::EditHistory* history = nullptr;
	std::vector<uint8_t> before;
	std::vector<uint8_t> loaded;
	std::string savedName;

	UndoKeysRig() {
		host.withBlock = true;
		host.bridge = &bridge;
		gui = std::make_unique<Gui>(host, bridge);
		REQUIRE(gui->create(CLAP_WINDOW_API_X11, false));
		browser = gui->presetBrowserForTest();
		history = gui->history();
		REQUIRE(browser != nullptr);
		REQUIRE(history != nullptr);
		host.adopt(ParamId::AZone, 400.0);
		host.block = { 1, 2, 3 };
		gui->tickForTest();
		saveThroughPrompt(*browser);
		savedName = browser->currentName();
		host.adopt(ParamId::AZone, 800.0);
		host.block = { 9, 9 };
		gui->tickForTest();
		before = capturedPatch(host);
	}
	void loadInit() {
		REQUIRE(browser->selectIndex(browser->indexOf("Init")));
		gui->tickForTest();
		loaded = capturedPatch(host);
		REQUIRE(loaded != before);
	}
	bool key(visage::KeyCode code, int modifiers) { return keyFromFocus(gui->root(), chord(code, modifiers)); }
};

} // namespace

TEST_CASE("Ctrl+Z after a preset load puts the whole patch back and is TAKEN; Ctrl+Shift+Z and Ctrl+Y load it "
          "again; the toast names each") {
	UndoKeysRig rig;
	int64_t now = 1'000;
	rig.gui->setPresetToastClockForTest([&now] { return now; });
	rig.loadInit();
	ui::NotifyTray* toast = rig.gui->presetToastForTest();
	REQUIRE(toast != nullptr);

	CHECK(rig.key(visage::KeyCode::Z, visage::kModifierRegCtrl));
	rig.gui->tickForTest();
	CHECK(capturedPatch(rig.host) == rig.before);
	CHECK(rig.bridge.value(static_cast<clap_id>(ParamId::AZone)) == doctest::Approx(800.0));
	CHECK(rig.host.block == std::vector<uint8_t>({ 9, 9 }));
	CHECK(toast->text() == "Loaded: " + rig.savedName);

	CHECK(rig.key(visage::KeyCode::Z, visage::kModifierRegCtrl | visage::kModifierShift));
	rig.gui->tickForTest();
	CHECK(capturedPatch(rig.host) == rig.loaded);
	CHECK(toast->text() == "Loaded: Init");

	// CTRL+Y IS REDO TOO — walked back first, then forward by Y.
	CHECK(rig.key(visage::KeyCode::Z, visage::kModifierRegCtrl));
	CHECK(capturedPatch(rig.host) == rig.before);
	CHECK(rig.key(visage::KeyCode::Y, visage::kModifierRegCtrl));
	CHECK(capturedPatch(rig.host) == rig.loaded);

	// AT THE TIP THERE IS NOTHING TO REDO, so the redo chord goes back to the host untaken.
	CHECK_FALSE(rig.history->canRedo());
	CHECK_FALSE(rig.key(visage::KeyCode::Z, visage::kModifierRegCtrl | visage::kModifierShift));
	// AND A PLAIN Z, OR ALT IN THE CHORD, IS NOT UNDO.
	CHECK_FALSE(rig.key(visage::KeyCode::Z, 0));
	CHECK_FALSE(rig.key(visage::KeyCode::Z, visage::kModifierRegCtrl | visage::kModifierAlt));
	CHECK(capturedPatch(rig.host) == rig.loaded);
}

TEST_CASE("Cmd+Z is the same undo: the main modifier is Cmd on macOS, and the editor asks for the main modifier") {
	// visage maps the platform's command key to `kModifierCmd` (macOS's ⌘); `isMainModifier` is RegCtrl OR
	// Cmd, so the chord is the platform's own. Asserted here by the modifier bit, which is what a Mac
	// delivers — the physical key mapping is visage's and is not testable on this host.
	UndoKeysRig rig;
	rig.loadInit();
	CHECK(rig.key(visage::KeyCode::Z, visage::kModifierCmd));
	CHECK(capturedPatch(rig.host) == rig.before);
	CHECK(rig.key(visage::KeyCode::Z, visage::kModifierCmd | visage::kModifierShift));
	CHECK(capturedPatch(rig.host) == rig.loaded);
}

TEST_CASE("with nothing to undo, Ctrl+Z is NOT taken — it goes back unhandled, so the host's own undo gets it") {
	UndoKeysRig rig;
	// THE RIG'S SAVE AND EDITS RECORD NOTHING ON THE EDITOR'S HISTORY (they are the host's), so it is empty.
	REQUIRE_FALSE(rig.history->canUndo());
	REQUIRE_FALSE(rig.history->canRedo());
	CHECK_FALSE(rig.key(visage::KeyCode::Z, visage::kModifierRegCtrl));
	CHECK_FALSE(rig.key(visage::KeyCode::Z, visage::kModifierRegCtrl | visage::kModifierShift));
	CHECK_FALSE(rig.key(visage::KeyCode::Y, visage::kModifierRegCtrl));
	CHECK(capturedPatch(rig.host) == rig.before);
}

TEST_CASE("in an EDITING text field Ctrl+Z is not the editor's: the history does not move, and the key is not "
          "taken by the editor") {
	UndoKeysRig rig;
	rig.loadInit();
	REQUIRE(rig.history->canUndo());
	ui::TextField field;
	field.setBounds(0, 0, 120, 18);
	rig.gui->root()->addChild(&field);
	field.begin("Macro 1");
	REQUIRE(field.editing());
	// (No assertion on `hasKeyboardFocus`: a headless editor has no window event handler to grant it.
	// The field edits only while it holds the keyboard in a real window, and the editor asks `editing()`.)

	// FROM THE FIELD UP, as visage routes it: the field declines a main-modifier chord (it has no undo),
	// and the root must decline it too because a text entry is editing.
	CHECK_FALSE(keyFromFocus(&field, chord(visage::KeyCode::Z, visage::kModifierRegCtrl)));
	CHECK(capturedPatch(rig.host) == rig.loaded);
	CHECK(rig.history->canUndo());
	CHECK(field.editing());

	// THE EDIT OVER, the same chord is the editor's again.
	field.processKeyPress(visage::KeyEvent { visage::KeyCode::Escape, 0, true });
	REQUIRE_FALSE(field.editing());
	CHECK(rig.key(visage::KeyCode::Z, visage::kModifierRegCtrl));
	CHECK(capturedPatch(rig.host) == rig.before);
	rig.gui->root()->removeChild(&field);
}

//--------------------------------------------------------------------------------------------------//
// BACKLOG PP (2): THE QUICKER SAVE — PRE-FILLED, IN PLACE, AND AS A NAME-VARIANT                    //
//--------------------------------------------------------------------------------------------------//

TEST_CASE("PP: the save prompt opens pre-filled with the name, folder and tags of the preset last LOADED, then "
          "last SAVED; before either, \"User 1\" and no tags") {
	Rig rig("pp-prefill");
	// NOTHING SAVED OR LOADED YET: the selection the bank opened on is not a load.
	rig.browser.chooseFromMenu(PresetBrowser::kMenuSave);
	CHECK(rig.browser.promptTextForTest() == "User 1");
	CHECK(rig.browser.tagsTextForTest().empty());
	rig.browser.cancelPrompt();
	CHECK(rig.browser.lastTouched() == -1);

	rig.save("Pads/Glass", "bright, mood:dark");
	rig.save("Air");
	// LOADED: Glass, from the list.
	REQUIRE(rig.browser.selectIndex(rig.browser.indexOf("Glass", "Pads")));
	rig.browser.chooseFromMenu(PresetBrowser::kMenuSave);
	CHECK(rig.browser.promptTextForTest() == "Pads/Glass");
	CHECK(rig.browser.tagsTextForTest() == "bright, mood:dark, ");
	CHECK(rig.captures == 2); // opening the prompt writes nothing
	rig.browser.cancelPrompt();

	// SAVED: Tape, with its tags — it is now the one the prompt names.
	rig.save("Tape", "lofi");
	rig.browser.chooseFromMenu(PresetBrowser::kMenuSave);
	CHECK(rig.browser.promptTextForTest() == "Tape");
	CHECK(rig.browser.tagsTextForTest() == "lofi, ");
	rig.browser.cancelPrompt();

	// AN UNDO THAT PUT GLASS BACK names it too (`markCurrent`, the editor's preset-load step).
	rig.browser.markCurrent(rig.browser.indexOf("Glass", "Pads"));
	rig.browser.chooseFromMenu(PresetBrowser::kMenuSave);
	CHECK(rig.browser.promptTextForTest() == "Pads/Glass");
	rig.browser.cancelPrompt();
}

TEST_CASE("PP: Enter on the pre-filled name asks ONCE and overwrites in place — the same file, the new sound, "
          "the same tags") {
	Rig rig("pp-enter");
	rig.save("Pads/Glass", "bright");
	REQUIRE(rig.browser.selectIndex(rig.browser.indexOf("Glass", "Pads")));
	rig.payload = 99;
	rig.browser.chooseFromMenu(PresetBrowser::kMenuSave);
	rig.browser.submitPrompt(); // Enter, nothing typed
	REQUIRE(rig.browser.prompt() == PresetBrowser::Prompt::ConfirmReplace);
	rig.browser.resolveReplace(true);
	CHECK(rig.browser.prompt() == PresetBrowser::Prompt::Closed);
	const std::vector<StoredPreset> disk = rig.disk();
	REQUIRE(disk.size() == 2); // Init and Glass: replaced, not added
	CHECK(disk[1].name == "Glass");
	CHECK(disk[1].path == PresetStore::directory("Test") / "Pads" / "Glass.fpreset");
	CHECK(disk[1].bytes == std::vector<uint8_t> { 99 });
	CHECK(disk[1].meta.tags == std::vector<std::string> { "bright" });
}

TEST_CASE("PP: Save \"X\" overwrites the preset last saved or loaded IN PLACE in one gesture, keeping its tags; "
          "disabled with nothing to save over") {
	Rig rig("pp-inplace");
	const auto row = [&rig](int id) {
		for (const ItemStepper::Row& r : rig.browser.listRows()) {
			if (r.id == id) {
				return r;
			}
		}
		FAIL("no row " << id);
		return ItemStepper::Row {};
	};
	CHECK_FALSE(row(PresetBrowser::kMenuOverwrite).enabled); // nothing saved or loaded yet
	CHECK_FALSE(row(PresetBrowser::kMenuSaveVariant).enabled);

	rig.save("Pads/Glass", "bright");
	rig.save("Air");
	REQUIRE(rig.browser.selectIndex(rig.browser.indexOf("Glass", "Pads")));
	CHECK(row(PresetBrowser::kMenuOverwrite).label == "Save \"Glass\"");
	CHECK(row(PresetBrowser::kMenuOverwrite).enabled);
	rig.payload = 55;
	rig.browser.clock = [] { return int64_t { 1'900'000'000 }; };
	rig.browser.chooseFromMenu(PresetBrowser::kMenuOverwrite);
	CHECK(rig.browser.prompt() == PresetBrowser::Prompt::Closed); // no prompt, no question
	const std::vector<StoredPreset> disk = rig.disk();
	REQUIRE(disk.size() == 3);
	const auto glass = std::find_if(disk.begin(), disk.end(), [](const StoredPreset& p) { return p.name == "Glass"; });
	REQUIRE(glass != disk.end());
	CHECK(glass->bytes == std::vector<uint8_t> { 55 });
	CHECK(glass->meta.tags == std::vector<std::string> { "bright" });
	CHECK(glass->meta.saved == 1'900'000'000);
	const auto air = std::find_if(disk.begin(), disk.end(), [](const StoredPreset& p) { return p.name == "Air"; });
	REQUIRE(air != disk.end());
	CHECK(air->bytes == std::vector<uint8_t> { 7 }); // the other one untouched
	CHECK(rig.browser.currentName() == "Glass");
}

TEST_CASE("PP: Save as \"X 2\" — the next free suffix, the same tags, the same folder; a variant of a variant "
          "counts on") {
	Rig rig("pp-variant");
	rig.save("Pads/Glass", "bright, mood:dark");
	REQUIRE(rig.browser.selectIndex(rig.browser.indexOf("Glass", "Pads")));
	CHECK(rig.browser.variantName(rig.browser.lastTouched()) == "Glass 2");
	rig.payload = 21;
	rig.browser.chooseFromMenu(PresetBrowser::kMenuSaveVariant);
	CHECK(rig.browser.prompt() == PresetBrowser::Prompt::Closed);
	{
		const int at = rig.browser.indexOf("Glass 2", "Pads");
		REQUIRE(at >= 0);
		CHECK(rig.browser.currentIndex() == at);
		CHECK(rig.browser.lastTouched() == at);
		const std::vector<StoredPreset> disk = rig.disk();
		const auto it = std::find_if(disk.begin(), disk.end(), [](const StoredPreset& p) { return p.name == "Glass 2"; });
		REQUIRE(it != disk.end());
		const StoredPreset& stored = *it;
		CHECK(stored.path == PresetStore::directory("Test") / "Pads" / "Glass 2.fpreset");
		CHECK(stored.bytes == std::vector<uint8_t> { 21 });
		CHECK(stored.meta.tags == std::vector<std::string> { "bright", "mood:dark" });
	}
	// FROM "Glass 2", the next is "Glass 3" — not "Glass 2 2"; and a taken one is skipped.
	rig.save("Pads/Glass 3");
	REQUIRE(rig.browser.selectIndex(rig.browser.indexOf("Glass 2", "Pads")));
	CHECK(rig.browser.variantName(rig.browser.lastTouched()) == "Glass 4");
	REQUIRE(rig.browser.saveVariant(rig.browser.lastTouched()));
	CHECK(rig.browser.indexOf("Glass 4", "Pads") >= 0);
	CHECK(rig.browser.indexOf("Glass 2 2", "Pads") < 0);
	// A NAME ENDING IN DIGITS WITHOUT A SPACE IS A NAME, NOT A SUFFIX.
	rig.save("TB303");
	CHECK(rig.browser.variantName(rig.browser.lastTouched()) == "TB303 2");
	// THE ROW NAMES IT before it is chosen.
	bool found = false;
	for (const ItemStepper::Row& r : rig.browser.listRows()) {
		if (r.id == PresetBrowser::kMenuSaveVariant) {
			found = true;
			CHECK(r.label == "Save as \"TB303 2\"");
			CHECK(r.enabled);
		}
	}
	CHECK(found);
}

TEST_CASE("PP: the preset last saved or loaded is followed through a rename and forgotten on delete") {
	Rig rig("pp-follow");
	rig.save("Pads/Glass", "bright");
	rig.save("Air");
	REQUIRE(rig.browser.selectIndex(rig.browser.indexOf("Glass", "Pads")));
	REQUIRE(rig.browser.editEntry(rig.browser.indexOf("Glass", "Pads"), "Keys/Crystal", { "bright" }));
	CHECK(rig.browser.lastTouched() == rig.browser.indexOf("Crystal", "Keys"));
	CHECK(rig.browser.currentName() == "Crystal");
	rig.browser.chooseFromMenu(PresetBrowser::kMenuSave);
	CHECK(rig.browser.promptTextForTest() == "Keys/Crystal");
	rig.browser.cancelPrompt();
	REQUIRE(rig.browser.deleteEntry(rig.browser.indexOf("Crystal", "Keys")));
	CHECK(rig.browser.lastTouched() == -1);
	rig.browser.chooseFromMenu(PresetBrowser::kMenuSave);
	CHECK(rig.browser.promptTextForTest() == "User 1");
	rig.browser.cancelPrompt();
}

TEST_CASE("PP: a rename of ANOTHER preset leaves the current one current — the sort moves every index") {
	Rig rig("pp-sort");
	rig.save("Bass");
	rig.save("Zed");
	REQUIRE(rig.browser.selectIndex(rig.browser.indexOf("Zed")));
	// "Bass" renamed to "Zz" sorts after Zed: Zed's index moves down one.
	REQUIRE(rig.browser.editEntry(rig.browser.indexOf("Bass"), "Zz", {}));
	CHECK(rig.browser.currentName() == "Zed");
}

TEST_CASE("PP: Save \"X\" is never offered for a FACTORY preset — the computed Init or a shipped file — and Save as… "
          "and the name-variant still are") {
	Rig rig("pp-factory", kPluginBank);
	rig.browser.factoryFiles = testFactoryPresetFiles(); // Bright and Pads/Glass, shipped as files
	rig.browser.loadBankFromDisk();
	const auto row = [&rig](int id) {
		for (const ItemStepper::Row& r : rig.browser.listRows()) {
			if (r.id == id) {
				return r;
			}
		}
		FAIL("no row " << id);
		return ItemStepper::Row {};
	};
	const int init = rig.browser.indexOf("Init");
	const int bright = rig.browser.indexOf("Bright"); // a shipped file (the fixture factory set), on disk
	REQUIRE(init >= 0);
	REQUIRE(bright >= 0);
	REQUIRE_FALSE(rig.browser.isReadOnly(bright)); // a real file: only "factory" can be what refuses it
	CHECK(rig.browser.isFactory(init));
	CHECK(rig.browser.isFactory(bright));
	CHECK(rig.browser.isFactory(rig.browser.indexOf("Glass", "Pads")));

	for (const int at : { init, rig.browser.indexOf("Bright") }) {
		REQUIRE(rig.browser.selectIndex(at));
		const std::string name = rig.browser.currentName();
		CAPTURE(name);
		CHECK_FALSE(row(PresetBrowser::kMenuOverwrite).enabled);
		CHECK(row(PresetBrowser::kMenuOverwrite).label == "Save");
		CHECK(row(PresetBrowser::kMenuSave).enabled);
		CHECK(row(PresetBrowser::kMenuSaveVariant).enabled);
		CHECK(row(PresetBrowser::kMenuSaveVariant).label == "Save as \"" + name + " 2\"");
		// AND THE ROW'S ID, chosen anyway, writes nothing.
		const int before = rig.captures;
		rig.browser.chooseFromMenu(PresetBrowser::kMenuOverwrite);
		CHECK(rig.captures == before);
	}
	CHECK(fileBytes(PresetStore::directory(kPluginBank) / "Bright.fpreset") == bytesOf("fixture payload: Bright\n"));

	// A USER'S OWN PRESET still has it.
	rig.save("Mine");
	CHECK_FALSE(rig.browser.isFactory(rig.browser.indexOf("Mine")));
	CHECK(row(PresetBrowser::kMenuOverwrite).enabled);
	CHECK(row(PresetBrowser::kMenuOverwrite).label == "Save \"Mine\"");
}

//--------------------------------------------------------------------------------------------------//
// BACKLOG PP (1): THE VARIANT CHIPS — "<base>_variant_<n>" FILES BESIDE THE NAME                     //
//--------------------------------------------------------------------------------------------------//

TEST_CASE("PP: the variant chips sit in 2 rows of 4 beside the name, inside the bar, none overlapping, tall enough to read") {
	for (const ItemStepper::Style style : { PresetBrowser::kHeaderStyle, PresetBrowser::kPanelStyle }) {
		CAPTURE(style.height);
		PresetBrowser browser(std::vector<std::string> { "Init" }, std::vector<PresetData> { PresetData { { 1 } } },
		                      "Test", style);
		browser.setBounds(0.0f, 0.0f, browser.barWidth(), style.height);
		const float stepperRight = style.arrowWidth * 2.0f + style.nameWidth;
		std::vector<visage::Bounds> chips;
		for (int n = 1; n <= PresetBrowser::kVariants; ++n) {
			CAPTURE(n);
			const visage::Bounds b = browser.variantChipBounds(n);
			chips.push_back(b);
			CHECK(b.x() >= stepperRight); // right of ▶
			CHECK(b.right() <= browser.barWidth() + 0.001f);
			CHECK(b.y() >= 0.0f);
			CHECK(b.bottom() <= style.height + 0.001f);
			CHECK(b.height() >= 9.0f); // a digit's room
			CHECK(b.width() > b.height()); // wider than tall: the number has side room
			CHECK(browser.variantChipAt({ b.x() + b.width() * 0.5f, b.y() + b.height() * 0.5f }) == n);
		}
		// TWO ROWS OF FOUR: one top per row, one left per column.
		for (int i = 0; i < 8; ++i) {
			CHECK(chips[i].y() == chips[i < 4 ? 0 : 4].y());
			CHECK(chips[i].x() == chips[i % 4].x());
		}
		CHECK(chips[4].y() > chips[0].y());
		for (int i = 0; i < 8; ++i) {
			for (int j = i + 1; j < 8; ++j) {
				CHECK_FALSE(chips[i].overlaps(chips[j]));
			}
		}
		CHECK(browser.variantChipAt({ stepperRight - 1.0f, style.height * 0.5f }) == 0);
		CHECK(PresetBrowser::barWidthFor(style) == browser.barWidth());
	}
}

TEST_CASE("PP: a chip's state is the files on disk — a click on a grey chip saves <base>_variant_<n> with the base's "
          "tags in its folder; a click on a saved one loads it; a variant keeps its base's chips") {
	Rig rig("pp-chips");
	rig.save("Pads/Glass", "bright, mood:dark");
	REQUIRE(rig.browser.selectIndex(rig.browser.indexOf("Glass", "Pads")));
	const std::filesystem::path folder = PresetStore::directory("Test") / "Pads";
	const auto onDisk = [&folder](int n) {
		return std::filesystem::exists(folder / (PresetBrowser::variantFileName("Glass", n) + ".fpreset"));
	};
	const auto statesMatchDisk = [&] {
		for (int n = 1; n <= PresetBrowser::kVariants; ++n) {
			CAPTURE(n);
			CHECK((rig.browser.chipState(n) != PresetBrowser::ChipState::Unused) == onDisk(n));
		}
	};
	// CHIP 1 IS THE ROOT: Glass itself, shown, so ACTIVE; 2..8 nothing yet.
	CHECK(rig.browser.chipState(1) == PresetBrowser::ChipState::Active);
	CHECK(rig.browser.variantIndex(1) == rig.browser.indexOf("Glass", "Pads"));
	for (int n = 2; n <= 8; ++n) {
		CHECK(rig.browser.chipState(n) == PresetBrowser::ChipState::Unused);
	}
	statesMatchDisk();

	// A GREY CHIP: saved, no prompt, under the right name, with the base's tags, in the base's folder.
	rig.payload = 33;
	REQUIRE(rig.browser.pressVariant(3));
	CHECK(rig.browser.prompt() == PresetBrowser::Prompt::Closed);
	REQUIRE(onDisk(3));
	{
		const std::vector<StoredPreset> disk = rig.disk();
		const auto it = std::find_if(disk.begin(), disk.end(), [](const StoredPreset& p) { return p.name == "Glass_variant_3"; });
		REQUIRE(it != disk.end());
		CHECK(it->category == "Pads");
		CHECK(it->bytes == std::vector<uint8_t> { 33 });
		CHECK(it->meta.tags == std::vector<std::string> { "bright", "mood:dark" });
	}
	CHECK(rig.browser.currentName() == "Glass_variant_3");
	CHECK(rig.browser.chipState(3) == PresetBrowser::ChipState::Active);
	statesMatchDisk();

	// FROM THE VARIANT, ITS BASE'S CHIPS: chip 5 is Glass_variant_5, never Glass_variant_3_variant_5.
	rig.payload = 55;
	REQUIRE(rig.browser.pressVariant(5));
	CHECK(onDisk(5));
	CHECK(rig.browser.indexOf("Glass_variant_3_variant_5", "Pads") < 0);
	CHECK(rig.browser.chipState(5) == PresetBrowser::ChipState::Active);
	CHECK(rig.browser.chipState(3) == PresetBrowser::ChipState::Saved);
	statesMatchDisk();

	// BACK ON THE BASE: both saved, neither active.
	REQUIRE(rig.browser.selectIndex(rig.browser.indexOf("Glass", "Pads")));
	CHECK(rig.browser.chipState(3) == PresetBrowser::ChipState::Saved);
	CHECK(rig.browser.chipState(5) == PresetBrowser::ChipState::Saved);

	// A SAVED CHIP: LOADED — the provider is handed that file's bytes, and nothing is written.
	const int captures = rig.captures;
	const size_t applied = rig.applied.size();
	REQUIRE(rig.browser.pressVariant(3));
	REQUIRE(rig.applied.size() == applied + 1);
	CHECK(rig.applied.back() == std::vector<uint8_t> { 33 });
	CHECK(rig.captures == captures);
	CHECK(rig.browser.currentName() == "Glass_variant_3");
	CHECK(rig.browser.chipState(3) == PresetBrowser::ChipState::Active);

	// THE BASE NAME, BY ITS SUFFIX: only _variant_2.._variant_8 is a variant; "_variant_1" (the old scheme) is a name.
	CHECK(PresetBrowser::variantBase("Glass_variant_1") == "Glass_variant_1");
	CHECK(PresetBrowser::variantBase("Glass_variant_8") == "Glass");
	CHECK(PresetBrowser::variantBase("Glass_variant_9") == "Glass_variant_9");
	CHECK(PresetBrowser::variantBase("Glass_variant_") == "Glass_variant_");
	CHECK(PresetBrowser::variantBase("Glass") == "Glass");
}

TEST_CASE("PP: a chip's context menu — Overwrite replaces that file in place, keeping its tags; Rename opens the prompt "
          "over it; Delete removes it") {
	Rig rig("pp-chip-menu");
	rig.save("Lead", "mono");
	REQUIRE(rig.browser.selectIndex(rig.browser.indexOf("Lead")));
	using Verb = PresetBrowser::VariantVerb;
	CHECK(rig.browser.variantVerbs(2) == std::vector<Verb> { Verb::Save });
	CHECK(PresetBrowser::variantVerbLabel(Verb::Overwrite, 2) == "Overwrite variant 2");
	rig.payload = 20;
	REQUIRE(rig.browser.pressVariant(2));
	REQUIRE(rig.browser.selectIndex(rig.browser.indexOf("Lead")));
	CHECK(rig.browser.variantVerbs(2) == std::vector<Verb> { Verb::Load, Verb::Overwrite, Verb::Promote, Verb::Delete, Verb::ClearOthers }); // and Clear, now there is a 2
	// A CLICK on a saved chip never overwrites it: only the menu's Overwrite does.
	CHECK_FALSE(rig.browser.actVariant(2, Verb::Save));

	const std::filesystem::path file = PresetStore::directory("Test") / "Lead_variant_2.fpreset";
	const size_t count = rig.disk().size();
	rig.payload = 21;
	rig.browser.clock = [] { return int64_t { 1'900'000'000 }; };
	REQUIRE(rig.browser.actVariant(2, Verb::Overwrite));
	{
		const std::vector<StoredPreset> disk = rig.disk();
		CHECK(disk.size() == count); // in place
		const auto it = std::find_if(disk.begin(), disk.end(), [](const StoredPreset& p) { return p.name == "Lead_variant_2"; });
		REQUIRE(it != disk.end());
		CHECK(it->path == file);
		CHECK(it->bytes == std::vector<uint8_t> { 21 });
		CHECK(it->meta.tags == std::vector<std::string> { "mono" });
		CHECK(it->meta.saved == 1'900'000'000);
	}
	CHECK(rig.browser.chipState(2) == PresetBrowser::ChipState::Active);

	// RENAME TO A FULL PRESET: the rename prompt over that file.
	REQUIRE(rig.browser.actVariant(2, Verb::Promote));
	CHECK(rig.browser.prompt() == PresetBrowser::Prompt::Naming);
	CHECK(rig.browser.promptKind() == PresetBrowser::PromptKind::Edit);
	CHECK(rig.browser.promptTextForTest() == "Lead_variant_2");
	rig.browser.cancelPrompt();

	// DELETE: the file goes and the chip is grey again.
	REQUIRE(rig.browser.selectIndex(rig.browser.indexOf("Lead")));
	REQUIRE(rig.browser.actVariant(2, Verb::Delete));
	CHECK_FALSE(std::filesystem::exists(file));
	CHECK(rig.browser.chipState(2) == PresetBrowser::ChipState::Unused);
}

TEST_CASE("PP: the chips as drawn — the active one in the accent, a saved one in another colour, an unused one grey") {
	Rig rig("pp-chip-capture");
	rig.save("Keys");
	REQUIRE(rig.browser.selectIndex(rig.browser.indexOf("Keys")));
	REQUIRE(rig.browser.pressVariant(5)); // saved, then shown: ACTIVE
	REQUIRE(rig.browser.selectIndex(rig.browser.indexOf("Keys")));
	REQUIRE(rig.browser.pressVariant(2)); // saved and shown: 2 ACTIVE, 5 SAVED
	REQUIRE(rig.browser.chipState(2) == PresetBrowser::ChipState::Active);
	REQUIRE(rig.browser.chipState(5) == PresetBrowser::ChipState::Saved);

	const int kW = static_cast<int>(std::ceil(rig.browser.barWidth()));
	const int kH = static_cast<int>(PresetBrowser::kHeight);
	visage::Canvas canvas;
	markedWindowless(canvas, kW, kH);
	canvas.setColor(0xff000000);
	canvas.fill(0.0f, 0.0f, static_cast<float>(kW), static_cast<float>(kH));
	visage::Region region;
	region.setBounds(0, 0, kW, kH);
	canvas.addRegion(&region);
	canvas.beginRegion(&region);
	rig.browser.draw(canvas);
	canvas.endRegion();
	const visage::Screenshot shot = takeMarkedCapture(canvas, kW, kH);
	REQUIRE(shot.width() == kW);
	if (const char* dir = std::getenv("FEATHERS_CAPTURE_DIR"); dir != nullptr) {
		shot.save((std::filesystem::path(dir) / "pp-variant-chips.png").string().c_str());
	}
	// A CHIP'S GROUND, sampled clear of its digit (the left quarter, mid-height) and of its rounded corner.
	const auto ground = [&](int n) {
		const visage::Bounds b = rig.browser.variantChipBounds(n);
		const visage::Color c = shot.sample(static_cast<int>(b.x() + 2.5f), static_cast<int>(b.y() + b.height() * 0.5f));
		return std::array<int, 3> { static_cast<int>(c.hexRed()), static_cast<int>(c.hexGreen()), static_cast<int>(c.hexBlue()) };
	};
	const auto rgb = [](uint32_t argb) {
		return std::array<int, 3> { static_cast<int>((argb >> 16) & 0xff), static_cast<int>((argb >> 8) & 0xff),
			                        static_cast<int>(argb & 0xff) };
	};
	const auto near = [](std::array<int, 3> a, std::array<int, 3> b) {
		return std::abs(a[0] - b[0]) <= 3 && std::abs(a[1] - b[1]) <= 3 && std::abs(a[2] - b[2]) <= 3;
	};
	CAPTURE(ground(2)[0]);
	CHECK(near(ground(2), rgb(kAccent)));  // active: bright
	CHECK(near(ground(3), rgb(kSurface))); // unused: grey
	CHECK_FALSE(near(ground(1), rgb(kSurface))); // chip 1 is the root, Keys, saved
	CHECK(near(ground(8), rgb(kSurface)));
	CHECK_FALSE(near(ground(5), rgb(kSurface))); // saved: neither grey...
	CHECK_FALSE(near(ground(5), rgb(kAccent)));  // ...nor the active colour
}

//--------------------------------------------------------------------------------------------------//
// OVERWRITE, BACK IN MANAGE ("lets bring back 'overwrite' to the manage preset menu even though //
// it kinda duplicates save as->enter without changes")                                             //
//--------------------------------------------------------------------------------------------------//

namespace {

/// The row with `id`, or none — Manage's rows included (`listRows` flattens the submenus, `section` naming each).
std::optional<ItemStepper::Row> rowWithId(PresetBrowser& browser, int id) {
	for (const ItemStepper::Row& r : browser.listRows()) {
		if (r.id == id) {
			return r;
		}
	}
	return std::nullopt;
}

} // namespace

TEST_CASE("Manage ▸ Overwrite \"X\": writes the loaded preset's file with the live state, keeping its name, folder and tags") {
	Rig rig("ow-manage");
	rig.save("Pads/Glass", "bright, mood:dark");
	rig.save("Air");
	REQUIRE(rig.browser.selectIndex(rig.browser.indexOf("Glass", "Pads")));
	const std::optional<ItemStepper::Row> row = rowWithId(rig.browser, PresetBrowser::kMenuManageOverwrite);
	REQUIRE(row.has_value());
	CHECK(row->section == PresetBrowser::kManageMenu);
	CHECK(row->label == "Overwrite \"Glass\"");
	CHECK(row->enabled);

	rig.payload = 55;
	rig.browser.clock = [] { return int64_t { 1'900'000'000 }; };
	rig.browser.chooseFromMenu(PresetBrowser::kMenuManageOverwrite);
	CHECK(rig.browser.prompt() == PresetBrowser::Prompt::Closed); // no prompt: the row named it
	const std::vector<StoredPreset> disk = rig.disk();
	REQUIRE(disk.size() == 3); // Init, Air, Glass — replaced, not added
	const auto glass = std::find_if(disk.begin(), disk.end(), [](const StoredPreset& p) { return p.name == "Glass"; });
	REQUIRE(glass != disk.end());
	CHECK(glass->path == PresetStore::directory("Test") / "Pads" / "Glass.fpreset");
	CHECK(glass->bytes == std::vector<uint8_t> { 55 });
	CHECK(glass->meta.tags == std::vector<std::string> { "bright", "mood:dark" });
	CHECK(glass->meta.saved == 1'900'000'000);
	const auto air = std::find_if(disk.begin(), disk.end(), [](const StoredPreset& p) { return p.name == "Air"; });
	REQUIRE(air != disk.end());
	CHECK(air->bytes == std::vector<uint8_t> { 7 }); // the other one untouched
	CHECK(rig.browser.currentName() == "Glass");
}

TEST_CASE("Manage ▸ Overwrite writes the SAME FILE, byte for byte, as Save as… then Enter with nothing changed") {
	std::vector<uint8_t> viaManage;
	std::vector<uint8_t> viaPrompt;
	for (const bool manage : { true, false }) {
		CAPTURE(manage);
		Rig rig(manage ? "ow-bytes-a" : "ow-bytes-b");
		rig.browser.clock = [] { return int64_t { 1'800'000'000 }; };
		rig.save("Pads/Glass", "bright, mood:dark");
		REQUIRE(rig.browser.selectIndex(rig.browser.indexOf("Glass", "Pads")));
		rig.payload = 42;
		rig.browser.clock = [] { return int64_t { 1'900'000'000 }; };
		if (manage) {
			rig.browser.chooseFromMenu(PresetBrowser::kMenuManageOverwrite);
		}
		else {
			rig.browser.chooseFromMenu(PresetBrowser::kMenuSave);
			rig.browser.submitPrompt(); // Enter, nothing typed
			REQUIRE(rig.browser.prompt() == PresetBrowser::Prompt::ConfirmReplace);
			rig.browser.resolveReplace(true);
		}
		REQUIRE(rig.browser.prompt() == PresetBrowser::Prompt::Closed);
		(manage ? viaManage : viaPrompt) = fileBytes(PresetStore::directory("Test") / "Pads" / "Glass.fpreset");
	}
	REQUIRE_FALSE(viaManage.empty());
	CHECK(viaManage == viaPrompt);
}

TEST_CASE("Manage ▸ Overwrite is REFUSED over a factory preset — shown disabled, saying why — and writes nothing if chosen") {
	Rig rig("ow-factory", kPluginBank);
	rig.browser.factoryFiles = testFactoryPresetFiles(); // Bright and Pads/Glass, shipped as files
	rig.browser.loadBankFromDisk();
	const int bright = rig.browser.indexOf("Bright");
	REQUIRE(bright >= 0);
	REQUIRE(rig.browser.isFactory(bright));
	REQUIRE(rig.browser.selectIndex(bright)); // LOADED: so the row is there, and must refuse
	const std::optional<ItemStepper::Row> row = rowWithId(rig.browser, PresetBrowser::kMenuManageOverwrite);
	REQUIRE(row.has_value());
	CHECK_FALSE(row->enabled);
	CHECK(row->label == "Overwrite \"Bright\" \xe2\x80\x94 factory preset, use Save as\xe2\x80\xa6");
	const int before = rig.captures;
	rig.browser.chooseFromMenu(PresetBrowser::kMenuManageOverwrite);
	CHECK(rig.captures == before);
	CHECK(fileBytes(PresetStore::directory(kPluginBank) / "Bright.fpreset") == bytesOf("fixture payload: Bright\n"));
	// A USER'S OWN PRESET, saved after, has it enabled.
	rig.save("Mine");
	const std::optional<ItemStepper::Row> mine = rowWithId(rig.browser, PresetBrowser::kMenuManageOverwrite);
	REQUIRE(mine.has_value());
	CHECK(mine->enabled);
	CHECK(mine->label == "Overwrite \"Mine\"");
}

TEST_CASE("Manage ▸ Overwrite is not offered with no preset saved or loaded") {
	Rig rig("ow-none");
	REQUIRE(rig.browser.lastTouched() == -1);
	CHECK_FALSE(rowWithId(rig.browser, PresetBrowser::kMenuManageOverwrite).has_value());
	// AND THE ID, chosen anyway, writes nothing.
	const int before = rig.captures;
	rig.browser.chooseFromMenu(PresetBrowser::kMenuManageOverwrite);
	CHECK(rig.captures == before);
	// A DELETE OF THE LOADED ONE takes the row away again.
	rig.save("Gone");
	CHECK(rowWithId(rig.browser, PresetBrowser::kMenuManageOverwrite).has_value());
	REQUIRE(rig.browser.deleteEntry(rig.browser.indexOf("Gone")));
	CHECK_FALSE(rowWithId(rig.browser, PresetBrowser::kMenuManageOverwrite).has_value());
}

//--------------------------------------------------------------------------------------------------//
// THE ROOT IS VARIANT 1; CLEAR VARIANTS 2–8; THE LIST SHOWS EACH PRESET ONCE    //
//--------------------------------------------------------------------------------------------------//

TEST_CASE("PP root: chip 1 IS the root preset — loading it gives the root's state, saving variant 1 overwrites the root's file, 2..8 map to <base>_variant_<n>") {
	Rig rig("root-v1");
	rig.payload = 10;
	rig.save("Pads/Glass", "bright");
	REQUIRE(rig.browser.selectIndex(rig.browser.indexOf("Glass", "Pads")));
	// THE NAMES, BY VALUE.
	CHECK(PresetBrowser::variantFileName("Glass", 1) == "Glass");
	for (int n = 2; n <= PresetBrowser::kVariants; ++n) {
		CHECK(PresetBrowser::variantFileName("Glass", n) == "Glass_variant_" + std::to_string(n));
	}
	// 2..8 SAVED FROM THE ROOT, each its own file and its own bytes.
	for (int n = 2; n <= PresetBrowser::kVariants; ++n) {
		REQUIRE(rig.browser.selectIndex(rig.browser.indexOf("Glass", "Pads")));
		rig.payload = static_cast<uint8_t>(20 + n);
		REQUIRE(rig.browser.pressVariant(n));
	}
	const std::filesystem::path folder = PresetStore::directory("Test") / "Pads";
	for (int n = 2; n <= PresetBrowser::kVariants; ++n) {
		CAPTURE(n);
		CHECK(fileBytes(folder / ("Glass_variant_" + std::to_string(n) + ".fpreset")).size() > 0);
		CHECK(rig.browser.variantIndex(n) == rig.browser.indexOf("Glass_variant_" + std::to_string(n), "Pads"));
	}
	CHECK_FALSE(std::filesystem::exists(folder / "Glass_variant_1.fpreset")); // variant 1 is never a file of its own

	// FROM VARIANT 5, CHIP 1 LOADS THE ROOT: the provider is handed the root's bytes.
	REQUIRE(rig.browser.pressVariant(5));
	CHECK(rig.applied.back() == std::vector<uint8_t> { 25 });
	REQUIRE(rig.browser.pressVariant(1));
	CHECK(rig.applied.back() == std::vector<uint8_t> { 10 });
	CHECK(rig.browser.currentName() == "Glass");
	CHECK(rig.browser.chipState(1) == PresetBrowser::ChipState::Active);

	// SAVING VARIANT 1 (its menu's Overwrite) WRITES THE ROOT'S FILE, in place, keeping its tags.
	using Verb = PresetBrowser::VariantVerb;
	REQUIRE(rig.browser.pressVariant(5));
	const auto v1 = rig.browser.variantVerbs(1);
	CHECK(v1 == std::vector<Verb> { Verb::Load, Verb::Overwrite, Verb::ClearOthers }); // never Delete or Rename the root from a chip
	rig.payload = 77;
	const size_t files = rig.disk().size();
	REQUIRE(rig.browser.actVariant(1, Verb::Overwrite));
	CHECK(rig.disk().size() == files);
	const std::vector<StoredPreset> disk = rig.disk();
	const auto root = std::find_if(disk.begin(), disk.end(), [](const StoredPreset& p) { return p.name == "Glass"; });
	REQUIRE(root != disk.end());
	CHECK(root->path == folder / "Glass.fpreset");
	CHECK(root->bytes == std::vector<uint8_t> { 77 });
	CHECK(root->meta.tags == std::vector<std::string> { "bright" });
	CHECK(rig.browser.currentName() == "Glass");
	CHECK(fileBytes(folder / "Glass_variant_5.fpreset") != fileBytes(folder / "Glass.fpreset")); // 5 untouched
	// AND MANAGE'S OVERWRITE, ON THE ROOT, IS THE SAME ROW IT WAS.
	const std::optional<ItemStepper::Row> row = rowWithId(rig.browser, PresetBrowser::kMenuManageOverwrite);
	REQUIRE(row.has_value());
	CHECK(row->label == "Overwrite \"Glass\"");
	CHECK(row->enabled);
}

TEST_CASE("PP root: over a FACTORY root, chip 1 loads but never overwrites — the factory guard") {
	Rig rig("root-factory", kPluginBank);
	rig.browser.factoryFiles = testFactoryPresetFiles();
	rig.browser.loadBankFromDisk();
	REQUIRE(rig.browser.selectIndex(rig.browser.indexOf("Bright")));
	REQUIRE(rig.browser.isFactory(rig.browser.variantIndex(1)));
	using Verb = PresetBrowser::VariantVerb;
	CHECK(rig.browser.variantVerbs(1) == std::vector<Verb> { Verb::Load });
	const int before = rig.captures;
	CHECK_FALSE(rig.browser.actVariant(1, Verb::Overwrite));
	CHECK(rig.captures == before);
	CHECK(fileBytes(PresetStore::directory(kPluginBank) / "Bright.fpreset") == bytesOf("fixture payload: Bright\n"));
	const std::optional<ItemStepper::Row> row = rowWithId(rig.browser, PresetBrowser::kMenuManageOverwrite);
	REQUIRE(row.has_value());
	CHECK_FALSE(row->enabled);
}

TEST_CASE("PP clear: \"Clear variants 2–8…\" asks first, then removes exactly the variants 2..8 and keeps the root") {
	Rig rig("clear-v");
	rig.payload = 10;
	rig.save("Pads/Glass", "bright");
	rig.save("Pads/Glass 2"); // a NAME-variant, not a chip variant: never touched
	REQUIRE(rig.browser.selectIndex(rig.browser.indexOf("Glass", "Pads")));
	using Verb = PresetBrowser::VariantVerb;
	// NOTHING TO CLEAR: not offered on any chip.
	for (int n = 1; n <= PresetBrowser::kVariants; ++n) {
		const auto verbs = rig.browser.variantVerbs(n);
		CHECK(std::find(verbs.begin(), verbs.end(), Verb::ClearOthers) == verbs.end());
	}
	CHECK_FALSE(rig.browser.actVariant(1, Verb::ClearOthers));
	CHECK_FALSE(rig.browser.clearVariantsPending());

	for (const int n : { 2, 5, 8 }) {
		REQUIRE(rig.browser.selectIndex(rig.browser.indexOf("Glass", "Pads")));
		REQUIRE(rig.browser.pressVariant(n));
	}
	CHECK(PresetBrowser::variantVerbLabel(Verb::ClearOthers, 1) == "Clear variants 2\xe2\x80\x93" "8\xe2\x80\xa6");
	const auto verbs = rig.browser.variantVerbs(3); // an unused chip offers it too
	CHECK(verbs == std::vector<Verb> { Verb::Save, Verb::ClearOthers });
	CHECK(rig.browser.clearableVariants().size() == 3);

	// ASKS: the menu arms the question; Cancel changes nothing.
	const std::filesystem::path folder = PresetStore::directory("Test") / "Pads";
	REQUIRE(rig.browser.actVariant(3, Verb::ClearOthers));
	CHECK(rig.browser.clearVariantsPending());
	rig.browser.resolveClearVariants(false);
	CHECK_FALSE(rig.browser.clearVariantsPending());
	CHECK(std::filesystem::exists(folder / "Glass_variant_5.fpreset"));

	// CONFIRMED, while SHOWING variant 8: 2, 5 and 8 go; the root and the name-variant stay; the root is shown.
	REQUIRE(rig.browser.actVariant(3, Verb::ClearOthers));
	rig.browser.resolveClearVariants(true);
	for (int n = 2; n <= PresetBrowser::kVariants; ++n) {
		CAPTURE(n);
		CHECK_FALSE(std::filesystem::exists(folder / ("Glass_variant_" + std::to_string(n) + ".fpreset")));
		CHECK(rig.browser.chipState(n) == PresetBrowser::ChipState::Unused);
	}
	CHECK(fileBytes(folder / "Glass.fpreset").size() > 0);
	CHECK(std::filesystem::exists(folder / "Glass 2.fpreset"));
	CHECK(rig.browser.currentName() == "Glass");
	CHECK(rig.browser.chipState(1) == PresetBrowser::ChipState::Active);
	CHECK(rig.disk().size() == 3); // Init, Glass, Glass 2
}

TEST_CASE("PP clear: the factory guard — a factory file is never cleared, and a factory root keeps its user variants clearable") {
	Rig rig("clear-factory", kPluginBank);
	rig.browser.factoryFiles = testFactoryPresetFiles(); // Bright and Pads/Glass, shipped as files
	rig.browser.loadBankFromDisk();
	REQUIRE(rig.browser.selectIndex(rig.browser.indexOf("Bright")));
	REQUIRE(rig.browser.pressVariant(4)); // The user's own variant of a shipped preset
	REQUIRE(rig.browser.clearableVariants().size() == 1);
	REQUIRE(rig.browser.actVariant(1, PresetBrowser::VariantVerb::ClearOthers));
	rig.browser.resolveClearVariants(true);
	CHECK_FALSE(std::filesystem::exists(PresetStore::directory(kPluginBank) / "Bright_variant_4.fpreset"));
	// THE SHIPPED ROOT, byte for byte.
	CHECK(fileBytes(PresetStore::directory(kPluginBank) / "Bright.fpreset") == bytesOf("fixture payload: Bright\n"));
	CHECK(rig.browser.clearableVariants().empty());
}

TEST_CASE("PP list: the browser lists each preset once — no variant files in the list, the tags or ◀ ▶ — and a root still offers its variants on the chips") {
	Rig rig("list-once");
	rig.save("Air", "mood:dark");
	rig.save("Pads/Glass", "bright");
	rig.save("Old_variant_1"); // the old scheme's name: an ordinary preset now, so LISTED
	REQUIRE(rig.browser.selectIndex(rig.browser.indexOf("Glass", "Pads")));
	REQUIRE(rig.browser.pressVariant(2));
	REQUIRE(rig.browser.selectIndex(rig.browser.indexOf("Glass", "Pads")));
	REQUIRE(rig.browser.pressVariant(6));
	REQUIRE(rig.browser.selectIndex(rig.browser.indexOf("Air")));
	REQUIRE(rig.browser.pressVariant(3));

	// THE LIST: every item row's label, and no variant file among them.
	std::vector<std::string> labels;
	for (const ItemStepper::Row& r : rig.browser.listRows()) {
		if (r.id >= 0 && r.id < ItemStepper::kActionBase) {
			labels.push_back(r.label);
		}
	}
	std::sort(labels.begin(), labels.end());
	CHECK(labels == std::vector<std::string> { "Air", "Glass", "Init", "Old_variant_1" });

	// THE TAG VIEW'S COUNTS: "bright" once (Glass), not three times (Glass + its two variants).
	rig.browser.chooseFromMenu(PresetBrowser::kMenuByTag);
	bool sawBright = false;
	for (const ItemStepper::Row& r : rig.browser.listRows()) {
		for (const std::string& sub : r.subsections) {
			if (sub.rfind("bright (", 0) == 0) {
				sawBright = true;
				CHECK(sub == "bright (1)");
			}
		}
		if (r.id >= 0 && r.id < ItemStepper::kActionBase) {
			CHECK_FALSE(PresetBrowser::isVariantFile(r.label));
		}
	}
	CHECK(sawBright);
	for (const TagStat& t : rig.browser.rankedTags()) {
		if (t.tag == "bright") {
			CHECK(t.count == 1);
		}
	}
	rig.browser.chooseFromMenu(PresetBrowser::kMenuByFolder);

	// ◀ ▶ NEVER LAND ON A VARIANT: a lap of steps from the root visits only listed presets.
	REQUIRE(rig.browser.selectIndex(rig.browser.indexOf("Air")));
	for (int i = 0; i < 8; ++i) {
		rig.browser.step(1);
		CAPTURE(rig.browser.currentName());
		CHECK_FALSE(PresetBrowser::isVariantFile(rig.browser.currentName()));
	}

	// A ROOT STILL OFFERS ITS VARIANTS ON THE CHIPS, and they load.
	REQUIRE(rig.browser.selectIndex(rig.browser.indexOf("Glass", "Pads")));
	CHECK(rig.browser.chipState(1) == PresetBrowser::ChipState::Active);
	CHECK(rig.browser.chipState(2) == PresetBrowser::ChipState::Saved);
	CHECK(rig.browser.chipState(6) == PresetBrowser::ChipState::Saved);
	CHECK(rig.browser.chipState(3) == PresetBrowser::ChipState::Unused);
	REQUIRE(rig.browser.pressVariant(6));
	CHECK(rig.browser.currentName() == "Glass_variant_6");
}

//--------------------------------------------------------------------------------------------------//
// BACKLOG QC: WHICH PRESET THE PATCH CAME FROM IS PATCH STATE ("when I restart the bitwig audio   //
// engine, feathers forgets which preset it has loaded/shows init")                                 //
//--------------------------------------------------------------------------------------------------//

TEST_CASE("QC: the editor's preset — a variant chip included — goes into the patch's copy, and a rebuilt editor, a fresh "
          "bridge and a host load put the face back on it") {
	const ScopedPresetLibrary library("px-face");
	feathers::PresetIdentity carried;
	{
		StatefulGuiHost host;
		ParamBridge bridge;
		feathers::BridgeDeviceHost deviceHost { bridge };
		host.bridge = &bridge;
		Gui gui(host, bridge);
		REQUIRE(gui.create(CLAP_WINDOW_API_X11, false));
		PresetBrowser* browser = gui.presetBrowserForTest();
		REQUIRE(browser != nullptr);
		CHECK(bridge.presetIdentity().empty()); // the bank opening on Init is not a load
		saveThroughPrompt(*browser);
		const std::string root = browser->currentName();
		CHECK(bridge.presetIdentity().name == root);
		// CHIP 3: saved and shown — the patch names the variant.
		REQUIRE(browser->pressVariant(3));
		gui.tickForTest();
		CHECK(bridge.presetIdentity().name == root + "_variant_3");
		CHECK(bridge.presetIdentity().category.empty());

		// THE EDITOR CLOSED AND REOPENED over the same instance: the face is the variant, chip 3 lit.
		gui.destroy();
		REQUIRE(gui.create(CLAP_WINDOW_API_X11, false));
		browser = gui.presetBrowserForTest();
		REQUIRE(browser != nullptr);
		CHECK(browser->currentName() == root + "_variant_3");
		CHECK(browser->chipState(3) == PresetBrowser::ChipState::Active);
		CHECK(browser->chipState(1) == PresetBrowser::ChipState::Saved);
		CHECK(browser->lastTouched() == browser->currentIndex()); // Save "X" and the prompt name it too
		carried = bridge.presetIdentity();
		gui.destroy();
	}
	{
		// A FRESH INSTANCE THE HOST LOADED (the `preset` block's commit is `restorePresetIdentity`; the plugin-level
		// round trip through the bytes is test_engine_restart's QC case): its editor opens on the same preset.
		StatefulGuiHost host;
		ParamBridge bridge;
		feathers::BridgeDeviceHost deviceHost { bridge };
		host.bridge = &bridge;
		bridge.restorePresetIdentity(carried);
		Gui gui(host, bridge);
		REQUIRE(gui.create(CLAP_WINDOW_API_X11, false));
		PresetBrowser* browser = gui.presetBrowserForTest();
		REQUIRE(browser != nullptr);
		CHECK(browser->currentName() == carried.name);
		CHECK(browser->chipState(3) == PresetBrowser::ChipState::Active);
		const std::string root = PresetBrowser::variantBase(carried.name);

		// A HOST LOAD UNDER THE OPEN EDITOR: the face follows on the next tick.
		bridge.restorePresetIdentity(feathers::PresetIdentity { root, "" });
		gui.tickForTest();
		CHECK(browser->currentName() == root);
		CHECK(browser->chipState(1) == PresetBrowser::ChipState::Active);

		// A NAME THIS BANK DOES NOT HOLD: the face names nothing, and the patch KEEPS the name rather than losing it.
		bridge.restorePresetIdentity(feathers::PresetIdentity { "Elsewhere", "Gone" });
		gui.tickForTest();
		CHECK(browser->currentIndex() == -1);
		CHECK(bridge.presetIdentity().name == "Elsewhere");

		// A DELETE OF THE NAMED PRESET: the patch names none.
		REQUIRE(browser->selectIndex(browser->indexOf(root)));
		gui.tickForTest();
		CHECK(bridge.presetIdentity().name == root);
		REQUIRE(browser->deleteEntry(browser->indexOf(root)));
		CHECK(bridge.presetIdentity().empty());
		gui.destroy();
	}
}

TEST_CASE("QC: a DEVICE PANEL's browser names its preset into the device's record and opens on it again — SKLITTER's panel "
          "and a Chain's container browser") {
	const ScopedPresetLibrary library("qb-panels");
	ModFixture f;
	// A CHAIN DEVICE'S BROWSER (the container road, `makeChainDevicePresets`): saved through its prompt, it names the
	// device; a second browser over the same device — the panel reopened — opens on that name.
	const size_t chainSlot = f.bridge.addFxDevice(composite::kTraits.tag);
	REQUIRE(chainSlot < feathers::kMaxFxSlots);
	const DeviceInstanceId chain = f.bridge.fxDeviceAt(chainSlot);
	std::string saved;
	{
		ui::ContainerPresets presets = ui::makeChainDevicePresets(f.deviceHost, chain, nullptr);
		REQUIRE(presets.browser != nullptr);
		CHECK(f.bridge.devicePreset(chain).empty()); // opening it names nothing
		saveThroughPrompt(*presets.browser);
		saved = presets.browser->currentName();
		REQUIRE_FALSE(saved.empty());
		CHECK(f.bridge.devicePreset(chain).name == saved);
		// CHIP 2: the variant is what the record names.
		REQUIRE(presets.browser->pressVariant(2));
		CHECK(f.bridge.devicePreset(chain).name == saved + "_variant_2");
	}
	{
		ui::ContainerPresets reopened = ui::makeChainDevicePresets(f.deviceHost, chain, nullptr);
		CHECK(reopened.browser->currentName() == saved + "_variant_2");
		CHECK(reopened.browser->chipState(2) == PresetBrowser::ChipState::Active);
	}
	// AND THE CHAINS BLOCK'S PATCH CARRIES IT: the record a save writes names the device's preset by value.
	{
		const rack::RackPatch patch = f.bridge.chainsPatch(true);
		bool found = false;
		for (const rack::RackPatch::Device& d : patch.device) {
			if (d.instanceId == chain) {
				found = true;
				CHECK(d.preset.name == saved + "_variant_2");
			}
		}
		CHECK(found);
	}

	// SKLITTER'S OWN PANEL (one of the four that build their browser by hand: Delay, Resonate, Featherverb, SKLITTER).
	ui::SklitterExpanded* panel = openSklitterPanel(f);
	REQUIRE(panel != nullptr);
	const DeviceInstanceId sklitter = f.bridge.fxDeviceAt(f.bridge.fxChain().count - 1);
	REQUIRE(f.bridge.descriptorOf(sklitter) != nullptr);
	REQUIRE(std::string(f.bridge.descriptorOf(sklitter)->traits.tag) == fx::sklitter::kTraits.tag);
	PresetBrowser& browser = panel->presetBrowserForTest();
	CHECK(browser.lastTouched() == -1); // a device whose record names nothing: the panel names nothing
	saveThroughPrompt(browser);
	CHECK_FALSE(f.bridge.devicePreset(sklitter).empty());
	CHECK(f.bridge.devicePreset(sklitter).name == browser.currentName());
}
