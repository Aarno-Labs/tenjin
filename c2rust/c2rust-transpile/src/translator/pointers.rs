use c2rust_ast_builder::{mk, properties::Mutability};
use c2rust_ast_exporter::clang_ast::LRValue;
use c2rust_rust_tools::RustEdition;
use failure::{err_msg, format_err};
use syn::{BinOp, Expr, Type, UnOp};

use crate::c_ast::CUnOp;
use crate::translator::tenjin::{self, GuidedType};
use crate::{
    diagnostics::{TranslationError, TranslationErrorKind, TranslationResult},
    format_translation_err,
    translator::{
        cast_int, neg_expr, transmute_expr, unwrap_function_pointer, ExprContext, Translation,
    },
    with_stmts::WithStmts,
    CExprId, CExprKind, CLiteral, CQualTypeId, CTypeId, CTypeKind, CastKind, ExternCrate,
};

impl<'c> Translation<'c> {
    pub fn convert_address_of(
        &self,
        mut ctx: ExprContext,
        cqual_type: CQualTypeId,
        arg: CExprId,
    ) -> TranslationResult<WithStmts<Box<Expr>>> {
        let arg_kind = &self.ast_context.index_unwrap_parens(arg).kind;

        match arg_kind {
            // C99 6.5.3.2 para 4
            CExprKind::Unary(_, CUnOp::Deref, target, _) => {
                return self.convert_expr(ctx, *target, None)
            }
            // Array subscript functions as a deref too.
            &CExprKind::ArraySubscript(_, lhs, rhs, _) => {
                return self.convert_array_subscript(
                    ctx.used().set_needs_address(true),
                    Some(cqual_type),
                    lhs,
                    rhs,
                    LRValue::RValue, // if we bypass the deref, we stay an RValue
                    false,           // don't deref, keep as pointer
                );
            }
            // An AddrOf DeclRef/Member is safe to not decay
            // if the translator isn't already giving a hard yes to decaying (ie, BitCasts).
            // So we only convert default to no decay.
            CExprKind::DeclRef(..) | CExprKind::Member(..) => ctx.decay_ref.set_default_to_no(),
            _ => (),
        }

        let val = self.convert_expr(ctx.used().set_needs_address(true), arg, None)?;

        // & becomes a no-op when applied to a function.
        if self.ast_context.is_function_pointer(cqual_type.ctype) {
            return Ok(val.map(|x| mk().call_expr(mk().ident_expr("Some"), vec![x])));
        }

        let arg_cty = arg_kind
            .get_qual_type()
            .ok_or_else(|| format_err!("bad source type"))?;

        self.convert_address_of_common(ctx, Some(arg), arg_cty, cqual_type, val, false, &None)
    }

    pub fn convert_array_to_pointer_decay(
        &self,
        ctx: ExprContext,
        source_cty: CQualTypeId,
        target_cty: CQualTypeId,
        val: WithStmts<Box<Expr>>,
        expr: Option<CExprId>,
        guided_type: &Option<GuidedType>,
    ) -> TranslationResult<WithStmts<Box<Expr>>> {
        // Because va_list is sometimes defined as a single-element
        // array in order for it to allocate memory as a local variable
        // and to be a pointer as a function argument we would get
        // spurious casts when trying to treat it like a VaList which
        // has reference semantics.
        if self.ast_context.is_va_list(target_cty.ctype) {
            return Ok(val);
        }

        let source_ty_kind = &self.ast_context.resolve_type(source_cty.ctype).kind;

        // Variable length arrays are already represented as pointers.
        if let CTypeKind::VariableArray(..) = source_ty_kind {
            return Ok(val);
        }

        self.convert_address_of_common(ctx, expr, source_cty, target_cty, val, true, guided_type)
    }

