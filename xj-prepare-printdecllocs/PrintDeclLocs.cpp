#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Lex/Lexer.h"
#include "clang/Tooling/Execution.h"
#include "clang/Tooling/Tooling.h"
#include "clang/Tooling/Transformer/SourceCode.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/ToolOutputFile.h"

using namespace clang;
using namespace clang::ast_matchers;
using namespace clang::tooling;
using namespace llvm;

/*
Some examples of Clang's source locations:
(pointers (^) are "after" the location indicated)

col
0        1         2         3         4         5         6         7         8
1234567890123456789012345678901234567890

int bvar[2], *cvar, *dvar[2];
^   ^      ^   
|   |      +- typeloc end   (col 11), also getEndLoc() of getSourceRange()
|   +-------- getLocation() (col 5) for `bvar`
+------------ typeloc begin       , also getBeginLoc() of getSourceRange()


int bvar[2], *cvar, *dvar[2];
^            ^^
|            |+-- getLocation() (col 5) for `cvar`
|            | &  getEndLoc() of getSourceRange()  (col )
|            |
|            +- typeloc end   (col 14)
+-------------- typeloc begin, also getBeginLoc() of getSourceRange()


                               +-- TL[0]<ArrayTypeLoc> getBracketsRange().getStart()
                               v
const int bvar[2], *cvar, *dvar[2];
^     ^                   ^      ^
|   ++ TL[0] begin        |      +- TL[0] end
|    & TL[2] end          +-- TL[1] end
|
+    

==============================================================

#define INTVAR(q) int q, yyyy
                  ^      ^
                  |      +--- getSpellingLoc(getLocation()) for `yyyy`
                  +---------- typeloc end for `yyyy` 
signed INTVAR(xxxx), *zzzz;
^      ^
|      +---- getLocation() for `yyyy`
+---- typeloc begin for `yyyy`, also getBeginLoc() of getSourceRange()

* The get_tokens() function in the Python API returns
    [signed,INTVAR,(,x,)]
  for the `yyyy` decl!


VarDecl::getPreviousDecl() does not return adjacent decls of different vars,
it only returns previous decls of the same var.

Lexer::getSourceText() is ""                           for xxxx
Lexer::getSourceText() is "signed INTVAR(xxxx)"        for yyyy
Lexer::getSourceText() is "signed INTVAR(xxxx), *zzzz" for zzzz

==============================================================

                 +--- typeloc begin & typeloc end
                 |
                 v
#define MULTI(x) int x ## 1; int e2 , x ## 3;
MULTI(e)                         
^                                
|                                
+----- getLocation() for `e1`

? <--- SM->getSpellingLoc(D->getLocation()) [<scratch space>:2:1]

* The get_tokens() function in the Python API returns
    [int,x,##,...,##,3,;,MULTI,(,e,)]
  for the `e1` decl!




                 +--- typeloc begin & typeloc end
                 |
                 v
#define MULTI(x) int x ## 1; int e2 , x ## 3;
MULTI(e)                         ^
^                                +--- getLocation() for `e2` (offset 16)
|                                +--- getSpellingLoc(getLocation()) (offset 243)
+----- getFileLoc(getLocation()) for `e2`
     = getExpansionLoc(getLocation()) for `e2`

Lexer::getSourceText() returns empty string for all 3 decls.
*/


namespace {

using StableFileLoc = std::pair<llvm::sys::fs::UniqueID, uint64_t>;

class PrintDeclLocsCallback : public MatchFinder::MatchCallback {
public:
  PrintDeclLocsCallback(ExecutionContext &Context,
                        llvm::ToolOutputFile &JsonOutput)
      : /*Context(Context),*/ SM(nullptr), Ctx(nullptr), JO(JsonOutput) {}

  void run(const MatchFinder::MatchResult &Result) override {
    if (!SM) {
      SM = Result.SourceManager;
      Ctx = Result.Context;
    }

    if (auto *FD = Result.Nodes.getNodeAs<FieldDecl>("fieldDecl")) {
      processDeclLocations("FieldDecl", FD);
    } else if (auto *TD = Result.Nodes.getNodeAs<TypedefDecl>("typedefDecl")) {
      processDeclLocations("TypedefDecl", TD);
    } else if (auto *VD = Result.Nodes.getNodeAs<VarDecl>("varDecl")) {
      if (VD->getKind() == Decl::ParmVar) {
          return;
      }
      processDeclLocations("VarDecl", VD);
    } else if (auto *FnD = Result.Nodes.getNodeAs<FunctionDecl>("functionDecl")) {
      processDeclLocations("FunctionDecl", FnD);
    }
  }

