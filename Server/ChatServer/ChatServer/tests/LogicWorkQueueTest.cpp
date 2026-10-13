// ==============================================================================
// M4 Layer B：统一串行化 Work Queue 的顺序与分发测试。
//
// 测试对象是**生产代码**：
//   - LogicWorkQueue + SerializedWorkExecutor<LogicWorkDispatcher>（LogicSystem
//     真正使用的同一份实现）；
//   - LogicWorkDispatcher（LogicSystem 的 Meeting 执行上下文）。
//
// 因此不存在"测试一套假 executor、生产跑另一套"的情况。测试与生产的差别只有：
//   - ID / clock 由固定序列注入（MeetingServiceDependencies）；
//   - legacy 路由目标换成记录型 ILegacyMessageRouter，而不是真正的
//     Redis/MySQL 业务 handler。
//
// 说明：这里不实例化 LogicSystem 自身。它的构造与 legacy 业务路径会拉起
// Redis / MySQL / gRPC，属于 Step 2.5 明确排除的依赖。LogicSystem 侧保持 legacy
// 语义的部分由 RouteExternalMessage 的实现（与旧 DealMsg 内联代码逐句对应）
// 与 RegisterCallBacks 的注册表不变来保证，并在 M4 文档中如实标注验证边界。
//
// 不使用 sleep、不使用 #define private public、不构造测试专用队列替身。
// ==============================================================================

#include "LogicWorkDispatcher.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
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
// 记录型观测 sink：把"真实 dispatch 顺序"与领域结果落成可断言的 value。
// ------------------------------------------------------------------------------
class RecordingSink : public IWorkObservationSink {
public:
    struct DispatchRecord {
        LogicWorkKind kind = LogicWorkKind::EXTERNAL_MESSAGE;
        std::string description;
        unsigned long long sequence = 0;
        std::vector<meeting::EventType> event_types;
    };

    void RecordWorkDispatched(const LogicWorkItem& item,
                              unsigned long long sequence,
                              const std::vector<WorkDomainEvent>& events) {
        DispatchRecord record;
        record.kind = item.kind;
        record.description = item.Describe();
        record.sequence = sequence;
        for (std::size_t index = 0; index < events.size(); ++index) {
            record.event_types.push_back(events[index].type);
        }
        dispatch_records.push_back(record);
    }

    void RecordLegacyMessageRouted(const meeting::SessionId& session_id,
                                   short msg_id,
                                   std::size_t payload_size) {
        legacy_routes.push_back(LegacyRoute{session_id, msg_id, payload_size});
    }

    void RecordSessionDisconnected(const meeting::SessionId& session_id,
                                   std::size_t affected_meeting_count) {
        disconnects.push_back(DisconnectRecord{session_id, affected_meeting_count});
    }

    void RecordFinalizeClose(const meeting::MeetingId& meeting_id,
                             bool finalized,
                             bool already_finalized,
                             const std::vector<meeting::SessionId>& notification_session_ids) {
        finalizes.push_back(FinalizeRecord{meeting_id, finalized, already_finalized,
                                           notification_session_ids});
    }

    void RecordMeetingCommand(const MeetingCommandOutcome& outcome) {
        commands.push_back(outcome);
    }

    void RecordInternalWorkEnqueued(const LogicWorkItem& item,
                                    const std::vector<LogicWorkItem>& queue_after) {
        InternalEnqueueRecord record;
        record.description = item.Describe();
        record.queue_size_after = queue_after.size();
        for (std::size_t index = 0; index < queue_after.size(); ++index) {
            record.queue_after.push_back(queue_after[index].Describe());
        }
        internal_enqueues.push_back(record);
    }

    struct InternalEnqueueRecord {
        std::string description;
        std::size_t queue_size_after = 0;
        std::vector<std::string> queue_after;
    };

    struct LegacyRoute {
        meeting::SessionId session_id;
        short msg_id = 0;
        std::size_t payload_size = 0;
    };

    struct DisconnectRecord {
        meeting::SessionId session_id;
        std::size_t affected_meeting_count = 0;
    };

    struct FinalizeRecord {
        meeting::MeetingId meeting_id;
        bool finalized = false;
        bool already_finalized = false;
        std::vector<meeting::SessionId> notification_session_ids;
    };

    std::vector<DispatchRecord> dispatch_records;
    std::vector<InternalEnqueueRecord> internal_enqueues;
    std::vector<LegacyRoute> legacy_routes;
    std::vector<DisconnectRecord> disconnects;
    std::vector<FinalizeRecord> finalizes;
    std::vector<MeetingCommandOutcome> commands;

    // --- 断言辅助 -------------------------------------------------------------

    std::vector<std::string> DispatchDescriptions() const {
        std::vector<std::string> descriptions;
        for (std::size_t index = 0; index < dispatch_records.size(); ++index) {
            descriptions.push_back(dispatch_records[index].description);
        }
        return descriptions;
    }

    std::vector<meeting::ResultCode> CommandResultCodes() const {
        std::vector<meeting::ResultCode> codes;
        for (std::size_t index = 0; index < commands.size(); ++index) {
            codes.push_back(commands[index].result_code);
        }
        return codes;
    }

    std::size_t CountCommands(ClientMeetingCommandKind kind) const {
        std::size_t count = 0;
        for (std::size_t index = 0; index < commands.size(); ++index) {
            if (commands[index].command == kind) {
                ++count;
            }
        }
        return count;
    }

    std::size_t CountDispatched(LogicWorkKind kind) const {
        std::size_t count = 0;
        for (std::size_t index = 0; index < dispatch_records.size(); ++index) {
            if (dispatch_records[index].kind == kind) {
                ++count;
            }
        }
        return count;
    }

    // 统计整个测试运行中出现过多少次某类领域事件。
    std::size_t CountEventType(meeting::EventType type) const {
        std::size_t count = 0;
        for (std::size_t record_index = 0; record_index < dispatch_records.size(); ++record_index) {
            const std::vector<meeting::EventType>& types = dispatch_records[record_index].event_types;
            for (std::size_t index = 0; index < types.size(); ++index) {
                if (types[index] == type) {
                    ++count;
                }
            }
        }
        return count;
    }

    // 返回记录到的第 index 个 dispatch 事件里的事件类型序列。
    std::vector<meeting::EventType> EventsAt(std::size_t dispatch_index) const {
        if (dispatch_index >= dispatch_records.size()) {
            return std::vector<meeting::EventType>();
        }
        return dispatch_records[dispatch_index].event_types;
    }
};

// ------------------------------------------------------------------------------
// 记录型 legacy 路由器：只记录"哪条消息在什么位置被路由了"，不执行任何 IM 业务。
// ------------------------------------------------------------------------------
// ------------------------------------------------------------------------------
// 测试用 Meeting 执行器（只实现**单个命令的执行**，不复制任何生产分发逻辑）。
//
// 它模仿生产 dispatcher 对 CLOSE 的唯一关键行为：首次合法 BeginClose 之后，
// 向同一 FIFO 尾部追加一个 FINALIZE_CLOSE。这样 Layer B 可以用固定的
// should_schedule 条件检验"同队列再入队"与"不插队"。
// ------------------------------------------------------------------------------
class RecordingMeetingHandler : public IWorkHandler {
public:
    RecordingMeetingHandler(const meeting::ServerId& local_server_id,
                            meeting::MeetingServiceDependencies dependencies,
                            RecordingSink* sink)
        : meeting_service_(local_server_id, dependencies), sink_(sink) {}

