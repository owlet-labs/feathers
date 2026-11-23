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
	ORDER 420
	DESCRIPTOR fx::balance::kDescriptor
	CODEC_INCLUDE device_codecs/balance.h
	CODEC_REG feathers::codecs::kBalanceCodec
	CARD_INCLUDE plugin/ui/cards/balance.h
	CARD_REG feathers::ui::cards::kBalanceCard
	INCLUDES fx/balance/balance_types.h
	TARGET feathers-fx
	SOURCES src/fx/balance/balance_types.cpp
		src/fx/balance/balance.cpp)

# STEREO BALANCE — a corrective tool: OWLTT's comb split into harmonic classes, each class's left/right imbalance measured (Attack / Release) and pulled
# towards the centre by Strength, then an equal-power Pan. HP Wet / Sub Thru ahead of the split. APPENDED.
