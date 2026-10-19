// ==============================================================================
// M6.2 / §16.2 组 1：MeetingRequestContext builder（CB1–CB6）。
// M6.3 / §16.2 组 2：session lookup（SL1–SL7）。
//
// Layer C：直接调用 builder。它**不**证明存在真实 TCP 入口（wire adapter 属
// Phase 3），只证明 builder 的契约，以及该契约在领域 API 上的可见边界。
//
// 本 target（MeetingResponseNotificationAdapterTest）只链接 GTest + MeetingDomain，
// 保持进程内、无网络、无 Redis、无 MySQL、无 gRPC、无 protobuf。
//
// M6.3 追加的验证分层（**必须如实区分**，不得把替身的通过当成真实 CServer 的通过）：
//   1. 真实生产类型（CServer / CSession）的继承关系、方法签名、const 性质、
//      是否为抽象类：由**本 TU 的编译期** static_assert 核对（包括目标文件
//      CServer.h / CSession.h 能够被真实包含）。本 target **不构造** CServer：
//      它的构造会绑定 TCP 端口，并经 CSession/LogicSystem 拉起整套 legacy
//      运行时图（Redis / MySQL / gRPC），那属于 M6.7 runtime smoke 的范围。
//   2. 真实实现的结构（锁 → find → 复制 shared_ptr → 返回）：由源码静态核对
//      （SL6 / SL7）覆盖，直接读 CServer.cpp / CSession.cpp / 接口头原文。
//   3. 所有权与判活**行为**（SL1–SL5）：由实现同一契约的轻量测试替身验证
//      （§16.3 授权的 FakeSessionDirectory / FakeLiveSession）。
//   4. 真实 _sessions 注册表上的 insert/erase/lookup 交织仍为
//      NOT VERIFIED —— 留给 M6.7 runtime smoke。
//
// M6.4 追加（§16.2 组 3/4/5 + response 映射）：
//   本 target 现在**编译真实生产实现**（../MeetingResponseNotificationAdapter.cpp
//   与 ../CountingNoWireCollector.cpp），并由真实 MeetingService /
//   MeetingRequestContextBuilder / FakeSessionDirectory 驱动：
//     · 真实 Adapter：全部七项 On* 都被真实调用，测试不复制它的算法；
//     · 真实 Collector：生产 CountingNoWireCollector 被真实驱动（BF6）；
//     · 领域状态、幂等、CLOSED 快照、历史成员授权全部由真实领域 API 产生，
//       测试不构造假的 MeetingServiceResult。
//   仍然 NOT VERIFIED（留给 M6.7 runtime smoke）：真实 CServer 的 _sessions
//   注册表、真实 CSession::Send（M6 不发送）。
//
// M6.5 追加（§16.2 组 6/7）：把真实 LogicWorkDispatcher + 真实
// SerializedWorkExecutor 编入本 target，让**生产分发代码**自己把领域结果交给
// adapter。新增用例因此覆盖：六个 Response 调用点、FINALIZE_CLOSE 调用点、
// Close 的两 turn 时序、唯一 FIFO 顺序、以及 adapter 违约时的异常隔离。
//   · SS1–SS5：SESSION_STATE_REJECTED / AUTH_REQUIRED 区分与 lookup 失败不改写结果
//   · OR1–OR3：response 顺序 == 入队顺序、FINALIZE_CLOSE 不插队、两 turn 收敛
//   · OR4：SerializedWorkExecutor::StopAndJoin 幂等（OR5 的真实关闭顺序属 M6.6/M6.7）
//   · EX1：adapter 违约抛异常不会杀死唯一消费者，也不阻止 Close 收敛
// 仍未装配 LogicSystem / ChatServer（M6.6），因此本 target 不宣称任何端到端送达。
// ==============================================================================

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

#include "MeetingRequestContextBuilder.h"
#include "meeting/MeetingService.h"

// M6.3：真实生产类型。这里只用它们做**编译期**断言，不构造、不连接、不发送。
// 这两个头引入了 boost/asio / boost/beast 的**头文件**依赖（与 ChatServer 生产
// 构建相同的系统 Boost），但不引入任何链接依赖：本 TU 不实例化 CServer /
// CSession，也不取它们的非 inline 成员函数地址（全部使用 decltype，未求值）。
#include "CServer.h"
#include "CSession.h"
#include "MeetingOutboundChannel.h"

// M6.4：真实生产实现（由 tests/CMakeLists.txt 编译进本 target）。
// 它们只依赖 C++ 标准库 + 纯领域头文件，因此不破坏本 target 的依赖隔离。
#include "CountingNoWireCollector.h"
#include "MeetingResponseNotificationAdapter.h"
// M6.5：真实分发器 + 真实串行化执行器（同一份生产代码，不是测试副本）。
#include "LogicWorkDispatcher.h"

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

// ==============================================================================
// M6.3 / §16.2 组 2：session lookup（SL1–SL7）。
//
// 生产代码在本轮的改动边界（§17.1 M6.3）：
//   CSession 实现 IMeetingLiveSession（**仅一个 const 方法** DescribeSessionId()）；
//   CServer  实现 IMeetingSessionDirectory（FindSessionBySessionId 在 _mutex 下
//   从既有 _sessions 复制 shared_ptr）。
//
// 本组用例**不**声称真实 CServer 已在运行时被驱动（见文件头第 4 条）。
// ==============================================================================

namespace {

// ------------------------------------------------------------------------------
// 替身 1：受控存活标记。
//
// 只实现 IMeetingLiveSession 的 const 方法，并带一个外置的析构计数：
// SL5 需要证明"释放最后一个强句柄后对象按其所有权规则析构"，这必须能在
// 测试里被观测到，而不是靠断言"程序没崩"。
// ------------------------------------------------------------------------------
class FakeLiveSession : public IMeetingLiveSession {
public:
    FakeLiveSession(const std::string& session_id,
                    const std::shared_ptr<int>& destroy_count)
        : session_id_(session_id), destroy_count_(destroy_count) {}

    ~FakeLiveSession() override { ++(*destroy_count_); }

    std::string DescribeSessionId() const override { return session_id_; }

private:
    std::string session_id_;
    std::shared_ptr<int> destroy_count_;
};

// ------------------------------------------------------------------------------
// 替身 2：与 CServer::FindSessionBySessionId **同一契约**的目录。
//
// 刻意只保留契约本身（互斥下 find + 复制 shared_ptr + 未命中返回空句柄），
// 不复制 CServer 的任何网络/领域行为。它因此只能证明"接口层面的所有权语义"，
// 不能证明真实 CServer 的运行时正确性。
// ------------------------------------------------------------------------------
class FakeSessionDirectory : public IMeetingSessionDirectory {
public:
    void Register(const std::shared_ptr<FakeLiveSession>& session) {
        std::lock_guard<std::mutex> lock(mutex_);
        sessions_[session->DescribeSessionId()] = session;
    }

    void Erase(const std::string& session_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        sessions_.erase(session_id);
    }

    std::size_t Size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return sessions_.size();
    }

    std::shared_ptr<const IMeetingLiveSession>
    FindSessionBySessionId(const std::string& session_id) override {
        std::lock_guard<std::mutex> lock(mutex_);
        std::map<std::string, std::shared_ptr<FakeLiveSession>>::iterator it =
            sessions_.find(session_id);
        if (it == sessions_.end()) {
            return std::shared_ptr<const IMeetingLiveSession>();
        }
        return it->second;
    }

private:
    mutable std::mutex mutex_;
    std::map<std::string, std::shared_ptr<FakeLiveSession>> sessions_;
};

// ------------------------------------------------------------------------------
// 静态核对的输入：真实生产源文件。
//
// CHATSERVER_SOURCE_DIR 由 tests/CMakeLists.txt 以绝对路径注入（与 CB6 的
// MEETING_CONTEXT_BUILDER_HEADER 同一手法），因此核对的是**仓库里的真实源码**，
// 而不是测试自己复制的实现。
// ------------------------------------------------------------------------------
const char* const kServerSourcePath = CHATSERVER_SOURCE_DIR "/CServer.cpp";
const char* const kSessionSourcePath = CHATSERVER_SOURCE_DIR "/CSession.cpp";
const char* const kChannelHeaderPath = CHATSERVER_SOURCE_DIR "/MeetingOutboundChannel.h";

std::string ReadSourceFile(const std::string& path) {
    std::ifstream file(path.c_str());
    if (!file.good()) {
        return std::string();
    }
    return std::string((std::istreambuf_iterator<char>(file)),
                       std::istreambuf_iterator<char>());
}

// 截取一个函数实现：从 marker 到其后第一个行首 '}'（本仓库源码的统一风格）。
// 这样静态核对被限制在目标函数体内，不会被同文件其它函数或前导注释误伤。
std::string FunctionWindow(const std::string& content, const std::string& marker) {
    const std::size_t start = content.find(marker);
    if (start == std::string::npos) {
        return std::string();
    }
    const std::size_t end = content.find("\n}", start);
    if (end == std::string::npos) {
        return content.substr(start);
    }
    return content.substr(start, end - start);
}

}  // namespace

// ------------------------------------------------------------------------------
// SL1：已注册的 session 查得到。
// 断言：句柄非空；DescribeSessionId() 与注册键一致；句柄指向**同一个对象**
// （不是副本），且确实增加了一条强引用。
// ------------------------------------------------------------------------------
TEST(SessionLookup, SL1_RegisteredSessionReturnsLiveHandle) {
    const std::shared_ptr<int> destroy_count = std::make_shared<int>(0);
    FakeSessionDirectory directory;

    const std::shared_ptr<FakeLiveSession> registered =
        std::make_shared<FakeLiveSession>("session-sl1", destroy_count);
    directory.Register(registered);

    // 只通过抽象接口查询：证明调用方不需要知道具体实现类型。
    IMeetingSessionDirectory& as_directory = directory;
    const std::shared_ptr<const IMeetingLiveSession> handle =
        as_directory.FindSessionBySessionId("session-sl1");

    ASSERT_TRUE(handle != nullptr);
    EXPECT_EQ(handle->DescribeSessionId(), std::string("session-sl1"));

    // 同一控制块：handle 指向的正是注册表里那个对象，不是拷贝。
    EXPECT_EQ(handle.get(),
              static_cast<const IMeetingLiveSession*>(registered.get()));

    // 强引用计数：注册表一项 + 局部 registered + handle == 3。
    EXPECT_EQ(registered.use_count(), 3);
    EXPECT_EQ(*destroy_count, 0);
}

// ------------------------------------------------------------------------------
// SL2：未知 session ⇒ 空句柄。
// 断言：不抛异常、不创建条目（注册表大小不变）、可重复查询仍为空。
// ------------------------------------------------------------------------------
TEST(SessionLookup, SL2_UnknownSessionReturnsEmptyHandleWithoutCreatingEntries) {
    FakeSessionDirectory directory;

    const std::shared_ptr<const IMeetingLiveSession> missing =
        directory.FindSessionBySessionId("session-never-registered");

    EXPECT_FALSE(missing != nullptr);
    EXPECT_EQ(directory.Size(), static_cast<std::size_t>(0));

    // 再次查询：仍为空，且依旧没有副作用。
    EXPECT_FALSE(directory.FindSessionBySessionId("session-never-registered") !=
                 nullptr);
    EXPECT_EQ(directory.Size(), static_cast<std::size_t>(0));
}

// ------------------------------------------------------------------------------
// SL3：移除之后查不到。
// insert → erase → lookup 为空；且同一 id 可以被重新注册（移除是删除，不是墓碑）。
// ------------------------------------------------------------------------------
TEST(SessionLookup, SL3_SessionIsNotFoundAfterRemoval) {
    const std::shared_ptr<int> destroy_count = std::make_shared<int>(0);
    FakeSessionDirectory directory;

    directory.Register(std::make_shared<FakeLiveSession>("session-sl3", destroy_count));
    ASSERT_TRUE(directory.FindSessionBySessionId("session-sl3") != nullptr);

    directory.Erase("session-sl3");

    const std::shared_ptr<const IMeetingLiveSession> after_erase =
        directory.FindSessionBySessionId("session-sl3");
    EXPECT_FALSE(after_erase != nullptr);
    EXPECT_EQ(directory.Size(), static_cast<std::size_t>(0));
}

// ------------------------------------------------------------------------------
// SL4：强句柄在移除之后仍然可用（这是 M6 整个设计的核心目的，§8.1 / §8.3）。
// 先取句柄 → 从目录移除 → 句柄仍可解引用且对象未被析构。
// ------------------------------------------------------------------------------
TEST(SessionLookup, SL4_StrongHandleSurvivesDirectoryRemoval) {
    const std::shared_ptr<int> destroy_count = std::make_shared<int>(0);
    FakeSessionDirectory directory;

    const std::shared_ptr<FakeLiveSession> registered =
        std::make_shared<FakeLiveSession>("session-sl4", destroy_count);
    directory.Register(registered);

    const std::shared_ptr<const IMeetingLiveSession> handle =
        directory.FindSessionBySessionId("session-sl4");
    ASSERT_TRUE(handle != nullptr);

    directory.Erase("session-sl4");

    // 目录侧已查不到，但句柄侧仍持有对象，因此析构尚未发生。
    EXPECT_FALSE(directory.FindSessionBySessionId("session-sl4") != nullptr);
    EXPECT_EQ(*destroy_count, 0);

    // 可重复解引用：不是"读一次就悬垂"。
    EXPECT_EQ(handle->DescribeSessionId(), std::string("session-sl4"));
    EXPECT_EQ(handle->DescribeSessionId(), std::string("session-sl4"));
    EXPECT_EQ(*destroy_count, 0);

    // 释放句柄后对象才析构：证明保活来自句柄本身，而非目录。
    EXPECT_EQ(*destroy_count, 0);
}

// ------------------------------------------------------------------------------
// SL5：释放最后一个强句柄后允许对象按其所有权规则析构。
// 断言析构**恰好一次**，且既不在目录移除时、也不在中间某次 reset 时发生。
// ------------------------------------------------------------------------------
TEST(SessionLookup, SL5_LastStrongHandleReleaseAllowsDestruction) {
    const std::shared_ptr<int> destroy_count = std::make_shared<int>(0);

    {
        FakeSessionDirectory directory;
        std::shared_ptr<FakeLiveSession> registered =
            std::make_shared<FakeLiveSession>("session-sl5", destroy_count);
        directory.Register(registered);

        std::shared_ptr<const IMeetingLiveSession> handle =
            directory.FindSessionBySessionId("session-sl5");
        ASSERT_TRUE(handle != nullptr);
        EXPECT_EQ(*destroy_count, 0);

        // 移除注册表项：仍有两条强引用（registered + handle）⇒ 不析构。
        directory.Erase("session-sl5");
        EXPECT_EQ(*destroy_count, 0);

        handle.reset();
        EXPECT_EQ(*destroy_count, 0);

        // 最后一条强引用释放 ⇒ 析构恰好一次。
        registered.reset();
        EXPECT_EQ(*destroy_count, 1);
    }

    // 目录析构不会重复析构已释放的对象。
    EXPECT_EQ(*destroy_count, 1);
}

// ------------------------------------------------------------------------------
// SL6：不存在通过裸指针创建第二个控制块（§8.6）。
//
// 静态核对真实 CServer.cpp 的实现窗口。任何一条命中都意味着以下风险之一：
//   · .get() / release() / make_shared / new CSession ⇒ 第二个控制块 ⇒ double free；
//   · erase / insert ⇒ 把"查询"变成"修改注册表"，破坏 single-writer 与 CLEAR 语义；
//   · 缺少 lock_guard，或 find 早于 lock_guard ⇒ 在注册表上无保护的并发读。
// ------------------------------------------------------------------------------
TEST(SessionLookup, SL6_NoSecondControlBlockInServerLookup) {
    const std::string server_source = ReadSourceFile(kServerSourcePath);
    ASSERT_FALSE(server_source.empty()) << "cannot read " << kServerSourcePath;

    const std::string window =
        FunctionWindow(server_source, "CServer::FindSessionBySessionId");
    ASSERT_FALSE(window.empty())
        << "CServer::FindSessionBySessionId not found in " << kServerSourcePath;

    // 唯一来源必须是注册表里既有的 shared_ptr：复制并隐式向上转换。
    EXPECT_NE(window.find("return it->second;"), std::string::npos);

    // 未命中必须返回空句柄，而不是抛出、也不是返回哨兵对象。
    EXPECT_NE(window.find("it == _sessions.end()"), std::string::npos);
    EXPECT_NE(window.find("return std::shared_ptr<const IMeetingLiveSession>();"),
              std::string::npos);

    // 禁止第二控制块。
    EXPECT_EQ(window.find(".get()"), std::string::npos);
    EXPECT_EQ(window.find("release()"), std::string::npos);
    EXPECT_EQ(window.find("make_shared"), std::string::npos);
    EXPECT_EQ(window.find("new CSession"), std::string::npos);
    EXPECT_EQ(window.find("shared_ptr<CSession>("), std::string::npos);

    // 禁止把查询变成写入。
    EXPECT_EQ(window.find("erase"), std::string::npos);
    EXPECT_EQ(window.find("insert"), std::string::npos);

    // 锁必须在 find 之前，且在同一函数体内。
    const std::size_t lock_pos = window.find("lock_guard<std::mutex> lock(_mutex)");
    const std::size_t find_pos = window.find("_sessions.find(session_id)");
    ASSERT_NE(lock_pos, std::string::npos) << "lookup is not protected by _mutex";
    ASSERT_NE(find_pos, std::string::npos) << "lookup does not use _sessions";
    EXPECT_LT(lock_pos, find_pos);
}

