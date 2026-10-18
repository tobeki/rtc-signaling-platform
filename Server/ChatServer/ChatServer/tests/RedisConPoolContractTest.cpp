// M6.6a / R-20: RedisConPool contract tests.
//
// RedisConPool lives in RedisMgr.h and depends on hiredis only (it does not use
// ConfigMgr), so this executable is deliberately separate from both the
// ChatServer binary and ChatServerUnitTests:
//   * ChatServerUnitTests must stay Redis-free / dependency-free;
//   * RedisMgr.cpp would pull in ConfigMgr + Boost.Filesystem + config.ini.
//
// No real Redis server is required: a tiny in-process RESP peer is used. The
// peer is test infrastructure; it adds no production thread and no production
// dependency.
//
// Scope (documented in section 12.3.16 of the M6 document):
//   1. empty pool acquire returns nullptr after the bounded wait;
//   2. Close() wakes a blocked acquirer;
//   3. normal AUTH + acquire + handoff works;
//   4. AUTH that never answers fails by I/O timeout (and must not dereference a
//      null reply);
//   5. a connection whose command times out can be discarded and is never
//      handed out again as a healthy connection;
//   6. Close() is idempotent and the destructor joins the health thread;
//   7. a connection returned after Close() is released (not silently dropped).
//
// Not covered here (see the M6.6a completion report):
//   * the 60-second health-check / reconnect cycle and its stop check: the
//     threshold is a private constant and waiting 60 s in a unit test is not
//     acceptable, so that path is verified by static review and by the later
//     ChatServer runtime smoke (M6.6/M6.7);
//   * RedisMgr's HSet/HDel wrappers: they need ConfigMgr + config.ini, so their
//     call-site behaviour is covered by the ChatServer runtime tests instead.

#include <gtest/gtest.h>

#include <hiredis.h>

#include "RedisMgr.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

long long ElapsedMs(Clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               Clock::now() - start)
        .count();
}

bool ReadExact(int fd, char* data, size_t len) {
    size_t offset = 0;
    while (offset < len) {
        const ssize_t n = ::recv(fd, data + offset, len - offset, 0);
        if (n == 0) {
            return false;
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        offset += static_cast<size_t>(n);
    }
    return true;
}

bool ReadLine(int fd, std::string& line) {
    line.clear();
    char c = '\0';
    while (true) {
        const ssize_t n = ::recv(fd, &c, 1, 0);
        if (n == 0) {
            return false;
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (c == '\n') {
            return true;
        }
        if (c != '\r') {
            line.push_back(c);
        }
    }
}

// Parses one RESP array-of-bulk-strings command (the only form hiredis sends).
bool ReadCommand(int fd, std::vector<std::string>& args) {
    std::string header;
    if (!ReadLine(fd, header) || header.empty() || header[0] != '*') {
        return false;
    }
    const long argc = std::strtol(header.c_str() + 1, nullptr, 10);
    if (argc <= 0) {
        return false;
    }

    args.clear();
    for (long i = 0; i < argc; ++i) {
        std::string length_line;
        if (!ReadLine(fd, length_line) || length_line.empty() ||
            length_line[0] != '$') {
            return false;
        }
        const long len = std::strtol(length_line.c_str() + 1, nullptr, 10);
        if (len < 0) {
            return false;
        }
        std::string arg(static_cast<size_t>(len), '\0');
        if (len > 0 &&
            !ReadExact(fd, &arg[0], static_cast<size_t>(len))) {
            return false;
        }
        char crlf[2] = {0, 0};
        if (!ReadExact(fd, crlf, 2)) {
            return false;
        }
        args.push_back(arg);
    }
    return true;
}

void SendAll(int fd, const char* data, size_t len) {
    size_t offset = 0;
    while (offset < len) {
        const ssize_t n =
            ::send(fd, data + offset, len - offset, MSG_NOSIGNAL);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) {
                continue;
            }
            return;
        }
        offset += static_cast<size_t>(n);
    }
}

void SendAll(int fd, const std::string& text) {
    SendAll(fd, text.data(), text.size());
}

void DrainUntilClosed(int fd) {
    char buffer[256];
    while (true) {
        const ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
        if (n == 0) {
            return;
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return;
        }
    }
}

std::string Upper(std::string text) {
    for (char& c : text) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return text;
}

// Minimal RESP peer. It answers AUTH/PING/anything, or stays silent at a chosen
// point so the pool's timeout / ownership behaviour can be exercised.
class FakeRedisPeer {
public:
    enum class Mode {
        kNormal,             // AUTH -> +OK, PING -> +PONG, others -> +OK
        kSilentAfterAccept,  // never answer AUTH
        kSilentAfterAuth     // answer AUTH, then never answer again
    };

