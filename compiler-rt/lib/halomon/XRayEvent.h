#pragma once

#include <thread>
#include "Logging.h"
#include "xray/xray_interface_internal.h"

namespace halo {

  static constexpr uint64_t NanosecondsPerSecond = 1000ULL * 1000 * 1000;

  struct XRayEvent {
    uint64_t Time;
    std::thread::id Thread;

    // raw events from instrumentation are in the XRayID form,
    // and when processed by the monitor they are converted
    // to be FuncPtrs.
    union {
      int32_t XRayID;
      uint64_t FuncPtr;
    };

    uint64_t EntryCount;

    XRayEvent(uint64_t time, std::thread::id thread, int32_t xrayID, uint64_t entries)
      : Time(time), Thread(thread), XRayID(xrayID), EntryCount(entries) {}
  };


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


}