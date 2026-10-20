#include <iostream>
#include "LogicSystem.h"
#include <csignal>
#include <condition_variable>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include "AsioIOServicePool.h"
#include "CServer.h"
#include "ConfigMgr.h"
#include "RedisMgr.h"
#include "ChatServiceImpl.h"
// M6.6：SetMeetingSessionDirectory 的实参类型（CServer 实现该接口）。
#include "MeetingOutboundChannel.h"

bool bstop = false;
std::condition_variable cond_quit;
std::mutex mutex_quit;

// ==============================================================================
// M6.6：进程级关闭协调器（§12.3.5 / §12.3.6 / §12.3.17 / §12.3.18）
//
// 它解决的问题不是"优雅"（M6 不声称无条件优雅），而是**责任归属**：
//   · 从第一个后台线程启动那一刻起，就必须有一个对象对"停止并 join 它"负责；
//   · 关键步骤失败时，绝不能"记录日志然后继续"，否则会在一堆仍未 join 的线程
//     还在运行时去销毁它们访问的对象（UB / std::terminate）。
//
// 三个结构性决定：
//   1. **空依赖构造 + 逐项 noexcept 登记**：协调器在阶段 1（任何 GetInstance
//      之前）以空依赖构造，之后每取得一项资源就立即登记 ⇒ "资源已启动但无人
//      负责清理"的窗口在结构上为零（§12.3.5.3）。
//   2. **K/B 分级**：K 级失败 fail-stop；B 级失败只记录并继续（§12.3.5.1）。
//   3. **Cleanup 幂等**（done_ 一次闸门）：可显式调用，析构时兜底再调一次是 no-op。
//
// 明确不在本类范围内的（M6 不声称覆盖）：
//   · 单例 GetInstance() **构造期内部**抛异常（Class A：对象尚未诞生，无法登记）；
//   · 任意 OS / 内核 / DNS 级永久阻塞；
//   · "进程在固定时间内完成优雅关闭"这一无条件命题。
// ==============================================================================
namespace {

class ShutdownCoordinator {
public:
    // 空依赖构造：此时 pool/redis/grpc_server/tcp_server/logic 全部为空。
    // 本调用不启动任何线程，因此可以在任何后台线程之前安全地建立。
    //
    // ⚠ 刻意**不加 noexcept**：构造函数要拷贝 std::string（可能 bad_alloc）。
    //   此时没有任何后台线程与已登记资源，抛异常只会导致"什么都没启动就退出"，
    //   是安全且可诊断的；若声明 noexcept 则会直接 std::terminate。
    explicit ShutdownCoordinator(const std::string& server_name)
        : grpc_server_(),
          grpc_thread_(),
          pool_(),
          redis_(),
          logic_(nullptr),
          tcp_server_(),
          server_name_(server_name),
          done_(false),
          registration_confirmed_(false) {
    }

    ShutdownCoordinator(const ShutdownCoordinator&) = delete;
    ShutdownCoordinator& operator=(const ShutdownCoordinator&) = delete;

    // ---- 责任登记：全部 noexcept -------------------------------------------
    // shared_ptr 的拷贝赋值、unique_ptr 的移动赋值、裸指针赋值、bool 写入
    // 都是 noexcept 且不分配内存 ⇒ "取得资源"与"登记责任"之间没有可抛点。
    void SetPool(std::shared_ptr<AsioIOServicePool> pool) noexcept { pool_ = pool; }
    void SetRedis(std::shared_ptr<RedisMgr> redis) noexcept { redis_ = redis; }

    // gRPC Server 由协调器**独占**持有：BuildAndStart() 的真实返回类型就是
    // std::unique_ptr<grpc::Server>，因此这里是 noexcept 移动，不经过
    // "从裸指针构造 shared_ptr"（那会分配控制块，是一个可抛点）。
    void SetGrpcServer(std::unique_ptr<grpc::Server> server) noexcept {
        grpc_server_ = std::move(server);
    }

    // LogicSystem 是 Singleton：只登记裸指针，不拥有。
    void SetLogicSystem(LogicSystem* logic) noexcept { logic_ = logic; }

    // 方案 B：强引用。SetTcpServer() 成功之后，协调器在整个 Cleanup() 期间
    // 保证 tcp_server 存活 ⇒ drain 期间 Session Directory 仍然可 lookup。
    void SetTcpServer(std::shared_ptr<CServer> tcp_server) noexcept {
        tcp_server_ = tcp_server;
    }

