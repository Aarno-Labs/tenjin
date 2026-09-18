# Source-aware context localization in PANGS and Tenjin

## Decision and scope

PANGS will accept the compilation database for the exact preprocessed source
used to produce `linked_module.bc`. An integrated Clang frontend will collect
source facts, and PANGS will combine them with its LLVM analysis to compute
source-complete localization candidates, safety blockers, and the selected
rewrite plan. Tenjin will materialize that plan and validate the resulting C
and Rust.

The source-plan-v2 implementation is described in
`~/pangs/SOURCE_PLANNING.md`, including its explicitly blocked forms and the
cross-repository development command. Its end-to-end measurements are in
[the source-obligations corpus comparison](PANGS_SOURCE_OBLIGATIONS_SURVEY.md).
This document remains the broader design and coverage plan; the problem and
failure-mode sections below describe the original pipeline. The existing
facts/policy/materialization ownership in `~/pangs/DISPOSITION.md` remains the
starting point; the manifest must evolve to describe the additional source
obligations. `~/pangs/DESIGN_lite.md` remains the authority for the semantic
analysis and its supported-program assumptions.

## Retention and representation update

Tenjin does not pass C2Rust's `--preserve-unused-functions`: preserving those
functions retains unwanted unsafe code. PANGS now mirrors C2Rust's per-TU
declaration-dependency pruning, including references in constant-false branches,
types, initializers and cleanup attributes. This is distinct from both LLVM
reachability and blindly retaining every preprocessed declaration. Differential
tests compare the retained declarations with actual C2Rust output.

The fixed `tenjin-c2rust-default-v1` emitter profile is part of source-plan v2.
PANGS records concrete observations with source locations and the default
representation's type/initializer characteristics. Disposition checks those
obligations before choosing a strategy, without changing LLVM's `written` or
escape evidence. Read-only address uses support immutable storage via C2Rust's
const-address lowering; discarded writers contribute nothing. Retained direct
assignments, default types that are not `Sync`, and section-extracted
initializers can reject immutable storage and let the normal cascade try
localization. Unsupported materializer forms are planning constraints.

Logical declaration retention is distinct from physical editing of intermediate
C. PANGS removes discarded bodies, storage and dependent declarations when
localization requires it, but leaves harmless unused header types/prototypes
for C2Rust to prune. Deleting those spellings needlessly expands system headers
in Tenjin's refolder and is not part of the source-obligation contract.

Tenjin forwards exact C2Rust declaration bindings, including function-local
static scope. It no longer overrides disposition with source flags and has no
automatic `reproject` round trip. Materialization either publishes a validated
private copy or reports a contract violation, with no policy retry. The older
standalone PANGS artifact-demotion command is not part of this workflow.

Tenjin drives PANGS's requirements and supplies both LLVM 14 and LLVM 21.
Source extraction uses Clang 14's C++ AST APIs in a linked shim inside PANGS,
alongside its existing LLVM 14 dependency. The implementation needs semantic
initializers and `TypeLoc` information not exposed by libclang's C API. A separate
extractor executable and an extraction interchange format are unnecessary
for this design.

## Problem

Tenjin localizes selected mutable globals by placing them in `struct
XjGlobals` and threading `struct XjGlobals *xjg` through the functions that
need them.  PANGS currently derives the initial function and callsite rewrite
plan from `linked_module.bc`.  Tenjin then applies that plan to the C source:

* it adds `xjg` parameters to selected function declarations and definitions;
* it changes selected function-pointer types; and
* it inserts `xjg` at the callsites supplied by the plan.

This assumes that the LLVM module and the source contain the same relevant
functions and calls.  That assumption is false, even though bitcode is built
at `-O0`.

For example, libtommath has source such as:

```c
if (sizeof("""S_MP_WORD_TOO_SMALL_C""") == 1u) {
    err = s_approx_log(a, b, &n);
} else {
    err = s_approx_log_d(a, b, &n);
}
```

Clang constant-folds the condition while lowering C to LLVM. The branch
containing `s_approx_log` is unreachable and that internal function is not
emitted into the module. Its source nevertheless remains in the preprocessed
C that Tenjin subsequently rewrites.

This is not caused by the final `opt` invocation.  For the libtommath input,
`s_approx_log` is absent from all of these modules:

| Stage | `s_approx_log` present |
| --- | --- |
| Clang `-O0 -Xclang -disable-O0-optnone`, before `opt` | no |
| after `opt -passes=mem2reg` | no |
| after `opt -passes=mem2reg,gvn` | no |

