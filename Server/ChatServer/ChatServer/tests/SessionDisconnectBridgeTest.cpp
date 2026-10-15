// ==============================================================================
// M5 Transport Disconnect Bridge 单元测试。
//
// 覆盖范围（本步任务要求逐项对应）：
//   Disconnect Bridge : connected session / authenticated session / repeated（含并发）
//   Ordering          : message → disconnect → message（真实 FIFO 顺序，不越过、不重排）
//   Lifecycle         : Leave→Disconnect / Disconnect→Leave / Close→Disconnect
//                       / Disconnect during ENDING / Disconnect after CLOSED
//   Shutdown          : queued disconnect work / graceful shutdown / 不丢 work
//
// 测试切口（与 M4 §12 同一哲学：测试执行的是**生产实现本身**，不复制任何生产逻辑）：
//
//   生产:  CSession::Close() / 终止分支
//              └── SessionDisconnectBridge（本文件第 1 组直接测试它）
//                        └── ISessionDisconnectSink = LogicSystem（唯一 FIFO 的拥有者）
//
//   测试:  同一个 SessionDisconnectBridge
//              └── ISessionDisconnectSink 替身（ExecutorDisconnectSink）
//                        └── SerializedWorkExecutor<LogicWorkDispatcher>
//                              = 真实队列 + 真实单消费者 + 真实 MeetingService
//
// 刻意**不**进入本 target 的：CSession.cpp / CServer.cpp / LogicSystem.cpp。
// 它们会把 asio / Boost / Redis / MySQL / gRPC 整条 legacy 业务链路拉进纯单元测试
// （Step 2.5 §14、M4 §18）。因此本文件证明的是"桥接语义 + FIFO 中的位置 + 领域结果"，
// 而**不是** socket 上的字节流：真实 CSession 侧的接入点是静态 reviewed 的一行调用
// （Close() / 非法头部终止分支），其验证边界与 M4 §20 保持同一口径。
// ==============================================================================

#include "LogicWorkDispatcher.h"
#include "SessionDisconnectBridge.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

// ------------------------------------------------------------------------------
// 固定 ID / clock 注入（MeetingService 内部禁止自己取时间或生成 UUID）。
// ------------------------------------------------------------------------------
struct FakeIdFactory {
    int next_meeting = 1;
    int next_event = 1;
    long long base_epoch_seconds = 1700000000;

    meeting::MeetingServiceDependencies Dependencies() {
        meeting::MeetingServiceDependencies deps;
        deps.next_meeting_id = [this]() {
            return std::string("m-") + std::to_string(next_meeting++);
        };
        deps.next_event_id = [this]() {
            return std::string("e-") + std::to_string(next_event++);
        };
        deps.now = [this]() {
            return meeting::Timestamp(std::chrono::seconds(base_epoch_seconds + next_event));
        };
        return deps;
    }
};

// ------------------------------------------------------------------------------
// 测试固定值（与 M4 的固定装置保持同名同值，便于对照阅读）。
// ------------------------------------------------------------------------------
const char* kLocalServer = "local-chatserver";
const char* kHostSession = "s-host";
const char* kBobSession = "s-bob";
const char* kRawSession = "s-raw";
const char* kMeetingOne = "m-1";
const int kHostUser = 1;
const int kBobUser = 2;

meeting::MeetingRequestContext MakeContext(const std::string& request_id,
                                           const std::string& session_id,
                                           meeting::UserId user_id) {
    meeting::MeetingRequestContext context;
    context.request_id = request_id;
    context.session_id = session_id;
    context.authenticated_user_id = user_id;
    context.handling_chat_server_id = kLocalServer;
    context.has_device_id = false;
    context.has_trusted_owner_chat_server_id = false;
    return context;
}

meeting::MeetingRequestContext HostContext(const std::string& request_id) {
    return MakeContext(request_id, kHostSession, kHostUser);
}

meeting::MeetingRequestContext BobContext(const std::string& request_id) {
    return MakeContext(request_id, kBobSession, kBobUser);
}

// ------------------------------------------------------------------------------
// 观测 sink。
//
// 关键点：legacy 消息与内部 work 记录进**同一个有序列表** events_in_order，
// 因此"消息 / 断线 / 消息"的相对顺序可以在一个列表上直接断言，
// 而不需要比较两个各自有序、彼此无序的容器。
// ------------------------------------------------------------------------------
class M5RecordingSink : public IWorkObservationSink {
public:
    struct DisconnectRecord {
        meeting::SessionId session_id;
        std::size_t affected_meeting_count = 0;
        std::size_t event_count = 0;
    };

    struct FinalizeRecord {
        meeting::MeetingId meeting_id;
        bool finalized = false;
        bool already_finalized = false;
        std::vector<meeting::SessionId> notification_session_ids;
    };

    void RecordWorkDispatched(const LogicWorkItem& item,
                              unsigned long long,
                              const std::vector<WorkDomainEvent>& events) {
        events_in_order.push_back(item.Describe());
        dispatched_kinds.push_back(item.kind);
        for (std::size_t index = 0; index < events.size(); ++index) {
            event_types.push_back(events[index].type);
        }
        // 只有单消费者线程会写这里；SessionDisconnected 的两次 sink 回调在同一个
        // 函数内相邻发生，因此这个暂存量就是"该 turn 的领域事件数"。纯观测。
        last_dispatch_event_count_ = events.size();
    }

    void RecordLegacyMessageRouted(const meeting::SessionId& session_id,
                                   short msg_id,
                                   std::size_t) {
        events_in_order.push_back("EXTERNAL_MESSAGE(session_id=" + session_id +
                                 " msg_id=" + std::to_string(msg_id) + ")");
    }

