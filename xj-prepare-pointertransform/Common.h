// Common.h — shared vocabulary for the pointer-to-index transformation tool.
//
// This header defines the data structures every other component reads or
// writes:
//   - PointerAccessKind: the classification of a single pointer use
//   - PointerCandidate: per-pointer metadata (base array, offset bounds, ...)
//   - PointerAccess:    one classified use of a pointer
//   - FunctionAnalysis: per-function snapshot saved from run() to use in
//                       onEndOfTranslationUnit()
//   - Edit:             one pending source-text rewrite
//   - Globals (extern): cross-phase state (transformed functions, emitted
//                       wrappers/typedefs, etc.). Defined in Common.cpp.

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
// these kinds by walking the AST parent chain (see
// PointerAccessCollector::classifyAccess). The rewrite is *total*: the
// pointer variable is its own base and is never deleted, so there is no
// syntactic context that has to be rejected except `&p`.
//
// Four buckets, and each bucket has one rule:
//
//   element access   -> p[p_index_xj + ...]     the index names the element
//   position         -> p_index_xj moves        the base never does
//   (base, index)    -> (p = ROOT, p_index_xj = OFF)
//   value read       -> (p + p_index_xj)        rebuild the pointer in place
//
// The value read is the fallback, and it is what makes the rewrite total:
// any use of the pointer's value that is not one of the other three
// buckets is still expressible.

enum class PointerAccessKind {
    // --- Element access: the index selects an element of the base ---------
    Deref,              // *p                        -> p[p_index_xj]
    DerefWrite,         // *p = v                    -> p[p_index_xj] = v
    DerefPostInc,       // *p++                      -> p[p_index_xj++]
    DerefPreInc,        // *++p                      -> p[++p_index_xj]
    DerefPostDec,       // *p--                      -> p[p_index_xj--]
    DerefPreDec,        // *--p                      -> p[--p_index_xj]
    DerefOffset,        // *(p + n)                  -> p[p_index_xj + n]
    DerefOffsetWrite,   // *(p + n) = v              -> p[p_index_xj + n] = v
    ArrowAccess,        // p->field                  -> p[p_index_xj].field
    ArrowWrite,         // p->field = v              -> p[p_index_xj].field = v
    Subscript,          // p[i]                      -> p[p_index_xj + i]
    SubscriptWrite,     // p[i] = v                  -> p[p_index_xj + i] = v

    // --- Position: the index moves, the base stays put --------------------
    Increment,          // p++ / ++p                 -> p_index_xj++ / ++p_index_xj
    Decrement,          // p-- / --p                 -> p_index_xj-- / --p_index_xj
    PlusAssign,         // p += n                    -> p_index_xj += n
    MinusAssign,        // p -= n                    -> p_index_xj -= n

    // --- (base, index) assignment -----------------------------------------
    // The RHS is split syntactically into a root and an offset; see
    // PointerAccess::root_expr. `int *q = p + 1;` is `q = p` paired with
    // `q_index_xj = p_index_xj + 1` — no special "inheritance" rule, just
    // the ordinary assignment.
    Init,               // T *p = RHS;
    Assign,             // p = RHS

    // A null right-hand side reseats the region to the null region and
    // drives the index to the -1 sentinel. Both are ordinary (base, index)
    // assignments in every other respect — these kinds exist so
    // pointerMayBeNull can recognize them.
    InitNull,           // T *p = NULL;              -> (p = NULL, p_index_xj = -1)
    AssignNull,         // p = NULL                  -> (p = NULL, p_index_xj = -1)

    // p = strchr(p, c) — the region is unchanged, so only the index moves.
    // The generated wrapper returns -1 for "not found", which is the one
    // way an index goes negative while the region stays non-null.
    AssignFromAllowedFunc,  // p = strchr(...)
                        //   -> p_index_xj = strchr_index_xj(p, p_index_xj, ...)

    // --- Reads of the pointer's value -------------------------------------
    ValueUse,           // f(p), return p, p < end, p - buf, (char *)p, ...
                        //                           -> (p + p_index_xj)
    PairwiseRoot,       // this reference *is* the root of another tracked
                        // pointer's Init/Assign RHS, whose rewrite carries
                        // the pair. No edit of its own.
    NullTest,           // if (p), !p, p == NULL, p && q — the retained
                        // pointer is null exactly when it was before, so
                        // these are left alone.
    NoEdit,             // sizeof p — the value is never read.

