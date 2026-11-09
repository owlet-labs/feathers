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
	ORDER 300
	DESCRIPTOR fx::spectral::kDescriptor
	CODEC_INCLUDE device_codecs/spectral.h
	CODEC_REG feathers::codecs::kSpectralCodec
	CARD_INCLUDE plugin/ui/cards/spectral.h
	CARD_REG feathers::ui::cards::kSpectralCard
	INCLUDES fx/spectral/spectral_types.h
	TARGET feathers-fx
	SOURCES src/fx/spectral/spectral_stft.cpp
		src/fx/spectral/spectral_sidechain.cpp
		src/fx/spectral/spectral_envelope.cpp
		src/fx/spectral/spectral_types.cpp
		src/fx/spectral/spectral.cpp)

# --- From the hand-written kDevices entry this manifest replaces (device_registry.h, before NY.1): ---
# **SPECTRAL** (design approved 2026-10-12). APPENDED. The additive drawer's
# operators by the same function (`spectral::gains()`) on any audio, through an STFT whose latency is fixed per
# Resolution stop. On the mix the Hz operators; per voice the harmonic ones follow the note.
