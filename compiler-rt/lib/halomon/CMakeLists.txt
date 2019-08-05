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

set(MAX_ERRORS "1")

if ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "Clang")
  set(HALOMON_CFLAGS "-ferror-limit=${MAX_ERRORS}")
elseif ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
  set(HALOMON_CFLAGS "-fmax-errors=${MAX_ERRORS}")
else()
  set(HALOMON_CFLAGS)
endif()

append_rtti_flag(OFF HALOMON_CFLAGS)
list(APPEND HALOMON_CFLAGS -fno-exceptions)
list(APPEND HALOMON_CFLAGS -DGOOGLE_PROTOBUF_NO_RTTI)
list(APPEND HALOMON_CFLAGS -DBOOST_EXCEPTION_DISABLE)
list(APPEND HALOMON_CFLAGS -DBOOST_NO_RTTI)

if (NOT HALO_NET_DIR)
  message( FATAL_ERROR "Please set HALO_NET_DIR to a directory containing networking files." )
endif()

# find protobuf files and generate C++ code for them.
file(GLOB PROTO_FILES ${HALO_NET_DIR}/*.proto)
protobuf_generate_cpp(PROTO_SRCS PROTO_HDRS ${PROTO_FILES})

# enumerate files in our local dir
file(GLOB HALOMON_HEADERS ${CMAKE_CURRENT_SOURCE_DIR}/*.h)
file(GLOB HALOMON_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)

include_directories(.. ${HALO_NET_DIR} ${Boost_INCLUDE_DIR} ${Protobuf_INCLUDE_DIR} ${LLVM_INCLUDE_DIRS})

add_dependencies(halomon install-LLVM)

add_compiler_rt_runtime(clang_rt.halomon
  STATIC
  ARCHS ${HALOMON_SUPPORTED_ARCH}
  SOURCES ${HALOMON_SOURCES} ${PROTO_SRCS}
  ADDITIONAL_HEADERS ${HALOMON_HEADERS} ${PROTO_HDRS}
  CFLAGS ${SANITIZER_COMMON_CFLAGS} ${LLVM_DEFINITIONS} ${HALOMON_CFLAGS}
  LINK_FLAGS ${SANITIZER_COMMON_LINK_FLAGS}
  OBJECT_LIBS RTSanitizerCommon
              RTSanitizerCommonLibc
              RTSanitizerCommonSymbolizer
              RTXray
  PARENT_TARGET halomon)
