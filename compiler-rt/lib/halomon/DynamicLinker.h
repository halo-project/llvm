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
  char GlobalSymbolPrefix;

public:
  DynamicLinker()
  : ObjectLayer(ES, []() { return std::make_unique<llvm::SectionMemoryManager>(); }),
    Ctx(std::make_unique<llvm::LLVMContext>()) {

      // FIXME: obtain the target triple from the .llvmcmd section
      // and pass that into this ctor.

      // llvm::Triple Triple;
      // Triple.setOS(llvm::Triple::OSType::Linux);
      // Triple.setObjectFormat(llvm::Triple::ObjectFormatType::ELF);
      //
      // orc::JITTargetMachineBuilder JTMB(Triple);
      // auto Layout = cantFail(JTMB.getDefaultDataLayoutForTarget());
      // GlobalSymbolPrefix = Layout.getGlobalPrefix();

      GlobalSymbolPrefix = '\0'; // ELF ONLY RIGHT NOW. FIX THE ABOVE

      ES.getMainJITDylib().addGenerator(
          cantFail(orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
              GlobalSymbolPrefix)));
    }

  void add(std::unique_ptr<std::string> ObjFile) {
    auto Buffer = llvm::MemoryBuffer::getMemBuffer(llvm::StringRef(*ObjFile));
    ObjectLayer.add(ES.getMainJITDylib(), std::move(Buffer));
  }

  llvm::Expected<llvm::JITEvaluatedSymbol> lookup(llvm::StringRef MangledName) {
    return ES.lookup({&ES.getMainJITDylib()}, MangledName);
  }

};

}
