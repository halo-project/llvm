#pragma once

#include <cinttypes>

#include "llvm/DebugInfo/Symbolize/Symbolize.h"

namespace sym = llvm::symbolize;

namespace halo {

using IDType = uint64_t;

enum DataKind {
  InstrPtr,
  TimeStamp
};


class CodeInfo {

};


class CodeRegion {
private:
  std::string BinaryPath;
  uint64_t VMAStart, VMAEnd; // start and end addresses of the code region.
  sym::LLVMSymbolizer *Symbolizer;

public:
  CodeRegion(std::string BinPath, std::string& BinTriple);

  ~CodeRegion() {
    delete Symbolizer;
  }

  void lookupInfo(uint64_t IP);

};


class Profiler {
private:
  uint64_t FreeID = 0;

  std::string ProcessTriple;
  std::string HostCPUName;

  CodeRegion CR;

public:

  IDType newSample() { return FreeID++; }

  Profiler(std::string SelfBinPath)
             : ProcessTriple(llvm::sys::getProcessTriple()),
               HostCPUName(llvm::sys::getHostCPUName()),
               CR(SelfBinPath, ProcessTriple) {
  }

  ~Profiler() {}

  void recordData1(IDType, DataKind, uint64_t);
  void recordData2(IDType, DataKind, uint64_t, uint64_t);

};


} // end halo namespace
