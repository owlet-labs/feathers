# Feathers development container.
#
# WHY THIS EXISTS: agents working in this repo prompt for permission on every
# command whose paths the checker cannot read as literal absolutes, and the alternative — allow rules on
# scripts any agent can edit — is a trust hole (CLAUDE.md, "Allow-listed scripts are a trust boundary").
# A container bounds what a command can touch STRUCTURALLY, so inside it every session runs with
# permissions bypassed and the prompts go to zero. The boundary moves from the checker to the mounts:
# see docker/feathers-dev.sh for exactly what is mounted and why `~/.clap` is the one path out.
#
# WHAT IT MATCHES: the host is Pop!_OS 22.04 on glibc 2.35, and the .clap built here is loaded by the
# host's Bitwig, so the base is Ubuntu 22.04 and the compiler is g++-14 from the same PPA the host uses.
# cmake comes from Kitware's apt repo because jammy ships 3.22 and the project floor is 3.24. The venue
# packages (Xvfb, lavapipe) are what scripts/venue-xvfb.sh needs — software rasterisation, no GPU, same
# as on the host's private display. The package list for the build is CI's (.github/workflows/build.yml),
# which is the list known to build this tree from nothing.
#
# WHAT THE TESTS AND SCRIPTS SHELL OUT TO, found by grepping them rather than by guessing: the windowed
# suite captures with ImageMagick's `import -window` and diffs with `compare` (MEASURED on the first
# display run inside: without imagemagick every capture came back invalid — `REQUIRE(before.valid())`
# failed in three cases, and nothing named the missing binary); the scripts use `xset` (x11-xserver-utils,
# NOT x11-utils), `xdpyinfo` (x11-utils), `timeout`, `python3`, `md5sum`/`sha256sum`. If a script grows
# a new dependency, it goes here, and the check is `grep -hoE '\b(tool)\b' scripts/*.sh tests/*.cpp`.
#
# The user inside is the host user by uid/gid and HOME, so every path is the SAME string inside and out
# — that is what lets ccache hit across the boundary and lets the peer sockets be shared.
#
# Build with: docker/feathers-dev.sh build   (never by hand — the uid/gid args must match the host)

FROM ubuntu:22.04

