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

// A REAL WINDOW WITH A REAL HAND IN IT — the venue the fan/polygon hunt never had.
//
//   build/windowed-probe                     # 20 gesture cycles, verdict per frame
//   FEATHERS_PROBE_CONTROL=1 build/windowed-probe   # the known-bad: ink where ink may not be
//
// THE GAP THIS FILLS, stated plainly because it is the whole reason the file exists. The defect is
// origin-anchored ink — dashed fans and lavender polygons — that appears over chrome which never
// repaints, and it is triggered near-deterministically by a HAND dragging the gamma knob. Every
// venue we own has been missing exactly one half of what it takes to photograph that:
//
//   - guihost and bitwighost open a real window, but they hold the plugin across the CLAP boundary
//     and cannot reach the widget tree, so the only input they can offer is the PARAMETER API. The
//     parameter API never calls NotifyTray::say(), and say() is the one thing that fires under a
//     hand and not under automation — which is why every storm ever run here came back clean.
//   - test_gui_widgets reaches the tree and injects real press/drag/release, but it never opens a
//     window: nothing is rasterised, nothing is composited, and retained pixels do not exist.
//   - X11 pointer synthesis would bridge the two and is BANNED on this display, which is the real
//     desktop. In-process injection is the way through, and it is the point of this tool.
//
// So this assembles the SHIPPED editor in-process — the same Gui the plugin builds, from the same
// gui.cpp, reached through the test hook that already exists for the routing venue — and then opens
// a real X11 window and embeds it, the way guihost does, so the frames are genuinely drawn,
// composited and retained. Then it drives a real gesture onto the Gamma knob through the frame tree
// and photographs the window around it.
//
// THE PARAMETER LOOP IS CLOSED, and that is not a detail. Knob::announce() reads back
// bridge_.value() — the ENGINE'S echo — to build the string it hands say(). With no engine in the
// process that value never moves, every say() prints the same characters, and the atlas sees no new
// glyphs at all: the probe would drive the gesture path while starving it of the exact churn it was
// built to observe. So the drained queue is applied to a ParamSet and published straight back, which
// is what the plugin does with it, and each drag step therefore produces a NOVEL readout string.
//
// WHAT COUNTS AS A FINDING. Not "the picture changed" — a live editor changes constantly. The
// allowed set is built three ways, none of them by eye, and each of the three was added because the
// two before it were not enough:
//
//   1. MEASURED RESTLESSNESS. The resting editor is photographed repeatedly and every pixel that
//      moves on its own is marked, which absorbs the sky and the ripples as a measurement rather
//      than as an assumption about what animates.
//   2. THE REGISTRY'S OWN ANSWER. WidgetRegistry::watchers(GlobalGamma) is the list of frames that
//      legitimately redraw when this parameter moves — the knob, its readouts — and the notify tray
//      is found in the tree by the property gui.cpp gives it. The regions come from the tree, so
//      they cannot drift away from the layout the way a hand-written rectangle would. A region too
//      large to excuse safely is DROPPED rather than trimmed, and said so out loud.
//   3. WHAT ANSWERS GAMMA, learned by running whole gestures before any are judged. Rule 2 cannot
//      express this: gamma is a macro over every sound, so the surfaces that answer it are larger
//      than anything it is safe to excuse by name. Without this, twenty photographs of a perfectly
//      clean editor were reported as findings because the star field had moved.
//
// And then a FLOOR, measured rather than guessed: one more gesture is run and judged against the
// finished mask, and whatever that clean gesture still leaves is this venue's noise. Three times it
// is the bar. A guessed floor is either too low — every clean cycle a finding, which happened — or
// too high by luck, and nobody can tell which.
//
// Ink over that bar, outside the allowed set and absent from the resting editor, is the photograph.
// It is reported with its bounding box and with the top-left quadrant counted separately, because
// that is where the reported artefact sits and because that quadrant is chrome the editor does not
// repaint. On this display a clean gesture leaves tens of stray pixels over that chrome and the
// injected known-bad leaves tens of thousands.
//
// TWO CADENCES, because they answer different questions and only one of them can be run at a hand's
// speed. `frame` pace captures after every single motion event, so the frame corruption FIRST
// appears in is on disk — at the cost of a drag that takes a second and a half of wall clock,
// because `import` is a process. `human` pace runs the motion events at ~40ms apart, which is a human
// hand, and captures at release and after; it cannot say which frame, but the defect is RETAINED —
// that is its defining property — so a corrupted frame is still corrupt when it is photographed.
// Both are run by default and reported separately.
//
// HOW A RUN OF THIS CAN LIE, and what is done about each:
//   - A frozen or obscured window hands back byte-identical PNGs that read as a clean run. Every
//     capture is hashed and the distinct count is required to exceed one per cycle; a cycle that
//     fails that is reported as INCONCLUSIVE and never as clean.
//   - A sleeping display never presents at all. Run scripts/venue-preflight.sh first — this tool
//     says so in its header and does not pretend to be a substitute for it.
//   - A detector that cannot fire is not a detector. FEATHERS_PROBE_CONTROL deliberately draws ink
//     over the top-left chrome, and the run FAILS if the diff does not catch it. The clean verdict
//     is not to be believed by anyone who has not seen that control go red.
//
// THE REST PHASE, which is the more sensitive of the two detectors and was added last. The rays were seen to
// FLICKER with hands nowhere near the machine, and a retained photograph cannot
// flicker: the artefact is being redrawn every frame by something with per-frame variation. So after
// each cycle the tool watches for two seconds with nothing touching anything and diffs CONSECUTIVE
// frames. Ambient answer: almost nothing. A live-redrawn artefact: every frame. That turns the diff
// into a lock-in amplifier, and the flicker's CADENCE names the redrawer — audio-rate points at the
// scope polyline, a slow decay at the notify tray's fade, a twinkle at the sky.
//
// THE MATRIX this is built to run, from the refined pattern. The rays show up on PATH in Explore
// and on GAMMA in Perform: exactly the two knob×surface pairs where the surface on screen does not
// watch the knob being turned. The other two repaint that surface heavily and would erase a corrupt
// draw before anyone saw it. So the model is that ANY knob gesture makes the bad draw and the
// surface only decides whether it survives — and a hit in one of the MASKED pairs, which per-frame
// mid-gesture capture can reach and a human eye cannot, would settle it in a single run.
//
//   for surface in perform explore; do for knob in gamma path; do ... done; done
//
// Run each combination as a FRESH PROCESS: the first drag on a new editor is the one that puts
// never-before-seen glyphs at a never-before-seen size into the font atlas, which is what makes it
// resize — and an atlas resize moving glyph UVs under already-submitted text quads is the diagnosed
// corruption family. With VISAGE_TRACE_ATLAS=1 the fork prints [VISAGE-ATLAS] lines to stderr
// stamped with CLOCK_REALTIME milliseconds; this tool stamps its rest frames from the same clock, so
// redirecting stderr to a file lines resize events up against frame numbers.
//
// Environment:
//   FEATHERS_PROBE_CYCLES=<n>     gesture cycles to run (default 20)
//   FEATHERS_PROBE_OUT=<dir>      where captures of a finding are written (default ".")
//   FEATHERS_PROBE_WIDTH/HEIGHT   window size (default 1240x700, the editor's logical canvas)
//   FEATHERS_PROBE_CADENCE        frame | human | both (default both, split evenly)
//   FEATHERS_PROBE_KNOB           gamma (default) | path — the other half of the matrix
//   FEATHERS_PROBE_CONTROL=<n>    run the known-bad instead: one cycle, ink injected at frame n
//   FEATHERS_PROBE_GAMMA_BASE=<a,b,..>  bases to HOLD, one per measured cycle, cycling — reached by
//                                 dragging the real Explore gamma corner (see driveGammaBaseTo)
//   FEATHERS_SURFACE=explore      read by gui.cpp itself — the surface half of the matrix
//   FEATHERS_UI_SCALE=<x>         read by gui.cpp itself; 1.75 matches the desktop display
//   VISAGE_TRACE_ATLAS=1          read by the visage fork; atlas lines to stderr, same clock
//
// Needs libX11 and ImageMagick's `import` on PATH.

#include "plugin/gui.h"
#include "plugin/gui_bridge.h"
#include "plugin/params.h"
#include "plugin/sound_gamma.h"
#include "plugin/ui/explore_stage.h"

#include <atomic>
#include <chrono>

#include "plugin/ui/fx_lane.h"
#include "plugin/ui/header_bar.h"
#include "plugin/ui/engine_rail.h"
#include "plugin/ui/knob.h"
#include "plugin/ui/mod_panel.h"
#include "plugin/ui/ui_state.h"
#include "plugin/ui/widget_registry.h"

#include "visage_graphics/canvas.h"
#include "visage_graphics/graphics_utils.h"
#include <visage/ui.h>

#include <X11/Xlib.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include <vector>

using namespace feathers;

namespace {

//---------------------//
// The plugin's side   //
//---------------------//

/// The editor talks to a host through this. Resizes are recorded and answered from the main loop,
/// the way guihost does it and the way a real host must: acting on one from inside the editor's own
/// callback is a different thing entirely.
struct ProbeGuiHost : GuiHost {
	uint32_t requestedWidth = 0;
	uint32_t requestedHeight = 0;
	bool resizePending = false;
	long flushes = 0;

	void guiRequestFlush() noexcept override { ++flushes; }
	void guiRequestResize(uint32_t width, uint32_t height) noexcept override {
		requestedWidth = width;
		requestedHeight = height;
		resizePending = true;
	}
	// No plugin state behind the probe, so the preset browser's capture/load are stubs.
	void guiCaptureState(clap_ostream_t*) noexcept override {}
	bool guiLoadState(clap_istream_t*) noexcept override { return false; }
};

//---------------------//
// The known-bad       //
//---------------------//

/// DELIBERATE INK WHERE INK MAY NOT BE — the control that has to fire before any clean verdict here
/// is worth reading.
///
/// It wears the costume the defect was reported in rather than a convenient one: a filled lavender
/// polygon anchored at the window origin, with a fan of thin strokes radiating from the same point
/// across the chrome. That is what was photographed, and drawing anything easier to detect would be
/// a control for a detector we are not shipping.
///
/// It is only ever added to the tree when the control is being run, so a measured cycle is a cycle
/// against the editor as it ships — an injector sitting dormant in the tree during the real runs
/// would be one more difference between this venue and the real one.
class OriginInk : public visage::Frame {
public:
	bool armed = false;

	void draw(visage::Canvas& canvas) override {
		if (!armed) {
			return;
		}
		// Lavender, the reported colour, at the origin and over whatever the chrome had drawn there.
		canvas.setColor(0xff9b8ec4);
		canvas.triangle(0.0f, 0.0f, width() * 0.62f, height() * 0.20f, width() * 0.18f, height() * 0.74f);
		// And the fan: thin strokes from the same anchor, which is the other half of the costume.
		canvas.setColor(0xffd6cef2);
		for (int spoke = 0; spoke < 9; ++spoke) {
			const float t = static_cast<float>(spoke) / 8.0f;
			canvas.segment(0.0f, 0.0f, width() * (0.25f + 0.7f * t), height() * (0.95f - 0.8f * t),
			               1.5f, false);
		}
	}
};

//---------------------//
// Pixels              //
//---------------------//

/// One captured frame: 8-bit RGB, as `import` writes it in a binary PPM.
struct Image {
	int width = 0;
	int height = 0;
	std::vector<unsigned char> rgb;

	bool valid() const { return width > 0 && height > 0 && rgb.size() == static_cast<size_t>(width) * height * 3; }
};

/// A PHOTOGRAPH OF AN EDITOR IS NOT ONE COLOUR — the standard deviation of luminance, normalised to
/// [0,1], so 0 means every pixel is identical.
///
/// THIS IS THE FINGERPRINT'S BLIND SPOT, not a duplicate of it. `fingerprint` below names a frame so
/// a cycle of IDENTICAL captures can be spotted — a window that was not drawing. It cannot see a
/// window that hands back a DIFFERENT capture which happens to be EMPTY, because a blank frame has a
/// perfectly good name of its own, distinct from its neighbours, so the distinct-count rule APPROVES
/// one.
///
/// WHICH WAY A BLANK FAILS HERE — AND IT IS NOT THE WAY IT FAILS IN `guihost`, which is worth saying
/// plainly because the first version of this comment had it backwards. This probe does not ask
/// "is there ink in the frame"; `judge` counts pixels that DIFFER FROM THE RESTING BASELINE and fall
/// outside the allowed mask. A blank frame differs from the baseline nearly everywhere, so during
/// scoring it is SHOUTED as a huge spurious finding, not swallowed as a clean pass. In `guihost`, by
/// contrast, a blank is a quiet success. Same defect, opposite symptom.
///
/// SO THE FALSE-NEGATIVE PATH RUNS THROUGH CALIBRATION, NOT SCORING, and that is the one worth
/// guarding: the restless mask is built by sampling an untouched editor and marking whatever moves
/// on its own. ONE blank frame among those samples marks essentially every pixel as restless, which
/// makes the mask permissive enough to excuse real ink for the rest of the run — a clean result
/// produced by a mask that forgives everything. (Measured evidence that this did NOT happen in the
/// storms already run: calibration reported 41,638 restless px, 4.8% of a 1240x700 window. A blank
/// among those 40 frames would have put that near 100%.)
///
/// NOT HYPOTHETICAL. Measured on this venue with `guihost`, which captures through the same
/// `import`: 12 runs produced 1 all-black frame — deviation exactly 0, 260 bytes — against ~140 KB
/// and 0.0684 / 0.0695 / 0.0731 for real captures of three different devices.
///
/// WHAT THAT RATE IS AND IS NOT. Each of those 12 was a capture taken shortly after its editor was
/// created, so 1-in-12 is the rate for a FRESH window, and the likely mechanism is a window not yet
/// ready to be grabbed. This probe holds ONE window open across hundreds of captures, so its rate is
/// a different number and an unmeasured one — quite possibly zero after the baseline. Quoting the
/// 8% as if it applied per capture here would be borrowing a figure from the wrong population.
double pixelDeviation(const Image& image) {
	if (!image.valid()) {
		return 0.0;
	}
	const size_t pixels = static_cast<size_t>(image.width) * static_cast<size_t>(image.height);
	double sum = 0.0;
	double sumSquares = 0.0;
	for (size_t i = 0; i < pixels; ++i) {
		// Rec. 601 luma, which is what ImageMagick's -colorspace Gray uses, so the threshold below
		// keeps the meaning it was measured with.
		const double luma = (0.299 * image.rgb[i * 3] + 0.587 * image.rgb[i * 3 + 1]
		                     + 0.114 * image.rgb[i * 3 + 2])
		                    / 255.0;
		sum += luma;
		sumSquares += luma * luma;
	}
	const double mean = sum / static_cast<double>(pixels);
	const double variance = sumSquares / static_cast<double>(pixels) - mean * mean;
	return variance > 0.0 ? std::sqrt(variance) : 0.0;
}

/// MEASURED, NOT CHOSEN: two orders of magnitude below the real floor of 0.068 quoted above, so it
/// answers "uniform" and not "boring". A frame at or under this is not a photograph of an editor.
constexpr double kBlankDeviation = 0.001;

/// FNV-1a over the pixels. Not a checksum for integrity — a name for the frame, so two captures can
/// be told apart without keeping both. A cycle whose captures all share one name photographed a
/// window that was not drawing.
uint64_t fingerprint(const Image& image) {
	uint64_t hash = 1469598103934665603ULL;
	for (const unsigned char byte : image.rgb) {
		hash ^= byte;
		hash *= 1099511628211ULL;
	}
	return hash;
}

/// PPM's header is whitespace-separated with `#` comments anywhere in it, so it is parsed rather
/// than assumed. A capture that arrives truncated — which is what a half-written file from a killed
/// `import` looks like — fails here instead of being compared as if it were a frame.
bool readPpm(const std::string& path, Image& out) {
	FILE* file = std::fopen(path.c_str(), "rb");
	if (file == nullptr) {
		return false;
	}
	const auto token = [&file](int& value) {
		int character = 0;
		do {
			character = std::fgetc(file);
			if (character == '#') {
				while (character != '\n' && character != EOF) {
					character = std::fgetc(file);
				}
			}
		} while (character == ' ' || character == '\t' || character == '\n' || character == '\r');
		if (character < '0' || character > '9') {
			return false;
		}
		value = 0;
		while (character >= '0' && character <= '9') {
			value = value * 10 + (character - '0');
			character = std::fgetc(file);
		}
		return true;
	};
	char magic[3] = {};
	int maximum = 0;
	if (std::fread(magic, 1, 2, file) != 2 || magic[0] != 'P' || magic[1] != '6' || !token(out.width)
	    || !token(out.height) || !token(maximum) || maximum != 255) {
		std::fclose(file);
		return false;
	}
	const size_t bytes = static_cast<size_t>(out.width) * out.height * 3;
	out.rgb.resize(bytes);
	const bool whole = std::fread(out.rgb.data(), 1, bytes, file) == bytes;
	std::fclose(file);
	return whole && out.valid();
}

/// The editor's own window, not the container we handed it. visage reparents its window into ours,
/// and capturing the parent picks up the wrong thing — guihost learned this first.
Window captureTarget(Display* display, Window parent) {
	Window root = 0;
	Window above = 0;
	Window* kids = nullptr;
	unsigned int count = 0;
	Window target = parent;
	if (XQueryTree(display, parent, &root, &above, &kids, &count) != 0 && count > 0) {
		target = kids[0];
	}
	if (kids != nullptr) {
		XFree(kids);
	}
	return target;
}

//---------------------//
// Regions             //
//---------------------//

/// A rectangle in CAPTURED PIXELS, which is not the same space the frame tree speaks in — the tree
/// is logical and the window is scaled by the display's content scale.
struct Rect {
	int left = 0;
	int top = 0;
	int right = 0;
	int bottom = 0;
};

/// Every pixel that is allowed to change, as one mask. Two very different things end up in here —
/// what was measured to move on its own, and what the registry says is entitled to move — and they
/// are combined rather than kept apart because the question each frame asks is a single one: may
/// this pixel have changed?
struct AllowedMask {
	int width = 0;
	int height = 0;
	std::vector<unsigned char> allowed;

