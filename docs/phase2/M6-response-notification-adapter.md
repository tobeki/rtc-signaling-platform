# Phase 2 - M6：Response / Notification Adapter

> 状态：**DESIGN COMPLETE（M6.0 / M6.0.1 / M6.0.2 / M6.0.3 四轮修订；Coding NOT STARTED）**
> Phase 2 M-planning 原始基线：`da19189a55ab13058a22c38c228d3043df72bdf6`（M5.1 收尾）
> 本文档首次提交：`afad0644170b0882ee6b58ed99c493e82909a70f`（`docs(phase2): add M6-response-notification-adapter.md`）
> 已发布的 M6.0 基线：`3456c7f96767ea23c59f856aca12ba50deb41699`
> 已发布的 M6.0.1 基线：`1454a8a5d6ec2a4c7238244c98f5f578e5bb2904`
> 已发布的 M6.0.2 基线：`2ffd08330a2047c6cf74fafba07f0f0a675a4f7f`
> 本文档当前修订：**M6.0.3 — Shutdown Failure Policy & Ownership Consistency Review**（工作区状态见 §1.3 与 §24）
> M6 Coding：**NOT STARTED**（Final Coding Gate 由人工 Review 决定）

---

## 1. Status

### 1.1 里程碑与文档状态

| 项 | 值 |
| --- | --- |
| Phase 2 M-planning 原始基线 | `da19189a55ab13058a22c38c228d3043df72bdf6`（HEAD == origin/dev，M5.1 收尾） |
| 本文档首次提交（Phase 2 M6 Planning） | `afad0644170b0882ee6b58ed99c493e82909a70f` — `docs(phase2): add M6-response-notification-adapter.md` |
| **已发布的 M6.0 基线** | `3456c7f96767ea23c59f856aca12ba50deb41699` — `docs(phase2): adjudicate M6 design - transport-neutral outbound boundary` |
| **已发布的 M6.0.1 基线** | `1454a8a5d6ec2a4c7238244c98f5f578e5bb2904` — `docs(phase2): adjudicate M6 design` |
| **已发布的 M6.0.2 基线** | `2ffd08330a2047c6cf74fafba07f0f0a675a4f7f` — `docs(phase2): fix M6 shutdown path compileability and exception safety` |
| 本修订轮次 | **M6.0.3 — Shutdown Failure Policy & Ownership Consistency Review** |
| 本修订依据 | 对 `2ffd083`（文档）与当前代码 + 本机 gRPC 1.51.1 头文件的只读复核；**未修改任何代码** |
| M6 Coding | **NOT STARTED** |
| 生产代码 / 测试代码 / CMake / Phase 1 文档 | **本轮零修改** |

### 1.2 本修订解决的问题

**M6.0（已发布 `3456c7f`）解决六项：**

```text
1. TCP message ID 与出站接口：剔除占位 msgid，改为严格 transport-neutral 的 typed 出站边界（§6、§7）
2. Session lookup 生命周期：裸借用指针 → weak_ptr 判活 + shared_ptr 强句柄（§8）
3. worker / CServer / io_context 关闭顺序：显式 ShutdownWorkExecutor + weak 目录（§12）
4. Response 原始 Session：adapter 接口增加可信 MeetingRequestContext（§6.3）
5. 发送失败可观测性：五档分辨率的显式边界，不再声称可观测送达（§13.3）
6. Context Builder 身份来源：只用服务端可信身份，绝不读 _user_uid（§14）
```

**M6.0.1（本轮）修正 M6.0 遗留的五处阻塞项：**

```text
B1  AsioIOServicePool::Stop() **非幂等**（§12.3 原方案会在信号回调与清理路径各调一次）
    → 采用方案 A：单一 Stop 调用点 + 全生命周期 ShutdownCoordinator；详见 §12.3
B2  §12.2 成员声明顺序与逆序析构论证有误
    → 更正为 _outbound_collector → _meeting_adapter → _dispatcher → _work_executor；详见 §12.2
B3  IMeetingOutboundSink::AcceptMeetingOutbound 为非 const，却经 shared_ptr<const ...> 返回
    → 改为 const-correct 的**存活标记**接口 IMeetingLiveSession；详见 §8.2 / §8.3
B4  通知逐目标算法与 collector 所有权未写死（多目标可能复用单条消息；collector 有悬垂风险）
    → 给出逐目标算法 + collector 由 LogicSystem 拥有；详见 §7.1 / §12.1
B5  §1.3 / §24 的 Git 状态已过期（`3456c7f` 实际已在 origin/dev）
    → 已更正；详见 §1.3
```

**M6.0.2（本轮）修正 M6.0.1 遗留的四处阻塞项：**

```text
C1  对象命名冲突：M6.0.1 §12.3.4 把 TCP `CServer` 也叫 `server`，而源码中
    `std::unique_ptr<grpc::Server> server` 已占用该名；且 `server->Shutdown()` 是
    gRPC 的 API，`CServer` 根本没有 `Shutdown()`。
    → 明确区分 grpc_server / tcp_server / grpc_server_thread / io_context / pool（§12.3.1 / §12.3.6）
C2  异常安全覆盖不足：M6.0.1 的 ShutdownGuard 只覆盖 run() 之后，且 on_leave 为 nullptr；
    grpc_server_thread 在 CServer 之前启动 ⇒ guard 构造前抛异常会直接 std::terminate。
    → 全生命周期 ShutdownCoordinator（gRPC 线程启动后立即构造）+ 逐步 try/catch（§12.3.5 / §12.3.9）
C3  信号回调线程模型描述错误：M6.0.1 称回调在 pool 线程上、可能自我 Join。
    按源码，signals 绑定主 io_context、run() 在主线程 ⇒ 回调在主线程。
    → 删除"必然自我 Join"结论，保留"单一受控 Stop 调用点"裁决并给出正确理由（§12.3.1 / §12.3.4）
C4  §12.3.4 目标形态未回答 12 个生命周期问题
    → §12.3.7 四条路径差异 + §12.3.8 逐条回答 + §12.3.9 各步异常性 + §12.3.12 更早失败
```

**M6.0.3（本轮）修正 M6.0.2 遗留的三处阻塞项：**

```text
D1  失败策略缺失："捕获异常并继续"被当成生命周期安全的充分证明。
    pool->Stop / ShutdownWorkExecutor / Redis Close / grpc join 任一 join 失败后，
    流程仍会关闭下一层依赖并让对象析构 ⇒ 可能在线程仍运行时销毁它们访问的对象。
    → 步骤分级（K 级 fail-stop / B 级 best-effort）+ 明确的分级表与代价声明
      （§12.3.5.1 / §12.3.9）

D2  tcp_server 所有权自相矛盾：成员是 CServer*（非拥有），但 §12.3.8 曾称"共同持有"。
    → 采用**方案 B**：成员改为 shared_ptr<CServer>，SetTcpServer 接收 shared_ptr；
      并重证五条路径的析构顺序，同时**取消** M6.0.2 的"兜底降级 no-op"说法
      （强引用使目录在兜底期间依然有效）（§12.3.8.1）

D3  R-18 断言错误：曾称"Redis 关闭可能与 gRPC handler 并发"。
    按本机 gRPC 1.51.1 头文件原文，无 deadline 的 Shutdown() 会阻塞到同步 handler 完成，
    而本仓库 gRPC 服务是同步实现 ⇒ 步骤 1a 之后不成立。
    → 删除该断言；改为 R-18'a（顺序不变量）与 R-18'b（活性/关闭挂起）（§12.3.14 / §18）
```

### 1.3 提交状态（必须与远端事实一致）

```text
基线（本轮进入时实测）:
  origin/dev == HEAD == 2ffd08330a2047c6cf74fafba07f0f0a675a4f7f
  ⇒ M6.0.2 修订已发布
     （历史基线：M6.0 = 3456c7f、M6.0.1 = 1454a8a，均已发布）
  ⇒ 进入本轮时 working tree / index 均 clean

本轮（M6.0.3）:
  只修改本文件 docs/phase2/M6-response-notification-adapter.md
  修订内容见 §1.2 的 D1–D3

本文档刻意**不**内嵌自身的 commit hash：任何 amend / rebase 都会让它立即过期。
确认当前状态的唯一可靠方式是：
  git log -1 --oneline
  git rev-parse HEAD
  git rev-parse origin/dev
  git status --short
三者一致才说明"该修订已发布"。

⚠️ 历史更正：M6.0 曾记录"未推送"，M6.0.1/M6.0.2 曾以更早的 hash 为基线 ——
   均为当时事实，之后都已推送。不得再把 1672888 / 3456c7f / 1454a8a 当作"当前 HEAD"。
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

### 2.2 本轮（M6.0.1）范围

```text
只修订本文档，内容是"关闭路径与出站契约的一致性复核"，具体五处：
  B1  AsioIOServicePool::Stop() 非幂等 → 方案 A（单一调用点，M6.0.2 升级为全生命周期协调器）（§12.3）
  B2  成员声明顺序 / 逆序析构论证更正（§12.2）
  B3  Session sink 的 const 契约矛盾 → 收窄为 const 存活标记（§8.2）
  B4  通知逐目标算法 + collector 所有权（§7.1 / §12.1）
  B5  §1.3 / §24 的 Git 元数据更正（§1.3）
```

### 2.3 本轮明确不做

```text
不修改任何生产代码 / 测试代码 / CMake / Phase 1 文档 / Windows 工程文件 / 其他文档
不执行 git add / commit / push；本轮末尾只做只读 git 检查（§24）
不创建占位 TCP message ID、不定义 Meeting wire format
不引入第二 FIFO、第二 worker、timer 或后台线程
不开始 M6.1 Coding
不自行批准 M6 Coding —— Final Coding Gate 由人工 Review 决定
不修改 AsioIOServicePool.*（方案 A 的设计目标是零改动该 GBK 文件）
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

> 下面这段是**当前 HEAD（`3456c7f`）的真实代码**，用于陈述"现状"，
> **不是** M6 的目标形态。目标形态见 §12.3.4（信号回调不再调用 `pool->Stop()`）。
> 特别地：现状中 `pool->Stop()` 位于信号回调内，而 `Stop()` **非幂等**（§12.3.1）
> ——这是 M6.0 设计的一处阻塞项，由 M6.0.1 的方案 A 解决。

```cpp
// ChatServer.cpp:41-56（现状，非目标形态）
boost::asio::io_context io_context;
boost::asio::signal_set signals(io_context, SIGINT, SIGTERM);
signals.async_wait([&io_context, pool, &server](auto, auto) {
    io_context.stop();
    pool->Stop();      // ⚠ 现状：非幂等；M6.0.1 将其移出信号回调（§12.3.3 方案 A）
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

### 7.1 唯一路径与**逐目标**处理算法（M6.0.1 修正 B4）

#### 7.1.1 为什么必须逐目标

`FinalizeCloseResult::notification_session_ids` 是 **0..N 个目标的集合**
（`MeetingService.h:219`），而 `MeetingNotificationMessage` 只携带**单个**
`notification_target_session_id`。若用一个没有明确目标的消息代表多个目标，
或复用同一条消息多次调用 collector，则"发给谁"不可断言、"调用几次"不可断言。
因此必须写死为**逐目标生成**。

#### 7.1.2 算法（权威，Coding 必须逐句对齐）

```text
输入：const FinalizeCloseResult& result（领域已收敛：finalized / already_finalized
                                         / snapshot 已发布 / MeetingClosed 恰好一次）
