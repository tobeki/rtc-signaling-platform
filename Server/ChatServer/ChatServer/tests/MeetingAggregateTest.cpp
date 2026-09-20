// ==============================================================================
// M2 / M2.1 MeetingAggregate 单元测试（Phase 1 Layer A：Pure Domain）。
//
// 覆盖聚合当前负责的核心分支：Create / Join 四种 outcome / Leave /
// BeginClose / CloseContext / Snapshot material / CURRENT participant projection。
//
// M2.1 增量：
//   - Create 通过 Create() factory 直接断言恰好一个 MeetingCreated；
//   - 所有事件的 meeting_state_after 与 request_id correlation 被显式断言；
//   - ListCurrentParticipantViews() 只列 ACTIVE 成员，历史身份仍可通过
//     TryGetParticipantView() 定位。
//
// 全部测试使用显式固定的 Timestamp、EventId、RequestId，不依赖真实时钟与
// 随机数，因此可重复、可比较。
//
// 刻意不测试的（属于 M3/M4）：
//   - MeetingService、registry、ClosedSnapshotRegistry；
//   - SessionDisconnected fan-out；
//   - FinalizeClose 的 work item 调度与 ENDING → CLOSED 发布；
//   - CLOSED 状态下的 Join/Leave（聚合本身不进入 CLOSED）。
//   Phase 1 规划的 57 个场景不在此一次实现。
// ==============================================================================

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <string>
#include <vector>

#include "meeting/MeetingAggregate.h"

namespace {

using meeting::BeginCloseResult;
using meeting::CloseContext;
using meeting::ClosedMeetingSnapshot;
using meeting::CreateMeetingParams;
using meeting::EventType;
using meeting::JoinOutcome;
using meeting::JoinResult;
using meeting::LeaveOutcome;
using meeting::LeaveResult;
using meeting::MeetingAggregate;
using meeting::MeetingEvent;
using meeting::MeetingState;
using meeting::MeetingView;
using meeting::MutationParams;
using meeting::ParticipantIdentitySnapshot;
using meeting::ParticipantRole;
using meeting::ParticipantState;
using meeting::ParticipantView;
using meeting::RequestId;
using meeting::ResultCode;
using meeting::ResultDisposition;
using meeting::Timestamp;

// ------------------------------------------------------------------------------
// 固定时间点：从 epoch 起按秒偏移，便于断言"输出就是调用方给的值"。
// ------------------------------------------------------------------------------
Timestamp AtSeconds(int seconds) {
    return Timestamp(std::chrono::seconds(seconds));
}

const int kHostUid = 1000;
const int kUserAUid = 2001;
const int kUserBUid = 2002;

const char* kMeetingId = "meeting-m2-1";
const char* kServerId = "chatserver-m2";

CreateMeetingParams MakeCreateParams() {
    CreateMeetingParams params;
    params.meeting_id = kMeetingId;
    params.host_user_id = kHostUid;
    params.host_session_id = "host-session-1";
    params.owner_chat_server_id = kServerId;
    params.created_at = AtSeconds(1);
    params.event_id = "evt-created-1";
    params.request_id = "req-create-1";
    return params;
}

MutationParams MakeMutation(int seconds, const std::string& event_id) {
    MutationParams params;
    params.now = AtSeconds(seconds);
    params.event_id = event_id;
    return params;
}

// Create 的唯一公开路径：同时拿到聚合与那个唯一的 MeetingCreated。
MeetingAggregate MakeAggregate(MeetingEvent& created_event) {
    return MeetingAggregate::Create(MakeCreateParams(), created_event);
}

// 有事件产生的 Join / Leave 才需要 request_id；被拒绝或幂等的路径同样传入一个
// 明确的值，用于证明"未产生事件时传入值不造成副作用"。
JoinResult DoJoin(MeetingAggregate& aggregate, int uid, const std::string& session,
                  const std::string& request_id, int seconds,
                  const std::string& event_id) {
    return aggregate.Join(uid, session, request_id, MakeMutation(seconds, event_id));
}

LeaveResult DoLeave(MeetingAggregate& aggregate, int uid, const std::string& request_id,
                    int seconds, const std::string& event_id) {
    return aggregate.Leave(uid, request_id, MakeMutation(seconds, event_id));
}

} // namespace

// ==============================================================================
// Create
// ==============================================================================

TEST(MeetingAggregateCreate, ProducesExactlyOneMeetingCreatedValue)
{
    MeetingEvent created;
    MeetingAggregate aggregate = MakeAggregate(created);

    // Create 的公开路径必须同时给出恰好一个 MeetingCreated value。
    EXPECT_EQ(created.event_type, EventType::MEETING_CREATED);

    // Step 1.4 §22.2 最低语义 + §21.2 字段规则。
    EXPECT_EQ(created.event_id, std::string("evt-created-1"));
    EXPECT_EQ(created.meeting_id, std::string(kMeetingId));
    EXPECT_EQ(created.owner_chat_server_id, std::string(kServerId));
    EXPECT_EQ(created.occurred_at, AtSeconds(1));
    EXPECT_EQ(created.meeting_state_after, MeetingState::CREATED);

    EXPECT_TRUE(created.has_actor_user_id);
    EXPECT_EQ(created.actor_user_id, kHostUid); // actor = Host

    // MeetingCreated 不强制填写 participant_user_id。
    EXPECT_FALSE(created.has_participant_user_id);

    // request_id correlation：由外部命令触发，必须携带。
    EXPECT_TRUE(created.has_request_id);
    EXPECT_EQ(created.request_id, std::string("req-create-1"));

    EXPECT_EQ(aggregate.Id(), std::string(kMeetingId));
}

