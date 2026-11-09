#!/usr/bin/env bash
# Run `clang-preflight.sh` and REFUSE TO CALL A RUN CLEAN THAT DID NOT HAPPEN.
#
# This file is part of Feathers.
# Copyright (C) 2026 Owlet Records — GPL-3.0-or-later.
#
# Usage:  scripts/run-preflight.sh [build-dir]      (default: build)
#
# WHY A WRAPPER EXISTS AT ALL, when the script it runs already exits non-zero on findings.
#
# `clang-preflight.sh` is the only instrument here that can see the classes that break the macOS
# leg, and until now it was reachable by nothing: no test, no target, no watcher — a line on a
# checklist that a tired session skips, invisibly, because nothing goes red to mark the skip
# (backlog R2). Wiring it to something that FAILS is the point. But the exit code alone cannot carry
# the whole verdict, for a reason measured twice in one evening on this very script:
#
#   - a 2-minute timeout killed a 15-minute serial sweep mid-run: it printed no findings and looked
#     exactly like a clean result;
#   - `./script.sh; echo "EXIT=$?"` reported the ECHO's status, so a run that exited 1 was read as 0.
#
# Both near-misses produce a confident "preflight clean" that is false. So this wrapper requires the
# script's own SUMMARY LINE before it agrees that anything was checked. NO SUMMARY LINE MEANS NO RUN,
# and a run that did not happen is not a pass.
#
# AND THE VERDICT IS ASYMMETRIC — preserved here rather than flattened. The stages inside
# clang-preflight are sequential: a CLEAN run means every stage completed, while a run that REPORTS
# findings halts at the first one and says nothing about what is downstream. This wrapper therefore
# never summarises findings as a count of problems: it passes the script's own output through
# untouched and says which stage stopped it.
#
# WHY NOT A ctest REGEX. `PASS_REGULAR_EXPRESSION` would look like the way to require the summary
# line, and it is a trap: ctest's PASS regex REPLACES the exit-code check, so an entry that required
# the summary would stop failing on a non-zero exit — the opposite of what is wanted.
# `FAIL_REGULAR_EXPRESSION` is the additive one, which is why the wiring keeps the default exit-code
# check plus a FAIL regex on the warning text, and puts the "did it run" question HERE. Same shape as
# `run-cases.sh` cross-checking its own tally against the count the binary declares.
#
# THE TU CENSUS BELOW IS A SECOND IMPLEMENTATION of clang-preflight's file filter. Its job is to
# disagree: if the sweep reports "clean over N TU(s)" while the compile database names M of our TUs
# and N < M, the run was SHORT — clean about a fraction of the tree, which is the same lie in a
# different costume. If the two ever disagree for an innocent reason, that disagreement is the finding
# and belongs in both files' headers, not in a loosened check.
#
# AND HERE IS WHAT THIS CENSUS CAN AND CANNOT CATCH, WRITTEN DOWN BECAUSE IT ALREADY FAILED ONCE —
# backlog BX, found 2026-09-04 before the sweep had ever been run. This header used to say the
# duplication was "ON PURPOSE" and that "a census that shared the filter could not catch a filter that
# had drifted". Both true; both beside the point. The census did not merely share the filter's SHAPE,
# it shared its BUG — the same substring test (`'/src/' in f`, excluding `'/libs/'`) written out twice
# from the same understanding, so it could not disagree about anything the test itself got wrong. On
# this host `'/src/' in f` matched the HOME PATH (`$HOME/src/feathers`) and FetchContent's
# `build/_deps/` is not `libs/`, so BOTH sides counted 375 where 178 were ours, the `SHORT SWEEP`
# guard stayed silent because the two agreed, and the success line would have read "clean over 375
# TU(s), which is every TU of ours" about 197 files we do not own.
#
# SO, PRECISELY: a census that shares a filter catches drift in the filter's USE — a stage that stops
# early, a sweep that is killed, a scan set narrowed by accident. It CANNOT catch an error in the
# filter itself, because it is computed by that error. Duplication tests transcription; only a
# different ORACLE tests understanding. The honest form of an independent check answers the same
# question from a different direction, the way this wrapper's summary-line requirement asks doctest's
# own output rather than recounting the lines it just printed.
#
# SO THE ORACLE WAS CHANGED, AND THAT IS WHAT THE CENSUS NOW IS: it asks VERSION CONTROL, not the
# path string. A TU is ours iff this repository TRACKS it. The path shape survives only to choose
# which files git is asked about; git decides which of them are ours. That is a different DIRECTION —
# provenance rather than spelling — which is the only kind of second implementation that can catch an
# error in the first.
#
# THE BEST SINGLE ARGUMENT FOR IT, AND THE ONE THAT SHOWS IT IS A DIFFERENT KIND OF CHECK RATHER THAN
# A BETTER-SPELLED SAME ONE: vendored code now drops out BY MECHANISM. The parent repo lists
# `libs/visage` as a gitlink and none of its files, so a submodule TU is simply not tracked here and
# needs no `/libs/` test at all. The exclusion stopped being a path spelling and became a fact about
# ownership — and an ownership fact cannot be defeated by a checkout living somewhere the string test
# did not anticipate, which is exactly how BX happened.
#
# IT WAS SWAPPED ON A MEASUREMENT, WHICH IS THE ONLY SAFE WAY TO SWAP AN ORACLE: checked against
# `build`'s database on 2026-09-04, `git ls-files -- src tests` accounted for 178 of the 178
# genuinely-ours TUs with ZERO generated-but-untracked, and rejected all 197 third-party ones. It
# changed the KIND of check without changing the answer. Note the honest limit of that: it says the
# two agree TODAY, not that they cannot drift — which is the same thing this header says about
# agreement generally, and the reason for the names below.
#
# TWO THINGS THE NEW ORACLE NEEDS, both learned before it was written rather than after:
#
#   1. IT MUST BE ASKED ABOUT THE RIGHT REPOSITORY, and it REFUSES rather than answers when it cannot
#      be. `$here` is this script's checkout; `$BUILD` may belong to another — on this host every
#      worktree lives inside the repo and carries its own build dir, so the two coming apart is
#      ordinary. A census taken of the wrong tree is worse than no census, because it answers
#      confidently about somebody else's files. Disagreement is a stop, never a silent number.
#
#   2. IT NAMES WHAT IT CANNOT VOUCH FOR, because this failure is the quiet one. A census SMALLER than
#      the scan never trips SHORT SWEEP — that guard only fires on `scanned < census` — so a
#      generated-but-untracked TU under `src/` would weaken the check with no red at all. Every
#      candidate git does not know is therefore printed BY NAME and the run fails. A number that
#      agrees is not evidence; an empty list is. If such a file is legitimate, that disagreement is
#      the finding and belongs in this header and clang-preflight.sh's — not in a loosened check.
#
# COST: MEASURED 6m30s, once — the first clean run, 2026-09-04, 178 TUs on the anchored filter
# (clang `-fsyntax-only` over every TU of ours, tests included — five of the six warnings in the last
# survey were in `tests/`). That is why whatever runs this is invoked deliberately and never joins a
# default `ctest`.
#
# THIS LINE USED TO SAY "about 15 minutes", AND THE HALVING IS NOT A SPEED-UP — it is the estimate
# having been made against the wrong scan set. Before the BZ fix the filter swept 375 TUs on this
# host, of which 197 were bgfx, bimg and astc-encoder; the real set is 178. So the old number was
# roughly right about the work it was measuring and wrong about which work that was, which is why it
# is corrected here rather than quietly overwritten.
#
# AND THE NEW NUMBER IS ONE MEASUREMENT ON A SHARED HOST, so take 6-7 minutes as the SHAPE and expect
# it to move with load — the singles rule in the backlog's container notes applies to this as much as
# to anything else. Do not let it become a premise: a run that takes three times as long is evidence
# about the machine before it is evidence about the sweep.
#
# WHAT A GREEN HERE MEANS, SAID EXACTLY: clang-14 on libstdc++ found none of the classes it knows
# about, over every TU of ours in that build's compile database. It is NOT "macOS is green" — the
# runner is Apple clang on libc++, and the newer compiler emits classes clang-14 does not (measured:
# no `-Wmissing-braces` here where the runner emitted three).
set -uo pipefail

