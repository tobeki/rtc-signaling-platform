# ==============================================================================
# ChatServer Linux unit tests.
#
# Phase 2, Step 2.5 scope: unit-test infrastructure only.
#
# This subdirectory is included only when BUILD_TESTING is ON (see the parent
# CMakeLists.txt), so GoogleTest is a developer/test-time dependency and never
# a requirement for someone who only wants to build ChatServer.
#
# Deliberate boundary (Step 2.5 §14): this target links GoogleTest and nothing
# else from the application's runtime dependency graph. It does NOT link gRPC,
# protobuf, hiredis, MySQL Connector/C++, JsonCpp or Boost.Filesystem, and it
# does NOT link the ChatServer executable. Tests here must stay
# process-local, network-free, Redis-free, MySQL-free and gRPC-free. Future
# pure Meeting-domain tests extend this target; adapter/integration tests that
# genuinely need the runtime graph belong in a separate, explicitly named
# target.
# ==============================================================================

# GoogleTest from the Ubuntu package (libgtest-dev). Ubuntu ships
# GTestConfig.cmake exporting GTest::gtest / GTest::gtest_main, so config-mode
# discovery finds the real imported targets. No vendoring, no submodule, no
# FetchContent, no manual build.
find_package(GTest REQUIRED)

add_executable(ChatServerUnitTests
    TestInfrastructureSmoke.cpp
)

target_compile_features(ChatServerUnitTests PRIVATE cxx_std_14)

# Test-only include path. Reaching ".." makes the production header root
# visible so a test can do #include "const.h" without installing headers
# globally or reorganizing the source tree.
target_include_directories(ChatServerUnitTests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/.."
)

# GTest::gtest_main supplies main() and pulls GTest::gtest plus Threads::Threads
# transitively. Threads is not listed separately on purpose: it arrives through
# the GoogleTest package, not through the ChatServer runtime graph.
target_link_libraries(ChatServerUnitTests PRIVATE
    GTest::gtest_main
)

# Register each TEST()/TEST_F() case with CTest automatically. This keeps CTest
# names derived from the GoogleTest names instead of a hand-maintained,
# easily-drifting duplicate list.
include(GoogleTest)
gtest_discover_tests(ChatServerUnitTests)
