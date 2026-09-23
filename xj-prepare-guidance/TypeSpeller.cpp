#include "TypeSpeller.h"

#include "llvm/Support/raw_ostream.h"

using namespace clang;

namespace xj {

static bool isFunctionLocal(const Decl *D) {
  return D->getParentFunctionOrMethod() != nullptr;
}

TypeSpeller::TypeSpeller(ASTContext &Ctx)
    : Ctx(Ctx), Policy(Ctx.getPrintingPolicy()) {
  Policy.SuppressTagKeyword = false;
  Policy.AnonymousTagLocations = false;
  Policy.PolishForDeclaration = true;
}

std::string TypeSpeller::print(QualType T, llvm::StringRef Declarator) const {
  std::string Out;
  llvm::raw_string_ostream OS(Out);
  T.print(OS, Policy, Declarator);
  return Out;
}

std::optional<Spelling> TypeSpeller::spell(QualType T,
                                           llvm::StringRef Declarator) const {
  Spelling S;
  QualType Canon = T.getCanonicalType();
  if (!namesTypedef(T) && headerSafe(Canon)) {
    S.Text = print(Canon, Declarator);
    return S;
  }
  if (!spellableAtFileScope(T))
    return std::nullopt;
  S.HeaderSafe = false;
  S.Text = print(T, Declarator);
  return S;
}

std::string TypeSpeller::identifier(QualType T) const {
  const Type *Ty = T.getTypePtr();
  if (const auto *ET = dyn_cast<ElaboratedType>(Ty))
    return identifier(ET->getNamedType());
  if (const auto *PT = dyn_cast<ParenType>(Ty))
    return identifier(PT->getInnerType());
  if (const auto *AT = dyn_cast<AttributedType>(Ty))
    return identifier(AT->getModifiedType());
  if (const auto *TT = dyn_cast<TypedefType>(Ty))
    return identifierFragment(TT->getDecl()->getName());
  if (const auto *PT = dyn_cast<PointerType>(Ty)) {
    QualType Pointee = PT->getPointeeType();
    return (Pointee.isConstQualified() ? "ptr_const_" : "ptr_") +
           identifier(Pointee);
  }
  if (const auto *CAT = dyn_cast<ConstantArrayType>(Ty))
    return "array_" + std::to_string(CAT->getZExtSize()) + "_" +
           identifier(CAT->getElementType());
  if (const auto *AT = dyn_cast<ArrayType>(Ty))
    return "array_" + identifier(AT->getElementType());
  if (const auto *TT = dyn_cast<TagType>(Ty)) {
    const TagDecl *D = TT->getDecl();
    return D->getIdentifier() ? identifierFragment(D->getName()) : "anon";
  }
  if (isa<FunctionType>(Ty))
    return "fn";
  if (const auto *BT = dyn_cast<BuiltinType>(Ty))
    return identifierFragment(BT->getName(Policy));
  return identifierFragment(print(T.getUnqualifiedType(), ""));
}

bool TypeSpeller::headerSafe(QualType Canon) const {
  const Type *Ty = Canon.getTypePtr();
  if (isa<BuiltinType>(Ty))
    return true;
  if (const auto *PT = dyn_cast<PointerType>(Ty))
    return headerSafe(PT->getPointeeType());
  if (isa<ConstantArrayType, IncompleteArrayType>(Ty))
    return headerSafe(cast<ArrayType>(Ty)->getElementType());
  if (const auto *FT = dyn_cast<FunctionType>(Ty)) {
    if (!headerSafe(FT->getReturnType()))
      return false;
    if (const auto *FPT = dyn_cast<FunctionProtoType>(FT))
      for (QualType P : FPT->getParamTypes())
        if (!headerSafe(P))
          return false;
    return true;
  }
  return false;
}

// `T` is written with a typedef name, which the canonical type would lose.
bool TypeSpeller::namesTypedef(QualType T) const {
  const Type *Ty = T.getTypePtr();
  if (isa<TypedefType>(Ty))
    return true;
  if (const auto *ET = dyn_cast<ElaboratedType>(Ty))
    return namesTypedef(ET->getNamedType());
  if (const auto *PT = dyn_cast<ParenType>(Ty))
    return namesTypedef(PT->getInnerType());
  if (const auto *AT = dyn_cast<AttributedType>(Ty))
    return namesTypedef(AT->getModifiedType());
  if (const auto *PT = dyn_cast<PointerType>(Ty))
    return namesTypedef(PT->getPointeeType());
  if (const auto *AT = dyn_cast<ArrayType>(Ty))
    return namesTypedef(AT->getElementType());
  if (const auto *FT = dyn_cast<FunctionProtoType>(Ty)) {
    for (QualType P : FT->getParamTypes())
      if (namesTypedef(P))
        return true;
    return namesTypedef(FT->getReturnType());
  }
  return false;
}

// True when every name `T` is written with is visible at file scope.
bool TypeSpeller::spellableAtFileScope(QualType T) const {
  const Type *Ty = T.getTypePtr();
  if (const auto *TT = dyn_cast<TypedefType>(Ty))
    return !isFunctionLocal(TT->getDecl());
  if (const auto *ET = dyn_cast<ElaboratedType>(Ty))
    return spellableAtFileScope(ET->getNamedType());
  if (const auto *TT = dyn_cast<TagType>(Ty))
    return TT->getDecl()->getIdentifier() && !isFunctionLocal(TT->getDecl());
  if (const auto *PT = dyn_cast<ParenType>(Ty))
    return spellableAtFileScope(PT->getInnerType());
  if (const auto *AT = dyn_cast<AttributedType>(Ty))
    return spellableAtFileScope(AT->getModifiedType());
  if (const auto *PT = dyn_cast<PointerType>(Ty))
    return spellableAtFileScope(PT->getPointeeType());
  if (isa<VariableArrayType>(Ty))
    return false;
  if (const auto *AT = dyn_cast<ArrayType>(Ty))
    return spellableAtFileScope(AT->getElementType());
  if (const auto *FT = dyn_cast<FunctionProtoType>(Ty)) {
    if (!spellableAtFileScope(FT->getReturnType()))
      return false;
    for (QualType P : FT->getParamTypes())
      if (!spellableAtFileScope(P))
        return false;
    return true;
  }
  return isa<BuiltinType>(Ty);
}

std::string qualifierText(Qualifiers Q) {
  std::string Out;
  if (Q.hasConst())
    Out += "const ";
  if (Q.hasVolatile())
    Out += "volatile ";
  if (Q.hasRestrict())
    Out += "restrict ";
  return Out;
}

} // namespace xj
