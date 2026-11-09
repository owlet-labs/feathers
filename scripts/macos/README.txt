Feathers for macOS (universal: Apple silicon + Intel)
====================================================

This archive holds:

  feathers.clap          the Feathers synth (CLAP)
  feathers-chain.clap    the Feathers Chain (CLAP)
  feathers.vst3          the Feathers synth (VST3)
  feathers-chain.vst3    the Feathers Chain (VST3)
  install.command        the installer (a Terminal script)
  README.txt             this file

Any subset may be present; the installer installs what it finds and reports
what it skips.

INSTALL
-------

1. Unzip the archive (double-click it).
2. Right-click install.command and choose Open, then click Open again in the
   dialog that says it is from an unidentified developer.

   A plain double-click will NOT work the first time: macOS blocks it because
   the script was downloaded and is not signed by an Apple Developer ID.
   Right-click > Open is the one-time exception macOS offers for exactly this.

   If you would rather do it from Terminal:

       cd <the unzipped folder>
       xattr -d com.apple.quarantine install.command
       ./install.command

   If Terminal says "permission denied", the executable bit was lost in
   transit; run  chmod +x install.command  and try again.

3. Restart your DAW, or make it rescan its plugins. It will not see the new
   build until it does.

The script prints where each bundle went and the sha256 of each bundle's
executable. Keep that output if you are going to report a bug: it names the
exact binary the report is about.

WHAT THE SCRIPT DOES
--------------------

- Refuses to run on anything but macOS.
- Removes the com.apple.quarantine attribute from every bundle. Everything a
  browser downloads gets this attribute, and a quarantined plugin that is not
  notarised by Apple is refused when a DAW tries to load it, usually silently:
  the plugin simply does not show up.
- Copies each CLAP bundle into  ~/Library/Audio/Plug-Ins/CLAP/  and each VST3
  bundle into  ~/Library/Audio/Plug-Ins/VST3/  (creating them if needed),
  replacing any earlier copy of the same name.
- Prints the installed paths and hashes.

WHY NOT A PROPER .PKG INSTALLER
-------------------------------

A .pkg would be subject to exactly the same check. Gatekeeper judges a package
by its signature, and an unsigned or ad-hoc-signed package is refused with the
same "unidentified developer" dialog as this script, and needs the same
right-click > Open. Only a package signed with an Apple Developer ID
Installer certificate and notarised by Apple installs without it. Feathers
has no Developer ID, so a .pkg would add a second thing to bypass and remove
nothing. Each bundle's executable carries the ad-hoc signature Apple's linker
puts on every Apple silicon binary (the executable only, no bundle seal: the
same state the .vst3 is in); CI signs nothing itself. That is what lets Apple
silicon load it at all, but ad hoc is not a Developer ID and does not satisfy
Gatekeeper.

If macOS still says a bundle is damaged or cannot be opened after installing,
run this in Terminal and rescan:

    xattr -dr com.apple.quarantine ~/Library/Audio/Plug-Ins/CLAP/feathers.clap
    xattr -dr com.apple.quarantine ~/Library/Audio/Plug-Ins/CLAP/feathers-chain.clap
    xattr -dr com.apple.quarantine ~/Library/Audio/Plug-Ins/VST3/feathers.vst3
    xattr -dr com.apple.quarantine ~/Library/Audio/Plug-Ins/VST3/feathers-chain.vst3

IF YOU COPY A BUNDLE BY HAND
----------------------------

The plain CI artifact (feathers-macos-universal2) holds the same bundles as
this archive, plus the .vst3. A bundle dragged out of it into
~/Library/Audio/Plug-Ins/CLAP keeps its quarantine, and the DAW will not load
it: that is what "the .clap is broken" looked like on the first Mac build.
The installer strips the attribute for you; for a hand copy, strip it
yourself with the two commands above, then rescan.

WHAT IS SIGNED, AND WHAT IS NOT — DO NOT "FIX" THIS
---------------------------------------------------

Each bundle is shipped exactly as the build left it (measured in CI on
2026-09-08):

  - the arm64 slice of the executable carries the ad-hoc signature Apple's
    linker puts on every arm64 binary (flags: adhoc, linker-signed). That
    is what an Apple silicon Mac requires, and it is the same state the
    .vst3 is in, which is the one that loaded from a hand copy;
  - the x86_64 slice is NOT signed. Apple's linker signs arm64 only, and
    Intel Macs have no signing requirement, so an Intel host loads it as is;
  - there is no bundle seal (no Contents/_CodeSignature). `codesign --verify`
    on the BUNDLE therefore says "code object is not signed at all"; that is
    the bundle-level answer for an unsealed bundle, not a statement about the
    executable, which verifies on its own.

Signing the bundle ourselves (`codesign --deep --sign -`) adds a seal that
satisfies nothing without a Developer ID and was the one difference from
the .vst3 when a hand-copied .clap failed to load. Leave the build's own
state alone.

REQUIREMENTS
------------

macOS 15 (Sequoia) or newer. The build targets 15.0 and will not load on
earlier releases.
