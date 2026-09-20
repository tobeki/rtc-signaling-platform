# M1 — Meeting Domain Value Types

状态：**已完成**（改动留在 working tree，等待人工审阅与提交）

---

## 1. 目标

M1 只落地 Meeting 领域的**值类型与契约类型**，为后续纯领域实现提供可编译的契约载体。

本步唯一新增的生产文件是：

```
Server/ChatServer/ChatServer/meeting/MeetingTypes.h
```

它负责表达：ID 别名、四个状态机的枚举、结果契约、请求上下文、对外视图、领域事件、
CLOSED 终态快照。

它**不包含**任何业务 mutation。`Join()`、`Leave()`、`Close()`、`FinalizeClose()`、
`SessionDisconnected()`、`GenerateMeetingId()`、`PublishEvent()` 全部属于 M2/M3。

M1 完成后领域状态只允许变成：

```
Meeting Domain Value Types: IMPLEMENTED
MeetingAggregate: NOT STARTED
MeetingService: NOT STARTED
```

---

## 2. 设计输入

本步的字段与取值**全部来自已提交的 Phase 1 文档**，没有自行重新设计：

| 文档 | 本步用到的内容 |
| --- | --- |
| `docs/phase1/STEP-1.2-domain-model.md` | Meeting / Participant / Binding / ServerNode / Event / ClosedMeetingSnapshot 的字段清单（§8–§13） |
| `docs/phase1/STEP-1.3-state-machines.md` | 四个状态机的正式取值：Session §5.1、Meeting §7.1、Participant §9.1、Binding §11.1 |
| `docs/phase1/STEP-1.4-api-contract.md` | `MeetingRequestContext` §5、ResultDisposition §9.2、ResultCode 数值表 §10.1、MeetingView §11、ParticipantView §12、JoinOutcome §14.2、LeaveOutcome §15.2、CloseOutcome §16.3 |
| `docs/phase1/STEP-1.6-implementation-plan-and-test-strategy.md` | M1 任务卡 §25.2、C++ 语言约束 §7、测试分层 §17 |
| `docs/phase2/STEP-2.5-linux-cpp-unit-test-infrastructure.md` | 测试目标边界与 `BUILD_TESTING` 约定 |

Source of Truth 优先级按任务约定执行：仓库现有代码 > 已提交 Phase 1 文档 > 本任务说明。

---

## 3. ID 与 Timestamp 表示

`namespace meeting`（仓库此前不存在已冻结的 Meeting namespace，因此按任务约定新建单层
namespace，不建立多层体系）。

### 3.1 标识别名

```cpp
using UserId = int;

using RequestId = std::string;
using SessionId = std::string;
using MeetingId = std::string;
using ServerId  = std::string;
using DeviceId  = std::string;
using EventId   = std::string;
```

**重要**：这些只是 C++ 内部表示，**不是 Protocol V2 的 wire format 承诺**。未来协议可以
选择完全不同的编码方式。头文件注释中已明确写出这一点，任何文档也不应声称"未来协议必须
使用字符串编码 UUID"。

### 3.2 Timestamp

```cpp
using Timestamp = std::chrono::system_clock::time_point;
```

选择理由：C++11 标准库能力、无第三方领域依赖、能表达领域时间点、不绑定字符串格式或
wire encoding。

M1 **不提供**格式化、解析、时区换算或序列化能力。

---

## 4. Optional 表达

Phase 1 中以下字段具有"可能不存在"的语义：`closed_at`、`left_at`、`device_id`、
trusted owner context、部分 event actor/participant。

M1 采用 **presence flag + value**：

```cpp
bool has_closed_at = false;
Timestamp closed_at;
```

明确**不使用**：`std::optional`、`boost::optional`、sentinel magic value。

理由：

1. 明确兼容 C++14 及更早能力；
2. `MeetingTypes` 保持标准库-only（`boost::optional` 会引入 Boost 依赖）；
3. presence 与真实值不会混淆 —— 一个恰好等于默认构造值的真实时间点，与"从未设置"
   是可区分的（契约测试 `PresenceFlagGovernsFieldValidity` 专门锁定这一点）。

使用规则：**只有 presence flag 决定字段是否有效**。默认构造出的值不得被解释为"已知的
空标识"，也因此所有整数类成员都带有确定的初值，避免出现未初始化成员或"用户 0"的暗示。

---

## 5. 状态与 Outcome（实际实现）

### 5.1 四个状态机

| 枚举 | 取值 | 契约来源 |
| --- | --- | --- |
| `DomainSessionState` | `CONNECTED` / `AUTHENTICATED` / `CLOSING` / `CLOSED` | Step 1.3 §5.1 |
| `MeetingState` | `CREATED` / `ACTIVE` / `ENDING` / `CLOSED` | Step 1.3 §7.1 |
| `ParticipantState` | `ACTIVE` / `LEFT` | Step 1.3 §9.1 |
| `BindingState` | `BOUND` / `UNBOUND` | Step 1.3 §11.1 |

