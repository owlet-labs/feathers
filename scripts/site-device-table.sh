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

# THE WEBSITE'S "EVERY DEVICE" TABLE, GENERATED FROM THE REGISTRY.
#
#   scripts/site-device-table.sh [build-dir]                 HTML fragment on stdout
#   scripts/site-device-table.sh [build-dir] --into <file>   replace what sits between the
#                                                            <!-- device-table:begin --> and
#                                                            <!-- device-table:end --> markers of <file>
#
# (default build dir: build; needs -DFEATHERS_BUILD_TOOLS=ON and `ninja widget-shot`.)
#
# Every name in the Extras column is read from the device's own declaration by `widget-shot --device-table` (see the
# comment there for the symbol behind each name). A device lists only what it declares, and
# nothing is typed by hand. The descriptions are docs/readme/device-descriptions.tsv, the same file the
# README's list uses, and this script FAILS when a device has no line there. Categories are the device
# browser's, in menu order, exactly as scripts/readme-device-list.sh groups them.
#
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
build=build
into=
while [ $# -gt 0 ]; do
	case $1 in
		--into) into=${2:?--into needs a file}; shift 2 ;;
		*) build=$1; shift ;;
	esac
done
tool="$build/widget-shot"
descriptions="$root/docs/readme/device-descriptions.tsv"

[ -x "$tool" ] || { echo "no $tool — configure with -DFEATHERS_BUILD_TOOLS=ON and build widget-shot" >&2; exit 1; }
dump=$("$tool" --device-table 2>/dev/null | grep -E '^device'$'\t' || true)
[ -n "$dump" ] || { echo "$tool --device-table printed nothing" >&2; exit 1; }

fragment=$(printf '%s\n' "$dump" | awk -F'\t' -v desc="$descriptions" '
function esc(s) { gsub(/&/, "\\&amp;", s); gsub(/</, "\\&lt;", s); gsub(/>/, "\\&gt;", s); gsub(/"/, "\\&quot;", s); return s }
BEGIN {
	while ((getline line < desc) > 0) {
		if (line ~ /^#/ || line == "") continue
		split(line, f, "\t")
		text[f[1]] = f[2]
	}
	# Extras vocabulary: dump field index and the name listed, in this order.
	ncol = 0
	col[++ncol] = 6;  head[ncol] = "Window"
	col[++ncol] = 7;  head[ncol] = "Poly"
	col[++ncol] = 8;  head[ncol] = "Keytrack"
	col[++ncol] = 9;  head[ncol] = "Linear phase"
	col[++ncol] = 10; head[ncol] = "Sidechain"
	col[++ncol] = 11; head[ncol] = "Mod outputs"
	col[++ncol] = 12; head[ncol] = "Note input"
	col[++ncol] = 13; head[ncol] = "Note output"
	col[++ncol] = 14; head[ncol] = "Latency"
}
{
	if (!($4 in text)) { print "no description for device " $4 " (" $5 ") in " desc > "/dev/stderr"; bad = 1 }
	n++
	rank[n] = $2; cat[n] = $3; tag[n] = $4; name[n] = $5
	for (c = 1; c <= ncol; c++) { v[n, c] = $(col[c]); used[c] += $(col[c]) }
}
END {
	if (bad) exit 1
	for (i = 1; i <= n; i++) order[i] = i
	for (i = 2; i <= n; i++) {
		k = order[i]; j = i - 1
		while (j >= 1 && (rank[order[j]] + 0 > rank[k] + 0 || (rank[order[j]] + 0 == rank[k] + 0 && tolower(name[order[j]]) > tolower(name[k])))) {
			order[j + 1] = order[j]; j--
		}
		order[j + 1] = k
	}
	print "<div class=\"devtable\" role=\"region\" aria-labelledby=\"devtable-title\" tabindex=\"0\">"
	print "<table>"
	print "<caption>" n " devices by category, with what each one can do.</caption>"
	print "<thead><tr><th scope=\"col\">Device</th><th scope=\"col\">Description</th><th scope=\"col\">Extras</th></tr></thead>"
	last = ""
	for (i = 1; i <= n; i++) {
		k = order[i]
		if (cat[k] != last) {
			if (last != "") print "</tbody>"
			print "<tbody>"
			print "<tr class=\"group\"><th scope=\"rowgroup\" colspan=\"3\">" esc(cat[k]) "</th></tr>"
			last = cat[k]
		}
		line = text[tag[k]]
		if (index(tolower(line), tolower(name[k]) ": ") == 1) { line = substr(line, length(name[k]) + 3); line = toupper(substr(line, 1, 1)) substr(line, 2) }
		row = "<tr><th scope=\"row\">" esc(name[k]) "</th><td>" esc(line) "</td>"
		extras = ""
		for (c = 1; c <= ncol; c++) if (v[k, c] == 1) extras = extras (extras == "" ? "" : ", ") head[c]
		row = row "<td>" esc(extras) "</td>"
		print row "</tr>"
	}
	print "</tbody>"
	print "</table>"
	print "</div>"
}')

if [ -z "$into" ]; then
	printf '%s\n' "$fragment"
	exit 0
fi

[ -f "$into" ] || { echo "no $into" >&2; exit 1; }
grep -qF '<!-- device-table:begin -->' "$into" && grep -qF '<!-- device-table:end -->' "$into" \
	|| { echo "$into has no device-table markers" >&2; exit 1; }
tmp=$(mktemp)
printf '%s\n' "$fragment" > "$tmp.frag"
awk -v frag="$tmp.frag" '
/<!-- device-table:begin -->/ { print; while ((getline l < frag) > 0) print l; skip = 1; next }
/<!-- device-table:end -->/ { skip = 0 }
!skip { print }' "$into" > "$tmp"
cat "$tmp" > "$into"
rm -f "$tmp" "$tmp.frag"
