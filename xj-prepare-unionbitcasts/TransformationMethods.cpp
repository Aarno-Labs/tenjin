#include "FunctionAccessAnalyzer.h"

// ============================================================================
// Modular Transformation Methods
// ============================================================================

FunctionAccessAnalyzer::InsertionInfo
FunctionAccessAnalyzer::analyzeCutPoint(TransformContext &ctx) {
    InsertionInfo info;

    if (!ctx.cutPoint || ctx.cutPoint->size() == 0)
        return info;

    // Collect all destination access expressions (reads AND writes) and source write expressions
    std::set<const Stmt *> dstAccesses;
    std::set<const Stmt *> srcWrites;
    for (const auto &a : ctx.seq) {
        if (a.expr && a.field == ctx.dst_field) {
            dstAccesses.insert(a.expr);
        }
        if (a.expr && a.field == ctx.src_field && a.is_write) {
            srcWrites.insert(a.expr);
        }
    }

    if (VERBOSE) {
        llvm::outs() << "[Debug] Cut point analysis: " << srcWrites.size() << " source writes, "
                     << dstAccesses.size() << " dest accesses\n";
    }

    // Helper to recursively check if a statement contains a target expression
    std::function<bool(const Stmt *, const Stmt *, int)> stmtContainsImpl =
        [&](const Stmt *haystack, const Stmt *needle, int depth) -> bool {
        if (!haystack || !needle)
            return false;
        if (depth > 100)
            return false;
        if (haystack == needle)
            return true;
        for (const Stmt *child : haystack->children()) {
            if (child && stmtContainsImpl(child, needle, depth + 1))
                return true;
        }
        return false;
    };

    // Cache to avoid redundant stmtContains calls
    std::map<std::pair<const Stmt *, const Stmt *>, bool> containsCache;
    auto cachedStmtContains = [&](const Stmt *haystack, const Stmt *needle) -> bool {
        auto key = std::make_pair(haystack, needle);
        auto it = containsCache.find(key);
        if (it != containsCache.end())
            return it->second;
        bool result = stmtContainsImpl(haystack, needle, 0);
        containsCache[key] = result;
        return result;
    };

    // Check if cut point contains any writes or reads
    bool cutPointHasWrite = false;
    bool cutPointHasRead = false;
    const Stmt *lastWriteStmt = nullptr;
    const Stmt *firstReadStmt = nullptr;
    const Stmt *firstNonDeclStmt = nullptr;

    for (const CFGElement &elem : *ctx.cutPoint) {
        if (auto cfgStmt = elem.getAs<CFGStmt>()) {
            const Stmt *stmt = cfgStmt->getStmt();
            if (!stmt)
                continue;

            // Skip union declaration
            if (const DeclStmt *DS = dyn_cast<DeclStmt>(stmt)) {
                if (DS->isSingleDecl() && DS->getSingleDecl() == ctx.VD)
                    continue;
            }

            if (!firstNonDeclStmt)
                firstNonDeclStmt = stmt;

            // Check for source writes
            for (const Stmt *writeExpr : srcWrites) {
                if (writeExpr && cachedStmtContains(stmt, writeExpr)) {
                    cutPointHasWrite = true;
                    lastWriteStmt = stmt;
                    break;
                }
            }

            // Check for destination accesses
            if (!firstReadStmt) {
                for (const Stmt *accessExpr : dstAccesses) {
                    if (accessExpr && cachedStmtContains(stmt, accessExpr)) {
                        cutPointHasRead = true;
                        firstReadStmt = stmt;
                        break;
                    }
                }
            }
        }
    }

    // Helper to find top-level statement
    auto findTopLevelStmt = [&](const Stmt *stmt) -> const Stmt * {
        const Stmt *topLevel = stmt;
        while (topLevel) {
            auto Parents = ctx.Ctx.getParents(*topLevel);
            if (Parents.empty())
                break;
            const Stmt *parentStmt = Parents[0].get<Stmt>();
            if (!parentStmt || isa<CompoundStmt>(parentStmt) || isa<DoStmt>(parentStmt))
                break;
            topLevel = parentStmt;
        }
        return topLevel;
    };

    // Helper to determine insertion location for a statement
    auto getInsertionForStmt = [&](const Stmt *stmt, bool atBottom) {
        const Stmt *topLevel = findTopLevelStmt(stmt);
        SourceLocation stmtLoc = atBottom ? Lexer::getLocForEndOfToken(topLevel->getEndLoc(), 0,
                                                                        ctx.SM, ctx.LO)
                                          : topLevel->getBeginLoc();

        if (atBottom) {
            info.point = stmtLoc;
            info.atBottom = true;
            return;
        }

        unsigned line = ctx.SM.getSpellingLineNumber(stmtLoc);
        FileID FID = ctx.SM.getFileID(stmtLoc);
        SourceLocation lineStart = ctx.SM.translateLineCol(FID, line, 1);
        StringRef lineText =
            Lexer::getSourceText(CharSourceRange::getCharRange(lineStart, stmtLoc), ctx.SM, ctx.LO);
        bool stmtAtLineStart = lineText.trim().empty();

        info.point = stmtAtLineStart ? lineStart : stmtLoc;
        info.midLine = !stmtAtLineStart;
        info.atBottom = false;
    };

    // Determine insertion strategy
    if (cutPointHasRead) {
        getInsertionForStmt(firstReadStmt, false);
    } else if (cutPointHasWrite) {
        getInsertionForStmt(lastWriteStmt, true);
    } else if (firstNonDeclStmt) {
        getInsertionForStmt(firstNonDeclStmt, false);
    }

    return info;
}