  void processDeclLocations(const char* declType, const NamedDecl* D) {
    if (!D || !D->getLocation().isValid()) {
      return;
    }

    SrcRangeExpStartLocToDeclMap[SM->getExpansionLoc(D->getSourceRange().getBegin())].push_back(D);
  }

  void onStartOfTranslationUnit() override {
    SM = nullptr;
    Ctx = nullptr;
  }

  void onEndOfTranslationUnit() override {
    StringMap<std::vector<SourceLocation>> failedDecls;
    StringMap<std::vector<SourceLocation>> curiousDecls;

    for (auto &[Loc, Decls] : SrcRangeExpStartLocToDeclMap) {
      NumDeclLocsVisitedOrRevisited++;
      if (Decls.size() < 2) {
        continue;
      }

      auto FC = SM->getFileCharacteristic(SM->getExpansionLoc(Loc));
      if (FC != SrcMgr::C_User) {
        // silently skip decls from system headers
        continue;
      }

      const NamedDecl* leftmostDecl = nullptr;
      const NamedDecl* rightmostDecl = nullptr;

      for (const NamedDecl* D : Decls) {
        if (!leftmostDecl || SM->isBeforeInTranslationUnit(D->getLocation(), leftmostDecl->getLocation())) {
          leftmostDecl = D;
        }
        if (!rightmostDecl || SM->isBeforeInTranslationUnit(rightmostDecl->getLocation(), D->getLocation())) {
          rightmostDecl = D;
        }
      }

      // We established earlier that this declaration was ultimately expanded in
      // a user file, which we consider to be editable. But we don't yet know
      // whether the spelling of the declaration is editable.
      bool decl_editable = true;
      if (SM->getFileCharacteristic(SM->getSpellingLoc(
                        leftmostDecl->getSourceRange().getBegin()))
            != SrcMgr::C_User) {
        // This will trigger on code that uses a type defined via macro in a system header,
        // usually `bool`. To distinguish the case of decls defined in system headers vs
        // decls which are merely using such types, we inspect the spelling locations of
        // each decl name location. If any are either in a scratch space or a non-user file,
        // we'll silently skip the entire set of decls at this location.
        for (const NamedDecl* D : Decls) {
          SourceLocation nameSpellLoc = SM->getSpellingLoc(D->getLocation());
          if (SM->isWrittenInScratchSpace(nameSpellLoc)
              || SM->getFileCharacteristic(nameSpellLoc) != SrcMgr::C_User) {
            decl_editable = false;
            break;
          }
        }
      }

      if (!decl_editable) {
        llvm::outs() << "  skipping non-editable decls at loc: "
                     << Loc.printToString(*SM) << "\n";
        continue;
      }

      SourceLocation leftmostSrcLoc = leftmostDecl->getLocation();
      SourceLocation declEndLoc = rightmostDecl->getSourceRange().getEnd();
      // We throw in getExpansionLoc here in case the rightmost declaration
      // has an initializer that ends with a macro invocation.
      SourceLocation afterDeclEndLoc = Lexer::getLocForEndOfToken(
                         SM->getExpansionLoc(declEndLoc), 0, *SM, Ctx->getLangOpts());
      
      std::optional<CharSourceRange> editableRange =
        clang::tooling::getFileRangeForEdit(
              CharSourceRange::getCharRange(Loc, afterDeclEndLoc),
              *SM, Ctx->getLangOpts(), /*IncludeMacroExpansion=*/true);

      if (!editableRange.has_value()) {
        llvm::outs() << "  unable to get editable range for decls at loc: "
                     << Loc.printToString(*SM) << "\n";
        llvm::outs() << "    to "<< SourceRange(Loc, afterDeclEndLoc).printToString(*SM) << "\n";
        failedDecls["unable_to_get_editable_range"].push_back(Loc);
        continue;
      }

      // We want to avoid processing the same location multiple times,
      // e.g. if a header is included in multiple translation units.
      // DenseMap doesn't have direct support for a full SourceRange
      // so we just track the end location.
      // The underlying SourceLocation ID will be different for each inclusion,
      // so we get the underlying UniqueID which is stable between TUs.
      auto decomposed = SM->getDecomposedLoc(editableRange.value().getEnd());
      llvm::sys::fs::UniqueID fsUID =
          SM->getFileEntryForID(decomposed.first)->getUniqueID();
      StableFileLoc stable_end_loc = std::make_pair(fsUID, decomposed.second);
      if (ProcessedRDeclEndLocs.contains(stable_end_loc)) {
        continue;
      }
      ProcessedRDeclEndLocs.insert(stable_end_loc);


      // for (const NamedDecl* D : Decls) {
      //   llvm::outs() << "  - " << D->getDeclKindName() << ": " << D->getNameAsString() << <<
      //                   D->getSourceRange().printToString(*SM) << "\n";
      // }
      
      bool adjacent_statements_from_macro_expansion = false;
      bool declarators_defined_within_same_macro = false;

      for (const NamedDecl* D : Decls) {
        
        for (const NamedDecl* Dother : Decls) {
          if (D == Dother) continue;

          bool sameStartExpLoc = SM->getExpansionLoc(D->getSourceRange().getBegin()) ==
                            SM->getExpansionLoc(Dother->getSourceRange().getBegin());
          bool sameStartSpellLoc = SM->getSpellingLoc(D->getSourceRange().getBegin()) ==
                              SM->getSpellingLoc(Dother->getSourceRange().getBegin());
          if (sameStartExpLoc && !sameStartSpellLoc) {
            // llvm::outs() << "  DECLS WITH SAME EXPANSION LOC BUT DIFFERENT SPELLING LOC!\n";
            // llvm::outs() << "    D: " << D->getDeclKindName() << " " << D->getNameAsString() << "\n";
            // llvm::outs() << "    common Loc: " << Loc.printToString(*SM) << "\n";
            // llvm::outs() << "      ExpLoc: " << SM->getExpansionLoc(D->getSourceRange().getBegin()).printToString(*SM) << "\n";
            // llvm::outs() << "      SpellLoc: " << SM->getSpellingLoc(D->getSourceRange().getBegin()).printToString(*SM) << "\n";
            // llvm::outs() << "    Dother: " << Dother->getDeclKindName() << " " << Dother->getNameAsString() << "\n";
            // llvm::outs() << "      SpellLoc: " << SM->getSpellingLoc(Dother->getSourceRange().getBegin()).printToString(*SM) << "\n";
            adjacent_statements_from_macro_expansion = true;
            break;
          }

          bool sameIdentExpLoc = SM->getExpansionLoc(D->getLocation()) ==
                            SM->getExpansionLoc(Dother->getLocation());
          bool sameIdentSpellLoc = SM->getSpellingLoc(D->getLocation()) ==
                              SM->getSpellingLoc(Dother->getLocation());
          if (sameIdentExpLoc) {
            // llvm::outs() << "  DECL IDENTS WITH SAME EXPANSION LOC!\n";
            // llvm::outs() << "    D: " << D->getDeclKindName() << " " << D->getNameAsString() << "\n";
            // llvm::outs() << "      SpellLoc: " << SM->getSpellingLoc(D->getSourceRange().getBegin()).printToString(*SM) << "\n";
            // llvm::outs() << "    Dother: " << Dother->getDeclKindName() << " " << Dother->getNameAsString() << "\n";
            // llvm::outs() << "      SpellLoc: " << SM->getSpellingLoc(Dother->getSourceRange().getBegin()).printToString(*SM) << "\n";
            declarators_defined_within_same_macro = true;
            break;
          }
        }
      }

      if (adjacent_statements_from_macro_expansion) {
        failedDecls["macro_multi_stmt"].push_back(Loc);
        continue;
      }
      if (declarators_defined_within_same_macro) {
        failedDecls["macro_multi_declarator"].push_back(Loc);
        continue;
      }


      NumMultiDeclaratorsAtUserScope++;
      if (auto VarD = dyn_cast<VarDecl>(leftmostDecl)) {
        if (VarD->hasGlobalStorage()) {
          NumMultiDeclaratorsGlobalStorage++;
        }
      }

      // We cannot split a declaration if it involves an anonymous tagged type,
      // either directly or indirectly via a typedef, because the split declarations
      // would end up declaring distinct (incompatible) anonymous types.
      //
      bool unhandled = false;
      if (auto ValD = dyn_cast<ValueDecl>(leftmostDecl)) {
        if (auto TagD = ValD->getType().getTypePtr()->getAsTagDecl()) {
          // isEmbeddedInDeclarator() merely checks whether the TagDecl is declared
          // in SOME declarator, not necessarily THIS declarator. For example, if we
          // have `typedef struct { int x; } T; T var1, var2;` then the
          // struct TagDecl is embedded in the declarator for T, but not for var1.
          bool embeddedInThisDeclarator = leftmostDecl->getSourceRange()
                                    .fullyContains(TagD->getSourceRange());
          if (embeddedInThisDeclarator && TagD->getTypedefNameForAnonDecl()) {
            failedDecls["anonymous_tagged_type"].push_back(Loc);
            // llvm::outs() << "  A anonymous_tagged_type  SourceRange: " << TagD->getSourceRange().printToString(*SM); 
            // llvm::outs() << " @ " << stable_end_loc.first.getFile() << " :: " << stable_end_loc.second << "\n";
            continue;
          } else if (embeddedInThisDeclarator && TagD->isThisDeclarationADefinition()) {
            unhandled = true;
          }
        }
      } else if (auto TdD = dyn_cast<TypedefDecl>(leftmostDecl)) {
        if (auto TagD = TdD->getUnderlyingType().getTypePtr()->getAsTagDecl()) {
          bool embeddedInThisDeclarator = leftmostDecl->getSourceRange()
                                    .fullyContains(TagD->getSourceRange());
          if (embeddedInThisDeclarator && TagD->getTypedefNameForAnonDecl()) {
            failedDecls["anonymous_tagged_type"].push_back(Loc);
            // llvm::outs() << "  B anonymous_tagged_type  SourceRange: " << TdD->getSourceRange().printToString(*SM); 
            // llvm::outs() << " @ " << stable_end_loc.first.getFile() << " :: " << stable_end_loc.second << "\n";
            continue;
          } else if (embeddedInThisDeclarator && TagD->isThisDeclarationADefinition()) {
            unhandled = true;
          }
        }
      }


      // PREFIX is everything from the SRSL to the Location of the leftmost Decl,
      // minus any preceding star characters,
      // and one can find the splitting commas by walking backward from the
      // ExpansionLoc of the remaining Decls.
      SourceLocation leftmostNonStarLoc = leftmostSrcLoc;
      while (1) {
        // INVARIANT: every decl's Loc has a distinct ExpansionLoc
        std::optional< Token > opt_prev = Lexer_findPreviousToken(
            SM->getExpansionLoc(leftmostSrcLoc), *SM, Ctx->getLangOpts());
        if (!opt_prev.has_value()) { 
          llvm::outs() << "       no previous token\n";
          break; }
        Token prev = opt_prev.value();
        if (prev.is(tok::star) || prev.is(tok::l_paren)) {
          leftmostSrcLoc = prev.getLocation();
        } else {
          leftmostNonStarLoc = leftmostSrcLoc;          
          std::optional<Token> opt_next = Lexer::findNextToken(
              prev.getLocation(), *SM, Ctx->getLangOpts());
          if (!opt_next.has_value()) { 
            llvm::outs() << "no token after previous token!!!!\n";
            break;
          }
          leftmostSrcLoc = opt_next.value().getLocation();
          break;
        }
      }

      std::vector<SourceLocation> splittingCommaLocs;
      for (const NamedDecl* D : Decls) {
        if (D == leftmostDecl) { continue; }
        SourceLocation Loc = D->getLocation();
        // INVARIANT: every decl's Loc has a distinct ExpansionLoc
        while (1) {
          std::optional< Token > opt_prev = Lexer_findPreviousToken(SM->getExpansionLoc(Loc), *SM, Ctx->getLangOpts());
          if (!opt_prev.has_value()) { break; }
          Token prev = opt_prev.value();
          if (prev.is(tok::comma)) {
            splittingCommaLocs.push_back(prev.getLocation());
            break;
          } else {
            Loc = prev.getLocation();
            if (SM->isBeforeInTranslationUnit(Loc, leftmostSrcLoc)) {
              // did not find comma before reaching leftmostDecl
              break;
            }
          }
        }
      }

      CharSourceRange prefixCharRange = CharSourceRange::getCharRange(
          leftmostDecl->getSourceRange().getBegin(), leftmostNonStarLoc);
      StringRef prefixText = Lexer::getSourceText(
          prefixCharRange, *SM, Ctx->getLangOpts());

      bool hasPreprocessorDirectives = false;
      for (char c : prefixText) {
        // This is a false positive for comments that contain #,
        // but good enough for now.
        if (c == '#') {
          hasPreprocessorDirectives = true;
        }
      }

      if (hasPreprocessorDirectives) {
        failedDecls["directive_in_prefix"].push_back(Loc);
        continue;
      }

      if (prefixText.contains('{')) {
        failedDecls["embedded_tag_definition_in_prefix"].push_back(Loc);
        continue;
      }

      if (prefixText.empty()) {
        failedDecls["empty_prefix"].push_back(Loc);
        continue;
      }

      if (unhandled) {
        ++NumDeclLocsNotYetHandled;
        continue;
      }

      char prefix_end_char = prefixText[prefixText.size()-1];
      if (!isspace(prefix_end_char)) {
        curiousDecls["prefix_ends_with_nonspace"].push_back(Loc);
      }

      std::vector<std::string> declaratorTexts;
      
      // llvm::outs() << "\n--- Declaration Splitter Info ---\n";
      // llvm::outs() << "  Leftmost Decl: " << leftmostDecl->getDeclKindName() << " "
      //              << leftmostDecl->getNameAsString() << "\n";
      // llvm::outs() << "    SourceRange: " << leftmostDecl->getSourceRange().printToString(*SM) << "\n";
      // llvm::outs() << "   LeftmostOffset: " << leftmostOffset << "\n";
      // llvm::outs() << "   LeftmostNonStarOffset: " << leftmostNonStarLoc << "\n";
      // llvm::outs() << "   SplittingCommaOffsets: " << "\n";
      std::sort(splittingCommaLocs.begin(), splittingCommaLocs.end());
      SourceLocation startloc = leftmostNonStarLoc;
      for (SourceLocation commaLoc : splittingCommaLocs) {
        StringRef declaratorText = Lexer::getSourceText(
          CharSourceRange::getCharRange(startloc, commaLoc), *SM, Ctx->getLangOpts());
        declaratorTexts.push_back(declaratorText.str());
        //llvm::outs() << "    " << startloc << ".." << commaLoc << " => '" << declaratorText << "'\n";
        startloc = commaLoc.getLocWithOffset(1); // move past comma
      }

      StringRef declaratorText = Lexer::getSourceText(
        CharSourceRange::getCharRange(startloc, afterDeclEndLoc), *SM, Ctx->getLangOpts());
      declaratorTexts.push_back(declaratorText.str());
      //llvm::outs() << "(fin) " << startloc << ".." << afterDeclEndLoc << " => '" << declaratorText << "'\n";



      unsigned declaratorCommas = 0;
      for (const std::string &declaratorText : declaratorTexts) {
        declaratorCommas += std::count(declaratorText.begin(), declaratorText.end(), ',');
      }

      unsigned prefixCommas = prefixText.count(',');

      // Merely using SourceRange(Loc, declEndLoc) here would fail (with empty span text)
      // on declarations that end with a macro expansion, e.g. `int x, *p = NULL`;
      StringRef overallSpanText = Lexer::getSourceText(editableRange.value(), *SM, Ctx->getLangOpts());
      if (overallSpanText.empty()) {
        llvm::outs() << "       overall span text is empty at " << Loc.printToString(*SM) << "\n";
        curiousDecls["empty_span_text"].push_back(Loc);
      }

      unsigned sawCommas = overallSpanText.count(',');
      if (sawCommas != (splittingCommaLocs.size() + prefixCommas + declaratorCommas)) {
        curiousDecls["comma_count_mismatch"].push_back(Loc);
      }




      // for (const std::string &declaratorText : declaratorTexts) {        
      //   llvm::outs() << prefixText << " " << declaratorText << "\n";
      // }

      // bool invalid = false;
      // const char* start = SM->getCharacterData(
      //   //SM->getFileLoc(rightmostDecl->getSourceRange().getBegin()), &invalid);
      //   rightmostDecl->getSourceRange().getBegin(), &invalid);
      // if (invalid) {
      //   llvm::outs() << "  getCharacterData invalid for rightmost decl range start\n";
      // } else {
      //   //const char* end = SM->getCharacterData(SM->getFileLoc(declEndLoc), &invalid);
      //   const char* end = SM->getCharacterData(
      //     Lexer::getLocForEndOfToken(declEndLoc, 0, *SM, Ctx->getLangOpts()), &invalid);
      //   if (invalid) {
      //     llvm::outs() << "  getCharacterData invalid for decl end loc\n";
      //   } else {

      //     //StringRef fullText(start, end - start);
      //     llvm::outs() << "=== Full text LENGTH from leftmost decl begin to rightmost decl end: ===\n"
      //                  //<< fullText
      //                  << (end - start)
      //                   << "\n=== End full text ===\n";
      //   }
      // }

      const char* cat = "unclassified";
      if (const VarDecl* VD = dyn_cast<VarDecl>(leftmostDecl)) {
        if (VD->hasGlobalStorage()) {
          cat = "global";
        } else {
          cat = "local";
        }
      } else if (leftmostDecl->getKind() == Decl::Typedef) {
        cat = "typedef";
      } else if (leftmostDecl->getKind() == Decl::Field) {
        cat = "field";
      } else if (leftmostDecl->getKind() == Decl::Function) {
        cat = "global";
      }

      if (firstEditWritten) {
        JO.os() << ",";
      } else {
        firstEditWritten = true;
      }
      JO.os() << "{\"r\":"; emitJsonCSR(editableRange.value());
      JO.os() << ",\"lnsloc_macro\":" << (leftmostNonStarLoc.isMacroID() ? "true" : "false");
      JO.os() << ",\"cat\":"; emitJsonString(cat);
      JO.os() << ",\"prefix\":"; emitJsonString(prefixText);
      JO.os() << ",\"declarators\":[";
      bool firstDeclWritten = false;
      for (const std::string &declaratorText : declaratorTexts) {
        if (firstDeclWritten) {
          JO.os() << ",";
        } else {
          firstDeclWritten = true;
        }
        emitJsonString(declaratorText);
      }
      JO.os() << "]}";
      JO.os() << "\n";
    }

    for (auto &[key, Locs] : failedDecls) {
      FailedDeclSplitsByReason[key] += Locs.size();
      // llvm::outs() << "\n=== Failed decl splits for reason: " << key.str() << " ===\n";
      // for (SourceLocation Loc : Locs) {
      //   llvm::outs() << "  - at " << Loc.printToString(*SM) << "\n";
      // }
    }

    for (auto &[key, Locs] : curiousDecls) {
      CuriousDeclSplitsByReason[key] += Locs.size();
      // llvm::outs() << "\n=== Curious decl splits for reason: " << key.str() << " ===\n";
      // for (SourceLocation Loc : Locs) {
      //   llvm::outs() << "  - at " << Loc.printToString(*SM) << "\n";
      // }
    }
    

    
    SrcRangeExpStartLocToDeclMap.clear();
  }

