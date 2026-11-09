/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program.
 * If not, see <https://www.gnu.org/licenses/>.
 *
 * --- Additional terms under GNU GPL version 3 section 7 ---
 * This file requires preservation of the above copyright notice and author attribution
 * in all copies or substantial portions of this file.
 */

#include "fixtures.h"

namespace gui_widgets_tests {

size_t expectedStateFrames(const feathers::Gui& gui) {
	const ui::FrameWatchers* watchers = gui.stateWatchers();
	if (watchers == nullptr) {
		return 0;
	}
	size_t total = 0;
	for (const visage::Frame* frame : watchers->framesForTest()) {
		total += ui::FrameWatchers::redrawTree(const_cast<visage::Frame*>(frame));
	}
	return total;
}

visage::MouseEvent at(float x, float y) {
	visage::MouseEvent event;
	event.relative_position = {3.0f, -2.0f};
	event.position = {x, y};
	event.button_id = visage::kMouseButtonLeft;
	event.button_state = visage::kMouseButtonLeft;
	event.is_down = true;
	return event;
}

visage::MouseEvent doubleAt(float x, float y) {
	visage::MouseEvent event = at(x, y);
	event.repeat_click_count = 2;
	return event;
}

visage::MouseEvent fineAt(float x, float y) {
	visage::MouseEvent event = at(x, y);
	event.modifiers = visage::kModifierShift;
	return event;
}

visage::MouseEvent doubleClickAt(float x, float y) {
	visage::MouseEvent event = at(x, y);
	event.repeat_click_count = 2;
	return event;
}

visage::MouseEvent tripleClickAt(float x, float y) {
	visage::MouseEvent event = at(x, y);
	event.repeat_click_count = 3;
	return event;
}

visage::MouseEvent optAt(float x, float y) {
	visage::MouseEvent event = at(x, y);
	event.modifiers = visage::kModifierAlt;
	return event;
}

std::vector<UiParamEvent> drain(ParamBridge& bridge) {
	std::vector<UiParamEvent> events;
	bridge.drain([&](const UiParamEvent& event) { events.push_back(event); });
	return events;
}

const ParamDesc& descOf(ParamId id) {
	const ParamDesc* desc = findParam(static_cast<clap_id>(id));
	REQUIRE(desc != nullptr);
	return *desc;
}

// **THE BOUND IS A DEADLINE, AND IT USED TO BE A COUNT OF SLEEPS** (2026-09-21, playbook 8cm).
//
// Both helpers below read `for (int waited = 0; waited < milliseconds; ++waited) { ...;
// sleep_for(1ms); }` — a parameter NAMED `milliseconds` that was an iteration count. A sleep is a
// FLOOR, so on a loaded host each one lands at five or ten and the "8 s" every caller thinks it asked
// for silently becomes a minute. MEASURED: a case in the explore suite with seven such loops was
// SIGTERMed by `run-cases.sh`'s 120 s watchdog at load average 30 while passing in seconds on an idle
// host.
//
// THIS IS THE SHARED FIXTURE, so the lie was the same one in every widget binary that settles a
// terrain. A `steady_clock` deadline means the same thing in every venue, and the parameter now means
// what its name says.
//
// CALLERS DID NOT CHANGE and did not need to: they passed a number they believed was milliseconds and
// now it is. What changes is that a BUSY host no longer quietly grants them ten times the wait — which
// is why the numbers at the call sites want to be generous rather than measured, and why the one that
// matters is sized against the runner's watchdog. See 8cm.
//
// **AND THE DEADLINE IS A STALL, NOT A BUDGET** (backlog KB, 2026-09-25). A fixed wall-clock bound
// traded one load-bound red for another: the dahdsr content-key case failed `REQUIRE(settle(...))`
// 3 times in 48 under ASan at load 43-73, never at 12-30. Nothing was wrong — a starved builder was
// still building, just slower than 20 s for one map (plus a prefetch it cannot abandon once started).
// What a settle guards against is a map that will NEVER arrive: a coalesced-away request, a lost
// wake, a worker gone idle on the wrong window. Every one of those looks the same from here — the
// builder stops doing work. So `milliseconds` is now how long the builder may do NOTHING before the
// settle fails, measured from its last texel, build or prefetch; while it computes, the wait goes on.
// A broken settle still fails after `milliseconds` of silence, exactly as fast as before, and a slow
// host no longer reads as a broken one. `kCeilingFactor` is the backstop for the one failure a
// progress count cannot see — a worker that keeps building and never builds the wanted window.
namespace {
constexpr int kCeilingFactor = 10;

template <typename Done, typename Progress>
bool settleUntil(int milliseconds, const Done& done, const Progress& progress) {
	const auto started = std::chrono::steady_clock::now();
	const auto ceiling = started + std::chrono::milliseconds(milliseconds) * kCeilingFactor;
	auto stalledAt = started + std::chrono::milliseconds(milliseconds);
	uint64_t seen = progress();
	while (true) {
		const auto now = std::chrono::steady_clock::now();
		if (now >= stalledAt || now >= ceiling) {
			break;
		}
		if (done()) {
			return true;
		}
		const uint64_t work = progress();
		if (work != seen) {
			seen = work;
			stalledAt = now + std::chrono::milliseconds(milliseconds);
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	// ONE LAST LOOK AFTER THE DEADLINE, so a settle that completed during the final sleep is not
	// reported as a timeout — the old loop had the same property by testing before each sleep.
	return done();
}

/// Everything the terrain thread counts as it works. Only CHANGE is read, so the sum need mean nothing.
uint64_t builderWork(const TerrainMap& map) {
	return map.builds() + map.texelsComputed() + map.prefetches();
}
} // namespace

bool settle(TerrainMap& map, const TerrainWindow& wanted, int milliseconds) {
	return settleUntil(
	    milliseconds,
	    [&] {
		    const std::shared_ptr<const TerrainSnapshot> now = map.snapshot();
		    return now->window == wanted && now->rows >= TerrainMap::kResolution;
	    },
	    [&] { return builderWork(map); });
}

bool settleRefined(TerrainMap& map, const TerrainWindow& wanted, int milliseconds) {
	const int target =
	    terrainSamplesFor(wanted.engine, wanted.travelYSpan * 1023.0
	                                         / static_cast<double>(TerrainMap::kResolution));
	return settleUntil(
	    milliseconds,
	    [&] {
		    const std::shared_ptr<const TerrainSnapshot> now = map.snapshot();
		    return now->window == wanted && now->rows >= TerrainMap::kResolution
		           && now->samples >= target;
	    },
	    [&] { return builderWork(map); });
}

TerrainWindow windowAt(double travelYCentre, double travelYSpan, double travelXSpan) {
	// **THE ARGUMENTS KEPT THEIR ORDER AND CHANGED THEIR MEANING** (backlog GY). The first two used to
	// be a home centre and a home span — the chart's vertical axis when that axis WAS gamma. Both axes
	// are displacements in the decoded plane now, so they are travel-Y, and the third is travel-X.
	//
	// RENAMED RATHER THAN RE-ORDERED, deliberately: every call site passes positionally, so a swap
	// would compile silently and draw a transposed map. The names are the only thing that can carry
	// the change to a reader.
	TerrainWindow window;
	window.engine = OscType::Morph;
	window.travelYCentre = travelYCentre;
	window.travelYSpan = travelYSpan;
	window.travelXSpan = travelXSpan;
	return window;
}

visage::Point centreInRoot(visage::Frame* frame) {
	const visage::Point at = frame->positionInWindow();
	return { at.x + frame->width() * 0.5f, at.y + frame->height() * 0.5f };
}

bool isWithin(visage::Frame* frame, visage::Frame* ancestor) {
	for (visage::Frame* at = frame; at != nullptr; at = at->parent()) {
		if (at == ancestor) {
			return true;
		}
	}
	return false;
}

visage::Frame* firstFrameOutside(visage::Frame* root, visage::Frame* frame) {
	if (!frame->isVisible()) {
		return nullptr;
	}
	if (frame != root) {
		const visage::Point at = frame->positionInWindow();
		constexpr float kSlack = 1.0f;
		if (at.x < -kSlack || at.y < -kSlack || at.x + frame->width() > root->width() + kSlack
		    || at.y + frame->height() > root->height() + kSlack) {
			return frame;
		}
	}
	for (visage::Frame* child : frame->children()) {
		if (visage::Frame* found = firstFrameOutside(root, child)) {
			return found;
		}
	}
	return nullptr;
}

void doubleClickHead(visage::Frame* root, visage::Point where) {
	for (int click = 1; click <= 2; ++click) {
		visage::Frame* target = root->frameAtPoint(where);
		if (target == nullptr) {
			return;
		}
		const visage::Point local = where - target->positionInWindow();
		target->onMouseDown().callback(click == 1 ? at(local.x, local.y)
		                                         : doubleAt(local.x, local.y));
		// **AND THE RELEASE, WHICH REAL HARDWARE SENDS AND THIS HELPER DID NOT.** `FxCard::onMouseUp`
		// carries the OTHER fold road — armed and never moved — so a helper that omitted the release
		// exercised only half the gesture and could not see the two roads firing together.
		target->onMouseUp().callback(click == 1 ? at(local.x, local.y) : doubleAt(local.x, local.y));
	}
}

void dragThroughRoot(visage::Frame* root, visage::Point from, visage::Point to) {
	visage::Frame* down = root->frameAtPoint(from);
	if (down == nullptr) {
		return;
	}
	const auto eventAt = [&](visage::Point windowPosition, bool isDown) {
		visage::MouseEvent e;
		e.window_position = windowPosition;
		e.position = windowPosition - down->positionInWindow();
		e.event_frame = down;
		e.button_id = visage::kMouseButtonLeft;
		e.button_state = isDown ? visage::kMouseButtonLeft : visage::kMouseButtonNone;
		e.is_down = isDown;
		e.repeat_click_count = 1;
		return e;
	};
	down->processMouseDown(eventAt(from, true));
	down->processMouseDrag(eventAt(to, true));
	down->processMouseUp(eventAt(to, false));
}

visage::Frame* keyboardFocusAfterPress(visage::Frame* root, visage::Point where) {
	visage::Frame* frame = root->frameAtPoint(where);
	while (frame != nullptr && !frame->acceptsKeystrokes()) {
		frame = frame->parent();
	}
	return frame;
}

bool keyFromFocus(visage::Frame* focused, const visage::KeyEvent& e) {
	visage::Frame* frame = focused;
	bool used = false;
	while (!used && frame != nullptr) {
		used = frame->processKeyPress(e);
		frame = frame->parent();
		while (frame != nullptr && !frame->acceptsKeystrokes()) {
			frame = frame->parent();
		}
	}
	return used;
}

bool keyUpFromFocus(visage::Frame* focused, const visage::KeyEvent& e) {
	visage::Frame* frame = focused;
	bool used = false;
	while (!used && frame != nullptr) {
		used = frame->processKeyRelease(e);
		frame = frame->parent();
		while (frame != nullptr && !frame->acceptsKeystrokes()) {
			frame = frame->parent();
		}
	}
	return used;
}

void markedWindowless(visage::Canvas& canvas, int width, int height) {
	canvas.setWindowless(width, height + 1);
}

visage::Screenshot takeMarkedCapture(visage::Canvas& canvas, int width, int height) {
	// A DIFFERENT COLOUR EVERY CAPTURE, so a readback of an EARLIER capture's frame fails the check
	// as well as a readback of memory nobody wrote. Mid-range in every channel: no fill byte, no
	// zeroed page and no opaque black or white can pass for it.
	static int serial = 0;
	++serial;
	const int r = 0x30 + (serial * 53) % 0xa0;
	const int g = 0x30 + (serial * 97) % 0xa0;
	const int b = 0x30 + (serial * 29) % 0xa0;
	canvas.setColor(0xff000000u | (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) |
	                static_cast<uint32_t>(b));
	canvas.fill(0.0f, static_cast<float>(height), static_cast<float>(width), 1.0f);
	canvas.submit();
	// THREE TIMES, as every windowless capture here does: the renderer can hand back the frame
	// before the one just submitted.
	canvas.takeScreenshot();
	canvas.takeScreenshot();
	const visage::Screenshot& full = canvas.takeScreenshot();
	REQUIRE(full.width() == width);
	REQUIRE(full.height() == height + 1);

	// ±2 PER CHANNEL, because the colour reaches the frame through an RGBA16F gradient atlas and
	// back. A frame that is not this one misses by far more than that.
	const auto close = [](int a, int e) { return a >= e - 2 && a <= e + 2; };
	int matched = 0;
	for (int x = 0; x < width; ++x) {
		const visage::Color c = full.sample(x, height);
		matched += close(c.hexRed(), r) && close(c.hexGreen(), g) && close(c.hexBlue(), b) ? 1 : 0;
	}
	CAPTURE(matched);
	CAPTURE(width);
	REQUIRE_MESSAGE(matched == width,
	                "the capture is not of the frame just drawn: its marker row did not come back. "
	                "This is the venue, not the widget (backlog CW, IT)");
	return visage::Screenshot(full.data(), width, height);
}

void collectKnobs(visage::Frame* root, std::vector<ui::Knob*>* out) {
	if (auto* knob = dynamic_cast<ui::Knob*>(root)) {
		out->push_back(knob);
	}
	for (visage::Frame* child : root->children()) {
		collectKnobs(child, out);
	}
}

} // namespace gui_widgets_tests