    void RecordSessionDisconnected(const meeting::SessionId& session_id,
                                   std::size_t affected_meeting_count) {
        DisconnectRecord record;
        record.session_id = session_id;
        record.affected_meeting_count = affected_meeting_count;
        record.event_count = last_dispatch_event_count_;
        disconnects.push_back(record);
    }

    void RecordFinalizeClose(const meeting::MeetingId& meeting_id,
                             bool finalized,
                             bool already_finalized,
                             const std::vector<meeting::SessionId>& notification_session_ids) {
        FinalizeRecord record;
        record.meeting_id = meeting_id;
        record.finalized = finalized;
        record.already_finalized = already_finalized;
        record.notification_session_ids = notification_session_ids;
        finalizes.push_back(record);
    }

    void RecordMeetingCommand(const MeetingCommandOutcome& outcome) {
        commands.push_back(outcome);
    }

    void RecordInternalWorkEnqueued(const LogicWorkItem& item,
                                    const std::vector<LogicWorkItem>& queue_after) {
        internal_enqueues.push_back(item.Describe() + " -> queue_size=" +
                                   std::to_string(queue_after.size()));
    }

    // --- 断言辅助 -------------------------------------------------------------

    std::size_t CountEventType(meeting::EventType type) const {
        std::size_t count = 0;
        for (std::size_t index = 0; index < event_types.size(); ++index) {
            if (event_types[index] == type) {
                ++count;
            }
        }
        return count;
    }

    std::size_t CountDispatchedKind(LogicWorkKind kind) const {
        std::size_t count = 0;
        for (std::size_t index = 0; index < dispatched_kinds.size(); ++index) {
            if (dispatched_kinds[index] == kind) {
                ++count;
            }
        }
        return count;
    }

    // 最后 count 条记录；用于只关心"尾部顺序"的断言。
    std::vector<std::string> TailOfEvents(std::size_t count) const {
        if (events_in_order.size() <= count) {
            return events_in_order;
        }
        return std::vector<std::string>(events_in_order.end() - count, events_in_order.end());
    }

    const MeetingCommandOutcome& LastCommand() const {
        return commands[commands.size() - 1];
    }

    std::vector<std::string> events_in_order;
    std::vector<LogicWorkKind> dispatched_kinds;
    std::vector<meeting::EventType> event_types;
    std::vector<DisconnectRecord> disconnects;
    std::vector<FinalizeRecord> finalizes;
    std::vector<MeetingCommandOutcome> commands;
    std::vector<std::string> internal_enqueues;

private:
    std::size_t last_dispatch_event_count_ = 0;
};

// ------------------------------------------------------------------------------
// 记录型 legacy 路由器 + **可控阻塞**。
//
// 为什么需要阻塞：M5 的顺序契约是"同一 session 的断线事件不得越过已排队的 legacy
// 消息"。要确定性地构造"消费者正在分发消息 A 时断线事件入队"这一交错，唯一不依赖
// sleep / 时序运气的办法，就是让消费者在分发 A 的过程中停在测试可控的闩锁上。
//
// 这不改变任何队列语义：A 的 dispatch 已经开始（A 已被 Pop），后续 work 的排队位置
// 仍然完全由真实入队次序决定。
// ------------------------------------------------------------------------------
class M5LegacyRouter : public ILegacyMessageRouter {
public:
    explicit M5LegacyRouter(M5RecordingSink* sink) : sink_(sink) {}

    void RouteExternalMessage(const std::shared_ptr<LogicNode>& node) {
        // LogicNode 在本 target 里刻意是不完整类型（它会把 asio / MsgNode 拖进来），
        // 因此这里只能确认"确实收到了一个 legacy 消息节点"，不能读取其内容。
        (void)node;

        short msg_id = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            msg_id = ++routed_count_;
        }

        // 先记录，再阻塞：保证 A 在有序列表中出现在断线事件之前。
        if (sink_ != nullptr) {
            sink_->RecordLegacyMessageRouted(kBobSession, msg_id, 0);
        }

        std::unique_lock<std::mutex> lock(mutex_);
        if (block_next_) {
            block_next_ = false;
            blocked_entered_ = true;
            condition_.notify_all();
            condition_.wait(lock, [this]() { return released_; });
        }
    }

    // 让下一条被路由的 legacy 消息阻塞在消费者线程上。
    void BlockNextRoute() {
        std::lock_guard<std::mutex> lock(mutex_);
        block_next_ = true;
        released_ = false;
        blocked_entered_ = false;
    }

    bool WaitUntilRouteIsBlocked(unsigned int timeout_ms) {
        std::unique_lock<std::mutex> lock(mutex_);
        return condition_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                                   [this]() { return blocked_entered_; });
    }

    void ReleaseBlockedRoute() {
        std::lock_guard<std::mutex> lock(mutex_);
        released_ = true;
        condition_.notify_all();
    }

    int RoutedCount() {
        std::lock_guard<std::mutex> lock(mutex_);
        return routed_count_;
    }

private:
    M5RecordingSink* sink_;
    std::mutex mutex_;
    std::condition_variable condition_;
    int routed_count_ = 0;
    bool block_next_ = false;
    bool released_ = false;
    bool blocked_entered_ = false;
};

// ------------------------------------------------------------------------------
// 只记录调用的投递目标：用于不经过 FIFO 的"恰好一次"语义断言。
// ------------------------------------------------------------------------------
class RecordingDisconnectSink : public ISessionDisconnectSink {
public:
    bool PostSessionDisconnectedWork(const std::string& session_id) {
        posted.push_back(session_id);
        return accept;
    }

