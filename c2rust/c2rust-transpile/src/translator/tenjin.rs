use super::*;
use proc_macro2::Literal;
use quote::ToTokens; // for to_token_stream()
use std::str::FromStr;
use syn::{Expr, Path, Type};

#[derive(Debug, Clone)]
pub struct GuidedType {
    pub pretty: String,
    pub parsed: Type,
}

impl FromStr for GuidedType {
    type Err = syn::parse::Error;

    fn from_str(pretty: &str) -> Result<Self, Self::Err> {
        let parsed = syn::parse_str(pretty)?;
        Ok(GuidedType {
            pretty: pretty.to_string(),
            parsed,
        })
    }
}

impl GuidedType {
    pub fn new(pretty: String, parsed: Type) -> Self {
        GuidedType { pretty, parsed }
    }

    pub fn from_type(ty: Type) -> Self {
        GuidedType {
            pretty: quote::quote!(#ty).to_string(),
            parsed: ty,
        }
    }

    pub fn pretty_sans_refs(&self) -> &str {
        self.pretty
            .trim_start_matches("& ")
            .trim_start_matches("mut ")
    }
}

pub fn is_known_size_1_type(ty: &Type) -> bool {
    match ty {
        Type::Path(path) => path.qself.is_none() && is_known_size_1_path(&path.path),
        _ => false,
    }
}

pub fn is_path_exactly_1(path: &Path, a: &str) -> bool {
    if path.segments.len() == 1 {
        path.segments[0].ident.to_string().as_str() == a
    } else {
        false
    }
}

fn is_path_exactly_2(path: &Path, a: &str, b: &str) -> bool {
    if path.segments.len() == 2 {
        path.segments[0].ident.to_string().as_str() == a
            && path.segments[1].ident.to_string().as_str() == b
    } else {
        false
    }
}

fn is_known_size_1_path(path: &Path) -> bool {
    match path.segments.len() {
        1 => matches!(
            path.segments[0].ident.to_string().as_str(),
            "u8" | "i8" | "bool" | "char"
        ),
        2 => is_path_exactly_2(path, "libc", "c_char"),
        _ => false,
    }
}

pub fn type_get_bare_path(ty: &Type) -> Option<&Path> {
    if let Type::Path(ref path) = *ty {
        if path.qself.is_none() {
            return Some(&path.path);
        }
    }
    None
}

pub fn expr_get_path(expr: &Expr) -> Option<&Path> {
    if let Expr::Path(ref path) = *expr {
        Some(&path.path)
    } else {
        None
    }
}

pub fn expr_is_ident(expr: &Expr, ident: &str) -> bool {
    if let Expr::Path(ref path) = *expr {
        is_path_exactly_1(&path.path, ident)
    } else {
        false
    }
}

pub fn expr_get_ident(expr: &Expr) -> Option<String> {
    if let Expr::Path(ref path) = *expr {
        if path.qself.is_none() && path.path.segments.len() == 1 {
            return Some(path.path.segments[0].ident.to_string());
        }
    }
    None
}

pub fn expr_is_stdout(expr: &Expr) -> bool {
    tenjin::expr_is_ident(expr, "stdout") || tenjin::expr_is_ident(expr, "__stdoutp")
}

pub fn expr_is_stderr(expr: &Expr) -> bool {
    tenjin::expr_is_ident(expr, "stderr") || tenjin::expr_is_ident(expr, "__stderrp")
}

pub fn expr_is_stdin(expr: &Expr) -> bool {
    tenjin::expr_is_ident(expr, "stdin") || tenjin::expr_is_ident(expr, "__stdinp")
}

pub fn expr_is_lit_char(expr: &Expr) -> bool {
    if let Expr::Lit(ref lit) = *expr {
        if let syn::Lit::Char(_) = lit.lit {
            return true;
        }
    }
    false
}

pub fn expr_strip_casts(expr: &Expr) -> &Expr {
    let mut ep = expr;
    loop {
        match ep {
            Expr::Cast(ExprCast { expr, .. }) => ep = expr,
            Expr::Type(ExprType { expr, .. }) => ep = expr,
            _ => break ep,
        }
    }
}

/// The given expression is being used in a context expecting a u64 value.
/// Builder::cast_expr() will strip value-preserving casts. Because we know
/// the type imposed by the context, we can also entirely elide casts of
/// compatible integer literals.
/// Examples with expr = x of type i16, and with the implicit `as u64` written explicitly:
///      (x as i8)    as u64 => (x as i8) as u64     # inner cast may change value
///      (x as i32)   as u64 => x as u64             # inner cast is value-preserving
///      (1000 as i8) as u64 => (1000 as i8) as u64  # with large literal, inner cast is not value-preserving
///      (10 as i8)   as u64 => 10                   # with small literal, inner cast is value-preserving
///      1000 as u64         => 1000                 # outer cast is made redundant by known context
pub fn expr_in_u64(expr: Box<Expr>) -> Box<Expr> {
    use crate::translator::mk;
    let cast_box = mk().cast_expr(expr, mk().path_ty(vec!["u64"]));
    // If we end up with a cast of a literal, we can elide the outer cast.
    if let Expr::Cast(ref cast) = *cast_box {
        if let Expr::Lit(ref elit) = *cast.expr {
            if let syn::Lit::Int(ref lit_int) = elit.lit {
                // If the literal is small enough, we can elide the cast
                if lit_int.base10_parse::<u64>().is_ok() {
                    return cast.expr.clone();
                }
            }
        }
    }
    cast_box
}

pub fn cast_expr_guided(
    e: Box<Expr>,
    t: Box<Type>,
    guided_type: &Option<tenjin::GuidedType>,
) -> Box<Expr> {
    if let Some(guided_type) = guided_type {
        // If we want a char and have a character literal, we don't need a cast.
        if guided_type.pretty == "char" && tenjin::expr_is_lit_char(&e) {
            return e;
        }
    }
    mk().cast_expr(e, t)
}

pub fn guide_type_name_path(pg: &ParsedGuidance, name: &str) -> Path {
    if pg.using_crates.contains("libz-rs-sys") {
        if name == "internal_state" || name == "gz_header" {
            return mk().path(vec!["libz_rs_sys", name]);
        }
        // zlib has `typedef struct z_stream_s { ... } z_stream;`
        // and libz-rs-sys names the struct z_stream, not z_stream_s.
        if name == "z_stream_s" {
            return mk().path(vec!["libz_rs_sys", "z_stream"]);
        }
    }
    // Fallback to the original name
    mk().path(vec![name.to_string()])
}

#[allow(clippy::borrowed_box)]
fn libz_rs_sys_call_form_cases(
    t: &Translation,
    func: &Box<Expr>,
    args: &[Box<Expr>],
) -> Option<RecognizedCallForm> {
    fn libz_rs_sys_fn_pred(ident: &str) -> bool {
        // See https://docs.rs/libz-sys/latest/libz_sys/
        matches!(
            ident,
            "adler32"
                | "adler32_combine"
                | "compress"
                | "compress2"
                | "compressBound"
                | "crc32"
                | "crc32_combine"
                | "deflate"
                | "deflateBound"
                | "deflateCopy"
                | "deflateEnd"
                | "deflateInit2_"
                | "deflateInit_"
                | "deflateParams"
                | "deflatePrime"
                | "deflateReset"
                | "deflateSetDictionary"
                | "deflateSetHeader"
                | "deflateTune"
                | "gzclearerr"
                | "gzclose"
                | "gzdirect"
                | "gzdopen"
                | "gzeof"
                | "gzerror"
                | "gzflush"
                | "gzgetc"
                | "gzgets"
                | "gzopen"
                | "gzputc"
                | "gzputs"
                | "gzread"
                | "gzrewind"
                | "gzseek"
                | "gzsetparams"
                | "gztell"
                | "gzungetc"
                | "gzwrite"
                | "inflate"
                | "inflateBack"
                | "inflateBackEnd"
                | "inflateBackInit_"
                | "inflateCopy"
                | "inflateEnd"
                | "inflateGetHeader"
                | "inflateInit2_"
                | "inflateInit_"
                | "inflateMark"
                | "inflatePrime"
                | "inflateReset"
                | "inflateReset2"
                | "inflateSetDictionary"
                | "inflateSync"
                | "uncompress"
                | "zlibCompileFlags"
                | "zlibVersion"
        )
    }

    if t.parsed_guidance
        .borrow()
        .using_crates
        .contains("libz-rs-sys")
    {
        if let Some(ident) = expr_get_ident(func) {
            if libz_rs_sys_fn_pred(&ident) {
                t.use_crate(ExternCrate::LibzRsSys);
                return Some(RecognizedCallForm::OtherCall(
                    mk().path_expr(vec!["libz_rs_sys", &ident]),
                    args.to_owned(),
                ));
            }
        }

        if tenjin::expr_is_ident(func, "crc32_z") {
            // libz_rs_sys does not provide crc32_z, so we'll emulate it.
            t.with_cur_file_item_store(|item_store| {
                item_store.add_item_str_once(
                    r#"unsafe fn crc32_zz(crc: libc::c_ulong, buf: *const Bytef, len: libc::c_ulong) -> libc::c_ulong { libz_rs_sys::crc32(crc, buf, libz_rs_sys::uInt::try_from(len).expect("crc32_z overflow")) }"#,
                );
            });
            return Some(RecognizedCallForm::OtherCall(
                mk().path_expr("crc32_zz"),
                args.to_owned(),
            ));
        }
    }
    None
}

#[allow(clippy::borrowed_box)]
fn recognize_scanf_and_fscanf_of_stdin(
    t: &Translation,
    func: &Box<Expr>,
    args: &[Box<Expr>],
    cargs: &[CExprId],
    ctx: &ExprContext,
) -> Option<RecognizedCallForm> {
    if ctx.is_used() {
        // We currently only translate to the scanf::scanf! macro, which
        // returns Result<(), _>. Eventually, we should find or create a
        // more faithful implementation which tracks assignments and can
        // thus compute the same integer return codes as scanf & fscanf.
        return None;
    }

    // TENJIN-TODO: extract helper method for mostly-duplicated code below
    if tenjin::expr_is_ident(func, "scanf") && args.len() > 1 {
        if let Some(fmt_raw) = cargs
            .first()
            .and_then(|&carg| t.c_expr_get_str_lit_bytes(carg))
        {
            let fmt = String::from_utf8_lossy(&fmt_raw);
            match tenjin_scanf::parse_scanf_format(&fmt) {
                Ok(directives) => {
                    if directives.iter().all(tenjin_scanf::directive_is_simple) {
                        let cargs_after_fmt = cargs[1..].to_vec();
                        if cargs_after_fmt
                            .iter()
                            .all(|&carg| t.c_expr_get_addr_of(carg).is_some())
                        {
                            // If all directives are simple, and all arguments are address-taken,
                            // we can use the scanf macro.
                            return Some(RecognizedCallForm::ScanfAddrTaken(
                                directives,
                                cargs_after_fmt,
                            ));
                        }
                    }
                }
                Err(e) => {
                    log::warn!("TENJIN: Failed to parse scanf format: {}", e);
                }
            }
        }
    }

    if tenjin::expr_is_ident(func, "fscanf") && args.len() > 2 && tenjin::expr_is_stdin(&args[0]) {
        if let Some(fmt_raw) = cargs
            .get(1)
            .and_then(|carg| t.c_expr_get_str_lit_bytes(*carg))
        {
            let fmt = String::from_utf8_lossy(&fmt_raw);
            match tenjin_scanf::parse_scanf_format(&fmt) {
                Ok(directives) => {
                    if directives.iter().all(tenjin_scanf::directive_is_simple) {
                        let cargs_after_fmt = cargs[2..].to_vec();
                        if cargs_after_fmt
                            .iter()
                            .all(|&carg| t.c_expr_get_addr_of(carg).is_some())
                        {
                            // If all directives are simple, and all arguments are address-taken,
                            // we can use the scanf macro.
                            return Some(RecognizedCallForm::ScanfAddrTaken(
                                directives,
                                cargs_after_fmt,
                            ));
                        } else {
                            log::trace!("TENJIN: fscanf arguments not all address-taken");
                        }
                    } else {
                        log::trace!("TENJIN: fscanf directives are not all simple");
                    }
                    // TENJIN-SHORTCOMINGS:
                    // - fscanf of non-stdin streams
                    // - format strings with non-trivial directives (including char/string inputs)
                    // - arguments that are not address-of expressions
                }
                Err(e) => {
                    log::warn!("TENJIN: Failed to parse fscanf format: {}", e);
                }
            }
        } else {
            log::trace!("TENJIN: fscanf fmt wasn't str lit");
        }
    }
    None
}

#[allow(clippy::vec_box)]
fn mac_call_exprs_tt(args: Vec<Box<Expr>>) -> TokenStream {
    let mut tokens = TokenStream::new();
    let mut first = true;
    for arg in args {
        if !first {
            tokens.extend(vec![TokenTree::Punct(Punct::new(',', Alone))]);
        }
        tokens.extend(arg.to_token_stream());
        first = false;
    }
    tokens
}

impl Translation<'_> {
    pub fn recognize_c_assignment_cases(
        &self,
        ctx: ExprContext,
        op: c_ast::BinOp,
        qtype: CQualTypeId,
        lhs: CExprId,
        rhs: CExprId,
        compute_type: Option<CQualTypeId>,
        result_type: Option<CQualTypeId>,
    ) -> TranslationResult<Option<WithStmts<Box<Expr>>>> {
        self.recognize_c_assignment_string_pop(ctx, op, qtype, lhs, rhs, compute_type, result_type)
    }

