//! Conservative substitution of glibc's x86-64 `sys/stat.h` API.
//!
//! Match C declaration IDs before Rust renaming. A familiar spelling is not
//! sufficient: header provenance, field layout, signatures, and record uses
//! must all agree. Adding another target requires another verified layout and
//! field mapping; libc's Rust representation need not mirror the C spelling.

use std::collections::{HashMap, HashSet};

use crate::c_ast::iterators::{immediate_children_all_types, SomeId};
use crate::c_ast::*;

#[derive(Default)]
pub(crate) struct LibcReplacements {
    pub(crate) stat_records: HashSet<CDeclId>,
    pub(crate) functions: HashMap<CDeclId, String>,
    pub(crate) constants: HashMap<CDeclId, String>,
    /// A retained system stat exists but cannot be substituted. The driver
    /// uses this to choose the same representation across translation units.
    pub(crate) requires_stat_fallback: bool,
}

impl LibcReplacements {
    pub(crate) fn discover(ast: &TypedAstContext, enabled: bool) -> Self {
        if !enabled {
            return Self::default();
        }

        let supported_target = matches!(
            ast.target.as_str(),
            "x86_64-unknown-linux-gnu" | "x86_64-pc-linux-gnu"
        );
        let mut result = Self::default();
        for (&id, decl) in ast.iter_decls() {
            if matches!(&decl.kind, CDeclKind::Struct { name: Some(name), .. } if name == "stat")
                && system_stat_header(ast, decl)
            {
                if supported_target
                    && compatible_stat_layout(ast, id)
                    && supported_stat_uses(ast, id)
                {
                    result.stat_records.insert(id);
                } else {
                    result.requires_stat_fallback = true;
                }
            }
        }
        // One incompatible declaration disables the record mapping for the
        // entire TU; never emit half of a connected stat API using libc.
        if result.requires_stat_fallback {
            result.stat_records.clear();
        }
        if !supported_target {
            return result;
        }
        for (&id, decl) in ast.iter_decls() {
            if !system_stat_header(ast, decl) {
                continue;
            }
            match &decl.kind {
                CDeclKind::Function {
                    name,
                    typ,
                    body: None,
                    is_inline: false,
                    is_global: true,
                    attrs,
                    ..
                } if !attrs.iter().any(|attr| matches!(attr, Attribute::Alias(_)))
                    && compatible_function(ast, *typ, name, &result.stat_records) =>
                {
                    result.functions.insert(id, name.clone());
                }
                CDeclKind::MacroObject { name } if stat_constant(name).is_some() => {
                    // Checking the expanded value also catches unusual header
                    // versions, even if their names and paths look familiar.
                    let expected = stat_constant(name).unwrap();
                    if ast.macro_expansions.get(&id).is_some_and(|expansions| {
                        !expansions.is_empty()
                            && expansions
                                .iter()
                                .all(|&expr| integer_value(ast, expr) == Some(expected))
                    }) {
                        result.constants.insert(id, name.clone());
                    }
                }
                _ => {}
            }
        }
        result
    }
}

/// The exporter currently lacks Clang's system-header flag. Use the known
/// glibc include root plus the header suffix, excluding project/sysroot
/// lookalikes rather than assuming every file named `sys/stat.h` is libc.
fn system_stat_header(ast: &TypedAstContext, decl: &CDecl) -> bool {
    ast.get_source_path(decl).is_some_and(|path| {
        let clang_sysroot = std::env::var_os("CLANG_PATH").and_then(|clang| {
            let clang = std::path::PathBuf::from(clang);
            Some(clang.parent()?.parent()?.join("sysroot/usr/include"))
        });
        (path.starts_with("/usr/include")
            || clang_sysroot.is_some_and(|root| root.is_absolute() && path.starts_with(root)))
            && (path.ends_with("sys/stat.h")
                || path.ends_with("bits/struct_stat.h")
                || path.ends_with("bits/stat.h")
                // glibc repeats the same file-kind/permission macros here;
                // the later definition is the active macro declaration ID.
                || (matches!(decl.kind, CDeclKind::MacroObject { .. }) && path.ends_with("fcntl.h")))
    })
}

