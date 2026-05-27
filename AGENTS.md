- Instead of running `clang`, `pytest`, or `opam` directly, prefix the invocation with `./cli/10j`
  (adjust the path if running from a subdirectory).
- Instead of running other commands like `make`, `cmake`, or `ninja` via `10j`, use `./cli/10j exec make ...`, etc.
- Most scripts within `cli/` are meant to be run indirectly via `10j`
- After any change to Python code, run `./cli/10j check-py`
- After any change to Rust code, run `./cli/10j check-rs && 10j test-unit-rs`. This should take about 15 seconds to run.
- When finished with a task, run `./cli/10j pytest tests -n auto`. This takes about 60 seconds to run.
- Results from `10j pytest` runs can be found in `/tmp/pytest-of-$USER/`
- The `translation_metadata.json` file within an output resultsdir may have stdout/stderr captured from subcommands executed by Tenjin.
- When making changes to clang tools like `xj-prepare-findfntprdecls`,
  `xj-prepare-pointertransform`, etc, you can build them by running
  `10j build-star` and run them from the built executables in
  `_local/_build_findfnptrdecls`, etc.