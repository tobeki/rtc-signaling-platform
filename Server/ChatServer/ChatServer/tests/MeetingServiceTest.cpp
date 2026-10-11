// ==============================================================================
// M3 MeetingService 单元测试（Phase 1 Layer A：Pure Domain）。
//
// 覆盖范围（Step 1.6 §19 的 57 个语义用例 + §11.3 的顺序要求）：
//   Create C1–C7 / Join J1–J9 / Leave L1–L11 / Disconnect D1–D8 /
//   Close X1–X11 / Query Q1–Q6 / Locality·Ordering O1–O5，
//   外加 domain session 生命周期、反向索引唯一性与事件契约回归。
//
// 全部测试使用注入的确定性来源：固定 meeting-id / event-id 序列与固定时间点，
// 绝不依赖真实 system_clock、随机 UUID 或全局状态。因此可重复、可比较。
//
// 刻意不测试的（属于 M4/M5/M6）：
//   - LogicSystem 队列与 unified work item 调度；
//   - CSession / CServer / TCP / Protocol V2；
//   - 通知发送（M6）：本文件只断言 FinalizeClose 输出的 notification intent。
// ==============================================================================

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <string>
#include <vector>

#include "meeting/MeetingService.h"

namespace {

using meeting::BindingRef;
using meeting::CloseMeetingResult;
using meeting::ClosedMeetingSnapshot;
using meeting::CreateMeetingResult;
using meeting::DomainSessionState;
using meeting::EventId;
using meeting::EventType;
using meeting::FinalizeCloseResult;
using meeting::JoinMeetingResult;
using meeting::JoinOutcome;
using meeting::LeaveMeetingResult;
using meeting::LeaveOutcome;
using meeting::ListParticipantsResult;
using meeting::MeetingEvent;
using meeting::MeetingId;
using meeting::MeetingRequestContext;
using meeting::MeetingService;
using meeting::MeetingServiceDependencies;
using meeting::MeetingServiceResultCommon;
using meeting::MeetingState;
using meeting::MeetingView;
using meeting::ParticipantIdentitySnapshot;
using meeting::ParticipantRole;
using meeting::ParticipantState;
using meeting::ParticipantView;
using meeting::ParticipantViewScope;
using meeting::QueryMeetingResult;
using meeting::RequestId;
using meeting::ResultCode;
using meeting::ResultDisposition;
using meeting::ResultDispositionFor;
using meeting::ServerId;
using meeting::SessionDisconnectedResult;
using meeting::SessionId;
using meeting::Timestamp;
using meeting::UserId;

// ------------------------------------------------------------------------------
// 固定时间点：从 epoch 起按秒偏移，便于断言"输出就是调用方给的值"。
// ------------------------------------------------------------------------------
Timestamp AtSeconds(int seconds) {
    return Timestamp(std::chrono::seconds(seconds));
}

const UserId kHostUid = 1000;
const UserId kHost2Uid = 1001;
const UserId kHost3Uid = 1002;
const UserId kUserAUid = 2001;
const UserId kUserBUid = 2002;
const UserId kUserCUid = 2003;
const UserId kStrangerUid = 3000;

const char* kLocalServer = "chatserver-local";
const char* kRemoteServer = "chatserver-remote";

// ------------------------------------------------------------------------------
// ServiceFixture：注入确定性 MeetingId / EventId / Timestamp，并可强制指定
// 下一个 ID（用于构造 Create 的 ID 冲突场景）。
// ------------------------------------------------------------------------------
class ServiceFixture {
public:
    explicit ServiceFixture(const std::string& local_server_id = kLocalServer)
        : local_server_id_(local_server_id),
          service_(local_server_id, MakeDependencies()) {
    }

    MeetingService& service() { return service_; }

    const std::string& local_server_id() const { return local_server_id_; }

    // 时钟完全由测试控制：不推进、不读真实 system_clock。
    void SetNow(int seconds) { now_seconds_ = seconds; }
    Timestamp Now() const { return AtSeconds(now_seconds_); }

    const std::vector<MeetingId>& generated_meeting_ids() const {
        return generated_meeting_ids_;
    }
    const std::vector<EventId>& generated_event_ids() const {
        return generated_event_ids_;
    }

    void ForceNextMeetingId(const MeetingId& id) {
        forced_meeting_ids_.push_back(id);
    }

    // 注册 + 认证一步到位，供"已认证 Session"场景使用。
    void RegisterAuthenticated(const SessionId& session_id, UserId user_id) {
        service_.RegisterConnectedSession(session_id);
        service_.AuthenticateSession(session_id, user_id);
    }

    MeetingRequestContext Context(const SessionId& session_id, UserId user_id,
                                  const RequestId& request_id) const {
        MeetingRequestContext context;
        context.request_id = request_id;
        context.authenticated_user_id = user_id;
        context.session_id = session_id;
        context.handling_chat_server_id = local_server_id_;
        return context;
    }

private:
    MeetingServiceDependencies MakeDependencies() {
        MeetingServiceDependencies dependencies;

        dependencies.next_meeting_id = [this]() {
            MeetingId id;
            if (!forced_meeting_ids_.empty()) {
                id = forced_meeting_ids_.front();
                forced_meeting_ids_.erase(forced_meeting_ids_.begin());
            } else {
                id = std::string("meeting-") + std::to_string(++meeting_seq_);
            }
            generated_meeting_ids_.push_back(id);
            return id;
        };

        dependencies.next_event_id = [this]() {
            const EventId id = std::string("event-") + std::to_string(++event_seq_);
            generated_event_ids_.push_back(id);
            return id;
        };

        dependencies.now = [this]() { return AtSeconds(now_seconds_); };
        return dependencies;
    }

    std::string local_server_id_;
    int now_seconds_ = 0;
    int meeting_seq_ = 0;
    int event_seq_ = 0;
    std::vector<MeetingId> forced_meeting_ids_;
    std::vector<MeetingId> generated_meeting_ids_;
    std::vector<EventId> generated_event_ids_;
    MeetingService service_;
};

// ------------------------------------------------------------------------------
// 常用准备步骤
// ------------------------------------------------------------------------------

// 注册并认证 host session 后创建 Meeting，返回 CreateMeetingResult。
CreateMeetingResult CreateMeetingFor(ServiceFixture& fixture,
                                    const SessionId& host_session,
                                    UserId host_uid = kHostUid,
                                    const RequestId& request_id = "req-create-1",
                                    int now_seconds = 1) {
    fixture.RegisterAuthenticated(host_session, host_uid);
    fixture.SetNow(now_seconds);
    return fixture.service().CreateMeeting(
        fixture.Context(host_session, host_uid, request_id));
}

JoinMeetingResult JoinAs(ServiceFixture& fixture, const SessionId& session_id, UserId uid,
                         const MeetingId& meeting_id, const RequestId& request_id) {
    return fixture.service().JoinMeeting(
        fixture.Context(session_id, uid, request_id), meeting_id);
}

LeaveMeetingResult LeaveAs(ServiceFixture& fixture, const SessionId& session_id, UserId uid,
                           const MeetingId& meeting_id, const RequestId& request_id) {
    return fixture.service().LeaveMeeting(
        fixture.Context(session_id, uid, request_id), meeting_id);
}

CloseMeetingResult CloseAs(ServiceFixture& fixture, const SessionId& session_id, UserId uid,
                           const MeetingId& meeting_id, const RequestId& request_id) {
    return fixture.service().CloseMeeting(
        fixture.Context(session_id, uid, request_id), meeting_id);
}

QueryMeetingResult QueryAs(ServiceFixture& fixture, const SessionId& session_id, UserId uid,
                           const MeetingId& meeting_id, const RequestId& request_id) {
    return fixture.service().QueryMeeting(
        fixture.Context(session_id, uid, request_id), meeting_id);
}

ListParticipantsResult ListAs(ServiceFixture& fixture, const SessionId& session_id, UserId uid,
                              const MeetingId& meeting_id, const RequestId& request_id) {
    return fixture.service().ListParticipants(
        fixture.Context(session_id, uid, request_id), meeting_id);
}

bool FindParticipantView(const std::vector<ParticipantView>& views, UserId uid,
                         ParticipantView& out) {
    for (std::size_t i = 0; i < views.size(); ++i) {
        if (views[i].user_id == uid) {
            out = views[i];
            return true;
        }
    }
    return false;
}

ParticipantState ParticipantStateOf(const ListParticipantsResult& listed, UserId uid,
                                    bool& found) {
    ParticipantView view;
    found = FindParticipantView(listed.participants, uid, view);
    return found ? view.participant_state : ParticipantState::ACTIVE;
}

} // namespace

// ==============================================================================
// MeetingServiceSession —— 领域 Session 生命周期与 tombstone
// ==============================================================================

TEST(MeetingServiceSession, ConnectedSessionIsRejectedWithAuthRequired)
{
    ServiceFixture fixture;
    fixture.service().RegisterConnectedSession("s-host");
    fixture.SetNow(1);

    const CreateMeetingResult result = fixture.service().CreateMeeting(
        fixture.Context("s-host", kHostUid, "req-1"));

    EXPECT_EQ(result.common.result_code, ResultCode::AUTH_REQUIRED);
    EXPECT_EQ(result.common.result_name, std::string("AUTH_REQUIRED"));
    EXPECT_EQ(result.common.disposition, ResultDisposition::ERROR);
    EXPECT_TRUE(result.events.empty());
    EXPECT_EQ(fixture.generated_meeting_ids().size(), 0u);
}

TEST(MeetingServiceSession, AuthenticatedSessionIsAllowed)
{
    ServiceFixture fixture;
    fixture.service().RegisterConnectedSession("s-host");

    EXPECT_TRUE(fixture.service().AuthenticateSession("s-host", kHostUid));

    DomainSessionState state = DomainSessionState::CONNECTED;
    EXPECT_TRUE(fixture.service().TryGetDomainSessionState("s-host", state));
    EXPECT_EQ(state, DomainSessionState::AUTHENTICATED);

    const CreateMeetingResult result = CreateMeetingFor(fixture, "s-host");
    EXPECT_EQ(result.common.result_code, ResultCode::OK);
}

TEST(MeetingServiceSession, RegisterConnectedSessionIsIdempotentAndDoesNotResetState)
{
    ServiceFixture fixture;
    EXPECT_TRUE(fixture.service().RegisterConnectedSession("s-host"));
    EXPECT_TRUE(fixture.service().AuthenticateSession("s-host", kHostUid));

    // 重复注册不得把已认证的 Session 打回 CONNECTED。
    EXPECT_FALSE(fixture.service().RegisterConnectedSession("s-host"));

    DomainSessionState state = DomainSessionState::CONNECTED;
    EXPECT_TRUE(fixture.service().TryGetDomainSessionState("s-host", state));
    EXPECT_EQ(state, DomainSessionState::AUTHENTICATED);
}

TEST(MeetingServiceSession, RegisterConnectedSessionRejectsEmptyId)
{
    ServiceFixture fixture;
    EXPECT_FALSE(fixture.service().RegisterConnectedSession(""));
}

TEST(MeetingServiceSession, AuthenticateSessionRejectsUnknownSession)
{
    ServiceFixture fixture;
    EXPECT_FALSE(fixture.service().AuthenticateSession("s-unknown", kHostUid));
}

TEST(MeetingServiceSession, DuplicateAuthenticationDoesNotChangeState)
{
    ServiceFixture fixture;
    fixture.service().RegisterConnectedSession("s-host");
    EXPECT_TRUE(fixture.service().AuthenticateSession("s-host", kHostUid));

    // Step 1.3 §5.3：重复认证视为无效，不改状态（身份也不被覆盖）。
    EXPECT_FALSE(fixture.service().AuthenticateSession("s-host", kUserAUid));

    const CreateMeetingResult result = CreateMeetingFor(fixture, "s-host");
    EXPECT_EQ(result.common.result_code, ResultCode::OK);
    EXPECT_EQ(result.host_participant.user_id, kHostUid);
}

TEST(MeetingServiceSession, AuthenticateSessionRejectsNonPositiveUserId)
{
    ServiceFixture fixture;
    fixture.service().RegisterConnectedSession("s-host");
    EXPECT_FALSE(fixture.service().AuthenticateSession("s-host", 0));

    const CreateMeetingResult result = fixture.service().CreateMeeting(
        fixture.Context("s-host", 0, "req-1"));
    EXPECT_EQ(result.common.result_code, ResultCode::AUTH_REQUIRED);
}

TEST(MeetingServiceSession, TryGetDomainSessionStateFailsForUnknownSession)
{
    ServiceFixture fixture;
    DomainSessionState state = DomainSessionState::AUTHENTICATED;
    EXPECT_FALSE(fixture.service().TryGetDomainSessionState("s-unknown", state));
}

TEST(MeetingServiceSession, ClosedSessionIsRejectedWithSessionStateRejected)
{
    ServiceFixture fixture;
    // 这个 Session 不加入任何 Meeting：断开时 fan-out 为 0。
    fixture.RegisterAuthenticated("s-user", kUserAUid);

    const SessionDisconnectedResult disconnected =
        fixture.service().SessionDisconnected("s-user");
    EXPECT_TRUE(disconnected.session_known);
    EXPECT_FALSE(disconnected.already_closed);
    EXPECT_EQ(disconnected.affected_meeting_count, 0u);
    EXPECT_EQ(disconnected.final_state, DomainSessionState::CLOSED);

    DomainSessionState state = DomainSessionState::CONNECTED;
    EXPECT_TRUE(fixture.service().TryGetDomainSessionState("s-user", state));
    EXPECT_EQ(state, DomainSessionState::CLOSED);

    // tombstone 的作用：已 CLOSED 的 Session 必须得到 SESSION_STATE_REJECTED，
    // 而不是被误报成 AUTH_REQUIRED。
    const CreateMeetingResult result = fixture.service().CreateMeeting(
        fixture.Context("s-user", kUserAUid, "req-2"));
    EXPECT_EQ(result.common.result_code, ResultCode::SESSION_STATE_REJECTED);
    EXPECT_EQ(result.common.result_name, std::string("SESSION_STATE_REJECTED"));
}

TEST(MeetingServiceSession, DuplicateSessionDisconnectedIsIdempotent)
{
    ServiceFixture fixture;
    fixture.RegisterAuthenticated("s-user", kUserAUid);

    EXPECT_TRUE(fixture.service().SessionDisconnected("s-user").session_known);

    const SessionDisconnectedResult second =
        fixture.service().SessionDisconnected("s-user");
    EXPECT_TRUE(second.session_known);
    EXPECT_TRUE(second.already_closed);
    EXPECT_EQ(second.affected_meeting_count, 0u);
    EXPECT_TRUE(second.events.empty());
    EXPECT_EQ(second.final_state, DomainSessionState::CLOSED);
}

TEST(MeetingServiceSession, SessionDisconnectedForUnknownSessionIsReported)
{
    ServiceFixture fixture;
    const SessionDisconnectedResult disconnected =
        fixture.service().SessionDisconnected("s-unknown");
    EXPECT_FALSE(disconnected.session_known);
    EXPECT_FALSE(disconnected.already_closed);
    EXPECT_TRUE(disconnected.events.empty());
}

TEST(MeetingServiceSession, SessionClosedOnlyAfterAllFanOutCleanup)
{
    ServiceFixture fixture;
    const MeetingId m1 =
        CreateMeetingFor(fixture, "s-host-1", kHostUid, "req-c1").meeting.meeting_id;
    const MeetingId m2 =
        CreateMeetingFor(fixture, "s-host-2", kHost2Uid, "req-c2").meeting.meeting_id;
    fixture.RegisterAuthenticated("s-user", kUserAUid);
    EXPECT_EQ(JoinAs(fixture, "s-user", kUserAUid, m1, "req-j1").common.result_code,
              ResultCode::OK);
    EXPECT_EQ(JoinAs(fixture, "s-user", kUserAUid, m2, "req-j2").common.result_code,
              ResultCode::OK);
    EXPECT_EQ(fixture.service().ReverseBindingCountForSession("s-user"), 2u);

    const SessionDisconnectedResult disconnected =
        fixture.service().SessionDisconnected("s-user");

    // Session 进入终态时，该 session 在任何 Meeting 中都不再留下 ref：
    // "部分 Meeting 已清理但 Session 已 CLOSED" 不可能出现。
    DomainSessionState state = DomainSessionState::CONNECTED;
    EXPECT_TRUE(fixture.service().TryGetDomainSessionState("s-user", state));
    EXPECT_EQ(state, DomainSessionState::CLOSED);
    EXPECT_EQ(fixture.service().ReverseBindingCountForSession("s-user"), 0u);
    EXPECT_EQ(disconnected.final_state, DomainSessionState::CLOSED);
}

TEST(MeetingServiceSession, ContextWithEmptyRequestIdIsInvalid)
{
    ServiceFixture fixture;
    fixture.RegisterAuthenticated("s-host", kHostUid);

    MeetingRequestContext context = fixture.Context("s-host", kHostUid, "");
    const CreateMeetingResult result = fixture.service().CreateMeeting(context);

    EXPECT_EQ(result.common.result_code, ResultCode::INVALID_ARGUMENT);
    EXPECT_EQ(result.common.result_name, std::string("INVALID_ARGUMENT"));
}

