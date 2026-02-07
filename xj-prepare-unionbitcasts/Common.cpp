#include "Common.h"

// ============================================================================
// Global State Definitions
// ============================================================================

bool addedMemcpyDecl = false;
int g_unions_found = 0;
int g_unions_replaced = 0;
TransformationLog gLog;
std::vector<FailedUnionLog> g_failed_unions;
std::set<std::string> generatedObjects;
std::map<const RecordDecl *, std::string> UnnamedTypedefMap;
DeclarationMatcher FunctionMatcher = functionDecl(isDefinition()).bind("funcDecl");

// ============================================================================
// Helper Function Implementations
// ============================================================================

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

llvm::StringRef getIndentBeforeLoc(SourceLocation Loc, const SourceManager &SM) {
    SourceLocation spellingLoc = SM.getSpellingLoc(Loc);
    FileID FID = SM.getFileID(spellingLoc);
    llvm::StringRef buffer = SM.getBufferData(FID);

    unsigned line = SM.getSpellingLineNumber(spellingLoc);
    unsigned col = SM.getSpellingColumnNumber(spellingLoc);

    SourceLocation lineStart = SM.translateLineCol(FID, line, 1);
    unsigned startOff = SM.getFileOffset(lineStart);
    unsigned locOff = SM.getFileOffset(spellingLoc);

    llvm::StringRef prefix = buffer.slice(startOff, locOff);
    return prefix.take_while([](char c) { return c == ' ' || c == '\t'; });
}

bool isValidUnion(const RecordDecl *RD, ASTContext &Ctx, const FieldDecl *&out_a_field,
                  const FieldDecl *&out_b_field, int &num_bytes, const VarDecl *VD) {
    gLog.foundUnion = true;
    if (VERBOSE)
        llvm::outs() << "[Debug] Checking Union\n";
    if (!RD || !RD->isUnion() || !RD->isCompleteDefinition()) {
        gLog.error = "Union is not complete";
        if (VERBOSE)
            llvm::outs() << "[Error] " + gLog.error + "\n";
        return false;
    }

    if (VD) {
        const DeclStmt *declStmt = findEnclosingStmt<DeclStmt>(VD, Ctx);
        if (!declStmt) {
            gLog.error = "Union variable must be local, not global";
            if (VERBOSE)
                llvm::outs() << "[Error] " + gLog.error + "\n";
            return false;
        }
    }

    const FieldDecl *a_field = nullptr;
    const FieldDecl *b_field = nullptr;
    uint64_t a_width = 0, b_width = 0;
    int num_fields = 0;

    for (const FieldDecl *FD : RD->fields()) {
        ++num_fields;
        QualType QT = FD->getType();
        QualType canon = Ctx.getCanonicalType(QT.getUnqualifiedType());
        if (a_field == nullptr) {
            a_field = FD;
            a_width = Ctx.getTypeSize(canon);
        } else if (a_field != nullptr && b_field == nullptr) {
            b_field = FD;
            b_width = Ctx.getTypeSize(canon);
        }
    }

    if (num_fields != 2 || a_field == nullptr || b_field == nullptr) {
        gLog.error = "Union does not have 2 fields";
        if (VERBOSE)
            llvm::outs() << "[Error] " + gLog.error + "\n";
        return false;
    }

    if (b_width != a_width) {
        gLog.error = "Union fields do not have same width";
        if (VERBOSE)
            llvm::outs() << "[Error] " + gLog.error + "\n";
        return false;
    }

    num_bytes = a_width / 8;
    out_a_field = a_field;
    out_b_field = b_field;
    return true;
}

std::string getOrCreateTypedefName(QualType QT, ASTContext &Ctx) {
    if (const clang::ArrayType *ArrType = Ctx.getAsArrayType(QT)) {
        QualType elemType = ArrType->getElementType();
        return getOrCreateTypedefName(elemType, Ctx);
    }

    if (const TagDecl *TD = QT->getAsTagDecl()) {
        if (IdentifierInfo *ID = TD->getIdentifier()) {
            return ID->getName().str();
        }

        if (const RecordDecl *RD = dyn_cast<RecordDecl>(TD)) {
            if (UnnamedTypedefMap.count(RD)) {
                return UnnamedTypedefMap[RD];
            }

            std::string typeKind = RD->isUnion() ? "union" : "struct";
            std::string uniqueName =
                "tenjin_" + typeKind + "_" + std::to_string(reinterpret_cast<uintptr_t>(RD));
            UnnamedTypedefMap[RD] = uniqueName;
            return uniqueName;
        }
    }

    return QT.getAsString();
}

std::string sanitizeTypes(const std::string &typeName) {
    std::string result;
    for (char c : typeName) {
        if (std::isalnum(c) || c == '_') {
            result += c;
        } else if (c == '[' || c == ']') {
            result += '_';
        } else if (c == ' ') {
            result += '_';
        }
    }
    return result;
}

