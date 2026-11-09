#!/usr/bin/env bash
# Pre-flight our sources under clang, so macOS CI stops being the first thing to say no.
#
# This file is part of Feathers.
# Copyright (C) 2026 Owlet Records — GPL-3.0-or-later.
#
# READ THE EXIT CODE UNPIPED, OR TAKE ${PIPESTATUS[0]}. MEASURED 2026-08-30, seconds apart on one
# tree: `clang-preflight.sh 2>&1 | tail -8; echo "exit $?"` reports exit 0 while the same run
# redirected to a file reports exit 1 — because `$?` after a pipeline is the LAST command's status,
# which is `tail`'s. This script prints a long log, so piping it to shorten the output is the natural
# thing to do, and that is exactly the act that turns a required check green.
#
# The VERDICT is always the last line of text on both paths, so a piped reader still sees the truth —
# it is only `$?` that lies. Playbook 8n-bis.
#
# WHY THIS EXISTS. The Linux build is gcc and the macOS leg is clang with -Werror, and clang has
# whole diagnostic classes gcc simply does not have: -Wunused-const-variable, -Wunused-private-field,
# -Wunused-lambda-capture, -Wbraced-scalar-init. A green local build says NOTHING about them. macOS CI
# was red for four days on one unused constant and nobody knew, because branch pushes deliberately do
# not build; the first dispatch found it, and then found a second class one run later. At ~10x the
# runner cost of Linux, discovering these one per round trip is the expensive way to read a compiler
# message you can read here in a minute.
#
# WHEN macOS IS RED, USE THE SURVEY INSTEAD OF THIS. `gh workflow run build.yml --ref <branch>
# -f survey_warnings=true` builds with -Werror OFF, so the compiler does not stop at the first
# problem and the log names EVERY warning at once. MEASURED: five dispatches found five warnings, one
# per run, and then one survey run listed all six that remained. This script is the free first pass;
# the survey is the authoritative one, and it is the right tool the moment a red macOS build appears.
# (Read the survey log for BOTH src/ and tests/ — filtering to src/ once hid five of six.)
#
# WHAT IT IS NOT. clang-14 here is not Apple clang, and this compiles with libstdc++ rather than
# libc++, so a clean run is not a promise the macOS leg is green — it is the removal of the classes
# that have actually broken it. Treat silence as "these classes are clear", never as "CI will pass".
#
# AND A VERSION BLIND SPOT THAT IS NOT FIXABLE HERE, DEMONSTRATED: clang-14 compiles
# tests/test_gui_bridge.cpp clean under -Wall -Wextra and emits NO -Wmissing-braces where the macOS
# runner's newer clang emits three. Same for a field bound in a member-init list and never read.
# So a clean run here CANNOT mean the macOS leg is green — only that the classes this compiler knows
# about are clear. Phase 3 below exists precisely because one of those gaps was worth covering by
# hand; the rest are not, and that is what the survey is for.
#
# AND NOW THE CONVERSE, WHICH IS THE HALF THAT WAS MISSING AND IS EASIER TO GET WRONG: AN EXIT 1 IS
# NOT A PROOF THAT macOS WOULD HAVE FAILED. The paragraph above says a green is not a promise CI
# passes, because clang-14 misses classes the newer compiler emits. The same version gap runs the
# other way — clang-14 can emit classes a NEWER compiler has since dropped — and a red is far more
# tempting to over-read than a green, because it arrives with a file, a line and a diagnostic name
# and looks like a fact about CI.
#
# MEASURED, on the first sweep this repo ever ran (2026-09-04, backlog BZ): it reported
# `-Wdeprecated-volatile` on a benchmark's `sink += x` where `sink` is `volatile float`. Real for
# clang-14, which implements the C++20 rule. But g++-14 emits NOTHING for that construct at
# `-std=c++17`, `c++20` OR `c++23`, because GCC applies P2327 — which de-deprecated volatile compound
# assignment — as a defect report. So the two compilers genuinely disagree, and whether the macOS leg
# breaks turns on the Apple clang version, which nobody had measured when the finding was reported as
# "a genuine -Werror break". It may well be one; that is not the point. THE POINT IS THAT THIS SCRIPT
# CANNOT TELL YOU, IN EITHER DIRECTION.
#
# SO READ A RED THE WAY YOU READ A GREEN: as "clang-14 on libstdc++ says this class is present here",
# never as "CI would have failed". The right response is still usually to fix it — the fixes for that
# first sweep were correct under every standard and cost zero instructions, which is the cheap case
# and the common one. But when a fix would cost something real, or would weaken something
# load-bearing, the version question has to be settled against the actual runner FIRST rather than
# conceded to this script's opinion. An instrument honest in only one direction invites exactly the
# error it was built to prevent, one reflection away.
#
# AND ITS ONE STRUCTURAL BLIND SPOT, WHICH BIT IMMEDIATELY: THIS RUNS WITH LINUX DEFINES, so it
# cannot see code that is only dead on ANOTHER platform. `xServerDpi()` in gui.cpp is called from the
# `#else` of an `#if defined(__APPLE__)`, so it is live here and unused there — clean under this
# script, `-Wunused-function` on macOS, and it cost a CI run the run after this script was written to
# stop costing CI runs. **When you touch anything inside a platform `#if`, this script has nothing to
# say about the other branch.** Read the guards by hand, and check that a helper's definition is
# guarded by the same condition as its only caller.
#
# AND THE BLIND SPOT NOBODY HAS BEEN BITTEN BY YET, WHICH IS WHY IT IS WORTH WRITING DOWN: THE
# STANDARD LIBRARY, NOT THE COMPILER. This runs on libstdc++; the macOS leg is libc++, and libc++
# includes far less transitively. So a TU that uses `uint32_t`, `std::move`, `std::size_t` or
# `std::find` WITHOUT including `<cstdint>`, `<utility>`, `<cstddef>` or `<algorithm>` compiles here
# because something else dragged the header in, and fails on macOS with a HARD ERROR rather than a
# warning.
#
# **THAT CLASS IS INVISIBLE TO EVERY FLAG ABOVE AND TO PHASE 3.** Nothing in -Wall -Wextra reports a
# header you got by luck, and a phase-3 grep cannot tell a transitively-available name from a
# deliberately-included one. It is also the class that a NEW FILE is most likely to introduce, which
# makes it the one to think about after an extraction: the file that used to sit in a big TU with
# fifty includes now has its own six.
#
# MARKED AS REASONED FOR THE BREAKAGE, MEASURED FOR THE EXPOSURE, and the two are different claims.
# The libstdc++/libc++ divergence is a documented property of the two libraries; that it has ever
# broken THIS repo's macOS leg is NOT established and no run here can establish it. Promote this to
# DEMONSTRATED with the file name if it ever bites, and do not leave it standing as folklore.
#
# THE EXPOSURE, HOWEVER, IS MEASURED, AND IT WAS FOUND BY WRITING THIS PARAGRAPH. Auditing the five
# files added by the backplane branch minutes later turned up three names taken on loan:
# `surface_host.h` used `std::move` with no `<utility>`, and `takeover_stack.h` used `std::function`
# and `std::unique_ptr` while including neither `<functional>` nor `<memory>` — it had them from
# `surface_host.h`. All three compiled clean here and under every flag above. **Five new files, three
# borrowed names**, which is the rate to expect from new code rather than a story about one branch,
# and it is exactly the prediction the paragraph above makes about extraction.
#
# WHAT WOULD ACTUALLY CLOSE IT is not another local flag: it is `-nostdinc++` against a libc++ tree,
# or the survey. Both cost more than this script is for.
#
# HOW IT WORKS: takes the real compile lines out of ninja's compdb, keeps only the include and define
# flags, and runs clang -fsyntax-only over each of OUR TUs. Vendored code is excluded — libs/ holds
# somebody else's headers and holding them to our warnings is not a bug we can fix.
#
# "OUR TUs" IS ANCHORED AT THE REPO ROOT, AND THAT IS A FIX, NOT A STYLE CHOICE (backlog BX). It used
# to ask for the SUBSTRING '/src/' or '/tests/', excluding '/libs/'. Both halves were wrong on this
# host and neither showed it: the checkout lives under `$HOME/src/feathers`, so `'/src/' in f`
# matched the HOME PATH and therefore every absolute path on the machine; and bgfx/bimg/astc-encoder
# arrive via FetchContent under `build/_deps/`, which is not `libs/`, so the vendored exclusion only
# ever covered the submodules. MEASURED before the fix: 375 TUs scanned where 178 are ours — 197
# files of somebody else's, at roughly double the runtime this header quotes. The warning filter
# further down was always anchored correctly, which is the only reason this never reported a finding
# about foreign code; it cost time and a false headline, not a false green.
#
# TESTS ARE SCANNED TOO, and leaving them out cost a CI run on its own. The first version looked at
# src/ only; the macOS leg builds the suites with the same -Werror, and five of the six warnings in
# the survey that finally listed them all were in tests/. A pre-flight that skips half of what CI
# compiles is a pre-flight that reports clean and is wrong.
#
#   Usage:  scripts/clang-preflight.sh [build-dir]      (default: build)
#
# NOTE ON `-w`: do not add it. In clang, `-w` is a hard suppress that later `-W` flags do NOT
# re-enable, so the scan goes silent and reports success having asked nothing. That cost one round of
# debugging here, and it is the same shape as every other venue that cannot express its outcome.
set -uo pipefail

