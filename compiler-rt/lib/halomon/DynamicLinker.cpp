#pragma once

#include "halomon/DynamicLinker.h"

#include "Messages.pb.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/ExecutionEngine/Orc/CompileUtils.h"
#include "llvm/ExecutionEngine/Orc/ExecutionUtils.h"
#include "llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h"
#include "llvm/ExecutionEngine/SectionMemoryManager.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Object/SymbolSize.h"

#include "Logging.h"

#include <memory>

namespace orc = llvm::orc;
namespace object = llvm::object;

namespace halo {

DyLib::DyLib (llvm::DataLayout DataLayout, std::unique_ptr<std::string> ObjFile)
  : DL(DataLayout),
    Mangle(ES, DL),
    ObjectLayer(ES, []() { return std::make_unique<llvm::SectionMemoryManager>(); }),
    RawObjFile(std::move(ObjFile)),
    MainJD(ES.createBareJITDylib("<main>")) {

  ObjectLayer.registerJITEventListener(LinkEvtListener);

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
    Info.retain();
    return Info;
  }

  auto MaybeEvalSymb = ES.lookup({&MainJD}, MangledName);
  if (!MaybeEvalSymb)
    return MaybeEvalSymb.takeError();

  auto EvalSymb = MaybeEvalSymb.get();
  if (!EvalSymb)
    return makeError("evaluated symbol has value zero!");

  auto MaybeSize = LinkEvtListener.getSize(MangledName);
  if (!MaybeSize)
    return makeError("evaluated symbol has no size?");

  auto &NewEntry = RequiredSymbols[MangledName];
  NewEntry.setSymbol(EvalSymb);
  NewEntry.setSize(MaybeSize.getValue());
  NewEntry.retain();

  return NewEntry;
}

size_t DyLib::numRequiredSymbols() const {
  size_t UsedSymbols = 0;
  for (auto const& Entry : RequiredSymbols)
    if (Entry.second.useCount() > 0)
      UsedSymbols++;

  return UsedSymbols;
}

// returns true if this symbol was contained in the dylib and a use was dropped.
bool DyLib::dropSymbol(llvm::StringRef Value) {
  if (haveSymbol(Value)) {
    auto &Symb = RequiredSymbols[Value];
    Symb.release();
    return true;
  }
  return false;
}

// returns true if this symbol was contained in the dylib and a use was dropped.
bool DyLib::dropSymbol(uint64_t Addr) {
  auto Maybe = findByAddr(Addr);
  if (Maybe) {
    Maybe.getValue()->release();
    return true;
  }
  return false;
}

void DyLib::dump(llvm::raw_ostream &OS) {
  ES.dump(OS);
  LinkEvtListener.dump(OS);
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

llvm::Optional<DySymbol*> DyLib::findByAddr(uint64_t Addr) {
  for (llvm::StringMapEntry<DySymbol> &Entry : RequiredSymbols) {
    auto &Sym = Entry.getValue();
    if (Sym.getAddress() == Addr)
      return &Sym;
  }
  return llvm::None;
}

llvm::Optional<DySymbol const*> DyLib::findByAddr(uint64_t Addr) const {
  for (llvm::StringMapEntry<DySymbol> const& Entry : RequiredSymbols) {
    auto const& Sym = Entry.getValue();
    if (Sym.getAddress() == Addr)
      return &Sym;
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


void DyLib::LinkingEventListener::notifyObjectLoaded(ObjectKey K, const object::ObjectFile &Obj,
                          const llvm::RuntimeDyld::LoadedObjectInfo &L) {

  // based on PerfJITEventListener::notifyObjectLoaded
  // NOTE: there are a lot of goodies in PerfJITEventListener. It demonstrates how
  // to get info about the source line number, for example.

  object::OwningBinary<object::ObjectFile> DebugObjOwner = L.getObjectForDebug(Obj);
  const object::ObjectFile &DebugObj = *DebugObjOwner.getBinary();

  for (const std::pair<object::SymbolRef, uint64_t> &P : object::computeSymbolSizes(DebugObj)) {
    object::SymbolRef Sym = P.first;

    llvm::Expected<object::SymbolRef::Type> SymTypeOrErr = Sym.getType();
    if (!SymTypeOrErr) {
      // There's not much we can with errors here
      consumeError(SymTypeOrErr.takeError());
      continue;
    }

    // we only care about functions
    object::SymbolRef::Type SymType = *SymTypeOrErr;
    if (SymType != object::SymbolRef::ST_Function)
      continue;

    llvm::Expected<llvm::StringRef> Name = Sym.getName();
    if (!Name) {
      consumeError(Name.takeError());
      continue;
    }

    // llvm::Expected<uint64_t> AddrOrErr = Sym.getAddress();
    // if (!AddrOrErr) {
    //   consumeError(AddrOrErr.takeError());
    //   continue;
    // }

    llvm::StringRef Label = Name.get();
    uint64_t Size = P.second;

    assert(SymbolInfo.find(Label) == SymbolInfo.end() && "redefined symbol? is the size / addr same?");
    SymbolInfo[Label] = Size;
  }
}

llvm::Optional<uint64_t> DyLib::LinkingEventListener::getSize(llvm::StringRef MangledName) const {
  auto Result = SymbolInfo.find(MangledName);
  if (Result != SymbolInfo.end())
    return Result->second;
  return llvm::None;
}

void DyLib::LinkingEventListener::dump(llvm::raw_ostream &OS) {
  OS << "LinkingEventListener {\n";
  for (auto const& Entry : SymbolInfo) {
    OS << Entry.first()
       << ", size = " << Entry.second
       << "\n";
  }
  OS << "}";
}

} // end namespace halo
