/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even
 * the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
 * Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

// WHICH DEVICES PUBLISH LIVE READINGS INTO THE RACK, AND HOW.
//
// **THIS EXISTS BECAUSE IT WAS A LIST NOBODY COULD TEST.** The wiring lived inside `RackPlugin`'s
// constructor in `rack_clap.cpp` — a translation unit whose only suite (`rack-clap`) cannot run
// headless — so "does the rack wire automod's phase" was not a question any test could ask. It
// answered for OWLTT and not for automod, and the way that surfaced was a card that
// never moved: the curves in automod did not animate at all.
//
// **AND IT IS THE SECOND HALF OF A TWO-LIST DISAGREEMENT.** `BridgeDeviceHost::readingCountAt`
// already answered for BOTH devices; this side answered for one. Neither list mentions the other and
// nothing made them disagree out loud — the same shape as `expanded_views.cpp`'s `buildable` list,
// where a panel existed, was correct, and was unreachable. Moving the wiring here does not merge the
// two lists, but it does put this one somewhere a test can reach, which is what was missing.

#pragma once

namespace feathers::rack {

class RackEngine;

/// Wire every device that publishes a live reading to `engine.publishReadings`.
///
/// Idempotent in the way that matters: it installs ONE connector, so calling it twice replaces rather
/// than accumulates. The connector is invoked by the engine for every device as it is constructed —
/// filtering is this side's job, which is the boundary `setDspConnector` documents.
void installDevicePublishers(RackEngine& engine);

} // namespace feathers::rack