    fn convert_address_of_common(
        &self,
        ctx: ExprContext,
        arg: Option<CExprId>,
        arg_cty: CQualTypeId,
        pointer_cty: CQualTypeId,
        mut val: WithStmts<Box<Expr>>,
        is_array_decay: bool,
        guided_type: &Option<GuidedType>,
    ) -> TranslationResult<WithStmts<Box<Expr>>> {
        let arg_expr_kind = arg.map(|arg| {
            let arg = self.ast_context.unwrap_predefined_ident(arg);
            &self.ast_context.index_unwrap_parens(arg).kind
        });
        let pointee_cty = self
            .ast_context
            .get_pointee_qual_type(pointer_cty.ctype)
            .ok_or_else(|| TranslationError::generic("Address-of should return a pointer"))?;
        let arg_is_macro = arg.is_some_and(|arg| self.expr_is_expanded_macro(ctx, arg, None));

        let mut needs_cast = false;
        let mut ref_cast_pointee_ty = None;
        let mutbl = if ctx.is_const && !pointee_cty.qualifiers.is_const {
            // const contexts aren't able to use &mut, so we work around that
            // by using & and an extra cast through & to *const to *mut
            // TODO: Rust 1.83: Allowed, so this can be removed.
            needs_cast = true;
            Mutability::Immutable
        } else {
            pointee_cty.mutability()
        };

        if let Some(CExprKind::DeclRef(_cqti, decl_id, _lrval)) = arg_expr_kind {
            let expr_guidance = self
                .parsed_guidance
                .borrow_mut()
                .query_decl_type(self, *decl_id);

            if let Some(eg) = expr_guidance {
                if tenjin::type_is_string(&eg.parsed)
                    || tenjin::type_is_vec(eg.strip_refs())
                    || tenjin::type_is_str_ref(&eg.parsed)
                {
                    // XREF:guided_array_decay
                    return Ok(val);
                }

                if let Some(cg) = guided_type {
                    if cg.is_slice_ref() && tenjin::type_try_arraylike_element(&eg.parsed).is_some()
                    {
                        // If the destination is guided to a slice type and the source is
                        // array-like with the same element type, then we can skip the decay.
                        //
                        // If the element types are different but compatible via bytemucking,
                        // we can insert an appropriate cast. (TODO)
                        //
                        // If the element types are different and not compatible, we should let
                        // the Rust compiler catch the mismatch.
                        return Ok(val);
                    }
                }
                // If the value is a slice but the context still wants a pointer,
                // we'll fall through to the normal decay logic.
            } else if is_array_decay {
                if let Some(cg) = guided_type {
                    if cg.is_slice_ref() {
                        // No explicit guidance on the source, but the source is an array and
                        // the destination is guided to a slice, which requires either:
                        //  * a (non-raw) borrow, or
                        //  * .as_mut() / .as_ref()
                        return Ok(val.map(|val| mk().set_mutbl(mutbl).borrow_expr(val)));
                    }
                }
            }
        }

        // Narrow string literals are translated directly as `[u8; N]` literals when their address
        // is taken, without the transmute. String/byte literals are already references in Rust.
        if let (
            Some(&CExprKind::Literal(literal_cty, CLiteral::String(ref bytes, element_size @ 1))),
            false,
        ) = (arg_expr_kind, arg_is_macro)
        {
            // XREF:guided_string_implicit_cast
            if let Some(e) = self.convert_string_literal_guided(bytes, element_size, guided_type) {
                return Ok(WithStmts::new_val(e));
            }

            if is_array_decay {
                val = val.map(|val| mk().method_call_expr(val, "as_ptr", vec![]));
            } else {
                let size = self.ast_context.array_len(literal_cty.ctype) * element_size as usize;
                ref_cast_pointee_ty =
                    Some(mk().array_ty(mk().ident_ty("u8"), mk().lit_expr(size as u128)));
            }
            needs_cast = true;
        }
        // Values that translate into const temporaries can't be raw-borrowed in Rust.
        // They must be regular-borrowed first, which will extend the lifetime to static.
        else if arg_is_macro || matches!(arg_expr_kind, Some(CExprKind::Literal(..))) {
            let arg_cty_kind = &self.ast_context.resolve_type(arg_cty.ctype).kind;

            if is_array_decay {
                let method = match mutbl {
                    Mutability::Mutable => "as_mut_ptr",
                    Mutability::Immutable => "as_ptr",
                };
                // XREF:array_decay
                val = val.map(|val| mk().method_call_expr(val, method, vec![]));

                // If the target pointee type is different from the source element type,
                // then we need to cast the ptr type as well.
                if arg_cty_kind
                    .element_ty()
                    .is_some_and(|arg_element_cty| arg_element_cty != pointee_cty.ctype)
                {
                    needs_cast = true;
                }
            } else {
                val = val.map(|val| mk().set_mutbl(mutbl).borrow_expr(val));

                // Add an intermediate reference-to-pointer cast if the context needs
                // reference-to-pointer decay, or if another cast follows.
                if ctx.decay_ref.is_yes() || needs_cast {
                    ref_cast_pointee_ty = Some(self.convert_pointee_type(arg_cty.ctype)?);
                }
            }
        } else {
            self.use_feature("raw_ref_op");
            val = val.map(|val| mk().set_mutbl(mutbl).raw_borrow_expr(val));

            if is_array_decay {
                needs_cast = true;
                // let method = match mutbl {
                //     Mutability::Mutable => "as_mut_ptr",
                //     Mutability::Immutable if needs_cast => "as_mut_ptr",
                //     Mutability::Immutable => "as_ptr",
                // };
                // needs_cast = false;
                // // XREF:array_decay
                // val = val.map(|val| mk().method_call_expr(val, method, vec![]));
            }
        }

        // Perform an intermediate reference-to-pointer cast if needed.
        // TODO: Rust 1.76: Use `ptr::from_ref`.
        if let Some(pointee_ty) = ref_cast_pointee_ty {
            val = val.map(|val| mk().cast_expr(val, mk().set_mutbl(mutbl).ptr_ty(pointee_ty)));
        }

        // Perform a final cast to the target type if needed.
        if needs_cast {
            let pointer_ty = self.convert_type(pointer_cty.ctype)?;
            val = val.map(|val| mk().cast_expr(val, pointer_ty));
        }

        Ok(val)
    }