TEST(MeetingAggregateCreate, InitialStateIsCreatedWithHostActiveAndBound)
{
    MeetingEvent created;
    MeetingAggregate aggregate = MakeAggregate(created);

    EXPECT_EQ(aggregate.State(), MeetingState::CREATED);
    EXPECT_EQ(aggregate.HostUserId(), kHostUid);
    EXPECT_EQ(aggregate.OwnerChatServerId(), std::string(kServerId));
    EXPECT_EQ(aggregate.CreatedAt(), AtSeconds(1));
    EXPECT_EQ(aggregate.ActiveParticipantCount(), 1u);

    ParticipantView host;
    ASSERT_TRUE(aggregate.TryGetParticipantView(kHostUid, host));
    EXPECT_EQ(host.role, ParticipantRole::HOST);
    EXPECT_EQ(host.participant_state, ParticipantState::ACTIVE);
    EXPECT_EQ(host.joined_at, AtSeconds(1));
    EXPECT_FALSE(host.has_left_at); // Host ACTIVE 时不得携带 left_at

    // Host 的初始 Session Binding 必须为 BOUND。
    EXPECT_TRUE(aggregate.IsSessionBound(kHostUid, "host-session-1"));
    EXPECT_EQ(aggregate.BoundSessionCount(kHostUid), 1u);

    // Create 的事件是 MeetingCreated，绝不是 ParticipantJoined：
    // 这里用真实 event_type 证明，而不是靠"没看到别的事件"间接推断。
    EXPECT_NE(created.event_type, EventType::PARTICIPANT_JOINED);

    // 且仅存在 Host 时不构成非 Host 激活。
    EXPECT_EQ(aggregate.State(), MeetingState::CREATED);
    EXPECT_EQ(aggregate.ActiveParticipantCount(), 1u);
}

TEST(MeetingAggregateCreate, ProjectionMatchesCreateInputs)
{
    MeetingEvent created;
    MeetingAggregate aggregate = MakeAggregate(created);
    const MeetingView view = aggregate.BuildMeetingView();

    EXPECT_EQ(view.meeting_id, std::string(kMeetingId));
    EXPECT_EQ(view.owner_chat_server_id, std::string(kServerId));
    EXPECT_EQ(view.host_user_id, kHostUid);
    EXPECT_EQ(view.meeting_state, MeetingState::CREATED);
    EXPECT_EQ(view.active_participant_count, 1u);
    EXPECT_EQ(view.created_at, AtSeconds(1));
    // M2 不进入 CLOSED 生命周期，因此 closed_at 必须为空。
    EXPECT_FALSE(view.has_closed_at);
}

// ==============================================================================
// Join —— 四种 outcome
// ==============================================================================

TEST(MeetingAggregateJoin, FirstNonHostProducesNewParticipantAndActivatesMeeting)
{
    MeetingEvent created;
    MeetingAggregate aggregate = MakeAggregate(created);

    const JoinResult result =
        DoJoin(aggregate, kUserAUid, "ua-session-1", "req-join-a-1", 10, "evt-join-a-1");

    EXPECT_EQ(result.result_code, ResultCode::OK);
    EXPECT_EQ(result.disposition, ResultDisposition::SUCCESS);
    EXPECT_EQ(result.outcome, JoinOutcome::NEW_PARTICIPANT);

    // CREATED → ACTIVE 由首个非 Host 激活触发，响应必须反映转换后的稳定状态。
    EXPECT_EQ(aggregate.State(), MeetingState::ACTIVE);
    EXPECT_EQ(result.meeting.meeting_state, MeetingState::ACTIVE);
    EXPECT_EQ(aggregate.ActiveParticipantCount(), 2u);
    EXPECT_EQ(result.meeting.active_participant_count, 2u);

    EXPECT_EQ(result.participant.user_id, kUserAUid);
    EXPECT_EQ(result.participant.role, ParticipantRole::PARTICIPANT);
    EXPECT_EQ(result.participant.participant_state, ParticipantState::ACTIVE);
    EXPECT_EQ(result.participant.joined_at, AtSeconds(10));
    EXPECT_FALSE(result.participant.has_left_at);
    EXPECT_TRUE(aggregate.IsSessionBound(kUserAUid, "ua-session-1"));

    // ParticipantJoined 恰好一次，且使用调用方提供的 event_id / 时间。
    ASSERT_TRUE(result.has_event);
    EXPECT_EQ(result.event.event_type, EventType::PARTICIPANT_JOINED);
    EXPECT_EQ(result.event.event_id, std::string("evt-join-a-1"));
    EXPECT_EQ(result.event.meeting_id, std::string(kMeetingId));
    EXPECT_EQ(result.event.occurred_at, AtSeconds(10));
    EXPECT_EQ(result.event.owner_chat_server_id, std::string(kServerId));
    EXPECT_TRUE(result.event.has_participant_user_id);
    EXPECT_EQ(result.event.participant_user_id, kUserAUid);

    // meeting_state_after 固定为 ACTIVE（Step 1.4 §23.2），不存在 Host 例外。
    EXPECT_EQ(result.event.meeting_state_after, MeetingState::ACTIVE);

    // request_id correlation。
    EXPECT_TRUE(result.event.has_request_id);
    EXPECT_EQ(result.event.request_id, std::string("req-join-a-1"));
}

TEST(MeetingAggregateJoin, DuplicateSameSessionIsIdempotentWithNoEvent)
{
    MeetingEvent created;
    MeetingAggregate aggregate = MakeAggregate(created);
    DoJoin(aggregate, kUserAUid, "ua-session-1", "req-join-a-1", 10, "evt-join-a-1");

    const std::size_t count_before = aggregate.ActiveParticipantCount();
    const JoinResult result =
        DoJoin(aggregate, kUserAUid, "ua-session-1", "req-dup", 11, "evt-dup");

    EXPECT_EQ(result.result_code, ResultCode::ALREADY_JOINED);
    EXPECT_EQ(result.disposition, ResultDisposition::IDEMPOTENT);
    EXPECT_EQ(result.outcome, JoinOutcome::ALREADY_BOUND);
    EXPECT_FALSE(result.has_event); // 幂等命中不得产生事件

    EXPECT_EQ(aggregate.ActiveParticipantCount(), count_before);
    EXPECT_EQ(aggregate.State(), MeetingState::ACTIVE);
    EXPECT_EQ(aggregate.BoundSessionCount(kUserAUid), 1u);
}

TEST(MeetingAggregateJoin, SecondSessionForActiveParticipantBindsWithoutCountChange)
{
    MeetingEvent created;
    MeetingAggregate aggregate = MakeAggregate(created);
    DoJoin(aggregate, kUserAUid, "ua-session-1", "req-join-a-1", 10, "evt-join-a-1");

    const JoinResult result =
        DoJoin(aggregate, kUserAUid, "ua-session-2", "req-join-a-2", 12, "evt-join-a-2");

    EXPECT_EQ(result.result_code, ResultCode::OK);
    EXPECT_EQ(result.disposition, ResultDisposition::SUCCESS);
    EXPECT_EQ(result.outcome, JoinOutcome::ADDITIONAL_SESSION_BOUND);

    // 逻辑成员不变：只增加接入关系。
    EXPECT_EQ(aggregate.ActiveParticipantCount(), 2u);
    EXPECT_EQ(result.meeting.active_participant_count, 2u);
    EXPECT_EQ(aggregate.BoundSessionCount(kUserAUid), 2u);
    EXPECT_TRUE(aggregate.IsSessionBound(kUserAUid, "ua-session-1"));
    EXPECT_TRUE(aggregate.IsSessionBound(kUserAUid, "ua-session-2"));

    // 不产生 ParticipantJoined。
    EXPECT_FALSE(result.has_event);
}

