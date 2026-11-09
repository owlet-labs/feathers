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

// THE CONTROL FOR THE BATCH TRACE, in the same windowless venue as the atlas control.
//
// TWO CEILINGS, and it took measuring to learn which one matters. The one everybody talks about is
// the index space: `initTransientQuadBuffers` writes quad indices as `uint16_t`, quad `i` addressing
// vertices `4i..4i+3`, so past 16384 quads the values wrap. The one that bites FIRST is the transient
// vertex arena — a fixed per-frame pool, bgfx's 6 MB default, which visage never enlarges. At 112
// bytes per ShapeVertex that pool holds about 14000 quads for the WHOLE FRAME, below the index
// ceiling, so an oversized batch never reaches the wrap: the allocation fails and `setupQuads` skips
// the batch ENTIRELY, silently, because the only report there is compiled out under NDEBUG.
//
// WHAT IT ASSERTS, and each part is a different failure:
//   1. An ordinary batch is SILENT, and it writes every quad it allocated. A diagnostic that fires
//      on normal frames is one nobody reads — and the short-write half has to be asserted HERE,
//      first, because the reports are bounded at eight per process and a later round can be reading
//      a log that went quiet because the budget ran out rather than because nothing was wrong.
//   2. A batch approaching the lower ceiling warns, naming the shape type and the count.
//   3. A batch one past the INDEX ceiling reports OVERFLOW — and a batch exactly at it does not,
//      because the two are one apart and a ceiling reported one too low sends somebody hunting a
//      wrap that never happened.
//   4. A batch past the ARENA is reported and DRAWS NOTHING — checked in pixels, since that is the
//      claim. This round also prints the machine's measured arena capacity beside the index ceiling,
//      which is how a reader learns which of the two their build meets first.
//   5. Every RUN of a split batch rebinds its own uniforms. Checked with images, because they bind
//      an atlas and fills bind nothing — so rounds 3's halves would pass against a build that bound
//      its state once before the run loop, and this round's would not.
//   6. The count is NOT the shape count: a shape is batched once per damage rectangle it overlaps,
//      so 9000 retained shapes cross the ceiling when the frame repaints two areas instead of one.
//   7. The cursor resumes MID-SHAPE, on the damage rectangle it stopped on rather than that shape's
//      first. Asserted on the trace, not on pixels, for reasons written out at the round.
//   8. With the trace switched OFF, 2 goes quiet and 3 still reports. That is the half that fires in
//      an ordinary session, so it runs in a process of its own — see untracedOverflow.
//
// A SHORT WRITE — the batch allocating and indexing N quads and the writer filling fewer, so the
// tail indices address vertices nobody wrote — is the only failure here that draws GARBAGE rather
// than something missing. It cannot be provoked from a caller, because the count and the walk share
// their predicate and so agree by construction; what round 1 asserts is that an ordinary frame
// reports none, and the known-bad below is what gives that teeth.
//
// THE KNOWN-BADS, each one line, and all were run. Unwire `traceBatchQuads` and rounds 2, 3 and 6
// fail; unwire `traceBatchDropped` and round 4 fails; hoist `prepare()` out of the run loop and
// round 5's tail comes back BLACK (b0a0ffff wanted, 000000ff got) while its head stays right; drop
// `rect` from the QuadCursor and reset it on resume and round 7 reports UNWALKED; stop the walk a
// shape early and round 1 reports SHORT. Round 1's silence half passes under all of them, which is
// exactly why it is not the only thing here: silence proves nothing on its own.
//
// ROUND ORDER IS LOAD-BEARING. Round 5 attaches a Region that keeps its shapes, and those shapes go
// on painting over everything afterwards — which is what made round 4's pixel checks pass against a
// batch that had in fact been dropped, until they were moved ahead of it.
//
// WHY A REAL CANVAS RATHER THAN CALLING THE TRACE DIRECTLY. Calling it with a synthetic number would
// test the arithmetic and nothing else; the claim being checked is that the batcher's own count
// reaches it, through the batching that decides what a batch even is. So these are real shapes on a
// real canvas, counted by the code that allocates the buffer. It is deliberately drawn as thousands
// of IDENTICAL rectangles: `ShapeBatcher::autoBatchIndex` puts same-type shapes in one batch, and
// identical bounds make its overlap scan answer on the first area rather than walking all of them.
//
// STDERR IS CAPTURED rather than eyeballed, because "stays silent" has to be checked as strictly as
// "logs". The trace writes to stderr and flushes, so redirecting fd 2 to a file around each round is
// enough, and it is POSIX-only for the same reason the trace is.
//
// VISAGE_TRACE_BATCH IS SET BY THIS PROCESS, before the first visage call — the gate is a
// function-local static read once. Setting it also disarms the debug-build trap on overflow, which is
// what lets round 3 exist at all in a Debug configuration.
//
//   ninja -C build batch-overflow-check && build/batch-overflow-check

