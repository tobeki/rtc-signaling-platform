# Phase 2 - M3：`MeetingService` 与 In-Memory Registry

本文件记录 M3 的实现范围、设计裁决、验证结果与明确的未实现内容。

M3 是 **Domain-first 阶段的最后一个 Coding Task**。完成之后：

```text
M1 MeetingTypes
+ M2 MeetingAggregate
+ M3 MeetingService / In-Memory Registry
= 可以完全脱离 TCP / LogicSystem / CSession / Redis / MySQL / gRPC 独立运行的 Meeting Core
```

依赖方向（构建上被强制，而不是靠约定）：

```text
MeetingService
      ↓
MeetingAggregate
      ↓
MeetingTypes
      ↓
C++ standard library
```

---

## 1. 目标

在 `Server/ChatServer/ChatServer/meeting/` 下新增 `MeetingService`，把 M2 的**单个** Meeting 聚合提升为**多 Meeting** 管理与 Phase 1 正式 API 应用服务：

| 组成 | 内容 |
| --- | --- |
| active meeting registry | `meeting_id → MeetingAggregate` |
| closed snapshot registry | `meeting_id → ClosedMeetingSnapshot` |
| Domain Session records | `session_id → DomainSessionRecord` |
| session → binding reverse index | `session_id → [ (meeting_id, user_id) ]` |
| 六项正式 API | `CreateMeeting` / `JoinMeeting` / `LeaveMeeting` / `CloseMeeting` / `QueryMeeting` / `ListParticipants` |
| `SessionDisconnected` fan-out | 0 / 1 / N 个 Meeting 独立裁决 |
| `FinalizeClose` | 独立 direct domain operation（第二个 serialized turn） |
| local domain event output | 每次操作直接返回本次产生的事件值 |

M3 仍然**不接 ChatServer 网络路径**：`ChatServer` 可执行文件依旧不链接 `MeetingDomain`，接入属于 M4。

---

## 2. 非目标

M3 未实现、且未触碰以下任何内容：

```text
LogicSystem.*        CSession.*        CServer.*        UserMgr.*
Redis*               Mysql*            ChatGrpc*        message.proto
TCP message ID       Protocol V2       Qt / MyChat_Qt
HTTP / WebSocket / MQ
*.vcxproj / *.props  config.ini
```

同时明确不引入：

- 新线程、线程池、strand、async task、timer、background worker；
- `std::mutex` / `std::recursive_mutex` / `std::shared_mutex`；
- 持久化（Redis / MySQL）、RPC、事件总线、MQ、ACK/retry；
- 第二套领域库（第二套 executable 亦未创建）。

---

## 3. `MeetingService` Ownership

**ordinary object，不是 Singleton，也不是 thread-safe object。**

```cpp
MeetingService(const ServerId& local_server_id,
               MeetingServiceDependencies dependencies);
```

- 每次构造都得到一份互不共享的内存 registry；两个实例之间没有任何全局状态
  （`MeetingServiceSession.TwoInstancesHaveIndependentRegistries` 锁定该性质）。
- `MeetingService` 自己**不加锁**。所有 mutation 必须由外部 single-writer serialized
  boundary 顺序调用；M4 才真正提供该统一 FIFO 边界。头文件中已用中文注释明确该约束。
- M3 的测试通过**直接调用** typed method 来模拟 serialized turn（Layer A + Layer B 顺序场景），
  不引入队列、线程或 fake dispatcher。

---

## 4. Deterministic Dependencies

Phase 1 冻结的正式 `CreateMeeting` 请求**没有** `meeting_id` 业务字段，客户端不得指定会议 ID；
而单元测试又必须可重复。因此 M3 使用标准库 dependency injection：

```cpp
struct MeetingServiceDependencies {
    std::function<MeetingId()> next_meeting_id;
    std::function<EventId()>   next_event_id;
    std::function<Timestamp()> now;
};
```

- `MeetingService` 业务方法内部**不得**出现 `boost::uuids`、`system_clock::now()`、全局序列号或 `rand()`；
- 单元测试注入固定序列（`meeting-1` 起、`event-1` 起、`epoch + N` 秒，且时钟只在测试显式推进时变化）；
- M4 之后的真实集成可注入真实 ID 与 clock 来源，而 API 形状不变。

`MeetingId` 冲突（注入来源产出空 ID、已 active ID 或已 CLOSED ID）一律为 `INTERNAL_ERROR`：
不得覆盖已有 Meeting、不得静默返回旧 Meeting、不得按 `request_id` 去重。

---

## 5. Domain Session Registry

`DomainSessionRecord` 只表达纯领域事实：

```text
session_id
DomainSessionState
optional authenticated user_id（presence flag + value）
```

绝不包含 `CSession*`、`shared_ptr<CSession>`、socket 或 `io_context`。