  void emitJsonString(StringRef Str) {
    JO.os() << "\"";
    for (char c : Str) {
      switch (c) {
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
          JO.os() << c;
          break;
      }
    }
    JO.os() << "\"";
  }

  void emitJsonLoc(SourceLocation Loc) {
    JO.os() << "{" << "\"f\":\"" << SM->getFilename(Loc).str() << "\","
            << "\"o\":" << SM->getFileOffset(Loc)
            << "}";
  }

  void emitJsonCSR(CharSourceRange CSR) {
    if (CSR.isInvalid()) {
      JO.os() << "null";
      return;
    }
    if (!SM->isWrittenInSameFile(CSR.getBegin(), CSR.getEnd())) {
      JO.os() << "null";
      return;
    }
    // if (CSR.isTokenRange()) {
    //   SourceLocation endLoc = Lexer::getLocForEndOfToken(
    //       CSR.getEnd(), 0, *SM, Ctx->getLangOpts());
    //   CSR = CharSourceRange::getCharRange(CSR.getBegin(), endLoc);
    // }
    JO.os() << "{\"f\":\"" << SM->getFilename(CSR.getBegin()).str() << "\"";
    JO.os() << ",\"b\":" << SM->getFileOffset(CSR.getBegin());
    JO.os() << ", \"e\":" << SM->getFileOffset(CSR.getEnd());
    //JO.os() << ", \"raw\":" << CSR.getEnd().getRawEncoding();
    JO.os() << "}";
  }

