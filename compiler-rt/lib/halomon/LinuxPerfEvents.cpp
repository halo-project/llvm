#include <cstdio>
#include <cstring>
#include <cassert>
#include <cstdlib>
#include <string>
#include "Logging.h"
#include <atomic>
#include <thread>

#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/signal.h>

#include <asm/unistd.h>

#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

// Don't include the Linux perf_event.h header because we're using the
// libpfm-provided header instead.
// #include <linux/perf_event.h>
#include <perfmon/pfmlib.h>
#include <perfmon/pfmlib_perf_event.h>

#include <linux/version.h>

#if LINUX_VERSION_CODE < KERNEL_VERSION(3,4,0)
  #error "Kernel versions older than 3.4 are incompatible."
#endif

#include "halomon/LinuxPerfEvents.h"
#include "halomon/MonitorState.h"
#include "Logging.h"

#define IS_POW_TWO(num)  (num) != 0 && (((num) & ((num) - 1)) == 0)


namespace asio = boost::asio;


namespace halo {
namespace linux {

void handle_perf_event(MonitorState *MS, perf_event_header *EvtHeader) {

  if (EvtHeader->type == PERF_RECORD_SAMPLE) {
    // clogs() << "PERF_RECORD_SAMPLE event.\n";

    struct SInfo {
      perf_event_header header;
      uint64_t sample_id;           // PERF_SAMPLE_IDENTIFIER
      uint64_t ip;                  // PERF_SAMPLE_IP
      uint32_t pid, tid;            // PERF_SAMPLE_TID
      uint64_t time;                // PERF_SAMPLE_TIME
      uint64_t addr;                // PERF_SAMPLE_ADDR
      uint64_t stream_id;           // PERF_SAMPLE_STREAM_ID

      // PERF_SAMPLE_CALLCHAIN
      uint64_t nr;
      uint64_t ips[1];        // ips[nr] length array.
    };

    struct SInfo2 {
      // PERF_SAMPLE_BRANCH_STACK
      uint64_t bnr;
      perf_branch_entry lbr[1];     // lbr[bnr] length array.
    };

    // struct SInfo3 {
    //   uint64_t weight;              // PERF_SAMPLE_WEIGHT
    //   perf_mem_data_src data_src;   // PERF_SAMPLE_DATA_SRC
    // };

    SInfo *SI = (SInfo *) EvtHeader;
    SInfo2 *SI2 = (SInfo2 *) &SI->ips[SI->nr];
    // SInfo3 *SI3 = (SInfo3 *) &SI2->lbr[SI2->bnr];

    pb::RawSample &Sample = MS->newSample();

    // find out whether this IP is exact or not.
    // auto IPSample = SI->header.misc & PERF_RECORD_MISC_EXACT_IP ?
    //                 DataKind::InstrPtrExact : DataKind::InstrPtr;

    Sample.set_instr_ptr(SI->ip);
    Sample.set_thread_id(SI->tid);
    Sample.set_time(SI->time);

    // record the call chain.
    uint64_t ChainLen = SI->nr;
    uint64_t* CallChain = (uint64_t*)&(SI->ips);
    for (uint64_t i = 0; i < ChainLen; ++i) {
      Sample.add_call_context(CallChain[i]);
    }

    uint64_t LBRLen = SI2->bnr;
    perf_branch_entry* LBR = (perf_branch_entry*)&(SI2->lbr);
    for (uint64_t i = 0; i < LBRLen; ++i) {
      // look in source code of linux kernel for sizes of these fields.
      // in particular, everything other than from/to are part of a bitfield
      // of varying sizes.
      perf_branch_entry* BR = LBR + i;
      pb::BranchInfo *BI = Sample.add_branch();
      BI->set_from(BR->from);
      BI->set_to(BR->to);
      BI->set_mispred((bool)BR->mispred);
      BI->set_predicted((bool)BR->predicted);
    }

  } else {
    // clogs() << "some unhandled perf event was encountered.\n";
  }

}

// reads the ring-buffer of perf data from perf_events for a single handle.
// returns true if the readyFD matches this handle.
bool PerfHandle::process_new_samples(int readyFD) {

  // is it ours?
  if (readyFD != FD)
    return false;

  perf_event_mmap_page *Header = (perf_event_mmap_page *) EventBuf;
  uint8_t *DataPtr = EventBuf + PageSz;
  const size_t NumEventBufPages = EventBufSz / PageSz;


  /////////////////////
  // This points to the head of the data section.  The value continu‐
  // ously  increases, it does not wrap.  The value needs to be manu‐
  // ally wrapped by the size of the mmap buffer before accessing the
  // samples.
  //
  // On  SMP-capable  platforms,  after  reading the data_head value,
  // user space should issue an rmb().
  //
  // NOTE -- an rmb is a memory synchronization operation.
  // source: https://community.arm.com/developer/ip-products/processors/b/processors-ip-blog/posts/memory-access-ordering-part-2---barriers-and-the-linux-kernel
  const uint64_t DataHead = Header->data_head;
  __sync_synchronize();

  const uint64_t TailStart = Header->data_tail;



  // Run through the ring buffer and handle the new perf event samples.
  // It's read from Tail --> Head.

  // a contiguous buffer to hold the event data.
  std::vector<uint8_t> TmpBuffer;

  // It's always a power of two size, so we use & instead of % to wrap
  const uint64_t DataPagesSize = (NumEventBufPages - 1)*PageSz;
  const uint64_t DataPagesSizeMask = DataPagesSize - 1;
  assert(IS_POW_TWO(DataPagesSize));

  uint64_t TailProgress = 0;
  while (TailStart + TailProgress != DataHead) {
    uint64_t Offset = (TailStart + TailProgress) & DataPagesSizeMask;
    perf_event_header *BEvtHeader = (perf_event_header*) (DataPtr + Offset);

    uint16_t EvtSz = BEvtHeader->size;
    if (EvtSz == 0)
        break;

    // We copy the data out whether it wraps around or not.

    // TODO: an optimization would be to only copy if wrapping happened.
    // Note that I've tried to do this and it's not as simple as it should be.
    // There seem to be special restrictions on accessing the ring buffer's
    // contents which are being avoided / simplified by always copying it
    // out immediately before processing. Perhaps we cannot access the contents
    // out-of-order? - kavon
    TmpBuffer.resize(EvtSz);

    // copy this event's data, stopping at the end of the ring buffer if needed.
    std::copy(DataPtr + Offset,
              DataPtr + std::min(Offset + EvtSz, DataPagesSize),
              TmpBuffer.begin());

    // if the rest of the event's data wrapped around, copy the data
    // from the start of the ring buff onto the end of our temp buffer.
    if (Offset + EvtSz > DataPagesSize) {
      uint64_t ODiff = (Offset + EvtSz) - DataPagesSize;
      std::copy(DataPtr, DataPtr + ODiff,
                TmpBuffer.begin() + (DataPagesSize - Offset));
    }

    handle_perf_event(Monitor, (perf_event_header*) TmpBuffer.data());

    TailProgress += EvtSz;

  } // end of ring buffer processing loop

  // done reading the ring buffer.
  // issue a smp_store_release(header.data_tail, current_tail_position)
  __sync_synchronize();
  Header->data_tail = TailStart + TailProgress;

  return true; // we performed a read
}

/// Provides a type-safe and slightly robust interface to making
/// a recoverable `syscall` to perf_event_open.
///
/// If a failure occurs, then the callback is invoked with the errno value
/// and a modifiable reference to an attr struct that has the same contents
/// as the original one passed in. Do NOT use the original pointer since
/// this call might pass a different one to the call back!
///
/// @param attr which might be modified by the system call.
///
inline int try_perf_event_open(perf_event_attr *attr,
                           pid_t pid, int cpu, int group_fd,
                           unsigned long flags,
                           std::function<int(int, perf_event_attr &)> Callback) {

#ifndef NDEBUG
  // In some scenarios, the attr struct is modified by the system call,
  // e.g., if -1 is returned and errno is set to E2BIG.
  // I'm sure there are other weird situations beyond that so when
  // asserts / debugging is enabled, we check that the call is being sensible.
  perf_event_attr attr_copy;
  memcpy(&attr_copy, attr, sizeof(perf_event_attr));
#endif

  int FD = syscall(__NR_perf_event_open, attr, pid, cpu, group_fd, flags);
  int ErrNo = errno;

  if (FD == -1) {
    // in this case, the syscall modified the struct so we fix it up.
    if (ErrNo == E2BIG)
      attr->size = sizeof(perf_event_attr);

    #ifndef NDEBUG
      // ensure that the contents are still the same.
      if (memcmp(&attr_copy, attr, sizeof(perf_event_attr) != 0))
        fatal_error("the perf_event_open modified the attr struct in an unexpected way!");
    #endif

    return Callback(ErrNo, *attr);
  }

  return FD;
}


////////////////////
// This function enables Linux's perf_events monitoring on the given
// "process/thread" and "cpu" as defined by perf_event_open.
// Please see the manpage for perf_event_open for more details on those args.
//
// The Name corresponds to a string describing the type of event
// to track. The EventPeriod describes how many of that event should occur
// before a sample is provided.
// libpfm allows you to use any valid name as defined in
// `perf list -v` utility itself.
//
// More info:
// - run `perf list -v` in your terminal to get a list of events.
// - http://web.eece.maine.edu/~vweaver/projects/perf_events/generalized_events/
//
// Based on code by Hal Finkel (hfinkel@anl.gov).
// Modified by Kavon Farvardin.
//
int get_perf_events_fd(const std::string &Name,
                       const uint64_t EventPeriod,
                       const pid_t TID,
                       const int CPU,
                       const int NumEventBufPages, // Must be set to (2^n)+1 for n >= 1
                       const int PageSz) {           // the system's page size

  assert(NumEventBufPages >= 3);
  assert(IS_POW_TWO(NumEventBufPages - 1));
  assert(IS_POW_TWO(PageSz));

  perf_event_attr Attr;
  memset(&Attr, 0, sizeof(perf_event_attr));
  Attr.size = sizeof(perf_event_attr);

  pfm_perf_encode_arg_t Arg;
  memset(&Arg, 0, sizeof(pfm_perf_encode_arg_t));
  Arg.size = sizeof(pfm_perf_encode_arg_t);
  Arg.attr = &Attr; // hand the perf_event_attr to libpfm for initalization

  int Ret = pfm_get_os_event_encoding(Name.c_str(), PFM_PLM3, PFM_OS_PERF_EVENT_EXT, &Arg);
  if (Ret != PFM_SUCCESS) {
    std::string Msg = pfm_strerror(Ret);
    clogs() << "Unable to get event encoding for " << Name << ": " <<
                Msg << "\n";
    return -1;
  }

  // The disabled bit specifies whether the counter starts  out  dis‐
  // abled  or  enabled.  If disabled, the event can later be enabled
  // by ioctl(2), prctl(2), or enable_on_exec.
  Attr.disabled = 1;

  // The inherit bit specifies that this counter should count events of child tasks as well as
  // the  task  specified.  This applies only to new children, not to any existing children at
  // the time the counter is created (nor to any new children of existing children).
  //
  // NOTE: this is used to ensure that any new threads spawned by the process are
  // also tracked by perf.
  Attr.inherit = 1;

  // These must be set, or else this process would require sudo
  // We only want to know about this process's code.
  Attr.exclude_kernel = 1;
  Attr.exclude_hv = 1;

  // NOTE: A flag to consider -- don't count when CPU is idle.
  // Attr.exclude_idle = 1;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,1,0)
  // If  use_clockid  is  set, then this field selects which internal
  // Linux timer to use for timestamps.   The  available  timers  are
  // defined   in  linux/time.h,  with  CLOCK_MONOTONIC,  CLOCK_MONO‐
  // TONIC_RAW, CLOCK_REALTIME, CLOCK_BOOTTIME,  and  CLOCK_TAI  cur‐
  // rently supported.
  Attr.use_clockid = 1;
  Attr.clockid = CLOCK_MONOTONIC_RAW;
#endif

  // If this bit is set, then fork/exit notifications are included in
  // the ring buffer.
  Attr.task = 1;

  // The comm bit enables tracking of process command name  as  modi‐
  // fied  by the exec(2) and prctl(PR_SET_NAME) system calls as well
  // as writing to /proc/self/comm.  If the comm_exec  flag  is  also
  // successfully set (possible since Linux 3.16), then the misc flag
  // PERF_RECORD_MISC_COMM_EXEC can  be  used  to  differentiate  the
  // exec(2) case from the others.
  Attr.comm = 1;
  #if LINUX_VERSION_CODE >= KERNEL_VERSION(3,16,0)
    Attr.comm_exec = 1;
  #endif

  // The mmap bit enables generation of PERF_RECORD_MMAP samples  for
  // every mmap(2) call that has PROT_EXEC set.  This allows tools to
  // notice new executable code being mapped into a program  (dynamic
  // shared  libraries  for  example) so that addresses can be mapped
  // back to the original code.
  Attr.mmap = 1;

  // the period indicates how many events of kind "Name" should happen until
  // a sample is provided.
  Attr.sample_period = EventPeriod;
  Attr.sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_DATA_SRC | PERF_SAMPLE_WEIGHT |
                     PERF_SAMPLE_ADDR | PERF_SAMPLE_TIME |
                     PERF_SAMPLE_TID | PERF_SAMPLE_IDENTIFIER |
                     PERF_SAMPLE_STREAM_ID | PERF_SAMPLE_BRANCH_STACK |
                     PERF_SAMPLE_CALLCHAIN;

  // Note: The callchain is collected in kernel space (and must be
  // collected there, as the context might have changed by the time we see
  // the sample). It is not tied to each sample, however, but collected at
  // interrupt time. If the application was not compiled to preserve frame
  // pointers, etc., then the information might not be complete.
  // Also, if the callchain is truncated, consider increasing:
  // /proc/sys/kernel/perf_event_max_stack
  Attr.exclude_callchain_kernel = 1;

  Attr.wakeup_watermark = (NumEventBufPages-1)*PageSz/2;
  Attr.watermark = 1;

  // 2 = Request no-skid (CPU-sampled events), 1 = Request constant skid.
  Attr.precise_ip = 2;

  // Note: For Intel hardware, these LBR records are only really associated
  // with the PEBS samples starting with Ice Lake, etc.
  Attr.branch_sample_type = PERF_SAMPLE_BRANCH_USER
  // choose specific branch types if supported by kernel.
  #if LINUX_VERSION_CODE >= KERNEL_VERSION(3,16,0)
                          | PERF_SAMPLE_BRANCH_ANY_CALL
                          | PERF_SAMPLE_BRANCH_ANY_RETURN
                          | PERF_SAMPLE_BRANCH_COND;
  #else
                          // ask for whatever is available!
                          | PERF_SAMPLE_BRANCH_ANY;
  #endif


  // NOTE: For newer Intel hardware, we can use PERF_SAMPLE_BRANCH_CALL_STACK.
  // NOTE that PERF_SAMPLE_BRANCH_ANY gives you everything, including local
  // conditional branches and transactional memory stuff etc. see the documentation.

  // NOTE: we have to disable this because the libpfm attr field and
  // the system's kernel can be mismatched. on my system pfm is too old for this.
  // #if LINUX_VERSION_CODE >= KERNEL_VERSION(4,8,0)
    // NOTE: this specifies the maximum depth of the call chain to sample,
    // when asking for the CALLCHAIN.
    // Must be <= /proc/sys/kernel/perf_event_max_stack
    // and it seems that on systems that do not have this field, the kernel
    // traces the whole stack.
    // Attr.sample_max_stack = 10;
  // #endif


  int NewPerfFD = try_perf_event_open(&Attr, TID, CPU, -1, 0, [&](int ErrNo, perf_event_attr &Attr) {
    // Unfortunately, some older hardware (at least Ivybridge)
    // does not support sampling the BTB in a specific capacity (e.g., asking
    // for only CALL or RETURN) so we retry with just ANY branch.

    Attr.branch_sample_type = PERF_SAMPLE_BRANCH_USER
                            | PERF_SAMPLE_BRANCH_ANY;

    return try_perf_event_open(&Attr, TID, CPU, -1, 0, [](int ErrNo, perf_event_attr &Attr) {

      // okay then we give up. print an error and forward the invalid FD
      clogs() << "Unsuccessful call to perf_event_open: ";
      switch (ErrNo) {
        case E2BIG: {clogs() << "E2BIG\n"; break;}
        case EACCES: {clogs() << "EACCES\n"; break;}
        case EBADF: {clogs() << "EBADF\n"; break;}
        case EBUSY: {clogs() << "EBUSY\n"; break;}
        case EFAULT: {clogs() << "EFAULT\n"; break;}
        case EINVAL: {clogs() << "EINVAL\n"; break;}
        case EMFILE: {clogs() << "EMFILE\n"; break;}
        case ENODEV: {clogs() << "ENODEV\n"; break;}
        case ENOSPC: {clogs() << "ENOSPC\n"; break;}
        case ENOSYS: {clogs() << "ENOSYS\n"; break;}
        case EOPNOTSUPP: {clogs() << "EOPNOTSUPP\n"; break;}
        case EOVERFLOW: {clogs() << "EOVERFLOW\n"; break;}
        case EPERM: {clogs() << "EPERM\n"; break;}
        case ESRCH: {clogs() << "ESRCH\n"; break;}
        default: {clogs() << "Code = " << ErrNo << " (unknown name)\n"; break;}
      };
      return -1;
    });
  });

  return NewPerfFD;
}



// Since perf_events sends SIGIO signals periodically to notify us
// of new profile data, we need to service these notifications.
// This function directs such signals to the file descriptor.
bool setup_sigio_fd(asio::io_service &PerfSignalService, asio::posix::stream_descriptor &SigSD, int &SigFD) {
  // make SIGIO signals available through a file descriptor instead of interrupts.
  sigset_t SigMask;
  sigemptyset(&SigMask);
  sigaddset(&SigMask, SIGIO);

  if (sigprocmask(SIG_BLOCK, &SigMask, NULL) == -1) {
    std::string Msg = strerror(errno);
    clogs() << "Unable to block signals: " << Msg << "\n";
    return true;
  }

  SigFD = signalfd(-1, &SigMask, 0);
  if (SigFD == -1) {
    std::string Msg = strerror(errno);
    clogs() << "Unable create signal file handle: " << Msg << "\n";
    return true;
  }

  // setup to read from the fd.
  SigSD = asio::posix::stream_descriptor(PerfSignalService, SigFD);
  return false;
}

void PerfHandle::start_sampling() {
  ioctl(FD, PERF_EVENT_IOC_ENABLE, PERF_IOC_FLAG_GROUP);
}

void PerfHandle::reset_sampling_counters() {
  ioctl(FD, PERF_EVENT_IOC_RESET, PERF_IOC_FLAG_GROUP);
}

void PerfHandle::stop_sampling() {
  ioctl(FD, PERF_EVENT_IOC_DISABLE, PERF_IOC_FLAG_GROUP);
}

void PerfHandle::set_sampling_period(uint64_t Period) {
  uint64_t NewPeriod = Period;
  ioctl(FD, PERF_EVENT_IOC_PERIOD, &NewPeriod);
}

std::string get_self_exe() {
  // get the path to this process's executable.
  std::vector<char> buf(PATH_MAX);
  ssize_t len = readlink("/proc/self/exe", buf.data(), buf.size()-1);
  if (len == -1) {
    std::string Msg = strerror(errno);
    clogs() << Msg << "\n";
    halo::fatal_error("path to process's executable not found.");
  }
  buf[len] = '\0'; // null terminate
  return std::string(buf.data());
}



void open_perf_handles(MonitorState *Mon, std::list<PerfHandle> &Handles) {
  size_t PageSz = sysconf(_SC_PAGESIZE);
  pid_t PID = getpid();

  // Handle kernels built to support large CPU sets as suggested by the
  // sched_setaffinity man page.

  // NOTE: a 'CPU' is equal to a 'core' in layman's terms.

  cpu_set_t *AffMask;
  size_t AffSize;
  int NumCPUs = CPU_SETSIZE;

  AffMask = CPU_ALLOC(NumCPUs);
  AffSize = CPU_ALLOC_SIZE(NumCPUs);
  CPU_ZERO_S(AffSize, AffMask);

  do {
    // figure out how many CPUs are available to this process
    if (sched_getaffinity(0, AffSize, AffMask) == -1) {
      if (errno == EINVAL && NumCPUs < (CPU_SETSIZE << 8)) {
        CPU_FREE(AffMask);
        NumCPUs <<= 2;
        continue;
      }
      std::string Msg = strerror(errno);
      clogs() << "Unable to get affinity mask: " << Msg << "\n";
      fatal_error("error in open_perf_handles");
    }

    break;
  } while(true);

  for (int CPU = 0; CPU < NumCPUs; CPU++) {
    if (!CPU_ISSET_S(CPU, AffSize, AffMask))
          continue;

    Handles.emplace_back(Mon, CPU, PID, PageSz);
  }
}



PerfHandle::PerfHandle(MonitorState *mon, int CPU, int MyPID, size_t pagesz)
          :  Monitor(mon), PageSz(pagesz) {

  // NOTE: By default on Ubuntu 18.04, /proc/sys/kernel/perf_event_mlock_kb
  // is set to a 516KiB max of data for this buffer, aka, 512KiB + 4KiB or
  // equivalently 128 + 1 pages *on the entire system*.
  //
  // Because we will end up creating one buffer *per core, per process*,
  // it is very easy to exceed this limit if you launch many instances of
  // the Halo-enabled program.
  //
  // The mmap size must be (2^n) + 1 pages, where the first page will be a metadata
  // page (struct perf_event_mmap_page) that contains various bits of infor‐
  // mation such as where the ring-buffer head is.
  //
  // FIXME: this ought to be a parameter of the binary that is obtained
  // by inspecting the variable environment.
  const int NumBufPages = 8+1;

  assert(IS_POW_TWO(NumBufPages - 1));

  int Ret = pfm_initialize();
  if (Ret != PFM_SUCCESS) {
    std::string Msg = pfm_strerror(Ret);
    clogs() << "Failed to initialize PFM library: " << Msg << "\n";
    fatal_error("error in initializing perf handle");
  }

  //////////////
  // open the perf_events file descriptor

  // Here are some large prime numbers to help deter periodicity:
  //
  //   https://primes.utm.edu/lists/small/millions/
  //
  // We want to avoid having as many divisors as possible in case of
  // repetitive behavior, e.g., a long-running loop executing exactly 323
  // instructions per iteration. There's a (slim) chance we sample the
  // same instruction every time because our period is a multiple of 323.
  // In reality, CPUs have noticable non-constant skid, but we don't want to
  // rely on that for good samples.

  std::string EventName = "instructions";
  uint64_t EventPeriod = 15485867;

  FD = get_perf_events_fd(EventName, EventPeriod,
                                  MyPID, CPU, NumBufPages, PageSz);
  if (FD == -1)
    fatal_error("error in perf handle ctor: get_perf_events_fd failed");

  EventBufSz = NumBufPages*PageSz;
  EventBuf = (uint8_t *) mmap(NULL, EventBufSz,
                           PROT_READ|PROT_WRITE, MAP_SHARED, FD, 0);
  if (EventBuf == MAP_FAILED) {
    if (errno == EPERM)
      clogs() << "Consider increasing /proc/sys/kernel/perf_event_mlock_kb or "
                   "allocating less memory for events buffer.\n";
    std::string Msg = strerror(errno);
    clogs() << "Unable to map perf events pages: " << Msg << "\n";
    fatal_error("error in perf handle ctor : unable to map perf events pages");
  }

  // configure the file descriptor
  (void) fcntl(FD, F_SETFL, O_RDWR|O_NONBLOCK|O_ASYNC);
  (void) fcntl(FD, F_SETSIG, SIGIO);
  (void) fcntl(FD, F_SETOWN, MyPID);
}



PerfHandle::~PerfHandle() {
  int ret = munmap(EventBuf, EventBufSz);
  if (ret) {
    std::string Msg = strerror(errno);
    clogs() << "Failed to unmap event buffer: " << Msg << "\n";
    fatal_error("error in PerfHandle dtor 1");
  }

  ret = close(FD);
  if (ret) {
    std::string Msg = strerror(errno);
    clogs() << "Failed to close perf_event file descriptor: " << Msg << "\n";
    fatal_error("error in PerfHandle dtor 2");
  }
}

} // end namespace linux
} // end namespace halo