生命周期入口：

```cpp
bool RegisterConnectedSession(const SessionId& session_id);   // new → CONNECTED
bool AuthenticateSession(const SessionId& session_id, UserId user_id); // → AUTHENTICATED
bool TryGetDomainSessionState(const SessionId& session_id, DomainSessionState& out) const;
```

状态转换严格按 Step 1.3 §5：

```text
CONNECTED ──auth success──→ AUTHENTICATED
CONNECTED ──SessionDisconnected──→ CLOSING → CLOSED
AUTHENTICATED ──SessionDisconnected──→ CLOSING → CLOSED
CLOSED 是终态（CLOSED → * 非法）
```

实现裁决（记录在代码注释中）：

- `AuthenticateSession` 只接受 `CONNECTED → AUTHENTICATED`；重复认证不改状态（Step 1.3 §5.3）；
- `AuthenticateSession` 拒绝非正数 `UserId`，避免登记"已认证但身份为 0"这一无法与任何
  context 匹配的退化记录；
- 这些方法**不是 TCP adapter**，只是 pure-domain state transition。M5/M6 才把真实
  `CSession` 生命周期接进来。

### 5.1 CLOSED Session Tombstone

M3 保留 **minimal CLOSED tombstone**，理由与 Phase 1 一致：

1. 重复 `SessionDisconnected` 可以明确幂等；
2. 已排入 serialized queue 的 stale command 在 disconnect 之后仍能得到
   `SESSION_STATE_REJECTED`（而不是被误报为 `AUTH_REQUIRED`）；
3. 不把已 `CLOSED` 的 Session 错报成未认证。

tombstone 不保留 socket、网络对象或 Meeting Binding。其 retention / reclamation policy
为 **Deferred**：M3 不实现 TTL、timer 或清理线程。

---

## 6. Active Meeting Registry

- `std::map<MeetingId, MeetingAggregate>`；只在**稳定状态**（某次 mutation 完整结束后）发布。
- 不向调用者暴露容器、`MeetingAggregate*` 或迭代器；所有对外投影都是值类型
  （`MeetingView` / `ParticipantView`）。
- 聚合只通过私有 `FindActiveMeeting()` 在内部定位；返回的指针绝不交给外部调用者。

---

## 7. Closed Snapshot Registry

- `std::map<MeetingId, ClosedMeetingSnapshot>`。
- 与 active registry **互斥**：稳定状态下同一个 `meeting_id` 不会同时以
  "active aggregate" 与 "CLOSED snapshot" 两种权威来源存在。
- CLOSED 的 `QueryMeeting` / `ListParticipants` **只能**从 snapshot 构造，绝不通过已释放的
  活动 Participant 推导。
- 淘汰钩子：

```cpp
bool EvictClosedSnapshot(const MeetingId& meeting_id);
```

它只表达"快照已被 retention layer 淘汰"，之后 Query / List 返回 `MEETING_NOT_FOUND`。
**M3 不实现任何自动策略**（无 TTL、无 timer、无 background cleaner）。

---

## 8. Reverse Index

```text
session_id → [ BindingRef{ meeting_id, user_id } ... ]
```

必要性来自 Step 1.3 §13.2：`SessionDisconnected(session_id)` 必须能枚举该 Session 当前关联的
**全部** BOUND Meeting Binding。该索引与 `UserMgr` 无关。

只保存稳定 ID，绝不保存 `Participant*` / `Binding*` / `MeetingAggregate*` / `CSession*`。

不变式：同一 `(session_id, meeting_id, user_id)` **最多一条** ref。

维护点与实现位置：

| 时机 | 动作 | 位置 |
| --- | --- | --- |
| Create | 登记 host session → `(meeting_id, host_user_id)` | `CreateMeeting` 第 8 步 |
| Join NEW_PARTICIPANT | 登记当前 session ref | `JoinMeeting` |
| Join REJOINED_PARTICIPANT | 登记当前 session ref | `JoinMeeting` |
| Join ADDITIONAL_SESSION_BOUND | 登记新的 session ref | `JoinMeeting` |
| Join ALREADY_BOUND | **不新增** ref（去重插入本身也不会产生重复） | `JoinMeeting` |
| Join 被拒绝（如 ENDING） | 不改索引 | `JoinMeeting` |
| Leave 成功 | 删除该 `(meeting_id, user_id)` 对应的**全部** Session refs | `LeaveMeeting` |
| Leave 幂等 / 被拒绝 | 不改索引 | `LeaveMeeting` |
| SessionDisconnected | 删除该 session 的全部 refs | `SessionDisconnected` 第 4 步 |
| FinalizeClose | 删除该 Meeting 的全部剩余 refs | `FinalizeClose` 第 3 步 |

### 8.1 实现过程中发现的一个真实缺陷

