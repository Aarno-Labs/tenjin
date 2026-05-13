//! Step 6: emit text edits for a `LiftPlan`.
//!
//! Operates on raw source text plus `TextRange`s. Doesn't depend on
//! rust-analyzer beyond `TextRange` and `TextEdit` for the change set.

use std::collections::BTreeMap;

use ra_ap_syntax::TextRange;

use crate::lift::{LiftPlan, LiftStrategy, PlannedLift};
use crate::text_edit::{TextEdit, TextEditBuilder};

/// Where to splice the `let` statements introduced by lifting.
///
/// Per spec Step 6: "If the call is in a position that requires a single
/// expression (e.g., it *is* the function body), wrap in a block as shown.
/// Otherwise, emit the `let`s as preceding statements and the rewritten
/// call in place."
#[derive(Debug, Clone, Copy)]
pub enum InsertSite {
    /// Insert `let`s before this position (statement context).
    BeforeStmt { pos: ra_ap_syntax::TextSize },
    /// Wrap the call in a block. `call_span` is replaced by
    /// `{ <lets>; <new_call> }`.
    WrapInBlock { call_span: TextRange },
}

/// Per-lift source snippet describing the subexpression being lifted.
#[derive(Debug, Clone)]
pub struct LiftSnippet {
    /// Text of the original subexpression as written.
    pub original_text: String,
    /// Text of the place "inside" a `&place` / `&mut place`, when the
    /// strategy is one of the BindInnerPlace* variants. Otherwise empty.
    pub inner_place_text: String,
    /// Source-text span of the inner place (inside the `&` / `&mut`).
    /// Required for nested-rewrite composition: an outer lift whose RHS
    /// is the inner place needs to know its bounds so we can substitute
    /// any further nested lifts that fall within it. `None` for non-borrow
    /// strategies, where the lift's full span is the RHS span.
    pub inner_place_span: Option<TextRange>,
}

pub struct EmitInput<'a> {
    pub source: &'a str,
    pub call_span: TextRange,
    pub insert: InsertSite,
    pub plan: &'a LiftPlan,
    /// Per-subexpression text. Map keyed by the planned lift's
    /// `subexpr_id` produced by the `lift` module.
    pub snippets: BTreeMap<crate::access::SubexprId, LiftSnippet>,
    /// Name generator. Called once per lift; returns the local name to
    /// bind.
    pub name_for: &'a mut dyn FnMut(&PlannedLift) -> String,
}