	bool at(int x, int y) const {
		return allowed[static_cast<size_t>(y) * width + x] != 0;
	}
	void add(const Rect& rect) {
		for (int y = std::max(0, rect.top); y < std::min(height, rect.bottom); ++y) {
			for (int x = std::max(0, rect.left); x < std::min(width, rect.right); ++x) {
				allowed[static_cast<size_t>(y) * width + x] = 1;
			}
		}
	}
};

/// Whether two captures differ at a pixel, by more than the renderer's own noise.
///
/// The tolerance is not a fudge factor for a detector that reports too much: a GPU compositing the
/// same scene twice is entitled to a least-significant bit, and the artefact being hunted is opaque
/// fills and strokes, which clear this by two orders of magnitude.
constexpr int kChannelTolerance = 12;

bool differs(const Image& a, const Image& b, size_t index) {
	return std::abs(static_cast<int>(a.rgb[index]) - static_cast<int>(b.rgb[index]))
	           > kChannelTolerance
	       || std::abs(static_cast<int>(a.rgb[index + 1]) - static_cast<int>(b.rgb[index + 1]))
	              > kChannelTolerance
	       || std::abs(static_cast<int>(a.rgb[index + 2]) - static_cast<int>(b.rgb[index + 2]))
	              > kChannelTolerance;
}

/// What a frame is accused of: ink that is neither in the resting editor nor anywhere it is allowed
/// to be.
struct Verdict {
	long stray = 0;
	long strayTopLeft = 0;
	/// Every pixel that moved, excused or not. Carried because "the detector saw nothing" and "the
	/// thing it was meant to see was excused" are the same verdict and very different bugs — the
	/// first run of this tool was the second one, and this number is what named it.
	long changed = 0;
	Rect bounds { 1 << 30, 1 << 30, -1, -1 };