输出：至多 N 条 MeetingNotificationMessage；每条恰好调用一次 collector_->Collect()

 0. 若 result.notification_session_ids 为空 ⇒ 直接返回（0 条消息，0 次 Collect）
    （典型：重复 FinalizeClose 的 already_finalized 路径；MeetingService.h:217-219）

 1. directory = session_directory_.lock()
    若 directory 为空 ⇒ directory_unavailable += 1；
                        **整批直接返回（0 条消息，0 次 Collect）**

 2. 对 result.notification_session_ids 中的**每一个** session_id（记为 target）独立处理：

    2a. handle = directory->FindSessionBySessionId(target)
        若 handle 为空 ⇒ target_missing += 1；**跳过该 target**
                          （不产生消息、不调用 Collect、不影响其它 target）

    2b. 有效目标 ⇒ target_resolved += 1
        构造**一条新的** MeetingNotificationMessage，并显式赋值：
            notification_type                = EventType::MEETING_CLOSED
            meeting_id                       = result.meeting_id
            notification_target_session_id   = target          ← **必须是当前 target**
            has_event_id / event_id          = 从 result.events 中取 MEETING_CLOSED
            has_correlation_request_id /
            correlation_request_id           = CloseContext.originating_request_id（若可得）

    2c. collector_->Collect(该消息)     ← **恰好一次**，且只针对本 target
        outbound_produced += 1

 3. 循环结束后返回（不等待、不重试、不回滚、不合并）
```

#### 7.1.3 计数守恒（可断言）

```text
设 N = result.notification_session_ids.size()
    M = 其中 lookup 为空的个数
则恒有：
    target_resolved + target_missing == N
    outbound_produced == target_resolved == 产出的 NOTIFICATION 消息条数
    （目录不可用时例外：target_resolved == target_missing == 0，outbound_produced == 0）
```

#### 7.1.4 逐目标算法与 M6.0 的差异（明确记录）

```text
M6.0 的缺陷：把"整批 no-op"与"逐目标 skip"混在同一段叙述里，且没有规定
             消息与 target 的绑定关系，存在"复用同一条无目标消息代表多个目标"的空间。
M6.0.1 修正：显式规定 2a / 2b / 2c 三步，并规定 2c 的 Collect 次数 == 有效目标数。
测试验收（§16.2 组 3）据此改写为"N 个有效目标 ⇒ N 条通知；M 个缺失 ⇒ 0 条通知"。
```

#### 7.1.5 时序图（整体）

```text
MeetingService::FinalizeClose(meeting_id)                          MeetingService.cpp:945
        │ 产出 result.notification_session_ids（intent，0..N 个稳定 SessionId 值）
        ▼
LogicWorkDispatcher::DispatchWork  case FINALIZE_CLOSE             LogicWorkDispatcher.cpp:157-165
        │ 领域 mutation 已完成；snapshot 已发布；ENDING→CLOSED 已收敛
        ▼
adapter_->OnFinalizeClose(result)
        │ weak 判活 → 逐 target：lookup → (skip | 构造单目标消息 → Collect)
        ▼
collector_->Collect(message)   ×  target_resolved      ← typed 值对象，绝不发送字节（§7.5）
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

    // **单目标**，且**必须非空**。语义见 §7.1.2 步骤 2b：
    //   一条 MeetingNotificationMessage 只代表**一个** session。
    //   N 个有效目标 ⇒ N 条消息 ⇒ N 次 Collect（§7.1.3 守恒式）。
    //   禁止复用一条消息代表多个目标，也禁止把它留空后"另想办法"表达目标。
    meeting::SessionId notification_target_session_id;

    bool has_event_id;
    meeting::EventId event_id;              // 来自 result.events 的 MEETING_CLOSED
    bool has_correlation_request_id;
    meeting::RequestId correlation_request_id;   // 来自 CloseContext.originating_request_id
};

// 不变式（adapter 内部必须自检，编码失败路径见 §13.2 第 4 条）：
//   kind == NOTIFICATION ⇒ !notification.notification_target_session_id.empty()

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

| 实现 | 使用方 | 所有权 | 行为 |
| --- | --- | --- | --- |
| `CountingNoWireCollector` | **生产**（唯一实例） | `LogicSystem::_outbound_collector`（`unique_ptr` 成员，§12.1） | 只计数（response/notification 各多少）、**绝不发送任何字节**、不 include `CSession.h` |
| `RecordingMessageCollector` | **测试** | 测试夹具（`LogicWorkDispatcher` 的所有者作用域内） | 记录全部 typed 消息，供 Layer C 逐字段断言 |

```text
生命周期硬约束（对应 B4 / R-13）：
  collector 的生命周期必须 ⊇ adapter 的生命周期。
  ✗ 禁止：在 EnsureWorkQueueStarted() 内建局部 collector 再把引用交给 adapter
          （call_once 结束时局部对象析构 ⇒ adapter 持悬垂引用）
  ✓ 要求：collector 为 LogicSystem 的 unique_ptr 成员，且声明在 adapter **之前**
          （逆序析构 ⇒ adapter 先于 collector 销毁，§12.2.3）
```

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

### 8.2 新设计：weak 判活 + shared 强句柄 + **const-correct 存活标记**

#### 8.2.1 先修正 M6.0 的契约矛盾（B3）

M6.0 同时写了两件互相矛盾的事：

```text
lookup 返回 std::shared_ptr<const IMeetingOutboundSink>   ← 只读句柄
IMeetingOutboundSink::AcceptMeetingOutbound(...) 是**非 const** 方法          ← 需要对只读句柄调用
```

二者不可能同时成立：经 `shared_ptr<const T>` 无法调用 `T` 的非 const 成员。
该矛盾必须二选一。

**裁决：M6 只需要"目标是否存活"，不需要"发送能力"。因此把接口收窄为 const-correct 的存活标记。**

```text
理由：
1. M6 的生产 collector **不发送任何字节**（§7.3 / §7.5）。若现在定义一个"可发送"的
   非 const 接口，M6 内部将没有任何合法调用者 —— 那是一个只能靠 Phase 3 才有意义的死接口，
   而且会诱导后人为了让编译通过而真的去发送；
2. 存活标记的语义与 M6 的真实职责完全一致：**逐目标判活并计数**（§7.1 / §9.1）；
3. Phase 3 需要真正的发送能力时，**新增**一个派生接口即可（§8.2.4），
   不需要修改本标记、也不需要在本轮为它设计协议。
```

#### 8.2.2 接口定义（const-correct）

```cpp
// MeetingOutboundChannel.h（新头；只含 <memory> / <string>）
//
// IMeetingLiveSession = 「某条当前存活的会话」的**只读存活标记**。
//
// 它刻意**不**提供任何发送方法：
//   - M6 的生产路径不发送字节（§7.5）；
//   - 因此不存在"定义了发送能力却永远不可调用"的死接口；
//   - 且所有方法均为 const ⇒ 与 shared_ptr<const IMeetingLiveSession> 契约一致。
//
// 它的存在价值（可断言、可测试）：
//   - 证明 lookup 返回的是**生命周期受控**的强句柄（§8.3）；
//   - 给 adapter 一个稳定的"目标已解析"证据，用于 resolved / missing 计数；
//   - Phase 3 的发送接口可基于它派生（§8.2.4）。
class IMeetingLiveSession {
public:
    virtual ~IMeetingLiveSession() {}

    // 该会话的稳定标识（值拷贝）。**只读**，用于日志与测试断言。
    virtual std::string DescribeSessionId() const = 0;
};

class IMeetingSessionDirectory {
public:
    virtual ~IMeetingSessionDirectory() {}

    // 非拥有、线程安全、**生命周期受控**：
    //   - 成功 ⇒ 返回非空 shared_ptr，句柄存活期间目标**必然存活**（强引用由句柄持有）
    //   - 失败 ⇒ 返回空 shared_ptr（目标不存在或已移除）
    // 实现方必须在自己的互斥下从"持有 shared_ptr 的注册表"中**复制**该 shared_ptr；
    // **禁止**用裸指针构造 shared_ptr（会建立第二个控制块 ⇒ 未定义行为）。
    virtual std::shared_ptr<const IMeetingLiveSession>
        FindSessionBySessionId(const std::string& session_id) = 0;
};
```

`CServer` 的实现要点（`CSession` 实现 `IMeetingLiveSession`）：

```cpp
std::shared_ptr<const IMeetingLiveSession>
CServer::FindSessionBySessionId(const std::string& session_id) {
        std::lock_guard<std::mutex> lock(_mutex);      // ← 必须在 _mutex 下 find（§8.5）
        std::map<std::string, std::shared_ptr<CSession>>::iterator it = _sessions.find(session_id);
        if (it == _sessions.end()) {
                return std::shared_ptr<const IMeetingLiveSession>();
        }
        // it->second 是 shared_ptr<CSession>（F4）；CSession 实现 IMeetingLiveSession。
        // **复制** shared_ptr ⇒ 引用计数 +1 ⇒ 语句结束后目标仍存活。
        // 隐式向上转换到 shared_ptr<const IMeetingLiveSession>。
        return it->second;
}
```

#### 8.2.3 const 一致性核对（Review gate）

```text
✅ IMeetingLiveSession 的**全部**成员函数均为 const
✅ lookup 返回 shared_ptr<const IMeetingLiveSession>
✅ adapter 只调用 const 成员（DescribeSessionId）⇒ 编译期合法
✅ adapter 从不发送：本接口根本没有发送方法
✅ CSession 实现该接口只需一个 const 方法；**不需要**改动 CSession 的既有发送语义
```

#### 8.2.4 Phase 3 如何接入真正的发送能力（不改本轮接口）

```text
Phase 3 的 wire adapter 需要一个"能接收 typed 消息"的会话能力，接入方式（择一，届时裁决）：

  方案 P1（派生接口，推荐）：
      class IMeetingOutboundSink : public IMeetingLiveSession {
      public:
          virtual void AcceptMeetingOutbound(const MeetingOutboundMessage&) = 0;   // 非 const
      };
      CSession 同时实现 IMeetingOutboundSink；
      directory 另增一个返回 shared_ptr<IMeetingOutboundSink> 的查询（或把返回类型收紧）。

  方案 P2（并列接口）：
      CSession 额外实现 IMeetingOutboundSink；directory 增一个并列查询方法。

两者都是**纯新增**，不需要修改 IMeetingLiveSession 或 M6 的任何 adapter 代码。
⇒ 本轮把"存活标记"与"发送能力"解耦，不给 M6 留下死接口，也不预占 Phase 3 的协议设计。
```

### 8.3 所有权证明（逐条）

