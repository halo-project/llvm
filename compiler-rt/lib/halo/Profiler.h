#pragma once

#include <cinttypes>
#include <memory>

#include "llvm/DebugInfo/Symbolize/Symbolize.h"
#include "llvm/ADT/Optional.h"

// Function interface reference:
// https://www.boost.org/doc/libs/1_65_0/libs/icl/doc/html/boost_icl/interface/function_synopsis.html

// [NOTE identity element]
// https://www.boost.org/doc/libs/1_65_0/libs/icl/doc/html/boost_icl/concepts/map_traits.html#boost_icl.concepts.map_traits.definedness_and_storage_of_identity_elements

#define BOOST_ICL_USE_STATIC_BOUNDED_INTERVALS
#include "boost/icl/interval_map.hpp"

namespace sym = llvm::symbolize;
namespace icl = boost::icl;

namespace halo {

using IDType = uint64_t;

enum DataKind {
  InstrPtr,  // the IP may have some skid.
  InstrPtrExact, // indicates that the IP "points to the actual instruction that triggered the event."
  TimeStamp,
  CallChain
};


struct FunctionInfo {
  uint64_t hits;
  std::string name;

  FunctionInfo() : hits(0), name("<unknown>") {}
  FunctionInfo(llvm::StringRef label) : hits(0), name(label.data()) {}
};


class CodeRegionInfo {
public:

  CodeRegionInfo() {}

  ~CodeRegionInfo() {}

  llvm::Optional<FunctionInfo*> lookup(uint64_t IP);
  bool loadObjFile(std::string Path);

private:
  // interval map FROM code address offset TO function information

  // NOTE:
  // 1. it seems impossible to use a unique_ptr here because
  //    of the interface of interval_map's find().
  // 2. partial_enricher ensures that the map doesn't stupidly ignore
  //    inserts of the identity elem in co-domain, e.g., the pair {0,0}.
  //    see [NOTE identity element] link.
  using CodeMap = icl::interval_map<uint64_t, std::shared_ptr<FunctionInfo>,
                                    icl::partial_enricher>;

  // map FROM object filename TO code-section vector.
  std::map<std::string, uint64_t> ObjFiles;

  // interval map FROM this process's virtual-memory code addresses
  //              TO <an index of the code-section vector>.
  icl::interval_map<uint64_t, size_t, icl::partial_enricher> VMAResolver;

  // the code-section vector, which is paired with the offset to apply
  // to the raw IP to index into it.
  std::vector<std::pair<CodeMap, uint64_t>> Data;

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
  void recordDataN(IDType, DataKind, uint64_t, uint64_t*);

};


} // end halo namespace
