//! Recover bounded string arguments before choosing a safe C formatting macro.

use quote::ToTokens;
use syn::{Expr, Type, parse::Parser, parse_quote};
use xj_cfmt_core::{Piece, Plan, Requirement};

use crate::{
    Depth, Rewriter, SymbolTable,
    byte_input::{is_u8_type, prefer_string_literal},
};

impl Rewriter {
    /// Normalize literal ByteView inputs in macros without changing raw pointers,
    /// mutable destinations, or Rust formatting arguments with ByteDisplay semantics.
    pub fn rewrite_byte_view_macro_literals(
        &self,
        _symbols: &SymbolTable,
        mac: &syn::Macro,
    ) -> Option<syn::Macro> {
        let path = &mac.path;
        if path.segments.len() != 2 {
            return None;
        }
        let namespace = path.segments[0].ident.to_string();
        let name = path.segments[1].ident.to_string();
        let operation = name
            .strip_suffix("_safe")
            .or_else(|| name.strip_suffix("_unchecked"))
            .unwrap_or(&name);
        let (format_index, input_index, printf) = match (namespace.as_str(), operation) {
            ("xj_cio", "printf") => (0, None, true),
            ("xj_cio", "fprintf") => (1, None, true),
            ("xj_cio" | "xj_cfmt", "sprintf") => (1, None, true),
            ("xj_cio" | "xj_cfmt", "snprintf") => (2, None, true),
            ("xj_scanf", "sscanf" | "bscanf") => (1, Some(0), false),
            ("xj_scanf", "brscanf") => (1, None, false),
            _ => return None,
        };
        let mut args = syn::punctuated::Punctuated::<Expr, syn::Token![,]>::parse_terminated
            .parse2(mac.tokens.clone())
            .ok()?;
        let before = args.to_token_stream().to_string();
        if let Some(input_index) = input_index
            && let Some(input) = prefer_string_literal(args.get(input_index)?)
        {
            args[input_index] = input;
        }
        if let Some(format) = prefer_string_literal(args.get(format_index)?) {
            args[format_index] = format;
        }
        if printf
            && let Some(bytes) = literal_bytes(&args[format_index])
            && let Ok(plan) = Plan::new(&bytes)
        {
            for (arg, requirement) in args
                .iter_mut()
                .skip(format_index + 1)
                .zip(plan.requirements)
            {
                if requirement == Requirement::String
                    && let Some(text) = prefer_string_literal(arg)
                {
                    *arg = text;
                }
            }
        }
        let tokens = args.to_token_stream();
        if tokens.to_string() == before {
            return None;
        }
        let mut result = mac.clone();
        result.tokens = tokens;
        Some(result)
    }

