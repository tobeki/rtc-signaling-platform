# Phase 2 - M6：Response / Notification Adapter

> 状态：**DESIGN COMPLETE（M6.0 Architecture Adjudication 已裁决；Coding NOT STARTED）**
> Phase 2 M-planning 原始基线：`da19189a55ab13058a22c38c228d3043df72bdf6`（M5.1 收尾）
> 本文档首次提交：`afad0644170b0882ee6b58ed99c493e82909a70f`（`docs(phase2): add M6-response-notification-adapter.md`）
> 本文档当前修订：**M6.0 设计修订**（工作区状态见 §1.3 与 §24）
> M6 Coding：**NOT STARTED**

---

## 1. Status

### 1.1 里程碑与文档状态

| 项 | 值 |
| --- | --- |
| Phase 2 M-planning 原始基线 | `da19189a55ab13058a22c38c228d3043df72bdf6`（HEAD == origin/dev，M5.1 收尾） |
| 本文档首次提交（Phase 2 M6 Planning） | `afad0644170b0882ee6b58ed99c493e82909a70f` — `docs(phase2): add M6-response-notification-adapter.md` |
| 本修订轮次 | **M6.0 — Architecture Adjudication & Design Revision** |
| 本修订依据 | 对 `afad064`（文档）+ 代码（`da19189`..`afad064` 期间代码未变）的只读复核；**未修改任何代码** |
| M6 Coding | **NOT STARTED** |
| 生产代码 / 测试代码 / CMake / Phase 1 文档 | **本轮零修改** |

### 1.2 本修订解决的问题（M6.0 六项）

```text
1. TCP message ID 与出站接口：剔除占位 msgid，改为严格 transport-neutral 的 typed 出站边界（§6、§7）
2. Session lookup 生命周期：裸借用指针 → weak_ptr 判活 + shared_ptr 强句柄（§8）
3. worker / CServer / io_context 关闭顺序：显式 ShutdownWorkExecutor + weak 目录（§12）
4. Response 原始 Session：adapter 接口增加可信 MeetingRequestContext（§6.3）
5. 发送失败可观测性：五档分辨率的显式边界，不再声称可观测送达（§13.3）
6. Context Builder 身份来源：只用服务端可信身份，绝不读 _user_uid（§14）
```

### 1.3 提交状态（不得提前声称已推送）

```text
本轮（M6.0）只修改本文件 docs/phase2/M6-response-notification-adapter.md。

本地提交  : 存在一条本地 commit，subject 为
            "docs(phase2): adjudicate M6 design - transport-neutral outbound boundary"
            （具体 hash 以 `git log -1` 为准；本文档刻意不内嵌自己的 commit hash，
              否则任何 amend 都会让它立刻过期）
推送状态  : **NOT PUSHED** —— `git push origin dev` 失败：
            "Failed to connect to 127.0.0.1 port 7897"（WSL 内配置的 HTTP 代理未运行）
            直连 github.com 亦不可达（curl 返回 000）；无 credential.helper、无 SSH key。
            ⇒ 修订已安全落盘于本地 commit，但**尚未到达 origin/dev**。

待网络恢复后由人工执行：
            git push origin dev
            # 若代理端口变化，先 git config http.proxy/https.proxy 指向可用代理

注意：本文档此前版本曾把 `afad064` 描述为"已提交"，那是**上一轮**的事实；
本轮修订（上面那条本地 commit）不含在内，直到 push 成功为止。
```

### 1.4 本修订对 Phase 1 的地位声明

```text
Phase 1（docs/phase1/STEP-1.1 ~ STEP-1.6）**仍然完全冻结，未被修改**。
§19 的四个"裁决"是**本项目在 M6 范围内对 Phase 1 条款的执行方式选择**，
不是对 Phase 1 原文的修改、扩充或豁免。
凡裁决与 Phase 1 字面表述存在张力之处，§19 均逐条标注"Phase 1 原文未变"。
```

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
条件说明            ：若外部 Meeting command 尚无正式 Protocol V2，可只通过
                      测试 adapter / direct invocation 验证
```

### 2.2 本轮（M6.0）范围

```text
只修订本文档：解决六项设计问题、记录四个 Conflict 裁决、消除内部矛盾、
              更新元数据与验收门禁
```

### 2.3 本轮明确不做

```text
不修改任何生产代码 / 测试代码 / CMake / Phase 1 文档 / Windows 工程文件 / 其他文档
本轮末尾按 §24 执行只读 git 命令；add/commit 由人工授权后执行
不创建占位 TCP message ID、不定义 Meeting wire format
不引入第二 FIFO、第二 worker、timer 或后台线程
不开始任何 M6 实现
```

### 2.4 授权范围的唯一产物

```text
MODIFIED: docs/phase2/M6-response-notification-adapter.md   （本文件）
```

**若 M6 Coding 需要超出 §15 Scope Matrix 的 MUST / MAY 集合，必须在 Coding 前单独裁决。**

---

## 3. Current Architecture

### 3.1 执行上下文（M4 / M5.1 冻结事实，本轮代码复核）

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

惰性启动（**关键**事实）：`_dispatcher` 与 `_work_executor` 都在
`LogicSystem::EnsureWorkQueueStarted()` 的 `std::call_once` 内构造，**生产 sink 为 `nullptr`**：

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
`:483`（`PostMeetingCommandWork`）。
⇒ 唯一 FIFO worker 的创建时刻 = 第一次真实入队时刻。

### 3.2 四条 internal ingress 与一条 external ingress

```text
CServer::HandleAccept 成功分支   → LogicSystem::PostSessionRegisterWork        CServer.cpp:42
LogicSystem::LoginHandler 成功尾部 → PostSessionAuthenticateWork               LogicSystem.cpp:130
CSession 终止（7 条路径）         → SessionDisconnectBridge → PostSessionDisconnectedWork
                                                                              CSession.cpp:88/146/160
DispatchClose（should_schedule_finalize）→ PostInternalWork → 同一 FIFO        LogicWorkDispatcher.cpp:275-277
legacy IM 消息                    → PostMsgToQue → EXTERNAL_MESSAGE            CSession.cpp:201
```

### 3.3 领域输出的当前类型与归属（逐项，含行号）

| 输出 | 类型定义 | 生产者 | 当前消费者 | 是否编码 | 是否发送 |
| --- | --- | --- | --- | --- | --- |
| `MeetingRequestContext` | `MeetingTypes.h:188-201` | **无生产构造者**（仅测试 `MakeContext`） | `MeetingService` 六项 API | 否 | 否 |
| `MeetingServiceResultCommon` | `MeetingService.h:119-128` | `MeetingService::MakeCommon` | `LogicWorkDispatcher::MakeCommandOutcome` | 否 | 否 |
| `CreateMeetingResult` | `MeetingService.h:145-151` | `MeetingService::CreateMeeting` | `DispatchCreate` 摘要 | 否 | 否 |
| `JoinMeetingResult` | `MeetingService.h:153-160` | `MeetingService::JoinMeeting` | `DispatchJoin` 摘要 | 否 | 否 |
| `LeaveMeetingResult` | `MeetingService.h:162-168` | `MeetingService::LeaveMeeting` | `DispatchLeave` 摘要 | 否 | 否 |
| `CloseMeetingResult` | `MeetingService.h:170-183` | `MeetingService::CloseMeeting` | `DispatchClose` 摘要 | 否 | 否 |
| `QueryMeetingResult` | `MeetingService.h:185-188` | `MeetingService::QueryMeeting` | `DispatchQuery` 摘要 | 否 | 否 |
| `ListParticipantsResult` | `MeetingService.h:190-196` | `MeetingService::ListParticipants` | `DispatchListParticipants` 摘要 | 否 | 否 |
| `FinalizeCloseResult` | `MeetingService.h:201-220` | `MeetingService::FinalizeClose` | `DispatchWork` 的 `FINALIZE_CLOSE` 分支 | 否 | 否 |
| `SessionDisconnectedResult` | `MeetingService.h:226-240` | `MeetingService::SessionDisconnected` | `LogicWorkDispatcher::SessionDisconnected` | 否 | 否 |
| `MeetingView` | `MeetingTypes.h:209-221` | `MeetingAggregate::BuildMeetingView` / `MeetingService::BuildClosedMeetingView` | 仅摘要为 `meeting_state`/`has_closed_at`/`active_participant_count` | 否 | 否 |
| `ParticipantView` | `MeetingTypes.h:229-240` | `MeetingAggregate` | **无**（`ListParticipantsResult.participants` 在 dispatcher 内被丢弃） | 否 | 否 |
| `MeetingEvent` | `MeetingTypes.h:301-338` | `MeetingAggregate` / `MeetingService` | 仅降级为 `WorkDomainEvent`（type + ids） | 否 | 否 |
| `notification_session_ids` | `MeetingService.h:219` | `MeetingService.cpp:945`（唯一赋值点） | `RecordFinalizeClose` 的**观测**参数 | 否 | **否，永远是 intent** |
| `ClosedMeetingSnapshot` | `MeetingTypes.h:269-283` | `MeetingService::FinalizeClose` | 仅 `has_snapshot` 布尔被观测 | 否 | 否 |

### 3.4 决定 M6 形态的关键事实（本轮重新核对）

**(F1) 生产侧没有任何 notification 投递路径。**
`notification_session_ids` 唯一赋值点 `MeetingService.cpp:945`，唯一下游读者
`LogicWorkDispatcher.cpp:162` 交给 `sink_->RecordFinalizeClose(...)`，而生产 `sink_` 是 `nullptr`。
链路精确为 `intent → null`。`IWorkObservationSink` 的**生产实现数为 0**
（三个实现全在 `tests/`：`LogicWorkQueueTest.cpp:64`、`DomainSessionLifecycleTest.cpp:138`、
`SessionDisconnectBridgeTest.cpp:109`）。

**(F2) 观测摘要不足以承载 response。**
`MeetingCommandOutcome`（`LogicWorkItem.h:243-267`）只含
`command / result_code / result_name / disposition / has_meeting_id / meeting_id /
meeting_state / has_closed_at / active_participant_count / event_types`。
它**丢弃** `ListParticipantsResult.participants`，并把 `MeetingEvent` 降级为仅 `event_type`
（`LogicWorkDispatcher.cpp:284`、`:298`）。
⇒ adapter 不能只消费观测摘要，必须消费 typed result struct。

**(F3) 领域结果类型是完整、值语义、可安全复制的。**
六项 API result、`FinalizeCloseResult`、`SessionDisconnectedResult` 均只含值类型。

**(F4) `CServer` 无任何 session lookup 能力，`_sessions` 私有。**

```cpp
// CServer.h:20-24
boost::asio::io_context& _io_context;
short _port;
tcp::acceptor _acceptor;
std::map<std::string, std::shared_ptr<CSession>> _sessions;   // private，value = shared_ptr
std::mutex _mutex;
```

访问纪律：`insert` 持 `_mutex`（`CServer.cpp:31-34`）；`erase` 持 `_mutex`（`:67-70`）；
`find` **未持锁**（`:60`，`ClearSession` 内）。`_sessions` 的 value 是 `shared_ptr<CSession>`
——这一点是 M6.0 生命周期方案的基础（§8）。

**(F5) `CSession::Send` 的既有语义（M6 不得改变，也不得用它承载 Meeting payload）。**

```cpp
void Send(std::string msg, short msgid);   CSession.cpp:44
  ├── 持 _send_lock
  ├── if (send_que_size > MAX_SENDQUE /* 1000 */) { 只打印一行日志; return; }   ← 静默丢弃，返回 void
  ├── push SendNode(msg, len, msgid)  ← 4 字节头：2B msgid + 2B length（HEAD_TOTAL_LEN=4）
  └── 通过 SharedSelf() + async_write 发送
```

`HandleWrite` 的写失败路径自行调用 `Close()`（`CSession.cpp:245-249`），**不回传给调用方**。

**(F6) `main` 的实际生命周期拓扑（本修订更正的重要事实）。**

```cpp
// ChatServer.cpp:41-56
boost::asio::io_context io_context;
boost::asio::signal_set signals(io_context, SIGINT, SIGTERM);
signals.async_wait([&io_context, pool, &server](auto, auto) {
    io_context.stop();
    pool->Stop();      // 内部对每个 io_context stop + 对每个 _threads join（AsioIOServicePool.cpp:41-49）
    server->Shutdown();
});

CServer s(io_context, atoi(port_str.c_str()));   // ← **栈对象**，局部于 try 作用域
io_context.run();

RedisMgr::GetInstance()->HDel(LOGIN_COUNT, server_name);
RedisMgr::GetInstance()->Close();
grpc_server_thread.join();
```

由此推出三条硬事实：

```text
(F6-a) CServer 是局部栈对象，在 try 作用域结束时析构；而 LogicSystem 是 Singleton，
       其析构发生在静态存储期结束（**晚于** CServer），且唯一 worker 的 StopAndJoin 目前
       **只**发生在 ~LogicSystem（LogicSystem.cpp:496-504）。
       ⇒ 现有代码下 worker 可能在 CServer 析构之后仍然存活。这是 M6 必须解决的生命周期缺口。

(F6-b) AsioIOServicePool::Stop() 会 join 全部 io_context 线程（AsioIOServicePool.cpp:41-49），
       因此它返回之后**网络回调已全部结束**，没有 handler 正在执行。
       这是"网络生产者已停止"的可证明依据（§12.3）。

