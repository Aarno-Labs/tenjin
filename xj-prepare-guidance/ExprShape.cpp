#include "ExprShape.h"

#include "clang/AST/ParentMapContext.h"

using namespace clang;

namespace xj {

static const Expr *stripCasts(const Expr *E, bool ThroughBitCasts) {
  while (true) {
    E = E->IgnoreParens();
    const auto *ICE = dyn_cast<ImplicitCastExpr>(E);
    if (!ICE)
      return E;
    switch (ICE->getCastKind()) {
    case CK_LValueToRValue:
    case CK_ArrayToPointerDecay:
    case CK_NoOp:
      break;
    case CK_BitCast:
      if (ThroughBitCasts)
        break;
      return E;
    default:
      return E;
    }
    E = ICE->getSubExpr();
  }
}

const Expr *stripValueCasts(const Expr *E) { return stripCasts(E, true); }

const Expr *stripGuidanceCasts(const Expr *E) { return stripCasts(E, false); }

const Expr *valueExpr(const Expr *E) {
  while (const auto *ICE = dyn_cast<ImplicitCastExpr>(E->IgnoreParens())) {
    CastKind K = ICE->getCastKind();
    if (K == CK_LValueToRValue || K == CK_ArrayToPointerDecay ||
        K == CK_FunctionToPointerDecay)
      return ICE;
    E = ICE->getSubExpr();
  }
  return E->IgnoreParens();
}

QualType valueType(const Expr *E) { return valueExpr(E)->getType(); }

QualType writtenType(const Expr *E) {
  const Expr *V = valueExpr(E);
  if (const auto *ICE = dyn_cast<ImplicitCastExpr>(V))
    if (ICE->getCastKind() == CK_ArrayToPointerDecay)
      return ICE->getSubExpr()->getType();
  return V->getType();
}

std::optional<PointerSum> asPointerSum(const Expr *E) {
  const auto *BO = dyn_cast<BinaryOperator>(E);
  if (!BO || BO->getOpcode() != BO_Add || !BO->getType()->isPointerType())
    return std::nullopt;
  if (BO->getLHS()->getType()->isPointerType())
    return PointerSum{BO->getLHS(), BO->getRHS()};
  return PointerSum{BO->getRHS(), BO->getLHS()};
}

const UnaryOperator *cancelledDeref(const UnaryOperator *AddrOf) {
  if (AddrOf->getOpcode() != UO_AddrOf)
    return nullptr;
  const auto *Deref =
      dyn_cast<UnaryOperator>(AddrOf->getSubExpr()->IgnoreParens());
  return Deref && Deref->getOpcode() == UO_Deref ? Deref : nullptr;
}

const Stmt *parentThroughParens(ASTContext &Ctx, const Expr *E) {
  while (true) {
    auto Parents = Ctx.getParents(*E);
    if (Parents.size() != 1)
      return nullptr;
    const auto *PE = Parents[0].get<ParenExpr>();
    if (!PE)
      return Parents[0].get<Stmt>();
    E = PE;
  }
}

bool isRead(ASTContext &Ctx, const Expr *E) {
  const auto *ICE =
      dyn_cast_or_null<ImplicitCastExpr>(parentThroughParens(Ctx, E));
  return ICE && ICE->getCastKind() == CK_LValueToRValue;
}

bool isStatement(ASTContext &Ctx, const Expr *E) {
  auto Parents = Ctx.getParents(*E);
  if (Parents.size() != 1)
    return false;
  const DynTypedNode &P = Parents[0];
  if (P.get<CompoundStmt>())
    return true;
  if (const auto *If = P.get<IfStmt>())
    return E == If->getThen() || E == If->getElse();
  if (const auto *While = P.get<WhileStmt>())
    return E == While->getBody();
  if (const auto *Do = P.get<DoStmt>())
    return E == Do->getBody();
  if (const auto *For = P.get<ForStmt>())
    return E == For->getInit() || E == For->getInc() || E == For->getBody();
  if (const auto *Switch = P.get<SwitchStmt>())
    return E == Switch->getBody();
  if (const auto *Case = P.get<SwitchCase>())
    return E == Case->getSubStmt();
  if (const auto *Label = P.get<LabelStmt>())
    return E == Label->getSubStmt();
  return false;
}

} // namespace xj
