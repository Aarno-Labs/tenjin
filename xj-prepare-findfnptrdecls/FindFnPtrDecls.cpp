#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Lex/Lexer.h"
#include "clang/Tooling/Execution.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Signals.h"

using namespace clang;
using namespace clang::ast_matchers;
using namespace clang::tooling;
using namespace llvm;

namespace {
StringSet ModifiedFnNames;
StringSet UnmodifiedFnNames;

bool is_modified_fn_name(const std::string &s) {
  return ModifiedFnNames.contains(s);
}

bool is_unmodified_fn_name(const std::string &s) {
  return UnmodifiedFnNames.contains(s);
}

// In phase 1, we look for non-call occurrences of functions which we
// know will be getting their signature modified with an XjGlobals*
// parameter, and try to identify what type declaration needs to be
// modified accordingly.
//
// For now, we look for specific usage contexts in which function
// pointers occur in practice: assignments where the RHS is a function
// name, and occurences of a function name in a (possibly nested)
// initializer list expression. These patterns can be expanded over
// time as we encounter additional programs.
//      (The "right" way to do this is probably some type inference/
//      abstract interpretation of the Clang AST, which would permit 
//      arbitrarily long chains of assignments. Our approach here is
//      incomplete but useful in the short term.)
//
// By the end of phase 1, we've identified a set of type declarations
// which need modifications because they have modified functions flowing
// to them. These will be concretely serialized as a list of int pairs
// for the open and close parens of the relevant function pointer type's
// parameter list.
//
// In phase 2, we look for the same kinds of occurrences of functions
// which we know will NOT be getting modified signatures, but which
// flow to the same type declarations identified in phase 1. These
// occurrences require us to generate wrapper functions with the
// XjGlobals* parameter in order to maintain type compatibility.
//      (The phases are conceptually separate/sequential. In practice,
//       we collect the information about occurrences during phase 1.
//       Only the analysis is implemented separately.)
//
// TODO: in general, we may need to link declarations across translation
//       units: if we have TU A and TU B, where B has an extern decl
//       for a definition in A, and B's (extern) decl is marked as
//       needing modification, we must also modify the definition in A,
//       or else we've introduced undefined behavior.
//
// TODO: even within a single translation unit, we must update both
//       definitions (from the .c code) and declarations (from
//       project headers).
//
// TODO: typedef introduces sharing in the type graph; when modifying
//       types, we don't want it to become false sharing.
//       E.g. if we have function pointer vars A and B, sharing
//       typedef T, we may need to change the types independently.

struct InitListOccurrence {
   unsigned idx;
   const DeclRefExpr* dre;
   bool dre_fn_was_mod; 
};

class FindFnPtrDeclsCallback : public MatchFinder::MatchCallback {
public:
  FindFnPtrDeclsCallback(ExecutionContext &Context)
      : Context(Context), SM(nullptr), Ctx(nullptr) {}