TEST(MeetingServiceSession, ContextWithForeignHandlingServerIsInvalid)
{
    ServiceFixture fixture;
    fixture.RegisterAuthenticated("s-host", kHostUid);

    MeetingRequestContext context = fixture.Context("s-host", kHostUid, "req-1");
    context.handling_chat_server_id = kRemoteServer;
    const CreateMeetingResult result = fixture.service().CreateMeeting(context);

    EXPECT_EQ(result.common.result_code, ResultCode::INVALID_ARGUMENT);
}

TEST(MeetingServiceSession, ContextIdentityContradictingDomainRecordIsInvalid)
{
    ServiceFixture fixture;
    fixture.RegisterAuthenticated("s-host", kHostUid);

    // server-built context 声称的身份与领域记录不一致：不得信任 context。
    const CreateMeetingResult result = fixture.service().CreateMeeting(
        fixture.Context("s-host", kUserAUid, "req-1"));
    EXPECT_EQ(result.common.result_code, ResultCode::INVALID_ARGUMENT);
}

TEST(MeetingServiceSession, ContextWithEmptyDeviceIdButPresenceFlagIsInvalid)
{
    ServiceFixture fixture;
    fixture.RegisterAuthenticated("s-host", kHostUid);

    MeetingRequestContext context = fixture.Context("s-host", kHostUid, "req-1");
    context.has_device_id = true;
    context.device_id = "";
    EXPECT_EQ(fixture.service().CreateMeeting(context).common.result_code,
              ResultCode::INVALID_ARGUMENT);
}

TEST(MeetingServiceSession, ContextWithEmptyTrustedOwnerButPresenceFlagIsInvalid)
{
    ServiceFixture fixture;
    fixture.RegisterAuthenticated("s-host", kHostUid);

    MeetingRequestContext context = fixture.Context("s-host", kHostUid, "req-1");
    context.has_trusted_owner_chat_server_id = true;
    context.trusted_owner_chat_server_id = "";
    EXPECT_EQ(fixture.service().CreateMeeting(context).common.result_code,
              ResultCode::INVALID_ARGUMENT);
}

TEST(MeetingServiceSession, TwoInstancesHaveIndependentRegistries)
{
    // 禁止 Singleton：两个实例互不共享内存 registry。
    ServiceFixture first;
    ServiceFixture second;

    const MeetingId created =
        CreateMeetingFor(first, "s-host", kHostUid, "req-c1").meeting.meeting_id;
    EXPECT_TRUE(first.service().HasActiveMeeting(created));
    EXPECT_FALSE(second.service().HasActiveMeeting(created));
}

// ==============================================================================
// MeetingServiceCreate —— C1–C7 与 ID 冲突
// ==============================================================================

TEST(MeetingServiceCreate, C1AuthenticatedCreateProducesCreatedState)
{
    ServiceFixture fixture;
    const CreateMeetingResult result = CreateMeetingFor(fixture, "s-host");

    EXPECT_EQ(result.common.result_code, ResultCode::OK);
    EXPECT_EQ(result.common.result_name, std::string("OK"));
    EXPECT_EQ(result.common.disposition, ResultDisposition::SUCCESS);
    EXPECT_EQ(result.common.request_id, std::string("req-create-1"));
    EXPECT_EQ(result.meeting.meeting_state, MeetingState::CREATED);
    EXPECT_FALSE(result.meeting.has_closed_at);
    EXPECT_EQ(result.meeting.created_at, AtSeconds(1));
    EXPECT_EQ(result.meeting.owner_chat_server_id, std::string(kLocalServer));
    EXPECT_TRUE(fixture.service().HasActiveMeeting(result.meeting.meeting_id));
    EXPECT_FALSE(fixture.service().HasClosedSnapshot(result.meeting.meeting_id));
}

TEST(MeetingServiceCreate, C2HostParticipantIsActiveHost)
{
    ServiceFixture fixture;
    const CreateMeetingResult result = CreateMeetingFor(fixture, "s-host");

    EXPECT_EQ(result.host_participant.user_id, kHostUid);
    EXPECT_EQ(result.host_participant.role, ParticipantRole::HOST);
    EXPECT_EQ(result.host_participant.participant_state, ParticipantState::ACTIVE);
    EXPECT_FALSE(result.host_participant.has_left_at);
    EXPECT_EQ(result.host_participant.joined_at, AtSeconds(1));
}

TEST(MeetingServiceCreate, C3HostSessionBindingIsRegisteredInReverseIndex)
{
    ServiceFixture fixture;
    const CreateMeetingResult result = CreateMeetingFor(fixture, "s-host");

    // 反向索引是 Binding 事实的可见代理：Host 初始 Binding 必须登记。
    EXPECT_EQ(fixture.service().ReverseBindingCountForSession("s-host"), 1u);
    EXPECT_EQ(result.meeting.active_participant_count, 1u);
}

TEST(MeetingServiceCreate, C4ActiveParticipantCountIsOne)
{
    ServiceFixture fixture;
    const CreateMeetingResult result = CreateMeetingFor(fixture, "s-host");
    EXPECT_EQ(result.meeting.active_participant_count, std::size_t(1));
}

TEST(MeetingServiceCreate, C5MeetingCreatedExactlyOnce)
{
    ServiceFixture fixture;
    const CreateMeetingResult result = CreateMeetingFor(fixture, "s-host");

    ASSERT_EQ(result.events.size(), 1u);
    const MeetingEvent& event = result.events[0];
    EXPECT_EQ(event.event_type, EventType::MEETING_CREATED);
    EXPECT_EQ(event.event_id, std::string("event-1"));
    EXPECT_EQ(event.meeting_id, result.meeting.meeting_id);
    EXPECT_EQ(event.owner_chat_server_id, std::string(kLocalServer));
    EXPECT_EQ(event.occurred_at, AtSeconds(1));
    EXPECT_EQ(event.meeting_state_after, MeetingState::CREATED);
    EXPECT_TRUE(event.has_actor_user_id);
    EXPECT_EQ(event.actor_user_id, kHostUid);
    EXPECT_FALSE(event.has_participant_user_id);
    EXPECT_TRUE(event.has_request_id);
    EXPECT_EQ(event.request_id, std::string("req-create-1"));
}

TEST(MeetingServiceCreate, C6NoParticipantJoinedIsProduced)
{
    ServiceFixture fixture;
    const CreateMeetingResult result = CreateMeetingFor(fixture, "s-host");

    for (std::size_t i = 0; i < result.events.size(); ++i) {
        EXPECT_NE(result.events[i].event_type, EventType::PARTICIPANT_JOINED);
    }
}

TEST(MeetingServiceCreate, C7TwoCreatesWithSameRequestIdProduceDifferentMeetings)
{
    ServiceFixture fixture;
    const CreateMeetingResult first =
        CreateMeetingFor(fixture, "s-host-1", kHostUid, "req-same");
    const CreateMeetingResult second =
        CreateMeetingFor(fixture, "s-host-2", kHost2Uid, "req-same");

    EXPECT_EQ(first.common.result_code, ResultCode::OK);
    EXPECT_EQ(second.common.result_code, ResultCode::OK);
    EXPECT_NE(first.meeting.meeting_id, second.meeting.meeting_id);

    ASSERT_EQ(first.events.size(), 1u);
    ASSERT_EQ(second.events.size(), 1u);
    // request_id 相同，但事件身份不同：request_id 不是幂等键。
    EXPECT_NE(first.events[0].event_id, second.events[0].event_id);
    EXPECT_EQ(first.events[0].request_id, std::string("req-same"));
    EXPECT_EQ(second.events[0].request_id, std::string("req-same"));
    EXPECT_TRUE(fixture.service().HasActiveMeeting(first.meeting.meeting_id));
    EXPECT_TRUE(fixture.service().HasActiveMeeting(second.meeting.meeting_id));
}

TEST(MeetingServiceCreate, CollisionWithActiveMeetingIsInternalError)
{
    ServiceFixture fixture;
    const CreateMeetingResult first = CreateMeetingFor(fixture, "s-host-1");
    const MeetingId existing = first.meeting.meeting_id;

    // 注入来源产出已存在的 ID：不得覆盖、不得静默返回旧 Meeting。
    fixture.ForceNextMeetingId(existing);
    fixture.RegisterAuthenticated("s-host-2", kHost2Uid);
    const CreateMeetingResult second = fixture.service().CreateMeeting(
        fixture.Context("s-host-2", kHost2Uid, "req-collide"));

    EXPECT_EQ(second.common.result_code, ResultCode::INTERNAL_ERROR);
    EXPECT_EQ(second.common.disposition, ResultDisposition::ERROR);
    EXPECT_TRUE(second.events.empty());
    EXPECT_EQ(fixture.service().ReverseBindingCountForSession("s-host-2"), 0u);
}

TEST(MeetingServiceCreate, CollisionWithClosedSnapshotIsInternalError)
{
    ServiceFixture fixture;
    const MeetingId meeting_id = CreateMeetingFor(fixture, "s-host").meeting.meeting_id;
    fixture.SetNow(2);
    ASSERT_EQ(CloseAs(fixture, "s-host", kHostUid, meeting_id, "req-close").common.result_code,
              ResultCode::OK);
    fixture.SetNow(3);
    ASSERT_TRUE(fixture.service().FinalizeClose(meeting_id).finalized);

    fixture.ForceNextMeetingId(meeting_id);
    fixture.RegisterAuthenticated("s-host-2", kHost2Uid);
    const CreateMeetingResult again = fixture.service().CreateMeeting(
        fixture.Context("s-host-2", kHost2Uid, "req-collide"));

    EXPECT_EQ(again.common.result_code, ResultCode::INTERNAL_ERROR);
    EXPECT_TRUE(fixture.service().HasClosedSnapshot(meeting_id));
}

TEST(MeetingServiceCreate, EmptyGeneratedMeetingIdIsInternalError)
{
    ServiceFixture fixture;
    fixture.ForceNextMeetingId("");
    fixture.RegisterAuthenticated("s-host", kHostUid);

    const CreateMeetingResult result = fixture.service().CreateMeeting(
        fixture.Context("s-host", kHostUid, "req-1"));
    EXPECT_EQ(result.common.result_code, ResultCode::INTERNAL_ERROR);
    EXPECT_FALSE(result.common.has_owner_chat_server_id);
}

// ==============================================================================
// MeetingServiceJoin —— J1–J9 与反向索引
// ==============================================================================

TEST(MeetingServiceJoin, J1FirstNonHostProducesNewParticipant)
{
    ServiceFixture fixture;
    const MeetingId meeting_id = CreateMeetingFor(fixture, "s-host").meeting.meeting_id;
    fixture.RegisterAuthenticated("s-user", kUserAUid);
    fixture.SetNow(5);

    const JoinMeetingResult joined =
        JoinAs(fixture, "s-user", kUserAUid, meeting_id, "req-join-1");

    EXPECT_EQ(joined.common.result_code, ResultCode::OK);
    EXPECT_EQ(joined.common.disposition, ResultDisposition::SUCCESS);
    EXPECT_EQ(joined.outcome, JoinOutcome::NEW_PARTICIPANT);
    EXPECT_EQ(joined.participant.user_id, kUserAUid);
    EXPECT_EQ(joined.participant.role, ParticipantRole::PARTICIPANT);
    EXPECT_EQ(joined.participant.participant_state, ParticipantState::ACTIVE);
    EXPECT_EQ(joined.participant.joined_at, AtSeconds(5));
    ASSERT_EQ(joined.events.size(), 1u);
    EXPECT_EQ(joined.events[0].event_type, EventType::PARTICIPANT_JOINED);
    EXPECT_EQ(joined.events[0].request_id, std::string("req-join-1"));
}

TEST(MeetingServiceJoin, J2FirstNonHostActivatesMeeting)
{
    ServiceFixture fixture;
    const MeetingId meeting_id = CreateMeetingFor(fixture, "s-host").meeting.meeting_id;
    fixture.RegisterAuthenticated("s-user", kUserAUid);

    const JoinMeetingResult joined =
        JoinAs(fixture, "s-user", kUserAUid, meeting_id, "req-join-1");

    EXPECT_EQ(joined.meeting.meeting_state, MeetingState::ACTIVE);
    EXPECT_EQ(joined.meeting.active_participant_count, std::size_t(2));
    ASSERT_EQ(joined.events.size(), 1u);
    // ParticipantJoined 的 meeting_state_after 固定为 ACTIVE（Step 1.4 §23.2）。
    EXPECT_EQ(joined.events[0].meeting_state_after, MeetingState::ACTIVE);
    EXPECT_EQ(joined.events[0].actor_user_id, kUserAUid);
    EXPECT_TRUE(joined.events[0].has_participant_user_id);
    EXPECT_EQ(joined.events[0].participant_user_id, kUserAUid);
}

TEST(MeetingServiceJoin, J3DuplicateSameSessionIsAlreadyJoined)
{
    ServiceFixture fixture;
    const MeetingId meeting_id = CreateMeetingFor(fixture, "s-host").meeting.meeting_id;
    fixture.RegisterAuthenticated("s-user", kUserAUid);
    ASSERT_EQ(JoinAs(fixture, "s-user", kUserAUid, meeting_id, "req-join-1").outcome,
              JoinOutcome::NEW_PARTICIPANT);

    const JoinMeetingResult duplicate =
        JoinAs(fixture, "s-user", kUserAUid, meeting_id, "req-join-2");

    EXPECT_EQ(duplicate.common.result_code, ResultCode::ALREADY_JOINED);
    EXPECT_EQ(duplicate.common.disposition, ResultDisposition::IDEMPOTENT);
    EXPECT_EQ(duplicate.outcome, JoinOutcome::ALREADY_BOUND);
    EXPECT_TRUE(duplicate.events.empty());
    EXPECT_EQ(duplicate.meeting.active_participant_count, std::size_t(2));
    // 重复 Join 不得把反向索引变成 [(M,U), (M,U)]。
    EXPECT_EQ(fixture.service().ReverseBindingCountForSession("s-user"), 1u);
}

TEST(MeetingServiceJoin, J4SecondSessionProducesAdditionalSessionBound)
{
    ServiceFixture fixture;
    const MeetingId meeting_id = CreateMeetingFor(fixture, "s-host").meeting.meeting_id;
    fixture.RegisterAuthenticated("s-user-1", kUserAUid);
    fixture.RegisterAuthenticated("s-user-2", kUserAUid);
    ASSERT_EQ(JoinAs(fixture, "s-user-1", kUserAUid, meeting_id, "req-join-1").outcome,
              JoinOutcome::NEW_PARTICIPANT);

    const JoinMeetingResult additional =
        JoinAs(fixture, "s-user-2", kUserAUid, meeting_id, "req-join-2");

    EXPECT_EQ(additional.common.result_code, ResultCode::OK);
    EXPECT_EQ(additional.common.disposition, ResultDisposition::SUCCESS);
    EXPECT_EQ(additional.outcome, JoinOutcome::ADDITIONAL_SESSION_BOUND);
    EXPECT_TRUE(additional.events.empty());
    EXPECT_EQ(fixture.service().ReverseBindingCountForSession("s-user-2"), 1u);
    EXPECT_EQ(fixture.service().ReverseBindingCountForSession("s-user-1"), 1u);
}

TEST(MeetingServiceJoin, J5AdditionalSessionDoesNotChangeParticipantCount)
{
    ServiceFixture fixture;
    const MeetingId meeting_id = CreateMeetingFor(fixture, "s-host").meeting.meeting_id;
    fixture.RegisterAuthenticated("s-user-1", kUserAUid);
    fixture.RegisterAuthenticated("s-user-2", kUserAUid);
    JoinAs(fixture, "s-user-1", kUserAUid, meeting_id, "req-join-1");

    const JoinMeetingResult additional =
        JoinAs(fixture, "s-user-2", kUserAUid, meeting_id, "req-join-2");
    EXPECT_EQ(additional.meeting.active_participant_count, std::size_t(2));
}

TEST(MeetingServiceJoin, J6LeftParticipantReJoinProducesRejoined)
{
    ServiceFixture fixture;
    const MeetingId meeting_id = CreateMeetingFor(fixture, "s-host").meeting.meeting_id;
    fixture.RegisterAuthenticated("s-user", kUserAUid);
    const JoinMeetingResult first =
        JoinAs(fixture, "s-user", kUserAUid, meeting_id, "req-join-1");
    ASSERT_EQ(LeaveAs(fixture, "s-user", kUserAUid, meeting_id, "req-leave-1").common.result_code,
              ResultCode::OK);
    fixture.SetNow(30);

    const JoinMeetingResult rejoined =
        JoinAs(fixture, "s-user", kUserAUid, meeting_id, "req-join-2");

    EXPECT_EQ(rejoined.common.result_code, ResultCode::OK);
    EXPECT_EQ(rejoined.outcome, JoinOutcome::REJOINED_PARTICIPANT);
    EXPECT_EQ(rejoined.participant.participant_state, ParticipantState::ACTIVE);
    EXPECT_FALSE(rejoined.participant.has_left_at);
    // joined_at 保持"首次逻辑加入"语义，不因 Re-Join 重置。
    EXPECT_EQ(rejoined.participant.joined_at, first.participant.joined_at);
    EXPECT_EQ(rejoined.meeting.active_participant_count, std::size_t(2));
}

