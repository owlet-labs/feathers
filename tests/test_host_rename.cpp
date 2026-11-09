/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without
 * even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not,
 * see <https://www.gnu.org/licenses/>.
 */

// WHAT A HOST SEES WHEN A MACRO IS RENAMED — measured on the SHIPPED ARTEFACTS, dlopened, through the
// format's own C/C++ API (backlog JT (a)/(g), KD).
//
// THE PLUGIN-SIDE TESTS STOP AT "the rescan call was made". This binary is the next step out: it is a
// host. For CLAP it records the `clap_host_params.rescan` flags and re-reads `params.get_info` the way
// a DAW does. For VST3 it loads `feathers.vst3`'s .so, implements `IComponentHandler`, and reads
// `ParameterInfo` before and after — so what clap-wrapper (v0.16.0, vendored) does with the rescan is
// read off the wrapper itself rather than off its source.
//
// TWO SUITES, TWO PROCESSES. The .clap and the .vst3's .so are built from the same static library and
// both export thousands of default-visibility symbols; ctest runs each suite as its own process so the
// two copies never share an address space. Nothing here depends on that being unsafe — it is simply
// not the question this file asks.
//
// THE VST3 RENAME ARRIVES BY `setState`, because no VST3 interface reaches the test-chain extension:
// the wrapper holds the CLAP plugin privately. The renamed stream is written by the SAME .so's own
// `clap_entry` (it exports one), so the bytes are the ones this binary would save. The wrapper passes
// the stream through untouched (`ClapAsVst3::setState` → `_plugin->load`), and a live rename reaches
// the SAME `ClapAsVst3::param_rescan` as a load's does — only the flags beside INFO differ. So the
// setState road measures the live road's title handling too; what it cannot show is a live rename's
// timing in a host that already has the handler, which is the ordinary case and needs no load.
//
// **THE KD CASES PIN TWO WRAPPER DEFECTS AS FIXED BY OUR PATCH.** Upstream v0.16.0's rescan rewrote
// `title` only, and widened the UTF-8 name byte by byte. Feathers builds clap-wrapper from a copy with
// patches/clap-wrapper/0001-vst3-rescan-info-refreshes-short-title-utf8.patch applied (see the
// clap-wrapper block in CMakeLists.txt). Each check that depends on it is marked `WRAPPER PATCH (KD)`:
// if one goes red, first ask whether the patch is still being applied — a pin bump that dropped it, or
// a build that took the submodule's own sources, reads exactly like a regression here.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "plugin/feathers_test_chain.h"

#include <clap/clap.h>

#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivsthostapplication.h"
#include "pluginterfaces/vst/ivstunits.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <dlfcn.h>
#include <unistd.h>

namespace {

using namespace Steinberg;

//------------------------------------------------------------------------------------------------//
// COMMON                                                                                          //
//------------------------------------------------------------------------------------------------//

/// A headless run must not write the user's config or preset library (tools/host_common.h has the
/// history). Nothing here opens an editor, but the redirect costs nothing and removes the question.
void isolateConfig() {
	static bool done = false;
	if (done) {
		return;
	}
	done = true;
	char dir[] = "/tmp/feathers-host-rename-XXXXXX";
	if (::mkdtemp(dir) != nullptr) {
		::setenv("XDG_CONFIG_HOME", dir, 1);
		::setenv("XDG_DATA_HOME", dir, 1);
	}
}

/// What one host-params rescan looked like. `duringLoad` is true when it arrived INSIDE the plugin's
/// `state.load` — the timing KD's third question is about.
struct Rescan {
	clap_param_rescan_flags flags;
	bool duringLoad;
};

/// ONE HOST PER INSTANCE, recording what the plugin asked of it.
struct RecordingHost {
	clap_host host {};
	std::vector<Rescan> rescans;
	bool inLoad = false;

	RecordingHost() {
		host.clap_version = CLAP_VERSION_INIT;
		host.host_data = this;
		host.name = "feathers-host-rename";
		host.vendor = "Owlet";
		host.url = "";
		host.version = "1";
		host.get_extension = &getExtension;
		host.request_restart = [](const clap_host*) {};
		host.request_process = [](const clap_host*) {};
		host.request_callback = [](const clap_host*) {};
	}
	RecordingHost(const RecordingHost&) = delete;
	RecordingHost& operator=(const RecordingHost&) = delete;

