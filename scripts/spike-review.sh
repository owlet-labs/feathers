#!/bin/sh
# Copyright (C) 2026 Owlet Records
# This file is part of Feathers.
# GPL-3.0-or-later; see LICENSE.
#
# THE HOURLY CPU-SPIKE REVIEW: a hook so that new CPU-spike records are reviewed periodically.
# Run by the UserPromptSubmit hook in .claude/settings.local.json. At most once an hour per session, it
# summarises the spike records the plugin wrote since that session's last summary
# and prints them, which the hook hands to Claude as context. Silent when there is nothing new, the hour
# has not passed, or the log dir is not mounted. It only READS the logs; its state lives in /tmp/claude-$UID.
#
# A HOOK RUNS THIS UNPROMPTED, so it is FROZEN like run-cases.sh and install-clap.sh (CLAUDE.md, "Allow-listed
# scripts are a trust boundary"): no agent edits it without telling the maintainer first.

dir="${FEATHERS_SPIKE_DIR:-$HOME/.var/app/com.bitwig.BitwigStudio/data/feathers/likes-log}"
[ -d "$dir" ] || exit 0
command -v jq >/dev/null 2>&1 || exit 0

session=$(jq -r '.session_id // "default"' 2>/dev/null | tr -cd 'A-Za-z0-9_-')
[ -n "$session" ] || session=default
state="/tmp/claude-$(id -u)/spike-review/${session}"
mkdir -p "$state" 2>/dev/null || exit 0

now=$(date +%s)
last=$(cat "$state/last-run" 2>/dev/null || echo 0)
[ $((now - last)) -ge 3600 ] || exit 0
echo "$now" >"$state/last-run"

seen=$(cat "$state/seen-t" 2>/dev/null || echo 0)
summary=$(cat "$dir"/*.spikes.jsonl 2>/dev/null | jq -s -r --argjson seen "$seen" '
	[ .[] | select(.event == "spike" and .t > $seen) ] as $s
	| if ($s | length) == 0 then empty else
		($s | max_by(.share)) as $w
		| "count=\($s | length) newest_t=\($s | map(.t) | max) worst_share=\($w.share) worst_us=\($w.us) worst_frames=\($w.frames) worst_plugin=\($w.plugin)",
		  "top_devices=" + ([ $s[] | .devices[]? ] | group_by(.tag)
			| map({tag: .[0].tag, n: length, us: (map(.us) | add)}) | sort_by(-.us) | .[:5]
			| map("\(.tag)(n=\(.n),total_us=\(.us | floor))") | join(" "))
	end')
[ -n "$summary" ] || exit 0

newest=$(printf '%s\n' "$summary" | sed -n 's/.*newest_t=\([0-9.]*\).*/\1/p')
[ -n "$newest" ] && echo "$newest" >"$state/seen-t"

printf 'SPIKE REVIEW (hourly hook, scripts/spike-review.sh): new CPU spike records in %s since the last review.\n%s\nReview them (src/plugin/spike_log.h describes a record) and tell the user briefly if anything is notable; say nothing if it is routine.\n' "$dir" "$summary"