TEST(MeetingServiceJoin, J7ReJoinProducesNewParticipantJoinedWithNewEventId)
{
    ServiceFixture fixture;
    const MeetingId meeting_id = CreateMeetingFor(fixture, "s-host").meeting.meeting_id;
    fixture.RegisterAuthenticated("s-user", kUserAUid);
    const JoinMeetingResult first =
        JoinAs(fixture, "s-user", kUserAUid, meeting_id, "req-join-1");
    LeaveAs(fixture, "s-user", kUserAUid, meeting_id, "req-leave-1");

    const JoinMeetingResult rejoined =
        JoinAs(fixture, "s-user", kUserAUid, meeting_id, "req-join-2");

    ASSERT_EQ(first.events.size(), 1u);
    ASSERT_EQ(rejoined.events.size(), 1u);
    EXPECT_EQ(rejoined.events[0].event_type, EventType::PARTICIPANT_JOINED);
    EXPECT_NE(first.events[0].event_id, rejoined.events[0].event_id);
}

TEST(MeetingServiceJoin, J8JoinEndingMeetingIsRejected)
{
    ServiceFixture fixture;
    const MeetingId meeting_id = CreateMeetingFor(fixture, "s-host").meeting.meeting_id;
    ASSERT_EQ(CloseAs(fixture, "s-host", kHostUid, meeting_id, "req-close").common.result_code,
              ResultCode::OK);
    fixture.RegisterAuthenticated("s-user", kUserAUid);

    const JoinMeetingResult joined =
        JoinAs(fixture, "s-user", kUserAUid, meeting_id, "req-join-1");

    EXPECT_EQ(joined.common.result_code, ResultCode::MEETING_STATE_REJECTED);
    EXPECT_EQ(joined.common.disposition, ResultDisposition::ERROR);
    EXPECT_EQ(joined.meeting.meeting_state, MeetingState::ENDING);
    EXPECT_TRUE(joined.events.empty());
    EXPECT_EQ(fixture.service().ReverseBindingCountForSession("s-user"), 0u);
}

TEST(MeetingServiceJoin, J9JoinClosedMeetingIsRejected)
{
    ServiceFixture fixture;
    const MeetingId meeting_id = CreateMeetingFor(fixture, "s-host").meeting.meeting_id;
    CloseAs(fixture, "s-host", kHostUid, meeting_id, "req-close");
    fixture.SetNow(10);
    ASSERT_TRUE(fixture.service().FinalizeClose(meeting_id).finalized);
    fixture.RegisterAuthenticated("s-user", kUserAUid);

    const JoinMeetingResult joined =
        JoinAs(fixture, "s-user", kUserAUid, meeting_id, "req-join-1");

    EXPECT_EQ(joined.common.result_code, ResultCode::MEETING_STATE_REJECTED);
    EXPECT_EQ(joined.meeting.meeting_state, MeetingState::CLOSED);
    EXPECT_TRUE(joined.events.empty());
    EXPECT_FALSE(fixture.service().HasActiveMeeting(meeting_id));
}

// ==============================================================================
// MeetingServiceLeave —— L1–L11 与 ENDING/CLOSED 身份区分
// ==============================================================================

TEST(MeetingServiceLeave, L1ActiveParticipantLeaveProducesLeft)
{
    ServiceFixture fixture;
    const MeetingId meeting_id = CreateMeetingFor(fixture, "s-host").meeting.meeting_id;
    fixture.RegisterAuthenticated("s-user", kUserAUid);
    JoinAs(fixture, "s-user", kUserAUid, meeting_id, "req-join-1");
    fixture.SetNow(20);

    const LeaveMeetingResult left =
        LeaveAs(fixture, "s-user", kUserAUid, meeting_id, "req-leave-1");

    EXPECT_EQ(left.common.result_code, ResultCode::OK);
    EXPECT_EQ(left.common.disposition, ResultDisposition::SUCCESS);
    EXPECT_EQ(left.outcome, LeaveOutcome::LEFT);
    EXPECT_EQ(left.meeting.active_participant_count, std::size_t(1));
    ASSERT_EQ(left.events.size(), 1u);
    EXPECT_EQ(left.events[0].event_type, EventType::PARTICIPANT_LEFT);
    EXPECT_EQ(left.events[0].occurred_at, AtSeconds(20));
    EXPECT_EQ(left.events[0].meeting_state_after, MeetingState::ACTIVE);
    EXPECT_TRUE(left.events[0].has_request_id);
    EXPECT_EQ(left.events[0].request_id, std::string("req-leave-1"));
}

TEST(MeetingServiceLeave, L2SuccessfulLeaveUnbindsAllSessionsOfThatUser)
{
    ServiceFixture fixture;
    const MeetingId meeting_id = CreateMeetingFor(fixture, "s-host").meeting.meeting_id;
    fixture.RegisterAuthenticated("s-user-1", kUserAUid);
    fixture.RegisterAuthenticated("s-user-2", kUserAUid);
    JoinAs(fixture, "s-user-1", kUserAUid, meeting_id, "req-join-1");
    ASSERT_EQ(JoinAs(fixture, "s-user-2", kUserAUid, meeting_id, "req-join-2").outcome,
              JoinOutcome::ADDITIONAL_SESSION_BOUND);

    const LeaveMeetingResult left =
        LeaveAs(fixture, "s-user-1", kUserAUid, meeting_id, "req-leave-1");
    EXPECT_EQ(left.common.result_code, ResultCode::OK);

    // 该 (meeting_id, user_id) 的全部 refs 都必须消失。
    EXPECT_EQ(fixture.service().ReverseBindingCountForSession("s-user-1"), 0u);
    EXPECT_EQ(fixture.service().ReverseBindingCountForSession("s-user-2"), 0u);

    // 行为性证据：两条 Binding 均已 UNBOUND，因此后续 disconnect 不产生任何
    // membership 变更与事件。
    const SessionDisconnectedResult first =
        fixture.service().SessionDisconnected("s-user-1");
    const SessionDisconnectedResult second =
        fixture.service().SessionDisconnected("s-user-2");
    EXPECT_EQ(first.affected_meeting_count, 0u);
    EXPECT_EQ(second.affected_meeting_count, 0u);
    EXPECT_TRUE(first.events.empty());
    EXPECT_TRUE(second.events.empty());
    EXPECT_EQ(QueryAs(fixture, "s-host", kHostUid, meeting_id, "req-q1")
                  .meeting.active_participant_count,
              std::size_t(1));
}

TEST(MeetingServiceLeave, L3ParticipantCountDecreasesExactlyOnce)
{
    ServiceFixture fixture;
    const MeetingId meeting_id = CreateMeetingFor(fixture, "s-host").meeting.meeting_id;
    fixture.RegisterAuthenticated("s-user", kUserAUid);
    JoinAs(fixture, "s-user", kUserAUid, meeting_id, "req-join-1");

    const LeaveMeetingResult left =
        LeaveAs(fixture, "s-user", kUserAUid, meeting_id, "req-leave-1");
    EXPECT_EQ(left.meeting.active_participant_count, std::size_t(1));

    // 重复 Leave 不得再次减计数。
    const LeaveMeetingResult again =
        LeaveAs(fixture, "s-user", kUserAUid, meeting_id, "req-leave-2");
    EXPECT_EQ(again.meeting.active_participant_count, std::size_t(1));
}

TEST(MeetingServiceLeave, L4DuplicateLeaveIsAlreadyLeft)
{
    ServiceFixture fixture;
    const MeetingId meeting_id = CreateMeetingFor(fixture, "s-host").meeting.meeting_id;
    fixture.RegisterAuthenticated("s-user", kUserAUid);
    JoinAs(fixture, "s-user", kUserAUid, meeting_id, "req-join-1");
    LeaveAs(fixture, "s-user", kUserAUid, meeting_id, "req-leave-1");

    const LeaveMeetingResult again =
        LeaveAs(fixture, "s-user", kUserAUid, meeting_id, "req-leave-2");

    EXPECT_EQ(again.common.result_code, ResultCode::ALREADY_LEFT);
    EXPECT_EQ(again.common.disposition, ResultDisposition::IDEMPOTENT);
    EXPECT_TRUE(again.events.empty());
}

TEST(MeetingServiceLeave, L5NeverParticipantIsNotParticipant)
{
    ServiceFixture fixture;
    const MeetingId meeting_id = CreateMeetingFor(fixture, "s-host").meeting.meeting_id;
    fixture.RegisterAuthenticated("s-stranger", kStrangerUid);

    const LeaveMeetingResult left =
        LeaveAs(fixture, "s-stranger", kStrangerUid, meeting_id, "req-leave-1");

    EXPECT_EQ(left.common.result_code, ResultCode::NOT_PARTICIPANT);
    EXPECT_EQ(left.common.disposition, ResultDisposition::ERROR);
    EXPECT_TRUE(left.events.empty());
}

TEST(MeetingServiceLeave, L6HostLeaveOnOpenMeetingMustCloseInstead)
{
    ServiceFixture fixture;
    const MeetingId meeting_id = CreateMeetingFor(fixture, "s-host").meeting.meeting_id;

    const LeaveMeetingResult left =
        LeaveAs(fixture, "s-host", kHostUid, meeting_id, "req-leave-1");

    EXPECT_EQ(left.common.result_code, ResultCode::HOST_MUST_CLOSE_MEETING);
    EXPECT_EQ(left.meeting.meeting_state, MeetingState::CREATED);
    EXPECT_EQ(left.meeting.active_participant_count, std::size_t(1));
    EXPECT_TRUE(left.events.empty());
    // Host 的 Binding 未被解除。
    EXPECT_EQ(fixture.service().ReverseBindingCountForSession("s-host"), 1u);
}

TEST(MeetingServiceLeave, L7EndingHistoricalMemberIsStateRejected)
{
    ServiceFixture fixture;
    const MeetingId meeting_id = CreateMeetingFor(fixture, "s-host").meeting.meeting_id;
    fixture.RegisterAuthenticated("s-user", kUserAUid);
    JoinAs(fixture, "s-user", kUserAUid, meeting_id, "req-join-1");
    ASSERT_EQ(CloseAs(fixture, "s-host", kHostUid, meeting_id, "req-close").common.result_code,
              ResultCode::OK);

    const LeaveMeetingResult left =
        LeaveAs(fixture, "s-user", kUserAUid, meeting_id, "req-leave-1");

    EXPECT_EQ(left.common.result_code, ResultCode::MEETING_STATE_REJECTED);
    EXPECT_TRUE(left.events.empty());
    // 成员关系已冻结：计数与 Binding 均不变。
    EXPECT_EQ(left.meeting.active_participant_count, std::size_t(2));
    EXPECT_EQ(fixture.service().ReverseBindingCountForSession("s-user"), 1u);
}

TEST(MeetingServiceLeave, L7bEndingNeverMemberIsNotParticipant)
{
    // Step 1.4 §20.2：never-participant 的判定必须先于 ENDING 状态暴露。
    ServiceFixture fixture;
    const MeetingId meeting_id = CreateMeetingFor(fixture, "s-host").meeting.meeting_id;
    ASSERT_EQ(CloseAs(fixture, "s-host", kHostUid, meeting_id, "req-close").common.result_code,
              ResultCode::OK);
    fixture.RegisterAuthenticated("s-stranger", kStrangerUid);

    const LeaveMeetingResult left =
        LeaveAs(fixture, "s-stranger", kStrangerUid, meeting_id, "req-leave-1");

    EXPECT_EQ(left.common.result_code, ResultCode::NOT_PARTICIPANT);
    EXPECT_NE(left.common.result_code, ResultCode::MEETING_STATE_REJECTED);
}

TEST(MeetingServiceLeave, L8ClosedHistoricalHostIsAlreadyLeft)
{
    ServiceFixture fixture;
    const MeetingId meeting_id = CreateMeetingFor(fixture, "s-host").meeting.meeting_id;
    CloseAs(fixture, "s-host", kHostUid, meeting_id, "req-close");
    fixture.SetNow(10);
    ASSERT_TRUE(fixture.service().FinalizeClose(meeting_id).finalized);

    const LeaveMeetingResult left =
        LeaveAs(fixture, "s-host", kHostUid, meeting_id, "req-leave-1");

    EXPECT_EQ(left.common.result_code, ResultCode::ALREADY_LEFT);
    EXPECT_EQ(left.common.disposition, ResultDisposition::IDEMPOTENT);
    EXPECT_EQ(left.meeting.meeting_state, MeetingState::CLOSED);
    EXPECT_TRUE(left.events.empty());
}

TEST(MeetingServiceLeave, L9ClosedHistoricalParticipantIsAlreadyLeft)
{
    ServiceFixture fixture;
    const MeetingId meeting_id = CreateMeetingFor(fixture, "s-host").meeting.meeting_id;
    fixture.RegisterAuthenticated("s-user", kUserAUid);
    JoinAs(fixture, "s-user", kUserAUid, meeting_id, "req-join-1");
    CloseAs(fixture, "s-host", kHostUid, meeting_id, "req-close");
    fixture.SetNow(10);
    ASSERT_TRUE(fixture.service().FinalizeClose(meeting_id).finalized);

    const LeaveMeetingResult left =
        LeaveAs(fixture, "s-user", kUserAUid, meeting_id, "req-leave-1");

    EXPECT_EQ(left.common.result_code, ResultCode::ALREADY_LEFT);
    EXPECT_EQ(left.meeting.active_participant_count, std::size_t(2));
    EXPECT_TRUE(left.events.empty());
}

TEST(MeetingServiceLeave, L10ClosedNeverParticipantIsNotParticipant)
{
    ServiceFixture fixture;
    const MeetingId meeting_id = CreateMeetingFor(fixture, "s-host").meeting.meeting_id;
    CloseAs(fixture, "s-host", kHostUid, meeting_id, "req-close");
    fixture.SetNow(10);
    ASSERT_TRUE(fixture.service().FinalizeClose(meeting_id).finalized);
    fixture.RegisterAuthenticated("s-stranger", kStrangerUid);

    const LeaveMeetingResult left =
        LeaveAs(fixture, "s-stranger", kStrangerUid, meeting_id, "req-leave-1");

    EXPECT_EQ(left.common.result_code, ResultCode::NOT_PARTICIPANT);
    EXPECT_TRUE(left.events.empty());
}

TEST(MeetingServiceLeave, L11ClosedPathsDoNotMutateSnapshotOrProduceEvents)
{
    ServiceFixture fixture;
    const MeetingId meeting_id = CreateMeetingFor(fixture, "s-host").meeting.meeting_id;
    fixture.RegisterAuthenticated("s-user", kUserAUid);
    JoinAs(fixture, "s-user", kUserAUid, meeting_id, "req-join-1");
    CloseAs(fixture, "s-host", kHostUid, meeting_id, "req-close");
    fixture.SetNow(10);
    ASSERT_TRUE(fixture.service().FinalizeClose(meeting_id).finalized);

    const QueryMeetingResult before =
        QueryAs(fixture, "s-host", kHostUid, meeting_id, "req-q1");

    const LeaveMeetingResult participant_left =
        LeaveAs(fixture, "s-user", kUserAUid, meeting_id, "req-leave-1");
    const LeaveMeetingResult host_left =
        LeaveAs(fixture, "s-host", kHostUid, meeting_id, "req-leave-2");
    fixture.RegisterAuthenticated("s-stranger", kStrangerUid);
    const LeaveMeetingResult stranger_left =
        LeaveAs(fixture, "s-stranger", kStrangerUid, meeting_id, "req-leave-3");

    EXPECT_TRUE(participant_left.events.empty());
    EXPECT_TRUE(host_left.events.empty());
    EXPECT_TRUE(stranger_left.events.empty());

    // 快照未被修改：CLOSED 视图完全一致。
    const QueryMeetingResult after =
        QueryAs(fixture, "s-host", kHostUid, meeting_id, "req-q2");
    EXPECT_EQ(after.common.result_code, ResultCode::OK);
    EXPECT_EQ(after.meeting.meeting_state, MeetingState::CLOSED);
    EXPECT_EQ(after.meeting.active_participant_count,
              before.meeting.active_participant_count);
    EXPECT_EQ(after.meeting.closed_at, before.meeting.closed_at);
    EXPECT_EQ(after.meeting.created_at, before.meeting.created_at);
    EXPECT_FALSE(fixture.service().HasActiveMeeting(meeting_id));
}

