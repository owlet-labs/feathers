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
	ORDER 160
	DESCRIPTOR fx::delay::kDescriptor
	CODEC_INCLUDE device_codecs/delay.h
	CODEC_REG feathers::codecs::kDelayCodec
	CARD_INCLUDE plugin/ui/cards/delay.h
	CARD_REG feathers::ui::cards::kDelayCard
	INCLUDES fx/delay/delay_types.h
		fx/delay/delay_taps.h
	TARGET feathers-fx
	SOURCES src/fx/delay/delay.cpp)

# --- From the hand-written kDevices entry this manifest replaces (device_registry.h, before NY.1): ---
# **THE DELAY — a tempo-syncable delay line with a granulator inside its feedback loop.** Added
# 2026-09-20 as the first of the table-stakes effects.
#
# APPENDED AT THE END for the reason every entry above already paid for: this list's ORDER IS
# OBSERVABLE and the browser lists in it.
#
# **THE SYNC LIST IS `automod`'S, NOT THE LFO'S**, and the device asked for "a sync section like
# the lfos". The two differ: the LFO's dropdown opens with `Off`, which re-reads the knob beside
# it as a free rate; `automod` puts the free constants IN the list and leaves the knob one
# meaning. The second is what was described in Bitwig's terms when `automod` was built and what
# chosen again here, so the newer shape is the one the third device follows.
#
# USES THE GENERIC CARD, and the four granular controls are GATED on its Mode rather than hidden
# by a bespoke face — `GenericCard` asks `device::relevantFor` per control, so the honesty rule
# costs a `gateControl` and a `gateMask` in the descriptor and no UI code at all.