    fn strip_integral_cast(&self, expr: CExprId) -> CExprId {
        let kind = &self.ast_context.index(expr).kind;
        if let CExprKind::ImplicitCast(_, inner, CastKind::IntegralCast, _, _) = kind {
            *inner
        } else {
            expr
        }
    }

    pub fn strip_implicit_array_to_pointer_cast(&self, expr: CExprId) -> CExprId {
        let kind = &self.ast_context.index(expr).kind;
        if let CExprKind::ImplicitCast(_, inner, CastKind::ArrayToPointerDecay, _, _) = kind {
            *inner
        } else {
            expr
        }
    }

    fn is_integral_lit(&self, expr: CExprId, val: u64) -> bool {
        let kind = &self.ast_context.index(expr).kind;
        if let CExprKind::Literal(_, clit) = kind {
            match clit {
                CLiteral::Integer(value, _base) => *value == val,
                CLiteral::Character(value) => *value == val,
                _ => false,
            }
        } else {
            false
        }
    }

    pub fn get_string_lit(&self, expr: CExprId) -> Option<&CLiteral> {
        let kind = &self.ast_context.index(expr).kind;
        if let CExprKind::Literal(_, clit) = kind {
            match clit {
                CLiteral::String(_, _) => Some(clit),
                _ => None,
            }
        } else {
            None
        }
    }

