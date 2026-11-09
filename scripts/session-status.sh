#!/usr/bin/env bash
# Who is working in this repo, on what, and how stale are they?
#
# This file is part of Feathers.
# Copyright (C) 2026 Owlet Records — GPL-3.0-or-later.
#
# WHY THIS EXISTS. On 2026-08-24 a session reported the main workspace "broken by an incomplete edit
# from another session". It was not: their branch predated the commit that added the symbol they were
# missing. The information that would have settled it in one second -- am I behind? -- took a
# conversation round trip and produced a wrong accusation on the way. Twice in the same session a
# stale or misread base led to a wrong conclusion about someone else's work.
#
# So: BEFORE blaming a peer, run this. Before merging, run this. It answers "is the red in front of
# me mine, theirs, or just old?" without asking anyone.

set -uo pipefail
INTEGRATION=${1:-v1-implementation}

git rev-parse --verify --quiet "$INTEGRATION" >/dev/null || {
	echo "no such integration branch: $INTEGRATION" >&2; exit 2; }

printf '%-34s %-22s %7s %7s  %s\n' "WORKTREE" "BRANCH" "AHEAD" "BEHIND" "TREE"
printf '%.0s-' {1..92}; echo

git worktree list --porcelain | awk '/^worktree /{w=$2} /^branch /{b=$2; print w"\t"b} /^detached/{print w"\tDETACHED"}' \
| while IFS=$'\t' read -r dir ref; do
	branch=${ref#refs/heads/}
	short=$(basename "$dir")
	# Ahead/behind the integration branch. A detached head still has a commit, so still answerable.
	if [[ $branch == DETACHED ]]; then
		head=$(git -C "$dir" rev-parse --short HEAD 2>/dev/null || echo '?')
		branch="(detached $head)"
	fi
	counts=$(git rev-list --left-right --count "$INTEGRATION...$(git -C "$dir" rev-parse HEAD 2>/dev/null)" 2>/dev/null)
	behind=$(awk '{print $1}' <<<"$counts"); ahead=$(awk '{print $2}' <<<"$counts")
	# A dirty tree is the single most important column: it is the difference between "holding" and
	# "mid-edit", and CLAUDE.md's hardest-won rule is that you cannot infer that from quietness.
	dirty=$(git -C "$dir" status --porcelain 2>/dev/null | wc -l)
	if (( dirty > 0 )); then
		tree="DIRTY ($dirty)"
	else
		tree="clean"
	fi
	printf '%-34s %-22s %7s %7s  %s\n' "${short:0:34}" "${branch:0:22}" "${ahead:-?}" "${behind:-?}" "$tree"
done

echo
echo "AHEAD  = commits this worktree has that $INTEGRATION does not (work waiting to be merged)"
echo "BEHIND = commits $INTEGRATION has that it does not (STALENESS -- the column that causes"
echo "         'another session broke the build'. A peer BEHIND by anything cannot see your last merge.)"
echo "TREE   = uncommitted changes. DIRTY is not a fault; it means do not read that tree as a"
echo "         statement of intent, and never write into it."
