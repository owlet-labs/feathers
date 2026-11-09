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
	ORDER 220
	DESCRIPTOR fx::resonate::kDescriptor
	CODEC_INCLUDE device_codecs/resonate.h
	CODEC_REG feathers::codecs::kResonateCodec
	CARD_INCLUDE plugin/ui/cards/resonate.h
	CARD_REG feathers::ui::cards::kResonateCard
	INCLUDES fx/resonate/resonate_types.h
	TARGET feathers-fx
	SOURCES src/fx/resonate/resonate_types.cpp
		src/fx/resonate/resonate_dsp.cpp)

# --- From the hand-written kDevices entry this manifest replaces (device_registry.h, before NY.1): ---
# **RESONATE — the scanned-string effect**: the Weave synth engine's string, ported as a plugin
# effect. One string on the bus, bowed and plucked by the
# input; held notes read it at pitch and the input is filtered through its shape. `resonate_types.h`
# opens with why its controls move the WAVEFORM and not the pitch. Its card is `GenericCard`.
