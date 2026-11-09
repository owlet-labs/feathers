# Asserts that no rack UI translation unit references `RackEngine`'s drain-only mutators.
#
# **BACKLOG BW's CONDITION (1), CLOSED BY MEASUREMENT RATHER THAN BY THE COMPILER.** The rule is that
# only `RackBridge::drain` — and `applyRack`, which the drain calls — may write engine state; a direct
# call races a block, and TSan has named both families it produces (BS/BV). After CD commit 4 no
# production code outside those two writes engine state, and this is what keeps that true.
#
# **WHY NOT THE COMPILER.** Making the mutators private was costed and declined: 114 call sites, every
# one of them in tests, none in `src/`. The guarantee constrains production code, production code is
# already clean, and the price would have fallen entirely on 44 engine cases whose job is to drive the
# bare engine single-threaded — which the drain-only rule does not forbid and never did.
#
# **WHY NOT A LINK LINE, WHICH IS THE STRONGER TOOL AND WAS TRIED FIRST.** `4e15040` made Feathers'
# threading model a link-time fact by dropping `feathers-ui`'s link on `feathers-synth`. That works on
# three conditions and here all three fail: the link is NOT vestigial (`rack_clap.cpp.o` legitimately
# references thirteen `RackEngine` symbols — constructor, `prepare`, `processStereo`, `setBlockNotes`
# and the readers — because `RackPlugin` owns the engine and runs the block); no cut separates the
# violator from the legitimate user, since a static archive links at OBJECT granularity and the
# mutators share a translation unit with everything else the plugin needs; and there is no enforcer
# that exists for its own reasons, because nothing links `feathers-rack-ui` without `feathers-rack`.
# An enforcer invented for the purpose is weaker evidence than one that was already there.
#
# **SO THIS READS WHAT THE COMPILER EMITTED, WHICH IS STRICTLY STRONGER THAN READING THE SOURCE.** A
# grep over `src/` answers "does anyone write this call"; `nm` answers "did the compiler emit a
# reference to it", which is the question. There is no LTO in this configuration, so a cross-TU call
# cannot vanish into an inline.
#
# TWO LIMITS, AND THEY BELONG HERE RATHER THAN IN A COMMIT MESSAGE:
#   * **It GATES rather than REFUSES.** The build succeeds and this test fails. Weaker than a link
#     error, stronger than a grep.
#   * **It covers a NAMED OBJECT LIST.** A translation unit added to `feathers-rack-ui` later is
#     outside this check until somebody names it in `tests/CMakeLists.txt`.
#
# **SO WHAT BW's CONDITION (1) ACTUALLY CLOSES AS, and this is the sentence to quote rather than
# "closed by grep": no production caller, verified at object level by `nm` rather than by grep, with
# the 114-site compile-time option costed and declined.**
#
# Run as `cmake -DENGINE_OBJ=<path> -DGUARDED_OBJS=<a|b|...> -P CheckRackWriteBoundary.cmake`.

if(NOT DEFINED ENGINE_OBJ OR NOT DEFINED GUARDED_OBJS)
	message(FATAL_ERROR "ENGINE_OBJ and GUARDED_OBJS must both be set")
endif()

# **THE MUTATORS, BY THEIR DEMANGLED SIGNATURES.** Demangled rather than mangled so a reader can see
# what is being guarded; the vacuity control below is what makes a fuzzy match safe, because a
# signature that stopped matching would fail to be FOUND rather than fail to be absent.
set(MUTATORS
	"RackEngine::addDevice("
	"RackEngine::addDeviceAt("
	"RackEngine::removeDeviceAtLane("
	"RackEngine::moveDevice("
	"RackEngine::setMacro("
	"RackEngine::setField("
	"RackEngine::setGate("
	"RackEngine::setRouteAmount("
)

find_program(NM_EXE NAMES nm llvm-nm)
if(NOT NM_EXE)
	# **A HARD FAILURE, AND DELIBERATELY UNLIKE `CheckRuntimeDeps.cmake`, WHICH SKIPS.** That check
	# asserts a property of a shipped artefact and a machine without readelf genuinely cannot answer
	# it either way. This one asserts an ABSENCE, and an absence check that skips is an absence check
	# that passes — which is the exact failure this whole file is shaped around. A red saying "no nm"
	# is honest and takes a minute to fix; a green saying nothing is not.
	message(FATAL_ERROR "nm not found — this check cannot be made, and a skip would read as a pass")
endif()

string(REPLACE "|" ";" GUARDED_LIST "${GUARDED_OBJS}")

