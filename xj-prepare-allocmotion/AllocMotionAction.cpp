#include "AllocMotionAction.h"
#include "AllocMotion.h"

#include "clang/AST/ASTConsumer.h"
#include "clang/AST/ASTContext.h"
#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Frontend/CompilerInstance.h"

#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"

#include <map>
#include <string>

using namespace clang;
using namespace clang::tooling;

bool g_allocmotion_inplace = false;
bool g_allocmotion_verbose = false;
bool g_allocmotion_report = false;

namespace {

// Drives the per-TU analysis. The actual edits are produced by
// runAllocMotion(); this consumer just hands it the ASTContext and a place
// to collect AtomicChanges.
class AllocMotionConsumer : public ASTConsumer {
  public:
    explicit AllocMotionConsumer(AtomicChanges &Changes) : Changes(Changes) {}

    void HandleTranslationUnit(ASTContext &Ctx) override {
        // Never transform a translation unit that did not parse cleanly: an
        // error-recovery AST (e.g. missing system headers because no
        // -resource-dir was supplied, so NULL/size_t/malloc are unresolved)
        // can match a candidate partially and yield a half-rewritten, broken
        // file. Bail and leave the source untouched.
        if (Ctx.getDiagnostics().hasErrorOccurred()) {
            llvm::errs() << "xj-prepare-allocmotion: translation unit has "
                            "compile errors; leaving it untouched\n";
            return;
        }
        runAllocMotion(Ctx, Changes);
    }

  private:
    AtomicChanges &Changes;
};

} // namespace

bool AllocMotionAction::BeginSourceFileAction(CompilerInstance &) {
    Changes.clear();
    return true;
}

std::unique_ptr<ASTConsumer>
AllocMotionAction::CreateASTConsumer(CompilerInstance &, StringRef) {
    return std::make_unique<AllocMotionConsumer>(Changes);
}

// Group the collected edits by file and apply them with
// tooling::applyAtomicChanges. With --inplace each changed file is
// overwritten; otherwise the rewritten main file is streamed to stdout
// (mirroring the other xj-prepare-* tools).
void AllocMotionAction::EndSourceFileAction() {
    // In report (dry-run) mode the analysis has already printed its findings
    // and produced no edits; do not touch files or echo the source.
    if (g_allocmotion_report)
        return;

    SourceManager &SM = getCompilerInstance().getSourceManager();
    const FileID MainID = SM.getMainFileID();
    std::string MainPath;
    if (auto FE = SM.getFileEntryRefForID(MainID))
        MainPath = FE->getName().str();

    // Bucket changes by absolute file path.
    std::map<std::string, AtomicChanges> ByFile;
    for (const auto &C : Changes)
        ByFile[C.getFilePath()].push_back(C);

    ApplyChangesSpec Spec;
    Spec.Cleanup = false;

    bool WroteStdout = false;
    for (auto &Entry : ByFile) {
        const std::string &File = Entry.first;
        llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> Buf =
            llvm::MemoryBuffer::getFile(File);
        if (!Buf) {
            llvm::errs() << "xj-prepare-allocmotion: cannot read " << File << ": "
                         << Buf.getError().message() << "\n";
            continue;
        }

        llvm::Expected<std::string> Result =
            applyAtomicChanges(File, (*Buf)->getBuffer(), Entry.second, Spec);
        if (!Result) {
            llvm::errs() << "xj-prepare-allocmotion: failed to apply changes to "
                         << File << ": " << toString(Result.takeError()) << "\n";
            continue;
        }

        if (g_allocmotion_inplace) {
            std::error_code EC;
            llvm::raw_fd_ostream Out(File, EC);
            if (EC) {
                llvm::errs() << "xj-prepare-allocmotion: cannot write " << File << ": "
                             << EC.message() << "\n";
                continue;
            }
            Out << *Result;
        } else if (File == MainPath) {
            llvm::outs() << *Result;
            WroteStdout = true;
        }
    }

    // No edits in the main file: still emit it unchanged so the default
    // (stdout) mode produces a complete translation unit.
    if (!g_allocmotion_inplace && !WroteStdout) {
        if (auto MainBuf = SM.getBufferOrNone(MainID))
            llvm::outs() << MainBuf->getBuffer();
    }
}
