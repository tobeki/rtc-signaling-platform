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
//   7. a connection returned after Close() is released (not silently dropped);
//   8. a non-"+OK" AUTH reply (e.g. a valid RESP integer) is rejected and the
//      failed connection never enters the pool.
//
// M6.7.2 addition - direct, runtime-only evidence for the 60 s health cycle
// (B29). These cases do NOT shorten the production period and do NOT reach into
// private state; they wait for events that the real health thread produces:
//   9.  HC1 HealthThreadPeriodicPingSucceeds
//         the real check thread PINGs on its own cycle, the peer answers +PONG,
//         and the pooled connection stays usable;
//   10. HC2 DiscardedConnectionReauthenticatedByHealthThread
//         a connection discarded through the production API is replaced by the
//         health thread's reconnect (a second, real AUTH) and works again;
//   11. HC3 CloseDuringSilentHealthPingIsBounded
//         Close() still joins the health thread while that thread is blocked on
//         a PING the peer deliberately never answers (released by the hiredis
//         I/O timeout, not by a reply).
// Each case waits ~60 s of wall clock on purpose: the 60 s period is the
// behaviour under test. Bounds are generous and only prevent a hang.
//
// Not covered here (see the M6.6a completion report):
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
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
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

// Budgets for the real 60 s health-check cycle. The production period is the
// behaviour under test, so nothing here shortens it; the bounds only keep a
// broken build from hanging the suite.
const int kHealthWaitMs = 90000;  // first check runs ~59-60 s after construction
const int kCloseBudgetMs = 8000;  // 2 s command I/O timeout + tick + join slack

// Thread-safe observation of what the peer actually parsed on the wire.
//
// Requirements this type exists for:
//   * every counter is incremented from the peer's real RESP parsing path, so a
//     TCP accept can never be mistaken for a successful AUTH or a PING;
//   * waits use steady_clock + a predicate (spurious wakeups cannot end them
//     early) and always have an explicit deadline;
//   * it is held through std::shared_ptr, so the peer's detached handler threads
//     keep the state alive and never touch a destroyed FakeRedisPeer.
class PeerEvents {
public:
    struct Snapshot {
        int accepted = 0;
        int auth_requests = 0;
        int auth_ok_replies = 0;
        int ping_requests = 0;
        int pong_replies = 0;
        int silenced_pings = 0;  // parsed PING deliberately left unanswered
        int closed = 0;
    };

    void RecordAccepted() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++counts_.accepted;
        }
        condition_.notify_all();
    }

    void RecordAuthRequest() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++counts_.auth_requests;
        }
        condition_.notify_all();
    }

    void RecordAuthOkReply() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++counts_.auth_ok_replies;
        }
        condition_.notify_all();
    }

    void RecordPingRequest() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++counts_.ping_requests;
        }
        condition_.notify_all();
    }

    void RecordPongReply() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++counts_.pong_replies;
        }
        condition_.notify_all();
    }

    // A PING that will never be answered. Besides counting it, remember when it
    // arrived: the Close-overlap proof is (Close return time - this instant).
    void RecordSilencedPing() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++counts_.silenced_pings;
            silenced_ping_at_ = Clock::now();
        }
        condition_.notify_all();
    }

    void RecordClosed() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++counts_.closed;
        }
        condition_.notify_all();
    }

    Snapshot Get() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return counts_;
    }

    bool TryGetSilencedPingTime(Clock::time_point& out) const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (counts_.silenced_pings == 0) {
            return false;
        }
        out = silenced_ping_at_;
        return true;
    }

    bool WaitForAuthRequests(int expected, int timeout_ms) const {
        return WaitUntil(timeout_ms, [this, expected]() {
            return counts_.auth_requests >= expected;
        });
    }

    bool WaitForAuthOkReplies(int expected, int timeout_ms) const {
        return WaitUntil(timeout_ms, [this, expected]() {
            return counts_.auth_ok_replies >= expected;
        });
    }

    bool WaitForPingRequests(int expected, int timeout_ms) const {
        return WaitUntil(timeout_ms, [this, expected]() {
            return counts_.ping_requests >= expected;
        });
    }

    bool WaitForPongReplies(int expected, int timeout_ms) const {
        return WaitUntil(timeout_ms, [this, expected]() {
            return counts_.pong_replies >= expected;
        });
    }

    bool WaitForSilencedPings(int expected, int timeout_ms) const {
        return WaitUntil(timeout_ms, [this, expected]() {
            return counts_.silenced_pings >= expected;
        });
    }

    bool WaitForClosedConnections(int expected, int timeout_ms) const {
        return WaitUntil(timeout_ms, [this, expected]() {
            return counts_.closed >= expected;
        });
    }

