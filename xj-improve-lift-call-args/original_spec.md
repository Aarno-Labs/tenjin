# Algorithm: Lift Call-Argument Subexpressions to Satisfy Rust Borrowck

## Purpose

Given a Rust function-call expression `f(e1, ..., en)` (including method calls
`recv.m(e1, ..., en)` and treating the receiver as `e0`), rewrite it into a
sequence of `let` bindings followed by a call whose arguments are simple enough
that borrowck accepts it — *whenever* lifting subexpressions to locals is
sufficient.

This spec is for cases that arise from a C-to-Rust translation where:
- C pointers may become `&T`, `&mut T`, `Box<T>`, `Rc<T>`, or owned `T`.
- C structs may translate to non-`Copy` Rust types.
- C's left-to-right evaluation with no aliasing constraints must be preserved.

## Scope

In scope:
- Call expressions `f(...)`, method calls `x.m(...)`, and nested calls inside
  argument positions.
- Argument subexpressions that share a path prefix with another argument or
  with the callee/receiver, where lifting one side resolves the conflict.

Out of scope (the algorithm must detect and report these, not rewrite them):
- Two `&mut` borrows that genuinely must alias (`swap(&mut v[i], &mut v[j])`).
- Partial move of a container followed by passing the whole container.
- Assignment LHS conflicts (`p.x = f(p)` where `f` returns a borrow into `p`).
- Anything requiring `mem::take`, `mem::replace`, `split_at_mut`,
  `get_disjoint_mut`, or structural changes.

## Definitions

**Place expression.** A syntactic path that denotes a memory location:
`x`, `*e`, `e.field`, `e[i]`, and chains thereof. Function calls and method
calls are not place expressions; their results are values.

**Path prefix.** The sequence of projections in a place. `p.a.b` has prefixes
`p`, `p.a`, `p.a.b`. Index expressions `e[i]` count as a single projection step
whose key is the *value* of `i` (which the algorithm treats as opaque — two
distinct `i` and `j` are assumed to possibly overlap unless proven otherwise).

**Access kind.** For each argument subexpression, classify the access it makes
to each place it touches:
- `Read` — read by value, type is `Copy`.
- `Move` — read by value, type is not `Copy`.
- `SharedBorrow` — `&place` or auto-ref to `&self`.
- `MutBorrow` — `&mut place` or auto-ref to `&mut self`.
- `Call` — opaque call whose return may borrow from arguments (treated
  conservatively as borrowing from every place expression argument).

**Conflict.** Two accesses to places `P` and `Q` conflict iff one is a prefix
of the other (or they are equal) AND at least one access is `Move`, `MutBorrow`,
or `Call`-returning-borrow, AND they are not both `SharedBorrow`.

**Splittable projection.** A field access `place.field` where:
- The type of `place` is a `struct` or `tuple` (not an enum variant accessed
  through pattern), AND
- `place` is reached without going through `Deref`/`DerefMut`/`Index`/`IndexMut`
  or a method call.

Splittable projections allow disjoint borrows of sibling fields. Non-splittable
projections (anything reached through `Box`, `Rc`, `RefCell`, accessor methods,
or indexing) do not.

## Inputs

- An AST for the call site `f(e1, ..., en)` with type information sufficient to
  determine, for each subexpression, its type and whether that type is `Copy`
  and `Clone`.
- For each path prefix, whether the projection is splittable.
- For each callee, its signature (so the access kind of each argument can be
  determined: by-value, `&`, `&mut`).

If types or signatures are unavailable, the algorithm degrades to conservative
mode: treat every non-primitive type as non-`Copy`, every projection through a
`*` as non-splittable, and every function's return as possibly borrowing from
its arguments.

## Algorithm

### Step 1: Build the access set

For each argument position `i` in `0..=n` (with `0` being the receiver for
method calls, if present), walk `ei` and produce a list of `Access` records:

```
Access {
  arg_index: usize,        // which argument this access belongs to
  place: Path,             // sequence of projections from a root variable
  kind: AccessKind,        // Read | Move | SharedBorrow | MutBorrow | Call
  span: Span,              // source location of the subexpression
  subexpr: ExprId,         // the subexpression that produced this access
}
```

Rules for the walk:

1. A bare place expression `e` used as a by-value argument produces one access
   to the place denoted by `e`, with kind `Read` if its type is `Copy` else
   `Move`.

