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

#pragma once

// THE PLUGIN'S HALF OF A DIAGNOSTIC BUILD, and it is here to be paired with the renderer's half.
//
// The fork's atlas trace says when a glyph was packed and when the atlas repacked under already
// submitted draws. That answers "what did the text layer do". It cannot answer "what asked it to",
// because by the time a character reaches the font atlas every trace of who wanted it drawn is gone.
// This is the other end of the same wire: the tray is the ONE piece of text in this editor that
// changes only under a hand — the parameter API never calls say(), which is why storms driven
// through automation have been clean all day while a hand on a knob is not. So a say() line and a
// glyph line, on the same clock, are the two halves of a sentence.
//
// CLOCK_REALTIME IN MILLISECONDS, deliberately the same clock and the same units the fork's traces
// use, because the entire value of these lines is that they interleave with those in one log. A
// different epoch here — visage::time::milliseconds counts from something else — would produce two
// sequences that could never be laid against each other, or worse, could be laid against each other
// wrongly.
//
// UNBOUNDED. Its neighbours in the renderer are bounded because they report faults that should never
// happen; this reports a human being touching a control, which happens a few hundred times in a
// session and is the timeline everything else gets read against. A gap in it would be
// indistinguishable from a gap in the gestures.
//
// HEADER-ONLY ON PURPOSE. It adds no source file, so it needs no edit to a build file that another
// agent has work in flight in — and there is nothing here that wants a translation unit of its own.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string_view>

#ifdef FEATHERS_DIAGNOSTIC
#if defined(__GLIBC__) && !defined(_WIN32)
#define FEATHERS_HAVE_BACKTRACE 1
#include <csignal>
#include <cstring>
#include <execinfo.h>
#include <unistd.h>
#endif
#endif

