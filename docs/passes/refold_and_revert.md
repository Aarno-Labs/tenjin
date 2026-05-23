# Refolding and Revert Restoration

Origin: *LLM-generated*, *human-edited*

## Where

- [cli/c_refact.py](/cli/c_refact.py) — `refold_build`, `ConsolidationRevert`,
  `ConsolidationRevertContext`, `restore_dropped_consolidation_reverts`.
- [cli/translation_preparation.py](/cli/translation_preparation.py) —
  `prep_pre_refold_consolidation`, `prep_refold_preprocessor`.
- [`clang-refold`](https://github.com/Aarno-Labs/llvm-project/tree/tenjin-21.1.8%2Brefold/clang-tools-extra/clang-refold)
  — `--pp`, `--pp-mod`, `--refold-map`, `--out`, `--emit-edit-map`.

## What

Preparation passes rewrite the *preprocessed* `.i` form of each translation
unit. That's convenient (macros are already expanded, conditional compilation
is settled) but unfortunate (every `#include`'d header is splatted inline, so
edits land in TU files instead of the source headers they came from). Two
coupled steps fix this:

1. **Pre-refold consolidation** moves the modifications from the `.i` back
   into the `.h` they originated in, restoring the `.i` to match the
   unmodified preprocessor output in those regions. The pass runs in
   `prep_pre_refold_consolidation`. If we do not do this, modifications
   to declarations originating from header files will inhibit refolding
   of the corresponding `#include`. We optimistically assume that nothing
   else will inhibit refolding of the `.h` file's region.

2. **Refolding** runs `clang-refold` against the `(modified .i, unmodified .i,
   refold-map)` triple to reconstruct a `.c` that re-introduces `#include`
   directives wherever the modified `.i` looks like the original include
   expansion. Where refolding can't produce an `#include`, it falls back to
   leaving the expanded content in place (with `#line` directives). The pass
   runs in `prep_refold_preprocessor`.

When our assumption from step 1 was wrong, and refolding falls back to expansion
for a consolidated `.h` file's region, the `.c` ends up with the *unmodified*
expanded content, the modified `.h` sits unused, and the modification has been
silently dropped. So we then have an additional step to fix this problem.
`restore_dropped_consolidation_reverts` is the post-refold pass that detects
those drops and splices the modification back into the residual `.c`. It uses
the byte-range mapping emitted by `clang-refold --emit-edit-map` to anchor
where to look.

## Worked example

### Step 0 — original sources

`foo.h`:

```c
#ifndef FOO_H
#define FOO_H

extern int g_total;

static inline int square(int n) {
    return n * n;
}

#endif
```

`main.c`:

```c
#include "foo.h"
#include <stdio.h>

int g_total = 0;

int main(void) {
    g_total = square(7);
    printf("%d\n", g_total);
    return 0;
}
```

### Step 1 — preprocessing

`clang-refold` needs the preprocessor output with no `#line` markers and an
accompanying *refold map* JSON that records the preprocessor's view of the
source structure. The Tenjin invocation that produces these is in
`c_refact.preprocess_build`; `clang-refold`'s `--refold-map` flag (read at
refold time) and the matching `--refold-map=` argument on the preprocessor
invocation pair them up.

For exposition we use a shrunken `main.nolines.i` that omits system header
content from `<stdio.h>`:

```c
extern int g_total;
static inline int square(int n) {
    return n * n;
}
int g_total = 0;
int main(void) {
    g_total = square(7);
    printf("%d\n", g_total);
    return 0;
}
```

A copy of this file is kept as `main.nolines.unmodified.i`; that's the
"original" the refolder will compare against later.

### Step 2 — preparation passes modify `main.nolines.i`

Two illustrative passes run before consolidation. The exact mechanics aren't
important here; what matters is that they introduce edits relative to
`unmodified.i`.

**Pass A: static-inline uniquification.** Suffixes static-inline names so
multi-TU programs don't collide. `square` becomes `square_xjtr_0` everywhere
in the TU, including its definition (which sits in the inlined `foo.h`
region).

**Pass B: mutable-global localization.** Rewrites every *use* of the
escaping mutable global `g_total` to go through a context pointer:
`g_total` (as an lvalue or rvalue inside a function body) becomes
`xjg->g_total`. Declarations and definitions are left alone.

After both passes, `main.nolines.i`:

```c
extern int g_total;
static inline int square_xjtr_0(int n) {
    return n * n;
}
int g_total = 0;
int main(void) {
    xjg->g_total = square_xjtr_0(7);
    printf("%d\n", xjg->g_total);
    return 0;
}
```

The differences from `unmodified.i`:

- `square` → `square_xjtr_0` (3 occurrences: definition + body call site name
  + call site at use)
- `g_total` → `xjg->g_total` (2 occurrences inside `main`)

### Step 3 — pre-refold consolidation

