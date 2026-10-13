# Phase 2 - M4：Unified Serialized Work Item

本文件记录 M4 的实现范围、设计裁决、验证结果与明确的未实现内容。

M4 是 `Meeting Core`（M1–M3）与**真实服务器执行上下文**之间的第一个集成步骤：把
`LogicSystem` 原有的 legacy message queue 演进为一个统一的、可测试的 FIFO Work Queue，
使四类工作共享同一个串行化执行顺序：

```text
legacy IM message
transport-neutral Meeting command
SESSION_DISCONNECTED   （internal work）
FINALIZE_CLOSE         （internal work）
```

仍然复用**原有的单 worker 线程语义**：没有第二条业务队列，没有第二个业务 worker，
没有线程池，没有 timer。

---

## 1. 目标与非目标

### 1.1 目标

| # | 目标 |
| --- | --- |
| 1 | 一条权威 FIFO，四类 work 共用相同的 enqueue/dequeue 规则 |
| 2 | 一个消费者线程，唯一决定 Meeting mutation 顺序（single-writer） |
| 3 | 显式 typed work item（方案 A）：`enum kind` + value payload |
| 4 | 保持 legacy IM 分发语义与调用点不变 |
| 5 | `CloseMeeting` / `FinalizeClose` 真的排成两个独立 queue turn |
| 6 | 出队锁边界：分发一律在 queue mutex 之外 |
| 7 | 可测试的 stop/drain 关闭策略 |
| 8 | `ChatServer` 真正链接 `MeetingDomain` |
| 9 | Layer B 顺序测试执行的是**生产同一份**队列/分发代码 |

### 1.2 非目标（M4 明确不做）

```text
TCP Meeting Protocol / message.proto 扩展 / 新 TCP message ID     → Phase 3
CSession / CServer 断线捕获与投递                                  → M5
响应编码（JSON/protobuf）与通知发送（session_id → CSession → send）  → M6
MeetingService 内部加锁、线程、timer、后台清理                      → 永不允许
ClosedSnapshot retention / TTL                                     → Deferred
持久化（Redis / MySQL 业务路径重写）                                → 不做
Windows .sln/.vcxproj/.props 改动                                  → 不做（见 §17）
```

M4 **没有**重新设计任何 M1–M3 领域契约：`ResultCode`、权限、状态机、事件语义、
`Join` 的四种 outcome、CLOSED 快照语义全部保持原样。

---

## 2. M3 前置状态

M4 起点（只读复核，未修改）：

```text
branch        : dev
HEAD          : d3d69bf  docs(phase2): add M3 closeout and M4 handoff
origin/dev    : d3d69bf  （ahead/behind = 0 0，working tree / index 均 clean）
M3 实现 commit : 57090d8  feat(meeting): implement meeting service and registry
```

`git fetch origin` 在本会话中**失败**：`Failed to connect to github.com port 443`
（网络不可达，不是仓库状态问题）。本地 `refs/remotes/origin/dev` 与 HEAD 相等，
且本会话未创建任何 commit、未 push，因此该失败不影响本次改动的正确性；如需最终确认
远端状态，请在网络恢复后由人工 `git fetch` 核对。

M3 交付且 M4 直接消费的契约：

```text
MeetingService(local_server_id, MeetingServiceDependencies)   // ordinary object, not thread-safe
RegisterConnectedSession / AuthenticateSession / TryGetDomainSessionState
CreateMeeting / JoinMeeting / LeaveMeeting / CloseMeeting / QueryMeeting / ListParticipants
FinalizeClose(meeting_id)          // 第二个 serialized turn
SessionDisconnected(session_id)    // 0/1/N fan-out
HasActiveMeeting / HasClosedSnapshot / ReverseBindingCountForSession / EvictClosedSnapshot
```

---

## 3. 旧队列结构事实（改造前，只读核实）

来源：`LogicSystem.h` / `LogicSystem.cpp`（改造前版本）。

| # | 事实 |
| --- | --- |
| 1 | `_msg_que` 类型：`std::queue<std::shared_ptr<LogicNode>>` |
| 2 | `PostMsgToQue()` 调用点：**只有一处** —— `CSession.cpp:165`（`AsyncReadBody` 读取完整 body 后） |
| 3 | `DealMsg()`：`for(;;)` → 加锁 → `_consume.wait()` → `if (_b_stop)` 时**排空并 break** → 否则 `front()` → `std::cout` → 查 `_fun_callbacks` → 命中则调用 → `pop()` |
| 4 | `_mutex` 持锁范围：**覆盖 callback 执行**（`call_back_iter->second(...)` 在 `unique_lk` 生命周期内） |
| 5 | worker 创建/停止：构造函数 `std::thread(&DealMsg)`；析构 `_b_stop = true` → `notify_one()` → `join()` |
| 6 | `_b_stop` 同步：与队列内容由同一个 `_mutex` 保护（`_b_stop` 只在持锁路径上读写） |
| 7 | 分发表：`std::map<short, FunCallBack> _fun_callbacks`，注册 5 个 msg_id：`MSG_CHAT_LOGIN=1005`、`ID_SEARCH_USER_REQ=1007`、`ID_ADD_FRIEND_REQ=1009`、`ID_AUTH_FRIEND_REQ=1013`、`ID_TEXT_CHAT_MSG_REQ=1017` |
| 8 | `LogicNode`（定义在 `CSession.h`）持有 `std::shared_ptr<CSession>` 与 `std::shared_ptr<RecvNode>`，因此队列元素**强引用 CSession** |
| 9 | `MeetingService` 可直接调用入口：见 §2 清单 |

### 3.1 改造前的真实风险（M4 的直接动因）

`DealMsg()` 在 `_mutex` 保护区内执行 legacy callback。一旦 Meeting 也接入这条队列，
`CloseMeeting` 的分发过程中需要向**同一**队列追加 `FINALIZE_CLOSE`：

```text
持有 queue mutex → 调用业务 → 业务向同一队列 push → 需要同一 mutex → 自死锁
```

因此 M4 的锁边界改造不是"风格问题"，而是接入的前置条件（见 §6）。

---

## 4. `LogicWorkItem` 设计

新增 `Server/ChatServer/ChatServer/LogicWorkItem.h`（UTF-8，仅值类型，无队列/线程/锁）。

### 4.1 kinds

```cpp
enum class LogicWorkKind {
    EXTERNAL_MESSAGE = 0,               // legacy IM 消息（可持有 LogicNode）
    MEETING_COMMAND = 1,                // transport-neutral Meeting 命令
    SESSION_DISCONNECTED = 2,           // 断线内部工作（只存 SessionId）
    FINALIZE_CLOSE = 3,                 // 第二个 serialized turn（只存 MeetingId）
    INTERNAL_SESSION_REGISTER = 4,      // domain Session → CONNECTED
    INTERNAL_SESSION_AUTHENTICATE = 5   // domain Session → AUTHENTICATED
};
```

`MEETING_COMMAND` **不是**新的网络协议类型，也不是 TCP message。它只是把 M3 已经存在的
transport-neutral API 表达成一个真实 typed work item，使得在没有 TCP Meeting message ID
的 M4 阶段，Meeting 命令可以与 legacy 消息、断线、finalize 共享同一个 FIFO，并且这个
FIFO 的顺序可以被确定性测试。

