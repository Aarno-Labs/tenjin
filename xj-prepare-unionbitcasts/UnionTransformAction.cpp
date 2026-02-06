#include "UnionTransformAction.h"
#include "FunctionAccessAnalyzer.h"

UnionTransformAction::UnionTransformAction() : FA(nullptr) {}

bool UnionTransformAction::BeginSourceFileAction(CompilerInstance &CI) {
    // Reset per-file counters and log
    g_unions_found = 0;
    g_unions_replaced = 0;
    g_failed_unions.clear();
    gLog.foundUnion = false;
    gLog.replacedUnion = false;
    gLog.error = "";
    addedMemcpyDecl = false;
    return true;
}

void UnionTransformAction::EndSourceFileAction() {
    SourceManager &SM = TheRewriter.getSourceMgr();
    if (auto FE = SM.getFileEntryRefForID(SM.getMainFileID())) {
        if (VERBOSE) {
            llvm::outs() << "=== Rewritten File: " << FE->getName() << " ===\n";
        }

        std::string summary = "[SUMMARY] " + FE->getName().str() + ": ";
        summary += "# unions found: " + std::to_string(g_unions_found) +
                   ", # unions replaced: " + std::to_string(g_unions_replaced) + "\n";
        if (gLog.foundUnion) {
            summary += "union FOUND";
            if (gLog.replacedUnion) {
                summary += " and REPLACED";
            } else {
                summary += " but INVALID: " + gLog.error;
            }
        } else {
            summary += "union NOT FOUND";
        }
        summary += "\n";

        // Print all failed unions with their line numbers and errors
        for (const auto &failed : g_failed_unions) {
            summary += "[FAILED] line " + std::to_string(failed.line) + ":" +
                       std::to_string(failed.col) + " union var '" + failed.varName +
                       "': " + failed.error + "\n";
        }

        llvm::errs() << summary;
    }
    TheRewriter.getEditBuffer(SM.getMainFileID()).write(llvm::outs());
}

std::unique_ptr<ASTConsumer> UnionTransformAction::CreateASTConsumer(CompilerInstance &CI, StringRef file) {
    TheRewriter.setSourceMgr(CI.getSourceManager(), CI.getLangOpts());
    FA = std::make_unique<FunctionAccessAnalyzer>(TheRewriter);
    Finder.addMatcher(FunctionMatcher, FA.get());
    return Finder.newASTConsumer();
}
