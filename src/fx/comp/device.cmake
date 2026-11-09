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
	ORDER 200
	DESCRIPTOR fx::comp::kDescriptor
	CODEC_INCLUDE device_codecs/comp.h
	CODEC_REG feathers::codecs::kCompCodec
	CARD_INCLUDE plugin/ui/cards/comp.h
	CARD_REG feathers::ui::cards::kCompCard
	INCLUDES fx/comp/comp_types.h
	TARGET feathers-fx
	SOURCES src/fx/comp/comp_types.cpp
		src/fx/comp/comp.cpp)

# --- From the hand-written kDevices entry this manifest replaces (device_registry.h, before NY.1): ---
# **COMP — a compressor and a limiter in one device.** Basic compressor/limiter/clipper devices, with
# comp/limit as one multimode device. APPENDED. Fresh and textbook by design (not the reference's
# compressor, not OWLTT's band compressor), zero latency by design (Limit's attack is instant, a clamp
# holds the ceiling); `comp_types.h` opens with both.
