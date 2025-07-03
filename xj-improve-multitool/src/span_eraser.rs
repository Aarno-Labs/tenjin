use std::collections::HashMap;

use rustc_middle::ty::TyCtxt;
use rustc_span::{RelativeBytePos, SourceFile, Span, StableSourceFileId};

pub struct SpanEraser {
    spans_to_erase: SpansToErase,
    rewriting: SourceBeingRewritten,
}

impl SpanEraser {
    pub fn new() -> Self {
        SpanEraser {
            spans_to_erase: SpansToErase {
                directly: Vec::new(),
                for_attributes: Vec::new(),
            },
            rewriting: SourceBeingRewritten {
                rewritten_source: HashMap::new(),
            },
        }
    }

    pub fn add_span(&mut self, span: Span) {
        self.spans_to_erase.directly.push(span);
    }

    /// Two issues with attributes in HIR, in the context of us wanting to modify source:
    /// 1) Not all attributes have usable spans. Parsed attributes may be merged, destroying
    ///    the original span information (as far as I can tell...).
    /// 2) Derive attributes end up attached to the generated impls, rather than
    ///    the original item.
    ///
    /// Therefore we do something very dumb!
    /// For top level items, we look at the first character of preceding lines,
    /// as long as we see a `#` or a space, we assume it's an attribute.
    pub fn mark_top_item_span_for_attribute_erasure(&mut self, span: Span) {
        self.spans_to_erase.for_attributes.push(span);
    }

    pub fn erase_spans(&mut self, tcx: TyCtxt) {
        // First, directly erase the requested spans.
        for span in &self.spans_to_erase.directly {
            let (srcfile, src) = self.rewriting.get_source_file(tcx, *span);
            let rel_start = srcfile.relative_position(span.lo());
            let rel_end = srcfile.relative_position(span.hi());

            erase_span_in_source(rel_start, rel_end, src);
        }

        // Second, erase the attributes preceding certain spans.
        for item_span in &self.spans_to_erase.for_attributes {
            let (srcfile, src) = self.rewriting.get_source_file(tcx, *item_span);
            let srclines = src.lines().collect::<Vec<_>>();

            let item_start = srcfile.relative_position(item_span.lo());
            let item_start_lineno = srcfile
                .lookup_line(item_start)
                .expect("Item start should have a line");
            if item_start_lineno == 0 {
                continue;
            }

            fn line_chars_looks_like_outer_attribute(line: &str, allow_whitespace: bool) -> bool {
                if line.len() < 2 {
                    return false;
                }
                let mut chars = line.chars();
                let first_char = chars.next().unwrap();
                let second_char = chars.next().unwrap();
                let outer_attr = first_char == '#' && second_char == '[';
                let was_spaces = first_char.is_whitespace() && second_char.is_whitespace();
                outer_attr || (allow_whitespace && was_spaces)
            }

            if !line_chars_looks_like_outer_attribute(srclines[item_start_lineno - 1], false) {
                // Looks like this item wasn't preceded by an attribute, so we'll short circuit.
                continue;
            }

            // Invariant: lines from current_lineno .. item_start_lineno are attributes.
            let mut current_lineno = item_start_lineno;
            loop {
                if current_lineno == 0 {
                    break;
                }

                if !line_chars_looks_like_outer_attribute(srclines[current_lineno - 1_usize], true)
                {
                    break;
                }

                current_lineno -= 1;
            }

            let attr_rel_start = srcfile.lines()[current_lineno];
            let attr_rel_end = srcfile.lines()[item_start_lineno];

            erase_span_in_source(attr_rel_start, attr_rel_end, src);
        }
    }

    pub fn print_rewritten_source(
        &self,
        srcmap: &rustc_span::source_map::SourceMap,
        modify_in_place: bool,
        print_to_stdout: bool,
    ) {
        for (id, src) in &self.rewriting.rewritten_source {
            if print_to_stdout {
                println!("Rewritten source for {:?}:\n{}", id, src);
            }
            if modify_in_place {
                // Apply the modifications to the original source files.
                let srcfile = srcmap
                    .source_file_by_stable_id(*id)
                    .expect("Stable ID did not map to a source file");
                match &srcfile.name {
                    rustc_span::FileName::Real(rustc_span::RealFileName::LocalPath(path)) => {
                        if let Ok(mut file) = std::fs::OpenOptions::new()
                            .write(true)
                            .truncate(true)
                            .open(path)
                        {
                            use std::io::Write;
                            if let Err(e) = file.write_all(src.as_bytes()) {
                                eprintln!("Error writing to {:?}: {}", path, e);
                            }
                        } else {
                            eprintln!("Could not open {:?} for writing", path);
                        }
                    }
                    _ => {
                        eprintln!(
                            "Cannot modify source file with non-real name: {:?}",
                            srcfile.name
                        );
                    }
                }
            }
        }
    }
}

struct SpansToErase {
    directly: Vec<Span>,
    for_attributes: Vec<Span>,
}
struct SourceBeingRewritten {
    rewritten_source: HashMap<StableSourceFileId, String>,
}

impl SourceBeingRewritten {
    fn get_source_file(
        &mut self,
        tcx: TyCtxt,
        span: Span,
    ) -> (std::sync::Arc<SourceFile>, &mut String) {
        let srcfile = tcx.sess.source_map().lookup_source_file(span.lo());
        self.rewritten_source
            .entry(srcfile.stable_id)
            .or_insert_with(|| {
                srcfile
                    .src
                    .as_ref()
                    .expect("Source file should have... source...")
                    .to_string()
            });
        (
            srcfile.clone(),
            self.rewritten_source.get_mut(&srcfile.stable_id).unwrap(),
        )
    }
}

fn erase_span_in_source(rel_start: RelativeBytePos, rel_end: RelativeBytePos, src: &mut String) {
    let srcbytes = src.as_bytes();

    let mut spacified = String::new();
    for x in rel_start.0..rel_end.0 {
        let existing = srcbytes[x as usize];
        match existing {
            b'\n' | b'\r' | b' ' | b'\t' => spacified.push(existing as char),
            _ => spacified.push(' '),
        }
    }

    src.replace_range(rel_start.0 as usize..rel_end.0 as usize, &spacified);
}
