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
	ORDER 280
	DESCRIPTOR fx::phaser::kDescriptor
	CODEC_INCLUDE device_codecs/phaser.h
	CODEC_REG feathers::codecs::kPhaserCodec
	CARD_INCLUDE plugin/ui/cards/phaser.h
	CARD_REG feathers::ui::cards::kPhaserCard
	INCLUDES fx/phaser/phaser_types.h
	TARGET feathers-fx
	SOURCES src/fx/phaser/phaser.cpp
		src/fx/phaser/phaser_response.cpp)

# --- From the hand-written kDevices entry this manifest replaces (device_registry.h, before NY.1): ---
# **THE PHASER/FLANGER** (approved 2026-10-03). APPENDED.
# One device, Mode = Phaser | Flanger, with a saturating feedback loop, an LFO and an envelope follower
# that can each be switched off for an external modulator on Position. Its card is `PhaserCard`, its expanded
# view `PhaserExpanded` (the response plot and the explore pad).
