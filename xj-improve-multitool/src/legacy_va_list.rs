use std::{
    collections::HashMap,
    io,
    path::{Path, PathBuf},
    sync::{Arc, Mutex},
};

use rustc_span::source_map::{FileLoader, RealFileLoader};

/// Original source for files whose old c-variadic syntax needs adapting for
/// the plugin's newer compiler. The trimmer uses this text when writing files.
pub type OriginalSources = Arc<Mutex<HashMap<PathBuf, OriginalSource>>>;

#[derive(Clone)]
pub struct OriginalSource {
    pub text: String,
    /// Byte offsets immediately after each inserted `next_` in the adapted source.
    pub insertion_ends: Vec<usize>,
}

impl OriginalSource {
    pub fn original_offset(&self, adapted_offset: usize) -> usize {
        let mut inserted_bytes = 0;
        for &end in &self.insertion_ends {
            if adapted_offset < end - 5 {
                break;
            }
            if adapted_offset < end {
                return end - 5 - inserted_bytes;
            }
            inserted_bytes += 5;
        }
        adapted_offset - inserted_bytes
    }
}

pub struct LegacyVaListFileLoader {
    originals: OriginalSources,
}

impl LegacyVaListFileLoader {
    pub fn new(originals: OriginalSources) -> Self {
        Self { originals }
    }
}

pub fn canonical_path(path: &Path) -> PathBuf {
    path.canonicalize().unwrap_or_else(|_| path.to_path_buf())
}

pub fn adapt_legacy_va_list(source: &str) -> (String, Vec<usize>) {
    const OLD_TYPE: &str = "::core::ffi::VaListImpl";
    const NEW_TYPE: &str = "::core::ffi::VaList";
    const OLD_METHOD: &str = ".as_va_list()";

    if !source.contains(OLD_TYPE) {
        return (source.to_owned(), Vec::new());
    }

    // Keep these replacements the same length. The `arg` rewrite below adds
    // bytes, so its offsets must be translated when trimming original source.
    let padded_type = format!("{NEW_TYPE}{}", " ".repeat(OLD_TYPE.len() - NEW_TYPE.len()));
    let adapted = source
        .replace(OLD_TYPE, &padded_type)
        .replace(OLD_METHOD, &" ".repeat(OLD_METHOD.len()));
    assert_eq!(adapted.len(), source.len());

    let mut rewritten = String::with_capacity(adapted.len());
    let mut insertion_ends = Vec::new();
    let mut rest = adapted.as_str();
    let qualified_type = format!("{padded_type}::");
    while let Some(index) = rest.find("arg") {
        let (prefix, from_method) = rest.split_at(index);
        rewritten.push_str(prefix);
        let is_call = from_method[3..].starts_with('(') || from_method[3..].starts_with("::<");
        let is_va_arg = rewritten.ends_with('.') || rewritten.ends_with(&qualified_type);
        if is_call && is_va_arg {
            rewritten.push_str("next_");
            insertion_ends.push(rewritten.len());
        }
        rewritten.push_str("arg");
        rest = &from_method[3..];
    }
    rewritten.push_str(rest);
    (rewritten, insertion_ends)
}

impl FileLoader for LegacyVaListFileLoader {
    fn file_exists(&self, path: &Path) -> bool {
        RealFileLoader.file_exists(path)
    }

    fn read_file(&self, path: &Path) -> io::Result<String> {
        let original = RealFileLoader.read_file(path)?;
        if path.extension().is_none_or(|extension| extension != "rs") {
            return Ok(original);
        }

        let (adapted, insertion_ends) = adapt_legacy_va_list(&original);
        if adapted != original {
            self.originals
                .lock()
                .expect("original source map was poisoned")
                .insert(
                    canonical_path(path),
                    OriginalSource {
                        text: original,
                        insertion_ends,
                    },
                );
        }
        Ok(adapted)
    }

    fn read_binary_file(&self, path: &Path) -> io::Result<Arc<[u8]>> {
        RealFileLoader.read_binary_file(path)
    }

    fn current_directory(&self) -> io::Result<PathBuf> {
        RealFileLoader.current_directory()
    }
}

#[cfg(test)]
mod tests {
    use super::{OriginalSource, adapt_legacy_va_list};

    #[test]
    fn rewrites_plain_and_turbofish_calls() {
        let original = "let x: ::core::ffi::VaListImpl; x.arg(); x.arg::<i32>(); ::core::ffi::VaListImpl::arg::<i32>(&mut x); x.argument();";
        let (adapted, insertion_ends) = adapt_legacy_va_list(original);
        assert!(adapted.contains("::core::ffi::VaList    "));
        assert!(adapted.contains("x.next_arg(); x.next_arg::<i32>();"));
        assert!(adapted.contains("x.argument();"));
        assert!(adapted.contains("::core::ffi::VaList    ::next_arg::<i32>(&mut x)"));
        assert_eq!(insertion_ends.len(), 3);

        let source = OriginalSource {
            text: original.into(),
            insertion_ends,
        };
        let second_call = adapted.find("x.next_arg::<i32>").unwrap();
        assert_eq!(
            source.original_offset(second_call),
            original.find("x.arg::<i32>").unwrap()
        );
        assert_eq!(source.original_offset(adapted.len()), original.len());
    }

    #[test]
    fn leaves_unrelated_source_unchanged() {
        let original = "x.arg::<i32>()";
        assert_eq!(
            adapt_legacy_va_list(original),
            (original.into(), Vec::new())
        );
    }
}