    fn c_expr_decl_id(&self, expr: CExprId) -> Option<CDeclId> {
        let kind = &self
            .ast_context
            .index(self.c_strip_implicit_casts(expr))
            .kind;
        if let CExprKind::DeclRef(_, decl_id, _) = kind {
            Some(*decl_id)
        } else {
            None
        }
    }

    fn recognize_c_assignment_string_pop(
        &self,
        ctx: ExprContext,
        op: c_ast::BinOp,
        _qtype: CQualTypeId,
        lhs: CExprId,
        rhs: CExprId,
        _compute_type: Option<CQualTypeId>,
        _result_type: Option<CQualTypeId>,
    ) -> TranslationResult<Option<WithStmts<Box<Expr>>>> {
        // Code matching
        //     s1[strlen(s1)-1] = '\0';
        //         when s1 is guided to be a String
        // can be translated to
        //     s1.pop();
        // (assuming the assignment expr is in a statement position)
        log::trace!("Recognizing C assignment string pop?...");
        if op != c_ast::BinOp::Assign {
            log::trace!("Not an assignment operator");
            return Ok(None);
        }

        if !self.is_integral_lit(self.strip_integral_cast(rhs), 0) {
            log::trace!("Not assigned an integral literal 0");
            return Ok(None);
        }

        let lhs_kind = &self.ast_context.index(lhs).kind;
        if let CExprKind::ArraySubscript(_, raw_base, index, _lrvalue) = lhs_kind {
            if let CExprKind::Binary(
                _sub_cq,
                c_ast::BinOp::Subtract,
                sub_lhs,
                sub_rhs,
                _opt_cq_lhs,
                _opt_cq_rhs,
            ) = self.ast_context.index(*index).kind
            {
                if !self.is_integral_lit(self.strip_integral_cast(sub_rhs), 1) {
                    log::trace!("Not subtracting 1 from the index");
                    return Ok(None);
                }

                if let Some(base_decl_id) = self.c_expr_decl_id(*raw_base) {
                    let guided_type = self
                        .parsed_guidance
                        .borrow_mut()
                        .query_decl_type(self, base_decl_id);

                    if guided_type.is_none_or(|g| g.pretty != "String") {
                        log::trace!("target variable not guided to be of type String");
                        return Ok(None);
                    }
                } else {
                    return Ok(None);
                };

                if let CExprKind::Call(_, callee_raw, args) = &self.ast_context.index(sub_lhs).kind
                {
                    let callee = self.c_strip_implicit_casts(*callee_raw);
                    if let CExprKind::DeclRef(_, callee_decl_id, _opt_cq) =
                        self.ast_context.index(callee).kind
                    {
                        if let CDeclKind::Function { name, .. } =
                            &self.ast_context[callee_decl_id].kind
                        {
                            if name != "strlen" {
                                return Ok(None);
                            }

                            if args.len() == 1
                                && self.c_expr_decl_id(args[0]) == self.c_expr_decl_id(*raw_base)
                            {
                                let tgt_expr =
                                    self.convert_expr(ctx, self.c_strip_implicit_casts(*raw_base))?;
                                let pop_call = mk().method_call_expr(
                                    tgt_expr.to_expr(),
                                    mk().path_segment("pop"),
                                    vec![],
                                );
                                log::trace!("Recognized assignment, returning pop call");
                                return Ok(Some(WithStmts::new_val(pop_call)));
                            } else {
                                log::trace!("Call to strlen does not match the expected argument");
                            }
                        }
                        log::trace!("Not a call to strlen");
                    } else {
                        log::trace!(
                            "Callee is not a decl ref: {:?}",
                            self.ast_context.index(callee).kind
                        );
                    }
                }
            }
        }
        log::trace!("LHS not an array subscript");
        Ok(None)
    }

