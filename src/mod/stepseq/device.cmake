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
	ORDER 340
	DESCRIPTOR mod::stepseq::kDescriptor
	CODEC_INCLUDE device_codecs/stepseq.h
	CODEC_REG feathers::codecs::kStepseqCodec
	CARD_INCLUDE plugin/ui/cards/stepseq.h
	CARD_REG feathers::ui::cards::kStepseqCard
	INCLUDES mod/stepseq/stepseq_types.h
	TARGET feathers-mod
	SOURCES src/mod/stepseq/stepseq.cpp)

# --- From the hand-written kDevices entry this manifest replaces (device_registry.h, before NY.1): ---
# **THE STEP SEQUENCER** (backlog MX). APPENDED. Up to 32 steps of value, gate, probability, ratchet and slew,
# five directions, a Rate or Trig clock and a Reset; Randomise and a Turing machine. Its steps are ROW STATE under
# `steps_pattern`, so a patch stores the pattern that was heard and a reseed never rewrites it. Its card is
# `StepSeqCard`, the step grid itself.