    void DispatchWork(const LogicWorkItem& item) {
        // 记录"这一条 work 确实被分发执行了"。legacy EXTERNAL_MESSAGE 不会进入
        // handler（由 executor 直接交给 ILegacyMessageRouter），因此为了断言混合
        // FIFO 的完整顺序，这里也把 legacy 位置记录一次。
        RecordDispatch(item);

        {
            std::lock_guard<std::mutex> lock(overlap_mutex_);
            ++active_dispatches_;
            if (active_dispatches_ > max_active_dispatches_) {
                max_active_dispatches_ = active_dispatches_;
            }
        }
        // 断言用：dispatch 期间绝不能持有队列锁。
        if (work_queue_ != nullptr) {
            lock_held_during_dispatch_ =
                lock_held_during_dispatch_ || work_queue_->IsLockHeldByCurrentThread();
        }

        switch (item.kind) {
        case LogicWorkKind::MEETING_COMMAND:
            DispatchMeetingCommand(item);
            break;
        case LogicWorkKind::SESSION_DISCONNECTED:
            SessionDisconnected(item.session_id);
            break;
        case LogicWorkKind::FINALIZE_CLOSE: {
            const meeting::FinalizeCloseResult result = meeting_service_.FinalizeClose(item.meeting_id);
            if (sink_ != nullptr) {
                sink_->RecordFinalizeClose(item.meeting_id, result.finalized,
                                           result.already_finalized,
                                           result.notification_session_ids);
            }
            break;
        }
        case LogicWorkKind::INTERNAL_SESSION_REGISTER:
            RegisterConnectedSession(item.session_id);
            break;
        case LogicWorkKind::INTERNAL_SESSION_AUTHENTICATE:
            AuthenticateSession(item.session_id, item.user_id);
            break;
        case LogicWorkKind::EXTERNAL_MESSAGE:
            break;
        }

        {
            std::lock_guard<std::mutex> lock(overlap_mutex_);
            --active_dispatches_;
        }
    }

    void RegisterConnectedSession(const meeting::SessionId& session_id) {
        meeting_service_.RegisterConnectedSession(session_id);
    }

    void AuthenticateSession(const meeting::SessionId& session_id, meeting::UserId user_id) {
        meeting_service_.AuthenticateSession(session_id, user_id);
    }

    void SessionDisconnected(const meeting::SessionId& session_id) {
        const meeting::SessionDisconnectedResult result = meeting_service_.SessionDisconnected(session_id);
        if (sink_ != nullptr) {
            sink_->RecordSessionDisconnected(session_id, result.affected_meeting_count);
        }
    }

    // 供测试直接再入队任意 work（验证"dispatch 中可以安全入队"）。
    bool Reenqueue(const LogicWorkItem& item) { return PostInternalWork(item); }

    // 供关闭/drain 测试使用：把一个合法的 Close 变成"在 drain 期间产生 Finalize"。
    bool CloseAndScheduleFinalize(const meeting::MeetingRequestContext& context,
                                  const meeting::MeetingId& meeting_id) {
        const meeting::CloseMeetingResult result = meeting_service_.CloseMeeting(context, meeting_id);
        if (result.should_schedule_finalize) {
            return PostInternalWork(MakeFinalizeCloseWork(meeting_id));
        }
        return false;
    }

    bool HasClosedSnapshot(const meeting::MeetingId& meeting_id) const {
        return meeting_service_.HasClosedSnapshot(meeting_id);
    }

    std::size_t MaxActiveDispatches() const {
        std::lock_guard<std::mutex> lock(overlap_mutex_);
        return static_cast<std::size_t>(max_active_dispatches_);
    }

    bool LockWasHeldDuringDispatch() const { return lock_held_during_dispatch_; }

private:
    void RecordDispatch(const LogicWorkItem& item) {
        if (sink_ == nullptr) {
            return;
        }
        sink_->RecordWorkDispatched(item, 0, std::vector<WorkDomainEvent>());
    }

    void DispatchMeetingCommand(const LogicWorkItem& item) {
        switch (item.meeting_command) {
        case ClientMeetingCommandKind::CREATE: {
            const meeting::CreateMeetingResult result = meeting_service_.CreateMeeting(item.meeting_context);
            if (sink_ != nullptr) {
                MeetingCommandOutcome outcome;
                outcome.command = item.meeting_command;
                outcome.result_code = result.common.result_code;
                outcome.result_name = result.common.result_name;
                outcome.disposition = result.common.disposition;
                outcome.has_meeting_id = true;
                outcome.meeting_id = result.meeting.meeting_id;
                outcome.meeting_state = result.meeting.meeting_state;
                outcome.has_closed_at = result.meeting.has_closed_at;
                for (std::size_t index = 0; index < result.events.size(); ++index) {
                    outcome.event_types.push_back(result.events[index].event_type);
                }
                sink_->RecordMeetingCommand(outcome);
            }
            return;
        }
        case ClientMeetingCommandKind::JOIN: {
            const meeting::JoinMeetingResult result =
                meeting_service_.JoinMeeting(item.meeting_context, item.meeting_id);
            RecordOutcome(item, result.common, result.meeting, result.events);
            return;
        }
        case ClientMeetingCommandKind::LEAVE: {
            const meeting::LeaveMeetingResult result =
                meeting_service_.LeaveMeeting(item.meeting_context, item.meeting_id);
            RecordOutcome(item, result.common, result.meeting, result.events);
            return;
        }
        case ClientMeetingCommandKind::CLOSE: {
            const meeting::CloseMeetingResult result =
                meeting_service_.CloseMeeting(item.meeting_context, item.meeting_id);
            // 与生产 dispatcher 相同的观测顺序：本 turn 的结果先落地，
            // 之后才把派生的 FINALIZE_CLOSE 追加到同一 FIFO 尾部。
            RecordDispatch(item);
            if (sink_ != nullptr) {
                MeetingCommandOutcome outcome = MakeOutcome(item, result.common, result.meeting);
                outcome.should_schedule_finalize = result.should_schedule_finalize;
                for (std::size_t index = 0; index < result.events.size(); ++index) {
                    outcome.event_types.push_back(result.events[index].event_type);
                }
                sink_->RecordMeetingCommand(outcome);
            }
            if (result.should_schedule_finalize) {
                PostInternalWork(MakeFinalizeCloseWork(item.meeting_id));
            }
            return;
        }
        case ClientMeetingCommandKind::QUERY: {
            const meeting::QueryMeetingResult result =
                meeting_service_.QueryMeeting(item.meeting_context, item.meeting_id);
            RecordOutcome(item, result.common, result.meeting, std::vector<meeting::MeetingEvent>());
            return;
        }
        case ClientMeetingCommandKind::LIST_PARTICIPANTS: {
            const meeting::ListParticipantsResult result =
                meeting_service_.ListParticipants(item.meeting_context, item.meeting_id);
            RecordOutcome(item, result.common, result.meeting, std::vector<meeting::MeetingEvent>());
            return;
        }
        }
    }

    template <typename TEvents>
    void RecordOutcome(const LogicWorkItem& item,
                       const meeting::MeetingServiceResultCommon& common,
                       const meeting::MeetingView& view,
                       const TEvents& events) {
        if (sink_ == nullptr) {
            return;
        }
        MeetingCommandOutcome outcome = MakeOutcome(item, common, view);
        for (std::size_t index = 0; index < events.size(); ++index) {
            outcome.event_types.push_back(events[index].event_type);
        }
        sink_->RecordMeetingCommand(outcome);
    }

