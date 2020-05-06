#pragma once

#include "llvm/ExecutionEngine/JITEventListener.h"
#include "llvm/ExecutionEngine/JITSymbol.h"
#include "llvm/ExecutionEngine/Orc/Core.h"
#include "llvm/ExecutionEngine/Orc/RTDyldObjectLinkingLayer.h"
#include "llvm/IR/DataLayout.h"

namespace halo {

class DyLib;

/// Representation of a symbol from a loaded, dynamically-linked library.
class DySymbol {
  friend DyLib; // The DyLib is exclusively in charge of managing reference-counts.
                // Users of a DySymbol should inform the corresponding DyLib of dropped uses.

public:
  void setSize(uint64_t Sz) { SymbolSize = Sz; }
  void setSymbol(llvm::JITEvaluatedSymbol Symb) { Symbol = Symb; }

  /// @returns the absolute address of this symbol in memory within this process.
  llvm::JITTargetAddress getAddress() const { return Symbol.getAddress(); }

  /// @returns the size (in bytes) of the data corresponding to this symbol
  uint64_t getSize() const { return SymbolSize; }

  /// @returns additional information about this symbol, such as whether it is callable code.
  llvm::JITSymbolFlags getFlags() const { return Symbol.getFlags(); }

private:
    // reference-counting: add an active use
    void retain() { Uses++; }
    // reference-counting: remove an active use (if one exists)
    void release() { Uses = (Uses == 0 ? 0 : Uses-1); }
    // returns the use-count of this symbol for garbage-collectors.
    uint32_t useCount() const { return Uses; };

    llvm::JITEvaluatedSymbol Symbol;
    uint64_t SymbolSize = 0;
    uint32_t Uses = 0;
};


/// Representation of a dynamically-linked library.
class DyLib {
public:
  DyLib (llvm::DataLayout DataLayout, std::unique_ptr<std::string> ObjFile);

    // Obtains the DySymbol for this mangled symbol name.
    // Each call to this method increases the reference count of the returned
    // symbol in the dylib. Use dropSymbol to release uses of the symbol when finished.
    llvm::Expected<DySymbol> requireSymbol(llvm::StringRef MangledName);

    // queries this library for the number of active symbols it contains.
    size_t numRequiredSymbols() const;

    // returns true if this symbol was contained in the dylib and a use was dropped.
    bool dropSymbol(llvm::StringRef Value);
    // returns true if this symbol was contained in the dylib and a use was dropped.
    bool dropSymbol(uint64_t Addr);
    // returns true if this symbol was contained in the dylib and a use was dropped.
    bool dropSymbol(DySymbol &Sym) { return dropSymbol(Sym.getAddress()); }

    void dump(llvm::raw_ostream &OS);

private:

  /// extracts information from the object file after dynamic linking happens, to aid profiling.
  class LinkingEventListener : public llvm::JITEventListener {
  public:
    void notifyObjectLoaded(ObjectKey K, const llvm::object::ObjectFile &Obj,
                              const llvm::RuntimeDyld::LoadedObjectInfo &L) override;

    /// @returns the size of the  given symbol, if a size is known.
    llvm::Optional<uint64_t> getSize(llvm::StringRef MangledName) const;

    void dump(llvm::raw_ostream &OS);
  private:
    // for now, we only need size info.
    llvm::StringMap<uint64_t> SymbolInfo;
  };

  bool haveSymbol(llvm::StringRef MangledName) const;

  bool haveSymbol(uint64_t Address) const;

  llvm::Optional<DySymbol*> findByAddr(uint64_t Addr);

  llvm::Optional<DySymbol const*> findByAddr(uint64_t Addr) const;

  llvm::DataLayout DL;
  llvm::orc::ExecutionSession ES;
  llvm::orc::MangleAndInterner Mangle; // this will be needed for linking C++ symbols later.
  llvm::orc::RTDyldObjectLinkingLayer ObjectLayer;
  std::unique_ptr<std::string> RawObjFile; // with unresolved symbols.
  // could delete RawObjFile once all symbols we need have been looked up,
  // since the memory for the linked code is kept inside the ExecutionSession,
  // i.e., the ES only has read-only access to the RawObjFile.
  llvm::StringMap<DySymbol> RequiredSymbols;
  llvm::orc::JITDylib &MainJD;
  LinkingEventListener LinkEvtListener;
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

  /// NOTE: this is a quite expensive setter.
  void setLayout(std::string const& Bitcode);

  llvm::Expected<std::unique_ptr<DyLib>> run(std::unique_ptr<std::string> ObjFile) const;
};

}
