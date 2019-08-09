
#include "halomon/Patcher.h"
#include "halomon/Error.h"
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


  void dump(std::ostream &out) const {
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
      if (LOG) log << "clock_gettime errno=" << errno << "\n";
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
      if (LOG) Log.dump(log); // lol

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

Patcher::Patcher() {
  __xray_init();

  // populate the initial map of addrs to IDs.
  MaxID = __xray_max_function_id();
  for (size_t i = 0; i < MaxID; i++) {
    AddrToID[__xray_function_address(i)] = i;
  }
}

void Patcher::measureRunningTime(uint64_t FnPtr) {
  auto id = AddrToID[FnPtr];
  __xray_set_handler(timingHandler);
  __xray_patch_function(id);
}

} // end namespace
