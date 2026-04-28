use syn::punctuated::Punctuated;
use syn::token::Comma;
use syn::{Expr, ExprCast, LitByteStr, Pat, Path, Stmt, Type};

use crate::{Depth, Rewriter, SymbolTable};

impl Rewriter {
    /// Rewrite `strstr(e1, e2)` into `xj_cstr::strstr_mut_ptr(e1, e2)` when
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

        let e1 = coerce_u8s(&call.args[0], symbols, true)?;
        let e2 = coerce_u8s(&call.args[1], symbols, false)?;

        self.add_dep("xj_cstr");

        let replacement: Expr = syn::parse_quote! {
            xj_cstr::strstr_mut_ptr(#e1, #e2)
        };

        Some((replacement, Depth::Limited(0)))
    }

    fn peek_array_decay_coercion(mut expr: &Expr) -> Option<&Expr> {
        if let Expr::Cast(cast) = expr {
            if let syn::Type::Ptr(_) = *cast.ty {
                expr = &*cast.expr;
            }
        }

        let Expr::MethodCall(method_call) = expr else {
            return None;
        };
        let is_array_decay_method_call = method_call.args.is_empty()
            && (method_call.method == "as_mut_ptr" || method_call.method == "as_ptr");

        if !is_array_decay_method_call {
            return None;
        }
        Some(&method_call.receiver)
    }

    /// Rewrite `e1.as_mut_ptr()[e2]` into `e1[e2]`
    /// (it's an artifact of guidance).
    pub fn rewrite_decayed_array_subscript(
        &self,
        _symbols: &SymbolTable,
        expr: &Expr,
    ) -> Option<(Expr, Depth)> {
        let Expr::Index(index) = expr else {
            return None;
        };
        if let Some(decayed) = Self::peek_array_decay_coercion(&index.expr) {
            let subscript = &index.index;
            let replacement: Expr = syn::parse_quote! {
                #decayed[#subscript]
            };
            Some((replacement, Depth::Limited(0)))
        } else {
            None
        }
    }

