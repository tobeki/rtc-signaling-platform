#include "LogicWorkDispatcher.h"

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>

// M6.5：dispatcher 只认识抽象接口，但调用虚函数需要完整类型，因此 .cpp 引入该头。
// 依赖边界不变：MeetingResponseNotificationAdapter.h 只依赖标准库 + 纯领域头文件，
// 不引入 asio / Redis / MySQL / gRPC / protobuf（§5.3 / §16.5）。
#include "MeetingResponseNotificationAdapter.h"

// ==============================================================================
// M4 生产 Meeting 分发器实现。
//
// 本编译单元只依赖：MeetingDomain（M1–M3）+ 标准库 + Boost.UUID（ID 生成）。
// 不依赖 LogicSystem / CSession / Redis / MySQL / gRPC / asio，
// 因此它可以被单元测试以完全确定性方式驱动。
//
// 时间来源故意使用 std::chrono::system_clock::now()：它出现在**集成层**，
// 而不是出现在 MeetingService 内部（M3 §4 明确禁止领域层直接取时间）。
//
// M6.5 追加：六个 Response 调用点 + 一个 FINALIZE_CLOSE 调用点（§6.5）。
//   顺序冻结为：领域 mutation → 既有 sink 观测 → adapter 调用 → 正常返回；
//   DispatchClose 额外保留"adapter 之后才判断 should_schedule_finalize 并入队"。
//   adapter 为 nullptr 时，代码路径与 M4 完全一致（调用点整体跳过）。
// ==============================================================================

namespace {

// boost::uuids 在这里被使用是刻意的：M4 的职责就是为领域层提供真实 ID 来源。
// 领域层本身仍然只通过 std::function 注入使用它们。
std::string MakeUuidText(const char* prefix) {
    static boost::uuids::random_generator generator;
    const boost::uuids::uuid value = generator();
    return std::string(prefix) + boost::uuids::to_string(value);
}

// ------------------------------------------------------------------------------
// M6.5 / §13.4：adapter 调用的**最小**异常隔离。
//
// dispatcher 持有的是抽象接口指针：M6.4 的生产实现是 noexcept，但**接口本身不是**，
// 因此任何实现都可能违约抛异常。而 SerializedWorkExecutor 的 WorkerLoop 没有
// 通用 try/catch —— 异常一旦从 DispatchWork 逃逸，就会从 std::thread 的入口函数
// 逃出 ⇒ std::terminate（整进程终止），且 in_flight_ 不再归零（等待方永久等不到
// 静止）。所以守卫必须加在**调用边界**上，而不是指望实现自觉。
//
// 守卫只做三件事：调用、吞异常、计一次数。
//   · 不回写领域状态（领域结果已经落地，且是 const 引用）；
//   · 不影响 sink 观测（sink 已在调用点之前完成）；
//   · 不阻止 Close 派生的 FINALIZE_CLOSE 入队（入队语句在调用点之后）；
//   · 不做任何 I/O、不起线程、不建第二套监控。
// ------------------------------------------------------------------------------
template <typename Callback>
void InvokeAdapterGuarded(std::atomic<std::size_t>& failure_count, Callback callback) {
    try {
        callback();
    } catch (...) {
        failure_count.fetch_add(1);
    }
}

} // namespace

// ------------------------------------------------------------------------------
// LogicWorkQueue
// ------------------------------------------------------------------------------

bool LogicWorkQueue::Push(const LogicWorkItem& item) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) {
            return false;
        }
        items_.push_back(item);
    }
    // 在锁外唤醒，避免被唤醒者立刻再次阻塞在同一个 mutex 上。
    condition_.notify_one();
    return true;
}

bool LogicWorkQueue::PushLocked(const LogicWorkItem& item) {
    // 调用方持有 Lock，因此这里不再加锁。
    if (stopping_) {
        return false;
    }
    items_.push_back(item);
    return true;
}

void LogicWorkQueue::NotifyWorkAvailable() {
    condition_.notify_one();
}