/// Produce the text edit set for a single call site. Returns `None` if the
/// plan has no lifts (nothing to do).
///
/// ## Nested / overlapping lifts
///
/// Within one call site's plan, two lifts can have textually overlapping
/// spans. The canonical case: the outer call lifts a nested-call argument
/// (e.g. `g(p, p.y)`), while the inner call's own access set — folded into
/// the outer plan by [`crate::analysis::recurse_children`] — also chooses a
/// lift (e.g. `p.y`). Naively emitting one in-place replacement per lift
/// produces overlapping `Indel`s; reverse-order application then corrupts
/// the text.
///
/// We resolve this by treating the chosen lifts as a *containment forest*:
/// lift `A` is the parent of lift `B` iff `B.span` lies within the source
/// span that `A` would otherwise copy verbatim into its `let` RHS. For the
/// example above, the outer lift covers `g(p, p.y)` and absorbs the inner
/// `p.y` lift as a child.
///
/// Emission rules under that forest:
/// - Each lift's RHS text starts as `source[rhs_span]`, with each direct
///   child's span substituted by that child's *use-site replacement*
///   (`name` for value strategies, `&name` for borrow strategies).
/// - `let` statements are emitted in post-order (children before parents)
///   so an outer `let` sees the inner local already in scope.
/// - In-place replacements at the call site are emitted **only for root
///   lifts**. A child's in-place text is absorbed by its parent's RHS,
///   so emitting a separate child replacement would overlap and corrupt.
///
/// The result is a non-overlapping edit set even for arbitrarily deep
/// nested rewrites within a single call site.
pub fn emit_edits(input: EmitInput) -> Option<TextEdit> {
    if input.plan.lifts.is_empty() {
        return None;
    }
    let EmitInput {
        source,
        call_span,
        insert,
        plan,
        snippets,
        name_for,
    } = input;

    // Step 1: per-lift emission data. Skip any lift whose snippet wasn't
    // captured (the front-end didn't supply text for it); the caller is
    // responsible for surfacing that case.
    let mut data: Vec<EmitItem> = Vec::with_capacity(plan.lifts.len());
    for lift in &plan.lifts {
        let Some(snippet) = snippets.get(&lift.subexpr_id) else {
            continue;
        };
        let name = name_for(lift);
        let inner_replacement = match lift.strategy {
            LiftStrategy::BindValue
            | LiftStrategy::BindClone
            | LiftStrategy::BindCallResultClone => name.clone(),
            LiftStrategy::BindInnerPlace
            | LiftStrategy::BindInnerPlaceClone
            | LiftStrategy::BindToOwned => format!("&{}", name),
        };
        // RHS text-span: for borrow strategies this is the inner place; for
        // value-shaped strategies it's the lift's full span. We need the
        // span (not just the text) so that nested lifts contained within
        // can be located in source coordinates.
        let rhs_span = match lift.strategy {
            LiftStrategy::BindInnerPlace
            | LiftStrategy::BindInnerPlaceClone
            | LiftStrategy::BindToOwned => snippet.inner_place_span.unwrap_or(lift.span),
            _ => lift.span,
        };
        data.push(EmitItem {
            lift_span: lift.span,
            rhs_span,
            name,
            strategy: lift.strategy.clone(),
            inner_replacement,
        });
    }

    if data.is_empty() {
        return None;
    }

    // Step 2: build the containment forest. For each item `i`, `parent[i]`
    // is the index of the smallest other item whose `rhs_span` strictly
    // contains `data[i].lift_span`, or `None` if `i` is a root.
    let parent = compute_parents(&data);

    // Step 3: emit in post-order so children's `let`s precede parents'.
    //
    // Sibling order: spec Step 5 ("Order the lifts") asks for left-to-right
    // by source position — sort siblings by their lift_span start. For the
    // overall traversal, post-order on the forest rooted by source-sorted
    // roots is enough.
    let order = post_order(&data, &parent);

    let mut lets = String::new();
    for &i in &order {
        let item = &data[i];
        let body = build_rhs_text(source, item.rhs_span, &data, &parent, i);
        let rhs = match item.strategy {
            LiftStrategy::BindValue | LiftStrategy::BindInnerPlace => body,
            LiftStrategy::BindClone | LiftStrategy::BindInnerPlaceClone => {
                format!("{}.clone()", body)
            }
            LiftStrategy::BindToOwned => format!("{}.to_owned()", body),
            LiftStrategy::BindCallResultClone => format!("{}.clone()", body),
        };
        lets.push_str(&format!("let {} = {};\n", item.name, rhs));
    }

    // Step 4: in-place replacements at the call site — roots only. A child
    // lift's span is already covered by its parent's `lift_span` (which the
    // parent will replace); emitting both would overlap. The replacement
    // text is the lift's `inner_replacement` (`&name` for borrow
    // strategies, `name` otherwise) — same string used when substituting
    // a child into a parent's RHS, so both code paths stay consistent.
    let mut replacements: Vec<(TextRange, String)> = data
        .iter()
        .enumerate()
        .filter(|(i, _)| parent[*i].is_none())
        .map(|(_, item)| (item.lift_span, item.inner_replacement.clone()))
        .collect();
    // Apply within-call replacements left-to-right by start so the
    // emit-time ordering below is deterministic.
    replacements.sort_by_key(|(s, _)| (s.start(), s.end()));

    if replacements.is_empty() {
        return None;
    }

    let mut builder = TextEditBuilder::default();
    match insert {
        InsertSite::BeforeStmt { pos } => {
            builder.insert(pos, lets);
            for (span, repl) in replacements {
                builder.replace(span, repl);
            }
        }
        InsertSite::WrapInBlock { call_span: cs } => {
            // Build the wrapped block locally: extract the call text and
            // apply the (root-only) replacements right-to-left so offsets
            // stay valid.
            let call_text = &source[range_as_usize(cs)];
            let mut local = call_text.to_string();
            let mut sorted = replacements.clone();
            sorted.sort_by_key(|(span, _)| std::cmp::Reverse(span.start()));
            for (span, repl) in sorted {
                let rel = TextRange::new(
                    span.start().checked_sub(cs.start()).unwrap_or_default(),
                    span.end().checked_sub(cs.start()).unwrap_or_default(),
                );
                local.replace_range(range_as_usize(rel), &repl);
            }
            let wrapped = format!("{{ {}{} }}", lets, local);
            builder.replace(cs, wrapped);
        }
    }

    let _ = call_span;
    Some(builder.finish())
}

