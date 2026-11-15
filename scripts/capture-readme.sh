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
# Regenerates every image of the README (docs/readme/) from a build with the tools on, and optionally the
# website's compressed copies, so the pictures can be refreshed with one command whenever the UI changes.
#
# Usage: scripts/capture-readme.sh <build-dir> [--only cards|windows|shots|gif|collages|site] [--check]
#                                  [--site-dir <dir>] [--reuse <dir>] [--keep]
#
#   <build-dir>   a build configured with -DFEATHERS_BUILD_TOOLS=ON that has widget-shot, guihost and
#                 feathers.clap (ninja -C <build-dir> widget-shot guihost feathers_clap).
#   --only        one group: cards (40 device cards), windows (20 expanded windows), shots (the 13 numbered
#                 pictures), collages (the four collages, built from the images already in docs/readme),
#                 site (the website's WebP copies, built from docs/readme). Default: all of them, in that order.
#   --check       writes nothing into the repository: captures into a temporary directory and reports which
#                 images differ from the committed ones. Exit 1 when any differ, so it doubles as a
#                 "the screenshots are stale" check.
#   --site-dir    the website's image directory (site/feathers/img of the website branch). Default:
#                 <repo>/site/feathers/img if it exists; with none, site mode is skipped.
#   --reuse       development aid: take the raw captures from <dir>/<shot id>.png instead of running them.
#   --keep        keep the temporary directory (raw captures included) and print its path.
#
# WHAT IT DOES. Every capture runs under scripts/venue-xvfb.sh (a private Xvfb, lavapipe Vulkan, full redraw),
# at FEATHERS_UI_SCALE=1.5 with FEATHERS_REDUCED_MOTION=1, with audio flowing through every device that meters
# (--audio / --mod-run) so no meter reads "no signal". The captures and the crop rectangles are in
# scripts/readme-captures.tsv; the collage layouts and the WebP sizes are below. A capture is refused when its
# frame is blank, a crop runs off its frame, or two images are byte-identical.
set -u

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
TABLE="$ROOT/scripts/readme-captures.tsv"
README_DIR="$ROOT/docs/readme"

BG='#05070D'
LABEL_FG='#C8D2E6'
HEAD_FG='#7f8aa3'
FONT=/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf
FONT_BOLD=/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf
UI_SCALE=1.5
SHOT_SECONDS_LIMIT=240
WEBP_QUALITY=82

die() {
	echo "capture-readme: $*" >&2
	exit 1
}

BUILD=""
ONLY=""
CHECK=0
KEEP=0
SITE_DIR=""
REUSE=""
while [ $# -gt 0 ]; do
	case "$1" in
	--only) ONLY="${2:-}"; shift 2 ;;
	--check) CHECK=1; shift ;;
	--keep) KEEP=1; shift ;;
	--site-dir) SITE_DIR="${2:-}"; shift 2 ;;
	--reuse) REUSE="${2:-}"; shift 2 ;;
	-h | --help) sed -n 14,36p "$0"; exit 0 ;;
	-*) die "unknown option $1" ;;
	*) [ -z "$BUILD" ] || die "one build directory only"; BUILD="$1"; shift ;;
	esac
done
[ -n "$BUILD" ] || die "usage: scripts/capture-readme.sh <build-dir> [--only cards|windows|shots|gif|collages|site] [--check]"
case "$ONLY" in "" | cards | windows | shots | gif | collages | site) ;; *) die "--only takes cards, windows, shots, gif, collages or site" ;; esac
BUILD=$(cd "$BUILD" 2>/dev/null && pwd) || die "no such build directory"
for tool in convert identify montage md5sum; do
	command -v "$tool" >/dev/null 2>&1 || die "$tool (ImageMagick/coreutils) is required"
done
[ -r "$FONT" ] && [ -r "$FONT_BOLD" ] || die "DejaVu fonts are required at $FONT"
if [ -z "$REUSE" ] && [ "$ONLY" != collages ] && [ "$ONLY" != site ]; then
	for f in widget-shot guihost feathers.clap; do
		[ -e "$BUILD/$f" ] || die "$BUILD/$f is missing: configure with -DFEATHERS_BUILD_TOOLS=ON and build widget-shot guihost feathers_clap"
	done
fi

if [ -z "$SITE_DIR" ] && [ -d "$ROOT/site/feathers/img" ]; then
	SITE_DIR="$ROOT/site/feathers/img"
