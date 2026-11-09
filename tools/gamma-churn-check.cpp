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

// DOES GLOBAL-GAMMA CHURN MAKE A BATCH LIE ABOUT ITS OWN SIZE?
//
// WHAT THIS IS FOR. The last open glitch — a solid lavender polygon with sheared streaks over the
// rails, and a white dotted diagonal — was screencast on a build that reports dropped
// batches, and it appeared with NO dropped line. So it is not the arena. The trigger identified
// is global-gamma churn, which is the maximum-turnover case: gamma is a macro over every sound's
// character, so moving it rebuilds every card's text and readouts rather than just moving geometry.
//
// The shape of the picture — geometry at coordinates nothing asked for, sheared because consecutive
// stale vertices differ progressively — is the signature of vertices that were INDEXED BUT NEVER
// WRITTEN. A batch allocates and indexes N quads and the writer fills fewer; the tail indices then
// address whatever the transient arena still held. That class is now instrumented: `SHORT` when the
// walk delivers fewer pieces than were counted, `UNWALKED` when it still holds pieces after the
// count was satisfied. Both report in release. This drives the churn and reads the reports.
//
// WHY WINDOWLESS, WHICH IS A LIMITATION AND NOT A PREFERENCE. The proper venue is guihost on a real
// display, driving the assembled editor. That venue deadlocks on this machine — the editor opens and
// the main thread parks in a futex beside Mesa's WSI swapchain threads, on binaries from today and
// from a day before today alike, so it is the session and not the code. The windowless renderer
// still works, so this is what can be run. What it therefore CANNOT do:
//
//   * It cannot photograph the glitch. There is no window and no screencast to compare against.
//   * It repaints in full every frame. The real fault lives in a retained-mode canvas with partial
//     damage, and a full repaint is one damage rectangle — so the piece-per-rect multiplier that
//     makes these ceilings reachable is 1 here. A negative result is therefore weak evidence, and
//     the file says so rather than letting a green line be read as an acquittal.
//
// What it CAN do is answer the discriminating question — does this churn provoke a count-versus-walk
// disagreement at all — without a display.
//
// THE SURFACES ARE THE ONES IN THE PICTURE: the Perform stage and the sound-stack rail. The glitch
// sits over the rails, and the rail is what a gamma change rebuilds hardest.
//
//   ninja -C build gamma-churn-check && build/gamma-churn-check

#include "plugin/gui_bridge.h"
#include "plugin/params.h"
#include "plugin/ui/notify_tray.h"
#include "plugin/ui/perform_stage.h"
#include "plugin/ui/edit_history.h"
#include "plugin/ui/sound_stack.h"
#include "plugin/ui/ui_state.h"

#include "visage_graphics/canvas.h"
#include "visage_graphics/graphics_utils.h"
#include "visage_graphics/region.h"
#include "visage_graphics/renderer.h"

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <string>
#include <unistd.h>

namespace {

constexpr double kPi = 3.14159265358979;

// The editor's size, the same fixture the widget suite and the census use.
constexpr float kWidth = 866.0f;
constexpr float kHeight = 534.0f;

int failures = 0;

void check(bool ok, const char* what) {
	std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok) {
		failures++;
	}
}

// Same device as batch-overflow-check: the trace writes to fd 2 and flushes, so redirecting the
// descriptor around the run is enough, and it is POSIX-only for the same reason the trace is.
class CapturedStderr {
public:
	CapturedStderr() {
		std::snprintf(path_, sizeof(path_), "/tmp/feathers-gamma-churn-XXXXXX");
		file_ = mkstemp(path_);
		saved_ = dup(STDERR_FILENO);
		std::fflush(stderr);
		dup2(file_, STDERR_FILENO);
	}

	~CapturedStderr() {
		if (saved_ >= 0) {
			restore();
		}
		unlink(path_);
	}

	std::string text() {
		restore();
		std::string contents;
		FILE* reader = std::fopen(path_, "r");
		if (reader != nullptr) {
			char buffer[512];
			size_t read = 0;
			while ((read = std::fread(buffer, 1, sizeof(buffer), reader)) > 0) {
				contents.append(buffer, read);
			}
			std::fclose(reader);
		}
		return contents;
	}

private:
	void restore() {
		std::fflush(stderr);
		if (saved_ >= 0) {
			dup2(saved_, STDERR_FILENO);
			close(saved_);
			saved_ = -1;
		}
		if (file_ >= 0) {
			close(file_);
			file_ = -1;
		}
	}

	char path_[64] {};
	int file_ = -1;
	int saved_ = -1;
};

bool mentions(const std::string& log, const char* what) {
	return log.find(what) != std::string::npos;
}

void reportLines(const std::string& log, const char* marker) {
	size_t at = log.find(marker);
	int shown = 0;
	while (at != std::string::npos && shown < 4) {
		const size_t from = log.rfind('\n', at);
		const size_t to = log.find('\n', at);
		const size_t begin = from == std::string::npos ? 0 : from + 1;
		std::printf("      %s\n", log.substr(begin, to == std::string::npos ? std::string::npos : to - begin).c_str());
		at = log.find(marker, at + 1);
		shown++;
	}
}

} // namespace

