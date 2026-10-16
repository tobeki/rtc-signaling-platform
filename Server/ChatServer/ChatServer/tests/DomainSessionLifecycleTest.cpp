// ==============================================================================
// M5.1 Domain Session Lifecycle 单元测试。
//
// 覆盖目标：把"真实 TCP Session 生命周期"接入 M4 唯一 serialized FIFO 之后的
// **领域可观测契约**：
//
//   accept 成功 → REGISTER    → MeetingService::RegisterConnectedSession → CONNECTED
//   登录成功    → AUTHENTICATE → MeetingService::AuthenticateSession      → AUTHENTICATED
//   断线        → DISCONNECT  → MeetingService::SessionDisconnected       → CLOSED
//
// 测试切口与 M4/M5 同一哲学：测试执行的是**生产实现本身**
//
//   生产:  CServer::HandleAccept / LogicSystem::LoginHandler / CSession::Close()
//              └── LogicSystem::PostSessionRegisterWork / PostSessionAuthenticateWork
//                  / SessionDisconnectBridge（断线）
//                        └── 唯一 LogicWorkQueue → SerializedWorkExecutor 单消费者
//                              └── LogicWorkDispatcher → MeetingService
//
//   测试:  与上面**同一批** typed 入队入口（MakeSession*Work / SessionDisconnectBridge）
//              └── 真实 SerializedWorkExecutor<LogicWorkDispatcher> + 真实 MeetingService
//
// 刻意不进入本 target 的：CServer.cpp / CSession.cpp / LogicSystem.cpp。它们会把
// asio / Boost / Redis / MySQL / gRPC / jsoncpp 整条 legacy 业务链路拉进纯单元测试
// （Step 2.5 §14、M4 §18）。因此本文件证明的是"生命周期事件的领域契约 + FIFO 顺序"，
// 而"CServer 是否在 accept 成功分支、Start() 之前投递"由静态复核保证（见 M5.1 文档 §6）。
// ==============================================================================

#include "LogicWorkDispatcher.h"
#include "SessionDisconnectBridge.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
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
// 测试固定值。
// ------------------------------------------------------------------------------
const char* kLocalServer = "local-chatserver";
const char* kMeetingOne = "m-1";

const char* kHostSession = "s-host";             // 会议创建者
const char* kHostSecondSession = "s-host-2";     // 同一 Host 的第二条 Session（用于读回状态）
const int kHostUser = 10;

const char* kUserASession = "s-user-a";          // 普通参与者 user 100 的第一条 Session
const char* kUserBSession = "s-user-b";          // 普通参与者 user 100 的第二条 Session
const char* kNeverSession = "s-never-accepted";  // 只被构造、从未 accept 成功的 Session
const int kMultiUser = 100;

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

meeting::MeetingRequestContext HostSecondContext(const std::string& request_id) {
    return MakeContext(request_id, kHostSecondSession, kHostUser);
}

meeting::MeetingRequestContext UserAContext(const std::string& request_id) {
    return MakeContext(request_id, kUserASession, kMultiUser);
}

meeting::MeetingRequestContext UserBContext(const std::string& request_id) {
    return MakeContext(request_id, kUserBSession, kMultiUser);
}

// ------------------------------------------------------------------------------
// 记录型 legacy 路由器。
//
// 它存在的意义是把"legacy 消息"放进**同一个有序列表**，从而让 M5.1 能断言
// "先入队的内部生命周期 work 对后入队的 legacy 消息 / Meeting 命令已经生效"。
// 注意 LogicNode 在本 target 里是不完整类型（刻意不把 asio 拖进来），因此只能
// 记录"收到了一条消息"以及它属于哪个 session。
// ------------------------------------------------------------------------------
class RecordingLegacyRouter : public ILegacyMessageRouter {
public:
    explicit RecordingLegacyRouter(IWorkObservationSink* sink) : sink_(sink) {}

    void RouteExternalMessage(const std::shared_ptr<LogicNode>& node) {
        (void)node;
        ++routed_count_;
        if (sink_ != nullptr) {
            sink_->RecordLegacyMessageRouted(kHostSession, static_cast<short>(routed_count_), 0);
        }
    }

    int RoutedCount() const { return routed_count_; }

private:
    IWorkObservationSink* sink_;
    int routed_count_ = 0;
};

