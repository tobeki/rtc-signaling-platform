# STEP 2.4 — Linux Runtime Smoke & Server-Side Baseline Cutover Decision

Status: **COMPLETE — CUTOVER APPROVED**

---

## 1. Scope

This step validates the ChatServer executable that Step 2.3 produced, at
**runtime**, on `Ubuntu-24.04-RTC`.

The purpose is narrow: establish whether Linux is sufficiently proven to become
the authoritative **server-side development baseline** for subsequent Phase 2 /
Meeting engineering.

**In scope:** fresh build, dependency isolation, ephemeral Redis, temporary
synthetic config, process start, Redis startup registration, gRPC listener, TCP
listener, Linux-local connectivity, Windows → WSL connectivity, SIGTERM
handling, listener teardown, Redis cleanup on shutdown.

**Out of scope and not performed:** business / login / friend workflows; MySQL
Server installation; schema creation; DAO business paths; protocol-level IM or
gRPC operations; benchmark claims.

This is a **runtime smoke**, not a unit test and not a functional regression of
the legacy IM feature set.

---

## 2. Starting State

| Item | Value |
|---|---|
| Repository | `rtc-signaling-platform` |
| Working tree | `/home/tobeki/projects/rtc-signaling-platform` |
| Distro | `Ubuntu-24.04-RTC` (Ubuntu 24.04.5 LTS, Noble Numbat) |
| Branch | `dev` |
| Starting commit | `019021e65feebc5c2cbc6dec72ccb6054bf61b2b` |
| `origin/dev` at start | `019021e65feebc5c2cbc6dec72ccb6054bf61b2b` |
| Starting `git status --short` | clean (empty) |
| GCC / G++ | 13.3.0 |
| CMake | 3.28.3 |
| Entry baseline | Step 2.3: Linux configure/compile/link ESTABLISHED; Windows Debug\|x64 fallback PRESERVED |

`Ubuntu-20.04` was not touched, not used and not modified in any way.

---

## 3. Fresh Build

A brand-new build directory was used; the Step 2.3 build tree was not relied on.

```
rm -rf /tmp/rtc-step24-build
cmake -S Server/ChatServer/ChatServer \
      -B /tmp/rtc-step24-build \
      -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build /tmp/rtc-step24-build --parallel 2
```

| Stage | Result |
|---|---|
| configure | **PASS** (exit 0) |
| protobuf generation | **PASS** (`message.pb.cc/.h`) |
| gRPC generation | **PASS** (`message.grpc.pb.cc/.h`) |
| compile | **PASS** (0 errors) |
| link | **PASS** |
| executable | `/tmp/rtc-step24-build/ChatServer`, 12,431,792 bytes |

Warning count: **1**, the same pre-existing source property recorded in Step 2.3
(`MysqlDao.cpp:95: control reaches end of non-void function [-Wreturn-type]`).

`--parallel 2` was used; `-j$(nproc)` was not used.

---

## 4. Dependency Isolation

### 4.1 `/mnt` contamination audit — PASS

`compile_commands.json` contains **no** `/mnt/...` and no `conda` reference.

Resolved include flags across all 15 compilation units:

```
-I<repo>/Server/ChatServer/ChatServer
-I<repo>/Server/ChatServer/ChatServer/cmake/compat/mysqlcppconn
-I/tmp/rtc-step24-build/generated
-isystem /usr/include/hiredis
-isystem /usr/include/jsoncpp
```

The link line contains no `/mnt/...` and no `conda` reference; every library
resolves under `/usr/lib/x86_64-linux-gnu/`.

### 4.2 One residual cache entry — diagnosed, confined, accepted

`CMakeCache.txt` still records:

```
re2_DIR:PATH=/mnt/f/conda/Library/lib/cmake/re2
```

