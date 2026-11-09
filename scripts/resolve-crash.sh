#!/usr/bin/env bash
# Turn a [FEATHERS] CRASH stack into function names.
#
# This file is part of Feathers.
# Copyright (C) 2026 Owlet Records — GPL-3.0-or-later.
#
# WHY THIS IS NEEDED. backtrace_symbols_fd reads the DYNAMIC symbol table, and the plugin exports
# none of its own symbols there — so a crash line reads `feathers.clap(+0x1eac70)` rather than a
# name. The STATIC table has them all, so the offset is resolvable after the fact; this does that
# lookup rather than leaving it as folklore.
#
# NO strtonum ANYWHERE: that is a gawk extension and this host's awk is mawk, where it is a runtime
# error rather than a parse error — so it fails per-frame, at the moment you need the frame, and
# prints "<no symbol>" instead of dying. Addresses are compared as ZERO-PADDED HEX STRINGS, which
# order lexicographically exactly as they order numerically, and need no conversion at all.
#
# Usage:
#   scripts/resolve-crash.sh [<binary>] [<logfile>]
# Defaults to the installed plugin and Bitwig's engine log, which is where a plugin's stderr goes.

set -uo pipefail
BIN=${1:-$HOME/.clap/feathers.clap}
LOG=${2:-$HOME/.BitwigStudio/log/engine.log}

[[ -f $BIN ]] || { echo "no binary at $BIN" >&2; exit 2; }
[[ -f $LOG ]] || { echo "no log at $LOG" >&2; exit 2; }

if ! grep -aq 'FEATHERS\] CRASH' "$LOG"; then
	echo "no [FEATHERS] CRASH stack in $LOG"
	echo "  (a crash before the handler shipped, or the handler never installed —"
	echo "   look for '[FEATHERS] crash handler installed' near the top of the log)"
	exit 1
fi

echo "binary: $BIN"
echo "log:    $LOG"

# DOES THIS BINARY MATCH THE CRASH? Refuse rather than resolve if it cannot be shown to.
#
# AN OFFSET IS ONLY MEANINGFUL AGAINST THE BINARY IT CAME FROM, and a host does not reload a .clap
# that is already open -- so installing between the crash and the resolve leaves this script reading
# a DIFFERENT build. It then prints real function names that are all the wrong functions. That has
# happened: a stack resolved to MsegCard::draw calling showChoiceMenu calling Canvas::fill calling
# ApplicationEditor::addToWindow, which is not a call chain that exists. Every frame landed inside
# some real symbol, so the >64KB distance guard below never fired -- a plausible answer is the
# dangerous kind of wrong.
#
# THE CRASH BANNER NOW CARRIES ITS BUILD STAMP and the stamp is compiled INTO the binary, so the two
# can be compared. ASCII only, because strings(1) ends a run at a non-ASCII byte.
CRASH_STAMP=$(grep -a "FEATHERS-BUILD" "$LOG" 2>/dev/null | tail -1 | sed "s/.*\(FEATHERS-BUILD [A-Za-z0-9 :]*\).*/\1/")
if [ -n "$CRASH_STAMP" ]; then
	# `grep -a` ON THE BINARY DIRECTLY, NOT `strings | grep -q`.
	#
	# THAT PIPELINE REPORTED A MATCH AS A MISMATCH. `grep -q` exits the instant it finds the string
	# and closes the pipe; `strings` then dies of SIGPIPE; and this script runs under `set -o
	# pipefail`, which takes the pipeline's status from the FAILING member. So the guard refused
	# hardest exactly when it should have passed -- caught only because the accept case was tested as
	# well as the refuse case.
	#
	# A GUARD THAT ONLY EVER SAYS NO IS NOT A GUARD, and testing only the failure path would have
	# shipped it. Same shape as the lesson in the playbook this stamp exists to serve: a pipeline's
	# exit status is not the status of the thing you care about.
	if LC_ALL=C grep -aqF "$CRASH_STAMP" "$BIN"; then
		echo "build:  $CRASH_STAMP (matches this binary)"
	else
		echo
		echo "  REFUSING TO RESOLVE -- this binary is not the one that crashed."
		echo "  crash was built: $CRASH_STAMP"
		echo "  and that stamp is not in $BIN."
		echo
		echo "  Every offset below would resolve to a REAL function and the WRONG one. Find the"
		echo "  binary that carries that stamp and pass it as the first argument, or rebuild the"
		echo "  commit it came from. ~/.clap/feathers-installed.log records what was installed when."
		exit 2
	fi
