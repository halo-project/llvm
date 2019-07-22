#pragma once

#include "halomon/MonitorState.h"

namespace asio = boost::asio;

namespace halo {
  namespace linux {
    // 'true' means there was an error.

    bool setup_sigio_fd(asio::io_service &,
                        asio::posix::stream_descriptor &SigSD,
                        int &SigFD);

    bool setup_perf_events(int &PerfFD, uint8_t* &EventBuf, size_t &EventBufSz, size_t &PageSz);
    void process_new_samples(MonitorState *MS, uint8_t *EventBuf, size_t EventBufSz, const size_t PageSz);
    bool close_perf_events(int PerfFD, uint8_t* EventBuf, size_t EventBufSz);

    void start_sampling(int PerfFD);
    void reset_sampling_counters(int PerfFD);
    void stop_sampling(int PerfFD);
    void set_sampling_period(int PerfFD, uint64_t Period);
  }
}