	static const void* getExtension(const clap_host*, const char* id) {
		// ALL THREE CALLBACKS, because clap-helpers' `canUseParams` requires every one before it
		// will send a rescan at all — a host missing `clear` would be measuring that refusal.
		static const clap_host_params kParams {
			[](const clap_host* h, clap_param_rescan_flags flags) {
				auto* self = static_cast<RecordingHost*>(h->host_data);
				self->rescans.push_back(Rescan {flags, self->inLoad});
			},
			[](const clap_host*, clap_id, clap_param_clear_flags) {},
			[](const clap_host*) {},
		};
		static const clap_host_timer_support kTimer {
			[](const clap_host*, uint32_t, clap_id* timer) {
				*timer = 1;
				return true;
			},
			[](const clap_host*, clap_id) { return true; },
		};
		if (id != nullptr && std::strcmp(id, CLAP_EXT_PARAMS) == 0) {
			return &kParams;
		}
		if (id != nullptr && std::strcmp(id, CLAP_EXT_TIMER_SUPPORT) == 0) {
			return &kTimer;
		}
		return nullptr;
	}
};

/// A CLAP output stream into a byte vector.
struct OutBytes {
	std::vector<uint8_t> bytes;
	clap_ostream stream {this, [](const clap_ostream* s, const void* buffer, uint64_t size) -> int64_t {
		auto* self = static_cast<OutBytes*>(s->ctx);
		const auto* p = static_cast<const uint8_t*>(buffer);
		self->bytes.insert(self->bytes.end(), p, p + size);
		return static_cast<int64_t>(size);
	}};
};

/// A CLAP input stream over a byte vector.
struct InBytes {
	const std::vector<uint8_t>& bytes;
	size_t at = 0;
	clap_istream stream {this, [](const clap_istream* s, void* buffer, uint64_t size) -> int64_t {
		auto* self = static_cast<InBytes*>(s->ctx);
		const size_t n = std::min<size_t>(size, self->bytes.size() - self->at);
		std::memcpy(buffer, self->bytes.data() + self->at, n);
		self->at += n;
		return static_cast<int64_t>(n);
	}};
	explicit InBytes(const std::vector<uint8_t>& b) : bytes(b) {}
};

/// ONE CLAP PLUGIN INSTANCE with its own recording host, created through a factory the caller got
/// from whichever binary it is measuring.
struct ClapInstance {
	RecordingHost host;
	const clap_plugin* plugin = nullptr;
	const clap_plugin_params* params = nullptr;
	const clap_plugin_state* state = nullptr;
	const clap_plugin_feathers_test_chain* chain = nullptr;

	explicit ClapInstance(const clap_plugin_factory* factory) {
		REQUIRE(factory != nullptr);
		REQUIRE(factory->get_plugin_count(factory) >= 1); // plugin 0: Feathers, or the Chain (the Note Rack is 1)
		const clap_plugin_descriptor* desc = factory->get_plugin_descriptor(factory, 0);
		REQUIRE(desc != nullptr);
		plugin = factory->create_plugin(factory, &host.host, desc->id);
		REQUIRE(plugin != nullptr);
		REQUIRE(plugin->init(plugin));
		params = static_cast<const clap_plugin_params*>(plugin->get_extension(plugin, CLAP_EXT_PARAMS));
		state = static_cast<const clap_plugin_state*>(plugin->get_extension(plugin, CLAP_EXT_STATE));
		chain = static_cast<const clap_plugin_feathers_test_chain*>(
		    plugin->get_extension(plugin, CLAP_EXT_FEATHERS_TEST_CHAIN));
		REQUIRE(params != nullptr);
		REQUIRE(state != nullptr);
		REQUIRE(chain != nullptr);
		REQUIRE(chain->rename_macro != nullptr);
	}
	~ClapInstance() {
		if (plugin != nullptr) {
			plugin->destroy(plugin);
		}
	}
	ClapInstance(const ClapInstance&) = delete;
	ClapInstance& operator=(const ClapInstance&) = delete;

	/// The info a host reads for the parameter whose id is `id` — by walking the list, as a host does.
	clap_param_info infoById(clap_id id) const {
		const uint32_t n = params->count(plugin);
		for (uint32_t i = 0; i < n; ++i) {
			clap_param_info info {};
			if (params->get_info(plugin, i, &info) && info.id == id) {
				return info;
			}
		}
		FAIL("no parameter with id " << id);
		return {};
	}

	/// The id of the parameter a host currently calls `name`. Asked, never hard-coded: the host knows
	/// nothing of our headers, and neither does this file.
	clap_id idByName(const char* name) const {
		const uint32_t n = params->count(plugin);
		for (uint32_t i = 0; i < n; ++i) {
			clap_param_info info {};
			if (params->get_info(plugin, i, &info) && std::strcmp(info.name, name) == 0) {
				return info.id;
			}
		}
		FAIL("no parameter named " << name);
		return CLAP_INVALID_ID;
	}

	std::vector<uint8_t> save() const {
		OutBytes out;
		REQUIRE(state->save(plugin, &out.stream));
		REQUIRE(!out.bytes.empty());
		return out.bytes;
	}

