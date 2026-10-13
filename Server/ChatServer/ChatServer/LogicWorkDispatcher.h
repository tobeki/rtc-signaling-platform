#pragma once

// ==============================================================================
// M4 统一串行化 Work Queue / Executor / Dispatcher（Phase 1 Step 1.6 §12）。
//
// 本文件承载**生产代码真正使用**的唯一 FIFO 实现与 Meeting 分发器。
//
// 为什么把它单独成文件（Step 1.6 §12.5「不做大规模 LogicSystem 重写」）：
//   - LogicSystem 是 Singleton，且它的构造/业务路径会拉起 Redis / MySQL / gRPC；
//     若把队列与分发逻辑写在 LogicSystem 内部，Layer B 顺序测试就无法在不启动
//     整条旧 IM 业务链路的前提下执行，只能退化成"测试一个假的 executor"。
//   - 因此这里只抽出**一个**可复用组件，并且满足：
//       1. 生产 LogicSystem 真正使用它；
//       2. 测试执行的是同一份队列/分发代码；
//       3. 生产进程内仍然只有一个逻辑 FIFO；
//       4. 不为测试再造一套模拟 executor；
//       5. 不增加第二个生产 worker 线程；
//       6. MeetingDomain 不反向依赖 LogicSystem（本文件依赖 MeetingDomain，反之不成立）。
//
// 依赖边界：MeetingService.h + LogicWorkItem.h + 标准库。
// 不 include LogicSystem.h / CSession.h（只用 LogicNode 的不完整类型），
// 因此本文件的编译单元不需要 asio / Redis / MySQL / gRPC。
// ==============================================================================

#include "LogicWorkItem.h"
#include "meeting/MeetingService.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class LogicNode;

// ------------------------------------------------------------------------------
// 唯一权威 FIFO。
//
// 只负责"存 + 唤醒"：不含分发、不含业务、不含第二个 worker。
// 锁只保护队列本身；本组件**从不**持有该锁去执行任何 handler。
// ------------------------------------------------------------------------------
class LogicWorkQueue {
public:
    // 队列锁的 RAII 封装。消费者循环用它把"等待 + 取出"放在同一个临界区内。
    class Lock {
    public:
        explicit Lock(LogicWorkQueue& queue)
            : queue_(queue), lock_(queue.mutex_) {
            queue_.lock_depth_ += 1;
        }

        ~Lock() { queue_.lock_depth_ -= 1; }

        Lock(const Lock&) = delete;
        Lock& operator=(const Lock&) = delete;

        std::unique_lock<std::mutex>& Raw() { return lock_; }

    private:
        LogicWorkQueue& queue_;
        std::unique_lock<std::mutex> lock_;
    };

    LogicWorkQueue() : stopping_(false), lock_depth_(0) {}

    LogicWorkQueue(const LogicWorkQueue&) = delete;
    LogicWorkQueue& operator=(const LogicWorkQueue&) = delete;

    // 入队。stop 请求之后一律拒绝（返回 false），因此"stop 完成后不再处理新增
    // 外部 Work"是队列层面的结构保证，而不是调用方的约定。
    //
    // 拒绝的语义是"丢弃"：stop 只会在进程关闭路径上被触发，此后到达的网络消息
    // 本来就已经失去处理意义。
    bool Push(const LogicWorkItem& item);

    // 取出队首。队列为空时返回 false。只有消费者线程调用它，且必须持有 Lock。
    bool Pop(LogicWorkItem& out_item);

    // ---- 已持锁版本（调用方必须已经持有 Lock）--------------------------------
    //
    // 这些方法存在的原因是一个真实的踩坑：消费者已经持有队列锁，如果它调用的是
    // 会**再次加锁**的公开版本（IsEmpty / IsStopRequested），就会在同一线程上
    // 自我死锁。因此"持锁时可调用的那一组"被显式命名出来，锁的归属在 API 上
    // 就可以直接读出来。
    bool PushLocked(const LogicWorkItem& item);
    bool IsEmptyLocked() const;
    bool IsStopRequestedLocked() const;
    bool WaitForWorkOrStopLocked(Lock& lock);

    // 在一次或一批入队结束后唤醒消费者（锁外调用）。
    void NotifyWorkAvailable();