| 待证明 | 证明 |
| --- | --- |
| lookup 在 `_mutex` 下完成 | 实现第一行即 `lock_guard(_mutex)`；`find` 与 `shared_ptr` 复制都在临界区内 |
| 不暴露可变注册表 | 返回类型 `shared_ptr<const IMeetingLiveSession>`；该接口只有 const 方法（`DescribeSessionId()`），既不返回容器也不返回引用；调用方无法增删会话、无法发送 |
| adapter 不依赖 `CServer`/`CSession` 具体类型 | 接口头只含 `<memory>`/`<string>`；实现细节封在 `CServer.cpp` |
| 不违反 Meeting Domain 所有权边界 | 目录接口只出现在 adapter 与 `CServer` 之间；`meeting/*` 零 include、零引用 |
| 不声称 `SharedSelf()` 补救悬垂 | 本设计**不依赖**它；生命周期由 `shared_ptr` 复制本身保证。`SharedSelf()` 只在 `async_write` 期间保活（既有语义），与本保障无关 |
| 强引用不会导致"复活"/泄漏 | 句柄只在单个目标的处理期间存活（局部变量），**不**把 Session 重新插入 `_sessions`，也不阻止 `Close()`；被移除后仍存活只是延迟析构，不是语义复活 |
| 不存在"从裸指针造 shared_ptr" | 显式禁止；唯一来源是 `_sessions` 中既有的 `shared_ptr`（§8.6） |
| const 契约自洽 | 见 §8.2.3：接口全 const、句柄 const、无发送方法 ⇒ 无"可定义不可调用"的接口 |

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
| 目录不可用（`weak_ptr` lock 失败） | **整批** no-op（0 条消息 / 0 次 Collect），计 `directory_unavailable` | §7.1.2 步骤 1 / §8.4 |
| 单个目标缺失 | **仅跳过该目标**（其余目标照常处理），计 `target_missing` | §7.1.2 步骤 2a |
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

## 12. Ownership / Lifetime（M6.0.1 重做：所有权拓扑与关闭顺序）

### 12.1 生产 ownership 拓扑（含 collector 归属，修正 B4）

```text
ChatServer.cpp（composition root，main 线程）
  └── auto server = std::make_shared<CServer>(io_context, port)      ← §12.4 改为 shared
        ├── _sessions : std::map<std::string, std::shared_ptr<CSession>>
        ├── 实现 IMeetingSessionDirectory（FindSessionBySessionId，§8.2）
        └── 构造后、io_context.run() 前：
              LogicSystem::GetInstance()->SetMeetingSessionDirectory(server)

LogicSystem（Singleton）—— **全部四个对象都由它拥有**
  ├── _outbound_collector  : unique_ptr<CountingNoWireCollector>        ← M6 新增，**必须**是成员
  ├── _meeting_adapter     : unique_ptr<MeetingResponseNotificationAdapter>   ← M6 新增
  │     ├── IMeetingOutboundMessageCollector&  → *_outbound_collector   （**非拥有引用**）
  │     └── weak_ptr<IMeetingSessionDirectory> → CServer（判活，不拥有）
  ├── _dispatcher          : unique_ptr<LogicWorkDispatcher>            ← 惰性构造
  │     └── IMeetingResponseNotificationAdapter* → *_meeting_adapter    （**非拥有指针**）
  └── _work_executor       : unique_ptr<SerializedWorkExecutor<LogicWorkDispatcher>>
        ├── THandler& handler_          → *_dispatcher                  （**非拥有引用**）
        └── has-a LogicWorkQueue queue_ + std::thread worker_
```

**collector 的归属裁决（B4 的第二个问题）**：

```text
裁决：CountingNoWireCollector 由 LogicSystem 以 unique_ptr 成员**独占拥有**，
      adapter 只持有一个**非拥有引用**。

禁止做法（M6.0 的隐含风险）：
  ✗ 在 EnsureWorkQueueStarted() 内创建局部 CountingNoWireCollector，
    再把引用交给 adapter —— 该局部对象在 call_once 结束时即析构，
    adapter 随后持有的是**悬垂引用**（典型"代码能编译、运行时 UB"）。

为什么必须是成员而不是 adapter 的成员：
  ✓ 若 collector 作为 adapter 的 value 成员，adapter 析构时它会先于 adapter 的
    其它成员/基类销毁次序产生额外约束，且 Phase 3 可能希望把真实 collector
    独立替换/共享 —— 放在 LogicSystem 层使"谁拥有出站汇点"一目了然；
  ✓ 由 LogicSystem 拥有 ⇒ 其生命周期严格覆盖 adapter（§12.2 的声明顺序保证）。

生命周期要求（可静态核对）：
  collector 的生命周期 ⊇ adapter 的生命周期
  ⇒ 声明顺序上 collector 必须**先于** adapter（逆序析构时 adapter 先销毁）。
```

### 12.2 成员声明顺序与逆序析构证明（修正 B2）

#### 12.2.1 M6.0 的错误

M6.0 §12.2 写作：

```text
"member 声明顺序必须为：_meeting_adapter → _work_executor → _dispatcher
 （C++ 逆序析构 ⇒ worker 先停，再销毁 adapter，最后销毁 dispatcher）"
```

该说明**自相矛盾**：若声明为 `_meeting_adapter → _work_executor → _dispatcher`，
则逆序析构是 `_dispatcher → _work_executor → _meeting_adapter`，
即 **dispatcher 先于 executor 被销毁** —— 而 executor 的 `handler_` 引用正指向 dispatcher。
这会把"先停 worker、再拆依赖"的意图**反过来**。

#### 12.2.2 正确约束（从依赖关系推导，而不是从偏好推导）

```text
依赖事实（均已在代码中核实）：
  D1  executor 持有 dispatcher 的引用：THandler& handler_（LogicWorkDispatcher.h:575），
      构造期绑定（LogicWorkDispatcher.h:284-285），并在 DispatchOutsideLock 中
      调用 handler_.DispatchWork(...)（LogicWorkDispatcher.h:555）。
  D2  executor 的 worker 线程体**解引用** handler_ 与 legacy_router_
      （LogicWorkDispatcher.h:551-555），因此 worker 存活期间二者必须存活。
  D3  dispatcher 持有 adapter 的非拥有指针（M6 新增），并在每个 Dispatch* 中调用它。
  D4  adapter 持有 collector 的非拥有引用（§12.1）。
  ⇒ 存活条件：collector ⊇ adapter ⊇ dispatcher ⊇ executor 的 worker
  ⇒ 销毁条件（逆序）：executor(worker 已停) → dispatcher → adapter → collector
```

#### 12.2.3 结论：声明顺序与析构顺序

```cpp
// LogicSystem.h —— 声明顺序（**必须**与此一致）
std::unique_ptr<CountingNoWireCollector>                        _outbound_collector;    // 1
std::unique_ptr<MeetingResponseNotificationAdapter>             _meeting_adapter;       // 2
std::unique_ptr<LogicWorkDispatcher>                            _dispatcher;            // 3
std::unique_ptr<SerializedWorkExecutor<LogicWorkDispatcher>>    _work_executor;         // 4
```

```text
声明顺序 : _outbound_collector → _meeting_adapter → _dispatcher → _work_executor
逆序析构 : _work_executor → _dispatcher → _meeting_adapter → _outbound_collector
                     ↑                ↑              ↑                    ↑
              worker 先被 join    随后销毁        随后销毁            最后销毁
```

**注意**：现有 `LogicSystem.h` 的声明顺序是 `_work_executor` 在前、`_dispatcher` 在后
（`LogicSystem.h:90` 与 `:94`），即逆序析构时 dispatcher 先于 executor 销毁。
当前之所以没有出问题，是因为 `~LogicSystem` **显式**先调用
`_work_executor->StopAndJoin()`（`LogicSystem.cpp:496-504`），
把 worker 在**任何成员析构之前**就 join 掉了。M6.0.1 要求把声明顺序也调整到与
依赖一致，使"即使显式调用被误删"也不至于立刻 UB（防御性，但不替代显式停止）。

#### 12.2.4 显式停止不可被析构顺序替代（硬约束）

```text
⚠️ 声明顺序只解决"析构时不访问已销毁对象"，**不**解决"线程何时停止"。
   worker 的停止必须由**显式调用** StopAndJoin() / ShutdownWorkExecutor() 完成：
     - main 中在 io_context.run() 返回后显式调用（§12.3 的必需路径）；
     - ~LogicSystem 中的兜底调用（既有）。
   二者都不可省略，也不可互相替代。
   理由：若只依赖逆序析构，则 worker 会一直运行到 ~LogicSystem 真正开始析构，
        而那时 CServer 早已销毁 ⇒ §12.3 的时序保证失效。
```

#### 12.2.5 为什么三者必须在同一个 `call_once` 内构造

`LogicWorkDispatcher` 的构造需要 `IMeetingResponseNotificationAdapter*`；
adapter 需要 collector 引用与 weak 目录。四者都在 `EnsureWorkQueueStarted()` 的
`std::call_once` 内一次性装配，保证：

```text
1. 装配顺序（构造，与声明顺序一致，因为初始化列表按声明顺序执行）：
     _outbound_collector → _meeting_adapter → _dispatcher → _work_executor
   ⇒ adapter 构造时 collector 已存在；dispatcher 构造时 adapter 已存在；
     executor 构造时 dispatcher 已存在（且其构造函数会 SetWorkQueue /
     SetInternalWorkSink 并启动 worker 线程）
2. 生命周期：collector ⊇ adapter ⊇ dispatcher ⊇ executor
3. 不需要为 collector / adapter 引入任何锁或引用计数（单线程装配，#12.5 保证可见性）
```

### 12.3 关闭路径（M6.0.2 重做：命名契约 + 全生命周期异常安全）

本节区分三类陈述，互不混淆：

```text
【源码事实】  直接来自当前 HEAD 的代码，可用 grep / 行号核对
【实际风险】  事实在某种执行序列下会产生什么后果（是否会发生，取决于时序）
【设计选择】  为消除风险而做的裁决；理由与代价一并写出
```

#### 12.3.1 【源码事实】对象与线程模型（`ChatServer.cpp`，当前 HEAD）

```cpp
// ChatServer.cpp:20-60（现状原文，局部变量名以实际源码为准）
try {
    auto pool = AsioIOServicePool::GetInstance();                  // shared_ptr<AsioIOServicePool>
    RedisMgr::GetInstance()->HSet(LOGIN_COUNT, server_name, "0");   // Redis 先注册

    std::string server_address(...);
    ChatServiceImpl service;
    grpc::ServerBuilder builder;
    ...
    std::unique_ptr<grpc::Server> server(builder.BuildAndStart());   // ← 变量名就是 server（gRPC）
    std::thread grpc_server_thread([&server]() { server->Wait(); }); // ← 线程在 CServer 之前启动

    boost::asio::io_context io_context;                              // 主线程驱动
    boost::asio::signal_set signals(io_context, SIGINT, SIGTERM);    // ← 绑定主 io_context
    signals.async_wait([&io_context, pool, &server](auto, auto) {
        io_context.stop();
        pool->Stop();                                                // ← 现状在回调里 Stop
        server->Shutdown();                                          // ← gRPC 的 API
    });

    auto port_str = cfg["SelfServer"]["Port"];
    CServer s(io_context, atoi(port_str.c_str()));                   // ← TCP server：**栈对象**
    io_context.run();

    RedisMgr::GetInstance()->HDel(LOGIN_COUNT, server_name);
    RedisMgr::GetInstance()->Close();
    grpc_server_thread.join();
}
catch (std::exception& e) { std::cerr << ...; }
```