// ------------------------------------------------------------------------------
// SL7：const 契约自洽（编译期 + 静态核对）。
//
// 编译期（本 TU 必须通过编译）：
//   · 真实 CServer 继承 IMeetingSessionDirectory，且实现该纯虚函数（非抽象类）；
//   · 真实 CSession 继承 IMeetingLiveSession，且 DescribeSessionId() 是 **const**；
//   · FindSessionBySessionId 的返回类型恰为 shared_ptr<const IMeetingLiveSession>；
//   · 解引用 const 句柄得到的是 **const** 对象 ⇒ 只能调用 const 成员。
// 静态核对：
//   · 接口头不存在任何发送语义的成员（没有 Send / send / Write / asio / boost）；
//   · 真实 CSession.cpp 的 DescribeSessionId 只返回 _session_id，不读 _user_uid、
//     不发送、不加锁、不修改任何成员。
// ------------------------------------------------------------------------------
TEST(SessionLookup, SL7_ConstContractIsSelfConsistent) {
    // ---- 真实类型的编译期断言 ----------------------------------------------
    static_assert(std::is_base_of<IMeetingSessionDirectory, CServer>::value,
                  "CServer must implement IMeetingSessionDirectory");
    static_assert(std::is_base_of<IMeetingLiveSession, CSession>::value,
                  "CSession must implement IMeetingLiveSession");
    static_assert(!std::is_abstract<CServer>::value,
                  "CServer must override every pure virtual of IMeetingSessionDirectory");
    static_assert(!std::is_abstract<CSession>::value,
                  "CSession must override every pure virtual of IMeetingLiveSession");

    // 既有继承不得被替换掉（两条继承必须同时存在）。
    static_assert(std::is_base_of<std::enable_shared_from_this<CServer>, CServer>::value,
                  "CServer must keep enable_shared_from_this");
    static_assert(std::is_base_of<std::enable_shared_from_this<CSession>, CSession>::value,
                  "CSession must keep enable_shared_from_this");

    // 返回类型必须**恰好**是 shared_ptr<const IMeetingLiveSession>。
    static_assert(
        std::is_same<decltype(&CServer::FindSessionBySessionId),
                     std::shared_ptr<const IMeetingLiveSession> (CServer::*)(
                         const std::string&)>::value,
        "FindSessionBySessionId must return shared_ptr<const IMeetingLiveSession>");

    // DescribeSessionId 必须**恰好**是 const 成员函数（签名包含 const）。
    static_assert(
        std::is_same<decltype(&CSession::DescribeSessionId),
                     std::string (CSession::*)() const>::value,
        "DescribeSessionId must be a const member function");

    // override 关系由上面两条 !is_abstract 断言覆盖：若 const、参数或返回类型
    // 与基类纯虚函数有任何一处不同，该纯虚函数就未被覆盖，类型即为抽象类，
    // static_assert 会直接失败。（C++ 只允许"基类成员指针 → 派生类成员指针"
    // 的隐式转换，反方向不可用，所以这里不用 is_convertible 表达 override。）

    // const 句柄的解引用结果是 const 对象 ⇒ 编译期就无法调用非 const 成员。
    static_assert(
        std::is_const<typename std::remove_reference<
            decltype(*std::declval<std::shared_ptr<const IMeetingLiveSession>>())>::
                type>::value,
        "dereferencing the handle must yield a const object");

    // 接口上是 const 调用：const 对象可以调用 DescribeSessionId()。
    static_assert(
        std::is_same<decltype(std::declval<const IMeetingLiveSession&>()
                                  .DescribeSessionId()),
                     std::string>::value,
        "DescribeSessionId must be callable on a const interface reference");

    // ---- 接口头：没有发送语义 ----------------------------------------------
    const std::string channel_header = ReadSourceFile(kChannelHeaderPath);
    ASSERT_FALSE(channel_header.empty()) << "cannot read " << kChannelHeaderPath;
    EXPECT_EQ(channel_header.find("Send"), std::string::npos);
    EXPECT_EQ(channel_header.find("send"), std::string::npos);
    EXPECT_EQ(channel_header.find("Write"), std::string::npos);
    EXPECT_EQ(channel_header.find("asio"), std::string::npos);
    EXPECT_EQ(channel_header.find("boost"), std::string::npos);

    // ---- 真实 CSession.cpp：只读实现 ---------------------------------------
    const std::string session_source = ReadSourceFile(kSessionSourcePath);
    ASSERT_FALSE(session_source.empty()) << "cannot read " << kSessionSourcePath;

    // 实现的声明行必须带 const。
    EXPECT_NE(session_source.find("std::string CSession::DescribeSessionId() const {"),
              std::string::npos);

    const std::string window =
        FunctionWindow(session_source, "CSession::DescribeSessionId");
    ASSERT_FALSE(window.empty())
        << "CSession::DescribeSessionId not found in " << kSessionSourcePath;

    EXPECT_NE(window.find("return _session_id;"), std::string::npos);
    // 不读 _user_uid（F7：该成员构造未初始化，本轮不碰）。
    EXPECT_EQ(window.find("_user_uid"), std::string::npos);
    // 不发送、不关闭、不加锁：const 且无副作用。
    EXPECT_EQ(window.find("Send"), std::string::npos);
    EXPECT_EQ(window.find("Close"), std::string::npos);
    EXPECT_EQ(window.find("lock_guard"), std::string::npos);
    EXPECT_EQ(window.find("_socket"), std::string::npos);
    EXPECT_EQ(window.find("_send_que"), std::string::npos);

    // ---- 运行期：通过 const 句柄读取 id ------------------------------------
    const std::shared_ptr<int> destroy_count = std::make_shared<int>(0);
    FakeSessionDirectory directory;
    directory.Register(std::make_shared<FakeLiveSession>("session-sl7", destroy_count));

    std::shared_ptr<const IMeetingLiveSession> handle =
        directory.FindSessionBySessionId("session-sl7");
    ASSERT_TRUE(handle != nullptr);
    EXPECT_EQ(handle->DescribeSessionId(), std::string("session-sl7"));
}

// ==============================================================================
// M6.4 / §16.2 组 3、组 4、组 5 + response 映射。
//
// 全部用例都**真实驱动**生产实现：
//   MeetingResponseNotificationAdapter（真实 Adapter）
//   CountingNoWireCollector           （真实生产 Collector，BF6）
//   MeetingService                    （真实领域，产生真实的幂等 / CLOSED / 快照）
//   MeetingRequestContextBuilder      （真实可信 context）
//   FakeSessionDirectory / FakeLiveSession（§16.3 授权的判活替身）
//
// 只有"目标是否存活"和"Collect 是否真的被调用"由测试替身提供；其余行为都是
// 生产代码自己的行为，测试里没有复制 Adapter 的算法。
// ==============================================================================

namespace {

// ------------------------------------------------------------------------------
// 路径：真实生产源文件（静态核对用，与 M6.3 的 SL6/SL7 同一手法）。
// ------------------------------------------------------------------------------
const char* const kAdapterHeaderPath =
    CHATSERVER_SOURCE_DIR "/MeetingResponseNotificationAdapter.h";
const char* const kAdapterSourcePath =
    CHATSERVER_SOURCE_DIR "/MeetingResponseNotificationAdapter.cpp";
const char* const kCollectorHeaderPath =
    CHATSERVER_SOURCE_DIR "/CountingNoWireCollector.h";
const char* const kCollectorSourcePath =
    CHATSERVER_SOURCE_DIR "/CountingNoWireCollector.cpp";
// M6.5：SS1 需要核对"CLOSING/CLOSED 同分支"这一领域不变量（只读该文件）。
const char* const kMeetingServiceSourcePath =
    CHATSERVER_SOURCE_DIR "/meeting/MeetingService.cpp";
// M6.5：调用点顺序的静态核对目标（只读该文件）。
const char* const kDispatcherSourcePath =
    CHATSERVER_SOURCE_DIR "/LogicWorkDispatcher.cpp";

// ------------------------------------------------------------------------------
// 去掉注释后的源码（**代码级**静态核对用）。
//
// 为什么必须先 StripComments：
//   生产文件里的说明性注释会写"不调用 Send / 不 include CSession.h"这类**否定**
//   语句；若直接对原文 grep，会把"声明不做某事"误判成"做了某事"。
//   去注释后仍有命中，才是真实调用。
// ------------------------------------------------------------------------------
std::string StripComments(const std::string& source) {
    std::string out;
    out.reserve(source.size());

    bool in_line_comment = false;
    bool in_block_comment = false;
    bool in_string = false;
    bool in_char = false;

    for (std::size_t i = 0; i < source.size(); ++i) {
        const char c = source[i];
        const char next = (i + 1 < source.size()) ? source[i + 1] : '\0';

        if (in_line_comment) {
            if (c == '\n') {
                in_line_comment = false;
                out.push_back(c);
            }
            continue;
        }
        if (in_block_comment) {
            if (c == '*' && next == '/') {
                in_block_comment = false;
                ++i;
            } else if (c == '\n') {
                out.push_back(c);
            }
            continue;
        }
        if (in_string || in_char) {
            out.push_back(c);
            if (c == '\\' && next != '\0') {
                out.push_back(next);
                ++i;
                continue;
            }
            if (in_string && c == '"') {
                in_string = false;
            } else if (in_char && c == '\'') {
                in_char = false;
            }
            continue;
        }
        if (c == '/' && next == '/') {
            in_line_comment = true;
            ++i;
            continue;
        }
        if (c == '/' && next == '*') {
            in_block_comment = true;
            ++i;
            continue;
        }
        if (c == '"') {
            in_string = true;
            out.push_back(c);
            continue;
        }
        if (c == '\'') {
            in_char = true;
            out.push_back(c);
            continue;
        }
        out.push_back(c);
    }
    return out;
}

std::size_t CountOccurrences(const std::string& text, const std::string& needle) {
    if (needle.empty()) {
        return 0;
    }
    std::size_t count = 0;
    std::size_t position = 0;
    while ((position = text.find(needle, position)) != std::string::npos) {
        ++count;
        position += needle.size();
    }
    return count;
}

std::string TargetOf(const MeetingOutboundMessage& message) {
    if (message.kind == MeetingOutboundKind::NOTIFICATION) {
        return message.notification.notification_target_session_id;
    }
    return message.response.response_target_session_id;
}

// ------------------------------------------------------------------------------
// 记录型 Collector（§16.3 授权的测试替身）。
//
// 内部加锁：即使将来被 consumer 线程写入、测试线程读取，也不会引入 §16.4 R4
// 指出的无同步跨线程读取。同时支持"注入前 N 次抛异常"（BF2/BF4）。
// ------------------------------------------------------------------------------
class RecordingMessageCollector : public IMeetingOutboundMessageCollector {
public:
    // 显式开关：只让**被测的那一次调用**违约，避免把生命周期里的其它 Collect
    // 也算成异常（否则断言就不再指向被测路径）。
    void SetThrowEnabled(bool enabled) {
        std::lock_guard<std::mutex> lock(mutex_);
        throw_enabled_ = enabled;
    }

    void Collect(const MeetingOutboundMessage& message) override {
        bool should_throw = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++attempts_;
            attempted_targets_.push_back(TargetOf(message));
            attempted_kinds_.push_back(message.kind);
            should_throw = throw_enabled_;
            if (!should_throw) {
                messages_.push_back(message);
            }
        }
        // 故意违反"不抛"契约：验证 adapter 的异常隔离（BF2）。
        if (should_throw) {
            throw std::runtime_error("injected collector failure");
        }
    }

    std::size_t Size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return messages_.size();
    }

    std::uint64_t Attempts() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return attempts_;
    }

    std::uint64_t AttemptsOf(MeetingOutboundKind kind) const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::uint64_t count = 0;
        for (std::size_t i = 0; i < attempted_kinds_.size(); ++i) {
            if (attempted_kinds_[i] == kind) {
                ++count;
            }
        }
        return count;
    }

    std::vector<std::string> AttemptedTargetsOf(MeetingOutboundKind kind) const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::string> targets;
        for (std::size_t i = 0; i < attempted_kinds_.size(); ++i) {
            if (attempted_kinds_[i] == kind) {
                targets.push_back(attempted_targets_[i]);
            }
        }
        return targets;
    }

    std::vector<MeetingOutboundMessage> OfKind(MeetingOutboundKind kind) const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<MeetingOutboundMessage> filtered;
        for (std::size_t i = 0; i < messages_.size(); ++i) {
            if (messages_[i].kind == kind) {
                filtered.push_back(messages_[i]);
            }
        }
        return filtered;
    }

private:
    mutable std::mutex mutex_;
    bool throw_enabled_ = false;
    std::uint64_t attempts_ = 0;
    std::vector<std::string> attempted_targets_;
    std::vector<MeetingOutboundKind> attempted_kinds_;
    std::vector<MeetingOutboundMessage> messages_;
};

// ------------------------------------------------------------------------------
// 计数差值：断言只依赖**本次调用**产生的变化，不依赖历史累计值。
// ------------------------------------------------------------------------------
MeetingAdapterCounters Delta(const MeetingAdapterCounters& before,
                             const MeetingAdapterCounters& after) {
    MeetingAdapterCounters delta;
    delta.response_produced = after.response_produced - before.response_produced;
    delta.notification_produced =
        after.notification_produced - before.notification_produced;
    delta.outbound_produced = after.outbound_produced - before.outbound_produced;
    delta.target_resolved = after.target_resolved - before.target_resolved;
    delta.target_missing = after.target_missing - before.target_missing;
    delta.directory_unavailable =
        after.directory_unavailable - before.directory_unavailable;
    delta.encoding_failed = after.encoding_failed - before.encoding_failed;
    delta.adapter_exception = after.adapter_exception - before.adapter_exception;
    return delta;
}

// ------------------------------------------------------------------------------
// 场景数据：全部 ID 都由真实领域流程产生（不硬编码 meeting/event 序号）。
// ------------------------------------------------------------------------------
struct OpenMeetingScenario {
    std::string host_session;
    std::string member_a_session;
    std::string member_b_session;
    int host_user = 0;
    int user_a = 0;
    int user_b = 0;
    std::string meeting_id;
    meeting::Timestamp created_at;
};

struct ClosedMeetingScenario {
    OpenMeetingScenario open;
    meeting::CloseMeetingResult close_result;
    meeting::FinalizeCloseResult finalize_result;
    int finalize_calls = 0;

    const std::string& MeetingId() const { return open.meeting_id; }
};

// ------------------------------------------------------------------------------
// 夹具：真实领域 + 判活替身 + 真实 Adapter。
//
// 刻意**不**使用真实 CServer / CSession / LogicSystem / dispatcher：那些会把
// asio / Boost / Redis / MySQL / gRPC 拉进本 target（§16.5）。
// ------------------------------------------------------------------------------
class AdapterTestFixture {
public:
    AdapterTestFixture()
        : service_(std::string(kLocalServer), MakeDeterministicDependencies()),
          directory_(std::make_shared<FakeSessionDirectory>()),
          destroy_count_(std::make_shared<int>(0)) {}

    meeting::MeetingService& Service() { return service_; }
    std::shared_ptr<FakeSessionDirectory> Directory() const { return directory_; }

    // 同时建立"领域会话记录"与"判活目录项"：前者决定领域判定（权限/状态），
    // 后者决定 adapter 能否解析目标（§8.2）。
    void AddSessionEverywhere(const std::string& session_id, int user_id) {
        EXPECT_TRUE(service_.RegisterConnectedSession(session_id));
        EXPECT_TRUE(service_.AuthenticateSession(session_id, user_id));
        directory_->Register(std::make_shared<FakeLiveSession>(session_id, destroy_count_));
    }

    // 只从判活目录移除：模拟"会话已断开、注册表项已 erase"（§8.4 / §16.4 R6）。
    void RemoveFromDirectory(const std::string& session_id) {
        directory_->Erase(session_id);
    }

    // 只把判活目录项加回来（领域 Session 记录已经是 AUTHENTICATED，不能重复注册）。
    void RestoreToDirectory(const std::string& session_id) {
        directory_->Register(std::make_shared<FakeLiveSession>(session_id, destroy_count_));
    }

    meeting::MeetingRequestContext Context(const std::string& request_id,
                                          const std::string& session_id,
                                          int user_id) const {
        return builder_.Build(session_id, user_id, request_id, std::string(kLocalServer));
    }

    std::unique_ptr<MeetingResponseNotificationAdapter> MakeAdapter(
        IMeetingOutboundMessageCollector& collector) const {
        return std::unique_ptr<MeetingResponseNotificationAdapter>(
            new MeetingResponseNotificationAdapter(collector, directory_));
    }

    // §8.4：目录已析构（weak 失效）——最坏结果是 no-op，不是崩溃。
    std::unique_ptr<MeetingResponseNotificationAdapter> MakeAdapterWithExpiredDirectory(
        IMeetingOutboundMessageCollector& collector) const {
        std::weak_ptr<IMeetingSessionDirectory> expired;
        {
            const std::shared_ptr<FakeSessionDirectory> temporary =
                std::make_shared<FakeSessionDirectory>();
            expired = temporary;
        }
        EXPECT_TRUE(expired.expired());
        return std::unique_ptr<MeetingResponseNotificationAdapter>(
            new MeetingResponseNotificationAdapter(collector, expired));
    }

    // 真实领域流程：host 创建 → 两名成员加入。每个命令都经真实 Adapter 产出 response。
    OpenMeetingScenario OpenMeeting(MeetingResponseNotificationAdapter& adapter) {
        OpenMeetingScenario scenario;
        scenario.host_session = "s-host";
        scenario.member_a_session = "s-a";
        scenario.member_b_session = "s-b";
        scenario.host_user = 10;
        scenario.user_a = 100;
        scenario.user_b = 200;

        AddSessionEverywhere(scenario.host_session, scenario.host_user);
        AddSessionEverywhere(scenario.member_a_session, scenario.user_a);
        AddSessionEverywhere(scenario.member_b_session, scenario.user_b);

        const meeting::MeetingRequestContext create_context =
            Context("req-create", scenario.host_session, scenario.host_user);
        const meeting::CreateMeetingResult created =
            service_.CreateMeeting(create_context);
        adapter.OnCreateMeeting(create_context, created);
        EXPECT_EQ(created.common.result_code, meeting::ResultCode::OK);
        scenario.meeting_id = created.meeting.meeting_id;
        scenario.created_at = created.meeting.created_at;

        const meeting::MeetingRequestContext join_a_context =
            Context("req-join-a", scenario.member_a_session, scenario.user_a);
        const meeting::JoinMeetingResult joined_a =
            service_.JoinMeeting(join_a_context, scenario.meeting_id);
        adapter.OnJoinMeeting(join_a_context, joined_a);
        EXPECT_EQ(joined_a.common.result_code, meeting::ResultCode::OK);

        const meeting::MeetingRequestContext join_b_context =
            Context("req-join-b", scenario.member_b_session, scenario.user_b);
        const meeting::JoinMeetingResult joined_b =
            service_.JoinMeeting(join_b_context, scenario.meeting_id);
        adapter.OnJoinMeeting(join_b_context, joined_b);
        EXPECT_EQ(joined_b.common.result_code, meeting::ResultCode::OK);

        return scenario;
    }

