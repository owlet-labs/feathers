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

feathers_register_device(
	ORDER 120
	DESCRIPTOR mod::noteexpr::kDescriptor
	CODEC_INCLUDE device_codecs/noteexpr.h
	CODEC_REG feathers::codecs::kNoteexprCodec
	INCLUDES mod/noteexpr/noteexpr_types.h
		plugin/noteexpr_clap_check.h
	TARGET feathers-mod
	SOURCES src/mod/noteexpr/noteexpr.cpp)

# --- From the hand-written kDevices entry this manifest replaces (device_registry.h, before NY.1): ---
# BUS UTILITY — gain, pan, dual pan, width and polarity on the summed bus.
#
# APPENDED AT THE END, WHICH THE NOTE ABOVE PAID FOR: this list's ORDER IS OBSERVABLE, the
# browser lists in it, and `test_gui_widgets` picks `offered[1]` positionally. Registering
# anywhere but the end shifts every index after it.
#
# NO CARD FACTORY ENTRY, DELIBERATELY, AND THE REACHABILITY GUARD IS SATISFIED ANYWAY.
# `makeCustomCard` returns null for an unknown tag and the lane falls back to `GenericCard`,
# which draws all eight of this device's controls — so every declared control has something on a
# surface that reaches it. That fallback is what `drwn` could NOT use (103 controls made a
# 2842-pixel card whose enable square was unreachable); eight is the case the generic grid was
# written for. A hand-written card is what a level-and-correlation METER would need, and that is
# deferred to its own change rather than smuggled into this one.
# NOTE EXPRESSIONS — the seven per-note controllers CLAP defines, made routable and mapped to
# nothing.
#
# APPENDED AT THE END, for the reason two entries above already paid for: this list's ORDER IS
# OBSERVABLE and `test_gui_widgets` picks positionally.
#
# **THE CARD OBJECTION THAT HELD THE TRACKER OUT IS GONE**, and the paragraph above it is stale
# about that: `ModPanel::CardKind` gained `Generic`, chosen structurally by what the strip can
# BUILD rather than from a list of devices missing cards. This device declares two controls — a
# Rule and an Enabled — which is squarely what the generic grid was written for.
#
# **AND IT IS THE FIRST REGISTERED DEVICE DECLARING `NoteOps`**, which closes a coverage gap the
# registry itself records: *"with no registered device declaring NoteOps, neither this host's note
# lane nor the rack's has END-TO-END coverage through a real chain"*, and
# `tests/test_rack_engine.cpp` skips its lane case saying so. The tracker was registered later and
# declares `NoteOps` too; this is the second, and the first that also publishes seven outputs.