// ------------------------------------------------------------------------------
// 观测 sink：把真实 dispatch 次序与领域结果落成可断言的 value。
// ------------------------------------------------------------------------------
class LifecycleRecordingSink : public IWorkObservationSink {
public:
    struct DisconnectRecord {
        meeting::SessionId session_id;
        std::size_t affected_meeting_count = 0;
        std::size_t event_count = 0;
    };

    void RecordWorkDispatched(const LogicWorkItem& item,
                              unsigned long long,
                              const std::vector<WorkDomainEvent>& events) {
        events_in_order.push_back(item.Describe());
        dispatched_kinds.push_back(item.kind);
        for (std::size_t index = 0; index < events.size(); ++index) {
            event_types.push_back(events[index].type);
        }
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

    void RecordFinalizeClose(const meeting::MeetingId&, bool, bool,
                             const std::vector<meeting::SessionId>&) {}

    void RecordMeetingCommand(const MeetingCommandOutcome& outcome) {
        commands.push_back(outcome);
    }

    void RecordInternalWorkEnqueued(const LogicWorkItem&,
                                    const std::vector<LogicWorkItem>&) {}

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
    std::vector<MeetingCommandOutcome> commands;

private:
    std::size_t last_dispatch_event_count_ = 0;
};

// ------------------------------------------------------------------------------
// FIFO 投递目标替身：与生产 LogicSystem::PostSessionDisconnectedWork 等价
// （唯一差别是生产实现还会先做一次 EnsureWorkQueueStarted 惰性启动）。
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
// 生产同构场景：真实 executor + 真实 dispatcher + 真实桥接 + 固定 ID 注入。
// ------------------------------------------------------------------------------
class DomainSessionScenario {
public:
    // 初始化顺序必须与成员声明顺序一致，否则会产生 -Wreorder 告警。
    DomainSessionScenario()
        : router_(&sink_),
          ids_(),
          dispatcher_(kLocalServer, ids_.Dependencies(), &sink_),
          executor_(dispatcher_, &router_),
          fifo_sink_(executor_) {}

    LifecycleRecordingSink& Sink() { return sink_; }
    RecordingLegacyRouter& Router() { return router_; }
    LogicWorkDispatcher& Dispatcher() { return dispatcher_; }
    SerializedWorkExecutor<LogicWorkDispatcher>& Executor() { return executor_; }
    LogicWorkQueue& Queue() { return executor_.Queue(); }

    // 真实 M5 断线桥接：投递目标是唯一 FIFO（与 CSession 内部形态完全一致）。
    bool BindBridge(const std::string& session_id) {
        bridge_.reset(new SessionDisconnectBridge());
        return bridge_->Bind(&fifo_sink_, session_id);
    }

    SessionDisconnectBridge& Bridge() { return *bridge_; }

    bool PostBatchThenWait(const std::vector<LogicWorkItem>& items) {
        const LogicWorkIdleProgress baseline = executor_.EnqueueBatchBaseline(items);
        return executor_.WaitUntilIdle(5000, baseline);
    }

    bool PostOneThenWait(const LogicWorkItem& item) {
        return PostBatchThenWait(std::vector<LogicWorkItem>(1, item));
    }

    bool Done() { return executor_.WaitUntilIdle(5000); }

    void StopAndJoin() { executor_.StopAndJoin(); }

    // --- 生命周期入口（与生产 LogicSystem / CSession 调用的是同一批 typed 入口）----

    bool Register(const std::string& session_id) {
        return PostOneThenWait(MakeSessionRegisterWork(session_id));
    }

    bool Authenticate(const std::string& session_id, meeting::UserId user_id) {
        return PostOneThenWait(MakeSessionAuthenticateWork(session_id, user_id));
    }

    void PostCommand(const meeting::MeetingRequestContext& context,
                     ClientMeetingCommandKind command,
                     const meeting::MeetingId& meeting_id) {
        executor_.PostMeetingCommand(context, command, meeting_id);
    }

    bool CommandThenWait(const meeting::MeetingRequestContext& context,
                         ClientMeetingCommandKind command,
                         const meeting::MeetingId& meeting_id) {
        return PostOneThenWait(MakeMeetingCommandWork(context, command, meeting_id));
    }

