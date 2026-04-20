#ifndef _STDLIB_SPEC
#define _STDLIB_SPEC
#include "clang/Tooling/Tooling.h"

enum class ErrnoBehavior {
  MaySet,
  MustNotSet,
};

// Returns false if Decl is a function that
// MUST NOT set errno
ErrnoBehavior FunctionMaySetErrno(clang::FunctionDecl *Decl); 
#endif