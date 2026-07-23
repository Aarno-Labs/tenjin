// Common.h — shared vocabulary for the pointer-to-index transformation tool.
//
// This header defines the data structures every other component reads or
// writes:
//   - PointerAccessKind: the classification of a single pointer use
//   - PointerCandidate: per-pointer metadata (base array, offset bounds, ...)
//   - PointerAccess:    one classified use of a pointer
//   - RustSliceInfo:    how a function was reshaped into a RustSlice signature
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
// Each DeclRefExpr to a tracked pointer is classified into exactly one
// of these kinds by walking the AST parent chain (see
// PointerAccessCollector::classifyAccess). The kind drives both
// validation (some kinds disqualify the pointer) and rewriting (each
// kind has a corresponding case in TransformationMethods.cpp).

enum class PointerAccessKind {
    // --- Initialization (at the declaration site) -------------------------
    InitNull,           // int *p = NULL;            → int p_index = -1;
    InitArray,          // int *p = arr;             → int p_index = 0;
    InitArrayOffset,    // int *p = arr + n;         → int p_index = n;

    // --- Reassignment after declaration -----------------------------------
    AssignNull,         // p = NULL;                 → p_index = -1;
    AssignAddrOf,       // p = &arr[i];              → p_index = i;
    AssignArray,        // p = arr;                  → p_index = 0;
    AssignArrayOffset,  // p = arr + n;              → p_index = n;

    // --- Pointer arithmetic on the pointer itself -------------------------
    Increment,          // p++ / ++p                 → p_index++ / ++p_index
    Decrement,          // p-- / --p                 → p_index-- / --p_index
    PlusAssign,         // p += n                    → p_index += n
    MinusAssign,        // p -= n                    → p_index -= n

    // --- Dereference (read) -----------------------------------------------
    Deref,              // *p                        → arr[p_index]
    DerefPostInc,       // *p++                      → arr[p_index++]
    DerefPreInc,        // *++p                      → arr[++p_index]
    DerefPostDec,       // *p--                      → arr[p_index--]
    DerefPreDec,        // *--p                      → arr[--p_index]

    // --- Structured access ------------------------------------------------
    ArrowAccess,        // p->field                  → arr[p_index].field
    Subscript,          // p[i]                      → arr[p_index + i]

    // --- Dereference with an offset expression ----------------------------
    DerefOffset,        // *(p + n) / *(p - n)       → arr[p_index ± n]
    DerefOffsetWrite,   // *(p + n) = v              → arr[p_index ± n] = v

    // --- Writes through the pointer ---------------------------------------
    DerefWrite,         // *p = v                    → arr[p_index] = v
    ArrowWrite,         // p->field = v              → arr[p_index].field = v
    SubscriptWrite,     // p[i] = v                  → arr[p_index + i] = v

    // --- Comparison -------------------------------------------------------
    Comparison,         // any comparison we can't resolve to an index form
                        // (causes rejection)
    ComparisonNull,     // p == NULL / p != NULL     → p_index == -1 / != -1
    ComparisonExpr,     // p < arr + n / p < end     → p_index < n / < (end - arr)

    // --- Implicit boolean (pointer used as a truth value) -----------------
    BoolTrue,           // if (p), while (p), p && ..., p ? : ...
                        //                           → p_index != -1
    BoolFalse,          // !p                        → p_index == -1

    // --- Calls to allowlisted library functions (g_allowed_funcs) ---------
    PassedToAllowedFunc,    // strchr(p, c), sscanf(p, ...) — argument is
                            // rebuilt as (base + p_index)
    AssignFromAllowedFunc,  // p = strchr(...) — generates an _index wrapper

    // --- Return value -----------------------------------------------------
    ReturnPtr,              // return p → return base + p_index
                            // (or return p_index if the function's return
                            // type was rewritten to int)

