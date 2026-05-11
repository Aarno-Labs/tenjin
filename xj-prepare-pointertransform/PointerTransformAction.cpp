#include "PointerTransformAction.h"
#include "FunctionAccessAnalyzer.h"

PointerTransformAction::PointerTransformAction() : FA(nullptr) {}

// Reset all per-file global state. The tool keeps a lot of state in
// Common.cpp globals (counters, logs, the global-pointer map), and
// without this reset they would leak between files when the tool is
// invoked over multiple sources in one ClangTool::run() call.
bool PointerTransformAction::BeginSourceFileAction(CompilerInstance &CI) {
    g_pointers_found = 0;
    g_pointers_replaced = 0;
    g_failed_pointers.clear();
    g_succeeded_pointers.clear();
    // Containers below hold raw Clang AST pointers; the prior TU's nodes are
    // freed before this callback fires for the next file, so anything not
    // cleared here becomes a use-after-free on the next translation unit.
    g_global_pointer_map.clear();
    g_transformed_functions.clear();
    g_function_analyses.clear();
    g_index_return_vars.clear();
    g_global_return_functions.clear();
    // NOTE: do NOT clear g_allowed_funcs — it's static configuration
    // (names of library functions we have wrappers for, e.g. strchr,
    // sscanf), initialized once in Common.cpp. Clearing it leaves an
    // empty set, after which `s = strchr(...)` no longer matches the
    // AssignFromAllowedFunc path, the assignment is classified Unknown,
    // and the validator rejects the candidate silently.
    g_emitted_wrappers.clear();
    g_emitted_typedefs.clear();
    gLog.foundPointer = false;
    gLog.replacedPointer = false;
    gLog.error = "";
    return true;
}

// Print the per-file [SUMMARY] / [REPLACED] / [FAILED] log lines and then
// either flush edits to disk (--inplace) or stream the rewritten main
// buffer to stdout. Called by Clang once after the analyzer has finished
// processing the translation unit.
void PointerTransformAction::EndSourceFileAction() {
    SourceManager &SM = TheRewriter.getSourceMgr();
    if (auto FE = SM.getFileEntryRefForID(SM.getMainFileID())) {
        std::string summary = "[SUMMARY] " + FE->getName().str() + ": ";
        summary += "# pointers found: " + std::to_string(g_pointers_found) +
                   ", # pointers replaced: " + std::to_string(g_pointers_replaced) + "\n";
        if (gLog.foundPointer) {
            summary += "pointer FOUND";
            if (gLog.replacedPointer) {
                summary += " and REPLACED";
            } else {
                summary += " but INVALID: " + gLog.error;
            }
        } else {
            summary += "pointer NOT FOUND";
        }
        summary += "\n";

        // Trim the file path to something readable. Test harness paths
        // typically live under .../in/ or .../out/, so prefer those as
        // the trim point; otherwise just drop the directory.
        std::string fullpath = FE->getName().str();
        std::string shortfile = fullpath;
        auto pos = shortfile.rfind("/out/");
        if (pos == std::string::npos) pos = shortfile.rfind("/in/");
        if (pos != std::string::npos) shortfile = shortfile.substr(pos + 5);
        else {
            auto slash = shortfile.rfind('/');
            if (slash != std::string::npos) shortfile = shortfile.substr(slash + 1);
        }

        for (const auto &ok : g_succeeded_pointers) {
            summary += "[REPLACED] " + shortfile + " line " + std::to_string(ok.line) + ":" +
                       std::to_string(ok.col) + " pointer var '" + ok.varName +
                       "' in " + ok.funcName + "\n";
        }

        // Pointers that never iterated, had no accesses, or hit an
        // unrecognized pattern aren't really "candidates" — they show
        // up only under --verbose so the default output stays focused
        // on real failures.
        for (const auto &failed : g_failed_pointers) {
            bool is_non_candidate =
                failed.error == "No array-like usage (no mutations or indexed assignments)" ||
                failed.error == "No accesses found" ||
                failed.error == "Unknown access pattern";
            if (is_non_candidate && !g_verbose)
                continue;
            summary += "[FAILED] " + shortfile + " line " + std::to_string(failed.line) + ":" +
                       std::to_string(failed.col) + " pointer var '" + failed.varName +
                       "': " + failed.error + "\n";
        }

        llvm::errs() << summary;
    }
    if (g_inplace) {
        TheRewriter.overwriteChangedFiles();
    } else {
        TheRewriter.getEditBuffer(SM.getMainFileID()).write(llvm::outs());
    }
}

// Wire the Rewriter to the source manager and register the analyzer as
// the callback for every function definition in the TU.
std::unique_ptr<ASTConsumer> PointerTransformAction::CreateASTConsumer(CompilerInstance &CI, StringRef file) {
    TheRewriter.setSourceMgr(CI.getSourceManager(), CI.getLangOpts());
    FA = std::make_unique<FunctionAccessAnalyzer>(TheRewriter);
    Finder.addMatcher(FunctionMatcher, FA.get());
    return Finder.newASTConsumer();
}
