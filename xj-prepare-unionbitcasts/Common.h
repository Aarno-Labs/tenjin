#pragma once

#include "clang/AST/ASTContext.h"
#include "clang/AST/ParentMapContext.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/Type.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/Analysis/Analyses/Dominators.h"
#include "clang/Analysis/CFG.h"
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

inline constexpr bool VERBOSE = false;

// ============================================================================
// Logging and State Structures
// ============================================================================

struct TransformationLog {
    bool foundUnion = false;
    bool replacedUnion = false;
    std::string error = "";
};

struct FailedUnionLog {
    std::string varName;
    unsigned line;
    unsigned col;
    std::string error;
};

// ============================================================================
// Global State (extern declarations - defined in Common.cpp)
// ============================================================================

extern bool addedMemcpyDecl;
extern int g_unions_found;
extern int g_unions_replaced;
extern TransformationLog gLog;
extern std::vector<FailedUnionLog> g_failed_unions;
extern std::set<std::string> generatedObjects;
extern std::map<const RecordDecl *, std::string> UnnamedTypedefMap;
extern DeclarationMatcher FunctionMatcher;

// ============================================================================
// Union Analysis Structures
// ============================================================================

struct TypePunCandidate {
    const VarDecl *var_decl;
    const FieldDecl *a_field;
    const FieldDecl *b_field;
    const int num_bytes;

    bool operator==(const TypePunCandidate &other) const {
        return var_decl == other.var_decl && a_field == other.a_field &&
               b_field == other.b_field && num_bytes == other.num_bytes;
    }
    bool operator<(const TypePunCandidate &other) const {
        if (var_decl != other.var_decl) return var_decl < other.var_decl;
        if (a_field != other.a_field) return a_field < other.a_field;
        if (b_field != other.b_field) return b_field < other.b_field;
        return num_bytes < other.num_bytes;
    }
};

struct FieldAccess {
    const FieldDecl *field;
    SourceLocation loc;
    bool is_write;
    const MemberExpr *expr;
    int64_t array_index;            // -1 if not an array access, otherwise the index
    const FieldDecl *struct_member; // if field is a struct, which member was accessed
};

// ============================================================================
// Helper Function Templates (must be in header)
// ============================================================================

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

// ============================================================================
// Helper Function Declarations (defined in Common.cpp)
// ============================================================================

const DeclStmt *findDeclStmtForVar(const VarDecl *VD, Stmt *FunctionBody);
llvm::StringRef getIndentBeforeLoc(SourceLocation Loc, const SourceManager &SM);
bool isValidUnion(const RecordDecl *RD, ASTContext &Ctx, const FieldDecl *&out_a_field,
                  const FieldDecl *&out_b_field, int &num_bytes, const VarDecl *VD = nullptr);
std::string getOrCreateTypedefName(QualType QT, ASTContext &Ctx);
std::string sanitizeTypes(const std::string &typeName);
std::string getTagDeclName(QualType QT);
std::string getFullTypeName(QualType QT, ASTContext &Ctx);
std::string generateTypedefForUnnamedType(QualType QT, ASTContext &Ctx);
std::string generateConversionFunction(const FieldDecl *src_field, const FieldDecl *dst_field,
                                        int copySizeBytes, SourceLocation insertLoc,
                                        ASTContext &Ctx);
std::string generateFuncName(QualType srcType, QualType dstType, SourceLocation unionLoc,
                              ASTContext &Ctx);
std::string generateUnionConversionFunction(QualType srcType, QualType dstType,
                                             SourceLocation unionLoc, ASTContext &Ctx,
                                             int copySizeBytes);