(F6-c) CServer 继承 enable_shared_from_this<CServer>（CServer.h:10），但全仓**没有任何**
       CServer::shared_from_this() 调用；唯一构造点是 ChatServer.cpp:50 的栈对象。
       ⇒ 它是"防御性继承"，当前未生效。M6.0 利用它建立 weak 目录（§8.4），
         并把构造改为 make_shared（把潜在缺陷变成正确用法）。
```

**(F7) `CSession::_user_uid` 在构造时未初始化**（`CSession.h:51`）。
M6 的 context builder **绝不**读取它。

---

## 4. Problem Statement

### 4.1 P1：生产侧没有 notification 投递路径（F1）

`notification_session_ids` 是 M3 冻结的**意图**输出，M4 从未把它变成行为。
M6 必须给它一条真实、唯一、可测试的路径，并回答"它在哪一个 serialized boundary 之后发生"。

### 4.2 P2：没有 session lookup，且裸指针不安全（F4）

领域只认识 `session_id`；`CServer::_sessions` 私有且无查询接口。
M6.0 在初版基础上追加要求：**lookup 必须返回生命周期受控的句柄**（§8）。

### 4.3 P3：没有 response 编码面，且 wire 交付必须留给 Phase 3

§25.7 要求"领域结果可回到调用侧"，§25.8 把
`TCP message ID` / `JSON` / `protobuf` / `Frame V2` / `request_id encoding` 划为 Deferred。

**M6.0 裁决（替代初版 1a/1b 二选一）**：`CSession::Send` 的 `short msgid`
**不可能**在"不分配协议号"的前提下被合法使用。因此 M6 的输出边界必须
**完全不经过 `CSession::Send`**，改为严格 transport-neutral 的 typed 出站值对象（§6、§7）。
这同时消除了初版遗留的"占位 msgid"问题（1a 被否决，1b 被强化到彻底）。

### 4.4 P4：`MeetingRequestContext` 的生产构造点缺失

六项 API 只接受由 transport adapter 构造的 context（`MeetingService.h:277-279`），
生产侧没有任何地方构造它（`PostMeetingCommandWork` 生产零调用点）。
M6 交付 builder，并明确其生产调用者在 Phase 3 之前**不存在**（§14）。

### 4.5 P5：response 无法知道原始 Session（初版缺陷）

初版 `OnCreateMeeting(const CreateMeetingResult&)` 无法取得发起命令的 `session_id`，
因此**根本无法投递 response**（领域 result 里没有、也不应有发起者身份）。
修订：adapter 接口同时接收可信 `MeetingRequestContext`（§6.3）。

### 4.6 P6：发送失败不可观测，而初版暗示了过度能力（初版缺陷）

`CSession::Send` 返回 `void`（F5）。M6.0 明确列出五档可观测性，并**声明**
"异步写失败"与"客户端送达"两档在 M6 **不可统计、不可验证**（§13.3）。

### 4.7 由此得到的 M6 准确定位

```text
M6 = 把"领域结果 → 调用侧"的适配能力做成
     **已实现、已注入、被真实 LogicWorkDispatcher 调用、可被 Layer C 确定性验证**的一层，
     产出**严格 transport-neutral 的 typed 出站值对象**，
     并具备**真实可用的 lifetime-safe session lookup**；
     但它的**实际字节传输能力明确留待 Phase 3**。
```

---

## 5. Adapter Boundary

### 5.1 候选方案比较（含 M6.0 修订）

| 方案 | 结构 | 优点 | 缺点 | 结论 |
| --- | --- | --- | --- | --- |
| A. adapter 作 `LogicWorkDispatcher` 成员 | 内联成员 | 最少新类型 | dispatcher 头被迫依赖 adapter 与 asio；单测 target 被污染 | 否 |
| B. adapter 经**抽象接口**注入 dispatcher | dispatcher 只认识纯接口；实现独立 TU | 头零重量依赖；可注入 recording 替身；rollback 清晰 | 多一个接口类型 | **采用** |
| C. adapter 作 `IWorkObservationSink` 生产实现 | 替代 `nullptr` | 零新接口 | **违反 M4 §4.5** 纯观测契约；摘要不含 `ParticipantView`/完整 `MeetingEvent`（F2） | 否 |
| D. 独立 service + 第二 FIFO/timer | 第二队列/worker | 形式解耦 | 违反 STEP-1.6 §12.1 / M4 §5.1 | 否 |
| E. 领域层直接回调 adapter | `MeetingService` 依赖 adapter | 少一层 | 违反 §6.1 依赖方向与 §15.3 | 否 |

### 5.2 最终结构（M6.0）

```text
LogicWorkDispatcher（消费者线程，唯一 Meeting mutation 上下文）
        │ 持有两个非拥有裸指针（均可为 nullptr）
        ├── IMeetingResponseNotificationAdapter*  adapter_
        │        │  实现在 adapter TU
        │        ▼
        │   MeetingResponseNotificationAdapter
        │        ├── IMeetingOutboundMessageCollector&        ← 出站值对象的**唯一**接受者
        │        └── std::weak_ptr<IMeetingSessionDirectory>  ← 判活式目录引用
        │                 └── lock() 成功 ⇒ 真实 lookup（CServer 实现）
        │
        └── IWorkObservationSink* sink_（既有，纯观测，生产 nullptr，**不承载行为**）
