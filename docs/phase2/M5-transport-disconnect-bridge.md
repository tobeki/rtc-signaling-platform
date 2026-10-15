# Phase 2 - M5：Transport Disconnect Bridge

> 状态：**COMPLETE（Linux 验证通过）**
> 基线：`dev`，起点 `e80099c7b3ea2908e2bca8808038b1597b129740`（HEAD == origin/dev，working tree clean）
> 本步不提交、不推送：最终改动以 working tree 形式交付，提交命令见本文 §15。

---

## 1. 目标与非目标

### 1.1 目标

把**旧网络层的会话生命周期终止事件**接入 M4 已经冻结的 Serialized Work Pipeline：

```text
CSession 生命周期终止（Close() / 会话被移除的终止分支）
        ↓  捕获稳定 SessionId（值拷贝）
SessionDisconnectBridge（exactly-once）
        ↓
ISessionDisconnectSink = LogicSystem::PostSessionDisconnectedWork
        ↓  同一 FIFO（legacy 消息 / Meeting 命令 / 断线 / Finalize 共用）
SerializedWorkExecutor 单消费者
        ↓  锁外分发
MeetingService::SessionDisconnected(session_id)
```

### 1.2 非目标（M5 明确不做）

```text
响应编码（JSON / protobuf）与客户端通知发送                → M6
TCP Meeting Protocol / message.proto / message ID         → Phase 3
CSession 建立 → RegisterConnectedSession 的真实接线        → 未在 M5 目标架构内（见 §13.1）
CSession 登录 → AuthenticateSession 的真实接线             → 未在 M5 目标架构内（见 §13.1）
MeetingDomain 任何修改                                     → 禁止
CServer.* / UserMgr.* / Redis / MySQL / gRPC 行为修改       → 未触碰
Windows .sln / .vcxproj / .props                          → 未触碰（见 §9）
```

---

## 2. 开始前的只读验证（结果）

```text
git status --short                      : 空（working tree clean）
git rev-parse HEAD                      : e80099c7b3ea2908e2bca8808038b1597b129740
git rev-parse origin/dev                : e80099c7b3ea2908e2bca8808038b1597b129740
git rev-list --left-right --count origin/dev...HEAD : 0	0
分支                                     : dev
```

未执行任何 `reset` / `checkout` / `stash` / 覆盖文件操作。

### 2.1 修改前的 Linux 基线复现

```text
cmake -S Server/ChatServer/ChatServer -B /tmp/m5-baseline-build \
      -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
  configure : PASS  exit 0
  build     : PASS  exit 0
  ctest -N  : Total Tests: 193
  ctest     : 100% tests passed, 0 tests failed out of 193
```

即 M4 记录的 193 个测试在 M5 起点上是可复现的，后续任何失败都可归因于 M5 的改动。

---

## 3. 只读核实：旧生命周期事实（改造前）

### 3.1 `CSession` 生命周期与全部终止边界

`CSession` 是 `boost::asio` 会话对象，`_session_id` 在构造函数体内生成后**不再改变**：

```cpp
CSession::CSession(io_context&, CServer*)   → uuid → _session_id（此后恒定）
CSession::Start()                           → AsyncReadHead(HEAD_TOTAL_LEN)
CSession::SetUserId(int)                    → 仅登录成功后由 LogicSystem::LoginHandler 调用
CSession::Close()                           → _socket.close(); _b_close = true;
CSession::~CSession()                       → 仅打印
```

逐条列出现有**会话终止路径**（只读 grep 的完整结果）：

| # | 位置 | 现状 | 是否终止会话 | 是否已有 `Close()` |
| --- | --- | --- | --- | --- |
| 1 | `AsyncReadHead` 读错误 `ec` | `Close(); ClearSession(id);` | 是 | 有 |
| 2 | `AsyncReadHead` 头部长度不足 | `Close(); ClearSession(id);` | 是 | 有 |
| 3 | `AsyncReadHead` `msg_id > MAX_LENGTH` | **仅** `ClearSession(id);` | 是（从 `_sessions` 移除，对象随后交给析构） | **无** |
| 4 | `AsyncReadHead` `msg_len > MAX_LENGTH` | **仅** `ClearSession(id);` | 同上 | **无** |
| 5 | `AsyncReadBody` 读错误 `ec` | `Close(); ClearSession(id);` | 是 | 有 |
| 6 | `AsyncReadBody` 正文长度不足 | `Close(); ClearSession(id);` | 是 | 有 |
| 7 | `HandleWrite` 写错误 | `Close();`（**不**调用 `ClearSession`） | 是（传输已死） | 有 |

结论：**存在 7 条终止路径，其中 5 条经过 `Close()`，2 条只经过 `ClearSession`。**
因此"只 hook `Close()`"会漏掉第 3、4 条；"只 hook `ClearSession`"会漏掉第 7 条。
这直接决定了 §5 的桥接点选择。

### 3.2 `CServer::ClearSession`

