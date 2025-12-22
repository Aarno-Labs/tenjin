use super::*;
use proc_macro2::Literal;
use quote::ToTokens; // for to_token_stream()
use std::str::FromStr;
use syn::{AngleBracketedGenericArguments, Expr, GenericArgument, Path, Type};

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
        let parts: Vec<&str> = self.pretty.splitn(4, " ").collect();
        match *parts.as_slice() {
            ["&", rest] => rest,
            ["&", life, rest] if life.starts_with("'") => rest,
            ["&", life, "mut", rest] if life.starts_with("'") => rest,
            ["&", "mut", rest] => rest,
            ["&", "mut", _, _] => self.pretty[6..].as_ref(),
            ["&", _, _, _] => self.pretty[2..].as_ref(),
            _ => self.pretty.as_ref(),
        }
    }

    pub fn is_borrow(&self) -> bool {
        self.pretty.starts_with('&')
    }

    pub fn is_exclusive_borrow(&self) -> bool {
        let parts: Vec<&str> = self.pretty.splitn(4, " ").collect();
        match *parts.as_slice() {
            ["&", _] => false,
            ["&", life, _] if life.starts_with("'") => false,
            ["&", life, "mut", _] if life.starts_with("'") => true,
            ["&", "mut", _] => true,
            ["&", "mut", _, _] => true,
            ["&", _, _, _] => false,
            _ => false,
        }
    }

    pub fn is_shared_borrow(&self) -> bool {
        self.is_borrow() && !self.is_exclusive_borrow()
    }
}

pub fn is_known_size_1_type(ty: &Type) -> bool {
    match ty {
        Type::Path(path) => path.qself.is_none() && is_known_size_1_path(&path.path),
        _ => false,
    }
}

pub fn path_get_1_segment(path: &Path) -> Option<&PathSegment> {
    if path.segments.len() == 1 {
        Some(&path.segments[0])
    } else {
        None
    }
}

pub fn segment_get_1_bracket_argument(seg: &PathSegment) -> Option<&GenericArgument> {
    match &seg.arguments {
        syn::PathArguments::AngleBracketed(AngleBracketedGenericArguments { args, .. }) => {
            args.first().or(args.last())
        }

        _ => None,
    }
}

