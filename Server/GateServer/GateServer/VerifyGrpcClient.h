#pragma once
#include <grpcpp/grpcpp.h>
#include "message.grpc.pb.h"
#include "const.h"
#include "Singleton.h"

using grpc::Channel;
using grpc::Status;
using grpc::ClientContext;

using message::GetVarifyReq;
using message::GetVarifyRsp;
using message::VarifyService;

class RPConPool {
public:
	RPConPool(std::size_t poolSize, std::string host, std::string port)
		:poolSize_(poolSize), host_(host), port_(port), b_stop_(false) {
		for (std::size_t i = 0; i < poolSize_; i++) {
			std::shared_ptr<Channel> channel = grpc::CreateChannel(host + ":" + port,
				grpc::InsecureChannelCredentials());

			connections_.push(VarifyService::NewStub(channel));
		}
	}

	~RPConPool() {
		std::lock_guard<std::mutex> lock(mutex_);
		Close();
		while (!connections_.empty()) {
			connections_.pop();
		}
	}

	//从队列中获取stub
	std::unique_ptr<VarifyService::Stub> getConnection() {
		std::unique_lock<std::mutex> lock(mutex_);
		cond_.wait(lock, [this]() {
			if (b_stop_) {
				return true;
			}
			return !connections_.empty();
			});
		//如果停止则直接返回空指针
		if (b_stop_) {
			return  nullptr;
		}

		auto context = std::move(connections_.front());
		connections_.pop();
		return context;
	}

	void returnConnection(std::unique_ptr<VarifyService::Stub> context) {
		std::lock_guard<std::mutex> lock(mutex_);

		if (b_stop_) {
			return;
		}
		// 回收一个，并通知一个线程来领取
		connections_.push(std::move(context));
		cond_.notify_one();
	}

	//通知所有线程关闭
	void Close() {
		b_stop_ = true;
		cond_.notify_all();
	}

private:
	std::atomic<bool> b_stop_;
	std::size_t poolSize_;
	std::string host_;
	std::string port_;
	std::queue<std::unique_ptr<VarifyService::Stub>> connections_;
	std::mutex mutex_;
	std::condition_variable cond_;
};

class VerifyGrpcClient:public Singleton<VerifyGrpcClient>
{
	friend class Singleton<VerifyGrpcClient>;
public:
	GetVarifyRsp GetVarifyCode(std::string email) {
		ClientContext context;
		GetVarifyRsp response;
		GetVarifyReq request;
		request.set_email(email);

		auto stub = pool_->getConnection();
		Status status = stub->GetVarifyCode(&context, request, &response);

		if (status.ok()) {
			pool_->returnConnection(std::move(stub));
			return response;
		}
		else {
			pool_->returnConnection(std::move(stub));
			response.set_error(ErrorCodes::RPCFailed);
			return response;
		}
	}

private:
	VerifyGrpcClient();

	std::unique_ptr<RPConPool> pool_;
};