```cpp
void CServer::ClearSession(std::string session_id) {
    if (_sessions.find(session_id) != _sessions.end()) {          // 注意：find 未持锁（既有隐患）
        auto uid = _sessions[session_id]->GetUserId();
        UserMgr::GetInstance()->RmvUserSession(uid);
    }
    { std::lock_guard<std::mutex> lock(_mutex); _sessions.erase(session_id); }
}
```

M5 **不修改** `CServer.*`，理由见 §5.3。

### 3.3 M4 已经固定的消费接口（M5 必须沿用）

```text
1. 网络侧只能通过 LogicSystem::PostSessionDisconnectedWork(session_id) 投递，不得直接调用
   MeetingService。                                                       → 已遵守
2. 必须在 CSession::Close() / CServer::ClearSession() 之前捕获稳定 session_id
   （R3；Step 1.6 §13）。                                                 → 已遵守（见 §5）
3. 不得为断线事件编造 TCP msg_id，不得新建第二条队列或第二个 worker。        → 已遵守
4. Meeting mutation 的唯一执行上下文仍是 SerializedWorkExecutor 消费者线程。  → 未改变
5. 关闭顺序依赖 drain：已受理的 Close 会在 drain 阶段补完 Finalize，不得绕过。 → 未改变
6. 若需新增 kind，应沿用显式 enum + value payload 形态。                    → 不新增 kind
```

---

## 4. 设计：`SessionDisconnectBridge`（新增，header-only）

新增文件：`Server/ChatServer/ChatServer/SessionDisconnectBridge.h`（175 行，UTF-8）。

```cpp
class ISessionDisconnectSink {
public:
    virtual bool PostSessionDisconnectedWork(const std::string& session_id) = 0;
};

enum class SessionDisconnectDispatch { POSTED, ALREADY_POSTED, REJECTED };

class SessionDisconnectBridge {
public:
    bool Bind(ISessionDisconnectSink* sink, const std::string& session_id);
    SessionDisconnectDispatch NotifyDisconnected();   // 可重复、可并发；最多投递一次
    bool AlreadyPosted() const;
    bool IsBound() const;
    std::string BoundSessionId() const;
private:
    mutable std::mutex mutex_;
    ISessionDisconnectSink* sink_;
    bool bound_;
    bool posted_;
    std::string session_id_;
};
```

### 4.1 为什么需要它，而不是让 `CSession` 直接调用 `LogicSystem`

```text
1. "恰好一次"必须是**结构保证**：同一会话有 7 条终止路径，靠调用方自觉去重无法验证；
2. 网络线程绝不允许触碰 MeetingService，也不允许建立第二条队列；
3. 入队决策必须与 legacy 依赖隔离，才能在不引入 asio / Redis / MySQL 的前提下被测试。
```

### 4.2 关键性质

| 性质 | 落点 |
| --- | --- |
| payload 只携带稳定 `SessionId` | `SessionDisconnectBridge` 只保存 `std::string session_id_`；`Explain()` 无任何指针 |
| 不持有连接对象 | 头文件不 include `CSession.h` / `socket` / `shared_ptr<CSession>` |
| 恰好一次 | `posted_` 在**同一把内部 mutex** 下先置位，只有一个线程能走到投递 |
| 投递在锁外 | `NotifyDisconnected()` 在释放 `mutex_` 后调用 sink，绝不与队列锁 / 空闲锁嵌套 |
| 重复投递不重试 | 首次投递被队列拒绝（stop）后返回 `REJECTED` 且**不**重试（M4 §11.2） |
| 零领域依赖 | 只 `#include <mutex>` / `<string>`；参数类型是 `std::string`（`meeting::SessionId` 的底层类型） |
| C++14 可实现 | 无 `variant` / `optional` / 结构化绑定 |

> 关于参数类型：`ISessionDisconnectSink` 刻意写成 `const std::string&`，这样网络层对
> `meeting/*` 头文件**零依赖**。生产实现 `LogicSystem::PostSessionDisconnectedWork`
> 的形参仍然是 `const meeting::SessionId&`（同一类型），并由 `override` 在编译期保证一致
> —— 一旦 `SessionId` 改成非 `std::string` 类型，会**编译失败**而不是静默错配。

### 4.3 为什么把桥接做成 header-only

M5 需要新增一个文件，但 STEP-1.6 §25.6 与本步任务约束在 Windows 工程上冲突（见 §9）。
把桥接做成 header-only 后：

```text
生产侧新增 TU 数 = 0   → ChatServer.vcxproj 的 <ClCompile> 列表无需改动（本步也确实没改）
测试侧只需把测试文件加入 tests/CMakeLists.txt（与 Windows 工程无关）
```

---

## 5. 桥接点选择（本步最重要的设计裁决）

### 5.1 统一通知点：`CSession::NotifySessionDisconnected()`

```cpp
void CSession::NotifySessionDisconnected() {
    const SessionDisconnectDispatch dispatch = _disconnect_bridge.NotifyDisconnected();
    if (dispatch == SessionDisconnectDispatch::REJECTED) {
        std::cout << "session disconnect work rejected, session: " << _session_id << std::endl;
    }
}
```

它是**进程内唯一的断线投递点**：任何终止路径都必须先到达这里。