  // backported for LLVM 18, which hadn't implemented this yet
  std::optional<Token> Lexer_findPreviousToken(SourceLocation Loc,
                                               const SourceManager &SM,
                                               const LangOptions &LangOpts,
                                               bool IncludeComments = false) {
  const auto StartOfFile = SM.getLocForStartOfFile(SM.getFileID(Loc));
  
  while (Loc != StartOfFile) {
    Loc = Loc.getLocWithOffset(-1);
    if (Loc.isInvalid())
      return std::nullopt;

    Loc = Lexer::GetBeginningOfToken(Loc, SM, LangOpts);
 
    Token Tok;
    if (Lexer::getRawToken(Loc, Tok, SM, LangOpts)) {
      continue; // Not a token, go to prev location.
    }
    if (!Tok.is(tok::comment) || IncludeComments) {
      return Tok;
    }
  }
  return std::nullopt;
}

public:
  //ExecutionContext &Context;
  SourceManager *SM;
  ASTContext *Ctx;
  llvm::ToolOutputFile &JO;
  
  unsigned NumDeclLocsVisitedOrRevisited = 0;
  unsigned NumMultiDeclaratorsGlobalStorage = 0;
  unsigned NumMultiDeclaratorsAtUserScope = 0;
  unsigned NumDeclLocsNotYetHandled = 0;
  DenseMap<SourceLocation, std::vector<const NamedDecl*>> SrcRangeExpStartLocToDeclMap;
  StringMap<int> FailedDeclSplitsByReason;
  StringMap<int> CuriousDeclSplitsByReason;
  DenseSet<StableFileLoc> ProcessedRDeclEndLocs;
  bool firstEditWritten = false;
};


} // end anonymous namespace

