
#include "halomon/MonitorState.h"
#include "halomon/LinuxPerfEvents.h"
#include "halomon/Error.h"

#include "llvm/Support/Host.h"


namespace asio = boost::asio;

namespace halo {

MonitorState::MonitorState() : SigSD(PerfSignalService),
                               ProcessTriple(llvm::sys::getProcessTriple()),
                               HostCPUName(llvm::sys::getHostCPUName()),
                               SamplingEnabled(false) {

  // get the path to this process's executable.
  std::vector<char> buf(PATH_MAX);
  ssize_t len = readlink("/proc/self/exe", buf.data(), buf.size()-1);
  if (len == -1) {
    std::cerr << strerror(errno) << "\n";
    fatal_error("path to process's executable not found.");
  }
  buf[len] = '\0'; // null terminate
  ExePath = std::string(buf.data());

  Prof = new Profiler(buf.data());

  // setup the monitor's initial state.
  // TODO: using a struct to hold the Linux Perf Events state with proper
  // destructor etc would be a lot nicer to do. Mainly the PerfFD and EventBuf.
  // and their setup, etc.
  if (linux::setup_perf_events(PerfFD, EventBuf, EventBufSz, PageSz) ||
      linux::setup_sigio_fd(PerfSignalService, SigSD, SigFD) ) {
    exit(EXIT_FAILURE);
  }

  // kick-off the chain of async read jobs for the signal file descriptor.
  schedule_signalfd_read();

  // Connect to the optimization server.
  Conn = new Client("localhost", "29000"); // TODO: get these from an env variable

  if (!(Conn->connect())) {
    exit(EXIT_FAILURE);
  }
}

MonitorState::~MonitorState() {
  // clean-up
  delete Prof;
  delete Conn;

  linux::close_perf_events(PerfFD, EventBuf, EventBufSz);
}

void MonitorState::start_sampling() {
  if (!SamplingEnabled) {
    linux::reset_sampling_counters(PerfFD);
    linux::start_sampling(PerfFD);
    SamplingEnabled = true;
  }
}

void MonitorState::stop_sampling() {
  if (SamplingEnabled) {
    linux::stop_sampling(PerfFD);
    SamplingEnabled = false;
  }
}

void MonitorState::reset_sampling_counters() {
  linux::reset_sampling_counters(PerfFD);
}

void MonitorState::set_sampling_period(uint64_t period) {
    linux::set_sampling_period(PerfFD, period);
}

void MonitorState::handle_signalfd_read(const boost::system::error_code &Error, size_t BytesTransferred) {
  bool IOError = false;
  if (Error) {
    std::cerr << "Error reading from signal file handle: " << Error.message() << "\n";
    IOError = true;
  }

  if (BytesTransferred != sizeof(SigFDInfo)) {
    std::cerr << "Read the wrong the number of bytes from the signal file handle: "
                 "read " << BytesTransferred << " bytes\n";
    IOError = true;
  }

  // TODO: convert this into a debug-mode assert.
  if (SigFDInfo.ssi_signo != SIGIO) {
    std::cerr << "Unexpected signal recieved on signal file handle: "
              << SigFDInfo.ssi_signo << "\n";
    IOError = true;
  }


  // SIGIO/SIGPOLL  (the two names are synonyms on Linux) fills in si_band
  //  and si_fd.  The si_band event is a bit mask containing the same  val‐
  //  ues  as  are filled in the revents field by poll(2).  The si_fd field
  //  indicates the file descriptor for which the I/O event  occurred;  for
  //  further details, see the description of F_SETSIG in fcntl(2).
  //
  //  See 'sigaction' man page for more information.



  // TODO: is it actually an error if we get a SIGIO for a different FD?
  // What if the process is doing IO? How do we forward the interrupt to
  // the right place? What should we do?
  if (SigFDInfo.ssi_fd != PerfFD) {
    std::cerr << "Unexpected file descriptor associated with SIGIO interrupt.\n";
    IOError = true;
  }

  // TODO: it's possibly worth checking ssi_code field to find out what in particular
  // is going on in this SIGIO.
  //  The following values can be placed in si_code for a SIGIO/SIGPOLL  signal:
  //
  // POLL_IN
  //        Data input available.
  // .....
  // see 'sigaction' man page

  if (IOError) {
    // stop the service and don't enqueue another read.
    PerfSignalService.stop(); // TODO: is a stop command right if we only poll?
    return;
  }

  linux::process_new_samples(this, EventBuf, EventBufSz, PageSz);

  // schedule another read.
  schedule_signalfd_read();
}

void MonitorState::schedule_signalfd_read() {
  // read a signalfd_siginfo from the file descriptor
  asio::async_read(SigSD, asio::buffer(&SigFDInfo, sizeof(SigFDInfo)),
    [&](const boost::system::error_code &Error, size_t BytesTransferred) {
      handle_signalfd_read(Error, BytesTransferred);
    });
}

void MonitorState::poll_for_sample_data() {
  if (SamplingEnabled)
    PerfSignalService.poll(); // check for new data
}

} // end namespace halo