here=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${1:-build}
[[ $BUILD = /* ]] || BUILD="$here/$BUILD"

log=$(mktemp -t run-preflight.XXXXXXXX)
trap 'rm -f "$log"' EXIT

# SELF-TEST SEAM: `PREFLIGHT_CMD=<command>` runs that instead of the sweep, so every branch below can
# be shown able to fire against a fake output in a second rather than in a whole sweep. A gate whose
# refusals have never been seen to fire is a paragraph; the commit that added this exercised all five,
# and the two the git-oracle census added were fired the same way (one of them against a real
# `git rm --cached`, because a doctored file list would only have tested the restatement).
sweep=${PREFLIGHT_CMD:-$here/scripts/clang-preflight.sh}

# NOT PIPED. `cmd | tee` hands back tee's status, which is the second of the two near-misses above,
# reproduced inside the wrapper written to prevent it. Capture, then print. It is also the same
# finding as the sanitizer gate's visibility chain, one level down: there a report is only visible
# because ASan aborts, run-cases dumps and ctest prints failing output; here a verdict is only visible
# because nothing in between replaces the status with its own.
$sweep "$BUILD" > "$log" 2>&1
rc=$?
cat "$log"

fail() {
	echo >&2
	echo "run-preflight: FAIL — $*" >&2
	exit 1
}

if [[ $rc -ne 0 ]]; then
	# FINDINGS OR ENVIRONMENT, and the two are different news. Exit 2 is the script's own refusal
	# (no clang, no compile database, an empty one, or a database naming no TU of ours) — nothing was
	# checked. Exit 1 is a stage reporting real findings, and there may be more behind it.
	echo
	if [[ $rc -eq 2 ]]; then
		echo "run-preflight: the sweep REFUSED to run (exit 2) — nothing was checked. See its lines above." >&2
	else
		echo "run-preflight: findings (exit $rc). The stages are sequential, so this is the FIRST" >&2
		echo "               stage that stopped — not a complete list of what is wrong." >&2
	fi
	exit $rc
fi

# EXIT 0 IS NOT ENOUGH. The summary line is the only thing that says a sweep happened.
summary=$(grep -m1 -E '^clang-preflight: clean over [0-9]+ TU\(s\)' "$log" || true)
[[ -n $summary ]] || fail "the sweep exited 0 with no summary line — that is a run that did not
               happen (a killed sweep prints no findings and looks exactly like a clean one).
               Nothing here has been checked; do not read this as clean."

scanned=$(sed -E 's/^clang-preflight: clean over ([0-9]+) TU\(s\).*/\1/' <<<"$summary")

# THE CENSUS — see the header. It asks GIT, not the path string: a TU is ours iff this repository
# TRACKS it. That is the different direction the header argues for; the path shape is kept only to
# decide which files git is asked ABOUT.

# THE REFUSAL COMES FIRST, and it is a refusal rather than a caveat because a census taken of the
# wrong tree is worse than no census: it answers, confidently, about somebody else's files. `$here` is
# this script's own checkout; `$BUILD` may belong to another one — on this host every worktree lives
# inside the repo and carries its own build dir, so the two coming apart is ordinary, not exotic.
buildroot=$(git -C "$BUILD" rev-parse --show-toplevel 2>/dev/null || true)
[[ -n $buildroot ]] || fail "cannot tell which repository $BUILD belongs to (git could not name a
               working tree containing it), so the census has no repository to ask about. The sweep
               says clean over $scanned TU(s) and nothing here can confirm that number."
if [[ $(cd "$buildroot" && pwd -P) != $(cd "$here" && pwd -P) ]]; then
	fail "WRONG TREE: $BUILD belongs to $buildroot, but this script is $here's. The census would be
               asking one checkout about another's files and would answer a confident number about
               the wrong tree. Run the copy of this script that lives in $buildroot."
fi

gitlist=$(mktemp -t run-preflight-git.XXXXXXXX)
trap 'rm -f "$log" "$gitlist"' EXIT
git -C "$here" ls-files -- src tests > "$gitlist" || fail "could not list $here's tracked files, so
               the census has no oracle. The sweep says clean over $scanned TU(s) and nothing here
               can confirm that number."

censusout=$(ninja -C "$BUILD" -t compdb 2>/dev/null | ROOT="$here" GITLIST="$gitlist" python3 -c '
import json, os, sys
try:
	db = json.load(sys.stdin)
except Exception:
	sys.exit(3)
root = os.environ["ROOT"].rstrip("/") + "/"
tracked = {root + line.strip() for line in open(os.environ["GITLIST"]) if line.strip()}
# The path shape only chooses the CANDIDATES; git decides which of them are ours. Vendored code needs
# no string test: the parent repo lists libs/visage as a gitlink and none of its files, so a submodule
# TU is simply not tracked here.
candidates = {e["file"] for e in db
              if e["file"].startswith(root + "src/") or e["file"].startswith(root + "tests/")}
ours = candidates & tracked
print(len(ours))
# NAMES, not a count. A census smaller than the scan never trips SHORT SWEEP, so this disagreement
# would otherwise be silent — and a number that agrees is not evidence, an empty list is.
for f in sorted(candidates - tracked):
	print(f)
') || fail "could not take a census of the compile database in $BUILD — the sweep says clean over
               $scanned TU(s) and nothing here can confirm that number."

census=$(head -1 <<<"$censusout")
unknown=$(tail -n +2 <<<"$censusout")

if [[ -n $unknown ]]; then
	echo >&2
	echo "run-preflight: scanned but NOT TRACKED by $here:" >&2
	sed 's/^/                   /' >&2 <<<"$unknown"
	fail "the census cannot vouch for the file(s) above: they sit under src/ or tests/ and would be
               swept, but git does not know them. THE FAILURE DIRECTION IS THE POINT — a census
               smaller than the scan never trips SHORT SWEEP, so without this list the check would
               have been quietly weakened rather than gone red. If these are generated sources and
               the disagreement is innocent, THAT disagreement is the finding and belongs in this
               file's header and clang-preflight.sh's, not in a loosened check."
fi

if [[ -z $census || $census -eq 0 ]]; then
	fail "the census found no TU of ours in $BUILD's compile database, while the sweep reports clean
               over $scanned. One of the two is wrong and neither may be believed."
fi

if [[ $scanned -lt $census ]]; then
	fail "SHORT SWEEP: clean over $scanned TU(s), but $BUILD's compile database names $census of ours.
               A sweep that skipped $((census - scanned)) is clean about a fraction of the tree."
fi

echo
echo "run-preflight: OK — clean over $scanned TU(s), which is every TU of ours the compile database"
echo "               in $BUILD names ($census). clang-14 on libstdc++; NOT a promise about macOS."