	bool load(const std::vector<uint8_t>& bytes) {
		InBytes in(bytes);
		host.inLoad = true;
		const bool ok = state->load(plugin, &in.stream);
		host.inLoad = false;
		return ok;
	}
};

struct Library {
	void* handle = nullptr;
	const clap_plugin_entry* entry = nullptr;
	const clap_plugin_factory* factory = nullptr;
};

/// dlopen a CLAP binary and init its entry, once per path for the life of the process.
Library openClap(const char* path) {
	isolateConfig();
	Library lib;
	lib.handle = ::dlopen(path, RTLD_NOW | RTLD_LOCAL);
	REQUIRE_MESSAGE(lib.handle != nullptr, "dlopen " << path << ": " << ::dlerror());
	lib.entry = static_cast<const clap_plugin_entry*>(::dlsym(lib.handle, "clap_entry"));
	REQUIRE(lib.entry != nullptr);
	REQUIRE(lib.entry->init(path));
	lib.factory =
	    static_cast<const clap_plugin_factory*>(lib.entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
	REQUIRE(lib.factory != nullptr);
	return lib;
}

const Library& feathersClap() {
	static const Library lib = openClap(FEATHERS_CLAP_PATH);
	return lib;
}
const Library& rackClap() {
	static const Library lib = openClap(FEATHERS_RACK_CLAP_PATH);
	return lib;
}

/// WHAT EACH PLUGIN SENDS BESIDE INFO AT THE END OF A LOAD: both fold the rename into the RESCAN_VALUES
/// the load sends. The rack's once sent INFO alone, which left the host showing the old macro values
/// after a load (backlog JW): a rack patch does carry its macros' values, and `paramsValue` answers them.
struct Subject {
	const char* label;
	const Library& (*clap)();
	clap_param_rescan_flags loadFlags;
};

const Subject kSubjects[] = {
    {"feathers", &feathersClap, CLAP_PARAM_RESCAN_VALUES | CLAP_PARAM_RESCAN_INFO},
    {"rack", &rackClap, CLAP_PARAM_RESCAN_VALUES | CLAP_PARAM_RESCAN_INFO},
};

std::string toUtf8(const Vst::TChar* s) {
	std::string out;
	for (; *s != 0; ++s) {
		out.push_back(*s < 0x80 ? static_cast<char>(*s) : '?');
	}
	return out;
}

} // namespace

//------------------------------------------------------------------------------------------------//
// CLAP — the .clap, in a host that honours `params.rescan`.                                        //
//------------------------------------------------------------------------------------------------//

TEST_SUITE("clap") {

// THE PRODUCT'S NAME, AS THE SHIPPED BUNDLE SAYS IT (backlog NP, 2026-10-18). Not a macro rename, but
// this is the one suite that dlopens the built artefacts, and the claim is about the artefact: the
// file a host scans for and the descriptor its factory hands over. The single series chain was
// "Feathers Rack" until then; "Rack" now names parallel chains (backlog NO).
TEST_CASE("clap host: the chain plugin ships as feathers-chain.clap, named Feathers Chain, id com.owlet.feathers-chain") {
	const std::string path = FEATHERS_RACK_CLAP_PATH;
	CHECK(path.substr(path.find_last_of('/') + 1) == "feathers-chain.clap");
	const clap_plugin_factory* factory = rackClap().factory;
	REQUIRE(factory->get_plugin_count(factory) == 2); // the Chain, and the Note Rack (2026-11-04)
	const clap_plugin_descriptor* d = factory->get_plugin_descriptor(factory, 0);
	REQUIRE(d != nullptr);
	CHECK(std::string(d->id) == "com.owlet.feathers-chain");
	CHECK(std::string(d->name) == "Feathers Chain");
	// THE NOTE RACK RIDES IN THE SAME BUNDLE: a CLAP note effect, which is what a host files under Note FX.
	const clap_plugin_descriptor* notes = factory->get_plugin_descriptor(factory, 1);
	REQUIRE(notes != nullptr);
	CHECK(std::string(notes->id) == "com.owlet.feathers.noterack");
	CHECK(std::string(notes->name) == "Feathers Note Rack");
	bool noteEffect = false;
	bool audioEffect = false;
	for (const char* const* f = notes->features; f != nullptr && *f != nullptr; ++f) {
		noteEffect = noteEffect || std::string(*f) == CLAP_PLUGIN_FEATURE_NOTE_EFFECT;
		audioEffect = audioEffect || std::string(*f) == CLAP_PLUGIN_FEATURE_AUDIO_EFFECT;
	}
	CHECK(noteEffect);
	CHECK_FALSE(audioEffect);
}

TEST_CASE("clap host: a live rename is one RESCAN_INFO, and get_info then answers the new name on the same id") {
	for (const Subject& s : kSubjects) {
		CAPTURE(s.label);
		ClapInstance inst(s.clap().factory);
		const clap_id m1 = inst.idByName("Macro 1");
		const clap_id m2 = inst.idByName("Macro 2");
		const clap_param_info before = inst.infoById(m1);
		CHECK(std::string(before.module) == "Macros");
		CHECK(inst.host.rescans.empty());

		REQUIRE(inst.chain->rename_macro(inst.plugin, 0, "Wobble"));

		REQUIRE(inst.host.rescans.size() == 1);
		CHECK(inst.host.rescans[0].flags == CLAP_PARAM_RESCAN_INFO);
		CHECK_FALSE(inst.host.rescans[0].duringLoad);
		const clap_param_info after = inst.infoById(m1);
		CHECK(std::string(after.name) == "Wobble");
		CHECK(std::string(after.module) == "Macros"); // the module does not move with the name
		CHECK(after.flags == before.flags);
		CHECK(std::string(inst.infoById(m2).name) == "Macro 2"); // the neighbour is untouched
		std::printf("measured[clap %s]: id %u  before name=\"%s\" module=\"%s\"  after name=\"%s\" "
		            "module=\"%s\"  rescans=%zu flags=0x%x\n",
		            s.label, m1, before.name, before.module, after.name, after.module,
		            inst.host.rescans.size(), inst.host.rescans[0].flags);

		// THE SAME NAME AGAIN IS NOT AN EDIT, so the host hears nothing (`ParamBridge::setMacroName`).
		REQUIRE(inst.chain->rename_macro(inst.plugin, 0, "Wobble"));
		CHECK(inst.host.rescans.size() == 1);
	}
}

TEST_CASE("clap host: save and reopen carries the name, told once, inside the load, beside the load's own flags") {
	for (const Subject& s : kSubjects) {
		CAPTURE(s.label);
		std::vector<uint8_t> saved;
		clap_id m1 = CLAP_INVALID_ID;
		{
			ClapInstance first(s.clap().factory);
			m1 = first.idByName("Macro 1");
			REQUIRE(first.chain->rename_macro(first.plugin, 0, "Wobble"));
			saved = first.save();
		}
		ClapInstance reopened(s.clap().factory); // the "reopen": a fresh instance, as a project load makes
		CHECK(std::string(reopened.infoById(m1).name) == "Macro 1");
		REQUIRE(reopened.load(saved));
		REQUIRE(reopened.host.rescans.size() == 1);
		CHECK(reopened.host.rescans[0].flags == s.loadFlags);
		CHECK(reopened.host.rescans[0].duringLoad); // sent from INSIDE state.load — KD's timing question
		CHECK(std::string(reopened.infoById(m1).name) == "Wobble");
		std::printf("measured[clap %s]: reopen -> name=\"%s\" rescans=%zu flags=0x%x duringLoad=%d\n",
		            s.label, reopened.infoById(m1).name, reopened.host.rescans.size(),
		            reopened.host.rescans[0].flags, int(reopened.host.rescans[0].duringLoad));
	}
}

TEST_CASE("clap host: loading an unnamed patch over a renamed macro gives the default name back, and says so") {
	for (const Subject& s : kSubjects) {
		CAPTURE(s.label);
		std::vector<uint8_t> plain;
		{
			ClapInstance fresh(s.clap().factory);
			plain = fresh.save();
		}
		ClapInstance inst(s.clap().factory);
		const clap_id m1 = inst.idByName("Macro 1");
		REQUIRE(inst.chain->rename_macro(inst.plugin, 0, "Wobble"));
		inst.host.rescans.clear();
		REQUIRE(inst.load(plain));
		REQUIRE(inst.host.rescans.size() == 1);
		CHECK(inst.host.rescans[0].flags == s.loadFlags);
		CHECK(std::string(inst.infoById(m1).name) == "Macro 1");
	}
}

} // TEST_SUITE("clap")

//------------------------------------------------------------------------------------------------//
// VST3 — the .vst3's .so through clap-wrapper, in a host that records restartComponent.            //
//------------------------------------------------------------------------------------------------//

namespace {

/// A non-owning FUnknown: every object below lives on the stack or in a static and outlives the
/// plugin's references to it, so the counts are never what frees anything.
#define FEATHERS_NOREFCOUNT                                   \
	uint32 PLUGIN_API addRef() override { return 1; }       \
	uint32 PLUGIN_API release() override { return 1; }

class HostApp final : public Vst::IHostApplication {
public:
	tresult PLUGIN_API getName(Vst::String128 name) override {
		const char* n = "feathers-host-rename";
		size_t i = 0;
		for (; n[i] != 0; ++i) {
			name[i] = static_cast<Vst::TChar>(n[i]);
		}
		name[i] = 0;
		return kResultOk;
	}
	tresult PLUGIN_API createInstance(TUID, TUID, void** obj) override {
		*obj = nullptr;
		return kNotImplemented;
	}
	tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
		if (FUnknownPrivate::iidEqual(iid, Vst::IHostApplication::iid) ||
		    FUnknownPrivate::iidEqual(iid, FUnknown::iid)) {
			*obj = this;
			return kResultOk;
		}
		*obj = nullptr;
		return kNoInterface;
	}
	FEATHERS_NOREFCOUNT
};

/// RECORDS EVERY restartComponent — the call KD predicts carries kParamTitlesChanged.
class Handler final : public Vst::IComponentHandler {
public:
	std::vector<int32> restarts;

