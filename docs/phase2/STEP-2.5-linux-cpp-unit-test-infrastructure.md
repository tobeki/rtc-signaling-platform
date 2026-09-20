# STEP 2.5 — Linux C++ Unit-Test Infrastructure

Status: **COMPLETE — unit-test infrastructure ESTABLISHED**

---

## 1. Scope

This step creates the C++ unit-test infrastructure required **before** Meeting
domain implementation begins.

**In scope:** GoogleTest + CTest integration, an optional `BUILD_TESTING` path, a
test subdirectory with its own CMake entry, and one real infrastructure smoke
test that proves the framework runs and that a test can include project headers.

**Out of scope and not performed:** Meeting implementation of any kind; runtime
integration tests; adapter/transport tests; MySQL/Redis/gRPC test doubles;
mocks for future Meeting code; benchmarks; coverage; sanitizers; CI; Docker.

The existing Linux ChatServer build/runtime baseline is authoritative for this
work. This is an **infrastructure Step**, not a Meeting implementation Step.

---

## 2. Starting Baseline

| Item | Value |
|---|---|
| Repository | `rtc-signaling-platform` |
| Working tree | `/home/tobeki/projects/rtc-signaling-platform` |
| Distro | `Ubuntu-24.04-RTC` |
| Branch | `dev` |
| Starting commit | `357fa33f400ecb541e6414260f299d3902c48780` |
| `origin/dev` at start | `357fa33f400ecb541e6414260f299d3902c48780` |
| ahead/behind at start | `0 0` |
| Starting working tree | clean |
| GCC / G++ | 13.3.0 |
| CMake | 3.28.3 |
| Linux server-side development baseline | **AUTHORITATIVE** (Step 2.4) |
| Windows Debug\|x64 baseline | **PRESERVED AS FALLBACK** |

`Ubuntu-20.04` was not used, not touched and not modified.

---

## 3. Framework Decision

**GoogleTest + CTest, from the Ubuntu package repository.**

| Decision | Rationale |
|---|---|
| GoogleTest | Phase 1 §18.3 recorded GoogleTest as the Phase 2 candidate because Meeting state machines suit table-driven unit tests. |
| CTest | Already provided by CMake; standard `BUILD_TESTING` option; no extra dependency. |
| Ubuntu `libgtest-dev` package | Package-managed, consistent with Step 2.2/2.3 dependency policy. |
| No vendoring | A vendored copy would put third-party sources in a tree that must also build on Windows, and would need manual updates. |
| No Git submodules | Submodules change clone/CI semantics and were not requested; keeps the repo self-contained. |
| No `FetchContent` | Would require network access at configure time and would defeat reproducible, package-managed dependency resolution. |
| No Catch2 in parallel | Two frameworks would double the maintenance surface and the Phase 1 decision already named GoogleTest. |
| No manual GoogleTest build | Ubuntu ships headers **and** static libraries plus a CMake package config, so building it ourselves would add nothing. |

Phase 1 §18.2 deliberately did **not** install any framework at that time. This
step is the Phase 2 follow-through that §18.3 recorded as a candidate.

---

## 4. Installed Package

Simulation before install: `2 newly installed, 0 to remove, 0 upgraded` — accepted.

```
sudo apt-get install -y libgtest-dev
```

| Package | Version |
|---|---|
| `libgtest-dev` | **1.14.0-1** |
| `googletest` | **1.14.0-1** |

No `apt upgrade` / `apt full-upgrade` / `do-release-upgrade` was run.

What the package actually provides (inspected, not assumed):

| Item | Path |
|---|---|
| CMake package config | `/usr/lib/x86_64-linux-gnu/cmake/GTest/GTestConfig.cmake` |
| Exported targets | **`GTest::gtest`**, **`GTest::gtest_main`** (both `STATIC IMPORTED`) |
| Static libraries | `/usr/lib/x86_64-linux-gnu/libgtest.a`, `libgtest_main.a` |
| Headers | `/usr/include/gtest/` |
| pkg-config | `gtest.pc`, version 1.14.0 |

Ubuntu ships **static** GoogleTest (`.a`) only — there is no `libgtest.so`. The
target names were confirmed by reading `GTestTargets.cmake`, not guessed.

`GTest::gtest_main` declares `INTERFACE_LINK_LIBRARIES "Threads::Threads;GTest::gtest"`,
so `Threads` arrives through the GoogleTest package rather than being added
manually.

---

## 5. CMake Integration

### 5.1 Main `CMakeLists.txt` (modified minimally)

Two changes only:

1. A short note in the existing header comment recording that a unit-test path
   exists and is optional.
2. A new block appended after the `ChatServer` target definition:

```cmake
include(CTest)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
```