#include "visage_graphics/canvas.h"
#include "visage_graphics/graphics_utils.h"
#include "visage_graphics/region.h"
#include "visage_graphics/renderer.h"
#include "visage_graphics/shape_batcher.h"
#include "visage_graphics/screenshot.h"

#include <cstdint>
#include <limits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
	std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok) {
		failures++;
	}
}

// The same idea for a pixel comparison: "the halves differ" is not a finding, and "expected b0a0ff,
// got 000000" says which of the two failures happened — nothing bound, or the wrong thing bound.
void checkPixel(uint32_t got, uint32_t want, const char* what) {
	check(got == want, what);
	if (got != want) {
		std::printf("      wanted %08x, got %08x\n", want, got);
	}
}

// A failing round prints what it actually saw, because "the log did not contain X" is not a finding
// on its own — the useful half is which line it contained instead.
void checkLog(bool ok, const char* what, const std::string& log) {
	check(ok, what);
	if (!ok) {
		std::printf("      log was: %s", log.empty() ? "(empty)\n" : log.c_str());
	}
}

// Redirects fd 2 into a temporary file for the life of the object, and hands back what was written.
// The dup of the original descriptor is what makes it restorable — reassigning `stderr` itself would
// not survive the C library's buffering, and the trace's own `fflush` targets the FILE*, not the fd.
class CapturedStderr {
public:
	CapturedStderr() {
		std::snprintf(path_, sizeof(path_), "/tmp/feathers-batch-trace-XXXXXX");
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

// One round: throw away the last round's shapes (which also invalidates the whole region, so every
// shape below is counted rather than clamped away), draw `quads` rectangles, and submit.
std::string drawBatch(visage::Canvas& canvas, int quads) {
	CapturedStderr captured;
	canvas.clearDrawnShapes();
	canvas.setColor(0xffb0a0ff);
	for (int i = 0; i < quads; ++i) {
		canvas.rectangle(0, 0, 1, 1);
	}
	canvas.submit();
	return captured.text();
}

bool mentions(const std::string& log, const char* what) {
	return log.find(what) != std::string::npos;
}

// The one line that carries `marker`, or empty.
//
// A round often provokes more than one report — a batch of 40000 both overflows the index space and
// exhausts the arena — so "the log mentions Rectangle" can be satisfied by the wrong line entirely.
// A known-bad caught exactly that: with the dropped report unwired, the type-name check still passed
// because it was reading the overflow line.
std::string lineWith(const std::string& log, const char* marker) {
	const size_t at = log.find(marker);
	if (at == std::string::npos) {
		return {};
	}
	const size_t from = log.rfind('\n', at);
	const size_t to = log.find('\n', at);
	const size_t begin = from == std::string::npos ? 0 : from + 1;
	return log.substr(begin, to == std::string::npos ? std::string::npos : to - begin);
}

// Did a batch of this size reach the screen at all?
//
// TWO THINGS THIS HAS TO GET RIGHT, both of which fooled an earlier version of it. The readback lags
// a frame — bgfx copies the frame buffer asynchronously, so the first screenshot after new content
// returns the PREVIOUS one — and the frame buffer is never cleared between frames, so yesterday's
// pixels read as today's draw. Hence: paint black on purpose, then draw, then let the readback catch
// up before believing it.
// Draws `head` quads over the LEFT half and `tail` over the RIGHT, and reports which halves reached
// the screen.
//
// THIS IS THE SHAPE OF THE WRAP, which is why the two halves are separate. Quad `i` past the ceiling
// takes vertices `4i mod 65536`, so the TAIL of a batch redraws its HEAD: the left half appears twice
// and the right half never appears at all. A single-colour test could not see that; two positions can.
struct Halves {
	bool left = false;
	bool right = false;
};

Halves paintsHalves(visage::Canvas& canvas, int head, int tail) {
	canvas.clearDrawnShapes();
	canvas.setColor(0xff000000);
	canvas.fill(0, 0, 400, 200);
	canvas.takeScreenshot();
	canvas.takeScreenshot();

	canvas.clearDrawnShapes();
	canvas.setColor(0xffff0000);
	for (int i = 0; i < head; ++i) {
		canvas.fill(0, 0, 200, 200);
	}
	for (int i = 0; i < tail; ++i) {
		canvas.fill(200, 0, 200, 200);
	}
	canvas.takeScreenshot();
	canvas.takeScreenshot();
	const visage::Screenshot& shot = canvas.takeScreenshot();
	if (shot.width() == 0) {
		return {};
	}

	auto lit = [&shot](int x) {
		const uint8_t* p = shot.data() + (100 * shot.width() + x) * 4;
		return (p[0] | p[1] | p[2]) != 0;
	};
	return { lit(100), lit(300) };
}

// THE SAME TWO-HALF SHAPE, DRAWN AS IMAGES, and reporting the actual pixels rather than "lit".
//
// WHY IMAGES AND NOT FILLS. A split batch submits more than one draw, and `bgfx::submit` defaults to
// BGFX_DISCARD_ALL — so every uniform and texture bound for a draw is GONE after it. Fills survive
// that because they bind nothing per batch: their `prepare` is empty, and the half-checks above would
// pass just as well against a build that bound its state once before the run loop. Images cannot:
// `submitShapes<ImageWrapper>` binds the atlas uniform and texture, and a second run that inherits
// nothing samples nothing. So this is the round that can tell the two apart.
//
// The colours come back rather than a boolean because "run 2 drew the wrong thing" and "run 2 drew
// nothing" are different bugs, and only one of them is black.
struct HalfPixels {
	uint32_t left = 0;
	uint32_t right = 0;
};

uint32_t pixelAt(const visage::Screenshot& shot, int x, int y) {
	const uint8_t* p = shot.data() + (y * shot.width() + x) * 4;
	return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

HalfPixels imageHalves(visage::Canvas& canvas, const unsigned char* pixels, int size, int head, int tail) {
	canvas.clearDrawnShapes();
	canvas.setColor(0xff000000);
	canvas.fill(0, 0, 400, 200);
	canvas.takeScreenshot();
	canvas.takeScreenshot();

	canvas.clearDrawnShapes();
	canvas.setColor(0xffffffff);
	for (int i = 0; i < head; ++i) {
		canvas.imageRaw(pixels, size, size, visage::ImageFormat::BGRA8, 0, 0, 200, 200);
	}
	for (int i = 0; i < tail; ++i) {
		canvas.imageRaw(pixels, size, size, visage::ImageFormat::BGRA8, 200, 0, 200, 200);
	}
	canvas.takeScreenshot();
	canvas.takeScreenshot();
	const visage::Screenshot& shot = canvas.takeScreenshot();
	if (shot.width() == 0) {
		return {};
	}
	return { pixelAt(shot, 100, 100), pixelAt(shot, 300, 100) };
}

// NO CAPTURE OF ITS OWN, deliberately. An earlier version captured stderr in here to keep the console
// tidy, and a caller that wrapped it in a capture of its own then read an empty log — the inner one
// swallowed everything into a temp file and unlinked it. Capturing is the caller's business.
bool paints(visage::Canvas& canvas, int quads) {
	canvas.clearDrawnShapes();
	canvas.setColor(0xff000000);
	canvas.fill(0, 0, 400, 200);
	canvas.takeScreenshot();
	canvas.takeScreenshot();

	canvas.clearDrawnShapes();
	canvas.setColor(0xffff0000);
	for (int i = 0; i < quads; ++i) {
		canvas.fill(0, 0, 400, 200);
	}
	canvas.takeScreenshot();
	canvas.takeScreenshot();
	const visage::Screenshot& shot = canvas.takeScreenshot();
	if (shot.width() == 0) {
		return false;
	}

	const uint8_t* pixel = shot.data() + (100 * shot.width() + 200) * 4;
	return (pixel[0] | pixel[1] | pixel[2]) != 0;
}

constexpr const char* kUntracedArgument = "--untraced";

// THE OTHER HALF OF THE CLAIM: with the trace switched OFF, a big batch is silent and an overflowing
// one still reports. That is the half that fires in somebody's ordinary session, where nobody has set
// an environment variable, and it is the half a shipping build depends on — so it is worth a process
// of its own rather than an argument that it must surely work.
//
// In a Debug build this process is EXPECTED TO DIE: with the trace off the report is followed by
// VISAGE_ASSERT, which traps. The parent reads that as a pass, and says so.
int untracedOverflow() {
	// UNSET, NOT MERELY UNSET-BY-DEFAULT. This process is a child of one that turned the trace ON, and
	// a child inherits its parent's environment — so without this it would run traced and quietly
	// prove nothing. It cost a confusing failure to notice.
	unsetenv("VISAGE_TRACE_BATCH");

	visage::Canvas canvas;
	canvas.setWindowless(400, 200);
	if (!visage::Renderer::instance().supported()) {
		return 77;
	}

	// A batch big enough to warn if anyone were listening, and small enough to fit the arena — so
	// silence here means the warning half is genuinely opt-in, not that the batch died another way.
	const std::string quiet = drawBatch(canvas, 13000);
	checkLog(!mentions(quiet, "[VISAGE-BATCH]"), "untraced: a big batch says nothing", quiet);

	// The split report is opt-in, so untraced it must be silent — but a DROPPED batch is a real
	// failure and has to speak in a shipping build. That is the claim this round exists for.
	const std::string split = drawBatch(canvas, visage::kMaxQuadsPerBatch + 1);
	checkLog(!mentions(split, "[VISAGE-BATCH]"), "untraced: a split says nothing", split);

	CapturedStderr captured;
	canvas.clearDrawnShapes();
	canvas.setColor(0xffb0a0ff);
	for (int i = 0; i < 400000; ++i) {
		canvas.rectangle(0, 0, 1, 1);
	}
	canvas.submit();
	const std::string dropped = captured.text();
	checkLog(mentions(dropped, "[VISAGE-BATCH] DROPPED"),
	         "untraced: a dropped batch reports anyway", dropped);

	return failures == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
	// THE UNTRACED HALF RUNS AS ITS OWN PROCESS, and it has to: the gate is read once into a
	// function-local static, so one process cannot see the trace both on and off. The parent re-execs
	// itself with this argument once the traced rounds are done.
	if (argc > 1 && std::string(argv[1]) == kUntracedArgument) {
		return untracedOverflow();
	}

	// BEFORE ANY VISAGE CALL, for the same reason.
	setenv("VISAGE_TRACE_BATCH", "1", 1);

	visage::Canvas canvas;
	canvas.setWindowless(400, 200);
	if (!visage::Renderer::instance().supported()) {
		// An honest skip rather than a green tick: this machine cannot make a graphics context, and a
		// control that cannot run has not passed.
		std::printf("SKIP  no renderer: %s\n", visage::Renderer::instance().errorMessage().c_str());
		return 77;
	}

	std::printf("index ceiling %d quads per run; transient arena %d quads; warn at %d%% of the lower\n",
	            visage::kMaxQuadsPerBatch, visage::transientQuadCapacity(), visage::kQuadWarnPercent);

	// 1. AN ORDINARY BATCH IS SILENT. Without this the other two rounds would pass just as well
	// against a trace that printed on every frame, which would be useless.
	//
	// THE SHORT-WRITE CHECK LIVES HERE, FIRST, AND THAT POSITION IS LOAD-BEARING. Every report in
	// graphics_utils.h is bounded — eight per process, then silence — so "no SHORT line" is only
	// evidence in a round that runs while the budget is unspent. A version of this check sitting
	// after the split rounds PASSED against a build whose walk stopped a shape early, because the
	// eight reports had already been spent upstream and the round was reading an empty log. Absence
	// of a bounded report is not absence of the fault unless you know the bound is still there.
	const std::string quiet = drawBatch(canvas, 1000);
	check(!mentions(quiet, "[VISAGE-BATCH]"), "an ordinary batch traces nothing");
	checkLog(!mentions(quiet, "SHORT"), "and writes every quad it allocated and indexed", quiet);

	// 2. APPROACHING THE CEILING WARNS, and says what is approaching it. The type name is checked
	// because a bare count would not tell which of a frame's batches grew — which is the whole
	// reason the name is plumbed down through the allocation.
	//
	// `Rectangle` unqualified, not `visage::Rectangle`: the name is read out of the compiler's
	// signature for `batchTypeName`, and gcc writes a type the way it would be written from inside the
	// namespace the function lives in. The qualification is the compiler's to choose; the identifier
	// is what matters and it is what a reader needs.
	// Past 85% of the index ceiling and still inside it, so this round is about the WARNING alone and
	// is not tangled up with a split or a drop. The arena is no longer the lower ceiling — raising it
	// to 32 MB is what made the index space the binding one, which is exactly why the split below had
	// to land in the same commit.
	constexpr int kNearCeiling = 15000;
	const std::string warned = drawBatch(canvas, kNearCeiling);
	checkLog(mentions(warned, "[VISAGE-BATCH] warn"), "a batch near the ceiling warns", warned);
	checkLog(mentions(lineWith(warned, "warn"), "Rectangle"), "and the warning names the shape type",
	         warned);
	checkLog(mentions(warned, std::to_string(kNearCeiling).c_str()), "and carries the count", warned);
	check(!mentions(warned, "OVERFLOW"), "a batch under the index ceiling is not an overflow");
	checkLog(!mentions(warned, "DROPPED"), "and one that fits the arena is not dropped", warned);

	// 3. PAST THE INDEX CEILING THE BATCH IS SPLIT, AND THE FRAME IS CORRECT.
	//
	// This is the whole fix, and the two halves are how it is visible. Quad `i` past the ceiling would
	// take vertices `4i mod 65536`, so the TAIL of the batch would redraw its HEAD — the left half
	// twice, the right half never. Split into runs of at most 16384, both halves are drawn.
	//
	// THE HEAD MUST ITSELF EXCEED THE CEILING, and getting this wrong made the control prove nothing.
	// The first version drew 16000 left then 2000 right, and its known-bad PASSED: with no chunking,
	// quads 16000..16383 are still addressable, so the first 384 right-hand quads drew the right half
	// anyway and the wrap hid behind them. With 16500 left, every right-hand quad is past the ceiling,
	// so an unchunked build cannot draw that half at all.
	//
	// 16500 + 2000 = 18500 pieces: run one is 16384 of the left group, run two is the remaining 116
	// left plus all 2000 right. The boundary lands mid-group on purpose — a cursor that resumed on the
	// wrong piece shows up as a seam rather than as a missing half.
	{
		CapturedStderr captured;
		const Halves halves = paintsHalves(canvas, 16500, 2000);
		const std::string log = captured.text();
		checkLog(mentions(log, "[VISAGE-BATCH] SPLIT"), "a batch past the ceiling is split", log);
		checkLog(mentions(lineWith(log, "SPLIT"), "18500"), "and the split reports the batch total", log);
		checkLog(!mentions(log, "OVERFLOW"), "and nothing overflows any more", log);
		check(halves.left, "the head of a split batch is drawn");
		check(halves.right, "AND SO IS THE TAIL, which is the entire point");
	}

	// AND A BATCH THAT FITS IN ONE RUN IS NOT SPLIT. Without this, a version that split everything
	// would pass the round above while paying a draw call per batch for no reason.
	const std::string exact = drawBatch(canvas, visage::kMaxQuadsPerBatch);
	checkLog(!mentions(exact, "SPLIT"), "a batch exactly at the ceiling needs no split", exact);
	check(!mentions(exact, "OVERFLOW"), "and is not an overflow");

	// 4. THE ARENA IS STILL A CEILING, just a much higher one, and it still reports.
	//
	// The transient pool is now 32 MB — sized from tools/quad-census.cpp against the worst frame the
	// Explore surface actually asks for — so a batch has to be enormous to exhaust it. When one does,
	// the runs that already drew are KEPT: partial geometry rather than the old all-or-nothing loss.
	// Both halves of that are checked, because "reports" and "draws what it could" are separate claims.
	{
		CapturedStderr captured;
		const bool painted = paints(canvas, 400000);
		const std::string log = captured.text();
		checkLog(mentions(log, "[VISAGE-BATCH] DROPPED"), "a batch beyond the arena still reports", log);
		checkLog(mentions(lineWith(log, "DROPPED"), "Fill"), "and the report names the shape type", log);
		check(painted, "and the runs that fitted are kept, rather than the whole batch lost");
	}
	check(paints(canvas, 1000), "an ordinary batch paints");

	// 5. EVERY RUN REBINDS ITS OWN UNIFORMS, which round 3 cannot check and this one can.
	//
	// `bgfx::submit` defaults to BGFX_DISCARD_ALL: everything bound for a draw is released after it.
	// A split batch is more than one draw, so a shape type that binds state must bind it PER RUN. The
	// fills above bind nothing — `submitShapes<Fill>` passes an empty `prepare` — so their two halves
	// would come out right whether the binding sat inside the run loop or before it. Images bind the
	// atlas uniform and the atlas texture, so a second run that inherits nothing samples nothing.
	//
	// The geometry is round 3's on purpose: 16500 left + 2000 right, so run one is 16384 left quads
	// and EVERY right-hand quad falls in run two. The right half is therefore drawn entirely by the
	// run that would have nothing bound.
	//
	// The pixels are compared against an unsplit baseline rather than merely tested for "not black".
	// A run that binds the wrong texture draws something, and something is not a pass; the claim is
	// that a split batch draws what an unsplit one draws.
	{
		// Solid mid-lavender, in the 0xAARRGGBB order BGRA8 means, so the atlas takes it unswapped.
		constexpr int kImageSize = 8;
		std::vector<uint32_t> pixels(kImageSize * kImageSize, 0xffb0a0ffu);
		const auto* bytes = reinterpret_cast<const unsigned char*>(pixels.data());

		const HalfPixels baseline = imageHalves(canvas, bytes, kImageSize, 1, 1);
		check(baseline.left != 0 && baseline.right != 0, "an unsplit image batch paints both halves");

		CapturedStderr captured;
		const HalfPixels split = imageHalves(canvas, bytes, kImageSize, 16500, 2000);
		const std::string log = captured.text();
		checkLog(mentions(log, "[VISAGE-BATCH] SPLIT"), "an image batch past the ceiling is split", log);
		checkLog(mentions(lineWith(log, "SPLIT"), "Image"), "and the split names the image batch", log);
		checkPixel(split.left, baseline.left, "the head of a split image batch draws what one run draws");
		checkPixel(split.right, baseline.right,
		           "AND SO DOES THE TAIL, which is the run with nothing left bound");
	}

	// 6. AND THE COUNT IS NOT THE SHAPE COUNT, which is the part that makes the ceiling reachable.
	//
	// `setupQuads` emits one quad per shape PER INVALID RECT the shape overlaps — `numShapePieces`
	// counts overlaps, not shapes — and a layer keeps its damage as a list of non-overlapping
	// rectangles that a busy frame breaks into more, not fewer. So a frame repainting two separate
	// areas doubles every shape that spans both, and 9000 shapes overflow a ceiling of 16384.
	//
	// Nothing is redrawn between the two submits below. The same retained shapes are batched twice,
	// because there are two damage rectangles and each shape is inside both.
	visage::Region region;
	region.setBounds(0, 0, 400, 200);
	canvas.clearDrawnShapes();
	canvas.addRegion(&region);

	constexpr int kSpanning = 9000;
	{
		CapturedStderr captured;
		canvas.beginRegion(&region);
		canvas.setColor(0xffb0a0ff);
		for (int i = 0; i < kSpanning; ++i) {
			canvas.rectangle(0, 0, 400, 200);
		}
		canvas.endRegion();
		canvas.submit();
		const std::string log = captured.text();
		checkLog(!mentions(log, "[VISAGE-BATCH]"), "9000 shapes over one damage rect is quiet", log);
	}
	{
		CapturedStderr captured;
		region.invalidateRect({ 0, 0, 100, 200 });
		region.invalidateRect({ 300, 0, 100, 200 });
		canvas.submit();
		const std::string log = captured.text();
		checkLog(mentions(log, "[VISAGE-BATCH] SPLIT"),
		         "the same 9000 shapes over two damage rects need a split", log);
	}

	// 7. THE CURSOR RESUMES MID-SHAPE, on the damage rectangle it stopped on.
	//
	// A piece is a (shape, rect) PAIR, and a shape spanning several damage rectangles can straddle a
	// run boundary — so the cursor has to carry all three of batch, shape and rect. A cursor that
	// carried only batch and shape, and restarted the resumed shape at its FIRST rectangle, would
	// redraw one piece and run out of room for the last one.
	//
	// 6000 shapes over 3 disjoint damage rectangles is 18000 pieces: run one takes 16384, run two the
	// remaining 1616. Piece 16384 is shape 5461, rect 1 — mid-shape by construction, which is the
	// case a pair-cursor gets wrong.
	//
	// WHY THIS ROUND ASSERTS ON THE TRACE AND NOT ON PIXELS, which took a reviewer's finding to learn
	// and is the whole reason it is written this way:
	//
	//   * The pair-cursor fault is INVISIBLE in pixels. It drops exactly one piece — one shape's
	//     third damage rectangle — and this is a retained-mode canvas, so that rectangle simply keeps
	//     the ink it already had. Nothing on screen changes.
	//   * `takeScreenshot` COALESCES the damage rectangles, so a probe that photographs the result is
	//     not even looking at the three-rectangle frame it thinks it set up. Naive pixel probes here
	//     are vacuous rather than merely weak.
	//
	// What the fault does leave is an unfinished walk: after the last run the cursor still has a
	// piece in it. `traceBatchUnwalked` says so, in release, and that is what is checked here — plus
	// the count-and-walk agreement that SHORT reports from the other side.
	{
		CapturedStderr captured;
		canvas.clearDrawnShapes();
		canvas.beginRegion(&region);
		canvas.setColor(0xffb0a0ff);
		for (int i = 0; i < 6000; ++i) {
			canvas.rectangle(0, 0, 400, 200);
		}
		canvas.endRegion();
		canvas.submit();

		region.invalidateRect({ 0, 0, 100, 200 });
		region.invalidateRect({ 150, 0, 100, 200 });
		region.invalidateRect({ 300, 0, 100, 200 });
		canvas.submit();
		const std::string log = captured.text();
		checkLog(mentions(log, "[VISAGE-BATCH] SPLIT"), "6000 shapes over 3 damage rects are split", log);
		checkLog(mentions(lineWith(log, "SPLIT"), "18000"), "and the split counts 18000 pieces", log);
		checkLog(!mentions(log, "UNWALKED"), "and the walk finishes: no piece is left behind", log);
		checkLog(!mentions(log, "SHORT"), "and every run writes the whole chunk it was given", log);
	}

	// 8. A SHAPE AT A COORDINATE NOBODY COULD HAVE MEANT is reported, and an ordinary one is not.
	//
	// THE FAILURE CLASS EVERY OTHER ROUND HERE IS BLIND TO. SHORT, UNWALKED, DROPPED and OVERFLOW all
	// detect a DISAGREEMENT — a count against a walk, a request against an arena. A shape whose
	// POSITION is wrong disagrees with nothing: counted once, walked once, written once, drawn once,
	// correctly, at the wrong place. Every log stays clean and the frame is still wrong, which is
	// exactly what a silent trace beside a screenshot of a glitch means.
	//
	// Reported in RELEASE, because the frames where it matters are somebody's session.
	{
		CapturedStderr captured;
		canvas.clearDrawnShapes();
		canvas.setColor(0xffb0a0ff);
		canvas.rectangle(0, 0, 40, 40);
		canvas.submit();
		const std::string log = captured.text();
		checkLog(!mentions(log, "WILD"), "an ordinary rectangle is not a wild position", log);
	}
	{
		CapturedStderr captured;
		canvas.clearDrawnShapes();
		canvas.setColor(0xffb0a0ff);
		// A non-finite coordinate reaches setQuadPositions and writes NaN into vertex positions; what
		// the GPU makes of those is undefined, and in practice it is a triangle stretched across the
		// viewport. That is the filled-wedge costume of the glitch this round exists to name.
		canvas.rectangle(std::numeric_limits<float>::quiet_NaN(), 0.0f, 40.0f, 40.0f);
		canvas.submit();
		const std::string log = captured.text();
		checkLog(mentions(log, "[VISAGE-BATCH] WILD"), "a non-finite position is reported", log);
		checkLog(mentions(lineWith(log, "WILD"), "Rectangle"), "and the report names the shape type", log);
	}

	// 9. A QUAD IN THE CORNER ITS REGION CANNOT ACCOUNT FOR — and, just as much, THE REACH OF THAT
	// CHECK, which is the half that took measuring and changed what the tripwire is claimed to do.
	//
	// The detector fires when a quad lands in the top-left 100x100 of the surface while the region
	// drawing it sits elsewhere. Whether that can ever happen is decided by something one file over:
	// `Canvas::beginRegion` clamps every shape to its OWN region's bounds, so a shape wholly outside
	// its region is refused by `totallyClamped` before the batcher looks at its position at all.
	//
	// So the two halves below are not "silent" and "loud". They are two different faults, and only
	// one of them is visible from here:
	//
	//   * A shape handed an ABSOLUTE coordinate where a region-local one was wanted — the shape of
	//     bug this hunt was aimed at — is clamped away and draws NOTHING. It is not reported here,
	//     and it also never reaches the screen, so a fan photographed at the window corner did NOT
	//     arrive this way. That is a finding, not a gap, which is why it is asserted rather than
	//     quietly omitted.
	//   * A shape that OVERLAPS its region and still reaches the corner survives the clamp, draws,
	//     and is reported. That is the case the tripwire can actually see.
	//
	// A region at (150, 120) for both, since a region already in the corner is exempt by design.
	{
		visage::Region corner_region;
		corner_region.setBounds(150, 120, 100, 60);
		canvas.clearDrawnShapes();
		canvas.addRegion(&corner_region);

		{
			CapturedStderr captured;
			canvas.beginRegion(&corner_region);
			canvas.setColor(0xffb0a0ff);
			canvas.rectangle(10, 10, 40, 40);
			canvas.endRegion();
			canvas.submit();
			const std::string log = captured.text();
			checkLog(!mentions(log, "ORIGIN"), "a shape inside its own region is not an origin quad", log);
		}
		{
			CapturedStderr captured;
			canvas.clearDrawnShapes();
			canvas.beginRegion(&corner_region);
			canvas.setColor(0xffb0a0ff);
			// Exactly minus the region's position: the arithmetic of a caller handing over an absolute
			// zero. It lands on (0, 0) of the surface and is entirely outside its own region.
			canvas.rectangle(-150, -120, 40, 40);
			canvas.endRegion();
			canvas.submit();
			const std::string log = captured.text();
			checkLog(!mentions(log, "ORIGIN"),
			         "an absolute-zero shape is clamped away before the batcher sees it", log);
		}
		{
			CapturedStderr captured;
			canvas.clearDrawnShapes();
			canvas.beginRegion(&corner_region);
			canvas.setColor(0xffb0a0ff);
			// Starts at the same absolute zero but is big enough to overlap its own region, so the clamp
			// lets it through and it really does draw across the corner.
			canvas.rectangle(-150, -120, 200, 200);
			canvas.endRegion();
			canvas.submit();
			const std::string log = captured.text();
			checkLog(mentions(log, "[VISAGE-BATCH] ORIGIN"),
			         "a shape that survives its clamp and reaches the corner is reported", log);
			checkLog(mentions(lineWith(log, "ORIGIN"), "Rectangle"),
			         "and the report names the shape type", log);
			checkLog(mentions(lineWith(log, "ORIGIN"), "(150, 120)"),
			         "and names where the region drawing it sits", log);
		}
	}

	// 9b. THE CORNER-DAMAGE LINE, both edges.
	//
	// This renderer carries pixels forward — there is no full clear anywhere, only opaque fills over
	// the invalid rectangles — so ink outside a frame's damage survives to the next frame and the one
	// after. That makes "when was this area last repainted" the question that separates ink written
	// once and kept from ink drawn fresh every frame, and the line under test is what answers it.
	//
	// BOTH EDGES OR NEITHER. A line that only ever says BEGAN cannot mark the end of a repaint run,
	// and the GAP between runs is the entire point: it is the window in which anything drawn in the
	// corner was preserved untouched. So this drives damage into the corner, then damage that misses
	// it, and requires one line from each.
	{
		visage::Region corner;
		corner.setBounds(0, 0, 60, 60);
		visage::Region far_away;
		far_away.setBounds(250, 120, 80, 40);
		canvas.clearDrawnShapes();
		canvas.addRegion(&corner);
		canvas.addRegion(&far_away);

		// DRAIN FIRST. The line is edge-triggered and earlier rounds have already been damaging the
		// whole canvas, so the run is in progress when this round starts — and a BEGAN that was emitted
		// into a previous round's capture cannot be emitted again here. Two quiet submits put the edge
		// detector back to rest so the transition below is a real one. (Caught by this round failing
		// exactly that way.)
		canvas.submit();
		canvas.submit();

		{
			CapturedStderr captured;
			canvas.beginRegion(&corner);
			canvas.setColor(0xffb0a0ff);
			canvas.rectangle(0, 0, 60, 60);
			canvas.endRegion();
			canvas.beginRegion(&far_away);
			canvas.setColor(0xffb0a0ff);
			canvas.rectangle(0, 0, 80, 40);
			canvas.endRegion();
			// EXPLICIT DAMAGE, because drawing into a region does not by itself invalidate it — setBounds
			// did that once, and the drain above consumed it. Without this the round submits a frame with
			// no damage at all and then reports that the corner never repainted, which is true and
			// entirely uninformative.
			corner.invalidateRect({ 0, 0, 60, 60 });
			canvas.submit();
			const std::string log = captured.text();
			checkLog(mentions(log, "[VISAGE-DAMAGE] corner repaint BEGAN"),
			         "damage reaching the corner reports a repaint beginning", log);
		}
		{
			// Quiet frames: nothing invalidated, so the corner is NOT being repainted and the run ends.
			CapturedStderr captured;
			canvas.submit();
			canvas.submit();
			const std::string log = captured.text();
			checkLog(mentions(log, "[VISAGE-DAMAGE] corner repaint ENDED"),
			         "and the run ends once the corner stops being repainted", log);
		}
		{
			// Damage that misses the corner must NOT restart it — the half that makes a gap mean
			// something. Without this, a line that fired on ANY damage would pass the two checks above.
			CapturedStderr captured;
			far_away.invalidateRect({ 0, 0, 80, 40 });
			canvas.submit();
			canvas.submit();
			const std::string log = captured.text();
			checkLog(!mentions(log, "[VISAGE-DAMAGE] corner repaint BEGAN"),
			         "damage that misses the corner does not count as a corner repaint", log);
		}
	}

	// 9c. DOES THIS VENUE CARRY PIXELS FORWARD? The question any harness hunting a PINNED artifact has
	// to ask itself before it believes a single clean frame.
	//
	// A venue that full-redraws every present cannot reproduce pinning however faithfully it stages the
	// corruption — it would report clean frames forever while the VENUE, not the code, was the reason.
	// Reading the source says visage never clears the whole surface, only the invalid rectangles. This
	// checks the claim in pixels, here, in the venue the controls actually run in.
	//
	// Paint the corner, let it present, then damage something FAR AWAY and present again. If the
	// corner keeps its colour through a frame that never damaged it, the surface is retained and one
	// bad draw would survive exactly the same way.
	{
		visage::Region painted;
		painted.setBounds(0, 0, 60, 60);
		visage::Region elsewhere;
		elsewhere.setBounds(250, 120, 80, 40);
		canvas.clearDrawnShapes();
		canvas.addRegion(&painted);
		canvas.addRegion(&elsewhere);

		canvas.beginRegion(&painted);
		canvas.setColor(0xffff0000);
		canvas.rectangle(0, 0, 60, 60);
		canvas.endRegion();
		canvas.submit();
		canvas.takeScreenshot();
		canvas.takeScreenshot();
		const visage::Screenshot& first = canvas.takeScreenshot();
		if (first.width() == 0) {
			std::printf("SKIP  no screenshot readback for the carry-forward round\n");
		}
		else {
			const uint32_t cornerBefore = pixelAt(first, 10, 10);

			// Damage ONLY the far region. The corner is not in any damage rectangle this frame.
			elsewhere.invalidateRect({ 0, 0, 80, 40 });
			canvas.submit();
			check(!visage::lastFrameDamage().corner,
			      "the far-only frame really did leave the corner undamaged");
			check(visage::lastFrameDamage().rects > 0, "and it did damage something, so it is a real frame");

			canvas.takeScreenshot();
			canvas.takeScreenshot();
			const visage::Screenshot& second = canvas.takeScreenshot();
			const uint32_t cornerAfter = pixelAt(second, 10, 10);

			checkPixel(cornerAfter, cornerBefore,
			           "a frame that does not damage the corner leaves the corner's pixels alone");
			std::printf("note  venue carries pixels forward: corner survived an undamaged frame as %08x\n",
			            cornerAfter);
		}
	}

	// 10. AND NOW WITH THE TRACE OFF, in a process that never set the variable — see untracedOverflow.
	// A Debug build traps there by design, which is a pass and is reported as one.
	//
	// UNLESS THE TRACE CANNOT BE TURNED OFF. A FEATHERS_DIAGNOSTIC build compiles the gate open, so
	// the child process traces however little its environment says — and the round below, which
	// asserts silence, would fail for the one reason that is not a fault. Skipped out loud rather
	// than quietly passed: a round that cannot run has not run, and a green tick claiming otherwise
	// is exactly the lie the rest of this file exists to avoid.
	if (visage::diagnosticBuild()) {
		std::printf("SKIP  untraced rounds: this build compiles the trace in, so it has no off state\n");
		std::printf("%s\n", failures == 0 ? "PASS" : "FAILED");
		return failures == 0 ? 0 : 1;
	}

	const std::string self = std::string("\"") + argv[0] + "\" " + kUntracedArgument;
	std::fflush(stdout);
	const int status = std::system(self.c_str());
	if (WIFSIGNALED(status)) {
		std::printf("ok    untraced: the overflow assert fired (debug build traps, as designed)\n");
	}
	else {
		check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "untraced rounds passed");
	}

	std::printf("%s\n", failures == 0 ? "PASS" : "FAILED");
	return failures == 0 ? 0 : 1;
}
