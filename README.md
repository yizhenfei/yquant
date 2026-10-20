# YQuant

## 项目简介

YQuant 是使用 C++ 编写的量化交易系统，采用 C++20，使用 CMake 管理构建配置。

项目不使用 C++ 异常。

日志等基础组件统一组织在 `yquant_base` 静态库中，供上层模块复用。

## 程序命名

项目中的所有程序统一采用 `yquant-$name[d]` 命名：

- `yquant-` 为统一前缀。
- `$name` 代表程序名称。
- 常驻服务进程在名称末尾加 `d`，其他程序不加此后缀。

核心策略进程命名为 `yquant-strategyd`。

## 目标平台

| 用途 | 操作系统 | CPU 架构 |
| --- | --- | --- |
| 生产环境 | Linux | x86_64 |
| 开发环境 | macOS | arm64、x86_64 |

生产环境执行构建、功能及并发验收；开发环境支持本机构建、功能测试和调试。

## 项目目录

- `include/yquant/base/`：基础组件公开头文件。
- `include/yquant/model/`：来源与标的标识、标的定义和一档报价。
- `include/yquant/strategy/`：策略生命周期、强类型配置与动态库协议。
- `src/base/`：基础组件及内部实现。
- `src/strategyd/`：策略进程与动态库加载实现。
- `examples/strategies/`：用户策略动态库示例。
- `tests/base/`：基础组件测试。
- `tests/strategyd/`：策略运行时与进程集成测试。
- `cmake/`：依赖与编译配置。
- `docs/design/`：架构与组件设计。
- `docs/research/`：系统研究资料与记录。

## 构建与测试

需要 CMake 3.21 或更高版本、Ninja 和支持 C++20 的编译器。日志依赖 fmt，配置解析依赖 simdjson，测试依赖 GoogleTest；依赖由 CMake 获取固定版本并以无异常配置构建。

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

## 策略进程

`yquant-strategyd` 读取 JSON 配置并加载用户策略动态库，全部校验成功后调用 `OnStart(context)`。主线程驱动行情、交易请求及委托和成交回报。收到停止信号后，宿主停止行情和策略新请求，自动撤单并处理回报；排空后调用 `OnStop(context)`、销毁实例并释放动态库句柄。

构建后运行生命周期策略示例：

```sh
./build/debug/bin/yquant-strategyd \
  --strategy ./build/debug/strategies/lifecycle_strategy.so \
  --config ./build/debug/strategies/lifecycle_strategy.json
```

`--strategy` 和 `--config` 均为必填参数，顺序不限。启动成功后输出 `Running` 和策略 ID。使用 `Ctrl+C` 或向进程发送 `SIGTERM` 停止，使用 `--help` 查看参数。路径以启动工作目录为基准；动态库文件名后缀由 CMake 的本机 `MODULE` 配置决定。

生命周期示例的 `LifecycleConfig` 派生自 `StrategyConfig`，读取必填字符串 `parameters.startup_message`，将解码后 1 至 128 字节、无内嵌 NUL 的内容保存在自身定长存储中，拒绝未知参数。配套配置将消息设为 `lifecycle strategy started`，`OnStart` 读取该参数并输出 `lifecycle_strategy: startup_message=lifecycle strategy started`。

用户策略继承 `yquant::strategy::Strategy`，导出 `YQuantGetStrategyPlugin()`，通过描述符提供创建和销毁函数。回调通过 `context.market_data()` 访问行情，通过 `context.trading()` 提交、撤销和查询委托，通过 `context.Now()` 查询单调时间。接口及所有权约定见[策略进程与动态策略加载设计](docs/design/strategyd.md)。

配置文件包含 `strategy_id` 和 `parameters`。框架校验公共字段，策略内嵌的 `StrategyConfig` 派生对象解析自定义参数；无自定义参数时使用基类并填写空对象。配置 ID 为 1 至 64 字节的 UTF-8 字符串，文件上限为 1 MiB，JSON 容器嵌套最多 64 层。