static cl::extrahelp CommonHelp(CommonOptionsParser::HelpMessage);
static cl::OptionCategory
    PrintDeclLocsCategory("xj-print-decl-locs options");

static cl::opt<std::string> JsonOutputPath(
    "json-output-path",
    cl::desc("Path to JSON output file"),
    cl::cat(PrintDeclLocsCategory));

int main(int argc, const char **argv) {
  // Standard LLVM boilerplate
  llvm::sys::PrintStackTraceOnErrorSignal(argv[0]);

  auto Executor = clang::tooling::createExecutorFromCommandLineArgs(
      argc, argv, PrintDeclLocsCategory);

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
  PrintDeclLocsCallback Callback(*Executor->get()->getExecutionContext(), JO);

  // Add matchers for FieldDecl, TypedefDecl, and VarDecl
  Finder.addMatcher(fieldDecl().bind("fieldDecl"), &Callback);
  Finder.addMatcher(typedefDecl().bind("typedefDecl"), &Callback);
  Finder.addMatcher(functionDecl().bind("functionDecl"), &Callback);
  Finder.addMatcher(varDecl().bind("varDecl"), &Callback);

  JO.os() << "{" << "\n";
  JO.os() << "\"edits\":[" << "\n";

  // Run the matchers over whatever TU(s) the command line args specified.
  auto Err = Executor->get()->execute(newFrontendActionFactory(&Finder));
  // getInsertArgumentAdjuster("-v")
  if (Err) {
    llvm::errs() << llvm::toString(std::move(Err)) << "\n";
  }

  JO.os() << "]" << "\n";
  JO.os() << "}" << "\n";
  JO.keep();
  llvm::outs() << "\n=== Tool Results: ===\n";
  llvm::outs() << "Number of decl locations visited/revisited: "
               << Callback.NumDeclLocsVisitedOrRevisited << "\n";
  llvm::outs() << "Number of multi-declarator decl locs at user scope: "
               << Callback.NumMultiDeclaratorsAtUserScope << "\n";
  llvm::outs() << "Number of multi-declarator decl locations with global storage: "
               << Callback.NumMultiDeclaratorsGlobalStorage << "\n";
  llvm::outs() << "Number of decl locations not yet handled: "
               << Callback.NumDeclLocsNotYetHandled << "\n";
  for (auto &[key, count] : Callback.FailedDeclSplitsByReason) {
    llvm::outs() << "  Failed decl splits for reason '" << key.str()
                 << "': " << count << "\n";
  }
  for (auto &[key, count] : Callback.CuriousDeclSplitsByReason) {
    llvm::outs() << "  Curious decl splits for reason '" << key.str()
                 << "': " << count << "\n";
  }

  Executor->get()->getToolResults()->forEachResult(
      [](llvm::StringRef key, llvm::StringRef value) {
        llvm::errs() << "----" << key.str() << "\n" << value.str() << "\n";
      });
  return 0;
}
