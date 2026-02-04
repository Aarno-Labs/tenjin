# Troubleshooting Failed Provisioning

Sometimes provisioning fails for reasons that are outside of Tenjin's control,
especially on macOS.

This page lists some of the errors we've seen.

### `_stdio.h` not found

Error occurs during `goblint-cil` compilation:

```
...
# /opt/homebrew/Cellar/gcc/14.2.0_1/lib/gcc/current/gcc/aarch64-apple-darwin22/14/include-fixed/stdio.h:78:10: fatal error: _stdio.h: No such file or directory
#    78 | #include <_stdio.h>
#       |          ^~~~~~~~~~
# compilation terminated.
...
```

This is likely due to using Homebrew GCC 14 on macOS 15+.

Run `brew uninstall gcc && brew install gcc`

### `llvm-mc` chokes on `-mmacosx-version-min=15.0`

```
# (cd _build/default/src && /opt/homebrew/bin/gcc-15 -D_GNUCC machdep-ml.c -o machdep-ml.exe)
# llvm-mc: Unknown command line argument '-mmacosx-version-min=15.0'.  Try: 'llvm-mc --help'
# llvm-mc: Did you mean '--masm-hexfloats=15.0'?
```

Likely due to an older copy of `llvm-mc` being used by a newer copy of `clang`. Remove the older LLVM
from `$PATH`.


### `pkgconf` not found

Run `brew install pkgconf`

### `ld.lld: error: unable to find library -lgmp`

E.g.

```
File "CHJ/jchmuse/dune", line 2, characters 8-27:    
2 |   (name jCHXExtractFeatures)
            ^^^^^^^^^^^^^^^^^^^
ld.lld: error: unable to find library -lgmp
clang: error: linker command failed with exit code 1 (use -v to see invocation)
File "caml_startup", line 1:
Error: Error during linking (exit code 1)
```

It's unclear what conditions lead to this error (suspect it has something to do with
`opam` having re-installed dependencies per below) but removing `_local` and re-provisioning fixed the issue for me.


```
<><> Updating package repositories ><><><><><><><><><><><><><><><><><><><><><><>
[default] synchronised from https://opam.ocaml.org
Now run 'opam upgrade' to apply any package updates.
The following actions will be performed:
=== recompile 2 packages
  ↻ camlzip   1.13 [uses conf-zlib]
  ↻ conf-zlib 1    [upstream or system changes]

<><> Processing actions <><><><><><><><><><><><><><><><><><><><><><><><><><><><>
⬇ retrieved camlzip.1.13  (cached)
⊘ removed   camlzip.1.13
⊘ removed   conf-zlib.1
∗ installed conf-zlib.1
∗ installed camlzip.1.13
```

### `ld.lld: error: undefined symbol: __libc_csu_fini`

```
>>> referenced by /PATH/TO/tenjin/_local/xj-llvm/bin/../sysroot/usr/lib/x86_64-linux-gnu/Scrt1.o:(_start)
```

Seen in CI due to stale CodeHawk cached files. Try removing `_local` and re-provisioning.