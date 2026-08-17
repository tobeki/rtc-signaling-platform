# rtc-signaling-platform

一个基于 gRPC / WebRTC 的即时通讯（聊天）项目，包含客户端与服务端。

## 目录结构

- `MyChat_Qt/` — 客户端，基于 Qt 4.11 + qmake 构建
- `Server/` — 服务端
  - `ChatServer/`、`ChatServer2/` — 聊天服务（C++，Visual Studio 2022）
  - `GateServer/` — 网关服务（C++）
  - `StatusServer/` — 状态服务（C++）
  - `VerifyServer/` — 验证码 / 邮件服务（Node.js）

## 构建

- 客户端：使用 Qt Creator 打开 `MyChat_Qt/MyChat.pro`
- 服务端（C++）：使用 Visual Studio 2022 打开 `Server/<工程名>/<工程名>.sln`
- VerifyServer：

  ```bash
  cd Server/VerifyServer
  npm install
  npm run server
  ```

## 配置说明

数据库、Redis、邮箱等敏感配置保存在本地的 `config.ini`、`config.json` 中，
这些文件已被 `.gitignore` 忽略。克隆仓库后，请根据实际情况自行创建：
服务端各 C++ 工程根目录下的 `config.ini`，以及 `Server/VerifyServer/config.json`。