`INTERNAL_SESSION_REGISTER` / `INTERNAL_SESSION_AUTHENTICATE` 是**最小的**领域 Session
生命周期登记入口，存在的唯一理由是：Layer B 测试必须在同一个 single-writer 执行路径上
准备有效的 `AUTHENTICATED` Session，而不能在 worker 运行时从测试线程直接改
`MeetingService`。它们**不代表** M4 已接入真实 `CSession` 的认证路径。

刻意**没有**增加的 kind：任何"未来可能用到"的类型、通知发送类、持久化类。

### 4.2 payload 与字段有效性

| kind | 有效字段 |
| --- | --- |
| `EXTERNAL_MESSAGE` | `external_logic_node`（`std::shared_ptr<LogicNode>`） |
| `MEETING_COMMAND` | `meeting_context`、`meeting_command`、`has_meeting_id`、`meeting_id` |
| `SESSION_DISCONNECTED` | `session_id` |
| `FINALIZE_CLOSE` | `meeting_id` |
| `INTERNAL_SESSION_REGISTER` | `session_id` |
| `INTERNAL_SESSION_AUTHENTICATE` | `session_id`、`user_id` |

字段有效性只由 kind 决定，不存在隐式约定。

内部 work item **不持有**：`CSession*`、`shared_ptr<CSession>`、`CServer*`、
`MeetingAggregate*`、socket。只有 `EXTERNAL_MESSAGE` 继续持有 `LogicNode`，从而保持
既有旧 IM 消息的生命周期语义。

### 4.3 刻意不采用的形态

```text
polyrmorphic work item 基类           → 需要 shared_ptr 多态 + 虚函数，调试需 RTTI
std::function<void()> opaque task queue → 逻辑藏在 lambda capture 里，无法断言队列内容与顺序
```

### 4.4 只读调试能力

`LogicWorkItem::Describe()` 直接输出队列里真实排了什么（例如
`MEETING_COMMAND(CLOSE meeting_id=m-1)`、`FINALIZE_CLOSE(meeting_id=m-1)`），
使"队列顺序"可以被断言而不是靠日志目测。

### 4.5 观测接口

`IWorkObservationSink` 是**纯观测**（实现方不得修改领域状态、发送响应或触发通知）：

```text
RecordWorkDispatched(item, sequence, events)       // 真实 dequeue/dispatch 次序
RecordInternalWorkEnqueued(item, queue_after)      // 内部 work 入队瞬间的队列内容
RecordLegacyMessageRouted(session_id, msg_id, size)
RecordSessionDisconnected(session_id, affected_meeting_count)
RecordFinalizeClose(meeting_id, finalized, already_finalized, notification_session_ids)
RecordMeetingCommand(outcome)                      // result_code / disposition / state / events
```

生产 `LogicSystem` 注入 `nullptr`，因此该路径零开销；它存在的意义是让 Layer B 断言
"真实执行顺序"，而不是靠 sleep 或日志。

---

## 5. 单一 FIFO 与 worker 所有权

### 5.1 为什么抽出独立文件

新增 `LogicWorkDispatcher.h` / `LogicWorkDispatcher.cpp`。原因不是"为了好看"，
而是因为 `LogicSystem` 是 Singleton，且其构造/业务路径会拉起 Redis / MySQL / gRPC：

```text
若把队列与分发逻辑写在 LogicSystem 内部
  → Layer B 测试要么启动整条旧 IM 业务链路
  → 要么只能测试一个"假的 executor"
```

抽取后同时满足全部六条约束：

| 约束 | 满足方式 |
| --- | --- |
| 生产 `LogicSystem` 真正使用该组件 | `_work_executor` 是 `SerializedWorkExecutor<LogicWorkDispatcher>` 成员 |
| 测试执行同一份队列/分发代码 | `LogicWorkQueueTest.cpp` 直接实例化同一模板与同一 dispatcher |
| 生产进程内仍只有一个逻辑 FIFO | 只有 `SerializedWorkExecutor` 内部那一个 `LogicWorkQueue` |
| 不为测试另造模拟 executor | 测试用真实 `SerializedWorkExecutor`；测试特有之物只有"记录型 handler/观测 sink" |
| 不增加第二个生产 worker | 全仓库仅 `SerializedWorkExecutor` 创建线程；`MeetingService` 无线程 |
| 不让 MeetingDomain 反向依赖 LogicSystem | `MeetingDomain` 只编译 `meeting/*.cpp`，无任何 `#include "Logic*"` |

### 5.2 单一队列与单消费者的结构证明

```text
生产者:
  LogicSystem::PostMsgToQue(LogicNode)      → EXTERNAL_MESSAGE
  LogicSystem::PostMeetingCommandWork(...)  → MEETING_COMMAND
  LogicSystem::PostSessionDisconnectedWork  → SESSION_DISCONNECTED
  LogicWorkDispatcher 分发 Close 时          → FINALIZE_CLOSE（同一队列尾部）

                    ↓  全部进入同一 LogicWorkQueue（一条 std::deque + 一个 mutex）

消费者:  SerializedWorkExecutor<THandler>::WorkerLoop（唯一线程）
             ↓ 锁外分发
         ┌───────────┴────────────┐
         ▼                        ▼
  ILegacyMessageRouter      IWorkHandler
  （LogicSystem 的            （LogicWorkDispatcher：
    _fun_callbacks 查表）      唯一 MeetingService owner）
```

结构上不存在第二条队列：`LogicWorkQueue` 在本仓库中只被 `SerializedWorkExecutor`
实例化一次（`_work_executor->queue_`）。也不存在第二个业务 worker：
`MeetingService` 不创建线程，`SESSION_DISCONNECTED` / `FINALIZE_CLOSE` 都只能通过入队
被执行，不存在"网络线程直接清理 Meeting"或"独立 async task 执行 Finalize"的路径。

### 5.3 MeetingService 所有权

```cpp
class LogicWorkDispatcher : public IWorkHandler {
    ...
    meeting::MeetingService meeting_service_;   // 普通成员，不是 Singleton、不是 shared_ptr
};
```

`MeetingService` 是普通成员，因为 single-writer 的前提正是"只有一个明确的 owner"。
它自己不加锁、不建线程、不发消息。**没有任何 getter 会返回可变的 `MeetingService`、
registry 或队列** —— 否则网络线程就能绕过 FIFO 直接 mutation：

```cpp
bool HasActiveMeeting(const meeting::MeetingId&) const;      // 只读、值语义
bool HasClosedSnapshot(const meeting::MeetingId&) const;     // 只读、值语义
const meeting::MeetingId& LastCreatedMeetingId() const;      // 上次成功创建的 ID（值拷贝）
```

---

## 6. enqueue / dequeue / dispatch 锁边界

### 6.1 消费者循环结构

