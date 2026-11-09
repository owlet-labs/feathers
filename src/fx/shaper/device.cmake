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
	ORDER 70
	DESCRIPTOR fx::shaper::kDescriptor
	CODEC_INCLUDE device_codecs/shaper.h
	CODEC_REG feathers::codecs::kShaperCodec
	CARD_INCLUDE plugin/ui/cards/shaper.h
	CARD_REG feathers::ui::cards::kShaperCard
	INCLUDES fx/shaper/shaper_types.h
	TARGET feathers-fx
	SOURCES src/fx/shaper/shaper_types.cpp
		src/fx/shaper/shaper.cpp)

# --- From the hand-written kDevices entry this manifest replaces (device_registry.h, before NY.1): ---
# THE TABLE SHAPER — a waveshaper whose curve is a position in a phi-field rather than a preset.
# Two axes build the curve (Knee, a monotone scale; Colour, a field) and everything else plays it.
#
# REGISTERED WITH ITS CARD AND ITS PANEL IN ONE CHANGE, which is the only way it can be: the
# reachability guard asks that every control a DEVICE declares has something on one of its
# surfaces that reaches it, and this device's twelve are split across the two — the card carries
# four and the enable, the panel carries all twelve. Registering ahead of either would fail that
# guard with no table entry, exactly as `drwn` did.
#
# IT DECLARES NO PER-VOICE ROAD. Each instance owns a 1025-entry double-buffered table and an
# amortised builder; what N of those cost has not been measured, and device.h's rule is that a
# device which has not implemented a per-voice road must not claim one.
