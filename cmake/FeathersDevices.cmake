# Copyright © 2026 Owlet Records
#
# This file is part of Feathers.
#
# Feathers is free software: you can redistribute it and/or modify it under the
# terms of the GNU General Public License as published by the Free Software Foundation,
# either version 3 of the License, or (at your option) any later version.
#
# This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
# without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
# See the GNU General Public License for more details.

# DEVICE SELF-REGISTRATION (backlog NY.1).
#
# A device registers itself from its own directory, so adding one edits NO shared list:
#
#     src/<fx|mod>/<name>/device.cmake            (or src/rack/<name>.device.cmake)
#
#     feathers_register_device(
#         ORDER      130                           # position in kDevices; ties break by DESCRIPTOR text
#         DESCRIPTOR fx::filter::kDescriptor       # the constexpr device::Descriptor
#         INCLUDES   fx/filter/filter_types.h      # what that expression needs, in order
#         TARGET     feathers-fx                   # the library that owns SOURCES (optional)
#         SOURCES    src/fx/filter/multimode_filter.cpp ...)
#
# C++ cannot register constexpr data across translation units (static-init order, and nothing is
# constexpr), so the assembly happens HERE, at configure time, into the build directory:
#
#     device_includes.inc   the INCLUDES of every device, in registry order
#     device_list.inc       the initialiser of `kDevices[]`, in registry order
#     card_includes.inc, card_table.inc   the editor's card factory table (CARD_INCLUDE / CARD_REG)
#
# `src/plugin/device_registry.h` includes both. The text is exactly what used to be written there by
# hand. ORDER is explicit because order is load-bearing (the browser, anything index-like); a tie is
# broken by the descriptor's own text, so two branches that pick the same number do not conflict
# textually and the result is still deterministic. tests/test_device_browser.cpp pins the sequence.

set(FEATHERS_GENERATED_DIR "${CMAKE_BINARY_DIR}/generated" CACHE INTERNAL "")
set_property(GLOBAL PROPERTY FEATHERS_DEVICE_ENTRIES "")
set_property(GLOBAL PROPERTY FEATHERS_DEVICE_CARDS "")
set_property(GLOBAL PROPERTY FEATHERS_DEVICE_CODECS "")

function(feathers_register_device)
	cmake_parse_arguments(D "" "ORDER;DESCRIPTOR;TARGET;CARD_INCLUDE;CARD_REG;CODEC_INCLUDE;CODEC_REG" "INCLUDES;SOURCES" ${ARGN})
	if(NOT D_ORDER MATCHES "^[0-9]+$")
		message(FATAL_ERROR "feathers_register_device: ORDER must be a non-negative integer, got '${D_ORDER}'")
	endif()
	if(NOT D_DESCRIPTOR OR NOT D_INCLUDES)
		message(FATAL_ERROR "feathers_register_device: DESCRIPTOR and INCLUDES are required (ORDER ${D_ORDER})")
	endif()
	if(D_SOURCES AND NOT D_TARGET)
		message(FATAL_ERROR "feathers_register_device: SOURCES needs a TARGET (${D_DESCRIPTOR})")
	endif()
	get_property(entries GLOBAL PROPERTY FEATHERS_DEVICE_ENTRIES)
	foreach(e IN LISTS entries)
		string(REGEX MATCH "^[0-9]+\\|([^|]*)\\|" _m "${e}")
		if(CMAKE_MATCH_1 STREQUAL D_DESCRIPTOR)
			message(FATAL_ERROR "feathers_register_device: ${D_DESCRIPTOR} registered twice")
		endif()
	endforeach()
	string(LENGTH "${D_ORDER}" len)
	math(EXPR pad "8 - ${len}")
	string(REPEAT "0" ${pad} zeros)
	string(REPLACE ";" "," inc "${D_INCLUDES}")
	set_property(GLOBAL APPEND PROPERTY FEATHERS_DEVICE_ENTRIES "${zeros}${D_ORDER}|${D_DESCRIPTOR}|${inc}")
	if((D_CARD_INCLUDE AND NOT D_CARD_REG) OR (D_CARD_REG AND NOT D_CARD_INCLUDE))
		message(FATAL_ERROR "feathers_register_device: CARD_INCLUDE and CARD_REG go together (${D_DESCRIPTOR})")
	endif()
	if(D_CARD_REG)
		set_property(GLOBAL APPEND PROPERTY FEATHERS_DEVICE_CARDS "${zeros}${D_ORDER}|${D_DESCRIPTOR}|${D_CARD_INCLUDE}|${D_CARD_REG}")
	endif()
	if((D_CODEC_INCLUDE AND NOT D_CODEC_REG) OR (D_CODEC_REG AND NOT D_CODEC_INCLUDE))
		message(FATAL_ERROR "feathers_register_device: CODEC_INCLUDE and CODEC_REG go together (${D_DESCRIPTOR})")
	endif()
	if(D_CODEC_REG)
		set_property(GLOBAL APPEND PROPERTY FEATHERS_DEVICE_CODECS "${zeros}${D_ORDER}|${D_DESCRIPTOR}|${D_CODEC_INCLUDE}|${D_CODEC_REG}")
	endif()
	if(D_TARGET)
		# Absolute from the source root, so a manifest names a path the way the build files always did.
		foreach(s IN LISTS D_SOURCES)
			set_property(GLOBAL APPEND PROPERTY FEATHERS_DEVICE_SOURCES_${D_TARGET} "${CMAKE_CURRENT_SOURCE_DIR}/${s}")
		endforeach()
	endif()
