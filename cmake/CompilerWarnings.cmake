# Warning set for feathers-OWNED targets only.
#
# Deliberately not linked into feathers-character: those translation units are firmware sources
# compiled unmodified, and their warnings are upstream's call, not ours. We suppress nothing
# there — we just decline to hold their code to our -Werror.

# STRICT BY DEFAULT, and the option exists only to answer ONE question cheaply: "what ELSE is wrong?"
#
# -Werror makes the compiler stop at the first problem, which is right for a build and wrong for a
# survey. Finding macOS-only warnings that way cost FIVE CI runs at ~10x Linux runner cost, one
# warning per run, because each fix simply revealed the next. With this OFF the build completes and
# reports every warning at once, so a single run yields the whole list.
#
# DO NOT SHIP IT OFF, and do not set it in a local workflow. and
# the reason is written below: a real -Wdangling-pointer sat unread inside a wall of 64 warnings for
# a day. This exists to be switched on for one diagnostic run and switched off again.
option(FEATHERS_WARNINGS_AS_ERRORS "Treat our own warnings as errors (see the comment above)" ON)

add_library(feathers-warnings INTERFACE)
if(MSVC)
	# /W4 is MSVC's equivalent of -Wall -Wextra; C4715 (not all paths return a value) is the
	# -Werror=return-type counterpart and is the one warning here worth failing a build over.
	#
	# /W4 IS WIDER THAN -Wall -Wextra, and the extra classes below are switched off one by one so MSVC holds this code
	# to the SAME contract GCC and Clang do, no stricter. Each is a class that -Wall -Wextra deliberately leave out
	# (GCC's -Wconversion, -Wsign-conversion, -Wshadow), or that cannot be acted on portably. Enabling them on one
	# platform only would make every Linux developer's green a Windows red they cannot see coming.
	#   C4244 C4267 C4305  implicit narrowing (double->float, size_t->int): GCC's -Wconversion, off there.
	#   C4245              signed/unsigned conversion on initialisation: GCC's -Wsign-conversion, off there.
	#   C4456-C4459        a declaration hides another: GCC's -Wshadow, off there.
	#   C4127              conditional expression is constant: fires on `if (kFlag)` and template constants.
	#   C4324              structure padded because of alignas: that padding is the point of the alignas.
	#   C4068              unknown pragma: `#pragma GCC diagnostic` lines aimed at GCC, which MSVC rightly ignores.
	#   C5030              unknown attribute: `[[gnu::always_inline]]`/`[[gnu::noinline]]`, GCC's hints.
	#   C4702              unreachable code: reported after inlining in optimised builds, not about the source.
	#   C4125              "decimal digit terminates octal escape": the standard ends an octal escape at three digits, and
	#                      UTF-8 names spelled as octal bytes ("\342\231\2574") rely on exactly that.
	target_compile_options(feathers-warnings INTERFACE /W4
		/wd4244 /wd4267 /wd4305 /wd4245 /wd4456 /wd4457 /wd4458 /wd4459 /wd4127 /wd4324 /wd4068 /wd5030 /wd4702 /wd4125)
	if(FEATHERS_WARNINGS_AS_ERRORS)
		target_compile_options(feathers-warnings INTERFACE /WX)
	endif()
else()
	target_compile_options(feathers-warnings INTERFACE
		-Wall
		-Wextra
		# A NAMESPACE-SCOPE `constexpr` NOBODY READS. clang folds this into -Wall for C++; GCC does NOT
		# (its -Wunused-variable covers C only), which is how twelve dead constants — eight identical
		# card corner radii and four output-row gaps superseded by SourceArm's own layout — passed
		# every g++-14 -Werror build here and stopped the first NORMAL macOS build (run 6, 2026-09-07,
		# backlog R) at owltt_card.cpp:38. Level 1 is the level that names definitions in the main
		# file and not in headers, which is the class that hides. MEASURED before the deletions: with
		# this line, g++-14 reports the same twelve as -Werror. The preflight now sees what Apple
		# clang sees.
		$<$<CXX_COMPILER_ID:GNU>:-Wunused-const-variable=1>
	)
	if(FEATHERS_WARNINGS_AS_ERRORS)
		target_compile_options(feathers-warnings INTERFACE -Werror)
	endif()
endif()

# WHY THE WHOLE SET IS AN ERROR AND NOT JUST return-type, and why locally as well as in CI.
#
# The strict form (2026-08-23) is chosen over the CI-only one: stay strict.
#
# THE ARGUMENT IS NOT TIDINESS, IT IS ONE MEASURED INCIDENT. On the morning of 2026-08-23 the tree
# carried 64 warnings. 53 were false positives from the two suites that replace the global allocator
# to count allocations — GCC misreading a correct replacement pair. Sitting inside that noise floor
# was a REAL -Wdangling-pointer in env_card.cpp: a reference bound through std::array::operator[] on
# a by-value ModRack, dangling for the rest of draw(), read for every plotted point — in the fix for
# the bipolar display bug that had been reported by name. It had been printing for a day and nobody read
# it, because a wall of warnings is not a signal.
#
# Zero was reached by four sessions clearing warnings across one evening. A person who remembers is
# not a mechanism; this is the mechanism. The cost is real and accepted: the next new warning stops
# whoever hits it, mid-edit, at a time they did not choose.
#
# SCOPE IS UNCHANGED AND IS THE WHOLE REASON THIS IS SAFE. feathers-warnings is linked PRIVATE into
# feathers' own targets only. The former firmware tree is gone (the phi layer is ours now, in
# src/character/, and compiles clean under this set); the sst libraries, visage, clap and clap-wrapper
# come in as SYSTEM includes. We hold our own code to this and nobody else's.
