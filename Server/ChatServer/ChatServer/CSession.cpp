#include "CSession.h"
#include "CServer.h"
#include <iostream>
#include <sstream>
#include <json/json.h>
#include <json/value.h>
#include <json/reader.h>
#include "LogicSystem.h"

CSession::CSession(boost::asio::io_context& io_context, CServer* server) :
	_socket(io_context), _server(server), _b_close(false), _b_head_parse(false) {
	boost::uuids::uuid  a_uuid = boost::uuids::random_generator()();
	_session_id = boost::uuids::to_string(a_uuid);
	_recv_head_node = std::make_shared<MsgNode>(HEAD_TOTAL_LEN);
	// M5：SessionId 一旦生成即固定，并在此绑定断线桥接。此后所有终止边界都只投递
	// 这个稳定值：领域层永远不会、也不需要拿到 CSession / socket / 连接对象。
	// 投递目标是 LogicSystem（唯一 FIFO 的拥有者），绝不是 MeetingService。
	_disconnect_bridge.Bind(LogicSystem::GetInstance().get(), _session_id);
}

CSession::~CSession() {
	std::cout << "~CSession destruct" << std::endl;
}

tcp::socket& CSession::GetSocket() {
	return _socket;
}

std::string& CSession::GetSessionId() {
	return _session_id;
}

void CSession::SetUserId(int uid) {
	_user_uid = uid;
}
int CSession::GetUserId() {
	return _user_uid;
}

// M6.3（§8.2.2）：IMeetingLiveSession 的唯一实现。
//
// 语义边界（刻意最小化）：
//   - 只读取 _session_id 并**返回值拷贝**：调用方拿不到可变引用，
//     也无法通过返回的 id 反向操作本会话；
//   - const：只读，不加锁、不修改任何成员，因此可以被
//     shared_ptr<const IMeetingLiveSession> 句柄安全调用；
//   - 不读取 _user_uid（F7：该成员构造未初始化，本轮不碰）；
//   - 不触碰 _socket / _send_que / _send_lock / _b_close：
//     Send() / Close() / 异步读写的既有语义与并发行为完全不变。
std::string CSession::DescribeSessionId() const {
	return _session_id;
}

void CSession::Start() {
	AsyncReadHead(HEAD_TOTAL_LEN);
}

void CSession::Send(std::string msg, short msgid) {
	std::lock_guard<std::mutex> lock(_send_lock);
	int send_que_size = _send_que.size();
	if (send_que_size > MAX_SENDQUE) {
		std::cout << "session: " << _session_id << " send que fulled, size is " << MAX_SENDQUE << std::endl;
		return;
	}

	_send_que.push(std::make_shared<SendNode>(msg.c_str(), msg.length(), msgid));
	if (send_que_size > 0) {
		return;
	}
	auto& msgnode = _send_que.front();
	boost::asio::async_write(_socket, boost::asio::buffer(msgnode->_data, msgnode->_total_len),
		std::bind(&CSession::HandleWrite, this, std::placeholders::_1, SharedSelf()));
}

void CSession::Send(char* msg, short max_length, short msgid) {
	std::lock_guard<std::mutex> lock(_send_lock);
	int send_que_size = _send_que.size();
	if (send_que_size > MAX_SENDQUE) {
		std::cout << "session: " << _session_id << " send que fulled, size is " << MAX_SENDQUE << std::endl;
		return;
	}

	_send_que.push(std::make_shared<SendNode>(msg, max_length, msgid));
	if (send_que_size > 0) {
		return;
	}
	auto& msgnode = _send_que.front();
	boost::asio::async_write(_socket, boost::asio::buffer(msgnode->_data, msgnode->_total_len),
		std::bind(&CSession::HandleWrite, this, std::placeholders::_1, SharedSelf()));
}

void CSession::Close() {
	{
		std::lock_guard<std::mutex> lock(_session_mtx);
		_socket.close();
		_b_close = true;
	}

	// M5：Close() 是显式的会话终止边界（socket 关闭 + _b_close 置位）。
	// 通知在**锁外**发出：桥接随后要取唯一 FIFO 的锁，绝不能与 _session_mtx 嵌套。
	// 这里刻意不判断"是否首次 Close"：去重是桥接的结构保证，不由调用方维护状态。
	NotifySessionDisconnected();
}

void CSession::NotifySessionDisconnected() {
	// M5：把"会话生命周期结束"变成一个只携带稳定 SessionId 的内部 work item。
	//
	// 这里**不**直接调用 MeetingService，也不持有连接对象：
	//   SessionDisconnectBridge → LogicSystem（唯一 FIFO 的拥有者）
	//   → 单消费者 → MeetingService::SessionDisconnected()
	// 因此断线事件与 legacy 消息、Meeting 命令共享同一个顺序域。
	const SessionDisconnectDispatch dispatch = _disconnect_bridge.NotifyDisconnected();
	if (dispatch == SessionDisconnectDispatch::REJECTED) {
		// 未绑定或队列已在停止路径上：两者都不是业务错误，但绝不静默吞掉。
		std::cout << "session disconnect work rejected, session: " << _session_id
			<< std::endl;
	}
}

std::shared_ptr<CSession>CSession::SharedSelf() {
	return shared_from_this();
}