    pub fn convert_deref(
        &self,
        ctx: ExprContext,
        cqual_type: CQualTypeId,
        arg: CExprId,
    ) -> TranslationResult<WithStmts<Box<Expr>>> {
        let arg_expr_kind = &self.ast_context.index_unwrap_parens(arg).kind;

        if let &CExprKind::Unary(_, CUnOp::AddressOf, arg, _) = arg_expr_kind {
            return self.convert_expr(ctx.used(), arg, None);
        }

        if let Some((dst_tykind, inner_exp, inner_is_signed)) =
            tenjin::is_bitcast_to_int_or_float(self, arg_expr_kind)
        {
            return self
                .convert_expr(ctx.used(), inner_exp, None)?
                .try_map(|val: Box<Expr>| {
                    match dst_tykind {
                        // XREF:recognize_int_float_bitcast
                        CTypeKind::Float => Ok(mk().call_expr(
                            // emit e.g. f32::from_bits(val)
                            mk().path_expr(vec!["f32".to_string(), "from_bits".into()]),
                            vec![if inner_is_signed {
                                mk().cast_expr(val, mk().path_ty(vec!["u32".to_string()]))
                            } else {
                                val
                            }],
                        )),
                        CTypeKind::Double => Ok(mk().call_expr(
                            mk().path_expr(vec!["f64".to_string(), "from_bits".into()]),
                            vec![if inner_is_signed {
                                mk().cast_expr(val, mk().path_ty(vec!["u64".to_string()]))
                            } else {
                                val
                            }],
                        )),
                        // TENJIN-TODO(intsizes): be more robust about determining actual int sizes
                        CTypeKind::ULongLong => {
                            // emit e.g. val.to_bits()
                            Ok(mk().method_call_expr(val, "to_bits".to_string(), vec![]))
                        }
                        CTypeKind::LongLong => {
                            // emit e.g. val.to_bits() as i64
                            Ok(mk().cast_expr(
                                mk().method_call_expr(val, "to_bits".to_string(), vec![]),
                                mk().path_ty(vec!["i64".to_string()]),
                            ))
                        }
                        _ => Err(TranslationError::generic(
                            "Unexpected type in bitcast deref",
                        )),
                    }
                });
        }

        self.convert_expr(ctx.used(), arg, None)?
            .try_map(|val: Box<Expr>| {
                if let CTypeKind::Function(..) =
                    self.ast_context.resolve_type(cqual_type.ctype).kind
                {
                    Ok(unwrap_function_pointer(val))
                } else if let Some(_vla) = self.compute_size_of_expr(cqual_type.ctype) {
                    Ok(val)
                } else {
                    Ok(mk().unary_expr(UnOp::Deref(Default::default()), val))
                }
            })
    }

