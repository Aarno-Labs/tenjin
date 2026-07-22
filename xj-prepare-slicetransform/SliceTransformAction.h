#pragma once

#include "PtrIndexMetadata.h"

#include "clang/AST/ASTConsumer.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Rewrite/Core/Rewriter.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/Support/CommandLine.h"

using namespace clang;
using namespace clang::tooling;
using namespace llvm;

// CLI flags, published from main.cpp.
extern bool g_slice_inplace;
extern bool g_slice_verbose;
extern std::string g_slice_metadata_in;

// Metadata parsed from --metadata-in (empty when the flag is absent).
extern xj::PtrIndexMetadata g_slice_metadata;

// Per-translation-unit FrontendAction for the slice signature reshaping
// pass. Runs after xj-prepare-pointertransform; consumes the pointer/index
// metadata side-file it wrote and applies the RustSlice reshaping (see
// SliceRewriter.h).
class SliceTransformAction : public ASTFrontendAction {
  public:
    SliceTransformAction();

    bool BeginSourceFileAction(CompilerInstance &CI) override;
    void EndSourceFileAction() override;
    std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &CI, StringRef file) override;

  private:
    Rewriter TheRewriter; // Holds all source edits for this TU.
};
