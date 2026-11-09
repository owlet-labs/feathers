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
	ORDER 140
	DESCRIPTOR rack::kDeviceDescriptor
	CODEC_INCLUDE device_codecs/chain.h
	CODEC_REG feathers::codecs::kChainCodec
	CARD_INCLUDE plugin/ui/cards/chain.h
	CARD_REG feathers::ui::cards::kChainCard
	INCLUDES rack/rack_device.h
	TARGET feathers-rack
	SOURCES src/rack/rack_device.cpp)

# --- From the hand-written kDevices entry this manifest replaces (device_registry.h, before NY.1): ---
# **THE RACK — the same `RackEngine` the standalone rack plugin runs, as a device.** So it sits in a
# chain like any effect: Feathers' fx lane, or another rack. A rack in a rack is this, in this;
# there is no separate nesting feature.
#
# AT THE END RATHER THAN THE FRONT: nothing indexes this list by position, but its order is what a
# browser shows, and a new device first would be a layout decision made by an edit.