最初的 `JoinMeeting` 只按 `JoinResult::outcome` 判断是否登记 ref。但聚合在 state gate
（`ENDING`/`CLOSED` → `MEETING_STATE_REJECTED`）上提前返回时**没有改写** `outcome`，
它仍保留 `JoinOutcome::NEW_PARTICIPANT` 这一默认值。结果是一条被拒绝的 Join 会往反向索引里
塞进一个并不存在的 Binding ref。

修正：登记条件必须以**真实成功**为准。

```cpp
const bool binding_established =
    joined.result_code == ResultCode::OK &&
    (outcome == NEW_PARTICIPANT || REJOINED_PARTICIPANT || ADDITIONAL_SESSION_BOUND);
```

该缺陷由 `MeetingServiceJoin.J8JoinEndingMeetingIsRejected` 与
`MeetingServiceReverseIndex.RejectedJoinDoesNotTouchIndex` 两个用例捕获并锁定。
（`JoinOutcome` 的取值集合由 Phase 1 冻结为 4 个，不能为"被拒绝"新增成员，因此修正在
service 侧而不是在聚合侧扩展枚举。）

---

## 9. 六项 API

六项 API 全部遵循 Step 1.4 §19.1 的八步求值顺序：

```text
1. Validate Request Context
2. Validate Domain Session usability
3. Validate API Arguments
4. Resolve local Meeting / ClosedSnapshot / locality
5. Perform resource-scoped authorization
6. Evaluate state and idempotency
7. Apply mutation
8. Produce response / event
```

### 9.1 Context 结构校验（步骤 1）

```text
request_id 非空
session_id 非空
handling_chat_server_id 非空
handling_chat_server_id == MeetingService.local_server_id
has_device_id == true                → device_id 非空
has_trusted_owner_chat_server_id == true → trusted owner 非空
```

任何一条不满足 → `INVALID_ARGUMENT`。

**刻意不**因为 `authenticated_user_id == 0` 就提前返回 `INVALID_ARGUMENT`：否则未认证 Session
永远得不到正式的 `AUTH_REQUIRED`。

### 9.2 Domain Session 可用性（步骤 2）

| 情形 | 结果 |
| --- | --- |
| Session 记录不存在 | `AUTH_REQUIRED` |
| `CONNECTED`（未认证） | `AUTH_REQUIRED` |
| `CLOSING` / `CLOSED` | `SESSION_STATE_REJECTED`（**不得**误报为 `AUTH_REQUIRED`） |
| `AUTHENTICATED` 但 `record.user_id != context.authenticated_user_id` | `INVALID_ARGUMENT`（server-built context 自相矛盾，不得信任 context 声称的身份） |

### 9.3 API 参数校验（步骤 3）

`JoinMeeting` / `LeaveMeeting` / `CloseMeeting` / `QueryMeeting` / `ListParticipants` 的
`meeting_id` 只要求**非空**；空值 → `INVALID_ARGUMENT`，且必须发生在任何 registry lookup
与 locality 判断**之前**。刻意不发明 UUID regex、固定长度或 wire encoding 约束。

`CreateMeeting` 无业务 payload，因此没有这一步。

### 9.4 Locality 解析（步骤 4）

先查 active registry，再查 closed snapshot registry；本地均不存在时才检查
`trusted_owner_chat_server_id`：

| 情形 | 结果 |
| --- | --- |
| 本地 active 存在 | 本地解析成功 |
| 本地 closed snapshot 存在 | 本地解析成功（"已 CLOSED"不等于"不存在"） |
| 本地都没有 + 有可信 owner 且 `owner != handling_chat_server_id` | `MEETING_NOT_LOCAL`（结果携带该 owner） |
| 本地都没有 + 无可信 owner | `MEETING_NOT_FOUND` |
| 本地都没有 + 可信 owner == 本节点 | `MEETING_NOT_FOUND` |

`MeetingRequestContext` 只有服务端可信的 `trusted_owner_chat_server_id`，**没有** client owner
hint 字段：未经验证的客户端提示根本无法进入这条 trusted path。M3 不新增该字段。

### 9.5 Result Disposition 映射（步骤 8）

唯一的集中映射（`ResultDispositionFor`），六个 API 共用，避免各自复制一套易漂移的判断：

```text
OK                                                  → SUCCESS
ALREADY_JOINED / ALREADY_LEFT /
CLOSE_IN_PROGRESS / ALREADY_CLOSED                  → IDEMPOTENT
其余                                                → ERROR
```

`result_name` 一律来自 `ResultCodeName(result_code)`，不维护第二套字符串映射。

### 9.6 MeetingView 填充规则（本地实现裁决）

