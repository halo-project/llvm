#pragma once

#include "halomon/CodePatcher.h"
#include "halomon/DynamicLinker.h"
#include "halomon/LinuxPerfEvents.h"
#include "halomon/XRayProfiler.h"

#include <list>

// NOTE: we are Linux only right now, but the public interface
// will try to remain OS independent.

namespace asio = boost::asio;

namespace halo {

// members related to reading from perf events FD
struct SignalHandler {

  SignalHandler() : SigSD(PerfSignalService) {
      // setup the SIGIO handler
    if (linux::setup_sigio_fd(PerfSignalService, SigSD, SigFD) )
      exit(EXIT_FAILURE);
  }

  asio::io_service PerfSignalService;
  asio::posix::stream_descriptor SigSD;
  int SigFD; // TODO: do we need to close this, or will SigSD's destructor do that for us?
  signalfd_siginfo SigFDInfo;
};

///////////////
// Maintains the working state of the Halo Monitor thread.
// This is effectively the global state of the client-side Halo system.
class MonitorState {
private:
  std::list<linux::PerfHandle> Handles;
  size_t PageSz;

  // data members related to sampling state
  bool SamplingEnabled;
  std::vector<pb::RawSample> RawSamples;

  SignalHandler &Handler;
  MonitorState *Monitor;  // why did I do this?

  // Boost.Asio methods to enqueue async reads of the SIGIO signal to obtain sampling info.
  void handle_signalfd_read(const boost::system::error_code &Error, size_t BytesTransferred);
  void schedule_signalfd_read();

public:
  Client Net;
  CodePatcher Patcher;
  DynamicLinker Linker;
  XRayProfiler Profiler;

  // information about this process
  std::string ExePath;

  MonitorState(SignalHandler &, std::string const& hostname, std::string const& port);
  ~MonitorState();

  // tends to the instrumented functions by flushing the
  // event queue and sending it to the server.
  void poll_instrumented_fns();

  // methods related to sampling
  void start_sampling();
  void reset_sampling_counters();
  void poll_for_sample_data(); // populates the RawSamples with new data.
  void send_samples();
  void set_sampling_period(uint64_t period);
  void stop_sampling();

  llvm::Error gather_module_info(std::string ObjPath, CodePatcher const&, pb::ModuleInfo*);

  // methods related to client-server communication
  void server_listen_loop();
  void check_msgs();

  pb::RawSample& newSample() {
    RawSamples.emplace_back();
    return RawSamples.back();
  }

};

} // end namespace halo