```cpp
for (;;) {
    LogicWorkItem item;
    {
        LogicWorkQueue::Lock lock(queue_);          // 唯一的队列临界区
        queue_.WaitForWorkOrStopLocked(lock);
        if (queue_.IsStopRequestedLocked() && queue_.IsEmptyLocked()) {
            break;                                  // stop 且已排空
        }
        if (!queue_.Pop(item)) { continue; }
        in_flight_.fetch_add(1);                    // 只用 atomic，绝不在此处再取 idle mutex
    }
    // ---------------- 队列锁已释放 ----------------
    DispatchOutsideLock(item);                      // legacy callback 或 Meeting mutation
}
```

### 6.2 三条必须证明的性质

| 性质 | 证明方式 |
| --- | --- |
| dispatch 不持 queue mutex | `LogicWorkQueueTest.QueueLockIsNotHeldWhileDispatching`：handler 在 `DispatchWork` 内读 `IsLockHeldByCurrentThread()`，断言为 false |
| dispatch 可以向同一 FIFO 再入队且不自死锁 | `LogicWorkQueue.DispatchCanReenqueueToTheSameFifoWithoutDeadlock` 与 `LogicWorkQueueBMixed.*`：Close 在分发中追加 FINALIZE_CLOSE，最终确实被第三个 turn 执行并产生 CLOSED 快照 |
| 不会有多消费者并发改 MeetingService | `SerializedWorkExecutor` 只创建一个线程；`MeetingQueueScenario` 的 6 条命令按投递顺序、逐条完成 |

### 6.3 实现过程中真实踩到的并发缺陷（记录以备回归）

M4 的实现过程本身产生了下列真实缺陷，全部由测试暴露并修正。它们不是理论问题，
因此逐一记录，避免以后"简化"时重新引入。

**(1) 同线程重复加同一非递归 mutex（出现了三次，形态各不相同）**

| 位置 | 形态 | 修正 |
| --- | --- | --- |
| `EnqueueBatch` | 先取 `LogicWorkQueue::Lock`，又调用会再次加锁的 `Push` | 区分"持锁可调用版本"（`PushLocked` / `IsEmptyLocked` / `IsStopRequestedLocked` / `WaitForWorkOrStopLocked`）与"自行加锁版本"，并在命名上直接体现锁归属 |
| 消费者循环 | 已持队列锁，却调用 `IsStopRequested()` / `IsEmpty()`（内部再 `lock_guard`） | 消费者一律只调用 `*Locked` 版本 |
| `PushInternalDuringDrain` | 调用方 `EnqueueInternalDuringDrain` 已持锁，被调方又取一次 | 重命名为 `PushInternalDuringDrainLocked`，用命名固化"调用方必须持锁"这一前置条件 |

教训：**"谁持锁"必须是 API 名称或类型的一部分，不能靠注释和记忆。**

**(2) 关闭路径静默丢弃内部 work**

最初的 `stop` 语义让 `Push` 在 `stopping_` 置位后拒绝**一切**入队。于是 Close 在 drain
阶段执行时，它派生出的 `FINALIZE_CLOSE` 被丢弃，Meeting 永久停在 `ENDING` 而没有
CLOSED 快照 —— 一个只在关闭窗口出现的悬挂状态。

修正：新增 drain 阶段的内部入队通道（`IInternalWorkSink` →
`EnqueueInternalDuringDrain`），只允许**消费者线程**为"已受理工作的必要后续"使用；
外部生产者仍然只能走会拒绝的 `Push()`。
由 `LogicWorkQueueShutdown.PendingCloseProducesFinalizeDuringDrain` 锁定。

**(3) 锁序反转：gate 持 `idle_mutex_` 时再入队（死锁）**

第一版的"测试门"在 `HoldGate()` 中获取 `idle_mutex_`，而测试随即调用 `EnqueueBatch`
去取队列锁，形成：

```text
调用方: idle_mutex_ → queue mutex
消费者: queue mutex（等待/取出） → （若需要）idle_mutex_
```

这是一次真实死锁。修正：gate 退化为**纯 atomic 标志，永不获取任何锁**，并且不再承担
"这批 work 已跑完"的判定（改由 §6.5 的进度基线负责）。

**(4) Lost wakeup：状态变化与 notify 不在同一把锁之下（表现为偶发 5000ms）**

这是最隐蔽、也是最后被解决的一个。等待谓词读取的状态里，有两处的"写"发生在
`idle_mutex_` **之外**：

| 状态 | 早期问题 | 修正 |
| --- | --- | --- |
| `gate_held_` | 锁外置 false 后 `notify_all`；等待方可能在此之间完成谓词判定并挂起，通知被丢弃 | gate 的写与 notify 都放回 `idle_mutex_` 之下（同时 gate 不再与入队嵌套） |
| **`in_flight_` 归零** | 消费者在锁外 `fetch_sub(1)` 再 `notify_all`。等待方判定时看到 `in_flight_ == 1`，随后挂起；通知在此之前已经发出并被丢弃 → 只能等到 5s 超时 | 把"在途归零 + notify"整体放进 `idle_mutex_`（此时已在队列锁之外，锁序安全） |

修正后实测：整轮 193 个用例稳定在 **23–27ms**；连续 **80 次** 独立进程整轮重跑
全部通过，且**没有任何单个用例耗时超过 100ms**（修正前约每 3 次就有一次 5000ms 停顿）。

教训：**任何被 `wait` 谓词读取的状态，其"写 + notify"必须与谓词判定处于同一把
mutex 之下。** 只靠 atomic 保证可见性，并不足以避免 lost wakeup。

**(5) 观测顺序与真实分发顺序不一致**

`DispatchClose` 最初先 `PostInternalWork(FINALIZE_CLOSE)`，再记录本次 Close 的 dispatch
结果。真实队列顺序没有问题，但**观测点**会看到"Finalize 出现在 Close 自己之前"。

修正：本 turn 的结果先落地，然后才追加派生的内部 work（见 §6.4）。

### 6.4 确定性空闲判定（`LogicWorkIdleProgress`）

"这次投递的 work 已经跑完"必须能被**确定性**等待，否则测试只能靠 sleep。
最终实现由两个同锁发布的计数器构成：

```text
enqueued_count_   队列**接受**一条 work 时自增（消费者的 drain 通道内入队也计入，
                  与队列内容在同一临界区；消费者线程入队由 IInternalWorkSink 统一记账）
consumed_count_   消费者**取出**一条 work 时自增，与 Pop 在同一临界区内

空闲 ⇔ !gate && !worker_exited && queue.IsEmpty() && in_flight_ == 0
        && consumed_count_ >= enqueued_count_          // 已接受的都取出过
        && consumed_count_ >= baseline.enqueued         // 且跨过了本批的下界
```

关键点：
- `consumed_count_` 与 `Pop` 同临界区，因此不存在"已取出但计数未更新"的窗口；
- 判定同时要求"队列已空 + 无在途"，因此**派生出的内部 work**（如 Close 的
  `FINALIZE_CLOSE`）也会抬高 `enqueued_count_` 并被要求执行完；
- 等待方在入队**之前**、于同一把 `idle_mutex_` 之下取得下界
  （`EnqueueBatchBaseline`），因此不会被"更早的一次停放"或"瞬时空队列"提前满足。

这正是修正前反复出现 5000ms 停顿的那条路径：早期版本只用"停放次数"，会被更早的
一次停放满足，于是等待提前返回，剩下的工作直到析构里的 `join()` 才被执行。

