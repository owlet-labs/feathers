# test targets accrete per task

# Same reason the libraries carry it: these translation units recompute character and eval math
# themselves, so a host that contracts a*b+c into an FMA must not disagree with one that does not.
add_compile_options(${FEATHERS_FP_EXACT})

# WINDOWS: the POSIX calls the suites make (setenv, unsetenv, usleep, clock_gettime, <unistd.h>), spelled once in
# win_compat/ and force-included, so no test file carries a Windows branch for them. See posix_compat.h.
# _CRT_NONSTDC_NO_WARNINGS: the UCRT declares getpid under its POSIX name and deprecates that name (C4996).
if(WIN32)
	add_compile_options(/FI${CMAKE_CURRENT_SOURCE_DIR}/win_compat/posix_compat.h)
	add_compile_definitions(_CRT_NONSTDC_NO_WARNINGS)
	include_directories(BEFORE ${CMAKE_CURRENT_SOURCE_DIR}/win_compat/include)
	add_library(feathers-test-win-compat STATIC win_compat/win_compat.cpp)
	link_libraries(feathers-test-win-compat)
	# AN 8 MB MAIN-THREAD STACK, as Linux and macOS give: the suites build a Synth (1.6 MB) and other engine objects as
	# locals, which the plugin keeps on the heap; Windows' 1 MB default overflowed in the first case of five suites.
	add_link_options(/STACK:8388608)
endif()

# THE EVENT LOG IS OFF IN EVERY TEST BINARY (src/plugin/event_log.h). Linked into every executable below as
# an object, whose static initialiser sets FEATHERS_EVENT_LOG=off unless the caller set it — so a binary run
# by hand is covered as well as one run by ctest, which sets the same variable at the end of this file. A
# test that inits the CLAP entry arms the DEFAULT file, which is the user's; nothing here may write it.
add_library(feathers-test-event-log-off OBJECT event_log_off.cpp)
link_libraries(feathers-test-event-log-off)

# AND EVERY TEST BINARY STARTS ON A SCRATCH DESK (backlog Q): XDG config/data/state pointed at a per-process temporary
# directory before any case, and `EditorPrefs::path()` aborts if it still resolves to the real home's editor.conf.
# Same posture as the event log above: an object, so a binary run by hand is covered as well as one run by ctest.
add_library(feathers-test-sandbox OBJECT test_sandbox.cpp)
link_libraries(feathers-test-sandbox)

# SHARDING A LONG SUITE ACROSS ctest ENTRIES.
#
# ctest runs test BINARIES in parallel, never the cases inside one, so a single long binary sets the
# floor for the whole run however many cores are idle behind it. MEASURED 2026-08-28, before this
# existed: `ctest -j16` took 176.1 s and `gui-widgets` alone took 176.09 s — the other 36 suites all
# finished inside it, and fifteen cores waited.
#
# THE SPLIT IS BY INDEX, NOT BY NAME. doctest's `--first`/`--last` select a range of the cases that
# pass the filters, so no name is ever typed — which matters here, because `--test-case=` splits its
# filter on commas and 329 of this repo's case names contain one (the trap scripts/run-cases.sh
# exists to catch). An index range cannot be fooled that way.
#
# `--order-by=name` because the DEFAULT ORDER IS DECLARATION ORDER, and that makes a shard's contents
# depend on where in the file a case happens to sit — so moving a case between two shards would be
# invisible in review and could turn a green run into a red one. Sorted by name, a shard's membership
# is a property of the names alone.
#
# THE LAST SHARD IS DELIBERATELY UNBOUNDED. A new TEST_CASE must never be able to land outside every
# shard: that failure is silent, and it is the same shape as the empty-filter trap — a green run
# reported for cases nobody executed. With the final `--last` past any plausible count, the shards
# cover 1..infinity by construction and the worst a drifting count can do is unbalance them.
#
# The case count is read from the source at configure time rather than written down, so the balance
# follows the file instead of going stale behind it.
function(feathers_count_test_cases OUT_VAR SOURCE)
	file(STRINGS "${SOURCE}" _lines REGEX "^TEST_CASE")
	list(LENGTH _lines _n)
	set(${OUT_VAR} ${_n} PARENT_SCOPE)
endfunction()

function(feathers_add_sharded_test)
	# VENUE: every shard runs under scripts/venue-xvfb.sh -- a private display of its own (-displayfd,
	# so concurrent runs cannot collide), lavapipe, VISAGE_FULL_REDRAW=1, scratch config and data.
	# MEASURED before it was switched on (2026-09-03, four full ctests alternated, gui-windowed-venue
	# worktree, load recorded before each): one venue around the whole ctest 272 s and 265 s (81/81,
	# load 3.9->5.5 and 7.8->4.8); a venue per widget shard 287 s (load 6.1->19.2) -- so about 15 s
	# per full run for fourteen displays, and no shard's own time moves. The second B run, 355 s, met
	# another session's build mid-run and is not a number. What it buys is the property gui-windowed
	# got the same day: two sessions' full suites on one desktop photographed each other (playbook
	# 8as, three reds in one hour). AND 8as's RULE STANDS WITH IT: a pixel-reading red inside a full
	# run is re-run alone before it is a finding -- B2 had one, "the response curve is drawn in greys
	# rather than in a blue-to-orange ramp" (`widestChroma 172 <= 40`) under load 20, green in the
	# other three runs and in every run since.
	cmake_parse_arguments(ARG "VENUE" "NAME;TARGET;SOURCE;SHARDS" "LABELS" ${ARGN})
	set(_launch "")
	if(ARG_VENUE)
		set(_launch ${CMAKE_SOURCE_DIR}/scripts/venue-xvfb.sh run --)
	endif()
	feathers_count_test_cases(_cases "${ARG_SOURCE}")
	if(_cases LESS 1)
		message(FATAL_ERROR "feathers_add_sharded_test(${ARG_NAME}): no TEST_CASE found in ${ARG_SOURCE} — "
			"the count is read from the source, and a zero would shard the suite into nothing")
	endif()
	math(EXPR _per "(${_cases} + ${ARG_SHARDS} - 1) / ${ARG_SHARDS}")
	set(_ranges "")
	foreach(_i RANGE 1 ${ARG_SHARDS})
		math(EXPR _first "(${_i} - 1) * ${_per} + 1")
		if(_i EQUAL ${ARG_SHARDS})
			set(_last 1000000)
		else()
			math(EXPR _last "${_i} * ${_per}")
		endif()
		set(_name "${ARG_NAME}-${_i}of${ARG_SHARDS}")
		add_test(NAME ${_name} COMMAND ${_launch} $<TARGET_FILE:${ARG_TARGET}> --order-by=name --first=${_first} --last=${_last})
		# A SHARD THAT EXECUTED NOTHING MUST GO RED, AND NOTHING ELSE HERE MAKES IT.
		#
		# MEASURED: a range entirely past the end of the suite prints
		#     test cases: 125 | 125 passed | 0 failed | 0 skipped
		#     Status: SUCCESS!          exit code 0
		# doctest's case tally counts the cases MATCHING the filters, not the cases it RAN, so a dead
		# shard reports a full green pass for zero work. `--first=1 --last=5` likewise prints "125
		# passed" while executing 167 assertions against the suite's 143,712. The one honest number
		# in that output is the ASSERTION count, so that is what this reads.
		#
		# THE STATIC COVERAGE GUARD BELOW DOES NOT COVER THIS, which is why both exist. That guard
		# asks `--list-test-cases` whether the RANGES name every case; this asks whether the shard,
		# when it actually ran, did anything. Listing and executing are different questions, and the
		# gap between them is exactly where a silent green lives.
		set_tests_properties(${_name} PROPERTIES FAIL_REGULAR_EXPRESSION "assertions: +0 +\\|")
		if(ARG_LABELS)
			set_tests_properties(${_name} PROPERTIES LABELS "${ARG_LABELS}")
		endif()
		list(APPEND _ranges "${_first}:${_last}")
	endforeach()
	# COMMA-JOINED, not passed as a CMake list: `-D` carries one argument, and a list's own `;`
	# separators are what add_test() splits arguments ON — so the semicolon form arrives at the script
	# as a single range and the other eleven vanish. Caught by this check failing when it was written
	# that way, which is the check doing its job before it had a bug to find.
	string(REPLACE ";" "," _ranges_arg "${_ranges}")

	# THE SHARDS ARE NOT TRUSTED TO COVER THE SUITE, THEY ARE MADE TO PROVE IT. A gap between two
	# ranges runs no case and reports nothing, so without this the split could quietly stop testing
	# whatever fell down it. The ranges come from the loop above rather than being restated, so the
	# check cannot agree with a boundary the shards do not actually use. See CheckShardCoverage.cmake
	# for why it compares names rather than counting assertions.
	#
	# It carries the suite's own labels: a coverage claim about a suite CI does not run must not be
	# made on a machine that cannot run it either.
	add_test(NAME ${ARG_NAME}-coverage COMMAND ${CMAKE_COMMAND}
		-DBINARY=$<TARGET_FILE:${ARG_TARGET}>
		-DRANGES=${_ranges_arg}
		-P ${CMAKE_SOURCE_DIR}/cmake/CheckShardCoverage.cmake)
	if(ARG_LABELS)
		set_tests_properties(${ARG_NAME}-coverage PROPERTIES LABELS "${ARG_LABELS}")
	endif()

	message(STATUS "sharded ${ARG_NAME}: ${_cases} cases over ${ARG_SHARDS} shards (~${_per} each)")
endfunction()


# THE SIX FIXED-POINT OPERATIONS, PINNED DIRECTLY. The golden capture below covers two of them —
# measured, not assumed, by breaking each in turn — so this is what holds the other three.
add_executable(test_fixedpoint test_fixedpoint.cpp)
target_link_libraries(test_fixedpoint PRIVATE feathers-warnings)
target_include_directories(test_fixedpoint PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest ${CMAKE_SOURCE_DIR}/src)
add_test(NAME fixedpoint COMMAND test_fixedpoint)

# THE φ-AUTHORED LFO SHAPE. Header-only and dependency-free but for the φ evaluator, so it links
# nothing — the same shape as test_fixedpoint above, and for the same reason: what is being pinned is
# arithmetic, and dragging a library in would only give it somewhere else to fail.
add_executable(test_phi_lfo_shape test_phi_lfo_shape.cpp)
target_link_libraries(test_phi_lfo_shape PRIVATE feathers-warnings)
target_include_directories(test_phi_lfo_shape PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest ${CMAKE_SOURCE_DIR}/src)
add_test(NAME phi-lfo-shape COMMAND test_phi_lfo_shape)

# THE AUTOMODULATOR'S LFO — the phase, the rate and the four taps, over the phi-authored shape.
# Header-only like the shape it reads, so it links nothing.
add_executable(test_automod_lfo test_automod_lfo.cpp)
target_link_libraries(test_automod_lfo PRIVATE feathers-warnings)
target_include_directories(test_automod_lfo PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest ${CMAKE_SOURCE_DIR}/src)
add_test(NAME automod-lfo COMMAND test_automod_lfo)

# THE AUTOMODULATOR'S COMB — a second CALLER of the delay line the filter's Comb type already runs,
# so it links feathers-fx for the shared sinc table rather than carrying a second copy of it.
add_executable(test_automod_comb test_automod_comb.cpp)
target_link_libraries(test_automod_comb PRIVATE feathers-fx feathers-warnings)
target_include_directories(test_automod_comb PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest ${CMAKE_SOURCE_DIR}/src)
add_test(NAME automod-comb COMMAND test_automod_comb)

# THE AUTOMODULATOR'S ENVELOPE FOLLOWER — level and derivative, per sample, in seconds.
add_executable(test_automod_envelope test_automod_envelope.cpp)
target_link_libraries(test_automod_envelope PRIVATE feathers-warnings)
target_include_directories(test_automod_envelope PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest ${CMAKE_SOURCE_DIR}/src)
add_test(NAME automod-envelope COMMAND test_automod_envelope)

# THE AUTOMODULATOR'S SPRING — one class, instantiated for the filter and the comb.
add_executable(test_automod_spring test_automod_spring.cpp)
target_link_libraries(test_automod_spring PRIVATE feathers-warnings)
target_include_directories(test_automod_spring PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest ${CMAKE_SOURCE_DIR}/src)
add_test(NAME automod-spring COMMAND test_automod_spring)

# THE AUTOMODULATOR'S FILTER — the SVF kernel addressed by a free (low, band, high) vector, with the
# coupling to the kernel's own mode table pinned by a bit-identity case.
add_executable(test_automod_filter test_automod_filter.cpp)
target_link_libraries(test_automod_filter PRIVATE feathers-fx feathers-warnings)
target_include_directories(test_automod_filter PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest ${CMAKE_SOURCE_DIR}/src)
add_test(NAME automod-filter COMMAND test_automod_filter)

# WHAT THREE ZONE COORDINATES MEAN — the phi field read as one pure function, so every number it
# decides is assertable without instantiating an effect.
add_executable(test_automod_derived test_automod_derived.cpp)
target_link_libraries(test_automod_derived PRIVATE feathers-fx feathers-warnings)
target_include_directories(test_automod_derived PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest ${CMAKE_SOURCE_DIR}/src)
add_test(NAME automod-derived COMMAND test_automod_derived)

# THE AUTOMODULATOR ITSELF — the assembly, which is the commit that gives the six components a caller.
add_executable(test_fx_automod test_fx_automod.cpp)
target_link_libraries(test_fx_automod PRIVATE feathers-fx feathers-warnings)
target_include_directories(test_fx_automod PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest ${CMAKE_SOURCE_DIR}/src)
add_test(NAME fx-automod COMMAND test_fx_automod)

add_executable(test_morph_renderer test_morph_renderer.cpp)
target_link_libraries(test_morph_renderer PRIVATE feathers-dsp feathers-warnings)
target_include_directories(test_morph_renderer PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest)
add_test(NAME morph-renderer COMMAND test_morph_renderer)

add_executable(test_stair_renderer test_stair_renderer.cpp)
target_link_libraries(test_stair_renderer PRIVATE feathers-dsp feathers-warnings)
target_include_directories(test_stair_renderer PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest)
add_test(NAME stair-renderer COMMAND test_stair_renderer)

add_executable(test_phi_weave_laws test_phi_weave_laws.cpp)
target_link_libraries(test_phi_weave_laws PRIVATE feathers-character feathers-warnings)
target_include_directories(test_phi_weave_laws PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest)
add_test(NAME phi-weave-laws COMMAND test_phi_weave_laws)

add_executable(test_weave_string test_weave_string.cpp)
target_link_libraries(test_weave_string PRIVATE feathers-dsp feathers-warnings)
target_include_directories(test_weave_string PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest)
add_test(NAME weave-string COMMAND test_weave_string "--test-case-exclude=*over phase half*")
# The bounded case, one entry per phase half, each well under a minute on a loaded host.
foreach(half 1 2)
	add_test(NAME weave-string-bounded-h${half} COMMAND test_weave_string
		"--test-case=weave string: bounded for three simulated minutes over phase half ${half} of 2")
endforeach()

# THE WEAVE PHYSICS MODELS (weave_models.h): Bar, Tube, Membrane and Stiffening against the Ring they
# generalise. Its own binary because its bounded cases are long, and ctest can run it beside the ring's.
add_executable(test_weave_models test_weave_models.cpp)
target_link_libraries(test_weave_models PRIVATE feathers-dsp feathers-warnings)
target_include_directories(test_weave_models PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest)
# The stress cases (bounded and bow x4) are one ctest entry per model and phase quarter so ctest -j runs
# them side by side; the rest run as one entry. No case name here has a comma, so the doctest filters
# below cannot split.
add_test(NAME weave-models COMMAND test_weave_models
	"--test-case-exclude=*over phase quarter*")
# The stress cases, one entry per model and phase quarter, each under about a minute on a loaded host.
foreach(quarter 1 2 3 4)
	foreach(model Bar Tube Membrane Stiffening)
		add_test(NAME weave-models-bounded-${model}-q${quarter} COMMAND test_weave_models
			"--test-case=weave models: ${model} stays bounded for three simulated minutes over phase quarter ${quarter} of 4")
	endforeach()
	foreach(model Ring Bar Tube Membrane Stiffening)
		add_test(NAME weave-models-x4-${model}-q${quarter} COMMAND test_weave_models
			"--test-case=weave models: ${model} at four times the bow stays finite with its rails as a net over phase quarter ${quarter} of 4")
	endforeach()
endforeach()

# THE RING PIN: a hash of the Ring's render, pinned from the code before the model switch existed, so
# "the Ring is unchanged by 2b" stays a test and not a claim. It asserts float bits, so it carries the
# platform-pinned label like vox-voice and flagship.
add_executable(test_weave_ring_pin test_weave_ring_pin.cpp)
target_link_libraries(test_weave_ring_pin PRIVATE feathers-dsp feathers-warnings)
target_include_directories(test_weave_ring_pin PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest)
add_test(NAME weave-ring-pin COMMAND test_weave_ring_pin)
set_tests_properties(weave-ring-pin PROPERTIES LABELS "platform-pinned")

# THE JAWARI BRIDGE (WeaveString::setBridge): the contact law, energy never gained, the side it sits on, the
# brightening, contact coming and going, and every model bounded under it.
add_executable(test_weave_jawari test_weave_jawari.cpp)
target_link_libraries(test_weave_jawari PRIVATE feathers-dsp feathers-warnings)
target_include_directories(test_weave_jawari PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest)
add_test(NAME weave-jawari COMMAND test_weave_jawari)

# THE WEAVE MODEL BENCHMARK — NOT A TEST (no add_test), the posture of bench_render: ns per physics
# tick for each model against the Ring. Its header says how to read it.
add_executable(bench_weave_models bench_weave_models.cpp)
target_link_libraries(bench_weave_models PRIVATE feathers-dsp feathers-warnings)

add_executable(test_aliasing test_aliasing.cpp)
target_link_libraries(test_aliasing PRIVATE feathers-dsp feathers-warnings)
target_include_directories(test_aliasing PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest)
add_test(NAME aliasing COMMAND test_aliasing)

add_executable(test_halfband test_halfband.cpp)
target_link_libraries(test_halfband PRIVATE feathers-dsp feathers-warnings)
target_include_directories(test_halfband PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest)
add_test(NAME halfband COMMAND test_halfband)

