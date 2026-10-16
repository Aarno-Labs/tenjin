//! Recover assertions from the control flow emitted by system assert macros.

use super::*;

impl Translation<'_> {
    fn assert_unwrap(&self, mut expr: CExprId) -> CExprId {
        loop {
            match self.ast_context[expr].kind {
                CExprKind::Paren(_, inner)
                | CExprKind::ImplicitCast(_, inner, ..)
                | CExprKind::Unary(_, CUnOp::Extension, inner, _) => expr = inner,
                _ => return expr,
            }
        }
    }

    fn assert_void_zero(&self, expr: CExprId) -> bool {
        let CExprKind::ExplicitCast(ty, inner, ..) =
            self.ast_context[self.assert_unwrap(expr)].kind
        else {
            return false;
        };
        matches!(
            self.ast_context.resolve_type(ty.ctype).kind,
            CTypeKind::Void
        ) && self.assert_zero(inner)
    }

    fn assert_zero(&self, expr: CExprId) -> bool {
        matches!(
            self.ast_context[self.assert_unwrap(expr)].kind,
            CExprKind::Literal(_, CLiteral::Integer(0, _))
        )
    }

    fn assert_metadata(&self, expr: CExprId) -> bool {
        match self.ast_context[self.assert_unwrap(expr)].kind {
            CExprKind::Literal(_, CLiteral::Integer(..) | CLiteral::String(..)) => true,
            CExprKind::Predefined(_, inner) => self.assert_metadata(inner),
            _ => false,
        }
    }

    fn assert_failure(&self, expr: CExprId) -> bool {
        let CExprKind::Call(_, callee, ref args) = self.ast_context[self.assert_unwrap(expr)].kind
        else {
            return false;
        };
        let Some(CDeclKind::Function {
            name, body: None, ..
        }) = self.ast_context.fn_declref_decl(callee)
        else {
            return false;
        };
        // These arguments describe the failed expression and its location.
        // Require literal metadata so the rewrite cannot discard side effects.
        matches!(
            (name.as_str(), args.len()),
            ("__assert_fail" | "__assert_rtn", 4) | ("__assert", 3)
        ) && args.iter().all(|&arg| self.assert_metadata(arg))
    }

    fn assert_empty_stmt(&self, stmt: CStmtId) -> bool {
        match &self.ast_context[stmt].kind {
            CStmtKind::Empty => true,
            CStmtKind::Compound(stmts) => stmts.is_empty(),
            CStmtKind::Expr(expr) => self.assert_void_zero(*expr),
            _ => false,
        }
    }

    fn assert_failure_stmt(&self, stmt: CStmtId) -> bool {
        match &self.ast_context[stmt].kind {
            CStmtKind::Expr(expr) => self.assert_failure(*expr),
            CStmtKind::Compound(stmts) if stmts.len() == 1 => self.assert_failure_stmt(stmts[0]),
            _ => false,
        }
    }

    pub(super) fn convert_expanded_assert(
        &self,
        ctx: ExprContext,
        expr: CExprId,
    ) -> TranslationResult<Option<WithStmts<Box<Expr>>>> {
        let (mut cond, mut expected) = match &self.ast_context[self.assert_unwrap(expr)].kind {
            // glibc's GNU C expansion: ({ if (cond) ; else __assert_fail(...); })
            CExprKind::Statements(_, stmt) => {
                let CStmtKind::Compound(stmts) = &self.ast_context[*stmt].kind else {
                    return Ok(None);
                };
                let [stmt] = stmts.as_slice() else {
                    return Ok(None);
                };
                let CStmtKind::If {
                    scrutinee,
                    true_variant,
                    false_variant: Some(false_variant),
                } = self.ast_context[*stmt].kind
                else {
                    return Ok(None);
                };
                if self.assert_empty_stmt(true_variant) && self.assert_failure_stmt(false_variant) {
                    (scrutinee, true)
                } else {
                    return Ok(None);
                }
            }
            // Strict C glibc and Darwin use conditional expressions.
            CExprKind::Conditional(_, cond, yes, no) => {
                if self.assert_void_zero(*yes) && self.assert_failure(*no) {
                    (*cond, true)
                } else if self.assert_failure(*yes) && self.assert_void_zero(*no) {
                    (*cond, false)
                } else {
                    return Ok(None);
                }
            }
            // musl uses (void)(cond || (__assert_fail(...), 0)). Only replace
            // the logical expression when its integer result is discarded.
            CExprKind::Binary(_, CBinOp::Or, cond, failure, ..) if ctx.is_unused() => {
                let CExprKind::Binary(_, CBinOp::Comma, call, zero, ..) =
                    self.ast_context[self.assert_unwrap(*failure)].kind
                else {
                    return Ok(None);
                };
                if self.assert_failure(call) && self.assert_zero(zero) {
                    (*cond, true)
                } else {
                    return Ok(None);
                }
            }
            _ => return Ok(None),
        };
        // Darwin tests __builtin_expect(!cond, 0). Recover the original
        // condition before recognizing the COND && "message" idiom.
        loop {
            match &self.ast_context[self.assert_unwrap(cond)].kind {
                CExprKind::Unary(_, CUnOp::Not, inner, _) => {
                    cond = *inner;
                    expected = !expected;
                }
                CExprKind::Call(_, callee, args)
                    if args.len() == 2 && self.assert_metadata(args[1]) =>
                {
                    let CExprKind::DeclRef(_, decl, _) =
                        self.ast_context[self.assert_unwrap(*callee)].kind
                    else {
                        break;
                    };
                    if self.ast_context[decl]
                        .kind
                        .get_name()
                        .is_none_or(|name| name != "__builtin_expect")
                    {
                        break;
                    }
                    cond = args[0];
                }
                _ => break,
            }
        }
        let assertion = self.convert_assert_condition(ctx, cond, expected)?;
        Ok(Some(self.convert_side_effects_expr(
            ctx,
            assertion,
            "Assertion value is not supposed to be used",
        )))
    }
}
