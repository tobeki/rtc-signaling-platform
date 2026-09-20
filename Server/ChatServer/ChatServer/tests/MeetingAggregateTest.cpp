// ==============================================================================
// M2 MeetingAggregate 单元测试（Phase 1 Layer A：Pure Domain）。
//
// 覆盖聚合当前负责的核心分支：Create / Join 四种 outcome / Leave /
// BeginClose / CloseContext / Snapshot material。
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
using meeting::ParticipantRole;
using meeting::ParticipantState;
using meeting::ParticipantView;
using meeting::ParticipantIdentitySnapshot;
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

MeetingAggregate MakeAggregate() {
    MeetingAggregate aggregate(MakeCreateParams());
    return aggregate;
}

// Create 本身已产生一次 MeetingCreated。聚合不返回该事件（见 M2 文档 §5），
// 因此 Create 的事件断言放在 §Create 组里通过"没有其他事件"来体现。

} // namespace

// ==============================================================================
// Create
// ==============================================================================

TEST(MeetingAggregateCreate, InitialStateIsCreatedWithHostActiveAndBound)
{
    MeetingAggregate aggregate(MakeCreateParams());

    EXPECT_EQ(aggregate.State(), MeetingState::CREATED);
    EXPECT_EQ(aggregate.Id(), std::string(kMeetingId));
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
}

