#include "Normalizer.h"

using namespace clang;

namespace xj {

namespace {

class OperatorVisitor : public EvaluatedCodeVisitor<OperatorVisitor> {
public:
  explicit OperatorVisitor(Normalizer &N) : N(N) {}

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

// The guided operand of `BO` when both operands are pointers; null when
// either is not a pointer or neither is guided.
const Expr *Normalizer::guidedPointerOperand(const BinaryOperator *BO) const {
  const Expr *L = BO->getLHS();
  const Expr *R = BO->getRHS();
  if (!L->getType()->isPointerType() || !R->getType()->isPointerType())
    return nullptr;
  if (Types.isGuidedPointer(L))
    return L;
  return Types.isGuidedPointer(R) ? R : nullptr;
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

// `Deref` is the `*p` of `&*p` for guided `p`, which visitAddrOf rewrites as
// a whole.
bool Normalizer::isCancelledByAddrOf(const UnaryOperator *Deref) const {
  const auto *AddrOf =
      dyn_cast_or_null<UnaryOperator>(parentThroughParens(S.Ctx, Deref));
  return AddrOf && cancelledDeref(AddrOf) == Deref &&
         Types.of(Deref->getSubExpr());
}

// `Node` reads or writes the element of guided `Base` at `Index` (null for
// 0). A read of a string's character becomes `xj_char_at_<T>`, an element of
// a buffer held in a pointer `(*xj_index_<T>(...))`, and an element of a
// guided array a subscript, which `Node` may already be. False when `Base`
// is none of these.
bool Normalizer::normalizeElementAccess(const Expr *Node, const Expr *Base,
                                        const Expr *Index) {
  if (isGuidedString(Base) && isRead(S.Ctx, Node)) {
    addCharAtCall(Node, Base, Index);
    return true;
  }
  if (isPointerBuffer(Base)) {
    addIndexCall(Node, Base, Index);
    return true;
  }
  if (!Types.isBuffer(Base))
    return false;
  if (!isa<ArraySubscriptExpr>(Node))
    S.edit(Node, Edit::subscript(Base, Index));
  return true;
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
  if (!T || !T->isSliceRef() || T->Mut || !isStatement(S.Ctx, Node) ||
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

void Normalizer::report(llvm::StringRef Kind, const Expr *Site,
                        const Expr *Operand, llvm::StringRef Message) {
  S.diagnose(Kind, Site->getExprLoc(), describe(Operand, Types.of(Operand)),
             Message);
}

void Normalizer::visitAddrOf(const UnaryOperator *UO) {
  const UnaryOperator *Deref = cancelledDeref(UO);
  if (!Deref)
    return;
  const Expr *Operand = Deref->getSubExpr();
  if (Types.of(Operand))
    S.edit(UO, Edit::keep(Operand));
}

void Normalizer::visitDeref(const UnaryOperator *UO) {
  if (isCancelledByAddrOf(UO))
    return;
  const Expr *Operand = UO->getSubExpr();
  if (std::optional<PointerSum> Sum = asPointerSum(stripValueCasts(Operand)))
    if (normalizeElementAccess(UO, Sum->Base, Sum->Index))
      return;
  normalizeElementAccess(UO, Operand, nullptr);
}

void Normalizer::visitSubscript(const ArraySubscriptExpr *ASE) {
  const Expr *Base = ASE->getBase();
  if (normalizeElementAccess(ASE, Base, ASE->getIdx()) ||
      !Types.isSingleObject(Base))
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
  if (!Types.isGuidedPointer(Operand))
    return;
  if (UO->isIncrementOp() && advanceSlice(UO, Operand, nullptr))
    return;
  report("pointer-motion", UO, Operand,
         "a guided pointer is moved; the pointer passes left it in place");
}

void Normalizer::visitBinary(const BinaryOperator *BO) {
  if (BO->isAssignmentOp())
    normalizeInPlaceMotion(BO);
  else if (BO->getOpcode() == BO_Sub)
    checkPointerSubtraction(BO);
  else if (BO->isRelationalOp())
    checkPointerOrdering(BO);
}

// `p += n`, `p -= n` and `p = q + n` on a guided pointer `p`. Only advancing a
// shared slice by a statement of its own has a Rust counterpart; the rest is
// reported.
void Normalizer::normalizeInPlaceMotion(const BinaryOperator *BO) {
  const Expr *L = BO->getLHS();
  const Expr *R = BO->getRHS();
  if (!Types.isGuidedPointer(L))
    return;
  switch (BO->getOpcode()) {
  case BO_AddAssign:
    if (!advanceSlice(BO, L, R))
      report("pointer-motion", BO, L, "a guided pointer is advanced in place");
    break;
  case BO_SubAssign:
    report("pointer-motion", BO, L, "a guided pointer is moved back in place");
    break;
  case BO_Assign: {
    const auto *Arith = dyn_cast<BinaryOperator>(stripValueCasts(R));
    if (Arith && Arith->isAdditiveOp() && Arith->getType()->isPointerType())
      report("pointer-motion", BO, L,
             "a guided pointer is reassigned from pointer arithmetic");
    break;
  }
  default:
    break;
  }
}

// `p - q` with either pointer guided, and `b - n` on a guided buffer `b`.
void Normalizer::checkPointerSubtraction(const BinaryOperator *BO) {
  const Expr *L = BO->getLHS();
  if (BO->getRHS()->getType()->isIntegerType()) {
    if (Types.isBuffer(L))
      report("backward-offset", BO, L,
             "a guided buffer is offset backwards; no base offset is known");
  } else if (const Expr *Guided = guidedPointerOperand(BO)) {
    report("pointer-difference", BO, Guided,
           "difference of pointers into a guided buffer");
  }
}

// `p < q` and the other orderings, with either pointer guided.
void Normalizer::checkPointerOrdering(const BinaryOperator *BO) {
  if (const Expr *Guided = guidedPointerOperand(BO))
    report("pointer-ordering", BO, Guided,
           "ordering comparison of guided pointers");
}

} // namespace xj