由此得到的事实清单：

| # | 【源码事实】 | 证据 |
| --- | --- | --- |
| A1 | gRPC Server 的类型是 `std::unique_ptr<grpc::Server>`，**变量名为 `server`** | `ChatServer.cpp:33` |
| A2 | `server->Shutdown()` 调的是 **gRPC** `Server::Shutdown()`，**不是** TCP `CServer` 的方法 | `ChatServer.cpp:46`；`CServer.h` 只有 构造/析构/`ClearSession` |
| A3 | `CServer` **没有** `Shutdown()` 方法 | `CServer.h:12-15`（公开面仅三项）；全仓无 `CServer::Shutdown` |
| A4 | TCP `CServer` 是**栈对象** `CServer s(...)`，构造在 gRPC 线程启动**之后** | `ChatServer.cpp:50` |
| A5 | `grpc_server_thread` 是**裸 `std::thread`**，在 `CServer` 构造之前就已启动 | `ChatServer.cpp:36-38` / `:50` |
| A6 | `signal_set` 绑定的是**主 `io_context`**，而 `io_context.run()` 在**主线程**执行 | `ChatServer.cpp:41-42,51` |
| A7 | `AsioIOServicePool` 管理的是**另一组** `io_context` 与 `_threads` | `AsioIOServicePool.cpp:11-15` |
| A8 | **信号回调运行在主线程**（A6+A7）：它不会因为"当前线程身份"而去 join pool 的线程 | 由 A6/A7 推出 |
| A9 | `AsioIOServicePool::Stop()` **非幂等** | 见 §12.3.2 |
| A10 | `RedisMgr::Close()` **同样非幂等**（内部 `check_thread_.join()`），且 `~RedisMgr()` **不**调用 `Close()` | `RedisMgr.h:106-110`、`RedisMgr.cpp:14-16` |
| A11 | `HDel` / `CreateSession` 等 Redis 调用**可能失败**（返回 bool），而 `Close()` 的 `join()` **可能抛** `std::system_error` | `RedisMgr.h:106-110` |

> **M6.0.1 的事实更正（B3）**：M6.0.1 §12.3.2 曾声称"信号回调可能运行在
> `AsioIOServicePool` 的线程上，因此 `pool->Stop()` 可能 join 当前线程自身"。
> 按 A6–A8，**该断言不成立**：回调在主线程执行，`pool` 的 `_threads` 是另外的线程，
> 不存在"必然自我 Join"。M6.0.2 **删除**该不准确结论。
> 单一 `Stop()` 调用点的**真实理由**见 §12.3.4（非幂等 + 顺序可证明），
> 与线程身份无关。

#### 12.3.2 【源码事实】`Stop()` 与 `Close()` 都不幂等

```cpp
// AsioIOServicePool.cpp:32-45（GBK 源，此处 UTF-8 视图）
void AsioIOServicePool::Stop() {
        for (auto& work : _works) {
                work->get_executor().context().stop();   // ← 解引用 unique_ptr
                work.reset();                           // ← 置空 ⇒ _works[i] == nullptr
        }
        for (auto& t : _threads) { t.join(); }          // ← 无条件 join
}

// RedisMgr.h:106-110（连接池 Close）
void Close() { b_stop_ = true; cond_.notify_all(); check_thread_.join(); }
```

```text
⇒ Stop() 第二次调用：`work->get_executor()` 解引用**已置空的 unique_ptr** ⇒ UB。
   已用 g++ 探针确认 boost::asio::executor_work_guard **无默认构造函数**，
   因此不存在"置空后仍是合法对象、可安全解引用"的退路。
⇒ Stop() 即便绕过第一循环，`t.join()` 对已 join 的 thread 会抛 std::system_error。
⇒ Close()（Redis）同理：第二次 `check_thread_.join()` 抛 std::system_error；
   且 ~RedisMgr() 是空实现，**不会**代为清理 ⇒ 泄漏或抛异常由调用方承担。
⇒ 结论：`Stop()` 与 `Close()` 都必须**恰好调用一次**，二者都不是幂等的。
```

#### 12.3.3 【实际风险】三条真实存在的风险（与线程身份无关）

```text
R-11a  重复调用风险：若信号回调与清理路径各调一次 Stop()，第二次即 UB（§12.3.2）。
R-12   异常跳过风险：若清理写成 run() 之后的顺序语句，run() 抛异常会跳过后续全部
       （含 Redis 关闭与 grpc_server_thread.join()）⇒ 见下。
R-12b  裸 std::thread 析构风险（**比 R-12 更早**）：
       grpc_server_thread 在 CServer 构造之前启动（A5）。若在其构造之后、join 之前
       抛出任何异常（CServer 构造失败 / 目录注册失败 / run() 抛异常），
       栈展开时该 std::thread 仍 joinable ⇒ 析构调用 std::terminate()。
       当前代码把 join 放在 try 尾部的顺序语句里，因此**这条路径确实不安全**。
R-12c  Redis 跳过风险：Close() 在 try 尾部；异常路径上不会执行，而 ~RedisMgr 不清理（A10）。
```

#### 12.3.4 【设计选择】唯一、受控的 Stop 调用点（保留 M6.0.1 的裁决）

```text
裁决（不变）：AsioIOServicePool::Stop() **只有一个调用点**，且位于受控清理路径中；
              信号回调不再调用它。

理由（更正后的正确版本）：
  1. Stop() 非幂等（§12.3.2）⇒ 多处调用必然 UB，必须从结构上排除；
  2. 关闭顺序需要可证明（生产者 → executor drain → 解引用目录 → 析构），
     这只能在单一线性的清理序列里成立，不能分散到信号回调里；
  3. 信号回调的职责应当最小化：只负责**唤醒** run() 返回（io_context.stop()），
     把有序清理留给主线程的清理路径。
  ✗ 不再主张"信号回调会自我 Join"——该理由已被 A6–A8 否定。

为什么不采用方案 B（把 Stop() 幂等化）：
  ✗ 需要修改 GBK 编码的 AsioIOServicePool.h/.cpp（转码/字节级编辑风险）；
  ✗ 需要新增停止状态位并给 _works / _threads 加状态机；
  ✗ 幂等化只掩盖"多处调用"，而单一调用点从结构上消除它。
```

#### 12.3.5 【设计选择】关闭协调器与**失败策略分级**（M6.0.3 重做）

**两个核心要求**：

```text
(B2/旧) 清理范围必须从第一个可 join 的线程启动那一刻起有效 —— 用协调器 + 按值持有线程解决。
(B1/新) **不是所有清理步骤都同等重要**：关键生命周期步骤失败后，绝不能"记录日志然后继续"，
        否则会在一堆仍未 Join 的线程仍在运行时，去销毁它们可能访问的对象。
```

##### 12.3.5.1 两步分级（本节的裁决）

| 级别 | 步骤 | 失败后果 | 策略 |
| --- | --- | --- | --- |
| **K — 关键生命周期前置条件** | 1a `grpc_server->Shutdown()`<br>1b `pool->Stop()`<br>2 `ShutdownWorkExecutor()`<br>4b `redis->Close()`<br>5 `grpc_thread_.join()` | 失败意味着**仍可能有线程在运行**，或留下**joinable `std::thread`** | **fail-stop**：记录致命错误后立即终止进程；**不继续**后续步骤；**不重试**（非幂等） |
| **B — best-effort** | 3 `ClearMeetingSessionDirectory()`<br>4a `redis->HDel(...)` | 失败不产生生命周期错误（前者 noexcept；后者只返回 bool） | 记录并继续 |

**为什么恰好是这五步属于 K**：

```text
1a  失败 ⇒ 同步 RPC handler 可能仍在执行 ⇒ 它们仍会访问 Redis（§12.3.14）
1b  失败 ⇒ pool 的 io_context 线程可能仍 joinable/运行 ⇒ 仍可能触达 CSession/CServer
2   失败 ⇒ executor 的 worker_ 可能仍 joinable/运行 ⇒ 仍会访问 MeetingService 与 Redis
4b  失败 ⇒ Redis 的 check_thread_ 仍 joinable ⇒ 之后静态析构期 std::terminate
5   失败 ⇒ grpc_server_thread 仍 joinable ⇒ 协调器成员析构即 std::terminate
⇒ 这五步中任何一步失败，"所有线程已 Join / 依赖已可安全析构"这一前提**就不成立**，
  因此不能继续走到对象析构阶段。
```

**fail-stop 的诚实声明（不得含糊）**：

```text
发生 fail-stop 时：
  ✗ 不能保证 Redis 清理完成（4a/4b 可能尚未执行或已失败）
  ✗ 不能再声称这是一次"优雅关闭"
  ✗ 不能声称所有线程都已 Join —— 恰恰是"没能 Join"才触发它
  ✓ 唯一的正确声明是：检测到不可恢复的关闭失败，为避免在未停止的线程之上析构对象
   （那会引发 UB 或 std::terminate），进程主动终止。
```

##### 12.3.5.2 协调器实现（B2 采用**方案 B：协调器持有强引用**）

