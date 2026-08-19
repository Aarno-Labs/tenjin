#pragma once

#include "clang/AST/Decl.h"

namespace xj::analysis
{

  // `V` is never the target of an assignment, never incremented or
  // decremented, and never has its address taken, anywhere in `FD`.
  // Address-taken counts as a possible write: an escaping local can be
  // written by anyone holding the pointer.
  bool neverReassigned(const clang::VarDecl *V, const clang::FunctionDecl &FD);

} // namespace xj::analysis