TEST(MeetingAggregateJoin, ReJoinRestoresActiveAndKeepsOriginalJoinedAt)
{
    MeetingEvent created;
    MeetingAggregate aggregate = MakeAggregate(created);
    DoJoin(aggregate, kUserAUid, "ua-session-1", "req-join-a-1", 10, "evt-join-a-1");
    DoLeave(aggregate, kUserAUid, "req-leave-a-1", 20, "evt-leave-a-1");
    EXPECT_EQ(aggregate.ActiveParticipantCount(), 1u);

    const JoinResult result =
        DoJoin(aggregate, kUserAUid, "ua-session-3", "req-rejoin-a-1", 30, "evt-rejoin-a-1");

    EXPECT_EQ(result.result_code, ResultCode::OK);
    EXPECT_EQ(result.disposition, ResultDisposition::SUCCESS);
    EXPECT_EQ(result.outcome, JoinOutcome::REJOINED_PARTICIPANT);

    // joined_at 保持首次逻辑加入时间，不因 Re-Join 重置。
    EXPECT_EQ(result.participant.joined_at, AtSeconds(10));
    EXPECT_EQ(result.participant.participant_state, ParticipantState::ACTIVE);
    EXPECT_FALSE(result.participant.has_left_at); // ACTIVE 时不得残留 left_at

    EXPECT_EQ(aggregate.ActiveParticipantCount(), 2u);
    EXPECT_TRUE(aggregate.IsSessionBound(kUserAUid, "ua-session-3"));

    // Re-Join 获得**新的** ParticipantJoined 事件（新的 event_id）。
    ASSERT_TRUE(result.has_event);
    EXPECT_EQ(result.event.event_type, EventType::PARTICIPANT_JOINED);
    EXPECT_EQ(result.event.event_id, std::string("evt-rejoin-a-1"));
    EXPECT_EQ(result.event.occurred_at, AtSeconds(30));

    // Re-Join 也必须满足 ParticipantJoined 的固定语义。
    EXPECT_EQ(result.event.meeting_state_after, MeetingState::ACTIVE);
    EXPECT_TRUE(result.event.has_request_id);
    EXPECT_EQ(result.event.request_id, std::string("req-rejoin-a-1"));
}

TEST(MeetingAggregateJoin, MeetingNeverRegressesFromActiveToCreated)
{
    MeetingEvent created;
    MeetingAggregate aggregate = MakeAggregate(created);
    DoJoin(aggregate, kUserAUid, "ua-session-1", "req-join-a-1", 10, "evt-join-a-1");
    EXPECT_EQ(aggregate.State(), MeetingState::ACTIVE);

    // 所有非 Host 成员离会后仍不得回退到 CREATED。
    const LeaveResult leave =
        DoLeave(aggregate, kUserAUid, "req-leave-a-1", 20, "evt-leave-a-1");
    EXPECT_EQ(aggregate.State(), MeetingState::ACTIVE);
    EXPECT_EQ(aggregate.ActiveParticipantCount(), 1u);

    // ParticipantLeft 的 meeting_state_after 是 mutation 后的当前状态，即 ACTIVE
    // —— 成员离会不得使状态回退。
    ASSERT_TRUE(leave.has_event);
    EXPECT_EQ(leave.event.meeting_state_after, MeetingState::ACTIVE);

    // 而且新的非 Host 加入也不再触发一次"转换"。
    DoJoin(aggregate, kUserBUid, "ub-session-1", "req-join-b-1", 40, "evt-join-b-1");
    EXPECT_EQ(aggregate.State(), MeetingState::ACTIVE);
}

TEST(MeetingAggregateJoin, HostAdditionalSessionDoesNotChangeStateOrCount)
{
    MeetingEvent created;
    MeetingAggregate aggregate = MakeAggregate(created);

    const std::size_t count_before = aggregate.ActiveParticipantCount();
    const JoinResult result =
        DoJoin(aggregate, kHostUid, "host-session-2", "req-host-join-2", 5, "evt-host-join-2");

    EXPECT_EQ(result.result_code, ResultCode::OK);
    EXPECT_EQ(result.outcome, JoinOutcome::ADDITIONAL_SESSION_BOUND);

    // Host 增加 Session 既不改变 Meeting 状态，也不改变逻辑成员数。
    EXPECT_EQ(aggregate.State(), MeetingState::CREATED);
    EXPECT_EQ(aggregate.ActiveParticipantCount(), count_before);
    EXPECT_EQ(aggregate.BoundSessionCount(kHostUid), 2u);
    EXPECT_FALSE(result.has_event);
}

TEST(MeetingAggregateJoin, HostDuplicateSessionIsIdempotent)
{
    MeetingEvent created;
    MeetingAggregate aggregate = MakeAggregate(created);

    const JoinResult result =
        DoJoin(aggregate, kHostUid, "host-session-1", "req-host-dup", 5, "evt-host-dup");

    EXPECT_EQ(result.result_code, ResultCode::ALREADY_JOINED);
    EXPECT_EQ(result.disposition, ResultDisposition::IDEMPOTENT);
    EXPECT_EQ(result.outcome, JoinOutcome::ALREADY_BOUND);
    EXPECT_FALSE(result.has_event);
    EXPECT_EQ(aggregate.State(), MeetingState::CREATED);
    EXPECT_EQ(aggregate.ActiveParticipantCount(), 1u);
}

TEST(MeetingAggregateJoin, JoinDuringEndingIsRejectedWithoutMutation)
{
    MeetingEvent created;
    MeetingAggregate aggregate = MakeAggregate(created);
    DoJoin(aggregate, kUserAUid, "ua-session-1", "req-join-a-1", 10, "evt-join-a-1");
    const BeginCloseResult close =
        aggregate.BeginClose(kHostUid, "req-close-1", MakeMutation(50, "evt-unused"));
    ASSERT_EQ(close.result_code, ResultCode::OK);

    const std::size_t count_before = aggregate.ActiveParticipantCount();
    const JoinResult result =
        DoJoin(aggregate, kUserBUid, "ub-session-1", "req-join-b-1", 60, "evt-join-b-1");

    EXPECT_EQ(result.result_code, ResultCode::MEETING_STATE_REJECTED);
    EXPECT_EQ(result.disposition, ResultDisposition::ERROR);
    EXPECT_FALSE(result.has_event);

    // 无 mutation：成员数不变，新成员并未被创建。
    EXPECT_EQ(aggregate.ActiveParticipantCount(), count_before);
    ParticipantView not_created;
    EXPECT_FALSE(aggregate.TryGetParticipantView(kUserBUid, not_created));
}

