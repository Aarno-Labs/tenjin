use std::{
    collections::HashMap,
    io,
    path::{Path, PathBuf},
    sync::{Arc, Mutex},
};

use rustc_span::source_map::{FileLoader, RealFileLoader};

/// Original source for files whose old syntax needs adapting for the plugin's
/// newer compiler. The trimmer uses this text when writing files.
pub type OriginalSources = Arc<Mutex<HashMap<PathBuf, OriginalSource>>>;

#[derive(Clone)]
pub struct OriginalSource {
    pub text: String,
    pub expansions: Vec<SourceExpansion>,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct SourceExpansion {
    /// Byte range occupied by the replacement in the compiler's source.
    adapted_start: usize,
    adapted_end: usize,
    /// Length of the text replaced in the original source (zero for insertions).
    original_len: usize,
}

impl OriginalSource {
    pub fn original_offset(&self, adapted_offset: usize) -> usize {
        let mut growth = 0;
        for expansion in &self.expansions {
            if adapted_offset < expansion.adapted_start {
                break;
            }
            if adapted_offset < expansion.adapted_end {
                return expansion.adapted_start - growth;
            }
            growth += expansion.adapted_end - expansion.adapted_start - expansion.original_len;
        }
        adapted_offset - growth
    }
}

pub struct LegacySourceFileLoader {
    originals: OriginalSources,
}

impl LegacySourceFileLoader {
    pub fn new(originals: OriginalSources) -> Self {
        Self { originals }
    }
}

pub fn canonical_path(path: &Path) -> PathBuf {
    path.canonicalize().unwrap_or_else(|_| path.to_path_buf())
}

fn atomic_ordering(suffix: &str) -> Option<&'static str> {
    match suffix {
        "relaxed" => Some("Relaxed"),
        "release" => Some("Release"),
        "acquire" => Some("Acquire"),
        "acqrel" => Some("AcqRel"),
        "seqcst" => Some("SeqCst"),
        _ => None,
    }
}

fn adapt_atomic_intrinsic(name: &str) -> Option<String> {
    let name = name.strip_prefix("atomic_")?;
    let (base, order_count, type_count) = [
        ("cxchgweak", 2, 1),
        ("cxchg", 2, 1),
        ("fence", 1, 0),
        ("load", 1, 1),
        ("store", 1, 1),
        ("xchg", 1, 1),
        ("xadd", 1, 2),
        ("xsub", 1, 2),
        ("or", 1, 2),
        ("xor", 1, 2),
        ("nand", 1, 2),
        ("and", 1, 2),
    ]
    .into_iter()
    .find(|(base, _, _)| name.starts_with(&format!("{base}_")))?;

    let order_suffixes = name.strip_prefix(base)?.strip_prefix('_')?;
    let orders = order_suffixes.split('_').collect::<Vec<_>>();
    if orders.len() != order_count {
        return None;
    }
    let orders = orders
        .into_iter()
        .map(atomic_ordering)
        .collect::<Option<Vec<_>>>()?;
    let type_args = std::iter::repeat_n("_".to_owned(), type_count);
    let order_args = orders
        .iter()
        .map(|order| format!("{{ ::core::intrinsics::AtomicOrdering::{order} }}"));
    let args = type_args.chain(order_args).collect::<Vec<_>>().join(", ");
    Some(format!("atomic_{base}::<{args}>"))
}

pub fn adapt_legacy_source(source: &str) -> (String, Vec<SourceExpansion>) {
    const OLD_TYPE: &str = "::core::ffi::VaListImpl";
    const NEW_TYPE: &str = "::core::ffi::VaList";
    const OLD_METHOD: &str = ".as_va_list()";
    const INTRINSICS_PREFIX: &str = "::core::intrinsics::";

    // Keep these replacements the same length. The method and intrinsic
    // rewrites below expand the source, so their offsets must be translated
    // when trimming the original source.
    let padded_type = format!("{NEW_TYPE}{}", " ".repeat(OLD_TYPE.len() - NEW_TYPE.len()));
    let has_legacy_va_list = source.contains(OLD_TYPE);
    let adapted = if has_legacy_va_list {
        source
            .replace(OLD_TYPE, &padded_type)
            .replace(OLD_METHOD, &" ".repeat(OLD_METHOD.len()))
    } else {
        source.to_owned()
    };
    assert_eq!(adapted.len(), source.len());

    let mut rewritten = String::with_capacity(adapted.len());
    let mut expansions = Vec::new();
    let qualified_type = format!("{padded_type}::");
    let mut cursor = 0;
    while cursor < adapted.len() {
        let rest = &adapted[cursor..];
        let next_arg = has_legacy_va_list.then(|| rest.find("arg")).flatten();
        let next_atomic = rest.find(INTRINSICS_PREFIX);
        let atomic_first =
            next_atomic.is_some_and(|atomic| next_arg.is_none_or(|arg| atomic < arg));

        if atomic_first {
            let path_start = cursor + next_atomic.unwrap();
            let name_start = path_start + INTRINSICS_PREFIX.len();
            rewritten.push_str(&adapted[cursor..name_start]);
            let name_end = name_start
                + adapted[name_start..]
                    .bytes()
                    .take_while(|byte| byte.is_ascii_alphanumeric() || *byte == b'_')
                    .count();
            let name = &adapted[name_start..name_end];
            let is_call = adapted[name_end..].trim_start().starts_with('(');
            if is_call && let Some(replacement) = adapt_atomic_intrinsic(name) {
                // Only the callee changes; nested argument expressions and
                // tuple-field accesses remain byte-for-byte untouched.
                assert!(replacement.len() >= name.len());
                let adapted_start = rewritten.len();
                rewritten.push_str(&replacement);
                expansions.push(SourceExpansion {
                    adapted_start,
                    adapted_end: rewritten.len(),
                    original_len: name.len(),
                });
            } else {
                rewritten.push_str(name);
            }
            cursor = name_end;
            continue;
        }

        let Some(index) = next_arg else {
            rewritten.push_str(rest);
            break;
        };
        let arg_start = cursor + index;
        rewritten.push_str(&adapted[cursor..arg_start]);
        let from_method = &adapted[arg_start..];
        let is_call = from_method[3..].starts_with('(') || from_method[3..].starts_with("::<");
        let is_va_arg =
            adapted[..arg_start].ends_with('.') || adapted[..arg_start].ends_with(&qualified_type);
        if is_call && is_va_arg {
            let adapted_start = rewritten.len();
            rewritten.push_str("next_");
            expansions.push(SourceExpansion {
                adapted_start,
                adapted_end: rewritten.len(),
                original_len: 0,
            });
        }
        rewritten.push_str("arg");
        cursor = arg_start + 3;
    }
    (rewritten, expansions)
}

