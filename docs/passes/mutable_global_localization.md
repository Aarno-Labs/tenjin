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
- PANGS also supplies the callback-type, typedef, and declaration edits needed to
  keep those calls well-typed. Tenjin applies these edits without discovering
  additional functions, generating callback wrappers, or repairing compiler errors.
  Unsupported callback forms block localization during planning.
- After their declarations and initializers have been copied into the context construction,
  selected globals' original definitions are overwritten with whitespace. Newlines and byte
  widths are preserved so later source locations remain stable.

## Other Notes

- Loosely based upon [Source-to-Source Refactoring and Elimination of Global Variables in C Programs](https://doi.org/10.4236/jsea.2013.65033).
- The idea of using a bipartite graph of call sites & callees comes from [Webs and Flow-Directed Well-Typedness Preserving Program Transformations](https://bquiring.github.io/pdfs/Webs.pdf).
- Beyond function definitions, we must also modify function pointer type annotations.