	tresult PLUGIN_API beginEdit(Vst::ParamID) override { return kResultOk; }
	tresult PLUGIN_API performEdit(Vst::ParamID, Vst::ParamValue) override { return kResultOk; }
	tresult PLUGIN_API endEdit(Vst::ParamID) override { return kResultOk; }
	tresult PLUGIN_API restartComponent(int32 flags) override {
		restarts.push_back(flags);
		return kResultOk;
	}
	tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
		if (FUnknownPrivate::iidEqual(iid, Vst::IComponentHandler::iid) ||
		    FUnknownPrivate::iidEqual(iid, FUnknown::iid)) {
			*obj = this;
			return kResultOk;
		}
		*obj = nullptr;
		return kNoInterface;
	}
	FEATHERS_NOREFCOUNT
};

/// An IBStream over a byte vector.
class Stream final : public IBStream {
public:
	std::vector<uint8_t> bytes;
	int64 at = 0;

	tresult PLUGIN_API read(void* buffer, int32 numBytes, int32* numBytesRead) override {
		const int64 n = std::max<int64>(0, std::min<int64>(numBytes, int64(bytes.size()) - at));
		std::memcpy(buffer, bytes.data() + at, size_t(n));
		at += n;
		if (numBytesRead != nullptr) {
			*numBytesRead = int32(n);
		}
		return kResultOk;
	}
	tresult PLUGIN_API write(void* buffer, int32 numBytes, int32* numBytesWritten) override {
		const auto* p = static_cast<const uint8_t*>(buffer);
		if (at < int64(bytes.size())) {
			bytes.resize(size_t(at));
		}
		bytes.insert(bytes.end(), p, p + numBytes);
		at += numBytes;
		if (numBytesWritten != nullptr) {
			*numBytesWritten = numBytes;
		}
		return kResultOk;
	}
	tresult PLUGIN_API seek(int64 pos, int32 mode, int64* result) override {
		const int64 base = mode == kIBSeekSet ? 0 : mode == kIBSeekCur ? at : int64(bytes.size());
		at = std::max<int64>(0, std::min<int64>(base + pos, int64(bytes.size())));
		if (result != nullptr) {
			*result = at;
		}
		return kResultOk;
	}
	tresult PLUGIN_API tell(int64* pos) override {
		*pos = at;
		return kResultOk;
	}
	tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
		if (FUnknownPrivate::iidEqual(iid, IBStream::iid) || FUnknownPrivate::iidEqual(iid, FUnknown::iid)) {
			*obj = this;
			return kResultOk;
		}
		*obj = nullptr;
		return kNoInterface;
	}
	FEATHERS_NOREFCOUNT
};