刻意**没有** `JOINED` 进入 Session 状态：Meeting participation 由 Binding 表达。
`MeetingState` 没有 `INITIALIZING` / `PAUSED` / `FAILED` / `DELETED`。
`BindingState` 没有 `PENDING` / `STALE` / `DISCONNECTED`。

### 5.2 角色

`ParticipantRole`：`HOST` / `PARTICIPANT`。

角色与状态**分开定义**，没有合并成一个 enum —— 两者语义正交，合并会让状态机判断容易出错。

### 5.3 结果分类

`ResultDisposition`：`SUCCESS` / `IDEMPOTENT` / `ERROR`。

刻意不使用 HTTP status、gRPC StatusCode 或 `bool success` 代替领域 disposition：`bool`
无法区分"成功"与"幂等但无业务变化"。

### 5.4 操作 outcome

| 枚举 | 取值 | 契约来源 |
| --- | --- | --- |
| `JoinOutcome` | `NEW_PARTICIPANT` / `REJOINED_PARTICIPANT` / `ADDITIONAL_SESSION_BOUND` / `ALREADY_BOUND` | Step 1.4 §14.2 |
| `LeaveOutcome` | `LEFT` | Step 1.4 §15.2 |
| `CloseOutcome` | `CLOSE_STARTED` | Step 1.4 §16.3 |

严格按契约取值，没有为了"enum 看起来完整"而增加成员。幂等与错误结果由
`ResultCode` + `ResultDisposition` 表达，**不另建第二套错误枚举**。

---

## 6. ResultCode（硬冻结，15 个且仅 15 个）

```cpp
enum class ResultCode : int { ... };
```

| 数值 | 名称 | disposition | 语义 |
| --- | --- | --- | --- |
| `0` | `OK` | `SUCCESS` | 成功 |
| `100` | `ALREADY_JOINED` | `IDEMPOTENT` | 同一 Session 已有有效 Binding |
| `101` | `ALREADY_LEFT` | `IDEMPOTENT` | 已知历史成员当前不在活动集合 |
| `102` | `CLOSE_IN_PROGRESS` | `IDEMPOTENT` | Meeting 已在 `ENDING` |
| `103` | `ALREADY_CLOSED` | `IDEMPOTENT` | Meeting 已 `CLOSED` |
| `1000` | `AUTH_REQUIRED` | `ERROR` | Session 未完成认证 |
| `1001` | `SESSION_STATE_REJECTED` | `ERROR` | Session 已 `CLOSING`/`CLOSED` |
| `1002` | `INVALID_ARGUMENT` | `ERROR` | 必要逻辑字段缺失或语义非法 |
| `1100` | `PERMISSION_DENIED` | `ERROR` | 调用者无操作权限 |
| `1101` | `NOT_PARTICIPANT` | `ERROR` | 从未成为该 Meeting Participant |
| `1102` | `HOST_MUST_CLOSE_MEETING` | `ERROR` | Host 对开放 Meeting 调用 Leave |
| `1200` | `MEETING_NOT_FOUND` | `ERROR` | 本节点无活动 Meeting/ClosedSnapshot |
| `1201` | `MEETING_NOT_LOCAL` | `ERROR` | 可信 owner 为其他 ChatServer |
| `1202` | `MEETING_STATE_REJECTED` | `ERROR` | 当前 Meeting State 不允许该操作 |
| `9000` | `INTERNAL_ERROR` | `ERROR` | 领域不变量破坏或不可分类的内部失败 |

**未新增、未改号、未重新编号。**

上面这张表不只是文档：它在测试里被写成 `static_assert`（含"恰好 15 个"的编译期计数），
因此任何改号或新增都会直接编译失败。反向对照已实测（见 §10）。

### 6.1 关于 `ResultCodeName`

Step 1.4 §9.1 要求每个 Response 都带 `result_name`。为了让这条冻结契约在 M1 就能被表达，
`MeetingTypes.h` 提供了唯一的纯查表函数：

```cpp
inline const char* ResultCodeName(ResultCode code);
```

它无状态、无副作用、不参与任何状态判断，不是业务逻辑；契约外的取值返回明确的
`"UNKNOWN_RESULT_CODE"`，而不是静默返回空指针。

---

## 7. Context / View / Event / Snapshot

### 7.1 `MeetingRequestContext`（Step 1.4 §5）