    // 真实领域流程：OpenMeeting → host BeginClose（turn 1）→ FinalizeClose（turn 2）。
    // FinalizeClose **不由本函数**调用 adapter：每个用例自己决定何时调用
    // OnFinalizeClose，以便对计数做差值断言。
    ClosedMeetingScenario CloseMeeting(MeetingResponseNotificationAdapter& adapter) {
        ClosedMeetingScenario scenario;
        scenario.open = OpenMeeting(adapter);

        const meeting::MeetingRequestContext close_context =
            Context("req-close", scenario.open.host_session, scenario.open.host_user);
        scenario.close_result = service_.CloseMeeting(close_context, scenario.open.meeting_id);
        adapter.OnCloseMeeting(close_context, scenario.close_result);
        EXPECT_EQ(scenario.close_result.common.result_code, meeting::ResultCode::OK);

        scenario.finalize_result = service_.FinalizeClose(scenario.open.meeting_id);
        ++scenario.finalize_calls;
        EXPECT_TRUE(scenario.finalize_result.finalized);
        return scenario;
    }

private:
    meeting::MeetingService service_;
    std::shared_ptr<FakeSessionDirectory> directory_;
    std::shared_ptr<int> destroy_count_;
    MeetingRequestContextBuilder builder_;
};

std::vector<std::string> Sorted(std::vector<std::string> values) {
    std::sort(values.begin(), values.end());
    return values;
}

}  // namespace

// ==============================================================================
// 组 3：notification targets（ND1–ND7）
//
// 全部由真实 FinalizeCloseResult 驱动：targets 来自领域冻结的 CloseContext，
// 不是测试自己拼的列表（§7.1.2 步骤 2 的输入）。
// ==============================================================================

// ------------------------------------------------------------------------------
// ND1：N 个目标全部存在 ⇒ N 条**单目标**消息，Collect 恰好 N 次。
// ------------------------------------------------------------------------------
TEST(MeetingNotificationPath, ND1_AllTargetsPresentProducesOneNotificationPerTarget) {
    AdapterTestFixture fixture;
    RecordingMessageCollector collector;
    const std::unique_ptr<MeetingResponseNotificationAdapter> adapter =
        fixture.MakeAdapter(collector);

    const ClosedMeetingScenario scenario = fixture.CloseMeeting(*adapter);

    // 领域侧的真实输入：3 个 BOUND session（host + 两名成员）。
    const std::vector<std::string> targets =
        scenario.finalize_result.notification_session_ids;
    ASSERT_EQ(targets.size(), static_cast<std::size_t>(3));

    const std::size_t before_size = collector.Size();
    const MeetingAdapterCounters before = adapter->Snapshot();

    adapter->OnFinalizeClose(scenario.finalize_result);

    const MeetingAdapterCounters delta = Delta(before, adapter->Snapshot());
    const std::vector<MeetingOutboundMessage> notifications =
        collector.OfKind(MeetingOutboundKind::NOTIFICATION);

    // N 条通知，且 Collect 恰好被多调用 N 次（不多不少）。
    ASSERT_EQ(notifications.size(), targets.size());
    EXPECT_EQ(collector.Size() - before_size, targets.size());

    // 每条都是"单目标 + MEETING_CLOSED + 真实 meeting_id"，且目标互不相同、
    // 与输入集合逐一对应（不复用一条消息代表多个目标）。
    std::vector<std::string> produced_targets;
    for (std::size_t i = 0; i < notifications.size(); ++i) {
        EXPECT_EQ(notifications[i].notification.notification_type,
                  meeting::EventType::MEETING_CLOSED);
        EXPECT_EQ(notifications[i].notification.meeting_id, scenario.MeetingId());
        EXPECT_FALSE(notifications[i].notification.notification_target_session_id.empty());
        produced_targets.push_back(
            notifications[i].notification.notification_target_session_id);

        // event_id 来自真实 MEETING_CLOSED 事件，correlation 来自真实
        // CloseContext.originating_request_id（经该事件透出）。
        EXPECT_TRUE(notifications[i].notification.has_event_id);
        EXPECT_FALSE(notifications[i].notification.event_id.empty());
        EXPECT_TRUE(notifications[i].notification.has_correlation_request_id);
        EXPECT_EQ(notifications[i].notification.correlation_request_id,
                  std::string("req-close"));
    }
    EXPECT_EQ(Sorted(produced_targets), Sorted(targets));

    // 计数守恒（§7.1.3）。
    EXPECT_EQ(delta.target_resolved, static_cast<std::uint64_t>(3));
    EXPECT_EQ(delta.target_missing, static_cast<std::uint64_t>(0));
    EXPECT_EQ(delta.directory_unavailable, static_cast<std::uint64_t>(0));
    EXPECT_EQ(delta.notification_produced, static_cast<std::uint64_t>(3));
    EXPECT_EQ(delta.outbound_produced, static_cast<std::uint64_t>(3));
    EXPECT_EQ(delta.adapter_exception, static_cast<std::uint64_t>(0));
}

// ------------------------------------------------------------------------------
// ND2：目标全部消失 ⇒ 0 条通知（0 次 Collect），只计 target_missing。
// ------------------------------------------------------------------------------
TEST(MeetingNotificationPath, ND2_AllTargetsMissingProducesNoNotification) {
    AdapterTestFixture fixture;
    RecordingMessageCollector collector;
    const std::unique_ptr<MeetingResponseNotificationAdapter> adapter =
        fixture.MakeAdapter(collector);

    const ClosedMeetingScenario scenario = fixture.CloseMeeting(*adapter);

    // §8.4 / §16.4 R6：目标消失的唯一模拟方式是"目录 lookup 返回空"，
    // 即真实语义里的 ClearSession ⇒ erase。
    for (std::size_t i = 0;
         i < scenario.finalize_result.notification_session_ids.size(); ++i) {
        fixture.RemoveFromDirectory(
            scenario.finalize_result.notification_session_ids[i]);
    }

    const std::size_t before_size = collector.Size();
    const MeetingAdapterCounters before = adapter->Snapshot();

    adapter->OnFinalizeClose(scenario.finalize_result);

    const MeetingAdapterCounters delta = Delta(before, adapter->Snapshot());
    EXPECT_EQ(collector.Size(), before_size);   // 0 次 Collect
    EXPECT_TRUE(collector.OfKind(MeetingOutboundKind::NOTIFICATION).empty());
    EXPECT_EQ(delta.target_resolved, static_cast<std::uint64_t>(0));
    EXPECT_EQ(delta.target_missing, static_cast<std::uint64_t>(3));
    EXPECT_EQ(delta.notification_produced, static_cast<std::uint64_t>(0));
    EXPECT_EQ(delta.directory_unavailable, static_cast<std::uint64_t>(0));
}

// ------------------------------------------------------------------------------
// ND3：部分消失 ⇒ 只对存活目标各产出一条，其它静默跳过。
// ------------------------------------------------------------------------------
TEST(MeetingNotificationPath, ND3_PartialMissingProducesOnlyForLiveTargets) {
    AdapterTestFixture fixture;
    RecordingMessageCollector collector;
    const std::unique_ptr<MeetingResponseNotificationAdapter> adapter =
        fixture.MakeAdapter(collector);

    const ClosedMeetingScenario scenario = fixture.CloseMeeting(*adapter);
    const std::vector<std::string> targets =
        scenario.finalize_result.notification_session_ids;
    ASSERT_EQ(targets.size(), static_cast<std::size_t>(3));

    // 只让"中间那个"目标消失，其它两个存活（含边界：非首非尾）。
    const std::string vanished = targets[1];
    fixture.RemoveFromDirectory(vanished);

    const MeetingAdapterCounters before = adapter->Snapshot();
    adapter->OnFinalizeClose(scenario.finalize_result);
    const MeetingAdapterCounters delta = Delta(before, adapter->Snapshot());

    const std::vector<MeetingOutboundMessage> notifications =
        collector.OfKind(MeetingOutboundKind::NOTIFICATION);
    ASSERT_EQ(notifications.size(), static_cast<std::size_t>(2));

    std::vector<std::string> produced_targets;
    for (std::size_t i = 0; i < notifications.size(); ++i) {
        const std::string target =
            notifications[i].notification.notification_target_session_id;
        produced_targets.push_back(target);
        EXPECT_NE(target, vanished);   // 消失的目标不得被产出
    }
    EXPECT_EQ(std::find(produced_targets.begin(), produced_targets.end(), vanished),
              produced_targets.end());

    // 守恒：resolved + missing == N；produced == resolved。
    EXPECT_EQ(delta.target_resolved, static_cast<std::uint64_t>(2));
    EXPECT_EQ(delta.target_missing, static_cast<std::uint64_t>(1));
    EXPECT_EQ(delta.target_resolved + delta.target_missing,
              static_cast<std::uint64_t>(targets.size()));
    EXPECT_EQ(delta.notification_produced, delta.target_resolved);
}

// ------------------------------------------------------------------------------
// ND4（X11 / §9.3）：通知失败不回滚 CLOSED，快照保留，MeetingClosed 恰好一次。
// ------------------------------------------------------------------------------
TEST(MeetingNotificationPath, ND4_NotificationFailureDoesNotRollBackClosedState) {
    AdapterTestFixture fixture;
    RecordingMessageCollector collector;
    const std::unique_ptr<MeetingResponseNotificationAdapter> adapter =
        fixture.MakeAdapter(collector);

    const ClosedMeetingScenario scenario = fixture.CloseMeeting(*adapter);

    // 让所有目标消失，使通知全部失败。
    for (std::size_t i = 0;
         i < scenario.finalize_result.notification_session_ids.size(); ++i) {
        fixture.RemoveFromDirectory(
            scenario.finalize_result.notification_session_ids[i]);
    }
    adapter->OnFinalizeClose(scenario.finalize_result);

    // 领域状态：CLOSED 已收敛、快照在、active 聚合已释放。
    EXPECT_TRUE(scenario.finalize_result.finalized);
    EXPECT_TRUE(scenario.finalize_result.has_snapshot);
    EXPECT_TRUE(fixture.Service().HasClosedSnapshot(scenario.MeetingId()));
    EXPECT_FALSE(fixture.Service().HasActiveMeeting(scenario.MeetingId()));

    // MeetingClosed 恰好一次。
    std::size_t closed_events = 0;
    for (std::size_t i = 0; i < scenario.finalize_result.events.size(); ++i) {
        if (scenario.finalize_result.events[i].event_type ==
            meeting::EventType::MEETING_CLOSED) {
            ++closed_events;
        }
    }
    EXPECT_EQ(closed_events, static_cast<std::size_t>(1));

    // 关闭之后查询仍然可用：授权视图来自保留的 snapshot。
    const meeting::MeetingRequestContext host_context =
        fixture.Context("req-query-after", scenario.open.host_session,
                        scenario.open.host_user);
    const meeting::QueryMeetingResult after =
        fixture.Service().QueryMeeting(host_context, scenario.MeetingId());
    EXPECT_EQ(after.common.result_code, meeting::ResultCode::OK);
    EXPECT_EQ(after.meeting.meeting_state, meeting::MeetingState::CLOSED);
    EXPECT_TRUE(after.meeting.has_closed_at);
}

// ------------------------------------------------------------------------------
// ND5：FinalizeClose 仍只发生一次 —— 通知失败不会诱发第二次收敛。
// ------------------------------------------------------------------------------
TEST(MeetingNotificationPath, ND5_NotificationFailureDoesNotTriggerSecondFinalizeClose) {
    AdapterTestFixture fixture;
    RecordingMessageCollector collector;
    const std::unique_ptr<MeetingResponseNotificationAdapter> adapter =
        fixture.MakeAdapter(collector);

    const ClosedMeetingScenario scenario = fixture.CloseMeeting(*adapter);
    for (std::size_t i = 0;
         i < scenario.finalize_result.notification_session_ids.size(); ++i) {
        fixture.RemoveFromDirectory(
            scenario.finalize_result.notification_session_ids[i]);
    }
    adapter->OnFinalizeClose(scenario.finalize_result);

    // 夹具只调用了一次 FinalizeClose，且领域只产生一个 MEETING_CLOSED。
    EXPECT_EQ(scenario.finalize_calls, 1);
    EXPECT_EQ(scenario.finalize_result.events.size(), static_cast<std::size_t>(1));

    // 结构保证：adapter 没有任何领域 mutation 能力 —— 它不持有 MeetingService，
    // 真实代码里不存在 FinalizeClose 调用（去注释后核对）。
    const std::string adapter_code =
        StripComments(ReadSourceFile(kAdapterSourcePath));
    ASSERT_FALSE(adapter_code.empty()) << "cannot read " << kAdapterSourcePath;
    EXPECT_EQ(adapter_code.find(".FinalizeClose("), std::string::npos);
    // 不持有 MeetingService 实例（"MeetingService " 带空格 ⇒ 只匹配类型/成员用法，
    // 不会误伤 #include "meeting/MeetingService.h" 与 MeetingServiceResultCommon）。
    EXPECT_EQ(adapter_code.find("MeetingService "), std::string::npos);
}

// ------------------------------------------------------------------------------
// ND6：重复 FinalizeClose 不重复通知（already_finalized 路径没有通知意图）。
// ------------------------------------------------------------------------------
TEST(MeetingNotificationPath, ND6_RepeatedFinalizeCloseProducesNoNotification) {
    AdapterTestFixture fixture;
    RecordingMessageCollector collector;
    const std::unique_ptr<MeetingResponseNotificationAdapter> adapter =
        fixture.MakeAdapter(collector);

    const ClosedMeetingScenario scenario = fixture.CloseMeeting(*adapter);
    adapter->OnFinalizeClose(scenario.finalize_result);

    const std::size_t after_first = collector
                                        .OfKind(MeetingOutboundKind::NOTIFICATION)
                                        .size();
    ASSERT_EQ(after_first, static_cast<std::size_t>(3));

    // 第二次 FinalizeClose：领域返回幂等结果，notification_session_ids 为空。
    const meeting::FinalizeCloseResult second =
        fixture.Service().FinalizeClose(scenario.MeetingId());
    EXPECT_FALSE(second.finalized);
    EXPECT_TRUE(second.already_finalized);
    EXPECT_TRUE(second.notification_session_ids.empty());
    EXPECT_TRUE(second.events.empty());

    const MeetingAdapterCounters before = adapter->Snapshot();
    adapter->OnFinalizeClose(second);
    const MeetingAdapterCounters delta = Delta(before, adapter->Snapshot());

    EXPECT_EQ(collector.OfKind(MeetingOutboundKind::NOTIFICATION).size(), after_first);
    EXPECT_EQ(delta.notification_produced, static_cast<std::uint64_t>(0));
    EXPECT_EQ(delta.target_resolved, static_cast<std::uint64_t>(0));
    EXPECT_EQ(delta.target_missing, static_cast<std::uint64_t>(0));
    EXPECT_EQ(delta.directory_unavailable, static_cast<std::uint64_t>(0));
}

// ------------------------------------------------------------------------------
// ND7：weak directory 失效 ⇒ 整批 no-op（0 条消息、0 次 Collect），不崩溃。
// ------------------------------------------------------------------------------
TEST(MeetingNotificationPath, ND7_ExpiredDirectoryIsAWholeBatchNoOp) {
    AdapterTestFixture fixture;
    RecordingMessageCollector collector;

    // 领域与判活目录都正常存在；只有 adapter 持有的 weak 句柄失效。
    RecordingMessageCollector domain_collector;
    const std::unique_ptr<MeetingResponseNotificationAdapter> domain_adapter =
        fixture.MakeAdapter(domain_collector);
    const ClosedMeetingScenario scenario = fixture.CloseMeeting(*domain_adapter);

    const std::unique_ptr<MeetingResponseNotificationAdapter> adapter =
        fixture.MakeAdapterWithExpiredDirectory(collector);

    adapter->OnFinalizeClose(scenario.finalize_result);

    EXPECT_EQ(collector.Size(), static_cast<std::size_t>(0));
    const MeetingAdapterCounters counters = adapter->Snapshot();
    EXPECT_EQ(counters.directory_unavailable, static_cast<std::uint64_t>(1));
    EXPECT_EQ(counters.target_resolved, static_cast<std::uint64_t>(0));
    EXPECT_EQ(counters.target_missing, static_cast<std::uint64_t>(0));
    EXPECT_EQ(counters.notification_produced, static_cast<std::uint64_t>(0));
}

// ==============================================================================
// 组 4：best-effort / 可观测性边界（BF1–BF6）
//
// §13.3 的可观测性档位：L0 target_missing / L1 directory_unavailable /
// L2 outbound_produced / L3 adapter_exception 可统计；L4（异步写失败）与
// L5（客户端已收到）在 M6 **不可统计、不可验证**，本组也不作任何声称。
// ==============================================================================

