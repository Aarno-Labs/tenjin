# xj-prepare-guidance

## Where

- [xj-prepare-guidance](/xj-prepare-guidance) (the C-to-C pass)
- [markers.rs](/c2rust/c2rust-transpile/src/translator/markers.rs) (the transpiler side)
- `prep_xj_prepare_guidance` in [translation_preparation.py](/cli/translation_preparation.py)

## What

C preparatory refactoring pass that applies type guidance (`vars_of_type`,
`fn_return_type`, `vars_mut`) in the C, so the transpiler reads it off the
program. It runs after the pointer passes and static-inline
un-uniquification, and before refolding. Running after the pointer passes
means a guided pointer that still moves is reported as a diagnostic rather
than translated.

In the marker names below, `<T>` and `<U>` stand for types, spelled as
described under [How](#how).

1. **Retyping.** Every guided declaration is rewritten to use a marker
   typedef: `const char *msg` guided as `String` becomes `xj_ty_String msg`
   with `typedef const char *xj_ty_String;`. Parameters are retyped at the
   same position in every redeclaration, file-scope variables in every
   redeclaration. `xj-guidance.json` gains
   `"marker_typedefs": {"xj_ty_String": "String"}`.
2. **Operator normalization.** Pointer operators on guided operands become the
   form whose Rust meaning is direct. On a buffer held in a pointer, `*p`,
   `*(p + i)` and `p[i]` become `(*xj_index_<T>(p, i))`; the marker returns a raw
   pointer to the element, so the result is still an lvalue. Guided arrays are
   subscripted as written (`*a` → `a[0]`). On single objects `p[0]` → `(*p)`;
   `&*p` → `p`. A shared slice moved forward by a statement of its own
   (`p++`, `p += n`) is resliced: `p = xj_slice_from_<T>(p, n)`. A `String`
   or `str` cannot be indexed in Rust, so reads of its characters (`s[i]`,
   `*s`, `*(s + i)`) become `xj_char_at_<T>(s, i)`; writes keep their C form.
3. **Place markers.** A pointer-valued flow (call argument, `=`, local
   initializer, `return`, explicit pointer cast) whose base is an array or a
   guided buffer becomes one of `xj_slice_all_<T>(b)`,
   `xj_slice_from_<T>(b, i)`, `xj_elem_ref_<T>(b, i)`, chosen from the
   source shape (`b`, `&b[i]`, `b + i`) and the sink's demand (`&T`, `&[T]`,
   raw pointer).
4. **Coercion markers.** Any other flow whose source and sink carry different
   guidance is wrapped in `xj_coerce_<T>_to_<U>(e)`; the marker's
   parameter and return types carry the two typedefs. A guided pointer or
   array flowing into a place with the same guidance but a differently
   written C type (`char *` into `const char *`, an array into a pointer) is
   wrapped too, in an identity coercion, so no C conversion is left to apply
   to the guided value.
5. **Null tests.** `p == NULL`, `p != 0`, `!p` and `p` in a condition, for
   guided `p`, become `xj_is_null_<T>(p)`.

Markers are `static __inline__` functions computing what C would have (the
value itself, `b + i`, `p == 0`), so the C still compiles and means the same
thing. They are spelled `__inline__` rather than `inline` so they also compile
under `-std=c89`, where `inline` is not a keyword.

## Why

Deciding how a guided value is read needs the shape of the source expression
and the demand of the sink together. The C pass sees both; the transpiler
converts one expression at a time and sees only one. Recording the decision
in the C leaves the transpiler one rule per marker family.

## Examples

Guidance:

```json
{"vars_of_type": {"&[u8]": ["take:s", "peek:s"], "String": "greet:name"}}
```

C:

```c
unsigned char take(const unsigned char *s);
unsigned char peek(const unsigned char *s) { return *s; }
void greet(const char *name);
void f(int i) {
    unsigned char buf[8] = {0};
    take(&buf[i]);
    greet("world");
}
```