fn compatible_stat_layout(ast: &TypedAstContext, id: CDeclId) -> bool {
    let CDeclKind::Struct {
        fields: Some(fields),
        platform_byte_size: 144,
        platform_alignment: 8,
        is_packed: false,
        manual_alignment: None,
        max_field_alignment: None,
        ..
    } = &ast[id].kind
    else {
        return false;
    };
    // Offsets and widths are in bytes. Both C reserved fields are private in
    // libc::stat and must only be initialized through whole-object zeroing.
    let expected = [
        ("st_dev", 0, 8),
        ("st_ino", 8, 8),
        ("st_nlink", 16, 8),
        ("st_mode", 24, 4),
        ("st_uid", 28, 4),
        ("st_gid", 32, 4),
        ("__pad0", 36, 4),
        ("st_rdev", 40, 8),
        ("st_size", 48, 8),
        ("st_blksize", 56, 8),
        ("st_blocks", 64, 8),
        ("st_atim", 72, 16),
        ("st_mtim", 88, 16),
        ("st_ctim", 104, 16),
        ("__glibc_reserved", 120, 24),
    ];
    fields.len() == expected.len()
        && fields
            .iter()
            .zip(expected)
            .all(|(&field, (wanted, offset, width))| {
                let CDeclKind::Field {
                    name,
                    typ,
                    platform_bit_offset,
                    platform_type_bitwidth,
                    bitfield_width: None,
                    manual_alignment: None,
                } = &ast[field].kind
                else {
                    return false;
                };
                if name != wanted
                    || *platform_bit_offset != offset * 8
                    || *platform_type_bitwidth != width * 8
                {
                    return false;
                }
                let kind = &ast.resolve_type(typ.ctype).kind;
                match wanted {
                    "st_dev" | "st_ino" | "st_nlink" | "st_rdev" => {
                        matches!(kind, CTypeKind::ULong)
                    }
                    "st_mode" | "st_uid" | "st_gid" => matches!(kind, CTypeKind::UInt),
                    "__pad0" => matches!(kind, CTypeKind::Int),
                    "st_size" | "st_blksize" | "st_blocks" => matches!(kind, CTypeKind::Long),
                    "st_atim" | "st_mtim" | "st_ctim" => {
                        if let CTypeKind::Struct(id) = kind {
                            compatible_timespec_layout(ast, *id)
                        } else {
                            false
                        }
                    }
                    "__glibc_reserved" => matches!(kind, CTypeKind::ConstantArray(element, 3)
                if matches!(ast.resolve_type(*element).kind, CTypeKind::Long)),
                    _ => false,
                }
            })
}

fn compatible_timespec_layout(ast: &TypedAstContext, id: CDeclId) -> bool {
    let CDeclKind::Struct {
        fields: Some(fields),
        platform_byte_size: 16,
        platform_alignment: 8,
        is_packed: false,
        ..
    } = &ast[id].kind
    else {
        return false;
    };
    fields.len() == 2
        && fields
            .iter()
            .zip(["tv_sec", "tv_nsec"])
            .enumerate()
            .all(|(index, (&field, wanted))| {
                matches!(&ast[field].kind, CDeclKind::Field { name, typ, platform_bit_offset,
            platform_type_bitwidth: 64, bitfield_width: None, .. }
            if name == wanted && *platform_bit_offset == index as u64 * 64
                && matches!(ast.resolve_type(typ.ctype).kind, CTypeKind::Long))
            })
}

fn supported_stat_uses(ast: &TypedAstContext, record: CDeclId) -> bool {
    let mut parents: HashMap<CExprId, Vec<CExprId>> = HashMap::new();
    for (&id, _) in ast.iter_exprs() {
        for child in immediate_children_all_types(ast, SomeId::Expr(id)) {
            if let SomeId::Expr(child) = child {
                parents.entry(child).or_default().push(id);
            }
        }
    }
    for (&id, expr) in ast.iter_exprs() {
        match &expr.kind {
            CExprKind::Member(_, _, field, _, _) if ast.parents.get(field) == Some(&record) => {
                let name = ast[*field].kind.get_name().unwrap();
                if name.starts_with("__") {
                    return false;
                }
                if timestamp_prefix(name).is_some() {
                    // A scalar timestamp member has a libc equivalent. The
                    // timespec subobject itself does not, including &st_mtim,
                    // sizeof(st_mtim), struct copying, and assignment.
                    let mut todo = vec![id];
                    while let Some(child) = todo.pop() {
                        let Some(users) = parents.get(&child) else {
                            return false;
                        };
                        for &user in users {
                            match &ast[user].kind {
                                CExprKind::Paren(_, _) => todo.push(user),
                                CExprKind::Member(_, base, scalar, MemberKind::Dot, _)
                                    if *base == child
                                        && matches!(
                                            ast[*scalar].kind.get_name().map(String::as_str),
                                            Some("tv_sec" | "tv_nsec")
                                        ) => {}
                                _ => return false,
                            }
                        }
                    }
                }
            }
            CExprKind::InitList(typ, values, _, _) if matches!(ast.resolve_type(typ.ctype).kind, CTypeKind::Struct(id) if id == record) =>
            {
                let CDeclKind::Struct {
                    fields: Some(fields),
                    ..
                } = &ast[record].kind
                else {
                    return false;
                };
                for (&field, &value) in fields.iter().zip(values) {
                    if ast[field].kind.get_name().unwrap().starts_with("__")
                        && !matches!(ast[value].kind, CExprKind::ImplicitValueInit(_))
                    {
                        return false;
                    }
                }
            }
            _ => {}
        }
    }
    true
}

