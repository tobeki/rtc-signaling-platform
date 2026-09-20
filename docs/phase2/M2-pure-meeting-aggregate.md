# M2 — Pure `MeetingAggregate`

状态：**已完成**（改动留在 working tree，等待人工审阅与提交）

---

## 1. 目标

M2 第一次实现 Meeting 的**核心状态机与单聚合业务规则**：一个 Meeting 内部的一致性。

新增：

```
Server/ChatServer/ChatServer/meeting/MeetingAggregate.h
Server/ChatServer/ChatServer/meeting/MeetingAggregate.cpp
```

它负责：

```
Meeting state（CREATED / ACTIVE / ENDING）
Host identity
Participant collection（UserId 唯一）
Participant ↔ Session Binding collection
historical Participant identity（LEFT 后不丢弃）
Create 初始状态
Join / Re-Join / duplicate Join / additional Session Binding
Leave
BeginClose
CloseContext 冻结材料
query projection（MeetingView / ParticipantView）
ClosedSnapshot 所需 material
```

这是一个 **Pure Domain Task**，不是网络集成任务。

---

## 2. 范围与非目标

### 明确不属于 M2

```
MeetingService
MeetingRegistry
ClosedSnapshotRegistry
session → meetings 反向索引
SessionDisconnected fan-out
LogicSystem 队列
FinalizeClose work item 调度
TCP response
protobuf / HTTP / WebSocket
Redis / MySQL / gRPC
```

特别注意两点：

1. **M2 不实现 `MeetingService`。**
2. **M2 不完成 `ENDING → CLOSED` 的 registry 生命周期。** 聚合本身不会进入
   `CLOSED`；`BuildClosedSnapshot()` 只根据冻结尾边界**生成材料**，不发布、不改状态。
   原子发布与活动聚合释放属于 M3。

### 设计输入

`docs/phase1/STEP-1.2-domain-model.md`、`STEP-1.3-state-machines.md`、
`STEP-1.4-api-contract.md`、`STEP-1.5-core-sequences.md`、
`STEP-1.6-implementation-plan-and-test-strategy.md`，以及
`docs/phase2/M1-meeting-domain-value-types.md`。

本步未推翻任何 Phase 1 已冻结语义。

---

## 3. Aggregate 内部模型

### 3.1 Meeting

| 字段 | 说明 |
| --- | --- |
| `meeting_id_` | 由调用方提供 |
| `host_user_id_` | Host 身份 |
| `owner_chat_server_id_` | 运行态所有者节点 |
| `meeting_state_` | 聚合自身只会经历 `CREATED → ACTIVE → ENDING` |
| `created_at_` | 由调用方提供 |
| `active_participant_count_` | ACTIVE 逻辑成员数 |
| `has_activated_non_host_` | "是否已发生过首个非 Host 激活"，用于保证不可回退 |

### 3.2 Participant 存储

`std::map<UserId, ParticipantRecord>` + 一个记录首次加入顺序的
`std::vector<UserId>`，后者用来保证投影与快照输出**可重复**。

按 `UserId` 唯一定位，满足：

```
(meeting_id, user_id) 最多一个逻辑 Participant   （I7）
```

没有为了存放 Participant 而建立 Repository class。

### 3.3 Binding 存储

`ParticipantRecord::bindings` 是一个 `std::vector<BindingRecord>`。每个
`BindingRecord` 只保存：

```
session_id        稳定 ID
binding_state     BOUND / UNBOUND
bound_at          绑定时间（调用方提供）
has_device_id     可选
device_id         仅在 has_device_id 时有效
```

**不保存** `shared_ptr<CSession>`、`CSession*` 或 `socket*` —— Binding 只保存稳定
ID/value。

同一 `session_id` 的重新绑定会**复用**已有的 UNBOUND 记录，而不是不断堆叠，因此
"同一 Participant + session 最多一条 BOUND Binding"（I8）成立。

### 3.4 历史记录保留

成功 `Leave` 后，`ParticipantRecord` **不会**被删除，只把
`participant_state` 置为 `LEFT` 并记录 `left_at`。这样同一份记录可以支撑：

- duplicate Leave 的幂等判断；
- Re-Join（复用同一条逻辑成员，`joined_at` 不重置）；
- `ClosedMeetingSnapshot` 的历史身份；
- 历史身份授权。

没有建立额外的数据库式 archive layer。

### 3.5 DeviceId

`MutationParams` / `CreateMeetingParams` 携带 `has_device_id`。只有为 true 时才
保存 `device_id`；否则**不伪造**。Device 不参与 Participant 唯一性。

---

## 4. Deterministic Inputs

