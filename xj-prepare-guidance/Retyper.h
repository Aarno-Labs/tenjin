// Retyping: every guided declaration is rewritten to use a marker typedef.
//
// `const char *ostr` guided as `String` becomes `xj_ty_String ostr` with
// `typedef const char *xj_ty_String;`. The C is unchanged (a typedef is an
// alias), but the typedef survives into the transpiler's view of every
// expression that reads the declaration, so the transpiler no longer needs
// to match specifiers itself. Parameters are retyped at the same position in
// every redeclaration of their function, and file-scope variables in every
// redeclaration, so prototypes and definitions stay compatible.

#pragma once

#include "TUState.h"

#include "clang/AST/Decl.h"

#include <optional>

namespace xj {

class Retyper {
public:
  explicit Retyper(TUState &S) : S(S) {}
  void run();

  void considerVar(const clang::VarDecl *VD);
  void considerField(const clang::FieldDecl *FD);
  void considerFunction(const clang::FunctionDecl *FD);

private:
  TUState &S;

  DeclFacts factsOf(const clang::DeclaratorDecl *D) const;
  const RustType *chooseType(const clang::VarDecl *VD, const DeclFacts &F);
  const RustType *chooseType(const DeclFacts &F, clang::SourceLocation Loc);
  void resolveMut(const DeclFacts &F);

  void guideParam(const clang::ParmVarDecl *P, const RustType &Ty);
  void guideRedecls(const clang::VarDecl *VD, const RustType &Ty);
  void guide(const clang::DeclaratorDecl *D, const RustType &Ty);
  void guideReturn(const clang::FunctionDecl *FD, const RustType &Ty);
  bool canRetype(const clang::DeclaratorDecl *D, const RustType &Ty);
  clang::QualType retypedType(const clang::DeclaratorDecl *D) const;

  const TypedefKey *keyFor(const RustType &Ty, clang::QualType T);
  std::optional<TypedefKey> nestedKey(const RustType &Ty, clang::QualType T);

  struct Rewrite {
    std::string Quals;
    std::string After;
  };

  bool retype(const clang::DeclaratorDecl *D, const TypedefKey &K,
              clang::Qualifiers Q);
  bool retypeReturn(const clang::FunctionDecl *FD, const TypedefKey &K);
  clang::SourceLocation declaratorEnd(const clang::DeclaratorDecl *D) const;
  bool replaceSpecifiers(clang::SourceRange R, const Rewrite &W,
                         const TypedefKey &K, const clang::NamedDecl *D);
};

// Storage classes, function specifiers and attributes in a declaration's
// specifier text, which retyping must keep while dropping the type.
std::string keptSpecifiers(llvm::StringRef Text);

// The name the transpiler gives a record: its tag, or the typedef naming it.
std::string recordName(const clang::RecordDecl *RD);

} // namespace xj
