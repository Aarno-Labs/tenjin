#include "Transfer.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Expr.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/Stmt.h"
#include "clang/Analysis/CFG.h"

namespace xj::analysis
{

  namespace
  {

    // Strip only the wrappers that do not change the value: parentheses,
    // the lvalue-to-rvalue conversion, and no-op casts. Anything else — an
    // integral-to-pointer cast, a bitcast between unrelated pointer types —
    // is left in place, because it is exactly the kind of thing that must
    // classify as Opaque rather than be seen through.
    const clang::Expr *stripToValue(const clang::Expr *E)
    {
      while (E != nullptr)
      {
        if (const auto *P = llvm::dyn_cast<clang::ParenExpr>(E))
        {
          E = P->getSubExpr();
          continue;
        }
        if (const auto *ICE = llvm::dyn_cast<clang::ImplicitCastExpr>(E))
        {
          if (ICE->getCastKind() == clang::CK_LValueToRValue ||
              ICE->getCastKind() == clang::CK_NoOp)
          {
            E = ICE->getSubExpr();
            continue;
          }
        }
        break;
      }
      return E;
    }

    // The constructs that make the CFG an unsound model of the function's
    // control flow. A clobber at the call site does not repair a missing
    // edge, so the only sound response is to decline the function.
    class DeclineDetector : public clang::RecursiveASTVisitor<DeclineDetector>
    {
    public:
      bool Declined = false;

      bool VisitIndirectGotoStmt(clang::IndirectGotoStmt *)
      {
        Declined = true;
        return false;
      }

      bool VisitAddrLabelExpr(clang::AddrLabelExpr *)
      {
        Declined = true;
        return false;
      }

      bool VisitGCCAsmStmt(clang::GCCAsmStmt *A)
      {
        for (unsigned I = 0, N = A->getNumClobbers(); I != N; ++I)
          if (A->getClobber(I) == "memory")
          {
            Declined = true;
            return false;
          }
        return true;
      }

      bool VisitCallExpr(clang::CallExpr *CE)
      {
        const clang::FunctionDecl *Callee = CE->getDirectCallee();
        if (Callee == nullptr)
          return true;
        // `longjmp` re-enters at the `setjmp` point along an edge the CFG
        // does not contain, so the fixpoint would be computed over a graph
        // missing paths.
        llvm::StringRef Name = Callee->getName();
        if (Name == "setjmp" || Name == "_setjmp" || Name == "sigsetjmp" ||
            Name == "__builtin_setjmp")
        {
          Declined = true;
          return false;
        }
        return true;
      }
    };

  } // namespace

  RValue classify(const clang::Expr *E, const CellUniverse &Cells,
                  clang::ASTContext &Ctx)
  {
    RValue R;
    if (E == nullptr)
      return R;

    const clang::Expr *S = stripToValue(E);

    if (const auto *UO = llvm::dyn_cast<clang::UnaryOperator>(S))
    {
      if (UO->getOpcode() == clang::UO_AddrOf)
        if (auto C = Cells.lookup(UO->getSubExpr()))
        {
          R.K = RValue::Kind::Address;
          R.Cell = *C;
        }
      return R;
    }

    if (const auto *ICE = llvm::dyn_cast<clang::ImplicitCastExpr>(S))
    {
      if (ICE->getCastKind() == clang::CK_ArrayToPointerDecay)
        if (auto C = Cells.lookup(ICE->getSubExpr()))
        {
          R.K = RValue::Kind::Address;
          R.Cell = *C;
        }
      return R;
    }

    if (auto C = Cells.lookup(S))
    {
      R.K = RValue::Kind::CellRead;
      R.Cell = *C;
      return R;
    }

    if (E->isNullPointerConstant(Ctx, clang::Expr::NPC_ValueDependentIsNotNull))
    {
      R.K = RValue::Kind::Constant;
      R.Value = llvm::APSInt::get(0);
      return R;
    }
    if (const auto *IL = llvm::dyn_cast<clang::IntegerLiteral>(S))
    {
      R.K = RValue::Kind::Constant;
      R.Value = llvm::APSInt(IL->getValue(), /*isUnsigned=*/false);
      return R;
    }

    return R;
  }

  Transfer::Transfer(clang::ASTContext &Ctx, const CellUniverse &Cells,
                     const EscapeInfo &Escapes)
      : Ctx(Ctx), Cells(Cells), Escapes(Escapes) {}