    #[allow(clippy::vec_box)]
    fn call_form_cases(
        &self,
        func: Box<Expr>,
        args: Vec<Box<Expr>>,
        cargs: &[CExprId],
        ctx: ExprContext,
    ) -> RecognizedCallForm {
        if tenjin::expr_is_ident(&func, "printf") {
            return RecognizedCallForm::PrintfOut(args, cargs[0]);
        }

        if tenjin::expr_is_ident(&func, "fprintf") && !args.is_empty() {
            if tenjin::expr_is_stderr(&args[0]) {
                return RecognizedCallForm::PrintfErr(args[1..].to_vec(), cargs[1]);
            }
            if tenjin::expr_is_stdout(&args[0]) {
                return RecognizedCallForm::PrintfOut(args[1..].to_vec(), cargs[1]);
            }
        }

        if tenjin::expr_is_ident(&func, "abort") && args.is_empty() {
            // TENJIN-TODO: guidance to allow mapping `abort()` to `panic!()`?
            return RecognizedCallForm::OtherCall(
                mk().path_expr(vec!["std", "process", "abort"]),
                args,
            );
        }

        if let Some(call_form) =
            recognize_scanf_and_fscanf_of_stdin(self, &func, &args, cargs, &ctx)
        {
            return call_form;
        }

        if let Some(call_form) = libz_rs_sys_call_form_cases(self, &func, &args) {
            return call_form;
        }

        RecognizedCallForm::OtherCall(func, args)
    }

