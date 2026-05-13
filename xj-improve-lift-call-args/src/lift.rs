//! Lift-strategy selection (Step 3) and feasibility classification (Step 4).
//!
//! Inputs: the access list from Step 1 and the conflict edges from Step 2.
//! Outputs: a `LiftPlan` describing which subexpressions to lift and how, or
//! a `Diagnostic` explaining why lifting alone is insufficient.

use std::collections::BTreeMap;

use ra_ap_syntax::TextRange;

#[rustfmt::skip]
use crate::access::{Access, AccessKind, SubexprId, conflict_edges, resolved_by_splitting};

/// Per-subexpression type/shape information supplied by the rust-analyzer
/// front-end. Captures only what Step 4 actually needs.
#[derive(Debug, Clone)]
pub struct TyShape {
    pub is_copy: bool,
    pub is_clone: bool,
    /// Whether this is a `&str`/`&[T]`-style slice-ish reference that lifts
    /// cleanly via `.to_owned()`.
    pub is_str_or_slice_ref: bool,
    /// The kind of expression form. Influences which Step-4 branch applies.
    pub form: ExprForm,
    /// For `form == BorrowOf` / `MutBorrowOf`: whether the inner place's
    /// type itself is Copy (Step 4: "If `s` is `&place` where `place`'s
    /// type is `Copy`, lift as `let tmp = place;`").
    pub inner_place_is_copy: bool,
    pub inner_place_is_clone: bool,
    /// For `form == NestedCall`: whether the return type is owned (true)
    /// or a borrow (false). For all other forms this is ignored.
    pub call_return_is_owned: bool,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ExprForm {
    /// A bare place expression (path, field, index).
    BarePlace,
    /// `&place`.
    BorrowOf,
    /// `&mut place`.
    MutBorrowOf,
    /// A nested call (function or method).
    NestedCall,
    /// Anything else — literal, arithmetic, block, etc.
    Other,
}

/// What to do with a chosen subexpression. Step 4 selects exactly one of these.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum LiftStrategy {
    /// `let tmp = <s>;` and replace `<s>` with `tmp`.
    BindValue,
    /// `let tmp = <s>.clone();` and replace `<s>` with `tmp`.
    BindClone,
    /// `<s>` is `&place`; lift as `let tmp = place;` and rewrite use site
    /// to `&tmp`.
    BindInnerPlace,
    /// `<s>` is `&place`; lift as `let tmp = place.clone();` and rewrite
    /// use site to `&tmp`.
    BindInnerPlaceClone,
    /// `<s>` is `&str` / `&[T]`; lift as `let tmp = s.to_owned();` and
    /// rewrite use site to `&tmp`.
    BindToOwned,
    /// `<s>` is a nested call `g(...)` returning a borrow; lift as
    /// `let tmp = g(...).clone();` and rewrite use site to `&tmp` (or to
    /// `tmp` if the outer call wants the value directly).
    BindCallResultClone,
}

/// One planned lift.
#[derive(Debug, Clone)]
pub struct PlannedLift {
    pub subexpr_id: SubexprId,
    pub span: TextRange,
    pub arg_index: usize,
    pub strategy: LiftStrategy,
}

#[derive(Debug, Clone)]
pub struct LiftPlan {
    pub lifts: Vec<PlannedLift>,
}

#[derive(Debug, Clone)]
pub struct Diagnostic {
    pub call_span: TextRange,
    pub conflicting: (Access, Access),
    pub reason: String,
    pub suggestion: Option<String>,
}

