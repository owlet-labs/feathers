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
	ORDER 370
	DESCRIPTOR rack::parallel::kDescriptor
	CODEC_INCLUDE device_codecs/parallel_rack.h
	CODEC_REG feathers::codecs::kParallelRackCodec
	CARD_INCLUDE plugin/ui/cards/parallel_rack.h
	CARD_REG feathers::ui::cards::kParallelRackCard
	INCLUDES rack/parallel_rack.h
	TARGET feathers-rack
	SOURCES src/rack/parallel_rack.cpp)

# --- From the hand-written kDevices entry this manifest replaces (device_registry.h, before NY.1): ---
# **THE RACK** (backlog NO, stage 1, 2026-10-18). APPENDED. Parallel chains: every chain hears the input, their
# outputs are summed through each one's level, mute and solo, then the rack's Mix and Gain. A rack is a device, so it
# nests in a Chain and a Chain in it (`rack/parallel_rack.h`). Its card is `ParallelRackCard`, its window `ParallelRackExpanded`.
