use syn::punctuated::Punctuated;
use syn::spanned::Spanned;
use syn::token::Comma;
use syn::{
    Expr, ExprCast, ExprLit, ExprPath, ExprUnary, LitByteStr, LitInt, Pat, Path, Stmt, Type,
};

use crate::{
    AtomicKind, Depth, Rewriter, SymbolTable, atomic_kind, atomic_kind_from_name,
    byte_input::is_u8_type, c_atomic_rust_name,
};

fn paren_if_cast(expr: &Expr) -> proc_macro2::TokenStream {
    if let Expr::Cast(_) = expr {
        syn::parse_quote! { (#expr) }
    } else {
        syn::parse_quote! { #expr }
    }
}

impl Rewriter {
    /// Compare a byte and a byte-sized character without promoting them to C integers.
    pub fn rewrite_u8_char_comparison(
        &self,
        symbols: &SymbolTable,
        expr: &Expr,
    ) -> Option<(Expr, Depth)> {
        let Expr::Binary(binary) = expr else {
            return None;
        };
        // Other operators can change the result type or overflow behavior.
        if !is_comparison_op(&binary.op) {
            return None;
        }
        let (Expr::Cast(left), Expr::Cast(right)) = (
            expr_strip_parens(&binary.left),
            expr_strip_parens(&binary.right),
        ) else {
            return None;
        };
        let is_c_int = |ty: &Type| {
            matches!(ty, Type::Path(path)
                if path.qself.is_none() && is_path_exactly_3(&path.path, "core", "ffi", "c_int"))
        };
        if !is_c_int(&left.ty) || !is_c_int(&right.ty) {
            return None;
        }
        let byte = expr_strip_parens(&left.expr);
        let byte_ty = match byte {
            Expr::Cast(cast) => (*cast.ty).clone(),
            _ => symbols.type_of_expr(byte)?,
        };
        if !matches!(&byte_ty, Type::Path(path) if path.qself.is_none()) || !is_u8_type(&byte_ty) {
            return None;
        }
        let Expr::Lit(ExprLit {
            lit: syn::Lit::Char(character),
            ..
        }) = expr_strip_parens(&right.expr)
        else {
            return None;
        };
        // A wider Unicode character would be truncated by the replacement cast.
        if u32::from(character.value()) > u32::from(u8::MAX) {
            return None;
        }
        let op = &binary.op;
        let byte = paren_if_cast(byte);
        Some((
            syn::parse_quote! { #byte #op #character as u8 },
            Depth::Unlimited,
        ))
    }

    /// Remove the C integer round trip from a known boolean condition.
    pub fn rewrite_bool_cast_nonzero(
        &self,
        symbols: &SymbolTable,
        expr: &Expr,
    ) -> Option<(Expr, Depth)> {
        let Expr::Binary(binary) = expr else {
            return None;
        };
        if !matches!(binary.op, syn::BinOp::Ne(_)) || !is_zero_integer_expr(&binary.right) {
            return None;
        }
        let Expr::Cast(cast) = expr_strip_parens(&binary.left) else {
            return None;
        };
        if !matches!(&*cast.ty, Type::Path(path)
            if path.qself.is_none() && is_path_exactly_3(&path.path, "core", "ffi", "c_int"))
            || !is_bool_expr(&cast.expr, symbols)
        {
            return None;
        }
        Some(((*cast.expr).clone(), Depth::Unlimited))
    }

    /// Replace accesses to glibc's errno storage with the errno crate's API.
    pub fn rewrite_errno_access(
        &self,
        _symbols: &SymbolTable,
        expr: &Expr,
    ) -> Option<(Expr, Depth)> {
        let replacement = match expr {
            Expr::Assign(assign) if is_errno_deref(&assign.left) => {
                let value = &assign.right;
                syn::parse_quote! { ::errno::set_errno(::errno::Errno(#value)) }
            }
            Expr::Binary(binary)
                if is_errno_deref(&binary.left) && is_assignment_operator(&binary.op) =>
            {
                let value = &binary.right;
                let op = &binary.op;
                // Primitive compound assignment evaluates the RHS before the
                // LHS. It may itself change errno, so read errno afterwards.
                syn::parse_quote! {{
                    let xj_errno_rhs = #value;
                    let mut xj_errno_value = ::errno::errno().0;
                    xj_errno_value #op xj_errno_rhs;
                    ::errno::set_errno(::errno::Errno(xj_errno_value));
                }}
            }
            // Borrowing the actual storage must not become a borrow of a copy.
            Expr::Reference(reference) if is_errno_deref(&reference.expr) => {
                return Some((expr.clone(), Depth::Limited(0)));
            }
            Expr::RawAddr(addr) if is_errno_deref(&addr.expr) => {
                return Some((expr.clone(), Depth::Limited(0)));
            }
            // Destructuring assignment requires real places. Keep these rare
            // writes intact rather than rewriting their targets into values.
            Expr::Assign(assign) if contains_errno_assignment_place(&assign.left) => {
                return Some((expr.clone(), Depth::Limited(0)));
            }
            Expr::Unary(_) if is_errno_deref(expr) => {
                syn::parse_quote! { ::errno::errno().0 }
            }
            _ => return None,
        };
        self.add_dep("errno");
        Some((replacement, Depth::Unlimited))
    }

    /// Rewrite a call to a C math function, such as `sqrt(x)`, into a call to
    /// the safe wrapper in `xj_cmath`, such as `::xj_cmath::sqrt(x)`.
    ///
    /// The callee must be declared in an `extern` block with the C signature
    /// that `xj_cmath` wraps, and must not be defined by the crate itself.
    ///
    /// For functions with output parameters, such as `frexp(x, &raw mut e)`,
    /// the outputs become mutable borrows, as in `::xj_cmath::frexp(x, &mut e)`,
    /// when each one is a distinct place that can be borrowed safely.
    /// Otherwise the call uses the unsafe pointer-taking variant, as in
    /// `::xj_cmath::frexp_ptr(x, p)`.
    pub fn rewrite_libm_call_to_cmath(
        &self,
        symbols: &SymbolTable,
        expr: &Expr,
    ) -> Option<(Expr, Depth)> {
        let Expr::Call(call) = expr else {
            return None;
        };
        let Expr::Path(function) = &*call.func else {
            return None;
        };
        if function.qself.is_some() {
            return None;
        }
        let name = function.path.get_ident()?;
        let name_str = name.to_string();
        let (params, outputs, ret) = cmath_signature(&name_str)?;
        let sig = symbols.external_fn(&name_str)?;
        if call.args.len() != params.len() + outputs.len()
            || !sig_matches_cmath(sig, params, outputs, ret)
        {
            return None;
        }
        let args = &call.args;
        self.add_dep("xj_cmath");
        if outputs.is_empty() {
            return Some((
                syn::parse_quote! { ::xj_cmath::#name(#args) },
                Depth::Unlimited,
            ));
        }

        // Pass output locations as mutable references when we can borrow
        // each one, and otherwise pass the original pointers to the unsafe
        // `_ptr` variant.
        let arg_list = args.iter().collect::<Vec<_>>();
        let (inputs, out_args) = arg_list.split_at(params.len());
        let places = out_args
            .iter()
            .zip(outputs)
            .map(|(arg, &ty)| cmath_output_place(arg, ty, symbols))
            .collect::<Option<Vec<_>>>()
            .filter(|places| places_are_disjoint(places));
        let replacement = match places {
            Some(places) => {
                let inputs = inputs.iter();
                let places = places.iter();
                syn::parse_quote! { ::xj_cmath::#name(#(#inputs,)* #(&mut #places),*) }
            }
            None => {
                let ptr_name = syn::Ident::new(&format!("{name}_ptr"), name.span());
                syn::parse_quote! { ::xj_cmath::#ptr_name(#args) }
            }
        };
        Some((replacement, Depth::Unlimited))
    }

    /// Compare complete float or double representations without calling C's `memcmp`.
    /// Bit equality preserves distinctions between signed zeros and NaN payloads.
    pub fn rewrite_memcmp_float_comparison(
        &self,
        symbols: &SymbolTable,
        expr: &Expr,
    ) -> Option<(Expr, Depth)> {
        let Expr::Binary(binary) = expr else {
            return None;
        };
        if !matches!(binary.op, syn::BinOp::Eq(_) | syn::BinOp::Ne(_)) {
            return None;
        }
        let call = if is_zero_integer_expr(&binary.right) {
            expr_strip_parens(&binary.left)
        } else if is_zero_integer_expr(&binary.left) {
            expr_strip_parens(&binary.right)
        } else {
            return None;
        };
        let Expr::Call(call) = call else {
            return None;
        };
        let Expr::Path(function) = expr_strip_parens(&call.func) else {
            return None;
        };
        if function.qself.is_some() || !function.path.is_ident("memcmp") || call.args.len() != 3 {
            return None;
        }
        let width = float_size_of(&call.args[2])?;
        let bits = |place: &Expr| {
            let place = expr_strip_parens(place);
            // Parenthesize dereferences before applying the method.
            if matches!(place, Expr::Unary(_)) {
                syn::parse_quote! { (#place).to_bits() }
            } else {
                syn::parse_quote! { #place.to_bits() }
            }
        };
        let left: Expr = bits(memcmp_float_operand(&call.args[0], symbols, width)?);
        let right: Expr = bits(memcmp_float_operand(&call.args[1], symbols, width)?);
        let op = &binary.op;
        let replacement = syn::parse_quote! { #left #op #right };
        Some((replacement, Depth::Unlimited))
    }

    /// Turn writes to existing atomic places into sequentially consistent
    /// stores, and construct atomic values in aggregate initializers.
    pub fn rewrite_atomic_initialization(
        &self,
        symbols: &SymbolTable,
        expr: &Expr,
    ) -> Option<(Expr, Depth)> {
        match expr {
            Expr::Assign(assign) => {
                let place = atomic_assignment_place(&assign.left);
                let ty = symbols.type_of_expr(place)?;
                if atomic_kind(&ty).is_none() || is_atomic_constructor(&assign.right) {
                    return None;
                }

                let value = coerce_atomic_method_value(&ty, "store", &assign.right);
                let replacement: Expr = syn::parse_quote! {
                    #place.store(#value, ::core::sync::atomic::Ordering::SeqCst)
                };
                Some((replacement, Depth::Unlimited))
            }
            Expr::Struct(expr_struct) => {
                let struct_name = expr_struct.path.segments.last()?.ident.to_string();
                let field_types = symbols.fields.get(&struct_name)?;
                let mut replacement = expr_struct.clone();
                let mut changed = false;

                for field in &mut replacement.fields {
                    let syn::Member::Named(field_name) = &field.member else {
                        continue;
                    };
                    // A shorthand field already has the declared type; only explicit
                    // `field: value` initializers need construction.
                    let Some(field_ty) = field_types.get(&field_name.to_string()) else {
                        continue;
                    };
                    if field.colon_token.is_none()
                        || atomic_kind(field_ty).is_none()
                        || is_atomic_constructor(&field.expr)
                    {
                        continue;
                    }
                    field.expr = atomic_constructor(field_ty, &field.expr)?;
                    changed = true;
                }

                changed.then_some((Expr::Struct(replacement), Depth::Unlimited))
            }
            _ => None,
        }
    }

    /// Replace legacy core atomic intrinsics on guided atomic places with
    /// atomic methods.
    pub fn rewrite_atomic_intrinsic(
        &self,
        symbols: &SymbolTable,
        expr: &Expr,
    ) -> Option<(Expr, Depth)> {
        let Expr::Call(call) = expr else {
            return None;
        };
        let Expr::Path(function) = &*call.func else {
            return None;
        };
        let pointer = atomic_pointer_operand(call.args.first()?)?;
        let raw_addr = pointer.raw_addr;
        let receiver_ty = symbols.type_of_expr(&raw_addr.expr)?;
        let kind = atomic_kind(&receiver_ty)?;
        let (method, ordering, value_arity) = atomic_intrinsic_method(&function.path, kind)?;
        if call.args.len() != value_arity + 1 {
            return None;
        }

        let receiver = &raw_addr.expr;
        let values = call
            .args
            .iter()
            .skip(1)
            .map(|value| coerce_atomic_method_value(&receiver_ty, method, value))
            .collect::<Vec<_>>();
        let method = syn::Ident::new(method, function.path.span());
        let ordering = syn::Ident::new(ordering, function.path.span());
        let mut replacement: Expr = syn::parse_quote! {
            #receiver.#method(#(#values,)* ::core::sync::atomic::Ordering::#ordering)
        };
        if method != "store"
            && let Some(result_ty) = pointer.c_value_type
        {
            replacement = syn::parse_quote! { (#replacement) as #result_ty };
        }
        Some((replacement, Depth::Unlimited))
    }

    /// Rewrite `array[Nusize]` into `array[N]`. Array indexing already constrains
    /// an unsuffixed integer literal to `usize`, so the suffix is redundant.
    pub fn rewrite_usize_array_subscript_literal(
        &self,
        _symbols: &SymbolTable,
        expr: &Expr,
    ) -> Option<(Expr, Depth)> {
        let Expr::Index(index) = expr else {
            return None;
        };
        let Expr::Lit(index_lit) = &*index.index else {
            return None;
        };
        let syn::Lit::Int(int_lit) = &index_lit.lit else {
            return None;
        };
        if int_lit.suffix() != "usize" {
            return None;
        }

        let token = int_lit.to_string();
        let unsuffixed = token.strip_suffix(int_lit.suffix())?.trim_end_matches('_');
        let mut replacement = index.clone();
        let mut replacement_lit = index_lit.clone();
        replacement_lit.lit = syn::Lit::Int(LitInt::new(unsuffixed, int_lit.span()));
        replacement.index = Box::new(Expr::Lit(replacement_lit));

        Some((Expr::Index(replacement), Depth::Unlimited))
    }

    /// Replace C-style null-pointer field offsets with Rust's `offset_of!`.
    /// Handles both byte-pointer `offset_from` and raw-pointer provenance idioms.
    /// Keep up to two integer casts, dropping a repeated cast to the same type.
    pub fn rewrite_null_pointer_offsetof(
        &self,
        _symbols: &SymbolTable,
        expr: &Expr,
    ) -> Option<(Expr, Depth)> {
        let mut core = expr_strip_parens(expr);
        let mut casts = Vec::new();
        for _ in 0..2 {
            let Expr::Cast(cast) = core else {
                break;
            };
            if !is_integer_cast_type(&cast.ty) {
                return None;
            }
            casts.push(&*cast.ty);
            core = expr_strip_parens(&cast.expr);
        }
        let Expr::MethodCall(call) = core else {
            return None;
        };
        if call.turbofish.is_some() {
            return None;
        }
        let (struct_ty, fields) = if call.method == "offset_from" && call.args.len() == 1 {
            let Expr::Cast(field_cast) = expr_strip_parens(&call.receiver) else {
                return None;
            };
            let Expr::Cast(null_cast) = expr_strip_parens(&call.args[0]) else {
                return None;
            };
            if !is_byte_pointer_type(&field_cast.ty) || !is_byte_pointer_type(&null_cast.ty) {
                return None;
            }
            let (struct_ty, fields) = raw_null_field(&field_cast.expr)?;
            let other_ty = null_mut_type(&null_cast.expr)?;
            if struct_ty != other_ty {
                return None;
            }
            (struct_ty, fields)
        } else if call.method == "expose_provenance" && call.args.is_empty() {
            raw_null_field(&call.receiver)?
        } else {
            return None;
        };

        let mut replacement: Expr = syn::parse_quote! {
            ::core::mem::offset_of!(#struct_ty, #(#fields).*)
        };
        let mut previous_cast: Option<&Type> = None;
        for ty in casts.into_iter().rev() {
            if previous_cast.is_some_and(|previous| same_integer_type(previous, ty)) {
                continue;
            }
            replacement = syn::parse_quote! { #replacement as #ty };
            previous_cast = Some(ty);
        }
        Some((replacement, Depth::Unlimited))
    }

    /// Rewrite `(_ BINOP1 _) as Y CMP_BINOP3 (_ BINOP2 _) as Y`
    /// into    `(_ BINOP1 _)      CMP_BINOP3 (_ BINOP2 _)`
    pub fn rewrite_casted_literal_comparison(
        &self,
        _symbols: &SymbolTable,
        expr: &Expr,
    ) -> Option<(Expr, Depth)> {
        let Expr::Binary(outer_bin) = expr else {
            return None;
        };

        // Only strip the outer casts when the operator is a comparison; bitwise and
        // arithmetic ops can change value when the cast width differs.
        if !is_comparison_op(&outer_bin.op) {
            return None;
        }

        let Expr::Cast(left_outer) = &*outer_bin.left else {
            return None;
        };
        let Expr::Cast(right_outer) = &*outer_bin.right else {
            return None;
        };

        // Inner expressions must be comparison binary ops, guaranteeing they are bool.
        // To be soundly elided, the outer casts must not be lossy, and the inner
        // subexpressions must be of the same type.
        let Expr::Binary(left_inner_bin) = expr_strip_parens(&left_outer.expr) else {
            return None;
        };
        let Expr::Binary(right_inner_bin) = expr_strip_parens(&right_outer.expr) else {
            return None;
        };
        if !is_comparison_op(&left_inner_bin.op) || !is_comparison_op(&right_inner_bin.op) {
            return None;
        }

        // Both outer casts must target the same type Y.
        if left_outer.ty != right_outer.ty {
            return None;
        }

        let left_inner = &left_outer.expr;
        let right_inner = &right_outer.expr;
        let op3 = &outer_bin.op;

        let replacement: Expr = syn::parse_quote! {
            #left_inner #op3 #right_inner
        };

        Some((replacement, Depth::Unlimited))
    }

    /// Rewrite `print!("{:}", E as char)` in expression position into a direct
    /// stdout byte write, wrapped in a block so the expression keeps type `()`.
    pub fn rewrite_print_byte(&self, _symbols: &SymbolTable, expr: &Expr) -> Option<(Expr, Depth)> {
        let Expr::Macro(mac) = expr else {
            return None;
        };
        if !mac.attrs.is_empty() {
            return None;
        }
        let write = self.print_byte_replacement(&mac.mac)?;
        Some((syn::parse_quote! { { #write; } }, Depth::Unlimited))
    }

    /// Rewrite `print!("{:}", E as char);` into a direct stdout byte write.
    /// A trailing `print!` without a semicolon gains one, so the enclosing
    /// block keeps type `()`.
    pub fn rewrite_stmt_print_byte(
        &self,
        _symbols: &SymbolTable,
        stmt: &Stmt,
    ) -> Option<(Stmt, Depth)> {
        let (attrs, mac) = match stmt {
            Stmt::Macro(m) => (&m.attrs, &m.mac),
            Stmt::Expr(Expr::Macro(m), None) => (&m.attrs, &m.mac),
            _ => return None,
        };
        if !attrs.is_empty() {
            return None;
        }
        let write = self.print_byte_replacement(mac)?;
        Some((
            Stmt::Expr(write, Some(Default::default())),
            Depth::Unlimited,
        ))
    }

    fn print_byte_replacement(&self, mac: &syn::Macro) -> Option<Expr> {
        use syn::parse::Parser;

        if !mac.path.is_ident("print") {
            return None;
        }
        let args = Punctuated::<Expr, Comma>::parse_terminated
            .parse2(mac.tokens.clone())
            .ok()?;
        if args.len() != 2 {
            return None;
        }

        let Expr::Lit(fmt_lit) = &args[0] else {
            return None;
        };
        if !matches!(&fmt_lit.lit, syn::Lit::Str(s) if s.value() == "{:}") {
            return None;
        }

        let Expr::Cast(char_cast) = expr_strip_parens(&args[1]) else {
            return None;
        };
        if !matches!(&*char_cast.ty, Type::Path(path) if path.path.is_ident("char")) {
            return None;
        }

        let byte_expr = &char_cast.expr;
        self.with_cur_file_item_store(|item_store| {
            item_store.add_use(false, vec!["std".into(), "io".into()], "Write");
        });

        Some(syn::parse_quote! {
            ::std::io::stdout().write_all(&[#byte_expr as u8])
        })
    }

    pub fn rewrite_ctime_time(&self, symbols: &SymbolTable, expr: &Expr) -> Option<(Expr, Depth)> {
        let Expr::Call(call) = expr else {
            return None;
        };
        let Expr::Path(ref func) = *call.func else {
            return None;
        };
        if !func.path.is_ident("time") {
            return None;
        }
        if call.args.len() != 1 {
            return None;
        }

        let arg = &call.args[0];
        let arg_form: Expr = if let Some(name) = expr_ident_name(arg) {
            let arg_ty = symbols.get(&name)?;
            if matches!(arg_ty, Type::Ptr(_)) {
                Some(syn::parse_quote! { #arg.as_mut() })
            } else if matches!(arg_ty, Type::Reference(r) if r.mutability.is_some()) {
                Some(syn::parse_quote! { Some(#arg) })
            } else {
                None
            }
        } else {
            // The argument is not a simple identifier, so we won't know how to rewrite it.
            None
        }?;
        self.add_dep("xj_ctime");
        let replacement: Expr = syn::parse_quote! {
            xj_ctime::compat::time(#arg_form)
        };
        Some((replacement, Depth::Limited(0)))
    }

    /// Use the safe stat wrappers for paths backed by a ByteView.
    pub fn rewrite_stat_of_zstring(
        &self,
        symbols: &SymbolTable,
        expr: &Expr,
    ) -> Option<(Expr, Depth)> {
        let Expr::Call(call) = expr else {
            return None;
        };
        let Expr::Path(function) = &*call.func else {
            return None;
        };
        let segments = &function.path.segments;
        let name = match segments.len() {
            1 => &segments[0].ident,
            2 if segments[0].ident == "libc" => &segments[1].ident,
            _ => return None,
        };
        if !(name == "stat" || name == "lstat" || name == "stat64" || name == "lstat64")
            || call.args.len() != 2
        {
            return None;
        }

        if symbols.defined_fns.contains(&name.to_string()) {
            return None;
        }
        let path = self.coerce_byte_view_path_input(&call.args[0], symbols)?;

        let output = match expr_strip_parens(&call.args[1]) {
            Expr::Reference(reference) if reference.mutability.is_some() => {
                Expr::Reference(reference.clone())
            }
            Expr::RawAddr(raw) if matches!(raw.mutability, syn::PointerMutability::Mut(_)) => {
                // A named local is an addressable place that can be borrowed.
                let Expr::Path(place) = expr_strip_parens(&raw.expr) else {
                    return None;
                };
                place.path.get_ident()?;
                syn::parse_quote! { &mut #place }
            }
            _ => return None,
        };

        self.add_dep("xj_cstat");
        let replacement: Expr = syn::parse_quote! {
            xj_cstat::c::#name(#path, #output)
        };
        Some((replacement, Depth::Unlimited))
    }

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

    /// Rewrite `isatty()` calls on the three standard file descriptors to the
    /// safe, cross-platform [`atty`] equivalent.  `atty` only models standard
    /// streams, so calls with arbitrary descriptors deliberately remain FFI.
    pub fn rewrite_isatty_standard_stream(
        &self,
        _symbols: &SymbolTable,
        expr: &Expr,
    ) -> Option<(Expr, Depth)> {
        let Expr::Call(call) = expr else {
            return None;
        };
        let Expr::Path(func) = &*call.func else {
            return None;
        };
        if !func.path.is_ident("isatty") || call.args.len() != 1 {
            return None;
        }

        let stream = match expr_strip_casts(&call.args[0]) {
            Expr::Path(path) if path.path.is_ident("STDIN_FILENO") => "Stdin",
            Expr::Path(path) if path.path.is_ident("STDOUT_FILENO") => "Stdout",
            Expr::Path(path) if path.path.is_ident("STDERR_FILENO") => "Stderr",
            Expr::Lit(ExprLit {
                lit: syn::Lit::Int(fd),
                ..
            }) => match fd.base10_parse::<u8>().ok()? {
                0 => "Stdin",
                1 => "Stdout",
                2 => "Stderr",
                _ => return None,
            },
            _ => return None,
        };

        let stream_ident = syn::Ident::new(stream, call.args[0].span());
        self.add_dep("atty");
        let replacement: Expr = syn::parse_quote! {
            atty::is(atty::Stream::#stream_ident) as ::core::ffi::c_int
        };
        Some((replacement, Depth::Limited(0)))
    }

    /// Rewrite `::xj_cmath::isinf(e as f64) != 0` into `e.is_infinite()`, and similarly for `isnan`.
    /// Rewrite `::xj_cmath::isinf(e as f64) == 0` into `!e.is_infinite()`, and similarly for `isnan`.
    pub fn rewrite_isinf_isnan_comparisons(
        &self,
        _symbols: &SymbolTable,
        expr: &Expr,
    ) -> Option<(Expr, Depth)> {
        let Expr::Binary(bin) = expr else {
            return None;
        };
        let is_equality = match bin.op {
            syn::BinOp::Eq(_) => true,
            syn::BinOp::Ne(_) => false,
            _ => return None,
        };
        if !is_zero_integer_expr(&bin.right) {
            return None;
        }
        let Expr::Call(call) = &*bin.left else {
            return None;
        };
        let Expr::Path(func) = &*call.func else {
            return None;
        };
        if func.qself.is_some() || call.args.len() != 1 {
            return None;
        }
        let segments = &func.path.segments;
        if segments.len() != 2 || segments[0].ident != "xj_cmath" {
            return None;
        }
        let method = match segments[1].ident.to_string().as_str() {
            "isinf" | "isinff" => "is_infinite",
            "isnan" | "isnanf" => "is_nan",
            _ => return None,
        };
        let method_ident = syn::Ident::new(method, func.path.span());

        // Stripping casts is valid because (A) isinf/isnan (at least the versions from math.h)
        // cannot be given arguments of non-floating-point type, and (B) floating point casts
        // do not change the value's isinf/isnan-ness.
        let receiver = expr_strip_casts(&call.args[0]);

        // Build the method call as a syntax tree, rather than by splicing
        // tokens, so that a receiver like `-x` keeps its parentheses.
        let mut predicate: syn::ExprMethodCall = syn::parse_quote! { receiver.#method_ident() };
        predicate.receiver = Box::new(receiver.clone());
        let predicate = Expr::MethodCall(predicate);

        let replacement = if is_equality {
            Expr::Unary(ExprUnary {
                attrs: Vec::new(),
                op: syn::UnOp::Not(Default::default()),
                expr: Box::new(predicate),
            })
        } else {
            predicate
        };

        Some((replacement, Depth::Unlimited))
    }

    /// Rewrite `strstr(e1, e2)` into `xj_zstr::strstr_mut_ptr(e1, e2)` when
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

        let e1 = self.coerce_u8s(&call.args[0], symbols, true)?;
        let e2 = self.coerce_u8s(&call.args[1], symbols, false)?;

        self.add_dep("xj_zstr");

        let replacement: Expr = syn::parse_quote! {
            xj_zstr::strstr_mut_ptr(#e1, #e2)
        };

        Some((replacement, Depth::Unlimited))
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
        if let Some(decayed) = Self::peek_array_decay_coercion(first_arg, symbols) {
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
                return Some((replacement, Depth::Unlimited));
            }
        }

        None
    }

    /// Given `e.as_mut_ptr()` or `&raw mut e` (where `e` is array-typed), return `Some(e)`.
    fn peek_array_decay_coercion<'e>(
        mut expr: &'e Expr,
        symbols: &SymbolTable,
    ) -> Option<&'e Expr> {
        if let Expr::Cast(cast) = expr {
            if let syn::Type::Ptr(_) = *cast.ty {
                expr = &*cast.expr;
            }
        }

        match expr {
            Expr::MethodCall(method_call) => {
                let is_array_decay = method_call.args.is_empty()
                    && (method_call.method == "as_mut_ptr" || method_call.method == "as_ptr");
                if !is_array_decay {
                    return None;
                }
                Some(&method_call.receiver)
            }
            Expr::RawAddr(raw_addr) if is_array_typed(&raw_addr.expr, symbols) => {
                Some(&raw_addr.expr)
            }
            _ => None,
        }
    }

    /// Rewrite `(&t)[i]` into t[i]
    pub fn rewrite_decayed_array_redundant_borrow(
        &self,
        _symbols: &SymbolTable,
        expr: &Expr,
    ) -> Option<(Expr, Depth)> {
        let Expr::Index(index) = expr else {
            return None;
        };
        let inner = expr_strip_refs(&index.expr);
        if std::ptr::eq(inner, index.expr.as_ref()) {
            return None;
        }
        let new_index = syn::ExprIndex {
            attrs: index.attrs.clone(),
            expr: Box::new(inner.clone()),
            bracket_token: index.bracket_token,
            index: index.index.clone(),
        };
        Some((Expr::Index(new_index), Depth::Unlimited))
    }

    /// Rewrite `e1.as_mut_ptr()[e2]` into `e1[e2]`
    /// (it's an artifact of guidance).
    pub fn rewrite_decayed_array_subscript(
        &self,
        symbols: &SymbolTable,
        expr: &Expr,
    ) -> Option<(Expr, Depth)> {
        let Expr::Index(index) = expr else {
            return None;
        };
        if let Some(decayed) = Self::peek_array_decay_coercion(&index.expr, symbols) {
            let subscript = &index.index;
            let replacement: Expr = syn::parse_quote! {
                #decayed[#subscript]
            };
            Some((replacement, Depth::Unlimited))
        } else {
            None
        }
    }

    /// Rewrite `*e.as_ptr()/*e` into `e[0]`
    pub fn rewrite_decayed_array_deref(
        &self,
        symbols: &SymbolTable,
        expr: &Expr,
    ) -> Option<(Expr, Depth)> {
        let Expr::Unary(ExprUnary {
            op: syn::UnOp::Deref(_),
            expr,
            ..
        }) = expr
        else {
            return None;
        };
        if let Some(expr) = Self::peek_array_decay_coercion(expr, symbols) {
            let rewrite: Expr = syn::parse_quote! {
                #expr[0]
            };
            return Some((rewrite, Depth::Unlimited));
        } else if is_indexable_typed(expr, symbols) {
            let rewrite: Expr = syn::parse_quote! {
                #expr[0]
            };
            return Some((rewrite, Depth::Unlimited));
        }
        None
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

        let base = self.coerce_u8s(&method_call.receiver, symbols, false)?;
        let offset = as_usize(&method_call.args[0]);

        self.with_cur_file_item_store(|item_store| {
            item_store.add_use(false, vec!["std".into(), "io".into()], "Write");
        });

        let replacement: Expr = syn::parse_quote! {
            ::std::io::stdout().write_all(& #base[#offset..])
        };
        Some((replacement, Depth::Unlimited))
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
        Some((replacement, Depth::Unlimited))
    }

    /// Rewrite `memset(arr.as_mut_ptr(), val, len)`
    /// into `arr.as_u8_mut_slice()[..len as usize].fill(val)`
    ///     when `arr` can be coerced to a u8 slice, or
    /// into `cast_slice_mut(arr)[..len as usize].fill(val)`
    ///     when `arr` is a non-byte-sized slice or array.
    ///
    pub fn rewrite_memset_on_slice_or_array(
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
        if !func.path.is_ident("memset") {
            return None;
        }
        if call.args.len() != 3 {
            return None;
        }

        let arr_arg = expr_strip_casts(&call.args[0]);
        let val_arg = paren_if_cast(&call.args[1]);
        let len_arg = &call.args[2];

        let val_arg_as_u8: Expr = syn::parse_quote! {
            #val_arg.try_into().unwrap()
        };

        if let Some(coerced_arr) = self.coerce_u8s(arr_arg, symbols, true) {
            self.add_dep("xj_cstr");
            self.with_cur_file_item_store(|item_store| {
                item_store.add_use(true, vec!["xj_cstr".into()], "ByteSlice");
            });
            let replacement: Expr = syn::parse_quote! {
                #coerced_arr[..#len_arg as usize].fill(#val_arg_as_u8)
            };
            return Some((replacement, Depth::Unlimited));
        }

        if let Some(receiver) = extract_slice_ptr_base(arr_arg, symbols) {
            // If the type is not pod-compatible, we cannot use `bytemuck`.
            if !is_pod_compatible_expr(receiver, symbols) {
                return None;
            }

            self.add_dep("bytemuck");
            self.with_cur_file_item_store(|item_store| {
                item_store.add_use(true, vec!["bytemuck".into()], "cast_slice_mut");
            });
            let replacement: Expr = syn::parse_quote! {
                cast_slice_mut(&mut #receiver)[..#len_arg as usize].fill(#val_arg_as_u8)
            };
            return Some((replacement, Depth::Unlimited));
        }
        None
    }

    /// Rewrite code like
    ///   ```ignore
    ///     xj_str_from_ptr((if COND { THN } else { ELS }) as *const core::ffi::c_char)
    /// ```
    /// to distribute xj_str_from_ptr() over the if, so long as at least one of
    /// THN and ELS is a byte string literal with valid UTF-8 text.
    pub fn rewrite_cstr_ctor_over_if(
        &self,
        _symbols: &SymbolTable,
        expr: &Expr,
    ) -> Option<(Expr, Depth)> {
        let Expr::Call(str_from_ptr_call) = expr else {
            return None;
        };
        let Expr::Path(ref func) = *str_from_ptr_call.func else {
            return None;
        };
        if !func.path.is_ident("xj_str_from_ptr") || str_from_ptr_call.args.len() != 1 {
            return None;
        }

        let arg = &str_from_ptr_call.args[0];
        let Expr::Cast(cast) = expr_strip_parens(arg) else {
            return None;
        };
        if !matches!(*cast.ty, Type::Ptr(_)) {
            return None;
        }

        let Expr::If(if_expr) = expr_strip_parens(&cast.expr) else {
            return None;
        };
        let (_, else_box) = if_expr.else_branch.as_ref()?;

        let then_expr_full = get_block_lone_expr(&if_expr.then_branch)?;
        let else_expr_full = get_expr_block_lone_expr(else_box.as_ref())?;

        let mb_then_lit = coerce_str_of_cast_byte_str(then_expr_full);
        let mb_else_lit = coerce_str_of_cast_byte_str(else_expr_full);

        if mb_then_lit.is_none() && mb_else_lit.is_none() {
            return None;
        }

        let then_expr = if let Some(lit) = mb_then_lit {
            *lit
        } else {
            syn::parse_quote! { xj_str_from_ptr( #then_expr_full as *const u8 ) }
        };

        let else_expr = if let Some(lit) = mb_else_lit {
            *lit
        } else {
            syn::parse_quote! { xj_str_from_ptr( #else_expr_full as *const u8 ) }
        };

        let cond = &if_expr.cond;

        let replacement: Expr = syn::parse_quote! {
            if #cond { #then_expr } else { #else_expr }
        };

        Some((replacement, Depth::Unlimited))
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

        Some((Stmt::Expr(stripped.clone(), *semi), Depth::Unlimited))
    }

    /// Rewrite
    ///     `*s1.offset(s1.len().wrapping_sub(1 as size_t) as isize) = '\0' as u8;`
    ///  or `*s1.offset((s1.len() as ___ - 1 as ___) as isize) = '\0' as u8;`
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

    /// Rewrite `xj_str_from_ptr(&raw mut s as *mut u8 as *const c_char)` (for `s: ZString`)
    /// or `xj_str_from_ptr(s as *const c_char)` (for `s` a `ZString`, `&ZStr`, or `&mut ZStr`)
    /// into `s.to_string_lossy()`.
    pub fn rewrite_str_from_ptr_of_zstr(
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
        if !func.path.is_ident("xj_str_from_ptr") || call.args.len() != 1 {
            return None;
        }

        let place = zstr_place_of_c_str_arg(&call.args[0], symbols)?;
        let replacement: Expr = syn::parse_quote! { #place.to_string_lossy() };
        Some((replacement, Depth::Limited(0)))
    }

    /// Rewrite `strlen(s)` into `s.len()`, for the same forms of `s`
    /// accepted by [`Rewriter::rewrite_str_from_ptr_of_zstr`].
    /// Also recognize pointer decay through an explicit ZStr view. Keep a cast
    /// only when the C result type is not known to be `usize`.
    pub fn rewrite_strlen_of_zstr(
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
        if !func.path.is_ident("strlen") || call.args.len() != 1 {
            return None;
        }

        let mut arg = strlen_pointer_base(&call.args[0]);
        if let Expr::MethodCall(view) = arg {
            if view.args.is_empty()
                && matches!(
                    view.method.to_string().as_str(),
                    "as_bytes"
                        | "as_bytes_mut"
                        | "as_bytes_with_nul"
                        | "as_buffer"
                        | "as_buffer_mut"
                        | "as_c_str"
                )
            {
                arg = &view.receiver;
            }
        }
        let place = zstr_place_of_c_str_arg(arg, symbols)?;
        let replacement = strlen_result(syn::parse_quote! { #place.len() }, symbols);
        Some((replacement, Depth::Limited(0)))
    }

    /// Rewrite `strcmp(s, b"lit\0".as_ptr()) == 0` (or `!= 0`, with the operands
    /// of either the call or the comparison in either order) into
    /// `s.as_bytes() == b"lit"` when `s` is a `ZString`, `&ZString`, `&ZStr`, or `&mut ZStr`.
    /// `ZStr::as_bytes` stops at the first NUL, as does `strcmp`.
    /// Recognize explicit ZStr views, pointer decay, and C int result casts,
    /// including calls already lowered to the xj ByteView APIs.
    pub fn rewrite_strcmp_zstr_eq_literal(
        &self,
        symbols: &SymbolTable,
        expr: &Expr,
    ) -> Option<(Expr, Depth)> {
        let Expr::Binary(binary) = expr else {
            return None;
        };
        if !matches!(binary.op, syn::BinOp::Eq(_) | syn::BinOp::Ne(_)) {
            return None;
        }
        let call = if is_zero_integer_expr(&binary.right) {
            strcmp_result_base(&binary.left)
        } else if is_zero_integer_expr(&binary.left) {
            strcmp_result_base(&binary.right)
        } else {
            return None;
        };
        let Expr::Call(call) = call else {
            return None;
        };
        let Expr::Path(function) = expr_strip_parens(&call.func) else {
            return None;
        };
        if function.qself.is_some() || call.args.len() != 2 {
            return None;
        }
        let bounded = is_path_exactly_2(&function.path, "xj_cstr", "strcmp")
            || is_path_exactly_2(&function.path, "xj_zstr", "strcmp")
            || is_path_exactly_3(&function.path, "xj_zstr", "c", "strcmp");
        if !bounded
            && !function.path.is_ident("strcmp")
            && !is_path_exactly_2(&function.path, "libc", "strcmp")
        {
            return None;
        }
        if function.path.is_ident("strcmp")
            && (symbols.has("strcmp") || symbols.defined_fns.contains("strcmp"))
        {
            return None;
        }

        let (place, literal) = match (
            strcmp_byte_literal(&call.args[0]),
            strcmp_byte_literal(&call.args[1]),
        ) {
            (None, Some(literal)) => (&call.args[0], literal),
            (Some(literal), None) => (&call.args[1], literal),
            _ => return None,
        };
        let place = strcmp_zstr_place(place, symbols)?;

        // Bounded xj inputs have a virtual terminator at the view's end.
        // Raw C literals need a stored NUL to keep the read within bounds.
        let bytes = literal.value();
        let nul = match bytes.iter().position(|&b| b == 0) {
            Some(nul) => nul,
            None if bounded => bytes.len(),
            None => return None,
        };
        let literal = LitByteStr::new(&bytes[..nul], literal.span());

        let op = &binary.op;
        let replacement: Expr = syn::parse_quote! { #place.as_bytes() #op #literal };
        Some((replacement, Depth::Unlimited))
    }

    /// Rewrite the `char`-array initializer of a `ZString`-typed local, which c2rust
    /// emits as a (mistyped) byte array, into a `ZString` constructor:
    ///   * `let s: ZString = *b"ab\0\0";` => `let s: ZString = ZString::new(b"ab\0");`
    ///   * `let s: ZString = [0xf0u8, 0x9fu8];` => `let s: ZString = ZString::new([0xf0u8, 0x9fu8]);`
    ///   * `let s: ZString = [0u8, 0, 0, 0];` or `[0u8; 4]` => `let s: ZString = ZString::with_capacity(3);`
    ///
    /// One trailing NUL is dropped because `ZString` supplies its own terminator,
    /// so the `ZString`'s buffer is the same size as the C array.
    pub fn rewrite_zstring_local_init(
        &self,
        _symbols: &SymbolTable,
        stmt: &Stmt,
    ) -> Option<(Stmt, Depth)> {
        let Stmt::Local(local) = stmt else {
            return None;
        };
        let Pat::Type(pat_type) = &local.pat else {
            return None;
        };
        if !is_zstring_type(&pat_type.ty) {
            return None;
        }
        let localinit = local.init.as_ref()?;
        if localinit.diverge.is_some() {
            return None;
        }

        let ty = &pat_type.ty;
        let with_capacity = |n: usize| -> Expr {
            let n = proc_macro2::Literal::usize_unsuffixed(n);
            syn::parse_quote! { #ty::with_capacity(#n) }
        };
        let init: Expr = match expr_strip_parens(&localinit.expr) {
            Expr::Unary(ExprUnary {
                op: syn::UnOp::Deref(_),
                expr: inner,
                ..
            }) => {
                let Expr::Lit(ExprLit {
                    lit: syn::Lit::ByteStr(byte_str),
                    ..
                }) = &**inner
                else {
                    return None;
                };
                let bytes = bytes_strip_trailing_zero(byte_str.value());
                if bytes.iter().all(|&b| b == 0) {
                    with_capacity(bytes.len())
                } else {
                    let trimmed = LitByteStr::new(&bytes, byte_str.span());
                    syn::parse_quote! { #ty::new(#trimmed) }
                }
            }
            Expr::Array(array) => {
                let mut elems: Vec<Expr> = array.elems.iter().cloned().collect();
                if elems.last().is_some_and(is_zero_byte_expr) {
                    elems.pop();
                }
                if elems.iter().all(is_zero_byte_expr) {
                    with_capacity(elems.len())
                } else {
                    // c2rust suffixes only the first element; keep the element type
                    // pinned to `u8` so that `ZString::new`'s generic argument is inferable.
                    if let Some(Expr::Lit(ExprLit {
                        lit: syn::Lit::Int(first),
                        ..
                    })) = elems.first_mut()
                    {
                        if first.suffix().is_empty() {
                            *first = LitInt::new(&format!("{first}u8"), first.span());
                        }
                    }
                    syn::parse_quote! { #ty::new([#(#elems),*]) }
                }
            }
            Expr::Repeat(repeat) if is_zero_byte_expr(&repeat.expr) => {
                let n: usize = expr_get_int_literal(&repeat.len)?.base10_parse().ok()?;
                with_capacity(n.checked_sub(1)?)
            }
            _ => return None,
        };

        let mut replacement = local.clone();
        replacement.init.as_mut()?.expr = Box::new(init);
        Some((Stmt::Local(replacement), Depth::Unlimited))
    }

    /// Rewrite `strlen(b"text\0".as_ptr())` into `b"text".len()`, trimming
    /// at the first NUL. Keep a result cast only when needed by the C type.
    /// Other bounded inputs use `xj_zstr::strlen`, with a virtual end terminator.
    pub fn rewrite_strlen_of_slice(
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
        if !func.path.is_ident("strlen") {
            return None;
        }
        if call.args.len() != 1 {
            return None;
        }

        let arg = &call.args[0];
        if let Expr::Lit(ExprLit {
            lit: syn::Lit::ByteStr(literal),
            ..
        }) = strlen_pointer_base(arg)
        {
            let bytes = literal.value();
            // Without a NUL, strlen could read beyond this literal's allocation.
            let nul = bytes.iter().position(|&byte| byte == 0)?;
            let trimmed = LitByteStr::new(&bytes[..nul], literal.span());
            return Some((
                strlen_result(syn::parse_quote! { #trimmed.len() }, symbols),
                Depth::Limited(0),
            ));
        }
        let input = self.coerce_byte_view_input(arg, symbols)?;
        self.add_dep("xj_zstr");
        Some((
            strlen_result(syn::parse_quote! { ::xj_zstr::strlen(#input) }, symbols),
            Depth::Unlimited,
        ))
    }

    /// Replace C numeric parsers when their input is backed by a ByteView.
    /// Null `endptr` forms use the safe `_n` variants; other forms use the
    /// crate's unsafe `_ptr` variants in the original call's unsafe context.
    /// End-pointer forms require storage that survives the call statement.
    pub fn rewrite_atone_of_slice(
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
        let name = func.path.get_ident()?.to_string();
        let expected_args = match name.as_str() {
            "atof" | "atoi" | "atol" | "atoll" => 1,
            "strtod" | "strtof" => 2,
            "strtol" | "strtoul" | "strtoll" | "strtoull" => 3,
            _ => return None,
        };
        if call.args.len() != expected_args {
            return None;
        }

        if expected_args > 1
            && !is_null_pointer_expr(&call.args[1])
            && !crate::printf::live_storage_for_endptr(&call.args[0], symbols)
        {
            // Check before recovering the input: recovery may add imports and
            // dependencies, and a rejected rewrite must not add either.
            return None;
        }
        let input = self.coerce_byte_view_input(&call.args[0], symbols)?;
        self.add_dep("xj_atone");
        let replacement: Expr = if expected_args == 1 {
            let function = syn::Ident::new(&name, func.span());
            syn::parse_quote! { ::xj_atone::#function(#input) }
        } else {
            let endptr = &call.args[1];
            let base = call.args.iter().nth(2);
            if is_null_pointer_expr(endptr) {
                let function = syn::Ident::new(&format!("{name}_n"), func.span());
                if let Some(base) = base {
                    syn::parse_quote! { ::xj_atone::#function(#input, #base) }
                } else {
                    syn::parse_quote! { ::xj_atone::#function(#input) }
                }
            } else {
                let function = syn::Ident::new(&format!("{name}_ptr"), func.span());
                if let Some(base) = base {
                    syn::parse_quote! { ::xj_atone::#function(#input, #endptr, #base) }
                } else {
                    syn::parse_quote! { ::xj_atone::#function(#input, #endptr) }
                }
            }
        };
        Some((replacement, Depth::Unlimited))
    }

    /// Rewrite let-bound expressions when simpler forms exist.
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

        if atomic_kind(&pat_type.ty).is_some() && !is_atomic_constructor(&localinit.expr) {
            let mut replacement = local.clone();
            replacement.init.as_mut()?.expr =
                Box::new(atomic_constructor(&pat_type.ty, &localinit.expr)?);
            return Some((Stmt::Local(replacement), Depth::Unlimited));
        }

        if let Some(elt_ty) = type_of_slice_ref(&pat_type.ty) {
            if is_u8_type(elt_ty) {
                let init_expr = &localinit.expr;
                let coerced = self.coerce_u8s(init_expr, symbols, true)?;
                let replacement: Stmt = syn::parse_quote! {
                    let #pat_type = #coerced;
                };
                return Some((replacement, Depth::Unlimited));
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

    /// Coerce supported string-like inputs into `u8` slice expressions.
    ///
    /// Handles:
    /// - casted byte-string literals (trimming trailing `\0` when present),
    /// - string literals and `&str` identifiers (`x.as_bytes()`),
    /// - `x.as_mut_ptr()` on `u8` slices (`x.as_u8_slice()`),
    /// - pointer expressions (`CStr::from_ptr(x).to_bytes()`) when
    ///   `exclusive == false`.
    fn coerce_u8s(
        &self,
        mut expr: &Expr,
        symbols: &SymbolTable,
        exclusive: bool,
    ) -> Option<Box<Expr>> {
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
        if let Some(coerced) = self.coerce_slice_ptr_call(expr, symbols, exclusive) {
            return Some(coerced);
        }
        if exclusive {
            return None;
        }

        Some(Box::new(syn::parse_quote! {
            ::core::ffi::CStr::from_ptr(#expr as *const core::ffi::c_char).to_bytes()
        }))
    }

    /// Convert `x.as_mut_ptr()` on a `u8` slice expression into `x.as_u8_slice()`.
    fn coerce_slice_ptr_call(
        &self,
        expr: &Expr,
        symbols: &SymbolTable,
        exclusive: bool,
    ) -> Option<Box<Expr>> {
        let receiver = extract_slice_ptr_base(expr, symbols)?;
        if !is_u8_or_i8_sliceable_expr(receiver, symbols) {
            return None;
        }

        self.add_dep("xj_cstr");
        self.with_cur_file_item_store(|item_store| {
            item_store.add_use(true, vec!["xj_cstr".into()], "ByteSlice");
        });
        let method = syn::Ident::new(
            if exclusive {
                "as_mut_u8_slice"
            } else {
                "as_u8_slice"
            },
            proc_macro2::Span::call_site(),
        );
        let coerced: Expr = syn::parse_quote! {
            #receiver.#method()
        };
        Some(Box::new(coerced))
    }
}

pub(crate) fn is_atomic_constructor(expr: &Expr) -> bool {
    let Expr::Call(call) = expr_strip_parens(expr) else {
        return false;
    };
    let Expr::Path(function) = &*call.func else {
        return false;
    };
    let mut segments = function.path.segments.iter().rev();
    matches!(
        (segments.next(), segments.next()),
        (Some(new), Some(atomic))
            if new.ident == "new"
                && atomic_kind_from_name(&atomic.ident.to_string()).is_some()
    )
}

pub(crate) fn atomic_constructor(ty: &Type, value: &Expr) -> Option<Expr> {
    atomic_kind(ty)?;
    let Type::Path(type_path) = ty else {
        return None;
    };
    let mut constructor_path = type_path.path.clone();
    if let Some(last) = constructor_path.segments.last_mut()
        && let syn::PathArguments::AngleBracketed(arguments) = &mut last.arguments
    {
        arguments.colon2_token = Some(Default::default());
    }
    let value = coerce_pointer_sized_atomic_value(ty, value);
    Some(syn::parse_quote! { #constructor_path::new(#value) })
}

fn coerce_atomic_method_value(ty: &Type, method: &str, value: &Expr) -> Expr {
    if atomic_kind(ty) == Some(AtomicKind::Pointer) && method.starts_with("fetch_") {
        return syn::parse_quote! { (#value) as usize };
    }
    coerce_pointer_sized_atomic_value(ty, value)
}

fn coerce_pointer_sized_atomic_value(ty: &Type, value: &Expr) -> Expr {
    let Type::Path(type_path) = ty else {
        return value.clone();
    };
    let Some(segment) = type_path.path.segments.last() else {
        return value.clone();
    };
    match segment.ident.to_string().as_str() {
        "AtomicIsize" => syn::parse_quote! { (#value) as isize },
        "AtomicUsize" => syn::parse_quote! { (#value) as usize },
        _ => value.clone(),
    }
}

fn atomic_assignment_place(expr: &Expr) -> &Expr {
    let expr = expr_strip_parens(expr);
    let Expr::Unary(unary) = expr else {
        return expr;
    };
    if !matches!(unary.op, syn::UnOp::Deref(_)) {
        return expr;
    }
    let Some(raw_addr) = raw_addr_through_cast(&unary.expr) else {
        return expr;
    };
    &raw_addr.expr
}

/// Recognize a raw address directly or through the pointer cast emitted when
/// an immutable Rust static has a mutable C pointer type.
fn raw_addr_through_cast(expr: &Expr) -> Option<&syn::ExprRawAddr> {
    atomic_pointer_operand(expr).map(|pointer| pointer.raw_addr)
}

struct AtomicPointerOperand<'a> {
    raw_addr: &'a syn::ExprRawAddr,
    c_value_type: Option<&'a Type>,
}

/// Recognize an atomic intrinsic pointer and retain the C pointee type from
/// the explicit cast emitted by C2Rust.  Rust pointer-sized atomic methods
/// return `usize`/`isize`, which are not necessarily the same Rust type as the
/// C typedef used at the intrinsic call site.
fn atomic_pointer_operand(expr: &Expr) -> Option<AtomicPointerOperand<'_>> {
    match expr_strip_parens(expr) {
        Expr::RawAddr(raw_addr) => Some(AtomicPointerOperand {
            raw_addr,
            c_value_type: None,
        }),
        Expr::Cast(cast) => match expr_strip_parens(&cast.expr) {
            Expr::RawAddr(raw_addr) => {
                let Type::Ptr(pointer) = &*cast.ty else {
                    return None;
                };
                let c_value_type = match &*pointer.elem {
                    Type::Path(path)
                        if path.qself.is_none()
                            && path.path.segments.len() == 1
                            && c_atomic_rust_name(&path.path.segments[0].ident.to_string())
                                .is_some() =>
                    {
                        Some(&*pointer.elem)
                    }
                    _ => None,
                };
                Some(AtomicPointerOperand {
                    raw_addr,
                    c_value_type,
                })
            }
            _ => None,
        },
        _ => None,
    }
}

/// Return `(method, Ordering variant, non-receiver argument count)`.
fn atomic_intrinsic_method(
    path: &Path,
    kind: AtomicKind,
) -> Option<(&'static str, &'static str, usize)> {
    let segments = path
        .segments
        .iter()
        .map(|segment| segment.ident.to_string())
        .collect::<Vec<_>>();
    if segments.len() != 3 || segments[0] != "core" || segments[1] != "intrinsics" {
        return None;
    }
    let name = segments[2].strip_prefix("atomic_")?;
    let integer_operations = [
        ("store", "store", 1),
        ("load", "load", 0),
        ("xadd", "fetch_add", 1),
        ("xsub", "fetch_sub", 1),
        ("and", "fetch_and", 1),
        ("or", "fetch_or", 1),
        ("xor", "fetch_xor", 1),
        ("nand", "fetch_nand", 1),
        ("xchg", "swap", 1),
    ];
    let bool_operations = [
        ("store", "store", 1),
        ("load", "load", 0),
        ("and", "fetch_and", 1),
        ("or", "fetch_or", 1),
        ("xor", "fetch_xor", 1),
        ("nand", "fetch_nand", 1),
        ("xchg", "swap", 1),
    ];
    let pointer_operations = [
        ("store", "store", 1),
        ("load", "load", 0),
        ("xadd", "fetch_ptr_add", 1),
        ("xsub", "fetch_ptr_sub", 1),
        ("and", "fetch_and", 1),
        ("or", "fetch_or", 1),
        ("xor", "fetch_xor", 1),
        ("xchg", "swap", 1),
    ];
    let orderings = [
        ("unordered", "Relaxed"),
        ("relaxed", "Relaxed"),
        ("acquire", "Acquire"),
        ("release", "Release"),
        ("acqrel", "AcqRel"),
        ("seqcst", "SeqCst"),
    ];

    let operations: &[(&str, &str, usize)] = match kind {
        AtomicKind::Bool => &bool_operations,
        AtomicKind::Integer => &integer_operations,
        AtomicKind::Pointer => &pointer_operations,
    };
    for &(operation, method, arity) in operations {
        let Some(suffix) = name
            .strip_prefix(operation)
            .and_then(|rest| rest.strip_prefix('_'))
        else {
            continue;
        };
        if let Some((_, ordering)) = orderings.iter().find(|(name, _)| *name == suffix) {
            return Some((method, ordering, arity));
        }
    }
    None
}

fn as_usize(expr: &Expr) -> Box<Expr> {
    Box::new(syn::parse_quote! { #expr as usize })
}

fn as_u64(expr: &Expr) -> Box<Expr> {
    Box::new(syn::parse_quote! { #expr as u64 })
}

fn strip_casts(expr: &Expr) -> &Expr {
    let mut inner = expr;
    while let Expr::Cast(cast) = inner {
        inner = &cast.expr;
    }
    inner
}

fn get_litbytestr(expr: &Expr) -> Option<LitByteStr> {
    let inner = strip_casts(expr);

    if let Expr::MethodCall(call) = inner {
        if call.method == "as_ptr" {
            return get_litbytestr(&call.receiver);
        }
    };

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

/// If `expr` is a casted `b"...\0"` literal, strip the trailing NUL
/// and return a byte string literal.
/// Returns `None` if `expr` is not a casted byte string literal with an optional trailing NUL.
fn coerce_cast_byte_str(expr: &Expr) -> Option<Box<Expr>> {
    let byte_str: LitByteStr = get_litbytestr(expr)?;
    let bytes_sans_zero = bytes_strip_trailing_zero(byte_str.value());
    let trimmed = syn::LitByteStr::new(&bytes_sans_zero, byte_str.span());
    Some(Box::new(syn::parse_quote! { #trimmed }))
}

/// If `expr` is a casted `b"...\0"` literal, strip the trailing NUL
/// and return a plain (non-byte) string literal, if the byte string is valid UTF-8.
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

fn is_null_pointer_expr(expr: &Expr) -> bool {
    match expr_strip_parens(expr) {
        Expr::Cast(cast) => is_null_pointer_expr(&cast.expr),
        Expr::Lit(lit) => {
            matches!(&lit.lit, syn::Lit::Int(value) if value.base10_parse::<u64>().ok() == Some(0))
        }
        Expr::Call(call) if call.args.is_empty() => {
            let Expr::Path(function) = &*call.func else {
                return false;
            };
            let mut segments = function.path.segments.iter().rev();
            let Some(last) = segments.next() else {
                return false;
            };
            matches!(last.ident.to_string().as_str(), "null" | "null_mut")
                && segments
                    .next()
                    .is_some_and(|segment| segment.ident == "ptr")
        }
        _ => false,
    }
}

pub(crate) fn is_atone_byte_slice_expr(expr: &Expr, symbols: &SymbolTable) -> bool {
    if let Expr::Index(index) = expr_strip_parens(expr) {
        return matches!(&*index.index, Expr::Range(_))
            && is_atone_byte_slice_expr(&index.expr, symbols);
    }
    let Some(ty) = symbols.type_of_expr(expr) else {
        return false;
    };
    is_u8_or_i8_sliceable_type(&ty) || is_byte_vec_type(&ty)
}

fn is_byte_vec_type(ty: &Type) -> bool {
    match ty {
        Type::Reference(reference) => is_byte_vec_type(&reference.elem),
        Type::Path(path) => {
            let Some(segment) = path.path.segments.last() else {
                return false;
            };
            if segment.ident != "Vec" {
                return false;
            }
            let syn::PathArguments::AngleBracketed(args) = &segment.arguments else {
                return false;
            };
            matches!(args.args.first(), Some(syn::GenericArgument::Type(elt)) if is_u8_or_i8_type(elt))
        }
        _ => false,
    }
}

fn extract_slice_ptr_base<'e>(expr: &'e Expr, symbols: &SymbolTable) -> Option<&'e Expr> {
    match expr {
        Expr::Paren(p) => extract_slice_ptr_base(&p.expr, symbols),
        Expr::Group(g) => extract_slice_ptr_base(&g.expr, symbols),
        Expr::Cast(c) if matches!(&*c.ty, Type::Ptr(_) | Type::Infer(_)) => {
            extract_slice_ptr_base(&c.expr, symbols)
        }
        Expr::MethodCall(call) if call.method == "cast" && call.args.is_empty() => {
            extract_slice_ptr_base(&call.receiver, symbols)
        }
        Expr::MethodCall(call) if call.method == "as_mut_ptr" && call.args.is_empty() => {
            Some(&call.receiver)
        }
        Expr::RawAddr(raw_addr)
            if matches!(symbols.type_of_expr(&raw_addr.expr), Some(Type::Array(_))) =>
        {
            Some(&raw_addr.expr)
        }
        _ => None,
    }
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

fn is_pod_compatible_expr(expr: &Expr, symbols: &SymbolTable) -> bool {
    matches!(expr_ident_type(expr, symbols), Some(ty) if is_pod_compatible_type(ty))
}

fn is_pod_compatible_type(ty: &syn::Type) -> bool {
    match ty {
        syn::Type::Path(path) => {
            // TODO: consume guidance to refine this check.
            // TODO: track type environment?
            let Some(type_name) = path.path.segments.last().map(|seg| seg.ident.to_string()) else {
                return false;
            };

            matches!(
                type_name.as_str(),
                "u8" | "i8"
                    | "u16"
                    | "i16"
                    | "u32"
                    | "i32"
                    | "u64"
                    | "i64"
                    | "u128"
                    | "i128"
                    | "f32"
                    | "f64"
            )
        }
        syn::Type::Reference(r) => is_pod_compatible_type(&r.elem),
        syn::Type::Array(array) => is_pod_compatible_type(&array.elem),
        syn::Type::Slice(slice) => is_pod_compatible_type(&slice.elem),
        _ => false,
    }
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

fn is_comparison_op(op: &syn::BinOp) -> bool {
    matches!(
        op,
        syn::BinOp::Eq(_)
            | syn::BinOp::Ne(_)
            | syn::BinOp::Lt(_)
            | syn::BinOp::Le(_)
            | syn::BinOp::Gt(_)
            | syn::BinOp::Ge(_)
    )
}

fn is_bool_expr(expr: &Expr, symbols: &SymbolTable) -> bool {
    let is_bool_type = |ty: &Type| matches!(ty, Type::Path(path) if path.qself.is_none() && path.path.is_ident("bool"));
    match expr_strip_parens(expr) {
        Expr::Lit(lit) => matches!(lit.lit, syn::Lit::Bool(_)),
        Expr::Binary(binary) => {
            is_comparison_op(&binary.op)
                || matches!(binary.op, syn::BinOp::And(_) | syn::BinOp::Or(_))
                || (matches!(
                    binary.op,
                    syn::BinOp::BitAnd(_) | syn::BinOp::BitOr(_) | syn::BinOp::BitXor(_)
                ) && is_bool_expr(&binary.left, symbols)
                    && is_bool_expr(&binary.right, symbols))
        }
        Expr::Unary(unary) if matches!(unary.op, syn::UnOp::Not(_)) => {
            is_bool_expr(&unary.expr, symbols)
        }
        Expr::Cast(cast) => is_bool_type(&cast.ty),
        expr => symbols
            .type_of_expr(expr)
            .as_ref()
            .is_some_and(is_bool_type),
    }
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

fn is_array_typed(expr: &Expr, symbols: &SymbolTable) -> bool {
    matches!(expr_ident_type(expr, symbols), Some(ty) if is_array_type(ty))
}

fn is_indexable_typed(expr: &Expr, symbols: &SymbolTable) -> bool {
    matches!(
        expr_ident_type(expr, symbols),
        Some(ty) if sliceable_type_elt_is(ty, |_| true)
    )
}

fn is_array_type(ty: &syn::Type) -> bool {
    matches!(ty, syn::Type::Array(_))
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

fn is_ref_str_type(ty: &syn::Type) -> bool {
    let syn::Type::Reference(reference) = ty else {
        return false;
    };
    matches!(&*reference.elem, syn::Type::Path(path) if path.path.is_ident("str"))
}

fn is_string_type(ty: &syn::Type) -> bool {
    matches!(ty, syn::Type::Path(path) if path.path.segments.last().is_some_and(|segment| segment.ident == "String"))
}

/// Strip parentheses, pointer casts, and pointer decay without discarding any
/// argument expressions that would otherwise be evaluated.
fn strlen_pointer_base(mut expr: &Expr) -> &Expr {
    loop {
        expr = expr_strip_parens(expr);
        match expr {
            Expr::Cast(cast) if matches!(&*cast.ty, Type::Ptr(_)) => expr = &cast.expr,
            Expr::MethodCall(call)
                if call.args.is_empty()
                    && matches!(
                        call.method.to_string().as_str(),
                        "as_ptr" | "as_mut_ptr" | "cast"
                    ) =>
            {
                expr = &call.receiver;
            }
            _ => return expr,
        }
    }
}

/// Remove only result casts that preserve a C int's zero/nonzero value.
fn strcmp_result_base(mut expr: &Expr) -> &Expr {
    loop {
        expr = expr_strip_parens(expr);
        match expr {
            Expr::Cast(cast) if c_scalar_of_type(&cast.ty) == Some(CScalar::Int) => {
                expr = &cast.expr;
            }
            _ => return expr,
        }
    }
}

/// Recover the literal without dropping evaluated pointer-method arguments.
fn strcmp_byte_literal(expr: &Expr) -> Option<LitByteStr> {
    match strlen_pointer_base(expr) {
        Expr::Reference(reference) => strcmp_byte_literal(&reference.expr),
        Expr::Unary(unary) if matches!(unary.op, syn::UnOp::Deref(_)) => {
            strcmp_byte_literal(&unary.expr)
        }
        Expr::Lit(lit) => match &lit.lit {
            syn::Lit::ByteStr(bytes) => Some(bytes.clone()),
            syn::Lit::Str(text) => Some(LitByteStr::new(text.value().as_bytes(), text.span())),
            syn::Lit::CStr(text) => Some(LitByteStr::new(
                text.value().as_bytes_with_nul(),
                text.span(),
            )),
            _ => None,
        },
        _ => None,
    }
}

/// Recover a guided string only through views of its complete effective contents.
/// Sub-slices and arbitrary method calls retain their original comparisons.
fn strcmp_zstr_place<'a>(mut expr: &'a Expr, symbols: &SymbolTable) -> Option<&'a Expr> {
    loop {
        expr = strlen_pointer_base(expr);
        match expr {
            Expr::Reference(reference) => expr = &reference.expr,
            Expr::RawAddr(_) => return zstr_place_of_c_str_arg(expr, symbols),
            Expr::MethodCall(view)
                if view.args.is_empty()
                    && matches!(
                        view.method.to_string().as_str(),
                        "as_bytes"
                            | "as_bytes_mut"
                            | "as_bytes_with_nul"
                            | "as_buffer"
                            | "as_buffer_mut"
                            | "as_c_str"
                            | "as_z_str"
                            | "as_z_str_mut"
                            | "view_bytes"
                    ) =>
            {
                expr = &view.receiver;
            }
            _ => break,
        }
    }
    // Restrict to places that can be method receivers without parenthesization.
    if !matches!(expr, Expr::Path(_) | Expr::Field(_) | Expr::Index(_)) {
        return None;
    }
    is_zstr_like_type(&symbols.type_of_expr(expr)?).then_some(expr)
}

/// Preserve strlen's declared return type while avoiding a redundant usize cast.
fn strlen_result(length: Expr, symbols: &SymbolTable) -> Expr {
    let result_ty = symbols
        .foreign_fns
        .get("strlen")
        .and_then(|signature| match &signature.output {
            syn::ReturnType::Type(_, ty) => Some((**ty).clone()),
            _ => None,
        })
        .unwrap_or_else(|| syn::parse_quote! { size_t });
    usize_result(length, &result_ty, symbols)
}

fn is_usize_type<'a>(mut ty: &'a Type, symbols: &'a SymbolTable) -> bool {
    // Bound alias traversal to also handle recursive or unresolved aliases.
    for _ in 0..=symbols.type_aliases.len() {
        match ty {
            Type::Paren(paren) => ty = &paren.elem,
            Type::Group(group) => ty = &group.elem,
            Type::Path(path) if path.qself.is_none() => {
                if path.path.is_ident("usize") {
                    return true;
                }
                let Some(ident) = path.path.get_ident() else {
                    break;
                };
                let Some(alias) = symbols
                    .type_aliases
                    .get(&ident.to_string())
                    .and_then(Option::as_ref)
                else {
                    break;
                };
                ty = alias;
            }
            _ => break,
        }
    }
    false
}

/// Keep the declared result type when it differs from the bounded API's usize.
fn usize_result(value: Expr, ty: &Type, symbols: &SymbolTable) -> Expr {
    if is_usize_type(ty, symbols) {
        value
    } else {
        syn::parse_quote! { #value as #ty }
    }
}

/// Avoid redundant conversions for counts already known to be usize.
fn usize_argument(expr: &Expr, symbols: &SymbolTable) -> Expr {
    let expr = expr_strip_parens(expr);
    if let Expr::Cast(cast) = expr
        && is_usize_type(&cast.ty, symbols)
        && let Expr::Lit(lit) = expr_strip_parens(&cast.expr)
        && let syn::Lit::Int(int) = &lit.lit
        && matches!(int.suffix(), "" | "usize")
    {
        return Expr::Lit(lit.clone());
    }
    if matches!(expr, Expr::Lit(lit) if matches!(&lit.lit, syn::Lit::Int(int) if matches!(int.suffix(), "" | "usize")))
        || crate::printf::expr_type(expr, symbols).is_some_and(|ty| is_usize_type(&ty, symbols))
    {
        expr.clone()
    } else {
        syn::parse_quote! { (#expr) as usize }
    }
}

/// Given a C string argument `s` or `&raw mut s` (each perhaps cast to a pointer),
/// return `s` if it names a `ZString`, `&ZString`, `&ZStr`, or `&mut ZStr`.
/// Raw addresses are accepted only of an owned `ZString`, because taking the
/// address of a reference would point at the reference, not the string.
fn zstr_place_of_c_str_arg<'e>(arg: &'e Expr, symbols: &SymbolTable) -> Option<&'e Expr> {
    let arg = expr_strip_parens(expr_strip_casts(expr_strip_parens(arg)));
    let (place, owned_only) = match arg {
        Expr::RawAddr(raw) => (expr_strip_parens(&raw.expr), true),
        other => (other, false),
    };
    // Restrict to places that can be method receivers without parenthesization.
    if !matches!(place, Expr::Path(_) | Expr::Field(_)) {
        return None;
    }
    let ty = symbols.type_of_expr(place)?;
    let is_match = if owned_only {
        is_zstring_type(&ty)
    } else {
        is_zstr_like_type(&ty)
    };
    is_match.then_some(place)
}

fn is_zstring_type(ty: &Type) -> bool {
    is_xj_zstr_type(ty, "ZString")
}

/// Returns `true` for `ZString`, `&ZString`, `&ZStr`, and `&mut ZStr` (bare or `xj_zstr::`-qualified).
fn is_zstr_like_type(ty: &Type) -> bool {
    match ty {
        Type::Reference(reference) => {
            is_xj_zstr_type(&reference.elem, "ZStr") || is_xj_zstr_type(&reference.elem, "ZString")
        }
        _ => is_zstring_type(ty),
    }
}

/// Returns `true` for `name` or `xj_zstr::name`.
fn is_xj_zstr_type(ty: &Type, name: &str) -> bool {
    let Type::Path(path) = ty else {
        return false;
    };
    if path.qself.is_some() {
        return false;
    }
    let segments = &path.path.segments;
    let correct_name = match segments.len() {
        1 => segments[0].ident == name,
        2 => segments[0].ident == "xj_zstr" && segments[1].ident == name,
        _ => false,
    };
    correct_name
        && segments
            .iter()
            .all(|segment| matches!(segment.arguments, syn::PathArguments::None))
}

fn is_c_float_type(ty: &syn::Type) -> bool {
    matches!(ty, syn::Type::Path(path) if path.path.segments.last().is_some_and(|segment| segment.ident == "c_float"))
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum FloatWidth {
    F32,
    F64,
}

fn float_width(ty: &Type) -> Option<FloatWidth> {
    match ty {
        Type::Paren(paren) => float_width(&paren.elem),
        Type::Group(group) => float_width(&group.elem),
        Type::Path(path) if path.qself.is_none() => {
            if path.path.is_ident("f32") {
                return Some(FloatWidth::F32);
            }
            if path.path.is_ident("f64") {
                return Some(FloatWidth::F64);
            }
            let segment = path.path.segments.last()?;
            if !matches!(segment.arguments, syn::PathArguments::None) {
                return None;
            }
            match segment.ident.to_string().as_str() {
                "c_float" => Some(FloatWidth::F32),
                "c_double" => Some(FloatWidth::F64),
                _ => None,
            }
        }
        _ => None,
    }
}

/// A C scalar type appearing in the signature of a `math.h` function.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
enum CScalar {
    Float,
    Double,
    Int,
    Long,
    LongLong,
}

/// The signature of the C math function `name`, for functions that
/// `xj_cmath` wraps safely: the types of its value parameters, the pointee
/// types of the output parameters that follow them, and its return type
/// (`None` for `void`).
///
/// Omits `lgamma`, `lgammaf`, `gamma`, and `gammaf`, whose `xj_cmath`
/// versions do not update C's global `signgam`.
fn cmath_signature(
    name: &str,
) -> Option<(&'static [CScalar], &'static [CScalar], Option<CScalar>)> {
    use CScalar::*;
    if let Some((params, ret)) = cmath_scalar_signature(name) {
        return Some((params, &[], Some(ret)));
    }
    let sig: (&[CScalar], &[CScalar], Option<CScalar>) = match name {
        "frexp" => (&[Double], &[Int], Some(Double)),
        "frexpf" => (&[Float], &[Int], Some(Float)),
        "modf" => (&[Double], &[Double], Some(Double)),
        "modff" => (&[Float], &[Float], Some(Float)),
        "remquo" => (&[Double, Double], &[Int], Some(Double)),
        "remquof" => (&[Float, Float], &[Int], Some(Float)),
        "sincos" => (&[Double], &[Double, Double], None),
        "sincosf" => (&[Float], &[Float, Float], None),
        "lgamma_r" => (&[Double], &[Int], Some(Double)),
        "lgammaf_r" => (&[Float], &[Int], Some(Float)),
        _ => return None,
    };
    Some(sig)
}

/// The parameter and return types of the C math functions whose
/// `xj_cmath` wrappers take and return only scalars.
fn cmath_scalar_signature(name: &str) -> Option<(&'static [CScalar], CScalar)> {
    use CScalar::*;
    const D: &[CScalar] = &[Double];
    const F: &[CScalar] = &[Float];
    const DD: &[CScalar] = &[Double, Double];
    const FF: &[CScalar] = &[Float, Float];
    const DDD: &[CScalar] = &[Double, Double, Double];
    const FFF: &[CScalar] = &[Float, Float, Float];
    const DI: &[CScalar] = &[Double, Int];
    const FI: &[CScalar] = &[Float, Int];
    const DL: &[CScalar] = &[Double, Long];
    const FL: &[CScalar] = &[Float, Long];
    const ID: &[CScalar] = &[Int, Double];
    const IF: &[CScalar] = &[Int, Float];
    let sig = match name {
        "acos" | "asin" | "atan" | "cos" | "sin" | "tan" | "acosh" | "asinh" | "atanh" | "cosh"
        | "sinh" | "tanh" | "exp" | "exp2" | "expm1" | "log" | "log10" | "log1p" | "log2"
        | "logb" | "cbrt" | "fabs" | "sqrt" | "erf" | "erfc" | "tgamma" | "ceil" | "floor"
        | "nearbyint" | "rint" | "round" | "trunc" | "significand" | "j0" | "j1" | "y0" | "y1" => {
            (D, Double)
        }
        "acosf" | "asinf" | "atanf" | "cosf" | "sinf" | "tanf" | "acoshf" | "asinhf" | "atanhf"
        | "coshf" | "sinhf" | "tanhf" | "expf" | "exp2f" | "expm1f" | "logf" | "log10f"
        | "log1pf" | "log2f" | "logbf" | "cbrtf" | "fabsf" | "sqrtf" | "erff" | "erfcf"
        | "tgammaf" | "ceilf" | "floorf" | "nearbyintf" | "rintf" | "roundf" | "truncf"
        | "significandf" | "j0f" | "j1f" | "y0f" | "y1f" => (F, Float),
        "atan2" | "hypot" | "pow" | "fmod" | "remainder" | "copysign" | "nextafter" | "fdim"
        | "fmax" | "fmin" | "drem" | "scalb" => (DD, Double),
        "atan2f" | "hypotf" | "powf" | "fmodf" | "remainderf" | "copysignf" | "nextafterf"
        | "fdimf" | "fmaxf" | "fminf" | "dremf" | "scalbf" => (FF, Float),
        "fma" => (DDD, Double),
        "fmaf" => (FFF, Float),
        "ilogb" | "finite" => (D, Int),
        "ilogbf" | "finitef" => (F, Int),
        "ldexp" | "scalbn" => (DI, Double),
        "ldexpf" | "scalbnf" => (FI, Float),
        "scalbln" => (DL, Double),
        "scalblnf" => (FL, Float),
        "lrint" | "lround" => (D, Long),
        "lrintf" | "lroundf" => (F, Long),
        "llrint" | "llround" => (D, LongLong),
        "llrintf" | "llroundf" => (F, LongLong),
        "jn" | "yn" => (ID, Double),
        "jnf" | "ynf" => (IF, Float),
        _ => return None,
    };
    Some(sig)
}

/// Whether an `extern` declaration has exactly the C signature
/// `(params..., *mut outputs...) -> ret`.
fn sig_matches_cmath(
    sig: &syn::Signature,
    params: &[CScalar],
    outputs: &[CScalar],
    ret: Option<CScalar>,
) -> bool {
    if sig.variadic.is_some()
        || !sig.generics.params.is_empty()
        || sig.inputs.len() != params.len() + outputs.len()
    {
        return false;
    }
    let ret_matches = match (&sig.output, ret) {
        (syn::ReturnType::Default, None) => true,
        (syn::ReturnType::Type(_, ret_ty), Some(ret)) => c_scalar_of_type(ret_ty) == Some(ret),
        _ => false,
    };
    if !ret_matches {
        return false;
    }
    let expected = params
        .iter()
        .map(|&ty| (ty, false))
        .chain(outputs.iter().map(|&ty| (ty, true)));
    sig.inputs
        .iter()
        .zip(expected)
        .all(|(input, (ty, is_output))| {
            let syn::FnArg::Typed(pat_ty) = input else {
                return false;
            };
            if !is_output {
                return c_scalar_of_type(&pat_ty.ty) == Some(ty);
            }
            matches!(&*pat_ty.ty, Type::Ptr(ptr)
                if ptr.mutability.is_some() && c_scalar_of_type(&ptr.elem) == Some(ty))
        })
}

/// The place that an output-pointer argument of pointee type `ty` points
/// to, if the argument can be replaced by a mutable borrow of that place.
///
/// Recognizes `&raw mut place` and `&mut place` (possibly cast to a
/// pointer), a variable of type `&mut T` (whose place is `*var`), and
/// `(&raw mut array as *mut T).offset(k)` (whose place is `array[k]`).
fn cmath_output_place(arg: &Expr, ty: CScalar, symbols: &SymbolTable) -> Option<Expr> {
    let arg = expr_strip_parens(arg);
    let has_cast = matches!(arg, Expr::Cast(_));
    let inner = expr_strip_parens(expr_strip_casts(arg));
    let (place, type_checked) = match inner {
        Expr::RawAddr(raw) if matches!(raw.mutability, syn::PointerMutability::Mut(_)) => {
            (expr_strip_parens(&raw.expr).clone(), !has_cast)
        }
        Expr::Reference(r) if r.mutability.is_some() => {
            (expr_strip_parens(&r.expr).clone(), !has_cast)
        }
        Expr::Path(_) => {
            let Type::Reference(r) = symbols.type_of_expr(inner)? else {
                return None;
            };
            if r.mutability.is_none() || c_scalar_of_type(&r.elem) != Some(ty) {
                return None;
            }
            (syn::parse_quote! { *#inner }, true)
        }
        Expr::MethodCall(m) if m.method == "offset" && m.args.len() == 1 => {
            let Expr::Cast(cast) = expr_strip_parens(&m.receiver) else {
                return None;
            };
            let Expr::RawAddr(raw) = expr_strip_parens(&cast.expr) else {
                return None;
            };
            let array = expr_strip_parens(&raw.expr);
            let Type::Array(array_ty) = symbols.type_of_expr(array)? else {
                return None;
            };
            if c_scalar_of_type(&array_ty.elem) != Some(ty) {
                return None;
            }
            let index = expr_strip_casts(&m.args[0]);
            (syn::parse_quote! { #array[#index as usize] }, true)
        }
        _ => return None,
    };
    if !is_borrowable_place(&place, symbols) {
        return None;
    }
    if !type_checked {
        let place_ty = symbols.type_of_expr(&place)?;
        if c_scalar_of_type(&place_ty) != Some(ty) {
            return None;
        }
    }
    Some(place)
}

/// Whether `&mut place` can be taken without dereferencing a raw pointer or
/// borrowing a `static mut`.
fn is_borrowable_place(place: &Expr, symbols: &SymbolTable) -> bool {
    match expr_strip_parens(place) {
        Expr::Path(path) => path
            .path
            .get_ident()
            .is_some_and(|ident| !symbols.mutable_statics.contains(&ident.to_string())),
        Expr::Field(field) => is_borrowable_place(&field.base, symbols),
        Expr::Index(index) => is_borrowable_place(&index.expr, symbols),
        Expr::Unary(ExprUnary {
            op: syn::UnOp::Deref(_),
            expr,
            ..
        }) => matches!(symbols.type_of_expr(expr_strip_parens(expr)),
            Some(Type::Reference(r)) if r.mutability.is_some()),
        _ => false,
    }
}

/// Whether mutable borrows of all of `places` can coexist: each must be a
/// variable or a chain of named fields of one, and none may contain another.
fn places_are_disjoint(places: &[Expr]) -> bool {
    if places.len() < 2 {
        return true;
    }
    let Some(chains) = places
        .iter()
        .map(place_field_chain)
        .collect::<Option<Vec<_>>>()
    else {
        return false;
    };
    chains.iter().enumerate().all(|(i, a)| {
        chains[i + 1..]
            .iter()
            .all(|b| !a.starts_with(b) && !b.starts_with(a))
    })
}

/// The variable and field names in a place like `a.b.c`, as `["a", "b", "c"]`.
fn place_field_chain(place: &Expr) -> Option<Vec<String>> {
    match expr_strip_parens(place) {
        Expr::Path(path) => Some(vec![path.path.get_ident()?.to_string()]),
        Expr::Field(field) => {
            let syn::Member::Named(member) = &field.member else {
                return None;
            };
            let mut chain = place_field_chain(&field.base)?;
            chain.push(member.to_string());
            Some(chain)
        }
        _ => None,
    }
}

/// Classify a type such as `::core::ffi::c_double` or `f64`.
fn c_scalar_of_type(ty: &Type) -> Option<CScalar> {
    let Type::Path(path) = ty else {
        return None;
    };
    if path.qself.is_some() {
        return None;
    }
    let segment = path.path.segments.last()?;
    if !segment.arguments.is_none() {
        return None;
    }
    match segment.ident.to_string().as_str() {
        "c_float" | "f32" => Some(CScalar::Float),
        "c_double" | "f64" => Some(CScalar::Double),
        "c_int" | "i32" => Some(CScalar::Int),
        "c_long" => Some(CScalar::Long),
        "c_longlong" => Some(CScalar::LongLong),
        _ => None,
    }
}

fn is_zero_integer_expr(expr: &Expr) -> bool {
    match expr_strip_parens(expr) {
        Expr::Cast(cast) if is_integer_cast_type(&cast.ty) => is_zero_integer_expr(&cast.expr),
        Expr::Lit(lit) => {
            matches!(&lit.lit, syn::Lit::Int(int) if int.base10_parse::<u64>().ok() == Some(0))
        }
        _ => false,
    }
}

fn memcmp_float_operand<'a>(
    expr: &'a Expr,
    symbols: &SymbolTable,
    width: FloatWidth,
) -> Option<&'a Expr> {
    let Expr::Cast(cast) = expr_strip_parens(expr) else {
        return None;
    };
    let Type::Ptr(pointer) = &*cast.ty else {
        return None;
    };
    if !matches!(&*pointer.elem, Type::Path(path)
        if path.qself.is_none() && path.path.segments.last().is_some_and(|segment|
            segment.ident == "c_void" && matches!(segment.arguments, syn::PathArguments::None)))
    {
        return None;
    }
    let Expr::RawAddr(address) = expr_strip_parens(&cast.expr) else {
        return None;
    };
    (float_width(&symbols.type_of_expr(&address.expr)?) == Some(width)).then_some(&address.expr)
}

fn float_size_of(expr: &Expr) -> Option<FloatWidth> {
    let Expr::Call(call) = expr_strip_parens(expr) else {
        return None;
    };
    let Expr::Path(function) = expr_strip_parens(&call.func) else {
        return None;
    };
    if !call.args.is_empty()
        || function.qself.is_some()
        || !(is_path_exactly_3(&function.path, "core", "mem", "size_of")
            || is_path_exactly_3(&function.path, "std", "mem", "size_of"))
        || !function
            .path
            .segments
            .iter()
            .take(2)
            .all(|segment| matches!(segment.arguments, syn::PathArguments::None))
    {
        return None;
    }
    let syn::PathArguments::AngleBracketed(arguments) = &function.path.segments[2].arguments else {
        return None;
    };
    if arguments.args.len() != 1 {
        return None;
    }
    match &arguments.args[0] {
        syn::GenericArgument::Type(ty) => float_width(ty),
        _ => None,
    }
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

/// Returns `true` for a byte-string literal wrapped in zero or more casts.
fn is_cast_byte_str(expr: &Expr) -> bool {
    match expr {
        Expr::Lit(lit) => matches!(lit.lit, syn::Lit::ByteStr(_)),
        Expr::Cast(cast) => is_cast_byte_str(&cast.expr),
        _ => false,
    }
}

fn get_block_lone_expr(block: &syn::Block) -> Option<&Expr> {
    if block.stmts.len() == 1 {
        if let Stmt::Expr(expr, None) = &block.stmts[0] {
            return Some(expr);
        }
    }
    None
}

fn get_expr_block_lone_expr(expr: &Expr) -> Option<&Expr> {
    if let Expr::Block(expr_block) = expr {
        return get_block_lone_expr(&expr_block.block);
    }
    None
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

fn is_errno_deref(expr: &Expr) -> bool {
    let Expr::Unary(unary) = expr_strip_parens(expr) else {
        return false;
    };
    if !matches!(unary.op, syn::UnOp::Deref(_)) {
        return false;
    }
    let Expr::Call(call) = expr_strip_parens(&unary.expr) else {
        return false;
    };
    let Expr::Path(function) = expr_strip_parens(&call.func) else {
        return false;
    };
    function.qself.is_none()
        && call.args.is_empty()
        && (function.path.is_ident("__errno_location")
            || (function.path.segments.len() == 2
                && function.path.segments[0].ident == "libc"
                && function.path.segments[1].ident == "__errno_location"
                && function
                    .path
                    .segments
                    .iter()
                    .all(|segment| matches!(segment.arguments, syn::PathArguments::None))))
}

fn is_assignment_operator(op: &syn::BinOp) -> bool {
    matches!(
        op,
        syn::BinOp::AddAssign(_)
            | syn::BinOp::SubAssign(_)
            | syn::BinOp::MulAssign(_)
            | syn::BinOp::DivAssign(_)
            | syn::BinOp::RemAssign(_)
            | syn::BinOp::BitXorAssign(_)
            | syn::BinOp::BitAndAssign(_)
            | syn::BinOp::BitOrAssign(_)
            | syn::BinOp::ShlAssign(_)
            | syn::BinOp::ShrAssign(_)
    )
}

fn contains_errno_assignment_place(expr: &Expr) -> bool {
    match expr_strip_parens(expr) {
        Expr::Tuple(tuple) => tuple.elems.iter().any(contains_errno_assignment_place),
        Expr::Array(array) => array.elems.iter().any(contains_errno_assignment_place),
        expr => is_errno_deref(expr),
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

fn raw_null_field(expr: &Expr) -> Option<(&Type, Vec<&syn::Ident>)> {
    let Expr::RawAddr(raw_addr) = expr_strip_parens(expr) else {
        return None;
    };
    let mut base = expr_strip_parens(&raw_addr.expr);
    let mut fields = Vec::new();
    while let Expr::Field(field) = base {
        let syn::Member::Named(field_name) = &field.member else {
            return None;
        };
        fields.push(field_name);
        base = expr_strip_parens(&field.base);
    }
    if fields.is_empty() {
        return None;
    }
    let Expr::Unary(deref) = base else {
        return None;
    };
    if !matches!(deref.op, syn::UnOp::Deref(_)) {
        return None;
    }
    let struct_ty = null_mut_type(&deref.expr)?;
    fields.reverse();
    Some((struct_ty, fields))
}

fn null_mut_type(expr: &Expr) -> Option<&Type> {
    let Expr::Call(call) = expr_strip_parens(expr) else {
        return None;
    };
    if !call.args.is_empty() {
        return None;
    }
    let Expr::Path(function) = expr_strip_parens(&call.func) else {
        return None;
    };
    if function.qself.is_some() || function.path.segments.len() != 3 {
        return None;
    }
    let mut segments = function.path.segments.iter();
    let core = segments.next()?;
    let ptr = segments.next()?;
    let null_mut = segments.next()?;
    if core.ident != "core"
        || ptr.ident != "ptr"
        || null_mut.ident != "null_mut"
        || !matches!(core.arguments, syn::PathArguments::None)
        || !matches!(ptr.arguments, syn::PathArguments::None)
    {
        return None;
    }
    let syn::PathArguments::AngleBracketed(args) = &null_mut.arguments else {
        return None;
    };
    if args.args.len() != 1 {
        return None;
    }
    let syn::GenericArgument::Type(ty) = &args.args[0] else {
        return None;
    };
    Some(ty)
}

fn is_byte_pointer_type(ty: &Type) -> bool {
    let Type::Ptr(pointer) = ty else {
        return false;
    };
    let Type::Path(path) = &*pointer.elem else {
        return false;
    };
    if path.qself.is_some() {
        return false;
    }
    let names = path
        .path
        .segments
        .iter()
        .map(|segment| segment.ident.to_string())
        .collect::<Vec<_>>();
    matches!(names.as_slice(), [name] if name == "c_char" || name == "i8" || name == "u8")
        || matches!(names.as_slice(), [core, ffi, char_ty] if core == "core" && ffi == "ffi" && char_ty == "c_char")
}

fn is_integer_cast_type(ty: &Type) -> bool {
    let Type::Path(path) = ty else {
        return false;
    };
    if path.qself.is_some() || path.path.segments.is_empty() {
        return false;
    }
    let name = path.path.segments.last().unwrap().ident.to_string();
    matches!(
        name.as_str(),
        "u8" | "u16"
            | "u32"
            | "u64"
            | "u128"
            | "usize"
            | "i8"
            | "i16"
            | "i32"
            | "i64"
            | "i128"
            | "isize"
            | "c_char"
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
            | "size_t"
            | "ssize_t"
            | "ptrdiff_t"
    )
}

fn same_integer_type(left: &Type, right: &Type) -> bool {
    let (Type::Path(left), Type::Path(right)) = (left, right) else {
        return false;
    };
    left.qself.is_none()
        && right.qself.is_none()
        && left.path.segments.len() == right.path.segments.len()
        && left
            .path
            .segments
            .iter()
            .zip(right.path.segments.iter())
            .all(|(a, b)| a.ident == b.ident && a.arguments == b.arguments)
}

fn is_nul_char_expr(expr: &Expr) -> bool {
    matches!(expr_strip_parens(expr_strip_casts(expr)), Expr::Lit(lit) if matches!(&lit.lit, syn::Lit::Char(ch) if ch.value() == '\0'))
}

/// Matches `0`, `0u8`, `'\0'`, and casts thereof.
fn is_zero_byte_expr(expr: &Expr) -> bool {
    is_nul_char_expr(expr)
        || matches!(expr_strip_parens(expr_strip_casts(expr)), Expr::Lit(lit) if matches!(&lit.lit, syn::Lit::Int(int) if int.base10_digits() == "0"))
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

fn is_path_exactly_2(path: &Path, a: &str, b: &str) -> bool {
    path.segments.len() == 2 && path.segments[0].ident == a && path.segments[1].ident == b
}

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

fn expr_strip_refs(expr: &Expr) -> &Expr {
    if let Expr::Reference(e) = expr {
        expr_strip_refs(&e.expr)
    } else if let Expr::Paren(e) = expr {
        expr_strip_refs(&e.expr)
    } else {
        expr
    }
}

impl Rewriter {
    /// Preserve bytes when formatting guided zero-terminated strings.
    pub fn rewrite_cstr_from_ptr_of_zstr(
        &self,
        symbols: &SymbolTable,
        expr: &Expr,
    ) -> Option<(Expr, Depth)> {
        let Expr::Call(call) = expr else { return None };
        let Expr::Path(func) = &*call.func else {
            return None;
        };
        let c_str_constructor = func.path.segments.len() == 4
            && matches!(
                func.path.segments[0].ident.to_string().as_str(),
                "core" | "std"
            )
            && func.path.segments[1].ident == "ffi"
            && func.path.segments[2].ident == "CStr"
            && func.path.segments[3].ident == "from_ptr";
        if !(func.path.is_ident("xj_cstr_from_ptr") || c_str_constructor) || call.args.len() != 1 {
            return None;
        }
        let mut argument = expr_strip_parens(expr_strip_casts(&call.args[0]));
        if let Expr::MethodCall(method) = argument {
            if method.method == "as_ptr" && method.args.is_empty() {
                argument = expr_strip_parens(expr_strip_casts(&method.receiver));
            }
        }
        let place = zstr_place_of_c_str_arg(argument, symbols)?;
        Some((syn::parse_quote! { #place.as_c_str() }, Depth::Limited(0)))
    }

    /// Use bounded item APIs when translated stdio arguments retain their storage.
    pub fn rewrite_cio_buffers(&self, symbols: &SymbolTable, expr: &Expr) -> Option<(Expr, Depth)> {
        let Expr::Call(call) = expr else { return None };
        let Expr::Path(func) = &*call.func else {
            return None;
        };
        let name = func.path.segments.last()?.ident.to_string();
        if func.path.segments.len() > 1
            && !(func.path.segments.len() == 2 && func.path.segments[0].ident == "xj_cio")
            && !(func.path.segments.len() == 3
                && func.path.segments[0].ident == "xj_cio"
                && func.path.segments[1].ident == "c_abi")
        {
            return None;
        }
        if matches!(name.as_str(), "fread" | "fwrite") && call.args.len() == 4 {
            let exclusive = name == "fread";
            let pointer = expr_strip_casts(&call.args[0]);
            let buffer = if exclusive {
                extract_slice_ptr_base(pointer, symbols)?
            } else {
                &call.args[0]
            };
            let read_view = if exclusive {
                None
            } else {
                self.coerce_byte_view_path_input(&call.args[0], symbols)
            };
            if exclusive && !is_u8_or_i8_sliceable_expr(buffer, symbols) {
                return None;
            }
            if !exclusive && read_view.is_none() {
                return None;
            }
            let size = usize_argument(&call.args[1], symbols);
            let count = usize_argument(&call.args[2], symbols);
            let stream = &call.args[3];
            let result_type = if func.path.segments.len() == 1 {
                symbols
                    .fn_outputs
                    .get(&name)
                    .cloned()
                    .unwrap_or_else(|| syn::parse_quote!(usize))
            } else {
                syn::parse_quote!(usize)
            };
            self.add_dep("xj_cio");
            let input: Expr = if let Some(input) = read_view {
                *input
            } else {
                self.add_dep("xj_cstr");
                self.with_cur_file_item_store(|store| {
                    store.add_use(true, vec!["xj_cstr".into()], "ByteSlice");
                });
                syn::parse_quote! { #buffer.as_mut_u8_slice() }
            };
            let target = syn::Ident::new(
                if exclusive {
                    "fread_items"
                } else {
                    "fwrite_items"
                },
                proc_macro2::Span::call_site(),
            );
            let result = syn::parse_quote! {
                xj_cio::#target(#input, #size, #count, #stream)
            };
            return Some((
                usize_result(result, &result_type, symbols),
                Depth::Limited(0),
            ));
        }
        None
    }
}

impl Rewriter {
    /// Replace raw views of known byte buffers with their bounded views.
    pub fn rewrite_cio_destination(
        &self,
        symbols: &SymbolTable,
        expr: &Expr,
    ) -> Option<(Expr, Depth)> {
        let Expr::Call(call) = expr else { return None };
        let Expr::Path(func) = &*call.func else {
            return None;
        };
        let format_operation = if func.path.is_ident("xj_astgrep_snprintf")
            || is_path_exactly_2(&func.path, "xj_cio", "snprintf")
            || is_path_exactly_2(&func.path, "xj_cfmt", "snprintf")
        {
            Some("snprintf")
        } else if func.path.is_ident("xj_astgrep_sprintf")
            || is_path_exactly_2(&func.path, "xj_cio", "sprintf")
            || is_path_exactly_2(&func.path, "xj_cfmt", "sprintf")
        {
            Some("sprintf")
        } else {
            None
        };
        if let Some(operation) = format_operation {
            let minimum = if operation == "snprintf" { 3 } else { 2 };
            if call.args.len() < minimum {
                return None;
            }
            let buffer = extract_slice_ptr_base(expr_strip_casts(&call.args[0]), symbols)?;
            if !is_u8_type_of_buffer(buffer, symbols) {
                return None;
            }
            let mut call = call.clone();
            call.args[0] = syn::parse_quote! { #buffer };
            let mut path = func.path.clone();
            let name = if path.segments.len() == 1 {
                format!("xj_astgrep_{operation}_unchecked")
            } else {
                format!("{operation}_unchecked")
            };
            path.segments.last_mut()?.ident =
                syn::Ident::new(&name, proc_macro2::Span::call_site());
            call.func = Box::new(syn::parse_quote! { #path });
            return Some((Expr::Call(call), Depth::Unlimited));
        }
        if !(func.path.is_ident("xj_cfmt_dest")
            || is_path_exactly_3(&func.path, "core", "slice", "from_raw_parts_mut")
            || is_path_exactly_3(&func.path, "std", "slice", "from_raw_parts_mut"))
            || call.args.len() != 2
        {
            return None;
        }
        let size = &call.args[1];
        let buffer = extract_slice_ptr_base(expr_strip_casts(&call.args[0]), symbols)?;
        if !is_u8_type_of_buffer(buffer, symbols) {
            return None;
        }
        Some((
            syn::parse_quote! { &mut #buffer[..#size as usize] },
            Depth::Limited(0),
        ))
    }
}

fn is_u8_type_of_buffer(expr: &Expr, symbols: &SymbolTable) -> bool {
    fn byte_elements(ty: &Type) -> bool {
        match ty {
            Type::Reference(r) => byte_elements(&r.elem),
            Type::Slice(s) => is_u8_type(&s.elem),
            Type::Array(a) => is_u8_type(&a.elem),
            Type::Path(p) => p.path.segments.last().is_some_and(|seg| {
                seg.ident == "Vec" && matches!(&seg.arguments, syn::PathArguments::AngleBracketed(args)
                    if args.args.first().is_some_and(|arg| matches!(arg, syn::GenericArgument::Type(ty) if is_u8_type(ty))))
            }),
            _ => false,
        }
    }
    symbols
        .type_of_expr(expr)
        .is_some_and(|ty| byte_elements(&ty))
}
