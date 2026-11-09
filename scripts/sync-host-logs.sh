#!/usr/bin/env bash
# Copyright © 2026 Owlet Records
#
# This file is part of Feathers.
#
# COPY THE HOST'S BITWIG AND FEATHERS LOGS WHERE A SESSION CAN READ THEM.
#
# Sessions run in the dev container, which mounts this repo but not ~/.var, ~/Library or the DAW's
# own directories, so when the plugin misbehaves in Bitwig nobody inside can see what Bitwig or
# Feathers wrote down. Run this ON THE HOST (or on the Mac) right after a sighting:
#
#     scripts/sync-host-logs.sh          # the last 2 days
#     scripts/sync-host-logs.sh 7        # the last 7 days
#
# It copies recent files into <repo>/host-logs/<hostname>/, keeping each one's path under $HOME, and
# prints what it copied. host-logs/ is gitignored. On a machine without the repo (the Mac), pass a
# destination as the second argument and send the folder over:
#
#     ./sync-host-logs.sh 2 ~/Desktop/feathers-logs
#
# What it looks for (whatever exists; a missing directory is skipped):
#   - Feathers' event log: Bitwig flatpak (its XDG_STATE_HOME), plain Linux, macOS.
#   - Bitwig's own logs: flatpak, native Linux, macOS.
#   - Crash reports: macOS DiagnosticReports entries naming Bitwig.
# Read-only on every source. It never deletes anything, here or there.

set -u

days="${1:-2}"
here="$(cd "$(dirname "$0")/.." && pwd)"
dest="${2:-$here/host-logs/$(hostname -s 2>/dev/null || hostname)}"

case "$days" in
	''|*[!0-9]*) echo "usage: $0 [days] [destination]" >&2; exit 2 ;;
esac

sources=(
	"$HOME/.var/app/com.bitwig.BitwigStudio/.local/state/feathers"
	"$HOME/.local/state/feathers"
	"$HOME/Library/Logs/Feathers"
	"$HOME/.var/app/com.bitwig.BitwigStudio"
	"$HOME/.BitwigStudio"
	"$HOME/Library/Logs/Bitwig"
	"$HOME/Library/Application Support/Bitwig"
	"$HOME/Library/Logs/DiagnosticReports"
)

mkdir -p "$dest" || exit 1
copied=0
for src in "${sources[@]}"; do
	[ -d "$src" ] || continue
	while IFS= read -r -d '' f; do
		case "$f" in
			*/DiagnosticReports/*) case "$f" in *[Bb]itwig*) ;; *) continue ;; esac ;;
		esac
		rel="${f#"$HOME"/}"
		mkdir -p "$dest/$(dirname "$rel")" && cp -p "$f" "$dest/$rel" && {
			echo "$rel"
			copied=$((copied + 1))
		}
	done < <(find "$src" -type f \( -name '*.log' -o -name '*.log.*' -o -name '*.txt' -o -name '*.ips' \
		-o -name '*.crash' -o -name 'hs_err*' -o -name 'events.log*' \) -mtime "-$days" -print0 2>/dev/null)
done

echo "copied $copied file(s) from the last $days day(s) into $dest"
