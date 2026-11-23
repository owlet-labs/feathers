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

feathers_register_device(
	ORDER 421
	DESCRIPTOR mod::macros8::kDescriptor
	CODEC_INCLUDE device_codecs/macros8.h
	CODEC_REG feathers::codecs::kMacros8Codec
	CARD_INCLUDE plugin/ui/cards/macros8.h
	CARD_REG feathers::ui::cards::kMacros8Card
	INCLUDES mod/macros8/macros8_types.h)