After xj-prepare-guidance (`xj_guidance.h` holds the typedefs and markers):

```c
unsigned char take(xj_ty_ref_slice_u8 s);
unsigned char peek(xj_ty_ref_slice_u8 s) { return (*xj_index_ref_slice_u8(s, 0)); }
void greet(xj_ty_String name);
void f(int i) {
    unsigned char buf[8] = {0};
    take(xj_slice_from_ref_slice_u8(buf, i));
    greet(xj_coerce_ptr_const_char_to_String("world"));
}
```

Rust:

```rust
pub unsafe extern "C" fn peek(mut s: &[u8]) -> ::core::ffi::c_uchar {
    return s[0 as usize];
}
pub unsafe extern "C" fn f(mut i: ::core::ffi::c_int) {
    let mut buf: [::core::ffi::c_uchar; 8] = [0 as ::core::ffi::c_uchar, 0, 0, 0, 0, 0, 0, 0];
    take(&buf[i as usize..]);
    greet(String::from("world"));
}
```

## How

The tool parses every translation unit twice. The first sweep interns every
typedef and marker key; the keys are then named, so a declaration from a
shared header gets the same typedef name in every TU and refolding can move
the edit back into the header. The second sweep renders the edits against
those names.

A key is one definition, shared by every declaration and flow that needs it.
Its name says what it is: a typedef is `xj_ty_` and its Rust type
(`xj_ty_ref_mut_slice_u8` for `&mut [u8]`, `xj_ty_Vec_c_int`,
`xj_ty_array_4_u8`); a marker is its family and the types it connects, each
the Rust type when guided and the C type otherwise
(`xj_coerce_ptr_const_char_to_String`, `xj_index_ref_slice_u8`,
`xj_slice_all_ptr_const_unsigned_char`). This is what `<T>` and `<U>` stand
for in the marker names above. `<T>` is the type of the buffer for `index`,
of the string for `char_at`, and of the operand for `is_null`; it is the
sink's type for the place markers (`slice_all`, `slice_from`, `elem_ref`);
and `xj_coerce_<T>_to_<U>` converts from `<T>` to `<U>`. Keys that would read the same, such
as `Vec<u8>` on both `unsigned char *` and `unsigned char[8]`, are told apart
by `_1`, `_2`, ... in sorted key order; the first keeps the plain name.

A declaration's guidance comes from matching `file@fn:var#line` specifiers
(suffix-blind on `_xjtr`), in which only `fn` is required. Parameters of
prototypes match as parameters of definitions do. When several specifiers
match, the most specific wins: a named function over `*`, then a line, then a
file; among equally specific ones, the first in the JSON wins. `vars_mut` is resolved to
`"vars_mut_resolved": {"fn:var": bool}` (bare `var` at file scope).
`semantically_immutable_globals`, which the PANGS pass records with the same
keys, is passed through and read by the transpiler by name.

Typedefs and markers whose types are written with builtins alone go in
`xj_guidance.h`, which `BuildInfo` force-includes into every later compile.
Anything that names a record, an enum, or a typedef is written as the program
wrote it and inserted once per TU, before the first top-level declaration that
needs it. Records are spelled as declared, so their declarations stay where
the program put them and the order of the translated items doesn't change.
Keeping typedef names matters because guidance such as `&[tflac_u8]` names
them, and the transpiler only emits typedefs that something uses. Buffers whose
element carries guidance of its own (`Vec<Vec<u8>>` on `char **`,
`&Option<&u8>` on `const u8 **`) get an element typedef, so `x[i]` and `*p`
carry it by clang's own typing.

Nothing is wrapped in constant initializers, unevaluated operands, variadic
arguments, or array initializers from brace lists and string literals; the
transpiler applies the same coercion table there, from the declared type. A
static whose Rust value cannot be a constant (an owned `String`, `Vec` or
`Box` its initializer fills, or a `Vec` in place of a C array) holds the empty
value (`String::new()`, `Vec::new()`) and is assigned in
`c2rust_run_static_initializers`, as c2rust does for any static initializer
Rust cannot evaluate at compile time; it is `static mut` even if `vars_mut`
says otherwise.

