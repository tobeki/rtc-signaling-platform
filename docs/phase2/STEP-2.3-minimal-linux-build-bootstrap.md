# STEP 2.3 — Minimal Linux Build Bootstrap (ChatServer Only)

Status: **COMPLETE — Linux configure/generate/compile/link path ESTABLISHED**

---

## 1. Scope

This step establishes the smallest real Linux build path that can **configure,
generate protobuf/gRPC sources, compile and link ChatServer**.

Explicitly out of scope and **not performed**:

- runtime execution of ChatServer;
- runtime `config.ini` creation;
- Redis Server / MySQL Server installation or startup;
- TCP or gRPC port binding;
- Docker, CI, whole-repository CMake migration;
- CMake support for Windows;
- Meeting implementation.

The Linux **authoritative runtime baseline remains NOT ESTABLISHED**.
Runtime smoke and baseline cutover belong to Step 2.4.

---

## 2. Starting State

| Item | Value |
|---|---|
| Repository | `rtc-signaling-platform` |
| Working tree | `/home/tobeki/projects/rtc-signaling-platform` |
| Distro | `Ubuntu-24.04-RTC` (Ubuntu 24.04.5 LTS, Noble Numbat) |
| Branch | `dev` |
| Starting commit | `df991a6907b46077ed1933b681e5e172447e4951` |
| `origin/dev` at start | `df991a6907b46077ed1933b681e5e172447e4951` |
| Starting `git status --short` | clean (empty) |
| GCC / G++ | 13.3.0 (`13.3.0-6ubuntu2~24.04.1`) |
| CMake | 3.28.3 |
| Git | 2.43.0 |
| Windows fallback baseline | `Windows + Visual Studio 2022 + Debug\|x64`, MSBuild 17.14, v143 |

`Ubuntu-20.04` was not touched, not used and not modified in any way.

---

## 3. Encoding Probe (Gate G1)

### 3.1 Fresh byte-level measurement

Performed over all tracked ChatServer `*.cpp` / `*.h` files (13 `.cpp` + 15 `.h`):

| Metric | Fresh measurement (Step 2.3) |
|---|---|
| Files examined | **28** |
| Valid UTF-8 | **11** |
| With UTF-8 BOM | **1** — `ChatServer.cpp` |
| Invalid UTF-8 | **17** |
| Non-ASCII **string literals** | **6** — all in `RedisMgr.h` |

Exact invalid-UTF-8 file list (17):

```
AsioIOServicePool.cpp   AsioIOServicePool.h   CServer.cpp
CSession.cpp            CSession.h            ChatGrpcClient.h
ChatServiceImpl.cpp     ConfigMgr.cpp         ConfigMgr.h
LogicSystem.cpp         MsgNode.cpp           MysqlDao.cpp
MysqlDao.h              RedisMgr.cpp          RedisMgr.h
UserMgr.cpp             const.h
```

The 6 non-ASCII string literals are all GBK `"认证失败"` / `"认证成功"` at
`RedisMgr.h` lines 25, 33, 122, 131, 236, 244 (bytes
`c8cfd6a4caa7b0dc` / `c8cfd6a4b3c9b9a6`). Every other non-ASCII byte in the tree
occurs inside comments only.

### 3.2 Step 2.2 documentation discrepancy — resolved

Step 2.2 §3.5 states `illegal UTF-8 / GBK files = 14` while the filename list
directly below it contains **17** names.

**Fresh measurement shows 17 invalid UTF-8 files, exactly matching the Step 2.2
filename list.** The Step 2.2 *count* of 14 is therefore the incorrect number;
the list was correct.

Step 2.2 itself was deliberately **not** modified to fix the count.

### 3.3 Gate G1 probes (executed outside the repository)

Probe directory `/tmp/rtc-step23-encoding-probe/` was created from **actual
repository bytes**, then deleted after the experiment.

