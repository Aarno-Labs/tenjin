//! Bind system stdio declarations to xj_cio, independently of type guidance.
use super::*;

#[derive(Default)]
pub(crate) struct CioReplacements {
    pub(crate) records: HashSet<CDeclId>,
    pub(crate) functions: HashMap<CDeclId, String>,
    pub(crate) streams: HashMap<CDeclId, &'static str>,
}

pub(crate) fn is_stdio_function(name: &str) -> bool {
    let name = name.strip_suffix("_unlocked").unwrap_or(name);
    matches!(
        name,
        "fopen"
            | "fdopen"
            | "freopen"
            | "fclose"
            | "fileno"
            | "tmpfile"
            | "tmpnam"
            | "ctermid"
            | "remove"
            | "rename"
            | "popen"
            | "pclose"
            | "fmemopen"
            | "open_memstream"
            | "fread"
            | "fwrite"
            | "fgetc"
            | "getc"
            | "getchar"
            | "fputc"
            | "putc"
            | "putchar"
            | "ungetc"
            | "fgets"
            | "fputs"
            | "puts"
            | "getline"
            | "getdelim"
            | "fflush"
            | "setvbuf"
            | "setbuf"
            | "feof"
            | "ferror"
            | "clearerr"
            | "perror"
            | "flockfile"
            | "ftrylockfile"
            | "funlockfile"
            | "fseek"
            | "fseeko"
            | "ftell"
            | "ftello"
            | "rewind"
            | "fgetpos"
            | "fsetpos"
            | "printf"
            | "fprintf"
            | "sprintf"
            | "snprintf"
            | "scanf"
            | "fscanf"
            | "vprintf"
            | "vfprintf"
            | "vsprintf"
            | "vsnprintf"
            | "vscanf"
            | "vfscanf"
    )
}

fn system_stdio_header(ast: &TypedAstContext, decl: &CDecl, tcfg: &TranspilerConfig) -> bool {
    ast.get_source_path(decl).is_some_and(|path| {
        let sysroot = std::env::var_os("CLANG_PATH").and_then(|p| {
            let p = PathBuf::from(p);
            Some(p.parent()?.parent()?.join("sysroot/usr/include"))
        });
        (path.starts_with("/usr/include")
            || sysroot.is_some_and(|p| path.starts_with(p))
            || tcfg.guidance_json["stdio_header_roots"]
                .as_array()
                .is_some_and(|roots| {
                    roots
                        .iter()
                        .filter_map(|r| r.as_str())
                        .any(|root| path.starts_with(root))
                }))
            && (path.ends_with("stdio.h")
                || path.ends_with("bits/stdio.h")
                || path.ends_with("bits/types/struct_FILE.h")
                || path.ends_with("bits/types/FILE.h")
                || path.ends_with("bits/alltypes.h"))
    })
}

fn canonical_stdio_decl(ast: &TypedAstContext, mut id: CDeclId) -> CDeclId {
    while let CDeclKind::NonCanonicalDecl { canonical_decl } = ast[id].kind {
        id = canonical_decl;
    }
    id
}

pub(crate) fn stdio_project_definitions(
    ast: &TypedAstContext,
    tcfg: &TranspilerConfig,
) -> HashSet<String> {
    ast.iter_decls()
        .filter_map(|(_, decl)| match &decl.kind {
            CDeclKind::Function {
                name,
                body: Some(_),
                is_global: true,
                ..
            } if is_stdio_function(name) && !system_stdio_header(ast, decl, tcfg) => {
                Some(name.clone())
            }
            _ => None,
        })
        .collect()
}
fn project_stdio_binding(tcfg: &TranspilerConfig, name: &str) -> bool {
    tcfg.guidance_json["xj_stdio_project_definitions"]
        .as_array()
        .is_some_and(|names| names.iter().any(|n| n.as_str() == Some(name)))
}

