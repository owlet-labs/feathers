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
	ORDER 320
	DESCRIPTOR mod::rtrig::kDescriptor
	CODEC_INCLUDE device_codecs/rtrig.h
	CODEC_REG feathers::codecs::kRtrigCodec
	CARD_INCLUDE plugin/ui/cards/rtrig.h
	CARD_REG feathers::ui::cards::kRtrigCard
	INCLUDES mod/rtrig/rtrig_types.h
	TARGET feathers-mod
	SOURCES src/mod/rtrig/rtrig.cpp)

# --- From the hand-written kDevices entry this manifest replaces (device_registry.h, before NY.1): ---
# **RANDOM TRIGGER** (backlog NA (a), decided 2026-10-17). APPENDED. NOISE's impulses as modulation: the same
# `dsp::RandomEvents` core and seed, published as Trig, Size and Decay; global or Poly. Its card is `RandomTriggerCard`.
