# Phase 2 - Step 2.2：Linux / WSL 迁移可行性审计

> 本文档是 **审计 / 设计 / 环境兼容性调查** 产物，**不是** Linux 迁移实现。
> 它**不改变**任何权威基线，**不安装**任何依赖，**不创建**任何构建文件，**不编译** ChatServer。

## 0. 证据标签约定

全文的每一处实质结论都标注来源类型（遵循「不得把推断写成事实」）：

| 标签 | 含义 |
| --- | --- |
| **【观测-仓库】** | 由当前仓库文件、`git grep`、字节级扫描直接得出的事实 |
| **【观测-Ubuntu】** | 由 `Ubuntu-24.04-RTC` 内只读命令直接得出的事实 |
| **【包元数据】** | 由 `apt-cache policy/show`、`apt-get -s`、`dpkg-deb -c/-x` 得出的事实 |
| **【推断】** | 基于上述事实的工程推断，给出推理链；**未经验证** |
| **【建议】** | 尚未选择的方案或下一步动作 |
| **【未知】** | 本 Step 无法确定的开放问题 |

---

## 1. 范围与基线（Scope and Baselines）

### 1.1 权威（Windows）基线

**【观测-仓库】** 当前唯一被证明可构建的基线仍为：

| 项目 | 值 |
| --- | --- |
| 环境 | Windows + Visual Studio Community 2022（`17.14.36603.0`，`G:\vs`） |
| MSBuild | `17.14.23.42201` |
| PlatformToolset | `v143` |
| 入口 | `Server/ChatServer/ChatServer.sln` |
| Configuration / Platform | `Debug` / `x64` |
| Target | `Rebuild` |
| 结果 | PASS，exit code `0`，errors `0`，warnings `46` |

本条基线在本次审计中**被重新验证**（见 §9.4 回归结果），且**未被任何操作改变**。

### 1.2 候选（Linux）环境

**【观测-Ubuntu】** 专用 WSL 发行版 `Ubuntu-24.04-RTC`，Ubuntu 24.04.5 LTS（`noble`），用户 `tobeki`。

**Linux 目前只是候选环境。本文不声明 Linux 已成为项目基线。**

### 1.3 本 Step 的性质

**【观测-仓库】** 本 Step 唯一的仓库改动是本文档。**未**修改源码、工程文件、`.gitignore`、`config.ini`；**未**创建 `CMakeLists.txt`、脚本、CI 配置；**未**安装任何依赖；**未**编译 ChatServer。

### 1.4 审计使用的命令（可复核）

| 目的 | 手段 |
| --- | --- |
| 仓库事实 | `git ls-files`、`git grep`、`git check-ignore`、字节级编码扫描 |
| Ubuntu 环境 | `wsl.exe -d Ubuntu-24.04-RTC -- <只读命令>`（显式指定发行版） |
| 包可用性 | `apt-cache policy/show`、`apt-get -s install`（仅模拟） |
| 包内容 | `apt-get download` + `dpkg-deb -c/-x` 到 `/tmp`（**未安装**，用后删除） |
| 依赖版本 | 只读 Windows 依赖树中的版本宏 / 元数据文件 |
| 依赖实际链接 | 只读 M0 构建产物 `.obj` / `.tlog` |

---

## 2. 候选环境（Candidate Environment）

### 2.1 系统与工具链

**【观测-Ubuntu】**

| 项目 | 值 |
| --- | --- |
| 发行版 | `Ubuntu 24.04.5 LTS` |
| Codename | `noble` |
| 内核 | `6.18.33.2-microsoft-standard-WSL2` |
| 用户 | `tobeki`（`uid=1000`，属于 `sudo` 组） |
| GCC | `gcc (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0` |
| G++ | `g++ 13.3.0` |
| CMake | `3.28.3` |
| Git | `2.43.0` |
| GNU Make | `4.3` |
| CPU | `6`（`nproc`），Intel i5-10200H，3 核 × 2 线程 |
| `ulimit -n` | `10240` |

### 2.2 资源

**【观测-Ubuntu】**

| 资源 | 值 |
| --- | --- |
| RAM total | `7.8 GiB` |
| RAM available（观测时） | `7.1 GiB` |
| Swap | `8.0 GiB`（`/dev/sdc` 分区，`used 0B`） |
| 根文件系统 | `/dev/sdd`，`79G` 总，`1.9G` 已用，**`73G` 可用** |

**【观测-仓库】** Windows 侧 `.wslconfig`（只读）声明：`memory=8GB`、`swap=8GB`、`processors=6`、`networkingMode=mirrored`、`dnsTunneling=true`、`autoProxy=true`。**未修改该文件。**

### 2.3 服务与 init

**【观测-Ubuntu】**

| 探测 | 结果 |
| --- | --- |
| `ps -p 1 -o comm=` | `systemd` |
| `systemctl is-system-running` | `running` |
| `/etc/wsl.conf` | `[boot] systemd=true`；`[user] default=tobeki` |

**【推断】** systemd 已启用，因此 `systemctl enable/start redis-server`、`mysql` 这类标准服务管理**预期可用**，无需 WSL 特例。这降低了未来 Redis/MySQL 常驻的工程成本。**本 Step 未启动任何服务。**

### 2.4 现有监听端口

**【观测-Ubuntu】** `ss -tln` 仅有 3 条 DNS 监听（`127.0.0.53:53`、`127.0.0.54:53`、`10.255.255.254:53`）。

**【观测-仓库（Windows 侧）】** `Get-NetTCPConnection -State Listen` 中 **无** `3306` / `6379` / `50051` 等端口监听。

**【推断】** 当前 Windows 与 WSL 两侧都不存在 Redis/MySQL/gRPC 端口占用。

### 2.5 依赖现状：干净环境

**【观测-Ubuntu】** 全部第三方开发头文件**均不存在**：

```text
ABSENT  /usr/include/boost/version.hpp
ABSENT  /usr/include/hiredis/hiredis.h
ABSENT  /usr/include/json/json.h
ABSENT  /usr/include/google/protobuf/message.h
ABSENT  /usr/include/grpcpp/grpcpp.h
ABSENT  /usr/include/cppconn/driver.h
ABSENT  /usr/include/mysql_connection.h
```

`pkg-config --modversion` 对 `protobuf` / `grpc++` / `grpc` / `jsoncpp` / `hiredis` 全部返回 `(not found)`。

**【观测-Ubuntu】** `ldconfig -p` 中仅有一个相关运行期库：`libjsoncpp.so.25`（由系统中其他包带入，非本项目建设）。

**【推断】** 该发行版目前**只具备基础工具链**，RTC 依赖栈为零。这符合「专用新建发行版」的预期，也意味着未来 Step 的依赖安装是可预测、可度量的。

---

## 3. ChatServer 源码可移植性（Source Portability）

审计范围：【观测-仓库】`Server/ChatServer/ChatServer/` 下的全部 33 个受版本控制条目（15 个 `.cpp` + 17 个 `.h` + `message.proto`）。
**未做业务代码质量审查。**

### 3.1 Windows 专有构造：逐 token 检索结果

**【观测-仓库】** 对非工程文件（即排除 `.vcxproj` / `.props`）逐 token 检索：

| 检索模式 | 命中文件 |
| --- | --- |
| `Windows\.h` / `windows\.h` | **无** |
| `winsock2?\.h` | **无** |
| `WSA[A-Za-z]`（`WSADATA`、`WSAStartup` 等） | **无** |
| `_WIN32` / `WIN32` | **无** |
| `_MSC_VER` | **无** |
| `__declspec` | **无** |
| `#pragma comment` / 任何 `#pragma` | **无** |
| `Sleep(` | **无** |
| `HANDLE` / `DWORD` / `SOCKET` | **无** |
| `BOOL` / `TCHAR` / `LPVOID` / `tchar` | **无** |
| `conio\.h` / `crtdbg` | **无** |
| `Win32` / `Interop` / `WinSock`（不区分大小写，全文件） | 仅 `ChatServer.vcxproj`（平台名 `Win32`）与 `PropertySheet.props`（库名 `Win32_Interop.lib`） |

**【观测-仓库】** 对 `win32|interop|WinSock`（不区分大小写）做全目录检索，命中的 **全部** 行都位于 `ChatServer.vcxproj`（`Debug|Win32` 等平台标识）与 `PropertySheet.props`（链接库名）。

**【推断】** 结论是明确的：**ChatServer 应用源码中不存在任何 Win32 / WinSock / MSVC 专有构造**。§14 中「假设源码不直接依赖 Win32 API」的假说，经仓库检索**得到验证**。此前唯一可能被误判为 Windows 专有的是 **构建配置**（§6），而非源码。

### 3.2 精确包含清单（全部三方头文件）

**【观测-仓库】** 源码中出现的全部三方 `#include`（去重）及其归属文件：

| 依赖 | 头文件 | 使用文件 |
| --- | --- | --- |
| Boost.Asio | `<boost/asio.hpp>` | `AsioIOServicePool.h`、`CServer.h`、`CSession.h`、`MsgNode.h` |
| Boost.Beast | `<boost/beast.hpp>`、`<boost/beast/http.hpp>` | `CSession.h` |
| Boost.UUID | `<boost/uuid/uuid_generators.hpp>`、`<boost/uuid/uuid_io.hpp>` | `CSession.h` |
| Boost.PropertyTree | `<boost/property_tree/ptree.hpp>`、`<boost/property_tree/ini_parser.hpp>` | `ConfigMgr.h` |
| **Boost.Filesystem** | `<boost/filesystem.hpp>` | `ConfigMgr.h` |
| JsonCpp | `<json/json.h>`、`<json/value.h>`、`<json/reader.h>` | `LogicSystem.h`、`ChatGrpcClient.h`、`ChatServiceImpl.h`、`CSession.cpp` |
| gRPC C++ | `<grpcpp/grpcpp.h>` | `ChatGrpcClient.h`、`ChatServiceImpl.h` |
| hiredis | `"hiredis.h"`（引号形式） | `RedisMgr.h` |
| MySQL Connector/C++（旧版 JDBC 风格） | `<jdbc/mysql_driver.h>`、`<jdbc/mysql_connection.h>`、`<jdbc/cppconn/{prepared_statement,resultset,statement,exception}.h>` | `MysqlDao.h` |
| 标准库 | `<atomic>` `<chrono>` `<csignal>` `<cstring>` `<fstream>` `<functional>` `<iostream>` `<map>` `<memory>` `<memory.h>` `<mutex>` `<queue>` `<sstream>` `<string>` `<thread>` `<unordered_map>` `<vector>` | 多处 |

**【观测-仓库】** `CSession.h` 第 5–6 行包含 Beast，第 14–15 行仅建立命名空间别名：

```cpp
namespace beast = boost::beast;         // from <boost/beast.hpp>
namespace http = beast::http;           // from <boost/beast/http.hpp>
```

全目录检索未发现任何 `beast::` / `http::` 的**实际调用**，也未发现 `ssl::` / `https`。

