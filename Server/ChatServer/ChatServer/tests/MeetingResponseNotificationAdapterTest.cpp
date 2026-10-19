// ==============================================================================
// M6.2 / §16.2 组 1：MeetingRequestContext builder（CB1–CB6）。
//
// Layer C：直接调用 builder。它**不**证明存在真实 TCP 入口（wire adapter 属
// Phase 3），只证明 builder 的契约，以及该契约在领域 API 上的可见边界。
//
// 本 target（MeetingResponseNotificationAdapterTest）只链接 GTest + MeetingDomain，
// 保持进程内、无网络、无 Redis、无 MySQL、无 gRPC、无 protobuf。
// ==============================================================================

#include <gtest/gtest.h>

#include <chrono>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>

#include "MeetingRequestContextBuilder.h"
#include "meeting/MeetingService.h"

namespace {

using meeting::CreateMeetingResult;
using meeting::MeetingRequestContext;
using meeting::MeetingService;
using meeting::MeetingServiceDependencies;
using meeting::ResultCode;
using meeting::Timestamp;

const char* kLocalServer = "chatserver-local";
const char* kRemoteServer = "chatserver-remote";

// 确定性依赖：固定 meeting/event 序号与固定时间点，不读 system_clock / UUID。
MeetingServiceDependencies MakeDeterministicDependencies() {
    MeetingServiceDependencies dependencies;
    const std::shared_ptr<int> sequence = std::make_shared<int>(0);

    dependencies.next_meeting_id = [sequence]() {
        return std::string("meeting-") + std::to_string(++(*sequence));
    };
    dependencies.next_event_id = [sequence]() {
        return std::string("event-") + std::to_string(++(*sequence));
    };
    dependencies.now = []() { return Timestamp(std::chrono::seconds(1)); };
    return dependencies;
}

}  // namespace

// ------------------------------------------------------------------------------
// CB1：合法输入构造成功。
// 四个字段原样透传且非空；builder 不改写入参，也不依赖隐藏状态。
// ------------------------------------------------------------------------------
TEST(MeetingRequestContextBuilder, CB1_BuildsFromTrustedInputsWithoutRewriting) {
    const MeetingRequestContextBuilder builder;

    const meeting::SessionId session_id = "session-1";
    const meeting::RequestId request_id = "request-1";
    const meeting::ServerId local_server_id = kLocalServer;
    const meeting::UserId user_id = 1001;

    const MeetingRequestContext context =
        builder.Build(session_id, user_id, request_id, local_server_id);

    EXPECT_EQ(context.request_id, request_id);
    EXPECT_EQ(context.session_id, session_id);
    EXPECT_EQ(context.handling_chat_server_id, local_server_id);
    EXPECT_EQ(context.authenticated_user_id, user_id);

    EXPECT_FALSE(context.request_id.empty());
    EXPECT_FALSE(context.session_id.empty());
    EXPECT_FALSE(context.handling_chat_server_id.empty());

    // 入参为 const 值：调用后必须保持不变。
    EXPECT_EQ(session_id, "session-1");
    EXPECT_EQ(request_id, "request-1");
    EXPECT_EQ(local_server_id, std::string(kLocalServer));

    // 同一输入重复构建必须得到相同结果（无缓存、无全局状态）。
    const MeetingRequestContext again =
        builder.Build(session_id, user_id, request_id, local_server_id);
    EXPECT_EQ(again.request_id, context.request_id);
    EXPECT_EQ(again.session_id, context.session_id);
    EXPECT_EQ(again.authenticated_user_id, context.authenticated_user_id);
    EXPECT_EQ(again.handling_chat_server_id, context.handling_chat_server_id);
    EXPECT_EQ(again.has_device_id, context.has_device_id);
    EXPECT_EQ(again.has_trusted_owner_chat_server_id,
              context.has_trusted_owner_chat_server_id);
}

// ------------------------------------------------------------------------------
// CB2：身份只来自服务端入参。
//
// 编译期证明公开接口恰好是 §14.1 的四个入参 + const；运行期证明不同入参
// 产生各自的身份，且 builder 不做任何推断（传入 0 就保留 0）。
// ------------------------------------------------------------------------------
TEST(MeetingRequestContextBuilder, CB2_IdentityComesOnlyFromArguments) {
    using BuildSignature =
        meeting::MeetingRequestContext (MeetingRequestContextBuilder::*)(
            const meeting::SessionId&, meeting::UserId,
            const meeting::RequestId&, const meeting::ServerId&) const;

    const BuildSignature signature = &MeetingRequestContextBuilder::Build;
    (void)signature;

    const MeetingRequestContextBuilder builder;

    const MeetingRequestContext first =
        builder.Build("session-a", 1001, "request-a", kLocalServer);
    const MeetingRequestContext second =
        builder.Build("session-b", 2002, "request-b", kLocalServer);

    EXPECT_EQ(first.session_id, "session-a");
    EXPECT_EQ(first.authenticated_user_id, 1001);
    EXPECT_EQ(first.request_id, "request-a");

    EXPECT_EQ(second.session_id, "session-b");
    EXPECT_EQ(second.authenticated_user_id, 2002);
    EXPECT_EQ(second.request_id, "request-b");

    // 不推断身份：0 原样保留，由领域按既有语义处理。
    const MeetingRequestContext anonymous =
        builder.Build("session-c", 0, "request-c", kLocalServer);
    EXPECT_EQ(anonymous.authenticated_user_id, 0);
}