    // ---- drain 期间由**消费者自己**入队的内部 work ---------------------------
    //
    // stop 之后 Push() 一律拒绝，这是"停止后不再接受外部 work"的结构保证。
    // 但 Close 在 drain 过程中派生出的 FINALIZE_CLOSE 属于**已经受理**的工作的
    // 必要后续，不能因为此刻已经在关闭而被静默丢弃（否则会出现"Close 返回
    // ENDING 之后 Meeting 永远停在 ENDING"的悬挂状态）。
    //
    // 因此给出这条唯一例外，并且它的语义被刻意收紧：
    //   - 只允许消费者线程调用；
    //   - 只影响"内部 work"，生产者侧仍然只能走会拒绝的 Push()。
    //
    // 命名上带 Locked 后缀是刻意为之：**调用方必须已经持有队列锁**。
    // （早期版本在内部又取了一次锁，而调用方也已持锁，于是同一线程重复加同一
    //   非递归 mutex，直接把自己锁死。这个坑在本步出现过两次，故用命名固化契约。）
    bool PushInternalDuringDrainLocked(const LogicWorkItem& item);

    bool IsEmpty() const;
    std::size_t Size() const;

    // 只读快照（用于观测/诊断）：在队列锁下复制当前队列内容。
    std::vector<LogicWorkItem> SnapshotForDiagnostics() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return std::vector<LogicWorkItem>(items_.begin(), items_.end());
    }

    // 请求停止：置标志并唤醒消费者。可重复调用。
    void RequestStop();
    bool IsStopRequested() const;

    // 本队列当前是否处于加锁状态。仅供断言使用
    // （例如"dispatch 期间不得持有队列锁"），不参与任何业务逻辑。
    //
    // 只有消费者线程才会加锁，断言也在该线程上执行，因此读这个计数是安全的；
    // 写成 instance 成员（而不是 thread_local 静态成员）才能正确区分同一线程上
    // 的多个队列实例。
    bool IsLockHeldByCurrentThread() const { return lock_depth_ > 0; }

private:
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<LogicWorkItem> items_;
    bool stopping_;

    // 队列锁的持有计数。只有消费者线程会修改它，因此不需要原子性。
    int lock_depth_;
};

// ------------------------------------------------------------------------------
// 内部 work 的入队通道。
//
// 存在的理由：internal work（例如 Close 派生的 FINALIZE_CLOSE）必须进入**同一**
// FIFO，而且必须与"已经接受的 work 计数"一起记账，否则确定性的空闲判定会失真。
// 由 executor 实现它，handler 只负责发起。
// ------------------------------------------------------------------------------
class IInternalWorkSink {
public:
    virtual ~IInternalWorkSink() {}
    virtual bool EnqueueInternalDuringDrain(const LogicWorkItem& item) = 0;
};

// ------------------------------------------------------------------------------
// work 执行接口（消费者线程上执行）。
//
// 刻意定义成纯虚接口而不是 std::function<void()>：
//   - work item 本身始终是显式 typed value；
//   - 执行体是具名方法，测试可替换成记录型实现而**不复制**任何生产逻辑。
// ------------------------------------------------------------------------------
class IWorkHandler {
public:
    virtual ~IWorkHandler() {}

    // 绑定唯一 FIFO。由 SerializedWorkExecutor 构造时注入。
    // handler 需要"向同一队列尾部追加 internal work"（例如 Close → FINALIZE_CLOSE）
    // 时必须通过它入队，而不得直接执行，也不得投递到别的队列。
    void SetWorkQueue(LogicWorkQueue* queue) { work_queue_ = queue; }

    // 内部 work 的入队通道（由 executor 提供）。生产与测试都会设置。
    void SetInternalWorkSink(IInternalWorkSink* sink) { internal_work_sink_ = sink; }

    // 执行一个 work item。**必须在持有队列锁之外调用**。
    virtual void DispatchWork(const LogicWorkItem& item) = 0;

    // 领域 Session 生命周期（内部 kind 的执行体）。
    virtual void RegisterConnectedSession(const meeting::SessionId& session_id) = 0;
    virtual void AuthenticateSession(const meeting::SessionId& session_id,
                                     meeting::UserId user_id) = 0;
    virtual void SessionDisconnected(const meeting::SessionId& session_id) = 0;

