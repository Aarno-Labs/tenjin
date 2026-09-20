# Mutable Global Localization

## Where

- [cli/c_refact.py](/cli/c_refact.py)
- [cli/c_refact_decl_splitter.py](/cli/c_refact_decl_splitter.py)
- PANGS disposition manifest schema v8

## What

Converts mutable global variables into field accesses on an explicitly-passed context struct.

This is a whole-program transformation; it only works on applications, not libraries.

## Why

Mutable global variables are unsafe in Rust.

## How

- PANGS computes a separate context-rewrite candidate for every defined mutable global.
  Each candidate names direct source accessors, the transitive internal callers that
  need a context parameter, exact callsites that must pass it, and any safety blockers.
- PANGS disposition policy chooses among immutable, atomic, localization, and unhandled
  outcomes. Its finalized manifest projects only `localize`-selected candidate fields
  into `context_rewrite.selected`.
- Tenjin treats that projection as authoritative. It does not infer localization from
  mutation, escape, or call-graph-component summaries.
- Selected callsites are modified to pass a context struct pointer. Selected functions
  are modified to receive it and redirect accesses to selected globals through it.
- PANGS supplies source-plan-v3 callback-type, typedef, declaration, and `_xjw`
  adapter edits to keep those calls well-typed. An adapter ignores the context
  and forwards to an unchanged function, preserving its original signature,
  direct calls, and unrelated callback uses. One adapter identity is shared by
  all affected uses of the same function, including across translation units.
  When another selected global requires that function to take context, the
  combined plan omits its adapter. PANGS alone composes and validates the final
  edit list; Tenjin checks supported operations, paths, ranges, and overlaps.
  Tenjin applies the finalized edits without reconstructing candidate recipes,
  discovering additional functions, choosing callback wrappers, or repairing
  compiler errors.
  Unsupported callback forms block localization during planning.
- Context construction brings required initializer function declarations into
  `main`'s translation unit and moves late type definitions before the context
  header. PANGS leaves callback storage in place if moving its initializer would
  require naming another translation unit's private function.
- After their declarations and initializers have been copied into the context construction,
  selected globals' original definitions are overwritten with whitespace. Newlines and byte
  widths are preserved so later source locations remain stable.

## Other Notes

- Loosely based upon [Source-to-Source Refactoring and Elimination of Global Variables in C Programs](https://doi.org/10.4236/jsea.2013.65033).
- The idea of using a bipartite graph of call sites & callees comes from [Webs and Flow-Directed Well-Typedness Preserving Program Transformations](https://bquiring.github.io/pdfs/Webs.pdf).
- Beyond function definitions, we must also modify function pointer type annotations.
