// Writing C types outside the declarations they came from.
//
// A marker typedef or marker function is declared somewhere other than the
// declaration whose type it names, so the type must be written in a form
// valid there. The force-included header comes before everything, so it can
// only mention builtin types: a type written with builtins alone goes there.
// Other types are written as the program wrote them and placed in the
// translation unit, which is only possible when nothing in them is local to
// a function. Keeping typedef names matters: guidance such as `&[tflac_u8]`
// names them, and the transpiler only emits typedefs something still uses.
// (A forward declaration in the header would also work for records, but it
// moves the record's first declaration, and with it the order and placement
// of the translated items.)

#pragma once

#include "Registry.h"

#include "clang/AST/ASTContext.h"

#include <optional>

namespace xj {

class TypeSpeller {
public:
  explicit TypeSpeller(clang::ASTContext &Ctx);

  // `T` declaring `Declarator` (which may be empty), or nothing when `T`
  // cannot be written at file scope.
  std::optional<Spelling> spell(clang::QualType T,
                                llvm::StringRef Declarator) const;

  std::string print(clang::QualType T, llvm::StringRef Declarator) const;

  // `T` as an identifier fragment, written the way the program wrote it:
  // typedef and tag names are kept, `const char *` is `ptr_const_char`.
  std::string identifier(clang::QualType T) const;

private:
  clang::ASTContext &Ctx;
  clang::PrintingPolicy Policy;

  bool headerSafe(clang::QualType Canon) const;
  bool namesTypedef(clang::QualType T) const;
  bool spellableAtFileScope(clang::QualType T) const;
};

// `const`, `volatile` and `restrict` on `T` itself, as source text with a
// trailing space, or empty.
std::string qualifierText(clang::Qualifiers Q);

} // namespace xj
