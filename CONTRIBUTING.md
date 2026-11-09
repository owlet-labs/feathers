# Contributing to Feathers

Thanks for your interest. Feathers is a young project and help is welcome: bug reports, sound
design, documentation, tests and code.

## Start with an issue

Please [open an issue](https://github.com/owlet-labs/feathers/issues) before you start anything
bigger than a small fix. Tell us what you would like to change and why. Feathers moves quickly, and a
short conversation first saves you from building on something that is about to change.

Good bug reports say which host and OS you used, which plugin format (CLAP, VST3 or standalone), what
you did, what you expected, and what happened. A preset or a short recording helps a lot.

## Making a change

1. Fork the repository and make a branch from the default branch. One topic per branch.
2. Build and test as described in the [README](README.md#building).
3. Keep the change focused. A pull request that does one thing is much easier to review.
4. Open a pull request and fill in the template. Link the issue it addresses.

### Code style

- Tabs for indentation. C++20.
- New source files start with the project's GPL header (copy it from any file in `src/`), with the
  current year and the line "This file is part of Feathers."
- Comments explain *why*, not *what*. If a number came from a measurement, say so.
- No memory allocation on the audio thread once the plugin is activated.

### Tests

Tests use [doctest](https://github.com/doctest/doctest) and run under `ctest`. In brief, the house
conventions are:

- **Every behaviour change comes with a test.** A bug fix comes with a test that fails without the fix.
- **Check that the test can fail.** Before you trust a passing test, break the code on purpose (a
  "known-bad") and confirm the test goes red. Then restore the code.
- **Assert the value, not a difference.** "The output changed" passes whenever anything changed. If
  the claim is "this value is X", read X, and add a test-only accessor if you need one.
- **Take expected values from the specification** (the CLAP spec, a paper, a measurement), not from
  what the code currently produces.
- **Run a single case by name** with `scripts/run-cases.sh <binary> "<case name>"`. Case names often
  contain commas, which doctest's own `--test-case=` filter splits on.
- Saved state is keyed. If you add, rename or change the meaning of a parameter, run
  `test_state_ledger` and follow what it tells you.

Run the full suite before you open the pull request:

```
ctest --test-dir build --output-on-failure
```

## Contributor licence

Feathers is licensed under the GNU General Public License, version 3 or later. You keep the copyright
in what you write.

To keep the project's licensing flexible in the future, we ask every contributor to agree to the short
[Contributor Licence Agreement](CLA.md). In plain words, it says:

- you wrote the contribution, or have the right to submit it;
- you give Owlet Records a broad licence to use it, including the right to release it under other
  licences in addition to the GPL (for example, to add a licence exception for plugins);
- you give a matching patent licence, so nobody can later use a patent on your contribution against
  people using Feathers;
- the contribution comes with no warranty.

Please read [CLA.md](CLA.md) itself; this summary is not a substitute for it. You agree by ticking
the box in the pull-request template, or by commenting "I have read CLA.md and I agree to it" on your
pull request. You only need to agree once; it covers all your later contributions.

If you are contributing on behalf of an employer, please check that you are allowed to, and say so in
the pull request.

## Code of conduct

Be kind and assume good faith. We are all here because we like making sounds.