    std::vector<std::string> posted;
    bool accept = true;
};

// ------------------------------------------------------------------------------
// FIFO 投递目标替身。
//
// 它与生产 LogicSystem::PostSessionDisconnectedWork **等价**：
//     return _work_executor->PostSessionDisconnected(session_id);
// 唯一差别是生产实现还先做一次 EnsureWorkQueueStarted() 惰性启动（构造单例队列）。
// 这样测试中"桥接之后发生的一切"都是生产代码：队列、单消费者、LogicWorkDispatcher、
// MeetingService 全部是真身。
// ------------------------------------------------------------------------------
class ExecutorDisconnectSink : public ISessionDisconnectSink {
public:
    explicit ExecutorDisconnectSink(SerializedWorkExecutor<LogicWorkDispatcher>& executor)
        : executor_(executor) {}

    bool PostSessionDisconnectedWork(const std::string& session_id) {
        return executor_.PostSessionDisconnected(session_id);
    }

private:
    SerializedWorkExecutor<LogicWorkDispatcher>& executor_;
};

// ------------------------------------------------------------------------------
// 一个"生产同构"的场景装配：真实桥接 + 真实 executor + 真实 dispatcher + 固定 ID。
// ------------------------------------------------------------------------------
class BridgeScenario {
public:
    // 初始化顺序必须与成员声明顺序一致，否则会产生 -Wreorder 告警。
    BridgeScenario()
        : router_(&sink_),
          ids_(),
          dispatcher_(kLocalServer, ids_.Dependencies(), &sink_),
          executor_(dispatcher_, &router_),
          fifo_sink_(executor_) {
        executor_.SetInternalEnqueueObserver(&sink_);
    }

    M5RecordingSink& Sink() { return sink_; }
    M5LegacyRouter& Router() { return router_; }
    LogicWorkDispatcher& Dispatcher() { return dispatcher_; }
    SerializedWorkExecutor<LogicWorkDispatcher>& Executor() { return executor_; }
    LogicWorkQueue& Queue() { return executor_.Queue(); }

    // 建立与 CSession 内部形态完全一致的桥接：投递目标是唯一 FIFO。
    bool BindBridge(const std::string& session_id) {
        bridge_.reset(new SessionDisconnectBridge());
        return bridge_->Bind(&fifo_sink_, session_id);
    }

    SessionDisconnectBridge& Bridge() { return *bridge_; }

    bool PostAllThenWait(const std::vector<LogicWorkItem>& items) {
        const LogicWorkIdleProgress baseline = executor_.EnqueueBatchBaseline(items);
        return executor_.WaitUntilIdle(5000, baseline);
    }

    bool PostOneThenWait(const LogicWorkItem& item) {
        return PostAllThenWait(std::vector<LogicWorkItem>(1, item));
    }

    void Post(const LogicWorkItem& item) { executor_.Enqueue(item); }

    bool Done() { return executor_.WaitUntilIdle(5000); }

    void StopAndJoin() { executor_.StopAndJoin(); }

    // 在消费者开始处理业务 work 之前一次性投完 Session 准备 work。
    bool Prepare() {
        std::vector<LogicWorkItem> items;
        items.push_back(MakeSessionRegisterWork(kHostSession));
        items.push_back(MakeSessionAuthenticateWork(kHostSession, kHostUser));
        items.push_back(MakeSessionRegisterWork(kBobSession));
        items.push_back(MakeSessionAuthenticateWork(kBobSession, kBobUser));
        return PostAllThenWait(items);
    }

    // 只注册（CONNECTED，未认证）一条领域 Session。
    bool RegisterOnly(const std::string& session_id) {
        return PostOneThenWait(MakeSessionRegisterWork(session_id));
    }

    void CreateMeeting() {
        executor_.PostMeetingCommand(HostContext("m5-create"), ClientMeetingCommandKind::CREATE, "");
    }

    void JoinBob(const std::string& meeting_id) {
        executor_.PostMeetingCommand(BobContext("m5-join"), ClientMeetingCommandKind::JOIN, meeting_id);
    }

private:
    // 声明顺序即初始化/析构顺序：executor_ 必须先于 dispatcher_ 析构（worker 在使用
    // 它），fifo_sink_ / bridge_ 必须先于 executor_ 析构。
    M5RecordingSink sink_;
    M5LegacyRouter router_;
    FakeIdFactory ids_;
    LogicWorkDispatcher dispatcher_;
    SerializedWorkExecutor<LogicWorkDispatcher> executor_;
    ExecutorDisconnectSink fifo_sink_;
    std::unique_ptr<SessionDisconnectBridge> bridge_;
};

} // namespace

// ==============================================================================
// 1. 桥接本体：exactly-once、稳定 SessionId、并发安全
//    （这一组不经过 FIFO，只验证"什么时候投递、投递了什么"）
// ==============================================================================
TEST(SessionDisconnectBridgeLatch, FirstNotifyPostsTheBoundSessionIdExactlyOnce) {
    RecordingDisconnectSink sink;
    SessionDisconnectBridge bridge;

    ASSERT_TRUE(bridge.Bind(&sink, "s-1"));
    EXPECT_TRUE(bridge.IsBound());
    EXPECT_FALSE(bridge.AlreadyPosted());
    EXPECT_EQ("s-1", bridge.BoundSessionId());

    EXPECT_EQ(SessionDisconnectDispatch::POSTED, bridge.NotifyDisconnected());

    ASSERT_EQ(1u, sink.posted.size());
    EXPECT_EQ("s-1", sink.posted[0]);
    EXPECT_TRUE(bridge.AlreadyPosted());
}

