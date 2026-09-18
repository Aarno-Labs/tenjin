# Retention-aware source obligations: corpus comparison

Survey date: 2026-09-19. This follows the historical
[reproject survey](PANGS_REPROJECT_SURVEY.md); it tests the replacement policy,
not merely removal of the subprocess call.

## Result

PANGS now checks the default C2Rust representation during disposition, using
the declarations C2Rust retains without `--preserve-unused-functions`. LLVM's
runtime facts are preserved. Tenjin forwards the selected declaration bindings
and materializes one finalized plan; it does not demote or reproject.

The five globals affected by the old policy have been checked in their actual
corpus projects, including the final compiled Rust:

| Project / global | Initial disposition now | Emitted result and reason |
| --- | --- | --- |
| `p0`: `stb__perlin_grad.basis` | immutable | Immutable `[[c_float; 4]; 12]` static. Read-only pointer access uses C2Rust's const-address lowering. Guidance preserves `stb__perlin_grad:basis`. |
| `synthetic_015`: `helperGood1.charString` | immutable | Immutable `[c_char; 19]` static. Returning its address does not require mutable storage. Guidance preserves `helperGood1:charString`. |
| `jumpnode`: `node_storage` | immutable | Immutable `[Node; 100]` static. C2Rust discards `add_node` and `initialize_test_data`; their assignments impose no emitted-storage obligation. |
| `guff`: `default_colorblind_safe` | localize | An `XjGlobals` field, passed through the context. Its default raw-pointer-containing array type cannot be an immutable Rust static. |
| `guff`: `default_colors` | localize | The same representation constraint and ordinary cascade fallback as the other palette. |

All four affected project tests pass. Jumpnode's unchanged unsafe-function
expectation now passes: three unsafe functions remain, versus four in the old
run. Guff's rendering-output checks pass with both palettes localized. Its
materialized manifest records **97 source edits and no demotions**. An
intermediate plan had 6,090 edits; preserving harmless header declarations
removed the unnecessary deletions without changing its selected strategies.

This is not a relocation of the old `requires_mutable_storage` predicate.
Three of its five demotions were unjustified; the other two now use the normal
strategy cascade instead of becoming `unhandled` after selection.

The exact declaration-binding fix also supplies immutable guidance for eight
function-local libspeex statics whose initial dispositions were already
immutable: `dradb3:taui`, `dradb3:taur`, `dradb4:sqrt2`, `dradbg:tpi_xjtr_2`,
`dradf4:hsqt2`, `dradfg:tpi_xjtr_1`, `drfti1:ntryh`, and `drfti1:tpi_xjtr_0`.
The libspeex test passes. These are guidance/materialization improvements, not
changes to its initial strategy choices.
Two libtommath local statics also receive their scoped bindings:
`test_mp_kronecker:kronecker` and `test_mp_sqrtmod_prime:sqrtmod_prime`.
Its Rust still compiles and matches the C test summary, but its existing
unsafe-count assertion still fails at 247 versus zero expected.

Across the whole corpus, eleven initial choices change. All eleven reject the
default immutable representation because it contains object raw pointers:

| New choice | Globals |
| --- | --- |
| localize (6) | Guff's two palettes; `G_OP_NAME` in the three macrodepth programs; c-markdown's `conv_xjtr_1` |
| unhandled (5) | Fribidi's `char_sets`, `fribidi_unicode_version`, `fribidi_version_info`; file-queue's `s_month`; static-vars-fpts' `keywords` |

The five `unhandled` records have no feasible remaining strategy. These are
initial disposition results, not consumer demotions. The three previously
unjustified demotions (`basis`, `charString`, `node_storage`) retain their
initial immutable choices and now reach the emitter correctly.

## Method

The corpus command is:

```sh
XJ_EXTRA_PREPARATION_PASSES=0 ./cli/10j pytest tests --also-slow -v -n 5
```

