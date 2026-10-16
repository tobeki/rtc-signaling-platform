# Phase 2 - M6：Response / Notification Adapter

> 状态：**PLANNING / DESIGN DRAFT（尚未 Coding，禁止据此直接开工）**
> 基线：`dev`，起点 `da19189a55ab13058a22c38c228d3043df72bdf6`（HEAD == origin/dev，working tree clean）
> 本文档为 M6 的**实施设计**，不含任何生产代码改动。M6 Coding 需先经人工 Review 本文档。
> 本文档由 M6 Planning 阶段产出，创建于 working tree，**未提交、未推送**（提交命令见 §20）。

---

## 1. Status

| 项 | 值 |
| --- | --- |
| 本步性质 | Planning / Design（**不是** Coding） |
| 起点 HEAD | `da19189a55ab13058a22c38c228d3043df72bdf6` == `origin/dev` |
| working tree / index | clean（本步开始时） |
| 前置里程碑 | M1 / M2 / M2.1 / M3 / M4 / M5 / M5.1 全部 COMPLETE |
| 本步修改的生产/测试/构建文件 | **0** |
| 本步新增文件 | 仅本文档 |
| M6 Coding | **NOT STARTED** |
| commit / push | **NO** |

### 1.1 本步的依据

```text
docs/phase1/STEP-1.6-implementation-plan-and-test-strategy.md
  §7   C++ constraints
  §12  统一 FIFO / work item
  §15  Adapter / session lookup / notification 约束
  §16  Close 两 turn 与 notification 约束
  §17  test layering（Layer A/B/C/D）
  §25.7 M6 Task Card
  §26  dependency DAG
  §27  Traceability（第 8/9/10/12 项直接指向 M6）
  §29  File Impact Map
  §30  Risk Register
  §31  Phase 2 Engineering Infrastructure Backlog
docs/phase2/M3-closeout-handoff.md（§8.2 MeetingView 安全裁决、§5 M4 契约）
docs/phase2/M4-unified-serialized-work-item.md（§4 观测接口、§10.2 notification 只是意图、§17 R6）
docs/phase2/M5-transport-disconnect-bridge.md（§13.1、§14、§15）
docs/phase2/M5.1-domain-session-lifecycle-integration.md（§6 FIFO 顺序、§7 retention Deferred、§11 Windows）
```

事实源优先级：**current repository code → committed Phase 1 docs → committed Phase 2 / M1–M5.1 docs**。
本文档中所有"当前事实"均由本轮对 `da19189` 的只读代码核对得出，并给出文件与行号。

---

## 2. Scope

### 2.1 目标（来自 STEP-1.6 §25.7）

```text
Goal                ：实现领域结果与当前 ChatServer 调用侧的适配能力
实现范围            ：MeetingRequestContext builder；
                      session_id → active CSession lookup；
                      local notification intent；
                      missing Session best effort
Integration tests   ：Layer C —— MeetingRequestContext 构造、session_id lookup、
                      notification target disappearing
Review focus        ：MeetingService 不直接持有 CServer；adapter 层承担 lookup
Domain invariants   ：通知失败不回滚状态；ENDING → CLOSED 必然收敛
Rollback boundary   ：回退 adapter 文件与相关改动
Resume value        ：领域结果可回到调用侧
```

### 2.2 本步（Planning）范围

```text
只读核对 M6 的真实代码接缝
设计 adapter boundary / response path / notification path / session lookup
设计 best-effort 语义、安全边界、顺序证明、ownership
设计测试策略（含如何避免复制生产分发逻辑、如何建立 happens-before）
产出 Scope Matrix、Known Risks、Deferred Work、Acceptance Criteria、Implementation Plan
```

### 2.3 本步明确不做

```text
不修改生产代码 / 测试代码 / CMake / *.sln / *.vcxproj / *.props / *.filters
不修改 message.proto / meeting/*
不修复既有 flaky（LogicWorkQueueTest.cpp:1135 与 :681）
不处理 Windows R6
不添加 tombstone TTL / LRU / timer / background cleanup
不 git add / commit / push
不开始任何 M6 实现
```

### 2.4 全部授权范围的唯一产物

```text
NEW: docs/phase2/M6-response-notification-adapter.md   （本文档）
```

**若 M6 Coding 需要超出 §15 Scope Matrix 中 MUST / MAY 的文件集合，必须在 Coding 前单独裁决，不得自行扩大。**

---

## 3. Current Architecture

### 3.1 执行上下文（M4 / M5.1 冻结事实，本轮复核）

```text
LogicSystem（Singleton<LogicSystem>）
  ├── std::unique_ptr<SerializedWorkExecutor<LogicWorkDispatcher>> _work_executor   LogicSystem.h:90
  └── std::unique_ptr<LogicWorkDispatcher> _dispatcher                              LogicSystem.h:94

SerializedWorkExecutor<THandler>（唯一队列 + 唯一消费者）        LogicWorkDispatcher.h:282
  ├── LogicWorkQueue queue_                                      LogicWorkDispatcher.h:574
  ├── std::thread worker_                                        LogicWorkDispatcher.h:295 / :578
  └── DispatchOutsideLock(item)  ← 队列锁之外                    LogicWorkDispatcher.h:548-572

LogicWorkDispatcher（MeetingService 的唯一 owner）               LogicWorkDispatcher.h:606
  ├── meeting::MeetingService meeting_service_                   LogicWorkDispatcher.h:652
  └── IWorkObservationSink* sink_                                LogicWorkDispatcher.h:653
```

惰性启动（本轮的**关键**事实）：`_dispatcher` 与 `_work_executor` 都在
`LogicSystem::EnsureWorkQueueStarted()` 的 `std::call_once` 内构造，**生产传入的 sink 是 `nullptr`**：

```cpp
// LogicSystem.cpp:409-421
void LogicSystem::EnsureWorkQueueStarted() {
        std::call_once(_work_start_once, [this]() {
                _local_server_id = "local-chatserver";
                _dispatcher.reset(new LogicWorkDispatcher(
                        _local_server_id, MakeSystemMeetingWorkDependencies(), nullptr));   // ← sink = nullptr
                _work_executor.reset(new SerializedWorkExecutor<LogicWorkDispatcher>(*_dispatcher, this));
        });
}
```

`EnsureWorkQueueStarted()` 的五个调用点全部是 typed 入队入口：
`LogicSystem.cpp:443`（`PostMsgToQue`）、`:456`（`PostSessionDisconnectedWork`）、
`:466`（`PostSessionRegisterWork`）、`:477`（`PostSessionAuthenticateWork`）、
`:483`（`PostMeetingCommandWork`）。因此**唯一 FIFO worker 的创建时刻 = 第一次真实入队时刻**（M5.1 §10 用它作为 accept→REGISTER 的运行时证据）。

### 3.2 四条 internal ingress 与一条 external ingress（M5.1 冻结，本轮复核）

```text
CServer::HandleAccept 成功分支  → LogicSystem::PostSessionRegisterWork      CServer.cpp:42
LogicSystem::LoginHandler 成功尾部 → PostSessionAuthenticateWork            LogicSystem.cpp:130
CSession 终止（7 条路径）        → SessionDisconnectBridge → PostSessionDisconnectedWork
                                                                            CSession.cpp:88/146/160
DispatchClose（should_schedule_finalize）→ PostInternalWork → 同一 FIFO      LogicWorkDispatcher.cpp:275-277
legacy IM 消息                   → PostMsgToQue → EXTERNAL_MESSAGE          CSession.cpp:201
```

### 3.3 领域输出的当前类型与归属（逐项，含行号）

| 输出 | 类型定义 | 生产者 | 当前消费者 | 当前是否编码 | 当前是否发送 |
| --- | --- | --- | --- | --- | --- |
| `MeetingRequestContext` | `MeetingTypes.h:188-201` | **无生产构造者**（只有测试 `MakeContext`） | `MeetingService` 六项 API | 否 | 否 |
| `MeetingServiceResultCommon` | `MeetingService.h:119-128` | `MeetingService::MakeCommon` | `LogicWorkDispatcher::MakeCommandOutcome` | 否 | 否 |
| `CreateMeetingResult` | `MeetingService.h:145-151` | `MeetingService::CreateMeeting` | `DispatchCreate` 摘要 | 否 | 否 |
| `JoinMeetingResult` | `MeetingService.h:153-160` | `MeetingService::JoinMeeting` | `DispatchJoin` 摘要 | 否 | 否 |
| `LeaveMeetingResult` | `MeetingService.h:162-168` | `MeetingService::LeaveMeeting` | `DispatchLeave` 摘要 | 否 | 否 |
| `CloseMeetingResult` | `MeetingService.h:170-183` | `MeetingService::CloseMeeting` | `DispatchClose` 摘要 | 否 | 否 |
| `QueryMeetingResult` | `MeetingService.h:185-188` | `MeetingService::QueryMeeting` | `DispatchQuery` 摘要 | 否 | 否 |
| `ListParticipantsResult` | `MeetingService.h:190-196` | `MeetingService::ListParticipants` | `DispatchListParticipants` 摘要 | 否 | 否 |
| `FinalizeCloseResult` | `MeetingService.h:201-220` | `MeetingService::FinalizeClose` | `DispatchWork` 的 `FINALIZE_CLOSE` 分支 | 否 | 否 |
| `SessionDisconnectedResult` | `MeetingService.h:226-240` | `MeetingService::SessionDisconnected` | `LogicWorkDispatcher::SessionDisconnected` | 否 | 否 |
| `MeetingView` | `MeetingTypes.h:209-221` | `MeetingAggregate::BuildMeetingView` / `MeetingService::BuildClosedMeetingView` | 仅被摘要为 `meeting_state` / `has_closed_at` / `active_participant_count` | 否 | 否 |
| `ParticipantView` | `MeetingTypes.h:229-240` | `MeetingAggregate` | **无**（`ListParticipantsResult.participants` 在 dispatcher 内被丢弃） | 否 | 否 |
| `MeetingEvent` | `MeetingTypes.h:301-338` | `MeetingAggregate` / `MeetingService` | 仅被降级为 `WorkDomainEvent`（type + ids） | 否 | 否 |
| `notification_session_ids` | `MeetingService.h:219` | `MeetingService.cpp:945`（唯一赋值点） | `RecordFinalizeClose` 的**观测**参数 | 否 | **否，永远为 intent** |
| `ClosedMeetingSnapshot` | `MeetingTypes.h:269-283` | `MeetingService::FinalizeClose` | 仅 `has_snapshot` 布尔被观测 | 否 | 否 |

### 3.4 三个决定 M6 形态的关键事实

**(F1) 生产侧没有任何 notification 投递路径。**
`notification_session_ids` 的唯一赋值点是 `MeetingService.cpp:945`（`result.notification_session_ids = context.notifiable_session_ids;`），
唯一的下游读者是 `LogicWorkDispatcher.cpp:162` 把它交给 `sink_->RecordFinalizeClose(...)`；
而生产 `sink_` 是 `nullptr`（`LogicSystem.cpp:417`）。因此当前链路精确地是：

```text
notification_session_ids → intent → null
```

`IWorkObservationSink` 的**全部实现都在 tests/ 内**（`LogicWorkQueueTest.cpp:64`、
`DomainSessionLifecycleTest.cpp:138`、`SessionDisconnectBridgeTest.cpp:109`），
生产侧零实现。这是一个"接口存在但生产无实现、且生产路径被 `nullptr` 短路"的结构缺口，**不是 bug**，是 M4 刻意的验证边界（M4 §4.5：生产注入 `nullptr`，该路径零开销）。

**(F2) 观测摘要不足以承载 response。**
`MeetingCommandOutcome`（`LogicWorkItem.h:243-267`）只携带
`command / result_code / result_name / disposition / has_meeting_id / meeting_id /
meeting_state / has_closed_at / active_participant_count / event_types`。
它在各 `Dispatch*` 中由 `result.meeting` **逐字段拷贝**得到（`LogicWorkDispatcher.cpp:209-211, 224-226, 239-241, 259-261, 287-289, 301-303`）。
因此 `meeting_id` 之外的所有 `MeetingView` 字段在**授权失败**时保持默认值——这正是 M3 的安全裁决，
但它同时意味着：**adapter 若只消费 `MeetingCommandOutcome`，就不可能正确编码
`ParticipantView`（`ListParticipants` 的成员列表被 dispatcher 完全丢弃），也无法获得
`MeetingEvent` 的完整内容（只有 `event_type`）。**

