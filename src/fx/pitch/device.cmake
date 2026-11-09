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
	ORDER 170
	DESCRIPTOR fx::pitch::kDescriptor
	CODEC_INCLUDE device_codecs/pitch.h
	CODEC_REG feathers::codecs::kPitchCodec
	INCLUDES fx/pitch/pitch_types.h
	TARGET feathers-fx
	SOURCES src/fx/pitch/pitch.cpp)

# --- From the hand-written kDevices entry this manifest replaces (device_registry.h, before NY.1): ---
# **THE PITCH SHIFTER — the second of the table-stakes effects**, with an option to lock to a
# scale (the same options as SKLITTER), and Free.
#
# APPENDED, for the reason above. GRANULAR, with formant control deliberately a second pass —
# `pitch_types.h` opens with why, and with the two roads that pass could take. The Scale menu is
# BUILT from SKLITTER's name table rather than retyped beside it; the scale CONTENTS are full
# degrees rather than SKLITTER's weighted picks, by decision.
