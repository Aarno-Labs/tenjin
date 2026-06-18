#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Lex/Lexer.h"
#include "clang/Tooling/Execution.h"
#include "clang/Tooling/Tooling.h"
#include "clang/Tooling/Transformer/SourceCode.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/ToolOutputFile.h"

#include <algorithm>
#include <cctype>
#include <optional>
#include <sstream>

using namespace clang;
using namespace clang::ast_matchers;
using namespace clang::tooling;
using namespace llvm;

namespace {

using StableFileLoc = std::pair<llvm::sys::fs::UniqueID, uint64_t>;

struct RefEdit {
  CharSourceRange Range;
};

class RefCollector : public RecursiveASTVisitor<RefCollector> {
public:
  RefCollector(const TagDecl *Target, SourceManager &SM, ASTContext &Ctx)
      : Target(Target->getCanonicalDecl()), SM(SM), Ctx(Ctx) {}

  bool VisitTypeLoc(TypeLoc TL) {
    const TagDecl *Referenced = nullptr;
    SourceLocation NameLoc;

    if (auto RTL = TL.getAs<RecordTypeLoc>()) {
      Referenced = RTL.getDecl();
      NameLoc = RTL.getNameLoc();
    } else if (auto ETL = TL.getAs<EnumTypeLoc>()) {
      Referenced = ETL.getDecl();
      NameLoc = ETL.getNameLoc();
    }

    if (!Referenced || Referenced->getCanonicalDecl() != Target || NameLoc.isInvalid()) {
      return true;
    }

    SourceLocation NameEnd =
        Lexer::getLocForEndOfToken(NameLoc, 0, SM, Ctx.getLangOpts());
    if (NameEnd.isInvalid()) {
      SawUnsafeRef = true;
      return true;
    }

    auto Editable = clang::tooling::getFileRangeForEdit(
        CharSourceRange::getCharRange(NameLoc, NameEnd), SM, Ctx.getLangOpts(),
        /*IncludeMacroExpansion=*/false);
    if (!Editable.has_value()) {
      SawUnsafeRef = true;
      return true;
    }
    Refs.push_back({Editable.value()});
    return true;
  }

  const TagDecl *Target;
  SourceManager &SM;
  ASTContext &Ctx;
  std::vector<RefEdit> Refs;
  bool SawUnsafeRef = false;
};

class HoistEmbeddedTagDefsCallback : public MatchFinder::MatchCallback {
public:
  HoistEmbeddedTagDefsCallback(ExecutionContext &Context,
                               llvm::ToolOutputFile &JsonOutput)
      : SM(nullptr), Ctx(nullptr), JO(JsonOutput) {}

  void run(const MatchFinder::MatchResult &Result) override {
    if (!SM) {
      SM = Result.SourceManager;
      Ctx = Result.Context;
    }

    if (auto *TD = Result.Nodes.getNodeAs<TagDecl>("tagDecl")) {
      if (TD->isThisDeclarationADefinition()) {
        AllTagDefs.push_back(TD);
        if (TD->getIdentifier()) {
          ExistingTagNames.insert(TD->getName());
        }
      }
      return;
    }

    if (auto *FD = Result.Nodes.getNodeAs<FieldDecl>("fieldDecl")) {
      collectDecl(FD);
    } else if (auto *TD = Result.Nodes.getNodeAs<TypedefDecl>("typedefDecl")) {
      collectDecl(TD);
    } else if (auto *VD = Result.Nodes.getNodeAs<VarDecl>("varDecl")) {
      if (VD->getKind() != Decl::ParmVar) {
        collectDecl(VD);
      }
    }
  }

  void collectDecl(const NamedDecl *D) {
    if (!D || D->getLocation().isInvalid()) {
      return;
    }
    SrcRangeExpStartLocToDeclMap[SM->getExpansionLoc(D->getSourceRange().getBegin())]
        .push_back(D);
  }