只有在**授权通过**（或幂等命中，而幂等命中本身已意味着授权通过）时才填充 `MeetingView`；
授权失败（`PERMISSION_DENIED` / `NOT_PARTICIPANT`）时保持默认空值。

理由：Phase 1 未要求在这些失败路径上返回视图，Step 1.4 §16.4 明确"按可见规则返回或不返回"，
因此这里选择不向非成员泄露会议摘要（成员数 / `host_user_id`），与 §26.2
"未加入过会议的用户不得借此枚举成员"的取向一致。

---

## 10. Create

```text
1 context validation
2 domain session usability
3 （无业务参数校验）
4 生成 MeetingId / EventId / Timestamp
5 确认生成的 meeting_id 不与 active / closed 冲突
6 MeetingAggregate::Create(...)
7 publish active registry
8 登记 host reverse index
9 return response + MeetingCreated
```

成功结果：

```text
OK / SUCCESS
MeetingView(CREATED)、count = 1、closed_at 为空
Host ParticipantView(HOST, ACTIVE, left_at 空)
events = [ MeetingCreated ] 恰好一次
```

**不产生** `ParticipantJoined`：Host 的建立不构成"加入一个已存在的会议"。

`request_id` 只做 correlation。两次 `CreateMeeting(request_id = X)` 会创建两个**不同**的
Meeting、两次都返回 `OK`、各自产生一个独立的 `MeetingCreated`（`event_id` 不同）。

---

## 11. Join

`JoinMeeting` 没有独立的身份门槛（Join 本身就是建立成员关系），因此步骤 5 不适用；
流程为 context → session → argument → locality/state gate → `MeetingAggregate::Join`
→ reverse index → response。

四种 outcome 保持 M2 语义：

| outcome | 结果码 / disposition | 计数 | 事件 | 索引 |
| --- | --- | --- | --- | --- |
| `NEW_PARTICIPANT` | `OK` / `SUCCESS` | +1 | `ParticipantJoined` ×1 | 新增 ref |
| `REJOINED_PARTICIPANT` | `OK` / `SUCCESS` | +1 | `ParticipantJoined` ×1（新 `event_id`） | 新增 ref |
| `ADDITIONAL_SESSION_BOUND` | `OK` / `SUCCESS` | 不变 | 无 | 新增 ref |
| `ALREADY_BOUND` | `ALREADY_JOINED` / `IDEMPOTENT` | 不变 | 无 | 不新增 |

- CLOSED snapshot 命中 → `MEETING_STATE_REJECTED`（不调用已释放的 aggregate）；
- ENDING → 由聚合的 state gate 返回 `MEETING_STATE_REJECTED`；
- `ParticipantJoined.meeting_state_after` 固定为 `ACTIVE`（首个非 Host 激活与
  `CREATED → ACTIVE` 在同一个 mutation 内完成）。

---

## 12. Leave

Leave 是 **User 级**逻辑离会：成功时解除该 User 在本 Meeting 下的**全部** Binding。

关键顺序：**身份/历史成员判定先于状态暴露**。

| 顺序 | 条件 | 结果 | disposition |
| --- | --- | --- | --- |
| 1 | 从未是该 Meeting Participant（含 ENDING / CLOSED） | `NOT_PARTICIPANT` | `ERROR` |
| 2 | 已知成员 + `ENDING` | `MEETING_STATE_REJECTED` | `ERROR` |
| 3 | 已知历史成员 + `CLOSED` | `ALREADY_LEFT` | `IDEMPOTENT` |
| 4 | Host + `CREATED`/`ACTIVE` | `HOST_MUST_CLOSE_MEETING` | `ERROR` |
| 5 | 已知成员当前 `LEFT` + `CREATED`/`ACTIVE` | `ALREADY_LEFT` | `IDEMPOTENT` |
| 6 | `ACTIVE` 普通成员 + `CREATED`/`ACTIVE` | `OK` + `LEFT`，计数 −1，`ParticipantLeft` ×1 | `SUCCESS` |

第 1 步必须最先执行：一个**从未加入过**的用户对 `ENDING` Meeting 执行 Leave，按
Step 1.4 §20.2 应为 `NOT_PARTICIPANT`，不能因为聚合先看到 `ENDING` 就泄露
`MEETING_STATE_REJECTED`。因此 Service 层先判定历史 Participant 是否存在，再暴露状态。

CLOSED 分支只读取 snapshot：`Host` 与任何历史 Participant 都是 `ALREADY_LEFT`（幂等），
未参加者为 `NOT_PARTICIPANT`；任何情况下都不修改 snapshot、不重建 aggregate、不产生事件。

---

## 13. Close / FinalizeClose

`CloseMeeting` 与 `FinalizeClose` 是**两个不同的 serialized domain operation**，
各自占据独立的 queue turn（Step 1.4 §16.5）。

### 13.1 CloseMeeting（只执行 BeginClose）

