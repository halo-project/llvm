#pragma once

#include "Logging.h"
#include "Messages.pb.h"
#include "xray/xray_interface.h"
#include "halomon/CodePatcher.h"

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


class CallCountProfiler {
public:

  static void Serialize(CodePatcher const& P, pb::CallCountData &CCD) {
    CCD.set_timestamp(getTimeStamp(CLOCK_MONOTONIC_RAW));

    auto Map = CCD.mutable_function_counts();
    for (size_t i = 0; i < P.RedirectionTable.size(); i++) {
      auto const& Info = P.Metadata[i];

      // skip if it's not patched, no call counts are being accumulated
      if (Info.first == PatchingStatus::Unpatched)
        continue;

      // function addr -> call count
      Map->insert({Info.second, P.RedirectionTable[i].CallCount});
    }

  }
};


}