    std::string CreateMeetingAs(const meeting::MeetingRequestContext& context) {
        if (!CommandThenWait(context, ClientMeetingCommandKind::CREATE, "")) {
            return std::string();
        }
        return dispatcher_.LastCreatedMeetingId();
    }

private:
    // 声明顺序即初始化/析构顺序：executor_ 必须先于 dispatcher_ 析构（worker 在使用它），
    // fifo_sink_ / bridge_ 必须先于 executor_ 析构。
    LifecycleRecordingSink sink_;
    RecordingLegacyRouter router_;
    FakeIdFactory ids_;
    LogicWorkDispatcher dispatcher_;
    SerializedWorkExecutor<LogicWorkDispatcher> executor_;
    ExecutorDisconnectSink fifo_sink_;
    std::unique_ptr<SessionDisconnectBridge> bridge_;
};

} // namespace

// ==============================================================================
// 1. 注册：accept 成功之后才有 CONNECTED
// ==============================================================================

TEST(DomainSessionRegister, RegisterWorkMakesSessionAuthenticatable) {
    DomainSessionScenario scenario;

    // REGISTER → AUTHENTICATE → MEETING_COMMAND，顺序即真实入队顺序。
    std::vector<LogicWorkItem> batch;
    batch.push_back(MakeSessionRegisterWork(kHostSession));
    batch.push_back(MakeSessionAuthenticateWork(kHostSession, kHostUser));
    batch.push_back(MakeMeetingCommandWork(HostContext("m51-create"),
                                           ClientMeetingCommandKind::CREATE, ""));
    ASSERT_TRUE(scenario.PostBatchThenWait(batch));

    ASSERT_EQ(1u, scenario.Sink().commands.size());
    const MeetingCommandOutcome& create = scenario.Sink().LastCommand();
    // CREATE 只对 AUTHENTICATED 会话成功；而 AUTHENTICATE 只对 CONNECTED 会话成功
    // （M3 契约）——因此 OK 同时证明 REGISTER 与 AUTHENTICATE 都真实生效了。
    EXPECT_EQ(meeting::ResultCode::OK, create.result_code);
    EXPECT_EQ(meeting::MeetingState::CREATED, create.meeting_state);
    EXPECT_EQ(kMeetingOne, create.meeting_id);
    EXPECT_EQ(1u, create.active_participant_count);   // Host 是唯一 ACTIVE 参与者
    EXPECT_TRUE(scenario.Dispatcher().HasActiveMeeting(kMeetingOne));
}

TEST(DomainSessionRegister, AuthenticateOrCommandForNeverRegisteredSessionIsRejected) {
    DomainSessionScenario scenario;

    // 模拟"CSession 被构造但 accept 从未成功"：没有任何 REGISTER 投递点。
    // 领域侧不得凭空产生这个 session，也不得因为 AUTHENTICATE 而伪造身份。
    std::vector<LogicWorkItem> batch;
    batch.push_back(MakeSessionAuthenticateWork(kNeverSession, kHostUser));
    batch.push_back(MakeMeetingCommandWork(
        MakeContext("m51-never", kNeverSession, kHostUser),
        ClientMeetingCommandKind::CREATE, ""));
    ASSERT_TRUE(scenario.PostBatchThenWait(batch));

    ASSERT_EQ(1u, scenario.Sink().commands.size());
    // 缺失的 Domain Session 报 AUTH_REQUIRED（而不是被当成已认证，也不是 SESSION_STATE_REJECTED）。
    EXPECT_EQ(meeting::ResultCode::AUTH_REQUIRED, scenario.Sink().LastCommand().result_code);
    EXPECT_TRUE(scenario.Dispatcher().LastCreatedMeetingId().empty());
    EXPECT_FALSE(scenario.Dispatcher().HasActiveMeeting(kMeetingOne));
}

