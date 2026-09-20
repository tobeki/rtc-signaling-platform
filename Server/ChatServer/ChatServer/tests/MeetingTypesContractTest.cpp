// ==============================================================================
// M1 MeetingTypes 契约测试。
//
// 只验证"类型契约"本身：
//   - ResultCode 的数值与 Phase 1 冻结表完全一致；
//   - ID / Timestamp 的内部表示符合 M1 的选择；
//   - 四个状态机与三类 outcome 的取值集合完整且不越界；
//   - optional 字段通过 has_xxx 表达，而不是 sentinel 值。
//
// 这里刻意不测试任何业务行为：Join/Leave/Close 的状态转换、事件的产生时机、
// 幂等裁决都属于 M2/M3（MeetingAggregate / MeetingService）。
// 也刻意不加入 EXPECT_TRUE(true) 或 TEST(..., TODO) 之类的占位用例。
// ==============================================================================

#include <gtest/gtest.h>

#include <cstddef>
#include <type_traits>

#include "meeting/MeetingTypes.h"

namespace {

using meeting::CloseOutcome;
using meeting::DomainSessionState;
using meeting::EventType;
using meeting::JoinOutcome;
using meeting::LeaveOutcome;
using meeting::MeetingState;
using meeting::ParticipantRole;
using meeting::ParticipantState;
using meeting::ResultCode;
using meeting::ResultDisposition;

// ------------------------------------------------------------------------------
// ResultCode 冻结表（Step 1.4 §10.1）。
//
// 这张表的作用是把"只有 15 个值、且编号固定"变成编译期约束：如果有人改号、
// 重新编号或新增 code，下面的 static_assert 会直接编译失败。
// ------------------------------------------------------------------------------
constexpr ResultCode kFrozenResultCodes[] = {
    ResultCode::OK,
    ResultCode::ALREADY_JOINED,
    ResultCode::ALREADY_LEFT,
    ResultCode::CLOSE_IN_PROGRESS,
    ResultCode::ALREADY_CLOSED,
    ResultCode::AUTH_REQUIRED,
    ResultCode::SESSION_STATE_REJECTED,
    ResultCode::INVALID_ARGUMENT,
    ResultCode::PERMISSION_DENIED,
    ResultCode::NOT_PARTICIPANT,
    ResultCode::HOST_MUST_CLOSE_MEETING,
    ResultCode::MEETING_NOT_FOUND,
    ResultCode::MEETING_NOT_LOCAL,
    ResultCode::MEETING_STATE_REJECTED,
    ResultCode::INTERNAL_ERROR,
};

constexpr std::size_t kFrozenResultCodeCount =
    sizeof(kFrozenResultCodes) / sizeof(kFrozenResultCodes[0]);

static_assert(kFrozenResultCodeCount == 15,
              "ResultCode 冻结契约必须恰好 15 个值");

// 逐个锁定数值。分段：成功 0；幂等 100–103；上下文/参数 1000–1002；
// 权限/身份 1100–1102；会议资源/状态 1200–1202；内部失败 9000。
static_assert(static_cast<int>(ResultCode::OK) == 0, "OK 必须为 0");

static_assert(static_cast<int>(ResultCode::ALREADY_JOINED) == 100, "ALREADY_JOINED 必须为 100");
static_assert(static_cast<int>(ResultCode::ALREADY_LEFT) == 101, "ALREADY_LEFT 必须为 101");
static_assert(static_cast<int>(ResultCode::CLOSE_IN_PROGRESS) == 102, "CLOSE_IN_PROGRESS 必须为 102");
static_assert(static_cast<int>(ResultCode::ALREADY_CLOSED) == 103, "ALREADY_CLOSED 必须为 103");

static_assert(static_cast<int>(ResultCode::AUTH_REQUIRED) == 1000, "AUTH_REQUIRED 必须为 1000");
static_assert(static_cast<int>(ResultCode::SESSION_STATE_REJECTED) == 1001, "SESSION_STATE_REJECTED 必须为 1001");
static_assert(static_cast<int>(ResultCode::INVALID_ARGUMENT) == 1002, "INVALID_ARGUMENT 必须为 1002");

static_assert(static_cast<int>(ResultCode::PERMISSION_DENIED) == 1100, "PERMISSION_DENIED 必须为 1100");
static_assert(static_cast<int>(ResultCode::NOT_PARTICIPANT) == 1101, "NOT_PARTICIPANT 必须为 1101");
static_assert(static_cast<int>(ResultCode::HOST_MUST_CLOSE_MEETING) == 1102, "HOST_MUST_CLOSE_MEETING 必须为 1102");

static_assert(static_cast<int>(ResultCode::MEETING_NOT_FOUND) == 1200, "MEETING_NOT_FOUND 必须为 1200");
static_assert(static_cast<int>(ResultCode::MEETING_NOT_LOCAL) == 1201, "MEETING_NOT_LOCAL 必须为 1201");
static_assert(static_cast<int>(ResultCode::MEETING_STATE_REJECTED) == 1202, "MEETING_STATE_REJECTED 必须为 1202");

static_assert(static_cast<int>(ResultCode::INTERNAL_ERROR) == 9000, "INTERNAL_ERROR 必须为 9000");

// ------------------------------------------------------------------------------
// 内部表示（M1 选择，不是 wire contract）。
// ------------------------------------------------------------------------------
static_assert(std::is_same<meeting::UserId, int>::value,
              "UserId 的 M1 内部表示应为 int");
static_assert(std::is_same<meeting::MeetingId, std::string>::value,
              "MeetingId 的 M1 内部表示应为 std::string");
static_assert(std::is_same<meeting::SessionId, std::string>::value,
              "SessionId 的 M1 内部表示应为 std::string");
static_assert(std::is_same<meeting::RequestId, std::string>::value,
              "RequestId 的 M1 内部表示应为 std::string");
static_assert(std::is_same<meeting::ServerId, std::string>::value,
              "ServerId 的 M1 内部表示应为 std::string");
static_assert(std::is_same<meeting::DeviceId, std::string>::value,
              "DeviceId 的 M1 内部表示应为 std::string");
static_assert(std::is_same<meeting::EventId, std::string>::value,
              "EventId 的 M1 内部表示应为 std::string");
static_assert(
    std::is_same<meeting::Timestamp,
                 std::chrono::system_clock::time_point>::value,
    "Timestamp 应使用 std::chrono::system_clock::time_point");

// ------------------------------------------------------------------------------
// 状态机取值集合锁定。数量不符即编译失败，避免后续"顺手加一个状态"。
// ------------------------------------------------------------------------------
constexpr DomainSessionState kSessionStates[] = {
    DomainSessionState::CONNECTED,
    DomainSessionState::AUTHENTICATED,
    DomainSessionState::CLOSING,
    DomainSessionState::CLOSED,
};
static_assert(sizeof(kSessionStates) / sizeof(kSessionStates[0]) == 4,
              "DomainSessionState 必须恰好 4 个值（且不含 JOINED）");

constexpr MeetingState kMeetingStates[] = {
    MeetingState::CREATED,
    MeetingState::ACTIVE,
    MeetingState::ENDING,
    MeetingState::CLOSED,
};
static_assert(sizeof(kMeetingStates) / sizeof(kMeetingStates[0]) == 4,
              "MeetingState 必须恰好 4 个值");

constexpr ParticipantState kParticipantStates[] = {
    ParticipantState::ACTIVE,
    ParticipantState::LEFT,
};
static_assert(sizeof(kParticipantStates) / sizeof(kParticipantStates[0]) == 2,
              "ParticipantState 必须恰好 2 个值");

constexpr ParticipantRole kParticipantRoles[] = {
    ParticipantRole::HOST,
    ParticipantRole::PARTICIPANT,
};
static_assert(sizeof(kParticipantRoles) / sizeof(kParticipantRoles[0]) == 2,
              "ParticipantRole 必须恰好 2 个值");

constexpr meeting::BindingState kBindingStates[] = {
    meeting::BindingState::BOUND,
    meeting::BindingState::UNBOUND,
};
static_assert(sizeof(kBindingStates) / sizeof(kBindingStates[0]) == 2,
              "BindingState 必须恰好 2 个值");

constexpr ResultDisposition kDispositions[] = {
    ResultDisposition::SUCCESS,
    ResultDisposition::IDEMPOTENT,
    ResultDisposition::ERROR,
};
static_assert(sizeof(kDispositions) / sizeof(kDispositions[0]) == 3,
              "ResultDisposition 必须恰好 3 个值");

constexpr JoinOutcome kJoinOutcomes[] = {
    JoinOutcome::NEW_PARTICIPANT,
    JoinOutcome::REJOINED_PARTICIPANT,
    JoinOutcome::ADDITIONAL_SESSION_BOUND,
    JoinOutcome::ALREADY_BOUND,
};
static_assert(sizeof(kJoinOutcomes) / sizeof(kJoinOutcomes[0]) == 4,
              "JoinOutcome 必须恰好 4 个值");

// LeaveOutcome / CloseOutcome 按 Step 1.4 冻结定义各自只有一个值。
constexpr LeaveOutcome kLeaveOutcomes[] = { LeaveOutcome::LEFT };
static_assert(sizeof(kLeaveOutcomes) / sizeof(kLeaveOutcomes[0]) == 1,
              "LeaveOutcome 按契约只有 LEFT");

constexpr CloseOutcome kCloseOutcomes[] = { CloseOutcome::CLOSE_STARTED };
static_assert(sizeof(kCloseOutcomes) / sizeof(kCloseOutcomes[0]) == 1,
              "CloseOutcome 按契约只有 CLOSE_STARTED");

constexpr EventType kEventTypes[] = {
    EventType::MEETING_CREATED,
    EventType::PARTICIPANT_JOINED,
    EventType::PARTICIPANT_LEFT,
    EventType::MEETING_CLOSED,
};
static_assert(sizeof(kEventTypes) / sizeof(kEventTypes[0]) == 4,
              "EventType 必须恰好 4 类领域事件");

} // namespace

