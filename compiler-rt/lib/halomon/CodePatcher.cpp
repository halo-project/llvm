
#include "halomon/CodePatcher.h"

#include "xray/xray_interface_internal.h"

#include "Logging.h"

#include <unordered_map>
#include <cmath>
#include <cassert>
#include <thread>

namespace xray = __xray;

namespace halo {

CodePatcher::CodePatcher() {
  __xray_init();

  // populate the initial map of addrs to IDs and initialize the function table
  MaxValidID = __xray_max_function_id();
  RedirectionTable.resize(MaxValidID+1);
  Metadata.resize(MaxValidID+1);

  for (size_t i = 0; i <= MaxValidID; i++) {
    uint64_t FnAddr = __xray_function_address(i);
    AddrToID[FnAddr] = i;
    RedirectionTable[i].Redirection = 0;
    RedirectionTable[i].CallCount = 0;
    Metadata[i] = {Unpatched, FnAddr};
  }

  XRayRedirectionEntry *Table = RedirectionTable.data();
  logs() << "redirection table base = " << (uint64_t) Table << "\n";

  __xray_set_redirection_table(Table);
}

/// atomically exchanges the redirection
XRayRedirectType CodePatcher::swapRedirection(int32_t XRayID, XRayRedirectType newRedirection) {
  XRayRedirectType *Entry = &(RedirectionTable[XRayID].Redirection);
  return __atomic_exchange_n(Entry, newRedirection, __ATOMIC_SEQ_CST);
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

  if (Metadata[XRayID].first == Unpatched)
    return llvm::Error::success();

  __xray_unpatch_function(XRayID);
  Metadata[XRayID].first = Unpatched;

  auto PrevRedirect = swapRedirection(XRayID, 0);

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
    if (!MaybeSymbol)
      return MaybeSymbol.takeError();

    DySymbol& Symb = MaybeSymbol.get();
    if (!Symb.isVisible())
      return makeError("Lib " + newLibName + ", symbol " + newFnName + "is not JIT visible.");

    NewFnPtr = Symb.getAddress();
  }

  auto PrevRedirect = swapRedirection(XRayID, NewFnPtr);

  auto FnStatus = Metadata[XRayID].first;
  if (FnStatus == Unpatched) {
    // patch in the redirect
    __xray_redirect_function(XRayID);
    Metadata[XRayID].first = Redirected;

  } else if (FnStatus != Redirected) {
    return makeError("trying to redirect a function while it is not in \
                       valid states unpatched or redirected.");
  }

  return setSymbolRequired(PrevRedirect, false);
}


llvm::Expected<int32_t> CodePatcher::getXRayID(uint64_t FnPtr) {
  // clogs() << "looking up xray id for addr = " << FnPtr << "\n";

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
    assert(Req.addr() != 0 && "address zero function? seems suspicious.");

    auto Error = redirectTo(Req.addr(), Req.other_lib(), Req.other_name());
    if (Error) {
      clogs() << "Redirection failure for " << Req.name() << "\n";
      return Error;
    }

    clogs() << "redirected " << Req.name() << " @ " << Req.addr()
            << " --> " << Req.other_lib() << "::" << Req.other_name() << "\n";


  } else if (NewState == pb::BAKEOFF) {

      return makeError("todo: implement a bakeoff");

  } else {
    return makeError("unhandled function modification request!");
  }

  return llvm::Error::success();
}

} // end namespace
