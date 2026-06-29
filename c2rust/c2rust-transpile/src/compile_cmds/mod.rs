use std::collections::{HashMap, HashSet};
use std::fs::File;
use std::path::{Path, PathBuf};
use std::rc::Rc;

use failure::Error;
use log::warn;
use regex::Regex;
use serde_derive::{Deserialize, Serialize};

#[derive(Deserialize, Serialize, Debug, Default, Clone)]
pub struct CompileCmd {
    /// The working directory of the compilation. All paths specified in the command
    /// or file fields must be either absolute or relative to this directory.
    pub directory: PathBuf,
    /// The main translation unit source processed by this compilation step. This is
    /// used by tools as the key into the compilation database. There can be multiple
    /// command objects for the same file, for example if the same source file is compiled
    /// with different configurations.
    pub file: PathBuf,
    /// The compile command executed. After JSON unescaping, this must be a valid command
    /// to rerun the exact compilation step for the translation unit in the environment
    /// the build system uses. Parameters use shell quoting and shell escaping of quotes,
    /// with ‘"’ and ‘\’ being the only special characters. Shell expansion is not supported.
    #[serde(skip_deserializing)]
    pub command: Option<String>,
    /// The compile command executed as list of strings. Either arguments or command is required.
    #[serde(default, skip_deserializing)]
    pub arguments: Vec<String>,
    /// The name of the output created by this compilation step. This field is optional. It can
    /// be used to distinguish different processing modes of the same input file.
    pub output: Option<String>,
}

impl CompileCmd {
    pub fn abs_file(&self) -> PathBuf {
        match self.file.is_absolute() {
            true => self.file.clone(),
            false => {
                let path = self.directory.join(&self.file);
                let e = format!("could not canonicalize {}", path.display());
                path.canonicalize().expect(&e)
            }
        }
    }
}

// XREF:c2rust_target_link_type
#[derive(Deserialize, Debug, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum LinkType {
    Exe,
    Shared,
    Static,
}

impl LinkType {
    pub fn is_library(&self) -> bool {
        match self {
            LinkType::Exe => false,
            LinkType::Shared => true,
            LinkType::Static => true,
        }
    }

    pub fn as_cargo_types(&self) -> &str {
        match self {
            LinkType::Exe => "\"rlib\"",
            LinkType::Shared => "\"cdylib\", \"rlib\"",
            LinkType::Static => "\"staticlib\", \"rlib\"",
        }
    }
}

#[derive(Deserialize, Debug)]
pub struct LinkCmd {
    /// All input files going into this link
    pub inputs: Vec<String>,
    /// The output file; this is taken from the `CompileCmd`
    #[serde(default)]
    pub output: Option<String>,
    /// List of libraries to link in (without `-l` prefix)
    pub libs: Vec<String>,
    /// List of library directories
    pub lib_dirs: Vec<PathBuf>,
    /// What type of binary we're building
    pub r#type: LinkType,
    /// Input files in `CompileCmd` form
    #[serde(default)]
    pub cmd_inputs: Vec<Rc<CompileCmd>>,
    #[serde(default)]
    pub top_level: bool,
}

/// Lexically normalize a path, resolving `.` and `..` components without
/// touching the filesystem (i.e. without following symlinks or requiring
/// the path to exist). This is used to match link-command inputs against
/// compile-command outputs: the same object file may be spelled differently
/// (e.g. `CMakeFiles/util.dir/foo.c.o` vs `../util/CMakeFiles/util.dir/foo.c.o`)
/// depending on the working directory of the command that references it.
fn normalize_lexically(p: &Path) -> PathBuf {
    use std::path::Component;
    let mut out = Vec::new();
    for comp in p.components() {
        match comp {
            Component::CurDir => {}
            Component::ParentDir if matches!(out.last(), Some(Component::Normal(_))) => {
                out.pop();
            }
            c => out.push(c),
        }
    }
    out.iter().collect()
}