BUILD=${1:-build}
CLANG=${CLANG:-$(command -v clang++ || echo /usr/lib/llvm-14/bin/clang++)}

# THE REPO ROOT, COMPUTED ONCE AND USED BY BOTH FILTERS. It was previously computed inline by the
# warning filter alone, while the scan-set filter used a substring — two ways of asking one question,
# which is how they came to disagree without either looking wrong. See backlog BX.
root=$(cd "$(dirname "$0")/.." && pwd)

if [[ ! -x $CLANG ]]; then
	echo "clang-preflight: no clang++ found (set CLANG=/path/to/clang++)" >&2
	exit 2
fi
if [[ ! -f $BUILD/build.ninja ]]; then
	echo "clang-preflight: no ninja build in '$BUILD'" >&2
	exit 2
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# NOT REDIRECTED TO /dev/null, and the emptiness is checked. The first version of this script
# swallowed ninja's stderr, produced a zero-byte compdb because `ninja` was not on PATH, scanned
# NOTHING, and printed "clean" — a pass that had asked no question. That is the failure this whole
# script exists to prevent, reproduced inside it on its first run.
if ! ninja -C "$BUILD" -t compdb > "$tmp/compdb.json"; then
	echo "clang-preflight: could not read a compile database from '$BUILD'." >&2
	echo "                 Did you '. scripts/env.sh' first? ninja must be on PATH." >&2
	exit 2