Removing `gvn`, or even omitting `opt` entirely, therefore cannot make the
LLVM plan complete with respect to the source.  It may also discard useful
semantic analysis quality for no benefit to this problem.

## Observed failure modes

The source/IR mismatch has already appeared in several forms.

### Missing direct-call rewrite

`s_mp_fp_log` and `mp_div` acquired `xjg` parameters because they were selected
from LLVM, while calls to them within source-only `s_approx_log` were absent
from the PANGS callsite plan.  The rewritten C consequently had argument-count
errors.

### Source-only global user

`s_warray_free` was source-only dead code.  It accessed a localized global and
was rewritten to use `xjg->...`, but it had not been selected as a context
function and therefore lacked an `xjg` parameter.

### Missing indirect-call rewrite

In libtommath's source-only disabled multithreading path, this type is changed
because its field can point at selected context functions:

```c
struct test_fn {
    int (*fn)(struct XjGlobals *);
};
```

But the dead-code call was not rewritten:

```c
tinfo->ret = tinfo->t->fn();
```

Clang diagnoses this as "too few arguments to function call, expected 1, have
0."  It then exports an incomplete AST for the erroneous assignment, and
c2rust later panics while assuming the binary expression has both children.
The c2rust panic is secondary; the generated C is invalid first.

There is a more general variant: a source-only function `X` may *only* call an
indirect context-taking function.  If PANGS cannot see that edge, `X` itself
will not be selected for context and has no `xjg` to pass.  Rewriting only the
indirect call is insufficient.

Tenjin's former `close_context_functions_over_source_calls()` supplemented
the LLVM plan with source global users and direct callers. That handles part
of the mismatch, but does not establish indirect-call and callable-type
closure. Its cases and tests must carry over into the PANGS implementation.

## Responsibility split

| Responsibility | Owner |
| --- | --- |
| Preprocessing, static-name uniquification, compilation database, bitcode build, source snapshot | Tenjin |
| Clang source-fact extraction and source/LLVM identity mapping | PANGS |
| Semantic analysis, source rewrite closure, safety checks, candidate recipes | PANGS analysis |
| Disposition selection and projection of the chosen recipes | PANGS disposition |
| Concrete text edits, wrapper emission, context construction, validation, materialization records | Tenjin |

PANGS already computes a context fixed point and checks unknown callers,
unresolved callee alternatives, and initializer-address dependencies. Extend
that planner with source facts so these checks cover every required edit.
The completed plan must include retained source-only functions and calls, even
when they have no LLVM counterpart. Discarded declarations contribute no
obligations; selected localization recipes prune them from intermediate C so
their old signatures and global references cannot invalidate C validation.

Tenjin's supported transformations must be an explicit part of the planning
contract: changing an internal signature, cloning a typedef, or introducing
an adapter each needs an implementable recipe. This can be a shared,
versioned contract between the two projects; it does not require a general
plugin or capability-negotiation system. Tenjin must reject unsupported
recipes rather than invent additional analysis during materialization.

## Integrated source frontend

A proposed CLI entry point is:

```text
pangs analyze linked_module.bc --source-compdb compile_commands.json ...
```

The source database must describe the post-preprocessing, post-uniquification
snapshot that Tenjin will copy into the localization stage. Tenjin must pass
the database for the analyzed target, together with the effective compilation
settings used by its bitcode builder. That builder currently modifies the
commands, including selecting LLVM 14, overriding optimization, and loading
`clang.cfg`; the database alone is therefore not a complete build record.

Record normalized TU identities, source content hashes, relevant target,
language and ABI options, compiler versions, and the linked-module hash.
Parsing must honor command working directories and preprocessed input mode.
A copied stage directory may have a different absolute path; match it by
normalized identity and content. Multiple command variants for one file must
remain distinguishable or produce an explicit unsupported-input error.

Implement extraction in a small internal module or crate such as
`pangs-source`. Parse each TU once, copy the required facts into owned data,
and release its Clang AST before retaining the next one. Run closure over
those facts with a worklist; repeated whole-project parsing is unnecessary.

The implementation uses a linked C++ shim for nested declarator structure,
semantic initializers and `TypeLoc` relationships that the C API does not
expose. The existing `xj-find-fn-ptr-decls` code is a
useful source of patterns and regression cases, but its current extraction
coverage must not be treated as a completeness guarantee.

