#!/usr/bin/env bash
# Local dev toolchain (host has no system cmake/ninja; g++-14 came from
# ppa:ubuntu-toolchain-r/test since jammy ships no such package).
# CI uses its own toolchains — this file is for this machine only.
#
# HOST-LOCAL SETTINGS live in an untracked `.env.local` at the repository root (gitignored), so this
# script names no machine's paths. It may set FEATHERS_TOOLCHAIN_DIR: a directory holding `cmake/bin`
# and `ninja-build/bin`, put first on PATH. Unset, the cmake and ninja already on PATH are used. A
# worktree has no copy of the untracked file, so the main checkout's is read when the worktree has none.
_feathers_root="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")/.." && pwd)"
_feathers_local="$_feathers_root/.env.local"
if [ ! -f "$_feathers_local" ]; then
	_feathers_common="$(git -C "$_feathers_root" rev-parse --path-format=absolute --git-common-dir 2>/dev/null)"
	[ -n "$_feathers_common" ] && _feathers_local="$(dirname "$_feathers_common")/.env.local"
fi
# shellcheck source=/dev/null
[ -f "$_feathers_local" ] && . "$_feathers_local"
unset _feathers_root _feathers_local _feathers_common
if [ -n "${FEATHERS_TOOLCHAIN_DIR:-}" ]; then
	export PATH="$FEATHERS_TOOLCHAIN_DIR/cmake/bin:$FEATHERS_TOOLCHAIN_DIR/ninja-build/bin:$PATH"
fi
export CC=gcc-14 CXX=g++-14
# ccache lives in ~/.local/bin (no root on this host; a static release binary). The top-level
# CMakeLists.txt finds it on PATH and fronts the compiler with it -- a launcher, not a flag.
case ":$PATH:" in *":$HOME/.local/bin:"*) ;; *) export PATH="$HOME/.local/bin:$PATH" ;; esac
# CROSS-WORKTREE HITS NEED THIS. ccache hashes the absolute paths of the source and of every header
# it pulls in, so an object built under one worktree is a MISS under another -- MEASURED 2026-09-02:
# the same tree rebuilt in place hit 474/475, a second worktree hit 4/475. With a base dir the paths
# are made relative before hashing (and the compiler is invoked with them), and the second worktree
# hit 469/475. Worktrees must sit at the same depth for the relative paths to agree; every one under
# ~/.config/superpowers/worktrees/feathers/ does.
export CCACHE_BASEDIR="$HOME"
# And hash the COMPILER'S CONTENT rather than its mtime: ccache keys the two modes differently, so a
# cache written under one is unreachable under the other. The Docker dev container hashes content
# (its g++-14 is a different file from the host's at the same version), and MEASURED from inside with
# the host still on mtime: 3 of 492 objects carried across the boundary, against 486 of 492 between two
# worktrees on the same side. Same mode both sides, same bytes, one cache.
export CCACHE_COMPILERCHECK=content
# THE CACHE CAP. The default 5 GiB was 100% full with a 49% hit rate on 2026-09-25: every worktree
# carries its own build, build-asan and install configs, which together far exceed 5 GiB, so a fresh
# worktree recompiled almost everything, so the limit is 15 GiB. Set here rather than
# with `ccache -M` because ~/.config is read-only in the sandbox; the variable governs cleanup on
# every compile made through this file.
export CCACHE_MAXSIZE=15G
# BUILDS AND TESTS GIVE WAY TO THE AUDIO THREAD. Bitwig runs on this host, and on
# 2026-11-05 our builds at load ~60 inflated Feathers' real CPU per block ~4x (p50 2.2 ms quiet, 8.2 ms
# under load) and caused the live dropouts. Everything built or tested after sourcing this file runs at a
# lowered priority: CPU nice at least 10 and the LOWEST best-effort I/O class. MEASURED: the dev container
# already runs everything at nice 12, so the CPU half was in force when the dropouts happened -- nice does
# not stop cache, memory-bandwidth and SMT contention. The real lever is HOW MUCH builds at once (CLAUDE.md). Deliberately NOT nice 19 and not
# the idle I/O class (-c3):, so a build still
# outranks idle-class background work, and only yields to normal-priority work such as the DAW.
[ "$(ps -o ni= -p $$ 2>/dev/null | tr -d " ")" -lt 10 ] 2>/dev/null && renice -n $((10 - $(ps -o ni= -p $$ | tr -d " "))) -p $$ >/dev/null 2>&1 || true
ionice -c2 -n7 -p $$ >/dev/null 2>&1 || true