fi
if [[ ! -s $tmp/compdb.json ]]; then
	echo "clang-preflight: the compile database is empty — nothing would be scanned." >&2
	exit 2
fi

CLANG="$CLANG" ROOT="$root" python3 - "$tmp/compdb.json" > "$tmp/scan.sh" <<'PY'
import json, io, shlex, sys, os
db = json.load(io.open(sys.argv[1]))
clang = os.environ["CLANG"]
# ANCHORED, not a substring — see the header and backlog BX. `'/src/' in f` matched the home path.
root = os.environ["ROOT"].rstrip('/') + '/'
print('cd %s' % shlex.quote(db[0]['directory']))
seen = set()
for e in db:
	f = e['file']
	if not (f.startswith(root + 'src/') or f.startswith(root + 'tests/')) or '/libs/' in f or f in seen:
		continue
	# C++ TRANSLATION UNITS ONLY: the compile database also lists bgfx shader sources (.sc), which clang
	# reads as linker inputs and warns about, noise that is not ours.
	if not f.endswith(('.cpp', '.cc', '.cxx', '.mm')):
		continue
	seen.add(f)
	args, keep, i = shlex.split(e['command']), [], 0
	while i < len(args):
		a = args[i]
		if a in ('-I', '-isystem', '-include'):
			keep += [a, args[i + 1]]; i += 2; continue
		if a.startswith(('-I', '-D', '-isystem', '-std=')):
			keep.append(a); i += 1; continue
		i += 1
	# -DFEATHERS_FORCE_SIMDE: the SSE2 headers take macOS's arm64 road (SIMDe), so a target missing SIMDe's
	# include path fails HERE ("simde/x86/sse2.h not found" reached CI first on 2026-10-06).
	cmd = [clang, '-fsyntax-only', '-Wall', '-Wextra', '-DFEATHERS_FORCE_SIMDE'] + keep + [f]
	print(' '.join(shlex.quote(x) for x in cmd) + ' 2>&1 || true')
PY

# Only OUR files: a warning inside a vendored header is not ours to fix.
tus=$(( $(wc -l < "$tmp/scan.sh") - 1 ))   # every line but the leading `cd`
if (( tus < 1 )); then
	echo "clang-preflight: the compile database named no TU of ours — refusing to report clean." >&2
	exit 2
fi

# ERRORS AS WELL AS WARNINGS. This read only "warning:", so a hard error (a header not found, which is how
# a missing include path shows) was filtered out and the run reported clean — the failure this script exists
# to prevent. 2026-10-06.
# ONE ERROR CLASS IS FILTERED, A VERSION GAP: clang-15 rejects a lambda capturing a C++20 structured binding
# ("reference to local binding … declared in enclosing function"); clang 16+ and the macOS runner's Apple
# clang accept it, and the same tests have built on macOS CI. 50 such lines on 2026-10-06, all false.
out=$(bash "$tmp/scan.sh" 2>/dev/null \
      | grep -E "warning:|error:" \
      | grep -v "reference to local binding .* declared in enclosing function" \
      | grep -E "$root/(src|tests)/" \
      | grep -v "/libs/" \
      | sort -u)

