#pragma once

#include "PtrIndexMetadata.h"
#include "Reconstructor.h"

#include "clang/AST/ASTConsumer.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Rewrite/Core/Rewriter.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/Support/CommandLine.h"

#include <set>
#include <string>

using namespace clang;
using namespace clang::tooling;
using namespace llvm;

// CLI flags, published from main.cpp.
extern bool g_base_inplace;
extern bool g_base_verbose;
extern std::string g_base_metadata_in;
extern std::string g_base_metadata_out;

// The side-file, read once at startup and *mutated* as bases are proved:
// this tool is both a consumer and a producer of it.
extern xj::PtrIndexMetadata g_base_metadata;
// Function keys already reconstructed, so that a function defined in a
// header is not rewritten again when the next TU includes it.
extern std::set<std::string> g_base_handled;
extern xj::ReconstructionStats g_base_stats;

// Per-translation-unit FrontendAction: runs the must-equality analysis
// over each function with records in the side-file and substitutes the
// bases it proves. One sweep — unlike the slice pass, nothing here depends
// on a fact from another TU.
class BaseRewriteAction : public ASTFrontendAction
{
public:
  BaseRewriteAction() = default;

  void EndSourceFileAction() override;
  std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &CI,
                                                 StringRef file) override;

private:
  Rewriter TheRewriter; // Holds all source edits for this TU.
};
