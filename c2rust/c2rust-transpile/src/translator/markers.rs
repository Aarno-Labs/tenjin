//! Guidance that xj-prepare-guidance applied in the C.
//!
//! Guided declarations have marker typedefs (`xj_ty_<rust type>`) for C types, so
//! the Rust type of anything that reads them is in the typedef sugar of its
//! C type. Value flows that change representation or C type are calls to
//! marker functions: the name says what the flow does (`xj_slice_all`,
//! `xj_slice_from`, `xj_elem_ref`, `xj_coerce`, `xj_is_null`), and the
//! parameter and return types say between which types. Elements of guided
//! buffers are read through `(*xj_index_<b>(b, i))`, characters of guided
//! strings through `xj_char_at_<s>(s, i)`. This module reads both;
//! nothing here matches declarations against specifiers.

use super::*;
use crate::convert_type::TypeConverter;
use crate::translator::tenjin::{self, GuidedType};
use quote::ToTokens;
use std::str::FromStr;
use syn::GenericArgument;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum MarkerFamily {
    SliceAll,
    SliceFrom,
    ElemRef,
    Index,
    CharAt,
    Coerce,
    IsNull,
}

impl MarkerFamily {
    /// The family named by a `markers` entry in `xj-guidance.json`.
    pub fn of_family(family: &str) -> Option<Self> {
        match family {
            "slice_all" => Some(Self::SliceAll),
            "slice_from" => Some(Self::SliceFrom),
            "elem_ref" => Some(Self::ElemRef),
            "index" => Some(Self::Index),
            "char_at" => Some(Self::CharAt),
            "coerce" => Some(Self::Coerce),
            "is_null" => Some(Self::IsNull),
            _ => None,
        }
    }

    /// Markers whose argument is the value itself, which a recognizer that
    /// looks at the value may see through.
    fn is_identity(self) -> bool {
        matches!(self, Self::SliceAll | Self::Coerce)
    }
}

/// What a place marker's result is fitted to.
enum PlaceSink {
    Guided(Box<GuidedType>),
    Raw { mutable: bool },
}

impl PlaceSink {
    fn is_mutable(&self) -> bool {
        match self {
            Self::Guided(g) => g.is_exclusive_borrow(),
            Self::Raw { mutable } => *mutable,
        }
    }
}

/// The element of a buffer type: `[T]`, `[T; N]`, `Vec<T>` or `Box<[T]>`,
/// possibly behind references.
pub fn type_buffer_element(ty: &Type) -> Option<&Type> {
    match tenjin::type_strip_refs(ty) {
        Type::Slice(slice) => Some(&slice.elem),
        Type::Array(array) => Some(&array.elem),
        t if tenjin::type_is_vec(t) => tenjin::try_type_vec_of(t),
        t => generic_arg_of(t, "Box").and_then(|inner| match inner {
            Type::Slice(slice) => Some(&*slice.elem),
            _ => None,
        }),
    }
}

/// `T` of a one-argument generic path `name<T>`.
fn generic_arg_of<'a>(ty: &'a Type, name: &str) -> Option<&'a Type> {
    let path = tenjin::type_get_bare_path(ty)?;
    if !tenjin::is_path_exactly_1(path, name) {
        return None;
    }
    match tenjin::path_get_1_segment(path).and_then(tenjin::segment_get_1_bracket_argument)? {
        GenericArgument::Type(arg) => Some(arg),
        _ => None,
    }
}

/// Rust primitive numbers and C's numeric aliases (`c_int`, `size_t`, ...).
pub fn type_is_numeric(ty: &Type) -> bool {
    let Some(path) = tenjin::type_get_bare_path(ty) else {
        return false;
    };
    let Some(last) = path.segments.last() else {
        return false;
    };
    let name = last.ident.to_string();
    let primitive = matches!(
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
            | "f32"
            | "f64"
            | "bool"
            | "size_t"
            | "ssize_t"
    );
    primitive || (name.starts_with("c_") && name != "c_void")
}

/// The primitive a C numeric alias names on the LP64 targets Tenjin supports.
/// `c_char` is left out: its signedness differs between targets.
fn primitive_of_alias(name: &str) -> &str {
    match name {
        "c_schar" => "i8",
        "c_uchar" => "u8",
        "c_short" => "i16",
        "c_ushort" => "u16",
        "c_int" => "i32",
        "c_uint" => "u32",
        "c_long" | "c_longlong" => "i64",
        "c_ulong" | "c_ulonglong" => "u64",
        "c_float" => "f32",
        "c_double" => "f64",
        other => other,
    }
}

/// `a` and `b` are the same type, looking through C numeric aliases.
fn same_numeric_type(a: &Type, b: &Type) -> bool {
    let last = |t: &Type| {
        tenjin::type_get_bare_path(t)
            .and_then(|p| p.segments.last())
            .map(|s| primitive_of_alias(&s.ident.to_string()).to_string())
    };
    a == b || (type_is_numeric(a) && type_is_numeric(b) && last(a) == last(b))
}

