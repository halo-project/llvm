
#include "halomon/CodePatcher.h"
#include "halomon/XRayEvent.h"

#include "xray/xray_interface_internal.h"

#include "Logging.h"

#include <unordered_map>
#include <cmath>
#include <cassert>
#include <thread>

namespace xray = __xray;

namespace halo {

// NOTE: another handler to consider is 'runningtime'
// which would time only the N'th function call

namespace entrycount {

// NOTE: with LIMIT = 1024, we see a ~30% overhead when patching one of
// linear_hot's fibs

using Data = uint64_t;
using FuncID = int32_t;
constexpr unsigned LIMIT = 1024;
static ThreadSafeList<XRayEvent> GlobalData;
thread_local std::unordered_map<FuncID, Data> LocalData; // NOTE: maybe a vector is better?

void handler(int32_t FuncID, XRayEntryType Kind) {
  if (Kind != XRayEntryType::ENTRY)
    return;

  auto &Data = LocalData[FuncID];

  if (Data >= LIMIT) {
    GlobalData.emplace_back(getTimeStamp(), std::this_thread::get_id(), FuncID, Data);
    Data = 0;
  } else {
    Data += 1;
  }
}

} // end namespace entrycount


CodePatcher::CodePatcher() {
  __xray_init();

  // populate the initial map of addrs to IDs and initialize the function table
  MaxValidID = __xray_max_function_id();
  RedirectionTable.resize(MaxValidID+1);
  Status.resize(MaxValidID+1);

  for (size_t i = 0; i <= MaxValidID; i++) {
    AddrToID[__xray_function_address(i)] = i;
    RedirectionTable[i] = 0;
    Status[i] = Unpatched;
  }

  __xray_set_redirection_table(RedirectionTable.data());

  // the only handler we use
  __xray_set_handler(entrycount::handler);

}

// boost::lockfree::queue<XRayEvent>& CodePatcher::getEvents() {
//   return entrycount::GlobalData;
// }

ThreadSafeList<XRayEvent>& CodePatcher::getEvents() {
  return entrycount::GlobalData;
}

uint64_t CodePatcher::getFnPtr(int32_t xrayID) {
  return __xray_function_address(xrayID);
}

// NOTE: it will not be safe to garbage collect until you can ensure that
// no threads are executing in the function!
// One possible way to handle this is to ptrace ourselves to pause all of
// the threads, and inspect their state: https://en.wikipedia.org/wiki/Ptrace
//
// void CodePatcher::garbageCollect() {}


// START instrumenting
llvm::Error CodePatcher::start_instrumenting(uint64_t FnPtr) {
  auto Maybe = getXRayID(FnPtr);
  if (!Maybe)
    return Maybe.takeError();

  auto id = Maybe.get();
  if (Status[id] != Measuring) {
    __xray_patch_function(id);
    Status[id] = Measuring;
  }

  return llvm::Error::success();
}


// STOP instrumenting
llvm::Error CodePatcher::stop_instrumenting(uint64_t FnPtr) {
  auto Maybe = getXRayID(FnPtr);
  if (!Maybe)
    return Maybe.takeError();

  auto id = Maybe.get();
  if (Status[id] == Measuring) {
    // NOTE: we go back to unpatched instead of some previous status!
    __xray_unpatch_function(id);
    Status[id] = Unpatched;
  }

  return llvm::Error::success();
}


llvm::Expected<std::unique_ptr<DyLib>&> CodePatcher::findDylib(uint64_t FnPtr) {
  for (auto &Entry : Dylibs)
    if (Entry.second->haveSymbol(FnPtr))
      return Entry.second;

  return makeError("no DyLib contains the given function pointer.");
}

llvm::Expected<std::unique_ptr<DyLib>&> CodePatcher::findDylib(std::string const& LibName) {
  auto Result = Dylibs.find(LibName);
  if (Result == Dylibs.end())
    return makeError("no DyLib with the given name: " + LibName);

  return Result->second;
}

llvm::Error CodePatcher::setSymbolRequired(uint64_t FnPtr, bool Require) {
  if (FnPtr == 0)
    return llvm::Error::success();

  auto MaybeLib = findDylib(FnPtr);
  if (!MaybeLib)
    return MaybeLib.takeError();

  if (!Require) {
    return MaybeLib.get()->dropSymbol(FnPtr)
              ? llvm::Error::success()
              : makeError("symbol drop failed!");

  } else {
    // its required
    auto Required = MaybeLib.get()->requireSymbol(FnPtr);
    if (!Required)
      return Required.takeError();
  }

  return llvm::Error::success();
}


llvm::Error CodePatcher::unpatch(uint64_t FnPtr) {
  auto Maybe = getXRayID(FnPtr);
  if (!Maybe)
    return Maybe.takeError();

  auto XRayID = Maybe.get();

  if (Status[XRayID] == Unpatched)
    return llvm::Error::success();

  __xray_unpatch_function(XRayID);
  Status[XRayID] = Unpatched;

  // NOTE: I don't think this needs to be an atomic write, since the the unpatch above is atomic.
  auto PrevRedirect = RedirectionTable[XRayID];
  RedirectionTable[XRayID] = 0;

  return setSymbolRequired(PrevRedirect, false);
}

llvm::Error CodePatcher::redirectTo(uint64_t OldFnPtr,
                            std::string const& newLibName, std::string const& newFnName) {
  auto MaybeX = getXRayID(OldFnPtr);
  if (!MaybeX)
    return MaybeX.takeError();
  auto XRayID = MaybeX.get();

  uint64_t NewFnPtr = 0;
  if (!isOriginalLib(newLibName)) {
    // find lib
    auto MaybeLib = findDylib(newLibName);
    if (!MaybeLib)
      return MaybeLib.takeError();

    // require the symbol from the dylib
    auto MaybeSymbol = MaybeLib.get()->requireSymbol(newFnName);
    if (MaybeSymbol)
      return MaybeSymbol.takeError();

    NewFnPtr = MaybeSymbol.get().getAddress();
  }

  // TODO: might need to become an atomic write!
  auto PrevRedirect = RedirectionTable[XRayID];
  RedirectionTable[XRayID] = NewFnPtr;

  auto FnStatus = Status[XRayID];
  if (FnStatus == Unpatched) {
    // patch in the redirect
    __xray_redirect_function(XRayID);
    Status[XRayID] = Redirected;

  } else if (FnStatus != Redirected) {
    return makeError("trying to redirect a function while it is not in \
                       valid states unpatched or redirected.");
  }

  return setSymbolRequired(PrevRedirect, false);
}


llvm::Expected<int32_t> CodePatcher::getXRayID(uint64_t FnPtr) {
  auto Result = AddrToID.find(FnPtr);
  if (Result == AddrToID.end())
    return makeError("function ptr has no known xray id");

  return Result->second;
}


llvm::Error CodePatcher::modifyFunction(pb::ModifyFunction const& Req) {
  pb::FunctionState NewState = Req.desired_state();

  if (NewState == pb::UNPATCHED) {

    auto Error = unpatch(Req.addr());
    if (Error) {
      clogs() << "Unpatching failure for " << Req.name() << "\n";
      return Error;
    }


  } else if (NewState == pb::REDIRECTED) {
    auto Error = redirectTo(Req.addr(), Req.other_lib(), Req.other_name());
    if (Error) {
      clogs() << "Redirection failure for " << Req.name() << "\n";
      return Error;
    }


  } else if (NewState == pb::BAKEOFF) {

      return makeError("todo: implement a bakeoff");

  } else {
    return makeError("unhandled function modification request!");
  }

  return llvm::Error::success();
}

} // end namespace
