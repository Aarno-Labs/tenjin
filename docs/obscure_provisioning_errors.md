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
