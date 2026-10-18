use super::*;

impl Translation<'_> {
    pub(super) fn variable_alignment(&self, decl: CDeclId) -> Option<u64> {
        match self.ast_context[decl].kind {
            CDeclKind::Variable {
                manual_alignment, ..
            } => manual_alignment,
            _ => None,
        }
    }

    /// Alignment changes storage, while C expressions still access the original
    /// payload type. A tuple wrapper lets Rust express both properties.
    pub(super) fn aligned_variable_access(&self, decl: CDeclId, value: Box<Expr>) -> Box<Expr> {
        if self.variable_alignment(decl).is_some() {
            mk().anon_field_expr(value, 0)
        } else {
            value
        }
    }

    fn alignment_wrapper(&self, alignment: u64) -> Ident {
        let key = (self.cur_file(), alignment);
        if let Some(name) = self.variable_alignment_types.borrow().get(&key) {
            return name.clone();
        }
        let name = self.renamer.borrow_mut().pick_name_root(
            &format!("C2Rust_Aligned{alignment}"),
            Namespaces::types() | Namespaces::values(),
        );
        let name = mk().ident(name);
        let alignment = syn::LitInt::new(&alignment.to_string(), Span::call_site());
        let item = syn::parse_quote! {
            #[derive(Clone, Copy)]
            #[repr(C, align(#alignment))]
            pub struct #name<T>(pub T);
        };
        self.with_cur_file_item_store(|store| store.add_item(Box::new(item)));
        self.variable_alignment_types
            .borrow_mut()
            .insert(key, name.clone());
        name
    }

    pub(super) fn aligned_variable_init(&self, decl: CDeclId, value: Box<Expr>) -> Box<Expr> {
        match self.variable_alignment(decl) {
            Some(alignment) => mk().call_expr(
                mk().ident_expr(self.alignment_wrapper(alignment)),
                vec![value],
            ),
            None => value,
        }
    }

    pub(super) fn align_variable(
        &self,
        decl: CDeclId,
        variable: ConvertedVariable,
    ) -> TranslationResult<ConvertedVariable> {
        let Some(alignment) = self.variable_alignment(decl) else {
            return Ok(variable);
        };
        let CDeclKind::Variable { typ, .. } = self.ast_context[decl].kind else {
            unreachable!()
        };
        if matches!(
            self.ast_context.resolve_type(typ.ctype).kind,
            CTypeKind::VariableArray(..)
        ) {
            return Err(TranslationError::generic(
                "Explicit alignment on a variable-length array is not supported",
            ));
        }
        let name = self.alignment_wrapper(alignment);
        let ConvertedVariable { ty, mutbl, init } = variable;
        Ok(ConvertedVariable {
            ty: Box::new(syn::parse_quote!(#name<#ty>)),
            mutbl,
            init: init.map(|init| init.map(|value| self.aligned_variable_init(decl, value))),
        })
    }
}