else
	echo
	echo "  WARNING: the crash carries no build stamp, so this binary CANNOT be shown to be the one"
	echo "  that crashed. Names below are trustworthy only if nothing was installed since."
	echo "  (Stamps arrived 2026-08-26; older logs predate them.)"
fi
echo

# The symbol table once, sorted by address: each frame is then a scan for the greatest symbol at or
# below the offset. nm prints 16-digit zero-padded addresses, which is what makes string order work.
SYMS=$(mktemp)
trap 'rm -f "$SYMS"' EXIT
nm -C --defined-only "$BIN" 2>/dev/null \
  | awk '$2 ~ /^[TtWw]$/ {print $1, substr($0, index($0,$3))}' | LC_ALL=C sort > "$SYMS"
if [[ ! -s $SYMS ]]; then
	echo "the binary has no static symbol table — it was stripped, and nothing here can name a frame." >&2
	exit 2
fi

# The LAST crash in the log: a session may hold several and the newest is the one being chased.
sed -n "$(awk '/FEATHERS\] CRASH/{n=NR} END{print n}' "$LOG"),\$p" "$LOG" \
  | sed -n '1,/end of stack/p' \
  | while IFS= read -r line; do
	if [[ $line == *feathers.clap*'+0x'* || $line == *feathers.so*'+0x'* ]]; then
		off=$(sed -n 's/.*+0x\([0-9a-fA-F]*\).*/\1/p' <<<"$line")
		padded=$(printf '%016s' "$off" | tr ' ' '0' | tr 'A-F' 'a-f')
		# THE NEAREST SYMBOL AT OR BELOW, **AND HOW FAR AWAY IT IS**.
		#
		# A NEAREST-SYMBOL LOOKUP ALWAYS ANSWERS, AND THAT IS THE PROBLEM. A crash whose PC landed in
		# unmapped memory or in a gap gets the last symbol before it, however distant -- and the
		# answer looks exactly like a real frame. FOUND 2026-08-25: a corrupted stack resolved to
		# `__mulsc3`, a compiler intrinsic for complex multiplication that nothing in this codebase
		# calls, and it was read as a lead before the distance was checked.
		#
		# So report the delta and refuse to name anything absurdly far. A frame inside a real
		# function is tens or hundreds of bytes past its start; tens of thousands means the symbol is
		# not the answer, it is just the last one before a hole.
		# FORCED INTO STRING CONTEXT, and this is not pedantry — it was a live bug.
		#
		# awk compares numerically when BOTH operands look like numbers, and a 16-digit hex address
		# containing `e` looks like scientific notation to it: `0000000000663e00` parses as 663,
		# which is <= almost anything. So the "greatest symbol at or below" quietly returned symbols
		# ABOVE the offset, and the tell was a NEGATIVE distance. Concatenating "" forces the
		# lexicographic comparison the zero-padding was chosen for in the first place.
		read -r symaddr name <<<"$(awk -v want="$padded" '(""$1) <= (""want) {a = $1; n = substr($0, index($0,$2))} END {print a, n}' "$SYMS")"
		if [[ -z ${name:-} ]]; then
			printf '  %-12s %s\n' "+0x$off" "<no symbol at or below this offset>"
		else
			delta=$(( 0x$off - 0x$symaddr ))
			if (( delta > 65536 )); then
				printf '  %-12s %s\n' "+0x$off" "<NO NEARBY SYMBOL — nearest is $name, $delta bytes back>"
				printf '  %-12s %s\n' "" "  a PC this far from any symbol is unmapped or in a gap:"
				printf '  %-12s %s\n' "" "  treat the stack as CORRUPT rather than as a lead."
			else
				printf '  %-12s %s  (+%d)\n' "+0x$off" "$name" "$delta"
			fi
		fi
	else
		printf '  %s\n' "$line"
	fi
done
