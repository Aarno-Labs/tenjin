#ifndef _STDLIB_SPEC
#define _STDLIB_SPEC
#include "clang/Tooling/Tooling.h"

// Returns false if Decl is a function that
// MUST NOT set errno
bool NeedsWrapper(clang::FunctionDecl *Decl); 
#endif