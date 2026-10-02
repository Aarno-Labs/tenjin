// Common.cpp — definitions for the cross-phase global state declared in
// Common.h, plus a handful of small AST/source-text helpers.

#include "Common.h"

// ============================================================================
// Global state — see Common.h for what each is for.
// ============================================================================

int g_pointers_found = 0;
int g_pointers_replaced = 0;
int g_invariant_violations = 0;
TransformationLog gLog;
std::vector<FailedPointerLog> g_failed_pointers;
std::vector<SucceededPointerLog> g_succeeded_pointers;

// Match every function definition in the TU. Bound name "funcDecl" is
// what FunctionAccessAnalyzer::run() looks up.
DeclarationMatcher FunctionMatcher = functionDecl(isDefinition()).bind("funcDecl");

bool g_inplace = false;
bool g_verbose = false;
std::map<const VarDecl *, std::vector<PointerAccess>> g_global_pointer_map;

std::set<std::string> g_allowed_funcs = {"strchr", "strstr"};

std::map<const FunctionDecl *, FunctionAnalysis> g_function_analyses;
xj::PtrIndexMetadata g_metadata;
std::string g_metadata_out;
std::vector<PendingDeclLoc> g_pending_decl_locs;

// ============================================================================
// Helpers
// ============================================================================

// A local's DeclStmt is its parent in the AST. A parameter's parent is its
// function and a file-scope variable's is the translation unit, so neither
// has one.
const DeclStmt *declStmtOf(const VarDecl *VD, ASTContext &Ctx)
{
    if (!VD)
        return nullptr;
    for (const DynTypedNode &P : Ctx.getParents(*VD))
        if (const auto *DS = P.get<DeclStmt>())
            return DS;
    return nullptr;
}

bool referencesAnyOf(const Stmt *S, const std::set<const Decl *> &decls)
{
    if (!S)
        return false;
    if (const auto *DRE = dyn_cast<DeclRefExpr>(S))
        if (decls.count(DRE->getDecl()))
            return true;
    for (const Stmt *Child : S->children())
        if (referencesAnyOf(Child, decls))
            return true;
    return false;
}

// The ForStmt `DS` is the init clause of, if any. Only the init clause
// counts: a DeclStmt in the loop *body* is an ordinary statement with an
// ordinary position after it.
const ForStmt *forStmtInitializedBy(const DeclStmt *DS, ASTContext &Ctx)
{
    if (!DS)
        return nullptr;
    auto Parents = Ctx.getParents(*DS);
    if (Parents.empty())
        return nullptr;
    const auto *FS = Parents[0].get<ForStmt>();
    if (FS && FS->getInit() == DS)
        return FS;
    return nullptr;
}

bool isMultiDeclarator(const DeclStmt *DS)
{
    return DS && !DS->isSingleDecl();
}

// Return the run of spaces/tabs at the start of the line containing
// `Loc`, so an emitted index declaration lines up with the surrounding
// source.
llvm::StringRef getIndentBeforeLoc(SourceLocation Loc, const SourceManager &SM)
{
    SourceLocation spellingLoc = SM.getSpellingLoc(Loc);
    FileID FID = SM.getFileID(spellingLoc);
    llvm::StringRef buffer = SM.getBufferData(FID);

    unsigned line = SM.getSpellingLineNumber(spellingLoc);
    unsigned col = SM.getSpellingColumnNumber(spellingLoc);
    (void)col; // computed for clarity; not used directly here

    SourceLocation lineStart = SM.translateLineCol(FID, line, 1);
    unsigned startOff = SM.getFileOffset(lineStart);
    unsigned locOff = SM.getFileOffset(spellingLoc);

    llvm::StringRef prefix = buffer.slice(startOff, locOff);
    return prefix.take_while([](char c)
                             { return c == ' ' || c == '\t'; });
}

// Lex back the literal source text for a range. We use the lexer rather
// than pretty-printing because we want to preserve user formatting,
// macro spellings, and anything else verbatim.
std::string getSourceText(SourceRange Range, const SourceManager &SM, const LangOptions &LO)
{
    CharSourceRange CSR = CharSourceRange::getTokenRange(Range);
    auto text = Lexer::getSourceText(CSR, SM, LO);
    return text.str();
}

std::string getSourceText(const Expr *E, const SourceManager &SM, const LangOptions &LO)
{
    return getSourceText(E->getSourceRange(), SM, LO);
}

// Index names, keyed by the pointer's declaration. See Common.h.
static std::map<const VarDecl *, std::string> g_index_names;

void resetIndexNames()
{
    g_index_names.clear();
}

void assignIndexNames(const std::vector<const VarDecl *> &ptrs)
{
    // A file-scope index is in scope throughout the function, and a local
    // index does not always share its pointer's scope, so a local must not
    // take a file-scope index's name.
    std::set<std::string> used;
    for (const auto &[GVD, accesses] : g_global_pointer_map)
        used.insert(indexNameFor(GVD));

    for (const VarDecl *VD : ptrs)
    {
        const std::string base = VD->getNameAsString() + "_index_xj";
        std::string name = base;
        for (unsigned n = 1; used.count(name); n++)
            name = base + "_" + std::to_string(n);
        used.insert(name);
        g_index_names[VD] = name;
    }
}

const std::string &indexNameFor(const VarDecl *VD)
{
    auto it = g_index_names.find(VD);
    if (it != g_index_names.end())
        return it->second;
    return g_index_names
        .emplace(VD, VD->getNameAsString() + "_index_xj")
        .first->second;
}

// Pre-increment yields the new position and post-increment the old one, so
// `q = ++p` and `q = p++` differ only in where the operator lands — the same
// distinction the index has to reproduce.
std::string applyStep(IndexStep step, const std::string &name) {
    switch (step) {
    case IndexStep::PostInc: return name + "++";
    case IndexStep::PostDec: return name + "--";
    case IndexStep::PreInc: return "++" + name;
    case IndexStep::PreDec: return "--" + name;
    case IndexStep::None: break;
    }
    return name;
}

// Stringify a PointerAccessKind for verbose / debug output.
const char *pointerAccessKindToString(PointerAccessKind kind)
{
    switch (kind)
    {
    case PointerAccessKind::Element:
        return "Element";
    case PointerAccessKind::Move:
        return "Move";
    case PointerAccessKind::Init:
        return "Init";
    case PointerAccessKind::Assign:
        return "Assign";
    case PointerAccessKind::InitNull:
        return "InitNull";
    case PointerAccessKind::AssignNull:
        return "AssignNull";
    case PointerAccessKind::AssignFromAllowedFunc:
        return "AssignFromAllowedFunc";
    case PointerAccessKind::ValueUse:
        return "ValueUse";
    case PointerAccessKind::NullTest:
        return "NullTest";
    case PointerAccessKind::NoEdit:
        return "NoEdit";
    case PointerAccessKind::AddressOf:
        return "AddressOf";
    case PointerAccessKind::Unknown:
        return "Unknown";
    }
    return "Unknown";
}