    IInternalWorkSink* internal_work_sink_ = nullptr;

protected:
    // 子类用它把 internal work 追加到**同一** FIFO 尾部。
    // 该入口在关闭（drain）阶段仍然有效：内部 work 是已受理工作的必要后续，
    // 不能在关闭时丢失。外部 work 仍然只能走会拒绝的 Push()。
    //
    // 它把内部 work 交给 **executor**（而不是直接操作队列），这样"入队计数"
    // 与"单一入队点"都只存在于一处，避免出现"队列里有 work 但计数不知道"的状态，
    // 那会让确定性的空闲判定失真。
    bool PostInternalWork(const LogicWorkItem& item) {
        if (internal_work_sink_ == nullptr) {
            if (work_queue_ == nullptr) {
                return false;
            }
            // 兜底路径：没有 executor 时直接操作队列（测试用），
            // 因此这里必须自己持有队列锁。
            LogicWorkQueue::Lock lock(*work_queue_);
            return work_queue_->PushInternalDuringDrainLocked(item);
        }
        return internal_work_sink_->EnqueueInternalDuringDrain(item);
    }

    LogicWorkQueue* work_queue_ = nullptr;
};

// ------------------------------------------------------------------------------
// legacy IM 消息路由接口。
//
// 生产实现就是既有 LogicSystem::_fun_callbacks 的查表路径；测试实现是记录型替身。
// 这样"legacy 消息确实被路由到既有分发路径"可以被断言，而不需要在测试中拉起
// Redis / MySQL。
// ------------------------------------------------------------------------------
class ILegacyMessageRouter {
public:
    virtual ~ILegacyMessageRouter() {}

    // 语义必须与既有 DealMsg 内联代码一致：查表，命中则调用，未命中忽略。
    virtual void RouteExternalMessage(const std::shared_ptr<LogicNode>& node) = 0;
};

// ------------------------------------------------------------------------------
// "空闲进度"快照：用于确定性等待"某一批 work 已经跑完"。
//
// 它只携带一个下界：入队那一刻"已被队列接受"的 work 数量。
// 判定见 IsIdleLocked：consumed_count_（与 Pop 同临界区自增）追上该下界，
// 且队列为空、无在途时，才认为这一批（含其派生的内部 work）已经处理完。
//
// 早期实现曾用"消费者停放次数"作为判据，会被**更早的一次停放**满足，使等待在本批
// work 执行之前就返回（表现为断言看到空结果、随后析构里的 join 才把剩余工作跑完，
// 用例因此偶发多花 5s）。以"已消费计数"为下界则不受时序影响。
// ------------------------------------------------------------------------------
struct LogicWorkIdleProgress {
    // "截至入队那一刻已被接受"的 work 计数下界。
    //
    // 判定用的是 consumed_count_（已从队列取出），它在**取出队首的同一临界区内**
    // 自增。因此只要队列已空且无在途，consumed 必然已经追上该下界，
    // 不存在"已出队但计数还没跟上"的窗口（这个窗口曾导致等待偶发超时 5s）。
    unsigned long long enqueued = 0;
};

// ------------------------------------------------------------------------------
// 单一消费者 + 单一 FIFO 的执行器。
//
// 锁边界（本步最重要的结构约束）：
//
//   for (;;) {
//       在锁内：等待 / 判断 stop 且 drain 完成 / 取出队首 work
//       在锁外：分发（legacy callback 或 MeetingService mutation）
//   }
//
// 因此 dispatch 期间绝不会持有队列锁：
//   - 不会自我死锁；
//   - 分发可以在中途向同一 FIFO 再入队（Close → FINALIZE_CLOSE）；
//   - 仍然只有一个消费者线程，Meeting mutation 不会并发。
//
// 两把锁的锁序纪律（本步真实踩过两次，务必保持）：
//
//   1. queue mutex → idle_mutex_  **绝对禁止**。
//      消费者在持有队列锁时只允许使用 atomic 计数；一旦在那里获取 idle_mutex_，
//      就会与入队路径（idle_mutex_ → queue mutex）形成锁序反转并死锁。
//   2. idle_mutex_ → queue mutex  是允许的，也是入队路径的唯一顺序。
//
// 条件变量纪律：任何被 wait 谓词读取的标志，其"写 + notify"都必须与谓词判定
// 处于同一把 mutex 之下，否则会出现 lost wakeup（本步真实表现为个别用例偶发
// 5000ms 才返回）。因此 gate_held_ / worker_exited_ 都由 idle_mutex_ 保护，
// 而 consumed_count_ / in_flight_ 因为要跨锁读取，使用 atomic。
//
// 关闭（stop/drain）策略：
//   - stopping 标志与队列内容由同一个队列 mutex 协调（无数据竞争）；
//   - stop 之后**先排空**队列再退出，因此"Close 已排入的 FINALIZE_CLOSE 不会在
//     stop 时被静默丢弃"；
//   - 最终 break 的条件同时是"已 stop **且** 队列为空"，所以分发过程中新追加
//     的 work 也一定会被处理；
//   - RequestStop 只置标志并唤醒，绝不持锁 join。
// ------------------------------------------------------------------------------
template <typename THandler>
class SerializedWorkExecutor : public IInternalWorkSink {
public:
    SerializedWorkExecutor(THandler& handler, ILegacyMessageRouter* legacy_router)
        : handler_(handler),
          legacy_router_(legacy_router),
          in_flight_(0),
          enqueued_count_(0),
          consumed_count_(0),
          gate_held_(false),
          worker_exited_(false) {
        handler_.SetWorkQueue(&queue_);
        handler_.SetInternalWorkSink(this);
        // 线程在构造体最后启动：此时所有成员都已完成初始化。
        worker_ = std::thread(&SerializedWorkExecutor::WorkerLoop, this);
    }

