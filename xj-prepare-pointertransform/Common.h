// Common.h — shared vocabulary for the pointer-to-index transformation tool.
//
// This header defines the data structures every other component reads or
// writes:
//   - PointerAccessKind: the classification of a single pointer use
//   - PointerAccess:    one classified use of a pointer
//   - FunctionAnalysis: per-function snapshot saved from run() to use in
//                       onEndOfTranslationUnit()
//   - Edit:             one pending source-text rewrite
//   - Globals (extern): cross-phase state (per-function analyses, logs,
//                       metadata, etc.). Defined in Common.cpp.

#pragma once

#include "PtrIndexMetadata.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/ParentMapContext.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/Type.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Rewrite/Core/Rewriter.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/Support/CommandLine.h"

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace clang::tooling;
using namespace llvm;
using namespace clang;
using namespace clang::ast_matchers;

// Compile-time flag for chatty per-pointer trace output. Leave off in
// normal builds; flip to true when debugging classification logic.
inline constexpr bool VERBOSE = false;

// ============================================================================
// PointerAccessKind — every way a tracked pointer can appear in the source.
// ============================================================================
//
// Each DeclRefExpr to a tracked pointer is classified into exactly one of
// these kinds.
enum class PointerAccessKind
{
    // --- Element access: the index selects an element of the base ---------
    Element, // *p                        -> p[p_index_xj]
             // *p++                      -> p[p_index_xj++]
             // *(p + n)                  -> p[p_index_xj + n]
             // p[i]                      -> p[p_index_xj + i]
             // p->field                  -> p[p_index_xj].field

    // --- Position: the index moves, the base stays put --------------------
    Move, // p++ / ++p                 -> p_index_xj++ / ++p_index_xj
          // p += n / p -= n           -> p_index_xj += n / -= n

    // --- (base, index) assignment -----------------------------------------
    // The RHS is split syntactically into a root and an offset; see
    // PointerAccess::root_expr. `int *q = p + 1;` is `q = p` paired with
    // `q_index_xj = p_index_xj + 1`.
    Init,   // T *p = RHS;
    Assign, // p = RHS

    // A null right-hand side reseats the region to the null region and
    // drives the index to the -1 sentinel.
    InitNull,   // T *p = NULL;              -> (p = NULL, p_index_xj = -1)
    AssignNull, // p = NULL                  -> (p = NULL, p_index_xj = -1)

    // p = strchr(p, c) — the region is unchanged, so only the index moves.
    AssignFromAllowedFunc, // p = strchr(...)
                           //   -> p_index_xj = strchr_index_xj(p, p_index_xj, ...)

    // --- Reads of the pointer's value -------------------------------------
    ValueUse, // f(p), return p, p < end, p - buf, (char *)p, ...
              //                           -> (p + p_index_xj)
    NullTest, // if (p), !p, p == NULL, p && q — left as written,
              // since the region is null exactly when the pointer
              // is, unless the sentinel can sit over a live
              // region; see PointerFacts::null_in_index.
    NoEdit,   // sizeof p — the value is never read.

    // --- Rejected ------------------------------------------------------------
    AddressOf, // &p — the pointer's storage is observable, so it
               // cannot carry a base while an index carries the
               // position.
    Unknown    // no parent at all; nothing to anchor an edit to
};

// One term of a pointer-arithmetic offset: `p + a - b` has terms `a` and
// `b`, the second flagged `minus`.
struct OffsetTerm
{
    const Expr *expr = nullptr;
    bool minus = false;
};

// How an index is stepped as it is read: the `++` or `--` on a pointer,
// carried over to the index that now holds its position.
enum class IndexStep
{
    None,
    PostInc,
    PreInc,
    PostDec,
    PreDec
};

// The base a pointer-valued expression starts from and the offset, in
// elements, that it lands at: `base(e)` and `offset(e)` for the `q = e` rule,
// so that `q = e` becomes `q = base(e); q_index_xj = offset(e)`.
//
// This is used by the edit plan to split the expression across
// a pointer base assignment and index assignment
struct PointerSplit
{
    const Expr *base = nullptr;
    std::vector<OffsetTerm> terms;
    IndexStep step = IndexStep::None;
};

// One classified use of a tracked pointer. The combination of `kind` and
// the populated fields tells the rewriter exactly what edit to produce;
// unused fields are left empty.
struct PointerAccess
{
    PointerAccessKind kind;
    SourceLocation loc;
    const Expr *expr = nullptr; // the DeclRefExpr (or, for Init, the initializer)