// ==============================================================================
// Leave
// ==============================================================================

TEST(MeetingAggregateLeave, ActiveParticipantLeavesUnbindingAllSessions)
{
    MeetingEvent created;
    MeetingAggregate aggregate = MakeAggregate(created);
    DoJoin(aggregate, kUserAUid, "ua-session-1", "req-join-a-1", 10, "evt-join-a-1");
    DoJoin(aggregate, kUserAUid, "ua-session-2", "req-join-a-2", 11, "evt-join-a-2");
    ASSERT_EQ(aggregate.BoundSessionCount(kUserAUid), 2u);

    const LeaveResult result =
        DoLeave(aggregate, kUserAUid, "req-leave-a-1", 20, "evt-leave-a-1");

    EXPECT_EQ(result.result_code, ResultCode::OK);
    EXPECT_EQ(result.disposition, ResultDisposition::SUCCESS);
    EXPECT_EQ(result.outcome, LeaveOutcome::LEFT);

    // User 级离会：该 User 在本 Meeting 下全部 Binding → UNBOUND。
    EXPECT_EQ(aggregate.BoundSessionCount(kUserAUid), 0u);
    EXPECT_FALSE(aggregate.IsSessionBound(kUserAUid, "ua-session-1"));
    EXPECT_FALSE(aggregate.IsSessionBound(kUserAUid, "ua-session-2"));

    // 成员数只减一次。
    EXPECT_EQ(aggregate.ActiveParticipantCount(), 1u);
    EXPECT_EQ(result.meeting.active_participant_count, 1u);

    ParticipantView view;
    ASSERT_TRUE(aggregate.TryGetParticipantView(kUserAUid, view));
    EXPECT_EQ(view.participant_state, ParticipantState::LEFT);
    EXPECT_TRUE(view.has_left_at);
    EXPECT_EQ(view.left_at, AtSeconds(20));
    EXPECT_EQ(view.joined_at, AtSeconds(10)); // 历史身份保留

    ASSERT_TRUE(result.has_event);
    EXPECT_EQ(result.event.event_type, EventType::PARTICIPANT_LEFT);
    EXPECT_EQ(result.event.event_id, std::string("evt-leave-a-1"));
    EXPECT_EQ(result.event.occurred_at, AtSeconds(20));
    EXPECT_TRUE(result.event.has_actor_user_id);
    EXPECT_EQ(result.event.actor_user_id, kUserAUid);

    // state_after 必须是 mutation 后的真实状态（此处仍为 ACTIVE，不回退）。
    EXPECT_EQ(result.event.meeting_state_after, aggregate.State());
    EXPECT_EQ(result.event.meeting_state_after, MeetingState::ACTIVE);

    // request_id correlation。
    EXPECT_TRUE(result.event.has_request_id);
    EXPECT_EQ(result.event.request_id, std::string("req-leave-a-1"));
}

TEST(MeetingAggregateLeave, DuplicateLeaveIsIdempotentWithNoEvent)
{
    MeetingEvent created;
    MeetingAggregate aggregate = MakeAggregate(created);
    DoJoin(aggregate, kUserAUid, "ua-session-1", "req-join-a-1", 10, "evt-join-a-1");
    DoLeave(aggregate, kUserAUid, "req-leave-a-1", 20, "evt-leave-a-1");

    const std::size_t count_before = aggregate.ActiveParticipantCount();
    const LeaveResult result =
        DoLeave(aggregate, kUserAUid, "req-leave-dup", 21, "evt-leave-dup");

    EXPECT_EQ(result.result_code, ResultCode::ALREADY_LEFT);
    EXPECT_EQ(result.disposition, ResultDisposition::IDEMPOTENT);
    EXPECT_FALSE(result.has_event);
    EXPECT_EQ(aggregate.ActiveParticipantCount(), count_before);
}

TEST(MeetingAggregateLeave, NeverParticipantIsNotParticipant)
{
    MeetingEvent created;
    MeetingAggregate aggregate = MakeAggregate(created);

    const LeaveResult result =
        DoLeave(aggregate, kUserBUid, "req-leave-b", 20, "evt-leave-b");

    EXPECT_EQ(result.result_code, ResultCode::NOT_PARTICIPANT);
    EXPECT_EQ(result.disposition, ResultDisposition::ERROR);
    EXPECT_FALSE(result.has_event);
    EXPECT_EQ(aggregate.ActiveParticipantCount(), 1u);
}

TEST(MeetingAggregateLeave, HostLeavingOpenMeetingMustCloseInstead)
{
    MeetingEvent created;
    MeetingAggregate aggregate = MakeAggregate(created);
    DoJoin(aggregate, kUserAUid, "ua-session-1", "req-join-a-1", 10, "evt-join-a-1");

    const std::size_t count_before = aggregate.ActiveParticipantCount();
    const LeaveResult result =
        DoLeave(aggregate, kHostUid, "req-host-leave", 20, "evt-host-leave");

    EXPECT_EQ(result.result_code, ResultCode::HOST_MUST_CLOSE_MEETING);
    EXPECT_EQ(result.disposition, ResultDisposition::ERROR);
    EXPECT_FALSE(result.has_event);

    // 不得解绑 Host、不得减成员数、不得改变 Host 的 ParticipantState。
    EXPECT_EQ(aggregate.ActiveParticipantCount(), count_before);
    EXPECT_TRUE(aggregate.IsSessionBound(kHostUid, "host-session-1"));
    ParticipantView host;
    ASSERT_TRUE(aggregate.TryGetParticipantView(kHostUid, host));
    EXPECT_EQ(host.participant_state, ParticipantState::ACTIVE);
    EXPECT_EQ(aggregate.State(), MeetingState::ACTIVE);
}

