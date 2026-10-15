#pragma once

// ==============================================================================
// M5 SessionDisconnectBridge：网络层会话生命周期 → 唯一 FIFO 的断线事件桥接。
//
// 目标路径（Step 1.6 §13.1、M4 §9 / §22）：
//
//   CSession 生命周期结束（Close() / 会话被移除的终止分支）
//        ↓
//   capture stable SessionId（值拷贝，不持有任何连接对象）
//        ↓
//   SessionDisconnectBridge（exactly-once）
//        ↓
//   ISessionDisconnectSink → LogicSystem::PostSessionDisconnectedWork
//        ↓
//   唯一 FIFO → 单消费者 → MeetingService::SessionDisconnected()
//
// 本文件刻意**只依赖 C++ 标准库**（<mutex> / <string>）：
//   - 不 include CSession.h / LogicSystem.h / LogicWorkItem.h / asio / Redis / MySQL；
//   - 不认识任何领域类型，只搬运一个**稳定 SessionId 值**；
//   - 因此它既能被生产 CSession 以值成员持有，也能被单元测试直接实例化，
//     并把事件投递进真实的 SerializedWorkExecutor（测试执行生产实现本身）。
//
// 为什么必须有这一层，而不是让 CSession 直接调用 LogicSystem：
//   1. "恰好一次"必须是**结构保证**：同一个会话可能被读错误、写错误、非法头部、
//      重复 Close 等多条路径终止，靠调用方自觉去重是不可验证的；
//   2. 网络线程**绝不允许**直接触碰 MeetingService，也不允许建立第二条队列；
//   3. 入队决策必须与 legacy 依赖隔离，才能在不引入 asio / Redis / MySQL 的
//      前提下被测试（Step 2.5 §14、M4 §18 的验证边界）。
//
// 明确禁止（Step 1.6 §12.5 / M3 Decision 4）：
//   payload 只允许是稳定 SessionId；绝不允许 CSession*、shared_ptr<CSession>、
//   socket 或任何连接对象跨越本桥接。
// ==============================================================================

#include <mutex>
#include <string>

// ------------------------------------------------------------------------------
// 断线事件的投递目标。
//
// 生产实现是 LogicSystem（唯一 FIFO 的拥有者，见 LogicSystem.h：
// LogicSystem::PostSessionDisconnectedWork）。测试实现是直接调用真实
// SerializedWorkExecutor 的替身。二者投递的是**同一种 work item**
// （MakeSessionDisconnectedWork），差别只有生产实现还做一次惰性启动。
//
// 接口只接受一个稳定 SessionId 值：
//   - 实现方负责把它变成 SESSION_DISCONNECTED work item 并投入唯一 FIFO；
//   - 返回 false 表示 FIFO 已停止接收（shutdown 路径），不是业务错误。
//
// 参数类型刻意写成 std::string 而不是 meeting::SessionId：两者是同一个类型
// （M1 的 SessionId 就是 std::string 的别名），但这样写可以让网络层对领域头文件
// 零依赖。这不是"绕过类型"：生产实现 LogicSystem::PostSessionDisconnectedWork
// 的形参仍然是 meeting::SessionId，并由 override 在编译期保证两者一致。
// ------------------------------------------------------------------------------
class ISessionDisconnectSink {
public:
    virtual ~ISessionDisconnectSink() {}

    // 参数是稳定 SessionId 的值拷贝；实现方不得保存连接对象、socket 或 Session 指针。
    virtual bool PostSessionDisconnectedWork(const std::string& session_id) = 0;
};

// ------------------------------------------------------------------------------
// 一次投递的结果。
//
// 刻意用具名枚举而不是 bool：调用方与测试必须能区分
//   "本次真的投递了一次" / "重复断线被幂等吞掉" / "队列已停止而拒绝"
// 这三种语义完全不同的情况（bool 会把后两者混在一起）。
// ------------------------------------------------------------------------------
enum class SessionDisconnectDispatch {
    POSTED = 0,          // 本次调用真正向唯一 FIFO 投递了一次
    ALREADY_POSTED = 1,  // 此前已投递过：重复断线不再产生第二个事件
    REJECTED = 2         // 首次投递，但目标不可用：未绑定，或 FIFO 已停止接收
};

