#pragma once
#include "const.h"
#include "hiredis.h"
#include <queue>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <condition_variable>
#include "Singleton.h"
#include <cstring>
#include <iostream>

class RedisConPool {
public:
	RedisConPool(size_t poolSize, const std::string& host, int port, const std::string& pwd)
		: b_stop_(false), poolSize_(poolSize), host_(host), pwd_(pwd), port_(port),
		  fail_count_(0), counter_(0) {
		for (size_t i = 0; i < poolSize_; ++i) {
			auto* context = CreateAuthenticatedContext();
			if (context != nullptr) {
				connections_.push(context);
			}
		}

		check_thread_ = std::thread([this]() {
			while (!b_stop_.load()) {
				counter_++;
				if (counter_ >= 60) {
					checkThreadPro();
					counter_ = 0;
				}

				std::this_thread::sleep_for(std::chrono::seconds(1));
			}
			});
	}

	~RedisConPool() {
		Close();
		ClearConnections();
	}

	void ClearConnections() {
		std::lock_guard<std::mutex> lock(mutex_);
		while (!connections_.empty()) {
			auto* context = connections_.front();
			redisFree(context);
			connections_.pop();
		}
	}

	// 有界获取（R-20 契约第 4 项）：超时或 Close() 之后返回 nullptr。
	// 等待期间会释放互斥量，因此其他线程的归还或 Close 不会被等待者阻塞。
	redisContext* getConnection() {
		std::unique_lock<std::mutex> lock(mutex_);
		const bool signaled = cond_.wait_for(
			lock,
			std::chrono::milliseconds(kAcquireTimeoutMs),
			[this]() { return b_stop_.load() || !connections_.empty(); });
		if (b_stop_.load()) {
			return nullptr;
		}
		if (!signaled) {
			return nullptr;
		}
		auto* context = connections_.front();
		connections_.pop();
		return context;
	}

	redisContext* getConNonBlock() {
		std::lock_guard<std::mutex> lock(mutex_);
		if (b_stop_.load()) {
			return nullptr;
		}

		if (connections_.empty()) {
			return nullptr;
		}

		auto* context = connections_.front();
		connections_.pop();
		return context;
	}

	// 所有权：Close() 之后连接池不再接收连接，
	// 必须释放该 context，而不能静默丢弃指针（否则泄漏）。
	void returnConnection(redisContext* context) {
		if (context == nullptr) {
			return;
		}
		bool must_free = false;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			if (b_stop_.load()) {
				must_free = true;
			}
			else {
				connections_.push(context);
				cond_.notify_one();
			}
		}
		if (must_free) {
			redisFree(context);
		}
	}

	// 丢弃已经失效的连接（I/O 错误或命令超时）。
	// 健康检查线程会在 fail_count_ > 0 时按既有机制重连。
	void discardConnection(redisContext* context) {
		if (context == nullptr) {
			return;
		}
		redisFree(context);
		fail_count_++;
	}

	// 幂等：停止标志在持锁时更新，解锁后再唤醒等待者；
	// 只有一个调用者能把线程移出并 join，且 join 本身不持锁。
	void Close() {
		std::thread to_join;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			b_stop_.store(true);
			if (check_thread_.joinable()) {
				to_join = std::move(check_thread_);
			}
		}
		cond_.notify_all();
		if (to_join.joinable()) {
			to_join.join();
		}
	}