### 6.5 一个被刻意修正的"观测顺序"缺陷

`DispatchClose` 最初先 `PostInternalWork(FINALIZE_CLOSE)`，再记录本次 Close 的 dispatch
结果。真实队列顺序本身没有问题，但**观测点**会看到"Finalize 出现在 Close 自己之前"。

修正：本 turn 的结果先落地，然后才追加派生的内部 work（见 `LogicWorkDispatcher.cpp`
的 `DispatchClose` 注释）。这保证了"观测到的顺序 == 真实分发顺序"。

---

## 7. legacy callback 保持方式

### 7.1 对外接口与调用点不变

```cpp
void LogicSystem::PostMsgToQue(std::shared_ptr<LogicNode> msg);   // 签名不变
```

调用点 `CSession.cpp:165` **未修改**（`CSession.cpp` / `CServer.cpp` 本次零改动）。

`PostMsgToQue` 现在把消息包装成 `EXTERNAL_MESSAGE` 后投入统一 FIFO：

```cpp
void LogicSystem::PostMsgToQue(std::shared_ptr<LogicNode> msg) {
    EnsureWorkQueueStarted();
    _work_executor->PostExternalMessage(msg);
}
```

`EXTERNAL_MESSAGE` 由消费者**直接交给 `ILegacyMessageRouter`**，不经过 Meeting
dispatcher，因此 Meeting 路径无法影响旧 IM 消息的行为。

### 7.2 分发语义逐句对齐

`LogicSystem::RouteExternalMessage` 与旧 `DealMsg` 内联代码逐句对应：

```text
旧: std::cout << "recv msg id is " << msg_node->_recvnode->_msg_id
新: 相同

旧: auto it = _fun_callbacks.find(msg_node->_recvnode->_msg_id);
     if (it != _fun_callbacks.end()) it->second(_session, msg_id, string(data, cur_len));
新: 相同（未命中仍然忽略；callback 参数个数/类型/顺序相同）
```

仅有两处差别，且都是 M4 的结构要求：

1. 本函数由消费者在**队列锁之外**调用（旧实现持锁执行回调）；
2. 增加空指针保护（旧实现直接解引用，而队列中不可能为空）。

`RegisterCallBacks` 与五个业务方法**逐字未改**（见 §16 的静态 diff 结论）。

---

## 8. Meeting typed command 路径

`LogicWorkDispatcher::DispatchWork` 是唯一的 Meeting 命令分发点，**只消费 M3 的 API**，
不重新实现任何领域判断：

| 命令 | 调用 |
| --- | --- |
| `CREATE` | `MeetingService::CreateMeeting(context)` |
| `JOIN` | `MeetingService::JoinMeeting(context, meeting_id)` |
| `LEAVE` | `MeetingService::LeaveMeeting(context, meeting_id)` |
| `CLOSE` | `MeetingService::CloseMeeting(context, meeting_id)` |
| `QUERY` | `MeetingService::QueryMeeting(context, meeting_id)` |
| `LIST_PARTICIPANTS` | `MeetingService::ListParticipants(context, meeting_id)` |

- **没有**新增 TCP message ID，**没有**改 `message.proto`，**没有**引入 protobuf 变化。
- Query / List 与 mutation 在**同一个 worker** 上执行，因此观察到的状态一定来自完整的
  serialized turn，不可能看到某个 mutation 的中间组合（Step 1.3 §22）。
- `result_name` 一律来自 M3 的 `ResultCodeName()`，不维护第二套字符串映射。

---

## 9. SessionDisconnected internal work

```cpp
bool LogicSystem::PostSessionDisconnectedWork(const meeting::SessionId& session_id);
```

- 只保存稳定 `SessionId`，不持有 `CSession`；
- 不包装成假 `LogicNode`，不生成 TCP msg_id；
- 消费者收到后在同一 serialized turn 内调用 `MeetingService::SessionDisconnected(session_id)`，
  由 M3 完成 0 / 1 / N fan-out；
- **M4 不修改** `CSession::Close()`、`CServer::ClearSession()` 或任何网络回调；
  真实断线捕获与投递（"在 `ClearSession` 之前捕获稳定 context"）属于 M5。

---

## 10. Close / FinalizeClose 两 turn

### 10.1 时序

```text
turn 1（Close 命令）:
  CloseMeeting(...) → OK + CLOSE_STARTED + MeetingView(ENDING)
  → sink 先记录本 turn 结果
  → should_schedule_finalize == true 时，向**同一 FIFO 尾部**追加恰好一个 FINALIZE_CLOSE
  → 本 turn 结束（响应此刻已是 ENDING）

turn 2（FINALIZE_CLOSE 被 dequeue）:
  FinalizeClose(meeting_id) → 发布 ClosedMeetingSnapshot → MeetingClosed 恰好一次
```

### 10.2 硬性保证

| 保证 | 落点 |
| --- | --- |
| `CloseMeeting` 不同步执行 Finalize | `DispatchClose` 不调用 `FinalizeClose`（M3 也刻意没在 `CloseMeeting` 内调用） |
| 不创建第二条队列 | 复用 `PostInternalWork` → 同一 `LogicWorkQueue` |
| 只有 `should_schedule_finalize == true` 才安排 | `if (result.should_schedule_finalize)` |
| duplicate Close 不重复安排 | 第二次 Close 在 M3 第 6 步命中 `CLOSE_IN_PROGRESS`，`should_schedule_finalize == false` |
| Finalize 在独立的 dequeue/dispatch 中运行 | 由 `SerializedWorkExecutor` 的下一轮循环取出 |
| 结束状态由 M3 发布 | snapshot 发布先于 active 释放（M3 §17） |
| 本次不发送通知 | `notification_session_ids` 只是**意图**，M4 不发任何东西 |

### 10.3 后入队顺序（不插队）

`Close` 执行时，FIFO 尾部可能已经有别的命令。新追加的 `FINALIZE_CLOSE` 排在它们**之后**：

```text
FIFO: [Close(M)] [Join(M, B)]
Close 执行 → ENDING → 尾部追加 Finalize(M)
实际执行次序:
  Close(M)     → OK + ENDING
  Join(M, B)   → MEETING_STATE_REJECTED
  Finalize(M)  → CLOSED
```

顺序**完全由真实 enqueue 次序决定**，不存在 kind 优先级；`CLOSE` 不会被提前，
`FINALIZE_CLOSE` 也不会插队。

`LogicWorkQueueBMixed.MixedKindsKeepExactEnqueueOrder` 用确定性方式锁定这一点：它记录
内部 work **入队瞬间**的队列内容（`RecordInternalWorkEnqueued`），断言 `FINALIZE_CLOSE`
是该快照的**队尾**，且此刻队列里至少还排着两条 work。之所以不在这里断言"最终观察到的
绝对位置"，是因为 Close 之后由外部生产者追加的 work 与 Close 派生的 Finalize 天然存在
并发交错；`FINALIZE_CLOSE` 一定是"排在入队那一刻已排队内容之后"，这才是契约。

---

## 11. Shutdown / Drain policy

### 11.1 语义

