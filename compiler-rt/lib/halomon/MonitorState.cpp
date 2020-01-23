
#include "halomon/MonitorState.h"
#include "halomon/LinuxPerfEvents.h"
#include "Logging.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSet.h"
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
    logs() << "recv msg ID " << (uint32_t) Kind << "\n";

    switch (Kind) {
      case msg::Shutdown: {
        logs() << "server session terminated.\n";
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

        logs() << "Recieved request to measure perf of func "
                  << Req.func_addr() << "\n";

        auto Error = Patcher.measureRunningTime(Req.func_addr());
        if (Error)
          llvm::report_fatal_error(std::move(Error));

      } break;

      case msg::CodeReplacement: {
        llvm::StringRef Blob(Body.data(), Body.size());
        pb::CodeReplacement CR;
        CR.ParseFromString(Blob);

        // msg::print_proto(CR);

        std::unique_ptr<std::string> ObjFileStorage(CR.release_objfile());
        auto MaybeDylib = Linker.run(std::move(ObjFileStorage));

        if (!MaybeDylib)
          llvm::report_fatal_error(std::move(MaybeDylib.takeError()));

        auto Dylib = std::move(MaybeDylib.get());

        auto Error = Patcher.replaceAll(CR, std::move(Dylib), Net.Chan);

        if (Error)
          llvm::report_fatal_error(std::move(Error));

      } break;

      default: {
        logs() << "recieved unknown message from server: #"
                  << (uint32_t) Kind << "\n";
      } break;
    };

    server_listen_loop();
  });
}

llvm::Error MonitorState::gather_module_info(std::string ObjPath, CodePatcher const& Patcher, pb::ModuleInfo *MI) {
  MI->set_obj_path(ObjPath);

  ///////////
  // initialize the code map

  auto ResOrErr = object::ObjectFile::createObjectFile(ObjPath);
  if (!ResOrErr) return ResOrErr.takeError();

  object::OwningBinary<object::ObjectFile> OB = std::move(ResOrErr.get());
  object::ObjectFile *Obj = OB.getBinary();

  // find the range of this object file in the process.
  san::uptr VMAStart, VMAEnd;
  bool res = san::GetCodeRangeForFile(ObjPath.data(), &VMAStart, &VMAEnd);
  if (!res) return makeError("unable to read proc map for VMA range");

  // Because the generic ObjectFile class is pessimistic about the availability
  // of size information for symbols (i.e., there is an assert checking that
  // size info is only available for symbols with common linkage),
  // we down cast to the specific object file we expect to be using.

  object::ELFObjectFileBase* ELF = llvm::dyn_cast_or_null<object::ELFObjectFileBase>(Obj);
  if (ELF == nullptr)
    return makeError("Only ELF object files are currently supported by Halo Monitor.");

  uint64_t Delta = VMAStart; // Assume PIE is enabled.
  // https://stackoverflow.com/questions/30426383/what-does-pie-do-exactly#30426603
  if (ELF->getEType() == ET_EXEC) {
    Delta = 0; // Then this is a non-PIE executable.
  }

  MI->set_vma_start(VMAStart);
  MI->set_vma_end(VMAEnd);
  MI->set_vma_delta(Delta);

  // Look for various sections in the object file
  llvm::StringSet<> PatchableFuns;
  for (const object::SectionRef &Sec : Obj->sections()) {
    if (Sec.isBitcode()) {
      llvm::Expected<llvm::StringRef> MaybeData = Sec.getContents();
      if (!MaybeData) halo::fatal_error("unable get bitcode section contents.");

      MI->set_bitcode(MaybeData.get().str());
      continue;
    }

    // test by section name
    auto MaybeName = Sec.getName();
    if (!MaybeName)
      continue;

    auto Name = MaybeName.get();
    if (Name == ".llvmcmd") {

      llvm::Expected<llvm::StringRef> MaybeData = Sec.getContents();
      if (!MaybeData) halo::fatal_error("unable get cmd section contents.");

      // each space is represented by a NULL character
      llvm::SmallVector<llvm::StringRef, 128> Cmds;
      MaybeData.get().split(Cmds, '\0', /*MaxSplit*/ -1, /*KeepEmpty*/ false);

      for (auto &Flag : Cmds)
        MI->add_build_flags(Flag.str());


    } else if (Name == ".halo.metadata") {

      llvm::Expected<llvm::StringRef> MaybeData = Sec.getContents();
      if (!MaybeData) halo::fatal_error("unable get halo metadata section contents.");

      llvm::SmallVector<llvm::StringRef, 128> FuncNames;
      MaybeData.get().split(FuncNames, '\0', /*MaxSplit*/ -1, /*KeepEmpty*/ false);

      // insert the names into a set for faster lookup operations.
      // otherwise the names should already be unique
      for (auto &Fn : FuncNames)
        PatchableFuns.insert(Fn);
    }
  }

  // Gather function information from the object file
  for (const object::ELFSymbolRef &Symb : ELF->symbols()) {
    auto MaybeType = Symb.getType();

    if (!MaybeType || MaybeType.get() != object::SymbolRef::Type::ST_Function)
      continue;

    auto MaybeName = Symb.getName();
    auto MaybeAddr = Symb.getAddress();
    uint64_t Size = Symb.getSize();
    if (MaybeName && MaybeAddr && Size > 0) {
      uint64_t Start = MaybeAddr.get();
      assert(Size > 0 && "size 0 function in object file?");
      auto Name = MaybeName.get();

      bool IsPatchable = PatchableFuns.count(Name) == 1;

      if (IsPatchable && !Patcher.isPatchable(Start))
        return makeError("Function marked patchable but unknown to CodePatcher!\n");

      pb::FunctionInfo *FI = MI->add_funcs();
      FI->set_label(Name);
      FI->set_start(Start);
      FI->set_size(Size);
      FI->set_patchable(IsPatchable);
    }
  }

  return llvm::Error::success();
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
    logs() << "Error reading from signal file handle: " << Error.message() << "\n";
    IOError = true;
  }

  if (BytesTransferred != sizeof(SigFDInfo)) {
    logs() << "Read the wrong the number of bytes from the signal file handle: "
                 "read " << BytesTransferred << " bytes\n";
    IOError = true;
  }

  // TODO: convert this into a debug-mode assert.
  if (SigFDInfo.ssi_signo != SIGIO) {
    logs() << "Unexpected signal recieved on signal file handle: "
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
    logs() << "Unexpected file descriptor associated with SIGIO interrupt.\n";
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