    ~SerializedWorkExecutor() { StopAndJoin(); }

    SerializedWorkExecutor(const SerializedWorkExecutor&) = delete;
    SerializedWorkExecutor& operator=(const SerializedWorkExecutor&) = delete;

    // ---------------- 入队入口（生产者线程可并发调用） ----------------

    bool Enqueue(const LogicWorkItem& item) {
        std::lock_guard<std::mutex> lock(idle_mutex_);
        return EnqueueLocked(item);
    }

    // 一次性投递一整批，并在**同一把 idle_mutex_** 之下返回"投递前的空闲进度"。
    //
    // 这是"投递完这批 work 之后才断言"的正确原语：
    //   const LogicWorkIdleProgress base = ex.EnqueueBatchBaseline(items);
    //   ASSERT_TRUE(ex.WaitUntilIdle(5000, base));
    //
    // 若改为"先入队、再单独取进度"，就会与消费者赛跑，等待可能提前返回。
    LogicWorkIdleProgress EnqueueBatchBaseline(const std::vector<LogicWorkItem>& items) {
        std::lock_guard<std::mutex> lock(idle_mutex_);
        LogicWorkIdleProgress progress;
        progress.enqueued = enqueued_count_.load();
        EnqueueBatchLocked(items);
        return progress;
    }

    bool EnqueueBatch(const std::vector<LogicWorkItem>& items) {
        std::lock_guard<std::mutex> lock(idle_mutex_);
        return EnqueueBatchLocked(items);
    }

    // legacy IM 消息：包装成 EXTERNAL_MESSAGE 后进入**同一** FIFO。
    // 这不是"为 internal command 编造 TCP message"，方向正好相反：
    // 真实的 TCP 消息被降级为统一 work item 的一个 kind。
    bool PostExternalMessage(const std::shared_ptr<LogicNode>& node) {
        return Enqueue(MakeExternalMessageWork(node));
    }

    bool PostMeetingCommand(const meeting::MeetingRequestContext& context,
                            ClientMeetingCommandKind command,
                            const meeting::MeetingId& meeting_id) {
        return Enqueue(MakeMeetingCommandWork(context, command, meeting_id));
    }

    bool PostSessionDisconnected(const meeting::SessionId& session_id) {
        return Enqueue(MakeSessionDisconnectedWork(session_id));
    }

    bool PostFinalizeClose(const meeting::MeetingId& meeting_id) {
        return Enqueue(MakeFinalizeCloseWork(meeting_id));
    }

    bool PostSessionRegister(const meeting::SessionId& session_id) {
        return Enqueue(MakeSessionRegisterWork(session_id));
    }

    bool PostSessionAuthenticate(const meeting::SessionId& session_id, meeting::UserId user_id) {
        return Enqueue(MakeSessionAuthenticateWork(session_id, user_id));
    }