    // --- Escape / rejection triggers --------------------------------------
    AddressOf,              // &p — pointer's identity matters; reject
    PassedToFunc,           // pointer passed to an unknown function; if the
                            // callee is RustSlice-transformed the call site
                            // is rewritten later, otherwise the argument
                            // becomes (base + p_index)
    Unknown                 // any pattern we don't recognize — reject
};

// One pointer the tool is considering rewriting. Populated by
// PointerAccessCollector and refined as more accesses are classified.
struct PointerCandidate {
    const VarDecl *ptr_var;
    const Expr *base_array;        // AST node the base array was extracted from
    std::string base_array_text;   // source text of the base, e.g. "users", "bs->buf"
    bool is_parameter;             // true if this pointer is a function parameter

    // Lookback / lookahead bounds, computed from constant *(p ± k) accesses.
    // Used by the RustSlice transform to extend slice bounds at call sites.
    int min_relative_offset = 0;   // most-negative constant offset seen (e.g. -1)
    int max_relative_offset = 0;   // most-positive constant offset seen (e.g. +2)
    bool constant_offsets = true;  // false if any *(p + variable) was seen → reject
};

// One classified use of a tracked pointer. The combination of `kind` and
// the populated string fields tells the rewriter exactly what edit to
// produce; unused fields are left empty.
struct PointerAccess {
    PointerAccessKind kind;
    SourceLocation loc;
    const Expr *expr;              // the DeclRefExpr (or wrapping expr) for the access
    const Stmt *enclosing_stmt;    // outer stmt used when replacing whole expressions
    std::string offset_text;       // InitArrayOffset / AssignArrayOffset / DerefOffset / wrapper-name
    std::string field_name;        // ArrowAccess / ArrowWrite
    std::string subscript_text;    // Subscript / SubscriptWrite
    std::string operand_text;      // PlusAssign / MinusAssign / comparison RHS / wrapper extra args
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
// RustSlice transformation metadata
// ============================================================================
//
// When a function's signature is rewritten to take a single RustSlice
// instead of a (pointer, length) or (pointer, pointer) pair, this struct
// records how the rewrite was performed so call sites can be patched up
// consistently. One entry per transformed function lives in
// g_transformed_functions.

struct RustSliceInfo {
    std::string slice_type;        // generated typedef name, e.g. "RustSlice_int"
    std::string pointee_type;      // element type, e.g. "int"
    std::string slice_param_name;  // name chosen for the new slice param, e.g. "arr"

    // Indices into the *original* parameter list. base_param_index is
    // the pointer that becomes arr.ptr; the others identify how the
    // length was originally expressed.
    int base_param_index = -1;     // pointer parameter (always set for RustSlice)
    int end_param_index = -1;      // end pointer (pointer-pair form);  -1 otherwise
    int len_param_index = -1;      // length parameter (ptr+len form);   -1 otherwise

    // Slice-bound padding driven by *(p ± k) accesses inside the body.
    // Call sites widen the constructed slice by these amounts.
    int lookback = 0;              // = -min_relative_offset
    int lookahead = 0;             // =  max_relative_offset

    bool return_type_changed = false;  // function's return type rewritten from T* to int
    bool inclusive_end = false;        // [lo, hi] pair where hi is dereferenced (length = hi - lo + 1)

