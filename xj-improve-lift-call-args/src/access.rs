//! Access set and conflict detection.
//!
//! Implements the data model from the algorithm spec
//! (`original_spec.md`, "Definitions" and "Algorithm/Step 1-2"):
//!
//! - `Place`: a root variable plus a chain of `Projection`s.
//! - `AccessKind`: Read | Move | SharedBorrow | MutBorrow | Call.
//! - `Access`: one record per touch of a place by a call-argument subexpression.
//! - `conflicts_between`: pairwise conflict detection honoring splittable
//!   projections.
//!
//! Nothing in this module touches rust-analyzer; it operates on the abstract
//! types the front-end translates the AST into.

use ra_ap_syntax::TextRange;

/// A single projection step in a place expression.
///
/// `Field { splittable: true }` is a struct/tuple field accessed without
/// going through `Deref`/`Index` or a method call — Rust allows disjoint
/// borrows of sibling such fields.
#[derive(Debug, Clone, PartialEq, Eq, Hash)]
pub enum Projection {
    /// `place.field` — splittable iff the field is reached directly on a
    /// struct/tuple value (no smart-pointer Deref in the path).
    Field { name: String, splittable: bool },
    /// `place.0`, `place.1`, ... — tuple/tuple-struct field.
    TupleField { idx: u32, splittable: bool },
    /// `*place` — Deref. Not splittable.
    Deref,
    /// `place[idx]`. Treated opaquely: any two index projections on the
    /// same base are assumed to overlap unless `key` shows they cannot.
    Index { key: IndexKey },
}

/// Identifies an index expression. Two literal-integer keys with different
/// values are provably disjoint; everything else is treated as overlapping.
#[derive(Debug, Clone, PartialEq, Eq, Hash)]
pub enum IndexKey {
    Literal(i128),
    Opaque(u32),
}

impl Projection {
    /// Per spec, a "splittable projection" is a direct struct/tuple field
    /// access. Anything reached through `*`, `[]`, or a method call is not.
    pub fn is_splittable(&self) -> bool {
        matches!(
            self,
            Projection::Field {
                splittable: true,
                ..
            } | Projection::TupleField {
                splittable: true,
                ..
            }
        )
    }
}

/// A place: root variable + chain of projections.
#[derive(Debug, Clone, PartialEq, Eq, Hash)]
pub struct Place {
    /// The root variable name. Two places with different roots never
    /// conflict (unless one of them is the synthetic `Unknown` root).
    pub root: PlaceRoot,
    /// Projections from root, outermost-first. `p.a.b` is
    /// `[Field("a"), Field("b")]`.
    pub projections: Vec<Projection>,
}

#[derive(Debug, Clone, PartialEq, Eq, Hash)]
pub enum PlaceRoot {
    /// A named local, parameter, static, or const.
    Named(String),
    /// Stands in for a place we couldn't analyze (e.g. the join of a nested
    /// call's place arguments when no common prefix exists). Conflicts with
    /// every other place.
    Unknown,
}

impl Place {
    pub fn named(name: impl Into<String>) -> Self {
        Self {
            root: PlaceRoot::Named(name.into()),
            projections: Vec::new(),
        }
    }

    pub fn unknown() -> Self {
        Self {
            root: PlaceRoot::Unknown,
            projections: Vec::new(),
        }
    }

    pub fn pushed(mut self, proj: Projection) -> Self {
        self.projections.push(proj);
        self
    }

    /// True when `self` is a (non-strict) prefix of `other`, treating
    /// `Unknown` roots as overlapping everything.
    pub fn is_prefix_of(&self, other: &Place) -> bool {
        match (&self.root, &other.root) {
            (PlaceRoot::Unknown, _) | (_, PlaceRoot::Unknown) => true,
            (PlaceRoot::Named(a), PlaceRoot::Named(b)) => {
                if a != b {
                    return false;
                }
                if self.projections.len() > other.projections.len() {
                    return false;
                }
                self.projections
                    .iter()
                    .zip(other.projections.iter())
                    .all(|(a, b)| projections_equal(a, b))
            }
        }
    }

    /// "Potentially overlapping" for the purpose of Step 2: same root
    /// (including the synthetic `Unknown` root) flags a candidate. Whether
    /// the candidate is *really* a conflict is then refined by
    /// `resolved_by_splitting` in Step 3.1.
    pub fn overlaps(&self, other: &Place) -> bool {
        match (&self.root, &other.root) {
            (PlaceRoot::Unknown, _) | (_, PlaceRoot::Unknown) => true,
            (PlaceRoot::Named(a), PlaceRoot::Named(b)) => a == b,
        }
    }