    // --- The only rejection ------------------------------------------------
    AddressOf,          // &p — the pointer's storage is observable, so it
                        // cannot carry a base while an index carries the
                        // position.
    Unknown             // no parent at all; nothing to anchor an edit to
};

// One pointer the tool is considering rewriting. The pointer variable is
// its own base, so there is nothing here about *what* the base is — that
// question belongs to base resolution, which runs on this tool's output.
struct PointerCandidate {
    const VarDecl *ptr_var = nullptr;
    bool is_parameter = false;      // true if this pointer is a function parameter
};

// One term of a pointer-arithmetic offset: `p + a - b` has terms `a` and
// `b`, the second flagged `minus`.
//
// The terms are kept as expressions rather than as one string because a
// term may itself contain a reference this pass has to rewrite, and only
// a node can be addressed by the edit plan. Slicing the offset out of the
// chain's source text — the shape this replaced — could neither do that
// nor survive a chain whose pointer is not its leftmost leaf.
struct OffsetTerm {
    const Expr *expr = nullptr;
    bool minus = false;
};

// One classified use of a tracked pointer. The combination of `kind` and
// the populated fields tells the rewriter exactly what edit to produce;
// unused fields are left empty.
//
// There are two kinds of field here and the difference is load-bearing.
// The *expression* fields are what a rewrite is built from: whatever they
// name is rendered by the edit plan, so a rewrite nested inside one is
// carried along instead of pasted over. The *text* fields are spellings
// snapshotted at classification time; no rewrite is emitted from them.
struct PointerAccess {
    PointerAccessKind kind;
    SourceLocation loc;
    const Expr *expr = nullptr;    // the DeclRefExpr (or, for Init, the initializer)
    const Stmt *enclosing_stmt = nullptr;  // Assign: the BinaryOperator;
                                           // DerefOffset: the UO_Deref node

    // DerefOffset / DerefOffsetWrite: the arithmetic after the pointer's
    // name — `*(p + a - b)` becomes `p[p_index_xj + a - b]`.
    std::vector<OffsetTerm> offset_terms;

    // Init / Assign: the terms lifted out of the right-hand side and into
    // the index — `q = p + 1` becomes `q_index_xj = p_index_xj + 1`. Empty
    // unless the split was taken.
    std::vector<OffsetTerm> index_terms;

    const Expr *subscript_expr = nullptr;  // Subscript / SubscriptWrite: the index

    // The member's name, not source text: it is an identifier the AST
    // supplies, so nothing can be nested inside it to lose.
    std::string field_name;        // ArrowAccess / ArrowWrite

    // ---- Spellings, for the verbose access dump --------------------------
    //
    // One of these is read for more than logging: validation asks whether
    // `offset_text` is something other than "0" to tell an Init or Assign
    // that lands at an offset from one that does not, which is part of
    // deciding whether the pointer is worth an index at all.
    std::string offset_text;       // DerefOffset: the arithmetic after the name.
                                   // Init/Assign: the whole index expression
                                   // ("0", "3", "0 + 1 - 2").
    std::string subscript_text;    // Subscript / SubscriptWrite
    std::string operand_text;      // PlusAssign / MinusAssign: the RHS.
                                   // Init/Assign: the remainder after the
                                   // root's index ("", " + 1 - 2").

    // Init / Assign only. `rhs_expr` is the whole right-hand side;
    // `root_expr` is the sub-expression that becomes the new base, or null
    // when the RHS is taken whole and the index starts at 0. When they
    // differ the split was taken: the rewriter replaces the RHS range with
    // the root, and `index_terms` holds what it dropped.
    const Expr *rhs_expr = nullptr;
    const Expr *root_expr = nullptr;

    // PairwiseRoot only: the pointer whose assignment carries this
    // reference. If that pointer turns out not to be rewritten, the
    // reference is demoted to an ordinary value read — nothing else
    // would restore the position the root used to hold.
    const VarDecl *pair_owner = nullptr;
};

// ============================================================================
// Logging and per-pointer status
// ============================================================================

// One-pointer-per-file rollup used to print the per-file [SUMMARY] line.
struct TransformationLog {
    bool foundPointer = false;
    bool replacedPointer = false;
    std::string error = "";
};