fi

TMP=$(mktemp -d -t capture-readme.XXXXXXXX) || die "no temporary directory"
if [ "$KEEP" = 1 ]; then
	echo "kept: $TMP"
else
	trap 'rm -rf "$TMP"' EXIT
fi
mkdir -p "$TMP/shots" "$TMP/site"
if [ "$CHECK" = 1 ]; then
	# The committed tree is copied first so a partial run (--only) compares only what it regenerated.
	OUT="$TMP/out"
	mkdir -p "$OUT"
	cp -a "$README_DIR/." "$OUT/"
else
	OUT="$README_DIR"
fi
mkdir -p "$OUT/cards" "$OUT/windows"

wants() { [ -z "$ONLY" ] || [ "$ONLY" = "$1" ]; }

# --- the tables ------------------------------------------------------------------------------------------
declare -A SHOT_GEOM SHOT_TOOL SHOT_ARGS LIVE VARS
declare -a CROPS=() NAMES=() GIFS=()
while IFS=$'\t' read -r kind a b c d; do
	case "$kind" in
	"" | \#*) continue ;;
	shot) SHOT_GEOM[$a]="$b"; SHOT_TOOL[$a]="$c"; SHOT_ARGS[$a]="$d" ;;
	crop) CROPS+=("$a	$b	$c") ;;
	name) NAMES+=("$a	$b") ;;
	live) LIVE[$a]=1 ;;
	var) VARS[$a]="$b" ;;
	gif) GIFS+=("$a	$b	$c	$d")  ;;
	*) die "unknown row kind '$kind' in $TABLE" ;;
	esac
done <"$TABLE"

declare -A LABEL
for row in "${NAMES[@]}"; do
	IFS=$'\t' read -r slug label <<<"$row"
	LABEL[$slug]="$label"
done

# --- helpers ---------------------------------------------------------------------------------------------
LOG="$TMP/log"
: >"$LOG"
declare -A SEEN_MD5
FAILED=0

colours() { identify -format '%k' "$1" 2>/dev/null || echo 0; }

# Refuses a blank frame and a duplicate of an earlier image.
verify_image() {
	local file=$1 what=$2 n sum
	[ -s "$file" ] || die "$what: no image at $file"
	n=$(colours "$file")
	[ "$n" -ge 64 ] || die "$what: BLANK FRAME ($n colours)"
	sum=$(md5sum <"$file" | cut -d' ' -f1)
	if [ -n "${SEEN_MD5[$sum]:-}" ]; then
		die "$what is byte-identical to ${SEEN_MD5[$sum]}: one of the two captures did not show what it claims"
	fi
	SEEN_MD5[$sum]="$what"
}

