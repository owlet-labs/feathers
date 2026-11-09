#!/bin/sh
# Copyright © 2026 Owlet Records
#
# This file is part of Feathers.
#
# scripts/collect-factory-presets.sh against a fixture root built here: which files it selects, that the
# copies are the bytes, that it refuses case clashes (within the tagged set, against DEST, and against the
# computed Init) copying nothing, that it never writes the root, and --prune. Never touches ~/Documents:
# HOME is pointed at the scratch folder too, so a default that leaked would land there.
#
#   tests/test_collect_factory_presets.sh <path to collect-factory-presets.sh>

set -u
script=$1
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT HUP INT TERM

# A CASE-INSENSITIVE FILESYSTEM CANNOT HOLD THIS FIXTURE (macOS CI, 2026-09-27): it builds pairs like
# Lead/lead side by side to test the clash refusals, and there the second file overwrites the first, so
# every later assertion fails in a cascade. The script is a release tool run on a Linux host, and the
# clash it guards against cannot exist on such a filesystem at all, so report SKIP (77, ctest's
# SKIP_RETURN_CODE) rather than a failure.
touch "$work/CaseProbe" "$work/caseprobe"
if [ "$(ls "$work" | grep -ci '^caseprobe$')" -lt 2 ]; then
	echo "SKIP: $work is on a case-insensitive filesystem; the Init/init clash fixture cannot exist here"
	exit 77
fi
rm -f "$work/CaseProbe" "$work/caseprobe"

export HOME="$work/home"
unset FEATHERS_PRESET_ROOT
failures=0

fail() {
	echo "FAIL: $*"
	failures=$((failures + 1))
}

# A preset file as the store writes one: FPRE, version 1, the metadata length (little-endian, < 256
# here), the metadata, then a payload.
preset() {
	file=$1 json=$2 payload=$3
	mkdir -p "$(dirname "$file")"
	n=$(printf '%s' "$json" | wc -c | tr -d ' ')
	printf "FPRE\\001\\$(printf '%03o' "$n")\\000\\000\\000" > "$file"
	printf '%s%s' "$json" "$payload" >> "$file"
}

root="$work/root"
dest="$work/dest"
preset "$root/Feathers/Pads/Glass.fpreset" '{"saved": 5, "tags": ["pad", "factory"]}' 'glass-bytes'
preset "$root/Devices/SKLITTER/Chop.fpreset" '{"saved": 6, "tags": [" Factory "]}' 'chop-bytes'
preset "$root/Feathers/Mine.fpreset" '{"saved": 7, "tags": ["bass"]}' 'mine-bytes'
preset "$root/Feathers/Filed.fpreset" '{"saved": 8, "tags": ["category:factory"]}' 'filed-bytes'
preset "$root/Feathers/Lead.fpreset" '{"saved": 9, "tags": ["factory"]}' 'lead-bytes'
preset "$root/Feathers/lead.fpreset" '{"saved": 10, "tags": ["factory"]}' 'lead2-bytes'
before=$(cd "$root" && find . -type f | LC_ALL=C sort | xargs cksum)

# 1. A CASE CLASH INSIDE THE TAGGED SET: refused, both named, nothing copied.
out=$("$script" --dest "$dest" "$root" 2>&1); rc=$?
[ "$rc" -eq 2 ] || fail "a tagged Lead/lead pair exits $rc, not 2"
echo "$out" | grep -q "Feathers/Lead.fpreset" || fail "the clash does not name Feathers/Lead.fpreset"
echo "$out" | grep -q "Feathers/lead.fpreset" || fail "the clash does not name Feathers/lead.fpreset"
[ ! -e "$dest" ] || fail "a refused run wrote $dest"
rm "$root/Feathers/lead.fpreset"

# 2. THE ROOT BY ENVIRONMENT: the two tagged files copied byte for byte; the untagged and the
# category:factory one not.
out=$(FEATHERS_PRESET_ROOT="$root" "$script" --dest "$dest" 2>&1); rc=$?
[ "$rc" -eq 0 ] || fail "a clean root exits $rc: $out"
for rel in Feathers/Pads/Glass.fpreset Devices/SKLITTER/Chop.fpreset Feathers/Lead.fpreset; do
	cmp -s "$root/$rel" "$dest/$rel" || fail "$rel was not copied byte for byte"
	echo "$out" | grep -q "^added    $rel (from $rel)$" || fail "$rel is not reported added"
done
[ ! -e "$dest/Feathers/Mine.fpreset" ] || fail "an untagged preset was copied"
[ ! -e "$dest/Feathers/Filed.fpreset" ] || fail "category:factory was taken for the factory tag"
echo "$out" | grep -q "^skip     Feathers/Mine.fpreset" || fail "the untagged preset is not reported skipped"
[ "$(cd "$dest" && find . -type f | wc -l | tr -d ' ')" -eq 3 ] || fail "dest holds other than the three"
[ ! -e "$HOME" ] || fail "the run wrote under HOME"