```cpp
// 目标形态（示意；Coding 时以实际为准。**不新增**线程/timer/依赖）
namespace {

class ShutdownCoordinator {
public:
    // 构造时机：grpc_server / io_context / signals 已构造，**先于** tcp_server 构造。
    ShutdownCoordinator(grpc::Server* grpc_server,
                        std::shared_ptr<AsioIOServicePool> pool,
                        std::shared_ptr<RedisMgr> redis,
                        LogicSystem* logic,
                        const std::string& server_name)
        : grpc_server_(grpc_server),
          grpc_thread_(),                    // 默认构造：非 joinable，可安全析构
          pool_(pool), redis_(redis), logic_(logic),
          tcp_server_(),                     // 强引用，由 SetTcpServer 填入
          server_name_(server_name), done_(false) {}

    // 启动 gRPC 线程。若此处抛 std::system_error，线程仍非 joinable ⇒ 无 terminate 风险。
    void StartGrpcThread() /* 可抛 */ {
        grpc_thread_ = std::thread([this]() { grpc_server_->Wait(); });
    }

    // **方案 B**：接收 shared_ptr 并**持有强引用**。
    // 效果：tcp_server 的生命周期 ⊇ 协调器生命周期 ⇒ 协调器析构兜底执行 Cleanup() 时，
    //       Session Directory 依然有效（无需"降级为 no-op"）。
    void SetTcpServer(std::shared_ptr<CServer> tcp_server) { tcp_server_ = tcp_server; }

    ~ShutdownCoordinator() { Cleanup(); }

    // 幂等（done_ 一次闸门）。可显式调用；析构时会再次调用但为 no-op。
    void Cleanup() /* noexcept */ {
        if (done_) { return; }
        // 先置位：防止重入，也保证非幂等操作（Stop/Close）绝不因重复调用而二次执行。
        // 注意：done_ **不**用于"掩盖失败" —— 关键步骤失败会直接 fail-stop，不会走到第二次。
        done_ = true;

        // ================= K 级：任何失败都 fail-stop ==========================
        Fatal("1a grpc_server->Shutdown",
              [&]{ if (grpc_server_ != nullptr) { grpc_server_->Shutdown(); } });

        Fatal("1b pool->Stop",
              [&]{ if (pool_) { pool_->Stop(); } });

        Fatal("2 ShutdownWorkExecutor",
              [&]{ if (logic_ != nullptr) { logic_->ShutdownWorkExecutor(); } });

        // ================= B 级：best-effort，失败只记录 =======================
        BestEffort("3 ClearMeetingSessionDirectory",
                   [&]{ if (logic_ != nullptr) { logic_->ClearMeetingSessionDirectory(); } });

        BestEffort("4a redis HDel",
                   [&]{ if (redis_) { redis_->HDel(LOGIN_COUNT, server_name_); } });

        // ================= 回到 K 级 ===========================================
        Fatal("4b redis Close",
              [&]{ if (redis_) { redis_->Close(); } });

        Fatal("5 grpc_thread join",
              [&]{ if (grpc_thread_.joinable()) { grpc_thread_.join(); } });

        // 走到这里 ⇒ 五步 K 全部成功 ⇒ 可以安全地让成员按逆序析构。
    }

private:
    // K 级：失败 ⇒ 记录致命错误并 fail-stop。**绝不重试**（Stop/Close 非幂等）。
    template <typename Fn>
    void Fatal(const char* name, Fn fn) /* noexcept */ {
        try {
            fn();
        }
        catch (const std::exception& e) {
            FailStop(name, e.what());
        }
        catch (...) {
            FailStop(name, "unknown exception");
        }
    }

    // B 级：失败 ⇒ 记录并继续。
    template <typename Fn>
    void BestEffort(const char* name, Fn fn) /* noexcept */ {
        try {
            fn();
        }
        catch (const std::exception& e) {
            std::cerr << "shutdown best-effort step failed: " << name
                      << ": " << e.what() << std::endl;
        }
        catch (...) {
            std::cerr << "shutdown best-effort step failed: " << name
                      << ": unknown" << std::endl;
        }
    }

    // fail-stop：不能 throw（会从析构函数逸出 ⇒ terminate，且丢失诊断）。
    // 显式 abort 比"继续析构"更安全：继续析构会遇到仍未 Join 的线程 ⇒ UB/terminate。
    void FailStop(const char* name, const char* what) /* noexcept */ {
        std::cerr << "FATAL: unrecoverable shutdown failure at step [" << name
                  << "]: " << what << std::endl;
        std::cerr << "FATAL: cannot guarantee that all threads are joined; "
                     "terminating to avoid destroying objects still in use."
                  << std::endl;
        std::cerr << "FATAL: Redis cleanup may be incomplete; this is NOT a graceful shutdown."
                  << std::endl;
        std::abort();
    }

    grpc::Server*                       grpc_server_;  // 不拥有（main 的 unique_ptr 拥有）
    std::thread                         grpc_thread_;  // **按值持有**
    std::shared_ptr<AsioIOServicePool>  pool_;
    std::shared_ptr<RedisMgr>           redis_;
    LogicSystem*                        logic_;
    std::shared_ptr<CServer>            tcp_server_;   // **强引用（方案 B）**
    std::string                         server_name_;
    bool                                done_;
};

} // namespace
```

#### 12.3.6 目标形态（与现有源码命名一致，消除 `server` 重定义）

**声明顺序（决定析构顺序）**：

```text
声明 : grpc_server → io_context/signals → shutdown(协调器) → tcp_server
析构 : tcp_server(main 的 shared_ptr, 2→1) → shutdown(成员 tcp_server_ 1→0, CServer 析构)
                                              → signals/io_context → grpc_server
```

```cpp
// ChatServer.cpp —— M6 目标形态（示意；Coding 时以实际为准）
int main()
{
    auto& cfg = ConfigMgr::Inst();
    auto server_name = cfg["SelfServer"]["Name"];
    try {
        auto pool  = AsioIOServicePool::GetInstance();
        auto redis = RedisMgr::GetInstance();
        redis->HSet(LOGIN_COUNT, server_name, "0");

        // ---- gRPC server：grpc_server（不与 tcp_server 重名）----
        ChatServiceImpl service;
        grpc::ServerBuilder builder;
        builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
        builder.RegisterService(&service);
        std::unique_ptr<grpc::Server> grpc_server(builder.BuildAndStart());
        // BuildAndStart() 可能返回 nullptr：既有缺陷，M6 不扩大范围修正（R-17）

        boost::asio::io_context io_context;                    // 必须先于 tcp_server
        boost::asio::signal_set signals(io_context, SIGINT, SIGTERM);

        // ---- 协调器：**早于** tcp_server ⇒ 覆盖 tcp_server 构造/注册的异常路径 ----
        ShutdownCoordinator shutdown(grpc_server.get(), pool, redis,
                                     LogicSystem::GetInstance().get(), server_name);

        // 信号回调只做"唤醒"：不调 pool->Stop / ShutdownWorkExecutor / gRPC Shutdown
        signals.async_wait([&io_context](auto, auto) { io_context.stop(); });

        // ---- TCP server：tcp_server，shared_ptr（weak 目录需要控制块）----
        auto tcp_server = std::make_shared<CServer>(io_context, atoi(port_str.c_str()));
        shutdown.SetTcpServer(tcp_server);      // 方案 B：协调器持**强引用**

        // 目录注册：tcp_server 已存在且 run() 未开始 ⇒ 无 handler 在跑
        LogicSystem::GetInstance()->SetMeetingSessionDirectory(tcp_server);

        // gRPC 线程在所有可能失败的初始化之后启动
        shutdown.StartGrpcThread();

        // run + 清理：无论 run() 正常返回还是抛异常，Cleanup 都会执行，
        // 且此时 main 的 tcp_server 仍在作用域内。
        try {
            io_context.run();
        }
        catch (...) {
            shutdown.Cleanup();   // 幂等
            throw;
        }
        shutdown.Cleanup();
    }                             // ← tcp_server(main) 先析构，再 shutdown
    catch (const std::exception& e) {
        std::cerr << "Exception: " << e.what() << std::endl;
    }
    return 0;
}
```

```text
命名契约（B1）：
  grpc_server        : unique_ptr<grpc::Server>；关闭 = grpc_server->Shutdown()（步骤 1a）
  tcp_server         : shared_ptr<CServer>；**没有** Shutdown()；关闭 = 停 io_context/pool、
                       清 _sessions 后析构
  grpc_server_thread : 协调器**按值持有**的 std::thread，运行 grpc_server->Wait()；步骤 5 join
  io_context         : 主线程 run()
  pool               : AsioIOServicePool 单例
禁止：把 TCP server 命名为 server；对 CServer 调 Shutdown()；虚构 CServer::Shutdown()
```

#### 12.3.7 四条关闭路径的差异（含失败策略）

| 情形 | `run()` | gRPC 线程已启动 | 清理由谁执行 | K 级失败时 |
| --- | --- | --- | --- | --- |
| 正常 | 返回 | 是 | `run()` 后**显式** `Cleanup()` | fail-stop |
| 信号 | `stop()` ⇒ 返回 | 是 | 同上 | fail-stop |
| **`run()` 抛异常** | 否 | 是 | 内层 `catch(...)` **先显式** `Cleanup()` 再 `throw` | fail-stop |
| **`tcp_server` 构造 / 目录注册抛异常** | 未开始 | **否** | 协调器析构兜底 `Cleanup()` | fail-stop（若 1b/4b 失败） |

**三条路径都会显式调用 `Cleanup()`**（不依赖析构兜底）；析构兜底只覆盖
"显式调用之前就抛异常"的路径，此时 `done_` 为 false，兜底执行真实清理。

#### 12.3.8 顺序与各项保证（逐条回答 12 问）

| 问题 | 答案 | 依据 |
| --- | --- | --- |
| ① `grpc_server` / `tcp_server` 构造、拥有、析构 | `grpc_server`：`unique_ptr`，main 作用域；`tcp_server`：`shared_ptr`，**main 与协调器共同强引用**（方案 B）；`tcp_server_` 1→0 时 CServer 析构，发生在 `io_context` 析构之前 | §12.3.6 |
| ② `grpc_server_thread` 启停/join | `StartGrpcThread()` 启动（在所有可失败初始化之后）；协调器**按值**持有；步骤 5 join | §12.3.5.2 |
| ③ 协调器何时建立、如何覆盖已启动线程 | 先于 `tcp_server` 构造；gRPC 线程由它创建并按值归其所有 ⇒ 线程全生命周期在协调器作用域内 | §12.3.6 |
| ④ 四条路径差异 | 三条显式 `Cleanup()`；一条析构兜底。K 级失败一律 fail-stop | §12.3.7 |
| ⑤ gRPC 与网络生产者何时停 | 步骤 1a / 1b，均早于 drain | §12.3.5.2 |
| ⑥ `pool->Stop()` 为何恰好一次 | 唯一调用点在 `Cleanup()`；`done_` 闸门；信号回调不调用；**失败不重试而是 fail-stop** | §12.3.5.1 |
| ⑦ `ShutdownWorkExecutor()` 为何在所有生产者之后 | 步骤 2 严格晚于 1a/1b；此时无生产者 | §12.3.10 |
| ⑧ `Close → FINALIZE_CLOSE` drain 为何能完成 | `StopAndJoin` 先 `RequestStop` 再排空；内部入队通道只对消费者开放 | §12.3.10 |
| ⑨ `ClearMeetingSessionDirectory()` 为何在 drain 之后 | 步骤 3 晚于步骤 2 | §12.3.5.2 |
| ⑩ `tcp_server` 与 drain 的关系 | **方案 B 下**：协调器持强引用 ⇒ 即使走析构兜底，`tcp_server` 在 `Cleanup()` 体内仍然存活 ⇒ **drain 期间目录始终有效**，无需降级 | §12.3.8.1 |
| ⑪ Redis 清理如何覆盖异常路径 | 步骤 4a/4b 在 `Cleanup()` 内；4a best-effort、4b 关键（fail-stop） | §12.3.5.1 / §12.3.14 |
| ⑫ 所有 `std::thread` 析构前为何不再 joinable | gRPC 线程 ← 步骤 5；pool `_threads` ← 1b；Redis `check_thread_` ← 4b；executor `worker_` ← 2。四者皆 K 级：失败即 fail-stop，不会继续到对象析构 | §12.3.9 |

#### 12.3.8.1 方案 B 的所有权与析构证明（B2 裁决）

