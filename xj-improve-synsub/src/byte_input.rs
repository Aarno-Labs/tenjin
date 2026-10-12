//! Recover borrowed ByteView inputs without reading through raw pointers.

use quote::ToTokens;
use syn::{Expr, Type, parse_quote};

use crate::{Depth, Rewriter, SymbolTable};

/// Recognize byte types whose unsignedness does not depend on the target.
/// In particular, `c_char` is not necessarily a `u8` alias.
pub(crate) fn is_u8_type(ty: &Type) -> bool {
    match ty {
        Type::Paren(p) => is_u8_type(&p.elem),
        Type::Group(g) => is_u8_type(&g.elem),
        Type::Path(p) if p.qself.is_none() => {
            if p.path.is_ident("u8") {
                return true;
            }
            if p.path
                .segments
                .iter()
                .any(|s| !matches!(s.arguments, syn::PathArguments::None))
            {
                return false;
            }
            let names: Vec<_> = p
                .path
                .segments
                .iter()
                .map(|s| s.ident.to_string())
                .collect();
            matches!(
                names
                    .iter()
                    .map(String::as_str)
                    .collect::<Vec<_>>()
                    .as_slice(),
                ["core" | "std", "ffi", "c_uchar"]
                    | ["std", "os", "raw", "c_uchar"]
                    | ["libc", "c_uchar"]
            )
        }
        _ => false,
    }
}