ARG UID=1000
ARG GID=1000
ARG USER=dev

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      ca-certificates curl gnupg software-properties-common \
 && add-apt-repository -y ppa:ubuntu-toolchain-r/test \
 && curl -fsSL https://apt.kitware.com/keys/kitware-archive-latest.asc \
      | gpg --dearmor -o /usr/share/keyrings/kitware-archive-keyring.gpg \
 && echo 'deb [signed-by=/usr/share/keyrings/kitware-archive-keyring.gpg] https://apt.kitware.com/ubuntu/ jammy main' \
      > /etc/apt/sources.list.d/kitware.list \
 # THE GITHUB CLI, for the integrator role (2026-09-07). A session in here has to dispatch the macOS
 # workflow, read a failing job's log and download the installer artifact; without `gh` it could do
 # none of the three (measured: `command -v gh` empty, and the git credential helper the host's
 # .gitconfig names is `gh auth git-credential`, so pushes failed with "could not read Username").
 # The BINARY is what this line installs; the CREDENTIAL is mounted at run time, read-only, by
 # feathers-dev.sh — read the paragraph there before deciding this is fine.
 && curl -fsSL https://cli.github.com/packages/githubcli-archive-keyring.gpg \
      -o /usr/share/keyrings/githubcli-archive-keyring.gpg \
 && chmod go+r /usr/share/keyrings/githubcli-archive-keyring.gpg \
 && echo 'deb [arch=amd64 signed-by=/usr/share/keyrings/githubcli-archive-keyring.gpg] https://cli.github.com/packages stable main' \
      > /etc/apt/sources.list.d/github-cli.list \
 && apt-get update \
 && apt-get install -y --no-install-recommends \
      gcc-14 g++-14 cmake ninja-build ccache pkg-config git git-lfs sudo gh jq \
      clang-15 \
      libasound2-dev libx11-dev libxrandr-dev libgl1-mesa-dev libglu1-mesa-dev libegl1-mesa-dev \
      libsm-dev libice-dev libxtst-dev \
      libfontconfig-dev libxcb-present-dev libx11-xcb-dev \
      libxcb-randr0-dev libxcb-xfixes0-dev libxcb-sync-dev libxcb-render0-dev \
      xvfb x11-utils x11-xserver-utils libvulkan1 mesa-vulkan-drivers vulkan-tools \
      imagemagick python3 \
      binutils file procps psmisc bc xxd less nano \
 && rm -rf /var/lib/apt/lists/* \
 # jammy's mesa names the lavapipe ICD lvp_icd.x86_64.json; the host (and scripts/venue-xvfb.sh, which
 # checks the file BY PATH) know it as lvp_icd.json. MEASURED on the first `down`+`up`: a symlink made by
 # hand inside the container went with the container, and the venue refused with "ICD is missing".
 && ln -s lvp_icd.x86_64.json /usr/share/vulkan/icd.d/lvp_icd.json

# **THE SECOND -dev LINE IS visage v1.0.0's** (2026-11-04).
# Its graphics library does `find_package(Fontconfig REQUIRED)` and its windowing FATAL_ERRORs without
# xcb/present.h; present.h includes randr/xfixes/sync, randr includes render, and Xlib-xcb.h is
# libx11-xcb-dev's. MEASURED: configure and compile stopped on each in turn until all seven were there.
# The bgfx-era pin needs none of them, so they are harmless before the port lands.

# **libsm-dev, libice-dev AND libxtst-dev ARE ONE CLASS, NOT THREE PACKAGES — read them that way, and
# the check below is how to know whether there is a fourth.** None is in CI's list, because CI
# configures from scratch and links what it finds.
# The HOST build dirs were configured on a host that HAS these, so their `build.ninja` names
# /usr/lib/x86_64-linux-gnu/libSM.so, libICE.so and libXtst.so as literal link inputs — and ninja
# refuses a named input it cannot find, whatever the compiler could have done. So the rule is: a
# library the host has and the image lacks breaks the SHARED dirs from inside while a fresh configure
# inside never asks for it, which is why this cannot be found by building in a container-local dir.
#
# Three instances, each found the same way — by a build inside that stopped on a missing .so:
#   libSM/libICE  a dry run of build-install-on (inside's post-rebuild check 1)
#   libXtst       `ninja -C build`, needed by `alt-probe`, during the first merge run from inside
#                 (2026-09-08, the harm/expand-lane merge). Installed live to finish that merge, which
#                 is ephemeral — it dies with the container, and this line is what makes it stick.
#
# THE GENERAL CHECK, so a fourth is found by asking rather than by a failed link:
#
#   grep -ohE '/usr/lib/x86_64-linux-gnu/[^ ]*\.so' build/build.ninja build-install-on/build.ninja \
#     | sort -u | while read -r p; do test -e "$p" || echo "MISSING: $p"; done
#
# RUN 2026-09-08 with this line in place: ten inputs across the two dirs — libGLU, libGLX, libOpenGL,
# libICE, libSM, libX11, libXext, libXrandr, libXtst, libasound — and every one resolves inside. So
# the list above is complete as of today, and the check is how to know that again rather than assume it.

# The host user, by number, so the bind mounts are owned by the same uid inside and out. ~/.cache is
# created here OWNED BY THE USER: docker creates a missing mount parent as root, and with only
# ~/.cache/ccache mounted, lavapipe could not write ~/.cache/mesa_shader_cache and recompiled its
# shaders on every launch ("Permission denied", inside's check 4).
RUN groupadd -g "${GID}" "${USER}" \
 && useradd -m -u "${UID}" -g "${GID}" -s /bin/bash "${USER}" \
 && mkdir -p "/home/${USER}/.cache/ccache" \
 && chown -R "${UID}:${GID}" "/home/${USER}/.cache" \
 && echo "${USER} ALL=(ALL) NOPASSWD:ALL" > "/etc/sudoers.d/${USER}" \
 && chmod 0440 "/etc/sudoers.d/${USER}"

# scripts/env.sh sets these too; set here so a bare `cmake` in the container picks the right compiler.
ENV CC=gcc-14 CXX=g++-14
# CLANG FOR scripts/clang-preflight.sh ONLY (2026-10-06: catch locally what macOS CI rejects). The build
# stays g++-14; the preflight syntax-checks our TUs under clang with -Wall -Wextra and the arm64 SIMDe road
# (-DFEATHERS_FORCE_SIMDE). clang-15 is jammy's newest and the nearest to the macOS runner's Apple clang;
# it uses g++-14's libstdc++. Two failures it would have caught that day: an unused lambda capture and a
# target missing SIMDe's include path.
ENV CLANG=/usr/bin/clang++-15
# ccache is the host's static binary under ~/.local/bin (mounted); its cache dir is mounted at the
# host path, and CCACHE_BASEDIR makes worktree paths relative so hits carry across worktrees.
ENV FEATHERS_USE_CCACHE=1
ENV CCACHE_BASEDIR=/home/${USER}
# ccache's default compiler check is the compiler's MTIME, and the container's g++-14 is a different
# file from the host's even at the same package version — so a fresh worktree inside hit 2 of 492
# objects from the host's cache (inside's capability check 2; an in-place rebuild hit 1/1, so the cache
# itself worked). Hashing the compiler's CONTENT lets identical bytes share a cache across the boundary
# and costs nothing when they differ.
ENV CCACHE_COMPILERCHECK=content

USER ${USER}
ENV HOME=/home/${USER}
ENV PATH=/home/${USER}/.local/bin:${PATH}
WORKDIR /home/${USER}/src/feathers
CMD ["sleep", "infinity"]
