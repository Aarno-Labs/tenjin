//! `xj-improve-relink` — wires C-symbol calls that go through `extern "C"`
//! foreign declarations directly to the Rust definition that exports them,
//! removing the redundant (and `unsafe`) foreign declarations.
//!
//! See [`relink`] for the analysis and [`collect`] for workspace
//! discovery.

pub mod collect;
pub mod manifest;
pub mod relink;
