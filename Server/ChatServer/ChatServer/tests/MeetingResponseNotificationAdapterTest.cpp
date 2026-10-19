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
// ==============================================================================

#include <gtest/gtest.h>

#include <chrono>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>

#include "MeetingRequestContextBuilder.h"
#include "meeting/MeetingService.h"

// M6.3：真实生产类型。这里只用它们做**编译期**断言，不构造、不连接、不发送。
// 这两个头引入了 boost/asio / boost/beast 的**头文件**依赖（与 ChatServer 生产
// 构建相同的系统 Boost），但不引入任何链接依赖：本 TU 不实例化 CServer /
// CSession，也不取它们的非 inline 成员函数地址（全部使用 decltype，未求值）。
#include "CServer.h"
#include "CSession.h"
#include "MeetingOutboundChannel.h"

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