void FunctionAccessAnalyzer::generateDeclarations(TransformContext &ctx) {
    // Generate tmp variable names
    ctx.tmp_in_name = "__tenjin_tmp_in_" + ctx.VD->getNameAsString();
    ctx.tmp_out_name = "__tenjin_tmp_out_" + ctx.VD->getNameAsString();

    // Generate typedefs for unnamed types
    ctx.src_typedef_code = generateTypedefForUnnamedType(ctx.src_field->getType(), ctx.Ctx);
    if (generatedObjects.find(ctx.src_typedef_code) != generatedObjects.end())
        ctx.src_typedef_code = "";
    else
        generatedObjects.insert(ctx.src_typedef_code);

    ctx.dst_typedef_code = generateTypedefForUnnamedType(ctx.dst_field->getType(), ctx.Ctx);
    if (generatedObjects.find(ctx.dst_typedef_code) != generatedObjects.end())
        ctx.dst_typedef_code = "";
    else
        generatedObjects.insert(ctx.dst_typedef_code);

    // Find union declaration locations
    const DeclStmt *declStmt = findEnclosingStmt<DeclStmt>(ctx.VD, ctx.Ctx);
    ctx.union_start = declStmt->getBeginLoc();
    ctx.union_end = Lexer::getLocForEndOfToken(declStmt->getEndLoc(), 0, ctx.SM, ctx.LO);

    // Get indentation
    ctx.indentation = getIndentBeforeLoc(ctx.union_start, ctx.SM).str();

    // Generate tmp declarations string
    std::string src_and_dst_tmp_decls =
        generateTmpDeclaration(ctx.tmp_in_name, ctx.src_field->getType(), ctx.Ctx) +
        ctx.indentation +
        generateTmpDeclaration(ctx.tmp_out_name, ctx.dst_field->getType(), ctx.Ctx);

    // Try to delete the union declaration if safe, otherwise just insert before it
    if (!tryDeleteUnionDecl(ctx, declStmt)) {
        ctx.edits.push_back({Edit::InsertBefore, ctx.SM.getFileOffset(ctx.union_start),
                             ctx.union_start, SourceLocation(),
                             src_and_dst_tmp_decls + ctx.indentation});
    }
}