`XJ_PANGS_EXE` selects a temporary observational wrapper around the sibling
development binary. It records subprocess arguments, exit status, elapsed time,
and compressed copies of initial manifests; it forwards standard streams and
the original exit status. `PYTEST_ADDOPTS` requests JUnit output. No production
instrumentation or retry policy is installed.

The final PANGS binary was frozen throughout the completed census:

```text
0c057f270f628a63db677f4c17e4ee6f385e133e79acd7738d7c7e705320de74
```

The retention checkpoint commits are PANGS `00bc92a61e00` and Tenjin
`b77ab5d25f60`. The final PANGS implementation is `63ba8a2df567`; its Tenjin
consumer is the change containing this report. The baseline is the archived
full run described in the reproject survey, not a reconstruction from the
current code.

PANGS invocation, manifest and global measurements exclude the deliberately
constructed `tests/test_pangs_source.py` integration cases. Test outcome totals
include all collected tests. Global counts are manifest record observations,
not all C declarations. Comparisons use test and global keys, and compare the
initial manifests before any materialization.

| Measurement | Baseline | Replacement |
| --- | ---: | ---: |
| Collected tests | 419 | 432 |
| Passed / failed / skipped / xfailed | 369 / 32 / 15 / 3 | 383 / 31 / 15 / 3 |
| Corpus analysis invocations / completed manifests | 242 / 241 | 242 / 241 |
| Nonempty analyses / global record observations | 61 / 271 | 61 / 271 |
| Initially immutable / localize / atomic / unhandled | 93 / 75 / 39 / 64 | 82 / 81 / 39 / 69 |
| Analyses selecting localization | 19 | 23 |
| Natural reproject calls | 4 | 0 |
| Materialization retries | 0 | 0 |

Among common tests, the only outcome change is jumpnode's failure becoming a
pass. The remaining 31 failures reproduce the baseline categories: 14 unsafe
counts, eight executable-name mismatches, six experimental snapshot differences,
two Rust compilation failures, and one cross-TU source-signature rejection.
No assertion was relaxed. The signature rejection remains
`underhanded_c_nuke_lib`'s incompatible `spectral_contrast` declarations.

All 271 global keys match. LLVM fact payloads and certificates match exactly
after replacing each run's absolute analysis-directory prefix with the same
placeholder. Only source augmentation and localization planning are excluded
from that comparison; boolean values alone were not treated as sufficient.

The final census ran from 23:18 UTC on September 19 to 00:06 UTC on September
20; pytest reported 2,933.99 seconds. This is a correctness comparison, not a
controlled performance benchmark.

## Final-build verification

The final tests cover syntax C2Rust folds before declaration
retention: `_Generic` alternatives, `typeof` operands, constant array bounds,
enum values, bitfield widths, alignment operands and type-compatibility
predicates. Variable-length-array `sizeof` bounds remain evaluated when
required; a retained `sizeof(int[++g])` cannot be treated like `sizeof(++g)`.
All 242 corpus analyses ran with this final build. There were 241 successful
analyses and the one existing source-signature rejection. All 23 selected
localization plans passed source validation. Failure messages were compared
as well as test outcomes: the remaining unsafe-count assertions retain their
same actual/expected counts, and the launch, snapshot and compiler failures
remain the same categories. No source-plan materialization failure occurred.

The full PANGS workspace tests pass. All 19 cross-repository source tests pass,
including actual Rust compilation for the compile-time cases, and are included
in the final 432-test census. Python checks and Tenjin Rust checks pass; Tenjin's
Rust unit suites pass all 36 and 105 tests respectively.

An intermediate normal run exposed excessive physical pruning: deleting plain
unused header declarations made `clang-refold` reject `cmake_lone_exe`, and its
float-classification case consumed 47.7 GiB RSS before earlyoom terminated it.
PANGS now preserves harmless discarded type/prototype spellings in intermediate
C; C2Rust still logically discards them. Bodies, storage and declarations with
value-reference dependencies (transitively through types) still get pruning
edits. Both failures and all 19 source tests pass after this correction.

