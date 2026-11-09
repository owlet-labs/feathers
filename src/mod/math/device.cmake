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
	ORDER 240
	DESCRIPTOR mod::math::kDescriptor
	CODEC_INCLUDE device_codecs/math.h
	CODEC_REG feathers::codecs::kMathCodec
	CARD_INCLUDE plugin/ui/cards/math.h
	CARD_REG feathers::ui::cards::kMathCard
	INCLUDES mod/math/math_types.h
	TARGET feathers-mod
	SOURCES src/mod/math/math.cpp)

# --- From the hand-written kDevices entry this manifest replaces (device_registry.h, before NY.1): ---
# **MATH — two inputs combined into one output (macro-modulation C).** `A op B`, clamped, mapped,
# scaled; A and B rest at 0 and take routes. APPENDED, and seated by both hosts as MACROS is. Its card
# is `GenericCard`: two knobs, two dropdowns, a chip, a knob — nothing the grid does not already draw.