```text
RequestStop():
  stopping_ = true（与队列内容由同一个 queue mutex 协调）
  condition_.notify_all()

消费者:
  结束条件 = 已请求停止 **且** 队列已排空
  → stop 之后**先排空**再退出
```

| 要求 | 落点 |
| --- | --- |
| stop 标志无数据竞争 | `stopping_` 只在持 `mutex_` 时读写 |
| stop 与入队由同一 mutex 协调 | `Push` / `PushLocked` / `PushInternalDuringDrain` / `RequestStop` 同锁 |
| worker 能在队列排空后退出 | `IsStopRequestedLocked() && IsEmptyLocked()` → `break` |
| 不在持锁状态下 `join()` | `StopAndJoin()` 不在任何队列临界区内 |
| 已受理的合法 Close 不丢 Finalize | `PushInternalDuringDrain()` + 由 `PendingCloseProducesFinalizeDuringDrain` 锁定 |
| stop 完成后不再处理新增外部 work | `Push()` 在 `stopping_` 后返回 `false`（丢弃语义见 §11.2） |
| 测试能确定性等待退出 | `WaitUntilIdle(timeout_ms, parked_baseline)` / `StopAndJoin()`，无 sleep |
| 不增加第二个线程解决关闭问题 | 没有额外线程 |

### 11.2 两处必须说清的语义

1. **stop 之后到达的外部 work 会被丢弃（`Push` 返回 `false`）**，不排队、不执行。
   这不是错误：stop 只会在进程关闭路径上被触发，此后的网络消息本来就已经失去处理意义。
2. **内部 work 是唯一例外，且只对消费者线程开放。** `PushInternalDuringDrain()` 不检查
   `stopping_`，因为 Close 派生出的 Finalize 属于**已受理**工作的必要后续；把它丢掉会
   产生"Close 返回 ENDING 之后 Meeting 永远停在 ENDING"的悬挂状态。

### 11.3 为什么需要一个"进度基线"

消费者在"队列恰好为空"与"生产者刚入队"之间存在极短窗口，只看队列是否为空会误判空闲。
更隐蔽的是：只看"消费者停放次数"也不行 —— 那个条件会被**更早的一次停放**满足，于是等待
在本批 work 执行之前就返回（本步真实触发，表现为断言看到空结果、随后析构里的 `join()`
才把剩余工作跑完，用例因此偶发多花 5s）。

最终方案见 §6.4：等待方在同一把 `idle_mutex_` 之下取"已接受 work 计数"的下界，并要求
`consumed_count_` 越过它，同时队列为空、无在途。这样"这批 work 已经跑完"（含它派生的
内部 work）成为可判定条件，而不是时序运气。

## 12. 测试切口如何复用生产代码

```text
生产:  LogicSystem
         └── SerializedWorkExecutor<LogicWorkDispatcher>   ← 真实队列 + 真实单消费者
               ├── ILegacyMessageRouter = LogicSystem（_fun_callbacks 查表）
               └── IWorkHandler        = LogicWorkDispatcher（拥有 MeetingService）

测试:  SerializedWorkExecutor<LogicWorkDispatcher>          ← 同一模板、同一队列实现
         ├── IWorkObservationSink  = RecordingSink（记录，不修改领域状态）
         ├── ILegacyMessageRouter  = RecordingRouterWithDispatchLog（记录，不执行 IM 业务）
         └── LogicWorkDispatcher   = 生产实现本身（Meeting 命令路径）
```

- 测试与生产的差别只有三处：ID/clock 由固定序列注入（M3 已支持的注入点）；legacy 路由
  目标是记录型替身；观测 sink 非空。
- 测试**没有**复制生产分发逻辑：Meeting 命令走的就是 `LogicWorkDispatcher`。
- `RECORDING` handler（`RecordingMeetingHandler`）只用于纯队列语义（再入队、drain、
  锁边界）测试，它只实现**单个命令的执行**，不复制 dispatch 分发表。
- 不使用 `#define private public`，不访问任何私有成员。
- `LogicSystem.cpp` **不进入**测试 target：它会把 Redis / MySQL / gRPC 整条业务链路拉进
  纯单元测试，违反 Step 2.5 的目标边界。因此 LogicSystem 侧保持 legacy 语义的部分由
  §7.2 的逐句对齐 + §16 的静态 diff 结论保证，验证边界在 §18 明确标注。

---

## 13. Layer B 顺序测试及结果

新增 `Server/ChatServer/ChatServer/tests/LogicWorkQueueTest.cpp`，**25 个测试 / 8 个 gtest 套件**，全部通过。逐组对应任务要求：

| 任务要求的场景 | 测试名 | 断言要点 |
| --- | --- | --- |
| Join → Close | `LogicWorkQueueLayerB.JoinThenClose` | Join OK；Close = OK + `ENDING` + `should_schedule_finalize`；随后 Finalize 完成并发布 CLOSED 快照 |
| Close → Join | `LogicWorkQueueLayerB.CloseThenJoinIsRejected` | 顺序 Close→Join→Finalize；Join = `MEETING_STATE_REJECTED` / `ERROR`，不得重新激活 |
| Leave → Disconnect | `LogicWorkQueueLayerB.LeaveThenDisconnectDoesNotDuplicateParticipantLeft` | `ParticipantLeft` 恰好一次；Disconnect 的 `affected_meeting_count == 0` |
| Disconnect → Leave | `LogicWorkQueueLayerB.DisconnectThenStaleLeaveIsRejectedBySessionState` | fan-out 1 次；旧 Leave = `SESSION_STATE_REJECTED`（不是 `AUTH_REQUIRED`） |
| Close → Close | `LogicWorkQueueLayerB.SecondCloseIsCloseInProgressAndSchedulesNoSecondFinalize` | 第二次 = `CLOSE_IN_PROGRESS` / `IDEMPOTENT`；不排第二个 Finalize；`MeetingClosed` 恰好一次 |
| Close → Finalize（两 turn） | `LogicWorkQueueLayerB.BeginCloseResponseEndingThenFinalizeInSeparateTurn` | turn 1 无事件 + `ENDING`；`FINALIZE_CLOSE` 恰好一次 dispatch；通知意图只输出一次 |
| （同上，重复 finalize） | `LogicWorkQueueLayerB.RepeatedFinalizeIsIdempotentAndProducesNoSecondEvent` | `already_finalized == true`，无第二次通知，无第二个 `MeetingClosed` |
| Mixed Work FIFO | `LogicWorkQueueBMixed.MixedKindsKeepExactEnqueueOrder` | 4 种 kind 完全按入队次序；内部 Finalize 断言为"入队瞬间的队尾" |
| Re-enqueue（同队列） | `LogicWorkQueue.DispatchCanReenqueueToTheSameFifoWithoutDeadlock` | 分发中向同一 FIFO 入队不死锁、不丢任务、最终 CLOSED |
| Re-enqueue 不插队 | `LogicWorkQueue.ReenqueueFromDispatchIsObservedAfterAlreadyQueuedWork` | 顺序 Close→Join(拒绝)→Finalize |
| Shutdown / drain | `LogicWorkQueueShutdown.{EmptyQueueStopExitsPromptly, NonEmptyQueueIsDrainedBeforeExit, PendingCloseProducesFinalizeDuringDrain, QueueRejectsNewWorkAfterStopRequest}` | 空/非空队列都能退出；drain 一个不丢；Close 派生的 Finalize 不被丢；stop 后拒绝新 work |
| Legacy callback 分发 | `LogicWorkQueueLegacy.{ExternalMessagesReachTheLegacyRouterInOrder, PostExternalMessageIsRejectedAfterStop}` | legacy 消息按位置路由，不被 Meeting 路径吞掉 |
| 单一 FIFO 基础顺序 | `LogicWorkQueue.DispatchesInStrictFifoOrderAcrossAllKinds`、`LegacyExternalMessageGoesThroughTheSameFifoAndRouter` | 入队次序 == 分发次序 |
| 锁边界 | `LogicWorkQueue.QueueLockIsNotHeldWhileDispatching` | dispatch 期间不持有队列锁 |
| Single-writer | `LogicWorkQueue.MeetingMutationsNeverOverlap`、`SingleConsumerThreadOwnsMeetingMutations` | 命令逐条、顺序一致、结果完整 |
| dispatcher wiring | `LogicWorkQueueDispatcher.{UnauthenticatedSessionIsRejectedWithAuthRequired, CreateThenQueryStillSeesActiveMeeting}` | 未认证 → `AUTH_REQUIRED`；Create 后 Query 仍看到 active |
| 队列原语 | `LogicWorkQueuePrimitive.{FifoAndAccounting, BatchEnqueueIsAtomicAndRejectsNothingBeforeStop}` | FIFO、计数、stop 拒收、批量入队 |