  void run(const MatchFinder::MatchResult &Result) override {
    if (!SM) {
      SM = Result.SourceManager;
      Ctx = Result.Context;
    }

    auto *ILE = Result.Nodes.getNodeAs<InitListExpr>("init_list_expr");
    if (ILE && ILE->getBeginLoc().isValid()) {
      handle_init_list_expr(ILE, Result);
      return;
    }

    auto *D = Result.Nodes.getNodeAs<Stmt>("assign_to_declrefexpr");
    if (D && D->getBeginLoc().isValid()) {
      handle_assign_to_decl(D, Result);
      return;
    }

    D = Result.Nodes.getNodeAs<Stmt>("assign_to_member");
    if (D && D->getBeginLoc().isValid()) {
      handle_assign_to_decl(D, Result);
      return;
    }

    if (auto *arg_dre = Result.Nodes.getNodeAs<DeclRefExpr>("call_arg")) {
      if (arg_dre->getBeginLoc().isValid()) {
        handle_call_arg_to_fn_ptr_param(arg_dre, Result);
        return;
      }
    }

    if (auto *CE = Result.Nodes.getNodeAs<CallExpr>("called_fn_ptr_expr")) {
        if (auto* CFE = Result.Nodes.getNodeAs<Expr>("called_fn_expr")) {
          if (auto *CED = CFE->getReferencedDeclOfCallee()) {
              // we're looking for callees that don't have obvious decls
              return;
          }

          if (auto* CECE = dyn_cast<CallExpr>(CFE->IgnoreParenImpCasts())) {
              if (auto *CED = CECE->getCallee()->getReferencedDeclOfCallee()) {
                  if (auto* CEDD = dyn_cast<DeclaratorDecl>(CED)) {
                      if (auto *TSI = CEDD->getTypeSourceInfo()) {
                          TypeLoc TL = TSI->getTypeLoc();
                            FunctionTypeLoc FTL;
                            if (!try_find_fn_ptr_TL(TL, FTL)) {
                                return;
                            }

                            FunctionTypeLoc RetFTL;
                            if (!try_find_fn_ptr_TL(FTL.getReturnLoc(), RetFTL)) {
                                return;
                            }
                            FnPtrTypeOpenParens_PotentiallyMod[RetFTL.getLParenLoc()] =
                                RetFTL.getRParenLoc();
                      }
                  }
              }
          }
        }
        return;
    }

    auto *VD = Result.Nodes.getNodeAs<VarDecl>("fn_ptr_var_decl");
    if (VD && VD->getBeginLoc().isValid()) {
      if (VD->getName().starts_with("__")) { return; }
      auto *TSI = VD->getTypeSourceInfo();
      if (TSI) {
        FunctionTypeLoc FTL;
        if (try_find_fn_ptr_TL(TSI->getTypeLoc(), FTL)) {
          byFile_fnptr_vardecls_lparen
              [SM->getFilename(VD->getLocation())]
              [VD->getNameAsString()] =
                  SM->getFileOffset(FTL.getLParenLoc());
        }
      }
      return;
    }

    VD = Result.Nodes.getNodeAs<VarDecl>("global_var_decl");
    if (VD && VD->getBeginLoc().isValid()) {
      if (!VD->hasInit()) {
          globals_without_initializers.insert(VD->getNameAsString());
      }
    }
  }

  void handle_assign_to_decl(const Stmt *D,
                             const MatchFinder::MatchResult &Result) {
    auto *rhs = Result.Nodes.getNodeAs<DeclRefExpr>("rhs");
    if (rhs && rhs->getBeginLoc().isValid()) {
      std::string rhs_name = rhs->getNameInfo().getName().getAsString();
      bool was_mod_fn = is_modified_fn_name(rhs_name);
      bool was_unmod_fn = is_unmodified_fn_name(rhs_name);
      if (!was_mod_fn && !was_unmod_fn) {
          return;
      }

      // MemberExpr in Clang has ValueDecl for the member, but for C it will
      // always be a FieldDecl (which is a DeclaratorDecl).
      auto *lhs_dd = Result.Nodes.getNodeAs<DeclaratorDecl>("lhs_value_decl");
      if (!lhs_dd) {
          lhs_dd = Result.Nodes.getNodeAs<DeclaratorDecl>("lhs_dcrr_decl");
      }
      
      if (lhs_dd && lhs_dd->getType()->isFunctionPointerType()) {
          // Get the TypeSourceInfo for better location tracking
          if (TypeSourceInfo *TSI = lhs_dd->getTypeSourceInfo()) {
            // We should have a PointerTypeLoc because lhs_dd was a function pointer type
            if (PointerTypeLoc PTL = TSI->getTypeLoc().getAs<PointerTypeLoc>()) {
              TypeLoc FuncTL = PTL.getPointeeLoc();

              FunctionTypeLoc FTL;
              if (try_find_fn_ptr_TL(FuncTL, FTL)) {
                if (was_mod_fn) {
                    FnPtrTypeOpenParens[FTL.getLParenLoc()] = FTL.getRParenLoc();
                    ModifyingDeclIDs.insert(lhs_dd);
                } else {
                    // found a non-modified function occurrence; record its location
                    // and the targeted declaration, so we can correlate in phase 2.
                    UnmodFnOccurrences.push_back(std::make_pair(rhs, lhs_dd));
                }
              }
            }
          }
      }
    }
  }

