//! Minimal text-edit type.
//!
//! Previous versions of rust-analyzer published `ra_ap_text_edit`, but it
//! stopped being published as a separate crate around 0.0.241. Reproduce
//! the slice we actually need here so the rest of the crate doesn't have
//! to care.
//!
//! Semantics: a `TextEdit` is a sequence of non-overlapping `Indel`s
//! sorted by start offset. `apply` rewrites a `String` in place.

use ra_ap_syntax::{TextRange, TextSize};

#[derive(Debug, Clone)]
pub struct Indel {
    pub delete: TextRange,
    pub insert: String,
}

#[derive(Debug, Clone, Default)]
pub struct TextEdit {
    indels: Vec<Indel>,
}

impl TextEdit {
    pub fn is_empty(&self) -> bool {
        self.indels.is_empty()
    }

    pub fn apply(&self, text: &mut String) {
        // Apply in reverse so earlier offsets remain valid.
        for indel in self.indels.iter().rev() {
            let start: usize = indel.delete.start().into();
            let end: usize = indel.delete.end().into();
            text.replace_range(start..end, &indel.insert);
        }
    }

    pub fn union(&mut self, other: TextEdit) -> Result<(), TextEdit> {
        // Disjoint-only union: merge sorted indels, error on overlap.
        let mut combined = self.indels.clone();
        combined.extend(other.indels.iter().cloned());
        combined.sort_by_key(|i| (i.delete.start(), i.delete.end()));
        for w in combined.windows(2) {
            if w[0].delete.end() > w[1].delete.start() {
                return Err(other);
            }
        }
        self.indels = combined;
        Ok(())
    }
}

#[derive(Debug, Default)]
pub struct TextEditBuilder {
    indels: Vec<Indel>,
}

impl TextEditBuilder {
    pub fn insert(&mut self, pos: TextSize, text: String) {
        self.indels.push(Indel {
            delete: TextRange::empty(pos),
            insert: text,
        });
    }

    pub fn replace(&mut self, range: TextRange, text: String) {
        self.indels.push(Indel {
            delete: range,
            insert: text,
        });
    }

    pub fn finish(mut self) -> TextEdit {
        self.indels
            .sort_by_key(|i| (i.delete.start(), i.delete.end()));
        TextEdit {
            indels: self.indels,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn apply_reverses_then_inserts() {
        let mut b = TextEditBuilder::default();
        b.insert(TextSize::from(0), "// prefix\n".into());
        b.replace(
            TextRange::new(TextSize::from(3), TextSize::from(6)),
            "BAR".into(),
        );
        let edit = b.finish();
        let mut text = "foofoo".to_string();
        edit.apply(&mut text);
        assert_eq!(text, "// prefix\nfooBAR");
    }
}