pub fn path_get_1_ident(path: &Path) -> Option<&Ident> {
    path_get_1_segment(path).map(|s| &s.ident)
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
    // TODO-TENJIN: expand this list
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

pub fn type_is_vec(ty: &Type) -> bool {
    if let Some(path) = type_get_bare_path(ty) {
        return is_path_exactly_1(path, "Vec");
    }
    false
}

pub fn type_is_mut_ref(ty: &Type) -> bool {
    if let Type::Reference(ref tref) = *ty {
        return tref.mutability.is_some();
    }
    false
}

fn type_try_arraylike_element(t: &Type) -> Option<&Type> {
    match t {
        Type::Path(p) => path_get_1_segment(&p.path)
            .and_then(|s| segment_get_1_bracket_argument(&s))
            .and_then(|args| match args {
                GenericArgument::Type(t) => Some(t),
                _ => None,
            }),

        Type::Slice(slice) => Some(&slice.elem),

        Type::Reference(reference) => type_try_arraylike_element(&reference.elem),

        _ => None,
    }
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

pub fn expr_is_lit_str_or_bytes(expr: &Expr) -> bool {
    if let Expr::Lit(ref lit) = *expr {
        return matches!(&lit.lit, syn::Lit::Str(_) | syn::Lit::ByteStr(_));
    }
    false
}

pub fn expr_is_lit_str_only(expr: &Expr) -> bool {
    if let Expr::Lit(ref lit) = *expr {
        if let syn::Lit::Str(_) = lit.lit {
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

pub fn expr_in_usize(expr: Box<Expr>) -> Box<Expr> {
    use crate::translator::mk;
    let cast_box = mk().cast_expr(expr, mk().path_ty(vec!["usize"]));
    // If we end up with a cast of a literal, we can elide the outer cast.
    if let Expr::Cast(ref cast) = *cast_box {
        if let Expr::Lit(ref elit) = *cast.expr {
            if let syn::Lit::Int(ref lit_int) = elit.lit {
                // If the literal is small enough, we can elide the cast
                if lit_int.base10_parse::<usize>().is_ok() {
                    return cast.expr.clone();
                }
            }
        }
    }
    cast_box
}

fn to_char_lossy(expr: Box<Expr>) -> Box<Expr> {
    // Converts an expression of integral type to char, using lossy conversion.
    // This is appropriate for calls to tolower() and similar functions,
    // which in C accept an int argument that must either be EOF or
    // representable as an unsigned char.
    let u8_ty = mk().path_ty(vec!["u8"]);
    let char_ty = mk().path_ty(vec!["char"]);
    mk().cast_expr(mk().cast_expr(expr, u8_ty), char_ty)
}

pub fn cast_expr_guided(
    e: Box<Expr>,
    t: Box<Type>,
    guided_type: &Option<tenjin::GuidedType>,
) -> Box<Expr> {
    if let Some(guided_type) = guided_type {
        if guided_type.pretty == "char" {
            // If we want a char and have a character literal, we don't need a cast.
            if tenjin::expr_is_lit_char(&e) {
                return e;
            }
            // Otherwise, we need to get a char either via 'as u8 as char'
            // or via 'char::from_u32(...).unwrap()'. For now, we'll limit ourselves
            // to doing the former.
            return to_char_lossy(e);
        }
        return mk().cast_expr(e, Box::new(guided_type.parsed.clone()));
    }
    mk().cast_expr(e, t)
}

/// This is called from a context that looks like *((T*) ...)
/// so if the ... looks like &FOO, where T is int/float and FOO has type float/int,
/// then the overall expression is a bitcast between int and float.
pub fn is_bitcast_to_int_or_float(
    t: &Translation,
    argkind: &CExprKind,
) -> Option<(CTypeKind, CExprId)> {
    if let CExprKind::ExplicitCast(outer_cqt, exp, CastKind::BitCast, _opt_field_id, _lrvalue) =
        argkind
    {
        if let CExprKind::Unary(_inner_cqt, c_ast::UnOp::AddressOf, inner_exp, _lrval) =
            t.ast_context[*exp].kind
        {
            // TENJIN-TODO(intsizes): be more robust about determining actual int sizes
            let outer_tykind = &t.ast_context.resolve_type(outer_cqt.ctype).kind;
            let outer_ty = if let CTypeKind::Pointer(pointee) = outer_tykind {
                &t.ast_context.resolve_type(pointee.ctype).kind
            } else {
                outer_tykind
            };
            // If the outer and inner types aren't the same bitwidth, it would correspond to code like
            //            *(double *)&some_short_var
            // or         *(float  *)&some_u64_var
            // and we're OK for now with having code like that produce a Rust compilation error.
            if matches!(
                outer_ty,
                CTypeKind::Double | CTypeKind::Float | CTypeKind::LongLong | CTypeKind::ULongLong
            ) {
                return Some((outer_ty.clone(), inner_exp));
            }
        }
    }
    None
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
fn libz_rs_sys_call_form_cases(t: &Translation, func: &Expr) -> Option<RecognizedCallForm> {
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
                return Some(RecognizedCallForm::RetargetedCallee(
                    mk().path_expr(vec!["libz_rs_sys", &ident]),
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
            return Some(RecognizedCallForm::RetargetedCallee(
                mk().path_expr("crc32_zz"),
            ));
        }
    }
    None
}

#[allow(clippy::borrowed_box)]
fn recognize_scanf_and_fscanf_of_stdin(
    t: &Translation,
    func: &Expr,
    args: &[Box<Expr>],
    cargs: &[CExprId],
) -> Option<RecognizedCallForm> {
    // TENJIN-TODO: extract helper method for mostly-duplicated code below
    if tenjin::expr_is_ident(func, "scanf") && args.len() > 1 {
        let cargs_after_fmt = cargs[1..].to_vec();
        if cargs_after_fmt
            .iter()
            .all(|&carg| t.c_expr_get_addr_of(carg).is_some())
        {
            // If all arguments are address-taken, we can use the scanf macro.
            return Some(RecognizedCallForm::ScanfAddrTaken(
                cargs[0],
                cargs_after_fmt,
            ));
        }
    }

    // TENJIN-SHORTCOMINGS:
    // - fscanf of non-stdin streams
    // - arguments that are not address-of expressions
    if tenjin::expr_is_ident(func, "fscanf") && args.len() > 2 && tenjin::expr_is_stdin(&args[0]) {
        let cargs_after_fmt = cargs[2..].to_vec();
        if cargs_after_fmt
            .iter()
            .all(|&carg| t.c_expr_get_addr_of(carg).is_some())
        {
            // If all arguments are address-taken, we can use the scanf macro.
            return Some(RecognizedCallForm::ScanfAddrTaken(
                cargs[1],
                cargs_after_fmt,
            ));
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

enum SizeofArgSituation {
    BareSizeof(Option<CExprId>, CTypeId),
    ExprTimesSizeof(CExprId, Option<CExprId>, CTypeId),
    Unrecognized,
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

    fn split_mul_by_sizeof(&self, expr: CExprId) -> SizeofArgSituation {
        // Maps:
        //   (   E      * sizeof(T)) or
        //   (sizeof(T) *     E)     ==> (E, None, T)
        //   (   E      * sizeof(V)) ==> (E, Some(V), typeof(V))

        let get_sizeof =
            |t: &Translation<'_>, expr: CExprId| -> Option<(Option<CExprId>, CTypeId)> {
                let kind = &t.ast_context.index(expr).kind;
                if let CExprKind::UnaryType(_cqt1, UnTypeOp::SizeOf, mb_expr_id, cqt2) = kind {
                    Some((*mb_expr_id, cqt2.ctype))
                } else {
                    None
                }
            };

        if let CExprKind::Binary(_cq, c_ast::BinOp::Multiply, lhs, rhs, _opt_cq_lhs, _opt_cq_rhs) =
            self.ast_context.index(expr).kind
        {
            if let Some((mb_expr_id, typ)) = get_sizeof(self, lhs) {
                return SizeofArgSituation::ExprTimesSizeof(rhs, mb_expr_id, typ);
            }

            if let Some((mb_expr_id, typ)) = get_sizeof(self, rhs) {
                return SizeofArgSituation::ExprTimesSizeof(lhs, mb_expr_id, typ);
            }
        }

        if let Some((mb_expr_id, typ)) = get_sizeof(self, expr) {
            return SizeofArgSituation::BareSizeof(mb_expr_id, typ);
        }

        SizeofArgSituation::Unrecognized
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
                                let tgt_expr = self.convert_expr(
                                    ctx,
                                    self.c_strip_implicit_casts(*raw_base),
                                    None,
                                )?;
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
        func: &Expr,
        args: &[Box<Expr>],
        cargs: &[CExprId],
        _ctx: ExprContext,
    ) -> RecognizedCallForm {
        if tenjin::expr_is_ident(func, "printf")
            && !args.is_empty()
            && tenjin::expr_is_lit_str_or_bytes(tenjin::expr_strip_casts(&args[0]))
        {
            return RecognizedCallForm::PrintfOut { fmt_string_idx: 0 };
        }

        if tenjin::expr_is_ident(func, "snprintf")
            && args.len() >= 3
            && tenjin::expr_is_lit_str_or_bytes(tenjin::expr_strip_casts(&args[2]))
            && self
                .parsed_guidance
                .borrow_mut()
                .query_expr_type(self, cargs[0])
                .is_some_and(|g| g.pretty_sans_refs() == "Vec < u8 >")
        {
            return RecognizedCallForm::PrintfS {
                fmt_string_idx: 2,
                opt_size: Some(args[1].clone()),
                dest: args[0].clone(),
            };
        }

        if tenjin::expr_is_ident(func, "sprintf")
            && args.len() >= 2
            && tenjin::expr_is_lit_str_or_bytes(tenjin::expr_strip_casts(&args[1]))
            && self
                .parsed_guidance
                .borrow_mut()
                .query_expr_type(self, cargs[0])
                .is_some_and(|g| g.pretty_sans_refs() == "Vec < u8 >")
        {
            return RecognizedCallForm::PrintfS {
                fmt_string_idx: 1,
                opt_size: None,
                dest: args[0].clone(),
            };
        }

        if tenjin::expr_is_ident(func, "fprintf")
            && args.len() >= 2
            && tenjin::expr_is_lit_str_or_bytes(tenjin::expr_strip_casts(&args[1]))
        {
            if tenjin::expr_is_stderr(&args[0]) {
                return RecognizedCallForm::PrintfErr { fmt_string_idx: 1 };
            }
            if tenjin::expr_is_stdout(&args[0]) {
                return RecognizedCallForm::PrintfOut { fmt_string_idx: 1 };
            }
        }

        if tenjin::expr_is_ident(func, "abort") && args.is_empty() {
            // TENJIN-TODO: guidance to allow mapping `abort()` to `panic!()`?
            return RecognizedCallForm::RetargetedCallee(
                mk().path_expr(vec!["std", "process", "abort"]),
            );
        }

        if let Some(call_form) = recognize_scanf_and_fscanf_of_stdin(self, func, args, cargs) {
            return call_form;
        }

        if let Some(call_form) = libz_rs_sys_call_form_cases(self, func) {
            return call_form;
        }

        RecognizedCallForm::OtherCall
    }

    #[allow(clippy::vec_box)]
    pub fn convert_call_with_args(
        &self,
        ctx: ExprContext,
        call_expr_ty: CQualTypeId,
        override_ty: Option<CQualTypeId>,
        func: Box<Expr>,
        args: Vec<Box<Expr>>,
        cargs: &[CExprId],
    ) -> TranslationResult<Box<Expr>> {
        let mk_call_with = |func: Box<Expr>, args: Vec<Box<Expr>>| {
            let mut call_expr = mk().call_expr(func, args);

            if let Some(expected_ty) = override_ty {
                if call_expr_ty != expected_ty {
                    let ret_ty = self.convert_type(expected_ty.ctype)?;
                    call_expr = mk().cast_expr(call_expr, ret_ty);
                }
            }

            let res: TranslationResult<_> = Ok(call_expr);
            res
        };
        match self.call_form_cases(&func, &args, cargs, ctx) {
            RecognizedCallForm::PrintfOut { fmt_string_idx } => {
                let fmt_string_span = self
                    .ast_context
                    .display_loc(&self.ast_context[cargs[fmt_string_idx]].loc);
                Ok(mk().mac_expr(refactor_format::build_format_macro(
                    self,
                    "print",
                    "println",
                    &args[fmt_string_idx..],
                    &cargs[fmt_string_idx..],
                    None,
                    fmt_string_span,
                )))
            }
            RecognizedCallForm::PrintfErr { fmt_string_idx } => {
                let fmt_string_span = self
                    .ast_context
                    .display_loc(&self.ast_context[cargs[fmt_string_idx]].loc);
                Ok(mk().mac_expr(refactor_format::build_format_macro(
                    self,
                    "eprint",
                    "eprintln",
                    &args[fmt_string_idx..],
                    &cargs[fmt_string_idx..],
                    None,
                    fmt_string_span,
                )))
            }
            RecognizedCallForm::PrintfS {
                fmt_string_idx,
                opt_size,
                dest,
            } => {
                let fmt_string_span = self
                    .ast_context
                    .display_loc(&self.ast_context[cargs[fmt_string_idx]].loc);
                let formatted_string = mk().mac_expr(refactor_format::build_format_macro(
                    self,
                    "format",
                    "format",
                    &args[fmt_string_idx..],
                    &cargs[fmt_string_idx..],
                    None,
                    fmt_string_span,
                ));
                let size_expr = if let Some(size_expr) = opt_size {
                    mk().call_expr(mk().path_expr(vec!["Some"]), vec![size_expr])
                } else {
                    mk().path_expr(vec!["None"])
                };

                self.with_cur_file_item_store(|item_store| {
                    item_store.add_item_str_once("fn xj_sprintf_Vec_u8(dest: &mut Vec<u8>, lim: Option<usize>, val: String) -> usize {
                        if lim == Some(0) { return 0; }
                        let bytes = val.as_bytes();
                        // We copy at most lim-1 bytes, to leave room for a NUL terminator.
                        let to_copy = if let Some(lim) = lim { std::cmp::min(lim - 1, bytes.len()) } else { bytes.len() };
                        dest.clear();
                        dest.extend_from_slice(&bytes[..to_copy]);
                        to_copy
                    }",
                    );
                });

                Ok(mk().call_expr(
                    mk().path_expr(vec!["xj_sprintf_Vec_u8"]),
                    vec![mk().mutbl().addr_of_expr(dest), size_expr, formatted_string],
                ))
            }
            RecognizedCallForm::ScanfAddrTaken(fmt_arg, cargs) => {
                let mut args_tts = Vec::new();

                let mut need_direct_fmt_expr = true;
                // TODO: Simplify this when we can use let chains.
                if let Some(fmt_lit) = self.c_expr_get_str_lit_bytes(fmt_arg) {
                    if let Ok(fmt_str) = std::str::from_utf8(&fmt_lit) {
                        args_tts.push(TokenTree::Literal(Literal::string(fmt_str)));
                        need_direct_fmt_expr = false;
                    }
                }

                if need_direct_fmt_expr {
                    let fmt: WithStmts<Box<Expr>> = self.convert_expr(ctx, fmt_arg, None)?;
                    args_tts.push(TokenTree::Group(proc_macro2::Group::new(
                        proc_macro2::Delimiter::None,
                        fmt.into_value().to_token_stream(),
                    )));
                }

                for carg in cargs {
                    let un_addr = self.c_expr_get_addr_of(carg).unwrap();
                    let arg = self.convert_expr(ctx, un_addr, None)?;
                    let addr_mut_arg = arg.map(|arg| mk().mutbl().addr_of_expr(arg));
                    args_tts.push(TokenTree::Punct(Punct::new(',', Alone)));
                    args_tts.push(TokenTree::Group(proc_macro2::Group::new(
                        proc_macro2::Delimiter::None,
                        addr_mut_arg.into_value().to_token_stream(),
                    )));
                }

                self.use_crate(ExternCrate::XjScanf);
                self.with_cur_file_item_store(|item_store| {
                    item_store.add_use(false, vec!["xj_scanf".into()], "scanf");
                });

                let scanf_call = mk().mac_expr(mk().mac(
                    mk().path("scanf"),
                    args_tts,
                    MacroDelimiter::Paren(Default::default()),
                ));
                Ok(scanf_call)
            }
            RecognizedCallForm::RetargetedCallee(func) => mk_call_with(func, args),
            RecognizedCallForm::OtherCall => mk_call_with(func, args),
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
                _ if tenjin::is_path_exactly_1(path, "isalnum") => {
                    self.recognize_ctype_is_1(ctx, "isalnum", "c.is_ascii_alphanumeric()", cargs)
                }
                _ if tenjin::is_path_exactly_1(path, "isalpha") => {
                    self.recognize_ctype_is_1(ctx, "isalpha", "c.is_ascii_alphabetic()", cargs)
                }
                _ if tenjin::is_path_exactly_1(path, "islower") => {
                    self.recognize_ctype_is_1(ctx, "islower", "c.is_ascii_lowercase()", cargs)
                }
                _ if tenjin::is_path_exactly_1(path, "isupper") => {
                    self.recognize_ctype_is_1(ctx, "isupper", "c.is_ascii_uppercase()", cargs)
                }
                _ if tenjin::is_path_exactly_1(path, "isdigit") => {
                    self.recognize_ctype_is_1(ctx, "isdigit", "c.is_ascii_digit()", cargs)
                }
                _ if tenjin::is_path_exactly_1(path, "isxdigit") => {
                    self.recognize_ctype_is_1(ctx, "isxdigit", "c.is_ascii_hexdigit()", cargs)
                }
                _ if tenjin::is_path_exactly_1(path, "iscntrl") => {
                    self.recognize_ctype_is_1(ctx, "iscntrl", "c.is_ascii_control()", cargs)
                }
                _ if tenjin::is_path_exactly_1(path, "isgraph") => {
                    self.recognize_ctype_is_1(ctx, "isgraph", "c.is_ascii_graphic()", cargs)
                }
                _ if tenjin::is_path_exactly_1(path, "isspace") => self.recognize_ctype_is_1(
                    ctx,
                    "isspace",
                    "(c.is_ascii_whitespace() || c == '\\x0b')",
                    cargs,
                ),
                _ if tenjin::is_path_exactly_1(path, "isprint") => self.recognize_ctype_is_1(
                    ctx,
                    "isprint",
                    "(c.is_ascii_graphic() || c == ' ')",
                    cargs,
                ),
                _ if tenjin::is_path_exactly_1(path, "ispunct") => {
                    self.recognize_ctype_is_1(ctx, "ispunct", "c.is_ascii_punctuation()", cargs)
                }
                _ if tenjin::is_path_exactly_1(path, "isblank") => {
                    self.recognize_ctype_is_1(ctx, "isblank", "(c == ' ' || c == '\\t')", cargs)
                }
                _ if tenjin::is_path_exactly_1(path, "iswprint") => {
                    self.recognize_preconversion_call_iswprint(ctx, func, cargs)
                }
                _ if tenjin::is_path_exactly_1(path, "tolower") => {
                    self.recognize_preconversion_call_tolower_guided(ctx, func, cargs)
                }
                _ if tenjin::is_path_exactly_1(path, "toupper") => {
                    self.recognize_preconversion_call_toupper_guided(ctx, func, cargs)
                }
                _ if tenjin::is_path_exactly_1(path, "toascii") => {
                    self.recognize_preconversion_call_toascii_guided(ctx, func, cargs)
                }
                _ if (tenjin::is_path_exactly_1(path, "fmin")
                    || tenjin::is_path_exactly_1(path, "fminf")
                    || tenjin::is_path_exactly_1(path, "fminl")) =>
                {
                    self.recognize_preconversion_call_method_2_guided(ctx, "min", cargs)
                }
                _ if (tenjin::is_path_exactly_1(path, "fmax")
                    || tenjin::is_path_exactly_1(path, "fmaxf")
                    || tenjin::is_path_exactly_1(path, "fmaxl")) =>
                {
                    self.recognize_preconversion_call_method_2_guided(ctx, "max", cargs)
                }
                _ if self.parsed_guidance.borrow().no_math_errno
                    && (tenjin::is_path_exactly_1(path, "atan2")
                        || tenjin::is_path_exactly_1(path, "atan2f")
                        || tenjin::is_path_exactly_1(path, "atan2l")) =>
                {
                    self.recognize_preconversion_call_method_2_guided(ctx, "atan2", cargs)
                }
                _ if self.parsed_guidance.borrow().no_math_errno
                    && (
                        /*tenjin::is_path_exactly_1(path, "log") // this one probably needs disambiguation
                        ||*/
                        tenjin::is_path_exactly_1(path, "logf")
                            || tenjin::is_path_exactly_1(path, "logl")
                    ) =>
                {
                    self.recognize_preconversion_call_method_1_guided(ctx, "ln", cargs)
                }
                _ if self.parsed_guidance.borrow().no_math_errno
                    && (tenjin::is_path_exactly_1(path, "hypot")
                        || tenjin::is_path_exactly_1(path, "hypotf")
                        || tenjin::is_path_exactly_1(path, "hypotl")) =>
                {
                    self.recognize_preconversion_call_method_2_guided(ctx, "hypot", cargs)
                }
                _ if self.parsed_guidance.borrow().no_math_errno
                    && (tenjin::is_path_exactly_1(path, "copysign")
                        || tenjin::is_path_exactly_1(path, "copysignf")
                        || tenjin::is_path_exactly_1(path, "copysignl")) =>
                {
                    self.recognize_preconversion_call_method_2_guided(ctx, "copysign", cargs)
                }
                _ if tenjin::is_path_exactly_1(path, "memset") => {
                    self.recognize_preconversion_call_memset_zero_guided(ctx, func, cargs)
                }
                _ if self.parsed_guidance.borrow().no_math_errno
                    && (tenjin::is_path_exactly_1(path, "pow")
                        || tenjin::is_path_exactly_1(path, "powf")
                        || tenjin::is_path_exactly_1(path, "powl")) =>
                {
                    self.recognize_preconversion_call_method_2_guided(ctx, "powf", cargs)
                }
                _ if self.parsed_guidance.borrow().no_math_errno
                    && (tenjin::is_path_exactly_1(path, "fmod")
                        || tenjin::is_path_exactly_1(path, "fmodf")
                        || tenjin::is_path_exactly_1(path, "fmodl")) =>
                {
                    self.recognize_preconversion_call_fmodf_guided(ctx, cargs)
                }
                /* This requires Rust 1.77, which snapshots do not yet use. */
                /*
                _ if (tenjin::is_path_exactly_1(path, "nearbyint")
                    || tenjin::is_path_exactly_1(path, "nearbyintf")
                    || tenjin::is_path_exactly_1(path, "nearbyintl")) =>
                {
                    self.recognize_preconversion_call_method_1_guided(ctx, "round_ties_even", cargs)
                }
                */
                // We don't yet handle rintf, which may raise the `inexact` floating-point exception.
                _ if self
                    .determine_libc_math_1_mapping(ctx, path, cargs)
                    .is_some() =>
                {
                    let Some(method) = self.determine_libc_math_1_mapping(ctx, path, cargs) else {
                        return Ok(None);
                    };
                    self.recognize_preconversion_call_method_1_guided(ctx, method, cargs)
                }
                _ => Ok(None),
            }
        } else {
            Ok(None)
        }
    }

    #[allow(clippy::borrowed_box)]
    fn determine_libc_math_1_mapping(
        &self,
        _ctx: ExprContext,
        path: &Path,
        cargs: &[CExprId],
    ) -> Option<&'static str> {
        if cargs.len() != 1 {
            return None;
        }

        // TODO: ensure type of argument is floating point

        if let Some(ident) = tenjin::path_get_1_ident(path) {
            let name = ident.to_string();
            match name.as_str() {
                "fabs" | "fabsf" | "fabsl" => Some("abs"),
                "floor" | "floorf" | "floorl" => Some("floor"),
                "ceil" | "ceilf" | "ceill" => Some("ceil"),
                "round" | "roundf" | "roundl" => Some("round"),
                "trunc" | "truncf" | "truncl" => Some("trunc"),
                _ if self.parsed_guidance.borrow().no_math_errno => match name.as_str() {
                    "sin" | "sinf" | "sinl" => Some("sin"),
                    "cos" | "cosf" | "cosl" => Some("cos"),
                    "tan" | "tanf" | "tanl" => Some("tan"),
                    "asin" | "asinf" | "asinl" => Some("asin"),
                    "acos" | "acosf" | "acosl" => Some("acos"),
                    "atan" | "atanf" | "atanl" => Some("atan"),
                    "cosh" | "coshf" | "coshl" => Some("cosh"),
                    "sinh" | "sinhf" | "sinhl" => Some("sinh"),
                    "tanh" | "tanhf" | "tanhl" => Some("tanh"),
                    "acosh" | "acoshf" | "acoshl" => Some("acosh"),
                    "asinh" | "asinhf" | "asinhl" => Some("asinh"),
                    "atanh" | "atanhf" | "atanhl" => Some("atanh"),
                    "exp" | "expf" | "expl" => Some("exp"),
                    "exp2" | "exp2f" | "exp2l" => Some("exp2"),
                    "expm1" | "expm1f" | "expm1l" => Some("exp_m1"),
                    "log2" | "log2f" | "log2l" => Some("log2"),
                    "log10" | "log10f" | "log10l" => Some("log10"),
                    "log1p" | "log1pf" | "log1pl" => Some("ln_1p"),
                    "sqrt" | "sqrtf" | "sqrtl" => Some("sqrt"),
                    "cbrt" | "cbrtf" | "cbrtl" => Some("cbrt"),
                    _ => None,
                },
                _ => None,
            }
        } else {
            None
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
    fn recognize_preconversion_call_method_1_guided(
        &self,
        ctx: ExprContext,
        method_name: &str,
        cargs: &[CExprId],
    ) -> TranslationResult<Option<WithStmts<Box<Expr>>>> {
        if cargs.len() == 1 {
            let expr_x = self.convert_expr(ctx.used(), cargs[0], None)?;
            return expr_x
                .and_then(|expr_x| {
                    Ok(WithStmts::new_val(mk().method_call_expr(
                        expr_x,
                        method_name,
                        Vec::new(),
                    )))
                })
                .map(Some);
        }

        Ok(None)
    }

    #[allow(clippy::borrowed_box)]
    fn recognize_preconversion_call_method_2_guided(
        &self,
        ctx: ExprContext,
        method_name: &str,
        cargs: &[CExprId],
    ) -> TranslationResult<Option<WithStmts<Box<Expr>>>> {
        if cargs.len() == 2 {
            let expr_x = self.convert_expr(ctx.used(), cargs[0], None)?;
            let expr_y = self.convert_expr(ctx.used(), cargs[1], None)?;
            return expr_x
                .and_then(|expr_x| {
                    Ok(WithStmts::new_val(mk().method_call_expr(
                        expr_x,
                        method_name,
                        vec![expr_y.to_expr()],
                    )))
                })
                .map(Some);
        }

        Ok(None)
    }

    #[allow(clippy::borrowed_box)]
    fn recognize_preconversion_call_fmodf_guided(
        &self,
        ctx: ExprContext,
        cargs: &[CExprId],
    ) -> TranslationResult<Option<WithStmts<Box<Expr>>>> {
        if cargs.len() == 2 {
            // fmodf(x, y)
            //    when we've been provided with no_math_errno guidance,
            //    or have otherwise ascertained that the program cannot
            //    observe any modifications to errno,
            // should be translated to
            // x % y
            let expr_x = self.convert_expr(ctx.used(), cargs[0], None)?;
            let expr_y = self.convert_expr(ctx.used(), cargs[1], None)?;
            return expr_x
                .and_then(|expr_x| {
                    Ok(WithStmts::new_val(mk().binary_expr(
                        BinOp::Rem(Default::default()),
                        expr_x,
                        expr_y.to_expr(),
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
                    let expr = self.convert_expr(ctx.used(), cargs[0], None)?;
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
                    let expr = self.convert_expr(ctx.used(), cargs[0], None)?;
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

                    let expr_foo = self.convert_expr(ctx.used(), cargs[0], None)?;
                    let expr_bar = self.convert_expr(ctx.used(), cargs[1], None)?;
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
    fn recognize_ctype_is_1(
        &self,
        ctx: ExprContext,
        c_fn_name: &str,
        rust_char_impl: &str,
        cargs: &[CExprId],
    ) -> TranslationResult<Option<WithStmts<Box<Expr>>>> {
        if cargs.len() == 1 {
            if let Some(var_cdecl_id_foo) = self.c_expr_get_var_decl_id(cargs[0]) {
                if self
                    .parsed_guidance
                    .borrow_mut()
                    .query_decl_type(self, var_cdecl_id_foo)
                    .is_some_and(|g| g.pretty == "char")
                {
                    let rust_helper_name = format!("{}_char_i", c_fn_name);
                    self.with_cur_file_item_store(|item_store| {
                        item_store.add_item_str_once(&format!(
                            "fn {}(c: char) -> core::ffi::c_int {{ ({}) as core::ffi::c_int }}",
                            rust_helper_name, rust_char_impl
                        ));
                    });

                    let expr_foo = self.convert_expr(ctx.used(), cargs[0], None)?;
                    let bare_foo: Box<Expr> =
                        Box::new(tenjin::expr_strip_casts(&(expr_foo.to_expr())).clone());
                    let call =
                        mk().call_expr(mk().path_expr(vec![&rust_helper_name]), vec![bare_foo]);
                    return Ok(Some(WithStmts::new_val(call)));
                }
            }

            // Fallthrough: no guidance, or expr was not a simple variable.
            let rust_helper_name = format!("xj_{}", c_fn_name);
            self.with_cur_file_item_store(|item_store| {
                item_store.add_item_str_once(&format!(
                    "fn {}(c: core::ffi::c_int) -> core::ffi::c_int {{ if c == -1 {{ 0 }} else {{ let c = c as u8 as char; ({}) as core::ffi::c_int }} }}",
                    rust_helper_name, rust_char_impl
                ));
            });

            let expr_foo = self.convert_expr(ctx.used(), cargs[0], None)?;
            let call = mk().call_expr(
                mk().path_expr(vec![&rust_helper_name]),
                vec![expr_foo.to_expr()],
            );
            return Ok(Some(WithStmts::new_val(call)));
        }

        Ok(None)
    }

    #[allow(clippy::borrowed_box)]
    fn recognize_preconversion_call_iswprint(
        &self,
        ctx: ExprContext,
        func: &Box<Expr>,
        cargs: &[CExprId],
    ) -> TranslationResult<Option<WithStmts<Box<Expr>>>> {
        if tenjin::expr_is_ident(func, "iswprint") && cargs.len() == 1 {
            self.with_cur_file_item_store(|item_store| {
                item_store.add_item_str_once(
                    "fn xj_iswprint(wc: u32) -> core::ffi::c_int {
                                if wc < 0 { return 0; }
                                match std::char::from_u32(wc) {
                                    Some(ch) if !ch.is_control() => 1,
                                    _ => 0,
                                }
                            }",
                );
            });

            let expr_foo = self.convert_expr(ctx.used(), cargs[0], None)?;
            let iswprint_call = mk().call_expr(
                mk().path_expr(vec!["xj_iswprint"]),
                vec![mk().cast_expr(expr_foo.to_expr(), mk().path_ty(mk().path("u32")))],
            );
            return Ok(Some(WithStmts::new_val(iswprint_call)));
        }

        Ok(None)
    }

    #[allow(clippy::borrowed_box)]
    fn recognize_preconversion_call_tolower_guided(
        &self,
        ctx: ExprContext,
        func: &Box<Expr>,
        cargs: &[CExprId],
    ) -> TranslationResult<Option<WithStmts<Box<Expr>>>> {
        if tenjin::expr_is_ident(func, "tolower") && cargs.len() == 1 {
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
                            "fn tolower_char_i(c: char) -> core::ffi::c_int { c.to_ascii_lowercase() as core::ffi::c_int }",
                        );
                    });

                    let expr_foo = self.convert_expr(ctx.used(), cargs[0], None)?;
                    // Stripping casts is correct because we know the underlying type is char,
                    // which matches the argument of the function we're redirecting to.
                    let bare_foo: Box<Expr> =
                        Box::new(tenjin::expr_strip_casts(&(expr_foo.to_expr())).clone());
                    let tolower_call =
                        mk().call_expr(mk().path_expr(vec!["tolower_char_i"]), vec![bare_foo]);
                    return Ok(Some(WithStmts::new_val(tolower_call)));
                }
            }
            // Fallthrough: no guidance, or expr was not a simple variable.

            self.with_cur_file_item_store(|item_store| {
                    item_store.add_item_str_once(
                        "fn xj_tolower(c: core::ffi::c_int) -> core::ffi::c_int { if c == -1 { -1 } else { (c as u8 as char).to_ascii_lowercase() as core::ffi::c_int } }",
                    );
                });

            let expr_foo = self.convert_expr(ctx.used(), cargs[0], None)?;
            let tolower_call =
                mk().call_expr(mk().path_expr(vec!["xj_tolower"]), vec![expr_foo.to_expr()]);
            return Ok(Some(WithStmts::new_val(tolower_call)));
        }

        Ok(None)
    }

    #[allow(clippy::borrowed_box)]
    fn recognize_preconversion_call_toupper_guided(
        &self,
        ctx: ExprContext,
        func: &Box<Expr>,
        cargs: &[CExprId],
    ) -> TranslationResult<Option<WithStmts<Box<Expr>>>> {
        if tenjin::expr_is_ident(func, "toupper") && cargs.len() == 1 {
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
                            "fn toupper_char_i(c: char) -> core::ffi::c_int { c.to_ascii_uppercase() as core::ffi::c_int }",
                        );
                    });

                    let expr_foo = self.convert_expr(ctx.used(), cargs[0], None)?;
                    // Stripping casts is correct because we know the underlying type is char,
                    // which matches the argument of the function we're redirecting to.
                    let bare_foo: Box<Expr> =
                        Box::new(tenjin::expr_strip_casts(&(expr_foo.to_expr())).clone());
                    let toupper_call =
                        mk().call_expr(mk().path_expr(vec!["toupper_char_i"]), vec![bare_foo]);
                    return Ok(Some(WithStmts::new_val(toupper_call)));
                }
            }
            // Fallthrough: no guidance, or expr was not a simple variable.

            self.with_cur_file_item_store(|item_store| {
                    item_store.add_item_str_once(
                        "fn xj_toupper(c: core::ffi::c_int) -> core::ffi::c_int { if c == -1 { -1 } else { (c as u8 as char).to_ascii_uppercase() as core::ffi::c_int } }",
                    );
                });

            let expr_foo = self.convert_expr(ctx.used(), cargs[0], None)?;
            let toupper_call =
                mk().call_expr(mk().path_expr(vec!["xj_toupper"]), vec![expr_foo.to_expr()]);
            return Ok(Some(WithStmts::new_val(toupper_call)));
        }

        Ok(None)
    }

    #[allow(clippy::borrowed_box)]
    fn recognize_preconversion_call_toascii_guided(
        &self,
        ctx: ExprContext,
        func: &Box<Expr>,
        cargs: &[CExprId],
    ) -> TranslationResult<Option<WithStmts<Box<Expr>>>> {
        if tenjin::expr_is_ident(func, "toascii") && cargs.len() == 1 {
            if let Some(var_cdecl_id_foo) = self.c_expr_get_var_decl_id(cargs[0]) {
                if self
                    .parsed_guidance
                    .borrow_mut()
                    .query_decl_type(self, var_cdecl_id_foo)
                    .is_some_and(|g| g.pretty == "char")
                {
                    self.with_cur_file_item_store(|item_store| {
                        item_store.add_item_str_once(
                            "fn toascii_char_i(c: char) -> core::ffi::c_int { char::from_u32((c as u32) & 0x7f).unwrap() as core::ffi::c_int }",
                        );
                    });

                    let expr_foo = self.convert_expr(ctx.used(), cargs[0], None)?;
                    // Stripping casts is correct because we know the underlying type is char,
                    // which matches the argument of the function we're redirecting to.
                    let bare_foo: Box<Expr> =
                        Box::new(tenjin::expr_strip_casts(&(expr_foo.to_expr())).clone());
                    let toascii_call =
                        mk().call_expr(mk().path_expr(vec!["toascii_char_i"]), vec![bare_foo]);
                    return Ok(Some(WithStmts::new_val(toascii_call)));
                }
            }
            // Fallthrough: no guidance, or expr was not a simple variable.

            self.with_cur_file_item_store(|item_store| {
                item_store.add_item_str_once(
                    "fn xj_toascii(c: core::ffi::c_int) -> core::ffi::c_int { c & 0x7f }",
                );
            });

            let expr_foo = self.convert_expr(ctx.used(), cargs[0], None)?;
            let toascii_call =
                mk().call_expr(mk().path_expr(vec!["xj_toascii"]), vec![expr_foo.to_expr()]);
            return Ok(Some(WithStmts::new_val(toascii_call)));
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
                        item_store.add_use(true, vec!["std".into(), "io".into()], "Read");
                        item_store.add_use(true, vec!["std".into(), "io".into()], "BufRead");
                        item_store.add_item_str_once(
                            "fn fgets_stdin_bool(buf: &mut String, limit: u64) -> bool {
                            let handle = ::std::io::stdin().lock();
                            let res = handle.take(limit - 1).read_line(buf);
                            res.is_ok() && res.unwrap() > 0
                        }",
                        );
                    });

                    let buf = self.convert_expr(ctx.used(), cargs[0], None)?;
                    let lim = self.convert_expr(ctx.used(), cargs[1], None)?;
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

    #[allow(clippy::borrowed_box)]
    fn recognize_preconversion_call_memset_zero_guided(
        &self,
        ctx: ExprContext,
        func: &Box<Expr>,
        cargs: &[CExprId],
    ) -> TranslationResult<Option<WithStmts<Box<Expr>>>> {
        if tenjin::expr_is_ident(func, "memset") && cargs.len() == 3 {
            // memset(DST, 0, NUM)
            //    when DST is guided to be of type Vec<X> (where type X has size Y)
            //    and NUM is of the form   E * sizeof(T)  (where type T has size Y)
            //                        or       sizeof DST (where DST is an array in C w/ E elts)
            //    and we know that the all-zero-bytes representation of X is V
            // should be translated to
            // dst[..E].fill(V);
            if !self.is_integral_lit(cargs[1], 0) {
                return Ok(None);
            }

            let arg0_sans_casts = self.c_strip_implicit_casts(cargs[0]);

            let mb_dst_guided_type = self
                .parsed_guidance
                .borrow_mut()
                .query_expr_type(self, arg0_sans_casts);
            if let Some(dst_guided_type) = mb_dst_guided_type {
                if !dst_guided_type.pretty_sans_refs().starts_with("Vec <") {
                    return Ok(None);
                }

                // pull out vec element type from dst_guided_type.parsed
                let _elt_type = match dst_guided_type.parsed {
                    syn::Type::Path(ref type_path) => {
                        if let Some(seg) = type_path.path.segments.last() {
                            if seg.ident == "Vec" {
                                if let syn::PathArguments::AngleBracketed(ref args) = seg.arguments
                                {
                                    if let Some(syn::GenericArgument::Type(ref ty)) =
                                        args.args.first()
                                    {
                                        ty.clone()
                                    } else {
                                        return Ok(None);
                                    }
                                } else {
                                    return Ok(None);
                                }
                            } else {
                                return Ok(None);
                            }
                        } else {
                            return Ok(None);
                        }
                    }
                    _ => {
                        return Ok(None);
                    }
                };

                let handle_sizeof =
                    |elt_count: Box<Expr>| -> TranslationResult<Option<WithStmts<Box<Expr>>>> {
                        let expr_dst = self.convert_expr(ctx.used(), arg0_sans_casts, None)?;

                        let slice = mk().index_expr(
                            expr_dst.to_expr(),
                            mk().range_expr(None, Some(tenjin::expr_in_usize(elt_count))),
                        );

                        // TODO(brk) - determine the Rust-side all-zero-bytes representation for non-primitive types.
                        // TODO(brk) - or use bytemuck?
                        let zero_value = mk().lit_expr(mk().int_unsuffixed_lit(0));
                        let fill_call = mk().method_call_expr(slice, "fill", vec![zero_value]);

                        Ok(Some(WithStmts::new_val(fill_call)))
                    };

                match self.split_mul_by_sizeof(cargs[2]) {
                    SizeofArgSituation::ExprTimesSizeof(
                        elt_count_cexpr,
                        mb_sized_expr,
                        elt_type,
                    ) => {
                        if self.memset_args_translate_plainly(
                            &arg0_sans_casts,
                            &mb_sized_expr,
                            &elt_type,
                        ) {
                            let elt_count = self.convert_expr(ctx.used(), elt_count_cexpr, None)?;
                            return handle_sizeof(elt_count.to_expr());
                        }
                    }
                    SizeofArgSituation::BareSizeof(mb_sized_expr, elt_type) => {
                        if self.memset_args_translate_plainly(
                            &arg0_sans_casts,
                            &mb_sized_expr,
                            &elt_type,
                        ) {
                            let elt_count = mk().lit_expr(mk().int_unsuffixed_lit(1));
                            return handle_sizeof(elt_count);
                        }
                    }
                    SizeofArgSituation::Unrecognized => {
                        log::trace!(
                            "memset zero recognition found mul-by-sizeof but the args seem wonky"
                        );
                    }
                }
            }
        }

        Ok(None)
    }

    fn memset_args_translate_plainly(
        &self,
        dst: &CExprId,
        sized_expr: &Option<CExprId>,
        elt_type: &CTypeId,
    ) -> bool {
        // For `memset(DST, 0, E * sizeof(T))` it's OK as long as sizeof(T) == sizeof(*DST), which we currently
        // approximate by requiring that the resolved types are identical.
        if sized_expr.is_none() && Some(*elt_type) == self.ast_context[*dst].kind.get_type() {
            return true;
        }

        // We can only proceed if the user wrote something sensible like memset(DST, 0, E * sizeof(*DST)).
        // For `memset(PTR, 0, E * sizeof(PTR))` we'd need to know the relationship between the target pointer size
        // and the size of the pointee type.
        if let Some(sized_expr) = sized_expr {
            match &self.ast_context[*sized_expr].kind {
                CExprKind::Unary(_, c_ast::UnOp::Deref, inner, _lrvalue) => {
                    self.c_expr_decl_id(*inner) == self.c_expr_decl_id(*dst)
                }
                CExprKind::ArraySubscript(_cqt, base, _idx, _lrval) => {
                    log::warn!(
                        "memset_args_translate_plainly: sized_expr is ArraySubscript, cqt={:?}",
                        _cqt
                    );
                    self.c_expr_decl_id(*base) == self.c_expr_decl_id(*dst)
                }
                _ => false,
            }
        } else {
            false
        }
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

    pub fn try_compute_guided_type(&self, expr: CExprId) -> Option<Type> {
        match self.ast_context[self.c_strip_noop_casts(expr)].kind {
            CExprKind::DeclRef(_, decl_id, _) => {
                self.ast_context
                    .get_decl(&decl_id)
                    .and_then(|decl| match decl.kind {
                        CDeclKind::Variable { .. } => self
                            .parsed_guidance
                            .borrow_mut()
                            .query_decl_type(self, decl_id)
                            .map(|gt| gt.parsed),
                        _ => None,
                    })
            }

            CExprKind::ArraySubscript(_, subexpr, _index, _lrvalue) => self
                .try_compute_guided_type(subexpr)
                .as_ref()
                .and_then(|ty| type_try_arraylike_element(ty))
                .map(|ty| ty.clone()),

            _ => None,
        }
    }

    pub fn should_subscript(&self, c_ptr: CExprId) -> bool {
        self.try_compute_guided_type(c_ptr)
            .as_ref()
            .and_then(|ty| type_try_arraylike_element(ty))
            .is_some()
    }
}
