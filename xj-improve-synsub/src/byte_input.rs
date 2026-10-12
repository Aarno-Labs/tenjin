//! Recover borrowed ByteView inputs without reading through raw pointers.

use quote::ToTokens;
use syn::{Expr, Type, parse_quote};

use crate::{Depth, Rewriter, SymbolTable};

fn unparen(mut expr: &Expr) -> &Expr {
    loop {
        expr = match expr {
            Expr::Paren(p) => &p.expr,
            Expr::Group(g) => &g.expr,
            _ => return expr,
        };
    }
}

fn view_type(ty: &Type, symbols: &SymbolTable, depth: usize, allow_os: bool) -> bool {
    if depth > symbols.type_aliases.len() + 8 {
        return false;
    }
    match ty {
        Type::Reference(r) => view_type(&r.elem, symbols, depth + 1, allow_os),
        Type::Paren(p) => view_type(&p.elem, symbols, depth + 1, allow_os),
        Type::Group(g) => view_type(&g.elem, symbols, depth + 1, allow_os),
        Type::Array(a) => matches!(&*a.elem, Type::Path(p) if p.path.is_ident("u8")),
        Type::Slice(s) => matches!(&*s.elem, Type::Path(p) if p.path.is_ident("u8")),
        Type::Path(p) if p.qself.is_none() => {
            if let Some(alias) = p.path.get_ident().and_then(|name| {
                symbols
                    .type_aliases
                    .get(&name.to_string())
                    .and_then(Option::as_ref)
            }) {
                return view_type(alias, symbols, depth + 1, allow_os);
            }
            let Some(last) = p.path.segments.last() else {
                return false;
            };
            if p.path.segments.len() > 1
                && !matches!(
                    p.path.segments[0].ident.to_string().as_str(),
                    "std" | "core" | "alloc" | "xj_zstr" | "xj_cstat"
                )
            {
                return false;
            }
            match last.ident.to_string().as_str() {
                "str" | "String" | "CStr" | "CString" | "ZStr" | "ZString" => true,
                "OsStr" | "OsString" | "Path" | "PathBuf" => allow_os,
                "Vec" | "Box" | "Rc" | "Arc" | "Cow" => {
                    let syn::PathArguments::AngleBracketed(args) = &last.arguments else {
                        return false;
                    };
                    let Some(inner) = args.args.iter().find_map(|arg| match arg {
                        syn::GenericArgument::Type(ty) => Some(ty),
                        _ => None,
                    }) else {
                        return false;
                    };
                    if last.ident == "Vec" {
                        matches!(inner, Type::Path(p) if p.path.is_ident("u8"))
                    } else {
                        view_type(inner, symbols, depth + 1, allow_os)
                    }
                }
                _ => false,
            }
        }
        _ => false,
    }
}

