# Tenjin User Documentation

N.B. Tenjin is still extremely early stage; these are mostly a placeholder.

## Translation

```
$ 10j translate \
    --codebase <PATH_TO_PROJECT_DIRECTORY_OR_LONE_C_FILE> \
    --resultsdir <WHATEVER>
```

The `resultsdir` will contain a series of codebase copies as it undergoes
various transformations during translation. The final version of the code
will be contained in a subdirectory called `final/`.

Currently, Tenjin requires either a project directory or a lone C file
(which should contain a `main` function, and should be compilable without
any preprocessor definitions or include flags). If providing a project
directory, it should contain a `CMakeLists.txt` file.
For other build systems, there is a `--buildcmd` flag which can, for example,
invoke `make` on a particular target, or using a particular Makefile.

Currently, Tenjin requires that the codebase being translated live in a
Git or jj repository, with a remote named `origin`. Information about the
state of the translated repository is embedded into generated JSON metadata
files in the `resultsdir`. This metadata is intended to allow Tenjin's
developers to easily replicate the results of translation. Eventually this
metadata will be made optional/opt-out.

Tenjin has preliminary support for producing Cargo workspaces for projects
which produce shared libraries for internal use. However, many of Tenjin's
automatic improvements are not yet applied to workspaces.

### Tips and Tricks

* When re-running the same translation, you'll probably want
    to pass `--reset-resultsdir`. Otherwise, you'll need to
    provide a fresh results directory every time.
* If you'd like to see the commands that Tenjin is running,
    set the `XJ_SHOW_CMDS=1` environment variable.
* For CMake projects, setting `XJ_CMAKE_PRESET=foo` will pass `--preset=foo`
    during configuration.

## Guidance

`10j translate` has an **optional** parameter called `--guidance`. It can be
either a JSON literal or a filepath, whose contents should be a JSON object.
The following keys are available:

* `vars_of_type` - a dict with keys as serialized Rust type strings. The
value for each key is either a variable specifier, or a list of specifiers.
A variable specifier is a string like `foo:bar`, which indicates the `bar`
parameter or local variable within the `foo` function. `*:bar` means every
such variable named `bar` in every function.
* `declspecs_of_type` - closely related, but applies to global variables.
* `vars_mut`: likewise, a dict mapping specifiers to boolean values
    indicating whether that declaration should be marked `mut` in Rust.
* `fn_return_type` - a dict mapping function names to the Rust type they should be made to / assumed to return
* `pod_types` - a list of type names which should be considered to
be plain old data, and thereby eligible for type-safe casting with
the `bytemuck` crate.
* `using_crates` - allows the human driver of translation to
specify third-party crates that should be used in the translation.
Currently restricted to a hard-coded list.
* `no_math_errno` - mostly for debugging/testing. Currently asserts
that no functions in the entire translated codebase make of use
`errno` in the math stdlib.
