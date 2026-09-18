# PANGS reproject corpus survey

Survey date: 2026-09-19. This is an observational report, not a policy change.

Interpretation correction from the subsequent emitter investigation: the
`node_storage` writers described below as "retained" are present in preprocessed
C but are **discarded by C2Rust's default declaration pruning**. They impose no
obligation on emitted storage. `basis` and `charString` also did not require
mutable storage: their read-only pointer uses are supported by const-address
lowering. Thus the observed demotions should not simply be relocated upstream;
the predicate itself was too strong. The original measurements below remain
historical evidence about that implementation.

## Conclusion

`pangs reproject` ran naturally **four times**, demoting **five globals** from
`immutable` to `unhandled`. Every decision used a source-representation fact
already present in PANGS's initial manifest. There were **no materialization
failure/retry calls**, and no selected localization plan changed.

Thus **100% of the observed demotions could have been handled before PANGS
published its initial result**. Their observed effect was to correct manifest
choices and record demotion provenance, not to change the generated program.
Bypassing these four calls preserved guidance and test outcomes; the generated
Rust was identical except for independently explained test-directory paths and
one offset-derived temporary name in `guff`.

The evidence supports moving the immediate source-representation decision into
initial PANGS finalization. It does not demonstrate a need for a separate public
reprojection round trip in the current pipeline. This is not a claim that future
materializers cannot discover genuinely new limitations.

## Method and coverage

Ran the requested command:

```sh
XJ_EXTRA_PREPARATION_PASSES=0 ./cli/10j pytest tests --also-slow -v -n 5
```

Additional environment selected the current sibling PANGS implementation and
enabled temporary observation:

```sh
XJ_PANGS_EXE=/home/brk/pangs/target/debug/pangs
XJ_PANGS_REPROJECT_SURVEY_DIR=/tmp/pangs-reproject-survey-dqoMiEoj
PYTEST_ADDOPTS=--junitxml=/tmp/pangs-reproject-survey-dqoMiEoj/junit.xml
```

Tenjin's provisioned PANGS pin was older than the source-planning implementation,
so using the sibling executable was necessary. Provenance:

- Tenjin parent revision: `96f775f3`; PANGS parent revision: `d5faf733`.
- PANGS executable SHA-256:
  `41623fa7e9e391d4c7c97201880de042655a837d384ea07609888c03c1348988`.
- Source analysis/bitcode used LLVM 14; Tenjin's LLVM 21 pin was
  `21.1.8+refold@rev-4f8fe4bef`.
- Run: 04:22:16–06:12:04 UTC; pytest reported 6,583.43 seconds.

Instrumentation recorded analysis eligibility, original manifests, before/after
reprojection manifests, failure witnesses, source snapshots, emitted guidance,
materialization attempts, and pytest outcomes. No recording errors appeared in
the run log. An independent scan of initial manifests predicted exactly the
observed four calls and five demotions.

| Measurement | Observed |
| --- | ---: |
| Collected tests | 419 |
| Passed / failed / skipped / xfailed | 369 / 32 / 15 / 3 |
| Corpus PANGS invocations / completed analyses | 242 / 241 |
| Completed analyses containing global disposition records | 61 |
| Global disposition record observations | 271 |
| Initially selected immutable / localize | 93 / 75 |
| Analyses selecting localization / materialization attempts | 19 / 19 |
| Materialization limitations triggering retry | 0 |
| Natural reproject calls / affected globals | 4 / 5 |
| Changes to selected projection, candidate recipes, facts, or cascade history | 0 |

Counts exclude the deliberately injected `tests/test_pangs_source.py` cases,
which exercised two additional reproject calls. Global counts are manifest
record observations, not a census of all source declarations or distinct globals
across the corpus. Constants and declarations absent from those records are not
included.

Coverage is also limited by Tenjin's existing single-target restriction:
18 tests produced 33 multi-target skip events. One eligible analysis rejected
inconsistent source signatures before producing a manifest. Consequently this
is a survey of the entire requested test run, not evidence about reproject on
every build target that Tenjin currently skips.

## What actually changed

All five records had `source_representation.mapped=true` and
`requires_mutable_storage=true` in the original manifest. All four calls had an
empty selected localization plan both before and after reprojection.