// ------------------------------------------------------------------------------
// 一次性（exactly-once）断线桥接。
//
// 典型生命周期（与 CSession 的实际用法一致）：
//   SessionDisconnectBridge bridge;            // 默认未绑定
//   bridge.Bind(sink, session_id);             // 会话建立时绑定稳定 SessionId
//   bridge.NotifyDisconnected();               // 每个终止边界都可以调用
//
// 性质（这些性质就是 M5 的验收内容）：
//   1. Bind 最多成功一次；重复绑定不改变既有绑定，返回 false；
//   2. NotifyDisconnected 最多只投递一次，重复/并发调用返回 ALREADY_POSTED；
//   3. 投递在**锁外**执行，因此本桥接的 mutex 绝不与队列锁 / 空闲等待锁嵌套；
//   4. 不复制、不移动、不持有任何会话对象；只保存稳定 SessionId 的值。
// ------------------------------------------------------------------------------
class SessionDisconnectBridge {
public:
    SessionDisconnectBridge() : sink_(nullptr), bound_(false), posted_(false) {}

    SessionDisconnectBridge(const SessionDisconnectBridge&) = delete;
    SessionDisconnectBridge& operator=(const SessionDisconnectBridge&) = delete;

    // 绑定稳定 SessionId（值拷贝）与投递目标。
    // sink 为空或 session_id 为空时返回 false 且不改变状态；已绑定过时返回 false。
    bool Bind(ISessionDisconnectSink* sink, const std::string& session_id) {
        if (sink == nullptr || session_id.empty()) {
            return false;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (bound_) {
            return false;
        }
        sink_ = sink;
        session_id_ = session_id;
        bound_ = true;
        return true;
    }

    // 生命周期终止边界调用；可重复、可并发调用。
    //
    // 这里是**唯一**的断线投递点：进程内不存在"网络线程直接调用 MeetingService"
    // 的第二条路径，也不存在第二条队列。
    SessionDisconnectDispatch NotifyDisconnected() {
        ISessionDisconnectSink* sink = nullptr;
        std::string session_id;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!bound_) {
                return SessionDisconnectDispatch::REJECTED;
            }
            if (posted_) {
                return SessionDisconnectDispatch::ALREADY_POSTED;
            }
            // 先在锁内把"已投递"定下来：并发调用中只有一个线程能走到投递，
            // 其余线程即使随后到达也只会得到 ALREADY_POSTED。
            posted_ = true;
            sink = sink_;
            session_id = session_id_;
        }

        // 锁外投递：队列锁 / 空闲等待绝不与桥接锁嵌套（M4 §6.3(3) 的锁序纪律）。
        if (!sink->PostSessionDisconnectedWork(session_id)) {
            // 队列已停止接收（shutdown 路径）。这里**刻意不重试**：
            // M4 §11.2 的语义是"stop 之后到达的外部 work 一律丢弃"，
            // 此时网络消息本来就已经失去处理意义，重试只会制造第二条队列的等价物。
            return SessionDisconnectDispatch::REJECTED;
        }
        return SessionDisconnectDispatch::POSTED;
    }

    // ---- 只读诊断（供日志与测试；不参与任何业务决策）--------------------------

    bool AlreadyPosted() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return posted_;
    }

    bool IsBound() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return bound_;
    }

    // 绑定时的稳定 SessionId 快照；未绑定时为空串。
    std::string BoundSessionId() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return session_id_;
    }

private:
    mutable std::mutex mutex_;

    // 投递目标。生产是 LogicSystem 单例（进程级生存期），不是 Session 对象。
    ISessionDisconnectSink* sink_;

    bool bound_;
    bool posted_;

    // 稳定 SessionId 的值拷贝：断线事件只携带它。
    std::string session_id_;
};
