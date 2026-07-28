#include "FunctionKey.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"

namespace xj
{

    // Both tools are invoked with the codebase root as their working
    // directory, so a path relative to it identifies a file the same way
    // in each — and, unlike an absolute path, does not vary with where
    // the codebase happens to be checked out.
    static std::string relativeToWorkingDir(llvm::StringRef path)
    {
        llvm::SmallString<256> p(path);
        llvm::sys::path::remove_dots(p, /*remove_dot_dot=*/true);

        llvm::SmallString<256> cwd;
        if (llvm::sys::fs::current_path(cwd))
            return std::string(p);
        if (llvm::sys::path::replace_path_prefix(p, cwd, ""))
            return llvm::StringRef(p).ltrim('/').str();
        return std::string(p);
    }

    std::string functionFilePath(const clang::FunctionDecl *FD,
                                 clang::SourceManager &SM)
    {
        if (!FD)
            return "";
        if (auto FE = SM.getFileEntryRefForID(
                SM.getFileID(SM.getSpellingLoc(FD->getLocation()))))
            return relativeToWorkingDir(FE->getName());
        return "";
    }

    std::string functionKey(const clang::FunctionDecl *FD,
                            clang::SourceManager &SM)
    {
        if (!FD)
            return "";
        std::string name = FD->getNameAsString();
        if (FD->isExternallyVisible())
            return name;
        // Key the definition's file, so a call site that only sees a
        // declaration keys the same as the defining TU did.
        const clang::FunctionDecl *Loc = FD->getDefinition();
        return name + "@" + functionFilePath(Loc ? Loc : FD, SM);
    }

} // namespace xj