Parse failures prevent a source-complete result. Unsupported operations must
be represented explicitly with their affected scope; an empty flow set must
never mean both "proven empty" and "not modeled." Resource exhaustion must
also produce an incomplete result or an error, never a partial accepted plan.
Bitcode-only analysis may remain available, but its recipes must be
distinguishable from source-complete recipes and must not satisfy Tenjin's
new localization contract.

## Two representations with different purposes

The LLVM solution describes runtime behavior under PANGS's existing analysis
assumptions. It continues to supply points-to information, call targets,
escape and violation facts, and semantic mutability evidence.

The source constraint graph describes obligations imposed by the C that
Tenjin will emit. It includes eliminated branches and IR-omitted functions
retained by C2Rust, their declaration occurrences, and retained initializers. A call in such a branch
can require a signature edit without constituting a runtime call edge.

Combine these representations in the localization planner. Do not inject all
source edges into the runtime call graph or pointer solver: that would make
dead code contaminate semantic facts and discard useful LLVM precision.
Conversely, LLVM's finite live target set cannot erase a syntactic producer
or consumer that remains in the source.

Source entities need identities independent of LLVM emission. Use declaration
identity and linkage for functions and globals, enclosing declaration plus
position for parameters and fields, and source-file identity plus an AST
occurrence/byte range for individual uses. Preserve all redeclarations and
join declarations across TUs explicitly. Existing qualified manifest keys
remain the identities of disposition subjects.

Map LLVM facts onto these identities where the correspondence is established.
Debug line/column information alone is insufficient to distinguish nested
calls or multiple IR operations attributed to one source expression. Missing
LLVM counterparts are expected for source-only entities; ambiguous matches
must remain unresolved rather than being attached to an arbitrary AST node.

## Source graph and closure rules

### Separate value flow, type constraints, and context requirements

The graph needs distinct entities for:

* functions, their declarations/definitions, and direct or indirect callsites;
* function-pointer values and storage slots, including fields, variables,
  array elements, parameter positions, and return positions;
* type occurrences and shared declarations that constrain how those values
  can be spelled and passed; and
* globals, initializers, and owned backing storage.

Equal or compatible function types alone do not establish value flow.
Unrelated slots with identical signatures must not be merged on that basis.
Shared field declarations and cross-TU redeclarations do impose common type
edits. A shared typedef may be cloned for affected uses when the materializer
supports doing so.

Collect direct calls and global references, indirect callee expressions,
function-address producers, assignments, initializers, argument/parameter
bindings, returns, conditional expressions, and field/array transfers.
Pointer indirection, aggregate copies, casts, and unsupported storage flows
need conservative constraints or explicit incompleteness. Simply walking
direct function-name references is insufficient.

Track "needs an `xjg` argument" separately from "contains a changed callable
type." For example, a function that only forwards a callback can need a
changed parameter or return type without needing its own context argument.
Nested callback types must propagate through higher-order signatures too.

### Compute a fixed point for each localization candidate

1. Seed the candidate from source uses of the global and its storage closure,
   together with applicable LLVM-derived rewrite obligations. Record
   initializer obligations separately from uses in function bodies.
2. A function that needs the context requires edits to all its declarations
   and calls. Its ordinary callers also need the context. `main` provides
   the context through its construction recipe and retains its entry ABI.
3. A changed function value constrains every slot it can flow into. A changed
   slot constrains all of its producers, transfers, and indirect calls.
   Shared type edits can introduce obligations at additional occurrences.
4. A context-taking indirect call requires its enclosing function to provide
   `xjg`. Propagate this requirement back through callers.
5. Continue until function requirements, callable types, producer adaptations,
   call edits, and storage obligations stop changing. Check safety over the
   entire resulting closure.

Calls and references in retained unevaluated expressions also require valid
types and names. Classify their evaluation context so satisfying their syntax
does not accidentally introduce runtime evaluation or hide a scope problem.

Every obligation carries its originating global(s) and a reason edge. This
supports deterministic diagnostics and identifies which candidates depend on
an unsupported edit.

### Every target of a changed slot must have the chosen signature

For example:

```c
fp = condition ? needs_context : ordinary_function;
```

Changing `fp` and its calls also requires adapting `ordinary_function`, even
if it has no global uses and LLVM never observes that alternative. The
initial implementation should prefer changing every eligible internal
producer to the common signature, with an unused context parameter where
necessary. This extends PANGS's existing rule for mixed indirect-call targets.

