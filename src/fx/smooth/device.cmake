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
	ORDER 250
	DESCRIPTOR fx::smooth::kDescriptor
	CODEC_INCLUDE device_codecs/smooth.h
	CODEC_REG feathers::codecs::kSmoothCodec
	CARD_INCLUDE plugin/ui/cards/smooth.h
	CARD_REG feathers::ui::cards::kSmoothCard
	INCLUDES fx/smooth/smooth_types.h
	TARGET feathers-fx
	SOURCES src/fx/smooth/smooth_types.cpp
		src/fx/smooth/smooth.cpp)

# --- From the hand-written kDevices entry this manifest replaces (device_registry.h, before NY.1): ---
# **THE SMOOTH SHAPER — the reference's sine shaper, ported first and improved later.**
# Ported with the gain-staging differences between the hardware and a DAW kept in mind, and named Smooth
# Shaper in the plugin. APPENDED. Per voice by default, the reference's place; verified bit-exact
# against the firmware at 44.1 kHz when it was ported. `smooth_types.h` opens
# with the gain staging, 1.0 = the reference's 0 dBFS, and the departures, each with its reason.