/// Internal per-lift data carried through the containment-aware emit pass.
#[derive(Debug, Clone)]
struct EmitItem {
    /// The span replaced in-place at the call site (full subexpression).
    lift_span: TextRange,
    /// The span whose text is copied into the `let`'s RHS. For borrow
    /// strategies this is the inner place; for value strategies it equals
    /// `lift_span`.
    rhs_span: TextRange,
    /// Local name bound by this lift's `let`.
    name: String,
    strategy: LiftStrategy,
    /// What replaces this lift at its `lift_span`: `name` for value
    /// strategies, `&name` for borrow strategies.
    inner_replacement: String,
}

/// For each item, find the smallest other item whose `rhs_span` contains
/// this item's `lift_span`. Returns `None` for roots.
///
/// Containment is non-strict — when an inner value-lift's span equals the
/// outer's inner-place RHS span (e.g., outer is `&p.field` lifted via
/// `BindInnerPlaceClone` with `rhs_span = "p.field"`, inner lifts the
/// same `p.field` by value) the inner is still a child of the outer; the
/// outer's RHS text is then entirely the substitution `name`.
///
/// Tie-break by smallest `rhs_span` length, then by lowest item index, so
/// the result is deterministic if two candidates have identical spans.
fn compute_parents(items: &[EmitItem]) -> Vec<Option<usize>> {
    let n = items.len();
    let mut parent = vec![None; n];
    for i in 0..n {
        let child = items[i].lift_span;
        let mut best: Option<usize> = None;
        let mut best_len: u32 = u32::MAX;
        for (j, jth) in items.iter().enumerate().take(n) {
            if i == j {
                continue;
            }
            let cand = jth.rhs_span;
            if cand.start() <= child.start() && cand.end() >= child.end() {
                let len: u32 = (cand.end() - cand.start()).into();
                if len < best_len || (len == best_len && Some(j) < best) {
                    best_len = len;
                    best = Some(j);
                }
            }
        }
        parent[i] = best;
    }
    parent
}

/// Compute post-order over the containment forest, with siblings ordered
/// left-to-right by `lift_span.start()` (spec Step 5).
fn post_order(items: &[EmitItem], parent: &[Option<usize>]) -> Vec<usize> {
    let n = items.len();
    let mut children: Vec<Vec<usize>> = vec![Vec::new(); n];
    let mut roots: Vec<usize> = Vec::new();
    for (i, ith) in parent.iter().enumerate().take(n) {
        match ith {
            Some(p) => children[*p].push(i),
            None => roots.push(i),
        }
    }
    let by_start = |a: &usize, b: &usize| {
        items[*a]
            .lift_span
            .start()
            .cmp(&items[*b].lift_span.start())
    };
    roots.sort_by(by_start);
    for c in &mut children {
        c.sort_by(by_start);
    }
    let mut order = Vec::with_capacity(n);
    for r in roots {
        push_post_order(r, &children, &mut order);
    }
    order
}

fn push_post_order(node: usize, children: &[Vec<usize>], order: &mut Vec<usize>) {
    for &c in &children[node] {
        push_post_order(c, children, order);
    }
    order.push(node);
}

