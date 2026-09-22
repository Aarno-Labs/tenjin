// xj-prepare-guidance: apply type guidance in the C.
//
//   xj-prepare-guidance --inplace -p <compdb dir> --guidance <xj-guidance.json>
//                       --header-out <dir> <sources...>
//
// Guided declarations are retyped to marker typedefs, pointer operators on
// guided operands are normalized, and value flows into and out of guided
// places are wrapped in marker functions. Definitions go to
// `<dir>/xj_guidance.h`, which the pipeline force-includes; the guidance file
// gains `marker_typedefs`, `markers`, `vars_mut_resolved` and
// `guidance_diagnostics`. With nothing to apply, no file is touched.

#include "GuidanceAction.h"

#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Signals.h"

using namespace clang::tooling;

namespace {

llvm::cl::OptionCategory Category("xj-prepare-guidance options");
llvm::cl::opt<bool> Inplace("inplace",
                            llvm::cl::desc("Overwrite source files in place"),
                            llvm::cl::init(false), llvm::cl::cat(Category));
llvm::cl::opt<std::string>
    GuidancePath("guidance", llvm::cl::desc("xj-guidance.json to apply"),
                 llvm::cl::Required, llvm::cl::cat(Category));
llvm::cl::opt<std::string>
    HeaderOut("header-out",
              llvm::cl::desc("Directory to write xj_guidance.h into"),
              llvm::cl::init("."), llvm::cl::cat(Category));

// One ClangTool per file, so no FileManager caches a file this tool has
// already rewritten.
int runSweep(CommonOptionsParser &Options, const xj::SweepConfig &C) {
  int RC = 0;
  auto Factory = xj::newGuidanceActionFactory(C);
  for (const std::string &Source : Options.getSourcePathList()) {
    ClangTool Tool(Options.getCompilations(), {Source});
    if (int R = Tool.run(Factory.get()))
      RC = R;
  }
  return RC;
}

void reportUnmatched(const xj::Guidance &G, xj::RunResults &Results) {
  auto Report = [&](const std::string &Spec) {
    if (!Results.MatchedSpecs.count(Spec))
      Results.Diagnostics.push_back({"unmatched-specifier", "", Spec,
                                     "matches no declaration"});
  };
  for (const auto &R : G.VarTypes)
    Report(R.Spec.Text);
  for (const auto &R : G.VarMut)
    Report(R.Spec.Text);
  for (const auto &[Fn, _] : G.FnReturnTypes)
    Report(Fn);
}

bool writeFile(llvm::StringRef Path, llvm::StringRef Text) {
  llvm::Error E = llvm::writeToOutput(Path, [&](llvm::raw_ostream &OS) {
    OS << Text;
    return llvm::Error::success();
  });
  if (!E)
    return true;
  llvm::errs() << "xj-prepare-guidance: cannot write " << Path << ": "
               << llvm::toString(std::move(E)) << "\n";
  return false;
}

bool writeGuidance(const xj::Guidance &G, const xj::Registry &Reg,
                   const xj::RunResults &Results) {
  llvm::json::Object Out = G.Raw;
  Out["marker_typedefs"] = Reg.markerTypedefsJson();
  Out["markers"] = Reg.markersJson();
  llvm::json::Object Mut;
  for (const auto &[Key, IsMut] : Results.MutResolved)
    Mut[Key] = IsMut;
  Out["vars_mut_resolved"] = std::move(Mut);
  Out["guidance_diagnostics"] = Results.diagnosticsJson();
  return writeFile(GuidancePath,
                   llvm::formatv("{0:2}", llvm::json::Value(std::move(Out)))
                       .str());
}

void printDiagnostics(const xj::RunResults &Results) {
  for (const xj::Diagnostic &D : Results.Diagnostics)
    llvm::errs() << "xj-prepare-guidance: " << D.Kind << ": "
                 << (D.Site.empty() ? "" : D.Site + ": ") << D.Subject << ": "
                 << D.Message << "\n";
}

} // namespace

int main(int argc, const char **argv) {
  llvm::sys::PrintStackTraceOnErrorSignal(argv[0]);
  auto Parser = CommonOptionsParser::create(argc, argv, Category);
  if (!Parser) {
    llvm::errs() << Parser.takeError();
    return 1;
  }
  auto G = xj::Guidance::load(GuidancePath);
  if (!G) {
    llvm::errs() << "xj-prepare-guidance: " << llvm::toString(G.takeError())
                 << "\n";
    return 1;
  }
  if (G->alreadyInstrumented()) {
    llvm::errs() << "xj-prepare-guidance: guidance is already applied\n";
    return 0;
  }

  xj::Registry Reg;
  int RC = runSweep(*Parser, {&*G, &Reg, nullptr, Inplace});
  Reg.finalize();
  xj::RunResults Results;
  RC |= runSweep(*Parser, {&*G, &Reg, &Results, Inplace});
  reportUnmatched(*G, Results);
  printDiagnostics(Results);

  bool Applied = !Reg.empty() || !Results.MutResolved.empty();
  if (!Applied)
    return RC;
  llvm::SmallString<256> Header(HeaderOut);
  llvm::sys::path::append(Header, "xj_guidance.h");
  if (!Reg.empty() && !writeFile(Header, Reg.headerText()))
    RC = 1;
  if (!writeGuidance(*G, Reg, Results))
    RC = 1;
  return RC;
}
