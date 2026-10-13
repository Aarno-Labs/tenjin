#include "Normalizer.h"

#include "clang/AST/ParentMapContext.h"
#include "clang/AST/RecursiveASTVisitor.h"

using namespace clang;

namespace xj {

namespace {

class OperatorVisitor : public RecursiveASTVisitor<OperatorVisitor> {
public:
  explicit OperatorVisitor(Normalizer &N) : N(N) {}

  bool TraverseUnaryExprOrTypeTraitExpr(UnaryExprOrTypeTraitExpr *) {
    return true;
  }
  bool TraverseGenericSelectionExpr(GenericSelectionExpr *) { return true; }

  bool VisitUnaryOperator(UnaryOperator *UO) {
    switch (UO->getOpcode()) {
    case UO_Deref:
      N.visitDeref(UO);
      break;
    case UO_AddrOf:
      N.visitAddrOf(UO);
      break;
    case UO_PreInc:
    case UO_PreDec:
    case UO_PostInc:
    case UO_PostDec:
      N.visitMotion(UO);
      break;
    default:
      break;
    }
    return true;
  }
  bool VisitArraySubscriptExpr(ArraySubscriptExpr *ASE) {
    N.visitSubscript(ASE);
    return true;
  }
  bool VisitBinaryOperator(BinaryOperator *BO) {
    N.visitBinary(BO);
    return true;
  }

private:
  Normalizer &N;
};

// The pointer and integer operands of `p + i` or `i + p`.
std::pair<const Expr *, const Expr *> splitOffset(const BinaryOperator *BO) {
  if (BO->getLHS()->getType()->isPointerType())
    return {BO->getLHS(), BO->getRHS()};
  return {BO->getRHS(), BO->getLHS()};
}

std::string describe(const Expr *E, XjType X) {
  std::string Name = "<expression>";
  if (const auto *DRE = dyn_cast<DeclRefExpr>(stripValueCasts(E)))
    Name = DRE->getDecl()->getName().str();
  else if (const auto *ME = dyn_cast<MemberExpr>(stripValueCasts(E)))
    Name = ME->getMemberDecl()->getName().str();
  return X ? Name + ": " + X.rustText() : Name;
}

} // namespace

void Normalizer::run() {
  OperatorVisitor(*this).TraverseDecl(S.Ctx.getTranslationUnitDecl());
}

bool Normalizer::isGuidedPointer(const Expr *E) const {
  return E->getType()->isPointerType() && Types.of(E).rust();
}

// A guided buffer held in a pointer, not an array.
bool Normalizer::isPointerBuffer(const Expr *E) const {
  return Types.isBuffer(E) && stripValueCasts(E)->getType()->isPointerType();
}

// Guided as `String` or `str`, behind references or not.
bool Normalizer::isGuidedString(const Expr *E) const {
  const RustType *T = Types.of(E).rust();
  return T && (T->stripRefs().isString() || T->stripRefs().isStr());
}

// `E` is read: C converts it from the place it names to its value.
bool Normalizer::isRead(const Expr *E) const {
  while (true) {
    auto Parents = S.Ctx.getParents(*E);
    if (Parents.size() != 1)
      return false;
    if (const auto *PE = Parents[0].get<ParenExpr>()) {
      E = PE;
      continue;
    }
    const auto *ICE = Parents[0].get<ImplicitCastExpr>();
    return ICE && ICE->getCastKind() == CK_LValueToRValue;
  }
}

// `M(Base, Index)` in place of `Node`, dereferenced when `Deref`; a null
// `Index` is 0.
void Normalizer::addMarkerCall(const Expr *Node, const MarkerKey &M,
                               const Expr *Base, const Expr *Index,
                               bool Deref) {
  S.edit(Node, Edit::call(M, Base, Index, "0")
                   .around(Deref ? Edit::Around::Deref : Edit::Around::None));
}

// `(*xj_index_<T>(Base, Index))` in place of `Node`.
void Normalizer::addIndexCall(const Expr *Node, const Expr *Base,
                              const Expr *Index) {
  XjType BaseX = Types.of(Base);
  const MarkerKey *M =
      Markers.index(stripValueCasts(Base)->getType(), BaseX,
                    Types.element(BaseX), Node->getBeginLoc());
  if (!M) {
    report("not-normalized", Node, Base,
           "no index marker can be written for this element type");
    return;
  }
  addMarkerCall(Node, *M, Base, Index, true);
}

// `xj_char_at_<T>(Base, Index)` in place of the read `Node`.
void Normalizer::addCharAtCall(const Expr *Node, const Expr *Base,
                               const Expr *Index) {
  const MarkerKey *M = Markers.charAt(stripValueCasts(Base)->getType(),
                                      Types.of(Base), Node->getBeginLoc());
  if (!M) {
    report("not-normalized", Node, Base,
           "no character marker can be written for this string");
    return;
  }
  addMarkerCall(Node, *M, Base, Index, false);
}

// A shared slice moved forward by a statement of its own is resliced:
// `p = xj_slice_from_<T>(p, n)`. A null `Step` is 1.
bool Normalizer::advanceSlice(const Expr *Node, const Expr *Operand,
                              const Expr *Step) {
  XjType X = Types.of(Operand);
  const RustType *T = X.rust();
  if (!T || !T->isSliceRef() || T->Mut || !isStatement(Node) ||
      Operand->HasSideEffects(S.Ctx))
    return false;
  const MarkerKey *M =
      Markers.place(Family::SliceFrom, Operand->getType().getUnqualifiedType(),
                    {Operand->getType(), X}, Node->getBeginLoc());
  if (!M)
    return false;
  S.edit(Node, Edit::advance(*M, Operand, Step));
  return true;
}

// `E` is evaluated for its effect alone: an expression statement, or the
// init or increment of a `for`. An initializer, a returned value or a
// condition uses its value.
bool Normalizer::isStatement(const Expr *E) const {
  auto Parents = S.Ctx.getParents(*E);
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

void Normalizer::report(llvm::StringRef Kind, const Expr *Site,
                        const Expr *Operand, llvm::StringRef Message) {
  S.diagnose(Kind, Site->getExprLoc(), describe(Operand, Types.of(Operand)),
             Message);
}

void Normalizer::visitAddrOf(const UnaryOperator *UO) {
  const auto *Deref = dyn_cast<UnaryOperator>(UO->getSubExpr()->IgnoreParens());
  if (!Deref || Deref->getOpcode() != UO_Deref)
    return;
  const Expr *Operand = Deref->getSubExpr();
  if (!Types.of(Operand))
    return;
  if (S.edit(UO, Edit::keep(Operand)))
    Consumed.insert(Deref);
}

void Normalizer::visitDeref(const UnaryOperator *UO) {
  if (Consumed.count(UO))
    return;
  const Expr *Operand = UO->getSubExpr();
  const auto *Sum = dyn_cast<BinaryOperator>(Operand->IgnoreParenImpCasts());
  if (Sum && Sum->getOpcode() == BO_Add && Sum->getType()->isPointerType()) {
    const Expr *Base = splitOffset(Sum).first;
    const Expr *Index = splitOffset(Sum).second;
    if (isGuidedString(Base) && isRead(UO)) {
      addCharAtCall(UO, Base, Index);
      return;
    }
    if (isPointerBuffer(Base)) {
      addIndexCall(UO, Base, Index);
      return;
    }
    if (Types.isBuffer(Base)) {
      S.edit(UO, Edit::subscript(Base, Index));
      return;
    }
  }
  if (isGuidedString(Operand) && isRead(UO))
    addCharAtCall(UO, Operand, nullptr);
  else if (isPointerBuffer(Operand))
    addIndexCall(UO, Operand, nullptr);
  else if (Types.isBuffer(Operand))
    S.edit(UO, Edit::subscript(Operand, nullptr));
}

void Normalizer::visitSubscript(const ArraySubscriptExpr *ASE) {
  const Expr *Base = ASE->getBase();
  if (isGuidedString(Base) && isRead(ASE)) {
    addCharAtCall(ASE, Base, ASE->getIdx());
    return;
  }
  if (isPointerBuffer(Base)) {
    addIndexCall(ASE, Base, ASE->getIdx());
    return;
  }
  if (!Types.isSingleObject(Base))
    return;
  Expr::EvalResult Index;
  bool IsZero = ASE->getIdx()->EvaluateAsInt(Index, S.Ctx) &&
                Index.Val.getInt().isZero();
  if (!IsZero) {
    report("subscript-of-single-object", ASE, Base,
           "a reference or box names one object; only index 0 has a meaning");
    return;
  }
  S.edit(ASE, Edit::deref(Base));
}

void Normalizer::visitMotion(const UnaryOperator *UO) {
  const Expr *Operand = UO->getSubExpr();
  if (!isGuidedPointer(Operand))
    return;
  if (UO->isIncrementOp() && advanceSlice(UO, Operand, nullptr))
    return;
  report("pointer-motion", UO, Operand,
         "a guided pointer is moved; the pointer passes left it in place");
}

void Normalizer::visitBinary(const BinaryOperator *BO) {
  const Expr *L = BO->getLHS();
  const Expr *R = BO->getRHS();
  bool LGuided = isGuidedPointer(L);
  bool BothPointers =
      L->getType()->isPointerType() && R->getType()->isPointerType();
  bool AnyGuided = LGuided || isGuidedPointer(R);
  switch (BO->getOpcode()) {
  case BO_AddAssign:
    if (LGuided && !advanceSlice(BO, L, R))
      report("pointer-motion", BO, L, "a guided pointer is advanced in place");
    break;
  case BO_SubAssign:
    if (LGuided)
      report("pointer-motion", BO, L, "a guided pointer is moved back in place");
    break;
  case BO_Assign: {
    const auto *Arith = dyn_cast<BinaryOperator>(R->IgnoreParenImpCasts());
    if (LGuided && Arith && Arith->isAdditiveOp() &&
        Arith->getType()->isPointerType())
      report("pointer-motion", BO, L,
             "a guided pointer is reassigned from pointer arithmetic");
    break;
  }
  case BO_Sub:
    if (BothPointers && AnyGuided)
      report("pointer-difference", BO, LGuided ? L : R,
             "difference of pointers into a guided buffer");
    else if (Types.isBuffer(L) && !BothPointers)
      report("backward-offset", BO, L,
             "a guided buffer is offset backwards; no base offset is known");
    break;
  case BO_LT:
  case BO_GT:
  case BO_LE:
  case BO_GE:
    if (BothPointers && AnyGuided)
      report("pointer-ordering", BO, LGuided ? L : R,
             "ordering comparison of guided pointers");
    break;
  default:
    break;
  }
}

} // namespace xj
