#pragma once

#include <unordered_map>

#include "halomon/DynamicLinker.h"
#include "xray/xray_interface.h"

#include "Channel.h"
#include "Messages.pb.h"

namespace halo {

class CallCountProfiler;

enum PatchingStatus {
  Unpatched,
  Redirected
};

class CodePatcher {
public:
  CodePatcher();

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

void garbageCollect();

friend CallCountProfiler;

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
  std::vector<XRayRedirectionEntry> RedirectionTable;  // NOTE: this is referenced by ASM code
  std::vector<std::pair<enum PatchingStatus, uint64_t>> Metadata; // <patching status, function address>

  };

} // end namespace