On the Rust side, `TypeConverter` maps marker typedefs to their Rust types,
marker typedef and function declarations produce no items, and calls to
markers are translated by family, which the transpiler reads from the
`markers` table in `xj-guidance.json`: `(*xj_index_<T>(b, i))` is
`b[i as usize]`; `xj_char_at_<T>(s, i)` is
`s.as_bytes().get(i).copied().unwrap_or(0)`, so reading at the length yields
the terminator's 0 as in C; and an identity `xj_coerce_<T>_to_<T>(e)` is `e`
without the C conversions around it.
The transpiler treats marker calls as free of side effects, so `p[i] += 1`
still names its place directly. Libc recognizers look through identity
markers (`xj_coerce`, `xj_slice_all`) at the value the program passed.

## Diagnostics

Guidance the program cannot honour is reported on stderr and in the
`"guidance_diagnostics"` list of `xj-guidance.json`. Each entry is an object
with four string fields:

- `kind`: one of the kinds below;
- `site`: the source location, as `file:line:column`, or empty for
  `unmatched-specifier`, which has none;
- `subject`: what the diagnostic is about, such as a declaration and its
  guided type (`p: &[u8]`), or the specifier that matched nothing;
- `message`: a sentence explaining it.

The kinds are:

- `pointer-motion`: `p--`, `p -= n`, `p = q + k`, or forward motion that is
  not a statement on a shared slice, on a guided pointer the pointer passes
  left in place;
- `backward-offset`, `pointer-difference`, `pointer-ordering`;
- `subscript-of-single-object`: `p[i]`, `i != 0`, on a `&T` or `Box<T>`;
- `null-test-of-reference`: the test is constant in Rust;
- `null-into-reference`: a null constant flows where a reference is guided;
- `unhandled-coercion`: for example an unguided pointer, which has no length,
  flowing into a slice;
- `not-retyped`: bit-fields, variably-modified types, declarations whose type
  cannot be rewritten in place (including one declarator of `T *a, *b;`,
  whose specifiers the others share), and fields guided with a borrow that is
  not `'static` (the record would need a lifetime parameter, which the
  transpiler does not declare);
- `not-normalized`: a subscript of a guided buffer whose element type (an
  array, a function pointer) no index marker can return;
- `edit-refused`: a rewrite the pass decided on could not be made, because
  the expression is not written contiguously in the file (it straddles a
  macro expansion) or because it overlaps a rewrite already made;
- `edit-lost`: a rewrite that a later rewrite containing it would silently
  drop. This is a bug in the pass, and the file is left unchanged;
- `conflicting-guidance`, `unmatched-specifier`.

## Other Notes

- The pass runs on the `.nolines.i` files, so the `#line` and `file@` specifier
  components are matched against locations in those files.
- A field (`Rec:field`) is guided as a variable is: its declaration is
  retyped, and since `s.field` and `p->field` then have the marker type,
  every use gets the same markers.
- System headers are part of the `.nolines.i`, so their declarations can be
  guided too. Refolding cannot carry the edit back into the header, so the
  edited text stays in the `.c`, and the transpiler then treats it as the
  codebase's own declaration: guiding a field of `struct stat` gives a
  generated `stat` with that field's type instead of `libc::stat`.
- A coercion the transpiler's table does not know is emitted as
  `xj_unhandled_coercion(value, "<from> -> <to>")`, an identity function that
  keeps the value's type (so rustc still reports the mismatch) and keeps the
  site greppable.
- A null test of a `String`, `Vec`, or slice becomes `.is_empty()`, which
  differs from C when the value can be empty and non-null.
- Rerunning xj-prepare-guidance on its own output (the guidance already has
  `marker_typedefs`) changes nothing.
