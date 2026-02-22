#include "FunctionAccessAnalyzer.h"

// ============================================================================
// FunctionAccessAnalyzer Core Implementation
// ============================================================================

FunctionAccessAnalyzer::FunctionAccessAnalyzer(Rewriter &R) : TheRewriter(R) {}

void FunctionAccessAnalyzer::run(const MatchFinder::MatchResult &Result) {
    const FunctionDecl *FD = Result.Nodes.getNodeAs<FunctionDecl>("funcDecl");
    if (!FD || !FD->hasBody())
        return;

    ASTContext &Ctx = *Result.Context;
    Stmt *Body = FD->getBody();

    if (Ctx.getSourceManager().isInSystemHeader(FD->getLocation())) {
        return;
    }

    UnionAccessCollector V(Ctx);
    traverseFunctionBody(Body, V);

    for (auto &pair : V.accesses) {
        g_unions_found++;
        transformUnionVar(FD, pair.first, pair.second, Ctx);
    }
}

void FunctionAccessAnalyzer::traverseFunctionBody(Stmt *Body, UnionAccessCollector &V) {
    if (VERBOSE)
        llvm::outs() << "[Debug] Traversing Function Body for union accesses\n";
    V.TraverseStmt(Body);
    if (VERBOSE)
        llvm::outs() << "[Debug] Done traversing Function Body for union accesses\n";
}

void FunctionAccessAnalyzer::logFailedUnion(const VarDecl *VD, ASTContext &Ctx,
                                            const std::string &error) {
    SourceManager &SM = Ctx.getSourceManager();
    SourceLocation loc = VD->getLocation();
    unsigned line = SM.getSpellingLineNumber(loc);
    unsigned col = SM.getSpellingColumnNumber(loc);
    g_failed_unions.push_back({VD->getNameAsString(), line, col, error});
}

bool FunctionAccessAnalyzer::compareBySource(const FieldAccess &A, const FieldAccess &B,
                                             ASTContext &Ctx) {
    const SourceManager &SM = Ctx.getSourceManager();
    if (A.loc.isInvalid() || B.loc.isInvalid())
        return false;
    return SM.getFileOffset(A.loc) < SM.getFileOffset(B.loc);
}

void FunctionAccessAnalyzer::printAccesses(const VarDecl *VD,
                                           const std::vector<FieldAccess> &seq,
                                           ASTContext &Ctx) {
    if (!VERBOSE)
        return;

    llvm::outs() << "[Debug] Access sequence for variable: " << VD->getNameAsString() << "\n";
    SourceManager &SM = Ctx.getSourceManager();
    for (const auto &a : seq) {
        std::string locStr = "<unknown>";
        if (a.loc.isValid())
            locStr = std::to_string(SM.getSpellingLineNumber(a.loc)) + ":" +
                     std::to_string(SM.getSpellingColumnNumber(a.loc));
        llvm::outs() << "  " << a.field->getNameAsString() << " | "
                     << (a.is_write ? "WRITE" : "READ") << " | at " << locStr << "\n";
    }
}

void FunctionAccessAnalyzer::countFieldAccesses(const std::vector<FieldAccess> &seq,
                                                unsigned &writes_a, unsigned &reads_a,
                                                unsigned &writes_b, unsigned &reads_b,
                                                const FieldDecl *&a_field,
                                                const FieldDecl *&b_field) {
    writes_a = reads_a = writes_b = reads_b = 0;
    for (const auto &access : seq) {
        if (access.field == a_field) {
            if (access.is_write)
                ++writes_a;
            else
                ++reads_a;
        } else if (access.field == b_field) {
            if (access.is_write)
                ++writes_b;
            else
                ++reads_b;
        }
    }

    if (VERBOSE)
        llvm::outs() << "[Debug] counts: writes_a=" << writes_a << " reads_a=" << reads_a
                     << " writes_b=" << writes_b << " reads_b=" << reads_b << "\n";
}

std::string FunctionAccessAnalyzer::getTypeString(QualType T, ASTContext &Ctx) {
    uint64_t size = Ctx.getTypeSize(T);
    if (T->isFloatingType())
        return (size == 32) ? "float" : "double";
    else
        return (T->isUnsignedIntegerType() ? "uint" : "int") + std::to_string(size) + "_t";
}

std::string FunctionAccessAnalyzer::generateTmpDeclaration(const std::string tmp_name,
                                                           QualType qual_type,
                                                           ASTContext &Ctx) {
    std::string type_name;
    if (qual_type->isStructureType() || qual_type->isUnionType()) {
        type_name = getFullTypeName(qual_type, Ctx);
    } else {
        type_name = getOrCreateTypedefName(qual_type, Ctx);
        if (type_name.empty())
            type_name = getFullTypeName(qual_type, Ctx);
    }

    const clang::ArrayType *maybe_array = Ctx.getAsArrayType(qual_type);

    clang::QualType elem_type = maybe_array ? maybe_array->getElementType() : qual_type;
    std::string elem_type_str = getOrCreateTypedefName(elem_type, Ctx);
    if (elem_type_str.empty())
        elem_type_str = elem_type.getAsString();

    std::string dst_decl;
    if (maybe_array) {
        if (const clang::ConstantArrayType *constArr =
                llvm::dyn_cast<clang::ConstantArrayType>(maybe_array)) {
            uint64_t arraySize = constArr->getSize().getZExtValue();
            return elem_type_str + " " + tmp_name + "[" + std::to_string(arraySize) + "];\n";
        } else {
            return elem_type_str + " *" + tmp_name + "\n";
        }
    } else {
        return type_name + " " + tmp_name + ";\n";
    }
}