  void onStartOfTranslationUnit() override {
    SM = nullptr;
    Ctx = nullptr;
    AllTagDefs.clear();
    ExistingTagNames.clear();
    SrcRangeExpStartLocToDeclMap.clear();
  }

  void onEndOfTranslationUnit() override {
    if (!SM || !Ctx) {
      return;
    }

    for (auto &[Loc, Decls] : SrcRangeExpStartLocToDeclMap) {
      if (Decls.size() < 2) {
        continue;
      }

      auto FC = SM->getFileCharacteristic(SM->getExpansionLoc(Loc));
      if (FC != SrcMgr::C_User) {
        continue;
      }

      const NamedDecl *LeftmostDecl = nullptr;
      const NamedDecl *RightmostDecl = nullptr;
      for (const NamedDecl *D : Decls) {
        if (!LeftmostDecl ||
            SM->isBeforeInTranslationUnit(D->getLocation(), LeftmostDecl->getLocation())) {
          LeftmostDecl = D;
        }
        if (!RightmostDecl ||
            SM->isBeforeInTranslationUnit(RightmostDecl->getLocation(), D->getLocation())) {
          RightmostDecl = D;
        }
      }
      if (!LeftmostDecl || !RightmostDecl) {
        continue;
      }

      SourceLocation LeftmostSrcLoc = LeftmostDecl->getLocation();
      SourceLocation LeftmostNonStarLoc = LeftmostSrcLoc;
      while (true) {
        std::optional<Token> OptPrev =
            Lexer_findPreviousToken(SM->getExpansionLoc(LeftmostSrcLoc), *SM,
                                    Ctx->getLangOpts());
        if (!OptPrev.has_value()) {
          break;
        }
        Token Prev = OptPrev.value();
        if (Prev.is(tok::star) || Prev.is(tok::l_paren)) {
          LeftmostSrcLoc = Prev.getLocation();
        } else {
          LeftmostNonStarLoc = LeftmostSrcLoc;
          std::optional<Token> OptNext =
              Lexer::findNextToken(Prev.getLocation(), *SM, Ctx->getLangOpts());
          if (OptNext.has_value()) {
            LeftmostSrcLoc = OptNext.value().getLocation();
          }
          break;
        }
      }

      CharSourceRange PrefixRange = CharSourceRange::getCharRange(
          LeftmostDecl->getSourceRange().getBegin(), LeftmostNonStarLoc);
      StringRef PrefixText = Lexer::getSourceText(PrefixRange, *SM, Ctx->getLangOpts());
      if (!PrefixText.contains('{') || PrefixText.contains('#')) {
        continue;
      }

      std::optional<CharSourceRange> EditablePrefix =
          clang::tooling::getFileRangeForEdit(PrefixRange, *SM, Ctx->getLangOpts(),
                                              /*IncludeMacroExpansion=*/false);
      if (!EditablePrefix.has_value()) {
        continue;
      }

      const TagDecl *Embedded = findEmbeddedTag(EditablePrefix.value());
      if (!Embedded) {
        continue;
      }

      processCandidate(LeftmostDecl, Embedded);
    }

    SrcRangeExpStartLocToDeclMap.clear();
  }

  const TagDecl *findEmbeddedTag(CharSourceRange PrefixRange) {
    const TagDecl *Best = nullptr;
    for (const TagDecl *TD : AllTagDefs) {
      if (!TD->isThisDeclarationADefinition()) {
        continue;
      }
      if (SM->getFileCharacteristic(SM->getExpansionLoc(TD->getBeginLoc())) !=
          SrcMgr::C_User) {
        continue;
      }
      std::optional<CharSourceRange> TagRange = tagDefRange(TD);
      if (!TagRange.has_value()) {
        continue;
      }
      if (!SM->isWrittenInSameFile(PrefixRange.getBegin(), TagRange->getBegin()) ||
          !SM->isWrittenInSameFile(PrefixRange.getBegin(), TagRange->getEnd())) {
        continue;
      }
      if (SM->isBeforeInTranslationUnit(TagRange->getBegin(), PrefixRange.getBegin()) ||
          SM->isBeforeInTranslationUnit(PrefixRange.getEnd(), TagRange->getEnd())) {
        continue;
      }
      if (!Best || SM->isBeforeInTranslationUnit(TagRange->getBegin(), Best->getBeginLoc())) {
        Best = TD;
      }
    }
    return Best;
  }