# 3. AGAIN: nothing changed. Then an edit in the root is reported changed and copied.
out=$("$script" --dest "$dest" "$root" 2>&1)
echo "$out" | grep -q "^same     Feathers/Lead.fpreset$" || fail "an unchanged preset is not reported same"
# --check IN DATE: exit 0, nothing printed.
out=$("$script" --check --dest "$dest" "$root" 2>&1); rc=$?
[ "$rc" -eq 0 ] || fail "--check on an in-date dest exits $rc, not 0: $out"
[ -z "$out" ] || fail "--check on an in-date dest printed: $out"
preset "$root/Feathers/Lead.fpreset" '{"saved": 11, "tags": ["factory"]}' 'lead-edited'
before=$(cd "$root" && find . -type f | LC_ALL=C sort | xargs cksum)
# --check OUT OF DATE: exit 3, names the file, copies nothing.
dest_before=$(cd "$dest" && find . -type f | LC_ALL=C sort | xargs cksum)
out=$("$script" --check --dest "$dest" "$root" 2>&1); rc=$?
[ "$rc" -eq 3 ] || fail "--check on an out-of-date dest exits $rc, not 3"
echo "$out" | grep -q "^changed  Feathers/Lead.fpreset" || fail "--check does not name the changed file"
[ "$dest_before" = "$(cd "$dest" && find . -type f | LC_ALL=C sort | xargs cksum)" ] || fail "--check wrote dest"
out=$("$script" --dest "$dest" "$root" 2>&1)
echo "$out" | grep -q "^changed  Feathers/Lead.fpreset (from Feathers/Lead.fpreset)$" || fail "an edited preset is not reported changed"
cmp -s "$root/Feathers/Lead.fpreset" "$dest/Feathers/Lead.fpreset" || fail "the edit was not copied"

# 4. A CLASH AGAINST A FILE ALREADY IN DEST: refused, both named, dest untouched.
preset "$root/Devices/SKLITTER/chop.fpreset" '{"saved": 12, "tags": ["factory"]}' 'x'
rm "$root/Devices/SKLITTER/Chop.fpreset"
out=$("$script" --dest "$dest" "$root" 2>&1); rc=$?
[ "$rc" -eq 2 ] || fail "a clash with dest exits $rc, not 2"
echo "$out" | grep -q "Devices/SKLITTER/Chop.fpreset (in " || fail "the dest file is not named"
echo "$out" | grep -q "Devices/SKLITTER/chop.fpreset (tagged" || fail "the tagged file is not named"
[ ! -e "$dest/Devices/SKLITTER/chop.fpreset" ] || fail "a refused run copied"
rm "$root/Devices/SKLITTER/chop.fpreset"

# 5. A TAGGED init IS THE BANK'S Init (a real preset root has exactly this: an untagged plugin-written Init
# beside a tagged init). It lands as Feathers/Init.fpreset; the untagged one is not selected.
preset "$root/Feathers/Init.fpreset" '{"saved": 0, "tags": []}' 'computed-init'
preset "$root/Feathers/init.fpreset" '{"saved": 13, "tags": ["factory"]}' 'init-bytes'
before=$(cd "$root" && find . -type f | LC_ALL=C sort | xargs cksum)
out=$("$script" --dest "$dest" "$root" 2>&1); rc=$?
[ "$rc" -eq 0 ] || fail "a tagged init exits $rc: $out"
echo "$out" | grep -q "^added    Feathers/Init.fpreset (from Feathers/init.fpreset)$" || fail "the tagged init is not reported as the Init"
cmp -s "$root/Feathers/init.fpreset" "$dest/Feathers/Init.fpreset" || fail "Init is not the tagged init's bytes"
[ ! -e "$dest/Feathers/init.fpreset" ] || fail "the tagged init landed under its own case"
out=$("$script" --check --dest "$dest" "$root" 2>&1)
case "$out" in *Init*) fail "--check still sees the Init as out of date: $out" ;; esac
# A STALE FILE IS OUT OF DATE TOO (Chop left the root in step 4).
echo "$out" | grep -q "^stale    Devices/SKLITTER/Chop.fpreset" || fail "--check does not report the stale file"
# BOTH TAGGED: they would be one file. Refused, both named.
preset "$root/Feathers/Init.fpreset" '{"saved": 0, "tags": ["factory"]}' 'computed-init'
out=$("$script" --dest "$dest" "$root" 2>&1); rc=$?
[ "$rc" -eq 2 ] || fail "a tagged Init and init exit $rc, not 2"
echo "$out" | grep -q "Feathers/Init.fpreset and Feathers/init.fpreset" || fail "the two Inits are not both named: $out"
cmp -s "$root/Feathers/init.fpreset" "$dest/Feathers/Init.fpreset" || fail "a refused run replaced the Init"
preset "$root/Feathers/Init.fpreset" '{"saved": 0, "tags": []}' 'computed-init'
before=$(cd "$root" && find . -type f | LC_ALL=C sort | xargs cksum)

# 6. STALE FILES: kept and reported without --prune, removed with it.
out=$("$script" --dest "$dest" "$root" 2>&1)
echo "$out" | grep -q "^stale    Devices/SKLITTER/Chop.fpreset" || fail "a stale file is not reported"
[ -e "$dest/Devices/SKLITTER/Chop.fpreset" ] || fail "a stale file was removed without --prune"
out=$("$script" --prune --dest "$dest" "$root" 2>&1)
echo "$out" | grep -q "^removed  Devices/SKLITTER/Chop.fpreset$" || fail "--prune does not report the removal"
[ ! -e "$dest/Devices/SKLITTER/Chop.fpreset" ] || fail "--prune did not remove the stale file"
[ -e "$dest/Feathers/Lead.fpreset" ] || fail "--prune removed a tagged file"

# THE ROOT WAS ONLY READ.
after=$(cd "$root" && find . -type f | LC_ALL=C sort | xargs cksum)
[ "$before" = "$after" ] || fail "the root changed"

if [ "$failures" -ne 0 ]; then
	echo "$failures failure(s)"
	exit 1
fi
echo "collect-factory-presets: all checks passed"