**(F3) 领域结果类型本身是完整的、值语义的、可安全复制的。**
六项 API 的 result struct 与 `FinalizeCloseResult` / `SessionDisconnectedResult`
都只含值类型（`std::string` / `std::vector` / POD），不含指针、不含 `CSession`。
因此 adapter **可以直接消费 typed result struct 本身**，无需经过观测摘要层。

**(F4) `CServer` 没有任何 session lookup 能力，且 `_sessions` 是私有的。**

```cpp
// CServer.h:20-24
boost::asio::io_context& _io_context;
short _port;
tcp::acceptor _acceptor;
std::map<std::string, std::shared_ptr<CSession>> _sessions;   // private
std::mutex _mutex;
```

`CServer` 的公开面只有构造、析构、`ClearSession`、以及一个 `accept` 私有路径。
`_sessions` 的访问纪律（STEP-1.6 §3.3 记录、本轮复核）为：

```text
insert  : 持 _mutex（CServer.cpp:31-34，M5.1 加入）
erase   : 持 _mutex（CServer.cpp:67-70）
find    : 未持锁（CServer.cpp:60 ClearSession 内部）
```

**(F5) `CSession` 已提供 adapter 所需的全部只读/发送原语，且 `Send` 自带互斥。**

```cpp
std::string& GetSessionId();                 CSession.h:28
int GetUserId();                             CSession.h:30
void Send(char* msg, short max_length, short msgid);   CSession.h:33
void Send(std::string msg, short msgid);               CSession.h:34
```

`CSession::Send` 在 `_send_lock` 下工作、通过 `SharedSelf()` 延长生命周期、使用
`boost::asio::async_write`（Asio 保证同一 socket 上并发 `async_write` 安全），
队满时**静默丢弃**（`MAX_SENDQUE`，`CSession.cpp:44-58`）。
`_send_que` 满 / 写失败的处理是 `HandleWrite` 中 `Close()`（`CSession.cpp:245-249`）。

**(F6) `ChatServer.cpp` 是天然的 composition root，且 `CServer` 的构造早于任何 work。**
`ChatServer.cpp:50` `CServer s(io_context, atoi(port_str.c_str()));` →
`CServer` 构造 `StartAccept()` → `io_context.run()`（`:51`）。
`LogicSystem` 的 worker 只在第一次入队时惰性创建（F1 前的 §3.1）。
因此"在 `io_context.run()` 之前完成 adapter 的依赖注入"在**时序上是可行且可证明的**。

---

## 4. Problem Statement

M6 要解决的是四个**互不相同**的问题，必须先分开陈述，否则会把它们混成一个"接个线"的任务。

### 4.1 P1：生产侧没有 notification 投递路径（F1）

`notification_session_ids` 是 M3 冻结的**意图**输出，M4 从未把它变成行为。
M6 必须在不改领域语义的前提下，给它一条真实的、唯一的、可测试的生产路径。
这不是"加一个 if"，而是要回答**它在哪一个 serialized boundary 之后发生**（§11）。

### 4.2 P2：没有 session lookup（F4）

领域只认识 `session_id`，而"把 session_id 变成可发送的连接"是 adapter 职责
（STEP-1.6 §15.1/§15.3 明令 `MeetingService` 不得持有 `CServer`）。
当前 `CServer::_sessions` 私有且无查询接口，因此 M6 必须提供一个**最小、非拥有、
线程安全**的查找能力，且不得顺手重写 `ClearSession` 的既有锁缺陷（R10 / STEP-1.6 §13.3）。

### 4.3 P3：没有 response 编码面，且"response"的边界在 Protocol V2 之前是模糊的

STEP-1.6 §25.7 的实现范围要求"领域结果可回到调用侧"，但
§25.8 同时把 `TCP message ID` / `JSON` / `protobuf` / `Frame V2` / `request_id encoding`
明确划为 Deferred；§25.7 的条件说明进一步允许
"若外部 Meeting command 尚无正式 Protocol V2，可只通过测试 adapter / direct invocation 验证"。

**因此 M6 的 response 面必须被削到这个精度**：定义并实现
"领域结果 → 可投递的 response 值对象/载荷 → 交给一个由调用方提供的出站通道"，
而**不**定义字节布局、**不**分配 TCP message ID。这是本文档 §6 与 §16.5 的核心裁决。

### 4.4 P4：`MeetingRequestContext` 的唯一生产构造点缺失

六项 API 只接受由 transport adapter 构造的 `MeetingRequestContext`
（`MeetingService.h:277-279`），但**生产侧没有任何地方构造它**
（`PostMeetingCommandWork` 在生产零调用点，仅测试调用）。
M6 必须提供 builder，并明确它的调用者**在 Protocol V2 落地前同样不存在**
（唯一合法的外部入口是 wire adapter，而那是 Phase 3）。

### 4.5 由此得到的 M6 准确定位

```text
M6 = 把"领域结果 → 调用侧"的适配能力做成**已实现、已注入、已被真实消费者调用、
     可被 Layer C 测试确定性驱动**的一层；
     但它的**最外层出口（字节、message ID、帧）仍然留在 Phase 3**。
```

---

## 5. Adapter Boundary

### 5.1 候选方案比较

| 方案 | 结构 | 优点 | 缺点 | 结论 |
| --- | --- | --- | --- | --- |
| A. adapter 作为 `LogicWorkDispatcher` 的成员 | `LogicWorkDispatcher` 内含 `MeetingResponseNotificationAdapter` | 最少新类型；直接访问 typed result | dispatcher 头文件被迫依赖 adapter 头与 session-lookup 头；单测 target 要拉入 adapter 实现；职责混杂 | 否 |
| B. adapter 通过**抽象回调接口**注入 dispatcher | dispatcher 只认识一个纯接口；真实现独立 TU | dispatcher 头零新增重量依赖；可注入 recording 替身而不复制生产逻辑；rollback 边界清晰 | 多一个接口类型 | **推荐** |
| C. adapter 作为 `IWorkObservationSink` 的生产实现 | 新 sink 实现替代 `nullptr` | 零新接口 | **违反 M4 §4.5**：`IWorkObservationSink` 契约明写"实现方不得修改领域状态、**发送响应**或触发通知"；且摘要不含 `ParticipantView`/完整 `MeetingEvent`（F2） | 否 |
| D. adapter 独立成 service，由独立 FIFO/timer 驱动 | 第二队列/第二 worker | "解耦" | 直接违反 STEP-1.6 §12.1（禁止第二 FIFO）与 M4 §5.1（禁止第二 worker）；破坏"顺序即裁决" | 否 |
| E. 领域层直接回调 adapter | `MeetingService` 依赖 adapter | 少一层 | 违反 STEP-1.6 §6.1 依赖方向与 §15.3（领域不得持有 `CServer`） | 否 |

### 5.2 裁决（推荐方案 B）

```text
LogicWorkDispatcher（消费者线程）
        │  持有裸指针 IMeetingResponseNotificationAdapter*（可为 nullptr）
        ▼
IMeetingResponseNotificationAdapter            ← 抽象接口，落点 LogicWorkDispatcher.h 之前的新头
        │  实现在 adapter TU 内
        ▼
MeetingResponseNotificationAdapter             ← 生产实现（新文件）
        │  持有 IMeetingSessionDirectory*（借用的**窄接口**，不是 CServer*）
        ▼
CServer（唯一生产实现；adapter 从不 include CSession.h）
        │  FindSessionSendHandle(session_id) → IMeetingOutboundChannel*
        ▼
CSession::Send(payload, owner-doc placeholder msg_id)     best-effort，never throws
```

三条硬性结构约束（M6 必须成立，且可被静态复核）：

```text
adapter 不认识 meeting::MeetingAggregate / registry / LogicWorkQueue
adapter 不持有 MeetingService、不调用任何领域 mutation
MeetingService 不持有 CServer、不认识 adapter、不认识 session
```

### 5.3 为什么依赖方向必须是"dispatcher → 接口 ← 实现"

`LogicWorkDispatcher.cpp` 刻意只依赖 MeetingDomain + 标准库 + Boost.UUID，并
**明确不 include `CSession.h` / asio**（`LogicWorkDispatcher.cpp:10-12` 注释）。
若 adapter 作为成员内联，adapter 的 `IMeetingSessionDirectory` 依赖会经头文件传递，
把 asio / Boost 拖进 `LogicWorkQueueTest.cpp` 所在的单测 target
（该 target 刻意排除 `LogicSystem.cpp` / `CServer.cpp` / `CSession.cpp`，见 `tests/CMakeLists.txt` 顶部注释）。
方案 B 让 dispatcher 只多认识一个**不含第三方 include** 的纯接口头，从而保住该边界。

### 5.4 adapter 是否允许持有 `CServer`

**不允许持有 `CServer` 类型。** 它只持有一个**窄接口** `IMeetingSessionDirectory*`：

```cpp
// 新头：MeetingOutboundChannel.h（只含标准库）
class IMeetingOutboundChannel {
public:
    virtual ~IMeetingOutboundChannel() {}
    // best-effort：不得抛出、不得阻塞消费者、失败不改变任何领域状态
    virtual bool SendToSession(const std::string& session_id,
                               short owner_document_message_id,
                               const std::string& payload) = 0;
};

class IMeetingSessionDirectory {
public:
    virtual ~IMeetingSessionDirectory() {}
    // 非拥有、线程安全。目标不存在 ⇒ 返回 nullptr（best-effort skip）
    virtual IMeetingOutboundChannel* FindSessionSendHandle(const std::string& session_id) = 0;
};
```

理由：
1. `CServer.h` 会引入 `boost/asio.hpp` 与 `CSession.h`（后者又引入 asio/beast/`const.h`/`MsgNode.h`）；
   adapter 只需要"按 id 找一个能发字节的东西"，不需要 `acceptor` / `io_context` / `_port`；
2. 窄接口让 Layer C 测试可以注入一个**纯记录型** directory + channel，
   从而在不建立真实 socket 的前提下断言"发了什么、发给了谁、目标消失时的行为"，
   且**不复制任何生产逻辑**（STEP-1.6 §17.3 的要求）；
3. 它把"adapter 是否持有 CServer"这个问题变成一个**类型层面可检查**的事实：
   `grep -n "CServer" MeetingResponseNotificationAdapter.h` 必须为 0 命中。

---

## 6. Response Path

### 6.1 语义定位（先说清"response 是什么"）

```text
Response   ：某个**外部命令**的直接请求-应答结果。
             唯一 target = 发起该命令的那个 session（命令来自哪条 session，响应回到哪条）。
             每个命令恰好 0 或 1 个 response。
             request_id 用于 correlation，**不是**幂等键。
Notification：某个**领域状态变化**需要告知的**其他** session 集合。
             与任何单一请求无关；target 是领域输出的 session_id 集合。
             数量为 0..N，允许目标不存在（best-effort skip）。
```

冻结依据：STEP-1.6 §15.1（领域只产生 session_id + intent）、§15.4（发送失败不回滚）、
§16.1（`FinalizeClose` **不是**外部 API，**无客户端 Response**）。

### 6.2 哪些结果有 response，哪些没有

| 领域操作 | 有 response? | 依据 |
| --- | --- | --- |
| `CreateMeeting` | ✅ 有 | 六项外部 API 之一（STEP-1.6 §25.7 实现范围） |
| `JoinMeeting` | ✅ 有 | 同上 |
| `LeaveMeeting` | ✅ 有 | 同上 |
| `CloseMeeting` | ✅ 有（**turn 1**，返回 `ENDING` + `CLOSE_STARTED`） | STEP-1.6 §16 |
| `QueryMeeting` | ✅ 有 | 同上 |
| `ListParticipants` | ✅ 有（含 `participants`） | 同上 |
| `FinalizeClose` | ❌ **无** | STEP-1.6 §16.1："不是外部 API，无客户端 Response" |
| `SessionDisconnected` | ❌ 无 | 内部 work，无请求方 |
| `RegisterConnectedSession` / `AuthenticateSession` | ❌ 无 | internal lifecycle，非客户端请求 |

