# Mutable Global Localization

## Where

- [cli/c_refact.py](/cli/c_refact.py)
- [cli/c_refact_decl_splitter.py](/cli/c_refact_decl_splitter.py)
- [@brk/cclyzerpp](https://github.com/brk/cclyzerpp/tree/tenjin)

## What

Converts mutable global variables into field accesses on an explicitly-passed context struct.

This is a whole-program transformation; it only works on applications, not libraries.

## Why

Mutable global variables are unsafe in Rust.

## How

- `cclyzerpp` computes a pointer analysis and call graph
- Based on the points-to and call graphs, we compute mutability.
  - Pointers that escape to unknown functions are assumed to be mutated.
- The call graph feeds into an updatability determination:
  - We construct a bipartite graph of call sites and callees.
  - Either may be "unknown". Unknown call sites correspond to callback invocations
    outside of our control.
  - Any connected component which contains an unknown element is not updatable.
- The call graph also determines the "mutable tissue" -- the set of functions which
access mutable globals, plus the transitive closure of their callers.
- Updatable call sites are modified to pass a context struct pointer.
- Updatable function definitions are modified to take a context struct pointer,
and to use it when accessing mutable globals.

## Other Notes

- Loosely based upon [Source-to-Source Refactoring and Elimination of Global Variables in C Programs](https://doi.org/10.4236/jsea.2013.65033).
- The idea of using a bipartite graph of call sites & callees comes from [Webs and Flow-Directed Well-Typedness Preserving Program Transformations](https://bquiring.github.io/pdfs/Webs.pdf).
- Beyond function definitions, we must also modify function pointer type annotations.