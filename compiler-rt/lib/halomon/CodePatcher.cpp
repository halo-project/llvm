
#include "halomon/CodePatcher.h"
#include "halomon/Logging.h"
#include "xray/xray_interface_internal.h"

#include <time.h>

#include <unordered_map>
#include <stack>
#include <cmath>
#include <cassert>

namespace xray = __xray;

namespace halo {

static constexpr bool Geometric = true;
static constexpr size_t MaxDepth = 128;
static constexpr uint64_t NanosecondsPerSecond = 1000ULL * 1000 * 1000;

struct TimeLog {
  std::stack<uint64_t> EntryStack; // stack of timestamps, in ns
  uint64_t Excess = 0;

  void record(uint64_t NewValue) {
    // https://en.wikipedia.org/wiki/Algorithms_for_calculating_variance#Welford's_online_algorithm
    if (Geometric) {
      assert(NewValue > 0);
      NewValue = std::log2(NewValue);
    }

    Count += 1;
    double Delta = NewValue - Mean;
    Mean += Delta / Count;
    double Delta2 = NewValue - Mean; // not a common-subexpression.
    SumSqDistance += Delta * Delta2;
  }

  uint32_t samples() const { return Count; }
  double mean() const { return Mean; }

  double population_variance() const { return Count == 0
                                    ? 0
                                    : SumSqDistance / Count; }

  // sample variance.
  double variance() const { return Count <= 1
                                          ? 0
                                          : SumSqDistance / (Count - 1); }

  double deviation() const {
    auto S = variance();
    if (S == 0)
      return 0;
    return std::sqrt(S);
  }

  // estimate.
  double error() const { return Count == 0
                                ? 0
                                : deviation() / std::sqrt(Count); }


  void dump(llvm::raw_ostream &out) const {
    auto Avg = mean();
    out << "mean = " << Avg
        << ", deviation = " << deviation()
        << ", error_pct = " << (error() / Avg) * 100.0
        << ", samples = " << samples()
        << "\n";
  }

private:
  double Mean = 0.0;
  double SumSqDistance = 0.0;
  uint64_t Count = 0;
};

// FIXME
// Need to come up with a mechanism whereby the TimeLog is stored
// elsewhere (not in TLS), so that other threads can sample / read
// the running values. Probably want to say:
// if (FunctionLogs[FuncID] == nullptr)
//    allocate timelog in global, thread-safe data structure where I have
//    exclusive write-access.
thread_local std::unordered_map<int32_t, TimeLog> FunctionLogs;

// returns the time in nanoseconds.
inline uint64_t getTimeStamp(clockid_t Kind = CLOCK_THREAD_CPUTIME_ID) {
    timespec TS;
    int result = clock_gettime(Kind, &TS);
    if (result != 0) {
      if (LOG) log() << "clock_gettime errno=" << errno << "\n";
      TS = {0, 0};
    }
    return TS.tv_sec * NanosecondsPerSecond + TS.tv_nsec;
}

void timingHandler(int32_t FuncID, XRayEntryType Kind) {
  auto &Log = FunctionLogs[FuncID];
  auto &EntryStack = Log.EntryStack;

  switch(Kind) {
    case ENTRY: {
      if (EntryStack.size() >= MaxDepth) {
        Log.Excess++;
        return;
      }

      EntryStack.push(getTimeStamp());
    } return;


    case EXIT: case TAIL: {

      if (Log.Excess > 0) {
        Log.Excess--;
        return;
      }

      if (EntryStack.empty())
        return;

      auto End = getTimeStamp();
      auto Start = EntryStack.top();
      EntryStack.pop();

      // NOTE: By taking the log, we're computing a geometric mean.
      uint64_t Elapsed = End - Start;
      Log.record(Elapsed);
      if (LOG) Log.dump(log()); // lol

      // FIXME: this should NOT be done by the application thread.
      // I think we need a thread that wakes up on a periodic timer
      // (or can be woken up by the application thread?) that
      // periodically unpatches functions?
      if (Log.samples() > 100)
        __xray_unpatch_function(FuncID);

    } return;

    default: return; // ignore
  };
}

CodePatcher::CodePatcher() {
  __xray_init();

  // populate the initial map of addrs to IDs and initialize the function table
  MaxID = __xray_max_function_id();
  RedirectionTable.resize(MaxID);
  Status.resize(MaxID);

  for (size_t i = 0; i < MaxID; i++) {
    AddrToID[__xray_function_address(i)] = i;
    RedirectionTable[i] = 0;
    Status[i] = Unpatched;
  }

  __xray_set_redirection_table(RedirectionTable.data());

  // the only handler we use
  __xray_set_handler(timingHandler);

}

// NOTE: it will not be safe to garbage collect until you can ensure that
// no threads are executing in the function!
// void CodePatcher::garbageCollect() {
//   auto Cur = Dylibs.begin();
//   while (Cur != Dylibs.end()) {
//     if ((*Cur)->numRequiredSymbols() == 0)
//       Cur = Dylibs.erase(Cur);
//     else
//       Cur++;
//   }
// }

void CodePatcher::measureRunningTime(uint64_t FnPtr) {
  auto id = llvm::cantFail(getXRayID(FnPtr));

  if (Status[id] != Measuring) {
    __xray_patch_function(id);
    Status[id] = Measuring;
  }
}

void CodePatcher::redirectTo(uint64_t OldFnPtr, uint64_t NewFnPtr) {
  auto XRayID = llvm::cantFail(getXRayID(OldFnPtr));

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

  return llvm::Error::success(); // FIXME the code below causes segfaults.

  llvm::SmallVector<std::pair<pb::FunctionSymbol const&, DySymbol>, 10> NewCode;

  for (pb::FunctionSymbol const& Request : CR.symbols()) {
    auto &Label = Request.label();
    auto MaybeSymbol = Dylib->requireSymbol(Label);

    if (!MaybeSymbol)
      return MaybeSymbol.takeError();

    auto DySym = MaybeSymbol.get();
    if (!DySym.Symbol.getFlags().isCallable())
      return makeError("JITed function symbol is not callable.");

    NewCode.push_back({Request, DySym});
  }

  // Nothing to do!
  if (NewCode.empty())
    return llvm::Error::success();

  if (LOG) Dylib->dump(log());

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

    redirectTo(OrigSymb.addr(), NewSymb.Symbol.getAddress());
  }

  return llvm::Error::success();
}

} // end namespace
