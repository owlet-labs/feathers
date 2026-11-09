# Turns the design mock's CSS custom properties into a C++ header.
#
# The mock is the design's source of truth — its `:root` block is labelled "swap this block to reskin
# everything" and it is where the palette is actually reasoned about, against a rendering of the real
# UI. Copying those values into C++ by hand would make a second source, and the two would drift the
# first time a colour is nudged in the browser and not here. So the header is generated, the mock
# stays the only place a token is written, and a token that changes there changes the plugin's next
# build.
#
# Written in CMake rather than a script language on purpose: it is a build step, and CI already has
# CMake. A Python dependency for thirty regex matches would be a toolchain requirement bought very
# cheaply and paid for on every machine.
#
# Run as `cmake -DMOCK=<file> -DHEADER=<file> -P GenerateTokens.cmake`.

if(NOT DEFINED MOCK OR NOT DEFINED HEADER)
	message(FATAL_ERROR "MOCK and HEADER are both required")
endif()

file(READ ${MOCK} MOCK_TEXT)

# The `:root { ... }` block, and only that one: later rules use var() references and would otherwise
# be read as definitions.
string(REGEX MATCH ":root[ \t]*[{][^}]*[}]" ROOT_BLOCK "${MOCK_TEXT}")
if(ROOT_BLOCK STREQUAL "")
	message(FATAL_ERROR "no :root block in ${MOCK} — has the mock been restructured?")
endif()

string(REGEX MATCHALL "--[a-z0-9-]+:[ \t]*[^;]+" DECLARATIONS "${ROOT_BLOCK}")

set(COLORS "")
set(LENGTHS "")
set(SKIPPED "")

foreach(DECLARATION IN LISTS DECLARATIONS)
	string(REGEX REPLACE "^--([a-z0-9-]+):[ \t]*(.*)$" "\\1" NAME "${DECLARATION}")
	string(REGEX REPLACE "^--([a-z0-9-]+):[ \t]*(.*)$" "\\2" VALUE "${DECLARATION}")
	string(STRIP "${VALUE}" VALUE)

	# kebab-case to the k-prefixed camel case the rest of the codebase uses.
	string(REPLACE "-" ";" WORDS "${NAME}")
	set(SYMBOL "k")
	foreach(WORD IN LISTS WORDS)
		string(SUBSTRING "${WORD}" 0 1 FIRST)
		string(SUBSTRING "${WORD}" 1 -1 REST)
		string(TOUPPER "${FIRST}" FIRST)
		set(SYMBOL "${SYMBOL}${FIRST}${REST}")
	endforeach()

	if(VALUE MATCHES "^#([0-9a-fA-F][0-9a-fA-F])([0-9a-fA-F][0-9a-fA-F])([0-9a-fA-F][0-9a-fA-F])$")
		# Opaque ARGB, which is what visage's Canvas::setColor takes. The mock's own transparency comes
		# from per-use rgba() and opacity rather than from the tokens, so alpha belongs at the call site.
		string(TOUPPER "${CMAKE_MATCH_1}${CMAKE_MATCH_2}${CMAKE_MATCH_3}" HEX)
		string(APPEND COLORS "constexpr uint32_t ${SYMBOL} = 0xff${HEX};\n")
	elseif(VALUE MATCHES "^([0-9]+(\\.[0-9]+)?)px$")
		# `236f` is not a float literal — the suffix needs a decimal point in front of it.
		set(NUMBER "${CMAKE_MATCH_1}")
		if(NOT NUMBER MATCHES "\\.")
			set(NUMBER "${NUMBER}.0")
		endif()
		string(APPEND LENGTHS "constexpr float ${SYMBOL} = ${NUMBER}f;\n")
	else()
		# Font stacks and anything else that has no C++ meaning. Recorded in the header as a comment so
		# the omission is visible rather than silent.
		string(APPEND SKIPPED "//   --${NAME}: ${VALUE}\n")
	endif()
endforeach()

if(COLORS STREQUAL "")
	message(FATAL_ERROR "no colour tokens found in ${MOCK}'s :root block")
endif()

set(CONTENT "// Generated from ${MOCK} by cmake/GenerateTokens.cmake. DO NOT EDIT.\n")
string(APPEND CONTENT "//\n")
string(APPEND CONTENT "// Change a colour in the mock's :root block and rebuild; that block is the\n")
string(APPEND CONTENT "// single source, and this file is only ever its shadow.\n\n")
string(APPEND CONTENT "#pragma once\n\n#include <cstdint>\n\nnamespace feathers::tokens {\n\n")
string(APPEND CONTENT "// Opaque ARGB, ready for visage's Canvas::setColor.\n${COLORS}\n")
if(NOT LENGTHS STREQUAL "")
	string(APPEND CONTENT "// Logical pixels, as the mock's layout contract states them.\n${LENGTHS}\n")
endif()
if(NOT SKIPPED STREQUAL "")
	string(APPEND CONTENT "// Not translated — no C++ meaning:\n${SKIPPED}\n")
endif()
string(APPEND CONTENT "} // namespace feathers::tokens\n")

# Only rewrite on a real change, so a build that touches nothing does not recompile everything that
# includes this.
if(EXISTS ${HEADER})
	file(READ ${HEADER} EXISTING)
	if(EXISTING STREQUAL CONTENT)
		return()
	endif()
endif()
file(WRITE ${HEADER} "${CONTENT}")
