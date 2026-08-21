// Common.cpp — definitions for the cross-phase global state declared in
// Common.h, plus a handful of small AST/source-text helpers.

#include "Common.h"

// ============================================================================
// Global state — see Common.h for what each is for.
// ============================================================================

int g_pointers_found = 0;
int g_pointers_replaced = 0;
TransformationLog gLog;
std::vector<FailedPointerLog> g_failed_pointers;
std::vector<SucceededPointerLog> g_succeeded_pointers;

// Match every function definition in the TU. Bound name "funcDecl" is
// what FunctionAccessAnalyzer::run() looks up.
DeclarationMatcher FunctionMatcher = functionDecl(isDefinition()).bind("funcDecl");

bool g_inplace = false;
bool g_verbose = false;
std::map<const VarDecl *, GlobalPointerState> g_global_pointer_map;

std::map<const FunctionDecl *, FunctionAnalysis> g_function_analyses;
xj::PtrIndexMetadata g_metadata;
std::string g_metadata_out;

// ============================================================================
// Helpers
// ============================================================================

// Locate the DeclStmt that declares `VD` inside `FunctionBody`. Useful
// when we want to rewrite the declaration line as a whole rather than
// just the VarDecl's range.
const DeclStmt *findDeclStmtForVar(const VarDecl *VD, Stmt *FunctionBody) {
    if (!VD || !FunctionBody)
        return nullptr;

    class DeclStmtFinder : public RecursiveASTVisitor<DeclStmtFinder> {
      public:
        const VarDecl *Target;
        const DeclStmt *Found = nullptr;

        explicit DeclStmtFinder(const VarDecl *V) : Target(V) {}

        bool VisitDeclStmt(DeclStmt *DS) {
            for (const Decl *D : DS->decls()) {
                if (D == Target) {
                    Found = DS;
                    return false;
                }
            }
            return true;
        }
    };

    DeclStmtFinder finder(VD);
    finder.TraverseStmt(FunctionBody);
    return finder.Found;
}

// The ForStmt `DS` is the init clause of, if any. Only the init clause
// counts: a DeclStmt in the loop *body* is an ordinary statement with an
// ordinary position after it.
const ForStmt *forStmtInitializedBy(const DeclStmt *DS, ASTContext &Ctx) {
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

bool isMultiDeclarator(const DeclStmt *DS) {
    return DS && !DS->isSingleDecl();
}

// Return the run of spaces/tabs at the start of the line containing
// `Loc`, so emitted code (typedefs, wrappers) lines up with the
// surrounding source.
llvm::StringRef getIndentBeforeLoc(SourceLocation Loc, const SourceManager &SM) {
    SourceLocation spellingLoc = SM.getSpellingLoc(Loc);
    FileID FID = SM.getFileID(spellingLoc);
    llvm::StringRef buffer = SM.getBufferData(FID);

    unsigned line = SM.getSpellingLineNumber(spellingLoc);
    unsigned col = SM.getSpellingColumnNumber(spellingLoc);
    (void)col;  // computed for clarity; not used directly here

    SourceLocation lineStart = SM.translateLineCol(FID, line, 1);
    unsigned startOff = SM.getFileOffset(lineStart);
    unsigned locOff = SM.getFileOffset(spellingLoc);

    llvm::StringRef prefix = buffer.slice(startOff, locOff);
    return prefix.take_while([](char c) { return c == ' ' || c == '\t'; });
}

// Lex back the literal source text for a range. We use the lexer rather
// than pretty-printing because we want to preserve user formatting,
// macro spellings, and anything else verbatim.
std::string getSourceText(SourceRange Range, const SourceManager &SM, const LangOptions &LO) {
    CharSourceRange CSR = CharSourceRange::getTokenRange(Range);
    auto text = Lexer::getSourceText(CSR, SM, LO);
    return text.str();
}

std::string getSourceText(const Expr *E, const SourceManager &SM, const LangOptions &LO) {
    return getSourceText(E->getSourceRange(), SM, LO);
}

// Index names, keyed by the pointer's declaration. See Common.h.
static std::map<const VarDecl *, std::string> g_index_names;

void assignIndexNames(const std::vector<const VarDecl *> &ptrs) {
    std::set<std::string> used;
    for (const VarDecl *VD : ptrs) {
        const std::string base = VD->getNameAsString() + "_index_xj";
        std::string name = base;
        for (unsigned n = 1; used.count(name); n++)
            name = base + "_" + std::to_string(n);
        used.insert(name);
        g_index_names[VD] = name;
    }
}

const std::string &indexNameFor(const VarDecl *VD) {
    auto it = g_index_names.find(VD);
    if (it != g_index_names.end())
        return it->second;
    return g_index_names
        .emplace(VD, VD->getNameAsString() + "_index_xj")
        .first->second;
}

// Stringify a PointerAccessKind for verbose / debug output.
const char *pointerAccessKindToString(PointerAccessKind kind) {
    switch (kind) {
    case PointerAccessKind::Deref: return "Deref";
    case PointerAccessKind::DerefWrite: return "DerefWrite";
    case PointerAccessKind::DerefPostInc: return "DerefPostInc";
    case PointerAccessKind::DerefPreInc: return "DerefPreInc";
    case PointerAccessKind::DerefPostDec: return "DerefPostDec";
    case PointerAccessKind::DerefPreDec: return "DerefPreDec";
    case PointerAccessKind::DerefOffset: return "DerefOffset";
    case PointerAccessKind::DerefOffsetWrite: return "DerefOffsetWrite";
    case PointerAccessKind::ArrowAccess: return "ArrowAccess";
    case PointerAccessKind::ArrowWrite: return "ArrowWrite";
    case PointerAccessKind::Subscript: return "Subscript";
    case PointerAccessKind::SubscriptWrite: return "SubscriptWrite";
    case PointerAccessKind::Increment: return "Increment";
    case PointerAccessKind::Decrement: return "Decrement";
    case PointerAccessKind::PlusAssign: return "PlusAssign";
    case PointerAccessKind::MinusAssign: return "MinusAssign";
    case PointerAccessKind::Init: return "Init";
    case PointerAccessKind::Assign: return "Assign";
    case PointerAccessKind::ValueUse: return "ValueUse";
    case PointerAccessKind::PairwiseRoot: return "PairwiseRoot";
    case PointerAccessKind::NullTest: return "NullTest";
    case PointerAccessKind::NoEdit: return "NoEdit";
    case PointerAccessKind::AddressOf: return "AddressOf";
    case PointerAccessKind::Unknown: return "Unknown";
    }
    return "Unknown";
}
