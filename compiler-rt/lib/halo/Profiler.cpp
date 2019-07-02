
#include "halo/Profiler.h"

#include <iostream>
#include <cassert>

#include "sanitizer_common/sanitizer_procmaps.h"

namespace object = llvm::object;
namespace san = __sanitizer;

namespace halo {

void Profiler::recordData1(IDType ID, DataKind DK, uint64_t Val) {
  switch (DK) {
    case DataKind::InstrPtr: {

      CR.lookupInfo(Val);



    } break;

  }
}

// void Profiler::recordData2(IDType ID, uint64_t Val1, uint64_t Val2) {
//   // todo.
// }


CodeRegion::CodeRegion(std::string BinPath, std::string& BinTriple)
                                              : BinaryPath(BinPath) {
  sym::LLVMSymbolizer::Options Opts;
  Opts.UseSymbolTable = true;
  Opts.Demangle = true;
  Opts.RelativeAddresses = true;
  Opts.DefaultArch = llvm::Triple(BinTriple).getArchName().str();

  // there are no copy / assign / move constructors.
  Symbolizer = new sym::LLVMSymbolizer(Opts);

  san::GetCodeRangeForFile(BinaryPath.data(), &VMAStart, &VMAEnd);
}


void CodeRegion::lookupInfo(uint64_t IP) {
  assert(VMAStart <= IP && IP <= VMAEnd && "Invalid IP.");

  uint64_t PCRaw = IP; // for non-PIE
  uint64_t PCOffset = IP - VMAStart; // for PIE

  // when PIE is ON, then the PCOffset should be passed in.
  // Otherwise when it is OFF, then you pass the raw PC in.
  // We can cheat and detect this by seeing if the returned function
  // name is <invalid>.
  auto ResOrErr = Symbolizer->symbolizeCode(
      BinaryPath, {PCOffset, object::SectionedAddress::UndefSection});

  if (!ResOrErr) {
    std::cerr << "Error in symbolization\n";
    return;
  }

  auto DILineInfo = ResOrErr.get();

  std::cerr << "IP = " << std::hex << "0x" << IP
            << ", region_start " << VMAStart
            << ", region_end " << VMAEnd
            << ", offset 0x" << PCOffset
            << ", function " << DILineInfo.FunctionName
            << "\n";
}

}