TEST(MeetingServiceLeave, L12SuccessfulLeaveRemovesAllReverseRefsOfThatUser)
{
    ServiceFixture fixture;
    const MeetingId meeting_id = CreateMeetingFor(fixture, "s-host").meeting.meeting_id;
    fixture.RegisterAuthenticated("s-user-1", kUserAUid);
    fixture.RegisterAuthenticated("s-user-2", kUserAUid);
    fixture.RegisterAuthenticated("s-user-3", kUserBUid);
    JoinAs(fixture, "s-user-1", kUserAUid, meeting_id, "req-join-1");
    JoinAs(fixture, "s-user-2", kUserAUid, meeting_id, "req-join-2");
    JoinAs(fixture, "s-user-3", kUserBUid, meeting_id, "req-join-3");

    EXPECT_EQ(LeaveAs(fixture, "s-user-1", kUserAUid, meeting_id, "req-leave-1")
                  .common.result_code,
              ResultCode::OK);

    EXPECT_EQ(fixture.service().ReverseBindingCountForSession("s-user-1"), 0u);
    EXPECT_EQ(fixture.service().ReverseBindingCountForSession("s-user-2"), 0u);
    // 其他 User 的 ref 不受影响。
    EXPECT_EQ(fixture.service().ReverseBindingCountForSession("s-user-3"), 1u);
}

// ==============================================================================
// MeetingServiceClose —— X1–X11
// ==============================================================================

TEST(MeetingServiceClose, X1HostFirstCloseMovesMeetingToEnding)
{
    ServiceFixture fixture;
    const MeetingId meeting_id = CreateMeetingFor(fixture, "s-host").meeting.meeting_id;
    fixture.SetNow(40);

    const CloseMeetingResult closed =
        CloseAs(fixture, "s-host", kHostUid, meeting_id, "req-close-1");

    EXPECT_EQ(closed.common.result_code, ResultCode::OK);
    EXPECT_EQ(closed.common.disposition, ResultDisposition::SUCCESS);
    EXPECT_EQ(closed.meeting.meeting_state, MeetingState::ENDING);
    EXPECT_FALSE(closed.meeting.has_closed_at);
    // 首次 Close 的 Response 绝不假装 FinalizeClose 已完成。
    EXPECT_TRUE(fixture.service().HasActiveMeeting(meeting_id));
    EXPECT_FALSE(fixture.service().HasClosedSnapshot(meeting_id));
}

TEST(MeetingServiceClose, X2ResponseCarriesCloseStarted)
{
    ServiceFixture fixture;
    const MeetingId meeting_id = CreateMeetingFor(fixture, "s-host").meeting.meeting_id;

    const CloseMeetingResult closed =
        CloseAs(fixture, "s-host", kHostUid, meeting_id, "req-close-1");

    EXPECT_EQ(closed.outcome, meeting::CloseOutcome::CLOSE_STARTED);
    EXPECT_EQ(closed.common.result_name, std::string("OK"));
    // BeginClose 不产生任何事件。
    EXPECT_TRUE(closed.events.empty());
}

TEST(MeetingServiceClose, X3ExactlyOneFinalizeIntentIsRequested)
{
    ServiceFixture fixture;
    const MeetingId meeting_id = CreateMeetingFor(fixture, "s-host").meeting.meeting_id;

    const CloseMeetingResult first =
        CloseAs(fixture, "s-host", kHostUid, meeting_id, "req-close-1");
    EXPECT_TRUE(first.should_schedule_finalize);

    const CloseMeetingResult second =
        CloseAs(fixture, "s-host", kHostUid, meeting_id, "req-close-2");
    EXPECT_FALSE(second.should_schedule_finalize);
}

TEST(MeetingServiceClose, X4SecondCloseBeforeFinalizeIsCloseInProgress)
{
    ServiceFixture fixture;
    const MeetingId meeting_id = CreateMeetingFor(fixture, "s-host").meeting.meeting_id;
    CloseAs(fixture, "s-host", kHostUid, meeting_id, "req-close-1");

    const CloseMeetingResult second =
        CloseAs(fixture, "s-host", kHostUid, meeting_id, "req-close-2");

    EXPECT_EQ(second.common.result_code, ResultCode::CLOSE_IN_PROGRESS);
    EXPECT_EQ(second.common.disposition, ResultDisposition::IDEMPOTENT);
    EXPECT_EQ(second.meeting.meeting_state, MeetingState::ENDING);
    EXPECT_TRUE(second.events.empty());
}

TEST(MeetingServiceClose, X5SecondCloseDoesNotFreezeASecondContext)
{
    ServiceFixture fixture;
    const MeetingId meeting_id = CreateMeetingFor(fixture, "s-host").meeting.meeting_id;
    CloseAs(fixture, "s-host", kHostUid, meeting_id, "req-close-1");
    CloseAs(fixture, "s-host", kHostUid, meeting_id, "req-close-2");
    fixture.SetNow(10);

    const FinalizeCloseResult finalized = fixture.service().FinalizeClose(meeting_id);

    // 只能安排一个 FinalizeClose：MeetingClosed 也只出现一次。
    EXPECT_TRUE(finalized.finalized);
    EXPECT_EQ(finalized.events.size(), 1u);
    // 与最初那次 Close 建立 correlation。
    EXPECT_EQ(finalized.events[0].request_id, std::string("req-close-1"));
}

TEST(MeetingServiceClose, X6FinalizeBuildsCompleteSnapshot)
{
    ServiceFixture fixture;
    const MeetingId meeting_id = CreateMeetingFor(fixture, "s-host", kHostUid,
                                                 "req-create-1", 1).meeting.meeting_id;
    fixture.RegisterAuthenticated("s-user", kUserAUid);
    JoinAs(fixture, "s-user", kUserAUid, meeting_id, "req-join-1");
    CloseAs(fixture, "s-host", kHostUid, meeting_id, "req-close-1");
    fixture.SetNow(60);

    const FinalizeCloseResult finalized = fixture.service().FinalizeClose(meeting_id);

    ASSERT_TRUE(finalized.finalized);
    ASSERT_TRUE(finalized.has_snapshot);
    const ClosedMeetingSnapshot& snapshot = finalized.snapshot;
    EXPECT_EQ(snapshot.meeting_id, meeting_id);
    EXPECT_EQ(snapshot.host_user_id, kHostUid);
    EXPECT_EQ(snapshot.owner_chat_server_id, std::string(kLocalServer));
    EXPECT_EQ(snapshot.meeting_state, MeetingState::CLOSED);
    EXPECT_EQ(snapshot.created_at, AtSeconds(1));
    EXPECT_EQ(snapshot.closed_at, AtSeconds(60));
    EXPECT_EQ(snapshot.final_participant_count, std::size_t(2));
    ASSERT_EQ(snapshot.participant_identities.size(), 2u);
    EXPECT_EQ(snapshot.participant_identities[0].user_id, kHostUid);
    EXPECT_EQ(snapshot.participant_identities[0].role, ParticipantRole::HOST);
    EXPECT_EQ(snapshot.participant_identities[1].user_id, kUserAUid);
    EXPECT_FALSE(snapshot.participant_identities[1].has_left_at);
}

TEST(MeetingServiceClose, X7SnapshotPublishedAndActiveAggregateReleased)
{
    ServiceFixture fixture;
    const MeetingId meeting_id = CreateMeetingFor(fixture, "s-host").meeting.meeting_id;
    CloseAs(fixture, "s-host", kHostUid, meeting_id, "req-close-1");
    fixture.SetNow(10);

    EXPECT_TRUE(fixture.service().HasActiveMeeting(meeting_id));
    EXPECT_FALSE(fixture.service().HasClosedSnapshot(meeting_id));

    ASSERT_TRUE(fixture.service().FinalizeClose(meeting_id).finalized);

    // 稳定状态：active 不存在 + snapshot 存在。不得同时存在或同时缺失。
    EXPECT_FALSE(fixture.service().HasActiveMeeting(meeting_id));
    EXPECT_TRUE(fixture.service().HasClosedSnapshot(meeting_id));
}

TEST(MeetingServiceClose, X8MeetingClosedProducedExactlyOnce)
{
    ServiceFixture fixture;
    const MeetingId meeting_id = CreateMeetingFor(fixture, "s-host", kHostUid,
                                                 "req-create-1", 1).meeting.meeting_id;
    CloseAs(fixture, "s-host", kHostUid, meeting_id, "req-close-1");
    fixture.SetNow(60);

    const FinalizeCloseResult finalized = fixture.service().FinalizeClose(meeting_id);

    ASSERT_EQ(finalized.events.size(), 1u);
    const MeetingEvent& event = finalized.events[0];
    EXPECT_EQ(event.event_type, EventType::MEETING_CLOSED);
    EXPECT_EQ(event.meeting_id, meeting_id);
    EXPECT_EQ(event.meeting_state_after, MeetingState::CLOSED);
    EXPECT_TRUE(event.has_actor_user_id);
    EXPECT_EQ(event.actor_user_id, kHostUid);
    EXPECT_FALSE(event.has_participant_user_id);
    EXPECT_EQ(event.owner_chat_server_id, std::string(kLocalServer));
    EXPECT_EQ(event.occurred_at, AtSeconds(60));
    EXPECT_TRUE(event.has_request_id);
    EXPECT_EQ(event.request_id, std::string("req-close-1"));
}

TEST(MeetingServiceClose, X9HostCloseOnClosedMeetingIsAlreadyClosed)
{
    ServiceFixture fixture;
    const MeetingId meeting_id = CreateMeetingFor(fixture, "s-host").meeting.meeting_id;
    CloseAs(fixture, "s-host", kHostUid, meeting_id, "req-close-1");
    fixture.SetNow(10);
    ASSERT_TRUE(fixture.service().FinalizeClose(meeting_id).finalized);

    const CloseMeetingResult again =
        CloseAs(fixture, "s-host", kHostUid, meeting_id, "req-close-2");

    EXPECT_EQ(again.common.result_code, ResultCode::ALREADY_CLOSED);
    EXPECT_EQ(again.common.disposition, ResultDisposition::IDEMPOTENT);
    EXPECT_EQ(again.meeting.meeting_state, MeetingState::CLOSED);
    EXPECT_TRUE(again.meeting.has_closed_at);
    EXPECT_EQ(again.meeting.closed_at, AtSeconds(10));
    EXPECT_FALSE(again.should_schedule_finalize);
    EXPECT_TRUE(again.events.empty());
}

TEST(MeetingServiceClose, X10NonHostCloseOnEndingIsPermissionDenied)
{
    ServiceFixture fixture;
    const MeetingId meeting_id = CreateMeetingFor(fixture, "s-host").meeting.meeting_id;
    fixture.RegisterAuthenticated("s-user", kUserAUid);
    JoinAs(fixture, "s-user", kUserAUid, meeting_id, "req-join-1");
    CloseAs(fixture, "s-host", kHostUid, meeting_id, "req-close-1");

    const CloseMeetingResult denied =
        CloseAs(fixture, "s-user", kUserAUid, meeting_id, "req-close-2");

    EXPECT_EQ(denied.common.result_code, ResultCode::PERMISSION_DENIED);
    EXPECT_NE(denied.common.result_code, ResultCode::CLOSE_IN_PROGRESS);
    EXPECT_FALSE(denied.should_schedule_finalize);
}

TEST(MeetingServiceClose, X10bNonHostCloseOnClosedIsPermissionDenied)
{
    ServiceFixture fixture;
    const MeetingId meeting_id = CreateMeetingFor(fixture, "s-host").meeting.meeting_id;
    fixture.RegisterAuthenticated("s-user", kUserAUid);
    JoinAs(fixture, "s-user", kUserAUid, meeting_id, "req-join-1");
    CloseAs(fixture, "s-host", kHostUid, meeting_id, "req-close-1");
    fixture.SetNow(10);
    ASSERT_TRUE(fixture.service().FinalizeClose(meeting_id).finalized);

    const CloseMeetingResult denied =
        CloseAs(fixture, "s-user", kUserAUid, meeting_id, "req-close-2");

    // 即使调用者是历史 Participant，也只有 Host 能得到 ALREADY_CLOSED。
    EXPECT_EQ(denied.common.result_code, ResultCode::PERMISSION_DENIED);
    // 授权失败时不返回 MeetingView，避免泄露 CLOSED 会议摘要。
    EXPECT_TRUE(denied.meeting.meeting_id.empty());
}

TEST(MeetingServiceClose, X10cNonHostCloseOnOpenMeetingIsPermissionDenied)
{
    ServiceFixture fixture;
    const MeetingId meeting_id = CreateMeetingFor(fixture, "s-host").meeting.meeting_id;
    fixture.RegisterAuthenticated("s-user", kUserAUid);
    JoinAs(fixture, "s-user", kUserAUid, meeting_id, "req-join-1");

    const CloseMeetingResult denied =
        CloseAs(fixture, "s-user", kUserAUid, meeting_id, "req-close-1");

    EXPECT_EQ(denied.common.result_code, ResultCode::PERMISSION_DENIED);
    EXPECT_TRUE(denied.meeting.meeting_id.empty());
    EXPECT_FALSE(fixture.service().HasClosedSnapshot(meeting_id));
    // 会议状态未被这次被拒绝的 Close 改变。
    EXPECT_EQ(QueryAs(fixture, "s-host", kHostUid, meeting_id, "req-q1")
                  .meeting.meeting_state,
              MeetingState::ACTIVE);
}

TEST(MeetingServiceClose, X11FinalizeSucceedsIndependentlyOfAnyNotificationAdapter)
{
    // M3 中 FinalizeClose 不依赖任何 notification callback，因此不存在
    // "发送失败阻止 CLOSED" 的结构。这里断言：通知只体现为纯数据意图，
    // 收敛为 CLOSED 不依赖任何外部副作用。
    ServiceFixture fixture;
    const MeetingId meeting_id = CreateMeetingFor(fixture, "s-host").meeting.meeting_id;
    fixture.RegisterAuthenticated("s-user", kUserAUid);
    JoinAs(fixture, "s-user", kUserAUid, meeting_id, "req-join-1");
    CloseAs(fixture, "s-host", kHostUid, meeting_id, "req-close-1");
    fixture.SetNow(70);

    const FinalizeCloseResult finalized = fixture.service().FinalizeClose(meeting_id);

    EXPECT_TRUE(finalized.finalized);
    // 可通知目标来自 CloseContext 的冻结快照，M3 只返回意图、不执行发送。
    ASSERT_EQ(finalized.notification_session_ids.size(), 2u);
    EXPECT_EQ(finalized.notification_session_ids[0], std::string("s-host"));
    EXPECT_EQ(finalized.notification_session_ids[1], std::string("s-user"));

    const QueryMeetingResult queried =
        QueryAs(fixture, "s-host", kHostUid, meeting_id, "req-q1");
    EXPECT_EQ(queried.common.result_code, ResultCode::OK);
    EXPECT_EQ(queried.meeting.meeting_state, MeetingState::CLOSED);
}

// ==============================================================================
// MeetingServiceFinalizeClose —— 独立 domain operation
// ==============================================================================

TEST(MeetingServiceFinalizeClose, PublishesSnapshotBeforeReleasingAggregate)
{
    ServiceFixture fixture;
    const MeetingId meeting_id = CreateMeetingFor(fixture, "s-host").meeting.meeting_id;
    CloseAs(fixture, "s-host", kHostUid, meeting_id, "req-close-1");
    fixture.SetNow(10);

    const FinalizeCloseResult finalized = fixture.service().FinalizeClose(meeting_id);

    // 返回值本身即可观察到发布顺序：finalized 为真时 snapshot 必然已经可用。
    EXPECT_TRUE(finalized.finalized);
    EXPECT_TRUE(finalized.has_snapshot);
    EXPECT_TRUE(fixture.service().HasClosedSnapshot(meeting_id));
    EXPECT_FALSE(fixture.service().HasActiveMeeting(meeting_id));
}

TEST(MeetingServiceFinalizeClose, DuplicateFinalizeIsAlreadyFinalizedWithoutEventOrIntent)
{
    ServiceFixture fixture;
    const MeetingId meeting_id = CreateMeetingFor(fixture, "s-host").meeting.meeting_id;
    fixture.RegisterAuthenticated("s-user", kUserAUid);
    JoinAs(fixture, "s-user", kUserAUid, meeting_id, "req-join-1");
    CloseAs(fixture, "s-host", kHostUid, meeting_id, "req-close-1");
    fixture.SetNow(10);
    ASSERT_TRUE(fixture.service().FinalizeClose(meeting_id).finalized);

    const FinalizeCloseResult duplicate = fixture.service().FinalizeClose(meeting_id);

    EXPECT_FALSE(duplicate.finalized);
    EXPECT_TRUE(duplicate.already_finalized);
    EXPECT_TRUE(duplicate.has_snapshot);
    EXPECT_EQ(duplicate.snapshot.closed_at, AtSeconds(10));
    // 不得第二次产生 MeetingClosed，也不得第二次返回通知意图。
    EXPECT_TRUE(duplicate.events.empty());
    EXPECT_TRUE(duplicate.notification_session_ids.empty());
}

TEST(MeetingServiceFinalizeClose, CreatedMeetingIsNotFinalized)
{
    ServiceFixture fixture;
    const MeetingId meeting_id = CreateMeetingFor(fixture, "s-host").meeting.meeting_id;

    const FinalizeCloseResult finalized = fixture.service().FinalizeClose(meeting_id);

    EXPECT_FALSE(finalized.finalized);
    EXPECT_FALSE(finalized.already_finalized);
    EXPECT_TRUE(finalized.events.empty());
    EXPECT_TRUE(fixture.service().HasActiveMeeting(meeting_id));
    EXPECT_FALSE(fixture.service().HasClosedSnapshot(meeting_id));
}