```text
resolve aggregate → verify Host → aggregate.BeginClose(...)
```

| 调用者 / 状态 | 结果 | disposition | 响应 meeting_state | should_schedule_finalize |
| --- | --- | --- | --- | --- |
| 非 Host（任意状态） | `PERMISSION_DENIED` | `ERROR` | 不返回视图 | `false` |
| Host + `CREATED`/`ACTIVE` | `OK` + `CLOSE_STARTED` | `SUCCESS` | `ENDING` | `true` |
| Host + `ENDING` | `CLOSE_IN_PROGRESS` | `IDEMPOTENT` | `ENDING` | `false` |
| Host + `CLOSED` | `ALREADY_CLOSED` | `IDEMPOTENT` | `CLOSED` | `false` |

- 权限先于幂等：非 Host 在 `ENDING`/`CLOSED` 下同样是 `PERMISSION_DENIED`，
  绝不能通过幂等结果推断 Host-only 的内部状态；
- CLOSED 分支的权限只依据 snapshot 中的 `host_user_id`；历史普通 Participant 即使曾经在会中，
  也只能得到 `PERMISSION_DENIED`；
- `BeginClose` **不产生任何事件**，`CloseMeeting` **绝不**在本方法内调用 `FinalizeClose`。

### 13.2 FinalizeClose（第二个 serialized turn，不是外部 API）

逻辑顺序（代码顺序可直接读出）：

```text
1 resolve active ENDING aggregate
2 obtain frozen CloseContext（值拷贝）
3 cleanup reverse-index Binding refs for this Meeting
4 build complete ClosedMeetingSnapshot
5 closed_snapshots_[meeting_id] = snapshot;      ← publish
6 produce MeetingClosed exactly once
7 capture notification target session IDs from frozen CloseContext
8 active_meetings_.erase(meeting_id);            ← release
```

**发布 snapshot 先于释放 active aggregate**（Step 1.3 §18.2）：不得先 erase 再尝试构造
snapshot，否则会出现"活动聚合已释放 + Snapshot 尚不存在"的可观测空窗。

从方法返回后必然满足：`active meeting absent` + `closed snapshot present`。

结果语义：

| 情形 | `finalized` | `already_finalized` | events | notification_session_ids |
| --- | --- | --- | --- | --- |
| 首次合法 finalize | `true` | `false` | `MeetingClosed` ×1 | 输出一次 |
| 重复 finalize（active 已释放、snapshot 存在） | `false` | `true` | 空 | 空（不得第二次输出通知意图） |
| active 为 `CREATED`/`ACTIVE`（内部调度错误） | `false` | `false` | 空 | 空（**不关闭**该 Meeting） |
| Meeting 完全不存在 | `false` | `false` | 空 | 空 |

由于它不是外部 API，`CREATED`/`ACTIVE` 上收到 FinalizeClose 只返回内部 not-finalized 结果，
**不**把它伪装成客户端可见的 `MEETING_STATE_REJECTED` response。

`MeetingClosed` 字段：

```text
event_type          = MEETING_CLOSED
actor_user_id       = Host（has_actor_user_id = true）
participant_user_id 不填写
meeting_state_after = CLOSED
request_id          = CloseContext.originating_request_id（correlation，不是幂等键）
occurred_at         = Finalize 处理时间
```

### 13.3 通知

`FinalizeCloseResult.notification_session_ids` 来自 `CloseContext.notifiable_session_ids`，
只在首次 finalize 返回一次。M3 **不执行任何发送**：

- 不引入 notification callback、fake network sender 或 adapter 接口；
- 因此不存在"发送失败阻止 CLOSED"的结构，Finalize 的成功与任何外部副作用无关；
- M6 才负责 `session_id → CSession lookup → best-effort send`。

---

## 14. Query / List

| API | 状态 | 授权 | 数据来源 | scope |
| --- | --- | --- | --- | --- |
| QueryMeeting | `CREATED`/`ACTIVE`/`ENDING` | Host 或当前 `ACTIVE` Participant | 活动聚合 | — |
| QueryMeeting | `CLOSED` | Host 或任何历史 Participant | **只**从 ClosedMeetingSnapshot | — |
| ListParticipants | `CREATED`/`ACTIVE`/`ENDING` | 同 QueryMeeting | `ListCurrentParticipantViews()`，只列 `ACTIVE` | `CURRENT` |
| ListParticipants | `CLOSED` | 同 QueryMeeting | `snapshot.participant_identities` | `HISTORICAL` |

- 已 `LEFT` 的历史成员在开放 Meeting 上返回 `NOT_PARTICIPANT`（不得因历史记录存在就继续查看
  CURRENT 会议）；`ENDING` 下当前 `ACTIVE` 成员仍可读，但不能修改 membership；