TEST(SessionDisconnectBridgeLatch, RepeatedNotifyIsIdempotentAndNeverPostsTwice) {
    RecordingDisconnectSink sink;
    SessionDisconnectBridge bridge;
    ASSERT_TRUE(bridge.Bind(&sink, "s-1"));

    EXPECT_EQ(SessionDisconnectDispatch::POSTED, bridge.NotifyDisconnected());
    EXPECT_EQ(SessionDisconnectDispatch::ALREADY_POSTED, bridge.NotifyDisconnected());
    EXPECT_EQ(SessionDisconnectDispatch::ALREADY_POSTED, bridge.NotifyDisconnected());

    // 重复断线（读错误 + 写错误 + 非法头部 + 重复 Close 都可能到达这里）只产生一次事件。
    EXPECT_EQ(1u, sink.posted.size());
}

TEST(SessionDisconnectBridgeLatch, UnboundBridgeNeverPosts) {
    RecordingDisconnectSink sink;
    SessionDisconnectBridge bridge;

    EXPECT_FALSE(bridge.IsBound());
    EXPECT_EQ(SessionDisconnectDispatch::REJECTED, bridge.NotifyDisconnected());
    EXPECT_FALSE(bridge.AlreadyPosted());
    EXPECT_TRUE(sink.posted.empty());
    EXPECT_TRUE(bridge.BoundSessionId().empty());
}

TEST(SessionDisconnectBridgeLatch, BindIsOneShotAndRejectsMissingTargetOrIdentity) {
    RecordingDisconnectSink sink;
    RecordingDisconnectSink other_sink;
    SessionDisconnectBridge bridge;

    EXPECT_FALSE(bridge.Bind(nullptr, "s-1"));
    EXPECT_FALSE(bridge.Bind(&sink, ""));
    EXPECT_FALSE(bridge.IsBound());

    ASSERT_TRUE(bridge.Bind(&sink, "s-1"));
    // 已绑定：不覆盖既有绑定（SessionId 是构造时就固定的稳定值）。
    EXPECT_FALSE(bridge.Bind(&other_sink, "s-2"));
    EXPECT_EQ("s-1", bridge.BoundSessionId());

    EXPECT_EQ(SessionDisconnectDispatch::POSTED, bridge.NotifyDisconnected());
    ASSERT_EQ(1u, sink.posted.size());
    EXPECT_TRUE(other_sink.posted.empty());
}

TEST(SessionDisconnectBridgeLatch, RejectedFirstPostIsNotRetried) {
    RecordingDisconnectSink sink;
    sink.accept = false;   // 模拟队列已在停止路径上（shutdown）

    SessionDisconnectBridge bridge;
    ASSERT_TRUE(bridge.Bind(&sink, "s-1"));

    EXPECT_EQ(SessionDisconnectDispatch::REJECTED, bridge.NotifyDisconnected());
    ASSERT_EQ(1u, sink.posted.size());

    // 拒绝之后**不重试**：重试等价于为断线事件另建一条投递路径（M4 §11.2）。
    // 因此第二次调用报告的是 ALREADY_POSTED，而不是再尝试一次。
    EXPECT_EQ(SessionDisconnectDispatch::ALREADY_POSTED, bridge.NotifyDisconnected());
    EXPECT_EQ(1u, sink.posted.size());
}

TEST(SessionDisconnectBridgeLatch, ConcurrentNotifyPostsExactlyOnce) {
    RecordingDisconnectSink sink;
    SessionDisconnectBridge bridge;
    ASSERT_TRUE(bridge.Bind(&sink, "s-race"));

    std::atomic<int> posted(0);
    std::atomic<int> already_posted(0);
    std::atomic<int> rejected(0);

    std::vector<std::thread> threads;
    for (int index = 0; index < 8; ++index) {
        threads.push_back(std::thread([&bridge, &posted, &already_posted, &rejected]() {
            switch (bridge.NotifyDisconnected()) {
            case SessionDisconnectDispatch::POSTED:         posted.fetch_add(1); break;
            case SessionDisconnectDispatch::ALREADY_POSTED:  already_posted.fetch_add(1); break;
            case SessionDisconnectDispatch::REJECTED:        rejected.fetch_add(1); break;
            }
        }));
    }
    for (std::size_t index = 0; index < threads.size(); ++index) {
        threads[index].join();
    }

    EXPECT_EQ(1, posted.load());
    EXPECT_EQ(7, already_posted.load());
    EXPECT_EQ(0, rejected.load());
    ASSERT_EQ(1u, sink.posted.size());
    EXPECT_EQ("s-race", sink.posted[0]);
}

// ==============================================================================
// 2. 桥接 → 唯一 FIFO → 单消费者 → MeetingService（全部为生产实现）
// ==============================================================================

// connected session（已注册、未认证）断线。
TEST(SessionDisconnectBridgeFifo, ConnectedSessionDisconnectClosesTheDomainSession) {
    BridgeScenario scenario;
    ASSERT_TRUE(scenario.RegisterOnly(kRawSession));
    ASSERT_TRUE(scenario.BindBridge(kRawSession));

    EXPECT_EQ(SessionDisconnectDispatch::POSTED, scenario.Bridge().NotifyDisconnected());
    ASSERT_TRUE(scenario.Done());

    ASSERT_EQ(1u, scenario.Sink().disconnects.size());
    EXPECT_EQ(kRawSession, scenario.Sink().disconnects[0].session_id);
    EXPECT_EQ(0u, scenario.Sink().disconnects[0].affected_meeting_count);
    EXPECT_EQ(1u, scenario.Sink().CountDispatchedKind(LogicWorkKind::SESSION_DISCONNECTED));

    // 领域侧证据：该 Session 现在必须是 CLOSED（tombstone 保留），因此后续命令得到
    // SESSION_STATE_REJECTED，而不是 AUTH_REQUIRED。这证明断线事件真的被消费者执行过，
    // 而不是被悄悄丢掉。
    ASSERT_TRUE(scenario.PostOneThenWait(MakeMeetingCommandWork(
        MakeContext("m5-raw-create", kRawSession, kBobUser),
        ClientMeetingCommandKind::CREATE, "")));
    ASSERT_FALSE(scenario.Sink().commands.empty());
    EXPECT_EQ(meeting::ResultCode::SESSION_STATE_REJECTED, scenario.Sink().LastCommand().result_code);
}