// ------------------------------------------------------------------------------
// BF1：L0–L3 计数准确。
//
// 一个用例里分别构造四个档位，逐一断言**精确**增量（不是"大于 0"）。
// ------------------------------------------------------------------------------
TEST(MeetingAdapterBestEffort, BF1_L0ToL3CountersAreExact) {
    AdapterTestFixture fixture;

    RecordingMessageCollector domain_collector;
    const std::unique_ptr<MeetingResponseNotificationAdapter> adapter =
        fixture.MakeAdapter(domain_collector);
    const OpenMeetingScenario scenario = fixture.OpenMeeting(*adapter);

    // ---- L2：目标存活 ⇒ 产出一条 response ----------------------------------
    {
        const meeting::MeetingRequestContext context =
            fixture.Context("req-query", scenario.host_session, scenario.host_user);
        const meeting::QueryMeetingResult result =
            fixture.Service().QueryMeeting(context, scenario.meeting_id);
        ASSERT_EQ(result.common.result_code, meeting::ResultCode::OK);

        const MeetingAdapterCounters before = adapter->Snapshot();
        adapter->OnQueryMeeting(context, result);
        const MeetingAdapterCounters delta = Delta(before, adapter->Snapshot());
        EXPECT_EQ(delta.response_produced, static_cast<std::uint64_t>(1));
        EXPECT_EQ(delta.outbound_produced, static_cast<std::uint64_t>(1));
        EXPECT_EQ(delta.target_resolved, static_cast<std::uint64_t>(1));
        EXPECT_EQ(delta.target_missing, static_cast<std::uint64_t>(0));
    }

    // ---- L0：lookup 为空 ⇒ 只计 target_missing，不产出 ----------------------
    {
        fixture.RemoveFromDirectory(scenario.host_session);
        const meeting::MeetingRequestContext context =
            fixture.Context("req-query-gone", scenario.host_session, scenario.host_user);
        const meeting::QueryMeetingResult result =
            fixture.Service().QueryMeeting(context, scenario.meeting_id);
        // 领域判定不受传输侧存活性影响（§13.2(2)）。
        ASSERT_EQ(result.common.result_code, meeting::ResultCode::OK);

        const std::size_t before_size = domain_collector.Size();
        const MeetingAdapterCounters before = adapter->Snapshot();
        adapter->OnQueryMeeting(context, result);
        const MeetingAdapterCounters delta = Delta(before, adapter->Snapshot());
        EXPECT_EQ(delta.response_produced, static_cast<std::uint64_t>(0));
        EXPECT_EQ(delta.target_missing, static_cast<std::uint64_t>(1));
        EXPECT_EQ(delta.target_resolved, static_cast<std::uint64_t>(0));
        EXPECT_EQ(domain_collector.Size(), before_size);
        fixture.RestoreToDirectory(scenario.host_session);
    }

    // ---- L1：目录不可用 ⇒ directory_unavailable，整批 no-op -----------------
    {
        RecordingMessageCollector orphan_collector;
        const std::unique_ptr<MeetingResponseNotificationAdapter> orphan =
            fixture.MakeAdapterWithExpiredDirectory(orphan_collector);
        const meeting::MeetingRequestContext context =
            fixture.Context("req-query-orphan", scenario.host_session, scenario.host_user);
        const meeting::QueryMeetingResult result =
            fixture.Service().QueryMeeting(context, scenario.meeting_id);
        ASSERT_EQ(result.common.result_code, meeting::ResultCode::OK);

        orphan->OnQueryMeeting(context, result);
        const MeetingAdapterCounters counters = orphan->Snapshot();
        EXPECT_EQ(counters.directory_unavailable, static_cast<std::uint64_t>(1));
        EXPECT_EQ(counters.response_produced, static_cast<std::uint64_t>(0));
        EXPECT_EQ(counters.target_resolved, static_cast<std::uint64_t>(0));
        EXPECT_EQ(counters.target_missing, static_cast<std::uint64_t>(0));
        EXPECT_EQ(orphan_collector.Size(), static_cast<std::size_t>(0));
    }

    // ---- L3：collector 违约抛异常 ⇒ adapter_exception ----------------------
    {
        RecordingMessageCollector throwing_collector;
        throwing_collector.SetThrowEnabled(true);   // 只让本次调用违约
        const std::unique_ptr<MeetingResponseNotificationAdapter> fragile =
            fixture.MakeAdapter(throwing_collector);
        const meeting::MeetingRequestContext context =
            fixture.Context("req-query-throw", scenario.host_session, scenario.host_user);
        const meeting::QueryMeetingResult result =
            fixture.Service().QueryMeeting(context, scenario.meeting_id);
        ASSERT_EQ(result.common.result_code, meeting::ResultCode::OK);

        fragile->OnQueryMeeting(context, result);
        const MeetingAdapterCounters counters = fragile->Snapshot();
        EXPECT_EQ(counters.adapter_exception, static_cast<std::uint64_t>(1));
        EXPECT_EQ(counters.response_produced, static_cast<std::uint64_t>(0));
        // 异常发生在 Collect 内部：目标本身是解析成功的。
        EXPECT_EQ(counters.target_resolved, static_cast<std::uint64_t>(1));
        EXPECT_EQ(throwing_collector.Size(), static_cast<std::size_t>(0));
    }
}

// ------------------------------------------------------------------------------
// BF2：collector 抛异常被吞掉，**后续处理仍可继续**（不终止消费者线程）。
// ------------------------------------------------------------------------------
TEST(MeetingAdapterBestEffort, BF2_CollectorExceptionIsIsolatedAndLaterWorkContinues) {
    AdapterTestFixture fixture;
    RecordingMessageCollector collector;   // 默认不抛：生命周期阶段正常产出
    const std::unique_ptr<MeetingResponseNotificationAdapter> adapter =
        fixture.MakeAdapter(collector);
    const OpenMeetingScenario scenario = fixture.OpenMeeting(*adapter);
    ASSERT_EQ(collector.Size(), static_cast<std::size_t>(3));

    const meeting::MeetingRequestContext context =
        fixture.Context("req-query", scenario.host_session, scenario.host_user);
    const meeting::QueryMeetingResult result =
        fixture.Service().QueryMeeting(context, scenario.meeting_id);
    ASSERT_EQ(result.common.result_code, meeting::ResultCode::OK);

    // 第 1 次：collector 违约，异常不得逃逸（否则本用例直接崩溃）。
    collector.SetThrowEnabled(true);
    const MeetingAdapterCounters before_first = adapter->Snapshot();
    adapter->OnQueryMeeting(context, result);
    const MeetingAdapterCounters after_first = Delta(before_first, adapter->Snapshot());
    EXPECT_EQ(after_first.adapter_exception, static_cast<std::uint64_t>(1));
    EXPECT_EQ(after_first.response_produced, static_cast<std::uint64_t>(0));
    EXPECT_EQ(collector.Size(), static_cast<std::size_t>(3));   // 未记录失败消息

    // 第 2 次：同一 adapter 仍然工作，产出与计数恢复正常。
    collector.SetThrowEnabled(false);
    const MeetingAdapterCounters before_second = adapter->Snapshot();
    adapter->OnQueryMeeting(context, result);
    const MeetingAdapterCounters after_second = Delta(before_second, adapter->Snapshot());
    EXPECT_EQ(after_second.adapter_exception, static_cast<std::uint64_t>(0));
    EXPECT_EQ(after_second.response_produced, static_cast<std::uint64_t>(1));
    EXPECT_EQ(collector.Size(), static_cast<std::size_t>(4));

    // 两次尝试：一次违约、一次成功（Attempts 记录尝试，Size 记录成功）。
    EXPECT_EQ(collector.Attempts(), static_cast<std::uint64_t>(5));
}

// ------------------------------------------------------------------------------
// BF3：不 sleep、不阻塞、不做同步网络 I/O、不等待队列（§9.2(3)）。
//
// 行为侧无法用计时断言（§16.2 BF3 明确"不用计时"），因此：
//   · 静态核对：真实源码里不存在 sleep / 线程 / 条件变量 / socket / 队列等待；
//   · 行为核对：一次调用即可完成并改变计数（调用序断言，不涉及时钟）。
// ------------------------------------------------------------------------------
TEST(MeetingAdapterBestEffort, BF3_AdapterDoesNotSleepBlockOrWait) {
    const std::string header_code = StripComments(ReadSourceFile(kAdapterHeaderPath));
    const std::string source_code = StripComments(ReadSourceFile(kAdapterSourcePath));
    ASSERT_FALSE(header_code.empty()) << "cannot read " << kAdapterHeaderPath;
    ASSERT_FALSE(source_code.empty()) << "cannot read " << kAdapterSourcePath;

    const char* const forbidden[] = {
        "sleep_for", "sleep_until", "usleep", "std::this_thread",
        "condition_variable", "std::mutex", "std::lock_guard",
        "future", "promise", "async", "socket", "asio", "boost",
        "Send(", "WaitFor", "wait_for", "Pop(", "Enqueue", "PostInternalWork"
    };
    for (std::size_t i = 0; i < sizeof(forbidden) / sizeof(forbidden[0]); ++i) {
        EXPECT_EQ(source_code.find(forbidden[i]), std::string::npos)
            << "adapter source must not contain: " << forbidden[i];
        EXPECT_EQ(header_code.find(forbidden[i]), std::string::npos)
            << "adapter header must not contain: " << forbidden[i];
    }

    // 行为核对：一次调用即可完成，且只依赖自身计数（无外部等待）。
    AdapterTestFixture fixture;
    RecordingMessageCollector collector;
    const std::unique_ptr<MeetingResponseNotificationAdapter> adapter =
        fixture.MakeAdapter(collector);
    const OpenMeetingScenario scenario = fixture.OpenMeeting(*adapter);

    const meeting::MeetingRequestContext context =
        fixture.Context("req-query", scenario.host_session, scenario.host_user);
    const meeting::QueryMeetingResult result =
        fixture.Service().QueryMeeting(context, scenario.meeting_id);
    ASSERT_EQ(result.common.result_code, meeting::ResultCode::OK);

    const std::size_t before_size = collector.Size();
    adapter->OnQueryMeeting(context, result);
    EXPECT_EQ(collector.Size(), before_size + 1);
}

// ------------------------------------------------------------------------------
// BF4：同一目标不重试、不过度调用 Collect（§16.2 BF4 / §9.1）。
// ------------------------------------------------------------------------------
TEST(MeetingAdapterBestEffort, BF4_EachTargetIsAttemptedExactlyOnce) {
    // (a) 正常 collector：每个目标恰好一条消息，且没有重复目标。
    {
        AdapterTestFixture fixture;
        RecordingMessageCollector collector;
        const std::unique_ptr<MeetingResponseNotificationAdapter> adapter =
            fixture.MakeAdapter(collector);
        const ClosedMeetingScenario scenario = fixture.CloseMeeting(*adapter);

        const std::uint64_t before_attempts =
            collector.AttemptsOf(MeetingOutboundKind::NOTIFICATION);
        adapter->OnFinalizeClose(scenario.finalize_result);

        const std::vector<MeetingOutboundMessage> notifications =
            collector.OfKind(MeetingOutboundKind::NOTIFICATION);
        EXPECT_EQ(collector.AttemptsOf(MeetingOutboundKind::NOTIFICATION) -
                      before_attempts,
                  static_cast<std::uint64_t>(notifications.size()));

        std::vector<std::string> targets;
        for (std::size_t i = 0; i < notifications.size(); ++i) {
            targets.push_back(
                notifications[i].notification.notification_target_session_id);
        }
        const std::vector<std::string> sorted = Sorted(targets);
        for (std::size_t i = 1; i < sorted.size(); ++i) {
            EXPECT_NE(sorted[i], sorted[i - 1]) << "duplicate target: " << sorted[i];
        }
    }

    // (b) 违约 collector：每个目标只被**尝试**一次（不重试），异常不外逸。
    {
        AdapterTestFixture fixture;
        RecordingMessageCollector collector;   // 在 finalize 之前打开抛异常
        const std::unique_ptr<MeetingResponseNotificationAdapter> adapter =
            fixture.MakeAdapter(collector);
        const ClosedMeetingScenario scenario = fixture.CloseMeeting(*adapter);

        collector.SetThrowEnabled(true);   // 只让 finalize 的通知产出违约
        adapter->OnFinalizeClose(scenario.finalize_result);

        const std::vector<std::string> attempted =
            collector.AttemptedTargetsOf(MeetingOutboundKind::NOTIFICATION);
        EXPECT_EQ(attempted.size(),
                  scenario.finalize_result.notification_session_ids.size());
        const std::vector<std::string> sorted = Sorted(attempted);
        for (std::size_t i = 1; i < sorted.size(); ++i) {
            EXPECT_NE(sorted[i], sorted[i - 1]) << "retried target: " << sorted[i];
        }

        const MeetingAdapterCounters counters = adapter->Snapshot();
        EXPECT_EQ(counters.notification_produced, static_cast<std::uint64_t>(0));
        EXPECT_EQ(counters.adapter_exception, static_cast<std::uint64_t>(3));
    }
}

// ------------------------------------------------------------------------------
// BF5：不得把"typed 消息已产出"表述成"已发送 / 已送达"（§7.5 / §13.3）。
//
// 静态核对目标：生产代码里不存在任何 delivery 语义的标识符；计数命名一律是
// produced / accepted 语义（outbound_produced）。
// ------------------------------------------------------------------------------
TEST(MeetingAdapterBestEffort, BF5_NoDeliveryClaimInProductionCode) {
    const std::string adapter_code = StripComments(ReadSourceFile(kAdapterSourcePath));
    const std::string adapter_header = StripComments(ReadSourceFile(kAdapterHeaderPath));
    const std::string collector_code = StripComments(ReadSourceFile(kCollectorSourcePath));
    const std::string collector_header = StripComments(ReadSourceFile(kCollectorHeaderPath));
    ASSERT_FALSE(adapter_code.empty());
    ASSERT_FALSE(adapter_header.empty());
    ASSERT_FALSE(collector_code.empty());
    ASSERT_FALSE(collector_header.empty());

    // 暗示"已交付"的标识符：一律不得出现。
    const char* const delivery_claims[] = {
        "delivered", "Delivered", "acked", "Acked", "Acknowledge",
        "send_success", "SendSuccess", "flushed", "Flushed",
        "bytes_written", "BytesWritten"
    };
    const std::string codes[4] = {adapter_code, adapter_header, collector_code,
                                  collector_header};
    for (std::size_t i = 0; i < 4; ++i) {
        for (std::size_t j = 0; j < sizeof(delivery_claims) / sizeof(delivery_claims[0]); ++j) {
            EXPECT_EQ(codes[i].find(delivery_claims[j]), std::string::npos)
                << "delivery claim found: " << delivery_claims[j];
        }
    }

    // 正面要求：计数以 produced 命名（"已产出"而不是"已发送"）。
    EXPECT_NE(adapter_header.find("outbound_produced"), std::string::npos);
    EXPECT_NE(adapter_code.find("response_produced_"), std::string::npos);
    EXPECT_NE(adapter_code.find("notification_produced_"), std::string::npos);
}

// ------------------------------------------------------------------------------
// BF6：生产 CountingNoWireCollector 只计数、不发送（可运行 + 静态双重核对）。
// ------------------------------------------------------------------------------
TEST(MeetingAdapterBestEffort, BF6_ProductionCollectorCountsWithoutSending) {
    AdapterTestFixture fixture;
    CountingNoWireCollector collector;   // **真实**生产 collector
    const std::unique_ptr<MeetingResponseNotificationAdapter> adapter =
        fixture.MakeAdapter(collector);

    // 真实驱动：create / join / join / close 各 1 条 response，finalize 3 条通知。
    const ClosedMeetingScenario scenario = fixture.CloseMeeting(*adapter);
    adapter->OnFinalizeClose(scenario.finalize_result);

    const CountingNoWireCollectorSnapshot snapshot = collector.Snapshot();
    EXPECT_EQ(snapshot.responses, static_cast<std::uint64_t>(4));
    EXPECT_EQ(snapshot.notifications, static_cast<std::uint64_t>(3));
    EXPECT_EQ(snapshot.total, static_cast<std::uint64_t>(7));
    EXPECT_EQ(snapshot.unknown_kind, static_cast<std::uint64_t>(0));
    EXPECT_EQ(collector.ResponseCount(), static_cast<std::uint64_t>(4));
    EXPECT_EQ(collector.NotificationCount(), static_cast<std::uint64_t>(3));
    EXPECT_EQ(collector.TotalCount(), static_cast<std::uint64_t>(7));

    // 计数与 adapter 自己的计数一致（两条独立观测路径相互印证）。
    const MeetingAdapterCounters counters = adapter->Snapshot();
    EXPECT_EQ(counters.response_produced, snapshot.responses);
    EXPECT_EQ(counters.notification_produced, snapshot.notifications);

    // 静态核对：生产 collector 的**代码**里没有任何发送 / 会话 / 网络运行时依赖。
    const std::string code = StripComments(ReadSourceFile(kCollectorSourcePath));
    const std::string header = StripComments(ReadSourceFile(kCollectorHeaderPath));
    ASSERT_FALSE(code.empty()) << "cannot read " << kCollectorSourcePath;
    ASSERT_FALSE(header.empty()) << "cannot read " << kCollectorHeaderPath;

    const char* const forbidden[] = {
        "Send", "send", "CSession", "CServer", "LogicSystem",
        "socket", "asio", "boost", "hiredis", "mysql", "grpc", "protobuf",
        "write(", "fwrite", "fprintf", "std::cout", "std::cerr"
    };
    for (std::size_t i = 0; i < sizeof(forbidden) / sizeof(forbidden[0]); ++i) {
        EXPECT_EQ(code.find(forbidden[i]), std::string::npos)
            << "collector source must not contain: " << forbidden[i];
        EXPECT_EQ(header.find(forbidden[i]), std::string::npos)
            << "collector header must not contain: " << forbidden[i];
    }
}

// ==============================================================================
// 组 5：MeetingView 安全边界（MV1–MV7）+ response 字段映射
//
// 全部断言都基于**真实领域**产生的授权/拒绝结果：测试不构造受控 view，
// 也不复制 Adapter 的安全判断（§10.1 / §10.3 的矩阵由生产代码执行）。
// ==============================================================================

namespace {

// 陌生人会话：已认证、已存活，但从未加入任何会议。
const char* const kStrangerSession = "s-stranger";
const int kStrangerUser = 999;

}  // namespace

// ------------------------------------------------------------------------------
// MV1：OK ⇒ has_view == true，字段完整且与领域一致。
// ------------------------------------------------------------------------------
TEST(MeetingViewSecurity, MV1_AuthorizedQueryEncodesCompleteView) {
    AdapterTestFixture fixture;
    RecordingMessageCollector collector;
    const std::unique_ptr<MeetingResponseNotificationAdapter> adapter =
        fixture.MakeAdapter(collector);
    const OpenMeetingScenario scenario = fixture.OpenMeeting(*adapter);

    const meeting::MeetingRequestContext context =
        fixture.Context("req-query-host", scenario.host_session, scenario.host_user);
    const meeting::QueryMeetingResult result =
        fixture.Service().QueryMeeting(context, scenario.meeting_id);
    ASSERT_EQ(result.common.result_code, meeting::ResultCode::OK);

    const std::size_t before_size = collector.Size();
    adapter->OnQueryMeeting(context, result);
    ASSERT_EQ(collector.Size(), before_size + 1);

    const std::vector<MeetingOutboundMessage> responses =
        collector.OfKind(MeetingOutboundKind::RESPONSE);
    const MeetingResponseMessage& response = responses.back().response;

    EXPECT_TRUE(response.has_view);
    EXPECT_EQ(response.view.meeting_id, scenario.meeting_id);
    EXPECT_EQ(response.view.owner_chat_server_id, std::string(kLocalServer));
    EXPECT_EQ(response.view.host_user_id, scenario.host_user);
    EXPECT_EQ(response.view.meeting_state, meeting::MeetingState::ACTIVE);
    EXPECT_EQ(response.view.active_participant_count, static_cast<std::size_t>(3));
    EXPECT_EQ(response.view.created_at, scenario.created_at);
    EXPECT_FALSE(response.view.has_closed_at);

    // 非 ListParticipants 的响应不带成员列表 / scope。
    EXPECT_FALSE(response.has_participants);
    EXPECT_FALSE(response.has_participant_scope);
    EXPECT_FALSE(response.has_owner_chat_server_id);
}

