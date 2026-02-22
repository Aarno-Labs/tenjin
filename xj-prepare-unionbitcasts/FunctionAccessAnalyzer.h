#pragma once

#include "UnionAccessCollector.h"

// ============================================================================
// FunctionAccessAnalyzer - Analyzes functions and rewrites union accesses
// ============================================================================

class FunctionAccessAnalyzer : public MatchFinder::MatchCallback {
  public:
    explicit FunctionAccessAnalyzer(Rewriter &R);

    void run(const MatchFinder::MatchResult &Result) override;

  private:
    Rewriter &TheRewriter;

    void traverseFunctionBody(Stmt *Body, UnionAccessCollector &V);

    void logFailedUnion(const VarDecl *VD, ASTContext &Ctx, const std::string &error);

    void transformUnionVar(const FunctionDecl *FD, TypePunCandidate UI,
                           std::vector<FieldAccess> seq, ASTContext &Ctx);

    void printAccesses(const VarDecl *VD, const std::vector<FieldAccess> &seq,
                       ASTContext &Ctx);

    static bool compareBySource(const FieldAccess &A, const FieldAccess &B, ASTContext &Ctx);

    void countFieldAccesses(const std::vector<FieldAccess> &seq,
                            unsigned &writes_a, unsigned &reads_a,
                            unsigned &writes_b, unsigned &reads_b,
                            const FieldDecl *&a_field, const FieldDecl *&b_field);

    bool hasUnsafeFunctionCall(const CompoundStmt *enclosingCompound,
                               const VarDecl *unionVar, ASTContext &Ctx);

    bool validateAccessSequence(const FunctionDecl *FD, const VarDecl *unionVar,
                                const std::vector<FieldAccess> &seq,
                                const FieldDecl *src_field,
                                const FieldDecl *dst_field, ASTContext &Ctx,
                                CFGBlock **outCutPoint = nullptr,
                                std::unique_ptr<CFG> *outCFG = nullptr);

    std::string getTypeString(QualType T, ASTContext &Ctx);

    std::string generateTmpDeclaration(const std::string tmp_name,
                                       QualType qual_type, ASTContext &Ctx);

    struct Edit {
        enum Type { Replace, InsertBefore, InsertAfterToken };
        Type type;
        unsigned offset;
        SourceLocation start;
        SourceLocation end;  // only used for Replace
        std::string text;
    };

    // Insertion point info from cut point analysis
    struct InsertionInfo {
        SourceLocation point;
        bool atBottom = false;   // Insert after last stmt vs before first
        bool midLine = false;    // Insert mid-line (e.g., inside do { } while(0))
    };

    // Shared state for transformation
    struct TransformContext {
        const FunctionDecl *FD;
        const VarDecl *VD;
        const std::vector<FieldAccess> &seq;
        const FieldDecl *src_field;
        const FieldDecl *dst_field;
        const std::string &funcName;
        const std::string &funcCode;
        ASTContext &Ctx;
        SourceManager &SM;
        LangOptions LO;
        CFGBlock *cutPoint;

        // Generated values
        std::string tmp_in_name;
        std::string tmp_out_name;
        std::string indentation;
        std::string src_typedef_code;
        std::string dst_typedef_code;
        SourceLocation union_start;
        SourceLocation union_end;
        std::vector<Edit> edits;
    };

    void applyEdits(std::vector<Edit> &edits, SourceManager &SM);

    void ensureMemcpyDeclared(const FunctionDecl *FD, ASTContext &Ctx,
                              std::vector<Edit> &edits);

    // Shared state for validation
    struct ValidationContext {
        const FunctionDecl *FD;
        const VarDecl *unionVar;
        const std::vector<FieldAccess> &seq;
        const FieldDecl *src_field;
        const FieldDecl *dst_field;
        ASTContext &Ctx;

        // Built during validation
        std::unique_ptr<CFG> cfg;
        clang::CFGDomTree DT;
        clang::CFGPostDomTree PDT;
        std::vector<CFGBlock *> srcWriteBlocks;
        std::vector<CFGBlock *> dstAccessBlocks;
        CFGBlock *cutPoint = nullptr;
        bool hasInitWrite = false;

        // Helper function for finding CFG blocks
        std::function<bool(const Stmt *, const Stmt *)> contains;
        std::function<CFGBlock *(const Stmt *)> findBlockForStmt;
    };

    // Modular validation methods
    bool buildCFGAnalysis(ValidationContext &ctx);
    bool collectAccessBlocks(ValidationContext &ctx);
    bool findCutPoint(ValidationContext &ctx);
    bool validateSourceOrdering(ValidationContext &ctx);
    bool validateNoUnsafeFunctionCalls(ValidationContext &ctx);
    bool validateArrayWrites(ValidationContext &ctx);
    bool validateStructWrites(ValidationContext &ctx);

    // Modular transformation methods
    InsertionInfo analyzeCutPoint(TransformContext &ctx);
    void generateDeclarations(TransformContext &ctx);
    bool tryDeleteUnionDecl(TransformContext &ctx, const DeclStmt *declStmt);
    bool handleInitOnlyCase(TransformContext &ctx);
    void collectReplacementEdits(TransformContext &ctx);
    void emitConversionCall(TransformContext &ctx, const InsertionInfo &info);
    void emitTypedefAndFunction(TransformContext &ctx);

    bool emitTransformation(const FunctionDecl *FD, const VarDecl *VD,
                            const std::vector<FieldAccess> &seq,
                            const FieldDecl *src_field, const FieldDecl *dst_field,
                            const std::string &srcC, const std::string &funcName,
                            const std::string &funcCode, ASTContext &Ctx,
                            CFGBlock *cutPoint);
};