    // 内部 work 入队（IInternalWorkSink）。
    // 刻意**不**受 stop 影响：它表达的是"已受理工作的必要后续"，丢弃会让 Meeting
    // 永久停在 ENDING。外部 work 仍然只能走会拒绝的 Push()。
    // 只有消费者线程会调用它。
    bool EnqueueInternalDuringDrain(const LogicWorkItem& item) {
        bool accepted = false;
        {
            LogicWorkQueue::Lock lock(queue_);
            accepted = queue_.PushInternalDuringDrainLocked(item);
            if (accepted) {
                // 与队列内容在同一临界区内记账：等待方不会看到"队列里有 work
                // 但 dispatched 已经追平 enqueued"的假静止。
                enqueued_count_.fetch_add(1);
            }
        }
        if (accepted) {
            // 入队瞬间的队列内容就是"这条内部 work 排在第几"的确定答案。
            // sink 契约是纯观测：实现绝不能再回调进队列。
            if (internal_enqueue_observer_ != nullptr) {
                internal_enqueue_observer_->RecordInternalWorkEnqueued(
                    item, queue_.SnapshotForDiagnostics());
            }
            queue_.NotifyWorkAvailable();
        }
        return accepted;
    }

    // 观测：内部 work 入队瞬间的队列快照。生产不设置。
    void SetInternalEnqueueObserver(IWorkObservationSink* sink) {
        internal_enqueue_observer_ = sink;
    }

    // ---------------- 测试 / 诊断入口 ----------------

    // 确定性等待："截至 baseline 的 work 都已出队执行 + 队列为空 + 无在途 +
    // 消费者已进入空闲等待"。
    //
    // 默认 baseline（dispatched = 0）表示"只要消费者至少跑过一轮"，
    // 适合"现在是否静止"的常规确认；确认某一批 work 跑完请传入
    // EnqueueBatchBaseline 的返回值。
    // 不使用 sleep，也不依赖"跑一会儿应该就好了"的竞态。
    bool WaitUntilIdle(unsigned int timeout_ms, const LogicWorkIdleProgress& baseline = LogicWorkIdleProgress()) {
        std::unique_lock<std::mutex> lock(idle_mutex_);
        return idle_condition_.wait_for(
            lock, std::chrono::milliseconds(timeout_ms),
            [this, baseline]() { return IsIdleLocked(baseline); });
    }

    // 请求停止并等待消费者排空后退出。可重复调用；析构也会调用它。
    // 先释放测试门，避免"门未释放 + 停止"这一组合把消费者卡在门后面。
    void StopAndJoin() {
        // 先释放测试门，避免"门未释放 + 停止"这一组合把消费者卡在门后面。
        ReleaseGate();
        queue_.RequestStop();
        if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id()) {
            worker_.join();
        }
    }

    bool IsIdle() {
        std::lock_guard<std::mutex> lock(idle_mutex_);
        return IsIdleLocked(LogicWorkIdleProgress());
    }

    // ---- 测试门（gate）----------------------------------------------------
    // 持门期间 "idle" 恒为 false，用于"明确不希望被判定为空闲"的场景。
    //
    // 纪律（踩过坑，勿改）：
    //   - 标志的写与 notify 都在 idle_mutex_ 之下，与谓词判定同锁，
    //     因此不会出现 lost wakeup；
    //   - 调用方**不得**在持门期间调用 Enqueue / EnqueueBatch：那会在持有
    //     idle_mutex_ 时再去取队列锁。虽然该方向本身是允许的锁序，但会与
    //     消费者长时间持锁形成不必要的等待。需要"投递后断言"请用
    //     EnqueueBatchBaseline（它同样在 idle_mutex_ 之下取基线，且不设门）。
    void HoldGate() {
        std::lock_guard<std::mutex> lock(idle_mutex_);
        gate_held_ = true;
    }

    void ReleaseGate() {
        std::lock_guard<std::mutex> lock(idle_mutex_);
        gate_held_ = false;
        idle_condition_.notify_all();
    }

    LogicWorkQueue& Queue() { return queue_; }
    const LogicWorkQueue& Queue() const { return queue_; }

