
#include "halomon/Logging.h"
#include <cstdlib>

namespace boost {

void throw_exception(std::exception const& ex) {
  if (halo::LOG) halo::log() << "uncaught exception: " << ex.what() << "\n";
  std::exit(EXIT_FAILURE);
}

}

namespace halo {
  void fatal_error(const std::string &msg) {
    llvm::report_fatal_error(llvm::createStringError(
                                    std::errc::operation_not_supported, msg.c_str()));
  }
}