endfunction()

# The sources devices registered for TARGET, to be spliced into its add_library().
function(feathers_device_sources out target)
	get_property(s GLOBAL PROPERTY FEATHERS_DEVICE_SOURCES_${target})
	set(${out} ${s} PARENT_SCOPE)
endfunction()

file(GLOB _feathers_manifests CONFIGURE_DEPENDS
	"${CMAKE_CURRENT_SOURCE_DIR}/src/fx/*/device.cmake"
	"${CMAKE_CURRENT_SOURCE_DIR}/src/mod/*/device.cmake"
	"${CMAKE_CURRENT_SOURCE_DIR}/src/rack/*.device.cmake")
list(SORT _feathers_manifests)
foreach(_m IN LISTS _feathers_manifests)
	include("${_m}")
endforeach()

get_property(_entries GLOBAL PROPERTY FEATHERS_DEVICE_ENTRIES)
list(SORT _entries COMPARE STRING)
set(_includes "")
set(_list "")
foreach(_e IN LISTS _entries)
	string(REPLACE "|" ";" _parts "${_e}")
	list(GET _parts 1 _desc)
	list(GET _parts 2 _inc)
	string(REPLACE "," ";" _inc "${_inc}")
	foreach(_i IN LISTS _inc)
		string(APPEND _includes "#include \"${_i}\"\n")
	endforeach()
	string(APPEND _list "\t${_desc},\n")
endforeach()
set(_banner "// GENERATED by cmake/FeathersDevices.cmake from every device.cmake — do not edit.\n// A device registers itself; see cmake/FeathersDevices.cmake.\n")
file(CONFIGURE OUTPUT "${FEATHERS_GENERATED_DIR}/device_includes.inc"
	CONTENT "${_banner}${_includes}" @ONLY)
file(CONFIGURE OUTPUT "${FEATHERS_GENERATED_DIR}/device_list.inc"
	CONTENT "${_banner}${_list}" @ONLY)

# The card table (backlog NY.1 step 3): the editor's card factory reads one `CardReg` per device that has a
# card of its own, in registry order. Each is declared in the device's own header (src/plugin/ui/cards/) and
# named by CARD_INCLUDE / CARD_REG in the device's manifest.
get_property(_cards GLOBAL PROPERTY FEATHERS_DEVICE_CARDS)
list(SORT _cards COMPARE STRING)
set(_card_includes "")
set(_card_table "")
foreach(_c IN LISTS _cards)
	string(REPLACE "|" ";" _cp "${_c}")
	list(GET _cp 2 _ci)
	list(GET _cp 3 _cr)
	string(APPEND _card_includes "#include \"${_ci}\"\n")
	string(APPEND _card_table "\t${_cr},\n")
endforeach()
file(CONFIGURE OUTPUT "${FEATHERS_GENERATED_DIR}/card_includes.inc"
	CONTENT "${_banner}${_card_includes}" @ONLY)
file(CONFIGURE OUTPUT "${FEATHERS_GENERATED_DIR}/card_table.inc"
	CONTENT "${_banner}${_card_table}" @ONLY)

# The row-codec table tests/test_device_ranges.cpp walks: one `codecs::Codec` per device, each declared in
# tests/device_codecs/<device>.h and named by CODEC_INCLUDE / CODEC_REG in the manifest.
get_property(_codecs GLOBAL PROPERTY FEATHERS_DEVICE_CODECS)
list(SORT _codecs COMPARE STRING)
set(_codec_includes "")
set(_codec_table "")
foreach(_c IN LISTS _codecs)
	string(REPLACE "|" ";" _cp "${_c}")
	list(GET _cp 2 _ci)
	list(GET _cp 3 _cr)
	string(APPEND _codec_includes "#include \"${_ci}\"\n")
	string(APPEND _codec_table "\t${_cr},\n")
endforeach()
file(CONFIGURE OUTPUT "${FEATHERS_GENERATED_DIR}/codec_includes.inc"
	CONTENT "${_banner}${_codec_includes}" @ONLY)
file(CONFIGURE OUTPUT "${FEATHERS_GENERATED_DIR}/codec_table.inc"
	CONTENT "${_banner}${_codec_table}" @ONLY)
include_directories(BEFORE "${FEATHERS_GENERATED_DIR}")