    explicit FakeRedisPeer(Mode mode) : mode_(mode) {}
    ~FakeRedisPeer() { Stop(); }

    bool Start() {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ < 0) {
            return false;
        }

        int reuse = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse,
                     sizeof(reuse));

        sockaddr_in address;
        std::memset(&address, 0, sizeof(address));
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;

        if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&address),
                   sizeof(address)) != 0) {
            ::close(listen_fd_);
            listen_fd_ = -1;
            return false;
        }
        if (::listen(listen_fd_, 16) != 0) {
            ::close(listen_fd_);
            listen_fd_ = -1;
            return false;
        }

        socklen_t length = sizeof(address);
        if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&address),
                          &length) != 0) {
            ::close(listen_fd_);
            listen_fd_ = -1;
            return false;
        }
        port_ = ntohs(address.sin_port);
        accept_thread_ = std::thread([this]() { AcceptLoop(); });
        return true;
    }

    void Stop() {
        if (stop_.exchange(true)) {
            return;
        }
        if (listen_fd_ >= 0) {
            ::shutdown(listen_fd_, SHUT_RDWR);
            ::close(listen_fd_);
            listen_fd_ = -1;
        }
        if (accept_thread_.joinable()) {
            accept_thread_.join();
        }
    }

    int port() const { return port_; }
    int active_connections() const { return active_->load(); }

private:
    static void Serve(int fd, Mode mode,
                      std::shared_ptr<std::atomic<int>> active) {
        if (mode == Mode::kSilentAfterAccept) {
            DrainUntilClosed(fd);
        } else {
            std::vector<std::string> args;
            while (ReadCommand(fd, args)) {
                if (args.empty()) {
                    continue;
                }
                const std::string command = Upper(args[0]);
                if (command == "AUTH") {
                    // kSilentAfterAuth: answer AUTH successfully, then go silent
                    // so later commands hit the command I/O timeout.
                    SendAll(fd, "+OK\r\n");
                    if (mode == Mode::kSilentAfterAuth) {
                        DrainUntilClosed(fd);
                        break;
                    }
                } else if (command == "PING") {
                    SendAll(fd, "+PONG\r\n");
                } else {
                    SendAll(fd, "+OK\r\n");
                }
            }
        }
        ::close(fd);
        active->fetch_sub(1);
    }

    void AcceptLoop() {
        while (!stop_.load()) {
            const int fd = ::accept(listen_fd_, nullptr, nullptr);
            if (fd < 0) {
                return;
            }
            active_->fetch_add(1);
            const Mode mode = mode_;
            std::shared_ptr<std::atomic<int>> active = active_;
            // Detached on purpose: the handler owns only the fd plus value
            // copies, so it never touches this object after the peer is gone.
            std::thread([fd, mode, active]() { Serve(fd, mode, active); })
                .detach();
        }
    }

    Mode mode_;
    std::atomic<bool> stop_{false};
    std::shared_ptr<std::atomic<int>> active_ =
        std::make_shared<std::atomic<int>>(0);
    int listen_fd_ = -1;
    int port_ = 0;
    std::thread accept_thread_;
};

bool WaitForActiveConnections(FakeRedisPeer& peer, int expected,
                              int timeout_ms) {
    const auto deadline =
        Clock::now() + std::chrono::milliseconds(timeout_ms);
    while (Clock::now() < deadline) {
        if (peer.active_connections() == expected) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return peer.active_connections() == expected;
}

}  // namespace

// 1. Empty pool: the bounded acquire wait returns nullptr. No peer is needed
//    because pool size 0 performs no connection attempt.
TEST(RedisConPoolContract, EmptyPoolAcquireTimesOut) {
    RedisConPool pool(0, "127.0.0.1", 1, "unused");

    const auto start = Clock::now();
    redisContext* context = pool.getConnection();
    const long long elapsed_ms = ElapsedMs(start);

    EXPECT_EQ(context, nullptr);
    EXPECT_GE(elapsed_ms, 800);   // acquire timeout is 1 s
    EXPECT_LE(elapsed_ms, 4000);  // generous upper bound for slow machines

    pool.Close();
}