```text
采用方案 B：协调器成员为 std::shared_ptr<CServer> tcp_server_（强引用）。

强引用关系：
  main 的局部 shared_ptr  ──┐
                            ├─→ CServer 对象（引用计数 ≥ 1 直到两者都释放）
  协调器成员 tcp_server_ ───┘
  LogicSystem 只持 weak_ptr<IMeetingSessionDirectory>（不参与计数）

正常路径析构顺序：
  run() 返回 → 显式 Cleanup()（tcp_server 引用计数 ≥2，对象存活 ⇒ 目录有效）
  → 离开 try 作用域 → main 的 tcp_server 释放（计数 ≥2→1，对象仍存活）
  → 协调器析构 → Cleanup() 为 no-op → 成员析构：tcp_server_ 释放（1→0 ⇒ **CServer 在此析构**）
  → signals/io_context 析构（CServer 已不在，且它持 io_context& ⇒ 顺序正确）

run() 抛异常路径析构顺序：
  内层 catch → 显式 Cleanup()（此时 tcp_server 两者均存活 ⇒ 目录有效 ⇒ drain 正常）
  → rethrow → 外层 catch
  → 离开 try → main 的 tcp_server 释放（→1）
  → 协调器析构 → Cleanup() no-op → tcp_server_ 释放（1→0 ⇒ CServer 析构）
  ⇒ 与正常路径**相同**，目录在 drain 期间始终有效

tcp_server 构造失败：tcp_server_ 为空；无目录注册、无 worker 启动；Cleanup 的 2/3 步骤无害
目录注册失败：tcp_server_ 已填 ⇒ 强引用有效；Cleanup 时目录可达

协调器析构兜底期间 Session Directory 是否有效：**有效**（强引用保证），
  因此兜底路径**有权且能够**执行 drain。这与 M6.0.2 的"降级 no-op"说法不同——
  M6.0.3 用方案 B **消除**了那个降级。
```

#### 12.3.9 每步的失败策略（不依赖"析构永不失败"）

| 步骤 | 级别 | 可能抛？ | 失败后 |
| --- | --- | --- | --- |
| 1a `grpc_server->Shutdown()` | **K** | 按 gRPC 文档不抛（§12.3.14）；若抛则视为致命 | fail-stop（handler 可能仍在跑） |
| 1b `pool->Stop()` | **K** | 是（`join` → `std::system_error`） | fail-stop（可能有 io_context 线程仍存活） |
| 2 `ShutdownWorkExecutor()` | **K** | 是（`worker_.join()`） | fail-stop（worker 可能仍访问 MeetingService/Redis） |
| 3 `ClearMeetingSessionDirectory()` | B | 否（`weak_ptr` 赋值 noexcept） | 记录并继续 |
| 4a `redis->HDel(...)` | B | 否（返回 bool） | 记录并继续 |
| 4b `redis->Close()` | **K** | 是（`check_thread_.join()`） | fail-stop（否则静态析构期 terminate） |
| 5 `grpc_thread_.join()` | **K** | 是（`std::system_error`） | fail-stop（成员析构期 terminate） |

```text
为什么"捕获并继续"不是充分证明（B1 的要点）：
  若 1b 的 join 失败而继续执行 2/3，就会出现"pool 线程仍在运行，而 Session Directory
  已被解除、tcp_server 即将析构"——那正是 UB。因此 K 级失败必须 fail-stop，
  而不是靠 try/catch 让流程继续。

fail-stop 的代价（如实）：Redis 清理可能未完成；不宣称优雅关闭；不宣称全部线程已 Join。
```

#### 12.3.10 drain 完成性与"生产者已停"的证据

```text
(a) 1b 成功返回 ⇒ pool 全部 io_context 已 stop 且 _threads 已 join
    ⇒ HandleAccept / 读回调 / ClearSession / ~CSession 不再执行
(b) 2 的 StopAndJoin（LogicWorkDispatcher.h:409-416）：RequestStop（幂等）+ join
    stopping_ 后外部 Push 返回 false；消费者排空队列后才 break
    ⇒ 已受理的 Close 及其派生的 FINALIZE_CLOSE 被执行完
(c) PushInternalDuringDrainLocked 只对消费者线程开放（M4 §11.2）
    ⇒ drain 期间追加的 FINALIZE_CLOSE 不依赖任何外部生产者
(d) 方案 B 保证：Cleanup() 体内 tcp_server 强引用存活 ⇒ 目录 lookup 可用（§12.3.8.1）
```

#### 12.3.11 重复调用行为

```text
pool->Stop()                          : 不可重复（非幂等）。唯一调用点 + done_；失败 fail-stop、不重试
RedisMgr::Close()                     : 不可重复（非幂等）。同上
SerializedWorkExecutor::StopAndJoin() : 可重复（RequestStop 幂等；joinable 守卫；非本线程守卫）
ShutdownCoordinator::Cleanup()        : 可重复（done_ 闸门；第二次为空操作）
ClearMeetingSessionDirectory()        : 幂等
SetMeetingSessionDirectory()          : 一次性
```

```text
done_ 的语义（B1 第 4 项）：
  done_ 只用于"防止重复执行"，**不**用于掩盖失败。
  K 级步骤失败会 fail-stop（进程终止），因此不存在"失败后靠 done_ 让第二次调用看起来成功"的路径。
```

#### 12.3.12 协调器构造之前失败的显式处理

```text
B-a  BuildAndStart() 返回 nullptr ⇒ 既有缺陷，M6 不修正（R-17）；不声称已解决
B-b  std::thread 构造抛 ⇒ 线程非 joinable ⇒ 无 terminate 风险
B-c  Redis HSet 抛 ⇒ 无线程、无协调器；既有行为不变
⇒ 协调器覆盖的起点是"gRPC 线程启动"，而本设计的启动时机已在所有可失败初始化之后。
```

#### 12.3.13 已知行为差异（如实记录）

```text
D-1 gRPC 端口开始服务时间推迟到 tcp_server 构造之后（毫秒级）
    理由：消除"裸 thread still-joinable ⇒ terminate"的结构性代价
D-2 信号回调不再直接 Shutdown gRPC（改由协调器步骤 1a）
D-3 Redis 关闭仍先于 grpc_thread_.join()
    但**不再声称**它与 gRPC handler 并发（见 §12.3.14：1a 已阻塞到 handler 完成）
```

#### 12.3.14 R-18 的重新裁决（B3）：删除"handler 并发访问 Redis"的断言

**证据 1：gRPC 1.51.1 `Shutdown()`（无 deadline）阻塞到同步 handler 完成。**

```text
来源：本机 /usr/include/grpcpp/impl/codegen/server_interface.h
      （libgrpc++-dev 1.51.1-4.1build5），第 60–107 行。

原文（逐字）：
  "2. Block until all rpc method handlers invoked automatically by the sync
      API finish."

  "/// Shutdown the server without a deadline and forced cancellation."
  void Shutdown() {
    ShutdownInternal(g_core_codegen_interface->gpr_inf_future(GPR_CLOCK_MONOTONIC));
  }
```

**证据 2：本仓库的 gRPC 服务是同步实现。**

```text
ChatServiceImpl : public ChatService::Service，覆写的是接收 ServerContext* 的
同步签名（ChatServiceImpl.h:30-37），无 CompletionQueue / 无 async API 使用
⇒ 属于上文的 "sync API" ⇒ step 1a 返回时 handler 均已结束
```

**证据 3：关闭阶段 Redis 的使用者只有两类，且都在 4b 之前停止。**

| Redis 使用者 | 所在线程 | 何时停止 |
| --- | --- | --- |
| `ChatServiceImpl` handler（`ChatServiceImpl.cpp:124,161` Redis Get/Set） | gRPC 同步 handler 线程 | **步骤 1a**（阻塞到 handler 完成） |
| `LogicSystem` 的 legacy handlers（`LogicSystem.cpp` 内 13 处 `RedisMgr::GetInstance()`） | 唯一 executor worker | **步骤 2** |
| `RedisMgr` 自己的 `check_thread_` | pool 内部 | **步骤 4b**（`Close()` 自行 join） |
| `CServer` / `CSession` | pool 线程 | 不使用 Redis（`grep` 计数 0），且已由 1b 停止 |
| `grpc_server_thread`（`Wait()`） | 独立线程 | 只阻塞在 `Wait()`，**不**访问 Redis；步骤 5 join |

**裁决**：

```text
✗ 删除 R-18 的断言"Redis 关闭可能与 gRPC handler 并发访问" —— 在 1a 之后不成立。
✓ 保留的**真实**风险（改写为 R-18'）：

   R-18'a 【顺序不变量】Redis 关闭必须晚于 1a 与 2。若将来有人把 4a/4b 前移，
           就会出现 use-after-close。这是必须由 Review 守护的顺序约束，不是当前缺陷。
   R-18'b 【活性风险】无 deadline 的 Shutdown() 会**无限期**等待未返回的同步 handler
           （例如 handler 卡在 MySQL 调用）。后果是关闭挂起，不是数据竞争。
           本设计不使用 deadline，因为强制取消会破坏 handler 状态；如需上限，
           必须作为独立裁决引入（M6 不做）。
```

