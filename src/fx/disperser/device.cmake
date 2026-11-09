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
	ORDER 270
	DESCRIPTOR fx::disperser::kDescriptor
	CODEC_INCLUDE device_codecs/disperser.h
	CODEC_REG feathers::codecs::kDisperserCodec
	CARD_INCLUDE plugin/ui/cards/disperser.h
	CARD_REG feathers::ui::cards::kDisperserCard
	INCLUDES fx/disperser/disperser_types.h
	TARGET feathers-fx
	SOURCES src/fx/disperser/disperser.cpp
		src/fx/disperser/disperser_response.cpp)

# --- From the hand-written kDevices entry this manifest replaces (device_registry.h, before NY.1): ---
# **THE DISPERSER — the reference's dispersion plugin, ported as the Eroder was.** APPENDED. Up to 32
# allpasses routed eight ways, with a delay that feeds the output back through them; bus only,
# following the last note (`setNoteContext`), by design.
# `disperser_types.h` opens with the reference and the port doc lists every deviation.