#------------------------------------------------------------------------------------------------#
# THE VACUITY CONTROL, FIRST AND INSIDE THE CHECK                                                  #
#------------------------------------------------------------------------------------------------#
#
# **"NONE OF THESE EIGHT SYMBOLS APPEARS" IS ALSO TRUE OF AN OBJECT THAT DOES NOT EXIST, AN OBJECT
# LIST A BUILD-LAYOUT CHANGE SILENTLY EMPTIED, AND A SYMBOL WHOSE SPELLING DRIFTED.** Every one of
# those makes this check green forever — and green is also the answer it gives when everything is
# fine, so the failure would be invisible in exactly the direction that matters. So before believing
# any absence: every named object must EXIST, and all eight mutators must be FOUND, defined, in the
# engine's own object. A missing object or a renamed symbol then reds this check instead of
# satisfying it.
#
# This is the repo's oldest failure family — an instrument that cannot express the outcome it is
# asked about. It was run by hand before the absence below was believed the first time; doing it here
# is what stops the next person having to remember.

if(NOT EXISTS "${ENGINE_OBJ}")
	message(FATAL_ERROR
		"the engine object is missing: ${ENGINE_OBJ}\n"
		"  This check cannot be made without it, and its absence must not read as a clean result.")
endif()

execute_process(COMMAND "${NM_EXE}" -C --defined-only "${ENGINE_OBJ}"
	OUTPUT_VARIABLE ENGINE_SYMS RESULT_VARIABLE ENGINE_RC ERROR_VARIABLE ENGINE_ERR)
if(NOT ENGINE_RC EQUAL 0)
	message(FATAL_ERROR "nm failed on ${ENGINE_OBJ}: ${ENGINE_ERR}")
endif()

set(MISSING "")
foreach(sym IN LISTS MUTATORS)
	string(FIND "${ENGINE_SYMS}" "${sym}" at)
	if(at EQUAL -1)
		list(APPEND MISSING "${sym}")
	endif()
endforeach()
if(MISSING)
	string(REPLACE ";" "\n    " MISSING_TEXT "${MISSING}")
	message(FATAL_ERROR
		"VACUITY: these mutators are not DEFINED in ${ENGINE_OBJ}:\n    ${MISSING_TEXT}\n"
		"  Either they were renamed, or their signatures changed, or this object is not the engine's.\n"
		"  Until the list matches the code, an absence in the guarded objects below proves nothing —\n"
		"  which is why this fails rather than reporting a boundary it cannot actually check.")
endif()

#------------------------------------------------------------------------------------------------#
# AND ONLY NOW, THE ABSENCE                                                                        #
#------------------------------------------------------------------------------------------------#

set(OFFENCES "")
foreach(obj IN LISTS GUARDED_LIST)
	if(NOT EXISTS "${obj}")
		message(FATAL_ERROR
			"a guarded object is missing: ${obj}\n"
			"  A build-layout change can empty this list silently; an empty list passes this check for\n"
			"  the wrong reason, so a missing object is a failure rather than one fewer thing to read.")
	endif()
	execute_process(COMMAND "${NM_EXE}" -C -u "${obj}"
		OUTPUT_VARIABLE OBJ_SYMS RESULT_VARIABLE OBJ_RC ERROR_VARIABLE OBJ_ERR)
	if(NOT OBJ_RC EQUAL 0)
		message(FATAL_ERROR "nm failed on ${obj}: ${OBJ_ERR}")
	endif()
	foreach(sym IN LISTS MUTATORS)
		string(FIND "${OBJ_SYMS}" "${sym}" at)
		if(NOT at EQUAL -1)
			get_filename_component(base "${obj}" NAME)
			list(APPEND OFFENCES "${base} references ${sym}...)")
		endif()
	endforeach()
endforeach()

if(OFFENCES)
	string(REPLACE ";" "\n    " OFFENCE_TEXT "${OFFENCES}")
	message(FATAL_ERROR
		"the rack's UI writes engine state directly:\n    ${OFFENCE_TEXT}\n"
		"  Only RackBridge::drain and applyRack may call these — a direct call races a block\n"
		"  (backlog BW/CD; the banner at each mutator in rack_engine.h says the same).\n"
		"  Route the write through RackBridge instead, as the CLAP event walk does.")
endif()

list(LENGTH GUARDED_LIST GUARDED_COUNT)
message(STATUS
	"rack write boundary: 8 mutators defined in the engine, 0 referenced across ${GUARDED_COUNT} UI objects")
