# 工程组织与验证方案

工程采用 C++20 和 Google C++ Style Guide，错误处理使用显式结果返回，构建、依赖与测试通过 CMake 统一组织。目标平台见项目 [README](../../README.md#目标平台)。

## 工具链

| 环境 | 工具链方案 | 验证用途 |
| --- | --- | --- |
| Linux x86_64 | GCC 与 libstdc++ 为生产基准；Clang 用于兼容构建及 sanitizer | 功能、并发与生产行为验收 |
| macOS | AppleClang 与 libc++ | 本机构建、功能测试及开发调试 |

配置阶段编译检查实际需要的 C++20 特性、`std::span` 和始终无锁的 64 位原子操作。不支持时明确失败，不以互斥锁替代队列原子操作。正式验收环境为 Linux x86_64。

生产编译器和标准库的具体版本随可复现的 Linux 构建环境固定，开发机通过同样的特性检查。平台相关的文件与线程调用收敛在内部适配层。

## 错误处理

### 状态与结果

项目及所用依赖关闭 C++ 异常支持，不使用 `throw`、`try` 或 `catch`。GCC、Clang 和 AppleClang 构建使用 `-fno-exceptions`。可恢复的失败通过状态返回，调用者负责处理；需要检查的接口标记 `[[nodiscard]]`。

通用管理操作使用 `Status`，错误码覆盖成功、参数错误、状态错误、资源不足、不支持、I/O 错误、数据损坏和内部错误。关键结构示意：

```text
Status = { code: ErrorCode, system_error: std::error_code }
Status.ok() -> bool  // code == kOk
```

`Status` 按值返回，不持有动态错误字符串。系统错误立即保存原因，错误说明在管理或报告路径生成。POSIX 函数以 `errno` 或自身返回的错误码为依据；例如 `pthread_create` 使用返回值，不读取 `errno`。

有返回值的管理操作使用专用结果结构，包含状态和结果值。成功时结果有效，失败时指针为空或长度为零。队列满、空、输出区不足等常态操作结果使用 `SpscStatus`，具体结构见 [SPSC 队列设计](spsc-queue.md)。

### 资源与程序错误

需要返回分配失败的路径采用 `new (std::nothrow)`、`malloc` 或操作系统资源接口，检查结果后再发布对象；对象构造本身不得隐藏分配。初始化时按配置上限预分配注册表和缓冲；运行期容量耗尽返回 `kResourceExhausted`。这些路径不通过 `std::vector`、`std::string` 的隐式扩容或 `std::make_unique` 获取资源。

线程创建采用 `pthread_create`，管理同步检查 pthread 接口的返回值。文件后端使用 POSIX `open`、`write`、`fsync`、`close`、`rename` 等接口，并立即保存系统错误。后台资源类型及 `pthread_t` 不进入公开接口。

构造器只建立可安全析构的空状态；会失败的资源准备放在显式初始化操作中，失败时回滚本次已取得的资源。析构器不承担错误结果的唯一出口，支持显式关闭并返回最终结果。

公开参数、对象状态和队列编码错误通过状态返回；队列损坏停用对应通道，其余健康通道继续收尾。内部不变量在开发构建中断言，无法安全继续的生命周期违例在所有构建中直接终止。错误处理不递归写回发生故障的日志通道。

### fmt 使用边界

fmt 与包含其公开模板的调用方统一设置 `FMT_USE_EXCEPTIONS=0` 并关闭编译器异常支持。该配置下，fmt 内部格式错误会进入断言终止路径，没有可转换为状态的异常出口。[fmt 配置与错误路径](https://github.com/fmtlib/fmt/blob/12.1.0/include/fmt/format.h#L155-L160)

日志入口使用项目的 `consteval LogFormat<Args...>` 检查格式和参数类型，初版开放 `{}`、`{{`、`}}`。允许基本整数、`bool`、`char`、默认格式的 `float`、`double`、有长度的字符串视图和字符串字面量。枚举显式转为底层整数，业务类型先转换为受支持的标量或定长文本。

格式入口在编译期拒绝运行期格式字符串、宽度和精度规格、locale 及任意自定义 formatter；裸 `const char*` 先转为有长度的视图。字符串输入规模在调用 fmt 前检查，拒绝时计数。`fmt::format_string` 的类型检查由封装复用，项目入口不开放 `fmt::runtime` 转换。默认格式的基本类型与字符串适合作为无分配格式化的验证范围。[fmt 格式检查与内存分配](https://fmt.dev/latest/api/)

fmt 内部格式错误属于程序契约违例，不作为可恢复的格式化失败计数。日志的具体检查、截断和丢弃规则见 [日志选型与设计](logging.md)。

## 目录与模块

构建对象分为公共库和可执行程序。被多个程序使用的实现归入公共库；`src/strategyd/`、`src/marketd/`、`src/tradingd/` 只保存对应程序专用的代码。程序通过库的公开头文件复用功能，不包含其他程序目录的头文件或链接其私有实现。

```text
include/yquant/base/        基础库公开接口
include/yquant/model/       公共领域类型
include/yquant/md/          行情库公开接口
include/yquant/trading/     交易库公开接口
include/yquant/strategy/    策略 SDK
src/base/                  基础库实现及内部平台适配
src/md/                    行情库实现
src/trading/               交易库实现
src/strategyd/             策略程序专用实现
src/marketd/               行情程序专用实现
src/tradingd/              交易网关程序专用实现
tests/                     各库和程序的单元与集成测试
tests/cmake/               构建与测试数据准备流程验证
test_data/                 外部测试数据与下载缓存，不纳入 Git
cmake/                     依赖、编译选项与工具链配置
docs/design/               组件设计
CMakeLists.txt
CMakePresets.json
```

库的公开头文件位于 `include/yquant/<library>/`，实现位于 `src/<library>/`，命名空间与库名一致。内部辅助代码使用库内的 `internal` 命名空间或实现文件内的匿名命名空间。公开头文件可独立编译，只包含公开依赖。

| 模块 | CMake target | 职责与依赖 |
| --- | --- | --- |
| `yquant::base` | `yquant_base`，别名 `yquant::base` | 日志、队列、共享内存、路径、JSON 文档、单调时钟及进程等待和退出信号；队列不依赖日志或 fmt |
| `yquant::model` | `yquant_model`，别名 `yquant::model`，`INTERFACE` 目标 | 标识、标的、行情、委托和账户等领域类型 |
| `yquant::md` | `yquant_md`，别名 `yquant::md` | 标的字典、运行标的范围、接收配置、模拟报价、编解码及行情广播适配；依赖 `base` 和 `model` |
| `yquant::trading` | `yquant_trading`，别名 `yquant::trading` | 交易账本、接入协议、配置、模拟柜台、网关协议及连接；依赖 `base`、`md`、`model` 和策略服务接口 |
| `yquant::strategy` | `yquant_strategy_api`，别名 `yquant::strategy_api` | 策略 SDK 的 `INTERFACE` 目标，提供公开头文件和编译要求；回调门禁枚举位于 `callback_phase.h` |

库不依赖程序私有实现。`MarketDataImpl` 保留在 `strategyd` 中，负责策略订阅、缓存与回调阶段；公共交易账本只借用 `md::InstrumentScope` 查询标的。程序私有对象目标与可执行目标的组织见[构建与依赖](#构建与依赖)。

## 进程基础设施

[base::MonotonicClock](../../include/yquant/base/monotonic_clock.h) 提供可注入的纳秒时钟接口；`base::SteadyMonotonicClock` 以自身构造时的 `std::chrono::steady_clock` 读数为原点。`Now()` 不分配内存且不递减，同一时间域中的时间戳和期限共用一个时钟实例，跨实例、进程或机器不比较相对读数。时钟由使用方持有，存活时间覆盖全部借用者。

[base::EventLoopEnvironment](../../include/yquant/base/event_loop_environment.h) 提供主线程的 `CheckStop()` 与可选超时的 `Wait()`，便于事件循环注入测试环境。[base::TerminationSignals](../../include/yquant/base/termination_signals.h) 实现 SIGINT、SIGTERM 的屏蔽、显式检查和 `pselect` 等待，由三个程序复用；信号处置与屏蔽掩码保留到进程退出，实现位于 `src/base/termination_signals.cc`。策略程序如何在启动、回调和收尾边界使用这些设施见[主循环与信号](strategyd.md#主循环与信号)。

## JSON 配置文档

`base::JsonConfigDocument` 负责配置文件读取、完整输入校验和源码所有权管理，供 `strategyd`、`marketd`、`tradingd` 及共享标的字典复用。公开接口位于 `include/yquant/base/json_config_document.h`，实现位于 `src/base/json_config_document.cc`；公开头文件不暴露 simdjson 类型。

配置文件使用 UTF-8 编码和标准 JSON 语法，根节点必须为对象。文件最大为 1 MiB，容器嵌套最多 64 层，根对象计为第一层。加载时完整检查所有字符串、标量及容器，并在所有对象中按解码后的键名拒绝重复键，例如 `a` 与 `\u0061` 视为同一键。数值在这一层只校验 JSON 语法，取值范围由各配置加载器检查。

文档持有带解析所需 padding 的原始源码，完整校验的解析器和重复键临时存储在 `Load()` 返回前释放。源码视图持续有效到下次 `Load()`、`Reset()` 或文档析构；加载失败后源码为空。各加载器复用基础库的根对象解析和解析错误转换，负责自身的字段、类型、必填项和组合约束，不向文档类加入业务 schema。

`base::ConfigError` 位于 `include/yquant/base/config_error.h`，用固定容量存储字段路径、原因和截断标志，复制诊断文本，不借用源码或动态库字符串。文档与 schema 解析共用该类型；JSON 解析器的分配失败保留为 `kResourceExhausted`，其他解析错误返回 `kInvalidArgument`，文件错误保留系统错误。需要长期保留的配置值由加载器或配置对象持有，运行期不保留文档借用。

## 构建与依赖

构建使用 CMake 3.21 或更高版本，生成器使用 Ninja。仓库维护 configure、build、test presets；本机路径及个人工具链放入不提交的 `CMakeUserPresets.json`。构建产物统一位于 `build/<preset>/`。[CMake Presets](https://cmake.org/cmake/help/v3.21/manual/cmake-presets.7.html)

公共实现由 `yquant_base`、`yquant_md`、`yquant_trading` 三个 `STATIC` 库提供。程序专用实现按下表编译为 `OBJECT` 目标，只供对应可执行程序及其单元测试使用；各程序对象目标之间不相互链接。测试访问程序内部接口不构成跨程序公共接口。

| 私有对象目标 | 可执行目标 | 输出程序 |
| --- | --- | --- |
| `yquant_strategyd_objects` | `yquant_strategyd` | `yquant-strategyd` |
| `yquant_marketd_objects` | `yquant_marketd` | `yquant-marketd` |
| `yquant_tradingd_objects` | `yquant_tradingd` | `yquant-tradingd` |

编译选项按 target 配置：C++20、公开 include 路径和日志编译级别向调用方传播；项目警告选项设为私有，不强加给依赖。日志公开模板使用 fmt，因此 `fmt::fmt` 为公开依赖。异常开关和 fmt 配置同时应用于依赖自身及相关调用方，避免不同编译单元采用不同配置。

第三方依赖由 `cmake/` 中的 `FetchContent` 声明集中管理，固定完整 Git commit 或归档 URL 与 SHA256。fmt 和 simdjson 为生产依赖，GoogleTest 仅由测试程序使用。依赖不跟随浮动分支，不自动切换到未固定版本的系统安装。[FetchContent](https://cmake.org/cmake/help/latest/module/FetchContent.html)

JSON 解析使用 simdjson `5.0.4`，关闭异常接口及线程支持。`yquant_base`、业务 schema 加载器和示例策略按需私有链接其静态实现，依赖不进入公开配置文档接口。版本与归档哈希由 `cmake/Dependencies.cmake` 固定。

构建提供以下 presets：

| Preset | 配置 |
| --- | --- |
| `debug` | 开发构建与功能测试 |
| `release` | 优化构建与功能测试 |
| `asan` | Clang，AddressSanitizer 与 UndefinedBehaviorSanitizer |
| `tsan` | Clang，独立的 ThreadSanitizer 构建 |

ASan 与 TSan 使用独立构建目录，不合并到同一个程序。依赖版本、编译器及系统镜像版本随实现固定在构建配置和 CI 中。

## 验证工具与检查

单元与集成测试使用 GoogleTest，由 CTest 调度，以 `gtest_discover_tests()` 注册。测试框架使用无异常构建。并发测试复用组件设计中的验收条件。[GoogleTest CMake 集成](https://google.github.io/googletest/quickstart-cmake.html)

外部测试数据由 `cmake/DownloadTestData.cmake` 统一准备，可独立执行或通过 `download_test_data` 构建目标调用。数据默认存入不纳入 Git 的 `test_data/`，不同构建配置共用缓存；`YQUANT_TEST_DATA_DIR` 可配置绝对路径，并作为环境变量传给测试程序。来源、存储路径和 SHA256 由 `cmake/TestDataManifest.cmake` 固定。

CTest 的 `TestData.Prepare` 通过 fixture 在功能、并发及编译检查前校验数据。有效文件直接复用，缺失或损坏时优先从已校验的归档恢复，归档不可用时才下载。下载和解压使用临时路径，CSV 校验通过后原子发布；进程间文件锁防止不同 preset 或手动下载同时修改缓存。准备失败时依赖该 fixture 的测试不运行。下载器自身的回归测试使用本地微型归档，覆盖缓存恢复与失败路径。

GoogleTest 的 `PRE_TEST` 发现步骤先于 fixture 执行；测试枚举及全局初始化不读取外部数据，数据读取放在测试执行阶段。

| 验证环境 | 检查内容 |
| --- | --- |
| Linux x86_64，GCC | Debug、Release 构建，完整功能与并发测试 |
| Linux x86_64，Clang | ASan 与 UBSan 构建，检查边界、生命周期及未定义行为 |
| Linux x86_64，Clang | TSan 构建，检查同进程线程间的队列与日志访问 |
| macOS，AppleClang | 本机架构构建、功能测试及开发调试 |
| 格式检查 | 使用固定版本的 clang-format 和仓库配置检查项目源码 |

TSan 检查线程间的内存访问，帧发布及空间复用的顺序由并发测试和参考模型共同验证。[ThreadSanitizer](https://clang.llvm.org/docs/ThreadSanitizer.html)

队列测试使用固定随机种子和参考消息序列，覆盖借用、短提交、取消、尾部回绕、整数回绕及损坏状态。线程间传输测试验证完整帧序列、发布和空间复用。

日志测试覆盖编译期删除、参数无求值、稳态无堆分配、后台暂停、文件故障和关闭排空。编译检查同时覆盖公开头文件自包含、无异常构建及不符合日志格式约束的调用。

性能测量保留调用延迟、吞吐、CPU 和内存数据，并记录硬件及构建配置。当前不设置性能数值门槛。