bool LogicWorkQueue::PushInternalDuringDrainLocked(const LogicWorkItem& item) {
    // 调用方（消费者线程）已经持有队列锁，因此这里**绝不再加锁**。
    //
    // stop 之后仍然接受：这是唯一例外，用于补完"已受理工作的必要后续"
    //（Close 派生出的 FINALIZE_CLOSE）。若不接受，Meeting 会永久停在 ENDING。
    // 外部 work 依旧只能走会拒绝的 Push()。
    items_.push_back(item);
    return true;
}

bool LogicWorkQueue::Pop(LogicWorkItem& out_item) {
    // 调用方（消费者）已经持有 Lock。
    if (items_.empty()) {
        return false;
    }
    out_item = items_.front();
    items_.pop_front();
    return true;
}

bool LogicWorkQueue::WaitForWorkOrStopLocked(Lock& lock) {
    // 谓词与等待都在调用方已持有的队列锁之下，因此"检查条件"与"进入等待"之间
    // 不存在丢失唤醒的窗口。
    condition_.wait(lock.Raw(), [this]() { return stopping_ || !items_.empty(); });
    return stopping_ || !items_.empty();
}

bool LogicWorkQueue::IsEmptyLocked() const {
    // 调用方持有 Lock，因此这里不再加锁。
    return items_.empty();
}

bool LogicWorkQueue::IsStopRequestedLocked() const {
    // 调用方持有 Lock，因此这里不再加锁。
    return stopping_;
}

bool LogicWorkQueue::IsEmpty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return items_.empty();
}

std::size_t LogicWorkQueue::Size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return items_.size();
}

void LogicWorkQueue::RequestStop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    condition_.notify_all();
}

bool LogicWorkQueue::IsStopRequested() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stopping_;
}

// ------------------------------------------------------------------------------
// LogicWorkDispatcher
// ------------------------------------------------------------------------------

meeting::MeetingServiceDependencies MakeSystemMeetingWorkDependencies() {
    meeting::MeetingServiceDependencies dependencies;
    dependencies.next_meeting_id = []() { return MakeUuidText("meeting-"); };
    dependencies.next_event_id = []() { return MakeUuidText("event-"); };
    dependencies.now = []() { return std::chrono::system_clock::now(); };
    return dependencies;
}

LogicWorkDispatcher::LogicWorkDispatcher(const meeting::ServerId& local_server_id,
                                         meeting::MeetingServiceDependencies dependencies,
                                         IWorkObservationSink* sink,
                                         IMeetingResponseNotificationAdapter* adapter)
    : meeting_service_(local_server_id, dependencies),
      sink_(sink),
      adapter_(adapter),
      adapter_exception_count_(0) {
    // 生产传 nullptr => 该观测路径完全没有开销，也不会向任何 sink 写入。
    // M6.5：adapter 同样允许为 nullptr（M6.6 之前的生产装配就是这个形态），
    // 此时七个调用点整体跳过，行为与 M4 完全一致。
}

