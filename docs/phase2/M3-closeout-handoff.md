# Phase 2 — M3 Closeout & M4 Handoff

本文件是 **M3 收尾 / M4 交接**状态记录，供新的 M4 Codex session 作为起步依据。
它只描述**已提交的事实**；本次生成过程中未修改任何已有的 `cpp` / `h` / `CMakeLists.txt` /
`docs`，也未创建 commit、未 push。

事实源优先级：current repository code → committed Phase 1 docs → committed Phase 2 / M1 / M2 / M2.1 / M3 docs。

---

## 1. 当前 commit

```text
branch        : dev
HEAD          : 57090d80bf99f36869c02db85905d19bcb6273d1
origin/dev    : 57090d80bf99f36869c02db85905d19bcb6273d1
ahead/behind  : 0 0
working tree  : CLEAN
index         : CLEAN（0 staged entries）
```

```text
subject   : feat(meeting): implement meeting service and registry
author    : 2026-10-11 23:16:18 +0800
committer : 2026-10-11 23:16:18 +0800
```

### 1.1 最近提交

```text
57090d8 (HEAD -> dev, origin/dev) feat(meeting): implement meeting service and registry
327cf1c fix(meeting): align aggregate events with phase1 contract
3e566df feat(meeting): implement pure meeting aggregate
8681ac7 feat(meeting): add domain value types
b94697a test(chatserver): establish Linux C++ unit test infrastructure
```

### 1.2 HEAD commit 内容

```text
 Server/ChatServer/ChatServer/CMakeLists.txt                  |   13 +-
 Server/ChatServer/ChatServer/meeting/MeetingAggregate.cpp    |  106 +-
 Server/ChatServer/ChatServer/meeting/MeetingAggregate.h      |   55 +-
 Server/ChatServer/ChatServer/meeting/MeetingService.cpp      | 1034 +
 Server/ChatServer/ChatServer/meeting/MeetingService.h        |  435 +
 Server/ChatServer/ChatServer/tests/CMakeLists.txt            |    1 +
 Server/ChatServer/ChatServer/tests/MeetingServiceTest.cpp    | 2623 +
 docs/phase2/M3-meeting-service-and-in-memory-registry.md     |  730 +
 8 files changed, 4979 insertions(+), 18 deletions(-)
```

`MeetingTypes.h` 未被 M3 修改。

---

## 2. 环境

```text
WSL distro : Ubuntu-24.04-RTC
repo       : /home/tobeki/projects/rtc-signaling-platform
distro     : Ubuntu 24.04.5 LTS
kernel     : 6.18.33.2-microsoft-standard-WSL2
g++        : 13.3.0 (Ubuntu 13.3.0-6ubuntu2~24.04.1)
cmake      : 3.28.3
GoogleTest : libgtest-dev (1.14.0) — 仅 BUILD_TESTING=ON 时需要
hiredis    : 1.2.0
C++ 标准   : C++14（cxx_std_14，CMAKE_CXX_EXTENSIONS OFF / ConformanceMode=true）
```

未访问、未修改：`Ubuntu-20.04`、旧 Windows authoritative/fallback checkout、`/mnt/c`、`/mnt/d`
下的任何项目副本。

---

## 3. 已完成模块

### 3.1 Phase 1 — design（COMPLETE，冻结）

```text
docs/phase1/STEP-1.1-meeting-mvp-spec.md
docs/phase1/STEP-1.2-domain-model.md
docs/phase1/STEP-1.3-state-machines.md
docs/phase1/STEP-1.4-api-contract.md
docs/phase1/STEP-1.5-core-sequences.md
docs/phase1/STEP-1.6-implementation-plan-and-test-strategy.md
```

其中的 ResultCode、API ordering、role permission、Join/Leave/Close 语义、event contract、
CLOSED semantics 均视为**冻结**，M4 不得自行修改。

### 3.2 Phase 2 — Linux engineering baseline（COMPLETE）

