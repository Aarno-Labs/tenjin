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
(which should be compilable without
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

## Coverage

Tenjin provides support for collecting and manipulating coverage
data for translated code---both (output) Rust and (input) C. This
support comes in the form of two subcommands: `10j covset-gen` and
`10j covset-eval`.

### `10j covset-gen`

After performing a translation with a command like

```sh
10j translate --codebase CODEBASE \
              --resultsdir RESDIR
```

one can run the C code and generate coverage:

```sh
10j covset-gen --codebase CODEBASE \
               --resultsdir RESDIR \
               --target EXENAME \
               --output WHATEVER.json \
               ...args for target binary...
```

Notes:

- The options may be passed in any order.
- The `--target` option is only needed for codebases that build
  multiple binaries. The target name may depend on Tenjin implementation
  details; if the name provided cannot be found, Tenjin will report
  which binaries it considered.
- If the target binary needs to be passed a conflicting argument,
precede that target's args with a lone double-dash (`--`).
- To generate a HTML coverage file next to the JSON, pass `--html`.
- To exercise the generated Rust code instead of the input C,
  pass `--rust`.
- The output JSON file will be restricted to the directly translated
  C or Rust code. However, the HTML report will include code from
  imported crates as well.

The output JSON file is in Tenjin's "covset" format.
It can be viewed and manipulated with the companion subcommand,
`10j covset-eval`.

### `10j covset-eval`

A covset file contains a bitmap for a particular codebase.

The `10j covset-eval` subcommand evaluates s-expressions consisting
of unary and binary set operators (`negate`, `union`, `intersection`, `difference`, `symmetric_diff`) over covset files.
There is also a `show` primitive, for viewing the contents of the
codebase annotated with the computed covset data, and `cat` for
emitting the raw underlying (or computed) JSON.

The covset file contains machine-specific paths, but also SHA256 hashes
of file contents, so they could (in a future Tenjin version) be
automatically matched to files on other machines.

### Coverage Demo

```sh
$ cat > xj_covset_demo.c <<EOF
  void puts(const char*);
  int main(int argc, char** argv) {
    puts("first line, always\n");

    if (argc > 2) {
      // oho!
      puts("second line..."
           "sometimes\n");
    }
    return 0;
  }
EOF

$ 10j translate --codebase xj_covset_demo.c --resultsdir xj-cdr
...

$ 10j covset-gen --codebase xj_covset_demo.c --resultsdir xj-cdr \
               --target xj_covset_demo.exe --output x.json
first line, always

$ 10j covset-eval '(show x.json)'
----------------------------------------
File: /PATH/TO/xj-cdr/c_02_build_coverage/xj_covset_demo.c
----------------------------------------
    void puts(const char*);
+   int main(int argc, char** argv) {
+     puts("first line, always\n");
  
+     if (argc > 2) {
  	    // oho!
-       puts("second line:"
-            "sometimes\n");
-     }
+     return 0;
+   }

========================================
Total covered lines: 5 / 8 = 62.50%

$ 10j covset-eval '(show (negate x.json))'
----------------------------------------
File: /PATH/TO/xj-cdr/c_02_build_coverage/xj_covset_demo.c
----------------------------------------
    void puts(const char*);
-   int main(int argc, char** argv) {
-     puts("first line, always\n");
  
-     if (argc > 2) {
  	    // oho!
+       puts("second line:"
+            "sometimes\n");
+     }
-     return 0;
-   }

========================================
Total covered lines: 6 / 8 = 75.00%

```

The file contains 11 total lines, but only 8 of them are considered
to be potentially executable by LLVM. In the first example, we see
five lines that were executed: main, the first puts, the comparison,
the return statement, and (according to LLVM, at least) the final curly brace.
There are three lines that are considered executable-but-not-executed,
and three that could not be covered by any execution.

When we inspect the negated covset, only the three uncovered lines are
printed as covered, but the "Total covered lines" indicates that all six
uncovered lines from before are now considered covered --- even the lines
for which printing was suppressed due to being considered un-executable.

We can then re-run our program to get a new coverage set, and manipulate them:

```sh
$ 10j covset-gen --codebase xj_covset_demo.c --resultsdir xj-cdr --output x2.json --target xj_covset_demo.exe one two three
first line, always

second line:sometimes

$ 10j covset-eval '(show (difference x2.json x.json))'
----------------------------------------
File: /PATH/TO/xj-cdr/c_02_build_coverage/xj_covset_demo.c
----------------------------------------
    void puts(const char*);
-   int main(int argc, char** argv) {
-     puts("first line, always\n");
  
-     if (argc > 2) {
  	    // oho!
+       puts("second line:"
+            "sometimes\n");
+     }
-     return 0;
-   }

========================================
Total covered lines: 3 / 8 = 37.50%
```

The total indicates that only three lines were covered by the
second execution which were not covered by the first.

We can then check the coverage of the associated Rust code:

```sh
$ 10j covset-gen --rust --codebase xj_covset_demo.c --resultsdir xj-cdr --output xr.json --target xj_covset_demo_nolines

$ 10j covset-eval '(show xr.json)'
----------------------------------------
File: /PATH/TO/xj-cdr/final/src/xj_covset_demo_nolines.rs
----------------------------------------
  #![allow(
      dead_code,
      non_camel_case_types,
      non_snake_case,
      non_upper_case_globals,
      unused_assignments,
      unused_mut
  )]
  use ::std::process::ExitCode;
  #[allow(unused_imports)]
  use ::tenjinized::*;
  extern "C" {
      fn puts(_: *const ::core::ffi::c_char);
  }
+ unsafe fn main_0(
+     mut argc: ::core::ffi::c_int,
+     mut argv: *mut *mut ::core::ffi::c_char,
+ ) -> ::core::ffi::c_int {
+     puts(b"first line, always\n\0" as *const u8 as *const ::core::ffi::c_char);
+     if argc > 2 {
-         puts(b"second line:sometimes\n\0" as *const u8 as *const ::core::ffi::c_char);
+     }
+     0
+ }
+ pub fn main() -> ExitCode {
+     let mut args: Vec<*mut ::core::ffi::c_char> = Vec::new();
+     for arg in ::std::env::args() {
+         args.push(
+             ::std::ffi::CString::new(arg)
+                 .expect("Failed to convert argument into CString.")
+                 .into_raw(),
+         );
+     }
+     args.push(::core::ptr::null_mut());
      unsafe {
+         ExitCode::from(main_0(
+             (args.len() - 1) as ::core::ffi::c_int,
+             args.as_mut_ptr() as *mut *mut ::core::ffi::c_char,
+         ) as u8)
      }
+ }

========================================
Total covered lines: 24 / 25 = 96.00%
```

# Error Cases

* If you see `gcc: error: unrecognized command-line option ‘-fcoverage-mapping’`
  the project you're trying to translate is using GCC but we require Clang.

