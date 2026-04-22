#ifndef _STDLIB_SPEC
#define _STDLIB_SPEC
#include "clang/Tooling/Tooling.h"

enum class ErrnoBehavior
{
  MaySet,     // May set errno
  MustNotSet, // Function documentation indicates errno is not set
};

ErrnoBehavior FunctionMaySetErrno(clang::FunctionDecl *Decl);
#endif