/// Run Step 3 + Step 4 + Step 5 on the access list.
///
/// `shape_of` returns `(TyShape, span)` for the given subexpression id.
/// Returning `None` indicates the front-end could not produce shape info;
/// `plan_lifts` will then refuse to lift that subexpression and report
/// infeasibility, because conservatively we cannot prove the lift is safe.
///
/// The lift target for each conflict is the access's `outer_subexpr` —
/// the top-level argument expression that contains the conflicting access.
/// This matters when the conflict resolves to a sub-expression of a
/// nested call (e.g. an inner `&mut x`): lifting that sub-expression
/// directly is infeasible (`let _ = &mut x;` just renames the borrow),
/// but lifting the enclosing nested call is feasible and produces a
/// local holding the call's return value, releasing the inner borrow.
pub fn plan_lifts<F>(
    call_span: TextRange,
    accesses: &[Access],
    mut shape_of: F,
) -> Result<LiftPlan, Box<Diagnostic>>
where
    F: FnMut(SubexprId) -> Option<(TyShape, TextRange)>,
{
    let edges = conflict_edges(accesses);
    if edges.is_empty() {
        return Ok(LiftPlan { lifts: Vec::new() });
    }

    // Step 3: choose ONE side of each unresolved edge to lift. Dedup by
    // the chosen access's *outer* subexpression — multiple inner accesses
    // of the same argument collapse to a single lift of the whole arg.
    let mut chosen: BTreeMap<SubexprId, usize> = BTreeMap::new();
    for (i, j) in &edges {
        let a = &accesses[*i];
        let b = &accesses[*j];

        if resolved_by_splitting(a, b) {
            continue;
        }

        let lift_idx = choose_lift_side(*i, *j, accesses);
        let chosen_access = &accesses[lift_idx];
        chosen
            .entry(chosen_access.outer_subexpr)
            .or_insert(lift_idx);
    }

    if chosen.is_empty() {
        return Ok(LiftPlan { lifts: Vec::new() });
    }

    // Step 4: classify each chosen subexpression's lift strategy on its
    // OUTER expression (see plan_lifts doc above). Abort on the first
    // infeasible lift with a precise diagnostic.
    let mut lifts: Vec<PlannedLift> = Vec::with_capacity(chosen.len());
    for (outer_sid, idx) in &chosen {
        let acc = &accesses[*idx];
        let outer_sid = *outer_sid;
        let Some((shape, outer_span)) = shape_of(outer_sid) else {
            return Err(Box::new(Diagnostic {
                call_span,
                conflicting: pick_witness_pair(accesses, &edges, acc.subexpr_id),
                reason: format!(
                    "could not determine type/shape for the chosen subexpression \
                     at {:?}; lifting cannot be proven safe",
                    acc.span,
                ),
                suggestion: None,
            }));
        };

        // Classify using the OUTER's shape (so e.g. NestedCall form → a
        // feasible BindValue) but the inner access's kind (so diagnostics
        // retain context if classification fails).
        let strategy = match classify_lift(acc, &shape) {
            Ok(s) => s,
            Err(reason) => {
                return Err(Box::new(Diagnostic {
                    call_span,
                    conflicting: pick_witness_pair(accesses, &edges, acc.subexpr_id),
                    reason,
                    suggestion: suggest_for(acc, &shape),
                }));
            }
        };

        lifts.push(PlannedLift {
            subexpr_id: outer_sid,
            span: outer_span,
            arg_index: acc.arg_index,
            strategy,
        });
    }

    // Step 5: order by source position (left-to-right). The original AST
    // already encodes left-to-right argument order, and within an argument
    // by `TextRange::start`.
    lifts.sort_by_key(|l| (l.span.start(), l.arg_index));

    Ok(LiftPlan { lifts })
}