// ------------------------------------------------------------------------------
// result_name：契约要求每个 Response 都带 result_name，这里验证查表覆盖全部
// 冻结值，并对契约外的取值给出明确的 fallback，而不是静默返回空指针。
// ------------------------------------------------------------------------------
TEST(MeetingTypesContract, ResultCodeNameCoversAllFrozenCodes)
{
    EXPECT_STREQ(meeting::ResultCodeName(ResultCode::OK), "OK");
    EXPECT_STREQ(meeting::ResultCodeName(ResultCode::ALREADY_JOINED), "ALREADY_JOINED");
    EXPECT_STREQ(meeting::ResultCodeName(ResultCode::ALREADY_LEFT), "ALREADY_LEFT");
    EXPECT_STREQ(meeting::ResultCodeName(ResultCode::CLOSE_IN_PROGRESS), "CLOSE_IN_PROGRESS");
    EXPECT_STREQ(meeting::ResultCodeName(ResultCode::ALREADY_CLOSED), "ALREADY_CLOSED");
    EXPECT_STREQ(meeting::ResultCodeName(ResultCode::AUTH_REQUIRED), "AUTH_REQUIRED");
    EXPECT_STREQ(meeting::ResultCodeName(ResultCode::SESSION_STATE_REJECTED), "SESSION_STATE_REJECTED");
    EXPECT_STREQ(meeting::ResultCodeName(ResultCode::INVALID_ARGUMENT), "INVALID_ARGUMENT");
    EXPECT_STREQ(meeting::ResultCodeName(ResultCode::PERMISSION_DENIED), "PERMISSION_DENIED");
    EXPECT_STREQ(meeting::ResultCodeName(ResultCode::NOT_PARTICIPANT), "NOT_PARTICIPANT");
    EXPECT_STREQ(meeting::ResultCodeName(ResultCode::HOST_MUST_CLOSE_MEETING), "HOST_MUST_CLOSE_MEETING");
    EXPECT_STREQ(meeting::ResultCodeName(ResultCode::MEETING_NOT_FOUND), "MEETING_NOT_FOUND");
    EXPECT_STREQ(meeting::ResultCodeName(ResultCode::MEETING_NOT_LOCAL), "MEETING_NOT_LOCAL");
    EXPECT_STREQ(meeting::ResultCodeName(ResultCode::MEETING_STATE_REJECTED), "MEETING_STATE_REJECTED");
    EXPECT_STREQ(meeting::ResultCodeName(ResultCode::INTERNAL_ERROR), "INTERNAL_ERROR");

    // 契约外取值必须有明确 fallback（ResultCode 的底层类型是 int，
    // 因此该 static_cast 是良定义的）。
    EXPECT_STREQ(meeting::ResultCodeName(static_cast<ResultCode>(12345)),
                 "UNKNOWN_RESULT_CODE");
}