### 6.3 response 值对象（M6 新增，**不是** wire encoding）

```text
MeetingCommandResponse
  ├── command            : ClientMeetingCommandKind      （既有枚举复用，不新增编码枚举）
  ├── request_id         : RequestId                     （correlation only）
  ├── result_code        : ResultCode                    （复用冻结的 15 个值）
  ├── result_name        : ResultCodeName()              （唯一来源，不维护第二套字符串映射）
  ├── disposition        : ResultDisposition             （SUCCESS / IDEMPOTENT / ERROR）
  ├── has_owner_chat_server_id / owner_chat_server_id    （仅 MEETING_NOT_LOCAL 携带）
  ├── has_view / view    : MeetingView                   （受 §10 安全规则约束）
  └── (ListParticipants) has_participants / participants : std::vector<ParticipantView>
```

### 6.4 关键裁决：adapter 消费 **typed result struct**，不消费 `MeetingCommandOutcome`

**裁决**：`MeetingResponseNotificationAdapter` 的入口方法按领域操作重载/分派，
直接接收 M3 的 typed result：

```cpp
virtual void OnCreateMeeting(const meeting::CreateMeetingResult&) = 0;
virtual void OnJoinMeeting(const meeting::JoinMeetingResult&) = 0;
virtual void OnLeaveMeeting(const meeting::LeaveMeetingResult&) = 0;
virtual void OnCloseMeeting(const meeting::CloseMeetingResult&) = 0;
virtual void OnQueryMeeting(const meeting::QueryMeetingResult&) = 0;
virtual void OnListParticipants(const meeting::ListParticipantsResult&) = 0;
virtual void OnFinalizeClose(const meeting::FinalizeCloseResult&) = 0;   // notification only
```

理由（三条，均为 F2/F3 的直接推论）：

1. `MeetingCommandOutcome` **丢失** `ListParticipantsResult.participants`（F2）⇒ 用它无法编码成员列表；
2. 它把 `MeetingEvent` 降级为 `event_type`（F2）⇒ 无法编码 event 的 `event_id` / `actor` / `occurred_at`；
3. M4 §4.5 已把 `IWorkObservationSink` 的契约钉死为**纯观测**（"实现方不得…发送响应或触发通知"），
   把生产 response 挂在它上面会让该契约失效，也会让"观测"与"行为"在同一个回调点混杂。

因此最终确定的分工：

```text
IWorkObservationSink          = 纯观测（测试专用；生产为 nullptr，零开销）—— 保持不变
IMeetingResponseNotificationAdapter = 生产行为（编码 response、执行 best-effort 通知）
```

### 6.5 出站通道与"没有 message ID"的矛盾处理

`CSession::Send` 的签名为 `Send(std::string msg, short msgid)`，而 M6 **禁止**分配 TCP message ID
（STEP-1.6 §25.8）。处理方式：

```text
1. M6 保留 CSession::Send 的既有签名与 4 字节头语义（HEAD_TOTAL_LEN/HEAD_ID_LEN/HEAD_DATA_LEN 不变）；
2. 本步**不**定义任何 Meeting 协议号；需要一个占位值时，只允许使用一个
   集中在 adapter 内的**具名 owner-document 占位常量**，并附注释说明：
     - 它不是 wire message ID；
     - Phase 3 必须替换为 Protocol V2 正式分配；
     - 该常量不得出现在 meeting/ 或 LogicWork*/ 内；
3. 若 Review 认为连占位常量也不可接受，则 §16.5 的 §5 合并方案为：
   只实现 response **值对象 + directory lookup 调用序列**，把 payload 交给
   `IMeetingOutboundChannel` 而不构造任何帧 —— 此时占位常量完全消失。
   **这是需要人工裁决的唯一取舍点**（本文档 §19 记录）。
```

### 6.6 response 的调用位置与顺序

response 的编码与出站必须发生**在消费者线程内、该 work item 的领域 mutation 完成之后、
下一个 work item 被取出之前**。因为在 FIFO 语义下，命令的 response 也是该 turn 的结果，
把它推给别的线程/队列会让"response 顺序 == 命令顺序"不再是结构保证。

```text
消费者线程
  for (;;) {
      锁内：等待 / 取出 item
      锁外：DispatchOutsideLock(item)
              └── handler_.DispatchWork(item)
                    ├── meeting_service_.<API>(...)          ← 领域 mutation（唯一写者）
                    ├── sink_->Record*（若 sink != nullptr）  ← 纯观测，保持不动
                    ├── adapter->On<API>(result)               ← M6 新增：编码 + best-effort send
                    └── （Close）PostInternalWork(FINALIZE_CLOSE)  ← 必须仍在 adapter 之后
      锁内：in_flight_ 归零 + notify
  }
```

**顺序硬约束（M6 Review 必须核对）**：

```text
1. adapter 调用**不得**改变 dispatch 的返回路径：它的返回值不参与任何领域判断；
2. adapter 调用**不得**改变 Close → FinalizeClose 的入队：FINALIZE_CLOSE 必须仍然
   由 DispatchClose 在 sink/adapter 之后追加到同一 FIFO 尾部（M4 §10.3 的"不插队"结论不变）；
3. adapter 的 sink-like 记录（若有）必须发生在领域结果已落地之后，
   以避免重演 M4 §6.5 记录过的"观测顺序 ≠ 真实分发顺序"缺陷；
4. adapter 抛异常不得逃逸到 WorkerLoop：Send 路径必须 noexcept 化
   （否则一个坏 session 会终止唯一消费者线程，等价于整个 Meeting 子系统停摆）。
```

---

## 7. Notification Path

### 7.1 唯一生产路径（M6 设计）

```text
MeetingService::FinalizeClose(meeting_id)                            MeetingService.cpp:945
        │  产出 result.notification_session_ids（intent，0..N 个稳定 SessionId 值）
        ▼
LogicWorkDispatcher::DispatchWork  case FINALIZE_CLOSE               LogicWorkDispatcher.cpp:157-165
        │  （领域 mutation 已完成；snapshot 已发布）
        ▼
adapter->OnFinalizeClose(result)                                     ← M6 新增
        │  逐个 session_id：
        ▼
IMeetingSessionDirectory::FindSessionSendHandle(session_id)          ← CServer 实现
        │
        ├── nullptr  ⇒ **skip**（best-effort；计入观测计数，不作为错误）
        └── handle   ⇒ IMeetingOutboundChannel::SendToSession(...)   ← CSession::Send
        │
        ▼
（回到消费者循环；不等待、不重试、不回滚）
```

### 7.2 为什么通知必须**同步**发生在这一 turn、而不是投递给别的队列

| 候选 | 分析 | 结论 |
| --- | --- | --- |
| 在同一 turn 内同步 best-effort 发送 | 不新增 FIFO / 不新增 worker / 不新增 timer；顺序可断言；符合 STEP-1.6 §15.4 | **采用** |
| 投递为新的 internal work kind | 同一 FIFO 但**多一个 turn**；`ENDING → CLOSED` 已在本 turn 收敛，通知 turn 只是副作用；会引入"通知 work 在 shutdown drain 期间是否需要补做"的新问题，且 `PushInternalDuringDrainLocked` 的例外语义（M4 §11.2）是为"已受理工作的必要后续"设计的，通知**不是**必要后续（丢了不影响收敛） | 否 |
| 专用通知线程 / asio post 到独立队列 | 违反 §12.1（第二 FIFO）与 M4 §5.1（第二 worker） | 否 |
| timer 批量发送 | 违反 M6 禁止项（不得新增 timer / background cleanup） | 否 |

**关键论证**：`FinalizeClose` 的领域效果（snapshot 发布 + `ENDING → CLOSED` + `MeetingClosed` exactly once）
在 `MeetingService.cpp:919-948` 内**已经全部完成**，`notification_session_ids` 是返回值的一部分。
因此通知是**收敛之后**的纯副作用，其失败在结构上不可能影响 `ENDING → CLOSED` 收敛
（§9 会把它变成可断言的验收项）。

### 7.3 notification 的观测（不引入第二套机制）

adapter 必须把"意图数 / 尝试数 / 跳过数"作为**只读计数**暴露给测试，而不是靠日志：

```text
observed_notification_intents     本次 OnFinalizeClose 收到的 session_id 数
observed_notification_attempts    成功取到 handle 并调用 SendToSession 的次数
observed_notification_skipped     FindSessionSendHandle 返回 nullptr 的次数
```

这些计数的**唯一用途**是 Layer C 断言"best-effort skip 发生了、且领域状态未变"。
它们不是领域状态、不参与任何判定、不进任何协议。

---

## 8. Session Lookup

### 8.1 最小 API 形式（对 `CServer`）

```cpp
// 只读、非拥有、线程安全。
// 目标不存在 / 会话已进入关闭路径 ⇒ 返回 nullptr（best-effort skip，不是错误）。
IMeetingOutboundChannel* FindSessionSendHandle(const std::string& session_id);
```

实现形式（`CServer` 同时实现 `IMeetingSessionDirectory`；`CSession` 实现 `IMeetingOutboundChannel`）：

```cpp
// CServer.cpp（示意；Coding 时以实际为准）
IMeetingOutboundChannel* CServer::FindSessionSendHandle(const std::string& session_id) {
        std::lock_guard<std::mutex> lock(_mutex);        // ← 必须在 _mutex 下 find（见 §8.3）
        std::map<std::string, std::shared_ptr<CSession>>::iterator it = _sessions.find(session_id);
        if (it == _sessions.end()) {
                return nullptr;
        }
        return it->second.get();                          // 非拥有裸指针
}
```

### 8.2 为什么用 `SendHandle` 而不是 `shared_ptr<CSession>`

STEP-1.6 §15.2 建议的候选是 `FindSession(session_id) → std::shared_ptr<CSession>`。
本文档**偏离**该建议形式，理由如下（这是"设计裁决"而非"实现细节"）：

| 维度 | `shared_ptr<CSession>` | `IMeetingOutboundChannel*`（本文采用） |
| --- | --- | --- |
| 所有权语义 | 延长生命周期，adapter 一旦持有就参与 Session 析构时机 | 明确**非拥有借用**，不改变任何生命周期 |
| 依赖面 | adapter 必须 include `CSession.h`（asio/beast/`const.h`） | adapter 只 include 纯接口头 |
| 可测性 | 测试需要构造/伪造 `CSession`（需 io_context + socket） | recording 替身即可，无需 socket |
| 与 §15.3 的一致性 | 一致 | 一致（更严格：连"可延长生命"的能力都不给） |

**但这是对 STEP-1.6 §15.2 的一次形式偏离，必须由人工确认**（§19 Conflict #2）。

### 8.3 `_mutex` 纪律（含一处**必须显式声明**的既有缺陷）

```text
M6 新增的 FindSessionSendHandle 必须在 _mutex 下 find
  → 它是 _sessions 的**第三个**访问者
  → 它绝不能复制 CServer::ClearSession 里"find 未持锁"的既有缺陷（STEP-1.6 §3.3、R10）

M6 不修 CServer::ClearSession 的既有锁缺陷
  → 理由：STEP-1.6 §13.3 明令"属于既有并发隐患；本文只记录，不在 M5 顺手重写"，
    同一纪律对 M6 成立（M6 的 Files allowed 里 CServer.* 是为 FindSession 而开，不是为重构）
  → 后果必须如实记录为未消除的风险（§18 R-2）
```

### 8.4 悬垂指针窗口（必须承认，并给出缓解）

`FindSessionSendHandle` 返回借用指针，理论上存在
"取到 handle 之后、`Send` 之前，另一线程 `ClearSession` 把该 Session 从 `_sessions` 摘除并析构"
的窗口。M6 的缓解与结论：

```text
1. 接受该窗口，不引入新的锁序（否则会与 _send_lock / 队列锁形成新的嵌套风险）；
2. 缓解手段：CSession::Send 内部通过 SharedSelf() 持有 shared_ptr 直到 async_write 完成，
   因此**只要 CSession 对象仍存活**发送就是安全的；
3. 该窗口属于既有网络层生命周期暴露面，M5 §14 item 6 已记录同类暴露面（进程退出窗口），
   M6 **不扩大、也不声称消除**；
4. Layer C 必须覆盖"目标在通知过程中消失"的**可决定性**情形：directory 返回 nullptr
   ⇒ 跳过；并断言"跳过不影响领域状态"。
```

