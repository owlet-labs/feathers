# Asserts that the built plugin asks the loader for nothing a DAW sandbox might not have.
#
# This is the check that the editor decision rests on. A plugin whose NEEDED list names a library the
# host's sandbox lacks does not fail visibly — it fails at scan time, with the host quietly skipping
# it and the musician seeing no plugin at all. Bitwig's flatpak runtime is the case in hand: it has
# GL and X11 and no webkit2gtk, which is why the editor is a statically linked GPU renderer rather
# than a webview, and why that has to stay true as dependencies come and go underneath us.
#
# Run as `cmake -DPLUGIN=<path> -P CheckRuntimeDeps.cmake`.

if(NOT DEFINED PLUGIN)
	message(FATAL_ERROR "PLUGIN is not set")
endif()

find_program(READELF_EXE NAMES readelf llvm-readelf)
if(NOT READELF_EXE)
	# Not a failure: the assertion is about the artefact, and a machine with no readelf cannot make it
	# either way. CI has one.
	message(STATUS "readelf not found — skipping the runtime dependency check")
	return()
endif()

# What a Linux DAW sandbox can be relied on to provide: the C runtime, the X11 client libraries, and
# GL. Nothing else belongs here without a reason written next to it.
#
# GL is absent from the list on purpose rather than by oversight — bgfx opens libGL with dlopen at
# runtime, so it never reaches the NEEDED list, and the plugin loads on a machine with no GL at all
# and merely fails to show an editor. That is the behaviour we want, so the entries are kept for the
# day bgfx changes its mind.
set(ALLOWED
	"ld-linux-.*\\.so.*"
	"libc\\.so.*"
	"libm\\.so.*"
	"libdl\\.so.*"
	"libpthread\\.so.*"
	"librt\\.so.*"
	# libstdc++ and libgcc_s are deliberately absent, and must stay absent. They are the exact
	# dependency this guard exists for: the root CMakeLists links them statically because a plugin
	# built with g++-14 wants CXXABI_1.3.15 while Bitwig's flatpak runtime tops out at 1.3.14, so a
	# dynamic link is a plugin the host skips at scan time and the musician never sees. Listing them
	# as allowed would let precisely that regression back in without a word.
	"libX11\\.so.*"
	"libXrandr\\.so.*"
	"libXext\\.so.*"
	"libXcursor\\.so.*"
	"libXi\\.so.*"
	"libxcb\\.so.*"
	# visage v1.0.0 (2026-11-05): fontconfig for the system-font fallback, xcb-present for the frame
	# pacing thread. BOTH MEASURED PRESENT in Bitwig's flatpak runtime on the development host:
	# libfontconfig.so.1 (1.12.1) and libxcb-present.so.0 (0.0.0).
	"libfontconfig\\.so.*"
	"libxcb-present\\.so.*"
	"libGL\\.so.*"
	"libGLX\\.so.*"
	"libGLdispatch\\.so.*"
	"libOpenGL\\.so.*"
)

execute_process(COMMAND ${READELF_EXE} -d ${PLUGIN} OUTPUT_VARIABLE DYNAMIC RESULT_VARIABLE STATUS)
if(NOT STATUS EQUAL 0)
	message(FATAL_ERROR "readelf failed on ${PLUGIN}")
endif()

string(REGEX MATCHALL "Shared library: \\[[^]]+\\]" ENTRIES "${DYNAMIC}")

set(FOUND "")
set(REJECTED "")
foreach(ENTRY IN LISTS ENTRIES)
	string(REGEX REPLACE "Shared library: \\[([^]]+)\\]" "\\1" LIB "${ENTRY}")
	list(APPEND FOUND ${LIB})

	set(OK FALSE)
	foreach(PATTERN IN LISTS ALLOWED)
		if(LIB MATCHES "^${PATTERN}$")
			set(OK TRUE)
			break()
		endif()
	endforeach()
	if(NOT OK)
		list(APPEND REJECTED ${LIB})
	endif()
endforeach()

if(REJECTED)
	string(REPLACE ";" ", " REJECTED_TEXT "${REJECTED}")
	string(REPLACE ";" ", " FOUND_TEXT "${FOUND}")
	message(FATAL_ERROR
		"${PLUGIN} depends on libraries a DAW sandbox may not have: ${REJECTED_TEXT}\n"
		"Full NEEDED list: ${FOUND_TEXT}\n"
		"Link the dependency statically, load it with dlopen, or — if it really is universal — add it "
		"to ALLOWED in cmake/CheckRuntimeDeps.cmake with a note saying why.")
endif()

string(REPLACE ";" ", " FOUND_TEXT "${FOUND}")
message(STATUS "Runtime dependencies OK: ${FOUND_TEXT}")