- CLOSED 视图：`meeting_state = CLOSED`、`active_participant_count = snapshot.final_participant_count`、
  `created_at` 保留、`has_closed_at = true`、`closed_at` 保留；
- 历史 ParticipantView：`has_left_at == true → LEFT`，否则 `ACTIVE`。这里的 `ACTIVE` 只表示
  **其在 Meeting 关闭边界上仍为 ACTIVE**，不是 Presence / Session 在线状态；
- 同一个 User 不因多个 Device/Session 出现多次；不设计分页；
- Query / List 完全只读：不改变 Meeting 状态、成员数、Binding、反向索引、事件或 Snapshot。

`ParticipantViewScope` 定义在 `MeetingService.h`，只有 `CURRENT` / `HISTORICAL`：它是
**domain API enum**，不是 wire encoding，也不承诺任何数值编码。

---

## 15. SessionDisconnected Fan-Out

```cpp
SessionDisconnectedResult SessionDisconnected(const SessionId& session_id);
```

全部动作在**一次** serialized mutation 内完成（Step 1.3 §13.2）：

```text
Domain Session → CLOSING
→ copy 反向索引中受影响的 refs
→ for each affected Meeting（各 Meeting 独立裁决）:
      解除本 Session 在该 Meeting 的精确 Binding（幂等 UNBOUND）
      普通 Participant：剩余 > 0 → 保持 ACTIVE
                        剩余第一次变为 0 → ACTIVE → LEFT、count −1、ParticipantLeft ×1
      Host：即使变为 0 → 保持 ACTIVE，不产生事件，不关闭 Meeting（I4）
      Meeting 为 ENDING/CLOSED → 只做必要的幂等 UNBOUND，不改 membership、不产生事件
→ 清除该 session 的反向索引
→ 只有全部受影响 Meeting 处理完成后，Domain Session 才 → CLOSED
```

- **0 / 1 / N** fan-out 全部覆盖（0 表示已认证但未加入任何会议，只完成 Session 领域终止）；
- 每个 Meeting 独立裁决：一个 Meeting 的 `ParticipantLeft` 不影响另一个 Meeting 的 membership；
- `ParticipantLeft` 的产生粒度是 `(meeting_id, user_id)`，每个真实发生 `ACTIVE → LEFT` 的组合
  最多一次；
- 重复 `SessionDisconnected` 整体幂等（`CLOSED` 是终态）；
- fan-out 完成后该 session 的反向索引必须为空；
- 已被 finalize 并释放的 Meeting 不再被触碰（其 refs 已在 Finalize 时清除）。

### 15.1 聚合侧的 M3 扩展

`MeetingAggregate` 新增一个**内部单 Binding** mutation：

```cpp
DisconnectSessionResult DisconnectSession(UserId user_id,
                                          const SessionId& session_id,
                                          const MutationParams& params);
```

只解除 `(user_id, session_id)` 这一条 Binding，**不是**外部 Leave API；fan-out 到哪些 Meeting
由 `MeetingService` 负责。结果表达 `binding_changed` / `participant_became_left` /
`MeetingView` / 可选 `ParticipantLeft`，不引入新的 `ResultCode`。

### 15.2 内部事件不得伪造 correlation

M2.1 已留下扩展点，M3 落实：`MakeEvent` 的 `request_id` 形参扩展为
`(bool has_request_id, const RequestId&)`。

| 事件来源 | `has_request_id` | `has_actor_user_id` |
| --- | --- | --- |
| Create / Join / Leave（外部命令） | `true` | `true` |
| SessionDisconnected 导致的 `ParticipantLeft`（系统清理） | `false` | `false`（`participant_user_id` 必须存在） |

### 15.3 一个需要记录的实现细节：EventId 分配粒度

fan-out 对每个受影响的 active Meeting 都会向注入来源取一个 `EventId`，**即使该次裁决最终
不产生事件**（例如 Host 断线、`ENDING` 下的幂等清理）。事件身份本身始终由注入来源决定，
聚合不生成 ID；被消耗但未被使用的 ID 不产生任何领域副作用。测试因此通过记录生成序列来断言
唯一性，而不是假定"第 k 个事件必然使用第 k 个 ID"。

---

## 16. Event Semantics

```text
request_id is correlation only
dedupe_context is not an idempotency key
```

| 事件 | 产生条件 | `actor_user_id` | `participant_user_id` | `meeting_state_after` | `request_id` |
| --- | --- | --- | --- | --- | --- |
| `MeetingCreated` | `CreateMeeting` 首次成功，恰好一次 | Host | 不填 | `CREATED` | 有 |
| `ParticipantJoined` | 仅 `NEW_PARTICIPANT` / `REJOINED_PARTICIPANT` | 该 User | 该 User | `ACTIVE`（固定） | 有 |
| `ParticipantLeft` | 仅在真实发生 `ACTIVE → LEFT` 时 | 用户主动时为其 User；断线清理时**为空** | 该 User | 当前 Meeting 状态 | 主动 Leave 有；断线清理**为空** |
| `MeetingClosed` | 仅第一次成功 `FinalizeClose` | Host | 不填 | `CLOSED` | 最初那次 Close 的 request_id |