void CSession::AsyncReadHead(int total_len) {
	auto self = shared_from_this();
	asyncReadFull(HEAD_TOTAL_LEN, [self, this](const boost::system::error_code& ec, std::size_t bytes_transfered) {
		try {
			if (ec) {
				std::cout << "handle read failed, error is " << ec.what() << std::endl;
				Close();
				_server->ClearSession(_session_id);
				return;
			}

			// 这段其实可以省略
			if (bytes_transfered < HEAD_TOTAL_LEN) {
				std::cout << "read length not match, read [" << bytes_transfered << "] , total ["
					<< HEAD_TOTAL_LEN << "]" << std::endl;
				Close();
				_server->ClearSession(_session_id);
				return;
			}

			_recv_head_node->Clear();
			memcpy(_recv_head_node->_data, _data, bytes_transfered);

			//获取头部MSGID数据
			short msg_id = 0;
			memcpy(&msg_id, _recv_head_node->_data, HEAD_ID_LEN);
			//网络字节序转化为本地字节序
			msg_id = boost::asio::detail::socket_ops::network_to_host_short(msg_id);
			std::cout << "msg_id is " << msg_id << std::endl;
			//id非法
			if (msg_id > MAX_LENGTH) {
				std::cout << "invalid msg_id is " << msg_id << std::endl;
				// M5：该分支同样会终止这个会话（随后从 _sessions 移除，Session 对象
				// 交给析构），因此必须走同一个断线通知；否则该 Session 会在领域侧
				// 永久停留在 CONNECTED/AUTHENTICATED，其 Meeting Binding 也不会被清理。
				// 这里刻意不改动既有 socket 语义（不额外调用 Close()）。
				NotifySessionDisconnected();
				_server->ClearSession(_session_id);
				return;
			}

			short msg_len = 0;
			memcpy(&msg_len, _recv_head_node->_data + HEAD_ID_LEN, HEAD_DATA_LEN);
			//网络字节序转化为本地字节序
			msg_len = boost::asio::detail::socket_ops::network_to_host_short(msg_len);
			std::cout << "msg_len is " << msg_len << std::endl;
			//长度非法
			if (msg_len > MAX_LENGTH) {
				std::cout << "invalid data length is " << msg_len << std::endl;
				// M5：理由同上：该分支是会话的终止边界，必须投递同一个断线事件。
				NotifySessionDisconnected();
				_server->ClearSession(_session_id);
				return;
			}

			_recv_msg_node = std::make_shared<RecvNode>(msg_len, msg_id);
			AsyncReadBody(msg_len);
		}
		catch (std::exception& e) {
			std::cout << "Exception code is " << e.what() << std::endl;
		}
		});
}

void CSession::AsyncReadBody(int total_len) {
	auto self = shared_from_this();
	asyncReadFull(total_len, [self, this, total_len](const boost::system::error_code& ec, std::size_t bytes_transfered) {
		try {
			if (ec) {
				std::cout << "handle read failed, error is " << ec.what() << std::endl;
				Close();
				_server->ClearSession(_session_id);
				return;
			}

			if (bytes_transfered < total_len) {
				std::cout << "read length not match, read [" << bytes_transfered << "] , total ["
					<< total_len << "]" << std::endl;
				Close();
				_server->ClearSession(_session_id);
				return;
			}

			memcpy(_recv_msg_node->_data, _data, bytes_transfered);
			_recv_msg_node->_cur_len += bytes_transfered;
			_recv_msg_node->_data[_recv_msg_node->_total_len] = '\0';
			std::cout << "receive data is " << _recv_msg_node->_data << std::endl;
			//此处将消息投递到逻辑队列中
			LogicSystem::GetInstance()->PostMsgToQue(std::make_shared<LogicNode>(shared_from_this(), _recv_msg_node));
			//继续监听头部接受事件
			AsyncReadHead(HEAD_TOTAL_LEN);
		}
		catch (std::exception& e) {
			std::cout << "Exception code is " << e.what() << std::endl;
		}
		});
}

//读取完整长度
void CSession::asyncReadFull(std::size_t maxLength, std::function<void(const boost::system::error_code&, std::size_t)> handler) {
	::memset(_data, 0, MAX_LENGTH);
	asyncReadLen(0, maxLength, handler);
}

//读取指定字节数
void CSession::asyncReadLen(std::size_t read_len, std::size_t total_len,
	std::function<void(const boost::system::error_code&, std::size_t)> handler) {
	auto self = shared_from_this();
	_socket.async_read_some(boost::asio::buffer(_data + read_len, total_len - read_len),
		[read_len, total_len, handler, self](const boost::system::error_code& ec, std::size_t  bytesTransfered) {
			if (ec) {
				// 出现错误，调用回调函数
				handler(ec, bytesTransfered + read_len);
				return;
			}

			if (read_len + bytesTransfered >= total_len) {
				//长度够了就调用回调函数
				handler(ec, read_len + bytesTransfered);
				return;
			}

			// 没有错误，且长度不足则继续读取
			self->asyncReadLen(read_len + bytesTransfered, total_len, handler);
		});
}

void CSession::HandleWrite(const boost::system::error_code& error, std::shared_ptr<CSession> shared_self) {
	//增加异常处理
	try {
		auto self = shared_from_this();
		if (!error) {
			std::lock_guard<std::mutex> lock(_send_lock);
			//cout << "send data " << _send_que.front()->_data+HEAD_LENGTH << endl;
			_send_que.pop();
			if (!_send_que.empty()) {
				auto& msgnode = _send_que.front();
				boost::asio::async_write(_socket, boost::asio::buffer(msgnode->_data, msgnode->_total_len),
					std::bind(&CSession::HandleWrite, this, std::placeholders::_1, shared_self));
			}
		}
		else {
			std::cout << "handle write failed, error is " << error.what() << std::endl;
			Close();
		}
	}
	catch (std::exception& e) {
		std::cerr << "Exception code : " << e.what() << std::endl;
	}

}

LogicNode::LogicNode(std::shared_ptr<CSession>  session,
	std::shared_ptr<RecvNode> recvnode) :_session(session), _recvnode(recvnode) {

}