`include(CTest)` defines the standard `BUILD_TESTING` option (default `ON`) and
provides the CTest driver. No project-specific toggle such as
`CHATSERVER_ENABLE_TESTS` was invented — the standard option is sufficient, so
the invocation stays conventional.

The application build contract is unchanged: the `ChatServer` target, its source
list, its dependencies and its link line are untouched.

### 5.2 `tests/CMakeLists.txt` (new)

Responsibilities:

1. discover the system-installed GoogleTest — `find_package(GTest REQUIRED)`;
2. create one unit-test executable — `ChatServerUnitTests`;
3. apply C++14 via `target_compile_features(... PRIVATE cxx_std_14)`, the same
   mechanism the application target uses;
4. add a **test-only** include path reaching the source root so `#include "const.h"`
   works;
5. link only `GTest::gtest_main`;
6. register cases with CTest automatically via
   `include(GoogleTest)` + `gtest_discover_tests(ChatServerUnitTests)`.

`gtest_discover_tests` derives CTest names from the GoogleTest names, so there is
no hand-maintained duplicate test list that could drift.

### 5.3 Test target boundary (deliberate)

The target does **not** link `ChatServer`, and does **not** link gRPC, protobuf,
hiredis, MySQL Connector/C++, JsonCpp or Boost.Filesystem merely because the
application uses them. Tests in this subdirectory must stay process-local,
network-free, Redis-free, MySQL-free and gRPC-free.

The rationale is architectural, not cosmetic: future pure Meeting-domain tests
(M2/M3) must be able to run without inheriting the server runtime dependency
graph, so a domain test failure can never be confused with an adapter or
datastore problem. Integration tests that genuinely need the runtime graph belong
in a separate, explicitly named target later.

### 5.4 `BUILD_TESTING` behaviour

`find_package(GTest ...)` lives **inside** `tests/CMakeLists.txt`, which is only
entered when `BUILD_TESTING` is `ON`. GoogleTest is therefore never a requirement
for someone who only wants to compile ChatServer.

---

## 6. Initial Test

**Test name:** `TestInfrastructureSmoke.DeferExecutesOnScopeExit`
**File:** `tests/TestInfrastructureSmoke.cpp`

**Behaviour validated:** the existing `Defer` RAII utility in `const.h` runs its
callback exactly once, at scope exit — not before.

```cpp
int invocation_count = 0;
{
    Defer defer([&invocation_count]() { ++invocation_count; });
    EXPECT_EQ(invocation_count, 0);   // must not run while the scope is alive
}
EXPECT_EQ(invocation_count, 1);       // ran exactly once as the scope ended
```

### Why this is not a dummy test

| Property | Why it matters |
|---|---|
| It tests **real production code** (`Defer` from `const.h`), not a synthetic subject | `EXPECT_TRUE(true)` would prove only that GoogleTest can print. This asserts an actual semantic property of code compiled into the application. |
| It asserts a **two-sided** property | The `EXPECT_EQ(..., 0)` inside the scope rules out "callback ran too early"; the outer `EXPECT_EQ(..., 1)` rules out "never ran" and "ran twice". A single assertion would not catch a premature invocation. |
| It has **zero runtime dependencies** | `Defer` needs only `<functional>`. No Redis, MySQL, gRPC, sockets or `config.ini`, so the test needs no service provisioning. |
| It proves a **second, structural** thing | `#include "const.h"` compiles, which demonstrates the include boundary that future Meeting-domain tests will rely on. |

`Defer` was chosen precisely because it is real production code with zero
infrastructure. No production behaviour was added or changed to create something
to test.

### Explicitly not Meeting coverage

This is **not** Meeting coverage. The Phase 1 test strategy defines 57
first-round Meeting correctness scenarios (Create 7, Join 9, Leave 11,
Disconnect 8, Close 11, Query 6, Locality/API ordering 5). **None of them is
implemented here.** This step only makes them implementable. No placeholder tests
were added to inflate the count.

---

## 7. Gate A — Application Build with Tests Disabled

```
rm -rf /tmp/rtc-step25-prod-build
cmake -S Server/ChatServer/ChatServer -B /tmp/rtc-step25-prod-build \
      -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=OFF
cmake --build /tmp/rtc-step25-prod-build --parallel 2
```

| Stage | Result |
|---|---|
| configure | **PASS** (exit 0) |
| protobuf generation | **PASS** |
| gRPC generation | **PASS** |
| ChatServer compile | **PASS** (0 errors) |
| ChatServer link | **PASS** |
| executable | `/tmp/rtc-step25-prod-build/ChatServer`, 12,431,816 bytes |