| Step | 文档 | 提交 |
| --- | --- | --- |
| Step 2.1 | `STEP-2.1-build-wiring-and-dependency-contract.md` | `b6f6448` |
| Step 2.2 | `STEP-2.2-linux-wsl-migration-feasibility-audit.md` | `df991a6` |
| Step 2.3 | `STEP-2.3-minimal-linux-build-bootstrap.md` | `019021e` |
| Step 2.4 | `STEP-2.4-linux-runtime-smoke-and-baseline-cutover.md` | `357fa33` |
| Step 2.5 | `STEP-2.5-linux-cpp-unit-test-infrastructure.md` | `b94697a` |

> 注：用户清单只要求确认 2.1–2.4；实际上 Step 2.5（Linux C++ unit-test infrastructure）也已
> 完成并提交，它是 M1–M3 全部单元测试得以运行的前提。

### 3.3 Meeting Core（M1–M3 COMPLETE）

| 里程碑 | 内容 | 提交 |
| --- | --- | --- |
| M1 | Meeting Domain Value Types | `8681ac7` |
| M2 | Pure `MeetingAggregate` | `3e566df` |
| M2.1 | Meeting Event Contract Correction | `327cf1c` |
| M3 | `MeetingService` / In-Memory Registry | `57090d8` |

已提交的实现文件：

```text
Server/ChatServer/ChatServer/meeting/MeetingTypes.h              340 lines   (M1)
Server/ChatServer/ChatServer/meeting/MeetingAggregate.h          343 lines   (M2 + M3 扩展)
Server/ChatServer/ChatServer/meeting/MeetingAggregate.cpp        667 lines   (M2 + M3 扩展)
Server/ChatServer/ChatServer/meeting/MeetingService.h            435 lines   (M3)
Server/ChatServer/ChatServer/meeting/MeetingService.cpp         1034 lines   (M3)
```

M3 交付的能力（对外接口已冻结，M4 只能消费，不应改变语义）：

```text
MeetingService(const ServerId&, MeetingServiceDependencies)      // ordinary object, 非 Singleton, 非 thread-safe

领域 Session 生命周期
  RegisterConnectedSession(session_id)
  AuthenticateSession(session_id, user_id)
  TryGetDomainSessionState(session_id, out_state)

六项正式 API（八步求值顺序）
  CreateMeeting(context)
  JoinMeeting(context, meeting_id)
  LeaveMeeting(context, meeting_id)
  CloseMeeting(context, meeting_id)
  QueryMeeting(context, meeting_id)
  ListParticipants(context, meeting_id)

内部 domain operations（不是外部 API）
  FinalizeClose(meeting_id)                  // 第二个 serialized turn
  SessionDisconnected(session_id)            // 0 / 1 / N fan-out

受控只读 / 维护
  LocalServerId()  HasActiveMeeting()  HasClosedSnapshot()
  ReverseBindingCountForSession()  EvictClosedSnapshot()
```

`MeetingServiceDependencies`（`std::function<MeetingId()>` / `std::function<EventId()>` /
`std::function<Timestamp()>`）是 M4 注入真实 ID 与 clock 的唯一入口。

### 3.4 测试基线（COMPLETE）

```text
ChatServerUnitTests = 168 tests / 24 test suites
  既有（Step 2.5 / M1 / M2 / M2.1） :  41
  M3 新增（MeetingServiceTest）     : 127
```

M3 测试组：Session 18 / Create 10 / Join 9 / Leave 13 / Close 13 / FinalizeClose 7 /
Query 9 / ListParticipants 7 / Locality 6 / Ordering 7 / Disconnect 14 / ReverseIndex 9 /
EventContract 5。

Step 1.6 §19 规划的 57 个语义用例（C1–C7 / J1–J9 / L1–L11 / D1–D8 / X1–X11 / Q1–Q6 / O1–O5）
全部覆盖。

---

## 4. 未完成模块

```text
M4  Unified Serialized Work Item          NOT STARTED   ← 下一步
M5  Transport Disconnect Bridge           NOT STARTED
M6  Response / Notification Adapter       NOT STARTED
```

未完成的功能面：

```text
LogicSystem 队列 / 统一 work item 分发            → M4
ChatServer 与 MeetingDomain 的链接                → M4（当前仍未链接）
CSession / CServer / 真实 Session 生命周期接入     → M5
TCP ingress 停止、稳定 disconnect context 捕获     → M5
响应编码 / 客户端通知发送（session_id → CSession）  → M6
Protocol V2 / message.proto 扩展 / TCP message ID → Phase 3
ClosedSnapshot retention / TTL / 淘汰策略          → Deferred（当前仅手工 EvictClosedSnapshot）
MySQL 依赖的 legacy 业务路径在 Linux 上的验证       → 未验证
```