---

## 9. Best-effort Semantics

### 9.1 冻结规则（STEP-1.6 §15.4 / §16.1，逐条落地）

| 情形 | 处理 | 依据 |
| --- | --- | --- |
| Session 不存在 | 跳过（best-effort），计入 `observed_notification_skipped` | §15.4 |
| Session lookup 失败 | 同上 | §15.4 |
| `Send` 内部失败（写队列满 / socket 错误） | **不回滚领域状态**；`CSession` 自行走既有 `Close()` 路径 | §15.4 + `CSession.cpp:245-249` |
| 目标在关闭过程中消失 | 不影响 `ENDING → CLOSED` 收敛 | §15.4 / §16.1 |
| ACK | **不存在**，Phase 1 不设计 | §15.4 |
| retry / backoff | **不存在** | §15.4 |
| 死信 / 重投 | **不存在** | §15.4 |
| response 的 session 不存在 | 无法投递 ⇒ 只记录，**不改变 result_code** | §15.4 + 本文 §13 |

### 9.2 三条约束（M6 Coding 必须写进代码注释与测试名）

```text
(1) 通知/响应的**任何**失败都只能产生"少发了一条字节"这一种后果，
    不得产生领域状态变化、不得产生事件、不得改变 result_code / disposition。
(2) adapter 的所有出站调用必须 noexcept 化：任何异常在 adapter 边界被吞掉并计数，
    绝不允许逃逸进 WorkerLoop（消费者线程死亡 = Meeting 子系统停摆）。
(3) adapter 不得阻塞：不 sleep、不等待、不做同步 I/O、不做重试。
```

### 9.3 可断言的验收形式

```text
X11（STEP-1.6 §19.5）：notification failure does not roll back CLOSED
  ⇒ 用 directory 返回 nullptr 的替身，断言：
     - FinalizeClose 仍然 finalized == true
     - HasClosedSnapshot(meeting_id) == true
     - MeetingClosed 恰好一次
     - observed_notification_skipped == 期望值
     - 无任何第二次事件、无第二次 FinalizeClose
```

---

## 10. MeetingView Security Boundary

### 10.1 冻结裁决（M3-closeout §8.2，**不得**因编码而被绕过）

> `MeetingView` 只有在**授权通过**（或幂等命中）时才填充；
> `PERMISSION_DENIED` / `NOT_PARTICIPANT` 时保持默认空值。

本轮代码复核确认该裁决在领域层**已被实现**（`MeetingService.cpp:755-776` 等）：

```cpp
// QueryMeeting：授权通过才填 view
if (caller == aggregate->HostUserId()) {
        result.common = MakeCommon(context.request_id, ResultCode::OK);
        result.meeting = aggregate->BuildMeetingView();     // ← 仅授权通过
        return result;
}
...
// 从未参加，或曾经参加但当前为 LEFT：开放 Meeting 下都不再可见。
// 授权失败时不返回 MeetingView，避免泄露私有会议摘要。
result.common = MakeCommon(context.request_id, ResultCode::NOT_PARTICIPANT);   // ← 不填 view
```

### 10.2 反向约束：M6 不得**反向推断**

M6 的编码路径不得使用"`result_code != OK` ⇒ 视图为空"之外的任何启发式，
也不得为了"响应完整"而从别处补齐字段（例如用 `MeetingCommandOutcome.active_participant_count`
或 `MeetingRequestContext` 推断 `host_user_id`）。

### 10.3 编码矩阵（M6 Review 必须逐格核对）

| `result_code` | `has_view` | 允许出现的字段 | 禁止出现 |
| --- | --- | --- | --- |
| `OK`（`SUCCESS`） | **true** | `meeting_id`、`owner_chat_server_id`、`host_user_id`、`meeting_state`、`active_participant_count`、`created_at`、`has_closed_at`/`closed_at` | 任何 `session_id`、Binding、socket |
| `ALREADY_JOINED` / `ALREADY_LEFT` / `CLOSE_IN_PROGRESS` / `ALREADY_CLOSED`（`IDEMPOTENT`） | 按 M3 是否填充照实透传（**不新增**填充逻辑） | 与 `OK` 同（因为幂等命中意味着授权通过） | 同上 |
| `PERMISSION_DENIED` | **false** | 仅 `result_code` / `result_name` / `disposition` / `request_id` / `has_meeting_id`+`meeting_id`（该 id 由请求携带，非泄露） | `active_participant_count`、`host_user_id`、`owner_chat_server_id`、`meeting_state`、`created_at` |
| `NOT_PARTICIPANT` | **false** | 同上 | 同上 |
| `AUTH_REQUIRED` / `SESSION_STATE_REJECTED` | false | 同上 | 同上 |
| `INVALID_ARGUMENT` | false | 同上 | 同上 |
| `MEETING_NOT_FOUND` | false | 同上 | 同上 |
| `MEETING_NOT_LOCAL` | false | 同上 + `has_owner_chat_server_id`/`owner_chat_server_id`（**唯一**合法携带异地 owner 的结果） | 同上 |
| `MEETING_STATE_REJECTED` / `HOST_MUST_CLOSE_MEETING` | false | 同上 | 同上 |
| `INTERNAL_ERROR` | false | 同上 | 同上 |

### 10.4 `ListParticipants` 的额外规则

`ParticipantView` 列表只在授权通过的 `ListParticipantsResult` 上存在；
`ListParticipantsResult.scope`（`CURRENT` / `HISTORICAL`）必须照实透传，
不得为了"统一形状"把 CLOSED 的历史身份与活动成员混为一谈（`MeetingTypes.h:229-240`、
`MeetingService.h:190-196`）。

### 10.5 必须新增的静态核对项（写进 M6 Review checklist）

```text
grep -n "active_participant_count\|host_user_id\|owner_chat_server_id" MeetingResponseNotificationAdapter.cpp
  ⇒ 每一处命中都必须位于"has_view == true 才执行"的分支内
  ⇒ 禁止存在任何无条件读取 result.meeting.* 后写入 response 的代码路径
```

---

## 11. Ordering / FIFO Semantics

### 11.1 冻结约束（不得违反）

```text
STEP-1.6 §12.1  ：Client command / SESSION_DISCONNECTED / FINALIZE_CLOSE 共享**同一个**
                  逻辑串行化顺序；禁止 A/B/C 三条队列。
STEP-1.6 §12.5  ：一个 FIFO、一个消费者决定 Meeting mutation 顺序。
M4 §5.1         ：全仓库仅 SerializedWorkExecutor 创建 work 消费线程。
M4 §11.2        ：stop 后外部 work 被丢弃；内部 work 是唯一例外（消费者线程专用）。
M5.1 §6         ：REGISTER < 第一条业务消息；AUTHENTICATE 落在 FIFO 尾部；
                  DISCONNECT < 迟到命令；不做优先级/插队。
```

### 11.2 M6 的顺序证明（三条，均为结构性的）

**(O1) adapter 不改变 FIFO 的成员与顺序。**
adapter 的唯一调用点是 `Dispatch*` 内部，且**不向队列投递任何 work**。
唯一由 dispatcher 发起的入队仍然是 `DispatchClose` 的 `PostInternalWork(FINALIZE_CLOSE)`
（`LogicWorkDispatcher.cpp:275-277`），其位置、条件（`should_schedule_finalize == true`）、
目标队列（同一 `queue_`，经 `EnqueueInternalDuringDrain`）全部不变。

```text
证明义务：grep adapter 调用点前后不得出现 Enqueue / Push / PostInternalWork
          （除既有的 PostInternalWork(FINALIZE_CLOSE) 之外）
```

**(O2) adapter 不引入第二消费者，也不引入新的等待。**
adapter 在消费者线程上被同步调用，不创建线程、不 sleep、不阻塞 I/O。
因此"Meeting mutation 不并发"（single-writer）保持不变。

**(O3) notification 发生在领域收敛之后，因而不参与裁决顺序。**
`FinalizeClose` 的 snapshot 发布（`MeetingService.cpp:923`）与该 turn 的
`MeetingClosed` 事件都在 `MeetingService` 内部完成；`OnFinalizeClose` 拿到的是
已经最终的结果值。所以"通知是否成功"不可能反过来影响任何裁决。

### 11.3 与 Response 顺序的关系（本步新增的可断言命题）

```text
命题：在同一个 FIFO 上，命令响应顺序 == 命令入队顺序（response ordering == enqueue ordering）

依据：（1）消费者唯一 ⇒ 命令被串行处理；
      （2）response 在同一 turn 内编码并出站 ⇒ 不存在"结果先于前一条命令的响应"的可能；
      （3）不引入任何并发出口（不 post 到别的 executor、不由别的线程编码）。
可断言形式（Layer C）：连续入队 CREATE/QUERY/CLOSE，断言记录型 channel 的调用序列
                        与入队序列逐项一致，且每条 response 携带对应的 request_id。
```

### 11.4 明确不引入的东西（M6 Review 检查项）

```text
第二队列 / 第二 worker / 线程池 / timer / 后台清理        → 禁止
adapter 内部的 async 编排（post/dispatch 到另一 executor）→ 禁止
notification 的批量合并 / 去重窗口                        → 禁止（会改变可观测的发送次数语义）
response 的延迟发送 / 合并                                → 禁止
```

---

## 12. Ownership / Lifetime

### 12.1 生产 ownership（推荐方案 B）

```text
ChatServer.cpp（composition root）
  └── CServer s(io_context, port)                       ← 栈对象，构造早于 io_context.run()
        ├── _sessions : std::map<std::string, std::shared_ptr<CSession>>
        ├── 实现 IMeetingSessionDirectory（FindSessionSendHandle）
        └── 在 main/run 之前把自己注册进 LogicSystem 的 adapter 依赖槽

LogicSystem（Singleton）
  ├── _dispatcher : unique_ptr<LogicWorkDispatcher>      ← 惰性构造（EnsureWorkQueueStarted）
  │     └── IMeetingResponseNotificationAdapter*         ← 裸指针，指向下述 adapter
  ├── _meeting_adapter : unique_ptr<MeetingResponseNotificationAdapter>   ← M6 新增，与 dispatcher 同批构造
  │     └── IMeetingSessionDirectory* session_directory_ ← 借用（非拥有），来自 CServer
  └── _work_executor : unique_ptr<SerializedWorkExecutor<LogicWorkDispatcher>>
```

### 12.2 为什么 adapter 与 dispatcher 必须在同一个 `call_once` 内构造

因为 `LogicWorkDispatcher` 的构造需要 `IMeetingResponseNotificationAdapter*`，
而 adapter 需要 `IMeetingSessionDirectory*`。两者都在 `EnsureWorkQueueStarted()` 的
`std::call_once` 内一次性建立，可以保证：

```text
1. adapter 的生命周期 ⊆ dispatcher 的生命周期 ⊆ LogicSystem 的生命周期；
2. dispatcher 析构时 worker 已经 StopAndJoin（LogicSystem.cpp:496-504），
   因此 adapter 不可能在消费者线程仍在运行时被销毁；
3. 不需要为 adapter 引入任何锁或引用计数。
```

### 12.3 依赖注册的时序与可见性证明（M6 必须显式论证）

`CServer` 实例必须把自己交给 `LogicSystem`——但**注入必须发生在 worker 线程创建之前**，
否则 `call_once` 内读到的 `session_directory_` 会是空。

```text
main 线程                                    worker 线程
─────────                                    ───────────
ConfigMgr / Redis init
CServer s(io_context, port)        ← 构造，此时尚未 accept
  └── 注册 session directory 到 LogicSystem    （线程尚未存在）
io_context.run()                   ← 之后才可能 HandleAccept
                                               EnsureWorkQueueStarted()
                                                 call_once: 读 session_directory_（已写）
                                                 new LogicWorkDispatcher(..., adapter)
                                                 new SerializedWorkExecutor(...)
                                                   └── std::thread 启动  ← happens-before 点
```

**可见性与顺序的双重保证**：

