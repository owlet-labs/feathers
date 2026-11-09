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
	ORDER 230
	DESCRIPTOR mod::macros::kDescriptor
	CODEC_INCLUDE device_codecs/macros.h
	CODEC_REG feathers::codecs::kMacrosCodec
	CARD_INCLUDE plugin/ui/cards/macros.h
	CARD_REG feathers::ui::cards::kMacrosCard
	INCLUDES mod/macros/macros_types.h
	TARGET feathers-mod
	SOURCES src/mod/macros/macros.cpp)

# --- From the hand-written kDevices entry this manifest replaces (device_registry.h, before NY.1): ---
# **MACROS — four macro knobs as a modulator device (macro-modulation phase B).** Each output is its
# macro's RESOLVED value, so unlike a header macro it is a destination too: an LFO can ride Macro 2.
# APPENDED, for the reason above.
#
# **SEATED BY BOTH HOSTS, AND NOTHING HERE HAS TO SAY SO.** There is one table: the rack reaches it
# through `hostCatalogue()` and lists it in its browser (`browseDevices`), and Feathers' modulator
# rail offers `maySitInModRack(d) && isTaggedModulator(d)` — four outputs and a `"modulator"` tag. Its
# card is `MacrosCard` (card_factory), the only reader of its Type params.