void LogicWorkDispatcher::DispatchWork(const LogicWorkItem& item) {
    switch (item.kind) {
    case LogicWorkKind::MEETING_COMMAND:
        switch (item.meeting_command) {
        case ClientMeetingCommandKind::CREATE:            DispatchCreate(item); break;
        case ClientMeetingCommandKind::JOIN:              DispatchJoin(item); break;
        case ClientMeetingCommandKind::LEAVE:             DispatchLeave(item); break;
        case ClientMeetingCommandKind::CLOSE:             DispatchClose(item); break;
        case ClientMeetingCommandKind::QUERY:             DispatchQuery(item); break;
        case ClientMeetingCommandKind::LIST_PARTICIPANTS: DispatchListParticipants(item); break;
        }
        return;

    case LogicWorkKind::SESSION_DISCONNECTED:
        SessionDisconnected(item.session_id);
        return;

    case LogicWorkKind::FINALIZE_CLOSE: {
        const meeting::FinalizeCloseResult result = meeting_service_.FinalizeClose(item.meeting_id);
        if (sink_ != nullptr) {
            sink_->RecordWorkDispatched(item, 0, ToWorkDomainEvents(result.events));
            sink_->RecordFinalizeClose(item.meeting_id, result.finalized,
                                       result.already_finalized, result.notification_session_ids);
        }
        // M6.5 / §6.5 第七个调用点：本 turn 的通知产出。
        // 顺序：领域收敛（已发布 CLOSED 快照、MeetingClosed 恰好一次）→ sink 观测
        // → adapter 逐目标通知（§7.1，best-effort）。
        // 这里**不**新增第二次 FinalizeClose，也**不**把通知结果重新投递为 work item：
        // 通知是本次已受理 work 的同步下游产物（§7.4）。
        if (adapter_ != nullptr) {
            InvokeAdapterGuarded(adapter_exception_count_, [this, &result]() {
                adapter_->OnFinalizeClose(result);
            });
        }
        return;
    }

    case LogicWorkKind::INTERNAL_SESSION_REGISTER:
        RegisterConnectedSession(item.session_id);
        return;

    case LogicWorkKind::INTERNAL_SESSION_AUTHENTICATE:
        AuthenticateSession(item.session_id, item.user_id);
        return;

    case LogicWorkKind::EXTERNAL_MESSAGE:
        // legacy 消息由 SerializedWorkExecutor 直接交给 ILegacyMessageRouter，
        // 不会走到这里。这里显式忽略，避免"静默吞掉"。
        return;
    }
}

void LogicWorkDispatcher::RegisterConnectedSession(const meeting::SessionId& session_id) {
    meeting_service_.RegisterConnectedSession(session_id);
}

void LogicWorkDispatcher::AuthenticateSession(const meeting::SessionId& session_id,
                                              meeting::UserId user_id) {
    meeting_service_.AuthenticateSession(session_id, user_id);
}

void LogicWorkDispatcher::SessionDisconnected(const meeting::SessionId& session_id) {
    const meeting::SessionDisconnectedResult result = meeting_service_.SessionDisconnected(session_id);
    if (sink_ != nullptr) {
        LogicWorkItem item = MakeSessionDisconnectedWork(session_id);
        sink_->RecordWorkDispatched(item, 0, ToWorkDomainEvents(result.events));
        sink_->RecordSessionDisconnected(session_id, result.affected_meeting_count);
    }
}

void LogicWorkDispatcher::DispatchCreate(const LogicWorkItem& item) {
    const meeting::CreateMeetingResult result = meeting_service_.CreateMeeting(item.meeting_context);
    if (result.common.result_code == meeting::ResultCode::OK) {
        last_created_meeting_id_ = result.meeting.meeting_id;
    }
    if (sink_ != nullptr) {
        sink_->RecordWorkDispatched(item, 0, ToWorkDomainEvents(result.events));
        MeetingCommandOutcome outcome =
            MakeCommandOutcome(item.meeting_command, result.meeting.meeting_id, result.common);
        outcome.meeting_state = result.meeting.meeting_state;
        outcome.has_closed_at = result.meeting.has_closed_at;
        outcome.active_participant_count = result.meeting.active_participant_count;
        FillEventTypes(result.events, outcome);
        sink_->RecordMeetingCommand(outcome);
    }

    // M6.5 / §6.5 调用点 1/6：CREATE 的 response。
    // 顺序冻结：领域 mutation → 既有 sink 观测 → adapter → 返回。
    if (adapter_ != nullptr) {
        InvokeAdapterGuarded(adapter_exception_count_, [this, &item, &result]() {
            adapter_->OnCreateMeeting(item.meeting_context, result);
        });
    }
}