```text
另外澄清一个常见误判：grpc_server_thread.join() 位于 Redis 关闭**之后**，
并不代表 handler 还在运行。Server::Wait() 与 handler 是不同生命周期：
Wait() 只是等待关闭完成，handler 由 Shutdown() 负责等待其结束。
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
                                                         装配（顺序 == 声明顺序，§12.2.3）：
                                                           new CountingNoWireCollector
                                                           new MeetingResponseNotificationAdapter(collector, weak 目录)
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
| `shared_ptr<const IMeetingLiveSession>` | `CServer::_sessions` | adapter（**仅在单个目标的处理期间**） | 从注册表复制 ⇒ 处理期间目标必然存活；逐目标局部变量，该目标处理完即释放 |
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
| `MeetingOutboundChannel.h`（**新增**） | `IMeetingLiveSession`（const 存活标记）、`IMeetingSessionDirectory`、`IMeetingOutboundMessageCollector`（仅 `<memory>`/`<string>`） | §7.3 / §8.2 |
| `MeetingRequestContextBuilder.h`（**新增**） | builder 声明 + inline 实现（纯函数） | §14 |
| `MeetingResponseNotificationAdapter.h`（**新增**） | `IMeetingResponseNotificationAdapter` + 生产 adapter 声明 + 只读计数 | §6.4 / §8.4 |
| `MeetingResponseNotificationAdapter.cpp`（**新增**） | 编码、安全矩阵、weak 判活、best-effort、noexcept 边界、计数 | §6 / §7 / §9 / §10 / §13 |
| `CountingNoWireCollector.h/.cpp`（**新增**，或并入 adapter TU） | 生产 collector：只计数、绝不发送 | §7.3 / §7.5 |
| `LogicWorkDispatcher.h` | 构造参数增加 adapter 指针；`Dispatch*` 内增加 adapter 调用点 | §6.5 |
| `LogicWorkDispatcher.cpp` | 六个 `Dispatch*` + `FINALIZE_CLOSE` 分支的 adapter 调用；保持 sink 顺序与 FINALIZE_CLOSE 入队顺序不变 | §6.5 / §11.2 |
| `LogicSystem.h` / `.cpp` | adapter 成员与一次性装配；`SetMeetingSessionDirectory` / `ShutdownWorkExecutor` / `ClearMeetingSessionDirectory` | §12.1 / §12.3 / §12.5 |
| `CServer.h` / `.cpp` | 实现 `IMeetingSessionDirectory`；新增 `FindSessionBySessionId`（`_mutex` 下 find，复制 shared_ptr） | §8.2 / §8.5 |
| `CSession.h` / `.cpp` | 声明实现 `IMeetingLiveSession`（**仅一个 const 方法**：`DescribeSessionId()`；不改变任何发送语义） | §8.2 |
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
| `AsioIOServicePool.*` | **M6.0.1/M6.0.2 裁决：不需要修改**。方案 A 用"单一调用点 + 协调器"消除重复调用，因此 `Stop()` 保持非幂等也安全 | 若 Coding 时发现必须改，则须**先停**并单独裁决（该文件为 GBK，改动有转码风险，且会把 B1 从"设计规避"变成"语义变更"） |

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
| SL7 | **const 契约自洽** | 编译期：`shared_ptr<const IMeetingLiveSession>` 上只调用 const 成员；adapter 内不存在任何发送调用（静态检查 §8.2.3） |

**组 3：notification target disappearing（Layer C，§25.7 明确要求）**

| # | 用例 | 断言要点 |
| --- | --- | --- |
| ND1 | **N 个目标全部存在** ⇒ **恰好 N 条通知** | `target_resolved == N`，`target_missing == 0`，`outbound_produced == N`；且**每条**消息的 `notification_target_session_id` 互不相同、与输入集合逐一对应（不复用） |
| ND2 | **M 个目标全部消失** ⇒ 0 条通知 | `target_resolved == 0`，`target_missing == N`，`outbound_produced == 0`；Collect 调用 0 次；无异常 |
| ND3 | 部分消失 ⇒ 只对存活目标产出 | 守恒 `resolved + missing == N`；`outbound_produced == resolved`；**每条存活目标各 1 条**且目标字段正确；缺失目标 0 条 |
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
  FakeLiveSession           : IMeetingLiveSession（新增，证明句柄生命周期与 const 契约）

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
| **M6.3** | `CServer::FindSessionBySessionId`（`_mutex` 下 find + 复制 shared_ptr）；`CSession` 声明实现 `IMeetingLiveSession`；组 2 用例 | 两个既有文件最小改动 | 回退 `CServer.*`/`CSession.*` |
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
      不改 ClearSession 既有行为；不改 Send 既有语义；
      **IMeetingLiveSession 全 const**（§8.2.3）
M6.4：安全矩阵逐格核对；L0–L5 边界如实；无 retry/ACK；异常不外逸；
      计数不被用于业务判定；生产 collector 不调用 Send；
      **逐目标生成 + 每条 Collect 恰好一次**（§7.1.2 / §7.1.3）
M6.5：O1–O5 顺序证明；FINALIZE_CLOSE 仍不插队；sink 观测顺序不变
M6.6：§12.3 关闭路径（**pool->Stop() 恰好一次** + ShutdownCoordinator 全生命周期覆盖 +
      信号回调只做 io_context.stop() + 每步独立 try/catch）；
      §12.5 注册时序三保证；§12.2.3 成员声明顺序；
      §12.1 collector 由 LogicSystem 拥有（非局部）；make_shared 的理由与影响
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

### R-11：重复调用 `AsioIOServicePool::Stop()`（M6.0.1 新增，**已由设计规避**）

```text
风险：Stop() 非幂等 —— 第二次调用会解引用已置空的 unique_ptr（UB），
      或对已 join 的 thread 再次 join（std::system_error）
影响：高（关闭路径崩溃/未定义行为，且只在进程退出时出现，回归测试易漏）
缓解：方案 A：唯一调用点置于 ShutdownCoordinator::Cleanup()；`done_` 闸门保证只执行一次；
      信号回调不再调用 pool->Stop()。
      验收 A16 / B15；smoke 中确认只有一次 Stop。
```

### R-12：`run()` 异常退出导致清理被跳过（M6.0.1 新增）

```text
风险：若只用"run() 之后的顺序代码"做清理，run() 抛异常会跳过 drain
影响：高（worker 未 join、CServer 先析构 ⇒ 悬垂）
缓解：协调器在 gRPC 线程启动后即建立，栈展开时析构并清理；tcp_server 由协调器
      持 shared_ptr，未到 drain 结束不会析构（§12.3.5/§12.3.6）；
      验收 B16。
```

### R-13：collector 生命周期短于 adapter（M6.0.1 新增）

```text
风险：把 CountingNoWireCollector 建成 EnsureWorkQueueStarted() 的局部对象，
      adapter 随后持有悬垂引用
影响：高（能编译、运行时 UB）
缓解：§12.1 规定 collector 由 LogicSystem 以 unique_ptr 成员独占拥有；
      声明顺序 collector 先于 adapter（§12.2.3）；验收 A18。
```

### R-14：声明顺序与依赖不一致（M6.0.1 新增，既有代码即如此）

```text
事实：现 LogicSystem.h 中 _work_executor（:90）声明在 _dispatcher（:94）之前，
      逆序析构时 dispatcher 先于 executor 销毁。当前靠 ~LogicSystem 显式 StopAndJoin 兜住。
风险：若显式停止被误删，就会在 worker 仍运行时销毁 dispatcher（UB）
缓解：§12.2.3 要求调整声明顺序为 collector → adapter → dispatcher → executor；
      §12.2.4 明确"声明顺序不替代显式停止"；验收 A17。
```

### R-16：`pool->Stop()` 的 join 若失败，pool 的 `_threads` 仍可能 joinable（既有性质）

```text
事实：AsioIOServicePool 没有 join 失败的补救路径；若 Stop() 内的 join 抛异常，
      该 thread 仍 joinable，之后 _threads 析构会 std::terminate。
性质：这是**既有代码**的性质，M6 不通过修改它来"证明安全"。
M6 的贡献：Step() 捕获并记录失败，然后继续完成其余清理，
           使失败不被静默跳过；本残余风险如实登记，不声称已消除。
```

### R-17：`BuildAndStart()` 可能返回空指针（既有，M6 不扩大范围修正）

```text
事实：现状 `std::unique_ptr<grpc::Server> server(builder.BuildAndStart());`
      之后立即 `server->Wait()`；若返回 nullptr 则解引用空指针。
性质：既有缺陷，与 M6 无关；M6 的协调器只接受 `grpc::Server*`，
      因此**不会**改变该行为。
处置：如实登记；若 Coding 时决定顺手加 null 检查，须作为**授权范围内的极小改动**
      单独记录（不得借机扩大 M6 范围）。
```

### R-18：Redis 关闭的并发性 —— **已重新裁决**（M6.0.3 / B3）

```text
【原先的错误断言，已删除】
  旧 R-18 称"Redis 关闭可能与 gRPC handler 并发访问"。
  该断言在步骤 1a 存在的前提下**不成立**：
    gRPC 1.51.1 的 Shutdown()（无 deadline）文档明确写
      "Block until all rpc method handlers invoked automatically by the sync API finish."
    （来源：/usr/include/grpcpp/impl/codegen/server_interface.h，libgrpc++-dev 1.51.1-4.1build5）
    而本仓库 ChatServiceImpl 是同步实现（覆写接收 ServerContext* 的同步签名，
    无 CompletionQueue / 无 async）
  ⇒ 步骤 1a 返回时 handler 均已结束 ⇒ 4b 关闭 Redis 时不存在 gRPC handler 在访问它。

【保留的真实风险 R-18'a：顺序不变量（必须由 Review 守护）】
  Redis 的 4a/4b 必须晚于 1a（gRPC handler 停止）与 2（executor worker 停止，
  因为 LogicSystem 的 legacy handler 也使用 Redis，共 13 处）。
  若将来有人把 Redis 清理前移，就会出现 use-after-close。
  ⇒ 这是**顺序约束**，不是当前缺陷；A16/B20 提供静态检查。

【保留的真实风险 R-18'b：活性（shutdown 可能挂起）】
  无 deadline 的 Shutdown() 会无限期等待未返回的同步 handler
  （例如 handler 卡在 MySQL 调用上）。后果是**关闭挂起**，不是数据竞争。
  本设计刻意不使用 deadline：强制取消会让 handler 处于状态不完整的位置
  （gRPC 文档亦指出流式 handler 不会被强制终止）。
  若需要关闭超时上限，属独立裁决，M6 不引入。

【澄清一个易误判点】
  grpc_server_thread.join() 在 Redis 关闭**之后**，并不代表 handler 仍在运行：
  Server::Wait() 与 handler 是不同生命周期 —— Wait() 只等待关闭完成，
  handler 由 Shutdown() 负责等待其结束。