Aggregate 内部**禁止**自行产生：

```
system_clock::now()
random UUID
Boost random
全局序列号
```

需要的时间与标识全部由调用方显式传入：

```cpp
struct MutationParams {
    Timestamp now;
    EventId event_id;
    bool has_device_id = false;
    DeviceId device_id;
};
```

理由：让测试具备 `deterministic / repeatable / fast` 三个性质，并让"事件发生时间"
成为可断言的外部输入，而不是隐藏副作用。

**实测结论**：`clock generation inside aggregate: NO`；`random/UUID generation
inside aggregate: NO`（审计命令见 §15）。

如果某次操作最终不产生事件（例如被拒绝的路径、幂等命中路径），传入的 `event_id`
不得造成任何副作用 —— 有专门的测试锁定这一点。

---

## 5. Create

构造聚合时一次性完成：

```
meeting_state = CREATED
Host Participant = ACTIVE
Host 初始 Session Binding = BOUND
active_participant_count = 1
```

并产生 **MeetingCreated 恰好一次**。

关键差异（Step 1.4 §13.3.1、Step 1.5 §6.1）：

> **Create 不产生 `ParticipantJoined`。**

Host 虽然是 `ACTIVE` Participant，但 Host 的建立**不构成"加入"**，因此 Meeting 停留
在 `CREATED`（Step 1.3 §8.2）。

`meeting_id` / `event_id` / `created_at` 全部由调用方提供，聚合不生成。

---

## 6. Join — 四种 outcome

共用一次 mutation，按固定求值顺序：**先 state gate，再判幂等，最后才 mutate**。

| Case | 条件 | outcome | ResultCode | disposition | 逻辑成员数 | 事件 |
| --- | --- | --- | --- | --- | --- | --- |
| A | 该 User 无历史记录 | `NEW_PARTICIPANT` | `OK` | `SUCCESS` | +1 | `ParticipantJoined` ×1 |
| C | 该 User 已 `ACTIVE`，新 session | `ADDITIONAL_SESSION_BOUND` | `OK` | `SUCCESS` | 不变 | 无 |
| B | 该 User 已 `ACTIVE`，同一 session 已 BOUND | `ALREADY_BOUND` | `ALREADY_JOINED` | `IDEMPOTENT` | 不变 | 无 |
| 5 | 该 User 历史为 `LEFT`，重新加入 | `REJOINED_PARTICIPANT` | `OK` | `SUCCESS` | +1 | `ParticipantJoined` ×1 |

### 6.1 `CREATED → ACTIVE`

只由**第一个非 Host 成员的真实逻辑激活**触发，且与 Participant 的激活落在同一个
mutation 内。因此不存在"Participant 已 ACTIVE 但 Meeting 仍 CREATED"的稳定可见状态。

以下行为**不触发**该转换（Step 1.3 §8.3）：

- Host 初始 Binding；
- Host 用第二个 Session Join；
- Host 重复 Join；
- 被拒绝的 Join。

并且**不可回退**（Step 1.3 §8.5）：即使所有非 Host 成员后续全部 `Leave`，Meeting
也不回到 `CREATED`。这一点由 `has_activated_non_host_` 显式保证，并有专门测试。

### 6.2 Re-Join

```
LEFT → ACTIVE
新 session → BOUND
count +1
ParticipantJoined ×1（新的 event_id）
joined_at 保持首次逻辑加入时间，不重置
has_left_at = false   ← 旧离会时间不得继续表现为 ACTIVE 的当前 left_at
```

### 6.3 附加 Session

Participant 保持 `ACTIVE`，成员数与 Meeting 状态都不变，不产生
`ParticipantJoined`。Host 的附加 Session 语义相同，且**不改变 Meeting 状态**。

### 6.4 重复 Join

完全幂等：无 mutation、无计数变化、无事件。幂等来自**领域状态**，不是
`request_id` 去重。

### 6.5 `ENDING` 下的 Join

一律 `MEETING_STATE_REJECTED` / `ERROR`，无 mutation、无事件。

`CLOSED` 下的 Join 留给 M3 的终态 registry 路径：本聚合不进入 `CLOSED`。

---

## 7. Leave

Leave 是 **User 级**离会。

