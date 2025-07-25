# Context

We assume that `c2rust` produces a direct, semantics-preserving transformation
of input C code into unsafe Rust code. Tenjin's goal is to produce output Rust
which is as safe as is feasible.
Idiomatic output is a nice but secondary concern.

# Priorities & Beliefs

When it comes to automated translations:

1. Preserving semantics is more important than removing unsafety.
   - We assume this matches the preferences of most (not all)
  decisionmakers/maintainers/users of Tenjin.
   - Also: Taking working-but-ugly code
  and making it safer, faster, and/or more idiomatic is fun. 
  Taking plausible-but-subtly-broken code and hunting down the
  lurking bugs: not so much.
   - LLMs do not (yet) reliably preserve semantics.
1. The input C code defines what it **is**;
  it does not define what it **ought** to become.
  [One cannot derive ought from is.](https://williamjbowman.com/resources/wjb2024-ethical-compiler.pdf)
  Sometimes code can have several reasonable translations.
  The driver should be able to influence which translation
  they end up with.
1. Full automation is sometimes not the most efficient option.
   - One ultimately seeks to minimize the sum of translation costs
  and maintenance costs.
   - The costs of maintainership are largely invisible to outsiders.

# Levels of Unsafety

We conceive of several tiers of unsafety in the Rust code produced by `c2rust`.
One can, mostly, think of these as being properties of the input C code.

- **Trivially Safe**: `c2rust` may mark it as `unsafe` but deleting the
keyword produces safe Rust without further code changes.
- **Structurally Safe**: safety can be achieved with localized pattern matching
of expressions. Modulo, perhaps...
    - ... generic program analysis (mild to overwhelming burden)
    - ... recognition of data structures
- **Minor Refactoring Needed**: individual functions or types may have
small rearrangements needed to obtain safety. But the concept of the
C program does not violate the borrow checker.
- **Major Refactoring Needed**: the basic approach used by the C code would
need to be reinvented to fit within the strictures of Rust's type system
and borrow checker. Example: garbage collector in a VM

Automated refactoring can move code from the **Refactoring Needed** to
**Structurally Safe** categories--a major win.
Tenjin's overall focus (in the realm of refactoring) is on refactoring C code
before translation, so as to make the initial Rust output as safe as is feasible.
Refactoring unsafe Rust output is certainly possible, and is a primary focus for
other tools.


Automating major semantics-changing refactorings of large real-world codebases
goes beyond Tenjin's forseeable remit.