// 2. Close() must wake a blocked acquirer and not deadlock with it.
TEST(RedisConPoolContract, CloseWakesBlockedAcquire) {
    RedisConPool pool(0, "127.0.0.1", 1, "unused");

    std::atomic<redisContext*> acquired(nullptr);
    std::atomic<bool> finished(false);
    std::thread waiter([&]() {
        acquired.store(pool.getConnection());
        finished.store(true);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    const auto start = Clock::now();
    pool.Close();
    const long long close_ms = ElapsedMs(start);
    waiter.join();

    EXPECT_TRUE(finished.load());
    EXPECT_EQ(acquired.load(), nullptr);
    // Close joins the health thread, which wakes within its 1 s sleep.
    EXPECT_LE(close_ms, 3000);
}

// 3. Normal AUTH + acquire + handoff, with a real PING round trip.
TEST(RedisConPoolContract, NormalAuthAcquireHandoffAndPing) {
    FakeRedisPeer peer(FakeRedisPeer::Mode::kNormal);
    ASSERT_TRUE(peer.Start());

    RedisConPool pool(2, "127.0.0.1", peer.port(), "secret");

    redisContext* context = pool.getConnection();
    ASSERT_NE(context, nullptr);

    redisReply* reply = static_cast<redisReply*>(
        redisCommand(context, "PING"));
    ASSERT_NE(reply, nullptr);
    EXPECT_EQ(reply->type, REDIS_REPLY_STATUS);
    ASSERT_NE(reply->str, nullptr);
    EXPECT_STREQ(reply->str, "PONG");
    freeReplyObject(reply);

    pool.returnConnection(context);

    // The healthy connection must be handed back on the next acquire.
    redisContext* again = pool.getConnection();
    ASSERT_NE(again, nullptr);
    pool.returnConnection(again);

    pool.Close();
    pool.ClearConnections();
    EXPECT_TRUE(WaitForActiveConnections(peer, 0, 3000));
}

// 4. AUTH that never answers must fail through the configured I/O timeout and
//    must not dereference a null reply. The pool stays empty.
TEST(RedisConPoolContract, AuthSilentFailsByTimeoutWithoutNullDeref) {
    FakeRedisPeer peer(FakeRedisPeer::Mode::kSilentAfterAccept);
    ASSERT_TRUE(peer.Start());

    const auto start = Clock::now();
    RedisConPool pool(1, "127.0.0.1", peer.port(), "secret");
    const long long ctor_ms = ElapsedMs(start);

    EXPECT_GE(ctor_ms, 1800);  // AUTH command I/O timeout is 2 s
    EXPECT_LE(ctor_ms, 8000);

    const auto acquire_start = Clock::now();
    EXPECT_EQ(pool.getConnection(), nullptr);
    EXPECT_GE(ElapsedMs(acquire_start), 800);

    pool.Close();
}

// 5. A command that times out leaves a connection that must be discarded; it
//    must never be handed out again as healthy.
TEST(RedisConPoolContract, CommandTimeoutDiscardsConnection) {
    FakeRedisPeer peer(FakeRedisPeer::Mode::kSilentAfterAuth);
    ASSERT_TRUE(peer.Start());

    RedisConPool pool(1, "127.0.0.1", peer.port(), "secret");

    redisContext* context = pool.getConnection();
    ASSERT_NE(context, nullptr);

    const auto command_start = Clock::now();
    redisReply* reply =
        static_cast<redisReply*>(redisCommand(context, "PING"));
    const long long command_ms = ElapsedMs(command_start);

    EXPECT_EQ(reply, nullptr);
    EXPECT_GE(command_ms, 1800);  // command I/O timeout is 2 s
    EXPECT_LE(command_ms, 8000);

    pool.discardConnection(context);

    const auto acquire_start = Clock::now();
    EXPECT_EQ(pool.getConnection(), nullptr);
    EXPECT_GE(ElapsedMs(acquire_start), 800);

    pool.Close();
}

// 6. Close() is idempotent, and destroying a pool that was never Closed still
//    joins the health thread (no std::terminate / deadlock).
TEST(RedisConPoolContract, CloseIsIdempotentAndDestructorJoins) {
    FakeRedisPeer peer(FakeRedisPeer::Mode::kNormal);
    ASSERT_TRUE(peer.Start());

    {
        RedisConPool pool(1, "127.0.0.1", peer.port(), "secret");
        EXPECT_NO_THROW(pool.Close());
        EXPECT_NO_THROW(pool.Close());
    }

    {
        // Never Closed explicitly: the destructor provides the thread reclaim.
        RedisConPool pool(0, "127.0.0.1", 1, "unused");
        (void)pool;
    }

    SUCCEED();
}

// 7. A connection returned after Close() must be released, not silently
//    dropped. The peer observing the close is the ownership evidence.
TEST(RedisConPoolContract, ReturnAfterCloseReleasesConnection) {
    FakeRedisPeer peer(FakeRedisPeer::Mode::kNormal);
    ASSERT_TRUE(peer.Start());

    RedisConPool pool(1, "127.0.0.1", peer.port(), "secret");

    redisContext* context = pool.getConnection();
    ASSERT_NE(context, nullptr);

    pool.Close();
    pool.returnConnection(context);  // must free the context

    EXPECT_TRUE(WaitForActiveConnections(peer, 0, 3000));

    // The queue is empty; clearing it again must not double free.
    pool.ClearConnections();
    SUCCEED();
}