TEST(MeetingAggregateLeave, LeaveDuringEndingIsRejectedForEveryone)
{
    MeetingEvent created;
    MeetingAggregate aggregate = MakeAggregate(created);
    DoJoin(aggregate, kUserAUid, "ua-session-1", "req-join-a-1", 10, "evt-join-a-1");
    aggregate.BeginClose(kHostUid, "req-close-1", MakeMutation(50, "evt-unused"));

    const std::size_t count_before = aggregate.ActiveParticipantCount();

    const LeaveResult member_result =
        DoLeave(aggregate, kUserAUid, "req-leave-a-1", 60, "evt-leave-a-1");
    EXPECT_EQ(member_result.result_code, ResultCode::MEETING_STATE_REJECTED);
    EXPECT_EQ(member_result.disposition, ResultDisposition::ERROR);
    EXPECT_FALSE(member_result.has_event);

    // 包括 Host。
    const LeaveResult host_result =
        DoLeave(aggregate, kHostUid, "req-host-leave", 61, "evt-host-leave");
    EXPECT_EQ(host_result.result_code, ResultCode::MEETING_STATE_REJECTED);
    EXPECT_FALSE(host_result.has_event);

    // 两条拒绝路径都不得 mutation。
    EXPECT_EQ(aggregate.ActiveParticipantCount(), count_before);
    EXPECT_TRUE(aggregate.IsSessionBound(kUserAUid, "ua-session-1"));
}

// ==============================================================================
// BeginClose
// ==============================================================================

TEST(MeetingAggregateBeginClose, HostClosesFromCreated)
{
    MeetingEvent created;
    MeetingAggregate aggregate = MakeAggregate(created);
    ASSERT_EQ(aggregate.State(), MeetingState::CREATED);

    const BeginCloseResult result =
        aggregate.BeginClose(kHostUid, "req-close-1", MakeMutation(50, "evt-unused"));

    EXPECT_EQ(result.result_code, ResultCode::OK);
    EXPECT_EQ(result.disposition, ResultDisposition::SUCCESS);
    EXPECT_EQ(result.outcome, meeting::CloseOutcome::CLOSE_STARTED);
    EXPECT_TRUE(result.should_schedule_finalize);
    EXPECT_TRUE(result.has_close_context);

    EXPECT_EQ(aggregate.State(), MeetingState::ENDING);
    EXPECT_EQ(result.meeting.meeting_state, MeetingState::ENDING);

    // 不产生 MeetingClosed：FinalizeClose 属于 M3。BeginCloseResult 本身不携带
    // 事件，因此这里的断言是"没有可供发布的终态事件"。
    EXPECT_EQ(aggregate.BuildMeetingView().meeting_state, MeetingState::ENDING);
}

TEST(MeetingAggregateBeginClose, HostClosesFromActive)
{
    MeetingEvent created;
    MeetingAggregate aggregate = MakeAggregate(created);
    DoJoin(aggregate, kUserAUid, "ua-session-1", "req-join-a-1", 10, "evt-join-a-1");
    ASSERT_EQ(aggregate.State(), MeetingState::ACTIVE);

    const BeginCloseResult result =
        aggregate.BeginClose(kHostUid, "req-close-1", MakeMutation(50, "evt-unused"));

    EXPECT_EQ(result.result_code, ResultCode::OK);
    EXPECT_TRUE(result.should_schedule_finalize);
    EXPECT_EQ(aggregate.State(), MeetingState::ENDING);
}

TEST(MeetingAggregateBeginClose, SecondHostCloseIsIdempotentAndDoesNotReschedule)
{
    MeetingEvent created;
    MeetingAggregate aggregate = MakeAggregate(created);
    DoJoin(aggregate, kUserAUid, "ua-session-1", "req-join-a-1", 10, "evt-join-a-1");
    const BeginCloseResult first =
        aggregate.BeginClose(kHostUid, "req-close-1", MakeMutation(50, "evt-unused"));
    ASSERT_TRUE(first.has_close_context);

    const CloseContext first_context = first.close_context;
    const BeginCloseResult second =
        aggregate.BeginClose(kHostUid, "req-close-2", MakeMutation(70, "evt-unused-2"));

    EXPECT_EQ(second.result_code, ResultCode::CLOSE_IN_PROGRESS);
    EXPECT_EQ(second.disposition, ResultDisposition::IDEMPOTENT);
    // 不得再安排第二个 FinalizeClose、不得冻结第二个 CloseContext。
    EXPECT_FALSE(second.should_schedule_finalize);
    EXPECT_FALSE(second.has_close_context);

    // 冻结材料保持不变：originating_request_id 与 close_started_at 仍是第一次的值。
    ASSERT_TRUE(aggregate.HasCloseContext());
    EXPECT_EQ(aggregate.FrozenCloseContext().originating_request_id,
              first_context.originating_request_id);
    EXPECT_EQ(aggregate.FrozenCloseContext().close_started_at,
              first_context.close_started_at);
    EXPECT_EQ(aggregate.State(), MeetingState::ENDING);
}

TEST(MeetingAggregateBeginClose, NonHostIsDeniedFromCreatedAndActive)
{
    MeetingEvent created_a;
    MeetingAggregate from_created = MakeAggregate(created_a);
    const BeginCloseResult created_result = from_created.BeginClose(
        kUserAUid, "req-close-x", MakeMutation(50, "evt-unused"));
    EXPECT_EQ(created_result.result_code, ResultCode::PERMISSION_DENIED);
    EXPECT_EQ(created_result.disposition, ResultDisposition::ERROR);
    EXPECT_FALSE(created_result.should_schedule_finalize);
    EXPECT_FALSE(created_result.has_close_context);
    EXPECT_EQ(from_created.State(), MeetingState::CREATED);

    MeetingEvent created_b;
    MeetingAggregate from_active = MakeAggregate(created_b);
    DoJoin(from_active, kUserAUid, "ua-session-1", "req-join-a-1", 10, "evt-join-a-1");
    DoJoin(from_active, kUserBUid, "ub-session-1", "req-join-b-1", 11, "evt-join-b-1");

    const BeginCloseResult active_result = from_active.BeginClose(
        kUserBUid, "req-close-y", MakeMutation(50, "evt-unused"));
    EXPECT_EQ(active_result.result_code, ResultCode::PERMISSION_DENIED);
    EXPECT_FALSE(active_result.has_close_context);
    EXPECT_EQ(from_active.State(), MeetingState::ACTIVE); // 未被非 Host 改变
}

