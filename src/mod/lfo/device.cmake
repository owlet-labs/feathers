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
	ORDER 150
	DESCRIPTOR mod::lfo::kDescriptor
	CODEC_INCLUDE device_codecs/lfo.h
	CODEC_REG feathers::codecs::kLfoCodec
	CARD_INCLUDE plugin/ui/cards/lfo.h
	CARD_REG feathers::ui::cards::kLfoCard
	INCLUDES mod/lfo/lfo_types.h
	TARGET feathers-mod
	SOURCES src/mod/lfo/lfo.cpp)

# --- From the hand-written kDevices entry this manifest replaces (device_registry.h, before NY.1): ---
# THE LFO — a basic waveform generator with sine, triangle, square and random walk. Skew knobs
# deform the periodic waveforms; seed and gamma control the random walk. Same sync/rate and
# loop modes as the MSEG.
#
# APPENDED AT THE END for the reason every entry above already paid for: this list's ORDER IS
# OBSERVABLE and the browser lists in it.
#
# USES THE GENERIC CARD — `ModPanel::CardKind::Generic` draws all of its controls, and the LFO's
# controls fit the generic grid. A hand-written card (waveform display with star) lands in its
# own change; the generic card makes the device playable now.
