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
- `include/yquant/strategy/`：策略生命周期接口与动态库协议。
- `src/base/`：基础组件及内部实现。
- `src/strategyd/`：策略进程与动态库加载实现。
- `examples/strategies/`：用户策略动态库示例。
- `tests/base/`：基础组件测试。
- `tests/strategyd/`：策略运行时与进程集成测试。
- `cmake/`：依赖与编译配置。
- `docs/design/`：架构与组件设计。
- `docs/research/`：系统研究资料与记录。

## 构建与测试

需要 CMake 3.21 或更高版本、Ninja 和支持 C++20 的编译器。日志依赖 fmt，测试依赖 GoogleTest；二者由 CMake 获取固定版本并以无异常配置构建。

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

`release`、`asan` 和 `tsan` 使用相同的配置、构建与测试命令。ASan 同时启用 UBSan，TSan 使用独立构建目录。不构建测试程序时可设置 `-DBUILD_TESTING=OFF`；只构建基础库时使用 `cmake --build --preset debug --target yquant_base`。

日志接口位于 `yquant/base/logging.h`。Debug 默认编译保留 DEBUG 及以上级别，其他构建保留 INFO 及以上；可在配置时使用 `-DYQUANT_LOG_COMPILED_LEVEL=TRACE` 等选项调整。

## 策略进程

`yquant-strategyd` 加载用户策略动态库，调用策略的 `OnStart()`，保持实例运行，收到停止信号后调用 `OnStop()`、销毁实例并释放动态库句柄。

构建后运行生命周期策略示例：

```sh
./build/debug/bin/yquant-strategyd \
  --strategy ./build/debug/strategies/lifecycle_strategy.so
```

启动成功后输出 `Running`。使用 `Ctrl+C` 或向进程发送 `SIGTERM` 停止，使用 `--help` 查看参数。动态库路径以启动工作目录为基准；文件名后缀由 CMake 的本机 `MODULE` 配置决定。

用户策略继承 `yquant::strategy::Strategy`，导出 `YQuantGetStrategyPlugin()`，通过描述符提供创建和销毁函数。示例位于 [lifecycle_strategy.cc](examples/strategies/lifecycle_strategy.cc)，接口及所有权约定见[策略进程与动态策略加载设计](docs/design/strategyd.md)。

## 工程方案

- [工程组织与验证方案](docs/design/engineering.md)：平台、错误处理、构建依赖与测试工具方案。

## 组件设计

- [日志选型与设计](docs/design/logging.md)：异步日志库对比、队列策略与验证标准。
- [SPSC 队列设计](docs/design/spsc-queue.md)：低延迟内存消息队列设计。
- [策略进程与动态策略加载设计](docs/design/strategyd.md)：生命周期接口、动态库协议与命令执行流程。
- [一档双边做市策略](docs/design/bbo-market-making.md)：策略伪代码、所需数据与回调语义。

## 参考系统与资料

- [交易系统参考资料](docs/research/trading-systems.md)：长期维护设计与开发参考的系统及其官方资料入口。
- [100 个 GitHub 参考系统](docs/research/github-trading-systems-100.md)：按 Stars、Forks 和仓库历史初筛，按研究／回测／实盘覆盖定位分组，各组按 Stars 排序，附指标快照与排除记录。
- [Tick 数据分类与固定 Slot 存储](docs/research/tick-data-model.md)：对照六个参考系统的行情载荷，整理市场差异、变长字段及定长槽容量边界。