反作弊对照（任务 §23）：

```text
不只验证 enum 存在                → 每个用例都断言真实 dispatch/领域结果
不绕过 FIFO 直接调用 M3           → Meeting 命令全部经 SerializedWorkExecutor 投递
不用测试专属队列替身              → 测试实例化生产同一份 SerializedWorkExecutor
不只验证 enqueue                  → 全部断言"分发后"的观测结果
无 EXPECT_TRUE(true) 占位          → 无
不依赖 sleep 竞态                 → 无 sleep；使用条件变量 + parked 基线 + gate
```

---

## 14. CMake / build / CTest

### 14.1 显式 source list（无 recursive glob）

```cmake
# ChatServer 可执行文件（新增一个 TU）
    LogicSystem.cpp
    LogicWorkDispatcher.cpp

# ChatServer 现在真正链接 MeetingDomain
target_link_libraries(ChatServer PRIVATE
    ...
    MeetingDomain
)

# 测试 target（同一份实现作为源码编入，不链接 ChatServer 可执行文件）
add_executable(ChatServerUnitTests
    ...
    LogicWorkQueueTest.cpp
    ../LogicWorkDispatcher.cpp
)
```

依赖方向（构建上强制，不是约定）：

```text
MeetingDomain                      → C++ 标准库（不链接任何东西，无 #include "Logic*"）
LogicSystem / LogicWorkDispatcher  → 可依赖 MeetingDomain
MeetingDomain                      → 不依赖 LogicSystem（反向依赖不存在）
```

### 14.2 构建与测试结果

```text
configure (-DBUILD_TESTING=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON) : PASS  exit 0
build                                                            : PASS  exit 0
  ChatServerGeneratedProto PASS / MeetingDomain PASS / ChatServer PASS / ChatServerUnitTests PASS
warning                                                          : 1 条
  MysqlDao.cpp:95 control reaches end of non-void function（既有，按任务要求只记录不修）
ctest -N                                                         : Total Tests: 193
ctest --output-on-failure                                        : 100% tests passed, 0 tests failed out of 193
直接运行 ChatServerUnitTests                                     : 193 tests / 31 test suites, exit 0（M3 时代 168 / 23）
连续 80 次独立进程整轮重跑（含并发缺陷修正后）                    : 193 PASSED × 80，0 failing
  整轮耗时分布 23–27 ms；无任何单个用例 > 100 ms
新增 UTF-8 源文件的 -Wall -Wextra 编译                          : 0 warning（测试文件曾有一处
                                                                   -Wreorder，已修正初始化顺序）
```

> 说明：修正 lost wakeup 之前，整轮重跑约每 3 次就有一次被 5000ms 超时拖慢
>（`WaitUntilIdle` 超时后才因谓词成立而返回）。§6.3(4) 记录了完整原因与修正。

测试数量守恒：

```text
M3 及以前            : 168  （未删除、未修改任何一个）
M4 新增              :  25  （LogicWorkQueueTest.cpp，8 个套件）
合计                 : 193
```

### 14.3 BUILD_TESTING=OFF

```text
configure  : PASS  exit 0
build      : PASS  exit 0（ChatServer + MeetingDomain 全部 Built）
gtest 字样  : 0 行（GoogleTest 不是生产构建的依赖）
```

---

## 15. C++14 / 依赖隔离

```text
g++ -std=c++14 -pedantic-errors -Wall -Wextra -c LogicWorkDispatcher.cpp   → exit 0
g++ -std=c++14 -pedantic-errors -fsyntax-only LogicWorkItem.h              → exit 0
g++ -std=c++14 -pedantic-errors -fsyntax-only LogicWorkDispatcher.h        → exit 0
C++17 语法探针（结构化绑定）在 -std=c++14 下被正确拒绝                       → 无 C++17 语法混入
```

依赖隔离：

```text
MeetingDomain 的 include 集合：仅 meeting/*.h + <chrono|map|string|vector|functional|utility|cstddef>
MeetingDomain 链接内容：无（target_link_libraries(MeetingDomain ...) 不存在）
libMeetingDomain.a 未定义符号中 grpc/hiredis/mysql/jsoncpp/boost/asio/pthread = 0
ChatServer link line 含 libMeetingDomain.a：YES（M4 的目标之一）
测试可执行文件 ldd：not found = 0，/mnt 第三方路径 = 0
compile_commands.json 中 /mnt 路径出现次数：0
本步未引入任何新包管理依赖
```

Boost.UUID：`LogicWorkDispatcher.cpp` 使用仓库既有的 Boost（`boost::uuids`，`CSession.cpp`
本就在用 `boost::uuids::random_generator`），未引入新的第三方 UUID 库。
它只出现在**集成层**，`MeetingService` 内部仍然只通过注入的 `std::function` 使用 ID 与时间。

**历史遗留（如实记录）**：`CMakeCache.txt` 中仍存在
`re2_DIR:PATH=/mnt/f/conda/Library/lib/cmake/re2`。该残留当前未生效（Ubuntu 的
`re2::re2` imported target 在 CMakeLists 中显式提供并优先命中），因此
`compile_commands.json` 里没有任何 `/mnt` include 路径。这是 Step 2.4 §4.2 已记录的
既有脆弱点（R7），M4 未处理。

---

## 16. 旧文件编码保持结果

`LogicSystem.h` / `LogicSystem.cpp` 是 legacy GBK/CP936 系编码文件。修改前先做了只读
编码与字节检查并保存 baseline SHA256：

