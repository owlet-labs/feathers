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
	ORDER 410
	DESCRIPTOR fx::drumdevice::kDescriptor
	CODEC_INCLUDE device_codecs/drum.h
	CODEC_REG feathers::codecs::kDrumCodec
	INCLUDES fx/drum/drum_types.h
	TARGET feathers-fx
	SOURCES src/fx/drum/drum_types.cpp
		src/fx/drum/drum.cpp)

# **DRUM — one drum, a generator in a chain.** It answers one key, strikes one hit of one kind (the drum engine's own hit) and adds
# it to what comes in; every note passes through, so drums on one key layer. Its card is the generic one, with the controls a kind does
# not read gated away. APPENDED: the device list's order is observable, so a new device goes at the end.