TEST(DomainSessionRegister, RegisterIsIdempotentAndDoesNotOverwriteAuthenticated) {
    DomainSessionScenario scenario;

    // 第二次 REGISTER 必须是幂等 no-op：M3 绝不把 AUTHENTICATED 覆盖回 CONNECTED。
    // 若被覆盖，紧随其后的 CREATE（要求 AUTHENTICATED）就会失败。
    std::vector<LogicWorkItem> batch;
    batch.push_back(MakeSessionRegisterWork(kHostSession));
    batch.push_back(MakeSessionAuthenticateWork(kHostSession, kHostUser));
    batch.push_back(MakeSessionRegisterWork(kHostSession));
    batch.push_back(MakeMeetingCommandWork(HostContext("m51-create"),
                                           ClientMeetingCommandKind::CREATE, ""));
    ASSERT_TRUE(scenario.PostBatchThenWait(batch));

    ASSERT_EQ(1u, scenario.Sink().commands.size());
    EXPECT_EQ(meeting::ResultCode::OK, scenario.Sink().LastCommand().result_code);
    EXPECT_EQ(1u, scenario.Sink().LastCommand().active_participant_count);
}

TEST(DomainSessionRegister, AuthenticateWithNonPositiveUserIdIsRejectedThenSucceedsWithValidUid) {
    DomainSessionScenario scenario;

    // 失败认证（uid = 0 / 负数）不得把会话变成 AUTHENTICATED，也不得破坏它。
    std::vector<LogicWorkItem> failed;
    failed.push_back(MakeSessionRegisterWork(kHostSession));
    failed.push_back(MakeSessionAuthenticateWork(kHostSession, 0));
    failed.push_back(MakeSessionAuthenticateWork(kHostSession, -5));
    failed.push_back(MakeMeetingCommandWork(HostContext("m51-create-1"),
                                            ClientMeetingCommandKind::CREATE, ""));
    ASSERT_TRUE(scenario.PostBatchThenWait(failed));
    ASSERT_EQ(1u, scenario.Sink().commands.size());
    EXPECT_EQ(meeting::ResultCode::AUTH_REQUIRED, scenario.Sink().LastCommand().result_code);

    // 会话仍然停在 CONNECTED：合法的认证随后必须成功（失败认证没有把它弄坏）。
    std::vector<LogicWorkItem> ok;
    ok.push_back(MakeSessionAuthenticateWork(kHostSession, kHostUser));
    ok.push_back(MakeMeetingCommandWork(HostContext("m51-create-2"),
                                        ClientMeetingCommandKind::CREATE, ""));
    ASSERT_TRUE(scenario.PostBatchThenWait(ok));
    ASSERT_EQ(2u, scenario.Sink().commands.size());
    EXPECT_EQ(meeting::ResultCode::OK, scenario.Sink().LastCommand().result_code);
    EXPECT_EQ(1u, scenario.Sink().LastCommand().active_participant_count);
}

// ==============================================================================
// 2. 顺序：先入队的生命周期 work 对后入队的一切已经生效
// ==============================================================================

TEST(DomainSessionOrdering, RegisterAndAuthenticateAreOrderedBeforeTheSessionsFirstCommand) {
    // A 组：内部生命周期 work 排在最前，随后是同一 session 的三条 legacy 消息，
    //       最后才是它的第一条 Meeting 命令。FIFO 语义要求前者对后者已经生效。
    DomainSessionScenario scenario;
    std::vector<LogicWorkItem> batch;
    batch.push_back(MakeSessionRegisterWork(kHostSession));
    batch.push_back(MakeSessionAuthenticateWork(kHostSession, kHostUser));
    batch.push_back(MakeExternalMessageWork(std::shared_ptr<LogicNode>()));
    batch.push_back(MakeExternalMessageWork(std::shared_ptr<LogicNode>()));
    batch.push_back(MakeExternalMessageWork(std::shared_ptr<LogicNode>()));
    batch.push_back(MakeMeetingCommandWork(HostContext("m51-create"),
                                           ClientMeetingCommandKind::CREATE, ""));
    ASSERT_TRUE(scenario.PostBatchThenWait(batch));

    EXPECT_EQ(3, scenario.Router().RoutedCount());
    ASSERT_EQ(1u, scenario.Sink().commands.size());
    EXPECT_EQ(meeting::ResultCode::OK, scenario.Sink().LastCommand().result_code);
    EXPECT_EQ(1u, scenario.Sink().LastCommand().active_participant_count);

    // 有序列表：三条 legacy 消息都排在 MEETING_COMMAND 之前，且队列没有按 kind 分组或重排。
    const std::vector<std::string> tail = scenario.Sink().TailOfEvents(4);
    ASSERT_EQ(4u, tail.size());
    EXPECT_EQ("EXTERNAL_MESSAGE(session_id=s-host msg_id=1)", tail[0]);
    EXPECT_EQ("EXTERNAL_MESSAGE(session_id=s-host msg_id=2)", tail[1]);
    EXPECT_EQ("EXTERNAL_MESSAGE(session_id=s-host msg_id=3)", tail[2]);
    EXPECT_EQ("MEETING_COMMAND(CREATE)", tail[3]);

    // B 组（对照）：没有 REGISTER 时，同一批次的 AUTHENTICATE 无法生效，
    //       同样的命令必然被拒 —— 即"顺序 + 前置状态"两者缺一不可。
    DomainSessionScenario without_register;
    std::vector<LogicWorkItem> contrast;
    contrast.push_back(MakeSessionAuthenticateWork(kHostSession, kHostUser));
    contrast.push_back(MakeExternalMessageWork(std::shared_ptr<LogicNode>()));
    contrast.push_back(MakeMeetingCommandWork(HostContext("m51-create"),
                                              ClientMeetingCommandKind::CREATE, ""));
    ASSERT_TRUE(without_register.PostBatchThenWait(contrast));
    ASSERT_EQ(1u, without_register.Sink().commands.size());
    EXPECT_EQ(meeting::ResultCode::AUTH_REQUIRED,
              without_register.Sink().LastCommand().result_code);
}

