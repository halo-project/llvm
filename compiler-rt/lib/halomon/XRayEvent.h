#pragma once

#include <thread>

#include "xray/xray_interface_internal.h"

namespace halo {

  static constexpr uint64_t NanosecondsPerSecond = 1000ULL * 1000 * 1000;

  struct XRayEvent {
    uint64_t Time;
    std::thread::id Thread;
    int32_t Func;
    XRayEntryType Kind;
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