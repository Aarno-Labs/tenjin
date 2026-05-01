use syn::punctuated::Punctuated;
use syn::spanned::Spanned;
use syn::token::Comma;
use syn::{Expr, ExprCast, ExprLit, ExprPath, LitByteStr, LitInt, Pat, Path, Stmt, Type};

use crate::{Depth, Rewriter, SymbolTable};

impl Rewriter {
    /// Rewrite `getchar()` and `fgetc(stdin)`
    pub fn rewrite_getchar_variants(
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
        fn is_getchar(func: &ExprPath, call: &syn::ExprCall) -> bool {
            func.path.is_ident("getchar") && call.args.is_empty()
        }
        fn is_fgetc_stdin(func: &ExprPath, call: &syn::ExprCall) -> bool {
            if !func.path.is_ident("fgetc") || call.args.len() != 1 {
                return false;
            }
            matches!(call.args.first(), Some(Expr::Path(fp)) if fp.path.is_ident("stdin"))
        }
        if !is_getchar(func, call) && !is_fgetc_stdin(func, call) {
            return None;
        }

        self.with_cur_file_item_store(|item_store| {
            item_store.add_use(true, vec!["std".into(), "io".into()], "Read");
            item_store.add_item_str_once(
                "fn xj_getchar_i() -> ::core::ffi::c_int {
    std::io::stdin()
        .bytes()
        .next()
        .map_or(-1, |b| b.map_or(-1, |byte| byte as i32))
}",
            );
        });

        let replacement: Expr = syn::parse_quote! {
            xj_getchar_i()
        };

