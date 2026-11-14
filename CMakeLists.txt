cmake_minimum_required(VERSION 3.24)
project(feathers VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Hidden by default, project-wide rather than only on the plugin module. A CLAP exports exactly one
# symbol, clap_entry, and CLAP_EXPORT already marks that one visible.
#
# Project-wide because the symbol this is really aimed at is not in the plugin's own translation
# unit: the audio_engine shim's `inline uint32_t audioSampleTimer` is an inline variable, which GCC
# emits as STB_GNU_UNIQUE, and it is compiled into feathers-character and feathers-dsp. A
# STB_GNU_UNIQUE symbol makes glibc mark the whole .so NODELETE, so a host that unloads the plugin
# does not get its memory back. Hidden visibility demotes it to a local symbol and the binding never
# arises. (Task 3 quality review.)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

# MSVC, PROJECT-WIDE, AND HERE, BEFORE ANY add_subdirectory: a directory added earlier (the device SDK, MEASURED)
# keeps the dynamic runtime and fails every link with LNK2038. What GCC and Clang assume and MSVC has to be told:
#   /utf-8           the sources are UTF-8 (em dashes, x signs, the Möbius in comments and strings); without it MSVC
#                    reads them in the runner's code page, warns C4819, and mis-encodes string literals.
#   /bigobj          several TUs (the widget suites, gui.cpp) pass the default 2^16 section limit (C1128).
#   /Zc:__cplusplus  report the real language level, which visage and doctest test.
#   _USE_MATH_DEFINES  M_PI and friends, which glibc and libc++ declare unasked.
#   NOMINMAX         <windows.h> otherwise defines min/max as macros and breaks std::min. NOT WIN32_LEAN_AND_MEAN
#                    project-wide: it drops shellapi.h, which visage's win32 backend needs (HDROP, DragQueryFile).
#   _CRT_SECURE_NO_WARNINGS  getenv, fopen and friends are the portable spelling here; C4996 asks for _s twins.
#
# THE STATIC C RUNTIME (/MT), project-wide and before any target. clap-wrapper builds its VST3 and standalone
# objects against it (MultiThreaded), so ours must match or every link fails LNK2038; and it is the Windows form
# of the static-link posture the Linux build takes with -static-libstdc++: the plugin needs no VC++ redistributable
# installed beside the host.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
if(MSVC)
	add_compile_options(/utf-8 /bigobj /Zc:__cplusplus)
	add_compile_definitions(_USE_MATH_DEFINES NOMINMAX _CRT_SECURE_NO_WARNINGS)
endif()

if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE Release)
endif()

# COMPILER CACHE. A launcher, not a flag: ccache sits in front of the compiler, hashes the
# preprocessed TU plus the exact compiler and command line, and hands back the object it already has
# for that hash or runs the real compile and stores it. It changes neither the flags on the compile
# line nor the bytes in the object, so a directory whose identity is its flags (`build-install-on`:
# both ship flags ON, installs only) is the same directory with the launcher in front of it -- the
# `-DFEATHERS_*` check on the real compile line still reads what it always read. CLAUDE.md's rule
# that nobody reconfigures that directory casually stands; this block is inherited by it on its next
# re-run, and that re-run is the integrator's to make.
#
# WHY: the fetch of bgfx, freetype, visage and the clap-wrapper SDKs is shared between worktrees; the
# COMPILE of them was not. A fresh worktree paid ~25 minutes of compile for objects identical to the
# ones every other worktree had already built (measured on this host, 2026-09-02, numbers in the
# commit). With the cache shared under ~/.cache/ccache, the second worktree pays for its own
# differences only (MEASURED: 474 of 475 compiles hit rebuilding in place, 469 of 475 from a second
# worktree with CCACHE_BASEDIR set -- see scripts/env.sh -- and the compile step went 654 s -> 97 s). In a
# FEATHERS_DIAGNOSTIC build six UI translation units embed `__TIME__` via diagnostic.h's build stamp
# and ccache refuses to cache those by default -- deliberately not relaxed with
# `sloppiness=time_macros`, because the stamp is what a crash log is resolved against and a cached
# one would lie. The ordinary build never sees the stamp and caches everything.
#
# BEFORE ANY TARGET, because the launcher variables initialise a per-target property at
# add_library/add_executable time. FetchContent subdirectories inherit them; an ExternalProject would
# not, and this project uses none. `FEATHERS_USE_CCACHE=OFF` or an absent binary leaves the compile
# line exactly as it was.
option(FEATHERS_USE_CCACHE "Front the compiler with ccache when one is on PATH (a launcher, not a flag)" ON)
find_program(FEATHERS_CCACHE ccache)
if(FEATHERS_USE_CCACHE AND FEATHERS_CCACHE)
	set(CMAKE_C_COMPILER_LAUNCHER ${FEATHERS_CCACHE})
	set(CMAKE_CXX_COMPILER_LAUNCHER ${FEATHERS_CCACHE})
	message(STATUS "feathers: compiler launcher ${FEATHERS_CCACHE}")
endif()

# SHARED FETCHED SOURCES. The other half of the fresh-worktree tax: ccache above makes the COMPILE of
# bgfx, freetype, the VST3 SDK, RtAudio and RtMidi free the second time, but each build directory
# still CLONES them (MEASURED 2026-09-02: 253-295 s of a 286 s warm-cache fresh build was the
# configure, nearly all of it network). visage and clap-wrapper both offer a by-path override for
# exactly this -- FETCHCONTENT_SOURCE_DIR_<NAME> and <NAME>_SDK_ROOT -- and scripts/sanitize-gate.sh
# has been passing them on its command line since it was written. This lifts that into the configure
# so every directory gets it: point FEATHERS_SHARED_DEPS at a configured build directory's `_deps`
# and its fetched sources (plus the `cpm/` beside it) are reused instead of cloned.
#
# THE DEFAULT IS THE MAIN WORKSPACE'S `build/_deps`, found through git rather than spelled: a
# worktree's `--git-common-dir` is the main checkout's `.git`, so `<that>/../build/_deps` is the one
# directory every worktree on this host shares. In the main workspace itself that is its own
# `build/`, which is the right answer for `build-asan` and a no-op for `build` (a directory never
# reuses itself -- see the guard).
#
# GUARDED BY TAG, NOT BY PRESENCE. A source directory that exists but sits at another revision would
# build against the wrong bgfx with no error from anyone, so each candidate is used only if
# `git describe --tags --exact-match` in it equals the tag the vendored CMake declares (parsed from
# that file, so a bump in visage or clap-wrapper retires the stale copy rather than silently keeping
# it). Anything that fails the check falls back to the fetch, and says so.
set(FEATHERS_SHARED_DEPS "" CACHE PATH
	"A configured build directory's _deps to reuse fetched sources from (empty: the main workspace's build/_deps if it exists)")