TEST(MeetingServiceFinalizeClose, ActiveMeetingIsNotFinalized)
{
    ServiceFixture fixture;
    const MeetingId meeting_id = CreateMeetingFor(fixture, "s-host").meeting.meeting_id;
    fixture.RegisterAuthenticated("s-user", kUserAUid);
    JoinAs(fixture, "s-user", kUserAUid, meeting_id, "req-join-1");

    const FinalizeCloseResult finalized = fixture.service().FinalizeClose(meeting_id);

    EXPECT_FALSE(finalized.finalized);
    EXPECT_FALSE(finalized.already_finalized);
    EXPECT_TRUE(fixture.service().HasActiveMeeting(meeting_id));
    EXPECT_FALSE(fixture.service().HasClosedSnapshot(meeting_id));
    // 它不是外部 API，因此不产生客户端 Response，也不把状态当成 rejected response。
    EXPECT_EQ(QueryAs(fixture, "s-host", kHostUid, meeting_id, "req-q1")
                  .meeting.meeting_state,
              MeetingState::ACTIVE);
}

TEST(MeetingServiceFinalizeClose, UnknownMeetingIsNotFinalized)
{
    ServiceFixture fixture;
    const FinalizeCloseResult finalized = fixture.service().FinalizeClose("meeting-unknown");

    EXPECT_FALSE(finalized.finalized);
    EXPECT_FALSE(finalized.already_finalized);
    EXPECT_TRUE(finalized.events.empty());
}

TEST(MeetingServiceFinalizeClose, SnapshotKeepsFrozenMembershipIncludingHistoricalLeft)
{
    ServiceFixture fixture;
    const MeetingId meeting_id = CreateMeetingFor(fixture, "s-host", kHostUid,
                                                 "req-create-1", 1).meeting.meeting_id;
    fixture.RegisterAuthenticated("s-user-a", kUserAUid);
    fixture.RegisterAuthenticated("s-user-b", kUserBUid);
    fixture.SetNow(2);
    JoinAs(fixture, "s-user-a", kUserAUid, meeting_id, "req-join-1");
    fixture.SetNow(3);
    JoinAs(fixture, "s-user-b", kUserBUid, meeting_id, "req-join-2");
    fixture.SetNow(4);
    ASSERT_EQ(LeaveAs(fixture, "s-user-a", kUserAUid, meeting_id, "req-leave-1")
                  .common.result_code,
              ResultCode::OK);
    CloseAs(fixture, "s-host", kHostUid, meeting_id, "req-close-1");
    fixture.SetNow(20);

    const FinalizeCloseResult finalized = fixture.service().FinalizeClose(meeting_id);

    ASSERT_TRUE(finalized.finalized);
    const ClosedMeetingSnapshot& snapshot = finalized.snapshot;
    // 冻结边界上的 ACTIVE 成员数是 2（Host + B），不是历史加入数 3。
    EXPECT_EQ(snapshot.final_participant_count, std::size_t(2));
    ASSERT_EQ(snapshot.participant_identities.size(), 3u);
    EXPECT_FALSE(snapshot.participant_identities[0].has_left_at);
    EXPECT_TRUE(snapshot.participant_identities[1].has_left_at);
    EXPECT_EQ(snapshot.participant_identities[1].left_at, AtSeconds(4));
    EXPECT_FALSE(snapshot.participant_identities[2].has_left_at);
}

TEST(MeetingServiceFinalizeClose, FinalizeRemovesAllReverseRefsForThatMeeting)
{
    ServiceFixture fixture;
    const MeetingId meeting_id = CreateMeetingFor(fixture, "s-host").meeting.meeting_id;
    fixture.RegisterAuthenticated("s-user", kUserAUid);
    JoinAs(fixture, "s-user", kUserAUid, meeting_id, "req-join-1");
    EXPECT_EQ(fixture.service().ReverseBindingCountForSession("s-host"), 1u);
    CloseAs(fixture, "s-host", kHostUid, meeting_id, "req-close-1");
    fixture.SetNow(10);
    ASSERT_TRUE(fixture.service().FinalizeClose(meeting_id).finalized);

    EXPECT_EQ(fixture.service().ReverseBindingCountForSession("s-host"), 0u);
    EXPECT_EQ(fixture.service().ReverseBindingCountForSession("s-user"), 0u);
}

// ==============================================================================
// MeetingServiceQuery —— Q1–Q6 与只读性
// ==============================================================================

TEST(MeetingServiceQuery, Q1HostCanQueryCreatedMeeting)
{
    ServiceFixture fixture;
    const CreateMeetingResult created = CreateMeetingFor(fixture, "s-host");

    const QueryMeetingResult queried =
        QueryAs(fixture, "s-host", kHostUid, created.meeting.meeting_id, "req-q1");

    EXPECT_EQ(queried.common.result_code, ResultCode::OK);
    EXPECT_EQ(queried.meeting.meeting_state, MeetingState::CREATED);
    EXPECT_EQ(queried.meeting.active_participant_count, std::size_t(1));
    EXPECT_EQ(queried.meeting.created_at, AtSeconds(1));
    EXPECT_FALSE(queried.meeting.has_closed_at);
}

TEST(MeetingServiceQuery, Q1bActiveParticipantCanQueryActiveMeeting)
{
    ServiceFixture fixture;
    const MeetingId meeting_id = CreateMeetingFor(fixture, "s-host").meeting.meeting_id;
    fixture.RegisterAuthenticated("s-user", kUserAUid);
    JoinAs(fixture, "s-user", kUserAUid, meeting_id, "req-join-1");

    const QueryMeetingResult queried =
        QueryAs(fixture, "s-user", kUserAUid, meeting_id, "req-q1");

    EXPECT_EQ(queried.common.result_code, ResultCode::OK);
    EXPECT_EQ(queried.meeting.meeting_state, MeetingState::ACTIVE);
    EXPECT_EQ(queried.meeting.active_participant_count, std::size_t(2));
}

TEST(MeetingServiceQuery, Q2QueryDuringEndingObservesEnding)
{
    ServiceFixture fixture;
    const MeetingId meeting_id = CreateMeetingFor(fixture, "s-host").meeting.meeting_id;
    CloseAs(fixture, "s-host", kHostUid, meeting_id, "req-close-1");

    const QueryMeetingResult queried =
        QueryAs(fixture, "s-host", kHostUid, meeting_id, "req-q1");

    EXPECT_EQ(queried.common.result_code, ResultCode::OK);
    EXPECT_EQ(queried.meeting.meeting_state, MeetingState::ENDING);
    EXPECT_FALSE(queried.meeting.has_closed_at);
}

TEST(MeetingServiceQuery, Q3ClosedQueryReadsSnapshotOnly)
{
    ServiceFixture fixture;
    const MeetingId meeting_id = CreateMeetingFor(fixture, "s-host").meeting.meeting_id;
    fixture.RegisterAuthenticated("s-user", kUserAUid);
    JoinAs(fixture, "s-user", kUserAUid, meeting_id, "req-join-1");
    CloseAs(fixture, "s-host", kHostUid, meeting_id, "req-close-1");
    fixture.SetNow(90);
    const FinalizeCloseResult finalized = fixture.service().FinalizeClose(meeting_id);
    ASSERT_TRUE(finalized.finalized);

    const QueryMeetingResult queried =
        QueryAs(fixture, "s-host", kHostUid, meeting_id, "req-q1");

    EXPECT_EQ(queried.common.result_code, ResultCode::OK);
    EXPECT_EQ(queried.meeting.meeting_state, MeetingState::CLOSED);
    EXPECT_EQ(queried.meeting.active_participant_count,
              finalized.snapshot.final_participant_count);
    EXPECT_TRUE(queried.meeting.has_closed_at);
    EXPECT_EQ(queried.meeting.closed_at, finalized.snapshot.closed_at);
    EXPECT_EQ(queried.meeting.closed_at, AtSeconds(90));
    EXPECT_EQ(queried.meeting.created_at, finalized.snapshot.created_at);
    // 活动聚合已释放：CLOSED 视图只能来自 Snapshot。
    EXPECT_FALSE(fixture.service().HasActiveMeeting(meeting_id));
}

TEST(MeetingServiceQuery, Q4HistoricalParticipantIsAuthorizedOnClosedMeeting)
{
    ServiceFixture fixture;
    const MeetingId meeting_id = CreateMeetingFor(fixture, "s-host").meeting.meeting_id;
    fixture.RegisterAuthenticated("s-user-a", kUserAUid);
    fixture.RegisterAuthenticated("s-user-b", kUserBUid);
    JoinAs(fixture, "s-user-a", kUserAUid, meeting_id, "req-join-1");
    JoinAs(fixture, "s-user-b", kUserBUid, meeting_id, "req-join-2");
    LeaveAs(fixture, "s-user-b", kUserBUid, meeting_id, "req-leave-1");
    CloseAs(fixture, "s-host", kHostUid, meeting_id, "req-close-1");
    fixture.SetNow(10);
    ASSERT_TRUE(fixture.service().FinalizeClose(meeting_id).finalized);

    // 当前仍是 ACTIVE 的历史成员。
    EXPECT_EQ(QueryAs(fixture, "s-user-a", kUserAUid, meeting_id, "req-q1")
                  .common.result_code,
              ResultCode::OK);
    // 已 LEFT 的历史成员同样保留 CLOSED 查询权限。
    EXPECT_EQ(QueryAs(fixture, "s-user-b", kUserBUid, meeting_id, "req-q2")
                  .common.result_code,
              ResultCode::OK);
}

TEST(MeetingServiceQuery, Q5NeverParticipantIsDenied)
{
    ServiceFixture fixture;
    const MeetingId meeting_id = CreateMeetingFor(fixture, "s-host").meeting.meeting_id;
    CloseAs(fixture, "s-host", kHostUid, meeting_id, "req-close-1");
    fixture.SetNow(10);
    ASSERT_TRUE(fixture.service().FinalizeClose(meeting_id).finalized);
    fixture.RegisterAuthenticated("s-stranger", kStrangerUid);

    const QueryMeetingResult queried =
        QueryAs(fixture, "s-stranger", kStrangerUid, meeting_id, "req-q1");

    EXPECT_EQ(queried.common.result_code, ResultCode::NOT_PARTICIPANT);
    EXPECT_EQ(queried.common.disposition, ResultDisposition::ERROR);
}

TEST(MeetingServiceQuery, Q5bLeftHistoricalParticipantCannotQueryOpenMeeting)
{
    ServiceFixture fixture;
    const MeetingId meeting_id = CreateMeetingFor(fixture, "s-host").meeting.meeting_id;
    fixture.RegisterAuthenticated("s-user", kUserAUid);
    JoinAs(fixture, "s-user", kUserAUid, meeting_id, "req-join-1");
    LeaveAs(fixture, "s-user", kUserAUid, meeting_id, "req-leave-1");

    const QueryMeetingResult queried =
        QueryAs(fixture, "s-user", kUserAUid, meeting_id, "req-q1");

    EXPECT_EQ(queried.common.result_code, ResultCode::NOT_PARTICIPANT);
}

TEST(MeetingServiceQuery, Q6EvictedSnapshotYieldsMeetingNotFound)
{
    ServiceFixture fixture;
    const MeetingId meeting_id = CreateMeetingFor(fixture, "s-host").meeting.meeting_id;
    CloseAs(fixture, "s-host", kHostUid, meeting_id, "req-close-1");
    fixture.SetNow(10);
    ASSERT_TRUE(fixture.service().FinalizeClose(meeting_id).finalized);
    ASSERT_EQ(QueryAs(fixture, "s-host", kHostUid, meeting_id, "req-q1").common.result_code,
              ResultCode::OK);

    EXPECT_TRUE(fixture.service().EvictClosedSnapshot(meeting_id));
    EXPECT_FALSE(fixture.service().EvictClosedSnapshot(meeting_id));

    const QueryMeetingResult queried =
        QueryAs(fixture, "s-host", kHostUid, meeting_id, "req-q2");
    EXPECT_EQ(queried.common.result_code, ResultCode::MEETING_NOT_FOUND);
    EXPECT_EQ(fixture.service().ReverseBindingCountForSession("s-host"), 0u);
}

TEST(MeetingServiceQuery, QueryIsReadOnly)
{
    ServiceFixture fixture;
    const MeetingId meeting_id = CreateMeetingFor(fixture, "s-host").meeting.meeting_id;
    fixture.RegisterAuthenticated("s-user", kUserAUid);
    fixture.RegisterAuthenticated("s-stranger", kStrangerUid);
    JoinAs(fixture, "s-user", kUserAUid, meeting_id, "req-join-1");
    const std::size_t refs_before_host =
        fixture.service().ReverseBindingCountForSession("s-host");
    const std::size_t refs_before_user =
        fixture.service().ReverseBindingCountForSession("s-user");

    const QueryMeetingResult first =
        QueryAs(fixture, "s-host", kHostUid, meeting_id, "req-q1");
    // 失败的只读查询同样不得改变任何状态。
    const QueryMeetingResult denied =
        QueryAs(fixture, "s-stranger", kStrangerUid, meeting_id, "req-q2");
    const QueryMeetingResult second =
        QueryAs(fixture, "s-host", kHostUid, meeting_id, "req-q3");

    EXPECT_EQ(denied.common.result_code, ResultCode::NOT_PARTICIPANT);
    EXPECT_TRUE(denied.meeting.meeting_id.empty());
    EXPECT_EQ(first.meeting.meeting_state, second.meeting.meeting_state);
    EXPECT_EQ(first.meeting.active_participant_count,
              second.meeting.active_participant_count);
    EXPECT_EQ(fixture.service().ReverseBindingCountForSession("s-host"),
              refs_before_host);
    EXPECT_EQ(fixture.service().ReverseBindingCountForSession("s-user"),
              refs_before_user);
}

// ==============================================================================
// MeetingServiceListParticipants —— CURRENT / HISTORICAL
// ==============================================================================

TEST(MeetingServiceListParticipants, CurrentScopeListsOnlyActiveMembers)
{
    ServiceFixture fixture;
    const MeetingId meeting_id = CreateMeetingFor(fixture, "s-host").meeting.meeting_id;
    fixture.RegisterAuthenticated("s-user-a", kUserAUid);
    fixture.RegisterAuthenticated("s-user-b", kUserBUid);
    JoinAs(fixture, "s-user-a", kUserAUid, meeting_id, "req-join-1");
    JoinAs(fixture, "s-user-b", kUserBUid, meeting_id, "req-join-2");
    LeaveAs(fixture, "s-user-a", kUserAUid, meeting_id, "req-leave-1");

    const ListParticipantsResult listed =
        ListAs(fixture, "s-host", kHostUid, meeting_id, "req-list-1");

    EXPECT_EQ(listed.common.result_code, ResultCode::OK);
    EXPECT_EQ(listed.scope, ParticipantViewScope::CURRENT);
    EXPECT_EQ(listed.participants.size(), 2u);

    bool found = false;
    EXPECT_EQ(ParticipantStateOf(listed, kHostUid, found), ParticipantState::ACTIVE);
    EXPECT_TRUE(found);
    EXPECT_EQ(ParticipantStateOf(listed, kUserBUid, found), ParticipantState::ACTIVE);
    EXPECT_TRUE(found);
    // LEFT 的历史成员不出现在 CURRENT 列表中。
    ParticipantView view;
    EXPECT_FALSE(FindParticipantView(listed.participants, kUserAUid, view));
}

TEST(MeetingServiceListParticipants, EndingScopeIsStillCurrent)
{
    ServiceFixture fixture;
    const MeetingId meeting_id = CreateMeetingFor(fixture, "s-host").meeting.meeting_id;
    fixture.RegisterAuthenticated("s-user", kUserAUid);
    JoinAs(fixture, "s-user", kUserAUid, meeting_id, "req-join-1");
    CloseAs(fixture, "s-host", kHostUid, meeting_id, "req-close-1");

    const ListParticipantsResult listed =
        ListAs(fixture, "s-host", kHostUid, meeting_id, "req-list-1");

    EXPECT_EQ(listed.common.result_code, ResultCode::OK);
    EXPECT_EQ(listed.scope, ParticipantViewScope::CURRENT);
    EXPECT_EQ(listed.meeting.meeting_state, MeetingState::ENDING);
    EXPECT_EQ(listed.participants.size(), 2u);
}

