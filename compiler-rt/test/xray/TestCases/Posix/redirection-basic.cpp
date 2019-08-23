// Simple single-threaded test of function redirection in XRay.
//
// RUN: %clangxx_xray -std=c++11 %s -o %t
// RUN: XRAY_OPTIONS="patch_premain=false verbosity=1" %run %t 2>&1 | FileCheck %s
// RUN: %clangxx_xray -std=c++11 -fpic -fpie %s -o %t
// RUN: XRAY_OPTIONS="patch_premain=false verbosity=1" %run %t 2>&1 | FileCheck %s
// FIXME: Support this in non-x86_64 as well
// REQUIRES: x86_64-linux
// REQUIRES: built-in-llvm-tree

#include <cstdio>
#include <cstdlib>
#include "xray/xray_interface.h"

// if `original` is inlined, xray will not be used.
#define NO_INLINE __attribute__((noinline))

[[clang::xray_always_instrument]] NO_INLINE void original(int x, int y, float z) {
  printf("ORIG -- %d, %d, %f\n", x, y, z);
}

void bar(int x, int y, float z) {
  printf("BAR -- %d, %d, %f\n", x, y, z);
}

void buzz(int x, int y, float z) {
  printf("BUZZ -- %d, %d, %f\n", x, y, z);
}

int main() {
  size_t maxID = __xray_max_function_id();
  if (maxID == 0)
    return 1;

  uintptr_t* table = (uintptr_t*) std::calloc(maxID, sizeof(uintptr_t));
  __xray_set_redirection_table(table);

  // find the entry corresponding ID for the original function
  size_t id = 0;
  for (; id < maxID; id++)
    if (__xray_function_address(id) == (uintptr_t) &original)
      break;

  if (id == maxID)
    return 2;

  // set the entry before redirection
  table[id] = (uintptr_t) &bar;

  original(1, 2, 3);
  // CHECK: ORIG -- 1, 2, 3.000000

  // perform the redirect
  if (__xray_redirect_function(id) != SUCCESS)
    return 3;

  original(4, 5, 6);
  // CHECK-NEXT: BAR -- 4, 5, 6.000000

  // change redirection with just a write to the table.
  table[id] = (uintptr_t) &buzz;

  original(7, 8, 9);
  // CHECK-NEXT: BUZZ -- 7, 8, 9.000000

  // undo patching
  __xray_unpatch_function(id);

  original(10, 11, 12);
  // CHECK-NEXT: ORIG -- 10, 11, 12.000000

  return 0;
}
