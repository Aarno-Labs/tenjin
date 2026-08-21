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
    g_function_analyses.clear();
    // Wrapper bodies are `static`, so each file that uses one needs its own
    // definition; carrying the set across files would suppress every
    // definition after the first and leave later files calling a name they
    // do not declare.
    g_emitted_wrappers.clear();
    // Positions are mapped through *this* TU's Rewriter, so a leftover
    // entry from an earlier file would be translated through the wrong
    // edits.
    g_pending_decl_locs.clear();
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

    stampDeclLocations();

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
                failed.error.rfind("Unknown access pattern", 0) == 0;
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

// Stamp each recorded pointer with the line:col its declaring identifier
// occupies in *this pass's output*, which is the file the base rewrite
// tool will parse. See PendingDeclLoc for why it cannot be done earlier.
//
// Offsets first, then line:col. Rewriter::getRangeSize over the char range
// [start of file, identifier) is exactly the identifier's offset in the
// rewrite buffer: the Rewriter maps the range's end past any insertion
// made at that same offset — which is what the for-init hoist emits — and
// the range's start is offset 0, which nothing can precede.
//
// The line:col conversion needs the *rewritten* text, so materialize each
// touched file once and binary-search one newline table per file rather
// than walking the rope per pointer.
void PointerTransformAction::stampDeclLocations() {
    if (g_pending_decl_locs.empty())
        return;

    SourceManager &SM = TheRewriter.getSourceMgr();

    std::map<FileID, std::vector<const PendingDeclLoc *>> byFile;
    for (const auto &P : g_pending_decl_locs)
        byFile[P.file].push_back(&P);

    for (const auto &[FID, pending] : byFile) {
        // Null means this pass made no edit to the file at all, so offsets
        // carry over unchanged and its current text is already the output.
        const llvm::RewriteBuffer *RB = TheRewriter.getRewriteBufferFor(FID);
        std::string text =
            RB ? std::string(RB->begin(), RB->end()) : SM.getBufferData(FID).str();

        std::vector<unsigned> newlines;
        for (unsigned i = 0, n = text.size(); i != n; ++i)
            if (text[i] == '\n')
                newlines.push_back(i);

        SourceLocation FileStart = SM.getLocForStartOfFile(FID);
        for (const PendingDeclLoc *P : pending) {
            auto fnIt = g_metadata.functions.find(P->function_key);
            if (fnIt == g_metadata.functions.end() ||
                P->pointer_index >= fnIt->second.pointers.size())
                continue;

            unsigned mapped = P->offset;
            if (RB) {
                int size = TheRewriter.getRangeSize(CharSourceRange::getCharRange(
                    FileStart, FileStart.getLocWithOffset(P->offset)));
                // Leave the record positionless rather than guess; the base
                // tool declines what it cannot match.
                if (size < 0)
                    continue;
                mapped = static_cast<unsigned>(size);
            }

            auto it = std::upper_bound(newlines.begin(), newlines.end(), mapped);
            unsigned lineStart = it == newlines.begin() ? 0 : *(it - 1) + 1;
            xj::PtrIndexPointerRecord &rec = fnIt->second.pointers[P->pointer_index];
            rec.decl_line = static_cast<int>(it - newlines.begin()) + 1;
            rec.decl_col = static_cast<int>(mapped - lineStart) + 1;
        }
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
