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
	ORDER 290
	DESCRIPTOR mod::stereo::kDescriptor
	CODEC_INCLUDE device_codecs/stereo.h
	CODEC_REG feathers::codecs::kStereoCodec
	CARD_INCLUDE plugin/ui/cards/stereo.h
	CARD_REG feathers::ui::cards::kStereoCard
	INCLUDES mod/stereo/stereo_types.h
	TARGET feathers-mod
	SOURCES src/mod/stereo/stereo.cpp)

# --- From the hand-written kDevices entry this manifest replaces (device_registry.h, before NY.1): ---
# THE STEREO TOOL (backlog KU) — Split and Width/Swap, the ear pairs stereo modulation needs that no
# other modulator is. Global, like Math; its card is Task 14 of the stereo-modulation plan.