// ------------------------------------------------------------------------------
// MV2：PERMISSION_DENIED ⇒ has_view == false，且不带任何敏感 MeetingView。
// ------------------------------------------------------------------------------
TEST(MeetingViewSecurity, MV2_PermissionDeniedEncodesNoView) {
    AdapterTestFixture fixture;
    RecordingMessageCollector collector;
    const std::unique_ptr<MeetingResponseNotificationAdapter> adapter =
        fixture.MakeAdapter(collector);
    const OpenMeetingScenario scenario = fixture.OpenMeeting(*adapter);

    // 非 Host 尝试 Close：领域返回 PERMISSION_DENIED（不填 view）。
    const meeting::MeetingRequestContext context =
        fixture.Context("req-close-member", scenario.member_a_session, scenario.user_a);
    const meeting::CloseMeetingResult result =
        fixture.Service().CloseMeeting(context, scenario.meeting_id);
    ASSERT_EQ(result.common.result_code, meeting::ResultCode::PERMISSION_DENIED);

    adapter->OnCloseMeeting(context, result);

    const std::vector<MeetingOutboundMessage> responses =
        collector.OfKind(MeetingOutboundKind::RESPONSE);
    ASSERT_FALSE(responses.empty());
    const MeetingResponseMessage& response = responses.back().response;

    EXPECT_EQ(response.result_code, meeting::ResultCode::PERMISSION_DENIED);
    EXPECT_EQ(response.result_name, std::string("PERMISSION_DENIED"));
    EXPECT_EQ(response.disposition, meeting::ResultDisposition::ERROR);

    EXPECT_FALSE(response.has_view);
    EXPECT_TRUE(response.view.meeting_id.empty());
    EXPECT_EQ(response.view.host_user_id, 0);
    EXPECT_EQ(response.view.active_participant_count, static_cast<std::size_t>(0));
    EXPECT_TRUE(response.view.owner_chat_server_id.empty());
    EXPECT_FALSE(response.view.has_closed_at);
    EXPECT_FALSE(response.has_participants);
    EXPECT_FALSE(response.has_participant_scope);
}

// ------------------------------------------------------------------------------
// MV3：NOT_PARTICIPANT ⇒ has_view == false（不得泄露会议摘要）。
// ------------------------------------------------------------------------------
TEST(MeetingViewSecurity, MV3_NotParticipantEncodesNoView) {
    AdapterTestFixture fixture;
    RecordingMessageCollector collector;
    const std::unique_ptr<MeetingResponseNotificationAdapter> adapter =
        fixture.MakeAdapter(collector);
    const OpenMeetingScenario scenario = fixture.OpenMeeting(*adapter);
    fixture.AddSessionEverywhere(kStrangerSession, kStrangerUser);

    const meeting::MeetingRequestContext context =
        fixture.Context("req-query-stranger", kStrangerSession, kStrangerUser);
    const meeting::QueryMeetingResult result =
        fixture.Service().QueryMeeting(context, scenario.meeting_id);
    ASSERT_EQ(result.common.result_code, meeting::ResultCode::NOT_PARTICIPANT);

    adapter->OnQueryMeeting(context, result);

    const std::vector<MeetingOutboundMessage> responses =
        collector.OfKind(MeetingOutboundKind::RESPONSE);
    ASSERT_FALSE(responses.empty());
    const MeetingResponseMessage& response = responses.back().response;

    EXPECT_FALSE(response.has_view);
    EXPECT_TRUE(response.view.meeting_id.empty());
    EXPECT_EQ(response.view.host_user_id, 0);
    EXPECT_EQ(response.view.active_participant_count, static_cast<std::size_t>(0));
}

// ------------------------------------------------------------------------------
// MV3b（安全加固的关键用例）：领域在**若干 ERROR 路径**上确实填充了 MeetingView，
// 但 §10.3 要求这些结果 has_view == false。
//
// 两个真实来源：
//   (A) CREATED/ACTIVE 下 host 调 Leave ⇒ HOST_MUST_CLOSE_MEETING
//       （MeetingAggregate::Leave 在入口就 BuildMeetingView()）
//   (B) ENDING 下 Join ⇒ MEETING_STATE_REJECTED
//       （MeetingAggregate::Join 同样在入口填充）
// 若编码路径只用"领域是否填了 view"一个判据，(A)(B) 就会把主持人 / 成员数 /
// 会议状态泄露给未授权或无权继续操作的调用方。
// ------------------------------------------------------------------------------
TEST(MeetingViewSecurity, MV3b_ErrorResultsWithDomainFilledViewAreStillNotEncoded) {
    // ---- (A) HOST_MUST_CLOSE_MEETING --------------------------------------
    {
        AdapterTestFixture fixture;
        RecordingMessageCollector collector;
        const std::unique_ptr<MeetingResponseNotificationAdapter> adapter =
            fixture.MakeAdapter(collector);
        const OpenMeetingScenario scenario = fixture.OpenMeeting(*adapter);

        const meeting::MeetingRequestContext context = fixture.Context(
            "req-leave-host", scenario.host_session, scenario.host_user);
        const meeting::LeaveMeetingResult result =
            fixture.Service().LeaveMeeting(context, scenario.meeting_id);
        ASSERT_EQ(result.common.result_code,
                  meeting::ResultCode::HOST_MUST_CLOSE_MEETING);
        // 前提：领域确实填了 view（否则本用例退化为 MV2 的同类断言）。
        ASSERT_FALSE(result.meeting.meeting_id.empty());
        ASSERT_EQ(result.meeting.host_user_id, scenario.host_user);

        adapter->OnLeaveMeeting(context, result);

        const std::vector<MeetingOutboundMessage> responses =
            collector.OfKind(MeetingOutboundKind::RESPONSE);
        ASSERT_FALSE(responses.empty());
        const MeetingResponseMessage& response = responses.back().response;
        EXPECT_EQ(response.result_code, meeting::ResultCode::HOST_MUST_CLOSE_MEETING);
        EXPECT_FALSE(response.has_view);
        EXPECT_TRUE(response.view.meeting_id.empty());
        EXPECT_EQ(response.view.host_user_id, 0);
    }

    // ---- (B) ENDING 下的 MEETING_STATE_REJECTED ---------------------------
    {
        AdapterTestFixture fixture;
        RecordingMessageCollector collector;
        const std::unique_ptr<MeetingResponseNotificationAdapter> adapter =
            fixture.MakeAdapter(collector);
        const OpenMeetingScenario scenario = fixture.OpenMeeting(*adapter);
        fixture.AddSessionEverywhere(kStrangerSession, kStrangerUser);

        const meeting::MeetingRequestContext close_context = fixture.Context(
            "req-close", scenario.host_session, scenario.host_user);
        const meeting::CloseMeetingResult close_result =
            fixture.Service().CloseMeeting(close_context, scenario.meeting_id);
        ASSERT_EQ(close_result.common.result_code, meeting::ResultCode::OK);

        const meeting::MeetingRequestContext join_context = fixture.Context(
            "req-join-ending", kStrangerSession, kStrangerUser);
        const meeting::JoinMeetingResult join_result =
            fixture.Service().JoinMeeting(join_context, scenario.meeting_id);
        ASSERT_EQ(join_result.common.result_code,
                  meeting::ResultCode::MEETING_STATE_REJECTED);
        ASSERT_FALSE(join_result.meeting.meeting_id.empty());

        adapter->OnJoinMeeting(join_context, join_result);

        const std::vector<MeetingOutboundMessage> responses =
            collector.OfKind(MeetingOutboundKind::RESPONSE);
        ASSERT_FALSE(responses.empty());
        const MeetingResponseMessage& response = responses.back().response;
        EXPECT_EQ(response.result_code, meeting::ResultCode::MEETING_STATE_REJECTED);
        EXPECT_FALSE(response.has_view);
        EXPECT_TRUE(response.view.meeting_id.empty());
    }
}

// ------------------------------------------------------------------------------
// MV4：CLOSED + 非历史成员 ⇒ NOT_PARTICIPANT，不泄露任何快照内容。
// ------------------------------------------------------------------------------
TEST(MeetingViewSecurity, MV4_ClosedMeetingNonMemberDoesNotLeakSnapshotView) {
    AdapterTestFixture fixture;
    RecordingMessageCollector collector;
    const std::unique_ptr<MeetingResponseNotificationAdapter> adapter =
        fixture.MakeAdapter(collector);

    fixture.AddSessionEverywhere(kStrangerSession, kStrangerUser);
    const ClosedMeetingScenario scenario = fixture.CloseMeeting(*adapter);

    const meeting::MeetingRequestContext context =
        fixture.Context("req-query-stranger", kStrangerSession, kStrangerUser);
    const meeting::QueryMeetingResult result =
        fixture.Service().QueryMeeting(context, scenario.MeetingId());
    ASSERT_EQ(result.common.result_code, meeting::ResultCode::NOT_PARTICIPANT);

    adapter->OnQueryMeeting(context, result);

    const std::vector<MeetingOutboundMessage> responses =
        collector.OfKind(MeetingOutboundKind::RESPONSE);
    ASSERT_FALSE(responses.empty());
    const MeetingResponseMessage& response = responses.back().response;
    EXPECT_FALSE(response.has_view);
    EXPECT_TRUE(response.view.meeting_id.empty());
    EXPECT_FALSE(response.view.has_closed_at);
    EXPECT_EQ(response.view.active_participant_count, static_cast<std::size_t>(0));
}

// ------------------------------------------------------------------------------
// MV5：CLOSED + 历史成员 ⇒ OK，视图来自真实 snapshot（closed_at 有效）。
// ------------------------------------------------------------------------------
TEST(MeetingViewSecurity, MV5_ClosedMeetingHistoricalMemberGetsSnapshotView) {
    AdapterTestFixture fixture;
    RecordingMessageCollector collector;
    const std::unique_ptr<MeetingResponseNotificationAdapter> adapter =
        fixture.MakeAdapter(collector);
    const ClosedMeetingScenario scenario = fixture.CloseMeeting(*adapter);

    const meeting::MeetingRequestContext context = fixture.Context(
        "req-query-member", scenario.open.member_a_session, scenario.open.user_a);
    const meeting::QueryMeetingResult result =
        fixture.Service().QueryMeeting(context, scenario.MeetingId());
    ASSERT_EQ(result.common.result_code, meeting::ResultCode::OK);
    ASSERT_EQ(result.meeting.meeting_state, meeting::MeetingState::CLOSED);

    adapter->OnQueryMeeting(context, result);

    const std::vector<MeetingOutboundMessage> responses =
        collector.OfKind(MeetingOutboundKind::RESPONSE);
    ASSERT_FALSE(responses.empty());
    const MeetingResponseMessage& response = responses.back().response;

    EXPECT_TRUE(response.has_view);
    EXPECT_EQ(response.view.meeting_id, scenario.MeetingId());
    EXPECT_EQ(response.view.meeting_state, meeting::MeetingState::CLOSED);
    EXPECT_TRUE(response.view.has_closed_at);
    EXPECT_EQ(response.view.closed_at, result.meeting.closed_at);
    EXPECT_EQ(response.view.host_user_id, scenario.open.host_user);
    // 终态摘要的成员数来自冻结边界（3 名 ACTIVE 成员）。
    EXPECT_EQ(response.view.active_participant_count, static_cast<std::size_t>(3));
}

// ------------------------------------------------------------------------------
// MV6：合法**幂等**结果按真实领域结果透传 —— 不清空、也不额外填充。
// ------------------------------------------------------------------------------
TEST(MeetingViewSecurity, MV6_IdempotentResultPassesDomainViewThrough) {
    AdapterTestFixture fixture;
    RecordingMessageCollector collector;
    const std::unique_ptr<MeetingResponseNotificationAdapter> adapter =
        fixture.MakeAdapter(collector);
    const OpenMeetingScenario scenario = fixture.OpenMeeting(*adapter);

    // 同一 session 再次 Join ⇒ ALREADY_JOINED（IDEMPOTENT），领域会填充 view。
    const meeting::MeetingRequestContext context = fixture.Context(
        "req-join-again", scenario.member_a_session, scenario.user_a);
    const meeting::JoinMeetingResult result =
        fixture.Service().JoinMeeting(context, scenario.meeting_id);
    ASSERT_EQ(result.common.result_code, meeting::ResultCode::ALREADY_JOINED);
    ASSERT_EQ(result.common.disposition, meeting::ResultDisposition::IDEMPOTENT);
    ASSERT_FALSE(result.meeting.meeting_id.empty());

    adapter->OnJoinMeeting(context, result);

    const std::vector<MeetingOutboundMessage> responses =
        collector.OfKind(MeetingOutboundKind::RESPONSE);
    ASSERT_FALSE(responses.empty());
    const MeetingResponseMessage& response = responses.back().response;

    EXPECT_EQ(response.result_code, meeting::ResultCode::ALREADY_JOINED);
    EXPECT_EQ(response.result_name, std::string("ALREADY_JOINED"));
    EXPECT_EQ(response.disposition, meeting::ResultDisposition::IDEMPOTENT);

    // 幂等命中意味着授权通过 ⇒ 保留领域填好的视图，逐字段与领域结果一致。
    EXPECT_TRUE(response.has_view);
    EXPECT_EQ(response.view.meeting_id, result.meeting.meeting_id);
    EXPECT_EQ(response.view.host_user_id, result.meeting.host_user_id);
    EXPECT_EQ(response.view.active_participant_count,
              result.meeting.active_participant_count);
    EXPECT_EQ(response.view.meeting_state, result.meeting.meeting_state);
    EXPECT_EQ(response.view.created_at, result.meeting.created_at);
    EXPECT_EQ(response.view.has_closed_at, result.meeting.has_closed_at);
}

// ------------------------------------------------------------------------------
// MV7：字段读取静态核对 —— 敏感字段在本层**从逐字段读取**，只做整块透传。
// ------------------------------------------------------------------------------
TEST(MeetingViewSecurity, MV7_ViewFieldsAreOnlyTouchedBehindTheHasViewGate) {
    const std::string code = StripComments(ReadSourceFile(kAdapterSourcePath));
    ASSERT_FALSE(code.empty()) << "cannot read " << kAdapterSourcePath;

    // (1) 三个敏感字段在**代码**里 0 命中 ⇒ 本层没有逐字段读取它们的入口。
    EXPECT_EQ(CountOccurrences(code, "host_user_id"), static_cast<std::size_t>(0));
    EXPECT_EQ(CountOccurrences(code, "active_participant_count"),
              static_cast<std::size_t>(0));
    EXPECT_EQ(CountOccurrences(code, "meeting_state"), static_cast<std::size_t>(0));

    // (2) 视图只通过**一次整块拷贝**进入出站消息，且该拷贝位于 has_view 门内。
    EXPECT_EQ(CountOccurrences(code, "message.view = domain_view;"),
              static_cast<std::size_t>(1));
    const std::size_t gate = code.find("message.has_view = true;");
    const std::size_t copy = code.find("message.view = domain_view;");
    ASSERT_NE(gate, std::string::npos);
    ASSERT_NE(copy, std::string::npos);
    EXPECT_LT(gate, copy);

    // (3) owner 字段默认拒绝：先置 false，且置 true 只出现在 MEETING_NOT_LOCAL 分支内。
    const std::size_t owner_default = code.find("has_owner_chat_server_id = false;");
    const std::size_t owner_allow = code.find("has_owner_chat_server_id = true;");
    const std::size_t not_local = code.find("MEETING_NOT_LOCAL");
    ASSERT_NE(owner_default, std::string::npos);
    ASSERT_NE(owner_allow, std::string::npos);
    ASSERT_NE(not_local, std::string::npos);
    EXPECT_LT(owner_default, owner_allow);
    EXPECT_LT(not_local, owner_allow);

    // (4) 行为侧的最终证据：MV2/MV3/MV3b/MV4 已断言被拒绝的结果不带视图。
}

// ------------------------------------------------------------------------------
// RESP1：一个完整的 response 字段映射样本（ListParticipants，HISTORICAL）。
// ------------------------------------------------------------------------------
TEST(MeetingResponseMapping, RESP1_ListParticipantsFullFieldMapping) {
    AdapterTestFixture fixture;
    RecordingMessageCollector collector;
    const std::unique_ptr<MeetingResponseNotificationAdapter> adapter =
        fixture.MakeAdapter(collector);
    const ClosedMeetingScenario scenario = fixture.CloseMeeting(*adapter);

    const meeting::MeetingRequestContext context = fixture.Context(
        "req-list", scenario.open.host_session, scenario.open.host_user);
    const meeting::ListParticipantsResult result =
        fixture.Service().ListParticipants(context, scenario.MeetingId());
    ASSERT_EQ(result.common.result_code, meeting::ResultCode::OK);
    ASSERT_EQ(result.scope, meeting::ParticipantViewScope::HISTORICAL);

    const std::size_t before_size = collector.Size();
    adapter->OnListParticipants(context, result);
    ASSERT_EQ(collector.Size(), before_size + 1);

    const std::vector<MeetingOutboundMessage> responses =
        collector.OfKind(MeetingOutboundKind::RESPONSE);
    ASSERT_FALSE(responses.empty());
    const MeetingOutboundMessage& outbound = responses.back();

    // 容器与命令
    EXPECT_EQ(outbound.kind, MeetingOutboundKind::RESPONSE);
    EXPECT_EQ(outbound.response.command,
              ClientMeetingCommandKind::LIST_PARTICIPANTS);
    // correlation 与目标
    EXPECT_EQ(outbound.response.request_id, std::string("req-list"));
    EXPECT_EQ(outbound.response.response_target_session_id,
              std::string(scenario.open.host_session));
    // 领域结果三件套（照实透传，不改写）
    EXPECT_EQ(outbound.response.result_code, meeting::ResultCode::OK);
    EXPECT_EQ(outbound.response.result_name, std::string("OK"));
    EXPECT_EQ(outbound.response.disposition, meeting::ResultDisposition::SUCCESS);
    // 非 MEETING_NOT_LOCAL ⇒ 不带异地 owner
    EXPECT_FALSE(outbound.response.has_owner_chat_server_id);
    EXPECT_TRUE(outbound.response.owner_chat_server_id.empty());
    // 授权通过 ⇒ view + participants + scope 全部存在且来自真实领域
    EXPECT_TRUE(outbound.response.has_view);
    EXPECT_EQ(outbound.response.view.meeting_state, meeting::MeetingState::CLOSED);
    EXPECT_TRUE(outbound.response.has_participants);
    EXPECT_EQ(outbound.response.participants.size(), result.participants.size());
    EXPECT_EQ(outbound.response.participants.size(), static_cast<std::size_t>(3));
    EXPECT_TRUE(outbound.response.has_participant_scope);
    EXPECT_EQ(outbound.response.participant_scope,
              meeting::ParticipantViewScope::HISTORICAL);
}