Probe A — GBK bytes inside a **comment**, taken verbatim from `ConfigMgr.cpp`
line 4 (`09 2f 2f 20 bb f1 c8 a1 ...`):

```
g++ -c probeA.cpp -o probeA.o                  -> exit 0, no diagnostics
g++ -Wall -Wextra -c probeA.cpp                -> exit 0, no diagnostics
g++ -std=c++14 -Wall -Wextra -pedantic \
    -finput-charset=UTF-8 -c probeA.cpp        -> exit 0, no diagnostics
```

Probe B — GBK bytes inside a **narrow string literal**, taken verbatim from
`RedisMgr.h` line 25 (`"c8 cf d6 a4 ca a7 b0 dc"`):

```
g++ -c probeB.cpp -o probeB.o                  -> exit 0, no diagnostics
g++ -Wall -Wextra -c probeB.cpp                -> exit 0, no diagnostics
g++ -std=c++14 -Wall -Wextra -pedantic \
    -finput-charset=UTF-8 -c probeB.cpp        -> exit 0, no diagnostics
```

Supporting evidence:

- **Positive control**: a genuinely unterminated string literal *does* fail
  (exit 1, `error: missing terminating " character`), proving the probes were
  actually being validated by the compiler.
- **Byte pass-through**: the emitted object file still contains the exact byte
  sequence `c8 cf d6 a4 ca a7 b0 dc`, i.e. GCC 13.3 passes the raw bytes through
  to the narrow string literal unchanged.
- **Null control**: even a raw `0xFF 0xFE` inside a narrow string literal is
  accepted (exit 0). GCC 13.3 with the default input charset does not validate
  UTF-8 in narrow string literals.