```

### R-15：Windows 门禁被误报

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
  - lookup 返回 std::shared_ptr<const IMeetingLiveSession>（**强句柄**，且 const-correct），
    禁止裸借用指针；
  - 实现必须在自有互斥下从持有 shared_ptr 的注册表**复制**（禁止 .get() 造控制块）；
  - 目录本身以 std::weak_ptr<IMeetingSessionDirectory> 被 adapter 引用（判活）。

与 §15.2 的关系：
  §15.2 原文建议 "FindSession(session_id) → std::shared_ptr<CSession>（或等价受控 lookup）"。
  本裁决保持 shared_ptr 语义（**强于**推荐形式的"受控"要求），
  但把元素类型收窄为 `const IMeetingLiveSession`——一个**全 const 的存活标记**接口，
  以同时满足"adapter 不依赖 CSession 具体类型"与"接口 const 性与句柄一致"。
  真正的发送能力由 Phase 3 以**派生接口**新增（§8.2.4），不在本轮定义。
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
| #2 lookup 形式 | 未决（裸指针） | **窄接口 + `shared_ptr<const IMeetingLiveSession>` 强句柄 + weak 目录（M6.0.1 修正为 const-correct）** | 否 |
| #3 Windows gate | 未决（互相矛盾） | **方案 iii：Linux 门禁；Windows NOT VERIFIED；R6 独立任务** | 否 |
| #4 builder 调用者 | 未决 | **direct invocation 验证；生产调用者 Phase 3** | 否 |

### 19.6 M6.0.1 追加的一致性裁决（不引入新 Conflict）

M6.0.1 修的是 M6.0 文档内部的**自相矛盾**，不是新的范围争点；
四项裁决仍保持原样，只是被写得更精确：

| 项 | M6.0 的矛盾/缺口 | M6.0.1 裁决 | 依据 |
| --- | --- | --- | --- |
| B1 `Stop()` 幂等性 | §12.3 在信号回调与清理路径各调一次，而 `Stop()` 非幂等 | **方案 A**：移除信号回调中的 `Stop()`，唯一调用点置于 `ShutdownCoordinator::Cleanup()`；**不**修改 `AsioIOServicePool.*` | §12.3.2–§12.3.9 |
| B2 成员声明顺序 | §12.2 的声明顺序与它自己声称的逆序析构结果相反 | **`_outbound_collector → _meeting_adapter → _dispatcher → _work_executor`**；显式停止不可被析构顺序替代 | §12.2.2–§12.2.4 |
| B3 sink const 契约 | 非 const 方法 + `shared_ptr<const ...>` 句柄，不可同时成立 | 收窄为**全 const 的存活标记** `IMeetingLiveSession`；发送能力由 Phase 3 派生新增 | §8.2.1–§8.2.4 |
| B4 逐目标 + collector 归属 | 通知算法未绑定 target；collector 可能成为局部对象 | **逐目标算法**（§7.1.2）+ **collector 由 `LogicSystem` 以 `unique_ptr` 独占拥有**（§12.1） | §7.1.2 / §12.1 |
| B5 Git 元数据 | §1.3/§24 声称 M6.0 未推送，而 `3456c7f` 已在 `origin/dev` | 更正为"**M6.0 已发布**"；M6.0.1 亦已发布（`1454a8a`）；M6.0.2 为工作区状态 | §1.3 / §24 |

### 19.7 M6.0.2 追加的一致性裁决

| 项 | M6.0.1 的问题 | M6.0.2 裁决 | 依据 |
| --- | --- | --- | --- |
| C1 对象命名 / API | 把 TCP `CServer` 命名 `server`，与源码中 `unique_ptr<grpc::Server> server` 冲突；并暗示 `CServer` 有 `Shutdown()` | 固定命名 `grpc_server` / `tcp_server` / `grpc_server_thread` / `io_context` / `pool`；关闭 gRPC 用 `grpc_server->Shutdown()`；**不虚构** `CServer::Shutdown()` | §12.3.1 / §12.3.6 |
| C2 异常安全覆盖 | Guard 只覆盖 `run()` 之后；`on_leave` 为 nullptr；gRPC 线程早于 guard 启动 ⇒ 更早异常会 terminate | 全生命周期 `ShutdownCoordinator`：在 gRPC 线程启动后立即构造；每步独立 try/catch；`Cleanup()` 不抛 | §12.3.5 / §12.3.7 / §12.3.9 |
| C3 信号回调线程模型 | 声称回调在 pool 线程上、可能自我 Join | **删除**该结论：按源码回调在主线程（A6–A8）；单一 Stop 调用点的理由改为"非幂等 + 顺序可证明" | §12.3.1 / §12.3.4 |
| C4 目标形态完整性 | 未回答 12 个生命周期问题 | §12.3.7 四条路径 + §12.3.8 逐条回答 + §12.3.9 异常性 + §12.3.12 更早失败 | §12.3.7–§12.3.12 |

**Phase 1 仍然完全冻结**：以上四项同样是文档内部一致性与实现可行性修正，
没有修改、豁免或放宽 Phase 1 的任何条款。

### 19.8 M6.0.3 追加的裁决

| 项 | M6.0.2 的问题 | M6.0.3 裁决 | 依据 |
| --- | --- | --- | --- |
| D1 失败策略 | 每个 `Step()` 捕获后继续，包括 join 失败的关键步骤 ⇒ 可能在线程仍运行时折构依赖 | 步骤分级：1a/1b/2/4b/5 为 **K 级 → fail-stop**（记录 FATAL 后 `std::abort`，不重试、不继续）；3/4a 为 best-effort | §12.3.5.1 / §12.3.9 |
| D2 `tcp_server` 所有权 | 成员是 `CServer*`（非拥有），与"共同持有"的表格描述矛盾；兜底清理时目录可能已失效 | 采用**方案 B**：`std::shared_ptr<CServer> tcp_server_` + `SetTcpServer(shared_ptr)`；强引用使兜底 drain 期间目录依然有效，**取消** M6.0.2 的降级说法 | §12.3.8.1 |
| D3 R-18 | 声称 Redis 关闭与 gRPC handler 并发 | **删除该断言**（gRPC 1.51.1 无 deadline `Shutdown()` 阻塞到同步 handler 完成）；改为 R-18'a 顺序不变量 + R-18'b 活性/关闭挂起 | §12.3.14 / §18 |

**关于 fail-stop 的性质声明（必须保留）**：

```text
fail-stop 是本设计对"不可恢复的关闭失败"的**明确策略**，不是缺陷掩盖。
它牺牲的是"尽量优雅"（Redis 清理可能未完成），换取的是"绝不在未停止的线程之上
析构对象"（那会引发 UB 或 std::terminate）。
⇒ 触发 fail-stop 时，文档与日志都**不得**宣称优雅关闭或全部线程已 Join。
```

**Phase 1 仍然完全冻结**：D1–D3 同样是文档内部一致性与实现可行性修正，
没有修改、豁免或放宽 Phase 1 的任何条款。

**Phase 1 仍然完全冻结**：以上五项均为文档内部一致性与实现可行性修正，
**没有**修改、豁免或放宽 Phase 1 的任何条款。

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
A15  IMeetingLiveSession 的全部成员函数为 const；lookup 返回 shared_ptr<const ...>；
     adapter 不调用任何发送方法（§8.2.3 编译期 + 静态检查）
A16  AsioIOServicePool::Stop() **恰好一个**调用点，且位于 io_context.run() 之后的 guard 内
     （静态检查：全仓 grep 'pool->Stop\|->Stop()' 在 ChatServer.cpp 只有一处，且不在 signal 回调）
A17  LogicSystem 成员声明顺序为
     _outbound_collector → _meeting_adapter → _dispatcher → _work_executor（§12.2.3）
A18  collector 不是任何函数的局部变量：由 LogicSystem 以 unique_ptr 成员独占拥有（§12.1）
A19  MeetingNotificationMessage 每条构造都显式赋值非空 notification_target_session_id；
     通知条数 == target_resolved（§7.1.3）
A20  命名契约：示例中 gRPC 对象为 grpc_server、TCP 对象为 tcp_server，
     不存在名为 server 的 TCP 对象；且**不调用** CServer::Shutdown()（该方法不存在）
A21  清理范围覆盖 gRPC 线程启动后的所有异常路径：协调器在 grpc_server_thread
     启动后、tcp_server 构造前建立（§12.3.7）
A22  每个清理步骤都在独立 try/catch 内（ShutdownCoordinator::Step），
     Cleanup() 本身不抛；不依赖"析构永不失败"的假设（§12.3.9）
A23  `Cleanup()` 在 `tcp_server` 作用域内被**显式调用**（正常与 run() 异常两条路径）；
     协调器持有 `CServer*` 仅作登记，不拥有；LogicSystem 仍只持 weak_ptr（§12.3.6 / §12.3.8.1）
A24  gRPC 线程由协调器**按值**持有（成员 `std::thread`），不是指向局部变量的裸指针；
     且 `StartGrpcThread()` 位于所有可能失败的初始化之后（§12.3.5 / §12.3.6）
A25  信号回调仅调用 `io_context.stop()`，不调用 pool->Stop / ShutdownWorkExecutor /
     grpc_server->Shutdown（静态检查）
A26  清理步骤已分级：1a/1b/2/4b/5 为 K 级（失败 fail-stop），3/4a 为 best-effort；
     不存在"关键步骤失败后仍继续"的代码路径（§12.3.5.1 / §12.3.9）
A27  `tcp_server_` 成员类型为 `std::shared_ptr<CServer>`（方案 B），
     `SetTcpServer` 接收 `shared_ptr`；协调器持强引用 ⇒ 兜底 Cleanup 期间目录有效（§12.3.8.1）
A28  不对 `pool->Stop()` / `RedisMgr::Close()` 做任何重试；
     失败路径只有 fail-stop，没有"再试一次"（§12.3.5.2）
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
B14  逐目标守恒：ND1/ND2/ND3 通过（N 个有效目标 ⇒ N 条通知；缺失目标 0 条）
B15  关闭路径幂等性：OR4 通过；且 AsioIOServicePool::Stop() 在整轮 smoke 中只被调用一次
B16  run() 异常路径清理：注入一个抛异常的 handler，断言 pool 已停、worker 已 join、
     tcp_server 尚未析构（可用可观测计数/日志顺序断言）
B17  gRPC 线程必被 join：分别注入（a）tcp_server 构造抛异常、（b）目录注册抛异常、
     （c）run() 抛异常，三种情况断言进程**未** std::terminate 且清理日志齐全
B18  pool->Stop() 与 Redis Close() 在整轮 smoke 中各只执行一次
B19  信号路径：发送 SIGTERM 后，清理日志顺序为
     grpc Shutdown → pool Stop → ShutdownWorkExecutor → ClearDirectory(HDel) → redis Close → join
B20  顺序不变量：Redis 的 HDel/Close 在日志中晚于 grpc Shutdown 与 ShutdownWorkExecutor
     （守护 R-18'a；若顺序被改动则该用例失败）
B21  fail-stop 行为：注入 join 失败（可用测试替身）时，进程以非零信号终止，
     且**不**输出"graceful shutdown"类日志；Redis 清理不宣称完成
B22  兜底路径目录有效：在 run() 抛异常且**不**显式调用 Cleanup 的变体下，
     断言 drain 期间同步通知的 target_resolved 计数正常（证明方案 B 的强引用生效）
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

### 24.1 M6.0.2 的基线

```text
进入本轮的远端基线 : origin/dev == HEAD == 2ffd08330a2047c6cf74fafba07f0f0a675a4f7f
                     ⇒ M6.0.2 已发布
进入本轮时状态     : working tree / index 均 clean
本轮修改           : 仅 docs/phase2/M6-response-notification-adapter.md
本轮 add/commit/push: 由本轮任务授权执行（结果见 §24.4）
```

### 24.4 本轮发布结果（由 add/commit/push 实际输出填写）

```text
本轮按任务授权执行了 git add / commit / push。
确切结果（是否成功推送、远端是否已前进）必须由下列命令的实际输出核对，
本文档不以陈述方式预设结论：
    git rev-parse HEAD
    git rev-parse origin/dev
    git status --short
若三者显示 HEAD == origin/dev 且 worktree clean ⇒ 已发布；
若 HEAD 领先 origin/dev ⇒ 仅本地已提交、未发布；
若 worktree 仍有本文件修改 ⇒ 仅工作区、未提交。
```

### 24.2 发布状态的判定规则（不使用内嵌 hash）

```text
本文档不声明"已发布"或"未发布"这一随时间变化的属性，而是给出判定规则：

  该修订已发布  ⇔  git rev-parse HEAD == git rev-parse origin/dev
                   且 git status --short 为空
  该修订未发布  ⇔  HEAD 领先 origin/dev，或工作区仍有本文件的未提交修改

这样无论本轮修订是否已被提交/推送，本节都不会成为错误陈述。
```

### 24.3 历史记录更正

```text
M6.0 记录过"本地 commit、未推送"；M6.0.1 以 3456c7f、M6.0.2 以 1454a8a 为基线 ——
均为当时事实，之后都已推送：当前远端为 2ffd083。
因此任何"当前 HEAD = 1672888 / 3456c7f / 1454a8a"的表述均属过期，不得沿用。
```

---

## 25. 下一步

```text
1. 人工 Review 本修订，确认 §19 的四个裁决；
2. 按 §17.1 切分为独立的 M6.1–M6.8 Coding Prompt；
3. 每步完成 → Review → Build → Test → 再进入下一步；
   不得把 M6.1–M6.8 合并成一个 Prompt（STEP-1.6 §26.2 同一纪律）。
```
