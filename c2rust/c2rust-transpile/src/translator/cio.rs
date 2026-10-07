//! Bind system stdio declarations to xj_cio, independently of type guidance.
use super::*;

#[derive(Default)]
pub(crate) struct CioReplacements {
    pub(crate) records: HashSet<CDeclId>,
    pub(crate) functions: HashMap<CDeclId, String>,
    pub(crate) streams: HashMap<CDeclId, &'static str>,
}

pub(crate) fn is_stdio_function(name: &str) -> bool {
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
            | "vscanf"
            | "vfscanf"
    )
}

fn system_stdio_header(ast: &TypedAstContext, decl: &CDecl) -> bool {
    ast.get_source_path(decl).is_some_and(|path| {
        let sysroot = std::env::var_os("CLANG_PATH").and_then(|p| {
            let p = PathBuf::from(p);
            Some(p.parent()?.parent()?.join("sysroot/usr/include"))
        });
        (path.starts_with("/usr/include") || sysroot.is_some_and(|p| path.starts_with(p)))
            && (path.ends_with("stdio.h")
                || path.ends_with("bits/types/struct_FILE.h")
                || path.ends_with("bits/types/FILE.h"))
    })
}

impl CioReplacements {
    pub(crate) fn discover(ast: &TypedAstContext) -> Self {
        let mut result = Self::default();
        for (&id, decl) in ast.iter_decls() {
            if !system_stdio_header(ast, decl) {
                continue;
            }
            match &decl.kind {
                CDeclKind::Struct {
                    name: Some(name), ..
                } if matches!(name.as_str(), "_IO_FILE" | "__sFILE") => {
                    result.records.insert(id);
                }
                CDeclKind::Function {
                    name, body: None, ..
                } if is_stdio_function(name) => {
                    result.functions.insert(id, name.clone());
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
        if variadic || matches!(target, "vprintf" | "vfprintf" | "vscanf" | "vfscanf") {
            return Ok(ConvertedDecl::NoItem);
        }
        let ret = self.convert_type(ret.ctype)?;
        let ident = Ident::new(name, span);
        let target_ident = Ident::new(target, span);
        let mut params = Vec::new();
        let mut args = Vec::new();
        let mut needs_unsafe = false;
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
                    | ("fdopen" | "fmemopen", 1)
                    | ("freopen", 1)
                    | ("remove", 0)
                    | ("rename", 0 | 1)
                    | ("fputs" | "puts", 0)
            );
            if cstr {
                needs_unsafe = true;
                args.push(quote::quote!(unsafe { ::core::ffi::CStr::from_ptr(#arg as _) }));
            } else {
                args.push(quote::quote!(#arg as _));
            }
        }
        // fmemopen's mode is its third argument.
        if target == "fmemopen" {
            args[1] = quote::quote!(arg1 as _);
            args[2] = quote::quote!(unsafe { ::core::ffi::CStr::from_ptr(arg2 as _) });
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
        func: &Expr,
        args: &[Box<Expr>],
        cargs: &[CExprId],
    ) -> TranslationResult<Option<Box<Expr>>> {
        let Some(path) = tenjin::expr_get_path(func) else {
            return Ok(None);
        };
        let Some(name) = tenjin::path_get_1_ident(path).map(|s| s.to_string()) else {
            return Ok(None);
        };
        if !self.cio_replacements.functions.values().any(|n| n == &name) {
            return Ok(None);
        }
        let fmt_idx = match name.as_str() {
            "printf" | "scanf" | "vprintf" | "vscanf" => 0,
            "fprintf" | "fscanf" | "sprintf" | "vfprintf" | "vfscanf" => 1,
            "snprintf" => 2,
            _ => return Ok(None),
        };
        let literal = tenjin::expr_is_lit_str_or_bytes(tenjin::expr_strip_casts(&args[fmt_idx]));
        if literal && matches!(name.as_str(), "printf" | "fprintf") {
            return Ok(None);
        }
        if literal
            && name == "snprintf"
            && self
                .parsed_guidance
                .borrow_mut()
                .query_expr_type(self, cargs[0])
                .is_some_and(|g| tenjin::type_is_vec_of_1_path(g.strip_refs(), "u8"))
        {
            return Ok(None);
        }
        if literal && name == "snprintf" {
            let mut mac = refactor_format::build_format_macro(
                self,
                "xj_cfmt::csnprintf",
                "xj_cfmt::csnprintf",
                &args[fmt_idx..],
                &cargs[fmt_idx..],
                None,
                None,
            );
            let dest = &args[0];
            let size = &args[1];
            let tokens = mac.tokens;
            mac.tokens = quote::quote! { unsafe { ::core::slice::from_raw_parts_mut(#dest as *mut u8, #size as usize) }, #tokens };
            return Ok(Some(mk().mac_expr(mac)));
        }
        if literal
            && name == "sprintf"
            && self
                .parsed_guidance
                .borrow_mut()
                .query_expr_type(self, cargs[0])
                .is_some_and(|g| tenjin::type_is_vec_of_1_path(g.strip_refs(), "u8"))
        {
            return Ok(None);
        }
        if matches!(name.as_str(), "vprintf" | "vfprintf") {
            self.use_feature("c_variadic");
            self.with_cur_file_item_store(|store| {
                let helper = include_str!("cio_va.rs");
                store.add_item_str_once(helper);
            });
            let fmt = &args[fmt_idx];
            let ap = &args[fmt_idx + 1];
            let prefix = &args[..fmt_idx];
            let target = Ident::new(
                if name == "vprintf" {
                    "printf"
                } else {
                    "fprintf"
                },
                Span::call_site(),
            );
            return Ok(Some(Box::new(syn::parse_quote! {
                { let fmt = unsafe { ::core::ffi::CStr::from_ptr(#fmt as _) };
                  ::xj_cio::#target(#(#prefix,)* fmt, &unsafe { xj_cio_vargs(fmt, #ap) }) }
            })));
        }
        let scanning = matches!(name.as_str(), "scanf" | "fscanf" | "vscanf" | "vfscanf");
        let mut values = Vec::new();
        for (i, arg) in args.iter().enumerate().skip(fmt_idx + 1) {
            let ty = self
                .ast_context
                .resolve_type(self.ast_context[cargs[i]].kind.get_type().unwrap());
            let value = if scanning {
                let CTypeKind::Pointer(pointee) = &ty.kind else {
                    return Err(TranslationError::generic(
                        "scanf destination is not a pointer",
                    ));
                };
                let variant = match self.ast_context.resolve_type(pointee.ctype).kind {
                    CTypeKind::Int => "Int",
                    CTypeKind::Short => "Short",
                    CTypeKind::Long => "Long",
                    CTypeKind::LongLong => "LongLong",
                    CTypeKind::UInt => "UInt",
                    CTypeKind::ULong => "ULong",
                    CTypeKind::ULongLong => "ULongLong",
                    CTypeKind::Float => "Float",
                    CTypeKind::Double => "Double",
                    _ => "Str",
                };
                let variant = Ident::new(variant, Span::call_site());
                quote::quote!(::xj_cio::OutArg::#variant(#arg as _))
            } else {
                let variant = match ty.kind {
                    CTypeKind::Float | CTypeKind::Double | CTypeKind::LongDouble => "F",
                    CTypeKind::UInt | CTypeKind::ULong | CTypeKind::ULongLong => "U",
                    CTypeKind::Pointer(p) => {
                        if matches!(
                            self.ast_context.resolve_type(p.ctype).kind,
                            CTypeKind::Char | CTypeKind::SChar | CTypeKind::UChar
                        ) {
                            "S"
                        } else {
                            "P"
                        }
                    }
                    _ => "I",
                };
                let variant = Ident::new(variant, Span::call_site());
                quote::quote!(::xj_cio::Arg::#variant(#arg as _))
            };
            values.push(value);
        }
        let ident = Ident::new(&name, Span::call_site());
        let fmt = &args[fmt_idx];
        let prefix = &args[..fmt_idx];
        let args = if scanning {
            quote::quote!(&mut [#(#values),*])
        } else {
            quote::quote!(&[#(#values),*])
        };
        self.use_crate(ExternCrate::XjCio);
        Ok(Some(Box::new(syn::parse2(quote::quote! {
            ::xj_cio::#ident(#(#prefix,)* unsafe { ::core::ffi::CStr::from_ptr(#fmt as _) }, #args)
        }).map_err(|e| format_err!("Invalid cio call: {e}"))?)))
    }
}