`prep_pre_refold_consolidation` finds top-level declarations in the modified
`.i` that originated in a header and were edited by earlier passes. For each
one it asks: is the modification identical across every TU that contains it?
For single-TU projects like this one that's trivially true; for multi-TU
projects it filters out divergent modifications.

When a modification is consolidatable, the pass:

1. **Reverts** the modified region in the `.i` back to the unmodified
   expansion (so refolding has a chance to fold it).
2. **Updates** the originating `.h` with the modified text (so refolding's
   `#include` brings the modification through).

In our example, `square_xjtr_0`'s definition is a header-origin region that
was modified identically across TUs (there's only one). The pass produces:

`main.nolines.i` after consolidation:

```c
extern int g_total;
static inline int square(int n) {
    return n * n;
}
int g_total = 0;
int main(void) {
    xjg->g_total = square_xjtr_0(7);
    printf("%d\n", xjg->g_total);
    return 0;
}
```

`foo.h` after consolidation:

```c
#ifndef FOO_H
#define FOO_H

extern int g_total;

static inline int square_xjtr_0(int n) {
    return n * n;
}

#endif
```

The pass also records a `ConsolidationRevert` for the reverted region:

- `pre_rewrite_i_start`, `pre_rewrite_i_length` — where the modified text
  *was* in the `.i` at consolidation time (before the BatchingRewriter
  applied any rewrites).
- `modified_version` — `"static inline int square_xjtr_0(int n) {\n    return n * n;\n}"`
- `expanded_header_version` — `"static inline int square(int n) {\n    return n * n;\n}"`
  (same byte sequence the unmodified `.i` has there).
- `original_header_version` — the `.h` form (here identical to
  `expanded_header_version` because there are no macros; with macros the two
  would differ).

The full set of rewrites the BatchingRewriter applied to this `.i` is
captured alongside the revert list as `all_i_rewrites`. We'll see in Step 6
why we need every rewrite, not just the revert itself.

Notice the calls inside `main` are *not* consolidated:

- `xjg->g_total` is a use of a global, not a header-origin declaration —
  outside the consolidation pass's scope.
- `square_xjtr_0(7)` is a call site whose declaration was just reverted; the
  call still names the suffixed function. After refolding succeeds, the
  modified `square_xjtr_0` reaches the `.c` via `#include "foo.h"` and the
  call resolves. After refolding falls back to expansion, the call is
  dangling — which is exactly the case Step 7 catches.

### Step 4 — refolding (the happy path)

`clang-refold` compares modified `.i` to unmodified `.i`. Wherever a span of
the modified `.i` matches what `#include "foo.h"` (or any other recorded
include) would expand to, it tries to replace that span with the `#include`
in the `.c` output. Where it can't, it leaves the expansion in place with
`#line` directives.

In the happy path, refolding succeeds for our `square` region. `main.c`:

```c
#include "foo.h"
#include <stdio.h>

int g_total = 0;
int main(void) {
    xjg->g_total = square_xjtr_0(7);
    printf("%d\n", xjg->g_total);
    return 0;
}
```

`#include "foo.h"` pulls in the modified `square_xjtr_0`. The call resolves.
Compilation succeeds.

### Step 5 — refolding (the dropped-edit path)

Suppose the surrounding `xjg->g_total` edits prevent the refolder from
folding `square`'s region back into `#include "foo.h"` (in real Tenjin runs,
the surrounding edits are far more invasive — `errno` localization, function
signature changes for context pointers, etc. — and the refolder routinely
falls back to expansion).

Falling back, the refolder emits the expanded foo.h content into `main.c`
with `#line` directives indicating where it came from:

```c
#include <stdio.h>

#line 4 "foo.h"
extern int g_total;
static inline int square(int n) {
    return n * n;
}
#line 4 "main.c"
int g_total = 0;
int main(void) {
    xjg->g_total = square_xjtr_0(7);
    printf("%d\n", xjg->g_total);
    return 0;
}
```

`foo.h` (modified by consolidation) is *not* `#include`'d. The call
`square_xjtr_0(7)` references an identifier that nothing in this TU
defines — `main.c` won't compile.

### Step 6 — the edit map

When invoked with `--emit-edit-map=foo.editmap.json`, `clang-refold` writes a
sidecar JSON containing one entry per *surviving* edit at the TU
edit-application boundary. Each entry pairs a half-open byte range in the
modified `.i` with the half-open byte range in `main.c` that materialized it:

```json
{
  "modified_pp_source": "main.nolines.i",
  "refolded_output": "main.c",
  "edits": [
    {
      "modified_pp_byte_range": { "begin": 138, "end": 213 },
      "refolded_output_byte_range": { "begin": 92, "end": 248 }
    }
  ]
}
```

