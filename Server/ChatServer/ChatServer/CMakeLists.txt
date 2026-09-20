# ==============================================================================
# Linux-only minimal build entry for ChatServer.
#
# Phase 2, Step 2.3 scope:
#   CMake configure + protobuf/gRPC generation + compile + link.
#
# Explicitly OUT of scope here:
#   runtime deployment, service configuration, CI, Docker, Meeting features.
#
# Unit-test infrastructure (Phase 2, Step 2.5) is optional and lives under
# tests/. It is enabled only when BUILD_TESTING is ON, so the application-only
# build path never requires GoogleTest.
#
# This file is deliberately NOT referenced by ChatServer.vcxproj, so the proven
# Windows + Visual Studio 2022 + Debug|x64 baseline is unaffected. The Windows
# build keeps using its own generated protobuf/gRPC sources and its own
# Connector/C++ include layout.
#
# Out-of-repository build directory is required, for example:
#   cmake -S Server/ChatServer/ChatServer \
#         -B /tmp/rtc-signaling-chatserver-build \
#         -DCMAKE_BUILD_TYPE=Debug
#   cmake --build /tmp/rtc-signaling-chatserver-build --parallel 2
# ==============================================================================

cmake_minimum_required(VERSION 3.16)

project(ChatServer LANGUAGES CXX)

# Mirror the Visual Studio project's ConformanceMode=true: no GNU extensions.
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ------------------------------------------------------------------------------
# Application sources.
#
# Derived from the authoritative Server/ChatServer/ChatServer/ChatServer.vcxproj
# <ClCompile> item list. No recursive globbing: the list is explicit so that a
# newly added file is a visible change in this file.
#
# The .vcxproj additionally lists message.pb.cc / message.grpc.pb.cc. Those are
# NOT listed here: repository copies of Windows-generated protobuf/gRPC output
# must not be reused on Linux. Linux sources are regenerated into the build tree
# from message.proto below.
# ------------------------------------------------------------------------------
set(CHATSERVER_APPLICATION_SOURCES
    AsioIOServicePool.cpp
    ChatGrpcClient.cpp
    ChatServer.cpp
    ChatServiceImpl.cpp
    ConfigMgr.cpp
    CServer.cpp
    CSession.cpp
    LogicSystem.cpp
    MsgNode.cpp
    MysqlDao.cpp
    MysqlMgr.cpp
    RedisMgr.cpp
    UserMgr.cpp
)

