#pragma once

#include "Common.h"
#include "FunctionAccessAnalyzer.h"

class UnionTransformAction : public ASTFrontendAction {
  public:
    UnionTransformAction();

    bool BeginSourceFileAction(CompilerInstance &CI) override;
    void EndSourceFileAction() override;
    std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &CI, StringRef file) override;

  private:
    Rewriter TheRewriter;
    std::unique_ptr<FunctionAccessAnalyzer> FA;
    MatchFinder Finder;
};
