//! `xj-improve-lift-call-args` — lift call-argument subexpressions to satisfy
//! borrowck.
//!
//! Implements the algorithm described in `original_spec.md`:
//! given `f(e1, ..., en)`, rewrite into a sequence of
//! `let` bindings followed by a call whose arguments borrow-check.
//!
//! The crate is laid out so that the algorithm proper does not depend on
//! rust-analyzer:
//!
//! - [`access`] — `Access`, `Place`, conflict detection (pure data).
//! - [`lift`] — Step 3-5: choose what to lift and verify feasibility.
//! - [`emit`] — Step 6: render text edits from a lift plan.
//! - [`analysis`] — bridge to rust-analyzer's HIR (`ra_ap_*`).
//!
//! The driver in `main.rs` wires them together over a Cargo workspace.

pub mod access;
pub mod analysis;
pub mod emit;
pub mod lift;
pub mod text_edit;
