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
# M6.2: MeetingRequestContextBuilder (CB1-CB6).
# M6.3: lifetime-safe session lookup (SL1-SL7).
#
# Layer C direct-invocation tests. Separate target on purpose:
#   * it links GTest + MeetingDomain only (no application runtime graph);
#   * it stays process-local: no network / Redis / MySQL / gRPC / protobuf;
#   * ChatServerUnitTests keeps its existing sources and semantics untouched.
#
# MEETING_CONTEXT_BUILDER_HEADER feeds the CB6 static check with the absolute
# source path, so the test can assert the builder header never references
# session internals.
#
# M6.3 note (deliberate, and narrower than it looks): the SL tests include
# CServer.h / CSession.h for *compile-time* assertions only. Those headers pull
# in Boost.Asio / Boost.Beast **headers**, not libraries: no CServer / CSession
# object is constructed, no member-function address is taken outside decltype,
# and the link line is unchanged. The alternative -- a separate build-time
# check file outside the test target -- would have made the assertions
# non-reproducible from the repository, so the real headers are preferred.
# ------------------------------------------------------------------------------
add_executable(MeetingResponseNotificationAdapterTest
    MeetingResponseNotificationAdapterTest.cpp
    # M6.4: the **real** adapter and the **real** production collector are part
    # of this target (not re-implemented in the test). Same style as
    # ../LogicWorkDispatcher.cpp in ChatServerUnitTests. Both TUs depend only on
    # the standard library + the pure domain headers, so the target still links
    # GTest + MeetingDomain only -- no asio, Boost, Redis, MySQL, gRPC,
    # protobuf or jsoncpp (M6 §16.5).
    ../MeetingResponseNotificationAdapter.cpp
    ../CountingNoWireCollector.cpp
    # M6.5: the **real** dispatcher is compiled into this target too, so the
    # adapter call sites (six responses + FINALIZE_CLOSE) are exercised through
    # the production dispatch code instead of a test-local copy. Same TU and
    # same style as ChatServerUnitTests above. It depends only on
    # MeetingDomain + the standard library + Boost.UUID headers, so the target
    # still links GTest + MeetingDomain only.
    ../LogicWorkDispatcher.cpp
)

target_compile_features(MeetingResponseNotificationAdapterTest PRIVATE cxx_std_14)

target_include_directories(MeetingResponseNotificationAdapterTest PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/.."
)

target_compile_definitions(MeetingResponseNotificationAdapterTest PRIVATE
    MEETING_CONTEXT_BUILDER_HEADER="${CMAKE_CURRENT_SOURCE_DIR}/../MeetingRequestContextBuilder.h"
    # M6.3: absolute source root for the SL1-SL7 static checks. Same pattern as
    # MEETING_CONTEXT_BUILDER_HEADER above: the checks read the *real*
    # CServer.cpp / CSession.cpp / MeetingOutboundChannel.h text instead of
    # duplicating the production implementation inside the test.
    CHATSERVER_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/.."
)

target_link_libraries(MeetingResponseNotificationAdapterTest PRIVATE
    GTest::gtest_main
    # Pure domain library: brings its own include root (PUBLIC) and links nothing.
    MeetingDomain
)

gtest_discover_tests(MeetingResponseNotificationAdapterTest)

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
