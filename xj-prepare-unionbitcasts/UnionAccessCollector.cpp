#include "UnionAccessCollector.h"

// ============================================================================
// UnionAccessCollector Implementation
// ============================================================================

UnionAccessCollector::UnionAccessCollector(ASTContext &Ctx) : Ctx(Ctx) {}

bool UnionAccessCollector::VisitMemberExpr(MemberExpr *ME) {
    const FieldDecl *FD = dyn_cast<FieldDecl>(ME->getMemberDecl());
    if (!FD)
        return true;

    if (VERBOSE)
        llvm::outs() << "[Debug] Visiting MemberExpr: " << FD->getNameAsString() << "\n";

    const Expr *base = ME->getBase()->IgnoreParenImpCasts();
    const VarDecl *ownerVar = nullptr;
    const FieldDecl *structMember = nullptr;
    const RecordDecl *parentRD = nullptr;

    // Check if base is a MemberExpr accessing a union field (for nested access like u.x.a)
    if (const MemberExpr *baseME = dyn_cast<MemberExpr>(base)) {
        const FieldDecl *baseFD = dyn_cast<FieldDecl>(baseME->getMemberDecl());
        if (baseFD) {
            const RecordDecl *baseParentRD = nullptr;
            for (auto &P : Ctx.getParents(*baseFD)) {
                if (auto RD = P.get<RecordDecl>()) {
                    baseParentRD = RD;
                    break;
                }
            }
            if (baseParentRD && baseParentRD->isUnion()) {
                structMember = FD;
                FD = baseFD;
                parentRD = baseParentRD;

                const Expr *baseBase = baseME->getBase()->IgnoreParenImpCasts();
                if (const DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(baseBase)) {
                    if (const VarDecl *VD = dyn_cast<VarDecl>(DRE->getDecl()))
                        ownerVar = VD;
                } else if (const UnaryOperator *UO = dyn_cast<UnaryOperator>(baseBase)) {
                    if (UO->getOpcode() == UO_Deref) {
                        const Expr *op = UO->getSubExpr()->IgnoreParenImpCasts();
                        if (const DeclRefExpr *DRE2 = dyn_cast<DeclRefExpr>(op))
                            if (const VarDecl *VD = dyn_cast<VarDecl>(DRE2->getDecl()))
                                ownerVar = VD;
                    }
                }

                if (VERBOSE)
                    llvm::outs() << "[Debug] Detected struct member access: "
                                 << structMember->getNameAsString() << "\n";
            }
        }
    }

    // Original logic for direct union field access (u.x)
    if (!ownerVar) {
        if (const DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(base)) {
            if (const VarDecl *VD = dyn_cast<VarDecl>(DRE->getDecl()))
                ownerVar = VD;
        } else if (const UnaryOperator *UO = dyn_cast<UnaryOperator>(base)) {
            if (UO->getOpcode() == UO_Deref) {
                const Expr *op = UO->getSubExpr()->IgnoreParenImpCasts();
                if (const DeclRefExpr *DRE2 = dyn_cast<DeclRefExpr>(op))
                    if (const VarDecl *VD = dyn_cast<VarDecl>(DRE2->getDecl()))
                        ownerVar = VD;
            }
        }
    }

    if (!parentRD) {
        for (auto &P : Ctx.getParents(*FD)) {
            if (auto RD = P.get<RecordDecl>()) {
                parentRD = RD;
                break;
            }
        }
        if (!parentRD || !parentRD->isUnion())
            return true;
    }

    if (!ownerVar)
        return true;

    if (VERBOSE)
        llvm::outs() << "[Debug] Owner variable: " << ownerVar->getNameAsString() << "\n";

    if (VERBOSE)
        llvm::outs() << "[Debug] Parent is union: ";
    if (parentRD->getIdentifier())
        if (VERBOSE)
            llvm::outs() << parentRD->getName() << "\n";
        else if (VERBOSE)
            llvm::outs() << "(anonymous)\n";

    const FieldDecl *a_field = nullptr, *b_field = nullptr;
    int num_bytes;
    if (!isValidUnion(parentRD, Ctx, a_field, b_field, num_bytes, ownerVar)) {
        return true;
    }

    if (VERBOSE)
        llvm::outs() << "[Debug] Union passed 2 fields, same size check\n";

    bool is_write = false;
    const BinaryOperator *enclosingBO = nullptr;
    if (const BinaryOperator *BO = findEnclosingStmt<BinaryOperator>(ME, Ctx)) {
        enclosingBO = BO;
        if ((BO->isAssignmentOp() || BO->isCompoundAssignmentOp())) {
            Expr *lhs = BO->getLHS()->IgnoreParenCasts();
            if (lhs == ME) {
                is_write = true;
            } else if (auto *ASE = dyn_cast<ArraySubscriptExpr>(lhs)) {
                Expr *base = ASE->getBase()->IgnoreParenCasts();
                if (base == ME) {
                    is_write = true;
                } else if (auto *ME2 = dyn_cast<MemberExpr>(base)) {
                    if (ME2->getBase()->IgnoreParenCasts() == ME) {
                        is_write = true;
                    }
                }
            } else if (auto *ME2 = dyn_cast<MemberExpr>(lhs)) {
                if (ME2->getBase()->IgnoreParenCasts() == ME)
                    is_write = true;
            }
        }
    } else if (const UnaryOperator *UO = findEnclosingStmt<UnaryOperator>(ME, Ctx)) {
        if (UO->isIncrementDecrementOp())
            is_write = true;
    }

    if (VERBOSE)
        llvm::outs() << "[Debug] Access type: " << (is_write ? "write" : "read") << "\n";

    int64_t array_index = -1;
    const ArraySubscriptExpr *ASE = findEnclosingStmt<ArraySubscriptExpr>(ME, Ctx);
    if (ASE && ASE->getBase()->IgnoreParenImpCasts() == ME) {
        const Expr *indexExpr = ASE->getIdx();
        if (const IntegerLiteral *IL = dyn_cast<IntegerLiteral>(indexExpr)) {
            array_index = IL->getValue().getSExtValue();
            if (VERBOSE)
                llvm::outs() << "[Debug] Array index: " << array_index << "\n";
        } else {
            if (VERBOSE)
                llvm::outs() << "[Debug] Non-constant array index\n";
        }
    }

    FieldAccess ar = {FD, ME->getExprLoc(), is_write, ME, array_index, structMember};
    TypePunCandidate ui = {ownerVar, a_field, b_field, num_bytes};
    accesses[ui].push_back(ar);

    // Handle compound literal assignment to struct field
    if (is_write && enclosingBO && FD->getType()->isStructureType() && structMember == nullptr) {
        const Expr *rhs = enclosingBO->getRHS()->IgnoreParenImpCasts();
        if (const CompoundLiteralExpr *CLE = dyn_cast<CompoundLiteralExpr>(rhs)) {
            if (const InitListExpr *ILE = dyn_cast<InitListExpr>(CLE->getInitializer())) {
                const RecordDecl *structRD = FD->getType()->getAsRecordDecl();
                if (structRD) {
                    unsigned initIdx = 0;
                    for (const FieldDecl *member : structRD->fields()) {
                        if (initIdx < ILE->getNumInits()) {
                            FieldAccess memberAr = {FD, ME->getExprLoc(), true, ME, -1, member};
                            accesses[ui].push_back(memberAr);
                            if (VERBOSE)
                                llvm::outs() << "[Debug] Compound literal writes to struct member: "
                                             << member->getNameAsString() << "\n";
                            initIdx++;
                        }
                    }
                }
            }
        }
    }

    return true;
}