In our example, the surviving edits are the two `g_total` → `xjg->g_total`
rewrites inside `main`. The refolder's edit normalization tends to coalesce
multiple nearby edits into a single range; here both rewrites and the bytes
between them are reported as one entry. Coalescing across `main()` is why the
modified-pp range is wider than the union of the two literal rewrites.

The reverted `square` region itself doesn't appear in the edit map, because
after consolidation it matches the unmodified `.i` byte-for-byte. But the
mega-edit's `modified_pp_byte_range` is wide enough that the revert region
(once we know its post-rewrite byte range in the `.i`) falls *inside* it.
That's the anchor restoration uses.

### Step 7 — detecting and repairing dropped edits

`restore_dropped_consolidation_reverts` walks the per-TU revert list. For
each revert:

1. **Translate the pre-rewrite `.i` region into its post-rewrite byte
   range.** The consolidation BatchingRewriter applies rewrites in
   descending-offset order, so each revert's post-rewrite location is its
   pre-rewrite offset shifted by the cumulative net length change of *every
   rewrite that preceded it* in the same `.i`. That's why
   `ConsolidationRevertContext` carries the full `all_i_rewrites` list, not
   just the revert itself: the `xjg->g_total` rewrites in `main` happen at
   higher offsets and don't shift our `square` revert, but in larger
   programs that's not generally true.

2. **Find the edit-map entry containing the post-rewrite `.i` region.** In
   our example the post-rewrite `.i` range for the reverted `square`
   definition is approximately `[20, 95)`. The edit map shows a single entry
   covering `[138, 213)` — *outside* the `square` region, so no entry
   contains it. The fallback in that case is the whole `.c` as the search
   scope. (In Tenjin's real runs the modified region is densely edited and
   coalescing produces an entry that swallows the revert region too; the
   edit-map entry's `refolded_output_byte_range` then becomes the search
   scope, which is much tighter than the whole file.)

3. **Search within the scope for the revert's `.h` form, then its `.i`
   form.** The refolder restores macros during expansion fallback, so the
   `.h` form (with macros) is the better needle than the `.i` form (with
   macros expanded). With no macros in this example the two are identical,
   so we find `"static inline int square(int n) { return n * n; }"` once in
   the residual `.c`.

4. **Splice the modified text in place of the match.** The `.c` becomes:

   ```c
   #include <stdio.h>

   #line 4 "foo.h"
   extern int g_total;
   static inline int square_xjtr_0(int n) {
       return n * n;
   }
   #line 4 "main.c"
   int g_total = 0;
   int main(void) {
       xjg->g_total = square_xjtr_0(7);
       printf("%d\n", xjg->g_total);
       return 0;
   }
   ```

   The call now resolves to the inline definition right above it.

If a revert's needle isn't found anywhere in the `.c`, refolding produced an
`#include` and the modified header carries the change. Nothing to do.

If the needle appears multiple times within the scope, the result is
ambiguous; restoration logs a warning and skips, leaving the dropped edit in
place. Ambiguity within the scope is the residual risk this strategy
accepts.

## Why textual matching inside the scope

The edit-map is *coalesced*: a single entry can cover many logical edits and
the unedited bytes between them. We can't ask `clang-refold` "where in the
`.c` is byte 73 of the `.i`?" for an arbitrary byte. So the edit-map can
constrain the search scope but can't pinpoint individual reverts inside that
scope. Picking the `.h`-then-`.i` form ordering and rejecting non-unique
matches reduces the risk of a wrong splice, but doesn't eliminate it.

An obvious next step (not implemented) would be a refolder extension that
accepts a list of "phantom edit" ranges — bytes that match the unmodified
`.i` but should be reported as edit-map entries anyway. That would give
restoration a precise byte-range in the `.c` and remove the residual textual
search. The plumbing in `clang-refold` for stamping materialized byte ranges
is already there (see the patch that added `--emit-edit-map`); what's
missing is a way to feed it the phantom-edit list.

Adding *sentinel comments* around each reverted region in the `.i` to force
the refolder to emit per-region edit-map entries is tempting but
counterproductive: the sentinels themselves become edits, and edits inside a
would-be-foldable region prevent the very `#include` fold this pipeline
exists to enable.

## Other notes

- The `--emit-edit-map` flag is from the [LLVM commit that introduced
  materialized edit-map emission](https://github.com/Aarno-Labs/llvm-project/commit/4bac3e45d1360f1aa34eb44629bbe2d7cabd0f28).
- `clang-refold --check` re-preprocesses the refolded `.c` and verifies that
  its tokens match the modified `.i`. Enabling `XJ_REFOLD_CHECK=1` runs it
  after every refold for paranoia; restoration runs *before* the check so
  that the check sees the post-restoration `.c`.
- Refolding and restoration only run when `XJ_EXTRA_PREPARATION_PASSES=1`;
  outside that mode the `.i` files are fed directly to downstream tools and
  there is no `.c` reconstruction step.
