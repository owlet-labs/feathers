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
	ORDER 190
	DESCRIPTOR fx::featherverb::kDescriptor
	CODEC_INCLUDE device_codecs/featherverb.h
	CODEC_REG feathers::codecs::kFeatherverbCodec
	CARD_INCLUDE plugin/ui/cards/featherverb.h
	CARD_REG feathers::ui::cards::kFeatherverbCard
	INCLUDES fx/featherverb/featherverb_types.h
	TARGET feathers-fx
	SOURCES src/fx/featherverb/featherverb_core.cpp
		src/fx/featherverb/featherverb.cpp
		src/fx/featherverb/featherverb_aurora.cpp)

# --- From the hand-written kDevices entry this manifest replaces (device_registry.h, before NY.1): ---
# **FEATHERVERB — the reference's reverb, ported first and improved later.**
# As with the shaper, it is ported first and improved after.
# APPENDED. The core is the firmware's own text, verified bit-exact against it at 44.1 kHz when it was
# ported; `featherverb_types.h` opens with the four design decisions.
