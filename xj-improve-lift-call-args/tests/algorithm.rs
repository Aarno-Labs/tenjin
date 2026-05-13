//! Algorithm-level tests that do not require rust-analyzer.
//!
//! These exercise the same code paths as the integration cases listed in
//! `original_spec.md` ("Test cases (minimum set)"), but supply the
//! per-subexpression `TyShape` directly instead of routing through a real
//! workspace. End-to-end coverage with rust-analyzer would live in a
//! workspace fixture under `tests/fixtures/` and is intentionally out of
//! scope for the unit suite.

use ra_ap_syntax::TextRange;

use xj_improve_lift_call_args::access::{
    Access, AccessKind, IndexKey, Place, Projection, SubexprId,
};
use xj_improve_lift_call_args::lift::{ExprForm, LiftStrategy, TyShape, plan_lifts};

fn at(arg: usize, place: Place, kind: AccessKind, id: u32) -> Access {
    Access {
        arg_index: arg,
        place,
        kind,
        span: TextRange::default(),
        subexpr_id: SubexprId(id),
        outer_subexpr: SubexprId(id),
    }
}

fn field(name: &str, splittable: bool) -> Projection {
    Projection::Field {
        name: name.into(),
        splittable,
    }
}

fn shape(form: ExprForm, is_copy: bool, is_clone: bool) -> TyShape {
    TyShape {
        is_copy,
        is_clone,
        is_str_or_slice_ref: false,
        form,
        inner_place_is_copy: is_copy,
        inner_place_is_clone: is_clone,
        call_return_is_owned: matches!(form, ExprForm::NestedCall),
    }
}

#[test]
fn case_01_f_p_px_copy_field() {
    // f(p, p.x) with p: &mut T, x: i32 (Copy) -> lifts p.x.
    let xs = vec![
        at(0, Place::named("p"), AccessKind::MutBorrow, 0),
        at(
            1,
            Place::named("p").pushed(field("x", true)),
            AccessKind::Read,
            1,
        ),
    ];
    let plan = plan_lifts(TextRange::default(), &xs, |_| {
        Some((shape(ExprForm::BarePlace, true, true), TextRange::default()))
    })
    .unwrap();
    assert_eq!(plan.lifts.len(), 1);
    assert_eq!(plan.lifts[0].subexpr_id, SubexprId(1));
    assert_eq!(plan.lifts[0].strategy, LiftStrategy::BindValue);
}

#[test]
fn case_05_f_pa_pb_direct_fields_no_change() {
    // f(p.a, p.b) on direct splittable fields, non-Copy -> emit unchanged.
    let xs = vec![
        at(
            0,
            Place::named("p").pushed(field("a", true)),
            AccessKind::Move,
            0,
        ),
        at(
            1,
            Place::named("p").pushed(field("b", true)),
            AccessKind::Move,
            1,
        ),
    ];
    let plan = plan_lifts(TextRange::default(), &xs, |_| {
        Some((
            shape(ExprForm::BarePlace, false, true),
            TextRange::default(),
        ))
    })
    .unwrap();
    assert!(plan.lifts.is_empty(), "splittable siblings need no lift");
}

#[test]
fn case_06_through_box_non_splittable_lifts_via_clone() {
    // f(p.a, p.b) where p: Box<T> — projections are non-splittable.
    let xs = vec![
        at(
            0,
            Place::named("p")
                .pushed(Projection::Deref)
                .pushed(field("a", false)),
            AccessKind::Move,
            0,
        ),
        at(
            1,
            Place::named("p")
                .pushed(Projection::Deref)
                .pushed(field("b", false)),
            AccessKind::Move,
            1,
        ),
    ];
    let plan = plan_lifts(TextRange::default(), &xs, |_| {
        Some((
            shape(ExprForm::BarePlace, false, true),
            TextRange::default(),
        ))
    })
    .unwrap();
    assert_eq!(plan.lifts.len(), 1);
    assert_eq!(plan.lifts[0].strategy, LiftStrategy::BindClone);
}

#[test]
fn case_17_swap_mut_indices_aborts() {
    // swap(&mut v[i], &mut v[j]) -> infeasible by lifting.
    let xs = vec![
        at(
            0,
            Place::named("v").pushed(Projection::Index {
                key: IndexKey::Opaque(0),
            }),
            AccessKind::MutBorrow,
            0,
        ),
        at(
            1,
            Place::named("v").pushed(Projection::Index {
                key: IndexKey::Opaque(1),
            }),
            AccessKind::MutBorrow,
            1,
        ),
    ];
    let err = plan_lifts(TextRange::default(), &xs, |_| {
        Some((
            shape(ExprForm::MutBorrowOf, false, false),
            TextRange::default(),
        ))
    })
    .unwrap_err();
    assert!(err.reason.contains("infeasible"));
    assert!(err.suggestion.is_some());
}

#[test]
fn case_18_shared_borrows_on_indices_no_change() {
    let xs = vec![
        at(
            0,
            Place::named("a").pushed(Projection::Index {
                key: IndexKey::Opaque(0),
            }),
            AccessKind::SharedBorrow,
            0,
        ),
        at(
            1,
            Place::named("a").pushed(Projection::Index {
                key: IndexKey::Opaque(1),
            }),
            AccessKind::SharedBorrow,
            1,
        ),
    ];
    let plan = plan_lifts(TextRange::default(), &xs, |_| {
        Some((shape(ExprForm::BorrowOf, false, true), TextRange::default()))
    })
    .unwrap();
    assert!(plan.lifts.is_empty(), "both shared borrows never conflict");
}

