use super::*;
use quote::ToTokens; // for to_token_stream()
use serde_derive::Deserialize;
use serde_json::Map;
use std::collections::HashSet;
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

    pub fn strip_refs(&self) -> &Type {
        type_strip_refs(&self.parsed)
    }

    pub fn is_borrow(&self) -> bool {
        type_is_ref(&self.parsed)
    }

    pub fn is_exclusive_borrow(&self) -> bool {
        type_is_mut_ref(&self.parsed)
    }

    pub fn is_shared_borrow(&self) -> bool {
        self.is_borrow() && !self.is_exclusive_borrow()
    }
}

#[derive(Debug, Clone)]
pub struct FFIConversion {
    pub ins: HashMap<String, FFIInConversion>,
    pub out: Option<FFIOutConversion>,
}

#[derive(Debug, Clone, PartialEq, Eq, PartialOrd, Ord, Deserialize)]
#[serde(tag = "method")]
pub enum FFIInConversion {
    #[serde(rename = "id")]
    Id,

    #[serde(rename = "pipe")]
    Pipeline { conversions: Vec<FFIInConversion> },

    #[serde(rename = "to-ref")]
    // *(const|mut) T -> Option<&(const|mut) T>
    ToRef { mutable: bool },

    #[serde(rename = "unwrap")]
    // Option<T> -> T
    Unwrap,

    #[serde(rename = "pointer-reinterp")]
    // For types that are guaranteed to share the same representation
    // i.e. &T and Option<&T> (for T where the nullable ptr optimization applies)
    PointerCast { ty: String },

    #[serde(rename = "to-slice-via-cstr")]
    ViaCStr {
        #[serde(default)]
        mutable: bool,
        #[serde(default)]
        #[serde(rename = "empty-if-null")]
        empty_if_null: bool,
    },

    #[serde(rename = "to-slice")]
    SliceWithLen {
        #[serde(default)]
        mutable: bool,
        length: String,
    },
}

#[derive(Debug, Clone, PartialEq, Eq, PartialOrd, Ord, Deserialize)]
#[serde(tag = "method")]
pub enum FFIOutConversion {
    #[serde(rename = "id")]
    Id,

    #[serde(rename = "from-ref")]
    // *(const|mut) T <- Option<&(const|mut) T>
    FromRef { mutable: bool },

    #[serde(rename = "lift")]
    // Dual of Unwrap -- unclear if needed at the moment
    Lift,

    #[serde(rename = "pipe")]
    Pipeline { conversions: Vec<FFIOutConversion> },

    #[serde(rename = "from-slice")]
    FromSlice {
        #[serde(rename = "mutable")]
        mutable: bool,
    },
}

fn make_slice(raw_ptr: Box<Expr>, len: Box<Expr>, mutable: bool) -> WithStmts<Box<Expr>> {
    let to_slice = if mutable {
        "from_raw_parts_mut"
    } else {
        "from_raw_parts"
    };
    let slice_method = mk().path_expr(vec!["std", "slice", to_slice]);

    let make_slice = mk().call_expr(
        slice_method,
        vec![
            mk().method_call_expr(raw_ptr, mk().path_segment("cast"), vec![]),
            len,
        ],
    );
    WithStmts::new_val(make_slice)
}

impl FFIInConversion {
    pub fn marshal(
        &self,
        x: &str,
        translation: &Translation,
        e: Box<Expr>,
    ) -> WithStmts<Box<Expr>> {
        match self {
            FFIInConversion::Id => WithStmts::new_val(e),
            FFIInConversion::ToRef { mutable } => {
                let method = if !*mutable { "as_ref" } else { "as_mut" };
                WithStmts::new_val(mk().method_call_expr(e, mk().path_segment(method), vec![]))
            }
            FFIInConversion::ViaCStr {
                empty_if_null,
                mutable,
            } => {
                translation.use_crate(ExternCrate::Libc);
                let len = mk().call_expr(mk().path_expr(vec!["libc", "strlen"]), vec![e.clone()]);
                let len_plus_one = mk().binary_expr(
                    BinOp::Add(Default::default()),
                    len,
                    mk().lit_expr(mk().int_unsuffixed_lit(1)),
                );
                let slice_of_string = make_slice(e.clone(), len_plus_one, *mutable);
                if *empty_if_null {
                    let null_check = mk().unary_expr(
                        UnOp::Not(Default::default()),
                        mk().method_call_expr(e, mk().path_segment("is_null"), vec![]),
                    );
                    let empty_slice = mk().borrow_expr(mk().array_expr(vec![]));
                    let out_local = mk().ident_pat(x);
                    let declare = mk().local(
                        out_local.clone(),
                        None,
                        Some(mk().ifte_expr(
                            null_check,
                            slice_of_string.to_block(),
                            Some(empty_slice),
                        )),
                    );
                    let assign = mk().local_stmt(Box::new(declare));
                    WithStmts::new(vec![assign], mk().path_expr(x))
                } else {
                    slice_of_string
                }
            }

            FFIInConversion::SliceWithLen { mutable, length } => {
                let len = length
                    .parse::<u128>()
                    .map(|i: u128| mk().lit_expr(mk().int_unsuffixed_lit(i)))
                    .unwrap_or_else(|_| {
                        mk().cast_expr(
                            mk().path_expr(vec![length]),
                            mk().path_ty(mk().path("usize")),
                        )
                    });
                make_slice(e, len, *mutable)
            }

            FFIInConversion::Pipeline { conversions } => {
                let mut stmts = vec![];
                let mut e = e;
                for c in conversions {
                    let mut r = c.marshal(x, translation, e);
                    stmts.append(r.stmts_mut());
                    e = r.into_value();
                }
                WithStmts::new(stmts, e)
            }

            FFIInConversion::PointerCast { ty } => {
                let gt = GuidedType::from_str(ty).unwrap();
                let casted = mk().method_call_expr(
                    e,
                    mk().path_segment_with_args(
                        "cast",
                        mk().angle_bracketed_args(vec![Box::new(gt.parsed)]),
                    ),
                    vec![],
                );
                WithStmts::new_val(casted)
            }

            FFIInConversion::Unwrap => {
                WithStmts::new_val(mk().method_call_expr(e, mk().path_segment("unwrap"), vec![]))
            }
        }
    }
}

