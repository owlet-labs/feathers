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
	ORDER 310
	DESCRIPTOR fx::noise::kDescriptor
	CODEC_INCLUDE device_codecs/noise.h
	CODEC_REG feathers::codecs::kNoiseCodec
	CARD_INCLUDE plugin/ui/cards/noise.h
	CARD_REG feathers::ui::cards::kNoiseCard
	INCLUDES fx/noise/noise_types.h
	TARGET feathers-fx
	SOURCES src/fx/noise/noise_types.cpp
		src/fx/noise/noise.cpp)

# --- From the hand-written kDevices entry this manifest replaces (device_registry.h, before NY.1): ---
# **NOISE** (backlog MZ, decided 2026-10-17). APPENDED. Adds coloured noise or random impulses through its own
# filter, leaving what passes untouched; one per voice by default, so it follows the note. Its impulses come from
# `dsp::RandomEvents`, the core NA's Random Trigger modulator will share. Its card is `NoiseCard`.
