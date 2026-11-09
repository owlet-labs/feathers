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
	ORDER 210
	DESCRIPTOR fx::clip::kDescriptor
	CODEC_INCLUDE device_codecs/clip.h
	CODEC_REG feathers::codecs::kClipCodec
	CARD_INCLUDE plugin/ui/cards/clip.h
	CARD_REG feathers::ui::cards::kClipCard
	INCLUDES fx/clip/clip_types.h
	TARGET feathers-fx
	SOURCES src/fx/clip/clip_types.cpp
		src/fx/clip/clip.cpp)

# --- From the hand-written kDevices entry this manifest replaces (device_registry.h, before NY.1): ---
# **CLIP — a clipper with five characters.** The clipper stands alone, with character modes and an
# oversampling toggle. APPENDED. Soft, Texture (Au5's
# detail-preserving foldback), Grit, Fold and Bits, 4x oversampled by default; `clip_types.h` opens with
# the normalised domain every curve works in.