- **Lexical-trap scan**: 2203 non-ASCII byte pairs were examined; **zero** have
  `0x5C` (`\`) or `0x22` (`"`) as the trail byte, and there are **zero**
  line-final backslashes. The classic GBK backslash-splicing hazard does not
  occur in this tree.

### 3.4 Decision — Gate G1 resolves to **Case A**

GCC accepts the current bytes exactly as committed, in both comments and narrow
string literals, at the real build path.

Therefore:

- `RedisMgr.h` was **not** modified for encoding reasons;
- the six Chinese log literals remain untouched in their original GBK bytes;
- no file was transcoded;
- no global `-finput-charset=GBK` was added;
- no mass encoding migration was performed.

---

## 4. Installed Dependencies

`apt-get update` followed by installation of the approved package set only.
`apt upgrade`, `apt full-upgrade` and `do-release-upgrade` were **not** run.

Pre-install `apt-get -s install` simulation result: **37 packages to install, 0
to remove, no distribution upgrade, no errors** — accepted.

Exact command:

```
sudo apt-get install -y pkg-config \
  libboost-dev libboost-filesystem-dev libboost-random-dev \
  libhiredis-dev libjsoncpp-dev \
  libprotobuf-dev protobuf-compiler \
  libgrpc++-dev protobuf-compiler-grpc \
  libmysqlcppconn-dev
```

Directly requested packages and versions:

| Package | Version |
|---|---|
| `pkg-config` | 1.8.1-2build1 |
| `libboost-dev` | 1.83.0.1ubuntu2 |
| `libboost-filesystem-dev` | 1.83.0.1ubuntu2 |
| `libboost-random-dev` | 1.83.0.1ubuntu2 |
| `libhiredis-dev` | 1.2.0-6ubuntu3 |
| `libjsoncpp-dev` | 1.9.5-6build1 |
| `libprotobuf-dev` | 3.21.12-8.2ubuntu0.3 |
| `protobuf-compiler` | 3.21.12-8.2ubuntu0.3 |
| `libgrpc++-dev` | 1.51.1-4.1build5 |
| `protobuf-compiler-grpc` | 1.51.1-4.1build5 |
| `libmysqlcppconn-dev` | 1.1.12-4.1ubuntu2 |

Key transitive packages pulled in: `libprotobuf32t64` 3.21.12, `libgrpc29t64`
1.51.1, `libabsl20220623t64`/`libabsl-dev` 20220623.1, `libre2-dev`/`libre2-10`,
`libc-ares-dev`/`libcares2` 1.27.0, `libssl-dev` 3.0.13, `zlib1g-dev` 1.3.

Not installed: MySQL Server, Redis Server, Docker, vcpkg, Conan, any additional
compiler or CMake, any source-built Boost/protobuf/gRPC.

---

## 5. CMake Structure

Single new file: `Server/ChatServer/ChatServer/CMakeLists.txt` (Linux-only).

Invocation used throughout:

```
cmake -S Server/ChatServer/ChatServer \
      -B /tmp/rtc-signaling-chatserver-build \
      -DCMAKE_BUILD_TYPE=Debug

cmake --build /tmp/rtc-signaling-chatserver-build --parallel 2
```

Build directory is out-of-repository; no build artifact is written inside the
Git working tree.

### 5.1 Source list strategy

No recursive globbing. The application source list is an explicit
`set(CHATSERVER_APPLICATION_SOURCES ...)` derived from the authoritative
`<ClCompile>` items of `ChatServer.vcxproj`:

```
AsioIOServicePool.cpp   ChatGrpcClient.cpp   ChatServer.cpp
ChatServiceImpl.cpp     ConfigMgr.cpp        CServer.cpp
CSession.cpp            LogicSystem.cpp      MsgNode.cpp
MysqlDao.cpp            MysqlMgr.cpp         RedisMgr.cpp
UserMgr.cpp
```

Source mapping:

| Kind | Origin | Files |
|---|---|---|
| Handwritten application | tracked in repo | 13 `.cpp` above |
| Generated protobuf | build tree, from `message.proto` | `generated/message.pb.cc` |
| Generated gRPC | build tree, from `message.proto` | `generated/message.grpc.pb.cc` |

`ChatServer.vcxproj` additionally lists `message.pb.cc` / `message.grpc.pb.cc`.
Those repository copies are deliberately **not** Linux source inputs — the Linux
build regenerates them from `message.proto`.

### 5.2 C++ standard

```cmake
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
target_compile_features(ChatServer PRIVATE cxx_std_14)
```

C++14 is the minimum clearly sufficient level and matches the Visual Studio
project, which pins no newer standard and only sets `ConformanceMode=true`.
`CMAKE_CXX_EXTENSIONS OFF` mirrors that conformance mode by disabling GNU
extensions. No source file required a newer language level — every translation
unit compiled at C++14 without a language-feature error.

### 5.3 Dependency discovery mechanism actually used

| Dependency | Mechanism |
|---|---|
| Threads | `find_package(Threads)` → `Threads::Threads` |
| Boost.Filesystem, Boost.Random | `find_package(Boost REQUIRED COMPONENTS filesystem random)` → `Boost::filesystem`, `Boost::random` |
| protobuf | CMake `FindProtobuf` module → `protobuf::libprotobuf`; `Protobuf_PROTOC_EXECUTABLE` = `/usr/bin/protoc` |
| gRPC | `find_package(gRPC CONFIG)` → `gRPC::grpc++` (Ubuntu's `gRPCConfig.cmake`) |
| JsonCpp | `find_package(jsoncpp CONFIG)` → `JsonCpp::JsonCpp` (carries include root `/usr/include/jsoncpp`) |
| hiredis | `pkg_check_modules(HIREDIS REQUIRED IMPORTED_TARGET hiredis)` → `PkgConfig::HIREDIS` (supplies `-I/usr/include/hiredis`) |
| MySQL Connector/C++ | `find_path` + `find_library` → `/usr/include`, `/usr/lib/x86_64-linux-gnu/libmysqlcppconn.so` |
| re2 | explicit imported target from the Ubuntu package (see §7) |

Boost.Random was confirmed to be a **real** compiled dependency:
`CSession.cpp` calls `boost::uuids::random_generator()`, which uses Boost.Random
in Boost 1.83.

hiredis: Ubuntu's `HiredisConfig.cmake` exports **no** imported target and sets
its include variable to `/usr/include`, while the source uses
`#include "hiredis.h"` which lives in `/usr/include/hiredis`. pkg-config is
therefore the correct integration and is what was used.

No Windows library list was reproduced: `ws2_32.lib`, `Win32_Interop.lib`,
`address_sorting.lib`, the ~54 Abseil `.lib` names and the BoringSSL list are
absent. The whole transitive closure (Abseil, re2, c-ares, BoringSSL, zlib) is
resolved from Ubuntu packages; `ldd` confirms every shared object resolves to
`/lib/x86_64-linux-gnu/`.

### 5.4 Generated-code directory

`${CMAKE_CURRENT_BINARY_DIR}/generated` — i.e.
`/tmp/rtc-signaling-chatserver-build/generated/`. It is on the target include
path and in the target dependency graph, so editing `message.proto` regenerates
the outputs.

---

## 6. MySQL Compatibility Layer (Constraint H1)

Ubuntu's `libmysqlcppconn-dev` installs a **flat** header layout:

```
/usr/include/mysql_driver.h
/usr/include/mysql_connection.h
/usr/include/cppconn/{prepared_statement,resultset,statement,exception,...}.h
```

The unchanged application source (`MysqlDao.h`) includes the Windows
Connector/C++ layout:

```
<jdbc/mysql_driver.h>
<jdbc/mysql_connection.h>
<jdbc/cppconn/prepared_statement.h>
<jdbc/cppconn/resultset.h>
<jdbc/cppconn/statement.h>
<jdbc/cppconn/exception.h>
```

Following the commander's Step 2.3 H1 decision, a **Linux-only compatibility
forwarding include tree** was created under:

```
Server/ChatServer/ChatServer/cmake/compat/mysqlcppconn/
└── jdbc/
    ├── mysql_driver.h                          -> #include <mysql_driver.h>
    ├── mysql_connection.h                      -> #include <mysql_connection.h>
    └── cppconn/
        ├── prepared_statement.h                -> #include <cppconn/prepared_statement.h>
        ├── resultset.h                         -> #include <cppconn/resultset.h>
        ├── statement.h                         -> #include <cppconn/statement.h>
        └── exception.h                         -> #include <cppconn/exception.h>
```

Exactly the six forwarding headers required by `MysqlDao.h` were created. They
contain no business logic — only `#pragma once` and one angle-bracket include
each. No MySQL Connector header was copied into the repository.

The compatibility root is added with `BEFORE`, so it is searched ahead of the
normal system include path:

```cmake
target_include_directories(ChatServer BEFORE PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/compat/mysqlcppconn")
```

**The six includes in `MysqlDao.h` were not modified.** The Windows build keeps
using its own `jdbc/` layout from `F:\cppsoft\mysql_connector\include`.

---

## 7. Non-obvious Blocker Found and Fixed — Foreign re2 / conda prefix

### 7.1 Symptom

First build attempt failed with the protobuf version guard in generated code:

```
message.pb.h:17: #error This file was generated by an older version of protoc ...
```

followed by cascading `PROTOBUF_NAMESPACE_OPEN does not name a type` errors.

### 7.2 Root cause (established by direct measurement)

The WSL shell inherits the Windows `PATH`, which exposes a conda installation at
`/mnt/f/conda` (`/mnt/f/conda/Library/bin`, ...). CMake derives candidate package
prefixes from `PATH`, so gRPC's own internal `find_package(re2)` resolved to:

```
re2_DIR = /mnt/f/conda/Library/lib/cmake/re2
```

Conda's `re2Config.cmake` exports `INTERFACE_INCLUDE_DIRECTORIES = /mnt/f/conda/Library/include`,
and that tree contains **protobuf 4.25** headers
(`PROTOBUF_MIN_PROTOC_VERSION 4025000`), whereas the Ubuntu system protobuf is
**3.21.12** (`PROTOBUF_VERSION 3021012`, `PROTOBUF_MIN_PROTOC_VERSION 3021000`).

Because gRPC's exported target links `re2::re2`, conda's include root entered the
compile line as a system include directory and was searched **before**
`/usr/include`, so `#include <google/protobuf/port_def.inc>` picked up the
conda/4.25 copy. The generated 3.21 code then correctly rejected it.

Cache audit confirmed **re2 was the only contaminant** — `re2_DIR` was the only
package-direction entry in the whole CMake cache pointing outside `/usr`, and
`/mnt/` appeared exactly once.

### 7.3 Why the obvious alternative was rejected

Globally disabling PATH-derived prefixes
(`set(CMAKE_FIND_USE_SYSTEM_ENVIRONMENT_PATH OFF)`) was tested and **rejected**:
it also disables compiler and `make` discovery, so CMake fails with
`CMAKE_MAKE_PROGRAM is not set` / `CMAKE_CXX_COMPILER not set`. It is too broad
and breaks fundamental toolchain detection.

### 7.4 Fix applied (CMake dependency wiring, within Step 2.3 scope)

Ubuntu's `libre2-dev` is a real system package but ships **no** CMake package
config. `CMakeLists.txt` therefore supplies the missing imported target from the
Ubuntu package before `find_package(gRPC CONFIG)`:

```cmake
if(NOT TARGET re2::re2)
    find_path(CHATSERVER_RE2_INCLUDE_DIR NAMES re2/re2.h)
    find_library(CHATSERVER_RE2_LIBRARY NAMES re2)
    ...
    add_library(re2::re2 UNKNOWN IMPORTED)
endif()
```

CMake's generated export files return early when the targets they would define
already exist, so this definition wins and the foreign include root never
reaches the compile line.

Verification after the fix:

- `CXX_INCLUDES` no longer contains any `/mnt/...` entry;
- the link line uses `/usr/lib/x86_64-linux-gnu/libre2.so`;
- configure succeeds.

The foreign conda installation itself was only read — never modified.

---

## 8. protobuf / gRPC Generation (Gate G3)

| Item | Value |
|---|---|
| `protoc` | `libprotoc 3.21.12` (`protobuf-compiler` 3.21.12-8.2ubuntu0.3) |
| `grpc_cpp_plugin` | package `protobuf-compiler-grpc` 1.51.1-4.1build5, `/usr/bin/grpc_cpp_plugin` |
| Source of truth | tracked `Server/ChatServer/ChatServer/message.proto` |
| Output location | `/tmp/rtc-signaling-chatserver-build/generated/` |
| Mechanism | two `add_custom_command` rules + a `ChatServerGeneratedProto` custom target that `ChatServer` depends on |

Generated outputs (build tree only):

```
message.pb.cc        179571 bytes
message.pb.h         155937 bytes
message.grpc.pb.cc    27197 bytes
message.grpc.pb.h     91359 bytes
```

`add_dependencies(ChatServer ChatServerGeneratedProto)` guarantees the generated
headers exist before **any** application source is compiled, including sources
that only reach them through a header (`ChatGrpcClient.h`, `ChatServiceImpl.h`),
which would otherwise be a parallel-build race.

Windows artifacts were not used: no `.exe`, no `start.bat`, and none of the
ignored Windows-generated `.pb.cc/.pb.h` files in the repository.

Repository cleanliness after generation:

```
git status --short
```

shows **no** generated protobuf/gRPC file. `.gitignore` was not modified.

---

## 9. Source Portability Blocker Found and Resolved (authorized decision)

### 9.1 Symptom

With the protobuf issue fixed, 5 of 15 translation units failed with missing
standard-library types:

```
ChatGrpcClient.h:90:  'condition_variable' in namespace 'std' does not name a type
MysqlDao.h:229:       'condition_variable' in namespace 'std' does not name a type
MysqlDao.h:230/232:   field 'b_stop_' / 'fail_count_' has incomplete type 'std::atomic<...>'
RedisMgr.h:258/259:   'condition_variable' / 'thread' in namespace 'std' does not name a type
```

Affected translation units: `ChatGrpcClient.cpp`, `MysqlDao.cpp`,
`MysqlMgr.cpp`, `RedisMgr.cpp`, `UserMgr.cpp`.

### 9.2 Root cause

These headers are **not self-contained**. The MSVC standard library transitively
includes these headers; libstdc++ does not. This is a transitive-include
difference, not a Win32/MSVC *API* dependency, which is why the Step 2.2 audit
did not surface it.

The complete missing set is exactly **three** headers: `<atomic>`,
`<condition_variable>`, `<thread>`.

### 9.3 Why this required a decision

Step 2.3 §8 authorizes modification of only `RedisMgr.h` (and only under the
encoding rule). Editing `ChatGrpcClient.h` / `MysqlDao.h` was outside the
original change set, so the step was **stopped and escalated** with evidence
rather than silently working around it.

Two options were presented with measured consequences:

- **A — build-layer**: `target_compile_options(... -include atomic -include condition_variable -include thread)`.
  Verified by a throwaway configure to produce a full PASS with zero source
  changes.
- **B — source fix**: add the missing standard includes to the three headers.
  Textbook-correct, but requires modifying three existing headers and a Windows
  regression build.

**The commander selected option B.** It is recorded here because it broadens the
original Step 2.3 change set on explicit authorization.

### 9.4 Change applied

5 added lines, 0 removed, 0 reformatted, byte-preserving:

```
ChatGrpcClient.h   +#include <condition_variable>          (after <json/reader.h>)
MysqlDao.h         +#include <atomic>                      (after <thread>)
                   +#include <condition_variable>
RedisMgr.h         +#include <thread>                      (after <mutex>)
                   +#include <condition_variable>
```

Byte-preservation of the legacy GBK content was **proved**: removing the
inserted bytes from each new file reproduces the original file byte-for-byte
(checked with an exact byte comparison, not by eye). The 6 GBK literals in
`RedisMgr.h` are unchanged, the invalid-UTF-8 file count is still 17, and the
diff is `3 files changed, 5 insertions(+)` with no deletions.

This change is harmless on Windows (adding a standard include that MSVC already
provided transitively) and was validated by the Windows regression in §11.

---

## 10. Build Result (Gate G4)

Authoritative clean rebuild, using the exact prescribed commands with no extra
flags, after `rm -rf /tmp/rtc-signaling-chatserver-build`:

```
cmake -S Server/ChatServer/ChatServer \
      -B /tmp/rtc-signaling-chatserver-build \
      -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/rtc-signaling-chatserver-build --parallel 2
```

| Stage | Result |
|---|---|
| CMake configure | **PASS** (exit 0) |
| protobuf generation | **PASS** |
| gRPC generation | **PASS** |
| C++ compile | **PASS** (0 errors) |
| link | **PASS** |
| ChatServer executable produced | **YES** |

Warning count: **1**, and it is a pre-existing source property, not a build
wiring issue:

```
MysqlDao.cpp:95:1: warning: control reaches end of non-void function [-Wreturn-type]
```

(This is the same `MysqlDao::CheckEmail` path-missing-return that MSVC also
reports as C4715 — see §11.)

No runtime execution was performed.

---

## 11. Windows Baseline Preservation

The Linux changes are not referenced by `ChatServer.vcxproj`, so the new
`CMakeLists.txt` and the MySQL compatibility headers need no Windows
verification. However, three tracked **source** files were modified in §9, which
does require a Windows same-diff regression (`RedisMgr.h` changed).

Procedure executed per §31:

1. The permanent Windows checkout `G:\visualStudioCode\rtc-signaling-platform`
   was **not** altered. It was left at `df991a6` on `dev`.
2. A temporary **detached** git worktree was created from `df991a6` at
   `G:\rtc-step23-regression` (outside both real working trees).
3. Only the ignored prerequisites required by the existing Windows build were
   copied from the preserved checkout: `message.pb.cc/.h`,
   `message.grpc.pb.cc/.h`, `config.ini`, `ChatServer.vcxproj.user`,
   `mysqlcppconn-9-vs14.dll`, `mysqlcppconn8-2-vs14.dll`.
   Old build output (`x64/`) was **deliberately not copied**.
4. Only the three-file source diff was applied
   (`git apply` reported all three applied cleanly).
5. Authoritative `Debug|x64 Rebuild`:

```
G:\vs\MSBuild\Current\Bin\amd64\MSBuild.exe \
  G:\rtc-step23-regression\Server\ChatServer\ChatServer.sln \
  -p:Configuration=Debug -p:Platform=x64 -t:Rebuild
```

Result — **exact match with the recorded M0 Windows baseline**:

| Metric | Baseline | Regression |
|---|---|---|
| errors | 0 | **0** |
| warnings | 46 | **46** |
| executable | produced | `x64\Debug\ChatServer.exe` (19,609,088 bytes) |

Warning codes observed: `C4101`, `C4244`, `C4267`, `C4715` — the same
pre-existing set. The only two warnings that reference the modified headers
(`MysqlDao.h(151,16) C4267`, `RedisMgr.h(183,27) C4101`) are the pre-existing
warnings whose line numbers shifted by exactly +2 because of the insertions; no
new warning was introduced.

6. The temporary worktree was removed (`git worktree remove --force` +
   `git worktree prune`).
7. The preserved checkout was verified afterwards: still `HEAD = df991a6`, still
   on `dev`, `git worktree list` shows only the main worktree, and
   `git diff --ignore-cr-at-eol` is empty.

Note: the preserved Windows checkout reports 233 pre-existing modified entries.
These were independently confirmed to be **purely CRLF/LF line-ending noise** —
`git diff --ignore-cr-at-eol` is completely empty and the change counts are
perfectly symmetric. This condition pre-dates Step 2.3 and was neither caused
nor altered by it.

---

## 12. Executable Produced

```
/tmp/rtc-signaling-chatserver-build/ChatServer
```

```
ELF 64-bit LSB pie executable, x86-64, version 1 (SYSV), dynamically linked,
interpreter /lib64/ld-linux-x86-64.so.2,
BuildID[sha1]=ebeb39fef215f1274f2e17b74224a40e016e5a6b,
for GNU/Linux 3.2.0, with debug_info, not stripped
```

Size: 12,431,848 bytes. **Not executed.**

Notable `ldd` bindings (all from `/lib/x86_64-linux-gnu/`):
`libboost_filesystem.so.1.83.0`, `libprotobuf.so.32`, `libgrpc++.so.1.51`,
`libgrpc.so.29`, `libgpr.so.29`, `libupb.so.29`, `libmysqlcppconn.so.7`,
`libmysqlclient.so.21`, `libjsoncpp.so.25`, `libhiredis.so.1.1.0`,
`libabsl_*.so.20220623`, `libz.so.1`.

---

## 13. Files Changed

Created:

```
Server/ChatServer/ChatServer/CMakeLists.txt
Server/ChatServer/ChatServer/cmake/compat/mysqlcppconn/jdbc/mysql_driver.h
Server/ChatServer/ChatServer/cmake/compat/mysqlcppconn/jdbc/mysql_connection.h
Server/ChatServer/ChatServer/cmake/compat/mysqlcppconn/jdbc/cppconn/prepared_statement.h
Server/ChatServer/ChatServer/cmake/compat/mysqlcppconn/jdbc/cppconn/resultset.h
Server/ChatServer/ChatServer/cmake/compat/mysqlcppconn/jdbc/cppconn/statement.h
Server/ChatServer/ChatServer/cmake/compat/mysqlcppconn/jdbc/cppconn/exception.h
docs/phase2/STEP-2.3-minimal-linux-build-bootstrap.md
```

Modified (authorized in §9.3 of this document):

```
Server/ChatServer/ChatServer/ChatGrpcClient.h    +1 line
Server/ChatServer/ChatServer/MysqlDao.h          +2 lines
Server/ChatServer/ChatServer/RedisMgr.h          +2 lines
```

Not modified: any `.sln`, `.vcxproj`, `.vcxproj.filters`, `.props`, `.gitignore`,
`message.proto`, `start.bat`, `config.ini`, any generated protobuf/gRPC source,
ChatServer2, GateServer, StatusServer, Qt client, any Meeting source.

---

## 14. Tests

```
Unit tests: N/A
```

No unit-test infrastructure exists yet. The Linux build is a build
verification, not a unit test. No GoogleTest/Catch2 was added. Unit-test
infrastructure remains to be addressed after the Linux runtime baseline
decision and before Meeting state-machine implementation.

---

## 15. Scope Compliance

| Check | Result |
|---|---|
| Ubuntu-20.04 modified | **NO** |
| Runtime services installed/started | **NO** |
| ChatServer executed | **NO** |
| Ports bound | **NO** |
| Meeting work started | **NO** |
| Generated protobuf/gRPC files committed | **NO** |
| Windows project files modified | **NO** |
| Windows baseline checkout modified | **NO** |
| `message.proto` modified | **NO** |
| `.gitignore` modified | **NO** |
| Build artifacts inside the Git tree | **NO** |
| Docker / CI added | **NO** |

---

## 16. Baseline Status

```
Linux configure/compile/link path: ESTABLISHED
Linux authoritative runtime baseline: NOT ESTABLISHED
Windows Debug|x64 fallback baseline: PRESERVED
```

---

## 17. Remaining Risks Relevant to Step 2.4 Runtime Smoke

1. **No runtime verification at all.** The binary has never been started. Any
   runtime-only failure (config parsing, Redis/MySQL connect paths, gRPC channel
   setup, Boost.Asio endpoint binding) is still completely unverified.
2. **`config.ini` has not been created or reviewed for Linux.** `ConfigMgr.cpp`
   resolves it relative to `boost::filesystem::current_path()`, so the Linux
   runtime working directory determines which file is read.
3. **Redis and MySQL are not installed on Linux**, and no schema exists for the
   MySQL tables the DAO expects. Step 2.4 must provision both services.
4. **MySQL client/server version skew is untested.** The Linux build links
   Ubuntu's Connector/C++ 1.1.12 (`libmysqlcppconn.so.7`, `libmysqlclient.so.21`),
   while the Windows baseline used Connector 8.x/9.x runtime DLLs. The DAO uses
   the legacy 1.1 JDBC API, which compiled and linked, but no statement has ever
   been executed against a server.
5. **The inherited Windows `PATH` remains contaminated** with conda and other
   foreign prefixes (`/mnt/f/conda/...`, `/mnt/f/cppsoft/...`). Only `re2` was
   hijacked here, and that is now pinned for this target. Step 2.4 should expect
   the same class of hazard for any newly discovered package and should verify
   new dependencies resolve under `/usr`.
6. **GBK string literals are passed through unvalidated.** GCC 13.3 accepts them
   without diagnostic, but the emitted narrow string bytes are GBK, so any
   future console/UTF-8 output path may render them incorrectly. Not a build
   blocker.
7. **One pre-existing warning** (`MysqlDao::CheckEmail` missing return path)
   remains on both platforms and is a latent runtime risk.

---

## 18. Next Step

Proposed only — **not started**:

**Step 2.4 — Linux Runtime Smoke & Baseline Cutover Decision**

It should provision Redis and MySQL, create a Linux `config.ini`, run ChatServer,
and exercise a minimal smoke path before any baseline cutover.

**Do not begin Step 2.4 from this document.**
