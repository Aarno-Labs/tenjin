#include "clang/AST/ASTConsumer.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Tooling.h"
#include "clang/Index/USRGeneration.h"
#include "clang/Tooling/Transformer/SourceCode.h"
#include "clang/Tooling/Transformer/SourceCodeBuilders.h"
#include "clang/Tooling/Transformer/Stencil.h"
#include "clang/Tooling/Transformer/Transformer.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Signals.h"

#include "FindExternal.h"
#include "LocalizeErrnoASTVisitor.h"

#include <iostream>
#include <fstream>
#include <stdio.h>
#include <filesystem>

using namespace clang;
using namespace clang::tooling;
using namespace llvm;

// Apply a custom category to all command-line options so that they are the
// only ones displayed.
static cl::OptionCategory LocalizeCategory("xj-localize-errno options");
cl::opt<bool> InPlace("i", cl::desc("Run localization in-place"), cl::cat(LocalizeCategory), cl::init(false));

int main(int argc, const char **argv)
{
  llvm::sys::PrintStackTraceOnErrorSignal(argv[0]);

  auto ExpectedParser = CommonOptionsParser::create(argc, argv, LocalizeCategory);
  if (!ExpectedParser)
  {
    // Fail gracefully for unsupported options.
    llvm::errs() << ExpectedParser.takeError();
    return 1;
  }

  CommonOptionsParser &OptionsParser = ExpectedParser.get();
  ClangTool Tool(OptionsParser.getCompilations(),
                 OptionsParser.getSourcePathList());

  // Collect all decls we will consider to be external;
  FindExternalFunctionActionFactory Action;
  Tool.run(&Action);

  auto ExternalUSRs = Action.GetExternalDecls();

  LocalizeErrnoActionFactory LocalizeAction(ExternalUSRs);
  Tool.run(&LocalizeAction);

  tooling::ApplyChangesSpec Spec;
  std::map<std::string, AtomicChanges> file_changes;
  for (const auto &Change : LocalizeAction.Changes)
  {
    auto changes = file_changes.find(Change.getFilePath());
    if (changes == file_changes.end())
    {
      file_changes.insert(std::pair<std::string, AtomicChanges>(Change.getFilePath(), {Change}));
    }
    else
    {
      changes->second.push_back(Change);
    }
  }

  std::vector<std::pair<std::string, std::string>> ToWrite;
  bool Abort = false;
  for (const auto &InFile : Tool.getSourcePaths())
  {
    auto File = std::filesystem::absolute(InFile).string();
    llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> BufferErr = llvm::MemoryBuffer::getFile(File);

    if (!BufferErr)
    {
      llvm::errs() << BufferErr.getError().message();
      Abort = true;
      continue;
    }

    std::string result;
    auto changes = file_changes.find(File);
    if (changes == file_changes.end())
    {
      result = (*BufferErr)->getBuffer();
    }
    else
    {
      auto applyResult = tooling::applyAtomicChanges(File, BufferErr.get()->getBuffer(), changes->second, Spec);
      if (!applyResult)
      {
        llvm::errs() << toString(applyResult.takeError());
        Abort = true;
        continue;
      }
      result = applyResult.get();
    }
    std::ofstream NewFile;
    ToWrite.push_back(std::pair(InPlace ? File : (File + ".errno"), result));
  }

  if (Abort) 
  {
    llvm::errs() << "Errors found when applying changes, not writing changes.\n";
    return 1;
  }

  for (auto &W : ToWrite)
  {
    std::ofstream NewFile;
    NewFile.open(W.first);
    NewFile << W.second;
    NewFile.close();
  }

  return 0;
}