  void handle_init_list_expr(const InitListExpr *ILE,
                             const MatchFinder::MatchResult &Result) {
    SmallVector<InitListOccurrence> field_nums;

    for (unsigned i = 0; i < ILE->getNumInits(); ++i) {
      const Expr *e = ILE->getInit(i)->IgnoreParenCasts();
      if (const DeclRefExpr *dre = dyn_cast<DeclRefExpr>(e)) {
        std::string name = dre->getDecl()->getNameAsString();
        if (is_modified_fn_name(name)) {
          field_nums.push_back(InitListOccurrence { .idx = i, .dre = dre, .dre_fn_was_mod = true });
        } else if (is_unmodified_fn_name(name)) {
          field_nums.push_back(InitListOccurrence { .idx = i, .dre = dre, .dre_fn_was_mod = false });
        } // else not a fn, ignore it
      }
    }

    int curr_num = 0;
    QualType QT = ILE->getType();
    if (const RecordType *RT = QT->getAs<RecordType>()) {
      RecordDecl *RD = RT->getDecl();
      auto FieldIt = RD->field_begin();
      for (unsigned i = 0; i < field_nums.size(); ++i) {
        while (curr_num < field_nums[i].idx) {
          ++curr_num;
          ++FieldIt;
        }
        FieldDecl *TargetField = *FieldIt;
        if (auto TSI = TargetField->getTypeSourceInfo()) {
          FunctionTypeLoc FTL;
          if (try_find_fn_ptr_TL(TSI->getTypeLoc(), FTL)) {
              if (field_nums[i].dre_fn_was_mod) {
                FnPtrTypeOpenParens[FTL.getLParenLoc()] = FTL.getRParenLoc();
                ModifyingDeclIDs.insert(TargetField);
              } else {
                // found a non-modified function occurrence; record its location
                // and the targeted declaration, so we can correlate in phase 2.
                UnmodFnOccurrences.push_back(std::make_pair(field_nums[i].dre, TargetField));
              }
          } else {
            llvm::errs() << "unable to find fn ptr lparen loc for InitListExpr"
                         << "\n";
          }
        } else {
          llvm::errs() << "unable to find fn ptr TSI for InitListExpr" << "\n";
        }
      }
    }
  }

  // Handles a function name being passed as an argument to a function-pointer
  // parameter. The parameter declaration is the targeted decl, mirroring how
  // member/declref assignment LHSes are treated.
  void handle_call_arg_to_fn_ptr_param(const DeclRefExpr *arg_dre,
                                       const MatchFinder::MatchResult &Result) {
    std::string arg_name = arg_dre->getNameInfo().getName().getAsString();
    bool was_mod_fn = is_modified_fn_name(arg_name);
    bool was_unmod_fn = is_unmodified_fn_name(arg_name);
    if (!was_mod_fn && !was_unmod_fn) {
      return;
    }

    auto *param = Result.Nodes.getNodeAs<ParmVarDecl>("call_param");
    if (!param || !param->getType()->isFunctionPointerType()) {
      return;
    }

    TypeSourceInfo *TSI = param->getTypeSourceInfo();
    if (!TSI) {
      return;
    }

    PointerTypeLoc PTL = TSI->getTypeLoc().getAs<PointerTypeLoc>();
    if (!PTL) {
      return;
    }

    TypeLoc FuncTL = PTL.getPointeeLoc();
    FunctionTypeLoc FTL;
    if (!try_find_fn_ptr_TL(FuncTL, FTL)) {
      return;
    }

    if (was_mod_fn) {
      FnPtrTypeOpenParens[FTL.getLParenLoc()] = FTL.getRParenLoc();
      ModifyingDeclIDs.insert(param);
    } else {
      UnmodFnOccurrences.push_back(std::make_pair(arg_dre, param));
    }
  }