---

## 5. M4 前置约束

来源：`STEP-1.6 §12 / §25.5`、`STEP-1.3 §20`、`STEP-1.5 §22`。

### 5.1 硬约束

```text
1. 只有一个 FIFO：Client command、SESSION_DISCONNECTED、FINALIZE_CLOSE 必须共享
   同一个逻辑串行化顺序，并且只有一个 consumer 决定 Meeting mutation 顺序。
2. 禁止三个独立 queue：
       external commands queue A / SessionDisconnected queue B / FinalizeClose queue C
3. 禁止为 internal work 编造 TCP msg_id、再包装成假 LogicNode。
4. Internal work item 只保存稳定 ID/value（SESSION_DISCONNECTED / FINALIZE_CLOSE
   不需要持有 CSession）。
5. External legacy message 可继续持有 CSession，保持与现有 LogicNode 的兼容。
6. 不做大规模 LogicSystem 重写：只新增 internal kind 与分发分支，可逐步迁移。
7. 推荐实现方向（Step 1.6 §12.4 方案 A）：一个轻量、显式 kind 的统一 LogicWorkItem，
   由一个 FIFO queue 消费。不使用 std::function<void()> task queue（无法断言队列内容）、
   不引入 polymorphic work item 基类。
```

### 5.2 M4 task card（`STEP-1.6 §25.5`）

| 项目 | 内容 |
| --- | --- |
| Goal | 在尽量少改现有 `LogicSystem` 的前提下，实现一个 FIFO work abstraction |
| Preconditions | M3 完成 ✅ |
| Files allowed | `LogicSystem.h` / `LogicSystem.cpp`；work item 相关新增头文件；两个工程文件 |
| Files forbidden | `message.proto`、`CSession.*`（除确有必要的极小改动）、Redis/MySQL 相关 |
| Domain invariants | Step 1.3 §21.1 的全部 16 条竞态裁决顺序 |
| Unit tests | Layer B：队列顺序用例 |
| Integration tests | 旧 IM 消息兼容性（R1 缓解措施） |
| Review focus | 仍然只有一个 worker 决定 Meeting mutation 顺序；不伪造 TCP msg_id；不建立多个独立业务 queue |
| Rollback boundary | 回退 `LogicSystem.*` 至 M3 后的状态；Meeting Core（M1–M3）不受影响 |
| 必须支持 | legacy external `LogicNode` / message、`SESSION_DISCONNECTED`、`FINALIZE_CLOSE` |
| 必须验证 | `Join → Close`、`Close → Join`、`Leave → Disconnect`、`Disconnect → Leave`、`Close → Close`、`BeginClose → later FinalizeClose` |

### 5.3 M4 必须落实的 M3 契约（消费方式）

```text
1. single-writer 保证：MeetingService 自己不加锁、不是 thread-safe object。
   M4 的 FIFO 必须保证所有 MeetingService mutation 串行调用。
2. CloseMeeting 与 FinalizeClose 是**两个独立的 work item / queue turn**：
   - CloseMeeting 返回 should_schedule_finalize == true 时，M4 必须向**同一** FIFO
     提交恰好一个 FINALIZE_CLOSE work item；
   - 不得在同一个 turn 内执行 BeginClose 后仍返回 ENDING；
   - 不得自动调用 FinalizeClose（M3 已刻意不在 CloseMeeting 内调用它）。
3. SessionDisconnected 必须在 ClearSession **之前**捕获稳定 context 并投递
   （R3；Step 1.6 §13）。
4. 事件与通知：MeetingService 只返回 event value 与 notification_session_ids，
   不执行任何发送。发送属于 M6。
5. 注入依赖：M4 需要为 MeetingService 提供真实的 next_meeting_id / next_event_id / now。
   M3 内部不使用 boost::uuids、system_clock::now()、全局序列号或 rand()。
6. ChatServer 需要真正链接 MeetingDomain（当前 CMake link line 中仍然没有
   libMeetingDomain）。
```