// ------------------------------------------------------------------------------
// CB3：handling server 必须等于服务本地节点。
// 不匹配时领域返回 INVALID_ARGUMENT；对照组（仅该字段改为本地）返回
// AUTH_REQUIRED，证明不匹配校验确实被触发，且没有被会话状态遮蔽。
// ------------------------------------------------------------------------------
TEST(MeetingRequestContextBuilder, CB3_ForeignHandlingServerIsRejectedByDomain) {
    const MeetingRequestContextBuilder builder;
    MeetingService service(kLocalServer, MakeDeterministicDependencies());

    const MeetingRequestContext foreign =
        builder.Build("session-1", 1001, "request-1", kRemoteServer);
    const CreateMeetingResult rejected = service.CreateMeeting(foreign);

    EXPECT_EQ(rejected.common.result_code, ResultCode::INVALID_ARGUMENT);
    EXPECT_EQ(rejected.common.result_name, std::string("INVALID_ARGUMENT"));

    // 对照组：只有 handling server 改成与本地一致；该 session 未注册，
    // 因此应当落到会话检查（AUTH_REQUIRED），而不是参数校验。
    const MeetingRequestContext local =
        builder.Build("session-1", 1001, "request-1", kLocalServer);
    EXPECT_EQ(service.CreateMeeting(local).common.result_code,
              ResultCode::AUTH_REQUIRED);
}

// ------------------------------------------------------------------------------
// CB4：不伪造可选身份。
// 两个 presence flag 均为 false，且对应值保持为空（不得构造"看似有效"的值）。
// ------------------------------------------------------------------------------
TEST(MeetingRequestContextBuilder, CB4_DoesNotForgeOptionalIdentities) {
    const MeetingRequestContextBuilder builder;

    const MeetingRequestContext context =
        builder.Build("session-1", 1001, "request-1", kLocalServer);

    EXPECT_FALSE(context.has_device_id);
    EXPECT_FALSE(context.has_trusted_owner_chat_server_id);
    EXPECT_TRUE(context.device_id.empty());
    EXPECT_TRUE(context.trusted_owner_chat_server_id.empty());
}

// ------------------------------------------------------------------------------
// CB5（request_id 为空）：builder 保留空值、不补偿；领域返回 INVALID_ARGUMENT。
// 对照组只把 request_id 换成非空，结果变为 AUTH_REQUIRED。
// ------------------------------------------------------------------------------
TEST(MeetingRequestContextBuilder, CB5_EmptyRequestIdIsPreservedAndRejected) {
    const MeetingRequestContextBuilder builder;
    MeetingService service(kLocalServer, MakeDeterministicDependencies());

    const MeetingRequestContext context =
        builder.Build("session-1", 1001, "", kLocalServer);
    EXPECT_TRUE(context.request_id.empty());

    EXPECT_EQ(service.CreateMeeting(context).common.result_code,
              ResultCode::INVALID_ARGUMENT);

    const MeetingRequestContext control =
        builder.Build("session-1", 1001, "request-1", kLocalServer);
    EXPECT_EQ(service.CreateMeeting(control).common.result_code,
              ResultCode::AUTH_REQUIRED);
}

// ------------------------------------------------------------------------------
// CB5（session_id 为空）：builder 保留空值、不补偿；领域返回 INVALID_ARGUMENT。
// 对照组只把 session_id 换成非空，结果变为 AUTH_REQUIRED。
// ------------------------------------------------------------------------------
TEST(MeetingRequestContextBuilder, CB5_EmptySessionIdIsPreservedAndRejected) {
    const MeetingRequestContextBuilder builder;
    MeetingService service(kLocalServer, MakeDeterministicDependencies());

    const MeetingRequestContext context =
        builder.Build("", 1001, "request-1", kLocalServer);
    EXPECT_TRUE(context.session_id.empty());

    EXPECT_EQ(service.CreateMeeting(context).common.result_code,
              ResultCode::INVALID_ARGUMENT);

    const MeetingRequestContext control =
        builder.Build("session-1", 1001, "request-1", kLocalServer);
    EXPECT_EQ(service.CreateMeeting(control).common.result_code,
              ResultCode::AUTH_REQUIRED);
}

// ------------------------------------------------------------------------------
// CB6：静态检查 —— builder 头文件不引用会话对象内部实现。
//
// 头文件路径由 CMake 以 MEETING_CONTEXT_BUILDER_HEADER 宏传入，测试直接读取
// 源码并断言不存在 CSession / _user_uid / CServer；这是 §16.2 CB6 的可执行版本。
// ------------------------------------------------------------------------------
TEST(MeetingRequestContextBuilder, CB6_BuilderHeaderDoesNotReferenceSessionInternals) {
    const std::string header_path = MEETING_CONTEXT_BUILDER_HEADER;
    std::ifstream header(header_path.c_str());
    ASSERT_TRUE(header.good()) << "cannot open builder header: " << header_path;

    const std::string content((std::istreambuf_iterator<char>(header)),
                              std::istreambuf_iterator<char>());

    EXPECT_EQ(content.find("CSession"), std::string::npos);
    EXPECT_EQ(content.find("_user_uid"), std::string::npos);
    EXPECT_EQ(content.find("CServer"), std::string::npos);

    // 防止路径错误导致"读到了别的空文件"却通过。
    EXPECT_NE(content.find("MeetingRequestContextBuilder"), std::string::npos);
}