### 5.2 两个调用点（覆盖 §3.1 的全部 7 条终止路径）

```text
调用点 A：CSession::Close()
          覆盖路径 1 / 2 / 5 / 6 / 7
          Close() 内部结构变为：
              { lock(_session_mtx); _socket.close(); _b_close = true; }   // 语义未变
              NotifySessionDisconnected();                                // 锁外
          刻意**不**判断"是否首次 Close"：去重是桥接的结构保证。

调用点 B：msg_id > MAX_LENGTH / msg_len > MAX_LENGTH 两个分支
          覆盖路径 3 / 4（这两个分支既有的行为是"只 ClearSession，不 Close"）
          在既有的 ClearSession 调用**之前**插入 NotifySessionDisconnected()，
          **不改动既有 socket 语义**（不额外调用 Close()）。
```

**为什么必须补上调用点 B**：这两个分支同样会终止会话（从 `_sessions` 移除、对象随后析构）。
若不通知，该 Session 会在领域侧永久停留在 `CONNECTED/AUTHENTICATED`，其 Meeting Binding
与反向索引永远不会被清理 —— 正是 M5 要消灭的"stale participant"类缺陷。

**为什么不用"把这两个分支改成先调用 `Close()`"这种更整齐的写法**：那会改变既有传输行为
（socket 提前关闭、可能中止在途写）。M5 的定位是"接入生命周期事件"，不是"顺手重写
socket 处理"，因此选择语义零变化、只补通知的写法。

### 5.3 为什么不 hook `CServer::ClearSession()`

```text
1. 断线 payload 只需要稳定 session_id，而这个值在 CSession 内部就已是最终值，CServer 不参与
   "捕获稳定上下文"；
2. 在 ClearSession 内通知需要先做一次 map 查找并持有 shared_ptr，而该函数的 find **既有的**
   未持锁问题（Step 1.6 §13.3 已记录为既有隐患）会让新代码依赖一个不安全的读；若改成持锁
   通知，就会引入 CServer::_mutex → 队列锁 的新锁序；
3. 现有全部 ClearSession 调用点都在 CSession 内部，已经被调用点 A/B 覆盖；
4. 因此修改 CServer.* 只会增加风险，覆盖率为零。
```

结论：**`CServer.h` / `CServer.cpp` 未修改**（`git status` 零命中）。

### 5.4 为什么不在析构函数里投递

```text
destructor 生命周期不稳定、顺序不可控、测试困难（任务明确禁止）。
本步的终止边界全部是显式的：Close() 与两个非法头部分支；
~CSession 只保留原有打印，不参与任何业务通知。
```

### 5.5 绑定时机

```cpp
// CSession 构造：SessionId 一旦生成即固定，并在此绑定断线桥接。
_disconnect_bridge.Bind(LogicSystem::GetInstance().get(), _session_id);
```

`LogicSystem` 单例在**首次 accept** 时被解析（此前首次接触发生在首条消息）。
`LogicSystem()` 构造函数只做 `RegisterCallBacks()`（绑定成员函数指针），不触碰
Redis / MySQL / gRPC / 队列，因此提前解析是安全的；FIFO 与 worker 仍然保持惰性启动。
`sink_` 指向进程级单例，其生存期覆盖全部会话（`Server` / `io_context` 都是 `main` 的局部对象，
先于静态单例析构）。

---

## 6. 顺序保证（为什么不会 reorder / duplicate / 直连）

### 6.1 不会 message reorder

```text
CSession 的 legacy 消息：  AsyncReadBody → LogicSystem::PostMsgToQue → EXTERNAL_MESSAGE
CSession 的断线事件：      Close() / 终止分支 → 桥接 → SESSION_DISCONNECTED
                                     ↓
                        同一个 LogicWorkQueue（一条 std::deque + 一把 mutex）
                                     ↓
                        同一个 SerializedWorkExecutor 单消费者，FIFO 取出
```

- 断线事件不经过任何"优先级"或"第二队列"，只做 `push_back`；
- 因此它的位置**完全由入队那一刻决定**。读取错误天然发生在"该会话此前投递的消息"之后，
  于是 `Message A → Disconnect → Message B` 的 FIFO 次序就是
  `Process A → Process Disconnect → Process B`；
- 这一条由 `SessionDisconnectBridgeOrdering.MessageDisconnectMessageKeepExactFifoOrder`
  用**真实队列 + 可控阻塞的 legacy 路由器**确定性锁定（不依赖 sleep/时序）。

### 6.2 不会 duplicate disconnect

```text
7 条终止路径可能对同一会话重复触发（读错误 + 写错误 + 非法头部 + 重复 Close）
        ↓
全部收敛到唯一的 NotifySessionDisconnected()
        ↓
SessionDisconnectBridge::NotifyDisconnected()：
    在内部 mutex 下先判定 posted_ —— 只有一个调用者能走到投递，其余得到 ALREADY_POSTED
        ↓
FIFO 中该会话的 SESSION_DISCONNECTED 恰好一条
```

