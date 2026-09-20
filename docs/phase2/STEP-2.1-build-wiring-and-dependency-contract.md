# Phase 2 - Step 2.1：ChatServer 构建接线与依赖契约审计

> 本文档是**审计与设计**产物，**不改变任何构建行为**。
> 全文严格区分三类陈述，并以标签显式标注：
>
> - **【事实】**：可由当前仓库文件、工具输出或实测命令直接验证。
> - **【判读】**：基于事实的推断，给出推断依据；可能被后续证据推翻。
> - **【选项】**：尚未选择的未来方案，不是承诺。

---

## 0. 文档元信息

| 项目 | 值 |
| --- | --- |
| 仓库 | `rtc-signaling-platform` |
| 分支 | `dev` |
| 审计起始 commit | `e2e52b4e194294688049c652d2753e8da6db566c` |
| 上游基线（M0） | `e2e52b4`（M0 Baseline Build Capture 所在 commit） |
| 审计日期 | 2026-09-20 |
| 权威构建入口 | `Server/ChatServer/ChatServer.sln` |
| 本 Step 是否改变构建行为 | **否** |
| 本 Step 新增文件 | 仅本文档 |
| 本 Step 是否读取应用源码逻辑 | **否**（只读取工程接线、目录清单、IGNORE 规则） |

### 0.1 审计使用的方法与命令（可复核）

| 目的 | 命令/方法 |
| --- | --- |
| 版本控制事实 | `git ls-files`、`git status --ignored --porcelain`、`git check-ignore -v`、`git grep -c "F:\\"` |
| 工程文件事实 | 直接读取 `.sln` / `.vcxproj` / `.vcxproj.filters` / `PropertySheet.props` |
| 依赖路径存在性 | `Test-Path` 逐条检测 |
| 依赖路径实际被消费 | 读取 `Server/ChatServer/ChatServer/x64/Debug/ChatServer.tlog/*.tlog`（M0 构建产物） |
| Windows SDK 实际解析 | 在 tlog 中检索 `Windows Kits\10\(Include\|Lib)\<version>` |
| 编译器语言标准 | 在 `CL.command.1.tlog` 中检索 `/std:` |
| 属性表计数 | 解析 `PropertySheet.props` XML 文本并逐节 split（脚本置于 `%TEMP%`，不进入仓库） |

**说明**：`ChatServer.tlog` 与本次审计所需的其他构建中间产物，均产生于 M0 构建（`2026-09-18 22:2x`），其目录被 `.gitignore` 忽略。文中引用它们时，作为「已发生的实测结果」而非「本次 Step 新执行的结果」。

---

## 1. 范围与基线（Scope and Baseline）

### 1.1 基线记录

| 项目 | 值 |
| --- | --- |
| Solution | `Server\ChatServer\ChatServer.sln` |
| Configuration / Platform | `Debug` / `x64` |
| Build target | `Rebuild` |
| Visual Studio | Visual Studio Community 2022，`17.14.36603.0`，安装路径 `G:\vs` |
| MSBuild | `G:\vs\MSBuild\Current\Bin\amd64\MSBuild.exe`，版本 `17.14.23.42201` |
| PlatformToolset | `v143` |
| 结果 | PASS（exit code `0`） |
| 错误数 | `0` |
| 警告数 | `46` |
| 耗时 | `00:01:28.15` |
| 产物 | `Server/ChatServer/x64/Debug/ChatServer.exe`，`19609088` B，`2026-09-18 22:22:00` |
| 起始工作树 | clean |

### 1.2 M0 证明了什么

- **【事实】** `Debug|x64` 在**该开发机**上、**该工作树状态**下，可以完整编译并链接出 `ChatServer.exe`。
- **【事实】** `PropertySheet.props` 的接线在该配置下**确实生效**（证据见第 4 节与第 5 节：`/MDd`、`/LIBPATH F:\...`、F: 依赖头文件被读取、post-build 复制了 3 个文件）。
- **【事实】** 现有源码在该配置下**没有编译错误**，只有 46 条警告。

### 1.3 M0 没有证明什么

- **【事实】** M0 **没有**证明另外三种配置（`Release|x64`、`Debug|Win32`、`Release|Win32`）可构建。本 Step 也未构建它们。
- **【事实】** M0 **没有**证明可移植性。构建依赖 31 条机器绝对路径（第 6 节）与若干**未被版本控制**的文件（第 8 节）。
- **【事实】** M0 **没有**证明"干净克隆即可构建"。其通过前提包括了机器上已存在、但**不在仓库中**的生成产物（`message.pb.cc` 等）。
- **【事实】** M0 **没有**提供任何运行期验证。本 Step 也未运行 `ChatServer.exe`。

**结论用语**：本 Step 一律使用「`Debug|x64` 在 M0 机器上已验证，其余配置与干净机器可移植性保持未验证」，不使用「构建系统已损坏」或「构建可复现」这类无证据的表述。

### 1.4 Step 2.1 回归验证（同配置、同源码）

**【事实】** 在写完本文档后执行了一次回归构建，环境与命令与 M0 相同：

| 项 | M0（2026-09-18） | Step 2.1（2026-09-20） |
| --- | --- | --- |
| Configuration / Platform | `Debug` / `x64` | `Debug` / `x64` |
| Build target | `Rebuild` | `Rebuild` |
| exit code | `0` | `0` |
| 错误数 | `0` | `0` |
| 警告数 | `46` | `46` |
| 耗时 | `00:01:28.15` | `00:01:37.16` |
| `ChatServer.exe` 大小 | `19609088` B | `19609088` B |
| `ChatServer.exe` SHA256 | `e23dc7fa910c54d3ba262eab8b3ef6729aef2313e7907df02c136dc62eff583a` | `bc3b316345dbd03e72d49eaca218aaa99c806d2cadc0fdb7c681655e56c164b2` |
| 产物时间戳 | `2026-09-18 22:22:00` | `2026-09-20 10:03:02` |

**【判读】**

1. 警告数与错误数与 M0 **完全一致**（`46` / `0`），耗时同量级、产物大小完全相同：本 Step 未引入任何构建行为变化，也未导致警告集合变动。
2. 两次构建的 `ChatServer.exe` **SHA256 不同**（尺寸相同），说明当前 Debug 构建**不具备字节级可复现性**。该现象与「同一源码两次构建产物位元级一致」这一更强的可复现性主张**相冲突**，故本文**不**做该主张，并将其记录为缺口 G21。
3. 本次回归是**回归验证**（同一配置仍可构建），**不是**新的可移植性声明。

---

## 2. Solution / Project 拓扑（Solution / Project Topology）

### 2.1 Solution 文件

**【事实】** `Server/ChatServer/ChatServer.sln`（读取自仓库当前内容）：

| 属性 | 值 |
| --- | --- |
| Solution File Format Version | `12.00` |
| Header comment | `# Visual Studio Version 17` |
| `VisualStudioVersion` | `17.14.36603.0 d17.14` |
| `MinimumVisualStudioVersion` | `10.0.40219.1` |
| Project 数量 | **1** |
| Project 条目 | `ChatServer` → `ChatServer\ChatServer.vcxproj` |
| Project GUID | `{5DDC3CD8-B71A-4FE5-BED9-3B22250CBD1E}` |
| Solution GUID | `{4660A743-7641-4F26-A4C3-0A81C79B2A33}` |
| `HideSolutionNode` | `FALSE` |
| Solution 平台 | `x64`、`x86` |

### 2.2 Project 文件

**【事实】** `Server/ChatServer/ChatServer/ChatServer.vcxproj`：

| 属性 | 值 |
| --- | --- |
| `VCProjectVersion` | `17.0` |
| `Keyword` | `Win32Proj` |
| `RootNamespace` | `ChatServer` |
| `WindowsTargetPlatformVersion` | `10.0`（定义在 `Label="Globals"` 的 `PropertyGroup`，对全部配置生效） |
| `ConfigurationType` | `Application`（四个配置均为 `Application`） |
| `CharacterSet` | `Unicode`（四个配置均为 `Unicode`） |
| `SubSystem` | `Console`（四个配置均为 `Console`） |
| 源文件登记方式 | 显式 `ClCompile` / `ClInclude` 项（**不使用通配符、不使用 `*.proto` 自动生成**） |
| CustomBuild / 代码生成步骤 | **无** |
| `ClCompile` 项数 | **15** |
| `ClInclude` 项数 | **17** |
| `None` 项 | `config.ini`、`message.proto` |