```text
(a) 顺序保证：CServer 构造 / 注册 早于 io_context.run()，而首次入队只能由 accept 后的
    HandleAccept 产生（M5.1 §4.2：投递在 Start() 之前但仍在 accept 成功之后）。
(b) 可见性保证：会话目录的写入发生在 std::thread 构造之前；std::thread 构造与线程启动
    之间是 C++ 标准规定的 happens-before 边。因此 worker 线程无需任何同步即可安全读该指针。
(c) 幂等性：注册接口使用 std::call_once 语义（或等价的一次性断言），重复注册不改变状态。
```

> ⚠️ 这是 M6 最容易写错的一处。若 Coding 把注册放到 `io_context.run()` 之后，
> 或在 `CServer` 构造函数内部依赖尚未建立的 `LogicSystem`，就会得到
> "adapter 永远看到 nullptr directory"的静默失效。Layer C 必须能**在测试中**复现
> 该时序（§14.4 给出确定性做法）。

### 12.4 非拥有借用一览（M6 新增的指针全部是借用）

| 指针 | 拥有者 | 借用者 | 生命周期论证 |
| --- | --- | --- | --- |
| `IMeetingResponseNotificationAdapter*` | `LogicSystem::_meeting_adapter` | `LogicWorkDispatcher::adapter_` | adapter ⊆ dispatcher ⊆ LogicSystem；dispatcher 先 StopAndJoin |
| `IMeetingSessionDirectory*` | `CServer`（栈对象，活在 `io_context.run()` 期间） | adapter | worker 线程生命周期 ⊆ `io_context.run()`；M6 **不**在 `run()` 返回后使用 adapter |
| `IMeetingOutboundChannel*` | `CServer::_sessions`（`shared_ptr`） | adapter（**仅在一次 `SendToSession` 调用期间**） | 借用窗口极短；不保存、不缓存、跨语句即失效（§8.4） |

---

## 13. Error Handling

### 13.1 三层错误必须严格分开

```text
层 1  领域判定（MeetingService）        → ResultCode / ResultDisposition（冻结的 15 个值）
层 2  适配编码（adapter）               → 只做映射，**不得**新增 ResultCode、不得改写 result_code
层 3  出站投递（CSession::Send）        → best-effort，失败只计数，不产生任何领域后果
```

### 13.2 硬性规则

```text
(1) adapter **绝不**把一个 ResultCode 改成另一个。
    尤其：不得把 SESSION_STATE_REJECTED 改成 AUTH_REQUIRED（STEP-1.4 §19.4 明令禁止）。
(2) session lookup 失败（response 目标已消失）**不得**改变 result_code / disposition。
    response 的角色是"报告领域结果"，而领域结果与"能否送达"无关。
(3) `result_name` 必须来自 `ResultCodeName()`（MeetingTypes.h:137），
    不得维护第二套字符串映射（M4 §8 同一纪律）。
(4) 未识别的 ResultCode（理论不可达）→ 编码为 `INTERNAL_ERROR` 并**计数**，
    不得静默产生空响应；同时不得抛出。
(5) adapter 内部任何异常都在边界处吞掉并计数：绝不允许异常逃逸进 WorkerLoop。
```

### 13.3 与 close/finalize 的相互作用（回答"是否会破坏收敛"）

```text
问题：response 发送失败会不会让 ENDING → CLOSED 不收敛？
答案：结构上不可能。原因链：
      (a) CloseMeeting 的 response 在 turn 1 编码，而 ENDING → CLOSED 由 turn 2 的
          FinalizeClose 完成（STEP-1.6 §16，两个独立 work item）；
      (b) FINALIZE_CLOSE 的入队由 `should_schedule_finalize` 决定（领域返回值），
          与 response 是否发送成功**无关**；
      (c) FinalizeClose 的领域效果在 MeetingService 内部一次性完成，
          notification 只是其返回值的下游副作用（§7.2）。
      ⇒ 故"通知/响应失败不回滚状态"与"ENDING → CLOSED 必然收敛"同时成立，
        且都是**结构**保证而非约定。
```

### 13.4 错误可观测性（不引入日志依赖的断言口径）

```text
adapter 必须暴露只读计数（§7.3）：intents / attempts / skipped / unrecognized
adapter **不得**直接写业务日志作为唯一证据来源（日志只能是辅助）
adapter 不得修改任何全局状态、不得调用 LogicSystem::GetInstance()
```

---

## 14. Test Strategy

### 14.1 分层归属（STEP-1.6 §17.5：Layer C 首次可运行于 M5 / M6）

| 层 | M6 任务 | 是否可运行于 M6 |
| --- | --- | --- |
| Layer A（纯领域） | 与通知无关的断言**必须保持通过**（STEP-1.6 §25.7 Unit tests 要求） | ✅（零改动即通过，作为回归护栏） |
| Layer B（串行化顺序） | 既有 25 个用例不改；M6 不复制其反模式 | ✅ |
| Layer C（ChatServer 集成） | M6 的**主战场**：context builder / lookup / target disappearing / best-effort / 安全边界 / session state | ✅ |
| Layer D（真实 TCP 端到端） | **Deferred**（无 Protocol V2） | ❌ |

### 14.2 测试用例矩阵（设计；本步不创建）

**组 1：`MeetingRequestContext` builder（Layer C）**

| # | 用例 | 断言要点 |
| --- | --- | --- |
| CB1 | 合法输入构造成功 | `request_id`/`session_id`/`handling_chat_server_id` 非空；`authenticated_user_id` 来自已认证 session |
| CB2 | identity 只来自服务端 | builder **不**接受客户端提供的 user_id 覆盖；构造结果与传入的 authenticated session 一致 |
| CB3 | `handling_chat_server_id` 恒为本地节点 | 与 `LogicSystem::LocalServerId()` 一致；不一致时领域返回 `INVALID_ARGUMENT`（`MeetingService.cpp:132-135`） |
| CB4 | device 概念缺失时不伪造 | `has_device_id == false`（`MeetingTypes.h:197` 明令不得伪造） |
| CB5 | 空 `request_id` / 空 `session_id` | 领域返回 `INVALID_ARGUMENT`（`MeetingService.cpp:119-126`），证明 builder 未补偿 |
| CB6 | 未认证 session | 领域返回 `AUTH_REQUIRED`；builder 不试图绕过 |

**组 2：session lookup（Layer C）**

| # | 用例 | 断言要点 |
| --- | --- | --- |
| SL1 | 已注册 session 可查到 | handle 非空；`SendToSession` 被调用一次，`session_id` 与请求一致 |
| SL2 | 未知 session ⇒ nullptr | 返回 nullptr，不抛、不创建条目 |
| SL3 | `ClearSession` 之后查不到 | insert → erase → lookup == nullptr（验证 M6 的 find 持锁实现） |
| SL4 | 同一 session 重复 lookup 幂等 | 不产生副作用、不改变 `_sessions` 大小 |
| SL5 |（并发 smoke，非回归门）多线程 lookup 与 erase 并发不崩溃 | **不**断言具体结果；仅作 sanitizer/压力观察，且必须标注非确定性 |

**组 3：notification target disappearing（Layer C，STEP-1.6 §25.7 明确要求）**

| # | 用例 | 断言要点 |
| --- | --- | --- |
| ND1 | 全部目标存在 | `attempts == intents`，`skipped == 0`，每个目标各收到 1 次 |
| ND2 | 全部目标消失 | `attempts == 0`，`skipped == intents`，**无异常、无崩溃** |
| ND3 | 部分目标消失 | 存在的收到、消失的跳过；总数守恒（`attempts + skipped == intents`） |
| ND4 | 目标消失后领域状态不回滚 | 见 X11：`finalized == true`、`HasClosedSnapshot == true`、`MeetingClosed` 恰好一次 |
| ND5 | 目标消失不产生第二个 FinalizeClose | FINALIZE_CLOSE 的 dispatch 计数仍为 1 |
| ND6 | 重复 FinalizeClose（幂等）不再产生通知意图 | 第二次 `OnFinalizeClose` 的 `notification_session_ids` 为空（`MeetingService.h:217-219`） |

**组 4：best-effort send failure（Layer C）**

| # | 用例 | 断言要点 |
| --- | --- | --- |
| BF1 | channel 返回 false（写队列满/写失败） | adapter 不重试、不抛；领域结果不变；`result_code` 不变 |
| BF2 | channel 抛异常 | 异常被吞、计数；WorkerLoop 存活（后续 work 仍被处理）⇒ 直接锁定 §9.2(2) |
| BF3 | 不阻塞 | 在 channel 内注入同步可观测的"调用即返回"，断言 adapter 未引入 sleep/等待（用计数与调用序，不用计时） |
| BF4 | 无 ACK / 无 retry | 断言同一目标在同一次 OnFinalizeClose 内只被调用**恰好一次** |

**组 5：MeetingView permission boundary（Layer C，安全）**

| # | 用例 | 断言要点 |
| --- | --- | --- |
| MV1 | `OK` ⇒ `has_view == true`，字段完整 | `host_user_id` / `active_participant_count` 与领域一致 |
| MV2 | `PERMISSION_DENIED` ⇒ `has_view == false` | 响应中**不存在** `active_participant_count` / `host_user_id`（按 §10.3 矩阵） |
| MV3 | `NOT_PARTICIPANT` ⇒ `has_view == false` | 同上 |
| MV4 | CLOSED + 非历史成员 ⇒ `NOT_PARTICIPANT` ⇒ 不泄露 | 与 MV3 同组，但走 snapshot 分支（`MeetingService.cpp:771-778`） |
| MV5 | CLOSED + 历史成员 ⇒ `OK` ⇒ 视图来自 snapshot | `has_closed_at == true` |
| MV6 | 幂等命中（`ALREADY_JOINED` 等）的视图透传 | 不新增填充逻辑；与 M3 行为逐字段一致 |
| MV7 | 静态核对 | 见 §10.5 的 grep 检查项（作为 Review gate，不是运行时用例） |

**组 6：`CLOSING` / `CLOSED` → `SESSION_STATE_REJECTED`（Layer C）**

| # | 用例 | 断言要点 |
| --- | --- | --- |
| SS1 | `CLOSING` 时命令 ⇒ `SESSION_STATE_REJECTED` | **不是** `AUTH_REQUIRED`（`MeetingService.cpp:170-174`） |
| SS2 | `CLOSED`（tombstone）时命令 ⇒ `SESSION_STATE_REJECTED` | 同上；证明 M5.1 的 tombstone 语义未被 M6 绕过 |
| SS3 | 从未 REGISTER 的 session ⇒ `AUTH_REQUIRED` | 与 SS1/SS2 形成对照，锁定"两种失败不可混淆" |
| SS4 | adapter 不因 lookup 失败而改写该结果 | 即使 directory 也返回 nullptr，`result_code` 仍为 `SESSION_STATE_REJECTED` |
| SS5 | response 无法投递（session 已消失） | 只计数；不产生 `AUTH_REQUIRED`、不产生第二次响应 |

**组 7：顺序（Layer C，对应 §11.3）**

| # | 用例 | 断言要点 |
| --- | --- | --- |
| OR1 | 命令响应顺序 == 入队顺序 | 连续 CREATE/QUERY/CLOSE，断言记录型 channel 的调用序列与入队序列逐项一致 |
| OR2 | adapter 不改变 FIFO 顺序 | CLOSE 之后 FIFO 中仍有后续命令时，FINALIZE_CLOSE 仍排在尾部（复用 M4 §10.3 的入队瞬间快照断言方式） |
| OR3 | Close 的 response 早于 FinalizeClose | turn 1 响应为 `ENDING`（`should_schedule_finalize == true`）；`MeetingClosed` 在 turn 2 后恰好一次 |

### 14.3 如何避免复制生产分发逻辑（沿用 M4/M5.1 的切口哲学）

