
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
    case DataKind::InstrPtr:
    case DataKind::InstrPtrExact: {

      std::string Exactness = DK == DataKind::InstrPtrExact ? "" : "~";

      auto MaybeInfo = CRI.lookup(Val);

      if (MaybeInfo) {
        FunctionInfo *FI = MaybeInfo.getValue();
        std::cerr << std::dec
          << "sample (" << ID << "): "
          << std::hex << Exactness << " 0x" << Val << " in "
          << FI->name << ", hits = "
          << std::dec << FI->hits << "\n";

        FI->hits++;
      } else {
        std::cerr << std::dec << "sample (" << ID << "): unknown IP "
                  << Exactness << " 0x" << std::hex << Val << "\n";
      }

    } break;

  }
}

void Profiler::recordDataN(IDType ID, DataKind DK, uint64_t Num, uint64_t* Vals) {
  switch (DK) {
    case DataKind::CallChain: {

        std::cerr << std::dec
          << "sample (" << ID << "): " << "length " << Num << " call stack:\n";

        for (uint64_t i = 0; i < Num; i++) {
          uint64_t addr = Vals[i];
          auto MaybeInfo = CRI.lookup(addr);

          std::string Name;
          if (MaybeInfo)
            Name = MaybeInfo.getValue()->name;
          else
            Name = "???";

          std::cerr << "\t\tframe "
                    << std::dec << i
                    << std::hex << ", 0x" << addr
                    << " --> " << Name << "\n";
        }

    }; break;
  }
}

// void Profiler::recordData2(IDType ID, uint64_t Val1, uint64_t Val2) {
//   // todo.
// }


bool CodeRegionInfo::loadObjFile(std::string ObjPath) {

  // create new function-offset map
  Data.emplace_back();
  auto &Info = Data.back();
  auto &CodeMap = Info.first;
  uint64_t &Delta = Info.second;
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

  Delta = VMAStart; // Assume PIE is enabled.
  if (auto *ELF = llvm::dyn_cast<object::ELFObjectFileBase>(Obj)) {
    // https://stackoverflow.com/questions/30426383/what-does-pie-do-exactly#30426603
    if (ELF->getEType() == ET_EXEC) {
      Delta = 0; // This is a non-PIE executable.
    }
  }

  auto VMARange =
      icl::right_open_interval<uint64_t>(VMAStart, VMAEnd);
  VMAResolver.insert(std::make_pair(VMARange, Index));

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

      auto FI = std::shared_ptr<FunctionInfo>(new FunctionInfo(MaybeName.get()));

      CodeMap.insert(std::make_pair(FuncRange, std::move(FI)));
    }
  }

  return true;
}


llvm::Optional<FunctionInfo*> CodeRegionInfo::lookup(uint64_t IP) {
  size_t Idx = 0;

  // Typically we only have one VMA range that we're tracking,
  // so we avoid the resolver lookup in that case.
  if (Data.size() != 1) {
    auto VMMap = VMAResolver.find(IP);
    if (VMMap == VMAResolver.end())
      return llvm::None;
    Idx = VMMap->second;
  }

  auto &Info = Data[Idx];
  auto &CodeMap = Info.first;
  uint64_t Delta = Info.second;
  IP -= Delta;

  auto FI = CodeMap.find(IP);
  if (FI == CodeMap.end())
    return llvm::None;

  return FI->second.get();
}

}