# ------------------------------------------------------------------------------
# Protobuf / gRPC code generation (Gate G3).
#
# message.proto is the single source of truth. Generated files go into the build
# tree only; they are never written into the repository and never committed.
# The custom commands below give CMake the dependency edges needed to regenerate
# whenever message.proto changes.
# ------------------------------------------------------------------------------
set(CHATSERVER_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(CHATSERVER_PROTO "${CMAKE_CURRENT_SOURCE_DIR}/message.proto")

set(CHATSERVER_PROTOBUF_SOURCE  "${CHATSERVER_GENERATED_DIR}/message.pb.cc")
set(CHATSERVER_PROTOBUF_HEADER  "${CHATSERVER_GENERATED_DIR}/message.pb.h")
set(CHATSERVER_GRPC_SOURCE      "${CHATSERVER_GENERATED_DIR}/message.grpc.pb.cc")
set(CHATSERVER_GRPC_HEADER      "${CHATSERVER_GENERATED_DIR}/message.grpc.pb.h")

file(MAKE_DIRECTORY "${CHATSERVER_GENERATED_DIR}")

# ---------------------------------------------------------------- dependencies
find_package(Threads REQUIRED)

# Boost.Random is a real compiled dependency: CSession.cpp calls
# boost::uuids::random_generator(), which uses Boost.Random in Boost 1.83.
find_package(Boost REQUIRED COMPONENTS filesystem random)

find_package(Protobuf REQUIRED)
find_package(PkgConfig REQUIRED)

# gRPC: Ubuntu ships a usable CMake package config which exposes the real
# imported targets (gRPC::grpc++, gRPC::grpc, gRPC::gpr). This keeps the large
# transitive closure (Abseil, re2, c-ares, BoringSSL, zlib) package-managed
# instead of hand-listing Windows .lib names.
# ---------------------------------------------------------------------------
# re2 import target (gRPC transitive dependency).
#
# gRPC's package config calls find_package(re2) itself and then unconditionally
# links the exported target re2::re2. Ubuntu's libre2-dev is a real system
# package but ships no CMake package config, so that lookup falls through to
# whatever re2 config the ambient search path happens to expose. On this WSL
# host the inherited Windows PATH exposes a conda installation, and conda's re2
# config exports the include root /mnt/f/conda/Library/include. That directory
# contains a newer protobuf header tree, which then shadows the Ubuntu protobuf
# headers on the compile line and breaks the generated protobuf code.
#
# Supplying re2::re2 from the Ubuntu package keeps the dependency
# package-managed and keeps the foreign include root out of the compile line.
# CMake's generated export files return early when the target they would define
# already exists, so this definition wins over any later foreign config.
# ---------------------------------------------------------------------------
if(NOT TARGET re2::re2)
    find_path(CHATSERVER_RE2_INCLUDE_DIR NAMES re2/re2.h)
    find_library(CHATSERVER_RE2_LIBRARY NAMES re2)
    if(NOT CHATSERVER_RE2_INCLUDE_DIR OR NOT CHATSERVER_RE2_LIBRARY)
        message(FATAL_ERROR "re2 not found (install libre2-dev).")
    endif()
    add_library(re2::re2 UNKNOWN IMPORTED)
    set_target_properties(re2::re2 PROPERTIES
        IMPORTED_LOCATION "${CHATSERVER_RE2_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${CHATSERVER_RE2_INCLUDE_DIR}"
    )
endif()

find_package(gRPC CONFIG REQUIRED)

# JsonCpp: the exported JsonCpp::JsonCpp target carries the include root
# /usr/include/jsoncpp, which is required for the source's #include <json/json.h>.
find_package(jsoncpp CONFIG REQUIRED)

# hiredis: Ubuntu's HiredisConfig.cmake exports no imported target and points its
# include variable at /usr/include, while the source uses #include "hiredis.h"
# which lives in /usr/include/hiredis. The pkg-config module is therefore the
# correct integration, and it supplies -I/usr/include/hiredis.
pkg_check_modules(HIREDIS REQUIRED IMPORTED_TARGET hiredis)

if(NOT Protobuf_PROTOC_EXECUTABLE)
    message(FATAL_ERROR "protoc was not found (install protobuf-compiler).")
endif()

find_program(CHATSERVER_GRPC_CPP_PLUGIN grpc_cpp_plugin REQUIRED
    DOC "gRPC C++ code generator plugin (package protobuf-compiler-grpc)")

# MySQL Connector/C++ 1.1.x from Ubuntu (libmysqlcppconn-dev).
find_path(CHATSERVER_MYSQLCPPCONN_INCLUDE_DIR
    NAMES mysql_driver.h
    DOC "MySQL Connector/C++ 1.1.x include directory")
find_library(CHATSERVER_MYSQLCPPCONN_LIBRARY
    NAMES mysqlcppconn
    DOC "MySQL Connector/C++ 1.1.x library")
if(NOT CHATSERVER_MYSQLCPPCONN_INCLUDE_DIR OR NOT CHATSERVER_MYSQLCPPCONN_LIBRARY)
    message(FATAL_ERROR
        "MySQL Connector/C++ 1.1.x not found (install libmysqlcppconn-dev).")
endif()

# ------------------------------------------------------------------- generation
add_custom_command(
    OUTPUT "${CHATSERVER_PROTOBUF_SOURCE}" "${CHATSERVER_PROTOBUF_HEADER}"
    COMMAND "${Protobuf_PROTOC_EXECUTABLE}"
            "--cpp_out=${CHATSERVER_GENERATED_DIR}"
            "-I" "${CMAKE_CURRENT_SOURCE_DIR}"
            "${CHATSERVER_PROTO}"
    DEPENDS "${CHATSERVER_PROTO}"
    COMMENT "[ChatServer] protoc --cpp_out (message.pb.cc/.h)"
    VERBATIM
)

add_custom_command(
    OUTPUT "${CHATSERVER_GRPC_SOURCE}" "${CHATSERVER_GRPC_HEADER}"
    COMMAND "${Protobuf_PROTOC_EXECUTABLE}"
            "--grpc_out=${CHATSERVER_GENERATED_DIR}"
            "--plugin=protoc-gen-grpc=${CHATSERVER_GRPC_CPP_PLUGIN}"
            "-I" "${CMAKE_CURRENT_SOURCE_DIR}"
            "${CHATSERVER_PROTO}"
    DEPENDS "${CHATSERVER_PROTO}" "${CHATSERVER_GRPC_CPP_PLUGIN}"
    COMMENT "[ChatServer] protoc --grpc_out (message.grpc.pb.cc/.h)"
    VERBATIM
)

add_custom_target(ChatServerGeneratedProto
    DEPENDS
        "${CHATSERVER_PROTOBUF_SOURCE}"
        "${CHATSERVER_PROTOBUF_HEADER}"
        "${CHATSERVER_GRPC_SOURCE}"
        "${CHATSERVER_GRPC_HEADER}"
)

# ----------------------------------------------------------------------- target
add_executable(ChatServer
    ${CHATSERVER_APPLICATION_SOURCES}
    "${CHATSERVER_PROTOBUF_SOURCE}"
    "${CHATSERVER_GRPC_SOURCE}"
)

# Ordering edge: generated headers must exist before any application source is
# compiled, including sources that only pull them in through a header
# (ChatGrpcClient.h / ChatServiceImpl.h include message.grpc.pb.h).
add_dependencies(ChatServer ChatServerGeneratedProto)

target_compile_features(ChatServer PRIVATE cxx_std_14)

target_include_directories(ChatServer BEFORE PRIVATE
    # MySQL compatibility root: supplies <jdbc/...> forwarding headers so the
    # unchanged application source resolves the Windows Connector/C++ layout.
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/compat/mysqlcppconn"
)

target_include_directories(ChatServer PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"          # local project headers
    "${CHATSERVER_GENERATED_DIR}"          # message.pb.h / message.grpc.pb.h
    "${CHATSERVER_MYSQLCPPCONN_INCLUDE_DIR}"
)

target_link_libraries(ChatServer PRIVATE
    Threads::Threads
    Boost::filesystem
    Boost::random
    JsonCpp::JsonCpp
    PkgConfig::HIREDIS
    protobuf::libprotobuf
    gRPC::grpc++
    "${CHATSERVER_MYSQLCPPCONN_LIBRARY}"
)

# ------------------------------------------------------------------------------
# Unit tests (Phase 2, Step 2.5).
#
# include(CTest) defines the standard BUILD_TESTING option (default ON) and
# provides the "test" target driver for CTest.
#
# Everything test-related, including GoogleTest discovery, lives inside the
# guard. A build-only configuration therefore stays valid without GoogleTest
# installed:
#
#   cmake -S . -B <dir> -DBUILD_TESTING=OFF     # no GTest required
#   cmake -S . -B <dir> -DBUILD_TESTING=ON      # tests + ctest
#
# No project-specific toggle such as CHATSERVER_ENABLE_TESTS is introduced: the
# standard option is sufficient and keeps the invocation conventional.
# ------------------------------------------------------------------------------
include(CTest)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