    pub fn convert_array_subscript(
        &self,
        ctx: ExprContext,
        expected_type_id: Option<CQualTypeId>,
        lhs: CExprId,
        rhs: CExprId,
        lrvalue: LRValue,
        deref: bool,
    ) -> TranslationResult<WithStmts<Box<Expr>>> {
        let (pointer_id, offset_id) = if self.ast_context.expr_is_indexable(lhs) {
            (lhs, rhs)
        } else {
            (rhs, lhs)
        };

        let pointer_kind = &self.ast_context.index_unwrap_parens(pointer_id).kind;
        let pointer_type_id = pointer_kind
            .get_type()
            .ok_or_else(|| format_err!("pointer_id bad type"))?;
        let pointer_type_kind = &self.ast_context.resolve_type(pointer_type_id).kind;

        if pointer_type_kind.is_vector() {
            return Err(TranslationError::from(
                err_msg("Attempting to index a vector type")
                    .context(TranslationErrorKind::OldLLVMSimd),
            ));
        }

        let simple_index_array = if ctx.needs_address() {
            // We can't necessarily index into an array if we're using
            // that element to compute an address.
            None
        } else {
            match pointer_kind {
                &CExprKind::ImplicitCast(_, array_id, CastKind::ArrayToPointerDecay, _, _) => {
                    match self.ast_context.index_unwrap_parens(array_id).kind {
                        CExprKind::Member(_, _, field_decl, _, _)
                            if self
                                .potential_flexible_array_members
                                .borrow()
                                .contains(&field_decl) =>
                        {
                            None
                        }
                        ref kind => {
                            let type_id =
                                kind.get_type().ok_or_else(|| format_err!("bad arr type"))?;
                            match self.ast_context.resolve_type(type_id).kind {
                                // These get translated to 0-element arrays, this avoids the bounds check
                                // that using an array subscript in Rust would cause
                                CTypeKind::IncompleteArray(_) => None,
                                _ => Some(array_id),
                            }
                        }
                    }
                }
                _ => None,
            }
        };

        if let Some(array_id) = simple_index_array {
            // If the LHS just underwent an implicit cast from array to pointer, bypass that
            // to make an actual Rust indexing operation

            let array_kind = &self.ast_context.index_unwrap_parens(array_id).kind;
            let array_type_id = array_kind
                .get_type()
                .ok_or_else(|| format_err!("bad arr type"))?;
            let array_type_kind = &self.ast_context.resolve_type(array_type_id).kind;
            let var_elt_type_id = match *array_type_kind {
                CTypeKind::ConstantArray(..) => None,
                CTypeKind::IncompleteArray(..) => None,
                CTypeKind::VariableArray(elt, _) => Some(elt),
                ref other => panic!("Unexpected array type {:?}", other),
            };

            let array_rs = self.convert_expr(ctx.used(), array_id, None)?;

            // Don't dereference the offset if we're still within the variable portion
            let val = if let Some(elt_type_id) = var_elt_type_id {
                let target_type_id = self.ast_context.type_for_kind(&CTypeKind::SSize);
                let offset_rs = self.convert_expr_with_cast(
                    ctx.used(),
                    CQualTypeId::new(target_type_id),
                    offset_id,
                    &None,
                )?;
                array_rs.zip(offset_rs).and_then(|(array_rs, offset_rs)| {
                    self.make_pointer_offset(array_rs, offset_rs, elt_type_id, false, deref)
                })
            } else {
                let target_type_id = self.ast_context.type_for_kind(&CTypeKind::Size);
                let offset_rs = self.convert_expr_with_cast(
                    ctx.used(),
                    CQualTypeId::new(target_type_id),
                    offset_id,
                    &None,
                )?;
                array_rs
                    .zip(offset_rs)
                    .map(|(array_rs, offset_rs)| mk().index_expr(array_rs, offset_rs))
            };

            Ok(val)
        } else {
            // Determine the type of element being indexed
            let pointee_type_id = match self.ast_context.resolve_type(pointer_type_id).kind {
                CTypeKind::Pointer(id) => id,
                _ => {
                    return Err(format_err!(
                        "Subscript applied to non-pointer: {:?}",
                        pointer_type_id
                    )
                    .into());
                }
            };

            // If the pointer is guided to a slice, convert it without decaying
            // to a raw pointer so `convert_pointer_offset` can index the slice
            // directly. Otherwise it must be ref-decayed for `.offset()`.
            let can_subscript = self.can_subscript(pointer_id);
            let pointer_ctx = if can_subscript {
                ctx.used()
            } else {
                ctx.used().decay_ref()
            };
            let pointer_rs = self.convert_expr(pointer_ctx, pointer_id, None)?;
            let offset_cty = self.ast_context[offset_id]
                .kind
                .get_qual_type()
                .ok_or_else(|| format_err!("offset_id bad type"))?;
            let offset_is_enum = matches!(
                self.ast_context.resolve_type(offset_cty.ctype).kind,
                CTypeKind::Enum(_)
            );
            let offset_rs = if offset_is_enum {
                // Enums are represented as newtypes, so preserve their C type long enough for
                // `convert_cast_from_enum` to extract the inner integer before casting it.
                let offset_type = if can_subscript {
                    CTypeKind::Size
                } else {
                    CTypeKind::SSize
                };
                let offset_type_id = self.ast_context.type_for_kind(&offset_type);
                self.convert_expr_with_cast(
                    ctx.used(),
                    CQualTypeId::new(offset_type_id),
                    offset_id,
                    &None,
                )?
            } else {
                // `convert_pointer_offset` performs the final usize/isize conversion.
                self.convert_expr(ctx.used(), offset_id, None)?
            };

            let mut val = pointer_rs
                .zip(offset_rs)
                .and_then(|(pointer_rs, offset_rs)| {
                    if offset_is_enum {
                        if can_subscript {
                            self.make_pointer_subscript(pointer_rs, offset_rs, deref)
                        } else {
                            self.make_pointer_offset(
                                pointer_rs,
                                offset_rs,
                                pointee_type_id.ctype,
                                false,
                                deref,
                            )
                        }
                    } else {
                        self.convert_pointer_offset(
                            Some(pointer_id),
                            pointer_rs,
                            offset_rs,
                            pointee_type_id.ctype,
                            false,
                            deref,
                        )
                    }
                });

            if lrvalue.is_rvalue() {
                let source_type_id = if deref {
                    pointee_type_id
                } else {
                    let pointer_type_id = self
                        .ast_context
                        .type_for_kind(&CTypeKind::Pointer(pointee_type_id));
                    CQualTypeId::new(pointer_type_id)
                };
                val = self.make_cast(
                    ctx,
                    source_type_id,
                    expected_type_id.unwrap_or(source_type_id),
                    val,
                    &None,
                )?;
            }

            Ok(val)
        }
    }