// authenticated session（已认证、未加入任何 Meeting）断线。
TEST(SessionDisconnectBridgeFifo, AuthenticatedSessionDisconnectIsAppliedWithoutMeetings) {
    BridgeScenario scenario;
    ASSERT_TRUE(scenario.Prepare());
    scenario.CreateMeeting();
    ASSERT_TRUE(scenario.Done());

    ASSERT_TRUE(scenario.BindBridge(kBobSession));
    EXPECT_EQ(SessionDisconnectDispatch::POSTED, scenario.Bridge().NotifyDisconnected());
    ASSERT_TRUE(scenario.Done());

    // Bob 从未 Join 过：扇出为 0，但领域状态仍然完成 CLOSED 转换。
    ASSERT_EQ(1u, scenario.Sink().disconnects.size());
    EXPECT_EQ(0u, scenario.Sink().disconnects[0].affected_meeting_count);
    EXPECT_EQ(0u, scenario.Sink().disconnects[0].event_count);

    // 迟到的 Join 必须按领域 Session 状态被拒绝，而不是被误报为 AUTH_REQUIRED，
    // 也不得真的加入会议。
    ASSERT_TRUE(scenario.PostOneThenWait(MakeMeetingCommandWork(
        BobContext("m5-late-join"), ClientMeetingCommandKind::JOIN, kMeetingOne)));
    EXPECT_EQ(meeting::ResultCode::SESSION_STATE_REJECTED, scenario.Sink().LastCommand().result_code);
    EXPECT_EQ(0u, scenario.Sink().CountEventType(meeting::EventType::PARTICIPANT_JOINED));
}

// repeated disconnect（同一会话被多条终止路径重复触发）+ 真实扇出。
TEST(SessionDisconnectBridgeFifo, RepeatedBridgeDisconnectProducesExactlyOneFifoEntry) {
    BridgeScenario scenario;
    ASSERT_TRUE(scenario.Prepare());
    scenario.CreateMeeting();
    scenario.JoinBob(kMeetingOne);
    ASSERT_TRUE(scenario.Done());
    ASSERT_EQ(1u, scenario.Sink().CountEventType(meeting::EventType::PARTICIPANT_JOINED));

    ASSERT_TRUE(scenario.BindBridge(kBobSession));
    EXPECT_EQ(SessionDisconnectDispatch::POSTED, scenario.Bridge().NotifyDisconnected());
    EXPECT_EQ(SessionDisconnectDispatch::ALREADY_POSTED, scenario.Bridge().NotifyDisconnected());
    EXPECT_EQ(SessionDisconnectDispatch::ALREADY_POSTED, scenario.Bridge().NotifyDisconnected());
    ASSERT_TRUE(scenario.Done());

    // 重复断线在 FIFO 里只占**一个**位置，领域侧只有一次 fan-out。
    EXPECT_EQ(1u, scenario.Sink().CountDispatchedKind(LogicWorkKind::SESSION_DISCONNECTED));
    ASSERT_EQ(1u, scenario.Sink().disconnects.size());
    EXPECT_EQ(1u, scenario.Sink().disconnects[0].affected_meeting_count);
    EXPECT_EQ(1u, scenario.Sink().disconnects[0].event_count);
    EXPECT_EQ(1u, scenario.Sink().CountEventType(meeting::EventType::PARTICIPANT_LEFT));
}

// ==============================================================================
// 3. Ordering：message → disconnect → message
//
// 用 legacy 路由器上的闩锁把"消费者正在处理消息 A"这一瞬间变成可控状态，
// 于是 B 与断线事件的入队次序是确定的，FIFO 顺序因此可断言而非靠时序运气。
// ==============================================================================
TEST(SessionDisconnectBridgeOrdering, MessageDisconnectMessageKeepExactFifoOrder) {
    BridgeScenario scenario;
    ASSERT_TRUE(scenario.Prepare());
    scenario.CreateMeeting();
    scenario.JoinBob(kMeetingOne);
    ASSERT_TRUE(scenario.Done());

    scenario.Router().BlockNextRoute();
    const LogicWorkIdleProgress baseline = scenario.Executor().EnqueueBatchBaseline(
        std::vector<LogicWorkItem>(1, MakeExternalMessageWork(std::shared_ptr<LogicNode>())));
    ASSERT_TRUE(scenario.Router().WaitUntilRouteIsBlocked(5000));

    // 消费者此刻停在消息 A 的分发里；A 已被 Pop，队列为空。
    EXPECT_TRUE(scenario.Queue().IsEmpty());

    ASSERT_TRUE(scenario.BindBridge(kBobSession));
    EXPECT_EQ(SessionDisconnectDispatch::POSTED, scenario.Bridge().NotifyDisconnected());
    EXPECT_EQ(1u, scenario.Queue().Size());   // 队列里只有断线事件

    EXPECT_TRUE(scenario.Executor().Enqueue(MakeExternalMessageWork(std::shared_ptr<LogicNode>())));
    EXPECT_EQ(2u, scenario.Queue().Size());   // 断线事件确实排在消息 B 之前

    scenario.Router().ReleaseBlockedRoute();
    ASSERT_TRUE(scenario.Executor().WaitUntilIdle(5000, baseline));

    // 断线事件没有越过已排队的 A，也没有插到后来入队的 B 之前。
    const std::vector<std::string> expected = {
        "EXTERNAL_MESSAGE(session_id=s-bob msg_id=1)",
        "SESSION_DISCONNECTED(session_id=s-bob)",
        "EXTERNAL_MESSAGE(session_id=s-bob msg_id=2)",
    };
    EXPECT_EQ(expected, scenario.Sink().TailOfEvents(3));

    // 并且这条断线事件携带的是**真实的**领域工作（Bob 的最后一个 Session 断开）。
    ASSERT_EQ(1u, scenario.Sink().disconnects.size());
    EXPECT_EQ(1u, scenario.Sink().disconnects[0].affected_meeting_count);
    EXPECT_EQ(1u, scenario.Sink().CountEventType(meeting::EventType::PARTICIPANT_LEFT));
    EXPECT_EQ(2, scenario.Router().RoutedCount());
}