/// Prefer text at ByteView inputs only when its complete view is identical.
/// Keep stored NULs, including the implicit terminator of a C string literal.
/// This must not be used where a byte array or CStr type is required.
pub(crate) fn prefer_string_literal(expr: &Expr) -> Option<Expr> {
    match expr {
        Expr::Reference(reference) if reference.mutability.is_none() => {
            let mut result = reference.clone();
            result.expr = Box::new(prefer_string_literal(&reference.expr)?);
            Some(Expr::Reference(result))
        }
        Expr::Paren(paren) => {
            let mut result = paren.clone();
            result.expr = Box::new(prefer_string_literal(&paren.expr)?);
            Some(Expr::Paren(result))
        }
        Expr::Group(group) => {
            let mut result = group.clone();
            result.expr = Box::new(prefer_string_literal(&group.expr)?);
            Some(Expr::Group(result))
        }
        Expr::Lit(lit) => {
            let (bytes, span) = match &lit.lit {
                syn::Lit::ByteStr(bytes) => (bytes.value(), bytes.span()),
                syn::Lit::CStr(string) => (string.value().into_bytes_with_nul(), string.span()),
                _ => return None,
            };
            let text = std::str::from_utf8(&bytes).ok()?;
            let mut result = lit.clone();
            result.lit = syn::Lit::Str(syn::LitStr::new(text, span));
            Some(Expr::Lit(result))
        }
        _ => None,
    }
}

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
        Type::Array(a) => is_u8_type(&a.elem),
        Type::Slice(s) => is_u8_type(&s.elem),
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
                        is_u8_type(inner)
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
        if let Expr::MethodCall(method) = expr {
            if method.method == "view_bytes" && method.args.is_empty() && method.turbofish.is_none()
            {
                let mut result = method.clone();
                result.receiver = Box::new(prefer_string_literal(&method.receiver)?);
                return Some((Expr::MethodCall(result), Depth::Unlimited));
            }
            if method.method == "format"
                && method.args.len() == 1
                && method.turbofish.is_none()
                && let Some(ty) = symbols.type_of_expr(&method.receiver)
            {
                let mut ty = &ty;
                while let Type::Reference(reference) = ty {
                    ty = &reference.elem;
                }
                if matches!(ty, Type::Path(p) if p.qself.is_none()
                    && (p.path.is_ident("Tm") || (p.path.segments.len() == 2
                        && p.path.segments[0].ident == "xj_ctime" && p.path.segments[1].ident == "Tm")))
                {
                    let mut result = method.clone();
                    result.args[0] = prefer_string_literal(&method.args[0])?;
                    return Some((Expr::MethodCall(result), Depth::Unlimited));
                }
            }
            return None;
        }
        let Expr::Call(call) = expr else { return None };
        let Expr::Path(function) = &*call.func else {
            return None;
        };
        if function.qself.is_some() {
            return None;
        }
        let segments = &function.path.segments;
        // Explicit generic arguments may require an array or CStr input type.
        if segments.iter().any(|segment| !segment.arguments.is_none()) {
            return None;
        }
        let name = segments.last()?.ident.to_string();
        let namespace = segments.first()?.ident.to_string();
        if segments.len() == 3
            && segments[1].ident == "ByteView"
            && name == "view_bytes"
            && matches!(
                namespace.as_str(),
                "xj_bytes"
                    | "xj_cfmt"
                    | "xj_zstr"
                    | "xj_cio"
                    | "xj_cstr"
                    | "xj_atone"
                    | "xj_scanf"
                    | "xj_cstat"
                    | "xj_ctime"
                    | "xj_cmath"
            )
            && call.args.len() == 1
        {
            let mut result = call.clone();
            result.args[0] = prefer_string_literal(&call.args[0])?;
            return Some((Expr::Call(result), Depth::Unlimited));
        }
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
                    self.add_dep("xj_zstr");
                    let result = if name == "memcmp" {
                        let length = &call.args[2];
                        parse_quote! { ::xj_zstr::memcmp(#left, #right, (#length) as usize).expect("memcmp length exceeds byte view") }
                    } else if count == 3 {
                        let length = &call.args[2];
                        parse_quote! { ::xj_zstr::#ident(#left, #right, (#length) as usize) }
                    } else {
                        parse_quote! { ::xj_zstr::#ident(#left, #right) }
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
        let namespace = namespace.as_str();
        let input_indices: &[usize] = match (namespace, name.as_str()) {
            ("xj_cio", "fopen" | "popen" | "rename") => &[0, 1],
            ("xj_cio", "fdopen" | "freopen" | "fscanf" | "vfscanf") => &[1],
            ("xj_cio", "freopen_") => &[0, 1],
            ("xj_cio", "perror_") => &[0],
            ("xj_cio", "fmemopen") => &[2],
            ("xj_cio", "fmemopen_") => &[1],
            (
                "xj_cio",
                "remove" | "fputs" | "puts" | "scanf" | "vscanf" | "fwrite_" | "fwrite_items",
            ) => &[0],
            ("xj_atone", "atof" | "atoi" | "atol" | "atoll") => &[0],
            ("xj_cmath", "nan" | "nanf") => &[0],
            ("xj_atone", name) if name.starts_with("strto") => &[0],
            ("xj_cstat", "stat" | "lstat" | "stat64" | "lstat64") => &[0],
            ("xj_ctime", "strftime") if segments.len() == 3 && segments[1].ident == "compat" => {
                &[1]
            }
            (
                "xj_zstr",
                "strcmp_eq" | "strcmp_ne" | "strcmp_lt" | "strcmp_le" | "strcmp_gt" | "strcmp_ge",
            ) => &[0, 1],
            ("xj_scanf", "scanf_str" | "sscanf" | "bscanf") => &[0, 1],
            ("xj_scanf", "parse_format" | "scanf") => &[0],
            ("xj_scanf", "scanf_core" | "brscanf") => &[1],
            (
                "xj_zstr",
                "strlen" | "strnlen" | "strdup" | "strndup" | "memchr" | "strchr_idx"
                | "strrchr_idx",
            ) => &[0],
            (
                "xj_zstr",
                "strcmp" | "strncmp" | "memcmp" | "strspn" | "strcspn" | "strstr_idx"
                | "strstr_ptr",
            ) => &[0, 1],
            ("xj_cfmt" | "xj_bytes" | "xj_zstr", "to_c_str") => &[0],
            ("xj_cio", "to_c_str") if segments.len() == 3 && segments[1].ident == "__cfmt" => &[0],
            ("xj_cfmt", "RawBytes") => &[0],
            (
                "xj_zstr",
                "memcpy" | "strcpy" | "strncpy" | "strcat" | "strncat" | "strstr" | "strpbrk"
                | "strstr_mut_ptr",
            ) => &[1],
            _ => return None,
        };
        let cio_adapter =
            namespace == "xj_cio" && segments.len() == 3 && segments[1].ident == "c_abi";
        let qualified_module = segments.len() == 3
            && matches!(
                (namespace, segments[1].ident.to_string().as_str()),
                ("xj_cstat", "c")
                    | ("xj_zstr", "c")
                    | ("xj_ctime", "compat")
                    | ("xj_scanf", "legacy")
            )
            || (namespace == "xj_cio"
                && segments.len() == 3
                && segments[1].ident == "__cfmt"
                && name == "to_c_str");
        if !cio_adapter && segments.len() != 2 && !qualified_module {
            return None;
        }
        let mut result = call.clone();
        for &index in input_indices {
            if namespace == "xj_cio"
                && index == 0
                && matches!(name.as_str(), "perror_" | "freopen_")
            {
                if let Expr::Call(optional) = unparen(call.args.get(index)?)
                    && optional.args.len() == 1
                    && let Expr::Path(function) = &*optional.func
                    && function.qself.is_none()
                    && (function.path.is_ident("Some")
                        || (function.path.segments.len() == 4
                            && matches!(
                                function.path.segments[0].ident.to_string().as_str(),
                                "std" | "core"
                            )
                            && function.path.segments[1].ident == "option"
                            && function.path.segments[2].ident == "Option"
                            && function.path.segments[3].ident == "Some"))
                    && function
                        .path
                        .segments
                        .iter()
                        .all(|segment| segment.arguments.is_none())
                    && let Some(literal) = prefer_string_literal(&optional.args[0])
                {
                    let mut optional = optional.clone();
                    optional.args[0] = literal;
                    result.args[index] = Expr::Call(optional);
                }
                continue;
            }
            let input = self.coerce_byte_view_input_with_os(
                call.args.get(index)?,
                symbols,
                matches!(namespace, "xj_cio" | "xj_cstat"),
            );
            if let Some(input) = input {
                result.args[index] = *input;
            } else if cio_adapter {
                return None;
            }
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
        if let Some(literal) = prefer_string_literal(expr) {
            return Some(Box::new(literal));
        }
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