### 5.4 不得违反的顺序不变量（回归护栏）

```text
permission-before-idempotency      非 Host + CLOSED + Close → PERMISSION_DENIED
argument-before-lookup             空 meeting_id → INVALID_ARGUMENT，绝不 MEETING_NOT_FOUND
identity-before-state-exposure     从未参加者 + ENDING + Leave → NOT_PARTICIPANT
snapshot-publish-before-release    closed_snapshots_[id] = snapshot 先于 active_meetings_.erase(id)
session-closed-after-fanout        Domain Session 只在全部受影响 Meeting 清理完成后 → CLOSED
request_id ≠ idempotency key       同一 request_id 的两次 Create → 两个不同 Meeting
```

---

## 6. 当前 Linux baseline 状态

```text
Linux server-side development baseline      : AUTHORITATIVE
Linux ChatServer build baseline             : ESTABLISHED
Linux runtime-smoke baseline（Step 2.4）     : ESTABLISHED / CUTOVER APPROVED
Linux C++ unit-test infrastructure（Step 2.5）: ESTABLISHED
Meeting Core（M1–M3）                        : COMPLETE
```

### 6.1 本次 closeout 对 HEAD `57090d8` 的重新验证（out-of-tree，仓库零改动）

```text
configure : PASS   (cmake -S Server/ChatServer/ChatServer -B /tmp/... -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON)
build     : PASS   (ChatServerGeneratedProto / MeetingDomain / ChatServer / ChatServerUnitTests 全部 Built)
warnings  : 仅 MysqlDao.cpp:95 control reaches end of non-void function（既有，未修）
ctest     : 100% tests passed, 0 tests failed out of 168
直接运行   : 在无 config.ini 的空目录运行 ChatServerUnitTests → 168 tests / 24 suites PASS, exit 0
ldd       : not found = 0，/mnt 第三方路径 = 0
            （依赖仅 libstdc++ / libm / libgcc_s / libc / ld-linux / linux-vdso）
ChatServer link line 中是否含 libMeetingDomain : NO（接入属于 M4）
```

M3 会话中另行执行且通过的 gate（对同样的文件内容）：

```text
C++14 gate : g++ -std=c++14 -pedantic-errors -fsyntax-only（MeetingService.h 探针）→ exit 0
             g++ -std=c++14 -pedantic-errors -Wall -Wextra -c MeetingAggregate.cpp / MeetingService.cpp → exit 0
BUILD_TESTING=OFF : configure + build PASS；gtest 相关输出 0 行；GoogleTest 不是必需依赖
MeetingDomain 依赖隔离 : 无 grpc / protobuf / hiredis / mysql / jsoncpp / boost / pthread / asio 未定义符号；
                        target_link_libraries(MeetingDomain …) 不存在（不链接任何东西）
```

### 6.2 构建入口

```bash
cmake -S Server/ChatServer/ChatServer -B /tmp/<build> -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build /tmp/<build> --parallel 2
ctest --test-dir /tmp/<build> --output-on-failure

# 只要应用、不要 GoogleTest：
cmake -S Server/ChatServer/ChatServer -B /tmp/<build> -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=OFF
```

构建目录必须在仓库之外；仓库内不得出现 build dir、binary、`.o`、`.a`、log 或生成的 protobuf。

---

## 7. Windows fallback baseline 状态

```text
Windows Debug|x64 baseline : PRESERVED AS FALLBACK
```

来源：`STEP-2.4 §12`。含义：

- Windows + Visual Studio 2022 + `Debug|x64` 的构建路径**未被移除**，仍可作为 fallback；
- 但它不再是权威开发基线；未来 C++ 单元测试基础设施、Meeting Domain 开发与常规 ChatServer
  server-side 开发应使用 `Ubuntu-24.04-RTC`；
- Step 2.4 的 Windows 侧验证仅为 **端口可达性**（`Test-NetConnection` 端口 18080 / 15051 均
  `TcpTestSucceeded = True`），**不是** Qt 客户端到服务器的真实会话；Qt 客户端在两个平台上都
  未构建、未运行；
- `Server/ChatServer/ChatServer/CMakeLists.txt` 刻意不被 `ChatServer.vcxproj` 引用，
  以保证既有 Windows 基线不受 Linux 构建影响。

