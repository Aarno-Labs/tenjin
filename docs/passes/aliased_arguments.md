# Aliased Argument Lifting

## Where

- [xj-improve-lift-call-args](/xj-improve-lift-call-args)

## What

Lifts potentially problematic constructs from call arguments into local variables.

## Why

Syntactic patterns that are fine in C can cause borrow checker errors in Rust.
These errors are incidental, in the sense that pulling one or more subexpressions
into local variables will eliminate the borrow checker errors.

## Examples

- `f(X, X->F)` => `let newvar = X->F; f(X, newvar)`
- `f(X, &X)` => `let newvar = X; f(newvar, &X)`

## Other Notes

- [spec.md (written by Claude Opus 4.7)](/xj-improve-lift-call-args/original_spec.md)