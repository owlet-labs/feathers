#!/usr/bin/env bash
# Copyright © 2026 Owlet Records
#
# This file is part of Feathers.
#
# Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
# General Public License as published by the Free Software Foundation, either version 3 of the
# License, or (at your option) any later version.
#
# This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without
# even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
# General Public License for more details.
#
# You should have received a copy of the GNU General Public License along with this program. If not,
# see <https://www.gnu.org/licenses/>.
#
# --- Additional terms under GNU GPL version 3 section 7 ---
# This file requires preservation of the above copyright notice and author attribution in all copies
# or substantial portions of this file.
#
# Install the built plugin for hands-on testing, as a COPY, and say exactly what was installed.
#
# WHY A COPY. ~/.clap/feathers.clap used to be a symlink into build/, which meant every rebuild
# replaced the binary under a running DAW. On 2026-08-12 that was not theoretical: Bitwig had the
# plugin loaded while the build directory was relinked repeatedly, /proc showed the loaded module as
# "(deleted)", and a live bug report could not be tied to any commit — the binary being tested no
# longer existed. Unlinking and replacing keeps the running mapping intact, so nothing crashes, which
# is precisely what makes it so easy to miss.
#
# A copy is inert. The installed editor stays whatever was installed until somebody installs another one on
# purpose, and the sha below says which one that was.
#
#   ./scripts/install-clap.sh                      # installs build/feathers.clap
#   ./scripts/install-clap.sh build-x/…            # or a specific build
#   ./scripts/install-clap.sh … --rollback "why"   # deliberately install something OLDER
#
# THE SLOT ONLY EVER MOVES FORWARD IN WHAT IT CAN READ. Two sessions install into this one file, and
# a patch format grows: on 2026-08-19 the residue block's tag went FSR1 -> FSR2 because the SIZE of
# its count-set changed, and an older reader handed a longer header does not merely misread its own
# block — it walks the rest of the stream at the wrong offset and corrupts every block after it. So a
# build that reads less than the one it replaces is not a smaller feature set, it is a build that
# REFUSES patches already saved. The rule both sessions agreed: the candidate must be a
# DESCENDANT of whatever is in the slot. That was a rule held in two agents' memories across a queue,
# which is a rule with a half-life — so it lives here instead, where it cannot be forgotten at the
# one moment it matters.
#
# --rollback exists because a deliberate rollback is legitimate (bisecting, say). It takes a REASON
# and it is a flag somebody types: deliberately not an environment variable, which can sit set in a
# shell and silently disable the gate for the next twenty installs.

set -euo pipefail

