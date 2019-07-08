#pragma once

#include <cinttypes>
#include <memory>
#include <vector>

#include "llvm/ADT/Optional.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Host.h" // for getProcessTriple

// Function interface reference:
// https://www.boost.org/doc/libs/1_65_0/libs/icl/doc/html/boost_icl/interface/function_synopsis.html

// [NOTE identity element]
// https://www.boost.org/doc/libs/1_65_0/libs/icl/doc/html/boost_icl/concepts/map_traits.html#boost_icl.concepts.map_traits.definedness_and_storage_of_identity_elements

#define BOOST_ICL_USE_STATIC_BOUNDED_INTERVALS
#include "boost/icl/interval_map.hpp"

namespace icl = boost::icl;

namespace halo {

struct BranchInfo {
  uint64_t From; // This indicates the source instruction (may not be a branch).
  uint64_t To;   // The branch target.

  // These fields may all be false, indicating that we don't have any info.
  bool Mispred;     // The branch target was mispredicted.
  bool Predicted;   // The branch target was predicted.

  BranchInfo(uint64_t from, uint64_t to, bool mispred, bool predicted) :
             From(from), To(to), Mispred(mispred), Predicted(predicted) {}
};

struct RawSample {
  uint64_t IP;
  uint32_t TID;
  uint64_t Time;
  std::vector<uint64_t> CallStack; // order is from latest -> oldest call. vals are IPs.
  std::vector<BranchInfo> LastBranch;
};


// Ideally this would also contain information about blocks in the function.
struct FunctionInfo {
  std::string Name;
  uint64_t VMStart;
  uint64_t Size;

  FunctionInfo(llvm::StringRef label, uint64_t vm_start, uint64_t size)
              : Name(label.data()), VMStart(vm_start), Size(size) {}
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
public:

  RawSample& newSample() {
    RawSamples.emplace_back();
    return RawSamples.back();
  }

  void processSamples();

  Profiler(std::string SelfBinPath)
             : ProcessTriple(llvm::sys::getProcessTriple()),
               HostCPUName(llvm::sys::getHostCPUName()) {
    CRI.loadObjFile(SelfBinPath);
  }

  ~Profiler() {}

private:

  std::string ProcessTriple;
  std::string HostCPUName;

  std::vector<RawSample> RawSamples;

  CodeRegionInfo CRI;

};


} // end halo namespace