    #[allow(clippy::vec_box)]
    pub fn convert_call_with_args(
        &self,
        ctx: ExprContext,
        _call_type_id: CTypeId,
        func: Box<Expr>,
        args: Vec<Box<Expr>>,
        cargs: &[CExprId],
    ) -> TranslationResult<Box<Expr>> {
        match self.call_form_cases(func, args, cargs, ctx) {
            RecognizedCallForm::PrintfOut(args, fmt_carg) => {
                let fmt_string_span = self
                    .ast_context
                    .display_loc(&self.ast_context[fmt_carg].loc);
                Ok(mk().mac_expr(refactor_format::build_format_macro(
                    self,
                    "print",
                    "println",
                    &args,
                    cargs,
                    None,
                    fmt_string_span,
                )))
            }
            RecognizedCallForm::PrintfErr(args, fmt_carg) => {
                let fmt_string_span = self
                    .ast_context
                    .display_loc(&self.ast_context[fmt_carg].loc);
                Ok(mk().mac_expr(refactor_format::build_format_macro(
                    self,
                    "eprint",
                    "eprintln",
                    &args,
                    cargs,
                    None,
                    fmt_string_span,
                )))
            }
            RecognizedCallForm::ScanfAddrTaken(all_directives, cargs) => {
                // If there are more conversion specifications than arguments,
                // that's UB in C, so we're permitted to drop any extras.
                let mut capped_directives = Vec::new();
                let mut seen_conversion_specs = 0;
                for directive in &all_directives {
                    if let tenjin_scanf::Directive::ConversionSpec(_) = directive {
                        if seen_conversion_specs < cargs.len() {
                            capped_directives.push(directive.clone());
                        }

                        seen_conversion_specs += 1;
                    } else {
                        capped_directives.push(directive.clone());
                    }
                }

                let mut scanf_rs_fmt = String::new();
                for directive in &capped_directives {
                    match directive {
                        tenjin_scanf::Directive::ZeroOrMoreWhitespace => {
                            scanf_rs_fmt.push(' ');
                        }
                        tenjin_scanf::Directive::OrdinaryChar(c) => {
                            scanf_rs_fmt.push(*c);
                        }
                        tenjin_scanf::Directive::ConversionSpec(_spec) => {
                            // So far we only get this far if we have simple specs,
                            // which are defined so that an empty format string is
                            // appropriate for the scanf macro.
                            scanf_rs_fmt.push('{');
                            scanf_rs_fmt.push('}');
                        }
                    }
                }

                let mut args_tts = Vec::new();
                args_tts.push(TokenTree::Literal(Literal::string(&scanf_rs_fmt)));
                for carg in cargs {
                    let un_addr = self.c_expr_get_addr_of(carg).unwrap();
                    let arg = self.convert_expr(ctx, un_addr)?;
                    args_tts.push(TokenTree::Punct(Punct::new(',', Alone)));
                    args_tts.push(TokenTree::Group(proc_macro2::Group::new(
                        proc_macro2::Delimiter::None,
                        arg.into_value().to_token_stream(),
                    )));
                }

                self.use_crate(ExternCrate::Scanf);
                self.with_cur_file_item_store(|item_store| {
                    item_store.add_use(vec!["scanf".into()], "scanf");
                });

                let scanf_call = mk().mac_expr(mk().mac(
                    mk().path("scanf"),
                    args_tts,
                    MacroDelimiter::Paren(Default::default()),
                ));
                Ok(mk().method_call_expr(scanf_call, "unwrap", vec![]))
            }
            RecognizedCallForm::OtherCall(func, args) => Ok(mk().call_expr(func, args)),
        }
    }