private:
	enum {
		kAcquireTimeoutMs = 1000,
		kConnectTimeoutSeconds = 1,
		kCommandTimeoutSeconds = 2
	};

	static struct timeval ConnectTimeout() {
		struct timeval tv;
		tv.tv_sec = kConnectTimeoutSeconds;
		tv.tv_usec = 0;
		return tv;
	}

	static struct timeval CommandTimeout() {
		struct timeval tv;
		tv.tv_sec = kCommandTimeoutSeconds;
		tv.tv_usec = 0;
		return tv;
	}

	// 为命令、PING 和 AUTH 设置 socket 读写超时。
	// 注意：这是 I/O 操作超时，不是整条命令的硬性总期限。
	static bool ApplyCommandTimeout(redisContext* context) {
		if (context == nullptr) {
			return false;
		}
		return redisSetTimeout(context, CommandTimeout()) == REDIS_OK;
	}

	// 新建连接的唯一入口：依次完成“建立连接 + 配置超时 + AUTH”。
	// 任一步失败都返回 nullptr，且不会泄漏 context 或 reply。
	redisContext* CreateAuthenticatedContext() {
		auto* context = redisConnectWithTimeout(host_.c_str(), port_, ConnectTimeout());
		if (context == nullptr || context->err != 0) {
			if (context != nullptr) {
				redisFree(context);
			}
			return nullptr;
		}

		if (!ApplyCommandTimeout(context)) {
			redisFree(context);
			return nullptr;
		}

		auto* reply = (redisReply*)redisCommand(context, "AUTH %s", pwd_.c_str());
		if (reply == nullptr) {
			// AUTH I/O 失败或超时：该连接不可用。
			redisFree(context);
			return nullptr;
		}
		if (reply->type == REDIS_REPLY_ERROR) {
			std::cout << "认证失败" << std::endl;
			freeReplyObject(reply);
			redisFree(context);
			return nullptr;
		}

		freeReplyObject(reply);
		std::cout << "认证成功" << std::endl;
		return context;
	}

	bool reconnect() {
		if (b_stop_.load()) {
			return false;
		}

		auto* context = CreateAuthenticatedContext();
		if (context == nullptr) {
			return false;
		}

		returnConnection(context);
		return true;
	}

	void checkThreadPro() {
		size_t pool_size = 0;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			if (b_stop_.load()) {
				return;
			}
			pool_size = connections_.size();
		}

		for (size_t i = 0; i < pool_size && !b_stop_.load(); ++i) {
			auto* context = getConNonBlock();
			if (context == nullptr) {
				break;
			}

			auto* reply = (redisReply*)redisCommand(context, "PING");
			if (context->err != 0 || reply == nullptr || reply->type == REDIS_REPLY_ERROR) {
				std::cout << "reply is null, redis ping failed: " << std::endl;
				if (reply != nullptr) {
					freeReplyObject(reply);
				}
				redisFree(context);
				fail_count_++;
				continue;
			}

			freeReplyObject(reply);
			returnConnection(context);
		}

		// 契约第 12 项：Close() 之后不得再开始新的重连。
		while (fail_count_ > 0 && !b_stop_.load()) {
			if (!reconnect()) {
				break;
			}
			fail_count_--;
		}
	}

	std::atomic<bool> b_stop_;
	size_t poolSize_;
	std::string host_;
	std::string pwd_;
	int port_;
	std::queue<redisContext*> connections_;
	std::atomic<int> fail_count_;
	std::mutex mutex_;
	std::condition_variable cond_;
	std::thread  check_thread_;
	int counter_;
};

class RedisMgr: public Singleton<RedisMgr>,
	public std::enable_shared_from_this<RedisMgr>
{
	friend class Singleton<RedisMgr>;
public:
	~RedisMgr();
	bool Get(const std::string& key, std::string& value);
	bool Set(const std::string& key, const std::string& value);
	bool LPush(const std::string& key, const std::string& value);
	bool LPop(const std::string& key, std::string& value);
	bool RPush(const std::string& key, const std::string& value);
	bool RPop(const std::string& key, std::string& value);
	bool HSet(const std::string& key, const std::string& hkey, const std::string& value);
	bool HSet(const char* key, const char* hkey, const char* hvalue, size_t hvaluelen);
	std::string HGet(const std::string& key, const std::string& hkey);
	bool HDel(const std::string& key, const std::string& field);
	bool Del(const std::string& key);
	bool ExistsKey(const std::string& key);
	void Close() {
		_con_pool->Close();
		_con_pool->ClearConnections();
	}

	/* std::string acquireLock(const std::string& lockName,
		int lockTimeout, int acquireTimeout);

	bool releaseLock(const std::string& lockName,
		const std::string& identifier);
	*/

	/*void IncreaseCount(std::string server_name);
	void DecreaseCount(std::string server_name);
	void InitCount(std::string server_name);
	void DelCount(std::string server_name);*/

private:
	RedisMgr();
	std::unique_ptr<RedisConPool>  _con_pool;
};
