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
	ORDER 330
	DESCRIPTOR mod::samplehold::kDescriptor
	CODEC_INCLUDE device_codecs/samplehold.h
	CODEC_REG feathers::codecs::kSampleholdCodec
	CARD_INCLUDE plugin/ui/cards/samplehold.h
	CARD_REG feathers::ui::cards::kSampleholdCard
	INCLUDES mod/samplehold/samplehold_types.h
	TARGET feathers-mod
	SOURCES src/mod/samplehold/samplehold.cpp)

# --- From the hand-written kDevices entry this manifest replaces (device_registry.h, before NY.1): ---
# **SAMPLE & HOLD** (backlog MW, 2026-10-17). APPENDED. Built-in noise (white/pink/brown) or a routed Input,
# clocked by its Rate, each note, its Trig or Random hits (`dsp::RandomEvents`); outputs the hold and a one-tick
# Trig Out. Poly holds per note. Its card is `SampleHoldCard`, drawn around the staircase.
