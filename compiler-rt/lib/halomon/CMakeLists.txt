add_compiler_rt_component(halomon)

#####################
# find dependencies

include(FindLibpfm)
if(HAVE_LIBPFM)
  set(LIBPFM pfm)
else()
  message(FATAL_ERROR "libpfm is required but not found for halomon!")
endif()

find_package(Boost 1.65 COMPONENTS system REQUIRED)
find_package(Protobuf 3 REQUIRED)

include_directories(.. ${Boost_INCLUDE_DIR} ${Protobuf_INCLUDE_DIR} ${LLVM_INCLUDE_DIRS})

set(MAX_ERRORS "1")

if ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "Clang")
  set(HALOMON_CFLAGS "-ferror-limit=${MAX_ERRORS}")
elseif ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
  set(HALOMON_CFLAGS "-fmax-errors=${MAX_ERRORS}")
else()
  set(HALOMON_CFLAGS)
endif()

# include LLVM itself
add_definitions(${LLVM_DEFINITIONS})
llvm_map_components_to_libnames(LLVM_LIBS all) # TODO: make this list shorter?

if (NOT HALO_NET_DIR)
  message( FATAL_ERROR "Please set HALO_NET_DIR to a directory containing networking files." )
endif()

# find protobuf files and generate C++ code for them.
file(GLOB PROTO_FILES ${HALO_NET_DIR}/*.proto)
protobuf_generate_cpp(PROTO_SRCS PROTO_HDRS ${PROTO_FILES})

set(HALOMON_HEADERS
      Error.h
      MonitorState.h
      Profiler.h
    )

set(HALOMON_SOURCES
      Error.cpp
      LinuxPerfEvents.cpp
      Monitor.cpp
      Profiler.cpp
    )

add_compiler_rt_runtime(clang_rt.halomon
  SHARED
  ARCHS ${HALOMON_SUPPORTED_ARCH}
  CFLAGS ${HALOMON_CFLAGS} ${SANITIZER_COMMON_CFLAGS}
  SOURCES ${HALOMON_SOURCES} ${PROTO_SRCS}
  ADDITIONAL_HEADERS ${HALOMON_HEADERS} ${PROTO_HDRS}
  LINK_LIBS ${LLVM_LIBS} ${Protobuf_LIBRARIES} ${Boost_LIBRARIES} ${LIBPFM}
  OBJECT_LIBS RTSanitizerCommon
              RTSanitizerCommonLibc
              RTSanitizerCommonSymbolizer
  PARENT_TARGET halomon)