This is a **side effect of the Step 2.3 fix, not a build contamination**.
Because `CMakeLists.txt` now pre-defines the `re2::re2` imported target, CMake's
generated `re2Targets.cmake` returns early and never applies conda's
`INTERFACE_INCLUDE_DIRECTORIES`. gRPC's `find_package(re2)` nevertheless still
executes and writes its discovered `re2_DIR` into the cache.

The workspace's own resolution is the Ubuntu one:

```
CHATSERVER_RE2_INCLUDE_DIR:PATH=/usr/include
CHATSERVER_RE2_LIBRARY:FILEPATH=/usr/lib/x86_64-linux-gnu/libre2.so
```

Verified consequence: `re2_DIR` appears **only** in the cache; it reaches neither
`compile_commands.json` nor `link.txt`, and `ldd` resolves `libre2.so.10` from
`/lib/x86_64-linux-gnu/`. The gate's operative requirement (the ChatServer build
must not consume third-party headers/libraries from `/mnt`) is met.

See §6 for the confirmed design consequence of this condition.

### 4.3 Key resolved dependencies

| Dependency | Resolution |
|---|---|
| Boost (filesystem, random) | `/usr/lib/x86_64-linux-gnu/cmake/Boost-1.83.0`, `/usr/include` |
| protobuf | `/usr/include`, `/usr/lib/x86_64-linux-gnu/libprotobuf.so` (3.21.12) |
| gRPC | `/usr/lib/x86_64-linux-gnu/cmake/grpc` (1.51.1) |
| JsonCpp | `/usr/lib/x86_64-linux-gnu/cmake/jsoncpp`, `-isystem /usr/include/jsoncpp` |
| hiredis | pkg-config, `-isystem /usr/include/hiredis` |
| MySQL Connector/C++ | `/usr/include`, `/usr/lib/x86_64-linux-gnu/libmysqlcppconn.so` |
| re2 | `/usr/include`, `/usr/lib/x86_64-linux-gnu/libre2.so` |

### 4.4 `ldd` — PASS

- `not found`: **0**
- any runtime library under `/mnt/...`: **none**

Important resolutions (all `/lib/x86_64-linux-gnu/`):
`libboost_filesystem.so.1.83.0`, `libprotobuf.so.32`, `libgrpc++.so.1.51`,
`libgrpc.so.29`, `libgpr.so.29`, `libupb.so.29`, `libmysqlcppconn.so.7`,
`libmysqlclient.so.21`, `libjsoncpp.so.25`, `libhiredis.so.1.1.0`,
`libre2.so.10`, `libcares.so.2`, `libaddress_sorting.so.29`,
`libabsl_*.so.20220623`, `libssl.so.3`, `libcrypto.so.3`, `libz.so.1`,
`libzstd.so.1`, `libstdc++.so.6`, `libc.so.6`.

---

## 5. Redis Runtime

### 5.1 Package

Redis was not previously installed. Simulation before install:
`4 newly installed, 0 to remove, 0 upgraded`.

| Package | Version |
|---|---|
| `redis-server` | 5:7.0.15-1ubuntu0.24.04.4 |
| `redis-tools` | 5:7.0.15-1ubuntu0.24.04.4 |

Transitive packages added: `libjemalloc2` 5.3.0-2build1, `liblzf1` 3.6-4.

No `apt upgrade` / `apt full-upgrade` / `do-release-upgrade` was executed.

**Installation auto-started the system Redis service** (the package created and
enabled `redis.service`, which came up `active` on `127.0.0.1:6379`). Per the
step's instruction this was recorded, and the system instance was **stopped** so
that the dedicated ephemeral instance was the only Redis in play. The unit's
`enabled` state was left untouched; `/etc/redis/redis.conf` was **not** modified.

### 5.2 Ephemeral instance

Dedicated instance started from `/tmp/rtc-step24-runtime/redis.conf`, **not** the
system configuration:

```
bind 127.0.0.1
protected-mode yes
port 16379
daemonize no
supervised no
save ""
appendonly no
requirepass <REDACTED-SYNTHETIC>
dir /tmp/rtc-step24-runtime/redis-data
logfile /tmp/rtc-step24-runtime/redis.log
```

