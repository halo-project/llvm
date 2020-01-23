#pragma once

#include <time.h>
#include <cinttypes>
#include <cmath>
#include <stack>
#include <cassert>

#include "halomon/SummaryStats.h"

#include "Logging.h"

namespace halo {
static constexpr uint64_t NanosecondsPerSecond = 1000ULL * 1000 * 1000;


// returns the time in nanoseconds.
inline uint64_t getTimeStamp(clockid_t Kind = CLOCK_THREAD_CPUTIME_ID) {
  timespec TS;
  int result = clock_gettime(Kind, &TS);
  if (result != 0) {
    logs() << "clock_gettime errno=" << errno << "\n";
    TS = {0, 0};
  }
  return TS.tv_sec * NanosecondsPerSecond + TS.tv_nsec;
}


// Time and entry-count logger for a specific function.
class TimeLog {
public:
  static constexpr bool Geometric = true;
  static constexpr size_t MaxDepth = 128;

  // FIXME: this shouldn't be needed!
  uint64_t samples() const { return RunningTime.samples(); }

  inline void entryEvent() {
    auto EntryTime = getTimeStamp();

    if (LastEntryTime != 0)
      TimeBetweenCalls.observe(EntryTime - LastEntryTime);

    LastEntryTime = EntryTime;


    if (EntryStack.size() >= TimeLog::MaxDepth) {
        Excess++;
        return;
      }

    EntryStack.push(EntryTime);
  }


  inline void exitEvent() {
    if (Excess > 0) {
      Excess--;
      return;
    }

    if (EntryStack.empty())
      return;

    auto End = getTimeStamp();
    auto Start = EntryStack.top(); // retrieve top element
    EntryStack.pop(); // drop top element

    uint64_t NewElapsedTime = End - Start;

    assert(NewElapsedTime > 0);

    // NOTE: By taking the log, we're computing a geometric mean.
    if (Geometric)
      NewElapsedTime = std::log2(NewElapsedTime);

    RunningTime.observe(NewElapsedTime);
  }

  void dump(llvm::raw_ostream &out) const {
    out << "running time stats:\n\t";
    RunningTime.dump(out);

    out << "time between calls stats:\n\t";
    TimeBetweenCalls.dump(out);
  }

private:
  // stack of currently-active entry times, in ns
  // This is needed to properly measure running times for recursive functions.
  std::stack<uint64_t> EntryStack;
  uint64_t Excess = 0;

  // the most recent time the function has been entered
  // this is needed to handle leaf functions when tracking
  // the time between calls
  uint64_t LastEntryTime = 0;

  SummaryStats<uint64_t> RunningTime;
  SummaryStats<uint64_t> TimeBetweenCalls;
};


} // end namespace