    #[allow(clippy::borrowed_box)]
    pub fn call_form_cases_preconversion(
        &self,
        call_type_id: CTypeId,
        ctx: ExprContext,
        func: &Box<Expr>,
        cargs: &[CExprId],
    ) -> TranslationResult<Option<WithStmts<Box<Expr>>>> {
        if let Some(path) = tenjin::expr_get_path(func) {
            match () {
                _ if tenjin::is_path_exactly_1(path, "assert") => {
                    self.recognize_preconversion_call_assert(ctx, cargs)
                }
                _ if tenjin::is_path_exactly_1(path, "fputs") => {
                    self.recognize_preconversion_call_fputs_stdout_guided(ctx, func, cargs)
                }
                _ if tenjin::is_path_exactly_1(path, "fgets") => {
                    self.recognize_preconversion_call_fgets_stdin(call_type_id, ctx, func, cargs)
                }
                _ if tenjin::is_path_exactly_1(path, "strlen") => {
                    self.recognize_preconversion_call_strlen_guided(ctx, func, cargs)
                }
                _ if tenjin::is_path_exactly_1(path, "strcspn") => {
                    self.recognize_preconversion_call_strcspn_guided(ctx, func, cargs)
                }
                _ if tenjin::is_path_exactly_1(path, "isblank") => {
                    self.recognize_preconversion_call_isblank_guided(ctx, func, cargs)
                }
                _ if self.parsed_guidance.borrow().no_math_errno
                    && (tenjin::is_path_exactly_1(path, "pow")
                        || tenjin::is_path_exactly_1(path, "powf")
                        || tenjin::is_path_exactly_1(path, "powl")) =>
                {
                    self.recognize_preconversion_call_powf_guided(ctx, cargs)
                }
                _ => Ok(None),
            }
        } else {
            Ok(None)
        }
    }

    #[allow(clippy::borrowed_box)]
    fn recognize_preconversion_call_assert(
        &self,
        ctx: ExprContext,
        cargs: &[CExprId],
    ) -> TranslationResult<Option<WithStmts<Box<Expr>>>> {
        if cargs.len() == 1 {
            // assert(FOO)
            //    when FOO is a simple variable
            // should be translated to
            // assert!(FOO)
            //
            // Note that in C the asserted expression must have integral type,
            // but in Rust the asserted expression is of boolean type. That mismatch
            // is why we recognize this case pre-conversion.
            let expr = self.convert_condition(ctx.used(), true, cargs[0])?;
            return expr
                .and_then(|expr| {
                    Ok(WithStmts::new_val(mk().mac_expr(mk().mac(
                        mk().path("assert"),
                        mac_call_exprs_tt(vec![expr]),
                        MacroDelimiter::Paren(Default::default()),
                    ))))
                })
                .map(Some);
        }

        Ok(None)
    }

    #[allow(clippy::borrowed_box)]
    fn recognize_preconversion_call_powf_guided(
        &self,
        ctx: ExprContext,
        cargs: &[CExprId],
    ) -> TranslationResult<Option<WithStmts<Box<Expr>>>> {
        if cargs.len() == 2 {
            // pow(x, y)
            //    when we've been provided with no_math_errno guidance,
            //    or have otherwise ascertained that the program cannot
            //    observe any modifications to errno,
            // should be translated to
            // x.powf(y)
            let expr_x = self.convert_expr(ctx.used(), cargs[0])?;
            let expr_y = self.convert_expr(ctx.used(), cargs[1])?;
            return expr_x
                .and_then(|expr_x| {
                    Ok(WithStmts::new_val(mk().method_call_expr(
                        expr_x,
                        "powf",
                        vec![expr_y.to_expr()],
                    )))
                })
                .map(Some);
        }

        Ok(None)
    }

    #[allow(clippy::borrowed_box)]
    fn recognize_preconversion_call_fputs_stdout_guided(
        &self,
        ctx: ExprContext,
        func: &Box<Expr>,
        cargs: &[CExprId],
    ) -> TranslationResult<Option<WithStmts<Box<Expr>>>> {
        if tenjin::expr_is_ident(func, "fputs") && cargs.len() == 2 {
            // fputs(FOO, stdout)
            //    when FOO is a simple variable with type String
            // should be translated to
            // println!(FOO)
            if !(self.c_expr_is_var_ident(cargs[1], "stdout")
                || self.c_expr_is_var_ident(cargs[1], "__stdoutp"))
            {
                return Ok(None);
            }

            if let Some(var_cdecl_id) = self.c_expr_get_var_decl_id(cargs[0]) {
                if self
                    .parsed_guidance
                    .borrow_mut()
                    .query_decl_type(self, var_cdecl_id)
                    .is_some_and(|g| g.pretty == "String")
                {
                    let expr = self.convert_expr(ctx.used(), cargs[0])?;
                    let print_call = mk().mac_expr(refactor_format::build_format_macro_from(
                        self,
                        "%s".to_string(),
                        "print",
                        "println",
                        &[expr.to_expr()],
                        &[cargs[0]],
                        None,
                        None,
                        self.ast_context
                            .display_loc(&self.ast_context[cargs[0]].loc),
                    ));
                    return Ok(Some(WithStmts::new_val(print_call)));
                }
            }
        }

        Ok(None)
    }

