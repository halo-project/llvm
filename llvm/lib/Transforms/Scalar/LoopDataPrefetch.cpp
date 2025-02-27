//===-------- LoopDataPrefetch.cpp - Loop Data Prefetching Pass -----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements a Loop Data Prefetching Pass.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Scalar/LoopDataPrefetch.h"
#include "llvm/InitializePasses.h"

#define DEBUG_TYPE "loop-data-prefetch"
#include "llvm/ADT/DepthFirstIterator.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/Analysis/CodeMetrics.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/OptimizationRemarkEmitter.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/ScalarEvolutionExpander.h"
#include "llvm/Transforms/Utils/ValueMapper.h"
using namespace llvm;

// By default, we limit this to creating 16 PHIs (which is a little over half
// of the allocatable register set).
cl::opt<cl::boolOrDefault>
PrefetchWrites("loop-prefetch-writes", cl::Hidden, cl::init(cl::boolOrDefault::BOU_UNSET),
               cl::desc("Prefetch write addresses"));

cl::opt<unsigned>
    PrefetchDistance("prefetch-distance",
                     cl::init(0),
                     cl::desc("Number of instructions to prefetch ahead"),
                     cl::Hidden);

static cl::opt<unsigned>
    MinPrefetchStride("min-prefetch-stride",
                      cl::desc("Min stride to add prefetches"), cl::Hidden);

static cl::opt<unsigned> MaxPrefetchIterationsAhead(
    "max-prefetch-iters-ahead",
    cl::desc("Max number of iterations to prefetch ahead"), cl::Hidden);

STATISTIC(NumPrefetches, "Number of prefetches inserted");

namespace {

/// Loop prefetch implementation class.
class LoopDataPrefetch {
public:
  LoopDataPrefetch(AssumptionCache *AC, DominatorTree *DT, LoopInfo *LI,
                   ScalarEvolution *SE, const TargetTransformInfo *TTI,
                   OptimizationRemarkEmitter *ORE)
      : AC(AC), DT(DT), LI(LI), SE(SE), TTI(TTI), ORE(ORE) {}

  bool run();

private:
  bool runOnLoop(Loop *L);

  /// Check if the stride of the accesses is large enough to
  /// warrant a prefetch.
  bool isStrideLargeEnough(const SCEVAddRecExpr *AR, unsigned TargetMinStride);

  unsigned getMinPrefetchStride(unsigned NumMemAccesses,
                                unsigned NumStridedMemAccesses,
                                unsigned NumPrefetches,
                                bool HasCall) {
    if (MinPrefetchStride.getNumOccurrences() > 0)
      return MinPrefetchStride;
    return TTI->getMinPrefetchStride(NumMemAccesses, NumStridedMemAccesses,
                                     NumPrefetches, HasCall);
  }

  unsigned getPrefetchDistance() {
    if (PrefetchDistance != 0)
      return PrefetchDistance;
    return TTI->getPrefetchDistance();
  }

  unsigned getMaxPrefetchIterationsAhead() {
    if (MaxPrefetchIterationsAhead.getNumOccurrences() > 0)
      return MaxPrefetchIterationsAhead;
    return TTI->getMaxPrefetchIterationsAhead();
  }

  bool doPrefetchWrites() {
    if (PrefetchWrites != cl::boolOrDefault::BOU_UNSET)
      return PrefetchWrites == cl::boolOrDefault::BOU_TRUE;
    return TTI->enableWritePrefetching();
  }

  AssumptionCache *AC;
  DominatorTree *DT;
  LoopInfo *LI;
  ScalarEvolution *SE;
  const TargetTransformInfo *TTI;
  OptimizationRemarkEmitter *ORE;
};

/// Legacy class for inserting loop data prefetches.
class LoopDataPrefetchLegacyPass : public FunctionPass {
public:
  static char ID; // Pass ID, replacement for typeid
  LoopDataPrefetchLegacyPass() : FunctionPass(ID) {
    initializeLoopDataPrefetchLegacyPassPass(*PassRegistry::getPassRegistry());
  }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<AssumptionCacheTracker>();
    AU.addRequired<DominatorTreeWrapperPass>();
    AU.addPreserved<DominatorTreeWrapperPass>();
    AU.addRequired<LoopInfoWrapperPass>();
    AU.addPreserved<LoopInfoWrapperPass>();
    AU.addRequired<OptimizationRemarkEmitterWrapperPass>();
    AU.addRequired<ScalarEvolutionWrapperPass>();
    AU.addPreserved<ScalarEvolutionWrapperPass>();
    AU.addRequired<TargetTransformInfoWrapperPass>();
  }