struct Vst3Library {
	void* handle = nullptr;
	IPluginFactory* factory = nullptr;
	/// THE SAME .so's OWN CLAP FACTORY — the one the wrapper wraps. Used only to write a renamed
	/// stream, since no VST3 interface can rename.
	const clap_plugin_factory* clapFactory = nullptr;
};

Vst3Library openVst3(const char* path) {
	isolateConfig();
	Vst3Library lib;
	lib.handle = ::dlopen(path, RTLD_NOW | RTLD_LOCAL);
	REQUIRE_MESSAGE(lib.handle != nullptr, "dlopen " << path << ": " << ::dlerror());
	using ModuleEntryFn = bool (*)(void*);
	using GetFactoryFn = IPluginFactory* (*)();
	auto moduleEntry = reinterpret_cast<ModuleEntryFn>(::dlsym(lib.handle, "ModuleEntry"));
	auto getFactory = reinterpret_cast<GetFactoryFn>(::dlsym(lib.handle, "GetPluginFactory"));
	REQUIRE(moduleEntry != nullptr);
	REQUIRE(getFactory != nullptr);
	REQUIRE(moduleEntry(lib.handle));
	lib.factory = getFactory();
	REQUIRE(lib.factory != nullptr);
	// AFTER GetPluginFactory, which is where the wrapper inits the CLAP entry it wraps — so this
	// reads a factory that is already live rather than initialising the entry a second time.
	const auto* entry = static_cast<const clap_plugin_entry*>(::dlsym(lib.handle, "clap_entry"));
	REQUIRE(entry != nullptr);
	lib.clapFactory =
	    static_cast<const clap_plugin_factory*>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
	REQUIRE(lib.clapFactory != nullptr);
	return lib;
}

