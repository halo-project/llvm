#pragma once

#include <unordered_map>

#include "halomon/DynamicLinker.h"
#include "halomon/ThreadSafeContainers.h"
#include "halomon/XRayEvent.h"

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

llvm::Error start_instrumenting(uint64_t FnPtr);
llvm::Error stop_instrumenting(uint64_t FnPtr);
void addDyLib(std::unique_ptr<DyLib> Lib) { Dylibs.push_back(std::move(Lib)); }
// llvm::Error replaceAll(pb::CodeReplacement const&, std::unique_ptr<DyLib>, Channel &);

bool isPatchable(uint64_t FnPtr) const {
  return AddrToID.find(FnPtr) != AddrToID.end();
}

uint64_t getFnPtr(int32_t xrayID);

bool isInstrumenting() const {
  for (auto S : Status)
    if (S == Measuring)
      return true;
  return false;
}

// access the thread-safe queue of instrumentation events
ThreadSafeList<XRayEvent>& getEvents();

void garbageCollect();

private:
  llvm::Expected<int32_t> getXRayID(uint64_t FnPtr);
  llvm::Error redirectTo(uint64_t OldFnPtr, uint64_t NewFnPtr);

  size_t MaxValidID;
  std::unordered_map<uintptr_t, int32_t> AddrToID;
  std::list<std::unique_ptr<DyLib>> Dylibs;

  // These are indexed by XRay function ID.
  std::vector<uintptr_t> RedirectionTable;
  std::vector<enum PatchingStatus> Status;
};

} // end namespace