强类型参数示例见 [parameterized_strategy.cc](examples/strategies/parameterized_strategy.cc) 和[配套配置](examples/strategies/parameterized_strategy.json)。构建后将上述命令的动态库与配置文件名都换成 `parameterized_strategy` 即可运行；配置约定见[策略配置设计](docs/design/strategy-config.md)。

使用模拟行情驱动观察策略：

```sh
./build/debug/bin/yquant-strategyd \
  --strategy ./build/debug/strategies/quote_observer_strategy.so \
  --config ./build/debug/strategies/quote_observer_strategy.json \
  --market-data-config ./build/debug/strategies/mock_market_data.json
```

`--market-data-config` 为可选的宿主行情配置。示例使用来源 `1`，每 100 ms 生成 `SSE/600000` 的一档报价；策略通过 `parameters.source_id` 选择来源。首次行情输出 `quote_observer: first quote received`，停止时输出 `ticks`、`last_sequence` 和 `max_age_ns` 汇总。省略行情配置时，两个不订阅行情的示例仍可运行。标识、订阅及模拟规则见[行情驱动设计](docs/design/market-data.md)。

运行单笔限价委托示例：

```sh
./build/debug/bin/yquant-strategyd \
  --strategy ./build/debug/strategies/order_lifecycle_strategy.so \
  --config ./build/debug/strategies/order_lifecycle_strategy.json \
  --market-data-config ./build/debug/strategies/mock_market_data.json \
  --trading-config ./build/debug/strategies/mock_trading.json
```

`--trading-config` 为可选的宿主交易配置，使用时必须同时提供行情配置作为标的字典。示例在首次有效卖一报价提交 200 股买单，默认 `fill` 模式按 100 股、100 股两笔成交，输出 `order_lifecycle: terminal state=filled cumulative=200 leaves=0`。成交后进程继续运行，停止时输出 `order_lifecycle: stopped fills=2 filled_qty=200 state=filled`。

将交易配置的 `mode` 改为 `working`，等到 `order_lifecycle: working` 后发送停止信号，可验证宿主自动撤单和排空。交易接口、配置及收尾规则见[交易执行与模拟柜台](docs/design/trading.md)。交易服务同时提供持仓与资金查询，买单预占资金，卖单预占可卖持仓。

## 工程方案

- [工程组织与验证方案](docs/design/engineering.md)：平台、错误处理、构建依赖与测试工具方案。

## 组件设计

- [日志选型与设计](docs/design/logging.md)：异步日志库对比、队列策略与验证标准。
- [SPSC 队列设计](docs/design/spsc-queue.md)：低延迟内存消息队列设计。
- [策略进程与动态策略加载设计](docs/design/strategyd.md)：生命周期接口、动态库协议与命令执行流程。
- [策略配置设计](docs/design/strategy-config.md)：JSON 分层、强类型配置与启动校验。
- [行情驱动与策略事件循环](docs/design/market-data.md)：一档报价、策略上下文、模拟行情源与事件分派。
- [交易执行与模拟柜台](docs/design/trading.md)：限价委托、撤单与异步回报的执行闭环。
- [一档双边做市策略](docs/design/bbo-market-making.md)：策略伪代码、所需数据与回调语义。

## 参考系统与资料

- [交易系统参考资料](docs/research/trading-systems.md)：长期维护设计与开发参考的系统及其官方资料入口。
- [100 个 GitHub 参考系统](docs/research/github-trading-systems-100.md)：按 Stars、Forks 和仓库历史初筛，按研究／回测／实盘覆盖定位分组，各组按 Stars 排序，附指标快照与排除记录。
- [Tick 数据分类与固定 Slot 存储](docs/research/tick-data-model.md)：对照六个参考系统的行情载荷，整理市场差异、变长字段及定长槽容量边界。
