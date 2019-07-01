
#include "halo/Profiler.h"

#include <iostream>

#include "sanitizer_common/sanitizer_procmaps.h"

namespace object = llvm::object;
namespace san = __sanitizer;

namespace halo {

void Profiler::recordData1(IDType ID, DataKind DK, uint64_t Val) {
  switch (DK) {
    case DataKind::InstrPtr: {

      // NOTE: this interval should be cached.
      uint64_t start, end;
      san::GetCodeRangeForFile(BinaryPath.data(), &start, &end);

      uint64_t PCRaw = Val; // for non-PIE
      uint64_t PCOffset = Val - start; // for PIE

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

      std::cerr << "IP = " << std::hex << "0x" << Val
                << ", region_start " << start
                << ", region_end " << end
                << ", offset 0x" << PCOffset
                << ", function " << DILineInfo.FunctionName
                << "\n";

    } break;

  }
}

// void Profiler::recordData2(IDType ID, uint64_t Val1, uint64_t Val2) {
//   // todo.
// }

}