| 调用者 / 状态 | ResultCode | disposition | mutation | 事件 |
| --- | --- | --- | --- | --- |
| 普通 `ACTIVE` Participant，`CREATED`/`ACTIVE` | `OK` + `LEFT` | `SUCCESS` | `ACTIVE → LEFT`；该 User 全部 Binding `UNBOUND`；count −1 | `ParticipantLeft` ×1 |
| 已 `LEFT` 的历史 Participant | `ALREADY_LEFT` | `IDEMPOTENT` | 无 | 无 |
| 从未加入 | `NOT_PARTICIPANT` | `ERROR` | 无 | 无 |
| Host，`CREATED`/`ACTIVE` | `HOST_MUST_CLOSE_MEETING` | `ERROR` | **无** | 无 |
| 任意身份，`ENDING`（含 Host） | `MEETING_STATE_REJECTED` | `ERROR` | 无 | 无 |

Host 在开放会议调用 `Leave` 时：

- 不把 Host 置为 `LEFT`；
- 不解绑 Host Binding；
- 不减成员数；
- 不产生 `ParticipantLeft`。

`ENDING` 是**外部 API 的 state gate**：内部 close cleanup 不是 Leave，不走这条路径。

---

## 8. BeginClose

### 8.1 求值顺序：permission before idempotency / state exposure

这是本步最容易写错的地方。正确顺序是：

1. **先判权限**：`caller != host_user_id_` → `PERMISSION_DENIED` / `ERROR`，
   无论当前是 `CREATED`、`ACTIVE` 还是 `ENDING`。
2. 再判 `ENDING` → `CLOSE_IN_PROGRESS` / `IDEMPOTENT`。
3. 再判 `CLOSED` → `ALREADY_CLOSED` / `IDEMPOTENT`。
4. 最后才做首次 mutation。

如果先判 `ENDING`，非 Host 就能通过 `CLOSE_IN_PROGRESS` **推断出 Host-only 的内部
状态**。有专门测试锁定非 Host 在 `ENDING` 下仍得到 `PERMISSION_DENIED`。

### 8.2 首次 Host Close

```
meeting_state: CREATED/ACTIVE → ENDING
ResultCode = OK
disposition = SUCCESS
close_outcome = CLOSE_STARTED
should_schedule_finalize = true
```

- **不执行 `FinalizeClose`**；
- response 中的 MeetingView 仍然是 `ENDING`；
- 不产生 `MeetingClosed`。

### 8.3 重复 Host Close

```
ResultCode = CLOSE_IN_PROGRESS
disposition = IDEMPOTENT
should_schedule_finalize = false
has_close_context = false
```

不得再冻结第二个 `CloseContext`、不得再安排第二个 `FinalizeClose`、不得产生
`MeetingClosed`。冻结材料保持第一次的值（`originating_request_id`、
`close_started_at` 不变），有测试验证。

因此"每个 Meeting 从 `CREATED`/`ACTIVE` 第一次进入 `ENDING` 时最多安排一个有效
`FinalizeClose`"这一约束由聚合状态自然保证。

---

## 9. CloseContext

`CloseContext` 定义在 `MeetingAggregate.h`（没有塞回 M1 的 `MeetingTypes.h`），
属于 domain/application 之间的交接类型。它是**值对象**：

| 字段 | 用途 |
| --- | --- |
| `meeting_id` | 定位待关闭 Meeting |
| `host_user_id` | 终态快照中的 Host 身份与事件 `actor_user_id` |
| `owner_chat_server_id` | 事件与快照的归属节点 |
| `originating_request_id` | 与最初 Close command 建立 correlation（**不是幂等键**） |
| `close_started_at` | `closed_at` 与审计 |
| `participant_identities` | 完整历史成员身份（**含已 LEFT 的历史成员**） |
| `notifiable_session_ids` | 冻结时仍为 `BOUND` 的 session 集合 |
| `frozen_active_participant_count` | 冻结边界上的 ACTIVE 成员数 |

**不保存** `MeetingAggregate*`、`Participant*`、`Binding*` 或 `CSession*`。

`notifiable_session_ids` 只收集仍 `BOUND` 的 session：已离会成员的 session 不再是
可通知目标，有测试验证。

---

## 10. Snapshot Material

`BuildClosedSnapshot(const CloseContext&, const Timestamp& closed_at)`：

```
meeting_state        = CLOSED
created_at           保持原值
closed_at            由调用方提供（M2 不在聚合内取时间）
participant_identities = 冻结的历史身份（完整）
final_participant_count = 冻结边界上的 ACTIVE 成员数
```

调用它**不发布 snapshot、不改变本聚合状态** —— 聚合仍停在 `ENDING`。

### 10.1 `final_participant_count` 的正式语义（重要）

```
final_participant_count
    = BeginClose / close snapshot boundary 时 ACTIVE Participant 的逻辑数量
```

它**不是**"历史上曾加入过的 participant 总人数"。

例：

```
Host ACTIVE
A    LEFT
B    ACTIVE
```

则：

```
participant_identities.size() = 3
final_participant_count       = 2
```

