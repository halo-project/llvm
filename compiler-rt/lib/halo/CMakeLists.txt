
#####################
# find dependencies

# find_library(PFM_LIB pfm)  # http://perfmon2.sourceforge.net/
#
# find_package(Boost 1.65 COMPONENTS system REQUIRED)
# include_directories(${Boost_INCLUDE_DIR})

# include the rest of the compiler-rt header dirs
include_directories(..)

add_compiler_rt_component(halo)

set(HALO_HEADERS
      Error.h
      MonitorState.h
      Profiler.h
    )

set(HALO_SOURCES
      Error.cpp
      LinuxPerfEvents.cpp
      Monitor.cpp
      Profiler.cpp
    )

add_compiler_rt_runtime(clang_rt.halo
  SHARED
  ARCHS ${HALO_SUPPORTED_ARCH}
  CFLAGS ${EXTRA_FLAGS}
  SOURCES ${HALO_SOURCES}
  ADDITIONAL_HEADERS ${HALO_HEADERS}
  PARENT_TARGET halo)

# setup to use LLVM
# include_directories(${LLVM_INCLUDE_DIRS})
# add_definitions(${LLVM_DEFINITIONS})
# llvm_map_components_to_libnames(LLVM_LIBS all)


# #####################
# # create the halomon shared library
# add_library(halomon SHARED
#             Monitor.cpp
#             Error.cpp
#             Profiler.cpp
#             LinuxPerfEvents.cpp
#            )
#
# #####################
# # link in dependencies
# target_link_libraries(halomon PRIVATE ${LLVM_LIBS} ${PFM_LIB} ${Boost_LIBRARIES})
#
# #####################
# # install it
# install(TARGETS halomon DESTINATION lib)