TEST(MeetingAggregateBeginClose, NonHostIsDeniedEvenWhenMeetingIsEnding)
{
    MeetingEvent created;
    MeetingAggregate aggregate = MakeAggregate(created);
    DoJoin(aggregate, kUserAUid, "ua-session-1", "req-join-a-1", 10, "evt-join-a-1");
    aggregate.BeginClose(kHostUid, "req-close-1", MakeMutation(50, "evt-unused"));
    ASSERT_EQ(aggregate.State(), MeetingState::ENDING);

    // 关键：非 Host 不能通过 CLOSE_IN_PROGRESS 推断 Host-only 内部状态。
    const BeginCloseResult result =
        aggregate.BeginClose(kUserAUid, "req-close-z", MakeMutation(60, "evt-unused"));

    EXPECT_EQ(result.result_code, ResultCode::PERMISSION_DENIED);
    EXPECT_EQ(result.disposition, ResultDisposition::ERROR);
    EXPECT_FALSE(result.should_schedule_finalize);
}

// ==============================================================================
// CloseContext
// ==============================================================================

TEST(MeetingAggregateCloseContext, FreezesStableIdentityAndSessionTargets)
{
    MeetingEvent created;
    MeetingAggregate aggregate = MakeAggregate(created);
    DoJoin(aggregate, kUserAUid, "ua-session-1", "req-join-a-1", 10, "evt-join-a-1");
    DoJoin(aggregate, kUserAUid, "ua-session-2", "req-join-a-2", 11, "evt-join-a-2");
    DoJoin(aggregate, kUserBUid, "ub-session-1", "req-join-b-1", 12, "evt-join-b-1");

    const BeginCloseResult result =
        aggregate.BeginClose(kHostUid, "req-close-1", MakeMutation(50, "evt-unused"));
    ASSERT_EQ(result.result_code, ResultCode::OK);
    ASSERT_TRUE(result.has_close_context);

    const CloseContext& context = result.close_context;
    EXPECT_EQ(context.meeting_id, std::string(kMeetingId));
    EXPECT_EQ(context.host_user_id, kHostUid);
    EXPECT_EQ(context.owner_chat_server_id, std::string(kServerId));
    EXPECT_EQ(context.originating_request_id, std::string("req-close-1"));
    EXPECT_EQ(context.close_started_at, AtSeconds(50));

    // 可通知目标只包含仍 BOUND 的 session。
    ASSERT_EQ(context.notifiable_session_ids.size(), 4u); // host1 + ua1 + ua2 + ub1
    bool has_host = false, has_ua1 = false, has_ua2 = false, has_ub1 = false;
    for (std::size_t i = 0; i < context.notifiable_session_ids.size(); ++i) {
        const std::string& s = context.notifiable_session_ids[i];
        if (s == "host-session-1") has_host = true;
        if (s == "ua-session-1") has_ua1 = true;
        if (s == "ua-session-2") has_ua2 = true;
        if (s == "ub-session-1") has_ub1 = true;
    }
    EXPECT_TRUE(has_host);
    EXPECT_TRUE(has_ua1);
    EXPECT_TRUE(has_ua2);
    EXPECT_TRUE(has_ub1);

    // 冻结边界上的活跃成员数。
    EXPECT_EQ(context.frozen_active_participant_count, 3u);
}

TEST(MeetingAggregateCloseContext, RetainsLeftParticipantHistoryAndExcludesThemFromCount)
{
    MeetingEvent created;
    MeetingAggregate aggregate = MakeAggregate(created);
    // Host ACTIVE, A 加入后 LEFT, B ACTIVE。
    DoJoin(aggregate, kUserAUid, "ua-session-1", "req-join-a-1", 10, "evt-join-a-1");
    DoJoin(aggregate, kUserBUid, "ub-session-1", "req-join-b-1", 11, "evt-join-b-1");
    DoLeave(aggregate, kUserAUid, "req-leave-a-1", 20, "evt-leave-a-1");
    ASSERT_EQ(aggregate.ActiveParticipantCount(), 2u);

    const BeginCloseResult result =
        aggregate.BeginClose(kHostUid, "req-close-1", MakeMutation(50, "evt-unused"));
    ASSERT_TRUE(result.has_close_context);
    const CloseContext& context = result.close_context;

    // 历史身份必须完整：Host + A（已 LEFT）+ B。
    ASSERT_EQ(context.participant_identities.size(), 3u);

    bool found_a_left = false;
    for (std::size_t i = 0; i < context.participant_identities.size(); ++i) {
        const ParticipantIdentitySnapshot& identity = context.participant_identities[i];
        if (identity.user_id == kUserAUid) {
            found_a_left = true;
            EXPECT_TRUE(identity.has_left_at); // 保留离会边界
            EXPECT_EQ(identity.left_at, AtSeconds(20));
            EXPECT_EQ(identity.joined_at, AtSeconds(10));
        }
    }
    EXPECT_TRUE(found_a_left);

    // 关键语义：final_participant_count 是**冻结边界上的 ACTIVE 数**，
    // 不是历史加入过的人数（此处 2 vs 3）。
    EXPECT_EQ(context.frozen_active_participant_count, 2u);
    EXPECT_NE(context.frozen_active_participant_count,
              context.participant_identities.size());

    // A 已离会，其 session 不再是可通知目标。
    for (std::size_t i = 0; i < context.notifiable_session_ids.size(); ++i) {
        EXPECT_NE(context.notifiable_session_ids[i], std::string("ua-session-1"));
    }
}

// ==============================================================================
// Snapshot material
// ==============================================================================

TEST(MeetingAggregateSnapshot, BuildsClosedSnapshotFromFrozenBoundary)
{
    MeetingEvent created;
    MeetingAggregate aggregate = MakeAggregate(created);
    DoJoin(aggregate, kUserAUid, "ua-session-1", "req-join-a-1", 10, "evt-join-a-1");
    DoJoin(aggregate, kUserBUid, "ub-session-1", "req-join-b-1", 11, "evt-join-b-1");
    DoLeave(aggregate, kUserAUid, "req-leave-a-1", 20, "evt-leave-a-1");

    const BeginCloseResult close =
        aggregate.BeginClose(kHostUid, "req-close-1", MakeMutation(50, "evt-unused"));
    ASSERT_TRUE(close.has_close_context);

    // closed_at 由外部提供（M2 不在聚合内取时间）。
    const Timestamp closed_at = AtSeconds(75);
    const ClosedMeetingSnapshot snapshot =
        aggregate.BuildClosedSnapshot(close.close_context, closed_at);

    EXPECT_EQ(snapshot.meeting_id, std::string(kMeetingId));
    EXPECT_EQ(snapshot.host_user_id, kHostUid);
    EXPECT_EQ(snapshot.owner_chat_server_id, std::string(kServerId));
    EXPECT_EQ(snapshot.meeting_state, MeetingState::CLOSED);
    EXPECT_EQ(snapshot.created_at, AtSeconds(1)); // created_at 保持原值
    EXPECT_EQ(snapshot.closed_at, closed_at);     // closed_at 使用调用方提供的值

    // A 的历史身份仍在（已 LEFT 但未被丢弃）。
    ASSERT_EQ(snapshot.participant_identities.size(), 3u);
    bool found_a = false;
    for (std::size_t i = 0; i < snapshot.participant_identities.size(); ++i) {
        if (snapshot.participant_identities[i].user_id == kUserAUid) {
            found_a = true;
            EXPECT_TRUE(snapshot.participant_identities[i].has_left_at);
        }
    }
    EXPECT_TRUE(found_a);

    // final_participant_count 来自冻结边界，而不是 identities 的数量。
    EXPECT_EQ(snapshot.final_participant_count, 2u);
    EXPECT_NE(snapshot.final_participant_count, snapshot.participant_identities.size());
}