/// Preflight before any stdio types or bindings are replaced. Unknown stdio
/// provenance cannot safely share streams with xj_cio, so require explicit
/// roots instead of attempting a per-call fallback.
pub(crate) fn stdio_discovery_error(
    ast: &TypedAstContext,
    tcfg: &TranspilerConfig,
) -> Option<String> {
    for (&id, decl) in ast.iter_decls() {
        let Some(path) = ast.get_source_path(decl) else {
            continue;
        };
        if !path.ends_with("stdio.h") || system_stdio_header(ast, decl, tcfg) {
            continue;
        }
        let target = &ast[canonical_stdio_decl(ast, id)];
        if matches!(&target.kind, CDeclKind::Function { name, body: None, .. } if is_stdio_function(name) && !project_stdio_binding(tcfg, name))
        {
            return Some(format!("System stdio discovery cannot trust {}. A coherent libc fallback with the selected FILE and guidance types cannot be established. Set guidance stdio_header_roots to trusted system include roots (including alternate sysroots or vendored libc headers); project include paths are not trusted automatically.", path.display()));
        }
    }
    None
}

impl CioReplacements {
    pub(crate) fn discover(ast: &TypedAstContext, tcfg: &TranspilerConfig) -> Self {
        let mut result = Self::default();
        for (&id, decl) in ast.iter_decls() {
            if !system_stdio_header(ast, decl, tcfg) {
                continue;
            }
            let id = canonical_stdio_decl(ast, id);
            let target_decl = &ast[id];
            // A project definition supersedes system prototypes. Header inline
            // definitions remain eligible because their provenance is trusted.
            if matches!(target_decl.kind, CDeclKind::Function { body: Some(_), .. })
                && !system_stdio_header(ast, target_decl, tcfg)
            {
                continue;
            }
            match &target_decl.kind {
                CDeclKind::Struct {
                    name: Some(name), ..
                } if matches!(name.as_str(), "_IO_FILE" | "__sFILE") => {
                    result.records.insert(id);
                }
                CDeclKind::Function { name, .. }
                    if is_stdio_function(name) && !project_stdio_binding(tcfg, name) =>
                {
                    // Optimized libc headers define these functions inline in
                    // terms of FILE's private fields. Replace those bodies too.
                    let target = name.strip_suffix("_unlocked").unwrap_or(name);
                    result.functions.insert(id, target.to_owned());
                }
                CDeclKind::Variable {
                    ident,
                    is_defn: false,
                    ..
                } => {
                    let name = match ident.as_str() {
                        "stdin" | "__stdinp" => "stdin",
                        "stdout" | "__stdoutp" => "stdout",
                        "stderr" | "__stderrp" => "stderr",
                        _ => continue,
                    };
                    result.streams.insert(id, name);
                }
                _ => {}
            }
        }
        result
    }
}