  void processCandidate(const NamedDecl *ContainingDecl, const TagDecl *TD) {
    std::optional<CharSourceRange> TagRange = tagDefRange(TD);
    if (!TagRange.has_value()) {
      return;
    }

    auto Decomposed = SM->getDecomposedLoc(TagRange->getBegin());
    const FileEntry *FE = SM->getFileEntryForID(Decomposed.first);
    if (!FE) {
      return;
    }
    StableFileLoc StableBegin = std::make_pair(FE->getUniqueID(), Decomposed.second);
    if (ProcessedTagDefLocs.contains(StableBegin)) {
      return;
    }

    StringRef TagText = Lexer::getSourceText(TagRange.value(), *SM, Ctx->getLangOpts());
    if (TagText.empty() || TagText.contains('#')) {
      return;
    }

    std::optional<CharSourceRange> BodyRange = tagBodyRange(TD);
    if (!BodyRange.has_value()) {
      return;
    }
    StringRef BodyText = Lexer::getSourceText(BodyRange.value(), *SM, Ctx->getLangOpts());
    if (BodyText.empty() || BodyText.contains('#')) {
      return;
    }

    SourceLocation InsertLoc = insertionLocFor(ContainingDecl, TD);
    auto EditableInsert = clang::tooling::getFileRangeForEdit(
        CharSourceRange::getCharRange(InsertLoc, InsertLoc), *SM, Ctx->getLangOpts(),
        /*IncludeMacroExpansion=*/false);
    if (!EditableInsert.has_value()) {
      return;
    }
    if (!SM->isWrittenInSameFile(EditableInsert->getBegin(), TagRange->getBegin())) {
      return;
    }

    std::string NewName = freshNameFor(TD, TagRange->getBegin());
    std::string Kind = tagKindName(TD);
    if (Kind.empty()) {
      return;
    }

    RefCollector RefCollector(TD, *SM, *Ctx);
    RefCollector.TraverseDecl(Ctx->getTranslationUnitDecl());
    if (RefCollector.SawUnsafeRef) {
      return;
    }

    std::string HoistedPrefix = Kind + " " + NewName + " ";
    std::string HoistedText = HoistedPrefix + BodyText.str();
    applyInternalReferenceRewrites(HoistedText, HoistedPrefix.size(), BodyRange.value(),
                                   RefCollector.Refs, NewName);
    HoistedText += ";\n\n";

    std::vector<RefEdit> ExternalRefs;
    for (const RefEdit &Ref : RefCollector.Refs) {
      if (rangeContains(TagRange.value(), Ref.Range)) {
        continue;
      }
      if (!SM->isWrittenInSameFile(TagRange->getBegin(), Ref.Range.getBegin())) {
        return;
      }
      ExternalRefs.push_back(Ref);
    }

    ProcessedTagDefLocs.insert(StableBegin);
    emitCandidate(EditableInsert.value(), HoistedText, TagRange.value(),
                  Kind + " " + NewName, ExternalRefs, NewName);
  }