```text
可复用的真实生产组件：
  SerializedWorkExecutor<LogicWorkDispatcher>     ← 真实队列 + 真实单消费者
  LogicWorkDispatcher                             ← 真实 Meeting 分发（六项 API 路径）
  meeting::MeetingService                         ← 真实领域
  MeetingResponseNotificationAdapter              ← M6 真实 adapter（本步新增，必须被真实驱动）
  FakeIdFactory / MakeSystemMeetingWorkDependencies 的真实或固定注入

必须由测试提供的替身（只记录，不实现业务）：
  RecordingSink（既有 IWorkObservationSink 实现）
  RecordingSessionDirectory  : IMeetingSessionDirectory   ← 返回受控 handle 或 nullptr
  RecordingOutboundChannel   : IMeetingOutboundChannel    ← 记录 (session_id, msg_id, payload)

明确禁止：
  在测试里复制 DispatchWork / Dispatch* 的分发表        → 必须调用真实 dispatcher
  用 #define private public 或访问私有成员               → 禁止
  只断言 enqueue 而不断言 dispatch 后的可观测结果         → 禁止
  EXPECT_TRUE(true) 占位 / 依赖 sleep 的竞态断言          → 禁止
```

### 14.4 如何建立可靠的 happens-before（**针对既有 flaky 的教训**）

**既有测试债（本轮已复核，M6 不得修改，也不得复制其反模式）**：

```text
flaky #1  LogicWorkQueueTest.cpp:1135
          LogicWorkQueueShutdown.PendingCloseProducesFinalizeDuringDrain
          ASSERT_EQ(1u, scenario.Queue().Size())     ← 队列占用瞬时值竞态
          实测 ≈2/400（整轮）/ 3/300（聚焦）
          根因类别：EnqueueBatch 只持 idle_mutex_ 就返回；消费者在 queue mutex 下 Pop；
                    Size() 只锁 queue mutex ⇒ 断言的前提本身不成立

flaky #2  LogicWorkQueueTest.cpp:681
          LogicWorkQueue.DispatchCanReenqueueToTheSameFifoWithoutDeadlock
          EXPECT_TRUE(fixture.Handler().HasClosedSnapshot(meeting_id))
          实测 ≈1/400（整轮）/ 19/20000（聚焦）
          根因类别：测试线程在**没有 happens-before** 的情况下读取消费者线程写入的
                    non-thread-safe 状态（裸 std::map + sink 的裸 std::vector）
          ⚠️ 正确根因**不是**"RecordDispatch 发生在 FinalizeClose 之前"。
             实测 LogicWorkDispatcher.cpp:157-165 的 FINALIZE_CLOSE 分支为：
               meeting_service_.FinalizeClose(...)   ← 先（内部已发布 closed_snapshots_）
               sink_->RecordWorkDispatched(...)      ← 后
             即领域效果在 program order 上早于 dispatch 记录。
             裁定实验：等到 dispatch 记录出现后快照仍读为 absent（14/14 打印
             finalize_records_seen=0）；而先 Done() 再断言 ⇒ 5000/5000 PASS。
```

**M6 测试的强制纪律**：

```text
R1  对领域状态 / sink 计数的读取，必须建立在**同步边**之上：
      - 优先 `WaitUntilIdle(5000, baseline)` 或 `StopAndJoin()`（executor 自身 mutex+condvar）；
      - 或持 `LogicWorkQueue::Lock` 并用 `*Locked` 版本断言；
      - 绝不"入队后 sleep 一下再读非同步状态"。
R2  绝不断言队列的瞬时占用（Size()/IsEmpty()）作为"某 work 已受理/未受理"的证据。
R3  绝不断言"某个中间状态的可见性"作为领域效果的证据；只断言最终稳定的领域结果。
R4  M6 新增的 recording 替身（directory / channel）会被测试线程读取，而它们由消费者线程写入
      ⇒ 要么把它们的读取放在同步边之后，要么让替身自身用 mutex/atomic 保护
      （推荐：替身内部加锁，因为这是**测试基础设施**，加锁不触碰生产语义）。
R5  若 M6 需要"通知已发生"的可等待信号，必须复用 executor 的 idle 判定
      （`enqueued_count_` / `consumed_count_` / `in_flight_`），不得自造第二套等待。
```

### 14.5 测试 target 的构建影响（设计，不在本步执行）

```text
tests/CMakeLists.txt 需要新增：
  ../MeetingResponseNotificationAdapter.cpp     （与既有的 ../LogicWorkDispatcher.cpp 同风格）

仍然**不**进入测试 target（必须保持）：
  LogicSystem.cpp / CServer.cpp / CSession.cpp / ChatServer.cpp
  ⇒ 理由：会拉入 asio / Boost / Redis / MySQL / gRPC / jsoncpp 整条 legacy 业务链路
     （Step 2.5 §14、M4 §18）

因此本组验收的验证边界必须如实标注（见 §17.2）：
  - adapter 行为、lookup 契约、安全矩阵、best-effort 语义：由 Layer C 单测保证
  - CServer 的 FindSessionSendHandle 实现、注册时序、CSession::Send 真正写字节：
    由**静态复核 + runtime smoke**保证，不由单测保证
```

### 14.6 M6 自带的回归护栏

```text
1. 既有 224 个测试**一个都不许改、一个都不许少**（M6 不得修改既有测试文件）；
2. Layer A 中与通知无关的断言必须保持通过（STEP-1.6 §25.7 明写）；
3. 整轮重复（例如 200 轮）用于发现 M6 自身的新竞态：
   **必须先能区分既有 flaky #1/#2 与 M6 回归**，否则会产生假阳性判断。
   建议做法：报告中同时给出"整轮失败项"与"是否命中既有两项已知 flaky"的对照。
```

---

## 15. Scope Matrix

### 15.1 MUST MODIFY

| 文件 | 修改内容 | 依据 |
| --- | --- | --- |
| `Server/ChatServer/ChatServer/MeetingOutboundChannel.h`（**新增**） | `IMeetingOutboundChannel` + `IMeetingSessionDirectory` 窄接口（仅标准库） | §5.4 / §8.1；M6 实现范围"session_id → active CSession lookup" |
| `Server/ChatServer/ChatServer/MeetingResponseNotificationAdapter.h`（**新增**） | `IMeetingResponseNotificationAdapter`（dispatcher 依赖的抽象）+ 生产 adapter 声明 + 只读计数 | §5.2 / §6.4 |
| `Server/ChatServer/ChatServer/MeetingResponseNotificationAdapter.cpp`（**新增**） | response 编码、安全矩阵、notification best-effort、异常边界 | §6 / §7 / §9 / §10 |
| `Server/ChatServer/ChatServer/MeetingRequestContextBuilder.h`（**新增**） | context builder（纯函数式，无副作用） | §2.1 实现范围；STEP-1.6 §27 第 8 项 |
| `Server/ChatServer/ChatServer/LogicWorkDispatcher.h` | 构造参数增加 adapter 指针；`Dispatch*` 内增加 adapter 调用点 | §6.6 |
| `Server/ChatServer/ChatServer/LogicWorkDispatcher.cpp` | 六个 `Dispatch*` + `FINALIZE_CLOSE` 分支的 adapter 调用；保持 sink 观测顺序不变 | §6.6 / §11.2 (O1) |
| `Server/ChatServer/ChatServer/LogicSystem.h` / `.cpp` | 新增 adapter 成员与一次性注册入口；`EnsureWorkQueueStarted` 内构造 adapter 并注入 dispatcher | §12.1 / §12.3 |
| `Server/ChatServer/ChatServer/CServer.h` / `.cpp` | 实现 `IMeetingSessionDirectory`；新增 `FindSessionSendHandle`（**在 `_mutex` 下 find**）；在 `main` 中完成注册调用点 | §8.1 / §8.3 / §12.3 |
| `Server/ChatServer/ChatServer/CSession.h` / `.cpp` | 实现 `IMeetingOutboundChannel`（`SendToSession` 委托既有 `Send`，noexcept 化） | §6.5 / §8.2 |
| `Server/ChatServer/ChatServer/CMakeLists.txt` | 新增 2 个 TU 到 `CHATSERVER_APPLICATION_SOURCES` | §14.5 |
| `Server/ChatServer/ChatServer/tests/CMakeLists.txt` | 新增 adapter TU + M6 测试文件 | §14.5 |
| `Server/ChatServer/ChatServer/tests/MeetingResponseNotificationAdapterTest.cpp`（**新增**） | Layer C 用例（组 1–7） | §14.2 |
| `docs/phase2/M6-response-notification-adapter.md`（本文档） | 状态由 PLANNING 更新为 COMPLETE（Coding 后） | 惯例 |

### 15.2 MAY MODIFY（仅当代码事实证明必要时）

| 文件 | 何时才可修改 | 必须记录的理由 |
| --- | --- | --- |
| `Server/ChatServer/ChatServer/ChatServer.cpp` | **仅当**注册点必须放在 `io_context.run()` 之前且不适合放进 `CServer` 构造时（§12.3 的时序要求） | 必须说明为何不能放在 `CServer` 内，并证明注册早于 worker 线程创建 |
| `Server/ChatServer/ChatServer/CSession.cpp`（发送队列语义） | **仅当**发现 `MAX_SENDQUE` 静默丢弃会导致 adapter 无法区分"已投递/被丢弃" | 不得改变既有 `Send` 对 legacy 路径的行为 |
| `Server/ChatServer/ChatServer/LogicWorkItem.h` | **仅当**需要为 adapter 观测补一个纯只读字段（沿用 M5.1 §8 增加 `active_participant_count` 的同一裁决方式） | 必须先证明全仓无该 struct 的聚合初始化（M5.1 §8 的核对方法） |
| `docs/phase1/*` | **不允许**（见 MUST NOT） | — |

### 15.3 MUST NOT MODIFY

```text
Server/ChatServer/ChatServer/message.proto                       （M6 Files forbidden；Phase 3）
Server/ChatServer/ChatServer/*.sln                               （M6 Files forbidden；R6 属 DEFERRED）
Server/ChatServer/ChatServer/*.vcxproj                           （同上）
Server/ChatServer/ChatServer/*.props                             （同上）
Server/ChatServer/ChatServer/*.filters                           （同上）
MyChat_Qt/**                                                     （M6 Files forbidden）
Server/ChatServer2/**                                            （R8 未解决；第一轮只在 ChatServer 落点）
Server/GateServer/**  Server/StatusServer/**  Server/VerifyServer/**  （与本阶段无关）

Server/ChatServer/ChatServer/meeting/**                          （领域契约已冻结：M6 不得改业务规则）
  ——例外：仅当出现**明确、不可避免**的极小修改需求（例如某个 result 字段确实无法从
     既有类型取得）。此时**必须停在人工裁决点**，不得自行修改，也不得自行发明 workaround。

Server/ChatServer/ChatServer/tests/LogicWorkQueueTest.cpp         （M6 不得修既有 flaky）
Server/ChatServer/ChatServer/tests/MeetingServiceTest.cpp
Server/ChatServer/ChatServer/tests/MeetingAggregateTest.cpp
Server/ChatServer/ChatServer/tests/MeetingTypesContractTest.cpp
Server/ChatServer/ChatServer/tests/SessionDisconnectBridgeTest.cpp
Server/ChatServer/ChatServer/tests/DomainSessionLifecycleTest.cpp
Server/ChatServer/ChatServer/tests/TestInfrastructureSmoke.cpp
  ——以上 7 个既有测试文件 M6 一个都不改（新增 M6 测试另开新文件）

docs/phase1/STEP-1.1 ~ STEP-1.6                               （冻结设计；发现冲突只报告不修改）
```

### 15.4 DEFERRED