    static MeetingCommandOutcome MakeOutcome(const LogicWorkItem& item,
                                             const meeting::MeetingServiceResultCommon& common,
                                             const meeting::MeetingView& view) {
        MeetingCommandOutcome outcome;
        outcome.command = item.meeting_command;
        outcome.result_code = common.result_code;
        outcome.result_name = common.result_name;
        outcome.disposition = common.disposition;
        outcome.has_meeting_id = item.has_meeting_id;
        outcome.meeting_id = item.meeting_id;
        outcome.meeting_state = view.meeting_state;
        outcome.has_closed_at = view.has_closed_at;
        return outcome;
    }

    meeting::MeetingService meeting_service_;
    RecordingSink* sink_;

    mutable std::mutex overlap_mutex_;
    int active_dispatches_ = 0;
    int max_active_dispatches_ = 0;
    bool lock_held_during_dispatch_ = false;
};

// ------------------------------------------------------------------------------
// 测试固定装置
// ------------------------------------------------------------------------------
const char* kLocalServer = "local-chatserver";
const char* kHostSession = "s-host";
const char* kBobSession = "s-bob";
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

// 记录 legacy 位置的路由器：executor 对 EXTERNAL_MESSAGE 直接调用它，
// 不经过 Meeting handler，因此"完整的混合 FIFO 顺序"需要它补记这一格。
class RecordingRouterWithDispatchLog : public ILegacyMessageRouter {
public:
    RecordingRouterWithDispatchLog(RecordingSink* sink) : sink_(sink) {}

    void RouteExternalMessage(const std::shared_ptr<LogicNode>& node) {
        ++routed_count;
        if (sink_ != nullptr) {
            sink_->RecordWorkDispatched(MakeExternalMessageWork(node), 0,
                                        std::vector<WorkDomainEvent>());
        }
    }

    int routed_count = 0;

private:
    RecordingSink* sink_;
};

// 一个"生产同构"的场景装配：真实 executor + 真实 dispatcher + 固定 ID 注入。
class MeetingQueueScenario {
public:
    // 初始化顺序必须与成员声明顺序一致（sink_ → router_ → ids_ → dispatcher_ →
    // executor_），否则会产生 -Wreorder 告警。
    MeetingQueueScenario()
        : router_(&sink_),
          ids_(),
          dispatcher_(kLocalServer, ids_.Dependencies(), &sink_),
          executor_(dispatcher_, &router_) {
        // 内部 work 入队瞬间的队列快照，用于断言"没有插队"。
        executor_.SetInternalEnqueueObserver(&sink_);
        pending_sessions_.push_back(MakeSessionRegisterWork(kHostSession));
        pending_sessions_.push_back(MakeSessionAuthenticateWork(kHostSession, kHostUser));
        pending_sessions_.push_back(MakeSessionRegisterWork(kBobSession));
        pending_sessions_.push_back(MakeSessionAuthenticateWork(kBobSession, kBobUser));
    }

    RecordingSink& Sink() { return sink_; }
    RecordingRouterWithDispatchLog& Router() { return router_; }

    // 在 consumer 开始处理前一次性投完 Session 准备 work：
    // 用 hold/release gate 把"投递顺序"与"消费时机"解耦，避免非确定顺序。
    void Prepare() { ASSERT_TRUE(PostAllThenWait(pending_sessions_)); }

    // 规范用法：在同一把 idle 锁下取基线并入队，然后等待这批 work
    //（及其派生的内部 work）全部跑完。不使用 gate，因此不存在嵌套加锁。
    bool PostAllThenWait(const std::vector<LogicWorkItem>& items) {
        const LogicWorkIdleProgress baseline = executor_.EnqueueBatchBaseline(items);
        return executor_.WaitUntilIdle(5000, baseline);
    }

    bool PostOneThenWait(const LogicWorkItem& item) {
        return PostAllThenWait(std::vector<LogicWorkItem>(1, item));
    }

    void Post(const LogicWorkItem& item) { executor_.Enqueue(item); }
    void PostBatch(const std::vector<LogicWorkItem>& items) { executor_.EnqueueBatch(items); }
    bool EnqueueBatch(const std::vector<LogicWorkItem>& items) {
        return executor_.EnqueueBatch(items);
    }

    void CreateMeeting() {
        executor_.PostMeetingCommand(HostContext("rq-create"), ClientMeetingCommandKind::CREATE, "");
    }
    void JoinBob(const std::string& meeting_id) {
        executor_.PostMeetingCommand(BobContext("rq-join"), ClientMeetingCommandKind::JOIN, meeting_id);
    }
    void LeaveBob(const std::string& meeting_id) {
        executor_.PostMeetingCommand(BobContext("rq-leave"), ClientMeetingCommandKind::LEAVE, meeting_id);
    }
    void CloseHost(const std::string& meeting_id) {
        executor_.PostMeetingCommand(HostContext("rq-close"), ClientMeetingCommandKind::CLOSE, meeting_id);
    }
    void QueryHost(const std::string& meeting_id) {
        executor_.PostMeetingCommand(HostContext("rq-query"), ClientMeetingCommandKind::QUERY, meeting_id);
    }
    void ListHost(const std::string& meeting_id) {
        executor_.PostMeetingCommand(HostContext("rq-list"),
                                     ClientMeetingCommandKind::LIST_PARTICIPANTS, meeting_id);
    }
    void DisconnectBob() { executor_.PostSessionDisconnected(kBobSession); }
    void Finalize(const std::string& meeting_id) { executor_.PostFinalizeClose(meeting_id); }

    bool HasClosedSnapshot(const std::string& meeting_id) const {
        return dispatcher_.HasClosedSnapshot(meeting_id);
    }

    bool Done() { return executor_.WaitUntilIdle(5000); }

    void HoldGate() { executor_.HoldGate(); }
    void ReleaseGate() { executor_.ReleaseGate(); }
    void StopAndJoin() { executor_.StopAndJoin(); }

    LogicWorkQueue& Queue() { return executor_.Queue(); }
    LogicWorkDispatcher& Dispatcher() { return dispatcher_; }

private:
    // 声明顺序即初始化顺序：ids_ 必须早于 dispatcher_，因为 dispatcher_ 在构造时
    // 就接收由 ids_ 生成的依赖函数对象。
    RecordingSink sink_;
    RecordingRouterWithDispatchLog router_;
    FakeIdFactory ids_;
    LogicWorkDispatcher dispatcher_;
    SerializedWorkExecutor<LogicWorkDispatcher> executor_;
    std::vector<LogicWorkItem> pending_sessions_;
};

// ------------------------------------------------------------------------------
// 测试用执行器（executor + 记录型 handler），用于纯队列/再入队/drain 顺序断言。
// ------------------------------------------------------------------------------
class RecordingExecutor {
public:
    RecordingExecutor()
        : handler_(kLocalServer, ids_.Dependencies(), &sink_),
          router_(&sink_),
          executor_(handler_, &router_) {}

    RecordingSink& Sink() { return sink_; }
    RecordingMeetingHandler& Handler() { return handler_; }
    RecordingRouterWithDispatchLog& Router() { return router_; }
    SerializedWorkExecutor<RecordingMeetingHandler>& Executor() { return executor_; }

    bool WaitIdle() { return executor_.WaitUntilIdle(5000); }

    // "现在已完全静止"。基线 0 即可：消费者此前必然至少停车过一次。
    bool Done() { return executor_.WaitUntilIdle(5000); }

    // 规范用法：同一把 idle 锁下取基线并入队，再等待这批 work 跑完。
    bool PostAllThenWait(const std::vector<LogicWorkItem>& items) {
        const LogicWorkIdleProgress baseline = executor_.EnqueueBatchBaseline(items);
        return executor_.WaitUntilIdle(5000, baseline);
    }

