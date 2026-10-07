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
                    | ("fdopen", 1)
                    | ("fmemopen", 2)
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
        self.use_crate(ExternCrate::XjCio);
        let literal = tenjin::expr_is_lit_str_or_bytes(tenjin::expr_strip_casts(&args[fmt_idx]));
        let macro_compatible = literal && refactor_format::macro_compatible(&args[fmt_idx]);
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
            quote::quote!(unsafe { ::core::ffi::CStr::from_ptr(#fmt as _) })
        };
        if macro_compatible && matches!(name.as_str(), "printf" | "fprintf") {
            return Ok(None);
        }
        if macro_compatible
            && name == "snprintf"
            && self
                .parsed_guidance
                .borrow_mut()
                .query_expr_type(self, cargs[0])
                .is_some_and(|g| tenjin::type_is_vec_of_1_path(g.strip_refs(), "u8"))
        {
            return Ok(None);
        }
        if macro_compatible && name == "snprintf" {
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
            let zstr_dest = self
                .parsed_guidance
                .borrow_mut()
                .query_expr_type(self, cargs[0])
                .is_some_and(|g| {
                    tenjin::type_is_xj_zstr(g.strip_refs(), "ZStr")
                        || tenjin::type_is_xj_zstr(g.strip_refs(), "ZString")
                });
            let destination = if zstr_dest {
                let dest = tenjin::expr_strip_casts(dest);
                let dest = if let Expr::RawAddr(raw) = dest {
                    &*raw.expr
                } else {
                    dest
                };
                let size: Box<Expr> = Box::new(syn::parse_quote!(#size as usize));
                let limit = mk().method_call_expr(Box::new(dest.clone()), "limit", vec![size]);
                quote::quote!(#limit)
            } else {
                self.with_cur_file_item_store(|store| store.add_item_str_once(
                    "unsafe fn xj_cfmt_dest<'a>(ptr: *mut u8, len: usize) -> &'a mut [u8] {
                        if len == 0 { &mut [] } else { unsafe { ::core::slice::from_raw_parts_mut(ptr, len) } }
                    }"));
                quote::quote!(unsafe { xj_cfmt_dest(#dest as *mut u8, #size as usize) })
            };
            mac.tokens = quote::quote! { #destination, #tokens };
            return Ok(Some(mk().mac_expr(mac)));
        }
        if macro_compatible
            && name == "sprintf"
            && self
                .parsed_guidance
                .borrow_mut()
                .query_expr_type(self, cargs[0])
                .is_some_and(|g| tenjin::type_is_vec_of_1_path(g.strip_refs(), "u8"))
        {
            return Ok(None);
        }
        if matches!(name.as_str(), "vprintf" | "vfprintf" | "vscanf" | "vfscanf") {
            self.use_feature("c_variadic");
            self.with_cur_file_item_store(|store| {
                let helper = if matches!(name.as_str(), "vscanf" | "vfscanf") {
                    include_str!("cio_vscan.rs")
                } else {
                    include_str!("cio_va.rs")
                };
                store.add_item_str_once(helper);
            });
            let ap = &args[fmt_idx + 1];
            let prefix = &args[..fmt_idx];
            let scanning = matches!(name.as_str(), "vscanf" | "vfscanf");
            let target = Ident::new(name.strip_prefix('v').unwrap(), Span::call_site());
            let values = if scanning {
                quote::quote!(&mut unsafe { xj_cio_vscanargs(xj_cio_fmt, #ap) })
            } else {
                quote::quote!(&unsafe { xj_cio_vargs(xj_cio_fmt, #ap) })
            };
            return Ok(Some(Box::new(syn::parse_quote! {
                { let xj_cio_fmt = #fmt_view;
                  ::xj_cio::#target(#(#prefix,)* xj_cio_fmt, #values) }
            })));
        }
        let scanning = matches!(name.as_str(), "scanf" | "fscanf" | "vscanf" | "vfscanf");
        let mut values = Vec::new();
        let mut storage = Vec::new();
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
                    CTypeKind::Short | CTypeKind::UShort => "Short",
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
                if matches!(ty.kind, CTypeKind::Pointer(_)) {
                    self.with_cur_file_item_store(|store| {
                        store.add_item_str_once(include_str!("cio_ptr_arg.rs"));
                    });
                    let position = i - fmt_idx - 1;
                    let g = self
                        .parsed_guidance
                        .borrow_mut()
                        .query_expr_type(self, cargs[i]);
                    let pointer = if g
                        .as_ref()
                        .is_some_and(|g| !g.required_xj_zstr_types().is_empty())
                    {
                        let arg = tenjin::expr_strip_casts(arg);
                        quote::quote!((#arg).as_c_str().as_ptr())
                    } else if let Some(g) = g.as_ref().filter(|g| {
                        tenjin::type_is_vec_of_1_path(g.strip_refs(), "u8")
                            || tenjin::type_is_string(&g.parsed)
                            || tenjin::type_is_str_ref(&g.parsed)
                    }) {
                        let arg = tenjin::expr_strip_casts(arg);
                        let bytes = if tenjin::type_is_vec_of_1_path(g.strip_refs(), "u8") {
                            quote::quote!((#arg).as_slice())
                        } else {
                            quote::quote!((#arg).as_bytes())
                        };
                        let saved =
                            Ident::new(&format!("xj_cio_arg_{position}"), Span::call_site());
                        storage.push(quote::quote! {
                            let #saved = ::std::ffi::CString::new(#bytes.split(|b| *b == 0).next().unwrap()).unwrap();
                        });
                        quote::quote!(#saved.as_ptr())
                    } else {
                        quote::quote!(#arg)
                    };
                    quote::quote!(xj_cio_ptr_arg(xj_cio_fmt, #position, ::xj_cio::Arg::#variant(#pointer as _)))
                } else {
                    quote::quote!(::xj_cio::Arg::#variant(#arg as _))
                }
            };
            values.push(value);
        }
        let zstr_dest = matches!(name.as_str(), "snprintf" | "sprintf")
            && self
                .parsed_guidance
                .borrow_mut()
                .query_expr_type(self, cargs[0])
                .is_some_and(|g| !g.required_xj_zstr_types().is_empty());
        let target = if zstr_dest { "snprintf_" } else { &name };
        let ident = Ident::new(target, Span::call_site());
        let prefix = if zstr_dest {
            let dest = tenjin::expr_strip_casts(&args[0]);
            let dest = if let Expr::RawAddr(raw) = dest {
                &*raw.expr
            } else {
                dest
            };
            if name == "snprintf" {
                let size = &args[1];
                quote::quote!((#dest).limit(#size as usize),)
            } else {
                self.use_crate(ExternCrate::XjZstr);
                quote::quote!(::xj_zstr::CStrDest::from(#dest),)
            }
        } else {
            let prefix = &args[..fmt_idx];
            quote::quote!(#(#prefix,)*)
        };
        let args = if scanning {
            quote::quote!(&mut [#(#values),*])
        } else {
            quote::quote!(&[#(#values),*])
        };
        self.use_crate(ExternCrate::XjCio);
        Ok(Some(Box::new(
            syn::parse2(quote::quote! {
            { let xj_cio_fmt = #fmt_view;
              #(#storage)*
              ::xj_cio::#ident(#prefix xj_cio_fmt, #args) }
            })
            .map_err(|e| format_err!("Invalid cio call: {e}"))?,
        )))
    }
}