int main(int argc, char** argv) {
	// The trace's opt-in half (warnings, splits) is off unless asked for, but SHORT, UNWALKED and
	// DROPPED report regardless — which is the half this file is about, and the half that fires in
	// somebody's session. Pass --traced to see the splits too.
	const bool traced = argc > 1 && std::string(argv[1]) == "--traced";
	if (traced) {
		setenv("VISAGE_TRACE_BATCH", "1", 1);
	}

	visage::Canvas canvas;
	canvas.setWindowless(static_cast<int>(kWidth), static_cast<int>(kHeight));
	if (!visage::Renderer::instance().supported()) {
		std::printf("SKIP  no renderer: %s\n", visage::Renderer::instance().errorMessage().c_str());
		return 77;
	}

	feathers::ParamBridge bridge;
	feathers::ui::WidgetRegistry registry;
	feathers::ui::NotifyTray tray;
	feathers::ui::SoundSelection selection;
	feathers::ui::EditHistory history;
	feathers::ui::FrameWatchers previewWatchers;
	feathers::ParamSet params;
	int64_t now = 1000;

	feathers::ui::PerformStage stage { bridge, registry, &tray, [&now] { return now; } };
	feathers::ui::SoundStack rail { bridge, registry, selection, history, previewWatchers, &tray };

	auto set = [&](feathers::ParamId id, double value) {
		params.setBase(static_cast<clap_id>(id), value);
		bridge.publish(params);
		bridge.refresh();
	};

	// The rail takes its own column at the mock's width; the stage takes what is left. Both are
	// drawn every frame, because the glitch sits across both.
	const float railWidth = feathers::ui::SoundStack::kWidth;
	rail.setBounds(0.0f, 0.0f, railWidth, kHeight);
	stage.setBounds(railWidth, 0.0f, kWidth - railWidth, kHeight);
	bridge.refresh();
	selection.select(1);

	// Weather up, because that is the state reported from and the one that makes the terrain
	// contribute at all.
	set(feathers::ParamId::Weather, 0.6);

	// ONE CAPTURE OVER THE WHOLE RUN, INCLUDING THE SETTLE, and this is not tidiness — it is the
	// difference between a control and a decoration.
	//
	// Every report in graphics_utils.h is bounded at eight per PROCESS. A first draft of this file
	// captured only the churn loops and let the settle frames run uncaptured, and against a build
	// whose walk stopped a shape early it PASSED: the eight reports were all spent during settle, on
	// the console, and by the time the capture opened the trace had gone permanently quiet. The
	// checks were reading an empty log and calling it silence.
	//
	// So the capture opens before the first submit of any kind and closes at the end, and the checks
	// read one log. It costs the ability to say WHICH phase a report came from — worth losing, since
	// with a budget of eight the first occurrence is the only one guaranteed to be recorded anyway.
	CapturedStderr captured;

	// Settle, so the runs below measure a populated surface rather than one still building.
	for (int settle = 0; settle < 60; ++settle) {
		now += 16;
		stage.update();
		canvas.clearDrawnShapes();
		rail.draw(canvas);
		stage.draw(canvas);
		canvas.submit();
	}

	// THE CHURN. Global Gamma full-range at 1.7 Hz, so the displayed number changes every frame
	// rather than settling — a readout that reads the same twice does not rebuild, and a card that
	// does not rebuild is not the load this is here to apply. The position parameters ride along on
	// incommensurate LFOs so the geometry never repeats, which is the census recipe.
	constexpr int kFrames = 900;
	uint64_t worst = 0;
	for (int frame = 0; frame < kFrames; ++frame) {
		const double t = static_cast<double>(frame) / 60.0;
		set(feathers::ParamId::GlobalGamma, 4.0 * std::sin(t * 2.0 * kPi * 1.7));
		set(feathers::ParamId::Path, 0.5 + 0.5 * std::sin(t * 2.0 * kPi * 0.31));
		set(feathers::ParamId::PosX, std::sin(t * 2.0 * kPi * 0.53));
		set(feathers::ParamId::PosY, std::cos(t * 2.0 * kPi * 0.83));

		now += 16;
		stage.update();
		canvas.clearDrawnShapes();
		rail.draw(canvas);
		stage.draw(canvas);
		canvas.submit();
		const uint64_t quads = visage::batchQuadsThisFrame();
		if (quads > worst) {
			worst = quads;
		}
	}

	// AND NOW THE SAME CHURN UNDER MULTI-RECT DAMAGE, which is the condition that makes any of these
	// ceilings reachable and the one the phase above cannot produce.
	//
	// `setupQuads` emits one quad per shape PER INVALID RECT it overlaps, so a frame repainting many
	// areas multiplies every full-width shape by the number of areas. A real editor repainting under
	// automation is exactly that: several widgets dirty at once, their damage kept as a list of
	// non-overlapping rectangles. Here the surfaces are drawn into a retained Region and then
	// re-submitted against a GRID of disjoint rectangles, so the multiplier is applied to the real
	// geometry rather than to stand-in rectangles.
	//
	// WHAT THE MULTIPLIER ACTUALLY COMES TO, measured rather than assumed, because the arithmetic is
	// not the naive one: 40 rectangles do NOT turn 559 quads into 22360. A shape is counted once per
	// rectangle it OVERLAPS, and these surfaces are mostly small shapes that fall inside one or two
	// cells of the grid, so the worst frame comes out at 845 — up from 559, which is the multiplier
	// working, and nowhere near the 16384 index ceiling.
	//
	// That is worth stating as a RESULT rather than hiding as a caveat. Under the churn that provokes
	// the glitch, these surfaces ask for three orders of magnitude less than the ceilings this
	// machinery exists to police, and the batches are never even chunked — so the resume path is not
	// exercised here and, more to the point, no ceiling is anywhere near being met. Whatever defect
	// three is, on this evidence it is not the batch sizes.
	visage::Region region;
	region.setBounds(0, 0, static_cast<int>(kWidth), static_cast<int>(kHeight));
	canvas.addRegion(&region);

	constexpr int kDamageColumns = 8;
	constexpr int kDamageRows = 5;
	uint64_t worstPieces = 0;
	{
		for (int frame = 0; frame < kFrames; ++frame) {
			const double t = static_cast<double>(frame) / 60.0;
			set(feathers::ParamId::GlobalGamma, 4.0 * std::sin(t * 2.0 * kPi * 1.7));
			set(feathers::ParamId::Path, 0.5 + 0.5 * std::sin(t * 2.0 * kPi * 0.31));
			set(feathers::ParamId::PosX, std::sin(t * 2.0 * kPi * 0.53));
			set(feathers::ParamId::PosY, std::cos(t * 2.0 * kPi * 0.83));

			now += 16;
			stage.update();
			canvas.clearDrawnShapes();
			canvas.beginRegion(&region);
			rail.draw(canvas);
			stage.draw(canvas);
			canvas.endRegion();
			canvas.submit();

			// The retained shapes, re-batched against a grid of disjoint damage. Nothing is redrawn
			// between these two submits: the multiplier is the whole point.
			const int cellWidth = static_cast<int>(kWidth) / kDamageColumns;
			const int cellHeight = static_cast<int>(kHeight) / kDamageRows;
			for (int column = 0; column < kDamageColumns; ++column) {
				for (int row = 0; row < kDamageRows; ++row) {
					region.invalidateRect({ column * cellWidth, row * cellHeight, cellWidth - 1, cellHeight - 1 });
				}
			}
			canvas.submit();
			const uint64_t pieces = visage::batchQuadsThisFrame();
			if (pieces > worstPieces) {
				worstPieces = pieces;
			}
		}
	}

	// EVERYTHING ABOVE, READ AS ONE LOG — settle, full repaints and damage grid together. See the
	// capture's own comment for why it cannot be split: with eight reports to spend for the whole
	// process, only the first occurrence is guaranteed to have been recorded at all.
	const std::string log = captured.text();

	std::printf("frames %d, worst full-repaint frame %llu quads, arena %d quads\n", kFrames,
	            static_cast<unsigned long long>(worst), visage::transientQuadCapacity());
	std::printf("multi-rect: %d rects, worst frame %llu quads\n", kDamageColumns * kDamageRows,
	            static_cast<unsigned long long>(worstPieces));

	check(worst > 0, "the churn actually drew something");
	check(worstPieces > worst, "the damage grid really did multiply the pieces");
	if (mentions(log, "SHORT")) {
		reportLines(log, "SHORT");
	}
	check(!mentions(log, "SHORT"), "no batch wrote fewer quads than it allocated and indexed");
	if (mentions(log, "UNWALKED")) {
		reportLines(log, "UNWALKED");
	}
	check(!mentions(log, "UNWALKED"), "and no batch left pieces unwalked after its last run");
	if (mentions(log, "DROPPED")) {
		reportLines(log, "DROPPED");
	}
	check(!mentions(log, "DROPPED"), "and nothing was dropped for want of arena");

	// A PASS IS STILL NOT AN ACQUITTAL, and printing so is the honest end of a control that cannot
	// reach its own subject. There is no window here and no screencast to compare against, and the
	// damage grid is a plausible shape for a busy frame rather than the shape a real editor produces.
	std::printf("%s\n", failures == 0 ? "PASS" : "FAILED");
	if (failures == 0) {
		std::printf("note: no window, so this cannot photograph the glitch. What it does say is that the\n");
		std::printf("      churn provokes no count-versus-walk disagreement, and that these surfaces ask\n");
		std::printf("      for hundreds of quads where the ceilings are at 16384 — so no batch here is\n");
		std::printf("      ever chunked, and the resume path is NOT covered by this run.\n");
	}
	return failures == 0 ? 0 : 1;
}