  bool try_find_fn_ptr_TL(TypeLoc TL, FunctionTypeLoc& FTL) {
    while (1) {
      if (auto TDTL = TL.getAs<TypedefTypeLoc>()) {
          if (auto* D = TDTL.getTypedefNameDecl()) {
              if (TypeSourceInfo *TSI = D->getTypeSourceInfo()) {
                  TL = TSI->getTypeLoc();
                  continue;
              }
          }
          break;
      }

      // FunctionProtoTypeLoc is the one that has parameter parens
      if (auto FPTL = TL.getAs<FunctionProtoTypeLoc>()) {
          FTL = TL.getAs<FunctionTypeLoc>();
          return true;
      }
      if (auto FNPL = TL.getAs<FunctionNoProtoTypeLoc>()) {
          FTL = TL.getAs<FunctionTypeLoc>();
          return true;
      }

      // walk "inward" one TypeLoc
      TL = TL.getNextTypeLoc();
      if (TL.isNull())
        break; // defensive
    }
    return false;
  }

  void onStartOfTranslationUnit() override {
    SM = nullptr;
    Ctx = nullptr;
    FnPtrTypeOpenParens.clear();
    ModifyingDeclIDs.clear();
    UnmodFnOccurrences.clear();
    FnPtrTypeOpenParens_PotentiallyMod.clear();
    // The byFile maps are not cleared; they accumulate across TUs.
    // They are indexed by strings, rather than SourceLocation, like
    // the stuff cleared here.
    // Indexing by SourceLocation across translation units does not
    // work because FileIDs are reused for different paths.
  }

  void onEndOfTranslationUnit() override {
    if (!SM && !FnPtrTypeOpenParens.empty()) {
      Context.reportResult(
          "END",
          "End of TU -- no SourceManager but had identified open parens");
      return;
    }

    collectMappedRangesByFile(byFile_fnptr_args, FnPtrTypeOpenParens);
    collectMappedRangesByFile(byFile_ho_fnptr_args, FnPtrTypeOpenParens_PotentiallyMod);

    for (auto &dre_dd : UnmodFnOccurrences) {
        if (ModifyingDeclIDs.count(dre_dd.second) > 0) {
            auto F = SM->getFilename(dre_dd.first->getLocation());
            byFile_wrappers[F].push_back(
                    fmtJSONDictForUnmodFnOccWrapper(dre_dd));
        }
    }
  }

  void collectMappedRangesByFile(
            StringMap<SmallVector<std::pair<int, int>>> &byFile,
            const DenseMap<SourceLocation, SourceLocation> &rangeMap) {
    for (auto &[Loc, RLoc] : rangeMap) {
        auto F = SM->getFilename(Loc);
        if (F.empty()) {
            llvm::errs() << "WARNING: empty filename for loc "
                         << Loc.printToString(*SM) << "\n";
            llvm::errs() << "    loc valid? " << Loc.isValid() << "\n";
            if (auto FER = SM->getFileEntryRefForID(SM->getFileID(Loc))) {
                //auto *FER = OFER;
                llvm::errs() << "    file entry name: "
                             << FER->getName() << "\n";
                llvm::errs() << "    canonical name: "
                  << Ctx->getSourceManager().getFileManager().getCanonicalName(*FER);
            } else {
                llvm::errs() << "    no file entry for loc\n";
            }
        }
        byFile[F].push_back(
            std::make_pair(SM->getFileOffset(Loc), SM->getFileOffset(RLoc)));
    }

  }

