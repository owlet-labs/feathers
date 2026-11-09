#!/usr/bin/env bash
# Copyright © 2026 Owlet Records
#
# This file is part of Feathers.
#
# WHO IS USING THE DISPLAY, AND FOR HOW LONG — a reservation, because etiquette is not a mechanism.
#
# THE NINTH WAY A RUN CAN LIE, and it is the only one caused by another session rather than by the
# machine or by ourselves. `:1` is one shared X display and several sessions photograph things on it.
# A windowed run that overlaps another does not merely fail — it CORRUPTS BOTH, silently and in both
# directions: extra windows appear in the other run's captures, and the other run's windows appear in
# yours. Neither exit code changes.
#
# MEASURED 2026-08-28: one session began a seven-cycle probe costing 24 minutes a cycle and broadcast
# a hold. The message arrived AFTER the integrator had already started a full `ctest` on the same
# display, which opens real windows in two suites. The cycle had to be discarded. Nobody did anything
# careless — the notice simply lost a race it was always going to lose eventually.
#
# So this is an ADVISORY LOCK with a name and a live pid. Advisory because nothing can stop a process
# opening a window, and a lock that pretends otherwise would be worse than one that is honest about
# being a convention with teeth: `venue-preflight.sh` refuses while somebody else holds it, so the
# check every windowed run already makes is where the collision is caught.
#
# STALE LOCKS CLEAR THEMSELVES. The holder's pid is written into the file and a lock whose holder is
# gone is ignored and removed — a session that crashes mid-run must not book the display forever.
#
# THE LOCK IS HELD BY A LIVE PROCESS, NOT BY A SCRIPT THAT HAS EXITED — and the first draft of this
# file got that wrong in a way its own test caught on the first run. `take` wrote its OWN pid, which
# died the instant the script returned, so the stale-lock pruner removed the lock and the next `take`
# succeeded. A reservation that always succeeds is worse than none: it reports "claimed" and books
# nothing.
#
# So the load-bearing form is `run`, which holds the lock for exactly as long as the command it wraps.
# The lifetime cannot drift from the work, there is no pid to bookkeep, and a crash frees it because
# the pid is gone. `take` survives for a job already running under a known pid, and REQUIRES that pid
# rather than inventing one.
#
# Usage:
#   scripts/venue-claim.sh run  <name> -- <command...>  # hold it for the command's lifetime
#   scripts/venue-claim.sh take <name> <pid> [display]  # book it on behalf of a live pid
#   scripts/venue-claim.sh free [display]               # release, if the holder is gone or is us
#   scripts/venue-claim.sh who  [display]               # print the holder, or nothing
set -u

ACTION="${1:-who}"
HOLDER_PID=""
if [ "$ACTION" = "take" ]; then
	NAME="${2:?usage: venue-claim.sh take <name> <pid> [display]}"
	HOLDER_PID="${3:?take needs the PID of the live job holding the venue}"
	DISPLAY_NAME="${4:-${DISPLAY:-:1}}"
elif [ "$ACTION" = "run" ]; then
	NAME="${2:?usage: venue-claim.sh run <name> -- <command...>}"
	shift 2
	[ "${1:-}" = "--" ] && shift
	DISPLAY_NAME="${DISPLAY:-:1}"
else
	NAME=""
	DISPLAY_NAME="${2:-${DISPLAY:-:1}}"
fi

LOCK="/tmp/feathers-venue-${DISPLAY_NAME//[^A-Za-z0-9]/_}.lock"

holder_pid() { [ -f "$LOCK" ] && cut -d' ' -f1 "$LOCK" 2>/dev/null || true; }
holder_name() { [ -f "$LOCK" ] && cut -d' ' -f2- "$LOCK" 2>/dev/null || true; }

# A LOCK WHOSE HOLDER IS GONE IS NOT A LOCK. Checked before every action, so a crashed session does
# not book the display until somebody notices.
prune() {
	local pid; pid="$(holder_pid)"
	if [ -n "$pid" ] && ! kill -0 "$pid" 2>/dev/null; then
		rm -f "$LOCK"
	fi
}

