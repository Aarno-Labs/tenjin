# K&R Syntax Elimination

## Where

- [cli/c_refact_knr.py](/cli/c_refact_knr.py), driven by `prep_eliminate_knr`
  in [translation_preparation.py](/cli/translation_preparation.py)

Runs directly after `expand_preprocessor`, so each translation unit is a
self-contained `.i` file.

## What

C preparatory refactoring pass which rewrites K&R (old-style) declarations and
definitions into ISO C prototype form:

| Construct | Example | Becomes |
|---|---|---|
| Old-style definition | `int f(x, s) int x; char *s; {…}` | `int f(int x, char *s) {…}` |
| …implicitly-typed parameters | `int f(a, b) {…}` | `int f(int a, int b) {…}` |
| …implicit `int` return type | `static f(a) int a; {…}` | `static int f(int a) {…}` |
| Unprototyped definition | `int f() {…}` | `int f(void) {…}` |
| Unprototyped declaration | `extern int f();` | the definition's parameter list |

Unprototyped *function pointer* types (`int (*fp)();`, `typedef int F();`,
no-prototype function types in casts and struct fields) are counted and
reported, but not rewritten. Reconstructing their parameter lists needs
assignment- and call-site flow analysis rather than a local syntactic fix; the
same `FunctionNoProtoTypeLoc`s are already walked by
[xj-prepare-findfnptrdecls](/xj-prepare-findfnptrdecls).

## Why

K&R syntax is annoying to modify, especially for parameters-omitted
function declarations; this allows subsequent passes to not need
to handle the corner cases.

It also improves translation correctness: Baseline c2rust translates a K&R
  definition using the *declared* parameter types, which silently changes how
  the function is called.

## Examples

Before:

```c
extern int scale();

int scale(x, factor)
int x;
int factor;
{
    return x * factor;
}
```

After:

```c
extern int scale(int x, int factor);

int scale(int x, int factor)
{
    return x * factor;
}
```

giving

```rs
#[no_mangle]
pub extern "C" fn scale(
    mut x: ::core::ffi::c_int,
    mut factor: ::core::ffi::c_int,
) -> ::core::ffi::c_int {
    x * factor
}
```

See [tests/snapshotted/knr_elimination](/tests/snapshotted/knr_elimination) for
the end-to-end fixture.

## Promoted parameters and the ABI

A parameter of a type subject to the default argument promotions — `_Bool`,
`char`, `short`, `float` — is passed differently depending on whether a
prototype is visible. Given

```c
int blend(c, weight)
char c;
float weight;
{ return (int)(c * weight); }
```

callers pass `c` as an `int` and `weight` as a `double`, and the callee
converts on entry. Rewriting this to `int blend(char c, float weight)` moves
those conversions to the call site. That is fine for every call that can see
the new prototype, but *not* for a call made through an unprototyped function
pointer — and those still exist after this pass, since they are out of scope
above. Nothing would diagnose the mismatch.

This is exactly what baseline c2rust does with the fixture: without this pass it
emits `blend(c: c_char, weight: c_float)`, taking the declared types rather than
the ones callers actually pass. The pass corrects that.

So the pass keeps the promoted signature and reintroduces the
declared parameter as a body-local, which is precisely the semantics C ascribes
to an old-style definition:

```c
int blend(int c_xjknr, double weight_xjknr)
{ char c = c_xjknr; float weight = weight_xjknr; return (int)(c * weight); }
```

c2rust renders this as safe Rust, with the conversions intact:

```rs
#[no_mangle]
pub extern "C" fn blend(
    mut c_xjknr: ::core::ffi::c_int,
    mut weight_xjknr: ::core::ffi::c_double,
) -> ::core::ffi::c_int {
    let mut c = c_xjknr as ::core::ffi::c_char;
    let mut weight = weight_xjknr as ::core::ffi::c_float;
    (c as ::core::ffi::c_int as ::core::ffi::c_float * weight) as ::core::ffi::c_int
}
```

Parameters of every other type — `int`, pointers, structs, and arrays, which
decay rather than promote — are passed identically either way, so they are
always rewritten directly and need no shadow copy.

## How K&R definitions are detected

Not from the type. libclang reports an old-style definition as a
`FUNCTIONPROTO`, not a `FUNCTIONNOPROTO`:

```
int f(x, s) int x; char *s; {…}    ->  cursor.type.spelling == 'int (int, char *)'
```

What distinguishes it is where the parameter cursors live. For an old-style
definition a parameter's extent covers its entry in the declaration list *after*
the closing paren, or — for an implicitly-typed parameter — just the bare
identifier inside the parens. Two further quirks matter:

- `cursor.get_arguments()` yields parameters in *identifier-list* order, which
  is the order the prototype needs, even when the declaration list below is
  written in a different order.
- `cursor.type` is already the *promoted* signature. `int d(x) float x;` reports
  `double (double)` while the parameter reports `float`, so prototypes must be
  rendered from the parameter cursors, never from `cursor.type.argument_types()`.

Note that clang rejects a parameter list mixing declared and undeclared
parameters (`int f(int x, a, s)`), so that form never reaches this pass.

## Consistency and safety

**One signature per function, project-wide.** A function may be declared
unprototyped in one translation unit and defined in another; if the two picked
different signatures the result would be a link-level ABI mismatch that nothing
downstream checks. The pass therefore analyzes every TU first, agrees on one
signature per function, and only then rewrites. Definitions are authoritative;
already-prototyped declarations are a fallback used only for functions defined
outside the project, since two of those may legitimately differ by parameter
name alone. Functions with internal linkage are keyed per-file, because
`prep_uniquify_statics` has not run yet and two files may each have their own
`static int helper()`.

**Verify and roll back.** Making a prototype visible turns a previously
unchecked call into a checked one. A call site that passed a wrong-typed
argument through an unprototyped declaration was already undefined, and becomes
a diagnostic the moment the prototype appears. After rewriting a TU the pass
re-parses it and compares diagnostics against the baseline; a new error, or a
new diagnostic matching `RISKY_DIAGNOSTIC_SUBSTRINGS`, restores that file
verbatim. Rollback is per-file, so one pathological TU does not lose the pass.

**What is deliberately left alone**, each reported in the pass summary:

- An unprototyped declaration of a function defined *outside* the project.
  Its parameter types could be guessed from call sites, but a wrong guess is a
  silent ABI error that still compiles, which the verify gate cannot catch.
  (An unreferenced declaration still becomes `(void)`, which is safe.)
- An unprototyped definition that some call site passes arguments to — already
  undefined behaviour, and not worth converting into a compile error.
- A parameter whose declarator cannot be rendered, or which is `volatile`-
  qualified in a position where the fallback renderer would drop the qualifier.
- Any function whose parameter list cannot be located in the token stream.

## Limitations

- Comments inside a replaced K&R declaration list are dropped.
- The fallback type renderer, used when clang reports a joined declarator
  (`int a, b;` gives `b` the extent `int a, b`), drops top-level `const` and
  `register`. Both are inert for a parameter; `volatile` is not, and is skipped
  instead.
- Only preprocessed (`.i`) translation units are rewritten; see below.