即使"恰好一次"被突破，M3 的 `SessionDisconnected` 本身对 tombstone 幂等
（`already_closed` / 事件为空）—— 这是**双重**保证：结构去重 + 领域幂等。

### 6.3 不会直连 MeetingService

```text
1. SessionDisconnectBridge.h 不 include 任何 meeting 头文件，物理上无法调用 MeetingService；
2. CSession 只调用桥接，桥接只调用 ISessionDisconnectSink；
3. ISessionDisconnectSink 的生产实现是 LogicSystem::PostSessionDisconnectedWork，其函数体
   只有两行：EnsureWorkQueueStarted(); return _work_executor->PostSessionDisconnected(id);
4. 没有任何 getter 返回可变的 MeetingService / 队列 / registry（M4 §5.3 的性质未被削弱）。
```

---

## 7. 文件编码结果

`CSession.h` / `CSession.cpp` 在 M5 实施时是磁盘上的 **GBK(CP936)** 文件
（`file` 报告 ISO-8859 系）；`LogicSystem.h` / `LogicSystem.cpp` 在 M4 §16.1 转 UTF-8 之后
一直是 UTF-8。

### 7.1 M5 实施时：byte-preserving 插入（保持当时的 GBK）

```text
CSession.h   : 2039 bytes, non-ASCII 30  → 2387 bytes, non-ASCII 198, CRLF=0（bare LF 77）
CSession.cpp : 7152 bytes, non-ASCII 250 → 8825 bytes, non-ASCII 992, CRLF=0（bare LF 265）
```

```text
1. 先验证 GBK 解码 → 重编码逐字节无损（与 M4 §16 相同的方法）；
2. 用 Python 以**唯一字节锚点**做精确插入，新增中文注释显式 encode('gbk')；
3. 既有字节不参与重编码，因此当时不可能产生全文件 diff 或中文乱码；
4. 独立的 iconv -f GBK -t UTF-8 全部成功（exit 0），证明新字节序列仍是有效 GBK；
5. 全文解码后 diff 复核：改动**只有新增行**，既有行零改动。
```

### 7.2 评审后续：按要求把 `CSession.h` / `CSession.cpp` 转为 UTF-8（人工授权）

项目负责人人工检阅时发现：在按 UTF-8 解读的编辑器里，这两个文件的注释（**既有的与新增的
都是**）显示为乱码，根因是整文件为 GBK(CP936)；随后明确要求"把注释统一换成 UTF-8"。
该授权与 M4 §16.1（把 `LogicSystem.cpp` 转为 UTF-8）属于同一类决定，已执行：

```text
转换方式          : GBK 严格解码（无损）→ 重新编码为 UTF-8，不带 BOM，换行保持 bare LF
转换范围          : 整文件（C++ 源文件只有一个字符编码，无法只转换"注释"本身）
CSession.h        : 2387 bytes(non-ASCII 198) → 2486 bytes(non-ASCII 297), CRLF=0, bare LF 77
                    SHA256 95fc816545d09d40… → dbde7ae022549d5de20513fa714e167852b8d4403e80aa37f40a882cbbde3c92
CSession.cpp      : 8825 bytes(non-ASCII 992) → 9321 bytes(non-ASCII 1488), CRLF=0, bare LF 265
                    SHA256 533ac6c49888a4b2… → 00e09fa51e67ef0a14be271956bba2e091aa6c8a93df98347dad1a37090341d7
file(转换后)      : Unicode text, UTF-8 text（两个文件均如此）
```

**零内容变更证明**（三条独立证据，缺一不可）：

```text
1. 解码后文本逐字符相同：new_raw.decode('utf-8') == old_raw.decode('gbk')
2. 去掉注释后的代码逐字节相同（含全部字符串字面量与 token 邻接关系）
3. 转换前**全部**非 ASCII 字符都位于注释中：non-ASCII(去注释代码) = 0、
   non-ASCII(字符串/字符字面量) = 0
   → 因此本次转换对运行时行为零影响：不是"看起来没影响"，而是结构上不可能有影响。
```

与 `HEAD` 的对照复核（把 `HEAD` 版本按 GBK 解码后与当前文件比较）：改动**只有 M5 新增行**
（CSession.h 8 行、CSession.cpp 39 行），既有行零改动；既有 GBK 中文注释现在正确显示。

Windows / MSVC 的附带事实（与 M4 §17.2 同类，如实记录）：

```text
这两个文件在 ChatServer.vcxproj 的 <ClCompile> 列表内。MSVC 在中文 locale（CP936）下把
"无 BOM 的 UTF-8" 当作 GBK 解读，会产生 C4819 类告警；理论上还有"注释行末字节被当作
双字节首字节而吞掉换行"的极端风险（MSVC 之外的编译器不受影响，Linux 构建已实测通过）。

处置：本步任务禁止修改 .vcxproj，因此**不**给 Windows 工程增加 /utf-8，也不加 BOM；
R6 已经使当前 dev HEAD 的 Windows 构建不可用（§9），补齐 R6 时应一并决定：
  ① 给 Windows 工程加 /utf-8；或 ② 给这些源文件加 UTF-8 BOM；或 ③ 全仓库编码统一。
```

