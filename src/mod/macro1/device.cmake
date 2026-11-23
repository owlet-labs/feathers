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
	ORDER 420
	DESCRIPTOR mod::macro1::kDescriptor
	CODEC_INCLUDE device_codecs/macro1.h
	CODEC_REG feathers::codecs::kMacro1Codec
	CARD_INCLUDE plugin/ui/cards/macro1.h
	CARD_REG feathers::ui::cards::kMacro1Card
	INCLUDES mod/macro1/macro1_types.h)
