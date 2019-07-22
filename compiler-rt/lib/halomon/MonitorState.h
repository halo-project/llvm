#pragma once

#include <boost/asio.hpp>
#include <sys/signalfd.h>

#ifndef BOOST_ASIO_HAS_POSIX_STREAM_DESCRIPTOR
  #error "Boost ASIO POSIX support required"
#endif

#include "halomon/Profiler.h"
#include "halomon/Client.h"

// NOTE: we are Linux only right now, but the public interface
// will try to remain OS independent.

namespace asio = boost::asio;

namespace halo {

///////////////
// Maintains the working state of the Halo Monitor thread.
// This is effectively the global state of the client-side Halo system.
class MonitorState {
private:
  int PerfFD;       // a file descriptor
  uint8_t* EventBuf;   // from mmapping the perf file descriptor.
  size_t EventBufSz;
  size_t PageSz;

  // members related to reading from perf events FD
  asio::io_service PerfSignalService;
  asio::posix::stream_descriptor SigSD;
  int SigFD; // TODO: do we need to close this, or will SigFD's destructor do that for us?
  signalfd_siginfo SigFDInfo;

  // Boost.Asio methods to enqueue async reads etc.
  void handle_signalfd_read(const boost::system::error_code &Error, size_t BytesTransferred);
  void schedule_signalfd_read();

  // profiling state
  bool SamplingEnabled;
  std::vector<pb::RawSample> RawSamples;

public:
  Profiler *Prof;
  Client *Conn;

  // information about this process
  std::string ExePath;

  MonitorState();
  ~MonitorState();

  // methods related to sampling
  void start_sampling();
  void reset_sampling_counters();
  void poll_for_sample_data(); // populates the RawSamples with new data.
  void set_sampling_period(uint64_t period);
  void stop_sampling();

  pb::RawSample& newSample() {
    RawSamples.emplace_back();
    return RawSamples.back();
  }

};

} // end namespace halo