impl Translation<'_> {
    pub(crate) fn convert_cio_function(
        &self,
        span: Span,
        typ: CTypeId,
        name: &str,
        target: &str,
        parameters: &[CDeclId],
    ) -> TranslationResult<ConvertedDecl> {
        let CTypeKind::Function(ret, _, variadic, _, _) = self.ast_context.resolve_type(typ).kind
        else {
            unreachable!()
        };
        // Variadic calls are marshalled at their call sites.
        if variadic
            || matches!(
                target,
                "vprintf" | "vfprintf" | "vsprintf" | "vsnprintf" | "vscanf" | "vfscanf"
            )
        {
            return Ok(ConvertedDecl::NoItem);
        }
        let ret = self.convert_type(ret.ctype)?;
        let ident = Ident::new(name, span);
        let target_ident = Ident::new(target, span);
        let mut params = Vec::new();
        let mut args = Vec::new();
        let mut needs_unsafe = matches!(
            target,
            "fdopen"
                | "freopen"
                | "tmpnam"
                | "ctermid"
                | "fmemopen"
                | "fread"
                | "fwrite"
                | "fgets"
                | "getline"
                | "getdelim"
                | "perror"
                | "fgetpos"
                | "fsetpos"
        );
        for (i, &id) in parameters.iter().enumerate() {
            let CDeclKind::Variable { typ, .. } = self.ast_context[id].kind else {
                unreachable!()
            };
            let ty = self.convert_type(typ.ctype)?;
            let arg = Ident::new(&format!("arg{i}"), span);
            params.push(quote::quote!(#arg: #ty));
            let cstr = matches!(
                (target, i),
                ("fopen" | "popen", 0 | 1)
                    | ("fdopen", 1)
                    | ("fmemopen", 2)
                    | ("freopen", 1)
                    | ("remove", 0)
                    | ("rename", 0 | 1)
                    | ("fputs" | "puts", 0)
            );
            if cstr {
                needs_unsafe = true;
                args.push(quote::quote!(unsafe { ::core::ffi::CStr::from_ptr((#arg) as _) }));
            } else {
                args.push(quote::quote!((#arg) as _));
            }
        }
        let unsafety = needs_unsafe.then(|| quote::quote!(unsafe));
        let body = if matches!(self.ast_context.resolve_type(typ).kind, CTypeKind::Function(ret, ..) if matches!(self.ast_context.resolve_type(ret.ctype).kind, CTypeKind::Void))
        {
            quote::quote! { ::xj_cio::#target_ident(#(#args),*); }
        } else {
            quote::quote! { ::xj_cio::#target_ident(#(#args),*) as #ret }
        };
        self.use_crate(ExternCrate::XjCio);
        Ok(ConvertedDecl::Item(Box::new(
            syn::parse2(quote::quote! {
                pub #unsafety extern "C" fn #ident(#(#params),*) -> #ret { #body }
            })
            .map_err(|e| format_err!("Invalid cio adapter: {e}"))?,
        )))
    }
}

impl Translation<'_> {
    #[allow(clippy::vec_box)]
    pub(crate) fn convert_cio_variadic(
        &self,
        callee: Option<CDeclId>,
        args: &[Box<Expr>],
        cargs: &[CExprId],
    ) -> TranslationResult<Option<Box<Expr>>> {
        let Some(name) = callee.and_then(|id| self.cio_replacements.functions.get(&id)) else {
            return Ok(None);
        };
        if matches!(
            name.as_str(),
            "printf"
                | "fprintf"
                | "sprintf"
                | "snprintf"
                | "vprintf"
                | "vfprintf"
                | "vsprintf"
                | "vsnprintf"
        ) {
            return self.convert_c_printf(name, args, cargs).map(Some);
        }
        let fmt_idx = match name.as_str() {
            "scanf" | "vscanf" => 0,
            "fscanf" | "vfscanf" => 1,
            _ => return Ok(None),
        };
        self.use_crate(ExternCrate::XjCio);
        let fmt = &args[fmt_idx];
        let fmt_view = if self
            .parsed_guidance
            .borrow_mut()
            .query_expr_type(self, cargs[fmt_idx])
            .is_some_and(|g| !g.required_xj_zstr_types().is_empty())
        {
            let fmt = tenjin::expr_strip_casts(fmt);
            quote::quote!((#fmt).as_c_str())
        } else {
            quote::quote!(unsafe { ::core::ffi::CStr::from_ptr((#fmt) as _) })
        };
        if matches!(name.as_str(), "vscanf" | "vfscanf") {
            self.use_feature("c_variadic");
            self.with_cur_file_item_store(|store| {
                store.add_item_str_once(include_str!("cio_vscan.rs"))
            });
            let ap = &args[fmt_idx + 1];
            let prefix = &args[..fmt_idx];
            let target = Ident::new(name.strip_prefix('v').unwrap(), Span::call_site());
            return Ok(Some(Box::new(syn::parse_quote! {
                { let xj_cio_fmt = #fmt_view;
                  ::xj_cio::#target(#(#prefix,)* xj_cio_fmt,
                    &mut unsafe { xj_cio_scanargs::xj_cio_vscanargs(xj_cio_fmt, #ap) }) }
            })));
        }
        let mut values = Vec::new();
        for (i, arg) in args.iter().enumerate().skip(fmt_idx + 1) {
            let ty = self
                .ast_context
                .resolve_type(self.ast_context[cargs[i]].kind.get_type().unwrap());
            let value = {
                let CTypeKind::Pointer(pointee) = &ty.kind else {
                    return Err(TranslationError::generic(
                        "scanf destination is not a pointer",
                    ));
                };
                let kind = &self.ast_context.resolve_type(pointee.ctype).kind;
                // The AST preserves portable integer typedefs as distinct kinds.
                // Their destinations must still receive numeric writes, including
                // pointer-sized integers on both 32-bit and 64-bit targets.
                if matches!(
                    kind,
                    CTypeKind::IntPtr
                        | CTypeKind::SSize
                        | CTypeKind::PtrDiff
                        | CTypeKind::UIntPtr
                        | CTypeKind::Size
                ) {
                    let (small, large) = if kind.is_unsigned_integral_type() {
                        ("UInt", "ULongLong")
                    } else {
                        ("Int", "LongLong")
                    };
                    let small = Ident::new(small, Span::call_site());
                    let large = Ident::new(large, Span::call_site());
                    values.push(quote::quote! {
                        if ::core::mem::size_of::<usize>() == ::core::mem::size_of::<::core::ffi::c_int>() {
                            ::xj_cio::OutArg::#small((#arg) as _)
                        } else {
                            ::xj_cio::OutArg::#large((#arg) as _)
                        }
                    });
                    continue;
                }
                let variant = match kind {
                    CTypeKind::Int | CTypeKind::Int32 => "Int",
                    CTypeKind::Short | CTypeKind::UShort | CTypeKind::Int16 | CTypeKind::UInt16 => {
                        "Short"
                    }
                    CTypeKind::Long => "Long",
                    CTypeKind::LongLong | CTypeKind::Int64 | CTypeKind::IntMax => "LongLong",
                    CTypeKind::UInt | CTypeKind::UInt32 => "UInt",
                    CTypeKind::ULong => "ULong",
                    CTypeKind::ULongLong | CTypeKind::UInt64 | CTypeKind::UIntMax => "ULongLong",
                    CTypeKind::Float => "Float",
                    CTypeKind::Double => "Double",
                    _ => "Str",
                };
                let variant = Ident::new(variant, Span::call_site());
                quote::quote!(::xj_cio::OutArg::#variant((#arg) as _))
            };
            values.push(value);
        }
        let ident = Ident::new(name, Span::call_site());
        let prefix_args = &args[..fmt_idx];
        let prefix = quote::quote!(#(#prefix_args,)*);
        let args = quote::quote!(&mut [#(#values),*]);
        self.use_crate(ExternCrate::XjCio);
        Ok(Some(Box::new(
            syn::parse2(quote::quote! {
            { let xj_cio_fmt = #fmt_view;
              ::xj_cio::#ident(#prefix xj_cio_fmt, #args) }
            })
            .map_err(|e| format_err!("Invalid cio call: {e}"))?,
        )))
    }
}

impl Translation<'_> {
    fn printf_guided_expr(expr: &Expr) -> &Expr {
        let expr = tenjin::expr_strip_casts(expr);
        match expr {
            Expr::MethodCall(call)
                if matches!(call.method.to_string().as_str(), "as_ptr" | "as_mut_ptr")
                    && call.args.is_empty() =>
            {
                Self::printf_guided_expr(&call.receiver)
            }
            Expr::Paren(paren) => Self::printf_guided_expr(&paren.expr),
            _ => expr,
        }
    }

    fn printf_literal(&self, expr: CExprId) -> Option<&[u8]> {
        match &self.ast_context[self.c_strip_implicit_casts(expr)].kind {
            CExprKind::Literal(_, CLiteral::String(bytes, 1)) => Some(xj_cfmt_core::c_bytes(bytes)),
            _ => None,
        }
    }

    #[allow(clippy::vec_box)]
    fn convert_c_printf(
        &self,
        operation: &str,
        args: &[Box<Expr>],
        cargs: &[CExprId],
    ) -> TranslationResult<Box<Expr>> {
        use xj_cfmt_core::{Length, Plan, Requirement};
        let name = operation.strip_prefix('v').unwrap_or(operation);
        let fmt_idx = match name {
            "printf" => 0,
            "snprintf" => 2,
            _ => 1,
        };
        if args.len() <= fmt_idx {
            return Err(TranslationError::generic("missing C printing format"));
        }
        let literal = self.printf_literal(cargs[fmt_idx]);
        let plan = literal.map(Plan::new).transpose().map_err(|e| {
            TranslationError::from(format_err!("unsupported {operation} format: {e}"))
        })?;
        let fmt = &args[fmt_idx];
        let fmt_guided = self
            .parsed_guidance
            .borrow_mut()
            .query_expr_type(self, cargs[fmt_idx]);
        let mut safe = literal.is_some()
            || fmt_guided.as_ref().is_some_and(|g| {
                !g.required_xj_zstr_types().is_empty()
                    || tenjin::type_is_vec_of_1_path(g.strip_refs(), "u8")
                    || tenjin::type_is_string(&g.parsed)
                    || tenjin::type_is_str_ref(&g.parsed)
            });
        let format = if let Some(bytes) = literal {
            let lit = syn::LitByteStr::new(bytes, Span::call_site());
            quote::quote!(#lit)
        } else if safe {
            let fmt = Self::printf_guided_expr(fmt);
            quote::quote!(#fmt)
        } else {
            quote::quote!(::core::ffi::CStr::from_ptr((#fmt) as _))
        };
        let mut values = Vec::new();
        for (position, arg) in args
            .iter()
            .enumerate()
            .skip(fmt_idx + 1)
            .filter(|_| !operation.starts_with('v'))
        {
            let index = position - fmt_idx - 1;
            let kind = &self
                .ast_context
                .resolve_type(self.ast_context[cargs[position]].kind.get_type().unwrap())
                .kind;
            let guidance = self
                .parsed_guidance
                .borrow_mut()
                .query_expr_type(self, cargs[position]);
            let guided = guidance.as_ref().is_some_and(|g| {
                !g.required_xj_zstr_types().is_empty()
                    || tenjin::type_is_vec_of_1_path(g.strip_refs(), "u8")
                    || tenjin::type_is_string(&g.parsed)
                    || tenjin::type_is_str_ref(&g.parsed)
            });
            let mut value = if guided {
                let arg = Self::printf_guided_expr(arg);
                quote::quote!(#arg)
            } else {
                quote::quote!(#arg)
            };
            if matches!(kind, CTypeKind::Pointer(_)) && !guided {
                safe = false;
            }
            if let Some(requirement) = plan.as_ref().and_then(|p| p.requirements.get(index)) {
                if guided && !matches!(requirement, Requirement::String) {
                    return Err(TranslationError::from(format_err!(
                        "{operation} argument {} has guided byte storage that cannot supply this conversion; use an explicit scalar or raw adapter",
                        index + 1
                    )));
                }
                // Promotions and length interpretation live in the engine. Only
                // mismatched source types receive visible normalization casts.
                let expected = match requirement {
                    Requirement::Int => Some((
                        matches!(kind, CTypeKind::Int | CTypeKind::Int32),
                        quote::quote!(::core::ffi::c_int),
                    )),
                    Requirement::UInt | Requirement::WideChar => Some((
                        matches!(kind, CTypeKind::UInt | CTypeKind::UInt32),
                        quote::quote!(::core::ffi::c_uint),
                    )),
                    Requirement::Long => Some((
                        matches!(kind, CTypeKind::Long),
                        quote::quote!(::core::ffi::c_long),
                    )),
                    Requirement::ULong => Some((
                        matches!(kind, CTypeKind::ULong),
                        quote::quote!(::core::ffi::c_ulong),
                    )),
                    Requirement::LongLong => Some((
                        matches!(kind, CTypeKind::LongLong),
                        quote::quote!(::core::ffi::c_longlong),
                    )),
                    Requirement::ULongLong => Some((
                        matches!(kind, CTypeKind::ULongLong),
                        quote::quote!(::core::ffi::c_ulonglong),
                    )),
                    Requirement::IntMax => Some((
                        matches!(kind, CTypeKind::IntMax | CTypeKind::Int64),
                        quote::quote!(i64),
                    )),
                    Requirement::UIntMax => Some((
                        matches!(kind, CTypeKind::UIntMax | CTypeKind::UInt64),
                        quote::quote!(u64),
                    )),
                    Requirement::SignedSize | Requirement::PtrDiff => Some((
                        matches!(kind, CTypeKind::SSize | CTypeKind::PtrDiff),
                        quote::quote!(isize),
                    )),
                    Requirement::Size | Requirement::UnsignedPtrDiff => Some((
                        matches!(kind, CTypeKind::Size | CTypeKind::UIntPtr),
                        quote::quote!(usize),
                    )),
                    Requirement::Double => {
                        Some((matches!(kind, CTypeKind::Double), quote::quote!(f64)))
                    }
                    Requirement::String | Requirement::Pointer | Requirement::Count(_) => {
                        let CTypeKind::Pointer(pointee) = kind else {
                            return Err(TranslationError::from(format_err!(
                                "{operation} argument {} requires a pointer",
                                index + 1
                            )));
                        };
                        if matches!(requirement, Requirement::Count(_)) {
                            safe = false;
                        }
                        let pointee = &self.ast_context.resolve_type(pointee.ctype).kind;
                        if guided {
                            None
                        } else {
                            Some(match requirement {
                                Requirement::String => {
                                    (matches!(pointee, CTypeKind::Char), quote::quote!(*const u8))
                                }
                                Requirement::Pointer => (
                                    matches!(pointee, CTypeKind::Void),
                                    quote::quote!(*mut ::core::ffi::c_void),
                                ),
                                Requirement::Count(length) => match length {
                                    Length::Hh => (
                                        matches!(pointee, CTypeKind::SChar),
                                        quote::quote!(*mut i8),
                                    ),
                                    Length::H => (
                                        matches!(pointee, CTypeKind::Short | CTypeKind::Int16),
                                        quote::quote!(*mut ::core::ffi::c_short),
                                    ),
                                    Length::None => (
                                        matches!(pointee, CTypeKind::Int | CTypeKind::Int32),
                                        quote::quote!(*mut ::core::ffi::c_int),
                                    ),
                                    Length::L => (
                                        matches!(pointee, CTypeKind::Long),
                                        quote::quote!(*mut ::core::ffi::c_long),
                                    ),
                                    Length::Ll => (
                                        matches!(pointee, CTypeKind::LongLong),
                                        quote::quote!(*mut ::core::ffi::c_longlong),
                                    ),
                                    Length::J => (
                                        matches!(pointee, CTypeKind::IntMax | CTypeKind::Int64),
                                        quote::quote!(*mut i64),
                                    ),
                                    Length::Z => (
                                        matches!(pointee, CTypeKind::SSize),
                                        quote::quote!(*mut isize),
                                    ),
                                    Length::T => (
                                        matches!(pointee, CTypeKind::PtrDiff),
                                        quote::quote!(*mut isize),
                                    ),
                                    Length::BigL => unreachable!(),
                                },
                                _ => unreachable!(),
                            })
                        }
                    }
                };
                if let Some((false, ty)) = expected {
                    warn!("{operation} argument {} has C type {:?}; normalizing to {} for its conversion", index + 1, kind, ty);
                    value = quote::quote!((#value) as #ty);
                }
            }
            values.push(value);
        }
        if let Some(plan) = &plan {
            if !operation.starts_with('v') && values.len() < plan.requirements.len() {
                return Err(TranslationError::from(format_err!(
                    "{operation} format needs {} arguments, found {}",
                    plan.requirements.len(),
                    values.len()
                )));
            }
            safe &= !plan.count_stores;
        }
        self.use_crate(ExternCrate::XjCio);
        if operation.starts_with('v') {
            self.use_feature("c_variadic");
            self.with_cur_file_item_store(|store| {
                store.add_item_str_once(include_str!("cio_va.rs"))
            });
            let ap = &args[fmt_idx + 1];
            let mut prefix: Vec<_> = args[..fmt_idx]
                .iter()
                .map(|arg| quote::quote!(#arg))
                .collect();
            let mut guided_dest = false;
            if matches!(name, "sprintf" | "snprintf") {
                let guidance = self
                    .parsed_guidance
                    .borrow_mut()
                    .query_expr_type(self, cargs[0]);
                if let Some(g) = guidance.filter(|g| {
                    !g.required_xj_zstr_types().is_empty()
                        || tenjin::type_is_vec_of_1_path(g.strip_refs(), "u8")
                }) {
                    guided_dest = true;
                    let dest = Self::printf_guided_expr(&args[0]);
                    prefix[0] = if let Expr::RawAddr(raw) = dest {
                        let dest = &raw.expr;
                        quote::quote!(&mut #dest)
                    } else if matches!(g.parsed, Type::Reference(_)) {
                        quote::quote!(&mut *(#dest))
                    } else {
                        quote::quote!(&mut #dest)
                    };
                } else {
                    let dest = &args[0];
                    prefix[0] = quote::quote!((#dest) as *mut u8);
                }
                if name == "snprintf" {
                    let size = &args[1];
                    prefix[1] = quote::quote!((#size) as usize);
                }
            }
            let target = Ident::new(
                &format!("{name}_plan{}", if guided_dest { "_" } else { "" }),
                Span::call_site(),
            );
            let fresh = |name: &str| {
                Ident::new(
                    &self
                        .renamer
                        .borrow_mut()
                        .pick_name(&format!("c2rust_{name}"), Namespaces::values()),
                    Span::call_site(),
                )
            };
            let fmt_name = fresh("xj_cio_fmt");
            let plan_name = fresh("xj_cio_plan");
            let args_name = fresh("xj_cio_args");
            let ap_name = fresh("xj_cio_ap");
            let prefix_names: Vec<_> = prefix.iter().map(|_| fresh("xj_cio_dest")).collect();
            let mut bindings: Vec<_> = prefix
                .iter()
                .zip(&prefix_names)
                .map(|(expr, name)| quote::quote!(let #name = #expr;))
                .collect();
            // Evaluate the bound before borrowing guided storage: the C size
            // expression can itself read that storage.
            if guided_dest && name == "snprintf" {
                bindings.swap(0, 1);
            }
            return Ok(Box::new(syn::parse_quote! {
                { #(#bindings)*
                  let #fmt_name = &#format;
                  let #ap_name = #ap;
                  match ::xj_cio::cformat::Plan::new(::xj_cio::__cfmt::CBytes::c_bytes(&#fmt_name)) {
                    Ok(#plan_name) => {
                        let #args_name = xj_cio_varargs::xj_cio_vargs(&#plan_name, #ap_name);
                        ::xj_cio::#target(#(#prefix_names,)* &#plan_name, &#args_name)
                    }
                    Err(e) => ::xj_cio::format_error(e),
                  }
                }
            }));
        }
        let mut prefix = Vec::new();
        let mut guided_dest = false;
        if matches!(name, "sprintf" | "snprintf") {
            let dest = &args[0];
            guided_dest = self
                .parsed_guidance
                .borrow_mut()
                .query_expr_type(self, cargs[0])
                .is_some_and(|g| {
                    !g.required_xj_zstr_types().is_empty()
                        || tenjin::type_is_vec_of_1_path(g.strip_refs(), "u8")
                });
            if guided_dest {
                let dest = Self::printf_guided_expr(dest);
                let dest = if let Expr::RawAddr(raw) = dest {
                    &*raw.expr
                } else {
                    dest
                };
                prefix.push(quote::quote!(#dest));
            } else {
                safe = false;
                prefix.push(quote::quote!((#dest) as *mut u8));
            }
            if name == "snprintf" {
                let size = &args[1];
                prefix.push(quote::quote!((#size) as usize));
            }
        } else if name == "fprintf" {
            let stream = &args[0];
            prefix.push(quote::quote!(#stream));
        }
        let suffix = if safe {
            "_safe"
        } else if guided_dest {
            "_unchecked"
        } else {
            ""
        };
        let mac = Ident::new(&format!("{name}{suffix}"), Span::call_site());
        // Leave unsafe operations visible to Tenjin's ununsafe pass, so the
        // enclosing function and its callers retain their required contracts.
        Ok(Box::new(syn::parse_quote! {
            ::xj_cio::#mac!(#(#prefix,)* #format #(, #values)*)
        }))
    }
}