    /// Rewrite `scanf(...)` and `fscanf(stdin, ...)` into `xj_scanf::scanf!(...)`.
    pub fn rewrite_scanf_and_fscanf(
        &self,
        _symbols: &SymbolTable,
        expr: &Expr,
    ) -> Option<(Expr, Depth)> {
        let Expr::Call(call) = expr else {
            return None;
        };
        let Expr::Path(ref func) = *call.func else {
            return None;
        };
        if !(func.path.is_ident("scanf") || func.path.is_ident("fscanf")) {
            return None;
        }

        if func.path.is_ident("fscanf") {
            if call.args.len() < 2 {
                return None;
            }
            let first_arg = &call.args[0];
            if !matches!(first_arg, Expr::Path(fp) if fp.path.is_ident("stdin")) {
                return None;
            }
        }

        let fmt_arg_zero_terminated = if func.path.is_ident("scanf") {
            call.args
                .get(0)
                .expect("scanf should have at least 1 argument")
        } else {
            call.args
                .get(1)
                .expect("fscanf should have at least 2 arguments")
        };

        let Some(fmt_arg) = coerce_str_of_cast_byte_str(fmt_arg_zero_terminated) else {
            eprintln!(
                "synsub: rewrite_scanf_and_fscanf: unsupported format string argument {fmt_arg_zero_terminated:?}"
            );
            return None;
        };

        // eprintln!(
        //     "synsub: rewrite_scanf_and_fscanf: format string argument coerced from  {} to {}"
        // );

        let value_args = if func.path.is_ident("scanf") {
            // skip fmt string
            &call.args.iter().skip(1).collect::<Vec<_>>()
        } else {
            // skip first FILE* arg and fmt string
            &call.args.iter().skip(2).collect::<Vec<_>>()
        };

        let mut scanf_compatible_args = vec![];
        for arg in value_args {
            if let Some(coerced) = coerce_scanf_arg(arg, self) {
                scanf_compatible_args.push(*coerced);
            } else {
                eprintln!("synsub: rewrite_scanf_and_fscanf: unsupported target argument {arg:?}");
                return None;
            }
        }

        self.add_dep("xj_scanf");
        self.with_cur_file_item_store(|item_store| {
            item_store.add_use(false, vec!["xj_scanf".into()], "scanf");
        });

        let comma_punctuated_args: Punctuated<Expr, Comma> =
            Punctuated::from_iter(scanf_compatible_args);

        let scanf_call: Expr = syn::parse_quote! {
            xj_scanf::scanf!(#fmt_arg, #comma_punctuated_args)
        };

        Some((scanf_call, Depth::Limited(0)))
    }

    /// Rewrite statement expressions like `((expr));` into `expr;`.
    pub fn rewrite_stmt_outer_parens(
        &self,
        _symbols: &SymbolTable,
        stmt: &Stmt,
    ) -> Option<(Stmt, Depth)> {
        let Stmt::Expr(expr, semi) = stmt else {
            return None;
        };

        let stripped = expr_strip_parens(expr);
        if std::ptr::eq(stripped, expr) {
            return None;
        }

        Some((Stmt::Expr(stripped.clone(), *semi), Depth::Limited(0)))
    }

    /// Rewrite
    ///     `*s1.offset(s1.len().wrapping_sub(1 as size_t) as isize) = '\0' as ::core::ffi::c_char;`
    ///  or `*s1.offset((s1.len() as ___ - 1 as ___) as isize) = '\0' as ::core::ffi::c_char;`
    /// into
    ///      `s1.pop();`
    /// when `s1` is an identifier typed as `String`.
    pub fn rewrite_string_pop_trailing_nul(
        &self,
        symbols: &SymbolTable,
        stmt: &Stmt,
    ) -> Option<(Stmt, Depth)> {
        let Stmt::Expr(Expr::Assign(assign), Some(_)) = stmt else {
            return None;
        };
        if !is_nul_char_expr(&assign.right) {
            return None;
        }

        let Expr::Unary(unary) = expr_strip_parens(&assign.left) else {
            return None;
        };
        if !matches!(unary.op, syn::UnOp::Deref(_)) {
            return None;
        }

        let Expr::MethodCall(call) = expr_strip_parens(&unary.expr) else {
            return None;
        };
        if call.method != "offset" || call.args.len() != 1 {
            return None;
        }

        if !is_string_expr(&call.receiver, symbols) {
            return None;
        }

        let base_ident: &syn::Ident = expr_ident_name(&call.receiver)?;
        if !is_len_sub_one_as_isize_expr(&call.args[0], base_ident) {
            return None;
        }

        let receiver = &call.receiver;
        let replacement: Stmt = syn::parse_quote! {
            #receiver.pop();
        };
        Some((replacement, Depth::Limited(0)))
    }

    /// Rewrite `strlen(e1.as_mut_ptr())` into:
    ///   * `(e1.len() - 1) as size_t` when `e1` is a `u8` slice (with the trailing NUL kept)
    ///     and we've determined that e1 will never have its length changed by having
    ///     a null byte written into its interior. (NOT YET IMPLEMENTED)
    ///   * `CStr::from_bytes_until_nul(e1).count_bytes() as size_t` when e1 is a `u8` slice.
    ///   * `CStr::from_bytes_until_nul(e1.as_u8_slice()).count_bytes() as size_t` otherwise.
    pub fn rewrite_strlen_of_slice(
        &self,
        _symbols: &SymbolTable,
        expr: &Expr,
    ) -> Option<(Expr, Depth)> {
        let Expr::Call(call) = expr else {
            return None;
        };
        let Expr::Path(ref func) = *call.func else {
            return None;
        };
        if !func.path.is_ident("strlen") {
            return None;
        }
        if call.args.len() != 1 {
            return None;
        }

        let arg = &call.args[0];
        if let Some(decayed) = Self::peek_array_decay_coercion(arg) {
            if is_u8_slice_expr(decayed, _symbols) {
                let replacement: Expr = syn::parse_quote! {
                    (::std::ffi::CStr::from_bytes_until_nul(#decayed).unwrap().count_bytes()) as size_t
                };
                Some((replacement, Depth::Limited(0)))
            } else {
                self.add_dep("xj_cstr");
                self.with_cur_file_item_store(|item_store| {
                    item_store.add_use(true, vec!["xj_cstr".into()], "ByteSlice");
                });
                let replacement: Expr = syn::parse_quote! {
                    (::std::ffi::CStr::from_bytes_until_nul(#decayed.as_u8_slice()).unwrap().count_bytes()) as size_t
                };
                Some((replacement, Depth::Limited(0)))
            }
        } else {
            None
        }
    }

    /// Rewrite let-binding statements.
    pub fn rewrite_local(&self, symbols: &SymbolTable, stmt: &Stmt) -> Option<(Stmt, Depth)> {
        let Stmt::Local(local) = stmt else {
            return None;
        };
        let Pat::Type(pat_type) = &local.pat else {
            return None;
        };
        let Some(localinit) = &local.init else {
            return None;
        };

        if let Some(elt_ty) = type_of_slice_ref(&pat_type.ty) {
            if is_u8_type(elt_ty) {
                let init_expr = &localinit.expr;
                let coerced = coerce_u8s(init_expr, symbols, true)?;
                let replacement: Stmt = syn::parse_quote! {
                    let #pat_type = #coerced;
                };
                return Some((replacement, Depth::Limited(0)));
            }
        }

        None
    }
}

enum ScanfArgCategory {
    Borrow(Box<Expr>),
    AsMutPtr(Box<Expr>),
    Other,
}

fn categorize_scanf_arg(expr: &Expr) -> ScanfArgCategory {
    if let Expr::Reference(reference) = expr {
        return ScanfArgCategory::Borrow(reference.expr.clone());
    }
    if let Expr::RawAddr(reference) = expr {
        return ScanfArgCategory::Borrow(reference.expr.clone());
    }

    if let Expr::MethodCall(method_call) = expr {
        if method_call.method == "as_mut_ptr" {
            return ScanfArgCategory::AsMutPtr(method_call.receiver.clone());
        }
    }

    ScanfArgCategory::Other
}

fn coerce_scanf_arg(expr: &Expr, rewriter: &Rewriter) -> Option<Box<Expr>> {
    match categorize_scanf_arg(expr) {
        ScanfArgCategory::Borrow(e) => Some(Box::new(syn::parse_quote! { &mut #e })),
        ScanfArgCategory::AsMutPtr(e) => {
            rewriter.add_dep("xj_cstr");
            rewriter.with_cur_file_item_store(|item_store| {
                item_store.add_use(true, vec!["xj_cstr".into()], "ByteSlice");
            });
            Some(Box::new(syn::parse_quote! { &mut #e.as_mut_u8_slice() }))
        }
        ScanfArgCategory::Other => None,
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
fn coerce_u8s(mut expr: &Expr, symbols: &SymbolTable, exclusive: bool) -> Option<Box<Expr>> {
    expr = expr_strip_transmute_deref(expr_strip_casts(expr));

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
        ::core::ffi::CStr::from_ptr(#expr).to_bytes()
    };
    Some(Box::new(coerced))
}

fn get_litbytestr(expr: &Expr) -> Option<LitByteStr> {
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

    Some(byte_str.clone())
}

fn bytes_strip_trailing_zero(mut bytes: Vec<u8>) -> Vec<u8> {
    if bytes.last().copied() == Some(0) {
        bytes.pop();
    }
    bytes
}

/// If `expr` is a casted `b"...\0"` literal, strip the trailing NUL.
/// Returns `None` if `expr` is not a casted byte string literal with an optional trailing NUL.
fn coerce_cast_byte_str(expr: &Expr) -> Option<Box<Expr>> {
    let byte_str: LitByteStr = get_litbytestr(expr)?;
    let bytes_sans_zero = bytes_strip_trailing_zero(byte_str.value());
    let trimmed = syn::LitByteStr::new(&bytes_sans_zero, byte_str.span());
    Some(Box::new(syn::parse_quote! { #trimmed }))
}

/// If `expr` is a casted `b"...\0"` literal, strip the trailing NUL.
/// Returns `None` if `expr` is not a casted byte string literal with an optional trailing NUL.
fn coerce_str_of_cast_byte_str(expr: &Expr) -> Option<Box<Expr>> {
    let byte_str: LitByteStr = get_litbytestr(expr)?;
    let bytes_sans_zero = bytes_strip_trailing_zero(byte_str.value());
    let str_val = std::str::from_utf8(&bytes_sans_zero).ok()?;
    let trimmed = syn::LitStr::new(str_val, byte_str.span());
    Some(Box::new(syn::parse_quote! { #trimmed }))
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

/// Returns `true` when `expr` is an identifier typed as `String`.
fn is_string_expr(expr: &Expr, symbols: &SymbolTable) -> bool {
    matches!(expr_ident_type(expr, symbols), Some(ty) if is_string_type(ty))
}

fn expr_ident_name(expr: &Expr) -> Option<&syn::Ident> {
    let Expr::Path(ref ep) = *expr_strip_parens(expr) else {
        return None;
    };
    ep.path.get_ident()
}

fn expr_ident_type<'a>(expr: &Expr, symbols: &'a SymbolTable) -> Option<&'a syn::Type> {
    let Expr::Path(ref ep) = *expr else {
        return None;
    };
    let ident = ep.path.get_ident()?;
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

fn is_string_type(ty: &syn::Type) -> bool {
    matches!(ty, syn::Type::Path(path) if path.path.segments.last().is_some_and(|segment| segment.ident == "String"))
}

fn type_of_slice_ref(ty: &Type) -> Option<&Type> {
    match ty {
        Type::Reference(tref) => match &*tref.elem {
            Type::Slice(slice) => Some(&slice.elem),
            _ => None,
        },
        _ => None,
    }
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

fn expr_strip_casts(expr: &Expr) -> &Expr {
    let mut ep = expr;
    loop {
        match ep {
            Expr::Cast(ExprCast { expr, .. }) => ep = expr,
            _ => break ep,
        }
    }
}

fn expr_strip_parens(expr: &Expr) -> &Expr {
    let mut ep = expr;
    loop {
        match ep {
            Expr::Paren(paren) => ep = &paren.expr,
            _ => break ep,
        }
    }
}

fn is_nul_char_expr(expr: &Expr) -> bool {
    matches!(expr_strip_parens(expr_strip_casts(expr)), Expr::Lit(lit) if matches!(&lit.lit, syn::Lit::Char(ch) if ch.value() == '\0'))
}

fn is_one_expr(expr: &Expr) -> bool {
    matches!(expr_strip_parens(expr_strip_casts(expr)), Expr::Lit(lit) if matches!(&lit.lit, syn::Lit::Int(int) if int.base10_digits() == "1"))
}

fn split_binary_or_wrapping_sub(expr: &Expr) -> Option<(&Expr, &Expr)> {
    let expr = expr_strip_parens(expr_strip_casts(expr));
    if let Expr::Binary(bin) = expr {
        if matches!(bin.op, syn::BinOp::Sub(_)) {
            return Some((&bin.left, &bin.right));
        }
    } else if let Expr::MethodCall(method_call) = expr {
        if method_call.method == "wrapping_sub" && method_call.args.len() == 1 {
            return Some((&method_call.receiver, &method_call.args[0]));
        }
    }
    None
}

fn is_len_sub_one_as_isize_expr(expr: &Expr, expected_ident: &syn::Ident) -> bool {
    let Some((left, right)) = split_binary_or_wrapping_sub(expr) else {
        return false;
    };

    if !is_one_expr(right) {
        return false;
    }

    let Expr::MethodCall(len_call) = expr_strip_casts(expr_strip_parens(left)) else {
        return false;
    };
    if len_call.method != "len" || !len_call.args.is_empty() {
        return false;
    }

    matches!(expr_ident_name(&len_call.receiver), Some(ident) if ident == expected_ident)
}

pub fn is_path_exactly_1(path: &Path, a: &str) -> bool {
    if path.segments.len() == 1 {
        path.segments[0].ident.to_string().as_str() == a
    } else {
        false
    }
}

// fn is_path_exactly_2(path: &Path, a: &str, b: &str) -> bool {
//     if path.segments.len() == 2 {
//         path.segments[0].ident.to_string().as_str() == a
//             && path.segments[1].ident.to_string().as_str() == b
//     } else {
//         false
//     }
// }

fn is_path_exactly_3(path: &Path, a: &str, b: &str, c: &str) -> bool {
    if path.segments.len() == 3 {
        path.segments[0].ident.to_string().as_str() == a
            && path.segments[1].ident.to_string().as_str() == b
            && path.segments[2].ident.to_string().as_str() == c
    } else {
        false
    }
}

fn expr_is_transmute(expr: &Expr) -> bool {
    if let Expr::Path(ref path) = *expr {
        if is_path_exactly_1(&path.path, "transmute") {
            return true;
        }
        if is_path_exactly_3(&path.path, "core", "mem", "transmute") {
            eprintln!("++++++++++++found core::mem::transmute");
            return true;
        }
        if is_path_exactly_3(&path.path, "core", "intrinsics", "transmute") {
            return true;
        }
    }
    false
}

fn expr_strip_transmute_deref(expr: &Expr) -> &Expr {
    let mut ep = expr;
    loop {
        match ep {
            Expr::Call(syn::ExprCall { func, args, .. }) => {
                if expr_is_transmute(func) && args.len() == 1 {
                    if let Expr::Unary(syn::ExprUnary {
                        op: syn::UnOp::Deref(_),
                        expr,
                        ..
                    }) = &args[0]
                    {
                        ep = expr;
                    } else {
                        break ep;
                    }
                } else {
                    break ep;
                }
            }
            _ => break ep,
        }
    }
}