Warning count: **1** — the same pre-existing
`MysqlDao.cpp:95: control reaches end of non-void function` recorded in Steps
2.3/2.4. No new warning.

Evidence that this path does not require GoogleTest discovery:

- the configure log contains **no** `gtest`/`GoogleTest` reference at all;
- `CMakeCache.txt` contains **no** `GTest` entry;
- `BUILD_TESTING:BOOL=OFF` is recorded in the cache;
- no `tests/` build subdirectory is produced.

**Additional, stronger proof.** A second throwaway configure was run with
GoogleTest discovery explicitly disabled via the standard CMake variable:

```
cmake -S Server/ChatServer/ChatServer -B <dir> \
      -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=OFF \
      -DCMAKE_DISABLE_FIND_PACKAGE_GTest=ON
```

It configured and built `ChatServer` successfully (exit 0, 0 errors), which
demonstrates that the production path would work even on a machine without
GoogleTest installed. That throwaway directory was deleted afterwards.

ChatServer was **not** executed.

---

## 8. Gate B — Test Build

```
rm -rf /tmp/rtc-step25-test-build
cmake -S Server/ChatServer/ChatServer -B /tmp/rtc-step25-test-build \
      -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON \
      -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build /tmp/rtc-step25-test-build --parallel 2
```

| Stage | Result |
|---|---|
| configure | **PASS** (exit 0) |
| ChatServer build | **PASS** (0 errors) |
| `ChatServerUnitTests` build | **PASS** |
| warnings | 1 (the same pre-existing warning) |

Artifacts:

| Target | Path | Size |
|---|---|---|
| ChatServer | `/tmp/rtc-step25-test-build/ChatServer` | 12,431,816 B |
| ChatServerUnitTests | `/tmp/rtc-step25-test-build/tests/ChatServerUnitTests` | **781,696 B** |

The ~16× size difference is itself corroboration that the test target did not
inherit the application dependency graph.

Neither build reused Step 2.3/2.4 artifacts.

---

## 9. Gate C — CTest Discovery

```
ctest --test-dir /tmp/rtc-step25-test-build -N
```

```
Test project /tmp/rtc-step25-test-build
  Test #1: TestInfrastructureSmoke.DeferExecutesOnScopeExit

Total Tests: 1
```

- test count: **1**
- discovered name: **`TestInfrastructureSmoke.DeferExecutesOnScopeExit`**

The name is GoogleTest-derived (via `gtest_discover_tests`), and the generated
`tests/CTestTestfile.cmake` includes the discovery include file as expected.

---

## 10. Gate D — CTest Execution

```
ctest --test-dir /tmp/rtc-step25-test-build --output-on-failure
```

```
    Start 1: TestInfrastructureSmoke.DeferExecutesOnScopeExit
1/1 Test #1: TestInfrastructureSmoke.DeferExecutesOnScopeExit ...   Passed    0.00 sec

100% tests passed, 0 tests failed out of 1

Total Test time (real) =   0.00 sec
```

- number of tests: **1**
- pass/fail: **1 passed, 0 failed**
- exit code: **0**

This is the project's **first actual formal C++ unit-test execution**. It is
reported as a test result, not as a build result.

---

## 11. Direct Test Binary Verification

Run from an empty working directory deliberately containing **no `config.ini`**:

```
cd /tmp/s25-empty-cwd
/tmp/rtc-step25-test-build/tests/ChatServerUnitTests
```

```
Running main() from ./googletest/src/gtest_main.cc
[==========] Running 1 test from 1 test suite.
[----------] 1 test from TestInfrastructureSmoke
[ RUN      ] TestInfrastructureSmoke.DeferExecutesOnScopeExit
[       OK ] TestInfrastructureSmoke.DeferExecutesOnScopeExit (0 ms)
[  PASSED  ] 1 test.
```

- exit code: **0**
- GoogleTest ran normally.
- **No config file was requested** — `ConfigMgr` is not part of this target's
  dependency graph at all, so it is structurally impossible for the test to read
  a config, not merely untested.
- **No Redis, MySQL or network dependency was touched.**

ChatServer itself was not run.

---

## 12. Dependency Isolation

### 12.1 GoogleTest resolution — PASS

The WSL environment inherits a Windows `PATH`, and a scan confirmed that conda
exposes a **conflicting GTest config** at
`/mnt/f/conda/Library/lib/cmake/GTest/GTestConfig.cmake` — the same class of
hazard Step 2.3 hit with `re2`.

Empirical result of the configure:

```
-- Found GTest: /usr/lib/x86_64-linux-gnu/cmake/GTest/GTestConfig.cmake (found version "1.14.0")
GTest_DIR:PATH=/usr/lib/x86_64-linux-gnu/cmake/GTest
```

