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

# APPENDED LAST (ORDER 400): the device list's order is observable, so a new device goes at the end.
feathers_register_device(
	ORDER 400
	DESCRIPTOR mod::twinfield::kDescriptor
	CODEC_INCLUDE device_codecs/twinfield.h
	CODEC_REG feathers::codecs::kTwinFieldCodec
	CARD_INCLUDE plugin/ui/cards/twinfield.h
	CARD_REG feathers::ui::cards::kTwinFieldCard
	INCLUDES mod/twinfield/twinfield_types.h
	TARGET feathers-mod
	SOURCES src/mod/twinfield/twinfield.cpp)