The password is a synthetic local-only value, not a real project credential, and
is redacted in this document. No password from any existing project config was
read or reused. Persistence was disabled (`save ""`, `appendonly no`) so no Redis
data file was produced; `redis-data/` remained empty.

Verification:

```
redis-cli -h 127.0.0.1 -p 16379 -a <redacted> --no-auth-warning PING
-> PONG
```

`INFO server` confirmed `redis_version:7.0.15`, `tcp_port:16379`,
`config_file:/tmp/rtc-step24-runtime/redis.conf`.

Before ChatServer start, `HGET logincount_lfh chatserver_step24` was **empty**,
proving the later value came from ChatServer and not from prior state.

---

## 6. Temporary ChatServer Config

The existing Windows `config.ini` was **not read and not copied**. A synthetic
temporary config was written to `/tmp/rtc-step24-runtime/config.ini`:

| Section | Keys | Intent |
|---|---|---|
| `[SelfServer]` | `Name=chatserver_step24`, `Host=127.0.0.1`, `Port=18080`, `RPCPort=15051` | identity + both listener ports |
| `[Redis]` | `Host=127.0.0.1`, `Port=16379`, `Passwd=<redacted synthetic>` | ephemeral Redis |
| `[PeerServer]` | `Servers=` (empty) | no peer routing |
| `[Mysql]` | `Host=127.0.0.1`, `Port=13306`, `User`/`Passwd`/`Schema=<synthetic placeholder>` | harmless placeholder; never connected |

The config is **not committed**; only its structure and intent are recorded here.

`ConfigMgr` resolves `current_path() / "config.ini"`, so ChatServer was started
with `/tmp/rtc-step24-runtime` as its working directory. The startup log confirms
this:

```
Config path: "/tmp/rtc-step24-runtime/config.ini"
```

Port pre-flight before start — all required ports free:

```
6379   free      (system Redis stopped)
16379  free
18080  free
15051  free
```

No preferred port had to be changed; no unrelated process was killed.

---

## 7. ChatServer Startup — all gates PASS

Started in the runtime workspace with stdout/stderr captured inside it.

| Gate | Evidence | Result |
|---|---|---|
| Process alive | `ps` showed `Ssl`, elapsed 01:15 | **PASS** |
| Redis startup registration | `HGET logincount_lfh chatserver_step24` -> `0`; `HGETALL` -> `chatserver_step24 / 0` | **PASS** |
| gRPC startup stage | log: `RPC Server listening on 127.0.0.1:15051` | **PASS** |
| gRPC listener | `LISTEN [::ffff:127.0.0.1]:15051` owned by `ChatServer` | **PASS** |
| TCP listener | `LISTEN 0.0.0.0:18080` owned by `ChatServer` | **PASS** |
| Redis connection pool | 10 established connections to `127.0.0.1:16379` | **PASS** |

The `HSET logincount_lfh chatserver_step24 0` registration was verified **by
reading the value back from Redis**, not merely by observing a log line.

Startup log progression (redacted):

```
Config path: "/tmp/rtc-step24-runtime/config.ini"
... config dump ...
<GBK redis AUTH success literals x10, one per pooled connection>
Execut command [ HSet logincount_lfh  chatserver_step24  0 ] success !
RPC Server listening on 127.0.0.1:15051
Server start success, listen on port : 18080
```

The ten GBK `认证成功` literals emitted at startup are the expected consequence of
the Step 2.3 Gate G1 Case A decision: GCC passes those bytes through unvalidated,
so they render as mojibake in a UTF-8 terminal. Cosmetic only; not a blocker.

**Observation (non-blocking):** `CServer` binds TCP via `tcp::v4()` and listens
on `0.0.0.0:18080`, while gRPC's `AddListeningPort("127.0.0.1:15051")` results in
the socket being reported as `[::ffff:127.0.0.1]:15051`. Both were reachable from
both Linux and Windows, so this did not affect the smoke. It is worth noting for
future hardening, since the gRPC socket is IPv6-mapped rather than plain IPv4.