bool FunctionAccessAnalyzer::tryDeleteUnionDecl(TransformContext &ctx, const DeclStmt *declStmt) {
    // Check if we can safely delete the union declaration
    // Safe to delete if: anonymous union type AND single variable in declaration
    // Note: For "union { ... } var;", isSingleDecl() returns false because the DeclStmt
    // contains both the RecordDecl (union type) and VarDecl (variable). We need to check
    // that the only decls are the union type and our variable.

    QualType varType = ctx.VD->getType();
    const RecordDecl *RD = varType->getAsRecordDecl();
    if (!RD) {
        if (VERBOSE)
            llvm::outs() << "[Debug] tryDeleteUnionDecl: no RecordDecl\n";
        return false;
    }
    if (!RD->isUnion()) {
        if (VERBOSE)
            llvm::outs() << "[Debug] tryDeleteUnionDecl: not a union\n";
        return false;
    }
    if (RD->getIdentifier()) {
        if (VERBOSE)
            llvm::outs() << "[Debug] tryDeleteUnionDecl: union has identifier '"
                         << RD->getIdentifier()->getName() << "'\n";
        return false;
    }

    // Check if this DeclStmt only contains the union type and our variable
    // (not other variables declared with the same union type)
    int varDeclCount = 0;
    bool hasOurVar = false;
    bool hasUnionDecl = false;
    for (const Decl *D : declStmt->decls()) {
        if (const VarDecl *VD = dyn_cast<VarDecl>(D)) {
            varDeclCount++;
            if (VD == ctx.VD)
                hasOurVar = true;
        } else if (const RecordDecl *DeclRD = dyn_cast<RecordDecl>(D)) {
            if (DeclRD == RD)
                hasUnionDecl = true;
        }
    }

    // Only allow deletion if there's exactly one variable and it's ours
    if (varDeclCount != 1 || !hasOurVar) {
        if (VERBOSE)
            llvm::outs() << "[Debug] tryDeleteUnionDecl: multiple variables or not our var "
                         << "(varDeclCount=" << varDeclCount << ", hasOurVar=" << hasOurVar << ")\n";
        return false;
    }

    // Check if this is an init-only case (no source access with expression)
    // In that case, handleInitOnlyCase will insert content after union_end,
    // so we can't safely delete the declaration
    bool hasSourceExpr = std::any_of(ctx.seq.begin(), ctx.seq.end(), [&](const FieldAccess &a) {
        return a.field == ctx.src_field && a.expr != nullptr;
    });
    if (!hasSourceExpr) {
        if (VERBOSE)
            llvm::outs() << "[Debug] tryDeleteUnionDecl: no source expression (init-only case)\n";
        return false;
    }

    // Anonymous union with single variable and non-init case - safe to delete

    // Generate tmp declarations string
    std::string src_and_dst_tmp_decls =
        generateTmpDeclaration(ctx.tmp_in_name, ctx.src_field->getType(), ctx.Ctx) +
        ctx.indentation +
        generateTmpDeclaration(ctx.tmp_out_name, ctx.dst_field->getType(), ctx.Ctx) +
        ctx.indentation;

    // Replace union declaration with just the tmp declarations
    // Note: Don't extend past union_end - other edits may insert content there
    ctx.edits.push_back({Edit::Replace, ctx.SM.getFileOffset(ctx.union_start),
                         ctx.union_start, ctx.union_end,
                         src_and_dst_tmp_decls});

    if (VERBOSE)
        llvm::outs() << "[Debug] Deleting anonymous union declaration\n";

    return true;
}

