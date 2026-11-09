#!/bin/bash
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
# Install the Feathers CLAP and VST3 bundles on macOS: strip Gatekeeper's quarantine, copy each
# into the matching user plugin folder, and say exactly what was installed.
#
# WHY THIS EXISTS. The CI build is signed only ad hoc (no Apple Developer ID, so no notarisation).
# Anything downloaded by a browser gets the com.apple.quarantine attribute, and a quarantined,
# unnotarised bundle is refused by Gatekeeper when a DAW tries to load it — silently, in most
# hosts: the plugin just does not appear. Clearing the attribute is the whole trick; the rest of
# this script is the copy and the receipt.
#
# AND IT IS THE TRICK A HAND COPY SKIPS. On CI run 8 (2026-09-08) a .clap dragged out of the
# plain CI artifact into ~/Library/Audio/Plug-Ins/CLAP kept its quarantine and Bitwig would not load
# it, while the .vst3 beside it loaded. If you copy a bundle by hand rather than through this
# script, run  xattr -dr com.apple.quarantine <the bundle>  yourself; this script does it for you.
#
# WHAT IT DOES, in order, stopping at the first failure:
#   1. refuses to run anywhere but macOS;
#   2. finds the bundles beside itself (the zip's own layout): CLAP and VST3, each optional —
#      installs whichever are present and says which it skipped;
#   3. removes com.apple.quarantine from every file inside each bundle and VERIFIES it is gone;
#   4. copies each into the matching user plugin folder — CLAP into ~/Library/Audio/Plug-Ins/CLAP/,
#      VST3 into ~/Library/Audio/Plug-Ins/VST3/ — with ditto, replacing any previous copy;
#   5. prints the installed path and the sha256 of each bundle's executable, so a bug report can
#      name the exact binary it was made against.
#
# Bash 3.2 throughout — that is the bash macOS ships, so no arrays-of-arrays, no `mapfile`, no
# `${var,,}`. `set -u` is on, so every variable is spelled out before it is read.
#
# This file ends in .command so Finder runs it in Terminal on double-click. Being inside the same
# zip as the bundles, it is quarantined too; see README.txt for the one right-click > Open it needs.

set -euo pipefail

fail() {
	printf '\ninstall.command: %s\n' "$*" >&2
	exit 1
}

# 1. macOS only. Everything below is macOS-specific: xattr's flag spelling, ditto, PlistBuddy, and
# the destination itself. On Linux the repo's scripts/install-clap.sh is the installer.
os="$(uname -s)"
if [ "$os" != "Darwin" ]; then
	fail "this installer is for macOS only (uname reports '$os'). On Linux use scripts/install-clap.sh from the repository; on Windows copy the .clap into %COMMONPROGRAMFILES%\\CLAP by hand."
fi

# 2. The bundles live beside this script. Resolve the script's own directory rather than trusting
# the working directory: Terminal starts a double-clicked .command in $HOME. CLAP and VST3 are each
# optional — a given archive may carry either or both; install what is present, skip what is not.
here="$(cd "$(dirname "$0")" && pwd)"
clap_bundles="feathers.clap feathers-chain.clap"
vst3_bundles="feathers.vst3 feathers-chain.vst3"
clap_dest="$HOME/Library/Audio/Plug-Ins/CLAP"
vst3_dest="$HOME/Library/Audio/Plug-Ins/VST3"

# The attribute is per file and Archive Utility stamps every file it extracts, so the check has to
# be recursive too. `xattr -d` exits non-zero for a file that never had the attribute, which is not
# a failure here, so the removal's own exit code is ignored and the RESULT is what is checked.
strip_quarantine() {
	xattr -dr com.apple.quarantine "$1" 2>/dev/null || true
	if xattr -lr "$1" 2>/dev/null | grep -q 'com.apple.quarantine'; then
		fail "com.apple.quarantine is still set somewhere under $1 after removal; Gatekeeper would refuse it. Run: xattr -dr com.apple.quarantine \"$1\""
	fi
}

