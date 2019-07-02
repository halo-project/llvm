#pragma once

#include <cinttypes>

#include "llvm/DebugInfo/Symbolize/Symbolize.h"
#include "llvm/ADT/Optional.h"

// https://www.boost.org/doc/libs/1_65_0/libs/icl/doc/html/boost_icl/interface/function_synopsis.html
#include "boost/icl/interval_map.hpp"

namespace sym = llvm::symbolize;
namespace icl = boost::icl;

namespace halo {

using IDType = uint64_t;

enum DataKind {
  InstrPtr,
  TimeStamp
};


using FunctionInfo = std::string;


class CodeRegionInfo {
public:
  // interval map FROM code address offset TO function information
  using CodeMap = icl::interval_map<uint64_t, FunctionInfo>;

  CodeRegionInfo() {}

  ~CodeRegionInfo() {}

  llvm::Optional<FunctionInfo> lookup(uint64_t IP);
  bool loadObjFile(std::string Path);

private:
  // map FROM object filename TO code-section vector.
  std::map<std::string, uint64_t> ObjFiles;

  // interval map FROM this process's virtual-memory code addresses
  //              TO <an index of the code-section vector, VMA adjustment>.
  icl::interval_map<uint64_t, std::pair<size_t, uint64_t>> VMAResolver;

  // the code-section vector.
  std::vector<CodeMap> Data;

};


class Profiler {
private:
  uint64_t FreeID = 0;

  std::string ProcessTriple;
  std::string HostCPUName;

  CodeRegionInfo CRI;

public:

  IDType newSample() { return FreeID++; }

  Profiler(std::string SelfBinPath)
             : ProcessTriple(llvm::sys::getProcessTriple()),
               HostCPUName(llvm::sys::getHostCPUName()) {
    CRI.loadObjFile(SelfBinPath);
  }

  ~Profiler() {}

  void recordData1(IDType, DataKind, uint64_t);
  void recordData2(IDType, DataKind, uint64_t, uint64_t);

};


} // end halo namespace