    // The node the rewrite replaces, where that is not `expr` itself:
    //   Element   the dereference, subscript or member access
    //   Move      the `p++`, or the `p += n`
    //   Assign    the assignment
    //   NullTest  the comparison, for `p == NULL` and `p != NULL`
    const Stmt *enclosing_stmt = nullptr;

    // The `++` or `--` applied as the position is read, and whose index it
    // moves:
    //   Element, Move   this pointer's own — `*p++`, `p++`
    //   Init, Assign    the root's — `q = p++`
    IndexStep step = IndexStep::None;

    // Element: the arithmetic after the pointer's name — `*(p + a - b)`
    // becomes `p[p_index_xj + a - b]`.
    std::vector<OffsetTerm> offset_terms;

    // Init / Assign: the terms lifted out of the right-hand side and into
    // the index — `q = p + 1` becomes `q_index_xj = p_index_xj + 1`. Empty
    // unless the split was taken.
    std::vector<OffsetTerm> index_terms;

    const Expr *subscript_expr = nullptr; // Element: the `i` of `p[i]`

    // The member's name, not source text: it is an identifier the AST
    // supplies, so nothing can be nested inside it to lose.
    std::string field_name; // Element: the `field` of `p->field`

    // Init / Assign. `rhs_expr` is the whole right-hand side; `root_expr` is
    // the sub-expression that becomes the new base, or null when the RHS is
    // taken whole and the index starts at 0. When they differ the split was
    // taken; see isSplit().
    //
    // AssignFromAllowedFunc sets `root_expr` alone: the region searched,
    // which is the base the assigned pointer is reseated to.
    const Expr *rhs_expr = nullptr;
    const Expr *root_expr = nullptr;

    // Init / Assign: true when the split was taken. The rewriter then
    // replaces the right-hand side with its root, and `index_terms` and
    // `step` hold what that dropped — at least one of them is set.
    //
    // This is what the syntax offers. A stepped root needs an index to step,
    // so whether the split is *used* waits until it is known which pointers
    // are rewritten; see EditPlan::splitStands.
    bool isSplit() const
    {
        return root_expr && rhs_expr &&
               root_expr != rhs_expr->IgnoreParenImpCasts();
    }
};

// ============================================================================
// Logging and per-pointer status
// ============================================================================

// Per-file rollup behind the [SUMMARY] line.
struct TransformationLog
{
    bool foundPointer = false;
    bool replacedPointer = false;
    std::string error = "";
};

// Detail row for a pointer that was rejected, printed as [FAILED] ...
struct FailedPointerLog
{
    std::string varName;
    unsigned line;
    unsigned col;
    std::string error;
};

// Detail row for a pointer that was successfully rewritten, printed as
// [REPLACED] ...
struct SucceededPointerLog
{
    std::string varName;
    std::string funcName;
    unsigned line;
    unsigned col;
};

// ============================================================================
// FunctionAnalysis — per-function snapshot saved during run()
// ============================================================================
//
// run() is called per FunctionDecl, but most rewriting happens later in
// onEndOfTranslationUnit() once we know the full set of transformable
// functions. We capture each function's collected pointer data here so
// later passes can look it up in g_function_analyses.

struct FunctionAnalysis
{
    const FunctionDecl *FD = nullptr;
    // Every local and parameter pointer the function declares, with its
    // accesses in the order they were visited.
    std::map<const VarDecl *, std::vector<PointerAccess>> accesses;
};

// ============================================================================
// Global state (defined in Common.cpp)
// ============================================================================
//
// The tool intentionally keeps cross-phase state in globals because
// analysis is snapshotted per function during run() and consumed at
// end-of-TU.

extern int g_pointers_found;
extern int g_pointers_replaced;

// Count of broken edit-plan invariants seen across the whole run,
// which indicate a bug.
extern int g_invariant_violations;
extern TransformationLog gLog;
extern std::vector<FailedPointerLog> g_failed_pointers;
extern std::vector<SucceededPointerLog> g_succeeded_pointers;
extern DeclarationMatcher FunctionMatcher; // matches every function definition
extern bool g_inplace;                     // --inplace CLI flag
extern bool g_verbose;                     // --verbose CLI flag

// File-scope pointers found in this TU, each with its accesses from every
// function. Collected once per TU, separately from per-function locals.
extern std::map<const VarDecl *, std::vector<PointerAccess>> g_global_pointer_map;

// Library functions whose return values we know how to turn into an
// index (see AssignFromAllowedFunc). Every name here must have a wrapper
// body in wrapperBodyFor(), or a rewritten call site would name a wrapper
// that is never emitted.
extern std::set<std::string> g_allowed_funcs;