同一次编码决策尚未覆盖的 legacy 文件（本步**未**修改，属后续一次性决定的范围）：

```text
仍为 GBK(CP936)、含中文注释因而在 UTF-8 编辑器里同样会显示乱码：
  const.h   ConfigMgr.h/.cpp   RedisMgr.h/.cpp   MysqlDao.h/.cpp   ChatGrpcClient.h
  ChatServiceImpl.cpp   AsioIOServicePool.cpp   CServer.cpp   MsgNode.cpp   UserMgr.cpp
纯 ASCII（无编码问题）：data.h  CServer.h  MsgNode.h  UserMgr.h  MysqlMgr.h/.cpp
  Singleton.h  ChatServiceImpl.h  ChatGrpcClient.cpp
已是 UTF-8：LogicSystem.h/.cpp  ChatServer.cpp(UTF-8+BOM)  以及 M1–M5 的全部新增文件
```

`LogicSystem.h` / `LogicSystem.cpp` 保持其现有编码（M4 §16.1 已授权转为 UTF-8），
M5 新增内容仍为 UTF-8 中文注释。

---

## 8. 依赖与 C++14

```text
SessionDisconnectBridge.h 只 include <mutex> / <string>
  → 不引入 asio / Boost / protobuf / gRPC / hiredis / MySQL / JsonCpp / Threads
  → 不引入任何 meeting/* 头文件（网络层对领域零依赖）

C++14：新增代码未使用 variant / optional / string_view / 结构化绑定 / if constexpr
日志使用既有 std::cout 风格（与 CSession.cpp 其余部分一致）
```

---

## 9. Windows 工程与 R6（如实报告，未隐藏）

**本步未修改** `.sln` / `.vcxproj` / `.vcxproj.filters` / `.props`（`git status` 零命中），
也未访问任何 Windows checkout。

事实与冲突：

```text
STEP-1.6 §25.6（M5 任务卡）要求：
  Files allowed   : 修改 CSession.* / CServer.*；**修改两个工程文件**
  Build gate      : Debug|x64 编译通过
本步任务约束（M5 Bootstrap Prompt §5 / §10）要求：
  禁止修改 Windows .sln / .vcxproj / .props
  若"Windows 工程必须同步修改"→ 停止并报告
```

M5 的实际设计**不要求**修改 Windows 工程文件：

```text
1. 新增的 SessionDisconnectBridge.h 是 header-only，MSVC 通过 #include 直接解析，
   不依赖 .vcxproj 的 <ClInclude>；
2. 新增 TU 数 = 0，因此 <ClCompile> 列表无需增加条目；
3. 被修改的 CSession.* / LogicSystem.* 本来就在工程里。
```

但必须区分两件事：

```text
M5 未新增 Windows 工程债                                    : 成立（§9 第 1–3 条）
当前 dev HEAD 的 Windows Debug|x64 构建                     : 仍然**不可用**
```

后者是 M4 §17.1 记录的既有 **R6**：`LogicWorkItem.h` / `LogicWorkDispatcher.h/.cpp` /
`meeting/*` 都不在 `ChatServer.vcxproj` 里，而 `LogicSystem.h` 现在依赖它们。
本文档不把"Linux 编译通过"表述成"Windows 构建通过"，
也**没有**在 Windows 上构建过任何东西（本步的全部验证都在 Ubuntu-24.04-RTC 上完成）。

**因此 STEP-1.6 §25.6 的 `Debug|x64` build gate 在本步约束下无法被满足，且未被满足。**
R6 的补齐需要一次性把 M1–M5 的领域/队列文件加入 Windows 工程，属于授权范围外的跨平台任务。

---

## 10. 测试

新增 `Server/ChatServer/ChatServer/tests/SessionDisconnectBridgeTest.cpp`，
**18 个测试 / 5 个 gtest 套件**，全部通过。

### 10.1 测试切口（与 M4 §12 同一哲学）

```text
生产:  CSession::Close() / 终止分支 → SessionDisconnectBridge → LogicSystem（唯一 FIFO）
                                    ↓
测试:  同一个 SessionDisconnectBridge → ISessionDisconnectSink 替身
                                    ↓
       SerializedWorkExecutor<LogicWorkDispatcher>   ← 真实队列 + 真实单消费者
       LogicWorkDispatcher / MeetingService          ← 生产实现本身（没有替身、没有复制）
```

测试与生产的差别只有三处：ID/clock 由固定序列注入；legacy 路由目标是记录型替身；
观测 sink 非空。**dev 生产代码的分发逻辑没有被复制。**

刻意**不**进入测试 target 的：`CSession.cpp` / `CServer.cpp` / `LogicSystem.cpp`
（会拉进 asio / Boost / Redis / MySQL / gRPC 整条 legacy 业务链路；Step 2.5 §14、M4 §18）。

### 10.2 用例与任务要求逐项对应