/// Step 3.2 selection. The spec lists three preferences (later arg, less
/// destructive, prefer nested-call). When they pull in different
/// directions, prefer feasibility-relevant signals first: a Read or
/// SharedBorrow is almost always trivially liftable, while a MutBorrow is
/// almost never. Hence: less destructive → nested-call → higher arg →
/// leftmost span. This matches the spec's explicit case (test 15: lift
/// the Call when paired with a MutBorrow) at the cost of contradicting
/// the spec's narrative ordering, which is acceptable given Step 3.2 also
/// requires deterministic tiebreaks.
fn choose_lift_side(i: usize, j: usize, accesses: &[Access]) -> usize {
    let a = &accesses[i];
    let b = &accesses[j];

    let da = a.kind.destructiveness();
    let db = b.kind.destructiveness();
    if da != db {
        return if da < db { i } else { j };
    }

    match (a.kind, b.kind) {
        (AccessKind::Call, k) if !matches!(k, AccessKind::Call) => return i,
        (k, AccessKind::Call) if !matches!(k, AccessKind::Call) => return j,
        _ => {}
    }

    if a.arg_index != b.arg_index {
        return if a.arg_index > b.arg_index { i } else { j };
    }

    if a.span.start() <= b.span.start() {
        i
    } else {
        j
    }
}

fn classify_lift(acc: &Access, shape: &TyShape) -> Result<LiftStrategy, String> {
    use AccessKind::*;
    use ExprForm::*;

    match shape.form {
        BarePlace => match acc.kind {
            Read => Ok(LiftStrategy::BindValue),
            Move => {
                if shape.is_copy {
                    Ok(LiftStrategy::BindValue)
                } else if shape.is_clone {
                    // A by-value Move of Clone with the source also touched
                    // elsewhere is infeasible per spec ("Move of a non-Copy,
                    // non-Clone value AND source place is also accessed
                    // elsewhere"). For non-Copy *Clone* values where the
                    // source IS reused, prefer .clone(). For straight Move
                    // with no reuse, BindValue is enough — but the only
                    // reason we're lifting at all is a conflict, which IS
                    // reuse. So default to clone.
                    Ok(LiftStrategy::BindClone)
                } else {
                    Err("infeasible: by-value Move of a non-Copy, non-Clone value \
                         whose source is also accessed elsewhere in the call — \
                         lifting alone cannot resolve this (would need \
                         mem::take/replace)"
                        .into())
                }
            }
            SharedBorrow | MutBorrow => {
                // We saw a bare place but classified the kind as a borrow —
                // happens when the callee takes &T / &mut T and Rust
                // auto-refs. Treat as borrow of the place.
                if matches!(acc.kind, MutBorrow) {
                    Err("cannot lift an auto-ref `&mut` of a place that is also \
                         borrowed by another argument; rewriting requires \
                         split_at_mut or mem::replace"
                        .into())
                } else if shape.inner_place_is_copy {
                    Ok(LiftStrategy::BindInnerPlace)
                } else if shape.inner_place_is_clone {
                    Ok(LiftStrategy::BindInnerPlaceClone)
                } else {
                    Err("shared auto-ref of a non-Copy, non-Clone place — cannot \
                         lift without changing semantics"
                        .into())
                }
            }
            Call => unreachable!("Call kind only arises for NestedCall form"),
        },
        BorrowOf => {
            if shape.inner_place_is_copy {
                Ok(LiftStrategy::BindInnerPlace)
            } else if shape.inner_place_is_clone {
                Ok(LiftStrategy::BindInnerPlaceClone)
            } else {
                Err("shared borrow `&place` of a non-Copy, non-Clone type — \
                     cannot lift without changing semantics"
                    .into())
            }
        }
        MutBorrowOf => Err(
            "infeasible by lifting alone: `&mut place` cannot be hoisted into a \
             local without aliasing the same memory the other conflicting \
             argument needs"
                .into(),
        ),
        NestedCall => {
            if shape.call_return_is_owned {
                Ok(LiftStrategy::BindValue)
            } else if shape.is_str_or_slice_ref {
                Ok(LiftStrategy::BindToOwned)
            } else if shape.is_clone {
                Ok(LiftStrategy::BindCallResultClone)
            } else {
                Err(
                    "nested call returns a borrow that ties the lifetime to the \
                     outer call's arguments, and the return type is not Clone or \
                     a slice/str; lifting cannot decouple the borrow"
                        .into(),
                )
            }
        }
        Other => {
            // Generic expressions (literals, arithmetic, blocks) are always
            // safe to bind by value. Copy-ness only affects whether the
            // source is consumed, which is moot for a freshly evaluated
            // expression with no source place.
            Ok(LiftStrategy::BindValue)
        }
    }
}

