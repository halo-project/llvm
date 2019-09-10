//===- Transforms/Instrumentation/Halo.h --------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
/// \file
/// This file defines passes that transform the module to be compatible with
/// Halo's dynamic optimization system.
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_HALO_H
#define LLVM_TRANSFORMS_HALO_H

#include "llvm/IR/PassManager.h"
#include "llvm/Pass.h"

namespace llvm {
  /// Legacy PM compatible version of the Halo prepare pass.
  ModulePass *createHaloPrepareLegacyPass();

  /// A module pass for preparing the module for interfacing with the halomon
  /// runtime.
  ///
  /// TODO: explain what it does
  ///
  struct HaloPreparePass : public PassInfoMixin<HaloPreparePass> {
    PreservedAnalyses run(Module &M, ModuleAnalysisManager &MAM);
  };
} // end namespace llvm

#endif // LLVM_TRANSFORMS_HALO_H