| 项 | 状态 | 依据 |
| --- | --- | --- |
| Wire Adapter：TCP message ID / JSON / protobuf / Frame V2 / request_id encoding / Qt command / HTTP / WebSocket | **DEFERRED → Phase 3** | STEP-1.6 §25.8 |
| Layer D（真实 TCP 端到端） | **DEFERRED** | STEP-1.6 §17.4 |
| Windows `Debug\|x64` 与 R6（新文件未加入 `.vcxproj`） | **DEFERRED**（见 §18 R-5） | M4 §17.1 / M5.1 §11.1；且 M6 Files forbidden 已含工程文件 |
| Domain session tombstone retention（TTL / LRU / timer / 后台清理） | **DEFERRED** | M3 §/ M5.1 §7（已有明确论证：需 FIFO 完成水位 + generation/epoch 才能安全回收） |
| ClosedSnapshot retention / 淘汰策略 | **DEFERRED** | M3 遗留；生产侧 `EvictClosedSnapshot` 零调用 |
| 既有 flaky #1 / #2 | **DEFERRED**（M6 不修） | M5.1 §9.2 / §12 item 4 + 本轮复核 |
| `CServer::ClearSession` 的 `find` 未持锁 | **DEFERRED**（M6 不顺手修） | STEP-1.6 §13.3 / R10 |
| `CSession::_user_uid` 构造未初始化 | **DEFERRED**（M6 不读它） | STEP-1.6 §13.3 / M5 §13.2 |
| `MysqlDao.cpp:95` 既有告警 | **DEFERRED**（不修） | 既有，唯一告警 |
| `re2_DIR` cache 残留（R7） | **DEFERRED** | M3 §8.3 / M4 §15 |
| ChatServer / ChatServer2 双份维护（R8） | **DEFERRED** | STEP-1.6 §22 |
| 多节点 / 跨 ChatServer Meeting 路由（`MEETING_NOT_LOCAL` 的真实重定向） | **DEFERRED** | Phase 1 明确 Meeting 不支持跨 ChatServer 操作 |
| `MeetingEvent` 的 wire 表达与去重投递 | **DEFERRED** | 事件只到 value 层；无 ACK/重试 |
| ACK / retry / 死信 / 可靠投递 | **DEFERRED** | STEP-1.6 §15.4 |

---

## 16. Known Risks

### R-1：M6 可能被误当成"接一根线"，而它实际缺少最外层出口（**首要风险**）

```text
风险：把"实现 response"理解成"分配 message ID 并发帧"，从而污染 Protocol V2
影响：高（不可逆的协议债）
缓解：§6.5 与 §16.5 的取值边界；§2.3 明确禁止 message ID；
      §19 Conflict #1 把"占位常量 vs 纯值对象"作为人工裁决点
```

### R-2：`CServer::ClearSession` 的 `find` 未持锁（既有，M6 会新增第三个访问者）

```text
风险：M6 新增 FindSessionSendHandle 后，_sessions 的并发访问面扩大
影响：中（既有隐患被放大，且 M6 明令不修）
缓解：新增的 find 必须在 _mutex 下；不修改 ClearSession；
      §8.4 明确承认借用指针窗口；如实写入 §16/§17 为"未消除"
```

### R-3：注册时序写错 ⇒ adapter 静默拿到 nullptr directory

```text
风险：把注册放在 io_context.run() 之后，或依赖尚未建立的 LogicSystem
影响：高（"通知永远发不出去"且无任何报错，属于静默失效）
缓解：§12.3 给出顺序 + 可见性 + 幂等三重论证；
      Layer C 必须能在测试中复现该时序（构造期注册 vs 首条命令）；
      runtime smoke 增加"adapter 已注入"的可观测证据（例如启动日志/计数器）
```

### R-4：reuse `IWorkObservationSink` 承载生产行为

```text
风险：为省一个新接口，把 response 挂到观测 sink 上
影响：中高（破坏 M4 §4.5 的"纯观测"契约；观测与行为在同一回调点混杂，
      使"测试观测"与"生产发送"无法分别验证）
缓解：§6.4 明确两层职责分离；Coding 不得给 sink 增加任何有副作用的实现
```

### R-5：Windows gate 与 M6 Task Card 的不一致（**已存在的冲突，不自行修正**）

```text
事实：STEP-1.6 §25.7 的 Build gate 写作 `Debug|x64 编译通过`；
      但当前 dev HEAD 在 Windows 上预期**链接失败**（LogicWorkDispatcher.cpp 与
      meeting/*.cpp 不在 ChatServer.vcxproj 的 ClCompile 列表中；15 个 ClCompile/17 个 ClInclude），
      且 M6 的 Files forbidden 明确包含 `PropertySheet.props`，
      §25.7 的 Files allowed 只提"修改两个工程文件"，未提补齐 R6 所需的完整条目。
影响：高（M6 无法同时满足"Build gate = Debug|x64 通过"与"不扩大范围"）
处置：**不自行修正 Phase 1，也不自行 workaround**；作为人工裁决点报告（§19 Conflict #3）
```

### R-6：M6 测试复制既有 flaky 的反模式

```text
风险：新写的 Layer C 用例延续"无 happens-before 读非同步状态 / 断言队列瞬时占用"
影响：中（新增 flaky，污染后续整轮重复验证）
缓解：§14.4 的 R1–R5 强制纪律；M6 Review 把该纪律作为独立检查项
```

### R-7：adapter 抛异常杀死唯一消费者线程

```text
风险：某个坏 session / 空 handle / 分配失败导致异常逃逸进 WorkerLoop
影响：高（Meeting 子系统整体停摆，且现象是"静默不再处理任何 work"）
缓解：§9.2(2) 强制 noexcept 边界 + 计数；组 4 的 BF2 用例专门锁定
```

### R-8：response 编码意外泄露会议摘要（安全）

```text
风险：为了"响应完整"而在授权失败路径上补齐 host_user_id / 成员数
影响：高（向非成员泄露私有会议信息，违反 M3 冻结裁决）
缓解：§10.3 编码矩阵 + §10.5 静态 grep 检查项 + MV2/MV3/MV4 用例
```

### R-9：偏离 STEP-1.6 §15.2 的 `FindSession → shared_ptr<CSession>` 建议形式

```text
风险：Review 认为窄接口 `SendHandle` 与冻结文档的形式不一致
影响：低（语义上更严格；§15.3 的"MeetingService 不持有 CServer"更强地成立）
缓解：§8.2 给出四条对比理由；§19 Conflict #2 显式请求确认
```

### R-10：通知计数成为"第二套领域状态"

```text
风险：为可测试性引入的 attempts/skipped 计数被后续代码当作业务判定依据
影响：中（观测指标变成隐式契约）
缓解：§7.3 明确计数"不是领域状态、不参与任何判定"；Review 核对无任何 if 依赖它
```

---

## 17. Deferred Work

### 17.1 明确不属于 M6 的工作（继承 + 本步新增）

```text
1.  Wire Adapter（TCP message ID / JSON / protobuf / Frame V2 / request_id encoding /
    Qt command / HTTP / WebSocket）                        → Phase 3（STEP-1.6 §25.8）
2.  Layer D 真实 TCP 端到端测试                            → 协议 adapter 落地后
3.  Windows Debug|x64 与 R6（补齐 vcxproj 条目 + /utf-8 或 BOM 策略）
                                                           → 未决定由哪一步处理（M4 §17.1）
4.  Domain session tombstone retention（TTL/LRU/timer/后台清理）→ Deferred（M5.1 §7）
5.  ClosedSnapshot retention / 淘汰策略                     → Deferred（M3 遗留）
6.  既有 flaky #1（LogicWorkQueueTest.cpp:1135）            → 建议单独一步修复
7.  既有 flaky #2（LogicWorkQueueTest.cpp:681）             → 建议与 #1 同一步修复
8.  CServer::ClearSession 的 find 未持锁                    → 既有并发隐患
9.  CSession::_user_uid 构造未初始化                        → 既有（M6 不读它）
10. MysqlDao.cpp:95 告警 / re2_DIR cache 残留（R7）         → 既有
11. ChatServer / ChatServer2 双份维护（R8）                  → 需先解决共享源码/构建结构
12. 多节点 Meeting 路由与 MEETING_NOT_LOCAL 真实重定向       → Phase 1 明确不支持跨节点
13. MeetingEvent 的可靠投递 / ACK / retry / 死信             → 无设计
14. 通知的批量合并 / 去重 / 延迟发送                         → 本步明确禁止引入（§11.4）
15. legacy IM 完整业务链路（MySQL 好友/申请）验证            → 无 MySQL 环境
16. 真实 Login → AUTHENTICATE 的 runtime 验证                → M5.1 §10.1 未解决（需真实 token + MySQL）
```

### 17.2 M6 完成后仍然存在的验证边界（必须如实标注，不得声称已消除）

```text
(a) CServer::FindSessionSendHandle 的实现、CServer 的注册时序、
    CSession::Send 真正把字节写到 socket —— 由**静态复核 + runtime smoke**保证，
    不由单元测试保证（CServer.cpp / CSession.cpp / LogicSystem.cpp 不在测试 target 内）。
(b) 真实客户端 → Meeting 命令 → 响应 的端到端路径**不存在**（无 Protocol V2），
    因此 M6 的 response 只能被测试 adapter / direct invocation 验证
    （STEP-1.6 §25.7 条件说明明确允许）。
(c) 通知的"送达"不可验证（无 ACK），只能验证"已尝试调用 Send"与"是否正确跳过"。
(d) Windows 侧一切结论均不可在 Linux 环境验证。
```

---

## 18. Acceptance Criteria

M6 只有在全部满足下列条件后才可宣布 COMPLETE（每条都必须可核对，不得用"看起来对"替代）。

### 18.1 结构类（静态可核）

```text
A1  adapter 不持有 CServer：grep -n "CServer" MeetingResponseNotificationAdapter.h == 0 命中
A2  adapter 不 include CSession.h / CServer.h / LogicSystem.h / asio / boost
A3  adapter 不认识 MeetingAggregate / registry / LogicWorkQueue / MeetingService
A4  MeetingService 与 meeting/* 零改动：git diff --stat -- meeting/ 为空
A5  只存在一条 FIFO、一个 work 消费者：LogicWorkQueue 实例与 std::thread（work 消费）
    的生产命中数不增加
A6  adapter 不向任何队列投递 work：新增代码中无 Enqueue / Push / PostInternalWork
    （除既有 FINALIZE_CLOSE 那一处）
A7  无新增 timer / background thread / 清理线程
A8  internal work payload 仍只含稳定 ID；无伪 TCP msg_id、无假 LogicNode
A9  MeetingView 的每次字段读取都位于 has_view == true 分支内（§10.5 的 grep 检查）
A10 message.proto / *.sln / *.vcxproj / *.props / *.filters / MyChat_Qt / ChatServer2 零改动
```

### 18.2 行为类（测试可核）

```text
B1  Layer C 组 1–7 全部通过（§14.2 的 34 个用例，可按表驱动合并但语义不得减少）
B2  既有 224 个测试**一个不改、一个不少**，全部通过
B3  Layer A 中与通知无关的断言保持通过（STEP-1.6 §25.7 明写）
B4  通知失败不回滚状态：X11 / ND4 通过（finalized==true、HasClosedSnapshot==true、
    MeetingClosed 恰好一次）
B5  ENDING → CLOSED 必然收敛：即使所有通知目标消失、channel 返回 false、channel 抛异常，
    收敛条件仍逐一成立（BF1/BF2/ND2）
B6  session lookup 失败不改变领域结果：SS4/SS5 通过
B7  CLOSING/CLOSED ⇒ SESSION_STATE_REJECTED（不是 AUTH_REQUIRED）：SS1/SS2 通过
B8  安全边界：MV2/MV3/MV4 通过（授权失败路径不泄露 host_user_id / 成员数）
B9  顺序：response ordering == enqueue ordering（OR1）；FINALIZE_CLOSE 仍不插队（OR2）
B10 adapter 无重试、无 ACK：BF4 通过（同一目标同一次通知恰好一次调用）
```

### 18.3 工程类

```text
C1  Linux out-of-tree 全新构建 PASS（BUILD_TESTING=ON 与 OFF 两种配置）
    ——警告不得新增（允许保留既有 MysqlDao.cpp:95）
C2  ctest 全绿；用例总数 = 224 + M6 新增数（M6 必须给出该数字并核对）
C3  C++14 门禁：新文件在 -std=c++14 -pedantic-errors 下通过；无 C++17 能力
C4  依赖隔离：MeetingDomain 仍不链接任何东西；adapter TU 不引入新第三方依赖
C5  runtime smoke：进程起停正常；无 terminate/abort/exception；
    新增"adapter 已注入且 directory 非空"的可观测证据（R-3 的缓解措施）
C6  仓库内无 build artifact / config.ini / 日志 / 密码进入版本控制
C7  Windows Debug|x64：**按 §19 Conflict #3 的裁决结果**执行
    （若裁决为"仍 OUT OF SCOPE"，则如实记为 NOT VERIFIED，不得声称通过）
```

### 18.4 文档类

