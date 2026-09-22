#include "Retyper.h"

#include "clang/AST/RecursiveASTVisitor.h"
#include "llvm/ADT/StringSet.h"

using namespace clang;

namespace xj {

namespace {

class DeclCollector : public RecursiveASTVisitor<DeclCollector> {
public:
  explicit DeclCollector(Retyper &R) : R(R) {}
  bool VisitVarDecl(VarDecl *VD) {
    R.considerVar(VD);
    return true;
  }
  bool VisitFieldDecl(FieldDecl *FD) {
    R.considerField(FD);
    return true;
  }
  bool VisitFunctionDecl(FunctionDecl *FD) {
    R.considerFunction(FD);
    return true;
  }

private:
  Retyper &R;
};

bool isIdentChar(char C) { return std::isalnum(C) || C == '_'; }

// Index just past the parenthesized group starting at or after `I`.
size_t skipParenGroup(llvm::StringRef Text, size_t I) {
  while (I < Text.size() && Text[I] != '(')
    ++I;
  int Depth = 0;
  char Quote = 0;
  for (; I < Text.size(); ++I) {
    char C = Text[I];
    if (Quote) {
      if (C == '\\')
        ++I;
      else if (C == Quote)
        Quote = 0;
    } else if (C == '"' || C == '\'') {
      Quote = C;
    } else if (C == '(') {
      ++Depth;
    } else if (C == ')' && --Depth == 0) {
      return I + 1;
    }
  }
  return I;
}

} // namespace

std::string keptSpecifiers(llvm::StringRef Text) {
  static const llvm::StringSet<> Keywords = {
      "static",   "extern",        "register", "auto",
      "inline",   "__inline",      "__inline__", "_Thread_local",
      "__thread", "_Noreturn",     "__extension__"};
  static const llvm::StringSet<> WithArguments = {
      "__attribute__", "__attribute", "_Alignas", "__declspec"};
  std::string Out;
  size_t I = 0;
  while (I < Text.size()) {
    if (!isIdentChar(Text[I])) {
      ++I;
      continue;
    }
    size_t Begin = I;
    while (I < Text.size() && isIdentChar(Text[I]))
      ++I;
    llvm::StringRef Word = Text.slice(Begin, I);
    if (Keywords.contains(Word)) {
      Out += Word.str() + " ";
    } else if (WithArguments.contains(Word)) {
      I = skipParenGroup(Text, I);
      Out += Text.slice(Begin, I).str() + " ";
    }
  }
  return Out;
}

std::string recordName(const RecordDecl *RD) {
  if (RD->getIdentifier())
    return RD->getName().str();
  if (const TypedefNameDecl *TD = RD->getTypedefNameForAnonDecl())
    return TD->getName().str();
  return "";
}

void Retyper::run() {
  DeclCollector(*this).TraverseDecl(S.Ctx.getTranslationUnitDecl());
}

DeclFacts Retyper::factsOf(const DeclaratorDecl *D) const {
  DeclFacts F;
  F.Name = D->getName().str();
  if (const auto *FD = dyn_cast<FieldDecl>(D)) {
    F.IsField = true;
    F.Record = recordName(FD->getParent());
  } else if (const auto *VD = dyn_cast<VarDecl>(D)) {
    F.IsFileScope = VD->isFileVarDecl();
  }
  // Parameters of prototypes count as inside their function.
  if (const auto *Fn =
          dyn_cast_or_null<FunctionDecl>(D->getParentFunctionOrMethod()))
    F.ParentFn = Fn->getName().str();
  PresumedLoc P = S.SM.getPresumedLoc(S.SM.getFileLoc(D->getBeginLoc()));
  if (P.isValid()) {
    F.Line = P.getLine();
    F.File = P.getFilename();
  }
  return F;
}

const RustType *Retyper::chooseType(const DeclFacts &F, SourceLocation Loc) {
  const Rule<RustType> *Conflict = nullptr;
  const Rule<RustType> *Rule = S.G.matchType(F, &Conflict);
  if (!Rule)
    return nullptr;
  if (S.Results)
    S.Results->MatchedSpecs.insert(Rule->Spec.Text);
  if (Conflict)
    S.diagnose("conflicting-guidance", Loc, F.Name,
               "`" + Rule->Spec.Text + "` (" + Rule->Value.str() +
                   ") and `" + Conflict->Spec.Text + "` (" +
                   Conflict->Value.str() + ") both match; using the first");
  return &Rule->Value;
}

const RustType *Retyper::chooseType(const VarDecl *VD, const DeclFacts &F) {
  if (const RustType *Builtin = builtinVarType(VD->getName()))
    return Builtin;
  return chooseType(F, VD->getLocation());
}

void Retyper::resolveMut(const DeclFacts &F) {
  const Rule<bool> *Rule = S.G.matchMut(F);
  if (!Rule || !S.Results)
    return;
  S.Results->MatchedSpecs.insert(Rule->Spec.Text);
  std::string Key = F.ParentFn.empty() ? F.Name : F.ParentFn + ":" + F.Name;
  S.Results->MutResolved[Key] = Rule->Value;
}

void Retyper::considerVar(const VarDecl *VD) {
  if (VD->isImplicit() || !S.inMainFile(VD->getLocation()))
    return;
  DeclFacts F = factsOf(VD);
  resolveMut(F);
  if (S.DeclKeys.count(VD))
    return;
  const RustType *Ty = chooseType(VD, F);
  if (!Ty)
    return;
  if (const auto *P = dyn_cast<ParmVarDecl>(VD))
    guideParam(P, *Ty);
  else if (VD->isFileVarDecl())
    guideRedecls(VD, *Ty);
  else
    guide(VD, *Ty);
}

void Retyper::considerField(const FieldDecl *FD) {
  if (FD->isImplicit() || !S.inMainFile(FD->getLocation()) ||
      S.DeclKeys.count(FD))
    return;
  if (const RustType *Ty = chooseType(factsOf(FD), FD->getLocation()))
    guide(FD, *Ty);
}

void Retyper::considerFunction(const FunctionDecl *FD) {
  if (FD->isImplicit() || !S.inMainFile(FD->getLocation()) ||
      S.ReturnKeys.count(FD))
    return;
  const auto *Entry = S.G.returnType(FD->getName());
  if (!Entry)
    return;
  if (S.Results)
    S.Results->MatchedSpecs.insert(Entry->first);
  for (const FunctionDecl *R : FD->redecls())
    guideReturn(R, Entry->second);
}

void Retyper::guideParam(const ParmVarDecl *P, const RustType &Ty) {
  const auto *Fn = dyn_cast<FunctionDecl>(P->getDeclContext());
  if (!Fn) {
    guide(P, Ty);
    return;
  }
  unsigned Index = P->getFunctionScopeIndex();
  for (const FunctionDecl *R : Fn->redecls())
    if (Index < R->getNumParams())
      guide(R->getParamDecl(Index), Ty);
}

void Retyper::guideRedecls(const VarDecl *VD, const RustType &Ty) {
  for (const VarDecl *R : VD->redecls())
    guide(R, Ty);
}

bool Retyper::canRetype(const DeclaratorDecl *D, const RustType &Ty) {
  std::string Subject = D->getName().str() + ": " + Ty.str();
  auto Refuse = [&](llvm::StringRef Why) {
    S.diagnose("not-retyped", D->getLocation(), Subject, Why);
    return false;
  };
  if (D->isImplicit() || !D->getTypeSourceInfo() ||
      !S.inMainFile(D->getLocation()))
    return Refuse("declaration is not written in the translation unit");
  if (const auto *FD = dyn_cast<FieldDecl>(D); FD && FD->isBitField())
    return Refuse("bit-fields cannot be typedef'd");
  if (D->getType()->isVariablyModifiedType())
    return Refuse("variably-modified types cannot be typedef'd at file scope");
  return true;
}

// The type the typedef names: parameters as adjusted, and an array whose
// extent comes from its initializer as completed. clang gives the completed
// array a fresh type, which would drop an incomplete typedef's sugar.
QualType Retyper::retypedType(const DeclaratorDecl *D) const {
  QualType Written = D->getTypeSourceInfo()->getType();
  if (isa<ParmVarDecl>(D) ||
      (Written->isIncompleteArrayType() && D->getType()->isConstantArrayType()))
    return D->getType();
  return Written;
}

void Retyper::guide(const DeclaratorDecl *D, const RustType &Ty) {
  if (S.DeclKeys.count(D) || !canRetype(D, Ty))
    return;
  QualType Written = retypedType(D);
  const TypedefKey *K = keyFor(Ty, Written.getLocalUnqualifiedType());
  if (!K) {
    S.diagnose("not-retyped", D->getLocation(),
               D->getName().str() + ": " + Ty.str(),
               "type mentions a name local to a function");
    return;
  }
  if (!retype(D, *K, Written.getLocalQualifiers()))
    return;
  S.DeclKeys[D] = K;
  S.placeLocal(*K, D->getBeginLoc());
}

void Retyper::guideReturn(const FunctionDecl *FD, const RustType &Ty) {
  if (S.ReturnKeys.count(FD) || !S.inMainFile(FD->getLocation()))
    return;
  const TypedefKey *K =
      keyFor(Ty, FD->getReturnType().getLocalUnqualifiedType());
  if (!K)
    return;
  if (!retypeReturn(FD, *K))
    return;
  S.ReturnKeys[FD] = K;
  S.placeLocal(*K, FD->getBeginLoc());
}

const TypedefKey *Retyper::keyFor(const RustType &Ty, QualType T) {
  if (auto Nested = nestedKey(Ty, T))
    return &S.Reg.intern(std::move(*Nested));
  auto Sp = S.Speller.spell(T, "@NAME@");
  if (!Sp)
    return nullptr;
  TypedefKey K;
  K.Rust = Ty;
  K.Decl = std::move(*Sp);
  K.Decl.Text = "typedef " + K.Decl.Text + ";";
  K.Id = keyId(Ty.str(), K.Decl.Text);
  return &S.Reg.intern(std::move(K));
}

// `Vec<Vec<u8>>` on `char **`: the element gets a typedef of its own, so
// `x[i]` carries `Vec<u8>` by clang's own typing. Likewise the referent of
// `&Option<&u8>` on `const u8 **`, so `*p` carries `Option<&u8>`.
std::optional<TypedefKey> Retyper::nestedKey(const RustType &Ty, QualType T) {
  const RustType *Elem = Ty.bufferElement();
  if (!Elem && Ty.isSingleObject())
    Elem = &Ty.Args[0];
  if (!Elem || !Elem->isGuidedLike())
    return std::nullopt;
  QualType Inner;
  std::string Declarator;
  if (const auto *PT = T->getAs<PointerType>()) {
    Inner = PT->getPointeeType();
    Declarator = " *@NAME@";
  } else if (const ArrayType *AT = S.Ctx.getAsArrayType(T)) {
    Inner = AT->getElementType();
    const auto *CAT = dyn_cast<ConstantArrayType>(AT);
    Declarator = " @NAME@[" +
                 (CAT ? std::to_string(CAT->getSize().getZExtValue()) : "") +
                 "]";
  } else {
    return std::nullopt;
  }
  if (!Inner->isPointerType() && !Inner->isArrayType())
    return std::nullopt;
  const TypedefKey *InnerKey = keyFor(*Elem, Inner.getLocalUnqualifiedType());
  if (!InnerKey)
    return std::nullopt;
  TypedefKey K;
  K.Rust = Ty;
  K.ElementId = InnerKey->Id;
  K.Decl.Text = "typedef " + qualifierText(Inner.getLocalQualifiers()) +
                typedefRef(InnerKey->Id) + Declarator + ";";
  K.Decl.Typedefs.insert(InnerKey->Id);
  K.Id = keyId(Ty.str(), K.Decl.Text);
  return K;
}

SourceLocation Retyper::declaratorEnd(const DeclaratorDecl *D) const {
  SourceLocation TypeEnd = D->getTypeSourceInfo()->getTypeLoc().getEndLoc();
  SourceLocation Name = D->getLocation();
  if (TypeEnd.isInvalid())
    return Name;
  return S.SM.isBeforeInTranslationUnit(TypeEnd, Name) ? Name : TypeEnd;
}

// Replaces the specifiers and declarator in `R` with `Quals <typedef>After`,
// keeping storage classes and attributes. Qualifiers written in `R` belong
// to the old type and are dropped; `Quals` are the top-level ones, which C
// keeps outside a typedef.
bool Retyper::replaceSpecifiers(SourceRange R, const Rewrite &W,
                                const TypedefKey &K, const NamedDecl *D) {
  auto Offsets = S.Edits.offsets(R);
  llvm::StringRef Text =
      Offsets ? S.SM.getBufferData(S.SM.getMainFileID())
                    .slice(Offsets->first, Offsets->second)
              : "";
  // A tag defined inside the declaration would be deleted with the type.
  if (!Offsets || Text.contains('{')) {
    S.diagnose("not-retyped", D->getLocation(),
               D->getName().str() + ": " + K.Rust.str(),
               "declaration's type cannot be rewritten in place");
    return false;
  }
  std::string Before = keptSpecifiers(Text) + W.Quals;
  std::string After = W.After;
  const Registry *Reg = &S.Reg;
  const TypedefKey *Key = &K;
  return S.Edits.addLayer(R, [=](const std::string &) {
    return Before + Reg->resolve(typedefRef(Key->Id)) + After;
  });
}

bool Retyper::retype(const DeclaratorDecl *D, const TypedefKey &K,
                     Qualifiers Q) {
  std::string Name = D->getName().str();
  return replaceSpecifiers(
      SourceRange(D->getBeginLoc(), declaratorEnd(D)),
      {qualifierText(Q), Name.empty() ? "" : " " + Name}, K, D);
}

bool Retyper::retypeReturn(const FunctionDecl *FD, const TypedefKey &K) {
  SourceRange Ret = FD->getReturnTypeSourceRange();
  if (Ret.isInvalid() ||
      !S.SM.isBeforeInTranslationUnit(Ret.getEnd(), FD->getLocation())) {
    S.diagnose("not-retyped", FD->getLocation(),
               FD->getName().str() + ": " + K.Rust.str(),
               "return type is not written before the function name");
    return false;
  }
  return replaceSpecifiers(SourceRange(FD->getBeginLoc(), Ret.getEnd()),
                           {"", " "}, K, FD);
}

} // namespace xj
