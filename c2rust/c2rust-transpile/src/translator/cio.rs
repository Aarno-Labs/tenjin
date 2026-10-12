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
            | "sscanf"
            | "vprintf"
            | "vfprintf"
            | "vsprintf"
            | "vsnprintf"
            | "vscanf"
            | "vfscanf"
            | "vsscanf"
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

/// Parameters that the ordinary Rust API accepts as borrowed byte views.
fn cio_string_argument(target: &str, index: usize) -> bool {
    matches!(
        (target, index),
        ("fopen" | "popen", 0 | 1)
            | ("fdopen", 1)
            | ("fmemopen", 2)
            | ("freopen", 1)
            | ("remove", 0)
            | ("rename", 0 | 1)
            | ("fputs" | "puts", 0)
    )
}

fn cio_byte_view_type(ty: &Type) -> bool {
    match ty {
        Type::Reference(r) => cio_byte_view_type(&r.elem),
        Type::Array(a) => matches!(&*a.elem, Type::Path(p) if p.path.is_ident("u8")),
        Type::Slice(s) => matches!(&*s.elem, Type::Path(p) if p.path.is_ident("u8")),
        Type::Path(p) if p.qself.is_none() => {
            let Some(last) = p.path.segments.last() else {
                return false;
            };
            if p.path.segments.len() > 1
                && !matches!(
                    p.path.segments[0].ident.to_string().as_str(),
                    "std" | "core" | "alloc" | "xj_zstr"
                )
            {
                return false;
            }
            match last.ident.to_string().as_str() {
                "str" | "String" | "CStr" | "CString" | "ZStr" | "ZString" | "OsStr"
                | "OsString" | "Path" | "PathBuf" => true,
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
                        cio_byte_view_type(inner)
                    }
                }
                _ => false,
            }
        }
        _ => false,
    }
}

/// Clang's canonical function type can lose the public size_t/ssize_t
/// spelling used by the declared API. Shared entry points use Rust's
/// corresponding pointer-sized integers, as translated public typedefs do.
pub(crate) fn cio_function_type(target: &str, mut ty: Box<Type>) -> Box<Type> {
    if let Type::BareFn(function) = &mut *ty {
        let ret = match target {
            "fread" | "fwrite" => Some(syn::parse_quote!(usize)),
            "getline" | "getdelim" => Some(syn::parse_quote!(isize)),
            _ => None,
        };
        if let Some(ret) = ret {
            function.output = ReturnType::Type(Default::default(), Box::new(ret));
        }
    }
    ty
}

fn cio_native_return_kind(target: &str) -> Option<CTypeKind> {
    match target {
        "fread" | "fwrite" => Some(CTypeKind::Size),
        "getline" | "getdelim" => Some(CTypeKind::SSize),
        _ => None,
    }
}