impl Rewriter {
    /// Retarget borrowed inputs only when their complete storage is known.
    /// Function references and raw pointers keep their existing ABI adapters.
    pub fn rewrite_byte_view_calls(
        &self,
        symbols: &SymbolTable,
        expr: &Expr,
    ) -> Option<(Expr, Depth)> {
        let Expr::Call(call) = expr else { return None };
        let Expr::Path(function) = &*call.func else {
            return None;
        };
        if function.qself.is_some() {
            return None;
        }
        let segments = &function.path.segments;
        let name = segments.last()?.ident.to_string();
        let namespace = segments.first()?.ident.to_string();
        let native = segments.len() == 1 || (segments.len() == 2 && namespace == "libc");
        if native {
            if segments.len() == 1 {
                symbols.external_fn(&name)?;
            }
            match name.as_str() {
                "strcmp" | "strncmp" | "strspn" | "strcspn" | "memcmp" => {
                    let count = if matches!(name.as_str(), "strncmp" | "memcmp") {
                        3
                    } else {
                        2
                    };
                    if call.args.len() != count {
                        return None;
                    }
                    let left = self.coerce_byte_view_input(&call.args[0], symbols)?;
                    let right = self.coerce_byte_view_input(&call.args[1], symbols)?;
                    let ident = &segments.last()?.ident;
                    let result = if name == "memcmp" {
                        let length = &call.args[2];
                        self.add_dep("xj_zstr");
                        parse_quote! { ::xj_zstr::memcmp(#left, #right, (#length) as usize).expect("memcmp length exceeds byte view") }
                    } else if name == "strcspn" {
                        self.add_dep("xj_zstr");
                        parse_quote! { ::xj_zstr::strcspn(#left, #right) }
                    } else {
                        self.add_dep("xj_cstr");
                        if count == 3 {
                            let length = &call.args[2];
                            parse_quote! { ::xj_cstr::#ident(#left, #right, (#length) as usize) }
                        } else {
                            parse_quote! { ::xj_cstr::#ident(#left, #right) }
                        }
                    };
                    // Preserve non-usize size_t spellings as well as c_int aliases.
                    let result = if let Some(syn::ReturnType::Type(_, ty)) =
                        symbols.foreign_fns.get(&name).map(|sig| &sig.output)
                    {
                        parse_quote! { (#result) as #ty }
                    } else {
                        result
                    };
                    return Some((result, Depth::Unlimited));
                }
                "nan" | "nanf" if call.args.len() == 1 => {
                    let input = self.coerce_byte_view_input(&call.args[0], symbols)?;
                    let ident = &segments.last()?.ident;
                    self.add_dep("xj_cmath");
                    return Some((
                        parse_quote! { ::xj_cmath::#ident(#input) },
                        Depth::Unlimited,
                    ));
                }
                _ => return None,
            }
        }
        let input_indices: &[usize] = match (namespace.as_str(), name.as_str()) {
            ("xj_cio", "fopen" | "popen" | "rename") => &[0, 1],
            ("xj_cio", "fdopen" | "freopen" | "freopen_" | "fscanf" | "vfscanf") => &[1],
            ("xj_cio", "fmemopen") => &[2],
            ("xj_cio", "fmemopen_") => &[1],
            ("xj_cio", "remove" | "fputs" | "puts" | "scanf" | "vscanf" | "fwrite_") => &[0],
            ("xj_atone", "atof" | "atoi" | "atol" | "atoll") => &[0],
            ("xj_cmath", "nan" | "nanf") => &[0],
            ("xj_atone", name) if name.starts_with("strto") => &[0],
            ("xj_cstat", "stat" | "lstat" | "stat64" | "lstat64") => &[0],
            ("xj_ctime", "strftime") if segments.len() == 3 && segments[1].ident == "compat" => {
                &[1]
            }
            ("xj_cstr", "strlen" | "strchr_idx" | "strrchr_idx") => &[0],
            ("xj_cstr", "strcmp" | "strncmp" | "strspn" | "strstr_idx" | "strstr_ptr") => &[0, 1],
            ("xj_cstr", "strcpy" | "strncpy" | "strcat" | "strncat" | "strstr_mut_ptr") => &[1],
            ("xj_scanf", "scanf_str" | "sscanf" | "bscanf") => &[0, 1],
            ("xj_scanf", "parse_format" | "scanf") => &[0],
            ("xj_scanf", "scanf_core" | "brscanf") => &[1],
            ("xj_zstr", "strlen" | "strnlen" | "strdup" | "strndup" | "memchr") => &[0],
            ("xj_zstr", "strcmp" | "strncmp" | "memcmp" | "strspn" | "strcspn") => &[0, 1],
            (
                "xj_zstr",
                "memcpy" | "strcpy" | "strncpy" | "strcat" | "strncat" | "strstr" | "strpbrk",
            ) => &[1],
            _ => return None,
        };
        let cio_adapter =
            namespace == "xj_cio" && segments.len() == 3 && segments[1].ident == "c_abi";
        let qualified_module = segments.len() == 3
            && matches!(
                (namespace.as_str(), segments[1].ident.to_string().as_str()),
                ("xj_cstat", "c") | ("xj_ctime", "compat") | ("xj_scanf", "legacy")
            );
        if !cio_adapter && segments.len() != 2 && !qualified_module {
            return None;
        }
        let mut result = call.clone();
        for &index in input_indices {
            result.args[index] = *self.coerce_byte_view_input_with_os(
                call.args.get(index)?,
                symbols,
                matches!(namespace.as_str(), "xj_cio" | "xj_cstat"),
            )?;
        }
        if cio_adapter {
            let ident = &segments.last()?.ident;
            result.func = Box::new(parse_quote! { ::xj_cio::#ident });
        }
        (result.to_token_stream().to_string() != call.to_token_stream().to_string())
            .then_some((Expr::Call(result), Depth::Unlimited))
    }

    /// Keep the initialized storage and its stored NULs; consumers choose the
    /// string boundary. Unknown pointers are left for the caller's raw fallback.
    pub(crate) fn coerce_byte_view_input(
        &self,
        expr: &Expr,
        symbols: &SymbolTable,
    ) -> Option<Box<Expr>> {
        self.coerce_byte_view_input_with_os(expr, symbols, false)
    }

    pub(crate) fn coerce_byte_view_path_input(
        &self,
        expr: &Expr,
        symbols: &SymbolTable,
    ) -> Option<Box<Expr>> {
        self.coerce_byte_view_input_with_os(expr, symbols, true)
    }

    fn coerce_byte_view_input_with_os(
        &self,
        expr: &Expr,
        symbols: &SymbolTable,
        allow_os: bool,
    ) -> Option<Box<Expr>> {
        let expr = unparen(expr);
        match expr {
            Expr::Cast(c) if matches!(&*c.ty, Type::Ptr(_)) => {
                return self.coerce_byte_view_input_with_os(&c.expr, symbols, allow_os);
            }
            Expr::MethodCall(m)
                if m.args.is_empty()
                    && matches!(
                        m.method.to_string().as_str(),
                        "as_ptr" | "as_mut_ptr" | "cast"
                    ) =>
            {
                return self.coerce_byte_view_input_with_os(&m.receiver, symbols, allow_os);
            }
            Expr::RawAddr(raw) => {
                let ty = symbols.type_of_expr(&raw.expr)?;
                // An address of a reference/container is not its data pointer.
                // Guided ZString addresses are synthetic C array decay.
                if !matches!(ty, Type::Array(_))
                    && !matches!(&ty, Type::Path(p) if p.path.segments.last().is_some_and(|s| s.ident == "ZString"))
                {
                    return None;
                }
                return self.coerce_byte_view_input_with_os(&raw.expr, symbols, allow_os);
            }
            Expr::Call(c) if c.args.len() == 1 => {
                if let Expr::Path(p) = &*c.func {
                    let segments = &p.path.segments;
                    let constructor = p.path.is_ident("xj_cstr_from_ptr")
                        || (segments.len() == 4
                            && matches!(segments[0].ident.to_string().as_str(), "std" | "core")
                            && segments[1].ident == "ffi"
                            && segments[2].ident == "CStr"
                            && segments[3].ident == "from_ptr");
                    if constructor {
                        if let Some(input) =
                            self.coerce_byte_view_input_with_os(&c.args[0], symbols, allow_os)
                        {
                            return Some(input);
                        }
                        return Some(Box::new(expr.clone()));
                    }
                }
            }
            Expr::Lit(lit)
                if matches!(
                    lit.lit,
                    syn::Lit::ByteStr(_) | syn::Lit::Str(_) | syn::Lit::CStr(_)
                ) =>
            {
                return Some(Box::new(expr.clone()));
            }
            _ => (),
        }
        if let Some(ty) = crate::printf::expr_type(expr, symbols)
            && view_type(&ty, symbols, 0, allow_os)
        {
            return Some(Box::new(if matches!(ty, Type::Reference(_)) {
                expr.clone()
            } else {
                parse_quote! { &#expr }
            }));
        }
        // Signed C character storage still requires a representation view.
        if super::rewrites::is_atone_byte_slice_expr(expr, symbols) {
            self.add_dep("xj_cstr");
            self.with_cur_file_item_store(|store| {
                store.add_use(true, vec!["xj_cstr".into()], "ByteSlice");
            });
            return Some(Box::new(if matches!(expr, Expr::Reference(_)) {
                parse_quote! { (#expr).as_u8_slice() }
            } else {
                parse_quote! { #expr.as_u8_slice() }
            }));
        }
        None
    }
}