bool FunctionAccessAnalyzer::handleInitOnlyCase(TransformContext &ctx) {
    // Check if there's a source access with an expression
    auto lastSrcAccessIt =
        std::find_if(ctx.seq.rbegin(), ctx.seq.rend(), [&](const FieldAccess &a) {
            return a.field == ctx.src_field && a.expr != nullptr;
        });

    if (lastSrcAccessIt != ctx.seq.rend())
        return false; // Not an init-only case

    // Check for initialization-only write
    auto initAccessIt = std::find_if(ctx.seq.begin(), ctx.seq.end(), [&](const FieldAccess &a) {
        return a.field == ctx.src_field && a.is_write && a.expr == nullptr;
    });

    if (initAccessIt == ctx.seq.end()) {
        gLog.error = "Could not identify last source access";
        if (VERBOSE)
            llvm::outs() << "[Error] " + gLog.error + "\n";
        return true; // Handled (as error)
    }

    // Extract initializer and generate init code
    std::string initCode;
    if (ctx.VD->hasInit()) {
        if (const InitListExpr *ILE = dyn_cast<InitListExpr>(ctx.VD->getInit())) {
            if (ILE->getNumInits() > 0) {
                const Expr *initExpr = ILE->getInit(0);
                SourceRange initRange = initExpr->getSourceRange();
                SourceLocation initStart = initRange.getBegin();
                SourceLocation initEnd =
                    Lexer::getLocForEndOfToken(initRange.getEnd(), 0, ctx.SM, ctx.LO);

                if (initStart.isValid() && initEnd.isValid()) {
                    StringRef initText = Lexer::getSourceText(
                        CharSourceRange::getCharRange(initStart, initEnd), ctx.SM, ctx.LO);

                    if (ctx.Ctx.getAsArrayType(ctx.src_field->getType())) {
                        std::string arrayTypeStr = ctx.src_field->getType().getAsString();
                        initCode = "\n" + ctx.indentation + "memcpy(" + ctx.tmp_in_name + ", (" +
                                   arrayTypeStr + ")" + initText.str() + ", sizeof(" +
                                   ctx.tmp_in_name + "));";
                    } else {
                        initCode = "\n" + ctx.indentation + ctx.tmp_in_name + " = " +
                                   initText.str() + ";";
                    }
                }
            }
        }
    }

    // Generate conversion call
    std::string conversion_func_call;
    bool isArrayDst = ctx.Ctx.getAsArrayType(ctx.dst_field->getType()) != nullptr;
    if (isArrayDst) {
        conversion_func_call = "\n" + ctx.indentation + ctx.funcName + "(" + ctx.tmp_in_name +
                               ", " + ctx.tmp_out_name + ");";
    } else {
        conversion_func_call = "\n" + ctx.indentation + ctx.funcName + "(" + ctx.tmp_in_name +
                               ", &" + ctx.tmp_out_name + ");";
    }

    ctx.edits.push_back({Edit::InsertAfterToken, ctx.SM.getFileOffset(ctx.union_end),
                         ctx.union_end, SourceLocation(), initCode + conversion_func_call});

    // Replace dst accesses
    for (const auto &a : ctx.seq) {
        if (!a.expr || a.field != ctx.dst_field)
            continue;

        SourceLocation meStart = a.expr->getSourceRange().getBegin();
        SourceLocation meEnd =
            Lexer::getLocForEndOfToken(a.expr->getSourceRange().getEnd(), 0, ctx.SM, ctx.LO);

        if (meStart.isValid() && meEnd.isValid()) {
            std::string replacement = ctx.tmp_out_name;
            if (a.struct_member)
                replacement += "." + a.struct_member->getNameAsString();
            ctx.edits.push_back(
                {Edit::Replace, ctx.SM.getFileOffset(meStart), meStart, meEnd, replacement});
        }
    }

    emitTypedefAndFunction(ctx);
    return true;
}