TEST(MeetingServiceListParticipants, ClosedScopeIsHistoricalFromSnapshot)
{
    ServiceFixture fixture;
    const MeetingId meeting_id = CreateMeetingFor(fixture, "s-host").meeting.meeting_id;
    fixture.RegisterAuthenticated("s-user-a", kUserAUid);
    fixture.RegisterAuthenticated("s-user-b", kUserBUid);
    fixture.SetNow(2);
    JoinAs(fixture, "s-user-a", kUserAUid, meeting_id, "req-join-1");
    fixture.SetNow(3);
    JoinAs(fixture, "s-user-b", kUserBUid, meeting_id, "req-join-2");
    fixture.SetNow(4);
    LeaveAs(fixture, "s-user-a", kUserAUid, meeting_id, "req-leave-1");
    CloseAs(fixture, "s-host", kHostUid, meeting_id, "req-close-1");
    fixture.SetNow(10);
    ASSERT_TRUE(fixture.service().FinalizeClose(meeting_id).finalized);

    const ListParticipantsResult listed =
        ListAs(fixture, "s-host", kHostUid, meeting_id, "req-list-1");

    EXPECT_EQ(listed.common.result_code, ResultCode::OK);
    EXPECT_EQ(listed.scope, ParticipantViewScope::HISTORICAL);
    EXPECT_EQ(listed.meeting.meeting_state, MeetingState::CLOSED);
    ASSERT_EQ(listed.participants.size(), 3u);

    bool found = false;
    EXPECT_EQ(ParticipantStateOf(listed, kHostUid, found), ParticipantState::ACTIVE);
    EXPECT_TRUE(found);
    // 关闭前已离会的历史成员保持 LEFT，并携带 left_at。
    ParticipantView left_view;
    EXPECT_TRUE(FindParticipantView(listed.participants, kUserAUid, left_view));
    EXPECT_EQ(left_view.participant_state, ParticipantState::LEFT);
    EXPECT_TRUE(left_view.has_left_at);
    EXPECT_EQ(left_view.left_at, AtSeconds(4));
    // "ACTIVE" 只表示关闭边界上仍为 ACTIVE，不是在线状态。
    ParticipantView active_view;
    EXPECT_TRUE(FindParticipantView(listed.participants, kUserBUid, active_view));
    EXPECT_EQ(active_view.participant_state, ParticipantState::ACTIVE);
    EXPECT_FALSE(active_view.has_left_at);
    EXPECT_EQ(active_view.joined_at, AtSeconds(3));
}

TEST(MeetingServiceListParticipants, MultiSessionUserAppearsOnce)
{
    ServiceFixture fixture;
    const MeetingId meeting_id = CreateMeetingFor(fixture, "s-host").meeting.meeting_id;
    fixture.RegisterAuthenticated("s-user-1", kUserAUid);
    fixture.RegisterAuthenticated("s-user-2", kUserAUid);
    JoinAs(fixture, "s-user-1", kUserAUid, meeting_id, "req-join-1");
    JoinAs(fixture, "s-user-2", kUserAUid, meeting_id, "req-join-2");

    const ListParticipantsResult listed =
        ListAs(fixture, "s-host", kHostUid, meeting_id, "req-list-1");

    EXPECT_EQ(listed.participants.size(), 2u);
    std::size_t occurrences = 0;
    for (std::size_t i = 0; i < listed.participants.size(); ++i) {
        if (listed.participants[i].user_id == kUserAUid) {
            ++occurrences;
        }
    }
    EXPECT_EQ(occurrences, 1u);
}

TEST(MeetingServiceListParticipants, NeverParticipantIsDenied)
{
    ServiceFixture fixture;
    const MeetingId meeting_id = CreateMeetingFor(fixture, "s-host").meeting.meeting_id;
    CloseAs(fixture, "s-host", kHostUid, meeting_id, "req-close-1");
    fixture.SetNow(10);
    ASSERT_TRUE(fixture.service().FinalizeClose(meeting_id).finalized);
    fixture.RegisterAuthenticated("s-stranger", kStrangerUid);

    const ListParticipantsResult listed =
        ListAs(fixture, "s-stranger", kStrangerUid, meeting_id, "req-list-1");

    EXPECT_EQ(listed.common.result_code, ResultCode::NOT_PARTICIPANT);
    EXPECT_TRUE(listed.participants.empty());
}

TEST(MeetingServiceListParticipants, LeftHistoricalMemberCannotListOpenMeeting)
{
    ServiceFixture fixture;
    const MeetingId meeting_id = CreateMeetingFor(fixture, "s-host").meeting.meeting_id;
    fixture.RegisterAuthenticated("s-user", kUserAUid);
    JoinAs(fixture, "s-user", kUserAUid, meeting_id, "req-join-1");
    LeaveAs(fixture, "s-user", kUserAUid, meeting_id, "req-leave-1");

    const ListParticipantsResult listed =
        ListAs(fixture, "s-user", kUserAUid, meeting_id, "req-list-1");

    EXPECT_EQ(listed.common.result_code, ResultCode::NOT_PARTICIPANT);
}

TEST(MeetingServiceListParticipants, EvictedSnapshotYieldsMeetingNotFound)
{
    ServiceFixture fixture;
    const MeetingId meeting_id = CreateMeetingFor(fixture, "s-host").meeting.meeting_id;
    CloseAs(fixture, "s-host", kHostUid, meeting_id, "req-close-1");
    fixture.SetNow(10);
    ASSERT_TRUE(fixture.service().FinalizeClose(meeting_id).finalized);
    ASSERT_TRUE(fixture.service().EvictClosedSnapshot(meeting_id));

    const ListParticipantsResult listed =
        ListAs(fixture, "s-host", kHostUid, meeting_id, "req-list-1");
    EXPECT_EQ(listed.common.result_code, ResultCode::MEETING_NOT_FOUND);
}

// ==============================================================================
// MeetingServiceLocality —— O2 / O3 / O4
// ==============================================================================

TEST(MeetingServiceLocality, O2TrustedRemoteOwnerYieldsMeetingNotLocal)
{
    ServiceFixture fixture;
    fixture.RegisterAuthenticated("s-user", kUserAUid);

    MeetingRequestContext context = fixture.Context("s-user", kUserAUid, "req-1");
    context.has_trusted_owner_chat_server_id = true;
    context.trusted_owner_chat_server_id = kRemoteServer;

    const JoinMeetingResult joined =
        fixture.service().JoinMeeting(context, "meeting-remote");

    EXPECT_EQ(joined.common.result_code, ResultCode::MEETING_NOT_LOCAL);
    EXPECT_EQ(joined.common.disposition, ResultDisposition::ERROR);
    // 结果必须携带可信的异地 owner，供调用方重定向。
    EXPECT_TRUE(joined.common.has_owner_chat_server_id);
    EXPECT_EQ(joined.common.owner_chat_server_id, std::string(kRemoteServer));
    EXPECT_TRUE(joined.events.empty());
}

TEST(MeetingServiceLocality, O2bAllMeetingIdApisReportMeetingNotLocal)
{
    ServiceFixture fixture;
    fixture.RegisterAuthenticated("s-user", kUserAUid);

    MeetingRequestContext context = fixture.Context("s-user", kUserAUid, "req-1");
    context.has_trusted_owner_chat_server_id = true;
    context.trusted_owner_chat_server_id = kRemoteServer;

    const MeetingId missing = "meeting-remote";
    EXPECT_EQ(fixture.service().JoinMeeting(context, missing).common.result_code,
              ResultCode::MEETING_NOT_LOCAL);
    EXPECT_EQ(fixture.service().LeaveMeeting(context, missing).common.result_code,
              ResultCode::MEETING_NOT_LOCAL);
    EXPECT_EQ(fixture.service().CloseMeeting(context, missing).common.result_code,
              ResultCode::MEETING_NOT_LOCAL);
    EXPECT_EQ(fixture.service().QueryMeeting(context, missing).common.result_code,
              ResultCode::MEETING_NOT_LOCAL);
    EXPECT_EQ(fixture.service().ListParticipants(context, missing).common.result_code,
              ResultCode::MEETING_NOT_LOCAL);
}

TEST(MeetingServiceLocality, O3NoTrustedOwnerYieldsMeetingNotFound)
{
    ServiceFixture fixture;
    fixture.RegisterAuthenticated("s-user", kUserAUid);

    const QueryMeetingResult queried = QueryAs(fixture, "s-user", kUserAUid,
                                               "meeting-missing", "req-1");

    EXPECT_EQ(queried.common.result_code, ResultCode::MEETING_NOT_FOUND);
    EXPECT_FALSE(queried.common.has_owner_chat_server_id);
    EXPECT_TRUE(queried.common.owner_chat_server_id.empty());
}

TEST(MeetingServiceLocality, O4NoClientHintPathCanBecomeTrusted)
{
    // MeetingRequestContext 只有服务端可信的 trusted_owner_chat_server_id，
    // 没有 client owner hint 字段。因此"缺少可信 owner"的本地缺失 Meeting
    // 只能得到 MEETING_NOT_FOUND，绝不能自动升级为 MEETING_NOT_LOCAL。
    ServiceFixture fixture;
    fixture.RegisterAuthenticated("s-user", kUserAUid);

    MeetingRequestContext context = fixture.Context("s-user", kUserAUid, "req-1");
    context.has_trusted_owner_chat_server_id = false;
    // 即使调用方把某个值塞进 trusted_owner 字段，presence flag 为 false 时无效。
    context.trusted_owner_chat_server_id = kRemoteServer;

    EXPECT_EQ(fixture.service().QueryMeeting(context, "meeting-missing").common.result_code,
              ResultCode::MEETING_NOT_FOUND);
}

TEST(MeetingServiceLocality, O4bTrustedOwnerEqualToLocalButAbsentIsNotFound)
{
    ServiceFixture fixture;
    fixture.RegisterAuthenticated("s-user", kUserAUid);

    MeetingRequestContext context = fixture.Context("s-user", kUserAUid, "req-1");
    context.has_trusted_owner_chat_server_id = true;
    context.trusted_owner_chat_server_id = kLocalServer;

    const QueryMeetingResult queried =
        fixture.service().QueryMeeting(context, "meeting-missing");
    EXPECT_EQ(queried.common.result_code, ResultCode::MEETING_NOT_FOUND);
    EXPECT_FALSE(queried.common.has_owner_chat_server_id);
}

TEST(MeetingServiceLocality, LocalRegistryTakesPrecedenceOverTrustedOwner)
{
    // 本地存在的 Meeting 必须先被解析为本地，不因为携带异地 owner 而 NOT_LOCAL。
    ServiceFixture fixture;
    const MeetingId meeting_id = CreateMeetingFor(fixture, "s-host").meeting.meeting_id;

    MeetingRequestContext context = fixture.Context("s-host", kHostUid, "req-1");
    context.has_trusted_owner_chat_server_id = true;
    context.trusted_owner_chat_server_id = kRemoteServer;

    const QueryMeetingResult queried = fixture.service().QueryMeeting(context, meeting_id);
    EXPECT_EQ(queried.common.result_code, ResultCode::OK);
}

// ==============================================================================
// MeetingServiceOrdering —— O1 / O5 与八步求值顺序
// ==============================================================================

TEST(MeetingServiceOrdering, O1InvalidMeetingIdBeatsLocality)
{
    // Step 1.4 §19.1.2：API 参数校验必须先于 Meeting lookup / locality。
    ServiceFixture fixture;
    fixture.RegisterAuthenticated("s-user", kUserAUid);

    MeetingRequestContext context = fixture.Context("s-user", kUserAUid, "req-1");
    context.has_trusted_owner_chat_server_id = true;
    context.trusted_owner_chat_server_id = kRemoteServer;

    EXPECT_EQ(fixture.service().JoinMeeting(context, "").common.result_code,
              ResultCode::INVALID_ARGUMENT);
    EXPECT_EQ(fixture.service().LeaveMeeting(context, "").common.result_code,
              ResultCode::INVALID_ARGUMENT);
    EXPECT_EQ(fixture.service().CloseMeeting(context, "").common.result_code,
              ResultCode::INVALID_ARGUMENT);
    EXPECT_EQ(fixture.service().QueryMeeting(context, "").common.result_code,
              ResultCode::INVALID_ARGUMENT);
    EXPECT_EQ(fixture.service().ListParticipants(context, "").common.result_code,
              ResultCode::INVALID_ARGUMENT);
}

TEST(MeetingServiceOrdering, O1bInvalidMeetingIdNeverBecomesMeetingNotFound)
{
    ServiceFixture fixture;
    fixture.RegisterAuthenticated("s-user", kUserAUid);

    const QueryMeetingResult queried =
        QueryAs(fixture, "s-user", kUserAUid, "", "req-1");
    EXPECT_EQ(queried.common.result_code, ResultCode::INVALID_ARGUMENT);
    EXPECT_NE(queried.common.result_code, ResultCode::MEETING_NOT_FOUND);
}

TEST(MeetingServiceOrdering, O5PermissionIsEvaluatedBeforeCloseIdempotency)
{
    ServiceFixture fixture;
    const MeetingId meeting_id = CreateMeetingFor(fixture, "s-host").meeting.meeting_id;
    fixture.RegisterAuthenticated("s-user", kUserAUid);
    JoinAs(fixture, "s-user", kUserAUid, meeting_id, "req-join-1");
    CloseAs(fixture, "s-host", kHostUid, meeting_id, "req-close-1");
    fixture.SetNow(10);
    ASSERT_TRUE(fixture.service().FinalizeClose(meeting_id).finalized);

    const CloseMeetingResult non_host =
        CloseAs(fixture, "s-user", kUserAUid, meeting_id, "req-close-2");
    const CloseMeetingResult host =
        CloseAs(fixture, "s-host", kHostUid, meeting_id, "req-close-3");

    EXPECT_EQ(non_host.common.result_code, ResultCode::PERMISSION_DENIED);
    EXPECT_EQ(host.common.result_code, ResultCode::ALREADY_CLOSED);
}

TEST(MeetingServiceOrdering, ContextInvalidBeatsSessionUnusable)
{
    ServiceFixture fixture;
    fixture.service().RegisterConnectedSession("s-host");

    // request_id 为空（context 非法）+ Session 未认证。
    const CreateMeetingResult result = fixture.service().CreateMeeting(
        fixture.Context("s-host", kHostUid, ""));
    EXPECT_EQ(result.common.result_code, ResultCode::INVALID_ARGUMENT);
}

TEST(MeetingServiceOrdering, SessionUnusableBeatsApiArgumentValidity)
{
    ServiceFixture fixture;
    fixture.service().RegisterConnectedSession("s-user");
    fixture.service().AuthenticateSession("s-user", kUserAUid);
    // 先让 Session 进入 CLOSED，再发送一个 meeting_id 为空的命令。
    fixture.service().SessionDisconnected("s-user");

    const QueryMeetingResult queried = QueryAs(fixture, "s-user", kUserAUid, "", "req-1");
    EXPECT_EQ(queried.common.result_code, ResultCode::SESSION_STATE_REJECTED);
}

TEST(MeetingServiceOrdering, SessionStateRejectedBeatsMeetingIdAndLocality)
{
    ServiceFixture fixture;
    fixture.service().RegisterConnectedSession("s-user");
    fixture.service().AuthenticateSession("s-user", kUserAUid);
    fixture.service().SessionDisconnected("s-user");

    MeetingRequestContext context = fixture.Context("s-user", kUserAUid, "req-1");
    context.has_trusted_owner_chat_server_id = true;
    context.trusted_owner_chat_server_id = kRemoteServer;

    EXPECT_EQ(fixture.service().JoinMeeting(context, "meeting-x").common.result_code,
              ResultCode::SESSION_STATE_REJECTED);
}

TEST(MeetingServiceOrdering, ResultDispositionMappingIsCentralized)
{
    EXPECT_EQ(ResultDispositionFor(ResultCode::OK), ResultDisposition::SUCCESS);
    EXPECT_EQ(ResultDispositionFor(ResultCode::ALREADY_JOINED),
              ResultDisposition::IDEMPOTENT);
    EXPECT_EQ(ResultDispositionFor(ResultCode::ALREADY_LEFT),
              ResultDisposition::IDEMPOTENT);
    EXPECT_EQ(ResultDispositionFor(ResultCode::CLOSE_IN_PROGRESS),
              ResultDisposition::IDEMPOTENT);
    EXPECT_EQ(ResultDispositionFor(ResultCode::ALREADY_CLOSED),
              ResultDisposition::IDEMPOTENT);
    EXPECT_EQ(ResultDispositionFor(ResultCode::AUTH_REQUIRED), ResultDisposition::ERROR);
    EXPECT_EQ(ResultDispositionFor(ResultCode::SESSION_STATE_REJECTED),
              ResultDisposition::ERROR);
    EXPECT_EQ(ResultDispositionFor(ResultCode::INVALID_ARGUMENT),
              ResultDisposition::ERROR);
    EXPECT_EQ(ResultDispositionFor(ResultCode::PERMISSION_DENIED),
              ResultDisposition::ERROR);
    EXPECT_EQ(ResultDispositionFor(ResultCode::NOT_PARTICIPANT),
              ResultDisposition::ERROR);
    EXPECT_EQ(ResultDispositionFor(ResultCode::HOST_MUST_CLOSE_MEETING),
              ResultDisposition::ERROR);
    EXPECT_EQ(ResultDispositionFor(ResultCode::MEETING_NOT_FOUND),
              ResultDisposition::ERROR);
    EXPECT_EQ(ResultDispositionFor(ResultCode::MEETING_NOT_LOCAL),
              ResultDisposition::ERROR);
    EXPECT_EQ(ResultDispositionFor(ResultCode::MEETING_STATE_REJECTED),
              ResultDisposition::ERROR);
    EXPECT_EQ(ResultDispositionFor(ResultCode::INTERNAL_ERROR),
              ResultDisposition::ERROR);
}

// ==============================================================================
// MeetingServiceDisconnect —— D1–D8 与 0 / 1 / N fan-out
// ==============================================================================