/// Convert a linear vector of `CompileCmd`s into a DAG of `LinkCmd`s and `CompileCmd`s
fn build_link_commands(mut v: Vec<Rc<CompileCmd>>) -> Result<Vec<LinkCmd>, Error> {
    // Map each compile command's output to its index, keyed on the resolved
    // absolute path (output joined with the command's working directory and
    // lexically normalized) so that references from other directories match.
    let mut output_map = HashMap::new();
    for (idx, ccmd) in v.iter().enumerate() {
        if let Some(ref output) = ccmd.output {
            output_map.insert(normalize_lexically(&ccmd.directory.join(output)), idx);
        }
    }

    let mut seen_ccmds = HashSet::new();
    let mut res = vec![];
    for (idx, ccmd) in v.iter().enumerate() {
        let lcmd = match ccmd.file.strip_prefix("/c2rust/link/") {
            Ok(lcmd) => lcmd.to_str().unwrap(),
            Err(_) => continue,
        };
        let mut lcmd: LinkCmd = serde_bencode::from_str(lcmd)?;

        lcmd.output = ccmd.output.clone();
        for inp in &lcmd.inputs {
            // Resolve the input against the link command's working directory
            // before looking it up, mirroring how `output_map` is keyed.
            let key = normalize_lexically(&ccmd.directory.join(inp));
            if let Some(ccmd_idx) = output_map.get(&key) {
                let inp_ccmd = Rc::clone(&v[*ccmd_idx]);
                lcmd.cmd_inputs.push(inp_ccmd);
                seen_ccmds.insert(*ccmd_idx);
            }
        }

        res.push(lcmd);
        seen_ccmds.insert(idx);
    }

    // TODO: add binaries

    // Check if we have left-over compile commands; if we do,
    // bind them to the crate itself (which becomes a `staticlib` or `rlib`)
    let mut idx = 0;
    v.retain(|_| {
        idx += 1;
        !seen_ccmds.contains(&(idx - 1))
    });
    if !v.is_empty() {
        let lcmd = LinkCmd {
            // FIXME: this doesn't catch all of them; do we need to???
            inputs: v.iter().filter_map(|ccmd| ccmd.output.clone()).collect(),
            output: None,
            libs: vec![],
            lib_dirs: vec![],
            r#type: LinkType::Static,
            cmd_inputs: v,
            top_level: true,
        };
        res.push(lcmd);
    }

    Ok(res)
}

/// some build scripts repeatedly compile the same input file with different
/// command line flags thus creating multiple outputs. We remove any duplicates
/// in the order we see them and warn the user.
fn filter_duplicate_cmds(v: Vec<Rc<CompileCmd>>) -> Vec<Rc<CompileCmd>> {
    let mut seen = HashSet::new();
    let mut cmds = vec![];

    for cmd in v {
        let absf = cmd.abs_file();
        if seen.contains(&absf) {
            warn!("Skipping duplicate compilation cmd for {}", absf.display());
            continue;
        }
        seen.insert(absf);
        cmds.push(cmd)
    }

    cmds
}