2. `&e` produces a `SharedBorrow` of the place `e`, plus recursive walks of
   any non-place subexpressions inside `e` (e.g., index expressions).
   `&mut e` likewise produces `MutBorrow`.

3. A call `g(a1, ..., am)` appearing inside `ei` produces:
   - Recursive accesses from each `aj`, tagged with `arg_index = i`.
   - One `Call` access whose place is the *join* of all place arguments to
     `g` — that is, the longest common prefix when there is one. If `g` has
     no place-typed arguments at all (e.g. the receiver chain contains only
     other calls or non-place operators), **no `Call` access is emitted**.
     The recursive sub-accesses still capture any real borrows the chain
     takes; synthesizing a "conflicts with everything" placeholder here is
     unsound in practice because it spuriously lifts the callee and any
     unrelated argument that shares an outer-call scope.
   This models the conservative assumption that `g`'s return value may borrow
   from any of its place arguments — bounded above by the places it actually
   touches.

4. Method calls `x.m(a1, ..., am)` are treated as `g(&x, a1, ..., am)` or
   `g(&mut x, a1, ..., am)` based on `m`'s receiver kind. If the receiver kind
   is unknown, assume `&mut x`.

5. Index expressions `e[i]` contribute a projection step keyed by an opaque
   index token. Two index expressions on the same base are assumed to overlap
   unless both indices are integer literals with different values.

6. The callee expression `f` itself is walked too. `p.vtable.func(p)` produces
   an access for `p.vtable.func` (a `Move` or `Read` depending on type) in
   addition to accesses for the arguments.

### Step 2: Detect conflicts

Build the conflict graph: nodes are accesses, edges connect conflicting pairs
(per the Definitions section). Two accesses *from the same argument index* do
not conflict with each other for the purposes of this algorithm — borrowck
handles intra-expression sequencing within a single argument fine; the problem
is between arguments.

If the conflict graph is empty, emit the call unchanged.

### Step 3: Choose subexpressions to lift

For each conflict edge `(A, B)`:

1. If both accesses' places are reached only through splittable projections
   from a common ancestor, *and* the two places are disjoint siblings, mark the
   edge as **resolved-by-splitting** and continue. (Rust borrowck will accept
   it without lifting.)

2. Otherwise, choose one of `A`, `B` to lift. Prefer to lift:
   - The argument with the later `arg_index` (preserves left-to-right intent).
   - The argument whose access kind is *less* destructive (`Read` over `Move`,
     `SharedBorrow` over `MutBorrow`), because lifting a read into a local is
     usually trivial, while lifting a move may strand the source.
   - The argument that is a nested call result rather than a bare place, since
     the call already implies a temporary.

   Tie-break deterministically (e.g., highest `arg_index`, then leftmost span).

3. Record the chosen subexpression for lifting in a set `ToLift`. If the same
   argument is chosen for multiple conflicts, lift it once.

### Step 4: Check feasibility of each lift

For each subexpression `s` in `ToLift`, determine the lift strategy:

- If `s` produces an *owned value of `Copy` type*: lift as `let tmp = s;`.
- If `s` produces an *owned value of non-`Copy` type that is `Clone`*: lift as
  `let tmp = s.clone();` if the original use was a borrow; as `let tmp = s;`
  if the original use was by-value (and we have verified the source is not
  needed afterward in the same call).
- If `s` is `&place` where `place`'s type is `Copy`: lift as `let tmp = place;`
  and rewrite the use site from `&...` to `&tmp`. (Cheaper than cloning.)
- If `s` is `&place` where `place`'s type is `Clone` but not `Copy`: lift as
  `let tmp = place.clone();` and rewrite the use site to `&tmp`.
- If `s` is `&mut place`: **infeasible by lifting alone.** Report and abort
  this call site.
- If `s` is a `Move` of a non-`Copy`, non-`Clone` value, AND the source place
  is also accessed elsewhere in the same call: **infeasible.** Report.
- If `s` is a nested call `g(...)` whose return type is a reference borrowed
  from arguments still accessed by the outer call: the lift only helps if the
  reference's lifetime ends before the outer call. Conservatively, require the
  return type to be owned or `Clone`; if it's a borrow, lift as
  `let tmp = g(...).clone();` (or `.to_owned()` for `&str`/`&[T]`); otherwise
  report infeasible.

If any lift in `ToLift` is infeasible, abort with a diagnostic that names the
specific subexpression and the reason. Do not emit a partial rewrite.

### Step 5: Order the lifts