# The bundle's executable is whatever Info.plist names, which is the one file Gatekeeper and the
# host actually load. Fall back to the sole file in Contents/MacOS if the plist does not say.
main_binary() {
	bundle="$1"
	plist="$bundle/Contents/Info.plist"
	exe=""
	if [ -f "$plist" ]; then
		exe="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' "$plist" 2>/dev/null || true)"
	fi
	if [ -n "$exe" ] && [ -f "$bundle/Contents/MacOS/$exe" ]; then
		printf '%s\n' "$bundle/Contents/MacOS/$exe"
		return 0
	fi
	count="$(find "$bundle/Contents/MacOS" -type f 2>/dev/null | wc -l | tr -d ' ')"
	if [ "$count" = "1" ]; then
		find "$bundle/Contents/MacOS" -type f
		return 0
	fi
	return 1
}

# Install every bundle in `$1` (space-separated names) into `$2` (destination dir). Skips any bundle
# not found in the script's directory and reports it. Fails on any actual install error.
install_bundles() {
	local bundles="$1"
	local dest="$2"
	local label="$3"
	local found=0
	mkdir -p "$dest" || fail "could not create $dest"
	for b in $bundles; do
		if [ ! -d "$here/$b" ]; then
			echo "skip     $label/$b (not present in this archive)"
			continue
		fi
		found=1
		src="$here/$b"
		target="$dest/$b"

		# 3. Dequarantine the source first, so what ditto copies is already clean; then check the
		# copy too, because the copy is what the DAW loads.
		strip_quarantine "$src"

		# 4. Replace, never merge: a stale file left inside a bundle from an older build is a bug
		# that looks like the new build. ditto keeps symlinks, permissions and the rest of the bundle intact.
		rm -rf "$target" || fail "could not remove the previous $target"
		ditto "$src" "$target" || fail "ditto failed copying $b into $dest"
		strip_quarantine "$target"

		# 5. The receipt. The hash is of the executable, not the bundle, because the bundle's plist
		# and signature files change with every signing and the executable is what the sound comes from.
		exe="$(main_binary "$target")" || fail "could not find the executable inside $target (no CFBundleExecutable and Contents/MacOS is not a single file)"
		sum="$(shasum -a 256 "$exe" | cut -d ' ' -f 1)"
		echo "installed  $target"
		echo "  binary   $exe"
		echo "  sha256   $sum"

		# Informational only. An ad-hoc signature is what lets Apple silicon load the binary at all;
		# if it is missing or broken this says so, but the install is still the install.
		#
		# VERIFIED AT THE EXECUTABLE, NOT THE BUNDLE, AND ON THE arm64 SLICE. Since 2026-09-08 CI
		# signs nothing itself: the bundle carries the LINKER's ad-hoc signature on its arm64 slice
		# and no Contents/_CodeSignature seal — the same state the .vst3 is in, which is the one that
		# loaded from a hand copy. So a bundle-level `codesign --verify --deep` would report a missing
		# resource envelope for a perfectly loadable bundle, and a verify without an architecture would
		# fail on the x86_64 slice, which the linker leaves unsigned because Intel Macs do not
		# require one. The arm64 slice's own signature is the thing an Apple silicon loader checks,
		# and it is what is asked here.
		#
		# AND ON A BARE COPY OUTSIDE THE BUNDLE: codesign given a bundle's main executable resolves
		# the bundle and judges THAT, and an unsealed bundle does not verify as a bundle — so verifying
		# the installed path would warn on every correct install. The copy carries the same bytes and
		# the same embedded signature and has no bundle around it.
		probe="$(mktemp -d)"
		cp "$exe" "$probe/mach-o"
		if ! codesign --verify --strict --architecture arm64 "$probe/mach-o" 2>/dev/null; then
			echo "  WARNING  codesign does not verify this executable; Apple silicon Macs may refuse to load it."
		fi
		rm -rf "$probe"
		echo
	done
	if [ "$found" -eq 0 ]; then
		echo "  (no $label bundles in this archive)"
		echo
	fi
}

echo "Installing Feathers"
echo
install_bundles "$clap_bundles" "$clap_dest" "CLAP"
install_bundles "$vst3_bundles" "$vst3_dest" "VST3"

echo "Done. Restart your DAW, or rescan its plugins, before it will see the new build."
echo
echo "If you ever copy a bundle by hand from the plain CI artifact instead of running this script,"
echo "it keeps its quarantine and the DAW will not load it. Run:"
echo "    xattr -dr com.apple.quarantine <the copied bundle>"
echo "This script has already done that for the bundles it installed."
