#pragma once

#include <string>
#include "llvm/Support/Error.h"
#include "llvm/Support/raw_ostream.h"

namespace halo {
  // LOG controls whether or not we completely silence output or not.
  constexpr bool LOG = false;

  inline llvm::raw_ostream& log() {
    // This function exists b/c in the future we'd like to log to a file instead.
    return LOG ? llvm::errs() : llvm::nulls();
  }

  void fatal_error(const std::string &msg);
}
