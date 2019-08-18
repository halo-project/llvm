
#include "halomon/MonitorState.h"
#include "halomon/LinuxPerfEvents.h"
#include "halomon/Error.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Object/ELFObjectFile.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SourceMgr.h"
#include "sanitizer_common/sanitizer_procmaps.h"

#include <cassert>
#include <elf.h>


namespace asio = boost::asio;
namespace san = __sanitizer;
namespace object = llvm::object;

namespace halo {

void MonitorState::server_listen_loop() {
  Net.Chan.async_recv([this](msg::Kind Kind, std::vector<char>& Body) {
    if (LOG) log << "got msg ID " << (uint32_t) Kind << "\n";

    switch (Kind) {
      case msg::Shutdown: {
        if (LOG) log << "server session terminated.\n";
      } return; // NOTE: the return.

      case msg::StartSampling: {
        start_sampling();
      } break;

      case msg::StopSampling: {
        stop_sampling();
      } break;

      case msg::ReqMeasureFunction: {
        llvm::StringRef Blob(Body.data(), Body.size());
        pb::ReqMeasureFunction Req;
        Req.ParseFromString(Blob);

        // TODO: invoke patcher.
        if (LOG) log << "Recieved request to measure perf of func "
                  << Req.func_addr() << "\n";

        Patch.measureRunningTime(Req.func_addr());

      } break;

      case msg::CodeReplacement: {
        llvm::StringRef Blob(Body.data(), Body.size());
        pb::CodeReplacement CR;
        CR.ParseFromString(Blob);

        Linker.run(CR);

      } break;

      default: {
        if (LOG) log << "recieved unknown message from server: #"
                  << (uint32_t) Kind << "\n";
      } break;
    };

    server_listen_loop();
  });
}

void MonitorState::gather_module_info(std::string ObjPath, pb::ModuleInfo *MI) {
  MI->set_obj_path(ObjPath);

  ///////////
  // initialize the code map

  auto ResOrErr = object::ObjectFile::createObjectFile(ObjPath);
  if (!ResOrErr) halo::fatal_error("error opening object file!");

  object::OwningBinary<object::ObjectFile> OB = std::move(ResOrErr.get());
  object::ObjectFile *Obj = OB.getBinary();

  // find the range of this object file in the process.
  san::uptr VMAStart, VMAEnd;
  bool res = san::GetCodeRangeForFile(ObjPath.data(), &VMAStart, &VMAEnd);
  if (!res) halo::fatal_error("unable to read proc map for VMA range");

  uint64_t Delta = VMAStart; // Assume PIE is enabled.
  if (auto *ELF = llvm::dyn_cast<object::ELFObjectFileBase>(Obj)) {
    // https://stackoverflow.com/questions/30426383/what-does-pie-do-exactly#30426603
    if (ELF->getEType() == ET_EXEC) {
      Delta = 0; // This is a non-PIE executable.
    }
  }

  MI->set_vma_start(VMAStart);
  MI->set_vma_end(VMAEnd);
  MI->set_vma_delta(Delta);

  // Gather function information from the object file
  for (const object::SymbolRef &Symb : Obj->symbols()) {
    auto MaybeType = Symb.getType();

    if (!MaybeType || MaybeType.get() != object::SymbolRef::Type::ST_Function)
      continue;

    auto MaybeName = Symb.getName();
    auto MaybeAddr = Symb.getAddress();
    uint64_t Size = Symb.getCommonSize();
    if (MaybeName && MaybeAddr && Size > 0) {
      uint64_t Start = MaybeAddr.get();
      assert(Size > 0 && "size 0 function in object file?");

      pb::FunctionInfo *FI = MI->add_funcs();
      FI->set_label(MaybeName.get());
      FI->set_start(Start);
      FI->set_size(Size);
    }
  }

  // Look for various sections in the object file
  for (const object::SectionRef &Sec : Obj->sections()) {
    if (Sec.isBitcode()) {
      llvm::Expected<llvm::StringRef> MaybeData = Sec.getContents();
      if (!MaybeData) halo::fatal_error("unable get bitcode section contents.");

      MI->set_bitcode(MaybeData.get().str());
      continue;
    }

    // test by section name
    llvm::StringRef Name;
    Sec.getName(Name);

    if (Name == ".llvmcmd") {
      llvm::Expected<llvm::StringRef> MaybeData = Sec.getContents();
      if (!MaybeData) halo::fatal_error("unable get cmd section contents.");

      // each space is represented by a NULL character
      llvm::SmallVector<llvm::StringRef, 128> Cmds;
      MaybeData.get().split(Cmds, '\0', /*MaxSplit*/ -1, /*KeepEmpty*/ false);

      for (auto &Flag : Cmds)
        MI->add_build_flags(Flag.str());
    }
  }
}

void MonitorState::send_samples() {
  if (SamplingEnabled) {
    for (const pb::RawSample &Sample : RawSamples) {
      Net.Chan.send_proto(msg::RawSample, Sample);
    }
    RawSamples.clear();
  }
}

MonitorState::MonitorState() : SigSD(PerfSignalService),
                               SamplingEnabled(false),
                               // TODO: get the server addr from an env variable.
                               Net("localhost", "29000"),
                               ExePath(linux::get_self_exe())
                               {

  // setup the monitor's initial state.
  // TODO: using a struct to hold the Linux Perf Events state with proper
  // destructor etc would be a lot nicer to do. Mainly the PerfFD and EventBuf.
  // and their setup, etc. It's a bit of a pain so I've left it alone for now.
  if (linux::setup_perf_events(PerfFD, EventBuf, EventBufSz, PageSz) ||
      linux::setup_sigio_fd(PerfSignalService, SigSD, SigFD) ) {
    exit(EXIT_FAILURE);
  }

  // kick-off the chain of async read jobs for the signal file descriptor.
  schedule_signalfd_read();
}

MonitorState::~MonitorState() {
  // clean-up
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
    if (LOG) log << "Error reading from signal file handle: " << Error.message() << "\n";
    IOError = true;
  }

  if (BytesTransferred != sizeof(SigFDInfo)) {
    if (LOG) log << "Read the wrong the number of bytes from the signal file handle: "
                 "read " << BytesTransferred << " bytes\n";
    IOError = true;
  }

  // TODO: convert this into a debug-mode assert.
  if (SigFDInfo.ssi_signo != SIGIO) {
    if (LOG) log << "Unexpected signal recieved on signal file handle: "
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
    if (LOG) log << "Unexpected file descriptor associated with SIGIO interrupt.\n";
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

void MonitorState::check_msgs() {
  Net.poll();
}

} // end namespace halo