Where an original ABI must remain, an adapter requires an explicit supported
recipe. Preserve function-pointer equality and identity-sensitive uses; do
not generate independent wrappers at occurrences without checking their
observability. Variadic forwarding and adapters that would need to recover
an unavailable context are unsupported unless separately implemented.
Unknown or unadaptable alternatives block the affected localization.

Use consistent choices for shared nodes so per-global recipes compose. Check
the combined selected plan for conflicting type edits and adaptations before
emitting it. Independent wrapper recipes cannot simply be concatenated.

## Safety checks over the completed source closure

Source closure can reach a boundary outside the original LLVM rewrite slice.
For example:

```c
#include <stdlib.h>

static int g;
static void callback(void) { if (0) ++g; }
int main(void) {
    ++g;
    atexit(callback);
    return 0;
}
```

LLVM can eliminate the callback's access to `g` while retaining the callback
registration. Source closure would then require a new parameter on a function
whose external caller cannot supply it. The source-complete candidate for
`g` must report this blocker, even if the IR-only candidate passed.

Carry PANGS's unknown-caller, unknown-callee, access-completeness, escape, and
violation evidence into the extended check. Audit source-only flows and
unsupported constructs as well. External callback contracts, exports,
constructors/destructors, signal and thread entries, and opaque or variadic
transfers must be considered wherever a signature or storage rewrite reaches
them. An ordinary outbound external call remains harmless when its ABI and
the relevant storage obligations are unaffected.

`main` being the construction boundary is not a lifetime proof. The context's
storage duration and initialization timing must cover every permitted use;
entry points before construction and uses after its lifetime need a supported
recipe or a blocker. Keep existing accepted-risk assumptions visible and
scoped; source-only additions must not silently inherit a safety proof that
covered a smaller rewrite slice.

Explicit casts can conceal incompatible calls from Clang. Successful C
type-checking therefore supplements these checks; it cannot establish ABI
or lifetime correctness by itself.

## Initializers and storage closure

Source dependencies outside function bodies are part of feasibility. Consider:

```c
static int g;
static int *p = &g;
```

If `p` is used only by omitted code, it can disappear from LLVM while its
source initializer survives. Replacing `&g` with `&xjg->g` at file scope is
invalid. A function-local static initializer has a similar constant-expression
obligation; adding a context parameter to the enclosing function does not
resolve it.

Record these dependencies even when the initializer's owner has no LLVM
counterpart. Until an explicit recipe can relocate or reconstruct the entire
dependency correctly, block localization of `g`. Do not silently add `p` to
the selected globals to make the edit possible.

The same rule covers anonymous backing objects and `storage_members` from the
manifest. Moving an owner must preserve its complete storage closure, alias
relationships, initialization order, and storage duration. Unsupported shared
storage remains a witnessed blocker.

## Semantic immutability and emitted Rust mutability

Keep semantic evidence separate from the requirements of the emitted source.
LLVM may establish that a global is never written during execution while
retained C still contains a write in dead code. Translating that write against
an immutable Rust static fails Rust type-checking. Likewise, a
section-extracted initializer becomes a generated assignment and requires
mutable storage.

Record retained source writes as representation constraints, without
reclassifying them as runtime writes. Tenjin must retain a mutable Rust
representation where required, or use a separately justified transformation
that removes or rewrites the offending source. When this prevents executing
an `immutable` disposition, record the materialization demotion while
preserving the original semantic evidence and cascade result.

Absence from the disposition manifest is not evidence that optimization
eliminated a global. Distinguish source-only definitions, manifest exclusions,
unmapped symbols, and missing dispositions from positively established
semantic properties. Replace the current `source_globals - assigned_names`
immutability inference with explicit evidence and conservative handling of
unknown cases. A source-only definition is not automatically eligible for
either immutable representation or localization.

## Manifest ownership and materialization

Evolve the manifest with an explicit schema/plan version. Existing v8 IR-only
recipes must not be interpreted as source-complete plans. Preserve the
ownership split in `DISPOSITION.md`:

* Analysis owns input provenance, source completeness evidence, per-global
  candidate recipes, and all source/semantic blockers. Extend
  `context_rewrite.fields` to include the necessary source declarations,
  callable type changes, adaptations, calls, and initialization obligations.
* Disposition owns strategy choices and `context_rewrite.selected`, whose
  fields exactly match the globals finally chosen for localization. Source
  feasibility participates in candidate eligibility before selection.
* Tenjin owns materialization records: concrete edits and their outcomes,
  generated symbols, validation results, and demotion witnesses. Analysis
  facts, candidate recipes, `cascade_chosen`, and `cascade_trace` remain
  unchanged.

