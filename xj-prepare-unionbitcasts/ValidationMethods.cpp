#include "FunctionAccessAnalyzer.h"

// ============================================================================
// Validation Methods Implementation
// ============================================================================

bool FunctionAccessAnalyzer::hasUnsafeFunctionCall(const CompoundStmt *enclosingCompound,
                                                    const VarDecl *unionVar, ASTContext &Ctx) {
    if (!enclosingCompound || !unionVar)
        return false;

    class UnionFunctionCallChecker : public RecursiveASTVisitor<UnionFunctionCallChecker> {
      public:
        const VarDecl *targetVar;
        bool hasUnsafeCall = false;
        ASTContext &Ctx;

        UnionFunctionCallChecker(const VarDecl *var, ASTContext &ctx) : targetVar(var), Ctx(ctx) {}

        bool VisitCallExpr(CallExpr *CE) {
            // Check each argument to see if it's our union variable
            for (unsigned i = 0; i < CE->getNumArgs(); ++i) {
                const Expr *arg = CE->getArg(i)->IgnoreParenImpCasts();

                // Check if argument is &unionVar
                if (const UnaryOperator *UO = dyn_cast<UnaryOperator>(arg)) {
                    if (UO->getOpcode() == UO_AddrOf) {
                        const Expr *subExpr = UO->getSubExpr()->IgnoreParenImpCasts();
                        if (const DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(subExpr)) {
                            if (const VarDecl *VD = dyn_cast<VarDecl>(DRE->getDecl())) {
                                if (VD == targetVar) {
                                    hasUnsafeCall = true;
                                    return false; // stop traversal
                                }
                            }
                        }
                    }
                }

                // Check if argument is unionVar passed by reference
                if (const DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(arg)) {
                    if (const VarDecl *VD = dyn_cast<VarDecl>(DRE->getDecl())) {
                        if (VD == targetVar) {
                            // Check if this parameter is a reference type
                            if (i < CE->getNumArgs() && CE->getCalleeDecl()) {
                                if (const FunctionDecl *FD =
                                        dyn_cast<FunctionDecl>(CE->getCalleeDecl())) {
                                    if (i < FD->getNumParams()) {
                                        QualType paramType = FD->getParamDecl(i)->getType();
                                        // Unsafe if passed by reference or pointer
                                        if (paramType->isReferenceType() ||
                                            paramType->isPointerType()) {
                                            hasUnsafeCall = true;
                                            return false;
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
            return true;
        }
    };

    UnionFunctionCallChecker checker(unionVar, Ctx);
    checker.TraverseStmt(const_cast<CompoundStmt *>(enclosingCompound));
    return checker.hasUnsafeCall;
}

bool FunctionAccessAnalyzer::validateAccessSequence(const FunctionDecl *FD, const VarDecl *unionVar,
                                                     const std::vector<FieldAccess> &seq,
                                                     const FieldDecl *src_field,
                                                     const FieldDecl *dst_field, ASTContext &Ctx,
                                                     CFGBlock **outCutPoint,
                                                     std::unique_ptr<CFG> *outCFG) {
    // Create validation context
    ValidationContext ctx{FD, unionVar, seq, src_field, dst_field, Ctx,
                          nullptr, {}, {}, {}, {}, nullptr, false, {}, {}};

    // Step 1: Build CFG and dominator trees
    if (!buildCFGAnalysis(ctx))
        return false;

    // Step 2: Collect access blocks for src writes and dst accesses
    if (!collectAccessBlocks(ctx))
        return false;

    // Step 3: Find the cut point using dominance analysis
    if (!findCutPoint(ctx))
        return false;

    // Return cut point to caller
    if (outCutPoint) {
        *outCutPoint = ctx.cutPoint;
    }

    // Step 4: Validate source-level ordering (catches within-block issues)
    if (!validateSourceOrdering(ctx))
        return false;

    // Step 5: Check for unsafe function calls
    if (!validateNoUnsafeFunctionCalls(ctx))
        return false;

    // Step 6: Validate array writes (all indices must be written)
    if (!validateArrayWrites(ctx))
        return false;

    // Step 7: Validate struct writes (all members must be written)
    if (!validateStructWrites(ctx))
        return false;

    // Transfer CFG ownership to caller so cutPoint remains valid
    if (outCFG) {
        *outCFG = std::move(ctx.cfg);
    }

    return true;
}

// ============================================================================
// Modular Validation Methods
// ============================================================================

bool FunctionAccessAnalyzer::buildCFGAnalysis(ValidationContext &ctx) {
    Stmt *Body = ctx.FD->getBody();
    if (!Body) {
        gLog.error = "Function has no body";
        if (VERBOSE)
            llvm::outs() << "[Error] " + gLog.error + "\n";
        return false;
    }

    CFG::BuildOptions BO;
    ctx.cfg = CFG::buildCFG(ctx.FD, Body, &ctx.Ctx, BO);
    if (!ctx.cfg) {
        gLog.error = "Failed to build CFG for function";
        if (VERBOSE)
            llvm::outs() << "[Error] " + gLog.error + "\n";
        return false;
    }

    if (VERBOSE) {
        llvm::outs() << "=== CFG for function " << ctx.FD->getNameAsString() << " ===\n";
        llvm::outs().flush();
        ctx.cfg->print(llvm::errs(), ctx.Ctx.getLangOpts(), true);
        llvm::errs() << "=== End CFG ===\n";
        llvm::errs().flush();
    }

    ctx.DT.buildDominatorTree(ctx.cfg.get());
    ctx.PDT.buildDominatorTree(ctx.cfg.get());

    // Setup helper: Check if a statement contains another statement recursively
    ctx.contains = [](const Stmt *haystack, const Stmt *needle) -> bool {
        if (!haystack || !needle)
            return false;
        if (haystack == needle)
            return true;

        std::function<bool(const Stmt *, const Stmt *)> search = [&](const Stmt *h,
                                                                      const Stmt *n) -> bool {
            if (!h)
                return false;
            if (h == n)
                return true;
            for (const Stmt *child : h->children()) {
                if (search(child, n))
                    return true;
            }
            return false;
        };
        return search(haystack, needle);
    };

    // Setup helper: Find CFG block containing a statement or expression
    ctx.findBlockForStmt = [&ctx](const Stmt *S) -> CFGBlock * {
        if (!S)
            return nullptr;

        for (CFGBlock *block : *ctx.cfg) {
            for (CFGElement elem : *block) {
                if (auto stmt = elem.getAs<CFGStmt>()) {
                    const Stmt *cfgStmt = stmt->getStmt();
                    if (cfgStmt == S || ctx.contains(cfgStmt, S)) {
                        return block;
                    }
                }
            }
        }
        return nullptr;
    };

    return true;
}

bool FunctionAccessAnalyzer::collectAccessBlocks(ValidationContext &ctx) {
    Stmt *Body = ctx.FD->getBody();

    // Check for initialization writes (expr == nullptr)
    ctx.hasInitWrite = false;
    for (const auto &a : ctx.seq) {
        if (!a.expr && a.field == ctx.src_field && a.is_write) {
            ctx.hasInitWrite = true;
            break;
        }
    }

    // If there's an initialization write, find the declaration block
    if (ctx.hasInitWrite && ctx.unionVar) {
        const DeclStmt *DS = findDeclStmtForVar(ctx.unionVar, Body);
        if (DS) {
            CFGBlock *declBlock = ctx.findBlockForStmt(DS);
            if (declBlock) {
                ctx.srcWriteBlocks.push_back(declBlock);
                if (VERBOSE)
                    llvm::outs() << "[Debug] Found declaration block for initialization write\n";
            } else if (VERBOSE) {
                llvm::outs()
                    << "[Debug] DeclStmt found but no CFG block for it - will skip init check\n";
            }
        } else if (VERBOSE) {
            llvm::outs() << "[Debug] Could not find DeclStmt for union variable\n";
        }
    }

    // Categorize accesses into src writes and dst accesses
    for (const auto &a : ctx.seq) {
        if (!a.expr)
            continue;

        CFGBlock *block = ctx.findBlockForStmt(a.expr);
        if (!block) {
            gLog.error = "Could not find CFG block for access expression";
            if (VERBOSE)
                llvm::outs() << "[Error] " + gLog.error + "\n";
            return false;
        }

        if (a.field == ctx.src_field && a.is_write) {
            ctx.srcWriteBlocks.push_back(block);
        } else if (a.field == ctx.dst_field) {
            ctx.dstAccessBlocks.push_back(block);
        }
    }

    return true;
}

bool FunctionAccessAnalyzer::findCutPoint(ValidationContext &ctx) {
    // Deduplicate destination access blocks
    std::vector<CFGBlock *> uniqueDstBlocks;
    for (CFGBlock *block : ctx.dstAccessBlocks) {
        if (std::find(uniqueDstBlocks.begin(), uniqueDstBlocks.end(), block) ==
            uniqueDstBlocks.end()) {
            uniqueDstBlocks.push_back(block);
        }
    }

    if (ctx.srcWriteBlocks.empty()) {
        if (!ctx.hasInitWrite) {
            gLog.error = "No source write blocks found and no initialization";
            if (VERBOSE)
                llvm::outs() << "[Error] " + gLog.error + "\n";
            return false;
        }
        // Init-only case: use the first destination access block as cut point
        if (uniqueDstBlocks.empty()) {
            gLog.error = "No destination access blocks found";
            if (VERBOSE)
                llvm::outs() << "[Error] " + gLog.error + "\n";
            return false;
        }
        ctx.cutPoint = uniqueDstBlocks[0];
        if (VERBOSE)
            llvm::outs() << "[Debug] Init-only case: using first dst block as cut point\n";
    } else {
        // Normal case: need transformation from src to dst
        if (ctx.dstAccessBlocks.empty()) {
            gLog.error = "No transformation from src to dst field";
            if (VERBOSE)
                llvm::outs() << "[Error] " + gLog.error + "\n";
            return false;
        }

        // Find blocks that post-dominate all src write blocks
        std::vector<CFGBlock *> candidates;
        for (CFGBlock *block : *ctx.cfg) {
            if (!block)
                continue;

            bool postDominatesAllWrites = true;
            for (CFGBlock *writeBlock : ctx.srcWriteBlocks) {
                if (!ctx.PDT.dominates(block, writeBlock)) {
                    postDominatesAllWrites = false;
                    break;
                }
            }

            if (postDominatesAllWrites) {
                candidates.push_back(block);
            }
        }

        if (candidates.empty()) {
            gLog.error = "No block post-dominates all source writes";
            if (VERBOSE)
                llvm::outs() << "[Error] " + gLog.error + "\n";
            return false;
        }

        // Pick the first candidate that also dominates all destination accesses
        ctx.cutPoint = nullptr;
        for (CFGBlock *candidate : candidates) {
            bool dominatesAllAccesses = true;
            for (CFGBlock *accessBlock : ctx.dstAccessBlocks) {
                if (!ctx.DT.dominates(candidate, accessBlock) && candidate != accessBlock) {
                    dominatesAllAccesses = false;
                    break;
                }
            }

            if (dominatesAllAccesses) {
                ctx.cutPoint = candidate;
                break;
            }
        }

        if (!ctx.cutPoint) {
            gLog.error = "No cut point found that separates src writes from dst accesses";
            if (VERBOSE)
                llvm::outs() << "[Error] " + gLog.error + "\n";
            return false;
        }
    }

    if (VERBOSE) {
        llvm::outs() << "[Debug] Found cut point: CFG Block B" << ctx.cutPoint->getBlockID() << "\n";
        llvm::outs() << "[Debug] Source write blocks: ";
        for (CFGBlock *block : ctx.srcWriteBlocks) {
            llvm::outs() << "B" << block->getBlockID() << " ";
        }
        llvm::outs() << "\n[Debug] Destination access blocks: ";
        for (CFGBlock *block : ctx.dstAccessBlocks) {
            llvm::outs() << "B" << block->getBlockID() << " ";
        }
        llvm::outs() << "\n";
    }

    return true;
}

bool FunctionAccessAnalyzer::validateSourceOrdering(ValidationContext &ctx) {
    // Ensure all src accesses occur before all dst accesses in source order
    // This catches within-block ordering issues that CFG analysis can't detect
    bool in_src_accesses_section = true;
    for (size_t i = 0; i < ctx.seq.size(); ++i) {
        if (ctx.seq[i].field == ctx.dst_field && in_src_accesses_section) {
            in_src_accesses_section = false;
        }
        if (ctx.seq[i].field == ctx.src_field && !in_src_accesses_section) {
            gLog.error = "Must have all src accesses before all dst accesses";
            if (VERBOSE)
                llvm::outs() << "[Error] " + gLog.error + "\n";
            return false;
        }
    }
    return true;
}

bool FunctionAccessAnalyzer::validateNoUnsafeFunctionCalls(ValidationContext &ctx) {
    const CompoundStmt *enclosingCompound = nullptr;
    for (const auto &a : ctx.seq) {
        if (!a.expr)
            continue;
        const CompoundStmt *C = findEnclosingStmt<CompoundStmt>(a.expr, ctx.Ctx);
        if (C) {
            enclosingCompound = C;
            break;
        }
    }

    if (!ctx.seq.empty()) {
        const VarDecl *checkUnionVar = nullptr;
        for (const auto &a : ctx.seq) {
            if (!a.expr)
                continue;
            const Expr *base = a.expr->getBase()->IgnoreParenImpCasts();
            if (const DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(base)) {
                if (const VarDecl *VD = dyn_cast<VarDecl>(DRE->getDecl())) {
                    checkUnionVar = VD;
                    break;
                }
            }
        }

        if (checkUnionVar && hasUnsafeFunctionCall(enclosingCompound, checkUnionVar, ctx.Ctx)) {
            gLog.error =
                "Union variable passed to function by pointer/reference - unsafe for conversion";
            if (VERBOSE)
                llvm::outs() << "[Error] " + gLog.error + "\n";
            return false;
        }
    }
    return true;
}

bool FunctionAccessAnalyzer::validateArrayWrites(ValidationContext &ctx) {
    const clang::ArrayType *srcArrayType = ctx.Ctx.getAsArrayType(ctx.src_field->getType());
    if (!srcArrayType)
        return true;

    const clang::ConstantArrayType *constArrType =
        dyn_cast<clang::ConstantArrayType>(srcArrayType);
    if (!constArrType)
        return true;

    uint64_t arraySize = constArrType->getSize().getZExtValue();

    // Collect all indices that were written
    std::set<int64_t> writtenIndices;
    bool hasNonConstantIndex = false;

    for (const auto &a : ctx.seq) {
        if (a.field == ctx.src_field && a.is_write) {
            if (a.array_index >= 0) {
                writtenIndices.insert(a.array_index);
            } else if (a.array_index == -1 && a.expr != nullptr) {
                const ArraySubscriptExpr *ASE =
                    findEnclosingStmt<ArraySubscriptExpr>(a.expr, ctx.Ctx);
                if (ASE && ASE->getBase()->IgnoreParenImpCasts() == a.expr) {
                    hasNonConstantIndex = true;
                }
            }
        }
    }

    if (hasNonConstantIndex) {
        gLog.error = "Array has non-constant index - cannot validate all elements written";
        if (VERBOSE)
            llvm::outs() << "[Error] " + gLog.error + "\n";
        return false;
    }

    // Check if all indices from 0 to arraySize-1 are written
    for (uint64_t i = 0; i < arraySize; ++i) {
        if (writtenIndices.find(i) == writtenIndices.end()) {
            gLog.error = "Array source field missing write to index " + std::to_string(i) +
                         " (array size: " + std::to_string(arraySize) + ")";
            if (VERBOSE)
                llvm::outs() << "[Error] " + gLog.error + "\n";
            return false;
        }
    }

    if (VERBOSE)
        llvm::outs() << "[Debug] All array indices written (array size: " << arraySize << ")\n";

    return true;
}

bool FunctionAccessAnalyzer::validateStructWrites(ValidationContext &ctx) {
    const RecordDecl *srcRecord = ctx.src_field->getType()->getAsRecordDecl();
    if (!srcRecord || !srcRecord->isStruct())
        return true;

    // Collect all struct members that need to be written
    std::set<const FieldDecl *> allMembers;
    for (const FieldDecl *field : srcRecord->fields()) {
        allMembers.insert(field);
    }

    // Collect all members that were written
    std::set<const FieldDecl *> writtenMembers;
    for (const auto &a : ctx.seq) {
        if (a.field == ctx.src_field && a.is_write && a.struct_member) {
            writtenMembers.insert(a.struct_member);
        }
    }

    // Check if all members were written
    for (const FieldDecl *member : allMembers) {
        if (writtenMembers.find(member) == writtenMembers.end()) {
            gLog.error =
                "Struct source field missing write to member '" + member->getNameAsString() + "'";
            if (VERBOSE)
                llvm::outs() << "[Error] " + gLog.error + "\n";
            return false;
        }
    }

    if (VERBOSE)
        llvm::outs() << "[Debug] All struct members written (struct has " << allMembers.size()
                     << " members)\n";

    return true;
}
