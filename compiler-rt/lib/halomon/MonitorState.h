#pragma once

#include "halomon/CodePatcher.h"
#include "halomon/DynamicLinker.h"
#include "halomon/LinuxPerfEvents.h"

#include <list>

// NOTE: we are Linux only right now, but the public interface
// will try to remain OS independent.

namespace asio = boost::asio;

namespace halo {

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

public:
  Client Net;
  CodePatcher Patcher;
  DynamicLinker Linker;

  // information about this process
  std::string ExePath;

  MonitorState();
  ~MonitorState();

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
