#pragma once

#include <unordered_map>

#include "halomon/DynamicLinker.h"

#include "Channel.h"
#include "Messages.pb.h"


namespace halo {

class CodePatcher {
public:
  CodePatcher();

void measureRunningTime(uint64_t FnPtr);
llvm::Error replaceAll(pb::CodeReplacement const&, std::unique_ptr<DyLib>, Channel &);

private:
  size_t MaxID;
  std::unordered_map<uintptr_t, int32_t> AddrToID;
  std::list<std::unique_ptr<DyLib>> Dylibs;
};

} // end namespace