private:
    template <typename Predicate>
    bool WaitUntil(int timeout_ms, Predicate predicate) const {
        std::unique_lock<std::mutex> lock(mutex_);
        return condition_.wait_until(
            lock,
            Clock::now() + std::chrono::milliseconds(timeout_ms),
            predicate);
    }

    mutable std::mutex mutex_;
    mutable std::condition_variable condition_;
    Snapshot counts_;
    Clock::time_point silenced_ping_at_ = Clock::time_point();
};

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
        kSilentAfterAuth,    // answer AUTH, then never answer again
        kUnexpectedAuthReply,// AUTH -> :1 (valid RESP, but not a +OK status)
        kSilenceOnHealthPing // AUTH -> +OK; every PING is parsed, recorded and
                             // deliberately left unanswered while the socket
                             // stays open, so the client is released only by its
                             // own command I/O timeout (never by a reply or EOF)
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

    // Shared event state: the detached handler threads keep their own strong
    // reference, so reading it later can never touch a destroyed peer.
    std::shared_ptr<PeerEvents> events() const { return events_; }

private:
    static void Serve(int fd, Mode mode,
                      std::shared_ptr<std::atomic<int>> active,
                      std::shared_ptr<PeerEvents> events) {
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
                    events->RecordAuthRequest();
                    if (mode == Mode::kUnexpectedAuthReply) {
                        // Valid RESP integer reply, but not "+OK": a correct
                        // client must reject it instead of treating it as
                        // authentication success.
                        SendAll(fd, ":1\r\n");
                        continue;
                    }
                    // kSilentAfterAuth: answer AUTH successfully, then go silent
                    // so later commands hit the command I/O timeout.
                    SendAll(fd, "+OK\r\n");
                    events->RecordAuthOkReply();
                    if (mode == Mode::kSilentAfterAuth) {
                        DrainUntilClosed(fd);
                        break;
                    }
                } else if (command == "PING") {
                    events->RecordPingRequest();
                    if (mode == Mode::kSilenceOnHealthPing) {
                        // Keep the socket open and send nothing: the peer proves
                        // it saw a complete PING, and the client must time out.
                        events->RecordSilencedPing();
                        continue;
                    }
                    SendAll(fd, "+PONG\r\n");
                    events->RecordPongReply();
                } else {
                    SendAll(fd, "+OK\r\n");
                }
            }
        }
        ::close(fd);
        events->RecordClosed();
        active->fetch_sub(1);
    }

    void AcceptLoop() {
        while (!stop_.load()) {
            const int fd = ::accept(listen_fd_, nullptr, nullptr);
            if (fd < 0) {
                return;
            }
            active_->fetch_add(1);
            events_->RecordAccepted();
            const Mode mode = mode_;
            std::shared_ptr<std::atomic<int>> active = active_;
            std::shared_ptr<PeerEvents> events = events_;
            // Detached on purpose: the handler owns only the fd plus value
            // copies (including a strong reference to the shared event state),
            // so it never touches this object after the peer is gone.
            std::thread([fd, mode, active, events]() {
                Serve(fd, mode, active, events);
            }).detach();
        }
    }

    Mode mode_;
    std::atomic<bool> stop_{false};
    std::shared_ptr<std::atomic<int>> active_ =
        std::make_shared<std::atomic<int>>(0);
    std::shared_ptr<PeerEvents> events_ = std::make_shared<PeerEvents>();
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

// Bounded retry around the production getConnection().
//
// The health thread hands the connection back *after* the peer has already
// observed the AUTH/PONG, so "event seen" does not imply "already pooled". This
// keeps retrying within an explicit budget instead of assuming the handoff.
redisContext* AcquireWithin(RedisConPool& pool, int timeout_ms) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    while (Clock::now() < deadline) {
        redisContext* context = pool.getConnection();  // itself bounded (1 s)
        if (context != nullptr) {
            return context;
        }
    }
    return nullptr;
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

