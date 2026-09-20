#pragma once

// ==============================================================================
// M2 单个 Meeting 聚合。
//
// 实现"一个 Meeting 内部"的一致性与状态机：Meeting 状态、Host 身份、Participant
// 集合、Participant ↔ Session 绑定集合、历史成员身份、以及 Close 所需的冻结材料。
//
// 明确不负责（属于 M3/M4 及以后）：
//   MeetingService、MeetingRegistry、ClosedSnapshotRegistry、
//   session→meetings 反向索引、SessionDisconnected fan-out、
//   FinalizeClose work item 调度、LogicSystem 队列、任何网络/存储/协议。
//
// 依赖边界：仅 C++ 标准库 + M1 的 MeetingTypes.h。不链接 gRPC、protobuf、
// hiredis、MySQL、JsonCpp、Boost，也不依赖 Threads。
//
// 线程模型：MeetingAggregate 不是 thread-safe object，内部不加任何锁。
// mutation 必须由调用方通过 Phase 1 冻结的 single-writer serialized boundary
// 串行调用。这不是缺陷，而是架构边界。
//
// 确定性：聚合内部不产生时间戳、UUID 或全局序列号；MeetingId / EventId /
// Timestamp / RequestId 全部由调用方显式传入，以保证测试可重复。
// ==============================================================================

#include "meeting/MeetingTypes.h"

#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace meeting {

// ------------------------------------------------------------------------------
// Create 的输入。
//
// 聚合不生成 MeetingId / EventId：创建所需的标识与时间全部由调用方提供。
// ------------------------------------------------------------------------------
struct CreateMeetingParams {
    MeetingId meeting_id;
    UserId host_user_id = 0;
    SessionId host_session_id;
    ServerId owner_chat_server_id; // 取自 handling_chat_server_id
    Timestamp created_at;
    EventId event_id;              // 用于 MeetingCreated
    RequestId request_id;          // correlation

    bool has_host_device_id = false;
    DeviceId host_device_id;
};

// ------------------------------------------------------------------------------
// 单次 mutation 的公共输入：调用者显式提供时间与（需要事件时的）EventId。
//
// 如果某次操作最终不产生事件，传入的 event_id 不得造成任何副作用。
// ------------------------------------------------------------------------------
struct MutationParams {
    Timestamp now;
    EventId event_id;

    bool has_device_id = false;
    DeviceId device_id;
};

// ------------------------------------------------------------------------------
// CloseContext：BeginClose 与后续 FinalizeClose 之间的冻结交接材料。
//
// 它是**值对象**：只保存稳定 ID/value，不保存 MeetingAggregate*、
// Participant*、Binding* 或 CSession*。M2 只负责冻结它，不负责执行
// FinalizeClose（那是 M3 的 two-turn 流程）。
// ------------------------------------------------------------------------------
struct CloseContext {
    MeetingId meeting_id;
    UserId host_user_id = 0;
    ServerId owner_chat_server_id;
    RequestId originating_request_id; // 与最初的 Close command 建立 correlation
    Timestamp close_started_at;

    // 关闭开始时（BEGIN_CLOSE 边界）的历史成员身份完整快照，含已 LEFT 的历史成员。
    std::vector<ParticipantIdentitySnapshot> participant_identities;

    // 关闭开始时仍处于 BOUND 的 session_id 集合（可通知目标）。
    std::vector<SessionId> notifiable_session_ids;

    // 关闭边界上的"活跃成员数"，即 ClosedMeetingSnapshot.final_participant_count。
    std::size_t frozen_active_participant_count = 0;
};

// ------------------------------------------------------------------------------
// 操作结果。
//
// 组合 M1 已冻结的 ResultCode / ResultDisposition / Outcome 与视图，不另建
// 一套错误枚举。可选 output 继续使用 M1 的 presence flag 风格，不引入 optional。
// ------------------------------------------------------------------------------
struct JoinResult {
    ResultCode result_code = ResultCode::INTERNAL_ERROR;
    ResultDisposition disposition = ResultDisposition::ERROR;
    JoinOutcome outcome = JoinOutcome::NEW_PARTICIPANT;

    MeetingView meeting;
    ParticipantView participant;

    bool has_event = false;
    MeetingEvent event;
};

struct LeaveResult {
    ResultCode result_code = ResultCode::INTERNAL_ERROR;
    ResultDisposition disposition = ResultDisposition::ERROR;
    LeaveOutcome outcome = LeaveOutcome::LEFT;

    MeetingView meeting;

    bool has_event = false;
    MeetingEvent event;
};

struct BeginCloseResult {
    ResultCode result_code = ResultCode::INTERNAL_ERROR;
    ResultDisposition disposition = ResultDisposition::ERROR;
    CloseOutcome outcome = CloseOutcome::CLOSE_STARTED;

    MeetingView meeting;

    // BEGIN_CLOSE 的可选输出：是否应当安排 FinalizeClose。
    // - 首次合法 BeginClose 为 true；
    // - CLOSE_IN_PROGRESS / PERMISSION_DENIED / MEETING_STATE_REJECTED 为 false。
    bool should_schedule_finalize = false;

    // 仅在首次合法 BeginClose 时为 true。值为 frozen context 的副本，
    // 不持有指向聚合内部的指针。
    bool has_close_context = false;
    CloseContext close_context;
};

