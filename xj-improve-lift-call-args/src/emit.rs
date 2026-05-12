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

    let mut builder = TextEditBuilder::default();

    // Build the let-statement block as a string. We render each lift as
    // `    let <name> = <expr>;\n` so that, when the existing surrounding
    // indentation is applied by rustfmt, the result looks natural.
    let mut lets = String::new();
    let mut replacements: Vec<(TextRange, String)> = Vec::new();

    for lift in &plan.lifts {
        let Some(snippet) = snippets.get(&lift.subexpr_id) else {
            // Without source text for the subexpression we cannot emit a
            // correct lift; skip and let the caller report.
            continue;
        };
        let name = name_for(lift);
        let (let_rhs, replacement) = match lift.strategy {
            LiftStrategy::BindValue => {
                (snippet.original_text.clone(), name.clone())
            }
            LiftStrategy::BindClone => (
                format!("{}.clone()", snippet.original_text),
                name.clone(),
            ),
            LiftStrategy::BindInnerPlace => (
                snippet.inner_place_text.clone(),
                format!("&{}", name),
            ),
            LiftStrategy::BindInnerPlaceClone => (
                format!("{}.clone()", snippet.inner_place_text),
                format!("&{}", name),
            ),
            LiftStrategy::BindToOwned => (
                format!("{}.to_owned()", snippet.inner_place_text),
                format!("&{}", name),
            ),
            LiftStrategy::BindCallResultClone => (
                format!("{}.clone()", snippet.original_text),
                name.clone(),
            ),
        };
        lets.push_str(&format!("let {} = {};\n", name, let_rhs));
        replacements.push((lift.span, replacement));
    }

    if replacements.is_empty() {
        return None;
    }

    match insert {
        InsertSite::BeforeStmt { pos } => {
            builder.insert(pos, lets);
            for (span, repl) in replacements {
                builder.replace(span, repl);
            }
        }
        InsertSite::WrapInBlock { call_span: cs } => {
            // Build the wrapped block by extracting the call text, applying
            // the replacements offset-relative to it, and producing one
            // single `replace` over `cs`.
            let call_text = &source[range_as_usize(cs)];
            let mut local = call_text.to_string();
            // Apply replacements right-to-left to keep offsets stable.
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

    // call_span is used only to identify the call site for logging; the
    // actual replacements are above.
    let _ = call_span;

    Some(builder.finish())
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
}