**【事实】** `Server/ChatServer/ChatServer/ChatServer.vcxproj.filters` 定义 3 个 Filter（中文名）：`源文件`、`头文件`、`资源文件`；15 个 `ClCompile` 与 17 个 `ClInclude` 与 vcxproj 一一对应（集合一致）。

### 2.3 参与权威构建的 Project

**【事实】** 权威 ChatServer 基线构建**只有 1 个项目**：`ChatServer\ChatServer.vcxproj`。Solution 中不存在第二个项目，也不存在项目间依赖。

### 2.4 仓库中的其他 Solution（边界声明）

**【事实】** 仓库中另有 3 个 solution：`Server/ChatServer2/ChatServer2.sln`、`Server/GateServer/GateServer.sln`、`Server/StatusServer/StatusServer.sln`。`Server/VerifyServer` 无 `.sln`。

**【事实】** 这 4 个 solution 各自带一份 `PropertySheet.props`，且**四份文件的 SHA256 完全相同**：

```text
BF7EF3A674A20501706368CF0F0BEAAEAB18B1BFFD46E81BF93BAD3C2579F8D0
  Server/ChatServer/ChatServer/PropertySheet.props
  Server/ChatServer2/ChatServer2/PropertySheet.props
  Server/GateServer/GateServer/PropertySheet.props
  Server/StatusServer/StatusServer/PropertySheet.props
```

**【判读】** 依赖契约在仓库中是**同一份内容被复制 4 次**，而不是被共享/继承。任何依赖路径调整都需要同步 4 个文件（或改为共享）。**本 Step 不修改其中任何一个。**

**【事实】ChatServer2 的处理边界**：按 Step 1.6 §22 与 Step 2.1 任务约束，`ChatServer2` 在本 Step **仅作事实记录，不分析、不修改、不纳入构建验证**。

### 2.5 仓库中不存在的构建基础设施

**【事实】** 全仓库检索（含被忽略目录）均未发现：

| 设施 | 状态 |
| --- | --- |
| `CMakeLists.txt` / `*.cmake` | 不存在 |
| `vcpkg.json` / `conanfile.txt` | 不存在 |
| `Makefile` | 不存在 |
| `Directory.Build.props` / `Directory.Build.targets` | 不存在 |
| `test/` / `tests/` / `gtest/` / `googletest/` | 不存在 |
| 任何 C++ 单元测试工程 | 不存在 |

---

## 3. 配置矩阵（Configuration Matrix）

### 3.1 Solution → Project 平台映射

**【事实】** `.sln` 声明 4 个 solution 平台，映射到 4 个 project 平台（**平台名不同**：solution 用 `x86`，project 用 `Win32`）：

| Solution 配置 | Project `ActiveCfg` | Project `Build.0` |
| --- | --- | --- |
| `Debug\|x64` | `Debug\|x64` | `Debug\|x64` |
| `Debug\|x86` | `Debug\|Win32` | `Debug\|Win32` |
| `Release\|x64` | `Release\|x64` | `Release\|x64` |
| `Release\|x86` | `Release\|Win32` | `Release\|Win32` |

**【事实】** `.vcxproj` 的 `ProjectConfigurations` 声明 4 个：`Debug|Win32`、`Release|Win32`、`Debug|x64`、`Release|x64`。

**【判读】** 命令行若使用 `/p:Platform=x86`，由 solution 层映射到 project 的 `Win32`；若直接对 `.vcxproj` 使用 `/p:Platform=x86`，则因 project 不存在该平台而失败。因此配置命名时必须区分「solution 平台」与「project 平台」。

### 3.2 四配置静态对比

**【事实】** 以下全部来自静态读取工程文件，**不是**构建结果：

| 项目 | `Debug\|Win32` | `Release\|Win32` | `Debug\|x64` | `Release\|x64` |
| --- | --- | --- | --- | --- |
| `ConfigurationType` | Application | Application | Application | Application |
| `PlatformToolset` | `v143` | `v143` | `v143` | `v143` |
| `UseDebugLibraries` | `true` | `false` | `true` | `false` |
| `WholeProgramOptimization` | 未设置 | `true` | 未设置 | `true` |
| `CharacterSet` | `Unicode` | `Unicode` | `Unicode` | `Unicode` |
| `WindowsTargetPlatformVersion` | `10.0` | `10.0` | `10.0` | `10.0` |
| `WarningLevel` | `Level3` | `Level3` | `Level3` | `Level3` |
| `SDLCheck` | `true` | `true` | `true` | `true` |
| `ConformanceMode` | `true` | `true` | `true` | `true` |
| `PreprocessorDefinitions` | `WIN32;_DEBUG;_CONSOLE` | `WIN32;NDEBUG;_CONSOLE` | `_DEBUG;_CONSOLE` | `NDEBUG;_CONSOLE` |
| `FunctionLevelLinking` | 未设置 | `true` | 未设置 | `true` |
| `IntrinsicFunctions` | 未设置 | `true` | 未设置 | `true` |
| `SubSystem` | `Console` | `Console` | `Console` | `Console` |
| `GenerateDebugInformation` | `true` | `true` | `true` | `true` |
| `RuntimeLibrary`（工程内） | 未设置 | 未设置 | 未设置 | 未设置 |
| 导入 `PropertySheet.props` | **否** | **否** | **是** | **否** |
| `LanguageStandard` | 未设置 | 未设置 | 未设置 | 未设置 |

**【事实】** `WIN32` 宏只在两个 `Win32` 配置中定义；两个 `x64` 配置**不定义** `WIN32`。

**【事实】** 四个配置的 `ItemDefinitionGroup` 中都**只**设置了 `ClCompile`（WarningLevel/SDLCheck/PreprocessorDefinitions/ConformanceMode）与 `Link`（SubSystem/GenerateDebugInformation）。**没有任何** `AdditionalIncludeDirectories`、`AdditionalDependencies`、`AdditionalLibraryDirectories`、`RuntimeLibrary`、`PostBuildEvent` 出现在 `.vcxproj` 中——这些**全部**由 `PropertySheet.props` 提供（见第 4 节）。

### 3.3 本 Step 实际构建的配置

| 配置 | 本 Step 是否构建 |
| --- | --- |
| `Debug\|x64` | **是**（回归验证，见第 8 节） |
| `Release\|x64` | 否（未构建，未验证） |
| `Debug\|x86` / `Debug\|Win32` | 否（未构建，未验证） |
| `Release\|x86` / `Release\|Win32` | 否（未构建，未验证） |

**【判读】** 因为 `PropertySheet.props` 仅被 `Debug|x64` 导入，而该 props 承载了**全部**外部依赖的 include/lib 配置，所以在静态层面可以确定：另外三个配置**缺少**这些依赖路径。至于它们是否还能凭借其他机制（例如工具集默认路径、环境变量、已存在的 `INCLUDE`/`LIB`）偶然编译成功，**本 Step 未验证，不做断言**。

---

## 4. Property Sheet 接线（Property Sheet Wiring）

### 4.1 导入位置与条件

**【事实】** `ChatServer.vcxproj` 中 `PropertySheet.props` 的导入出现在下行（第 68 行），且位于 `Condition="'$(Configuration)|$(Platform)'=='Debug|x64'"` 的 `ImportGroup` 内部：

```xml
<ImportGroup Label="PropertySheets" Condition="'$(Configuration)|$(Platform)'=='Debug|x64'">
  <Import Project="$(UserRootDir)\Microsoft.Cpp.$(Platform).user.props" Condition="exists(...)" Label="LocalAppDataPlatform" />
  <Import Project="PropertySheet.props" />
</ImportGroup>
```

**【事实】** 四个 `ImportGroup` 的完整情况：

| `ImportGroup` 的 Condition | 导入 `Microsoft.Cpp.$(Platform).user.props` | 导入 `PropertySheet.props` |
| --- | --- | --- |
| `Debug\|Win32` | 是（带 `exists()` 条件） | **否** |
| `Release\|Win32` | 是（带 `exists()` 条件） | **否** |
| `Debug\|x64` | 是（带 `exists()` 条件） | **是** |
| `Release\|x64` | 是（带 `exists()` 条件） | **否** |

