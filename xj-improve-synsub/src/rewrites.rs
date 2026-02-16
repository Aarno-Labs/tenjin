use syn::Expr;

use crate::{Depth, Rewriter, SymbolTable};

impl Rewriter {
    /// Rewrite `strstr(e1, e2)` into `xj_cstr::strstr_ptr(e1, e2)` when
    /// both arguments are either (possibly cast) byte-string literals or
    /// identifiers whose symbol-table type is `&[u8]` or `&str`.
    pub fn rewrite_strstr(
        &self,
        symbols: &SymbolTable,
        expr: &Expr,
    ) -> Option<(Expr, Depth)> {
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

        let e1 = &call.args[0];
        let e2 = &call.args[1];

        if !is_eligible(e1, symbols) || !is_eligible(e2, symbols) {
            return None;
        }

        self.add_dep("xj_cstr");

        let replacement: Expr = syn::parse_quote! {
            xj_cstr::strstr_ptr(#e1, #e2)
        };

        Some((replacement, Depth::Limited(0)))
    }
}

/// An argument is eligible if it is a (possibly cast) byte-string literal,
/// or an identifier whose symbol-table type is `&[u8]` or `&str`.
fn is_eligible(expr: &Expr, symbols: &SymbolTable) -> bool {
    if is_cast_byte_str(expr) {
        return true;
    }
    if let Expr::Path(ref ep) = *expr {
        if let Some(ident) = ep.path.get_ident() {
            if let Some(ty) = symbols.get(&ident.to_string()) {
                return is_ref_str_like(ty);
            }
        }
    }
    false
}

/// Returns `true` for a byte-string literal wrapped in zero or more casts.
fn is_cast_byte_str(expr: &Expr) -> bool {
    match expr {
        Expr::Lit(lit) => matches!(lit.lit, syn::Lit::ByteStr(_)),
        Expr::Cast(cast) => is_cast_byte_str(&cast.expr),
        _ => false,
    }
}

/// Returns `true` if `ty` is `&[u8]` or `&str` (any lifetime/mutability).
fn is_ref_str_like(ty: &syn::Type) -> bool {
    let syn::Type::Reference(r) = ty else {
        return false;
    };
    match &*r.elem {
        syn::Type::Path(p) => p.path.is_ident("str"),
        syn::Type::Slice(s) => {
            matches!(&*s.elem, syn::Type::Path(p) if p.path.is_ident("u8"))
        }
        _ => false,
    }
}