const Vst3Library& feathersVst3() {
	static const Vst3Library lib = openVst3(FEATHERS_VST3_PATH);
	return lib;
}
const Vst3Library& rackVst3() {
	static const Vst3Library lib = openVst3(FEATHERS_RACK_VST3_PATH);
	return lib;
}

/// What a VST3 host reads for one parameter: the two titles and the name of its unit.
struct Seen {
	std::string title;
	std::string shortTitle;
	std::string unit;
	std::u16string titleUtf16; // the title as the host receives it, for names past ASCII
	std::u16string shortTitleUtf16; // the same for the short title
};

/// ONE VST3 INSTANCE (single-component: the IComponent is also the IEditController), initialised
/// with a host context and NOT yet given a component handler — each case chooses when, because the
/// order is one of the things measured.
struct Vst3Instance {
	HostApp app;
	Handler handler;
	Vst::IComponent* component = nullptr;
	Vst::IEditController* controller = nullptr;
	Vst::IUnitInfo* units = nullptr;

	explicit Vst3Instance(const Vst3Library& lib) {
		const int32 n = lib.factory->countClasses();
		for (int32 i = 0; i < n && component == nullptr; ++i) {
			PClassInfo ci;
			REQUIRE(lib.factory->getClassInfo(i, &ci) == kResultOk);
			if (std::strcmp(ci.category, kVstAudioEffectClass) == 0) {
				void* obj = nullptr;
				REQUIRE(lib.factory->createInstance(ci.cid, Vst::IComponent::iid, &obj) == kResultOk);
				component = static_cast<Vst::IComponent*>(obj);
			}
		}
		REQUIRE(component != nullptr);
		REQUIRE(component->initialize(&app) == kResultOk);
		void* obj = nullptr;
		REQUIRE(component->queryInterface(Vst::IEditController::iid, &obj) == kResultOk);
		controller = static_cast<Vst::IEditController*>(obj);
		obj = nullptr;
		if (component->queryInterface(Vst::IUnitInfo::iid, &obj) == kResultOk) {
			units = static_cast<Vst::IUnitInfo*>(obj);
		}
	}
	~Vst3Instance() {
		if (controller != nullptr) {
			controller->setComponentHandler(nullptr);
		}
		if (units != nullptr) {
			units->release();
		}
		if (controller != nullptr) {
			controller->release();
		}
		if (component != nullptr) {
			component->terminate();
			component->release();
		}
	}
	Vst3Instance(const Vst3Instance&) = delete;
	Vst3Instance& operator=(const Vst3Instance&) = delete;

	void attachHandler() { REQUIRE(controller->setComponentHandler(&handler) == kResultOk); }

	Seen seen(Vst::ParamID id) const {
		const int32 n = controller->getParameterCount();
		for (int32 i = 0; i < n; ++i) {
			Vst::ParameterInfo info {};
			if (controller->getParameterInfo(i, info) == kResultOk && info.id == id) {
				Seen s {toUtf8(info.title), toUtf8(info.shortTitle), {},
				        std::u16string(reinterpret_cast<const char16_t*>(info.title)),
				        std::u16string(reinterpret_cast<const char16_t*>(info.shortTitle))};
				if (units != nullptr) {
					const int32 u = units->getUnitCount();
					for (int32 k = 0; k < u; ++k) {
						Vst::UnitInfo unit {};
						if (units->getUnitInfo(k, unit) == kResultOk && unit.id == info.unitId) {
							s.unit = toUtf8(unit.name);
						}
					}
				}
				return s;
			}
		}
		FAIL("no VST3 parameter with id " << id);
		return {};
	}

	tresult setState(const std::vector<uint8_t>& bytes) {
		Stream s;
		s.bytes = bytes;
		return component->setState(&s);
	}
	std::vector<uint8_t> getState() {
		Stream s;
		REQUIRE(component->getState(&s) == kResultOk);
		return s.bytes;
	}
};

struct Vst3Subject {
	const char* label;
	const Vst3Library& (*lib)();
};

const Vst3Subject kVst3Subjects[] = {
    {"feathers", &feathersVst3},
    {"rack", &rackVst3},
};