| 任务要求 | 测试名 | 断言要点 |
| --- | --- | --- |
| Disconnect Bridge：connected | `SessionDisconnectBridgeFifo.ConnectedSessionDisconnectClosesTheDomainSession` | 已注册未认证会话断线 → 1 次 fan-out / affected 0；后续命令得到 `SESSION_STATE_REJECTED`（证明领域真的把它置为 CLOSED，而不是被丢弃） |
| Disconnect Bridge：authenticated | `SessionDisconnectBridgeFifo.AuthenticatedSessionDisconnectIsAppliedWithoutMeetings` | 已认证未入会 → affected 0 / 无事件；迟到 Join 被 `SESSION_STATE_REJECTED` |
| Disconnect Bridge：repeated | `SessionDisconnectBridgeFifo.RepeatedBridgeDisconnectProducesExactlyOneFifoEntry` | 3 次 notify → FIFO 中 `SESSION_DISCONNECTED` 恰好 1 条、`ParticipantLeft` 恰好 1 次 |
| （桥接本体）恰好一次 | `SessionDisconnectBridgeLatch.*`（6 个） | 首次投递携带绑定 SessionId；重复调用幂等；未绑定不投递；Bind 一次性；首次被拒后不重试；**8 线程并发只投递一次** |
| Ordering：message→disconnect→message | `SessionDisconnectBridgeOrdering.MessageDisconnectMessageKeepExactFifoOrder` | 用 legacy 路由器闩锁固定交错，断言有序列表恰为 `A / DISCONNECT / B`；断线确实做了真实领域工作 |
| Lifecycle：Leave before disconnect | `...Lifecycle.LeaveBeforeBridgeDisconnectDoesNotDuplicateParticipantLeft` | `ParticipantLeft` 仍为 1；断线 affected 0 |
| Lifecycle：Disconnect before Leave | `...Lifecycle.BridgeDisconnectBeforeLeaveRejectsTheStaleLeave` | 迟到 Leave = `SESSION_STATE_REJECTED`；无第二个 `ParticipantLeft` / 无 `MeetingClosed` |
| Lifecycle：Close before disconnect | `...Lifecycle.HostBridgeDisconnectDuringEndingKeepsFrozenCloseContext` | Host 在 ENDING 期间断线：affected 1、无成员事件、"已冻结"的 CloseContext 仍含该 session、快照照常发布 |
| Lifecycle：Disconnect during ENDING | `...Lifecycle.BridgeDisconnectDuringEndingUnbindsWithoutMembershipChange` | dispatch 次序为 `CLOSE / DISCONNECT / FINALIZE_CLOSE`（即断线确实落在 ENDING 窗口内）；只做幂等 UNBOUND，Meeting 不停在 ENDING |
| Lifecycle：Disconnect after CLOSED | `...Lifecycle.BridgeDisconnectAfterClosedTouchesNothing` | 次序 `FINALIZE_CLOSE / DISCONNECT`；affected 0、无新事件、快照仍在 |
| Shutdown：queued disconnect work / graceful / 不丢 | `...Shutdown.*`（3 个） | 已受理的断线 work 在 stop 后仍被执行；**确定性前提**下（测试线程持队列锁 `PushLocked`）入队的断线 work 被 drain 完；stop 之后到达的断线事件被如实 `REJECTED` 且领域侧零影响 |

### 10.3 结果

```text
configure (-DBUILD_TESTING=ON)   : PASS  exit 0
build (--parallel 2)             : PASS  exit 0
新增测试（仅 M5 过滤）            : 18/18 PASSED
ctest -N                         : Total Tests: 211   （M4 的 193 个未删、未改）
ctest --output-on-failure        : 100% tests passed, 0 tests failed out of 211
整轮独立进程重跑 60 次            : 211 PASSED × 60，0 failing（无偶发超时/无 flake）
```

测试数量守恒：

```text
M4 及以前 : 193（未删除、未修改任何一个 —— LogicWorkQueueTest.cpp 等文件零改动）
M5 新增   :  18
合计      : 211
```

`-Wall -Wextra` 检查：新增的 `SessionDisconnectBridge.h` / `SessionDisconnectBridgeTest.cpp`
零告警；`CSession.cpp` / `LogicSystem.cpp` 的告警全部是既有告警
（`MsgNode.h` / `RedisMgr.h` / `ChatGrpcClient.h` 的 `-Wreorder`、既有 `unused parameter`、
`MysqlDao.cpp:95` 既有告警），本步新增内容不产生任何新告警。

---

## 11. 运行时 smoke（Step 2.4 方法 + M5 专用观测）

在**仓库外**临时目录 `/tmp/m5-smoke` 执行（执行后已删除），使用合成 `config.ini`：

```text
仓库外临时目录 + 合成 config.ini（未读取、未复制真实配置；密码仅用于本机 loopback 临时实例）
专用 Redis 实例（bind 127.0.0.1, --save '', --requirepass <loopback 临时口令>）
端口预检：三个端口开始时全部空闲；系统 Redis 6379 全程未触碰
SIGTERM（未使用 kill -9）；不执行任何 MySQL 业务；未解析任何协议帧
```

M5 专用观测（这是单元测试覆盖不到的一段）：