// ==============================================================================
// 4. Lifecycle
// ==============================================================================

// Leave 先完成：断线不得再产生第二个 ParticipantLeft。
TEST(SessionDisconnectBridgeLifecycle, LeaveBeforeBridgeDisconnectDoesNotDuplicateParticipantLeft) {
    BridgeScenario scenario;
    ASSERT_TRUE(scenario.Prepare());
    scenario.CreateMeeting();
    scenario.JoinBob(kMeetingOne);
    ASSERT_TRUE(scenario.Done());

    ASSERT_TRUE(scenario.PostOneThenWait(MakeMeetingCommandWork(
        BobContext("m5-leave"), ClientMeetingCommandKind::LEAVE, kMeetingOne)));
    ASSERT_EQ(1u, scenario.Sink().CountEventType(meeting::EventType::PARTICIPANT_LEFT));

    ASSERT_TRUE(scenario.BindBridge(kBobSession));
    EXPECT_EQ(SessionDisconnectDispatch::POSTED, scenario.Bridge().NotifyDisconnected());
    ASSERT_TRUE(scenario.Done());

    EXPECT_EQ(1u, scenario.Sink().CountEventType(meeting::EventType::PARTICIPANT_LEFT));
    ASSERT_EQ(1u, scenario.Sink().disconnects.size());
    EXPECT_EQ(0u, scenario.Sink().disconnects[0].affected_meeting_count);
    EXPECT_EQ(0u, scenario.Sink().disconnects[0].event_count);
}

// 断线先完成：其后的 stale Leave 必须按领域 Session 状态被拒绝。
TEST(SessionDisconnectBridgeLifecycle, BridgeDisconnectBeforeLeaveRejectsTheStaleLeave) {
    BridgeScenario scenario;
    ASSERT_TRUE(scenario.Prepare());
    scenario.CreateMeeting();
    scenario.JoinBob(kMeetingOne);
    ASSERT_TRUE(scenario.Done());

    ASSERT_TRUE(scenario.BindBridge(kBobSession));
    EXPECT_EQ(SessionDisconnectDispatch::POSTED, scenario.Bridge().NotifyDisconnected());
    ASSERT_TRUE(scenario.Done());
    ASSERT_EQ(1u, scenario.Sink().disconnects.size());
    EXPECT_EQ(1u, scenario.Sink().disconnects[0].affected_meeting_count);
    EXPECT_EQ(1u, scenario.Sink().CountEventType(meeting::EventType::PARTICIPANT_LEFT));

    ASSERT_TRUE(scenario.PostOneThenWait(MakeMeetingCommandWork(
        BobContext("m5-stale-leave"), ClientMeetingCommandKind::LEAVE, kMeetingOne)));
    EXPECT_EQ(meeting::ResultCode::SESSION_STATE_REJECTED, scenario.Sink().LastCommand().result_code);
    EXPECT_EQ(1u, scenario.Sink().CountEventType(meeting::EventType::PARTICIPANT_LEFT));
    EXPECT_EQ(0u, scenario.Sink().CountEventType(meeting::EventType::MEETING_CLOSED));
}

