use syn::Expr;

use crate::{Depth, Rewriter, SymbolTable};

impl Rewriter {
    /// Rewrite `strstr(e1, e2)` into `xj_cstr::strstr_ptr(e1, e2)` when
    /// both arguments can be coerced to byte slices.
    pub fn rewrite_strstr(&self, symbols: &SymbolTable, expr: &Expr) -> Option<(Expr, Depth)> {
        let Expr::Call(call) = expr else {
            return None;
        };
        let Expr::Path(ref func) = *call.func else {
            return None;
        };
        if !func.path.is_ident("strstr") {
            return None;
        }
        if call.args.len() != 2 {
            return None;
        }

        let e1 = coerce_u8s(&call.args[0], symbols, false)?;
        let e2 = coerce_u8s(&call.args[1], symbols, false)?;

        self.add_dep("xj_cstr");

        let replacement: Expr = syn::parse_quote! {
            xj_cstr::strstr_ptr(#e1, #e2)
        };

        Some((replacement, Depth::Limited(0)))
    }
}

/// Coerce supported string-like inputs into `u8` slice expressions.
///
/// Handles:
/// - casted byte-string literals (trimming trailing `\0` when present),
/// - string literals and `&str` identifiers (`x.as_bytes()`),
/// - `x.as_mut_ptr()` on `u8` slices (`x.as_u8_slice()`),
/// - pointer expressions (`CStr::from_ptr(x).to_bytes()`) when
///   `exclusive == false`.
fn coerce_u8s(expr: &Expr, symbols: &SymbolTable, exclusive: bool) -> Option<Box<Expr>> {
    if let Some(coerced) = coerce_cast_byte_str(expr) {
        return Some(coerced);
    }
    if is_cast_byte_str(expr) || is_u8_slice_expr(expr, symbols) {
        return Some(Box::new(expr.clone()));
    }
    if let Some(coerced) = coerce_str_as_bytes(expr, symbols) {
        return Some(coerced);
    }
    if let Some(coerced) = coerce_slice_ptr_call(expr, symbols) {
        return Some(coerced);
    }
    if exclusive || !is_pointer_expr(expr, symbols) {
        return None;
    }

    let coerced: Expr = syn::parse_quote! {
        CStr::from_ptr(#expr).to_bytes()
    };
    Some(Box::new(coerced))
}

/// If `expr` is a casted `b"...\0"` literal, strip the trailing NUL.
fn coerce_cast_byte_str(expr: &Expr) -> Option<Box<Expr>> {
    let mut inner = expr;
    while let Expr::Cast(cast) = inner {
        inner = &cast.expr;
    }

    let Expr::Lit(expr_lit) = inner else {
        return None;
    };
    let syn::Lit::ByteStr(byte_str) = &expr_lit.lit else {
        return None;
    };

    let mut bytes = byte_str.value();
    if bytes.last().copied() != Some(0) {
        return None;
    }
    bytes.pop();

    let trimmed = syn::LitByteStr::new(&bytes, byte_str.span());
    let coerced: Expr = syn::parse_quote! { #trimmed };
    Some(Box::new(coerced))
}

/// Convert string literals and `&str` identifiers to `x.as_bytes()`.
fn coerce_str_as_bytes(expr: &Expr, symbols: &SymbolTable) -> Option<Box<Expr>> {
    if !is_str_expr(expr, symbols) {
        return None;
    }

    let coerced: Expr = syn::parse_quote! {
        #expr.as_bytes()
    };
    Some(Box::new(coerced))
}

/// Convert `x.as_mut_ptr()` on a `u8` slice expression into `x.as_u8_slice()`.
fn coerce_slice_ptr_call(expr: &Expr, symbols: &SymbolTable) -> Option<Box<Expr>> {
    let Expr::MethodCall(call) = expr else {
        return None;
    };
    if call.method != "as_mut_ptr" || !call.args.is_empty() {
        return None;
    }
    if !is_u8_slice_expr(&call.receiver, symbols) {
        return None;
    }

    let receiver = &call.receiver;
    let coerced: Expr = syn::parse_quote! {
        #receiver.as_u8_slice()
    };
    Some(Box::new(coerced))
}

/// Returns `true` when `expr` is an identifier typed as a `u8` slice.
fn is_u8_slice_expr(expr: &Expr, symbols: &SymbolTable) -> bool {
    matches!(expr_ident_type(expr, symbols), Some(ty) if is_u8_slice_type(ty))
}

/// Returns `true` when `expr` is a string literal or an `&str` identifier.
fn is_str_expr(expr: &Expr, symbols: &SymbolTable) -> bool {
    if matches!(expr, Expr::Lit(lit) if matches!(lit.lit, syn::Lit::Str(_))) {
        return true;
    }
    matches!(expr_ident_type(expr, symbols), Some(ty) if is_ref_str_type(ty))
}

fn expr_ident_type<'a>(expr: &Expr, symbols: &'a SymbolTable) -> Option<&'a syn::Type> {
    let Expr::Path(ref ep) = *expr else {
        return None;
    };
    let Some(ident) = ep.path.get_ident() else {
        return None;
    };
    symbols.get(&ident.to_string())
}

fn is_u8_slice_type(ty: &syn::Type) -> bool {
    match ty {
        syn::Type::Slice(slice) => is_u8_type(&slice.elem),
        syn::Type::Reference(reference) => {
            matches!(&*reference.elem, syn::Type::Slice(slice) if is_u8_type(&slice.elem))
        }
        _ => false,
    }
}

fn is_u8_type(ty: &syn::Type) -> bool {
    matches!(ty, syn::Type::Path(path) if path.path.is_ident("u8"))
}

fn is_ref_str_type(ty: &syn::Type) -> bool {
    let syn::Type::Reference(reference) = ty else {
        return false;
    };
    matches!(&*reference.elem, syn::Type::Path(path) if path.path.is_ident("str"))
}

fn is_pointer_expr(expr: &Expr, symbols: &SymbolTable) -> bool {
    if let Expr::Cast(cast) = expr {
        if matches!(&*cast.ty, syn::Type::Ptr(_)) {
            return true;
        }
    }

    matches!(expr_ident_type(expr, symbols), Some(syn::Type::Ptr(_)))
}

/// Returns `true` for a byte-string literal wrapped in zero or more casts.
fn is_cast_byte_str(expr: &Expr) -> bool {
    match expr {
        Expr::Lit(lit) => matches!(lit.lit, syn::Lit::ByteStr(_)),
        Expr::Cast(cast) => is_cast_byte_str(&cast.expr),
        _ => false,
    }
}