void FunctionAccessAnalyzer::ensureMemcpyDeclared(const FunctionDecl *FD, ASTContext &Ctx,
                                                   std::vector<Edit> &edits) {
    SourceManager &SM = Ctx.getSourceManager();
    FileID FID = SM.getFileID(FD->getSourceRange().getBegin());
    const FileEntry *FE = SM.getFileEntryForID(FID);
    if (!FE)
        return;

    llvm::StringRef buffer = SM.getBufferData(FID);
    bool isCPlusPlus = Ctx.getLangOpts().CPlusPlus;

    if (isCPlusPlus) {
        if (!buffer.contains("extern \"C\" void *memcpy") && !addedMemcpyDecl) {
            SourceLocation file_start = SM.getLocForStartOfFile(FID);
            edits.push_back({Edit::InsertBefore, SM.getFileOffset(file_start), file_start,
                             SourceLocation(),
                             "extern \"C\" void *memcpy(void *dest, const void "
                             "*src, unsigned long n) noexcept;\n"});
            addedMemcpyDecl = true;
        }
    } else {
        if (!buffer.contains("void *memcpy(void *dest, const void *src") && !addedMemcpyDecl) {
            SourceLocation file_start = SM.getLocForStartOfFile(FID);
            edits.push_back({Edit::InsertBefore, SM.getFileOffset(file_start), file_start,
                             SourceLocation(),
                             "void *memcpy(void *dest, const void *src, unsigned long n);\n"});
            addedMemcpyDecl = true;
        }
    }
}

void FunctionAccessAnalyzer::applyEdits(std::vector<Edit> &edits, SourceManager &SM) {
    // Sort by offset in descending order (end to beginning of file)
    // This ensures earlier edits don't shift positions of later edits
    std::sort(edits.begin(), edits.end(),
              [](const Edit &A, const Edit &B) { return A.offset > B.offset; });

    for (const auto &e : edits) {
        switch (e.type) {
        case Edit::Replace:
            TheRewriter.ReplaceText(CharSourceRange::getCharRange(e.start, e.end), e.text);
            break;
        case Edit::InsertBefore:
            TheRewriter.InsertTextBefore(e.start, e.text);
            break;
        case Edit::InsertAfterToken:
            TheRewriter.InsertTextAfterToken(e.start, e.text);
            break;
        }
    }
}

void FunctionAccessAnalyzer::transformUnionVar(const FunctionDecl *FD, TypePunCandidate UI,
                                                   std::vector<FieldAccess> seq, ASTContext &Ctx) {
    if (seq.empty())
        return;

    printAccesses(UI.var_decl, seq, Ctx);
    std::sort(seq.begin(), seq.end(), [&Ctx](const FieldAccess &A, const FieldAccess &B) {
        return compareBySource(A, B, Ctx);
    });

    // Try both directions and use whichever finds a valid cut point
    // Direction 1: a -> b (a is source, b is destination)
    // Direction 2: b -> a (b is source, a is destination)
    const FieldDecl *src_field = nullptr;
    const FieldDecl *dst_field = nullptr;
    CFGBlock *cutPoint = nullptr;
    std::unique_ptr<CFG> cfgOwner; // Keeps CFG alive so cutPoint remains valid

    // Try a -> b first
    std::string error_a_to_b;
    bool a_to_b_valid =
        validateAccessSequence(FD, UI.var_decl, seq, UI.a_field, UI.b_field, Ctx, &cutPoint, &cfgOwner);
    if (a_to_b_valid) {
        src_field = UI.a_field;
        dst_field = UI.b_field;
    } else {
        error_a_to_b = gLog.error;
    }

    // If that fails, try b -> a
    if (!a_to_b_valid) {
        std::string error_b_to_a;
        bool b_to_a_valid =
            validateAccessSequence(FD, UI.var_decl, seq, UI.b_field, UI.a_field, Ctx, &cutPoint, &cfgOwner);
        if (b_to_a_valid) {
            src_field = UI.b_field;
            dst_field = UI.a_field;
        } else {
            error_b_to_a = gLog.error;

            // Neither direction worked - report the more informative error
            // Prefer interleaved access errors over "no source write blocks" errors
            std::string final_error;
            if (error_a_to_b.find("interleaved") != std::string::npos) {
                final_error = error_a_to_b;
            } else if (error_b_to_a.find("interleaved") != std::string::npos) {
                final_error = error_b_to_a;
            } else if (error_a_to_b.find("No source write blocks") == std::string::npos) {
                final_error = error_a_to_b;
            } else {
                final_error = error_b_to_a;
            }

            gLog.error = final_error;
            logFailedUnion(UI.var_decl, Ctx, final_error);
            if (VERBOSE)
                llvm::outs() << "[Error] " + final_error + "\n";
            return;
        }
    }
    SourceManager &SM = Ctx.getSourceManager();
    LangOptions LO = Ctx.getLangOpts();

    // Get union definition location
    const RecordDecl *unionDecl = src_field->getParent();
    SourceLocation unionLoc = unionDecl->getLocation();

    std::string srcC = getTypeString(src_field->getType(), Ctx);
    std::string funcName =
        generateFuncName(src_field->getType(), dst_field->getType(), unionLoc, Ctx);
    std::string funcCode = generateUnionConversionFunction(src_field->getType(), dst_field->getType(),
                                                            unionLoc, Ctx, UI.num_bytes);
    if (funcName.empty() || funcCode.empty()) {
        gLog.error = "Could not generate conversion function";
        logFailedUnion(UI.var_decl, Ctx, gLog.error);
        if (VERBOSE)
            llvm::outs() << "[Error] " + gLog.error + "\n";
        return;
    }

    if (emitTransformation(FD, UI.var_decl, seq, src_field, dst_field, srcC, funcName, funcCode, Ctx,
                   cutPoint)) {
        gLog.replacedUnion = true;
        g_unions_replaced++;
    }
}