private:
    // 调用方必须持有 idle_mutex_。锁序：idle_mutex_ → queue mutex。
    bool EnqueueLocked(const LogicWorkItem& item) {
        bool accepted = false;
        {
            LogicWorkQueue::Lock lock(queue_);
            // 持队列锁之后只能调用 *Locked 版本，否则会自死锁。
            accepted = queue_.PushLocked(item);
            if (accepted) {
                enqueued_count_.fetch_add(1);
            }
        }
        if (accepted) {
            queue_.NotifyWorkAvailable();
        }
        return accepted;
    }

    // 调用方必须持有 idle_mutex_。整批在**一次**队列锁内可见。
    bool EnqueueBatchLocked(const std::vector<LogicWorkItem>& items) {
        bool accepted = true;
        bool pushed_any = false;
        {
            LogicWorkQueue::Lock lock(queue_);
            for (std::size_t index = 0; index < items.size(); ++index) {
                if (!queue_.PushLocked(items[index])) {
                    accepted = false;
                    break;
                }
                enqueued_count_.fetch_add(1);
                pushed_any = true;
            }
        }
        if (pushed_any) {
            queue_.NotifyWorkAvailable();
        }
        return accepted;
    }

    // 调用方必须持有 idle_mutex_。gate_held_ / worker_exited_ 属该域；
    // consumed_count_ / in_flight_ 是 atomic，因此消费者可在持队列锁时更新。
    bool IsIdleLocked(const LogicWorkIdleProgress& baseline) const {
        if (gate_held_ || worker_exited_ || !queue_.IsEmpty() || in_flight_.load() != 0) {
            return false;
        }
        // 关键判定："所有已被接受的 work 都已经从队列取出"。
        //   - 空执行器：enqueued 与 consumed 都是 0，立即为真；
        //   - 投递后：必须等到 consumed 追上入队时的下界，因此不会被"更早的某次
        //     停放"或"瞬时空队列"提前满足；
        //   - 派生的内部 work（FINALIZE_CLOSE 等）会再次抬高 enqueued，
        //     因此它也必须被取出，等待才可能成立。
        if (consumed_count_.load() < enqueued_count_.load()) {
            return false;
        }
        return consumed_count_.load() >= baseline.enqueued;
    }

    void WorkerLoop() {
        for (;;) {
            LogicWorkItem item;

            {
                LogicWorkQueue::Lock lock(queue_);

                // 队列为空且未请求停止 → 即将让出 CPU。
                // 注意：这里已经持有队列锁，因此只能调用 *Locked 版本，
                // 而且只允许使用 atomic 计数（不得获取 idle_mutex_）。
                // 空闲判定所需的"在途归零"由 DispatchOutsideLock 在 idle_mutex_
                // 之下发布，因此这里只需要进入等待即可。
                queue_.WaitForWorkOrStopLocked(lock);

                // 结束条件：已请求停止**且**队列已排空。
                if (queue_.IsStopRequestedLocked() && queue_.IsEmptyLocked()) {
                    break;
                }

                if (!queue_.Pop(item)) {
                    continue;
                }

                // 与"取出队首"在同一临界区内登记在途与已消费计数：
                //   - consumed 与 Pop 同临界区 => 不存在"已取出但计数未更新"的窗口；
                //   - in_flight 与 Pop 同临界区 => 不存在"队列已空 + 无在途"的假空闲。
                in_flight_.fetch_add(1);
                consumed_count_.fetch_add(1);
            }

            // ---------------- 队列锁已释放 ----------------
            DispatchOutsideLock(item);
        }

        // 消费者退出：持 idle_mutex_ 发布，等待者不会漏掉这次通知。
        {
            std::lock_guard<std::mutex> idle_lock(idle_mutex_);
            worker_exited_ = true;
        }
        idle_condition_.notify_all();
    }

    // 明确命名的"锁外分发"：legacy callback 与 MeetingService mutation 都在这里，
    // 而这里绝不持有队列锁。
    void DispatchOutsideLock(const LogicWorkItem& item) {
        if (item.kind == LogicWorkKind::EXTERNAL_MESSAGE) {
            // legacy 路径保持原样：仍然按既有 msg_id 查既有 callback 表。
            if (legacy_router_ != nullptr) {
                legacy_router_->RouteExternalMessage(item.external_logic_node);
            }
        } else {
            handler_.DispatchWork(item);
        }

        // 关键：把"在途归零"这一**等待者所依赖的状态变化**，与 notify 一起放在
        // idle_mutex_ 之下发布。
        //
        // 若像早期实现那样只做 in_flight_.fetch_sub 再 notify_all，就会出现
        // 经典 lost wakeup：等待方在收到通知之后才挂起（通知早于挂起即被丢弃），
        // 而它判定谓词时 in_flight 仍为 1，于是只能一直等到 5s 超时。
        // 本步真实表现为"个别用例偶发 5000ms 才返回"，且超时时谓词其实已经满足。
        //
        // 这里处于队列锁之外，因此取 idle_mutex_ 是安全的（允许的锁序）。
        {
            std::lock_guard<std::mutex> idle_lock(idle_mutex_);
            in_flight_.fetch_sub(1);
            idle_condition_.notify_all();
        }
    }

    LogicWorkQueue queue_;
    THandler& handler_;
    ILegacyMessageRouter* legacy_router_;

    std::thread worker_;

    mutable std::mutex idle_mutex_;
    std::condition_variable idle_condition_;
    std::atomic<std::size_t> in_flight_;
    std::atomic<unsigned long long> enqueued_count_;
    std::atomic<unsigned long long> consumed_count_;
    IWorkObservationSink* internal_enqueue_observer_ = nullptr;
    bool gate_held_;
    bool worker_exited_;
};

