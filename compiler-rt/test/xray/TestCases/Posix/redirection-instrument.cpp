// Simple single-threaded test of the interaction between instrumentation and
// redirection.
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

[[clang::xray_always_instrument]] NO_INLINE void original() {
  printf("ORIGINAL.\n");
}

[[clang::xray_always_instrument]] NO_INLINE void somethingElse() {
  printf("SOMETHING ELSE.\n");
}

void different() {
  printf("DIFFERENT.\n");
}

void myhandler(int32_t funcID, XRayEntryType kind) {
  if (kind == ENTRY) {
    printf("entered function.\n");
  } else if (kind == EXIT) {
    printf("exited function.\n");
  } else {
    printf("unexpected entry type!\n");
  }
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

  // setup
  table[id] = (uintptr_t) &different;
  __xray_set_handler(myhandler);

  /////////////
  // test function-specific redirection and patching/unpatching.

  __xray_patch_function(id);
  original();
  // CHECK:      entered function.
  // CHECK-NEXT: ORIGINAL.
  // CHECK-NEXT: exited function.

  // enable redirection.
  __xray_redirect_function(id);
  original();
  original();
  // CHECK-NEXT: DIFFERENT.
  // CHECK-NEXT: DIFFERENT.

  // undo the redirect via table write.
  // this ensures that enabling redirection unpatches exit sleds in that function.
  table[id] = 0;
  original();
  original();
  // CHECK-NEXT: ORIGINAL.
  // CHECK-NEXT: ORIGINAL.


  ///////
  // test function specific patching, but global patching / unpatching.

  // redo redirection via table write.
  table[id] = (uintptr_t) &different;
  original();
  // CHECK-NEXT: DIFFERENT.
  somethingElse();
  // CHECK-NEXT: SOMETHING ELSE.

  // patch over top everything.
  __xray_patch();
  original();
  // CHECK-NEXT: entered function.
  // CHECK-NEXT: ORIGINAL.
  // CHECK-NEXT: exited function.
  somethingElse();
  // CHECK-NEXT: entered function.
  // CHECK-NEXT: SOMETHING ELSE.
  // CHECK-NEXT: exited function.

  // redirect only `original`
  __xray_redirect_function(id);
  original();
  // CHECK-NEXT: DIFFERENT.
  somethingElse();
  // CHECK-NEXT: entered function.
  // CHECK-NEXT: SOMETHING ELSE.
  // CHECK-NEXT: exited function.

  // unpatch everything.
  __xray_unpatch();
  original();
  // CHECK-NEXT: ORIGINAL.
  somethingElse();
  // CHECK-NEXT: SOMETHING ELSE.

  return 0;
}
