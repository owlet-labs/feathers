#!/bin/sh
# Copyright © 2026 Owlet Records
#
# This file is part of Feathers.
#
# COLLECT THE FACTORY PRESETS — RUN THIS BEFORE TAGGING A RELEASE (and the pre-push hook runs --check).
#
# Copies every preset tagged `factory` out of a preset root into `presets/factory/`, at the same
# relative path, so the next build embeds it (cmake/EmbedPresets.cmake) and a fresh machine gets it on
# first open (PresetStore::installFactoryFiles, copy-when-missing). Then review `git status`, and commit.
#
#   scripts/collect-factory-presets.sh [--check] [--prune] [--dest DIR] [ROOT]
#
#   ROOT     the preset root to read. Default: $FEATHERS_PRESET_ROOT, else
#            $HOME/Documents/Owlet/Feathers/Presets (the store's own default, preset_store.h).
#   --dest   where to copy to. Default: presets/factory beside this script. The tests use it.
#   --prune  also REMOVE any .fpreset in DEST that the root no longer tags `factory`. Off by default:
#            without it nothing is ever removed.
#   --check  COPY NOTHING. Exit 3 when DEST is out of date with the root (a tagged preset missing or
#            different, or a file in DEST the root no longer tags), 0 when it is in date.
#            scripts/hooks/pre-push runs this.
#
# IT NEVER WRITES THE ROOT. It only reads it (the dev container mounts it read-only anyway).
#
# WHAT COUNTS AS A FACTORY TAG: a tag that is exactly `factory`, any case, surrounding spaces ignored —
# the same normalisation the store gives tags. A NAMESPACED `<ns>:factory` IS NOT ACCEPTED: the store's
# only namespace is `category:` (preset_store.cpp, isCategoryTag), so `category:factory` means "in a
# folder called Factory", not "ship this". Tags are read from the file's metadata line
# (`FPRE`, a version byte, a 4-byte little-endian length, then `{"saved": N, "tags": [...]}`); a file with
# no `FPRE` header has no tags and is never selected. A tag containing `]` would confuse the reader; none
# of ours do.
#
# A TAGGED INIT IS THE BANK'S INIT, WHATEVER ITS CASE (a tagged Feathers/init.fpreset
# is THE factory Init). A tagged `<bank>/init.fpreset` in any case — `<bank>` being `Feathers` or
# `Devices/<TAG>` — is collected as `<bank>/Init.fpreset`, the name the plugin gives its computed Init
# (PresetBrowser::loadBankFromDisk), so the shipped file replaces it on a fresh machine and the synth's
# "Init" entry resolves to it. The plugin-written, untagged `Init.fpreset` beside it in the root is not
# selected.
#
# IT REFUSES, AND COPIES NOTHING, WHEN TWO PATHS DIFFER ONLY BY CASE — across the selected files (after
# the Init rename), the files already in DEST, and each bank's computed Init. On a case-insensitive disk
# (macOS, Windows) such a pair is ONE file, and which one wins is an accident. It names both. Two tagged
# files that both become one bank's Init are refused the same way.
#
# Exit: 0 done (or in date), 1 bad usage or no root, 2 refused (a case clash), 3 --check: out of date.

set -eu

here=$(cd "$(dirname "$0")/.." && pwd)
dest="$here/presets/factory"
prune=0
check=0
root=""
while [ $# -gt 0 ]; do
	case "$1" in
		--prune) prune=1 ;;
		--check) check=1 ;;
		--dest) [ $# -ge 2 ] || { echo "--dest needs a folder" >&2; exit 1; }; dest=$2; shift ;;
		-h|--help) sed -n '2,45p' "$0"; exit 0 ;;
		-*) echo "unknown option: $1" >&2; exit 1 ;;
		*) root=$1 ;;
	esac
	shift
done
if [ -z "$root" ]; then
	root=${FEATHERS_PRESET_ROOT:-"$HOME/Documents/Owlet/Feathers/Presets"}
fi
if [ ! -d "$root" ]; then
	echo "no preset root at $root" >&2
	exit 1
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
tab=$(printf '\t')

# The .fpreset files under a folder, relative, sorted, hidden files and folders skipped (the store and the
# embedding skip them too).
list() {
	(cd "$1" && find . -type f -name '*.fpreset' | sed 's|^\./||' | grep -v -e '^\.' -e '/\.' | LC_ALL=C sort) || true
}

