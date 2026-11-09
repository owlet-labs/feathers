#!/usr/bin/env bash
# The Feathers development container: build it, start it, get a shell or a Claude session inside.
#
#   docker/feathers-dev.sh                THE ONE COMMAND: builds the image if there is none, starts
#                                         the container if it is not running, and opens a Claude
#                                         session inside, PERMISSIONS BYPASSED. Any extra arguments go
#                                         to claude (e.g. `--resume`).
#   docker/feathers-dev.sh build          build (or rebuild) the image; uid/gid/user from the caller
#   docker/feathers-dev.sh up             start the container (idempotent; restarts a stopped one)
#   docker/feathers-dev.sh sh             a shell inside, in the repo
#   docker/feathers-dev.sh claude [args]  same as the no-argument form
#   docker/feathers-dev.sh down           stop and remove the container (the image stays)
#
# After editing the Dockerfile: `build` then `down` then the one command. After editing the mount
# list below: `down` then the one command (a container keeps the mounts it was created with).
#
# THE BOUNDARY IS THE MOUNT LIST BELOW, so read it as a security document and not as plumbing.
# Everything a session inside can touch on the host is one of these paths; everything else on the host
# is out of reach however a script inside is edited. What each one is for:
#
#   the repo         rw   the work. Worktrees live under it (worktrees/), build dirs too.
#   ~/.clap          rw   THE ONE PATH OUT: the host's Bitwig loads what lands here. This is the mount
#                         that carries the trust the frozen-script rule in CLAUDE.md used to carry —
#                         a session inside can still put a binary in front of Bitwig, and nothing else.
#   ~/.claude        rw   Claude Code's state: auth, settings, this project's memory, sessions —
#                         except settings.json, which is OVERLAID by a copy generated at `up` with
#                         blockReadsOutsideWorkingDirectories removed (see there for why).
#   ~/.claude.json   rw   Claude Code's user config (MCP servers, onboarding), a file beside the dir.
#   ~/.local         rw   the claude CLI binary and the static ccache binary live in ~/.local/bin.
#   ~/.cache/ccache  rw   the compile cache, at the host path so hits carry across the boundary.
#   ~/.code-index-feathers
#                    rw   jcodemunch's index of THIS repo, and nothing else: a store of its own (the
#                         repo's .mcp.json and the .git/hooks re-index both point CODE_INDEX_PATH at
#                         it) so that it could be mounted here. The general ~/.code-index holds cached
#                         source of every other repo jcodemunch has indexed on this host and stays
#                         UNMOUNTED by decision. Everything in this store is
#                         derived from the repo mount above, so it widens nothing. Without it the MCP
#                         server inside would build a throwaway index in the container's own fs, and
#                         merges made inside would leave the host's stale.
#   ~/Documents/Owlet/Feathers/Presets
#                    ro   the host preset folder, scanned at each release for presets
#                         tagged 'factory'. READ-ONLY on purpose: a session reads which presets carry the
#                         `factory` tag and copies them into the repo's presets/factory; nothing inside can
#                         change or delete the host's presets. Tests never use it (FEATHERS_PRESET_ROOT redirects).
#   ~/.gitconfig     ro   commit identity.
#   ~/.config/gh     ro   THE GITHUB CREDENTIAL, and the widest thing in this list — read the paragraph
#                         under "THE TOKEN" below before changing it. Mounted 2026-09-07 so a session
#                         inside can hold the integrator role: push to origin and drive the macOS CI.
#   ~/.config/git    ro   the GLOBAL ignore file. Without it, `.claude/settings.local.json` — the host's
#                         own permission rules, ignored only globally — shows as untracked inside, and
#                         one `git add -A` would commit it. MEASURED on the first `up`.
#   toolchain        ro   the parent of FEATHERS_TOOLCHAIN_DIR (.env.local), WHOLE: the cmake and ninja the
#                         HOST build dirs were configured with. Their build.ninja names the cmake by its
#                         RESOLVED path (toolchain/v22/linux-x86_64/..., `current` being a symlink), so
#                         mounting only `current` left `ninja -n -C build-install-on` saying "Re-running
#                         CMake" against a binary that did not exist inside (inside's capability check
#                         1b). With the whole tree mounted both spellings resolve; scripts/env.sh stays
#                         true; both sides drive the shared dirs with the same binaries.
#
# TWO THINGS A SESSION INSIDE MUST NEVER RUN, because the host's worktrees are NOT mounted and so look
# deleted from inside: `git worktree prune` (it would drop every host worktree's metadata — inside's
# `git worktree list` shows them all as "prunable"), and any cleanup that reasons from that listing.
#   cc-socks         rw   the peer sessions' UNIX sockets — sessions inside and outside keep talking.
#   /tmp/claude-1000 rw   the scratchpads, so result files written inside are readable outside.
#
# THE TOKEN, and what mounting it costs. It is mounted so that a session inside the container
# can take the integrator role.
# ~/.config/gh holds the OAuth credential `gh auth login` wrote for the account owlet-labs. It is NOT
# scoped to this repository: it is the account's token, so anything inside that can run `gh` can reach
# every repository and workflow that account can, not only owlet-labs/feathers. The mount is READ-ONLY,
# which stops the container from rewriting the host's login and nothing else — a read is all it takes to
# use a bearer token. Two consequences worth stating plainly rather than discovering later:
#   1. The frozen-script rule in CLAUDE.md exists because an allow rule is an allow rule on whatever the
#      script says tomorrow. The same reasoning applies here: this mount is a standing grant to every
#      agent that ever runs in this container, not to one session for one task.
#   2. It closes exactly two gaps, both measured empty before it: `command -v gh` (so no CI dispatch, no
#      job log, no artifact download) and the git credential helper the host's .gitconfig names,
#      `gh auth git-credential`, whose absence made a push fail with "could not read Username".
# THE NARROWER SHAPE, if this ever feels too wide: a fine-grained personal access token limited to
# owlet-labs/feathers with contents:write and actions:write, written to a file that only this container
# mounts, and GH_TOKEN pointed at it. That cannot be minted from a shell — it is a browser page — which
# is why the account token is what is mounted today. Revoking is `gh auth logout` on the host plus
# deleting the token in GitHub settings; a container restart is not needed to cut it off, since the
# mount is read at use time.
#
# NOT mounted, on purpose: the rest of $HOME, the old worktree location under ~/.config/superpowers
# (move those worktrees inside the repo first — CLAUDE.md says where), the display sockets (the venue
# inside is scripts/venue-xvfb.sh's own Xvfb; the GPU display :1 is host-only and stays that way).
#
# The network is the default bridge: FetchContent needs it at configure time (bgfx, clap-wrapper) and
# the claude CLI needs it always.
#
# Paths are the SAME STRING inside and out (same uid, same HOME, same mount points). That is not
# cosmetic: ccache keys on paths, the peer sockets are addressed by path, and a result file named in a
# message means the same file to a reader on either side.

