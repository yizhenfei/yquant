# YQuant

## 项目简介

YQuant 是一个支持多策略运行的低延迟量化交易系统，使用 C++ 开发。

为支持多个策略稳定、高效地同时运行，系统提供以下能力：

- **策略隔离**：每个策略在独立进程中执行，可在部署时绑定独立的 CPU 核心，减少策略之间的调度干扰和故障传播，提升延迟稳定性与运行可靠性。
- **集群扩展**：策略可分布到集群中的多台机器上，共享行情与交易服务，通过增加节点扩展策略运行规模。
- **高效通信**：提供独立的行情、交易进程服务。单机进程间通过共享内存与无锁数据结构实现高吞吐、低延迟通信；跨机进程间目前使用 TCP，后续将采用基于 UDP 的协议与 Kernel Bypass 技术进一步降低通信延迟。

项目定位、系统架构与技术设计见[项目介绍](docs/project-introduction.md)。

## 程序命名

项目中的所有程序统一采用 `yquant-$name[d]` 命名：

- `yquant-` 为统一前缀。
- `$name` 代表程序名称。
- 常驻服务进程在名称末尾加 `d`，其他程序不加此后缀。

核心策略进程命名为 `yquant-strategyd`。
行情进程命名为 `yquant-marketd`。
共享交易网关命名为 `yquant-tradingd`。
日志进程命名为 `yquant-logd`。

## 目标平台

| 用途 | 操作系统 | CPU 架构 |
| --- | --- | --- |
| 生产环境 | Linux | x86_64 |
| 开发环境 | macOS | arm64、x86_64 |

生产环境执行构建、功能及并发验收；开发环境支持本机构建、功能测试和调试。

## 项目目录

公共实现按职责组织为 `yquant_base`、`yquant_md` 和 `yquant_trading` 静态库，供可执行程序复用。

- `include/yquant/base/`：基础组件公开头文件。
- `include/yquant/model/`：交易领域公共类型。
- `include/yquant/md/`：标的字典与运行范围、行情配置、模拟报价、编解码与广播。
- `include/yquant/trading/`：交易账本、柜台接入、交易配置与网关传输。
- `include/yquant/strategy/`：策略开发接口与动态库协议。
- `src/base/`：基础组件及内部实现，包括内存广播队列和共享内存封装。
- `src/md/`：行情库实现。
- `src/trading/`：交易库实现。
- `src/strategyd/`：策略程序专用的动态库加载、行情服务、远端交易服务与调度。
- `src/marketd/`：行情程序专用的配置、输入与输出适配及调度。
- `src/tradingd/`：交易网关专用的配置、账户服务与请求路由。
- `src/logd/`：独立日志进程。
- `examples/tradingd/`：共享交易网关与策略客户端配置。
- `examples/marketd/`：本机分发、TCP 中继、历史回放与录制配置。
- `examples/strategies/`：用户策略动态库示例。
- `examples/ctp/`：CTP 行情与 SimNow 交易共用的合约规格示例。
- `examples/data/`：国内期货历史报价样本、来源与许可。
- `scripts/`：本地运行辅助脚本。
- `tests/base/`：基础组件测试。
- `tests/trading/`：交易账本、配置、模拟柜台与网关传输组件测试。
- `tests/strategyd/`：策略运行时与进程集成测试。
- `tests/md/`、`tests/marketd/`：行情组件与多进程分发测试。
- `tests/tradingd/`：交易网关服务与多进程共享账户测试。
- `tests/scripts/`：CTP 字段、协议、录制转换及运行脚本的离线与进程测试。
- `cmake/`：依赖与编译配置。
- `docs/design/`：架构与组件设计。
- `docs/research/`：系统研究资料与记录。

