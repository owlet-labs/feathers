# Copyright © 2026 Owlet Records
#
# This file is part of Feathers.
#
# Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
# General Public License as published by the Free Software Foundation,
# either version 3 of the License, or (at your option) any later version.
#
# Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
# without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
# General Public License for more details.
#
# You should have received a copy of the GNU General Public License along with Feathers. If not, see
# <https://www.gnu.org/licenses/>.

# THE REAL editor.conf, WATCHED ACROSS A ctest RUN (backlog Q). `editor-conf-watch-before` (a FIXTURES_SETUP) records
# whether the user's real ~/.config/feathers/editor.conf exists, its SHA-256 and its mtime; every test REQUIRES
# the fixture, so `editor-conf-watch-after` (its FIXTURES_CLEANUP) runs once they have all finished and goes RED if
# any of the three moved. It only READS the file. The real path is HOME's, never XDG_CONFIG_HOME's: a redirected
# desk is exactly what the tests are supposed to write.
#
#   cmake -DMODE=snapshot|compare -DSTAMP=<file> -P editor_conf_watch.cmake
#
# A RED HERE WITH NO TEST TO BLAME: a plugin open in a DAW writes this file when a preference changes, so a
# preference changed by hand during the run moves it too. Re-run with the DAW idle before calling it a test.

if(NOT DEFINED ENV{HOME} OR "$ENV{HOME}" STREQUAL "")
	message(STATUS "editor-conf-watch: no HOME, nothing real to watch")
	return()
endif()
set(_real "$ENV{HOME}/.config/feathers/editor.conf")

function(_editor_conf_state OUT)
	if(EXISTS "${_real}")
		file(SHA256 "${_real}" _sha)
		file(TIMESTAMP "${_real}" _mtime "%Y-%m-%dT%H:%M:%S" UTC)
		set(${OUT} "present ${_sha} ${_mtime}" PARENT_SCOPE)
	else()
		set(${OUT} "absent" PARENT_SCOPE)
	endif()
endfunction()

_editor_conf_state(_now)
if(MODE STREQUAL "snapshot")
	file(WRITE "${STAMP}" "${_now}")
	message(STATUS "editor-conf-watch: ${_real} is ${_now}")
elseif(MODE STREQUAL "compare")
	if(NOT EXISTS "${STAMP}")
		message(FATAL_ERROR "editor-conf-watch: no snapshot at ${STAMP} — the setup fixture did not run")
	endif()
	file(READ "${STAMP}" _before)
	if(NOT _before STREQUAL _now)
		message(FATAL_ERROR "editor-conf-watch: THE REAL ${_real} CHANGED DURING THIS ctest RUN (backlog Q)\n"
		                    "  before: ${_before}\n  after:  ${_now}\n"
		                    "A test reached the user's preferences. (Or a DAW wrote it meanwhile — see this file's header.)")
	endif()
	message(STATUS "editor-conf-watch: ${_real} unchanged (${_now})")
else()
	message(FATAL_ERROR "editor-conf-watch: MODE must be snapshot or compare")
endif()