set -euo pipefail

IMAGE=feathers-dev
NAME=feathers-dev
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "${HERE}/.." && pwd)"
ME_UID="$(id -u)"
ME_GID="$(id -g)"
ME_USER="$(id -un)"
ME_HOME="${HOME}"
# THE TOOLCHAIN MOUNT comes from the untracked `.env.local` at the repo root (see scripts/env.sh):
# FEATHERS_TOOLCHAIN_DIR names the toolchain's `current` link, and its PARENT is mounted whole.
# shellcheck source=/dev/null
[ -f "${REPO}/.env.local" ] && . "${REPO}/.env.local"
toolchain_mounts=()
if [ -n "${FEATHERS_TOOLCHAIN_DIR:-}" ]; then
  toolchain_root="$(dirname "${FEATHERS_TOOLCHAIN_DIR}")"
  toolchain_mounts=(-v "${toolchain_root}:${toolchain_root}:ro")
fi

cmd="${1:-claude}"
[ $# -gt 0 ] && shift

build_image() {
  docker build -t "${IMAGE}" \
    --build-arg "UID=${ME_UID}" --build-arg "GID=${ME_GID}" --build-arg "USER=${ME_USER}" \
    "${HERE}"
}

ensure_image() {
  if ! docker image inspect "${IMAGE}" >/dev/null 2>&1; then
    echo "${IMAGE}: no image yet, building (a few minutes the first time)"
    build_image
  fi
}

ensure_container() {
  if docker container inspect "${NAME}" >/dev/null 2>&1; then
    if [ "$(docker container inspect -f '{{.State.Running}}' "${NAME}")" != "true" ]; then
      docker start "${NAME}" >/dev/null
      echo "${NAME}: restarted"
    fi
    return 0
  fi
  "${BASH_SOURCE[0]}" up
}

case "${cmd}" in
  build)
    exec docker build -t "${IMAGE}" \
      --build-arg "UID=${ME_UID}" --build-arg "GID=${ME_GID}" --build-arg "USER=${ME_USER}" \
      "${HERE}"
    ;;

  up)
    ensure_image
    if docker container inspect "${NAME}" >/dev/null 2>&1; then
      docker start "${NAME}" >/dev/null
      echo "${NAME}: running (existing container)"
      exit 0
    fi
    mkdir -p "${ME_HOME}/.clap" "${ME_HOME}/.cache/ccache" "${ME_HOME}/.code-index-feathers" \
      "${ME_HOME}/Documents/Owlet/Feathers/Presets" "/tmp/claude-${ME_UID}"
    touch "${ME_HOME}/.claude.json"
    # THE INSIDE SETTINGS FILE. The host's ~/.claude/settings.json turns on
    # blockReadsOutsideWorkingDirectories, which bypass mode does not lift and which refuses any path
    # the checker cannot resolve (a relative path after a cd, a $VAR) however many directories are
    # added — measured three times on the first day. Inside, the container is the boundary and that
    # setting only makes a session unable to read its own container. So the container gets the host's
    # settings file with that one key removed, bind-mounted OVER ~/.claude/settings.json. Regenerated on
    # every `up` from the live host file, so other settings stay in step.
    mkdir -p "${ME_HOME}/.config/feathers-dev"
    jq 'del(.permissions.blockReadsOutsideWorkingDirectories)' "${ME_HOME}/.claude/settings.json" \
      > "${ME_HOME}/.config/feathers-dev/claude-settings.json"
    # ~/.claude REACHES THE CONTAINER AS A DIRECTORY AT A SIDE PATH, AND ~/.claude INSIDE IS A FARM OF
    # SYMLINKS INTO IT. Two measured failures produced this shape, and both were the same trap.
    #   1. A whole-directory mount of ~/.claude cannot be shadowed one file deep: the container read
    #      the host's real settings.json, block and all, despite the overlay (2026-09-03).
    #   2. Mounting each ENTRY individually fixed that and broke authentication: a file bind pins an
    #      INODE, and Claude Code replaces ~/.claude/.credentials.json on every token refresh via
    #      atomic rename. MEASURED 2026-09-04: host inode 36349824 at 09:09, container inode 36349825
    #      from 23:18 the night before — the container held an orphaned file with a stale token and
    #      could not log in. Every refresh would break it again, so a restart is a workaround, not a
    #      fix.
    # A SYMLINK IS RESOLVED AT OPEN TIME, so it follows a replaced file where a bind cannot; the side
    # directory is mounted as a DIRECTORY, so replacement inside it is invisible to the mount. The farm
    # is built at container start (below), and settings.json is a REAL FILE in the container's own
    # filesystem — sanitized, and writable inside without touching the host's.
    claude_mounts=(-v "${ME_HOME}/.claude:/host-claude")
    # THE PLUGIN'S LOGS (likes, pattern likes, CPU spikes), read-only. Bitwig is a flatpak, so the plugin's
    # XDG_DATA_HOME is the flatpak's data dir, not ~/.local/share — measured 2026-11-04, the logs were only
    # there. Mounted only if it exists: a missing bind source would be created as an empty root-owned dir.
    log_mounts=()
    bitwig_feathers="${ME_HOME}/.var/app/com.bitwig.BitwigStudio/data/feathers"
    if [ -d "${bitwig_feathers}" ]; then
      log_mounts=(-v "${bitwig_feathers}:${bitwig_feathers}:ro")
    fi
    docker run -d --init --name "${NAME}" \
      --hostname feathers-dev \
      -v "${REPO}:${REPO}" \
      -v "${ME_HOME}/.clap:${ME_HOME}/.clap" \
      "${claude_mounts[@]}" \
      -v "${ME_HOME}/.config/feathers-dev/claude-settings.json:/host-claude-settings.json:ro" \
      -v "${ME_HOME}/.claude.json:${ME_HOME}/.claude.json" \
      -v "${ME_HOME}/.local:${ME_HOME}/.local" \
      -v "${ME_HOME}/.cache/ccache:${ME_HOME}/.cache/ccache" \
      -v "${ME_HOME}/.code-index-feathers:${ME_HOME}/.code-index-feathers" \
      -v "${ME_HOME}/Documents/Owlet/Feathers/Presets:${ME_HOME}/Documents/Owlet/Feathers/Presets:ro" \
      "${log_mounts[@]}" \
      -v "${ME_HOME}/.gitconfig:${ME_HOME}/.gitconfig:ro" \
      -v "${ME_HOME}/.config/git:${ME_HOME}/.config/git:ro" \
      -v "${ME_HOME}/.config/gh:${ME_HOME}/.config/gh:ro" \
      "${toolchain_mounts[@]}" \
      -v "/run/user/${ME_UID}/cc-socks:/run/user/${ME_UID}/cc-socks" \
      -v "/tmp/claude-${ME_UID}:/tmp/claude-${ME_UID}" \
      -e "XDG_RUNTIME_DIR=/run/user/${ME_UID}" \
      -e "GH_NO_UPDATE_NOTIFIER=1" \
      -w "${REPO}" \
      "${IMAGE}" >/dev/null
    # THE SYMLINK FARM, built once the container is up: every entry of the host's ~/.claude reached
    # through a link that resolves at open time, and settings.json a real sanitized file. Rebuilt on
    # every `up`, so an entry the host gained since is picked up by a restart.
    docker exec "${NAME}" sh -c '
      set -e
      mkdir -p "$HOME/.claude"
      for entry in /host-claude/* /host-claude/.*; do
        base=$(basename "$entry")
        case "$base" in
          .|..|settings.json) continue ;;
        esac
        [ -e "$entry" ] || continue
        ln -sfn "$entry" "$HOME/.claude/$base"
      done
      cp /host-claude-settings.json "$HOME/.claude/settings.json"
    ' || echo "${NAME}: WARNING — the ~/.claude symlink farm did not build; auth and history will be missing"
    echo "${NAME}: started"
    ;;

  sh)
    exec docker exec -it -w "${REPO}" "${NAME}" bash
    ;;

  claude)
    # Permissions bypassed BECAUSE the container is the boundary. Do not run this form on the host.
    ensure_image
    ensure_container
    # ~/.claude/settings.json (mounted) sets blockReadsOutsideWorkingDirectories, which bypass mode
    # does not lift: without this, a session inside can read only the repo and the host's added
    # directories — "local files only", and the first thing it tripped on was
    # /usr/share/vulkan/icd.d/ (the venue's ICD check). And adding directories is NOT enough: with the
    # block on, a path the checker cannot resolve (`cd X && tail log`, `$VAR/file`) is refused however
    # many directories are added — the second thing it tripped on. So the setting itself is turned off
    # for the inside session: command-line settings outrank the user file. Inside, the container IS the
    # boundary; there is nothing here to protect from a read.
    # crossSessionInbound is loosened here too, on the command line, because a project or local
    # settings value for it can only TIGHTEN (a looser project/local value is ignored — measured: the
    # hold stayed with "accept" in settings.local.json); only user, managed or --settings can loosen.
    exec docker exec -it -w "${REPO}" "${NAME}" claude --dangerously-skip-permissions \
      --settings '{"permissions":{"blockReadsOutsideWorkingDirectories":false},"crossSessionInbound":"accept"}' "$@"
    ;;

  down)
    docker rm -f "${NAME}" >/dev/null && echo "${NAME}: removed"
    ;;

  *)
    sed -n '2,9p' "${BASH_SOURCE[0]}"
    exit 2
    ;;
esac
