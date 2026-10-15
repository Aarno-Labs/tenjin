//! Rewrite scanf calls toward safe `ScanTarget` destinations.

use std::collections::HashSet;

use proc_macro2::{TokenStream, TokenTree};
use quote::ToTokens;
use syn::punctuated::Punctuated;
use syn::token::Comma;
use syn::{Expr, ExprCall, Type, parse_quote};

use crate::{Depth, Rewriter, SymbolTable};

/// How one raw `OutArg` destination can be expressed safely.
struct SafeDest {
    /// A `&mut dyn ScanTarget` expression.
    target: Expr,
    /// A checked `OutArg::Buf` replacement, for byte arrays.
    bounded: Option<Expr>,
    /// The local whose storage the destination borrows.
    root: String,
}

impl Rewriter {
    /// Rewrite scanf calls toward safe destinations:
    ///
    /// - c2rust's `xj_cio::fscanf_raw`, `scanf_raw` and `sscanf_raw` become
    ///   `xj_cio::fscanf`, `xj_cio::scanf` and `xj_scanf::sscanf` when every
    ///   destination can be borrowed. Otherwise byte arrays still become
    ///   bounded `OutArg::Buf` destinations in the raw call.
    /// - An unqualified `sscanf(...)` becomes `xj_scanf::bscanf!(...)`.
    ///
    /// The `va_list` forms are left alone, since their destinations are not
    /// visible, as are unqualified `scanf` and `fscanf`, which would read a
    /// different stream buffer than the rest of the program.
    pub fn rewrite_scanf_variants(
        &self,
        symbols: &SymbolTable,
        expr: &Expr,
    ) -> Option<(Expr, Depth)> {
        let Expr::Call(call) = expr else {
            return None;
        };
        let Expr::Path(func) = &*call.func else {
            return None;
        };
        let segments: Vec<String> = func
            .path
            .segments
            .iter()
            .map(|s| s.ident.to_string())
            .collect();
        match segments.iter().map(String::as_str).collect::<Vec<_>>()[..] {
            ["xj_cio", name @ ("fscanf_raw" | "scanf_raw" | "sscanf_raw")] => {
                self.rewrite_cio_raw_scanf(symbols, call, name)
            }
            ["sscanf"] if !symbols.defined_fns.contains("sscanf") && !symbols.has("sscanf") => {
                self.rewrite_libc_sscanf(symbols, call)
            }
            _ => None,
        }
    }