pub(crate) fn timestamp_prefix(name: &str) -> Option<&'static str> {
    match name {
        "st_atim" => Some("st_atime"),
        "st_mtim" => Some("st_mtime"),
        "st_ctim" => Some("st_ctime"),
        _ => None,
    }
}

fn compatible_function(
    ast: &TypedAstContext,
    typ: CTypeId,
    name: &str,
    records: &HashSet<CDeclId>,
) -> bool {
    use CTypeKind::*;
    let Function(ret, args, false, false, true) = &ast.resolve_type(typ).kind else {
        return false;
    };
    let signature: &[&str] = match name {
        "stat" | "lstat" => &["path", "stat"],
        "fstat" => &["int", "stat"],
        "fstatat" => &["int", "path", "stat", "int"],
        "chmod" | "mkdir" | "mkfifo" => &["path", "mode"],
        "fchmod" => &["int", "mode"],
        "fchmodat" => &["int", "path", "mode", "int"],
        "mkdirat" | "mkfifoat" => &["int", "path", "mode"],
        "mknod" => &["path", "mode", "dev"],
        "mknodat" => &["int", "path", "mode", "dev"],
        "umask" => &["mode"],
        _ => return false,
    };
    matches!(ast.resolve_type(ret.ctype).kind, UInt) == (name == "umask")
        && (name == "umask" || matches!(ast.resolve_type(ret.ctype).kind, Int))
        && args.len() == signature.len()
        && args.iter().zip(signature).all(|(arg, expected)| {
            let kind = &ast.resolve_type(arg.ctype).kind;
            match *expected {
                "int" => matches!(kind, Int), "mode" => matches!(kind, UInt), "dev" => matches!(kind, ULong),
                "path" => matches!(kind, Pointer(pointee) if pointee.qualifiers.is_const && matches!(ast.resolve_type(pointee.ctype).kind, Char)),
                "stat" => matches!(kind, Pointer(pointee) if !pointee.qualifiers.is_const && matches!(ast.resolve_type(pointee.ctype).kind, Struct(id) if records.contains(&id))),
                _ => false,
            }
        })
}

fn integer_value(ast: &TypedAstContext, expr: CExprId) -> Option<u64> {
    match &ast.index_unwrap_parens(expr).kind {
        CExprKind::Literal(_, CLiteral::Integer(value, _)) => Some(*value),
        CExprKind::ImplicitCast(_, inner, _, _, _)
        | CExprKind::ExplicitCast(_, inner, _, _, _)
        | CExprKind::ConstantExpr(_, inner, _) => integer_value(ast, *inner),
        CExprKind::Binary(_, op, lhs, rhs, _, _) => {
            let lhs = integer_value(ast, *lhs)?;
            let rhs = integer_value(ast, *rhs)?;
            match op {
                CBinOp::BitOr => Some(lhs | rhs),
                CBinOp::BitAnd => Some(lhs & rhs),
                CBinOp::BitXor => Some(lhs ^ rhs),
                CBinOp::ShiftLeft => lhs.checked_shl(rhs.try_into().ok()?),
                CBinOp::ShiftRight => lhs.checked_shr(rhs.try_into().ok()?),
                _ => None,
            }
        }
        _ => None,
    }
}

fn stat_constant(name: &str) -> Option<u64> {
    Some(match name {
        "S_IFMT" => 0o170000,
        "S_IFSOCK" => 0o140000,
        "S_IFLNK" => 0o120000,
        "S_IFREG" => 0o100000,
        "S_IFBLK" => 0o060000,
        "S_IFDIR" => 0o040000,
        "S_IFCHR" => 0o020000,
        "S_IFIFO" => 0o010000,
        "S_ISUID" => 0o4000,
        "S_ISGID" => 0o2000,
        "S_ISVTX" => 0o1000,
        "S_IRWXU" => 0o700,
        "S_IRUSR" | "S_IREAD" => 0o400,
        "S_IWUSR" | "S_IWRITE" => 0o200,
        "S_IXUSR" | "S_IEXEC" => 0o100,
        "S_IRWXG" => 0o070,
        "S_IRGRP" => 0o040,
        "S_IWGRP" => 0o020,
        "S_IXGRP" => 0o010,
        "S_IRWXO" => 0o007,
        "S_IROTH" => 0o004,
        "S_IWOTH" => 0o002,
        "S_IXOTH" => 0o001,
        _ => return None,
    })
}