fn suggest_for(acc: &Access, shape: &TyShape) -> Option<String> {
    if matches!(shape.form, ExprForm::MutBorrowOf) {
        Some(format!(
            "consider `split_at_mut` / `get_disjoint_mut` / `mem::swap` for the \
             `&mut` at {:?}",
            acc.span,
        ))
    } else {
        None
    }
}

fn pick_witness_pair(
    accesses: &[Access],
    edges: &[(usize, usize)],
    sid: SubexprId,
) -> (Access, Access) {
    for (i, j) in edges {
        if accesses[*i].subexpr_id == sid || accesses[*j].subexpr_id == sid {
            return (accesses[*i].clone(), accesses[*j].clone());
        }
    }
    // Should be unreachable; pick the first edge defensively.
    let (i, j) = edges[0];
    (accesses[i].clone(), accesses[j].clone())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::access::{Place, PlaceRoot, Projection, SubexprId};

    fn mk_access(arg: usize, place: Place, kind: AccessKind, id: u32) -> Access {
        Access {
            arg_index: arg,
            place,
            kind,
            span: TextRange::default(),
            subexpr_id: SubexprId(id),
            outer_subexpr: SubexprId(id),
        }
    }

    fn shape(form: ExprForm) -> TyShape {
        TyShape {
            is_copy: true,
            is_clone: true,
            is_str_or_slice_ref: false,
            form,
            inner_place_is_copy: true,
            inner_place_is_clone: true,
            call_return_is_owned: true,
        }
    }

    #[test]
    fn no_conflict_yields_empty_plan() {
        let a = mk_access(
            0,
            Place {
                root: PlaceRoot::Named("p".into()),
                projections: vec![],
            },
            AccessKind::SharedBorrow,
            0,
        );
        let b = mk_access(
            1,
            Place {
                root: PlaceRoot::Named("q".into()),
                projections: vec![],
            },
            AccessKind::SharedBorrow,
            1,
        );
        let plan = plan_lifts(TextRange::default(), &[a, b], |_| {
            Some((shape(ExprForm::BarePlace), TextRange::default()))
        })
        .unwrap();
        assert!(plan.lifts.is_empty());
    }

    #[test]
    fn mut_borrow_lift_is_infeasible() {
        let a = mk_access(0, Place::named("p"), AccessKind::MutBorrow, 0);
        let b = mk_access(
            1,
            Place::named("p").pushed(Projection::Field {
                name: "x".into(),
                splittable: false,
            }),
            AccessKind::MutBorrow,
            1,
        );
        let err = plan_lifts(TextRange::default(), &[a, b], |_| {
            Some((shape(ExprForm::MutBorrowOf), TextRange::default()))
        })
        .unwrap_err();
        assert!(err.reason.contains("infeasible"));
    }

    #[test]
    fn read_lift_chosen_over_mut_borrow_partner() {
        // f(p, p.x) — p MutBorrow vs p.x Read; Read is less destructive,
        // and arg_index 1 > 0, both push us to lift arg 1.
        let a = mk_access(0, Place::named("p"), AccessKind::MutBorrow, 0);
        let b = mk_access(
            1,
            Place::named("p").pushed(Projection::Field {
                name: "x".into(),
                splittable: true,
            }),
            AccessKind::Read,
            1,
        );
        let plan = plan_lifts(TextRange::default(), &[a, b], |_| {
            Some((shape(ExprForm::BarePlace), TextRange::default()))
        })
        .unwrap();
        assert_eq!(plan.lifts.len(), 1);
        assert_eq!(plan.lifts[0].subexpr_id, SubexprId(1));
        assert_eq!(plan.lifts[0].strategy, LiftStrategy::BindValue);
    }
}