if [[ -n $out ]]; then
	echo "$out"
	n=$(wc -l <<<"$out")
	echo
	echo "clang-preflight: $n warning(s) in our sources — macOS builds these with -Werror."
	exit 1
fi
# PHASE 3 (below, after phase 2): INIT-ONLY MEMBERS. clang-14 does not warn about a field that is
# bound in a constructor's member-init list and never read again; NEWER clang, including the macOS
# runner's, does. So this is a VERSION blind spot on top of the platform one, and it cost a run of its
# own: `OwlttCard::notifier_`. Detected textually instead — a member whose every mention in the .cpp is
# inside a mem-initializer.
#
# PHASE 2: THE BLIND SPOT ABOVE, PARTLY COVERED. clang here compiles the Linux branches, so it cannot
# see a helper that is live here and dead on macOS. This tracks `#if` nesting and flags any file-local
# function whose DEFINITION is outside a platform guard while EVERY call site sits inside one — which
# is exactly the shape of xServerDpi(). It is a heuristic over a crude definition regex, not a parser;
# it catches the shape that has actually broken the build.
python3 - "$(cd "$(dirname "$0")/.." && pwd)" <<'GUARDS' || exit 1
import io, re, glob, sys, os
root = sys.argv[1]
os.chdir(root)
# SRC AND TESTS, AND ANY PLATFORM GUARD: test_synth's kPerVoiceKeytrackDigest was used only under
# `__x86_64__ && __linux__` and reached macOS CI as unused-const-variable on 2026-10-06, unseen because this
# walked src/ files naming __APPLE__ only, and functions only.
files = [f for f in glob.glob('src/**/*.cpp', recursive=True) + glob.glob('src/**/*.h', recursive=True)
         + glob.glob('tests/*.cpp', recursive=False) + glob.glob('tests/gui_widgets/*.cpp', recursive=False)
         if any(k in io.open(f, encoding='utf-8', errors='ignore').read() for k in ('__APPLE__', '__linux__', '_WIN32'))]
bad = 0
for path in files:
	lines = io.open(path, encoding='utf-8', errors='ignore').read().split('\n')
	stack, ctx = [], []
	for ln in lines:
		t = ln.strip()
		if t.startswith('#if'):
			stack.append(t)
		elif t.startswith(('#elif', '#else')):
			if stack: stack[-1] += ' |' + t
		elif t.startswith('#endif'):
			if stack: stack.pop()
		ctx.append(' >> '.join(stack))
	defs = {}
	for i, ln in enumerate(lines):
		m = re.match(r'^(?:static\s+)?(?:inline\s+)?(?:[A-Za-z_][\w:<>,\s\*&]*?)\s+([a-z][A-Za-z0-9_]*)\s*\([^;]*\)\s*\{\s*$', ln)
		if m: defs[m.group(1)] = i
		# FILE-SCOPE CONSTANTS TOO (unused-const-variable), unless already marked [[maybe_unused]].
		c = re.match(r'^(?:static\s+)?(?:inline\s+)?(?:constexpr|const)\s+[\w:<>,\s\*&]+?\s+(k[A-Z][A-Za-z0-9_]*)\s*(=|\{)', ln)
		if c: defs[c.group(1)] = i
	# A FUNCTION THE FILE'S OWN HEADER DECLARES HAS EXTERNAL LINKAGE, so it is never an unused-function on any
	# platform: skip it. Without this, event_log.cpp's xdgPath() (declared in event_log.h, called by tests on
	# every platform) was flagged on 2026-10-06.
	header = os.path.splitext(path)[0] + '.h'
	declared = io.open(header, encoding='utf-8', errors='ignore').read() if path.endswith('.cpp') and os.path.exists(header) else ''
	for name, i in defs.items():
		if declared and re.search(r'\b%s\s*\(' % re.escape(name), declared):
			continue
		pat = r'\b%s\b' if re.match(r'k[A-Z]', name) else r'\b%s\s*\('
		uses = [j for j, l in enumerate(lines) if j != i and re.search(pat % re.escape(name), l)]
		if not uses: continue
		defguard, useguards = ctx[i], {ctx[j] for j in uses}
		if all(('__linux__' in g or '_WIN32' in g or ('__APPLE__' in g and '|#else' in g)) for g in useguards) \
		   and not any(k in defguard for k in ('__linux__', '_WIN32', '__APPLE__')):
			konst = bool(re.match(r'k[A-Z]', name))
			print('%s:%d: %s%s is defined unguarded but used only under a platform guard — %s on the other'
			      ' platform (guard it, or mark it [[maybe_unused]])'
			      % (path, i + 1, name, '' if konst else '()', 'unused-const-variable' if konst else 'unused-function'))
			bad += 1