// 8. AUTH returning a syntactically valid RESP reply that is not "+OK" (here
//    an integer) must be rejected: not treated as success, not pooled, and the
//    failed connection must be released.
TEST(RedisConPoolContract, UnexpectedAuthReplyIsRejected) {
    FakeRedisPeer peer(FakeRedisPeer::Mode::kUnexpectedAuthReply);
    ASSERT_TRUE(peer.Start());

    const auto start = Clock::now();
    RedisConPool pool(1, "127.0.0.1", peer.port(), "secret");
    const long long ctor_ms = ElapsedMs(start);

    // This is not the timeout path: the constructor must return promptly.
    EXPECT_GE(ctor_ms, 0);
    EXPECT_LE(ctor_ms, 3000);

    // The rejected connection must not be available from the pool.
    const auto acquire_start = Clock::now();
    EXPECT_EQ(pool.getConnection(), nullptr);
    EXPECT_GE(ElapsedMs(acquire_start), 800);

    // The failed redisContext must have been freed: the peer observes the close.
    EXPECT_TRUE(WaitForActiveConnections(peer, 0, 3000));

    // The pool must still close normally.
    EXPECT_NO_THROW(pool.Close());
    SUCCEED();
}

// 9. HC1 (M6.7.2 / B29): the real 60 s health thread must PING on its own and
//    the pooled connection must stay usable afterwards.
//
// Attribution: this test sends no command at all before the PING is observed, so
// the observed PING can only have come from the production health thread. The
// wait is on the +PONG reply (which the peer records after really sending it),
// so no snapshot race can turn a pending reply into a false failure.
TEST(RedisConPoolContract, HealthThreadPeriodicPingSucceeds) {
    FakeRedisPeer peer(FakeRedisPeer::Mode::kNormal);
    ASSERT_TRUE(peer.Start());
    const std::shared_ptr<PeerEvents> events = peer.events();

    const auto pool_start = Clock::now();
    RedisConPool pool(1, "127.0.0.1", peer.port(), "secret");

    // Construction authenticates exactly one pooled connection.
    ASSERT_TRUE(events->WaitForAuthOkReplies(1, kHealthWaitMs))
        << "initial AUTH was not observed";

    // Wait for the health thread's own PING. No test-issued PING exists yet.
    ASSERT_TRUE(events->WaitForPingRequests(1, kHealthWaitMs))
        << "health thread did not PING within " << kHealthWaitMs << " ms";
    ASSERT_TRUE(events->WaitForPongReplies(1, kHealthWaitMs))
        << "peer did not answer the health PING with +PONG";
    const long long ping_ms = ElapsedMs(pool_start);

    const PeerEvents::Snapshot after_ping = events->Get();
    EXPECT_GE(after_ping.ping_requests, 1);
    EXPECT_GE(after_ping.pong_replies, 1);
    EXPECT_EQ(after_ping.auth_requests, 1) << "a healthy PING must not reconnect";
    EXPECT_EQ(after_ping.silenced_pings, 0);

    // The connection survived the health PING and is still handed out.
    redisContext* context = AcquireWithin(pool, 5000);
    ASSERT_NE(context, nullptr) << "pool did not hand back the health-checked connection";

    redisReply* reply = static_cast<redisReply*>(redisCommand(context, "PING"));
    ASSERT_NE(reply, nullptr);
    EXPECT_EQ(reply->type, REDIS_REPLY_STATUS);
    ASSERT_NE(reply->str, nullptr);
    EXPECT_STREQ(reply->str, "PONG");
    freeReplyObject(reply);

    pool.returnConnection(context);

    EXPECT_NO_THROW(pool.Close());
    pool.ClearConnections();
    EXPECT_TRUE(events->WaitForClosedConnections(1, 5000));

    // The health PING can only happen on the production 60 s cycle.
    EXPECT_GE(ping_ms, 30000) << "health PING arrived far earlier than the 60 s cycle";
    EXPECT_LE(ping_ms, kHealthWaitMs);
}

