
#include "halo/Error.h"
#include "halo/Profiler.h"

#include <iostream>
#include <cassert>

#include "llvm/Object/ELFObjectFile.h"
#include "llvm/Object/ObjectFile.h"

#include <elf.h>

#include "sanitizer_common/sanitizer_procmaps.h"

namespace san = __sanitizer;
namespace object = llvm::object;

namespace halo {

std::string getFunc(CodeRegionInfo &CRI, uint64_t Addr) {
  auto MaybeInfo = CRI.lookup(Addr);
  if (MaybeInfo)
    return MaybeInfo.getValue()->Name;
  return "???";
}

void Profiler::processSamples() {
  for (RawSample &Sample : RawSamples) {
    std::cout << "tid " << Sample.TID
              << ", time " << Sample.Time
              << ", " << getFunc(CRI, Sample.IP)
              << "\n";

    std::cout << "CallChain sample len: " << Sample.CallStack.size() << "\n";
    for (auto RetAddr : Sample.CallStack) {
      std::cout << "\t\t " << getFunc(CRI, RetAddr) << " @ 0x"
                << std::hex << RetAddr << std::dec << "\n";
    }

    std::cout << "LBR sample len: " << Sample.LastBranch.size() << "\n";
    uint64_t Missed = 0, Predicted = 0, Total = 0;
    for (auto &BR : Sample.LastBranch) {
      Total++;
      if (BR.Mispred) Missed++;
      if (BR.Predicted) Predicted++;

      std::cout << std::hex << "\t\t"
        << getFunc(CRI, BR.From) << " @ 0x" << BR.From << " --> "
        << getFunc(CRI, BR.To)   << " @ 0x" << BR.To
        << ", mispred = " << BR.Mispred
        << ", pred = " << BR.Predicted
        << std::dec << "\n";
    }

    std::cout << "miss rate: " << Missed / ((double) Total)
              << ", predict rate: " << Predicted / ((double) Total)
              << "\n";

  }

  RawSamples.clear();
}

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
  san::uptr VMAStart, VMAEnd;
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

      auto FI = std::shared_ptr<FunctionInfo>(
        new FunctionInfo(MaybeName.get(), Start, Size)
      );

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