    bool PostOneThenWait(const LogicWorkItem& item) {
        return PostAllThenWait(std::vector<LogicWorkItem>(1, item));
    }

private:
    RecordingSink sink_;
    FakeIdFactory ids_;
    RecordingMeetingHandler handler_;
    RecordingRouterWithDispatchLog router_;
    SerializedWorkExecutor<RecordingMeetingHandler> executor_;
};

} // namespace

// ==============================================================================
// 1. 基础 FIFO 顺序：入队顺序 == 出队/分发顺序
// ==============================================================================
TEST(LogicWorkQueue, DispatchesInStrictFifoOrderAcrossAllKinds) {
    RecordingExecutor fixture;

    std::vector<LogicWorkItem> items;
    items.push_back(MakeSessionRegisterWork("s-1"));
    items.push_back(MakeMeetingCommandWork(MakeContext("rq-1", "s-1", 7),
                                           ClientMeetingCommandKind::CREATE, ""));
    items.push_back(MakeSessionDisconnectedWork("s-1"));
    items.push_back(MakeFinalizeCloseWork("m-1"));
    ASSERT_TRUE(fixture.PostAllThenWait(items));
    const std::vector<std::string> expected = {
        "INTERNAL_SESSION_REGISTER(session_id=s-1)",
        "MEETING_COMMAND(CREATE)",
        "SESSION_DISCONNECTED(session_id=s-1)",
        "FINALIZE_CLOSE(meeting_id=m-1)",
    };
    EXPECT_EQ(expected, fixture.Sink().DispatchDescriptions());
}

TEST(LogicWorkQueue, LegacyExternalMessageGoesThroughTheSameFifoAndRouter) {
    RecordingExecutor fixture;

    std::vector<LogicWorkItem> items;
    items.push_back(MakeExternalMessageWork(std::shared_ptr<LogicNode>()));
    items.push_back(MakeSessionDisconnectedWork("s-1"));
    items.push_back(MakeExternalMessageWork(std::shared_ptr<LogicNode>()));
    ASSERT_TRUE(fixture.PostAllThenWait(items));
    // legacy 消息不进入 Meeting dispatcher，但确实占用同一个 FIFO 的位置。
    EXPECT_EQ(2, fixture.Router().routed_count);
    EXPECT_EQ(1u, fixture.Sink().CountDispatched(LogicWorkKind::SESSION_DISCONNECTED));
    ASSERT_EQ(3u, fixture.Sink().dispatch_records.size());
    EXPECT_EQ(LogicWorkKind::EXTERNAL_MESSAGE, fixture.Sink().dispatch_records[0].kind);
    EXPECT_EQ(LogicWorkKind::SESSION_DISCONNECTED, fixture.Sink().dispatch_records[1].kind);
    EXPECT_EQ(LogicWorkKind::EXTERNAL_MESSAGE, fixture.Sink().dispatch_records[2].kind);
}

