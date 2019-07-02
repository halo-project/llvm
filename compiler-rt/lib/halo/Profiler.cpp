
#include "halo/Profiler.h"
#include "halo/Error.h"

#include <iostream>
#include <cassert>

#include "llvm/Object/ObjectFile.h"
#include "llvm/Object/ELFObjectFile.h"

#include <elf.h>

#include "sanitizer_common/sanitizer_procmaps.h"

namespace san = __sanitizer;
namespace object = llvm::object;

namespace halo {

void Profiler::recordData1(IDType ID, DataKind DK, uint64_t Val) {
  switch (DK) {
    case DataKind::InstrPtr: {

      auto MaybeInfo = CRI.lookup(Val);
      if (!MaybeInfo) halo::fatal_error("unknown IP encountered");

      FunctionInfo *FI = MaybeInfo.getValue();
      std::cerr << FI->name << ", hits = " << FI->hits << "\n";
      FI->hits++;

    } break;

  }
}

// void Profiler::recordData2(IDType ID, uint64_t Val1, uint64_t Val2) {
//   // todo.
// }


bool CodeRegionInfo::loadObjFile(std::string ObjPath) {

  // create new function-offset map
  Data.emplace_back();
  auto &CodeMap = Data.back();
  auto Index = Data.size() - 1;
  ObjFiles[ObjPath] = Index;

  ///////////
  // initialize the code map

  auto ResOrErr = object::ObjectFile::createObjectFile(ObjPath);
  if (!ResOrErr) halo::fatal_error("error opening object file!");

  object::OwningBinary<object::ObjectFile> OB = std::move(ResOrErr.get());
  object::ObjectFile *Obj = OB.getBinary();

  // find the range of this object file in the process.
  uint64_t VMAStart, VMAEnd;
  san::GetCodeRangeForFile(ObjPath.data(), &VMAStart, &VMAEnd);

  uint64_t CodeDelta = VMAStart; // Assume PIE is enabled.
  if (auto *ELF = llvm::dyn_cast<object::ELFObjectFileBase>(Obj)) {
    // https://stackoverflow.com/questions/30426383/what-does-pie-do-exactly#30426603
    if (ELF->getEType() == ET_EXEC) {
      CodeDelta = 0; // This is a non-PIE executable.
      halo::fatal_error("Please recompile with PIE enabled."); // see FIXME in lookup.
    }
  }

  // std::cerr << std::hex
  //           << "VMAStart = 0x" << VMAStart << ", VMAEnd = 0x" << VMAEnd << "\n";

  auto VMARange =
      icl::right_open_interval<uint64_t>(VMAStart, VMAEnd);
  VMAResolver.insert(std::make_pair(VMARange, // -->
                                    std::make_pair(Index, CodeDelta)));

  // Gather function information and place it into the code map.
  for (const object::SymbolRef &Symb : Obj->symbols()) {
    auto MaybeType = Symb.getType();

    if (!MaybeType || MaybeType.get() != object::SymbolRef::Type::ST_Function)
      continue;

    auto MaybeName = Symb.getName();
    auto MaybeAddr = Symb.getAddress();
    uint64_t Size = Symb.getCommonSize();
    if (MaybeName && MaybeAddr && Size > 0) {
      // std::cerr << std::hex
      //           << MaybeName.get().data()
      //           << " at 0x" << MaybeAddr.get()
      //           << " of size 0x" << Size
      //           << "\n";

      uint64_t Start = MaybeAddr.get();
      uint64_t End = Start + Size;
      auto FuncRange = icl::right_open_interval<uint64_t>(Start, End);

      CodeMap.insert(std::make_pair(FuncRange, // -->
                                    new FunctionInfo(MaybeName.get())));
    }
  }

  return true;
}


llvm::Optional<FunctionInfo*> CodeRegionInfo::lookup(uint64_t IP) {
  // FIXME: why does a non-PIE IP fail a lookup in the VMAResolver?
  // the range looks correct to me!?
  std::cerr << std::hex << "lookup 0x" << IP << "\n";

  auto VMMap = VMAResolver.find(IP);
  if (VMMap == VMAResolver.end())
    return llvm::None;

  size_t Idx = VMMap->second.first;
  auto Delta = VMMap->second.second;

  auto &CodeMap = Data[Idx];
  IP -= Delta;

  auto FI = CodeMap.find(IP);
  if (FI == CodeMap.end())
    return llvm::None;

  return FI->second;
}

}
