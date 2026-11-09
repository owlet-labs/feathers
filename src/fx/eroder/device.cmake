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
	ORDER 180
	DESCRIPTOR fx::eroder::kDescriptor
	CODEC_INCLUDE device_codecs/eroder.h
	CODEC_REG feathers::codecs::kEroderCodec
	INCLUDES fx/eroder/eroder_types.h
	TARGET feathers-fx
	SOURCES src/fx/eroder/eroder.cpp)

# --- From the hand-written kDevices entry this manifest replaces (device_registry.h, before NY.1): ---
# **THE ERODER — the reference's, ported as faithfully as float and a variable sample rate allow.**
# As faithful as possible, with bus pitch tracking. APPENDED. `eroder_types.h` opens with the
# reference's commit and with the
# short list of what is deliberately NOT the firmware's, each with its reason. On the bus, tuned
# by `setNoteContext` — the firmware's own arrangement, `getLastNoteCode()`.