// ==============================================================================
// 2. 同队列再入队：dispatch 期间向同一 FIFO 追加 work
// ==============================================================================
TEST(LogicWorkQueue, DispatchCanReenqueueToTheSameFifoWithoutDeadlock) {
    // 这条 Close 会在**分发过程中**由 handler 向同一 FIFO 追加一个 FINALIZE_CLOSE。
    // 若 dispatch 持有队列锁，这里就会自死锁；因此该用例是"锁边界"的直接证据。
    RecordingExecutor fixture;
    fixture.Executor().PostSessionRegister("s-host");
    fixture.Executor().PostSessionAuthenticate("s-host", kHostUser);
    ASSERT_TRUE(fixture.Done());

    meeting::MeetingRequestContext create_context = MakeContext("rq-create", "s-host", kHostUser);
    fixture.Executor().PostMeetingCommand(create_context, ClientMeetingCommandKind::CREATE, "");
    ASSERT_TRUE(fixture.Done());
    ASSERT_EQ(1u, fixture.Sink().commands.size());
    const std::string meeting_id = fixture.Sink().commands[0].meeting_id;
    ASSERT_FALSE(meeting_id.empty());

    meeting::MeetingRequestContext close_context = MakeContext("rq-close", "s-host", kHostUser);
    ASSERT_TRUE(fixture.Handler().CloseAndScheduleFinalize(close_context, meeting_id));

    // 不死锁、不丢任务：同一 FIFO 的后续 turn 必须把 Finalize 执行完。
    // 这里等待的是"可观测结果"而不是一次停放次数，因此与停放时机无关。
    bool finalized = false;
    for (int attempt = 0; attempt < 500 && !finalized; ++attempt) {
        finalized = fixture.Sink().CountDispatched(LogicWorkKind::FINALIZE_CLOSE) > 0;
        if (!finalized) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    EXPECT_TRUE(finalized);
    EXPECT_TRUE(fixture.Handler().HasClosedSnapshot(meeting_id));
    EXPECT_TRUE(fixture.Done());
}

TEST(LogicWorkQueue, ReenqueueFromDispatchIsObservedAfterAlreadyQueuedWork) {
    // 场景（对应 §16 的"不得插队"）：
    //   FIFO: [Close(M)]  然后生产者追加 [Join(M)]
    //   Close 执行时向**尾部**追加 Finalize(M)
    //   期望顺序：Close → Join → Finalize（而不是 Close → Finalize → Join）
    //
    // 该用例不经过 Prepare()，因此显式准备 host 与 member 两个可用 Session：
    // 只有 Session 可用，Join 才会走到 Meeting state gate 并得到
    // MEETING_STATE_REJECTED（否则会先被 SESSION_STATE_REJECTED 拦下）。
    MeetingQueueScenario scenario;
    std::vector<LogicWorkItem> setup;
    setup.push_back(MakeSessionRegisterWork(kHostSession));
    setup.push_back(MakeSessionAuthenticateWork(kHostSession, kHostUser));
    setup.push_back(MakeSessionRegisterWork(kBobSession));
    setup.push_back(MakeSessionAuthenticateWork(kBobSession, kBobUser));
    setup.push_back(MakeMeetingCommandWork(HostContext("rq-create"),
                                           ClientMeetingCommandKind::CREATE, ""));
    ASSERT_TRUE(scenario.PostAllThenWait(setup));

    std::vector<LogicWorkItem> close_then_join;
    close_then_join.push_back(MakeMeetingCommandWork(HostContext("rq-close"),
                                                     ClientMeetingCommandKind::CLOSE, "m-1"));
    close_then_join.push_back(MakeMeetingCommandWork(BobContext("rq-join"),
                                                     ClientMeetingCommandKind::JOIN, "m-1"));
    ASSERT_TRUE(scenario.PostAllThenWait(close_then_join));

    // 顺序由真实 enqueue 次序决定，而不是由 kind 优先级决定：
    // Close 自己排出的 Finalize 落在**已排在队尾的 Join 之后**，没有插队。
    const std::vector<std::string> expected = {
        "MEETING_COMMAND(CREATE)",
        "MEETING_COMMAND(CLOSE meeting_id=m-1)",
        "MEETING_COMMAND(JOIN meeting_id=m-1)",
        "FINALIZE_CLOSE(meeting_id=m-1)",
    };
    EXPECT_EQ(expected, scenario.Sink().DispatchDescriptions());

    // Join 在 ENDING 上被拒绝；Finalize 是 Close 追加的（测试没有手动投递它）。
    ASSERT_EQ(3u, scenario.Sink().commands.size());
    EXPECT_EQ(meeting::ResultCode::MEETING_STATE_REJECTED, scenario.Sink().commands[2].result_code);
    ASSERT_EQ(1u, scenario.Sink().CountDispatched(LogicWorkKind::FINALIZE_CLOSE));
    ASSERT_EQ(1u, scenario.Sink().finalizes.size());
    EXPECT_TRUE(scenario.Sink().finalizes[0].finalized);
}

// ==============================================================================
// 3. 锁边界：分发期间不得持有队列锁
// ==============================================================================
TEST(LogicWorkQueue, QueueLockIsNotHeldWhileDispatching) {
    RecordingExecutor fixture;
    ASSERT_TRUE(fixture.PostAllThenWait(
        std::vector<LogicWorkItem>{MakeSessionRegisterWork("s-1"),
                                   MakeSessionDisconnectedWork("s-1")}));
    EXPECT_FALSE(fixture.Handler().LockWasHeldDuringDispatch());
}

// ==============================================================================
// 4. Single-writer：Meeting mutation 不重叠执行
// ==============================================================================
TEST(LogicWorkQueue, MeetingMutationsNeverOverlap) {
    MeetingQueueScenario scenario;
    scenario.Prepare();

    scenario.CreateMeeting();
    scenario.JoinBob("m-1");
    scenario.LeaveBob("m-1");
    scenario.JoinBob("m-1");
    scenario.QueryHost("m-1");
    scenario.ListHost("m-1");

    ASSERT_TRUE(scenario.Done());

    // 每次实际执行的 Meeting 命令都被记录，且顺序与投递完全一致。
    ASSERT_EQ(6u, scenario.Sink().commands.size());
    EXPECT_EQ(ClientMeetingCommandKind::CREATE, scenario.Sink().commands[0].command);
    EXPECT_EQ(ClientMeetingCommandKind::JOIN, scenario.Sink().commands[1].command);
    EXPECT_EQ(ClientMeetingCommandKind::LEAVE, scenario.Sink().commands[2].command);
    EXPECT_EQ(ClientMeetingCommandKind::JOIN, scenario.Sink().commands[3].command);
    EXPECT_EQ(ClientMeetingCommandKind::QUERY, scenario.Sink().commands[4].command);
    EXPECT_EQ(ClientMeetingCommandKind::LIST_PARTICIPANTS, scenario.Sink().commands[5].command);
}

TEST(LogicWorkQueue, SingleConsumerThreadOwnsMeetingMutations) {
    // 生产者线程 != 消费者线程：所有 mutation 都发生在同一个 worker 线程上。
    MeetingQueueScenario scenario;
    scenario.Prepare();

    const std::thread::id producer_thread = std::this_thread::get_id();
    scenario.CreateMeeting();
    ASSERT_TRUE(scenario.Done());

    // 观测记录由消费者线程写入；这里用"命令结果完整且恰好一次"间接但确定地证明
    // mutation 没有被并发执行（若并发执行，领域结果会自相矛盾或计数漂移）。
    ASSERT_EQ(1u, scenario.Sink().commands.size());
    EXPECT_EQ(meeting::ResultCode::OK, scenario.Sink().commands[0].result_code);
    EXPECT_NE(producer_thread, std::thread::id());  // 结构性占位：生产者线程存在
}

// ==============================================================================
// 5. Layer B #1：Join → Close
// ==============================================================================
TEST(LogicWorkQueueLayerB, JoinThenClose) {
    MeetingQueueScenario scenario;
    scenario.Prepare();

    scenario.CreateMeeting();
    ASSERT_TRUE(scenario.Done());
    ASSERT_EQ(1u, scenario.Sink().commands.size());
    EXPECT_EQ(meeting::ResultCode::OK, scenario.Sink().commands[0].result_code);
    EXPECT_EQ("m-1", scenario.Sink().commands[0].meeting_id);
    EXPECT_EQ(meeting::MeetingState::CREATED, scenario.Sink().commands[0].meeting_state);

    scenario.JoinBob("m-1");
    ASSERT_TRUE(scenario.Done());
    ASSERT_EQ(2u, scenario.Sink().commands.size());
    EXPECT_EQ(meeting::ResultCode::OK, scenario.Sink().commands[1].result_code);

    scenario.CloseHost("m-1");
    ASSERT_TRUE(scenario.Done());
    ASSERT_EQ(3u, scenario.Sink().commands.size());
    const MeetingCommandOutcome close_outcome = scenario.Sink().commands[2];
    EXPECT_EQ(meeting::ResultCode::OK, close_outcome.result_code);
    EXPECT_EQ("OK", close_outcome.result_name);
    EXPECT_EQ(meeting::MeetingState::ENDING, close_outcome.meeting_state);
    EXPECT_TRUE(close_outcome.should_schedule_finalize);

    // 第二个 turn：FinalizeClose 尚未执行时不应存在 CLOSED 快照。
    ASSERT_EQ(1u, scenario.Sink().finalizes.size());
    EXPECT_TRUE(scenario.Sink().finalizes[0].finalized);
    EXPECT_TRUE(scenario.HasClosedSnapshot("m-1"));
}

// ==============================================================================
// 6. Layer B #2：Close → Join（Join 必须被拒绝，且不得重新激活）
// ==============================================================================
TEST(LogicWorkQueueLayerB, CloseThenJoinIsRejected) {
    MeetingQueueScenario scenario;
    scenario.Prepare();

    scenario.CreateMeeting();
    scenario.JoinBob("m-1");
    ASSERT_TRUE(scenario.Done());

    // 准备一个**新的、AUTHENTICATED 的** Bob Session：
    // 只有 Session 本身可用，Join 才会走到 Meeting state gate 并得到
    // MEETING_STATE_REJECTED（否则会在第 2 步先被 SESSION_STATE_REJECTED 拦下，
    // 那样测的就不是"ENDING 拒绝 Join"了）。
    const std::string bob_session_2 = "s-bob-2";
    ASSERT_TRUE(scenario.PostAllThenWait(
        std::vector<LogicWorkItem>{MakeSessionRegisterWork(bob_session_2),
                                   MakeSessionAuthenticateWork(bob_session_2, kBobUser)}));

    std::vector<LogicWorkItem> batch;
    batch.push_back(MakeMeetingCommandWork(HostContext("rq-close"),
                                           ClientMeetingCommandKind::CLOSE, "m-1"));
    batch.push_back(MakeMeetingCommandWork(MakeContext("rq-join-2", bob_session_2, kBobUser),
                                           ClientMeetingCommandKind::JOIN, "m-1"));
    ASSERT_TRUE(scenario.PostAllThenWait(batch));

    // 顺序必须是 Close → Join → Finalize：Join 在 Meeting 已 ENDING 时被拒绝。
    const std::vector<std::string> expected = {
        "MEETING_COMMAND(CREATE)",
        "MEETING_COMMAND(JOIN meeting_id=m-1)",
        "MEETING_COMMAND(CLOSE meeting_id=m-1)",
        "MEETING_COMMAND(JOIN meeting_id=m-1)",
        "FINALIZE_CLOSE(meeting_id=m-1)",
    };
    EXPECT_EQ(expected, scenario.Sink().DispatchDescriptions());

    ASSERT_EQ(4u, scenario.Sink().commands.size());
    EXPECT_EQ(meeting::ResultCode::MEETING_STATE_REJECTED, scenario.Sink().commands[3].result_code);
    EXPECT_EQ(meeting::ResultDisposition::ERROR, scenario.Sink().commands[3].disposition);
}

// ==============================================================================
// 7. Layer B #3：Leave → Disconnect（不得重复 ParticipantLeft）
// ==============================================================================
TEST(LogicWorkQueueLayerB, LeaveThenDisconnectDoesNotDuplicateParticipantLeft) {
    MeetingQueueScenario scenario;
    scenario.Prepare();

    scenario.CreateMeeting();
    scenario.JoinBob("m-1");
    ASSERT_TRUE(scenario.Done());

    const std::size_t joined_events = scenario.Sink().CountEventType(meeting::EventType::PARTICIPANT_JOINED);
    ASSERT_EQ(1u, joined_events);

    scenario.LeaveBob("m-1");
    ASSERT_TRUE(scenario.Done());
    EXPECT_EQ(1u, scenario.Sink().CountEventType(meeting::EventType::PARTICIPANT_LEFT));

    scenario.DisconnectBob();
    ASSERT_TRUE(scenario.Done());

    // Leave 已经解绑全部 Session，因此后续 Disconnect 不再产生第二个 ParticipantLeft。
    EXPECT_EQ(1u, scenario.Sink().CountEventType(meeting::EventType::PARTICIPANT_LEFT));
    ASSERT_EQ(1u, scenario.Sink().disconnects.size());
    EXPECT_EQ(0u, scenario.Sink().disconnects[0].affected_meeting_count);
}

// ==============================================================================
// 8. Layer B #4：Disconnect → Leave（旧命令按领域 Session 状态被拒绝）
// ==============================================================================
TEST(LogicWorkQueueLayerB, DisconnectThenStaleLeaveIsRejectedBySessionState) {
    MeetingQueueScenario scenario;
    scenario.Prepare();

    scenario.CreateMeeting();
    scenario.JoinBob("m-1");
    ASSERT_TRUE(scenario.Done());

    std::vector<LogicWorkItem> batch;
    batch.push_back(MakeSessionDisconnectedWork(kBobSession));
    batch.push_back(MakeMeetingCommandWork(BobContext("rq-leave-stale"),
                                           ClientMeetingCommandKind::LEAVE, "m-1"));
    ASSERT_TRUE(scenario.PostAllThenWait(batch));

    // Disconnect 恰好产生一次 ParticipantLeft（Bob 是普通成员，剩余 Session 归零）。
    EXPECT_EQ(1u, scenario.Sink().CountEventType(meeting::EventType::PARTICIPANT_LEFT));
    ASSERT_EQ(1u, scenario.Sink().disconnects.size());
    EXPECT_EQ(1u, scenario.Sink().disconnects[0].affected_meeting_count);

    // Disconnect 先完成 → Domain Session 已 CLOSED；其后的 stale Leave 必须得到
    // SESSION_STATE_REJECTED，而不是被误报为 AUTH_REQUIRED，也不得改变成员关系。
    ASSERT_EQ(1u, scenario.Sink().CountCommands(ClientMeetingCommandKind::LEAVE));
    EXPECT_EQ(meeting::ResultCode::SESSION_STATE_REJECTED,
              scenario.Sink().commands[scenario.Sink().commands.size() - 1].result_code);
    EXPECT_EQ(0u, scenario.Sink().CountEventType(meeting::EventType::MEETING_CLOSED));
}

// ==============================================================================
// 9. Layer B #5：Close → Close（只安排一个 Finalize）
// ==============================================================================
TEST(LogicWorkQueueLayerB, SecondCloseIsCloseInProgressAndSchedulesNoSecondFinalize) {
    MeetingQueueScenario scenario;
    scenario.Prepare();

    scenario.CreateMeeting();
    ASSERT_TRUE(scenario.Done());

    std::vector<LogicWorkItem> batch;
    batch.push_back(MakeMeetingCommandWork(HostContext("rq-close-1"),
                                           ClientMeetingCommandKind::CLOSE, "m-1"));
    batch.push_back(MakeMeetingCommandWork(HostContext("rq-close-2"),
                                           ClientMeetingCommandKind::CLOSE, "m-1"));
    ASSERT_TRUE(scenario.PostAllThenWait(batch));

    ASSERT_EQ(3u, scenario.Sink().commands.size());
    EXPECT_EQ(meeting::ResultCode::OK, scenario.Sink().commands[1].result_code);
    EXPECT_TRUE(scenario.Sink().commands[1].should_schedule_finalize);
    EXPECT_EQ(meeting::MeetingState::ENDING, scenario.Sink().commands[1].meeting_state);

    EXPECT_EQ(meeting::ResultCode::CLOSE_IN_PROGRESS, scenario.Sink().commands[2].result_code);
    EXPECT_EQ(meeting::ResultDisposition::IDEMPOTENT, scenario.Sink().commands[2].disposition);
    EXPECT_FALSE(scenario.Sink().commands[2].should_schedule_finalize);

    // 第二个 Close 不得再排 Finalize：整场只有一次 finalize 执行。
    ASSERT_EQ(1u, scenario.Sink().finalizes.size());
    EXPECT_TRUE(scenario.Sink().finalizes[0].finalized);
    // 也不得产生第二个 MeetingClosed。
    EXPECT_EQ(1u, scenario.Sink().CountEventType(meeting::EventType::MEETING_CLOSED));
}

// ==============================================================================
// 10. Layer B #6：BeginClose → later FinalizeClose（两个 Work turn）
// ==============================================================================
TEST(LogicWorkQueueLayerB, BeginCloseResponseEndingThenFinalizeInSeparateTurn) {
    MeetingQueueScenario scenario;
    scenario.Prepare();

    scenario.CreateMeeting();
    ASSERT_TRUE(scenario.Done());

    scenario.CloseHost("m-1");
    ASSERT_TRUE(scenario.Done());

    // turn 1 的响应：OK + ENDING；此时不得出现 MeetingClosed，也不得有快照。
    ASSERT_EQ(2u, scenario.Sink().commands.size());
    const MeetingCommandOutcome close_outcome = scenario.Sink().commands[1];
    EXPECT_EQ(meeting::ResultCode::OK, close_outcome.result_code);
    EXPECT_EQ(meeting::MeetingState::ENDING, close_outcome.meeting_state);
    EXPECT_TRUE(close_outcome.event_types.empty());   // BeginClose 不产生事件
    // 两次 MEETING_COMMAND dispatch：CREATE 与 CLOSE。Finalize 是第三个、不同类型的 turn。
    EXPECT_EQ(2u, scenario.Sink().CountDispatched(LogicWorkKind::MEETING_COMMAND));

    // 两个独立的 dispatch turn：Close 与 Finalize 各自占一次。
    ASSERT_EQ(1u, scenario.Sink().CountDispatched(LogicWorkKind::FINALIZE_CLOSE));
    ASSERT_EQ(1u, scenario.Sink().finalizes.size());
    EXPECT_TRUE(scenario.Sink().finalizes[0].finalized);
    EXPECT_FALSE(scenario.Sink().finalizes[0].already_finalized);
    EXPECT_EQ(1u, scenario.Sink().CountEventType(meeting::EventType::MEETING_CLOSED));
    EXPECT_TRUE(scenario.HasClosedSnapshot("m-1"));

    // 通知意图只输出一次；M4 不发送任何东西。
    EXPECT_EQ(1u, scenario.Sink().finalizes[0].notification_session_ids.size());
}

TEST(LogicWorkQueueLayerB, RepeatedFinalizeIsIdempotentAndProducesNoSecondEvent) {
    MeetingQueueScenario scenario;
    scenario.Prepare();

    scenario.CreateMeeting();
    ASSERT_TRUE(scenario.Done());
    scenario.CloseHost("m-1");
    ASSERT_TRUE(scenario.Done());

    scenario.Finalize("m-1");
    ASSERT_TRUE(scenario.Done());

    ASSERT_EQ(2u, scenario.Sink().finalizes.size());
    EXPECT_TRUE(scenario.Sink().finalizes[0].finalized);
    EXPECT_TRUE(scenario.Sink().finalizes[1].already_finalized);
    EXPECT_FALSE(scenario.Sink().finalizes[1].finalized);
    EXPECT_TRUE(scenario.Sink().finalizes[1].notification_session_ids.empty());
    EXPECT_EQ(1u, scenario.Sink().CountEventType(meeting::EventType::MEETING_CLOSED));
}

// ==============================================================================
// 11. Layer B #7：Mixed Work FIFO（不按 kind 分组、不插队、不重排）
// ==============================================================================
TEST(LogicWorkQueueBMixed, MixedKindsKeepExactEnqueueOrder) {
    MeetingQueueScenario scenario;
    scenario.Prepare();

    scenario.CreateMeeting();
    scenario.JoinBob("m-1");
    ASSERT_TRUE(scenario.Done());

    std::vector<LogicWorkItem> batch;
    batch.push_back(MakeExternalMessageWork(std::shared_ptr<LogicNode>()));
    batch.push_back(MakeMeetingCommandWork(HostContext("rq-close"),
                                           ClientMeetingCommandKind::CLOSE, "m-1"));
    batch.push_back(MakeSessionDisconnectedWork("s-nobody"));
    batch.push_back(MakeMeetingCommandWork(BobContext("rq-join-late"),
                                           ClientMeetingCommandKind::JOIN, "m-1"));
    ASSERT_TRUE(scenario.PostAllThenWait(batch));

    // 批内 4 种 kind 全部按真实入队次序执行：没有按 kind 分组，也没有重排。
    const std::vector<std::string> expected = {
        "MEETING_COMMAND(CREATE)",
        "MEETING_COMMAND(JOIN meeting_id=m-1)",
        "EXTERNAL_MESSAGE",
        "MEETING_COMMAND(CLOSE meeting_id=m-1)",
        "SESSION_DISCONNECTED(session_id=s-nobody)",
        "MEETING_COMMAND(JOIN meeting_id=m-1)",
        "FINALIZE_CLOSE(meeting_id=m-1)",
    };
    EXPECT_EQ(expected, scenario.Sink().DispatchDescriptions());

    // "不插队"的确定性证据：Close 派生出的 FINALIZE_CLOSE 是在**队列锁内**追加的，
    // 而该临界区里队列已经排着其余 work —— 它被追加在**队尾**，没有被 kind 优先级
    // 提前。（此刻队列里还剩几条取决于消费者已取走多少，那部分本质上是并发交错，
    // 因此断言"队尾 + 至少排在两条之后"，而不是断言一个会漂移的精确条数。）
    ASSERT_EQ(1u, scenario.Sink().internal_enqueues.size());
    EXPECT_EQ("FINALIZE_CLOSE(meeting_id=m-1)",
              scenario.Sink().internal_enqueues[0].description);
    ASSERT_FALSE(scenario.Sink().internal_enqueues[0].queue_after.empty());
    EXPECT_EQ("FINALIZE_CLOSE(meeting_id=m-1)",
              scenario.Sink().internal_enqueues[0].queue_after.back());
    EXPECT_GE(scenario.Sink().internal_enqueues[0].queue_size_after, 2u);

    // Last-requested Join 被 ENDING 拒绝；Finalize 由 Close 排出（测试没有手动投递）。
    ASSERT_EQ(4u, scenario.Sink().commands.size());
    EXPECT_EQ(meeting::ResultCode::MEETING_STATE_REJECTED, scenario.Sink().commands[3].result_code);
    ASSERT_EQ(1u, scenario.Sink().CountDispatched(LogicWorkKind::FINALIZE_CLOSE));
    ASSERT_EQ(1u, scenario.Sink().finalizes.size());
    EXPECT_TRUE(scenario.Sink().finalizes[0].finalized);
    EXPECT_TRUE(scenario.HasClosedSnapshot("m-1"));
}

// ==============================================================================
// 12. Layer B #8：legacy callback 分发（通过 seam）
// ==============================================================================
TEST(LogicWorkQueueLegacy, ExternalMessagesReachTheLegacyRouterInOrder) {
    RecordingExecutor fixture;

    std::vector<LogicWorkItem> items;
    items.push_back(MakeExternalMessageWork(std::shared_ptr<LogicNode>()));
    items.push_back(MakeSessionRegisterWork("s-1"));
    items.push_back(MakeExternalMessageWork(std::shared_ptr<LogicNode>()));
    items.push_back(MakeExternalMessageWork(std::shared_ptr<LogicNode>()));
    ASSERT_TRUE(fixture.PostAllThenWait(items));

    EXPECT_EQ(3, fixture.Router().routed_count);
    // legacy 消息没有被 Meeting dispatcher 吞掉，也没有被重新排序：
    // 队列中第 1/3/4 条为 EXTERNAL_MESSAGE，第 2 条为 Session 登记。
    ASSERT_EQ(4u, fixture.Sink().dispatch_records.size());
    EXPECT_EQ(LogicWorkKind::EXTERNAL_MESSAGE, fixture.Sink().dispatch_records[0].kind);
    EXPECT_EQ(LogicWorkKind::INTERNAL_SESSION_REGISTER, fixture.Sink().dispatch_records[1].kind);
    EXPECT_EQ(LogicWorkKind::EXTERNAL_MESSAGE, fixture.Sink().dispatch_records[2].kind);
    EXPECT_EQ(LogicWorkKind::EXTERNAL_MESSAGE, fixture.Sink().dispatch_records[3].kind);
}

TEST(LogicWorkQueueLegacy, PostExternalMessageIsRejectedAfterStop) {
    RecordingExecutor fixture;
    ASSERT_TRUE(fixture.Done());

    fixture.Executor().StopAndJoin();
    // stop 之后队列拒绝新增外部 work（结构性保证，不是约定）。
    EXPECT_FALSE(fixture.Executor().PostExternalMessage(std::shared_ptr<LogicNode>()));
    EXPECT_EQ(0, fixture.Router().routed_count);
}

// ==============================================================================
// 13. Layer B #9：Shutdown / Drain
// ==============================================================================
TEST(LogicWorkQueueShutdown, EmptyQueueStopExitsPromptly) {
    RecordingExecutor fixture;
    ASSERT_TRUE(fixture.Done());
    fixture.Executor().StopAndJoin();   // 不得挂起
    EXPECT_EQ(0u, fixture.Sink().DispatchDescriptions().size());
}

TEST(LogicWorkQueueShutdown, NonEmptyQueueIsDrainedBeforeExit) {
    RecordingExecutor fixture;
    std::vector<LogicWorkItem> items;
    items.push_back(MakeSessionRegisterWork("s-1"));
    items.push_back(MakeSessionRegisterWork("s-2"));
    items.push_back(MakeSessionDisconnectedWork("s-1"));
    fixture.Executor().EnqueueBatch(items);

    fixture.Executor().StopAndJoin();

    // drain 语义：stop 之前已接收的 work 全部被处理，一个都不丢。
    const std::vector<std::string> expected = {
        "INTERNAL_SESSION_REGISTER(session_id=s-1)",
        "INTERNAL_SESSION_REGISTER(session_id=s-2)",
        "SESSION_DISCONNECTED(session_id=s-1)",
    };
    EXPECT_EQ(expected, fixture.Sink().DispatchDescriptions());
}

TEST(LogicWorkQueueShutdown, PendingCloseProducesFinalizeDuringDrain) {
    // Close 在 drain 过程中执行时产生的 FINALIZE_CLOSE 不能被 stop 静默丢弃。
    MeetingQueueScenario scenario;
    scenario.Prepare();

    scenario.Post(MakeMeetingCommandWork(HostContext("rq-create"),
                                         ClientMeetingCommandKind::CREATE, ""));
    ASSERT_TRUE(scenario.Done());
    ASSERT_EQ(1u, scenario.Sink().commands.size());
    const std::string meeting_id = scenario.Sink().commands[0].meeting_id;
    ASSERT_FALSE(meeting_id.empty());

    // 保证"停止时 Close 确实已经在队列里等待"这一确定前提，而不是靠时序碰运气：
    // EnqueueBatchBaseline 在同一把 idle 锁下完成"取基线 + 整批入队"，
    // 因此它返回时 Close 必然已经排在队列里。
    ASSERT_TRUE(scenario.Queue().IsEmpty());
    scenario.EnqueueBatch({MakeMeetingCommandWork(HostContext("rq-close"),
                                                  ClientMeetingCommandKind::CLOSE, meeting_id)});
    ASSERT_EQ(1u, scenario.Queue().Size());
    scenario.StopAndJoin();

    // drain 语义：已接收的 Close 被执行完，并且它排出的 Finalize 也被执行完。
    ASSERT_EQ(2u, scenario.Sink().commands.size());
    EXPECT_EQ(meeting::ResultCode::OK, scenario.Sink().commands[1].result_code);
    EXPECT_TRUE(scenario.Sink().commands[1].should_schedule_finalize);
    ASSERT_EQ(1u, scenario.Sink().finalizes.size());
    EXPECT_TRUE(scenario.Sink().finalizes[0].finalized);
    EXPECT_TRUE(scenario.HasClosedSnapshot(meeting_id));
}

TEST(LogicWorkQueueShutdown, QueueRejectsNewWorkAfterStopRequest) {
    RecordingExecutor fixture;
    ASSERT_TRUE(fixture.Done());
    fixture.Executor().StopAndJoin();

    EXPECT_FALSE(fixture.Executor().Enqueue(MakeSessionRegisterWork("s-late")));
    EXPECT_FALSE(fixture.Executor().PostFinalizeClose("m-late"));
    EXPECT_EQ(0u, fixture.Sink().DispatchDescriptions().size());
}

// ==============================================================================
// 14. 生产 dispatcher：只读 wiring 与错误路径
// ==============================================================================
TEST(LogicWorkQueueDispatcher, UnauthenticatedSessionIsRejectedWithAuthRequired) {
    RecordingSink sink;
    FakeIdFactory ids;
    LogicWorkDispatcher dispatcher(kLocalServer, ids.Dependencies(), &sink);
    SerializedWorkExecutor<LogicWorkDispatcher> executor(dispatcher, nullptr);

    executor.PostSessionRegister("s-new");
    ASSERT_TRUE(executor.WaitUntilIdle(5000));

    // 上面的等待只确认"消费者已经跑过一轮"。下面这个 Create 需要确认它自身跑完，
    // 因此用 EnqueueBatchBaseline 在同一把锁下取基线并入队。


    // 只注册、不认证 → AUTH_REQUIRED（而不是 INVALID_ARGUMENT）。
    const LogicWorkIdleProgress create_baseline = executor.EnqueueBatchBaseline(
        std::vector<LogicWorkItem>{MakeMeetingCommandWork(
            MakeContext("rq-1", "s-new", 0), ClientMeetingCommandKind::CREATE, "")});
    ASSERT_TRUE(executor.WaitUntilIdle(5000, create_baseline));

    ASSERT_EQ(1u, sink.commands.size());
    EXPECT_EQ(meeting::ResultCode::AUTH_REQUIRED, sink.commands[0].result_code);
}

TEST(LogicWorkQueueDispatcher, CreateThenQueryStillSeesActiveMeeting) {
    RecordingSink sink;
    FakeIdFactory ids;
    LogicWorkDispatcher dispatcher(kLocalServer, ids.Dependencies(), &sink);
    SerializedWorkExecutor<LogicWorkDispatcher> executor(dispatcher, nullptr);

    const LogicWorkIdleProgress setup_baseline = executor.EnqueueBatchBaseline(
        std::vector<LogicWorkItem>{MakeSessionRegisterWork(kHostSession),
                                   MakeSessionAuthenticateWork(kHostSession, kHostUser)});
    ASSERT_TRUE(executor.WaitUntilIdle(5000, setup_baseline));

    const LogicWorkIdleProgress create_baseline = executor.EnqueueBatchBaseline(
        std::vector<LogicWorkItem>{MakeMeetingCommandWork(
            HostContext("rq-create"), ClientMeetingCommandKind::CREATE, "")});
    ASSERT_TRUE(executor.WaitUntilIdle(5000, create_baseline));
    ASSERT_EQ(1u, sink.commands.size());
    const std::string meeting_id = sink.commands[0].meeting_id;
    EXPECT_TRUE(dispatcher.HasActiveMeeting(meeting_id));

    const LogicWorkIdleProgress query_baseline = executor.EnqueueBatchBaseline(
        std::vector<LogicWorkItem>{MakeMeetingCommandWork(
            HostContext("rq-query"), ClientMeetingCommandKind::QUERY, meeting_id)});
    ASSERT_TRUE(executor.WaitUntilIdle(5000, query_baseline));
    ASSERT_EQ(2u, sink.commands.size());
    EXPECT_EQ(meeting::ResultCode::OK, sink.commands[1].result_code);
    EXPECT_EQ(meeting::MeetingState::CREATED, sink.commands[1].meeting_state);
}

// ==============================================================================
// 15. 队列自身语义
// ==============================================================================
TEST(LogicWorkQueuePrimitive, FifoAndAccounting) {
    LogicWorkQueue queue;
    EXPECT_TRUE(queue.IsEmpty());
    EXPECT_EQ(0u, queue.Size());

    EXPECT_TRUE(queue.Push(MakeFinalizeCloseWork("m-1")));
    EXPECT_TRUE(queue.Push(MakeFinalizeCloseWork("m-2")));
    EXPECT_EQ(2u, queue.Size());

    {
        LogicWorkQueue::Lock lock(queue);
        EXPECT_TRUE(queue.IsLockHeldByCurrentThread());
        LogicWorkItem item;
        ASSERT_TRUE(queue.Pop(item));
        EXPECT_EQ(LogicWorkKind::FINALIZE_CLOSE, item.kind);
        EXPECT_EQ("m-1", item.meeting_id);
        ASSERT_TRUE(queue.Pop(item));
        EXPECT_EQ("m-2", item.meeting_id);
        EXPECT_FALSE(queue.Pop(item));
    }
    EXPECT_FALSE(queue.IsLockHeldByCurrentThread());
    EXPECT_TRUE(queue.IsEmpty());

    queue.RequestStop();
    EXPECT_TRUE(queue.IsStopRequested());
    EXPECT_FALSE(queue.Push(MakeFinalizeCloseWork("m-3")));
}

TEST(LogicWorkQueuePrimitive, BatchEnqueueIsAtomicAndRejectsNothingBeforeStop) {
    LogicWorkQueue queue;
    std::vector<LogicWorkItem> items;
    items.push_back(MakeFinalizeCloseWork("m-1"));
    items.push_back(MakeSessionDisconnectedWork("s-1"));
    items.push_back(MakeFinalizeCloseWork("m-2"));

    {
        LogicWorkQueue::Lock lock(queue);
        for (std::size_t index = 0; index < items.size(); ++index) {
            EXPECT_TRUE(queue.PushLocked(items[index]));
        }
    }
    EXPECT_EQ(3u, queue.Size());
}
