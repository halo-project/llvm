
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
// void CodePatcher::garbageCollect() {
//   auto Cur = Dylibs.begin();
//   while (Cur != Dylibs.end()) {
//     if ((*Cur)->numRequiredSymbols() == 0)
//       Cur = Dylibs.erase(Cur);
//     else
//       Cur++;
//   }
// }


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


llvm::Error CodePatcher::redirectTo(uint64_t OldFnPtr, uint64_t NewFnPtr) {
  auto Maybe = getXRayID(OldFnPtr);
  if (!Maybe)
    return Maybe.takeError();

  auto XRayID = Maybe.get();

  // TODO: might need to become an atomic write!
  auto PrevRedirect = RedirectionTable[XRayID];
  RedirectionTable[XRayID] = NewFnPtr;

  if (Status[XRayID] != Redirected) {
    // patch in the redirect
    __xray_redirect_function(XRayID);
    Status[XRayID] = Redirected;
  } else {
    // we don't need the old redirect anymore.
    for (auto &Lib : Dylibs)
      if (Lib->dropSymbol(PrevRedirect))
        break;
  }

  return llvm::Error::success();
}


llvm::Expected<int32_t> CodePatcher::getXRayID(uint64_t FnPtr) {
  auto Result = AddrToID.find(FnPtr);
  if (Result == AddrToID.end())
    return makeError("function ptr has no known xray id");

  return Result->second;
}

llvm::Error CodePatcher::replaceAll(pb::CodeReplacement const& CR,
                                      std::unique_ptr<DyLib> Dylib, Channel &Chan) {
  // perform linking on all requested symbols and collect those
  // new addresses.

  llvm::SmallVector<std::pair<pb::FunctionSymbol const&, DySymbol>, 10> NewCode;

  for (pb::FunctionSymbol const& Request : CR.symbols()) {
    auto &Label = Request.label();
    auto MaybeSymbol = Dylib->requireSymbol(Label);

    if (!MaybeSymbol)
      return MaybeSymbol.takeError();

    auto DySym = MaybeSymbol.get();
    if (!DySym.getFlags().isCallable())
      return makeError("JITed function symbol is not callable.");

    NewCode.push_back({Request, DySym});
  }

  // Nothing to do!
  if (NewCode.empty())
    return llvm::Error::success();

  Dylib->dump(logs());

  // save the dylib since we definitely need it.
  Dylibs.push_back(std::move(Dylib));

  // TODO: send a message to server containing the new address & size information.

  // TODO: inform perf-events of these new code addresses so we recieve samples
  // from them. it might be the case that any pages mapped as executable
  // generates the corresponding perf_event or something? research is needed.

  // TODO: update XRay's function information table so that we can instrument
  // the new code, etc.
  // I believe the way we need to do this is to have the DyLib
  // resolve its XRay function entry table. Then, in this code patcher, we
  // translate requests to say, instrument a function, to the right XRaySledEntry
  // based on whether the process's version has been overridden / redirected.
  // If it has been redirected, we obtain the SledEntry from the Dylib instead
  // of XRay's table.


  // for now, let's try patching in the new functions.
  for (auto &Info : NewCode) {
    auto &OrigSymb = Info.first;
    auto &NewSymb = Info.second;

    auto Error = redirectTo(OrigSymb.addr(), NewSymb.getAddress());
    if (Error) {
      llvm::errs() << "Unable to redirect " << OrigSymb.label() << "\n";
      return Error;
    }

  }

  return llvm::Error::success();
}

} // end namespace
