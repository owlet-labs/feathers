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
	ORDER 50
	DESCRIPTOR fx::waypoint::kDescriptor
	CODEC_INCLUDE device_codecs/waypoint.h
	CODEC_REG feathers::codecs::kWaypointCodec
	CARD_INCLUDE plugin/ui/cards/waypoint.h
	CARD_REG feathers::ui::cards::kWaypointCard
	INCLUDES fx/waypoint/waypoint_types.h
	TARGET feathers-fx
	SOURCES src/fx/waypoint/waypoint_types.cpp
		src/fx/waypoint/waypoint.cpp)