/// `vec![a, b]` from `[a, b]` and `vec![x; n]` from `[x; n]`.
fn vec_of_array(val: &Expr) -> Option<Box<Expr>> {
    let tokens = match val {
        Expr::Array(array) => array.elems.to_token_stream(),
        Expr::Repeat(repeat) => {
            let (elem, len) = (&repeat.expr, &repeat.len);
            quote::quote!(#elem; #len)
        }
        _ => return None,
    };
    Some(mk().mac_expr(mk().mac(
        mk().path("vec"),
        tokens,
        MacroDelimiter::Bracket(Default::default()),
    )))
}

/// The `T` of `&T`, `&mut T` or `Box<T>`.
fn referent(g: &GuidedType) -> Option<&Type> {
    tenjin::type_of_ref(&g.parsed).or_else(|| generic_arg_of(&g.parsed, "Box"))
}

fn type_text(ty: &Type) -> String {
    ty.to_token_stream().to_string()
}

fn is_str_or_string(ty: &Type) -> bool {
    tenjin::type_is_string(ty) || tenjin::type_is_exactly_1_path(ty, "str")
}

/// Marks a coercion the table below does not know. The value keeps its own
/// type, so rustc still reports the mismatch, and the call names the pair.
fn unhandled_coercion(t: &Translation, val: Box<Expr>, from: &str, to: &str) -> Box<Expr> {
    t.with_cur_file_item_store(|store| {
        store.add_item_str_once("fn xj_unhandled_coercion<T>(x: T, _coercion: &str) -> T { x }");
    });
    let why = format!("{from} -> {to}");
    mk().call_expr(
        mk().path_expr(vec!["xj_unhandled_coercion"]),
        vec![val, mk().lit_expr(why.as_str())],
    )
}

fn borrow(val: Box<Expr>, mutable: bool) -> Box<Expr> {
    if mutable {
        mk().mutbl().borrow_expr(val)
    } else {
        mk().borrow_expr(val)
    }
}

fn reborrow(val: Box<Expr>, mutable: bool) -> Box<Expr> {
    borrow(
        mk().unary_expr(UnOp::Deref(Default::default()), val),
        mutable,
    )
}

fn method(val: Box<Expr>, name: &str) -> Box<Expr> {
    mk().method_call_expr(val, name, Vec::<Box<Expr>>::new())
}

fn as_ptr(val: Box<Expr>, mutable: bool) -> Box<Expr> {
    method(val, if mutable { "as_mut_ptr" } else { "as_ptr" })
}

fn guided_text(g: Option<&GuidedType>) -> String {
    g.map_or_else(|| "raw".to_string(), |g| g.pretty.clone())
}

/// A marker typedef as the Rust type guidance gave it, or as the C type
/// behind it where the C ABI must be kept: the typedef has no Rust item.
pub fn convert_marker_typedef(
    converter: &mut TypeConverter,
    ctxt: &TypedAstContext,
    decl_id: CDeclId,
    pg: &ParsedGuidance,
) -> Option<TranslationResult<Box<Type>>> {
    let CDeclKind::Typedef { name, typ, .. } = &ctxt[decl_id].kind else {
        return None;
    };
    let guided = pg.marker_typedefs.get(name)?;
    if pg.c_abi_markers.get() {
        return Some(converter.convert(ctxt, typ.ctype, pg));
    }
    Some(Ok(Box::new(guided.parsed.clone())))
}

impl Translation<'_> {
    /// The Rust type a marker typedef declaration stands for.
    pub fn marker_typedef(&self, decl: CDeclId) -> Option<GuidedType> {
        match &self.ast_context.get_decl(&decl)?.kind {
            CDeclKind::Typedef { name, .. } => self
                .parsed_guidance
                .borrow()
                .marker_typedefs
                .get(name)
                .cloned(),
            _ => None,
        }
    }

    /// The Rust type named by the marker typedef in `ctype`'s sugar, or for a
    /// pointer to such a typedef, a raw pointer to it.
    pub fn xj_type_of_ctype(&self, ctype: CTypeId) -> Option<GuidedType> {
        if let Some(g) = self.xj_sugar(ctype) {
            return Some(g);
        }
        let pointee = match self.ast_context.resolve_type(ctype).kind {
            CTypeKind::Pointer(pointee) => pointee,
            _ => return None,
        };
        let inner = self.xj_sugar(pointee.ctype)?;
        let ptr = if pointee.qualifiers.is_const {
            "*const"
        } else {
            "*mut"
        };
        GuidedType::from_str(&format!("{ptr} {}", inner.pretty)).ok()
    }

    fn xj_sugar(&self, mut ctype: CTypeId) -> Option<GuidedType> {
        loop {
            ctype = match self.ast_context[ctype].kind {
                CTypeKind::Typedef(decl) => {
                    if let Some(g) = self.marker_typedef(decl) {
                        return Some(g);
                    }
                    match self.ast_context[decl].kind {
                        CDeclKind::Typedef { typ, .. } => typ.ctype,
                        _ => return None,
                    }
                }
                CTypeKind::Elaborated(t) | CTypeKind::Paren(t) => t,
                CTypeKind::Attributed(t, _) => t.ctype,
                _ => return None,
            };
        }
    }

    /// Drops the `Copy` and `Clone` derives a record's guided fields rule
    /// out: owned guidance is not `Copy`, and an exclusive reference is
    /// neither.
    pub fn drop_guided_derives(&self, fields: &[CDeclId], derives: &mut Vec<syn::Meta>) {
        let (mut copy, mut clone) = (true, true);
        for guided in fields.iter().filter_map(|f| self.xj_type_of_decl(*f)) {
            let ty = &guided.parsed;
            if guided.is_exclusive_borrow() {
                (copy, clone) = (false, false);
            } else if tenjin::type_is_string(ty)
                || tenjin::type_is_vec(ty)
                || tenjin::type_is_exactly_1_path(ty, "Box")
            {
                copy = false;
            }
        }
        derives.retain(|d| {
            let is = |name| d.path().is_ident(name);
            (copy || !is("Copy")) && (clone || !is("Clone"))
        });
    }

    /// A value guided as something other than a raw pointer: the base of
    /// `p->f` is then the object itself, as in `s.f`.
    pub fn is_guided_object(&self, expr: CExprId) -> bool {
        self.xj_type_of_expr(expr)
            .is_some_and(|g| !g.pretty.starts_with('*'))
    }

    /// The guidance an expression carries: its type's marker sugar, looking
    /// through parentheses and the casts that keep a value (clang drops the
    /// sugar at array decay).
    pub fn xj_type_of_expr(&self, expr: CExprId) -> Option<GuidedType> {
        let mut current = expr;
        loop {
            let kind = &self.ast_context.index_unwrap_parens(current).kind;
            if let Some(g) = kind.get_type().and_then(|t| self.xj_type_of_ctype(t)) {
                return Some(g);
            }
            current = match *kind {
                CExprKind::ImplicitCast(
                    _,
                    inner,
                    CastKind::LValueToRValue | CastKind::NoOp | CastKind::ArrayToPointerDecay,
                    _,
                    _,
                ) => inner,
                _ => return None,
            };
        }
    }

    pub fn xj_type_of_decl(&self, decl: CDeclId) -> Option<GuidedType> {
        match self.ast_context.get_decl(&decl)?.kind {
            CDeclKind::Variable { typ, .. } | CDeclKind::Field { typ, .. } => {
                self.xj_type_of_ctype(typ.ctype)
            }
            _ => None,
        }
    }

    /// The family of the marker function `name`, as the C pass declared it.
    fn marker_family(&self, name: &str) -> Option<MarkerFamily> {
        self.parsed_guidance.borrow().markers.get(name).copied()
    }

    /// The marker function a call expression's callee names.
    pub fn marker_callee(&self, func: CExprId) -> Option<(MarkerFamily, CDeclId)> {
        let CExprKind::ImplicitCast(_, fexp, CastKind::FunctionToPointerDecay, _, _) =
            self.ast_context.index_unwrap_parens(func).kind
        else {
            return None;
        };
        let CExprKind::DeclRef(_, decl, _) = self.ast_context.index_unwrap_parens(fexp).kind else {
            return None;
        };
        match &self.ast_context[decl].kind {
            CDeclKind::Function { name, .. } => Some((self.marker_family(name)?, decl)),
            _ => None,
        }
    }

    /// A marker typedef or marker function declaration. It has no Rust item:
    /// uses are translated in their place.
    pub fn is_marker_decl(&self, decl: CDeclId) -> bool {
        match &self.ast_context[decl].kind {
            CDeclKind::Typedef { .. } => self.marker_typedef(decl).is_some(),
            CDeclKind::Function { name, .. } => self.marker_family(name).is_some(),
            _ => false,
        }
    }

    /// What a marker typedef's Rust type needs imported: the imports of the
    /// C type behind it.
    pub(super) fn marker_typedef_imports(&self, decl: CDeclId) -> IndexSet<Import> {
        match self.ast_context[decl].kind {
            CDeclKind::Typedef { typ, .. } => self.imports_for_type(typ.ctype),
            _ => IndexSet::default(),
        }
    }

    /// `expr` without an enclosing identity marker, so a recognizer that
    /// inspects an argument sees the value the program wrote.
    pub fn strip_identity_markers(&self, expr: CExprId) -> CExprId {
        let stripped = self.c_strip_implicit_casts(expr);
        match self.ast_context.index_unwrap_parens(stripped).kind {
            CExprKind::Call(_, func, ref args) if args.len() == 1 => {
                match self.marker_callee(func) {
                    Some((family, _)) if family.is_identity() => args[0],
                    _ => expr,
                }
            }
            _ => expr,
        }
    }

    pub fn strip_identity_markers_all(&self, exprs: &[CExprId]) -> Vec<CExprId> {
        exprs
            .iter()
            .map(|e| self.strip_identity_markers(*e))
            .collect()
    }

    /// Parameter types and return type of a marker function.
    fn marker_signature(&self, callee: CDeclId) -> Option<(Vec<CQualTypeId>, CQualTypeId)> {
        let CDeclKind::Function { typ, .. } = self.ast_context[callee].kind else {
            return None;
        };
        match &self.ast_context.resolve_type(typ).kind {
            CTypeKind::Function(ret, params, ..) => Some((params.clone(), *ret)),
            _ => None,
        }
    }

    pub fn convert_marker_call(
        &self,
        ctx: ExprContext,
        family: MarkerFamily,
        callee: CDeclId,
        cargs: &[CExprId],
    ) -> TranslationResult<WithStmts<Box<Expr>>> {
        let (params, ret) = self
            .marker_signature(callee)
            .ok_or_else(|| format_err!("marker function without a prototype"))?;
        match family {
            MarkerFamily::SliceAll | MarkerFamily::SliceFrom | MarkerFamily::ElemRef => {
                self.convert_place_marker(ctx, family, ret, cargs)
            }
            MarkerFamily::Index => self.convert_index_address(ctx, ret, cargs),
            MarkerFamily::CharAt => self.convert_char_at(ctx, ret, cargs),
            MarkerFamily::Coerce => {
                let from = self
                    .xj_type_of_expr(cargs[0])
                    .or_else(|| self.xj_type_of_ctype(params[0].ctype));
                let to = self.xj_type_of_ctype(ret.ctype);
                if matches!((&from, &to), (Some(f), Some(t)) if f.parsed == t.parsed) {
                    return self.convert_guided_value(ctx, cargs[0]);
                }
                self.coerce_value(ctx, cargs[0], from.as_ref(), to.as_ref(), ret)
            }
            MarkerFamily::IsNull => self.convert_is_null_marker(ctx, cargs[0]),
        }
    }

    fn place_sink(&self, ret: CQualTypeId) -> PlaceSink {
        if let Some(g) = self.xj_type_of_ctype(ret.ctype) {
            return PlaceSink::Guided(Box::new(g));
        }
        let mutable = match self.ast_context.resolve_type(ret.ctype).kind {
            CTypeKind::Pointer(pointee) => !pointee.qualifiers.is_const,
            _ => false,
        };
        PlaceSink::Raw { mutable }
    }

    /// `xj_slice_all(b)`, `xj_slice_from(b, i)`, `xj_elem_ref(b, i)`: the
    /// marker name is the shape, the return type the demand, and the base's
    /// guidance whether it is owned or already a reference.
    fn convert_place_marker(
        &self,
        ctx: ExprContext,
        family: MarkerFamily,
        ret: CQualTypeId,
        cargs: &[CExprId],
    ) -> TranslationResult<WithStmts<Box<Expr>>> {
        let sink = self.place_sink(ret);
        let base_id = self.c_strip_noop_casts(cargs[0]);
        let base = self.convert_place_base(ctx, base_id)?;
        let index = match cargs.get(1) {
            Some(&i) => Some(self.convert_marker_index(ctx, i)?),
            None => None,
        };
        let base_is_ref = self.xj_type_of_expr(base_id).is_some_and(|g| g.is_borrow());
        let place = match index {
            None => base.map(|b| Self::whole_place(&sink, b, base_is_ref)),
            Some(index) => base
                .zip(index)
                .map(|(b, i)| Self::offset_place(family, &sink, b, i)),
        };
        match (&sink, self.raw_place_cast(base_id, ret)?) {
            (PlaceSink::Raw { .. }, Some(ty)) => Ok(place.map(|p| mk().cast_expr(p, ty))),
            _ => Ok(place),
        }
    }

    /// A marker's `long i` argument as a Rust index. An enum is a newtype,
    /// so its integer is taken out by a C conversion to `size_t`.
    fn convert_marker_index(
        &self,
        ctx: ExprContext,
        index: CExprId,
    ) -> TranslationResult<WithStmts<Box<Expr>>> {
        let index = self.c_strip_implicit_casts(index);
        let is_enum = self.ast_context[index]
            .kind
            .get_type()
            .is_some_and(|t| matches!(self.ast_context.resolve_type(t).kind, CTypeKind::Enum(_)));
        if is_enum {
            let size = self.ast_context.type_for_kind(&CTypeKind::Size);
            return self.convert_expr_with_cast(ctx.used(), CQualTypeId::new(size), index);
        }
        Ok(self
            .convert_expr(ctx.used(), index, None)?
            .map(|i| cast_int(i, "usize", false)))
    }

    /// The value of a guided expression without the C conversions around
    /// it: a guided value is its own representation.
    fn convert_guided_value(
        &self,
        ctx: ExprContext,
        expr: CExprId,
    ) -> TranslationResult<WithStmts<Box<Expr>>> {
        self.convert_expr(ctx.used(), self.c_strip_noop_casts(expr), None)
    }

    /// The base and index of a call `xj_index_<b>(b, i)`, through parentheses
    /// and implicit casts.
    pub fn index_marker_operands(&self, expr: CExprId) -> Option<(CExprId, CExprId)> {
        let mut current = expr;
        loop {
            match self.ast_context.index_unwrap_parens(current).kind {
                CExprKind::ImplicitCast(_, inner, _, _, _) => current = inner,
                CExprKind::Call(_, func, ref args) if args.len() == 2 => {
                    let (family, _) = self.marker_callee(func)?;
                    return (family == MarkerFamily::Index).then(|| (args[0], args[1]));
                }
                _ => return None,
            }
        }
    }

    /// `TypedAstContext::is_expr_pure`, except that a marker call is pure when
    /// its arguments are, so `(*xj_index_<p>(p, i)) += 1` names its place
    /// directly. Covers the forms an lvalue is built from; anything else is
    /// left to `is_expr_pure`.
    pub fn is_pure_with_markers(&self, expr: CExprId) -> bool {
        let pure = |e| self.is_pure_with_markers(e);
        match self.ast_context[expr].kind {
            CExprKind::Call(_, func, ref args) if self.marker_callee(func).is_some() => {
                args.iter().all(|&a| pure(a))
            }
            CExprKind::Paren(_, e)
            | CExprKind::ImplicitCast(_, e, _, _, _)
            | CExprKind::ExplicitCast(_, e, _, _, _)
            | CExprKind::Member(_, e, _, _, _) => pure(e),
            CExprKind::Unary(_, op, e, _)
                if !matches!(
                    op,
                    CUnOp::PreIncrement
                        | CUnOp::PostIncrement
                        | CUnOp::PreDecrement
                        | CUnOp::PostDecrement
                ) =>
            {
                pure(e)
            }
            CExprKind::ArraySubscript(_, lhs, rhs, _) => pure(lhs) && pure(rhs),
            CExprKind::Binary(_, op, lhs, rhs, _, _)
                if op != CBinOp::Assign && op.underlying_assignment().is_none() =>
            {
                pure(lhs) && pure(rhs)
            }
            _ => self.ast_context.is_expr_pure(expr),
        }
    }

    /// `(*xj_index_<b>(b, i))`: element `i` of the buffer `b`.
    pub fn convert_index_deref(
        &self,
        ctx: ExprContext,
        base: CExprId,
        index: CExprId,
    ) -> TranslationResult<WithStmts<Box<Expr>>> {
        let base = self.convert_guided_value(ctx, base)?;
        let index = self.convert_marker_index(ctx, index)?;
        Ok(base.zip(index).map(|(b, i)| mk().index_expr(b, i)))
    }

    /// `xj_index_<b>(b, i)` itself, as in `&b[i]`: a raw pointer to the
    /// element, mutable unless the C pointee is const or `b` a shared borrow.
    fn convert_index_address(
        &self,
        ctx: ExprContext,
        ret: CQualTypeId,
        cargs: &[CExprId],
    ) -> TranslationResult<WithStmts<Box<Expr>>> {
        let base_id = self.c_strip_noop_casts(cargs[0]);
        let shared = self
            .xj_type_of_expr(base_id)
            .is_some_and(|g| g.is_shared_borrow());
        let const_pointee = match self.ast_context.resolve_type(ret.ctype).kind {
            CTypeKind::Pointer(pointee) => pointee.qualifiers.is_const,
            _ => false,
        };
        let sink = PlaceSink::Raw {
            mutable: !const_pointee && !shared,
        };
        let base = self.convert_guided_value(ctx, base_id)?;
        let index = self.convert_marker_index(ctx, cargs[1])?;
        let place = base
            .zip(index)
            .map(|(b, i)| Self::offset_place(MarkerFamily::ElemRef, &sink, b, i));
        match self.raw_place_cast(base_id, ret)? {
            Some(ty) => Ok(place.map(|p| mk().cast_expr(p, ty))),
            None => Ok(place),
        }
    }

    /// `xj_char_at_<s>(s, i)`: byte `i` of a guided string as a C character,
    /// or 0 at and past its end, where C would read the terminator.
    fn convert_char_at(
        &self,
        ctx: ExprContext,
        ret: CQualTypeId,
        cargs: &[CExprId],
    ) -> TranslationResult<WithStmts<Box<Expr>>> {
        let ret_ty = self.convert_type(ret.ctype)?;
        let string = self.convert_guided_value(ctx, cargs[0])?;
        let index = self.convert_marker_index(ctx, cargs[1])?;
        Ok(string.zip(index).map(|(s, i)| {
            let bytes = method(Self::borrowed_base(s, false), "as_bytes");
            let byte = method(mk().method_call_expr(bytes, "get", vec![i]), "copied");
            let zero = mk().lit_expr(mk().int_unsuffixed_lit(0));
            mk().cast_expr(mk().method_call_expr(byte, "unwrap_or", vec![zero]), ret_ty)
        }))
    }

    /// The pointer type a raw place must be cast to, when the buffer's Rust
    /// element (`u8` for `&[u8]`) is not the C pointee (`c_char`).
    fn raw_place_cast(
        &self,
        base_id: CExprId,
        ret: CQualTypeId,
    ) -> TranslationResult<Option<Box<Type>>> {
        let ret_ty = self.convert_type(ret.ctype)?;
        let Type::Ptr(ref ptr) = *ret_ty else {
            return Ok(None);
        };
        let element = match self.xj_type_of_expr(base_id) {
            Some(g) => type_buffer_element(&g.parsed).cloned().map(Box::new),
            None => match self.ast_context[base_id].kind.get_type() {
                Some(t) => match self.ast_context.resolve_type(t).kind.element_ty() {
                    Some(elt) => Some(self.convert_type(elt)?),
                    None => None,
                },
                None => None,
            },
        };
        Ok(element
            .filter(|e| !same_numeric_type(e, &ptr.elem))
            .map(|_| ret_ty.clone()))
    }

    /// The base of a place, undecayed. A string literal base is its byte
    /// string, which is already a reference.
    fn convert_place_base(
        &self,
        ctx: ExprContext,
        base_id: CExprId,
    ) -> TranslationResult<WithStmts<Box<Expr>>> {
        if let Some(CLiteral::String(bytes, 1)) = self.get_string_lit(base_id) {
            let ty = self.ast_context[base_id].kind.get_type();
            let padded = match ty {
                Some(ty) => self.string_literal_bytes(ty, bytes, 1),
                None => bytes.clone(),
            };
            return Ok(WithStmts::new_val(mk().lit_expr(padded)));
        }
        self.convert_expr(ctx.used(), base_id, None)
    }

    /// A reference base, or a byte string literal, is already a reference;
    /// the sink is a coercion site, which reborrows it implicitly.
    fn whole_place(sink: &PlaceSink, base: Box<Expr>, base_is_ref: bool) -> Box<Expr> {
        let mutable = sink.is_mutable();
        match sink {
            PlaceSink::Raw { .. } => as_ptr(Self::borrowed_base(base, mutable), mutable),
            PlaceSink::Guided(_) if base_is_ref || tenjin::expr_is_lit_str_or_bytes(&base) => base,
            PlaceSink::Guided(_) => borrow(base, mutable),
        }
    }

    /// A base reached through a raw pointer (`(*s).buf`) is borrowed
    /// explicitly before it is indexed or has a method called on it; the
    /// implicit borrow there is an error on newer toolchains.
    fn borrowed_base(base: Box<Expr>, mutable: bool) -> Box<Expr> {
        let direct = matches!(*base, Expr::Path(_) | Expr::Lit(_) | Expr::Reference(_));
        if direct {
            base
        } else {
            mk().paren_expr(borrow(base, mutable))
        }
    }

    fn offset_place(
        family: MarkerFamily,
        sink: &PlaceSink,
        base: Box<Expr>,
        index: Box<Expr>,
    ) -> Box<Expr> {
        let mutable = sink.is_mutable();
        let base = Self::borrowed_base(base, mutable);
        if family == MarkerFamily::ElemRef {
            let elem = mk().index_expr(base, index);
            return match sink {
                PlaceSink::Guided(_) => borrow(elem, mutable),
                PlaceSink::Raw { .. } => {
                    let mutbl = if mutable {
                        Mutability::Mutable
                    } else {
                        Mutability::Immutable
                    };
                    mk().set_mutbl(mutbl).raw_borrow_expr(elem)
                }
            };
        }
        let suffix = mk().index_expr(base, mk().range_expr(Some(index), None));
        match sink {
            PlaceSink::Guided(_) => borrow(suffix, mutable),
            PlaceSink::Raw { .. } => as_ptr(suffix, mutable),
        }
    }

    // XREF:guided_condition_string_null_check_neq
    fn convert_is_null_marker(
        &self,
        ctx: ExprContext,
        arg: CExprId,
    ) -> TranslationResult<WithStmts<Box<Expr>>> {
        let guided = self.xj_type_of_expr(arg);
        let val = self.convert_expr(ctx.used(), arg, None)?;
        Ok(val.map(|v| match guided {
            Some(g) if generic_arg_of(&g.parsed, "Option").is_some() => method(v, "is_none"),
            Some(g)
                if type_buffer_element(&g.parsed).is_some()
                    || is_str_or_string(tenjin::type_strip_refs(&g.parsed)) =>
            {
                method(v, "is_empty")
            }
            Some(_) => mk().lit_expr(mk().bool_lit(false)),
            None => method(v, "is_null"),
        }))
    }

    /// Fits the value of `arg` to `to` (or to the unguided C type `to_cty`).
    /// Used by `xj_coerce` markers and by initializers the C pass could not
    /// wrap (constant contexts, array initializers).
    pub fn coerce_value(
        &self,
        ctx: ExprContext,
        arg: CExprId,
        from: Option<&GuidedType>,
        to: Option<&GuidedType>,
        to_cty: CQualTypeId,
    ) -> TranslationResult<WithStmts<Box<Expr>>> {
        if let Some(to) = to {
            if let Some(val) = self.coerce_constant(arg, to) {
                return Ok(WithStmts::new_val(val));
            }
            if let Some(inner) = self.address_of_operand(arg) {
                return self.coerce_address_of(ctx, inner, from, to);
            }
        }
        let val = self.convert_expr(ctx.used(), arg, None)?;
        match to {
            Some(to) => Ok(val.map(|v| self.coerce_to_guided(v, from, to))),
            None => {
                let to_ty = self.convert_type(to_cty.ctype)?;
                let same_pointee = self.ast_context[arg]
                    .kind
                    .get_type()
                    .is_some_and(|from_ty| self.same_c_pointee(from_ty, to_cty.ctype));
                Ok(val.map(|v| self.coerce_to_c(v, from, to_ty, same_pointee)))
            }
        }
    }

    /// Null constants and literals written as the guided value directly.
    fn coerce_constant(&self, arg: CExprId, to: &GuidedType) -> Option<Box<Expr>> {
        if self.ast_context.is_null_expr(arg) {
            return Self::guided_empty(to);
        }
        let core = self.c_strip_implicit_casts(arg);
        if let Some(CLiteral::String(bytes, width)) = self.get_string_lit(core) {
            return self.guided_string_literal(core, bytes, *width, to);
        }
        let CExprKind::Literal(_, CLiteral::Integer(value, _) | CLiteral::Character(value)) =
            self.ast_context.index_unwrap_parens(core).kind
        else {
            return None;
        };
        if tenjin::type_is_char(&to.parsed) {
            return char::from_u32(value as u32).map(|c| mk().lit_expr(c));
        }
        None
    }

    /// The value a guided type takes for C's null pointer.
    fn guided_empty(to: &GuidedType) -> Option<Box<Expr>> {
        let ty = &to.parsed;
        let call = |path: Vec<&str>| Some(mk().call_expr(mk().path_expr(path), vec![]));
        if tenjin::type_is_string(ty) {
            return call(vec!["String", "new"]);
        }
        if tenjin::type_is_vec(ty) {
            return call(vec!["Vec", "new"]);
        }
        if generic_arg_of(ty, "Option").is_some() {
            return Some(mk().path_expr(vec!["None"]));
        }
        if tenjin::type_is_str_ref(ty) {
            return Some(mk().lit_expr(""));
        }
        if tenjin::type_of_slice_ref(ty).is_some() {
            return Some(borrow(mk().array_expr(vec![]), to.is_exclusive_borrow()));
        }
        None
    }

    fn guided_string_literal(
        &self,
        lit: CExprId,
        bytes: &[u8],
        width: u8,
        to: &GuidedType,
    ) -> Option<Box<Expr>> {
        let ty = &to.parsed;
        if tenjin::type_is_string(ty) {
            // XREF:snapshot_guided_ret_ostr
            return Some(self.convert_literal_to_rust_string(bytes, width));
        }
        if tenjin::type_is_str_ref(ty) {
            return self.convert_literal_to_rust_str(bytes, width);
        }
        if width != 1 {
            return None;
        }
        let padded = match self.ast_context[lit].kind.get_type() {
            Some(t) => self.string_literal_bytes(t, bytes, 1),
            None => bytes.to_vec(),
        };
        if tenjin::type_is_vec(ty) {
            return Some(method(mk().lit_expr(padded), "to_vec"));
        }
        if tenjin::type_of_slice_ref(ty).is_some() && to.is_shared_borrow() {
            return Some(mk().lit_expr(padded));
        }
        None
    }

    fn address_of_operand(&self, arg: CExprId) -> Option<CExprId> {
        match self
            .ast_context
            .index_unwrap_parens(self.c_strip_implicit_casts(arg))
            .kind
        {
            CExprKind::Unary(_, CUnOp::AddressOf, inner, _) => Some(inner),
            _ => None,
        }
    }

    /// `&x` into a reference: borrow `x` directly instead of taking a raw
    /// address and converting it back.
    fn coerce_address_of(
        &self,
        ctx: ExprContext,
        inner: CExprId,
        from: Option<&GuidedType>,
        to: &GuidedType,
    ) -> TranslationResult<WithStmts<Box<Expr>>> {
        let val = self.convert_expr(ctx.used(), inner, None)?;
        if !to.is_borrow() {
            return Ok(val.map(|v| {
                unhandled_coercion(self, borrow(v, true), &guided_text(from), &to.pretty)
            }));
        }
        Ok(val.map(|v| borrow(v, to.is_exclusive_borrow())))
    }

    fn coerce_to_guided(
        &self,
        val: Box<Expr>,
        from: Option<&GuidedType>,
        to: &GuidedType,
    ) -> Box<Expr> {
        let converted = match from {
            Some(from) => Self::guided_to_guided(val.clone(), from, to),
            None => Self::raw_to_guided(val.clone(), to),
        };
        converted.unwrap_or_else(|| unhandled_coercion(self, val, &guided_text(from), &to.pretty))
    }

    fn raw_to_guided(val: Box<Expr>, to: &GuidedType) -> Option<Box<Expr>> {
        let ty = &to.parsed;
        if tenjin::type_is_vec(ty) {
            return vec_of_array(&val);
        }
        if matches!(ty, Type::Array(_)) {
            return Some(val);
        }
        if type_is_numeric(ty) {
            return Some(mk().cast_expr(val, Box::new(ty.clone())));
        }
        if to.is_borrow() && type_buffer_element(ty).is_none() && !tenjin::type_is_str_ref(ty) {
            let opt = method(
                val,
                if to.is_exclusive_borrow() {
                    "as_mut"
                } else {
                    "as_ref"
                },
            );
            return Some(method(opt, "unwrap"));
        }
        if tenjin::type_is_char(ty) {
            let byte = mk().cast_expr(val, mk().path_ty(vec!["u8"]));
            return Some(mk().cast_expr(byte, mk().path_ty(vec!["char"])));
        }
        None
    }

    fn guided_to_guided(val: Box<Expr>, from: &GuidedType, to: &GuidedType) -> Option<Box<Expr>> {
        let (f, t) = (&from.parsed, &to.parsed);
        let mutable = to.is_exclusive_borrow();
        if to.is_borrow() && !from.is_borrow() {
            // An owned value into a borrow of itself, or of what it derefs to.
            return Some(borrow(val, mutable));
        }
        if to.is_borrow() && from.is_borrow() {
            return Some(reborrow(val, mutable));
        }
        if tenjin::type_is_string(t) && (tenjin::type_is_str_ref(f) || tenjin::type_is_char(f)) {
            return Some(method(val, "to_string"));
        }
        if tenjin::type_is_string(t) && tenjin::type_is_string(tenjin::type_strip_refs(f)) {
            return Some(method(val, "clone"));
        }
        if tenjin::type_is_vec(t) && from.is_borrow() && type_buffer_element(f).is_some() {
            return Some(method(val, "to_vec"));
        }
        if generic_arg_of(t, "Option") == Some(f) {
            return Some(mk().call_expr(mk().path_expr(vec!["Some"]), vec![val]));
        }
        if tenjin::type_is_char(t) || tenjin::type_is_char(f) {
            return Some(mk().cast_expr(val, Box::new(t.clone())));
        }
        None
    }

    /// Both are pointers to the same C type.
    fn same_c_pointee(&self, a: CTypeId, b: CTypeId) -> bool {
        let pointee = |t: CTypeId| match self.ast_context.resolve_type(t).kind {
            CTypeKind::Pointer(q) => Some(self.ast_context.resolve_type_id(q.ctype)),
            _ => None,
        };
        pointee(a).is_some() && pointee(a) == pointee(b)
    }

    /// A guided value into an unguided C type. `same_pointee` says the C
    /// types on both sides point to the same object type.
    fn coerce_to_c(
        &self,
        val: Box<Expr>,
        from: Option<&GuidedType>,
        to: Box<Type>,
        same_pointee: bool,
    ) -> Box<Expr> {
        let Some(from) = from else {
            return val;
        };
        let f = &from.parsed;
        let Type::Ptr(ref to_ptr) = *to else {
            if from.is_borrow() || is_str_or_string(f) || type_buffer_element(f).is_some() {
                return unhandled_coercion(self, val, &from.pretty, &type_text(&to));
            }
            // Scalars, including Rust `char`, convert with `as`.
            return mk().cast_expr(val, to);
        };
        let to_mut = to_ptr.mutability.is_some();
        if type_buffer_element(f).is_some() {
            return mk().cast_expr(as_ptr(val, to_mut), to);
        }
        if let Type::Ptr(_) = f {
            return mk().cast_expr(val, to);
        }
        match referent(from) {
            Some(target) if !tenjin::type_is_exactly_1_path(target, "str") => {
                Self::ref_to_raw(val, from, target, to, same_pointee)
            }
            _ => unhandled_coercion(self, val, &from.pretty, &type_text(&to)),
        }
    }

    /// `&T`, `&mut T` or `Box<T>` into a raw pointer. At a coercion site Rust
    /// turns a reference into a raw pointer to the same type by itself, but
    /// only a `&mut` into a `*mut`.
    fn ref_to_raw(
        val: Box<Expr>,
        from: &GuidedType,
        target: &Type,
        to: Box<Type>,
        same_pointee: bool,
    ) -> Box<Expr> {
        let mutable = from.is_exclusive_borrow() || generic_arg_of(&from.parsed, "Box").is_some();
        let to_mut = matches!(*to, Type::Ptr(ref p) if p.mutability.is_some());
        let val = if from.is_borrow() {
            val
        } else {
            reborrow(val, mutable)
        };
        if same_pointee && from.is_borrow() && (mutable || !to_mut) {
            return val;
        }
        let raw = if mutable {
            mk().mutbl().ptr_ty(Box::new(target.clone()))
        } else {
            mk().ptr_ty(Box::new(target.clone()))
        };
        mk().cast_expr(mk().cast_expr(val, raw), to)
    }

    /// The default of a guided type C leaves zeroed: `String::new()`, an
    /// empty `Vec`, `None`; a `Vec` in place of a C array keeps the array's
    /// length, since code indexes into it later.
    pub fn guided_implicit_default(
        &self,
        ctx: ExprContext,
        ty_id: CTypeId,
    ) -> TranslationResult<Option<WithStmts<Box<Expr>>>> {
        let Some(guided) = self.xj_type_of_ctype(ty_id) else {
            return Ok(None);
        };
        let is_array = matches!(
            self.ast_context.resolve_type(ty_id).kind,
            CTypeKind::ConstantArray(..)
        );
        if tenjin::type_is_vec(&guided.parsed) && is_array {
            // The resolved type has no marker sugar, so this is the C array.
            let array_ty = self.ast_context.resolve_type_id(ty_id);
            let array = self.implicit_default_expr(ctx, array_ty)?;
            return Ok(Some(array.map(|a| method(a, "to_vec"))));
        }
        Ok(Self::guided_empty(&guided).map(WithStmts::new_val))
    }

    /// A static whose Rust value cannot be a constant, so it is assigned in
    /// `c2rust_run_static_initializers`: it holds an owned guided value
    /// (`String`, `Vec`, `Box`) that its initializer fills, or a `Vec` in
    /// place of a C array, whose default has a length.
    pub fn guided_static_is_uncompilable(&self, init: Option<CExprId>, typ: CQualTypeId) -> bool {
        match init {
            Some(init) if !self.ast_context.is_null_expr(init) => {
                self.holds(typ.ctype, &|t| self.is_owned_guided(t))
            }
            _ => self.holds(typ.ctype, &|t| self.is_vec_in_place_of_array(t)),
        }
    }

    /// What a static holds until `c2rust_run_static_initializers` assigns
    /// it: C's default, except that a guided value is its empty form, which
    /// is a constant.
    pub fn static_default_expr(
        &self,
        ctx: ExprContext,
        ty_id: CTypeId,
    ) -> TranslationResult<WithStmts<Box<Expr>>> {
        match self
            .xj_type_of_ctype(ty_id)
            .and_then(|g| Self::guided_empty(&g))
        {
            Some(empty) => Ok(WithStmts::new_val(empty)),
            None => self.implicit_default_expr(ctx, ty_id),
        }
    }

    /// `pred` holds for `ty` or for an element or field it holds by value.
    fn holds(&self, ty: CTypeId, pred: &dyn Fn(CTypeId) -> bool) -> bool {
        if pred(ty) {
            return true;
        }
        match self.ast_context.resolve_type(ty).kind {
            CTypeKind::ConstantArray(elt, _) | CTypeKind::IncompleteArray(elt) => {
                self.holds(elt, pred)
            }
            CTypeKind::Struct(record) | CTypeKind::Union(record) => self
                .field_types(record)
                .into_iter()
                .any(|field| self.holds(field, pred)),
            _ => false,
        }
    }

    fn field_types(&self, record: CRecordId) -> Vec<CTypeId> {
        let fields = match &self.ast_context[record].kind {
            CDeclKind::Struct {
                fields: Some(fields),
                ..
            }
            | CDeclKind::Union {
                fields: Some(fields),
                ..
            } => fields,
            _ => return vec![],
        };
        fields
            .iter()
            .filter_map(|&f| match self.ast_context[f].kind {
                CDeclKind::Field { typ, .. } => Some(typ.ctype),
                _ => None,
            })
            .collect()
    }

    fn is_owned_guided(&self, ty: CTypeId) -> bool {
        self.xj_type_of_ctype(ty).is_some_and(|g| {
            tenjin::type_is_string(&g.parsed)
                || tenjin::type_is_vec(&g.parsed)
                || generic_arg_of(&g.parsed, "Box").is_some()
        })
    }

    fn is_vec_in_place_of_array(&self, ty: CTypeId) -> bool {
        matches!(
            self.ast_context.resolve_type(ty).kind,
            CTypeKind::ConstantArray(..)
        ) && self
            .xj_type_of_ctype(ty)
            .is_some_and(|g| tenjin::type_is_vec(&g.parsed))
    }

    /// `convert_expr` for an initializer, fitted to a guided variable's Rust
    /// type where the C pass could not wrap it: constant initializers, and
    /// arrays initialized from literals or brace lists.
    pub fn convert_init(
        &self,
        ctx: ExprContext,
        init: CExprId,
        override_ty: Option<CQualTypeId>,
    ) -> TranslationResult<WithStmts<Box<Expr>>> {
        if let Some(typ) = override_ty {
            if let Some(to) = self.xj_type_of_ctype(typ.ctype) {
                let from = self.initializer_guidance(init);
                if self.initializer_needs_coercion(init, from.as_ref(), &to) {
                    return self.coerce_value(ctx, init, from.as_ref(), Some(&to), typ);
                }
            }
        }
        self.convert_expr(ctx, init, override_ty)
    }

    /// Brace lists and string literals take the declared type's sugar, but
    /// their values are plain C arrays.
    fn initializer_guidance(&self, init: CExprId) -> Option<GuidedType> {
        let core = self.c_strip_implicit_casts(init);
        match self.ast_context.index_unwrap_parens(core).kind {
            CExprKind::InitList(..) | CExprKind::Literal(..) => None,
            _ => self.xj_type_of_expr(init),
        }
    }

    fn initializer_needs_coercion(
        &self,
        init: CExprId,
        from: Option<&GuidedType>,
        to: &GuidedType,
    ) -> bool {
        if from.is_some_and(|f| f.parsed == to.parsed) {
            return false;
        }
        // c2rust already converts between numeric types.
        let init_is_numeric = from.map_or_else(
            || {
                self.ast_context[init].kind.get_type().is_some_and(|t| {
                    let kind = &self.ast_context.resolve_type(t).kind;
                    kind.is_integral_type() || kind.is_floating_type()
                })
            },
            |f| type_is_numeric(&f.parsed),
        );
        !(init_is_numeric && type_is_numeric(&to.parsed))
    }

    /// Runs `f` with marker typedefs converted as the C types behind them,
    /// for code that must keep the C ABI.
    pub fn without_markers<T>(&self, f: impl FnOnce() -> T) -> T {
        let saved = self.parsed_guidance.borrow().c_abi_markers.replace(true);
        let result = f();
        self.parsed_guidance.borrow().c_abi_markers.set(saved);
        result
    }

    /// `vars_mut` guidance for a variable in function `parent`, or at file scope.
    pub fn guided_mutability(&self, decl_id: CDeclId, parent: Option<&str>) -> Option<Mutability> {
        let name = self.ast_context[decl_id].kind.get_name()?;
        self.parsed_guidance.borrow().query_var_mut(parent, name)
    }

    /// The function a variable's guidance is keyed under: none at file
    /// scope, otherwise the function being translated, statics included.
    fn guidance_parent(&self, decl_id: CDeclId) -> Option<String> {
        if self.ast_context.c_decls_top.contains(&decl_id) {
            return None;
        }
        self.function_context.borrow().name.clone()
    }

    /// `vars_mut` guidance for a variable declared at file scope or, for a
    /// function's statics converted as items, in the current function.
    pub fn variable_mutability(&self, decl_id: CDeclId) -> Option<Mutability> {
        self.guided_mutability(decl_id, self.guidance_parent(decl_id).as_deref())
    }

    /// Whether PANGS proved the variable immutable
    /// (`semantically_immutable_globals`), keyed as `vars_mut` is.
    pub fn semantically_immutable(&self, decl_id: CDeclId) -> bool {
        let Some(name) = self.ast_context[decl_id].kind.get_name() else {
            return false;
        };
        self.parsed_guidance
            .borrow()
            .is_semantically_immutable(self.guidance_parent(decl_id).as_deref(), name)
    }

    /// `vars_mut` guidance for a variable declared in the current function.
    pub fn local_mutability(&self, decl_id: CDeclId) -> Option<Mutability> {
        let parent = self.function_context.borrow().name.clone();
        self.guided_mutability(decl_id, parent.as_deref())
    }
}