Lifts must execute in the original left-to-right evaluation order of their
subexpressions to preserve C semantics (which the translator presumably already
honored at the source level). Sort `ToLift` by source position.

Detect ordering hazards: if lift `L1` reads a place that lift `L2` writes (or
that the remaining call argument writes), and `L1` appears textually before
`L2`, the original order is fine. If the dependency runs the other way (the
read needs to see the post-write value), that's a translator bug at a higher
level — report it.

### Step 6: Emit

Produce a block:

```rust
{
    let tmp_1 = <lift_1>;
    let tmp_2 = <lift_2>;
    // ...
    f(<rewritten args>)
}
```

where each `tmp_k` is a fresh name (use a hygienic generator; suggested
template `__lift_{arg_index}_{counter}`), and each original subexpression in
the call is replaced by the corresponding `tmp_k` (or `&tmp_k` / `&mut tmp_k`
if the original was a reference expression).

If the call is in a position that requires a single expression (e.g., it *is*
the function body), wrap in a block as shown. Otherwise, emit the `let`s as
preceding statements and the rewritten call in place.

### Step 7: Verify (optional but recommended)

If the surrounding pipeline can invoke `rustc --emit=metadata` or
`cargo check` on the rewritten code cheaply, do so. If borrowck still rejects
the rewrite, the algorithm's conservative model missed a case; fall back to
reporting infeasible rather than iterating blindly.

## Worked examples

### Example A: `f(p, p->x)` with Copy field

Input (post-translation):
```rust
f(p, p.x)        // p: &mut S, p.x: i32 (Copy)
```

Step 1 accesses:
- arg 0: `MutBorrow` of `p` (auto-ref through `&mut self`-like signature, or
  explicit if `f` takes `&mut S`)
- arg 1: `Read` of `p.x`

Step 2: conflict (prefix relation, one is `MutBorrow`).

Step 3: lift arg 1 (the `Read`, less destructive).

Step 4: feasible — `Copy` value, `let tmp = p.x;`.

Output:
```rust
let __lift_1_0 = p.x;
f(p, __lift_1_0)
```

### Example B: `p.m(&p.x)` with non-Copy, Clone field

Input:
```rust
p.m(&p.x)        // p: &mut S, p.x: String
```

Step 1 accesses:
- arg 0 (receiver): `MutBorrow` of `p`
- arg 1: `SharedBorrow` of `p.x`

Step 2: conflict.

Step 3: lift arg 1.

Step 4: `&place` with `Clone` type → `let tmp = p.x.clone(); ...&tmp`.

Output:
```rust
let __lift_1_0 = p.x.clone();
p.m(&__lift_1_0)
```

### Example C: `swap(&mut v[i], &mut v[j])`

Step 1 accesses:
- arg 0: `MutBorrow` of `v[i]`
- arg 1: `MutBorrow` of `v[j]`

Step 2: conflict (index expressions on same base, indices not provably
distinct).

Step 3: lift the later one.

Step 4: `&mut place` → infeasible by lifting. **Abort with diagnostic.**

### Example D: `f(g(p), h(p))`

Step 1 accesses:
- arg 0: recursive accesses from `g(p)` — `MutBorrow` of `p` (assuming `g`
  takes `&mut`), plus a `Call` access on `p`.
- arg 1: same shape for `h(p)`.

Step 2: conflicts between the two `MutBorrow`s, and between each `Call` and
the other's borrow.

Step 3: lift arg 0's `g(p)`.

Step 4: if `g` returns owned → `let tmp = g(p);`. If `g` returns `&T` borrowed
from `p` → `let tmp = g(p).clone();` if `Clone`, else infeasible.

Output (owned case):
```rust
let __lift_0_0 = g(p);
f(__lift_0_0, h(p))
```

### Example E: `f(&p.a, &p.b)` with splittable fields

Step 1 accesses:
- arg 0: `SharedBorrow` of `p.a`
- arg 1: `SharedBorrow` of `p.b`

Step 2: places are siblings under a splittable projection, both `SharedBorrow`
→ no conflict to begin with. Emit unchanged.

(Same shape with `&mut p.a, &mut p.b`: still no conflict, because field splits
work for `&mut` too on splittable projections.)

### Example F: `f(&p.a, &p.b)` through a `Box`

