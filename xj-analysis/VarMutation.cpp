#include "VarMutation.h"

#include "clang/AST/Expr.h"
#include "clang/AST/RecursiveASTVisitor.h"

namespace xj::analysis
{

  namespace
  {

    class MutationFinder : public clang::RecursiveASTVisitor<MutationFinder>
    {
    public:
      explicit MutationFinder(const clang::VarDecl *V)
          : V(V->getCanonicalDecl()) {}

      bool Mutated = false;

      bool VisitBinaryOperator(clang::BinaryOperator *BO)
      {
        if (BO->isAssignmentOp() && isTarget(BO->getLHS()))
          Mutated = true;
        return !Mutated;
      }

      bool VisitUnaryOperator(clang::UnaryOperator *UO)
      {
        // `&x` counts as a possible write: an escaping local can be written
        // by anyone holding the pointer.
        if ((UO->isIncrementDecrementOp() ||
             UO->getOpcode() == clang::UO_AddrOf) &&
            isTarget(UO->getSubExpr()))
          Mutated = true;
        return !Mutated;
      }

    private:
      bool isTarget(const clang::Expr *E) const
      {
        if (E == nullptr)
          return false;
        const auto *DRE =
            llvm::dyn_cast<clang::DeclRefExpr>(E->IgnoreParenImpCasts());
        if (DRE == nullptr)
          return false;
        const auto *VD = llvm::dyn_cast<clang::VarDecl>(DRE->getDecl());
        return VD != nullptr && VD->getCanonicalDecl() == V;
      }

      const clang::VarDecl *V;
    };

  } // namespace

  bool neverReassigned(const clang::VarDecl *V, const clang::FunctionDecl &FD)
  {
    if (V == nullptr || FD.getBody() == nullptr)
      return false;
    MutationFinder F(V);
    F.TraverseStmt(const_cast<clang::Stmt *>(FD.getBody()));
    return !F.Mutated;
  }

} // namespace xj::analysis