**【事实】** `PropertySheet.props` 中的导入项本身**不带任何 `Condition`**。也就是说，它一旦被导入就**无条件**作用于当前配置。因此「只对 `Debug|x64` 生效」完全由上面的 `ImportGroup` 条件决定。

**【事实】** 该 `Import` 使用**相对路径** `PropertySheet.props`，即相对于 `.vcxproj` 所在目录 `Server/ChatServer/ChatServer/`。因此该文件是**仓库相对路径引用**，不构成可移植性问题；构成可移植性问题的是**它内部的值**（第 6 节）。

### 4.2 求值顺序与覆盖关系

**【事实】** `ChatServer.vcxproj` 中相关元素的**文件顺序**为：

```text
1. PropertyGroup Label="Globals"（含 WindowsTargetPlatformVersion）
2. Import $(VCTargetsPath)\Microsoft.Cpp.Default.props
3. 四个 PropertyGroup Label="Configuration"（按条件）
4. Import $(VCTargetsPath)\Microsoft.Cpp.props
5. ImportGroup Label="PropertySheets"（按条件，Debug|x64 内含 PropertySheet.props）
6. PropertyGroup Label="UserMacros"
7. 四个 ItemDefinitionGroup（按条件）
8. ClCompile / ClInclude / None ItemGroup
9. Import $(VCTargetsPath)\Microsoft.Cpp.targets
10. ImportGroup Label="ExtensionTargets"
```

**【判读】** MSBuild 对 `ItemDefinitionGroup` 的同名元数据采用「后出现者覆盖先出现者」的累积语义。由于 `PropertySheet.props` 的导入（步骤 5）位于 `.vcxproj` 自身 `ItemDefinitionGroup`（步骤 7）**之前**，`.vcxproj` 中显式写出的值具有更高优先级。

**【事实】** 该判读的后果在本次审计中**得到实测支持**：`.vcxproj` 的 `<Link>` 只设置 `SubSystem` 与 `GenerateDebugInformation`，因此 `PropertySheet.props` 中的 `AdditionalDependencies` / `AdditionalLibraryDirectories` **未被覆盖**，实际进入了链接命令行（第 5.4 节）。同理 `RuntimeLibrary` 只在 props 中出现，未被覆盖，实际生效为 `/MDd`（第 7.4 节）。

### 4.3 `PropertySheet.props` 内部结构（按行）

**【事实】** 文件共 24 行，结构如下：

| 行 | 内容 | 作用域 |
| --- | --- | --- |
| 6 | `<IncludePath>` | `PropertyGroup`（全局，对导入它的配置生效） |
| 7 | `<LibraryPath>` | `PropertyGroup`（全局） |
| 11 | `<RuntimeLibrary>MultiThreadedDebugDLL</RuntimeLibrary>` | `ItemDefinitionGroup/ClCompile` |
| 12 | `<AdditionalIncludeDirectories>` | `ItemDefinitionGroup/ClCompile` |
| 15 | `<AdditionalDependencies>` | `ItemDefinitionGroup/Link` |
| 16 | `<AdditionalLibraryDirectories>` | `ItemDefinitionGroup/Link` |
| 18–21 | `<PostBuildEvent><Command>` | `ItemDefinitionGroup`（2 条 `xcopy`） |

**【事实】** `IncludePath` / `LibraryPath` 采用「前置 + `$(IncludePath)` / `$(LibraryPath)` 收尾」的写法，即**保留工具集默认值**并追加机器路径。`AdditionalIncludeDirectories` / `AdditionalLibraryDirectories` / `AdditionalDependencies` 同样以 `%(...)` 收尾并保留继承值。

### 4.4 关键判读：该 props 在内容上就是 Debug 专用

**【事实】** props 中含 4 处 Debug 专用设定：

| 行 | 内容 | 含义 |
| --- | --- | --- |
| 11 | `RuntimeLibrary = MultiThreadedDebugDLL` | Debug CRT（`/MDd`） |
| 15 | `libprotobufd.lib` | protobuf Debug 库（`d` 后缀） |
| 15 | `zlibstaticd.lib` | zlib Debug 静态库（`d` 后缀） |
| 15 | `debug\mysqlcppconn.lib`、`debug\mysqlcppconn8.lib` | 相对子目录名即 `debug\` |

**【事实】** `AdditionalLibraryDirectories` 的 18 条机器路径**全部以 `\Debug` 结尾**。

**【判读】** 因此「`Release` 配置缺少依赖路径」并不只是一处 import 条件书写不全的问题：**该 props 的内容本身带有 Debug 语义**（Debug CRT、Debug 后缀库、Debug 库目录）。若把同一份 props 直接接入 `Release`，会引入 Release 配置使用 Debug CRT 与 Debug 库的组合，属于**新的构建契约问题**，而不是简单修复。

**【判读的边界】** 上述判读只基于仓库文件内容，**未经 `Release` 构建验证**。本 Step 不修改该文件，也不对「Release 应该如何配置」给出结论。

---

## 5. 编译/链接依赖清单（Compile / Link Dependency Inventory）

本节回答的是**构建契约**问题：每个外部依赖**从哪里获得头文件、从哪里获得库、以何种形式进入构建**。

### 5.1 汇总表

| 依赖族 | 头文件来源 | 库来源 | 链接库（示例） | 运行期 DLL | 路径性质 |
| --- | --- | --- | --- | --- | --- |
| Boost（Asio 等） | `IncludePath` 中的 `F:\minGw\boost_1_89_0` | `LibraryPath` 中的 `F:\minGw\boost_1_89_0\stage\lib` | **未**出现在 `AdditionalDependencies` 中；实测链接命令行的 72 个库名里也未出现任何 Boost 库 | 无（输出目录中未观察到） | 机器绝对 |
| gRPC / protobuf / abseil / re2 / BoringSSL / zlib / c-ares / upb | `AdditionalIncludeDirectories` 5 条 `F:\cppsoft\grpc\...` | `AdditionalLibraryDirectories` 18 条 `F:\cppsoft\grpc\visualpro\...` | `gpr.lib`、`grpc.lib`、`grpc++.lib`、`grpc++_reflection.lib`、`libprotobufd.lib`、`address_sorting.lib`、`upb.lib`、`ssl.lib`、`crypto.lib`、`re2.lib`、`zlibstaticd.lib`、`cares.lib`、54 个 `absl_*.lib` | 无（均为 `.lib` 静态导入） | 机器绝对 |
| hiredis（Redis Windows 端口） | `IncludePath` 中的 `F:\cppsoft\reids\deps\hiredis`（其头文件再以 `../../src/Win32_Interop/...` 相对包含） | `LibraryPath` 中的 `F:\cppsoft\reids\lib` | `hiredis.lib`、`Win32_Interop.lib` | 无 | 机器绝对 |
| libjson | `IncludePath` 中的 `F:\cppsoft\libjson\include` | `LibraryPath` 中的 `F:\cppsoft\libjson\lib` | `json_vc71_libmtd.lib` | 无 | 机器绝对 |
| MySQL Connector/C++ | `IncludePath` 中的 `F:\cppsoft\mysql_connector\include` | `LibraryPath` 中的 `F:\cppsoft\mysql_connector\lib64\vs14`（库名带 `debug\` 前缀） | `debug\mysqlcppconn.lib`、`debug\mysqlcppconn8.lib` | **`mysqlcppconn-9-vs14.dll`、`mysqlcppconn8-2-vs14.dll`** | 机器绝对 |
| Win32 系统库 | Windows SDK（工具集提供） | Windows SDK（工具集提供） | `ws2_32.lib` | 系统自带 | 工具集提供 |
| C/C++ 运行库、SDK、MSVC STL | MSVC 工具集 `G:\vs\VC\Tools\MSVC\14.44.35207` + Windows SDK `G:\Windows Kits\10` | 同上 | 由工具集隐式链接 | 无（`/MDd` 使用系统 UCRT） | 工具集提供（非仓库配置） |

### 5.2 计数（由脚本解析 `PropertySheet.props` 得出）

【事实】

| 项 | 数量 |
| --- | --- |
| `IncludePath` 条目 | 5（4 条机器绝对 + `$(IncludePath)`） |
| `LibraryPath` 条目 | 5（4 条机器绝对 + `$(LibraryPath)`） |
| `AdditionalIncludeDirectories` 条目 | 6（5 条机器绝对 + `%(AdditionalIncludeDirectories)`） |
| `AdditionalLibraryDirectories` 条目 | 19（18 条机器绝对 + `%(AdditionalLibraryDirectories)`） |
| `AdditionalDependencies` 条目 | 73（72 个库名 + `%(AdditionalDependencies)`） |
| 其中 `absl_*.lib` | 54 |
| 其中非 absl 库名 | 18 |
| **机器绝对路径合计（去重）** | **31** |
| 涉及的盘符 | 仅 `F:` |

【事实】18 个非 absl 库名（按出现顺序）：

```text
json_vc71_libmtd.lib, libprotobufd.lib, gpr.lib, grpc.lib, grpc++.lib,
grpc++_reflection.lib, address_sorting.lib, ws2_32.lib, cares.lib,
zlibstaticd.lib, upb.lib, ssl.lib, crypto.lib, re2.lib,
Win32_Interop.lib, hiredis.lib, debug\mysqlcppconn.lib, debug\mysqlcppconn8.lib
```

### 5.3 依赖路径的解析风格

【事实】三类混用，必须在文档中分清：

| 风格 | 例子 | 解析依据 |
| --- | --- | --- |
| 仓库相对 | `PropertySheet.props`、`config.ini`、`message.proto`、`*.dll`（post-build 源） | 相对于 `.vcxproj` 所在目录（`$(ProjectDir)`） |
| 工具集提供 | MSVC STL、Windows SDK、`ws2_32.lib` | 由 `Microsoft.Cpp.props`/工具集解析 |
| 机器绝对 | 全部 31 条 `F:\...` | 直接写死盘符与目录名 |

【事实】`IncludePath` / `LibraryPath` 通过 MSBuild 的 `IncludePath`/`LibraryPath` 属性影响传入编译器的 `INCLUDE`/`LIB` 环境变量；`AdditionalIncludeDirectories` / `AdditionalLibraryDirectories` 则直接进入命令行。两者在本 Step 中**都观察到生效**（见 5.4）。

### 5.4 实测证据（来自 M0 构建的 tlog）

【事实】`CL.command.1.tlog` 中出现 `AdditionalIncludeDirectories` 的条目：

```text
/I"F:\CPPSOFT\GRPC\THIRD_PARTY\ABSEIL-CPP"
```

【事实】`CL.read.1.tlog` 中观察到的依赖根（去重后）包括：

```text
F:\MINGW                                     ← Boost
F:\CPPSOFT\REIDS\DEPS\HIREDIS                ← hiredis
F:\CPPSOFT\REIDS\SRC\WIN32_INTEROP           ← Redis Windows 端口（经相对包含到达）
F:\CPPSOFT                                   ← libjson 等
```

【事实】`link.command.1.tlog` 中出现 `/LIBPATH:F:\...`（18 条 `AdditionalLibraryDirectories`）与全部 `AdditionalDependencies` 库名（与上面 18 个非 absl 库名一致）。

【事实】编译器使用 `/MDd`，对应 `RuntimeLibrary = MultiThreadedDebugDLL`。

【判读】以上四项共同证明：`PropertySheet.props` 的 `PropertyGroup`、`ClCompile`、`Link` 三部分在 `Debug|x64` 下**均已生效**，不存在「写了但没被使用」的情况。

### 5.5 hiredis 的相对结构依赖

【事实】`F:\cppsoft\reids\deps\hiredis` 下的头文件通过**相对路径**包含 Win32 端口实现头：

```text
hiredis.h:39          #include "../../src/Win32_Interop/win32_types_hiredis.h"
sds.h:37,38           #include "../../src/Win32_Interop/Win32_Portability.h"
                      #include "../../src/Win32_Interop/win32_types_hiredis.h"