    /// Pointer offset that casts its argument to isize
    pub fn convert_pointer_offset(
        &self,
        c_ptr: Option<CExprId>,
        ptr: Box<Expr>,
        offset: Box<Expr>,
        pointee_cty: CTypeId,
        neg: bool,
        deref: bool,
    ) -> WithStmts<Box<Expr>> {
        if !neg && c_ptr.is_some_and(|ptr_id| self.can_subscript(ptr_id)) {
            let subscript = cast_int(offset, "usize", false);
            return self.make_pointer_subscript(ptr, subscript, deref);
        }

        self.make_pointer_offset(
            ptr,
            cast_int(offset, "isize", false),
            pointee_cty,
            neg,
            deref,
        )
    }

    /// Creates a slice indexing expression. Assumes that `subscript` is of type `usize`.
    fn make_pointer_subscript(
        &self,
        ptr: Box<Expr>,
        subscript: Box<Expr>,
        deref: bool,
    ) -> WithStmts<Box<Expr>> {
        let overall = if deref {
            // ptr[idx]
            mk().index_expr(ptr, subscript)
        } else {
            // XREF:guided_subscript_noderef
            // &ptr[idx..]
            mk().borrow_expr(mk().index_expr(ptr, mk().range_expr(Some(subscript), None)))
        };

        WithStmts::new_val(overall)
    }

    /// Creates a pointer offset expression. Assumes that `offset_rs` is of type `isize`.
    pub(crate) fn make_pointer_offset(
        &self,
        pointer_rs: Box<Expr>,
        mut offset_rs: Box<Expr>,
        pointee_type_id: CTypeId,
        neg: bool,
        mut deref: bool,
    ) -> WithStmts<Box<Expr>> {
        if let Some(mul) = self.compute_size_of_expr(pointee_type_id) {
            let mul = cast_int(mul, "isize", false);
            offset_rs = mk().binary_expr(BinOp::Mul(Default::default()), offset_rs, mul);
            deref = false;
        }

        if neg {
            offset_rs = neg_expr(offset_rs);
        }

        let mut expr = mk().method_call_expr(pointer_rs, "offset", vec![offset_rs]);

        if deref {
            expr = mk().unary_expr(UnOp::Deref(Default::default()), expr);
        }

        WithStmts::new_val(expr).set_unsafe()
    }

