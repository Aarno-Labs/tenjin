//! Guidance that xj-prepare-guidance applied in the C.
//!
//! Guided declarations have marker typedefs (`xj_ty_<rust type>`) for C types, so
//! the Rust type of anything that reads them is in the typedef sugar of its
//! C type. Value flows that change representation or C type are calls to
//! marker functions: the name says what the flow does (`xj_slice_all`,
//! `xj_slice_from`, `xj_elem_ref`, `xj_coerce`, `xj_is_null`), and the
//! parameter and return types say between which types. Elements of guided
//! buffers are read through `(*xj_index_<T>(b, i))`, characters of guided
//! strings through `xj_char_at_<T>(s, i)`. This module reads both.

use super::*;
use crate::convert_type::TypeConverter;
use crate::translator::tenjin::{
    self, same_numeric_type, type_buffer_element, type_is_not_clone, type_is_not_copy, GuidedType,
    Shape,
};
use quote::ToTokens;
use std::cell::Ref;

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

    /// The number of parameters every marker of the family declares.
    fn arity(self) -> usize {
        match self {
            Self::SliceAll | Self::Coerce | Self::IsNull => 1,
            Self::SliceFrom | Self::ElemRef | Self::Index | Self::CharAt => 2,
        }
    }

    /// Markers whose C body returns the argument unchanged. In Rust they may
    /// still convert it, but a recognizer that looks at the value may see
    /// through them to the value the program wrote.
    fn is_identity(self) -> bool {
        matches!(self, Self::SliceAll | Self::Coerce)
    }
}

/// Where a converted value goes: a place whose type carries guidance, or an
/// unguided C one.
pub enum Sink {
    Guided(Box<GuidedType>),
    C(CQualTypeId),
}

/// What a place marker's result is fitted to: a borrow of the place for a
/// guided sink (`&[T]`, `&mut T`, ...), a raw pointer for an unguided one.
#[derive(Clone, Copy)]
enum PlaceSink {
    Borrow { mutable: bool },
    Raw { mutable: bool },
}

impl PlaceSink {
    fn is_mutable(self) -> bool {
        match self {
            Self::Borrow { mutable } | Self::Raw { mutable } => mutable,
        }
    }
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

/// `val.name()` for a `&self` method `name` when `val` is a plain variable,
/// and otherwise `f(&val)`, with `f` naming the method by its type
/// (`str::is_empty`): a method call through a raw-pointer place uses UFCS
/// with an explicit borrow. Deref and unsize coercion fit `&val` to the
/// parameter whether `val` is owned or a borrow.
fn ref_method(val: Box<Expr>, f: Box<Expr>, name: &str) -> Box<Expr> {
    if matches!(*val, Expr::Path(_)) {
        method(val, name)
    } else {
        mk().call_expr(f, vec![borrow(val, false)])
    }
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
    pub fn marker_typedef(&self, decl: CDeclId) -> Option<Ref<'_, GuidedType>> {
        let CDeclKind::Typedef { name, .. } = &self.ast_context.get_decl(&decl)?.kind else {
            return None;
        };
        Ref::filter_map(self.parsed_guidance.borrow(), |pg| {
            pg.marker_typedefs.get(name)
        })
        .ok()
    }

    /// The Rust type named by the marker typedef in `ctype`'s sugar, or for a
    /// pointer to such a typedef, a raw pointer to it.
    pub fn guided_type_of_ctype(&self, ctype: CTypeId) -> Option<GuidedType> {
        if let Some(g) = self.marker_sugar(ctype) {
            return Some(g);
        }
        let pointee = match self.ast_context.resolve_type(ctype).kind {
            CTypeKind::Pointer(pointee) => pointee,
            _ => return None,
        };
        let inner = self.marker_sugar(pointee.ctype)?;
        let (ptr, builder) = if pointee.qualifiers.is_const {
            ("*const", mk())
        } else {
            ("*mut", mk().mutbl())
        };
        Some(GuidedType::new(
            format!("{ptr} {}", inner.pretty),
            *builder.ptr_ty(Box::new(inner.parsed)),
        ))
    }

