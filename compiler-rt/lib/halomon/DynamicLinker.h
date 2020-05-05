#pragma once

#include "llvm/ExecutionEngine/JITSymbol.h"
#include "llvm/ExecutionEngine/Orc/Core.h"
#include "llvm/ExecutionEngine/Orc/RTDyldObjectLinkingLayer.h"
#include "llvm/IR/DataLayout.h"

namespace orc = llvm::orc;

namespace halo {


struct DySymbol {
  llvm::JITEvaluatedSymbol Symbol;
  // TODO: provide symbol size information from the unresolved object file.
};

class DyLib {

  struct RefCountedSymbol {
    DySymbol Value;
    int32_t Uses;
  };

public:
  DyLib (llvm::DataLayout DataLayout, std::unique_ptr<std::string> ObjFile);

    // Obtains the JITEvaluated symbol for this mangled symbol name.
    // Each call to this method increases the reference count of this
    // symbol in the dylib. use dropSymbol to release uses of the symbol.
    llvm::Expected<DySymbol> requireSymbol(llvm::StringRef MangledName);

    size_t numRequiredSymbols() const;

    // returns true if this symbol was contained in the dylib and a use was dropped.
    bool dropSymbol(llvm::StringRef Value);

    // returns true if this symbol was contained in the dylib and a use was dropped.
    bool dropSymbol(uint64_t Addr);

    void dump(llvm::raw_ostream &OS);

private:
  bool haveSymbol(llvm::StringRef MangledName) const;

  bool haveSymbol(uint64_t Address) const;

  llvm::Optional<RefCountedSymbol*> findByAddr(uint64_t Addr);

  llvm::Optional<RefCountedSymbol const*> findByAddr(uint64_t Addr) const;

  llvm::DataLayout DL;
  orc::ExecutionSession ES;
  orc::MangleAndInterner Mangle; // this will be needed for linking C++ symbols later.
  orc::RTDyldObjectLinkingLayer ObjectLayer;
  std::unique_ptr<std::string> RawObjFile; // with unresolved symbols.
  // could delete RawObjFile once all symbols we need have been looked up,
  // since the memory for the linked code is kept inside the ExecutionSession,
  // i.e., the ES only has read-only access to the RawObjFile.
  llvm::StringMap<RefCountedSymbol> RequiredSymbols;
  orc::JITDylib &MainJD;
};

class DynamicLinker {
private:
  llvm::DataLayout Layout;
  bool Valid; // prevents linking when only default-constructed.

public:
  DynamicLinker(llvm::DataLayout Layout) : Layout(Layout), Valid(true) {}
  DynamicLinker() : Layout(""), Valid(false) {}

  void setLayout(llvm::DataLayout DL) {
    Layout = DL;
    Valid = true;
  }

  // NOTE: this is a quite expensive setter.
  void setLayout(std::string const& Bitcode);

  llvm::Expected<std::unique_ptr<DyLib>> run(std::unique_ptr<std::string> ObjFile) const;
};

}
