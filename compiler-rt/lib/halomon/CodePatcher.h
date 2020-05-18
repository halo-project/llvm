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

/// the name used to refer to the 'library'
/// that consists of the code in the original executable
/// that was loaded on process launch.
bool isOriginalLib(std::string const& libName) {
  return libName == "" || libName == "<original>";
}

void addDyLib(std::unique_ptr<DyLib> Lib) {
  std::string Name = Lib->getName();
  if (isOriginalLib(Name))
    fatal_error("DyLib cannot have this name; it is reserved for non-dynamic code.");

  auto Result = Dylibs.find(Name);
  if (Result != Dylibs.end())
    fatal_error("DyLib name already in use: " + Name);

  Dylibs[Name] = std::move(Lib);
}

llvm::Error modifyFunction(pb::ModifyFunction const&);

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

  llvm::Error redirectTo(uint64_t OldFnPtr, std::string const& NewLib, std::string const& NewFn);
  llvm::Error unpatch(uint64_t FnPtr);

  llvm::Expected<std::unique_ptr<DyLib>&> findDylib(uint64_t FnPtr);
  llvm::Expected<std::unique_ptr<DyLib>&> findDylib(std::string const& LibName);

  llvm::Error setSymbolRequired(uint64_t FnPtr, bool Require);

  size_t MaxValidID;
  std::unordered_map<uintptr_t, int32_t> AddrToID;
  std::unordered_map<std::string, std::unique_ptr<DyLib>> Dylibs;

  // These are indexed by XRay function ID.
  std::vector<uintptr_t> RedirectionTable;
  std::vector<enum PatchingStatus> Status;
};

} // end namespace