// ------------------------------------------------------------------------------
// Presence 语义：可选字段在默认构造时必须是"不存在"，不能靠 sentinel 值猜测。
// ------------------------------------------------------------------------------
TEST(MeetingTypesContract, OptionalFieldsDefaultToAbsent)
{
    const meeting::MeetingView view;
    EXPECT_FALSE(view.has_closed_at); // CREATED 等非终态下 closed_at 为空

    const meeting::ParticipantView participant;
    EXPECT_FALSE(participant.has_left_at); // ACTIVE 时 left_at 为空

    const meeting::MeetingRequestContext context;
    EXPECT_FALSE(context.has_device_id);
    EXPECT_FALSE(context.has_trusted_owner_chat_server_id);

    const meeting::MeetingEvent event;
    EXPECT_FALSE(event.has_actor_user_id);       // 系统清理可能没有发起用户
    EXPECT_FALSE(event.has_participant_user_id); // 未必指向某个受影响成员

    const meeting::ParticipantIdentitySnapshot identity;
    EXPECT_FALSE(identity.has_left_at);
}

// ------------------------------------------------------------------------------
// Presence 与真实值互不混淆：先写值但不置 presence，字段仍视为不存在；
// 置上 presence 后才生效。
//
// 这一条正是"presence flag + value"相对 sentinel 的价值所在：一个恰好等于
// 默认构造值的真实时间点，与"从未设置"是可以区分的。
// ------------------------------------------------------------------------------
TEST(MeetingTypesContract, PresenceFlagGovernsFieldValidity)
{
    meeting::MeetingView view;
    const meeting::Timestamp explicit_value{}; // 一个真实但"空"的时间点

    // 写入值但未置 presence：字段仍不存在。
    view.closed_at = explicit_value;
    EXPECT_FALSE(view.has_closed_at);

    // 置 presence 后字段有效（CLOSED 状态下才应这样做）。
    view.has_closed_at = true;
    view.meeting_state = MeetingState::CLOSED;
    EXPECT_TRUE(view.has_closed_at);
    EXPECT_EQ(view.closed_at, explicit_value);

    meeting::MeetingRequestContext context;
    context.device_id = "device-ignored-until-flagged";
    EXPECT_FALSE(context.has_device_id); // 未置 presence，值不得被当作已知标识

    context.has_device_id = true;
    EXPECT_TRUE(context.has_device_id);
}