// ==============================================================================
// 3. 生命周期收敛：CONNECTED → AUTHENTICATED → CLOSED（含 tombstone 语义）
// ==============================================================================

TEST(DomainSessionLifecycle, ConnectedThenAuthenticatedThenClosedFollowsEnqueueOrder) {
    DomainSessionScenario scenario;
    ASSERT_TRUE(scenario.Register(kHostSession));
    ASSERT_TRUE(scenario.Authenticate(kHostSession, kHostUser));

    // 断线走真实 M5 桥接（不是伪造的 work item）。
    ASSERT_TRUE(scenario.BindBridge(kHostSession));
    EXPECT_EQ(SessionDisconnectDispatch::POSTED, scenario.Bridge().NotifyDisconnected());
    ASSERT_TRUE(scenario.Done());

    ASSERT_EQ(1u, scenario.Sink().disconnects.size());
    EXPECT_EQ(0u, scenario.Sink().disconnects[0].affected_meeting_count);
    EXPECT_EQ(1u, scenario.Sink().CountDispatchedKind(LogicWorkKind::SESSION_DISCONNECTED));

    // 迟到命令看到的是 CLOSED tombstone：SESSION_STATE_REJECTED，而不是 AUTH_REQUIRED。
    // 这正是"真实生命周期接线 + Domain tombstone + 单一 FIFO"三者接通的证据。
    ASSERT_TRUE(scenario.CommandThenWait(HostContext("m51-stale"),
                                         ClientMeetingCommandKind::CREATE, ""));
    EXPECT_EQ(meeting::ResultCode::SESSION_STATE_REJECTED,
              scenario.Sink().LastCommand().result_code);
}

TEST(DomainSessionLifecycle, DuplicateDisconnectAfterAuthenticateIsIdempotent) {
    DomainSessionScenario scenario;
    ASSERT_TRUE(scenario.Register(kHostSession));
    ASSERT_TRUE(scenario.Authenticate(kHostSession, kHostUser));
    ASSERT_EQ(kMeetingOne, scenario.CreateMeetingAs(HostContext("m51-create")));

    ASSERT_TRUE(scenario.BindBridge(kHostSession));
    EXPECT_EQ(SessionDisconnectDispatch::POSTED, scenario.Bridge().NotifyDisconnected());
    ASSERT_TRUE(scenario.Done());
    ASSERT_EQ(1u, scenario.Sink().disconnects.size());
    EXPECT_EQ(1u, scenario.Sink().disconnects[0].affected_meeting_count);
    EXPECT_EQ(0u, scenario.Sink().CountEventType(meeting::EventType::PARTICIPANT_LEFT));

    // 第一层去重：桥接本身对同一会话只投递一次，FIFO 里仍然只有一条断线 work。
    EXPECT_EQ(SessionDisconnectDispatch::ALREADY_POSTED, scenario.Bridge().NotifyDisconnected());
    EXPECT_EQ(1u, scenario.Sink().CountDispatchedKind(LogicWorkKind::SESSION_DISCONNECTED));

    // 第二层去重：即使重复的断线 work 真的进入 FIFO，M3 也只做幂等 no-op。
    ASSERT_TRUE(scenario.PostOneThenWait(MakeSessionDisconnectedWork(kHostSession)));
    ASSERT_EQ(2u, scenario.Sink().disconnects.size());
    EXPECT_EQ(0u, scenario.Sink().disconnects[1].affected_meeting_count);
    EXPECT_EQ(0u, scenario.Sink().disconnects[1].event_count);
    EXPECT_EQ(0u, scenario.Sink().CountEventType(meeting::EventType::PARTICIPANT_LEFT));
}

