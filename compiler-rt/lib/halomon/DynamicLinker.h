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

  struct RefCountedSymbol {
    DySymbol Value;
    int32_t Uses;
  };

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
      cantFail(ObjectLayer.add(MainDylib, std::move(Buffer)));
    }

    // Obtains the JITEvaluated symbol for this mangled symbol name.
    // Each call to this method increases the reference count of this
    // symbol in the dylib. use dropSymbol to release uses of the symbol.
    llvm::Expected<DySymbol> requireSymbol(llvm::StringRef MangledName) {
      if (haveSymbol(MangledName)) {
        auto &Info = RequiredSymbols[MangledName];
        Info.Uses++;
        return Info.Value;
      }

      auto MaybeEvalSymb = ES.lookup({&ES.getMainJITDylib()}, MangledName);
      if (!MaybeEvalSymb)
        return MaybeEvalSymb.takeError();

      auto EvalSymb = MaybeEvalSymb.get();
      if (!EvalSymb)
        return makeError("evaluated symbol has value zero!");

      auto &NewEntry = RequiredSymbols[MangledName];
      NewEntry.Value.Symbol = EvalSymb;
      NewEntry.Uses = 1;

      return NewEntry.Value;
    }

    size_t numRequiredSymbols() const {
      size_t UsedSymbols = 0;
      for (auto const& Entry : RequiredSymbols)
        if (Entry.second.Uses > 0)
          UsedSymbols++;

      return UsedSymbols;
    }

    // returns true if this symbol was contained in the dylib and a use was dropped.
    bool dropSymbol(llvm::StringRef Value) {
      if (haveSymbol(Value)) {
        auto &RefCountSymb = RequiredSymbols[Value];
        RefCountSymb.Uses = std::max(RefCountSymb.Uses-1, 0);
        return true;
      }
      return false;
    }

    // returns true if this symbol was contained in the dylib and a use was dropped.
    bool dropSymbol(uint64_t Addr) {
      auto Maybe = findByAddr(Addr);
      if (Maybe) {
        auto RefCountSymb = Maybe.getValue();
        RefCountSymb->Uses = std::max(RefCountSymb->Uses-1, 0);
        return true;
      }
      return false;
    }

    void dump(llvm::raw_ostream &OS) {
      ES.dump(OS);
    }

private:
  bool haveSymbol(llvm::StringRef MangledName) const {
    return RequiredSymbols.find(MangledName) != RequiredSymbols.end();
  }

  bool haveSymbol(uint64_t Address) const {
    // not sure how to convert to bool in return
    if (findByAddr(Address))
      return true;
    return false;
  }

  llvm::Optional<RefCountedSymbol*> findByAddr(uint64_t Addr) {
    for (llvm::StringMapEntry<RefCountedSymbol> &Entry : RequiredSymbols) {
      auto &RefCntSym = Entry.getValue();
      if (RefCntSym.Value.Symbol.getAddress() == Addr)
        return &RefCntSym;
    }
    return llvm::None;
  }

  llvm::Optional<RefCountedSymbol const*> findByAddr(uint64_t Addr) const {
    for (llvm::StringMapEntry<RefCountedSymbol> const& Entry : RequiredSymbols) {
      auto const& RefCntSym = Entry.getValue();
      if (RefCntSym.Value.Symbol.getAddress() == Addr)
        return &RefCntSym;
    }
    return llvm::None;
  }

  llvm::DataLayout DL;
  orc::ExecutionSession ES;
  orc::MangleAndInterner Mangle; // this will be needed for linking C++ symbols later.
  orc::RTDyldObjectLinkingLayer ObjectLayer;
  std::unique_ptr<std::string> RawObjFile; // with unresolved symbols.
  // could delete RawObjFile once all symbols we need have been looked up,
  // since the memory for the linked code is kept inside the ExecutionSession,
  // i.e., the ES only has read-only access to the RawObjFile.
  llvm::StringMap<RefCountedSymbol> RequiredSymbols;
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
