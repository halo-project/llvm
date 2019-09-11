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
// #include "llvm/Analysis/CallGraph.h"
#include "llvm/IR/GlobalValue.h"

#define DEBUG_TYPE "halo-prepare"

using namespace llvm;

struct HaloPrepare {
  PreservedAnalyses runOnModule(Module &M) {

    // We need to expose mutable globals defined in this module to the
    // dynamic linker by marking them as external in this module.
    //
    // NOTE: we may need to do this via creating freshly-named aliases
    // that are global, and perform a renaming during JIT compilation to
    // prevent a name clash.
    for (GlobalVariable &Global : M.globals()) {
      if (Global.isDeclaration())
        continue;

      if (Global.getName().startswith("llvm."))
        continue;

      LLVM_DEBUG(dbgs() << "before: \n\t" << Global << "\n");

      Global.setLinkage(GlobalValue::ExternalLinkage);
      Global.setDSOLocal(false);

      LLVM_DEBUG(dbgs() << "after: \n\t" << Global << "\n\n");
    }

    // TODO: embed information about the calling convention used by
    // each function in this module. We might be saved by the fact that
    // all XRay-instrumented functions might end up using the default C
    // calling convention?

    return PreservedAnalyses::none(); // conservative guess for now
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
    // auto &CG = getAnalysis<CallGraphWrapperPass>() ?? .getTLI();
    auto Preserved = Prepare.runOnModule(M);
    return !Preserved.areAllPreserved();
  }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    // AU.addRequired<CallGraphWrapperPass>();
  }

private:
  HaloPrepare Prepare;
};

char HaloPrepareLegacyPass::ID = 0;
INITIALIZE_PASS_BEGIN(
    HaloPrepareLegacyPass, "halo-prepare",
    "Prepare the module for use with Halo.", false, false)
// INITIALIZE_PASS_DEPENDENCY(CallGraphWrapperPass)
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
///
/// TODO: explain what it does
///
PreservedAnalyses HaloPreparePass::run(Module &M, ModuleAnalysisManager &MAM) {
  HaloPrepare Prepare;

  // auto &CG = MAM.getResult<CallGraphAnalysis>(M);
  return Prepare.runOnModule(M);
}
