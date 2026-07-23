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
extern std::string g_slice_metadata_out;

// Metadata parsed from --metadata-in (empty when the flag is absent),
// then enriched in place by the detection sweep (SliceDetector fills in
// the per-function slice records and global-return entries) before the
// rewrite sweep consumes it.
extern xj::PtrIndexMetadata g_slice_metadata;

// Read-only per-TU FrontendAction for the detection sweep: runs
// SliceDetector over the index-transformed TU and folds the detected
// RustSlice candidates into g_slice_metadata. Makes no edits.
class SliceDetectAction : public ASTFrontendAction {
  public:
    SliceDetectAction() = default;

    std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &CI,
                                                   StringRef file) override;
};

// Per-translation-unit FrontendAction for the slice signature reshaping
// pass. Runs after the detection sweep and applies the RustSlice
// reshaping recorded in g_slice_metadata (see SliceRewriter.h).
class SliceTransformAction : public ASTFrontendAction {
  public:
    SliceTransformAction();

    bool BeginSourceFileAction(CompilerInstance &CI) override;
    void EndSourceFileAction() override;
    std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &CI, StringRef file) override;

  private:
    Rewriter TheRewriter; // Holds all source edits for this TU.
};
