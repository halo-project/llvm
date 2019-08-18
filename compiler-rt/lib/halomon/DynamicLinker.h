#pragma once

#include "Messages.pb.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/ExecutionEngine/JITSymbol.h"
#include "llvm/ExecutionEngine/Orc/CompileUtils.h"
#include "llvm/ExecutionEngine/Orc/Core.h"
#include "llvm/ExecutionEngine/Orc/ExecutionUtils.h"
#include "llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h"
#include "llvm/ExecutionEngine/Orc/RTDyldObjectLinkingLayer.h"
#include "llvm/ExecutionEngine/SectionMemoryManager.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/LLVMContext.h"
#include <memory>

namespace orc = llvm::orc;

namespace halo {

class DynamicLinker {

  orc::ExecutionSession ES;
  orc::RTDyldObjectLinkingLayer ObjectLayer;
  orc::ThreadSafeContext Ctx;

public:
  DynamicLinker()
  : ObjectLayer(ES, []() { return llvm::make_unique<llvm::SectionMemoryManager>(); }),
    Ctx(llvm::make_unique<llvm::LLVMContext>()) {}

  void run(pb::CodeReplacement &CR) {
    llvm::DataLayout DL(CR.data_layout());

    // TODO: this might be a one-time-only action.
    ES.getMainJITDylib().setGenerator( // became addGenerator, since now it supports more than 1?
        cantFail(orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
            DL.getGlobalPrefix())));

    // TODO: add the object file from the CR to the dylib.

  }

  llvm::Expected<llvm::JITEvaluatedSymbol> lookup(llvm::StringRef MangledName) {
    return ES.lookup({&ES.getMainJITDylib()}, MangledName);
  }

};

}