    /// Creates a pointer difference expression. Returns an expression of type `isize`.
    pub(crate) fn make_pointer_difference(
        &self,
        lhs_rs: Box<Expr>,
        rhs_rs: Box<Expr>,
        pointee_type_id: CTypeId,
    ) -> WithStmts<Box<Expr>> {
        let mut expr_rs = mk().method_call_expr(lhs_rs, "offset_from", vec![rhs_rs]);

        // If the pointee is a variable array type, the actual pointee type used by `offset_from`
        // will be its element type rather than the whole array. So we need to divide by the
        // variable holding the length of the array.
        if let Some(sz) = self.compute_size_of_expr(pointee_type_id) {
            let div_rs = cast_int(sz, "isize", false);
            expr_rs = mk().binary_expr(BinOp::Div(Default::default()), expr_rs, div_rs);
        }

        WithStmts::new_val(expr_rs).set_unsafe()
    }

    /// Construct an expression for a NULL at any type, including forward declarations,
    /// function pointers, and normal pointers.
    pub fn null_ptr(&self, type_id: CTypeId) -> TranslationResult<Box<Expr>> {
        if self.ast_context.is_function_pointer(type_id) {
            return Ok(mk().path_expr(vec!["None"]));
        }

        let pointer_qty = self
            .ast_context
            .get_pointee_qual_type(type_id)
            .ok_or_else(|| TranslationError::generic("null_ptr requires a pointer"))?;

        let func = if pointer_qty.qualifiers.is_const {
            "null"
        } else {
            "null_mut"
        };
        let pointee_ty = self.convert_pointee_type(pointer_qty.ctype)?;
        let type_args = mk().angle_bracketed_args(vec![pointee_ty.clone()]);
        let val = mk().call_expr(
            mk().abs_path_expr(vec![
                mk().path_segment("core"),
                mk().path_segment("ptr"),
                mk().path_segment_with_args(func, type_args),
            ]),
            vec![],
        );

        Ok(val)
    }

    fn convert_pointee_type(&self, type_id: CTypeId) -> TranslationResult<Box<Type>> {
        self.import_type(type_id);

        self.type_converter.borrow_mut().convert_pointee(
            &self.ast_context,
            type_id,
            &self.parsed_guidance.borrow(),
        )
    }

