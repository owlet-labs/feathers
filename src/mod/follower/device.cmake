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
	ORDER 350
	DESCRIPTOR mod::follower::kDescriptor
	CODEC_INCLUDE device_codecs/follower.h
	CODEC_REG feathers::codecs::kFollowerCodec
	CARD_INCLUDE plugin/ui/cards/follower.h
	CARD_REG feathers::ui::cards::kFollowerCard
	INCLUDES mod/follower/follower_types.h
	TARGET feathers-mod
	SOURCES src/mod/follower/follower.cpp)

# --- From the hand-written kDevices entry this manifest replaces (device_registry.h, before NY.1): ---
# **THE FOLLOWER** (backlog MY, 2026-10-17; NW removed its Sidechain source; OP folded the Follower Tap into it as
# Mode = Audio, 2026-10-27). An envelope follower on a control Input (Control) or on the audio at its slot (Audio,
# its voice under Poly), with a Threshold that turns either into Trig pulses. What it may reach follows the Mode
# (`ModOps::readsAudioOf`). Its card is `FollowerCard`.
