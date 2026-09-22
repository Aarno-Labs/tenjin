#include "XjTyper.h"

using namespace clang;

namespace xj {

const Expr *stripValueCasts(const Expr *E) {
  while (true) {
    E = E->IgnoreParens();
    const auto *ICE = dyn_cast<ImplicitCastExpr>(E);
    if (!ICE)
      return E;
    switch (ICE->getCastKind()) {
    case CK_LValueToRValue:
    case CK_ArrayToPointerDecay:
    case CK_NoOp:
    case CK_BitCast:
      E = ICE->getSubExpr();
      break;
    default:
      return E;
    }
  }
}

XjType XjTyper::ofDecl(const Decl *D) const {
  auto It = S.DeclKeys.find(D);
  return It == S.DeclKeys.end() ? XjType{} : XjType{It->second, 0};
}

XjType XjTyper::ofReturn(const FunctionDecl *FD) const {
  auto It = S.ReturnKeys.find(FD);
  return It == S.ReturnKeys.end() ? XjType{} : XjType{It->second, 0};
}

XjType XjTyper::element(XjType X) const {
  if (!X)
    return {};
  if (X.AddrOf)
    return {X.Key, X.AddrOf - 1};
  if (X.Key->ElementId.empty())
    return {};
  return {S.Reg.typedefById(X.Key->ElementId), 0};
}

XjType XjTyper::ofCall(const CallExpr *CE) const {
  const FunctionDecl *FD = CE->getDirectCallee();
  return FD ? ofReturn(FD) : XjType{};
}

XjType XjTyper::ofConditional(const ConditionalOperator *CO) const {
  XjType T = of(CO->getTrueExpr());
  XjType F = of(CO->getFalseExpr());
  return T.Key == F.Key && T.AddrOf == F.AddrOf ? T : XjType{};
}

XjType XjTyper::ofUnary(const UnaryOperator *UO) const {
  switch (UO->getOpcode()) {
  case UO_Deref:
    return element(of(UO->getSubExpr()));
  case UO_AddrOf: {
    const auto *Inner = dyn_cast<UnaryOperator>(UO->getSubExpr()->IgnoreParens());
    if (Inner && Inner->getOpcode() == UO_Deref)
      return of(Inner->getSubExpr());
    XjType X = of(UO->getSubExpr());
    if (X)
      ++X.AddrOf;
    return X;
  }
  default:
    return {};
  }
}

XjType XjTyper::of(const Expr *E) const {
  E = E->IgnoreParens();
  if (const auto *ICE = dyn_cast<ImplicitCastExpr>(E)) {
    CastKind K = ICE->getCastKind();
    bool KeepsValue =
        K == CK_LValueToRValue || K == CK_NoOp || K == CK_ArrayToPointerDecay;
    return KeepsValue ? of(ICE->getSubExpr()) : XjType{};
  }
  if (const auto *DRE = dyn_cast<DeclRefExpr>(E))
    return ofDecl(DRE->getDecl());
  if (const auto *ME = dyn_cast<MemberExpr>(E))
    return ofDecl(ME->getMemberDecl());
  if (const auto *CE = dyn_cast<CallExpr>(E))
    return ofCall(CE);
  if (const auto *ASE = dyn_cast<ArraySubscriptExpr>(E))
    return element(of(ASE->getBase()));
  if (const auto *UO = dyn_cast<UnaryOperator>(E))
    return ofUnary(UO);
  if (const auto *CO = dyn_cast<ConditionalOperator>(E))
    return ofConditional(CO);
  return {};
}

bool XjTyper::isBuffer(const Expr *E) const {
  XjType X = of(E);
  return X.rust() && X.rust()->isBuffer();
}

bool XjTyper::isSingleObject(const Expr *E) const {
  XjType X = of(E);
  return X.rust() && X.rust()->isSingleObject();
}

} // namespace xj