// Detail row for a pointer that was rejected, printed as [FAILED] ...
struct FailedPointerLog {
    std::string varName;
    unsigned line;
    unsigned col;
    std::string error;
};

// Detail row for a pointer that was successfully rewritten, printed as
// [REPLACED] ...
struct SucceededPointerLog {
    std::string varName;
    std::string funcName;
    unsigned line;
    unsigned col;
};

// ============================================================================
// Global (file-scope) pointer tracking
// ============================================================================

// File-scope pointer variables are collected once per TU into
// g_global_pointer_map and transformed separately from local pointers.
struct GlobalPointerState {
    PointerCandidate candidate;
    std::vector<PointerAccess> accesses;
};

// ============================================================================
// FunctionAnalysis — per-function snapshot saved during run()
// ============================================================================
//
// run() is called per FunctionDecl, but most rewriting happens later in
// onEndOfTranslationUnit() once we know the full set of transformable
// functions. We capture each function's collected pointer data here so
// later passes can look it up in g_function_analyses.

struct FunctionAnalysis {
    const FunctionDecl *FD = nullptr;
    std::map<const VarDecl *, PointerCandidate> tracked_pointers;
    std::map<const VarDecl *, std::vector<PointerAccess>> accesses;
};

// ============================================================================
// Global state (defined in Common.cpp)
// ============================================================================
//
// The tool intentionally keeps cross-phase state in globals because
// analysis is snapshotted per function during run() and consumed at
// end-of-TU, and emission needs to dedupe across functions (one
// strchr_index wrapper per TU).

extern int g_pointers_found;
extern int g_pointers_replaced;

// Count of broken edit-plan invariants seen across the whole run. Any hit
// is a bug in this tool, not in the input, so it is reported and the run
// exits non-zero rather than writing C whose meaning we cannot vouch for.
// Deliberately not reset per file.
extern int g_invariant_violations;
extern TransformationLog gLog;
extern std::vector<FailedPointerLog> g_failed_pointers;
extern std::vector<SucceededPointerLog> g_succeeded_pointers;
extern DeclarationMatcher FunctionMatcher;     // matches every function definition
extern bool g_inplace;                         // --inplace CLI flag
extern bool g_verbose;                         // --verbose CLI flag

// File-scope pointers found in this TU (separate from per-function locals).
extern std::map<const VarDecl *, GlobalPointerState> g_global_pointer_map;

// Library functions whose return values we know how to turn into an
// index (see AssignFromAllowedFunc). Every name here must have a wrapper
// body in wrapperBodyFor(), or a rewritten call site would name a wrapper
// that is never emitted.
extern std::set<std::string> g_allowed_funcs;

// Names of _index wrappers already emitted (e.g. "strchr_index_xj"). Used
// to make wrapper emission idempotent across the TU.
extern std::set<std::string> g_emitted_wrappers;

// Per-function analysis snapshots saved during run() for later phases.
extern std::map<const FunctionDecl *, FunctionAnalysis> g_function_analyses;

// Metadata accumulated across every TU in this run, written to
// g_metadata_out (if set) after the last file is processed. Consumed by
// xj-prepare-slicetransform.
extern xj::PtrIndexMetadata g_metadata;
extern std::string g_metadata_out; // --metadata-out CLI flag ("" = don't write)

// One deferred decl-position stamp: where a recorded pointer's declaring
// identifier sits in the *input* buffer, and which record wants its
// position in the *output*.
//
// The stamp cannot be applied where the record is built, because pointers
// earlier in source order have not been rewritten yet at that point; it
// happens once per TU in PointerTransformAction::EndSourceFileAction, with
// every edit in place. The record is named indirectly — a raw
// PtrIndexPointerRecord* would dangle as soon as the vector grew.
struct PendingDeclLoc {
    std::string function_key; // key into g_metadata.functions
    size_t pointer_index;     // index into that record's `pointers`
    FileID file;              // spelling file of the identifier
    unsigned offset;          // spelling offset of it, pre-rewrite
};

// Cleared per TU: a record from an earlier file must not be re-mapped
// through this file's Rewriter.
extern std::vector<PendingDeclLoc> g_pending_decl_locs;

// ============================================================================
// Edit — one pending source-text rewrite
// ============================================================================
//
// Transformation methods build a vector<Edit> per pointer (or per
// function) and applyEdits() applies them in reverse-offset order so
// earlier offsets stay stable. `offset` is the file offset used purely
// for sorting; `start`/`end` are the actual SourceLocations passed to
// the Rewriter.