    #[allow(clippy::borrowed_box)]
    fn recognize_preconversion_call_strlen_guided(
        &self,
        ctx: ExprContext,
        func: &Box<Expr>,
        cargs: &[CExprId],
    ) -> TranslationResult<Option<WithStmts<Box<Expr>>>> {
        if tenjin::expr_is_ident(func, "strlen") && cargs.len() == 1 {
            // strlen(FOO)
            //    when FOO is a simple variable with type String
            // should be translated to
            // FOO.len()
            if let Some(var_cdecl_id) = self.c_expr_get_var_decl_id(cargs[0]) {
                if self
                    .parsed_guidance
                    .borrow_mut()
                    .query_decl_type(self, var_cdecl_id)
                    .is_some_and(|g| g.pretty == "String")
                {
                    let expr = self.convert_expr(ctx.used(), cargs[0])?;
                    let len_call = mk().method_call_expr(expr.to_expr(), "len", vec![]);
                    return Ok(Some(WithStmts::new_val(len_call)));
                }
            }
        }

        Ok(None)
    }

    #[allow(clippy::borrowed_box)]
    fn recognize_preconversion_call_strcspn_guided(
        &self,
        ctx: ExprContext,
        func: &Box<Expr>,
        cargs: &[CExprId],
    ) -> TranslationResult<Option<WithStmts<Box<Expr>>>> {
        if tenjin::expr_is_ident(func, "strcspn") && cargs.len() == 2 {
            // strcspn(FOO, BAR)
            //    when FOO is a simple variable with type String
            //    and BAR is a simple variable with type String
            // should be translated to
            // strcspn_str(&FOO, &BAR)
            if let (Some(var_cdecl_id_foo), Some(var_cdecl_id_bar)) = (
                self.c_expr_get_var_decl_id(cargs[0]),
                self.c_expr_get_var_decl_id(cargs[1]),
            ) {
                if self
                    .parsed_guidance
                    .borrow_mut()
                    .query_decl_type(self, var_cdecl_id_foo)
                    .is_some_and(|g| g.pretty == "String")
                    && self
                        .parsed_guidance
                        .borrow_mut()
                        .query_decl_type(self, var_cdecl_id_bar)
                        .is_some_and(|g| g.pretty == "String")
                {
                    self.with_cur_file_item_store(|item_store| {
                        item_store.add_item_str_once("fn strcspn_str(s: &str, chars: &str) -> usize { s.chars().take_while(|c| !chars.contains(*c)).count() }",
                        );
                    });

                    let expr_foo = self.convert_expr(ctx.used(), cargs[0])?;
                    let expr_bar = self.convert_expr(ctx.used(), cargs[1])?;
                    let strcspn_call = mk().call_expr(
                        mk().path_expr(vec!["strcspn_str"]),
                        vec![
                            mk().addr_of_expr(expr_foo.to_expr()),
                            mk().addr_of_expr(expr_bar.to_expr()),
                        ],
                    );
                    return Ok(Some(WithStmts::new_val(strcspn_call)));
                }
            }
        }

        Ok(None)
    }

    #[allow(clippy::borrowed_box)]
    fn recognize_preconversion_call_isblank_guided(
        &self,
        ctx: ExprContext,
        func: &Box<Expr>,
        cargs: &[CExprId],
    ) -> TranslationResult<Option<WithStmts<Box<Expr>>>> {
        if tenjin::expr_is_ident(func, "isblank") && cargs.len() == 1 {
            // isblank(FOO)
            //    when FOO is a simple variable with type char
            // should be translated to
            // isblank_char(FOO)
            if let Some(var_cdecl_id_foo) = self.c_expr_get_var_decl_id(cargs[0]) {
                if self
                    .parsed_guidance
                    .borrow_mut()
                    .query_decl_type(self, var_cdecl_id_foo)
                    .is_some_and(|g| g.pretty == "char")
                {
                    self.with_cur_file_item_store(|item_store| {
                        // For now we return an integer code rather than a bool,
                        // to better match the C function signature.
                        item_store.add_item_str_once(
                            "fn isblank_char_i(c: char) -> libc::c_int { (c == ' ' || c == '\\t') as libc::c_int }",
                        );
                    });

                    let expr_foo = self.convert_expr(ctx.used(), cargs[0])?;
                    // Stripping casts is correct because we know the underlying type is char,
                    // which matches the argument of the function we're redirecting to.
                    let bare_foo: Box<Expr> =
                        Box::new(tenjin::expr_strip_casts(&(expr_foo.to_expr())).clone());
                    let isblank_call =
                        mk().call_expr(mk().path_expr(vec!["isblank_char_i"]), vec![bare_foo]);
                    return Ok(Some(WithStmts::new_val(isblank_call)));
                }
            }
        }

        Ok(None)
    }