// Per-function analysis snapshots saved during run() for later phases.
extern std::map<const FunctionDecl *, FunctionAnalysis> g_function_analyses;

// Metadata accumulated across every TU in this run, written to
// g_metadata_out (if set) after the last file is processed. Consumed by
// xj-prepare-baserewrite, then by xj-prepare-slicetransform.
extern xj::PtrIndexMetadata g_metadata;
extern std::string g_metadata_out; // --metadata-out CLI flag ("" = don't write)

// A request to fill in the `decl_line` and `decl_col` fields of one
// PtrIndexPointerRecord in g_metadata after the translation unit has been
// fully rewritten.
//
// `function_key` and `pointer_index` identify the record, as
// g_metadata.functions[function_key].pointers[pointer_index].
//
// `file` and `offset` give the position of the pointer's declaring
// identifier in the original source text.
// PointerTransformAction::resolveDeclLocations uses this to set `decl_line`
// and `decl_col` once every edit in the translation unit is in place.
struct PendingDeclLoc
{
    std::string function_key; // key into g_metadata.functions
    size_t pointer_index;     // index into that function record's `pointers`
    FileID file;              // file containing the declaring identifier
    unsigned offset;          // its offset in the original text of that file
};

// Cleared per TU: a record from an earlier file must not be re-mapped
// through this file's Rewriter.
extern std::vector<PendingDeclLoc> g_pending_decl_locs;

// ============================================================================
// Edit — one pending source-text rewrite
// ============================================================================
//
// emitIndexDecl and EditPlan::appendRootEdits build one vector<Edit> per
// translation unit, and applyEdits() applies it in reverse-offset order so
// earlier offsets stay stable. `offset` is the file offset used purely
// for sorting; `start`/`end` are the actual SourceLocations passed to
// the Rewriter.

struct Edit
{
    enum Type
    {
        Replace,
        InsertBefore,
        InsertAfterToken
    };
    Type type;
    unsigned offset;
    SourceLocation start;
    SourceLocation end; // only used for Replace
    std::string text;
};

// ============================================================================
// Index declaration placement
// ============================================================================
//
// Where one pointer's companion index is declared.
struct IndexDeclSite
{
    SourceLocation at;       // insert the declaration before this position
    std::string prefix;      // text ahead of the declaration
    std::string suffix;      // text after it
    SourceLocation brace_at; // a for-init hoist that had to wrap its loop
    std::string brace_text;  //   closes the block after this token
};

// Locate a home for `PtrVar`'s index. False when there is none; such a
// pointer is not rewritten.
bool findIndexDeclSite(const FunctionDecl *FD, const VarDecl *PtrVar,
                       ASTContext &Ctx, IndexDeclSite &site);

// Append the edits that write the declaration at `site`. `index_init` is
// the index's starting value, rendered by the edit plan — see
// EditPlan::indexDeclInit.
void emitIndexDecl(const IndexDeclSite &site, const VarDecl *PtrVar,
                   const std::string &index_init, const SourceManager &SM,
                   std::vector<Edit> &edits);

// ============================================================================
// AST helpers
// ============================================================================
//
// findEnclosingStmt<T>(node) walks up the AST parent chain from `node`
// and returns the first ancestor that is a `T` (e.g. CompoundStmt,
// ForStmt). Three overloads cover Decl / Stmt / Expr starting points.
// Returns nullptr if no such ancestor exists.

template <typename T>
const T *findEnclosingStmt(const Decl *D, ASTContext &Ctx)
{
    for (DynTypedNode parentNode : Ctx.getParents(*D))
    {
        if (const Stmt *stmtParent = parentNode.get<Stmt>())
        {
            const T *result = nullptr;
            const Stmt *current = stmtParent;
            while (current)
            {
                if ((result = dyn_cast<T>(current)))
                    return result;
                auto grandparents = Ctx.getParents(*current);
                if (grandparents.empty())
                    break;
                current = grandparents[0].get<Stmt>();
            }
        }
    }
    return nullptr;
}

template <typename T>
const T *findEnclosingStmt(const Stmt *S, ASTContext &Ctx)
{
    const Stmt *Current = S;
    while (Current)
    {
        auto Parents = Ctx.getParents(*Current);
        if (Parents.empty())
            break;
        const Stmt *ParentStmt = Parents[0].get<Stmt>();
        if (!ParentStmt)
            break;
        if (const T *Target = dyn_cast<T>(ParentStmt))
            return Target;
        Current = ParentStmt;
    }
    return nullptr;
}