# True when the file's metadata carries the tag `factory` (or the tag given as the second argument).
tagged() {
	want=${2:-factory}
	[ "$(dd if="$1" bs=4 count=1 2>/dev/null)" = "FPRE" ] || return 1
	# The length is bytes 5..8, little-endian.
	set -- "$1" $(od -An -tu1 -j5 -N4 "$1")
	[ $# -eq 5 ] || return 1
	n=$(( $2 + $3 * 256 + $4 * 65536 + $5 * 16777216 ))
	dd if="$1" bs=1 skip=9 count="$n" 2>/dev/null | tr '\n' ' ' |
		sed -n 's/.*"tags"[[:space:]]*:[[:space:]]*\[\([^]]*\)\].*/,\1,/p' |
		grep -qi ",[[:space:]]*\"[[:space:]]*$want[[:space:]]*\"[[:space:]]*,"
}

# Where a selected file lands in DEST: its own path, except a bank's init in any case, which is `Init`.
landing() {
	printf '%s\n' "$1" | sed -e 's|^\(Feathers\)/[Ii][Nn][Ii][Tt]\.fpreset$|\1/Init.fpreset|' \
	                         -e 's|^\(Devices/[^/]*\)/[Ii][Nn][Ii][Tt]\.fpreset$|\1/Init.fpreset|'
}

# $tmp/map: one line per selected file, "<landing path><TAB><root path>".
list "$root" > "$tmp/all"
: > "$tmp/map"
while IFS= read -r rel; do
	if tagged "$root/$rel"; then
		printf '%s\t%s\n' "$(landing "$rel")" "$rel" >> "$tmp/map"
	elif [ "$check" -eq 0 ]; then
		echo "skip     $rel (not tagged factory)"
	fi
done < "$tmp/all"
cut -f1 "$tmp/map" | LC_ALL=C sort > "$tmp/landed"

# Where a path comes from, for naming it in a refusal.
origin() {
	src=""
	while IFS="$tab" read -r to from; do
		if [ "$to" = "$1" ]; then
			src="$src${src:+ and }$from"
		fi
	done < "$tmp/map"
	if [ -n "$src" ]; then
		echo "tagged factory in $root: $src"
	elif [ -f "$dest/$1" ]; then
		echo "in $dest"
	else
		echo "the Init the plugin computes"
	fi
}

# THE CASE CHECK: every path that will exist in a fresh install, once each, then any two whose lower-case
# spellings agree. And first, two selected files landing on the SAME path (Init and init, both tagged).
clash=0
uniq -d "$tmp/landed" > "$tmp/same"
while IFS= read -r p; do
	clash=1
	echo "REFUSED: two tagged files would both be $p ($(origin "$p"))" >&2
done < "$tmp/same"
{
	cat "$tmp/landed"
	[ -d "$dest" ] && list "$dest"
	sed -n -e 's|^\(Feathers\)/.*|\1/Init.fpreset|p' -e 's|^\(Devices/[^/]*\)/.*|\1/Init.fpreset|p' "$tmp/landed"
} | LC_ALL=C sort -u > "$tmp/paths"
tr '[:upper:]' '[:lower:]' < "$tmp/paths" | LC_ALL=C sort | uniq -d > "$tmp/dupes"
while IFS= read -r low; do
	clash=1
	echo "REFUSED: these differ only by case, and are one file on macOS and Windows:" >&2
	while IFS= read -r p; do
		if [ "$(printf '%s' "$p" | tr '[:upper:]' '[:lower:]')" = "$low" ]; then
			echo "    $p ($(origin "$p"))" >&2
		fi
	done < "$tmp/paths"
done < "$tmp/dupes"
if [ "$clash" -ne 0 ]; then
	echo "Nothing copied. Rename one (see this script's header), then run again." >&2
	exit 2
fi

stale=0
while IFS="$tab" read -r to from; do
	if [ -f "$dest/$to" ] && cmp -s "$root/$from" "$dest/$to"; then
		[ "$check" -eq 1 ] || echo "same     $to"
		continue
	fi
	stale=1
	if [ -f "$dest/$to" ]; then
		echo "changed  $to (from $from)"
	else
		echo "added    $to (from $from)"
	fi
	if [ "$check" -eq 0 ]; then
		mkdir -p "$(dirname "$dest/$to")"
		cp "$root/$from" "$dest/$to"
	fi
done < "$tmp/map"

if [ -d "$dest" ]; then
	list "$dest" > "$tmp/have"
	while IFS= read -r rel; do
		grep -qxF "$rel" "$tmp/landed" && continue
		# A `test` PRESET IS NOT COLLECTED: tests/bench_patch writes it into presets/factory (--save, from the
		# survey's plans), never a preset root, so a root without it is not one this folder is out of date with.
		tagged "$dest/$rel" test && continue
		# A `test` PRESET IS NOT COLLECTED: it is written into presets/factory by tests/bench_patch (--save) from the
		# survey's plans, never from a preset root, so a root that lacks it is not one it is out of date with.
		tagged "$dest/$rel" test && continue
		stale=1
		if [ "$prune" -eq 1 ] && [ "$check" -eq 0 ]; then
			rm -f "$dest/$rel"
			echo "removed  $rel"
		else
			echo "stale    $rel (no longer tagged factory; --prune removes it)"
		fi
	done < "$tmp/have"
fi

if [ "$check" -eq 1 ] && [ "$stale" -ne 0 ]; then
	exit 3
fi
exit 0