    fn rewrite_cio_raw_scanf(
        &self,
        symbols: &SymbolTable,
        call: &ExprCall,
        name: &str,
    ) -> Option<(Expr, Depth)> {
        let lead = if name == "scanf_raw" { 1 } else { 2 };
        if call.args.len() != lead + 1 {
            return None;
        }
        let Expr::Reference(reference) = strip_parens(&call.args[lead]) else {
            return None;
        };
        reference.mutability?;
        let Expr::Array(array) = strip_parens(&reference.expr) else {
            return None;
        };
        let elems: Vec<&Expr> = array.elems.iter().collect();
        let dests: Vec<Option<SafeDest>> = elems
            .iter()
            .map(|elem| {
                let (variant, inner) = out_arg_parts(elem)?;
                self.safe_scan_dest(symbols, &variant, inner)
            })
            .collect();
        // Borrowing a destination must not conflict with anything else the
        // call mentions: another destination, the stream, input or format.
        let idents: Vec<HashSet<String>> = elems.iter().map(|e| expr_idents(e)).collect();
        let lead_idents: HashSet<String> =
            call.args.iter().take(lead).flat_map(expr_idents).collect();
        let clean = |i: usize, dest: &SafeDest| {
            !lead_idents.contains(&dest.root)
                && idents
                    .iter()
                    .enumerate()
                    .all(|(j, set)| j == i || !set.contains(&dest.root))
        };
        let usable: Vec<Option<&SafeDest>> = dests
            .iter()
            .enumerate()
            .map(|(i, d)| d.as_ref().filter(|d| clean(i, d)))
            .collect();
        let prefix: Vec<Expr> = call.args.iter().take(lead).cloned().collect();
        if usable.iter().all(Option::is_some) {
            let mut targets: Vec<Expr> = usable.iter().map(|d| d.unwrap().target.clone()).collect();
            for target in &mut targets {
                self.rewrite_expr_fully(symbols, target);
            }
            let targets: Punctuated<Expr, Comma> = targets.into_iter().collect();
            let call: Expr = match name {
                "fscanf_raw" => {
                    let [stream, fmt] = &prefix[..] else {
                        return None;
                    };
                    parse_quote! { ::xj_cio::fscanf(#stream, #fmt, &mut [#targets]) }
                }
                "scanf_raw" => {
                    let [fmt] = &prefix[..] else { return None };
                    parse_quote! { ::xj_cio::scanf(#fmt, &mut [#targets]) }
                }
                _ => {
                    let [input, fmt] = &prefix[..] else {
                        return None;
                    };
                    self.add_dep("xj_scanf");
                    parse_quote! { ::xj_scanf::sscanf(#input, #fmt, &mut [#targets]) }
                }
            };
            return Some((call, Depth::Limited(0)));
        }
        if !usable
            .iter()
            .any(|d| d.is_some_and(|d| d.bounded.is_some()))
        {
            return None;
        }
        let elems: Punctuated<Expr, Comma> = elems
            .iter()
            .zip(&usable)
            .map(|(elem, dest)| match dest.and_then(|d| d.bounded.clone()) {
                Some(mut bounded) => {
                    self.rewrite_expr_fully(symbols, &mut bounded);
                    bounded
                }
                None => (*elem).clone(),
            })
            .collect();
        let func = &call.func;
        let prefix: Punctuated<Expr, Comma> = prefix.into_iter().collect();
        Some((
            parse_quote! { #func(#prefix, &mut [#elems]) },
            Depth::Limited(0),
        ))
    }

    /// A safe destination for `OutArg::<variant>(inner)`, if the pointer is
    /// the address of a local (or a field or element of one) whose type
    /// implements `ScanTarget` with the extent C would write.
    fn safe_scan_dest(
        &self,
        symbols: &SymbolTable,
        variant: &str,
        inner: &Expr,
    ) -> Option<SafeDest> {
        let (place, array_ptr) = match strip_casts(inner) {
            Expr::RawAddr(raw) if matches!(raw.mutability, syn::PointerMutability::Mut(_)) => {
                (&*raw.expr, false)
            }
            Expr::Reference(r) if r.mutability.is_some() => (&*r.expr, false),
            // `buf.as_mut_ptr()` decays an array to its first byte.
            Expr::MethodCall(m) if m.method == "as_mut_ptr" && m.args.is_empty() => {
                (&*m.receiver, true)
            }
            _ => return None,
        };
        let place = strip_parens(place);
        let root = place_root(place)?;
        if !symbols.has(&root)
            || symbols.mutable_statics.contains(&root)
            || symbols.constants.contains(&root)
        {
            return None;
        }
        let ty = resolve_alias(&symbols.type_of_expr(place)?, symbols)?;
        let byte_storage = |elem: &Type| -> Option<(Expr, Expr)> {
            match scalar_name(&resolve_alias(elem, symbols)?)? {
                "u8" | "c_uchar" => Some((
                    parse_quote! { &mut #place },
                    parse_quote! { ::xj_cio::OutArg::Buf(::xj_cio::CStrDest::from(&mut #place)) },
                )),
                "i8" | "c_char" | "c_schar" => {
                    self.add_dep("xj_cstr");
                    self.with_cur_file_item_store(|store| {
                        store.add_use(true, vec!["xj_cstr".into()], "ByteSlice");
                    });
                    Some((
                        parse_quote! { &mut #place.as_mut_u8_slice() },
                        parse_quote! { ::xj_cio::OutArg::Buf(::xj_cio::CStrDest::from(#place.as_mut_u8_slice())) },
                    ))
                }
                _ => None,
            }
        };
        let (target, bounded) = match (variant, &ty) {
            // The whole array bounds C's write.
            ("Str", Type::Array(array)) => {
                let (target, bounded) = byte_storage(&array.elem)?;
                (target, Some(bounded))
            }
            _ if array_ptr => return None,
            // A single `char`, for `%c` or an `hh` directive. An array
            // element is excluded: C would write a string from there on.
            ("Str", _) => {
                if matches!(place, Expr::Index(_))
                    || !matches!(
                        scalar_name(&ty)?,
                        "u8" | "i8" | "c_char" | "c_schar" | "c_uchar"
                    )
                {
                    return None;
                }
                (parse_quote! { &mut #place }, None)
            }
            ("Buf", _) => return None,
            _ => {
                scalar_name(&ty)?;
                (parse_quote! { &mut #place }, None)
            }
        };
        Some(SafeDest {
            target,
            bounded,
            root,
        })
    }

    /// `sscanf(input, fmt, args...)` from libc, as `xj_scanf::bscanf!`.
    fn rewrite_libc_sscanf(&self, symbols: &SymbolTable, call: &ExprCall) -> Option<(Expr, Depth)> {
        if call.args.len() < 2 {
            return None;
        }
        let fmt = &call.args[1];
        let fmt = self
            .coerce_byte_view_input(fmt, symbols)
            .unwrap_or_else(|| {
                Box::new(parse_quote! {
                    ::core::ffi::CStr::from_ptr((#fmt) as *const core::ffi::c_char)
                })
            });
        let mut targets = vec![];
        for arg in call.args.iter().skip(2) {
            let mut target = self.coerce_libc_scanf_arg(arg, symbols)?;
            self.rewrite_expr_fully(symbols, &mut target);
            targets.push(target);
        }
        let targets: Punctuated<Expr, Comma> = targets.into_iter().collect();
        let raw_input = &call.args[0];
        let mut input = *self
            .coerce_byte_view_input(raw_input, symbols)
            .unwrap_or_else(|| {
                Box::new(parse_quote! {
                    ::core::ffi::CStr::from_ptr((#raw_input) as *const core::ffi::c_char)
                })
            });
        self.rewrite_expr_fully(symbols, &mut input);
        self.add_dep("xj_scanf");
        Some((
            parse_quote! { xj_scanf::bscanf!(#input, #fmt, #targets) },
            Depth::Limited(0),
        ))
    }

    fn coerce_libc_scanf_arg(&self, expr: &Expr, symbols: &SymbolTable) -> Option<Expr> {
        let expr = strip_casts(expr);
        let as_mut_u8_slice = |e: &Expr| -> Expr {
            self.add_dep("xj_cstr");
            self.with_cur_file_item_store(|store| {
                store.add_use(true, vec!["xj_cstr".into()], "ByteSlice");
            });
            parse_quote! { &mut #e.as_mut_u8_slice() }
        };
        match expr {
            Expr::Reference(reference) => {
                let e = &reference.expr;
                Some(parse_quote! { &mut #e })
            }
            Expr::RawAddr(raw) => {
                // Raw borrows of arrays decay, as with `.as_mut_ptr()`.
                if matches!(symbols.type_of_expr(&raw.expr), Some(Type::Array(_))) {
                    Some(as_mut_u8_slice(&raw.expr))
                } else {
                    let e = &raw.expr;
                    Some(parse_quote! { &mut #e })
                }
            }
            Expr::MethodCall(m) if m.method == "as_mut_ptr" => Some(as_mut_u8_slice(&m.receiver)),
            // Probably a direct raw pointer; can't safely convert that.
            _ => {
                eprintln!("synsub: rewrite_scanf_variants: unsupported target argument {expr:?}");
                None
            }
        }
    }
}

/// `OutArg::Variant(inner)`, with any path prefix.
fn out_arg_parts(expr: &Expr) -> Option<(String, &Expr)> {
    let Expr::Call(call) = strip_parens(expr) else {
        return None;
    };
    let Expr::Path(path) = &*call.func else {
        return None;
    };
    let mut segments = path.path.segments.iter().rev();
    let variant = segments.next()?;
    if segments.next()?.ident != "OutArg" || call.args.len() != 1 {
        return None;
    }
    Some((variant.ident.to_string(), &call.args[0]))
}

fn strip_parens(mut expr: &Expr) -> &Expr {
    loop {
        match expr {
            Expr::Paren(p) => expr = &p.expr,
            Expr::Group(g) => expr = &g.expr,
            _ => return expr,
        }
    }
}

fn strip_casts(mut expr: &Expr) -> &Expr {
    loop {
        match expr {
            Expr::Cast(cast) => expr = &cast.expr,
            Expr::Paren(p) => expr = &p.expr,
            Expr::Group(g) => expr = &g.expr,
            _ => return expr,
        }
    }
}

/// The local variable at the base of a place built from fields and
/// indexing, without dereferences.
fn place_root(expr: &Expr) -> Option<String> {
    match strip_parens(expr) {
        Expr::Path(path) if path.qself.is_none() && path.path.segments.len() == 1 => {
            Some(path.path.segments[0].ident.to_string())
        }
        Expr::Field(field) => place_root(&field.base),
        Expr::Index(index) => place_root(&index.expr),
        _ => None,
    }
}

fn expr_idents(expr: &Expr) -> HashSet<String> {
    fn walk(tokens: TokenStream, out: &mut HashSet<String>) {
        for token in tokens {
            match token {
                TokenTree::Ident(ident) => {
                    out.insert(ident.to_string());
                }
                TokenTree::Group(group) => walk(group.stream(), out),
                _ => {}
            }
        }
    }
    let mut out = HashSet::new();
    walk(expr.to_token_stream(), &mut out);
    out
}

/// Follows local type aliases such as `size_t` and `int8_t`.
fn resolve_alias(ty: &Type, symbols: &SymbolTable) -> Option<Type> {
    let mut ty = ty.clone();
    for _ in 0..8 {
        let Type::Path(path) = &ty else {
            return Some(ty);
        };
        if path.qself.is_some() || path.path.segments.len() != 1 {
            return Some(ty);
        }
        match symbols
            .type_aliases
            .get(&path.path.segments[0].ident.to_string())
        {
            Some(Some(target)) => ty = target.clone(),
            // A conflicting alias has no single meaning.
            Some(None) => return None,
            None => return Some(ty),
        }
    }
    None
}

/// The name of a primitive integer or float type that implements
/// `ScanTarget`, including the `core::ffi` and `libc` C names.
fn scalar_name(ty: &Type) -> Option<&'static str> {
    let Type::Path(path) = ty else {
        return None;
    };
    if path.qself.is_some() {
        return None;
    }
    let segments: Vec<String> = path
        .path
        .segments
        .iter()
        .map(|s| s.ident.to_string())
        .collect();
    let (last, module) = segments.split_last()?;
    let module: Vec<&str> = module.iter().map(String::as_str).collect();
    let c_module = matches!(
        module[..],
        [] | ["core", "ffi"] | ["std", "ffi"] | ["std", "os", "raw"] | ["libc"]
    );
    const RUST: &[&str] = &[
        "i8", "i16", "i32", "i64", "isize", "u8", "u16", "u32", "u64", "usize", "f32", "f64",
    ];
    const C: &[&str] = &[
        "c_char",
        "c_schar",
        "c_uchar",
        "c_short",
        "c_ushort",
        "c_int",
        "c_uint",
        "c_long",
        "c_ulong",
        "c_longlong",
        "c_ulonglong",
        "c_float",
        "c_double",
    ];
    if module.is_empty() {
        if let Some(name) = RUST.iter().find(|n| **n == last) {
            return Some(name);
        }
    }
    if c_module {
        return C.iter().find(|n| **n == last).copied();
    }
    None
}