std::string getTagDeclName(QualType QT) {
    if (const TagDecl *TD = QT->getAsTagDecl()) {
        if (IdentifierInfo *ID = TD->getIdentifier()) {
            return ID->getName().str();
        }

        if (const RecordDecl *RD_found = dyn_cast<RecordDecl>(TD)) {
            if (UnnamedTypedefMap.count(RD_found)) {
                return UnnamedTypedefMap[RD_found];
            }
        }
    }
    return QT.getAsString();
}

std::string getFullTypeName(QualType T, ASTContext &Ctx) {
    QualType CanonicalT = T.getCanonicalType();

    if (const RecordDecl *RD = CanonicalT->getAsRecordDecl()) {
        std::string name = RD->getNameAsString();

        if (name.empty()) {
            std::string typedefName = getOrCreateTypedefName(T, Ctx);
            if (!typedefName.empty()) {
                return typedefName;
            }
        }

        if (RD->isStruct())
            return "struct " + name;
        else if (RD->isUnion())
            return "union " + name;
        else if (RD->isClass())
            return "class " + name;

        return name;
    }

    if (const EnumType *ET = CanonicalT->getAs<EnumType>()) {
        if (const EnumDecl *ED = dyn_cast_or_null<EnumDecl>(ET->getAsTagDecl())) {
            return "enum " + ED->getNameAsString();
        }
    }

    return T.getAsString();
}

std::string generateTypedefForUnnamedType(QualType QT, ASTContext &Ctx) {
    if (const TagDecl *TD = QT->getAsTagDecl()) {
        if (!TD->getIdentifier()) {
            if (const RecordDecl *RD = dyn_cast<RecordDecl>(TD)) {
                std::string typedefName = getOrCreateTypedefName(QT, Ctx);
                std::string typeKind = RD->isUnion() ? "union" : "struct";

                std::string typeBody;
                for (const FieldDecl *FD : RD->fields()) {
                    QualType FT = FD->getType();
                    typeBody += "    " + FT.getAsString() + " " + FD->getNameAsString() + ";\n";
                }

                return "typedef " + typeKind + " {\n" + typeBody + "} " + typedefName + ";\n";
            }
        }
    }
    return "";
}

std::string generateConversionFunction(const FieldDecl *src_field, const FieldDecl *dst_field,
                                        int copySizeBytes, SourceLocation insertLoc,
                                        ASTContext &Ctx) {
    QualType srcType = src_field->getType();
    QualType dstType = dst_field->getType();
    return generateUnionConversionFunction(srcType, dstType, insertLoc, Ctx, copySizeBytes);
}

std::string generateFuncName(QualType srcType, QualType dstType, SourceLocation aLoc,
                              ASTContext &Ctx) {
    std::string srcStr = getOrCreateTypedefName(srcType, Ctx);
    std::string dstStr = getOrCreateTypedefName(dstType, Ctx);

    SourceManager &SM = Ctx.getSourceManager();
    unsigned line = SM.getSpellingLineNumber(aLoc);
    unsigned col = SM.getSpellingColumnNumber(aLoc);

    // bvm = bitcast via memcpy
    return sanitizeTypes("__tenjin_bvm_" + std::to_string(line) + "_" + std::to_string(col) + "_" +
                         srcStr + "_to_" + dstStr);
}

std::string generateUnionConversionFunction(QualType srcType, QualType dstType,
                                             SourceLocation unionLoc, ASTContext &Ctx,
                                             int copySizeBytes) {
    std::string funcName = generateFuncName(srcType, dstType, unionLoc, Ctx);

    auto asArray = [&](QualType T) -> const clang::ArrayType * { return Ctx.getAsArrayType(T); };

    const clang::ArrayType *srcArr = asArray(srcType);
    const clang::ArrayType *dstArr = asArray(dstType);

    QualType elemSrc = srcArr ? srcArr->getElementType() : srcType;
    QualType elemDst = dstArr ? dstArr->getElementType() : dstType;

    std::string elemSrcStr;
    if (elemSrc->isStructureType() || elemSrc->isUnionType()) {
        elemSrcStr = getFullTypeName(elemSrc, Ctx);
    } else {
        elemSrcStr = getOrCreateTypedefName(elemSrc, Ctx);
        if (elemSrcStr.empty())
            elemSrcStr = getFullTypeName(elemSrc, Ctx);
    }

    std::string elemDstStr;
    if (elemDst->isStructureType() || elemDst->isUnionType()) {
        elemDstStr = getFullTypeName(elemDst, Ctx);
    } else {
        elemDstStr = getOrCreateTypedefName(elemDst, Ctx);
        if (elemDstStr.empty())
            elemDstStr = getFullTypeName(elemDst, Ctx);
    }

    bool srcWasArray = (srcArr != nullptr);
    std::string srcParam;
    std::string memcpySrcExpr;
    if (srcWasArray) {
        srcParam = elemSrcStr + " *x";
        memcpySrcExpr = "x";
    } else {
        srcParam = elemSrcStr + " x";
        memcpySrcExpr = "&x";
    }

    std::string dstParam = elemDstStr + " *out";

    std::string code;
    code += "static void " + funcName + "(" + srcParam + ", " + dstParam + ") {\n";
    code += "    memcpy(out, " + memcpySrcExpr + ", " + std::to_string(copySizeBytes) + ");\n";
    code += "}\n";

    return code;
}
