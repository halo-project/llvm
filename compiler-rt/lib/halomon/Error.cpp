
#include "halomon/Error.h"
#include <cstdlib>
#include <iostream>

namespace boost {

void throw_exception(std::exception const& ex) {
  if (halo::LOG) halo::log << "uncaught exception: " << ex.what() << "\n";
  std::exit(EXIT_FAILURE);
}

}

namespace halo {
  std::ostream log(std::cerr.rdbuf()); // TODO: have an option to log to file.

  void fatal_error(const std::string &msg) {
    if (LOG) log << "(halo) fatal error: " << msg << "\n";
    std::exit(EXIT_FAILURE);
  }
}