// ------------------------------------------------------------------------------
// 生产 Meeting 分发器。
//
// 所有权（Step 1.6 §6.4、M3 §3，以及本步 §9）：
//   LogicSystem execution context 拥有 MeetingService，并串行化它的**全部** mutation。
//
// MeetingService 是普通成员（不是 Singleton、也不是 shared_ptr），因为 Single-Writer
// 的前提正是"只有一个明确的 owner"。它自己不加锁、不建线程、不发消息。
// 真实网络层不得持有它，也不得绕开 FIFO 直接调用它的 mutation。
//
// ID / clock 注入：MakeSystemMeetingWorkDependencies() 提供真实的 meeting id /
// event id / clock；测试注入固定序列。MeetingService 内部仍然不允许出现
// boost::uuids、system_clock::now()、全局序列号或 rand()。
// ------------------------------------------------------------------------------
meeting::MeetingServiceDependencies MakeSystemMeetingWorkDependencies();

class LogicWorkDispatcher : public IWorkHandler {
public:
    LogicWorkDispatcher(const meeting::ServerId& local_server_id,
                        meeting::MeetingServiceDependencies dependencies,
                        IWorkObservationSink* sink);

    // IWorkHandler
    void DispatchWork(const LogicWorkItem& item);
    void RegisterConnectedSession(const meeting::SessionId& session_id);
    void AuthenticateSession(const meeting::SessionId& session_id, meeting::UserId user_id);
    void SessionDisconnected(const meeting::SessionId& session_id);

    const meeting::ServerId& LocalServerId() const { return meeting_service_.LocalServerId(); }

    // 只读观测入口。刻意不提供 GetActiveRegistry() 或任何可变 registry 访问。
    bool HasActiveMeeting(const meeting::MeetingId& meeting_id) const {
        return meeting_service_.HasActiveMeeting(meeting_id);
    }
    bool HasClosedSnapshot(const meeting::MeetingId& meeting_id) const {
        return meeting_service_.HasClosedSnapshot(meeting_id);
    }

    // CREATE 命令不携带 meeting_id（ID 由注入来源产生），因此把最近一次成功创建
    // 的 ID 作为只读值暴露出来，供观测点与控制流关联使用。
    // 这是值拷贝，不是 registry 访问，也不构成任何可变入口。
    const meeting::MeetingId& LastCreatedMeetingId() const { return last_created_meeting_id_; }

private:
    // 六项 API 的 typed 分发：只消费 M3 的 API，不重新实现任何领域判断。
    void DispatchCreate(const LogicWorkItem& item);
    void DispatchJoin(const LogicWorkItem& item);
    void DispatchLeave(const LogicWorkItem& item);
    void DispatchClose(const LogicWorkItem& item);
    void DispatchQuery(const LogicWorkItem& item);
    void DispatchListParticipants(const LogicWorkItem& item);

    // M3 结果 → 观测用最小摘要。
    static MeetingCommandOutcome MakeCommandOutcome(ClientMeetingCommandKind command,
                                                    const meeting::MeetingId& meeting_id,
                                                    const meeting::MeetingServiceResultCommon& common);

    static std::vector<WorkDomainEvent> ToWorkDomainEvents(const std::vector<meeting::MeetingEvent>& events);

    static void FillEventTypes(const std::vector<meeting::MeetingEvent>& events,
                               MeetingCommandOutcome& outcome);

    meeting::MeetingService meeting_service_;
    IWorkObservationSink* sink_;
    meeting::MeetingId last_created_meeting_id_;
};