  std::string fmtUnmodFnWrapper(const DeclaratorDecl* d, const std::string& suffix) {
      std::string rv;
      llvm::raw_string_ostream sout(rv);

      FunctionTypeLoc FTL;
      if (d->getType()->isFunctionPointerType()) {
          auto TSI = d->getTypeSourceInfo();
          if (!TSI) {
              return "!<no TypeSourceInfo for unmod fn ptr decl>";
          }
          if (!try_find_fn_ptr_TL(TSI->getTypeLoc(), FTL)) {
              return "!<unable to find type loc for fn ptr>";
          }
      } else if (const FunctionDecl* fd = dyn_cast<FunctionDecl>(d)) {
          FTL = fd->getFunctionTypeLoc();
            //sout << "hasWrittenPrototype()=" << fd->hasWrittenPrototype();
      } else {
        sout << "!<not a function decl!?> offset=";
        sout << SM->getFileOffset(d->getBeginLoc()); 
        return rv;
      }

      // llvm::errs() << "ret type range: " << FTL.getReturnLoc().getSourceRange().printToString(*SM) << "\n";
      // llvm::errs() << "ret type begin loc: " << FTL.getReturnLoc().getBeginLoc().printToString(*SM) << "\n";
      // llvm::errs() << "ret type end loc: " << FTL.getReturnLoc().getEndLoc().printToString(*SM) << "\n";
      // llvm::errs() << "decl primary loc: " << d->getLocation().printToString(*SM) << "\n";
      // llvm::errs() << "\n";

      // return type as written
      //    (getReturnLoc().getSourceRange() can be empty for void returns)
      //    (d->getLocation() is the start of the name of the function)
      sout << Lexer::getSourceText(CharSourceRange::getCharRange(
                                        FTL.getReturnLoc().getBeginLoc(),
                                        d->getLocation()),
                                   *SM, Ctx->getLangOpts());

      // wrapper function name
      sout << d->getName() << suffix;

      sout << "(struct XjGlobals*";

      if (FTL.getNumParams() > 0) {
         sout << ", ";
         // existing parameters, as-is
         sout << Lexer::getSourceText(
                 CharSourceRange::getCharRange(
                    FTL.getLParenLoc().getLocWithOffset(1),
                    FTL.getRParenLoc()
                 ),
                 *SM, Ctx->getLangOpts());
      }
      sout << ")";
      sout << " { return " << d->getName() << "(";
      for (unsigned i = 0; i < FTL.getNumParams(); ++i) {
          ParmVarDecl* pvd = FTL.getParam(i);
          if (i > 0) { sout << ", "; }
          if (pvd) {
              sout << pvd->getName();
          } else {
              sout << "_";
          }
      }
      sout << "); }";

      return rv;
  }

  std::string fmtJSONDictForUnmodFnOccWrapper(std::pair<const DeclRefExpr*, const DeclaratorDecl*> p) {
      std::string rv;
      llvm::raw_string_ostream sout(rv);

      // The DeclaratorDecl of p.second is the location the fn occurrence is flowing to,
      // but we need the decl of the function itself.
      const DeclaratorDecl* fdd = dyn_cast<DeclaratorDecl>(p.first->getDecl());
      if (!fdd) {
        return "<unmod fn decl was not a declarator?!>";
      }
      auto final_tok = fdd->hasBody() ? tok::r_brace : tok::semi ;
      SourceLocation post_loc =
          Lexer::findLocationAfterToken(
                        fdd->getEndLoc(),
                        final_tok,
                        *SM,
                        Ctx->getLangOpts(),
                        /*SkipTrailingWhitespaceAndNewline=*/ true);
      if (fdd->hasBody()) {
          llvm::errs() << "For body-having function decl of " << fdd->getNameAsString() << "\n";
          llvm::errs() << "post_loc for body after rbrace was " << post_loc.printToString(*SM) << "\n";
          llvm::errs() << "end_loc for body               was " << fdd->getEndLoc().printToString(*SM) << "\n";
      }
      if (post_loc.isInvalid()) {
          post_loc = fdd->getEndLoc().getLocWithOffset(1); // YOLO i guess
      }
      std::string suffix = "_xjw";
      sout << "{ \"name\": \"" << p.first->getNameInfo().getAsString() << "\""
           << ", \"suffix\": \"" << suffix << "\""
           << ", \"occ_offset\": " << SM->getFileOffset(p.first->getBeginLoc())
           << ", \"decl_post_offset\": " << SM->getFileOffset(post_loc)
           << ", \"wrapper_defn\": \"" << fmtUnmodFnWrapper(fdd, suffix) << "\""
           << "}";
      return rv;
  }

