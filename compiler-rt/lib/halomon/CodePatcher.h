#pragma once

#include <unordered_map>

#include "halomon/DynamicLinker.h"



namespace halo {

class CodePatcher {
public:
  CodePatcher();

void measureRunningTime(uint64_t FnPtr);
void replace(pb::CodeReplacement const&, DynamicLinker const&);

private:
  size_t MaxID;
  std::unordered_map<uintptr_t, int32_t> AddrToID;
};

} // end namespace