// 10. HC2 (M6.7.2 / B29): a connection discarded through the production API must
//     be replaced by the health thread's reconnect, including a second real AUTH.
//
// The reconnect is triggered only by the production 60 s cycle: the test never
// calls the private reconnect()/checkThreadPro(), never touches fail_count_ or
// counter_, and the peer sees two independent TCP connections.
TEST(RedisConPoolContract, DiscardedConnectionReauthenticatedByHealthThread) {
    FakeRedisPeer peer(FakeRedisPeer::Mode::kNormal);
    ASSERT_TRUE(peer.Start());
    const std::shared_ptr<PeerEvents> events = peer.events();

    RedisConPool pool(1, "127.0.0.1", peer.port(), "secret");
    ASSERT_TRUE(events->WaitForAuthOkReplies(1, kHealthWaitMs))
        << "initial AUTH was not observed";

    // Take the only pooled connection, then discard it (I/O-error path).
    redisContext* first = pool.getConnection();
    ASSERT_NE(first, nullptr);
    pool.discardConnection(first);

    // Before the health cycle the pool must offer nothing: the discarded handle
    // is gone and must never be handed out again as healthy.
    const auto empty_start = Clock::now();
    EXPECT_EQ(pool.getConnection(), nullptr);
    EXPECT_GE(ElapsedMs(empty_start), 800);  // bounded acquire wait is 1 s

    const PeerEvents::Snapshot before_cycle = events->Get();
    EXPECT_EQ(before_cycle.auth_requests, 1);
    EXPECT_EQ(before_cycle.ping_requests, 0) << "empty pool means no health PING";

    // The health thread must reconnect on its own cycle: a second real AUTH.
    ASSERT_TRUE(events->WaitForAuthRequests(2, kHealthWaitMs))
        << "health thread did not reconnect within " << kHealthWaitMs << " ms";
    ASSERT_TRUE(events->WaitForAuthOkReplies(2, kHealthWaitMs))
        << "reconnect AUTH did not receive +OK";

    const PeerEvents::Snapshot after_reconnect = events->Get();
    EXPECT_EQ(after_reconnect.auth_requests, 2);
    EXPECT_EQ(after_reconnect.auth_ok_replies, 2);
    EXPECT_GE(after_reconnect.accepted, 2) << "reconnect must use a new TCP connection";
    EXPECT_GE(after_reconnect.closed, 1) << "the discarded connection must be closed";
    EXPECT_EQ(after_reconnect.silenced_pings, 0) << "normal mode never withholds a reply";

    // The reconnected connection must really work.
    redisContext* second = AcquireWithin(pool, 5000);
    ASSERT_NE(second, nullptr) << "pool did not recover after the health reconnect";

    redisReply* reply = static_cast<redisReply*>(redisCommand(second, "PING"));
    ASSERT_NE(reply, nullptr);
    EXPECT_EQ(reply->type, REDIS_REPLY_STATUS);
    ASSERT_NE(reply->str, nullptr);
    EXPECT_STREQ(reply->str, "PONG");
    freeReplyObject(reply);

    pool.returnConnection(second);

    EXPECT_NO_THROW(pool.Close());
    pool.ClearConnections();
    EXPECT_TRUE(events->WaitForClosedConnections(2, 5000));
}