win32_hiredis.h:26-29 #include "../../src/Win32_Interop/Win32_Portability.h"
                      #include "../../src/Win32_Interop/Win32_types_hiredis.h"
                      #include "../../src/Win32_Interop/Win32_Error.h"
                      #include "../../src/Win32_Interop/Win32_FDAPI.h"
```

【事实】`F:\cppsoft\reids\deps\hiredis\win32_interop` **不存在**；实际存在的是 `F:\cppsoft\reids\src\win32_interop`。

【判读】因此 hiredis 依赖**不是一个目录**，而是 `deps/hiredis` 与 `src/Win32_Interop` 之间的**相对布局**。任何「只把 hiredis 头文件拷到别处」的迁移方式都会破坏编译。这是第 10 节各选项中必须考虑的约束。

### 5.6 关于 per-user / 全局注入的排除性检查

【事实】`ImportGroup` 中导入的 `Microsoft.Cpp.$(Platform).user.props` 位于 `$(UserRootDir)`。本机实测：

| 文件 | 大小 | 修改时间 | 内容 |
| --- | --- | --- | --- |
| `%LOCALAPPDATA%\Microsoft\MSBuild\v4.0\Microsoft.Cpp.x64.user.props` | 168 B | 2009-08-31 | 空模板（无任何属性） |
| `Microsoft.Cpp.Win32.user.props` | 168 B | 2009-08-31 | 空模板 |
| `Microsoft.Cpp.Itanium.user.props` | 168 B | 2009-08-31 | 空模板 |

【事实】`Server/ChatServer/ChatServer/ChatServer.vcxproj.user` **存在**（被 `.gitignore` 的 `*.vcxproj.user` 忽略），内容为空 `PropertyGroup`：

```xml
<PropertyGroup />
```

【判读】依赖路径**不是**由这些 out-of-repo / 本地文件注入的，可归因于仓库内的 `PropertySheet.props` 单点。该结论也排除了「本机某处隐藏配置使其可用」这一可能来源。

---

## 6. 机器绝对路径清单（Machine-Specific Paths）

### 6.1 清单

**【事实】** `Server/ChatServer/ChatServer/PropertySheet.props` 中出现的**全部**机器绝对路径（共 31 条，全部位于 `F:` 盘）：

`IncludePath`（4 条）：

```text
F:\cppsoft\mysql_connector\include
F:\cppsoft\reids\deps\hiredis
F:\cppsoft\libjson\include
F:\minGw\boost_1_89_0
```

`LibraryPath`（4 条）：

```text
F:\cppsoft\mysql_connector\lib64\vs14
F:\cppsoft\reids\lib
F:\minGw\boost_1_89_0\stage\lib
F:\cppsoft\libjson\lib
```

`AdditionalIncludeDirectories`（5 条）：

```text
F:\cppsoft\grpc\include
F:\cppsoft\grpc\third_party\protobuf\src
F:\cppsoft\grpc\third_party\abseil-cpp
F:\cppsoft\grpc\third_party\address_sorting\include
F:\cppsoft\grpc\third_party\re2
```

`AdditionalLibraryDirectories`（18 条）：

```text
F:\cppsoft\grpc\visualpro\third_party\re2\Debug
F:\cppsoft\grpc\visualpro\third_party\abseil-cpp\absl\types\Debug
F:\cppsoft\grpc\visualpro\third_party\abseil-cpp\absl\synchronization\Debug
F:\cppsoft\grpc\visualpro\third_party\abseil-cpp\absl\status\Debug
F:\cppsoft\grpc\visualpro\third_party\abseil-cpp\absl\random\Debug
F:\cppsoft\grpc\visualpro\third_party\abseil-cpp\absl\flags\Debug
F:\cppsoft\grpc\visualpro\third_party\abseil-cpp\absl\debugging\Debug
F:\cppsoft\grpc\visualpro\third_party\abseil-cpp\absl\hash\Debug
F:\cppsoft\grpc\visualpro\third_party\abseil-cpp\absl\container\Debug
F:\cppsoft\grpc\visualpro\third_party\boringssl-with-bazel\Debug
F:\cppsoft\grpc\visualpro\third_party\abseil-cpp\absl\numeric\Debug
F:\cppsoft\grpc\visualpro\third_party\abseil-cpp\absl\time\Debug
F:\cppsoft\grpc\visualpro\third_party\abseil-cpp\absl\base\Debug
F:\cppsoft\grpc\visualpro\third_party\abseil-cpp\absl\strings\Debug
F:\cppsoft\grpc\visualpro\third_party\protobuf\Debug
F:\cppsoft\grpc\visualpro\third_party\zlib\Debug
F:\cppsoft\grpc\visualpro\Debug
F:\cppsoft\grpc\visualpro\third_party\cares\cares\lib\Debug
```

### 6.2 这些路径在本机的存在性

**【事实】** 上述路径的代表性子集逐条 `Test-Path` 结果：**全部存在**。另核对了 6 个被链接的 `.lib` 实际文件，也全部存在：

| 文件 | 存在 |
| --- | --- |
| `F:\cppsoft\mysql_connector\lib64\vs14\debug\mysqlcppconn.lib` | 是 |
| `F:\cppsoft\mysql_connector\lib64\vs14\debug\mysqlcppconn8.lib` | 是 |
| `F:\cppsoft\libjson\lib\json_vc71_libmtd.lib` | 是 |
| `F:\cppsoft\reids\lib\hiredis.lib` | 是 |
| `F:\cppsoft\reids\lib\Win32_Interop.lib` | 是 |
| `F:\minGw\boost_1_89_0\stage\lib` | 是（目录） |

**【判读】** 这解释了 M0 为何成功：M0 机器就是这套 `F:\...` 布局的**原始构建机**（或与之等价）。

### 6.3 可移植性风险说明

**【判读】** 风险机制：

1. 路径以盘符 `F:` 开头，**不**相对于仓库、**不**相对于用户目录、**不**由环境变量参数化。
2. 因此在任何不具备等价 `F:\cppsoft\...` 与 `F:\minGw\...` 布局的环境（其他开发者机器、CI、容器）中，这些路径无法解析，`INCLUDE`/`LIB` 与 `/I`、`/LIBPATH` 将指向不存在的目录。
3. 由于项目**没有** CMake/vcpkg/conan 等包管理器声明，也没有 `Directory.Build.props` 之类的集中变量，这些路径**无法**被外部覆盖（唯一的覆盖点是直接修改 `PropertySheet.props`，而它是 4 份复制之一）。
4. 同一份 `PropertySheet.props` 被 4 个 solution 复制引用（第 2.4 节），意味着未来任何迁移必须**同时**处理 4 处，否则会造成 4 份契约分叉。

**【事实】本 Step 未在任何其他机器或干净环境上验证。** 因此本文**不**断言「另一台机器必然失败」；可断言的是：这些路径是**未参数化的机器绑定值**，在缺少等价布局时可以预期无法解析（第 9 节列为 gap）。

### 6.4 明确不做

- 本 Step **未**重写任何路径。
- 本 Step **未**添加环境变量、**未**修改 `PATH`、**未**创建 junction/symlink、**未**把依赖复制进仓库。

---

## 7. 工具链与语言假设（Toolchain / Language Assumptions）

### 7.1 VS / MSBuild

【事实】

| 项 | 值 |
| --- | --- |
| Visual Studio | `Visual Studio Community 2022` |
| `installationVersion` | `17.14.36603.0` |
| `installationPath` | `G:\vs` |
| `productId` | `Microsoft.VisualStudio.Product.Community` |
| `vswhere.exe` | `C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe` |
| MSBuild 可执行 | `G:\vs\MSBuild\Current\Bin\amd64\MSBuild.exe` |
| MSBuild 版本 | `17.14.23.42201` |
| `msbuild` 是否在 `PATH` | **否** |

【判读】权威构建入口必须通过完整路径或 Developer Command Prompt 调用。此外 `.sln` 的 `VisualStudioVersion` 记录为 `17.14.36603.0 d17.14`，与本机 VS 一致；这不是可移植性保障。

### 7.2 `v143` 与 MSVC 工具集

【事实】四个 project 配置均设置 `PlatformToolset = v143`。

【事实】本次 M0 构建实际使用的 MSVC 工具集目录为 `G:\vs\VC\Tools\MSVC\14.44.35207`（由构建警告中的头文件绝对路径 `G:\vs\VC\Tools\MSVC\14.44.35207\include\xutility` 证实）。

【事实】工程**未**设置 `VCToolsVersion` 或 `VCToolsInstallDir`，即**未固定**具体 MSVC 版本。

【判读】`v143` 只固定「工具集主版本族」，实际编译器版本由**机器上安装的 VS 更新级别**决定。因此仓库并未固定编译器补丁版本。

### 7.3 `vswhere` 未涉及

【事实】本 Step 与 M0 均使用 `vswhere.exe` 定位 VS。仓库内**没有**任何脚本封装该定位逻辑（无 `build.bat` / `build.ps1` / `.cmd`）。

### 7.4 Windows SDK：选择器 ≠ 实际版本

【事实】工程只在 `Label="Globals"` 的 `PropertyGroup` 中设置：

```xml
<WindowsTargetPlatformVersion>10.0</WindowsTargetPlatformVersion>
```

【事实】本机安装了两个 SDK 版本：

```text
G:\Windows Kits\10\Include\10.0.22621.0
G:\Windows Kits\10\Include\10.0.26100.0
```

【事实】M0 构建的 tlog 中记录的实际解析结果为：

```text
WINDOWS KITS\10\INCLUDE\10.0.26100.0
WINDOWS KITS\10\LIB\10.0.26100.0
```

【判读】**必须区分**：

| 概念 | 值 | 性质 |
| --- | --- | --- |
| 工程设置 | `10.0` | **选择器**（表示"使用已安装的最新 10.x SDK"） |
| 本机实际解析 | `10.0.26100.0` | 机器相关结果，取决于本机安装了哪些 SDK |

因此「工程固定使用 10.0.26100.0」是**错误**陈述；正确陈述是「工程未固定具体 SDK 版本，本机解析到 `10.0.26100.0`，且本机另有 `10.0.22621.0` 可被安装集合变化所影响」。

【事实】`C:\Program Files (x86)\Windows Kits\10` **不存在**；SDK 安装在与 VS 相同的 `G:` 盘。这也是机器相关事实（工具集提供，非仓库配置）。

### 7.5 C++ 语言标准

【事实】

| 检查 | 结果 |
| --- | --- |
| `.vcxproj` 中 `LanguageStandard` | **未出现** |
| `PropertySheet.props` 中 `LanguageStandard` | **未出现** |
| 任何 `/std:` 出现在 `CL.command.1.tlog` 记录的编译命令行 | **未出现** |

【判读】仓库**没有**固定 C++ 语言标准；实际生效标准完全由 MSVC 工具集的**默认值**决定。由于工具集版本未被固定（7.2），该默认值**也不在仓库控制范围内**。

**【明确不做推断】** 本文**不**断言具体的标准版本号（如 `C++14`/`C++17`），因为工程文件没有声明它、本 Step 也没有做能直接证明它的实验。若后续需要该事实，应以一次显式测量（例如编译期打印 `__cplusplus` 或读取编译器 `/Bv` 输出）来确立。

【事实】四个配置均设置 `ConformanceMode = true`（即 `/permissive-`）。

【判读】`/permissive-` 会收紧部分非标准构造的接受度，但**不**设定语言标准本身。Step 1.6 §7.2 已经基于「标准未固定」这一现状，明确禁止使用 C++17 能力；本文与之一致。

### 7.6 `_WIN32_WINNT` 观察

【事实】M0 构建输出中反复出现以下提示（**不是错误**），涉及 `AsioIOServicePool.cpp`、`ChatServer.cpp`、`CServer.cpp`、`CSession.cpp`、`LogicSystem.cpp`、`MsgNode.cpp`、`UserMgr.cpp`：

```text
Please define _WIN32_WINNT or _WIN32_WINDOWS appropriately. For example:
- add -D_WIN32_WINNT=0x0601 to the compiler command line; or
- add _WIN32_WINNT=0x0601 to your project's Preprocessor Definitions.
Assuming _WIN32_WINNT=0x0601 (i.e. Windows 7 target).
```

【事实】该宏在 `.vcxproj` 与 `PropertySheet.props` 中**均未定义**（`PreprocessorDefinitions` 只有 `WIN32;_DEBUG;_CONSOLE` / `WIN32;NDEBUG;_CONSOLE` / `_DEBUG;_CONSOLE` / `NDEBUG;_CONSOLE`）。

【判读】实际生效目标为 Windows 7（`0x0601`），由 Boost.Asio 的**回退假设**决定，而非仓库声明。这属于隐式工具链假设：若将来 Boost 版本变更或 SDK 行为变化，该假设可能改变。**本 Step 不修复**（明确禁止）。

---

## 8. 运行期与 Post-Build 契约（Runtime / Post-Build Contract）

### 8.1 Post-Build 行为

【事实】`PropertySheet.props` 第 18–21 行：

```xml
<PostBuildEvent>
  <Command>xcopy $(ProjectDir)config.ini  $(SolutionDir)$(Platform)\$(Configuration)\   /y
