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
	ORDER 260
	DESCRIPTOR fx::eq::kDescriptor
	CODEC_INCLUDE device_codecs/eq.h
	CODEC_REG feathers::codecs::kEqCodec
	CARD_INCLUDE plugin/ui/cards/eq.h
	CARD_REG feathers::ui::cards::kEqCard
	INCLUDES fx/eq/eq_types.h
	TARGET feathers-fx
	SOURCES src/fx/eq/eq_types.cpp
		src/fx/eq/eq.cpp
		src/fx/eq/eq_linear.cpp)

# --- From the hand-written kDevices entry this manifest replaces (device_registry.h, before NY.1): ---
# **EQ — the surgical / mastering parametric EQ.** Drawn is a creative drawn-curve
# filter, not a mixing EQ, so a separate device. APPENDED. Eight bands, Offset, Amount, Tilt and an
# output trim, minimum phase.
# On `GenericCard` until its own card lands (step 3).
