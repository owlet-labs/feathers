#!/usr/bin/env bash
# Copyright © 2026 Owlet Records
#
# This file is part of Feathers.
#
# Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
# General Public License as published by the Free Software Foundation, either version 3 of the
# License, or (at your option) any later version.
#
# Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even
# the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
# Public License for more details.
#
# You should have received a copy of the GNU General Public License along with Feathers. If not, see
# <https://www.gnu.org/licenses/>.

# THE README'S DEVICE LIST, GENERATED FROM THE REGISTRY.
#
#   scripts/readme-device-list.sh [build-dir]      (default: build; needs -DFEATHERS_BUILD_TOOLS=ON)
#
# Prints Markdown on stdout: the engines, then every device under its device-browser category (menu order,
# alphabetical within one), with its one-line description and whether it has an expanded window. Paste it
# between the `device-list` markers in README.md.
#
# The device names, categories and windows come from `widget-shot --device-list`, which asks the same
# `kDevices` table and `ui::hasExpandedView` the editor does. The registry has no description field, so the
# descriptions come from docs/readme/device-descriptions.tsv, and this script FAILS when a device or engine
# has no line there, so a new device cannot silently drop out of the list.

set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
build=${1:-build}
tool="$build/widget-shot"
descriptions="$root/docs/readme/device-descriptions.tsv"

[ -x "$tool" ] || { echo "no $tool — configure with -DFEATHERS_BUILD_TOOLS=ON and build widget-shot" >&2; exit 1; }

dump=$("$tool" --device-list 2>/dev/null | grep -E '^(device|engine)'$'\t')
[ -n "$dump" ] || { echo "$tool --device-list printed nothing" >&2; exit 1; }

printf '%s\n' "$dump" | awk -F'\t' -v desc="$descriptions" '
BEGIN {
	while ((getline line < desc) > 0) {
		if (line ~ /^#/ || line == "") continue
		split(line, f, "\t")
		text[f[1]] = f[2]
	}
}
$1 == "engine" {
	key = "engine:" $2
	if (!(key in text)) { print "no description for engine " $2 " in " desc > "/dev/stderr"; bad = 1 }
	engines[++ne] = $2
	next
}
$1 == "device" {
	if (!($4 in text)) { print "no description for device " $4 " (" $5 ") in " desc > "/dev/stderr"; bad = 1 }
	n++
	rank[n] = $2; cat[n] = $3; tag[n] = $4; name[n] = $5; win[n] = ($6 == "window")
}
END {
	if (bad) exit 1
	print "#### Engines"
	print ""
	for (i = 1; i <= ne; i++) print "- **" engines[i] "**: " text["engine:" engines[i]]
	# Sort by (category rank, name): a plain insertion sort, the list is short.
	for (i = 1; i <= n; i++) order[i] = i
	for (i = 2; i <= n; i++) {
		k = order[i]; j = i - 1
		while (j >= 1 && (rank[order[j]] + 0 > rank[k] + 0 || (rank[order[j]] + 0 == rank[k] + 0 && tolower(name[order[j]]) > tolower(name[k])))) {
			order[j + 1] = order[j]; j--
		}
		order[j + 1] = k
	}
	last = ""
	for (i = 1; i <= n; i++) {
		k = order[i]
		if (cat[k] != last) { print ""; print "#### " cat[k]; print ""; last = cat[k] }
		line = text[tag[k]]
		# "Name: what it is" in the file reads twice beside the bold name, so the repeat is dropped here.
		if (index(tolower(line), tolower(name[k]) ": ") == 1) { line = substr(line, length(name[k]) + 3); line = toupper(substr(line, 1, 1)) substr(line, 2) }
		print "- **" name[k] "**" (win[k] ? " (window)" : "") ": " line
	}
}'
