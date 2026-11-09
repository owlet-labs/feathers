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
	ORDER 110
	DESCRIPTOR mod::tracker::kDescriptor
	CODEC_INCLUDE device_codecs/tracker.h
	CODEC_REG feathers::codecs::kTrackerCodec
	INCLUDES mod/tracker/tracker_types.h
	TARGET feathers-mod
	SOURCES src/mod/tracker/tracker.cpp)

# --- From the hand-written kDevices entry this manifest replaces (device_registry.h, before NY.1): ---
# **THE PITCH TRACKER IS REGISTERED — the line at the bottom of this note. THE NOTE ITSELF IS THE
# HISTORY OF WHY IT WAS NOT, kept because it records what registering costs, and corrected
# 2026-09-04 by reading the entry under it rather than the sentence over it.**
#
# It said "NOT REGISTERED HERE YET, AND THAT IS A HELD DECISION RATHER THAN AN OVERSIGHT",
# directly above the entry that registers it — a comment that answers the reader's question
# confidently and wrongly, which is worse than none. An audit of every device's enable default
# walked this table mechanically and got ten; a reader who believed this paragraph would have
# audited nine and never known which one was missing.
#
# WHAT REMAINS TRUE BELOW is everything about the COST, and it is still the reason to read on:
# registering a modulator obliges Feathers a hand-written card, because `ModPanel::CardKind` has
# no generic fallback.
#
# It exists — `src/mod/tracker/` — and the rack's note lane is built for it.
#
# REGISTERING IT BROKE FEATHERS, AND THE TEST THAT CAUGHT IT IS RIGHT: *"if one does not, this
# build has a modulator it cannot draw and the fallback is hiding it rather than the fallback
# being unused."* `ModPanel::CardKind` is `{ Dormant, Envelope, Mseg, NoEditor }` — TWO real
# cards, one per modulator, and no generic fallback. So every device in this list that publishes
# an output obliges a hand-written card in Feathers' modulator strip.
#
# THE QUESTION THAT NEEDS ANSWERING BEFORE IT GOES BACK IN, AND HALF OF IT IS NOW ANSWERED. It
# used to read: *"a tracker is driven by NOTES, and Feathers' modulator strip has no note road —
# so a card there would be a control surface for a device that cannot move. Either Feathers grows
# a Tracker card (and the note road to make it mean something), or this vocabulary grows a way to
# say a device belongs to one host."*
#
# **THE NOTE ROAD EXISTS NOW.** `Synth` captures the block's notes and `ModRunner::processNotes`
# offers them to the rack in order, on the rack engine's own terms — chained not broadcast, the
# host not interpreting, the device's reported count clamped. A tracker seated in Feathers'
# modulator strip would move. So the objection that a card would be a surface for a dead device
# is retired; what is left is the narrower one, which is still real: `ModPanel::CardKind` is
# `{ Dormant, Envelope, Mseg, NoEditor }`, and a test asserts the fallback goes unused — so
# registering this obliges a hand-written Tracker card, and that card was explicitly deferred
# (plumbing first). The registration waits on the card, not on the
# plumbing.
#
# AND THE COST OF THE WAIT IS NOW WRITTEN DOWN RATHER THAN DISCOVERED: with no registered device
# declaring `NoteOps`, neither this host's note lane nor the rack's has END-TO-END coverage
# through a real chain. `tests/test_rack_engine.cpp` skips its lane case for exactly this reason
# and says so. Feathers' side is covered instead through `ModRunner::seatForTest`, which seats a
# descriptor the registry does not carry — see the note there on why a capability nothing
# interrogates is indistinguishable from one that is absent.
#
# AND A SMALLER LESSON, PAID FOR IN THE SAME HOUR: **this list's ORDER IS OBSERVABLE.** The
# browser lists in it and `test_gui_widgets` picks `offered[1]` positionally, so registering at
# the TOP shifted every index and failed three cases for a second, unrelated reason. Append.