if(NOT FEATHERS_SHARED_DEPS)
	execute_process(COMMAND git -C "${CMAKE_SOURCE_DIR}" rev-parse --git-common-dir
		OUTPUT_VARIABLE _feathers_gitdir OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE _feathers_gitrc)
	if(_feathers_gitrc EQUAL 0 AND _feathers_gitdir)
		get_filename_component(_feathers_gitdir "${_feathers_gitdir}" ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
		get_filename_component(_feathers_main "${_feathers_gitdir}" DIRECTORY)
		set(FEATHERS_SHARED_DEPS_EFFECTIVE "${_feathers_main}/build/_deps")
	endif()
else()
	set(FEATHERS_SHARED_DEPS_EFFECTIVE "${FEATHERS_SHARED_DEPS}")
endif()

# The tag a vendored CMake file declares for one fetched dependency: the first uncommented GIT_TAG
# line after the line that names it. Empty if not found, which the caller treats as "do not reuse".
function(feathers_declared_tag OUT_VAR FILE MARKER)
	set(${OUT_VAR} "" PARENT_SCOPE)
	file(STRINGS "${FILE}" _lines)
	set(_in 0)
	foreach(_line IN LISTS _lines)
		if(_in AND _line MATCHES "^[ \t]*GIT_TAG[ \t]+\"?([^\" \t)]+)")
			set(${OUT_VAR} "${CMAKE_MATCH_1}" PARENT_SCOPE)
			return()
		elseif(_line MATCHES "${MARKER}")
			set(_in 1)
		endif()
	endforeach()
endfunction()

# One candidate: reuse DIR for the dependency if it is checked out at exactly TAG.
function(feathers_reuse_source WHAT DIR TAG VAR)
	if(NOT TAG)
		message(STATUS "feathers: ${WHAT} not reused -- could not read its declared tag")
		return()
	endif()
	if(NOT IS_DIRECTORY "${DIR}")
		message(STATUS "feathers: ${WHAT} not reused -- no ${DIR}")
		return()
	endif()
	execute_process(COMMAND git -C "${DIR}" describe --tags --exact-match
		OUTPUT_VARIABLE _have OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE _rc)
	if(NOT _rc EQUAL 0 OR NOT _have STREQUAL TAG)
		if(NOT _have)
			# No exact tag at HEAD: name the revision so the message says what IS there.
			execute_process(COMMAND git -C "${DIR}" rev-parse --short HEAD
				OUTPUT_VARIABLE _have OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
		endif()
		message(STATUS "feathers: ${WHAT} not reused -- ${DIR} is at '${_have}', declared ${TAG}")
		return()
	endif()
	set(${VAR} "${DIR}" PARENT_SCOPE)
	message(STATUS "feathers: ${WHAT} ${TAG} reused from ${DIR}")
endfunction()

if(FEATHERS_SHARED_DEPS_EFFECTIVE AND IS_DIRECTORY "${FEATHERS_SHARED_DEPS_EFFECTIVE}")
	get_filename_component(_feathers_shared "${FEATHERS_SHARED_DEPS_EFFECTIVE}" ABSOLUTE)
	get_filename_component(_feathers_own "${CMAKE_BINARY_DIR}/_deps" ABSOLUTE)
	if(_feathers_shared STREQUAL _feathers_own)
		message(STATUS "feathers: shared deps is this build directory; fetching as usual")
	else()
		get_filename_component(_feathers_shared_cpm "${_feathers_shared}/../cpm" ABSOLUTE)
		set(_visage_graphics "${CMAKE_CURRENT_SOURCE_DIR}/libs/visage/visage_graphics/CMakeLists.txt")
		set(_clap_sdks "${CMAKE_CURRENT_SOURCE_DIR}/libs/clap-wrapper/cmake/base_sdks.cmake")
		feathers_declared_tag(_tag "${_visage_graphics}" "FetchContent_Declare\\(bgfx ")
		feathers_reuse_source(bgfx "${_feathers_shared}/bgfx-src" "${_tag}" FETCHCONTENT_SOURCE_DIR_BGFX)
		feathers_declared_tag(_tag "${_visage_graphics}" "FetchContent_Declare\\(freetype ")
		feathers_reuse_source(freetype "${_feathers_shared}/freetype-src" "${_tag}" FETCHCONTENT_SOURCE_DIR_FREETYPE)
		feathers_declared_tag(_tag "${_clap_sdks}" "NAME \"vst3sdk\"")
		feathers_reuse_source(vst3sdk "${_feathers_shared_cpm}/vst3sdk" "${_tag}" VST3_SDK_ROOT)
		feathers_declared_tag(_tag "${_clap_sdks}" "NAME \"rtaudio\"")
		feathers_reuse_source(rtaudio "${_feathers_shared_cpm}/rtaudio" "${_tag}" RTAUDIO_SDK_ROOT)
		feathers_declared_tag(_tag "${_clap_sdks}" "NAME \"rtmidi\"")
		feathers_reuse_source(rtmidi "${_feathers_shared_cpm}/rtmidi" "${_tag}" RTMIDI_SDK_ROOT)
	endif()
elseif(FEATHERS_SHARED_DEPS_EFFECTIVE)
	message(STATUS "feathers: no shared deps at ${FEATHERS_SHARED_DEPS_EFFECTIVE}; fetching as usual")
endif()

include(cmake/CompilerWarnings.cmake)
include(cmake/FeathersDevices.cmake)

# THE DEVICE SDK — the MIT-licensed part of the device vocabulary.
# Its headers MOVED out of src/device/ (one source of truth), and they keep their `device/…` spelling, so
# every `#include "device/device.h"` in the tree resolves unchanged. DIRECTORY-WIDE, before any target, because
# ~200 targets reach `device/` through their own `src` include and none of them should have to learn the move.
# The SDK's own targets see ONLY this directory, which is what makes them the proof of separation.
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/libs/feathers-device-sdk/include)
add_subdirectory(libs/feathers-device-sdk)

# SANITIZER BUILDS — a SEPARATE configure, never the shared `build` or `build-install-on`.
#
#   cmake -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DFEATHERS_SANITIZE=address
#
# Three use-after-frees shipped through a green `ctest` in one week (b36e197, e9b84ee, and the
# aligned-new/unaligned-delete in the rack engine), because reading freed memory does not fault and
# every test here asserts values. `scripts/sanitize-gate.sh` is the thing that FAILS; this option is
# only the build half of it; the script's header has the recipe.
#
# `-UNDEBUG` is not optional: RelWithDebInfo defines NDEBUG, which compiles `~FrameWatchers`'s
# ordering assert to nothing — the one assert written for exactly this class. Compile options land
# AFTER the per-config flags on the command line, so the -U wins; the gate script checks the real
# compile line rather than trusting this sentence.
set(FEATHERS_SANITIZE "" CACHE STRING "Build everything under a sanitizer: address, thread, or empty for none")
set_property(CACHE FEATHERS_SANITIZE PROPERTY STRINGS "" address thread)
if(FEATHERS_SANITIZE)
	if(NOT FEATHERS_SANITIZE MATCHES "^(address|thread)$")
		message(FATAL_ERROR "FEATHERS_SANITIZE must be 'address' or 'thread', got '${FEATHERS_SANITIZE}'")
	endif()
	add_compile_options(-fsanitize=${FEATHERS_SANITIZE} -fno-omit-frame-pointer -g -UNDEBUG)
	add_link_options(-fsanitize=${FEATHERS_SANITIZE})
endif()

# The macOS editor and macos-probe are written in Objective-C++ (.mm). visage enables OBJC/OBJCXX in
# its own subdirectory, but under the Ninja generator that does NOT register the OBJCXX compile rule
# for targets declared in the parent scope — CMake errors at generation time with
# "CMAKE_OBJCXX_COMPILE_OBJECT not set". Enabling the languages here, at the top level and only on
# Apple, loads their information module project-wide so BOTH visage's .mm sources and feathers' own
# macos_probe.mm compile. Linux/Windows are untouched.
if(APPLE)
	enable_language(OBJC OBJCXX)
endif()

# The golden capture pins exact bits. Contracting a*b+c into an FMA changes the rounding, so a host
# with FMA would disagree with one without — drift the golden cannot tell apart from a real character
# change. This flag makes every target round identically, and is applied to every translation unit
# that recomputes character or eval math. MSVC has no direct counterpart: it does not contract under
# /fp:precise, which is already its default, so naming it is how we say so out loud rather than
# leaning on a default someone could change.
if(MSVC)
	set(FEATHERS_FP_EXACT /fp:precise)
else()
	set(FEATHERS_FP_EXACT -ffp-contract=off)
endif()


# Firmware sources, compiled unmodified. src/shims comes first on the include path so the three
# shadow headers win over the firmware originals.
add_library(feathers-character STATIC
	src/character/phi_morph.cpp
	src/character/phi_stair.cpp
	src/character/phi_weave.cpp
	# THE PLATFORM-EXACT SINE AND COSINE are compiled at the lowest layer so the character layer can reach them too
	# (feathers-dsp links this target PUBLIC): the Weave ring's home shape is built here, and macOS's sinf rounding
	# a few of its arguments differently was what made Resonate buzz differently there (backlog LP, 2026-10-06).
	src/dsp/portable_trig.cpp
)
target_include_directories(feathers-character PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

# WARNINGS APPLY HERE NOW. This target was excluded from feathers-warnings while it compiled
# the upstream firmware tree unmodified — see cmake/CompilerWarnings.cmake, which still says so about
# firmware sources in general. These are Owlet's own files (every one carries Owlet's copyright),
# they are ours to edit, and they build clean under -Wall -Wextra.
target_link_libraries(feathers-character PRIVATE feathers-warnings)
target_compile_options(feathers-character PRIVATE ${FEATHERS_FP_EXACT})

# SIMDe FOR THE SSE2-INTRINSIC HEADERS (src/dsp/fast_math_approx.h, tpt_one_pole.h, the filter's
# cytomic_svf.h): only a target with no SSE2 (arm64 macOS/Linux) reads it; x86-64 takes <emmintrin.h>.
# It was on feathers-fx alone, so when modulator and rack code began reaching those dsp headers the
# macOS CI leg failed ("simde/x86/sse2.h not found" in feathers-mod-rack) while Linux, which never reads
# it, stayed green. Every base target that can reach a dsp header links it, so no new consumer can miss it.
add_library(feathers-simd INTERFACE)
target_include_directories(feathers-simd SYSTEM INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/libs/simde)

# Plugin-owned DSP: float evaluation of what the character layer builds. Never the firmware's own
# render layer — that one is Q31 and lives on the far side of the submodule boundary.
add_library(feathers-dsp STATIC
	src/dsp/basic_shapes.cpp
	src/dsp/character_bank.cpp
	src/dsp/constellation.cpp
	src/dsp/flagship.cpp
	src/dsp/halfband.cpp
	src/dsp/ks_picture.cpp
	src/dsp/drum_picture.cpp
	src/dsp/drum_voice.cpp
	src/dsp/ks_string.cpp
	src/dsp/morph_renderer.cpp
	src/dsp/osc_voice.cpp
	src/dsp/puck_motion.cpp
	src/dsp/phase_warp.cpp
	src/dsp/slice_build.cpp
	src/dsp/partial_bank.cpp
	src/dsp/partial_spectrum.cpp
	src/dsp/phase_profile.cpp
	src/dsp/spectral_shaper.cpp
	src/dsp/vocode.cpp
	src/dsp/vocode_bands.cpp
	src/dsp/vox_bake.cpp
	src/dsp/stair_renderer.cpp
	src/dsp/weave_models.cpp
	src/dsp/weave_string.cpp
)
target_include_directories(feathers-dsp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
# No vendored DSP include paths: the one feathers-dsp used (sst-filters' HalfRateFilter, for the
# voice's decimator) was replaced by our own halfband in src/dsp/halfband.h during the licence
# migration (2026-09-24). Keep it that way — a GPL header reached from here would be reached from
# every voice.
target_link_libraries(feathers-dsp PUBLIC feathers-character feathers-simd PRIVATE feathers-warnings)
# Same reason as feathers-character: an FMA-contracted host must not disagree with one without.
target_compile_options(feathers-dsp PRIVATE ${FEATHERS_FP_EXACT})

# The CLAP SDK and its helper templates, both header-only. SYSTEM for the same reason the sst
# includes above are: clap-helpers' Plugin<> expands a lot of code inside our translation unit, and
# holding somebody else's headers to our -Wall -Wextra is not a bug we can fix.
add_library(feathers-clap-sdk INTERFACE)
target_include_directories(feathers-clap-sdk SYSTEM INTERFACE
	${CMAKE_CURRENT_SOURCE_DIR}/libs/clap/include
	${CMAKE_CURRENT_SOURCE_DIR}/libs/clap-helpers/include
)

# The parameter contract, built apart from the plugin so tests can reach it without a host. It needs
# the CLAP headers for clap_id, the param flags and the stream types, and nothing else.
add_library(feathers-params STATIC src/plugin/params.cpp src/plugin/stack_index.cpp)
target_include_directories(feathers-params PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(feathers-params PUBLIC feathers-clap-sdk feathers-device PRIVATE feathers-warnings)

# THE DEVICE VOCABULARY — what any device this plugin HOSTS rather than implements declares about
# itself, and the one interface a widget implements to join the modulation surface. See
# src/device/device.h; it is src/fx/fx_tenant.h promoted, because every word of that file's argument
# was about devices rather than about effects.
#
# It links nothing and includes nothing. That is the whole point: this is the header a third-party
# device would compile against, so anything it dragged in would be something that device has to
# have too. The part of it that is ALREADY clean of GPL includes lives in libs/feathers-device-sdk (MIT)
# and is linked here; what stays in src/device/ is listed, with its blocker, in the SDK split note.
add_library(feathers-device STATIC src/device/background.cpp)
target_include_directories(feathers-device PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(feathers-device PUBLIC feathers-device-sdk feathers-simd PRIVATE feathers-warnings)

# THE STATE FORMAT'S TEXT (backlog JF): our JSON writer and reader, numbers by to_chars/from_chars. Links
# nothing — the rack's patch road and the tests take it; see src/state/json.h for why it is not vendored.
add_library(feathers-state STATIC src/state/json.cpp)
target_include_directories(feathers-state PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(feathers-state PRIVATE feathers-warnings)

# THE EVENT LOG (src/plugin/event_log.h): what the host did to each instance, in every build, so a rare
# "it forgot my patch" can be read after the fact. Both plugins link it. The commit it stamps on each line
# is regenerated at EVERY build (cmake/BuildStamp.cmake says why), and the header is rewritten only when the
# commit moves, so an unchanged commit rebuilds nothing.
set(FEATHERS_STAMP_DIR "${CMAKE_BINARY_DIR}/generated/stamp")
add_custom_target(feathers-build-stamp
	COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
	        -DOUTPUT=${FEATHERS_STAMP_DIR}/feathers_build_stamp.h -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/BuildStamp.cmake
	BYPRODUCTS ${FEATHERS_STAMP_DIR}/feathers_build_stamp.h
	COMMENT "Stamping the build's commit"
	VERBATIM)
add_library(feathers-event-log STATIC src/plugin/event_log.cpp)
add_dependencies(feathers-event-log feathers-build-stamp)
target_include_directories(feathers-event-log PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src PRIVATE ${FEATHERS_STAMP_DIR})
target_link_libraries(feathers-event-log PRIVATE feathers-warnings)
# Windows finds %LOCALAPPDATA% with SHGetKnownFolderPath (shell32) and frees it with CoTaskMemFree (ole32).
if(WIN32)
	target_link_libraries(feathers-event-log PRIVATE shell32 ole32)
endif()

# THE HOST SLOT POOL (backlog JU): which host slot names which device
# control, and in which state. Pure bookkeeping, main-thread only; it includes only the device vocabulary's
# instance id, and no CLAP and no visage. The CLAP glue (`slot_host`) is a separate target that links this.
add_library(feathers-hostslots STATIC src/hostslots/slot_pool.cpp)
target_include_directories(feathers-hostslots PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(feathers-hostslots PRIVATE feathers-warnings)
# THE SLOTS AS CLAP SEES THEM (`slot_host`, plan §1.1, T6a): the glue both plugins share, and the one hostslots
# file that includes CLAP. It formats a slot's value with its target's own formatter (`device::formatValue`).
add_library(feathers-hostslots-clap STATIC src/hostslots/slot_host.cpp)
target_link_libraries(feathers-hostslots-clap PUBLIC feathers-hostslots feathers-device feathers-clap-sdk
                      PRIVATE feathers-warnings)

# THE FX LANE'S TENANTS, and the boundary is the point. A device in this library owns its DSP, its
# parameter descriptors, its state payload and its card, and includes nothing from src/plugin — see
# src/fx/fx_tenant.h for why, and tests/test_fx_filter.cpp for the case that pins it by reading these
# sources and refusing a plugin include.
#
# It does NOT link feathers-dsp, although that target already carries the sst include paths this one
# needs. Taking them from there would drag the character layer and the firmware submodule in behind
# them, and a device that needs the whole synth to compile is not a device that could be hosted
# anywhere else. The three SYSTEM paths are repeated here on purpose; the duplication is the price of
# the boundary and it is a low one.
# PFFFT (libs/pffft, a submodule pinned to e1dbebc9; BSD-style UCAR/FFTPACK licence, LICENSE.txt there): the
# FFT for convolution (backlog JD) and whatever spectral work follows. The FLOAT core only; its bundled
# kissfft/pocketfft are for its own benchmarks and are not built. Static, so its export macro is empty.
add_library(feathers-pffft STATIC libs/pffft/src/pffft.c libs/pffft/src/pffft_common.c)
target_include_directories(feathers-pffft PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/libs/pffft/include
	PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/libs/pffft/include/pffft ${CMAKE_CURRENT_SOURCE_DIR}/libs/pffft/src)
target_compile_definitions(feathers-pffft PUBLIC PFFFT_STATIC_DEFINE PRIVATE _USE_MATH_DEFINES)
set_target_properties(feathers-pffft PROPERTIES POSITION_INDEPENDENT_CODE ON)

# The device sources register themselves (cmake/FeathersDevices.cmake, each src/fx/*/device.cmake); what stays
# here is the shared convolution engine, which belongs to no one device.
feathers_device_sources(FEATHERS_FX_DEVICE_SOURCES feathers-fx)
add_library(feathers-fx STATIC ${FEATHERS_FX_DEVICE_SOURCES}
                               src/fx/conv/test_ir.cpp
                               src/fx/conv/convolver.cpp
                               src/fx/conv/ir_file.cpp
                               src/fx/conv/ir_ref.cpp
                               src/fx/conv/ir_browse.cpp)
# feathers-mod for the MSEG's shape and geometry: Featherverb's IR envelope IS an MSEG shape, drawn and
# evaluated by the MSEG's own code (backlog JD). feathers-mod links only feathers-device, so no cycle.
target_link_libraries(feathers-fx PUBLIC feathers-pffft feathers-mod feathers-dsp)
# dr_wav / dr_flac (libs/dr_libs, public domain or MIT-0): single headers, implemented in ir_file.cpp only.
target_include_directories(feathers-fx SYSTEM PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/libs/dr_libs)
target_include_directories(feathers-fx PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
# simde (MIT) and nothing else vendored: sst-filters and sst-basic-blocks (GPL-3) were removed in the
# licence migration (2026-09-24). simde is here because src/fx/filter/cytomic_svf.h and
# src/dsp/fast_math_approx.h are written in SSE2 intrinsics and take them from simde where the target
# has no SSE2 (arm64 macOS/Linux); on x86-64 they include <emmintrin.h> and simde is never read.
# tests/test_fx_filter.cpp's "nothing in src or tools includes a GPL sst library" refuses them again.
target_include_directories(feathers-fx SYSTEM PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}/libs/simde
)
# feathers-dsp for RESONATE, whose string is the synth engine's WeaveString.
target_link_libraries(feathers-fx PUBLIC feathers-device feathers-dsp PRIVATE feathers-warnings)
# The filter is in the render path with everything else that carries this flag, and its response
# evaluator has to agree with what the sample loop computed on any host.
target_compile_options(feathers-fx PRIVATE ${FEATHERS_FP_EXACT})
# dr_wav, compiled into ir_file.cpp, trips MSVC's C4701 (maybe-uninitialised) in drwav_init. It is reported by the
# optimiser, which the `#pragma warning(push, 0)` around the include cannot reach, so it is off for that one file. The
# file's own code is held to the same class by GCC's -Wmaybe-uninitialized.
if(MSVC)
	set_source_files_properties(src/fx/conv/ir_file.cpp PROPERTIES COMPILE_OPTIONS /wd4701)
endif()

# THE MODULATOR RACK'S DEVICES, on the fx lane's terms exactly — see src/mod/dahdsr/dahdsr_types.h.
# A modulator owns its DSP, its settings, its state payload and its card, and includes nothing from
# src/plugin; tests/test_mod_dahdsr.cpp reads these sources and refuses a plugin include, the way
# test_fx_filter.cpp does for the lane.
#
# Separate from feathers-fx rather than folded into it, for the reason that library gives for not
# folding itself into feathers-dsp: an effect and a modulator are different kinds of device, and a
# target that holds both is a target neither can be lifted out of. It carries no vendored include path:
# the DAHDSR's envelope was sst-basic-blocks' until the licence migration (2026-09-24) and is our own now.
feathers_device_sources(FEATHERS_MOD_DEVICE_SOURCES feathers-mod)
add_library(feathers-mod STATIC ${FEATHERS_MOD_DEVICE_SOURCES})
target_include_directories(feathers-mod PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(feathers-mod PUBLIC feathers-device feathers-dsp PRIVATE feathers-warnings)
# An envelope's shape reaches the sample loop through every destination it is routed to, so it is
# under the same no-contraction rule as the layers it modulates.
target_compile_options(feathers-mod PRIVATE ${FEATHERS_FP_EXACT})


# WHAT IS IN THE RACK, as opposed to what a device in it does. Its own target because BOTH SIDES need
# it: the engine holds a ModRack and the editor publishes one, so it cannot live with the state road
# in feathers-gui-bridge without making the voice layer depend on the editor's parameter road. See
# the note at the top of src/plugin/mod_rack.cpp.
# THE RACK — the backplane's second host. Links the device libraries and the registry table, and
# NOTHING from the synth: no feathers-synth, no feathers-gui-bridge, no feathers-ui.
feathers_device_sources(FEATHERS_RACK_DEVICE_SOURCES feathers-rack)
add_library(feathers-rack STATIC ${FEATHERS_RACK_DEVICE_SOURCES} src/rack/rack_engine.cpp src/rack/rack_bridge.cpp src/rack/converger.cpp
                                 src/rack/chain_record.cpp
                                 src/rack/slot_owner.cpp
                                 src/rack/nested_slots.cpp
                                 src/rack/rack_state.cpp src/rack/fragment.cpp src/rack/nesting.cpp
                                 src/rack/extra_state_harvest.cpp
                                 src/rack/inner_chain.cpp src/rack/inner_chain_bundle.cpp src/rack/site_chains.cpp
                                 src/rack/device_browser.cpp src/rack/device_publishers.cpp)
# THE SAME FLAG AS feathers-fx, AND IT WAS MISSING HERE (2026-11-08): this library compiles the device sources a second time for
# the rack, so on Apple arm64 (clang contracts to FMA by default) the lane rendered through THESE objects and a lone device
# rendered through feathers-fx differed by 1e-8..2e-7 (test_lane_rack "reorder: two devices of one kind swap"). Every library
# that recomputes DSP math carries it.
target_compile_options(feathers-rack PRIVATE ${FEATHERS_FP_EXACT})

# THE CPU SPIKE RECORDER (backlog SR, src/plugin/spike_log.h): a ring, a JSONL writer and the minute's stats. Compiled in
# every build and REACHED only under FEATHERS_DIAGNOSTIC (the two plugins' process()), so a ship build links nothing of it.
add_library(feathers-spike-log STATIC src/plugin/spike_log.cpp)
target_include_directories(feathers-spike-log PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(feathers-spike-log PRIVATE feathers-warnings)

add_library(feathers-mod-rack STATIC src/plugin/mod_rack.cpp)
target_include_directories(feathers-mod-rack PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(feathers-mod-rack PUBLIC feathers-mod feathers-params PRIVATE feathers-warnings)

# The voice layer, built apart from the plugin for the same reason the param contract is: a test can
# drive notes into a Synth and read audio back out of it without standing up a host.
#
# sst-voicemanager is header-only, and it pulls sst-cpputils with it — its SmallHashMap is what keeps
# the manager's per-key group state off the heap, and its rtsan_support.h is what tags the note API
# as a realtime context. Vendored as a submodule rather than fetched at configure time, matching
# every other dependency here. SYSTEM for the same reason the sst includes above are.
add_library(feathers-synth STATIC src/plugin/synth.cpp src/plugin/lane_rack.cpp)
target_include_directories(feathers-synth PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_include_directories(feathers-synth SYSTEM PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}/libs/sst-voicemanager/include
	${CMAKE_CURRENT_SOURCE_DIR}/libs/sst-cpputils/include
)
target_link_libraries(feathers-synth PUBLIC feathers-dsp feathers-params feathers-fx feathers-fx-sklitter feathers-rack feathers-mod-rack PRIVATE feathers-warnings)
# Same reason as the libraries it sits on: an FMA-contracted host must not disagree with one without.
target_compile_options(feathers-synth PRIVATE ${FEATHERS_FP_EXACT})

# The plugin itself, as a static library rather than as the .clap module directly. This is
# clap-wrapper's "clap first" shape: the plugin exposes three functions (see feathers_entry.h) and
# each output format — CLAP, VST3, standalone — is that static library plus a per-format recompile of
# the one file that builds the clap_entry structure out of them. One implementation, three binaries,
# and no dynamic hunt for a sibling .clap at runtime the way the wrapper's other mode does.
# The editor's half of the parameter road, built apart from the editor itself for the same reason the
# param contract is built apart from the plugin: it is where the queue and the change detection live,
# it needs no window and no host, and a test should be able to call it directly.
add_library(feathers-gui-bridge STATIC src/plugin/audition.cpp src/plugin/gui_bridge.cpp)
target_include_directories(feathers-gui-bridge PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
# feathers-fx is PUBLIC because the bridge's fx channel takes a fx::filter::State in its interface —
# setFilterSettings — so every consumer of this library sees that type. It arrived with the channel:
# the lane's settings live on the bridge now, so the bridge has to know how to turn one into a row.
target_link_libraries(feathers-gui-bridge PUBLIC feathers-params feathers-mod-rack feathers-fx feathers-fx-sklitter feathers-rack PRIVATE feathers-warnings)
# Same flag as every layer that computes character. slice_basis.h is reachable from here — the
# orientation channel carries a generator, and the basis it decodes to is contractible arithmetic. No
# call instantiates it in this library today (verified with nm), which is exactly why the flag belongs
# here now rather than after the first readout call makes it matter and nobody remembers why.
target_compile_options(feathers-gui-bridge PRIVATE ${FEATHERS_FP_EXACT})

# THE EDITOR'S WIDGETS, COMPILED ONCE. Seven targets draw the real surfaces — the plugin itself, the
# two GUI test binaries, windowed-probe, the two census tools and bake-feature-norm — and each used
# to name these sources in a list of its own, so all 51 were compiled up to six times over. MEASURED
# 2026-08-28: 247 object edges for 51 sources, and compiling the set once costs 903 s of CPU, so the
# duplication was spending roughly 3,100 s where 900 s would do. One library, six consumers.
#
# THE LISTS IT REPLACES WERE ALSO DRIFTING APART, which is the better argument for it. windowed-probe,
# quad-census and gamma-churn-check compiled these files WITHOUT -ffp-contract=off while the plugin
# compiled them with it — so three venues that exist to measure the shipped surface were free to
# contract a*b+c where the shipped surface is not. That is the one difference this project treats as a
# bug rather than as a rounding, and a single library makes it unspellable rather than merely fixed.
#
# STATIC rather than OBJECT deliberately: quad-census and gamma-churn-check each need about twenty of
# these, and a static archive lets the linker take the members they use and leave the rest. That is
# safe here because nothing in this directory self-registers — widget_registry.h is header-only and
# holds no static initialisers — so no member exists only for a side effect at load. IF A WIDGET EVER
# STARTS REGISTERING ITSELF FROM A .cpp, THIS MUST BECOME AN OBJECT LIBRARY, or the registration will
# be dropped, silently, from every consumer that does not otherwise name that file.
#
# WHAT IS DELIBERATELY NOT HERE: gui.cpp, plugin_window.cpp, editor_prefs.cpp and preset_store.cpp.
# gui.cpp and plugin_window.cpp are the files in the editor that read FEATHERS_GUI_VISAGE or
# FEATHERS_DEBUG_PANEL — plugin_window.cpp joined them when the window shell was extracted out of
# gui.cpp, and putting it here first compiled its STUB branch and reported success. feathers-impl may
# compile it with the debug panel switched on while the probes never do — so it is genuinely a
# different translation unit per target and stays in each of them. Every file below is
# definition-independent; only header_bar.cpp reads anything at all (FEATHERS_VERSION), and it is
# given it here.
add_library(feathers-ui STATIC
	# Not widgets, but the two files the widgets CALL: preset_browser.cpp reaches
	# PresetStore and the rail reaches EditorPrefs. Left in feathers-impl they made the two
	# archives mutually dependent, which a static link cannot satisfy in one pass. Neither
	# reads FEATHERS_GUI_VISAGE or FEATHERS_DEBUG_PANEL, so neither has a per-target meaning
	# the way gui.cpp does.
	src/plugin/editor_prefs.cpp
	src/plugin/preset_store.cpp
	src/plugin/device_preset.cpp
	src/plugin/chain_preset.cpp
	src/plugin/preset_tags.cpp
	src/plugin/ui/item_stepper.cpp
	src/plugin/ui/celestial.cpp
	src/plugin/ui/controls.cpp
	src/plugin/ui/automod_card.cpp
	src/plugin/ui/automod_strip.cpp
	src/plugin/ui/owltt_card.cpp
	src/plugin/ui/owltt_expanded.cpp
	src/plugin/ui/owltt_graph.cpp
	src/plugin/ui/drawn_card.cpp
	src/plugin/ui/drawn_expanded.cpp
	src/plugin/ui/edit_history.cpp
	src/plugin/ui/env_card.cpp
	src/plugin/ui/trig_pad.cpp
	src/plugin/ui/escape_stack.cpp
	src/plugin/ui/expanded_canvas.cpp
	src/plugin/ui/expanded_overlay.cpp
	src/plugin/ui/expanded_views.cpp
	src/plugin/ui/explore_stage.cpp
	src/plugin/ui/drum_panel.cpp
	src/plugin/ui/spectrum_panel.cpp
	src/plugin/ui/spectrum_plot.cpp
	src/plugin/ui/feature_norm.cpp
	src/plugin/ui/feature_norm_baked.cpp
	src/plugin/ui/filter_card.cpp
	src/plugin/ui/device_control.cpp
	src/plugin/ui/mod_readout.cpp
	src/plugin/ui/fx_control.cpp
	src/plugin/ui/fx_lane.cpp
	src/plugin/ui/fragment_verbs.cpp
	src/plugin/ui/os_clipboard.cpp
	src/plugin/ui/lane_selection.cpp
	src/plugin/ui/card_factory.cpp
	src/plugin/ui/generic_card.cpp
	src/plugin/ui/sklitter_card.cpp
	src/plugin/ui/sklitter_expanded.cpp
	src/plugin/ui/fx_lane_rail.cpp
	src/plugin/ui/generic_expanded.cpp
	src/plugin/ui/featherverb_expanded.cpp
	src/plugin/ui/featherverb_card.cpp
	src/plugin/ui/delay_card.cpp
	src/plugin/ui/delay_strip.cpp
	src/plugin/ui/delay_expanded.cpp
	src/plugin/ui/ir_view.cpp
	src/plugin/ui/harm_card.cpp
	src/plugin/ui/harm_expanded.cpp
	src/plugin/ui/harm_scope.cpp
	src/plugin/ui/harm_envelope_view.cpp
	src/plugin/ui/harm_partial_strip.cpp
	src/plugin/ui/harmonic_row.cpp
	src/plugin/ui/header_bar.cpp
	src/plugin/ui/settings_screen.cpp
	src/plugin/ui/open_url.cpp
	src/plugin/ui/libra_door.cpp
	src/plugin/ui/engine_rail.cpp
	src/plugin/ui/knob.cpp
	src/plugin/ui/macro_polarity.cpp
	src/plugin/ui/macro_menu.cpp
	src/plugin/ui/macro_slot.cpp
	src/plugin/ui/encoder_face.cpp
	src/plugin/ui/relative_macro.cpp
	src/plugin/ui/macros_card.cpp
	src/plugin/ui/math_card.cpp
	src/plugin/ui/stereo_tool_card.cpp
	src/plugin/ui/sample_hold_card.cpp
	src/plugin/ui/stepseq_card.cpp
	src/plugin/ui/follower_card.cpp
	src/plugin/ui/follower_plot.cpp
	src/plugin/ui/arp_card.cpp
	src/plugin/ui/arp_expanded.cpp
	src/plugin/ui/pattern_card.cpp
	src/plugin/ui/pattern_expanded.cpp
	src/plugin/ui/pattern_likes_log.cpp
	src/plugin/ui/knob_tray.cpp
	src/plugin/ui/master_meter.cpp
	src/plugin/ui/mod_panel.cpp
	src/plugin/ui/mseg_card.cpp
	src/plugin/ui/mseg_control.cpp
	src/plugin/ui/lfo_card.cpp
	src/plugin/ui/notify_tray.cpp
	src/plugin/ui/orientation_row.cpp
	src/plugin/ui/param_control.cpp
	src/plugin/ui/param_row.cpp
	src/plugin/ui/perform_pad.cpp
	src/plugin/ui/perform_stage.cpp
	src/plugin/ui/preset_browser.cpp
	src/plugin/ui/rack_control.cpp
	src/plugin/ui/rail.cpp
	src/plugin/ui/stage_rail.cpp
	src/plugin/ui/shaper_card.cpp
	src/plugin/ui/shaper_expanded.cpp
	src/plugin/ui/automod_expanded.cpp
	src/plugin/ui/automod_pad.cpp
	src/plugin/ui/twinfield_pad.cpp
	src/plugin/ui/twinfield_card.cpp
	src/plugin/ui/shaper_pad.cpp
	src/plugin/ui/phaser_pad.cpp
	src/plugin/ui/sound_identity.cpp
	src/plugin/ui/sound_stack.cpp
	src/plugin/ui/source_grid.cpp
	src/plugin/ui/stack_index_plot.cpp
	src/plugin/ui/terrain.cpp
	src/plugin/ui/terrain_likes.cpp
	src/plugin/ui/likes_log.cpp
	src/plugin/ui/terrain_warp.cpp
	src/plugin/ui/like_surface.cpp
	src/plugin/ui/surface_layers.cpp
	src/plugin/ui/takeover_stack.cpp
	src/plugin/ui/chrome_hue.cpp
	src/plugin/ui/theme.cpp
	src/plugin/ui/tooltip.cpp
                                 src/plugin/ui/source_arm.cpp
	src/plugin/ui/tuning_row.cpp
	src/plugin/ui/switch_chip.cpp
	src/plugin/ui/text_field.cpp
	src/plugin/ui/type_in.cpp
	src/plugin/ui/keyboard_claim.cpp
	src/plugin/ui/value_box.cpp
	src/plugin/ui/fader_pill.cpp
	src/plugin/ui/vertical_fader.cpp
	src/plugin/ui/pill_stack.cpp
	src/plugin/ui/file_dialog.cpp
	src/plugin/ui/file_dialog_portal.cpp
	src/plugin/ui/file_dialog_linux.cpp
	src/plugin/ui/file_dialog_win.cpp
	src/plugin/ui/dbus_wire.cpp
	src/plugin/ui/wave_display.cpp
	src/plugin/ui/busutil_card.cpp
	src/plugin/ui/comp_card.cpp
	src/plugin/ui/comp_plot.cpp
	src/plugin/ui/comp_expanded.cpp
	src/plugin/ui/inject_card.cpp
	src/plugin/ui/eq_card.cpp
	src/plugin/ui/eq_controls.cpp
	src/plugin/ui/eq_expanded.cpp
	src/plugin/ui/eq_graph.cpp
	src/plugin/ui/disperser_card.cpp
	src/plugin/ui/disperser_plot.cpp
	src/plugin/ui/disperser_expanded.cpp
	src/plugin/ui/phaser_card.cpp
	src/plugin/ui/phaser_plot.cpp
	src/plugin/ui/phaser_expanded.cpp
	src/plugin/ui/clip_card.cpp
	src/plugin/ui/noise_card.cpp
	src/plugin/ui/noise_plot.cpp
	src/plugin/ui/rtrig_card.cpp
	src/plugin/ui/spectral_card.cpp
	src/plugin/ui/spectrum_source.cpp
	src/plugin/ui/spectral_source.cpp
	src/plugin/ui/spectral_expanded.cpp
	src/plugin/ui/resonate_card.cpp
	src/plugin/ui/resonate_expanded.cpp
	src/plugin/ui/resonate_face.cpp
	src/plugin/ui/resonate_meter.cpp
	src/plugin/ui/resonate_model_picker.cpp
	src/plugin/ui/resonate_ring_view.cpp
	src/plugin/ui/smooth_picture.cpp
	src/plugin/ui/smooth_card.cpp
	src/plugin/ui/smooth_expanded.cpp
	src/plugin/ui/chain_card.cpp
	src/plugin/ui/parallel_rack_card.cpp
	src/plugin/ui/rack_mode_views.cpp
	src/plugin/ui/parallel_rack_expanded.cpp
	src/plugin/ui/container_presets.cpp
	src/plugin/ui/chain_column.cpp
	src/plugin/ui/mod_route_menu.cpp
	src/plugin/ui/menu_anchor.cpp
	src/plugin/ui/waypoint_card.cpp
	src/plugin/ui/waypoint_expanded.cpp
	src/plugin/ui/windowed_expanded.cpp
	src/plugin/ui/zone_coord_cell.cpp
	src/plugin/ui/sigil.cpp
	src/plugin/ui/mode_value_cell.cpp
	src/plugin/ui/zone_select.cpp
)
target_include_directories(feathers-ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
# THE SETTINGS SCREEN'S DEBUG OPTIONS SECTION: its own section, so the whole of it can be switched
# off or hidden later. ON for now; a release build turns it OFF here and loses the section, its click road, and the
# stored repaint choice's effect (the canvas then keeps visage's compile-time FEATHERS_FULL_REDRAW default).
option(FEATHERS_DEBUG_OPTIONS "Settings screen: the DEBUG OPTIONS section (repaint switch, diagnostic frame times)" ON)
if (FEATHERS_DEBUG_OPTIONS)
	target_compile_definitions(feathers-ui PUBLIC FEATHERS_DEBUG_OPTIONS)
endif ()
# THE FACTORY PRESETS, EMBEDDED — every .fpreset under presets/factory, in both plugins (both link
# feathers-ui). Adding a file there rebuilds; see preset_store.h for how to add one.
include(cmake/EmbedPresets.cmake)
feathers_embed_presets(feathers-ui DIR ${CMAKE_CURRENT_SOURCE_DIR}/presets/factory FUNCTION factoryPresetFiles)
# **AND DELIBERATELY NOT `feathers-synth`: THE UI MAY NOT ADDRESS THE AUDIO-SIDE HOST.**
#
# This is the threading model of the standard, made a LINK-TIME fact rather than a convention. The UI
# reaches the engine through `ParamBridge` — `bridge_device_host.h` is an adapter that forwards and
# says in its own header that it must never decide anything — with `setFxChain` handing over per block
# and `provisionDevices` building on the message thread. That protocol is why Feathers has no
# equivalent of the rack's standing hazard, which `rack_engine.cpp` records in its own words: *"the
# editor mutates engine state directly from the UI thread with no handoff, so `removeDevice` still
# frees `storage` that a block may be inside."*
#
# **THE LINK WAS HERE AND WAS VESTIGIAL, WHICH MEANS THE SEPARATION WE ALL RELIED ON WAS A CONVENTION.**
# Measured: NO file under `src/plugin/ui/` includes `synth.h`, `fx_runner.h`, `mod_runner.h` or
# `device_instance_pool.h` — not one; the whole build and every test succeed without the link, because
# `feathers-impl` links the engine directly (below) which is the right place; and line ~735 already
# records another target as linking "DELIBERATELY NOT feathers-synth", so this is the house rule
# written down rather than a new one.
#
# **IT BITES, AND THAT WAS MEASURED TOO RATHER THAN ARGUED.** A probe adding
# `#include "plugin/device_instance_pool.h"` and one call to a UI translation unit fails to link —
# `undefined reference to feathers::DeviceInstancePool::DeviceInstancePool()` — in `quad-census`, a
# tool that links `feathers-ui` ALONE. Restoring the link and keeping the probe builds clean, which is
# what makes the refusal attributable to this line and not to something else. Any target that links the
# UI without the engine is the enforcement; there are several, and they run on every build.
target_link_libraries(feathers-ui PUBLIC feathers-gui-bridge PRIVATE feathers-warnings feathers-pffft) # pffft: the likes' fine spectra
# The event log, for the settings screen's build stamp (`eventlog::buildSha`, backlog LG) — the stamp the
# event log writes, so the About part and the log can never name two different builds.
target_link_libraries(feathers-ui PRIVATE feathers-event-log)
# THE OS FILE DIALOG'S PLATFORM HALVES (`file_dialog.h`). Linux speaks D-Bus by hand and links nothing
# new — the static-link posture, which `runtime-deps` checks. macOS is Objective-C++ under ARC, as
# visage's own is; Windows is COM from the system DLLs every Windows has.
if(APPLE)
	target_sources(feathers-ui PRIVATE src/plugin/ui/file_dialog_mac.mm)
	set_source_files_properties(src/plugin/ui/file_dialog_mac.mm PROPERTIES COMPILE_OPTIONS -fobjc-arc)
	target_link_libraries(feathers-ui PUBLIC "-framework AppKit")
elseif(WIN32)
	target_link_libraries(feathers-ui PUBLIC ole32 shell32 uuid)
endif()
target_compile_definitions(feathers-ui PRIVATE FEATHERS_VERSION="${PROJECT_VERSION}")
# THE EDITOR RECOMPUTES THE CHARACTER, so it carries the same flag the layers under it do — the note
# on feathers-impl below is where that requirement is argued, and it applies to these files first.
target_compile_options(feathers-ui PRIVATE ${FEATHERS_FP_EXACT})

add_library(feathers-impl STATIC
	src/plugin/feathers_clap.cpp
	src/plugin/gui.cpp
	src/plugin/ui/plugin_window.cpp
)
target_link_libraries(feathers-impl PUBLIC feathers-gui-bridge feathers-ui feathers-rack-editor)
target_link_libraries(feathers-impl PUBLIC feathers-synth feathers-clap-sdk feathers-hostslots-clap feathers-event-log
                      feathers-spike-log PRIVATE feathers-warnings)
target_include_directories(feathers-impl PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
# The version a host displays comes from project(), so a release bump is one line rather than two
# that can disagree. A plugin reporting a version it is not is a support conversation nobody can win.
# PUBLIC because the entry file is compiled into each format's target rather than into this library.
target_compile_definitions(feathers-impl PUBLIC FEATHERS_VERSION="${PROJECT_VERSION}")
# THE EDITOR RECOMPUTES THE CHARACTER, so it carries the same flag the layers under it do.
#
# It did not have to while the wave display only read tables somebody else had built. It does now:
# character_params.h resolves the blend weights and the whole of the blue shift in this translation
# unit, and a compiler free to contract a*b+c into an FMA here would draw a picture of a sound the
# engine is not making — the one difference this project treats as a bug rather than as a rounding.
target_compile_options(feathers-impl PRIVATE ${FEATHERS_FP_EXACT})

# clap-wrapper resolves its SDKs lazily, one per format actually asked for. The CLAP SDK is our own
# submodule; the VST3 SDK (VST3 target) and RtAudio/RtMidi (standalone target) are fetched at
# configure time, so the first configure of a fresh build directory needs network and later ones
# reuse what is already under build/. Vendoring those three as submodules would put ~500 MB into
# every clone to save that one download — not a trade worth making while the wrapper's own defaults
# work.
# Distributed Linux binaries must not assume the host's libstdc++: a plugin built with g++-14
# needs CXXABI_1.3.15, and sandboxed hosts (Bitwig's flatpak runtime ships gcc 13's libstdc++,
# ceiling 1.3.14) dlopen-fail and silently skip the plugin at scan time. Statically linking
# libstdc++/libgcc removes the versioned C++ runtime dependency entirely; glibc itself stays
# dynamic (our floor is GLIBC_2.29, over five years old). GNU/Linux only — MSVC and Apple
# toolchains have no equivalent problem.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
	add_link_options(-static-libstdc++ -static-libgcc)
endif()

# The editor. visage is a GPU renderer (bgfx) plus a UI toolkit, linked statically, which is the
# whole reason it was chosen over a webview: on a desktop host the only shared libraries the editor
# pulls in are the ones the platform's GPU framework already provides.
#
# Linux and macOS. visage's Cocoa/Metal backend (visage_windowing/macos) supplies the macOS window;
# the editor's graphics backend is selected by bgfx, which drives Metal there and Vulkan/GL on Linux.
# The gui extension is written to report no editor rather than a broken one when this is not built
# (see src/plugin/gui.cpp), so Windows builds and play as before.
#
# visage resolves bgfx and freetype with FetchContent at configure time rather than vendoring them.
# Same trade the VST3 SDK and RtAudio make above, for the same reason and with the same consequence:
# the first configure of a fresh build directory needs network, later ones reuse what is under
# build/. The versions are visage's to pin, and it does — bgfx v1.129.8958-499, freetype VER-2-14-1.
# WINDOWS TOO, since the widget library (feathers-ui) and the probe below compile against visage on every platform:
# without it they do not build at all. The Windows EMBED is not written yet (see PluginWindow::isAvailable), so
# there the editor is compiled and linked but never offered to a host.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_SYSTEM_NAME STREQUAL "Darwin" OR WIN32)
	# Its examples fetch their own floating copy of the CLAP SDK from `main`, which would put a second
	# unpinned clap in the build beside our submodule. Its tests fetch Catch2, and we test with
	# doctest. Neither is wanted, and both default ON when visage is the top-level project.
	set(VISAGE_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
	set(VISAGE_BUILD_TESTS OFF CACHE BOOL "" FORCE)

	# freetype comes down through visage's FetchContent and defaults to a SHARED library, which put
	# libfreetype.so.6 on the plugin's NEEDED list — a runtime dependency on a library that is not
	# there in a flatpak sandbox, which is the precise failure this whole approach exists to avoid.
	# Static, and with every optional codec off: freetype will link system zlib, libpng, brotli and
	# harfbuzz if it finds them, so a developer with those headers installed would otherwise produce a
	# plugin with more dependencies than CI's. We rasterise one bundled TTF and need none of them.
	#
	# BUILD_SHARED_LIBS is NOT scoped to freetype, and the wider reach is intended rather than
	# incidental. Set as a normal variable here, it is inherited by every directory added after it —
	# visage and everything its FetchContent pulls, and further down clap-wrapper along with the VST3
	# SDK and RtAudio it fetches. Static is what we want from all of them for the same reason we want
	# it from freetype: a .so beside the plugin is a .so that has to be found at load time. It also
	# changes nothing for the ones that were already static, since OFF is CMake's own default, and the
	# plugin and VST3 modules are MODULE libraries, which this does not govern.
	set(BUILD_SHARED_LIBS OFF)
	set(FT_DISABLE_ZLIB ON)
	set(FT_DISABLE_BZIP2 ON)
	set(FT_DISABLE_PNG ON)
	set(FT_DISABLE_HARFBUZZ ON)
	set(FT_DISABLE_BROTLI ON)

	add_subdirectory(libs/visage EXCLUDE_FROM_ALL)

	# A typeface has to be compiled in — visage rasterises from font bytes and there is no system font
	# lookup to fall back on. Lato is the one that ships inside visage, under the SIL OFL, and it is a
	# placeholder: Task 3 brings the mock's own type and this line goes with it.
	add_embedded_resources(feathers-ui-fonts "feathers_fonts.h" "resources::fonts"
		"${CMAKE_CURRENT_SOURCE_DIR}/libs/visage/examples/fonts/Lato-Regular.ttf")
	# OWLET'S MARKS, as `resources::branding::<name>_svg` — the libra constellation the header draws. Compiled
	# in for the reason the font is: the editor reads no file at run time.
	add_embedded_resources(feathers-ui-branding "feathers_branding.h" "resources::branding"
		"${CMAKE_CURRENT_SOURCE_DIR}/assets/branding/libra_constellation.svg")

	# FEATHERS' OWN SHADERS (backlog FE, the terrain as a planet), compiled by visage's shaderc for this platform's
	# renderer and embedded as `resources::shaders::<name>`. `varying.def.sc` sits beside them because shaderc reads it
	# from the shader's own folder.
	#
	# CONFIGURE_DEPENDS, AND IT IS NOT DECORATION: a plain GLOB is evaluated once, at configure time, so
	# an EXISTING build directory never learns that a new shader exists — `ninja` says nothing is out of
	# date and then the compile fails on `resources::shaders::<the new one>` not being a member. MEASURED
	# 2026-09-17, on the two shaders the bubble added: a fresh tree built them and both long-lived dirs
	# (`build`, `build-install-on`) did not, which is the "my build is green is not the repository is
	# buildable" rule arriving from its other side — here the FRESH checkout is the one that works.
	file(GLOB FEATHERS_SHADERS CONFIGURE_DEPENDS
		"${CMAKE_CURRENT_SOURCE_DIR}/src/plugin/ui/shaders/[vf]s_*.sc")
	visage_embed_shaders(feathers-ui-shaders "feathers_shaders.h" "resources::shaders" "${FEATHERS_SHADERS}")

	# The design tokens, generated from the mock's :root block so that block stays the only place a
	# colour is written. Regenerated whenever the mock changes, and the script leaves the file alone
	# when the result is identical, so editing prose in the mock does not rebuild the editor.
	set(FEATHERS_TOKENS_DIR ${CMAKE_BINARY_DIR}/generated)
	set(FEATHERS_TOKENS_HEADER ${FEATHERS_TOKENS_DIR}/feathers/tokens.h)
	file(MAKE_DIRECTORY ${FEATHERS_TOKENS_DIR}/feathers)
	add_custom_command(
		OUTPUT ${FEATHERS_TOKENS_HEADER}
		COMMAND ${CMAKE_COMMAND}
			-DMOCK=${CMAKE_CURRENT_SOURCE_DIR}/mock/feathers-ui-mock.html
			-DHEADER=${FEATHERS_TOKENS_HEADER}
			-P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenerateTokens.cmake
		DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/mock/feathers-ui-mock.html
			${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenerateTokens.cmake
		COMMENT "Generating design tokens from the mock"
		VERBATIM
	)
	# THE REQUIRED CREDITS for the settings screen's About part (backlog LG), generated from
	# THIRD-PARTY-NOTICES.md the same way, so the screen cannot drift from what ships.
	set(FEATHERS_CREDITS_HEADER ${FEATHERS_TOKENS_DIR}/feathers/credits.h)
	add_custom_command(
		OUTPUT ${FEATHERS_CREDITS_HEADER}
		COMMAND ${CMAKE_COMMAND}
			-DNOTICES=${CMAKE_CURRENT_SOURCE_DIR}/THIRD-PARTY-NOTICES.md
			-DHEADER=${FEATHERS_CREDITS_HEADER}
			-P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenerateCredits.cmake
		DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/THIRD-PARTY-NOTICES.md
			${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenerateCredits.cmake
		COMMENT "Generating the required credits from THIRD-PARTY-NOTICES.md"
		VERBATIM
	)
	add_custom_target(feathers-tokens DEPENDS ${FEATHERS_TOKENS_HEADER} ${FEATHERS_CREDITS_HEADER})
	add_dependencies(feathers-impl feathers-tokens)

	target_link_libraries(feathers-impl PRIVATE visage feathers-ui-fonts)
	target_include_directories(feathers-impl PRIVATE ${FEATHERS_TOKENS_DIR})
	# The widget library needs the same three, on the same platforms and for the same reasons. visage
	# and the tokens are PUBLIC because these files' HEADERS include both — a consumer of a card's
	# header cannot compile without them, and every consumer here is such a consumer.
	target_link_libraries(feathers-ui PUBLIC visage feathers-ui-shaders PRIVATE feathers-ui-fonts
		feathers-ui-branding)
	target_include_directories(feathers-ui PUBLIC ${FEATHERS_TOKENS_DIR})
	add_dependencies(feathers-ui feathers-tokens)
	target_compile_definitions(feathers-impl PRIVATE FEATHERS_GUI_VISAGE)

	# The Task 2 parameter panel: every parameter as a bare row, filling the stage. It was the whole
	# editor before there was a design, and it stays buildable because it is the cheapest way to see
	# all 24 values move at once — including the ones no designed control has reached yet. Off by
	# default, because shipping the scaffolding would be an editor lying about what it is.
	#
	#   cmake -B build -DFEATHERS_DEBUG_PANEL=ON
	option(FEATHERS_DEBUG_PANEL "Draw the raw parameter rows over the stage" OFF)
	if(FEATHERS_DEBUG_PANEL)
		target_compile_definitions(feathers-impl PRIVATE FEATHERS_DEBUG_PANEL)
	endif()
endif()

# THE macOS GUI INTEGRITY VENUE. An in-process assembly of the SHIPPED editor (the same Gui the
# plugin builds, from the same src/plugin/gui.cpp, reached through the test hook that already exists
# for the routing venue) opened in a real Cocoa window, so frames are genuinely drawn, composited and
# retained by the window server rather than read back from a headless buffer — which the repo's venue
# rules require ("a repaint cannot be observed headlessly"). It drives a parameter storm through the
# bridged ParamSet and captures before/after frames of the presented window, asserting the editor
# responded, that it is silent when nothing touches it, and that a deliberately injected control is
# caught. Mirror of tools/windowed-probe.cpp, but Cocoa + CoreGraphics capture instead of X11 +
# ImageMagick's `import`.
#
# Only built on macOS: the Cocoa/NSView embed is what this target is here to exercise, and the X11
# hosts above already cover the Linux venue. Off by default like the other developer tools — enable
# with -DFEATHERS_BUILD_TOOLS=ON.
#
# UNVERIFIED FROM THIS REPO: this target compiles on macOS in CI, but the Cocoa presentation timing
# and the CoreGraphics capture path are validated on a Mac, not in this Linux-first environment. Until
# then it is a compile-time gate plus a run scaffold; it FAILS LOUD (never false-green) until a Mac run
# confirms rest-silent + known-bad-fires + storm-differs.
option(FEATHERS_BUILD_TOOLS "Build developer tools (guihost, bitwighost, cyclehost, macos-probe)" OFF)
if(FEATHERS_BUILD_TOOLS AND CMAKE_SYSTEM_NAME STREQUAL "Darwin")
	# THE SAME SOURCE LIST AS windowed-probe: gui.cpp and plugin_window.cpp, the two editor files that
	# read FEATHERS_GUI_VISAGE and so must be compiled by the consumer that sets it. editor_prefs.cpp
	# and preset_store.cpp are feathers-ui members and come from it, as they do for every other consumer.
	add_executable(macos-probe tools/macos_probe.mm
		src/plugin/gui.cpp
		src/plugin/ui/plugin_window.cpp
	)
	# feathers-ui FIRST, as windowed-probe and the widget fixture library link it: gui.cpp is the tree
	# the widgets are placed in, and the widgets themselves (notify_tray, explore_stage, perform_stage,
	# sound_stack, wave_display, terrain — lines ~428-471) are feathers-ui sources. The third macOS CI
	# run (34096783452) got through the compile and stopped at THIS link with every one of them
	# undefined for arm64, all referenced from gui.cpp.o — the source-list/link-list class CLAUDE.md
	# names, in the one gui.cpp consumer that had been written without its Linux twin beside it.
	target_link_libraries(macos-probe
		PRIVATE feathers-ui feathers-rack-editor feathers-gui-bridge feathers-dsp feathers-character feathers-fx visage
		        feathers-ui-fonts feathers-warnings)
	target_include_directories(macos-probe PRIVATE ${FEATHERS_TOKENS_DIR})
	target_compile_definitions(macos-probe PRIVATE FEATHERS_VERSION="${PROJECT_VERSION}" FEATHERS_GUI_VISAGE)
	target_compile_options(macos-probe PRIVATE ${FEATHERS_FP_EXACT})
	add_dependencies(macos-probe feathers-tokens)

	# macos-probe is run as a dedicated CI step rather than a ctest case: its exit codes mean
	# different things (0 green, 1 detector dead, 2 editor failed, 3 INCONCLUSIVE) and ctest can't
	# soft-skip an INCONCLUSIVE without false-greening. Run it directly: build/macos-probe.
endif()

set(CLAP_SDK_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/libs/clap CACHE STRING "CLAP SDK — our own submodule")
set(CLAP_WRAPPER_DOWNLOAD_DEPENDENCIES ON CACHE BOOL "Fetch the VST3 SDK and RtAudio/RtMidi")

# CLAP-WRAPPER IS BUILT FROM A PATCHED COPY, NOT FROM THE SUBMODULE. libs/clap-wrapper is pinned to upstream free-audio/clap-wrapper v0.16.0 and we have NO
# fork of it, so a fix we need before upstream has one is carried as a patch file in
# patches/clap-wrapper/ (tracked here, so every worktree and every fresh clone gets it) and applied at
# configure time. The first one is backlog KD: a VST3 rescan refreshed `title` but not `shortTitle`, and
# widened the UTF-8 name byte by byte. DROP A PATCH WHEN UPSTREAM CARRIES ITS FIX and the pin moves past
# it -- this step says so, because the fresh copy then reports the patch as already applied.
#
# WHY A COPY IN THE BUILD DIRECTORY AND NOT THE SUBMODULE'S OWN WORKING TREE: a patched submodule is a
# DIRTY submodule, and `git diff --quiet HEAD` -- install-clap.sh's gate, which is frozen -- reports a
# dirty submodule, so every install would refuse. `ignore = dirty` in .gitmodules would quiet that, but
# only by blinding the gate to ANY edit in the wrapper, which is what it exists to see; and a patched
# working tree also makes `git submodule update` refuse the next pin bump. The copy leaves the tracked
# tree exactly as committed, so the gate keeps its full strength.
#
# THE COPY IS REFRESHED BY CONTENT, NOT BY PRESENCE: its stamp is a hash of every file in the submodule
# plus every patch, and every one of those files is a configure dependency. So a pin bump, a hand edit
# in libs/clap-wrapper, or a new or changed patch re-runs this step and re-copies; anything else leaves
# the copy (and its timestamps, so nothing recompiles) alone. Each patch is then IDEMPOTENT: already
# applied (`git apply --reverse --check`) is skipped, applies cleanly is applied, anything else is a
# FATAL_ERROR, because it means the pin moved under the patch.
find_package(Git REQUIRED)
set(_cw_src "${CMAKE_CURRENT_SOURCE_DIR}/libs/clap-wrapper")
set(_cw_root "${CMAKE_BINARY_DIR}/_patched")
set(_cw_copy "${_cw_root}/clap-wrapper")
set(_cw_patchdir "${CMAKE_CURRENT_SOURCE_DIR}/patches/clap-wrapper")
file(GLOB _cw_patches LIST_DIRECTORIES false "${_cw_patchdir}/*.patch")
list(SORT _cw_patches)
file(GLOB_RECURSE _cw_files LIST_DIRECTORIES false RELATIVE "${_cw_src}" "${_cw_src}/*")
# The submodule's `.git` is a gitdir pointer; copied, it would make the copy claim to be the submodule.
list(FILTER _cw_files EXCLUDE REGEX "^\\.git(/|$)")
if(NOT _cw_files)
	message(FATAL_ERROR "feathers: libs/clap-wrapper is empty -- run `git submodule update --init --recursive`")
endif()
set(_cw_manifest "")
set(_cw_deps "${_cw_patchdir}")
foreach(_f IN LISTS _cw_files)
	file(SHA256 "${_cw_src}/${_f}" _h)
	string(APPEND _cw_manifest "${_f} ${_h}\n")
	list(APPEND _cw_deps "${_cw_src}/${_f}")
endforeach()
foreach(_p IN LISTS _cw_patches)
	file(SHA256 "${_p}" _h)
	get_filename_component(_n "${_p}" NAME)
	string(APPEND _cw_manifest "patch ${_n} ${_h}\n")
	list(APPEND _cw_deps "${_p}")
endforeach()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_cw_deps})
string(SHA256 _cw_stamp "${_cw_manifest}")
set(_cw_old_stamp "")
if(EXISTS "${_cw_copy}.stamp")
	file(READ "${_cw_copy}.stamp" _cw_old_stamp)
endif()
# The copy sits inside this repository's build tree; the ceiling stops git from finding the enclosing
# repository, so `git apply` works on the copy as a plain directory.
set(_cw_git "${CMAKE_COMMAND}" -E env "GIT_CEILING_DIRECTORIES=${_cw_root}" "${GIT_EXECUTABLE}" apply)
set(_cw_refresh FALSE)
if(NOT IS_DIRECTORY "${_cw_copy}" OR NOT _cw_old_stamp STREQUAL _cw_stamp)
	set(_cw_refresh TRUE)
else()
	# Same inputs, but a hand edit inside the copy would still be compiled: every patch must read as
	# applied there, or the copy is rebuilt.
	foreach(_p IN LISTS _cw_patches)
		execute_process(COMMAND ${_cw_git} --reverse --check "${_p}"
			WORKING_DIRECTORY "${_cw_copy}" RESULT_VARIABLE _rc OUTPUT_QUIET ERROR_QUIET)
		if(NOT _rc EQUAL 0)
			set(_cw_refresh TRUE)
		endif()
	endforeach()
endif()
if(_cw_refresh)
	file(REMOVE "${_cw_copy}.stamp")
	# STAGE, THEN REPLACE ONLY WHAT DIFFERS, AND TOUCH IT. A plain re-copy is wrong in both directions:
	# a fresh mtime on every file recompiles the whole wrapper on every refresh, and the SOURCE's mtime
	# (what file(COPY) keeps) is older than an object built from a patched file -- so dropping a patch
	# left ninja linking the patched object. MEASURED: that known-bad stayed green until this was fixed.
	set(_cw_stage "${_cw_root}/clap-wrapper.stage")
	file(REMOVE_RECURSE "${_cw_stage}")
	file(MAKE_DIRECTORY "${_cw_stage}")
	file(COPY "${_cw_src}/" DESTINATION "${_cw_stage}" PATTERN ".git" EXCLUDE)
	foreach(_p IN LISTS _cw_patches)
		get_filename_component(_n "${_p}" NAME)
		execute_process(COMMAND ${_cw_git} --reverse --check "${_p}"
			WORKING_DIRECTORY "${_cw_stage}" RESULT_VARIABLE _rc OUTPUT_QUIET ERROR_QUIET)
		if(_rc EQUAL 0)
			message(WARNING "feathers: clap-wrapper patch ${_n} is ALREADY in libs/clap-wrapper -- upstream "
				"carries the fix, so delete patches/clap-wrapper/${_n}")
			continue()
		endif()
		execute_process(COMMAND ${_cw_git} --check "${_p}"
			WORKING_DIRECTORY "${_cw_stage}" RESULT_VARIABLE _rc OUTPUT_QUIET ERROR_VARIABLE _err)
		if(NOT _rc EQUAL 0)
			message(FATAL_ERROR "feathers: clap-wrapper patch ${_n} does not apply to libs/clap-wrapper -- "
				"the pin has moved under it. Rebase the patch on the new pin, or drop it if upstream fixed "
				"the defect.\n${_err}")
		endif()
		execute_process(COMMAND ${_cw_git} "${_p}"
			WORKING_DIRECTORY "${_cw_stage}" RESULT_VARIABLE _rc ERROR_VARIABLE _err)
		if(NOT _rc EQUAL 0)
			message(FATAL_ERROR "feathers: applying clap-wrapper patch ${_n} failed:\n${_err}")
		endif()
		message(STATUS "feathers: clap-wrapper patch ${_n} applied")
	endforeach()
	file(GLOB_RECURSE _cw_staged LIST_DIRECTORIES false RELATIVE "${_cw_stage}" "${_cw_stage}/*")
	set(_cw_changed 0)
	foreach(_f IN LISTS _cw_staged)
		if(EXISTS "${_cw_copy}/${_f}")
			file(SHA256 "${_cw_stage}/${_f}" _a)
			file(SHA256 "${_cw_copy}/${_f}" _b)
			if(_a STREQUAL _b)
				continue()
			endif()
		endif()
		get_filename_component(_d "${_cw_copy}/${_f}" DIRECTORY)
		file(MAKE_DIRECTORY "${_d}")
		file(COPY_FILE "${_cw_stage}/${_f}" "${_cw_copy}/${_f}")
		file(TOUCH "${_cw_copy}/${_f}")
		math(EXPR _cw_changed "${_cw_changed} + 1")
	endforeach()
	file(GLOB_RECURSE _cw_had LIST_DIRECTORIES false RELATIVE "${_cw_copy}" "${_cw_copy}/*")
	foreach(_f IN LISTS _cw_had)
		if(NOT _f IN_LIST _cw_staged)
			file(REMOVE "${_cw_copy}/${_f}")
			math(EXPR _cw_changed "${_cw_changed} + 1")
		endif()
	endforeach()
	file(REMOVE_RECURSE "${_cw_stage}")
	message(STATUS "feathers: clap-wrapper copy refreshed, ${_cw_changed} file(s) changed")
endif()
file(WRITE "${_cw_copy}.stamp" "${_cw_stamp}")
# THE WRAPPER CACHES ITS OWN ROOT, AND EVERY FORMAT'S SOURCES ARE BUILT FROM THAT CACHE ENTRY:
# `set(CLAP_WRAPPER_CMAKE_CURRENT_SOURCE_DIR ... CACHE STRING)` without FORCE, read by wrap_vst3,
# wrap_standalone, wrap_auv2 and the rest. In a build directory first configured before this block
# existed, the entry still names libs/clap-wrapper, so the copy supplied the INCLUDE path while every
# source compiled from the unpatched submodule. MEASURED 2026-09-25: the standalone lib then failed on
# a duplicate StandaloneHost, and the VST3 lib compiled the unpatched wrapasvst3.cpp with no error at
# all. So the entry is forced here, and the check deferred to the end of this file refuses any target
# that still compiles a source from the submodule.
set(CLAP_WRAPPER_CMAKE_CURRENT_SOURCE_DIR "${_cw_copy}" CACHE STRING "Clap Wrapper Source Location" FORCE)
# The same binary directory as before, so the build tree's layout does not move.
add_subdirectory("${_cw_copy}" "${CMAKE_BINARY_DIR}/libs/clap-wrapper" EXCLUDE_FROM_ALL)
function(feathers_refuse_unpatched_wrapper_sources)
	set(_bad "")
	foreach(_dir IN ITEMS "${CMAKE_SOURCE_DIR}" "${_cw_copy}")
		get_property(_targets DIRECTORY "${_dir}" PROPERTY BUILDSYSTEM_TARGETS)
		foreach(_t IN LISTS _targets)
			get_target_property(_srcs ${_t} SOURCES)
			foreach(_s IN LISTS _srcs)
				string(FIND "${_s}" "${_cw_src}/" _at)
				if(_at EQUAL 0)
					list(APPEND _bad "${_t}: ${_s}")
				endif()
			endforeach()
		endforeach()
	endforeach()
	if(_bad)
		list(JOIN _bad "\n  " _bad)
		message(FATAL_ERROR "feathers: these targets compile clap-wrapper from the UNPATCHED submodule "
			"instead of ${_cw_copy}:\n  ${_bad}")
	endif()
endfunction()
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL feathers_refuse_unpatched_wrapper_sources)

# Makes targets feathers_clap, feathers_vst3, feathers_standalone and the aggregate feathers_all;
# OUTPUT_NAME is what lands on disk. AUv2/AUv3/AAX are deliberately absent — macOS-only or
# licence-gated SDKs, and nothing in v1 asks for them.
make_clapfirst_plugins(
	TARGET_NAME feathers
	IMPL_TARGET feathers-impl

	OUTPUT_NAME "feathers"

	ENTRY_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/src/plugin/feathers_entry.cpp"

	BUNDLE_IDENTIFIER "com.owlet.feathers"
	BUNDLE_VERSION ${PROJECT_VERSION}

	# Where a musician's plugins live is theirs to manage, not the build's to decide.
	COPY_AFTER_BUILD FALSE

	PLUGIN_FORMATS CLAP VST3

	ASSET_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}

	# target-postfix, output name, clap id. The standalone hosts the plugin in-process, so the id has
	# to match kDescriptor.id in feathers_clap.cpp or it finds nothing to instantiate.
	STANDALONE_CONFIGURATIONS
	standalone "feathers-standalone" "com.owlet.feathers"
)

# THE RACK, AS ITS OWN PLUGIN — a second instantiation of the same template.
#
# SHIPPED AS "Feathers Chain" (feathers-chain.clap, com.owlet.feathers-chain) since backlog NP,
# 2026-10-18: it is a single series chain, and "Rack" now names parallel chains (backlog NO). The
# library targets (`feathers-rack`, `feathers-rack-ui`, `feathers-rack-editor`) and `src/rack/` keep
# their names: they are the chain engine, and renaming them is churn with nothing to buy.
#
# The device-browser design expected this to be an instantiation of the existing template, not new
# machinery, on the strength of the comment above `feathers-impl` that
# each output format is "that static library plus a per-format recompile". That held: this is the
# same call with different names.
#
# THE INTEGRATION PATH IS (1) THIS, then (2) Feathers hosting the rack in place of its own fx lane and
# modulator rack (decided 2026-08-30). So the standalone is not a demo of the rack, it is the shipping
# form of the thing Feathers will later contain, which is why it gets the full treatment rather than a
# test host.
make_clapfirst_plugins(
	TARGET_NAME feathers-chain-plugin
	IMPL_TARGET feathers-rack-ui

	OUTPUT_NAME "feathers-chain"

	ENTRY_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/src/rack/rack_entry.cpp"

	BUNDLE_IDENTIFIER "com.owlet.feathers-chain"
	BUNDLE_VERSION ${PROJECT_VERSION}

	# Where a musician's plugins live is theirs to manage, not the build's to decide.
	COPY_AFTER_BUILD FALSE

	PLUGIN_FORMATS CLAP VST3

	ASSET_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}

	# The id has to match kDescriptor.id in rack_clap.cpp or the standalone finds nothing to
	# instantiate — the same trap the note on Feathers' call names.
	STANDALONE_CONFIGURATIONS
	standalone "feathers-chain-standalone" "com.owlet.feathers-chain"
)

# THIRD-PARTY NOTICES TRAVEL WITH THE BINARIES. Several licences of what we link (bgfx/bx/bimg, TinyEXR,
# pffft's FFTPACK, FreeType's FTL credit, Twemoji's CC BY attribution) require their notice in the
# documentation that accompanies a binary. The .clap on Linux is ONE file, with nowhere inside it to
# put the notice, so the notice sits BESIDE it in the asset output directory. The .vst3 bundles also
# get a copy in Contents/Resources, which is where a bundle keeps its own files. configure_file
# re-copies whenever the source changes, because CMake re-configures on it.
# The install script copies only the .clap (it is a dev convenience, not a release); a release package
# must carry this file too.
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/THIRD-PARTY-NOTICES.md
	${CMAKE_BINARY_DIR}/THIRD-PARTY-NOTICES.md COPYONLY)
foreach(_vst3 IN ITEMS feathers:feathers_vst3 feathers-chain:feathers-chain-plugin_vst3)
	string(REPLACE ":" ";" _pair "${_vst3}")
	list(GET _pair 0 _name)
	list(GET _pair 1 _target)
	# NOT ON WINDOWS: there clap-wrapper links each .vst3 as ONE FILE (VST3/feathers.vst3 is the DLL itself), so it has
	# no Contents/ to put a notice in, and making the directory failed the link step. The notice ships beside the plugins
	# in the artifact (build/THIRD-PARTY-NOTICES.md), which is where a single-file plugin's documentation goes.
	set(_vst3_dir "${CMAKE_BINARY_DIR}")
	if(TARGET ${_target} AND NOT WIN32)
		add_custom_command(TARGET ${_target} POST_BUILD
			COMMAND ${CMAKE_COMMAND} -E make_directory
				"${_vst3_dir}/${_name}.vst3/Contents/Resources"
			COMMAND ${CMAKE_COMMAND} -E copy_if_different
				"${CMAKE_CURRENT_SOURCE_DIR}/THIRD-PARTY-NOTICES.md"
				"${_vst3_dir}/${_name}.vst3/Contents/Resources/THIRD-PARTY-NOTICES.md"
			VERBATIM)
	endif()
endforeach()

# The clap-wrapper configures the macOS CLAP target with MACOSX_BUNDLE but no
# MACOSX_BUNDLE_INFO_PLIST (wrap_clap.cmake), so CMake emits its default app
# plist — CFBundlePackageType = APPL, which is not a loadable-bundle type. A CLAP
# is a bundle and must declare BNDL, so feed a project plist; only CMake-
# substituted keys (all already populated by the auto-plist) are kept, to avoid
# any @VAR@ left un-substituted at generate time.
#
# BOTH clap targets, not just the first. Until 2026-09-06 only feathers_clap got this plist, so on
# macOS feathers-rack.clap (now feathers-chain.clap) was emitted with CMake's default APPL plist and no usable
# CFBundleExecutable — a bundle no host could load. The plist's CFBundleExecutable now comes from
# MACOSX_BUNDLE_BUNDLE_NAME (which wrap_clap.cmake sets to the OUTPUT_NAME, the file in
# Contents/MacOS/), so one template serves both. COMPILE-INFERRED for macOS from a Linux host; the CI
# macOS run is the verification.
if(APPLE)
	foreach(clap_target feathers_clap feathers-chain-plugin_clap)
		set_target_properties(${clap_target} PROPERTIES
			MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/cmake/macos/clap_Info.plist.in")
	endforeach()
endif()

# Real CLAP hosts for the editor, off by default because they are developer instruments rather than
# part of the product, and because they want X11 and an X display that nothing else here needs.
#
#   cmake -B build -DFEATHERS_BUILD_TOOLS=ON && ninja -C build guihost bitwighost
#   build/guihost build/feathers.clap /tmp     # engine -> editor, with no mouse input
#   build/bitwighost build/feathers.clap       # the embed protocol a real DAW performs
#
# They exist because the seams they test cannot be tested anywhere else — see each file's own header,
# and the venue notes there.
#
# The option is declared above the macOS venue block: CMake treats a second option() of the same name
# as a no-op, but we don't repeat it here.
if(FEATHERS_BUILD_TOOLS AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
	find_package(X11 REQUIRED)
	# The atlas control links visage rather than dlopening the plugin — it drives a Canvas directly,
	# which is the only way to test what a repack does to a retained frame without a GPU test suite.
	add_executable(atlas-repack-check tools/atlas-repack-check.cpp)
	target_link_libraries(atlas-repack-check PRIVATE visage feathers-ui-fonts feathers-warnings)

	# batch-overflow-check is RETIRED with visage v1.0.0 (2026-11-05): it measured the bgfx-era batcher's
	# quad ceiling, arena and damage instruments (kMaxQuadsPerBatch, transientQuadCapacity,
	# lastFrameDamage), none of which exists on the new GPU layer.

	# The raw-image control needs a real atlas upload and a real readback, so it belongs in the same
	# venue as the rest: a windowless canvas is the only place the byte order can be checked in pixels.
	add_executable(raw-image-check tools/raw-image-check.cpp)
	target_link_libraries(raw-image-check PRIVATE visage feathers-warnings)

	# The glyph-heal control is the atlas venue again: it drives a Font directly, because the deferral
	# it checks happens at pack time and never reaches a canvas at all.
	add_executable(glyph-heal-check tools/glyph-heal-check.cpp)
	target_link_libraries(glyph-heal-check PRIVATE visage feathers-ui-fonts feathers-warnings)

	# quad-census and gamma-churn-check are RETIRED with visage v1.0.0 (2026-11-05), for
	# batch-overflow-check's reason: quad-census sized bgfx's per-frame transient vertex arena from the
	# Explore surface under the chaos recipe (batchQuadsThisFrame, transientQuadCapacity), and
	# gamma-churn-check drove Perform + the stack rail under global-gamma churn and read the bgfx
	# batcher's SHORT/UNWALKED count-versus-walk reports. Neither arena nor batcher exists on the new GPU
	# layer, and nothing ships, tests or scripts against either tool. Sources stay in tools/ as the
	# record. What v1.0.0 leaves unmeasured — its ShapeBatcher chunker drops a batch silently when
	# `nextQuad` returns null — is a known gap.

	# THE ASSEMBLED EDITOR IN A REAL WINDOW, WITH A REAL GESTURE IN IT. Every other venue has exactly
	# one half of that: the hosts open a window but hold the plugin across the CLAP boundary and can
	# only reach it through the parameter API, and the widget suite reaches the tree but rasterises
	# nothing. So this links gui.cpp and the widgets the way tests/CMakeLists.txt does — the same
	# assembled editor, not a stand-in — and opens an X11 window of its own to embed it in. See the
	# file's header for what that combination is for.
	add_executable(windowed-probe tools/windowed-probe.cpp
		# gui.cpp only — the rest of the editor comes from feathers-ui. This file is compiled here
		# because it is the one that reads FEATHERS_GUI_VISAGE, set just below.
		src/plugin/gui.cpp
		src/plugin/ui/plugin_window.cpp
	)
	target_link_libraries(windowed-probe
		PRIVATE feathers-ui feathers-rack-editor feathers-gui-bridge feathers-dsp feathers-character feathers-fx visage
		        feathers-ui-fonts feathers-warnings ${X11_LIBRARIES})
	target_include_directories(windowed-probe PRIVATE ${FEATHERS_TOKENS_DIR} ${X11_INCLUDE_DIR})
	# The header draws the build stamp, and gui.cpp is only the visage editor when it is told to be —
	# the same two definitions the widget suite compiles it with, from the same place.
	target_compile_definitions(windowed-probe PRIVATE FEATHERS_VERSION="${PROJECT_VERSION}"
		FEATHERS_GUI_VISAGE)
	add_dependencies(windowed-probe feathers-tokens)

	# PHOTOGRAPH AN EXPANDED PANEL. Same assembled editor as windowed-probe and for the same reason —
	# a panel is not a parameter, so guihost cannot open one, and the widget suite rasterises nothing.
	# It opens the panel through `Gui::fxLaneForTest()->onExpandDevice`, the road the card's mark
	# takes, rather than by synthesising a click: see the file's header for why that road is closed.
	add_executable(widget-shot tools/widget-shot.cpp
		src/plugin/gui.cpp
		src/plugin/ui/plugin_window.cpp
	)
	target_link_libraries(widget-shot
		PRIVATE feathers-ui feathers-rack-editor feathers-gui-bridge feathers-dsp feathers-character feathers-fx visage
		        feathers-ui-fonts feathers-warnings ${X11_LIBRARIES})
	target_include_directories(widget-shot PRIVATE ${FEATHERS_TOKENS_DIR} ${X11_INCLUDE_DIR})
	target_compile_definitions(widget-shot PRIVATE FEATHERS_VERSION="${PROJECT_VERSION}"
		FEATHERS_GUI_VISAGE)
	add_dependencies(widget-shot feathers-tokens)

	# DOES ALT REACH THE KNOB THROUGH A REAL X11 WINDOW. Same assembled editor as windowed-probe, but
	# the hand is the X SERVER'S: the gesture is fed through XTEST so the modifier visage reads back
	# with XQueryPointer is the server's own state rather than a bit a test wrote into a MouseEvent.
	# That is the road every widget-suite Alt case stops short of, and the one backlog CU is about.
	# Needs libXtst, and a display XTEST is allowed on — venue-xvfb.sh, never `:1`.
	if(X11_Xtst_FOUND)
		add_executable(alt-probe tools/alt-probe.cpp
			src/plugin/gui.cpp
			src/plugin/ui/plugin_window.cpp
		)
		target_link_libraries(alt-probe
			PRIVATE feathers-ui feathers-rack-editor feathers-gui-bridge feathers-dsp feathers-character feathers-fx visage
			        feathers-ui-fonts feathers-warnings ${X11_LIBRARIES} ${X11_Xtst_LIB})
		target_include_directories(alt-probe PRIVATE ${FEATHERS_TOKENS_DIR} ${X11_INCLUDE_DIR})
		target_compile_definitions(alt-probe PRIVATE FEATHERS_VERSION="${PROJECT_VERSION}"
			FEATHERS_GUI_VISAGE)
		add_dependencies(alt-probe feathers-tokens)
	else()
		message(STATUS "feathers: alt-probe not built -- libXtst not found")
	endif()

	# WHICH KEYS REACH THE HOST WHILE A NAME IS TYPED. Same assembled editor and XTEST hand as alt-probe;
	# the parent window stands for the DAW and hears every key X delivers to it or visage forwards to it.
	# venue-xvfb.sh only, never `:1`.
	if(X11_Xtst_FOUND)
		add_executable(key-probe tools/key-probe.cpp
			src/plugin/gui.cpp
			src/plugin/ui/plugin_window.cpp
		)
		target_link_libraries(key-probe
			PRIVATE feathers-ui feathers-rack-editor feathers-gui-bridge feathers-dsp feathers-character feathers-fx visage
			        feathers-ui-fonts feathers-warnings ${X11_LIBRARIES} ${X11_Xtst_LIB})
		target_include_directories(key-probe PRIVATE ${FEATHERS_TOKENS_DIR} ${X11_INCLUDE_DIR})
		target_compile_definitions(key-probe PRIVATE FEATHERS_VERSION="${PROJECT_VERSION}"
			FEATHERS_GUI_VISAGE)
		add_dependencies(key-probe feathers-tokens)
	endif()

	# WHAT FRACTION OF THE CHARACTER SPACE CAN BE REACHED — an analysis tool, no behaviour attached.
	# Links the shipped basis code and the shipped banks so the numbers are the engine's own rather
	# than a restatement of them.
	add_executable(rank-census tools/rank-census.cpp)
	target_link_libraries(rank-census PRIVATE feathers-dsp feathers-character feathers-warnings)

	# BAKES THE FEATURE STANDARDISATION into a checked-in table — standardisation is "part of the artifact
	# generation (so we wont forget to do it manually and ci is a pita)". The suite's staleness case
	# recomputes what this writes and compares, so forgetting is a red test rather than a wrong colour.
	# It compiles terrain.cpp directly rather than linking the plugin, because it needs the feature
	# measurement and none of the widgets.
	# It links the table it GENERATES, which looks circular and is not: the baker measures RAW features
	# through terrainFeaturesAt, and only terrainColour reads the table. Baking is therefore a function
	# of the world alone rather than a fixpoint of its own previous output — worth stating, because a
	# generator that fed on its own artifact would converge to something nobody chose.
	add_executable(bake-feature-norm tools/bake-feature-norm.cpp)
	target_link_libraries(bake-feature-norm
		PRIVATE feathers-ui feathers-rack-editor feathers-gui-bridge feathers-dsp feathers-character visage
		        feathers-warnings)
	target_include_directories(bake-feature-norm PRIVATE ${FEATHERS_TOKENS_DIR})
	add_dependencies(bake-feature-norm feathers-tokens)

	foreach(tool guihost bitwighost cyclehost)
		# THE EVENT LOG OFF (src/plugin/event_log.h): the one file of ours beside the dlopened plugin, and it holds no
		# code of the plugin's — a static initialiser that sets FEATHERS_EVENT_LOG=off unless the caller set a path, so
		# a session's guihost run never writes lines into the user's log. The plugin reads the variable in-process.
		add_executable(${tool} tools/${tool}.cpp ${CMAKE_CURRENT_SOURCE_DIR}/tests/event_log_off.cpp)
		target_link_libraries(${tool} PRIVATE feathers-clap-sdk feathers-warnings ${X11_LIBRARIES} ${CMAKE_DL_LIBS})
		# They dlopen() the plugin rather than linking it, which is the point: they drive the shipped
		# artefact through the same C API a DAW does, with nothing of ours compiled in beside it.
		# `src` IS FOR THE ABI HEADER ONLY -- plugin/feathers_test_chain.h, which is a host-facing
		# extension declaration in the same sense <clap/clap.h> is. It does NOT relax the rule above:
		# still nothing of OURS is compiled in beside the dlopened plugin, and in particular the
		# device registry is not, so the host cannot hold a second copy of the device table. That is
		# why the extension addresses a control by NAME -- see feathers_test_chain.h.
		target_include_directories(${tool} PRIVATE ${X11_INCLUDE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/src)
	endforeach()
endif()

# Regenerates the golden character capture. Not run by the build — see tools/dump_character.cpp.
add_executable(dump_character tools/dump_character.cpp)
target_link_libraries(dump_character PRIVATE feathers-character feathers-warnings)
target_include_directories(dump_character PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tools)

enable_testing()
add_subdirectory(tests)

# THE CARD FLOOR'S GATE — the floor compiled against the DEVICE vocabulary alone, on every `ninja`.
#
# It links feathers-device and visage and DELIBERATELY NOT feathers-synth, feathers-params or
# feathers-gui-bridge, and its include path carries no clap. So a floor file that reaches the plugin —
# `plugin/params.h` through any chain — fails to compile HERE, in the default build, rather than being
# noticed when somebody remembers to build this target. That is the boundary the standards push
# stands on (the device standard needs a UI floor that does not reach the plugin), and it is a
# gate because it was a paragraph once: it linked on 2026-08-30 and was red again by 2026-09-01, via
# `fx_card.h -> expanded_views.h -> widget_registry.h -> params.h`, with nothing to say so.
#
# IT WAS EXCLUDE_FROM_ALL while it could not build ("a target that must not build yet has no
# business failing `ninja`"). It builds now, so it MUST — a check excluded from the default build is
# a check that exists and cannot fail on its own. If this target goes red, the fix is on the floor
# side (a card asks its `DeviceHost`; it does not include a host's table), never a link line added
# here.
#
# WHAT IT MEASURES AND WHAT IT CANNOT: a STATIC library compiles and archives, so this gate is the
# compiler's — an include that reaches the plugin fails; a symbol merely DECLARED and reached would
# not, and a header-only reach whose headers happen to compile (`theme.h -> dsp/constellation.h`)
# passes it. Backlog X and the vision doc §5.4 name the `nm` check that would close the first gap.
add_library(feathers-device-ui-probe STATIC
	src/plugin/ui/chrome_hue.cpp
	src/plugin/ui/theme.cpp
	src/plugin/ui/tooltip.cpp
	src/plugin/ui/notify_tray.cpp
	src/plugin/ui/switch_chip.cpp
	src/plugin/ui/text_field.cpp
	src/plugin/ui/type_in.cpp
	src/plugin/ui/keyboard_claim.cpp
	src/plugin/ui/value_box.cpp
	src/plugin/ui/knob.cpp
	src/plugin/ui/macro_polarity.cpp
	src/plugin/ui/macro_menu.cpp
	src/plugin/ui/macro_slot.cpp
	src/plugin/ui/encoder_face.cpp
	src/plugin/ui/macros_card.cpp
	src/plugin/ui/math_card.cpp
	src/plugin/ui/generic_card.cpp
	src/plugin/ui/sklitter_card.cpp
)
#   sklitter_expanded.cpp — a THIRD file came off for the same kind of reason (2026-09-15): it reaches
#     `generic_expanded.h`, which reaches the host's parameter space, and the probe said so at once
#     with `clap/ext/params.h: No such file`. `sklitter_card.cpp` stays because the CARD only needs
#     `generic_card.h`, which is floor material — the card and the panel are not coupled alike, and
#     this list is where that difference is recorded.
#
# TWO FILES CAME OFF THIS LIST ONCE THE PROBE SAID WHY THEY WERE FAILING, and the reclassification is
# a finding rather than a convenience — both are host-side BY NATURE, not merely coupled:
#
#   param_control.cpp — it IS the clap-param seating. A control addressed by clap_id is exactly what
#     a docked device does not have, so this belongs to whatever host owns a parameter space.
#   edit_history.cpp  — it reaches sound_identity.h and sound_hues.h for SoundIdentity and
#     SoundVariations. Those are Feathers' own undo entries. The generic undo STACK is floor material;
#     these particular records are not, and splitting them is separate work.
#
#   controls.cpp      — `Stepper` holds a `ParamBridge&` and a `ParamDesc&` as MEMBERS, so it is a
#     host-parameter widget by construction. `SegmentedControl` beside it has the same two-road shape
#     `Knob` had and could take the same treatment; nothing in the MVP floor needs it, because
#     `GenericCard` draws knobs only.
#
#   stereo_tool_card.cpp — never on it (backlog KU, 2026-10-05): its tabs are a `SegmentedControl`, which lives in
#     controls.cpp beside `Stepper` (above), so it reaches the host's parameter space the way `phaser_card.cpp` does.
#     It comes on with `SegmentedControl` the day that widget takes `Knob`'s treatment.
#
#   fx_control.cpp    — off the list for a third reason: it is the seating being REWRITTEN against
#     src/backplane, so probing today's version measures code that is going away.
target_include_directories(feathers-device-ui-probe PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(feathers-device-ui-probe PUBLIC feathers-device visage PRIVATE feathers-ui-fonts feathers-warnings)
# The generated token table, same as feathers-ui gets. A missing include path would read as coupling
# and is not — the probe must only report the coupling it exists to find.
add_dependencies(feathers-device-ui-probe feathers-tokens)
target_include_directories(feathers-device-ui-probe PRIVATE ${FEATHERS_TOKENS_DIR})

# THE RACK'S SURFACE — built entirely from the card floor, and linking `feathers-ui` for it. If this
# target ever needs `feathers-synth`, the floor was not finished.
# `rack_clap.cpp` IS HERE AND NOT IN feathers-rack, because the plugin has an editor: it names
# RackGui, which names PluginWindow, which reads FEATHERS_GUI_VISAGE. The engine below it stays
# UI-free and is what the headless suites link.
# THE RACK'S EDITOR, WITHOUT THE RACK PLUGIN — what Feathers links so a chain in its lane can open the
# inner rack's own editor (`chain_expanded.cpp`, rack roadmap §6). Split from feathers-rack-ui because
# that target carries `rack_clap.cpp` and its own `plugin_window.cpp`, which Feathers already has one of.
add_library(feathers-rack-editor STATIC src/rack/rack_editor.cpp
                                        src/rack/add_device.cpp
                                        src/rack/chain_expanded.cpp src/rack/site_expanded.cpp)
target_include_directories(feathers-rack-editor PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src ${FEATHERS_TOKENS_DIR})
target_link_libraries(feathers-rack-editor PUBLIC feathers-rack feathers-ui visage PRIVATE feathers-warnings)
target_compile_definitions(feathers-rack-editor PRIVATE FEATHERS_GUI_VISAGE
                                                      FEATHERS_VERSION="${PROJECT_VERSION}")
add_dependencies(feathers-rack-editor feathers-tokens)

add_library(feathers-rack-ui STATIC src/rack/rack_gui.cpp
                                    src/rack/rack_clap.cpp
                                    src/plugin/ui/plugin_window.cpp)
target_include_directories(feathers-rack-ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
# `feathers-hostslots-clap`: the standalone rack owns a host slot pool (T10) and answers for it as Feathers does.
target_link_libraries(feathers-rack-ui PUBLIC feathers-rack feathers-rack-editor feathers-ui feathers-hostslots-clap feathers-event-log
                      feathers-spike-log PRIVATE feathers-warnings)
# THE SAME FLAG gui.cpp NEEDS, for the same reason: `plugin_window.cpp` is a different translation
# unit per target and each consumer compiles its own.
#
# **AND THIS TARGET IS DEFINED DOWN HERE BECAUSE OF THAT FLAG.** It sat above with the other libraries
# behind an `if(TARGET visage)`, which is FALSE up there — visage arrives with
# `add_subdirectory(libs/clap-wrapper)` several hundred lines later. So the define was silently
# dropped, `plugin_window.cpp` compiled its STUB branch, and the build reported success. Twice: the
# WP2 probe target had the same bug for the same reason.
#
# The object is what says which branch you got. `nm` the .o for `XOpenDisplay`, or read DEFINES in
# build.ninja — the exit code cannot tell you.
target_compile_definitions(feathers-rack-ui PRIVATE FEATHERS_GUI_VISAGE
                                                  FEATHERS_VERSION="${PROJECT_VERSION}")
target_link_libraries(feathers-rack-ui PUBLIC visage)
target_include_directories(feathers-rack-ui PUBLIC ${FEATHERS_TOKENS_DIR})
add_dependencies(feathers-rack-ui feathers-tokens)
target_include_directories(feathers-rack PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(feathers-rack PUBLIC feathers-fx feathers-mod feathers-device feathers-clap-sdk feathers-state
                      feathers-hostslots PRIVATE feathers-warnings)
target_compile_definitions(feathers-rack PRIVATE FEATHERS_VERSION="${PROJECT_VERSION}")

# **SKLITTER — A DEVICE THAT HOLDS A CHAIN, SO IT SITS ABOVE THE RACK RATHER THAN BESIDE THE TENANTS.**
#
# It was in `feathers-fx` and cannot stay there, and the reason is the direction of the arrow above:
# `feathers-rack` links `feathers-fx`, because the rack is a HOST and the tenants are what it runs. A
# tenant that holds a `RackEngine` reverses that — a static-library cycle, and a header cycle too until
# `rack_engine.h`'s include of `sklitter_types.h` moved out (the one static_assert it was there for now
# lives in `src/plugin/device_registry.h`, beside `widestDeviceOutputs`, which is the same kind of
# claim). Backlog EW.
#
# **THE PRECEDENT IS THE ONE OTHER CONTAINER WE HAVE.** `rack::kDeviceDescriptor` — a rack AS a device —
# lives in `feathers-rack` for exactly this reason: a device that holds devices needs whatever runs
# them, so it cannot live below it. SKLITTER is the second, and this target is where a third goes.
#
# **WHAT IT COSTS, SAID PLAINLY.** SKLITTER's suites can no longer claim the fx boundary — *"it links
# feathers-fx and nothing else, so a link error here is the boundary"* — because it is not a plain
# tenant any more. Every OTHER device in `feathers-fx` still makes that claim and every other suite
# still checks it; what changed is one device's honest description of itself, not the rule.
add_library(feathers-fx-sklitter STATIC src/fx/sklitter/sklitter_types.cpp
                                        # RESONATE'S OPS (2026-10-05): the third container, two fx lanes — the
                                        # comment above said this target is where a third goes. Its DSP stays in
                                        # feathers-fx; only the holder of the chains moves up.
                                        src/fx/resonate/resonate.cpp
                                        # THE DELAY'S OPS (delay-love, 2026-11-04): the fourth holder, one lazy feedback lane.
                                        src/fx/delay/delay_ops.cpp
                                        # COMP'S OPS (backlog QG, 2026-11-07): the fifth holder, the Compander's lazy encode/decode lane
                                        # (`rack::LazyLane`, the Delay's component). Its DSP stays in feathers-fx.
                                        src/fx/comp/comp_ops.cpp
                                        src/fx/sklitter/sklitter.cpp)
target_include_directories(feathers-fx-sklitter PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(feathers-fx-sklitter PUBLIC feathers-fx feathers-rack PRIVATE feathers-warnings)
# UNDER THE SAME NO-CONTRACTION RULE AS THE LIBRARY IT LEFT: its slice arithmetic and its blend reach
# the sample loop, and a host that rounded differently would be a host that changed the sound.
target_compile_options(feathers-fx-sklitter PRIVATE ${FEATHERS_FP_EXACT})
# **THE CRUSH TRIPWIRE (2026-09-24) — `src/fx/sklitter/tripwire.h`.** The ship-flag diagnostic build logs
# SKLITTER's host inputs and wet anomalies to stderr (Bitwig's engine.log). PRIVATE, and read only in
# `sklitter.cpp`: the class layout does not depend on it, so the includers that never see the define
# (the plugin's non-UI units, the tests) agree with this one about what a `Sklitter` is.
if (FEATHERS_DIAGNOSTIC)
	target_compile_definitions(feathers-fx-sklitter PRIVATE FEATHERS_DIAGNOSTIC)
endif()


# THE macOS PRE-FLIGHT, REACHABLE BY SOMETHING RATHER THAN BY A HUMAN REMEMBERING (backlog R2).
#
# `scripts/clang-preflight.sh` is the only instrument here that sees the classes that break the macOS
# leg — clang-14 knows several that g++-14 does not — and it was on a checklist and in nothing else.
# An instrument wired into nothing is one a tired session skips, and the skip is invisible because
# nothing goes red to mark it. `ninja preflight` in any directory that has a compile database now runs
# it through `scripts/run-preflight.sh`, which fails on findings AND on a sweep that did not happen
# (see that file's header: the exit code alone cannot tell "clean" from "killed at minute two").
#
# A TARGET RATHER THAN A ctest ENTRY, and the reason is worth the line. A ctest entry is kept out of
# an ordinary `ctest` run by REGISTRATION, not by its label — the sanitize entries are inside
# `if(FEATHERS_SANITIZE)` for exactly that reason — so a labelled entry would put this sweep's several
# minutes into every run anybody makes, and an option-guarded one would need a configure of its own.
# A custom target needs neither: it costs nothing to a build that does not ask for it, it fails the
# command when the sweep fails, and it runs against the compile database the directory already has.
# If a ctest label ever earns its keep, it is a two-line follow-up (an option, an `add_test` with
# LABELS "preflight") and `run-preflight.sh` is unchanged either way.
#
# NOT in `all`, deliberately: minutes of clang `-fsyntax-only` over every TU of ours, tests included
# — five of the six warnings in the last survey were in `tests/`. MEASURED 6m30s on the first clean
# run (2026-09-04, 178 TUs); it said 15 minutes until then, an estimate made against a scan set of
# 375 that was more than half foreign code. `run-preflight.sh`'s COST section has the detail and the
# caveat that one run on a shared host is a shape, not a cost.
add_custom_target(preflight
	COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/run-preflight.sh ${CMAKE_BINARY_DIR}
	WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
	COMMENT "clang pre-flight over this directory's compile database (~6-7 min; macOS classes)"
	USES_TERMINAL
	VERBATIM
)
