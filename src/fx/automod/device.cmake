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
	ORDER 80
	DESCRIPTOR fx::automod::kDescriptor
	CODEC_INCLUDE device_codecs/automod.h
	CODEC_REG feathers::codecs::kAutomodCodec
	CARD_INCLUDE plugin/ui/cards/automod.h
	CARD_REG feathers::ui::cards::kAutomodCard
	INCLUDES fx/automod/automod_types.h
	TARGET feathers-fx
	SOURCES src/fx/automod/automod.cpp)
