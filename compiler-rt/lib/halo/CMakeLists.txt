add_compiler_rt_component(halo)

#####################
# find dependencies

include(FindLibpfm)
if(HAVE_LIBPFM)
  set(LIBPFM pfm)
else()
  message(FATAL_ERROR "libpfm is required but not found for Halo!")
endif()

find_package(Boost 1.65 COMPONENTS system REQUIRED)

include_directories(.. ${Boost_INCLUDE_DIR} ${LLVM_INCLUDE_DIRS})

# include LLVM itself
add_definitions(${LLVM_DEFINITIONS})
llvm_map_components_to_libnames(LLVM_LIBS all) # TODO: make this list shorter?

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
  CFLAGS ${SANITIZER_COMMON_CFLAGS}
  SOURCES ${HALO_SOURCES}
  ADDITIONAL_HEADERS ${HALO_HEADERS}
  LINK_LIBS ${LLVM_LIBS} ${Boost_LIBRARIES} ${LIBPFM}
  OBJECT_LIBS RTSanitizerCommon
              RTSanitizerCommonLibc
              RTSanitizerCommonSymbolizer
  PARENT_TARGET halo)