| Test/project | Demoted global(s) | Retained source use | Initial localization verdict |
| --- | --- | --- | --- |
| `test_tractor_ta3_corpus_p0_app` | `stb__perlin_grad.basis` | Static lookup array decays to `float *grad`; reads through that pointer | `ok` |
| `test_tractor_b1_synthetic_015_app` | `helperGood1.charString` | Returns a pointer to a static character array | `ok` |
| `test_tractor_b2_synthetic_jumpnode_lib` | `node_storage` | Assignments in retained, unused static helper `add_node` | blocked: `source-missing-main` |
| `test_silentbicycle__guff` | `default_colorblind_safe`, `default_colors` | Palette arrays selected through a local `char **palette` | `ok` for both |

The first two keys are in `src/main.nolines.i`, the third in
`src/lib.nolines.i`, and the last two in `args.nolines.i`. Full keys and witnesses
are in [demotions.json](/tmp/pangs-reproject-survey-dqoMiEoj/demotions.json).

These are not five instances of retained dead writes. Four are pointer/address
representation cases under the current conservative source predicate; only
`jumpnode` has the retained dead writer. Its `add_node` helper is absent from
linked bitcode, explaining the difference from the runtime `written=false` fact.
Reanalyzing the same inputs with `--build-mode library` still chose `immutable`
with the mutable-source flag, so changing build mode alone does not remove this
particular discrepancy.

The manifest diffs changed `disposition.chosen`, demotion/provenance records,
and materialization validity. They did not recompute semantic facts or rerun the
cascade. The existing `run.dispose.measurement_report` still describes the
original policy result; this survey counts actual per-global choices instead of
treating that historical aggregate as a post-demotion census.

Measured reproject time was **0.257 seconds total**, at most **0.102 seconds per
call**. This includes temporary failure JSON, subprocess execution, and manifest
reload, but not observation snapshot compression. It is not a controlled
performance benchmark. The main issue is ownership and complexity, not a
material corpus runtime cost.

## Counterfactual: leave the manifests unchanged

Reran all four affected tests with the same PANGS executable behind a wrapper
that delegated analysis normally but made `reproject` copy its input manifest
unchanged. The wrapper rejected any other failure kind. Extra preparation passes
remained disabled.

Results: **three passed, one failed**, exactly as in the original run. `jumpnode`
failed the same unsafe-function-count assertion, **4 versus 3 expected**.

- Emitted guidance was identical in all four cases.
- Generated Rust was byte-identical in `p0`, `synthetic_015`, and `jumpnode`.
- `guff` had 14 embedded assertion-path substitutions across six Rust files due
  to the different pytest root. Its only other difference was one local binding
  and use, `__lift_1_14615_8` → `__lift_1_14702_8`. The name generator uses the
  source span offset; three preceding paths each grew by 29 bytes, accounting
  exactly for the 87-byte offset change. With those explicit substitutions,
  all 12 generated Rust files match. Raw diffs are retained; this is not a claim
  of byte identity for `guff`.

See [comparison.json](/tmp/pangs-reproject-survey-dqoMiEoj/counterfactual/comparison.json)
and the exact [guff audit](/tmp/pangs-reproject-survey-dqoMiEoj/counterfactual/guff-audit.json).

There is a direct code explanation: Tenjin's
[guidance helper](cli/translation_preparation.py) independently excludes
`requires_mutable_storage` globals from immutable guidance and adds mutable
storage guidance. Changing their chosen disposition therefore does not change
that output. None of these five records had an explicit `vars_mut` override.
Also, PANGS's selected context rewrite depends on `localize` choices, not on
whether a nonlocalized record says `immutable` or `unhandled`.

## How much belongs upstream?

### All observed calls

PANGS already runs `pangs_source::augment_manifest` before `apply_policy` in
[`pangs-cli/src/main.rs`](/home/brk/pangs/crates/pangs-cli/src/main.rs).
The source layer has already computed the exact predicate that Tenjin's
`demote_source_immutable_representations` subsequently reads. No additional
Clang pass or materialization result is needed to make these five decisions.

Two possible changes should not be conflated:

1. **Preserve current behavior:** apply known source-representation exclusions
   during initial PANGS finalization, before publishing the manifest and selected
   plan. Keep the same five `unhandled` outcomes and their evidence, without the
   Tenjin→PANGS subprocess round trip.
2. **Change selection policy:** make source representation a strategy eligibility
   condition and continue the cascade when `immutable` cannot be represented.
   Four of these five globals already have `localization.verdict=ok`, so they
   could become localization candidates instead of being forced to `unhandled`.
   That would be a substantive change, not an equivalent cleanup. This survey
   did not materialize or validate those alternative selections.

Keep LLVM semantic immutability separate from source/backend representation
feasibility: the source pointer uses do not establish runtime mutation. There
may also be room to relax the conservative representation predicate, but this
run does not establish the safety of doing so.

### The unused late-retry path