xcopy $(ProjectDir)*.dll   $(SolutionDir)$(Platform)\$(Configuration)\   /y</Command>
</PostBuildEvent>
```

【事实】变量解析（在 `ChatServer.sln` 的 `Debug|x64` 构建下）：

| MSBuild 变量 | 解析值 |
| --- | --- |
| `$(ProjectDir)` | `Server\ChatServer\ChatServer\` |
| `$(SolutionDir)` | `Server\ChatServer\`（`.sln` 所在目录，含尾随 `\`） |
| `$(Platform)` | `x64` |
| `$(Configuration)` | `Debug` |
| 目标目录 | `Server\ChatServer\x64\Debug\` |

【事实】M0 构建日志中出现：

```text
ChatServer.vcxproj -> ...\Server\ChatServer\x64\Debug\ChatServer.exe
...\ChatServer\ChatServer\config.ini
复制了 1 个文件
...\mysqlcppconn-9-vs14.dll
...\mysqlcppconn8-2-vs14.dll
复制了 2 个文件
```

【判读】Post-build 使用 `$(SolutionDir)`，其正确性**依赖以 `.sln` 为构建入口**。若改为直接构建 `.vcxproj`，`$(SolutionDir)` 的取值语义会不同，复制目标可能不再落在 `Server/ChatServer/x64/Debug/`。**本 Step 未实验该场景**，仅指出该耦合存在。

### 8.2 构建产物（`Debug|x64`）

【事实】`Server/ChatServer/x64/Debug/` 目录内容（M0 构建后）：

| 文件 | 大小 | 时间戳 | 来源 |
| --- | --- | --- | --- |
| `ChatServer.exe` | 19609088 | 2026-09-18 22:22:00 | 链接产物 |
| `ChatServer.pdb` | 94949376 | 2026-09-18 22:22:01 | 链接产物 |
| `config.ini` | 455 | 2025-11-18 16:14:12 | post-build 复制（源保留自身时间戳） |
| `mysqlcppconn-9-vs14.dll` | 16142848 | 2023-12-15 19:36:25 | post-build 复制 |
| `mysqlcppconn8-2-vs14.dll` | 9538048 | 2023-12-15 20:11:34 | post-build 复制 |

【判读】输出目录中**只有 2 个 DLL**，与第 5.1 节的静态分析一致：gRPC/protobuf/abseil/re2/SSL/zlib/c-ares/hiredis/libjson 均以 `.lib` 形式链接，因此**不需要**运行期 DLL（按当前链接方式）。

### 8.3 Git 跟踪状态（构建契约的关键部分）

【事实】`git check-ignore -v` 结果：

| 路径 | 忽略规则 | 是否被版本控制 |
| --- | --- | --- |
| `Server/ChatServer/ChatServer/config.ini` | `.gitignore:102  Server/**/config.ini` | **否** |
| `Server/ChatServer/ChatServer/message.pb.cc` | `.gitignore:56` | **否** |
| `Server/ChatServer/ChatServer/message.pb.h` | `.gitignore:55` | **否** |
| `Server/ChatServer/ChatServer/message.grpc.pb.cc` | `.gitignore:58` | **否** |
| `Server/ChatServer/ChatServer/message.grpc.pb.h` | `.gitignore:57` | **否** |
| `Server/ChatServer/ChatServer/mysqlcppconn-9-vs14.dll` | `.gitignore:52  *.dll` | **否** |
| `Server/ChatServer/ChatServer/mysqlcppconn8-2-vs14.dll` | `.gitignore:52  *.dll` | **否** |
| `Server/ChatServer/ChatServer/ChatServer.vcxproj.user` | `.gitignore:48  *.vcxproj.user` | **否** |
| `Server/ChatServer/PropertySheet.props`（**在 `ChatServer/` 子目录**） | — | **是** |
| `Server/ChatServer/ChatServer/message.proto` | — | **是** |
| `Server/ChatServer/ChatServer/start.bat` | — | **是** |

【事实】`git status --ignored --porcelain Server/ChatServer/ChatServer` 的输出：

```text
!! Server/ChatServer/ChatServer/ChatServer.vcxproj.user
!! Server/ChatServer/ChatServer/config.ini
!! Server/ChatServer/ChatServer/message.grpc.pb.cc
!! Server/ChatServer/ChatServer/message.grpc.pb.h
!! Server/ChatServer/ChatServer/message.pb.cc
!! Server/ChatServer/ChatServer/message.pb.h
!! Server/ChatServer/ChatServer/mysqlcppconn-9-vs14.dll
!! Server/ChatServer/ChatServer/mysqlcppconn8-2-vs14.dll
!! Server/ChatServer/ChatServer/x64/
```

【事实】该目录磁盘上的顶层文件数为 **41**，其中受版本控制 **33**，不受版本控制 **8**（上表前 8 项）。

> 这与 Step 1.6 §3.8 记录的「两侧文件数各 41 个」一致；本文补充：**其中 8 个不在版本控制内**。

### 8.4 关键判读：编译所需的源文件不在仓库中

【事实】`ChatServer.vcxproj` 把以下文件登记为编译单元：

```xml
<ClCompile Include="message.grpc.pb.cc" />
<ClCompile Include="message.pb.cc" />
```

【事实】这两个 `.cc` 及其对应 `.h` **均被 `.gitignore` 忽略**（8.3）。

【事实】工程中**没有** `CustomBuild` 步骤调用 `protoc`；仓库中唯一的生成入口是 `Server/ChatServer/ChatServer/start.bat`，其内容为：

```bat
set PROTOC_PATH=F:\cppsoft\grpc\visualpro\third_party\protobuf\Debug\protoc.exe
set GRPC_PLUGIN_PATH=F:\cppsoft\grpc\visualpro\Debug\grpc_cpp_plugin.exe
set PROTO_FILE=message.proto
%PROTOC_PATH% -I="." --grpc_out="." --plugin=protoc-gen-grpc="%GRPC_PLUGIN_PATH%" "%PROTO_FILE%"
%PROTOC_PATH% --cpp_out=. "%PROTO_FILE%"
```

【事实】磁盘上的生成文件时间戳表明它们是一次历史手工生成的产物：

```text
message.proto        2153 B   2025-11-17 16:41:39   （受版本控制）
start.bat             400 B   2025-11-17 16:45:39   （受版本控制）
message.pb.cc      194555 B   2025-11-17 16:46:03   （被忽略）
message.pb.h       168034 B   2025-11-17 16:46:03   （被忽略）
message.grpc.pb.cc  27246 B   2025-11-17 16:46:03   （被忽略）
message.grpc.pb.h  107745 B   2025-11-17 16:46:03   （被忽略）
```

【判读】因此在一个**不含这些被忽略文件**的干净克隆中，`Debug|x64` 构建会因找不到 `message.pb.cc` / `message.grpc.pb.cc` 而**在编译阶段失败**，正确流程需要先执行 `start.bat`。而 `start.bat` 自身又依赖两台机器绝对路径下的 `protoc.exe` 与 `grpc_cpp_plugin.exe`。

**【判读的边界说明】** 本 Step **未**执行干净克隆实验，上述为**静态接线推出的预期后果**，不是实测结果。若将来需要实测，应在一个临时克隆目录中验证，且不得改变本仓库。

### 8.5 「构建成功」与「仓库自带可运行」的区别

【事实 + 判读】必须显式区分：

| 命题 | 状态 |
| --- | --- |
| 「`Debug\|x64` 在 M0 机器上构建成功」 | **事实**（exit code 0） |
| 「仓库自身包含编译所需全部输入」 | **不成立**：`message.pb.cc` / `message.grpc.pb.cc` 被忽略（8.4） |
| 「仓库自身包含运行所需全部输入」 | **不成立**：`config.ini` 被忽略，2 个运行期 DLL 被忽略（8.3） |
| 「仓库可提供默认运行配置」 | **无法核实**：`.gitignore` 的「敏感配置」注释段（对应的规则为 `Server/**/config.ini`，位于第 102 行）说明该文件含数据库/Redis/邮箱等敏感信息，故本 Step **未读取其内容**，只记录「该文件存在且被忽略」这一事实 |

【判读】故本 Step 只可断言：构建成功**依赖**机器上已有的（非仓库）生成文件与运行期文件。是否「可运行」不在本 Step 的验证范围（未运行程序）。

---

## 9. 当前可复现性缺口（Current Reproducibility Gaps）

以下均为**已确认、未修复**的事实性缺口。本 Step 不解决任何一项。

| # | 缺口 | 证据 | 影响面 |
| --- | --- | --- | --- |
| G1 | 31 条机器绝对依赖路径写死在 `PropertySheet.props`，未参数化 | 第 6.1 节 | 换机器/CI 无法解析 |
| G2 | 该 props 同一份内容被复制到 4 个工程且**哈希完全相同** | 第 2.4 节 | 未来修改需同步 4 处，易分叉 |
| G3 | `PropertySheet.props` 仅在 `Debug\|x64` 被导入 | 第 4.1 节 | 另外 3 个配置缺少依赖路径 |
| G4 | 该 props 的**内容**本身是 Debug 专用（`/MDd`、`d` 后缀库、`\Debug` 目录） | 第 4.4 节 | 「接入另外 3 个配置」不是简单加一行 import |
| G5 | 编译所需的 `message*.pb.cc/.h` 被忽略且**无自动生成步骤** | 第 8.3、8.4 节 | 干净克隆无法编译 |
| G6 | 生成入口 `start.bat` 依赖 2 条机器绝对路径的工具 | 第 8.4 节 | 生成步骤同样不可移植 |
| G7 | `config.ini`（运行期必需）被忽略且含敏感信息 | 第 8.3 节 | 仓库不含可运行配置 |
| G8 | 2 个运行期 DLL 被忽略（`*.dll` 全局忽略） | 第 8.3 节 | 仓库不含运行期依赖 |
| G9 | hiredis 依赖 `deps/hiredis` ↔ `src/Win32_Interop` 的**相对布局** | 第 5.5 节 | 迁移头文件时会静默破坏编译 |
| G10 | `LanguageStandard` 未声明，仓库不控制实际语言标准 | 第 7.5 节 | 语言能力假设不可靠 |
| G11 | MSVC 工具集具体版本未固定（仅 `v143`） | 第 7.2 节 | 编译器补丁级别漂移 |
| G12 | SDK 使用 `10.0` 选择器，实际解析依赖本机安装集合 | 第 7.4 节 | 解析结果随机器变化 |
| G13 | `_WIN32_WINNT` 未定义，实际目标由 Boost.Asio 回退决定 | 第 7.6 节 | 隐式平台假设 |
| G14 | 无 `DIRECTORY.Build.props` 等集中变量层，路径无法外部覆盖 | 第 2.5、6.3 节 | 无法在不改仓库文件的前提下重定向依赖 |
| G15 | 无 CMake/vcpkg/conan 声明 | 第 2.5 节 | 依赖无法声明式获取 |
| G16 | 无任何 C++ 单元测试基础设施 | 第 2.5 节 | 无法进行编译期/单元级回归 |
| G17 | 未在干净机器或 CI 上验证 | 第 6.3 节 | 可复现性完全未证明 |
| G18 | Post-build 依赖 `$(SolutionDir)`，与「必须经 `.sln` 构建」耦合 | 第 8.1 节 | 构建入口不可自由替换 |
| G19 | `ChatServer` / `ChatServer2` 源码人工重复（本 Step 仅记录） | 第 2.4 节、Step 1.6 §22 | 双份维护风险（**本 Step 不处理**） |
| G20 | 本机 `msbuild` 不在 `PATH`，且仓库无封装构建脚本 | 第 7.1 节 | 构建步骤不可自助发现 |
| G21 | 字节级可复现性未成立：同配置同源码两次 `Rebuild` 得到相同大小、**不同** SHA256 的 `ChatServer.exe` | 第 1.4 节 | 无法用二进制哈希作为构建一致性证据 |

---

## 10. Step 2.2 设计选项（Step 2.2 Design Options）

以下为**候选**，本节**不选择、不实现**。目标是为人工评审提供足够证据以选定 Step 2.2 的方向。

共同约束（由第 5.5、6、9 节得出）：

- 必须处理 hiredis 的 `deps/hiredis` ↔ `src/Win32_Interop` **相对布局**（G9）；
- 必须处理生成文件 `message*.pb.*`（G5）与生成工具路径（G6）；
- 必须处理 4 份 props 的同步问题（G2）；
- 必须处理「props 内容本身是 Debug 专用」（G4）——否则 `Release` 接入后会引入 Debug CRT 与 Debug 库。

---

### 选项 A：显式依赖根属性 + 环境变量覆盖

**机制**

在 `PropertySheet.props`（或新的共享 props）顶部引入单一变量，例如：

```xml
<PropertyGroup Label="UserMacros">
  <DepsRoot Condition="'$(DepsRoot)'==''">F:\cppsoft</DepsRoot>
  <BoostRoot Condition="'$(BoostRoot)'==''">F:\minGw\boost_1_89_0</BoostRoot>