void LogicWorkDispatcher::DispatchJoin(const LogicWorkItem& item) {
    const meeting::JoinMeetingResult result =
        meeting_service_.JoinMeeting(item.meeting_context, item.meeting_id);
    if (sink_ != nullptr) {
        sink_->RecordWorkDispatched(item, 0, ToWorkDomainEvents(result.events));
        MeetingCommandOutcome outcome =
            MakeCommandOutcome(item.meeting_command, item.meeting_id, result.common);
        outcome.meeting_state = result.meeting.meeting_state;
        outcome.has_closed_at = result.meeting.has_closed_at;
        outcome.active_participant_count = result.meeting.active_participant_count;
        FillEventTypes(result.events, outcome);
        sink_->RecordMeetingCommand(outcome);
    }

    // M6.5 / §6.5 调用点 2/6：JOIN 的 response。
    if (adapter_ != nullptr) {
        InvokeAdapterGuarded(adapter_exception_count_, [this, &item, &result]() {
            adapter_->OnJoinMeeting(item.meeting_context, result);
        });
    }
}

void LogicWorkDispatcher::DispatchLeave(const LogicWorkItem& item) {
    const meeting::LeaveMeetingResult result =
        meeting_service_.LeaveMeeting(item.meeting_context, item.meeting_id);
    if (sink_ != nullptr) {
        sink_->RecordWorkDispatched(item, 0, ToWorkDomainEvents(result.events));
        MeetingCommandOutcome outcome =
            MakeCommandOutcome(item.meeting_command, item.meeting_id, result.common);
        outcome.meeting_state = result.meeting.meeting_state;
        outcome.has_closed_at = result.meeting.has_closed_at;
        outcome.active_participant_count = result.meeting.active_participant_count;
        FillEventTypes(result.events, outcome);
        sink_->RecordMeetingCommand(outcome);
    }

    // M6.5 / §6.5 调用点 3/6：LEAVE 的 response。
    if (adapter_ != nullptr) {
        InvokeAdapterGuarded(adapter_exception_count_, [this, &item, &result]() {
            adapter_->OnLeaveMeeting(item.meeting_context, result);
        });
    }
}

void LogicWorkDispatcher::DispatchClose(const LogicWorkItem& item) {
    const meeting::CloseMeetingResult result =
        meeting_service_.CloseMeeting(item.meeting_context, item.meeting_id);

    // 观测顺序必须与真实分发顺序一致：本 turn 的 Close 结果**先**被记录，
    // 然后才把派生的 FINALIZE_CLOSE 追加到同一 FIFO 的尾部。否则观测点会看到
    // "Finalize 出现在 Close 自己之前"的假象（真实队列顺序其实没有问题）。
    if (sink_ != nullptr) {
        sink_->RecordWorkDispatched(item, 0, ToWorkDomainEvents(result.events));
        MeetingCommandOutcome outcome =
            MakeCommandOutcome(item.meeting_command, item.meeting_id, result.common);
        outcome.should_schedule_finalize = result.should_schedule_finalize;
        outcome.meeting_state = result.meeting.meeting_state;
        outcome.has_closed_at = result.meeting.has_closed_at;
        outcome.active_participant_count = result.meeting.active_participant_count;
        FillEventTypes(result.events, outcome);
        sink_->RecordMeetingCommand(outcome);
    }

    // M6.5 / §6.5 调用点 4/6：CLOSE（turn 1）的 response。
    //
    // 位置是本函数最敏感的一处，必须夹在"领域结果 + sink 观测"之后、
    // "should_schedule_finalize 判断与入队"之前：
    //   · adapter 出现在领域 mutation 之前 ⇒ 可能报告未落地的结果（禁止）；
    //   · adapter 决定/取代 should_schedule_finalize ⇒ 出站失败会改变 Close 的
    //     两 turn 收敛（禁止，§13.5）；
    //   · 因此 adapter 既不参与入队判断，也不被入队结果影响。
    if (adapter_ != nullptr) {
        InvokeAdapterGuarded(adapter_exception_count_, [this, &item, &result]() {
            adapter_->OnCloseMeeting(item.meeting_context, result);
        });
    }

    // 两个独立 work item（Step 1.4 §16.5、Step 1.5 §13）：
    // 本 turn 只完成 BeginClose。只有首次合法 BeginClose 才向**同一** FIFO 的
    // **尾部**追加恰好一个 FINALIZE_CLOSE。
    //
    // 这里绝不调用 FinalizeClose：那会把两个 turn 压成一个，使 first Close 直接
    // 返回 CLOSED，破坏冻结的响应语义（R5）。
    //
    // 入队发生在队列锁之外（本函数由 SerializedWorkExecutor 在锁外调用），
    // 因此向当前 FIFO 再入队不会死锁。
    if (result.should_schedule_finalize) {
        PostInternalWork(MakeFinalizeCloseWork(item.meeting_id));
    }
}