不得产生事件的路径：重复 Join / 重复 Leave / 重复 Disconnect / 重复 Close / 重复 Finalize /
被拒绝的路径 / `BeginClose` 本身 / 幂等 cleanup。

M3 明确**不**建立：`request_id` 去重表、`dedupe_context` 去重表、persistent EventRepository、
EventBus、MQ。Phase 1 事件的语义是 **local domain fact output**，不是可靠消息系统。

---

## 17. Snapshot Atomic Publication

代码中可直接读出：

```cpp
closed_snapshots_[meeting_id] = snapshot;   // 5. publish
...
active_meetings_.erase(it);                 // 8. release
```

因此任何查询都不可能观察到：

```text
Meeting 已 CLOSED 但 Snapshot 只组装了一半
活动聚合已被删除但 Snapshot 尚未建立
伪完整的 CLOSED 状态
```

`FinalizeClose` 的返回值本身即是顺序证据：`finalized == true` 时 snapshot 必然已经可读，
且 active aggregate 必然已经释放。

---

## 18. Invariants

I1–I12（Step 1.3 §23.1）在 M3 中的落点：

| # | 不变量 | M3 中的保证方式 |
| --- | --- | --- |
| I1 | `Meeting == CLOSED` ⇒ 无有效会议 Binding | Finalize 前清理该 Meeting 的全部反向 refs；CLOSED 后不再被 Disconnect 触碰 |
| I2 | `Participant == LEFT` ⇒ 不属于活动成员集合 | 聚合 `participant_state` 为唯一权威；CURRENT 投影只列 ACTIVE |
| I3 | 普通 `ACTIVE` Participant ⇒ 至少一个有效会议 Session | `SessionDisconnected` 在**同一 mutation** 内完成解绑与 `ACTIVE → LEFT` |
| I4 | Host `ACTIVE` ⇒ 可以有 0 个有效 Session | `DisconnectSession` 对 Host 提前返回，不改 membership |
| I5 | Binding 有效 ⇒ Domain Session 为 `AUTHENTICATED` | 六项 API 的步骤 2 拒绝 `CONNECTED`/`CLOSING`/`CLOSED` |
| I6 | `ENDING`/`CLOSED` ⇒ 不创建新逻辑 Participant | 聚合 state gate + CLOSED 路由到 snapshot |
| I7 | `(meeting_id, user_id)` 唯一 | 复用 M2 的 `std::map<UserId, ParticipantRecord>` |
| I8 | 同一 Participant + session 最多一条有效 Binding | 复用 M2 `BindSession`；反向索引去重插入 |
| I9 | `CLOSED` ⇒ 存在完整 Snapshot | snapshot 发布先于 active 释放；二者在同一 mutation 内完成 |
| I10 | `participant_count` == 活动成员集合基数 | 计数只在聚合的 Join/Leave/Disconnect 内增减 |
| I11 | 非 Host 只在真实 activation 时变 ACTIVE | Binding 变化不直接改写 membership |
| I12 | `is_active(p) == (participant_state == ACTIVE)` | `ParticipantView` 由聚合状态单向派生 |

索引侧补充不变量（M3 引入）：

```text
稳定状态下，任一条反向索引 ref 对应的 Domain Session 必须为 AUTHENTICATED；
SessionDisconnected 内部允许 CLOSING + 尚未清理的 BOUND，但 mutation 返回前必须全部清理，
且该暂态不得暴露给 Query。
```

---

## 19. Tests

新增 `Server/ChatServer/ChatServer/tests/MeetingServiceTest.cpp`，继续复用同一个
`ChatServerUnitTests` executable（**未**创建第二套 executable）。

全部用例使用注入的固定 ID 序列与固定时间点，不依赖真实 clock 或随机 UUID，因此可重复。

