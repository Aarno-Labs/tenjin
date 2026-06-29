#pragma once

#include "clang/Frontend/FrontendAction.h"
#include "clang/Tooling/Refactoring/AtomicChange.h"

#include <memory>

// CLI flags, published from main.cpp.
extern bool g_allocmotion_inplace;
extern bool g_allocmotion_verbose;
// Dry-run: report which allocations would be rewritten, change nothing.
extern bool g_allocmotion_report;

// Per-translation-unit FrontendAction. For each function with a body it
// runs the alloc-motion analysis (AllocMotion.{h,cpp}), accumulates a set
// of source edits as clang::tooling::AtomicChange objects, and finally
// applies them per file in EndSourceFileAction (either overwriting the
// source with --inplace or streaming the rewritten main file to stdout).
class AllocMotionAction : public clang::ASTFrontendAction {
  public:
    bool BeginSourceFileAction(clang::CompilerInstance &CI) override;
    void EndSourceFileAction() override;
    std::unique_ptr<clang::ASTConsumer>
    CreateASTConsumer(clang::CompilerInstance &CI, clang::StringRef file) override;

  private:
    clang::tooling::AtomicChanges Changes;
};