---

## 8. Connectivity Gates

### 8.1 Linux-local — PASS

```
TCP 127.0.0.1:18080 -> CONNECTED (peer=('127.0.0.1', 18080))
TCP 127.0.0.1:15051 -> CONNECTED (peer=('127.0.0.1', 15051))
```

Only socket establishment was tested. No framed IM protocol data and no gRPC
call was sent.

### 8.2 Windows → WSL — PASS

```
Test-NetConnection -ComputerName localhost -Port 18080
    PORT 18080 TcpTestSucceeded = True
Test-NetConnection -ComputerName localhost -Port 15051
    PORT 15051 TcpTestSucceeded = True
```

Both listeners are reachable from Windows localhost. No firewall rule,
`.wslconfig`, `portproxy` or networking-mode change was made.

---

## 9. Graceful Shutdown — all gates PASS

State immediately before shutdown: process alive, both listeners present,
`HGET logincount_lfh chatserver_step24` -> `0`.

`SIGTERM` was sent (`kill -TERM`); `kill -9` was **not** used.

| Gate | Evidence | Result |
|---|---|---|
| Process exits normally | `ps` -> `PROCESS GONE`; `pgrep` -> empty | **PASS** |
| TCP listener disappears | `ss` -> no listener on 18080 | **PASS** |
| gRPC listener disappears | `ss` -> no listener on 15051 | **PASS** |
| Redis cleanup reached | `HGET logincount_lfh chatserver_step24` -> empty (nil); `HGETALL` -> empty; `EXISTS logincount_lfh` -> `0` | **PASS** |

The Redis cleanup evidence is the decisive proof that the shutdown path executed
`HDEL logincount_lfh <SelfServer.Name>` and that RedisMgr's pool shut down in an
orderly way — the hash no longer exists at all.

Shutdown log tail (redacted):

```
destruct MsgNode
Server destruct listen on port : 18080
~CSession destruct
destruct MsgNode
this is singleton destruct
this is singleton destruct
AsioIOServicePool destruct
this is singleton destruct
```

The signal handler path (`io_context.stop()` -> `pool->Stop()` ->
`server->Shutdown()`) completed without a crash, hang, or forced kill.

### 9.1 Ephemeral Redis teardown

The dedicated Redis instance was shut down gracefully
(`SHUTDOWN NOSAVE`). Afterwards: no listener on 16379, no `redis-server` process,
and all three smoke ports closed.

The system Redis unit was left `enabled` but `inactive`, matching the isolation
decision in §5.1.

---

## 10. MySQL Scope

MySQL Server was deliberately **not** installed and no schema was created.

Justification, verified in the source rather than assumed:

- `main()` only performs `RedisMgr::HSet(LOGIN_COUNT, server_name, "0")`, builds
  the gRPC server, installs the SIGINT/SIGTERM handler, and constructs `CServer`.
- `MysqlMgr::GetInstance()` / `MysqlDao` are referenced only in **request
  handling** paths (`ChatServiceImpl`, `LogicSystem`), never during startup.
- `ChatServiceImpl`'s constructor is empty; the DAO is only reached inside
  `GetBaseInfo` and the notification RPCs, which the smoke never invoked.

Therefore MySQL initialization is lazy and is not required by the
startup/listener path. The Connector/C++ library remains linked
(`libmysqlcppconn.so.7`, `libmysqlclient.so.21`) and loads successfully, but no
statement was ever executed against a server.

---

## 11. Cleanup

- ChatServer: not running.
- Ephemeral Redis: not running.
- System Redis: stopped (unit left enabled, unmodified config).
- Ports 16379 / 18080 / 15051: all closed.
- `/tmp/rtc-step24-runtime`: **deleted** (config, logs, pid files, redis data).
- Windows temp probe script: **deleted**.
- `/tmp/rtc-step24-build`: retained at the time of writing for metadata
  reference; it lives outside the repository.