GoogleTest resolved from the **Ubuntu filesystem**, not from `/mnt`. No
compatibility workaround was needed and none was created.

`/mnt` occurrences in `compile_commands.json`: **0**.

### 12.2 Test-target compile command

Compiling `tests/TestInfrastructureSmoke.cpp`:

```
/usr/bin/c++ -I<repo>/Server/ChatServer/ChatServer/tests/.. -g -DGTEST_HAS_PTHREAD=1 -c TestInfrastructureSmoke.cpp
```

Only the test-only include path, the standard Debug flag and GoogleTest's own
interface definition. No `/mnt`, no application include roots, no
MySQL-compat root.

### 12.3 `ldd` — PASS

```
linux-vdso.so.1
libstdc++.so.6 => /lib/x86_64-linux-gnu/libstdc++.so.6
libm.so.6      => /lib/x86_64-linux-gnu/libm.so.6
libgcc_s.so.1  => /lib/x86_64-linux-gnu/libgcc_s.so.1
libc.so.6      => /lib/x86_64-linux-gnu/libc.so.6
/lib64/ld-linux-x86-64.so.2
```

- `not found`: **0**
- libraries resolved under `/mnt`: **none**
- **none** of the application-infrastructure libraries appear:

| Library | In test binary |
|---|---|
| `libgrpc++` | absent |
| `libgrpc` | absent |
| `libprotobuf` | absent |
| `libhiredis` | absent |
| `libmysqlcppconn` | absent |
| `libmysqlclient` | absent |
| `libjsoncpp` | absent |
| `libboost_filesystem` | absent |

There is no `libgtest.so` dependency because Ubuntu ships GoogleTest as static
libraries — the framework is linked into the binary (link line:
`libgtest_main.a libgtest.a`).

### 12.4 Additional boundary evidence

- Translation units compiled into `ChatServerUnitTests`: **exactly one**
  (`TestInfrastructureSmoke.cpp.o`) — no application `.cpp` is linked in.
- Include closure of the test TU contains **exactly one** project header
  (`const.h`); everything else is `gtest` plus libstdc++/gcc system headers.
- Undefined symbols referencing `redis`/`mysql`/`grpc`/`protobuf`/`jsoncpp`/`boost`:
  **0**.

---

## 13. Test Infrastructure Status

```
Linux C++ unit-test infrastructure: ESTABLISHED
GoogleTest integration: ESTABLISHED
CTest integration: ESTABLISHED
Meeting domain tests: NOT STARTED
```

---

## 14. Intended Evolution (documented only, not implemented)

```
Step 2.5   GoogleTest + CTest infrastructure
    ↓
M1         Meeting value types compile
    ↓
M2         Pure MeetingAggregate  +  state-machine unit tests   (Layer A)
    ↓
M3         MeetingService  +  API / fan-out / ordering unit tests (Layer A)
    ↓
M4         Serialized work ordering tests                        (Layer B)
    ↓
M5 / M6    ChatServer integration tests                          (Layer C)
```

Phase 1 §17 layer mapping is preserved: Layer A first runs at M2/M3, Layer B at
M4, Layer C at M5/M6, Layer D deferred until a real protocol adapter exists.

This infrastructure exists so that M2/M3 can be tested with **no real network and
no datastore**. The 57 Phase 1 scenarios remain unimplemented by design; this
step only makes them implementable.

---

## 15. Remaining Work / Observations

1. **M1 is next.** `meeting/MeetingTypes.h` plus the Visual Studio project-file
   entries. Not started here.
2. **Effective language standard is the compiler default, not an explicit flag.**
   `target_compile_features(... cxx_std_14)` states a C++14 *minimum*; because
   GCC 13's default already satisfies it (`__cplusplus == 201703L`), CMake adds
   no `-std=` flag to either the application or the test target. This is
   consistent with the existing application target, but it means Phase 1 §7's
   "no C++17 features" rule is currently a **discipline rule, not a
   compiler-enforced one**. Worth a deliberate decision at M1 (either accept it,
   or pin an explicit standard on both targets).
3. **This smoke test is not a regression net.** One assertion pair on `Defer`
   proves the plumbing, not the product.
4. **Test target split is still single-target.** When M4+ integration tests
   arrive they must not be added to `ChatServerUnitTests`, which is intentionally
   light; a separate target will be needed.
5. **GoogleTest version is pinned by the distro** (1.14.0-1). A later upgrade of
   the Ubuntu package could change GoogleTest behaviour or its CMake export.
6. **No coverage, sanitizers or benchmarks** were added — all out of scope by
   instruction.

---

## 16. Next Step

Proposed only — **not started**:

**M1 — Meeting Domain Value Types**

**Do not begin M1 from this document.**
