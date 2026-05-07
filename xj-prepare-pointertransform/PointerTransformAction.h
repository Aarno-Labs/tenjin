#pragma once

#include "FunctionAccessAnalyzer.h"

// Per-translation-unit FrontendAction. Clang invokes this action once for
// each source file in the compilation database. Its job is to:
//   1. Reset per-file global state (BeginSourceFileAction)
//   2. Wire up the AST matcher + analyzer (CreateASTConsumer)
//   3. Print the per-file summary and either overwrite the source file
//      or stream the rewritten buffer to stdout (EndSourceFileAction)
class PointerTransformAction : public ASTFrontendAction {
  public:
    PointerTransformAction();

    bool BeginSourceFileAction(CompilerInstance &CI) override;
    void EndSourceFileAction() override;
    std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &CI, StringRef file) override;

  private:
    Rewriter TheRewriter;                          // Holds all source edits for this TU.
    std::unique_ptr<FunctionAccessAnalyzer> FA;    // Owns analysis + transformation logic.
    MatchFinder Finder;                            // Drives the AST matcher.
};