// ------------------------------------------------------------------------------
// MeetingAggregate
// ------------------------------------------------------------------------------
class MeetingAggregate {
public:
    // 构造初始聚合：CREATED + Host(ACTIVE) + Host 初始 Binding(BOUND) + count=1，
    // 并产生恰好一次 MeetingCreated（**不**产生 ParticipantJoined）。
    MeetingAggregate(const CreateMeetingParams& params);

    // 只读投影。
    MeetingId Id() const { return meeting_id_; }
    MeetingState State() const { return meeting_state_; }
    UserId HostUserId() const { return host_user_id_; }
    ServerId OwnerChatServerId() const { return owner_chat_server_id_; }
    Timestamp CreatedAt() const { return created_at_; }
    std::size_t ActiveParticipantCount() const { return active_participant_count_; }

    MeetingView BuildMeetingView() const;

    // 返回指定 User 的 ParticipantView。该 User 从未成为本 Meeting 成员时返回
    // false（此时 out 不被视为有效）。
    bool TryGetParticipantView(UserId user_id, ParticipantView& out) const;

    // 按首次加入顺序返回全部已知 Participant 的视图（含已 LEFT 的历史成员）。
    std::vector<ParticipantView> ListParticipantViews() const;

    // domain query（供 M3 判断 Binding，而不是测试后门）。
    bool IsSessionBound(UserId user_id, const SessionId& session_id) const;
    std::size_t BoundSessionCount(UserId user_id) const;

    // ---------------------------------------------------------------------------
    // mutation（调用方保证串行）
    // ---------------------------------------------------------------------------

    // Join / Re-Join / 附加 Session / 重复 Join。四种 outcome 见 M2 文档 §6。
    JoinResult Join(UserId user_id, const SessionId& session_id,
                    const MutationParams& params);

    // 逻辑离会（User 级：解除该 User 在本 Meeting 下全部 Binding）。
    LeaveResult Leave(UserId caller_user_id, const MutationParams& params);

    // Host 发起关闭：CREATED/ACTIVE → ENDING，并冻结 CloseContext。
    BeginCloseResult BeginClose(UserId caller_user_id, const RequestId& request_id,
                                const MutationParams& params);

    // 基于冻结尾边界生成 ClosedMeetingSnapshot 所需材料。
    //
    // closed_at 由调用方提供（M2 不让聚合自己取时间）。本方法不发布 snapshot、
    // 不改变本聚合状态：registry 的原子发布是 M3 MeetingService 的职责。
    ClosedMeetingSnapshot BuildClosedSnapshot(const CloseContext& context,
                                              const Timestamp& closed_at) const;

    // CloseContext 的只读访问（仅供需要检查冻结材料的调用方/M3 使用）。
    bool HasCloseContext() const { return has_close_context_; }
    const CloseContext& FrozenCloseContext() const { return close_context_; }

private:
    struct BindingRecord {
        SessionId session_id;
        BindingState binding_state = BindingState::BOUND;
        Timestamp bound_at;
        bool has_device_id = false;
        DeviceId device_id;
    };

    struct ParticipantRecord {
        UserId user_id = 0;
        ParticipantRole role = ParticipantRole::PARTICIPANT;
        ParticipantState participant_state = ParticipantState::ACTIVE;
        Timestamp joined_at;
        bool has_left_at = false;
        Timestamp left_at;
        std::vector<BindingRecord> bindings;
    };

    // 返回 nullptr 表示该 User 从未成为本 Meeting 成员。
    ParticipantRecord* FindParticipant(UserId user_id);
    const ParticipantRecord* FindParticipant(UserId user_id) const;

    // 在 ParticipantRecord::bindings 中查找该 session 的 BOUND 记录。
    static BindingRecord* FindBoundBinding(ParticipantRecord& participant,
                                           const SessionId& session_id);
    static const BindingRecord* FindBoundBinding(const ParticipantRecord& participant,
                                                 const SessionId& session_id);

    // 为给定 User 绑定 session（已存在 BOUND 记录时复用并刷新 device 信息）。
    static void BindSession(ParticipantRecord& participant,
                            const SessionId& session_id, const Timestamp& now,
                            bool has_device_id, const DeviceId& device_id);

    static ParticipantView MakeParticipantView(const ParticipantRecord& participant);
    static ParticipantIdentitySnapshot MakeIdentitySnapshot(
        const ParticipantRecord& participant);

    // dedupe_context 由各调用点按"逻辑动作 + meeting_id + user_id"构造，
    // 与 Phase 1 对最小去重上下文的定义一致；它不是幂等键。
    MeetingEvent MakeEvent(EventType type, const EventId& event_id,
                           const Timestamp& occurred_at, bool has_actor,
                           UserId actor_user_id, bool has_participant,
                           UserId participant_user_id,
                           const std::string& dedupe_context) const;

    MeetingId meeting_id_;
    UserId host_user_id_ = 0;
    ServerId owner_chat_server_id_;
    MeetingState meeting_state_ = MeetingState::CREATED;
    Timestamp created_at_;
    std::size_t active_participant_count_ = 0;

    std::map<UserId, ParticipantRecord> participants_;
    std::vector<UserId> participant_order_; // 首次加入顺序，保证投影可重复

    // 首个非 Host 激活是否已经发生过。用于保证 Meeting 一旦离开 CREATED
    // 就永不回退（即使后续所有非 Host 成员全部离会）。
    bool has_activated_non_host_ = false;

    // BeginClose 冻结材料。只在首次合法 BeginClose 时填充一次，保证
    // "每个 Meeting 最多冻结一个 CloseContext、最多安排一个 FinalizeClose"。
    bool has_close_context_ = false;
    CloseContext close_context_;
};

} // namespace meeting