The normal Tenjin suite passes with extra preparation enabled: **178 passed,
254 skipped** in 282.13 seconds (`-n auto`, capped at five workers). Its expected
snapshot update changes `guided_static:u8` from `static mut` to `static` in the
initial Rust.

## Contract regressions

Targeted tests combine initial disposition assertions with actual C2Rust
translation and Rust compilation. They cover the five source patterns above,
retained constant-false assignments, address-only access, joined declarations,
runtime-lowered static initializers, and function-pointer types that are Sync.
LLVM `written=false` is asserted independently of the representation choice.

Retention tests compare PANGS's retained function set with actual C2Rust output.
They include declaration dependencies in dead branches, cleanup attributes,
exported initializers, and `used` declarations. Localization tests verify that
discarded writers and callback initializers are pruned before signatures and
storage are rewritten. Overlapping typedef/tag declarations and trailing
typedef attributes have explicit regressions.

Disposition tests reject unavailable source representations even for an
accepted-risk override: an override cannot manufacture an emitter recipe.
Tenjin fault-injection tests verify a single failed materialization attempt
reports a contract violation and leaves the original source and manifest
unchanged.

## Evidence and limits

The final run's [artifact directory](/tmp/pangs-source-final-KLP4Pvav)
contains `run.json`, `pytest.log`, `junit.xml`, `events/`, `comparison.json`,
`choice-details.json`, `guidance-details.json`, and
`runtime-fact-comparison.json`. `failure-comparison.json` preserves old/new
failure messages and traces. `normal.xml` records the passing normal run.
`inputs/` preserves compressed source snapshots from successful analyses.
`subjects/` preserves the five globals' initial and materialized manifests,
guidance, source, and final Rust, plus the other changed-choice projects,
libspeex and libtommath. This protects the inspected evidence from pytest's
temporary-directory rotation. The old evidence remains in
[/tmp/pangs-reproject-survey-dqoMiEoj](/tmp/pangs-reproject-survey-dqoMiEoj).
Earlier development evidence is retained in
[/tmp/pangs-disposition-implementation-7qYAk3kc](/tmp/pangs-disposition-implementation-7qYAk3kc).
It includes the first completed census (380 passed, 31 failed), an intermediate
242-analysis replay, and the 15 affected-project reruns. Those measurements are
not substituted for the final census reported above.

An initial, interrupted attempt exposed overlapping pruning ranges for typedef
and nested tag declarations, and a trailing-attribute extent problem. It was
stopped, archived under `aborted-overlapping-pruning/`, fixed with regressions,
and restarted from the beginning. It is excluded from the completed census.

During the earlier development run, the system's `earlyoom` monitor terminated
several launchers, an archive process and an overlapping normal test run.
`clang-refold` alone reached about 22.7 GiB RSS. The census and affected-project pytest processes
survived their launchers and completed, producing the full JUnit files and
summaries retained in that earlier artifact directory. Its `run.json` therefore
retains launcher exit 143, with completed pytest evidence recorded separately.
The final census ran without overlapping heavy validation, was not interrupted,
and has the ordinary pytest exit 1 for its 31 known failures. The intermediate
normal-run pruning regression was fixed and retested, not dismissed as an
environmental failure.

The disabled-extra-passes snapshot diff was archived and reversed, restoring
the exact snapshot state verified by the passing normal run. Rebuildable Cargo
output from completed tests was cleaned to recover disk space; C, generated
Rust, manifests and logs were retained.

This profile describes Tenjin's default C2Rust representation. Explicit user
type/mutability guidance remains an override outside that guarantee. Source-only
globals are not silently promoted into LLVM disposition subjects. Existing
multi-target analysis exclusions and fail-closed cross-TU signature checks
remain in force. The contract does not imply that all corpus translations are
safe or that all existing unsafe-count expectations pass.