/// Build the RHS text for `idx`: take `source[rhs_span]` and substitute
/// each direct child's `lift_span` with the child's `inner_replacement`.
fn build_rhs_text(
    source: &str,
    rhs_span: TextRange,
    items: &[EmitItem],
    parent: &[Option<usize>],
    idx: usize,
) -> String {
    let mut subs: Vec<(TextRange, String)> = items
        .iter()
        .enumerate()
        .filter(|(j, _)| parent[*j] == Some(idx))
        .map(|(_, c)| (c.lift_span, c.inner_replacement.clone()))
        .collect();
    if subs.is_empty() {
        return source[range_as_usize(rhs_span)].to_string();
    }
    // Apply right-to-left in local coordinates so offsets stay valid.
    subs.sort_by_key(|(s, _)| std::cmp::Reverse(s.start()));
    let base = rhs_span.start();
    let mut text = source[range_as_usize(rhs_span)].to_string();
    for (span, repl) in subs {
        let lo: usize = (span.start() - base).into();
        let hi: usize = (span.end() - base).into();
        text.replace_range(lo..hi, &repl);
    }
    text
}

fn range_as_usize(r: TextRange) -> std::ops::Range<usize> {
    let start: usize = r.start().into();
    let end: usize = r.end().into();
    start..end
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::access::SubexprId;
    use crate::lift::LiftStrategy;
    use ra_ap_syntax::TextSize;

    fn mk_lift(id: u32, span: TextRange, strategy: LiftStrategy) -> PlannedLift {
        PlannedLift {
            subexpr_id: SubexprId(id),
            span,
            arg_index: 0,
            strategy,
        }
    }

    #[test]
    fn before_stmt_inserts_let_and_rewrites_arg() {
        let source = "    f(p, p.x);";
        let call = TextRange::new(TextSize::from(4), TextSize::from(13));
        let arg_span = TextRange::new(TextSize::from(9), TextSize::from(12));

        let plan = LiftPlan {
            lifts: vec![mk_lift(1, arg_span, LiftStrategy::BindValue)],
        };

        let mut snippets = BTreeMap::new();
        snippets.insert(
            SubexprId(1),
            LiftSnippet {
                original_text: "p.x".into(),
                inner_place_text: String::new(),
                inner_place_span: None,
            },
        );

        let mut counter = 0;
        let mut name_for = |_: &PlannedLift| -> String {
            counter += 1;
            format!("__lift_{}", counter)
        };

        let input = EmitInput {
            source,
            call_span: call,
            insert: InsertSite::BeforeStmt {
                pos: TextSize::from(4),
            },
            plan: &plan,
            snippets,
            name_for: &mut name_for,
        };

        let edit = emit_edits(input).unwrap();
        let mut out = source.to_string();
        edit.apply(&mut out);
        assert!(out.contains("let __lift_1 = p.x;\n"));
        assert!(out.contains("f(p, __lift_1)"));
    }

    #[test]
    fn nested_lifts_compose_without_overlap() {
        // f(g(p, p.y), h(p));
        // Outer chooses to lift `g(p, p.y)` (BindCallResultClone).
        // Inner — folded into the outer plan — chooses to lift `p.y`
        // (BindValue). The inner span lies inside the outer span; emit
        // must absorb the inner lift into the outer's RHS rather than
        // emitting two overlapping in-place replacements.
        //
        //         0         1         2
        //         0123456789012345678901234567
        let source = "    f(g(p, p.y), h(p));    ";
        let call_span = TextRange::new(TextSize::from(4), TextSize::from(22));
        let outer_span = TextRange::new(TextSize::from(6), TextSize::from(15)); // "g(p, p.y)"
        let inner_span = TextRange::new(TextSize::from(11), TextSize::from(14)); // "p.y"

        let plan = LiftPlan {
            lifts: vec![
                mk_lift(10, outer_span, LiftStrategy::BindCallResultClone),
                mk_lift(11, inner_span, LiftStrategy::BindValue),
            ],
        };

        let mut snippets = BTreeMap::new();
        snippets.insert(
            SubexprId(10),
            LiftSnippet {
                original_text: "g(p, p.y)".into(),
                inner_place_text: String::new(),
                inner_place_span: None,
            },
        );
        snippets.insert(
            SubexprId(11),
            LiftSnippet {
                original_text: "p.y".into(),
                inner_place_text: String::new(),
                inner_place_span: None,
            },
        );

        let mut counter = 0;
        let mut name_for = |_: &PlannedLift| -> String {
            counter += 1;
            format!("__t{}", counter)
        };

        let input = EmitInput {
            source,
            call_span,
            insert: InsertSite::BeforeStmt {
                pos: TextSize::from(4),
            },
            plan: &plan,
            snippets,
            name_for: &mut name_for,
        };

        let edit = emit_edits(input).unwrap();
        let mut out = source.to_string();
        edit.apply(&mut out);

        // The two name_for calls happen in plan-iteration order (outer
        // first, inner second), so __t1 is the outer name and __t2 the
        // inner name. Inner `let` must precede outer `let`, and outer's
        // RHS must reference the inner local — not the original `p.y`.
        let inner_let = "let __t2 = p.y;";
        let outer_let = "let __t1 = g(p, __t2).clone();";
        let inner_pos = out.find(inner_let).expect("inner let missing");
        let outer_pos = out.find(outer_let).expect("outer let missing");
        assert!(
            inner_pos < outer_pos,
            "inner let must come first; got out:\n{}",
            out,
        );

        // The call site itself: only the outer's in-place replacement
        // applies; the inner is absorbed.
        assert!(out.contains("f(__t1, h(p));"), "got:\n{}", out);
        // And the original `p.y` text inside the call must be gone.
        let after_lets = &out[outer_pos + outer_let.len()..];
        assert!(
            !after_lets.contains("p.y"),
            "original `p.y` should be gone from the call site; got:\n{}",
            out,
        );
    }

    #[test]
    fn borrow_strategy_with_nested_value_lift_inside_inner_place() {
        // Outer lifts `&p.field` via BindInnerPlaceClone.
        // The inner-place text is `p.field`. We add a synthetic nested
        // value-lift covering `p.field` itself to prove the substitution
        // path through `inner_place_span`.
        //
        //         0         1
        //         01234567890123456789
        let source = "    f(&p.field);    ";
        let call_span = TextRange::new(TextSize::from(4), TextSize::from(15));
        let outer_span = TextRange::new(TextSize::from(6), TextSize::from(14)); // "&p.field"
        let inner_place = TextRange::new(TextSize::from(7), TextSize::from(14)); // "p.field"

        let plan = LiftPlan {
            lifts: vec![
                mk_lift(20, outer_span, LiftStrategy::BindInnerPlaceClone),
                mk_lift(21, inner_place, LiftStrategy::BindValue),
            ],
        };

        let mut snippets = BTreeMap::new();
        snippets.insert(
            SubexprId(20),
            LiftSnippet {
                original_text: "&p.field".into(),
                inner_place_text: "p.field".into(),
                inner_place_span: Some(inner_place),
            },
        );
        snippets.insert(
            SubexprId(21),
            LiftSnippet {
                original_text: "p.field".into(),
                inner_place_text: String::new(),
                inner_place_span: None,
            },
        );

        let mut counter = 0;
        let mut name_for = |_: &PlannedLift| -> String {
            counter += 1;
            format!("__t{}", counter)
        };

        let input = EmitInput {
            source,
            call_span,
            insert: InsertSite::BeforeStmt {
                pos: TextSize::from(4),
            },
            plan: &plan,
            snippets,
            name_for: &mut name_for,
        };

        let edit = emit_edits(input).unwrap();
        let mut out = source.to_string();
        edit.apply(&mut out);

        // Inner value-lift must precede outer borrow-lift, and outer's
        // RHS uses the inner name, with `.clone()` appended.
        let inner_let = "let __t2 = p.field;";
        let outer_let = "let __t1 = __t2.clone();";
        let inner_pos = out.find(inner_let).expect("inner let missing");
        let outer_pos = out.find(outer_let).expect("outer let missing");
        assert!(inner_pos < outer_pos);

        // At the call site the borrow strategy substitutes `&__t1`.
        assert!(out.contains("f(&__t1);"), "got:\n{}", out);
    }
}
