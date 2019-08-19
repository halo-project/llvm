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

  uint64_t LibTicker = 0;

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

      // ES.getMainJITDylib().addGenerator(
      //     cantFail(orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
      //         GlobalSymbolPrefix)));
    }

  // All lookups performed after adding the object file must happen before
  // the ObjFile passed in here goes out of scope!
  std::string add(llvm::StringRef ObjFile) {
    // NOTE: testing here. We need a mechanism to unload / deallocate these dylibs!

    // Also: what happens if one dylib's symbols override another, do the old
    // addresses in the later lib become invalid?
    std::string Name = "newCode";
    Name += std::to_string(LibTicker++);
    auto &NewDylib = ES.createJITDylib(Name); // UNCOMMENT TO TRY ADDING TO NEW DYLIB INSTEAD
    // auto &NewDylib = ES.getMainJITDylib();

    NewDylib.addGenerator(
        cantFail(orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
            GlobalSymbolPrefix)));

    auto Buffer = llvm::MemoryBuffer::getMemBuffer(ObjFile);
    ObjectLayer.add(NewDylib, std::move(Buffer));

    return Name;
  }

  llvm::Expected<llvm::JITEvaluatedSymbol> lookup(llvm::StringRef LibName, llvm::StringRef MangledName) {
    return ES.lookup(ES.getJITDylibByName(LibName), ES.intern(MangledName));
  }

  void dump(llvm::raw_ostream &OS) {
    ES.dump(OS);
  }

};

}