// ------------------------------------------------------------------------------
// RESP2：response 目标已消失 ⇒ 0 条 response，且 result_code 不被改写（§13.2(2)）。
// ------------------------------------------------------------------------------
TEST(MeetingResponseMapping, RESP2_MissingTargetDoesNotChangeResultCode) {
    AdapterTestFixture fixture;
    RecordingMessageCollector collector;
    const std::unique_ptr<MeetingResponseNotificationAdapter> adapter =
        fixture.MakeAdapter(collector);
    const OpenMeetingScenario scenario = fixture.OpenMeeting(*adapter);

    const meeting::MeetingRequestContext context =
        fixture.Context("req-query", scenario.host_session, scenario.host_user);
    const meeting::QueryMeetingResult result =
        fixture.Service().QueryMeeting(context, scenario.meeting_id);
    ASSERT_EQ(result.common.result_code, meeting::ResultCode::OK);

    // 目标从注册表消失（等价于 ClearSession → erase）。
    fixture.RemoveFromDirectory(scenario.host_session);

    const std::size_t before_size = collector.Size();
    const MeetingAdapterCounters before = adapter->Snapshot();
    adapter->OnQueryMeeting(context, result);
    const MeetingAdapterCounters delta = Delta(before, adapter->Snapshot());

    EXPECT_EQ(collector.Size(), before_size);                 // 不产出
    EXPECT_EQ(delta.response_produced, static_cast<std::uint64_t>(0));
    EXPECT_EQ(delta.target_missing, static_cast<std::uint64_t>(1));

    // 领域结果完全没有被适配层改写（const 引用 + 无回写通道）。
    EXPECT_EQ(result.common.result_code, meeting::ResultCode::OK);
    EXPECT_EQ(result.common.result_name, std::string("OK"));
    EXPECT_EQ(result.common.disposition, meeting::ResultDisposition::SUCCESS);
}

// ------------------------------------------------------------------------------
// RESP3：未识别 ResultCode ⇒ 降级为 INTERNAL_ERROR + encoding_failed（§13.2(4)）。
// ------------------------------------------------------------------------------
TEST(MeetingResponseMapping, RESP3_UnknownResultCodeDegradesToInternalError) {
    AdapterTestFixture fixture;
    RecordingMessageCollector collector;
    const std::unique_ptr<MeetingResponseNotificationAdapter> adapter =
        fixture.MakeAdapter(collector);
    const OpenMeetingScenario scenario = fixture.OpenMeeting(*adapter);

    const meeting::MeetingRequestContext context =
        fixture.Context("req-query", scenario.host_session, scenario.host_user);
    const meeting::QueryMeetingResult valid =
        fixture.Service().QueryMeeting(context, scenario.meeting_id);
    ASSERT_EQ(valid.common.result_code, meeting::ResultCode::OK);

    // 人工构造"理论不可达"的未识别结果码；并故意给出自相矛盾的
    // disposition=SUCCESS 与非空 view，验证降级路径不会被它绕过。
    meeting::QueryMeetingResult bogus = valid;
    bogus.common.result_code = static_cast<meeting::ResultCode>(123456);
    bogus.common.result_name.clear();
    bogus.common.disposition = meeting::ResultDisposition::SUCCESS;

    const MeetingAdapterCounters before = adapter->Snapshot();
    adapter->OnQueryMeeting(context, bogus);
    const MeetingAdapterCounters delta = Delta(before, adapter->Snapshot());

    EXPECT_EQ(delta.encoding_failed, static_cast<std::uint64_t>(1));

    const std::vector<MeetingOutboundMessage> responses =
        collector.OfKind(MeetingOutboundKind::RESPONSE);
    ASSERT_FALSE(responses.empty());
    const MeetingResponseMessage& response = responses.back().response;

    // 不静默产出空响应；码被**显式**降级为 INTERNAL_ERROR。
    EXPECT_EQ(response.result_code, meeting::ResultCode::INTERNAL_ERROR);
    EXPECT_EQ(response.result_name, std::string("INTERNAL_ERROR"));
    EXPECT_EQ(response.disposition, meeting::ResultDisposition::ERROR);
    // 降级后属于 ERROR 等价类 ⇒ 不得编码视图（即便输入声称 SUCCESS 且有 view）。
    EXPECT_FALSE(response.has_view);
    EXPECT_TRUE(response.view.meeting_id.empty());
}

// ------------------------------------------------------------------------------
// RESP4：SESSION_STATE_REJECTED 不得被改写成 AUTH_REQUIRED（STEP-1.4 §19.4）。
// ------------------------------------------------------------------------------
TEST(MeetingResponseMapping, RESP4_SessionStateRejectedIsNotRewrittenAsAuthRequired) {
    AdapterTestFixture fixture;
    RecordingMessageCollector collector;
    const std::unique_ptr<MeetingResponseNotificationAdapter> adapter =
        fixture.MakeAdapter(collector);
    const OpenMeetingScenario scenario = fixture.OpenMeeting(*adapter);

    // 让该会话在**领域**侧终止（AUTHENTICATED → CLOSED），但传输侧仍然存活。
    const meeting::SessionDisconnectedResult disconnected =
        fixture.Service().SessionDisconnected(scenario.host_session);
    EXPECT_TRUE(disconnected.session_known);

    const meeting::MeetingRequestContext context =
        fixture.Context("req-query-closed-session", scenario.host_session,
                        scenario.host_user);
    const meeting::QueryMeetingResult result =
        fixture.Service().QueryMeeting(context, scenario.meeting_id);
    ASSERT_EQ(result.common.result_code, meeting::ResultCode::SESSION_STATE_REJECTED);

    adapter->OnQueryMeeting(context, result);

    const std::vector<MeetingOutboundMessage> responses =
        collector.OfKind(MeetingOutboundKind::RESPONSE);
    ASSERT_FALSE(responses.empty());
    const MeetingResponseMessage& response = responses.back().response;

    EXPECT_EQ(response.result_code, meeting::ResultCode::SESSION_STATE_REJECTED);
    EXPECT_EQ(response.result_name, std::string("SESSION_STATE_REJECTED"));
    EXPECT_NE(response.result_code, meeting::ResultCode::AUTH_REQUIRED);
    EXPECT_FALSE(response.has_view);
}

// ==============================================================================
// M6.5 / §16.2 组 6/7：dispatcher → adapter 的真实集成。
//
// 本节的每一个断言都建立在**真实生产链路**上：
//
//   测试线程                                         消费者线程（生产 WorkerLoop）
//   ────────                                         ────────────────────────────
//   executor.EnqueueBatchBaseline(items)  ──┐
//   executor.WaitUntilIdle(5000, base)      │        queue.Pop → DispatchOutsideLock
//                                           └──────► LogicWorkDispatcher::DispatchWork
//                                                      ├── MeetingService::<API>（真实领域）
//                                                      ├── sink_->Record*（原观测顺序）
//                                                      └── adapter_->On<API>（M6.5 新增）
//
// 测试**不复制**任何分发逻辑：它只入队 work item，然后等待真实执行器静止
// （§16.4 R1 认可的唯一等待边界），再对 collector / sink 的**加锁快照**做断言。
// 不使用 sleep、不断言队列瞬时 Size()/IsEmpty()。
// ==============================================================================

namespace {

const char* const kDispatchHostSession = "d-host";
const int kDispatchHostUser = 11;
const char* const kDispatchStrangerSession = "d-stranger";
const int kDispatchStrangerUser = 77;

// m6.5 用例依赖注入的确定性 ID 序列：全新 fixture 的第一个 meeting id 恒为它。
// 同一批次里需要"先 CREATE 再引用该 id"时只能靠这个可断言的前置事实，
// 因此每个用例都会先断言 CREATE 的响应确实用了这个 id。
const char* const kFirstMeetingId = "meeting-1";

// ------------------------------------------------------------------------------
// 记录型观测 sink（§16.4 R4：工作线程写、测试线程读 ⇒ 内部加锁）。
//
// 它只记录，不修改任何状态；同时扮演 executor 的"内部入队瞬间快照"观察者。
// ------------------------------------------------------------------------------
class RecordingObservationSink : public IWorkObservationSink {
public:
    void RecordWorkDispatched(const LogicWorkItem& item,
                              unsigned long long /*sequence*/,
                              const std::vector<WorkDomainEvent>& events) override {
        std::lock_guard<std::mutex> lock(mutex_);
        dispatched_kinds_.push_back(item.kind);
        dispatched_events_.push_back(events);
    }

    void RecordLegacyMessageRouted(const meeting::SessionId& /*session_id*/,
                                   short /*msg_id*/,
                                   std::size_t /*payload_size*/) override {
        std::lock_guard<std::mutex> lock(mutex_);
        legacy_routed_ += 1;
    }

    void RecordSessionDisconnected(const meeting::SessionId& /*session_id*/,
                                   std::size_t affected_meeting_count) override {
        std::lock_guard<std::mutex> lock(mutex_);
        disconnect_fanout_.push_back(affected_meeting_count);
    }

    void RecordFinalizeClose(const meeting::MeetingId& meeting_id,
                             bool finalized,
                             bool already_finalized,
                             const std::vector<meeting::SessionId>& notification_session_ids) override {
        std::lock_guard<std::mutex> lock(mutex_);
        finalize_meeting_ids_.push_back(meeting_id);
        finalize_flags_.push_back(std::make_pair(finalized, already_finalized));
        finalize_targets_.push_back(notification_session_ids);
    }

    void RecordMeetingCommand(const MeetingCommandOutcome& outcome) override {
        std::lock_guard<std::mutex> lock(mutex_);
        outcomes_.push_back(outcome);
    }

    void RecordInternalWorkEnqueued(const LogicWorkItem& item,
                                    const std::vector<LogicWorkItem>& queue_after) override {
        std::lock_guard<std::mutex> lock(mutex_);
        internal_enqueue_kinds_.push_back(item.kind);
        internal_enqueue_queue_kinds_.push_back(KindsOf(queue_after));
    }

    std::vector<MeetingCommandOutcome> Outcomes() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return outcomes_;
    }

    std::vector<std::vector<WorkDomainEvent> > DispatchedEvents() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return dispatched_events_;
    }

    std::vector<LogicWorkKind> DispatchedKinds() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return dispatched_kinds_;
    }

    std::vector<LogicWorkKind> InternalEnqueueKinds() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return internal_enqueue_kinds_;
    }

    std::vector<std::vector<LogicWorkKind> > InternalEnqueueQueueKinds() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return internal_enqueue_queue_kinds_;
    }

    std::vector<std::pair<bool, bool> > FinalizeFlags() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return finalize_flags_;
    }

    std::vector<std::vector<meeting::SessionId> > FinalizeTargets() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return finalize_targets_;
    }

private:
    static std::vector<LogicWorkKind> KindsOf(const std::vector<LogicWorkItem>& items) {
        std::vector<LogicWorkKind> kinds;
        kinds.reserve(items.size());
        for (std::size_t i = 0; i < items.size(); ++i) {
            kinds.push_back(items[i].kind);
        }
        return kinds;
    }

    mutable std::mutex mutex_;
    std::vector<LogicWorkKind> dispatched_kinds_;
    std::vector<std::vector<WorkDomainEvent> > dispatched_events_;
    std::size_t legacy_routed_ = 0;
    std::vector<std::size_t> disconnect_fanout_;
    std::vector<meeting::MeetingId> finalize_meeting_ids_;
    std::vector<std::pair<bool, bool> > finalize_flags_;
    std::vector<std::vector<meeting::SessionId> > finalize_targets_;
    std::vector<MeetingCommandOutcome> outcomes_;
    std::vector<LogicWorkKind> internal_enqueue_kinds_;
    std::vector<std::vector<LogicWorkKind> > internal_enqueue_queue_kinds_;
};

// ------------------------------------------------------------------------------
// 违约型 adapter：七项 On* **都抛异常**。
//
// M6.4 的生产实现是 noexcept，但**接口本身不是**，所以 dispatcher 必须自己兜住
// 抽象接口的违约实现（§13.4）。这个替身就是用来证明那一层的：
//   没有 dispatcher 的守卫时，异常会穿出 std::thread 入口 ⇒ std::terminate。
// ------------------------------------------------------------------------------
class ThrowingAdapter : public IMeetingResponseNotificationAdapter {
public:
    void OnCreateMeeting(const meeting::MeetingRequestContext&,
                         const meeting::CreateMeetingResult&) override {
        Throw();
    }
    void OnJoinMeeting(const meeting::MeetingRequestContext&,
                       const meeting::JoinMeetingResult&) override {
        Throw();
    }
    void OnLeaveMeeting(const meeting::MeetingRequestContext&,
                        const meeting::LeaveMeetingResult&) override {
        Throw();
    }
    void OnCloseMeeting(const meeting::MeetingRequestContext&,
                        const meeting::CloseMeetingResult&) override {
        Throw();
    }
    void OnQueryMeeting(const meeting::MeetingRequestContext&,
                        const meeting::QueryMeetingResult&) override {
        Throw();
    }
    void OnListParticipants(const meeting::MeetingRequestContext&,
                            const meeting::ListParticipantsResult&) override {
        Throw();
    }
    void OnFinalizeClose(const meeting::FinalizeCloseResult&) override {
        Throw();
    }

    std::uint64_t Calls() const { return calls_.load(); }

private:
    void Throw() {
        calls_.fetch_add(1);
        throw std::runtime_error("injected adapter contract violation");
    }

    std::atomic<std::uint64_t> calls_{0};
};

// ------------------------------------------------------------------------------
// 真实分发流水线：真实 executor → 真实 dispatcher → 真实 adapter → 真实领域。
//
// 成员声明顺序 = 构造顺序；逆序析构保证"worker 先停，再拆依赖"：
//   executor_（dtor 内 StopAndJoin）→ dispatcher_ → sink_/adapters → directory_ → collector_
// 也就是说 worker 引用的每一个对象都比它活得久（§8 的生命周期要求）。
// ------------------------------------------------------------------------------
class DispatchPipeline {
public:
    enum AdapterMode {
        kRealAdapter = 0,   // 真实 MeetingResponseNotificationAdapter
        kThrowingAdapter = 1
    };

    explicit DispatchPipeline(AdapterMode mode = kRealAdapter)
        : collector_(),
          directory_(std::make_shared<FakeSessionDirectory>()),
          sink_(),
          // 只构造被选中的那个 adapter；两者都声明在 dispatcher_ **之前**，
          // 因此 dispatcher_ 的初始化表达式可以安全取用其地址。
          real_adapter_(mode == kRealAdapter
                            ? new MeetingResponseNotificationAdapter(collector_, directory_)
                            : nullptr),
          throwing_adapter_(mode == kThrowingAdapter ? new ThrowingAdapter() : nullptr),
          dispatcher_(std::string(kLocalServer), MakeDeterministicDependencies(), &sink_,
                      real_adapter_
                          ? static_cast<IMeetingResponseNotificationAdapter*>(
                                real_adapter_.get())
                          : static_cast<IMeetingResponseNotificationAdapter*>(
                                throwing_adapter_.get())),
          executor_(dispatcher_, nullptr) {
        executor_.SetInternalEnqueueObserver(&sink_);
    }

    ~DispatchPipeline() { executor_.StopAndJoin(); }

    DispatchPipeline(const DispatchPipeline&) = delete;
    DispatchPipeline& operator=(const DispatchPipeline&) = delete;

    LogicWorkDispatcher& Dispatcher() { return dispatcher_; }
    SerializedWorkExecutor<LogicWorkDispatcher>& Executor() { return executor_; }
    RecordingObservationSink& Sink() { return sink_; }
    RecordingMessageCollector& Collector() { return collector_; }
    std::shared_ptr<FakeSessionDirectory> Directory() const { return directory_; }

    MeetingResponseNotificationAdapter& RealAdapter() { return *real_adapter_; }
    ThrowingAdapter& ThrowingTestAdapter() { return *throwing_adapter_; }

    // 判活目录项（传输侧存活）。领域会话记录由 INTERNAL_SESSION_* work 建立。
    void AddDirectoryEntry(const std::string& session_id) {
        directory_->Register(std::make_shared<FakeLiveSession>(session_id, destroy_count_));
    }

    // 等价于真实 ClearSession ⇒ erase：领域记录仍在，但传输侧已消失。
    void RemoveDirectoryEntry(const std::string& session_id) {
        directory_->Erase(session_id);
    }

    // 释放测试对目录的强引用 ⇒ adapter 的 weak 失效（§8.4 的既有契约）。
    void ReleaseDirectoryStrongRefs() { directory_.reset(); }

    static std::vector<LogicWorkItem> BootstrapWork(const std::string& session_id,
                                                   int user_id) {
        std::vector<LogicWorkItem> items;
        items.push_back(MakeSessionRegisterWork(session_id));
        items.push_back(MakeSessionAuthenticateWork(session_id, user_id));
        return items;
    }

    LogicWorkItem Command(const std::string& request_id,
                          const std::string& session_id,
                          int user_id,
                          ClientMeetingCommandKind command,
                          const std::string& meeting_id) const {
        const meeting::MeetingRequestContext context =
            builder_.Build(session_id, user_id, request_id, std::string(kLocalServer));
        return MakeMeetingCommandWork(context, command, meeting_id);
    }

    // 入队一批 work 并等待**这一批**被完全消费（唯一的等待边界，§16.4 R1）。
    bool EnqueueAndWait(const std::vector<LogicWorkItem>& items) {
        const LogicWorkIdleProgress baseline = executor_.EnqueueBatchBaseline(items);
        return executor_.WaitUntilIdle(5000, baseline);
    }

    bool EnqueueAndWait(const LogicWorkItem& item) {
        return EnqueueAndWait(std::vector<LogicWorkItem>(1, item));
    }

    std::vector<MeetingOutboundMessage> Responses() const {
        return collector_.OfKind(MeetingOutboundKind::RESPONSE);
    }