TEST(MeetingAggregateCreate, ProjectionMatchesCreateInputs)
{
    MeetingAggregate aggregate(MakeCreateParams());
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

TEST(MeetingAggregateCreate, FirstNonHostJoinDoesNotEmitParticipantJoinedForHost)
{
    // Create 只创建 Host。仅存在 Host 时不应有任何 ParticipantJoined 语义：
    // Host 的 ACTIVE 不计入"非 Host 激活"，Meeting 必须仍是 CREATED。
    MeetingAggregate aggregate = MakeAggregate();
    EXPECT_EQ(aggregate.State(), MeetingState::CREATED);
    EXPECT_EQ(aggregate.ActiveParticipantCount(), 1u);
}

// ==============================================================================
// Join —— 四种 outcome
// ==============================================================================

TEST(MeetingAggregateJoin, FirstNonHostProducesNewParticipantAndActivatesMeeting)
{
    MeetingAggregate aggregate = MakeAggregate();

    const JoinResult result =
        aggregate.Join(kUserAUid, "ua-session-1", MakeMutation(10, "evt-join-a-1"));

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
}

TEST(MeetingAggregateJoin, DuplicateSameSessionIsIdempotentWithNoEvent)
{
    MeetingAggregate aggregate = MakeAggregate();
    aggregate.Join(kUserAUid, "ua-session-1", MakeMutation(10, "evt-join-a-1"));

    const std::size_t count_before = aggregate.ActiveParticipantCount();
    const JoinResult result =
        aggregate.Join(kUserAUid, "ua-session-1", MakeMutation(11, "evt-dup"));

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
    MeetingAggregate aggregate = MakeAggregate();
    aggregate.Join(kUserAUid, "ua-session-1", MakeMutation(10, "evt-join-a-1"));

    const JoinResult result =
        aggregate.Join(kUserAUid, "ua-session-2", MakeMutation(12, "evt-join-a-2"));

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
    MeetingAggregate aggregate = MakeAggregate();
    aggregate.Join(kUserAUid, "ua-session-1", MakeMutation(10, "evt-join-a-1"));
    aggregate.Leave(kUserAUid, MakeMutation(20, "evt-leave-a-1"));
    EXPECT_EQ(aggregate.ActiveParticipantCount(), 1u);

    const JoinResult result =
        aggregate.Join(kUserAUid, "ua-session-3", MakeMutation(30, "evt-rejoin-a-1"));

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
}

TEST(MeetingAggregateJoin, MeetingNeverRegressesFromActiveToCreated)
{
    MeetingAggregate aggregate = MakeAggregate();
    aggregate.Join(kUserAUid, "ua-session-1", MakeMutation(10, "evt-join-a-1"));
    EXPECT_EQ(aggregate.State(), MeetingState::ACTIVE);

    // 所有非 Host 成员离会后仍不得回退到 CREATED。
    aggregate.Leave(kUserAUid, MakeMutation(20, "evt-leave-a-1"));
    EXPECT_EQ(aggregate.State(), MeetingState::ACTIVE);
    EXPECT_EQ(aggregate.ActiveParticipantCount(), 1u);

    // 而且新的非 Host 加入也不再触发一次"转换"。
    aggregate.Join(kUserBUid, "ub-session-1", MakeMutation(40, "evt-join-b-1"));
    EXPECT_EQ(aggregate.State(), MeetingState::ACTIVE);
}

TEST(MeetingAggregateJoin, HostAdditionalSessionDoesNotChangeStateOrCount)
{
    MeetingAggregate aggregate = MakeAggregate();

    const std::size_t count_before = aggregate.ActiveParticipantCount();
    const JoinResult result =
        aggregate.Join(kHostUid, "host-session-2", MakeMutation(5, "evt-host-join-2"));

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
    MeetingAggregate aggregate = MakeAggregate();

    const JoinResult result =
        aggregate.Join(kHostUid, "host-session-1", MakeMutation(5, "evt-host-dup"));

    EXPECT_EQ(result.result_code, ResultCode::ALREADY_JOINED);
    EXPECT_EQ(result.disposition, ResultDisposition::IDEMPOTENT);
    EXPECT_EQ(result.outcome, JoinOutcome::ALREADY_BOUND);
    EXPECT_FALSE(result.has_event);
    EXPECT_EQ(aggregate.State(), MeetingState::CREATED);
    EXPECT_EQ(aggregate.ActiveParticipantCount(), 1u);
}

TEST(MeetingAggregateJoin, JoinDuringEndingIsRejectedWithoutMutation)
{
    MeetingAggregate aggregate = MakeAggregate();
    aggregate.Join(kUserAUid, "ua-session-1", MakeMutation(10, "evt-join-a-1"));
    const BeginCloseResult close =
        aggregate.BeginClose(kHostUid, "req-close-1", MakeMutation(50, "evt-unused"));
    ASSERT_EQ(close.result_code, ResultCode::OK);

    const std::size_t count_before = aggregate.ActiveParticipantCount();
    const JoinResult result =
        aggregate.Join(kUserBUid, "ub-session-1", MakeMutation(60, "evt-join-b-1"));

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
    MeetingAggregate aggregate = MakeAggregate();
    aggregate.Join(kUserAUid, "ua-session-1", MakeMutation(10, "evt-join-a-1"));
    aggregate.Join(kUserAUid, "ua-session-2", MakeMutation(11, "evt-join-a-2"));
    ASSERT_EQ(aggregate.BoundSessionCount(kUserAUid), 2u);

    const LeaveResult result = aggregate.Leave(kUserAUid, MakeMutation(20, "evt-leave-a-1"));

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
}

TEST(MeetingAggregateLeave, DuplicateLeaveIsIdempotentWithNoEvent)
{
    MeetingAggregate aggregate = MakeAggregate();
    aggregate.Join(kUserAUid, "ua-session-1", MakeMutation(10, "evt-join-a-1"));
    aggregate.Leave(kUserAUid, MakeMutation(20, "evt-leave-a-1"));

    const std::size_t count_before = aggregate.ActiveParticipantCount();
    const LeaveResult result =
        aggregate.Leave(kUserAUid, MakeMutation(21, "evt-leave-dup"));

    EXPECT_EQ(result.result_code, ResultCode::ALREADY_LEFT);
    EXPECT_EQ(result.disposition, ResultDisposition::IDEMPOTENT);
    EXPECT_FALSE(result.has_event);
    EXPECT_EQ(aggregate.ActiveParticipantCount(), count_before);
}

TEST(MeetingAggregateLeave, NeverParticipantIsNotParticipant)
{
    MeetingAggregate aggregate = MakeAggregate();

    const LeaveResult result = aggregate.Leave(kUserBUid, MakeMutation(20, "evt-leave-b"));

    EXPECT_EQ(result.result_code, ResultCode::NOT_PARTICIPANT);
    EXPECT_EQ(result.disposition, ResultDisposition::ERROR);
    EXPECT_FALSE(result.has_event);
    EXPECT_EQ(aggregate.ActiveParticipantCount(), 1u);
}

TEST(MeetingAggregateLeave, HostLeavingOpenMeetingMustCloseInstead)
{
    MeetingAggregate aggregate = MakeAggregate();
    aggregate.Join(kUserAUid, "ua-session-1", MakeMutation(10, "evt-join-a-1"));

    const std::size_t count_before = aggregate.ActiveParticipantCount();
    const LeaveResult result = aggregate.Leave(kHostUid, MakeMutation(20, "evt-host-leave"));

    EXPECT_EQ(result.result_code, ResultCode::HOST_MUST_CLOSE_MEETING);
    EXPECT_EQ(result.disposition, ResultDisposition::ERROR);
    EXPECT_FALSE(result.has_event);

    // 不得解绑 Host、不得减成员数、不得改变 Host 的 ParticipantState。
    EXPECT_EQ(aggregate.ActiveParticipantCount(), count_before);
    EXPECT_TRUE(aggregate.IsSessionBound(kHostUid, "host-session-1"));
    ParticipantView host;
    ASSERT_TRUE(aggregate.TryGetParticipantView(kHostUid, host));
    EXPECT_EQ(host.participant_state, ParticipantState::ACTIVE);
    EXPECT_TRUE(aggregate.State() == MeetingState::ACTIVE);
}

TEST(MeetingAggregateLeave, LeaveDuringEndingIsRejectedForEveryone)
{
    MeetingAggregate aggregate = MakeAggregate();
    aggregate.Join(kUserAUid, "ua-session-1", MakeMutation(10, "evt-join-a-1"));
    aggregate.BeginClose(kHostUid, "req-close-1", MakeMutation(50, "evt-unused"));

    const std::size_t count_before = aggregate.ActiveParticipantCount();

    const LeaveResult member_result =
        aggregate.Leave(kUserAUid, MakeMutation(60, "evt-leave-a-1"));
    EXPECT_EQ(member_result.result_code, ResultCode::MEETING_STATE_REJECTED);
    EXPECT_EQ(member_result.disposition, ResultDisposition::ERROR);
    EXPECT_FALSE(member_result.has_event);

    // 包括 Host。
    const LeaveResult host_result =
        aggregate.Leave(kHostUid, MakeMutation(61, "evt-host-leave"));
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
    MeetingAggregate aggregate = MakeAggregate();
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

    // 不产生 MeetingClosed：FinalizeClose 属于 M3。
    // BeginCloseResult 不携带事件，因此这里断言"没有可供发布的终态事件"。
    EXPECT_EQ(aggregate.BuildMeetingView().meeting_state, MeetingState::ENDING);
}

TEST(MeetingAggregateBeginClose, HostClosesFromActive)
{
    MeetingAggregate aggregate = MakeAggregate();
    aggregate.Join(kUserAUid, "ua-session-1", MakeMutation(10, "evt-join-a-1"));
    ASSERT_EQ(aggregate.State(), MeetingState::ACTIVE);

    const BeginCloseResult result =
        aggregate.BeginClose(kHostUid, "req-close-1", MakeMutation(50, "evt-unused"));

    EXPECT_EQ(result.result_code, ResultCode::OK);
    EXPECT_TRUE(result.should_schedule_finalize);
    EXPECT_EQ(aggregate.State(), MeetingState::ENDING);
}

TEST(MeetingAggregateBeginClose, SecondHostCloseIsIdempotentAndDoesNotReschedule)
{
    MeetingAggregate aggregate = MakeAggregate();
    aggregate.Join(kUserAUid, "ua-session-1", MakeMutation(10, "evt-join-a-1"));
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
    MeetingAggregate from_created = MakeAggregate();
    const BeginCloseResult created_result =
        from_created.BeginClose(kUserAUid, "req-close-x", MakeMutation(50, "evt-unused"));
    EXPECT_EQ(created_result.result_code, ResultCode::PERMISSION_DENIED);
    EXPECT_EQ(created_result.disposition, ResultDisposition::ERROR);
    EXPECT_FALSE(created_result.should_schedule_finalize);
    EXPECT_FALSE(created_result.has_close_context);
    EXPECT_EQ(from_created.State(), MeetingState::CREATED);

    MeetingAggregate from_active = MakeAggregate();
    from_active.Join(kUserAUid, "ua-session-1", MakeMutation(10, "evt-join-a-1"));
    from_active.Join(kUserBUid, "ub-session-1", MakeMutation(11, "evt-join-b-1"));

    const BeginCloseResult active_result =
        from_active.BeginClose(kUserBUid, "req-close-y", MakeMutation(50, "evt-unused"));
    EXPECT_EQ(active_result.result_code, ResultCode::PERMISSION_DENIED);
    EXPECT_FALSE(active_result.has_close_context);
    EXPECT_EQ(from_active.State(), MeetingState::ACTIVE); // 未被非 Host 改变
}

TEST(MeetingAggregateBeginClose, NonHostIsDeniedEvenWhenMeetingIsEnding)
{
    MeetingAggregate aggregate = MakeAggregate();
    aggregate.Join(kUserAUid, "ua-session-1", MakeMutation(10, "evt-join-a-1"));
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
    MeetingAggregate aggregate = MakeAggregate();
    aggregate.Join(kUserAUid, "ua-session-1", MakeMutation(10, "evt-join-a-1"));
    aggregate.Join(kUserAUid, "ua-session-2", MakeMutation(11, "evt-join-a-2"));
    aggregate.Join(kUserBUid, "ub-session-1", MakeMutation(12, "evt-join-b-1"));

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
    MeetingAggregate aggregate = MakeAggregate();
    // Host ACTIVE, A 加入后 LEFT, B ACTIVE。
    aggregate.Join(kUserAUid, "ua-session-1", MakeMutation(10, "evt-join-a-1"));
    aggregate.Join(kUserBUid, "ub-session-1", MakeMutation(11, "evt-join-b-1"));
    aggregate.Leave(kUserAUid, MakeMutation(20, "evt-leave-a-1"));
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
    MeetingAggregate aggregate = MakeAggregate();
    aggregate.Join(kUserAUid, "ua-session-1", MakeMutation(10, "evt-join-a-1"));
    aggregate.Join(kUserBUid, "ub-session-1", MakeMutation(11, "evt-join-b-1"));
    aggregate.Leave(kUserAUid, MakeMutation(20, "evt-leave-a-1"));

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
    MeetingAggregate aggregate = MakeAggregate();
    aggregate.Join(kUserAUid, "ua-session-1", MakeMutation(10, "evt-join-a-1"));
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
// Determinism
// ==============================================================================

TEST(MeetingAggregateDeterminism, OutputsUseCallerSuppliedTimeAndEventIds)
{
    // 同一串输入两次必须得到完全相同的可观测输出，且全部时间/ID 都来自调用方。
    const Timestamp t_join = AtSeconds(1234);
    const Timestamp t_leave = AtSeconds(5678);
    const Timestamp t_close = AtSeconds(9012);

    MeetingAggregate first = MakeAggregate();
    const JoinResult first_join = first.Join(kUserAUid, "ua-s1", MakeMutation(1234, "E-J"));
    const LeaveResult first_leave = first.Leave(kUserAUid, MakeMutation(5678, "E-L"));
    const BeginCloseResult first_close =
        first.BeginClose(kHostUid, "R-C", MakeMutation(9012, "E-X"));

    MeetingAggregate second = MakeAggregate();
    const JoinResult second_join = second.Join(kUserAUid, "ua-s1", MakeMutation(1234, "E-J"));
    const LeaveResult second_leave = second.Leave(kUserAUid, MakeMutation(5678, "E-L"));
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
    EXPECT_EQ(first_leave.event.event_id, second_leave.event.event_id);
    EXPECT_EQ(first_close.close_context.close_started_at,
              second_close.close_context.close_started_at);
    EXPECT_EQ(first_close.close_context.originating_request_id,
              second_close.close_context.originating_request_id);
    EXPECT_EQ(first_close.close_context.frozen_active_participant_count,
              second_close.close_context.frozen_active_participant_count);
}

TEST(MeetingAggregateDeterminism, UnusedEventIdHasNoEffectOnRejectedPaths)
{
    MeetingAggregate aggregate = MakeAggregate();

    // 被拒绝的路径：调用方提供了 event_id，但不允许产生任何副作用。
    const LeaveResult never = aggregate.Leave(kUserBUid, MakeMutation(20, "E-UNUSED"));
    EXPECT_EQ(never.result_code, ResultCode::NOT_PARTICIPANT);
    EXPECT_FALSE(never.has_event);

    const BeginCloseResult denied =
        aggregate.BeginClose(kUserBUid, "R-DENIED", MakeMutation(21, "E-UNUSED-2"));
    EXPECT_EQ(denied.result_code, ResultCode::PERMISSION_DENIED);
    EXPECT_FALSE(denied.has_close_context);
    EXPECT_FALSE(denied.should_schedule_finalize);

    // 聚合状态保持初始形态。
    EXPECT_EQ(aggregate.State(), MeetingState::CREATED);
    EXPECT_EQ(aggregate.ActiveParticipantCount(), 1u);
    EXPECT_EQ(aggregate.ListParticipantViews().size(), 1u);
}

// ==============================================================================
// Invariants
// ==============================================================================

TEST(MeetingAggregateInvariants, CountEqualsActiveParticipantsAndLeftAreRetained)
{
    MeetingAggregate aggregate = MakeAggregate();
    aggregate.Join(kUserAUid, "ua-s1", MakeMutation(10, "E1"));
    aggregate.Join(kUserBUid, "ub-s1", MakeMutation(11, "E2"));
    aggregate.Leave(kUserAUid, MakeMutation(20, "E3"));
    aggregate.Join(kUserAUid, "ua-s2", MakeMutation(30, "E4")); // Re-Join

    // 同一 User 只有一个逻辑 Participant（I7）：历史记录被复用而非新建。
    const std::vector<ParticipantView> views = aggregate.ListParticipantViews();
    std::size_t count_a = 0;
    std::size_t active_count = 0;
    for (std::size_t i = 0; i < views.size(); ++i) {
        if (views[i].user_id == kUserAUid) {
            ++count_a;
        }
        if (views[i].participant_state == ParticipantState::ACTIVE) {
            ++active_count;
        }
    }
    EXPECT_EQ(count_a, 1u);
    EXPECT_EQ(views.size(), 3u); // Host + A + B

    // I10：active_participant_count 等于 ACTIVE Participant 数量。
    EXPECT_EQ(aggregate.ActiveParticipantCount(), active_count);
    EXPECT_EQ(aggregate.ActiveParticipantCount(), 3u);

    // I3：ACTIVE 非 Host Participant 至少有一条 BOUND Binding。
    for (std::size_t i = 0; i < views.size(); ++i) {
        if (views[i].participant_state == ParticipantState::ACTIVE &&
            views[i].role == ParticipantRole::PARTICIPANT) {
            EXPECT_GE(aggregate.BoundSessionCount(views[i].user_id), 1u);
        }
    }
}

TEST(MeetingAggregateInvariants, ActiveNonHostKeepsAtLeastOneBoundSessionAcrossRebinds)
{
    MeetingAggregate aggregate = MakeAggregate();
    aggregate.Join(kUserAUid, "ua-s1", MakeMutation(10, "E1"));
    aggregate.Join(kUserAUid, "ua-s2", MakeMutation(11, "E2"));

    EXPECT_EQ(aggregate.BoundSessionCount(kUserAUid), 2u);

    // 离会后全部解绑；Re-Join 用新 session 重新满足 I3。
    aggregate.Leave(kUserAUid, MakeMutation(20, "E3"));
    EXPECT_EQ(aggregate.BoundSessionCount(kUserAUid), 0u);

    aggregate.Join(kUserAUid, "ua-s3", MakeMutation(30, "E4"));
    EXPECT_EQ(aggregate.BoundSessionCount(kUserAUid), 1u);
    EXPECT_TRUE(aggregate.IsSessionBound(kUserAUid, "ua-s3"));
    EXPECT_FALSE(aggregate.IsSessionBound(kUserAUid, "ua-s1")); // 旧 binding 未复活
}
