#pragma once

#include <boost/asio.hpp>
#include <sys/signalfd.h>
#include <list>

#ifndef BOOST_ASIO_HAS_POSIX_STREAM_DESCRIPTOR
  #error "Boost ASIO POSIX support required"
#endif

#include "halomon/Client.h"

namespace asio = boost::asio;

namespace halo {
  class MonitorState;

  namespace linux {

    // Per-CPU registrations with the perf_events API.
    //
    // Since we want to track all "tasks", aka threads,
    // of this process, we have to create a handle per CPU.
    // As far as I can tell, "CPU" in the kernel means 'hardware thread',
    // not CPU chips on the system
    class PerfHandle {
    public:

      // CPU / PID are as defined by perf_events API.
      // PageSz must be the system's page size.
      PerfHandle(MonitorState*, int CPU, int PID, size_t PageSz);
      ~PerfHandle();

      // polls the ASIO task to check for new data
      // that data is then added to the MonitorState of this Handle.
      void poll() { PerfSignalService.poll(); }

      void start_sampling();
      void reset_sampling_counters();
      void stop_sampling();
      void set_sampling_period(uint64_t Period);

private:
      // Boost.Asio methods to enqueue async reads etc, for reading sampling info.
      void handle_signalfd_read(const boost::system::error_code &Error, size_t BytesTransferred);
      void schedule_signalfd_read();

      void process_new_samples();

      int FD = -1; // a file descriptor used to control the sampling system
      uint8_t* EventBuf; // from mmapping the perf file descriptor.
      size_t EventBufSz;  // size of the mmapping
      size_t PageSz;

      // members related to reading from perf events FD
      asio::io_service PerfSignalService;
      asio::posix::stream_descriptor SigSD;
      int SigFD; // TODO: do we need to close this, or will SigSD's destructor do that for us?
      signalfd_siginfo SigFDInfo;

      MonitorState *Monitor;


    };


    // 'true' means there was an error.
    // this function is primarily used internally to setup a PerfHandler
    bool setup_sigio_fd(asio::io_service &,
                        asio::posix::stream_descriptor &SigSD,
                        int &SigFD);

    // registers perf_event handles with the kernel for each CPU on the system
    // and places them in the list
    void open_perf_handles(MonitorState *, std::list<PerfHandle> &Handles);

    // obtains the path to the currently executing process's executable.
    std::string get_self_exe();
  }
}