// 11. HC3 (M6.7.2 / B29): Close() must still join the health thread while that
//     thread is blocked on a PING the peer deliberately never answers.
//
// Overlap proof (M6.7.2a - three separately measured intervals, because the
// total span alone is not evidence of overlap: it also grows when the PING has
// already timed out before Close() is even called):
//   A  ping_at     -> close_start : Close() must begin while the ~2 s command
//                                  I/O timeout is still pending (< 1500 ms);
//   B  close_start -> Close return: Close() must really wait, so it cannot
//                                  return in a few milliseconds;
//   C  ping_at     -> Close return: total span, reported for diagnosis only.
// A structural cross-check backs A up: until the health thread frees the
// redisContext the peer cannot have observed the socket closing, so "no close
// observed yet" proves the PING was still blocked at that instant.
// This is a bounded-completion check inside the documented fault model, NOT a
// claim of boundedness for arbitrary OS/kernel hangs.
TEST(RedisConPoolContract, CloseDuringSilentHealthPingIsBounded) {
    FakeRedisPeer peer(FakeRedisPeer::Mode::kSilenceOnHealthPing);
    ASSERT_TRUE(peer.Start());
    const std::shared_ptr<PeerEvents> events = peer.events();

    RedisConPool pool(1, "127.0.0.1", peer.port(), "secret");
    ASSERT_TRUE(events->WaitForAuthOkReplies(1, kHealthWaitMs))
        << "initial AUTH was not observed";

    // The health thread sends its PING; the peer parses it and stays silent.
    ASSERT_TRUE(events->WaitForPingRequests(1, kHealthWaitMs))
        << "health thread did not PING within " << kHealthWaitMs << " ms";
    ASSERT_TRUE(events->WaitForSilencedPings(1, kHealthWaitMs));

    Clock::time_point ping_at;
    ASSERT_TRUE(events->TryGetSilencedPingTime(ping_at));

    const PeerEvents::Snapshot before_close = events->Get();
    EXPECT_EQ(before_close.silenced_pings, 1);
    EXPECT_EQ(before_close.pong_replies, 0) << "the health PING must stay unanswered";
    EXPECT_EQ(before_close.auth_requests, 1);

    // The production window between "the peer parsed the PING" and "hiredis
    // reports the command I/O timeout" is ~2000 ms. Requiring A below 1500 ms
    // leaves >=500 ms of margin for WSL scheduling while still proving that the
    // PING was outstanding when Close() was called.
    const long long kPendingPingWindowMs = 1500;

    // Interval A: PING arrival -> the instant Close() is invoked.
    const auto close_start = Clock::now();
    const long long elapsed_before_close_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            close_start - ping_at)
            .count();

    // Structural cross-check for A: the peer can only observe the socket
    // closing after the health thread has freed that redisContext, which only
    // happens after its PING already timed out. "No close observed yet" and
    // "still a single AUTH" therefore both prove the PING is still blocked.
    const PeerEvents::Snapshot at_close = events->Get();
    EXPECT_EQ(at_close.closed, 0)
        << "the health thread had already dropped its connection before Close() "
           "started (elapsed_before_close_ms="
        << elapsed_before_close_ms << ")";
    EXPECT_EQ(at_close.auth_requests, 1)
        << "a reconnect before Close() means the PING had already timed out "
           "(elapsed_before_close_ms="
        << elapsed_before_close_ms << ")";
    EXPECT_LT(elapsed_before_close_ms, kPendingPingWindowMs)
        << "Close() started after the PING may already have timed out "
           "(elapsed_before_close_ms="
        << elapsed_before_close_ms << ", window=" << kPendingPingWindowMs
        << " ms): this run proves NO overlap and must not be reported as PASS";

    // Close while that PING is outstanding.
    EXPECT_NO_THROW(pool.Close());

    // Interval B: how long Close() itself blocked.
    const long long close_ms = ElapsedMs(close_start);
    // Interval C: total span, reported for diagnosis only (never the proof).
    const long long since_ping_ms = ElapsedMs(ping_at);

    const std::string timing =
        "elapsed_before_close_ms=" + std::to_string(elapsed_before_close_ms) +
        " close_ms=" + std::to_string(close_ms) +
        " since_ping_ms=" + std::to_string(since_ping_ms);

    // Publish the three measurements so a run is auditable afterwards (they
    // appear in --gtest_output=xml) without adding runtime log noise.
    RecordProperty("elapsed_before_close_ms",
                   static_cast<int>(elapsed_before_close_ms));
    RecordProperty("close_ms", static_cast<int>(close_ms));
    RecordProperty("since_ping_ms", static_cast<int>(since_ping_ms));

    // After Close the pool must not hand out connections, and the silent PING
    // must not have triggered any extra reconnect.
    EXPECT_EQ(pool.getConnection(), nullptr) << timing;
    const PeerEvents::Snapshot after_close = events->Get();
    EXPECT_EQ(after_close.auth_requests, 1)
        << "Close must not trigger reconnects; " << timing;
    EXPECT_EQ(after_close.pong_replies, 0) << timing;

    // Interval B lower bound: with A < 1500 ms at least ~500 ms of the 2 s I/O
    // timeout is still pending, so a Close() that really waits for the health
    // thread cannot return this quickly. This is what rules out "returned
    // immediately because nothing was blocked".
    EXPECT_GE(close_ms, 300)
        << "Close() did not wait for the blocked PING; " << timing;
    // Interval B upper bound: 2 s command I/O timeout + one 1 s health tick +
    // join slack. A budget for the test, not a production SLA.
    EXPECT_LE(close_ms, kCloseBudgetMs)
        << "Close() did not finish within budget; " << timing;

    // Close() keeps its documented idempotence.
    EXPECT_NO_THROW(pool.Close());

    pool.ClearConnections();
    EXPECT_TRUE(events->WaitForClosedConnections(1, 5000));
}