  // Prints a JSON dict of the form
  // ```
  //     { "<FILEPATH_1>":[ {...}, ...],
  //       "<FILEPATH_2>":[...], ... }
  // ```
  void emitJSONDictForUnmodFnOccWrappers() {
      llvm::outs() << "{" << "\n";
      bool firstfile = true;
      for (auto &[F, PreformattedJsonStrs] : byFile_wrappers) {
        if (!firstfile) {
          llvm::outs() << ",\n";
        } else {
          firstfile = false;
        }

        llvm::outs() << "\"" << F << "\""
                     << ":" << "\n"
                     << "[";

        bool first = true;
        for (auto S : PreformattedJsonStrs) {
          if (!first) {
            llvm::outs() << ", ";
          } else {
            first = false;
          }
          llvm::outs() << S;
        }
        llvm::outs() << "]";
      }
      llvm::outs() << "}" << "\n";
  }


 // Prints a JSON dict of the form
 // ```
 //     { "<FILEPATH_1>":[ [o1,c1], [o2,c2], ...],
 //       "<FILEPATH_2>":[...], ... }
 // ```
 void emitJSONDictForSourceRangesByFile(
  StringMap<SmallVector<std::pair<int, int>>> &ranges_byFile
) {
      llvm::outs() << "{" << "\n";
      bool firstfile = true;
      for (auto &[F, Offsets] : ranges_byFile) {
        if (!firstfile) {
          llvm::outs() << ",\n";
        } else {
          firstfile = false;
        }

        llvm::outs() << "\"" << F << "\""
                     << ":" << "\n"
                     << "[";

        bool first = true;
        for (auto Off : Offsets) {
          if (!first) {
            llvm::outs() << ", ";
          } else {
            first = false;
          }
          llvm::outs() << "[" << Off.first << ", " << Off.second << "]";
        }
        llvm::outs() << "]";
      }
      llvm::outs() << "}" << "\n";
  }

  void emitJSONDictForModifiedFnPtrTypeLocs() {
      emitJSONDictForSourceRangesByFile(byFile_fnptr_args);
  }

  void emitJSONDictForHigherOrderPotentiallyModifiedFnPtrTypeLocs() {
      emitJSONDictForSourceRangesByFile(byFile_ho_fnptr_args);
  }

  void emitJSONDictForVarDeclFnPtrArgLParenLocs() {
      llvm::outs() << "{" << "\n";
      bool firstfile = true;
      for (auto &[F, VarOffsetMap] : byFile_fnptr_vardecls_lparen) {
        if (!firstfile) {
          llvm::outs() << ",\n";
        } else {
          firstfile = false;
        }

        llvm::outs() << "\"" << F << "\""
                     << ":" << "\n"
                     << "{";

        bool first = true;
        for (auto &[VarName, Offset] : VarOffsetMap) {
          if (!first) {
            llvm::outs() << ", ";
          } else {
            first = false;
          }
          llvm::outs() << "\"" << VarName << "\": " << Offset;
        }
        llvm::outs() << "}";
      }
      llvm::outs() << "}" << "\n";
  }

  void emitJSONListOfGlobalsWithoutInitializers() {
      llvm::outs() << "[";
      bool first = true;
      for (auto &Entry : globals_without_initializers) {
          if (!first) {
              llvm::outs() << ", ";
          } else {
              first = false;
          }
          llvm::outs() << "\"" << Entry.getKey() << "\"";
      }
      llvm::outs() << "]";
  }

private:
  ExecutionContext &Context;
  SourceManager *SM;
  ASTContext *Ctx;
  std::string CurrentTUPath;

  // We track which Decls are getting modified via FnPtrTypeOpenParens
  // and, when we see an assignment of an unmodified function to that Decl,
  // we know we need to generate a wrapper for that function.
  DenseSet<const DeclaratorDecl*>
     ModifyingDeclIDs;