# The lookahead limiter (backlog LD/LE): header-only, in src/dsp; feathers-dsp for the include path.
add_executable(test_lookahead_limiter test_lookahead_limiter.cpp)
target_link_libraries(test_lookahead_limiter PRIVATE feathers-dsp feathers-warnings)
target_include_directories(test_lookahead_limiter PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest)
add_test(NAME lookahead-limiter COMMAND test_lookahead_limiter)

add_executable(test_modifiers test_modifiers.cpp)
target_link_libraries(test_modifiers PRIVATE feathers-dsp feathers-warnings)
target_include_directories(test_modifiers PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest)
add_test(NAME modifiers COMMAND test_modifiers)

add_executable(test_osc_voice test_osc_voice.cpp)
target_link_libraries(test_osc_voice PRIVATE feathers-dsp feathers-warnings)
target_include_directories(test_osc_voice PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest)
add_test(NAME osc-voice COMMAND test_osc_voice)

# Neither the firmware nor a host: the basis is plain arithmetic in a header, and the claim it has to
# answer for — that the default orientation decodes to the sum the engine always took — is provable
# without building a single wavetable.
add_executable(test_slice_basis test_slice_basis.cpp)
target_link_libraries(test_slice_basis PRIVATE feathers-warnings)
target_include_directories(test_slice_basis PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest ${CMAKE_SOURCE_DIR}/src)
target_compile_options(test_slice_basis PRIVATE ${FEATHERS_FP_EXACT})
add_test(NAME slice-basis COMMAND test_slice_basis)

# The other half of the migration guard: the basis test proves the DECODE reduces to what the engine
# always computed, this one proves the whole character does — every waypoint and every segment, for
# both engines. It needs the firmware's own builders to compare against, so it takes the character
# library the basis test does without.
add_executable(test_slice_build test_slice_build.cpp)
target_link_libraries(test_slice_build PRIVATE feathers-dsp feathers-warnings)
target_include_directories(test_slice_build PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest)
add_test(NAME slice-build COMMAND test_slice_build)

# The operator tier: the three φ fields, the three cycle operators, the identity corner and the
# bake. Every case carries its known-bad — the plan's rule that a control must be seen to fail
# before its pass is believed.
add_executable(test_vox_bake test_vox_bake.cpp)
target_link_libraries(test_vox_bake PRIVATE feathers-dsp feathers-warnings)
target_include_directories(test_vox_bake PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest)
add_test(NAME vox-bake COMMAND test_vox_bake)

# Vox through the voice: the note-dependent half of the engine, and the pin that says terrain with
# no vowel in it renders exactly the bits it rendered before vox could be heard at all.
add_executable(test_vox_voice test_vox_voice.cpp)
target_link_libraries(test_vox_voice PRIVATE feathers-dsp feathers-warnings)
target_include_directories(test_vox_voice PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest)
add_test(NAME vox-voice COMMAND test_vox_voice)
add_executable(test_flagship test_flagship.cpp)
target_link_libraries(test_flagship PRIVATE feathers-dsp feathers-warnings)
target_include_directories(test_flagship PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest)
add_test(NAME flagship COMMAND test_flagship)

# WARP — Serum's phase-domain family, ported 2026-08-28. Links feathers-dsp and nothing else: warp is
# engine maths, and a case for it that needed the plugin would be a case about the wrong layer.
add_executable(test_warp test_warp.cpp)
target_link_libraries(test_warp PRIVATE feathers-dsp feathers-warnings)
target_include_directories(test_warp PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest)
add_test(NAME warp COMMAND test_warp)
# The carrier table's THD/wrap/interp claims are float measurements, not bit-exact bakes, so they run on
# every platform — no platform-pinned label.
add_executable(test_fm_carrier test_fm_carrier.cpp)
target_link_libraries(test_fm_carrier PRIVATE feathers-dsp feathers-warnings)
target_include_directories(test_fm_carrier PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest)
add_test(NAME fm-carrier COMMAND test_fm_carrier)
# These two suites assert BAKE IDENTITY TO THE BIT (memcmp on FlagshipTable). That identity is a
# Linux/x86-64 property: macOS arm64 (Apple clang + Apple libm/NEON) produces different bits for the
# same bake even with -ffp-contract=off, so the assertions can't hold there — not a bug, a platform
# float difference. Same mechanism as golden-character (above): the "platform-pinned" label runs them
# on the Linux leg and excludes them on macOS via `ctest --label-exclude platform-pinned`. The macOS
# UI-integrity gate is macos-probe, not these bit-exact render checks.
set_tests_properties(vox-voice flagship PROPERTIES LABELS "platform-pinned")

# The blend's geometry with no sound in the room: weights and positions are arithmetic, and the
# arithmetic is the part that goes wrong in ways nobody hears until it is a patch that used to work.
add_executable(test_constellation test_constellation.cpp)
target_link_libraries(test_constellation PRIVATE feathers-dsp feathers-warnings)
target_include_directories(test_constellation PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest)
add_test(NAME constellation COMMAND test_constellation)

# The seam between the parameter table and the blend: which sounds are live, whose weight is whose,
# and which of the two blend rules a patch gets. Neither the geometry suite nor the renderer suite can
# see it — they are each handed numbers somebody else decided — and every bug that lives here still
# makes a perfectly plausible sound.
add_executable(test_character_blend test_character_blend.cpp)
target_link_libraries(test_character_blend PRIVATE feathers-dsp feathers-params feathers-warnings)
target_include_directories(test_character_blend PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest)
add_test(NAME character-blend COMMAND test_character_blend)

add_executable(test_params test_params.cpp)
target_link_libraries(test_params PRIVATE feathers-params feathers-warnings)
target_include_directories(test_params PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest)
add_test(NAME params COMMAND test_params)

# THE EVENT LOG (src/plugin/event_log.h): where a line goes, its shape, the cap, silence on failure.
add_executable(test_event_log test_event_log.cpp)
target_link_libraries(test_event_log PRIVATE feathers-event-log feathers-warnings)
target_include_directories(test_event_log PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest)
add_test(NAME event-log COMMAND test_event_log)

# AN AUDIO ENGINE RESTART MUST NOT COST THE PATCH (seen on macOS): the restart sequences through the
# real CLAP entries, by value, and a stress loop with process() on its own thread. The editor case needs a
# display, so it runs under the private one.
add_executable(test_engine_restart test_engine_restart.cpp ${CMAKE_SOURCE_DIR}/src/plugin/feathers_entry.cpp)
target_link_libraries(test_engine_restart PRIVATE feathers-impl feathers-warnings)
target_include_directories(test_engine_restart PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest)
add_test(NAME engine-restart COMMAND ${CMAKE_SOURCE_DIR}/scripts/venue-xvfb.sh run -- $<TARGET_FILE:test_engine_restart>)
set_tests_properties(engine-restart PROPERTIES LABELS "headless-incompatible")
# THE SOAK (2026-11-08): the whole plugin driven for minutes of simulated time — the Pattern in TB-Glide, legato slides,
# the Engine switched across every engine — with everything that could grow read by value. Its short cases run here;
# the long `[soak]` case is skipped unless asked for (`test_soak --no-skip -tc="soak*"`).
add_executable(test_soak test_soak.cpp ${CMAKE_SOURCE_DIR}/src/plugin/feathers_entry.cpp)
target_link_libraries(test_soak PRIVATE feathers-impl feathers-warnings)
target_include_directories(test_soak PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest)
add_test(NAME soak COMMAND $<TARGET_FILE:test_soak>)
add_executable(test_rack_restart test_engine_restart.cpp ${CMAKE_SOURCE_DIR}/src/rack/rack_entry.cpp)
target_compile_definitions(test_rack_restart PRIVATE FEATHERS_RESTART_RACK=1)
target_link_libraries(test_rack_restart PRIVATE feathers-rack-ui feathers-warnings)
target_include_directories(test_rack_restart PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest ${CMAKE_SOURCE_DIR}/src)
add_test(NAME rack-restart COMMAND ${CMAKE_SOURCE_DIR}/scripts/venue-xvfb.sh run -- $<TARGET_FILE:test_rack_restart>)
set_tests_properties(rack-restart PROPERTIES LABELS "headless-incompatible")

# The voice layer with no host at all: notes go in as calls, audio comes out as buffers. Same reason
# the param test needs no plugin object — the logic worth testing is not in the binding.
add_executable(test_synth test_synth.cpp)
target_link_libraries(test_synth PRIVATE feathers-synth feathers-warnings)
target_include_directories(test_synth PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest)
add_test(NAME synth COMMAND test_synth)

# VOICE STACKING (backlog KE): the fan, the 1/√N gain, a live Count change walked rather than jumped, the CPU
# guard, and no allocation for any of it. Its own binary because it replaces operator new to count allocations,
# which test_synth does not.
add_executable(test_voice_stack test_voice_stack.cpp)
target_link_libraries(test_voice_stack PRIVATE feathers-synth feathers-warnings)
target_include_directories(test_voice_stack PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest)
add_test(NAME voice-stack COMMAND test_voice_stack)

# THE RESONATE ENGINE through the synth (OscType::Resonate): the one string, its laws from the
# character, block-size invariance, and its loudness beside Stair's.
add_executable(test_resonate_engine test_resonate_engine.cpp)
target_link_libraries(test_resonate_engine PRIVATE feathers-synth feathers-warnings)
target_include_directories(test_resonate_engine PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest)
add_test(NAME resonate-engine COMMAND test_resonate_engine)

# THE BASIC SHAPES ENGINE: the band-limited ladders against their Fourier
# series, the mix, the mip rule, the voice's spectrum, V5's FM on it, and the honesty sweep on the parameter road.
add_executable(test_basic_shapes test_basic_shapes.cpp)
target_link_libraries(test_basic_shapes PRIVATE feathers-synth feathers-warnings)
target_include_directories(test_basic_shapes PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest)
add_test(NAME basic-shapes COMMAND test_basic_shapes)

# THE ADDITIVE ENGINE'S BANK BUILD: the blended spectrum, the
# Shape blend, the renormalisations, the per-sound Smoothness knob, and every other engine's Character unchanged.
add_executable(test_additive_engine test_additive_engine.cpp)
target_link_libraries(test_additive_engine PRIVATE feathers-synth feathers-gui-bridge feathers-warnings)
target_include_directories(test_additive_engine PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest)
add_test(NAME additive-engine COMMAND test_additive_engine)

# THE SPECTRAL DRAWER'S OPERATORS: each operator against its
# own restatement, and the exact identity at rest.
add_executable(test_spectral_shaper test_spectral_shaper.cpp)
target_link_libraries(test_spectral_shaper PRIVATE feathers-dsp feathers-warnings)
target_include_directories(test_spectral_shaper PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest)
add_test(NAME spectral-shaper COMMAND test_spectral_shaper)

# THE TERRAIN'S SPECTRUM FOR ONE SOUND: the envelope, the
# fine structure and the off-grid ratios, each against its own restatement.
add_executable(test_partial_spectrum test_partial_spectrum.cpp)
target_link_libraries(test_partial_spectrum PRIVATE feathers-dsp feathers-warnings)
target_include_directories(test_partial_spectrum PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest)
add_test(NAME partial-spectrum COMMAND test_partial_spectrum)

# THE ADDITIVE ROTATOR BANK: rotation, ramps,
# the Nyquist fade, renormalisation and exact silence, each read from the bank or a single sinusoid.
add_executable(test_partial_bank test_partial_bank.cpp)
target_link_libraries(test_partial_bank PRIVATE feathers-dsp feathers-warnings)
target_include_directories(test_partial_bank PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest)
add_test(NAME partial-bank COMMAND test_partial_bank)

# THE BASIC SHAPES BENCHMARK — NOT A TEST (no add_test): ns per voice sample, V5 against Basic Shapes, alternated.
add_executable(bench_basic_shapes bench_basic_shapes.cpp)
target_link_libraries(bench_basic_shapes PRIVATE feathers-dsp feathers-warnings)

# THE ADDITIVE BUILD BENCHMARK — NOT A TEST (no add_test): µs per 8-sound stereo bank build, Partials against V5.
add_executable(bench_partials_build bench_partials_build.cpp)
target_link_libraries(bench_partials_build PRIVATE feathers-dsp feathers-warnings)

# THE ROTATOR BANK BENCHMARK — NOT A TEST (no add_test): ns per partial per sample, setTargets + render beside
# render alone, 320 / 128 / 32 partials, mono and stereo, alternated.
add_executable(bench_partial_bank bench_partial_bank.cpp)
target_link_libraries(bench_partial_bank PRIVATE feathers-dsp feathers-warnings)

# THE DRAWER'S OPERATOR BENCHMARK — NOT A TEST (no add_test): ns per partial per control block for prepare() + gains(),
# each operator alone and bench_render's `--drawer 1` set, f0 moving every block, alternated (backlog LY).
add_executable(bench_spectral_shaper bench_spectral_shaper.cpp)
target_link_libraries(bench_spectral_shaper PRIVATE feathers-dsp feathers-warnings)

# THE STACK LIKENESS READ BENCHMARK — NOT A TEST (no add_test): µs per OscVoice::playingCycle(256), a V5 table part against
# a Partials part, at C2 and C4, alternated (Task 9b quality round).
add_executable(bench_coherence_read bench_coherence_read.cpp)
target_link_libraries(bench_coherence_read PRIVATE feathers-dsp feathers-warnings)

# EVERY AUDIO DEVICE IN THE REGISTRY, ASKED THE SAME QUESTIONS, each built alone the way a host builds
# it. This was the fx runner's suite; the runner is gone (the lane runs on the rack engine) and its
# lane claims moved to test_lane_rack.cpp, leaving the device sweeps here.
add_executable(test_fx_devices test_fx_devices.cpp)
target_link_libraries(test_fx_devices PRIVATE feathers-synth feathers-warnings)
target_include_directories(test_fx_devices PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest)
# FOUR SHARDS OVER A CASE THAT WAS SPLIT FOR THEM (2026-10-18). The control-honesty sweep was one case and 485 s of
# this suite's 505 (load ~28), so no index split could help; it is now four cases — Drawn, Resonate and SKLITTER alone,
# every other device in a catch-all — named so they sort into different shards (see the note above them). Measured
# at the same load the worst shard should be ~190 s. A case added here shifts the boundaries; the worst it can do is
# put two slow parts in one shard again, which a slower fx-devices-Nof4 in the ctest timings will show.
feathers_add_sharded_test(NAME fx-devices TARGET test_fx_devices
	SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/test_fx_devices.cpp SHARDS 4)


# Terrain stereo end to end: Width on a V5 patch is two full character evaluations, one per ear,
# and everything else — width 0, the frozen engines, every patch that predates the param — is the
# untouched mono render to the bit. The identity claims live here; the ear-indexed bank and the
# voice's pair loop have their units in test_osc_voice.
add_executable(test_terrain_stereo test_terrain_stereo.cpp)
target_link_libraries(test_terrain_stereo PRIVATE feathers-synth feathers-warnings)
target_include_directories(test_terrain_stereo PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest)
add_test(NAME terrain-stereo COMMAND test_terrain_stereo)

# The editor's road to the audio thread with neither end attached: no window, no host. The queue and
# the change detection are the parts worth holding to account, and neither needs a GPU.
add_executable(test_gui_bridge test_gui_bridge.cpp)
# feathers-dsp because the round-trip case asks the question that matters: not "did the offsets come
# back" but "does the patch rebuild the CHARACTER it saved", which needs the builders.
target_link_libraries(test_gui_bridge PRIVATE feathers-gui-bridge feathers-dsp feathers-warnings)
target_include_directories(test_gui_bridge PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest)
add_test(NAME gui-bridge COMMAND test_gui_bridge)

