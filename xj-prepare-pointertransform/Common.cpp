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

// Library functions whose pointer args / returns we know how to translate.
// Keep this list small and conservative — anything added here needs
// matching support in PointerAccessCollector and TransformationMethods.
std::set<std::string> g_allowed_funcs = {"strchr", "sscanf"};

std::set<std::string> g_emitted_wrappers;
std::map<const FunctionDecl *, RustSliceInfo> g_transformed_functions;
std::map<const FunctionDecl *, FunctionAnalysis> g_function_analyses;
std::map<const FunctionDecl *, GlobalReturnInfo> g_global_return_functions;
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

// Stringify a PointerAccessKind for verbose / debug output.
const char *pointerAccessKindToString(PointerAccessKind kind) {
    switch (kind) {
    case PointerAccessKind::InitNull: return "InitNull";
    case PointerAccessKind::InitArray: return "InitArray";
    case PointerAccessKind::InitArrayOffset: return "InitArrayOffset";
    case PointerAccessKind::AssignNull: return "AssignNull";
    case PointerAccessKind::AssignAddrOf: return "AssignAddrOf";
    case PointerAccessKind::AssignArray: return "AssignArray";
    case PointerAccessKind::AssignArrayOffset: return "AssignArrayOffset";
    case PointerAccessKind::Increment: return "Increment";
    case PointerAccessKind::Decrement: return "Decrement";
    case PointerAccessKind::PlusAssign: return "PlusAssign";
    case PointerAccessKind::MinusAssign: return "MinusAssign";
    case PointerAccessKind::Deref: return "Deref";
    case PointerAccessKind::DerefPostInc: return "DerefPostInc";
    case PointerAccessKind::DerefPreInc: return "DerefPreInc";
    case PointerAccessKind::DerefPostDec: return "DerefPostDec";
    case PointerAccessKind::DerefPreDec: return "DerefPreDec";
    case PointerAccessKind::ArrowAccess: return "ArrowAccess";
    case PointerAccessKind::Subscript: return "Subscript";
    case PointerAccessKind::DerefOffset: return "DerefOffset";
    case PointerAccessKind::DerefOffsetWrite: return "DerefOffsetWrite";
    case PointerAccessKind::DerefWrite: return "DerefWrite";
    case PointerAccessKind::ArrowWrite: return "ArrowWrite";
    case PointerAccessKind::SubscriptWrite: return "SubscriptWrite";
    case PointerAccessKind::Comparison: return "Comparison";
    case PointerAccessKind::ComparisonNull: return "ComparisonNull";
    case PointerAccessKind::ComparisonExpr: return "ComparisonExpr";
    case PointerAccessKind::BoolTrue: return "BoolTrue";
    case PointerAccessKind::BoolFalse: return "BoolFalse";
    case PointerAccessKind::PassedToAllowedFunc: return "PassedToAllowedFunc";
    case PointerAccessKind::AssignFromAllowedFunc: return "AssignFromAllowedFunc";
    case PointerAccessKind::AddressOf: return "AddressOf";
    case PointerAccessKind::PassedToFunc: return "PassedToFunc";
    case PointerAccessKind::ReturnPtr: return "ReturnPtr";
    case PointerAccessKind::Unknown: return "Unknown";
    }
    return "Unknown";
}
