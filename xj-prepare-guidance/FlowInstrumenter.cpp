#include "FlowInstrumenter.h"

using namespace clang;

namespace xj {

namespace {

class FlowVisitor : public EvaluatedCodeVisitor<FlowVisitor> {
  using Base = EvaluatedCodeVisitor<FlowVisitor>;

public:
  explicit FlowVisitor(FlowInstrumenter &F) : F(F) {}

  bool TraverseFunctionDecl(FunctionDecl *FD) {
    const FunctionDecl *Saved = Current;
    Current = FD;
    bool Continue = Base::TraverseFunctionDecl(FD);
    Current = Saved;
    return Continue;
  }
  // Static initializers are constant expressions and stay as written.
  bool TraverseVarDecl(VarDecl *VD) {
    return VD->hasGlobalStorage() || Base::TraverseVarDecl(VD);
  }

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
  if (placeFlow(Operand, {CE->getType(), {}}) == PlaceOutcome::NotAPlace)
    coerceFlow(Operand, {valueType(Operand), {}});
}

// A conversion of a conditional applies to each arm, so each arm flows into
// the sink under whatever implicit casts surround the whole.
void FlowInstrumenter::flow(const Expr *Src, const Sink &K) {
  if (const auto *CO = dyn_cast<ConditionalOperator>(Src->IgnoreParenImpCasts())) {
    flow(CO->getTrueExpr(), K);
    flow(CO->getFalseExpr(), K);
    return;
  }
  if (placeFlow(Src, K) == PlaceOutcome::NotAPlace)
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
    flowArrayInit(Sem, AT, K);
    return;
  }
  const RecordDecl *RD = K.Type->getAsRecordDecl();
  if (RD && RD->isUnion())
    flowUnionInit(Sem);
  else if (RD)
    flowStructInit(Sem, RD);
}

void FlowInstrumenter::flowArrayInit(const InitListExpr *Sem,
                                     const ArrayType *AT, const Sink &K) {
  Sink Elem{AT->getElementType(), Types.element(K.X)};
  for (const Expr *E : Sem->inits())
    flowInit(E, Elem);
}

void FlowInstrumenter::flowUnionInit(const InitListExpr *Sem) {
  if (const FieldDecl *FD = Sem->getInitializedFieldInUnion())
    if (Sem->getNumInits() == 1)
      flowInit(Sem->getInit(0), fieldSink(FD));
}

// The semantic form has one initializer per named field, in order.
void FlowInstrumenter::flowStructInit(const InitListExpr *Sem,
                                      const RecordDecl *RD) {
  unsigned I = 0;
  for (const FieldDecl *FD : RD->fields()) {
    if (FD->isUnnamedBitField())
      continue;
    if (I >= Sem->getNumInits())
      break;
    flowInit(Sem->getInit(I++), fieldSink(FD));
  }
}

Sink FlowInstrumenter::fieldSink(const FieldDecl *FD) const {
  return Sink{FD->getType(), Types.ofDecl(FD)};
}

// `&b[i]`, `b + i` (or `i + b`), `b - i`, or else `b` itself; null when the
// base is neither an array nor a guided buffer.
std::optional<FlowInstrumenter::Place>
FlowInstrumenter::classifyPlace(const Expr *Src) const {
  const Expr *Core = stripValueCasts(Src);
  Place P{Core, Shape::Whole, Core, nullptr};
  if (const auto *UO = dyn_cast<UnaryOperator>(Core)) {
    const auto *ASE =
        dyn_cast<ArraySubscriptExpr>(UO->getSubExpr()->IgnoreParens());
    if (UO->getOpcode() == UO_AddrOf && ASE)
      P = {Core, Shape::Element, ASE->getBase(), ASE->getIdx()};
  } else if (std::optional<PointerSum> Sum = asPointerSum(Core)) {
    P = {Core, Shape::Suffix, Sum->Base, Sum->Index};
  } else if (const auto *BO = dyn_cast<BinaryOperator>(Core)) {
    const Expr *L = BO->getLHS();
    const Expr *R = BO->getRHS();
    if (BO->getOpcode() == BO_Sub && L->getType()->isPointerType() &&
        R->getType()->isIntegerType())
      P = {Core, Shape::Backward, L, R};
  }
  QualType BaseType = stripValueCasts(P.Base)->getType();
  P.ArrayBase =
      BaseType->isConstantArrayType() || BaseType->isIncompleteArrayType();
  P.GuidedBase = Types.isBuffer(P.Base);
  if (!P.ArrayBase && !P.GuidedBase)
    return std::nullopt;
  return P;
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

FlowInstrumenter::PlaceOutcome FlowInstrumenter::placeFlow(const Expr *Src,
                                                          const Sink &K) {
  Demand D = demandOf(K);
  if (D == Demand::None)
    return PlaceOutcome::NotAPlace;
  std::optional<Place> P = classifyPlace(Src);
  // An unguided array decays into a raw pointer as in C.
  if (!P || (D == Demand::Raw && !P->GuidedBase))
    return PlaceOutcome::NotAPlace;
  // The whole of a buffer already guided as the sink wants is the value
  // itself.
  if (P->Kind == Shape::Whole &&
      Types.of(P->Base).rustText() == K.X.rustText())
    return PlaceOutcome::NotAPlace;
  if (P->Kind == Shape::Backward) {
    // Guided bases were reported by the normalizer.
    if (!P->GuidedBase)
      S.diagnose("backward-offset", P->Core->getExprLoc(), "<array>",
                 "an array is offset backwards into a guided sink");
    return PlaceOutcome::Reported;
  }
  return wrapPlace(*P, D, K) ? PlaceOutcome::Wrapped : PlaceOutcome::NotAPlace;
}

// The pointer the marker takes the place's base as.
QualType FlowInstrumenter::basePointerType(const Place &P) const {
  QualType T =
      P.ArrayBase
          ? S.Ctx.getArrayDecayedType(stripValueCasts(P.Base)->getType())
          : valueType(P.Base).getUnqualifiedType();
  return constIfLiteral(S.Ctx, P.Base, T);
}

// False when no marker can be written for the place.
bool FlowInstrumenter::wrapPlace(const Place &P, Demand D, const Sink &K) {
  const MarkerKey *M = Markers.place(familyFor(P.Kind, D), basePointerType(P),
                                     K, P.Core->getBeginLoc());
  if (!M)
    return false;
  if (P.Kind == Shape::Whole) {
    S.edit(P.Core, Edit::wrapIn(*M, M->F == Family::ElemRef ? "0" : ""));
    return true;
  }
  // The marker takes `b[i]` from `&b[i]` itself, so the normalizer's
  // rewrite of `b[i]` goes with the `&`.
  if (S.edit(P.Core, Edit::call(*M, P.Base, P.Index)) &&
      P.Kind == Shape::Element)
    S.Edits.discard(cast<UnaryOperator>(P.Core)
                        ->getSubExpr()
                        ->IgnoreParens()
                        ->getSourceRange());
  return true;
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
  if (K.Type->isPointerType() && isNullConstant(Src, S.Ctx))
    nullFlow(Src, K);
  else
    valueFlow(Src, K);
}

// A null constant carries no guidance and takes the sink's pointer type, as
// C gives it.
void FlowInstrumenter::nullFlow(const Expr *Src, const Sink &K) {
  if (K.X.rust() && K.X.rust()->isSingleObject())
    S.diagnose("null-into-reference", Src->getExprLoc(), K.X.rustText(),
               "a null pointer flows where a reference is guided");
  QualType From = K.Type.getUnqualifiedType();
  if (!needsCoercion(XjType{}, From, K))
    return;
  if (const MarkerKey *M = coercionMarker(Src, From, XjType{}, K))
    S.edit(Src, Edit::wrapIn(*M));
}

void FlowInstrumenter::valueFlow(const Expr *Src, const Sink &K) {
  XjType FX = Types.of(valueExpr(Src));
  if (!needsCoercion(FX, writtenType(Src), K))
    return;
  QualType From = constIfLiteral(S.Ctx, Src, valueType(Src));
  const MarkerKey *M = coercionMarker(Src, From, FX, K);
  if (!M)
    return;
  if (!FX && From->isPointerType() && K.X.rust() && K.X.rust()->isSliceRef())
    S.diagnose("unhandled-coercion", Src->getExprLoc(), K.X.rustText(),
               "an unguided pointer carries no length for a slice");
  S.edit(Src, Edit::wrapIn(*M));
}

// `xj_coerce_<from>_to_<to>` for `Src`; null, and reported, when none can be
// written.
const MarkerKey *FlowInstrumenter::coercionMarker(const Expr *Src,
                                                  QualType From, XjType FX,
                                                  const Sink &K) {
  const MarkerKey *M = Markers.coerce(From, FX, K, Src->getBeginLoc());
  if (!M)
    S.diagnose("unhandled-coercion", Src->getExprLoc(), K.X.rustText(),
               "no marker can be written between these C types");
  return M;
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
  Edit Test =
      Whole == Operand ? Edit::wrapIn(*M) : Edit::call(*M, Operand, nullptr);
  S.edit(Whole, std::move(Test).around(Negate ? Edit::Around::Not
                                              : Edit::Around::None));
}

void FlowInstrumenter::visitNullComparison(const BinaryOperator *BO) {
  const Expr *L = BO->getLHS();
  const Expr *R = BO->getRHS();
  const Expr *Operand = isNullConstant(R, S.Ctx)   ? L
                        : isNullConstant(L, S.Ctx) ? R
                                                   : nullptr;
  if (Operand && Types.isGuidedPointer(Operand))
    nullTest(BO, Operand, BO->getOpcode() == BO_NE);
}

void FlowInstrumenter::visitLogicalNot(const UnaryOperator *UO) {
  if (Types.isGuidedPointer(UO->getSubExpr()))
    nullTest(UO, UO->getSubExpr(), false);
}

void FlowInstrumenter::visitCondition(const Expr *Cond) {
  if (Cond && Types.isGuidedPointer(Cond))
    nullTest(Cond, Cond, true);
}

} // namespace xj