impl Translation<'_> {
    /// Recover ByteView storage without reading through the translated pointer.
    fn cio_byte_view(&self, arg: &Expr, carg: CExprId) -> Option<Box<Expr>> {
        if let Some(bytes) = self.printf_literal(carg) {
            let literal = syn::LitByteStr::new(bytes, Span::call_site());
            return Some(Box::new(syn::parse_quote!(#literal)));
        }
        let guided = self
            .parsed_guidance
            .borrow_mut()
            .query_expr_type(self, carg)?;
        let mut arg = Self::printf_guided_expr(arg);
        // Pointer lowering may have inserted a byte view before as_ptr().
        // Recover the guided storage, which implements ByteView directly.
        while let Expr::MethodCall(call) = arg {
            if !call.args.is_empty()
                || !matches!(
                    call.method.to_string().as_str(),
                    "as_bytes_with_nul" | "as_bytes"
                )
            {
                break;
            }
            arg = Self::printf_guided_expr(&call.receiver);
        }
        if cio_byte_view_type(guided.strip_refs()) {
            Some(Box::new(syn::parse_quote!(&(#arg))))
        } else {
            None
        }
    }

    /// Fixed-signature calls use ordinary Rust functions whenever their
    /// arguments already match. Raw strings and void buffers use the shared
    /// argument adapters; only function references require the C ABI itself.
    #[allow(clippy::vec_box)]
    pub(crate) fn convert_cio_fixed_call(
        &self,
        callee: Option<CDeclId>,
        args: &[Box<Expr>],
        cargs: &[CExprId],
        call_type: CQualTypeId,
    ) -> TranslationResult<Option<(Box<Expr>, CQualTypeId)>> {
        let Some(target) = callee.and_then(|id| self.cio_replacements.functions.get(&id)) else {
            return Ok(None);
        };
        if matches!(
            target.as_str(),
            "printf"
                | "fprintf"
                | "sprintf"
                | "snprintf"
                | "scanf"
                | "fscanf"
                | "sscanf"
                | "vprintf"
                | "vfprintf"
                | "vsprintf"
                | "vsnprintf"
                | "vscanf"
                | "vfscanf"
                | "vsscanf"
        ) {
            return Ok(None);
        }
        let mut views = Vec::with_capacity(args.len());
        let mut adapter = matches!(target.as_str(), "fread" | "fwrite" | "fgetpos" | "fsetpos");
        for (index, arg) in args.iter().enumerate() {
            let view = if cio_string_argument(target, index) {
                self.cio_byte_view(arg, cargs[index])
            } else {
                None
            };
            adapter |= cio_string_argument(target, index) && view.is_none();
            views.push(view);
        }
        let optional_view = if matches!(target.as_str(), "freopen" | "perror") {
            self.cio_byte_view(&args[0], cargs[0])
        } else {
            None
        };
        let mut values: Vec<Box<Expr>> = args
            .iter()
            .zip(views)
            .map(|(arg, view)| match (adapter, view) {
                (false, Some(view)) => view,
                // The ABI adapter needs a stored NUL, even for an unterminated
                // ByteView. This temporary owner lives until the call returns.
                (true, Some(view)) => Box::new(syn::parse_quote!(
                    ::xj_cio::__cfmt::to_c_str(#view).as_ptr().cast()
                )),
                (_, None) => arg.clone(),
            })
            .collect();
        let direct_target = if let Some(view) = optional_view {
            if adapter {
                values[0] = Box::new(syn::parse_quote!(
                    ::xj_cio::__cfmt::to_c_str(#view).as_ptr().cast()
                ));
                target.as_str()
            } else {
                values[0] = Box::new(syn::parse_quote!(Some(#view)));
                if target == "freopen" {
                    "freopen_"
                } else {
                    "perror_"
                }
            }
        } else {
            target.as_str()
        };
        let path = if adapter {
            vec!["xj_cio", "c_abi", target.as_str()]
        } else {
            vec!["xj_cio", direct_target]
        };
        self.use_crate(ExternCrate::XjCio);
        let mut call = mk().call_expr(mk().abs_path_expr(path), values);
        let mut source_type = call_type;
        if let Some(kind) = cio_native_return_kind(target) {
            if let Some(ty) = self.ast_context.try_type_for_kind(&kind) {
                source_type = CQualTypeId::new(ty);
            } else {
                // Without a C node for the shared API's return representation,
                // normalize it to the header type before ordinary C casts.
                call = mk().cast_expr(call, self.convert_type(call_type.ctype)?);
            }
        }
        Ok(Some((call, source_type)))
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
            "fscanf" | "vfscanf" | "sscanf" | "vsscanf" => 1,
            _ => return Ok(None),
        };
        if args.len() <= fmt_idx {
            return Err(TranslationError::generic("missing scanf format"));
        }
        self.use_crate(ExternCrate::XjCio);
        let fmt = &args[fmt_idx];
        let fmt_view = if let Some(view) = self.cio_byte_view(fmt, cargs[fmt_idx]) {
            quote::quote!(#view)
        } else {
            quote::quote!(unsafe { ::core::ffi::CStr::from_ptr((#fmt) as _) })
        };
        // The stream, or the C string input of `sscanf`.
        let prefix = match name.as_str() {
            "sscanf" | "vsscanf" => {
                let input = &args[0];
                match self.cio_byte_view(input, cargs[0]) {
                    Some(view) => quote::quote!(#view,),
                    None => quote::quote!(unsafe { ::core::ffi::CStr::from_ptr((#input) as _) },),
                }
            }
            _ => {
                let prefix_args = &args[..fmt_idx];
                quote::quote!(#(#prefix_args,)*)
            }
        };
        if name.starts_with('v') {
            // The engine's parser decides which pointer to pull from the
            // `va_list` for each directive.
            self.use_crate(ExternCrate::XjCioVariadic);
            self.use_feature("c_variadic");
            let ap = args
                .get(fmt_idx + 1)
                .ok_or_else(|| TranslationError::generic("missing va_list argument"))?;
            let ident = Ident::new(name, Span::call_site());
            return Ok(Some(Box::new(syn::parse_quote! {
                { let xj_cio_fmt = #fmt_view;
                  ::xj_cio::#ident(#prefix xj_cio_fmt, #ap) }
            })));
        }
        let requirements = self.scanf_literal_requirements(name, cargs[fmt_idx]);
        let model = xj_scanf_core::DataModel::for_target(&self.ast_context.target);
        let destinations = &args[fmt_idx + 1..];
        if let Some(reqs) = &requirements {
            if reqs.len() > destinations.len() {
                warn!(
                    "{name}: the format has {} destinations but the call passes {}; the call will stop with EINVAL at the first missing one",
                    reqs.len(),
                    destinations.len()
                );
            }
        }
        let mut values = Vec::new();
        for (index, arg) in destinations.iter().enumerate() {
            let ty = self.ast_context.resolve_type(
                self.ast_context[cargs[fmt_idx + 1 + index]]
                    .kind
                    .get_type()
                    .unwrap(),
            );
            let CTypeKind::Pointer(pointee) = &ty.kind else {
                return Err(TranslationError::generic(
                    "scanf destination is not a pointer",
                ));
            };
            let kind = &self.ast_context.resolve_type(pointee.ctype).kind;
            let requirement = requirements.as_ref().and_then(|r| r.get(index).copied());
            let (variant, slot) = cio_scan_destination(kind, requirement);
            if let (Some(req), Some(slot)) = (requirement, slot) {
                if !req.accepts(slot, &model) {
                    warn!(
                        "{name} argument {}: its {req:?} directive does not fit a {kind:?} destination; the call will stop with EINVAL before that directive reads input",
                        index + 1
                    );
                }
            }
            let variant = Ident::new(variant, Span::call_site());
            values.push(quote::quote!(::xj_cio::OutArg::#variant((#arg) as _)));
        }
        let ident = Ident::new(&format!("{name}_raw"), Span::call_site());
        Ok(Some(Box::new(
            syn::parse2(quote::quote! {
            { let xj_cio_fmt = #fmt_view;
              ::xj_cio::#ident(#prefix xj_cio_fmt, &mut [#(#values),*]) }
            })
            .map_err(|e| format_err!("Invalid cio call: {e}"))?,
        )))
    }

    /// The destinations a literal scanf format requires, in order. A format
    /// the engine will reject is reported here and analyzed no further.
    fn scanf_literal_requirements(
        &self,
        name: &str,
        fmt: CExprId,
    ) -> Option<Vec<xj_scanf_core::Requirement>> {
        let literal = self.printf_literal(fmt)?;
        match xj_scanf_core::Format::requirements(literal).collect() {
            Ok(requirements) => Some(requirements),
            Err(e) => {
                warn!("{name}: {e}; the call will return without reading input, setting EINVAL");
                None
            }
        }
    }
}

/// The raw `OutArg` variant for a scanf destination of C pointee type
/// `kind`, with its class and width for the compatibility rule. Plain `char`
/// takes `hh` integers as `SChar`/`UChar` by the directive's signedness and
/// otherwise names byte storage, as do unknown pointee types.
fn cio_scan_destination(
    kind: &CTypeKind,
    requirement: Option<xj_scanf_core::Requirement>,
) -> (&'static str, Option<xj_scanf_core::Slot>) {
    use xj_scanf_core::{IntKind as K, Requirement, Slot};
    let int = |name, kind| (name, Some(Slot::Int(kind)));
    match kind {
        CTypeKind::Char => match requirement {
            Some(Requirement::Int { signed, .. }) => {
                int(if signed { "SChar" } else { "UChar" }, K::Char)
            }
            Some(Requirement::Count(_)) => int("SChar", K::Char),
            _ => ("Str", Some(Slot::Bytes)),
        },
        CTypeKind::SChar | CTypeKind::Int8 => int("SChar", K::Char),
        CTypeKind::UChar | CTypeKind::UInt8 => int("UChar", K::Char),
        CTypeKind::Short | CTypeKind::Int16 => int("Short", K::Short),
        CTypeKind::UShort | CTypeKind::UInt16 => int("UShort", K::Short),
        CTypeKind::Int | CTypeKind::Int32 => int("Int", K::Int),
        CTypeKind::UInt | CTypeKind::UInt32 => int("UInt", K::Int),
        CTypeKind::Long => int("Long", K::Long),
        CTypeKind::ULong => int("ULong", K::Long),
        CTypeKind::LongLong | CTypeKind::Int64 | CTypeKind::IntMax => int("LongLong", K::LongLong),
        CTypeKind::ULongLong | CTypeKind::UInt64 | CTypeKind::UIntMax => {
            int("ULongLong", K::LongLong)
        }
        CTypeKind::Size | CTypeKind::UIntPtr => int("Size", K::Size),
        CTypeKind::SSize | CTypeKind::PtrDiff | CTypeKind::IntPtr => int("ISize", K::PtrDiff),
        CTypeKind::Float => ("Float", Some(Slot::Float)),
        CTypeKind::Double => ("Double", Some(Slot::Double)),
        _ => ("Str", None),
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
            // Guided C arrays can still appear as synthetic raw addresses of
            // their Rust storage. Borrow that storage for byte consumers.
            Expr::RawAddr(raw) => Self::printf_guided_expr(&raw.expr),
            _ => expr,
        }
    }

    fn printf_literal(&self, expr: CExprId) -> Option<&[u8]> {
        match &self.ast_context[self.c_strip_implicit_casts(expr)].kind {
            CExprKind::Literal(_, CLiteral::String(bytes, 1)) => Some(xj_bytes::c_bytes(bytes)),
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
            || fmt_guided
                .as_ref()
                .is_some_and(|g| cio_byte_view_type(g.strip_refs()));
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
            let guided = guidance
                .as_ref()
                .is_some_and(|g| cio_byte_view_type(g.strip_refs()));
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
                  match ::xj_cio::cformat::Plan::new(::xj_cio::ByteView::view_bytes(#fmt_name)) {
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