The other production caller is
[`localize_mutable_globals`](cli/c_refact.py), which catches `UnsupportedRecipe`
and retries from an unchanged source snapshot after demoting selected fields.
It was never exercised naturally in this run.

The sole current production `UnsupportedRecipe` raise concerns joined global
declarations sharing a declaration start. That is AST-visible before
materialization, and Tenjin already runs `split_joined_decls` before analysis
even with extra preparation passes disabled. Any residual unsupported shape
could be normalized there or blocked while PANGS constructs candidate recipes.

This supports retaining an internal helper to compose the selected plan from
candidate recipes while questioning the separate public retry protocol for
today's limitations. Do not simply remove validation: stale source snapshots
and unexplained C validation failures currently abort, not successfully recover
through reprojection. Future genuinely backend-specific discoveries would need
an explicit contract.

One cleanup hazard: missing source facts currently default to rejection in the
demoter but not in the guidance filter. No missing-fact case occurred here.
Moving/removing the demoter must preserve fail-closed handling rather than
assuming that duplicate filtering is equivalent for malformed/incomplete inputs.

## Failures and interpretation limits

The full run **did not pass**. The 32 failures break down as:

| Observed failure category | Count |
| --- | ---: |
| Unsafe-function-count assertions | 15 |
| Executable-name mismatches (`*_nolines` versus harness expectations) | 8 |
| Snapshot differences | 6 |
| Rust compilation failures (`float_classification_macros`, `silentbicycle__skel`) | 2 |
| PANGS source signature rejection | 1 |

The rejected project was `underhanded_c_nuke_lib`: `spectral_contrast` is declared
using `float_t`, but one translation unit defines that type as `double` and
another as `float`. The diagnostic was `cross-TU signature mismatch:
spectral_contrast`. This is an upstream analysis rejection, not a reproject call.

`libtommath` also illustrates the distinction. Its 163 translation units and
5,882 source calls produced nine global records: five immutable and four
unhandled, with no selected fields and no reproject. Source blockers included
retained thread callbacks and opaque aggregate flow. The Rust binary compiled
and its exit status/test summary matched C, but the test failed its unsafe-count
expectation, 247 versus 0. That is upstream refusal of localization, not evidence
that reproject rescued a large transformation.

These categories describe observed failures; they do not establish that every
failure is new, caused by source planning, or caused by disabling extra
preparation passes. Of the four reproject-affected cases, only `jumpnode` failed,
and bypassing reproject preserved that failure exactly.

## Artifacts and workspace state

The durable conclusion is this report. Detailed local artifacts are under
[/tmp/pangs-reproject-survey-dqoMiEoj](/tmp/pangs-reproject-survey-dqoMiEoj),
which is temporary storage and should be copied elsewhere if long-term retention
is required:

- [Run metadata](/tmp/pangs-reproject-survey-dqoMiEoj/run.json),
  [full pytest log](/tmp/pangs-reproject-survey-dqoMiEoj/pytest.log), and JUnit XML.
- [Summary](/tmp/pangs-reproject-survey-dqoMiEoj/summary.json),
  [analysis/failure details](/tmp/pangs-reproject-survey-dqoMiEoj/detail.json),
  before/after manifests, source snapshots, and analysis scripts.
- [Instrumentation patch](/tmp/pangs-reproject-survey-dqoMiEoj/instrumentation.patch)
  and [experimental snapshot diff](/tmp/pangs-reproject-survey-dqoMiEoj/experiment-snapshots.patch).
- Counterfactual run logs, generated-Rust content archives, and the separate
  `jumpnode-library-mode` diagnostic.

To avoid exhausting disk during the run, 91 explicitly identified, rebuildable
Cargo target directories from already-completed tests in this run were cleaned.
Source, manifests, metadata, and diagnostic logs were retained; the removed build
products can be rebuilt. Exact cleanup records are in `socat-cargo-clean.json`
and `completed-cargo-clean.json` in the artifact directory.

Temporary instrumentation and nondefault-run snapshot updates have been
restored. No production policy or PANGS source change is part of this survey.
Post-restoration `./cli/10j check-py` passed. The required normal
`./cli/10j pytest tests -n auto` verification also passed: **165 passed,
254 skipped**, in 349.42 seconds. That run used the same sibling PANGS executable,
with the extra-preparation override and survey instrumentation flag unset.
Its [separate log](/tmp/pangs-reproject-survey-dqoMiEoj/default-check/pytest.log)
and run metadata are in `default-check/`. No snapshot changes resulted.

Final workspace audit: only this report is changed in Tenjin; PANGS has no
working-copy changes.