TEST(MeetingAggregateSnapshot, BuildingSnapshotDoesNotMutateAggregateState)
{
    MeetingEvent created;
    MeetingAggregate aggregate = MakeAggregate(created);
    DoJoin(aggregate, kUserAUid, "ua-session-1", "req-join-a-1", 10, "evt-join-a-1");
    aggregate.BeginClose(kHostUid, "req-close-1", MakeMutation(50, "evt-unused"));

    const MeetingState state_before = aggregate.State();
    const std::size_t count_before = aggregate.ActiveParticipantCount();
    const ClosedMeetingSnapshot snapshot =
        aggregate.BuildClosedSnapshot(aggregate.FrozenCloseContext(), AtSeconds(75));

    EXPECT_EQ(snapshot.meeting_state, MeetingState::CLOSED);
    // 聚合本身不进入 CLOSED：registry 的原子发布是 M3 的职责。
    EXPECT_EQ(aggregate.State(), state_before);
    EXPECT_EQ(aggregate.State(), MeetingState::ENDING);
    EXPECT_EQ(aggregate.ActiveParticipantCount(), count_before);
    EXPECT_FALSE(aggregate.BuildMeetingView().has_closed_at);
}

// ==============================================================================
// CURRENT participant projection（M2.1 Correction D）
// ==============================================================================

TEST(MeetingAggregateCurrentProjection, ListsActiveParticipantsOnly)
{
    MeetingEvent created;
    MeetingAggregate aggregate = MakeAggregate(created);
    // Host ACTIVE, A 加入, B 加入, A 离会。
    DoJoin(aggregate, kUserAUid, "ua-session-1", "req-join-a-1", 10, "evt-join-a-1");
    DoJoin(aggregate, kUserBUid, "ub-session-1", "req-join-b-1", 11, "evt-join-b-1");
    DoLeave(aggregate, kUserAUid, "req-leave-a-1", 20, "evt-leave-a-1");

    // 历史记录仍是 Host + A + B。
    const std::vector<ParticipantView> current = aggregate.ListCurrentParticipantViews();
    ASSERT_EQ(current.size(), 2u); // Host + B only

    bool a_present = false;
    bool host_present = false;
    bool b_present = false;
    for (std::size_t i = 0; i < current.size(); ++i) {
        EXPECT_EQ(current[i].participant_state, ParticipantState::ACTIVE);
        if (current[i].user_id == kUserAUid) a_present = true;
        if (current[i].user_id == kHostUid) host_present = true;
        if (current[i].user_id == kUserBUid) b_present = true;
    }
    EXPECT_FALSE(a_present); // A 不出现在 CURRENT 列表
    EXPECT_TRUE(host_present);
    EXPECT_TRUE(b_present);

    // CURRENT 投影 ≠ 历史存储：A 仍可通过单点查询定位，且状态为 LEFT。
    ParticipantView a_view;
    ASSERT_TRUE(aggregate.TryGetParticipantView(kUserAUid, a_view));
    EXPECT_EQ(a_view.participant_state, ParticipantState::LEFT);
    EXPECT_TRUE(a_view.has_left_at);
    EXPECT_EQ(a_view.joined_at, AtSeconds(10));
}

TEST(MeetingAggregateCurrentProjection, ReJoinReappearsInCurrentList)
{
    MeetingEvent created;
    MeetingAggregate aggregate = MakeAggregate(created);
    DoJoin(aggregate, kUserAUid, "ua-session-1", "req-join-a-1", 10, "evt-join-a-1");
    DoLeave(aggregate, kUserAUid, "req-leave-a-1", 20, "evt-leave-a-1");
    ASSERT_EQ(aggregate.ListCurrentParticipantViews().size(), 1u); // 仅 Host

    DoJoin(aggregate, kUserAUid, "ua-session-2", "req-rejoin-a-1", 30, "evt-rejoin-a-1");

    const std::vector<ParticipantView> current = aggregate.ListCurrentParticipantViews();
    ASSERT_EQ(current.size(), 2u);
    bool a_present = false;
    for (std::size_t i = 0; i < current.size(); ++i) {
        if (current[i].user_id == kUserAUid) {
            a_present = true;
            EXPECT_EQ(current[i].participant_state, ParticipantState::ACTIVE);
            EXPECT_FALSE(current[i].has_left_at); // ACTIVE 时不得携带 left_at
        }
    }
    EXPECT_TRUE(a_present);
}

// ==============================================================================
// Determinism
// ==============================================================================