**【推断】** Boost.Beast 在 ChatServer 中**只是被包含而未使用**（可能是从其他工程的模板残留）。这是**构建期负担**而非阻塞项；`ssl.lib` / `crypto.lib` 出现在 Windows 链接行是 **gRPC 的传递依赖**，不是 Beast 引入的。本 Step **不删除**这些 include。

### 3.3 网络与并发：已使用现代、可移植的 API

**【观测-仓库】**

| 关注点 | 实际使用 |
| --- | --- |
| Asio 上下文 | **只有** `io_context`（未出现 `io_service` / `get_io_service` 等已废弃 API） |
| 定时器 | 未使用 Asio 定时器；使用 `std::this_thread::sleep_for` |
| TCP 监听 | `CServer.cpp:7`：`_acceptor(io_context, tcp::endpoint(tcp::v4(), port))` |
| 并发 | `std::thread`、`std::mutex`、`std::condition_variable`、`std::atomic` |
| 信号 | `boost::asio::signal_set(io_context, SIGINT, SIGTERM)` |
| UUID | `boost::uuids::random_generator()()` + `boost::uuids::to_string(...)` |

**【推断】**

1. Asio 只使用 `io_context` 这一代 API，**与 Ubuntu 的 Boost 1.83 兼容**（不存在已删除的旧 API 依赖）。
2. `tcp::v4()` 绑定**所有 IPv4 接口**（非 `127.0.0.1`）。这对「Windows 客户端 → WSL 服务端」是有利的（§10.3）。
3. `signal_set` 使用 `SIGINT`/`SIGTERM`，是 POSIX 原生信号，**Linux 侧无需改动**。

### 3.4 文件路径假设

**【观测-仓库】** `ConfigMgr.cpp`（全文 50 行）是唯一涉及路径的文件：

```cpp
boost::filesystem::path current_path = boost::filesystem::current_path();
boost::filesystem::path config_path  = current_path / "config.ini";
boost::property_tree::read_ini(config_path.string(), pt);
```

**【推断】**

