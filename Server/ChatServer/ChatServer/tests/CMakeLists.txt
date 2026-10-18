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
    MeetingTypesContractTest.cpp
    MeetingAggregateTest.cpp
    MeetingServiceTest.cpp
    LogicWorkQueueTest.cpp
    # M5: the transport disconnect bridge. SessionDisconnectBridge.h is
    # header-only on purpose, so this test file needs no extra translation unit
    # and the Windows .vcxproj source list gains nothing either.
    SessionDisconnectBridgeTest.cpp
    # M5.1: domain session lifecycle (accept -> CONNECTED, login -> AUTHENTICATED,
    # disconnect -> CLOSED) driven through the same production typed work items and
    # the real SerializedWorkExecutor<LogicWorkDispatcher>.
    DomainSessionLifecycleTest.cpp
    # M4: the unified work queue/dispatcher implementation. It is added to the
    # test target as SOURCE (not linked through the ChatServer executable) so the
    # tests exercise the production implementation itself while staying free of
    # Redis / MySQL / gRPC / protobuf. LogicSystem.cpp is deliberately NOT part of
    # this target: it pulls in the whole legacy IM business graph.
    ../LogicWorkDispatcher.cpp
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
    # M2: the pure Meeting domain library. It brings its own include root with
    # it (PUBLIC on the MeetingDomain target) and links nothing else, so the
    # test executable stays free of Redis/MySQL/gRPC/protobuf dependencies.
    MeetingDomain
)

# Register each TEST()/TEST_F() case with CTest automatically. This keeps CTest
# names derived from the GoogleTest names instead of a hand-maintained,
# easily-drifting duplicate list.
include(GoogleTest)
gtest_discover_tests(ChatServerUnitTests)

# ------------------------------------------------------------------------------
# M6.6a / R-20: RedisConPool contract tests.
#
# Deliberately a SEPARATE executable:
#   * RedisConPool is defined in RedisMgr.h and needs hiredis only (no ConfigMgr,
#     no Boost.Filesystem, no config.ini);
#   * ChatServerUnitTests must stay Redis-free, so this never joins that target;
#   * RedisMgr.cpp is still NOT compiled here: it calls ConfigMgr::Inst(), which
#     reads config.ini through Boost.Filesystem. The RedisMgr HSet/HDel wrapper
#     behaviour is covered by the ChatServer runtime tests instead (M6.6/M6.7).
#
# PkgConfig::HIREDIS comes from the parent CMakeLists.txt (pkg_check_modules),
# and Threads::Threads from the parent find_package(Threads) call. No new
# third-party dependency is introduced.
# ------------------------------------------------------------------------------
add_executable(RedisConPoolContractTest
    RedisConPoolContractTest.cpp
)

target_compile_features(RedisConPoolContractTest PRIVATE cxx_std_14)

target_include_directories(RedisConPoolContractTest PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/.."
)

target_link_libraries(RedisConPoolContractTest PRIVATE
    GTest::gtest_main
    PkgConfig::HIREDIS
    Threads::Threads
)

gtest_discover_tests(RedisConPoolContractTest)