```text
客户端 connect 后**一个字节都不发**就 close
  → AsyncReadHead 读错误分支 → CSession::Close()
  → SessionDisconnectBridge → LogicSystem::PostSessionDisconnectedWork
  → EnsureWorkQueueStarted() → 唯一 FIFO 的 worker 线程被创建
唯一 FIFO worker 是惰性创建且**只由这里创建**，所以
"没有投递过任何消息，却出现了 worker 线程" 就是断线桥接在真实二进制里确实生效的直接证据。
```

结果（17 项检查全部 PASS）：

```text
三个端口预检空闲                                     : PASS
专用 Redis PONG                                     : PASS
ChatServer 进程启动                                 : PASS
TCP listener 就绪（用 ss 检查，不建立连接）           : PASS
gRPC listener 就绪（用 ss 检查，不建立连接）          : PASS
日志中从未出现 "recv msg id"（确实没有消息被投递）    : PASS
accept/读路径记录了被关闭的客户端                     : PASS
桥接未报告 rejection                                 : PASS
M5 断线桥接创建了唯一 FIFO worker 线程                : PASS  16 → 16（连接后）→ 17（断开后）
Redis 登录计数注册（legacy key 硬编码为 logincount_lhf）: PASS
注册可在专用 Redis 中查到                             : PASS
SIGTERM → 进程自行退出 exit_status = 0                : PASS
listeners 消失                                       : PASS
Redis 注册被清理（HEXISTS = 0）                       : PASS
shutdown 日志无 terminate/abort/exception             : PASS
系统 Redis (127.0.0.1:6379) 未被触碰                  : PASS
专用 Redis 实例关闭                                   : PASS
```

该 smoke **只证明** startup / 真实断线路径 / shutdown 回归：

```text
它不能替代 §10 的单元测试，也不能声称"领域侧真的完成了断线清理"——
因为当前 dev HEAD 上没有任何 CSession 被注册到领域侧（见 §13.1）。
```

---

## 12. 验证边界（严格区分）

```text
Disconnect bridge semantics (exactly-once / stable SessionId / no session object):
VERIFIED BY TEST SEAM
（① 桥接本体：6 个纯单元用例，含 8 线程并发；
  ② 桥接之后的队列位置与领域结果：同一份生产 SessionDisconnectBridge 投递给真实
     SerializedWorkExecutor + 真实 LogicWorkDispatcher + 真实 MeetingService；
  ③ 19 项顺序 / 生命周期 / shutdown 断言均为确定性断言，不使用 sleep 或时序运气）

Transport ingress on the real binary (CSession 真实断线路径触发桥接):
VERIFIED BY RUNTIME SMOKE
（唯一 FIFO worker 线程只可能由 PostSessionDisconnectedWork 惰性创建；
  "无消息而出现 worker 线程" 即为真实断线路径触发桥接的直接证据）

CSession::Close() / 非法头部分支 的实际接入点代码:
VERIFIED BY STATIC REVIEW
（CSession.cpp 不在测试 target 内；接入点为静态复核过的 3 行调用，验证边界与 M4 §20 同口径）

CSession → 领域 Session 注册/认证接线:
NOT DONE（不在 M5 目标架构内，见 §13.1）

领域侧断线的端到端可见效果（真实网络断线 → Meeting 成员清理）:
NOT VERIFIED / CURRENTLY INERT（原因同上，见 §13.1）
```

---

## 13. 未隐藏的问题（本步报告的 2 项）

### 13.1 领域 Session 的注册 / 认证尚未接线 → 真实断线事件当前是"有效但无害"的

```text
事实：M5 只实现了 CSession → SESSION_DISCONNECTED 这一段。CSession 建立时并不会调用
      RegisterConnectedSession，登录成功时也不会调用 AuthenticateSession
      （这两条属于 M4 的 INTERNAL_SESSION_* typed 入口，M4 文档明确"M4 不代表已接入
      真实 CSession 的认证路径"；M5 任务目标架构也未包含它们）。

影响：在**当前**二进制上，真实网络断线产生的 SessionDisconnected 找不到领域 Session
      （session_known=false, affected_meeting_count=0）。也就是说：
        断言"桥接正确"成立；
        断言"真实断线会清理 Meeting 成员"目前**不成立**。
      这正是 §11 的 smoke 只能以"worker 线程出现"作为证据、而无法以领域结果作为证据的原因。

候选方案：
  A. 立即接线：CSession 构造 → PostSessionRegister；SetUserId → PostSessionAuthenticate。
     优点：端到端立刻可用。风险：M3 的 DomainSessionRecord tombstone 保留期是 **Deferred**
     （无 TTL / 无回收），把"每一个 TCP 连接"都登记成领域 Session 会让 sessions_ 与
     tombstone 无界增长 —— 这是需要先决策的 retention 问题，而不是顺手接线。
  B. 先做 retention / 回收策略，再接线注册与认证。
  C. 保持 M5 现状，把注册/认证接线作为独立一步（M5.1 或 M6 前置）。

推荐：C + B（先补 retention 决策，再接线），并且**不把当前状态表述为"断线已端到端可用"**。

本步的具体处置：不扩大授权范围，如实报告；M5 的测试用 M4 已经提供的 INTERNAL_SESSION_*
入口在同一 FIFO 上准备 CONNECTED / AUTHENTICATED 会话，从而把"未来接线之后的契约"
用真实生产实现锁定下来。
```

