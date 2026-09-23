#include "MarkerFactory.h"

using namespace clang;

namespace xj {

std::optional<Spelling> MarkerFactory::spellValue(QualType T, XjType X,
                                                  llvm::StringRef Declarator) const {
  std::string Space = Declarator.empty() ? "" : " ";
  if (X && X.AddrOf <= 1) {
    Spelling Sp;
    Sp.Typedefs.insert(X.Key->Id);
    Sp.Text = typedefRef(X.Key->Id);
    if (X.AddrOf)
      Sp.Text = qualifierText(T->getPointeeType().getLocalQualifiers()) +
                Sp.Text + " *";
    Sp.Text += Space + Declarator.str();
    return Sp;
  }
  return S.Speller.spell(T.getUnqualifiedType(), Declarator);
}

// A function cannot be declared returning a function pointer without a
// declarator the marker grammar does not use.
std::optional<Spelling> MarkerFactory::spellReturn(QualType T, XjType X) const {
  if (!X && T->isFunctionPointerType())
    return std::nullopt;
  return spellValue(T, X, "");
}

// The pointer a place marker returns for a raw sink: the base's element type,
// const when the sink's pointee is.
QualType MarkerFactory::rawPlaceType(QualType BasePointer,
                                     QualType SinkType) const {
  QualType Pointee = BasePointer->getPointeeType();
  const auto *SinkPtr = SinkType->getAs<PointerType>();
  if (SinkPtr && SinkPtr->getPointeeType().isConstQualified())
    Pointee.addConst();
  return S.Ctx.getPointerType(Pointee);
}

// One side of a flow in a marker name: its Rust type when guided, its C
// type otherwise.
std::string MarkerFactory::sideName(QualType T, XjType X) const {
  if (!X)
    return S.Speller.identifier(T);
  std::string Out = X.Key->Rust.identifier();
  for (unsigned I = 0; I < X.AddrOf; ++I)
    Out = "ptr_" + Out;
  return Out;
}

const MarkerKey *MarkerFactory::intern(MarkerKey K, SourceLocation Use) {
  K.Id = keyId(familyName(K.F), K.Ret.Text + "|" + K.Param.Text);
  const MarkerKey &M = S.Reg.intern(std::move(K));
  S.placeLocal(M, Use);
  return &M;
}

const MarkerKey *MarkerFactory::place(Family F, QualType BasePointer,
                                      const Sink &K, SourceLocation Use) {
  bool Guided = K.X.rust() != nullptr;
  QualType Ret = Guided ? K.Type : rawPlaceType(BasePointer, K.Type);
  auto RetSp = spellReturn(Ret, Guided ? K.X : XjType{});
  auto ParamSp = spellValue(BasePointer, {}, "b");
  if (!RetSp || !ParamSp)
    return nullptr;
  MarkerKey M;
  M.F = F;
  M.Base = "xj_" + familyName(F).str() + "_" +
           sideName(Ret, Guided ? K.X : XjType{});
  M.Body = "(" + RetSp->Text + ")" + (F == Family::SliceAll ? "b" : "(b + i)");
  M.Ret = std::move(*RetSp);
  M.Param = std::move(*ParamSp);
  M.ToRust = K.X.rustText();
  return intern(std::move(M), Use);
}

// The parameter is the base's own typedef, so the call converts nothing; a
// guided element is returned through its typedef, so the element expression
// carries it. `R name(P)` cannot write a pointer to an array or to a
// function pointer.
const MarkerKey *MarkerFactory::index(QualType BasePointer, XjType BaseX,
                                      XjType ElemX, SourceLocation Use) {
  QualType Elem = BasePointer->getPointeeType();
  if (Elem->isFunctionPointerType() || Elem->isArrayType())
    return nullptr;
  QualType Ret = S.Ctx.getPointerType(Elem);
  auto RetSp = spellValue(Ret, ElemX ? XjType{ElemX.Key, 1} : XjType{}, "");
  auto ParamSp = spellValue(BasePointer, BaseX, "b");
  if (!RetSp || !ParamSp)
    return nullptr;
  MarkerKey M;
  M.F = Family::Index;
  M.Base = "xj_index_" + sideName(BasePointer, BaseX);
  M.Body = "(" + RetSp->Text + ")(b + i)";
  M.Ret = std::move(*RetSp);
  M.Param = std::move(*ParamSp);
  M.FromRust = BaseX.rustText();
  M.ToRust = ElemX.rustText();
  return intern(std::move(M), Use);
}

const MarkerKey *MarkerFactory::coerce(QualType From, XjType FX, const Sink &K,
                                       SourceLocation Use) {
  auto RetSp = spellReturn(K.Type, K.X);
  auto ParamSp = spellValue(From, FX, "x");
  if (!RetSp || !ParamSp)
    return nullptr;
  bool Castable = K.Type->isScalarType() && From->isScalarType();
  bool Same = S.Ctx.hasSameUnqualifiedType(K.Type, From);
  if (!Castable && !Same)
    return nullptr;
  MarkerKey M;
  M.F = Family::Coerce;
  M.Base = "xj_coerce_" + sideName(From, FX) + "_to_" + sideName(K.Type, K.X);
  M.Body = Same ? "x" : "(" + RetSp->Text + ")x";
  M.Ret = std::move(*RetSp);
  M.Param = std::move(*ParamSp);
  M.FromRust = FX.rustText();
  M.ToRust = K.X.rustText();
  return intern(std::move(M), Use);
}

const MarkerKey *MarkerFactory::isNull(QualType T, XjType X,
                                       SourceLocation Use) {
  auto ParamSp = spellValue(T, X, "p");
  if (!ParamSp)
    return nullptr;
  MarkerKey M;
  M.F = Family::IsNull;
  M.Base = "xj_is_null_" + sideName(T, X);
  M.Ret.Text = "_Bool";
  M.Param = std::move(*ParamSp);
  M.Body = "p == 0";
  M.FromRust = X.rustText();
  return intern(std::move(M), Use);
}

} // namespace xj