    // Pointer parameters that don't iterate but are dereferenced (e.g.
    // swap's a,b). They turn into int indices alongside the slice.
    std::vector<int> singleton_param_indices;
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

// Records functions whose return type the tool has rewritten from
// "T *" to "int" because they only ever return into one global/static
// array. Lets callers be patched up to receive an index.
struct GlobalReturnInfo {
    std::string global_array_name;   // e.g., "node_storage"
    std::string pointee_type;        // e.g., "Node"
};

// ============================================================================
// Global state (defined in Common.cpp)
// ============================================================================
//
// The tool intentionally keeps cross-phase state in globals because
// detection runs in a fixpoint (one function's classification can mark
// another for rewriting) and emission needs to dedupe across functions
// (one strchr_index wrapper per TU, one typedef per pointee type).

extern int g_pointers_found;
extern int g_pointers_replaced;
extern TransformationLog gLog;
extern std::vector<FailedPointerLog> g_failed_pointers;
extern std::vector<SucceededPointerLog> g_succeeded_pointers;
extern DeclarationMatcher FunctionMatcher;     // matches every function definition
extern bool g_inplace;                         // --inplace CLI flag
extern bool g_verbose;                         // --verbose CLI flag

// File-scope pointers found in this TU (separate from per-function locals).
extern std::map<const VarDecl *, GlobalPointerState> g_global_pointer_map;

// Library functions whose pointer arguments / return values we know how
// to handle (see PassedToAllowedFunc / AssignFromAllowedFunc).
extern std::set<std::string> g_allowed_funcs;

// Names of _index wrappers already emitted (e.g. "strchr_index"). Used
// to make wrapper emission idempotent across the TU.
extern std::set<std::string> g_emitted_wrappers;

// Functions whose signature has been (or will be) rewritten to use a
// RustSlice. Populated during detection, consumed by call-site rewriting.
extern std::map<const FunctionDecl *, RustSliceInfo> g_transformed_functions;

// Names of slice typedefs already emitted (e.g. "RustSlice_int").
extern std::set<std::string> g_emitted_typedefs;

// Per-function analysis snapshots saved during run() for later phases.
extern std::map<const FunctionDecl *, FunctionAnalysis> g_function_analyses;

// Local variables whose type was rewritten from T* to int because they
// were initialized from a return-type-changed function. Tracked so
// later argument translation knows to use the bare name (no "_index"
// suffix).
extern std::set<const VarDecl *> g_index_return_vars;

// Functions whose return type was rewritten from T* to int.
extern std::map<const FunctionDecl *, GlobalReturnInfo> g_global_return_functions;

// Metadata accumulated across every TU in this run, written to
// g_metadata_out (if set) after the last file is processed. See
// xj-prepare-support/PtrIndexMetadata.h.
extern xj::PtrIndexMetadata g_metadata;
extern std::string g_metadata_out; // --metadata-out CLI flag ("" = don't write)

// The RustSlice pointer-pair path retargets a candidate's base to
// arr.ptr before rewriting it. The metadata records the *original* base
// (the fact about the input C), so the original text is stashed here at
// the retarget site, keyed by the pointer's VarDecl.
extern std::map<const VarDecl *, std::string> g_pre_slice_base_text;

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

// Return the leading whitespace (spaces/tabs) on the line containing
// `Loc`. Used to indent emitted code (wrappers, typedefs) consistently.
llvm::StringRef getIndentBeforeLoc(SourceLocation Loc, const SourceManager &SM);

// Lex back the original source text for a range / expression. Cheaper
// and more faithful than pretty-printing the AST node.
std::string getSourceText(SourceRange Range, const SourceManager &SM, const LangOptions &LO);
std::string getSourceText(const Expr *E, const SourceManager &SM, const LangOptions &LO);

// Debug helper: stringify a PointerAccessKind for trace logs.
const char *pointerAccessKindToString(PointerAccessKind kind);

// Turn a C type string into something legal inside an identifier.
// e.g. "char *" -> "char_ptr", "unsigned char" -> "unsigned_char".
inline std::string sanitizeTypeForIdentifier(const std::string &type) {
    std::string result = type;
    size_t pos;
    while ((pos = result.find(" *")) != std::string::npos)
        result.replace(pos, 2, "_ptr");
    while ((pos = result.find('*')) != std::string::npos)
        result.replace(pos, 1, "_ptr");
    for (char &c : result) {
        if (c == ' ') c = '_';
    }
    return result;
}

// Generate the slice typedef name for a pointee type.
// e.g. "int" -> "RustSlice_int", "char *" -> "RustSlice_char_ptr".
inline std::string makeSliceTypeName(const std::string &pointee_type) {
    return "RustSlice_" + sanitizeTypeForIdentifier(pointee_type);
}