  bool runOnFunction(Function &F) override;
  };
}

char LoopDataPrefetchLegacyPass::ID = 0;
INITIALIZE_PASS_BEGIN(LoopDataPrefetchLegacyPass, "loop-data-prefetch",
                      "Loop Data Prefetch", false, false)
INITIALIZE_PASS_DEPENDENCY(AssumptionCacheTracker)
INITIALIZE_PASS_DEPENDENCY(TargetTransformInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(LoopInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(OptimizationRemarkEmitterWrapperPass)
INITIALIZE_PASS_DEPENDENCY(ScalarEvolutionWrapperPass)
INITIALIZE_PASS_END(LoopDataPrefetchLegacyPass, "loop-data-prefetch",
                    "Loop Data Prefetch", false, false)

FunctionPass *llvm::createLoopDataPrefetchPass() {
  return new LoopDataPrefetchLegacyPass();
}

bool LoopDataPrefetch::isStrideLargeEnough(const SCEVAddRecExpr *AR,
                                           unsigned TargetMinStride) {
  // No need to check if any stride goes.
  if (TargetMinStride <= 1)
    return true;

  const auto *ConstStride = dyn_cast<SCEVConstant>(AR->getStepRecurrence(*SE));
  // If MinStride is set, don't prefetch unless we can ensure that stride is
  // larger.
  if (!ConstStride)
    return false;

  unsigned AbsStride = std::abs(ConstStride->getAPInt().getSExtValue());
  return TargetMinStride <= AbsStride;
}

PreservedAnalyses LoopDataPrefetchPass::run(Function &F,
                                            FunctionAnalysisManager &AM) {
  DominatorTree *DT = &AM.getResult<DominatorTreeAnalysis>(F);
  LoopInfo *LI = &AM.getResult<LoopAnalysis>(F);
  ScalarEvolution *SE = &AM.getResult<ScalarEvolutionAnalysis>(F);
  AssumptionCache *AC = &AM.getResult<AssumptionAnalysis>(F);
  OptimizationRemarkEmitter *ORE =
      &AM.getResult<OptimizationRemarkEmitterAnalysis>(F);
  const TargetTransformInfo *TTI = &AM.getResult<TargetIRAnalysis>(F);

  LoopDataPrefetch LDP(AC, DT, LI, SE, TTI, ORE);
  bool Changed = LDP.run();

  if (Changed) {
    PreservedAnalyses PA;
    PA.preserve<DominatorTreeAnalysis>();
    PA.preserve<LoopAnalysis>();
    return PA;
  }

  return PreservedAnalyses::all();
}

bool LoopDataPrefetchLegacyPass::runOnFunction(Function &F) {
  if (skipFunction(F))
    return false;

  DominatorTree *DT = &getAnalysis<DominatorTreeWrapperPass>().getDomTree();
  LoopInfo *LI = &getAnalysis<LoopInfoWrapperPass>().getLoopInfo();
  ScalarEvolution *SE = &getAnalysis<ScalarEvolutionWrapperPass>().getSE();
  AssumptionCache *AC =
      &getAnalysis<AssumptionCacheTracker>().getAssumptionCache(F);
  OptimizationRemarkEmitter *ORE =
      &getAnalysis<OptimizationRemarkEmitterWrapperPass>().getORE();
  const TargetTransformInfo *TTI =
      &getAnalysis<TargetTransformInfoWrapperPass>().getTTI(F);

  LoopDataPrefetch LDP(AC, DT, LI, SE, TTI, ORE);
  return LDP.run();
}

bool LoopDataPrefetch::run() {
  // If PrefetchDistance is not set, don't run the pass.  This gives an
  // opportunity for targets to run this pass for selected subtargets only
  // (whose TTI sets PrefetchDistance).
  if (getPrefetchDistance() == 0)
    return false;
  assert(TTI->getCacheLineSize() && "Cache line size is not set for target");

  bool MadeChange = false;

  for (Loop *I : *LI)
    for (auto L = df_begin(I), LE = df_end(I); L != LE; ++L)
      MadeChange |= runOnLoop(*L);

  return MadeChange;
}

/// A record for a potential prefetch made during the initial scan of the
/// loop. This is used to let a single prefetch target multiple memory accesses.
struct Prefetch {
  /// The address formula for this prefetch as returned by ScalarEvolution.
  const SCEVAddRecExpr *LSCEVAddRec;
  /// The point of insertion for the prefetch instruction.
  Instruction *InsertPt;
  /// True if targeting a write memory access.
  bool Writes;
  /// The (first seen) prefetched instruction.
  Instruction *MemI;

  /// Constructor to create a new Prefetch for \p I.
  Prefetch(const SCEVAddRecExpr *L, Instruction *I)
      : LSCEVAddRec(L), InsertPt(nullptr), Writes(false), MemI(nullptr) {
    addInstruction(I);
  };

  /// Add the instruction \param I to this prefetch. If it's not the first
  /// one, 'InsertPt' and 'Writes' will be updated as required.
  /// \param PtrDiff the known constant address difference to the first added
  /// instruction.
  void addInstruction(Instruction *I, DominatorTree *DT = nullptr,
                      int64_t PtrDiff = 0) {
    if (!InsertPt) {
      MemI = I;
      InsertPt = I;
      Writes = isa<StoreInst>(I);
    } else {
      BasicBlock *PrefBB = InsertPt->getParent();
      BasicBlock *InsBB = I->getParent();
      if (PrefBB != InsBB) {
        BasicBlock *DomBB = DT->findNearestCommonDominator(PrefBB, InsBB);
        if (DomBB != PrefBB)
          InsertPt = DomBB->getTerminator();
      }

      if (isa<StoreInst>(I) && PtrDiff == 0)
        Writes = true;
    }
  }
};

bool LoopDataPrefetch::runOnLoop(Loop *L) {
  bool MadeChange = false;

  // Only prefetch in the inner-most loop
  if (!L->empty())
    return MadeChange;

  SmallPtrSet<const Value *, 32> EphValues;
  CodeMetrics::collectEphemeralValues(L, AC, EphValues);

  // Calculate the number of iterations ahead to prefetch
  CodeMetrics Metrics;
  bool HasCall = false;
  for (const auto BB : L->blocks()) {
    // If the loop already has prefetches, then assume that the user knows
    // what they are doing and don't add any more.
    for (auto &I : *BB) {
      if (isa<CallInst>(&I) || isa<InvokeInst>(&I)) {
        if (const Function *F = cast<CallBase>(I).getCalledFunction()) {
          if (F->getIntrinsicID() == Intrinsic::prefetch)
            return MadeChange;
          if (TTI->isLoweredToCall(F))
            HasCall = true;
        } else { // indirect call.
          HasCall = true;
        }
      }
    }
    Metrics.analyzeBasicBlock(BB, *TTI, EphValues);
  }
  unsigned LoopSize = Metrics.NumInsts;
  if (!LoopSize)
    LoopSize = 1;

  unsigned ItersAhead = getPrefetchDistance() / LoopSize;
  if (!ItersAhead)
    ItersAhead = 1;

  if (ItersAhead > getMaxPrefetchIterationsAhead())
    return MadeChange;

  unsigned ConstantMaxTripCount = SE->getSmallConstantMaxTripCount(L);
  if (ConstantMaxTripCount && ConstantMaxTripCount < ItersAhead + 1)
    return MadeChange;

  unsigned NumMemAccesses = 0;
  unsigned NumStridedMemAccesses = 0;
  SmallVector<Prefetch, 16> Prefetches;
  for (const auto BB : L->blocks())
    for (auto &I : *BB) {
      Value *PtrValue;
      Instruction *MemI;

      if (LoadInst *LMemI = dyn_cast<LoadInst>(&I)) {
        MemI = LMemI;
        PtrValue = LMemI->getPointerOperand();
      } else if (StoreInst *SMemI = dyn_cast<StoreInst>(&I)) {
        if (!doPrefetchWrites()) continue;
        MemI = SMemI;
        PtrValue = SMemI->getPointerOperand();
      } else continue;

      unsigned PtrAddrSpace = PtrValue->getType()->getPointerAddressSpace();
      if (PtrAddrSpace)
        continue;
      NumMemAccesses++;
      if (L->isLoopInvariant(PtrValue))
        continue;

      const SCEV *LSCEV = SE->getSCEV(PtrValue);
      const SCEVAddRecExpr *LSCEVAddRec = dyn_cast<SCEVAddRecExpr>(LSCEV);
      if (!LSCEVAddRec)
        continue;
      NumStridedMemAccesses++;

      // We don't want to double prefetch individual cache lines. If this
      // access is known to be within one cache line of some other one that
      // has already been prefetched, then don't prefetch this one as well.
      bool DupPref = false;
      for (auto &Pref : Prefetches) {
        const SCEV *PtrDiff = SE->getMinusSCEV(LSCEVAddRec, Pref.LSCEVAddRec);
        if (const SCEVConstant *ConstPtrDiff =
            dyn_cast<SCEVConstant>(PtrDiff)) {
          int64_t PD = std::abs(ConstPtrDiff->getValue()->getSExtValue());
          if (PD < (int64_t) TTI->getCacheLineSize()) {
            Pref.addInstruction(MemI, DT, PD);
            DupPref = true;
            break;
          }
        }
      }
      if (!DupPref)
        Prefetches.push_back(Prefetch(LSCEVAddRec, MemI));
    }

  unsigned TargetMinStride =
    getMinPrefetchStride(NumMemAccesses, NumStridedMemAccesses,
                         Prefetches.size(), HasCall);

  LLVM_DEBUG(dbgs() << "Prefetching " << ItersAhead
             << " iterations ahead (loop size: " << LoopSize << ") in "
             << L->getHeader()->getParent()->getName() << ": " << *L);
  LLVM_DEBUG(dbgs() << "Loop has: "
             << NumMemAccesses << " memory accesses, "
             << NumStridedMemAccesses << " strided memory accesses, "
             << Prefetches.size() << " potential prefetch(es), "
             << "a minimum stride of " << TargetMinStride << ", "
             << (HasCall ? "calls" : "no calls") << ".\n");

  for (auto &P : Prefetches) {
    // Check if the stride of the accesses is large enough to warrant a
    // prefetch.
    if (!isStrideLargeEnough(P.LSCEVAddRec, TargetMinStride))
      continue;

    const SCEV *NextLSCEV = SE->getAddExpr(P.LSCEVAddRec, SE->getMulExpr(
      SE->getConstant(P.LSCEVAddRec->getType(), ItersAhead),
      P.LSCEVAddRec->getStepRecurrence(*SE)));
    if (!isSafeToExpand(NextLSCEV, *SE))
      continue;

    BasicBlock *BB = P.InsertPt->getParent();
    Type *I8Ptr = Type::getInt8PtrTy(BB->getContext(), 0/*PtrAddrSpace*/);
    SCEVExpander SCEVE(*SE, BB->getModule()->getDataLayout(), "prefaddr");
    Value *PrefPtrValue = SCEVE.expandCodeFor(NextLSCEV, I8Ptr, P.InsertPt);

    IRBuilder<> Builder(P.InsertPt);
    Module *M = BB->getParent()->getParent();
    Type *I32 = Type::getInt32Ty(BB->getContext());
    Function *PrefetchFunc = Intrinsic::getDeclaration(
        M, Intrinsic::prefetch, PrefPtrValue->getType());
    Builder.CreateCall(
        PrefetchFunc,
        {PrefPtrValue,
         ConstantInt::get(I32, P.Writes),
         ConstantInt::get(I32, 3), ConstantInt::get(I32, 1)});
    ++NumPrefetches;
    LLVM_DEBUG(dbgs() << "  Access: "
               << *P.MemI->getOperand(isa<LoadInst>(P.MemI) ? 0 : 1)
               << ", SCEV: " << *P.LSCEVAddRec << "\n");
    ORE->emit([&]() {
        return OptimizationRemark(DEBUG_TYPE, "Prefetched", P.MemI)
          << "prefetched memory access";
      });

    MadeChange = true;
  }

  return MadeChange;
}