  std::vector<std::pair<const DeclRefExpr*, const DeclaratorDecl*>>
      UnmodFnOccurrences;

  DenseMap<SourceLocation, SourceLocation>
      FnPtrTypeOpenParens; // maps left paren to right paren

  DenseMap<SourceLocation, SourceLocation>
      FnPtrTypeOpenParens_PotentiallyMod;

  StringMap<SmallVector<std::pair<int, int>>>
      byFile_fnptr_args; // file -> list[pair[offset]]

  // Since these are higher-order usages, it's rather harder
  // to be sure that they really need modification (or that
  // they have only modified functions flow to them). So we
  // track them separately, so they can be modified speculatively.
  StringMap<SmallVector<std::pair<int, int>>>
      byFile_ho_fnptr_args; // file -> list[pair[offset]]

  // For variables with function pointer types (of which the
  // non-canonical version might be behind a typedef), for which
  // we want to replicate edits across translation units, we can't
  // track just the span of the function type's arguments, because
  // the text of those can & will vary across TUs due to things like
  // const qualifiers, use of typedefs, etc. So we track only the
  // lparen location, and only support edits directly there.
  StringMap<StringMap<int>>
      byFile_fnptr_vardecls_lparen; // file -> varname -> offset of lparen

  StringMap<SmallVector<std::string>>
      byFile_wrappers;

  StringSet<> globals_without_initializers;
};


} // end anonymous namespace

static cl::extrahelp CommonHelp(CommonOptionsParser::HelpMessage);
static cl::OptionCategory
    FindFnPtrDeclsCategory("xj-find-fn-ptr-decls options");

static cl::opt<std::string> ModifiedFunctionNamesFile(
    "modified_fns_file",
    cl::desc("File containing names of modified functions"),
    cl::cat(FindFnPtrDeclsCategory));

static cl::opt<std::string> UnmodifiedFunctionNamesFile(
    "unmodified_fns_file",
    cl::desc("File containing names of unmodified functions"),
    cl::cat(FindFnPtrDeclsCategory));

// return false on failure
bool initialize_ModifiedFnNames() {
  if (ModifiedFunctionNamesFile.empty()) {
    llvm::errs() << "Must provide path for --modified_fns_file" << "\n";
    return false;
  }

  llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> Buffer =
      llvm::MemoryBuffer::getFile(ModifiedFunctionNamesFile);

  if (auto Err = Buffer.getError()) {
    llvm::errs() << "Unable to open " << ModifiedFunctionNamesFile << ": "
                 << Err.message() << "\n";
    return false;
  }

  llvm::StringRef FileContent = Buffer.get()->getBuffer();
  while (!FileContent.empty()) {
    llvm::StringRef Name;
    std::tie(Name, FileContent) = FileContent.split("\n");
    ModifiedFnNames.insert(Name);
  }

  return true;
}

// return false on failure
bool initialize_UnmodFnNames() {
  if (UnmodifiedFunctionNamesFile.empty()) {
    llvm::errs() << "Must provide path for --unmodified_fns_file" << "\n";
    return false;
  }

  llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> Buffer =
      llvm::MemoryBuffer::getFile(UnmodifiedFunctionNamesFile);

  if (auto Err = Buffer.getError()) {
    llvm::errs() << "Unable to open " << UnmodifiedFunctionNamesFile << ": "
                 << Err.message() << "\n";
    return false;
  }

  llvm::StringRef FileContent = Buffer.get()->getBuffer();
  while (!FileContent.empty()) {
    llvm::StringRef Name;
    std::tie(Name, FileContent) = FileContent.split("\n");
    UnmodifiedFnNames.insert(Name);
  }

  return true;
}