    /// Where a value of C type `ty` goes, with the guidance its sugar names.
    fn sink_of(&self, ty: CQualTypeId) -> Sink {
        self.guided_type_of_ctype(ty.ctype)
            .map_or(Sink::C(ty), |g| Sink::Guided(Box::new(g)))
    }

    fn marker_sugar(&self, mut ctype: CTypeId) -> Option<GuidedType> {
        loop {
            ctype = match self.ast_context[ctype].kind {
                CTypeKind::Typedef(decl) => {
                    if let Some(g) = self.marker_typedef(decl) {
                        return Some(g.clone());
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
    /// out, directly or through the records and arrays they hold: owned
    /// guidance is not `Copy`, and an exclusive reference is neither.
    pub fn drop_guided_derives(&self, fields: &[CDeclId], derives: &mut Vec<syn::Meta>) {
        let field_types: Vec<CTypeId> = fields
            .iter()
            .filter_map(|&f| match self.ast_context[f].kind {
                CDeclKind::Field { typ, .. } => Some(typ.ctype),
                _ => None,
            })
            .collect();
        let any_field =
            |pred: fn(&Type) -> bool| field_types.iter().any(|&t| self.holds_guided(t, pred));
        let (copy, clone) = (!any_field(type_is_not_copy), !any_field(type_is_not_clone));
        derives.retain(|d| {
            let is = |name| d.path().is_ident(name);
            (copy || !is("Copy")) && (clone || !is("Clone"))
        });
    }

    /// A value guided as something other than a raw pointer: the base of
    /// `p->f` is then the object itself, as in `s.f`.
    pub fn is_guided_object(&self, expr: CExprId) -> bool {
        self.guided_type_of_expr(expr)
            .is_some_and(|g| !matches!(g.shape(), Shape::Ptr(_)))
    }

    /// The guidance an expression carries: its type's marker sugar, looking
    /// through parentheses and the casts that keep a value (clang drops the
    /// sugar at array decay).
    pub fn guided_type_of_expr(&self, expr: CExprId) -> Option<GuidedType> {
        let mut current = expr;
        loop {
            let kind = &self.ast_context.index_unwrap_parens(current).kind;
            if let Some(g) = kind.get_type().and_then(|t| self.guided_type_of_ctype(t)) {
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

    /// The guidance on a variable or field, read off the marker typedef in
    /// its declared type.
    pub fn guided_type_of_decl(&self, decl: CDeclId) -> Option<GuidedType> {
        match self.ast_context.get_decl(&decl)?.kind {
            CDeclKind::Variable { typ, .. } | CDeclKind::Field { typ, .. } => {
                self.guided_type_of_ctype(typ.ctype)
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
        Some((self.marker_function(decl)?, decl))
    }

    /// The family of a marker function declaration. A program function that
    /// happens to share a marker's name, but not its arity, is not one.
    fn marker_function(&self, decl: CDeclId) -> Option<MarkerFamily> {
        let CDeclKind::Function { name, .. } = &self.ast_context[decl].kind else {
            return None;
        };
        let family = self.marker_family(name)?;
        let (params, _) = self.marker_signature(decl)?;
        (params.len() == family.arity()).then_some(family)
    }

    /// A marker typedef or marker function declaration. It has no Rust item:
    /// uses are translated in their place.
    pub fn is_marker_decl(&self, decl: CDeclId) -> bool {
        match &self.ast_context[decl].kind {
            CDeclKind::Typedef { .. } => self.marker_typedef(decl).is_some(),
            CDeclKind::Function { .. } => self.marker_function(decl).is_some(),
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

    /// The family and arguments of a marker call `expr`, through parentheses
    /// and implicit casts.
    fn marker_call(&self, expr: CExprId) -> Option<(MarkerFamily, &[CExprId])> {
        let mut current = expr;
        loop {
            match self.ast_context.index_unwrap_parens(current).kind {
                CExprKind::ImplicitCast(_, inner, _, _, _) => current = inner,
                CExprKind::Call(_, func, ref args) => {
                    let (family, _) = self.marker_callee(func)?;
                    return (args.len() == family.arity()).then_some((family, args.as_slice()));
                }
                _ => return None,
            }
        }
    }

    /// `expr` without an enclosing identity marker, so a recognizer that
    /// inspects an argument sees the value the program wrote.
    pub fn strip_identity_markers(&self, expr: CExprId) -> CExprId {
        match self.marker_call(expr) {
            Some((family, args)) if family.is_identity() => args[0],
            _ => expr,
        }
    }

    /// `arg` without its identity markers, if the value it holds is guided
    /// with a type satisfying `pred`. A recognizer rewrites a call for that
    /// value; one that keeps the call converts `arg` itself, so the
    /// conversions the markers stand for still apply.
    pub fn guided_arg(&self, arg: CExprId, pred: impl Fn(&GuidedType) -> bool) -> Option<CExprId> {
        let value = self.strip_identity_markers(arg);
        self.guided_type_of_expr(value)
            .is_some_and(|g| pred(&g))
            .then_some(value)
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

    /// Translates a call to a marker function of `family`, as named by
    /// `marker_callee`. The marker's own prototype gives the source and sink
    /// types; a call whose arity is not the family's is an error.
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
        if cargs.len() != family.arity() {
            return Err(format_err!(
                "marker call with {} arguments, expected {}",
                cargs.len(),
                family.arity()
            )
            .into());
        }
        match family {
            MarkerFamily::SliceAll | MarkerFamily::SliceFrom | MarkerFamily::ElemRef => {
                self.convert_place_marker(ctx, family, ret, cargs)
            }
            MarkerFamily::Index => self.convert_index_address(ctx, ret, cargs),
            MarkerFamily::CharAt => self.convert_char_at(ctx, ret, cargs),
            MarkerFamily::Coerce => self.convert_coerce_marker(ctx, params[0], ret, cargs[0]),
            MarkerFamily::IsNull => self.convert_is_null_marker(ctx, cargs[0]),
        }
    }

    fn place_sink(&self, ret: CQualTypeId) -> PlaceSink {
        match self.sink_of(ret) {
            Sink::Guided(g) => PlaceSink::Borrow {
                mutable: g.is_exclusive_borrow(),
            },
            Sink::C(ret) => PlaceSink::Raw {
                mutable: !self.points_to_const(ret),
            },
        }
    }

    /// `ty` is a pointer to a const-qualified type.
    fn points_to_const(&self, ty: CQualTypeId) -> bool {
        matches!(
            self.ast_context.resolve_type(ty.ctype).kind,
            CTypeKind::Pointer(pointee) if pointee.qualifiers.is_const
        )
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
        let base_is_ref = self
            .guided_type_of_expr(base_id)
            .is_some_and(|g| g.is_borrow());
        let place = match index {
            None => base.map(|b| Self::whole_place(sink, b, base_is_ref)),
            Some(index) => base
                .zip(index)
                .map(|(b, i)| Self::offset_place(family, sink, b, i)),
        };
        match (sink, self.raw_place_cast(base_id, ret)?) {
            (PlaceSink::Raw { .. }, Some(ty)) => Ok(place.map(|p| mk().cast_expr(p, ty))),
            _ => Ok(place),
        }
    }

    /// A marker's `long i` argument as a Rust index, without C's conversion
    /// to `long`.
    fn convert_marker_index(
        &self,
        ctx: ExprContext,
        index: CExprId,
    ) -> TranslationResult<WithStmts<Box<Expr>>> {
        self.convert_index_operand(ctx.used(), self.c_strip_implicit_casts(index), false)
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

    /// The base and index of a call `xj_index_<T>(b, i)`, through parentheses
    /// and implicit casts.
    pub fn index_marker_operands(&self, expr: CExprId) -> Option<(CExprId, CExprId)> {
        match self.marker_call(expr)? {
            (MarkerFamily::Index, args) => Some((args[0], args[1])),
            _ => None,
        }
    }

    /// `TypedAstContext::is_expr_pure`, except that a marker call is pure when
    /// its arguments are, so `(*xj_index_<T>(p, i)) += 1` names its place
    /// directly.
    pub fn is_pure_with_markers(&self, expr: CExprId) -> bool {
        self.ast_context
            .is_expr_pure_with(expr, &|func| self.marker_callee(func).is_some())
    }

    /// `(*xj_index_<T>(b, i))`: element `i` of the buffer `b`.
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

    /// `xj_index_<T>(b, i)` itself, as in `&b[i]`: a raw pointer to the
    /// element, mutable unless the C pointee is const or `b` a shared borrow.
    fn convert_index_address(
        &self,
        ctx: ExprContext,
        ret: CQualTypeId,
        cargs: &[CExprId],
    ) -> TranslationResult<WithStmts<Box<Expr>>> {
        let base_id = self.c_strip_noop_casts(cargs[0]);
        let shared = self
            .guided_type_of_expr(base_id)
            .is_some_and(|g| g.is_shared_borrow());
        let sink = PlaceSink::Raw {
            mutable: !self.points_to_const(ret) && !shared,
        };
        let base = self.convert_guided_value(ctx, base_id)?;
        let index = self.convert_marker_index(ctx, cargs[1])?;
        let place = base
            .zip(index)
            .map(|(b, i)| Self::offset_place(MarkerFamily::ElemRef, sink, b, i));
        match self.raw_place_cast(base_id, ret)? {
            Some(ty) => Ok(place.map(|p| mk().cast_expr(p, ty))),
            None => Ok(place),
        }
    }

    /// `xj_char_at_<T>(s, i)`: byte `i` of a guided string as a C character,
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
        let element = match self.guided_type_of_expr(base_id) {
            Some(g) => type_buffer_element(&g.parsed).cloned().map(Box::new),
            None => self.c_element_type(base_id)?,
        };
        Ok(element
            .filter(|e| !same_numeric_type(e, &ptr.elem))
            .map(|_| ret_ty.clone()))
    }

    /// The Rust type of the elements of an unguided array or pointer.
    fn c_element_type(&self, expr: CExprId) -> TranslationResult<Option<Box<Type>>> {
        self.ast_context[expr]
            .kind
            .get_type()
            .and_then(|t| self.ast_context.resolve_type(t).kind.element_ty())
            .map(|elt| self.convert_type(elt))
            .transpose()
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
    fn whole_place(sink: PlaceSink, base: Box<Expr>, base_is_ref: bool) -> Box<Expr> {
        let mutable = sink.is_mutable();
        match sink {
            PlaceSink::Raw { .. } => as_ptr(Self::borrowed_base(base, mutable), mutable),
            PlaceSink::Borrow { .. } if base_is_ref || tenjin::expr_is_lit_str_or_bytes(&base) => {
                base
            }
            PlaceSink::Borrow { .. } => borrow(base, mutable),
        }
    }

    /// A base reached through a raw pointer (`(*s).buf`) is borrowed
    /// explicitly before it is indexed or has a method called on it: Rust
    /// rejects an implicit borrow through a raw-pointer place.
    fn borrowed_base(base: Box<Expr>, mutable: bool) -> Box<Expr> {
        let direct = matches!(*base, Expr::Path(_) | Expr::Lit(_) | Expr::Reference(_));
        if direct {
            base
        } else {
            mk().paren_expr(borrow(base, mutable))
        }
    }

    /// `xj_slice_from(b, i)` or `xj_elem_ref(b, i)` on the converted base:
    /// the suffix `&b[i..]` or the element `&b[i]`, as a borrow for a guided
    /// sink and as a raw pointer (`b[i..].as_ptr()`, `&raw const b[i]`)
    /// otherwise.
    fn offset_place(
        family: MarkerFamily,
        sink: PlaceSink,
        base: Box<Expr>,
        index: Box<Expr>,
    ) -> Box<Expr> {
        let mutable = sink.is_mutable();
        let base = Self::borrowed_base(base, mutable);
        if family == MarkerFamily::ElemRef {
            let elem = mk().index_expr(base, index);
            return match sink {
                PlaceSink::Borrow { .. } => borrow(elem, mutable),
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
            PlaceSink::Borrow { .. } => borrow(suffix, mutable),
            PlaceSink::Raw { .. } => as_ptr(suffix, mutable),
        }
    }

    /// `xj_coerce_<from>_to_<to>(v)`: `v` converted from the guidance it
    /// carries, or else its parameter's, to the guidance the marker returns.
    /// Between equal guidance it is `v` itself.
    fn convert_coerce_marker(
        &self,
        ctx: ExprContext,
        param: CQualTypeId,
        ret: CQualTypeId,
        arg: CExprId,
    ) -> TranslationResult<WithStmts<Box<Expr>>> {
        let from = self
            .guided_type_of_expr(arg)
            .or_else(|| self.guided_type_of_ctype(param.ctype));
        let to = self.sink_of(ret);
        if matches!((&from, &to), (Some(f), Sink::Guided(t)) if f.parsed == t.parsed) {
            return self.convert_guided_value(ctx, arg);
        }
        self.coerce_value(ctx, arg, from.as_ref(), &to)
    }

    /// `xj_is_null(p)`: what C's `p == 0` means for the guidance on `p`.
    /// An `Option` is `None`; a string, `Vec` or slice is empty; any other
    /// guided value (a reference, a `Box`) is never null, so the test is
    /// `false`; an unguided pointer keeps `.is_null()`.
    // XREF:guided_condition_string_null_check_neq
    fn convert_is_null_marker(
        &self,
        ctx: ExprContext,
        arg: CExprId,
    ) -> TranslationResult<WithStmts<Box<Expr>>> {
        let guided = self.guided_type_of_expr(arg);
        let val = self.convert_expr(ctx.used(), arg, None)?;
        Ok(val.map(|v| match guided {
            Some(g) if matches!(g.shape(), Shape::Option(_)) => {
                ref_method(v, mk().path_expr(vec!["Option", "is_none"]), "is_none")
            }
            Some(g) if is_str_or_string(tenjin::type_strip_refs(&g.parsed)) => {
                ref_method(v, mk().path_expr(vec!["str", "is_empty"]), "is_empty")
            }
            Some(g) if type_buffer_element(&g.parsed).is_some() => ref_method(
                v,
                Box::new(syn::parse_quote! { <[_]>::is_empty }),
                "is_empty",
            ),
            Some(_) => mk().lit_expr(mk().bool_lit(false)),
            None => method(v, "is_null"),
        }))
    }

    /// Fits the value of `arg` to `to`. Used by `xj_coerce` markers and by initializers the C pass could not
    /// wrap (constant contexts, array initializers).
    ///
    /// | from | to | Rust |
    /// |---|---|---|
    /// | null constant | `String`, `Vec`, `Option`, `&str`, slice | `String::new()`, `Vec::new()`, `None`, `""`, `&[]` |
    /// | string literal | `String`, `&str`, `Vec<u8>`, `&[u8]` | the literal in that form |
    /// | `&x` | a borrow | `&x` or `&mut x` |
    /// | unguided | `Vec`, array | `vec![..]` from an array expression; an array as is |
    /// | unguided | number, `char` | `v as T`; `v as u8 as char` |
    /// | unguided pointer | `&T`, `&mut T` | `v.as_ref().unwrap()`, `v.as_mut().unwrap()` |
    /// | owned | a borrow | `&v` or `&mut v` |
    /// | a borrow | a borrow | `&*v` or `&mut *v` |
    /// | `&str`, `char` | `String` | `v.to_string()` |
    /// | `String`, `&String` | `String` | `v.clone()` |
    /// | borrowed buffer | `Vec` | `v.to_vec()` |
    /// | `T` | `Option<T>` | `Some(v)` |
    /// | `char` | a number | `v as T` |
    /// | a number | `char` | `v as char` |
    /// | buffer | raw pointer | `v.as_ptr() as *T` |
    /// | `&T`, `&mut T`, `Box<T>` | raw pointer | `v` where Rust coerces it, else `v as *const T as *U` |
    /// | raw pointer, scalar | C type | `v as T` |
    ///
    /// Any other pair is `xj_unhandled_coercion`.
    pub fn coerce_value(
        &self,
        ctx: ExprContext,
        arg: CExprId,
        from: Option<&GuidedType>,
        to: &Sink,
    ) -> TranslationResult<WithStmts<Box<Expr>>> {
        match to {
            Sink::Guided(to) => {
                if let Some(val) = self.coerce_constant(arg, to) {
                    return Ok(WithStmts::new_val(val));
                }
                if let Some(inner) = self.address_of_operand(arg) {
                    return self.coerce_address_of(ctx, inner, from, to);
                }
                let val = self.convert_expr(ctx.used(), arg, None)?;
                Ok(val.map(|v| self.coerce_to_guided(v, from, to)))
            }
            Sink::C(to_cty) => {
                let val = self.convert_expr(ctx.used(), arg, None)?;
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
        let call = |path: Vec<&str>| Some(mk().call_expr(mk().path_expr(path), vec![]));
        match to.shape() {
            Shape::String => call(vec!["String", "new"]),
            Shape::Vec(_) => call(vec!["Vec", "new"]),
            Shape::Option(_) => Some(mk().path_expr(vec!["None"])),
            Shape::StrRef => Some(mk().lit_expr("")),
            Shape::SliceRef(_) => Some(borrow(mk().array_expr(vec![]), to.is_exclusive_borrow())),
            _ => None,
        }
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

    /// A converted value into a guided type, from guidance `from` or from an
    /// unguided C value. A pair neither table knows is unhandled.
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

    /// An unguided C value into guided `to`, or `None` when there is no
    /// conversion.
    fn raw_to_guided(val: Box<Expr>, to: &GuidedType) -> Option<Box<Expr>> {
        match to.shape() {
            Shape::Vec(_) => vec_of_array(&val),
            Shape::Array(_) => Some(val),
            Shape::Number => Some(mk().cast_expr(val, Box::new(to.parsed.clone()))),
            // A pointer to one object; a buffer has no length to borrow with.
            Shape::Ref(referent) if type_buffer_element(referent).is_none() => {
                let as_ref = if to.is_exclusive_borrow() {
                    "as_mut"
                } else {
                    "as_ref"
                };
                Some(method(method(val, as_ref), "unwrap"))
            }
            Shape::Char => {
                let byte = mk().cast_expr(val, mk().path_ty(vec!["u8"]));
                Some(mk().cast_expr(byte, mk().path_ty(vec!["char"])))
            }
            _ => None,
        }
    }

    /// A value guided `from` into guided `to`, or `None` when there is no
    /// conversion. The first arm that applies wins.
    fn guided_to_guided(val: Box<Expr>, from: &GuidedType, to: &GuidedType) -> Option<Box<Expr>> {
        let owned_method = |val, name| Some(method(Self::borrowed_base(val, false), name));
        match (from.shape(), to.shape()) {
            // An owned value into a borrow of itself, or of what it derefs to.
            (f, t) if t.is_borrow() && !f.is_borrow() => {
                Some(borrow(val, to.is_exclusive_borrow()))
            }
            (_, t) if t.is_borrow() => Some(reborrow(val, to.is_exclusive_borrow())),
            (Shape::StrRef | Shape::Char, Shape::String) => owned_method(val, "to_string"),
            (_, Shape::String) if tenjin::type_is_string(from.strip_refs()) => {
                owned_method(val, "clone")
            }
            (f, Shape::Vec(_)) if f.is_borrow() && type_buffer_element(&from.parsed).is_some() => {
                owned_method(val, "to_vec")
            }
            (_, Shape::Option(inner)) if *inner == from.parsed => {
                Some(mk().call_expr(mk().path_expr(vec!["Some"]), vec![val]))
            }
            (Shape::Char, _) | (_, Shape::Char) => {
                Some(mk().cast_expr(val, Box::new(to.parsed.clone())))
            }
            _ => None,
        }
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
        let shape = from.shape();
        let is_buffer = type_buffer_element(&from.parsed).is_some();
        let Type::Ptr(ref to_ptr) = *to else {
            if shape.is_borrow() || matches!(shape, Shape::String) || is_buffer {
                return unhandled_coercion(self, val, &from.pretty, &type_text(&to));
            }
            // Scalars, including Rust `char`, convert with `as`.
            return mk().cast_expr(val, to);
        };
        let to_mut = to_ptr.mutability.is_some();
        match shape {
            _ if is_buffer => mk().cast_expr(as_ptr(Self::borrowed_base(val, to_mut), to_mut), to),
            Shape::Ptr(_) => mk().cast_expr(val, to),
            Shape::Ref(target) | Shape::Box(target)
                if !tenjin::type_is_exactly_1_path(target, "str") =>
            {
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
        let mutable = from.is_exclusive_borrow() || matches!(from.shape(), Shape::Box(_));
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
    /// length, since code indexes into it later. A constant cannot allocate,
    /// so there the `Vec` is empty: constants of such types are only the
    /// placeholders of statics that `c2rust_run_static_initializers` assigns.
    pub fn guided_implicit_default(
        &self,
        ctx: ExprContext,
        ty_id: CTypeId,
    ) -> TranslationResult<Option<WithStmts<Box<Expr>>>> {
        let Some(guided) = self.guided_type_of_ctype(ty_id) else {
            return Ok(None);
        };
        let is_array = matches!(
            self.ast_context.resolve_type(ty_id).kind,
            CTypeKind::ConstantArray(..)
        );
        if tenjin::type_is_vec(&guided.parsed) && is_array && !ctx.is_const {
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
    /// it: C's default as a constant, in which guided values are empty.
    pub fn static_default_expr(
        &self,
        ctx: ExprContext,
        ty_id: CTypeId,
    ) -> TranslationResult<WithStmts<Box<Expr>>> {
        self.implicit_default_expr(ctx.const_(), ty_id)
    }

    /// `[elt_default; len]` for an element that is not `Copy`, which a
    /// repeat expression only accepts as a named constant: one in a
    /// constant, and `core::array::from_fn` elsewhere, where the default may
    /// allocate.
    pub(super) fn non_copy_array_default(
        &self,
        ctx: ExprContext,
        elt: CTypeId,
        len: usize,
        elt_default: WithStmts<Box<Expr>>,
    ) -> TranslationResult<WithStmts<Box<Expr>>> {
        if !ctx.is_const {
            return Ok(
                elt_default.map(|e| Box::new(syn::parse_quote! { ::core::array::from_fn(|_| #e) }))
            );
        }
        let elt_ty = self.convert_type(elt)?;
        let len = mk().lit_expr(mk().int_unsuffixed_lit(len as u128));
        Ok(elt_default.map(|e| {
            Box::new(syn::parse_quote! {{
                const XJ_ELEMENT: #elt_ty = #e;
                [XJ_ELEMENT; #len]
            }})
        }))
    }

    /// `ty`, or a record or array it holds by value, is guided with a type
    /// that does not implement `Copy`.
    pub fn holds_non_copy_guidance(&self, ty: CTypeId) -> bool {
        self.holds_guided(ty, type_is_not_copy)
    }

    fn holds_guided(&self, ty: CTypeId, pred: fn(&Type) -> bool) -> bool {
        self.holds(ty, &|t| {
            self.guided_type_of_ctype(t)
                .is_some_and(|g| pred(&g.parsed))
        })
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
        self.guided_type_of_ctype(ty)
            .is_some_and(|g| g.shape().is_owned())
    }

    fn is_vec_in_place_of_array(&self, ty: CTypeId) -> bool {
        matches!(
            self.ast_context.resolve_type(ty).kind,
            CTypeKind::ConstantArray(..)
        ) && self
            .guided_type_of_ctype(ty)
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
            let to = self.sink_of(typ);
            if let Sink::Guided(ref guided) = to {
                let from = self.initializer_guidance(init);
                if self.initializer_needs_coercion(init, from.as_ref(), guided) {
                    return self.coerce_value(ctx, init, from.as_ref(), &to);
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
            _ => self.guided_type_of_expr(init),
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
            |f| matches!(f.shape(), Shape::Number),
        );
        !(init_is_numeric && matches!(to.shape(), Shape::Number))
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