    std::vector<MeetingOutboundMessage> Notifications() const {
        return collector_.OfKind(MeetingOutboundKind::NOTIFICATION);
    }

    // 常用前置：建立 host 会话 + 创建会议（全部走真实 FIFO）。
    bool BootstrapHostAndCreate() {
        AddDirectoryEntry(kDispatchHostSession);
        if (!EnqueueAndWait(BootstrapWork(kDispatchHostSession, kDispatchHostUser))) {
            return false;
        }
        return EnqueueAndWait(Command("req-create", kDispatchHostSession,
                                      kDispatchHostUser, ClientMeetingCommandKind::CREATE,
                                      std::string()));
    }

private:
    // 声明顺序即构造顺序（见类注释）。
    RecordingMessageCollector collector_;
    std::shared_ptr<FakeSessionDirectory> directory_;
    RecordingObservationSink sink_;
    std::unique_ptr<MeetingResponseNotificationAdapter> real_adapter_;
    std::unique_ptr<ThrowingAdapter> throwing_adapter_;
    LogicWorkDispatcher dispatcher_;
    SerializedWorkExecutor<LogicWorkDispatcher> executor_;
    std::shared_ptr<int> destroy_count_ = std::make_shared<int>(0);
    MeetingRequestContextBuilder builder_;
};

std::vector<std::string> ResponseRequestIds(const std::vector<MeetingOutboundMessage>& responses) {
    std::vector<std::string> ids;
    ids.reserve(responses.size());
    for (std::size_t i = 0; i < responses.size(); ++i) {
        ids.push_back(responses[i].response.request_id);
    }
    return ids;
}

std::vector<ClientMeetingCommandKind> ResponseCommands(
    const std::vector<MeetingOutboundMessage>& responses) {
    std::vector<ClientMeetingCommandKind> commands;
    commands.reserve(responses.size());
    for (std::size_t i = 0; i < responses.size(); ++i) {
        commands.push_back(responses[i].response.command);
    }
    return commands;
}

}  // namespace

// ==============================================================================
// 组 6：CLOSING / CLOSED → SESSION_STATE_REJECTED（SS1–SS5）
//
// 全部经真实 FIFO / dispatcher 路径，断言落在**真实产出的 response 消息**或
// 真实 sink 记录的领域结果上，而不是直接调用 adapter。
// ==============================================================================

// ------------------------------------------------------------------------------
// SS1：CLOSING 与 CLOSED 落在**同一个**领域分支上，都返回 SESSION_STATE_REJECTED。
//
// 为什么这一条必须是静态核对：CLOSING 是**单次 mutation 内部的瞬态** ——
// MeetingService::SessionDisconnected 在同一个调用里先把记录置为 CLOSING（步骤 1），
// 完成 fan-out 清理后才置为 CLOSED（步骤 5）。因此任何**排队**的命令都观察不到
// CLOSING，只能观察到 CLOSED tombstone（SS2 已实测）。
// "CLOSING 状态的可运行构造"因此登记为 NOT VERIFIED（见报告），但可以核对：
// 该分支同时覆盖 CLOSING 与 CLOSED，且映射目标是 SESSION_STATE_REJECTED 而不是
// AUTH_REQUIRED —— 这正是 §16.2 SS1 要锁定的不变量。
// ------------------------------------------------------------------------------
TEST(DispatcherAdapterIntegration, SS1_SessionStateRejectedBranchCoversClosingAndClosed) {
    const std::string service_source = ReadSourceFile(kMeetingServiceSourcePath);
    ASSERT_FALSE(service_source.empty()) << "cannot read " << kMeetingServiceSourcePath;

    // (1) CLOSING / CLOSED 同分支，且该分支返回 SESSION_STATE_REJECTED。
    const std::size_t branch = service_source.find(
        "record.state == DomainSessionState::CLOSING");
    ASSERT_NE(branch, std::string::npos);
    const std::string window = service_source.substr(branch, 240);
    EXPECT_NE(window.find("DomainSessionState::CLOSED"), std::string::npos);
    EXPECT_NE(window.find("ResultCode::SESSION_STATE_REJECTED"), std::string::npos);
    EXPECT_EQ(window.find("ResultCode::AUTH_REQUIRED"), std::string::npos);

    // (2) CLOSING → CLOSED 的两次赋值都在 SessionDisconnected 内（瞬态前提）：
    //     若它们分属两个 mutation，CLOSING 就会变成可观测状态，本用例的前提失效。
    const std::string session_disconnected =
        FunctionWindow(service_source, "MeetingService::SessionDisconnected");
    ASSERT_FALSE(session_disconnected.empty());
    EXPECT_NE(session_disconnected.find("record.state = DomainSessionState::CLOSING;"),
              std::string::npos);
    EXPECT_NE(session_disconnected.find("record.state = DomainSessionState::CLOSED;"),
              std::string::npos);
}

// ------------------------------------------------------------------------------
// SS2：CLOSED tombstone ⇒ SESSION_STATE_REJECTED（而不是 AUTH_REQUIRED）。
// ------------------------------------------------------------------------------
TEST(DispatcherAdapterIntegration, SS2_ClosedTombstoneYieldsSessionStateRejected) {
    DispatchPipeline pipeline;
    ASSERT_TRUE(pipeline.BootstrapHostAndCreate());
    const std::string meeting_id = pipeline.Dispatcher().LastCreatedMeetingId();
    ASSERT_EQ(meeting_id, std::string(kFirstMeetingId));

    // 领域侧断线：AUTHENTICATED → CLOSING → CLOSED（tombstone 保留）。
    ASSERT_TRUE(pipeline.EnqueueAndWait(MakeSessionDisconnectedWork(kDispatchHostSession)));

    // 传输侧仍然存活（目录项未被移除）：因此 response 一定会被产出，
    // 断言落在产出的消息本身上。
    ASSERT_TRUE(pipeline.EnqueueAndWait(pipeline.Command(
        "req-query-closed-session", kDispatchHostSession, kDispatchHostUser,
        ClientMeetingCommandKind::QUERY, meeting_id)));

    const std::vector<MeetingOutboundMessage> responses = pipeline.Responses();
    ASSERT_EQ(responses.size(), static_cast<std::size_t>(2));   // create + 本条
    const MeetingResponseMessage& response = responses.back().response;

    EXPECT_EQ(response.result_code, meeting::ResultCode::SESSION_STATE_REJECTED);
    EXPECT_EQ(response.result_name, std::string("SESSION_STATE_REJECTED"));
    EXPECT_EQ(response.disposition, meeting::ResultDisposition::ERROR);
    EXPECT_NE(response.result_code, meeting::ResultCode::AUTH_REQUIRED);
    EXPECT_FALSE(response.has_view);

    // sink 侧记录的领域结果一致（两条独立观测路径互相印证）。
    const std::vector<MeetingCommandOutcome> outcomes = pipeline.Sink().Outcomes();
    ASSERT_FALSE(outcomes.empty());
    EXPECT_EQ(outcomes.back().result_code, meeting::ResultCode::SESSION_STATE_REJECTED);
}

// ------------------------------------------------------------------------------
// SS3：从未 REGISTER 的 Session ⇒ AUTH_REQUIRED（与 SS2 形成对照）。
// ------------------------------------------------------------------------------
TEST(DispatcherAdapterIntegration, SS3_NeverRegisteredSessionYieldsAuthRequired) {
    DispatchPipeline pipeline;
    ASSERT_TRUE(pipeline.BootstrapHostAndCreate());
    const std::string meeting_id = pipeline.Dispatcher().LastCreatedMeetingId();

    // 只存在于判活目录（传输侧存活），领域从未登记 ⇒ 未认证。
    pipeline.AddDirectoryEntry(kDispatchStrangerSession);

    ASSERT_TRUE(pipeline.EnqueueAndWait(pipeline.Command(
        "req-query-unknown", kDispatchStrangerSession, kDispatchStrangerUser,
        ClientMeetingCommandKind::QUERY, meeting_id)));

    const std::vector<MeetingOutboundMessage> responses = pipeline.Responses();
    ASSERT_EQ(responses.size(), static_cast<std::size_t>(2));
    const MeetingResponseMessage& response = responses.back().response;

    EXPECT_EQ(response.result_code, meeting::ResultCode::AUTH_REQUIRED);
    EXPECT_EQ(response.result_name, std::string("AUTH_REQUIRED"));
    EXPECT_NE(response.result_code, meeting::ResultCode::SESSION_STATE_REJECTED);
    EXPECT_EQ(response.response_target_session_id,
              std::string(kDispatchStrangerSession));
}

// ------------------------------------------------------------------------------
// SS4：即使 adapter 无法解析 response 目标，也**不能改写**领域 result_code。
// ------------------------------------------------------------------------------
TEST(DispatcherAdapterIntegration, SS4_UnresolvableTargetDoesNotRewriteDomainResult) {
    DispatchPipeline pipeline;
    ASSERT_TRUE(pipeline.BootstrapHostAndCreate());
    const std::string meeting_id = pipeline.Dispatcher().LastCreatedMeetingId();
    ASSERT_EQ(pipeline.Responses().size(), static_cast<std::size_t>(1));

    // 传输侧消失（等价于 ClearSession ⇒ erase），领域记录与会议都还在。
    pipeline.RemoveDirectoryEntry(kDispatchHostSession);

    const MeetingAdapterCounters before = pipeline.RealAdapter().Snapshot();
    ASSERT_TRUE(pipeline.EnqueueAndWait(pipeline.Command(
        "req-query-lost-target", kDispatchHostSession, kDispatchHostUser,
        ClientMeetingCommandKind::QUERY, meeting_id)));
    const MeetingAdapterCounters delta = Delta(before, pipeline.RealAdapter().Snapshot());

    // (a) 没有产出 —— 只有这一点由"目标不可解析"决定。
    EXPECT_EQ(pipeline.Responses().size(), static_cast<std::size_t>(1));
    EXPECT_EQ(delta.response_produced, static_cast<std::uint64_t>(0));
    EXPECT_EQ(delta.target_missing, static_cast<std::uint64_t>(1));
    EXPECT_EQ(delta.adapter_exception, static_cast<std::uint64_t>(0));

    // (b) 领域结果**没有**被改写：sink 记录的仍是领域真实结果 OK，
    //     并且领域状态没有被适配层的失败触碰。
    const std::vector<MeetingCommandOutcome> outcomes = pipeline.Sink().Outcomes();
    ASSERT_FALSE(outcomes.empty());
    EXPECT_EQ(outcomes.back().result_code, meeting::ResultCode::OK);
    EXPECT_EQ(outcomes.back().result_name, std::string("OK"));
    EXPECT_EQ(outcomes.back().disposition, meeting::ResultDisposition::SUCCESS);
    EXPECT_TRUE(pipeline.Dispatcher().HasActiveMeeting(meeting_id));
    EXPECT_FALSE(pipeline.Dispatcher().HasClosedSnapshot(meeting_id));
}

// ------------------------------------------------------------------------------
// SS5：目标不存在时不产生第二条 response，也不伪造新的业务失败结果。
// ------------------------------------------------------------------------------
TEST(DispatcherAdapterIntegration, SS5_MissingTargetProducesNoSecondOrFabricatedResponse) {
    DispatchPipeline pipeline;
    ASSERT_TRUE(pipeline.BootstrapHostAndCreate());
    const std::string meeting_id = pipeline.Dispatcher().LastCreatedMeetingId();

    pipeline.RemoveDirectoryEntry(kDispatchHostSession);

    ASSERT_TRUE(pipeline.EnqueueAndWait(pipeline.Command(
        "req-query-vanished", kDispatchHostSession, kDispatchHostUser,
        ClientMeetingCommandKind::QUERY, meeting_id)));

    // 恰好只有 create 那一条 response：既没有第二条，也没有"伪造的失败 response"。
    const std::vector<std::string> request_ids = ResponseRequestIds(pipeline.Responses());
    ASSERT_EQ(request_ids.size(), static_cast<std::size_t>(1));
    EXPECT_EQ(request_ids[0], std::string("req-create"));
    EXPECT_EQ(std::find(request_ids.begin(), request_ids.end(),
                        std::string("req-query-vanished")),
              request_ids.end());

    // 也没有把该请求"改写"成别的业务失败结果：领域侧记录的仍是真实 OK，
    // 适配层既没有产出、也没有伪造第二条。
    const std::vector<MeetingCommandOutcome> outcomes = pipeline.Sink().Outcomes();
    ASSERT_FALSE(outcomes.empty());
    EXPECT_EQ(outcomes.back().command, ClientMeetingCommandKind::QUERY);
    EXPECT_EQ(outcomes.back().result_code, meeting::ResultCode::OK);
    EXPECT_EQ(outcomes.back().result_name, std::string("OK"));
}

// ==============================================================================
// 组 7：顺序与生命周期（OR1–OR5）
// ==============================================================================

// ------------------------------------------------------------------------------
// OR1：response 的实际 Collect 顺序 == 入队顺序，request_id 一一对应。
//
// 同一批次里 CREATE 先于 QUERY/CLOSE，因此后两者使用注入来源确定的 meeting id；
// 用例先断言 CREATE 的 response 确实用了该 id（前提失效时立即失败，不会假通过）。
// ------------------------------------------------------------------------------
TEST(DispatcherAdapterIntegration, OR1_ResponseOrderMatchesEnqueueOrder) {
    DispatchPipeline pipeline;
    pipeline.AddDirectoryEntry(kDispatchHostSession);
    ASSERT_TRUE(pipeline.EnqueueAndWait(
        DispatchPipeline::BootstrapWork(kDispatchHostSession, kDispatchHostUser)));

    std::vector<LogicWorkItem> batch;
    batch.push_back(pipeline.Command("req-create", kDispatchHostSession,
                                     kDispatchHostUser, ClientMeetingCommandKind::CREATE,
                                     std::string()));
    batch.push_back(pipeline.Command("req-q1", kDispatchHostSession, kDispatchHostUser,
                                     ClientMeetingCommandKind::QUERY, kFirstMeetingId));
    batch.push_back(pipeline.Command("req-close", kDispatchHostSession,
                                     kDispatchHostUser, ClientMeetingCommandKind::CLOSE,
                                     kFirstMeetingId));
    ASSERT_TRUE(pipeline.EnqueueAndWait(batch));

    const std::vector<MeetingOutboundMessage> responses = pipeline.Responses();
    const std::vector<std::string> request_ids = ResponseRequestIds(responses);
    const std::vector<ClientMeetingCommandKind> commands = ResponseCommands(responses);

    ASSERT_EQ(request_ids.size(), static_cast<std::size_t>(3));
    EXPECT_EQ(request_ids[0], std::string("req-create"));
    EXPECT_EQ(request_ids[1], std::string("req-q1"));
    EXPECT_EQ(request_ids[2], std::string("req-close"));

    EXPECT_EQ(commands[0], ClientMeetingCommandKind::CREATE);
    EXPECT_EQ(commands[1], ClientMeetingCommandKind::QUERY);
    EXPECT_EQ(commands[2], ClientMeetingCommandKind::CLOSE);

    // correlation 与 target 一一对应（不是复用同一条消息）。
    for (std::size_t i = 0; i < responses.size(); ++i) {
        EXPECT_EQ(responses[i].response.response_target_session_id,
                  std::string(kDispatchHostSession));
    }
    EXPECT_EQ(responses[0].response.view.meeting_id, std::string(kFirstMeetingId));

    // FINALIZE_CLOSE 的响应式产物（通知）在所有 response 之后。
    EXPECT_EQ(pipeline.Notifications().size(), static_cast<std::size_t>(1));
}

// ------------------------------------------------------------------------------
// OR2：adapter 不改变唯一 FIFO 顺序；FINALIZE_CLOSE 仍按原条件加入**尾部**。
//
// 证据是 M4 §10.3 的"入队瞬间队列快照"（RecordInternalWorkEnqueued，在队列锁内
// 生成），而不是事后猜测。因此本用例对顺序的断言是**确定性**的。
// ------------------------------------------------------------------------------
TEST(DispatcherAdapterIntegration, OR2_FinalizeCloseStillQueuesBehindPendingWork) {
    DispatchPipeline pipeline;
    ASSERT_TRUE(pipeline.BootstrapHostAndCreate());
    const std::size_t responses_before = pipeline.Responses().size();

    // CLOSE 之后还排着一条 QUERY：FINALIZE_CLOSE 必须排在它**后面**。
    std::vector<LogicWorkItem> batch;
    batch.push_back(pipeline.Command("req-close", kDispatchHostSession,
                                     kDispatchHostUser, ClientMeetingCommandKind::CLOSE,
                                     kFirstMeetingId));
    batch.push_back(pipeline.Command("req-after-close", kDispatchHostSession,
                                     kDispatchHostUser, ClientMeetingCommandKind::QUERY,
                                     kFirstMeetingId));
    ASSERT_TRUE(pipeline.EnqueueAndWait(batch));

    // (a) 入队瞬间快照：队列内容恰好是 [待处理的 QUERY, FINALIZE_CLOSE]。
    const std::vector<LogicWorkKind> enqueued_kinds = pipeline.Sink().InternalEnqueueKinds();
    const std::vector<std::vector<LogicWorkKind> > queue_snapshots =
        pipeline.Sink().InternalEnqueueQueueKinds();
    ASSERT_EQ(enqueued_kinds.size(), static_cast<std::size_t>(1));   // 恰好一个 FINALIZE_CLOSE
    EXPECT_EQ(enqueued_kinds[0], LogicWorkKind::FINALIZE_CLOSE);
    ASSERT_EQ(queue_snapshots.size(), static_cast<std::size_t>(1));
    ASSERT_EQ(queue_snapshots[0].size(), static_cast<std::size_t>(2));
    EXPECT_EQ(queue_snapshots[0][0], LogicWorkKind::MEETING_COMMAND);   // 不插队
    EXPECT_EQ(queue_snapshots[0][1], LogicWorkKind::FINALIZE_CLOSE);    // 追加在尾部

    // (b) 可观测顺序同样成立：CLOSE 的 response → QUERY 的 response → 通知。
    const std::vector<MeetingOutboundMessage> responses = pipeline.Responses();
    ASSERT_EQ(responses.size(), responses_before + 2);
    const std::vector<std::string> request_ids = ResponseRequestIds(responses);
    EXPECT_EQ(request_ids[request_ids.size() - 2], std::string("req-close"));
    EXPECT_EQ(request_ids[request_ids.size() - 1], std::string("req-after-close"));
    EXPECT_EQ(pipeline.Notifications().size(), static_cast<std::size_t>(1));

    // 收敛已经发生：FINALIZE_CLOSE 虽然排在尾部，但确实被处理了。
    EXPECT_TRUE(pipeline.Dispatcher().HasClosedSnapshot(kFirstMeetingId));
}