impl FileLoader for LegacySourceFileLoader {
    fn file_exists(&self, path: &Path) -> bool {
        RealFileLoader.file_exists(path)
    }

    fn read_file(&self, path: &Path) -> io::Result<String> {
        let original = RealFileLoader.read_file(path)?;
        if path.extension().is_none_or(|extension| extension != "rs") {
            return Ok(original);
        }

        let (adapted, expansions) = adapt_legacy_source(&original);
        if adapted != original {
            self.originals
                .lock()
                .expect("original source map was poisoned")
                .insert(
                    canonical_path(path),
                    OriginalSource {
                        text: original,
                        expansions,
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
    use super::{OriginalSource, adapt_legacy_source};

    #[test]
    fn rewrites_plain_and_turbofish_calls() {
        let original = "let x: ::core::ffi::VaListImpl; x.arg(); x.arg::<i32>(); ::core::ffi::VaListImpl::arg::<i32>(&mut x); x.argument();";
        let (adapted, expansions) = adapt_legacy_source(original);
        assert!(adapted.contains("::core::ffi::VaList    "));
        assert!(adapted.contains("x.next_arg(); x.next_arg::<i32>();"));
        assert!(adapted.contains("x.argument();"));
        assert!(adapted.contains("::core::ffi::VaList    ::next_arg::<i32>(&mut x)"));
        assert_eq!(expansions.len(), 3);

        let source = OriginalSource {
            text: original.into(),
            expansions,
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
        assert_eq!(adapt_legacy_source(original), (original.into(), Vec::new()));
    }

    #[test]
    fn rewrites_atomic_cxchg_without_touching_arguments_or_tuple_access() {
        let original =
            "::core::intrinsics::atomic_cxchg_seqcst_seqcst(pc, f(c, g(1, 2)), new_string).1";
        let (adapted, expansions) = adapt_legacy_source(original);
        assert_eq!(
            adapted,
            "::core::intrinsics::atomic_cxchg::<_, { ::core::intrinsics::AtomicOrdering::SeqCst }, { ::core::intrinsics::AtomicOrdering::SeqCst }>(pc, f(c, g(1, 2)), new_string).1"
        );
        assert_eq!(expansions.len(), 1);
        let source = OriginalSource {
            text: original.into(),
            expansions,
        };
        let adapted_args = adapted.find("(pc, f(").unwrap();
        let original_args = original.find("(pc, f(").unwrap();
        assert_eq!(source.original_offset(adapted_args), original_args);
        assert_eq!(source.original_offset(adapted.len()), original.len());
    }

    #[test]
    fn rewrites_other_legacy_atomic_orderings_and_leaves_new_intrinsics() {
        let original = "::core::intrinsics::atomic_cxchgweak_acqrel_acquire(p, old, new); ::core::intrinsics::atomic_load_seqcst(p); ::core::intrinsics::atomic_cxchg::<_, { ::core::intrinsics::AtomicOrdering::SeqCst }, { ::core::intrinsics::AtomicOrdering::SeqCst }>(p, old, new);";
        let (adapted, expansions) = adapt_legacy_source(original);
        assert!(adapted.contains("atomic_cxchgweak::<_, { ::core::intrinsics::AtomicOrdering::AcqRel }, { ::core::intrinsics::AtomicOrdering::Acquire }>(p, old, new)"));
        assert!(
            adapted.contains("atomic_load::<_, { ::core::intrinsics::AtomicOrdering::SeqCst }>(p)")
        );
        assert!(adapted.contains("atomic_cxchg::<_, { ::core::intrinsics::AtomicOrdering::SeqCst }, { ::core::intrinsics::AtomicOrdering::SeqCst }>(p, old, new)"));
        assert_eq!(expansions.len(), 2);
        let source = OriginalSource {
            text: original.into(),
            expansions,
        };
        assert_eq!(source.original_offset(adapted.len()), original.len());
    }

    #[test]
    fn maps_offsets_across_both_compatibility_rewrites() {
        let original = "let ap: ::core::ffi::VaListImpl; ap.arg::<i32>(); ::core::intrinsics::atomic_cxchg_seqcst_seqcst(p, old, new); fn after() {}";
        let (adapted, expansions) = adapt_legacy_source(original);
        assert_eq!(expansions.len(), 2);
        let source = OriginalSource {
            text: original.into(),
            expansions,
        };
        assert_eq!(
            source.original_offset(adapted.find("fn after").unwrap()),
            original.find("fn after").unwrap()
        );
        assert_eq!(source.original_offset(adapted.len()), original.len());
    }
}