```text
D1  本文档状态更新为 COMPLETE，并补入：实际改动清单、测试结果、验证边界、已知限制
D2  如实记录：M6 完成后 response 的最外层出口仍留在 Phase 3
D3  如实记录 §17.2 的四条验证边界
D4  若发生任何对 §15 Scope Matrix 的偏离，必须逐条记录并获得授权
```

---

## 19. Conflicts Requiring Human Adjudication

本节记录本轮 Planning 发现的、**不得自行修正**的冲突与取舍点。

### Conflict #1（本步新增）：`CSession::Send` 需要 message ID，而 M6 禁止分配 message ID

```text
冲突：CSession::Send(std::string, short msgid) 的既有签名要求一个 short 协议号；
      STEP-1.6 §25.8 与 M6 Files forbidden 明确禁止"TCP message ID 分配"。
选项：
  1a. 在 adapter 内使用一个**具名 owner-document 占位常量**，注释标明非 wire ID、
      Phase 3 必须替换，且该常量不得出现在 meeting/ 或 LogicWork*/ 内；
  1b. 完全不构造帧：adapter 只做 adapter 所需范围内的事，把 payload 交给
      IMeetingOutboundChannel，由 Phase 3 的 wire adapter 决定帧与协议号。
倾向：1b（最干净、零协议污染）；1a 更接近"可运行"，但会引入一个必须被清理的占位值。
**需要人工裁决。**
```

### Conflict #2（本步新增）：偏离 STEP-1.6 §15.2 建议的 `FindSession → shared_ptr<CSession>`

```text
冲突：§15.2 的措辞是 "FindSession(session_id) → std::shared_ptr<CSession>（或等价受控 lookup）"，
      且 STEP-1.6 §25.7 的 Files allowed 引用的是 "FindSession 类能力"。
本文档采用 `IMeetingOutboundChannel*`（非拥有借用）+ `IMeetingSessionDirectory` 窄接口。
理由见 §8.2（所有权语义、依赖面、可测性三条）。
"（或等价受控 lookup）"这一措辞为偏离留了空间，但**需要人工确认**该形式是否被视为等价。
```

### Conflict #3（继承，本步确认仍存在）：M6 的 Build gate 与 Windows R6 / Files forbidden 互相矛盾

```text
冲突：
  - STEP-1.6 §25.7 Build gate = "Debug|x64 编译通过"；
  - 但当前 dev HEAD 在 Windows 上预期**链接失败**（R6 OPEN：LogicWorkDispatcher.cpp 与
    meeting/*.cpp 不在 ClCompile；见 §18 R-5 与 M4 §17.1、M5.1 §11.1）；
  - 且 §25.7 Files forbidden 含 `PropertySheet.props`，而补齐 R6 需要动工程文件。
因此"满足 Debug|x64 gate"与"不扩大范围"在当前基线上**不可能同时成立**。
处置：不自行修正 Phase 1、不自行 workaround。请裁决以下之一：
  （i）M6 的 Build gate 改为 Linux CMake 构建（并显式记录 Windows NOT VERIFIED）；
  （ii）把 R6 补齐作为 M6 的授权范围扩展（需明确允许修改 *.vcxproj / *.props / *.filters）；
  （iii）M6 期间保持 R6 DEFERRED，Build gate 按 (i) 执行，Windows 验证留到独立一步。
倾向：(iii)。**需要人工裁决。**
```

### Conflict #4（继承，本步确认）：`MeetingRequestContext` builder 在 M6 没有生产调用者

```text
冲突：M6 的 Integration tests 要求覆盖 "MeetingRequestContext 构造"，
      但生产侧唯一合法调用者是 wire adapter（Phase 3，明确 Deferred）。
处置：STEP-1.6 §25.7 的条件说明允许"只通过测试 adapter / direct invocation 验证"。
因此 M6 交付 builder + 其测试，并在文档中如实记录"生产调用者在 Phase 3 接入"。
**不构成阻塞**，但需确认该边界被接受（本文档 §17.2(a)(b) 已记录）。
```

---

## 20. Implementation Plan

### 20.1 建议的编码切分（每一步都可独立 Review / Build / Test / 回退）

| 步 | 内容 | 产出 | 回退边界 |
| --- | --- | --- | --- |
| **M6.0** | 只读复核本文档与基线（HEAD、224/224、构建 PASS），确认 §19 三个冲突的裁决 | 复核记录（可并入 M6 文档） | 无代码改动即无需回退 |
| **M6.1** | 新增 `MeetingOutboundChannel.h`（两个窄接口，仅标准库）+ C++14 门禁 | 新头文件 | 删除该文件 |
| **M6.2** | 新增 `MeetingRequestContextBuilder.h` + Layer C 组 1 用例 | builder + CB1–CB6 | 删除新文件与用例 |
| **M6.3** | `CServer` 实现 `IMeetingSessionDirectory`（**`_mutex` 下 find**）；`CSession` 实现 `IMeetingOutboundChannel`（noexcept 委托 `Send`）；Layer C 组 2 | 两个既有文件的最小改动 + SL1–SL5 | 回退 `CServer.*` / `CSession.*` |
| **M6.4** | 新增 `MeetingResponseNotificationAdapter.h/.cpp`（response 编码 + 安全矩阵 + notification best-effort + 异常边界 + 只读计数）；Layer C 组 3/4/5 | adapter + ND/BF/MV 用例 | 删除 adapter 文件与用例 |
| **M6.5** | `LogicWorkDispatcher` 增加 adapter 指针与六个 `Dispatch*` + `FINALIZE_CLOSE` 调用点；保持 sink 观测顺序与 FINALIZE_CLOSE 入队顺序不变；Layer C 组 6/7 | dispatcher 改动 + SS/OR 用例 | 回退 `LogicWorkDispatcher.*` |
| **M6.6** | `LogicSystem` 新增 adapter 成员与一次性注册入口；`EnsureWorkQueueStarted` 内装配；`CServer`/`main` 完成注册调用点（**必须早于 worker 线程创建**） | `LogicSystem.*` + 注册点 + CMake | 回退 `LogicSystem.*` / `ChatServer.cpp` / CMake |
| **M6.7** | 全量验证：Linux 双配置构建、ctest（224 + 新增）、C++14 门禁、依赖隔离、runtime smoke（含 adapter 注入证据）、整轮重复 N 次并区分既有 flaky | 验证记录 | 回退该步改动 |
| **M6.8** | 文档收尾：更新本文档为 COMPLETE，补 Scope Review / 验证边界 / 已知限制 / 人工提交命令 | 文档 | 无需回退 |

### 20.2 每步的强制 Review focus

```text
M6.1–M6.2：接口是否只依赖标准库；是否引入了任何协议号
M6.3     ：find 是否在 _mutex 下；是否**没有**改 ClearSession 的既有行为；
           SendToSession 是否 noexcept 且未改变 legacy Send 语义
M6.4     ：安全矩阵逐格核对（§10.3）；无重试/无 ACK；异常不外逸；计数不被用于判定
M6.5     ：O1/O2/O3 三条顺序证明（§11.2）；FINALIZE_CLOSE 仍不插队；sink 观测顺序不变
M6.6     ：§12.3 的顺序 + 可见性 + 幂等三重论证；注册必须早于 worker 线程创建
M6.7     ：既有 224 用例零改动；新增 flaky 与既有 flaky #1/#2 明确区分
M6.8     ：验证边界如实标注（§17.2），不得声称端到端可用
```

### 20.3 回退策略（整体）

```text
M6 的全部改动集中在：
  新增 4 个文件（2 个接口/头 + adapter.h + adapter.cpp + builder.h + 测试）
  修改 8 个既有文件（LogicWorkDispatcher.h/.cpp、LogicSystem.h/.cpp、CServer.h/.cpp、
                    CSession.h/.cpp）+ 2 个 CMakeLists
因此整体回退 = 删除新增文件 + `git checkout --` 上述既有文件 + 回退两个 CMakeLists。
Sink 观测路径、领域层、FIFO/executor、internal work payload 均不受影响
（MeetingDomain 目标本轮零改动）。
```

### 20.4 人工提交命令（M6 Coding 完成后由人工执行；本步**不**执行）

```bash
cd /home/tobeki/projects/rtc-signaling-platform

git status --short
git diff --check
git diff --stat
git --no-pager diff

# 预期新增
#   Server/ChatServer/ChatServer/MeetingOutboundChannel.h
#   Server/ChatServer/ChatServer/MeetingRequestContextBuilder.h
#   Server/ChatServer/ChatServer/MeetingResponseNotificationAdapter.h
#   Server/ChatServer/ChatServer/MeetingResponseNotificationAdapter.cpp
#   Server/ChatServer/ChatServer/tests/MeetingResponseNotificationAdapterTest.cpp
#   docs/phase2/M6-response-notification-adapter.md

# 预期修改
#   Server/ChatServer/ChatServer/LogicWorkDispatcher.h / .cpp
#   Server/ChatServer/ChatServer/LogicSystem.h / .cpp
#   Server/ChatServer/ChatServer/CServer.h / .cpp
#   Server/ChatServer/ChatServer/CSession.h / .cpp
#   Server/ChatServer/ChatServer/CMakeLists.txt
#   Server/ChatServer/ChatServer/tests/CMakeLists.txt

git add <以上路径>          # 逐项显式列出，不使用 git add -A
git diff --cached --stat
git --no-pager diff --cached
git commit -m "feat(chatserver): add meeting response and notification adapter"
git push origin dev

git status --short
git rev-parse HEAD
git rev-parse origin/dev
git rev-list --left-right --count origin/dev...HEAD
```

### 20.5 本步（Planning）的提交边界

```text
本步只新增 docs/phase2/M6-response-notification-adapter.md，不改任何既有文件。
是否提交本文档由人工决定；本步**未** git add / commit / push。
```

---

## 21. Explicit Non-Goals

```text
1.  不实现 TCP Meeting Protocol、不分配任何 TCP message ID、不做 JSON/protobuf 映射、
    不定义 Frame V2、不编码 request_id 的 wire 形式                → Phase 3（STEP-1.6 §25.8）
2.  不做真实客户端端到端（Layer D）                                → 协议 adapter 落地后
3.  不修改领域语义：ResultCode 不新增/不重编号、事件契约不变、
    状态机不变、Close 两 turn 不变、MeetingView 填充规则不变         → Phase 1 冻结
4.  不引入第二 FIFO、第二 worker、线程池、timer、后台清理            → STEP-1.6 §12.1 / M4 §5.1
5.  不引入 ACK / retry / 死信 / 可靠投递 / 通知批量合并              → STEP-1.6 §15.4
6.  不让 MeetingService 持有 CServer、不直接操作 CSession、
    不把 CSession 放进 LogicWorkItem、不伪造 msg_id、不包装假 LogicNode → M4 §4.2 / §12.5
7.  不把生产 response/notification 挂到 IWorkObservationSink 上      → M4 §4.5 契约
8.  不给 MeetingService 加锁、不引入线程、不引入 async task          → Step 1.3 §20 / M3
9.  不修既有 flaky #1/#2、不修 ClearSession 的 find 锁缺陷、
    不初始化 CSession::_user_uid（M6 不读它）                        → 既有债，单独处理
10. 不处理 Windows R6、不添加 tombstone TTL/LRU/timer/background cleanup、
    不做 ClosedSnapshot retention/淘汰                              → DEFERRED（§15.4）
11. 不改 message.proto、不改 *.sln/*.vcxproj/*.props/*.filters、
    不改 MyChat_Qt/、不改 Server/ChatServer2/                       → §15.3 MUST NOT
12. 不改既有 7 个测试文件、不减少既有 224 个测试                      → §18.2 B2
13. 不引入新的第三方依赖、不使用 C++17 能力                          → STEP-1.6 §7 / §15
14. 不在本步（Planning）实现任何代码                                  → §2.3
```

---

## 22. 下一步

```text
1. 人工 Review 本文档，并对 §19 的四个 Conflict 给出裁决
   （尤其 Conflict #1 与 #3，它们直接决定 M6 的 Build gate 与"是否有 message ID 占位常量"）；
2. 裁决后按 §20.1 切分为独立的 M6.x Coding Prompt；
3. 每个 M6.x 完成后 → Review → Build → Test → 再进入下一步；
   不得把 M6.1–M6.8 合并成一个 Prompt（STEP-1.6 §26.2 同一纪律）。
```