也就是说：

> **`participant_identities.size()` 与 `final_participant_count` 不是同一概念。**

这个语义同时写入测试（`MeetingAggregateSnapshot` 与 `MeetingAggregateCloseContext`
两组各有一个测试显式断言 `final_participant_count != participant_identities.size()`）。

---

## 11. Invariants

| 编号 | 不变量 | M2 状态 |
| --- | --- | --- |
| I2 | Participant `LEFT` → 不计入 active participant count | 保持（有测试） |
| I3 | ACTIVE 非 Host Participant → 至少一条 BOUND Binding | 保持（有测试） |
| I4 | ACTIVE Host → 允许零条 Binding | 模型不禁止（M2 尚不做 disconnect） |
| I6 | `ENDING` → 不创建新 Participant | 保持（Join 被拒，有测试） |
| I7 | 同一 Meeting + user → 最多一个 Participant | 保持（Re-Join 复用记录，有测试） |
| I8 | 同一 Participant + session → 最多一条 BOUND Binding | 保持（重绑复用记录） |
| I10 | `active_participant_count` == ACTIVE Participant 数量 | 保持（有测试） |
| I11 | 非 Host 只在真实 activation 时 `ACTIVE` | 保持 |
| I12 | `ParticipantView` ACTIVE ↔ `participant_state == ACTIVE` | 保持（构造时互斥赋值） |
| I1 / I9 | 终态 `CLOSED` 发布 | 属于 M3 |

---

## 12. Threading Boundary

```
MeetingAggregate itself is not thread-safe.
Mutation must come through serialized single-writer caller.
```

聚合内部**没有** `std::mutex`，也**没有**任何 thread-safety 包装。这不是缺陷，而是
Phase 1 冻结的架构边界：Meeting mutation 由外部 single-writer serialization 保证。

（`Connection = meeting.connection` 之类的细化留给 M4；M2 只是把这条边界写成代码事实。）

---

## 13. CMake / `MeetingDomain` target

`Server/ChatServer/ChatServer/CMakeLists.txt` 新增：

```cmake
add_library(MeetingDomain STATIC
    meeting/MeetingAggregate.cpp
)

target_compile_features(MeetingDomain PUBLIC cxx_std_14)
target_include_directories(MeetingDomain PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
```

依赖方向：

```
MeetingDomain
    ↓
MeetingTypes
    ↓
C++ standard library only
```

`MeetingDomain` **没有** `target_link_libraries` —— 它不链接 gRPC、protobuf、
hiredis、MySQL、JsonCpp、Boost，也**不链接 Threads**。

**ChatServer executable 目前刻意不链接 `MeetingDomain`**：在 M4 之前没有任何网络
适配代码消费 `MeetingAggregate`，为了形式强行接线只会制造假依赖。

`tests/CMakeLists.txt` 把新测试源加入既有 `ChatServerUnitTests`，并让该目标链接
`MeetingDomain`：

```cmake
target_link_libraries(ChatServerUnitTests PRIVATE
    GTest::gtest_main
    MeetingDomain
)
```

没有创建第二个 GoogleTest executable。

---

## 14. Unit Tests

新增 `tests/MeetingAggregateTest.cpp`，共 **29** 个用例，全部为 Phase 1 **Layer A
Pure Domain**。加上 Step 2.5 与 M1 的 6 个，CTest 总数 **35**。

| 分组 | 用例数 | 覆盖的分支 |
| --- | --- | --- |
| `MeetingAggregateCreate` | 3 | 初始 `CREATED`、Host `ACTIVE`、Host Binding `BOUND`、count=1、投影正确、Create 不产生 `ParticipantJoined` |
| `MeetingAggregateJoin` | 9 | 四种 outcome 全部分支、`CREATED → ACTIVE`、不可回退、Host 附加/重复 Session、`ENDING` 拒绝 |
| `MeetingAggregateLeave` | 5 | 成功 Leave（全部解绑、count −1、事件 ×1）、幂等、非成员、Host 拒绝、`ENDING` 拒绝 |
| `MeetingAggregateBeginClose` | 6 | 从 `CREATED`/`ACTIVE` 关闭、`CLOSE_STARTED`、`should_schedule_finalize`、view 仍 `ENDING`、无 `MeetingClosed`、重复 Close、非 Host 拒绝（含 `ENDING` 下） |
| `MeetingAggregateCloseContext` | 2 | 冻结身份字段、只收 BOUND session、保留 LEFT 历史成员、冻结 ACTIVE 数 |
| `MeetingAggregateSnapshot` | 2 | 从冻结边界构造快照、`final_participant_count != identities.size()`、不改变聚合状态 |
| `MeetingAggregateDeterminism` | 2 | 输出完全使用调用方提供的值、被拒绝路径的未用 `event_id` 无副作用 |
| `MeetingAggregateInvariants` | 2 | I7/I10/I3、跨重绑保持 I3 |