TEST(DomainSessionLifecycle, StaleAuthenticateAfterDisconnectIsRejected) {
    DomainSessionScenario scenario;
    ASSERT_TRUE(scenario.Register(kHostSession));
    ASSERT_TRUE(scenario.Authenticate(kHostSession, kHostUser));

    ASSERT_TRUE(scenario.BindBridge(kHostSession));
    EXPECT_EQ(SessionDisconnectDispatch::POSTED, scenario.Bridge().NotifyDisconnected());
    ASSERT_TRUE(scenario.Done());

    // CLOSED → AUTHENTICATED 非法：迟到认证必须失败，且不能复活该 Session。
    ASSERT_TRUE(scenario.CommandThenWait(HostContext("m51-create"),
                                         ClientMeetingCommandKind::CREATE, ""));
    EXPECT_EQ(meeting::ResultCode::SESSION_STATE_REJECTED,
              scenario.Sink().LastCommand().result_code);
    EXPECT_TRUE(scenario.Dispatcher().LastCreatedMeetingId().empty());
}

TEST(DomainSessionLifecycle, DisconnectBeforeAuthenticateClosesFromConnected) {
    DomainSessionScenario scenario;
    // 只注册、未认证就断线：CONNECTED → CLOSING → CLOSED 是合法转换。
    ASSERT_TRUE(scenario.Register(kHostSession));

    ASSERT_TRUE(scenario.BindBridge(kHostSession));
    EXPECT_EQ(SessionDisconnectDispatch::POSTED, scenario.Bridge().NotifyDisconnected());
    ASSERT_TRUE(scenario.Done());
    ASSERT_EQ(1u, scenario.Sink().disconnects.size());
    EXPECT_EQ(0u, scenario.Sink().disconnects[0].affected_meeting_count);

    // 之后补送的认证同样被 tombstone 拒绝。
    ASSERT_TRUE(scenario.CommandThenWait(HostContext("m51-create"),
                                         ClientMeetingCommandKind::CREATE, ""));
    EXPECT_EQ(meeting::ResultCode::SESSION_STATE_REJECTED,
              scenario.Sink().LastCommand().result_code);
}

// ==============================================================================
// 4. Meeting 清理语义（普通参与者 / Host）——直接断言 participant count
// ==============================================================================