库与程序的依赖边界、CMake 目标见[目录与模块](docs/design/engineering.md#目录与模块)。

## 构建与测试

需要 CMake 3.21 或更高版本、Ninja 和支持 C++20 的编译器。类型检查的文本格式化与日志依赖 fmt，配置解析依赖 simdjson，测试依赖 GoogleTest；依赖由 CMake 获取固定版本，项目及依赖均以无异常配置构建。

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

`release`、`asan` 和 `tsan` 使用相同的配置、构建与测试命令。ASan 同时启用 UBSan，TSan 使用独立构建目录。不构建测试程序时可设置 `-DBUILD_TESTING=OFF`；只构建基础库时使用 `cmake --build --preset debug --target yquant_base`。

测试运行前由 CTest 检查外部测试数据，缺失或校验失败时自动准备。数据与下载缓存默认位于 `test_data/`，不纳入 Git；各 preset 共用此目录。首次准备需要联网下载约 112.5 MB 的官方压缩包，之后仅校验本地文件。也可以在测试前单独执行统一下载命令：

```sh
cmake -P cmake/DownloadTestData.cmake
```

已配置构建目录时，也可执行 `cmake --build --preset debug --target download_test_data`。修改存储位置时，配置阶段传入 `-DYQUANT_TEST_DATA_DIR=/absolute/path`；独立下载命令使用同名 `-D` 参数，并放在 `-P` 前。测试程序可通过环境变量 `YQUANT_TEST_DATA_DIR` 取得配置后的目录。

当前数据集是 [DolphinDB 股票回放样本](https://github.com/dolphindb/Tutorials_CN/blob/master/stock_market_replay.md)中的快照文件，保存为 `test_data/dolphindb/2020-12-31/snapshot.csv`。实际文件包含 2020-12-31 的 100 只深市 A 股、367,171 条十档盘口快照，买卖一价量字段为 `BidPX1`、`BidSize1`、`OfferPX1`、`OfferSize1`。下载来源与归档、CSV 的 SHA256 固定在 `cmake/TestDataManifest.cmake`；已有归档可用于离线恢复缺失或损坏的 CSV。

日志接口位于 `yquant/base/logging.h`。Debug 默认编译保留 DEBUG 及以上级别，其他构建保留 INFO 及以上；可在配置时使用 `-DYQUANT_LOG_COMPILED_LEVEL=TRACE` 等选项调整。

### 编码规范检查

C++ 代码遵循 [C++ 编码规范](docs/coding-styles.md)。使用 clang-format 23.1.3；格式配置由仓库根目录的 `.clang-format` 维护。提交前执行：

```sh
python3 scripts/check_cpp_style.py
```

CTest 的 `Project.HeaderSelfContainment` 使用编译数据库逐个检查项目头文件能否独立编译。生产代码与测试均使用 `yquant/base/path.h` 和 `yquant/base/file.h` 中的路径及文件操作；定长缓冲区格式化和标准流输出使用 `yquant/base/format.h`，保留显式错误返回。

### 路径解析

所有接受相对路径的命令行参数和配置字段，都以进程的当前工作目录为基准；绝对路径直接使用。嵌套配置中的引用沿用同一工作目录，不随配置文件所在目录改变。程序在初始化时解析引用，运行期间不改变工作目录。共享内存的 `runtime_directory` 仍要求显式填写绝对路径。

文中的命令均在仓库根目录执行。示例配置使用 `examples/...` 引用配套文件；录制示例中的 `capture.yqr`、`live_capture.yqr` 写入工作目录。从其他目录启动时，需要调整配置中的相对路径或使用绝对路径。

### VS Code

使用 VS Code 打开仓库根目录，安装推荐的 C/C++ 和 CMake Tools 扩展。工作区通过 CMake Tools 提供 IntelliSense 编译参数，同时指定 `build/debug/compile_commands.json`，以 C++20 和项目头文件目录作为后备配置。

工作区在打开项目时自动配置 CMake。新 checkout 或 worktree 首次打开时，在命令面板执行 `CMake: Select Configure Preset` 选择 `debug`，等待配置成功；需要手动重试时执行 `CMake: Configure`，生成编译数据库，无须先编译整个项目。CMake 和 Ninja 需要在 VS Code 的运行环境中可用；个人工具路径放在用户设置中，不写入仓库配置。切换构建 preset 时，CMake Tools 提供对应构建的编译参数。

已有编译数据库时，重新加载窗口即可读取工作区配置。如果仍显示旧诊断，执行 `C/C++: Reset IntelliSense Database`。头文件的语言标准、系统 SDK、宏和第三方依赖路径应与 CMake 编译参数保持一致。

## 日志进程

日志默认由进程内后台线程写盘。设置 `LogConfig::backend = LogBackend::kLogd` 后，业务进程通过共享内存 SPSC 队列向 `yquant-logd` 发送日志，不启动本地写盘线程。

先启动 logd，队列及日志文件的父目录须存在，队列文件须不存在：

```sh
./build/debug/bin/yquant-logd \
  --channel /tmp/strategy-log.queue /tmp/strategy.log
```

业务进程使用 `LogManager::Initialize()` 选择 `kLogd`，再通过 `LoggerConfig::queue_path` 连接队列；生产端不配置 `file_path`。多通道重复传入 `--channel QUEUE_FILE LOG_FILE`，每条队列只接收一个 Logger。队列容量默认 1 MiB，`--capacity` 和 `--max-file-bytes` 必须放在所有 `--channel` 之前，统一配置消费端容量和轮转阈值；两端的队列容量须一致。

退出顺序为业务进程调用 `Shutdown()`、收到排空及文件同步确认，再向 logd 发送 `SIGTERM` 或 `SIGINT`。每条队列只供一次生产者会话使用；新会话重新启动 logd 并创建新队列。进程异常退出后的队列恢复与错误语义见[日志设计](docs/design/logging.md#跨进程日志)。

## 策略进程

`yquant-strategyd` 读取 JSON 配置并加载用户策略动态库，全部校验成功后调用 `OnStart(context)`。主线程驱动行情、交易请求、委托与成交回报及策略定时事件。收到停止信号后，宿主停止行情和策略新请求，自动撤单并处理回报；排空后调用 `OnStop(context)`、销毁实例并释放动态库句柄。

先按[行情进程](#行情进程)启动 `marketd`，再在另一个终端启动交易网关：

```sh
./build/debug/bin/yquant-tradingd --config examples/tradingd/local.json
```

两个服务就绪后，运行生命周期策略示例：

```sh
./build/debug/bin/yquant-strategyd \
  --strategy ./build/debug/strategies/lifecycle_strategy.so \
  --config ./build/debug/strategies/lifecycle_strategy.json \
  --market-data-config ./build/debug/strategies/market_data.json \
  --trading-config examples/tradingd/client_1.json
```

`--strategy`、`--config`、`--market-data-config` 和 `--trading-config` 均为必填参数，顺序不限。所有交易统一经 `tradingd` 执行；连接或握手失败时不加载策略。启动成功后向 `stdout` 输出 `Running` 和策略 ID。控制台流遵循[标准输出与诊断约定](docs/design/engineering.md#标准输出与诊断)。使用 `Ctrl+C` 或向进程发送 `SIGTERM` 停止，使用 `--help` 查看参数。路径以启动工作目录为基准；动态库文件名后缀由 CMake 的本机 `MODULE` 配置决定。

生命周期示例的 `LifecycleConfig` 派生自 `StrategyConfig`，读取必填字符串 `parameters.startup_message`，将解码后 1 至 128 字节、无内嵌 NUL 的内容保存在自身定长存储中，拒绝未知参数。配套配置将消息设为 `lifecycle strategy started`，`OnStart` 读取该参数并输出 `lifecycle_strategy: startup_message=lifecycle strategy started`。

用户策略继承 `yquant::strategy::Strategy`，导出 `YQuantGetStrategyPlugin()`，通过描述符提供创建和销毁函数。回调通过 `context.market_data()` 访问行情，通过 `context.trading()` 提交、撤销和查询委托，通过 `context.Now()` 查询单调时间，通过 `context.timer()` 登记一次性时间唤醒。接口及所有权约定见[策略进程与动态策略加载设计](docs/design/strategyd.md)。

配置文件包含 `strategy_id` 和 `parameters`。框架校验公共字段，策略内嵌的 `StrategyConfig` 派生对象解析自定义参数；无自定义参数时使用基类并填写空对象。配置 ID 为 1 至 64 字节的 UTF-8 字符串。各进程配置与共享标的字典统一遵循[通用 JSON 输入规则](docs/design/engineering.md#json-配置文档)。

强类型参数示例见 [parameterized_strategy.cc](examples/strategies/parameterized_strategy.cc) 和[配套配置](examples/strategies/parameterized_strategy.json)。构建后将上述命令的动态库与配置文件名都换成 `parameterized_strategy` 即可运行；配置约定见[策略配置设计](docs/design/strategy-config.md)。

先按[行情进程](#行情进程)启动 `marketd` 的模拟输入，再在另一个终端启动观察策略：

```sh
./build/debug/bin/yquant-strategyd \
  --strategy ./build/debug/strategies/quote_observer_strategy.so \
  --config ./build/debug/strategies/quote_observer_strategy.json \
  --market-data-config ./build/debug/strategies/market_data.json \
  --trading-config examples/tradingd/client_1.json
```

`--market-data-config` 指定宿主行情接收配置，支持共享内存及有限历史回放，并绑定唯一来源。配置通过 `instrument_catalog` 引用[共享标的文件](examples/instruments.json)，`instruments` 只列接收范围内的标的身份。模拟报价的价量、步长和间隔配置在 `marketd` 生产端。观察策略首次行情输出 `quote_observer: first quote received`，停止时输出 `ticks`、`last_sequence` 和 `max_age_ns` 汇总。路径规则与字典格式见[行情驱动设计](docs/design/market-data.md#共享标的字典)。

保持 `marketd` 运行，再启动单笔限价委托示例：

```sh
./build/debug/bin/yquant-strategyd \
  --strategy ./build/debug/strategies/order_lifecycle_strategy.so \
  --config ./build/debug/strategies/order_lifecycle_strategy.json \
  --market-data-config ./build/debug/strategies/market_data.json \
  --trading-config ./build/debug/tradingd/client_1.json
```

`--trading-config` 指定网关连接、客户端身份和超时，不接受柜台或初始账户配置。柜台和账户由 `tradingd` 的配置决定。示例在首次有效卖一报价提交 200 股买单，网关的默认 `fill` 模式按 100 股、100 股两笔成交，输出 `order_lifecycle: terminal state=filled cumulative=200 leaves=0`。成交后进程继续运行，停止时输出 `order_lifecycle: stopped fills=2 filled_qty=200 state=filled`。

将网关柜台配置的 `mode` 改为 `working`，等到 `order_lifecycle: working` 后发送停止信号，可验证宿主自动撤单和排空。交易接口、配置及收尾规则见[交易模块设计](docs/design/trading.md)。交易服务同时提供持仓与资金查询，买单预占资金，卖单预占可卖持仓。

多个策略共享同一个网关时，为每个客户端配置不同的 `client_id`。沿用前面的网关，启动共享账户策略：

```sh
./build/debug/bin/yquant-strategyd \
  --strategy ./build/debug/strategies/shared_account_strategy.so \
  --config ./build/debug/strategies/shared_account_strategy.json \
  --market-data-config ./build/debug/strategies/market_data.json \
  --trading-config ./build/debug/tradingd/client_1.json
```

另一个策略使用 `client_2.json`。跨机时修改服务端监听地址和客户端目标地址，各客户端保持不同身份。`tradingd` 统一管理账户资源，向所属策略回送委托与成交，并推送共享账户快照；本地模拟柜台由网关预占资源，CTP 模式使用柜台查询的冻结口径。协议、停止与故障边界见[共享交易网关](docs/design/tradingd.md)。

网关与策略宿主从同一[共享标的字典](docs/design/market-data.md#共享标的字典)解析各自运行清单，客户端运行范围须为网关子集。示例策略只配置场所、代码、方向和数量，行情来源由策略宿主绑定。

运行有底仓双边报价策略：

```sh
./scripts/run_bbo_strategy.sh

# 保持挂单，观察随行情撤旧补新。
./scripts/run_bbo_strategy.sh working
```

脚本使用已编译的 Debug 产物，默认以 `fill` 模式生成模拟成交。它自动定位项目目录，在临时目录中保存本次柜台配置与 `run.log`，启动时打印路径，不修改示例配置。运行 3～5 秒后按 `Ctrl+C`，查看 `bbo_market_making: stopped` 汇总；资金以微元计量，正常收尾的 `cash_reserved`、`reserved_qty` 和 `slots` 均为零。

样例使用 3 万元现金、1,000 股总持仓和 800 股可卖底仓，买卖各挂 100 股。`working` 模式保持挂单，策略跟随行情撤旧补新。行情过期撤单和失败重试使用主线程定时器。交易规则与直接运行命令见[有底仓双边策略](docs/design/bbo-market-making.md)。

运行零持仓启动的国内期货双边策略：

```sh
./scripts/run_futures_strategy.sh
```

脚本回放大商所豆一期货 `a1805` 在 2017-12-27 的 744 条真实买卖一报价，以 100 倍速度运行，约 20 秒后自动撤单收尾。交易所在独立的 `tradingd` 中模拟实现，支持买卖开仓、平今平昨、保证金预占、手续费与损益。启动时打印本次配置和日志目录，结束时查看 `futures_bbo: stopped` 汇总。数据来源、计量和撮合规则见[国内期货模拟交易](docs/design/domestic-futures.md)。

接入实时国内期货行情时，使用 [CTP 行情桥](docs/design/ctp-market-data.md)。
它通过只读行情 API 向 `marketd` 发布买卖一报价，保留双边策略并同步录制；
同一录制可在无账户的环境中驱动模拟交易回放。
实时接入支持账户登录，也支持前置允许时显式使用 `--anonymous` 空登录。

使用 SimNow 的模拟交易柜台时，运行
[SimNow 报撤单入口](docs/design/simnow-trading.md#运行)：

```sh
python3 scripts/run_simnow_strategy.py live \
  --run-dir /tmp/yquant-simnow-run \
  --catalog examples/ctp/treasury_instruments.json \
  --venue CFFEX --symbol TF2612 --anonymous-md
```

交易使用注册的 SimNow 账户，通过环境变量配置；`--anonymous-md` 只用于行情登录。
此入口将订单交给 SimNow 柜台，资金、持仓及手续费保留柜台查询口径。

## 行情进程

`yquant-marketd` 按 JSON 组装 `Source → QuoteTick → Publisher[]`。输入支持模拟行情、共享内存、TCP 和历史文件回放；输出支持共享内存广播、TCP 分发和异步录制。接入与中继使用相同程序，由配置决定组合。

先启动行情进程：

```sh
./build/debug/bin/yquant-marketd --config ./build/debug/marketd/local.json
```

再在其他终端启动一个或多个策略进程：

```sh
./build/debug/bin/yquant-strategyd \
  --strategy ./build/debug/strategies/quote_observer_strategy.so \
  --config ./build/debug/strategies/quote_observer_strategy.json \
  --market-data-config ./build/debug/strategies/shm_market_data.json \
  --trading-config examples/tradingd/client_1.json
```

各策略进程通过私有游标读取同一份共享内存。槽位覆盖时报告缺口并停止策略，避免继续使用缺失事件后的状态。共享内存和网络中继均保留源标识、品种、报价序号与 UTC 事件时间，接收时间在策略进程本地赋值。

跨机部署使用 [tcp_source.json](examples/marketd/tcp_source.json) 与 [tcp_relay.json](examples/marketd/tcp_relay.json)。策略机器将输入地址改为行情机器的数字 IPv4 地址，随后向本机共享内存发布。TCP 单个慢客户端被独立断开，其他输出继续工作。

`--check-config` 只校验配置，不创建发布者；`--max-events N` 用于有限事件演示。使用 SIGINT 或 SIGTERM 停止进程。完整配置、故障处理及协议见[行情进程设计](docs/design/marketd.md)。

用真实历史数据完成回放与录制：

```sh
cmake --build --preset debug --target download_test_data
./build/debug/bin/yquant-marketd --config ./build/debug/marketd/replay_csv.json
```

示例最快回放全部 367,171 条 DolphinDB 历史快照，将一档报价录制到工作目录的 `capture.yqr`。已有同名文件时失败，重新运行需更换文件名或移动已有文件。`replay_recording.json` 可将录制文件发到共享内存；原速、倍速、启动等待与策略接入见[行情回放与录制](docs/design/market-data-replay.md)。

## 工程方案

- [架构图制作与维护](docs/architecture-diagrams.md)：SVG 内容确认、手绘展示图、图例与配色规范。
- [工程组织与验证方案](docs/design/engineering.md)：平台、错误处理、构建依赖与测试工具方案。

## 组件设计

- [日志选型与设计](docs/design/logging.md)：异步日志库对比、队列策略与验证标准。
- [SPSC 队列设计](docs/design/spsc-queue.md)：低延迟内存消息队列设计。
- [单写多读广播队列](docs/design/broadcast-queue.md)：外部内存、seqlock、独立游标和覆盖缺口。
- [策略进程与动态策略加载设计](docs/design/strategyd.md)：生命周期接口、动态库协议与命令执行流程。
- [策略配置设计](docs/design/strategy-config.md)：JSON 分层、强类型配置与启动校验。
- [国内期货模拟交易](docs/design/domestic-futures.md)：开平仓、期货账本、模拟撮合与历史报价回放。
- [CTP 行情接入与同源回放](docs/design/ctp-market-data.md)：只读实时行情、录制和离线策略运行。
- [SimNow 模拟柜台接入](docs/design/simnow-trading.md)：交易登录、报撤单、成交与柜台账户。
- [策略定时事件](docs/design/strategy-timer.md)：主线程的一次性时间唤醒、调度与生命周期。
- [行情驱动与策略事件循环](docs/design/market-data.md)：一档报价、策略上下文、共享内存接收与事件分派。
- [行情进程设计](docs/design/marketd.md)：可配置行情管线、本机广播和跨机中继。
- [行情回放与录制](docs/design/market-data-replay.md)：真实 CSV 转换、回放时序和异步录制格式。
- [共享内存行情广播组件](docs/design/shared-memory-broadcast.md)：映射、流注册、心跳、重启隔离和行情适配。
- [交易模块设计](docs/design/trading.md)：功能需求、订单与成交、账户管理及交易接入设计。
- [订单核心与请求执行](docs/design/order-core.md)：订单业务状态机、新单与撤单的 in-flight 跟踪及事务边界。
- [共享交易网关](docs/design/tradingd.md)：多机策略共享账户、订单归属、TCP 协议和断线收尾。
- [有底仓的一档双边报价策略](docs/design/bbo-market-making.md)：库存约束、撤补、过期撤单与失败退避。

## 参考系统与资料

- [交易系统参考资料](docs/research/trading-systems.md)：长期维护设计与开发参考的系统及其官方资料入口。
- [100 个 GitHub 参考系统](docs/research/github-trading-systems-100.md)：按 Stars、Forks 和仓库历史初筛，按研究／回测／实盘覆盖定位分组，各组按 Stars 排序，附指标快照与排除记录。
- [Tick 数据分类与固定 Slot 存储](docs/research/tick-data-model.md)：对照六个参考系统的行情载荷，整理市场差异、变长字段及定长槽容量边界。
- [国内期货 Tick 数据渠道](docs/research/domestic-futures-data-sources.md)：实时行情、历史数据与账户权限。