| 字段 | 必需性 |
| --- | --- |
| `request_id` | 必需；仅用于关联，**不是幂等键** |
| `authenticated_user_id` | 必需；业务身份的**唯一**来源 |
| `session_id` | 必需；当前服务端 Session，不得由客户端指定 |
| `handling_chat_server_id` | 必需；实际处理该请求的节点 |
| `device_id` | 可选（presence flag） |
| `trusted_owner_chat_server_id` | 可选（presence flag）；只能由服务端可信来源填充 |

由 transport adapter 构造，不由客户端 payload 填充 —— 客户端不得覆盖这些服务端身份字段。
刻意不包含 `socket`、`CSession*`、`shared_ptr<CSession>` 或 `LogicNode*`，也不包含任何
Redis key。

### 7.2 `MeetingView`（Step 1.4 §11）

`meeting_id`、`owner_chat_server_id`、`host_user_id`、`meeting_state`、
`active_participant_count`、`created_at`，以及可选 `closed_at`（presence flag）。

`CREATED`/`ACTIVE`/`ENDING` 下 `closed_at` 必须为空；`CLOSED` 下必须有效。
不含 Session、Binding、socket、Redis/MySQL 字段或任何内部指针。

### 7.3 `ParticipantView`（Step 1.4 §12）

`user_id`、`role`、`participant_state`、`joined_at`，以及可选 `left_at`（presence flag）。

`joined_at` 保持"首次逻辑加入"语义，不因 Re-Join 重置；`ACTIVE` 时 `left_at` 必须为空。
**不暴露** `binding_id`、`session_binding_ids`、device 列表或 socket。

### 7.4 `ClosedMeetingSnapshot`（Step 1.2 §13.2）

`meeting_id`、`host_user_id`、`owner_chat_server_id`、`meeting_state`（固定 `CLOSED`）、
`created_at`、`closed_at`、`participant_identities`、`final_participant_count`。

历史成员身份由轻量值类型 `ParticipantIdentitySnapshot` 承载：`user_id`、`role`、
`joined_at`，以及可选 `left_at`。

边界：不保存 `CSession`、不保存 Binding 指针、不保存活动 Session、不做 Redis/MySQL
持久化；快照不会使 Meeting 重新激活。

`closed_at` 在此**不使用** presence flag：快照只存在于 `CLOSED` 之后，该字段必然有效。

`snapshot_version` 在 Phase 1 中属于"可选建议字段"。当前实现没有并发保护或观测需求，
因此**没有为了字段数量而加入**。

### 7.5 `MeetingEvent`（Step 1.2 §12）

四类事件用 `EventType` + 单一 value struct 表达，**不建立**继承体系、虚基类、event bus、
dispatcher 或 MQ 抽象：

| 字段 | 必需性 |
| --- | --- |
| `event_id` | 必需 |
| `event_type` | 必需（`MEETING_CREATED` / `PARTICIPANT_JOINED` / `PARTICIPANT_LEFT` / `MEETING_CLOSED`） |
| `meeting_id` | 必需 |
| `actor_user_id` | 可选（presence flag）；系统清理可能无具体发起用户 |
| `participant_user_id` | 可选（presence flag）；未必指向某个受影响成员 |
| `owner_chat_server_id` | 必需 |
| `occurred_at` | 必需 |
| `dedupe_context` | 必需；本文件不规定事件顺序或去重算法 |

Event 在这里**只是 value type**，不包含任何发布逻辑。

---

## 8. 依赖边界

```
MeetingTypes  →  C++ 标准库 only
```

实际 include：

```cpp
#include <chrono>
#include <cstddef>
#include <string>
#include <vector>
```

**未**引入：`CSession.h`、`CServer.h`、`LogicSystem.h`、`RedisMgr.h`、`MysqlMgr.h`、
`MysqlDao.h`、`ChatGrpcClient.h`、`grpc/*`、`protobuf/*`、`hiredis/*`、`json/*`、Boost。

审计命令与结果见 §10。

---

## 9. C++14 验证

因为 `cxx_std_14` 只是 minimum requirement（GCC 13 默认 `gnu++17` 已满足，CMake 不会加
`-std=`），仅靠纪律声明不足以证明兼容 C++14，因此本步额外执行独立 syntax-only 门禁。

探针 TU（临时放在 `/tmp`，已删除）：

```cpp
#include "meeting/MeetingTypes.h"
int main() { return 0; }
```

命令与结果：

```
g++ -std=c++14 -pedantic-errors -fsyntax-only -I Server/ChatServer/ChatServer /tmp/rtc-m1-meeting-types-cxx14-probe.cpp
→ exit code 0

g++ -std=c++14 -pedantic-errors -Wall -Wextra -fsyntax-only -I Server/ChatServer/ChatServer /tmp/rtc-m1-meeting-types-cxx14-probe.cpp
→ exit code 0
```

