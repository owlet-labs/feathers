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
	ORDER 40
	DESCRIPTOR fx::sklitter::kDescriptor
	CODEC_INCLUDE device_codecs/sklitter.h
	CODEC_REG feathers::codecs::kSklitterCodec
	CARD_INCLUDE plugin/ui/cards/sklitter.h
	CARD_REG feathers::ui::cards::kSklitterCard
	INCLUDES fx/sklitter/sklitter_types.h)

# --- From the hand-written kDevices entry this manifest replaces (device_registry.h, before NY.1): ---
# SKLITTER — the reference's stutterer, ported. Eight modes over one captured bar; the six looper
# modes share one slice-selection algorithm and differ in what the rate buys.
#
# **THREE REGISTRY-WIDE SWEEPS NAME IT AS ONE THEY CANNOT ASSESS, each with its reason**, because
# it is a MOMENTARY, TEMPO-LOCKED LOOPER and they are built for always-on processors: it has no
# smoother for the partition sweep to measure, it sustains by design rather than decaying, and
# thirteen of its controls act only in the looper modes — which capture a BAR before they sound,
# far past any probe those sweeps can afford. What each would have asked is asserted directly in
# `test_fx_sklitter*.cpp`, where the device can be heard. See `tests/test_fx_devices.cpp`.
