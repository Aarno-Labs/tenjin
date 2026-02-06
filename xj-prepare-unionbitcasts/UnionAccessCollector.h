#pragma once

#include "Common.h"

// ============================================================================
// UnionAccessCollector - Collects union field accesses in a function
// ============================================================================

class UnionAccessCollector : public RecursiveASTVisitor<UnionAccessCollector> {
  public:
    explicit UnionAccessCollector(ASTContext &Ctx);

    bool VisitMemberExpr(MemberExpr *ME);
    bool VisitVarDecl(VarDecl *VD);

    std::map<TypePunCandidate, std::vector<FieldAccess>> accesses;

  private:
    ASTContext &Ctx;
};