Repository scope is respected — see §13.

---

## 12. Baseline Decision

All required runtime gates passed.

```
CUTOVER APPROVED
```

Resulting baseline state:

```
Linux ChatServer build/runtime-smoke baseline: ESTABLISHED
Linux server-side development baseline: AUTHORITATIVE
Windows Debug|x64 baseline: PRESERVED AS FALLBACK
MySQL-dependent legacy workflow parity: NOT VERIFIED
```

Meaning: future C++ unit-test infrastructure, Meeting Domain implementation, and
normal ChatServer server-side development should use `Ubuntu-24.04-RTC` as the
primary environment.

This does **NOT** mean:

- all historical IM functionality has been revalidated on Linux;
- MySQL-dependent legacy workflows are proven;
- Windows support is removed;
- cross-platform production portability is guaranteed.

---

## 13. Files Changed

Created:

```
docs/phase2/STEP-2.4-linux-runtime-smoke-and-baseline-cutover.md
```

No application source, header, `CMakeLists.txt`, `.sln`, `.vcxproj`, `.props`,
`.gitignore`, `message.proto`, generated protobuf/gRPC file, or existing config
file was modified. No runtime config, log, Redis data or build output is inside
the repository.

No runtime defect was discovered, so no repair was required and none was made.

---

## 14. Tests

```
Unit tests: N/A
```

No unit-test infrastructure exists. Runtime smoke is not a unit test. No
GoogleTest/Catch2 was added.

---

## 15. Scope Compliance

| Check | Result |
|---|---|
| Application source changed | **NO** |
| CMake changed | **NO** |
| Windows build files changed | **NO** |
| Preserved Windows checkout modified | **NO** |
| Ubuntu-20.04 modified | **NO** |
| Meeting work started | **NO** |
| Temporary config committed | **NO** |
| Generated protobuf/gRPC committed | **NO** |
| `apt upgrade` / distro upgrade | **NO** |
| `/etc/redis/redis.conf` modified | **NO** |
| Firewall / `.wslconfig` / portproxy changed | **NO** |
| MySQL Server installed | **NO** |
| Docker / CI added | **NO** |
| Build artifacts inside the Git tree | **NO** |

---

## 16. Remaining Gaps

Kept deliberately narrow:

1. **MySQL-dependent legacy workflows unverified** (login, friend, auth, chat
   history, user search). No server, no schema, no statement executed.
2. **No protocol-level validation.** Only socket establishment was proven; no IM
   frame was parsed and no gRPC method was invoked, so the request-handling path
   including `LogicSystem` is still untested at runtime.
3. **Windows smoke was port-reachability only**, not a Qt-client-to-server
   session. The Qt client was not built or run on either platform.
4. **No IPv6-mapped-versus-IPv4 loopback assertion.** gRPC listened as
   `[::ffff:127.0.0.1]:15051`; this worked everywhere tested but was not
   deliberately exercised over IPv6 or non-loopback interfaces.
5. **GBK log literals remain mojibake** in UTF-8 terminals (see §7). Cosmetic
   today; will matter if logs are consumed programmatically.
6. **`MysqlDao::CheckEmail` still lacks a return on all paths** — the single
   pre-existing warning, a latent runtime risk whenever MySQL paths are used.
7. **System Redis was auto-started and enabled by package installation.** It is
   currently stopped. This is an environment state change from package
   installation, not a project change.
8. **`re2_DIR` cache residue** pointing at the conda tree (§4.2). Harmless today
   because the explicit imported target wins, but it is fragile if that
   pre-definition is ever removed.

---

## 17. Next Step

Proposed only — **not started**:

**Phase 2 — Step 2.5: Linux C++ Unit-Test Infrastructure**

**Do not begin Step 2.5 from this document.**