  bool Transfer::declines(const clang::FunctionDecl &FD)
  {
    const clang::Stmt *Body = FD.getBody();
    if (Body == nullptr)
      return true;
    DeclineDetector D;
    D.TraverseStmt(const_cast<clang::Stmt *>(Body));
    return D.Declined;
  }

  void Transfer::apply(const clang::CFGElement &Elt, SED &State) const
  {
    if (State.isBottom())
      return;

    auto CS = Elt.getAs<clang::CFGStmt>();
    if (!CS)
      return; // scope markers and lifetime ends write nothing

    const clang::Stmt *St = CS->getStmt();

    // T1, via a declaration's initialiser.
    if (const auto *DS = llvm::dyn_cast<clang::DeclStmt>(St))
    {
      for (const clang::Decl *D : DS->decls())
      {
        const auto *VD = llvm::dyn_cast<clang::VarDecl>(D);
        if (VD == nullptr)
          continue;
        auto Dest = Cells.lookup(VD);
        if (!Dest)
          continue;
        if (const clang::Expr *Init = VD->getInit())
          store(*Dest, classify(Init, Cells, Ctx), State);
        else
          // Re-entering the scope produces a new, indeterminate object; it
          // is not still holding whatever the last iteration left.
          State.detach(*Dest);
      }
      return;
    }

    if (const auto *BO = llvm::dyn_cast<clang::BinaryOperator>(St))
    {
      if (!BO->isAssignmentOp())
        return; // arithmetic and comparison write nothing
      auto Dest = Cells.lookup(BO->getLHS());
      // A compound assignment computes from the old value, which this
      // domain does not model, so its result is Opaque whatever the RHS is.
      RValue V; // default-constructs as Opaque
      if (BO->getOpcode() == clang::BO_Assign)
        V = classify(BO->getRHS(), Cells, Ctx);
      if (!Dest)
      {
        // T2: the destination is not nameable, so the store could have
        // landed anywhere the writer can reach.
        havocReachable(State);
        return;
      }
      store(*Dest, V, State);
      return;
    }

    if (const auto *UO = llvm::dyn_cast<clang::UnaryOperator>(St))
    {
      if (!UO->isIncrementDecrementOp())
        return;
      auto Dest = Cells.lookup(UO->getSubExpr());
      if (!Dest)
      {
        // The store could have landed anywhere
        havocReachable(State);
        return;
      }
      RValue Opaque; // Essentially just detach Dest
      store(*Dest, Opaque, State);
      return;
    }

    if (llvm::isa<clang::CallExpr>(St))
    {
      havocReachable(State);
      return;
    }

    if (llvm::isa<clang::Expr>(St))
      return;

    // fail-safe
    havocAll(State);
  }

  void Transfer::store(CellId Dest, const RValue &V, SED &State) const
  {
    switch (V.K)
    {
    case RValue::Kind::CellRead:
      State.move(Dest, V.Cell);
      break;
    case RValue::Kind::Address:
      State.move(Dest, Label::addrOf(V.Cell));
      break;
    case RValue::Kind::Constant:
      State.move(Dest, Label::constant(V.Value));
      break;
    case RValue::Kind::Opaque:
      State.detach(Dest);
      break;
    }

    havocInvalidated(Dest, State);
  }

  void Transfer::havocReachable(SED &State) const
  {
    for (unsigned I = 0, N = Cells.size(); I != N; ++I)
    {
      CellId C = CellUniverse::id(I);
      if (!isOutOfReach(C, Cells, Escapes))
        State.detach(C);
    }
  }

  void Transfer::havocInvalidated(CellId Dest, SED &State) const
  {
    for (CellId C : Cells.ids())
    {
      if (C == Dest)
        continue;
      // The two ways a store falsifies a recorded equality, which are not
      // the same question: the store may have changed what `C`'s storage
      // *holds*, or it may have changed *which storage* `C` names.
      if (denotationDependsOn(C, Dest, Cells) ||
          mayOverlap(C, Dest, Cells, Escapes))
        State.detach(C);
    }
  }

  void Transfer::havocAll(SED &State) const
  {
    for (CellId C : Cells.ids())
    {
      State.detach(C);
    }
  }

} // namespace xj::analysis
