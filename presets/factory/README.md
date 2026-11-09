# Factory presets

Every `.fpreset` under this folder is embedded into `feathers.clap` and `feathers-chain.clap` at build
time and installed into the user's preset folder when a preset browser opens, **only when nothing is
there already**: a copy the user has edited is never overwritten.

The layout mirrors the user's preset root (`~/Documents/Owlet/Feathers/Presets`):

    Feathers/                      the synth's bank
        Pads/Glass.fpreset         a category is a folder
    Devices/
        SKLITTER/                  one device's bank, named by its tag (sanitised)
            Stutters/Tape Chop.fpreset

To add one: save it from the plugin, tag it `factory` in the browser, and run
`scripts/collect-factory-presets.sh` (before tagging a release; it reads your preset root, never writes
it, and copies the tagged files here at the same path). Or copy a file here by hand at the path it
should install to. Then rebuild. The file ships byte for byte, so its tags and category travel in its
metadata. Anything that is not `.fpreset`, and anything hidden, is ignored.

A bank without an `Init.fpreset` here gets the Init the plugin computes from its defaults. A file named
`Init.fpreset` at a bank's top level replaces it on a fresh machine (it is installed first):
`Feathers/Init.fpreset` is the tagged `init` collected on 2026-09-27. The collect script lands a tagged `init` in
any case as `Init.fpreset`, and refuses any other pair of paths that differ only by case (one file on macOS
and Windows), naming both. See `src/plugin/preset_store.h`.

**The pre-push hook** (`scripts/hooks/pre-push`) runs `collect-factory-presets.sh --check` and refuses a
push while this folder is out of date with the tagged presets. On a machine without the preset root it
passes silently. Install it once from the main workspace:

    ln -s ../../scripts/hooks/pre-push .git/hooks/pre-push

(An existing `.git/hooks/pre-push` is renamed `pre-push.local` first; the hook runs it after its check.)

## The `test` tag

A preset tagged `test` (all of them under `Feathers/Test/`) is a **stress patch**, not a sound. Each is one of the patches
the performance survey built, named for what it stresses: one device added to
Init, a Rack in each mode, a dense chain, 16 voices, a stack, or a factory preset with a device removed. They ship so that
anyone can load the patch a number in the survey came from and hear or profile it in a real host. They are written by
`tests/bench_patch --plan tests/bench_patch_plans/save-test-presets.plan --rounds 1` through the browser's own save road
(`FEATHERS_PRESET_ROOT` pointed at a scratch folder, then the files copied into `Feathers/Test/`), not collected from a
preset root, so `collect-factory-presets.sh` neither copies them nor calls them stale. Host automation, note counts and a
sidechain are not patch state, so a survey row that depends on one of those (Pos X automated, a Vocode's sidechain)
needs the host to supply it. The case "every test-tagged factory preset loads into a fresh instance and renders a block"
(test_clap_render) loads each one.