#[test]
fn case_22_double_move_non_copy_aborts() {
    // f(p, p) with p non-Copy.
    let xs = vec![
        at(0, Place::named("p"), AccessKind::Move, 0),
        at(1, Place::named("p"), AccessKind::Move, 1),
    ];
    let err = plan_lifts(TextRange::default(), &xs, |_| {
        Some((
            shape(ExprForm::BarePlace, false, false),
            TextRange::default(),
        ))
    })
    .unwrap_err();
    assert!(err.reason.contains("infeasible") || err.reason.contains("Move"));
}

#[test]
fn outer_mut_borrow_vs_inner_nested_call_mut_borrow_lifts_the_call() {
    // outer(&mut x, inner(&mut x, ...))
    //   arg 0: MutBorrow of x  (outer's &mut x)
    //   arg 1: Call on some place  (from inner call's join)
    //   arg 1: MutBorrow of x  (inner's &mut x, sub-access — outer_subexpr
    //                            points back to the whole inner call)
    // Conflict between the two MutBorrows of x. Step 3 picks arg 1 (higher
    // index); Step 4 classifies the OUTER (the whole nested call) and
    // since calls return owned values, BindValue is feasible.
    let outer_call_id = SubexprId(10);
    let xs = vec![
        at(0, Place::named("x"), AccessKind::MutBorrow, 0),
        Access {
            arg_index: 1,
            place: Place::named("x"),
            kind: AccessKind::MutBorrow,
            span: TextRange::default(),
            subexpr_id: SubexprId(11),
            outer_subexpr: outer_call_id,
        },
    ];
    let plan = plan_lifts(TextRange::default(), &xs, |sid| {
        // Shape lookup keyed by outer_subexpr. For sid==10 (the outer
        // arg's whole nested call): NestedCall form, owned return.
        if sid == outer_call_id {
            Some((
                shape(ExprForm::NestedCall, false, true),
                TextRange::default(),
            ))
        } else {
            Some((shape(ExprForm::BarePlace, true, true), TextRange::default()))
        }
    })
    .unwrap();
    assert_eq!(plan.lifts.len(), 1, "expected 1 lift, got {:?}", plan.lifts);
    assert_eq!(plan.lifts[0].subexpr_id, outer_call_id);
    assert_eq!(plan.lifts[0].strategy, LiftStrategy::BindValue);
}

#[test]
fn mut_borrow_and_read_of_same_static_lifts_the_read() {
    // func(&mut errno, errno) where errno is Copy.
    //   arg 0: MutBorrow of place `errno` (from &mut errno)
    //   arg 1: Read of place `errno` (Copy value)
    // Same place, MutBorrow vs Read -> conflict. Lift the Read.
    let xs = vec![
        at(0, Place::named("errno"), AccessKind::MutBorrow, 0),
        at(1, Place::named("errno"), AccessKind::Read, 1),
    ];
    let plan = plan_lifts(TextRange::default(), &xs, |_| {
        Some((shape(ExprForm::BarePlace, true, true), TextRange::default()))
    })
    .unwrap();
    assert_eq!(plan.lifts.len(), 1);
    assert_eq!(plan.lifts[0].arg_index, 1);
    assert_eq!(plan.lifts[0].strategy, LiftStrategy::BindValue);
}

#[test]
fn regression_nested_call_with_no_place_args_does_not_pollute() {
    // Models: f(callee, chain.method().method(), raw_ptr)
    // where `chain.method().method()` is a nested call whose place
    // arguments don't resolve to any place expression — so the front-end
    // omits the Call access entirely (it would otherwise default to
    // `Unknown` and conflict with every other arg, spuriously lifting the
    // callee and `raw_ptr`).
    //
    // With the fix, the only accesses present are the callee (Move) and
    // raw_ptr (Read), on disjoint roots. No conflicts, no lifts.
    let xs = vec![
        at(0, Place::named("callee"), AccessKind::Move, 0),
        // arg_index 1 = the nested-call argument: NO access record
        // synthesized, only its recursive sub-accesses (which in this
        // model are absent because the chain touches only globals).
        at(2, Place::named("raw_ptr"), AccessKind::Read, 2),
    ];
    let plan = plan_lifts(TextRange::default(), &xs, |_| {
        Some((shape(ExprForm::BarePlace, true, true), TextRange::default()))
    })
    .unwrap();
    assert!(
        plan.lifts.is_empty(),
        "expected no lifts; got {:?}",
        plan.lifts
    );
}

#[test]
fn case_15_f_gp_p_nested_call_owned_return_lifts_gp() {
    // f(g(p), p) — g returns owned T. Lift arg 0's call.
    let xs = vec![
        at(0, Place::named("p"), AccessKind::Call, 0),
        at(1, Place::named("p"), AccessKind::MutBorrow, 1),
    ];
    let plan = plan_lifts(TextRange::default(), &xs, |sid| {
        if sid == SubexprId(0) {
            Some((
                shape(ExprForm::NestedCall, false, true),
                TextRange::default(),
            ))
        } else {
            Some((
                shape(ExprForm::BarePlace, false, true),
                TextRange::default(),
            ))
        }
    })
    .unwrap();
    assert_eq!(plan.lifts.len(), 1);
    // arg 1 is MutBorrow (most destructive), arg 0 is Call — Step 3 picks
    // the less destructive partner, which is Call at arg 0.
    assert_eq!(plan.lifts[0].arg_index, 0);
    assert_eq!(plan.lifts[0].strategy, LiftStrategy::BindValue);
}
