#include <thread>
#include <atomic>
#include "halomon/Logging.h"

#include "halomon/MonitorState.h"
#include "llvm/Support/Host.h"

namespace halo {

////////////////////////////////////////////////
// Main loop of the Halo Monitor
void monitor_loop(MonitorState &M, std::atomic<bool> &ShutdownRequested) {
  /////////////////
  // Setup

  Client &C = M.Net;

  {
    // try to establish a connection with the optimization server.
    while (!C.connect()) {
      if (ShutdownRequested)
        return;

      std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }

    // start listening for messages
    M.server_listen_loop();

    // enroll ourselves with the server.
    pb::ClientEnroll CE;
    CE.set_process_triple(llvm::sys::getProcessTriple());
    CE.set_host_cpu(llvm::sys::getHostCPUName());
    M.gather_module_info(M.ExePath, CE.mutable_module());

    // obtain our data layout from the bitcode.
    M.Linker.setLayout(CE.module().bitcode());

    C.Chan.send_proto(msg::ClientEnroll, CE);
  }


  //////////////////
  // Event Loop

  while (!ShutdownRequested) {

    M.check_msgs();

    M.poll_for_sample_data();
    M.send_samples();

    // M.Prof->dumpSamples();

    // M.Prof->processSamples(M.Conn);



    // TODO: communicate with optimization server and perform code replacement
    // experiments as needed.


    // TODO: this should probably be a random interval.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

  } // end of event loop
}


class HaloMonitor {
private:
  std::thread MonitorThread;
  std::atomic<bool> ShutdownRequested;
  MonitorState State;
public:
  /////////////////////////////////////////////////////////////////////////
  // The main entry-point to start halo's process monitoring system.
  HaloMonitor() : ShutdownRequested(false) {
    // start the monitor thread
    MonitorThread = std::thread(monitor_loop,
                                  std::ref(State), std::ref(ShutdownRequested));

    if (LOG) log() << "Halo Running!\n";
  }

  /////////////////////////////////////////////////////////////////////////
  // shut down the monitor
  ~HaloMonitor() {
    // stop the monitor thread gracefully
    ShutdownRequested = true;
    MonitorThread.join();
  }
};


static HaloMonitor SystemMonitor;

///////////////////
// Static initalizer which causes programs this library is linked with
// to start the monitor.
// Caveats:
//
//     (1) statically linked libraries. In this case, you need to create a
//         .so file that is a linker script to force the linking of this object
//         file. Otherwise, you'll need to create some sort of dependency on
//         this file to ensure it is not dropped!
//
//     (2) the order of static initalizers is technically undefined,
//         so other static initializers may launch threads that we
//         miss out on profiling!
//
// Other ideas:
//  - ld main.o --undefined=__my_static_ctor -lhalomon
//         this injects a dependency on a symbol, to force halo lib to be linked.
//
//  - -Wl,-no-as-needed halolib.so -Wl,-as-needed
//
//  - make the .so file a linker script that demands halomon be included.
///////////////////

} // end namespace halo