# capture <id>: the raw frame lands in $TMP/shots/<id>.png, once per run.
capture() {
	local id=$1 dst="$TMP/shots/$1.png"
	[ -s "$dst" ] && return 0
	if [ -n "$REUSE" ]; then
		[ -s "$REUSE/$id.png" ] || die "--reuse: $REUSE/$id.png is missing"
		cp "$REUSE/$id.png" "$dst"
		return 0
	fi
	[ -n "${SHOT_TOOL[$id]:-}" ] || die "no shot '$id' in $TABLE"
	local geom=${SHOT_GEOM[$id]} tool=${SHOT_TOOL[$id]} args=() cmd=()
	[ "$geom" = "-" ] && geom=2560x1600x24
	IFS='|' read -ra args <<<"${SHOT_ARGS[$id]}"
	# An argument "@fpreset:<path>" is a factory preset, handed to guihost as the saved state it holds (a preset
	# file is the magic, a version byte, a u32 length of its JSON header, the header, then the plugin state).
	local i hdr
	for i in "${!args[@]}"; do
		case "${args[$i]}" in
		@fpreset:*)
			local pf="$ROOT/${args[$i]#@fpreset:}"
			[ -r "$pf" ] || die "shot $id: no preset $pf"
			hdr=$(od -An -tu4 -j5 -N4 "$pf" | tr -d ' ')
			tail -c +$((9 + hdr + 1)) "$pf" >"$TMP/$id.state"
			args[$i]="$TMP/$id.state"
			;;
		esac
	done
	# ${NAME} is a setting from a "var" row; "@env:K=V" sets a variable for this capture only.
	local envs=() kept=() name
	for i in "${!args[@]}"; do
		for name in "${!VARS[@]}"; do
			args[$i]=${args[$i]//\$\{$name\}/${VARS[$name]}}
		done
		case "${args[$i]}" in
		@env:*) envs+=("${args[$i]#@env:}") ;;
		*) kept+=("${args[$i]}") ;;
		esac
	done
	args=("${kept[@]}")
	case "$tool" in
	widget-shot) cmd=("$BUILD/widget-shot" "${args[0]}" "$dst" "${args[@]:1}") ;;
	guihost)
		mkdir -p "$TMP/guihost-$id"
		cmd=("$BUILD/guihost" "$BUILD/feathers.clap" "$TMP/guihost-$id" "${args[@]}")
		;;
	*) die "unknown tool '$tool' for shot $id" ;;
	esac
	echo "capture $id ($tool ${args[*]:0:3}...)"
	(
		cd "$ROOT" &&
			FEATHERS_UI_SCALE=$UI_SCALE FEATHERS_REDUCED_MOTION=1 VISAGE_FULL_REDRAW=1 FEATHERS_XVFB_GEOMETRY="$geom" \
				env "${envs[@]}" timeout "$SHOT_SECONDS_LIMIT" nice -n 10 scripts/venue-xvfb.sh run -- "${cmd[@]}"
	) >"$TMP/shots/$id.log" 2>&1 || {
		tail -8 "$TMP/shots/$id.log" >&2
		die "capture $id failed"
	}
	if [ "$tool" = guihost ]; then
		# guihost writes its before/after frames into the directory; the first is the editor as it opened.
		mv "$TMP/guihost-$id/guihost_before.png" "$dst" 2>/dev/null || die "guihost wrote no frame for $id"
	fi
	[ -s "$dst" ] || die "capture $id wrote no image (see $TMP/shots/$id.log)"
	verify_image "$dst" "raw capture $id"
}

crop_one() {
	local out=$1 id=$2 rect=$3 src="$TMP/shots/$2.png" dst="$OUT/$1"
	capture "$id"
	mkdir -p "$(dirname "$dst")"
	if [ "$rect" = "-" ]; then
		convert "$src" -depth 8 -strip "$dst"
		return 0
	fi
	local want=${rect%%+*} got fw fh
	read -r fw fh <<<"$(identify -format '%w %h' "$src")"
	convert "$src" -crop "$rect" +repage -depth 8 -strip "$dst"
	got=$(identify -format '%wx%h' "$dst")
	[ "$got" = "$want" ] || die "$out: the crop $rect runs off its ${fw}x${fh} frame (got $got): the UI moved, update the table"
	# A crop that is only background means the rectangle moved off the thing it frames.
	verify_image "$dst" "$out"
}

