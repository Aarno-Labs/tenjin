#pragma once

#include "clang/Tooling/Refactoring/AtomicChange.h"

namespace clang {
class ASTContext;
}

// Analyze every function in the translation unit and append the source
// edits needed to turn "heap-allocate then field-by-field initialize"
// patterns into "stack-initialize then box" form. Edits are emitted as
// AtomicChange objects appended to `Changes`; nothing is written here.
//
// Conservative by construction: a candidate is only rewritten when the
// analysis can prove the struct is fully initialized before its first use
// (see AllocMotion.cpp for the exact preconditions). Anything else is left
// untouched.
void runAllocMotion(clang::ASTContext &Ctx, clang::tooling::AtomicChanges &Changes);