```text
LogicSystem.h   : 50 lines, 1837 bytes, charset=us-ascii,  non-ASCII bytes = 0
                  SHA256 3776846e67e653728946c4e69926af10fd964d3caf87602619cad7c62de61427
LogicSystem.cpp : 639 lines, 18948 bytes, charset=iso-8859-1, non-ASCII bytes = 912
                  SHA256 41040b7ca0f71866bd28494b28d2e7a4002e42805ed6946628acb8740406f448
换行           : 两个文件都只有 bare LF（CRLF = 0）
```

round-trip 无损性（修改前验证）：

```text
iconv -f GBK  -t UTF-8 | iconv -f UTF-8 -t GBK  → cmp 与原文逐字节相同（LOSSLESS）
iconv -f CP936 -t UTF-8 | iconv -f UTF-8 -t CP936 → 同样逐字节相同
```

采用的方法（byte-preserving）：

1. 用已验证无损的 GBK 解码定位与替换**仅目标区段**；
2. 以 GBK 编码写回；
3. 对整文件做运行验证（构建 + 全部测试），确认新字节序列是有效 GBK。

结果（M4 实施当时的约束状态）：

```text
LogicSystem.h   : 仍然纯 ASCII（新增内容全部 ASCII 注释），换行形式不变
LogicSystem.cpp : 当时仍然 charset=iso-8859-1（GBK 字节），换行形式不变（bare LF）
```

### 16.1 评审后续：按要求把 `LogicSystem.cpp` 转为 UTF-8（人工授权）

M4 实施时，任务规则要求"不得为了加入中文注释而把旧文件批量转 UTF-8"。评审阶段
项目负责人**明确授权**改变该文件的编码，使 `LogicSystem.cpp` 与仓库内其它文件统一为
UTF-8。该转换已执行，并且满足下列性质：

```text
转换方式          : GBK 严格解码（无损）→ 重新编码为 UTF-8，不带 BOM，换行保持 bare LF
非 ASCII 内容位置 : 全部位于行注释中（791 个字符）；字符串/字符字面量与代码部分为 0
纯注释性证明      : 去掉注释后的 token 流完全一致（5434 个 token），
                    194 个字符串字面量内容完全一致 => 运行时行为零变化
中文注释保全      : 转换前后"含中文注释的行"逐行完全一致（64 行）
构建与测试        : 全新 out-of-tree 构建 PASS（仅既有 MysqlDao.cpp:95 告警）；
                    193/193 测试通过；直接运行 exit 0
```

**注意：本次只转换了 `LogicSystem.cpp`。** 其余 legacy 文件（`CSession.*`、`CServer.cpp`、
`UserMgr.cpp`、`RedisMgr.*`、`MysqlDao.*`、`ConfigMgr.*`、`MsgNode.cpp`、
`AsioIOServicePool.*`、`ChatGrpcClient.h`、`ChatServiceImpl.cpp`、`const.h`）**仍为 GBK**，
本次未改动、用户也未要求。因此仓库当前处于"新旧文件编码不同"的过渡状态；若后续决定
统一，应作为一次独立、显式的任务处理（见 §17.2 对 Windows 侧的影响）。

逐字节核对：

```text
baseline 中的非 ASCII 字节片段共 70 段
修改后缺失 5 段 —— 全部位于被删除的旧 DealMsg 区段内（字节 11459–13010）
删除区段之外缺失 0 段（即没有破坏任何既有中文注释或字符串字节）
```

被删除的 5 段正是旧 `DealMsg` 里描述"判断队列为空则用条件变量等待"、
"判断如果为关闭状态…"、"如果没有停服…"等旧实现的注释；它们随旧实现一起被替换为
新分派路径的注释。没有发生任何批量转码，也没有引入 mojibake。

新增文件一律 UTF-8（无 BOM）：

```text
LogicWorkItem.h / LogicWorkDispatcher.h / LogicWorkDispatcher.cpp / LogicWorkQueueTest.cpp
```

因此 `LogicSystem.cpp` 中的新增注释也随原文件一起以 **GBK** 落盘，符合"新增中文注释落盘
时应使用原文件兼容的编码"；`LogicSystem.h` 是纯 ASCII，GBK 与 ASCII 等价。

---

## 17. Windows fallback 与 R6

必须严格区分两件事：

```text
historical Windows fallback（Step 2.4 之前验证过的 Debug|x64 基线）
  : 历史上 PASS（46 warnings），本次**未重新构建、未重新验证**

current dev HEAD 的 Windows 构建
  : NOT VERIFIED —— 本次没有在 Windows 上构建过任何东西
```

M4 的改动与 Windows 工程的关系：

- 未修改 `.sln` / `.vcxproj` / `.props`（本次 `git status` 中这三个模式零命中）；
- 未访问任何旧 Windows checkout、`/mnt/c`、`/mnt/d` 下的项目副本；
- **不得**因为 Linux 编译成功就声称最新 `dev` 已通过 Windows 构建。

### 17.1 R6 现状（仍然开放且范围扩大了）

```text
grep -rn "meeting" --include='*.vcxproj' --include='*.props' Server/   → 无命中
```

即 M1–M3 的四个领域文件与一个测试文件**都不在** Windows 工程里；M4 又新增了
`LogicWorkItem.h`、`LogicWorkDispatcher.h/.cpp`、`LogicWorkQueueTest.cpp`，
并且 `LogicSystem.h` 现在还 `#include "LogicWorkDispatcher.h"`。

结论：**当前 dev HEAD 在 Windows 上无法编译**（`LogicSystem.h` 引用了未加入工程的
`LogicWorkDispatcher.h`）。这不是 M4 引入的新问题，而是 R6 从"Meeting Core 未接线"
升级为"LogicSystem 也依赖未接线的头文件"。补齐 Windows 工程条目需要一次性加入：

```text
meeting/MeetingTypes.h
meeting/MeetingAggregate.h  meeting/MeetingAggregate.cpp
meeting/MeetingService.h    meeting/MeetingService.cpp
LogicWorkItem.h
LogicWorkDispatcher.h       LogicWorkDispatcher.cpp
tests/LogicWorkQueueTest.cpp（若要在 Windows 上跑测试）
```

并按任务要求：这属于授权范围外的跨平台任务，M4 不自行扩大。**已如实报告该问题。**

### 17.2 编码与 Windows 的一个附带事实

编码事实（已按 §16.1 更新）：`LogicSystem.cpp` 与全部 M4 新增文件现在都是 **UTF-8**，
因此 `LogicSystem.cpp` 这一侧不再存在"UTF-8 头文件被按 GBK 解读"的 C4819 隐患。

但 `LogicSystem.cpp` 仍然 `#include` 若干 **GBK** 的 legacy 头文件
（`CSession.h` / `const.h` / `ConfigMgr.h` / `RedisMgr.h` / `MysqlDao.h` / `data.h` 等）。
反过来看：在 MSVC 中文 locale 下，若把此 TU 加入 Windows 工程，UTF-8 的 `.cpp` 与 GBK 的
`.h` 之间仍会出现编码混用，可能产生 C4819 类告警。彻底消除这一混用状态需要一次性决定
整个 Server 目录的源文件编码策略，属于 R6 的后续工作，M4 及本次编码调整都不做。