    pub fn convert_pointer_to_pointer_cast(
        &self,
        source_cty: CQualTypeId,
        target_cty: CQualTypeId,
        val: WithStmts<Box<Expr>>,
        c_expr: Option<CExprId>,
        guided_type: Option<tenjin::GuidedType>,
    ) -> TranslationResult<WithStmts<Box<Expr>>> {
        if self.ast_context.is_function_pointer(target_cty.ctype)
            || self.ast_context.is_function_pointer(source_cty.ctype)
        {
            let source_ty = self.type_converter.borrow_mut().convert(
                &self.ast_context,
                source_cty.ctype,
                &self.parsed_guidance.borrow(),
            )?;
            let target_ty = self.type_converter.borrow_mut().convert(
                &self.ast_context,
                target_cty.ctype,
                &self.parsed_guidance.borrow(),
            )?;

            if source_ty == target_ty {
                return Ok(val);
            }

            self.import_type(source_cty.ctype);
            self.import_type(target_cty.ctype);

            Ok(val.and_then(|val| {
                WithStmts::new_val(transmute_expr(source_ty, target_ty, val)).set_unsafe()
            }))
        } else {
            // Normal case

            // TENJIN-TODO: use type inference to decide whether we should be
            // omitting the cast, or using some other form of coercion.
            //if !is_explicit && guided_type.is_some() {
            //    return Ok(val);
            //}
            let source_ty_kind = &self.ast_context.resolve_type(source_cty.ctype).kind;
            let target_ty_kind = &self.ast_context.resolve_type(target_cty.ctype).kind;
            let guided_type: Option<tenjin::GuidedType> = match (guided_type, c_expr) {
                (Some(gt), _) => Some(gt.clone()),
                (None, Some(expr)) => self
                    .parsed_guidance
                    .borrow_mut()
                    .query_expr_type(self, expr),
                _ => {
                    log::warn!(
                        "No guided type and no C exprid for cast from {:?} to {:?}",
                        source_ty_kind,
                        target_ty_kind
                    );
                    None
                }
            };
            if let Some(guided_type) = guided_type {
                if let CTypeKind::Pointer(pcq) = source_ty_kind {
                    if let CTypeKind::Struct(s) = self.ast_context.resolve_type(pcq.ctype).kind {
                        // Casting from a pointer-to-struct

                        // Can we use bytemuck to do the cast safely?
                        let name = self.type_converter.borrow().resolve_decl_name(s).unwrap();
                        if self.parsed_guidance.borrow().pod_types.contains(&name) {
                            match &guided_type.parsed {
                                Type::Reference(tref) => {
                                    if tenjin::type_is_vec(&tref.elem) {
                                        // emit bytemuck::cast_slice_mut(&mut x)
                                        return Ok(val.map(|x| {
                                            mk().call_expr(
                                                mk().path_expr(vec!["bytemuck", "cast_slice_mut"]),
                                                vec![mk()
                                                    .set_mutbl(Mutability::Mutable)
                                                    .borrow_expr(x)],
                                            )
                                        }));
                                    }
                                    // emit bytemuck::cast_mut(&mut x)
                                    return Ok(val.map(|x| {
                                        mk().call_expr(
                                            mk().path_expr(vec!["bytemuck", "cast_mut"]),
                                            vec![mk()
                                                .set_mutbl(Mutability::Mutable)
                                                .borrow_expr(x)],
                                        )
                                    }));
                                }
                                _ => {
                                    log::error!(
                                        "Unhandled type guidance for cast: {:?}",
                                        guided_type
                                    );
                                }
                            }
                        }
                    } else {
                        // Casting from a pointer type, not a pointer-to-struct
                    }
                    // Casting from a pointer type.
                    // If our guidance is that we actually have a Vec, we need
                    // to insert an as_mut_ptr() call here.
                    if tenjin::type_is_vec(&guided_type.parsed) {
                        let target_ty = self.convert_type(target_cty.ctype)?;
                        return Ok(val.map(|x| {
                            let x_as_ptr =
                                mk().method_call_expr(x, "as_mut_ptr", Vec::<Box<Expr>>::new());

                            mk().cast_expr(x_as_ptr, target_ty)
                        }));
                    }
                }
            }

            let target_ty = self.convert_type(target_cty.ctype)?;
            Ok(val.map(|val| mk().cast_expr(val, target_ty)))
        }
    }

    pub fn convert_integral_to_pointer_cast(
        &self,
        ctx: ExprContext,
        source_cty: CQualTypeId,
        target_cty: CQualTypeId,
        val: WithStmts<Box<Expr>>,
    ) -> TranslationResult<WithStmts<Box<Expr>>> {
        let source_ty_kind = &self.ast_context.resolve_type(source_cty.ctype).kind;
        let target_ty = self.convert_type(target_cty.ctype)?;

        if self.ast_context.is_function_pointer(target_cty.ctype) {
            if ctx.is_const {
                return Err(format_translation_err!(
                    None,
                    "cannot transmute integers to Option<fn ...> in `const` context",
                ));
            }

            self.use_crate(ExternCrate::Libc);
            Ok(val.and_then(|mut val| {
                // First cast the integer to pointer size
                let intptr_t = mk().abs_path_ty(vec!["libc", "intptr_t"]);
                val = mk().cast_expr(val, intptr_t.clone());

                WithStmts::new_val(transmute_expr(intptr_t, target_ty, val)).set_unsafe()
            }))
        }
        // Rust 1.90: `const_strict_provenance` feature added
        // Rust 1.91: stabilized
        else if ctx.is_const && self.tcfg.edition < RustEdition::Edition2024 {
            if source_ty_kind.is_bool() {
                self.use_crate(ExternCrate::Libc);
                Ok(val.map(|mut val| {
                    // First cast the boolean to pointer size
                    val = mk().cast_expr(val, mk().abs_path_ty(vec!["libc", "size_t"]));
                    mk().cast_expr(val, target_ty)
                }))
            } else if let &CTypeKind::Enum(enum_id) = source_ty_kind {
                val.and_then_try(|val| self.convert_cast_from_enum(ctx, enum_id, target_cty, val))
            } else {
                Ok(val.map(|val| mk().cast_expr(val, target_ty)))
            }
        } else {
            // First cast the value to `usize`.
            let source_type_kind = &self.ast_context.resolve_type(source_cty.ctype).kind;
            let size_type_id = self.ast_context.type_for_kind(&CTypeKind::Size);

            let val = if let &CTypeKind::Enum(enum_id) = source_type_kind {
                val.and_then_try(|val| {
                    self.convert_cast_from_enum(ctx, enum_id, CQualTypeId::new(size_type_id), val)
                })?
            } else {
                let size_type_rs = self.convert_type(size_type_id)?;
                val.map(|val| mk().cast_expr(val, size_type_rs))
            };

            // Then convert the `usize` into a pointer.
            let pointee_type_id = self
                .ast_context
                .get_pointee_qual_type(target_cty.ctype)
                .expect("target type must be a pointer");
            let mutability = pointee_type_id.mutability();

            let fn_name = match mutability {
                Mutability::Immutable => "with_exposed_provenance",
                Mutability::Mutable => "with_exposed_provenance_mut",
            };
            let pointee_type_rs = self.convert_pointee_type(pointee_type_id.ctype)?;
            let type_args = mk().angle_bracketed_args(vec![pointee_type_rs]);
            let fn_expr = mk().abs_path_expr(vec![
                mk().path_segment("core"),
                mk().path_segment("ptr"),
                mk().path_segment_with_args(fn_name, type_args),
            ]);
            let val = val.map(|val| mk().call_expr(fn_expr, vec![val]));

            Ok(val)
        }
    }