/// A stream with macro 1 named `name`, written by the .so's own CLAP side; and the VST3 id of macro 1
/// (the CLAP id with the top bit cleared — the wrapper's own mapping, `Vst3Parameter::create`).
struct Renamed {
	std::vector<uint8_t> bytes;
	Vst::ParamID id;
};

Renamed renamedStream(const Vst3Library& lib, const char* name) {
	ClapInstance clap(lib.clapFactory);
	Renamed r;
	r.id = clap.idByName("Macro 1") & 0x7FFFFFFF;
	REQUIRE(clap.chain->rename_macro(clap.plugin, 0, name));
	r.bytes = clap.save();
	return r;
}

bool hasFlag(const std::vector<int32>& restarts, int32 flag) {
	for (int32 f : restarts) {
		if ((f & flag) == flag) {
			return true;
		}
	}
	return false;
}

} // namespace

TEST_SUITE("vst3") {

TEST_CASE("vst3 host: at startup Macro 1 reads title \"Macro 1\" and shortTitle \"Macro 1\", module as its unit") {
	for (const Vst3Subject& s : kVst3Subjects) {
		CAPTURE(s.label);
		const Renamed r = renamedStream(s.lib(), "Wobble"); // for the id only
		Vst3Instance inst(s.lib());
		inst.attachHandler();
		const Seen seen = inst.seen(r.id);
		std::printf("measured[vst3 %s]: startup title=\"%s\" shortTitle=\"%s\" unit=\"%s\"\n", s.label,
		            seen.title.c_str(), seen.shortTitle.c_str(), seen.unit.c_str());
		// KD SAID the title is built "module/name" and reads "Macros/Macro 1". REFUTED: with a module
		// and the unit lambda (`setupParameters` always passes one), `Vst3Parameter::create` takes the
		// name ALONE and files the module as the parameter's UNIT. So there is no prefix to lose.
		CHECK(seen.title == "Macro 1");
		CHECK(seen.shortTitle == "Macro 1");
		CHECK(seen.unit == "Macros");
	}
}

TEST_CASE("vst3 host: handler set, then setState with a renamed macro — the title and the short title move, kParamTitlesChanged is sent") {
	for (const Vst3Subject& s : kVst3Subjects) {
		CAPTURE(s.label);
		const Renamed r = renamedStream(s.lib(), "Wobble");
		Vst3Instance inst(s.lib());
		inst.attachHandler(); // the order a host normally uses: initialize -> handler -> setState
		REQUIRE(inst.setState(r.bytes) == kResultOk);
		const Seen seen = inst.seen(r.id);
		std::printf("measured[vst3 %s]: handler-then-setState title=\"%s\" shortTitle=\"%s\" unit=\"%s\" "
		            "restarts=%zu",
		            s.label, seen.title.c_str(), seen.shortTitle.c_str(), seen.unit.c_str(),
		            inst.handler.restarts.size());
		for (int32 f : inst.handler.restarts) {
			std::printf(" 0x%x", unsigned(f));
		}
		std::printf("\n");
		CHECK(seen.title == "Wobble");
		CHECK(seen.unit == "Macros");
		REQUIRE(inst.handler.restarts.size() == 1);
		CHECK(hasFlag(inst.handler.restarts, Vst::RestartFlags::kParamTitlesChanged));
		CHECK(hasFlag(inst.handler.restarts, Vst::RestartFlags::kParamValuesChanged));
		// WRAPPER PATCH (KD): upstream's `ClapAsVst3::param_rescan` rewrites `title` only, leaving
		// "Macro 1" here; our patch rewrites `shortTitle` too, as setup does.
		CHECK(seen.shortTitle == "Wobble");
	}
}

TEST_CASE("vst3 host: setState BEFORE the handler — the wrapper's titles move, and the host is never told") {
	for (const Vst3Subject& s : kVst3Subjects) {
		CAPTURE(s.label);
		const Renamed r = renamedStream(s.lib(), "Wobble");
		Vst3Instance inst(s.lib());
		REQUIRE(inst.setState(r.bytes) == kResultOk); // no handler yet
		inst.attachHandler();
		const Seen seen = inst.seen(r.id);
		std::printf("measured[vst3 %s]: setState-then-handler title=\"%s\" shortTitle=\"%s\" restarts=%zu\n",
		            s.label, seen.title.c_str(), seen.shortTitle.c_str(), inst.handler.restarts.size());
		// NOTHING REACHES THE HOST: the rescan fired inside setState with `componentHandler` null, and
		// setComponentHandler does not replay it. A host that reads ParameterInfo AFTER this point
		// (any host that builds its parameter list once the handler is set) still sees the new title —
		// but one that cached titles before setState is never told they moved.
		CHECK(inst.handler.restarts.empty());
		CHECK(seen.title == "Wobble");
		CHECK(seen.shortTitle == "Wobble"); // WRAPPER PATCH (KD), as above
	}
}

TEST_CASE("vst3 host: getState carries the name, and a fresh instance given it reads the name back (save and reopen)") {
	for (const Vst3Subject& s : kVst3Subjects) {
		CAPTURE(s.label);
		const Renamed r = renamedStream(s.lib(), "Wobble");
		std::vector<uint8_t> projectBlob;
		{
			Vst3Instance first(s.lib());
			first.attachHandler();
			REQUIRE(first.setState(r.bytes) == kResultOk);
			projectBlob = first.getState(); // what a host writes into its project
		}
		Vst3Instance reopened(s.lib());
		CHECK(reopened.seen(r.id).title == "Macro 1");
		reopened.attachHandler();
		REQUIRE(reopened.setState(projectBlob) == kResultOk);
		const Seen seen = reopened.seen(r.id);
		std::printf("measured[vst3 %s]: reopen title=\"%s\" shortTitle=\"%s\" restarts=%zu\n", s.label,
		            seen.title.c_str(), seen.shortTitle.c_str(), reopened.handler.restarts.size());
		CHECK(seen.title == "Wobble");
		CHECK(hasFlag(reopened.handler.restarts, Vst::RestartFlags::kParamTitlesChanged));
		CHECK(seen.shortTitle == "Wobble"); // WRAPPER PATCH (KD)
	}
}

TEST_CASE("vst3 host: a patch that names nothing gives the title back, and says so") {
	for (const Vst3Subject& s : kVst3Subjects) {
		CAPTURE(s.label);
		const Renamed named = renamedStream(s.lib(), "Wobble");
		const Renamed plain = renamedStream(s.lib(), ""); // empty is the default, "Macro 1"
		Vst3Instance inst(s.lib());
		inst.attachHandler();
		REQUIRE(inst.setState(named.bytes) == kResultOk);
		REQUIRE(inst.seen(named.id).title == "Wobble");
		inst.handler.restarts.clear();
		REQUIRE(inst.setState(plain.bytes) == kResultOk);
		CHECK(inst.seen(named.id).title == "Macro 1");
		CHECK(hasFlag(inst.handler.restarts, Vst::RestartFlags::kParamTitlesChanged));
	}
}

TEST_CASE("vst3 host: a name past ASCII reaches the title and the short title as UTF-16") {
	// A macro name is UTF-8 (`device::clampMacroName`), and at setup the wrapper converts one properly
	// (`utf8_to_utf16l` in `Vst3Parameter::create`). Upstream's rescan used the SDK's `str8ToStr16`,
	// which widens each BYTE of a signed char — so "Wöbble" (W C3 B6 b b l e) arrived as W, U+FFC3,
	// U+FFB6. Our patch converts with `utf8_to_utf16l`, as setup does.
	for (const Vst3Subject& s : kVst3Subjects) {
		CAPTURE(s.label);
		const Renamed r = renamedStream(s.lib(), "W\xC3\xB6" "bble");
		{
			// The plugin's side is right: CLAP get_info hands back the UTF-8 it was given.
			ClapInstance clap(s.lib().clapFactory);
			REQUIRE(clap.load(r.bytes));
			CHECK(std::string(clap.infoById(r.id).name) == "W\xC3\xB6" "bble");
		}
		Vst3Instance inst(s.lib());
		inst.attachHandler();
		REQUIRE(inst.setState(r.bytes) == kResultOk);
		const Seen seen = inst.seen(r.id);
		const std::u16string& got = seen.titleUtf16;
		std::printf("measured[vst3 %s]: \"W\\xC3\\xB6bble\" -> title UTF-16 units:", s.label);
		for (char16_t c : got) {
			std::printf(" %04X", unsigned(c));
		}
		std::printf("; short title:");
		for (char16_t c : seen.shortTitleUtf16) {
			std::printf(" %04X", unsigned(c));
		}
		std::printf("\n");
		// WRAPPER PATCH (found measuring KD): W U+00F6 b b l e, spelled as code units so the source
		// file's encoding cannot make this agree with itself. Unpatched, both read W FFC3 FFB6 b b l e.
		const std::u16string woebble {u'W', char16_t(0x00F6), u'b', u'b', u'l', u'e'};
		const std::u16string bytewise {u'W', char16_t(0xFFC3), char16_t(0xFFB6), u'b', u'b', u'l', u'e'};
		CHECK(got == woebble);
		CHECK(seen.shortTitleUtf16 == woebble);
		CHECK(got != bytewise);
	}
}

} // TEST_SUITE("vst3")