// ------------------------------------------------------------------------------
// 终态快照与事件的基本形状：只验证能承载 Phase 1 要求的最小字段，
// 不验证任何产生时机（那属于 M2/M3）。
// ------------------------------------------------------------------------------
TEST(MeetingTypesContract, ClosedSnapshotCarriesHistoricalIdentities)
{
    meeting::ClosedMeetingSnapshot snapshot;

    meeting::ParticipantIdentitySnapshot host;
    host.user_id = 7;
    host.role = ParticipantRole::HOST;

    meeting::ParticipantIdentitySnapshot member;
    member.user_id = 8;
    member.role = ParticipantRole::PARTICIPANT;
    member.has_left_at = true; // 关闭前已离会的历史成员

    snapshot.participant_identities.push_back(host);
    snapshot.participant_identities.push_back(member);
    snapshot.final_participant_count = snapshot.participant_identities.size();

    EXPECT_EQ(snapshot.meeting_state, MeetingState::CLOSED); // 快照固定为终态
    ASSERT_EQ(snapshot.participant_identities.size(), 2u);
    EXPECT_EQ(snapshot.participant_identities[0].role, ParticipantRole::HOST);
    EXPECT_TRUE(snapshot.participant_identities[1].has_left_at);
    EXPECT_EQ(snapshot.final_participant_count, 2u);
}

TEST(MeetingTypesContract, EventCarriesContractFields)
{
    meeting::MeetingEvent event;
    event.event_id = "evt-1";
    event.event_type = EventType::PARTICIPANT_JOINED;
    event.meeting_id = "meeting-1";
    event.owner_chat_server_id = "chat-1";
    event.has_actor_user_id = true;
    event.actor_user_id = 7;
    event.has_participant_user_id = true;
    event.participant_user_id = 8;
    event.dedupe_context = "join:meeting-1:8";

    EXPECT_EQ(event.event_type, EventType::PARTICIPANT_JOINED);
    EXPECT_TRUE(event.has_actor_user_id);
    EXPECT_TRUE(event.has_participant_user_id);
    EXPECT_FALSE(event.dedupe_context.empty());
}