- 使用的是 `boost::filesystem::path` 的 `operator/`，**无硬编码分隔符**，无盘符，无 `\` 字面量（全目录检索未发现 `"\\..."` 形式的路径字面量）。
- **源码层面可移植**；但存在一个**运行期契约**：进程的工作目录（CWD）下必须存在 `config.ini`。这在 Windows 上由 post-build `xcopy` 保证，在 Linux 上必须由部署方式保证（§8.3）。

### 3.5 源文件编码：**已确认的真实风险**

**【观测-仓库】** 对全部 33 个源文件做严格 UTF-8 解码校验：

| 分类 | 数量 |
| --- | --- |
| 合法 UTF-8（无 BOM） | 18 |
| 合法 UTF-8（**有 UTF-8 BOM**） | 1（`ChatServer.cpp`） |
| **非法 UTF-8（GBK / CP936 编码）** | **14** |

非法 UTF-8 文件：

```text
AsioIOServicePool.cpp, AsioIOServicePool.h, ChatGrpcClient.h, ChatServiceImpl.cpp,
ConfigMgr.cpp, ConfigMgr.h, const.h, CServer.cpp, CSession.cpp, CSession.h,
LogicSystem.cpp, MsgNode.cpp, MysqlDao.cpp, MysqlDao.h, RedisMgr.cpp, RedisMgr.h, UserMgr.cpp
```

**【观测-仓库】** 将上述文件按 CP936 解码后逐行分类：

| 分类 | 数量 |
| --- | --- |
| 含中文的**注释行** | 186 |
| 含中文的**非纯注释行**（多数实为行尾注释） | 44 |
| **中文字符出现在字符串字面量内部** | **6** |

这 6 处**全部**在 `RedisMgr.h`：

```text
RedisMgr.h:25   std::cout << "认证失败" << std::endl;
RedisMgr.h:33   std::cout << "认证成功" << std::endl;
RedisMgr.h:122  std::cout << "认证失败" << std::endl;
RedisMgr.h:131  std::cout << "认证成功" << std::endl;
RedisMgr.h:236  std::cout << "认证失败" << std::endl;
RedisMgr.h:244  std::cout << "认证成功" << std::endl;
```

**【推断】** 这是本次审计发现的**最值得优先处理的可移植性风险**：

- GBK 字节序列不是合法 UTF-8。GCC/G++ 的默认 `-finput-charset` 是 UTF-8。
- 出现在**注释**中的非法字节，通常不阻止编译（注释不参与字符集转换）。
- 出现在**字符串字面量**中的非法 UTF-8 字节，GCC 在转换到执行字符集时会**报错**（典型信息为 `converting to execution character set: Invalid or incomplete multibyte or wide character`），而非仅警告。
- 因此 `RedisMgr.h` 在 Linux 上有较高概率是**编译期硬阻塞**。

**【未知】** 本 Step **未编译** ChatServer（明确禁止），因此「GCC 对该 6 处究竟报 error 还是 warning」**未经验证**。这是 §11 中列为「可处理风险 R1」并在 §12 中列为 Step 2.3 **第一道门**的原因。

**【建议（非本 Step 执行）】** 三种候选处理方向，**均未实施**：

| 方向 | 机制 | 优点 | 代价 |
| --- | --- | --- | --- |
| a. 转码为 UTF-8 | 将 14 个文件（或仅 `RedisMgr.h`）转为 UTF-8 | 一劳永逸；两个平台一致 | 触及源码，产生 14 文件的巨大 diff |
| b. 编译选项适配 | 对相关目标加 `-finput-charset=GBK -fexec-charset=UTF-8` | 零源码改动 | 属 Linux 构建脚本内的配置，需要长期维护；且「一部分文件是 GBK」这一事实继续存在 |
| c. 仅改造 6 处字面量 | 将 `RedisMgr.h` 的 6 处中文改为 ASCII 或转义 | 改动最小、定位精确 | 只解决已发现的 6 处；注释中的 GBK 仍有理论风险 |

### 3.6 运行期输出会打印配置明文（非可移植性问题）

**【观测-仓库】** `ConfigMgr.cpp:37-40` 在构造时遍历全部 section 并输出：

```cpp
std::cout << "[" << section_name << "]" << std::endl;
for (const auto& key_value_pair : section_config._section_datas) {
    std::cout << key_value_pair.first << "=" << key_value_pair.second << std::endl;
}
```

**【观测-仓库】** 被引用的配置键（**只读取键名，未读取任何键值**）：

```text
SelfServer.Name / SelfServer.Host / SelfServer.Port / SelfServer.RPCPort
Mysql.Host / Mysql.Port / Mysql.User / Mysql.Passwd / Mysql.Schema
PeerServer.Servers
```

**【推断】** 程序启动时会把 `Mysql.Passwd` 等**明文凭据打印到标准输出**。这不是可移植性阻塞，但属**安全相关的观测**，且会影响未来 smoke 测试的日志处理方式（日志会含密钥，不应提交、不应贴进文档）。**本 Step 未读取 `config.ini` 内容，也未复现该输出。**

### 3.7 可移植性结论汇总

| 分类 | 内容 |
| --- | --- |
| **源码级 Linux 阻塞（待验证）** | `RedisMgr.h` 的 6 处 GBK 字符串字面量（§3.5，**未编译验证**） |
| **源码级可移植性调整（可能）** | MySQL 头文件 `jdbc/` 前缀与 Ubuntu 包布局不符（§5.3，见 §6.5） |
| **Windows 构建专用（与源码无关）** | `.vcxproj` / `.props` 中的 `Win32` 平台、`_WIN32_WINNT` 警告、`ws2_32.lib`、`Win32_Interop.lib`、`.lib` 命名、`/MDd`、`*.dll` 拷贝、`protoc.exe` |
| **与 Linux 兼容性无关** | 业务逻辑、并发模型、协议处理、JSON/Redis 使用方式 |
| **纯可移植源码（已确认）** | Asio `io_context`、`std::thread/mutex/condition_variable`、`signal_set`、`boost::filesystem::path`、UUID、PropertyTree、hiredis API 调用、gRPC `Channel/ServerBuilder` 调用 |

---

## 4. Linux 依赖矩阵（Linux Dependency Matrix）

### 4.1 版本对照（这是本 Step 最重要的一张表）

**【观测-仓库（Windows 依赖树只读）】**

| 依赖 | Windows 实际版本 | 证据来源 |
| --- | --- | --- |
| Boost | **1.89.0** | `boost/version.hpp`：`BOOST_VERSION 108900`、`BOOST_LIB_VERSION "1_89"` |
| hiredis | **0.11.0**（Windows 移植版） | `hiredis.h`：`HIREDIS_MAJOR 0`、`HIREDIS_MINOR 11`、`HIREDIS_PATCH 0` |
| MySQL Connector/C++ | **8.3.0** | `jdbc/cppconn/version_info.h`：`MYSQL_CONCPP_VERSION_NUMBER 8030000` |
| JsonCpp | 无版本宏；`json_vc71_libmtd.lib` / `lib_json.pdb` 命名 | 属经典 VS 工程命名（0.x / 1.0.x 世代） |
| protobuf | **3.13.0** | `google/protobuf/port_def.inc`：`PROTOBUF_VERSION 3013000` |
| gRPC | **1.34.0** | `package.xml`：`<release>1.34.0</release>`；`gRPC-C++.podspec`：`version = '1.34.0'` |
| abseil | **1.20200923.2** | `gRPC-C++.podspec`：`abseil_version = '1.20200923.2'` |

**【包元数据（Ubuntu 24.04 noble）】**

| 包 | Candidate 版本 |
| --- | --- |
| `libboost-dev` / `libboost-filesystem-dev` / `libboost-random-dev` / `libboost-system-dev` | `1.83.0.1ubuntu2` |
| `libhiredis-dev` | `1.2.0-6ubuntu3`（依赖 `libhiredis1.1.0`） |
| `libmysqlcppconn-dev` | **`1.1.12-4.1ubuntu2`**（依赖 `libmysqlcppconn7t64`） |
| `libjsoncpp-dev` | `1.9.5-6build1` |
| `libprotobuf-dev` / `protobuf-compiler` | `3.21.12-8.2ubuntu0.3` |
| `libgrpc++-dev` / `libgrpc-dev` / `protobuf-compiler-grpc` | `1.51.1-4.1build5` |
| `redis-server` | `5:7.0.15-1ubuntu0.24.04.4` |
| `mysql-server` | `8.0.46-0ubuntu0.24.04.4` |

**【推断】** 两张表揭示三档难度：

| 难度 | 依赖 | 理由 |
| --- | --- | --- |
| **低** | Boost、JSON、hiredis | 世代接近，源码只用稳定 API |
| **中** | protobuf、gRPC | 世代差距大（3.13→3.21、1.34→1.51），但**必须重新生成**代码，且 `message.proto` 用的是极保守子集（§6） |
| **高** | MySQL Connector/C++ | Ubuntu 只有 **1.1.12**，而 Windows 用 **8.3.0**，且**头文件目录布局不同**（§4.3 / §6.5） |

### 4.2 依赖明细矩阵

| # | 依赖 | 源码如何使用 | Windows 表示 | 源码级/构建级 | Ubuntu 包 | 是否包安装即足 | 需源码构建? | 置信度 | 未解决问题 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 1 | Boost | `asio.hpp`、`beast*`、`uuid*`、`property_tree*`、`filesystem.hpp` | `IncludePath` 指向 `F:\minGw\boost_1_89_0`；**仅自动链接 `libboost_filesystem`** | 构建级 | `libboost-dev` 1.83 + `libboost-filesystem-dev` | **很可能**（§4.4） | 否 | 高 | `random` 是否需显式链接（§4.4） |
| 2 | gRPC C++ | `grpcpp/grpcpp.h`；`Channel/ServerBuilder/Server/Status` 等 | 手工准备的 `F:\cppsoft\grpc` 树 + `grpc++.lib` 等 | 构建级 | `libgrpc++-dev` 1.51.1 | **很可能**（依赖链全由包解析，§4.5） | 否 | 中 | Ubuntu 是否提供 `gRPCConfig.cmake`（**未验证**） |
| 3 | protobuf runtime | 仅通过生成代码，无直接 API 调用 | `libprotobufd.lib` + `F:\cppsoft\grpc\third_party\protobuf\src` | 构建级 | `libprotobuf-dev` 3.21.12 | **很可能** | 否 | 中 | 生成代码与 3.21 运行时的组合（§6） |
| 4 | protoc | Windows 侧手工调用 `protoc.exe`（`start.bat`） | `F:\cppsoft\grpc\visualpro\third_party\protobuf\Debug\protoc.exe`（**机器绝对**） | 构建级 | `protobuf-compiler` 3.21.12 | 是 | 否 | 高 | — |
| 5 | grpc_cpp_plugin | 同上（`start.bat`） | `F:\cppsoft\grpc\visualpro\Debug\grpc_cpp_plugin.exe`（**机器绝对**） | 构建级 | `protobuf-compiler-grpc` 1.51.1 | 是 | 否 | 高 | — |
| 6 | hiredis | `redisConnect`、`redisCommand`、`redisCommandArgv`、`redisFree`、`redisReply`、`REDIS_REPLY_*` | `F:\cppsoft\reids`（Windows 端口）+ `hiredis.lib` + **`Win32_Interop.lib`** | 构建级 | `libhiredis-dev` 1.2.0 | **很可能** | 否 | 中高 | 0.11→1.2 的行为差异（API 签名一致，见 §4.6） |
| 7 | MySQL Connector/C++ | `sql::mysql::get_driver_instance()`、`sql::Connection/Statement/PreparedStatement/ResultSet/SQLException` | `F:\cppsoft\mysql_connector` + `debug\mysqlcppconn.lib` / `mysqlcppconn8.lib` + **2 个运行期 DLL** | 构建级（+ 头路径） | `libmysqlcppconn-dev` **1.1.12** | **不确定**（§4.3） | 可能需要 | 中 | 头文件 `jdbc/` 前缀；8.3→1.1.12 API/行为等价性 |
| 8 | JSON（**JsonCpp**） | `Json::Reader`、`reader.parse`、`Json::Value`、`operator[]`、`asInt/asString`、`toStyledString` | `F:\cppsoft\libjson` + `json_vc71_libmtd.lib` | 构建级 | `libjsoncpp-dev` 1.9.5 | 是（**含路径前缀注意项**，§4.7） | 否 | 高 | — |
| 9 | Threads / pthread | `std::thread/mutex/condition_variable`；Asio 内部依赖 | MSVC 运行库隐式提供 | 构建级 | CMake `Threads::Threads`（`libpthread` 已并入 glibc） | 是 | 否 | 高 | — |
| 10 | OpenSSL / zlib / c-ares / re2 / abseil | **源码未直接使用** | 手工逐一列出 `ssl.lib`、`crypto.lib`、`zlibstaticd.lib`、`cares.lib`、**54 个 `absl_*.lib`** | 构建级（gRPC 传递依赖） | `libgrpc++-dev` 自动带入 `libssl-dev`、`zlib1g-dev`、`libc-ares-dev`、`libre2-dev`、`libabsl-dev` | 是 | 否 | 高 | — |
| — | ~~`ws2_32.lib`~~ | 未使用 | Windows 专有 | — | **Linux 消失** | — | — | 高 | — |
| — | ~~`Win32_Interop.lib`~~ | 未使用 | 仅 Windows hiredis 端口需要 | — | **Linux 消失** | — | — | 高 | — |

### 4.3 MySQL Connector 的**关键不符**：头文件布局

**【包元数据】** `apt-cache show libmysqlcppconn-dev`：

```text
Version: 1.1.12-4.1ubuntu2
Source:  mysql-connector-c++
Homepage: https://dev.mysql.com/doc/relnotes/connector-cpp/en/news-1-1.html
Description-en: ... It mimics the JDBC 4.0 API. ...
Depends: libboost-dev, libmysqlcppconn7t64 (= 1.1.12-4.1ubuntu2)
Size: 274892
```

**【包元数据】** `apt-get -s install` 证明 **Ubuntu 24.04 只有这一个** Connector/C++ 包（`apt-cache search mysqlcppconn` 仅返回 `libmysqlcppconn-dev` 与 `libmysqlcppconn7t64`）。

**【包元数据 + 观测-Ubuntu】** 下载该 `.deb` 到 `/tmp` 后用 `dpkg-deb -c` 列出安装路径（**未安装**）：

```text
/usr/include/cppconn/build_config.h
/usr/include/cppconn/config.h
/usr/include/cppconn/connection.h
/usr/include/cppconn/datatype.h
/usr/include/cppconn/driver.h
/usr/include/cppconn/exception.h
/usr/include/cppconn/metadata.h
/usr/include/cppconn/parameter_metadata.h
/usr/include/cppconn/prepared_statement.h
/usr/include/cppconn/resultset.h
/usr/include/cppconn/resultset_metadata.h
/usr/include/cppconn/sqlstring.h
/usr/include/cppconn/statement.h
/usr/include/cppconn/variant.h
/usr/include/cppconn/version_info.h
/usr/include/cppconn/warning.h
/usr/include/mysql_connection.h
/usr/include/mysql_driver.h
/usr/include/mysql_error.h
/usr/lib/x86_64-linux-gnu/libmysqlcppconn-static.a
/usr/lib/x86_64-linux-gnu/libmysqlcppconn.so        （由 libmysqlcppconn7t64 提供 soname）
```

**【观测-仓库】** Windows 侧布局（`F:\cppsoft\mysql_connector\include`）为：

```text
jdbc/cppconn/*.h          （16 个）
jdbc/mysql_connection.h
jdbc/mysql_driver.h
jdbc/mysql_error.h
mysql/jdbc.h
mysqlx/...                （X DevAPI，源码未使用）
```

**【观测-仓库】** 源码的包含形式是带 `jdbc/` 前缀的：

```cpp
#include <jdbc/mysql_driver.h>
#include <jdbc/mysql_connection.h>
#include <jdbc/cppconn/prepared_statement.h>
#include <jdbc/cppconn/resultset.h>
#include <jdbc/cppconn/statement.h>
#include <jdbc/cppconn/exception.h>
```

**【推断】** **Ubuntu 包中不存在 `jdbc/` 目录**，因此 **6 条 include 在 Linux 上无法解析**。这是**确定性的构建期问题**（不是「可能」），且**与连接器版本无关地存在**：Ubuntu 的 1.1.12 用扁平布局。

**【观测-Ubuntu】** 进一步把该 `.deb` 解包到 `/tmp` 只读检查头文件内容，确认 **API 本身是可用的**：

```text
mysql_driver.h:45   namespace sql
mysql_driver.h:47   namespace mysql
mysql_driver.h:49   namespace NativeAPI
mysql_driver.h:92   CPPCONN_PUBLIC_FUNC MySQL_Driver * get_driver_instance_by_name(const char * const clientlib);
mysql_driver.h:94   CPPCONN_PUBLIC_FUNC MySQL_Driver * get_driver_instance();
mysql_driver.h:95   static inline MySQL_Driver * get_mysql_driver_instance() { return get_driver_instance(); }

driver.h:49         virtual Connection * connect(const sql::SQLString& hostName, const sql::SQLString& userName, const sql::SQLString& password) = 0;
connection.h:97     virtual Statement *createStatement() = 0;
connection.h:133    virtual PreparedStatement * prepareStatement(const sql::SQLString& sql) = 0;
connection.h:151    virtual void setAutoCommit(bool autoCommit) = 0;
resultset.h:116     virtual int32_t getInt(uint32_t columnIndex) const = 0;
resultset.h:122     virtual int64_t getInt64(uint32_t columnIndex) const = 0;
resultset.h:152     virtual bool isFirst() const = 0;
prepared_statement.h:81  virtual void setInt(unsigned int parameterIndex, int32_t value) = 0;
prepared_statement.h:85  virtual void setInt64(unsigned int parameterIndex, int64_t value) = 0;
prepared_statement.h:91  virtual void setString(unsigned int parameterIndex, const sql::SQLString& value) = 0;
```

源码实际用到的类/方法（`sql::mysql::get_driver_instance()`、`get_mysql_driver_instance()`、`driver->connect(...)`、`createStatement()`、`prepareStatement()`、`executeQuery()`、`executeUpdate()`、`setInt/setString/...`、`ResultSet` 取值、`sql::SQLException`）**在 1.1.12 中均存在**。

**【包元数据】** 且 `libmysqlcppconn7t64` 的依赖为：

```text
Depends: libc6 (>= 2.38), libgcc-s1 (>= 3.3.1), libmysqlclient21 (>= 8.0.11), libstdc++6 (>= 13.1)
```

**【推断】** 该 1.1.12 是**针对 MySQL 8.0 客户端库（`libmysqlclient21`）重新构建**的，因此在协议/认证层面**预期**能与 MySQL 8.0 服务端配合。这消除了「1.1.12 太旧无法连接 8.0」这一最初担心的大部分。

**【建议（未实施）】** 头文件前缀问题有三种处理方式，**均未实施**：

| 方向 | 机制 | 优点 | 缺点 |
| --- | --- | --- | --- |
| a. 仓库内加兼容 include 目录 | 在 Linux 构建路径下放一个 `jdbc/` 目录，内含转发头（如 `jdbc/mysql_driver.h` → `#include "mysql_driver.h"`） | 源码零改动 | 新增仓库文件；需说明其用途；`cppconn/` 子前缀同样需要转发 |
| b. 给目标加 `-I` 与宏 | 无法解决，因为 `<jdbc/...>` 的前缀是写死的 | — | **不可行**（除非用 `-I` 指向一个含 `jdbc/` 的合成根） |
| c. 改源码 include | 把 6 条 include 的 `jdbc/` 去掉 | 直接 | 触及源码；破坏 Windows 构建（Windows 只有 `jdbc/` 布局） |

**【推断】** 方向 a 与 c 各有取舍：a 不碰源码但要新增文件；c 碰源码且会破坏 Windows 基线。**这正是 §11 中「需要指挥官裁决」的核心约束之一。**

### 4.4 Boost 明细

**【观测-仓库】** 实际使用组件：

| 组件 | 使用 | 是否 header-only | 是否需要链接库 |
| --- | --- | --- | --- |
| Asio | `io_context`、`acceptor`、`signal_set` | 是（`BOOST_ASIO_SEPARATE_COMPILATION` 未定义） | 否（需 `Threads`） |
| Beast | 仅 include + 命名空间别名，**无实际调用** | 是 | 否 |
| UUID | `random_generator`、`to_string` | 是（Windows 侧未出现 `libboost_uuid`） | 否 |
| PropertyTree | `ptree`、`read_ini` | 是 | 否 |
| **Filesystem** | `boost::filesystem::current_path()`、`path::operator/` | **否** | **是** |

**【观测-仓库】** 关键证据 —— 扫描 M0 构建产物的 15 个 `.obj`，查找 Boost 自动链接指令：

```text
libboost_filesystem-vc143-mt-gd-x64-1_89.lib
  ← ChatGrpcClient.obj, ChatServer.obj, ConfigMgr.obj, LogicSystem.obj, MysqlDao.obj, RedisMgr.obj
```

`ConfigMgr.obj` 中的 `.drectve` 段包含：

```text
/DEFAULTLIB:"libboost_filesystem-vc143-mt-gd-x64-1_89.lib"
/DEFAULTLIB:"msvcprtd"
/DEFAULTLIB:"MSVCRTD"
/DEFAULTLIB:"OLDNAMES"
```

**【推断】** 这是一个在 Step 2.1 中**未被发现**的重要机制：

- `ConfigMgr.cpp` 调用 `boost::filesystem::current_path()`，产生对**编译库**的符号依赖。
- Windows 上该依赖由 **Boost 的自动链接**（`#pragma comment(lib, ...)` → `.obj` 的 `.drectve` → `/DEFAULTLIB`）满足，因此它**从未出现在 `AdditionalDependencies` 里，也从未出现在 `link.command.1.tlog` 的命令行中**（这也解释了 Step 2.1 §5.4 的检索为何在链接命令行找不到 Boost）。
- **Linux 没有 Boost 自动链接机制**。因此 CMake 必须**显式** `find_package(Boost ... COMPONENTS filesystem)` 并链接 `Boost::filesystem`；否则会出现 `undefined reference to boost::filesystem::current_path()`。

**【推断（中等置信度）】** 源码使用 `boost::uuids::random_generator()`。在 Boost **1.83**（Ubuntu 版本）中，`basic_random_generator` 的默认种子来源涉及 Boost.Random 的编译组件；而 Windows 侧的 Boost **1.89** 未产生 `libboost_random` 自动链接（1.86 起 UUID 已 header-only）。因此 **Linux 上可能需要额外链接 `Boost::random`**。

**【建议】** Step 2.3 将 Boost 组件写成 `filesystem random`；若 `random` 不存在则构建期会立即报错并暴露该假设。**本 Step 不做任何改动，也不断言结论。**

**【观测-仓库】** `_WIN32_WINNT` 未在源码中定义，也未在工程中定义；M0 构建中 Boost.Asio 的提示

```text
Please define _WIN32_WINNT or _WIN32_WINDOWS appropriately. ... Assuming _WIN32_WINNT=0x0601 (i.e. Windows 7 target).
```

**【推断】** 这是 **Boost.Asio 在 Windows 上的特有回退行为**，由 MSVC 侧的 Win32 头文件触发。**在 Linux 上不会出现**，因此该提示**与 Linux 构建无关**，应归类为「Windows 构建专用」。**本 Step 未修改该 Windows 配置。**

### 4.5 gRPC / protobuf：依赖链由包管理承担

**【包元数据】** `apt-cache show libgrpc++-dev`：

```text
Version: 1.51.1-4.1build5
Depends: libgrpc++1.51t64, libgrpc-dev, pkgconf, zlib1g-dev, libssl-dev,
         libabsl-dev (>= 20220623.1), libc-ares-dev, libre2-dev
Size: 675906
```

**【包元数据】** `apt-get -s install libgrpc++-dev` 解析出的完整安装链：

```text
libabsl20220623t64, libabsl-dev, libcares2, libprotobuf32t64, libprotoc32t64,
libre2-10, libgrpc29t64, libgrpc++1.51t64, zlib1g-dev, libssl-dev,
libc-ares-dev, libre2-dev, libgrpc-dev, libgrpc++-dev
```

**【推断】** 与 Windows 上需要**手工维护 18 条库目录 + 54 个 `absl_*.lib` + `ssl/crypto/zlib/upb/cares/address_sorting/re2/gpr`** 相比，Linux 侧这条依赖链**完全由 apt 解析**。这是一次显著的工程简化，也是本 Step 支持「包优先」策略的主要依据之一。

**【包元数据 + 观测-Ubuntu】** `protobuf-compiler-grpc`（38942 B）下载后 `dpkg-deb -c`：

```text
./usr/bin/grpc_cpp_plugin
./usr/bin/grpc_php_plugin
./usr/bin/grpc_python_plugin
./usr/bin/grpc_ruby_plugin
```

**【推断】** `grpc_cpp_plugin` 由包提供，**取代** Windows 上机器绝对路径的 `grpc_cpp_plugin.exe`。`protoc` 由 `protobuf-compiler` 提供。

### 4.6 hiredis 明细：区分「Windows 构建权宜」与「真实 API 需求」

**【观测-仓库】** 应用源码使用的 hiredis API（全目录检索）：

```text
redisConnect(), redisCommand(), redisCommandArgv(), redisFree()
redisContext, redisReply
REDIS_REPLY_ERROR / REDIS_REPLY_INTEGER / REDIS_REPLY_NIL / REDIS_REPLY_STATUS / REDIS_REPLY_STRING
```

**【观测-仓库】** 应用源码中**没有任何** `Win32` / `Interop` / `WinSock` 引用（§3.1 已逐 token 验证）。

**【观测-仓库】** Windows 侧 `hiredis.h` 的签名：

```c
void *redisCommandArgv(redisContext *c, int argc, const char **argv, const size_t *argvlen);
```

**【观测-仓库】** hiredis 版本为 **0.11.0**，且 Windows 依赖树利用 `F:\cppsoft\reids\deps\hiredis` 与 `.../src/Win32_Interop` 的**相对布局**（`hiredis.h:39` 等处的 `#include "../../src/Win32_Interop/..."`）。

**【推断】** 结论明确：

| 区分 | 内容 |
| --- | --- |
| **Windows hiredis 构建权宜** | `Win32_Interop.lib`、`src/Win32_Interop` 相对布局、`deps/hiredis` 的目录嵌套 —— 这些都是 **MicrosoftArchive/redis Windows 移植**的产物，**Windows 专有** |
| **真实 Redis API 需求** | 仅 `redisConnect/redisCommand/redisCommandArgv/redisFree` + `redisReply` + `REDIS_REPLY_*` —— **上游 hiredis 原生 API** |

**【推断】** 因此 Linux 侧可直接使用**上游 native hiredis**（`libhiredis-dev` 1.2.0），**无需**拷贝 Windows 的依赖树，**无需** `Win32_Interop`。API 签名在 0.11 → 1.2 之间保持一致，`REDIS_REPLY_*` 常量亦一致，**预期零源码改动**。置信度：中高（未编译验证）。

### 4.7 JSON 明细：确认是 JsonCpp，但**头文件前缀不同**

**【观测-仓库】** 源码使用 `#include <json/json.h>`、`<json/value.h>`、`<json/reader.h>`，API 为 `Json::Reader` + `reader.parse(...)` + `Json::Value` + `operator[]` + `asInt()/asString()` + `toStyledString()`。

**【推断】** 这是 **JsonCpp**（`Json::Value`/`Json::Reader` 是 JsonCpp 特征 API，非 nlohmann/rapidjson）。**未仅凭 Windows 库名 `json_vc71_libmtd.lib` 下判断**，而是依据源码 API 确认。

**【包元数据 + 观测-Ubuntu】** 下载 `libjsoncpp-dev` 1.9.5（22980 B）并解包到 `/tmp`（**未安装**）：

```text
/usr/include/jsoncpp/json/json.h
/usr/include/jsoncpp/json/reader.h
/usr/include/jsoncpp/json/value.h
/usr/include/jsoncpp/json/version.h
... （共 10 个头文件）
/usr/lib/x86_64-linux-gnu/libjsoncpp.so
/usr/lib/x86_64-linux-gnu/cmake/jsoncpp/jsoncppConfig.cmake
/usr/lib/x86_64-linux-gnu/cmake/jsoncpp/jsoncpp-targets.cmake
/usr/lib/x86_64-linux-gnu/pkgconfig/jsoncpp.pc
```

`jsoncpp.pc`：

```text
includedir=${prefix}/include/jsoncpp
Cflags: -I${includedir}
Libs: -L${libdir} -ljsoncpp
```

**【推断】** 头文件位于 **`/usr/include/jsoncpp/json/`**，因此 `<json/json.h>` **必须**配合 include 根 `/usr/include/jsoncpp`。CMake 的 `jsoncpp` config / `pkg-config` 会**自动**提供该路径 → **预期零源码改动**，但**不能**只写 `find_library(jsoncpp)` 而忘记 include 目录。

**【观测-Ubuntu】** 版本与 API 确认：

```text
JSONCPP_VERSION_STRING "1.9.5"
reader.h:36   class JSON_API Reader {
reader.h:76   bool parse(const std::string& document, Value& root, bool collectComments = true);
reader.h:95   bool parse(const char* beginDoc, const char* endDoc, Value& root, bool collectComments = true);
reader.h:110  JSONCPP_DEPRECATED("Use getFormattedErrorMessages() instead.")
```

**【推断】** `Json::Reader` 在 1.9.5 中**仍存在**，`parse(const std::string&, Value&, bool)` 重载**仍存在** → 源码 `reader.parse(info_str, root)` **可编译**。仅 `getFormattedErrorMessages` 相关成员被标记 deprecated（源码未使用）。**预期零源码改动**。置信度：高。

---

## 5. gRPC / protobuf 契约（Codegen Contract）— 本次审计最高优先级

### 5.1 现有生成产物的版本绑定

**【观测-仓库】** 生成的 `message.pb.h` 中嵌入了 protoc 版本断言：

```cpp
// message.pb.h:11
#if PROTOBUF_VERSION < 3013000
#error This file was generated by a newer version of protoc which is ...
// message.pb.h:16
#if 3013000 < PROTOBUF_MIN_PROTOC_VERSION
#error This file was generated by an older version of protoc which is ...
```

**【推断】** 该文件由 **protoc 3.13.0** 生成（三处 `3013000` 一致），并要求运行时 protobuf 与 3.13.0 兼容。与 §4.1 中 Windows 的 `PROTOBUF_VERSION 3013000` 一致。

**【观测-仓库】** 生成的 `message.grpc.pb.h` 包含 1.34 世代的 codegen 头：

```text
#include <grpc/impl/codegen/port_platform.h>
#include <grpcpp/impl/codegen/async_generic_service.h>
#include <grpcpp/impl/codegen/async_stream.h>
#include <grpcpp/impl/codegen/async_unary_call.h>
#include <grpcpp/impl/codegen/client_callback.h>
#include <grpcpp/impl/codegen/client_context.h>
#include <grpcpp/impl/codegen/completion_queue.h>
#include <grpcpp/impl/codegen/message_allocator.h>
#include <grpcpp/impl/codegen/method_handler.h>
#include <grpcpp/impl/codegen/proto_utils.h>
#include <grpcpp/impl/codegen/rpc_method.h>
#include <grpcpp/impl/codegen/server_callback.h>
#include <grpcpp/impl/codegen/server_callback_handlers.h>
#include <grpcpp/impl/codegen/server_context.h>
#include <grpcpp/impl/codegen/service_type.h>
#include <grpcpp/impl/codegen/status.h>
#include <grpcpp/impl/codegen/stub_options.h>
#include <grpcpp/impl/codegen/sync_stream.h>
```

**【推断】** 这是 **gRPC 1.34 时代 codegen 契约**的直接证据。`grpcpp/impl/codegen/*` 在后续版本中被逐步转发/弃用并最终移除（1.60+）。**因此 Windows 生成的这几个文件不能作为 Linux 的编译输入**——不能指望它们与 1.51 的头文件组合可用。**结论：Linux 必须重新生成。** 置信度：高。

### 5.2 应用对 protobuf/gRPC 的耦合度

**【观测-仓库】** `message.proto` 全部 121 行：`syntax = "proto3"`，`package message`，**无** `import`、**无** `option`、**无** `map`、**无** `oneof`、**无** `optional`、**无** `stream`（全部 unary RPC）。字段类型仅 `int32` / `string` / `bool` / `repeated <message>`。3 个 service、8 个 RPC。

**【观测-仓库】** 应用自身的 gRPC API 面：

```text
grpc::Channel, grpc::CreateChannel, grpc::InsecureChannelCredentials,
grpc::ClientContext, grpc::Status,
grpc::ServerBuilder, grpc::Server, grpc::ServerContext, grpc::InsecureServerCredentials
```

**【观测-仓库】** 未发现任何直接 protobuf API 调用（无 `google::protobuf::`、无 `Arena`、无 `Reflection` 手工使用）；对生成类型的使用限于生成的 message 类与 stub/service 基类。

**【推断】**

1. `message.proto` 使用的是 **proto3 最保守的子集**，跨 protoc 版本（3.13 → 3.21）的**生成结果是语义等价**的。高风险 proto 特性（map、oneof、optional、Any、自定义 option）**一个都没用**。
2. 应用只使用 **gRPC 的稳定公开 API**（`Channel` / `ServerBuilder` / `Status` / `ClientContext` / `ServerContext`），这些在 1.34 → 1.51 之间**没有破坏性变更**。
3. 因此「用 Ubuntu 的 protoc 3.21.12 + grpc_cpp_plugin 1.51.1 重新生成 → 与 `libprotobuf-dev` 3.21.12 + `libgrpc++-dev` 1.51.1 一起编译」在**版本自洽性**上是可行的。置信度：中高（**未编译验证**）。

### 5.3 两种 Linux gRPC 策略的评估

#### 策略 A — Ubuntu 包（**审计倾向**）

| 维度 | 事实 / 推断 |
| --- | --- |
| 组成 | `libprotobuf-dev` 3.21.12 + `protobuf-compiler` 3.21.12 + `libgrpc++-dev` 1.51.1 + `protobuf-compiler-grpc` 1.51.1 |
| 工具位置 | `/usr/bin/protoc`、`/usr/bin/grpc_cpp_plugin`【包元数据】 |
| 依赖链 | abseil / re2 / c-ares / zlib / OpenSSL **全部由 apt 解析**【包元数据】 |
| 结论 | **技术上合理**（§5.1–5.2 未发现阻断性证据） |
| 优势 | 装配简单；编译成本极低（无 gRPC 源码构建）；RAM/磁盘压力小；包管理可升级可回滚 |
| 风险 | 版本差（3.13→3.21、1.34→1.51）；生成代码差异；CMake 集成方式差异 |
| **未验证项** | Ubuntu 是否提供 `gRPCConfig.cmake`（**未知**）；生成的代码在 3.21/1.51 下能否一次编译通过（**未编译**） |

#### 策略 B — 固定版本源码构建

| 维度 | 判断 |
| --- | --- |
| 必要性 | **目前没有证据表明必需**。除「必须重新生成」外，仓库中未发现任何要求特定 gRPC/protobuf 精确版本的证据 |
| 构建复杂度 | 高：需自行处理 abseil / re2 / c-ares / zlib / OpenSSL / upb / BoringSSL 的版本匹配 |
| RAM | gRPC 源码构建在链接阶段是内存密集型；在 **7.8 GiB RAM + 8 GiB swap + 6 vCPU** 上存在 OOM 风险（§9） |
| 时间 | **本 Step 不做任何时间估计**（禁止编造） |
| 结论 | **不作为首选**；仅在策略 A 出现**具体、可复现**的失败（例如生成的代码无法在 1.51 上编译，且差异不可调和）时才升级 |

**【建议】** 遵循「审计优先选择更简单的包方案，除非仓库证据给出具体理由说明它不可行」。**本 Step 未选择最终策略**，也未安装任何东西。

### 5.4 生成源码策略（未来 Linux 构建）

**【观测-仓库】** 当前事实：

| 事实 | 内容 |
| --- | --- |
| `message.proto` | **受版本控制**（真值来源） |
| `message.pb.cc/.h`、`message.grpc.pb.cc/.h` | **被 `.gitignore` 忽略**（第 55–58 行），磁盘上存在的是 2025-11-17 手工生成的产物 |
| 工程中的生成步骤 | **不存在**（`.vcxproj` 无 `CustomBuild`） |
| 唯一生成入口 | `Server/ChatServer/ChatServer/start.bat`，内含 2 条机器绝对路径的 `protoc.exe` / `grpc_cpp_plugin.exe` |

**【建议（未实施）】** 未来 Linux 构建采用「proto 为真值 → 构建期生成到构建目录 → 编译进 ChatServer」：

```text
message.proto
   → /usr/bin/protoc（protobuf-compiler 3.21.12）
   → /usr/bin/grpc_cpp_plugin（protobuf-compiler-grpc 1.51.1）
   → ${CMAKE_CURRENT_BINARY_DIR}/message.pb.cc / message.grpc.pb.cc
   → 作为源文件加入 ChatServer 目标
```

对该方向的评估：

| 维度 | 判断 |
| --- | --- |
| 可行性 | 高。`protoc` 与插件都是普通的构建期工具；proto 只有 1 个文件、121 行、无 import |
| 需要的工具 | `protobuf-compiler` + `protobuf-compiler-grpc`（均由包提供） |
| CMake 集成（预期） | `find_package(Protobuf REQUIRED)` + `protobuf_generate_cpp()`；gRPC 部分需 `protobuf_generate(... PLUGIN grpc_cpp_plugin)` 或自定义 `add_custom_command` |
| 兼容性风险 | 生成的代码与 1.51 头文件的配合（未验证）；`PROTOBUF_MIN_PROTOC_VERSION` 断言由新版 protoc 自动满足 |
| 与现状的一致性 | **好**——它把「被忽略的生成文件」从构建输入中移除，让构建不再依赖仓库外的历史产物（这正是 Step 2.1 记录的缺口 G5） |

**简要对比其他两种做法（不展开）**：

| 做法 | 评价 |
| --- | --- |
| 把生成源码**提交**进仓库 | 消除生成步骤依赖，但引入「生成产物与 proto 可能不同步」的长期风险，且与现有 `.gitignore` 策略相反 |
| 继续**手工预生成**（现状） | 已被证明不可复现（依赖机器绝对路径的 `protoc.exe`，且产物被忽略）；Linux 上会重复同一问题 |

**【建议】** 该方向与现有忽略策略**不冲突**（生成到构建目录本就不会进入 Git），因此不需要修改 `.gitignore`。**本 Step 未创建任何 CMake 文件。**

---

## 6. 需要在 Linux 路径中移除的 Windows 专有构建接线

**【观测-仓库】** 以下条目**只存在于 Windows 构建配置**中，Linux 路径应当**没有**对应物。**本 Step 未修改任何一项。**

| # | Windows 条目 | 位置 | Linux 对应 | 性质 |
| --- | --- | --- | --- | --- |
| 1 | `ws2_32.lib` | `PropertySheet.props` | **消失**：Asio 在 POSIX 上使用系统 socket，无需显式链接 | Windows 专有 |
| 2 | `Win32_Interop.lib` | `PropertySheet.props` | **消失**：上游 hiredis 无此需求 | Windows 专有 |
| 3 | `debug\mysqlcppconn.lib`、`mysqlcppconn8.lib` | `PropertySheet.props` | 变为 `-lmysqlcppconn`（1.1.12 只提供这一个库；8.x 的 `mysqlcppconn8` X DevAPI 库在 Linux 不可得且**源码未使用**） | 需替换 |
| 4 | `RuntimeLibrary = MultiThreadedDebugDLL`（`/MDd`） | `PropertySheet.props` | **不适用**：Linux 使用 libstdc++，无 CRT 选择项 | Windows 专有 |
| 5 | `*.dll` post-build 拷贝 | `PropertySheet.props` | **消失**：无 `mysqlcppconn*.dll`；Linux 侧为 `.so`，由动态链接器解析 | Windows 专有 |
| 6 | `xcopy config.ini` post-build | `PropertySheet.props` | **消失**：需由运行方式保证 CWD 含 `config.ini`（§8.3） | 需替换 |
| 7 | `protoc.exe` / `grpc_cpp_plugin.exe`（机器绝对） | `start.bat` | `/usr/bin/protoc`、`/usr/bin/grpc_cpp_plugin` | 需替换 |
| 8 | 18 条 `AdditionalLibraryDirectories`（`F:\cppsoft\grpc\visualpro\...`） | `PropertySheet.props` | **全部消失**：由 `find_package(gRPC)` / 包提供的 CMake 目标替代 | Windows 专有 |
| 9 | 54 个 `absl_*.lib` | `PropertySheet.props` | **全部消失**：由 `libabsl-dev` + gRPC 的 CMake 目标传递 | Windows 专有 |
| 10 | `json_vc71_libmtd.lib` | `PropertySheet.props` | 变为 `-ljsoncpp`（+ include 根 `/usr/include/jsoncpp`） | 需替换 |
| 11 | `.lib` 命名整体范式 | 全部 | 替换为 CMake imported target（**不做机械的 `.lib → .so` 映射**） | 需替换 |
| 12 | `PropertySheet.props` 的 31 条机器绝对路径 | 该文件 | **全部消失**：改为 CMake `find_package` / `pkg-config` | Windows 专有 |
| 13 | `_WIN32_WINNT` 回退提示 | M0 构建日志 | **不出现**：Boost.Asio 的 Win32 分支在 Linux 不编译 | Windows 专有 |
| 14 | **Boost 自动链接**（`/DEFAULTLIB:...libboost_filesystem...`） | `.obj` 的 `.drectve` | **无等价机制**：必须显式链接 `Boost::filesystem` | 需显式化 |
| 15 | MySQL 头文件 `jdbc/` 前缀 | 源码 `MysqlDao.h` | Ubuntu 包**无** `jdbc/` 目录 → 需 include 兼容层或改源码（§4.3） | 需处理 |

**【推断】** 15 项中，11 项是「Linux 上自然消失」，3 项是「换成 CMake 目标/包库」，**只有第 15 项需要额外设计决策**。这是本次审计得出的最重要的结构性结论：**Windows 接线的复杂度远高于 Linux 侧所需的接线。**

---

## 7. 最小 Linux 构建引导方案（提案，不实施）

**【建议】** 以下仅为**概念结构**，**本 Step 未创建任何文件**。

### 7.1 定位与边界

| 决策点 | 建议 | 理由 |
| --- | --- | --- |
| 构建系统 | **CMake**（本项目已安装 3.28.3） | 无需引入新工具；依赖生态（Boost/jsoncpp/gRPC/protobuf）均以 CMake config 或 `pkg-config` 暴露 |
| 落点 | 新增一个**仅用于 Linux** 的 `CMakeLists.txt`（位置待定，**不与 `.vcxproj` 同目录冲突即可**） | 避免与 `.sln` 争抢同一目录语义 |
| Windows 工程文件 | **完全不动** | 保持 `Debug\|x64` 基线有效 |
| 构建目标 | **先只构建 ChatServer** | 不尝试一次性迁移整个仓库（GateServer / StatusServer / MyChat_Qt / VerifyServer 均不在本阶段范围） |
| 源文件列举 | **显式列出**，与 `ChatServer.vcxproj` 的 15 个 `ClCompile` 一一对应，再追加生成文件 | 显式列举使「文件是否真的参与构建」可被直接核对（对应 Step 1.6 R6 的「假成功」风险） |
| 同时支持 Windows | **不追求** | 明确不做「同一套 CMake 也支持 MSVC」；那会立刻把两套接线绑在一起 |

### 7.2 预期的依赖查找（逻辑需求，非代码）

| 逻辑依赖 | 倾向的查找方式 | 备注 |
| --- | --- | --- |
| Boost.Filesystem（+ 可能 Boost.Random） | `find_package(Boost COMPONENTS filesystem [random])` | **必须显式**，因为 Linux 无自动链接（§4.4） |
| Threads | `find_package(Threads REQUIRED)` | Asio 与 `std::thread` 需要 |
| protobuf（runtime + `protoc`） | `find_package(Protobuf REQUIRED)` | 用于生成与链接 |
| gRPC C++ | `find_package(gRPC CONFIG)` → **若不存在则回退** `pkg-config grpc++` 或 `find_library` | **Ubuntu 是否提供 `gRPCConfig.cmake` 仍属未知**（§11 U1） |
| hiredis | `pkg-config hiredis` 或 `find_library(hiredis)` | 头文件位置 `hiredis/hiredis.h`，而源码用 `"hiredis.h"` → **需要注意 include 根**（见下） |
| JsonCpp | `find_package(jsoncpp CONFIG)` → `JsonCpp::JsonCpp` | 会自动提供 `/usr/include/jsoncpp`（§4.7） |
| MySQL Connector/C++ | `find_library(mysqlcppconn)` + 显式 include 处理 | **必须先解决 `jdbc/` 前缀问题** |

**【推断（需在 Step 2.3 验证的两个 include 细节）】**

1. 源码写 `#include "hiredis.h"`（**引号形式、无路径前缀**）。Ubuntu 包安装在 `/usr/include/hiredis/hiredis.h`。因此**必须**提供 `-I/usr/include/hiredis`（或等效），否则引号形式在「当前目录」找不到后不会去 `<hiredis/hiredis.h>`。这是一个**具体的、必须显式处理的接线细节**。
2. 源码写 `<json/json.h>` 而头文件在 `/usr/include/jsoncpp/json/json.h` → 必须提供 `-I/usr/include/jsoncpp`（CMake 的 jsoncpp 目标会做）。

### 7.3 生成集成（预期形态）

```text
① protoc + grpc_cpp_plugin 生成 message.pb.{h,cc} 与 message.grpc.pb.{h,cc} 到构建目录
② 生成文件作为源文件加入 ChatServer 目标
③ 构建目录中的生成头目录加入 include 路径（因为 message.grpc.pb.h 内含 #include "message.pb.h"）
```

【推断】第 ③ 点是必需细节：生成的 `.grpc.pb.h` 与 `.pb.h` 互相以引号形式包含，因此生成目录必须在 include 路径中。

### 7.4 明确不做的构建系统动作

- 不创建 `CMakeLists.txt`（本 Step）
- 不引入 Conan / vcpkg
- 不修改 `.vcxproj` / `.props` / `.gitignore`
- 不把 `GateServer` / `StatusServer` / `VerifyServer` / `MyChat_Qt` 纳入 CMake
- 不为 Windows 提供 CMake 路径

---

## 8. 运行期 Smoke 前置条件（Runtime Smoke Prerequisites）

**【推断】** 一次**有意义的** smoke 需要区分三个层级。**本 Step 未安装、未启动任何服务，也未运行 ChatServer。**

### 8.1 级别 1：进程能起来（最小）

| 前置 | 内容 |
| --- | --- |
| Redis | **必须**。`main()` 第一件事就是 `RedisMgr::GetInstance()->HSet(LOGIN_COUNT, server_name, "0")` |
| `config.ini` | **必须**，且必须在**进程工作目录**下（`ConfigMgr` 用 `current_path() / "config.ini"`） |
| `config.ini` 所需 section | `SelfServer`(Name/Host/Port/RPCPort)、`Mysql`、`PeerServer` |
| MySQL | **本级不需要**（见 §8.2） |

### 8.2 MySQL 是惰性的——但有例外

**【观测-仓库】** `MysqlMgr` 是单例，成员为 `MysqlDao _dao`；`MySqlPool` 的**构造函数中会立即建立 `poolSize_` 个连接**并调用 `con->setSchema(schema_)`。

**【观测-仓库】** 首次调用点都在**逻辑处理器内部**，而非进程启动路径：

```text
ChatServiceImpl.cpp:144   MysqlMgr::GetInstance()->GetUser(uid)
LogicSystem.cpp:182       MysqlMgr::GetInstance()->AddFriendApply(...)
LogicSystem.cpp:273/276   MysqlMgr::GetInstance()->AuthFriendApply/AddFriend(...)
LogicSystem.cpp:489/555/606  MysqlMgr::GetInstance()->GetUser(...)
LogicSystem.cpp:632/638   MysqlMgr::GetInstance()->GetApplyList / GetFriendList
```

**【推断】** MySQL 连接是**首次用到用户/好友逻辑时**才建立。因此「启动 + TCP 接受连接 + 心跳」这一最小链路**预期不需要 MySQL**；而任何涉及用户/好友查询的路径**必须**有 MySQL。

### 8.3 数据库 schema 不在仓库中

**【观测-仓库】** 全仓库检索 `*.sql` / schema / DDL：**无任何命中**。

**【观测-仓库】** 源码中出现表名的位置（`MysqlDao.cpp`）包括 `user`、`friend_apply` 等（`SELECT * FROM user WHERE uid = ?`、`UPDATE friend_apply SET status = 1`）。

**【推断】** 因此 **MySQL 侧的表结构必须由仓库之外提供**。这意味着即使 MySQL 服务就绪，「用户/好友」路径仍然需要外部 DDL 才能通过。这是**真实的可复现性缺口**，与 Linux 迁移本身无关，但会直接影响 smoke 的范围界定。

**【建议】** 未来 smoke 应明确定义为「级别 1（进程起来 + Redis 通 + 端口监听）」，并可选择性地把 MySQL 路径列为**独立后续目标**。

### 8.4 端口与监听

**【观测-仓库】**

| 监听 | 地址来源 | 绑定方式 |
| --- | --- | --- |
| gRPC | `cfg["SelfServer"]["Host"] + ":" + cfg["SelfServer"]["RPCPort"]` | `grpc::ServerBuilder::AddListeningPort` |
| TCP | `atoi(cfg["SelfServer"]["Port"])` | `tcp::endpoint(tcp::v4(), port)` → **所有 IPv4 接口** |
| 出向 gRPC 客户端 | `cfg["PeerServer"]["Servers"]` 派生的 host/port | `grpc::CreateChannel(host + ":" + port, InsecureChannelCredentials())` |

**【推断】** 两个监听端口都**取自 `config.ini`**，未硬编码。`tcp::v4()` 绑定全网卡（非仅 loopback），这对「Windows 侧 Qt 客户端连接 WSL 服务端」是**有利的**。注意 gRPC 的 `Host` 若被写成机器名，在 Linux 上需要可解析。**本 Step 未读取 `config.ini`，因此不判断其当前值是否适用于 WSL。**

---

## 9. 资源评估（Resource Assessment）

### 9.1 实测资源（重申）

**【观测-Ubuntu】** RAM `7.8 GiB`（可用 `7.1 GiB`）、swap `8.0 GiB`、磁盘 `/` **可用 `73 GiB`**、`nproc = 6`。

### 9.2 包式构建路径

**【推断】**

| 环节 | 资源判断 |
| --- | --- |
| 依赖安装 | 包体积从几十 KB（jsoncpp 23 KB、hiredis 78 KB、connector 275 KB、protobuf-compiler 29 KB、grpc plugin 39 KB）到数百 KB（`libgrpc++-dev` 675 KB）；**加上运行期库**（`libgrpc++1.51t64`、`libprotobuf32t64`、`libgrpc29t64`、`libabsl20220623t64` 等）总体量在**数百 MiB 量级**，相对 `73 GiB` 可用空间**完全充裕** |
| protobuf 代码生成 | 单个 121 行 proto，**秒级** |
| ChatServer 编译 | 15 个编译单元 + 2 个生成单元；属于**小型目标**，RAM 压力小 |
| 结论 | **7.8 GiB RAM / 8 GiB swap / 73 GiB 磁盘对包式构建路径是充足的** |

### 9.3 源码构建依赖路径（若被迫）

**【推断】** 若策略 A 失败而必须源码构建 gRPC：

- gRPC 源码构建需要同时构建 abseil、re2、c-ares、zlib、protobuf、BoringSSL/OpenSSL，**编译单元数量与单文件内存占用都远超本项目自身**。
- 在 6 vCPU / 7.8 GiB RAM 上，`-j$(nproc)`（= 6）在链接与重型 TU（BoringSSL、absl、protobuf 的 `.pb.cc` 大文件）阶段有**现实的 OOM 风险**；swap 8 GiB 可缓解但会显著拖慢。

**【建议（保守上界，非实测）】** 如发生源码构建，并行度不超过 **`-j2`**（保守）或 `-j3`（可接受但需观察内存）；**不使用 `-j6`**，也不使用 `-j$(nproc)`。**本 Step 未做任何基准测试，也不提供时间估计。**

### 9.4 Windows 基线回归（本 Step 的强制验证）

**【观测-仓库】** 在本文档完成后执行了一次 `Debug|x64` 的 `Rebuild`（命令与 M0 相同）：

| 项 | 结果 |
| --- | --- |
| 命令 | `G:\vs\MSBuild\Current\Bin\amd64\MSBuild.exe Server\ChatServer\ChatServer.sln /m /t:Rebuild /p:Configuration=Debug /p:Platform=x64 /nologo /clp:Verbosity=minimal /fl ...` |
| exit code | `0` |
| warnings | `46` |
| errors | `0` |
| result | PASS（与 M0 参考值一致） |

**【推断】** Windows 权威基线在本次审计前后**保持不变**（源码与工程文件均未被修改），因此该回归结果符合预期。

### 9.5 构建与测试状态（明确声明）

**【事实】** 本 Step 的构建与测试状态如下，**无任何含糊表述**：

```text
Linux compile : NOT EXECUTED — audit-only Step
Unit tests    : N/A
```

**逐条说明：**

| 项 | 状态 | 理由 |
| --- | --- | --- |
| Linux 上编译 ChatServer | **NOT EXECUTED** | 本 Step 定义为 audit-only；任务明确禁止在 Linux 上编译 ChatServer |
| Linux 上运行 ChatServer | **NOT EXECUTED** | 同上 |
| Linux 上构建任何第三方库 | **NOT EXECUTED** | 明确禁止（未构建 gRPC / protobuf / Boost / hiredis / MySQL） |
| 依赖安装 | **NOT EXECUTED** | 明确禁止；仅做 `apt-cache` 查询、`apt-get -s` 模拟与 `/tmp` 内的包内容只读检查（用后删除） |
| C++ 单元测试 | **Unit tests: N/A** | 仓库中不存在 `test/`、`tests/`、`gtest/` 或任何 C++ 测试工程（Step 2.1 / §2.5 已确认），本 Step 也未引入 |
| Windows `Debug\|x64` 回归构建 | **EXECUTED，PASS**（`0` errors / `46` warnings） | 见 §9.4；这是**回归验证**，**不是**可移植性声明，**不是** Linux 构建，**也不是**单元测试 |

**【判读】** 必须避免的三种误读：

1. 把 `apt-get -s install` 的**模拟解析成功**当作「依赖已可安装并可用」——模拟只证明依赖图可解，不证明 API 兼容；
2. 把 `dpkg-deb -c/-x` 的**包内容检查**当作「包已安装」——本 Step **未安装**任何包；
3. 把 Windows 回归构建当作「Linux 构建已通过」。

---

## 10. 风险与未知（Risks / Unknowns）

### 10.1 硬阻塞（Hard blockers）

**【推断】** 目前只有 **1 项**可被定性为「确定会阻止在 Linux 上构建」：

| # | 内容 | 证据强度 | 说明 |
| --- | --- | --- | --- |
| **H1** | Ubuntu 的 MySQL Connector/C++ 包**不存在 `jdbc/` 目录**，而源码的 6 条 include 带 `jdbc/` 前缀 | **确定性**（包内容已逐条列出） | 不处理则 `MysqlDao.h` 的 include 直接失败。**处理方式存在（兼容 include 层 / 改源码），属可解**，但需要设计决策 |

（`RedisMgr.h` 编码问题是否构成硬阻塞取决于 GCC 行为，因此被列为 R1 而非 H1，见下。）

### 10.2 可处理风险（Manageable risks）

| # | 内容 | 依据 | 处理方向（未实施） |
| --- | --- | --- | --- |
| **R1** | `RedisMgr.h` 的 6 处 **GBK 字节位于字符串字面量内**；GCC 默认以 UTF-8 读取输入，预期报字符集转换错误 | 字节级扫描确认为非法 UTF-8；6 处字面量位置已定位 | 转码为 UTF-8 / 加 `-finput-charset=GBK` / 只改这 6 行；**须先验证 GCC 的实际行为** |
| **R2** | Ubuntu 只有 Connector/C++ **1.1.12**，Windows 用 **8.3.0**；API 存在但不能假定行为完全等价 | 包元数据 + 头文件内容已比对 | Step 2.3 中做最小连接测试；若行为不符则考虑源码构建 8.x（昂贵）或限定 MySQL 功能范围 |
| **R3** | gRPC/protobuf 版本差（1.34→1.51、3.13→3.21）；生成代码必须重做 | 生成头中的 codegen include 与版本断言已确认 | 以 `message.proto` 重新生成，用编译结果验证；proto 为保守子集，风险可控 |
| **R4** | Boost 1.83 上 UUID 是否需要额外链接 `Boost::random` | 1.89 无 `libboost_random` 自动链接；1.83 实现可能不同 | 显式声明组件；用链接错误快速证伪 |
| **R5** | `#include "hiredis.h"` 的引号形式要求显式提供 `/usr/include/hiredis` | 源码形式与包布局已确认 | 在 CMake 中显式加 include 目录 |
| **R6** | CMake 集成 gRPC 的方式未知（是否有 `gRPCConfig.cmake`） | 未下载 `libgrpc++-dev` 校验 | 以 `pkg-config grpc++` 或 `find_library` 作为回退 |
| **R7** | MySQL 表结构不在仓库中，用户/好友路径需要外部 DDL | 全仓库无 `*.sql` | 把 smoke 范围限定为「进程起来 + Redis + 端口」 |
| **R8** | 程序启动会把 `config.ini` 全部键值（含 `Mysql.Passwd`）打印到 stdout | `ConfigMgr.cpp:37-40` 源码 | smoke 日志必须按含密处理，不得提交或粘贴 |

### 10.3 WSL 网络拓扑（仅记录会影响本项目的具体问题）

**【观测-Ubuntu + 观测-仓库】**

| 项 | 值 |
| --- | --- |
| `.wslconfig` 网络模式 | `networkingMode=mirrored`（只读查得） |
| WSL `eth0` | `192.168.1.75/23` |
| Windows `WLAN` | `192.168.1.75` → **与 WSL 相同** |
| WSL `/etc/resolv.conf` | `nameserver 10.255.255.254` |
| Windows 侧提示 | `wsl: 检测到 localhost 代理配置，但未镜像到 WSL` |
| WSL 内监听 | 仅 3 条 DNS（`*:53`） |
| Windows 内相关端口 | `3306/6379/50051/8080/9000/2379` **均无监听** |

**【推断】** 具体的、与项目相关的影响：

| # | 影响 | 说明 |
| --- | --- | --- |
| N1 | **localhost 双向可用** | mirrored 模式下 WSL 与 Windows 共享网络栈，因此「Windows Qt 客户端 → `localhost:<TCP port>` → WSL ChatServer」**无需端口转发**。这是有利条件 |
| N2 | **端口冲突成为真实冲突** | 与旧 NAT 模式不同，mirrored 模式下 Windows 与 WSL **共享端口空间**。若 Windows 侧已有 Redis(6379)/MySQL(3306)，WSL 内**将无法再绑定同一端口**。当前两侧均无占用（已实测），但**未来必须避免在 Windows 上同时启动同端口服务** |
| N3 | `tcp::v4()` 绑全网卡 | **有利**：不限制为 loopback，客户端可从任一方向连入 |
| N4 | `autoProxy=true` 与 localhost 代理 | 观测到 WSL 的 localhost-proxy 警告；**仅影响** WSL 内访问 Windows 侧 localhost 代理的场景（例如某些 HTTP 代理），**不影响**本地 socket 监听/连接 |
| N5 | gRPC 的 `Host` 取值 | `AddListeningPort` 使用 `config.ini` 的 `SelfServer.Host`。若该值为 Windows 机器名，Linux 侧需要可解析；**本 Step 未读取该值，不做判断** |

**【建议】** 不修改任何网络配置。未来的开发拓扑「Windows Qt 客户端 → WSL ChatServer → WSL 内 Redis/MySQL」在 mirrored 模式下**技术上成立**；Redis/MySQL **建议安装在 WSL 内**（原生 Ubuntu 包 + systemd 已就绪），而不是让 WSL 去连 Windows 侧的服务——后者会立刻触发 N2 的端口冲突问题。

---

## 11. 可行性判定（Feasibility Decision）

### 11.1 判定

```text
PROCEED WITH CONSTRAINTS
```

### 11.2 依据

**支持继续的证据：**

| 证据 | 强度 |
| --- | --- |
| **ChatServer 源码零 Win32 / WinSock / MSVC 专有构造**（逐 token 检索，`Windows.h`/`WSA*`/`_WIN32`/`_MSC_VER`/`__declspec`/`#pragma`/`Sleep`/`HANDLE`/`DWORD`/`SOCKET` 全部无命中） | 强（确定性） |
| 网络与并发使用 `io_context` + `std::thread` + `signal_set`，与现代 Boost/POSIX 完全一致 | 强 |
| hiredis 使用**上游原生 API**；`Win32_Interop` 纯属 Windows 移植权宜，Linux 直接消除 | 强 |
| JsonCpp 1.9.5 的 `Json::Reader` 与 `parse(...)` 重载**均已确认存在**，API 面匹配 | 强（头文件内容已核对） |
| `message.proto` 是**最保守的 proto3 子集**（无 import/option/map/oneof/optional/stream），跨版本生成语义等价 | 强 |
| 应用只用 gRPC 稳定公开 API（`Channel`/`ServerBuilder`/`Status`/...），不含内部 API 依赖 | 强 |
| gRPC/abseil/re2/c-ares/zlib/OpenSSL 依赖链在 Ubuntu **完全由 apt 解析**，取代 Windows 手工维护的 54 个 absl 库 + 18 条库目录 | 强（模拟安装已验证解析） |
| 环境层面 **systemd 可用**、**无端口占用**、**mirrored 网络使 localhost 双向可达** | 强 |
| 资源（7.8 GiB RAM / 8 GiB swap / 73 GiB 磁盘 / 6 vCPU）对**包式**构建路径充足 | 中高 |

**要求附加约束的证据：**

| 证据 | 强度 |
| --- | --- |
| Ubuntu MySQL Connector/C++ 只有 **1.1.12**（Windows 8.3.0），且**头文件布局无 `jdbc/` 目录** → 确定性构建问题，需设计决策 | 强（包内容已逐条列出） |
| `RedisMgr.h` 有 **6 处 GBK 字符串字面量**，位于非法 UTF-8 文件中 → 高概率编译阻塞（**未编译验证**） | 中高（字节级证据强，GCC 行为未验证） |
| gRPC/protobuf 存在 **17 / 8 个 minor 的版本差**，生成代码必须重做且**尚未编译验证** | 中 |
| MySQL 表结构**不在仓库**中，用户/好友路径不可仅凭仓库复现 | 强 |
| Boost 1.83 的 UUID 是否需要 `Boost::random` **未定** | 中 |

**结论理由**：源码侧与依赖侧的证据都指向「这不是一次需要重写架构的迁移」——**没有任何一项证据要求改变业务逻辑、并发模型或领域设计**，且 Linux 侧的构建接线**显著少于** Windows 侧。因此给出 `DEFER` 是不恰当的。但存在 **1 项确定性构建问题（MySQL 头布局）** 与 **1 项高概率编译阻塞（源码编码）**，两者都尚未通过实际编译证伪，因此**不能**给出无条件 `PROCEED`。

### 11.3 本判定对基线的影响

**【事实】** 本判定**不改变**权威基线。

```text
Windows 权威基线（Debug|x64，PASS）：保持不变
Linux build baseline：NOT ESTABLISHED
```

即使判定为 `PROCEED WITH CONSTRAINTS`，在后续的**编译 / 链接 / 运行 smoke 门**全部通过之前，**Linux 仍不是项目基线**。

---

## 12. 建议的 Step 2.3（Proposed Step 2.3）

**【建议】** 仅描述，**不执行**。

### Step 2.3 — Linux 构建引导（ChatServer only），含严格的门顺序

**目标**：在 `Ubuntu-24.04-RTC` 中，从**干净的 Linux 克隆**出发，为 **ChatServer** 建立最小 CMake 构建，并按固定顺序逐门推进；**允许失败，失败必须如实记录**。

**边界**

| 项 | 内容 |
| --- | --- |
| 仓库克隆位置 | `/home/tobeki/projects/rtc-signaling-platform`（**不在** `/mnt/c` 或 `/mnt/d`） |
| 允许改动 | 新增 Linux 侧 CMake 相关文件；**不得**修改 `.sln` / `.vcxproj` / `.props` / `.gitignore` |
| 允许安装 | 仅限已声明的包集合（Boost/JSON/hiredis/protobuf/gRPC/MySQL connector + `build-essential` 已具），**包管理器之外不构建任何第三方** |
| 禁止 | 修改 Windows 工程；改动 `config.ini`；提交生成文件；动 `Ubuntu-20.04`；开始 Meeting M1 |
| 权威基线 | **仍为 Windows `Debug\|x64`**；每个门结束后重跑 Windows 回归 |

**门顺序（前门未过不得进入后门）**

| 门 | 内容 | 目的 |
| --- | --- | --- |
| **G0** | Windows `Debug\|x64` Rebuild 回归（`46/0`） | 证明改动未污染基线 |
| **G1** | **源码字符集行为验证**：确认 GCC 对 GBK 字符串字面量报 error 还是 warning；据此在「转码 / `-finput-charset` / 改 6 行」中做**一次**决策 | 解除 R1 的不确定性，避免在后门反复试错 |
| **G2** | 依赖安装 + `find_package` 全部成功解析（Boost / Threads / Protobuf / gRPC / jsoncpp / hiredis / mysqlcppconn） | 暴露 R4/R6 的真实状态 |
| **G3** | **`message.proto` 生成成功**且生成文件在 Linux 工具链下**单独编译通过** | 解除 R3 |
| **G4** | **首次链接 ChatServer**；如失败，逐条记录 `undefined reference` 并归类（Boost 组件 / hiredis include / MySQL 头布局 / gRPC 目标名） | 一次性暴露所有接线缺口 |
| **G5** | 进程级 smoke（**级别 1**，见 §8.1）：进程启动 + Redis 连通 + 两个端口监听 | 首次产生「Linux 上可运行」的证据 |

**明确不产出**：性能数字、可移植性承诺、Windows 侧任何改动、Meeting 相关代码。

**【判读】** 之所以把「源码字符集」放在最前面（G1）：它是唯一一个**会改写源码**的决策点（其余门大多只影响构建脚本）。先确认事实，可以避免在「改源码」与「改编译选项」之间反悔。

---

## 13. 明确的非目标（Explicit Non-Goals）

**Step 2.2 没有做以下任何一件事：**

1. **没有**建立 Linux 基线（`Linux build baseline: NOT ESTABLISHED`）；
2. **没有**在 Linux 上编译 ChatServer；
3. **没有**在 Linux 上运行 ChatServer；
4. **没有**安装任何 RTC 依赖（未执行 `apt install` / `apt upgrade` / 任何依赖源码构建）；
5. **没有**创建 `CMakeLists.txt`、CMake 模块、shell 构建脚本、Linux 配置文件、Dockerfile 或 CI workflow；
6. **没有**重新生成 protobuf/gRPC 文件；
7. **没有**修改 Windows 构建的任何部分（`.sln` / `.vcxproj` / `.vcxproj.filters` / `.props` / `.gitignore` / `.bat` / `config.ini`）；
8. **没有**修改任何 `.cpp` / `.h` / `.hpp` / `.proto`；
9. **没有**安装依赖二进制或改动生成的 protobuf 文件；
10. **没有**触碰 `Ubuntu-20.04`（未对其执行任何命令，未修改，未改变默认发行版）；
11. **没有**运行 `do-release-upgrade`、`wsl --set-default`、`wsl --unregister`、`wsl --shutdown` 或任何破坏性 WSL 迁移命令；
12. **没有**修改全局 `.wslconfig`（仅只读查看）；
13. **没有**修改 `PATH` 或任何环境变量；
14. **没有**修改 `.bashrc` / `.profile` / `/etc/environment`；
15. **没有**开始 Meeting M1 或实现任何 Meeting C++ 代码。

**本 Step 的交付物**：一份可追溯的可行性记录，使 Step 2.3（以及未来任何 Linux 迁移实现）有明确的「改动前契约」可比对。

---

## 14. 附录

### 14.1 临时产物的处置（透明度）

为核实包内容（§4.3 / §4.5 / §4.7），在 WSL 的 `/tmp` 下做过**下载但不安装**的操作：

| 动作 | 内容 |
| --- | --- |
| 下载到 `/tmp/s22-probe/` | `libmysqlcppconn-dev`（275 kB）、`libjsoncpp-dev`（23 kB）、`protobuf-compiler-grpc`（39 kB） |
| 解包检查 | `dpkg-deb -c` 列路径、`dpkg-deb -x` 到 `/tmp/s22-probe/{x,jx}` 后 grep 头文件内容 |
| **未执行** | 任何 `apt install` / `apt upgrade` / `sudo` 操作 |
| **清理** | `/tmp/s22-probe` 已用 `rm -rf` 完全删除，并已用 `ls` 验证不存在（`No such file or directory`） |
| Windows 侧临时文件 | 仅若干只读探针脚本与输出，位于 `%TEMP%`，**不在仓库内** |

### 14.2 本轮未读取的内容

| 未读取 | 原因 |
| --- | --- |
| `config.ini` **的内容** | 含数据库/Redis/邮箱敏感信息（任务明确禁止）；仅读取了**键名** |
| 应用业务逻辑（协议处理、好友/聊天流程细节） | 本 Step 只审可移植性，不做业务代码审查 |
| `ChatServer2` / `GateServer` / `StatusServer` 的实现 | 超出本 Step 范围 |
| `MyChat_Qt/` | Qt 客户端，不在范围 |
| Windows 依赖目录的**全部**内容 | 仅读取版本宏与元数据文件（`version.hpp`、`port_def.inc`、`package.xml`、`version_info.h`、`hiredis.h`），**未修改** |

### 14.3 与既有文档的关系

| 文档 | 关系 |
| --- | --- |
| `docs/phase2/STEP-2.1-build-wiring-and-dependency-contract.md` | 本文**不重复**其 31 条路径清单、配置矩阵与 G1–G21 缺口编号；本文聚焦 **Linux 侧**的可行性与增量发现 |
| `docs/phase1/STEP-1.6-...md` §31 B1–B7 | 其 Phase 2 工程基础设施 backlog 与本文 §4/§6/§10 对应；本文**未**解决其中任何一项 |
| Step 2.1 中「此前设计的 Windows 依赖路径集中化 Step 2.2」 | **已被本 Step 取代**（未执行） |

### 14.4 本文新增的、Step 2.1 中未记录的发现

| # | 发现 | 章节 |
| --- | --- | --- |
| 1 | **17 个 ChatServer 源文件为 GBK 编码（非法 UTF-8）**，其中 **6 处在字符串字面量内**（全在 `RedisMgr.h`） | §3.5 |
| 2 | **Boost.Filesystem 是真实的编译库依赖**，Windows 侧由 Boost **自动链接**通过 `.obj` 的 `/DEFAULTLIB` 满足（`ConfigMgr.obj` 实测），因此从未出现在链接命令行 | §4.4 |
| 3 | Ubuntu 的 MySQL Connector/C++ **只有 1.1.12**，且**头文件无 `jdbc/` 目录**，与源码的 6 条 include 不符 | §4.3 |
| 4 | 该 1.1.12 **依赖 `libmysqlclient21 (>= 8.0.11)`**，即为 MySQL 8.0 重新构建，协议层面预期可用 | §4.3 |
| 5 | jsoncpp 头文件在 **`/usr/include/jsoncpp/json/`**（非 `/usr/include/json/`） | §4.7 |
| 6 | 生成的 `message.grpc.pb.h` 使用 **gRPC 1.34 的 `grpcpp/impl/codegen/*` 契约** → 不可作为 Linux 编译输入 | §5.1 |
| 7 | `message.proto` 为**保守 proto3 子集**（无任何高风险特性），跨 protoc 版本重生成风险低 | §5.2 |
| 8 | `CSession.h` **包含但未使用** Boost.Beast | §3.2 |
| 9 | `MysqlMgr`（及 `MySqlPool`）是**首次调用时**才建立连接，进程启动路径不需要 MySQL | §8.2 |
| 10 | 仓库中**不存在任何 `*.sql` / DDL**，MySQL 表结构必须外部提供 | §8.3 |
| 11 | `ConfigMgr` 启动时把**全部配置键值（含密码）打印到 stdout** | §3.6 |
| 12 | WSL 使用 **`networkingMode=mirrored`**：`eth0` 与 Windows WLAN **同 IP**；localhost 双向可达，但**端口冲突成为真实冲突** | §10.3 |
| 13 | `libgrpc++-dev` 的 `Depends` **自动覆盖** abseil / re2 / c-ares / zlib / OpenSSL，取代 Windows 手工维护的 54 个 `absl_*.lib` | §4.5 |