### 13.2 `_user_uid` 未初始化（既有事实）与 payload 选择

```text
事实：CSession 构造函数不初始化 _user_uid（Step 1.6 §13.3 已记录）。
处置：M5 的 payload **只**包含 SessionId，不读取 uid，因此不受该隐患影响。
      "是否已认证"的判断完全由领域侧自己的会话登记决定，不依赖网络对象的成员值 ——
      这同时是"payload 只允许稳定值"这条约束的实际收益。
```

---

## 14. 已知限制

```text
1. CSession::Close() / 终止分支的接入点由静态复核保证，不由单元测试保证（CSession.cpp
   不在测试 target 内）。
2. 真实网络断线在领域侧当前是 no-op（§13.1）。
3. 没有协议级验证：smoke 只建立 TCP 连接并关闭，不解析任何帧。
4. legacy IM 完整业务链路仍未验证（无 MySQL server、无 gRPC 调用）。
5. Windows Debug|x64 gate 未满足（R6，见 §9）。
6. 进程退出窗口内的极端情形：若某个 Asio handler 在 `main` 返回之后才执行 Close()，
   会访问正在析构的单例 —— 这是 M4 时代 `PostMsgToQue` 已经存在的同一类暴露面，
   本步没有扩大它（桥接只在显式终止边界被调用，~CSession 不参与），但也**没有消除**它。
7. ClosedSnapshot retention / TTL 仍未实现（Deferred）。
```

---

## 15. Scope Review 与提交

### 15.1 修改清单（`git status --short` 全量）

```text
修改：
  Server/ChatServer/ChatServer/CSession.h            （+8 行：include / 声明 / 成员；
                                                      编码：GBK → UTF-8，见 §7.2）
  Server/ChatServer/ChatServer/CSession.cpp          （+39/-3 行：绑定 + 通知点；
                                                      编码：GBK → UTF-8，见 §7.2）
  Server/ChatServer/ChatServer/LogicSystem.h         （UTF-8；+13/-2 行：实现 sink 接口）
  Server/ChatServer/ChatServer/LogicSystem.cpp       （UTF-8；+8 行注释，函数体未变）
  Server/ChatServer/ChatServer/tests/CMakeLists.txt  （+4 行：纳入 M5 测试）

新增：
  Server/ChatServer/ChatServer/SessionDisconnectBridge.h        （175 行，UTF-8，header-only）
  Server/ChatServer/ChatServer/tests/SessionDisconnectBridgeTest.cpp （877 行，UTF-8）
  docs/phase2/M5-transport-disconnect-bridge.md                 （本文档）

删除：无
diff 规模（含授权编码转换）：5 files changed, 84 insertions(+), 22 deletions(-) + 3 个新增文件
  ——其中 22 处删除**全部**来自 CSession.h/.cpp 的 GBK → UTF-8 行（既有中文注释行按新编码
  重新出现），内容层面相对 HEAD 的改动仍然只有 M5 新增行（见 §7.2 的对照复核）。
```

### 15.2 明确未修改（逐项核对，`git status` 零命中）

```text
meeting/MeetingTypes.h            meeting/MeetingAggregate.h/.cpp   meeting/MeetingService.h/.cpp
CServer.h                         CServer.cpp
UserMgr.*  RedisMgr.*  MysqlMgr.*  MysqlDao.*  ChatGrpcClient.*  ChatServiceImpl.*
MsgNode.*  const.h  data.h  Singleton.h  ConfigMgr.*
message.proto                     CMakeLists.txt（父级，未新增 TU 故无需改动）
LogicWorkItem.h  LogicWorkDispatcher.h/.cpp  LogicWorkQueueTest.cpp 及其余既有测试文件
ChatServer.vcxproj / .filters / PropertySheet.props / *.sln
Server/ChatServer2/               MyChat_Qt/
无 build artifacts / 无 config.ini / 无日志 / 无密码进入仓库
```

### 15.3 手工提交命令（本步不自动提交、不推送）

```bash
cd /home/tobeki/projects/rtc-signaling-platform

git add Server/ChatServer/ChatServer/SessionDisconnectBridge.h \
        Server/ChatServer/ChatServer/tests/SessionDisconnectBridgeTest.cpp \
        Server/ChatServer/ChatServer/CSession.h \
        Server/ChatServer/ChatServer/CSession.cpp \
        Server/ChatServer/ChatServer/LogicSystem.h \
        Server/ChatServer/ChatServer/LogicSystem.cpp \
        Server/ChatServer/ChatServer/tests/CMakeLists.txt \
        docs/phase2/M5-transport-disconnect-bridge.md

git commit -m "feat(chatserver): bridge transport disconnect into the serialized work fifo"
```

> 提交前请再次确认：`git status --short` 只包含 §15.1 列出的路径。