template <typename T>
const T *findEnclosingStmt(const Expr *E, ASTContext &Ctx)
{
    llvm::SmallVector<clang::DynTypedNode, 8> Worklist;
    for (const DynTypedNode &ParentNode : Ctx.getParents(*E))
    {
        Worklist.push_back(ParentNode);
    }
    while (!Worklist.empty())
    {
        const DynTypedNode Node = Worklist.pop_back_val();
        if (const Stmt *S = Node.get<Stmt>())
        {
            if (const T *Target = dyn_cast<T>(S))
                return Target;
            for (const DynTypedNode &P : Ctx.getParents(*S))
                Worklist.push_back(P);
        }
        else if (const Decl *D = Node.get<Decl>())
        {
            for (const DynTypedNode &P : Ctx.getParents(*D))
                Worklist.push_back(P);
        }
    }
    return nullptr;
}

// Step up to the first parent that isn't an ImplicitCastExpr or
// ParenExpr — Clang inserts both routinely and they would otherwise
// hide the "real" syntactic context the classifier wants to see
// (e.g. *p sitting inside a UnaryOperator parent).
inline const Stmt *skipTransparentParents(const Stmt *S, ASTContext &Ctx)
{
    const Stmt *Current = S;
    while (true)
    {
        auto Parents = Ctx.getParents(*Current);
        if (Parents.empty())
            return nullptr;
        const Stmt *P = Parents[0].get<Stmt>();
        if (!P)
            return nullptr;
        if (isa<ImplicitCastExpr>(P) || isa<ParenExpr>(P))
        {
            Current = P;
            continue;
        }
        return P;
    }
}

// ============================================================================
// Free helpers (defined in Common.cpp)
// ============================================================================

// The DeclStmt that declares `VD`, or null when it has none: a parameter
// or a file-scope variable.
const DeclStmt *declStmtOf(const VarDecl *VD, ASTContext &Ctx);

// True if `S` names any of `decls` anywhere in its subtree.
bool referencesAnyOf(const Stmt *S, const std::set<const Decl *> &decls);

// The ForStmt whose init clause is `DS`, or null when `DS` is an ordinary
// statement-level declaration.
//
// The distinction matters wherever a companion declaration is emitted
// alongside `DS`: a for-init has no position *after* it that accepts a
// statement — that slot is the loop condition — so the companion has to be
// placed before the whole loop instead.
const ForStmt *forStmtInitializedBy(const DeclStmt *DS, ASTContext &Ctx);

// True if `DS` introduces more than one entity: `int *p = buf, *q = buf + 1;`.
// Such a declaration cannot be replaced wholesale by one pointer's rewrite,
// since the other declarators have to survive.
bool isMultiDeclarator(const DeclStmt *DS);

// Return the leading whitespace (spaces/tabs) on the line containing
// `Loc`, so that an emitted index declaration lines up with its anchor.
llvm::StringRef getIndentBeforeLoc(SourceLocation Loc, const SourceManager &SM);

// Lex back the original source text for a range / expression. Cheaper
// and more faithful than pretty-printing the AST node.
std::string getSourceText(SourceRange Range, const SourceManager &SM, const LangOptions &LO);
std::string getSourceText(const Expr *E, const SourceManager &SM, const LangOptions &LO);

// Debug helper: stringify a PointerAccessKind for trace logs.
const char *pointerAccessKindToString(PointerAccessKind kind);

// `name` stepped as `step` says: p++, ++p, p--, --p.
std::string applyStep(IndexStep step, const std::string &name);

// ============================================================================
// Index variable naming
// ============================================================================
//
// Every rewritten pointer gets a companion index variable. The name is
// assigned once, up front, rather than derived at each use site, because
// an index does not always share its pointer's scope: a pointer declared
// in a multi-declarator for-init has its index placed before the whole
// loop, where it outlives the pointer. Two same-named pointers in sibling
// loops would then put two identically-named indices in one block — a
// redefinition, or worse a silent resolution to the wrong one.
//
// assignIndexNames() takes one function's pointers in source order and
// hands out `p_index_xj`, then `p_index_xj_1`, `p_index_xj_2`, ... on
// collision. The first pointer of a given name keeps the plain form. The
// file-scope pointers in
// g_global_pointer_map have their names first, so a local that shares a
// name with one of them starts at `p_index_xj_1`.
void assignIndexNames(const std::vector<const VarDecl *> &ptrs);

// The index name for `VD`. Falls back to the plain convention for
// pointers that never went through assignIndexNames (file-scope ones,
// which are rewritten on their own path).
const std::string &indexNameFor(const VarDecl *VD);

// Forget every name. They are keyed by declarations that do not outlive
// their translation unit.
void resetIndexNames();
