use std::{
    collections::HashMap,
    io,
    path::{Path, PathBuf},
    sync::{Arc, Mutex},
};

use rustc_span::source_map::{FileLoader, RealFileLoader};

/// Original source for files whose old c-variadic syntax needs adapting for
/// the plugin's newer compiler. The trimmer uses this text when writing files.
pub type OriginalSources = Arc<Mutex<HashMap<PathBuf, String>>>;

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

pub fn adapt_legacy_va_list(source: &str) -> String {
    const OLD_TYPE: &str = "::core::ffi::VaListImpl";
    const NEW_TYPE: &str = "::core::ffi::VaList";
    const OLD_METHOD: &str = ".as_va_list()";

    if !source.contains(OLD_TYPE) {
        return source.to_owned();
    }

    // rustc's spans are byte offsets. Keep both replacements the same length
    // so that every span also indexes the original source correctly.
    let padded_type = format!("{NEW_TYPE}{}", " ".repeat(OLD_TYPE.len() - NEW_TYPE.len()));
    let adapted = source
        .replace(OLD_TYPE, &padded_type)
        .replace(OLD_METHOD, &" ".repeat(OLD_METHOD.len()));
    assert_eq!(adapted.len(), source.len());
    adapted
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

        let adapted = adapt_legacy_va_list(&original);
        if adapted != original {
            self.originals
                .lock()
                .expect("original source map was poisoned")
                .insert(canonical_path(path), original);
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