void LogicWorkDispatcher::DispatchQuery(const LogicWorkItem& item) {
    const meeting::QueryMeetingResult result =
        meeting_service_.QueryMeeting(item.meeting_context, item.meeting_id);
    if (sink_ != nullptr) {
        sink_->RecordWorkDispatched(item, 0, std::vector<WorkDomainEvent>());
        MeetingCommandOutcome outcome =
            MakeCommandOutcome(item.meeting_command, item.meeting_id, result.common);
        outcome.meeting_state = result.meeting.meeting_state;
        outcome.has_closed_at = result.meeting.has_closed_at;
        outcome.active_participant_count = result.meeting.active_participant_count;
        sink_->RecordMeetingCommand(outcome);
    }

    // M6.5 / §6.5 调用点 5/6：QUERY 的 response。
    if (adapter_ != nullptr) {
        InvokeAdapterGuarded(adapter_exception_count_, [this, &item, &result]() {
            adapter_->OnQueryMeeting(item.meeting_context, result);
        });
    }
}

void LogicWorkDispatcher::DispatchListParticipants(const LogicWorkItem& item) {
    const meeting::ListParticipantsResult result =
        meeting_service_.ListParticipants(item.meeting_context, item.meeting_id);
    if (sink_ != nullptr) {
        sink_->RecordWorkDispatched(item, 0, std::vector<WorkDomainEvent>());
        MeetingCommandOutcome outcome =
            MakeCommandOutcome(item.meeting_command, item.meeting_id, result.common);
        outcome.meeting_state = result.meeting.meeting_state;
        outcome.has_closed_at = result.meeting.has_closed_at;
        outcome.active_participant_count = result.meeting.active_participant_count;
        sink_->RecordMeetingCommand(outcome);
    }

    // M6.5 / §6.5 调用点 6/6：LIST_PARTICIPANTS 的 response（含 participants / scope）。
    if (adapter_ != nullptr) {
        InvokeAdapterGuarded(adapter_exception_count_, [this, &item, &result]() {
            adapter_->OnListParticipants(item.meeting_context, result);
        });
    }
}

MeetingCommandOutcome LogicWorkDispatcher::MakeCommandOutcome(
    ClientMeetingCommandKind command,
    const meeting::MeetingId& meeting_id,
    const meeting::MeetingServiceResultCommon& common) {
    MeetingCommandOutcome outcome;
    outcome.command = command;
    outcome.result_code = common.result_code;
    outcome.result_name = common.result_name;
    outcome.disposition = common.disposition;
    outcome.has_meeting_id = !meeting_id.empty();
    outcome.meeting_id = meeting_id;
    return outcome;
}

void LogicWorkDispatcher::FillEventTypes(const std::vector<meeting::MeetingEvent>& events,
                                         MeetingCommandOutcome& outcome) {
    outcome.event_types.clear();
    outcome.event_types.reserve(events.size());
    for (std::size_t index = 0; index < events.size(); ++index) {
        outcome.event_types.push_back(events[index].event_type);
    }
}

std::vector<WorkDomainEvent> LogicWorkDispatcher::ToWorkDomainEvents(
    const std::vector<meeting::MeetingEvent>& events) {
    std::vector<WorkDomainEvent> converted;
    converted.reserve(events.size());
    for (std::size_t index = 0; index < events.size(); ++index) {
        WorkDomainEvent value;
        value.type = events[index].event_type;
        value.meeting_id = events[index].meeting_id;
        value.event_id = events[index].event_id;
        value.meeting_state_after = events[index].meeting_state_after;
        converted.push_back(value);
    }
    return converted;
}