### 7.1 Meeting Core 与 Windows 工程文件的关系（重要交接事实）

```text
grep -rn meeting --include='*.vcxproj' --include='*.props'  →  无任何命中
```

即：`MeetingTypes.h`、`MeetingAggregate.h/.cpp`、`MeetingService.h/.cpp`、`MeetingServiceTest.cpp`
**都没有**加入 `ChatServer.vcxproj` / `PropertySheet.props`。

这是一个**有意的、当前仍开放**的差异：

- M1 / M2 / M3 的正式步骤均将 Linux CMake 作为权威入口，并明确禁止修改 `*.vcxproj` / `*.props`；
- 因此 Windows 工程目前看不到 Meeting Core，无法在 Windows 上编译它；
- `STEP-1.6 §21.2` 的 gate 规则（新增文件需进入 `vcxproj`）目前**处于未生效状态**，最终由
  哪次任务补齐 Windows 工程条目尚未决定。

M4 及以后若决定恢复 Windows 可编译性，需要一次性补齐这四个源文件与一个测试文件的工程条目，
并重新评估 R6。

仓库中同时存在 `Server/ChatServer2/`（另一套 ChatServer 副本，见 R8），M3 未触碰。

---

## 8. 已知风险

### 8.1 Step 1.6 §30 Risk Register（仍然有效）

| # | 风险 | 与 M4 的关系 |
| --- | --- | --- |
| R1 | 修改 `LogicSystem` queue 导致旧 IM 消息行为回归 | **M4 的首要风险**；缓解：保留 legacy `LogicNode` 路径、只新增 kind 分发、集成测试覆盖旧消息 |
| R2 | Meeting Domain 持有 `CSession` 导致循环引用/生命周期耦合 | M3 已用"只存稳定 ID"规避，M4 不得破坏 |
| R3 | `SessionDisconnected` 与 `ClearSession` 顺序错误导致无法 fan-out | M5 接入前 M4 必须先把捕获/投递顺序固定 |
| R4 | 使用 `UserMgr` 做 Meeting 多 Session 路由 | M3 反向索引与 `UserMgr` 无关，M4 不得引入该依赖 |
| R5 | `FinalizeClose` 直接执行导致 first Close 返回 `CLOSED` | M3 已结构上禁止（两 turn）；M4 必须真的排两个 turn |
| R6 | 新增文件未加入 `vcxproj` | 见 §7.1，当前仍未加入，状态开放 |
| R7 | 本机绝对依赖路径导致另一机器无法 build | Step 2.4 §4.2 已诊断 `re2_DIR` cache 残留指向 conda 树，当前因显式 imported target 覆盖而无害，但脆弱 |
| R8 | ChatServer / ChatServer2 双份维护 | 未解决；M3 未触碰 ChatServer2 |
| R9 | C++ 标准未显式声明导致能力假设错误 | 已由 `cxx_std_14` + `CMAKE_CXX_EXTENSIONS OFF` 固定 |
| R10 | 现有并发隐患被 Meeting 放大 | M4 引入统一 FIFO 时会直接触碰到这块 |

### 8.2 M3 会话中新发现并已修正的缺陷（记录以备回归）

1. **被拒绝的 Join 曾错误写入反向索引。**
   `JoinMeeting` 最初只按 `JoinResult::outcome` 判断是否登记 `BindingRef`，但聚合在 state gate
   （`ENDING` / `CLOSED` → `MEETING_STATE_REJECTED`）上提前返回时没有改写 `outcome`，它仍是
   `NEW_PARTICIPANT`。修正：必须以 `result_code == ResultCode::OK` **且** outcome 属三种成功值为准。
   由 `MeetingServiceJoin.J8JoinEndingMeetingIsRejected` 与
   `MeetingServiceReverseIndex.RejectedJoinDoesNotTouchIndex` 锁定。
   → 教训：**不要用只在成功路径上有意义的字段来判断是否发生 mutation。**

2. **授权失败路径曾泄露 `MeetingView`。**
   现已改为：`MeetingView` 只在授权通过（或幂等命中）时填充；`PERMISSION_DENIED` /
   `NOT_PARTICIPANT` 时保持默认空值。这是 M3 记录的本地实现裁决，M4/M6 编码响应前必须遵守，
   否则会向非成员泄露成员数与 `host_user_id`。