struct Edit {
    enum Type { Replace, InsertBefore, InsertAfterToken };
    Type type;
    unsigned offset;
    SourceLocation start;
    SourceLocation end;  // only used for Replace
    std::string text;
};

// ============================================================================
// Index declaration placement (TransformationMethods.cpp)
// ============================================================================
//
// Where one pointer's companion index is declared. Finding the position is
// separated from writing the declaration because a pointer with nowhere to
// put its index is not rewritten at all, and that has to be known before
// any other pointer's index is allowed to name this one.

struct IndexDeclSite {
    bool valid = false;
    SourceLocation at;         // insert the declaration before this position
    std::string prefix;        // text ahead of the declaration
    std::string suffix;        // text after it
    SourceLocation brace_at;   // a for-init hoist that had to wrap its loop
    std::string brace_text;    //   closes the block after this token
};

// Locate a home for `PtrVar`'s index. False when there is none, which is
// the one remaining reason a validated pointer is left alone.
bool findIndexDeclSite(const FunctionDecl *FD, const VarDecl *PtrVar,
                       const PointerCandidate &candidate, ASTContext &Ctx,
                       IndexDeclSite &site);

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
const T *findEnclosingStmt(const Decl *D, ASTContext &Ctx) {
    for (DynTypedNode parentNode : Ctx.getParents(*D)) {
        if (const Stmt *stmtParent = parentNode.get<Stmt>()) {
            const T *result = nullptr;
            const Stmt *current = stmtParent;
            while (current) {
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
const T *findEnclosingStmt(const Stmt *S, ASTContext &Ctx) {
    const Stmt *Current = S;
    while (Current) {
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
const T *findEnclosingStmt(const Expr *E, ASTContext &Ctx) {
    llvm::SmallVector<clang::DynTypedNode, 8> Worklist;
    for (const DynTypedNode &ParentNode : Ctx.getParents(*E)) {
        Worklist.push_back(ParentNode);
    }
    while (!Worklist.empty()) {
        const DynTypedNode Node = Worklist.pop_back_val();
        if (const Stmt *S = Node.get<Stmt>()) {
            if (const T *Target = dyn_cast<T>(S))
                return Target;
            for (const DynTypedNode &P : Ctx.getParents(*S))
                Worklist.push_back(P);
        } else if (const Decl *D = Node.get<Decl>()) {
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
inline const Stmt *skipTransparentParents(const Stmt *S, ASTContext &Ctx) {
    const Stmt *Current = S;
    while (true) {
        auto Parents = Ctx.getParents(*Current);
        if (Parents.empty())
            return nullptr;
        const Stmt *P = Parents[0].get<Stmt>();
        if (!P)
            return nullptr;
        if (isa<ImplicitCastExpr>(P) || isa<ParenExpr>(P)) {
            Current = P;
            continue;
        }
        return P;
    }
}

// ============================================================================
// Free helpers (defined in Common.cpp)
// ============================================================================

// Find the DeclStmt that introduces `VD` inside `FunctionBody`. Used to
// position rewrites at the variable's declaration line.
const DeclStmt *findDeclStmtForVar(const VarDecl *VD, Stmt *FunctionBody);

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
// `Loc`. Used to indent emitted code (wrappers, typedefs) consistently.
llvm::StringRef getIndentBeforeLoc(SourceLocation Loc, const SourceManager &SM);

// Lex back the original source text for a range / expression. Cheaper
// and more faithful than pretty-printing the AST node.
std::string getSourceText(SourceRange Range, const SourceManager &SM, const LangOptions &LO);
std::string getSourceText(const Expr *E, const SourceManager &SM, const LangOptions &LO);

// Debug helper: stringify a PointerAccessKind for trace logs.
const char *pointerAccessKindToString(PointerAccessKind kind);

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
// collision. The first pointer of a given name keeps the plain form, so
// the common case reads exactly as before.
void assignIndexNames(const std::vector<const VarDecl *> &ptrs);

// The index name for `VD`. Falls back to the plain convention for
// pointers that never went through assignIndexNames (file-scope ones,
// which are rewritten on their own path).
const std::string &indexNameFor(const VarDecl *VD);