if bad:
	print()
	print('clang-preflight: %d platform-guard mismatch(es). Guard the definition with the same'
	      ' condition as its only caller.' % bad)
	sys.exit(1)
GUARDS

python3 - "$(cd "$(dirname "$0")/.." && pwd)" <<'INITONLY' || exit 1
import io, re, glob, sys, os
os.chdir(sys.argv[1])
bad = 0
for cpp in glob.glob('src/**/*.cpp', recursive=True):
	# THE PAIRED HEADER COUNTS AS THE SAME TRANSLATION UNIT FOR THIS QUESTION, and leaving it out was a
	# FALSE POSITIVE that halted the whole sweep.
	#
	# MEASURED 2026-08-31: `surface_layers.cpp` binds `title_` and `minimum_` in a member-init list and
	# reads neither — in the .cpp. Both are read by inline accessors in `surface_layers.h`
	# (`title()` and `minimumSize()`, the two `ExpandedView` overrides the class exists to provide).
	# Scanning only the .cpp cannot see them.
	#
	# WHY IT MATTERED MORE THAN A WRONG LINE OF OUTPUT: this check is STAGED and it HALTS. A false
	# positive here hides everything behind it, so the repo could not get a clean preflight at all
	# while it stood — and the advice it prints ("stop storing it") would have deleted a member two
	# overrides return.
	txt = io.open(cpp, encoding='utf-8', errors='ignore').read()
	for ext in ('.h', '.hpp'):
		hdr = cpp[:-4] + ext
		if os.path.exists(hdr):
			txt += '\n' + io.open(hdr, encoding='utf-8', errors='ignore').read()
	# COMMENTS ARE NOT USES. Blanked rather than removed so line numbers survive. Found the hard way:
	# a comment explaining why a member is read defeated this check when the read itself was deleted,
	# so the detector went quiet on exactly the mutation written to test it.
	txt = re.sub(r'/\*.*?\*/', lambda m: re.sub(r'\S', ' ', m.group(0)), txt, flags=re.S)
	txt = re.sub(r'//[^\n]*', lambda m: ' ' * len(m.group(0)), txt)
	for name in sorted(set(re.findall(r'\b([a-z][A-Za-z0-9]*_)\b', txt))):
		def line_of(m):
			return txt[txt.rfind('\n', 0, m.start()) + 1: txt.find('\n', m.start())]
		# THE MEMBER'S OWN DECLARATION IS NOT A USE. `Notifier* notifier_ = nullptr;` in the header counted as
		# one, so a member declared WITH A DEFAULT could never be flagged — AutomodExpanded::notifier_ reached
		# macOS CI that way on 2026-10-06. A declaration line is a type, the name, an optional default, and `;`.
		decl = re.compile(r'^\s*[A-Za-z_][\w:<>,\s\*&]*[\s\*&]%s\s*(=[^;]*|\{[^;]*\})?;\s*$' % re.escape(name))
		uses = [m for m in re.finditer(r'\b%s\b' % re.escape(name), txt) if not decl.match(line_of(m))]
		if not uses:
			continue
		if all(re.search(r'^\s*[:,].*%s\s*\(' % re.escape(name), line_of(m))
		       or re.search(r',\s*%s\s*\(' % re.escape(name), line_of(m)) for m in uses):
			print('%s: %s is bound in the member-init list and never read —'
			      ' unused-private-field on newer clang' % (cpp, name))
			bad += 1
if bad:
	print()
	print('clang-preflight: %d init-only member(s). Either USE it (a missing call is what this'
	      ' usually means) or stop storing it.' % bad)
	sys.exit(1)
INITONLY

# THE TU COUNT IS PART OF THE VERDICT, not a detail. "clean" without it is indistinguishable from
# "scanned nothing", which is how this script's own first run lied.
echo "clang-preflight: clean over $tus TU(s) — unused-const-variable, unused-private-field,"
echo "                 unused-lambda-capture, braced-scalar-init and the rest of -Wall -Wextra."
# THE VERSION IS READ, NOT WRITTEN: this line said "clang-14" for a day while the image ran clang-15.
echo "                 NOT a promise macOS is green: this is $("$CLANG" --version | head -1) on libstdc++, not Apple clang."
