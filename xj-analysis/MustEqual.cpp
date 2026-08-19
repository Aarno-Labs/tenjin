#include "MustEqual.h"

#include "Escape.h"
#include "Transfer.h"

#include "clang/AST/ASTContext.h"
#include "clang/Analysis/AnalysisDeclContext.h"
#include "clang/Analysis/CFG.h"
#include "clang/Analysis/CFGStmtMap.h"
#include "clang/Analysis/FlowSensitive/DataflowWorklist.h"

#include <utility>
#include <vector>

namespace xj::analysis
{

  // Everything the driver needs and no consumer does.
  struct MustEqualAnalysis::Impl
  {
    Impl(clang::ASTContext &Ctx, CellUniverse Cells, EscapeInfo Escapes)
        : Ctx(&Ctx), Cells(std::move(Cells)), Escapes(std::move(Escapes)) {}

    clang::ASTContext *Ctx;
    CellUniverse Cells;
    EscapeInfo Escapes;

    std::unique_ptr<clang::AnalysisDeclContext> ADC;

    // In-state per block, indexed by CFGBlock::getBlockID().
    std::vector<SED> BlockIn;
  };

  MustEqualAnalysis::MustEqualAnalysis() = default;
  MustEqualAnalysis::MustEqualAnalysis(MustEqualAnalysis &&) = default;
  MustEqualAnalysis &MustEqualAnalysis::operator=(MustEqualAnalysis &&) = default;
  MustEqualAnalysis::~MustEqualAnalysis() = default;

  llvm::Expected<MustEqualAnalysis>
  MustEqualAnalysis::run(clang::ASTContext &Ctx, const clang::FunctionDecl &FD,
                         Options Opts)
  {
    if (!FD.doesThisDeclarationHaveABody())
      return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                     "function has no body");
    if (Transfer::declines(FD))
      return llvm::createStringError(
          llvm::inconvertibleErrorCode(),
          "declined: setjmp, computed goto, or asm with a memory clobber");

    MustEqualAnalysis A;
    A.P = std::make_unique<Impl>(
        Ctx,
        CellUniverse::collect(Ctx, FD),
        EscapeInfo::compute(FD));
    Impl &I = *A.P;

    I.ADC = std::make_unique<clang::AnalysisDeclContext>(
        nullptr, const_cast<clang::FunctionDecl *>(&FD));
    I.ADC->getCFGBuildOptions().setAllAlwaysAdd();
    I.ADC->getCFGBuildOptions().PruneTriviallyFalseEdges = true;
    clang::CFG *G = I.ADC->getCFG();
    if (G == nullptr)
      return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                     "CFG::buildCFG failed");

    Transfer Xfer(Ctx, I.Cells, I.Escapes);

    // --- the fixpoint ---------------------------------------------------
    I.BlockIn.assign(G->getNumBlockIDs(), SED::bottom(I.Cells));
    I.BlockIn[G->getEntry().getBlockID()] = SED::atEntry(I.Cells);

    clang::ForwardDataflowWorklist W(*G, *I.ADC);
    W.enqueueBlock(&G->getEntry());
    unsigned Visits = 0;
    while (const clang::CFGBlock *B = W.dequeue())
    {
      if (++Visits > Opts.MaxBlockVisits)
        return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                       "block visit cap reached");
      SED Out = I.BlockIn[B->getBlockID()];
      if (Out.isBottom())
        continue;
      for (const clang::CFGElement &Elt : *B)
        Xfer.apply(Elt, Out);
      for (const clang::CFGBlock *S : B->succs())
        if (S != nullptr && I.BlockIn[S->getBlockID()].join(Out))
          W.enqueueBlock(S);
    }

    return std::move(A);
  }

  void MustEqualAnalysis::forEachStmt(
      llvm::function_ref<void(const clang::Stmt *, const SED &)> F) const
  {
    Transfer Xfer(*P->Ctx, P->Cells, P->Escapes);
    for (const clang::CFGBlock *B : cfg())
    {
      if (B == nullptr || P->BlockIn[B->getBlockID()].isBottom())
        continue;
      SED Cur = P->BlockIn[B->getBlockID()];
      for (const clang::CFGElement &Elt : *B)
      {
        if (auto CS = Elt.getAs<clang::CFGStmt>())
          F(CS->getStmt(), Cur);
        Xfer.apply(Elt, Cur);
      }
    }
  }

  bool MustEqualAnalysis::isReachable(const clang::Stmt *S) const
  {
    clang::CFGStmtMap *SM = P->ADC->getCFGStmtMap();
    const clang::CFGBlock *B = SM ? SM->getBlock(S) : nullptr;
    return B != nullptr && !P->BlockIn[B->getBlockID()].isBottom();
  }

  const CellUniverse &MustEqualAnalysis::cells() const { return P->Cells; }

  const EscapeInfo &MustEqualAnalysis::escapes() const { return P->Escapes; }

  const clang::CFG &MustEqualAnalysis::cfg() const { return *P->ADC->getCFG(); }

  const SED &MustEqualAnalysis::stateAtEntryOf(const clang::CFGBlock &B) const
  {
    return P->BlockIn[B.getBlockID()];
  }

} // namespace xj::analysis
