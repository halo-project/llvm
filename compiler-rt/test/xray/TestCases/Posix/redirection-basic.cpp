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

[[clang::xray_always_instrument]] NO_INLINE int original(int x, int y, float z) {
  printf("ORIG -- %d, %d, %f\n", x, y, z);
  return x;
}

int bar(int x, int y, float z) {
  printf("BAR -- %d, %d, %f\n", x, y, z);
  return y;
}

int buzz(int x, int y, float z) {
  printf("BUZZ -- %d, %d, %f\n", x, y, z);
  return x + y;
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

  int res = original(2, 3, 4);
  // CHECK: ORIG -- 2, 3, 4.000000
  if (res != 2)
    printf("bad return value\n");

  // perform the redirect
  __xray_redirect_function(id);

  res = original(2, 3, 4);
  // CHECK-NEXT: BAR -- 2, 3, 4.000000
  if (res != 3)
    printf("bad return value\n");

  // disable redirection with just a write to the table.
  table[id] = 0;
  res = original(2, 3, 4);
  // CHECK-NEXT: ORIG -- 2, 3, 4.000000
  if (res != 2)
    printf("bad return value\n");

  // change redirection with just a write to the table.
  table[id] = (uintptr_t) &buzz;
  res = original(2, 3, 4);
  // CHECK-NEXT: BUZZ -- 2, 3, 4.000000
  if (res != 5)
    printf("bad return value\n");

  // undo patching entirely
  __xray_unpatch_function(id);

  res = original(2, 3, 4);
  // CHECK-NEXT: ORIG -- 2, 3, 4.000000
  if (res != 2)
    printf("bad return value\n");

  return 0;
}
