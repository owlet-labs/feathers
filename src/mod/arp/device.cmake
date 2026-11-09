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

# THE CLASSIC ARP — the note lane's first device that emits notes on its own clock
# (`NoteOps::emitNotes`). Appended: this list's order is observable (the browser lists in it).
feathers_register_device(
	ORDER 390
	DESCRIPTOR mod::arp::kDescriptor
	CODEC_INCLUDE device_codecs/arp.h
	CODEC_REG feathers::codecs::kArpCodec
	CARD_INCLUDE plugin/ui/cards/arp.h
	CARD_REG feathers::ui::cards::kArpCard
	INCLUDES mod/arp/arp_types.h
	TARGET feathers-mod
	SOURCES src/mod/arp/arp_core.cpp src/mod/arp/step_emitter.cpp src/mod/arp/theory.cpp src/mod/arp/genes.cpp src/mod/arp/pattern.cpp src/mod/arp/humanize.cpp src/mod/arp/arp.cpp)

# THE PATTERN DEVICE — the house's terrain arp: four patterns, each a full control
# set decoded from a phi terrain, one playing at a time under a DAW-automated selector. It shares the classic arp's
# `StepEmitter` (the sources above are the same target's). Its card (`PatternCard`: the tabs, the focused pattern's lane) is
# its own; its window is `GenericExpanded` until the terrain's.
feathers_register_device(
	ORDER 391
	DESCRIPTOR mod::arp::pat::kDescriptor
	CODEC_INCLUDE device_codecs/pattern.h
	CODEC_REG feathers::codecs::kPatternCodec
	CARD_INCLUDE plugin/ui/cards/pattern.h
	CARD_REG feathers::ui::cards::kPatternCard
	INCLUDES mod/arp/pattern_types.h
	TARGET feathers-mod
	SOURCES src/mod/arp/pattern_types.cpp src/mod/arp/pattern_cycle.cpp src/mod/arp/pattern_midi.cpp src/mod/arp/pattern_device.cpp src/mod/arp/pattern_text.cpp src/mod/arp/pattern_likes.cpp src/mod/arp/pattern_warp.cpp)