  void applyInternalReferenceRewrites(std::string &HoistedText,
                                      unsigned BodyTextStart,
                                      CharSourceRange BodyRange,
                                      const std::vector<RefEdit> &Refs,
                                      StringRef NewName) {
    unsigned BodyBegin = SM->getFileOffset(BodyRange.getBegin());
    std::vector<std::pair<unsigned, unsigned>> BodyRefOffsets;
    for (const RefEdit &Ref : Refs) {
      if (!rangeContains(BodyRange, Ref.Range)) {
        continue;
      }
      unsigned B = SM->getFileOffset(Ref.Range.getBegin()) - BodyBegin;
      unsigned E = SM->getFileOffset(Ref.Range.getEnd()) - BodyBegin;
      BodyRefOffsets.push_back({B, E});
    }
    std::sort(BodyRefOffsets.begin(), BodyRefOffsets.end());
    BodyRefOffsets.erase(std::unique(BodyRefOffsets.begin(), BodyRefOffsets.end()),
                         BodyRefOffsets.end());
    std::sort(BodyRefOffsets.begin(), BodyRefOffsets.end(),
              [](auto A, auto B) { return A.first > B.first; });

    for (auto [B, E] : BodyRefOffsets) {
      HoistedText.replace(BodyTextStart + B, E - B, NewName.str());
    }
  }

  bool rangeContains(CharSourceRange Outer, CharSourceRange Inner) {
    return !SM->isBeforeInTranslationUnit(Inner.getBegin(), Outer.getBegin()) &&
           !SM->isBeforeInTranslationUnit(Outer.getEnd(), Inner.getEnd());
  }

  SourceLocation insertionLocFor(const NamedDecl *ContainingDecl, const TagDecl *TD) {
    const DeclContext *DC = TD->getLexicalDeclContext();
    if (const auto *RD = dyn_cast_or_null<RecordDecl>(DC)) {
      if (RD->isThisDeclarationADefinition() && RD->getBeginLoc().isValid()) {
        return RD->getBeginLoc();
      }
    }
    return ContainingDecl->getSourceRange().getBegin();
  }

  std::optional<CharSourceRange> tagDefRange(const TagDecl *TD) {
    SourceLocation Begin = TD->getBeginLoc();
    SourceLocation End = TD->getEndLoc();
    if (Begin.isInvalid() || End.isInvalid() || Begin.isMacroID() || End.isMacroID()) {
      return std::nullopt;
    }
    SourceLocation AfterEnd = Lexer::getLocForEndOfToken(End, 0, *SM, Ctx->getLangOpts());
    if (AfterEnd.isInvalid()) {
      return std::nullopt;
    }
    auto Editable = clang::tooling::getFileRangeForEdit(
        CharSourceRange::getCharRange(Begin, AfterEnd), *SM, Ctx->getLangOpts(),
        /*IncludeMacroExpansion=*/false);
    if (!Editable.has_value()) {
      return std::nullopt;
    }
    return Editable.value();
  }

  std::optional<CharSourceRange> tagBodyRange(const TagDecl *TD) {
    SourceRange Braces;
    if (const auto *RD = dyn_cast<RecordDecl>(TD)) {
      Braces = RD->getBraceRange();
    } else if (const auto *ED = dyn_cast<EnumDecl>(TD)) {
      Braces = ED->getBraceRange();
    } else {
      return std::nullopt;
    }
    if (Braces.isInvalid() || Braces.getBegin().isMacroID() || Braces.getEnd().isMacroID()) {
      return std::nullopt;
    }
    SourceLocation AfterEnd =
        Lexer::getLocForEndOfToken(Braces.getEnd(), 0, *SM, Ctx->getLangOpts());
    if (AfterEnd.isInvalid()) {
      return std::nullopt;
    }
    auto Editable = clang::tooling::getFileRangeForEdit(
        CharSourceRange::getCharRange(Braces.getBegin(), AfterEnd), *SM,
        Ctx->getLangOpts(), /*IncludeMacroExpansion=*/false);
    if (!Editable.has_value()) {
      return std::nullopt;
    }
    return Editable.value();
  }