    // 仅当启动 HSet 判真之后调用；决定 Cleanup 的 4a（HDel）是否执行。
    void MarkRegistrationConfirmed() noexcept { registration_confirmed_ = true; }

    // 启动 gRPC Wait 线程（阶段 9，全部可失败初始化之后）。
    //
    // 若此处抛 std::system_error，线程仍不可 join ⇒ 没有 terminate 风险。
    // 一次性语义：绝不对一个仍 joinable 的 std::thread 做覆盖式移动赋值。
    void StartGrpcThread() {
        if (grpc_thread_.joinable()) {
            throw std::logic_error("StartGrpcThread() must be called exactly once");
        }
        grpc_thread_ = std::thread([this]() {
            if (grpc_server_) {
                grpc_server_->Wait();
            }
        });
    }

    ~ShutdownCoordinator() { Cleanup(); }

    // 清理：幂等（done_ 一次闸门）、noexcept（K 级失败走 fail-stop，不返回）。
    //
    // 正常返回 ⇒ 五个 K 级步骤全部成功完成。
    void Cleanup() noexcept {
        if (done_) {
            return;
        }
        // 先置位再执行：K 级失败会 fail-stop（进程不返回），因此不存在
        // "第一次做到一半、第二次从头再来"的重复执行。
        done_ = true;

        // ============ K 级：任何失败都 fail-stop；均做空指针检查 ==============
        // 1a）先停 gRPC：同步 RPC handler 可能仍在执行，它们仍会访问 Redis。
        Fatal("1a grpc_server->Shutdown", [this]() {
            if (grpc_server_) {
                grpc_server_->Shutdown();
            }
        });

        // 1b）再停 pool：它是**唯一**的 pool->Stop() 调用点（§12.3.5.1 / A16）。
        //     Stop() 非幂等（内部 join 线程），因此绝不重试；失败即 fail-stop。
        Fatal("1b pool->Stop", [this]() {
            if (pool_) {
                pool_->Stop();
            }
        });

        // 2）停止并排空唯一 executor：此时 1a/1b 已完成 ⇒ 不存在外部生产者。
        //    Close 派生的 FINALIZE_CLOSE 在 drain 期间仍会被消费（队列层面保证）。
        Fatal("2 ShutdownWorkExecutor", [this]() {
            if (logic_ != nullptr) {
                logic_->ShutdownWorkExecutor();
            }
        });

        // ============ B 级：best-effort，失败只记录并继续 ======================
        // 3）清除目录弱引用：**必须在 drain 之后**，否则 FINALIZE_CLOSE 无法 lookup。
        BestEffort("3 ClearMeetingSessionDirectory", [this]() {
            if (logic_ != nullptr) {
                logic_->ClearMeetingSessionDirectory();
            }
        });

        // 4a）Redis 启动注册的清理（有业务语义的有界 best-effort，不是纯诊断）。
        BestEffort("4a redis HDel", [this]() {
            if (!redis_ || !registration_confirmed_) {
                std::cerr << "shutdown diagnostic: redis HDel skipped"
                             " (startup registration was never confirmed)"
                          << std::endl;
                return;
            }
            // HDel 的 bool 契约无法区分原因：(i) 无可用连接 (ii) 命令执行失败
            // (iii) 回复不是 INTEGER (iv) 字段不存在/未删除任何字段。
            // ⇒ 只输出**中性**诊断：既不断言字段一定不存在，也不断言连接一定失败。
            if (!redis_->HDel(LOGIN_COUNT, server_name_)) {
                std::cerr << "shutdown diagnostic: redis HDel not confirmed deleted"
                             " (cause unknown: no connection / command error /"
                             " error reply / field absent or not removed);"
                             " continuing shutdown"
                          << std::endl;
            }
        });

        // ============ 回到 K 级 ================================================
        // 4b）关闭 Redis 连接池：必须能停止并 join 健康检查线程（有界，§12.3.16）。
        Fatal("4b redis Close", [this]() {
            if (redis_) {
                redis_->Close();
            }
        });

        // 5）最后 join gRPC Wait 线程：它是最后启动、也最后停止的那个线程。
        Fatal("5 grpc_thread join", [this]() {
            if (grpc_thread_.joinable()) {
                grpc_thread_.join();
            }
        });
    }

private:
    // K 级步骤包装：异常 ⇒ fail-stop（不返回、不继续后续步骤）。
    template <typename Fn>
    void Fatal(const char* name, Fn fn) noexcept {
        try {
            fn();
        } catch (const std::exception& e) {
            FailStop(name, e.what());
        } catch (...) {
            FailStop(name, "unknown exception");
        }
    }