void FunctionAccessAnalyzer::collectReplacementEdits(TransformContext &ctx) {
    for (const auto &a : ctx.seq) {
        if (!a.expr)
            continue;

        // Skip synthetic tracking records for compound literal writes
        if (a.struct_member && a.field->getType()->isStructureType()) {
            const MemberExpr *ME = a.expr;
            if (ME && ME->getMemberDecl() == a.field)
                continue;
        }

        const ArraySubscriptExpr *accessASE =
            findEnclosingStmt<ArraySubscriptExpr>(a.expr, ctx.Ctx);
        bool isArrayBase = false;
        if (accessASE) {
            const Expr *base = accessASE->getBase()->IgnoreParenImpCasts();
            isArrayBase = (base == a.expr);
        }

        SourceLocation meStart = a.expr->getSourceRange().getBegin();
        SourceLocation meEnd = isArrayBase
                                   ? Lexer::getLocForEndOfToken(a.expr->getMemberLoc(), 0, ctx.SM,
                                                                ctx.LO)
                                   : Lexer::getLocForEndOfToken(a.expr->getSourceRange().getEnd(),
                                                                0, ctx.SM, ctx.LO);

        if (!meStart.isValid() || !meEnd.isValid())
            continue;

        std::string replacement;
        if (a.field == ctx.src_field) {
            replacement = ctx.tmp_in_name;
        } else if (a.field == ctx.dst_field) {
            replacement = ctx.tmp_out_name;
        } else {
            continue;
        }

        if (a.struct_member)
            replacement += "." + a.struct_member->getNameAsString();

        ctx.edits.push_back({Edit::Replace, ctx.SM.getFileOffset(meStart), meStart, meEnd,
                             replacement});
    }
}

void FunctionAccessAnalyzer::emitConversionCall(TransformContext &ctx,
                                                  const InsertionInfo &info) {
    bool isArrayDst = ctx.Ctx.getAsArrayType(ctx.dst_field->getType()) != nullptr;

    // Find last source access statement if no valid insertion point
    SourceLocation fallbackLoc;
    const Stmt *lastAccessStmt = nullptr;

    if (!info.point.isValid()) {
        auto lastSrcAccessIt =
            std::find_if(ctx.seq.rbegin(), ctx.seq.rend(), [&](const FieldAccess &a) {
                return a.field == ctx.src_field && a.expr != nullptr;
            });

        if (lastSrcAccessIt != ctx.seq.rend()) {
            const Expr *exprToFind = lastSrcAccessIt->expr;
            const ArraySubscriptExpr *ASE =
                findEnclosingStmt<ArraySubscriptExpr>(lastSrcAccessIt->expr, ctx.Ctx);
            if (ASE && ASE->getBase()->IgnoreParenImpCasts() == lastSrcAccessIt->expr)
                exprToFind = ASE;

            const CallExpr *CE = findEnclosingStmt<CallExpr>(exprToFind, ctx.Ctx);
            const BinaryOperator *BO = findEnclosingStmt<BinaryOperator>(exprToFind, ctx.Ctx);
            const UnaryOperator *UO = findEnclosingStmt<UnaryOperator>(exprToFind, ctx.Ctx);

            if (CE)
                lastAccessStmt = CE;
            else if (BO)
                lastAccessStmt = BO;
            else if (UO)
                lastAccessStmt = UO;
            else
                lastAccessStmt = findEnclosingStmt<Stmt>(exprToFind, ctx.Ctx);
        }

        if (!lastAccessStmt) {
            gLog.error = "Couldn't find last access statement";
            return;
        }

        fallbackLoc = Lexer::getLocForEndOfToken(lastAccessStmt->getEndLoc(), 0, ctx.SM, ctx.LO);
    }

    // Generate conversion call string
    std::string callStr;
    if (info.point.isValid()) {
        if (info.atBottom) {
            callStr = "\n" + ctx.indentation + ctx.funcName + "(" + ctx.tmp_in_name + ", " +
                      (isArrayDst ? "" : "&") + ctx.tmp_out_name + ");";
            ctx.edits.push_back({Edit::InsertAfterToken, ctx.SM.getFileOffset(info.point),
                                 info.point, SourceLocation(), callStr});
        } else if (info.midLine) {
            callStr = ctx.funcName + "(" + ctx.tmp_in_name + ", " + (isArrayDst ? "" : "&") +
                      ctx.tmp_out_name + "); ";
            ctx.edits.push_back({Edit::InsertBefore, ctx.SM.getFileOffset(info.point), info.point,
                                 SourceLocation(), callStr});
        } else {
            callStr = ctx.indentation + ctx.funcName + "(" + ctx.tmp_in_name + ", " +
                      (isArrayDst ? "" : "&") + ctx.tmp_out_name + ");\n";
            ctx.edits.push_back({Edit::InsertBefore, ctx.SM.getFileOffset(info.point), info.point,
                                 SourceLocation(), callStr});
        }
    } else {
        callStr = "\n" + ctx.indentation + ctx.funcName + "(" + ctx.tmp_in_name + ", " +
                  (isArrayDst ? "" : "&") + ctx.tmp_out_name + ");";

        if (isa<CallExpr>(lastAccessStmt)) {
            SourceLocation afterSemi =
                Lexer::findLocationAfterToken(fallbackLoc, tok::semi, ctx.SM, ctx.LO, false);
            if (afterSemi.isValid()) {
                ctx.edits.push_back({Edit::InsertBefore, ctx.SM.getFileOffset(afterSemi),
                                     afterSemi, SourceLocation(), callStr});
            } else {
                ctx.edits.push_back({Edit::InsertAfterToken, ctx.SM.getFileOffset(fallbackLoc),
                                     fallbackLoc, SourceLocation(), callStr});
            }
        } else {
            ctx.edits.push_back({Edit::InsertAfterToken, ctx.SM.getFileOffset(fallbackLoc),
                                 fallbackLoc, SourceLocation(), callStr});
        }
    }
}

