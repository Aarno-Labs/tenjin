//! Compute a map from `CExprId`s to the `CExprId` of their parent expression.

use crate::c_ast::iterators::SomeId;
use crate::c_ast::{iterators::DFExpr, CDeclKind, CExprId, CStmtId, TypedAstContext};
use std::collections::HashMap;

struct ParentExprCollector<'a> {
    ast_context: &'a TypedAstContext,
    parent_expr_map: HashMap<CExprId, CExprId>,
}

impl<'a> ParentExprCollector<'a> {
    fn new(ast_context: &'a TypedAstContext) -> Self {
        ParentExprCollector {
            ast_context,
            parent_expr_map: HashMap::new(),
        }
    }

    fn visit_function_body(&mut self, body_id: CStmtId) {
        let iter = DFExpr::new(self.ast_context, SomeId::Stmt(body_id));
        for node in iter {
            if let SomeId::Expr(expr_id) = node {
                for child in crate::c_ast::iterators::immediate_children_all_types(
                    self.ast_context,
                    SomeId::Expr(expr_id),
                ) {
                    if let SomeId::Expr(child_expr_id) = child {
                        self.parent_expr_map.insert(child_expr_id, expr_id);
                    }
                }
            }
        }
    }
}

pub fn compute_parent_expr_map(ast_context: &TypedAstContext) -> HashMap<CExprId, CExprId> {
    let mut collector = ParentExprCollector::new(ast_context);
    for &decl_id in ast_context.c_decls_top.iter() {
        let decl = &ast_context[decl_id];
        if let CDeclKind::Function {
            body: Some(body_id),
            ..
        } = decl.kind
        {
            collector.visit_function_body(body_id);
        }
    }
    collector.parent_expr_map
}
