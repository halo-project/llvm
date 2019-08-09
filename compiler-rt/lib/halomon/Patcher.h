#pragma once

#include <unordered_map>

namespace halo {

class Patcher {
public:
  Patcher();

void measureRunningTime(uint64_t FnPtr);

private:
  size_t MaxID;
  std::unordered_map<uintptr_t, int32_t> AddrToID;
};

} // end namespace