int main(int argc, const char **argv) {
  // Standard LLVM boilerplate
  llvm::sys::PrintStackTraceOnErrorSignal(argv[0]);

  auto Executor = clang::tooling::createExecutorFromCommandLineArgs(
      argc, argv, FindFnPtrDeclsCategory);

  if (!Executor) {
    llvm::errs() << llvm::toString(Executor.takeError()) << "\n";
    return 1;
  }

  if (!initialize_ModifiedFnNames()) { return 1; }
  if (!initialize_UnmodFnNames()) { return 1; }

  ast_matchers::MatchFinder Finder;
  FindFnPtrDeclsCallback Callback(*Executor->get()->getExecutionContext());

  // Configure matchers to identify non-call occurrences
  // of (potential) function pointers. 
  Finder.addMatcher(
      binaryOperator(
          hasOperatorName("="),
          hasLHS(declRefExpr(hasDeclaration(declaratorDecl().bind("lhs_dcrr_decl")))
                     .bind("lhs")),
          hasRHS(expr(ignoringImpCasts(declRefExpr().bind("rhs")))))
          .bind("assign_to_declrefexpr"),
      &Callback);

  Finder.addMatcher(
      binaryOperator(
          hasOperatorName("="),
          hasLHS(memberExpr(member(valueDecl().bind("lhs_value_decl")))
                     .bind("lhs")),
          hasRHS(expr(ignoringImpCasts(declRefExpr().bind("rhs")))))
          .bind("assign_to_member"),
      &Callback);

  Finder.addMatcher(
      callExpr(callee(expr(hasType(hasCanonicalType(
                                      pointerType(
                                          pointee(
                                              functionType()
                                          )
                                      )))
                                  ).bind("called_fn_expr"))
              ).bind("called_fn_ptr_expr"),
      &Callback
  );

  // Function-name arguments passed to function-pointer parameters: the
  // parameter declaration acts like the LHS of an assignment.
  Finder.addMatcher(
      callExpr(forEachArgumentWithParam(
          ignoringParenImpCasts(declRefExpr().bind("call_arg")),
          parmVarDecl(hasType(hasCanonicalType(
                                  pointerType(
                                      pointee(
                                          functionType()
                                      )
                                  ))))
              .bind("call_param"))),
      &Callback
  );

  Finder.addMatcher(
      varDecl(hasType(hasCanonicalType(
                                      pointerType(
                                          pointee(
                                              functionType()
                                          )
                                      )))
              ).bind("fn_ptr_var_decl"),
      &Callback
  );

  Finder.addMatcher(
      varDecl(hasGlobalStorage()).bind("global_var_decl"),
      &Callback
  );

  Finder.addMatcher(initListExpr(has(expr(ignoringImpCasts(declRefExpr()))))
                        .bind("init_list_expr"),
                    &Callback);

  // Run the matchers over whatever TU(s) the command line args specified.
  auto Err = Executor->get()->execute(newFrontendActionFactory(&Finder));
  if (Err) {
    llvm::errs() << llvm::toString(std::move(Err)) << "\n";
  }
  Executor->get()->getToolResults()->forEachResult(
      [](llvm::StringRef key, llvm::StringRef value) {
        llvm::errs() << "----" << key.str() << "\n" << value.str() << "\n";
      });

  llvm::outs() << "{\n";
  llvm::outs() << "\"modified_fn_ptr_type_locs\": ";
  Callback.emitJSONDictForModifiedFnPtrTypeLocs();
  llvm::outs() << ",\n";
  llvm::outs() << "\"unmod_fn_occ_wrappers\": ";
  Callback.emitJSONDictForUnmodFnOccWrappers();
  llvm::outs() << ",\n";
  llvm::outs() << "\"higher_order_potentially_modified_fn_ptr_type_locs\": ";
  Callback.emitJSONDictForHigherOrderPotentiallyModifiedFnPtrTypeLocs();
  llvm::outs() << ",\n";
  llvm::outs() << "\"var_decl_fn_ptr_arg_lparen_locs\": ";
  Callback.emitJSONDictForVarDeclFnPtrArgLParenLocs();
  llvm::outs() << ",\n";
  llvm::outs() << "\"globals_without_initializers\": ";
  Callback.emitJSONListOfGlobalsWithoutInitializers();
  llvm::outs() << "}";
}
