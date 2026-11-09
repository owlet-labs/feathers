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
	ORDER 380
	DESCRIPTOR fx::inject::kDescriptor
	CODEC_INCLUDE device_codecs/inject.h
	CODEC_REG feathers::codecs::kInjectCodec
	CARD_INCLUDE plugin/ui/cards/inject.h
	CARD_REG feathers::ui::cards::kInjectCard
	INCLUDES fx/inject/inject_types.h
	TARGET feathers-fx
	SOURCES src/fx/inject/inject.cpp)

# **SIDECHAIN INJECT** (backlog NW). Brings the plugin's Sidechain input into the chain at its position,
# with a Mix (passing signal against sidechain) and a Gain. It replaced the Follower's own Sidechain source: following a
# sidechain is now Inject plus a Follower in Audio mode after it. `inject_types.h` has the design decisions.