run_crops() {
	local group=$1 row out id rect
	for row in "${CROPS[@]}"; do
		IFS=$'\t' read -r out id rect <<<"$row"
		case "$out" in
		cards/*) [ "$group" = cards ] || continue ;;
		windows/*) [ "$group" = windows ] || continue ;;
		*) [ "$group" = shots ] || continue ;;
		esac
		crop_one "$out" "$id" "$rect"
	done
}

# --- collages --------------------------------------------------------------------------------------------
# The modulation collage: four pictures at one height, side by side.
collage_modulation() {
	local H=700 G=16 M=8 files=() ws=() f w h x i W args
	files=("$OUT/02-modulation-lane-and-routes.png" "$OUT/05-explore-terrain.png" "$OUT/03-twin-field-modulator.png" "$OUT/04-pattern-window.png")
	for f in "${files[@]}"; do
		[ -s "$f" ] || die "collage-modulation needs $f"
		read -r w h <<<"$(identify -format '%w %h' "$f")"
		ws+=("$(awk -v w="$w" -v h="$h" -v H=$H 'BEGIN{printf "%d", w*H/h+0.5}')")
	done
	W=$((ws[0] + ws[1] + ws[2] + ws[3] + 3 * G + 2 * M))
	args=(-size "${W}x$((H + 2 * M))" "xc:$BG")
	x=$M
	for i in 0 1 2 3; do
		args+=('(' "${files[$i]}" -resize "${ws[$i]}x$H!" ')' -geometry "+$x+$M" -composite)
		x=$((x + ws[i] + G))
	done
	convert "${args[@]}" -depth 8 -strip "$OUT/collage-modulation.png"
}

# The effects collage: Phaser/Flanger, Compressor, Resonate, SKLITTER, one per quarter of a 2000x1013 sheet,
# each fitted into its cell with a 7 px margin and centred.
collage_effects() {
	local files=() i c r cx cy ch nw nh x y args
	files=("$OUT/07-phaser-flanger.png" "$OUT/09-compressor-excomper.png" "$OUT/11-resonate-with-fx-lanes.png" "$OUT/10-sklitter.png")
	args=(-size 2000x1013 "xc:$BG")
	for i in 0 1 2 3; do
		[ -s "${files[$i]}" ] || die "collage-effects needs ${files[$i]}"
		c=$((i % 2)); r=$((i / 2))
		read -r nw nh <<<"$(convert "${files[$i]}" -resize 986x493 -format '%w %h' info:)"
		cx=$((c * 1000))
		cy=$((r == 0 ? 0 : 506))
		ch=$((r == 0 ? 507 : 506))
		x=$((cx + (1000 - nw + 1) / 2))
		y=$((cy + (ch - nh + 1) / 2))
		args+=('(' "${files[$i]}" -resize 986x493 ')' -geometry "+$x+$y" -composite)
	done
	convert "${args[@]}" -depth 8 -strip "$OUT/collage-effects.png"
}

# Every expanded window, labelled, five to a row. montage reserves the label's row (drawn in the background
# colour) and the visible label is then drawn four pixels lower, where the published sheet has it.
collage_windows() {
	local order="harm:Harm owltt:OWLTT sklitter:SKLITTER waypoint:Waypoint drawn:Drawn shaper:Shaper automod:Automod chain:Chain delay:Delay featherverb:Featherverb compressor:Compressor resonate:Resonate smooth-shaper:Smooth_Shaper eq:EQ disperser:Disperser phaser-flanger:Phaser/Flanger spectral:Spectral rack:Rack arp:Arp pattern:Pattern"
	local o f l tw x y i=0 args=() ann=()
	for o in $order; do
		f=${o%%:*}; l=${o#*:}; l=${l//_/ }
		[ -s "$OUT/windows/$f.png" ] || die "collage-all-windows needs windows/$f.png"
		args+=(-label "$l" "$OUT/windows/$f.png")
		tw=$(convert -font "$FONT" -pointsize 22 "label:$l" -format %w info:)
		x=$(((i % 5) * 580 + (580 - tw) / 2))
		y=$(((i / 5) * 471 + 436))
		ann+=(-annotate "+$x+$y" "$l")
		i=$((i + 1))
	done
	montage -font "$FONT" -pointsize 22 -background "$BG" -fill "$BG" "${args[@]}" -tile 5x4 -geometry 560x420+10+10 \
		-depth 8 "$TMP/windows-sheet.png"
	convert "$TMP/windows-sheet.png" -font "$FONT" -pointsize 22 -fill "$LABEL_FG" -gravity NorthWest "${ann[@]}" \
		-depth 8 -strip "$OUT/collage-all-windows.png"
}

# Every device card, grouped by category, on one sheet.
collage_cards() {
	local M=40 GAP=26 GGAP=70 HEAD=56 LAB=44
	# group name|first slug|...  ; a row is groups, optionally a slice "Modulators:0:7"
	local -A G=(
		["Filter & EQ"]="drawn eq filter resonate spectral waypoint"
		["Dynamics"]="compressor owltt"
		["Drive & Colour"]="clipper eroder shaper smooth-shaper"
		["Time & Space"]="delay featherverb"
		["Pitch"]="pitch"
		["Rhythm & Glitch"]="sklitter"
		["Modulation FX"]="automod disperser phaser-flanger"
		["Utility"]="bus-utility harm noise sidechain-inject"
		["Modulators"]="dahdsr follower lfo macros math mseg note-expressions random-trigger sample-hold step-seq stereo-tool tracker twin-field"
		["Note FX"]="arp pattern"
		["Containers"]="chain rack"
	)
	local plan=("Filter & EQ|Pitch" "Dynamics|Drive & Colour|Rhythm & Glitch" "Time & Space|Modulation FX|Note FX" "Utility|Containers" "Modulators:0:7" "Modulators:7:13")
	local draws=() y=$M maxx=0 spec g a b items slug i x w h rh tw first label head
	for spec in "${plan[@]}"; do
		local IFS_OLD=$IFS cells=() cx=$M started=0
		IFS='|' read -ra groups <<<"$spec"
		IFS=$IFS_OLD
		rh=0
		for g in "${groups[@]}"; do
			a=0; b=999
			if [[ "$g" == *:*:* ]]; then
				IFS=: read -r g a b <<<"$g"
				IFS=$IFS_OLD
			fi
			read -ra items <<<"${G[$g]}"
			items=("${items[@]:a:b-a}")
			for ((i = 0; i < ${#items[@]}; i++)); do
				slug=${items[$i]}
				[ -s "$OUT/cards/$slug.png" ] || die "collage-all-cards needs cards/$slug.png"
				read -r w h <<<"$(identify -format '%w %h' "$OUT/cards/$slug.png")"
				if [ "$started" = 1 ]; then
					if [ "$i" = 0 ]; then cx=$((cx + GGAP)); else cx=$((cx + GAP)); fi
				fi
				started=1
				head=""
				[ "$i" = 0 ] && [ "$a" = 0 ] && head=$g
				cells+=("$slug|$w|$h|$cx|$head")
				[ "$h" -gt "$rh" ] && rh=$h
				cx=$((cx + w))
				[ "$cx" -gt "$maxx" ] && maxx=$cx
			done
		done
		local c
		for c in "${cells[@]}"; do
			IFS='|' read -r slug w h x head <<<"$c"
			if [ -n "$head" ]; then
				draws+=(-font "$FONT_BOLD" -pointsize 24 -fill "$HEAD_FG" -annotate "+$x+$((y + 12))" "${head^^}")
			fi
			draws+=(-draw "image over $x,$((y + HEAD)) $w,$h '$OUT/cards/$slug.png'")
			label=${LABEL[$slug]:-$slug}
			tw=$(convert -font "$FONT" -pointsize 22 "label:$label" -format %w info:)
			draws+=(-font "$FONT" -pointsize 22 -fill "$LABEL_FG" -gravity NorthWest -annotate "+$((x + (w - tw) / 2))+$((y + HEAD + h + 12))" "$label")
		done
		y=$((y + HEAD + rh + LAB + 30))
	done
	local W=$((maxx + M)) H=$((y - 30 + M - 10))
	convert -size "${W}x${H}" "xc:$BG" -gravity NorthWest "${draws[@]}" -depth 8 -strip "$OUT/collage-all-cards.png"
}

run_collages() {
	collage_modulation
	collage_effects
	collage_windows
	collage_cards
	local f
	for f in collage-modulation collage-effects collage-all-windows collage-all-cards; do
		verify_image "$OUT/$f.png" "$f.png"
	done
}

# --- animated GIFs ---------------------------------------------------------------------------------------
# The frames are separate, deterministic captures (widget-shot sets the plane's turn per frame, no real-time
# animation); each is cropped and scaled, and they are assembled with one loop lasting <loop ms>.
GIF_MAX_BYTES=3000000
run_gif() {
	local row file id rect frames loop scale colours k src dst delay sum
	for row in "${GIFS[@]}"; do
		IFS=$'\t' read -r file id rect frames loop scale colours <<<"$row"
		capture "$id"
		local dir="$TMP/gif-$id"
		mkdir -p "$dir"
		declare -A seen=()
		for ((k = 0; k < frames; k++)); do
			printf -v src '%s/shots/%s-r%02d.png' "$TMP" "$id" "$k"
			if [ -n "$REUSE" ] && [ ! -s "$src" ]; then cp "$REUSE/$id-r$(printf %02d "$k").png" "$src"; fi
			[ -s "$src" ] || die "gif $file: frame $k was not written ($src)"
			dst="$dir/f$(printf %02d "$k").png"
			convert "$src" -crop "$rect" +repage -resize "${scale}%" -depth 8 -strip "$dst"
			verify_image "$dst" "gif $file frame $k"
		done
		delay=$((loop / 10 / frames))
		convert -delay "$delay" -loop 0 "$dir"/f*.png -layers Optimize -colors "$colours" -strip "$OUT/$file"
		[ "$(stat -c %s "$OUT/$file")" -le "$GIF_MAX_BYTES" ] || die "$file is $(stat -c %s "$OUT/$file") bytes, over $GIF_MAX_BYTES"
		echo "$file: $frames frames, $(stat -c %s "$OUT/$file") bytes, loop $((delay * frames * 10)) ms"
	done
}

# --- the website's copies --------------------------------------------------------------------------------
# name  source image  width. The same sources and widths the site was built from, WebP at quality 82.
SITE_TABLE=(
	"hero.webp|01-hero-full-editor.png|1600"
	"terrain.webp|05-explore-terrain.png|1000"
	"rack.webp|12-rack-parallel-chains.png|1000"
	"modulation.webp|collage-modulation.png|1600"
	"effects.webp|collage-effects.png|1600"
	"cards.webp|collage-all-cards.png|1400"
)
SITE_OUT=""
run_site() {
	if [ -z "$SITE_DIR" ] || [ ! -d "$SITE_DIR" ]; then
		echo "site: no site directory (pass --site-dir <dir>); skipped"
		return 0
	fi
	SITE_OUT="$TMP/site"
	[ "$CHECK" = 1 ] || SITE_OUT="$SITE_DIR"
	local row name src w
	for row in "${SITE_TABLE[@]}"; do
		IFS='|' read -r name src w <<<"$row"
		[ -s "$OUT/$src" ] || die "site needs $OUT/$src"
		convert "$OUT/$src" -resize "${w}x" -quality "$WEBP_QUALITY" "$SITE_OUT/$name"
		[ -s "$SITE_OUT/$name" ] || die "site: $name was not written"
	done
	# The animated pictures go as they are.
	for row in "${GIFS[@]}"; do
		name=${row%%$'\t'*}
		[ -s "$OUT/$name" ] || die "site needs $OUT/$name (run --only gif)"
		cp "$OUT/$name" "$SITE_OUT/$name"
	done
}

# --- run -------------------------------------------------------------------------------------------------
START=$(date +%s)
wants cards && run_crops cards
wants windows && run_crops windows
wants shots && run_crops shots
wants gif && run_gif
wants collages && run_collages
wants site && run_site

# --- the report ------------------------------------------------------------------------------------------
if [ "$CHECK" = 1 ]; then
	DIFFS=0 SAME=0
	# Compared by pixels: the committed PNGs carry a timestamp and text chunks that ours (stripped, so a
	# rerun is byte-stable) do not.
	compare_one() {
		local new=$1 old=$2 name=$3 ndim odim ae
		if [ ! -e "$old" ]; then
			echo "NEW      $name"
			DIFFS=$((DIFFS + 1))
			return
		fi
		if [[ "$name" == *.gif ]]; then
			if cmp -s "$new" "$old"; then SAME=$((SAME + 1)); else echo "DIFFERS  $name  (bytes differ)"; DIFFS=$((DIFFS + 1)); fi
			return
		fi
		ndim=$(identify -format '%wx%h' "$new" 2>/dev/null | head -1)
		odim=$(identify -format '%wx%h' "$old" 2>/dev/null | head -1)
		if [ "$ndim" != "$odim" ]; then
			echo "DIFFERS  $name  (committed $odim, now $ndim)"
			DIFFS=$((DIFFS + 1))
			return
		fi
		ae=$(compare -metric AE "$new" "$old" null: 2>&1 | awk '{printf "%d", $1}')
		if [ "$ae" = 0 ]; then
			SAME=$((SAME + 1))
		elif [ -n "${LIVE[$name]:-}" ] && [ $((ae * 100)) -le $((8 * ${ndim%x*} * ${ndim#*x})) ]; then
			echo "live     $name  ($ae pixels differ: a live display, within tolerance)"
			SAME=$((SAME + 1))
		else
			echo "DIFFERS  $name  ($ndim, $ae pixels)"
			DIFFS=$((DIFFS + 1))
		fi
	}
	while read -r f; do
		f=${f#./}
		compare_one "$OUT/$f" "$README_DIR/$f" "$f"
	done < <(cd "$OUT" && find . \( -name '*.png' -o -name '*.gif' \) | sort)
	if wants site && [ -n "$SITE_OUT" ]; then
		for row in "${SITE_TABLE[@]}"; do
			name=${row%%|*}
			compare_one "$SITE_OUT/$name" "$SITE_DIR/$name" "site/$name"
		done
		for row in "${GIFS[@]}"; do
			name=${row%%$'\t'*}
			compare_one "$SITE_OUT/$name" "$SITE_DIR/$name" "site/$name"
		done
	fi
	echo "checked: $SAME identical, $DIFFS differ ($(($(date +%s) - START)) s)"
	[ "$DIFFS" = 0 ] || exit 1
	exit 0
fi
echo "done in $(($(date +%s) - START)) s"