TEST(MeetingAggregateDeterminism, OutputsUseCallerSuppliedTimeAndEventIds)
{
    // 同一串输入两次必须得到完全相同的可观测输出，且全部时间/ID 都来自调用方。
    const Timestamp t_join = AtSeconds(1234);
    const Timestamp t_leave = AtSeconds(5678);
    const Timestamp t_close = AtSeconds(9012);

    MeetingEvent created_first;
    MeetingAggregate first = MakeAggregate(created_first);
    const JoinResult first_join =
        DoJoin(first, kUserAUid, "ua-s1", "REQ-J", 1234, "E-J");
    const LeaveResult first_leave = DoLeave(first, kUserAUid, "REQ-L", 5678, "E-L");
    const BeginCloseResult first_close =
        first.BeginClose(kHostUid, "R-C", MakeMutation(9012, "E-X"));

    MeetingEvent created_second;
    MeetingAggregate second = MakeAggregate(created_second);
    const JoinResult second_join =
        DoJoin(second, kUserAUid, "ua-s1", "REQ-J", 1234, "E-J");
    const LeaveResult second_leave = DoLeave(second, kUserAUid, "REQ-L", 5678, "E-L");
    const BeginCloseResult second_close =
        second.BeginClose(kHostUid, "R-C", MakeMutation(9012, "E-X"));

    ASSERT_TRUE(first_join.has_event);
    ASSERT_TRUE(second_join.has_event);
    ASSERT_TRUE(first_leave.has_event);
    ASSERT_TRUE(second_leave.has_event);
    ASSERT_TRUE(first_close.has_close_context);
    ASSERT_TRUE(second_close.has_close_context);

    EXPECT_EQ(first_join.event.occurred_at, t_join);
    EXPECT_EQ(first_leave.event.occurred_at, t_leave);
    EXPECT_EQ(first_close.close_context.close_started_at, t_close);

    EXPECT_EQ(first_join.event.event_id, second_join.event.event_id);
    EXPECT_EQ(first_join.event.occurred_at, second_join.event.occurred_at);
    EXPECT_EQ(first_join.event.request_id, second_join.event.request_id);
    EXPECT_EQ(first_join.event.meeting_state_after,
              second_join.event.meeting_state_after);
    EXPECT_EQ(first_leave.event.event_id, second_leave.event.event_id);
    EXPECT_EQ(first_close.close_context.close_started_at,
              second_close.close_context.close_started_at);
    EXPECT_EQ(first_close.close_context.originating_request_id,
              second_close.close_context.originating_request_id);
    EXPECT_EQ(first_close.close_context.frozen_active_participant_count,
              second_close.close_context.frozen_active_participant_count);

    // Create 事件的确定性同样成立。
    EXPECT_EQ(created_first.event_id, created_second.event_id);
    EXPECT_EQ(created_first.meeting_state_after, created_second.meeting_state_after);
    EXPECT_EQ(created_first.request_id, created_second.request_id);
}

TEST(MeetingAggregateDeterminism, UnusedEventIdHasNoEffectOnRejectedPaths)
{
    MeetingEvent created;
    MeetingAggregate aggregate = MakeAggregate(created);

    // 被拒绝的路径：调用方提供了 event_id 与 request_id，但不允许产生任何副作用。
    const LeaveResult never =
        DoLeave(aggregate, kUserBUid, "REQ-UNUSED", 20, "E-UNUSED");
    EXPECT_EQ(never.result_code, ResultCode::NOT_PARTICIPANT);
    EXPECT_FALSE(never.has_event);

    const BeginCloseResult denied =
        aggregate.BeginClose(kUserBUid, "R-DENIED", MakeMutation(21, "E-UNUSED-2"));
    EXPECT_EQ(denied.result_code, ResultCode::PERMISSION_DENIED);
    EXPECT_FALSE(denied.has_close_context);
    EXPECT_FALSE(denied.should_schedule_finalize);

    // 幂等路径同样不产生事件。
    const JoinResult idem =
        DoJoin(aggregate, kHostUid, "host-session-1", "REQ-IDEM", 22, "E-IDEM");
    EXPECT_EQ(idem.result_code, ResultCode::ALREADY_JOINED);
    EXPECT_FALSE(idem.has_event);

    // 聚合状态保持初始形态。
    EXPECT_EQ(aggregate.State(), MeetingState::CREATED);
    EXPECT_EQ(aggregate.ActiveParticipantCount(), 1u);
    EXPECT_EQ(aggregate.ListCurrentParticipantViews().size(), 1u);
}

// ==============================================================================
// Invariants
// ==============================================================================

TEST(MeetingAggregateInvariants, CountEqualsActiveParticipantsAndLeftAreRetained)
{
    MeetingEvent created;
    MeetingAggregate aggregate = MakeAggregate(created);
    DoJoin(aggregate, kUserAUid, "ua-s1", "R1", 10, "E1");
    DoJoin(aggregate, kUserBUid, "ub-s1", "R2", 11, "E2");
    DoLeave(aggregate, kUserAUid, "R3", 20, "E3");
    DoJoin(aggregate, kUserAUid, "ua-s2", "R4", 30, "E4"); // Re-Join

    // I10：active_participant_count 等于 ACTIVE Participant 数量，
    // 而 CURRENT 投影正是 ACTIVE 成员，因此两者必须一致。
    const std::vector<ParticipantView> current = aggregate.ListCurrentParticipantViews();
    EXPECT_EQ(aggregate.ActiveParticipantCount(), current.size());
    EXPECT_EQ(aggregate.ActiveParticipantCount(), 3u);

    // I7：同一 User 只有一个逻辑 Participant —— CURRENT 列表中不会出现重复 uid。
    for (std::size_t i = 0; i < current.size(); ++i) {
        for (std::size_t j = i + 1; j < current.size(); ++j) {
            EXPECT_NE(current[i].user_id, current[j].user_id);
        }
    }

    // I3：ACTIVE 非 Host Participant 至少有一条 BOUND Binding。
    for (std::size_t i = 0; i < current.size(); ++i) {
        if (current[i].role == ParticipantRole::PARTICIPANT) {
            EXPECT_GE(aggregate.BoundSessionCount(current[i].user_id), 1u);
        }
    }

    // A 的 Re-Join 复用同一条历史记录（joined_at 不重置），历史身份未丢失。
    ParticipantView a_view;
    ASSERT_TRUE(aggregate.TryGetParticipantView(kUserAUid, a_view));
    EXPECT_EQ(a_view.participant_state, ParticipantState::ACTIVE);
    EXPECT_EQ(a_view.joined_at, AtSeconds(10));
}

TEST(MeetingAggregateInvariants, ActiveNonHostKeepsAtLeastOneBoundSessionAcrossRebinds)
{
    MeetingEvent created;
    MeetingAggregate aggregate = MakeAggregate(created);
    DoJoin(aggregate, kUserAUid, "ua-s1", "R1", 10, "E1");
    DoJoin(aggregate, kUserAUid, "ua-s2", "R2", 11, "E2");

    EXPECT_EQ(aggregate.BoundSessionCount(kUserAUid), 2u);

    // 离会后全部解绑；Re-Join 用新 session 重新满足 I3。
    DoLeave(aggregate, kUserAUid, "R3", 20, "E3");
    EXPECT_EQ(aggregate.BoundSessionCount(kUserAUid), 0u);

    DoJoin(aggregate, kUserAUid, "ua-s3", "R4", 30, "E4");
    EXPECT_EQ(aggregate.BoundSessionCount(kUserAUid), 1u);
    EXPECT_TRUE(aggregate.IsSessionBound(kUserAUid, "ua-s3"));
    EXPECT_FALSE(aggregate.IsSessionBound(kUserAUid, "ua-s1")); // 旧 binding 未复活
}