bool UnionAccessCollector::VisitVarDecl(VarDecl *VD) {
    if (!VD->hasInit())
        return true;

    QualType QT = VD->getType();
    const RecordDecl *RD = QT->getAsRecordDecl();
    if (!RD || !RD->isUnion())
        return true;

    const FieldDecl *a_field = nullptr, *b_field = nullptr;
    int num_bytes;
    if (!isValidUnion(RD, Ctx, a_field, b_field, num_bytes, VD))
        return true;

    const Expr *Init = VD->getInit();
    if (const InitListExpr *ILE = dyn_cast<InitListExpr>(Init)) {
        if (ILE->getNumInits() > 0) {
            const Expr *firstInit = ILE->getInit(0);
            TypePunCandidate ui = {VD, a_field, b_field, num_bytes};

            if (const clang::ArrayType *ArrType = Ctx.getAsArrayType(a_field->getType())) {
                if (const InitListExpr *arrayILE = dyn_cast<InitListExpr>(firstInit)) {
                    for (unsigned i = 0; i < arrayILE->getNumInits(); ++i) {
                        FieldAccess ar = {a_field, VD->getLocation(), true, nullptr,
                                        static_cast<int64_t>(i), nullptr};
                        accesses[ui].push_back(ar);
                    }
                    if (VERBOSE)
                        llvm::outs() << "[Debug] Found union initialization for variable: "
                                     << VD->getNameAsString() << " (write to array indices 0-"
                                     << (arrayILE->getNumInits() - 1) << ")\n";
                } else {
                    FieldAccess ar = {a_field, VD->getLocation(), true, nullptr, 0, nullptr};
                    accesses[ui].push_back(ar);
                    if (VERBOSE)
                        llvm::outs() << "[Debug] Found union initialization for variable: "
                                     << VD->getNameAsString() << " (write to array index 0)\n";
                }
            } else {
                FieldAccess ar = {a_field, VD->getLocation(), true, nullptr, -1, nullptr};
                accesses[ui].push_back(ar);
                if (VERBOSE)
                    llvm::outs() << "[Debug] Found union initialization for variable: "
                                 << VD->getNameAsString() << " (write to first field)\n";
            }
        }
    }

    return true;
}