ROLLBACK_REASON=""
POSITIONAL=()
while (( $# > 0 )); do
	case "$1" in
	--rollback)
		shift
		if (( $# == 0 )) || [[ "$1" == --* ]]; then
			echo "install-clap: --rollback needs a reason, e.g. --rollback 'bisecting the wedge glitch'" >&2
			exit 2
		fi
		ROLLBACK_REASON="$1"
		;;
	--*)
		echo "install-clap: unknown option $1" >&2
		exit 2
		;;
	*)
		POSITIONAL+=("$1")
		;;
	esac
	shift
done

SOURCE="build/feathers.clap"
if (( ${#POSITIONAL[@]} > 0 )); then
	SOURCE="${POSITIONAL[0]}"
fi
DEST_DIR="${CLAP_DIR:-$HOME/.clap}"

# THE SLOT IS NAMED AFTER THE BUNDLE, NOT HARDCODED — and this was a real footgun, not a tidy-up.
#
# `DEST` was `$DEST_DIR/feathers.clap` whatever `$SOURCE` was, so the moment a SECOND plugin existed
# in this repo, `install-clap.sh build/feathers-rack.clap` would have installed the RACK over the
# SYNTH, under the synth's filename. The rollback log and the descendant gate would then have been
# comparing the ancestry of two unrelated plugins, which is not a check that can mean anything.
#
# Found the day `feathers-rack.clap` first existed and before anyone ran it. Each bundle now gets its
# own slot and its own log, so the gate compares a plugin against ITSELF — which is what it was
# always for.
BUNDLE="$(basename "$SOURCE")"
DEST="$DEST_DIR/$BUNDLE"
LOG="$DEST_DIR/${BUNDLE%.clap}-installed.log"

if [[ ! -e "$SOURCE" ]]; then
	echo "install-clap: no plugin at $SOURCE — build it first (ninja -C build)" >&2
	exit 1
fi

# WHICH TREE THIS BINARY CAME FROM, which is not necessarily the one you are standing in. Installing
# from a worktree while sitting in the main checkout reported the MAIN tree's state — so the stamp
# described a tree that had nothing to do with the build.
#
# Computed BEFORE the copy, because the gate below has to be able to refuse: a check that runs after
# the file is in place is a report, not a gate.
REPO="$(git -C "$(dirname "$SOURCE")" rev-parse --show-toplevel 2>/dev/null || true)"
if [[ -z "$REPO" ]]; then
	REPO="$(git rev-parse --show-toplevel 2>/dev/null || true)"
fi

SHA="$(git -C "${REPO:-.}" rev-parse --short HEAD 2>/dev/null || echo 'not-a-git-checkout')"

# ── AND THE TREE MUST BE THE COMMIT ── because this build directory compiles from the WORKING TREE.
#
# The property that makes build-install-on trustworthy — it lives in the main checkout, so it never
# sees another worktree's half-applied source — is the same property that makes it see EVERYTHING the
# main checkout has, committed or not. On 2026-08-19 a second session ran its install verification
# here while an agent of the first had ten uncommitted files in the tree: the suite passed, the run
# was a true statement about a tree nobody had reviewed, and the stamp would have named a commit that
# binary did not contain. It was caught because an unrelated merge aborted, which is luck.
#
# So a dirty tree refuses. The stamp below still NOTES uncommitted files, but a note printed after the
# copy is a report and this is a gate, and the difference is the whole lesson.
if [[ -n "$REPO" ]] && ! git -C "$REPO" diff --quiet HEAD 2>/dev/null; then
	if [[ -z "$ROLLBACK_REASON" ]]; then
		echo "install-clap: REFUSED — the tree has uncommitted changes, so this binary is not $SHA" >&2
		echo "              What differs:" >&2
		git -C "$REPO" diff --name-only HEAD 2>/dev/null | sed 's/^/                    - /' >&2
		echo "              A build directory that compiles from a working tree verifies the TREE," >&2
		echo "              not the commit. Commit the work, or install deliberately with:" >&2
		echo "              --rollback \"the reason\"" >&2
		exit 4
	fi
	echo "install-clap: DIRTY TREE — installing something that is not any commit"
	echo "              proceeding because: $ROLLBACK_REASON"
fi

# ── THE GATE ── the candidate must be a descendant of what is in the slot. See the header for why.
#
# The commit is read back out of the log's last line rather than tracked anywhere else, because the
# log is the only thing that knows what is ACTUALLY installed — a session's memory of it can be
# stale, and the file itself carries no commit. A line reads
#   2026-08-18 22:27:31  commit 8e030b7  binary 02dbebc49473  from build-install-on/feathers.clap
# and may carry "+uncommitted-changes" after the sha, so take the hex token and nothing else.
PREV_COMMIT=""
if [[ -f "$LOG" ]]; then
	PREV_COMMIT="$(tail -n 1 "$LOG" | sed -n 's/.*commit \([0-9a-f][0-9a-f]*\).*/\1/p')"
fi

if [[ -n "$PREV_COMMIT" && -n "$REPO" ]]; then
	GATE_REFUSAL=""
	if ! git -C "$REPO" cat-file -e "${PREV_COMMIT}^{commit}" 2>/dev/null; then
		# Unverifiable is not the same as fine. A predecessor this repository has never heard of is
		# exactly the case where a backward step would hide, so it refuses too and says which.
		GATE_REFUSAL="the installed commit $PREV_COMMIT is not in this repository, so the step cannot be checked"
	elif ! git -C "$REPO" merge-base --is-ancestor "$PREV_COMMIT" HEAD 2>/dev/null; then
		GATE_REFUSAL="$SHA is NOT a descendant of the installed $PREV_COMMIT — this install would move the slot BACKWARD"
	fi

	if [[ -n "$GATE_REFUSAL" ]]; then
		if [[ -z "$ROLLBACK_REASON" ]]; then
			echo "install-clap: REFUSED — $GATE_REFUSAL" >&2
			echo "              installed: $PREV_COMMIT" >&2
			echo "              candidate: $SHA" >&2
			echo "              A build that reads less than the one it replaces will REFUSE patches" >&2
			echo "              already saved with the current one. Merge the missing work first." >&2
			echo "              If you mean it: --rollback \"the reason\"" >&2
			exit 3
		fi
		echo "install-clap: ROLLBACK — $GATE_REFUSAL"
		echo "              proceeding because: $ROLLBACK_REASON"
	fi
fi

mkdir -p "$DEST_DIR"

if [[ -L "$DEST" ]]; then
	echo "install-clap: replacing the old SYMLINK at $DEST with a copy"
	rm -f "$DEST"
fi

# Written beside and moved into place, so a DAW that happens to be loading it sees one file or the
# other and never a half-written one.
TMP="$DEST.installing.$$"
cp "$SOURCE" "$TMP"
chmod +x "$TMP"
mv -f "$TMP" "$DEST"

# AND WHAT IS DIRTY, BY NAME. A single flag raised for four different reasons is a flag that stops
# meaning anything: it fired for an edited plan document on one install and for a STALE SUBMODULE on
# the next, and the second one shipped a binary built without a fix that was already committed.
#
# `git submodule status` marks a submodule whose checkout differs from the recorded gitlink with a
# leading '+' and an uninitialised one with '-'. That is exactly the case a worktree creates, because
# `git checkout` in a worktree does not update submodules — and `git diff` never mentions it, so
# nothing else here could have caught it.
DIRTY=""
DIRTY_WHAT=()
if [[ -n "$REPO" ]]; then
	if ! git -C "$REPO" diff --quiet HEAD 2>/dev/null; then
		DIRTY_WHAT+=("tracked files: $(git -C "$REPO" diff --name-only HEAD 2>/dev/null | tr '\n' ' ')")
	fi
	# `|| true` because a CLEAN tree is the case where grep matches nothing and exits 1 — which under
	# `set -euo pipefail` killed the whole script silently. Found by running it on a clean tree, which
	# is the one path the check had never been exercised on: every earlier test happened to be dirty.
	SUBS="$(git -C "$REPO" submodule status 2>/dev/null | grep -v '^ ' | awk '{print $2}' | tr '\n' ' ' || true)"
	if [[ -n "${SUBS// }" ]]; then
		DIRTY_WHAT+=("submodules NOT at the recorded commit: $SUBS")
	fi
fi
if (( ${#DIRTY_WHAT[@]} > 0 )); then
	DIRTY=" +uncommitted-changes"
fi
STAMP="$(date '+%Y-%m-%d %H:%M:%S')"
BINSHA="$(sha256sum "$DEST" | cut -c1-12)"

# A deliberate rollback is recorded in the log with its reason, because the NEXT install's gate reads
# this line: without the note, the step that went backward would look like an ordinary predecessor and
# the following install would be checked against it in silence.
ROLLBACK_NOTE=""
if [[ -n "$ROLLBACK_REASON" ]]; then
	ROLLBACK_NOTE="  ROLLBACK($ROLLBACK_REASON)"
fi

echo "$STAMP  commit $SHA$DIRTY  binary $BINSHA  from $SOURCE$ROLLBACK_NOTE" >> "$LOG"

# A COPY IS NOT A RELOAD. Replacing the file unlinks the old inode, and a DAW that already mapped it
# keeps running the old one — /proc shows it as "(deleted)" and every later install misses that
# session entirely. This has now confused a live debugging round twice, so the script says it out
# loud and names who is holding an old build.
HOLDERS=""
for pid in $(pgrep -f "PluginHost|bitwig|reaper|ardour|carla" 2>/dev/null || true); do
	if grep -qs "feathers.clap" "/proc/$pid/maps" 2>/dev/null; then
		HOLDERS="$HOLDERS $pid($(cat /proc/$pid/comm 2>/dev/null))"
	fi
done

echo "install-clap: installed $DEST"
echo "              commit  $SHA$DIRTY"
echo "              binary  sha256:$BINSHA"
echo "              history $LOG"
echo "              tree    ${REPO:-<unknown>}"
if [[ -n "$DIRTY" ]]; then
	echo "              NOTE: this binary is not any commit. What differs:"
	for what in "${DIRTY_WHAT[@]}"; do
		echo "                    - $what"
	done
fi
if [[ -n "$HOLDERS" ]]; then
	echo
	echo "              ALREADY LOADED ELSEWHERE:$HOLDERS"
	echo "              Those processes keep the binary they mapped. This install reaches them only"
	echo "              when the plugin is loaded again — reload the device, or restart the host."
fi