If `p: Box<S>`, the projection goes through `Deref`, which is **not**
splittable. Step 2 sees a conflict; Step 3 lifts arg 1; Step 4 succeeds with
`let tmp = &p.b; f(&p.a, tmp)` — but wait, this still holds two borrows
through `*p` simultaneously. The lift only ends the *subexpression's* borrow,
not the local's. For two `SharedBorrow`s this is fine (both shared). For a
`SharedBorrow`/`MutBorrow` mix through `Deref`, lift the value:
`let tmp = p.b.clone(); f(&mut p.a, &tmp)`.

This nuance is why Step 4 distinguishes "lift the reference" from "lift the
value via clone" based on what the conflict actually is.

## Diagnostics

When aborting, the diagnostic must include:

1. The call site span.
2. The two conflicting accesses (places, kinds, spans).
3. The reason lifting cannot resolve it (e.g., "second argument requires
   `&mut` to a place also mutably borrowed by the first argument; lifting
   would require `mem::replace` or `split_at_mut`").
4. A suggested manual fix where one is obvious (e.g., "consider
   `v.split_at_mut(j)`").

## Test cases (minimum set)

The implementation should pass at least these, drawn from the patterns
enumerated earlier in the conversation:

1. `f(p, p.x)` — Copy field, `p: &mut T`. Lifts.
2. `f(p, p.x)` — non-Copy Clone field. Lifts with `.clone()`.
3. `f(p.x, p)` — `p: &mut T`, x Copy. Lifts.
4. `f(p.x, p)` — `p` owned, x non-Copy. Aborts (partial move).
5. `f(p.a, p.b)` — direct fields, non-Copy. Emits unchanged (field split).
6. `f(p.a, p.b)` — `p: Box<T>`, non-Copy fields. Lifts (via clone).
7. `f(&p.a, &p.b)` — direct fields. Unchanged.
8. `f(&mut p.a, &mut p.b)` — direct fields. Unchanged.
9. `f(&mut p.a, &p.b)` — direct fields. Unchanged.
10. `f(&mut p.a, &p.b)` — through `Box`. Lifts via clone.
11. `p.m(p.x)` — Copy field. Lifts.
12. `p.m(&p.x)` — Clone field. Lifts via clone.
13. `p.set_y(p.get_x())` — getter returns value. Lifts.
14. `p.set_y(p.get_x())` — getter returns `&T`. Lifts via clone if Clone, else
    abort.
15. `f(g(p), p)` — `g` owned return. Lifts.
16. `f(g(p), h(p))` — both `&mut p`. Lifts arg 0.
17. `swap(&mut v[i], &mut v[j])`. Aborts with split_at_mut suggestion.
18. `f(&a[i], &a[j])`. Unchanged (both shared).
19. `f(a[i], a[j])` — non-Copy Clone. Lifts via clone.
20. `f(cond ? p.a : p.b, p)` — written as `if`-expression. Lifts the `if`.
21. `f(p.vtable.func, p)` and `(p.vtable.func)(p)`. Lifts the function ptr.
22. `f(p, p)` — `p: &T` Copy of reference. Unchanged. `p: T` non-Copy: abort
    (double move).
23. `f(g(p, p.x), h(p))` — nested. Recursive application: inner `g(p, p.x)`
    lifts `p.x`; outer then lifts `g(...)` result.

## Implementation notes

- The access-set walk is the only part that needs deep AST/type knowledge.
  Everything after Step 1 operates on the `Access` list.
- The conflict relation is symmetric; deduplicate edges.
- "Splittable" status of a projection can be cached per type.
- Hygiene for `__lift_*` names: include the call site's line number and a
  per-call counter to avoid collisions when multiple call sites are rewritten
  in the same function.
- The algorithm is single-pass per call site, but nested calls require
  recursion: handle inner calls before outer, so that lifts from inner calls
  are already materialized as locals by the time the outer call's access set
  is built.
- Preserve evaluation order. If the translator relied on left-to-right
  evaluation of arguments for side effects (common in C-to-Rust output), the
  lift order must match.
- Do not lift subexpressions with side effects more than once. The `ToLift`
  set is keyed by subexpression identity, not by syntactic appearance.

## What this algorithm does not do

- It does not change function signatures.
- It does not introduce `mem::take`, `mem::replace`, `split_at_mut`,
  `get_disjoint_mut`, `RefCell`, or `unsafe`.
- It does not reorder arguments or change which function is called.
- It does not fix conflicts that arise outside argument positions (e.g., the
  LHS/RHS of an assignment, the scrutinee of a `match`, the condition of an
  `if let`).

Those cases require separate passes with their own algorithms; this one is
deliberately scoped to "lifting subexpressions in call arguments is enough."