TEST(MeetingServiceDisconnect, D1OneOfTwoSessionsKeepsParticipantActive)
{
    ServiceFixture fixture;
    const MeetingId meeting_id = CreateMeetingFor(fixture, "s-host").meeting.meeting_id;
    fixture.RegisterAuthenticated("s-user-1", kUserAUid);
    fixture.RegisterAuthenticated("s-user-2", kUserAUid);
    JoinAs(fixture, "s-user-1", kUserAUid, meeting_id, "req-join-1");
    JoinAs(fixture, "s-user-2", kUserAUid, meeting_id, "req-join-2");

    const SessionDisconnectedResult disconnected =
        fixture.service().SessionDisconnected("s-user-1");

    EXPECT_EQ(disconnected.affected_meeting_count, 1u);
    EXPECT_TRUE(disconnected.events.empty());
    EXPECT_EQ(disconnected.final_state, DomainSessionState::CLOSED);

    const ListParticipantsResult listed =
        ListAs(fixture, "s-host", kHostUid, meeting_id, "req-list-1");
    bool found = false;
    EXPECT_EQ(ParticipantStateOf(listed, kUserAUid, found), ParticipantState::ACTIVE);
    EXPECT_TRUE(found);
    EXPECT_EQ(listed.meeting.active_participant_count, std::size_t(2));
    // 只清除了该 session 的 ref，另一条仍然存在。
    EXPECT_EQ(fixture.service().ReverseBindingCountForSession("s-user-1"), 0u);
    EXPECT_EQ(fixture.service().ReverseBindingCountForSession("s-user-2"), 1u);
}

TEST(MeetingServiceDisconnect, D2LastSessionMakesOrdinaryParticipantLeftOnce)
{
    ServiceFixture fixture;
    const MeetingId meeting_id = CreateMeetingFor(fixture, "s-host").meeting.meeting_id;
    fixture.RegisterAuthenticated("s-user", kUserAUid);
    JoinAs(fixture, "s-user", kUserAUid, meeting_id, "req-join-1");
    fixture.SetNow(15);

    const SessionDisconnectedResult disconnected =
        fixture.service().SessionDisconnected("s-user");

    ASSERT_EQ(disconnected.events.size(), 1u);
    const MeetingEvent& event = disconnected.events[0];
    EXPECT_EQ(event.event_type, EventType::PARTICIPANT_LEFT);
    EXPECT_EQ(event.meeting_id, meeting_id);
    EXPECT_EQ(event.meeting_state_after, MeetingState::ACTIVE);
    EXPECT_EQ(event.occurred_at, AtSeconds(15));
    // 系统内部清理：不携带 request_id，也不伪造 actor。
    EXPECT_FALSE(event.has_request_id);
    EXPECT_TRUE(event.request_id.empty());
    EXPECT_FALSE(event.has_actor_user_id);
    EXPECT_TRUE(event.has_participant_user_id);
    EXPECT_EQ(event.participant_user_id, kUserAUid);

    const ListParticipantsResult listed =
        ListAs(fixture, "s-host", kHostUid, meeting_id, "req-list-1");
    EXPECT_EQ(listed.participants.size(), 1u);
    EXPECT_EQ(listed.meeting.active_participant_count, std::size_t(1));
}

TEST(MeetingServiceDisconnect, D3HostLastSessionLeavesHostActiveWithoutEvent)
{
    ServiceFixture fixture;
    const MeetingId meeting_id = CreateMeetingFor(fixture, "s-host").meeting.meeting_id;
    // Host 的第二条 Session：用于在第一条断开后仍然可以读回会议状态。
    fixture.RegisterAuthenticated("s-host-2", kHostUid);
    EXPECT_EQ(fixture.service().ReverseBindingCountForSession("s-host-2"), 0u);

    const SessionDisconnectedResult disconnected =
        fixture.service().SessionDisconnected("s-host");

    EXPECT_EQ(disconnected.affected_meeting_count, 1u);
    EXPECT_TRUE(disconnected.events.empty());

    EXPECT_TRUE(fixture.service().HasActiveMeeting(meeting_id));
    EXPECT_FALSE(fixture.service().HasClosedSnapshot(meeting_id));
    // Host 允许 "ACTIVE + 0 个有效 Session"（I4）：成员数不变、无事件。
    const ListParticipantsResult listed =
        ListAs(fixture, "s-host-2", kHostUid, meeting_id, "req-list-1");
    EXPECT_EQ(listed.common.result_code, ResultCode::OK);
    EXPECT_EQ(listed.meeting.active_participant_count, std::size_t(1));
    ASSERT_EQ(listed.participants.size(), 1u);
    EXPECT_EQ(listed.participants[0].user_id, kHostUid);
    EXPECT_EQ(listed.participants[0].role, ParticipantRole::HOST);
    EXPECT_EQ(listed.participants[0].participant_state, ParticipantState::ACTIVE);
    EXPECT_EQ(fixture.service().ReverseBindingCountForSession("s-host"), 0u);
}

TEST(MeetingServiceDisconnect, D3bHostDisconnectAcrossMultipleMeetingsKeepsAllMeetingsOpen)
{
    ServiceFixture fixture;
    const MeetingId m1 = CreateMeetingFor(fixture, "s-host", kHostUid, "req-c1")
                             .meeting.meeting_id;
    const MeetingId m2 = CreateMeetingFor(fixture, "s-host", kHostUid, "req-c2")
                             .meeting.meeting_id;
    EXPECT_EQ(fixture.service().ReverseBindingCountForSession("s-host"), 2u);

    const SessionDisconnectedResult disconnected =
        fixture.service().SessionDisconnected("s-host");

    EXPECT_EQ(disconnected.affected_meeting_count, 2u);
    EXPECT_TRUE(disconnected.events.empty());
    EXPECT_TRUE(fixture.service().HasActiveMeeting(m1));
    EXPECT_TRUE(fixture.service().HasActiveMeeting(m2));
    EXPECT_FALSE(fixture.service().HasClosedSnapshot(m1));
    EXPECT_FALSE(fixture.service().HasClosedSnapshot(m2));
}

TEST(MeetingServiceDisconnect, D4SessionWithZeroMeetingsClosesNormally)
{
    ServiceFixture fixture;
    fixture.RegisterAuthenticated("s-user", kUserAUid);

    const SessionDisconnectedResult disconnected =
        fixture.service().SessionDisconnected("s-user");

    EXPECT_TRUE(disconnected.session_known);
    EXPECT_FALSE(disconnected.already_closed);
    EXPECT_EQ(disconnected.affected_meeting_count, 0u);
    EXPECT_TRUE(disconnected.events.empty());

    DomainSessionState state = DomainSessionState::CONNECTED;
    EXPECT_TRUE(fixture.service().TryGetDomainSessionState("s-user", state));
    EXPECT_EQ(state, DomainSessionState::CLOSED);
}

TEST(MeetingServiceDisconnect, D5SessionInThreeMeetingsFansOutToAll)
{
    ServiceFixture fixture;
    const MeetingId m1 = CreateMeetingFor(fixture, "s-host-1", kHostUid, "req-c1", 1)
                             .meeting.meeting_id;
    const MeetingId m2 = CreateMeetingFor(fixture, "s-host-2", kHost2Uid, "req-c2", 1)
                             .meeting.meeting_id;
    const MeetingId m3 = CreateMeetingFor(fixture, "s-host-3", kHost3Uid, "req-c3", 1)
                             .meeting.meeting_id;
    fixture.RegisterAuthenticated("s-user", kUserAUid);
    JoinAs(fixture, "s-user", kUserAUid, m1, "req-j1");
    JoinAs(fixture, "s-user", kUserAUid, m2, "req-j2");
    JoinAs(fixture, "s-user", kUserAUid, m3, "req-j3");

    const SessionDisconnectedResult disconnected =
        fixture.service().SessionDisconnected("s-user");

    EXPECT_EQ(disconnected.affected_meeting_count, 3u);
    // 三个 Meeting 各自真实发生一次 ACTIVE → LEFT。
    EXPECT_EQ(disconnected.events.size(), 3u);
    EXPECT_EQ(fixture.service().ReverseBindingCountForSession("s-user"), 0u);
}

TEST(MeetingServiceDisconnect, D6OneMeetingVerdictDoesNotAffectAnother)
{
    ServiceFixture fixture;
    const MeetingId m1 = CreateMeetingFor(fixture, "s-host-1", kHostUid, "req-c1", 1)
                             .meeting.meeting_id;
    const MeetingId m2 = CreateMeetingFor(fixture, "s-host-2", kHost2Uid, "req-c2", 1)
                             .meeting.meeting_id;
    fixture.RegisterAuthenticated("s-user", kUserAUid);
    fixture.RegisterAuthenticated("s-user-extra", kUserAUid);
    // M1：只有 s-user 一条 Binding → 断开后变为 LEFT。
    JoinAs(fixture, "s-user", kUserAUid, m1, "req-j1");
    // M2：s-user 与 s-user-extra 两条 Binding → 断开一条后仍 ACTIVE。
    JoinAs(fixture, "s-user", kUserAUid, m2, "req-j2");
    ASSERT_EQ(JoinAs(fixture, "s-user-extra", kUserAUid, m2, "req-j3").outcome,
              JoinOutcome::ADDITIONAL_SESSION_BOUND);

    const SessionDisconnectedResult disconnected =
        fixture.service().SessionDisconnected("s-user");

    // 只有 M1 产生事件；M2 的裁决独立。
    ASSERT_EQ(disconnected.events.size(), 1u);
    EXPECT_EQ(disconnected.events[0].meeting_id, m1);

    const ListParticipantsResult m1_list =
        ListAs(fixture, "s-host-1", kHostUid, m1, "req-l1");
    EXPECT_EQ(m1_list.participants.size(), 1u);
    EXPECT_EQ(m1_list.meeting.active_participant_count, std::size_t(1));

    const ListParticipantsResult m2_list =
        ListAs(fixture, "s-host-2", kHost2Uid, m2, "req-l2");
    EXPECT_EQ(m2_list.participants.size(), 2u);
    EXPECT_EQ(m2_list.meeting.active_participant_count, std::size_t(2));
    bool found = false;
    EXPECT_EQ(ParticipantStateOf(m2_list, kUserAUid, found), ParticipantState::ACTIVE);
    EXPECT_TRUE(found);
}

TEST(MeetingServiceDisconnect, D7SessionIsClosedOnlyAfterAllFanOutCleanup)
{
    ServiceFixture fixture;
    const MeetingId m1 = CreateMeetingFor(fixture, "s-host-1", kHostUid, "req-c1", 1)
                             .meeting.meeting_id;
    const MeetingId m2 = CreateMeetingFor(fixture, "s-host-2", kHost2Uid, "req-c2", 1)
                             .meeting.meeting_id;
    fixture.RegisterAuthenticated("s-user", kUserAUid);
    JoinAs(fixture, "s-user", kUserAUid, m1, "req-j1");
    JoinAs(fixture, "s-user", kUserAUid, m2, "req-j2");

    fixture.service().SessionDisconnected("s-user");

    DomainSessionState state = DomainSessionState::CONNECTED;
    ASSERT_TRUE(fixture.service().TryGetDomainSessionState("s-user", state));
    ASSERT_EQ(state, DomainSessionState::CLOSED);
    // 已 CLOSED 的 Session 不得在任何 Meeting 中留下未清理的 Binding。
    EXPECT_EQ(fixture.service().ReverseBindingCountForSession("s-user"), 0u);
    EXPECT_TRUE(fixture.service().HasActiveMeeting(m1));
    EXPECT_TRUE(fixture.service().HasActiveMeeting(m2));
}

TEST(MeetingServiceDisconnect, D8StaleDisconnectAfterNewSessionKeepsParticipantActive)
{
    ServiceFixture fixture;
    const MeetingId meeting_id = CreateMeetingFor(fixture, "s-host").meeting.meeting_id;
    fixture.RegisterAuthenticated("s-user-a", kUserAUid);
    fixture.RegisterAuthenticated("s-user-b", kUserAUid);
    JoinAs(fixture, "s-user-a", kUserAUid, meeting_id, "req-join-1");
    // 新 Session 加入后，旧 Session 的 stale disconnect 才到达。
    ASSERT_EQ(JoinAs(fixture, "s-user-b", kUserAUid, meeting_id, "req-join-2").outcome,
              JoinOutcome::ADDITIONAL_SESSION_BOUND);

    const SessionDisconnectedResult disconnected =
        fixture.service().SessionDisconnected("s-user-a");

    EXPECT_TRUE(disconnected.events.empty());
    const ListParticipantsResult listed =
        ListAs(fixture, "s-host", kHostUid, meeting_id, "req-list-1");
    bool found = false;
    EXPECT_EQ(ParticipantStateOf(listed, kUserAUid, found), ParticipantState::ACTIVE);
    EXPECT_TRUE(found);
    EXPECT_EQ(listed.meeting.active_participant_count, std::size_t(2));
    EXPECT_EQ(fixture.service().ReverseBindingCountForSession("s-user-b"), 1u);
}

TEST(MeetingServiceDisconnect, EventCountFollowsPerMeetingVerdictOnly)
{
    // Step 1.5 §12.1：ParticipantLeft 的粒度是 (meeting_id, user_id)。
    // 同一 Session 跨 M1/M2/M3，只有真实发生离会的 Meeting 各产生一个事件。
    ServiceFixture fixture;
    const MeetingId m1 = CreateMeetingFor(fixture, "s-host-1", kHostUid, "req-c1", 1)
                             .meeting.meeting_id;
    const MeetingId m2 = CreateMeetingFor(fixture, "s-host-2", kHost2Uid, "req-c2", 1)
                             .meeting.meeting_id;
    const MeetingId m3 = CreateMeetingFor(fixture, "s-host-3", kHost3Uid, "req-c3", 1)
                             .meeting.meeting_id;
    fixture.RegisterAuthenticated("s-user", kUserAUid);
    fixture.RegisterAuthenticated("s-user-2", kUserAUid);

    JoinAs(fixture, "s-user", kUserAUid, m1, "req-j1");
    JoinAs(fixture, "s-user", kUserAUid, m2, "req-j2");
    // M2 多一条 Binding：断开 s-user 后该 Meeting 仍保持 ACTIVE。
    ASSERT_EQ(JoinAs(fixture, "s-user-2", kUserAUid, m2, "req-j3").outcome,
              JoinOutcome::ADDITIONAL_SESSION_BOUND);
    JoinAs(fixture, "s-user", kUserAUid, m3, "req-j4");

    const SessionDisconnectedResult disconnected =
        fixture.service().SessionDisconnected("s-user");

    EXPECT_EQ(disconnected.affected_meeting_count, 3u);
    ASSERT_EQ(disconnected.events.size(), 2u);
    // 不是 1 个、不是 3 个，也不允许重复。
    EXPECT_EQ(disconnected.events[0].meeting_id, m1);
    EXPECT_EQ(disconnected.events[1].meeting_id, m3);
    EXPECT_NE(disconnected.events[0].event_id, disconnected.events[1].event_id);
    for (std::size_t i = 0; i < disconnected.events.size(); ++i) {
        EXPECT_EQ(disconnected.events[i].event_type, EventType::PARTICIPANT_LEFT);
        EXPECT_FALSE(disconnected.events[i].has_request_id);
        EXPECT_FALSE(disconnected.events[i].has_actor_user_id);
        EXPECT_TRUE(disconnected.events[i].has_participant_user_id);
        EXPECT_EQ(disconnected.events[i].participant_user_id, kUserAUid);
    }
}

TEST(MeetingServiceDisconnect, EndingMeetingOnlyUnbindsWithoutMembershipChange)
{
    ServiceFixture fixture;
    const MeetingId meeting_id = CreateMeetingFor(fixture, "s-host").meeting.meeting_id;
    fixture.RegisterAuthenticated("s-user", kUserAUid);
    JoinAs(fixture, "s-user", kUserAUid, meeting_id, "req-join-1");
    ASSERT_EQ(CloseAs(fixture, "s-host", kHostUid, meeting_id, "req-close-1")
                  .common.result_code,
              ResultCode::OK);

    const SessionDisconnectedResult disconnected =
        fixture.service().SessionDisconnected("s-user");

    // membership 已冻结给 close flow：只做必要的幂等 UNBOUND。
    EXPECT_EQ(disconnected.affected_meeting_count, 1u);
    EXPECT_TRUE(disconnected.events.empty());
    EXPECT_EQ(fixture.service().ReverseBindingCountForSession("s-user"), 0u);

    fixture.SetNow(10);
    const FinalizeCloseResult finalized = fixture.service().FinalizeClose(meeting_id);
    ASSERT_TRUE(finalized.finalized);
    // 关闭边界上的 ACTIVE 计数没有被 disconnect 改动。
    EXPECT_EQ(finalized.snapshot.final_participant_count, std::size_t(2));
}

