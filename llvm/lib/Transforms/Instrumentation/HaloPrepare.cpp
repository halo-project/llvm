//===-- Halo.cpp ----------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements passes to be run on the module to prepare it for
// use with the Halo dynamic optimizer.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Instrumentation/Halo.h"
#include "llvm/InitializePasses.h"
#include "llvm/Analysis/CallGraph.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/GlobalValue.h"

#define DEBUG_TYPE "halo-prepare"

using namespace llvm;

struct HaloPrepare {

  // returns a pair of the analyses preserved by this function,
  // and a bool indicating if the function was made patchable.
  std::pair<PreservedAnalyses, bool> makePatchable(Function &Func, CallGraph &CG, LoopInfo &LI) {
    const size_t INSTR_COUNT_THRESH = 50; // minimum number of instrs to not be considered small
    std::pair<PreservedAnalyses, bool> Skip = {PreservedAnalyses::all(), false};

    // skip if it has some odd attributes
    if (Func.hasFnAttribute(Attribute::NoDuplicate) ||
        Func.hasFnAttribute(Attribute::Naked) ||
        Func.hasFnAttribute(Attribute::Builtin) ||
        Func.hasFnAttribute(Attribute::ReturnsTwice))
      return Skip;

    // skip non-rentrant functions like 'main'
    if (Func.hasFnAttribute(Attribute::NoRecurse))
      return Skip;

    // skip functions that are run only during startup
    //
    // NOTE: Ideally this would be transitive: if a function is only
    // reachable in the call graph from a startup function, don't patch it.
    if (Func.hasSection() && Func.getSection() == ".text.startup")
      return Skip;


    auto CGNode = CG[&Func];
    size_t NumCallees = CGNode->size();
    bool IsLeaf = NumCallees == 0;
    bool NoLoops = LI.empty(); // NOTE: this does NOT mean a cycle-free CFG!
    bool IsSmall = Func.getInstructionCount() < INSTR_COUNT_THRESH;

    LLVM_DEBUG(dbgs() << "\n" << Func.getName() << " calls " << NumCallees
           << " funs;\n\t has loops = " << !NoLoops << ".\n");

    // skip if it's a leaf with no loop and is small
    if (IsLeaf && NoLoops && IsSmall)
      return Skip;

    // Otherwise we mark it as patchable.
    Func.setLinkage(GlobalValue::ExternalLinkage);
    Func.addFnAttr("xray-instruction-threshold", "1"); // XRay force patching

    return {PreservedAnalyses::none(), true};
  }



  PreservedAnalyses recordPatchableFuncs(Module &M, SmallPtrSet<Function*, 32> &PatchedFuncs) {
    SmallString<512> NameList;

    for (Function *Func : PatchedFuncs) {
      NameList += Func->getName();
      NameList.append(1, '\0'); // use NULL as delimiter
    }

    // ensure it's at least got one NULL on the end
    if (NameList == "")
      NameList.append(1, '\0');

    auto &Cxt = M.getContext();
    Constant *Lit = ConstantDataArray::getString(Cxt, NameList, false);

    GlobalVariable *Glob = dyn_cast<GlobalVariable>(
                              M.getOrInsertGlobal("halo.patchableFuncs",
                                                  Lit->getType()));
    Glob->setInitializer(Lit);
    Glob->setSection(".halo.metadata");
    // mark it extern so its not dropped. I am lazy to add it to @llvm.compiler.used
    Glob->setLinkage(GlobalVariable::ExternalLinkage);

    // conservative guess. not sure if a new global invalidates any analyses
    return PreservedAnalyses::none();
  }


  PreservedAnalyses fixGlobals(Module &M) {
    bool MadeChange = false;

    // We need to expose mutable globals defined in this module to the
    // dynamic linker by marking them as external in this module.
    for (GlobalVariable &Global : M.globals()) {
      if (Global.isDeclaration())
        continue;

      if (Global.getName().startswith("llvm."))
        continue;

      LLVM_DEBUG(dbgs() << "before: \n\t" << Global << "\n");

      Global.setLinkage(GlobalValue::ExternalLinkage);

      LLVM_DEBUG(dbgs() << "after: \n\t" << Global << "\n\n");
      MadeChange = true;
    }

    return MadeChange ? PreservedAnalyses::none() : PreservedAnalyses::all();
  }
};

///////// Legacy PM compatibility /////////
class HaloPrepareLegacyPass : public ModulePass {
public:
  static char ID;
  HaloPrepareLegacyPass() : ModulePass(ID) {
    initializeHaloPrepareLegacyPassPass(*PassRegistry::getPassRegistry());
  }

  StringRef getPassName() const override { return "Halo Prepare Pass"; }

  // returns true if something changed to invalidate analyses.
  bool runOnModule(Module &M) override {
    auto &CG = getAnalysis<CallGraphWrapperPass>().getCallGraph();

    // STEP 1: fix up global linkages.
    bool PreservedTotal = Prepare.fixGlobals(M).areAllPreserved();

    // STEP 2: make (some) functions patchable by the runtime system.
    SmallPtrSet<Function*, 32> PatchedFuncs;
    for (Function &Func : M.functions()) {
      if (Func.isDeclaration())
        continue;

      auto &LI = getAnalysis<LoopInfoWrapperPass>(Func).getLoopInfo();
      auto Result = Prepare.makePatchable(Func, CG, LI);

      PreservedTotal = PreservedTotal && Result.first.areAllPreserved();

      LLVM_DEBUG(dbgs() << Func.getName() << " made patchable: " << Result.second << "\n");

      // was it made patchable?
      if (Result.second)
        PatchedFuncs.insert(&Func);
    }

    // STEP 3: record in the module itself information about
    // which functions were made patchable
    auto Result = Prepare.recordPatchableFuncs(M, PatchedFuncs);
    PreservedTotal = PreservedTotal && Result.areAllPreserved();


    return !PreservedTotal;
  }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<CallGraphWrapperPass>();
    AU.addRequired<LoopInfoWrapperPass>();
  }

private:
  HaloPrepare Prepare;
};

char HaloPrepareLegacyPass::ID = 0;
INITIALIZE_PASS_BEGIN(
    HaloPrepareLegacyPass, "halo-prepare",
    "Prepare the module for use with Halo.", false, false)
INITIALIZE_PASS_DEPENDENCY(CallGraphWrapperPass)
INITIALIZE_PASS_DEPENDENCY(LoopInfoWrapperPass)
INITIALIZE_PASS_END(
    HaloPrepareLegacyPass, "halo-prepare",
    "Prepare the module for use with Halo.", false, false)

/// Legacy PM compatible version of the Halo prepare pass.
ModulePass *llvm::createHaloPrepareLegacyPass() {
  return new HaloPrepareLegacyPass();
}

///////////////////////////

///////// New Pass Manager version

/// A module pass for preparing the module for interfacing with the halomon
/// runtime.
PreservedAnalyses HaloPreparePass::run(Module &M, ModuleAnalysisManager &MAM) {
  // HaloPrepare Prepare;

  // auto &CG = MAM.getResult<CallGraphAnalysis>(M);
  // auto &LI = MAM.getResult<LoopAnalysis>(M); // must be function specific!
  // return Prepare.runOnModule(M, CG, LI);
  report_fatal_error("TODO: implement HaloPrepare for new PM!");
}