  std::string tagKindName(const TagDecl *TD) {
    if (isa<RecordDecl>(TD)) {
      if (TD->getTagKind() == TagTypeKind::Struct) {
        return "struct";
      }
      if (TD->getTagKind() == TagTypeKind::Union) {
        return "union";
      }
    } else if (isa<EnumDecl>(TD)) {
      return "enum";
    }
    return "";
  }

  std::string freshNameFor(const TagDecl *TD, SourceLocation Loc) {
    std::string ContextName;
    if (const auto *RD = dyn_cast_or_null<RecordDecl>(TD->getLexicalDeclContext())) {
      if (RD->getIdentifier()) {
        ContextName = sanitize(RD->getNameAsString());
      }
    }

    std::string OriginalName =
        TD->getIdentifier() ? sanitize(TD->getNameAsString()) : "";
    std::string Kind = tagKindName(TD);
    std::string Base;
    if (!ContextName.empty() && !OriginalName.empty()) {
      Base = ContextName + "_" + OriginalName;
    } else if (!OriginalName.empty()) {
      Base = "xj_" + OriginalName + "_" + stableId(Loc);
    } else {
      Base = "xj_anon_" + Kind + "_" + stableId(Loc);
    }

    std::string Candidate = Base;
    unsigned Suffix = 1;
    while (ExistingTagNames.contains(Candidate)) {
      Candidate = Base + "_xj" + std::to_string(Suffix++);
    }
    ExistingTagNames.insert(Candidate);
    return Candidate;
  }

  std::string sanitize(std::string S) {
    if (S.empty()) {
      return S;
    }
    for (char &C : S) {
      if (!std::isalnum(static_cast<unsigned char>(C)) && C != '_') {
        C = '_';
      }
    }
    if (std::isdigit(static_cast<unsigned char>(S[0]))) {
      S = "_" + S;
    }
    return S;
  }

  std::string stableId(SourceLocation Loc) {
    SourceLocation FileLoc = SM->getFileLoc(Loc);
    std::string Key = SM->getFilename(FileLoc).str() + ":" +
                      std::to_string(SM->getFileOffset(FileLoc));
    uint64_t H = 1469598103934665603ULL;
    for (unsigned char C : Key) {
      H ^= C;
      H *= 1099511628211ULL;
    }
    std::stringstream SS;
    SS << std::hex << (H & 0xffffffffULL);
    return SS.str();
  }

  void emitCandidate(CharSourceRange InsertRange, StringRef InsertText,
                     CharSourceRange ReplaceRange, StringRef ReplaceText,
                     const std::vector<RefEdit> &ExternalRefs, StringRef NewName) {
    if (FirstEditWritten) {
      JO.os() << ",";
    } else {
      FirstEditWritten = true;
    }
    JO.os() << "{\"insert\":"; emitJsonCSR(InsertRange);
    JO.os() << ",\"insert_text\":"; emitJsonString(InsertText);
    JO.os() << ",\"replace\":"; emitJsonCSR(ReplaceRange);
    JO.os() << ",\"replace_text\":"; emitJsonString(ReplaceText);
    JO.os() << ",\"refs\":[";
    bool FirstRef = true;
    for (const RefEdit &Ref : ExternalRefs) {
      if (FirstRef) {
        FirstRef = false;
      } else {
        JO.os() << ",";
      }
      JO.os() << "{\"r\":"; emitJsonCSR(Ref.Range);
      JO.os() << ",\"text\":"; emitJsonString(NewName);
      JO.os() << "}";
    }
    JO.os() << "]}";
    JO.os() << "\n";
  }

  void emitJsonString(StringRef Str) {
    JO.os() << "\"";
    for (char C : Str) {
      switch (C) {
      case '\"':
        JO.os() << "\\\"";
        break;
      case '\\':
        JO.os() << "\\\\";
        break;
      case '\n':
        JO.os() << "\\n";
        break;
      case '\r':
        JO.os() << "\\r";
        break;
      case '\t':
        JO.os() << "\\t";
        break;
      default:
        JO.os() << C;
        break;
      }
    }
    JO.os() << "\"";
  }

