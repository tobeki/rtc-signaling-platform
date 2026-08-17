#include "CServer.h"
#include "HttpConnection.h"
#include "AsioIOServicePool.h"

CServer::CServer(net::io_context& ioc, unsigned short& port):_ioc(ioc),
_acceptor(ioc, tcp::endpoint(tcp::v4(), port)){

}

void CServer::Start() {
	auto self = shared_from_this();
	auto& io_context = AsioIOServicePool::GetInstance()->GetIOService();
	// 先创建连接
	std::shared_ptr<HttpConnection> new_con = std::make_shared<HttpConnection>(io_context);
	_acceptor.async_accept(new_con->GetSocket(), [self, new_con](beast::error_code ec) {
		try {
			if (ec) {
				self->Start();
				return;
			}

			//处理新链接，创建HpptConnection类管理新连接
			new_con->Start();
			//继续监听
			self->Start();
		}
		catch (std::exception& exp) {
			std::cout << "exception is " << exp.what() << std::endl;
			self->Start();
		}
		});
}