TEST(DomainSessionLifecycle, LastSessionDisconnectMakesOrdinaryParticipantLeaveOnce) {
    DomainSessionScenario scenario;
    ASSERT_TRUE(scenario.Register(kHostSession));
    ASSERT_TRUE(scenario.Authenticate(kHostSession, kHostUser));
    ASSERT_EQ(kMeetingOne, scenario.CreateMeetingAs(HostContext("m51-create")));

    ASSERT_TRUE(scenario.Register(kUserASession));
    ASSERT_TRUE(scenario.Authenticate(kUserASession, kMultiUser));
    ASSERT_TRUE(scenario.CommandThenWait(UserAContext("m51-join"),
                                         ClientMeetingCommandKind::JOIN, kMeetingOne));
    ASSERT_EQ(2u, scenario.Sink().commands.size());   // CREATE + JOIN
    EXPECT_EQ(meeting::ResultCode::OK, scenario.Sink().LastCommand().result_code);
    EXPECT_EQ(2u, scenario.Sink().LastCommand().active_participant_count);   // N = 2

    // 该参与者的最后一个 Session 断线 → ACTIVE → LEFT，计数 N → N-1。
    ASSERT_TRUE(scenario.BindBridge(kUserASession));
    EXPECT_EQ(SessionDisconnectDispatch::POSTED, scenario.Bridge().NotifyDisconnected());
    ASSERT_TRUE(scenario.Done());
    ASSERT_EQ(1u, scenario.Sink().disconnects.size());
    EXPECT_EQ(1u, scenario.Sink().disconnects[0].affected_meeting_count);
    EXPECT_EQ(1u, scenario.Sink().CountEventType(meeting::EventType::PARTICIPANT_LEFT));

    ASSERT_TRUE(scenario.CommandThenWait(HostContext("m51-query"),
                                         ClientMeetingCommandKind::QUERY, kMeetingOne));
    EXPECT_EQ(meeting::ResultCode::OK, scenario.Sink().LastCommand().result_code);
    EXPECT_EQ(1u, scenario.Sink().LastCommand().active_participant_count);   // N - 1

    // Binding 已被清理：再断线一次不再命中任何 Meeting。
    ASSERT_TRUE(scenario.PostOneThenWait(MakeSessionDisconnectedWork(kUserASession)));
    ASSERT_EQ(2u, scenario.Sink().disconnects.size());
    EXPECT_EQ(0u, scenario.Sink().disconnects[1].affected_meeting_count);
    EXPECT_EQ(1u, scenario.Sink().CountEventType(meeting::EventType::PARTICIPANT_LEFT));
}

TEST(DomainSessionLifecycle, HostDisconnectKeepsHostActiveWithoutLeftEvent) {
    DomainSessionScenario scenario;
    ASSERT_TRUE(scenario.Register(kHostSession));
    ASSERT_TRUE(scenario.Authenticate(kHostSession, kHostUser));
    // Host 的第二条 Session：用于在第一条断开后仍能读回会议状态。
    ASSERT_TRUE(scenario.Register(kHostSecondSession));
    ASSERT_TRUE(scenario.Authenticate(kHostSecondSession, kHostUser));
    ASSERT_EQ(kMeetingOne, scenario.CreateMeetingAs(HostContext("m51-create")));
    ASSERT_EQ(1u, scenario.Sink().LastCommand().active_participant_count);

    ASSERT_TRUE(scenario.BindBridge(kHostSession));
    EXPECT_EQ(SessionDisconnectDispatch::POSTED, scenario.Bridge().NotifyDisconnected());
    ASSERT_TRUE(scenario.Done());

    ASSERT_EQ(1u, scenario.Sink().disconnects.size());
    EXPECT_EQ(1u, scenario.Sink().disconnects[0].affected_meeting_count);
    // Host 断线语义（I4）：Binding 被清理，但成员状态不变、不产生事件、会议不关闭。
    EXPECT_EQ(0u, scenario.Sink().CountEventType(meeting::EventType::PARTICIPANT_LEFT));
    EXPECT_TRUE(scenario.Dispatcher().HasActiveMeeting(kMeetingOne));

    ASSERT_TRUE(scenario.CommandThenWait(HostSecondContext("m51-query"),
                                         ClientMeetingCommandKind::QUERY, kMeetingOne));
    EXPECT_EQ(meeting::ResultCode::OK, scenario.Sink().LastCommand().result_code);
    EXPECT_EQ(1u, scenario.Sink().LastCommand().active_participant_count);   // 计数不变
}

