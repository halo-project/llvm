#pragma once

#include <string>
#include <ostream>

namespace halo {
  constexpr bool LOG = false;
  extern std::ostream log;

  void fatal_error(const std::string &msg);
}