TEST(MeetingServiceDisconnect, DuplicateDisconnectProducesNoSecondMutationOrEvent)
{
    ServiceFixture fixture;
    const MeetingId meeting_id = CreateMeetingFor(fixture, "s-host").meeting.meeting_id;
    fixture.RegisterAuthenticated("s-user", kUserAUid);
    JoinAs(fixture, "s-user", kUserAUid, meeting_id, "req-join-1");

    const SessionDisconnectedResult first =
        fixture.service().SessionDisconnected("s-user");
    ASSERT_EQ(first.events.size(), 1u);

    const SessionDisconnectedResult second =
        fixture.service().SessionDisconnected("s-user");

    EXPECT_TRUE(second.session_known);
    EXPECT_TRUE(second.already_closed);
    EXPECT_TRUE(second.events.empty());

    const ListParticipantsResult listed =
        ListAs(fixture, "s-host", kHostUid, meeting_id, "req-list-1");
    EXPECT_EQ(listed.meeting.active_participant_count, std::size_t(1));
}

TEST(MeetingServiceDisconnect, DisconnectAfterLeavePerformsNoMeetingMutation)
{
    // Step 1.6 §19.4 场景：Leave 已解绑全部 Bindings，之后的 disconnect 不得再
    // 触碰 membership，也不得产生 ParticipantLeft。
    ServiceFixture fixture;
    const MeetingId meeting_id = CreateMeetingFor(fixture, "s-host").meeting.meeting_id;
    fixture.RegisterAuthenticated("s-user-a", kUserAUid);
    fixture.RegisterAuthenticated("s-user-b", kUserAUid);
    JoinAs(fixture, "s-user-a", kUserAUid, meeting_id, "req-join-1");
    ASSERT_EQ(JoinAs(fixture, "s-user-b", kUserAUid, meeting_id, "req-join-2").outcome,
              JoinOutcome::ADDITIONAL_SESSION_BOUND);
    ASSERT_EQ(LeaveAs(fixture, "s-user-a", kUserAUid, meeting_id, "req-leave-1")
                  .common.result_code,
              ResultCode::OK);

    EXPECT_EQ(fixture.service().SessionDisconnected("s-user-a").affected_meeting_count, 0u);
    EXPECT_EQ(fixture.service().SessionDisconnected("s-user-b").affected_meeting_count, 0u);

    const ListParticipantsResult listed =
        ListAs(fixture, "s-host", kHostUid, meeting_id, "req-list-1");
    EXPECT_EQ(listed.participants.size(), 1u);
    EXPECT_EQ(listed.meeting.active_participant_count, std::size_t(1));
}

TEST(MeetingServiceDisconnect, ConnectedSessionCanBeDisconnected)
{
    // Step 1.3 §5.2：CONNECTED → CLOSING 是合法转换（不必先认证）。
    ServiceFixture fixture;
    EXPECT_TRUE(fixture.service().RegisterConnectedSession("s-raw"));

    const SessionDisconnectedResult disconnected =
        fixture.service().SessionDisconnected("s-raw");

    EXPECT_TRUE(disconnected.session_known);
    EXPECT_EQ(disconnected.affected_meeting_count, 0u);
    EXPECT_EQ(disconnected.final_state, DomainSessionState::CLOSED);
}

// ==============================================================================
// MeetingServiceReverseIndex —— 索引唯一性与生命周期
// ==============================================================================

TEST(MeetingServiceReverseIndex, CreatedMeetingRegistersHostBindingRef)
{
    ServiceFixture fixture;
    const MeetingId meeting_id = CreateMeetingFor(fixture, "s-host").meeting.meeting_id;

    EXPECT_EQ(fixture.service().ReverseBindingCountForSession("s-host"), 1u);
    EXPECT_TRUE(fixture.service().HasActiveMeeting(meeting_id));
}

TEST(MeetingServiceReverseIndex, JoinNewAndRejoinAndAdditionalAllRegisterRefs)
{
    ServiceFixture fixture;
    const MeetingId meeting_id = CreateMeetingFor(fixture, "s-host").meeting.meeting_id;
    fixture.RegisterAuthenticated("s-user-1", kUserAUid);
    fixture.RegisterAuthenticated("s-user-2", kUserAUid);

    JoinAs(fixture, "s-user-1", kUserAUid, meeting_id, "req-join-1");
    EXPECT_EQ(fixture.service().ReverseBindingCountForSession("s-user-1"), 1u);

    JoinAs(fixture, "s-user-2", kUserAUid, meeting_id, "req-join-2");
    EXPECT_EQ(fixture.service().ReverseBindingCountForSession("s-user-2"), 1u);

    LeaveAs(fixture, "s-user-1", kUserAUid, meeting_id, "req-leave-1");
    EXPECT_EQ(fixture.service().ReverseBindingCountForSession("s-user-1"), 0u);

    // Re-Join 重新登记该 session 的 ref。
    JoinAs(fixture, "s-user-1", kUserAUid, meeting_id, "req-join-3");
    EXPECT_EQ(fixture.service().ReverseBindingCountForSession("s-user-1"), 1u);
}

TEST(MeetingServiceReverseIndex, RepeatedJoinNeverDuplicatesRef)
{
    ServiceFixture fixture;
    const MeetingId meeting_id = CreateMeetingFor(fixture, "s-host").meeting.meeting_id;
    fixture.RegisterAuthenticated("s-user", kUserAUid);

    JoinAs(fixture, "s-user", kUserAUid, meeting_id, "req-join-1");
    for (int i = 0; i < 5; ++i) {
        const JoinMeetingResult duplicate =
            JoinAs(fixture, "s-user", kUserAUid, meeting_id, "req-dup");
        EXPECT_EQ(duplicate.outcome, JoinOutcome::ALREADY_BOUND);
        EXPECT_TRUE(duplicate.events.empty());
    }

    // 索引不得变成 [(M,U), (M,U), ...]。
    EXPECT_EQ(fixture.service().ReverseBindingCountForSession("s-user"), 1u);
}

TEST(MeetingServiceReverseIndex, RejectedJoinDoesNotTouchIndex)
{
    ServiceFixture fixture;
    const MeetingId meeting_id = CreateMeetingFor(fixture, "s-host").meeting.meeting_id;
    CloseAs(fixture, "s-host", kHostUid, meeting_id, "req-close-1");
    fixture.RegisterAuthenticated("s-user", kUserAUid);

    EXPECT_EQ(JoinAs(fixture, "s-user", kUserAUid, meeting_id, "req-join-1")
                  .common.result_code,
              ResultCode::MEETING_STATE_REJECTED);
    EXPECT_EQ(fixture.service().ReverseBindingCountForSession("s-user"), 0u);
}

TEST(MeetingServiceReverseIndex, SuccessfulLeaveRemovesEveryRefOfThatUser)
{
    ServiceFixture fixture;
    const MeetingId meeting_id = CreateMeetingFor(fixture, "s-host").meeting.meeting_id;
    fixture.RegisterAuthenticated("s-user-a", kUserAUid);
    fixture.RegisterAuthenticated("s-user-b", kUserAUid);
    fixture.RegisterAuthenticated("s-user-c", kUserAUid);
    JoinAs(fixture, "s-user-a", kUserAUid, meeting_id, "req-join-1");
    JoinAs(fixture, "s-user-b", kUserAUid, meeting_id, "req-join-2");
    JoinAs(fixture, "s-user-c", kUserAUid, meeting_id, "req-join-3");

    LeaveAs(fixture, "s-user-a", kUserAUid, meeting_id, "req-leave-1");

    EXPECT_EQ(fixture.service().ReverseBindingCountForSession("s-user-a"), 0u);
    EXPECT_EQ(fixture.service().ReverseBindingCountForSession("s-user-b"), 0u);
    EXPECT_EQ(fixture.service().ReverseBindingCountForSession("s-user-c"), 0u);
}

TEST(MeetingServiceReverseIndex, DisconnectRemovesOnlyThatSessionsRefs)
{
    ServiceFixture fixture;
    const MeetingId m1 = CreateMeetingFor(fixture, "s-host-1", kHostUid, "req-c1", 1)
                             .meeting.meeting_id;
    const MeetingId m2 = CreateMeetingFor(fixture, "s-host-2", kHost2Uid, "req-c2", 1)
                             .meeting.meeting_id;
    fixture.RegisterAuthenticated("s-user", kUserAUid);
    JoinAs(fixture, "s-user", kUserAUid, m1, "req-j1");
    JoinAs(fixture, "s-user", kUserAUid, m2, "req-j2");
    ASSERT_EQ(fixture.service().ReverseBindingCountForSession("s-user"), 2u);

    fixture.service().SessionDisconnected("s-user");

    EXPECT_EQ(fixture.service().ReverseBindingCountForSession("s-user"), 0u);
    // Host 的 refs 不受影响。
    EXPECT_EQ(fixture.service().ReverseBindingCountForSession("s-host-1"), 1u);
    EXPECT_EQ(fixture.service().ReverseBindingCountForSession("s-host-2"), 1u);
}

TEST(MeetingServiceReverseIndex, FinalizeRemovesAllRemainingRefsOfThatMeeting)
{
    ServiceFixture fixture;
    const MeetingId m1 = CreateMeetingFor(fixture, "s-host-1", kHostUid, "req-c1", 1)
                             .meeting.meeting_id;
    const MeetingId m2 = CreateMeetingFor(fixture, "s-host-2", kHost2Uid, "req-c2", 1)
                             .meeting.meeting_id;
    fixture.RegisterAuthenticated("s-user", kUserAUid);
    JoinAs(fixture, "s-user", kUserAUid, m1, "req-j1");
    JoinAs(fixture, "s-user", kUserAUid, m2, "req-j2");

    CloseAs(fixture, "s-host-1", kHostUid, m1, "req-close");
    fixture.SetNow(10);
    ASSERT_TRUE(fixture.service().FinalizeClose(m1).finalized);

    EXPECT_EQ(fixture.service().ReverseBindingCountForSession("s-host-1"), 0u);
    // 另一 Meeting 的索引完全不受影响。
    EXPECT_EQ(fixture.service().ReverseBindingCountForSession("s-user"), 1u);
    EXPECT_EQ(fixture.service().ReverseBindingCountForSession("s-host-2"), 1u);
}

TEST(MeetingServiceReverseIndex, DisconnectAfterFinalizeDoesNotTouchClosedMeeting)
{
    ServiceFixture fixture;
    const MeetingId meeting_id = CreateMeetingFor(fixture, "s-host").meeting.meeting_id;
    fixture.RegisterAuthenticated("s-user", kUserAUid);
    JoinAs(fixture, "s-user", kUserAUid, meeting_id, "req-join-1");
    CloseAs(fixture, "s-host", kHostUid, meeting_id, "req-close-1");
    fixture.SetNow(10);
    ASSERT_TRUE(fixture.service().FinalizeClose(meeting_id).finalized);

    const SessionDisconnectedResult disconnected =
        fixture.service().SessionDisconnected("s-user");

    // 该 Meeting 的 refs 已在 finalize 时清除，因此 disconnect 不再触碰它。
    EXPECT_EQ(disconnected.affected_meeting_count, 0u);
    EXPECT_TRUE(disconnected.events.empty());
    EXPECT_TRUE(fixture.service().HasClosedSnapshot(meeting_id));
    EXPECT_FALSE(fixture.service().HasActiveMeeting(meeting_id));
    EXPECT_EQ(QueryAs(fixture, "s-host", kHostUid, meeting_id, "req-q1")
                  .meeting.meeting_state,
              MeetingState::CLOSED);
}

TEST(MeetingServiceReverseIndex, StableStateHasNoBoundRefForClosedOrLeftMembership)
{
    // I1 的索引侧证据：Meeting 关闭后不得留下任何有效 Binding ref。
    ServiceFixture fixture;
    const MeetingId meeting_id = CreateMeetingFor(fixture, "s-host").meeting.meeting_id;
    fixture.RegisterAuthenticated("s-user-a", kUserAUid);
    fixture.RegisterAuthenticated("s-user-b", kUserBUid);
    JoinAs(fixture, "s-user-a", kUserAUid, meeting_id, "req-j1");
    JoinAs(fixture, "s-user-b", kUserBUid, meeting_id, "req-j2");
    LeaveAs(fixture, "s-user-a", kUserAUid, meeting_id, "req-l1");
    CloseAs(fixture, "s-host", kHostUid, meeting_id, "req-close");
    fixture.SetNow(10);
    ASSERT_TRUE(fixture.service().FinalizeClose(meeting_id).finalized);

    EXPECT_EQ(fixture.service().ReverseBindingCountForSession("s-host"), 0u);
    EXPECT_EQ(fixture.service().ReverseBindingCountForSession("s-user-a"), 0u);
    EXPECT_EQ(fixture.service().ReverseBindingCountForSession("s-user-b"), 0u);
}

// ==============================================================================
// MeetingServiceEventContract —— 事件契约回归
// ==============================================================================

TEST(MeetingServiceEventContract, RequestIdIsCorrelationOnly)
{
    ServiceFixture fixture;
    const MeetingId meeting_id = CreateMeetingFor(fixture, "s-host", kHostUid,
                                                 "req-shared", 1).meeting.meeting_id;
    fixture.RegisterAuthenticated("s-user", kUserAUid);

    const JoinMeetingResult first =
        JoinAs(fixture, "s-user", kUserAUid, meeting_id, "req-shared");
    const JoinMeetingResult duplicate =
        JoinAs(fixture, "s-user", kUserAUid, meeting_id, "req-shared");

    // 幂等命中不产生第二个事件：幂等来自领域状态，不是 request_id 去重。
    ASSERT_EQ(first.events.size(), 1u);
    EXPECT_TRUE(duplicate.events.empty());
    EXPECT_EQ(duplicate.common.result_code, ResultCode::ALREADY_JOINED);
}

TEST(MeetingServiceEventContract, DedupeContextIsNotAnIdempotencyKey)
{
    ServiceFixture fixture;
    const MeetingId meeting_id = CreateMeetingFor(fixture, "s-host").meeting.meeting_id;
    fixture.RegisterAuthenticated("s-user-a", kUserAUid);
    fixture.RegisterAuthenticated("s-user-b", kUserBUid);
    JoinAs(fixture, "s-user-a", kUserAUid, meeting_id, "req-j1");
    LeaveAs(fixture, "s-user-a", kUserAUid, meeting_id, "req-l1");

    const JoinMeetingResult rejoined =
        JoinAs(fixture, "s-user-a", kUserAUid, meeting_id, "req-j2");

    // 两次 ParticipantJoined 的 dedupe_context 相同（同一动作 + meeting + user），
    // 但它们是两个独立事件：dedupe_context 只是最小去重上下文，不是幂等键。
    ASSERT_EQ(rejoined.events.size(), 1u);
    EXPECT_EQ(rejoined.events[0].dedupe_context,
              std::string("PARTICIPANT_JOINED:") + meeting_id + ":" +
                  std::to_string(kUserAUid));
    EXPECT_EQ(rejoined.events[0].event_type, EventType::PARTICIPANT_JOINED);
    EXPECT_NE(rejoined.events[0].event_id, std::string("event-2"));
}

TEST(MeetingServiceEventContract, ParticipantLeftStateAfterReflectsCurrentMeetingState)
{
    ServiceFixture fixture;
    const MeetingId meeting_id = CreateMeetingFor(fixture, "s-host").meeting.meeting_id;
    fixture.RegisterAuthenticated("s-user", kUserAUid);
    JoinAs(fixture, "s-user", kUserAUid, meeting_id, "req-j1");

    const LeaveMeetingResult left =
        LeaveAs(fixture, "s-user", kUserAUid, meeting_id, "req-l1");

    ASSERT_EQ(left.events.size(), 1u);
    // 成员离会不得使 Meeting 状态回退，因此 state_after 为当前真实状态。
    EXPECT_EQ(left.events[0].meeting_state_after, MeetingState::ACTIVE);
}

TEST(MeetingServiceEventContract, MeetingClosedRequestIdTracesOriginatingCloseCommand)
{
    ServiceFixture fixture;
    const MeetingId meeting_id = CreateMeetingFor(fixture, "s-host").meeting.meeting_id;
    ASSERT_EQ(CloseAs(fixture, "s-host", kHostUid, meeting_id, "req-close-origin")
                  .common.result_code,
              ResultCode::OK);
    fixture.SetNow(11);

    const FinalizeCloseResult finalized = fixture.service().FinalizeClose(meeting_id);

    ASSERT_EQ(finalized.events.size(), 1u);
    EXPECT_TRUE(finalized.events[0].has_request_id);
    EXPECT_EQ(finalized.events[0].request_id, std::string("req-close-origin"));
    EXPECT_EQ(finalized.events[0].meeting_state_after, MeetingState::CLOSED);
    EXPECT_EQ(finalized.events[0].occurred_at, AtSeconds(11));
}

TEST(MeetingServiceEventContract, BeginCloseItselfProducesNoEvent)
{
    ServiceFixture fixture;
    const MeetingId meeting_id = CreateMeetingFor(fixture, "s-host").meeting.meeting_id;

    const CloseMeetingResult closed =
        CloseAs(fixture, "s-host", kHostUid, meeting_id, "req-close-1");

    EXPECT_TRUE(closed.events.empty());
    // 只有第一次成功 FinalizeClose 才产生 MeetingClosed。
    EXPECT_FALSE(fixture.service().HasClosedSnapshot(meeting_id));
}