```

三条硬性结构约束（可静态复核）：

```text
S1  adapter 不认识 MeetingAggregate / registry / LogicWorkQueue / LogicSystem
S2  adapter 不持有 MeetingService、不调用任何领域 mutation
S3  MeetingService 不持有 CServer、不认识 adapter、不认识 session；meeting/* 零改动
```

### 5.3 为什么依赖方向必须是 "dispatcher → 接口 ← 实现"

`LogicWorkDispatcher.cpp` 刻意只依赖 MeetingDomain + 标准库 + Boost.UUID，
并明确不 include `CSession.h` / asio（`LogicWorkDispatcher.cpp:10-12` 注释）。
若 adapter 内联为成员，其依赖会经头文件传递，把 asio/Boost 拖进
`ChatServerUnitTests`（该 target 刻意排除 `LogicSystem.cpp` / `CServer.cpp` / `CSession.cpp`，
见 `tests/CMakeLists.txt` 顶部注释）。
方案 B 让 dispatcher 只多认识**不含第三方 include** 的纯接口头。

### 5.4 adapter 是否允许持有 `CServer`

**不允许持有 `CServer` 类型。** M6.0 进一步收紧为
`std::weak_ptr<IMeetingSessionDirectory>`（§8.4）：

```text
- 窄接口（只含 <memory> / <string>），不含 asio / boost / CSession
- weak_ptr 语义：必须先 lock() 成功才可用；lock 失败 ⇒ 明确 no-op
- 因此 adapter 在 CServer 析构之后**不可能**访问已销毁对象（不存在悬垂引用）
```

`grep -n "CServer" MeetingResponseNotificationAdapter.h` 必须为 **0 命中**。

---

## 6. Response Path

### 6.1 语义定位

```text
Response    ：某个**外部命令**的直接请求-应答结果。
              唯一 target = 发起该命令的那个 session（= 该命令 MeetingRequestContext.session_id）。
              每个命令恰好 0 或 1 个 response。
              request_id 仅用于 correlation，**不是**幂等键。
Notification：某个**领域状态变化**需要告知的**其他** session 集合。
              与任何单一请求无关；target 是领域输出的 session_id 集合。
              数量 0..N，允许目标不存在（best-effort skip）。
```

冻结依据：STEP-1.6 §15.1、§15.4、§16.1。

### 6.2 哪些结果有 response

| 领域操作 | 有 response? | 依据 |
| --- | --- | --- |
| `CreateMeeting` | ✅ | 六项外部 API 之一 |
| `JoinMeeting` | ✅ | 同上 |
| `LeaveMeeting` | ✅ | 同上 |
| `CloseMeeting` | ✅（**turn 1**，返回 `ENDING` + `CLOSE_STARTED`） | STEP-1.6 §16 |
| `QueryMeeting` | ✅ | 同上 |
| `ListParticipants` | ✅（含 `participants`） | 同上 |
| `FinalizeClose` | ❌ **无** | STEP-1.6 §16.1："不是外部 API，无客户端 Response" |
| `SessionDisconnected` | ❌ | 内部 work，无请求方 |
| `RegisterConnectedSession` / `AuthenticateSession` | ❌ | internal lifecycle |

### 6.3 response target 的数据流（M6.0 解决 P5）

**问题**：领域 result 不携带发起者 session，也不应携带（领域不认识传输）。

**裁决**：adapter 的每个 `On<API>` 同时接收**可信 `MeetingRequestContext`**——
它就是**同一个** `LogicWorkItem::meeting_context`：

```text
1. 由 transport adapter 构造（服务端事实，客户端不得覆盖；MeetingTypes.h:180-186）
2. 已被 MeetingService::ValidateContext 校验（request_id/session_id/handling 非空、
   handling == 本节点、身份与领域 Session 记录一致；MeetingService.cpp:117-185）
3. 与传给 MeetingService 的是**同一个值**（同一 Dispatch* 内的同一个 item.meeting_context）
```

因此 `context.session_id` 是**可信的 response target**，且**不需要**领域 result 携带它，
**不需要**修改 `MeetingService`，**不需要**在 `LogicWorkItem` 放 `CSession`。

```text
数据流（response）：

[Phase 3: CSession → wire adapter 构造 context]
MeetingRequestContextBuilder::Build(session_id, authenticated_user_id, request_id, local_server_id)
   └─ MeetingRequestContext（服务端可信值）
LogicWorkItem{ MEETING_COMMAND, meeting_context, command, meeting_id }
   └─ 唯一 FIFO → 唯一消费者
LogicWorkDispatcher::Dispatch<API>(item)
   ├─ result = meeting_service_.<API>(item.meeting_context, ...)
   ├─ sink_->Record*（若 sink != nullptr；纯观测，不变）
   └─ adapter_->On<API>(item.meeting_context, result)        ← M6 新增
          ├─ 构造 MeetingResponseMessage（typed，§7.2）
          │     └─ response_target_session_id = context.session_id
          └─ collector_->Collect(message)                    ← 唯一出站接受者
```

**失败路径同样成立**：命令被拒绝时（`PERMISSION_DENIED` / `AUTH_REQUIRED` / …）
response 仍要回到**同一个** `context.session_id`——错误必须能回给发起者。
因此成功/失败路径使用**同一条** target 来源。

### 6.4 adapter 接口（M6.0 修订）

```cpp
class IMeetingResponseNotificationAdapter {
public:
    virtual ~IMeetingResponseNotificationAdapter() {}

    // 六项外部 API：同时接收可信 context（提供 response target）与 typed result。
    virtual void OnCreateMeeting(const meeting::MeetingRequestContext& context,
                                 const meeting::CreateMeetingResult& result) = 0;
    virtual void OnJoinMeeting(const meeting::MeetingRequestContext& context,
                               const meeting::JoinMeetingResult& result) = 0;
    virtual void OnLeaveMeeting(const meeting::MeetingRequestContext& context,
                                const meeting::LeaveMeetingResult& result) = 0;
    virtual void OnCloseMeeting(const meeting::MeetingRequestContext& context,
                                const meeting::CloseMeetingResult& result) = 0;
    virtual void OnQueryMeeting(const meeting::MeetingRequestContext& context,
                                const meeting::QueryMeetingResult& result) = 0;
    virtual void OnListParticipants(const meeting::MeetingRequestContext& context,
                                    const meeting::ListParticipantsResult& result) = 0;

    // FINALIZE_CLOSE：**没有** response，也没有发起请求；只产生 notification。
    virtual void OnFinalizeClose(const meeting::FinalizeCloseResult& result) = 0;
};
```

### 6.5 调用位置与顺序

```text
消费者线程
  for (;;) {
      锁内：等待 / 取出 item
      锁外：DispatchOutsideLock(item)
              └── handler_.DispatchWork(item)
                    ├── meeting_service_.<API>(...)            ← 领域 mutation（唯一写者）
                    ├── sink_->Record*（若 sink != nullptr）    ← 纯观测，保持不动
                    ├── adapter_->On<API>(context, result)     ← M6 新增：产出 typed 出站对象
                    └──（Close）PostInternalWork(FINALIZE_CLOSE)  ← 必须仍在 adapter 之后
      锁内：in_flight_ 归零 + notify
  }
```

**顺序硬约束**：

```text
O-1  adapter 调用不得改变 dispatch 返回路径；其返回值不参与任何领域判断
O-2  adapter 调用不得改变 Close → FinalizeClose 的入队（M4 §10.3 的"不插队"结论不变）
O-3  adapter 的产出必须发生在领域结果已落地之后（避免重演 M4 §6.5 的观测顺序缺陷）
O-4  adapter 不得抛异常逃逸到 WorkerLoop（§13.4）
O-5  adapter 不向任何队列投递 work
```

---

## 7. Notification Path

### 7.1 唯一路径（M6.0）

```text
MeetingService::FinalizeClose(meeting_id)                          MeetingService.cpp:945
        │ 产出 result.notification_session_ids（intent，0..N 个稳定 SessionId 值）
        ▼
LogicWorkDispatcher::DispatchWork  case FINALIZE_CLOSE             LogicWorkDispatcher.cpp:157-165
        │ 领域 mutation 已完成；snapshot 已发布；ENDING→CLOSED 已收敛
        ▼
adapter_->OnFinalizeClose(result)
        │ 1. 构造 MeetingNotificationMessage（typed，§7.2）
        │ 2. 对 result.notification_session_ids 逐个做**lookup 判活**
        ▼
std::weak_ptr<IMeetingSessionDirectory>::lock()
        ├── 失败（目录已析构/未注入）⇒ 整批 no-op，计数 directory_unavailable
        └── 成功 ⇒ directory->FindSessionBySessionId(session_id)
                       ├── 空 ⇒ skip（计数 target_missing）
                       └── 句柄 ⇒ 计数 target_resolved
        ▼
collector_->Collect(message)（typed 值对象交给唯一出站接受者）
        ▼
（回到消费者循环；不等待、不重试、不回滚）
```

### 7.2 typed 出站值对象（M6.0 新增，**M6→Phase 3 的正式交付边界**）

M6.0 **不**定义字节布局，也**不**定义协议号。它定义**值对象**：

```cpp
// ---- 响应 ----------------------------------------------------------------
struct MeetingResponseMessage {
    ClientMeetingCommandKind command;   // 复用既有枚举（LogicWorkItem.h:76-83），不新增编码枚举
    meeting::RequestId request_id;      // correlation only，不是幂等键
    meeting::SessionId response_target_session_id;   // ← 来自可信 context.session_id（§6.3）

    meeting::ResultCode result_code;    // 复用冻结的 15 个值
    std::string result_name;            // 唯一来源 ResultCodeName()（MeetingTypes.h:137）
    meeting::ResultDisposition disposition;

    bool has_owner_chat_server_id;
    meeting::ServerId owner_chat_server_id;          // 仅 MEETING_NOT_LOCAL

    bool has_view;
    meeting::MeetingView view;          // 受 §10 安全规则约束

    bool has_participants;
    std::vector<meeting::ParticipantView> participants;   // 仅 ListParticipants
    bool has_participant_scope;
    meeting::ParticipantViewScope participant_scope;
};

// ---- 通知 ----------------------------------------------------------------
struct MeetingNotificationMessage {
    meeting::EventType notification_type;   // 当前仅 MEETING_CLOSED
    meeting::MeetingId meeting_id;
    meeting::SessionId notification_target_session_id;   // 单目标；多目标 = 多条消息

    bool has_event_id;
    meeting::EventId event_id;              // 来自 result.events 的 MEETING_CLOSED
    bool has_correlation_request_id;
    meeting::RequestId correlation_request_id;   // 来自 CloseContext.originating_request_id
};

// ---- 统一出站容器 ---------------------------------------------------------
enum class MeetingOutboundKind { RESPONSE = 0, NOTIFICATION = 1 };

struct MeetingOutboundMessage {
    MeetingOutboundKind kind;
    MeetingResponseMessage response;         // kind == RESPONSE 时有效
    MeetingNotificationMessage notification; // kind == NOTIFICATION 时有效
};
```

**为什么这不是"未定义格式的字符串 payload"**：

```text
1. 字段是**强类型的领域值**（ResultCode / MeetingView / ParticipantView / EventType / ids），
   不是 char buffer、不是 JSON、不是 protobuf；
2. 每个字段的取值来源在代码里可追溯（§6.3 / §7.1 的数据流）；
3. 不承诺任何序列化方式 ⇒ 不构成 wire format；
4. Phase 3 的 wire adapter 拿到该值对象之后，才决定字节布局、协议号与帧格式。
```

**为什么这不是"占位 message ID"**：

```text
该结构体中**不存在**任何 short/int 协议号字段；
CSession::Send 的 short msgid 参数在 M6 的路径上**完全不被使用**（§7.3）。
```

### 7.3 出站接受者接口（不含协议号）

```cpp
class IMeetingOutboundMessageCollector {
public:
    virtual ~IMeetingOutboundMessageCollector() {}
    // 接受一个 typed 出站消息。不得抛出；不得阻塞；不得回写领域状态。
    virtual void Collect(const MeetingOutboundMessage& message) = 0;
};
```

M6 提供的两个实现：

| 实现 | 使用方 | 行为 |
| --- | --- | --- |
| `CountingNoWireCollector` | **生产**（唯一实例） | 只计数（response/notification 各多少）、**绝不发送任何字节**、不 include `CSession.h` |
| `RecordingMessageCollector` | **测试** | 记录全部 typed 消息，供 Layer C 逐字段断言 |

> **关键**：生产 collector **不**调用 `CSession::Send`，因此 M6 生产链路
> **不存在任何占位/伪造的 msgid**。真实字节传输由 Phase 3 的 wire adapter 实现新的
> collector（或 transformer）完成，**不需要**修改 M6 的领域或 adapter 代码。

### 7.4 为什么通知必须同步发生在这一 turn

| 候选 | 分析 | 结论 |
| --- | --- | --- |
| 同一 turn 内同步产出 typed 消息 + 判活 | 不新增 FIFO/worker/timer；顺序可断言；符合 §15.4 | **采用** |
| 投递为新的 internal work kind | 同一 FIFO 但多一 turn；`ENDING→CLOSED` 已收敛，通知非"必要后续"，与 `PushInternalDuringDrainLocked` 的例外语义冲突 | 否 |
| 专用通知线程 / 另一 executor | 违反 §12.1、M4 §5.1 | 否 |
| timer 批量发送 | 违反 M6 禁止项 | 否 |

论证：`FinalizeClose` 的领域效果（`MeetingService.cpp:923` 发布 snapshot、`ENDING → CLOSED`、
`MeetingClosed` exactly once）在 `MeetingService` 内部**一次性完成**；
`notification_session_ids` 是返回值的一部分。⇒ 通知是**收敛之后**的纯副作用，
其失败在结构上不可能影响收敛。

### 7.5 ⚠️ M6 **不**具备真实生产通知发送能力（必须如实声明）

```text
M6 生产侧：OnFinalizeClose 会被真实调用、会真实执行 lookup 判活、会产出 typed 消息、
           会被 CountingNoWireCollector 计数——**但不会有任何字节到达任何客户端**。
真实发送：Phase 3（wire adapter + 真实 collector + 正式协议号）。
```

这是 STEP-1.6 §25.8（Wire Adapter Deferred）与 §25.7 条件说明
（"可只通过测试 adapter / direct invocation 验证"）的直接后果，
**不是 M6 实现缺陷**。但它必须被显式声明，不得被表述为"通知已端到端可用"。

---

## 8. Session Lookup（M6.0 重做：生命周期安全）

### 8.1 初版缺陷（本修订要解决的问题）

```text
初版：IMeetingOutboundChannel* FindSessionSendHandle(session_id);   ← 裸借用指针
缺陷：lookup 返回后、调用 Send 之前，另一线程可 ClearSession 并销毁该 CSession
      ⇒ 悬垂指针 ⇒ 未定义行为
      ⇒ 且"SharedSelf() 能补救"的说法是**错的**：SharedSelf() 要求对象已存活，
        无法补救调用前就发生的悬垂。
```

### 8.2 新设计：weak 判活 + shared 强句柄

```cpp
// MeetingOutboundChannel.h（新头；只含 <memory> / <string>）
class IMeetingOutboundSink {
public:
    virtual ~IMeetingOutboundSink() {}
    // 注意：M6 的**生产**路径**不调用**它。它存在的理由是：
    //   - Phase 3 的 wire adapter 需要一个"已判活的会话目标"类型，其生命周期必须由 lookup 保证；
    //   - M6 阶段仅用于 session lookup 的可观测性验证（resolved / missing）。
    virtual void AcceptMeetingOutbound(const MeetingOutboundMessage& message) = 0;
};

class IMeetingSessionDirectory {
public:
    virtual ~IMeetingSessionDirectory() {}

    // 非拥有、线程安全、**生命周期受控**：
    //   - 成功 ⇒ 返回 shared_ptr，调用期间目标**必然存活**（强引用由该句柄持有）
    //   - 失败 ⇒ 返回空 shared_ptr（目标不存在或已移除）
    // 实现方必须在自己的互斥下从"持有 shared_ptr 的注册表"中**复制**该 shared_ptr；
    // **禁止**用裸指针构造 shared_ptr（会建立第二个控制块 ⇒ 未定义行为）。
    virtual std::shared_ptr<const IMeetingOutboundSink>
        FindSessionBySessionId(const std::string& session_id) = 0;
};
```

`CServer` 的实现要点：

```cpp
std::shared_ptr<const IMeetingOutboundSink>
CServer::FindSessionBySessionId(const std::string& session_id) {
        std::lock_guard<std::mutex> lock(_mutex);      // ← 必须在 _mutex 下 find（§8.5）
        std::map<std::string, std::shared_ptr<CSession>>::iterator it = _sessions.find(session_id);
        if (it == _sessions.end()) {
                return std::shared_ptr<const IMeetingOutboundSink>();
        }
        // it->second 是 shared_ptr<CSession>（F4）；CSession 实现 IMeetingOutboundSink。
        // **复制** shared_ptr ⇒ 引用计数 +1 ⇒ 语句结束后目标仍存活。
        return it->second;
}
```

### 8.3 所有权证明（逐条）

| 待证明 | 证明 |
| --- | --- |
| lookup 在 `_mutex` 下完成 | 实现第一行即 `lock_guard(_mutex)`；`find` 与 `shared_ptr` 复制都在临界区内 |
| 不暴露可变注册表 | 返回类型 `shared_ptr<const IMeetingOutboundSink>`；该接口无任何返回容器/引用的方法；调用方无法增删会话 |
| adapter 不依赖 `CServer`/`CSession` 具体类型 | 接口头只含 `<memory>`/`<string>`；实现细节封在 `CServer.cpp` |
| 不违反 Meeting Domain 所有权边界 | 目录接口只出现在 adapter 与 `CServer` 之间；`meeting/*` 零 include、零引用 |
| 不声称 `SharedSelf()` 补救悬垂 | 本设计**不依赖**它；生命周期由 `shared_ptr` 复制本身保证。`SharedSelf()` 只在 `async_write` 期间保活（既有语义），与本保障无关 |
| 强引用不会导致"复活"/泄漏 | 句柄只在一次 `OnFinalizeClose` 处理期间存活（局部变量），**不**把 Session 重新插入 `_sessions`，也不阻止 `Close()`；被移除后仍存活只是延迟析构，不是语义复活 |
| 不存在"从裸指针造 shared_ptr" | 显式禁止；唯一来源是 `_sessions` 中既有的 `shared_ptr`（§8.6） |

### 8.4 目录本身的生命周期：`weak_ptr` 判活

初版用裸 `IMeetingSessionDirectory*`。若 `CServer`（局部对象）先于唯一 worker 的
`StopAndJoin` 析构，则该指针悬垂（F6-a 证实这是真实风险）。

M6.0 改为：

```cpp
// adapter 成员
std::weak_ptr<IMeetingSessionDirectory> session_directory_;   // 判活引用，不拥有

// 使用
std::shared_ptr<IMeetingSessionDirectory> directory = session_directory_.lock();
if (!directory) {
        // 目录已析构（或从未注入）⇒ 整批 no-op，计数，绝不崩溃
        return;
}
```

配套：`CServer` 改为 `std::make_shared<CServer>(...)`（F6-c：它已继承
`enable_shared_from_this` 但从未使用；唯一构造点是 `ChatServer.cpp:50` 的栈对象）。

```text
性质：
1. CServer 存活 ⇒ lock() 成功 ⇒ 真实 lookup 可用
2. CServer 已析构 ⇒ lock() 失败 ⇒ adapter 明确 no-op（**不是**崩溃、不是悬垂）
3. adapter 不延长 CServer 生命周期（weak 不 +1 引用计数）
4. 因此即使关闭顺序出错，最坏结果是"通知不发出"，而不是未定义行为
   —— 这是对 F6-a 的**兜底**；关闭顺序仍按 §12 显式保证
```

### 8.5 `_mutex` 纪律与既有缺陷边界

```text
M6 新增的 FindSessionBySessionId 必须在 _mutex 下 find。
M6 **不**修 CServer::ClearSession 的既有"find 未持锁"缺陷（CServer.cpp:60）。
  理由：STEP-1.6 §13.3 明令该类既有并发隐患只记录不顺手重写；同一纪律对 M6 成立。
  后果如实记为未消除风险（§18 R-2）。
```

### 8.6 关于 `.get()` 与第二个控制块（必须写进 Review checklist）

```text
禁止：std::shared_ptr<X>(ptr.get())        ← 两个控制块 ⇒ double free
禁止：std::shared_ptr<X>(raw_ptr)          ← 同上
允许：从 _sessions 中的既有 shared_ptr **复制**
允许：std::shared_ptr<const IFace> h = it->second;（隐式向上转换）
```

---

## 9. Best-effort Semantics

### 9.1 冻结规则落地（STEP-1.6 §15.4 / §16.1）

| 情形 | 处理 | 依据 |
| --- | --- | --- |
| Session 不存在 | 跳过（best-effort），计 `target_missing` | §15.4 |
| 目录不可用（`weak_ptr` lock 失败） | 整批 no-op，计 `directory_unavailable` | §8.4（M6.0 新增） |
| 目标在关闭过程中消失 | 不影响 `ENDING → CLOSED` 收敛 | §15.4 / §16.1 |
| 出站消息产出失败（不可达分支） | 计 `encoding_failed`，不抛 | §13 |
| ACK / retry / 死信 | **不存在** | §15.4 |
| 客户端是否收到 | **M6 不统计、不验证**（§13.3） | M6.0 新增边界 |

### 9.2 三条强制约束

```text
(1) 通知/响应的**任何**失败都只能产生"少发出一条消息"这一种后果，
    不得产生领域状态变化、不得产生事件、不得改变 result_code / disposition。
(2) adapter 的所有出站调用必须 noexcept 化：异常在 adapter 边界被吞掉并计数，
    绝不允许逃逸进 WorkerLoop（消费者线程死亡 = Meeting 子系统停摆）。
(3) adapter 不得阻塞：不 sleep、不等待、不做同步 I/O、不做重试。
```

### 9.3 可断言的验收形式

```text
X11（STEP-1.6 §19.5）：notification failure does not roll back CLOSED
  ⇒ 用"目录 lock 失败 / target 全部 missing"的替身，断言：
     - FinalizeClose 仍然 finalized == true
     - HasClosedSnapshot(meeting_id) == true
     - MeetingClosed 恰好一次
     - target_missing == 期望值
     - 无第二次事件、无第二次 FinalizeClose
```

---

## 10. MeetingView Security Boundary

### 10.1 冻结裁决（M3-closeout §8.2，**不得**因编码而被绕过）

> `MeetingView` 只有在**授权通过**（或幂等命中）时才填充；
> `PERMISSION_DENIED` / `NOT_PARTICIPANT` 时保持默认空值。

代码复核确认该裁决在领域层已实现（`MeetingService.cpp:755-776` 等）：

```cpp
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

### 10.2 反向约束

M6 编码路径不得使用"`result_code != OK` ⇒ 视图为空"之外的任何启发式，
也不得为了"响应完整"而从别处补齐字段（例如用 `MeetingCommandOutcome.active_participant_count`
或 `MeetingRequestContext` 推断 `host_user_id`）。

### 10.3 编码矩阵（M6 Review 必须逐格核对）

| `result_code` | `has_view` | 允许出现的字段 | 禁止出现 |
| --- | --- | --- | --- |
| `OK`（`SUCCESS`） | **true** | `meeting_id`、`owner_chat_server_id`、`host_user_id`、`meeting_state`、`active_participant_count`、`created_at`、`has_closed_at`/`closed_at` | 任何 `session_id`、Binding、socket |
| `ALREADY_JOINED` / `ALREADY_LEFT` / `CLOSE_IN_PROGRESS` / `ALREADY_CLOSED`（`IDEMPOTENT`） | 按 M3 是否填充**照实透传**（不新增填充逻辑） | 与 `OK` 同（幂等命中意味着授权通过） | 同上 |
| `PERMISSION_DENIED` | **false** | 仅 `result_code` / `result_name` / `disposition` / `request_id` / `has_meeting_id`+`meeting_id`（该 id 由请求携带，非泄露） | `active_participant_count`、`host_user_id`、`owner_chat_server_id`、`meeting_state`、`created_at` |
| `NOT_PARTICIPANT` | **false** | 同上 | 同上 |
| `AUTH_REQUIRED` / `SESSION_STATE_REJECTED` | false | 同上 | 同上 |
| `INVALID_ARGUMENT` | false | 同上 | 同上 |
| `MEETING_NOT_FOUND` | false | 同上 | 同上 |
| `MEETING_NOT_LOCAL` | false | 同上 + `has_owner_chat_server_id`/`owner_chat_server_id`（**唯一**合法携带异地 owner 的结果） | 同上 |
| `MEETING_STATE_REJECTED` / `HOST_MUST_CLOSE_MEETING` | false | 同上 | 同上 |
| `INTERNAL_ERROR` | false | 同上 | 同上 |

### 10.4 `ListParticipants` 的额外规则

`participants` 与 `scope` 只在授权通过的 `ListParticipantsResult` 上存在；
`scope`（`CURRENT` / `HISTORICAL`）必须照实透传，不得把 CLOSED 的历史身份与
活动成员混为一谈（`MeetingTypes.h:229-240`、`MeetingService.h:190-196`）。

### 10.5 必须新增的静态核对项（Review gate）

```text
grep -n "active_participant_count\|host_user_id\|owner_chat_server_id" MeetingResponseNotificationAdapter.cpp
  ⇒ 每一处命中都必须位于 has_view == true 才执行的分支内
  ⇒ 禁止存在任何无条件读取 result.meeting.* 后写入 response 的代码路径
```

---

## 11. Ordering / FIFO Semantics

### 11.1 冻结约束（不得违反）

```text
STEP-1.6 §12.1  ：三类 work 共享同一个逻辑串行化顺序；禁止 A/B/C 三条队列
STEP-1.6 §12.5  ：一个 FIFO、一个消费者决定 Meeting mutation 顺序
M4 §5.1         ：全仓库仅 SerializedWorkExecutor 创建 work 消费线程
M4 §11.2        ：stop 后外部 work 被丢弃；内部 work 是唯一例外（消费者线程专用）
M5.1 §6         ：REGISTER < 第一条业务消息；AUTHENTICATE 落在 FIFO 尾部；
                  DISCONNECT < 迟到命令；不做优先级/插队
Step 1.3 §20    ：MeetingService 为 single-writer / non-thread-safe；
                  M6 **不**给它加锁、不引入线程、不引入 async task（§23 第 10 条）
```

**M6.0 保持不变的核心不变量（逐条，Review 必须核对）**：

```text
INV-1  唯一 LogicWorkQueue；唯一 SerializedWorkExecutor 消费者；single-writer
INV-2  MeetingService 无锁、无线程、无 timer、无后台清理；meeting/* 零改动
INV-3  通知/响应失败不回滚领域状态（§9.1）
INV-4  ENDING → CLOSED 必然收敛（§13.5）
INV-5  MeetingView 授权失败时不泄露 host_user_id / active_participant_count / 成员信息（§10）
INV-6  SESSION_STATE_REJECTED 不得被改写为 AUTH_REQUIRED（§13.2 第 1 条）
INV-7  internal work payload 只含稳定 ID；无伪 msg_id、无假 LogicNode、不持 CSession
```

### 11.2 M6 的顺序证明

**(O1) adapter 不改变 FIFO 成员与顺序。**
adapter 唯一调用点在 `Dispatch*` 内，且**不向队列投递任何 work**。
唯一由 dispatcher 发起的入队仍是 `DispatchClose` 的 `PostInternalWork(FINALIZE_CLOSE)`
（`LogicWorkDispatcher.cpp:275-277`），其位置、条件、目标队列全部不变。

```text
证明义务：adapter 调用点前后不得出现 Enqueue / Push / PostInternalWork
          （除既有 PostInternalWork(FINALIZE_CLOSE) 之外）
```

**(O2) adapter 不引入第二消费者，也不引入新的等待。**
adapter 在消费者线程上同步执行，不创建线程、不 sleep、不阻塞 I/O、不 post 到别的 executor。

**(O3) 通知发生在领域收敛之后，不参与裁决顺序。**
`FinalizeClose` 的 snapshot 发布与 `MeetingClosed` 都在 `MeetingService` 内部完成；
`OnFinalizeClose` 拿到的是最终结果值。

### 11.3 response ordering == enqueue ordering（可断言命题）

```text
依据：（1）消费者唯一 ⇒ 命令被串行处理；
      （2）response 在同一 turn 内产出 ⇒ 不存在"结果先于前一条命令的 response"；
      （3）不引入任何并发出口。
可断言（Layer C）：连续入队 CREATE/QUERY/CLOSE，断言 RecordingMessageCollector 中
                    RESPONSE 消息顺序与入队顺序逐项一致，且各自携带对应 request_id。
```

### 11.4 明确不引入的东西

```text
第二队列 / 第二 worker / 线程池 / timer / 后台清理
adapter 内的 async 编排（post/dispatch 到另一 executor）
notification 的批量合并 / 去重窗口
response 的延迟发送 / 合并
```

---

## 12. Ownership / Lifetime（M6.0 重做：关闭顺序）

### 12.1 生产 ownership 图

```text
ChatServer.cpp（composition root）
  └── auto server = std::make_shared<CServer>(io_context, port)      ← §12.4 改为 shared
        ├── _sessions : std::map<std::string, std::shared_ptr<CSession>>
        ├── 实现 IMeetingSessionDirectory（FindSessionBySessionId，§8.2）
        └── 构造后、io_context.run() 前：
              LogicSystem::GetInstance()->SetMeetingSessionDirectory(server)

LogicSystem（Singleton）
  ├── _dispatcher : unique_ptr<LogicWorkDispatcher>            ← 惰性构造（EnsureWorkQueueStarted）
  ├── _meeting_adapter : unique_ptr<MeetingResponseNotificationAdapter>   ← M6 新增
  │     ├── IMeetingOutboundMessageCollector&  → CountingNoWireCollector（生产）
  │     └── weak_ptr<IMeetingSessionDirectory> → 指向 CServer（判活，不拥有）
  └── _work_executor : unique_ptr<SerializedWorkExecutor<LogicWorkDispatcher>>
```

### 12.2 为什么 adapter 与 dispatcher 必须在同一个 `call_once` 内构造

`LogicWorkDispatcher` 的构造需要 `IMeetingResponseNotificationAdapter*`；
adapter 需要 collector 与 weak 目录。三者都在 `EnsureWorkQueueStarted()` 的
`std::call_once` 内一次性建立，保证：

```text
1. adapter 生命周期 ⊆ dispatcher 生命周期 ⊆ LogicSystem 生命周期
2. dispatcher 析构时 worker 已 StopAndJoin ⇒ adapter 不可能在消费者仍运行时被销毁
3. member 声明顺序必须为：_meeting_adapter → _work_executor → _dispatcher
   （C++ 逆序析构 ⇒ worker 先停，再销毁 adapter，最后销毁 dispatcher）
4. 不需要为 adapter 引入任何锁或引用计数
```

### 12.3 关闭顺序（M6.0 核心新增：显式 ShutdownWorkExecutor）

**问题（F6-a）**：`CServer` 是 `main` 的局部对象，`LogicSystem` 是 Singleton；
后者的析构（也是目前唯一的 `StopAndJoin` 时机）**晚于** `CServer` 析构。
因此在现有代码下，"worker 生命周期 ⊆ io_context.run() 期间"**不成立**，
初版据此推出的"目录借用安全"是错的。

**M6.0 裁决：在 `main` 中显式停止并 join 唯一 executor，顺序不可交换。**

```cpp
// ChatServer.cpp —— M6 目标形态（示意；Coding 时以实际为准）
auto server = std::make_shared<CServer>(io_context, atoi(port_str.c_str()));
LogicSystem::GetInstance()->SetMeetingSessionDirectory(server);   // §12.5 时序

io_context.run();

// ---- 关闭序列（顺序不可交换）------------------------------------------
pool->Stop();          // 1. 幂等；确保 AsioIOServicePool 线程全部 join
                       //    ⇒ 之后不可能再有网络回调执行（F6-b）
LogicSystem::GetInstance()->ShutdownWorkExecutor();   // 2. RequestStop + join；
                       //    drain 已受理 work（含 Close→FinalizeClose）
                       //    此时 server 仍存活 ⇒ 目录 lookup 仍可用
LogicSystem::GetInstance()->ClearMeetingSessionDirectory();   // 3. 断开目录引用
                       //    （weak 语义下即使漏掉也只会 no-op；显式化以利可观测）
RedisMgr::GetInstance()->HDel(LOGIN_COUNT, server_name);       // 4. Redis 关闭之前
RedisMgr::GetInstance()->Close();
grpc_server_thread.join();
// 5. server 与 io_context 在此之后才析构
```

**为什么必须是这个顺序（逐条）**：

| # | 步骤 | 为什么必须在这 |
| --- | --- | --- |
| 1 | `pool->Stop()` | `~CSession` / 读回调 / `ClearSession` 都在 pool 线程上跑。`Stop()` 内部对每个 io_context `stop()` 并对每个 `_threads` `join()`（`AsioIOServicePool.cpp:41-49`）⇒ 返回后**网络生产者已全部停止**，drain 期间不会再有新入队 |
| 2 | `ShutdownWorkExecutor()` | 必须在 `server` 析构之前：drain 期间 `FINALIZE_CLOSE` 仍可能触发 `OnFinalizeClose`，其 lookup 需要存活的目录。也必须在 Redis 关闭之前（Step 2.4 的 runtime smoke 要求 Redis 注册清理在正常退出前完成） |
| 3 | `ClearMeetingSessionDirectory()` | 解除 adapter 对目录的引用。weak 语义下这是"显式化 + 可观测"，而非正确性所必需 |
| 4 | Redis 关闭 | 既有顺序，不变 |
| 5 | `server` / `io_context` 析构 | 此时已无任何线程会使用它们 |

**重复 `StopAndJoin()` 的行为（必须核对，已在代码中确认）**：

```cpp
// LogicWorkDispatcher.h:409-416
void StopAndJoin() {
    ReleaseGate();
    queue_.RequestStop();                                        // 幂等：只置标志 + notify_all
    if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id()) {
        worker_.join();
    }
}
```

```text
⇒ 可重复调用：RequestStop 幂等；join 前检查 joinable()，join 后变为非 joinable
⇒ 不会自我 join（get_id() != this_thread::get_id() 守卫）
⇒ 因此 main 里显式调用一次、~LogicSystem 再调用一次，是安全的（析构里第二次为 no-op）
```

**Join 期间不会再有新 work 的证据**：

```text
(a) pool->Stop() 已 join 全部网络线程 ⇒ HandleAccept / 读回调 / ClearSession 不再执行
(b) 即使某个异步路径仍在极窄窗口内投递，LogicWorkQueue::Push 在 stopping_ 为真后
    一律返回 false（M4 §11.2）⇒ 拒绝，不进入队列
(c) 消费者线程是唯一能走的 drain 例外（PushInternalDuringDrainLocked），
    用于补完已受理工作的必要后续（Close→FinalizeClose）
```

### 12.4 `CServer` 改为 `shared_ptr` 的必要性与风险

```text
必要性：weak_ptr<IMeetingSessionDirectory> 需要一个既有控制块。
        CServer 已继承 enable_shared_from_this<CServer>（CServer.h:10），
        但当前是栈对象（ChatServer.cpp:50）且全仓无 shared_from_this() 调用（F6-c）。
        ⇒ 改为 make_shared 是**唯一**能安全得到控制块的方式。
        这不是扩大范围，而是把既有但未生效的继承关系与 weak 目录方案对齐。

不变性：CServer 的公开接口、_sessions 语义、ClearSession 行为均不改变；
        CSession 持有 CServer* 的既有裸指针成员不受影响（_server 仍指向同一对象）。
风险：若某处依赖"CServer 是栈对象"会受影响。已核对：唯一构造点即 ChatServer.cpp:50
      （grep `CServer s(` / `make_shared<CServer>` / `new CServer` 仅 1 处）。
```

### 12.5 依赖注册的时序与可见性证明

```text
main 线程                                              worker 线程
─────────                                              ───────────
ConfigMgr / Redis init
make_shared<CServer>(io_context, port)     ← 构造，尚未 accept
SetMeetingSessionDirectory(server)         ← 写入 LogicSystem（线程尚不存在）
io_context.run()                           ← 之后才可能 HandleAccept
                                                       EnsureWorkQueueStarted()
                                                         call_once: 读目录（已写）
                                                         new LogicWorkDispatcher(..., adapter)
                                                         new SerializedWorkExecutor(...)
                                                           └── std::thread 启动 ← happens-before
```

**顺序 + 可见性 + 幂等 三重保证**：

```text
(a) 顺序：注册早于 io_context.run()；而首次入队只能由 accept 后的 HandleAccept 产生
    （M5.1 §4.2：投递在 Start() 之前但仍在 accept 成功之后）。
(b) 可见性：目录写入发生在 std::thread 构造之前；std::thread 构造与线程启动之间
    是 C++ 标准规定的 happens-before 边 ⇒ worker 无需额外同步即可安全读。
(c) 幂等：SetMeetingSessionDirectory 用一次性语义（std::call_once 或等价断言），
    重复注册不改变状态；ClearMeetingSessionDirectory 之后 adapter 只会 no-op。
```

> ⚠️ 这是 M6 最容易写错的一处。若把注册放到 `io_context.run()` 之后，
> 或在 `CServer` 构造内部依赖尚未建立的 `LogicSystem`，会得到
> "adapter 永远看到空目录"的静默失效。Layer C 必须能复现该时序（§16.2 组 3 ND7）。

### 12.6 非拥有/受控引用一览（M6.0 后）

| 引用 | 拥有者 | 引用者 | 生命周期保障 |
| --- | --- | --- | --- |
| `IMeetingResponseNotificationAdapter*` | `LogicSystem::_meeting_adapter` | dispatcher | adapter ⊆ dispatcher ⊆ LogicSystem；worker 先 join |
| `weak_ptr<IMeetingSessionDirectory>` | 不拥有（`CServer` 由 `main` 的 `shared_ptr` 拥有） | adapter | `lock()` 判活；失败 ⇒ no-op，**不会悬垂** |
| `shared_ptr<const IMeetingOutboundSink>` | `CServer::_sessions` | adapter（**仅在一次 OnFinalizeClose 处理期间**） | 从注册表复制 ⇒ 处理期间目标必然存活；局部变量结束后释放 |
| `IMeetingOutboundMessageCollector*` | `LogicSystem::_meeting_adapter` | adapter | 同 adapter 生命周期 |

---

## 13. Error Handling

### 13.1 三层错误必须严格分开

```text
层 1  领域判定（MeetingService）  → ResultCode / ResultDisposition（冻结的 15 个值）
层 2  适配编码（adapter）         → 只做映射；**不得**新增 ResultCode、不得改写 result_code
层 3  出站交付（collector）       → M6 只到 typed 值对象；真实传输属 Phase 3
```

### 13.2 硬性规则

```text
(1) adapter 绝不把一个 ResultCode 改成另一个。
    尤其：不得把 SESSION_STATE_REJECTED 改成 AUTH_REQUIRED（STEP-1.4 §19.4 明令禁止）。
(2) session lookup 失败（response 目标已消失）**不得**改变 result_code / disposition。
    response 的职责是"报告领域结果"，与"能否送达"无关。
(3) result_name 必须来自 ResultCodeName()（MeetingTypes.h:137），
    不得维护第二套字符串映射（M4 §8 同一纪律）。
(4) 未识别的 ResultCode（理论不可达）→ 编码为 INTERNAL_ERROR 并计 encoding_failed，
    不得静默产生空响应，也不得抛出。
(5) adapter 内部任何异常都在边界处吞掉并计数：绝不允许逃逸进 WorkerLoop。
```

### 13.3 发送失败的可观测性边界（M6.0 新增，**必须逐条遵守**）

| 档 | 情形 | M6 是否可统计 | 依据 / 说明 |
| --- | --- | --- | --- |
| **L0** | 目标不存在 / lookup 跳过 | ✅ **可统计** | `FindSessionBySessionId` 返回空 ⇒ 计 `target_missing` |
| **L1** | 目录不可用（`weak` lock 失败） | ✅ **可统计** | 计 `directory_unavailable` |
| **L2** | 已产出并交给出站接口 | ✅ **可统计** | `Collect()` 被调用即计 `outbound_produced`（生产为 `CountingNoWireCollector`） |
| **L3** | adapter 捕获的同步异常 | ✅ **可统计** | adapter 边界 try/catch，计 `adapter_exception` |
| **L4** | **异步写入失败**（`HandleWrite` 的 error） | ❌ **M6 不可统计、不可验证** | `CSession::Send` 返回 `void`；写失败只在 `HandleWrite` 内部调用 `Close()` 并打印日志（`CSession.cpp:245-249`），**无回传通道** |
| **L5** | **客户端实际收到** | ❌ **M6 不可统计、不可验证** | 协议无 ACK（STEP-1.6 §15.4：当前没有 ACK / retry） |

**因此**：

```text
M6 只能声称"已决定发给谁 / 已跳过谁 / 已产出什么 typed 消息"；
M6 **不得**声称"发送成功""写入成功""客户端已收到"。
为提高可观测性而改变 legacy 发送语义（例如让 Send 返回 bool 或加回调）——**M6 不做**。
L4/L5 的可观测化属于 Phase 3（需协议层 ACK 与 wire adapter 协同）。
```

### 13.4 noexcept 边界

```text
adapter 对 collector / directory 的所有调用都必须在 try/catch 内，且 noexcept 化。
理由：任何异常逃逸进 WorkerLoop 会终止唯一消费者线程 ⇒ 整个 Meeting 子系统停摆，
      且现象是"静默不再处理任何 work"（R-7）。
```

### 13.5 与 close/finalize 的相互作用

```text
问题：response/notification 的失败会不会让 ENDING → CLOSED 不收敛？
答案：结构上不可能。原因链：
      (a) CloseMeeting 的 response 在 turn 1 产出，而 ENDING→CLOSED 由 turn 2 的
          FinalizeClose 完成（STEP-1.6 §16，两个独立 work item）；
      (b) FINALIZE_CLOSE 的入队由 should_schedule_finalize（领域返回值）决定，
          与出站是否成功**无关**；
      (c) FinalizeClose 的领域效果在 MeetingService 内部一次性完成，
          notification 只是其返回值的下游产物（§7.4）。
```

---

## 14. Context Builder

### 14.1 接口（纯函数式，无副作用）

```cpp
// MeetingRequestContextBuilder.h（新头；只含标准库 + meeting/MeetingTypes.h）
class MeetingRequestContextBuilder {
public:
    // 全部输入都必须是**服务端可信值**：
    //   session_id            ← CSession::GetSessionId()（稳定值；**不是** CSession 本身）
    //   authenticated_user_id ← 已认证成功路径上的局部变量 uid
    //                           （M5.1 §5.3：LoginHandler 中已完成 token 校验的 uid）
    //   request_id            ← 由调用方提供；空 ⇒ 领域返回 INVALID_ARGUMENT（不做补偿）
    //   local_server_id       ← LogicSystem::LocalServerId()
    //   has_device_id         ← M6 阶段恒为 false（无 Device 概念，不得伪造）
    MeetingRequestContext Build(const meeting::SessionId& session_id,
                                meeting::UserId authenticated_user_id,
                                const meeting::RequestId& request_id,
                                const meeting::ServerId& local_server_id) const;
};
```

### 14.2 硬性约束

```text
CB-R1  **绝不**读取 CSession::_user_uid（F7：构造未初始化）。
       身份只来自调用方传入的、已认证成功路径上的 uid。
CB-R2  不读取 CSession 的任何其他成员；builder 不 include CSession.h。
CB-R3  has_device_id 恒为 false（MeetingTypes.h:197 明令不得伪造）。
CB-R4  has_trusted_owner_chat_server_id 恒为 false（只有服务端可信来源可填；M6 无该来源）。
CB-R5  request_id 为空时**不生成**凑数值；让它走到领域的 INVALID_ARGUMENT，
       以保持"参数校验先于资源 lookup"可观测（STEP-1.6 §11.3）。
CB-R6  handling_chat_server_id 必须等于 local_server_id，否则领域返回 INVALID_ARGUMENT
       （MeetingService.cpp:132-135）——builder 不得"修正"它。
```

### 14.3 生产调用者：Phase 3 之前不存在（必须如实声明）

```text
唯一合法的生产调用者是 wire adapter（把一条已认证 TCP 帧转成 MeetingRequestContext），
而它属于 STEP-1.6 §25.8 的 Deferred：Phase 3。
⇒ M6 **不**存在"真实客户端 → Meeting 命令"的入口。
⇒ M6 的 builder 只通过 direct invocation（Layer C）验证；
   这**不是**生产端到端验证，不得如此表述。
```

该边界被 STEP-1.6 §25.7 的条件说明明确允许。

---

## 15. Scope Matrix

### 15.1 MUST MODIFY

| 文件 | 修改内容 | 依据 |
| --- | --- | --- |
| `MeetingOutboundMessage.h`（**新增**） | `MeetingResponseMessage` / `MeetingNotificationMessage` / `MeetingOutboundMessage`（typed 值对象） | §7.2 |
| `MeetingOutboundChannel.h`（**新增**） | `IMeetingOutboundSink`、`IMeetingSessionDirectory`、`IMeetingOutboundMessageCollector`（仅 `<memory>`/`<string>`） | §7.3 / §8.2 |
| `MeetingRequestContextBuilder.h`（**新增**） | builder 声明 + inline 实现（纯函数） | §14 |
| `MeetingResponseNotificationAdapter.h`（**新增**） | `IMeetingResponseNotificationAdapter` + 生产 adapter 声明 + 只读计数 | §6.4 / §8.4 |
| `MeetingResponseNotificationAdapter.cpp`（**新增**） | 编码、安全矩阵、weak 判活、best-effort、noexcept 边界、计数 | §6 / §7 / §9 / §10 / §13 |
| `CountingNoWireCollector.h/.cpp`（**新增**，或并入 adapter TU） | 生产 collector：只计数、绝不发送 | §7.3 / §7.5 |
| `LogicWorkDispatcher.h` | 构造参数增加 adapter 指针；`Dispatch*` 内增加 adapter 调用点 | §6.5 |
| `LogicWorkDispatcher.cpp` | 六个 `Dispatch*` + `FINALIZE_CLOSE` 分支的 adapter 调用；保持 sink 顺序与 FINALIZE_CLOSE 入队顺序不变 | §6.5 / §11.2 |
| `LogicSystem.h` / `.cpp` | adapter 成员与一次性装配；`SetMeetingSessionDirectory` / `ShutdownWorkExecutor` / `ClearMeetingSessionDirectory` | §12.1 / §12.3 / §12.5 |
| `CServer.h` / `.cpp` | 实现 `IMeetingSessionDirectory`；新增 `FindSessionBySessionId`（`_mutex` 下 find，复制 shared_ptr） | §8.2 / §8.5 |
| `CSession.h` / `.cpp` | 声明实现 `IMeetingOutboundSink`（**仅接口，不改变发送语义**） | §8.2 |
| `ChatServer.cpp` | `CServer` 改 `make_shared`；注册目录；关闭序列（§12.3） | §12.3 / §12.4 |
| `CMakeLists.txt` | 新增 TU 到 `CHATSERVER_APPLICATION_SOURCES` | §16.5 |
| `tests/CMakeLists.txt` | 新增 adapter TU + M6 测试文件 | §16.5 |
| `tests/MeetingResponseNotificationAdapterTest.cpp`（**新增**） | Layer C 用例（组 1–7） | §16.2 |
| 本文件 | 状态更新为 COMPLETE（Coding 后） | 惯例 |

### 15.2 MAY MODIFY（仅当代码事实证明必要）

| 文件 | 何时才可修改 | 必须记录的理由 |
| --- | --- | --- |
| `CSession.cpp`（发送队列语义） | **仅当**发现"无法从外部判断排队是否发生"会破坏某条验收 | 不得改变既有 `Send` 对 legacy 路径的**任何**行为（§13.3） |
| `LogicWorkItem.h` | **仅当**需要为 adapter 观测补一个纯只读字段 | 须先证明全仓无该 struct 的聚合初始化（M5.1 §8 的方法） |
| `AsioIOServicePool.*` | **仅当**发现 `Stop()` 不幂等 | 改动须单独说明；该文件为 GBK，须保持原编码 |

### 15.3 MUST NOT MODIFY

```text
Server/ChatServer/ChatServer/message.proto                    （§25.7 Files forbidden；Phase 3）
Server/ChatServer/ChatServer/*.sln                            （§25.7 Files forbidden）
Server/ChatServer/ChatServer/*.vcxproj                        （§25.7 Files forbidden；Conflict #3 裁决 iii）
Server/ChatServer/ChatServer/*.filters                        （同上）
Server/ChatServer/ChatServer/PropertySheet.props              （同上）
MyChat_Qt/**                                                  （§25.7 Files forbidden）
Server/ChatServer2/**                                         （R8 未解决；第一轮只在 ChatServer 落点）
Server/GateServer/** Server/StatusServer/** Server/VerifyServer/**

Server/ChatServer/ChatServer/meeting/**                       （领域契约已冻结）
  ——例外：仅当出现**明确、不可避免**的极小修改需求。此时必须停在人工裁决点，
     不得自行修改，也不得自行发明 workaround。

Server/ChatServer/ChatServer/tests/ 下既有 7 个测试文件       （M6 不得修改既有测试）
  LogicWorkQueueTest.cpp / MeetingServiceTest.cpp / MeetingAggregateTest.cpp /
  MeetingTypesContractTest.cpp / SessionDisconnectBridgeTest.cpp /
  DomainSessionLifecycleTest.cpp / TestInfrastructureSmoke.cpp

docs/phase1/STEP-1.1 ~ STEP-1.6                              （冻结；发现冲突只报告不修改）
```

### 15.4 DEFERRED

| 项 | 状态 | 依据 |
| --- | --- | --- |
| **真实字节交付（wire adapter / TCP message ID / JSON / protobuf / Frame V2 / request_id encoding / Qt / HTTP / WebSocket）** | **DEFERRED → Phase 3** | §25.8；§7.5 |
| **L4（异步写失败）与 L5（客户端送达）的可观测化** | **DEFERRED → Phase 3** | §13.3 |
| Layer D（真实 TCP 端到端） | **DEFERRED** | §17.4 |
| Windows `Debug|x64` 验证与 R6 | **DEFERRED**（Conflict #3 裁决 iii） | §19.3 / §22 |
| Domain session tombstone retention（TTL / LRU / timer / 后台清理） | **DEFERRED** | M5.1 §7 |
| ClosedSnapshot retention / 淘汰策略 | **DEFERRED** | M3 遗留 |
| 既有 flaky #1（`LogicWorkQueueTest.cpp:1135`）与 #2（`:681`） | **DEFERRED（M6 不修）** | M5.1 §9.2 + 本轮复核 |
| `CServer::ClearSession` 的 `find` 未持锁 | **DEFERRED**（M6 不顺手修） | STEP-1.6 §13.3 / R10 |
| `CSession::_user_uid` 构造未初始化 | **DEFERRED**（M6 不读它） | F7 / M5 §13.2 |
| `MysqlDao.cpp:95` 告警、`re2_DIR` 残留（R7） | **DEFERRED** | 既有 |
| ChatServer / ChatServer2 双份维护（R8） | **DEFERRED** | STEP-1.6 §22 |
| 多节点 Meeting 路由与 `MEETING_NOT_LOCAL` 真实重定向 | **DEFERRED** | Phase 1 明确不支持跨 ChatServer |
| ACK / retry / 死信 / 可靠投递 / 通知合并去重 | **DEFERRED** | STEP-1.6 §15.4 |

---

## 16. Test Strategy

### 16.1 分层归属（STEP-1.6 §17.5：Layer C 首次可运行于 M5 / M6）

| 层 | M6 任务 | 是否可运行于 M6 |
| --- | --- | --- |
| Layer A（纯领域） | 与通知无关的断言**必须保持通过**（§25.7 Unit tests 要求） | ✅（零改动即通过，作为回归护栏） |
| Layer B（串行化顺序） | 既有 25 用例不改；M6 不复制其反模式 | ✅ |
| Layer C（ChatServer 集成） | M6 主战场 | ✅ |
| Layer D（真实 TCP 端到端） | **Deferred** | ❌ |

### 16.2 用例矩阵（设计；本步不创建）

**组 1：`MeetingRequestContext` builder（Layer C）**

| # | 用例 | 断言要点 |
| --- | --- | --- |
| CB1 | 合法输入构造成功 | 三字段非空；`authenticated_user_id` 与传入值一致 |
| CB2 | identity 只来自服务端 | builder 不接受客户端覆盖路径；构造结果仅由入参决定 |
| CB3 | `handling_chat_server_id` == `local_server_id` | 不一致时领域 `INVALID_ARGUMENT`（`MeetingService.cpp:132-135`） |
| CB4 | 不伪造 device | `has_device_id == false` |
| CB5 | 空 `request_id` / 空 `session_id` | 领域 `INVALID_ARGUMENT`，证明 builder 未补偿 |
| CB6 | **不读 `_user_uid`** | 静态检查：builder TU 内无 `_user_uid`、无 `CSession` include（Review gate） |

**组 2：session lookup（Layer C）**

| # | 用例 | 断言要点 |
| --- | --- | --- |
| SL1 | 已注册 session 可查到 | 句柄非空；`session_id` 一致 |
| SL2 | 未知 session ⇒ 空句柄 | 不抛、不创建条目 |
| SL3 | `ClearSession` 之后查不到 | insert → erase → lookup 为空 |
| SL4 | 强句柄在 erase 之后仍可用 | 先取句柄 → `ClearSession` → 句柄仍可解引用且不崩溃（证明持有强引用） |
| SL5 | 句柄释放后不阻止析构 | 释放句柄后对象按自身时机析构（引用计数回到预期） |
| SL6 | 无 `.get()` 建第二控制块 | 静态检查（§8.6） |

**组 3：notification target disappearing（Layer C，§25.7 明确要求）**

| # | 用例 | 断言要点 |
| --- | --- | --- |
| ND1 | 全部目标存在 | `target_resolved == intents`，`target_missing == 0`，产出 N 条 NOTIFICATION |
| ND2 | 全部目标消失 | `target_resolved == 0`，`target_missing == intents`，产出 0 条；无异常 |
| ND3 | 部分消失 | 守恒：`resolved + missing == intents`；存在的各产出 1 条 |
| ND4 | 目标消失后领域状态不回滚 | `finalized == true`、`HasClosedSnapshot == true`、`MeetingClosed` 恰好一次 |
| ND5 | 不产生第二个 FinalizeClose | FINALIZE_CLOSE 的 dispatch 计数仍为 1 |
| ND6 | 重复 FinalizeClose 不再产生通知 | 第二次 `OnFinalizeClose` 的 `notification_session_ids` 为空 |
| ND7 | 目录不可用（weak 失效） | 计 `directory_unavailable`；无异常；领域状态不变 |

**组 4：best-effort / 可观测性边界（Layer C）**

| # | 用例 | 断言要点 |
| --- | --- | --- |
| BF1 | L0/L1/L2/L3 计数可断言 | `target_missing` / `directory_unavailable` / `outbound_produced` / `adapter_exception` |
| BF2 | collector 抛异常 | 异常被吞、计 `adapter_exception`；WorkerLoop 存活（后续 work 仍被处理） |
| BF3 | 不阻塞 | 用调用序与计数断言，**不用计时** |
| BF4 | 无 ACK / 无 retry | 同一目标在同一次 `OnFinalizeClose` 内只被解析/产出**恰好一次** |
| BF5 | **不得声称送达** | Review gate：adapter/collector 内不得出现暗示送达的语义与命名 |
| BF6 | 生产 collector 不调用 `Send` | 静态检查：`CountingNoWireCollector` TU 内无 `Send(`、无 `CSession` include |

**组 5：MeetingView permission boundary（安全）**

| # | 用例 | 断言要点 |
| --- | --- | --- |
| MV1 | `OK` ⇒ `has_view == true`，字段完整 | `host_user_id`/`active_participant_count` 与领域一致 |
| MV2 | `PERMISSION_DENIED` ⇒ `has_view == false` | 按 §10.3 矩阵，两字段**不存在** |
| MV3 | `NOT_PARTICIPANT` ⇒ `has_view == false` | 同上 |
| MV4 | CLOSED + 非历史成员 ⇒ `NOT_PARTICIPANT` ⇒ 不泄露 | 走 snapshot 分支（`MeetingService.cpp:771-778`） |
| MV5 | CLOSED + 历史成员 ⇒ `OK` ⇒ 视图来自 snapshot | `has_closed_at == true` |
| MV6 | 幂等命中的视图透传 | 不新增填充逻辑；与 M3 行为逐字段一致 |
| MV7 | 静态核对 | §10.5 的 grep 检查项（Review gate） |

**组 6：`CLOSING` / `CLOSED` → `SESSION_STATE_REJECTED`**

| # | 用例 | 断言要点 |
| --- | --- | --- |
| SS1 | `CLOSING` ⇒ `SESSION_STATE_REJECTED` | **不是** `AUTH_REQUIRED`（`MeetingService.cpp:170-174`） |
| SS2 | `CLOSED` tombstone ⇒ `SESSION_STATE_REJECTED` | 同上；M5.1 tombstone 语义未被绕过 |
| SS3 | 从未 REGISTER ⇒ `AUTH_REQUIRED` | 与 SS1/SS2 形成对照，锁定"两种失败不可混淆" |
| SS4 | adapter 不因 lookup 失败而改写结果 | 即使目录返回空，`result_code` 仍为 `SESSION_STATE_REJECTED` |
| SS5 | response 无法解析目标 | 只计数；不产生 `AUTH_REQUIRED`、不产生第二条 response |

**组 7：顺序与生命周期**

| # | 用例 | 断言要点 |
| --- | --- | --- |
| OR1 | response ordering == enqueue ordering | 连续 CREATE/QUERY/CLOSE，RESPONSE 顺序与入队一致，request_id 对应 |
| OR2 | adapter 不改变 FIFO 顺序 | CLOSE 之后仍有命令时，FINALIZE_CLOSE 仍排在尾部（复用 M4 §10.3 的入队瞬间快照断言） |
| OR3 | Close 的 response 早于 FinalizeClose | turn 1 响应 `ENDING`；`MeetingClosed` 在 turn 2 后恰好一次 |
| OR4 | `ShutdownWorkExecutor` 幂等 | 连续调用两次不崩溃、不重复 drain；之后再入队返回 false |
| OR5 | drain 期间仍可 lookup | `ShutdownWorkExecutor` 前后目录均可用（server 尚未析构，§12.3） |

### 16.3 如何避免复制生产分发逻辑

```text
可复用的真实生产组件：
  SerializedWorkExecutor<LogicWorkDispatcher> / LogicWorkDispatcher / MeetingService
  MeetingResponseNotificationAdapter（真实实现，必须被真实驱动）
  MeetingRequestContextBuilder（真实实现）

测试提供的替身（只记录，不实现业务）：
  RecordingSink             : IWorkObservationSink（既有）
  RecordingMessageCollector : IMeetingOutboundMessageCollector（新增，记录 typed 消息）
  FakeSessionDirectory      : IMeetingSessionDirectory（新增，返回受控 shared_ptr 或空）
  FakeOutboundSink          : IMeetingOutboundSink（新增，证明句柄生命周期）

明确禁止：
  在测试里复制 DispatchWork/Dispatch* 的分发表
  #define private public 或访问私有成员
  只断言 enqueue 而不断言 dispatch 后的可观测结果
  EXPECT_TRUE(true) 占位 / 依赖 sleep 的竞态断言
```

### 16.4 happens-before 纪律（针对既有 flaky 的教训，M6 必须遵守）

**既有测试债（本轮复核，M6 **不**修，也**不得**复制其反模式）**：

```text
flaky #1  LogicWorkQueueTest.cpp:1135
          LogicWorkQueueShutdown.PendingCloseProducesFinalizeDuringDrain
          ASSERT_EQ(1u, scenario.Queue().Size())     ← 队列占用瞬时值竞态
          实测 ≈2/400（整轮）/ 3/300（聚焦）
          根因：EnqueueBatch 只持 idle_mutex_ 就返回；消费者在 queue mutex 下 Pop；
                Size() 只锁 queue mutex ⇒ 断言前提不成立

flaky #2  LogicWorkQueueTest.cpp:681
          LogicWorkQueue.DispatchCanReenqueueToTheSameFifoWithoutDeadlock
          EXPECT_TRUE(fixture.Handler().HasClosedSnapshot(meeting_id))
          实测 ≈1/400（整轮）/ 19/20000（聚焦）
          根因：测试线程在**没有 happens-before** 的情况下读取消费者线程写入的
                non-thread-safe 状态（裸 std::map + sink 裸 std::vector）
          ⚠️ 正确根因**不是**"RecordDispatch 发生在 FinalizeClose 之前"。
             实测 LogicWorkDispatcher.cpp:157-165 的 FINALIZE_CLOSE 分支为
             FinalizeClose(...) 先、RecordWorkDispatched(...) 后；
             裁定实验：等到 dispatch 记录出现后快照仍 absent（14/14 finalize_records_seen=0）；
             而先 Done() 再断言 ⇒ 5000/5000 PASS。
```

**M6 测试的强制纪律**：

```text
R1  对领域状态 / collector 计数的读取，必须建立在**同步边**之上：
      WaitUntilIdle(5000, baseline) / StopAndJoin() / 持 LogicWorkQueue::Lock 用 *Locked 版本
R2  绝不断言队列瞬时占用（Size()/IsEmpty()）作为"某 work 已受理"的证据
R3  绝不断言中间状态的可见性作为领域效果的证据；只断言最终稳定结果
R4  M6 新增的 recording 替身由测试线程读取、消费者线程写入
      ⇒ 替身内部必须用 mutex/atomic 保护（测试基础设施加锁不触碰生产语义）
R5  需要"通知已发生"的可等待信号时，复用 executor 的 idle 判定，不自造第二套等待
R6  **不得**用"目录 weak 失效"以外的任何方式模拟目标消失（保持与生产语义一致）
```

### 16.5 测试 target 的构建影响

```text
tests/CMakeLists.txt 新增：../MeetingResponseNotificationAdapter.cpp
（与既有 ../LogicWorkDispatcher.cpp 同风格）

仍然**不**进入测试 target（必须保持）：
  LogicSystem.cpp / CServer.cpp / CSession.cpp / ChatServer.cpp
  ⇒ 会拉入 asio / Boost / Redis / MySQL / gRPC / jsoncpp 整条 legacy 链路
```

### 16.6 M6 自带的回归护栏

```text
1. 既有 224 个测试**一个都不许改、一个都不许少**
2. Layer A 中与通知无关的断言必须保持通过
3. 整轮重复（如 200 轮）用于发现 M6 自身新竞态：
   **必须先能区分既有 flaky #1/#2 与 M6 回归**，否则产生假阳性
```

---

## 17. Implementation Plan

### 17.1 编码切分（每步可独立 Review / Build / Test / 回退）

| 步 | 内容 | 产出 | 回退边界 |
| --- | --- | --- | --- |
| **M6.1** | `MeetingOutboundMessage.h` + `MeetingOutboundChannel.h`（三接口 + typed 值对象，仅标准库）+ C++14 门禁 | 两个新头 | 删除文件 |
| **M6.2** | `MeetingRequestContextBuilder.h` + 组 1 用例（CB1–CB6） | builder + 测试 | 删除文件与用例 |
| **M6.3** | `CServer::FindSessionBySessionId`（`_mutex` 下 find + 复制 shared_ptr）；`CSession` 声明实现 `IMeetingOutboundSink`；组 2 用例 | 两个既有文件最小改动 | 回退 `CServer.*`/`CSession.*` |
| **M6.4** | adapter `.h/.cpp` + `CountingNoWireCollector`；组 3/4/5 用例 | adapter 实现 | 删除 adapter 文件与用例 |
| **M6.5** | `LogicWorkDispatcher` 增加 adapter 指针与八个调用点；保持 sink 顺序与 FINALIZE_CLOSE 入队不变；组 6/7 用例（含 OR4/OR5） | dispatcher 改动 | 回退 `LogicWorkDispatcher.*` |
| **M6.6** | `LogicSystem` 装配 + 三个新入口；`ChatServer.cpp` 改 `make_shared` + 注册 + 关闭序列；CMake | 装配与关闭顺序 | 回退 `LogicSystem.*`/`ChatServer.cpp`/CMake |
| **M6.7** | 全量验证：Linux 双配置构建、ctest（224 + 新增）、C++14 门禁、依赖隔离、runtime smoke（含目录可用证据）、整轮重复并区分既有 flaky | 验证记录 | 回退该步 |
| **M6.8** | 文档收尾：COMPLETE、Scope Review、验证边界、已知限制、提交命令 | 文档 | 无需回退 |

### 17.2 每步强制 Review focus

```text
M6.1：接口只依赖标准库；值对象**不含任何协议号字段**
M6.2：builder 不读 _user_uid、不伪造 device、不补偿空 request_id
M6.3：find 在 _mutex 下；从注册表复制 shared_ptr；无 .get() 造控制块；
      不改 ClearSession 既有行为；不改 Send 既有语义
M6.4：安全矩阵逐格核对；L0–L5 边界如实；无 retry/ACK；异常不外逸；
      计数不被用于业务判定；生产 collector 不调用 Send
M6.5：O1–O5 顺序证明；FINALIZE_CLOSE 仍不插队；sink 观测顺序不变
M6.6：§12.3 关闭顺序；§12.5 注册时序三保证；make_shared 的理由与影响
M6.7：既有 224 用例零改动；新增 flaky 与既有 flaky 区分
M6.8：验证边界如实（§20.2），不声称端到端可用
```

### 17.3 回退策略（整体）

```text
新增：MeetingOutboundMessage.h / MeetingOutboundChannel.h / MeetingRequestContextBuilder.h /
      MeetingResponseNotificationAdapter.h/.cpp / CountingNoWireCollector(.h/.cpp) /
      tests/MeetingResponseNotificationAdapterTest.cpp
修改：LogicWorkDispatcher.h/.cpp、LogicSystem.h/.cpp、CServer.h/.cpp、CSession.h/.cpp、
      ChatServer.cpp、CMakeLists.txt、tests/CMakeLists.txt
整体回退 = 删除新增 + git checkout -- 上述既有文件
meeting/* 零改动；FIFO/executor/internal work payload 不受影响
```

### 17.4 人工提交命令（M6 Coding 完成后）

```bash
cd /home/tobeki/projects/rtc-signaling-platform
git status --short
git diff --check
git diff --stat
git --no-pager diff
git add <显式逐项列出，不使用 git add -A>
git diff --cached --stat
git --no-pager diff --cached
git commit -m "feat(chatserver): add meeting response and notification adapter"
git push origin dev
```

---

## 18. Known Risks

### R-1：M6 被误当成"可发送"（**首要**）

```text
风险：把 typed 出站对象误解为"已经能发消息"，或在 Phase 3 之前偷偷塞协议号
影响：高（协议污染 + 能力误报）
缓解：§7.5 显式声明无真实发送；§7.3 生产 collector 不调用 Send；
      §21.1 A7/A8；§13.3 L4/L5 不可验证
```

### R-2：`ClearSession` 的 `find` 未持锁（既有，M6 新增第三个访问者）

```text
风险：新增 FindSessionBySessionId 后 _sessions 并发面扩大
影响：中（既有隐患被放大，且 M6 明令不修）
缓解：新增 find 在 _mutex 下；不改 ClearSession；如实记录为未消除
```

### R-3：注册时序写错 ⇒ adapter 静默拿到空目录

```text
影响：高（通知永远不发出且无报错）
缓解：§12.5 三重论证；ND7；runtime smoke 增加目录可用证据
```

### R-4：复用 `IWorkObservationSink` 承载生产行为

```text
影响：中高（破坏 M4 §4.5 契约；观测与行为混杂）
缓解：§5.1 方案 C 被否决；Coding 不得给 sink 增加有副作用的实现
```

### R-5：`CServer` 改 `make_shared` 的连带影响

```text
风险：若有未知代码依赖 CServer 为栈对象或依赖其析构时机
影响：中
缓解：已核对唯一构造点（ChatServer.cpp:50）；§12.3 关闭顺序把析构时机显式化；
      Review 时全仓 grep CServer 构造与引用
```

### R-6：关闭顺序若被交换（如先关 Redis 再 ShutdownWorkExecutor）

```text
风险：drain 期间 lookup 访问已析构目录 / Redis 已被关闭
影响：高（UB 或启动失败）
缓解：§12.3 顺序表；OR4/OR5；runtime smoke 检查关闭日志顺序
```

### R-7：adapter 异常杀死唯一消费者线程

```text
影响：高（Meeting 子系统整体停摆，现象是静默不再处理 work）
缓解：§13.4 noexcept 边界；BF2
```

### R-8：response 编码泄露会议摘要

```text
影响：高（违反 M3 冻结裁决）
缓解：§10.3 矩阵 + §10.5 静态检查 + MV2/MV3/MV4
```

### R-9：强句柄延长对象生命被误读为"复活"

```text
风险：有人以为 lookup 会让已 CLOSED 的 Session 重新可用
影响：低-中
缓解：§8.3 明确"不重新插入 _sessions、不阻止 Close()"；SL4/SL5
```

### R-10：通知计数退化为第二套领域状态

```text
影响：中（观测指标变成隐式契约）
缓解：§7.3 / §13.3 明确计数不参与判定；Review 核对无 if 依赖它
```

### R-11：Windows 门禁被误报

```text
影响：中高（虚假通过）
缓解：§22 明确 Linux CMake 为唯一门禁；Windows 记 NOT VERIFIED
```

---

## 19. Conflicts & Adjudication Record

本节记录四个 Conflict 的**裁决**。**Phase 1 仍然冻结，未被修改**。
以下裁决是"本项目在 M6 范围内对 Phase 1 条款的执行方式选择"，不是对原文的修订。

### 19.1 Conflict #1：TCP message ID 与出站接口

**裁决：拒绝占位 message ID。采用严格 transport-neutral 的 typed 出站边界。**

```text
理由：
  1. CSession::Send(std::string, short) 的 short 必然是 4 字节头里的协议号；
     传任何值都等价于"分配了一个未定义的协议号"，无论怎么注释都无法自证不是 wire ID。
  2. STEP-1.6 §25.7 Files forbidden 明确含"TCP message ID 分配"；
     §25.8 把 Wire Adapter 整体延到 Phase 3。
  3. 因此 M6 的输出边界必须**完全绕开 CSession::Send**。

裁决内容：
  - M6 交付 typed 值对象 MeetingOutboundMessage（MeetingResponseMessage /
    MeetingNotificationMessage），**不含任何协议号字段**（§7.2）；
  - 出站接口为 IMeetingOutboundMessageCollector::Collect(const MeetingOutboundMessage&)，
    不接受未定义格式的字符串 payload（§7.3）；
  - 生产的 CountingNoWireCollector 只计数、**不调用 CSession::Send**、不产生任何字节；
  - 真实字节交付（含协议号分配、帧、编码）由 Phase 3 的 wire adapter 实现。

与 §25.7 的关系（**必须如实**）：
  - "session_id → active CSession lookup"   ：M6 **真实交付**（§8，lifetime-safe）
  - "local notification intent"             ：M6 **真实交付**（typed 通知消息）
  - "missing Session best effort"           ：M6 **真实交付**（L0 skip 可观测）
  - "领域结果可回到调用侧"（Resume value）   ：M6 交付到 typed 值对象为止
  - 真实通知发送能力                         ：**M6 不具备**，属 Phase 3（§7.5）

Phase 1 原文未变：§25.7 的 Build gate 与 Files forbidden 均未修改；
本裁解决的是"在不违反 §25.8 的前提下，M6 的输出边界长什么样"。
```

### 19.2 Conflict #2：Session lookup 的形式

**裁决：允许窄接口，但拒绝裸指针返回；必须解决对象生命周期。**

```text
裁决内容：
  - 保留窄接口 IMeetingSessionDirectory（不暴露 CServer / CSession 具体类型，
    不暴露可变注册表）；
  - lookup 返回 std::shared_ptr<const IMeetingOutboundSink>（**强句柄**），
    禁止裸借用指针；
  - 实现必须在自有互斥下从持有 shared_ptr 的注册表**复制**（禁止 .get() 造控制块）；
  - 目录本身以 std::weak_ptr<IMeetingSessionDirectory> 被 adapter 引用（判活）。

与 §15.2 的关系：
  §15.2 原文建议 "FindSession(session_id) → std::shared_ptr<CSession>（或等价受控 lookup）"。
  本裁决保持 shared_ptr 语义（**强于**推荐形式的"受控"要求），
  但把元素类型收窄为 const IMeetingOutboundSink，以同时满足
  "adapter 不依赖 CSession 具体类型"。
  ⇒ 属于"等价受控 lookup"的实现选择，**Phase 1 原文未变**。

Phase 1 原文未变：§15.2 / §15.3 / §25.7 均未修改。
```

### 19.3 Conflict #3：Windows Build gate 与 R6

**裁决：采用方案 iii。**

```text
裁决内容：
  - M6 的工程验收门禁 = **Linux CMake build + ctest**（BUILD_TESTING=ON 与 OFF 两种配置）；
  - Windows `Debug|x64` **保持 NOT VERIFIED**；
  - R6（新文件未加入 .vcxproj）**留待独立任务**，不在 M6 内处理；
  - **不得**修改 .vcxproj / .filters / PropertySheet.props / .sln；
  - 不得声称 Windows 通过。

理由：
  R6 未接线（LogicWorkDispatcher.cpp 与 meeting/*.cpp 不在 ClCompile，15 个 ClCompile /
  17 个 ClInclude），而 §25.7 Files forbidden 含 PropertySheet.props
  ⇒ 当前基线上"满足 Windows Debug gate"与"不扩大范围"不可能同时成立。
  方案 iii 是唯一不伪造、不扩范围的选择。

Phase 1 原文未变：§25.7 的 "Build gate: Debug|x64 编译通过" 未被修改；
本裁决记录的是"M6 在本基线上实际采用的门禁"，并把差异留给人工与后续任务。
```

### 19.4 Conflict #4：Context builder 的生产调用者

**裁决：允许 builder 通过 direct invocation / 测试 adapter 验证；生产调用者留待 Phase 3。**

```text
裁决内容：
  - M6 交付 MeetingRequestContextBuilder（真实实现）+ Layer C 用例；
  - 生产调用者（wire adapter）在 Phase 3 接入，M6 **不存在**该入口；
  - **不得**把 direct invocation 表述为生产端到端验证（§14.3）。

依据：STEP-1.6 §25.7 条件说明明确允许该验证方式。

Phase 1 原文未变。
```

### 19.5 裁决汇总表

| Conflict | 初版状态 | M6.0 裁决 | 是否阻塞 Coding |
| --- | --- | --- | --- |
| #1 占位 msgid | 未决（1a/1b 二选一） | **拒绝占位 ID；typed transport-neutral 边界；无真实发送** | 否 |
| #2 lookup 形式 | 未决（裸指针） | **窄接口 + shared_ptr 强句柄 + weak 目录** | 否 |
| #3 Windows gate | 未决（互相矛盾） | **方案 iii：Linux 门禁；Windows NOT VERIFIED；R6 独立任务** | 否 |
| #4 builder 调用者 | 未决 | **direct invocation 验证；生产调用者 Phase 3** | 否 |

---

## 20. Deferred Work

### 20.1 明确不属于 M6

```text
1.  真实字节交付：TCP message ID / JSON / protobuf / Frame V2 / request_id encoding /
    Qt command / HTTP / WebSocket                              → Phase 3（§25.8）
2.  L4（异步写失败）与 L5（客户端送达）的可观测化               → Phase 3（§13.3）
3.  Layer D 真实 TCP 端到端测试                                → 协议 adapter 落地后
4.  Windows Debug|x64 验证与 R6                                → 独立任务（Conflict #3）
5.  Domain session tombstone retention                         → Deferred（M5.1 §7）
6.  ClosedSnapshot retention / 淘汰策略                        → Deferred（M3 遗留）
7.  既有 flaky #1（LogicWorkQueueTest.cpp:1135）                → 单独一步修复
8.  既有 flaky #2（LogicWorkQueueTest.cpp:681）                 → 与 #1 同一步修复
9.  CServer::ClearSession 的 find 未持锁                       → 既有并发隐患
10. CSession::_user_uid 构造未初始化                           → 既有（M6 不读它）
11. MysqlDao.cpp:95 告警 / re2_DIR 残留（R7）                   → 既有
12. ChatServer / ChatServer2 双份维护（R8）                     → 需先解决共享源码/构建
13. 多节点 Meeting 路由与 MEETING_NOT_LOCAL 真实重定向          → Phase 1 明确不支持跨节点
14. MeetingEvent 的可靠投递 / ACK / retry / 死信                → 无设计
15. 通知的批量合并 / 去重 / 延迟发送                            → 本步明确禁止（§11.4）
16. legacy IM 完整业务链路（MySQL 好友/申请）验证               → 无 MySQL 环境
17. 真实 Login → AUTHENTICATE 的 runtime 验证                   → M5.1 §10.1 未解决
```

### 20.2 M6 完成后仍然存在的验证边界（必须如实标注）

```text
(a) CServer::FindSessionBySessionId 的实现、CServer 的注册时序、关闭顺序、
    CSession 的实际析构时机 —— 由**静态复核 + runtime smoke**保证，不由单测保证
    （CServer.cpp / CSession.cpp / LogicSystem.cpp / ChatServer.cpp 不在测试 target 内）。
(b) 真实客户端 → Meeting 命令 → 响应 的端到端路径**不存在**（无 Protocol V2）。
(c) 通知的**送达**不可验证（L5）；异步写失败不可验证（L4）。
(d) 真实字节交付**未实现**（Phase 3）。
(e) Windows 侧一切结论均不可在 Linux 环境验证。
```

---

## 21. Acceptance Criteria

### 21.1 结构类（静态可核）

```text
A1   adapter 不持有 CServer：grep -n "CServer" MeetingResponseNotificationAdapter.h == 0 命中
A2   adapter 不 include CSession.h / CServer.h / LogicSystem.h / asio / boost
A3   adapter 不认识 MeetingAggregate / registry / LogicWorkQueue / MeetingService
A4   meeting/* 零改动：git diff --stat -- Server/ChatServer/ChatServer/meeting/ 为空
A5   只存在一条 FIFO、一个 work 消费者：LogicWorkQueue 实例与 work 消费线程数不增加
A6   adapter 不向任何队列投递 work（无新增 Enqueue / Push / PostInternalWork）
A7   typed 出站值对象**不含任何协议号字段**（MeetingOutboundMessage.h grep 无协议号）
A8   生产 collector 不调用 CSession::Send、不 include CSession.h、不产生字节
A9   无新增 timer / background thread / 清理线程
A10  internal work payload 仍只含稳定 ID；无伪 msg_id、无假 LogicNode
A11  MeetingView 的每次字段读取都位于 has_view == true 分支内（§10.5 grep）
A12  lookup 返回 shared_ptr；无 .get() 造第二控制块（§8.6）
A13  message.proto / *.sln / *.vcxproj / *.props / *.filters / MyChat_Qt / ChatServer2 零改动
A14  Phase 1 文档零改动
```

### 21.2 行为类（测试可核）

```text
B1   Layer C 组 1–7 全部通过（§16.2；可按表驱动合并，语义不得减少）
B2   既有 224 个测试**一个不改、一个不少**，全部通过
B3   Layer A 中与通知无关的断言保持通过
B4   通知失败不回滚状态：X11 / ND4（finalized==true、HasClosedSnapshot==true、MeetingClosed 恰好一次）
B5   ENDING → CLOSED 必然收敛（即使目标全失、目录失效、collector 抛异常）
B6   lookup 失败不改变领域结果（SS4/SS5）
B7   CLOSING/CLOSED ⇒ SESSION_STATE_REJECTED（不是 AUTH_REQUIRED）（SS1/SS2/SS3）
B8   安全边界：MV2/MV3/MV4 通过
B9   顺序：OR1/OR2/OR3 通过
B10  无 ACK / 无 retry：BF4 通过
B11  句柄生命周期：SL4/SL5 通过（erase 后句柄仍可用；释放后不泄漏）
B12  关闭幂等与 drain：OR4/OR5 通过
B13  可观测性边界：BF1/BF5/BF6 通过（仅 L0–L3 可统计；不声称 L4/L5）
```

### 21.3 工程类

```text
C1  Linux out-of-tree 全新构建 PASS（BUILD_TESTING=ON 与 OFF）；不得新增警告
C2  ctest 全绿；用例总数 = 224 + M6 新增数（必须给出该数字）
C3  C++14 门禁：新文件在 -std=c++14 -pedantic-errors 下通过；无 C++17 能力
C4  依赖隔离：MeetingDomain 仍不链接任何东西；adapter TU 不引入新第三方依赖
C5  runtime smoke：起停正常；无 terminate/abort/exception；
    含"adapter 已注入 + 目录可用"的可观测证据；检查关闭顺序日志
C6  仓库内无 build artifact / config.ini / 日志 / 密码
C7  Windows Debug|x64：**NOT VERIFIED**（Conflict #3 裁决 iii），不得声称通过
```

### 21.4 文档类

```text
D1  本文档状态更新为 COMPLETE，补入实际改动清单、测试结果、验证边界、已知限制
D2  如实记录"真实字节交付仍留 Phase 3"（§7.5）
D3  如实记录 §20.2 的五条验证边界
D4  任何对 §15 Scope Matrix 的偏离必须逐条记录并获得授权
```

---

## 22. Windows / 编码 / 门禁

### 22.1 M6 的工程门禁（Conflict #3 裁决 iii）

```text
权威门禁：Linux CMake build + ctest
  cmake -S Server/ChatServer/ChatServer -B <out-of-tree> -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
  cmake --build <dir> --parallel 2
  ctest --test-dir <dir> --output-on-failure
  以及 BUILD_TESTING=OFF 的纯应用构建

Windows Debug|x64           = **NOT VERIFIED**（本环境无法验证；且 R6 未接线）
R6                          = **DEFERRED → 独立任务**
.sln / .vcxproj / .props / .filters = **不得修改**
```

### 22.2 编码

```text
新增文件一律 UTF-8（无 BOM），与 M4/M5/M5.1 新增文件一致。
本次修改涉及的既有文件编码现状（M6 Coding 时必须保持各自原编码或按显式授权转换）：
  LogicSystem.h/.cpp        : UTF-8（M4 §16.1 起）
  LogicWorkDispatcher.h/.cpp: UTF-8
  CServer.cpp               : UTF-8（M5.1 §11.2 授权转换）
  CServer.h                 : ASCII
  CSession.h/.cpp           : UTF-8（M5 §7.2 授权转换）
  ChatServer.cpp            : UTF-8 带 BOM（现有文件；修改时需保持）
  AsioIOServicePool.h/.cpp  : **GBK**（既有；M6 不修改）
M6 不主动转换任何既有文件编码。
```

---

## 23. Explicit Non-Goals

```text
1.  不实现 TCP Meeting Protocol、不分配任何 TCP message ID、不做 JSON/protobuf 映射、
    不定义 Frame V2、不编码 request_id 的 wire 形式                    → Phase 3（§25.8）
2.  不实现真实字节交付；生产 collector 不发送                       → §7.5
3.  不统计/不声称 L4（异步写失败）与 L5（客户端送达）                → §13.3
4.  不做真实客户端端到端（Layer D）                                → 协议 adapter 落地后
5.  不修改领域语义（ResultCode / 事件契约 / 状态机 / Close 两 turn /
    MeetingView 填充规则）                                          → Phase 1 冻结
6.  不引入第二 FIFO、第二 worker、线程池、timer、后台清理            → §11.4 / M4 §5.1
7.  不引入 ACK / retry / 死信 / 可靠投递 / 通知批量合并              → §9.1
8.  不让 MeetingService 持有 CServer、不直接操作 CSession、
    不把 CSession 放进 LogicWorkItem、不伪造 msg_id、不包装假 LogicNode → M4 §4.2 / §12.5
9.  不把生产 response/notification 挂到 IWorkObservationSink 上      → M4 §4.5
10. 不给 MeetingService 加锁、不引入线程、不引入 async task          → Step 1.3 §20
11. 不修既有 flaky #1/#2、不修 ClearSession 的 find 锁缺陷、
    不初始化 CSession::_user_uid（M6 不读它）                        → 既有债
12. 不处理 Windows R6、不添加 tombstone TTL/LRU/timer/background cleanup、
    不做 ClosedSnapshot retention/淘汰；**Windows 记 NOT VERIFIED**    → Conflict #3
13. 不改 message.proto / *.sln / *.vcxproj / *.props / *.filters /
    MyChat_Qt/ / Server/ChatServer2/                                → §15.3
14. 不改既有 7 个测试文件、不减少既有 224 个测试                      → B2
15. 不引入新的第三方依赖、不使用 C++17 能力                          → §7 / C3
16. 不在本步（M6.0）实现任何代码                                    → §2.3
17. 不把本轮的裁决描述为"Phase 1 已被修改"                          → §1.4 / §19
```

---

## 24. 本轮工作区状态与 git 命令

```bash
git status --short
git diff --check
git diff --stat
git --no-pager diff -- docs/phase2/M6-response-notification-adapter.md
```

> 本轮为纯文档修订：上述命令显示仅 `docs/phase2/M6-response-notification-adapter.md`
> 被修改，且无空白错误。
>
> 提交与推送的实际结果：
>
> ```text
> 本地 commit : 一条本地 commit（subject "docs(phase2): adjudicate M6 design -
>               transport-neutral outbound boundary"；hash 以 git log -1 为准）
> push        : FAILED（网络不可达：代理 127.0.0.1:7897 未运行；直连亦不可达）
> origin/dev  : 仍为 afad064（本轮 commit 未到达远端）
> ```
>
> 因此本文档的当前状态是"**已本地提交、未推送**"。
> 在 `git push` 成功之前，任何地方都不得声称本轮修订已推送。

---

## 25. 下一步

```text
1. 人工 Review 本修订，确认 §19 的四个裁决；
2. 按 §17.1 切分为独立的 M6.1–M6.8 Coding Prompt；
3. 每步完成 → Review → Build → Test → 再进入下一步；
   不得把 M6.1–M6.8 合并成一个 Prompt（STEP-1.6 §26.2 同一纪律）。
```