| 测试组 | 数量 | 覆盖 |
| --- | --- | --- |
| `MeetingServiceSession` | 18 | CONNECTED → `AUTH_REQUIRED`、AUTHENTICATED 可用、CLOSED → `SESSION_STATE_REJECTED`、重复 disconnect 幂等、fan-out 完成后才 CLOSED、context 结构校验、tombstone 语义、非 Singleton |
| `MeetingServiceCreate` | 10 | C1–C7、ID 冲突（active / closed / 空 ID）、`MeetingCreated` exactly once、无 `ParticipantJoined` |
| `MeetingServiceJoin` | 9 | J1–J9：四种 outcome、`CREATED → ACTIVE`、ENDING/CLOSED 拒绝、索引唯一性 |
| `MeetingServiceLeave` | 13 | L1–L12：成功 Leave、全 Binding 解绑、计数 −1、`ALREADY_LEFT`、`NOT_PARTICIPANT`、Host、ENDING/CLOSED 身份区分、快照只读 |
| `MeetingServiceClose` | 13 | X1–X11：`ENDING`、`CLOSE_STARTED`、唯一 Finalize 意图、`CLOSE_IN_PROGRESS`、snapshot、`MeetingClosed`、`ALREADY_CLOSED`、非 Host 权限、通知无依赖 |
| `MeetingServiceFinalizeClose` | 7 | 发布先于释放、重复 finalize、`CREATED`/`ACTIVE` 不关闭、未知 Meeting、冻结尾边界、refs 清理 |
| `MeetingServiceQuery` | 9 | Q1–Q6：`CREATED`/`ACTIVE`/`ENDING`/`CLOSED`、历史成员授权、未参加拒绝、淘汰 → `MEETING_NOT_FOUND`、只读性 |
| `MeetingServiceListParticipants` | 7 | CURRENT vs HISTORICAL、多 Session 单次出现、未参加拒绝、淘汰 |
| `MeetingServiceLocality` | 6 | O2/O3/O4：可信异地 owner、无可信 owner、可信 owner == 本地、本地优先 |
| `MeetingServiceOrdering` | 7 | O1/O5：参数先于 lookup、权限先于幂等、context → session → argument 优先级、disposition 映射 |
| `MeetingServiceDisconnect` | 14 | D1–D8、0/1/N fan-out、每 Meeting 独立裁决、事件计数、ENDING 只解绑、重复幂等、Leave 后 disconnect、`CONNECTED → CLOSED` |
| `MeetingServiceReverseIndex` | 9 | create/join/leave/disconnect/finalize 五个维护点、无重复 ref、拒绝路径不动索引 |
| `MeetingServiceEventContract` | 5 | `request_id` 仅 correlation、`dedupe_context` 非幂等键、`state_after` 规则、`MeetingClosed` 关联、BeginClose 无事件 |
| **M3 合计** | **127** | |

真实运行结果：

```text
Step 1.6 §19 规划的 57 个语义用例：全部覆盖
M3 新增 TEST 数量：127
既有测试（Step 2.5 / M1 / M2 / M2.1）：41（未删除、未修改）
CTest 总数：168
CTest 结果：100% tests passed, 0 tests failed out of 168
直接运行 ChatServerUnitTests：168 tests from 24 test suites，exit 0
```

---

## 20. Dependency Isolation

`MeetingDomain` 只新增 `meeting/MeetingService.cpp`，仍然**不链接任何东西**：

```text
target_link_libraries(MeetingDomain ...)   → 不存在
libMeetingDomain.a members                 → MeetingAggregate.cpp.o, MeetingService.cpp.o
forbidden undefined symbols
  (grpc / protobuf / hiredis / mysql / jsoncpp / boost / pthread / asio) → NONE
```

`std::function` 属标准库，允许使用（`libMeetingDomain.a` 中只出现
`std::__throw_bad_function_call()` 这类标准库符号）。

`ldd tests/ChatServerUnitTests`：

```text
not found = 0
/mnt 第三方路径 = 0
依赖：libstdc++ / libm / libgcc_s / libc / ld-linux / linux-vdso
```

`ChatServer` 可执行文件的 link line 中**没有** `libMeetingDomain`：网络路径接入仍属于 M4。

`BUILD_TESTING=OFF` 时 configure 与 build 均通过（`gtest` 相关输出为 0 行），
`ChatServer` 与 `MeetingDomain` 均 PASS，GoogleTest 不是必需依赖。

---

## 21. 明确未实现

```text
LogicSystem queue / unified serialized work item      → M4
CSession / CServer adapter                            → M5
TCP / Protocol V2 / message.proto                     → Phase 3
notification sending（session_id → CSession → send）  → M6
ClosedSnapshot retention / TTL / 淘汰策略             → Deferred
Client owner hint                                     → 不引入
snapshot_version 对外暴露                             → 不要求
持久化（Redis / MySQL）                               → 不实现
```

已知未修问题（与本步无关，保持原样）：`MysqlDao.cpp:95` 的
`control reaches end of non-void function [-Wreturn-type]` 告警。

---

## 22. 下一步

```text
M4 — Unified Serialized Work Item
```

把六项 API、`SessionDisconnected`、`FinalizeClose` 统一接入 `LogicSystem`
single-writer serialized boundary，并让 `ChatServer` 真正链接 `MeetingDomain`。

M4 之前不得合并其他 prompt。