    // B 级步骤包装：异常 ⇒ 非致命诊断，然后继续下一步。
    template <typename Fn>
    void BestEffort(const char* name, Fn fn) noexcept {
        try {
            fn();
        } catch (const std::exception& e) {
            std::cerr << "shutdown best-effort step failed: " << name << ": "
                      << e.what() << std::endl;
        } catch (...) {
            std::cerr << "shutdown best-effort step failed: " << name
                      << ": unknown" << std::endl;
        }
    }

    // fail-stop：检测到不可恢复的关闭失败 ⇒ 主动终止进程。
    //
    // 诚实的声明边界：此时**不能**声称这是一次优雅关闭，也**不能**声称所有线程
    // 都已 join（恰恰是"没能停住/join 住"才走到这里）。唯一的正确声明是：
    // 为避免在仍未停止的线程之上析构对象（UB / std::terminate），进程主动终止。
    void FailStop(const char* name, const char* what) noexcept {
        std::cerr << "FATAL: unrecoverable shutdown failure at step [" << name
                  << "]: " << what << std::endl;
        std::cerr << "FATAL: cannot guarantee that all threads are joined; "
                     "terminating to avoid destroying objects still in use."
                  << std::endl;
        std::cerr << "FATAL: Redis cleanup may be incomplete; this is NOT a "
                     "graceful shutdown."
                  << std::endl;
        std::abort();
    }

    // 成员声明顺序决定逆序析构顺序：
    //   tcp_server_ → logic_(裸)、redis_ → pool_ → grpc_thread_ → grpc_server_
    // 协调器自身声明在 io_context 之后 ⇒ 比 io_context 更早析构 ⇒
    // tcp_server 在协调器析构体内销毁时，CServer 引用的 io_context 仍然存活。
    std::unique_ptr<grpc::Server> grpc_server_;   // 独占
    std::thread grpc_thread_;                     // 按值持有
    std::shared_ptr<AsioIOServicePool> pool_;     // 强引用
    std::shared_ptr<RedisMgr> redis_;             // 强引用
    LogicSystem* logic_;                          // 不拥有（Singleton）
    std::shared_ptr<CServer> tcp_server_;         // 强引用（方案 B）
    std::string server_name_;
    bool done_;
    bool registration_confirmed_;                 // 4a 的门（§12.3.18.2）
};

}  // namespace

