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
	ORDER 60
	DESCRIPTOR fx::drawn::kDescriptor
	CODEC_INCLUDE device_codecs/drawn.h
	CODEC_REG feathers::codecs::kDrawnCodec
	CARD_INCLUDE plugin/ui/cards/drawn.h
	CARD_REG feathers::ui::cards::kDrawnCard
	INCLUDES fx/drawn/drawn_types.h
	TARGET feathers-fx
	SOURCES src/fx/drawn/drawn_types.cpp
		src/fx/drawn/drawn.cpp)

# --- From the hand-written kDevices entry this manifest replaces (device_registry.h, before NY.1): ---
# THE DRAWN-CURVE FILTER — four frames of six pole-zero bands, blended bilinearly by an XY puck.
# A FORK of the waypoint filter above rather than a replacement: that one interpolates
# (type, cutoff, resonance) along a polyline, this one blends POSITIONS in the z plane, and the
# two reach different sounds.
#
# IT IS ALSO THE DEVICE THAT FORCED kMaxDeviceFields TO 128, and device_row.h says what that cost — and to 160
# on 2026-10-28, when the slot's Mix and wet Gain took it to 129 (backlog NR).
#
# REGISTERED IN THE SAME CHANGE AS `DrawnCard` AND ITS REACHABILITY ENTRY, which is the only way
# it can be. Seven of its 103 controls are reachable from the card; the other 96 are pole and zero
# positions belonging to the phase-4 editor, so tests/test_gui_widgets.cpp's coverage table
# carries them as KNOWN GAPS, each naming phase 4 — and that table pins a gap BOTH ways, so the
# entry fails the moment the editor makes them reachable and nobody updates it. It cannot rot
# into a description of the code.
#
# (An earlier reading held this back until phase 4 on the grounds that the guard could not pass
# with any card. It can: `knownGaps` is exactly the mechanism for a control declared, drawn
# perhaps, and reached by nothing. What that mechanism was once REFUSED for is the opposite
# case — HARM's controls, which WERE reachable through a second surface, where recording them as
# gaps would have turned the guard into a description of the change it was meant to police. The
# cost of waiting is what settled it: registering with the card puts a playable device in front
# of a player a whole phase before the expensive editor is built, and THIS DEVICE EXISTS BECAUSE a
# complete, coherent, well-tested filter turned out not to be the one asked for, and nobody
# found out until the finished card was seen the finished card.)
#
# AND WHY IT COULD NOT SIMPLY HAVE TAKEN `GenericCard` IN THE MEANTIME — measured, with the
# experiment attached, because a number in a comment is a different object from a measurement.
# At 98d2b41 (the phase-2 tip, device unregistered) plus exactly the one line below and nothing
# else, `scripts/run-cases.sh` on the two cases by name, on a clean tree, reverted after:
#
#   - "every control a DEVICE declares has something on one of its surfaces that reaches it":
#     FATAL at test_gui_widgets.cpp:19280, `tag := drwn` — no table entry.
#   - "EVERY card in the lane folds to its head when the head is pressed": `tag := drwn`, the
#     enable toggle reads `0 != 0`, and the open card measures **2842 pixels** — reported as
#     `24 == Approx(2842)` for BOTH `laneHeight()` and `height()`. `GenericCard` draws every
#     declared control, and the enable square of a card that tall could not be reached at all.
#
# `DrawnCard` measures 420, and its own case pins that it stays under 600. 34/34 before and
# after the experiment.