	bool fired() const;
};

/// The floor a verdict has to clear, below which nothing is called.
///
/// NOT A CONSTANT, because this venue's noise is not a constant. Antialiasing leaks a pixel or two
/// past a padded rectangle, and the Perform star field is stochastic in gamma — a different scatter
/// lights up at each value — so a handful of stars survive even a mask learned from whole gestures.
/// A guessed floor is either too low (and every clean cycle is a finding, which happened) or too
/// high by luck. So the floor starts here and is RAISED to what this display, at this size, actually
/// produces on a gesture known to be clean — measured below, and printed, so the number a verdict
/// was judged against is in the log beside the verdict.
constexpr long kStrayFloorMinimum = 30;
long gStrayFloor = kStrayFloorMinimum;

/// The rest phase's own floor, measured the same way and kept separate because it is a different
/// measurement: frame-to-frame change while nobody is touching anything, not change since the
/// resting editor. The ambient answer is far smaller, so sharing the gesture's floor would throw
/// away most of this detector's sensitivity — which is the whole reason it exists.
constexpr long kRestFloorMinimum = 8;
long gRestFloor = kRestFloorMinimum;

/// How long to watch after each cycle, and how often to look.
///
/// Two seconds at roughly ten frames a second: long enough for a slow decay to show its shape, often
/// enough that a per-frame redraw cannot hide between looks. `import` is a process, so this is the
/// honest ceiling on the rate — a genuinely audio-rate flicker will alias, but it will still be SEEN,
/// and being seen is what decides whether there is anything to chase.
constexpr int kRestFrames = 20;
constexpr int kRestFramePeriodMs = 100;

/// CLOCK_REALTIME in milliseconds — deliberately the SAME clock and the same units as the fork's
/// [VISAGE-ATLAS] lines (visage_graphics/graphics_utils.h traceMilliseconds). That is what lets an
/// atlas resize be placed against a frame number rather than merely known to have happened in the
/// same run, which the hunt now needs: the corruption is texture-side, so the frame at and just
/// after a repack is the frame that matters.
long long traceMilliseconds() {
	struct timespec ts {};
	clock_gettime(CLOCK_REALTIME, &ts);
	return static_cast<long long>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

bool Verdict::fired() const {
	return stray >= gStrayFloor;
}

Verdict judge(const Image& frame, const Image& resting, const AllowedMask& mask) {
	Verdict verdict;
	if (!frame.valid() || frame.width != resting.width || frame.height != resting.height) {
		return verdict;
	}
	const int halfWidth = frame.width / 2;
	const int halfHeight = frame.height / 2;
	for (int y = 0; y < frame.height; ++y) {
		for (int x = 0; x < frame.width; ++x) {
			const size_t index = (static_cast<size_t>(y) * frame.width + x) * 3;
			if (!differs(frame, resting, index)) {
				continue;
			}
			++verdict.changed;
			if (mask.at(x, y)) {
				continue;
			}
			++verdict.stray;
			if (x < halfWidth && y < halfHeight) {
				++verdict.strayTopLeft;
			}
			verdict.bounds.left = std::min(verdict.bounds.left, x);
			verdict.bounds.top = std::min(verdict.bounds.top, y);
			verdict.bounds.right = std::max(verdict.bounds.right, x);
			verdict.bounds.bottom = std::max(verdict.bounds.bottom, y);
		}
	}
	return verdict;
}

/// Grow the restless mask by a few pixels in each direction.
///
/// A pixel on the boundary of something that animates is only sometimes covered by it, and a
/// one-pixel halo around every moving thing would otherwise be reported every frame. Done as two
/// separable passes because a square kernel is what is wanted and two passes is what it costs.
void dilate(AllowedMask& mask, int radius) {
	std::vector<unsigned char> pass(mask.allowed.size(), 0);
	for (int y = 0; y < mask.height; ++y) {
		for (int x = 0; x < mask.width; ++x) {
			unsigned char hit = 0;
			for (int dx = -radius; dx <= radius && hit == 0; ++dx) {
				const int sample = x + dx;
				if (sample >= 0 && sample < mask.width) {
					hit = mask.allowed[static_cast<size_t>(y) * mask.width + sample];
				}
			}
			pass[static_cast<size_t>(y) * mask.width + x] = hit;
		}
	}
	for (int y = 0; y < mask.height; ++y) {
		for (int x = 0; x < mask.width; ++x) {
			unsigned char hit = 0;
			for (int dy = -radius; dy <= radius && hit == 0; ++dy) {
				const int sample = y + dy;
				if (sample >= 0 && sample < mask.height) {
					hit = pass[static_cast<size_t>(sample) * mask.width + x];
				}
			}
			mask.allowed[static_cast<size_t>(y) * mask.width + x] = hit;
		}
	}
}

/// Where a frame sits, in captured pixels, padded.
///
/// The padding is for the same reason the dilation is: a control's own antialiased edge is not
/// reliably inside the rectangle its layout gives it.
Rect rectOf(const visage::Frame* frame, float scale, int pad) {
	const visage::Point at = frame->positionInWindow();
	return { static_cast<int>(std::floor(at.x * scale)) - pad,
		     static_cast<int>(std::floor(at.y * scale)) - pad,
		     static_cast<int>(std::ceil((at.x + frame->width()) * scale)) + pad,
		     static_cast<int>(std::ceil((at.y + frame->height()) * scale)) + pad };
}

/// A widget of a given type anywhere under a frame, for the two the base sweep has to drive.
///
/// BY TYPE, not by geometry, and that is the honest choice here: the tray above is found by BEHAVIOUR
/// because naming its type would drag its header in for no gain, whereas the gamma corner's hit box
/// and the surface switch's halves are things only their own classes can answer — partBounds() and
/// surfaceAt() exist precisely so a venue can ask instead of restating a layout that will move.
template <typename T>
T* findInTree(visage::Frame* frame) {
	if (T* self = dynamic_cast<T*>(frame); self != nullptr) {
		return self;
	}
	for (visage::Frame* child : frame->children()) {
		if (T* found = findInTree<T>(child); found != nullptr) {
			return found;
		}
	}
	return nullptr;
}

/// One press/drag/release, routed the way visage's WindowEventHandler routes it: the frame that was
/// pressed keeps every drag and the release, even once the pointer has left it.
visage::MouseEvent eventOn(visage::Frame* frame, visage::Point local, bool isDown) {
	visage::MouseEvent event;
	event.window_position = local + frame->positionInWindow();
	event.position = local;
	event.event_frame = frame;
	event.button_id = visage::kMouseButtonLeft;
	event.button_state = isDown ? visage::kMouseButtonLeft : visage::kMouseButtonNone;
	event.is_down = isDown;
	event.repeat_click_count = 1;
	return event;
}

/// The notify tray, found by the property gui.cpp gives it rather than by type.
///
/// It is added to the root as a direct child and told to ignore mouse events — "it is a readout, not
/// a control" — and it is the only child of the root for which that is true. Finding it that way
/// keeps this file from needing the widget's header, and makes the search fail loudly (null, and a
/// printed warning at the call site) if that ever stops being true, rather than silently matching
/// something else.
visage::Frame* findNotifyTray(visage::Frame* root) {
	visage::Frame* found = nullptr;
	for (visage::Frame* child : root->children()) {
		if (child->ignoresMouseEvents()) {
			if (found != nullptr) {
				return nullptr;
			}
			found = child;
		}
	}
	return found;
}

} // namespace

int main() {
	const int cycles = std::getenv("FEATHERS_PROBE_CYCLES") != nullptr
	                       ? std::atoi(std::getenv("FEATHERS_PROBE_CYCLES"))
	                       : 20;
	const std::string outDir = std::getenv("FEATHERS_PROBE_OUT") != nullptr
	                               ? std::getenv("FEATHERS_PROBE_OUT")
	                               : ".";
	const char* controlAt = std::getenv("FEATHERS_PROBE_CONTROL");
	const std::string cadence = std::getenv("FEATHERS_PROBE_CADENCE") != nullptr
	                                ? std::getenv("FEATHERS_PROBE_CADENCE")
	                                : "both";
	uint32_t width = std::getenv("FEATHERS_PROBE_WIDTH") != nullptr
	                     ? static_cast<uint32_t>(std::atoi(std::getenv("FEATHERS_PROBE_WIDTH")))
	                     : Gui::kLogicalWidth;
	uint32_t height = std::getenv("FEATHERS_PROBE_HEIGHT") != nullptr
	                      ? static_cast<uint32_t>(std::atoi(std::getenv("FEATHERS_PROBE_HEIGHT")))
	                      : Gui::kLogicalHeight;

	XInitThreads();
	Display* display = XOpenDisplay(nullptr);
	if (display == nullptr) {
		std::printf("XOpenDisplay failed — is DISPLAY set?\n");
		return 2;
	}

	ProbeGuiHost host;
	ParamBridge bridge;
	ParamSet params;
	bridge.publish(params);
	bridge.refresh();

	Gui gui(host, bridge);
	if (!Gui::isAvailable() || !gui.create(CLAP_WINDOW_API_X11, false)) {
		std::printf("gui.create failed — no X11 backend in this build\n");
		return 2;
	}
	gui.setSize(width, height);

	// The container, unmapped across set_parent the way a real host leaves it — see guihost, which
	// spent a release cycle mapping it early and excusing an editor that never opened in Bitwig.
	const int screen = DefaultScreen(display);
	Window parent = XCreateSimpleWindow(display, RootWindow(display, screen), 0, 0, width, height, 0,
	                                    BlackPixel(display, screen), BlackPixel(display, screen));
	XStoreName(display, parent, "feathers windowed-probe");
	XFlush(display);

	clap_window window {};
	window.api = CLAP_WINDOW_API_X11;
	window.x11 = parent;
	if (!gui.setParent(&window)) {
		std::printf("gui.setParent failed\n");
		return 2;
	}
	XMapWindow(display, parent);
	XRaiseWindow(display, parent);
	XSetInputFocus(display, parent, RevertToParent, CurrentTime);
	XFlush(display);

	// What a host's main loop does, minus the plugin: give the editor its thread back and drain the
	// X connection. The parameter loop is closed here too — see the note at the top of this file;
	// without it the readout never changes and the gesture drives no glyph churn at all.
	// THE STORM — a hand ON TOP OF automation, which is the state the reports actually come from and
	// which no venue could produce until this one.
	//
	// guihost can stream automation but cannot touch a knob; the widget suite can touch a knob but
	// draws nothing. This process holds both roads, so it can do the thing neither could: churn
	// parameters the way a host streams them WHILE a real gesture is in flight. That matters twice
	// over — it is the reported condition, and eight sound cards rebuilding their readouts on eight
	// incommensurate rates is far more text turnover than one knob's readout, which is the pressure
	// an atlas actually feels.
	//
	// IT RUNS FROM STARTUP, through calibration and every gesture alike, rather than being switched
	// on for the measured cycles. A storm that starts after the mask is learned would light up every
	// surface it touches as stray ink; a storm the calibration lived through is a storm the mask
	// knows about, and the chrome is still chrome.
	//
	// The ids are found BY NAME, the way guihost finds them, so a renumbered parameter downgrades
	// the run visibly instead of silently churning the wrong thing.
	const char* knobName = std::getenv("FEATHERS_PROBE_KNOB");
	const bool dragPath = knobName != nullptr && std::strcmp(knobName, "path") == 0;
	const clap_id targetId = static_cast<clap_id>(dragPath ? ParamId::Path : ParamId::GlobalGamma);

	const char* stormWhich = std::getenv("FEATHERS_PROBE_STORM");
	const bool storming = stormWhich != nullptr;
	if (stormWhich == nullptr) {
		stormWhich = "";
	}
	std::vector<clap_id> stormIds;
	if (storming) {
		for (const ParamDesc& desc : paramDescs()) {
			const std::string name = desc.name;
			const bool perSoundGamma = name.size() == 7 && name.compare(1, 6, " Gamma") == 0
			                           && name[0] >= 'A' && name[0] <= 'H';
			// NEVER THE KNOB BEING DRAGGED. The storm writes every slice, so leaving the target in it
			// would have automation fighting the hand for the same parameter — the gesture's own
			// writes overwritten a few milliseconds later, the readout showing the storm's value, and
			// a drag that reports itself as working while measuring nothing it did.
			if (static_cast<clap_id>(desc.id) == targetId) {
				continue;
			}
			// WHICH HALF OF THE STORM, so the ingredient can be bisected rather than argued about.
			// The two halves drive different roads: the per-sound gammas rebuild eight cards' worth
			// of READOUT TEXT, while Path/Pos/Weather move the GEOMETRY the surfaces draw. The wedge
			// appeared with both running; which half it needs names which road to audit.
			const bool travelParam = name == "Path" || name == "Pos X" || name == "Pos Y"
			                         || name == "Weather";
			const bool wanted = std::strcmp(stormWhich, "gammas") == 0   ? perSoundGamma
			                    : std::strcmp(stormWhich, "travel") == 0 ? travelParam
			                                                            : (perSoundGamma || travelParam);
			if (wanted) {
				stormIds.push_back(static_cast<clap_id>(desc.id));
			}
		}
		std::printf("storm:    churning %zu parameter(s) continuously (%s), under everything else\n",
		            stormIds.size(), stormWhich[0] == '\0' ? "all" : stormWhich);
	}
	long stormStep = 0;

	// Which frames to keep verbatim, as <cycle>:<lo>-<hi>. Off unless asked for.
	int keepCycle = -999;
	int keepLow = 0;
	int keepHigh = -1;
	if (const char* keep = std::getenv("FEATHERS_PROBE_KEEP")) {
		if (std::sscanf(keep, "%d:%d-%d", &keepCycle, &keepLow, &keepHigh) != 3) {
			keepCycle = -999;
			std::printf("keep:     FEATHERS_PROBE_KEEP must read <cycle>:<lo>-<hi> — ignoring \"%s\"\n",
			            keep);
		}
		else {
			std::printf("keep:     saving cycle %d frames %d..%d verbatim\n", keepCycle, keepLow,
			            keepHigh);
		}
	}

	long valueWrites = 0;
	const auto pump = [&](int milliseconds) {
		for (int slice = 0; slice < std::max(1, milliseconds / 4); ++slice) {
			gui.processEvents();
			bool moved = false;
			if (storming && !stormIds.empty()) {
				// Incommensurate rates, so the cards never rebuild in step with one another: eight
				// readouts changing together is one repaint, and eight changing at eight different
				// moments is eight — which is the damage pattern that multiplies pieces.
				const double t = static_cast<double>(stormStep++) / 60.0;
				for (size_t which = 0; which < stormIds.size(); ++which) {
					const ParamDesc* desc = findParam(stormIds[which]);
					if (desc == nullptr) {
						continue;
					}
					const double rate = 0.7 + 0.37 * static_cast<double>(which);
					const double swing = 0.5 + 0.5 * std::sin(t * 2.0 * 3.14159265358979 * rate
					                                          + static_cast<double>(which));
					params.setBase(stormIds[which], desc->min + (desc->max - desc->min) * swing);
				}
				moved = true;
			}
			bridge.drain([&](const UiParamEvent& event) {
				if (event.kind == UiParamEvent::Kind::Value) {
					params.setBase(event.id, event.value);
					moved = true;
					++valueWrites;
				}
			});
			if (moved) {
				bridge.publish(params);
				bridge.refresh();
			}
			while (XPending(display) > 0) {
				XEvent event;
				XNextEvent(display, &event);
			}
			usleep(4000);
		}
	};

	// SETTLE THE SIZE, AND TELL THE EDITOR WHAT IT ACTUALLY GOT.
	//
	// A resize is not a number the host grants, it is a negotiation the window manager gets the last
	// word in. At scale 1.75 the editor asks for a window bigger than this 1920x1080 screen, the WM
	// clamps it, and a host that then reports back the size it ASKED for has told the editor
	// something false: the run that did that produced one distinct frame in twenty-five gestures — a
	// window that never drew again, which the capture hashes caught and which would otherwise have
	// read as a beautifully clean result.
	//
	// So the real geometry is read back from X after each round, and that is what setSize is told.
	// Several rounds, because settling to a size can change the size that is wanted.
	pump(400);
	for (int round = 0; round < 4 && host.resizePending; ++round) {
		host.resizePending = false;
		// Never larger than the screen: a window the WM has to clamp is a window whose size the
		// editor and the server disagree about, which is the disagreement above.
		const uint32_t askedWidth = std::min<uint32_t>(host.requestedWidth,
		                                              static_cast<uint32_t>(DisplayWidth(display, screen)));
		const uint32_t askedHeight = std::min<uint32_t>(host.requestedHeight,
		                                               static_cast<uint32_t>(DisplayHeight(display, screen)));
		XResizeWindow(display, parent, askedWidth, askedHeight);
		XSync(display, False);

		Window ignoredRoot = 0;
		int ignoredX = 0;
		int ignoredY = 0;
		unsigned int actualWidth = askedWidth;
		unsigned int actualHeight = askedHeight;
		unsigned int ignoredBorder = 0;
		unsigned int ignoredDepth = 0;
		XGetGeometry(display, parent, &ignoredRoot, &ignoredX, &ignoredY, &actualWidth, &actualHeight,
		             &ignoredBorder, &ignoredDepth);
		width = actualWidth;
		height = actualHeight;
		gui.setSize(width, height);
		std::printf("editor:   asked for %ux%u, clamped to %ux%u, and told the truth about it\n",
		            host.requestedWidth, host.requestedHeight, width, height);
		pump(400);
	}
	// Raised again AFTER the size settles: a window that changed size may have been restacked, and a
	// capture of something sitting on top of it is a capture of something else.
	XRaiseWindow(display, parent);
	XSetInputFocus(display, parent, RevertToParent, CurrentTime);
	XFlush(display);
	pump(1200);

	visage::Frame* root = gui.root();
	const ui::WidgetRegistry* widgets = gui.widgets();
	if (root == nullptr || widgets == nullptr) {
		std::printf("no assembled tree — gui.root()/widgets() are null\n");
		return 2;
	}

	//-------------------------------------------------------------------------------------------//
	// POPOUT MODE — the crash sequence, in the one topology that can express it                //
	//-------------------------------------------------------------------------------------------//
	//
	// FEATHERS_PROBE_POPOUT=1 replaces the gesture storm with the sequence off the Bitwig log:
	// fold a device's card, then open its popout. The host died three milliseconds after the
	// popout's window textures were created, and the engine log's last four lines are that
	// creation.
	//
	// WHY HERE AND NOT IN test_gui_windowed, which already opens real popouts and passes. Those
	// cases build a TOP-LEVEL editor, and a top-level window takes `X11Connection::globalInstance()`
	// — the same connection the popout takes. A PLUGIN's editor takes the embedded constructor,
	// which makes a PRIVATE `X11Connection`. So in the suite both windows share one connection and
	// in Bitwig they do not, and the suite cannot express the difference at all. This process
	// reaches the editor through `gui.setParent`, which is the embedded road, so it has the same two
	// connections a host does.
	//
	// A VENUE MUST BE SHOWN CAPABLE OF EXPRESSING THE OUTCOME. That is why this prints what it
	// reached at every step rather than only its verdict: a run that never found the card, or never
	// found a device with a panel, would otherwise report "survived" for a sequence it never drove.
	if (std::getenv("FEATHERS_PROBE_POPOUT") != nullptr) {
		auto* lane = findInTree<ui::FxLane>(root);
		if (lane == nullptr) {
			std::printf("popout:   NO FX LANE IN THE TREE — nothing was driven\n");
			return 2;
		}
		const char* wantTag = std::getenv("FEATHERS_PROBE_POPOUT_TAG");
		std::string tag = wantTag != nullptr ? wantTag : "OWLTT";
		const size_t slot = bridge.addFxDevice(tag.c_str());
		std::printf("popout:   added %s at slot %zu\n", tag.c_str(), slot);
		lane->resized();
		lane->scrollCardIntoView(slot);
		lane->resized();
		pump(400);

		ui::FxCard* card = lane->cardAt(slot);
		if (card == nullptr) {
			std::printf("popout:   NO CARD AT SLOT %zu — nothing was driven\n", slot);
			return 2;
		}

		// MODULATION MAPPED ONTO THE DEVICE'S OWN CONTROL, because a mapping may have been live
		// when the reorder crash fired, and this file's own header says the defect that
		// has arrived THREE times here is a modulation watcher list outliving the frames on it:
		// `Knob::setModulation` puts every modulatable knob on `ModState`'s FrameWatchers, a second
		// list joined by children the popout host never sees. A popout cycle with nothing armed
		// cannot touch that list at all, so every clean run so far was blind to it by construction.
		if (std::getenv("FEATHERS_PROBE_POPOUT_MOD") != nullptr) {
			// THE FIRST CONTROL THAT IS ACTUALLY A DESTINATION, found rather than assumed — control 0
			// is the enable switch on every fx device, and a stepped Choice is not modulatable. The
			// first draft mapped control 0, got no column, and would have reported a clean run for a
			// mapping it never made.
			// BY ENDPOINT (kill-lanes step 2), and asked of the one authority rather than of a column.
			const DeviceInstanceId device = bridge.fxDeviceAt(slot);
			const backplane::Source macro1 = backplane::Source::core(0);
			backplane::Destination into {};
			bool found = false;
			const device::Descriptor* desc = card->descriptor();
			for (size_t c = 0; desc != nullptr && c < desc->params.size(); ++c) {
				if (bridge.mayRoute(macro1, backplane::Destination::control(device, c))) {
					into = backplane::Destination::control(device, c);
					found = true;
					break;
				}
			}
			if (!found) {
				std::printf("popout:   NO ROUTABLE CONTROL for slot %zu — mapping not driven\n", slot);
				return 2;
			}
			bridge.setRouteAmount(macro1, into, 0.375f);
			std::printf("popout:   mapped Macro 1 -> %s control %u (device %u, amount %.3f)\n", tag.c_str(),
			            into.index, device, static_cast<double>(bridge.routeAmount(macro1, into)));
			pump(200);
		}
		std::printf("popout:   card at (%.0f,%.0f) %.0fx%.0f\n", card->positionInWindow().x,
		            card->positionInWindow().y, card->width(), card->height());

		const auto clickAt = [&](visage::Point where, const char* what) {
			visage::Frame* down = root->frameAtPoint(where);
			if (down == nullptr) {
				std::printf("popout:   %s -> NO FRAME AT (%.0f,%.0f)\n", what, where.x, where.y);
				return;
			}
			const auto ev = [&](bool isDown) {
				visage::MouseEvent e;
				e.window_position = where;
				e.position = where - down->positionInWindow();
				e.event_frame = down;
				e.button_id = visage::kMouseButtonLeft;
				e.button_state = isDown ? visage::kMouseButtonLeft : visage::kMouseButtonNone;
				e.is_down = isDown;
				e.repeat_click_count = 1;
				return e;
			};
			std::printf("popout:   %s at (%.0f,%.0f)\n", what, where.x, where.y);
			std::fflush(stdout);
			down->processMouseDown(ev(true));
			down->processMouseUp(ev(false));
		};

		// THE FOLD FIRST, because that is the order the log records: the card's graph went
		// visible=1 then visible=0, and the popout came 2.7 seconds later.
		const bool foldFirst = std::getenv("FEATHERS_PROBE_POPOUT_NOFOLD") == nullptr;
		if (foldFirst) {
			clickAt(card->positionInWindow()
			            + visage::Point { card->width() * 0.25f, ui::FxCard::kSlotHeadHeight * 0.5f },
			        "fold (left quarter of the head)");
			pump(600);
			std::printf("popout:   card height after fold %.0f\n", card->height());
		}

		// AND THEN THE PANEL. The expand button is inboard of the enable square by the geometry
		// every card shares; derived rather than typed so a card that moves it moves this too.
		const visage::Bounds sw = card->enableBounds();
		if (sw.width() <= 0.0f) {
			std::printf("popout:   %s DECLARES NO ENABLE, so no expand button is derivable\n",
			            tag.c_str());
			return 2;
		}
		const float size = sw.height();
		const visage::Point expandAt = card->positionInWindow()
		                               + visage::Point { sw.x() - 6.0f - size * 0.5f,
			                                             sw.y() + size * 0.5f };
		// AND THEN OPEN AND CLOSE IT REPEATEDLY, because ONE cycle is not what was run. The crash
		// log shows the popout's textures taking handles 6 and 7; a first open in a fresh process
		// takes 0 and 1. Those numbers are the session's churn made visible, and they are the one
		// measurable difference between the run that died and the run that did not — so the cycle
		// count is the variable to sweep rather than a number to pick.
		const int cycles = std::getenv("FEATHERS_PROBE_POPOUT_CYCLES") != nullptr
		                       ? std::atoi(std::getenv("FEATHERS_PROBE_POPOUT_CYCLES"))
		                       : 1;
		for (int cycle = 0; cycle < std::max(1, cycles); ++cycle) {
			std::printf("popout:   --- cycle %d ---\n", cycle);
			std::fflush(stdout);
			clickAt(expandAt, "expand");
			std::fflush(stdout);
			pump(500);
			// AND FOLD/UNFOLD BETWEEN THEM, because the fold is half of what was being done and it is
			// what moves the button under the pointer.
			if (foldFirst) {
				clickAt(card->positionInWindow()
				            + visage::Point { card->width() * 0.25f,
					                          ui::FxCard::kSlotHeadHeight * 0.5f },
				        "fold toggle");
				pump(200);
			}
			clickAt(expandAt, "expand (close)");
			pump(300);
			std::printf("popout:   cycle %d survived\n", cycle);
			std::fflush(stdout);
		}
		std::printf("popout:   SURVIVED all %d cycles\n", std::max(1, cycles));
		return 0;
	}

	//-------------------------------------------------------------------------------------------//
	// RAIL CAPTURE: a picture of the collapsed rails, and nothing else                           //
	//-------------------------------------------------------------------------------------------//
	//
	// WHY IT LIVES HERE. #48 gives the Engine rail and the modulator strip a closed form — 34px, an
	// upright label, a chevron — and none of that is geometry a test can judge: the questions are
	// whether the rotated label is legible, whether it starts at the top, and whether the chevron
	// reads as a different mark from the ones beside it. Those need a photograph, and the venues that
	// take photographs (guihost, bitwighost) hold the plugin across the CLAP boundary and cannot
	// reach a panel to close it. This process already assembles the shipped editor AND owns a camera,
	// so it is the only place the picture can be taken at all.
	//
	// It short-circuits before the storm and the calibration: this mode is a portrait, not a hunt.
	//
	//   FEATHERS_PROBE_RAILS=mod|engine|both|open   which rails to close before the shutter
	if (const char* rails = std::getenv("FEATHERS_PROBE_RAILS")) {
		const std::string which = rails;
		auto* strip = findInTree<ui::ModPanel>(root);
		auto* engineRail = findInTree<ui::EngineRail>(root);
		auto* lane = findInTree<ui::FxLane>(root);
		if (strip == nullptr || engineRail == nullptr || lane == nullptr) {
			std::printf("rails:    no ModPanel/EngineRail in the tree\n");
			return 2;
		}
		// FOLDS TOO, since #59: `folds` mixes them — two cards folded, two not, the rail open — and
		// `folds-closed` asks the question that only a picture can answer, which is what a folded card
		// looks like when the rail it lives in is itself closed (nothing: the rail wins, and the point
		// is to SEE that the two collapses do not fight).
		const bool folding = which == "folds" || which == "folds-closed";
		strip->setCollapsed(which == "mod" || which == "both" || which == "folds-closed");
		engineRail->setCollapsed(which == "engine" || which == "both");
		if (folding) {
			// THE FIRST TWO DEVICE CARDS, by their devices' first outputs (kill-lanes step 2). This asked
			// for flat sources 1 and 2, which have been MACROS since the device sources moved behind them —
			// the header row, which never folds — so `folds` had been folding nothing.
			strip->setCardMinimised(backplane::Source::output(bridge.rackDeviceAt(0), 0), true);
			strip->setCardMinimised(backplane::Source::output(bridge.rackDeviceAt(1), 0), true);
		}
		// THE FX LANE, which ships closed — `fx` opens it, and `all-closed` is the maximal-stage
		// arrangement with every rail on both sides put away.
		lane->setCollapsed(which != "fx" && which != "everything");
		if (which == "all-closed") {
			strip->setCollapsed(true);
			engineRail->setCollapsed(true);
		}
		if (which == "everything") {
			strip->setCollapsed(false);
			engineRail->setCollapsed(false);
		}
		pump(600);

		const Window shot = captureTarget(display, parent);
		char command[512];
		const std::string out = outDir + "/rails_" + which + ".png";
		std::snprintf(command, sizeof(command), "import -depth 8 -window 0x%lx %s",
		              static_cast<unsigned long>(shot), out.c_str());
		if (std::system(command) != 0) {
			std::printf("rails:    import failed on 0x%lx\n", static_cast<unsigned long>(shot));
			return 2;
		}
		// The measurement beside the picture, so a capture that looks right for the wrong reason —
		// a rail that never closed, a window that never resized — cannot pass as evidence.
		std::printf("rails:    %s — Engine rail %.0fpx %s, strip %.0fpx %s, stage from x=%.0f\n",
		            which.c_str(), engineRail->width(), engineRail->collapsed() ? "closed" : "open",
		            strip->width(), strip->collapsed() ? "closed" : "open",
		            engineRail->width() + strip->width());
		std::printf("lane:     %.0fpx %s, its inboard edge at x=%.0f\n", lane->width(),
		            lane->collapsed() ? "closed" : "open", lane->x());
		// THE MACRO ROW AND THE DEVICE CARDS, which is what the strip is made of since the macros
		// became a header row rather than four cards. Reported separately because they are different
		// kinds of thing: the row is permanent and never folds, a device card does both.
		std::printf("macros:   one row, %.0fpx\n", ui::ModPanel::kMacroRowHeight);
		std::printf("cards:    ");
		for (size_t slot = 0; slot < strip->deviceCount(); ++slot) {
			// THE DEVICE AT THIS POSITION, BY ID, AND ITS FIRST OUTPUT (kill-lanes step 2) — a source names
			// its device, not a slot in a flat space.
			const backplane::Source source = backplane::Source::output(bridge.rackDeviceAt(slot), 0);
			std::printf("%zu=%.0fpx%s ", slot, strip->sourceHeight(source),
			            strip->cardMinimised(source) ? " folded" : "");
		}
		std::printf("\n");
		std::printf("rails:    wrote %s\n", out.c_str());
		return 0;
	}

	//-------------------------------------------------------------------------------------------//
	// THE GAMMA BASE, REACHED THE WAY A HAND REACHES IT                                         //
	//-------------------------------------------------------------------------------------------//
	//
	// WHY A GESTURE AND NOT A NUMBER. The base is the UNBOUNDED coarse half of gamma — the thing the
	// Explore corner accumulates, the half that has no parameter, no id and therefore no env handle.
	// Every arm this tool has ever run held it at zero, so a defect that needs a base of eight has
	// never been in front of the camera. The reachable shortcut would be to call the bridge's
	// setSoundGamma directly, and it is the wrong instrument for the same reason my predecessor's
	// gesture model was: `nudgeSelectedGamma` opens a gesture, writes through a pure function on the
	// state of record, and asks for a redraw, while `grabbed_ == Part::Gamma` holds the view still for
	// the whole drag — a shortcut reproduces the VALUE and skips the code path, which is precisely the
	// class of error that reported 1.2% void where a hand gets 100%.
	//
	// So this presses the real corner, at the box the stage itself reports, and drags it. The rate is
	// the widget's own — kGammaTurn of base per kEncoderTravel of pixels — so the pixel distance is
	// arithmetic, but the ACHIEVED base is read back from the bridge and printed, because a corner
	// whose hit box moved would otherwise drag nothing and say nothing.
	std::vector<double> baseSweep;
	if (const char* text = std::getenv("FEATHERS_PROBE_GAMMA_BASE")) {
		std::string field;
		for (const char* walk = text;; ++walk) {
			if (*walk == ',' || *walk == '\0') {
				if (!field.empty()) {
					baseSweep.push_back(std::atof(field.c_str()));
					field.clear();
				}
				if (*walk == '\0') {
					break;
				}
				continue;
			}
			field.push_back(*walk);
		}
	}

	ui::ExploreStage* exploreStage = findInTree<ui::ExploreStage>(root);
	ui::HeaderBar* header = findInTree<ui::HeaderBar>(root);

	// The switch is clicked, not called: mode.show() would move the surface without proving the header
	// can be reached, and the click is a gesture made between every one of these arms anyway.
	// The hitting point is SEARCHED rather than computed, because modeBounds() is private and a
	// restated rectangle is a rectangle that goes stale — hitsMode()/surfaceAt() are public for this.
	const auto clickSurface = [&](ui::StageMode::Surface wanted) -> bool {
		if (header == nullptr) {
			return false;
		}
		for (float x = 0.0f; x < header->width(); x += 2.0f) {
			const visage::Point local { x, header->height() * 0.5f };
			if (header->hitsMode(local) && header->surfaceAt(local) == wanted) {
				header->processMouseDown(eventOn(header, local, true));
				header->processMouseUp(eventOn(header, local, false));
				pump(400);
				return true;
			}
		}
		return false;
	};

	const auto driveGammaBaseTo = [&](double wanted) -> double {
		if (exploreStage == nullptr) {
			return 0.0;
		}
		const size_t slot = 0; // the selection a fresh editor hands you, and what the corner writes
		// The corner only exists on Explore, and dragging a hidden widget would be exactly the
		// gesture-nobody-makes mistake in a new costume — so the surface is switched for the drag and
		// switched back, which is also what a hand does.
		const bool wasExploring = exploreStage->isVisible();
		if (!wasExploring && !clickSurface(ui::StageMode::Surface::Explore)) {
			return bridge.soundGamma().base[slot];
		}
		const double before = bridge.soundGamma().base[slot];
		const double delta = wanted - before;
		const visage::Bounds box = exploreStage->partBounds(ui::ExploreStage::Part::Gamma);
		const visage::Point centre { box.x() + box.width() * 0.5f, box.y() + box.height() * 0.5f };
		// Upward raises gamma, so a positive delta is a negative y — the widget's own reading of the
		// drag, restated nowhere.
		const float pixels = static_cast<float>(delta / ui::ExploreStage::kGammaTurn)
		                     * ui::Knob::kDragRange;
		exploreStage->processMouseDown(eventOn(exploreStage, centre, true));
		constexpr int kSteps = 10;
		for (int step = 1; step <= kSteps; ++step) {
			const float travelled = pixels * static_cast<float>(step) / kSteps;
			exploreStage->processMouseDrag(
			    eventOn(exploreStage, { centre.x, centre.y - travelled }, true));
			pump(60);
		}
		exploreStage->processMouseUp(eventOn(exploreStage, { centre.x, centre.y - pixels }, false));
		pump(400);
		if (!wasExploring) {
			clickSurface(ui::StageMode::Surface::Perform);
		}
		return bridge.soundGamma().base[slot];
	};

	if (!baseSweep.empty()) {
		if (exploreStage == nullptr) {
			std::printf("base:     REFUSING — no ExploreStage in the tree, so the corner cannot be "
			            "dragged\n");
			return 2;
		}
		std::printf("base:     sweeping the gamma base through %zu value(s) by real corner drags\n",
		            baseSweep.size());
		// The mask is learned at the FIRST of them rather than at zero, so the calibration lives at a
		// base the measured cycles actually use. It still only learns one, which is a stated limit:
		// the stage's own area is excused from a picture taken at one base, so a verdict INSIDE the
		// chart is worth less than a verdict over the chrome, which does not move with the base at all.
		const double reached = driveGammaBaseTo(baseSweep.front());
		std::printf("base:     calibrating at %+.3f (asked %+.3f)\n", reached, baseSweep.front());
		if (std::abs(reached - baseSweep.front()) > 0.5) {
			std::printf("base:     WARNING — the corner drag did not land where it was aimed; treat "
			            "every base below as the ACHIEVED number, not the asked one\n");
		}
	}


	//-------------------------------------------------------------------------------------------//
	// FRAME-TIME MODE — the editor held in one stated condition while the clock runs              //
	//-------------------------------------------------------------------------------------------//
	//
	// WHY A MODE OF ITS OWN, when every other arm of this tool already draws frames. Because a
	// frame-time distribution is a comparison between CONDITIONS, and every existing arm mixes the
	// condition with a camera: `import` is a process, capture stalls the loop, and the restlessness
	// calibration photographs the window dozens of times before anything is judged. A tail measured
	// through that is a tail of the harness. This arm takes no pictures at all — it holds one stated
	// condition and pumps, so the only thing between two arms is the thing named on the command line.
	//
	//   FEATHERS_PROBE_FRAMETIME=<ms>   how long to hold. A marker goes to stdout every second so the
	//                                   log can be segmented by wall time as well as by event.
	//   FEATHERS_PROBE_FT_DEVICE=<tag>  seat an fx device first, and open its panel — the population
	//                                   Names the "owltt band analyzer".
	//   FEATHERS_PROBE_FT_TERRAIN=1     Explore, with the gamma corner under a hand for the whole
	//                                   hold, so the map is REBUILDING rather than sitting still.
	//                                   Named first in the report and absent from every run so far.
	//
	// The two are independent, so "terrain moving with a OWLTT panel open" is one run and not a story.
	if (const char* holdText = std::getenv("FEATHERS_PROBE_FRAMETIME")) {
		const int holdMs = std::max(100, std::atoi(holdText));
		const bool driveTerrain = std::getenv("FEATHERS_PROBE_FT_TERRAIN") != nullptr;
		// How often to open-or-close the seated device's panel during the hold, in ms. 0 is off.
		const int toggleEvery = std::getenv("FEATHERS_PROBE_FT_TOGGLE") != nullptr
		                            ? std::max(50, std::atoi(std::getenv("FEATHERS_PROBE_FT_TOGGLE")))
		                            : 0;
		long toggles = 0;
		// THE BLOCK-RATE PUBLISH ROAD, DRIVEN WITHOUT A DSP.
		//
		// `Owltt::updateMeters` calls its callback UNCONDITIONALLY at the end of every block
		// (owltt.cpp:1084) and `ParamBridge::publishOwlttMeters` ends by setting `repaintDirty_`
		// (gui_bridge.cpp:1008) — so a OWLTT in a running plugin asks for a frame every block, in
		// silence, forever. This process has no audio thread, so that road has never been driven
		// here and every OWLTT arm measured so far was BLIND to it.
		//
		// WHAT IS DRIVEN IS THE ROAD, NOT THE ANALYSER: the same publish call the audio thread makes,
		// at a stated rate, with values that MOVE — so a content-diff fix could not be satisfied by
		// republishing a constant, and so the meter drawing has something to draw.
		//
		// The rate is a knob because the question is whether RATE is a lever at all:
		// `takeRepaintDirty()` is an `exchange(false)` drained once per tick, so anything at or above
		// the frame rate collapses to one notify a frame by construction.
		const int metersHz = std::getenv("FEATHERS_PROBE_FT_METERS") != nullptr
		                         ? std::atoi(std::getenv("FEATHERS_PROBE_FT_METERS"))
		                         : 0;
		long meterPublishes = 0;
		// HOW FAR THE HAND MOVES PER 50 ms STEP. It is a knob because the answer to "what does a
		// terrain build cost" turned out to depend on WHERE, and a fixed 3 px carries the gamma base
		// to +27 over eight seconds — a country nobody works in. A small step keeps the drag near the
		// coordinate the editor opens at, which is the population to measure first.
		const float terrainPx = std::getenv("FEATHERS_PROBE_FT_TERRAIN_PX") != nullptr
		                            ? static_cast<float>(
		                                  std::atof(std::getenv("FEATHERS_PROBE_FT_TERRAIN_PX")))
		                            : 3.0f;

		ui::FxCard* ftCard = nullptr;
		visage::Point ftExpandAt {};
		// THE SLOT THE DEVICE ACTUALLY TOOK. Publishing meters to slot 0 would address the FILTER the
		// lane already holds, and the OWLTT card would read the zeros it was built with — a road
		// driven past the widget it was meant to reach, reporting as a quiet arm.
		size_t ftSlot = kNoPosition;
		if (const char* tag = std::getenv("FEATHERS_PROBE_FT_DEVICE")) {
			auto* lane = findInTree<ui::FxLane>(root);
			if (lane == nullptr) {
				std::printf("frametime: NO FX LANE IN THE TREE — nothing was seated\n");
				return 2;
			}
			// SEVERAL, WHEN ASKED. The blast-radius number was taken with ONE card in a short lane,
			// and a lane that fits everything it holds cannot answer the question that matters:
			// whether a card SCROLLED OUT of the viewport still redraws. fx_lane.cpp:847 sets
			// visibility from `!collapsed() && slot < deviceCount()` and NOT from the scroll, so by
			// reading it does — this seats enough cards to overflow the lane and measures it.
			const int deviceCount = std::getenv("FEATHERS_PROBE_FT_DEVICE_COUNT") != nullptr
			                            ? std::max(1, std::atoi(std::getenv("FEATHERS_PROBE_FT_DEVICE_COUNT")))
			                            : 1;
			size_t slot = kNoPosition;
			for (int seat = 0; seat < deviceCount; ++seat) {
				const size_t took = bridge.addFxDevice(tag);
				if (took == kNoPosition) {
					std::printf("frametime: lane REFUSED device %d of %d\n", seat + 1, deviceCount);
					break;
				}
				slot = took;
				lane->resized();
			}
			lane->scrollCardIntoView(slot);
			lane->resized();
			pump(400);
			if (deviceCount > 1) {
				// EVERY CARD'S OWN ANSWER, from the frame rather than from the rule that set it.
				for (size_t s = 0; s <= slot; ++s) {
					ui::FxCard* c = lane->cardAt(s);
					if (c == nullptr) {
						continue;
					}
					const visage::Bounds b { c->x(), c->y(), c->width(), c->height() };
					const bool onScreen = b.bottom() > 0.0f && b.y() < lane->height();
					std::printf("frametime: card %zu visible=%d bounds y=%.0f h=%.0f -> %s\n", s,
					            c->isVisible() ? 1 : 0, b.y(), b.height(),
					            onScreen ? "in the viewport" : "SCROLLED OUT");
				}
			}
			ui::FxCard* card = lane->cardAt(slot);
			if (card == nullptr) {
				std::printf("frametime: NO CARD AT SLOT %zu — nothing was seated\n", slot);
				return 2;
			}
			// THE PANEL OPEN, through the same derived button the popout arm presses, so the two arms
			// cannot disagree about where it is.
			const visage::Bounds sw = card->enableBounds();
			if (sw.width() <= 0.0f) {
				std::printf("frametime: %s DECLARES NO ENABLE, so no expand button is derivable\n", tag);
				return 2;
			}
			const float size = sw.height();
			const visage::Point expandAt = card->positionInWindow()
			                               + visage::Point { sw.x() - 6.0f - size * 0.5f,
				                                             sw.y() + size * 0.5f };
			visage::Frame* down = root->frameAtPoint(expandAt);
			if (down == nullptr) {
				std::printf("frametime: NO FRAME AT THE EXPAND BUTTON (%.0f,%.0f)\n", expandAt.x,
				            expandAt.y);
				return 2;
			}
			visage::MouseEvent e;
			e.window_position = expandAt;
			e.position = expandAt - down->positionInWindow();
			e.event_frame = down;
			e.button_id = visage::kMouseButtonLeft;
			e.button_state = visage::kMouseButtonLeft;
			e.is_down = true;
			e.repeat_click_count = 1;
			down->processMouseDown(e);
			e.button_state = visage::kMouseButtonNone;
			e.is_down = false;
			down->processMouseUp(e);
			pump(600);
			ftCard = card;
			ftExpandAt = expandAt;
			ftSlot = slot;
			// THE CONTROL FOR THE VISIBILITY GATE. Collapsing the lane runs fx_lane.cpp:847 with
			// `!collapsed()` false, so every card goes `setVisible(false)` — which is the ONE road by
			// which a card stops being redrawn. If the cost falls back to the no-card baseline, the
			// gate is proven to work and extending it to SCROLLED-OUT cards would recover the same
			// cost. If it does not, the gate is not the lever and nothing should be built on it.
			if (std::getenv("FEATHERS_PROBE_FT_COLLAPSE") != nullptr) {
				lane->setCollapsed(true);
				lane->resized();
				pump(400);
				int stillVisible = 0;
				for (size_t s = 0; s <= slot; ++s) {
					ui::FxCard* c = lane->cardAt(s);
					stillVisible += (c != nullptr && c->isVisible()) ? 1 : 0;
				}
				std::printf("frametime: lane collapsed; %d of %zu cards still visible\n", stillVisible,
				            slot + 1);
			}
			std::printf("frametime: seated %s at slot %zu, card %.0fx%.0f, panel opened\n", tag, slot,
			            card->width(), card->height());
		}

		if (metersHz > 0 && ftSlot == kNoPosition) {
			// A VENUE MUST BE SHOWN CAPABLE OF EXPRESSING THE OUTCOME. Publishing into an empty slot
			// would still set repaintDirty_ and would still look like a measurement.
			std::printf("frametime: REFUSING — meters asked for with no device seated; set "
			            "FEATHERS_PROBE_FT_DEVICE=OWLTT\n");
			return 2;
		}

		// HOW MANY FRAMES ONE STATE NOTIFY TELLS — the blast radius of a single `repaintDirty_`.
		//
		// `FrameWatchers::notify()` calls `redrawTree` on every watcher, which walks the whole
		// SUBTREE (ui_state.h:94). The state channel carries PerformStage, ExploreStage, FxLane,
		// SoundStack, WaveDisplay and any open panel (gui.cpp:1282-1322), so this is the number that
		// says whether "repaint the strip rather than the card" is a lever worth building.
		//
		// ASKED THROUGH THE REAL ROAD: the bit is armed exactly as the audio thread arms it, then the
		// tick is run and its own count is read. A count of WATCHERS would be six either way and
		// would say nothing — this is the count the notification itself makes.
		{
			bridge.publishOwlttMeters(ftSlot < kMaxFxSlots ? ftSlot : 0, { 0.1f, 0.1f, 0.1f },
			                         { 0.2f, 0.2f, 0.2f }, { 0.3f, 0.3f, 0.3f }, false);
			const Gui::TickReport blast = gui.tickForTest();
			std::printf("frametime: one state notify tells %zu frames (param road told %zu)\n",
			            blast.stateFrames, blast.paramFrames);
		}

		// THE HAND STAYS DOWN FOR THE WHOLE HOLD, which is the difference between measuring a rebuild
		// and measuring the frame after one. `grabbed_ == Part::Gamma` holds the view still and the
		// base moves under it, which is exactly the scrub reported as laggy.
		visage::Point gammaCentre {};
		// WHO TAKES THE PRESS. Gamma is still the stage's own corner; Radius is a child `Knob` since
		// 2026-09-21 (it had to become one to glow and take a modulation source), and in the shipped
		// editor the child receives the press — so pressing the STAGE at that corner drives a road no
		// hand can reach, and the probe would report a drag that moved nothing.
		visage::Frame* grabbed = nullptr;
		float travelled = 0.0f;
		if (driveTerrain) {
			if (exploreStage == nullptr) {
				std::printf("frametime: NO EXPLORE STAGE IN THE TREE — the terrain cannot be driven\n");
				return 2;
			}
			if (!exploreStage->isVisible() && !clickSurface(ui::StageMode::Surface::Explore)) {
				std::printf("frametime: COULD NOT REACH EXPLORE through the header — refusing\n");
				return 2;
			}
			// WHICH PART THE HAND IS ON, and the two are still different questions after backlog GY.
			//
			// GAMMA is a CONTENT input: it is what the whole plane is taken at, so turning it asks for a
			// genuinely different map and no part of the old one can be reused. RADIUS moves the sound
			// WITHIN the plane, so it is a pan across a fixed world and the window walks over ground it
			// has already earned. Those are the two shapes a terrain build can have and they cost
			// wildly different amounts, which is why the part is a knob.
			//
			// IT NAMED `Part::Drift` UNTIL 2026-09-21, when the polar corners replaced that part with
			// Radius and Theta. **Nothing caught it but a full build**: `FEATHERS_BUILD_TOOLS` defaults
			// OFF, so the whole widget suite compiles and passes with this file broken. The env value
			// stays "drift" so any script or note that sets it keeps working — what it selects is the
			// corner that now does that job.
			const char* partName = std::getenv("FEATHERS_PROBE_FT_TERRAIN_PART");
			const bool onDrift = partName != nullptr
			                     && (std::strcmp(partName, "drift") == 0
			                         || std::strcmp(partName, "radius") == 0);
			const visage::Bounds box = exploreStage->partBounds(
			    onDrift ? ui::ExploreStage::Part::Radius : ui::ExploreStage::Part::Gamma);
			if (box.width() <= 0.0f || box.height() <= 0.0f) {
				std::printf("frametime: the %s part has no bounds — refusing\n",
				            onDrift ? "Radius" : "Gamma");
				return 2;
			}
			if (onDrift) {
				grabbed = &exploreStage->radiusKnobForTest(0);
				gammaCentre = { grabbed->width() * 0.5f, grabbed->height() * 0.5f }; // the knob's own frame
			}
			else {
				grabbed = exploreStage;
				gammaCentre = { box.x() + box.width() * 0.5f, box.y() + box.height() * 0.5f };
			}
			grabbed->processMouseDown(eventOn(grabbed, gammaCentre, true));
			std::printf("frametime: Explore up, %s corner grabbed at (%.0f,%.0f)\n",
			            onDrift ? "radius" : "gamma", gammaCentre.x, gammaCentre.y);
		}

		// THE PRIMITIVE, TIMED IN THE REAL PROCESS. This timed `ParamBridge::routes()` — a 111,360-byte
		// ModRoutes built BY VALUE, which `FxControl::display()` once called per source per paint. That
		// road is gone with the dense table (kill-lanes step 2); what a ring reads per paint now is
		// `routeAmount` by endpoint, one walk of the record's edges, and this is its number.
		//
		// THE SINK IS `volatile` ON THE VALUE, not on a pointer to it — CLAUDE.md's measured trap:
		// `volatile T* p = ...` leaves a dead local the optimiser is free to delete along with the
		// work that fed it.
		{
			static volatile float sink = 0.0f;
			constexpr int kCalls = 2000;
			const auto t0 = std::chrono::steady_clock::now();
			for (int i = 0; i < kCalls; ++i) {
				sink = bridge.routeAmount(backplane::Source::core(static_cast<size_t>(i) % 8),
				                          backplane::Destination::core(0));
			}
			const auto t1 = std::chrono::steady_clock::now();
			const double us =
			    std::chrono::duration<double, std::micro>(t1 - t0).count() / kCalls;
			std::printf("frametime: ParamBridge::routeAmount() costs %.2f us per call (%d calls, sink %.3f)\n",
			            us, kCalls, static_cast<double>(sink));
		}
		// THE WHOLE FRAME, WHICH `VISAGE_TRACE_FRAMETIME` CANNOT SEE.
		//
		// MEASURED, not assumed: `ApplicationEditor::drawFrame` (application_editor.cpp:160-161) calls
		// `drawStaleChildren()` and THEN `canvas_->submit()`, and the frame-time trace starts at the
		// top of `Canvas::submit`. So every widget `draw()` — every `Knob::draw`, every
		// `FxControl::display()`, `ExploreStage::drawTerrain` and its 256-sample vignette — runs
		// OUTSIDE the timed window. That trace is a batching-and-submission clock and it is
		// STRUCTURALLY BLIND to the half of a frame where the widget code lives.
		//
		// So the hold is bracketed with two CPU clocks as well. THREAD is this thread, which is the UI
		// thread and nothing else; PROCESS adds the terrain worker and whatever else the editor has
		// running. Their difference is what is being spent off the UI thread — which is the number
		// that decides whether a terrain build is a lag anybody can feel or a lag that hides.
		const auto cpuNow = [](clockid_t which) {
			timespec ts {};
			clock_gettime(which, &ts);
			return static_cast<double>(ts.tv_sec) * 1000.0 + static_cast<double>(ts.tv_nsec) / 1e6;
		};
		// AND THE WALL CLOCK, because the hold's "8000 ms" is NOMINAL: `pump(n)` sleeps 4 ms a slice
		// and then does a slice's WORK, so a slower editor makes a longer hold. Measured 2026-08-30:
		// the same nominal 8 s hold ran long enough for 99 terrain builds on one binary and 69 on
		// another, which is a 43% difference in wall time hiding inside a constant called 8000.
		// Every rate below divides by THIS, never by holdMs.
		const double wallBefore = cpuNow(CLOCK_MONOTONIC);
		const double threadCpuBefore = cpuNow(CLOCK_THREAD_CPUTIME_ID);
		const double processCpuBefore = cpuNow(CLOCK_PROCESS_CPUTIME_ID);
		const uint64_t requestsBefore =
		    exploreStage != nullptr ? exploreStage->terrainForTest().requests() : 0;
		const uint64_t buildsBefore =
		    exploreStage != nullptr ? exploreStage->terrainForTest().builds() : 0;
		std::printf("frametime: holding %d ms%s%s\n", holdMs,
		            driveTerrain ? " with the gamma corner under a hand" : " at rest",
		            std::getenv("FEATHERS_SURFACE") != nullptr ? " (FEATHERS_SURFACE set)" : "");
		std::fflush(stdout);
		for (int second = 0; second * 1000 < holdMs; ++second) {
			const int slice = std::min(1000, holdMs - second * 1000);
			if (driveTerrain) {
				// Broken into 50ms steps so the drag is a MOTION rather than a jump: one event a
				// second would rebuild once and sit still, which is the picture at rest wearing a
				// drag's name. 3 px a step is a hand's speed on this widget.
				for (int step = 0; step * 50 < slice; ++step) {
					travelled += terrainPx;
					grabbed->processMouseDrag(
					    eventOn(grabbed, { gammaCentre.x, gammaCentre.y - travelled }, true));
					pump(50);
				}
			}
			else if (metersHz > 0) {
				// 4 ms slices, because that is the pump's own granularity; the publish count per slice
				// is derived from the asked rate rather than typed, so 750 Hz and 60 Hz differ in the
				// one thing being varied.
				const double perSlice = static_cast<double>(metersHz) * 0.004;
				double owed = 0.0;
				for (int step = 0; step * 4 < slice; ++step) {
					owed += perSlice;
					while (owed >= 1.0) {
						// MOVING VALUES. A meter republishing a constant is exactly what a content
						// check would filter out, so a constant here would measure the fix rather
						// than the road.
						const float phase = static_cast<float>(meterPublishes % 200) / 200.0f;
						const std::array<float, 3> gr { -phase, phase * 0.5f, -phase * 0.25f };
						const std::array<float, 3> in { phase, 1.0f - phase, phase * 0.5f };
						const std::array<float, 3> outLvl { phase * 0.75f, phase, 1.0f - phase };
						bridge.publishOwlttMeters(ftSlot, gr, in, outLvl, false);
						++meterPublishes;
						owed -= 1.0;
					}
					pump(4);
				}
			}
			else if (toggleEvery > 0 && ftCard != nullptr) {
				// THE PANEL'S FIRST DRAW IS THE FRAME UNDER TEST, and it is the frame the setup above
				// puts OUTSIDE the measured window. A panel that is merely OPEN redraws nothing: under
				// FEATHER_FULL_REDRAW the layer's quads are re-submitted without `draw()` being
				// re-entered, so a knob's `display()` is not called again and an arm that only holds
				// an open panel is blind to what a knob costs to draw. Opening it inside the hold is
				// what puts twenty-four knob draws in front of the clock.
				for (int step = 0; step * toggleEvery < slice; ++step) {
					visage::Frame* down = root->frameAtPoint(ftExpandAt);
					if (down != nullptr) {
						visage::MouseEvent e;
						e.window_position = ftExpandAt;
						e.position = ftExpandAt - down->positionInWindow();
						e.event_frame = down;
						e.button_id = visage::kMouseButtonLeft;
						e.button_state = visage::kMouseButtonLeft;
						e.is_down = true;
						e.repeat_click_count = 1;
						down->processMouseDown(e);
						e.button_state = visage::kMouseButtonNone;
						e.is_down = false;
						down->processMouseUp(e);
						++toggles;
					}
					pump(toggleEvery);
				}
			}
			else {
				pump(slice);
			}
			std::printf("frametime: t=%ds\n", second + 1);
			std::fflush(stdout);
		}
		if (driveTerrain) {
			grabbed->processMouseUp(
			    eventOn(grabbed, { gammaCentre.x, gammaCentre.y - travelled }, false));
			std::printf("frametime: released; gamma base reached %+.3f\n", bridge.soundGamma().base[0]);
		}
		if (exploreStage != nullptr) {
			// THE VALUE, not the gesture. A hold that leaves these where it found them measured a map
			// sitting still however far the corner travelled.
			std::printf("frametime: terrain requests %llu -> %llu, builds %llu -> %llu\n",
			            static_cast<unsigned long long>(requestsBefore),
			            static_cast<unsigned long long>(exploreStage->terrainForTest().requests()),
			            static_cast<unsigned long long>(buildsBefore),
			            static_cast<unsigned long long>(exploreStage->terrainForTest().builds()));
		}
		// A PORTRAIT AT THE END OF THE HOLD, when one is asked for. The frame-time arm takes no
		// pictures by design — a camera in the loop is a camera in the measurement — but the question
		// "is a coarser map ACCEPTABLE" is not a duration and cannot be answered by one.
		//
		// The whole window, so the chart is photographed in the editor it lives in rather than in a
		// crop chosen to flatter it; the crop is done afterwards, by whoever is comparing.
		if (const char* shotPath = std::getenv("FEATHERS_PROBE_FT_SHOT")) {
			pump(600); // let the map settle before the shutter
			const Window shot = captureTarget(display, parent);
			char command[768];
			std::snprintf(command, sizeof(command), "import -depth 8 -window 0x%lx %s",
			              static_cast<unsigned long>(shot), shotPath);
			if (std::system(command) != 0) {
				std::printf("frametime: import FAILED on 0x%lx\n", static_cast<unsigned long>(shot));
				return 2;
			}
			// THE CONDITION BESIDE THE PICTURE, so a frame that looks right for the wrong reason
			// cannot pass as evidence: which resolution built it, and where it is standing.
			// WHICH PASS THIS PICTURE IS OF, asked of the snapshot rather than inferred from the
			// timing. `samples` is stamped only once the WHOLE map holds it — the builder refuses to
			// claim the finer count mid-pass — so 1 is the rough first paint and >1 is the settled
			// map. Without it a shot taken a little too late is a refined map labelled "first paint",
			// which is the picture that would decide this wrongly.
			int samples = -1;
			int rows = -1;
			double homeSpan = 0.0;
			if (exploreStage != nullptr) {
				const auto snap = exploreStage->terrainForTest().snapshot();
				samples = snap->samples;
				rows = snap->rows;
				homeSpan = snap->window.travelYSpan;
			}
			std::printf("frametime: shot %s at %dx%d texels, samples %d, rows %d, homeSpan %.6f, "
			            "gamma base %+.4f\n",
			            shotPath, ui::TerrainMap::kResolution, ui::TerrainMap::kResolution, samples,
			            rows, homeSpan, bridge.soundGamma().base[0]);
		}

		if (metersHz > 0) {
			const Gui::TickReport drive = gui.takeDriveReportForTest();
			std::printf("frametime: published %ld meter frames at %d Hz; drive report since the last "
			            "read: paramFrames %zu stateFrames %zu\n",
			            meterPublishes, metersHz, drive.paramFrames, drive.stateFrames);
		}
		if (toggleEvery > 0) {
			// THE COUNT, because a toggle that found no frame under the point drove nothing and would
			// otherwise report as a quiet arm rather than as a broken one.
			std::printf("frametime: panel toggled %ld times\n", toggles);
		}
		{
			const double wall = cpuNow(CLOCK_MONOTONIC) - wallBefore;
			const double threadCpu = cpuNow(CLOCK_THREAD_CPUTIME_ID) - threadCpuBefore;
			const double processCpu = cpuNow(CLOCK_PROCESS_CPUTIME_ID) - processCpuBefore;
			std::printf("frametime: wall %.1f ms (nominal %d)\n", wall, holdMs);
			std::printf("frametime: CPU over the hold: ui-thread %.1f ms, whole process %.1f ms "
			            "(%.1f%% and %.1f%% of one core over the WALL time)\n",
			            threadCpu, processCpu, 100.0 * threadCpu / wall, 100.0 * processCpu / wall);
		}
		std::printf("frametime: HELD %d ms\n", holdMs);
		return 0;
	}

	const Window target = captureTarget(display, parent);
	const std::string scratch = outDir + "/probe_scratch.ppm";
	// The two halves report separately, because "no capture" has two causes a morning apart: `import`
	// refusing the window, and a file that arrived but is not a frame.
	int lastImportStatus = 0;
	bool lastParseOk = true;
	// A THIRD CAUSE OF "no capture", and the one that used to be scored as a good frame.
	bool lastWasBlank = false;
	long blankCaptures = 0;
	const auto capture = [&](Image& image) {
		char command[512];
		// `-depth 8` is not a size optimisation. This display hands `import` a 16-bit visual, so the
		// default PPM comes out at maxval 65535 — a perfectly good file that the parser below refuses,
		// and refuses correctly: comparing it byte-wise as if it were 8-bit would halve the width of
		// every row and every diff after it would be nonsense.
		std::snprintf(command, sizeof(command), "import -depth 8 -window 0x%lx ppm:%s",
		              static_cast<unsigned long>(target), scratch.c_str());
		lastImportStatus = std::system(command);
		if (lastImportStatus != 0) {
			return false;
		}
		lastParseOk = readPpm(scratch, image);
		if (!lastParseOk) {
			return false;
		}
		lastWasBlank = pixelDeviation(image) <= kBlankDeviation;
		if (lastWasBlank) {
			++blankCaptures;
			return false;
		}
		return true;
	};

	Image resting;
	// RETRIED, because this one is fatal and a blank frame is transient at roughly 8%. Every other
	// call site already tolerates a failed capture; only the baseline ends the run, so only the
	// baseline is worth a second ask.
	bool restingOk = false;
	for (int attempt = 0; attempt < 5 && !restingOk; ++attempt) {
		restingOk = capture(resting);
	}
	if (!restingOk) {
		std::printf("capture failed — `import -window 0x%lx` exited %d%s\n",
		            static_cast<unsigned long>(target), lastImportStatus,
		            lastWasBlank      ? ", and every attempt came back a single flat colour"
		            : lastImportStatus == 0 ? ", and what it wrote did not parse as a PPM"
		                                    : "");
		return 2;
	}
	// The tree is logical; the window is scaled. Derived from what was actually captured rather than
	// from the scale we think is in force, so a display that disagrees with us cannot silently put
	// every region in the wrong place.
	// STARTUP BLANKS DO NOT CONDEMN THE RUN. The retry above may have burned one or two, and a
	// baseline that was RECOVERED is not evidence against the cycles that follow — counting them into
	// the verdict below would make the tool cry wolf on a run that was fine, which is how a guard
	// gets switched off. Reported for the record, then the counter starts again at zero so that what
	// it holds at the end is blanks that cost a SCORED frame.
	const long startupBlanks = blankCaptures;
	blankCaptures = 0;
	if (startupBlanks > 0) {
		std::printf("capture:  %ld blank frame(s) before the baseline landed, retried successfully;"
		            " not counted against the run\n",
		            startupBlanks);
	}

	const float scale = static_cast<float>(resting.width) / root->width();
	std::printf("window:   0x%lx  %dx%d captured, tree is %.0fx%.0f logical, scale %.3f\n",
	            static_cast<unsigned long>(target), resting.width, resting.height, root->width(),
	            root->height(), static_cast<double>(scale));

	//-------------------------------------------------------------------------------------------//
	// CALIBRATION: what moves on its own                                                        //
	//-------------------------------------------------------------------------------------------//
	AllowedMask mask;
	mask.width = resting.width;
	mask.height = resting.height;
	mask.allowed.assign(static_cast<size_t>(mask.width) * mask.height, 0);

	// LONG ENOUGH TO SEE THE SLOW THINGS. The first calibration here ran fourteen frames over about a
	// second, and reported twenty clean-editor cycles as findings: the Perform star field drifts on a
	// cadence longer than that, so it was still moving when the calibration had stopped watching and
	// every twinkle read as stray ink. A calibration has to cover the timescale of the thing it is
	// calibrating against, which is a whole gesture cycle, not a moment.
	constexpr int kCalibrationFrames = 40;
	int restlessSeen = 0;
	for (int frame = 0; frame < kCalibrationFrames; ++frame) {
		pump(60);
		Image sample;
		if (!capture(sample) || !sample.valid() || sample.width != resting.width
		    || sample.height != resting.height) {
			continue;
		}
		++restlessSeen;
		for (int y = 0; y < resting.height; ++y) {
			for (int x = 0; x < resting.width; ++x) {
				const size_t index = (static_cast<size_t>(y) * resting.width + x) * 3;
				if (differs(sample, resting, index)) {
					mask.allowed[static_cast<size_t>(y) * mask.width + x] = 1;
				}
			}
		}
	}
	long restless = 0;
	for (const unsigned char pixel : mask.allowed) {
		restless += pixel;
	}
	dilate(mask, 3);

	//-------------------------------------------------------------------------------------------//
	// The regions the gesture is entitled to change                                             //
	//-------------------------------------------------------------------------------------------//
	// A REGION BIG ENOUGH TO HIDE THE DEFECT IS NOT A REGION, IT IS A BLINDFOLD, and the first run of
	// this tool proved the point by excusing 100% of the window and then reporting a deliberately
	// injected polygon as clean. Some frames that legitimately watch a parameter are whole surfaces —
	// a stage redraws entirely when the character moves — and excusing one of those excuses the
	// chrome, the origin, and everything the hunt is about.
	//
	// So a candidate region is admitted only if it is small enough to still leave a window to watch.
	// One that is not is REPORTED AND DROPPED rather than silently trimmed: the honest consequence is
	// that real repaints inside it will be reported as stray ink, and the operator needs to know that
	// is why, not be quietly handed a detector with a hole in it.
	const double windowArea = static_cast<double>(mask.width) * mask.height;
	constexpr double kLargestExcusableShare = 0.12;
	const auto admit = [&](visage::Frame* frame, const char* what) {
		const Rect rect = rectOf(frame, scale, 6);
		const double share = static_cast<double>(rect.right - rect.left) * (rect.bottom - rect.top)
		                     / windowArea;
		if (share > kLargestExcusableShare) {
			std::printf("region:   DROPPED %-16s %4d,%-4d %4dx%-4d — %.0f%% of the window is too\n"
			            "          much to excuse; repaints inside it will be reported as stray\n",
			            what, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
			            100.0 * share);
			return;
		}
		std::printf("region:   excused %-16s %4d,%-4d %4dx%-4d (%.1f%%)\n", what, rect.left, rect.top,
		            rect.right - rect.left, rect.bottom - rect.top, 100.0 * share);
		mask.add(rect);
	};

	// WHICH KNOB, because the trigger is a knob×surface PAIR rather than a knob.
	//
	// The refined pattern: the rays appear on PATH in Explore and on GAMMA in Perform — the two
	// combinations where the surface on screen does NOT watch the knob being turned. The other two
	// drive a heavy repaint of that surface, which would erase a corrupt draw before it could be
	// seen. So the working model is that any knob gesture makes the bad draw and the surface only
	// decides whether it survives, and testing that needs both knobs against both surfaces. The
	// surface half is gui.cpp's own FEATHERS_SURFACE; this is the other half.
	const ParamDesc* targetDesc = findParam(targetId);
	if (targetDesc == nullptr) {
		std::printf("no such parameter to drag\n");
		return 2;
	}
	// A quarter in from each end, so a twelve-step drag has room to run either way without railing —
	// the failure that made nineteen of the first twenty cycles photographs of nobody's hand.
	const double lowStart = targetDesc->min + (targetDesc->max - targetDesc->min) * 0.25;
	const double highStart = targetDesc->min + (targetDesc->max - targetDesc->min) * 0.75;
	const char* surfaceName = std::getenv("FEATHERS_SURFACE");
	std::printf("knob:     %s (id 0x%04x, range %.2f..%.2f) on the %s surface\n", targetDesc->name,
	            static_cast<unsigned>(targetId), targetDesc->min, targetDesc->max,
	            surfaceName != nullptr ? surfaceName : "perform (default)");
	const std::span<visage::Frame* const> watchers = widgets->watchers(targetId);
	visage::Frame* gammaKnob = nullptr;
	for (visage::Frame* watcher : watchers) {
		if (watcher->width() > 0.0f && watcher->height() > 0.0f) {
			admit(watcher, "knob watcher");
			// The smallest control among the watchers is the one to drag: the knob is a knob, and a
			// whole surface that also watches gamma is not something a hand grabs.
			if (gammaKnob == nullptr
			    || watcher->width() * watcher->height() < gammaKnob->width() * gammaKnob->height()) {
				gammaKnob = watcher;
			}
		}
	}
	if (gammaKnob == nullptr) {
		std::printf("no %s watcher with a size — nothing to drag\n", targetDesc->name);
		return 2;
	}
	// The knob's own parent draws its label and its box, so it is excused too — subject to the same
	// size rule as everything else.
	if (gammaKnob->parent() != nullptr && gammaKnob->parent() != root) {
		admit(gammaKnob->parent(), "knob's parent");
	}
	// THE TRAY, and anything else the root holds that ignores the mouse. gui.cpp gives the notify tray
	// that property in as many words — "it is a readout, not a control" — and finding it by behaviour
	// rather than by type keeps this file out of the widget's header. Every such child is excused,
	// because the reason to excuse one is the reason to excuse them all.
	int deafChildren = 0;
	std::vector<Rect> trayBoxes;
	for (visage::Frame* child : root->children()) {
		if (child->ignoresMouseEvents() && child->width() > 0.0f && child->height() > 0.0f) {
			admit(child, "deaf child (tray?)");
			trayBoxes.push_back(rectOf(child, scale, 0));
			++deafChildren;
		}
	}
	visage::Frame* tray = findNotifyTray(root);
	if (tray == nullptr) {
		std::printf("region:   NOTE — %d deaf children of the root, so the tray is not uniquely\n"
		            "          identified; \"tray spoke\" below reads the whole set.\n",
		            deafChildren);
	}
	long allowedPixels = 0;
	for (const unsigned char pixel : mask.allowed) {
		allowedPixels += pixel;
	}
	std::printf("regions:  %ld restless px measured over %d frames; %ld px allowed in total "
	            "(%.1f%% of the window)\n",
	            restless, restlessSeen, allowedPixels, 100.0 * allowedPixels / windowArea);
	if (allowedPixels > windowArea * 0.5) {
		std::printf("          REFUSING TO RUN: more than half the window is excused, so this could\n"
		            "          not photograph the defect even if it happened.\n");
		return 2;
	}

	//-------------------------------------------------------------------------------------------//
	// The known-bad, when asked for                                                             //
	//-------------------------------------------------------------------------------------------//
	OriginInk ink;
	int injectAtFrame = -1;
	if (controlAt != nullptr) {
		injectAtFrame = std::max(0, std::atoi(controlAt));
		ink.setIgnoresMouseEvents(true, false);
		ink.setBounds(0.0f, 0.0f, root->width() * 0.45f, root->height() * 0.42f);
		root->addChild(&ink);
		std::printf("CONTROL:  one cycle, ink injected at gesture frame %d. This run FAILS unless\n"
		            "          the diff catches it.\n",
		            injectAtFrame);
	}

	//-------------------------------------------------------------------------------------------//
	// The gesture                                                                               //
	//-------------------------------------------------------------------------------------------//
	const visage::Point centre { gammaKnob->positionInWindow().x + gammaKnob->width() * 0.5f,
		                         gammaKnob->positionInWindow().y + gammaKnob->height() * 0.5f };
	std::printf("gesture:  Gamma knob centred at %.1f,%.1f logical\n",
	            static_cast<double>(centre.x), static_cast<double>(centre.y));

	// The drag spans multiple frames at a hand's rate: twelve motion events, upward, which is how a
	// knob is turned. The travel is the knob's own vertical sensitivity range rather than a round
	// number, so every step lands on a different value and every readout is a different string.
	constexpr int kMotionEvents = 12;
	constexpr float kTravel = 96.0f;

	const int totalCycles = controlAt != nullptr ? 1 : std::max(1, cycles);
	int framePacedCycles = totalCycles;
	if (controlAt == nullptr) {
		if (cadence == "human") {
			framePacedCycles = 0;
		}
		else if (cadence == "both") {
			framePacedCycles = totalCycles / 2;
		}
	}

	int findings = 0;
	int inconclusive = 0;
	int controlCaught = 0;
	int flickerFindings = 0;
	int controlFlickerCaught = 0;
	long worstStray = 0;

	// WHAT MOVES WHEN GAMMA MOVES, measured the same way restlessness was.
	//
	// A rectangle cannot express this. Gamma is a macro over every sound's character, so the surfaces
	// that answer it are large — the blend circle's whole star field re-scrambles, the wave display
	// redraws — and the size rule above rightly refuses to excuse a 55%-of-the-window region by name,
	// because excusing it would excuse the origin and the chrome along with it. The first honest run
	// of this tool therefore reported twenty clean editors as findings: the stars had moved.
	//
	// So the excuse is earned per pixel, by running whole gestures and watching. A pixel that answers
	// gamma answers it EVERY time, so the bar is a majority of the calibration gestures; an
	// intermittent artefact, which is what is being hunted, does not clear that bar and is still
	// reported afterwards. This is the one part of the mask that could in principle swallow the
	// defect, and the majority rule is what stops it swallowing anything that is not reliable.
	constexpr int kCalibrationGestures = 4;
	std::vector<uint16_t> answersGamma(static_cast<size_t>(mask.width) * mask.height, 0);
	std::vector<unsigned char> movedThisGesture;
	bool calibratingGesture = false;

	// THE ARMED FIRST GESTURE — and it took a measured run to see why it has to exist.
	//
	// The corruption is diagnosed as texture-side: an atlas resize moves glyph UVs under text quads
	// that are already in flight. A resize happens when never-before-seen glyphs at a never-before-
	// seen size arrive, so the drag that can trigger one is THE FIRST DRAG ON A FRESH EDITOR — after
	// that the atlas has the digits and stops growing, which is exactly what the second session
	// found (retriggers produced 828 log lines and zero new atlas events).
	//
	// The first version of this tool could never have caught that. Its calibration ran first: forty
	// resting frames, four learning gestures and a noise gesture, all before the first measured drag
	// — and the atlas log proves the cost, with all 36 resizes inside the first 6.5 seconds and NOT
	// ONE during any of the eight gestures that were actually being judged. The instrument was
	// arriving after the event it was built to photograph.
	//
	// So the very first gesture now runs before any calibration, and its frames are RETAINED in
	// memory rather than judged on the spot — there is no mask yet to judge them against. Once
	// calibration has built the mask and measured the floors, the retained frames are judged with the
	// full detector, at full sensitivity, at the one moment that matters.
	const bool armFirstGesture = std::getenv("FEATHERS_PROBE_ARMED_FIRST") == nullptr
	                             || std::strcmp(std::getenv("FEATHERS_PROBE_ARMED_FIRST"), "0") != 0;
	std::vector<Image> armedFrames;
	std::vector<long long> armedTimes;
	std::vector<int> armedFrameNumbers;
	Image armedBaseline;
	bool retainingFrames = false;

	// One more gesture than there are learning ones: the last of them is judged against the finished
	// mask, and whatever it reports is this venue's noise rather than a defect — it is the same
	// gesture that just taught the mask what is legitimate. That measurement sets the floor.
	const int firstCycle = -(kCalibrationGestures + 1) - (armFirstGesture ? 1 : 0);
	for (int cycle = firstCycle; cycle < totalCycles; ++cycle) {
		const bool armedFirst = armFirstGesture && cycle == firstCycle;
		retainingFrames = armedFirst;
		calibratingGesture = cycle < -1 && !armedFirst;
		const bool noiseGesture = cycle == -1;
		if (calibratingGesture) {
			movedThisGesture.assign(static_cast<size_t>(mask.width) * mask.height, 0);
		}
		const bool framePaced = controlAt != nullptr || calibratingGesture || cycle < framePacedCycles;

		// A KNOB ON THE RAIL IS NOT A GESTURE. The first twenty-cycle run of this tool dragged Gamma
		// to its maximum in cycle 0 and then photographed nineteen more cycles in which every motion
		// event computed the same railed value, wrote nothing, and never made the tray say anything —
		// nineteen photographs of a control nobody was touching, reported as if they were evidence.
		//
		// So each cycle is given room before it starts, and takes it in the opposite direction from
		// the last. The reset goes through the parameter road rather than the gesture, because it is
		// the STARTING STATE and not part of what is being measured — the same road a host's
		// automation would use to put the knob somewhere.
		const bool upward = (cycle % 2) == 0;
		params.setBase(targetId, upward ? lowStart : highStart);
		bridge.publish(params);
		bridge.refresh();
		const long writesBefore = valueWrites;

		// The base for this cycle, moved BEFORE the baseline below is photographed so the cycle is
		// judged against the editor it is about to gesture on rather than against the previous base.
		double heldBase = 0.0;
		if (!baseSweep.empty() && cycle >= 0) {
			const double asked = baseSweep[static_cast<size_t>(cycle) % baseSweep.size()];
			heldBase = driveGammaBaseTo(asked);
			std::printf("cycle %2d: gamma base held at %+.3f (asked %+.3f)\n", cycle, heldBase, asked);
		}

		// The resting picture this cycle is judged against is taken fresh: the editor is a live thing
		// and the sky has moved since the last one. Judging every cycle against a photograph from ten
		// cycles ago would report the passage of time as a defect.
		pump(120);
		Image baseline;
		if (!capture(baseline) || baseline.width != mask.width || baseline.height != mask.height) {
			std::printf("cycle %2d: INCONCLUSIVE — no usable baseline capture\n", cycle);
			++inconclusive;
			continue;
		}

		std::vector<uint64_t> names;
		names.push_back(fingerprint(baseline));
		Verdict firstFired;
		int firstFiredFrame = -1;
		bool trayHadSpoken = false;
		bool trayWhenFired = false;
		long cycleWorst = 0;
		long cycleChanged = 0;
		long cycleWorstTopLeft = 0;
		int cornerRepaints = 0;
		std::string cornerFrames;

		// Press. Routed exactly as visage's WindowEventHandler routes it: the down-frame is found by
		// hit test, `position` is the window position minus that frame's own, and the SAME frame
		// receives every drag and the release even once the pointer has left it.
		visage::Frame* down = root->frameAtPoint(centre);
		if (down == nullptr) {
			std::printf("cycle %2d: INCONCLUSIVE — nothing under the knob's centre\n", cycle);
			++inconclusive;
			continue;
		}
		const auto eventAt = [&](visage::Point at, bool isDown) {
			visage::MouseEvent event;
			event.window_position = at;
			event.position = at - down->positionInWindow();
			event.event_frame = down;
			event.button_id = visage::kMouseButtonLeft;
			event.button_state = isDown ? visage::kMouseButtonLeft : visage::kMouseButtonNone;
			event.is_down = isDown;
			event.repeat_click_count = 1;
			return event;
		};

		const auto look = [&](int gestureFrame) {
			// MEASURED CYCLES ONLY — every calibration gesture, learning or noise, must see the
			// editor as it ships. Ink on the window while the mask is being learned teaches the mask
			// to excuse it; ink on the window while the NOISE is being measured teaches the run that
			// the artefact is the normal amount of artefact, which is worse. The second of those was
			// not hypothetical: it set the floor to 116067 px and the control went blind at once.
			if (controlAt != nullptr && cycle >= 0) {
				ink.armed = gestureFrame >= injectAtFrame;
				if (ink.armed) {
					ink.redraw();
				}
			}
			pump(framePaced ? 24 : 40);
			if (!framePaced && gestureFrame < kMotionEvents) {
				return; // human pace: the motion events run uninterrupted, and the photograph waits
			}
			// WHAT THIS FRAME ACTUALLY REPAINTED, read from the renderer rather than inferred from
			// the pixels. visage is a partial-damage renderer with no clear, so a stray mark drawn
			// once is carried forward by every later frame that does not damage its region — which
			// means "the stray is still there" is the DEFAULT, not evidence of a redraw. The corner
			// flag is what separates a frame that painted the top-left 100x100 from one that merely
			// inherited it, and without it the two are indistinguishable in a screenshot.
			// RE-POINTED FOR visage v1.0.0 (2026-11-05): the fork's per-frame damage query
			// (`lastFrameDamage`) instrumented bgfx's damage and has no v1.0.0 counterpart. Under
			// FEATHERS_FULL_REDRAW every layer repaints every frame, so the corner IS repainted by
			// construction; without it this build cannot say, and the count reads -1 ("unknown")
			// rather than a zero that would claim the corner was never painted.
#ifdef FEATHERS_FULL_REDRAW
			const bool cornerRepainted = true;
#else
			const bool cornerRepainted = false;
			cornerRepaints = -1;
#endif
			Image frame;
			if (!capture(frame) || frame.width != mask.width || frame.height != mask.height) {
				return;
			}
			names.push_back(fingerprint(frame));
			// A WINDOW OF FRAMES AROUND A KNOWN MOMENT. Once a fault reproduces at a predictable
			// cycle and frame, the interesting picture is no longer the first frame that fires — it
			// is the frames on either side of it, which show what the editor looked like immediately
			// before the mark appeared and how it developed after. FEATHERS_PROBE_KEEP=<cycle>:<lo>-<hi>
			// keeps that range verbatim; without it nothing extra is written, because these are
			// megabytes each and most runs want none of them.
			if (cycle == keepCycle && gestureFrame >= keepLow && gestureFrame <= keepHigh) {
				char kept[512];
				std::snprintf(kept, sizeof(kept), "%s/seq_c%02d_f%02d.png", outDir.c_str(), cycle,
				              gestureFrame);
				char convert[1200];
				std::snprintf(convert, sizeof(convert), "convert %s %s 2>/dev/null", scratch.c_str(),
				              kept);
				if (std::system(convert) != 0) {
					std::printf("          WARNING: could not write %s\n", kept);
				}
			}
			if (cornerRepainted && cornerRepaints >= 0) {
				++cornerRepaints;
				cornerFrames += std::to_string(gestureFrame) + " ";
			}
			// Has the tray spoken yet? Asked in PIXELS, because say() is not reachable from here and
			// what matters is whether its ink is on the window at the moment the frame was taken.
			for (const Rect& box : trayBoxes) {
				for (int y = std::max(0, box.top); y < std::min(frame.height, box.bottom) && !trayHadSpoken; ++y) {
					for (int x = std::max(0, box.left); x < std::min(frame.width, box.right); ++x) {
						if (differs(frame, baseline, (static_cast<size_t>(y) * frame.width + x) * 3)) {
							trayHadSpoken = true;
							break;
						}
					}
				}
			}
			if (retainingFrames) {
				// KEPT, NOT JUDGED. There is no mask yet — calibration has not run — so judging here
				// would be judging against nothing. The frames wait in memory for the full detector.
				armedFrames.push_back(frame);
				armedTimes.push_back(traceMilliseconds());
				armedFrameNumbers.push_back(gestureFrame);
				return;
			}
			if (calibratingGesture) {
				// Learning, not judging: every pixel this gesture moved at any point in it.
				for (size_t pixel = 0; pixel < movedThisGesture.size(); ++pixel) {
					if (differs(frame, baseline, pixel * 3)) {
						movedThisGesture[pixel] = 1;
					}
				}
				return;
			}
			const Verdict verdict = judge(frame, baseline, mask);
			cycleWorst = std::max(cycleWorst, verdict.stray);
			cycleChanged = std::max(cycleChanged, verdict.changed);
			cycleWorstTopLeft = std::max(cycleWorstTopLeft, verdict.strayTopLeft);
			// The noise gesture is judged but never photographed: its whole job is to report what a
			// clean gesture still leaves behind, and a "finding" written from it would be a picture
			// of the venue's own noise sitting in the evidence directory.
			if (verdict.fired() && firstFiredFrame < 0 && !noiseGesture) {
				firstFired = verdict;
				firstFiredFrame = gestureFrame;
				trayWhenFired = trayHadSpoken;
				// THE PHOTOGRAPH. Both halves of the pair are kept — a corrupt frame on its own does
				// not show what the editor looked like a moment earlier, and the difference is the
				// whole evidence.
				char keptFrame[512];
				char keptBase[512];
				std::snprintf(keptFrame, sizeof(keptFrame), "%s/finding_c%02d_f%02d.png",
				              outDir.c_str(), cycle, gestureFrame);
				std::snprintf(keptBase, sizeof(keptBase), "%s/finding_c%02d_baseline.png",
				              outDir.c_str(), cycle);
				char convert[1200];
				std::snprintf(convert, sizeof(convert), "convert %s %s 2>/dev/null", scratch.c_str(),
				              keptFrame);
				int converted = std::system(convert);
				FILE* basePpm = std::fopen((outDir + "/probe_base.ppm").c_str(), "wb");
				if (basePpm != nullptr) {
					std::fprintf(basePpm, "P6\n%d %d\n255\n", baseline.width, baseline.height);
					std::fwrite(baseline.rgb.data(), 1, baseline.rgb.size(), basePpm);
					std::fclose(basePpm);
					std::snprintf(convert, sizeof(convert), "convert %s/probe_base.ppm %s 2>/dev/null",
					              outDir.c_str(), keptBase);
					converted |= std::system(convert);
				}
				// Said rather than assumed: a finding whose evidence did not reach the disk is a
				// finding somebody will go looking for tomorrow and not find.
				if (converted != 0) {
					std::printf("          WARNING: could not write the capture pair — is `convert` "
					            "on PATH?\n");
				}
			}
		};

		// Upward turns a knob up, downward turns it down; alternating is what keeps every cycle off
		// the rail the previous one drove towards.
		const float direction = upward ? -1.0f : 1.0f;
		down->processMouseDown(eventAt(centre, true));
		look(0);
		for (int step = 1; step <= kMotionEvents; ++step) {
			const float travelled = kTravel * static_cast<float>(step) / kMotionEvents;
			down->processMouseDrag(eventAt({ centre.x, centre.y + direction * travelled }, true));
			look(step);
		}
		down->processMouseUp(eventAt({ centre.x, centre.y + direction * kTravel }, false));
		// A full second after the release, which is where a retained artefact lives: it does not
		// vanish when the hand comes off, and the chrome it sits on is not going to repaint it away.
		for (int after = 0; after < 8; ++after) {
			look(kMotionEvents + 1 + after);
		}

		//---------------------------------------------------------------------------------------//
		// THE REST PHASE — a lock-in amplifier for something that betrays itself by being alive  //
		//---------------------------------------------------------------------------------------//
		//
		// THE CASE-CHANGING OBSERVATION: with the rays on screen and NOBODY TOUCHING ANYTHING,
		// there was a tiny bit of flicker in the rays. A retained photograph cannot flicker. So
		// the artefact is not a stale mark baked into a layer and left alone — it is being REDRAWN
		// every frame by something whose coordinates vary a little each time.
		//
		// That makes a far more sensitive detector available than inspecting single frames against a
		// resting one. At rest the editor is almost perfectly still, so instead of asking "is this
		// frame different from the resting editor", which the star field answers loudly, the question
		// becomes "did anything change between two CONSECUTIVE frames while nobody was touching it".
		// The ambient answer to that is nearly nothing, and a live-redrawn artefact answers it every
		// frame. Small signal, near-zero noise floor: the artefact's own liveness betrays it.
		//
		// The CADENCE is worth as much as the detection, and it is why every rest frame is timed
		// rather than only counted. The short list of things that redraw at rest each has its own
		// rhythm — audio-rate for the scope polyline, a slow decay for the notify tray's fade, a
		// twinkle for the sky — so measuring how often the stray pixels change NAMES the redrawer.
		long restFlickerWorst = 0;
		int restFlickerFrames = 0;
		Rect restBounds { 1 << 30, 1 << 30, -1, -1 };
		long long restFirstFlickerAt = 0;
		long long restLastFlickerAt = 0;
		{
			Image restPrevious;
			bool havePrevious = false;
			for (int restFrame = 0; restFrame < kRestFrames; ++restFrame) {
				// The control's ink TOGGLES here rather than sitting still, because a static
				// injection cannot test a detector that looks for change. A frame-to-frame diff would
				// report a permanently-drawn polygon as perfectly quiet — correctly — and the control
				// would pass while proving nothing about the thing it is meant to certify.
				if (controlAt != nullptr && cycle >= 0) {
					ink.armed = (restFrame % 2) == 0;
					ink.redraw();
				}
				pump(kRestFramePeriodMs);
				Image restFrameImage;
				if (!capture(restFrameImage) || restFrameImage.width != mask.width
				    || restFrameImage.height != mask.height) {
					continue;
				}
				const long long at = traceMilliseconds();
				names.push_back(fingerprint(restFrameImage));
				if (havePrevious) {
					const Verdict flicker = judge(restFrameImage, restPrevious, mask);
					if (flicker.stray > 0) {
						restBounds.left = std::min(restBounds.left, flicker.bounds.left);
						restBounds.top = std::min(restBounds.top, flicker.bounds.top);
						restBounds.right = std::max(restBounds.right, flicker.bounds.right);
						restBounds.bottom = std::max(restBounds.bottom, flicker.bounds.bottom);
					}
					if (flicker.stray >= gRestFloor) {
						++restFlickerFrames;
						restLastFlickerAt = at;
						if (restFirstFlickerAt == 0) {
							restFirstFlickerAt = at;
						}
					}
					restFlickerWorst = std::max(restFlickerWorst, flicker.stray);
				}
				restPrevious = std::move(restFrameImage);
				havePrevious = true;
			}
			if (controlAt != nullptr && cycle >= 0) {
				ink.armed = true;
			}
		}

		// THE SEVENTH WAY A RUN CAN LIE, refused here rather than in the write-up: a window that
		// froze, or one that something is sitting on top of, hands back the same bytes every time.
		// A cycle of identical frames is not a clean cycle, it is no cycle.
		std::sort(names.begin(), names.end());
		const size_t distinct = static_cast<size_t>(std::unique(names.begin(), names.end()) - names.begin());
		worstStray = std::max(worstStray, cycleWorst);

		if (armedFirst) {
			armedBaseline = baseline;
			std::printf("armed:    first gesture on a fresh editor captured — %zu frames held for\n"
			            "          judging once the mask exists (%lld..%lld)\n",
			            armedFrames.size(), armedTimes.empty() ? 0 : armedTimes.front(),
			            armedTimes.empty() ? 0 : armedTimes.back());
			std::fflush(stdout);
			continue;
		}

		if (calibratingGesture) {
			long moved = 0;
			for (size_t pixel = 0; pixel < movedThisGesture.size(); ++pixel) {
				answersGamma[pixel] += movedThisGesture[pixel];
				moved += movedThisGesture[pixel];
			}
			std::printf("learning: gesture %d of %d moved %ld px; %zu distinct frames\n",
			            cycle + kCalibrationGestures + 1, kCalibrationGestures, moved, distinct);
			// The last one closes the mask, and the total is reported so a mask that has quietly
			// grown to cover the window is visible BEFORE any verdict is read from it.
			if (cycle == -2) {
				// A MAJORITY of the calibration gestures, not every pixel any of them moved — and
				// this reverses an earlier decision in this file on evidence that arrived later.
				//
				// The union was chosen because the star field is STOCHASTIC in gamma: a different
				// scatter of stars lights up at each value, no single star clears a majority, and the
				// residue fired a verdict on clean cycles. That reasoning was sound and is now
				// obsolete, because the MEASURED FLOOR handles stochastic residue directly — it is
				// derived from exactly that noise — and the union's stated risk turned out to be
				// real rather than theoretical.
				//
				// WHAT THE UNION SWALLOWED. Under the automation storm the artefact is already on
				// screen while calibration runs, so the union learned it: the mask reached 48.7% of
				// the window in one run and 52.5% in another, and the gesture that actually PRODUCED
				// the artefact was reported clean. The detector had been taught to ignore the thing
				// it was built to find.
				//
				// WHY MAJORITY IS RIGHT FOR THIS FAULT SPECIFICALLY, rather than merely stricter.
				// The artefact PINS: it is drawn once and then carried forward unchanged, measured
				// here as thirteen byte-stable frames after its birth. So it MOVES in the one
				// calibration gesture that births it and in none of the others — every later
				// gesture's baseline already contains it — and it cannot reach a majority. Ordinary
				// surfaces that answer the parameter move in every gesture and clear it easily. The
				// rule separates them on the property that actually distinguishes them.
				long learned = 0;
				long union_ = 0;
				for (size_t pixel = 0; pixel < answersGamma.size(); ++pixel) {
					union_ += answersGamma[pixel] > 0 ? 1 : 0;
					if (answersGamma[pixel] * 2 >= kCalibrationGestures) {
						mask.allowed[pixel] = 1;
						++learned;
					}
				}
				// THE ONE THING THE UNION COULD HIDE, so it is measured rather than hoped about: an
				// artefact present in every calibration gesture would have been learned as legitimate.
				// It would also be an artefact that moved the same pixels all four times, which the
				// majority count sees. A big gap between the two numbers is ordinary stochastic
				// surface; a large majority OUTSIDE the surfaces that answer gamma is not.
				// Both numbers are printed because the GAP is diagnostic: a large union over a small
				// majority is a stochastic surface, which the floor absorbs. A union far larger than
				// the majority AND far larger than earlier runs is the shape of an artefact sitting
				// in the calibration frames, which is worth knowing before reading any verdict.
				std::printf("learning: %ld px in a majority of gestures (excused); %ld moved in at\n"
				            "          least one (not excused — the gap is stochastic surface, and a\n"
				            "          pinned artefact would sit in it)\n",
				            learned, union_);
				dilate(mask, 2);
				long total = 0;
				for (const unsigned char pixel : mask.allowed) {
					total += pixel;
				}
				std::printf("learning: mask closed at %ld px (%.1f%% of the window)\n", total,
				            100.0 * total / windowArea);
				if (total > windowArea * 0.5) {
					std::printf("          REFUSING TO RUN: more than half the window is excused.\n");
					return 2;
				}
			}
			std::fflush(stdout);
			continue;
		}

		// THE NOISE GESTURE. Judged like a real one, but its verdict is a measurement rather than a
		// finding: it is the gesture that taught the mask, so anything it still reports is what this
		// venue produces when nothing is wrong. Three times that is the floor, so a defect has to be
		// three times the noise before it is called — and the number is printed, because a threshold
		// nobody can see is a threshold nobody can argue with.
		if (noiseGesture) {
			gStrayFloor = std::max(kStrayFloorMinimum, cycleWorst * 3);
			// The rest floor is measured from the same known-clean gesture's own rest phase, which
			// ran just above with the OLD floor still in force — so restFlickerWorst is the ambient
			// frame-to-frame change of an editor nobody is touching, which is exactly the number this
			// detector has to clear.
			gRestFloor = std::max(kRestFloorMinimum, restFlickerWorst * 3);
			std::printf("noise:    a known-clean gesture leaves %ld stray px (%ld over the top-left\n"
			            "          chrome); the floor for the run is %ld\n",
			            cycleWorst, cycleWorstTopLeft, gStrayFloor);
			std::printf("noise:    at rest it changes %ld px between consecutive frames; the rest\n"
			            "          floor is %ld\n",
			            restFlickerWorst, gRestFloor);

			// NOW judge the frames from the armed first gesture, with the finished mask and the
			// measured floors — the full detector, applied retrospectively to the one moment the
			// atlas can still grow. Timestamps are printed with every frame that fires so an
			// [VISAGE-ATLAS] resize line can be placed against a frame NUMBER, which is what decides
			// between "corrupt at the resize" and "corrupt for some other reason".
			if (!armedFrames.empty() && armedBaseline.valid()) {
				long armedWorst = 0;
				long armedWorstTopLeft = 0;
				int armedFired = 0;
				int armedFirstFiredAt = -1;
				for (size_t index = 0; index < armedFrames.size(); ++index) {
					const Verdict verdict = judge(armedFrames[index], armedBaseline, mask);
					armedWorst = std::max(armedWorst, verdict.stray);
					armedWorstTopLeft = std::max(armedWorstTopLeft, verdict.strayTopLeft);
					if (!verdict.fired()) {
						continue;
					}
					++armedFired;
					if (armedFirstFiredAt < 0) {
						armedFirstFiredAt = armedFrameNumbers[index];
						char kept[512];
						std::snprintf(kept, sizeof(kept), "%s/armed_first_f%02d.png", outDir.c_str(),
						              armedFrameNumbers[index]);
						FILE* ppm = std::fopen((outDir + "/probe_armed.ppm").c_str(), "wb");
						if (ppm != nullptr) {
							std::fprintf(ppm, "P6\n%d %d\n255\n", armedFrames[index].width,
							             armedFrames[index].height);
							std::fwrite(armedFrames[index].rgb.data(), 1, armedFrames[index].rgb.size(),
							            ppm);
							std::fclose(ppm);
							char convert[1200];
							std::snprintf(convert, sizeof(convert),
							              "convert %s/probe_armed.ppm %s 2>/dev/null", outDir.c_str(),
							              kept);
							if (std::system(convert) != 0) {
								std::printf("          WARNING: could not write %s\n", kept);
							}
						}
					}
					std::printf("armed:    *** frame %d FIRED *** %ld stray px, %ld over the top-left\n"
					            "          chrome, bbox %d,%d..%d,%d, at %lld\n",
					            armedFrameNumbers[index], verdict.stray, verdict.strayTopLeft,
					            verdict.bounds.left, verdict.bounds.top, verdict.bounds.right,
					            verdict.bounds.bottom, armedTimes[index]);
				}
				if (armedFired > 0) {
					++findings;
					std::printf("armed:    THE FIRST GESTURE ON A FRESH EDITOR IS DIRTY — %d of %zu\n"
					            "          frames fired, first at frame %d; saved "
					            "%s/armed_first_f%02d.png\n",
					            armedFired, armedFrames.size(), armedFirstFiredAt, outDir.c_str(),
					            armedFirstFiredAt);
				}
				else {
					std::printf("armed:    first gesture on a fresh editor is CLEAN — worst %ld stray "
					            "px\n          (%ld over the chrome) across %zu frames, floor %ld; "
					            "frames spanned %lld..%lld\n",
					            armedWorst, armedWorstTopLeft, armedFrames.size(), gStrayFloor,
					            armedTimes.front(), armedTimes.back());
				}
				// Released: they are large, and the run has many cycles still to go.
				armedFrames.clear();
				armedFrames.shrink_to_fit();
			}
			if (cycleWorstTopLeft > 200) {
				std::printf("          NOTE: that is a lot of unexplained ink over chrome that should\n"
				            "          not repaint at all. Worth a look before trusting the run.\n");
			}
			std::fflush(stdout);
			continue;
		}

		// THE GUARD THE FIRST RUN DID NOT HAVE. A cycle is only evidence if the hand in it did
		// something; one that wrote no values photographed a knob on its rail, and its verdict —
		// clean or otherwise — is about nothing. Checked per cycle rather than once at the end,
		// because that is the granularity at which it went wrong.
		const long cycleWrites = valueWrites - writesBefore;
		if (cycleWrites == 0) {
			std::printf("cycle %2d: INCONCLUSIVE — the gesture wrote no value. The knob was on its\n"
			            "          rail, so this cycle photographed a control nobody moved.\n",
			            cycle);
			++inconclusive;
			continue;
		}
		if (distinct < 2) {
			std::printf("cycle %2d: INCONCLUSIVE — %zu captures, %zu distinct. The window is not\n"
			            "          drawing (asleep, obscured, or frozen); this cycle proves nothing.\n",
			            cycle, names.size(), distinct);
			++inconclusive;
			continue;
		}
		// FLICKER IS ITS OWN FINDING, reported whether or not the absolute diff also fired. The two
		// detectors answer different questions — "is there ink that should not be there" and "is
		// something being redrawn while nobody is touching it" — and the observation is that the
		// artefact answers the SECOND one even when it is quiet enough to slip the first.
		if (restFlickerFrames > 0) {
			++flickerFindings;
			if (controlAt != nullptr) {
				++controlFlickerCaught;
			}
			const long long span = restLastFlickerAt - restFirstFlickerAt;
			std::printf("cycle %2d: *** SOMETHING IS REDRAWING AT REST *** %d of %d rest frames "
			            "changed,\n"
			            "          worst %ld px (rest floor %ld), bbox %d,%d..%d,%d, over %lld ms —\n"
			            "          cadence names the redrawer: audio-rate = the scope polyline,\n"
			            "          slow decay = the tray fade, twinkle = the sky\n",
			            cycle, restFlickerFrames, kRestFrames - 1, restFlickerWorst, gRestFloor,
			            restBounds.left, restBounds.top, restBounds.right, restBounds.bottom, span);
		}

		if (firstFiredFrame >= 0) {
			++findings;
			// THE CONTROL IS ONLY PASSED BY THE THING IT INJECTED. Counting any verdict would have
			// passed it on a hundred pixels of star field in a frame BEFORE the ink was drawn, which
			// is exactly what happened the first time this ran: a control that passes on noise
			// certifies nothing, and would have blessed a detector that could not see the injection
			// at all. The injected polygon puts tens of thousands of pixels over the top-left chrome;
			// nothing else observed here puts more than a hundred.
			if (controlAt != nullptr && cycleWorstTopLeft >= 5000) {
				++controlCaught;
			}
			std::printf("cycle %2d: *** INK OUTSIDE THE ALLOWED REGIONS *** at gesture frame %d (%s)\n"
			            "          %ld stray px, %ld of them in the TOP-LEFT QUADRANT, bbox "
			            "%d,%d..%d,%d\n"
			            "          the tray had %s spoken; %zu distinct frames; saved "
			            "%s/finding_c%02d_f%02d.png\n",
			            cycle, firstFiredFrame, framePaced ? "frame-paced" : "human-paced",
			            firstFired.stray, firstFired.strayTopLeft, firstFired.bounds.left,
			            firstFired.bounds.top, firstFired.bounds.right, firstFired.bounds.bottom,
			            trayWhenFired ? "ALREADY" : "NOT YET", distinct, outDir.c_str(), cycle,
			            firstFiredFrame);
			std::printf("          corner (top-left 100x100) was REPAINTED in %d frame(s)%s%s — a "
			            "stray\n          in a frame that did NOT repaint it was pinned earlier, not "
			            "drawn then\n",
			            cornerRepaints, cornerFrames.empty() ? "" : ": ", cornerFrames.c_str());
		}
		else {
			// The changed count is printed on a CLEAN cycle too, and that is deliberate: it is the
			// evidence that the frames were different pictures of a live editor rather than the same
			// picture of a dead one, which "clean" on its own cannot distinguish.
			std::printf("cycle %2d: clean (%s) — worst stray %ld px (floor %ld, %ld of it over the\n"
			            "          top-left chrome), %ld px changed somewhere allowed; %zu distinct "
			            "frames; tray %s; REST %ld px worst over %d frames%s\n",
			            cycle, framePaced ? "frame-paced" : "human-paced", cycleWorst, gStrayFloor,
			            cycleWorstTopLeft, cycleChanged, distinct,
			            trayHadSpoken ? "spoke" : "stayed silent", restFlickerWorst, kRestFrames,
			            restFlickerFrames > 0 ? " *** FLICKERING ***" : " (still)");
		}
		std::fflush(stdout);
	}

	std::printf("\nwrites:   %ld parameter values written by the gesture(s), %ld flush request(s)\n",
	            valueWrites, host.flushes);

	if (controlAt != nullptr) {
		// BOTH detectors have to prove themselves, because both are relied on. The absolute diff
		// must see ink over the chrome, and the rest-phase diff must see the SAME ink toggling while
		// nobody is touching anything. A run where only one fires ships half a blind instrument.
		std::printf("\nCONTROL:  absolute diff — the injected ink was %s\n",
		            controlCaught > 0
		                ? "CAUGHT over the chrome"
		                : "MISSED (blind: it never reported a large mark over the top-left chrome)");
		std::printf("CONTROL:  rest-phase diff — the toggling ink was %s\n",
		            controlFlickerCaught > 0 ? "CAUGHT flickering at rest"
		                                     : "MISSED (blind to a live-redrawn artefact)");
		std::remove(scratch.c_str());
		std::remove((outDir + "/probe_base.ppm").c_str());
		return controlCaught > 0 && controlFlickerCaught > 0 ? 0 : 1;
	}

	std::printf("\nRESULT:   %d ink finding(s), %d REST-FLICKER finding(s), %d inconclusive, of %d\n"
	            "          cycle(s); worst stray in any clean cycle was %ld px\n",
	            findings, flickerFindings, inconclusive, totalCycles, worstStray);
	// THE BLANK-FRAME GUARD ON THE VERDICT, in the same spirit as the one below: a count of clean
	// cycles is only a negative result if the frames behind it were photographs.
	if (blankCaptures > 0) {
		std::printf("          %ld CAPTURE(S) CAME BACK A SINGLE FLAT COLOUR and were discarded.\n"
		            "          A discarded frame is one the run did not judge, so this is a SHORT\n"
		            "          run rather than a clean one — compare the per-cycle distinct-frame\n"
		            "          counts against the canonical 42 (frame-paced) and 30 (human-paced).\n"
		            "          And if any landed during calibration the mask was built from them,\n"
		            "          which makes it too permissive and can excuse real ink. Re-run it.\n",
		            blankCaptures);
	}
	if (valueWrites == 0) {
		std::printf("          NOTHING WAS WRITTEN — the gesture never reached the parameter, so this\n"
		            "          is a photograph of an editor nobody touched. Not a negative result.\n");
	}
	std::printf("          A clean run means nothing until FEATHERS_PROBE_CONTROL=1 has been seen to\n"
	            "          fail on the injected known-bad.\n");

	std::remove(scratch.c_str());
	std::remove((outDir + "/probe_base.ppm").c_str());
	return findings > 0 ? 1 : 0;
}
