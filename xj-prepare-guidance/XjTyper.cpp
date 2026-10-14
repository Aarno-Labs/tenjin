#include "XjTyper.h"

using namespace clang;

namespace xj {

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
    if (const UnaryOperator *Deref = cancelledDeref(UO))
      return of(Deref->getSubExpr());
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
  E = stripGuidanceCasts(E);
  if (isa<ImplicitCastExpr>(E))
    return {};
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

bool XjTyper::isGuidedPointer(const Expr *E) const {
  return valueType(E)->isPointerType() && of(E).rust();
}

} // namespace xj
