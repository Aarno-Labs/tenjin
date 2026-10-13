// The frontend action run on each translation unit, once per sweep.
//
// Both sweeps run the same analysis, so they intern the same keys. The
// collecting sweep only fills the registry; the rewriting sweep renders the
// edits against the final names and records diagnostics.

#pragma once

#include "Guidance.h"
#include "Registry.h"
#include "TUState.h"

#include "clang/Frontend/FrontendAction.h"
#include "clang/Tooling/Tooling.h"

namespace xj {

struct SweepConfig {
  const Guidance *G;
  Registry *Reg;
  // Null in the collecting sweep.
  RunResults *Results;
  bool Inplace;
};

class GuidanceAction : public clang::ASTFrontendAction {
public:
  explicit GuidanceAction(const SweepConfig &C) : C(C) {}

  std::unique_ptr<clang::ASTConsumer>
  CreateASTConsumer(clang::CompilerInstance &CI, llvm::StringRef) override;

private:
  SweepConfig C;
};

std::unique_ptr<clang::tooling::FrontendActionFactory>
newGuidanceActionFactory(const SweepConfig &C);

} // namespace xj
