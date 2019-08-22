#pragma once

#include "Messages.pb.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/ExecutionEngine/JITSymbol.h"
#include "llvm/ExecutionEngine/Orc/CompileUtils.h"
#include "llvm/ExecutionEngine/Orc/Core.h"
#include "llvm/ExecutionEngine/Orc/ExecutionUtils.h"
#include "llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h"
#include "llvm/ExecutionEngine/Orc/RTDyldObjectLinkingLayer.h"
#include "llvm/ExecutionEngine/SectionMemoryManager.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/LLVMContext.h"

#include "halomon/Logging.h"

#include <memory>



namespace orc = llvm::orc;

namespace halo {


struct DySymbol {
  llvm::JITEvaluatedSymbol Symbol;
  // TODO: provide symbol size information from the unresolved object file.
};

class DyLib {

public:
  DyLib (llvm::DataLayout DataLayout, std::unique_ptr<std::string> ObjFile)
    : DL(DataLayout),
      Mangle(ES, DL),
      ObjectLayer(ES, []() { return std::make_unique<llvm::SectionMemoryManager>(); }),
      RawObjFile(std::move(ObjFile))
    {
      auto &MainDylib = ES.getMainJITDylib();

      // TODO: look at Orc/ExecutionUtils.h for utilities to link in C++ stuff.
      // MainDylib.addGenerator(
      //     cantFail(orc::LocalCXXRuntimeOverrides)
      // );...

      // exposes symbols found via dlsym to this dylib.
      MainDylib.addGenerator(
          cantFail(orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
              DL.getGlobalPrefix())));



      auto Buffer = llvm::MemoryBuffer::getMemBuffer(*RawObjFile);
      ObjectLayer.add(MainDylib, std::move(Buffer));
    }

    llvm::Expected<DySymbol> requireSymbol(llvm::StringRef MangledName) {
      if (providesSymbol(MangledName))
        return RequiredSymbols.lookup(MangledName);

      auto MaybeEvalSymb = ES.lookup({&ES.getMainJITDylib()}, MangledName);
      if (!MaybeEvalSymb)
        return MaybeEvalSymb.takeError();

      auto &NewEntry = RequiredSymbols[MangledName];
      NewEntry.Symbol = MaybeEvalSymb.get();

      NumRequiredSymbols++;
      return NewEntry;
    }

    bool providesSymbol(llvm::StringRef MangledName) const {
      return RequiredSymbols.find(MangledName) != RequiredSymbols.end();
    }

    size_t numRequiredSymbols() const {
      return NumRequiredSymbols;
    }

    void dropSymbol(llvm::StringRef MangledName) {
      if (RequiredSymbols.erase(MangledName))
        NumRequiredSymbols--;
    }

    void dump(llvm::raw_ostream &OS) {
      ES.dump(OS);
    }

private:
  llvm::DataLayout DL;
  orc::ExecutionSession ES;
  orc::MangleAndInterner Mangle; // this will be needed for linking C++ symbols later.
  orc::RTDyldObjectLinkingLayer ObjectLayer;
  std::unique_ptr<std::string> RawObjFile; // with unresolved symbols.
  // could delete RawObjFile once all symbols we need have been looked up,
  // since the memory for the linked code is kept inside the ExecutionSession,
  // i.e., the ES only has read-only access to the RawObjFile.
  llvm::StringMap<DySymbol> RequiredSymbols;
  size_t NumRequiredSymbols = 0;
};

class DynamicLinker {
private:
  llvm::DataLayout Layout;
  bool Valid; // prevent linking when only default-constructed.

public:
  DynamicLinker(llvm::DataLayout Layout) : Layout(Layout), Valid(true) {}
  DynamicLinker() : Layout(""), Valid(false) {}

  void setLayout(llvm::DataLayout DL) {
    Layout = DL;
    Valid = true;
  }

  // NOTE: this is a quite expensive setter.
  void setLayout(std::string const& Bitcode) {
    // JITTargetMachine's getDefaultLayout(Triple) crashes, and there doesn't
    // seem to be a better way to do this than looking in the module.

    llvm::LLVMContext Cxt;
    auto Buffer = llvm::MemoryBuffer::getMemBuffer(llvm::StringRef(Bitcode));
    auto MaybeModule = llvm::getLazyBitcodeModule(Buffer->getMemBufferRef(), Cxt);

    if (!MaybeModule) {
      warning(MaybeModule.takeError());
      return;
    }

    auto Module = std::move(MaybeModule.get());
    llvm::DataLayout DL(Module.get());
    setLayout(DL);
  }

  llvm::Expected<std::unique_ptr<DyLib>> run(std::unique_ptr<std::string> ObjFile) const {
    if (!Valid)
      return makeError("Dynamic linker's DataLayout was not set properly!");

    return std::make_unique<DyLib>(Layout, std::move(ObjFile));
  }

};

}