// Disconnect during ENDING（普通成员）：成员关系已冻结给 close flow，只做幂等 UNBOUND。
TEST(SessionDisconnectBridgeLifecycle, BridgeDisconnectDuringEndingUnbindsWithoutMembershipChange) {
    BridgeScenario scenario;
    ASSERT_TRUE(scenario.Prepare());
    scenario.CreateMeeting();
    scenario.JoinBob(kMeetingOne);
    ASSERT_TRUE(scenario.Done());

    // 确定性交错：[blocker, CLOSE] 一次入队 → 消费者停在 blocker 上
    //            → 断线事件追加到 CLOSE **之后** → 释放后 CLOSE 才被执行。
    scenario.Router().BlockNextRoute();
    const LogicWorkIdleProgress baseline = scenario.Executor().EnqueueBatchBaseline(
        {MakeExternalMessageWork(std::shared_ptr<LogicNode>()),
         MakeMeetingCommandWork(HostContext("m5-close"), ClientMeetingCommandKind::CLOSE, kMeetingOne)});
    ASSERT_TRUE(scenario.Router().WaitUntilRouteIsBlocked(5000));

    ASSERT_TRUE(scenario.BindBridge(kBobSession));
    EXPECT_EQ(SessionDisconnectDispatch::POSTED, scenario.Bridge().NotifyDisconnected());
    // 此刻队列里恰好是 [CLOSE, 断线]：断线事件确定地排在已入队的 CLOSE 之后。
    EXPECT_EQ(2u, scenario.Queue().Size());

    scenario.Router().ReleaseBlockedRoute();
    ASSERT_TRUE(scenario.Executor().WaitUntilIdle(5000, baseline));

    // dispatch 次序本身就是证据：断线发生在 CLOSE（→ ENDING）与 FINALIZE（→ CLOSED）之间。
    const std::vector<std::string> expected = {
        "EXTERNAL_MESSAGE(session_id=s-bob msg_id=1)",
        "MEETING_COMMAND(CLOSE meeting_id=m-1)",
        "SESSION_DISCONNECTED(session_id=s-bob)",
        "FINALIZE_CLOSE(meeting_id=m-1)",
    };
    EXPECT_EQ(expected, scenario.Sink().TailOfEvents(4));

    ASSERT_EQ(1u, scenario.Sink().disconnects.size());
    EXPECT_EQ(kBobSession, scenario.Sink().disconnects[0].session_id);
    EXPECT_EQ(1u, scenario.Sink().disconnects[0].affected_meeting_count);
    // ENDING 边界上的 disconnect 不改变 membership，也不产生事件。
    EXPECT_EQ(0u, scenario.Sink().disconnects[0].event_count);
    EXPECT_EQ(0u, scenario.Sink().CountEventType(meeting::EventType::PARTICIPANT_LEFT));

    // Close 的两个 turn 仍然完整收敛，Meeting 不会停在 ENDING。
    EXPECT_EQ(meeting::MeetingState::ENDING, scenario.Sink().LastCommand().meeting_state);
    EXPECT_TRUE(scenario.Sink().LastCommand().should_schedule_finalize);
    ASSERT_EQ(1u, scenario.Sink().finalizes.size());
    EXPECT_TRUE(scenario.Sink().finalizes[0].finalized);
    EXPECT_TRUE(scenario.Dispatcher().HasClosedSnapshot(kMeetingOne));
}

// Close before disconnect（Host 视角）：Host 在 ENDING 期间断线，
// 已冻结的 CloseContext 不受影响，快照与通知意图仍然成立。
TEST(SessionDisconnectBridgeLifecycle, HostBridgeDisconnectDuringEndingKeepsFrozenCloseContext) {
    BridgeScenario scenario;
    ASSERT_TRUE(scenario.Prepare());
    scenario.CreateMeeting();
    ASSERT_TRUE(scenario.Done());

    scenario.Router().BlockNextRoute();
    const LogicWorkIdleProgress baseline = scenario.Executor().EnqueueBatchBaseline(
        {MakeExternalMessageWork(std::shared_ptr<LogicNode>()),
         MakeMeetingCommandWork(HostContext("m5-close"), ClientMeetingCommandKind::CLOSE, kMeetingOne)});
    ASSERT_TRUE(scenario.Router().WaitUntilRouteIsBlocked(5000));

    ASSERT_TRUE(scenario.BindBridge(kHostSession));
    EXPECT_EQ(SessionDisconnectDispatch::POSTED, scenario.Bridge().NotifyDisconnected());
    scenario.Router().ReleaseBlockedRoute();
    ASSERT_TRUE(scenario.Executor().WaitUntilIdle(5000, baseline));

    ASSERT_EQ(1u, scenario.Sink().disconnects.size());
    EXPECT_EQ(kHostSession, scenario.Sink().disconnects[0].session_id);
    EXPECT_EQ(1u, scenario.Sink().disconnects[0].affected_meeting_count);
    // Host 断线语义（I4）：Host 允许 "ACTIVE + 0 个有效 Session"，既不产生事件，
    // 也不让 Meeting 关闭或提前结束。
    EXPECT_EQ(0u, scenario.Sink().disconnects[0].event_count);
    EXPECT_EQ(0u, scenario.Sink().CountEventType(meeting::EventType::PARTICIPANT_LEFT));

    // 通知意图来自 BeginClose 时冻结的 CloseContext，因此仍然包含断线前 BOUND 的 Host session。
    ASSERT_EQ(1u, scenario.Sink().finalizes.size());
    EXPECT_TRUE(scenario.Sink().finalizes[0].finalized);
    const std::vector<meeting::SessionId>& targets =
        scenario.Sink().finalizes[0].notification_session_ids;
    EXPECT_NE(targets.end(), std::find(targets.begin(), targets.end(), std::string(kHostSession)));
    EXPECT_TRUE(scenario.Dispatcher().HasClosedSnapshot(kMeetingOne));
}