namespace feathers::ui::diagnostic {

#ifdef FEATHERS_DIAGNOSTIC

inline long long milliseconds() {
	// POSIX only, like the traces this is meant to interleave with. See graphics_utils.h's note for
	// what this becomes if it is ever wanted off POSIX.
	struct timespec ts {};
	clock_gettime(CLOCK_REALTIME, &ts);
	return static_cast<long long>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

// WHETHER THE STRING IS NEW IS THE LOAD-BEARING FIELD, not the string. A repeated reading asks the
// font atlas for nothing — every glyph is already packed — so it is a gesture that cannot have
// caused atlas churn. A novel one is the event the whole hypothesis rests on. Printing both kinds
// and marking which is which is what lets a reader tell a busy drag from a churning one, and the
// distinction is free here while it is unrecoverable from the glyph lines alone.
/// WHERE THE OWLTT PLOT IS AND WHAT IT WAS TOLD — the line that ends a guess.
///
/// Two wrong fixes were shipped for bars that still did not drag, because the venue disagreed with the
/// host: in a full editor fixture the plot is reachable and the drag lands, and in Bitwig nothing in
/// the rectangle responds at all. When a measurement and a report disagree, the measurement is
/// asking a different question — and the only way to find WHICH is to instrument the thing the
/// report is about rather than reason about the thing the test is about.
///
/// THREE FACTS, BECAUSE EACH KILLS A DIFFERENT HYPOTHESIS. Bounds of zero say the layout never ran;
/// bounds that are right with no press line say the frame is unreachable in the tree; a press line
/// with `hit=0` says it is reachable and the hit test disagrees about where its own bars are.
inline void plotLaidOut(const char* surface, float x, float y, float w, float h, bool visible) {
	std::fprintf(stderr, "[FEATHERS-PLOT] %s laid out at (%.0f,%.0f) %.0fx%.0f visible=%d at %lld\n",
	             surface, x, y, w, h, visible ? 1 : 0, milliseconds());
	std::fflush(stderr);
}

inline void plotPressed(float x, float y, int hit, float w, float h) {
	std::fprintf(stderr, "[FEATHERS-PLOT] press at (%.1f,%.1f) in %.0fx%.0f -> hit=%d at %lld\n", x,
	             y, w, h, hit, milliseconds());
	std::fflush(stderr);
}

/// WHICH PART OF WHICH MODULATOR A PRESS RESOLVED TO — the branch `focusOrArm` is about to take.
///
/// A crash: clicking a background modulator while the OWLTT popout was focused. The stack is
/// clean and names `ModPanel::focusOrArm` +915, and the gestures before it are already in the log
/// from the plot diagnostics -- two crossover handles and two level strips on the panel's graph. What
/// is NOT in the log is which of that function's six branches ran, and they do very different things:
/// one opens a slot menu by walking device slots to rack slots, one names a source, one arms.
///
/// REPRODUCED IN THE FIXTURE AND IT DID NOT CRASH -- panel open, drag left deliberately unreleased,
/// modulator clicked through the real tree, twice. So the venue and the host disagree again, and the
/// only thing that closes that is instrumenting the host. `part` and the source are what pick the
/// branch — the source as its endpoint (kill-lanes step 2): the device's id, 0 for a host source, and
/// the output or host index.
inline void modPressed(int part, uint32_t sourceDevice, uint32_t sourceIndex, bool armed) {
	std::fprintf(stderr, "[FEATHERS-MOD] press part=%d source=%u.%u armed=%d at %lld\n", part, sourceDevice,
	             sourceIndex, armed ? 1 : 0, milliseconds());
	std::fflush(stderr);
}

inline void said(std::string_view text, bool novel) {
	std::fprintf(stderr, "[FEATHERS-SAY] %s \"%.*s\" at %lld\n", novel ? "new " : "same",
	             static_cast<int>(text.size()), text.data(), milliseconds());
	std::fflush(stderr);
}

// THE SCOPE'S POLYLINE, WATCHED WHERE IT IS COMPUTED — the one place in this editor where a
// finite-but-wrong coordinate can be caught before anything downstream hides it.
//
// WHY THIS NEEDS ITS OWN LINE when the batcher already carries an origin tripwire. Two reasons, and
// both are about what the batcher cannot see:
//
//   * A point zeroed in LOCAL coordinates draws from the scope's OWN top-left corner, not the
//     window's. The batcher's check measures against the surface and exempts a region already in the
//     corner, so a fan anchored on this widget is invisible to it and entirely visible to a person.
//   * A NON-FINITE point never reaches the batcher at all: the loop refuses it and draws nothing.
//     That refusal is correct, and it is also why WILD has stayed silent — the value is destroyed one
//     frame before anything is in a position to name it.
//
// The at-rest flicker is what makes this worth a line. The artifact is redrawn every frame rather
// than retained, and of the three things that redraw with no hand on a control — starfield twinkle,
// tray fade, and this — this is the only one whose coordinates come out of an evaluated character
// rather than a clamped parameter.
//
// BOUNDED THREE WAYS, because a fault here fires per point per frame and an unbounded line would
// fill a disk during the very session it exists to survive. The FIRST occurrence always, since that
// is the one carrying the timestamp that gets laid against a screenshot; then every 100th, so the
// rate is visible; then silence past 200 lines.
//
// AND BOUNDED PER REASON RATHER THAN IN TOTAL, which a control caught and is not a refinement. With
// one shared counter, a known-bad carrying BOTH a zeroed point and a non-finite one reported only
// the zeroed: the second anomaly was occurrence two, and every-100th swallowed it. In a real session
// that is the difference between naming the mechanism and missing it — a fan produces its own kind
// of anomaly hundreds of times a frame, and the rare different one underneath it is exactly the
// thing worth seeing. Three counters cost three words.
enum class ScopeFault { NonFinite, Zeroed, WindowCorner };

inline void scopePoint(std::size_t index, ScopeFault fault, float x, float y, float window_x,
                       float window_y) {
	static constexpr int kEvery = 100;
	static constexpr int kMaxReports = 200;
	static constexpr const char* kNames[] = { "non-finite", "zeroed", "window-corner" };
	static std::atomic<int> seen[3] { { 0 }, { 0 }, { 0 } };
	static std::atomic<int> reported[3] { { 0 }, { 0 }, { 0 } };

	const auto which = static_cast<std::size_t>(fault);
	const int ordinal = seen[which].fetch_add(1, std::memory_order_relaxed) + 1;
	if (ordinal != 1 && ordinal % kEvery != 0) {
		return;
	}

	const int lines = reported[which].fetch_add(1, std::memory_order_relaxed) + 1;
	if (lines > kMaxReports) {
		return;
	}

	std::fprintf(stderr, "[FEATHERS-SCOPE] #%d %s point %zu at (%g, %g) -> window (%g, %g) at %lld%s\n",
	             ordinal, kNames[which], index, static_cast<double>(x), static_cast<double>(y),
	             static_cast<double>(window_x), static_cast<double>(window_y), milliseconds(),
	             lines == kMaxReports ? " (further reports of this kind suppressed)" : "");
	std::fflush(stderr);
}

// THE BUILD SAYING WHAT IT IS, once, before anything else can be misread as ordinary.
//
// A diagnostic install lives in the same place as a release install and answers to the same name, so
// the only thing standing between it and being mistaken for one is this line. It is also the marker
// that tells a reader of engine.log which build produced the lines below it — a log without it is a
// log from a build that was NOT instrumented, which is exactly the confusion a compiled-in trace
// exists to prevent.
//
// An inline variable, so it is one object across every translation unit that includes this, and its
// constructor runs when the plugin binary is loaded. No destructor and no state: static destruction
// order has already cost this project one crash, and nothing here is worth a second.
#ifdef FEATHERS_HAVE_BACKTRACE

/// WHERE A CRASH GOES WHEN NOBODY IS WATCHING A TERMINAL.
///
/// Two SIGSEGVs on 2026-08-24, both in Bitwig, both with a good trace of what the HAND did and no
/// stack at all: no core dump, and Bitwig writes no plugin crash log. A one-shot fault with no stack
/// is nearly worthless; the same fault with one is often the whole answer. So the diagnostic build
/// prints its own.
///
/// EVERYTHING HERE IS ASYNC-SIGNAL-SAFE, which rules out the obvious spelling. `backtrace_symbols`
/// allocates and `fprintf` takes a lock — either can deadlock or fault a second time inside a handler
/// that is already standing on a broken stack. `backtrace_symbols_fd` writes the frames itself with
/// no allocation, and `write` is on the safe list, so those are what this uses.
///
/// IT RE-RAISES rather than returning or exiting. The host has to see the process die the way it
/// actually died — Bitwig reports "exited with code 139" and that number is the diagnosis's first
/// line. Swallowing the signal would trade a crash we can read for a hang we cannot.
///
/// GATED ON FEATHERS_DIAGNOSTIC, so a shipping build installs nothing and the audio path is
/// untouched either way: a handler that never fires costs one sigaction at load.
/// A WRITE WHOSE RESULT IS DELIBERATELY DROPPED, and said so rather than cast away silently. There is
/// no recovery available inside a crash handler: if stderr is full or closed there is nowhere to
/// report that, and the next statement is the one that matters. -Werror is right to ask.
inline void emit(const char* text, std::size_t length) {
	const ssize_t written = ::write(STDERR_FILENO, text, length);
	(void)written;
}

/// WHICH BUILD THIS IS, as a string the binary itself contains.
///
/// A STACK IS A LIST OF OFFSETS AND AN OFFSET IS ONLY MEANINGFUL AGAINST THE BINARY IT CAME FROM.
/// `resolve-crash.sh` reads whatever is in the slot NOW, and a host does not reload a `.clap` that is
/// already open -- so an install between the crash and the resolve produces a stack of real function
/// names that are all the WRONG functions. That happened: a crash resolved to `MsegCard::draw`
/// calling `showChoiceMenu` calling `Canvas::fill` calling `ApplicationEditor::addToWindow`, which is
/// not a call chain that exists. Every frame landed inside some real symbol, so the resolver's
/// >64KB distance guard never fired.
///
/// CLAUDE.md's OWN RULE, ARRIVING FROM A NEW DIRECTION: an address is only a valid identity while the
/// thing at it cannot be replaced. Installing replaces it.
///
/// SO THE CRASH SAYS WHICH BUILD IT IS, and the string is compiled INTO the binary -- which means the
/// resolver can check it is reading the right file by looking for that same string in the file. Two
/// builds minutes apart differ, and a binary that does not contain the stamp is not the one that
/// crashed.
///
/// ASCII ONLY, DELIBERATELY. `strings` breaks a run at a non-ASCII byte, and this repo has already
/// lost an hour to `strings | grep 'CRASH — signal'` returning nothing because of the em-dash three
/// lines below. The stamp must survive the tool that will look for it.
static constexpr char kBuildStamp[] = "FEATHERS-BUILD " __DATE__ " " __TIME__;

inline void crashHandler(int sig) {
	// THE BUILD FIRST, so a truncated log still says which binary to resolve against.
	emit("\n[FEATHERS] ", 12);
	emit(kBuildStamp, sizeof(kBuildStamp) - 1);
	emit("\n", 1);
	static constexpr char kHead[] = "\n[FEATHERS] CRASH — signal ";
	emit(kHead, sizeof(kHead) - 1);
	const char digit = static_cast<char>('0' + (sig % 10));
	const char tens = static_cast<char>('0' + ((sig / 10) % 10));
	if (sig >= 10) {
		emit(&tens, 1);
	}
	emit(&digit, 1);
	static constexpr char kTail[] = " — stack follows, innermost first\n";
	emit(kTail, sizeof(kTail) - 1);

	void* frames[64];
	const int depth = ::backtrace(frames, 64);
	::backtrace_symbols_fd(frames, depth, STDERR_FILENO);

	static constexpr char kEnd[] = "[FEATHERS] end of stack\n";
	emit(kEnd, sizeof(kEnd) - 1);

	// THE DEFAULT HANDLER, THEN THE SAME SIGNAL AGAIN, so the process dies of what killed it and the
	// host's exit code still says 128 + sig rather than something this handler invented.
	std::signal(sig, SIG_DFL);
	::raise(sig);
}

inline void installCrashHandler() {
	for (const int sig : { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT }) {
		std::signal(sig, &crashHandler);
	}
}

#endif // FEATHERS_HAVE_BACKTRACE

struct Banner {
	Banner() {
		std::fprintf(stderr, "[FEATHERS] DIAGNOSTIC BUILD - atlas, batch and say() traces are on\n");
#ifdef FEATHERS_HAVE_BACKTRACE
		installCrashHandler();
		std::fprintf(stderr, "[FEATHERS] crash handler installed — a fault prints its stack here\n");
#endif
		std::fflush(stderr);
	}
};

inline const Banner banner;

#else

enum class ScopeFault { NonFinite, Zeroed, WindowCorner };

inline void said(std::string_view, bool) { }
inline void scopePoint(std::size_t, ScopeFault, float, float, float, float) { }

#endif

}  // namespace feathers::ui::diagnostic