TEST(DomainSessionLifecycle, MultiSessionParticipantStaysActiveUntilLastSessionDisconnects) {
    DomainSessionScenario scenario;
    ASSERT_TRUE(scenario.Register(kHostSession));
    ASSERT_TRUE(scenario.Authenticate(kHostSession, kHostUser));
    ASSERT_EQ(kMeetingOne, scenario.CreateMeetingAs(HostContext("m51-create")));

    // 同一个 user 100 的两条 Session 都加入同一会议。
    ASSERT_TRUE(scenario.Register(kUserASession));
    ASSERT_TRUE(scenario.Authenticate(kUserASession, kMultiUser));
    ASSERT_TRUE(scenario.CommandThenWait(UserAContext("m51-join-a"),
                                         ClientMeetingCommandKind::JOIN, kMeetingOne));
    ASSERT_EQ(2u, scenario.Sink().LastCommand().active_participant_count);

    ASSERT_TRUE(scenario.Register(kUserBSession));
    ASSERT_TRUE(scenario.Authenticate(kUserBSession, kMultiUser));
    ASSERT_TRUE(scenario.CommandThenWait(UserBContext("m51-join-b"),
                                         ClientMeetingCommandKind::JOIN, kMeetingOne));
    // 第二条 Session 只是 ADDITIONAL_SESSION_BOUND：参与者计数不应增加。
    EXPECT_EQ(meeting::ResultCode::OK, scenario.Sink().LastCommand().result_code);
    EXPECT_EQ(2u, scenario.Sink().LastCommand().active_participant_count);

    // 断开第一条 Session：user 100 仍然 ACTIVE（还有第二条 Binding），计数不变。
    ASSERT_TRUE(scenario.BindBridge(kUserASession));
    EXPECT_EQ(SessionDisconnectDispatch::POSTED, scenario.Bridge().NotifyDisconnected());
    ASSERT_TRUE(scenario.Done());
    ASSERT_EQ(1u, scenario.Sink().disconnects.size());
    EXPECT_EQ(1u, scenario.Sink().disconnects[0].affected_meeting_count);
    EXPECT_EQ(0u, scenario.Sink().CountEventType(meeting::EventType::PARTICIPANT_LEFT));

    ASSERT_TRUE(scenario.CommandThenWait(HostContext("m51-query-1"),
                                         ClientMeetingCommandKind::QUERY, kMeetingOne));
    EXPECT_EQ(2u, scenario.Sink().LastCommand().active_participant_count);   // 不变

    // 断开最后一条 Session：这才 ACTIVE → LEFT，且只产生一个事件，计数 -1。
    ASSERT_TRUE(scenario.BindBridge(kUserBSession));
    EXPECT_EQ(SessionDisconnectDispatch::POSTED, scenario.Bridge().NotifyDisconnected());
    ASSERT_TRUE(scenario.Done());
    ASSERT_EQ(2u, scenario.Sink().disconnects.size());
    EXPECT_EQ(1u, scenario.Sink().disconnects[1].affected_meeting_count);
    EXPECT_EQ(1u, scenario.Sink().CountEventType(meeting::EventType::PARTICIPANT_LEFT));

    ASSERT_TRUE(scenario.CommandThenWait(HostContext("m51-query-2"),
                                         ClientMeetingCommandKind::QUERY, kMeetingOne));
    EXPECT_EQ(1u, scenario.Sink().LastCommand().active_participant_count);   // 2 - 1
}

// ==============================================================================
// 5. Shutdown 边界：停止之后的生命周期 work 一律被拒绝，且不改变领域状态
// ==============================================================================

TEST(DomainSessionLifecycleShutdown, RegisterAndAuthenticateWorkRejectedAfterStopDoNotReachDomain) {
    DomainSessionScenario scenario;
    ASSERT_TRUE(scenario.Register(kHostSession));
    ASSERT_TRUE(scenario.Authenticate(kHostSession, kHostUser));
    const std::size_t commands_before = scenario.Sink().commands.size();

    scenario.StopAndJoin();

    // M4 §11.2：stop 之后到达的外部 work 被队列拒绝（返回 false），不排队、不执行。
    // 生产侧由此在日志中如实记录 "…rejected because serialized executor is stopping"，
    // 并且不改变 legacy 登录协议语义、不重试、也不建立第二条异步路径。
    EXPECT_FALSE(scenario.Executor().PostSessionRegister("s-late"));
    EXPECT_FALSE(scenario.Executor().PostSessionAuthenticate("s-late", kHostUser));
    EXPECT_FALSE(scenario.Executor().PostMeetingCommand(HostContext("m51-late-create"),
                                                       ClientMeetingCommandKind::CREATE, ""));

    // 领域侧完全没有变化：没有任何新的命令结果、断线结果或事件。
    EXPECT_EQ(commands_before, scenario.Sink().commands.size());
    EXPECT_TRUE(scenario.Sink().disconnects.empty());
    EXPECT_TRUE(scenario.Sink().event_types.empty());
    EXPECT_TRUE(scenario.Dispatcher().LastCreatedMeetingId().empty());
}
