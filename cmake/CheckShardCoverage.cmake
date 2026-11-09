# Asserts that a sharded doctest suite still runs every one of its cases, exactly once.
#
# WHY THIS EXISTS RATHER THAN A COMMENT SAYING THE SHARDS ARE FINE. Splitting a suite across ctest
# entries by index range moves a guarantee out of the binary and into the build file: coverage is now
# a property of arithmetic in feathers_add_sharded_test, and arithmetic that is wrong fails SILENTLY
# and in the SAFE-LOOKING direction. A gap between two shards does not error — it reports a green run
# for cases nobody executed, which is the same shape as the empty-filter trap scripts/run-cases.sh
# exists to catch, and CLAUDE.md's rule about it applies here word for word: a venue must be shown
# capable of expressing the outcome before its silence counts as evidence.
#
# WHY IT COMPARES NAMES AND NOT COUNTS. The obvious check — sum the assertions across the shards and
# compare with an unsharded run — does not work, and it took a measurement to find out: three
# identical unsharded runs of gui-widgets returned 356,806 / 356,986 / 356,754 assertions, because
# some cases loop against the clock. A count that moves by 232 between identical runs cannot detect a
# missing case. The case NAMES are exact, so the comparison is made on those.
#
# The ranges are passed in by the same loop that creates the shards, so there is one source of truth
# for the boundaries rather than a second copy here that could agree with the first only by luck.
#
# Run as `cmake -DBINARY=<path> -DRANGES=<first:last,first:last,...> -P CheckShardCoverage.cmake`.

if(NOT DEFINED BINARY)
	message(FATAL_ERROR "BINARY is not set")
endif()
if(NOT DEFINED RANGES)
	message(FATAL_ERROR "RANGES is not set")
endif()

# doctest prints a banner and a trailing summary around the names; strip both.
function(_list_cases OUT_VAR)
	execute_process(COMMAND ${BINARY} --order-by=name ${ARGN} --list-test-cases
		OUTPUT_VARIABLE _out RESULT_VARIABLE _rc ERROR_VARIABLE _err)
	if(NOT _rc EQUAL 0)
		message(FATAL_ERROR "${BINARY} --list-test-cases failed (${_rc}): ${_err}")
	endif()
	string(REPLACE "\n" ";" _lines "${_out}")
	set(_names "")
	foreach(_line IN LISTS _lines)
		if(_line STREQUAL "" OR _line MATCHES "^\\[doctest\\]" OR _line MATCHES "^=+$")
			continue()
		endif()
		list(APPEND _names "${_line}")
	endforeach()
	set(${OUT_VAR} "${_names}" PARENT_SCOPE)
endfunction()

_list_cases(_all)
list(LENGTH _all _all_count)
if(_all_count EQUAL 0)
	message(FATAL_ERROR "${BINARY} lists no test cases at all — the check cannot mean anything")
endif()

# Comma-separated rather than a CMake list, because add_test() splits its arguments on `;` — see the
# note at the call site.
string(REPLACE "," ";" _range_list "${RANGES}")

set(_covered "")
foreach(_range IN LISTS _range_list)
	string(REPLACE ":" ";" _pair "${_range}")
	list(GET _pair 0 _first)
	list(GET _pair 1 _last)
	_list_cases(_shard --first=${_first} --last=${_last})
	list(APPEND _covered ${_shard})
endforeach()

list(LENGTH _covered _covered_count)
set(_distinct ${_covered})
list(REMOVE_DUPLICATES _distinct)
list(LENGTH _distinct _distinct_count)

# A case counted twice is a real defect even though nothing goes unrun: it means two shards overlap,
# so the wall-clock the split was bought for is being paid twice and a failure would be reported from
# two places at once.
if(NOT _covered_count EQUAL _distinct_count)
	math(EXPR _dupes "${_covered_count} - ${_distinct_count}")
	message(FATAL_ERROR "shard ranges OVERLAP: ${_dupes} case(s) appear in more than one shard")
endif()

if(NOT _distinct_count EQUAL _all_count)
	set(_missing ${_all})
	list(REMOVE_ITEM _missing ${_covered})
	message(FATAL_ERROR
		"shard ranges DO NOT COVER the suite: ${_all_count} cases exist, the shards run ${_distinct_count}.\n"
		"Missing: ${_missing}")
endif()

message(STATUS "shard coverage: ${_all_count} cases, each run exactly once")
