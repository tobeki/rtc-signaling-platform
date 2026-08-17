#pragma once
#include "Singleton.h"
#include <functional>
#include <map>
#include "const.h"

class HttpConnection;
typedef std::function<void(std::shared_ptr<HttpConnection>)> HttpHandler;
class LogicSystem:public Singleton<LogicSystem>
{
	friend class Singleton<LogicSystem>;
public:
	~LogicSystem() {}
	bool HandleGet(std::string path, std::shared_ptr<HttpConnection> connection);
	bool HandlePost(std::string path, std::shared_ptr<HttpConnection> connection);
	void RegGet(std::string url, HttpHandler handler);
	void RegPost(std::string url, HttpHandler handler);
private:
	LogicSystem();
	std::map<std::string, HttpHandler> _get_handlers;
	std::map<std::string, HttpHandler> _post_handlers;
};