    #[allow(clippy::borrowed_box)]
    fn recognize_preconversion_call_fgets_stdin(
        &self,
        call_type_id: CTypeId,
        ctx: ExprContext,
        func: &Box<Expr>,
        cargs: &[CExprId],
    ) -> TranslationResult<Option<WithStmts<Box<Expr>>>> {
        if tenjin::expr_is_ident(func, "fgets") && cargs.len() == 3 {
            // fgets(FOO, limit_expr, stdin)
            //    when FOO is a simple variable with type String
            // should be translated to
            // fgets_stdin_bool(&mut FOO, limit_expr, io::stdin())
            //
            // where fgets_stdin_bool is a wrapper around
            // io::stdin().lock().take(limit_expr - 1).read_line(&mut FOO)
            //
            // The awkward name reflects that this is not a generally-correct translation,
            // since we're not accounting for code that does anything non-trivial with the
            // return value of fgets(), nor for code that checks
            // errno, etc. But it's a useful strawman for the time being.
            //
            // Because take() expects a u64, we may be able to elide unnecessary casts from limit_expr.
            //
            // One might think the `.take()` is unnecessary, since the C code needed the limit to
            // avoid a memory safety violation. But the limit_expr also serves to place a bound on the
            // period spent blocking on the read. If the stream produces limit+1 bytes then blocks,
            // the fgets() call would return before blocking,
            // and the .take() is what stops Rust from blocking.

            if !(self.c_expr_is_var_ident(cargs[2], "stdin")
                || self.c_expr_is_var_ident(cargs[2], "__stdinp"))
            {
                return Ok(None);
            }

            // XREF:TENJIN-GUIDANCE-STRAWMAN
            if let Some(var_cdecl_id) = self.c_expr_get_var_decl_id(cargs[0]) {
                if self
                    .parsed_guidance
                    .borrow_mut()
                    .query_decl_type(self, var_cdecl_id)
                    .is_some_and(|g| g.pretty == "String")
                {
                    self.with_cur_file_item_store(|item_store| {
                        item_store.add_use(vec!["std".into(), "io".into()], "Read");
                        item_store.add_use(vec!["std".into(), "io".into()], "BufRead");
                        item_store.add_item_str_once(
                            "fn fgets_stdin_bool(buf: &mut String, limit: u64) -> bool {
                            let handle = std::io::stdin().lock();
                            let res = handle.take(limit - 1).read_line(buf);
                            res.is_ok() && res.unwrap() > 0
                        }",
                        );
                    });

                    let buf = self.convert_expr(ctx.used(), cargs[0])?;
                    let lim = self.convert_expr(ctx.used(), cargs[1])?;
                    let fgets_stdin_bool_call = mk().call_expr(
                        mk().path_expr(vec!["fgets_stdin_bool"]),
                        vec![
                            mk().mutbl().addr_of_expr(buf.to_expr()),
                            tenjin::expr_in_u64(lim.to_expr()),
                        ],
                    );

                    self.type_overrides.borrow_mut().insert(
                        call_type_id,
                        GuidedType::from_str("bool").expect("failed to parse 'bool'!?"),
                    );

                    return Ok(Some(WithStmts::new_val(fgets_stdin_bool_call)));
                }
            }
        }
        Ok(None)
    }

    pub fn get_callee_function_arg_guidances(
        &self,
        func_id: CExprId,
    ) -> Option<Vec<Option<tenjin::GuidedType>>> {
        match self.ast_context[func_id].kind {
            CExprKind::ImplicitCast(_, fexp, CastKind::FunctionToPointerDecay, _, _) => {
                match self.ast_context[fexp].kind {
                    CExprKind::DeclRef(_qtyid, fndeclid, _lrvalue) => {
                        match &self.ast_context[fndeclid].kind {
                            CDeclKind::Function { parameters, .. } => Some(
                                parameters
                                    .iter()
                                    .map(|param| {
                                        self.parsed_guidance
                                            .borrow_mut()
                                            .query_decl_type(self, *param)
                                    })
                                    .collect(),
                            ),
                            _ => None,
                        }
                    }
                    _ => None,
                }
            }
            _ => None,
        }
    }
}