### 执行结果

```
ctest -N                        → Total Tests: 35（保留 TestInfrastructureSmoke + MeetingTypesContract）
ctest --output-on-failure       → 100% tests passed, 0 tests failed out of 35
                                   exit code 0
```

直接运行测试二进制（工作目录为空、无 `config.ini`）：`[ PASSED ] 35 tests`，退出码 0。

### 刻意未实现的测试

Phase 1 规划的 57 个场景覆盖 M2/M3 两段，本步只覆盖聚合当前负责的分支。**没有**
创建 `TEST(..., TODO)` 或 `EXPECT_TRUE(true)` 占位用例。

`request_id` 重复 `Create` 返回不同 `meeting_id` 属于 Service/ID 生成层，**不在 M2
测试**，也未伪造。

---

## 15. Dependency Isolation

| 检查 | 结果 |
| --- | --- |
| `ldd ChatServerUnitTests` → `not found` | **0** |
| `ldd` → `/mnt` 解析 | **无** |
| `ldd` 实际依赖 | 仅 `libstdc++` / `libm` / `libgcc_s` / `libc` |
| 测试二进制是否引入 grpc/protobuf/hiredis/mysql/jsoncpp/boost | **全部 absent** |
| `MeetingDomain` 的 `link.txt` | 只有 `ar`/`ranlib`，**没有任何库** |
| `MeetingDomain` 目标文件 | 只有 `MeetingAggregate.cpp.o` |
| `MeetingDomain` 编译标志 | `-g` + 项目 include 根，无其他 |
| `MeetingDomain` 未定义符号中 redis/mysql/grpc/protobuf/jsoncpp/boost/pthread/asio | **0** |
| `MeetingDomain` 未定义符号实际内容 | 仅 libstdc++（`std::string`/`_Rb_tree`）与 libc（`memcpy` 等） |
| `ChatServerUnitTests` 链接行 | `libMeetingDomain.a` + `libgtest_main.a` + `libgtest.a` |

**关于运行中的 Redis**：环境里有一个 `redis-server` 在 `127.0.0.1:6379` 运行。它是
Step 2.4 中由 `redis-server` **包管理自动 enable 的 systemd 系统服务**（WSL 重启后
自动恢复），不是本步或测试的依赖 —— 测试二进制根本不链接 hiredis，也不读任何
`config.ini`，且从空目录运行全部通过。

聚合内部时间/随机性审计：

```
grep -nE 'system_clock::now|rand|uuid|random_generator' meeting/MeetingAggregate.cpp
→ 无匹配

grep -nE 'mutex|thread|condition_variable|asio|socket|CSession|CServer|LogicSystem|Singleton|shared_ptr'
→ 无匹配
```

---

## 16. 明确未实现

```
MeetingService
MeetingRegistry
ClosedSnapshotRegistry
session → meetings 反向索引
SessionDisconnected fan-out
FinalizeClose 执行与 work item 调度
ENDING → CLOSED 的 registry 原子发布
LogicSystem / CSession / CServer 集成
TCP / protobuf / HTTP / WebSocket
Redis / MySQL / gRPC
```

另外，M2 新增 Linux CMake target 与 Meeting 文件，Windows `.vcxproj` 不引用它们：

```
Windows regression build: N/A
Windows fallback baseline: PRESERVED
```

未触碰 Windows checkout。

### 一个已识别但未自行扩权的契约缺口

Phase 1（Step 1.5 §7.1、Step 1.4 §23.2）要求领域事件携带
`meeting_state_after`（例如 `ParticipantJoined.meeting_state_after = ACTIVE`）。

但 M1 已冻结的 `MeetingEvent` **没有**这个字段，只有
`event_id / event_type / meeting_id / actor / participant / owner_chat_server_id /
occurred_at / dedupe_context`。

M2 的处理：

- **不**修改 `MeetingTypes.h`（M1 contract 已冻结，本任务明令禁止自行扩大）；
- **不**把状态塞进 `dedupe_context` 之类语义不符的字段；
- 记录为缺口，留待 M3/M6（通知/适配层）在**明确授权**下决定如何补上。

这不是 M2 的阻塞项：M2 的事件规则（何时产生、恰好一次、某些路径不产生）已全部实现
并可测试。

---

## 17. 下一步

只提出，**不开始**：

```
M3 — MeetingService / In-Memory Registry
```