// ------------------------------------------------------------------------------
// OR3：合法 Close 的 response 反映 turn 1 的 ENDING；FINALIZE_CLOSE 是 turn 2，
//      之后收敛为 CLOSED；通知只从**最终**领域结果生成。
// ------------------------------------------------------------------------------
TEST(DispatcherAdapterIntegration, OR3_CloseResponseShowsEndingThenTurnTwoCloses) {
    DispatchPipeline pipeline;
    ASSERT_TRUE(pipeline.BootstrapHostAndCreate());
    ASSERT_EQ(pipeline.Dispatcher().LastCreatedMeetingId(), std::string(kFirstMeetingId));

    ASSERT_TRUE(pipeline.EnqueueAndWait(pipeline.Command(
        "req-close", kDispatchHostSession, kDispatchHostUser,
        ClientMeetingCommandKind::CLOSE, kFirstMeetingId)));

    // turn 1：response 反映 ENDING（还没有收敛）。
    const std::vector<MeetingOutboundMessage> responses = pipeline.Responses();
    ASSERT_FALSE(responses.empty());
    const MeetingResponseMessage& close_response = responses.back().response;
    EXPECT_EQ(close_response.command, ClientMeetingCommandKind::CLOSE);
    EXPECT_EQ(close_response.request_id, std::string("req-close"));
    EXPECT_EQ(close_response.result_code, meeting::ResultCode::OK);
    EXPECT_EQ(close_response.disposition, meeting::ResultDisposition::SUCCESS);
    EXPECT_TRUE(close_response.has_view);
    EXPECT_EQ(close_response.view.meeting_state, meeting::MeetingState::ENDING);
    EXPECT_FALSE(close_response.view.has_closed_at);

    // turn 2 已完成：领域已收敛到 CLOSED。
    EXPECT_TRUE(pipeline.Dispatcher().HasClosedSnapshot(kFirstMeetingId));
    EXPECT_FALSE(pipeline.Dispatcher().HasActiveMeeting(kFirstMeetingId));

    // sink 记录的 FinalizeClose：finalized == true，且通知意图来自冻结上下文。
    const std::vector<std::pair<bool, bool> > finalize_flags =
        pipeline.Sink().FinalizeFlags();
    ASSERT_EQ(finalize_flags.size(), static_cast<std::size_t>(1));
    EXPECT_TRUE(finalize_flags[0].first);    // finalized
    EXPECT_FALSE(finalize_flags[0].second);  // not already_finalized
    const std::vector<std::vector<meeting::SessionId> > finalize_targets =
        pipeline.Sink().FinalizeTargets();
    ASSERT_EQ(finalize_targets.size(), static_cast<std::size_t>(1));
    ASSERT_EQ(finalize_targets[0].size(), static_cast<std::size_t>(1));
    EXPECT_EQ(finalize_targets[0][0], std::string(kDispatchHostSession));

    // 通知**只**来自最终领域结果：其 event_id 必须等于 turn 2 产生的
    // MEETING_CLOSED 事件 id，状态投影必须是 CLOSED。
    const std::vector<LogicWorkKind> dispatched_kinds = pipeline.Sink().DispatchedKinds();
    const std::vector<std::vector<WorkDomainEvent> > dispatched_events =
        pipeline.Sink().DispatchedEvents();
    ASSERT_EQ(dispatched_kinds.size(), dispatched_events.size());
    std::size_t finalize_index = dispatched_kinds.size();
    for (std::size_t i = 0; i < dispatched_kinds.size(); ++i) {
        if (dispatched_kinds[i] == LogicWorkKind::FINALIZE_CLOSE) {
            finalize_index = i;
            break;
        }
    }
    ASSERT_LT(finalize_index, dispatched_kinds.size());
    ASSERT_EQ(dispatched_events[finalize_index].size(), static_cast<std::size_t>(1));
    const WorkDomainEvent& closed_event = dispatched_events[finalize_index][0];
    EXPECT_EQ(closed_event.type, meeting::EventType::MEETING_CLOSED);
    EXPECT_EQ(closed_event.meeting_state_after, meeting::MeetingState::CLOSED);
    EXPECT_EQ(closed_event.meeting_id, std::string(kFirstMeetingId));

    const std::vector<MeetingOutboundMessage> notifications = pipeline.Notifications();
    ASSERT_EQ(notifications.size(), static_cast<std::size_t>(1));
    EXPECT_EQ(notifications[0].notification.notification_type,
              meeting::EventType::MEETING_CLOSED);
    EXPECT_EQ(notifications[0].notification.meeting_id, std::string(kFirstMeetingId));
    EXPECT_EQ(notifications[0].notification.notification_target_session_id,
              std::string(kDispatchHostSession));
    EXPECT_TRUE(notifications[0].notification.has_event_id);
    EXPECT_EQ(notifications[0].notification.event_id, closed_event.event_id);
    EXPECT_TRUE(notifications[0].notification.has_correlation_request_id);
    EXPECT_EQ(notifications[0].notification.correlation_request_id,
              std::string("req-close"));

    // 通知在 CLOSE 的 response **之后**产出（同一 turn 内的下游产物，不倒序）。
    // 两者由不同的容器记录，这里用"通知存在且 turn 1 的 response 仍是最后一条
    // response"来确认它们不是同一条消息、也没有互相替代。
    EXPECT_EQ(pipeline.Responses().size(), responses.size());
}

// ------------------------------------------------------------------------------
// OR4：SerializedWorkExecutor::StopAndJoin 的幂等性与"停止后拒绝新外部 work"。
//
// 注意范围：文档 OR4 的正式目标是 LogicSystem::ShutdownWorkExecutor（M6.6）。
// 本轮只能核实**它将来会调用**的底层执行器原语，不做任何生产装配改动。
// ------------------------------------------------------------------------------
TEST(DispatcherAdapterIntegration, OR4_StopAndJoinIsIdempotentAndRejectsNewWork) {
    DispatchPipeline pipeline;
    ASSERT_TRUE(pipeline.BootstrapHostAndCreate());
    ASSERT_TRUE(pipeline.Dispatcher().HasActiveMeeting(kFirstMeetingId));

    pipeline.Executor().StopAndJoin();
    pipeline.Executor().StopAndJoin();   // 幂等：不崩溃、不重复 join

    // stop 之后外部 work 一律被拒绝（Push 的 stopping_ 闸门），且不被处理。
    const std::size_t responses_before = pipeline.Responses().size();
    EXPECT_FALSE(pipeline.Executor().PostMeetingCommand(
        pipeline.Command("req-after-stop", kDispatchHostSession, kDispatchHostUser,
                         ClientMeetingCommandKind::QUERY, kFirstMeetingId)
            .meeting_context,
        ClientMeetingCommandKind::QUERY, kFirstMeetingId));
    EXPECT_EQ(pipeline.Responses().size(), responses_before);

    // 领域状态仍可只读访问（执行器停止不等于领域被销毁）。
    EXPECT_TRUE(pipeline.Dispatcher().HasActiveMeeting(kFirstMeetingId));
}

// ------------------------------------------------------------------------------
// OR5（契约级）：目录的强/弱引用契约在 dispatcher 装配下仍然成立。
//
// 真实 ChatServer 关闭过程中"目录仍然可用"的顺序属于 M6.6 装配 + M6.7 runtime
// smoke，**本轮 NOT VERIFIED**。这里只证明 adapter 所依赖的既有契约：
// 目录最后一个强引用释放后 weak 立即失效，此后出站只 no-op。
// ------------------------------------------------------------------------------
TEST(DispatcherAdapterIntegration, OR5_DirectoryStrongWeakContractUnderDispatcher) {
    DispatchPipeline pipeline;
    ASSERT_TRUE(pipeline.BootstrapHostAndCreate());
    ASSERT_EQ(pipeline.Responses().size(), static_cast<std::size_t>(1));

    // 测试独占目录的唯一强引用 ⇒ reset 之后 weak 必然失效（§8.4）。
    pipeline.ReleaseDirectoryStrongRefs();

    const MeetingAdapterCounters before = pipeline.RealAdapter().Snapshot();
    ASSERT_TRUE(pipeline.EnqueueAndWait(pipeline.Command(
        "req-query-no-directory", kDispatchHostSession, kDispatchHostUser,
        ClientMeetingCommandKind::QUERY, kFirstMeetingId)));
    const MeetingAdapterCounters delta = Delta(before, pipeline.RealAdapter().Snapshot());

    // 整次调用 no-op：不崩溃、不产出、只计一次 directory_unavailable。
    EXPECT_EQ(delta.directory_unavailable, static_cast<std::uint64_t>(1));
    EXPECT_EQ(delta.response_produced, static_cast<std::uint64_t>(0));
    EXPECT_EQ(delta.target_resolved, static_cast<std::uint64_t>(0));
    EXPECT_EQ(delta.target_missing, static_cast<std::uint64_t>(0));
    EXPECT_EQ(pipeline.Responses().size(), static_cast<std::size_t>(1));

    // 领域结果不受影响（sink 记录仍是 OK）。
    const std::vector<MeetingCommandOutcome> outcomes = pipeline.Sink().Outcomes();
    ASSERT_FALSE(outcomes.empty());
    EXPECT_EQ(outcomes.back().result_code, meeting::ResultCode::OK);
}

// ==============================================================================
// EX1：adapter 违约抛异常时，唯一消费者仍然存活，领域结果不回滚，Close 仍然收敛。
// ==============================================================================

// ------------------------------------------------------------------------------
// 为什么必须有这一层守卫（§13.4 / R-7）：
//   SerializedWorkExecutor::WorkerLoop 没有通用 try/catch，异常从 DispatchWork
//   逃逸就会穿出 std::thread 的入口函数 ⇒ std::terminate（整进程终止），
//   并且 in_flight_ 不再归零。生产 adapter 是 noexcept 实现，但**接口不是**，
//   因此 dispatcher 必须在调用边界上兜住违约实现。
//
// 本用例用"七项 On* 全抛"的替身证明：每个调用点都被守住、计数可断言、
// 后续 work 继续被处理、CLOSE 派生的 FINALIZE_CLOSE 仍然入队并完成收敛。
// ------------------------------------------------------------------------------
TEST(DispatcherAdapterIntegration, EX1_AdapterViolationDoesNotKillTheConsumer) {
    DispatchPipeline pipeline(DispatchPipeline::kThrowingAdapter);
    pipeline.AddDirectoryEntry(kDispatchHostSession);
    ASSERT_TRUE(pipeline.EnqueueAndWait(
        DispatchPipeline::BootstrapWork(kDispatchHostSession, kDispatchHostUser)));

    // CREATE：adapter 抛异常，但领域 mutation 已完成、消费者仍然活着。
    ASSERT_TRUE(pipeline.EnqueueAndWait(pipeline.Command(
        "req-create", kDispatchHostSession, kDispatchHostUser,
        ClientMeetingCommandKind::CREATE, std::string())));
    EXPECT_EQ(pipeline.Dispatcher().LastCreatedMeetingId(), std::string(kFirstMeetingId));
    EXPECT_EQ(pipeline.Dispatcher().AdapterExceptionCount(), static_cast<std::size_t>(1));
    EXPECT_EQ(pipeline.ThrowingTestAdapter().Calls(), static_cast<std::uint64_t>(1));

    // QUERY：消费者继续处理后续 work（若线程已死，这里会等待超时 / 进程终止）。
    ASSERT_TRUE(pipeline.EnqueueAndWait(pipeline.Command(
        "req-query", kDispatchHostSession, kDispatchHostUser,
        ClientMeetingCommandKind::QUERY, kFirstMeetingId)));
    EXPECT_EQ(pipeline.Dispatcher().AdapterExceptionCount(), static_cast<std::size_t>(2));
    EXPECT_TRUE(pipeline.Dispatcher().HasActiveMeeting(kFirstMeetingId));

    // CLOSE：adapter 违约**不阻止** FINALIZE_CLOSE 入队，两 turn 仍然收敛。
    //
    // 等待到静止时，同一批次内已经跑完两个 turn：turn 1 的 OnCloseMeeting 与
    // turn 2 的 OnFinalizeClose 各违约一次，因此计数从 2 变成 4。
    ASSERT_TRUE(pipeline.EnqueueAndWait(pipeline.Command(
        "req-close", kDispatchHostSession, kDispatchHostUser,
        ClientMeetingCommandKind::CLOSE, kFirstMeetingId)));
    EXPECT_EQ(pipeline.Dispatcher().AdapterExceptionCount(), static_cast<std::size_t>(4));
    EXPECT_EQ(pipeline.ThrowingTestAdapter().Calls(), static_cast<std::uint64_t>(4));

    // 收敛没有被 adapter 的违约影响（领域效果在 adapter 之前已经落地）。
    EXPECT_TRUE(pipeline.Dispatcher().HasClosedSnapshot(kFirstMeetingId));
    EXPECT_FALSE(pipeline.Dispatcher().HasActiveMeeting(kFirstMeetingId));

    // sink 观测到的 finalize 结果仍是真实的 finalized == true。
    const std::vector<std::pair<bool, bool> > finalize_flags =
        pipeline.Sink().FinalizeFlags();
    ASSERT_EQ(finalize_flags.size(), static_cast<std::size_t>(1));
    EXPECT_TRUE(finalize_flags[0].first);
}

// ==============================================================================
// ORD：调用点顺序的静态核对（§5 / §6.5 的顺序硬约束）
//
// 顺序要求（对每个调用点都必须成立）：
//   1. 领域 mutation 已经返回结果（result 是 const 引用 ⇒ 结构上不可能先调用）
//   2. 既有 sink_ 观测先完成（原有观测顺序不得被 adapter 插入或改变）
//   3. adapter 调用在 sink 之后
//   4. DispatchClose：adapter 在**入队判断之前**
//
// 这一条用真实源码的函数窗口做静态核对：顺序是代码结构属性，
// 用文本位置即可判定，且能直接抓住"把 adapter 挪到 sink 之前/入队之后"的回归。
// ==============================================================================
TEST(DispatcherAdapterIntegration, ORD_CallSitesKeepSinkBeforeAdapter) {
    const std::string code = StripComments(ReadSourceFile(kDispatcherSourcePath));
    ASSERT_FALSE(code.empty()) << "cannot read " << kDispatcherSourcePath;

    struct Expectation {
        const char* function_marker;
        const char* sink_marker;
        const char* adapter_marker;
    };
    const Expectation expectations[] = {
        { "LogicWorkDispatcher::DispatchCreate", "sink_->RecordMeetingCommand",
          "adapter_->OnCreateMeeting" },
        { "LogicWorkDispatcher::DispatchJoin", "sink_->RecordMeetingCommand",
          "adapter_->OnJoinMeeting" },
        { "LogicWorkDispatcher::DispatchLeave", "sink_->RecordMeetingCommand",
          "adapter_->OnLeaveMeeting" },
        { "LogicWorkDispatcher::DispatchClose", "sink_->RecordMeetingCommand",
          "adapter_->OnCloseMeeting" },
        { "LogicWorkDispatcher::DispatchQuery", "sink_->RecordMeetingCommand",
          "adapter_->OnQueryMeeting" },
        { "LogicWorkDispatcher::DispatchListParticipants", "sink_->RecordMeetingCommand",
          "adapter_->OnListParticipants" },
    };

    for (std::size_t i = 0; i < sizeof(expectations) / sizeof(expectations[0]); ++i) {
        const std::string window = FunctionWindow(code, expectations[i].function_marker);
        ASSERT_FALSE(window.empty()) << "missing function: " << expectations[i].function_marker;

        const std::size_t sink_pos = window.find(expectations[i].sink_marker);
        const std::size_t adapter_pos = window.find(expectations[i].adapter_marker);
        ASSERT_NE(sink_pos, std::string::npos)
            << "no sink record in " << expectations[i].function_marker;
        ASSERT_NE(adapter_pos, std::string::npos)
            << "no adapter call in " << expectations[i].function_marker;
        EXPECT_LT(sink_pos, adapter_pos)
            << "adapter must be called after the sink record in "
            << expectations[i].function_marker;

        // adapter 调用必须存在"非空才调用"的守卫，否则 nullptr 注入会崩。
        EXPECT_NE(window.find("if (adapter_ != nullptr)"), std::string::npos)
            << "missing null guard in " << expectations[i].function_marker;
        // 且必须被异常守卫包住（抽象接口可能违约）。
        EXPECT_NE(window.find("InvokeAdapterGuarded"), std::string::npos)
            << "missing exception guard in " << expectations[i].function_marker;
    }

    // DispatchClose：adapter 必须在 FINALIZE_CLOSE 入队**之前**。
    const std::string close_window =
        FunctionWindow(code, "LogicWorkDispatcher::DispatchClose");
    ASSERT_FALSE(close_window.empty());
    const std::size_t close_adapter = close_window.find("adapter_->OnCloseMeeting");
    const std::size_t close_enqueue = close_window.find("PostInternalWork(MakeFinalizeCloseWork");
    ASSERT_NE(close_adapter, std::string::npos);
    ASSERT_NE(close_enqueue, std::string::npos);
    EXPECT_LT(close_adapter, close_enqueue)
        << "adapter must run before the derived FINALIZE_CLOSE is enqueued";

    // FINALIZE_CLOSE 分支：RecordFinalizeClose 在 OnFinalizeClose 之前。
    const std::size_t finalize_sink = code.find("sink_->RecordFinalizeClose");
    const std::size_t finalize_adapter = code.find("adapter_->OnFinalizeClose");
    ASSERT_NE(finalize_sink, std::string::npos);
    ASSERT_NE(finalize_adapter, std::string::npos);
    EXPECT_LT(finalize_sink, finalize_adapter)
        << "adapter must be called after RecordFinalizeClose";

    // 七个调用点，不多不少（不得出现第八个业务回调）。
    EXPECT_EQ(CountOccurrences(code, "adapter_->On"), static_cast<std::size_t>(7));
    // 每个调用点都经由守卫（按**调用形式**计数，避免把模板定义本身算进去）。
    EXPECT_EQ(CountOccurrences(code, "InvokeAdapterGuarded(adapter_exception_count_"),
              static_cast<std::size_t>(7));
    EXPECT_EQ(CountOccurrences(code, "void InvokeAdapterGuarded("),
              static_cast<std::size_t>(1));
}