case "$ACTION" in
	take)
		prune
		if [ -f "$LOCK" ]; then
			echo "[venue] $DISPLAY_NAME is held by '$(holder_name)' (pid $(holder_pid))" >&2
			exit 1
		fi
		# O_EXCL, so two sessions racing here cannot both win.
		if ! (set -o noclobber; echo "$HOLDER_PID $NAME" > "$LOCK") 2>/dev/null; then
			echo "[venue] $DISPLAY_NAME was taken while we were taking it: '$(holder_name)'" >&2
			exit 1
		fi
		echo "[venue] $DISPLAY_NAME claimed by '$NAME' (pid $HOLDER_PID)"
		;;
	run)
		prune
		if [ -f "$LOCK" ]; then
			echo "[venue] $DISPLAY_NAME is held by '$(holder_name)' (pid $(holder_pid))" >&2
			exit 1
		fi
		if ! (set -o noclobber; echo "$$ $NAME" > "$LOCK") 2>/dev/null; then
			echo "[venue] $DISPLAY_NAME was taken while we were taking it: '$(holder_name)'" >&2
			exit 1
		fi
		# RELEASED FROM A TRAP, never as the last line — the whole failure mode is not finishing, and
		# this file's own playbook records a mutation harness stranded by exactly that.
		trap 'rm -f "$LOCK"' EXIT INT TERM
		echo "[venue] $DISPLAY_NAME claimed by '$NAME' (pid $$) for: $*"
		# AND TELL THE CHILD WHOSE CLAIM IT IS RUNNING UNDER, or the reservation deadlocks against
		# itself. `venue-preflight.sh` refuses while ANY holder exists — which is the point — so
		# without this a claimed command that preflights is refused by its own claim. That is not
		# hypothetical: `tools/drag-storm.sh` runs preflight as its second line, so
		# `venue-claim.sh run storm -- tools/drag-storm.sh` could never succeed, and preflight is
		# deliberately "the check every windowed run already makes".
		#
		# Found by hunt-engine on the first honest use of the mechanism, which is the only reason it
		# was found at all: the self-test I wrote checked that a STRANGER is refused and never that
		# the HOLDER is admitted. A lock that refuses everybody passes a test for exclusivity.
		export FEATHERS_VENUE_HOLDER="$$"
		"$@"
		exit $?
		;;
	free)
		# A DISPLAY, NOT A CLAIM NAME — and this branch used to accept either silently. `free he-take`
		# treated "he-take" as a display, computed a lock path nobody had ever written, removed
		# nothing, and printed "released". hunt-engine did exactly that while believing they were
		# releasing a real claim, and the only reason it cost nothing is that the argument landed in a
		# different namespace rather than on somebody's live lock. The usage line four lines up
		# documented `take <name>` beside `free [display]`, which is what invited it.
		case "$DISPLAY_NAME" in
			:*) ;;
			*) echo "[venue] free takes a DISPLAY (like :1), not a claim name — got '$DISPLAY_NAME'" >&2; exit 2 ;;
		esac
		prune
		if [ ! -f "$LOCK" ]; then
			# SAYING "released" ABOUT A LOCK NOBODY HELD is the reassuring answer, and it is how a
			# session concludes it has cleaned up when it has not.
			echo "[venue] $DISPLAY_NAME was not claimed — nothing to release" >&2
			exit 1
		fi
		if [ "$(holder_pid)" != "$$" ] && kill -0 "$(holder_pid)" 2>/dev/null; then
			echo "[venue] refusing to free $DISPLAY_NAME: held by '$(holder_name)' (pid $(holder_pid)), not us" >&2
			exit 1
		fi
		rm -f "$LOCK"
		echo "[venue] $DISPLAY_NAME released"
		;;
	who)
		prune
		[ -f "$LOCK" ] && echo "$(holder_name) (pid $(holder_pid))" || true
		;;
	*)
		echo "usage: venue-claim.sh run <name> -- <command...> | take <name> <pid> [display] | free [display] | who [display]" >&2
		exit 2
		;;
esac