/// Read `compile_commands` file, optionally ignore any entries not matching
/// `filter`, and filter out any .S files since they're likely assembly files.
pub fn get_compile_commands(
    compile_commands: &Path,
    filter: &Option<Regex>,
) -> Result<Vec<LinkCmd>, Error> {
    let f = std::io::BufReader::new(File::open(compile_commands)?); // open read-only

    // Read the JSON contents of the file as an instance of `Value`
    let v: Vec<Rc<CompileCmd>> = serde_json::from_reader(f)?;

    // apply the filter argument, if any
    let v = if let Some(re) = filter {
        v.into_iter()
            .filter(|c| re.is_match(c.file.to_str().unwrap()))
            .collect::<Vec<Rc<CompileCmd>>>()
    } else {
        v
    };

    // Filter out any assembly files
    let v = v
        .into_iter()
        .filter(|c| {
            let file = c.file.to_str().unwrap();
            let likely_asm = file.ends_with(".S") || file.ends_with(".s");
            !likely_asm
        })
        .collect::<Vec<Rc<CompileCmd>>>();

    let mut lcmds = build_link_commands(v)?;

    for lcmd in &mut lcmds {
        let inputs = std::mem::take(&mut lcmd.cmd_inputs);
        let inputs = filter_duplicate_cmds(inputs);
        lcmd.cmd_inputs = inputs;
    }

    Ok(lcmds)
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Build a `bencode` string for a `LinkCmd` `file` field, of the shape
    /// produced by the build-command interceptor (`/c2rust/link/<bencode>`).
    fn link_file(inputs: &[&str], ty: &str) -> String {
        // bencode dict, keys in sorted order: inputs, lib_dirs, libs, type
        let mut s = String::from("d");
        s.push_str("6:inputsl");
        for inp in inputs {
            s.push_str(&format!("{}:{}", inp.len(), inp));
        }
        s.push('e');
        s.push_str("8:lib_dirsle");
        s.push_str("4:libsle");
        s.push_str(&format!("4:type{}:{}", ty.len(), ty));
        s.push('e');
        format!("/c2rust/link/{s}")
    }

    fn compile_cmd(directory: &str, file: &str, output: Option<&str>) -> Rc<CompileCmd> {
        Rc::new(CompileCmd {
            directory: PathBuf::from(directory),
            file: PathBuf::from(file),
            command: None,
            arguments: vec![],
            output: output.map(ToOwned::to_owned),
        })
    }

    /// A link command whose working directory differs from the directory of
    /// the compile command that produced one of its inputs. The input is
    /// spelled relative to the link command's directory (`../util/...`), while
    /// the compile command records its output relative to its own directory
    /// (`CMakeFiles/...`). These must still be matched to the same object so
    /// the module lands in the member crate rather than the top-level package.
    #[test]
    fn cross_directory_object_is_matched() {
        let v = vec![
            // util/foo.c.o, compiled in the util directory
            compile_cmd(
                "/build/src/util",
                "/build/src/util/foo.c",
                Some("CMakeFiles/util.dir/foo.c.o"),
            ),
            // libgit2/bar.c.o, compiled in the libgit2 directory
            compile_cmd(
                "/build/src/libgit2",
                "/build/src/libgit2/bar.c",
                Some("CMakeFiles/libgit2.dir/bar.c.o"),
            ),
            // The shared-library link command, run from the libgit2 directory,
            // referencing the util object via a `../util/...` relative path.
            compile_cmd(
                "/build/src/libgit2",
                &link_file(
                    &[
                        "../util/CMakeFiles/util.dir/foo.c.o",
                        "CMakeFiles/libgit2.dir/bar.c.o",
                    ],
                    "shared",
                ),
                Some("../../libdriver.so"),
            ),
        ];

        let lcmds = build_link_commands(v).unwrap();

        // There should be exactly one link command and no synthetic top-level
        // leftover crate: both objects were consumed by the shared library.
        assert_eq!(lcmds.len(), 1, "unexpected leftover top-level crate");
        let lcmd = &lcmds[0];
        assert!(!lcmd.top_level);
        assert_eq!(lcmd.r#type, LinkType::Shared);

        let consumed: Vec<_> = lcmd
            .cmd_inputs
            .iter()
            .map(|c| c.file.to_str().unwrap())
            .collect();
        assert!(
            consumed.contains(&"/build/src/util/foo.c"),
            "cross-directory util object was not matched: {consumed:?}"
        );
        assert!(consumed.contains(&"/build/src/libgit2/bar.c"));
        assert_eq!(consumed.len(), 2);
    }

    /// An object that is genuinely not consumed by any link command should
    /// still fall through to the synthetic top-level static crate.
    #[test]
    fn unconsumed_object_becomes_top_level_crate() {
        let v = vec![
            compile_cmd(
                "/build/src/libgit2",
                "/build/src/libgit2/bar.c",
                Some("CMakeFiles/libgit2.dir/bar.c.o"),
            ),
            compile_cmd(
                "/build/src/orphan",
                "/build/src/orphan/orphan.c",
                Some("CMakeFiles/orphan.dir/orphan.c.o"),
            ),
            compile_cmd(
                "/build/src/libgit2",
                &link_file(&["CMakeFiles/libgit2.dir/bar.c.o"], "shared"),
                Some("../../libdriver.so"),
            ),
        ];

        let lcmds = build_link_commands(v).unwrap();

        assert_eq!(lcmds.len(), 2);
        let top_level = lcmds
            .iter()
            .find(|l| l.top_level)
            .expect("no top-level crate");
        assert_eq!(top_level.r#type, LinkType::Static);
        let orphan: Vec<_> = top_level
            .cmd_inputs
            .iter()
            .map(|c| c.file.to_str().unwrap())
            .collect();
        assert_eq!(orphan, vec!["/build/src/orphan/orphan.c"]);
    }
}