</PropertyGroup>
```

随后把所有 `F:\cppsoft\...` 改写为 `$(DepsRoot)\...`，从而支持通过环境变量或 `/p:DepsRoot=...` 覆盖。

**优点**

- 改动集中在 1 个概念上，不引入新的构建系统；
- 保留现有 `Debug|x64` 行为（默认值不变）；
- 允许 CI / 其他开发者在**不改仓库文件**的前提下重定向（`/p:` 命令行即可）。

**缺点 / 风险**

- 仍要求使用者拥有**等价目录结构**，否则只改根目录仍然不够（例如 `grpc\visualpro\...\Debug` 的深层结构必须一致）；
- Boost 与 `cppsoft` 不同盘/不同根，需要 2 个变量；
- 未解决 G2（4 份 props）、G4（Debug 专用内容）、G5/G6（生成文件）。

**迁移风险**：低（纯替换，可用 `Debug|x64` 回归验证）。
**对现有 `Debug|x64` 的影响**：若默认值与原值一致，预期**无行为变化**（需以回归构建确认）。

---

### 选项 B：提交模板 + 忽略本地覆盖文件

**机制**

- 提交 `PropertySheet.props.template`（或 `DependencyPaths.props.template`），内容含占位符与说明；
- 仓库中保留一个**被忽略**的本地文件（例如 `DependencyPaths.local.props`，加入 `.gitignore`），由开发者自行填写；
- 主 props 以条件导入该本地文件：

```xml
<Import Project="DependencyPaths.local.props" Condition="Exists('DependencyPaths.local.props')" />
```

并在缺失时给出可读的 `Error`/`Warning`。

**优点**

- 仓库中不再写入任何机器绝对路径（G1 的直接消除路径）；
- 本机路径与仓库脱钩，多人协作不会互相覆盖；
- 模板可携带注释，说明需要哪些依赖与各自的目录结构要求（可同时缓解 G9 的文档缺口）。

**缺点 / 风险**

- 引入「构建前必须先配置本地文件」的步骤，若未配置需给出**明确报错**而不是晦涩的 include 失败；
- 需要同时新增 `.gitignore` 规则（本 Step 不允许改 `.gitignore`，故该动作属 Step 2.2 或更后）；
- 未解决 G5/G6（生成文件与 `protoc`）、G4（Debug 专用内容）。

**迁移风险**：中（新增文件与导入链，首次配置不当会造成"能编译/不能编译"的环境差异）。
**对现有 `Debug|x64` 的影响**：若本地文件准确填写，预期**无行为变化**；若缺失，会从「可构建」退化为「明确报错」——这是期望行为，但需在 Step 2.2 明确 gate。

---

### 选项 C：仓库相对依赖布局（vendor / 脚本化获取）

**机制**

把依赖放进仓库内某个**相对**位置（如 `third_party/`），或提供脚本从固定位置/制品库拉取到该相对位置，再让 props 全部使用 `$(SolutionDir)..\..\third_party\...` 之类的相对路径。

**优点**

- 真正消除机器绑定（G1），并可用于 CI；
- 相对布局天然满足 hiredis 的 `deps/hiredis` ↔ `src/Win32_Interop` 结构要求（只要按原结构放置）。

**缺点 / 风险**

- 体量问题：Boost `1.89.0`、gRPC + abseil + BoringSSL + protobuf 的 Debug 静态库集合体量很大；直接入库会显著膨胀仓库；
- 当前 `.gitignore` 忽略 `*.dll`、`x64/` 等，若走 vendor 路线需要相应调整忽略规则（属 Step 2.2+ 的决策）；
- 若走「脚本拉取」路线，则引入新的获取机制与缓存目录约定，需要定义校验（版本/哈希）以避免不可复现；
- 需要为 4 个 props 提供共享方式，否则仍会分叉（G2）。

**迁移风险**：中高（仓库结构变化 + 首次构建准备时间 + 忽略规则调整）。
**对现有 `Debug|x64` 的影响**：若相对布局被完整提供，预期**无行为变化**；若提供不全，则从「可构建」退化为「不可构建」。

---

### 选项 D：维持现状 + 仅补文档与构建记录

**机制**：不改变任何构建配置，只把依赖清单、目录结构要求与已知缺口（本文档）固化为文档，由开发者按文档自行准备环境。

**优点**：零构建风险；对现有 `Debug|x64` 完全无影响。
**缺点**：G1–G20 全部保留，可复现性依然为零。
**迁移风险**：无。
**对现有 `Debug|x64` 的影响**：无。

---

### 选项 E：迁移到 CMake + 依赖包管理器（独立轨道）

**机制**：引入 `CMakeLists.txt` 与包管理器（vcpkg/conan）声明依赖，由 CMake 生成 VS 工程或 Ninja 构建。

**说明**：Step 1.6 §31 B3/B4 已把「CMake 可行性评估」列为 Phase 2 候选。该选项**远超**本 Step 与单一依赖路径问题的范围，且与 G4（Debug 专用链接库清单）、G5（protobuf 生成步骤，可由 CMake 的 `protobuf_generate` 类机制承接）、G16（测试框架）存在天然交叠。

**迁移风险**：高（构建系统替换、全部依赖重新声明、工程文件与 IDE 流程变更）。
**对现有 `Debug|x64` 的影响**：若替换完成则影响大；切换期间会存在双轨。

---

### 10.1 选项与缺口的对应关系（辅助评审）

| 缺口 | A | B | C | D | E |
| --- | --- | --- | --- | --- | --- |
| G1 机器绝对路径 | 部分（参数化） | 是（移出仓库） | 是 | 否 | 是 |
| G2 4 份 props | 否 | 部分 | 部分 | 否 | 是（单一声明） |
| G3/G4 配置与 Debug 语义 | 否 | 否 | 否 | 否 | 是 |
| G5/G6 生成文件与 protoc | 否 | 否 | 部分 | 否 | 是 |
| G14 无集中变量层 | 是 | 是 | 是 | 否 | 是 |
| G16 测试框架 | 否 | 否 | 否 | 否 | 部分 |
| G17 干净机器验证 | 部分（可被验证） | 部分 | 是 | 否 | 是 |

**【判读】** 本表只用于帮助评审排序；**不**构成对任何选项的推荐，也**不**暗示上述效果已被验证（除「D 无影响」外，其余均为机制层面的推断）。

---

## 11. 明确的非目标（Explicit Non-Goals）

本 Step 2.1 **不**做以下任何事情：

1. **不**使构建可移植（portable）；
2. **不**使构建可复现（reproducible）；
3. **不**统一/规范化 4 个配置（含不修改 `ImportGroup` 条件）；
4. **不**新增测试（不引入 GoogleTest 或任何测试框架）；
5. **不**修改 C++ 语言标准（不新增 `LanguageStandard`）；
6. **不**修复任何编译警告（46 条警告保持不变）；
7. **不**修复 `MysqlDao::CheckEmail`（C4715「不是所有的控件路径都返回值」）；
8. **不**定义 `_WIN32_WINNT`；
9. **不**修改 `PropertySheet.props` 中任何路径或属性；
10. **不**修改 `.sln` / `.vcxproj` / `.vcxproj.filters` / `.gitignore` / `config.ini`；
11. **不**修改 `ChatServer2`，也**不**分析/修复其源码重复问题（仅在 §2.4 记录为未来范围）；
12. **不**引入 CMake、vcpkg、conan 或任何包管理器；
13. **不**安装任何依赖、**不**修改 `PATH`、**不**以环境变量作为构建补救手段；
14. **不**开始 Meeting M1，**不**实现任何 Meeting C++ 代码；
15. **不**执行干净克隆实验（§8.4 的结论为静态推断，已在文中标注）。

**本 Step 交付物**：一份可追溯的事实记录，使 Step 2.2 及以后的任何构建系统改动都有一份明确的**改动前契约**可比对。

---

## 12. 与 Step 1.6 事实记录的对照

【事实】本文对 Step 1.6 的构建相关记录做了**独立复核**，结果如下：

| Step 1.6 记录 | 本文复核结果 |
| --- | --- |
| §3.5 权威入口、项目数 1、配置清单、`PlatformToolset=v143`、`LanguageStandard` 未设置、PostBuild 行为 | **一致** |
| §3.5 `PropertySheet.props` 仅 `Debug\|x64` 导入 | **一致** |
| §3.5/§3.6 `F:\...` 路径与存在性 | **一致**，本文扩充为精确的 31 条清单与逐节计数 |
| §3.6 VS 版本 `17.14.36603.0`、`msbuild` 不在 `PATH` | **一致** |
| §3.6 既有产物 `ChatServer.exe` 19106 KB / `2025-11-28 16:54:36` | **一致**（该产物已被 M0 的 Rebuild 覆盖为 19150 KB / `2026-09-18 22:22:00`） |
| §3.7 无 CMake / 无测试框架 / 无 `meeting/` | **一致** |
| §3.8 `ChatServer` 与 `ChatServer2` 各 41 个文件 | **一致**（本文补充：其中 33 个受版本控制、8 个被忽略） |
| §7.2 禁止 C++17 能力 | **一致**，且本文给出了「仓库未声明语言标准、命令行无 `/std:`」的实测依据 |
| §30 R7 机器绝对路径风险 | **一致**，本文细化为 31 条 + 4 份 props 同哈希 + Debug 专用内容（G4） |
| §31 B1–B7 Phase 2 backlog | **一致**，本文将其扩展为 G1–G20 |

**【判读】** 本文在以下方面**补充或细化**了 Step 1.6 尚未记录的事实：

1. `PropertySheet.props` 的四份副本哈希完全相同（G2）；
2. props **内容本身**是 Debug 专用（G4）——这会影响 Step 2.2 对「统一四配置」的难度判断；
3. `message*.pb.cc/.h` 被忽略且**无构建期自动生成**（G5），因而干净克隆无法编译；
4. hiredis 对 `src/Win32_Interop` 的**相对结构依赖**（G9）;
5. `WindowsTargetPlatformVersion=10.0` 的实际解析结果为 `10.0.26100.0`（G12）；
6. `CL.command.1.tlog` 中**不存在** `/std:`，语言标准未固定这一事实有了命令行级证据（G10）；
7. 排除 per-user props 与 `.vcxproj.user` 作为依赖路径注入源（§5.6）。

---

## 13. 附录：本 Step 未读取的内容（透明度声明）

为避免越界，本 Step **未**读取：

- `config.ini` 的**内容**（`.gitignore` §102 注释表明其含数据库/Redis/邮箱敏感信息）。仅记录其存在性、大小、时间戳与忽略状态。
- 任何 `.cpp` / `.h` 的**实现逻辑**。仅依据 `.vcxproj` 的文件清单与构建输出中的文件名与警告位置。
- `ChatServer2` / `GateServer` / `StatusServer` 的工程内部细节（仅比对 4 份 props 的哈希）。
- `MyChat_Qt/`（Qt 工程，不在本 Step 范围）。
