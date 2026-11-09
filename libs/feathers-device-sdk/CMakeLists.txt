# Copyright © 2026 Owlet Records
# SPDX-License-Identifier: MIT
#
# THE FEATHERS DEVICE SDK — see README.md.
#
# Every target here sees ONLY this directory's include/ and the standard library. That is the proof
# of separation: a header that reached for anything of Feathers' (GPL) would fail to compile here
# even though it compiles inside Feathers, where `src/` is on every target's path.

enable_testing()

add_library(feathers-device-sdk STATIC src/format_value.cpp src/modulatable.cpp)
target_include_directories(feathers-device-sdk PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(feathers-device-sdk PUBLIC cxx_std_20)
if(TARGET feathers-warnings)
	target_link_libraries(feathers-device-sdk PRIVATE feathers-warnings)
endif()

# A third-party device in miniature: a sine modulator that declares itself in the vocabulary and
# runs through the storage-and-function-pointer shape the host calls. Built in the normal build.
add_executable(feathers-device-sdk-example example/sine_mod_check.cpp)
target_link_libraries(feathers-device-sdk-example PRIVATE feathers-device-sdk)
if(TARGET feathers-warnings)
	target_link_libraries(feathers-device-sdk-example PRIVATE feathers-warnings)
endif()
add_test(NAME device-sdk-example COMMAND feathers-device-sdk-example)