    /// Rewrite an ignored, literal ASCII stderr line to `xj_cio::eprintln!`.
    pub fn rewrite_stmt_fprintf_stderr(
        &self,
        _symbols: &SymbolTable,
        stmt: &syn::Stmt,
    ) -> Option<(syn::Stmt, Depth)> {
        // This rewrite is not conservative: it assumes the program's use of
        // errno (if any) cannot be affected by these calls, and that differences
        // in observable behavior in failure conditions is acceptable.
        let (attrs, mac) = match stmt {
            syn::Stmt::Macro(m) if m.semi_token.is_some() => (&m.attrs, &m.mac),
            syn::Stmt::Expr(Expr::Macro(m), Some(_)) => (&m.attrs, &m.mac),
            _ => return None,
        };
        if mac.path.segments.len() != 2
            || mac.path.segments[0].ident != "xj_cio"
            || mac.path.segments[1].ident != "fprintf_safe"
        {
            return None;
        }
        let args = syn::punctuated::Punctuated::<Expr, syn::Token![,]>::parse_terminated
            .parse2(mac.tokens.clone())
            .ok()?;
        // Additional arguments must not lose their evaluation or formatting.
        if args.len() != 2 {
            return None;
        }
        let Expr::Call(stream) = unparen(&args[0]) else {
            return None;
        };
        let Expr::Path(function) = unparen(&stream.func) else {
            return None;
        };
        if !stream.args.is_empty()
            || function.qself.is_some()
            || function.path.segments.len() != 2
            || function.path.segments[0].ident != "xj_cio"
            || function.path.segments[1].ident != "stderr"
        {
            return None;
        }
        let bytes = literal_bytes(&args[1])?;
        let bytes = xj_bytes::c_bytes(&bytes);
        if !bytes.is_ascii() {
            return None;
        }
        let plan = Plan::new(bytes).ok()?;
        let mut text = Vec::new();
        for piece in plan.pieces {
            match piece {
                Piece::Literal(bytes) => text.extend_from_slice(bytes),
                Piece::Conversion(c) if c.conversion == b'%' => text.push(b'%'),
                _ => return None,
            }
        }
        // eprintln supplies exactly one newline; preserve all other bytes.
        if text.pop()? != b'\n' {
            return None;
        }
        let text = String::from_utf8(text).ok()?;
        let format = text.replace('{', "{{").replace('}', "}}");
        let format = syn::LitStr::new(&format, mac.path.segments[1].ident.span());
        self.add_dep("xj_cio");
        Some((
            parse_quote! { #(#attrs)* ::xj_cio::eprintln!(#format); },
            Depth::Limited(0),
        ))
    }

    pub fn rewrite_c_printf_safe(
        &self,
        symbols: &SymbolTable,
        mac: &syn::Macro,
    ) -> Option<syn::Macro> {
        let path = &mac.path;
        if path.segments.len() != 2 {
            return None;
        }
        let namespace = path.segments[0].ident.to_string();
        if !matches!(namespace.as_str(), "xj_cio" | "xj_cfmt") {
            return None;
        }
        let name = path.segments[1].ident.to_string();
        let operation = name
            .strip_suffix("_unchecked")
            .or_else(|| name.strip_suffix("_safe"))
            .unwrap_or(&name);
        let format_index = match operation {
            "printf" if namespace == "xj_cio" => 0,
            "fprintf" if namespace == "xj_cio" => 1,
            "sprintf" => 1,
            "snprintf" => 2,
            _ => return None,
        };
        let parser = syn::punctuated::Punctuated::<Expr, syn::Token![,]>::parse_terminated;
        let mut args = parser.parse2(mac.tokens.clone()).ok()?;
        let bytes = literal_bytes(args.get(format_index)?)?;
        let plan = Plan::new(&bytes).ok()?;
        // Pointer identity and count stores cannot be expressed by SafeAdapt.
        if plan
            .requirements
            .iter()
            .any(|r| matches!(r, Requirement::Pointer | Requirement::Count(_)))
            || args.len() < format_index + 1 + plan.requirements.len()
        {
            return None;
        }
        if matches!(operation, "sprintf" | "snprintf")
            && !name.ends_with("_safe")
            && !name.ends_with("_unchecked")
        {
            // Raw destinations are handled by the existing destination rewrite.
            return None;
        }
        if let Some(format) = prefer_string_literal(&args[format_index]) {
            args[format_index] = format;
        }
        for (i, arg) in args.iter_mut().skip(format_index + 1).enumerate() {
            if matches!(plan.requirements.get(i), Some(Requirement::String)) {
                *arg = string_arg(arg, symbols, false)?;
            } else if plan.requirements.get(i).is_some() {
                if !scalar(arg, symbols) {
                    return None;
                }
            } else {
                // Extra arguments are still evaluated, and must also implement SafeAdapt.
                if !scalar(arg, symbols) {
                    *arg = string_arg(arg, symbols, false)?;
                }
            }
        }
        let mut result = mac.clone();
        result.path.segments.last_mut()?.ident =
            syn::Ident::new(&format!("{operation}_safe"), path.segments[1].ident.span());
        result.tokens = args.to_token_stream();
        if result.path == mac.path && result.tokens.to_string() == mac.tokens.to_string() {
            None
        } else {
            Some(result)
        }
    }
}

fn unparen(mut expr: &Expr) -> &Expr {
    loop {
        expr = match expr {
            Expr::Paren(p) => &p.expr,
            Expr::Group(g) => &g.expr,
            _ => return expr,
        };
    }
}

fn literal_bytes(expr: &Expr) -> Option<Vec<u8>> {
    match unparen(expr) {
        Expr::Lit(lit) => match &lit.lit {
            syn::Lit::ByteStr(s) => Some(s.value()),
            syn::Lit::Str(s) => Some(s.value().into_bytes()),
            syn::Lit::CStr(s) => Some(s.value().into_bytes()),
            _ => None,
        },
        _ => None,
    }
}

#[derive(Clone, Copy)]
enum Storage {
    Bytes,
    Text,
    CStr,
    CString,
    ZStr,
}

fn storage(ty: &Type) -> Option<Storage> {
    match ty {
        Type::Reference(r) => storage(&r.elem),
        Type::Paren(p) => storage(&p.elem),
        Type::Array(a) if is_u8_type(&a.elem) => Some(Storage::Bytes),
        Type::Slice(s) if is_u8_type(&s.elem) => Some(Storage::Bytes),
        Type::Path(p) if p.qself.is_none() => {
            let last = p.path.segments.last()?;
            let prefix: Vec<_> = p
                .path
                .segments
                .iter()
                .map(|s| s.ident.to_string())
                .collect();
            let allowed = prefix.len() == 1
                || matches!(prefix[0].as_str(), "std" | "core" | "alloc" | "xj_zstr");
            if !allowed {
                return None;
            }
            match last.ident.to_string().as_str() {
                "str" | "String" => Some(Storage::Text),
                "CStr" => Some(Storage::CStr),
                "CString" => Some(Storage::CString),
                "ZStr" | "ZString" => Some(Storage::ZStr),
                "Vec"
                    if matches!(&last.arguments, syn::PathArguments::AngleBracketed(a)
                    if matches!(a.args.first(), Some(syn::GenericArgument::Type(t)) if is_u8_type(t))) =>
                {
                    Some(Storage::Bytes)
                }
                _ => None,
            }
        }
        _ => None,
    }
}

pub(crate) fn expr_type(expr: &Expr, symbols: &SymbolTable) -> Option<Type> {
    let expr = unparen(expr);
    match expr {
        Expr::Lit(lit) => Some(match &lit.lit {
            syn::Lit::ByteStr(s) => {
                let n = s.value().len();
                parse_quote!(&[u8; #n])
            }
            syn::Lit::Str(_) => parse_quote!(&str),
            syn::Lit::CStr(_) => parse_quote!(&::core::ffi::CStr),
            syn::Lit::Int(i) => {
                if i.suffix().is_empty() {
                    parse_quote!(i32)
                } else {
                    syn::parse_str(i.suffix()).ok()?
                }
            }
            syn::Lit::Float(f) => {
                if f.suffix().is_empty() {
                    parse_quote!(f64)
                } else {
                    syn::parse_str(f.suffix()).ok()?
                }
            }
            syn::Lit::Byte(_) => parse_quote!(u8),
            syn::Lit::Char(_) => parse_quote!(char),
            syn::Lit::Bool(_) => parse_quote!(bool),
            _ => return None,
        }),
        Expr::Cast(c) => Some((*c.ty).clone()),
        Expr::Reference(r) => {
            let inner = expr_type(&r.expr, symbols)?;
            Some(if r.mutability.is_some() {
                parse_quote!(&mut #inner)
            } else {
                parse_quote!(&#inner)
            })
        }
        Expr::Call(c) => {
            let Expr::Path(p) = unparen(&c.func) else {
                return None;
            };
            if p.qself.is_none()
                && p.path.segments.len() == 1
                && !symbols.has(&p.path.segments[0].ident.to_string())
            {
                symbols
                    .fn_outputs
                    .get(&p.path.segments[0].ident.to_string())
                    .cloned()
            } else {
                None
            }
        }
        Expr::MethodCall(m) if m.args.is_empty() => {
            let kind = storage(&expr_type(&m.receiver, symbols)?)?;
            match (m.method.to_string().as_str(), kind) {
                ("view_bytes", _) => Some(parse_quote!(&[u8])),
                ("as_bytes", Storage::Text | Storage::CString | Storage::ZStr)
                | ("as_bytes_with_nul", Storage::ZStr)
                | ("as_buffer", Storage::ZStr)
                | ("to_bytes" | "to_bytes_with_nul", Storage::CStr)
                | ("as_slice", Storage::Bytes) => Some(parse_quote!(&[u8])),
                ("as_bytes_mut" | "as_buffer_mut", Storage::ZStr) => Some(parse_quote!(&mut [u8])),
                ("as_z_str", Storage::ZStr) => Some(parse_quote!(&::xj_zstr::ZStr)),
                ("as_c_str", Storage::CString | Storage::ZStr) => {
                    Some(parse_quote!(&::core::ffi::CStr))
                }
                _ => None,
            }
        }
        Expr::Index(i) if matches!(unparen(&i.index), Expr::Range(_)) => {
            if matches!(storage(&expr_type(&i.expr, symbols)?), Some(Storage::Bytes)) {
                Some(parse_quote!([u8]))
            } else {
                None
            }
        }
        Expr::If(i) if !condition_binds(&i.cond) => {
            let a = block_type(&i.then_branch, symbols)?;
            let b = expr_type(&i.else_branch.as_ref()?.1, symbols)?;
            (a == b).then_some(a)
        }
        Expr::Block(b) if b.label.is_none() => block_type(&b.block, symbols),
        _ => symbols.type_of_expr(expr),
    }
}

fn scalar_type(ty: &Type) -> bool {
    match ty {
        Type::Reference(r) => scalar_type(&r.elem),
        Type::Path(p) if p.qself.is_none() => {
            let Some(last) = p.path.segments.last() else {
                return false;
            };
            let name = last.ident.to_string();
            if p.path.segments.len() == 1
                && matches!(
                    name.as_str(),
                    "i8" | "i16"
                        | "i32"
                        | "i64"
                        | "isize"
                        | "u8"
                        | "u16"
                        | "u32"
                        | "u64"
                        | "usize"
                        | "f32"
                        | "f64"
                        | "char"
                        | "bool"
                )
            {
                return true;
            }
            matches!(
                p.path.segments[0].ident.to_string().as_str(),
                "core" | "std" | "libc" | "xj_cio"
            ) && matches!(
                name.as_str(),
                "c_char"
                    | "c_schar"
                    | "c_uchar"
                    | "c_short"
                    | "c_ushort"
                    | "c_int"
                    | "c_uint"
                    | "c_long"
                    | "c_ulong"
                    | "c_longlong"
                    | "c_ulonglong"
                    | "c_float"
                    | "c_double"
                    | "size_t"
                    | "ssize_t"
                    | "ptrdiff_t"
            )
        }
        _ => false,
    }
}

fn scalar(expr: &Expr, symbols: &SymbolTable) -> bool {
    if expr_type(expr, symbols).is_some_and(|t| scalar_type(&t)) {
        return true;
    }
    match unparen(expr) {
        Expr::Unary(u) if !matches!(u.op, syn::UnOp::Deref(_)) => scalar(&u.expr, symbols),
        Expr::Binary(b) => scalar(&b.left, symbols) && scalar(&b.right, symbols),
        Expr::If(i) if !condition_binds(&i.cond) => {
            block_scalar(&i.then_branch, symbols)
                && i.else_branch
                    .as_ref()
                    .is_some_and(|(_, e)| scalar(e, symbols))
        }
        Expr::Block(b) if b.label.is_none() => block_scalar(&b.block, symbols),
        _ => false,
    }
}

fn condition_binds(expr: &Expr) -> bool {
    match unparen(expr) {
        Expr::Let(_) => true,
        Expr::Binary(b) => condition_binds(&b.left) || condition_binds(&b.right),
        _ => false,
    }
}

fn shadow_functions(pat: &syn::Pat, scope: &mut SymbolTable) {
    let mut names = SymbolTable::new();
    names.items = scope.fn_outputs.clone();
    crate::invalidate_pat_bindings(pat, &mut names);
    scope.fn_outputs.retain(|name, _| names.has(name));
}

// This type-only path can retain a conditional producing owned safe storage,
// without borrowing a temporary or a branch-local value out of its scope.
fn block_type(block: &syn::Block, symbols: &SymbolTable) -> Option<Type> {
    let (last, prefix) = block.stmts.split_last()?;
    let syn::Stmt::Expr(tail, None) = last else {
        return None;
    };
    let mut scope = symbols.clone();
    for stmt in prefix {
        match stmt {
            syn::Stmt::Local(local) => {
                shadow_functions(&local.pat, &mut scope);
                crate::collect_pat_bindings(&local.pat, &mut scope);
            }
            syn::Stmt::Item(_) => return None,
            _ => (),
        }
    }
    expr_type(tail, &scope)
}

fn block_scope<'a>(
    block: &'a syn::Block,
    symbols: &SymbolTable,
) -> Option<(SymbolTable, &'a Expr)> {
    let (last, prefix) = block.stmts.split_last()?;
    let syn::Stmt::Expr(tail, None) = last else {
        return None;
    };
    let mut scope = symbols.clone();
    for stmt in prefix {
        match stmt {
            syn::Stmt::Local(local) => {
                shadow_functions(&local.pat, &mut scope);
                crate::invalidate_pat_bindings(&local.pat, &mut scope);
            }
            // Local items may shadow functions or outer bindings, too.
            syn::Stmt::Item(_) => return None,
            _ => (),
        }
    }
    Some((scope, tail))
}

fn block_scalar(block: &syn::Block, symbols: &SymbolTable) -> bool {
    block_scope(block, symbols).is_some_and(|(scope, tail)| scalar(tail, &scope))
}

// Only borrow storage that outlives a conditional arm. Owned call results and
// block locals cannot supply a borrowed result from that arm.
fn live_storage(expr: &Expr, symbols: &SymbolTable) -> bool {
    live_storage_inner(expr, symbols, StorageLifetime::ConditionalArm)
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum StorageLifetime {
    ConditionalArm,
    EndPointer,
}

/// Recognize existing storage for a C end pointer without introducing an owner
/// that dies at the end of the call statement. This is a local check, not proof
/// that the original program keeps that storage alive for every pointer use.
/// In particular, even reference-returning calls may borrow temporary arguments,
/// so calls, blocks, conditionals, and unknown methods are conservatively rejected.
pub(crate) fn live_storage_for_endptr(expr: &Expr, symbols: &SymbolTable) -> bool {
    live_storage_inner(expr, symbols, StorageLifetime::EndPointer)
}

fn live_storage_inner(expr: &Expr, symbols: &SymbolTable, lifetime: StorageLifetime) -> bool {
    let live = |expr: &Expr| live_storage_inner(expr, symbols, lifetime);
    match unparen(expr) {
        Expr::Lit(lit) => {
            lifetime == StorageLifetime::ConditionalArm
                || matches!(
                    lit.lit,
                    syn::Lit::Str(_) | syn::Lit::ByteStr(_) | syn::Lit::CStr(_)
                )
        }
        Expr::Path(path) => {
            let Some(ty) = expr_type(expr, symbols) else {
                return false;
            };
            // A constant owner is materialized at each use. Reference constants
            // already refer to persistent storage; alias types are unknown here.
            lifetime == StorageLifetime::ConditionalArm
                || matches!(ty, Type::Reference(_))
                || path
                    .path
                    .get_ident()
                    .is_some_and(|name| !symbols.constants.contains(&name.to_string()))
        }
        Expr::Field(f) => live(&f.base),
        Expr::Index(i) => live(&i.expr),
        Expr::Reference(r) => live(&r.expr),
        Expr::Unary(u) if matches!(u.op, syn::UnOp::Deref(_)) => {
            matches!(expr_type(&u.expr, symbols), Some(Type::Reference(_))) && live(&u.expr)
        }
        Expr::Call(_) => {
            lifetime == StorageLifetime::ConditionalArm
                && matches!(expr_type(expr, symbols), Some(Type::Reference(_)))
        }
        Expr::Cast(c)
            if lifetime == StorageLifetime::EndPointer && matches!(&*c.ty, Type::Ptr(_)) =>
        {
            live(&c.expr)
        }
        Expr::RawAddr(raw) if lifetime == StorageLifetime::EndPointer => live(&raw.expr),
        Expr::MethodCall(m)
            if lifetime == StorageLifetime::EndPointer
                && m.args.is_empty()
                && ((m.turbofish.is_none()
                    && matches!(m.method.to_string().as_str(), "as_ptr" | "as_mut_ptr"))
                    || m.method == "cast") =>
        {
            // These are the pointer-decay layers stripped by ByteView recovery.
            live(&m.receiver)
        }
        Expr::MethodCall(m) => {
            let ty = expr_type(expr, symbols);
            (if lifetime == StorageLifetime::EndPointer {
                m.turbofish.is_none() && matches!(ty, Some(Type::Reference(_)))
            } else {
                ty.is_some()
            }) && live(&m.receiver)
        }
        _ => false,
    }
}

fn byte_view(expr: &Expr, symbols: &SymbolTable) -> Option<Expr> {
    if !live_storage(expr, symbols) {
        return None;
    }
    Some(match storage(&expr_type(expr, symbols)?)? {
        Storage::Bytes => parse_quote!(&(#expr)[..]),
        Storage::Text | Storage::CString => parse_quote!((#expr).as_bytes()),
        Storage::CStr => parse_quote!((#expr).to_bytes()),
        Storage::ZStr => parse_quote!((#expr).as_bytes_with_nul()),
    })
}

fn pointer_base<'a>(expr: &'a Expr, symbols: &SymbolTable) -> Option<&'a Expr> {
    match unparen(expr) {
        Expr::Cast(c) if matches!(&*c.ty, Type::Ptr(_) | Type::Infer(_)) => {
            pointer_base(&c.expr, symbols)
        }
        Expr::MethodCall(m) if m.args.is_empty() && m.method == "cast" => {
            pointer_base(&m.receiver, symbols)
        }
        Expr::MethodCall(m)
            if m.args.is_empty()
                && m.turbofish.is_none()
                && matches!(m.method.to_string().as_str(), "as_ptr" | "as_mut_ptr") =>
        {
            Some(&m.receiver)
        }
        // An array address decays to its first element through the pointer cast
        // emitted by c2rust. Addresses of references or container objects do not.
        Expr::RawAddr(r) if matches!(expr_type(&r.expr, symbols), Some(Type::Array(_))) => {
            Some(&r.expr)
        }
        _ => None,
    }
}

fn string_block(block: &syn::Block, symbols: &SymbolTable) -> Option<syn::Block> {
    let (scope, tail) = block_scope(block, symbols)?;
    let replacement = string_arg(tail, &scope, true)?;
    let mut result = block.clone();
    *result.stmts.last_mut()? = syn::Stmt::Expr(replacement, None);
    Some(result)
}

fn string_arg(expr: &Expr, symbols: &SymbolTable, as_view: bool) -> Option<Expr> {
    match unparen(expr) {
        Expr::Cast(c) if matches!(&*c.ty, Type::Ptr(_) | Type::Infer(_)) => {
            return string_arg(&c.expr, symbols, as_view);
        }
        Expr::MethodCall(m) if m.args.is_empty() && m.method == "cast" => {
            return string_arg(&m.receiver, symbols, as_view);
        }
        Expr::If(i) => {
            if condition_binds(&i.cond) {
                return None;
            }
            if !as_view && expr_type(expr, symbols).is_some_and(|ty| storage(&ty).is_some()) {
                return Some(expr.clone());
            }
            let mut result = i.clone();
            result.then_branch = string_block(&i.then_branch, symbols)?;
            let (_, other) = result.else_branch.as_mut()?;
            **other = string_arg(other, symbols, true)?;
            return Some(Expr::If(result));
        }
        Expr::Block(b) if b.label.is_none() => {
            if !as_view && expr_type(expr, symbols).is_some_and(|ty| storage(&ty).is_some()) {
                return Some(expr.clone());
            }
            let mut result = b.clone();
            result.block = string_block(&b.block, symbols)?;
            return Some(Expr::Block(result));
        }
        _ => (),
    }
    if let Some(base) = pointer_base(expr, symbols) {
        let ty = expr_type(base, symbols)?;
        storage(&ty)?;
        if as_view {
            return byte_view(base, symbols);
        }
        if matches!(unparen(base), Expr::Lit(_)) {
            return Some(prefer_string_literal(base).unwrap_or_else(|| base.clone()));
        }
        // Preserve pointer receivers by borrowing rather than moving their storage.
        return Some(match storage(&ty)? {
            Storage::Bytes => parse_quote!(&(#base)[..]),
            Storage::Text | Storage::CString => parse_quote!((#base).as_bytes()),
            Storage::CStr => parse_quote!((#base).to_bytes()),
            Storage::ZStr => parse_quote!((#base).as_bytes_with_nul()),
        });
    }
    storage(&expr_type(expr, symbols)?)?;
    if as_view {
        byte_view(expr, symbols)
    } else {
        Some(prefer_string_literal(expr).unwrap_or_else(|| expr.clone()))
    }
}