---

## 18. 已知限制

1. **`LogicSystem` 侧没有单元测试覆盖。** `LogicSystem.cpp` 不在测试 target 内，
   因为它会引入 Redis / MySQL / gRPC 整条业务链路。因此：
   - legacy 消息"从 `PostMsgToQue` 到 `_fun_callbacks`"的端到端路径由
     **静态 diff 审核 + 逐句对齐**保证，不由单元测试保证；
   - `RouteExternalMessage` 的实际回调分发由**运行时 smoke**间接覆盖（进程能正常起停、
     `LogicSystem` 的 worker 与析构路径正常），但不覆盖任何真实 IM 业务。
2. **legacy IM 完整业务链路未验证。** 无 MySQL server、无 schema、未执行任何语句，
   未解析任何协议帧，未调用任何 gRPC 方法。
3. **M4 未接入真实 `CSession` 认证/断线路径**：`INTERNAL_SESSION_*` 与
   `SESSION_DISCONNECTED` 目前只有 typed 入队入口，真实捕获属于 M5。
4. **没有协议级验证**：TCP 只证明监听与连接建立，不解析帧。
5. **`notification_session_ids` 仍是意图值**，没有发送，也没有 exactly-once 网络投递承诺。
6. **ClosedSnapshot retention/TTL 未实现**（Deferred）：只有手工 `EvictClosedSnapshot`。
7. `MysqlDao.cpp:95` 既有告警保持原样（唯一告警）。
8. `re2_DIR` cache 残留（R7）未处理，见 §15。
9. `Server/ChatServer2/` 与 Qt 客户端未触碰。

---

## 19. Runtime smoke

用 Step 2.4 的已验证方法，在仓库外临时目录执行（目录已删除）。最终代码上共做两轮，
第二轮（`BUILD_TESTING=OFF` 的产物）使用 18082 / 15053 / 16381：

```text
仓库外临时目录 + 合成 config.ini（未读取、未复制真实配置；密码仅用于本机 loopback 临时实例）
专用 Redis 实例（bind 127.0.0.1, --save ''）
端口预检：三个端口开始前全部空闲（未杀任何无关进程；系统 Redis 6379 全程未触碰）
SIGTERM（未使用 kill -9）
不执行任何 MySQL 业务
```

结果（最终冻结代码上复跑，使用 18083 / 15054 / 16382）：

```text
ChatServer process starts                 : PASS（ps 确认 comm=ChatServer，工作目录 = config.ini 所在目录）
TCP listener                              : PASS（预检空闲端口，监听出现）
gRPC listener [::ffff:127.0.0.1]          : PASS
Redis 注册 logincount_lfh / <server name>  : PASS（值为 "0"）
SIGTERM → 进程自行退出，exit_status = 0    : PASS
listeners disappear                       : PASS（两个端口均消失）
Redis 注册清理（EXISTS = 0）               : PASS
shutdown 日志中无 terminate/abort/exception : PASS
专用 Redis 实例关闭                        : PASS
系统 Redis (127.0.0.1:6379) 未被触碰        : PASS（全程 PONG）
```

（端口号在三次 smoke 中分别为 18080/15051/16379、18081/15052/16380、
18082/15053/16381、18083/15054/16382，均为开始前经预检确认空闲的临时端口。）

> 第一次尝试曾把 `cd <dir> && ChatServer &` 作为一条命令后台化，于是 `$!` 拿到的是
> 子 shell 的 pid，SIGTERM 只杀掉了子 shell，看起来像"监听未消失、Redis 未清理"。
> 第二次改为先 `cd` 再单独后台启动，`$!` 才是真实进程 pid，结论如上。
> 记录这一点是因为它正是"smoke 结论必须可溯源"的反例。

该 smoke **只证明 startup/shutdown regression**：

```text
它不能替代 §13 的 Work Queue 测试，也不能声称 legacy login（Redis + MySQL 业务）已验证。
```

---

## 20. 验证边界（严格区分）

```text
Legacy IM callback dispatch compatibility:
VERIFIED BY TEST SEAM
（① ILegacyMessageRouter 接口层：legacy 消息确实被路由到既有分发路径，且位置与顺序正确；
   该测试使用记录型 router 替身，因此证明的是"路由路径 + 参数/顺序"而不是真实业务 handler；
  ② LogicSystem 的 RouteExternalMessage 与旧 DealMsg 内联代码逐句对齐；
  ③ RegisterCallBacks 与五个业务方法经静态 diff 确认逐字未改；
  ④ 运行期由 §19 的 smoke 间接覆盖 worker 与析构路径）

Legacy IM full business workflow:
NOT VERIFIED
（无 MySQL、无真实 token、无协议帧、无 gRPC 调用，未做任何端到端登录/好友/文本聊天验证）
```

---

## 21. Scope Review 结论

```text
修改：LogicSystem.h / LogicSystem.cpp / CMakeLists.txt / tests/CMakeLists.txt
新增：LogicWorkItem.h / LogicWorkDispatcher.h / LogicWorkDispatcher.cpp
      tests/LogicWorkQueueTest.cpp / docs/phase2/M4-unified-serialized-work-item.md
删除：无

未修改（逐项核对）：
  meeting/MeetingTypes.h  meeting/MeetingAggregate.*  meeting/MeetingService.*
  CSession.*  CServer.*  UserMgr.*  RedisMgr.*  MysqlMgr.*  MysqlDao.*
  ChatGrpcClient.*  ChatServiceImpl.*  MsgNode.*  const.h  message.proto
  *.sln / *.vcxproj / *.props
  Server/ChatServer2/  MyChat_Qt/
无 build artifacts / 无 config.ini / 无日志 / 无密码进入仓库
```

---

## 22. M5 前置条件

M5 接入真实断线路径时，必须消费 M4 已固定的接口与顺序：

```text
1. 网络侧只能通过 LogicSystem::PostSessionDisconnectedWork(session_id) 投递，
   不得直接调用 MeetingService。
2. 必须在 CSession::Close() / CServer::ClearSession() **之前**捕获稳定 session_id 并投递
   （R3；Step 1.6 §13），因为投递之后进程内不得再依赖 CSession 存活。
3. 不得为断线事件编造 TCP msg_id，也不得新建第二条队列或第二个 worker。
4. Meeting mutation 的唯一执行上下文仍然是 SerializedWorkExecutor 的消费者线程。
5. 关闭顺序依赖：已受理的 Close 会在 drain 阶段补完 Finalize，M5 不应绕过该 drain 语义。
6. M5 若需要新增 kind（例如"transport 已停止"这类状态），应沿用显式 enum + value payload 形态，
   不得引入 std::function<void()> 任务队列或 work item 多态基类。
```

---

## 23. 明确未实现

```text
CSession / CServer 断线捕获与投递                       → M5
响应编码（JSON/protobuf）与通知发送                     → M6
TCP Meeting Protocol / message.proto / message ID      → Phase 3
Windows .vcxproj 接线（R6，范围已扩大，见 §17.1）        → 未决定由哪一步补齐
ClosedSnapshot retention / TTL / 淘汰策略               → Deferred
持久化（Redis / MySQL）业务路径重写                      → 不实现
```