    pub fn convert_pointer_to_integral_cast(
        &self,
        ctx: ExprContext,
        source_cty: CQualTypeId,
        target_cty: CQualTypeId,
        val: WithStmts<Box<Expr>>,
    ) -> TranslationResult<WithStmts<Box<Expr>>> {
        if ctx.is_const {
            return Err(format_translation_err!(
                None,
                "cannot observe pointer values in `const` context",
            ));
        }

        let target_type_rs = self.convert_type(target_cty.ctype)?;

        if self.ast_context.is_function_pointer(source_cty.ctype) {
            let source_ty = self.convert_type(source_cty.ctype)?;

            Ok(val.and_then(|val| {
                WithStmts::new_val(transmute_expr(source_ty, target_type_rs, val)).set_unsafe()
            }))
        } else {
            // First convert the pointer to `usize`.
            let val = val.map(|val| mk().method_call_expr(val, "expose_provenance", vec![]));

            // Then cast the `usize` to the target type.
            let size_type_id = self.ast_context.type_for_kind(&CTypeKind::Size);
            let target_ty_kind = &self.ast_context.resolve_type(target_cty.ctype).kind;

            if let &CTypeKind::Enum(enum_decl_id) = target_ty_kind {
                val.and_then_try(|val| {
                    self.convert_cast_to_enum(
                        ctx,
                        CQualTypeId::new(size_type_id),
                        enum_decl_id,
                        val,
                    )
                })
            } else {
                Ok(val.map(|val| mk().cast_expr(val, target_type_rs)))
            }
        }
    }

    pub fn convert_pointer_is_null(
        &self,
        ctx: ExprContext,
        ptr_type: CTypeId,
        val: Box<Expr>,
        is_null: bool,
        mb_guided_type: &Option<tenjin::GuidedType>,
    ) -> TranslationResult<Box<Expr>> {
        if let Some(guided_type) = mb_guided_type {
            if guided_type.pretty == "String" {
                // strings are never null
                // XREF:guided_condition_string_null_check_neq
                return Ok(mk().lit_expr(mk().bool_lit(!is_null)));
            }
        }

        Ok(if self.ast_context.is_function_pointer(ptr_type) {
            let method = if is_null { "is_none" } else { "is_some" };
            mk().method_call_expr(val, method, vec![])
        } else {
            // TODO: `pointer::is_null` becomes stably const in Rust 1.84.
            if ctx.is_const {
                return Err(format_translation_err!(
                    None,
                    "cannot check nullity of pointer in `const` context",
                ));
            }
            // TENJIN:TODO: guided references return false
            let val = mk().method_call_expr(val, "is_null", vec![]);
            if !is_null {
                mk().unary_expr(UnOp::Not(Default::default()), val)
            } else {
                val
            }
        })
    }
}