// Disconnect after CLOSED：FIFO 照常受理并分发，但领域侧已经无事可做。
TEST(SessionDisconnectBridgeLifecycle, BridgeDisconnectAfterClosedTouchesNothing) {
    BridgeScenario scenario;
    ASSERT_TRUE(scenario.Prepare());
    scenario.CreateMeeting();
    scenario.JoinBob(kMeetingOne);
    ASSERT_TRUE(scenario.Done());

    ASSERT_TRUE(scenario.PostOneThenWait(MakeMeetingCommandWork(
        HostContext("m5-close"), ClientMeetingCommandKind::CLOSE, kMeetingOne)));
    ASSERT_TRUE(scenario.Dispatcher().HasClosedSnapshot(kMeetingOne));
    EXPECT_FALSE(scenario.Dispatcher().HasActiveMeeting(kMeetingOne));
    const std::size_t closed_events_before =
        scenario.Sink().CountEventType(meeting::EventType::MEETING_CLOSED);
    EXPECT_EQ(1u, closed_events_before);   // Finalize 恰好产生一次 MeetingClosed

    ASSERT_TRUE(scenario.BindBridge(kBobSession));
    // FIFO 仍然受理（队列并没有因为 Meeting 已关闭而拒绝外部 work）。
    EXPECT_EQ(SessionDisconnectDispatch::POSTED, scenario.Bridge().NotifyDisconnected());
    ASSERT_TRUE(scenario.Done());

    // FINALIZE 在断线之前：断线确实发生在 CLOSED 之后。
    const std::vector<std::string> expected = {
        "FINALIZE_CLOSE(meeting_id=m-1)",
        "SESSION_DISCONNECTED(session_id=s-bob)",
    };
    EXPECT_EQ(expected, scenario.Sink().TailOfEvents(2));

    // 该 Meeting 的 refs 已在 finalize 时清除，因此断线不再触碰它。
    ASSERT_EQ(1u, scenario.Sink().disconnects.size());
    EXPECT_EQ(0u, scenario.Sink().disconnects[0].affected_meeting_count);
    EXPECT_EQ(0u, scenario.Sink().disconnects[0].event_count);
    // 断线之后不得产生任何**新的**领域事件（尤其是第二个 MeetingClosed）。
    EXPECT_EQ(closed_events_before, scenario.Sink().CountEventType(meeting::EventType::MEETING_CLOSED));
    EXPECT_TRUE(scenario.Dispatcher().HasClosedSnapshot(kMeetingOne));
}

// ==============================================================================
// 5. Shutdown：已受理的断线 work 不丢、drain 生效、stop 之后如实拒绝
// ==============================================================================
TEST(SessionDisconnectBridgeShutdown, AcceptedBridgeDisconnectIsNeverLostAtShutdown) {
    BridgeScenario scenario;
    ASSERT_TRUE(scenario.Prepare());
    scenario.CreateMeeting();
    scenario.JoinBob(kMeetingOne);
    ASSERT_TRUE(scenario.Done());

    ASSERT_TRUE(scenario.BindBridge(kBobSession));
    ASSERT_EQ(SessionDisconnectDispatch::POSTED, scenario.Bridge().NotifyDisconnected());

    // 立刻请求停止：无论这条 work 落在正常轮次还是 drain 轮次，它都不得丢失。
    scenario.StopAndJoin();

    ASSERT_EQ(1u, scenario.Sink().disconnects.size());
    EXPECT_EQ(kBobSession, scenario.Sink().disconnects[0].session_id);
    EXPECT_EQ(1u, scenario.Sink().disconnects[0].affected_meeting_count);
    EXPECT_EQ(1u, scenario.Sink().CountEventType(meeting::EventType::PARTICIPANT_LEFT));
}

TEST(SessionDisconnectBridgeShutdown, QueuedDisconnectWorkIsDrainedBeforeExit) {
    BridgeScenario scenario;
    ASSERT_TRUE(scenario.Prepare());
    scenario.CreateMeeting();
    scenario.JoinBob(kMeetingOne);
    ASSERT_TRUE(scenario.Done());

    // 确定前提（不依赖时序运气）：消费者此刻已停放，测试线程可以持有队列锁，
    // 从而把"停止请求时队列里已经排着一条断线 work"变成可断言的事实。
    //
    // 注意：持锁期间只能调用 *Locked 版本。IsEmpty() / Size() / SnapshotForDiagnostics()
    // 会再次获取同一把非递归 mutex，直接自我死锁（M4 §6.3(1) 记录的坑）。
    {
        LogicWorkQueue::Lock lock(scenario.Queue());
        ASSERT_TRUE(scenario.Queue().PushLocked(MakeSessionDisconnectedWork(kBobSession)));
        EXPECT_FALSE(scenario.Queue().IsEmptyLocked());
    }

    scenario.StopAndJoin();

    // drain 语义：stop 时已受理的断线 work 一定被执行完，一个都不丢。
    ASSERT_EQ(1u, scenario.Sink().disconnects.size());
    EXPECT_EQ(kBobSession, scenario.Sink().disconnects[0].session_id);
    EXPECT_EQ(1u, scenario.Sink().disconnects[0].affected_meeting_count);
    EXPECT_EQ(1u, scenario.Sink().CountEventType(meeting::EventType::PARTICIPANT_LEFT));
}

TEST(SessionDisconnectBridgeShutdown, BridgeDisconnectAfterStopIsRejectedAndNeverReachesDomain) {
    BridgeScenario scenario;
    ASSERT_TRUE(scenario.Prepare());
    ASSERT_TRUE(scenario.BindBridge(kBobSession));

    scenario.StopAndJoin();

    // stop 之后到达的断线事件被队列拒绝：这是 M4 §11.2 的既定语义（外部 work 丢弃），
    // 不是业务错误，日志与桥接返回值都如实反映。
    EXPECT_EQ(SessionDisconnectDispatch::REJECTED, scenario.Bridge().NotifyDisconnected());
    EXPECT_TRUE(scenario.Bridge().AlreadyPosted());
    EXPECT_EQ(SessionDisconnectDispatch::ALREADY_POSTED, scenario.Bridge().NotifyDisconnected());

    // 直接证据：同一队列在 stop 之后也拒绝内部 kind 的显式入队。
    EXPECT_FALSE(scenario.Executor().Enqueue(MakeSessionDisconnectedWork(kBobSession)));
    EXPECT_TRUE(scenario.Sink().disconnects.empty());
    EXPECT_TRUE(scenario.Sink().events_in_order.empty());
}