        Some((replacement, Depth::Limited(0)))
    }

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

    /// Rewrite `fgets(e1.as_mut_ptr(), e2, e3).is_null()`
    /// into `fgets_stdin_u8_count(e1.as_mut_u8_slice(), e2, e3).is_none()`
    pub fn rewrite_fgets_stdin_is_null(
        &self,
        symbols: &SymbolTable,
        expr: &Expr,
    ) -> Option<(Expr, Depth)> {
        let Expr::MethodCall(method_call) = expr else {
            return None;
        };
        if method_call.method != "is_null" || !method_call.args.is_empty() {
            return None;
        }

        let Expr::Call(call) = &*method_call.receiver else {
            return None;
        };
        let Expr::Path(ref func) = *call.func else {
            return None;
        };
        if !func.path.is_ident("fgets") {
            return None;
        }
        if call.args.len() != 3 {
            return None;
        }

        let name = expr_ident_name(&call.args[2])?;
        if name != "stdin" {
            return None;
        }

        let first_arg = &call.args[0];
        if let Some(decayed) = Self::peek_array_decay_coercion(first_arg) {
            // If the first argument is a String, we should use a different (simpler!) helper.
            if is_u8_or_i8_sliceable_expr(decayed, symbols)
                && is_effectively_mutable_expr(decayed, symbols)
            {
                self.add_dep("xj_cstr");
                self.with_cur_file_item_store(|item_store| {
                    item_store.add_use(true, vec!["xj_cstr".into()], "ByteSlice");
                    item_store.add_use(true, vec!["std".into(), "io".into()], "BufRead");
                    item_store.add_item_str_once(
                        "fn fgets_stdin_u8_count(buf: &mut [u8], limit: usize) -> Option<usize> {
    let f = std::io::stdin();
    let mut handle = f.lock();

    let Ok(src) = handle.fill_buf() else {
        return None; // error
    };
    if src.is_empty() {
        return None; // EOF
    }

    let n = src.iter()
        .position(|&b| b == b'\\n')
        .map(|i| i + 1)          // include the '\\n'
        .unwrap_or(src.len())
        .min(limit - 1); // leave room for the trailing NUL

    buf[..n].copy_from_slice(&src[..n]);
    buf[n] = 0; // NUL-terminate
    handle.consume(n);
    Some(n)
}",
                    );
                });
                let limit = &call.args[1];
                let replacement: Expr = syn::parse_quote! {
                    fgets_stdin_u8_count(#decayed.as_mut_u8_slice(), #limit as usize).is_none()
                };
                return Some((replacement, Depth::Limited(0)));
            }
        }

        None
    }

    /// Given `e.as_mut_ptr()`, implying that `e` is an array or slice, return `Some(e)`.
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

    /// Rewrite `printf(x.offset(e))` into `io::stdout().write_all(x[e..].as_u8_slice())`
    pub fn rewrite_printf_with_lone_offset_fmt(
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
        if !func.path.is_ident("printf") {
            return None;
        }
        if call.args.len() != 1 {
            return None;
        }

        let arg = &call.args[0];
        let Expr::MethodCall(method_call) = expr_strip_parens(arg) else {
            return None;
        };
        if method_call.method != "offset" || method_call.args.len() != 1 {
            return None;
        }

        let base = coerce_u8s(&method_call.receiver, symbols, false)?;
        let offset = as_usize(&method_call.args[0]);

        self.with_cur_file_item_store(|item_store| {
            item_store.add_use(false, vec!["std".into(), "io".into()], "Write");
        });

        let replacement: Expr = syn::parse_quote! {
            ::std::io::stdout().write_all(& #base[#offset..])
        };
        Some((replacement, Depth::Limited(0)))
    }

    /// Rewrite `usleep(n)` into `std::thread::sleep(std::time::Duration::from_micros(n))`.
    pub fn rewrite_usleep(&self, _symbols: &SymbolTable, expr: &Expr) -> Option<(Expr, Depth)> {
        let Expr::Call(call) = expr else {
            return None;
        };
        let Expr::Path(ref func) = *call.func else {
            return None;
        };
        if !func.path.is_ident("usleep") {
            return None;
        }
        if call.args.len() != 1 {
            return None;
        }

        let arg = as_u64(&call.args[0]);

        let replacement: Expr = syn::parse_quote! {
            std::thread::sleep(std::time::Duration::from_micros(#arg))
        };
        Some((replacement, Depth::Limited(0)))
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

        let base_ident: &syn::Ident = expr_ident(&call.receiver)?;
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
            if is_u8_sliceable_expr(decayed, _symbols) {
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

    pub fn strip_as_c_float_of_int_literals(
        &self,
        _symbols: &SymbolTable,
        expr: &Expr,
    ) -> Option<(Expr, Depth)> {
        let Expr::Cast(ExprCast {
            expr: inner_expr,
            ty,
            ..
        }) = expr
        else {
            return None;
        };
        if !is_c_float_type(ty) {
            return None;
        }
        let int_lit = expr_get_int_literal(inner_expr)?;

        let int_lit_digits = format!("{}.", int_lit.base10_digits());
        let lit_float = syn::LitFloat::new(&int_lit_digits, expr.span());
        let lit_float_expr = Expr::Lit(ExprLit {
            attrs: Vec::new(),
            lit: syn::Lit::Float(lit_float),
        });

        Some((lit_float_expr, Depth::Limited(0)))
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

fn as_usize(expr: &Expr) -> Box<Expr> {
    Box::new(syn::parse_quote! { #expr as usize })
}

fn as_u64(expr: &Expr) -> Box<Expr> {
    Box::new(syn::parse_quote! { #expr as u64 })
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
    expr = expr_strip_transmute_deref(expr_strip_casts(expr_strip_parens(expr)));

    if let Some(coerced) = coerce_cast_byte_str(expr) {
        return Some(coerced);
    }
    if is_cast_byte_str(expr) || is_u8_sliceable_expr(expr, symbols) {
        return Some(Box::new(expr.clone()));
    }
    if let Some(coerced) = coerce_str_as_bytes(expr, symbols) {
        return Some(coerced);
    }
    if let Some(coerced) = coerce_slice_ptr_call(expr, symbols, exclusive) {
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

fn extract_slice_ptr_base(expr: &Expr) -> Option<&Expr> {
    let Expr::MethodCall(call) = expr else {
        return None;
    };
    if call.method != "as_mut_ptr" || !call.args.is_empty() {
        return None;
    }
    Some(&call.receiver)
}

/// Convert `x.as_mut_ptr()` on a `u8` slice expression into `x.as_u8_slice()`.
fn coerce_slice_ptr_call(expr: &Expr, symbols: &SymbolTable, exclusive: bool) -> Option<Box<Expr>> {
    let receiver = extract_slice_ptr_base(expr)?;
    if !is_u8_or_i8_sliceable_expr(receiver, symbols) {
        return None;
    }

    let method = if exclusive {
        "as_mut_u8_slice"
    } else {
        "as_u8_slice"
    };
    let coerced: Expr = syn::parse_quote! {
        #receiver.#method()
    };
    Some(Box::new(coerced))
}

/// Returns `true` when `expr` is an identifier typed as a `u8` slice.
fn is_u8_sliceable_expr(expr: &Expr, symbols: &SymbolTable) -> bool {
    matches!(expr_ident_type(expr, symbols), Some(ty) if is_u8_sliceable_type(ty))
}

/// Returns `true` when `expr` is an identifier typed as a `u8` slice.
fn is_u8_or_i8_sliceable_expr(expr: &Expr, symbols: &SymbolTable) -> bool {
    matches!(expr_ident_type(expr, symbols), Some(ty) if is_u8_or_i8_sliceable_type(ty))
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

/// Returns `true` when `expr` is an owned or exclusively borrowed type
fn is_effectively_mutable_expr(expr: &Expr, symbols: &SymbolTable) -> bool {
    matches!(expr_ident_type(expr, symbols), Some(ty) if is_effectively_mutable_type(ty))
}

fn expr_get_int_literal(expr: &Expr) -> Option<LitInt> {
    let Expr::Lit(ExprLit {
        lit: syn::Lit::Int(lit_int),
        ..
    }) = expr
    else {
        return None;
    };
    Some(lit_int.clone())
}

fn expr_ident(expr: &Expr) -> Option<&syn::Ident> {
    let Expr::Path(ref ep) = *expr_strip_parens(expr) else {
        return None;
    };
    ep.path.get_ident()
}

fn expr_ident_name(expr: &Expr) -> Option<String> {
    let ident = expr_ident(expr)?;
    Some(ident.to_string())
}

fn expr_ident_type<'a>(expr: &Expr, symbols: &'a SymbolTable) -> Option<&'a syn::Type> {
    let name = expr_ident_name(expr)?;
    symbols.get(&name)
}

fn is_u8_sliceable_type(ty: &syn::Type) -> bool {
    sliceable_type_elt_is(ty, is_u8_type)
}

fn is_u8_or_i8_sliceable_type(ty: &syn::Type) -> bool {
    sliceable_type_elt_is(ty, is_u8_or_i8_type)
}

/// Returns `true` for types that are either owned
/// or borrowed with exclusive access (e.g. `&mut T`).
fn is_effectively_mutable_type(ty: &syn::Type) -> bool {
    match ty {
        syn::Type::Array(_) => true,
        syn::Type::Slice(_) => true,
        syn::Type::Paren(tp) => is_effectively_mutable_type(&tp.elem),
        syn::Type::Path(_) => true, // owned types are effectively mutable
        syn::Type::Reference(reference) => reference.mutability.is_some(),
        _ => false,
    }
}

fn sliceable_type_elt_is(ty: &syn::Type, pred: fn(&syn::Type) -> bool) -> bool {
    fn array_or_slice_elt_is(ty: &syn::Type, pred: fn(&syn::Type) -> bool) -> bool {
        match ty {
            syn::Type::Array(array) => pred(&array.elem),
            syn::Type::Slice(slice) => pred(&slice.elem),
            _ => false,
        }
    }
    match ty {
        syn::Type::Reference(reference) => array_or_slice_elt_is(&reference.elem, pred),
        _ => array_or_slice_elt_is(ty, pred),
    }
}

fn is_u8_or_i8_path(p: &syn::Path) -> bool {
    p.is_ident("u8")
        || p.is_ident("i8")
        || p.segments.last().is_some_and(|segment| {
            segment.ident == "c_char" || segment.ident == "c_schar" || segment.ident == "c_uchar"
        })
}

fn is_u8_or_i8_type(ty: &syn::Type) -> bool {
    matches!(ty, syn::Type::Path(path) if is_u8_or_i8_path(&path.path))
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

fn is_c_float_type(ty: &syn::Type) -> bool {
    matches!(ty, syn::Type::Path(path) if path.path.segments.last().is_some_and(|segment| segment.ident == "c_float"))
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

    matches!(expr_ident(&len_call.receiver), Some(ident) if ident == expected_ident)
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