Use source identities and pre-edit anchors tied to the recorded snapshot.
Tenjin verifies every anchor and expected declaration/type before editing.
Byte offsets are evidence for this snapshot, not durable symbol identities.
Missing, ambiguous, overlapping, or stale edits are errors.

Apply edits in a staged copy and publish the stage only after validation.
If an implementation limitation prevents materializing a candidate, record
the existing `unhandled` demotion with a witness and remove every selected
global depending on the failed obligation. Regenerate the effective
selection and its plan through PANGS's shared planner from the original
snapshot, and repeat until stable. Keep the pre-demotion decisions as history
and the final selected projection consistent with final choices. Preserve
the existing whole-group demotion rules for joint representations.

This regeneration is a selection/plan operation over the surviving choices;
it does not rerun the disposition cascade and undo the demotions. Discard
staged edits from a failed attempt and materialize the regenerated plan from
the original source snapshot.

A stale input or an unexplained validation failure aborts the stage; it must
not be hidden by arbitrary demotions or partial edits. No path promotes a
disposition, clears analysis uncertainty, or continues from a damaged AST.
Regeneration must follow the manifest's existing downstream-invalidation
rules so re-disposing invalidates previous materialization.

## Validation and regression coverage

Before invoking c2rust, require a clean parse/type-check of every rewritten TU
under the compiler versions and flags required by Tenjin. Reject all C errors
and incompatible-function-pointer diagnostics even if configured as warnings.
Also check cross-TU declaration/type consistency explicitly: separate TU
compilation and ordinary linking do not guarantee matching signatures.

Validate that every planned declaration, type, and call edit was materialized,
no temporary context placeholders remain, and no unhandled references to
removed global definitions survive. Compile the resulting Rust as well as
the C. Clang diagnostics are validation evidence, not the closure algorithm.

Focused tests must cover:

* the libtommath direct-call, global-user, and indirect-call failures, including
  chains whose only context requirement comes from an indirect call;
* fields, arrays, local pointer variables, typedef cloning, redeclarations,
  parameter/return flows, nested callable types, and cross-TU transfers;
* mixed context/ordinary targets, pointer copies and aggregate copies, casts,
  adapters, and function-pointer comparisons;
* source-only external callback uses, unresolved alternatives, unsupported
  variadic flows, and lifecycle boundaries, with specific blocker witnesses;
* source-only file-scope and function-static initializer dependencies,
  anonymous backing storage, and retained dead writes to immutable candidates;
* parse failures, incomplete extraction, ambiguous source/IR mappings, stale
  source hashes, missing dispositions, and conflicting shared edits; and
* composition of per-global recipes, demotion and replanning, reproducible
  output, and preservation of analysis and cascade history.

For successful transformations, compare observable behavior before and after
rewriting, exercising both ordinary and context-taking indirect targets.
For rejected cases, assert the blocker and absence of partially published
output. Include examples that compile despite a bad explicit cast so that
passing compiler diagnostics cannot substitute for the boundary checks.

## Implementation sequence

1. Define the integrated frontend API, source identities, completeness states,
   input provenance, and the versioned planner/materializer contract. Add
   source inventory and direct-call/global-use extraction inside PANGS.
2. Extend the existing PANGS planner to cover source-only direct calls and
   global uses, initializer obligations, and safety boundaries. Compare its
   results against the current Tenjin source supplement during migration.
3. Add callable value/storage constraints and distinct type-edit constraints,
   including all producers, indirect callers, nested types, and cross-TU
   declarations. Compute closure with a worklist; block unsupported flows.
4. Extend candidate recipes and the selected manifest projection. Validate
   recipe composition and integrate source feasibility into disposition.
   Replace absence-based immutability guidance with explicit evidence and
   source representation constraints.
5. Make Tenjin consume the complete plan, including supported adaptations,
   initializers, and the context construction recipe. Add staged validation
   and demotion/replanning before retiring its independent source closure
   and speculative higher-order type repair.
6. Run the focused regressions and full Tenjin suite, then validate libtommath
   and broader inputs. Measure additional parse/closure time, peak memory,
   source-only obligations, and new blockers alongside localization coverage.

During migration, the old helpers may remain as diagnostic comparisons, but
the production path must have one authority for closure. This work does not
require LLVM to retain dead code, a second source points-to solver, or a
standalone extraction service. Preserve LLVM analysis quality while making
every accepted source rewrite complete and executable by Tenjin.
