#!/usr/bin/env bash
# Copyright 2026 Owlet Records
#
# This file is part of Feathers.
#
# Feathers is free software: you can redistribute it and/or modify it under the
# terms of the GNU General Public License as published by the Free Software
# Foundation, either version 3 of the License, or (at your option) any later
# version.
#
# This program is distributed in the hope that it will be useful, but WITHOUT ANY
# WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
# PARTICULAR PURPOSE.  See the GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License along with
# this program.  If not, see <https://www.gnu.org/licenses/>.

# INSTALL THE LATEST FEATHERS RELEASE (macOS and Linux), in one command:
#
#   curl -fsSL https://raw.githubusercontent.com/owlet-labs/feathers/main/scripts/install.sh | bash
#
# It finds the newest release on GitHub, downloads the zip for this platform, checks it against the release's
# SHA256SUMS.txt, and copies the synth and the Chain (CLAP and VST3) into YOUR plugin folders; no administrator
# password is needed. A download made by curl is not quarantined by macOS (quarantine is added by browsers), so
# nothing needs clearing; the script clears the flag anyway in case an older copy carried it.
#
# Options (after `bash -s --` when piped):  --version v0.1.0   install that release instead of the latest
#                                           --dry-run          download and check, but copy nothing
# Windows: download the zip from the Releases page; see the README.
set -euo pipefail

repo="owlet-labs/feathers"
version=""
dry_run=0
while [ $# -gt 0 ]; do
	case "$1" in
		--version) version="${2:?--version needs a tag, e.g. v0.1.0}"; shift 2 ;;
		--dry-run) dry_run=1; shift ;;
		-h|--help) sed -n '19,30p' "$0" 2>/dev/null || true; exit 0 ;;
		*) echo "install.sh: unknown option $1" >&2; exit 2 ;;
	esac
done

say() { printf '\033[1m%s\033[0m\n' "$*"; }
die() { printf 'install.sh: %s\n' "$*" >&2; exit 1; }
need() { command -v "$1" >/dev/null 2>&1 || die "this needs '$1', which is not installed${2:+ ($2)}"; }

need curl
# Unzip with whatever this machine has: unzip (every Mac), bsdtar, or Python's zipfile module.
extract() {
	if command -v unzip >/dev/null 2>&1; then unzip -q "$1" -d "$2"
	elif command -v bsdtar >/dev/null 2>&1; then mkdir -p "$2" && bsdtar -xf "$1" -C "$2"
	elif command -v python3 >/dev/null 2>&1; then python3 -I -m zipfile -e "$1" "$2"
	else die "this needs 'unzip' to open the download (on Debian/Ubuntu: sudo apt install unzip)"; fi
}

os="$(uname -s)"
case "$os" in
	Darwin)
		suffix="macos-universal2"
		clap_dir="$HOME/Library/Audio/Plug-Ins/CLAP"
		vst3_dir="$HOME/Library/Audio/Plug-Ins/VST3"
		sha() { shasum -a 256 "$1" | cut -d' ' -f1; }
		;;
	Linux)
		[ "$(uname -m)" = "x86_64" ] || die "only Linux x86-64 builds are published (this machine is $(uname -m))"
		suffix="linux-x86_64"
		clap_dir="$HOME/.clap"
		vst3_dir="$HOME/.vst3"
		need sha256sum
		sha() { sha256sum "$1" | cut -d' ' -f1; }
		;;
	*) die "no installer for $os; download the zip from https://github.com/$repo/releases" ;;
esac

if [ -n "$version" ]; then api="https://api.github.com/repos/$repo/releases/tags/$version"
else api="https://api.github.com/repos/$repo/releases/latest"; fi
# `releases/latest` skips pre-releases; while every release is a pre-release, take the newest of all instead.
release_json="$(curl -fsSL "$api" 2>/dev/null || true)"
if [ -z "$release_json" ] && [ -z "$version" ]; then
	release_json="$(curl -fsSL "https://api.github.com/repos/$repo/releases?per_page=1")" || die "cannot reach GitHub"
fi
[ -n "$release_json" ] || die "release ${version:-latest} not found"

tag="$(printf '%s' "$release_json" | grep -o '"tag_name": *"[^"]*"' | head -1 | sed 's/.*"\([^"]*\)"$/\1/')"
url_of() { printf '%s' "$release_json" | grep -o '"browser_download_url": *"[^"]*"' | sed 's/.*"\([^"]*\)"$/\1/' | grep -- "$1" | head -1; }
zip_url="$(url_of "-$suffix.zip")"
sums_url="$(url_of "SHA256SUMS.txt")"
[ -n "$tag" ] && [ -n "$zip_url" ] || die "release ${version:-latest} has no $suffix build"
[ -n "$sums_url" ] || die "release $tag has no SHA256SUMS.txt, so the download cannot be checked"

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
zip_name="${zip_url##*/}"
say "Feathers $tag for $suffix"
curl -fL --progress-bar -o "$work/$zip_name" "$zip_url"
curl -fsSL -o "$work/SHA256SUMS.txt" "$sums_url"

want="$(grep -- " $zip_name\$" "$work/SHA256SUMS.txt" | head -1 | cut -d' ' -f1)"
got="$(sha "$work/$zip_name")"
[ -n "$want" ] || die "SHA256SUMS.txt does not list $zip_name"
[ "$want" = "$got" ] || die "checksum MISMATCH for $zip_name (expected $want, got $got); nothing installed"
echo "checksum ok"

extract "$work/$zip_name" "$work/x"
# The macOS zip holds one folder (with an install.command for people who download it by hand); Linux's is flat.
src="$work/x"
inner="$(find "$work/x" -mindepth 1 -maxdepth 1 -type d -name 'Feathers-*' | head -1)"
[ -n "$inner" ] && src="$inner"

if [ "$dry_run" -eq 1 ]; then
	say "dry run: would install into $clap_dir and $vst3_dir:"
	(cd "$src" && ls -d feathers*.clap feathers*.vst3)
	exit 0
fi

mkdir -p "$clap_dir" "$vst3_dir"
installed=()
for p in feathers.clap feathers-chain.clap; do
	[ -e "$src/$p" ] || continue
	rm -rf "${clap_dir:?}/$p"; cp -R "$src/$p" "$clap_dir/"; installed+=("$clap_dir/$p")
done
for p in feathers.vst3 feathers-chain.vst3; do
	[ -e "$src/$p" ] || continue
	rm -rf "${vst3_dir:?}/$p"; cp -R "$src/$p" "$vst3_dir/"; installed+=("$vst3_dir/$p")
done
[ "${#installed[@]}" -gt 0 ] || die "the zip held no plugins"
if [ "$os" = "Darwin" ]; then
	for p in "${installed[@]}"; do xattr -dr com.apple.quarantine "$p" 2>/dev/null || true; done
fi

say "Installed Feathers $tag:"
printf '  %s\n' "${installed[@]}"
echo "Restart your DAW, or have it rescan its plugins."