int main()
{
    auto& cfg = ConfigMgr::Inst();
    auto server_name = cfg["SelfServer"]["Name"];
    try {
        // ---- 阶段 0：仅构造，不启动任何后台线程 -------------------------------
        // io_context 必须**先于**协调器声明 ⇒ 比协调器更晚析构 ⇒
        // 协调器析构体内销毁 tcp_server 时，CServer 的 io_context& 仍然有效。
        boost::asio::io_context io_context;
        // service 必须先于协调器声明 ⇒ 比 grpc_server 更晚析构
        // （builder.RegisterService(&service)：handler 使用它）。
        ChatServiceImpl service;
        // ⚠ M6.6（D-R20-04）：signal_set **不在这里**构造。
        //   否则 Redis 初始化阻塞期间由 Asio 接管 SIGTERM，进程无法被正常终止。

        // ---- 阶段 1：关闭协调器就位（空依赖；**任何后台线程之前**）------------
        ShutdownCoordinator shutdown(server_name);

        // ---- 阶段 1.5：信号持有者只声明、不构造 -------------------------------
        // 声明在协调器**之后** ⇒ 失败路径上先于协调器析构 ⇒
        // 协调器执行 Cleanup() 时信号已恢复默认处置（§12.3.17.3 规则 4）。
        std::unique_ptr<boost::asio::signal_set> signals;

        // ---- 阶段 2：pool（构造即启动 _threads）→ 立即登记 --------------------
        auto pool = AsioIOServicePool::GetInstance();
        shutdown.SetPool(pool);

        // ---- 阶段 3：Redis（构造即启动 check_thread_）→ 立即登记 --------------
        auto redis = RedisMgr::GetInstance();
        shutdown.SetRedis(redis);

        // ---- 阶段 4：启动注册（**硬依赖，必须判真**）--------------------------
        // false 只表示"未确认成功"（可能命令未发出、也可能已执行但回复超时），
        // 但无论如何都不得开放服务。此时 registration_confirmed_ 保持 false，
        // Cleanup 会跳过 4a ⇒ 可能残留一个实际已写入的 field（已如实记录）。
        if (!redis->HSet(LOGIN_COUNT, server_name, "0")) {
            throw std::runtime_error(
                "redis HSet(LOGIN_COUNT) failed; refusing to open services");
        }
        shutdown.MarkRegistrationConfirmed();

        // ---- 阶段 5：gRPC Server（BuildAndStart 起即开始服务）→ 立即登记 ------
        std::string server_address(
            cfg["SelfServer"]["Host"] + ":" + cfg["SelfServer"]["RPCPort"]);
        grpc::ServerBuilder builder;
        builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
        builder.RegisterService(&service);

        // BuildAndStart() 的真实返回类型就是 std::unique_ptr<grpc::Server>
        // （失败时为空 unique_ptr）⇒ 不存在"从裸指针再包一层"的转换。
        auto grpc_server = builder.BuildAndStart();
        if (!grpc_server) {
            throw std::runtime_error(
                "grpc BuildAndStart() returned nullptr; aborting startup");
        }
        // 所有权经 noexcept 移动交给协调器 ⇒ 交接窗口为零。
        shutdown.SetGrpcServer(std::move(grpc_server));
        std::cout << "RPC Server listening on " << server_address << std::endl;

        // ---- 阶段 6：TCP Server（需要 io_context 存活）→ 立即登记 -------------
        // 必须用 make_shared：weak_ptr Session Directory 方案需要一个既有控制块，
        // 而栈对象没有控制块（§12.4）。
        auto port_str = cfg["SelfServer"]["Port"];
        auto tcp_server = std::make_shared<CServer>(io_context, atoi(port_str.c_str()));
        shutdown.SetTcpServer(tcp_server);

        // ---- 阶段 7：先登记 LogicSystem，再注册目录 ---------------------------
        // 顺序不可交换：Cleanup 的步骤 2/3 都以 logic_ 为前提，而
        // LogicSystem::GetInstance() 本身可能抛 ⇒ 它必须尽早完成登记。
        // SetMeetingSessionDirectory 是 noexcept 契约（§12.3.5.3）。
        //
        // 时序要求（§12.5 / R-3）：本注册发生在 io_context.run() 之前，
        // 而唯一 worker 只在首个 work item 入队时才惰性启动（accept 之后），
        // 因此 adapter 在构造时一定能拷到已经写入的目录弱引用。
        LogicSystem* logic = LogicSystem::GetInstance().get();
        shutdown.SetLogicSystem(logic);
        logic->SetMeetingSessionDirectory(tcp_server);

        // ---- 阶段 8：此时才让 Asio 接管信号 -----------------------------------
        // 信号回调**只**允许唤醒 io_context：pool->Stop() 非幂等，关闭顺序必须由
        // 唯一的 Cleanup() 统一控制（A16 / A25）。
        signals = std::make_unique<boost::asio::signal_set>(io_context, SIGINT, SIGTERM);
        signals->async_wait([&io_context](auto, auto) {
            io_context.stop();
        });

        // ---- 阶段 9：最后启动 gRPC Wait 线程 ----------------------------------
        shutdown.StartGrpcThread();

        // ---- 阶段 10：运行与清理 ----------------------------------------------
        try {
            io_context.run();
        }
        catch (...) {
            // run() 抛异常：先恢复默认信号处置，再显式清理，然后继续上抛。
            // 此时 tcp_server 局部仍在其作用域内，协调器另持一份强引用 ⇒ 都存活。
            signals.reset();
            shutdown.Cleanup();
            throw;
        }
        // 正常返回：同样先 reset signals（避免 io_context 已 stop 时吞掉后续 SIGTERM），
        // 再显式清理。Cleanup() 幂等，析构时的兜底调用为 no-op。
        signals.reset();
        shutdown.Cleanup();
    }
    catch (const std::exception& e) {
        // 初始化阶段的任何失败都由协调器析构兜底完成清理（done_ 为 false，
        // 因此析构会执行真实清理），并在栈展开之后到达这里。
        std::cerr << "Exception: " << e.what() << std::endl;
        // M6.7.1：清理**已经完成**，但这是一次失败启动 / 失败运行，因此必须以
        // 非零退出码报告给操作系统（此前会落到下面的 return 0，使"拒绝启动"与
        // "run() 抛异常"被误报为成功退出）。
        //   · 正常路径不会到达这里，仍然返回 0；
        //   · K 级失败走 FailStop() 的 std::abort()，那是信号终止，不受本行影响；
        //   · 不涉及任何清理顺序、K/B 划分、HDel 语义或信号处理。
        return EXIT_FAILURE;
    }
    return 0;
}