为确认该门禁确实有效，另做了正对照：包含 `<optional>` 并使用 `std::optional` 的 TU
在 `-std=c++14 -pedantic-errors` 下**编译失败**（`'optional' is not a member of 'std'`）。
因此 `MeetingTypes.h` 的 C++14 兼容性是**被真实验证过的**，不是纪律声明。

M1 代码未使用 `std::optional`、`std::variant`、`std::string_view`、structured bindings、
`if constexpr`、`std::filesystem`、inline variables，以及任何 C++20/23 能力。

---

## 10. Unit Tests

新增契约测试加入**现有** `ChatServerUnitTests` 目标（没有新建第二套测试可执行文件）：

```
Server/ChatServer/ChatServer/tests/MeetingTypesContractTest.cpp
```

`tests/CMakeLists.txt` 仅增加一行源文件（未改动 root `CMakeLists.txt`）。

### 10.1 测试清单

| 测试名 | 验证内容 |
| --- | --- |
| `MeetingTypesContract.ResultCodeNameCoversAllFrozenCodes` | 15 个冻结值的 `result_name` 全覆盖 + 契约外取值有明确 fallback |
| `MeetingTypesContract.OptionalFieldsDefaultToAbsent` | 默认构造时所有可选字段均为"不存在" |
| `MeetingTypesContract.PresenceFlagGovernsFieldValidity` | 只写值不置 presence 时字段仍视为不存在；置 presence 后才生效 |
| `MeetingTypesContract.ClosedSnapshotCarriesHistoricalIdentities` | 快照可承载历史成员身份与终态字段（只验证形状，不验证产生时机） |
| `MeetingTypesContract.EventCarriesContractFields` | 事件可承载契约要求的字段 |

另有编译期断言（不在运行期计为用例）：

- 15 个 `ResultCode` 的**逐个数值**；
- "恰好 15 个"的计数；
- 四个状态机、三类 outcome、`EventType`、`ResultDisposition` 的取值集合与数量；
- ID / Timestamp 的内部表示（`std::is_same`）。

### 10.2 反向对照（证明断言真实有效）

```
把 ALREADY_JOINED 写成 101 → static_assert 编译失败（(100 == 101)）
把 MeetingState 数组写成 5 个 → static_assert 编译失败（(5 == 4)）
```

### 10.3 执行结果

```
ctest --test-dir /tmp/rtc-m1-build -N
  Test #1: TestInfrastructureSmoke.DeferExecutesOnScopeExit
  Test #2: MeetingTypesContract.ResultCodeNameCoversAllFrozenCodes
  Test #3: MeetingTypesContract.OptionalFieldsDefaultToAbsent
  Test #4: MeetingTypesContract.PresenceFlagGovernsFieldValidity
  Test #5: MeetingTypesContract.ClosedSnapshotCarriesHistoricalIdentities
  Test #6: MeetingTypesContract.EventCarriesContractFields
  Total Tests: 6

ctest --test-dir /tmp/rtc-m1-build --output-on-failure
  100% tests passed, 0 tests failed out of 6
  exit code 0
```

Step 2.5 原有的 `TestInfrastructureSmoke` 用例仍然存在且通过。

### 10.4 依赖隔离实测

```
ldd tests/ChatServerUnitTests
  → not found = 0
  → 无任何 /mnt 解析
  → 仅 libstdc++ / libm / libgcc_s / libc
  → libgrpc++、libgrpc、libprotobuf、libhiredis、libmysqlcppconn、
    libmysqlclient、libjsoncpp、libboost_filesystem 全部「不存在」
```

`MeetingTypes.h` 的 include 只有标准库；测试 TU 的编译命令只含一个 include 路径
（测试目录的 `..`）与 `-DGTEST_HAS_PTHREAD=1`。

---

## 11. 明确非目标

本步**未**实现、也**不应**在本步实现：

- `MeetingAggregate`；
- `MeetingService` / registry；
- TCP / wire protocol；
- Redis；
- MySQL / DAO / 持久化；
- gRPC；
- `LogicSystem` / `CSession` / `CServer` 集成；
- 任何业务 mutation 与状态转换；
- Phase 1 规划的 57 个 Meeting 正确性场景（Create 7 / Join 9 / Leave 11 /
  Disconnect 8 / Close 11 / Query 6 / Locality-API ordering 5）。

这 57 个场景属于 M2/M3，本步**没有**创建 `TEST(..., TODO)` 或 `EXPECT_TRUE(true)`
之类的占位用例来凑数量。

另外，M1 新文件不会被旧 Visual Studio project 引用：

```
Windows regression build: N/A
```

没有修改旧 Windows checkout，也没有把 `MeetingTypes.h` 强行加入 `.vcxproj`。

---

## 12. 下一步

只提出，**不开始**：

```
M2 — Pure MeetingAggregate
```