void FunctionAccessAnalyzer::emitTypedefAndFunction(TransformContext &ctx) {
    if (generatedObjects.find(ctx.funcName) == generatedObjects.end()) {
        SourceLocation funcStart = ctx.FD->getSourceRange().getBegin();
        ctx.edits.push_back({Edit::InsertBefore, ctx.SM.getFileOffset(funcStart), funcStart,
                             SourceLocation(),
                             ctx.src_typedef_code + ctx.dst_typedef_code + ctx.funcCode + "\n"});
        generatedObjects.insert(ctx.funcName);
    }
}

// ============================================================================
// Main Transformation Entry Point
// ============================================================================

bool FunctionAccessAnalyzer::emitTransformation(const FunctionDecl *FD, const VarDecl *VD,
                                         const std::vector<FieldAccess> &seq,
                                         const FieldDecl *src_field, const FieldDecl *dst_field,
                                         const std::string &srcC, const std::string &funcName,
                                         const std::string &funcCode, ASTContext &Ctx,
                                         CFGBlock *cutPoint) {
    // Initialize transformation context
    TransformContext ctx{FD,       VD,       seq,      src_field,     dst_field,
                         funcName, funcCode, Ctx,      Ctx.getSourceManager(),
                         Ctx.getLangOpts(),  cutPoint, "",
                         "",       "",       "",       "",
                         SourceLocation(),   SourceLocation(), {}};

    // Ensure memcpy is declared
    ensureMemcpyDeclared(FD, Ctx, ctx.edits);

    // Analyze cut point for insertion location
    InsertionInfo insertInfo = analyzeCutPoint(ctx);

    // Generate declarations and typedefs
    generateDeclarations(ctx);

    // Handle initialization-only case
    if (handleInitOnlyCase(ctx)) {
        applyEdits(ctx.edits, ctx.SM);
        return true;
    }

    // Collect replacement edits for member expressions
    collectReplacementEdits(ctx);

    // Emit conversion function call
    emitConversionCall(ctx, insertInfo);

    // Emit typedef and conversion function definition
    emitTypedefAndFunction(ctx);

    // Apply all edits
    applyEdits(ctx.edits, ctx.SM);

    if (VERBOSE)
        llvm::outs() << "Rewrote union pun for variable '" << VD->getNameAsString() << "' using "
                     << funcName << " with tmps " << ctx.tmp_in_name << " " << ctx.tmp_out_name
                     << "\n";

    return true;
}