    /// Returns the longest common projection prefix length on the SAME root.
    /// Returns `None` if the roots differ (and neither is Unknown).
    pub fn common_prefix_len(&self, other: &Place) -> Option<usize> {
        match (&self.root, &other.root) {
            (PlaceRoot::Named(a), PlaceRoot::Named(b)) if a == b => {
                let mut n = 0;
                for (lhs, rhs) in self.projections.iter().zip(other.projections.iter()) {
                    if projections_equal(lhs, rhs) {
                        n += 1;
                    } else {
                        break;
                    }
                }
                Some(n)
            }
            _ => None,
        }
    }
}

/// `Index { key }` is conservative: opaque keys overlap, literal keys with
/// the same value overlap, distinct literals do not.
fn projections_equal(a: &Projection, b: &Projection) -> bool {
    match (a, b) {
        (Projection::Field { name: n1, .. }, Projection::Field { name: n2, .. }) => n1 == n2,
        (Projection::TupleField { idx: i1, .. }, Projection::TupleField { idx: i2, .. }) => {
            i1 == i2
        }
        (Projection::Deref, Projection::Deref) => true,
        (Projection::Index { key: k1 }, Projection::Index { key: k2 }) => {
            match (k1, k2) {
                (IndexKey::Literal(a), IndexKey::Literal(b)) => a == b,
                // Anything else might overlap — treat as same step.
                _ => true,
            }
        }
        _ => false,
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum AccessKind {
    /// Read by value, type is `Copy`.
    Read,
    /// Read by value, type is not `Copy`.
    Move,
    /// `&place` or auto-ref to `&self`.
    SharedBorrow,
    /// `&mut place` or auto-ref to `&mut self`.
    MutBorrow,
    /// Opaque call whose return may borrow from arguments.
    Call,
}

impl AccessKind {
    pub fn is_destructive(self) -> bool {
        !matches!(self, AccessKind::Read | AccessKind::SharedBorrow)
    }

    /// Ordering used by Step 3's "less destructive" preference.
    /// Smaller value = less destructive = preferred to lift.
    pub fn destructiveness(self) -> u8 {
        match self {
            AccessKind::Read => 0,
            AccessKind::SharedBorrow => 1,
            AccessKind::Move => 2,
            AccessKind::Call => 3,
            AccessKind::MutBorrow => 4,
        }
    }
}

/// One access recorded by the Step-1 walk.
///
/// `subexpr_id` is an opaque key uniquely identifying the AST node that
/// produced this access. `outer_subexpr` identifies the top-level
/// argument expression that contains it: for a top-level access the two
/// are equal; for sub-accesses produced by recursing into a nested call,
/// `outer_subexpr` points back to the enclosing argument's outer
/// expression. Step 4 lifts that outer expression, since `&mut x` etc.
/// inside a nested call can only be resolved by hoisting the whole call
/// out, not by binding the inner borrow.
#[derive(Debug, Clone)]
pub struct Access {
    pub arg_index: usize,
    pub place: Place,
    pub kind: AccessKind,
    pub span: TextRange,
    pub subexpr_id: SubexprId,
    pub outer_subexpr: SubexprId,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct SubexprId(pub u32);

/// Per spec Step 2: two accesses conflict iff one place is a prefix of the
/// other AND at least one access is destructive AND they are not both
/// SharedBorrow.
///
/// "Both SharedBorrow" of an overlapping place is fine — Rust allows
/// arbitrarily many shared borrows.
pub fn raw_conflict(a: &Access, b: &Access) -> bool {
    if !a.place.overlaps(&b.place) {
        return false;
    }
    if matches!(
        (a.kind, b.kind),
        (AccessKind::SharedBorrow, AccessKind::SharedBorrow)
    ) {
        return false;
    }
    if a.kind == AccessKind::Read && b.kind == AccessKind::Read {
        // Two Copy reads of the same place are always fine.
        return false;
    }
    // At least one must be destructive (Move/MutBorrow/Call).
    a.kind.is_destructive() || b.kind.is_destructive()
}

/// Step 3.1: a conflict edge is "resolved by splitting" when both places are
/// reached through splittable projections from a common ancestor AND the two
/// places diverge at a splittable step (i.e. siblings, not prefix-of-each-
/// other).
///
/// Returns `true` if borrowck will accept the pair without lifting.
pub fn resolved_by_splitting(a: &Access, b: &Access) -> bool {
    let Some(prefix) = a.place.common_prefix_len(&b.place) else {
        return false;
    };
    let a_rest = &a.place.projections[prefix..];
    let b_rest = &b.place.projections[prefix..];

    // If one place is a prefix of the other, splitting cannot help.
    if a_rest.is_empty() || b_rest.is_empty() {
        return false;
    }

    // The divergence step on EACH side must be splittable.
    if !a_rest[0].is_splittable() || !b_rest[0].is_splittable() {
        return false;
    }

    // The shared prefix must itself be entirely splittable
    // (Step 3.1: "reached only through splittable projections from a
    // common ancestor"). The walk that produces accesses already enforces
    // splittable=false for projections that pass through Deref, so the
    // prefix check reduces to: no non-splittable projection appears before
    // the divergence.
    a.place.projections[..prefix]
        .iter()
        .all(|p| p.is_splittable())
}

/// Pairwise enumeration of raw conflict edges (Step 2). Skips intra-argument
/// pairs per the spec ("accesses from the same argument index do not
/// conflict ...").
pub fn conflict_edges(accesses: &[Access]) -> Vec<(usize, usize)> {
    let mut edges = Vec::new();
    for i in 0..accesses.len() {
        for j in (i + 1)..accesses.len() {
            let a = &accesses[i];
            let b = &accesses[j];
            if a.arg_index == b.arg_index {
                continue;
            }
            if raw_conflict(a, b) {
                edges.push((i, j));
            }
        }
    }
    edges
}

#[cfg(test)]
mod tests {
    use super::*;

    fn mk(arg: usize, root: &str, projs: Vec<Projection>, kind: AccessKind, id: u32) -> Access {
        Access {
            arg_index: arg,
            place: Place {
                root: PlaceRoot::Named(root.into()),
                projections: projs,
            },
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

    #[test]
    fn shared_borrows_never_conflict() {
        let a = mk(0, "p", vec![], AccessKind::SharedBorrow, 0);
        let b = mk(1, "p", vec![], AccessKind::SharedBorrow, 1);
        assert!(!raw_conflict(&a, &b));
    }

    #[test]
    fn mut_borrow_conflicts_with_overlapping_read() {
        let a = mk(0, "p", vec![], AccessKind::MutBorrow, 0);
        let b = mk(1, "p", vec![field("x", true)], AccessKind::Read, 1);
        assert!(raw_conflict(&a, &b));
    }

    #[test]
    fn disjoint_roots_never_conflict() {
        let a = mk(0, "p", vec![], AccessKind::MutBorrow, 0);
        let b = mk(1, "q", vec![], AccessKind::MutBorrow, 1);
        assert!(!raw_conflict(&a, &b));
    }

    #[test]
    fn splittable_siblings_resolve_without_lifting() {
        let a = mk(0, "p", vec![field("a", true)], AccessKind::MutBorrow, 0);
        let b = mk(1, "p", vec![field("b", true)], AccessKind::MutBorrow, 1);
        assert!(raw_conflict(&a, &b));
        assert!(resolved_by_splitting(&a, &b));
    }

    #[test]
    fn non_splittable_siblings_still_need_lifting() {
        let a = mk(0, "p", vec![field("a", false)], AccessKind::MutBorrow, 0);
        let b = mk(1, "p", vec![field("b", false)], AccessKind::MutBorrow, 1);
        assert!(raw_conflict(&a, &b));
        assert!(!resolved_by_splitting(&a, &b));
    }

    #[test]
    fn prefix_relation_blocks_splitting() {
        // p vs p.a — one is a prefix of the other, splitting can't help.
        let a = mk(0, "p", vec![], AccessKind::MutBorrow, 0);
        let b = mk(1, "p", vec![field("a", true)], AccessKind::SharedBorrow, 1);
        assert!(raw_conflict(&a, &b));
        assert!(!resolved_by_splitting(&a, &b));
    }

    #[test]
    fn intra_argument_pairs_are_ignored() {
        let xs = vec![
            mk(0, "p", vec![], AccessKind::MutBorrow, 0),
            mk(0, "p", vec![field("x", true)], AccessKind::Read, 1),
        ];
        assert!(conflict_edges(&xs).is_empty());
    }
}