  void emitJsonCSR(CharSourceRange CSR) {
    if (CSR.isInvalid() || !SM->isWrittenInSameFile(CSR.getBegin(), CSR.getEnd())) {
      JO.os() << "null";
      return;
    }
    JO.os() << "{\"f\":\"" << SM->getFilename(CSR.getBegin()).str() << "\"";
    JO.os() << ",\"b\":" << SM->getFileOffset(CSR.getBegin());
    JO.os() << ",\"e\":" << SM->getFileOffset(CSR.getEnd());
    JO.os() << "}";
  }

  std::optional<Token> Lexer_findPreviousToken(SourceLocation Loc,
                                               const SourceManager &SM,
                                               const LangOptions &LangOpts,
                                               bool IncludeComments = false) {
    const auto StartOfFile = SM.getLocForStartOfFile(SM.getFileID(Loc));

    while (Loc != StartOfFile) {
      Loc = Loc.getLocWithOffset(-1);
      if (Loc.isInvalid()) {
        return std::nullopt;
      }

      Loc = Lexer::GetBeginningOfToken(Loc, SM, LangOpts);

      Token Tok;
      if (Lexer::getRawToken(Loc, Tok, SM, LangOpts)) {
        continue;
      }
      if (!Tok.is(tok::comment) || IncludeComments) {
        return Tok;
      }
    }
    return std::nullopt;
  }

  SourceManager *SM;
  ASTContext *Ctx;
  llvm::ToolOutputFile &JO;

  std::vector<const TagDecl *> AllTagDefs;
  DenseMap<SourceLocation, std::vector<const NamedDecl *>>
      SrcRangeExpStartLocToDeclMap;
  llvm::StringSet<> ExistingTagNames;
  DenseSet<StableFileLoc> ProcessedTagDefLocs;
  bool FirstEditWritten = false;
};

} // end anonymous namespace

static cl::extrahelp CommonHelp(CommonOptionsParser::HelpMessage);
static cl::OptionCategory
    HoistEmbeddedTagDefsCategory("xj-hoist-embedded-tag-defs options");

static cl::opt<std::string>
    JsonOutputPath("json-output-path", cl::desc("Path to JSON output file"),
                   cl::cat(HoistEmbeddedTagDefsCategory));

int main(int argc, const char **argv) {
  llvm::sys::PrintStackTraceOnErrorSignal(argv[0]);

  auto Executor = clang::tooling::createExecutorFromCommandLineArgs(
      argc, argv, HoistEmbeddedTagDefsCategory);

  if (!Executor) {
    llvm::errs() << llvm::toString(Executor.takeError()) << "\n";
    return 1;
  }

  std::error_code JO_EC;
  llvm::ToolOutputFile JO(JsonOutputPath, JO_EC, llvm::sys::fs::OF_Text);
  if (JO_EC) {
    llvm::errs() << "Error opening JSON output file: " << JO_EC.message() << "\n";
    return 1;
  }

  ast_matchers::MatchFinder Finder;
  HoistEmbeddedTagDefsCallback Callback(*Executor->get()->getExecutionContext(), JO);

  Finder.addMatcher(tagDecl(isDefinition()).bind("tagDecl"), &Callback);
  Finder.addMatcher(fieldDecl().bind("fieldDecl"), &Callback);
  Finder.addMatcher(typedefDecl().bind("typedefDecl"), &Callback);
  Finder.addMatcher(varDecl().bind("varDecl"), &Callback);

  JO.os() << "{\n\"edits\":[\n";
  auto Err = Executor->get()->execute(newFrontendActionFactory(&Finder));
  if (Err) {
    llvm::errs() << llvm::toString(std::move(Err)) << "\n";
  }
  JO.os() << "]\n}\n";
  JO.keep();
  return 0;
}