### 8.3 环境 / 运行时风险（Step 2.4 §16 遗留）

1. **MySQL 依赖的 legacy 工作流未验证**：无 server、无 schema、未执行任何语句。
2. **无协议级验证**：仅证明 socket 建立，未解析任何 IM 帧、未调用任何 gRPC 方法，
   因此包含 `LogicSystem` 的请求处理路径在运行时仍未被覆盖（M4 正好会触碰它）。
3. **Windows 侧仅端口可达**，非真实客户端会话。
4. **无 IPv6 / 非 loopback 断言**：gRPC 监听呈现为 `[::ffff:127.0.0.1]:15051`。
5. **GBK 日志字面量在 UTF-8 终端仍为 mojibake**（cosmetic，但会被程序化消费影响）。
6. **`MysqlDao::CheckEmail` 缺少全路径 return**（唯一既有告警，`MysqlDao.cpp:95`）；
   **不修** —— 与本阶段无关，保持原样，但属于潜在运行时风险。
7. **系统 Redis 被包安装自动启用**（当前已停止）：环境状态变化，不是项目变更。
8. **`re2_DIR` cache 残留**指向 conda 树（见 R7）。

### 8.4 M3 记录的设计细节（不是缺陷，但 M4 需要知道）

```text
EventId 分配粒度：SessionDisconnected fan-out 对每个受影响的 active Meeting 都会向注入来源
消耗一个 EventId，即使该次裁决最终不产生事件（Host 断线、ENDING 下的幂等清理）。
聚合不生成 ID；被消耗但未使用的 ID 不产生领域副作用。
M4 若要在测试中断言"第 k 个事件使用第 k 个 ID"，必须先记录生成序列。
```

---

## 9. M4 进入前必读文件

```text
docs/phase1/STEP-1.3-state-machines.md     §20（single-writer）、§21（Race Arbitration Matrix 16 条）、§23（I1–I12）
docs/phase1/STEP-1.4-api-contract.md       §13–§18（六项 API）、§19–§20（顺序与错误优先级）、§21–§26（事件 / 快照）
docs/phase1/STEP-1.5-core-sequences.md     §5（线程边界）、§13（Close 两 turn）、§21–§23（顺序与快照发布路径）
docs/phase1/STEP-1.6-implementation-plan-and-test-strategy.md
                                           §6.5 / §11 / §12 / §13 / §16 / §17 / §25.5 / §30
docs/phase2/M3-meeting-service-and-in-memory-registry.md      §3 / §8 / §13 / §15 / §22
Server/ChatServer/ChatServer/meeting/MeetingService.h          （对外契约与注入点）
Server/ChatServer/ChatServer/LogicSystem.h / .cpp              （M4 的改造对象）
```

---

## 10. 验证记录（本次只读 closeout）

```text
git branch --show-current                  → dev
git fetch origin                           → 无新对象
git rev-parse HEAD                         → 57090d80bf99f36869c02db85905d19bcb6273d1
git rev-parse origin/dev                   → 57090d80bf99f36869c02db85905d19bcb6273d1
git rev-list --left-right --count origin/dev...HEAD → 0 0
git status --short                         → （空）
git diff --cached --stat                    → （空）
git log --oneline -5                        → 见 §1.1
fresh out-of-tree build + ctest             → PASS / 168 passed, 0 failed
直接运行 ChatServerUnitTests（空目录）        → exit 0
ldd（not found / /mnt）                      → 0 / 0
```

本次 closeout：

```text
修改的已有文件 : 无（cpp / h / cmake / docs 全部未改）
新增文件       : docs/phase2/M3-closeout-handoff.md
commit         : 未创建
push           : 未执行
```

---

## 11. 结论

```text
M3 CLOSED
Meeting Core M1–M3: COMPLETE
HEAD == origin/dev == 57090d8, working tree clean

M4 READY（下一步：M4 — Unified Serialized Work Item）
```

M4 **不要**从本文件直接开始编码：必须以正式的 M4 Coding Prompt 为准，并按其中的 Required Reading
清单重新阅读对应文档。
