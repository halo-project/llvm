#pragma once

#include <unordered_map>

#include "halomon/DynamicLinker.h"

#include "Channel.h"
#include "Messages.pb.h"


namespace halo {

enum PatchingStatus {
  Unpatched,
  Redirected,
  Measuring,
};

class CodePatcher {
public:
  CodePatcher();

llvm::Error measureRunningTime(uint64_t FnPtr);
llvm::Error replaceAll(pb::CodeReplacement const&, std::unique_ptr<DyLib>, Channel &);
void garbageCollect();

private:
  llvm::Expected<int32_t> getXRayID(uint64_t FnPtr);
  llvm::Error redirectTo(uint64_t OldFnPtr, uint64_t NewFnPtr);

  size_t MaxID;
  std::unordered_map<uintptr_t, int32_t> AddrToID;
  std::list<std::unique_ptr<DyLib>> Dylibs;

  // These are indexed by XRay function ID.
  std::vector<uintptr_t> RedirectionTable;
  std::vector<enum PatchingStatus> Status;
};

} // end namespace
