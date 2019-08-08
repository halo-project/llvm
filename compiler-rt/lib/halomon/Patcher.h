#pragma once

#include "xray/xray_interface_internal.h"

namespace halo {

class Patcher {
public:
  Patcher() {
    __xray_init();
  }

private:

};

} // end namespace