# The editor's controls, driven by injecting events into them rather than by pointing a mouse at a
# window — see the file's own header for the three separate ways synthetic clicks failed here. It
# needs the widgets, so it exists only where the GUI is built, and it still needs no display: visage's
# event entry points are ordinary function calls.
#
# NOT ON WINDOWS, where visage is built but the editor has no embed yet (PluginWindow::isAvailable answers false):
# these suites drive an editor no Windows host can open, every entry is labelled headless-incompatible and so
# excluded from CI on every leg anyway, and their fixtures speak X11 and POSIX (setenv, getpid, cxxabi). They
# join the Windows build with the Windows embed.
if(TARGET visage AND NOT WIN32)
	# THE ASSEMBLED EDITOR, not just its widgets. The controls themselves come from feathers-ui, which
	# is the same archive the plugin links; gui.cpp is the tree they are placed in, and the routing test
	# needs the placement to be the REAL one. A report that params could no longer be edited was
	# unanswerable without it: a knob can pass every test it has and still be unreachable where the layout puts it.
	#
	# gui.cpp is compiled HERE rather than taken from feathers-ui because it is the one editor file
	# whose meaning depends on the definitions its target sets — see the note on feathers-ui in the
	# top-level CMakeLists.
	#
	# SIX BINARIES OVER ONE FIXTURE LIBRARY (2026-09-03). `tests/test_gui_widgets.cpp` was one
	# translation unit of 36,148 lines and 554 cases, and every UI edit that reached it paid 140–220 s
	# to recompile all of it (measured: 145 s for that TU on this host the morning it was split). The
	# cases now live under `tests/gui_widgets/`, cut by what people EDIT TOGETHER — a DAHDSR change
	# touches env_card and its cases and nothing else — so an append recompiles one section.
	#
	# THE FIXTURE LIBRARY compiles gui.cpp and plugin_window.cpp ONCE for all seven consumers (the six
	# below and test_gui_windowed) — checked before doing so that neither file registers anything from
	# a static initialiser, which a static archive would silently drop — plus the free helper
	# functions of gui_widgets/fixtures.h. Everything a consumer needs (the seven libraries, X11, the
	# doctest and tokens include dirs, the two definitions) is PUBLIC here so the six executables
	# below say nothing twice.
	add_library(feathers-gui-widget-fixtures STATIC
		gui_widgets/fixtures.cpp
		# doctest's implementation ONCE, not once per binary. It was DOCTEST_CONFIG_IMPLEMENT in each
		# main.cpp: measured serially with ccache off, 10.6–11.8 s per binary under ASan against 0.92 s
		# without it, one TU here at 12.0 s. The file's own header has the link argument — main() stays
		# a source of each executable, only the implementation moves.
		gui_widgets/doctest_impl.cpp
		${CMAKE_SOURCE_DIR}/src/plugin/gui.cpp
		${CMAKE_SOURCE_DIR}/src/plugin/ui/plugin_window.cpp
	)
	# feathers-dsp for the wave display: it draws the engine's own table, so the test binary builds the
	# same character layer the plugin plays rather than a stand-in for it.
	# feathers-fx for the filter card: the curve it draws comes from the DEVICE's own response
	# evaluator, so the test binary draws from the same module the plugin plays rather than a stand-in.
	# feathers-synth for the stance cases that ask what a recalled patch hands the VOICE (the drawer's ShaperParams): the
	# claim is about the engine reading the params a stance writes, which no widget can answer.
	target_link_libraries(feathers-gui-widget-fixtures
		PUBLIC feathers-ui feathers-gui-bridge feathers-rack-editor feathers-dsp feathers-character feathers-fx visage
		       feathers-ui-fonts feathers-warnings feathers-synth)
	# tests/ itself is on the path for `spectrum.h`, which the one file found beside itself and the
	# header under gui_widgets/ cannot.
	target_include_directories(feathers-gui-widget-fixtures PUBLIC
		${CMAKE_SOURCE_DIR}/libs/doctest
		${FEATHERS_TOKENS_DIR}
		${CMAKE_CURRENT_SOURCE_DIR}
		${CMAKE_CURRENT_SOURCE_DIR}/gui_widgets
	)
	# The header draws the build stamp, so it needs the same version the plugin is built with. Same
	# value, from the same place — project() — rather than a second spelling of it.
	target_compile_definitions(feathers-gui-widget-fixtures PUBLIC FEATHERS_VERSION="${PROJECT_VERSION}"
		FEATHERS_GUI_VISAGE)
	# gui.cpp asks X directly for the display's DPI and work area, so the test binary needs X11 whether
	# or not the developer tools are switched on — that block finds it for itself and may not have run.
	if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
		find_package(X11 REQUIRED)
		target_link_libraries(feathers-gui-widget-fixtures PUBLIC ${X11_LIBRARIES})
		target_include_directories(feathers-gui-widget-fixtures PUBLIC ${X11_INCLUDE_DIR})
	endif()
	add_dependencies(feathers-gui-widget-fixtures feathers-tokens)

	# THE SIX. Shards scale with case count (the macro reads the count from each SOURCE, so a case
	# appended to a section is sharded without anyone editing a number here); every shard carries the
	# headless-incompatible label the one binary's twelve did, because CI selects by label. main() is a
	# SOURCE of each rather than a member of the archive so that no linker search order decides
	# whether a binary has one.
	#   controls — knobs, notify tray, derivation clamp, perform stage and sky, wave display, sound
	#              stack, order of the list, terrain
	#   explore  — the Explore stage
	#   lane     — the modulation panel, the fx lane's first tenant, the source grid
	#   dahdsr   — the DAHDSR card (the largest section: 38% of the old file)
	#   devices  — the table shaper's surfaces and plots, the automodulator's strip and panel, the
	#              structural sweeps
	#   mod_ring — the mod ring and what it draws, gathered from three sections into one file because
	#              it is the surface the next work extends; ten cases that READ modRing()'s struct
	set(_feathers_widget_shards_controls 2)
	set(_feathers_widget_shards_explore 3)
	set(_feathers_widget_shards_lane 2)
	set(_feathers_widget_shards_dahdsr 4)
	set(_feathers_widget_shards_devices 2)
	set(_feathers_widget_shards_mod_ring 1)
	# nest — backlog HG: nesting past one level (SKLITTER → chain → SKLITTER) and the panel-teardown
	#        orders it reaches. Its own binary because the cases are about the LEVELS rather than any
	#        one widget; everything at one level stays in devices.
	set(_feathers_widget_shards_nest 1)
	# presets — backlog FO: the preset store on disk, the shared ItemStepper, the browser, its name-and-
	#           tags prompt, rename/delete/show, tag browsing and suggestions; gathered from controls and
	#           lane when phase 2 made presets a subsystem of their own.
	set(_feathers_widget_shards_presets 1)
	# tooltips — hover tooltips on every device control: the layer, the delay, the wrap, and the sweep
	#            over every card and panel in every host.
	set(_feathers_widget_shards_tooltips 1)
	# undo — backlog JB: every road that changes the patch is one step in the editor's one history, undone and
	#        redone by value, and a parameter's undo reaches the host as a gesture.
	set(_feathers_widget_shards_undo 1)
	foreach(_suite controls explore lane dahdsr devices mod_ring nest presets tooltips undo)
		string(REPLACE "_" "-" _suite_name "${_suite}")
		add_executable(test_gui_widgets_${_suite}
			gui_widgets/test_gui_widgets_${_suite}.cpp
			gui_widgets/main.cpp
		)
		target_link_libraries(test_gui_widgets_${_suite} PRIVATE feathers-gui-widget-fixtures)
		feathers_add_sharded_test(NAME gui-widgets-${_suite_name} TARGET test_gui_widgets_${_suite}
			SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/gui_widgets/test_gui_widgets_${_suite}.cpp
			SHARDS ${_feathers_widget_shards_${_suite}}
			LABELS "headless-incompatible" VENUE)
	endforeach()
	# HOST SLOTS T7: the devices suite reads an exposed control's slot name through `SlotHost`, the object the
	# plugin answers `params.get_info` with — the CLAP glue, which the fixtures do not otherwise link.
	target_link_libraries(test_gui_widgets_devices PRIVATE feathers-hostslots-clap)
	# FACTORY PRESETS: the presets suite checks the plugin's embedded set against presets/factory, and runs
	# the embedding over a fixture folder of its own so the road is exercised while the real one is empty.
	feathers_embed_presets(test_gui_widgets_presets DIR ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/factory-presets
	                       FUNCTION testFactoryPresetFiles)
	target_compile_definitions(test_gui_widgets_presets PRIVATE
		FEATHERS_FACTORY_PRESET_DIR="${CMAKE_SOURCE_DIR}/presets/factory"
		FEATHERS_TEST_FACTORY_PRESET_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/factory-presets")
	# gui-widgets drives visage's windowing and holds pixel/red-channel assertions, so it needs a
	# display: no display -> SegFault.
	#
	# THE "SOFTWARE RENDERING SHOWS BLANK" HALF OF THIS NOTE WAS TRUE AND IS NOW FALSE, CORRECTED
	# 2026-08-30. It was verified against software GL (llvmpipe) and visage now hardcodes VULKAN on
	# Linux, so the venue that failed is not the venue we have. MEASURED under
	# `scripts/venue-xvfb.sh` (Xvfb + lavapipe + VISAGE_FULL_REDRAW=1): gui-widgets 492/492 and
	# gui-windowed 7/7, per-case. **These suites are no longer headless-INCOMPATIBLE; they are
	# headless-UNTESTED-IN-CI**, which is a different claim and the reason the label stays.
	#
	# What has NOT changed and is why the label is still correct for CI: the runners have neither Xvfb
	# nor lavapipe configured, enabling them costs scarce Actions minutes, and the
	# Apple Silicon flake below is a timing fault rather than a display one, so no venue fixes it.
	#
	# It also flakes on fast Apple Silicon, where a rebuild
	# completes within a frame and the timing assertion (REQUIRE(sawIncomplete)) goes false. It passes
	# on a host with a real display (DISPLAY=:1, verified). Excluded from CI ctest on every leg by the
	# headless-incompatible label; run it on a host, per the GUI-on-a-host stance in the UI plan.
	# The label is set on every shard by feathers_add_sharded_test above — CI selects this suite out by
	# label rather than by name (.github/workflows/build.yml), so the shards must each carry it.

	# THE POPOUT'S OWN BINARY, AND THE SEPARATE PROCESS IS A FINDING RATHER THAN A CONVENIENCE.
	#
	# These cases open a REAL OS WINDOW. Measured on the branch this came from: the case passes alone
	# and SEGFAULTS when it runs after several hundred other cases in the same process — so a second
	# visage window is not safe in a process that has already built and torn down many editors. A DAW
	# is exactly such a process. Recorded here rather than made to disappear: this SPLIT DOES NOT FIX
	# the underlying instability and nothing here should be read as saying it does.
	#
	# ITS OWN FILE RATHER THAN A FILTER OVER `test_gui_widgets`, because that file is already enormous
	# and because a whole second binary is what makes "runs in a process with no other editor churn in
	# it" a property of the target rather than of a command-line argument somebody must not drop.
	#
	# THE EDITOR COMES FROM THE FIXTURE LIBRARY RATHER THAN BEING SPELLED A SECOND TIME. This used to
	# copy `test_gui_widgets`'s SOURCES list minus its own case file, so that a source added there for
	# a new card could not leave this binary failing to link — the exact failure CLAUDE.md records for
	# three tool targets that did not link for a day. The library is that list now: one archive, seven
	# consumers. This binary keeps its own main() (DOCTEST_CONFIG_IMPLEMENT in test_gui_windowed.cpp)
	# and includes nothing from gui_widgets/fixtures.h.
	add_executable(test_gui_windowed test_gui_windowed.cpp)
	target_link_libraries(test_gui_windowed PRIVATE feathers-gui-widget-fixtures)
	target_link_libraries(test_gui_windowed
		PRIVATE feathers-ui feathers-rack-editor feathers-gui-bridge feathers-dsp feathers-character feathers-fx visage
		        feathers-ui-fonts feathers-warnings)
	target_include_directories(test_gui_windowed PRIVATE
		${CMAKE_SOURCE_DIR}/libs/doctest
		${FEATHERS_TOKENS_DIR}
	)
	target_compile_definitions(test_gui_windowed PRIVATE FEATHERS_VERSION="${PROJECT_VERSION}"
		FEATHERS_GUI_VISAGE)
	if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
		find_package(X11 REQUIRED)
		target_link_libraries(test_gui_windowed PRIVATE ${X11_LIBRARIES})
		target_include_directories(test_gui_windowed PRIVATE ${X11_INCLUDE_DIR})
	endif()
	add_dependencies(test_gui_windowed feathers-tokens)
	# ON THE PRIVATE DISPLAY, ONE PROCESS PER CASE (2026-09-03). This entry ran the
	# binary on whatever DISPLAY ctest inherited -- the desktop's `:1` -- so two sessions' full suites at
	# once put two capture runs on one desktop and each photographed the other's window, or nothing:
	# three `REQUIRE(before.valid())` reds in one hour, from two sessions independently, both read as
	# findings until re-run alone in 11 s. venue-xvfb.sh claims a display of its own (-displayfd, so
	# concurrent runs cannot collide), lavapipe, VISAGE_FULL_REDRAW=1 and a scratch config; and
	# run-cases.sh --each is the command that is this suite's truth and the install loop
	# already runs regardless of the tally. ctest's verdict is therefore run-cases' exit, which fails a
	# SHORT tally -- so a crash mid-suite reads as FAIL here rather than as a truncated green.
	#
	# MEASURED before the change: DISPLAY unset, `ctest -R gui-windowed` SegFault in 0.12 s. After:
	# green with DISPLAY unset, and green twice concurrently from two shells.
	add_test(NAME gui-windowed COMMAND ${CMAKE_SOURCE_DIR}/scripts/venue-xvfb.sh run --
		${CMAKE_SOURCE_DIR}/scripts/run-cases.sh --each $<TARGET_FILE:test_gui_windowed>)
	# The label STAYS: CI's runners have neither Xvfb nor lavapipe, and CI selects this suite out by
	# label (.github/workflows/build.yml). On a host without them the venue exits 1 with "NOT READY"
	# rather than a SegFault, which is the same red said more plainly.
	set_tests_properties(gui-windowed PROPERTIES LABELS "headless-incompatible")
endif()

# THE MODIFIER ROAD THROUGH A REAL X11 WINDOW — tools/alt-probe.cpp, backlog CU. The widget suites
# above hand the knob a MouseEvent with `kModifierAlt` written in by the case; this is the one venue
# where the bit comes from the X server, through visage's XQueryPointer, and the verdict is read off
# the bridge. Only built with FEATHERS_BUILD_TOOLS and libXtst, so registered only when it exists; the
# venue is the private Xvfb, because XTEST on `:1` moves the desktop's real pointer.
if(TARGET alt-probe)
	add_test(NAME alt-probe-x11 COMMAND ${CMAKE_SOURCE_DIR}/scripts/venue-xvfb.sh run --
		$<TARGET_FILE:alt-probe>)
	set_tests_properties(alt-probe-x11 PROPERTIES LABELS "headless-incompatible")
endif()
# WHICH KEYS REACH THE HOST WHILE A NAME IS TYPED — tools/key-probe.cpp: the macro rename and the preset
# prompt typed through XTEST, with a host window that takes the focus back as Bitwig's does, a key forwarded
# without Shift, Enter on the prompt's panel, and a click elsewhere letting go of the keyboard grab.
if(TARGET key-probe)
	add_test(NAME key-probe-x11 COMMAND ${CMAKE_SOURCE_DIR}/scripts/venue-xvfb.sh run --
		$<TARGET_FILE:key-probe>)
	set_tests_properties(key-probe-x11 PROPERTIES LABELS "headless-incompatible")
endif()

# Not a test of behaviour but of the artefact: what the shipped .clap will ask the loader for. A
# dependency a DAW sandbox lacks does not crash, it makes the plugin silently invisible — see
# cmake/CheckRuntimeDeps.cmake for the case that motivated it.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
	add_test(NAME runtime-deps COMMAND ${CMAKE_COMMAND}
		-DPLUGIN=$<TARGET_FILE:feathers_clap>
		-P ${CMAKE_SOURCE_DIR}/cmake/CheckRuntimeDeps.cmake
	)
endif()

# The plugin through its own C API. The entry point is compiled in rather than dlopen()ed — see the
# file's header for what that trades away and why — so this target builds the clap_entry structure
# the same way the shipped formats do: the plugin as a static library plus the one entry file.
add_executable(test_clap_render test_clap_render.cpp ${CMAKE_SOURCE_DIR}/src/plugin/feathers_entry.cpp)
target_link_libraries(test_clap_render PRIVATE feathers-impl feathers-warnings)
target_include_directories(test_clap_render PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest)
# X11 for the case that embeds the editor in a real parent window in Bitwig's order (backlog LX, 2026-10-10).
if(UNIX AND NOT APPLE)
	find_package(X11 REQUIRED)
	target_link_libraries(test_clap_render PRIVATE ${X11_LIBRARIES})
	target_include_directories(test_clap_render PRIVATE ${X11_INCLUDE_DIR})
endif()
add_test(NAME clap-render COMMAND test_clap_render)
# THE SAME BINARY WITH A DISPLAY. The DE case (the first Mac build, 2026-09-07: devices added after
# init never reached the synth) creates the real editor through the CLAP gui road, and on X11 that
# needs a display -- visage asks X for the monitor scale at create. Headless, the case skips itself
# with a MESSAGE so `clap-render` above stays green on a runner; here it EXECUTES, under the private
# Xvfb venue. Labelled like gui-windowed, and for the same reason: CI's runners have no Xvfb.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
	add_test(NAME clap-render-display COMMAND ${CMAKE_SOURCE_DIR}/scripts/venue-xvfb.sh run --
		$<TARGET_FILE:test_clap_render>)
	set_tests_properties(clap-render-display PROPERTIES LABELS "headless-incompatible")
endif()

# The fx lane's first tenant, with no lane and no plugin around it — which is the claim the module
# was built to make. It links feathers-fx and nothing else, so a link error here is the boundary
# breaking before the include check even runs.
add_executable(test_fx_filter test_fx_filter.cpp)
target_link_libraries(test_fx_filter PRIVATE feathers-fx feathers-warnings)
target_include_directories(test_fx_filter PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest)
# The include-graph case reads the module's own sources rather than trusting them.
target_compile_definitions(test_fx_filter PRIVATE FEATHERS_FX_SOURCE_DIR="${CMAKE_SOURCE_DIR}/src/fx"
	FEATHERS_SRC_DIR="${CMAKE_SOURCE_DIR}/src" FEATHERS_TOOLS_DIR="${CMAKE_SOURCE_DIR}/tools")
# Second-longest suite at 77.5 s, and the next floor once gui-widgets is split. Four shards: one
# case ("a comb arrives tuned to the note") is 20.8 s on its own, so that is the floor no split can
# get under and a wider split would only add entries that finish instantly.
feathers_add_sharded_test(NAME fx-filter TARGET test_fx_filter
	SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/test_fx_filter.cpp SHARDS 4)

# The primitives the licence migration wrote (2026-09-24): fast tan/tanh, the TPT one-pole and the
# windowed-sinc delay line, each held to the number its header states. Header-only, so nothing to link.
add_executable(test_dsp_primitives test_dsp_primitives.cpp)
target_link_libraries(test_dsp_primitives PRIVATE feathers-warnings)
target_include_directories(test_dsp_primitives PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest ${CMAKE_SOURCE_DIR}/src)
target_include_directories(test_dsp_primitives SYSTEM PRIVATE ${CMAKE_SOURCE_DIR}/libs/simde)
add_test(NAME dsp-primitives COMMAND test_dsp_primitives)

# HARM — the notch and the sub-oscillator. Same shape as the filter's suite above; it links only
# feathers-fx, which is the boundary the fx lane's devices are held to.
add_executable(test_fx_harm test_fx_harm.cpp)
target_link_libraries(test_fx_harm PRIVATE feathers-fx feathers-warnings)
target_include_directories(test_fx_harm PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest)
add_test(NAME fx-harm COMMAND test_fx_harm)

# HARM's four partials and its envelope: bit-identity of the single-partial path against a copy of the device as it was.
add_executable(test_fx_harm_partials test_fx_harm_partials.cpp)
target_link_libraries(test_fx_harm_partials PRIVATE feathers-fx feathers-warnings)
target_include_directories(test_fx_harm_partials PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest)
add_test(NAME fx-harm-partials COMMAND test_fx_harm_partials)

# THE DELAY. Links `feathers-fx` ALONE, which is the fx-boundary claim rather than a minimal link
# line: a `src/plugin` include in the device becomes a link error here.
add_executable(test_fx_delay test_fx_delay.cpp)
target_link_libraries(test_fx_delay PRIVATE feathers-fx feathers-warnings)
target_include_directories(test_fx_delay PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest)
add_test(NAME fx-delay COMMAND test_fx_delay)
# THE MULTI-TAP UNDER MOVING CONTROLS (backlog MD) — NOT A TEST (no add_test), bench_phaser's posture: the worst
# step and clicks per second with Shape and Taps moved, and ns per frame static and modulated. Its header says how.
add_executable(bench_delay_multitap bench_delay_multitap.cpp)
target_link_libraries(bench_delay_multitap PRIVATE feathers-fx feathers-warnings)
# THE DELAY'S COST PER MODE (delay-love S0) — NOT A TEST: the baseline the feedback lane is held against.
add_executable(bench_delay_modes bench_delay_modes.cpp)
target_link_libraries(bench_delay_modes PRIVATE feathers-fx feathers-warnings)

# THE PITCH SHIFTER. `feathers-fx` alone, for the delay's reason.
add_executable(test_fx_pitch test_fx_pitch.cpp)
target_link_libraries(test_fx_pitch PRIVATE feathers-fx feathers-warnings)
target_include_directories(test_fx_pitch PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest)
add_test(NAME fx-pitch COMMAND test_fx_pitch)

# THE ERODER. `feathers-fx` alone — which here also proves the phi layer it reads (`src/character`)
# is reachable from the portable library and not only from the plugin.
add_executable(test_fx_eroder test_fx_eroder.cpp)
target_link_libraries(test_fx_eroder PRIVATE feathers-fx feathers-warnings)
target_include_directories(test_fx_eroder PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest)
add_test(NAME fx-eroder COMMAND test_fx_eroder)

# THE DISPERSER. The suite carries the firmware's own loop transcribed in q31 — the instrument the
# Eroder's first release lacked — and holds the port to it sample for sample.
add_executable(test_fx_disperser test_fx_disperser.cpp)
target_link_libraries(test_fx_disperser PRIVATE feathers-fx feathers-warnings)
target_include_directories(test_fx_disperser PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest)
add_test(NAME fx-disperser COMMAND test_fx_disperser)

# THE PHASER/FLANGER. Value assertions through the engine's
# test seams; the shared sweeps in test_fx_devices cover what every device owes.
add_executable(test_fx_phaser test_fx_phaser.cpp)
target_link_libraries(test_fx_phaser PRIVATE feathers-fx feathers-warnings)
target_include_directories(test_fx_phaser PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest)
add_test(NAME fx-phaser COMMAND test_fx_phaser)
# THE PHASER/FLANGER BENCHMARK — NOT A TEST (no add_test), bench_render's posture: ns per sample, Normal against
# the barberpole's Up, both engines (the plan's Task 5). Its header says how to read it.
add_executable(bench_phaser bench_phaser.cpp)
target_link_libraries(bench_phaser PRIVATE feathers-fx feathers-warnings)

add_executable(test_fx_featherverb test_fx_featherverb.cpp)
target_link_libraries(test_fx_featherverb PRIVATE feathers-fx feathers-warnings)
target_include_directories(test_fx_featherverb PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest)
target_compile_options(test_fx_featherverb PRIVATE ${FEATHERS_FP_EXACT})
add_test(NAME fx-featherverb COMMAND test_fx_featherverb)

add_executable(test_fx_smooth test_fx_smooth.cpp)
target_link_libraries(test_fx_smooth PRIVATE feathers-fx feathers-warnings)
target_include_directories(test_fx_smooth PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest)
target_compile_options(test_fx_smooth PRIVATE ${FEATHERS_FP_EXACT})
add_test(NAME fx-smooth COMMAND test_fx_smooth)

# The Smooth Shaper's primitives pinned by value: the sine table, the q31 pieces, the zone core.
add_executable(test_smooth_values test_smooth_values.cpp)
target_link_libraries(test_smooth_values PRIVATE feathers-fx feathers-warnings)
target_include_directories(test_smooth_values PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest)
target_compile_options(test_smooth_values PRIVATE ${FEATHERS_FP_EXACT})
add_test(NAME smooth-values COMMAND test_smooth_values)

# CONVOLUTION (backlog JD): the generated test IR, then the engine. `feathers-fx` alone.
add_executable(test_conv test_conv.cpp)
target_link_libraries(test_conv PRIVATE feathers-fx feathers-warnings)
target_include_directories(test_conv PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest)
add_test(NAME conv COMMAND test_conv)

# EQ — the design library: matched bells and shelves against an
# independently restated analog prototype, Butterworth cuts, the tilt, the SVF / one-pole realisations,
# the Offset/Amount/edge rules and the musical readout. Header-only, so it needs only the include path
# `feathers-fx` carries.
add_executable(test_eq_design test_eq_design.cpp)
target_link_libraries(test_eq_design PRIVATE feathers-fx feathers-warnings)
target_include_directories(test_eq_design PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest)
add_test(NAME eq-design COMMAND test_eq_design)

# EQ — the device, minimum phase (step 2): codec, identity, the analog prototype at probe frequencies,
# block-size invariance, the smoother landing, Amount's inverse through two devices, the Nyquist fade,
# off, and the rest reaching exact silence.
add_executable(test_fx_eq test_fx_eq.cpp)
target_link_libraries(test_fx_eq PRIVATE feathers-fx feathers-warnings)
target_include_directories(test_fx_eq PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest)
add_test(NAME fx-eq COMMAND test_fx_eq)

# SKLITTER — the reference's Scatter, ported. The hash first, because it is the foundation every one of
# the eight modes stands on: which slice plays, reversed or not, how long, is bits out of these two
# functions. Ported verbatim, so this suite pins it against independently computed values.
#
# **THREE OF THESE EIGHT SUITES LINK `feathers-fx-sklitter` AND FIVE STILL LINK `feathers-fx` ALONE,
# and the split is not arbitrary — it is exactly which ones touch the `Sklitter` OBJECT.** SKLITTER
# became a device that holds a chain, so its two translation units moved above the rack (see the
# target's own note in the top-level CMakeLists); the five suites that test its header-only pieces —
# the hash, the decision table, the ring, the engage machine, the mode dispatch — are untouched by
# that and still make the fx-boundary claim, checked by a link error exactly as before.
#
# So the boundary did not weaken by eight suites' worth. It weakened by three, for one device, which
# is the honest accounting: a device that holds a `RackEngine` is not a plain tenant and a suite that
# said otherwise would be describing the build it wishes it had.
add_executable(test_fx_sklitter_hash test_fx_sklitter_hash.cpp)
target_link_libraries(test_fx_sklitter_hash PRIVATE feathers-fx feathers-warnings)
target_include_directories(test_fx_sklitter_hash PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest)
add_test(NAME fx-sklitter-hash COMMAND test_fx_sklitter_hash)

# FACTORY PRESETS: scripts/collect-factory-presets.sh against a fixture root the test builds in a temp dir
# (selection by tag, byte copies, the case-clash refusals, --prune, the root only read). POSIX sh.
if(NOT WIN32)
	add_test(NAME collect-factory-presets
	         COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/test_collect_factory_presets.sh
	                 ${CMAKE_SOURCE_DIR}/scripts/collect-factory-presets.sh)
	# 77: the fixture's case pairs cannot exist on a case-insensitive filesystem (macOS) — see the script.
	set_tests_properties(collect-factory-presets PROPERTIES SKIP_RETURN_CODE 77)
endif()

add_executable(test_fx_sklitter_decisions test_fx_sklitter_decisions.cpp)
target_link_libraries(test_fx_sklitter_decisions PRIVATE feathers-fx feathers-warnings)
target_include_directories(test_fx_sklitter_decisions PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest)
add_test(NAME fx-sklitter-decisions COMMAND test_fx_sklitter_decisions)

add_executable(test_fx_sklitter_types test_fx_sklitter_types.cpp)
target_link_libraries(test_fx_sklitter_types PRIVATE feathers-fx-sklitter feathers-warnings)
target_include_directories(test_fx_sklitter_types PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest)
add_test(NAME fx-sklitter-types COMMAND test_fx_sklitter_types)

add_executable(test_fx_sklitter_perform test_fx_sklitter_perform.cpp)
target_link_libraries(test_fx_sklitter_perform PRIVATE feathers-fx-sklitter feathers-warnings)
target_include_directories(test_fx_sklitter_perform PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest)
add_test(NAME fx-sklitter-perform COMMAND test_fx_sklitter_perform)

add_executable(test_fx_sklitter_ring test_fx_sklitter_ring.cpp)
target_link_libraries(test_fx_sklitter_ring PRIVATE feathers-fx feathers-warnings)
target_include_directories(test_fx_sklitter_ring PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest)
add_test(NAME fx-sklitter-ring COMMAND test_fx_sklitter_ring)

add_executable(test_fx_sklitter_engage test_fx_sklitter_engage.cpp)
target_link_libraries(test_fx_sklitter_engage PRIVATE feathers-fx feathers-warnings)
target_include_directories(test_fx_sklitter_engage PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest)
add_test(NAME fx-sklitter-engage COMMAND test_fx_sklitter_engage)

add_executable(test_fx_sklitter test_fx_sklitter.cpp)
target_link_libraries(test_fx_sklitter PRIVATE feathers-fx-sklitter feathers-warnings)
target_include_directories(test_fx_sklitter PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest)
# 137 s on a quiet round (2026-10-18). Two shards: "ribbon truth: every slot the device plays..." is ~40% of the
# suite on its own, so that is the floor and a wider split would only add entries that finish early.
feathers_add_sharded_test(NAME fx-sklitter TARGET test_fx_sklitter
	SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/test_fx_sklitter.cpp SHARDS 2)

# **BACKLOG HN — THE DEFERRED-WRITE FEASIBILITY PROOF.** A separate binary on purpose: it is an
# INSTRUMENT rather than a guard — it measures whether a proposed change would be exact, and its
# assertions are about that measurement — and a known-bad protocol wants to run its cases one process
# per case without dragging the 38-second `fx-sklitter` suite along. MEASURED: 1.7 s for all seven.
add_executable(test_fx_sklitter_defer test_fx_sklitter_defer.cpp)
target_link_libraries(test_fx_sklitter_defer PRIVATE feathers-fx-sklitter feathers-warnings)
target_include_directories(test_fx_sklitter_defer PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest)
add_test(NAME fx-sklitter-defer COMMAND test_fx_sklitter_defer)

add_executable(test_fx_sklitter_liveness test_fx_sklitter_liveness.cpp)
target_link_libraries(test_fx_sklitter_liveness PRIVATE feathers-fx-sklitter feathers-warnings)
target_include_directories(test_fx_sklitter_liveness PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest)
add_test(NAME fx-sklitter-liveness COMMAND test_fx_sklitter_liveness)

# **BACKLOG GU/GV — THE THREE SEQUENTIAL BLENDS.** Its own binary for the reason the defer suite has
# one: every claim in it is a VALUE against a buffer the case put there, and a known-bad protocol
# wants to run those one process per case without dragging the 38-second `fx-sklitter` suite along.
add_executable(test_fx_sklitter_feedback test_fx_sklitter_feedback.cpp)
target_link_libraries(test_fx_sklitter_feedback PRIVATE feathers-fx-sklitter feathers-warnings)
target_include_directories(test_fx_sklitter_feedback PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest ${CMAKE_SOURCE_DIR}/src)
add_test(NAME fx-sklitter-feedback COMMAND test_fx_sklitter_feedback)

# backlog MJ — the Record Gate and the record light. Its own suite rather than the tail of the feedback one, so
# the branches that also append to that file do not meet this one there.
add_executable(test_fx_sklitter_gate test_fx_sklitter_gate.cpp)
target_link_libraries(test_fx_sklitter_gate PRIVATE feathers-fx-sklitter feathers-warnings)
target_include_directories(test_fx_sklitter_gate PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest ${CMAKE_SOURCE_DIR}/src)
add_test(NAME fx-sklitter-gate COMMAND test_fx_sklitter_gate)

# **BACKLOG HU — THE FEEDBACK CIRCUIT IN GRAIN, AND THE GUARD IT NEEDED.** Its own binary for the
# reason the other two sklitter-write suites have one: it MEASURES a loop over several bar-long
# passes, and a known-bad protocol wants those one process per case.
add_executable(test_fx_sklitter_grainfb test_fx_sklitter_grainfb.cpp)
target_link_libraries(test_fx_sklitter_grainfb PRIVATE feathers-fx-sklitter feathers-warnings)
target_include_directories(test_fx_sklitter_grainfb PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest ${CMAKE_SOURCE_DIR}/src)
add_test(NAME fx-sklitter-grainfb COMMAND test_fx_sklitter_grainfb)

# **BACKLOG HU — THE GRAIN BLEND ORDER AS AN AUDITION.** Its own binary like the two write suites
# beside it. **IT IS EXPECTED TO BE DELETED WITH `Param::GrainOrder`** once a choice is made between the two
# orders by ear; it is scaffold that measures a scaffold, not a permanent guard.
add_executable(test_fx_sklitter_grainorder test_fx_sklitter_grainorder.cpp)
target_link_libraries(test_fx_sklitter_grainorder PRIVATE feathers-fx-sklitter feathers-warnings)
target_include_directories(test_fx_sklitter_grainorder PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest ${CMAKE_SOURCE_DIR}/src)
add_test(NAME fx-sklitter-grainorder COMMAND test_fx_sklitter_grainorder)

# **SKLITTER AS A HOLDER** — backlog EW's post lane. The chain's own machinery is pinned in
# `test_inner_chain.cpp`; this suite asks only what this device can answer: that the chain runs on every
# road `processStereo` can take (including the four that return early), that it is handed this device's
# output AFTER the blend and whole (against a REFERENCE device with no chain, sample for sample), and
# that the seven aux outputs reach the chain's core sources meaning what they mean outside it.
add_executable(test_sklitter_chain test_sklitter_chain.cpp)
target_link_libraries(test_sklitter_chain PRIVATE feathers-fx-sklitter feathers-warnings)
target_include_directories(test_sklitter_chain PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest ${CMAKE_SOURCE_DIR}/src)
add_test(NAME sklitter-chain COMMAND test_sklitter_chain)

add_executable(test_fx_sklitter_modes test_fx_sklitter_modes.cpp)
target_link_libraries(test_fx_sklitter_modes PRIVATE feathers-fx feathers-warnings)
target_include_directories(test_fx_sklitter_modes PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest)
add_test(NAME fx-sklitter-modes COMMAND test_fx_sklitter_modes)

# OWLTT — the multiband compressor. Crossover test first, as it is the smallest dependency and the
# foundation the compressor is built on.
add_executable(test_fx_owltt_crossover test_fx_owltt_crossover.cpp)
target_link_libraries(test_fx_owltt_crossover PRIVATE feathers-fx feathers-warnings)
target_include_directories(test_fx_owltt_crossover PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest)
add_test(NAME fx-owltt-crossover COMMAND test_fx_owltt_crossover)

# OWLTT zone parameter computation.
add_executable(test_fx_owltt_zone test_fx_owltt_zone.cpp)
target_link_libraries(test_fx_owltt_zone PRIVATE feathers-fx feathers-warnings)
target_include_directories(test_fx_owltt_zone PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest)
add_test(NAME fx-owltt-zone COMMAND test_fx_owltt_zone)

# OWLTT band compressor — the core DSP. Tests assert gain reduction in dB against an independent
# reference (the brief's required discipline, not a buffer comparison).
add_executable(test_fx_owltt_compressor test_fx_owltt_compressor.cpp)
target_link_libraries(test_fx_owltt_compressor PRIVATE feathers-fx feathers-warnings)
target_include_directories(test_fx_owltt_compressor PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest)
add_test(NAME fx-owltt-compressor COMMAND test_fx_owltt_compressor)

# OWLTT device integration test.
add_executable(test_fx_owltt test_fx_owltt.cpp)
target_link_libraries(test_fx_owltt PRIVATE feathers-fx feathers-warnings)
target_include_directories(test_fx_owltt PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest)
add_test(NAME fx-owltt COMMAND test_fx_owltt)

# THE TABLE SHAPER. It links feathers-fx and nothing else, which is the point: a link error here is
# the device having reached into the plugin, and the boundary is checked by reading the include graph
# in test_fx_filter.cpp's own case (which walks all of src/fx, so this device is covered by it).
add_executable(test_fx_shaper test_fx_shaper.cpp)
target_link_libraries(test_fx_shaper PRIVATE feathers-fx feathers-warnings)
target_include_directories(test_fx_shaper PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest)
add_test(NAME fx-shaper COMMAND test_fx_shaper)

# The waypoint filter, headless. It links feathers-fx and nothing else, so a link error here is the
# device boundary breaking before any include check runs -- the same terms as OWLTT above.
add_executable(test_fx_waypoint test_fx_waypoint.cpp)
target_link_libraries(test_fx_waypoint PRIVATE feathers-fx feathers-warnings)
target_include_directories(test_fx_waypoint PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest)
add_test(NAME fx-waypoint COMMAND test_fx_waypoint)

# BUS UTILITY — gain, pan, dual pan, width and polarity as one 2x2 matrix, headless. Links
# feathers-fx and nothing else, on the same terms as the waypoint suite above: a link error here is
# the device boundary breaking before any include check runs.
add_executable(test_fx_busutil test_fx_busutil.cpp)
target_link_libraries(test_fx_busutil PRIVATE feathers-fx feathers-warnings)
target_include_directories(test_fx_busutil PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest)
add_test(NAME fx-busutil COMMAND test_fx_busutil)

# The dynamics devices' shared helpers (`src/fx/dyn/`): the one soft-clip curve, the held peak and the
# linear ramp, the stereo 4x oversampler. Header-only, so it links feathers-fx for Bus Utility, whose
# clip is asserted to BE the shared curve, and feathers-dsp for `Halfband4x::decimate`, the voice's
# decimator, which the oversampler's down path is pinned bit-identical to.
add_executable(test_fx_dyn test_fx_dyn.cpp)
target_link_libraries(test_fx_dyn PRIVATE feathers-fx feathers-dsp feathers-warnings)
target_include_directories(test_fx_dyn PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest)
add_test(NAME fx-dyn COMMAND test_fx_dyn)

# COMP — the compressor/limiter: its static curve asserted against hand-computed values, its time
# constants in samples, the Limit ceiling across bursts, and Mix 0 as the dry signal to the bit.
add_executable(test_fx_comp test_fx_comp.cpp)
# BACKLOG QG: COMP's ops hold a lane, so they live in feathers-fx-sklitter (the Delay's arrangement).
target_link_libraries(test_fx_comp PRIVATE feathers-rack feathers-fx-sklitter feathers-warnings)
target_include_directories(test_fx_comp PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest ${CMAKE_SOURCE_DIR}/src)
add_test(NAME fx-comp COMMAND test_fx_comp)

# CLIP — the clipper's five curves asserted on their own (inside ±1, exact below the knee, Texture's
# structural |y| <= |c|), then the device: the ceiling, the untouched passband, the aliases 4x removes.
add_executable(test_fx_clip test_fx_clip.cpp)
target_link_libraries(test_fx_clip PRIVATE feathers-fx feathers-warnings)
target_include_directories(test_fx_clip PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest)
add_test(NAME fx-clip COMMAND test_fx_clip)

# NOISE (backlog MZ) — the tilt's slope per octave against the target, the impulses' rate by count (the shared
# random-event core and the device), the seed's determinism, the filter's corner, the bit-exact pass-through.
add_executable(test_fx_noise test_fx_noise.cpp)
target_link_libraries(test_fx_noise PRIVATE feathers-fx feathers-warnings)
target_include_directories(test_fx_noise PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest)
add_test(NAME fx-noise COMMAND test_fx_noise)

# RANDOM TRIGGER (backlog NA (a)) — counts per distribution, hit-for-hit parity with a NOISE instance, the Trig
# pulse, Size and Decay at known hits, Poly independence, and the card's drawn distributions against a histogram.
add_executable(test_mod_rtrig test_mod_rtrig.cpp)
target_link_libraries(test_mod_rtrig PRIVATE feathers-rack feathers-fx-sklitter feathers-warnings)
target_include_directories(test_mod_rtrig PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest ${CMAKE_SOURCE_DIR}/src)
add_test(NAME mod-rtrig COMMAND test_mod_rtrig)

# RESONATE — the scanned-string effect. Its descriptor first (keys, defaults pinned to Settings{}, the row
# round trip, missing fields at their declared defaults); the DSP's cases join it with the plan's Task 7.
add_executable(test_fx_resonate test_fx_resonate.cpp)
target_link_libraries(test_fx_resonate PRIVATE feathers-fx-sklitter feathers-warnings)
target_include_directories(test_fx_resonate PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest)
# The longest suite in the tree (421 s on a quiet round, 1669 s under load, 2026-10-18), so it set the floor of
# every full ctest. SIX shards, not four, and the reason is where the cost sits in NAME order: the buzz cases
# ("a Cycle pluck's...", "at Bridge Fixed 1...", "at Sympathy's default...", "at the defaults...") sort next to each
# other, so at four the third shard held 353 s of a 1000 s suite; at six the worst is ~240 s. The floor no split
# gets under is the longest single case, ~85 s loaded ("a Cycle pluck's drone string buzzes more the closer its bridge").
feathers_add_sharded_test(NAME fx-resonate TARGET test_fx_resonate
	SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/test_fx_resonate.cpp SHARDS 6)

# THE RESONATE BENCHMARK — NOT A TEST (no add_test), bench_render's posture: microseconds per host block
# at eight heads and the longest FIR. Its header says how to read it.
add_executable(bench_resonate bench_resonate.cpp)
target_link_libraries(bench_resonate PRIVATE feathers-fx-sklitter feathers-warnings)

# The modulator rack's first device, on the same terms: it links feathers-mod and nothing else, so a
# link error here is the boundary breaking before the include check runs.
add_executable(test_mod_dahdsr test_mod_dahdsr.cpp)
target_link_libraries(test_mod_dahdsr PRIVATE feathers-mod feathers-warnings)
target_include_directories(test_mod_dahdsr PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest)
target_compile_definitions(test_mod_dahdsr PRIVATE FEATHERS_MOD_SOURCE_DIR="${CMAKE_SOURCE_DIR}/src/mod")
add_test(NAME mod-dahdsr COMMAND test_mod_dahdsr)

# The SECOND modulator, and its own suite for the same reason the DAHDSR has one: the device's
# arithmetic is answerable without a voice layer or an editor, and a case that needs neither should
# not pay for both.
add_executable(test_mod_mseg test_mod_mseg.cpp)
target_link_libraries(test_mod_mseg PRIVATE feathers-mod feathers-warnings)
target_include_directories(test_mod_mseg PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest)
add_test(NAME mod-mseg COMMAND test_mod_mseg)

# THE LFO — waveform generator with skew, random walk, and the same sync/rate/modes as the MSEG.
add_executable(test_mod_lfo test_mod_lfo.cpp)
target_link_libraries(test_mod_lfo PRIVATE feathers-mod feathers-warnings)
target_include_directories(test_mod_lfo PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest)
add_test(NAME mod-lfo COMMAND test_mod_lfo)

# Smoothing across a retrigger (backlog MM), for the three shaped modulators at once — the LFO, the MSEG
# and the DAHDSR share `mod::PolyShape`, which is where a new note's shape begins.
add_executable(test_mod_retrigger_smoothing test_mod_retrigger_smoothing.cpp)
target_link_libraries(test_mod_retrigger_smoothing PRIVATE feathers-mod feathers-warnings)
target_include_directories(test_mod_retrigger_smoothing PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest)
add_test(NAME mod-retrigger-smoothing COMMAND test_mod_retrigger_smoothing)

# The Trig convention (backlog MV): fires at 0.5, re-arms below 0.25 — the edge, the three takers, and a route.
add_executable(test_mod_trig test_mod_trig.cpp)
target_link_libraries(test_mod_trig PRIVATE feathers-rack feathers-fx-sklitter feathers-warnings)
target_include_directories(test_mod_trig PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest ${CMAKE_SOURCE_DIR}/src)
add_test(NAME mod-trig COMMAND test_mod_trig)

# The rack where it meets the engine — the gate, the control tick, the bus and the bridge. Unlike the
# device's own suite above, this one needs the whole voice layer and the editor's parameter road,
# because every claim in it is about a seam between them.
add_executable(test_mod_rack test_mod_rack.cpp)
target_link_libraries(test_mod_rack PRIVATE feathers-synth feathers-gui-bridge feathers-warnings)
target_include_directories(test_mod_rack PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest)
add_test(NAME mod-rack COMMAND test_mod_rack)


# THE DRAWN-CURVE FILTER, PHASE 1 — the types and the blend, headless. Links feathers-fx and nothing
# else, on the same terms as the waypoint suite above: a link error here is the device boundary
# breaking before any include check runs.
add_executable(test_fx_drawn test_fx_drawn.cpp)
target_link_libraries(test_fx_drawn PRIVATE feathers-fx feathers-warnings)
target_include_directories(test_fx_drawn PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest)
add_test(NAME fx-drawn COMMAND test_fx_drawn)


# THE ROUTING AUTHORITY — the structural half of "may this source reach this destination", with the
# host's own rules passed in. Links feathers-fx and feathers-mod for REAL descriptors: the cases
# assert the relationship between what a device declares and what the table admits, so a fake
# descriptor would let the relationship hold against numbers no shipping device has.
add_executable(test_backplane_routing test_backplane_routing.cpp)
target_link_libraries(test_backplane_routing PRIVATE feathers-fx feathers-mod feathers-warnings)
target_include_directories(test_backplane_routing PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest ${CMAKE_SOURCE_DIR}/src)
add_test(NAME backplane-routing COMMAND test_backplane_routing)


# THE GRAPH — nodes keyed on identity, lanes that never move, typed edges, chains as paths over one
# pool, and the order. Header-only, fakes for descriptors, so it links nothing but the harness. The
# allocation case replaces global operator new, which is why it has its own binary.
add_executable(test_backplane_graph test_backplane_graph.cpp)
target_link_libraries(test_backplane_graph PRIVATE feathers-warnings)
target_include_directories(test_backplane_graph PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest ${CMAKE_SOURCE_DIR}/src)
add_test(NAME backplane-graph COMMAND test_backplane_graph)

# THE HOST SLOTS' LOCK-FREE ROADS (host-slots plan T2) — the bus the host writes on the audio thread
# and the outbox the editor writes for the host, with its out-event debts. Header-only; its own binary
# because the allocation case replaces global operator new.
add_executable(test_slot_bus test_slot_bus.cpp)
target_link_libraries(test_slot_bus PRIVATE feathers-warnings)
target_include_directories(test_slot_bus PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest ${CMAKE_SOURCE_DIR}/src)
add_test(NAME slot-bus COMMAND test_slot_bus)

# CAN THE GRAPH HOLD FEATHERS' TWO CHAINS AND AGREE ABOUT WHICH ROUTES EXIST? The gating measurement
# of the edge arc's step A, and a case rather than
# a store on purpose. Links the REAL registry — feathers-fx and feathers-mod — because the question is
# which of Feathers' own devices declare `mod` ops and outputs, and a fake would answer about the fake.
add_executable(test_feathers_graph_order test_feathers_graph_order.cpp)
target_link_libraries(test_feathers_graph_order PRIVATE feathers-params feathers-fx feathers-fx-sklitter feathers-mod feathers-rack feathers-warnings)
target_include_directories(test_feathers_graph_order PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest ${CMAKE_SOURCE_DIR}/src)
add_test(NAME feathers-graph-order COMMAND test_feathers_graph_order)

# SOURCE ENUMERATION — one index, one answer, whether the source is the host's own or a device output.
# Links feathers-fx and feathers-mod for real declarations: the filter publishes no outputs and the
# DAHDSR publishes some, which is the contrast the slack-row cases are built on.
add_executable(test_backplane_sources test_backplane_sources.cpp)
target_link_libraries(test_backplane_sources PRIVATE feathers-fx feathers-mod feathers-warnings)
target_include_directories(test_backplane_sources PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest ${CMAKE_SOURCE_DIR}/src)
add_test(NAME backplane-sources COMMAND test_backplane_sources)

# A DEVICE'S CONTROL, DRIVEN BY A HOST THAT IS NOT FEATHERS. The fixture implements
# `backplane::DeviceHost` in about forty lines over a route table and two arrays — no ParamBridge, no
# clap ids, no synth. It is the second implementor `device.h` said the modulation contract should have
# been designed against, and the first evidence that the card layer is portable rather than merely
# tidy.
add_executable(test_backplane_device_control test_backplane_device_control.cpp)
target_link_libraries(test_backplane_device_control PRIVATE feathers-ui feathers-warnings)
target_include_directories(test_backplane_device_control PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest ${CMAKE_SOURCE_DIR}/src)
add_test(NAME backplane-device-control COMMAND test_backplane_device_control)

# THE VALUE FORMATTER OUT OF THE UI (host slots T3): `device::formatValue` against the UI's name for it
# over every registry control and against strings captured before the move, and the 0..1 road a host
# slot takes. It links feathers-ui for the UI half and feathers-synth because it walks `kDevices`.
add_executable(test_format_value test_format_value.cpp)
target_link_libraries(test_format_value PRIVATE feathers-ui feathers-synth feathers-warnings)
target_include_directories(test_format_value PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest ${CMAKE_SOURCE_DIR}/src)
add_test(NAME format-value COMMAND test_format_value)

# THE RACK ENGINE — a chain, a route table and eight macros, hosted by nothing. Every case about
# modulation asserts the RESOLVED VALUE; the render comparisons appear only as second assertions
# proving the value claims are not vacuous.
# THE CPU SPIKE RECORDER (backlog SR): the recorder against a real engine that times its devices.
add_executable(test_spike_log test_spike_log.cpp)
target_link_libraries(test_spike_log PRIVATE feathers-spike-log feathers-rack feathers-fx-sklitter feathers-warnings)
target_include_directories(test_spike_log PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest ${CMAKE_SOURCE_DIR}/src)
# NOT RUN ON WINDOWS (built, so it still compiles there): the recorder's cpu and preemption fields come from
# CLOCK_THREAD_CPUTIME_ID and getrusage, which the Windows build does not read (threadCpuNs answers kNoCpuTime), so the
# suite's claims are about values that platform does not produce, and the CPU-clock spin never ends there.
if(NOT WIN32)
	add_test(NAME spike-log COMMAND test_spike_log)
endif()

add_executable(test_rack_engine test_rack_engine.cpp)
target_link_libraries(test_rack_engine PRIVATE feathers-rack feathers-fx-sklitter feathers-warnings)
target_include_directories(test_rack_engine PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest ${CMAKE_SOURCE_DIR}/src)
add_test(NAME rack-engine COMMAND test_rack_engine)

# THE DEVICE SDK'S EXAMPLE, A GUEST BUILT AGAINST THE SDK ALONE, REGISTERED AND ROUTED BY VALUE IN THE RACK.
add_executable(test_sdk_device test_sdk_device.cpp)
target_link_libraries(test_sdk_device PRIVATE feathers-rack feathers-fx-sklitter feathers-warnings)
target_include_directories(test_sdk_device PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/libs/feathers-device-sdk/example)
add_test(NAME sdk-device COMMAND test_sdk_device)

# THE DESIRED-STATE PROTOCOL (desired state, in both directions) — a `ChainState` and the
# `Converger` that makes a bare engine become it, asked without Feathers or a bridge in the way.
add_executable(test_converger test_converger.cpp)
target_link_libraries(test_converger PRIVATE feathers-rack feathers-fx-sklitter feathers-warnings)
target_include_directories(test_converger PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest ${CMAKE_SOURCE_DIR}/src)
add_test(NAME converger COMMAND test_converger)

# THE RECORD a host's editor edits, and what it publishes to the drain — the other half of the
# desired-state protocol, and the end-to-end road through a bare engine.
add_executable(test_chain_record test_chain_record.cpp)
target_link_libraries(test_chain_record PRIVATE feathers-rack feathers-fx-sklitter feathers-warnings)
target_include_directories(test_chain_record PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest ${CMAKE_SOURCE_DIR}/src)
add_test(NAME chain-record COMMAND test_chain_record)

# THE RACK THROUGH THE REAL CLAP API — ports, parameters, notes and audio, with a host. The engine's
# own suite proves the arithmetic headless; this proves the BINDING, which that suite cannot see.
add_executable(test_rack_clap test_rack_clap.cpp)
target_link_libraries(test_rack_clap PRIVATE feathers-rack-ui feathers-warnings)
target_include_directories(test_rack_clap PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest ${CMAKE_SOURCE_DIR}/src)
# X11 for the one case that embeds the editor in a real parent window, the way a host does (the window's
# first resize, 2026-10-10).
if(UNIX AND NOT APPLE)
	find_package(X11 REQUIRED)
	target_link_libraries(test_rack_clap PRIVATE ${X11_LIBRARIES})
	target_include_directories(test_rack_clap PRIVATE ${X11_INCLUDE_DIR})
endif()
# ON THE PRIVATE DISPLAY TOO (2026-09-03): with the widget shards and gui-windowed venued, this was the
# one entry a full ctest still ran on whatever DISPLAY it inherited -- and the one red in the A/B below
# when ctest ran with none. Now every headless-incompatible entry brings its own display, so a full
# ctest needs no DISPLAY at all and two sessions' suites cannot photograph each other anywhere.
add_test(NAME rack-clap COMMAND ${CMAKE_SOURCE_DIR}/scripts/venue-xvfb.sh run -- $<TARGET_FILE:test_rack_clap>)
set_tests_properties(rack-clap PROPERTIES LABELS "headless-incompatible")

# THE RACK'S PATCH — round trip, and the refusals. The case that matters most is the HOLE: a device
# this build cannot run must leave its slot empty, or every route above it silently re-points.
add_executable(test_rack_state test_rack_state.cpp)
target_link_libraries(test_rack_state PRIVATE feathers-rack feathers-fx-sklitter feathers-warnings)
target_include_directories(test_rack_state PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest ${CMAKE_SOURCE_DIR}/src)
add_test(NAME rack-state COMMAND test_rack_state)

# THE STATE FORMAT'S TEXT (backlog JF step 1a) — the number road to the bit, the grammar, `$bin`, and a
# seeded mutation fuzzer. Links the rack only for `kDevices`, whose every control value it round-trips.
add_executable(test_state_json test_state_json.cpp)
target_link_libraries(test_state_json PRIVATE feathers-state feathers-rack feathers-fx-sklitter feathers-warnings)
target_include_directories(test_state_json PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest ${CMAKE_SOURCE_DIR}/src)
add_test(NAME state-json COMMAND test_state_json)

# THE HOST SLOT POOL (backlog JU, host-slots plan T1) — the states, the allocation order, the default tiers,
# free and undo, rebase and snapshot, all by value. Pure: links the pool and nothing else.
add_executable(test_slot_pool test_slot_pool.cpp)
target_link_libraries(test_slot_pool PRIVATE feathers-hostslots feathers-warnings)
target_include_directories(test_slot_pool PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest ${CMAKE_SOURCE_DIR}/src)
add_test(NAME slot-pool COMMAND test_slot_pool)

# THE SCHEMA LEDGER (backlog JF step 1b) — every stored key, checked against `tests/ledger/*.ledger` (one file per device);
# what guards a device edit in place of `kStateVersion`. The suite's banner says what each difference means.
add_executable(test_state_ledger test_state_ledger.cpp)
target_link_libraries(test_state_ledger PRIVATE feathers-state feathers-params feathers-rack feathers-fx-sklitter feathers-warnings)
target_include_directories(test_state_ledger PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(test_state_ledger PRIVATE FEATHERS_LEDGER_DIR="${CMAKE_SOURCE_DIR}/tests/ledger")
add_test(NAME state-ledger COMMAND test_state_ledger)

# THE STATE FORMAT'S WHOLE POINT (backlog JF step 1c) — a SKLITTER device preset and a Feathers state
# holding Featherverb, loaded by a build whose device changed: every survivor by key, the new control at
# its default, a Choice by its label, and a load report naming exactly what did not load as saved.
add_executable(test_state_evolution test_state_evolution.cpp)
target_link_libraries(test_state_evolution PRIVATE feathers-ui feathers-gui-bridge feathers-rack feathers-fx-sklitter
                      feathers-warnings)
target_include_directories(test_state_evolution PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest ${CMAKE_SOURCE_DIR}/src)
add_test(NAME state-evolution COMMAND test_state_evolution)

# RESONATE'S FACTORY PRESETS — the table is the spec, the shipped files are asserted against it by value, and every
# one is rendered at a typical input and held under 0 dBFS. It also WRITES them (FEATHERS_RESONATE_PRESETS_OUT).
add_executable(test_resonate_presets test_resonate_presets.cpp)
target_link_libraries(test_resonate_presets PRIVATE feathers-ui feathers-gui-bridge feathers-rack feathers-fx-sklitter
                      feathers-warnings)
target_include_directories(test_resonate_presets PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest ${CMAKE_SOURCE_DIR}/src)
add_test(NAME resonate-presets COMMAND test_resonate_presets)

# RESONATE'S TWO FX LANES, BY VALUE — the empty-lane render is the DSP's own in both routings, the routing switch decides
# whether WET FX hears the drone, a latent device is reported and aligned on every path, and a lane's tail rings past
# the string and still ends in exact zero.
add_executable(test_resonate_lanes test_resonate_lanes.cpp)
target_link_libraries(test_resonate_lanes PRIVATE feathers-rack feathers-fx-sklitter feathers-warnings)
target_include_directories(test_resonate_lanes PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest ${CMAKE_SOURCE_DIR}/src)
add_test(NAME resonate-lanes COMMAND test_resonate_lanes)

# BACKLOG ME: a modulator inside a holder's inner chain reaching the HOLDER's own controls — the audit over every
# device that holds a chain, the requirement (a known gap, may_fail) and the gap as measured.
add_executable(test_holder_inner_mod test_holder_inner_mod.cpp)
target_link_libraries(test_holder_inner_mod PRIVATE feathers-rack feathers-fx-sklitter feathers-warnings)
target_include_directories(test_holder_inner_mod PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest ${CMAKE_SOURCE_DIR}/src)
add_test(NAME holder-inner-mod COMMAND test_holder_inner_mod)

# THE OS FILE DIALOG — its async contract with a fake backend, the D-Bus codec against golden bytes from
# GLib and libdbus, and (Linux, where dbus-daemon is installed) a private bus with a fake portal on it.
# No window, no display: `nativeWindowOf` is asked only about frames in no window.
add_executable(test_file_dialog test_file_dialog.cpp)
target_link_libraries(test_file_dialog PRIVATE feathers-ui feathers-warnings)
target_include_directories(test_file_dialog PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest ${CMAKE_SOURCE_DIR}/src)
add_test(NAME file-dialog COMMAND test_file_dialog)

# THE NON-WINDOWING SURFACE HOST — the contract, not the pixels. It links `feathers-ui` alone, which
# is the claim: a takeover is a card-floor thing, so Feathers and the rack get the same one.
add_executable(test_takeover test_takeover.cpp)
target_link_libraries(test_takeover PRIVATE feathers-ui feathers-warnings)
target_include_directories(test_takeover PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest ${CMAKE_SOURCE_DIR}/src)
add_test(NAME takeover COMMAND test_takeover)
# NOT headless-incompatible, MEASURED: `env -u DISPLAY -u WAYLAND_DISPLAY ./build/tests/test_takeover`
# is 15/15. Nothing here opens a window — a `TakeoverStack` is a `Frame` that is laid out, pressed and
# keyed by injection, never shown. The label was on it because it was copied from `rack-editor` below,
# which is the way a wrong label spreads: it is inherited from a neighbour rather than decided.

# THE CHROME HUE (backlog LG) — OKLCH L, C and h of every token the offset turns, by value, and the prefs
# road on a scratch desk its own main() makes. No window: colours are numbers here.
add_executable(test_chrome_hue test_chrome_hue.cpp)
target_link_libraries(test_chrome_hue PRIVATE feathers-ui feathers-warnings)
target_include_directories(test_chrome_hue PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest ${CMAKE_SOURCE_DIR}/src)
add_test(NAME chrome-hue COMMAND test_chrome_hue)

# THE RACK'S WRITE BOUNDARY — not a test of behaviour but of the artefact, like `runtime-deps` above.
#
# Only `RackBridge::drain` and `applyRack` may write engine state; a direct call races a block
# (backlog BW/CD). This asserts that no rack UI object REFERENCES one of the eight drain-only
# mutators, read out of what the compiler emitted rather than out of the source. The script's header
# carries why this rather than a private section (114 test call sites, costed and declined) and why
# this rather than a link line (the three conditions `4e15040` relied on all fail here).
#
# **THE OBJECT LIST IS NAMED HERE AND THAT IS THIS CHECK'S BOUNDARY**: a translation unit added to
# `feathers-rack-ui` later is outside it until somebody adds it to this line. The script fails rather
# than passes if any named object is missing, so an emptied list reds instead of going quiet.
#
# Linux only, following `runtime-deps`: the object layout below is the Makefile/Ninja generators' and
# `nm`'s output is binutils'. A second platform wants its own measurement rather than this one's
# assumptions.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
	# TWO LIBRARIES SINCE 2026-09-13: the editor (rack_editor, add_device, chain_expanded) is
	# `feathers-rack-editor`, so Feathers can link it; the plugin (rack_clap, rack_gui) stays in
	# `feathers-rack-ui`. Both are guarded. **A split that moves an object out of this list is what the
	# missing-object failure below exists for** — and it was found by exactly that: the first tree to build
	# the split from scratch went red where a tree with stale objects in the old directory had stayed green.
	set(_rack_ui_objs "${CMAKE_BINARY_DIR}/CMakeFiles/feathers-rack-ui.dir/src/rack")
	set(_rack_editor_objs "${CMAKE_BINARY_DIR}/CMakeFiles/feathers-rack-editor.dir/src/rack")
	add_test(NAME rack-write-boundary COMMAND ${CMAKE_COMMAND}
		-DENGINE_OBJ=${CMAKE_BINARY_DIR}/CMakeFiles/feathers-rack.dir/src/rack/rack_engine.cpp.o
		-DGUARDED_OBJS=${_rack_ui_objs}/rack_clap.cpp.o|${_rack_ui_objs}/rack_gui.cpp.o|${_rack_editor_objs}/rack_editor.cpp.o|${_rack_editor_objs}/add_device.cpp.o|${_rack_editor_objs}/chain_expanded.cpp.o
		-P ${CMAKE_SOURCE_DIR}/cmake/CheckRackWriteBoundary.cmake
	)
endif()

# THE RACK'S SURFACE, HEADLESS. Nothing here claims anything about pixels — a repaint cannot be
# observed without a window. What it claims is that the surface is built from the card floor at all.
add_executable(test_rack_editor test_rack_editor.cpp)
target_link_libraries(test_rack_editor PRIVATE feathers-rack-ui feathers-warnings)
target_include_directories(test_rack_editor PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest ${CMAKE_SOURCE_DIR}/src)
add_test(NAME rack-editor COMMAND test_rack_editor)
# BACKLOG PU: the as-modulated readout, by value against the engine and an independent restatement (headless).
add_executable(test_mod_readout test_mod_readout.cpp)
target_link_libraries(test_mod_readout PRIVATE feathers-rack-ui feathers-warnings)
target_include_directories(test_mod_readout PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest ${CMAKE_SOURCE_DIR}/src)
add_test(NAME mod-readout COMMAND test_mod_readout)
add_executable(test_mod_readout_feathers test_mod_readout_feathers.cpp)
target_link_libraries(test_mod_readout_feathers PRIVATE feathers-ui feathers-synth feathers-gui-bridge feathers-warnings)
target_include_directories(test_mod_readout_feathers PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest ${CMAKE_SOURCE_DIR}/src)
add_test(NAME mod-readout-feathers COMMAND test_mod_readout_feathers)
# NOT headless-incompatible either, MEASURED the same way: 19/19 with no display. The editor is built
# and laid out but never shown, and the one rack suite that DOES need a display is `rack-clap`, which
# segfaults without one (exit 139) — so the distinction is real and this is on the wrong side of it.
#
# WHY IT MATTERS RATHER THAN BEING TIDINESS: a headless CI leg SKIPS this label, so a mislabelled suite
# contributes nothing there while looking like coverage. And it enlarges the population of tests that
# open real windows in a full local run, which is the population the unexplained windowed flake lives
# in — two suites that never needed a window were sitting in it.

# WHAT A RACK CAN BE GIVEN, grouped the way the devices ask to be. A pure function over the registry,
# so the grouping is checkable without a window — the widget that draws it is a separate thing.
add_executable(test_device_browser test_device_browser.cpp)
target_link_libraries(test_device_browser PRIVATE feathers-rack-ui feathers-warnings)
target_include_directories(test_device_browser PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest ${CMAKE_SOURCE_DIR}/src)
add_test(NAME device-browser COMMAND test_device_browser)

# WHAT EVERY DEVICE'S DECLARED RANGES PROMISE, AND WHETHER ITS DECODE KEEPS THE PROMISE. A pure
# function over the registry, like the browser above and headless for the same reason: no window is
# involved in the claim that a value inside a control's declared range survives `stateOf`.
#
# `feathers-synth` RATHER THAN `feathers-fx`, because the sweep walks `kDevices` and that table names
# the modulators too — a link error here would be a device that cannot be reached from the registry.
add_executable(test_device_ranges test_device_ranges.cpp)
target_link_libraries(test_device_ranges PRIVATE feathers-synth feathers-warnings)
target_include_directories(test_device_ranges PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest ${CMAKE_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME device-ranges COMMAND test_device_ranges)

# THE NOTE-EXPRESSION MODULATOR, with no host on it. Its PLUMBING — an expression reaching a voice
# through the note lane — is test_synth.cpp's; this is the rule and the held-note stack.
add_executable(test_mod_noteexpr test_mod_noteexpr.cpp)
target_link_libraries(test_mod_noteexpr PRIVATE feathers-mod feathers-warnings)
target_include_directories(test_mod_noteexpr PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest ${CMAKE_SOURCE_DIR}/src)
add_test(NAME mod-noteexpr COMMAND test_mod_noteexpr)

# THE TRACKER THROUGH ITS OPS (backlog EH): its note stack had no suite of its own — its row codec is
# covered by test_device_ranges and its rule by bench_render's seams — and the sweep found a note
# kind it mishandled.
add_executable(test_mod_tracker test_mod_tracker.cpp)
target_link_libraries(test_mod_tracker PRIVATE feathers-mod feathers-warnings)
target_include_directories(test_mod_tracker PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest ${CMAKE_SOURCE_DIR}/src)
add_test(NAME mod-tracker COMMAND test_mod_tracker)

# THE CLASSIC ARP, by value: every case reads note events and the sample each landed
# on — through the synth's own clock-call protocol, and in the synth, its steps reaching the voices.
add_executable(test_arp test_arp.cpp)
target_link_libraries(test_arp PRIVATE feathers-synth feathers-warnings)
target_include_directories(test_arp PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest ${CMAKE_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME arp COMMAND test_arp)

# THE PATTERN DEVICE'S THEORY (terrain-arp design §9, §10): scales, chords stacked in a scale, voicings and voice
# leading, by value and against a brute force. Pure functions in feathers-mod; nothing else.
add_executable(test_arp_theory test_arp_theory.cpp)
target_link_libraries(test_arp_theory PRIVATE feathers-synth feathers-warnings)
target_include_directories(test_arp_theory PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest ${CMAKE_SOURCE_DIR}/src)
add_test(NAME arp-theory COMMAND test_arp_theory)

# THE PATTERN DEVICE'S GENE SPACE (terrain-arp design §4–§6): the row bank, the pins, and the continuity bound.
add_executable(test_arp_genes test_arp_genes.cpp)
target_link_libraries(test_arp_genes PRIVATE feathers-synth feathers-warnings)
target_include_directories(test_arp_genes PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest ${CMAKE_SOURCE_DIR}/src)
add_test(NAME arp-genes COMMAND test_arp_genes)

# THE PATTERN BUILDER (terrain-arp design §5, §8–§11): the five Modes, Keys, scales, chords, ties, slides, the polymeter.
add_executable(test_arp_pattern test_arp_pattern.cpp)
target_link_libraries(test_arp_pattern PRIVATE feathers-synth feathers-warnings)
target_include_directories(test_arp_pattern PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest ${CMAKE_SOURCE_DIR}/src)
add_test(NAME arp-pattern COMMAND test_arp_pattern)

# THE PATTERN DEVICE (terrain-arp design §7, §10, §11): which pattern plays, on which sample, with which keys; the
# selector, solo, slides, loops, mode switches with no stuck notes, and the hand's block across a save.
add_executable(test_pattern_device test_pattern_device.cpp)
target_link_libraries(test_pattern_device PRIVATE feathers-synth feathers-warnings)
target_include_directories(test_pattern_device PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest ${CMAKE_SOURCE_DIR}/src)
add_test(NAME pattern-device COMMAND test_pattern_device)

# PATTERN'S CYCLE PLAY MODE (terrain-arp design §7.6): the walk derived from the song position — Next, Previous, Random,
# Stop, Loop across mixed Lengths, Repeats and Rates; a scrub lands on the walk; the Selector road pinned bit for bit.
add_executable(test_pattern_cycle test_pattern_cycle.cpp)
target_link_libraries(test_pattern_cycle PRIVATE feathers-synth feathers-warnings)
target_include_directories(test_pattern_cycle PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest ${CMAKE_SOURCE_DIR}/src)
add_test(NAME pattern-cycle COMMAND test_pattern_cycle)

# PATTERN → .mid (terrain-arp design §7.9): the bytes parsed back to the device's own notes; TB-Glide as MPE.
add_executable(test_pattern_midi test_pattern_midi.cpp)
target_link_libraries(test_pattern_midi PRIVATE feathers-synth feathers-warnings)
target_include_directories(test_pattern_midi PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest ${CMAKE_SOURCE_DIR}/src)
add_test(NAME pattern-midi COMMAND test_pattern_midi)

# THE PATTERN LIKES' WARP, measured before it ships (mod/arp/pattern_warp.h): the quick checks under ctest, the sweep by
# hand (FEATHERS_STUDY=1).
add_executable(study_pattern_warp study_pattern_warp.cpp)
target_link_libraries(study_pattern_warp PRIVATE feathers-synth feathers-warnings)
target_include_directories(study_pattern_warp PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest ${CMAKE_SOURCE_DIR}/src)
add_test(NAME pattern-warp COMMAND study_pattern_warp)

# THE MACROS MODULATOR (macro-modulation B1): its outputs through its ops, a route into a macro at the
# engine, and its seat in both hosts. It links feathers-synth, as test_device_ranges does, because the
# last two read `kDevices` and run a `RackEngine`.
add_executable(test_mod_macros test_mod_macros.cpp)
target_link_libraries(test_mod_macros PRIVATE feathers-synth feathers-warnings)
target_include_directories(test_mod_macros PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest ${CMAKE_SOURCE_DIR}/src)
add_test(NAME mod-macros COMMAND test_mod_macros)

# THE MATH MODULATOR (macro-modulation C): `A op B`, clamped, mapped, scaled — against the design restated;
# then the device through its ops and routes into it at the engine. feathers-synth for MACROS' reason: the
# engine cases read `kDevices` and run a `RackEngine`.
add_executable(test_mod_math test_mod_math.cpp)
target_link_libraries(test_mod_math PRIVATE feathers-synth feathers-warnings)
target_include_directories(test_mod_math PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest ${CMAKE_SOURCE_DIR}/src)
add_test(NAME mod-math COMMAND test_mod_math)

# THE ROUTE TRANSFER CURVES PINNED TO THE BIT, written before the device SDK cut the phi curve out of the vocabulary.
add_executable(test_transfer_pin test_transfer_pin.cpp)
target_link_libraries(test_transfer_pin PRIVATE feathers-device feathers-warnings)
target_include_directories(test_transfer_pin PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest ${CMAKE_SOURCE_DIR}/src)
add_test(NAME transfer-pin COMMAND test_transfer_pin)
set_tests_properties(transfer-pin PROPERTIES LABELS "platform-pinned")

# SAMPLE & HOLD (backlog MW): what is held and when, by value, through the device's ops and through the rack for Poly.
# test_mod_trig's link line, for the same reason: the Poly cases run a `RackEngine` over `hostCatalogue()`.
add_executable(test_mod_samplehold test_mod_samplehold.cpp)
target_link_libraries(test_mod_samplehold PRIVATE feathers-synth feathers-gui-bridge feathers-warnings)
target_include_directories(test_mod_samplehold PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest ${CMAKE_SOURCE_DIR}/src)
add_test(NAME mod-samplehold COMMAND test_mod_samplehold)

# A DEVICE'S KEYED BLOCKS (backlog NM step 1, `device/state_blocks.h`) — a fixture larger than the row, saved and loaded
# into a fresh rack through the bridge and through an engine with no bridge.
add_executable(test_state_blocks test_state_blocks.cpp)
target_link_libraries(test_state_blocks PRIVATE feathers-rack feathers-fx-sklitter feathers-warnings)
target_include_directories(test_state_blocks PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest ${CMAKE_SOURCE_DIR}/src)
add_test(NAME state-blocks COMMAND test_state_blocks)

# A HAND'S BLOCK (`StateBlockDesc::handEdited`): the road for user-edited state too large for the row, over a fixture device.
add_executable(test_hand_blocks test_hand_blocks.cpp)
target_link_libraries(test_hand_blocks PRIVATE feathers-rack feathers-fx-sklitter feathers-warnings)
target_include_directories(test_hand_blocks PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest ${CMAKE_SOURCE_DIR}/src)
add_test(NAME hand-blocks COMMAND test_hand_blocks)

# THE STEP SEQUENCER (backlog MX) — directions, dice, ratchets, Turing, the Trigs, Poly, Randomise, and the pattern's
# save and load through the rack's patch. Links the rack for the patch road and `hostCatalogue`.
add_executable(test_mod_stepseq test_mod_stepseq.cpp)
target_link_libraries(test_mod_stepseq PRIVATE feathers-synth feathers-gui-bridge feathers-warnings)
target_include_directories(test_mod_stepseq PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest ${CMAKE_SOURCE_DIR}/src)
add_test(NAME mod-stepseq COMMAND test_mod_stepseq)

# THE FOLLOWER (backlog MY) — the time constants on a step, Peak against RMS on a sine, Gain and Floor, the Trig
# edge and its hysteresis, the control Input and the sidechain through the engine, the graph's admission, no
# allocation. Values asserted, references restated from the words.
add_executable(test_mod_follower test_mod_follower.cpp)
target_link_libraries(test_mod_follower PRIVATE feathers-synth feathers-warnings)
target_include_directories(test_mod_follower PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest ${CMAKE_SOURCE_DIR}/src)
add_test(NAME mod-follower COMMAND test_mod_follower)

# THE COMPOSITE'S OUTWARD SURFACE — a descriptor with no ops (step 6a). It links feathers-synth for
# the same reason test_device_ranges does: `kDevices` holds pointers to ops defined in the device
# .cpp files, so a TU that merely READS the catalogue still needs them at link time.
add_executable(test_composite_surface test_composite_surface.cpp)
target_link_libraries(test_composite_surface PRIVATE feathers-synth feathers-warnings)
target_include_directories(test_composite_surface PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(test_composite_surface PRIVATE FEATHERS_DEVICE_SOURCE_DIR="${CMAKE_SOURCE_DIR}/src/device")
target_compile_definitions(test_composite_surface PRIVATE FEATHERS_DEVICE_SDK_DIR="${CMAKE_SOURCE_DIR}/libs/feathers-device-sdk")
add_test(NAME composite-surface COMMAND test_composite_surface)

# WHERE A CONTAINER'S BODY WAITS between the record read and the device's construction. Header-only,
# so it links nothing but the warnings interface.
add_executable(test_container_bodies test_container_bodies.cpp)
target_link_libraries(test_container_bodies PRIVATE feathers-warnings)
target_include_directories(test_container_bodies PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest ${CMAKE_SOURCE_DIR}/src)
add_test(NAME container-bodies COMMAND test_container_bodies)



# A CHAIN'S CONTENTS ON THE REAL SAVE, LOAD AND PROVISION ROAD — the serialisers live in
# feathers-gui-bridge (audition.cpp), the runner in feathers-synth.
add_executable(test_container_state test_container_state.cpp)
target_link_libraries(test_container_state PRIVATE feathers-gui-bridge feathers-synth feathers-rack feathers-warnings)
target_include_directories(test_container_state PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest ${CMAKE_SOURCE_DIR}/src)
add_test(NAME container-state COMMAND test_container_state)

# EVERY REGISTERED DEVICE CONTROL HAS A TOOLTIP — walks kDevices and fails on a nullptr tooltip.
# Backlog EX: the count of tooltipless controls was 89; this is what stops it creeping back.
add_executable(test_device_tooltips test_device_tooltips.cpp)
target_link_libraries(test_device_tooltips PRIVATE feathers-synth feathers-warnings)
target_include_directories(test_device_tooltips PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest ${CMAKE_SOURCE_DIR}/src)
add_test(NAME device-tooltips COMMAND test_device_tooltips)

# A RACK INSIDE A RACK — which is not a feature, it is the rack: the same RackEngine the standalone
# plugin runs, as a device in a slot. Also a rack in Feathers' lane, and a third party's chain hosting
# our rack, written against the vocabulary alone.
add_executable(test_rack_nesting test_rack_nesting.cpp)
target_link_libraries(test_rack_nesting PRIVATE feathers-gui-bridge feathers-synth feathers-rack feathers-warnings)
target_include_directories(test_rack_nesting PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(test_rack_nesting PRIVATE FEATHERS_RACK_SOURCE_DIR="${CMAKE_SOURCE_DIR}/src/rack")
add_test(NAME rack-nesting COMMAND test_rack_nesting)

# BACKLOG RN — notes through a parallel Rack: what each chain emits, by mode and Mix, by value.
add_executable(test_rack_notes test_rack_notes.cpp)
target_link_libraries(test_rack_notes PRIVATE feathers-gui-bridge feathers-synth feathers-rack feathers-warnings)
target_include_directories(test_rack_notes PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest ${CMAKE_SOURCE_DIR}/src)
add_test(NAME rack-notes COMMAND test_rack_notes)

# BACKLOG LW — events and modulation that follow the audio past a latent device:
# each node's arrival latency, and what a late device is handed, by value.
add_executable(test_latency_domains test_latency_domains.cpp)
target_link_libraries(test_latency_domains PRIVATE feathers-gui-bridge feathers-synth feathers-rack feathers-warnings)
target_include_directories(test_latency_domains PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest ${CMAKE_SOURCE_DIR}/src)
add_test(NAME latency-domains COMMAND test_latency_domains)

# BACKLOG OA — multi-select, copy/paste/duplicate, remove-many, Group into Chain/Rack, by value (rack/fragment.h,
# plugin/ui/fragment_verbs.h): the record, the routes, the labels and the host slots, in the standalone chain's
# shape and in Feathers' lane.
add_executable(test_group_copy test_group_copy.cpp)
target_link_libraries(test_group_copy PRIVATE feathers-ui feathers-gui-bridge feathers-synth feathers-rack feathers-warnings)
target_include_directories(test_group_copy PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest ${CMAKE_SOURCE_DIR}/src)
add_test(NAME group-copy COMMAND test_group_copy)

# BACKLOG OU — chain and rack presets, and the rack plugin's: by value.
add_executable(test_rack_presets test_rack_presets.cpp)
target_link_libraries(test_rack_presets PRIVATE feathers-ui feathers-gui-bridge feathers-synth feathers-rack feathers-warnings)
target_include_directories(test_rack_presets PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest ${CMAKE_SOURCE_DIR}/src)
add_test(NAME rack-presets COMMAND test_rack_presets)

# THE POISONED HEAP — every allocation in this binary arrives full of 0.5f, so a device fed silence
# that answers with anything but zeros has read memory nobody wrote (the 2026-09-24 SKLITTER crush).
add_executable(test_poison_heap test_poison_heap.cpp)
target_link_libraries(test_poison_heap PRIVATE feathers-gui-bridge feathers-synth feathers-rack feathers-warnings)
target_include_directories(test_poison_heap PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest ${CMAKE_SOURCE_DIR}/src)
add_test(NAME poison-heap COMMAND test_poison_heap)
add_executable(test_poison_heap_clap test_poison_heap_clap.cpp)
target_link_libraries(test_poison_heap_clap PRIVATE feathers-rack-ui feathers-warnings)
target_include_directories(test_poison_heap_clap PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest ${CMAKE_SOURCE_DIR}/src)
add_test(NAME poison-heap-clap COMMAND test_poison_heap_clap)

# WHAT A HOST SEES WHEN A MACRO IS RENAMED (backlog JT (a)/(g), KD) — a CLAP host and a VST3 host over
# the SHIPPED artefacts, dlopened. Nothing of ours is compiled in beside them except the test-chain ABI
# header, as for the tool hosts. Links base-sdk-vst3 for the VST3 interface ids only. One ctest entry
# per suite, so the .clap and the .vst3's .so (the same code, both exporting it) never share a process.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND TARGET feathers_vst3 AND TARGET feathers-chain-plugin_vst3)
	add_executable(test_host_rename test_host_rename.cpp)
	target_link_libraries(test_host_rename PRIVATE feathers-clap-sdk base-sdk-vst3 feathers-warnings ${CMAKE_DL_LIBS})
	target_include_directories(test_host_rename PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest ${CMAKE_SOURCE_DIR}/src)
	target_compile_definitions(test_host_rename PRIVATE
		FEATHERS_CLAP_PATH="$<TARGET_FILE:feathers_clap>"
		FEATHERS_RACK_CLAP_PATH="$<TARGET_FILE:feathers-chain-plugin_clap>"
		FEATHERS_VST3_PATH="$<TARGET_FILE:feathers_vst3>"
		FEATHERS_RACK_VST3_PATH="$<TARGET_FILE:feathers-chain-plugin_vst3>")
	add_dependencies(test_host_rename feathers_clap feathers-chain-plugin_clap feathers_vst3 feathers-chain-plugin_vst3)
	add_test(NAME host-rename-clap COMMAND test_host_rename --test-suite=clap)
	add_test(NAME host-rename-vst3 COMMAND test_host_rename --test-suite=vst3)
endif()

# THE RACK RUNS FEATHERS' PER-VOICE ARRANGEMENT ITSELF — an instance per voice beside the bus one, on
# the same build/park/reap road. Against a probe device whose every effect is a readable number.
add_executable(test_rack_voices test_rack_voices.cpp)
target_link_libraries(test_rack_voices PRIVATE feathers-rack feathers-warnings)
target_include_directories(test_rack_voices PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest ${CMAKE_SOURCE_DIR}/src)
add_test(NAME rack-voices COMMAND test_rack_voices)

# **A CHAIN A DEVICE HOLDS, AS ONE REUSABLE UNIT** — `rack::InnerChain`, the extraction the
# bolt-on chains need (gap G1). Against the same shape of probe device: every claim
# is a VALUE (the mix restated arithmetically, the chunking as a COUNT of inner blocks, the publish as
# a mirror only a publish can move), because a chain holding a linear device sounds identical however
# it is cut and a difference check is blind to the traps this unit exists to avoid.
add_executable(test_inner_chain test_inner_chain.cpp)
target_link_libraries(test_inner_chain PRIVATE feathers-rack feathers-warnings)
target_include_directories(test_inner_chain PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest ${CMAKE_SOURCE_DIR}/src)
add_test(NAME inner-chain COMMAND test_inner_chain)

# CHAIN ACCESS SITES (backlog NR): the slot's furniture (a Mix and a wet
# Gain the engine blends) and its PRE and POST chains, built, offered, saved and freed by the host's converger.
add_executable(test_site_chains test_site_chains.cpp)
target_link_libraries(test_site_chains PRIVATE feathers-rack feathers-fx feathers-warnings)
target_include_directories(test_site_chains PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest ${CMAKE_SOURCE_DIR}/src)
add_test(NAME site-chains COMMAND test_site_chains)

# LATENCY — backlog JM 1–3: a device declares how late it is, an engine
# sums it serially, a rack device and an inner chain delay their dry by it, and both plugins report it
# through clap_plugin_latency. Its own binary for its own allocation counter; the standalone rack for the
# CLAP road and the Synth for Feathers' lane.
add_executable(test_latency test_latency.cpp)
target_link_libraries(test_latency PRIVATE feathers-rack-ui feathers-synth feathers-warnings)
target_include_directories(test_latency PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest ${CMAKE_SOURCE_DIR}/src)
add_test(NAME latency COMMAND test_latency)

# THE RACK RESOLVES FEATHERS' WAY — through the published edge list and the shared span and landing
# rules — pinned as values, with the list following the graph through every mutation.
add_executable(test_rack_resolve test_rack_resolve.cpp)
target_link_libraries(test_rack_resolve PRIVATE feathers-rack feathers-fx-sklitter feathers-warnings)
target_include_directories(test_rack_resolve PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest ${CMAKE_SOURCE_DIR}/src)
add_test(NAME rack-resolve COMMAND test_rack_resolve)

add_executable(test_stereo_mod test_stereo_mod.cpp)
target_link_libraries(test_stereo_mod PRIVATE feathers-rack feathers-fx-sklitter feathers-warnings)
target_include_directories(test_stereo_mod PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest ${CMAKE_SOURCE_DIR}/src)
add_test(NAME stereo-mod COMMAND test_stereo_mod)

# FEATHERS' FX LANE ON THE RACK ENGINE — every claim the retired FxRunner's suite made about the lane,
# asked of the lane that replaced it: adoption without allocation, parking, identity, the per-voice
# arrangement, reorder keeping each device's state, the connector, the clock, row state.
add_executable(test_lane_rack test_lane_rack.cpp)
target_link_libraries(test_lane_rack PRIVATE feathers-synth feathers-warnings)
target_include_directories(test_lane_rack PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest ${CMAKE_SOURCE_DIR}/src)
add_test(NAME lane-rack COMMAND test_lane_rack)

# THE SANITIZER GATE'S ctest ENTRIES — registered only in a sanitizer configure (-DFEATHERS_SANITIZE),
# so the ordinary `build` never sees them. `scripts/sanitize-gate.sh` is the front door and its header
# is the record; this block is the half of it that FAILS.
#
# ONE PROCESS PER CASE, via run-cases.sh --each, for two reasons the plain add_test() cannot give:
# an ASan report aborts the process, and a whole-suite run would then end at the FIRST finding and
# report a truncated tally in the same shape as a complete one (the trap --each was written for); and
# run-cases.sh cross-checks its tally against the count the binary declares, so a run cut short is a
# FAIL rather than a shorter green. The exit code is honoured AND `ERROR: AddressSanitizer` in the
# output fails the entry — a FAIL regex is additive with the exit code where a PASS regex would replace
# it, which is why there is no PASS regex here.
#
# ASAN_OPTIONS live here rather than in the script so that `ctest -L sanitize` by hand is the same
# instrument. abort_on_error: die on SIGABRT so run-cases names the signal, instead of ASan's default
# _exit(1) that prints no summary. detect_leaks=0: a different class, noise floor unmeasured.
if(FEATHERS_SANITIZE)
	set(_feathers_each ${CMAKE_SOURCE_DIR}/scripts/run-cases.sh --each)
	# RUN_CASES_JOBS=4: four case-processes at once inside each --each. Still one process per case,
	# still the binary's own count cross-checked (run-cases.sh, 2026-09-02); what changes is that the
	# widget suite's 469 s under ASan stops being the gate's floor. 4 rather than 8 because ctest
	# already runs these six entries with -j4, so the rack entries stack on top of the widget
	# entry's four -- and because at 8 a case's own 8 s bounded wait for a terrain bake starved on
	# a loaded host (run-cases.sh's header has the table: 4 was clean on every run, 8 was not).
	set(_feathers_asan_env "ASAN_OPTIONS=abort_on_error=1:halt_on_error=1:detect_leaks=0;RUN_CASES_JOBS=4")
	# synth and mod-runner joined 2026-09-02: step 4 moved the modulator lifetimes into ModRunner and
	# DeviceInstancePool (a Lane-selected destructor, two retire sites), which none of the original four
	# suites exercise -- a green gate would have been the instrument staying silent about the one thing
	# it exists for.
	# container-host, composite-chain and container-bodies joined 2026-09-10, on the same argument
	# the line above makes: step 6e put device lifetimes behind a NEW road — a container builds
	# members through DeviceInstancePool and gives them back through retire, and its destructor is
	# what returns the last of them — which none of the five above walk. A gate that did not build
	# these would be silent about the one thing this commit could get wrong.
	# rack-voices joined 2026-09-11: per-voice instances are parked and reaped beside the bus one, a
	# new lifetime on the rack's road.
	# lane-rack joined the same day: Feathers' fx lane moved onto the rack engine, so its provisions,
	# parking and reaping are the rack's lifetimes now, walked by Feathers' mirror.
	# mod-runner LEFT the same day, with ModRunner and DeviceInstancePool: the strip runs on the lane's
	# engine too, so its lifetimes are lane-rack's — which carries every claim the runner suite made.
	# converger joined 2026-09-11: the mailboxes, the hand-back list and the reap moved out of LaneRack
	# into the protocol every host uses, so they are walked bare here as well as through the lane.
	# chain-record joined with it: the record's end-to-end case drives the converger through a publish.
	# **SKLITTER'S OWN SUITES JOIN THE LIST, and they had to join it EXPLICITLY.** The reason first
	# given was that SKLITTER was not in `kDevices` -- true on 2026-09-14, NOT true now: it is in the
	# table, so the gate's other entries do reach it through the rack. The names stay for the reason
	# that outlived it, which is the stronger one: reaching a device THROUGH a host exercises whatever
	# that host happens to do with it, and these suites exercise the device.
	#
	# WHAT THEY ADD IS A LIFETIME, not more arithmetic: a four-second ring allocated at activate and
	# indexed every sample, a second ring for the delay, and a status machine that resets and
	# re-presses. `fx-sklitter` drives all of it through `processStereo`; `fx-sklitter-modes` is pure
	# and cheap, and is here because it is the one that sweeps eighty-odd thousand plans.
	#
	# **AND THE TWO CHAIN SUITES JOIN FOR THE ONE REASON THIS GATE EXISTS.** `inner-chain` builds and
	# destroys a `RackEngine` + `RackBridge` inside a device's storage, provisions members through the
	# converger and reaps them; `sklitter-chain` does it inside a device that is itself being
	# constructed, moved and destroyed. That is a nest of owning pointers whose failure mode is a read
	# of freed memory -- which does not fault, so a green `ctest` cannot see it and this can.
	#
	# **AND THE DELAY JOINS FOR SKLITTER'S OWN REASON, one device over.** It allocates a 16-second ring
	# at activate and then INDEXES IT EVERY SAMPLE from a double — `ringIndex` folds a position that is
	# negative during the first lap (the tap reaches back past the start of the recording) and
	# `readLerp` reads the frame after it, so the two places an off-by-one could live are a modulo and
	# a `+ 1`. In Granular mode up to 128 grains each carry their own drifting read head, clamped
	# against a window that moves with the write pointer. None of that faults when it is wrong by one
	# frame — a stereo ring is one `vector<float>` and a read just past the end is somebody else's
	# heap — so `ctest` cannot see this class and this can.
	#
	# **AND COMP, because the dynamics plan's Task 6 lists it**: its per-sample state (the SC biquads, two
	# mean squares, five ramps, three holds) lives in host-owned storage built by `construct` and torn
	# down by `destruct`, and the readings sink is a raw context pointer back into the host. **AND CLIP**,
	# for the same reasons plus the oversampler's eight halfband stages held in place inside that storage.
	#
	# **AND THE OS FILE DIALOG, because its whole contract is a lifetime**: a worker thread and a
	# `FileDialog` share one channel, and the dialog may be destroyed while the worker is still inside the
	# portal. A callback into the freed owner is exactly the read that does not fault.
	#
	# **AND THE STATE READER, because a patch is a file on somebody's disk** (backlog JF §3.12 case 8): its
	# mutation fuzzer feeds the reader damaged text, and an overread in a hand-written parser does not
	# fault either.
	#
	# **AND A-PH'S OPTIMISER THREAD (2026-10-18), inside `synth`**: a thread the synth owns, started at activate and joined at
	# deactivate, reading a request slot and a profile store the voices also read — test_synth's "A-PH: the optimiser's
	# thread lives activate → deactivate" posts under load and tears down five times; a use after free there does not fault.
	#
	# **AND FIVE THAT AN ASan `ctest` OF THE WHOLE TREE FOUND REPORTS IN (2026-10-03), which is the
	# argument for each in one sentence: the gate is silent about every suite it does not build.**
	# fx-shaper (a short travel array read past its end by `deriveCurve`), fx-eq (a counting allocator
	# whose nothrow form fell to ASan, freed with `free`), takeover (a parked panel reaped after its
	# counter's scope ended), and below, needing a display, rack-clap and clap-render — whose aborts
	# were not memory at all: this build strips NDEBUG, so clap-helpers TERMINATES on host misbehaviour,
	# and the tests' own host committed some (a bare `set_size` that skipped `adjust_size`, six times;
	# two deliberate probes, now compiled only where a release build is what they describe).
	# **AND THE PATTERN'S LIKES WARP (2026-10-31)**: a fit on the background worker (device/background.h) captures its device,
	# and test_pattern_device tears one down mid-fit; a use after free there does not fault either.
	# **AND GROUP-COPY**: its rigs destroy a top rack's bridge — the slot pool's owner —
	# before the engine that holds racks nested several deep with sites; a link left pointing at the freed owner reads it.
	foreach(_suite rack-engine rack-state rack-editor synth container-bodies container-state rack-nesting rack-voices lane-rack converger chain-record fx-sklitter fx-sklitter-modes fx-sklitter-ring fx-sklitter-engage inner-chain sklitter-chain fx-delay fx-pitch fx-eroder fx-disperser fx-phaser fx-comp fx-clip file-dialog state-json fx-shaper fx-eq takeover pattern-device group-copy)
		string(REPLACE "-" "_" _target "test_${_suite}")
		add_test(NAME sanitize-${_suite} COMMAND ${_feathers_each} $<TARGET_FILE:${_target}>)
		# **RUN_CASES_TIMEOUT HERE TOO, AND ITS ABSENCE WAS A HOLE IN THE GATE RATHER THAN A FINDING IN
		# ANYBODY'S BRANCH.** The widget entries below have carried a 2400 s per-case bound since
		# 2026-09-01; these did not, so every case here ran under `run-cases.sh`'s 120 s default —
		# which is sized for a known-bad hunt in an ordinary build, not for ASan at JOBS=4. MEASURED
		# 2026-09-20/21: SKLITTER's "MATRIX: every control in every mode" takes ~144 s under ASan and
		# was SIGTERMed at 120-128 s in FOUR separate agents' gate runs, each reported as a timeout,
		# none carrying an assertion or a sanitizer report — and the matrix has since grown three
		# columns (GU/GV), so at a loaded host it could not pass here by construction. A case the gate
		# cannot run is a case the gate says nothing about, and a red everybody learns to discount is
		# worse than no entry. Same bound and the same reason as the widget entries: the entry's own
		# TIMEOUT stays past the sum of its cases, and a per-case bound exists to turn a HANG into a
		# failure, not to assert anything about speed.
		set_tests_properties(sanitize-${_suite} PROPERTIES LABELS "sanitize" TIMEOUT 5400
			ENVIRONMENT "${_feathers_asan_env};RUN_CASES_TIMEOUT=2400"
			FAIL_REGULAR_EXPRESSION "ERROR: AddressSanitizer")
	endforeach()
	# THE TWO CLAP-HOSTING SUITES open an editor, so they run on a private display like the widget ones.
	foreach(_suite rack-clap clap-render)
		string(REPLACE "-" "_" _target "test_${_suite}")
		add_test(NAME sanitize-${_suite} COMMAND ${CMAKE_SOURCE_DIR}/scripts/venue-xvfb.sh run --
			${_feathers_each} $<TARGET_FILE:${_target}>)
		set_tests_properties(sanitize-${_suite} PROPERTIES LABELS "sanitize" TIMEOUT 5400
			ENVIRONMENT "${_feathers_asan_env};RUN_CASES_TIMEOUT=2400"
			FAIL_REGULAR_EXPRESSION "ERROR: AddressSanitizer")
	endforeach()
	if(TARGET test_gui_widgets_controls)
		# The widget suites need a display. venue-xvfb.sh gives each a private one plus a scratch
		# XDG_CONFIG_HOME/XDG_DATA_HOME (the suite's own main() also redirects both) and lavapipe. If Xvfb
		# or the lavapipe ICD is missing the venue exits 1 with "NOT READY", so a host that cannot run this
		# goes RED here rather than quietly skipping the suite the gate exists for — which is why this
		# entry is NOT labelled headless-incompatible.
		#
		# RUN_CASES_TIMEOUT: run-cases.sh kills a case after 120 s by default, which is right for a
		# known-bad hunt and wrong here. MEASURED 2026-09-01, ASan at -O2 under lavapipe: "the terrain is
		# one quad, whatever the map and the view disagree about" takes 4m26s and "a breakpoint at either
		# extreme is as selectable as one in the middle" 8m20s, both PASSING — the first gate run reported
		# both as crashes, which is the same shape as §8n-quater from the other side: a timeout is not a
		# verdict.
		#
		# THE 8m20s IS A CPU COST MEASURED NEAR-IDLE, NOT A WALL-CLOCK BOUND. That run used 479 s of CPU
		# in 500 s of wall, so it had nearly a whole core, with the 1/5/15-minute load averages at about
		# 6/10/14 and falling. The case is CPU-bound, so its wall time scales with contention: the same
		# suite was BUILT under three concurrent builds at load 30-60 on 16 cores, and at that load the
		# case would get a quarter to a half of a core and take 17-25 minutes. (The terrain case, timed at
		# the same mild load, already got only 60% of a core: 161 s CPU in 266 s wall.) 2400 s is that
		# loaded-host estimate with a margin; the entry's own TIMEOUT below is set past the sum of the
		# two slow cases plus the rest of the suite at the same contention, so the outer bound cannot
		# fire before the inner ones. A timeout here on a loaded host is the FIRST thing to suspect
		# before believing a crash — run the named case alone and read its CPU time against its wall.
		#
		# SIX ENTRIES SINCE THE SPLIT (2026-09-03), one per widget binary, each with the timeouts the one
		# binary had: the two slow cases named above now live in controls (the terrain) and dahdsr (the
		# breakpoint), and the other four are bounded far below them. Under `ctest -L sanitize -j4` up
		# to four of these run at once, each with RUN_CASES_JOBS=4 case processes and its own Xvfb — a
		# heavier moment than the one widget entry stacking on the rack suites was; the gate's cost line
		# is the place to re-measure it.
		foreach(_suite controls explore lane dahdsr devices mod_ring nest presets tooltips undo)
			string(REPLACE "_" "-" _suite_name "${_suite}")
			add_test(NAME sanitize-gui-widgets-${_suite_name} COMMAND ${CMAKE_SOURCE_DIR}/scripts/venue-xvfb.sh run --
				${_feathers_each} $<TARGET_FILE:test_gui_widgets_${_suite}>)
			set_tests_properties(sanitize-gui-widgets-${_suite_name} PROPERTIES LABELS "sanitize" TIMEOUT 5400
				ENVIRONMENT "${_feathers_asan_env};RUN_CASES_TIMEOUT=2400"
				FAIL_REGULAR_EXPRESSION "ERROR: AddressSanitizer")
		endforeach()
	endif()
endif()

# BUS UTILITY'S METER — K-weighting, the gated integrated measurement, correlation and the over.
# Its own binary rather than more cases in `test_fx_busutil`: the device is arithmetic on four
# coefficients and this is a standards implementation, and the two go red for entirely different
# reasons. Same split, same reason, as the OWLTT suites.
add_executable(test_fx_busutil_meter test_fx_busutil_meter.cpp)
target_link_libraries(test_fx_busutil_meter PRIVATE feathers-fx feathers-warnings)
target_include_directories(test_fx_busutil_meter PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest)
add_test(NAME fx-busutil-meter COMMAND test_fx_busutil_meter)

# THE RENDER BENCHMARK — NOT A TEST (no add_test). One patch built through the editor's road, rendered in
# blocks and timed per block; `bench_render.cpp`'s header says what it builds and prints. It links what
# test_mod_rack links, because it drives the same seam. Run it alternately against two builds and take
# medians (CLAUDE.md, benchmarking on a shared host).
add_executable(bench_render bench_render.cpp)
target_link_libraries(bench_render PRIVATE feathers-synth feathers-gui-bridge feathers-warnings)

# THE PATCH BENCHMARK — NOT A TEST (no add_test). The whole plugin through clap_entry, a patch loaded through the
# state road or built through the test-chain seam, per-block thread CPU and wall in idle/held/tail, allocations and
# mutex locks counted on the audio thread; `bench_patch.cpp`'s header says how (the perf survey, 2026-10-29).
# NOT ON WINDOWS: it counts mutex locks by interposing pthread_mutex_lock through dlsym and reads getrusage, which
# have no Windows form; it is an instrument run by hand on the Linux host, not a test.
if(NOT WIN32)
	add_executable(bench_patch bench_patch.cpp ${CMAKE_SOURCE_DIR}/src/plugin/feathers_entry.cpp)
	target_link_libraries(bench_patch PRIVATE feathers-impl feathers-warnings ${CMAKE_DL_LIBS})
endif()

# THE VOICE-STACKING BENCHMARK — NOT A TEST (no add_test). The unit costs the stack's CPU guard is built from
# (an oscillator still and moving, a table build, a vowel bake) and the budget B they imply; its header says
# how it measures and why it alternates (backlog KE, step 0).
add_executable(bench_stack bench_stack.cpp)
target_link_libraries(bench_stack PRIVATE feathers-dsp feathers-warnings)

# THE PERFORM PUSH BENCHMARK — NOT A TEST (no add_test), same posture as bench_render above. It is the
# instrument behind `perform.h`'s cost table for `performPushOf`: one `macroPushOf` against one blend,
# alternated A/B, three reps. `bench_perform_push.cpp`'s header says why its first draft was invalid and
# what this one does about it. Header-only maths, so it links only the warnings interface.
add_executable(bench_perform_push bench_perform_push.cpp)
target_link_libraries(bench_perform_push PRIVATE feathers-warnings)
target_include_directories(bench_perform_push PRIVATE ${CMAKE_SOURCE_DIR}/src)

# The likes' phase warp (P-MÖBIUS, backlog ON): the pin, the voice's road allocation-free, save/load, no likes plain.
add_executable(test_likes_warp test_likes_warp.cpp)
target_link_libraries(test_likes_warp PRIVATE feathers-impl feathers-warnings)
target_include_directories(test_likes_warp PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest ${CMAKE_SOURCE_DIR}/src)
add_test(NAME likes-warp COMMAND test_likes_warp)

# The like-set log replayed offline (plugin/ui/likes_log.h): the strength sweep by hand, its self-check under ctest.
add_executable(study_likes_replay study_likes_replay.cpp)
target_link_libraries(study_likes_replay PRIVATE feathers-impl feathers-warnings)
target_include_directories(study_likes_replay PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest ${CMAKE_SOURCE_DIR}/src)
add_test(NAME likes-replay COMMAND study_likes_replay)

# THE PRESET ROOT, FOR EVERY TEST. Since backlog FO phase 2 the library is ~/Documents/Owlet/Feathers/Presets,
# which no XDG redirect covers, and opening a device panel INSTALLS its factory presets there
# (copy-when-missing). A suite that opens a SKLITTER panel would otherwise write into the developer's real
# Documents folder. APPENDED, so an entry's own ENVIRONMENT (the sanitize ones) keeps what it set.
#
# DEFERRED TO THE END OF THIS DIRECTORY, not placed last in the file: it was once "LAST IN THIS FILE SO IT
# REACHES EVERY ENTRY ABOVE", and by 2026-10-30 three suites had been appended below it (backlog Q). A
# deferred call reaches every test wherever the next one is added.
#
# AND THE REAL editor.conf IS WATCHED ACROSS THE RUN (backlog Q, `editor_conf_watch.cmake`): every test requires
# the fixture, so the snapshot runs before them all and the compare after them all, and the compare goes red if
# the real user preferences moved. The seam in `EditorPrefs::path()` stops a test from reaching them; this
# is the instrument that would say so if something reached them by another road.
function(feathers_every_test_env)
	get_property(_feathers_all_tests DIRECTORY PROPERTY TESTS)
	set(_stamp "${CMAKE_CURRENT_BINARY_DIR}/editor-conf-watch.stamp")
	add_test(NAME editor-conf-watch-before COMMAND ${CMAKE_COMMAND} -DMODE=snapshot -DSTAMP=${_stamp}
		-P ${CMAKE_CURRENT_SOURCE_DIR}/editor_conf_watch.cmake)
	add_test(NAME editor-conf-watch-after COMMAND ${CMAKE_COMMAND} -DMODE=compare -DSTAMP=${_stamp}
		-P ${CMAKE_CURRENT_SOURCE_DIR}/editor_conf_watch.cmake)
	set_tests_properties(editor-conf-watch-before PROPERTIES FIXTURES_SETUP editor-conf-watch)
	set_tests_properties(editor-conf-watch-after PROPERTIES FIXTURES_CLEANUP editor-conf-watch)
	foreach(_feathers_t IN LISTS _feathers_all_tests)
		set_property(TEST ${_feathers_t} APPEND PROPERTY ENVIRONMENT
			"FEATHERS_PRESET_ROOT=${CMAKE_BINARY_DIR}/test-preset-root"
			"FEATHERS_IR_ROOT=${CMAKE_BINARY_DIR}/test-ir-root"
			"FEATHERS_EVENT_LOG=off"
			"FEATHERS_LIKES_LOG_DIR=off")
		set_property(TEST ${_feathers_t} APPEND PROPERTY FIXTURES_REQUIRED editor-conf-watch)
	endforeach()
endfunction()
cmake_language(DEFER CALL feathers_every_test_env)

add_executable(test_fx_spectral test_fx_spectral.cpp)
target_link_libraries(test_fx_spectral PRIVATE feathers-fx feathers-synth feathers-gui-bridge feathers-warnings) # synth, gui-bridge: persist_rig.h (Z10b)
target_include_directories(test_fx_spectral PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest ${CMAKE_SOURCE_DIR}/src)
add_test(NAME fx-spectral COMMAND test_fx_spectral)

# THE RACK (backlog NO, stage 1): parallel chains summed through level, mute and solo, then Mix and Gain — every
# claim a value, against probes whose gain and latency are their controls; latency against the Flanger; per voice
# through Feathers' lane.
add_executable(test_parallel_rack test_parallel_rack.cpp)
target_link_libraries(test_parallel_rack PRIVATE feathers-gui-bridge feathers-synth feathers-rack feathers-warnings)
target_include_directories(test_parallel_rack PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest ${CMAKE_SOURCE_DIR}/src)
add_test(NAME parallel-rack COMMAND test_parallel_rack)

# SIDECHAIN INJECT (backlog NW) — Mix 1 is the sidechain times Gain to the bit, Mix 0 is the input to the bit, a silent or
# absent sidechain is silence and says so, and the bus road's pointers die with the call.
add_executable(test_fx_inject test_fx_inject.cpp)
target_link_libraries(test_fx_inject PRIVATE feathers-fx feathers-warnings)
target_include_directories(test_fx_inject PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest)
add_test(NAME fx-inject COMMAND test_fx_inject)

# TIME BASE AS A MODULATION DESTINATION on the LFO, Step Seq, S&H and MSEG (2026-10-30, provisional), by
# value: whole indices resolved, the device running at the new division from the next tick, and no jump.
add_executable(test_mod_timebase test_mod_timebase.cpp)
target_link_libraries(test_mod_timebase PRIVATE feathers-synth feathers-warnings)
target_include_directories(test_mod_timebase PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest ${CMAKE_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME mod-timebase COMMAND test_mod_timebase)

# THE KARPLUS-STRONG STRING ALONE (S1-S2): the fundamental's tuning, the
# T60 law, exact silence, block-size invariance, the glide's handoff, the pick and the pickups, each read as a value.
add_executable(test_ks_string test_ks_string.cpp)
target_link_libraries(test_ks_string PRIVATE feathers-dsp feathers-warnings)
target_include_directories(test_ks_string PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest)
target_compile_options(test_ks_string PRIVATE ${FEATHERS_FP_EXACT})
add_test(NAME ks-string COMMAND test_ks_string)
add_executable(bench_ks_string bench_ks_string.cpp)
target_link_libraries(bench_ks_string PRIVATE feathers-dsp feathers-warnings)
# THE DRUM HIT ALONE: six kinds, by value.
add_executable(test_drum_voice test_drum_voice.cpp)
target_link_libraries(test_drum_voice PRIVATE feathers-dsp feathers-warnings)
target_include_directories(test_drum_voice PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest)
target_compile_options(test_drum_voice PRIVATE ${FEATHERS_FP_EXACT})
add_test(NAME drum-voice COMMAND test_drum_voice)

# THE KARPLUS-STRONG ENGINE (S3 on): the bank's tone and material, then the
# voice, the params and the synth, each by value.
add_executable(test_ks_engine test_ks_engine.cpp)
target_link_libraries(test_ks_engine PRIVATE feathers-synth feathers-gui-bridge feathers-warnings)
target_include_directories(test_ks_engine PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest)
target_compile_options(test_ks_engine PRIVATE ${FEATHERS_FP_EXACT})
add_test(NAME ks-engine COMMAND test_ks_engine)

# THE DELAY'S FEEDBACK FX LANE (delay-love S1): the loop with a fake lane, by
# value, and the holder with a real one — laziness, the body, latent and tailed devices inside.
# THE DELAY WITH A REAL FEEDBACK LANE, PRICED (the cost pass, 2026-11-06). NOT A TEST: ctest never runs it.
add_executable(bench_delay_lane bench_delay_lane.cpp)
target_link_libraries(bench_delay_lane PRIVATE feathers-rack feathers-fx-sklitter feathers-warnings)
target_include_directories(bench_delay_lane PRIVATE ${CMAKE_SOURCE_DIR}/src)

add_executable(test_delay_lane test_delay_lane.cpp)
target_link_libraries(test_delay_lane PRIVATE feathers-rack feathers-fx-sklitter feathers-warnings)
target_include_directories(test_delay_lane PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest ${CMAKE_SOURCE_DIR}/src)
add_test(NAME delay-lane COMMAND test_delay_lane)

# THE DELAY'S FACTORY PRESETS (delay-love S5): lane recipes, by value, under 0 dBFS, and Init empties the lane.
add_executable(test_delay_presets test_delay_presets.cpp)
target_link_libraries(test_delay_presets PRIVATE feathers-ui feathers-gui-bridge feathers-rack feathers-fx-sklitter
                      feathers-warnings)
target_include_directories(test_delay_presets PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest ${CMAKE_SOURCE_DIR}/src)
add_test(NAME delay-presets COMMAND test_delay_presets)

# THE DRUM ENGINE IN THE BANK AND THE SYNTH: pads, the kit map,
# chokes, per-pad voices, the Trigger, derived seeds, each by value.
add_executable(test_drum_engine test_drum_engine.cpp)
target_link_libraries(test_drum_engine PRIVATE feathers-synth feathers-gui-bridge feathers-warnings)
target_include_directories(test_drum_engine PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest)
target_compile_options(test_drum_engine PRIVATE ${FEATHERS_FP_EXACT})
add_test(NAME drum-engine COMMAND test_drum_engine)

# TWIN FIELD: eight outputs read off AutoMod's terrain in two crossfaded layers —
# each output by value against an independent restatement through AutoMod's full derive, the solos, per-layer isolation,
# song-position determinism, save/load through the rack, and every output routed. Links the rack for `persist_rig.h`.
add_executable(test_mod_twinfield test_mod_twinfield.cpp)
target_link_libraries(test_mod_twinfield PRIVATE feathers-rack feathers-synth feathers-gui-bridge feathers-warnings)
target_include_directories(test_mod_twinfield PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest ${CMAKE_SOURCE_DIR}/src)
add_test(NAME mod-twinfield COMMAND test_mod_twinfield)
