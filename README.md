# YQuant

## 项目简介

YQuant 是使用 C++ 编写的量化交易系统，采用 C++20，使用 CMake 管理构建配置。

项目不使用 C++ 异常。

日志等基础组件统一组织在 `yquant_base` 静态库中，供上层模块复用。

## 目标平台

| 用途 | 操作系统 | CPU 架构 |
| --- | --- | --- |
| 生产环境 | Linux | x86_64 |
| 开发环境 | macOS | arm64、x86_64 |

生产环境执行构建、功能、并发及跨进程共享内存验收；开发环境支持本机构建、功能测试和调试。

## 项目目录

- `include/yquant/base/`：基础组件公开头文件。
- `src/base/`：基础组件及内部实现。
- `tests/base/`：基础组件测试。
- `cmake/`：依赖与编译配置。
- `docs/design/`：架构与组件设计。
- `docs/research/`：系统研究资料与记录。

## 构建与测试

需要 CMake 3.21 或更高版本、Ninja 和支持 C++20 的编译器。测试依赖固定版本的 GoogleTest，由 CMake 获取并以无异常配置构建。

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

`release`、`asan` 和 `tsan` 使用相同的配置、构建与测试命令。ASan 同时启用 UBSan，TSan 使用独立构建目录。仅构建基础库时可设置 `-DBUILD_TESTING=OFF`。

## 工程方案

- [工程组织与验证方案](docs/design/engineering.md)：平台、错误处理、构建依赖与测试工具方案。

## 组件设计

- [日志选型与设计](docs/design/logging.md)：异步日志库对比、队列策略与验证标准。
- [SPSC 队列设计](docs/design/spsc-queue.md)：低延迟内存消息队列设计。

## 参考系统与资料

- [交易系统参考资料](docs/research/trading-systems.md)：长期维护设计与开发参考的系统及其官方资料入口。
- [100 个 GitHub 参考系统](docs/research/github-trading-systems-100.md)：按 Stars、Forks 和仓库历史初筛，按研究／回测／实盘覆盖定位分组，各组按 Stars 排序，附指标快照与排除记录。
