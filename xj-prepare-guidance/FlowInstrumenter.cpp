#include "FlowInstrumenter.h"

#include "clang/AST/RecursiveASTVisitor.h"

using namespace clang;

namespace xj {

namespace {

class FlowVisitor : public RecursiveASTVisitor<FlowVisitor> {
public:
  explicit FlowVisitor(FlowInstrumenter &F) : F(F) {}

  bool TraverseFunctionDecl(FunctionDecl *FD) {
    const FunctionDecl *Saved = Current;
    Current = FD;
    bool Continue = RecursiveASTVisitor::TraverseFunctionDecl(FD);
    Current = Saved;
    return Continue;
  }
  // Static initializers are constant expressions and stay as written.
  bool TraverseVarDecl(VarDecl *VD) {
    return VD->hasGlobalStorage() || RecursiveASTVisitor::TraverseVarDecl(VD);
  }
  bool TraverseUnaryExprOrTypeTraitExpr(UnaryExprOrTypeTraitExpr *) {
    return true;
  }
  bool TraverseGenericSelectionExpr(GenericSelectionExpr *) { return true; }

  bool VisitCallExpr(CallExpr *CE) {
    F.visitCall(CE);
    return true;
  }
  bool VisitVarDecl(VarDecl *VD) {
    F.visitLocalInit(VD);
    return true;
  }
  bool VisitReturnStmt(ReturnStmt *RS) {
    F.visitReturn(RS, Current);
    return true;
  }
  bool VisitCStyleCastExpr(CStyleCastExpr *CE) {
    F.visitExplicitCast(CE);
    return true;
  }
  bool VisitBinaryOperator(BinaryOperator *BO) {
    if (BO->getOpcode() == BO_Assign)
      F.visitAssign(BO);
    else if (BO->isEqualityOp())
      F.visitNullComparison(BO);
    else if (BO->isLogicalOp()) {
      F.visitCondition(BO->getLHS());
      F.visitCondition(BO->getRHS());
    }
    return true;
  }
  bool VisitUnaryOperator(UnaryOperator *UO) {
    if (UO->getOpcode() == UO_LNot)
      F.visitLogicalNot(UO);
    return true;
  }
  bool VisitIfStmt(IfStmt *S) {
    F.visitCondition(S->getCond());
    return true;
  }
  bool VisitWhileStmt(WhileStmt *S) {
    F.visitCondition(S->getCond());
    return true;
  }
  bool VisitDoStmt(DoStmt *S) {
    F.visitCondition(S->getCond());
    return true;
  }
  bool VisitForStmt(ForStmt *S) {
    F.visitCondition(S->getCond());
    return true;
  }
  bool VisitConditionalOperator(ConditionalOperator *CO) {
    F.visitCondition(CO->getCond());
    return true;
  }

private:
  FlowInstrumenter &F;
  const FunctionDecl *Current = nullptr;
};

const FunctionProtoType *calleePrototype(const CallExpr *CE) {
  QualType T = CE->getCallee()->getType();
  if (const auto *PT = T->getAs<PointerType>())
    T = PT->getPointeeType();
  return T->getAs<FunctionProtoType>();
}

bool isNullConstant(const Expr *E, ASTContext &Ctx) {
  return E->isNullPointerConstant(Ctx, Expr::NPC_ValueDependentIsNotNull) !=
         Expr::NPCK_NotNull;
}

} // namespace

// A string literal's elements are const for the transpiler, which parses
// with -Wwrite-strings semantics; a marker taking one says so.
static QualType constIfLiteral(ASTContext &Ctx, const Expr *E, QualType T) {
  if (!isa<StringLiteral>(stripValueCasts(E)) || !T->isPointerType())
    return T;
  return Ctx.getPointerType(T->getPointeeType().withConst());
}

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

void FlowInstrumenter::run() {
  FlowVisitor(*this).TraverseDecl(S.Ctx.getTranslationUnitDecl());
}

void FlowInstrumenter::visitCall(const CallExpr *CE) {
  const FunctionProtoType *FPT = calleePrototype(CE);
  if (!FPT)
    return;
  const FunctionDecl *FD = CE->getDirectCallee();
  unsigned N = std::min(FPT->getNumParams(), CE->getNumArgs());
  for (unsigned I = 0; I < N; ++I) {
    const ParmVarDecl *P = FD && I < FD->getNumParams() ? FD->getParamDecl(I)
                                                        : nullptr;
    Sink K{P ? P->getType() : FPT->getParamType(I),
           P ? Types.ofDecl(P) : XjType{}};
    flow(CE->getArg(I), K);
  }
}

void FlowInstrumenter::visitAssign(const BinaryOperator *BO) {
  flow(BO->getRHS(), {BO->getLHS()->getType(), Types.of(BO->getLHS())});
}

void FlowInstrumenter::visitLocalInit(const VarDecl *VD) {
  if (isa<ParmVarDecl>(VD) || !VD->hasInit() || VD->hasGlobalStorage())
    return;
  flowInit(VD->getInit(), {VD->getType(), Types.ofDecl(VD)});
}

void FlowInstrumenter::visitReturn(const ReturnStmt *RS,
                                   const FunctionDecl *Fn) {
  if (Fn && RS->getRetValue())
    flow(RS->getRetValue(), {Fn->getReturnType(), Types.ofReturn(Fn)});
}

// `(T *)e` for guided `e`: the cast stays, and `e` is fitted to what the
// cast expects, a raw pointer of its own C type.
void FlowInstrumenter::visitExplicitCast(const CStyleCastExpr *CE) {
  const Expr *Operand = CE->getSubExpr();
  if (!CE->getType()->isPointerType() || !Types.of(Operand))
    return;
  if (!placeFlow(Operand, {CE->getType(), {}}))
    coerceFlow(Operand, {valueType(Operand), {}});
}

void FlowInstrumenter::flow(const Expr *Src, const Sink &K) {
  if (const auto *CO = dyn_cast<ConditionalOperator>(Src->IgnoreParenImpCasts())) {
    flow(CO->getTrueExpr(), K);
    flow(CO->getFalseExpr(), K);
    return;
  }
  if (!placeFlow(Src, K))
    coerceFlow(Src, K);
}

void FlowInstrumenter::flowInit(const Expr *Init, const Sink &K) {
  Init = Init->IgnoreParens();
  if (isa<ImplicitValueInitExpr, NoInitExpr, DesignatedInitUpdateExpr>(Init))
    return;
  if (const auto *ILE = dyn_cast<InitListExpr>(Init))
    flowInitList(ILE, K);
  // An array initialized from a string literal has no value to wrap.
  else if (!K.Type->isArrayType())
    flow(Init, K);
}

void FlowInstrumenter::flowInitList(const InitListExpr *ILE, const Sink &K) {
  const InitListExpr *Sem = ILE->isSemanticForm() ? ILE : ILE->getSemanticForm();
  if (!Sem)
    return;
  if (const ArrayType *AT = S.Ctx.getAsArrayType(K.Type)) {
    Sink Elem{AT->getElementType(), Types.element(K.X)};
    for (const Expr *E : Sem->inits())
      flowInit(E, Elem);
    return;
  }
  const RecordDecl *RD = K.Type->getAsRecordDecl();
  if (!RD)
    return;
  auto fieldSink = [&](const FieldDecl *FD) {
    return Sink{FD->getType(), Types.ofDecl(FD)};
  };
  if (RD->isUnion()) {
    if (const FieldDecl *FD = Sem->getInitializedFieldInUnion())
      if (Sem->getNumInits() == 1)
        flowInit(Sem->getInit(0), fieldSink(FD));
    return;
  }
  unsigned I = 0;
  for (const FieldDecl *FD : RD->fields()) {
    if (FD->isUnnamedBitField())
      continue;
    if (I >= Sem->getNumInits())
      break;
    flowInit(Sem->getInit(I++), fieldSink(FD));
  }
}

FlowInstrumenter::Place FlowInstrumenter::placeOf(const Expr *Core) const {
  if (const auto *UO = dyn_cast<UnaryOperator>(Core))
    if (UO->getOpcode() == UO_AddrOf)
      if (const auto *ASE =
              dyn_cast<ArraySubscriptExpr>(UO->getSubExpr()->IgnoreParens()))
        return {Shape::Element, ASE->getBase(), ASE->getIdx()};
  if (const auto *BO = dyn_cast<BinaryOperator>(Core)) {
    const Expr *L = BO->getLHS();
    const Expr *R = BO->getRHS();
    if (BO->getOpcode() == BO_Add && BO->getType()->isPointerType())
      return L->getType()->isPointerType() ? Place{Shape::Suffix, L, R}
                                           : Place{Shape::Suffix, R, L};
    if (BO->getOpcode() == BO_Sub && L->getType()->isPointerType() &&
        R->getType()->isIntegerType())
      return {Shape::Backward, L, R};
  }
  return {Shape::Whole, Core, nullptr};
}

FlowInstrumenter::Demand FlowInstrumenter::demandOf(const Sink &K) const {
  if (const RustType *T = K.X.rust()) {
    if (T->isSliceRef())
      return Demand::Slice;
    return T->isElementRef() ? Demand::Element : Demand::None;
  }
  return !K.X && K.Type->isPointerType() ? Demand::Raw : Demand::None;
}

Family FlowInstrumenter::familyFor(Shape Sh, Demand D) {
  if (D == Demand::Element)
    return Family::ElemRef;
  if (Sh == Shape::Whole)
    return Family::SliceAll;
  if (Sh == Shape::Element && D == Demand::Raw)
    return Family::ElemRef;
  return Family::SliceFrom;
}

bool FlowInstrumenter::placeFlow(const Expr *Src, const Sink &K) {
  Demand D = demandOf(K);
  if (D == Demand::None)
    return false;
  const Expr *Core = stripValueCasts(Src);
  Place P = placeOf(Core);
  const Expr *BaseCore = stripValueCasts(P.Base);
  QualType BaseType = BaseCore->getType();
  bool IsArray = BaseType->isConstantArrayType() ||
                 BaseType->isIncompleteArrayType();
  bool Guided = Types.isBuffer(P.Base);
  if ((!IsArray && !Guided) || (D == Demand::Raw && !Guided))
    return false;
  // The whole of a buffer already guided as the sink wants is the value
  // itself.
  if (P.Kind == Shape::Whole && Types.of(P.Base).rustText() == K.X.rustText())
    return false;
  if (P.Kind == Shape::Backward) {
    // Guided bases were reported by the normalizer.
    if (!Guided)
      S.diagnose("backward-offset", Core->getExprLoc(), "<array>",
                 "an array is offset backwards into a guided sink");
    return true;
  }
  QualType BasePointer = IsArray ? S.Ctx.getArrayDecayedType(BaseType)
                                 : valueType(P.Base).getUnqualifiedType();
  BasePointer = constIfLiteral(S.Ctx, P.Base, BasePointer);
  const MarkerKey *M =
      Markers.place(familyFor(P.Kind, D), BasePointer, K, Core->getBeginLoc());
  if (!M)
    return false;
  wrapPlace(*M, Core, P);
  return true;
}

void FlowInstrumenter::wrapPlace(const MarkerKey &M, const Expr *Core,
                                 const Place &P) {
  const Registry *R = &S.Reg;
  const EditSet *E = &S.Edits;
  const MarkerKey *Key = &M;
  if (P.Kind == Shape::Whole) {
    std::string Offset = M.F == Family::ElemRef ? ", 0" : "";
    S.Edits.addLayer(Core, [=](const std::string &Inner) {
      return R->name(*Key) + "(" + Inner + Offset + ")";
    });
    return;
  }
  const Expr *Base = P.Base;
  const Expr *Index = P.Index;
  S.Edits.addLayer(Core, [=](const std::string &) {
    return R->name(*Key) + "(" + E->render(Base) + ", " + E->render(Index) +
           ")";
  });
}

bool FlowInstrumenter::needsCoercion(XjType FX, QualType Written,
                                     const Sink &K) const {
  if (FX.rustText() == K.X.rustText())
    return retypesGuidedValue(FX, Written, K);
  // c2rust already converts between numeric types.
  bool FromScalar = FX.rust() ? FX.rust()->isScalarNumeric()
                              : !FX && Written->isArithmeticType();
  bool ToScalar = K.X.rust() ? K.X.rust()->isScalarNumeric()
                             : !K.X && K.Type->isArithmeticType();
  return !(FromScalar && ToScalar);
}

// A pointer or array with the same guidance as its sink, written as a
// different C type: C would convert it, and the guided value must not be.
bool FlowInstrumenter::retypesGuidedValue(XjType FX, QualType Written,
                                          const Sink &K) const {
  return FX.rust() && (Written->isPointerType() || Written->isArrayType()) &&
         !S.Ctx.hasSameUnqualifiedType(Written, K.Type);
}

void FlowInstrumenter::coerceFlow(const Expr *Src, const Sink &K) {
  bool IsNull = K.Type->isPointerType() && isNullConstant(Src, S.Ctx);
  XjType FX = IsNull ? XjType{} : Types.of(valueExpr(Src));
  // A null constant takes the sink's pointer type, as C gives it.
  QualType From = IsNull ? K.Type.getUnqualifiedType()
                         : constIfLiteral(S.Ctx, Src, valueType(Src));
  if (IsNull && K.X.rust() && K.X.rust()->isSingleObject())
    S.diagnose("null-into-reference", Src->getExprLoc(), K.X.rustText(),
               "a null pointer flows where a reference is guided");
  if (!needsCoercion(FX, IsNull ? From : writtenType(Src), K))
    return;
  const MarkerKey *M = Markers.coerce(From, FX, K, Src->getBeginLoc());
  if (!M) {
    S.diagnose("unhandled-coercion", Src->getExprLoc(), K.X.rustText(),
               "no marker can be written between these C types");
    return;
  }
  if (!FX && !IsNull && From->isPointerType() && K.X.rust() &&
      K.X.rust()->isSliceRef())
    S.diagnose("unhandled-coercion", Src->getExprLoc(), K.X.rustText(),
               "an unguided pointer carries no length for a slice");
  S.Edits.addLayer(Src, S.callLayer(*M));
}

bool FlowInstrumenter::isGuidedPointer(const Expr *E) const {
  return valueType(E)->isPointerType() && Types.of(E).rust();
}

void FlowInstrumenter::nullTest(const Expr *Whole, const Expr *Operand,
                                bool Negate) {
  XjType X = Types.of(Operand);
  const MarkerKey *M =
      Markers.isNull(valueType(Operand), X, Operand->getBeginLoc());
  if (!M)
    return;
  if (X.rust()->isSingleObject())
    S.diagnose("null-test-of-reference", Operand->getExprLoc(), X.rustText(),
               "a reference is never null; the test is constant");
  const Registry *R = &S.Reg;
  const EditSet *E = &S.Edits;
  const MarkerKey *Key = M;
  bool Self = Whole == Operand;
  S.Edits.addLayer(Whole, [=](const std::string &Inner) {
    std::string Call =
        R->name(*Key) + "(" + (Self ? Inner : E->render(Operand)) + ")";
    return Negate ? "(!" + Call + ")" : Call;
  });
}

void FlowInstrumenter::visitNullComparison(const BinaryOperator *BO) {
  const Expr *L = BO->getLHS();
  const Expr *R = BO->getRHS();
  const Expr *Operand = isNullConstant(R, S.Ctx)   ? L
                        : isNullConstant(L, S.Ctx) ? R
                                                   : nullptr;
  if (Operand && isGuidedPointer(Operand))
    nullTest(BO, Operand, BO->getOpcode() == BO_NE);
}

void FlowInstrumenter::visitLogicalNot(const UnaryOperator *UO) {
  if (isGuidedPointer(UO->getSubExpr()))
    nullTest(UO, UO->getSubExpr(), false);
}

void FlowInstrumenter::visitCondition(const Expr *Cond) {
  if (Cond && isGuidedPointer(Cond))
    nullTest(Cond, Cond, true);
}

} // namespace xj