impl FFIOutConversion {
    pub fn marshal(&self, e: Box<Expr>) -> Box<Expr> {
        match self {
            FFIOutConversion::Id => e,
            FFIOutConversion::Lift => mk().call_expr(mk().ident_expr("Some"), vec![e]),
            FFIOutConversion::Pipeline { conversions } => {
                let mut e = e;
                for c in conversions {
                    e = c.marshal(e);
                }
                e
            }
            FFIOutConversion::FromSlice { mutable } => {
                let ptr_method = if *mutable { "as_mut_ptr" } else { "as_ptr" };
                mk().method_call_expr(e, mk().path_segment(ptr_method), vec![])
            }
            FFIOutConversion::FromRef { mutable } => {
                //opt.map_or(ptr::null(), |r| r as *const u8);
                let argexpr = mk().path_expr("x");
                let (null_fn, cast_ty) = if *mutable {
                    ("null_mut", "*mut _")
                } else {
                    ("null", "*const _")
                };
                let null = mk().call_expr(mk().path_expr(vec!["std", "ptr", null_fn]), vec![]);
                let body = mk().cast_expr(argexpr, syn::parse_str(cast_ty).unwrap());
                let cast = mk().closure_expr(
                    c2rust_ast_builder::CaptureBy::Ref,
                    Movability::Movable,
                    vec![mk().ident_pat("x")],
                    ReturnType::Default,
                    body,
                );
                mk().method_call_expr(e, mk().path_segment("map_or"), vec![null, cast])
            }
        }
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

pub fn is_path_exactly_1_starts_with(path: &Path, a: &str) -> bool {
    if path.segments.len() == 1 {
        path.segments[0].ident.to_string().as_str().starts_with(a)
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

fn is_path_exactly_3(path: &Path, a: &str, b: &str, c: &str) -> bool {
    if path.segments.len() == 3 {
        path.segments[0].ident.to_string().as_str() == a
            && path.segments[1].ident.to_string().as_str() == b
            && path.segments[2].ident.to_string().as_str() == c
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
        3 => is_path_exactly_3(path, "core", "ffi", "c_char"),
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

pub fn type_is_exactly_1_path(ty: &Type, s: &str) -> bool {
    if let Some(path) = type_get_bare_path(ty) {
        return is_path_exactly_1(path, s);
    }
    false
}

pub fn type_is_char(ty: &Type) -> bool {
    type_is_exactly_1_path(ty, "char")
}

pub fn type_is_string(ty: &Type) -> bool {
    type_is_exactly_1_path(ty, "String")
}

pub fn type_is_str_ref(ty: &Type) -> bool {
    type_of_ref(ty).is_some_and(|inner| type_is_exactly_1_path(inner, "str"))
}

pub fn try_type_vec_of(ty: &Type) -> Option<&Type> {
    if let Some(path) = type_get_bare_path(ty) {
        if is_path_exactly_1(path, "Vec") {
            return path_get_1_segment(path)
                .and_then(segment_get_1_bracket_argument)
                .and_then(|ga| match ga {
                    GenericArgument::Type(arg) => Some(arg),
                    _ => None,
                });
        }
    }
    None
}

pub fn type_is_vec_of_1_path(ty: &Type, a: &str) -> bool {
    try_type_vec_of(ty).is_some_and(|arg| type_is_exactly_1_path(arg, a))
}

pub fn type_is_c_char(ty: &Type) -> bool {
    if let Some(path) = type_get_bare_path(ty) {
        return is_path_exactly_3(path, "core", "ffi", "c_char")
            || is_path_exactly_2(path, "libc", "c_char");
    }
    false
}

pub fn type_is_vec(ty: &Type) -> bool {
    type_is_exactly_1_path(ty, "Vec")
}

pub fn type_is_ref(ty: &Type) -> bool {
    matches!(ty, Type::Reference(_))
}

pub fn type_of_ref(ty: &Type) -> Option<&Type> {
    if let Type::Reference(ref tref) = *ty {
        Some(&tref.elem)
    } else {
        None
    }
}

pub fn type_is_mut_ref(ty: &Type) -> bool {
    if let Type::Reference(ref tref) = *ty {
        return tref.mutability.is_some();
    }
    false
}

pub fn type_is_shared_borrow(ty: &Type) -> bool {
    if let Type::Reference(ref tref) = *ty {
        return tref.mutability.is_none();
    }
    false
}

pub fn type_of_slice_ref(ty: &Type) -> Option<&Type> {
    match ty {
        Type::Reference(ref tref) => match *tref.elem {
            Type::Slice(ref slice) => Some(&slice.elem),
            _ => None,
        },
        _ => None,
    }
}

pub fn type_of_array_ref(ty: &Type) -> Option<&Type> {
    match ty {
        Type::Reference(ref tref) => match *tref.elem {
            Type::Array(ref arr) => Some(&arr.elem),
            _ => None,
        },
        _ => None,
    }
}

pub fn type_strip_refs(t: &Type) -> &Type {
    match t {
        Type::Reference(refty) => type_strip_refs(&refty.elem),
        _ => t,
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

pub fn expr_is_lit_str_or_bytes(mut expr: &Expr) -> bool {
    if let Expr::MethodCall(ref call) = *expr {
        if call.method == "as_ptr" {
            expr = &call.receiver;
        }
    }

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

pub fn expr_is_borrow(expr: &Expr) -> bool {
    matches!(expr, Expr::Reference(_))
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

pub fn expr_is_transmute(expr: &Expr) -> bool {
    if let Expr::Path(ref path) = *expr {
        if is_path_exactly_1(&path.path, "transmute") {
            return true;
        }
        if is_path_exactly_3(&path.path, "core", "mem", "transmute") {
            return true;
        }
        if is_path_exactly_3(&path.path, "core", "intrinsics", "transmute") {
            return true;
        }
    }
    false
}

pub fn expr_strip_transmute(expr: &Expr) -> &Expr {
    let mut ep = expr;
    loop {
        match ep {
            Expr::Call(syn::ExprCall { func, args, .. }) => {
                if tenjin::expr_is_transmute(func) && args.len() == 1 {
                    ep = &args[0];
                } else {
                    break ep;
                }
            }
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

fn expr_within_raw_addr(expr: &Expr) -> Option<&Expr> {
    if let Expr::RawAddr(syn::ExprRawAddr { expr: inner, .. }) = expr {
        return Some(inner);
    }
    None
}

pub fn expr_is_call_of_ctime_with_raw_addr(expr: &Expr) -> Option<Box<Expr>> {
    if let Expr::Call(syn::ExprCall { func, args, .. }) = expr {
        if tenjin::expr_is_ident(func, "ctime") && args.len() == 1 {
            if let Some(inner) = tenjin::expr_within_raw_addr(&args[0]) {
                return Some(Box::new(inner.clone()));
            }
        }
    }
    None
}

/// This is called from a context that looks like *((T*) ...)
/// so if the ... looks like &FOO, where T is int/float and FOO has type float/int,
/// then the overall expression is a bitcast between int and float.
pub fn is_bitcast_to_int_or_float(
    t: &Translation,
    argkind: &CExprKind,
) -> Option<(CTypeKind, CExprId, bool)> {
    if let CExprKind::ExplicitCast(outer_cqt, exp, CastKind::BitCast, _opt_field_id, _lrvalue) =
        argkind
    {
        if let CExprKind::Unary(inner_cqt, CUnOp::AddressOf, inner_exp, _lrval) =
            t.ast_context.index_unwrap_parens(*exp).kind
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
                let inner_is_signed = t
                    .ast_context
                    .resolve_type(t.c_type_pointee(inner_cqt.ctype).unwrap_or(inner_cqt.ctype))
                    .kind
                    .is_signed_integral_type();
                return Some((outer_ty.clone(), inner_exp, inner_is_signed));
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
    if pg.using_crates.contains("libbz2-rs-sys") {
        // bzlib.h has `typedef struct { ... } bz_stream;`, and the anonymous
        // struct inherits the typedef's name, matching libbz2-rs-sys.
        if name == "bz_stream" {
            return mk().path(vec!["libbz2_rs_sys", "bz_stream"]);
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
                | "crc32_z"
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
    }
    None
}

/// The `bz_stream`-based and buffer-to-buffer entry points of libbz2, whose
/// libbz2-rs-sys signatures line up with what Tenjin translates the C
/// declarations to, so calls can be retargeted directly.
/// See https://docs.rs/libbz2-rs-sys/latest/libbz2_rs_sys/
fn libbz2_rs_sys_direct_fn_pred(ident: &str) -> bool {
    matches!(
        ident,
        "BZ2_bzBuffToBuffCompress"
            | "BZ2_bzBuffToBuffDecompress"
            | "BZ2_bzCompress"
            | "BZ2_bzCompressEnd"
            | "BZ2_bzCompressInit"
            | "BZ2_bzDecompress"
            | "BZ2_bzDecompressEnd"
            | "BZ2_bzDecompressInit"
            | "BZ2_bzlibVersion"
    )
}

/// The remaining libbz2 entry points are the `BZFILE`-based ones. bzlib.h
/// declares `typedef void BZFILE;`, so Tenjin translates `BZFILE*` as
/// `*mut c_void`, whereas libbz2-rs-sys has its own opaque `BZFILE` struct;
/// likewise `FILE*` translates to a pointer to whichever `FILE` struct the
/// platform headers produced, not to `libc::FILE`. So instead of retargeting
/// these calls directly, we route them through a shim that casts the pointers
/// across. The `FILE` parameters are left generic, since the name of the
/// translated `FILE` struct varies with the platform (and may be
/// disambiguated with a unique suffix).
///
/// Returns the shim's definition; its name is always `xj_` plus `ident`.
fn libbz2_rs_sys_shim_defn(ident: &str) -> Option<&'static str> {
    Some(match ident {
        "BZ2_bzReadOpen" => {
            "unsafe fn xj_BZ2_bzReadOpen<F>(bzerror: *mut ::core::ffi::c_int, f: *mut F, verbosity: ::core::ffi::c_int, small: ::core::ffi::c_int, unused: *mut ::core::ffi::c_void, nUnused: ::core::ffi::c_int) -> *mut ::core::ffi::c_void { libbz2_rs_sys::BZ2_bzReadOpen(bzerror, f.cast(), verbosity, small, unused, nUnused).cast() }"
        }
        "BZ2_bzReadClose" => {
            "unsafe fn xj_BZ2_bzReadClose(bzerror: *mut ::core::ffi::c_int, b: *mut ::core::ffi::c_void) { libbz2_rs_sys::BZ2_bzReadClose(bzerror, b.cast()) }"
        }
        "BZ2_bzReadGetUnused" => {
            "unsafe fn xj_BZ2_bzReadGetUnused(bzerror: *mut ::core::ffi::c_int, b: *mut ::core::ffi::c_void, unused: *mut *mut ::core::ffi::c_void, nUnused: *mut ::core::ffi::c_int) { libbz2_rs_sys::BZ2_bzReadGetUnused(bzerror, b.cast(), unused, nUnused) }"
        }
        "BZ2_bzRead" => {
            "unsafe fn xj_BZ2_bzRead(bzerror: *mut ::core::ffi::c_int, b: *mut ::core::ffi::c_void, buf: *mut ::core::ffi::c_void, len: ::core::ffi::c_int) -> ::core::ffi::c_int { libbz2_rs_sys::BZ2_bzRead(bzerror, b.cast(), buf, len) }"
        }
        "BZ2_bzWriteOpen" => {
            "unsafe fn xj_BZ2_bzWriteOpen<F>(bzerror: *mut ::core::ffi::c_int, f: *mut F, blockSize100k: ::core::ffi::c_int, verbosity: ::core::ffi::c_int, workFactor: ::core::ffi::c_int) -> *mut ::core::ffi::c_void { libbz2_rs_sys::BZ2_bzWriteOpen(bzerror, f.cast(), blockSize100k, verbosity, workFactor).cast() }"
        }
        "BZ2_bzWrite" => {
            "unsafe fn xj_BZ2_bzWrite(bzerror: *mut ::core::ffi::c_int, b: *mut ::core::ffi::c_void, buf: *const ::core::ffi::c_void, len: ::core::ffi::c_int) { libbz2_rs_sys::BZ2_bzWrite(bzerror, b.cast(), buf, len) }"
        }
        "BZ2_bzWriteClose" => {
            "unsafe fn xj_BZ2_bzWriteClose(bzerror: *mut ::core::ffi::c_int, b: *mut ::core::ffi::c_void, abandon: ::core::ffi::c_int, nbytes_in: *mut ::core::ffi::c_uint, nbytes_out: *mut ::core::ffi::c_uint) { libbz2_rs_sys::BZ2_bzWriteClose(bzerror, b.cast(), abandon, nbytes_in, nbytes_out) }"
        }
        "BZ2_bzWriteClose64" => {
            "unsafe fn xj_BZ2_bzWriteClose64(bzerror: *mut ::core::ffi::c_int, b: *mut ::core::ffi::c_void, abandon: ::core::ffi::c_int, nbytes_in_lo32: *mut ::core::ffi::c_uint, nbytes_in_hi32: *mut ::core::ffi::c_uint, nbytes_out_lo32: *mut ::core::ffi::c_uint, nbytes_out_hi32: *mut ::core::ffi::c_uint) { libbz2_rs_sys::BZ2_bzWriteClose64(bzerror, b.cast(), abandon, nbytes_in_lo32, nbytes_in_hi32, nbytes_out_lo32, nbytes_out_hi32) }"
        }
        "BZ2_bzopen" => {
            "unsafe fn xj_BZ2_bzopen(path: *const ::core::ffi::c_char, mode: *const ::core::ffi::c_char) -> *mut ::core::ffi::c_void { libbz2_rs_sys::BZ2_bzopen(path, mode).cast() }"
        }
        "BZ2_bzdopen" => {
            "unsafe fn xj_BZ2_bzdopen(fd: ::core::ffi::c_int, mode: *const ::core::ffi::c_char) -> *mut ::core::ffi::c_void { libbz2_rs_sys::BZ2_bzdopen(fd, mode).cast() }"
        }
        "BZ2_bzread" => {
            "unsafe fn xj_BZ2_bzread(b: *mut ::core::ffi::c_void, buf: *mut ::core::ffi::c_void, len: ::core::ffi::c_int) -> ::core::ffi::c_int { libbz2_rs_sys::BZ2_bzread(b.cast(), buf, len) }"
        }
        "BZ2_bzwrite" => {
            "unsafe fn xj_BZ2_bzwrite(b: *mut ::core::ffi::c_void, buf: *const ::core::ffi::c_void, len: ::core::ffi::c_int) -> ::core::ffi::c_int { libbz2_rs_sys::BZ2_bzwrite(b.cast(), buf, len) }"
        }
        "BZ2_bzflush" => {
            "unsafe fn xj_BZ2_bzflush(b: *mut ::core::ffi::c_void) -> ::core::ffi::c_int { libbz2_rs_sys::BZ2_bzflush(b.cast()) }"
        }
        "BZ2_bzclose" => {
            "unsafe fn xj_BZ2_bzclose(b: *mut ::core::ffi::c_void) { libbz2_rs_sys::BZ2_bzclose(b.cast()) }"
        }
        "BZ2_bzerror" => {
            "unsafe fn xj_BZ2_bzerror(b: *const ::core::ffi::c_void, errnum: *mut ::core::ffi::c_int) -> *const ::core::ffi::c_char { libbz2_rs_sys::BZ2_bzerror(b.cast(), errnum) }"
        }
        _ => return None,
    })
}

#[allow(clippy::borrowed_box)]
fn libbz2_rs_sys_call_form_cases(t: &Translation, func: &Expr) -> Option<RecognizedCallForm> {
    if !t
        .parsed_guidance
        .borrow()
        .using_crates
        .contains("libbz2-rs-sys")
    {
        return None;
    }

    let ident = expr_get_ident(func)?;

    if libbz2_rs_sys_direct_fn_pred(&ident) {
        t.use_crate(ExternCrate::Libbz2RsSys);
        return Some(RecognizedCallForm::RetargetedCallee(
            mk().path_expr(vec!["libbz2_rs_sys", &ident]),
        ));
    }

    if let Some(shim_defn) = libbz2_rs_sys_shim_defn(&ident) {
        t.use_crate(ExternCrate::Libbz2RsSys);
        t.with_cur_file_item_store(|item_store| {
            item_store.add_item_str_once(shim_defn);
        });
        return Some(RecognizedCallForm::RetargetedCallee(
            mk().path_expr(vec![format!("xj_{ident}")]),
        ));
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

fn parse_ffi_in_conversion(v: &serde_json::Value) -> Option<FFIInConversion> {
    serde_json::from_value(v.clone()).unwrap_or(None)
}

fn parse_ffi_out_conversion(v: &serde_json::Value) -> Option<FFIOutConversion> {
    serde_json::from_value(v.clone()).unwrap_or(None)
}

pub struct ParsedGuidance {
    pub _raw: serde_json::Value,
    /// Rust types of the marker typedefs xj-prepare-guidance declared.
    pub marker_typedefs: HashMap<String, tenjin::GuidedType>,
    /// `vars_mut` resolved to declarations, keyed `fn:var` for locals and
    /// parameters and `var` at file scope.
    pub vars_mut_resolved: HashMap<String, Mutability>,
    pub using_crates: HashSet<String>,
    pub pod_types: HashSet<String>,
    pub no_math_errno: bool,
    pub public_api: Option<HashSet<String>>,
    /// Globals PANGS proved are never written after initialization, keyed
    /// as `vars_mut_resolved` is.
    pub semantically_immutable_globals: HashSet<String>,
    pub ffi_conversions: HashMap<String, FFIConversion>,
    /// Convert marker typedefs as the C types behind them, for code that must
    /// keep the C ABI.
    pub c_abi_markers: Cell<bool>,
}

impl ParsedGuidance {
    pub fn new(raw: serde_json::Value) -> Self {
        ParsedGuidance {
            marker_typedefs: parse_marker_typedefs(&raw),
            vars_mut_resolved: parse_vars_mut_resolved(&raw),
            using_crates: crate::guidance_use_crates(&raw),
            pod_types: parse_string_set(&raw, "pod_types").unwrap_or_default(),
            no_math_errno: raw
                .get("no_math_errno")
                .and_then(|v| v.as_bool())
                .unwrap_or(false),
            public_api: parse_string_set(&raw, "public_api"),
            semantically_immutable_globals: parse_string_set(
                &raw,
                "semantically_immutable_globals",
            )
            .unwrap_or_default(),
            ffi_conversions: parse_ffi_conversions(&raw),
            c_abi_markers: Cell::new(false),
            _raw: raw,
        }
    }

    pub fn query_ffi_in_conversion(&self, fn_name: &str, arg_name: &str) -> FFIInConversion {
        self.ffi_conversions
            .get(fn_name)
            .and_then(|it| it.ins.get(arg_name))
            .unwrap_or(&FFIInConversion::Id)
            .clone()
    }

    pub fn query_ffi_out_conversion(&self, fn_name: &str) -> FFIOutConversion {
        self.ffi_conversions
            .get(fn_name)
            .and_then(|it| it.out.as_ref())
            .unwrap_or(&FFIOutConversion::Id)
            .clone()
    }

    /// Mutability guidance for variable `name`, declared in function `parent`
    /// or at file scope.
    pub fn query_var_mut(&self, parent: Option<&str>, name: &str) -> Option<Mutability> {
        self.vars_mut_resolved
            .get(&guidance_key(parent, name))
            .copied()
    }

    /// Whether PANGS proved variable `name`, declared in function `parent` or
    /// at file scope, immutable.
    pub fn is_semantically_immutable(&self, parent: Option<&str>, name: &str) -> bool {
        self.semantically_immutable_globals
            .contains(&guidance_key(parent, name))
    }
}

/// How guidance names a variable: `fn:var` in a function, `var` at file scope.
fn guidance_key(parent: Option<&str>, name: &str) -> String {
    match parent {
        Some(parent) => format!("{parent}:{name}"),
        None => name.to_string(),
    }
}

fn parse_marker_typedefs(raw: &serde_json::Value) -> HashMap<String, tenjin::GuidedType> {
    let mut typedefs = HashMap::new();
    let Some(entries) = raw.get("marker_typedefs").and_then(|v| v.as_object()) else {
        return typedefs;
    };
    for (name, ty) in entries {
        match ty.as_str().map(str::parse::<tenjin::GuidedType>) {
            Some(Ok(g)) => {
                typedefs.insert(name.clone(), g);
            }
            _ => log::error!("Tenjin marker typedef {name} has invalid type {ty}"),
        }
    }
    typedefs
}

fn parse_vars_mut_resolved(raw: &serde_json::Value) -> HashMap<String, Mutability> {
    let Some(entries) = raw.get("vars_mut_resolved").and_then(|v| v.as_object()) else {
        return HashMap::new();
    };
    entries
        .iter()
        .filter_map(|(key, is_mut)| {
            let mutbl = if is_mut.as_bool()? {
                Mutability::Mutable
            } else {
                Mutability::Immutable
            };
            Some((key.clone(), mutbl))
        })
        .collect()
}

fn parse_string_set(raw: &serde_json::Value, key: &str) -> Option<HashSet<String>> {
    let values = raw.get(key)?.as_array()?;
    Some(
        values
            .iter()
            .filter_map(|v| v.as_str().map(str::to_string))
            .collect(),
    )
}

fn parse_ffi_conversions(raw: &serde_json::Value) -> HashMap<String, FFIConversion> {
    let mut ffi_conversions = HashMap::new();
    let Some(strategies) = raw.get("ffi").and_then(|it| it.as_object()) else {
        return ffi_conversions;
    };
    for (func, conv) in strategies {
        let mut ffi_in_conversions = HashMap::new();
        let mut ffi_out_conversion = None;
        for (arg, strategy) in conv.as_object().unwrap_or(&Map::new()) {
            if arg == "$return" {
                ffi_out_conversion = parse_ffi_out_conversion(strategy);
                if ffi_out_conversion.is_none() {
                    log::error!(
                        "Tenjin `ffi` guidance for return value of {} is invalid: {}",
                        func,
                        strategy
                    );
                }
            } else if let Some(strategy) = parse_ffi_in_conversion(strategy) {
                ffi_in_conversions.insert(arg.clone(), strategy);
            } else {
                log::error!(
                    "Tenjin `ffi` guidance for function {} argument {} is invalid: {}",
                    func,
                    arg,
                    strategy
                );
            }
        }
        if !ffi_in_conversions.is_empty() || ffi_out_conversion.is_some() {
            ffi_conversions.insert(
                func.clone(),
                FFIConversion {
                    ins: ffi_in_conversions,
                    out: ffi_out_conversion,
                },
            );
        }
    }
    ffi_conversions
}

impl Translation<'_> {
    // fn strip_integral_cast(&self, expr: CExprId) -> CExprId {
    //     let kind = &self.ast_context.index_unwrap_parens(expr).kind;
    //     if let CExprKind::ImplicitCast(_, inner, CastKind::IntegralCast, _, _) = kind {
    //         *inner
    //     } else {
    //         expr
    //     }
    // }

    pub fn strip_implicit_array_to_pointer_cast(&self, expr: CExprId) -> CExprId {
        let kind = &self.ast_context.index_unwrap_parens(expr).kind;
        if let CExprKind::ImplicitCast(_, inner, CastKind::ArrayToPointerDecay, _, _) = kind {
            *inner
        } else {
            expr
        }
    }

    fn is_integral_lit(&self, expr: CExprId, val: u64) -> bool {
        let kind = &self.ast_context.index_unwrap_parens(expr).kind;
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
        let kind = &self.ast_context.index_unwrap_parens(expr).kind;
        if let CExprKind::Literal(_, clit) = kind {
            match clit {
                CLiteral::String(_, _) => Some(clit),
                _ => None,
            }
        } else {
            None
        }
    }

    fn c_type_pointee(&self, typ: CTypeId) -> Option<CTypeId> {
        if let CTypeKind::Pointer(pointee) = self.ast_context.resolve_type(typ).kind {
            Some(pointee.ctype)
        } else {
            None
        }
    }

    fn c_expr_decl_id(&self, expr: CExprId) -> Option<CDeclId> {
        let kind = &self
            .ast_context
            .index_unwrap_parens(self.c_strip_implicit_casts(expr))
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

        let expr = self.c_strip_implicit_casts(expr);

        let get_sizeof =
            |t: &Translation<'_>, expr: CExprId| -> Option<(Option<CExprId>, CTypeId)> {
                let kind = &t.ast_context.index_unwrap_parens(expr).kind;
                if let CExprKind::UnaryType(_cqt1, CUnTypeOp::SizeOf, mb_expr_id, cqt2) = kind {
                    Some((*mb_expr_id, cqt2.ctype))
                } else {
                    None
                }
            };

        if let CExprKind::Binary(_cq, CBinOp::Multiply, lhs, rhs, _opt_cq_lhs, _opt_cq_rhs) =
            self.ast_context.index_unwrap_parens(expr).kind
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

    /// A `Vec<u8>` (or a reference to one) behind any identity marker.
    fn is_guided_byte_vec(&self, carg: CExprId) -> bool {
        self.xj_type_of_expr(self.strip_identity_markers(carg))
            .is_some_and(|g| type_is_vec_of_1_path(g.strip_refs(), "u8"))
    }

    #[allow(clippy::vec_box)]
    fn call_form_cases(
        &self,
        func: &Expr,
        args: &[Box<Expr>],
        cargs: &[CExprId],
        ctx: ExprContext,
    ) -> RecognizedCallForm {
        if tenjin::expr_is_ident(func, "puts") && ctx.is_unused() && !args.is_empty() {
            return RecognizedCallForm::Puts;
        }

        if tenjin::expr_is_ident(func, "printf")
            && ctx.is_unused()
            && !args.is_empty()
            && tenjin::expr_is_lit_str_or_bytes(tenjin::expr_strip_casts(&args[0]))
        {
            return RecognizedCallForm::PrintfOut { fmt_string_idx: 0 };
        }

        if tenjin::expr_is_ident(func, "snprintf")
            && ctx.is_unused()
            && args.len() >= 3
            && tenjin::expr_is_lit_str_or_bytes(tenjin::expr_strip_casts(&args[2]))
            && self.is_guided_byte_vec(cargs[0])
        {
            // XREF:sprint_into_mutref_vec_u8
            return RecognizedCallForm::PrintfS {
                fmt_string_idx: 2,
                opt_size: Some(expr_in_usize(args[1].clone())),
                dest: self.strip_identity_markers(cargs[0]),
            };
        }

        if tenjin::expr_is_ident(func, "sprintf")
            && ctx.is_unused()
            && args.len() >= 2
            && tenjin::expr_is_lit_str_or_bytes(tenjin::expr_strip_casts(&args[1]))
            && self.is_guided_byte_vec(cargs[0])
        {
            // XREF:sprint_into_mutref_vec_u8
            return RecognizedCallForm::PrintfS {
                fmt_string_idx: 1,
                opt_size: None,
                dest: self.strip_identity_markers(cargs[0]),
            };
        }

        if tenjin::expr_is_ident(func, "fprintf")
            && ctx.is_unused()
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

        if let Some(call_form) = libz_rs_sys_call_form_cases(self, func) {
            return call_form;
        }

        if let Some(call_form) = libbz2_rs_sys_call_form_cases(self, func) {
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
    ) -> TranslationResult<WithStmts<Box<Expr>>> {
        let mk_call_with = |func: Box<Expr>, args: Vec<Box<Expr>>| {
            let call_expr = mk().call_expr(func, args);
            self.make_cast(
                ctx,
                call_expr_ty,
                override_ty.unwrap_or(call_expr_ty),
                WithStmts::new_val(call_expr),
            )
        };
        match self.call_form_cases(&func, &args, cargs, ctx) {
            RecognizedCallForm::Puts => Ok(WithStmts::new_val(mk().mac_expr(
                refactor_format::build_format_macro_from(
                    self,
                    "%s\n".into(),
                    "println",
                    "println",
                    &args,
                    cargs,
                    None,
                    None,
                    None,
                ),
            ))),
            RecognizedCallForm::PrintfOut { fmt_string_idx } => {
                let fmt_string_span = self.ast_context.display_loc(
                    &self
                        .ast_context
                        .index_unwrap_parens(cargs[fmt_string_idx])
                        .loc,
                );
                Ok(WithStmts::new_val(mk().mac_expr(
                    refactor_format::build_format_macro(
                        self,
                        "print",
                        "println",
                        &args[fmt_string_idx..],
                        &cargs[fmt_string_idx..],
                        None,
                        fmt_string_span,
                    ),
                )))
            }
            RecognizedCallForm::PrintfErr { fmt_string_idx } => {
                let fmt_string_span = self.ast_context.display_loc(
                    &self
                        .ast_context
                        .index_unwrap_parens(cargs[fmt_string_idx])
                        .loc,
                );
                Ok(WithStmts::new_val(mk().mac_expr(
                    refactor_format::build_format_macro(
                        self,
                        "eprint",
                        "eprintln",
                        &args[fmt_string_idx..],
                        &cargs[fmt_string_idx..],
                        None,
                        fmt_string_span,
                    ),
                )))
            }
            RecognizedCallForm::PrintfS {
                fmt_string_idx,
                opt_size,
                dest,
            } => {
                let fmt_string_span = self.ast_context.display_loc(
                    &self
                        .ast_context
                        .index_unwrap_parens(cargs[fmt_string_idx])
                        .loc,
                );
                let formatted_string = mk().mac_expr(refactor_format::build_format_macro(
                    self,
                    "format",
                    "format",
                    &args[fmt_string_idx..],
                    &cargs[fmt_string_idx..],
                    None,
                    fmt_string_span,
                ));
                let dest = self.convert_expr(ctx.used(), dest, None)?.to_expr();
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

                Ok(WithStmts::new_val(mk().call_expr(
                    mk().path_expr(vec!["xj_sprintf_Vec_u8"]),
                    vec![mk().mutbl().borrow_expr(dest), size_expr, formatted_string],
                )))
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
        // Recognizers inspect the values the program passes, not the markers
        // that fit them to C parameter types.
        let cargs = &self.strip_identity_markers_all(cargs);
        if let Some(path) = tenjin::expr_get_path(func) {
            match () {
                _ if tenjin::is_path_exactly_1(path, "exit") => {
                    // TODO: should check source of symbol to ensure it's the C standard library exit()
                    self.recognize_preconversion_call_exit(ctx, cargs)
                }
                _ if tenjin::is_path_exactly_1(path, "assert") => {
                    self.recognize_preconversion_call_assert(ctx, cargs)
                }
                _ if tenjin::is_path_exactly_1(path, "FD_ZERO") => {
                    self.recognize_preconversion_call_fd_zero(ctx, cargs)
                }
                _ if tenjin::is_path_exactly_1(path, "FD_SET") => {
                    self.recognize_preconversion_call_fd_set(ctx, cargs, "xj_fd_set")
                }
                _ if tenjin::is_path_exactly_1(path, "FD_CLR") => {
                    self.recognize_preconversion_call_fd_set(ctx, cargs, "xj_fd_clr")
                }
                _ if tenjin::is_path_exactly_1(path, "FD_ISSET") => {
                    self.recognize_preconversion_call_fd_set(ctx, cargs, "xj_fd_isset")
                }
                _ if tenjin::is_path_exactly_1(path, "fputs") => {
                    self.recognize_preconversion_call_fputs_stdout_guided(ctx, func, cargs)
                }
                _ if tenjin::is_path_exactly_1(path, "fgets") => {
                    self.recognize_preconversion_call_fgets_stdin(call_type_id, ctx, func, cargs)
                }
                _ if tenjin::is_path_exactly_1(path, "fputc") => {
                    self.recognize_preconversion_call_fputc_stdout_guided(ctx, func, cargs)
                }
                _ if tenjin::is_path_exactly_1(path, "putchar") => {
                    self.recognize_preconversion_call_putchar_stdout_guided(ctx, func, cargs)
                }
                _ if tenjin::is_path_exactly_1(path, "strlen") => {
                    self.recognize_preconversion_call_strlen_guided(ctx, func, cargs)
                }
                _ if tenjin::is_path_exactly_1(path, "strcspn") => {
                    self.recognize_preconversion_call_strcspn_guided(ctx, func, cargs)
                }
                _ if tenjin::is_path_exactly_1(path, "isalnum") => {
                    self.recognize_ctype_is_1(ctx, "isalnum", "xjc.is_ascii_alphanumeric()", cargs)
                }
                _ if tenjin::is_path_exactly_1(path, "isalpha") => {
                    self.recognize_ctype_is_1(ctx, "isalpha", "xjc.is_ascii_alphabetic()", cargs)
                }
                _ if tenjin::is_path_exactly_1(path, "islower") => {
                    self.recognize_ctype_is_1(ctx, "islower", "xjc.is_ascii_lowercase()", cargs)
                }
                _ if tenjin::is_path_exactly_1(path, "isupper") => {
                    self.recognize_ctype_is_1(ctx, "isupper", "xjc.is_ascii_uppercase()", cargs)
                }
                _ if tenjin::is_path_exactly_1(path, "isdigit") => {
                    self.recognize_ctype_is_1(ctx, "isdigit", "xjc.is_ascii_digit()", cargs)
                }
                _ if tenjin::is_path_exactly_1(path, "isxdigit") => {
                    self.recognize_ctype_is_1(ctx, "isxdigit", "xjc.is_ascii_hexdigit()", cargs)
                }
                _ if tenjin::is_path_exactly_1(path, "iscntrl") => {
                    self.recognize_ctype_is_1(ctx, "iscntrl", "xjc.is_ascii_control()", cargs)
                }
                _ if tenjin::is_path_exactly_1(path, "isgraph") => {
                    self.recognize_ctype_is_1(ctx, "isgraph", "xjc.is_ascii_graphic()", cargs)
                }
                _ if tenjin::is_path_exactly_1(path, "isspace") => self.recognize_ctype_is_1(
                    ctx,
                    "isspace",
                    "(xjc.is_ascii_whitespace() || xjc == '\\x0b')",
                    cargs,
                ),
                _ if tenjin::is_path_exactly_1(path, "isprint") => self.recognize_ctype_is_1(
                    ctx,
                    "isprint",
                    "(xjc.is_ascii_graphic() || xjc == ' ')",
                    cargs,
                ),
                _ if tenjin::is_path_exactly_1(path, "ispunct") => {
                    self.recognize_ctype_is_1(ctx, "ispunct", "xjc.is_ascii_punctuation()", cargs)
                }
                _ if tenjin::is_path_exactly_1(path, "isblank") => {
                    self.recognize_ctype_is_1(ctx, "isblank", "(xjc == ' ' || xjc == '\\t')", cargs)
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
                _ if (tenjin::is_path_exactly_1(path, "isinf")) => {
                    self.recognize_preconversion_call_isinf(ctx, cargs)
                }
                _ if (tenjin::is_path_exactly_1(path, "isnan")) => {
                    self.recognize_preconversion_call_isnan(ctx, cargs)
                }
                _ if tenjin::is_path_exactly_1(path, "isnormal") => {
                    self.recognize_preconversion_call_float_predicate(ctx, cargs, "is_normal")
                }
                _ if tenjin::is_path_exactly_1(path, "isfinite") => {
                    self.recognize_preconversion_call_float_predicate(ctx, cargs, "is_finite")
                }
                _ if tenjin::is_path_exactly_1(path, "signbit") => self
                    .recognize_preconversion_call_float_predicate(ctx, cargs, "is_sign_negative"),
                _ if tenjin::is_path_exactly_1(path, "isgreater") => {
                    self.recognize_preconversion_call_float_comparison(ctx, cargs, "isgreater")
                }
                _ if tenjin::is_path_exactly_1(path, "isgreaterequal") => {
                    self.recognize_preconversion_call_float_comparison(ctx, cargs, "isgreaterequal")
                }
                _ if tenjin::is_path_exactly_1(path, "isless") => {
                    self.recognize_preconversion_call_float_comparison(ctx, cargs, "isless")
                }
                _ if tenjin::is_path_exactly_1(path, "islessequal") => {
                    self.recognize_preconversion_call_float_comparison(ctx, cargs, "islessequal")
                }
                _ if tenjin::is_path_exactly_1(path, "islessgreater") => {
                    self.recognize_preconversion_call_float_comparison(ctx, cargs, "islessgreater")
                }
                _ if tenjin::is_path_exactly_1(path, "isunordered") => {
                    self.recognize_preconversion_call_float_comparison(ctx, cargs, "isunordered")
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
                _ if tenjin::is_path_exactly_1(path, "difftime") => {
                    self.recognize_preconversion_call_difftime(ctx, cargs)
                }
                _ if tenjin::is_path_exactly_1(path, "localtime")
                    || tenjin::is_path_exactly_1(path, "localtime_r") =>
                {
                    self.use_crate(ExternCrate::XjCtime);
                    Ok(None)
                }
                _ if tenjin::is_path_exactly_1_starts_with(path, "__tenjin_bvm_") => {
                    self.recognize_preconversion_call_bitcastviamemcpy(ctx, cargs)
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
            // The C idiom of tacking a message onto the condition,
            // assert(FOO && "message")
            // becomes the two-argument form,
            // assert!(FOO, "message")
            //
            // Note that in C the asserted expression must have integral type,
            // but in Rust the asserted expression is of boolean type. That mismatch
            // is why we recognize this case pre-conversion.
            let (ccond, opt_message) = match self.recognize_assert_condition_message(cargs[0]) {
                Some((ccond, message)) => (ccond, Some(message)),
                None => (cargs[0], None),
            };
            let expr = self.convert_condition(ctx.used(), true, ccond)?;
            return Ok(Some(expr.and_then(|expr| {
                let mut mac_args = vec![expr];
                mac_args.extend(opt_message);
                WithStmts::new_val(mk().mac_expr(mk().mac(
                    mk().path("assert"),
                    mac_call_exprs_tt(mac_args),
                    MacroDelimiter::Paren(Default::default()),
                )))
            })));
        }

        Ok(None)
    }

    fn recognize_preconversion_call_fd_zero(
        &self,
        ctx: ExprContext,
        cargs: &[CExprId],
    ) -> TranslationResult<Option<WithStmts<Box<Expr>>>> {
        if cargs.len() != 1 {
            return Ok(None);
        }

        self.use_crate(ExternCrate::Libc);
        self.with_cur_file_item_store(|item_store| {
            item_store.add_item_str_once(
                "pub fn xj_fd_zero(set: &mut libc::fd_set) {
                    // SAFETY: sound because fd_set is a plain array of integers with no invalid bit patterns.
                    *set = unsafe { std::mem::zeroed() };
                }",
            );
        });
        let set = self.convert_fd_set_arg(ctx, cargs[0], true)?;
        Ok(Some(set.and_then(|set| {
            WithStmts::new_val(mk().call_expr(mk().path_expr(vec!["xj_fd_zero"]), vec![set]))
        })))
    }

    fn recognize_preconversion_call_fd_set(
        &self,
        ctx: ExprContext,
        cargs: &[CExprId],
        helper_name: &'static str,
    ) -> TranslationResult<Option<WithStmts<Box<Expr>>>> {
        if cargs.len() != 2 {
            return Ok(None);
        }

        self.use_crate(ExternCrate::Libc);
        self.with_cur_file_item_store(|item_store| {
            item_store.add_item_str_once(
                "pub fn xj_fd_set(fd: libc::c_int, set: &mut libc::fd_set) {
                    assert!((0..libc::FD_SETSIZE as libc::c_int).contains(&fd));
                    // SAFETY: sound due to unconditional checked `assert!`
                    unsafe { libc::FD_SET(fd, set) }
                }",
            );
            item_store.add_item_str_once(
                "pub fn xj_fd_clr(fd: libc::c_int, set: &mut libc::fd_set) {
                    assert!((0..libc::FD_SETSIZE as libc::c_int).contains(&fd));
                    // SAFETY: sound due to unconditional checked `assert!`
                    unsafe { libc::FD_CLR(fd, set) }
                }",
            );
            item_store.add_item_str_once(
                "pub fn xj_fd_isset(fd: libc::c_int, set: &libc::fd_set) -> libc::c_int {
                    assert!((0..libc::FD_SETSIZE as libc::c_int).contains(&fd));
                    // SAFETY: sound due to unconditional checked `assert!`
                    unsafe { libc::FD_ISSET(fd, set) as libc::c_int }
                }",
            );
        });
        let fd = self.convert_expr(ctx.used(), cargs[0], None)?;
        let set = self.convert_fd_set_arg(ctx, cargs[1], helper_name != "xj_fd_isset")?;
        Ok(Some(fd.and_then(|fd| {
            set.and_then(|set| {
                WithStmts::new_val(mk().call_expr(mk().path_expr(vec![helper_name]), vec![fd, set]))
            })
        })))
    }

    fn convert_fd_set_arg(
        &self,
        ctx: ExprContext,
        cexpr: CExprId,
        mutbl: bool,
    ) -> TranslationResult<WithStmts<Box<Expr>>> {
        let set = self.convert_expr(ctx.used(), cexpr, None)?;
        Ok(set.and_then(|set| {
            // The macro argument has been cast to void* by the marker declaration.
            // Preserve its raw borrow, then retarget it to libc's fd_set before
            // making the safe reference required by the helper.
            let raw_set = Box::new(tenjin::expr_strip_casts(&set).clone());
            let fd_set_ty = mk().path_ty(mk().path(vec!["libc", "fd_set"]));
            let fd_set_ptr = if mutbl {
                mk().mutbl().ptr_ty(fd_set_ty)
            } else {
                mk().ptr_ty(fd_set_ty)
            };
            let set = mk().unary_expr(
                UnOp::Deref(Default::default()),
                mk().cast_expr(raw_set, fd_set_ptr),
            );
            let set = if mutbl {
                mk().mutbl().borrow_expr(set)
            } else {
                mk().borrow_expr(set)
            };
            WithStmts::new_val(set)
        }))
    }

    /// Recognizes the C idiom `COND && "message"`, returning the condition along with
    /// the message as a Rust string literal suitable for use as a format string.
    fn recognize_assert_condition_message(&self, expr: CExprId) -> Option<(CExprId, Box<Expr>)> {
        let CExprKind::Binary(_, CBinOp::And, lhs, rhs, _, _) = self
            .ast_context
            .index_unwrap_parens(self.c_strip_implicit_casts(expr))
            .kind
        else {
            return None;
        };

        // Only narrow (byte-per-character) literals can be reproduced as Rust string literals.
        let Some(CLiteral::String(bytes, 1)) =
            self.get_string_lit(self.c_strip_implicit_casts(rhs))
        else {
            return None;
        };

        let message = std::str::from_utf8(bytes).ok()?;
        // The message is used as a format string by `assert!`, so braces must be escaped.
        let message = message.replace('{', "{{").replace('}', "}}");
        Some((lhs, mk().lit_expr(message.as_str())))
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
            return Ok(Some(expr_x.and_then(|expr_x| {
                WithStmts::new_val(mk().method_call_expr(expr_x, method_name, Vec::new()))
            })));
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
            return Ok(Some(expr_x.and_then(|expr_x| {
                WithStmts::new_val(mk().method_call_expr(
                    expr_x,
                    method_name,
                    vec![expr_y.to_expr()],
                ))
            })));
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
            return Ok(Some(expr_x.and_then(|expr_x| {
                WithStmts::new_val(mk().binary_expr(
                    BinOp::Rem(Default::default()),
                    expr_x,
                    expr_y.to_expr(),
                ))
            })));
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

            if self
                .xj_type_of_expr(cargs[0])
                .is_some_and(|g| type_is_string(&g.parsed))
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
                        .display_loc(&self.ast_context.index_unwrap_parens(cargs[0]).loc),
                ));
                return Ok(Some(WithStmts::new_val(print_call)));
            }
        }

        Ok(None)
    }

    #[allow(clippy::borrowed_box)]
    fn recognize_preconversion_call_fputc_stdout_guided(
        &self,
        ctx: ExprContext,
        func: &Box<Expr>,
        cargs: &[CExprId],
    ) -> TranslationResult<Option<WithStmts<Box<Expr>>>> {
        if tenjin::expr_is_ident(func, "fputc") && cargs.len() == 2 && ctx.is_unused() {
            // fputc(FOO, stdout)
            //    (when not used by the context)
            // should be translated to
            // print!("{}", FOO)
            // TODO: if the context is used, we need a wrapper around stdout().write_all()
            // which returns EOF on error, and otherwise returns the character written as an integer.
            if !(self.c_expr_is_var_ident(cargs[1], "stdout")
                || self.c_expr_is_var_ident(cargs[1], "__stdoutp"))
            {
                return Ok(None);
            }

            let expr = self.convert_expr(ctx.used(), cargs[0], None)?;
            let print_call = mk().mac_expr(refactor_format::build_format_macro_from(
                self,
                "%c".to_string(),
                "print",
                "println",
                &[expr.to_expr()],
                &[cargs[0]],
                None,
                None,
                self.ast_context
                    .display_loc(&self.ast_context.index_unwrap_parens(cargs[0]).loc),
            ));
            return Ok(Some(WithStmts::new_val(print_call)));
        }
        Ok(None)
    }

    #[allow(clippy::borrowed_box)]
    fn recognize_preconversion_call_putchar_stdout_guided(
        &self,
        ctx: ExprContext,
        func: &Box<Expr>,
        cargs: &[CExprId],
    ) -> TranslationResult<Option<WithStmts<Box<Expr>>>> {
        if tenjin::expr_is_ident(func, "putchar") && cargs.len() == 1 && ctx.is_unused() {
            // putchar(FOO)
            //    (when not used by the context)
            // should be translated to
            // print!("{}", FOO)
            let expr = self.convert_expr(ctx.used(), cargs[0], None)?;
            let print_call = mk().mac_expr(refactor_format::build_format_macro_from(
                self,
                "%c".to_string(),
                "print",
                "println",
                &[expr.to_expr()],
                &[cargs[0]],
                None,
                None,
                self.ast_context
                    .display_loc(&self.ast_context.index_unwrap_parens(cargs[0]).loc),
            ));
            return Ok(Some(WithStmts::new_val(print_call)));
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
            // FOO.len() as size_t
            // XREF:guided_c_strlen
            if self
                .xj_type_of_expr(cargs[0])
                .is_some_and(|g| type_is_string(&g.parsed))
            {
                let expr = self.convert_expr(ctx.used(), cargs[0], None)?;
                let len_call = mk().method_call_expr(expr.to_expr(), "len", vec![]);
                let len_call_as_size_t = mk().cast_expr(len_call, mk().path_ty(vec!["size_t"]));
                return Ok(Some(WithStmts::new_val(len_call_as_size_t)));
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
            // XREF:guided_strcspn
            if self
                .xj_type_of_expr(cargs[0])
                .is_some_and(|g| type_is_string(&g.parsed))
                && self
                    .xj_type_of_expr(cargs[1])
                    .is_some_and(|g| type_is_string(&g.parsed))
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
                        mk().borrow_expr(expr_foo.to_expr()),
                        mk().borrow_expr(expr_bar.to_expr()),
                    ],
                );
                return Ok(Some(WithStmts::new_val(strcspn_call)));
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
            // XREF:guided_isalnum
            if self
                .xj_type_of_expr(cargs[0])
                .is_some_and(|g| type_is_char(&g.parsed))
            {
                let rust_helper_name = format!("{}_char_i", c_fn_name);
                self.with_cur_file_item_store(|item_store| {
                    item_store.add_item_str_once(&format!(
                        "fn {}(xjc: char) -> core::ffi::c_int {{ ({}) as core::ffi::c_int }}",
                        rust_helper_name, rust_char_impl
                    ));
                });

                let expr_foo = self.convert_expr(ctx.used(), cargs[0], None)?;
                let bare_foo: Box<Expr> =
                    Box::new(tenjin::expr_strip_casts(&(expr_foo.to_expr())).clone());
                let call = mk().call_expr(mk().path_expr(vec![&rust_helper_name]), vec![bare_foo]);
                return Ok(Some(WithStmts::new_val(call)));
            }

            // Fallthrough: no guidance, or expr was not a simple variable.
            let rust_helper_name = format!("xj_{}", c_fn_name);
            self.with_cur_file_item_store(|item_store| {
            item_store.add_item_str_once(&format!(
                "fn {}(xjc: core::ffi::c_int) -> core::ffi::c_int {{ if xjc == -1 {{ 0 }} else {{ let xjc = xjc as u8 as char; ({}) as core::ffi::c_int }} }}",
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
            // XREF:guided_tolower
            if self
                .xj_type_of_expr(cargs[0])
                .is_some_and(|g| type_is_char(&g.parsed))
            {
                self.with_cur_file_item_store(|item_store| {
                // For now we return an integer code rather than a bool,
                // to better match the C function signature.
                item_store.add_item_str_once(
                    "fn tolower_char_i(xjc: char) -> core::ffi::c_int { xjc.to_ascii_lowercase() as core::ffi::c_int }",
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
            // Fallthrough: no guidance, or expr was not a simple variable.

            self.with_cur_file_item_store(|item_store| {
            item_store.add_item_str_once(
                "fn xj_tolower(xjc: core::ffi::c_int) -> core::ffi::c_int { if xjc == -1 { -1 } else { (xjc as u8 as char).to_ascii_lowercase() as core::ffi::c_int } }",
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
            if self
                .xj_type_of_expr(cargs[0])
                .is_some_and(|g| type_is_char(&g.parsed))
            {
                self.with_cur_file_item_store(|item_store| {
                // For now we return an integer code rather than a bool,
                // to better match the C function signature.
                item_store.add_item_str_once(
                    "fn toupper_char_i(xjc: char) -> core::ffi::c_int { xjc.to_ascii_uppercase() as core::ffi::c_int }",
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
            // Fallthrough: no guidance, or expr was not a simple variable.

            self.with_cur_file_item_store(|item_store| {
            item_store.add_item_str_once(
                "fn xj_toupper(xjc: core::ffi::c_int) -> core::ffi::c_int { if xjc == -1 { -1 } else { (xjc as u8 as char).to_ascii_uppercase() as core::ffi::c_int } }",
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
            if self
                .xj_type_of_expr(cargs[0])
                .is_some_and(|g| type_is_char(&g.parsed))
            {
                self.with_cur_file_item_store(|item_store| {
                item_store.add_item_str_once(
                    "fn toascii_char_i(xjc: char) -> core::ffi::c_int { char::from_u32((xjc as u32) & 0x7f).unwrap() as core::ffi::c_int }",
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
            // Fallthrough: no guidance, or expr was not a simple variable.

            self.with_cur_file_item_store(|item_store| {
                item_store.add_item_str_once(
                    "fn xj_toascii(xjc: core::ffi::c_int) -> core::ffi::c_int { xjc & 0x7f }",
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
    fn recognize_preconversion_call_isinf(
        &self,
        ctx: ExprContext,
        cargs: &[CExprId],
    ) -> TranslationResult<Option<WithStmts<Box<Expr>>>> {
        if cargs.len() == 1 {
            self.with_cur_file_item_store(|item_store| {
                item_store.add_item_str_once(
                "fn xj_isinf(xjf: f64) -> core::ffi::c_int { if xjf.is_infinite() { 1 } else { 0 } }",
            );
            });
            let e1 = self.convert_expr(ctx.used(), cargs[0], None)?;
            let cast = mk().cast_expr(e1.to_expr(), mk().path_ty(vec!["f64"]));
            let call = mk().call_expr(mk().path_expr(vec!["xj_isinf"]), vec![cast]);
            return Ok(Some(WithStmts::new_val(call)));
        }
        Ok(None)
    }

    #[allow(clippy::borrowed_box)]
    fn recognize_preconversion_call_isnan(
        &self,
        ctx: ExprContext,
        cargs: &[CExprId],
    ) -> TranslationResult<Option<WithStmts<Box<Expr>>>> {
        if cargs.len() == 1 {
            self.with_cur_file_item_store(|item_store| {
                item_store.add_item_str_once(
                    "fn xj_isnan(xjf: f64) -> core::ffi::c_int { if xjf.is_nan() { 1 } else { 0 } }",
                );
            });
            let e1 = self.convert_expr(ctx.used(), cargs[0], None)?;
            let cast = mk().cast_expr(e1.to_expr(), mk().path_ty(vec!["f64"]));
            let call = mk().call_expr(mk().path_expr(vec!["xj_isnan"]), vec![cast]);
            return Ok(Some(WithStmts::new_val(call)));
        }
        Ok(None)
    }

    #[allow(clippy::borrowed_box)]
    fn recognize_preconversion_call_float_predicate(
        &self,
        ctx: ExprContext,
        cargs: &[CExprId],
        method_name: &str,
    ) -> TranslationResult<Option<WithStmts<Box<Expr>>>> {
        if cargs.len() == 1 {
            // Invoke the method on the translated operand itself.  Casting to
            // a common floating-point type can change classification (for
            // example, an f32 subnormal is a normal f64).
            self.import_num_traits(cargs[0])?;
            let value = self.convert_expr(ctx.used(), cargs[0], None)?;
            return Ok(Some(value.map(|value| {
                let predicate = mk().method_call_expr(value, method_name, Vec::new());
                mk().cast_expr(predicate, mk().abs_path_ty(vec!["core", "ffi", "c_int"]))
            })));
        }
        Ok(None)
    }

    #[allow(clippy::borrowed_box)]
    fn recognize_preconversion_call_float_comparison(
        &self,
        ctx: ExprContext,
        cargs: &[CExprId],
        comparison_name: &str,
    ) -> TranslationResult<Option<WithStmts<Box<Expr>>>> {
        if cargs.len() != 2 {
            return Ok(None);
        }

        self.import_num_traits(cargs[0])?;
        self.import_num_traits(cargs[1])?;
        let arg_type = |arg| {
            self.ast_context
                .index_unwrap_parens(arg)
                .kind
                .get_qual_type()
                .ok_or_else(|| format_err!("floating-point comparison argument has no type"))
        };
        let lhs_type = arg_type(cargs[0])?;
        let rhs_type = arg_type(cargs[1])?;
        let float_rank =
            |type_id: CQualTypeId| match self.ast_context.resolve_type(type_id.ctype).kind {
                CTypeKind::Float => Some(0),
                CTypeKind::Double => Some(1),
                CTypeKind::LongDouble | CTypeKind::Float128 => Some(2),
                _ => None,
            };
        let lhs_rank = float_rank(lhs_type)
            .ok_or_else(|| format_err!("left comparison argument is not floating-point"))?;
        let rhs_rank = float_rank(rhs_type)
            .ok_or_else(|| format_err!("right comparison argument is not floating-point"))?;
        let (common_type, common_rank) = if lhs_rank >= rhs_rank {
            (lhs_type, lhs_rank)
        } else {
            (rhs_type, rhs_rank)
        };
        let lhs = if lhs_rank == common_rank {
            self.convert_expr(ctx.used(), cargs[0], None)?
        } else {
            self.convert_expr_with_cast(ctx.used(), common_type, cargs[0])?
        };
        let rhs = if rhs_rank == common_rank {
            self.convert_expr(ctx.used(), cargs[1], None)?
        } else {
            self.convert_expr_with_cast(ctx.used(), common_type, cargs[1])?
        };

        let lhs_name = self
            .renamer
            .borrow_mut()
            .pick_name("c2rust_cmp_lhs", Namespaces::values());
        let rhs_name = self
            .renamer
            .borrow_mut()
            .pick_name("c2rust_cmp_rhs", Namespaces::values());
        let lhs_let = mk().local_stmt(Box::new(mk().local(
            mk().ident_pat(&lhs_name),
            None,
            Some(lhs.to_expr()),
        )));
        let rhs_let = mk().local_stmt(Box::new(mk().local(
            mk().ident_pat(&rhs_name),
            None,
            Some(rhs.to_expr()),
        )));

        let lhs = mk().ident_expr(&lhs_name);
        let rhs = mk().ident_expr(&rhs_name);
        let predicate = match comparison_name {
            "isgreater" => mk().binary_expr(BinOp::Gt(Default::default()), lhs, rhs),
            "isgreaterequal" => mk().binary_expr(BinOp::Ge(Default::default()), lhs, rhs),
            "isless" => mk().binary_expr(BinOp::Lt(Default::default()), lhs, rhs),
            "islessequal" => mk().binary_expr(BinOp::Le(Default::default()), lhs, rhs),
            "islessgreater" => {
                let less =
                    mk().binary_expr(BinOp::Lt(Default::default()), lhs.clone(), rhs.clone());
                let greater = mk().binary_expr(BinOp::Gt(Default::default()), lhs, rhs);
                mk().binary_expr(BinOp::Or(Default::default()), less, greater)
            }
            "isunordered" => {
                let lhs_is_nan = mk().method_call_expr(lhs, "is_nan", Vec::new());
                let rhs_is_nan = mk().method_call_expr(rhs, "is_nan", Vec::new());
                mk().binary_expr(BinOp::Or(Default::default()), lhs_is_nan, rhs_is_nan)
            }
            _ => unreachable!("unrecognized float comparison: {comparison_name}"),
        };
        let result = mk().cast_expr(predicate, mk().abs_path_ty(vec!["core", "ffi", "c_int"]));
        Ok(Some(WithStmts::new(vec![lhs_let, rhs_let], result)))
    }

    #[allow(clippy::borrowed_box)]
    fn recognize_preconversion_call_exit(
        &self,
        ctx: ExprContext,
        cargs: &[CExprId],
    ) -> TranslationResult<Option<WithStmts<Box<Expr>>>> {
        if cargs.len() == 1 {
            let c_int_val = self.convert_expr(ctx.used(), cargs[0], None)?;
            let as_i32 = mk().cast_expr(c_int_val.to_expr(), mk().path_ty(vec!["i32"]));
            let call = mk().call_expr(
                mk().abs_path_expr(vec!["std", "process", "exit"]),
                vec![as_i32],
            );
            return Ok(Some(WithStmts::new_val(call)));
        }

        Ok(None)
    }

    #[allow(clippy::borrowed_box)]
    fn recognize_preconversion_call_difftime(
        &self,
        ctx: ExprContext,
        cargs: &[CExprId],
    ) -> TranslationResult<Option<WithStmts<Box<Expr>>>> {
        if cargs.len() == 2 {
            self.use_crate(ExternCrate::XjCtime);
            let e1 = self.convert_expr(ctx.used(), cargs[0], None)?;
            let e2 = self.convert_expr(ctx.used(), cargs[1], None)?;
            let difftime_call = mk().call_expr(
                mk().path_expr(vec!["xj_ctime", "compat", "difftime"]),
                vec![e1.to_expr(), e2.to_expr()],
            );
            return Ok(Some(WithStmts::new_val(difftime_call)));
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
            if self
                .xj_type_of_expr(cargs[0])
                .is_some_and(|g| type_is_string(&g.parsed))
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
                        mk().mutbl().borrow_expr(buf.to_expr()),
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

            // XREF:guided_vec_memset_zero_mulsizeof
            let mb_dst_guided_type = self.xj_type_of_expr(arg0_sans_casts);
            if let Some(dst_guided_type) = mb_dst_guided_type {
                if !type_is_vec(dst_guided_type.strip_refs()) {
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
        if let Some(dst_type_id) = self.ast_context.index_unwrap_parens(*dst).kind.get_type() {
            if sized_expr.is_none() && Some(*elt_type) == self.c_type_pointee(dst_type_id) {
                // XREF:guided_vec_memset_zero_mulsizeof_ty
                return true;
            }

            // We can only proceed if the user wrote something sensible like memset(DST, 0, E * sizeof(*DST)).
            // For `memset(PTR, 0, E * sizeof(PTR))` we'd need to know the relationship between the target pointer size
            // and the size of the pointee type.
            if let Some(sized_expr) = sized_expr {
                return match &self.ast_context.index_unwrap_parens(*sized_expr).kind {
                    CExprKind::Unary(_, CUnOp::Deref, inner, _lrvalue) => {
                        // XREF:guided_vec_memset_zero_mulsizeof_deref
                        self.c_expr_decl_id(*inner) == self.c_expr_decl_id(*dst)
                    }
                    CExprKind::ArraySubscript(_cqt, base, _idx, _lrval) => {
                        self.c_expr_decl_id(*base) == self.c_expr_decl_id(*dst)
                    }
                    _ => false,
                };
            }
        };
        false
    }

    #[allow(clippy::borrowed_box)]
    fn recognize_preconversion_call_bitcastviamemcpy(
        &self,
        ctx: ExprContext,
        cargs: &[CExprId],
    ) -> TranslationResult<Option<WithStmts<Box<Expr>>>> {
        // xj-prepare-unionbitcasts produced a call to a synthesized wrapper
        // around memcpy for bitcasting between int/float types of the same size:
        //    F(S, [&]D)
        // The first parameter is the value being cast, and the second parameter
        // is the pointer to the destination value of the target type.
        // If S is an integer type of size NN, we want to produce
        //      D = fNN::from_bits(x);
        // and if it's a float type of size NN we want to produce
        //      D = x.to_bits() [as iNN];
        if cargs.len() == 2 {
            let src_expr_id = self.c_strip_implicit_casts(cargs[0]);
            let dst_expr_id = self.c_strip_implicit_casts(cargs[1]);

            let Some(src_type_id) = self
                .ast_context
                .index_unwrap_parens(src_expr_id)
                .kind
                .get_type()
            else {
                return Ok(None);
            };
            let Some(dst_ptr_type_id) = self
                .ast_context
                .index_unwrap_parens(dst_expr_id)
                .kind
                .get_type()
            else {
                return Ok(None);
            };
            let Some(dst_type_id) = self.c_type_pointee(dst_ptr_type_id) else {
                return Ok(None);
            };

            let src_kind = &self.ast_context.resolve_type(src_type_id).kind;
            let dst_kind = &self.ast_context.resolve_type(dst_type_id).kind;

            let int_bits = |k: &CTypeKind| -> Option<u32> {
                match k {
                    CTypeKind::Int | CTypeKind::UInt | CTypeKind::UInt32 => Some(32),
                    CTypeKind::Long
                    | CTypeKind::ULong
                    | CTypeKind::LongLong
                    | CTypeKind::ULongLong => Some(64),
                    _ => None,
                }
            };

            let float_bits = |k: &CTypeKind| -> Option<u32> {
                match k {
                    CTypeKind::Float => Some(32),
                    CTypeKind::Double => Some(64),
                    _ => None,
                }
            };

            let dst_addr_of = self.c_expr_get_addr_of(cargs[1]);
            let (dst_assign_expr_id, dst_is_ptr) = if let Some(inner) = dst_addr_of {
                (inner, false)
            } else {
                (dst_expr_id, true)
            };

            if let (Some(src_bits), Some(dst_float_bits)) =
                (int_bits(src_kind), float_bits(dst_kind))
            {
                if src_bits != dst_float_bits {
                    return Ok(None);
                }

                let src_expr = self.convert_expr(ctx.used(), src_expr_id, None)?;
                let dst_expr = self.convert_expr(ctx.used(), dst_assign_expr_id, None)?;

                let res = src_expr.and_then_try(move |src_expr| {
                    Ok(dst_expr.and_then(|dst_expr| {
                        let bits_ty = if dst_float_bits == 32 {
                            mk().path_ty(vec!["u32"])
                        } else {
                            mk().path_ty(vec!["u64"])
                        };
                        let bits_expr = mk().cast_expr(src_expr, bits_ty);
                        let float_ctor = if dst_float_bits == 32 {
                            mk().path_expr(vec!["f32", "from_bits"])
                        } else {
                            mk().path_expr(vec!["f64", "from_bits"])
                        };
                        let float_expr = mk().call_expr(float_ctor, vec![bits_expr]);
                        let lhs = if dst_is_ptr {
                            mk().unary_expr(UnOp::Deref(Default::default()), dst_expr)
                        } else {
                            dst_expr
                        };
                        WithStmts::new_val(mk().assign_expr(lhs, float_expr))
                    }))
                });

                return res.map(Some);
            }

            if let (Some(src_float_bits), Some(dst_bits)) =
                (float_bits(src_kind), int_bits(dst_kind))
            {
                if src_float_bits != dst_bits {
                    return Ok(None);
                }

                let src_expr = self.convert_expr(ctx.used(), src_expr_id, None)?;
                let dst_expr = self.convert_expr(ctx.used(), dst_assign_expr_id, None)?;
                let dst_ty = self.convert_type(dst_type_id)?;

                let res = src_expr.and_then_try(move |src_expr| {
                    let dst_ty = dst_ty.clone();
                    Ok(dst_expr.and_then(|dst_expr| {
                        let to_bits = mk().method_call_expr(src_expr, "to_bits", vec![]);
                        let casted = mk().cast_expr(to_bits, dst_ty);
                        let lhs = if dst_is_ptr {
                            mk().unary_expr(UnOp::Deref(Default::default()), dst_expr)
                        } else {
                            dst_expr
                        };
                        WithStmts::new_val(mk().assign_expr(lhs, casted))
                    }))
                });

                return res.map(Some);
            }
        }
        Ok(None)
    }

    /// Convert a C string literal to a Rust expression of type `String`
    pub fn convert_literal_to_rust_string(&self, val: &[u8], width: u8) -> Box<Expr> {
        if val.is_empty() {
            // XREF:guided_string_empty
            return mk().call_expr(mk().path_expr(vec!["String", "new"]), vec![]);
        }
        if let Some(s) = self.convert_literal_to_rust_str(val, width) {
            mk().call_expr(mk().path_expr(vec!["String", "from"]), vec![s])
        } else {
            log::warn!("TENJIN failed to losslessly convert C string literal to Rust str");
            mk().call_expr(
                mk().path_expr(vec!["String", "new"]),
                vec![mk().lit_expr(String::from_utf8_lossy(val).as_ref())],
            )
        }
    }

    /// Convert a C string literal to a Rust expression of type `&str`
    pub fn convert_literal_to_rust_str(&self, val: &[u8], _width: u8) -> Option<Box<Expr>> {
        if let Ok(s) = std::str::from_utf8(val) {
            return Some(mk().lit_expr(s));
        }
        None
    }

    /// A pointer offset as `isize`: cast with `as`, which later passes drop
    /// where it is trivial, rather than written as a suffixed literal. An
    /// enum is a newtype, so it takes C's conversion to get its integer out.
    pub fn convert_expr_as_offset(
        &self,
        ctx: ExprContext,
        target: CQualTypeId,
        expr: CExprId,
    ) -> TranslationResult<WithStmts<Box<Expr>>> {
        let is_enum = self.ast_context[expr]
            .kind
            .get_qual_type()
            .is_some_and(|t| {
                matches!(
                    self.ast_context.resolve_type(t.ctype).kind,
                    CTypeKind::Enum(_)
                )
            });
        if is_enum {
            return self.convert_expr_with_cast(ctx, target, expr);
        }
        Ok(self
            .convert_expr(ctx, expr, None)?
            .map(|offset| cast_int(offset, "isize", false)))
    }

    /// A C function's return type as a Rust one; `void` is omitted.
    fn convert_return_type(
        &self,
        return_type: Option<CQualTypeId>,
    ) -> TranslationResult<ReturnType> {
        let ret = match return_type {
            Some(return_type) => self.convert_type(return_type.ctype)?,
            None => mk().never_ty(),
        };
        let is_void_ret = return_type
            .map(|qty| self.ast_context[qty.ctype].kind == CTypeKind::Void)
            .unwrap_or(false);

        // If a return type is void, we should instead omit the unit type return,
        // -> (), to be more idiomatic
        if is_void_ret {
            Ok(ReturnType::Default)
        } else {
            Ok(ReturnType::Type(Default::default(), ret))
        }
    }

    pub fn generate_ffi_wrapper(
        &self,
        span: Span,
        new_name: &str,
        name: &str,
        arguments: &[(CDeclId, String, CQualTypeId)],
        return_type: Option<CQualTypeId>,
    ) -> TranslationResult<Box<Item>> {
        // Forwarding parameters / call arguments used to build the optional
        // `xj_ffi` export wrapper (XREF:ffi_export_wrapper).
        let mut ffi_wrapper_args: Vec<FnArg> = vec![];
        let mut ffi_wrapper_call_args: Vec<WithStmts<Box<Expr>>> = vec![];

        for &(_, ref var, typ) in arguments.iter() {
            let original_type = self.without_markers(|| self.convert_type(typ.ctype))?;
            let strategy = self
                .parsed_guidance
                .borrow()
                .query_ffi_in_conversion(name, var);
            let ffi_wrapper_arg_name = if var.is_empty() {
                self.renamer.borrow_mut().fresh(Namespaces::values())
            } else {
                var.clone()
            };
            ffi_wrapper_args
                .push(mk().arg(original_type.clone(), mk().ident_pat(&ffi_wrapper_arg_name)));
            ffi_wrapper_call_args.push(strategy.marshal(
                var,
                self,
                mk().path_expr(vec![&ffi_wrapper_arg_name]),
            ));
        }
        let ffi_wrapper_ret = self.without_markers(|| self.convert_return_type(return_type))?;

        let wrapper_decl = mk().fn_decl(new_name, ffi_wrapper_args, None, ffi_wrapper_ret);
        let wrappers: WithStmts<Vec<Box<Expr>>> = WithStmts::from_iter(ffi_wrapper_call_args);
        let (mut stmts, exprs) = wrappers.discard_unsafe();
        let wrapper_call = mk().call_expr(mk().path_expr(vec!["super", new_name]), exprs);
        let output_conversion = self.parsed_guidance.borrow().query_ffi_out_conversion(name);
        let convert_output = output_conversion.marshal(wrapper_call);
        stmts.push(mk().expr_stmt(convert_output));
        let wrapper_block = mk().block(stmts);
        // #[no_mangle]
        // fn foo(...) -> r {
        //   super::foo(*ffi_wrapper_call_args)
        // }
        Ok(mk_linkage(false, new_name, name, self.tcfg.edition)
            .pub_()
            .extern_("C")
            .span(span)
            .unsafe_()
            .fn_item(wrapper_decl, wrapper_block))
    }
}

#[cfg(test)]
mod guided_type_tests {
    use super::*;

    #[test]
    fn parse_vec() {
        let t = GuidedType::from_str("Vec<u8>").expect("Failed to parse 'Vec<u8>'?");
        assert!(type_is_vec(&t.parsed));
        assert!(try_type_vec_of(&t.parsed).is_some());
        assert!(type_is_vec_of_1_path(&t.parsed, "u8"));
    }

    #[test]
    fn parse_ref_vec() {
        let t1 = GuidedType::from_str("&Vec<u8>").expect("Failed to parse '&Vec<u8>'?");
        let t2 = GuidedType::from_str("&'a Vec<u8>").expect("Failed to parse '&'a Vec<u8>'?");
        let t3 = GuidedType::from_str("&mut Vec<u8>").expect("Failed to parse '&mut Vec<u8>'?");
        let t4 =
            GuidedType::from_str("&'a mut Vec<u8>").expect("Failed to parse '&'a mut Vec<u8>'?");
        let t5 = GuidedType::from_str("&'b &'a mut Vec<u8>")
            .expect("Failed to parse '&'b &'a mut Vec<u8>'?");
        for t in [t1, t2, t3, t4, t5] {
            assert!(type_is_vec(t.strip_refs()));
            assert!(try_type_vec_of(t.strip_refs()).is_some());
            assert!(type_is_vec_of_1_path(t.strip_refs(), "u8"));
        }
    }

    #[test]
    fn parse_char() {
        let t = GuidedType::from_str("char").expect("Failed to parse 'char'?");
        assert!(type_is_char(&t.parsed));
    }

    #[test]
    fn parse_string() {
        let t = GuidedType::from_str("String").expect("Failed to parse 'String'?");
        assert!(type_is_string(&t.parsed))
    }

    #[test]
    fn parse_exclusive_borrow() {
        let t1 = GuidedType::from_str("&mut i32").expect("failed to parse '&mut i32'!?");
        assert!(t1.is_borrow());
        assert!(!t1.is_shared_borrow());
        assert!(t1.is_exclusive_borrow());

        let t2 = GuidedType::from_str("&'a mut i32").expect("failed to parse '&'a mut i32'!?");
        assert!(t2.is_borrow());
        assert!(!t2.is_shared_borrow());
        assert!(t2.is_exclusive_borrow());
    }

    #[test]
    fn parse_borrow() {
        let t1 = GuidedType::from_str("&i32").expect("failed to parse '&i32'!?");
        assert!(t1.is_borrow());
        assert!(t1.is_shared_borrow());
        assert!(!t1.is_exclusive_borrow());

        let t2 = GuidedType::from_str("&'a i32").expect("failed to parse '&'a i32'!?");
        assert!(t2.is_borrow());
        assert!(t2.is_shared_borrow());
        assert!(!t2.is_exclusive_borrow());
    }
}
