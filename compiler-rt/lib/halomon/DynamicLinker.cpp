#pragma once

#include "halomon/DynamicLinker.h"

#include "Messages.pb.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/ExecutionEngine/JITEventListener.h"
#include "llvm/ExecutionEngine/Orc/CompileUtils.h"
#include "llvm/ExecutionEngine/Orc/ExecutionUtils.h"
#include "llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h"
#include "llvm/ExecutionEngine/SectionMemoryManager.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/Object/ObjectFile.h"

#include "Logging.h"

#include <memory>

namespace orc = llvm::orc;

namespace halo {

DyLib::DyLib (llvm::DataLayout DataLayout, std::unique_ptr<std::string> ObjFile)
  : DL(DataLayout),
    Mangle(ES, DL),
    ObjectLayer(ES, []() { return std::make_unique<llvm::SectionMemoryManager>(); }),
    RawObjFile(std::move(ObjFile)),
    MainJD(ES.createBareJITDylib("<main>")) {

  // ObjectLayer.registerJITEventListener(...);

  // TODO: look at Orc/ExecutionUtils.h for utilities to link in C++ stuff.
  // MainJD.addGenerator(
  //     cantFail(orc::LocalCXXRuntimeOverrides)
  // );...

  // exposes symbols found via dlsym to this dylib.
  MainJD.addGenerator(
      cantFail(orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
          DL.getGlobalPrefix())));



  auto Buffer = llvm::MemoryBuffer::getMemBuffer(*RawObjFile);
  cantFail(ObjectLayer.add(MainJD, std::move(Buffer)));
}


llvm::Expected<DySymbol> DyLib::requireSymbol(llvm::StringRef MangledName) {
  if (haveSymbol(MangledName)) {
    auto &Info = RequiredSymbols[MangledName];
    Info.Uses++;
    return Info.Value;
  }

  auto MaybeEvalSymb = ES.lookup({&MainJD}, MangledName);
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

size_t DyLib::numRequiredSymbols() const {
  size_t UsedSymbols = 0;
  for (auto const& Entry : RequiredSymbols)
    if (Entry.second.Uses > 0)
      UsedSymbols++;

  return UsedSymbols;
}

// returns true if this symbol was contained in the dylib and a use was dropped.
bool DyLib::dropSymbol(llvm::StringRef Value) {
  if (haveSymbol(Value)) {
    auto &RefCountSymb = RequiredSymbols[Value];
    RefCountSymb.Uses = std::max(RefCountSymb.Uses-1, 0);
    return true;
  }
  return false;
}

// returns true if this symbol was contained in the dylib and a use was dropped.
bool DyLib::dropSymbol(uint64_t Addr) {
  auto Maybe = findByAddr(Addr);
  if (Maybe) {
    auto RefCountSymb = Maybe.getValue();
    RefCountSymb->Uses = std::max(RefCountSymb->Uses-1, 0);
    return true;
  }
  return false;
}

void DyLib::dump(llvm::raw_ostream &OS) {
  ES.dump(OS);
}


bool DyLib::haveSymbol(llvm::StringRef MangledName) const {
  return RequiredSymbols.find(MangledName) != RequiredSymbols.end();
}

bool DyLib::haveSymbol(uint64_t Address) const {
  // not sure how to convert to bool in return
  if (findByAddr(Address))
    return true;
  return false;
}

llvm::Optional<DyLib::RefCountedSymbol*> DyLib::findByAddr(uint64_t Addr) {
  for (llvm::StringMapEntry<RefCountedSymbol> &Entry : RequiredSymbols) {
    auto &RefCntSym = Entry.getValue();
    if (RefCntSym.Value.Symbol.getAddress() == Addr)
      return &RefCntSym;
  }
  return llvm::None;
}

llvm::Optional<DyLib::RefCountedSymbol const*> DyLib::findByAddr(uint64_t Addr) const {
  for (llvm::StringMapEntry<RefCountedSymbol> const& Entry : RequiredSymbols) {
    auto const& RefCntSym = Entry.getValue();
    if (RefCntSym.Value.Symbol.getAddress() == Addr)
      return &RefCntSym;
  }
  return llvm::None;
}


// NOTE: this is a quite expensive setter.
void DynamicLinker::setLayout(std::string const& Bitcode) {
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

llvm::Expected<std::unique_ptr<DyLib>> DynamicLinker::run(std::unique_ptr<std::string> ObjFile) const {
  if (!Valid)
    return makeError("Dynamic linker's DataLayout was not set properly!");

  return std::make_unique<DyLib>(Layout, std::move(ObjFile));
}

} // end namespace halo
