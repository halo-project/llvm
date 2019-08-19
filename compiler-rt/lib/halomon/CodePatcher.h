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
llvm::Error replaceAll(pb::CodeReplacement const&, llvm::StringRef &ObjFile,
                        DynamicLinker &, Channel &);

private:
  size_t MaxID;
  std::unordered_map<uintptr_t, int32_t> AddrToID;
};

} // end namespace
