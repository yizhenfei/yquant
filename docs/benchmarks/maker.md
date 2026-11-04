# 单进程报价延迟

运行 `yquant_scenarios --maker`。每轮向一个合约输入 1,280 条行情，每个调度周期交付一条，再由交易 service 处理模拟柜台回报。前 256 条用于预热，后 1,024 条交替改变买一价、保持卖一价不变。每条测量行情产生一次撤单和一次替换报单。

两条路径均从行情源写好盘口、返回给行情 service 前读取 `steady_clock` 开始计时：

- **撤单**：到模拟交易 adapter 收到撤单请求为止，包含行情分发、策略判断、交易 client 调用和 service 的撤单处理。
- **替换报单**：到同一条行情产生的新买单到达 adapter 为止，另含单线程调度、模拟撤单回报、订单构造与报单资金检查。每轮完成本次替换后才进入下一条行情。

测量使用真实 service、factory 和进程内 client，不包含网络、真实柜台和交易所耗时。策略状态和采样数组在循环前建立；交易 service 和模拟 adapter 的订单、撤单记录仍可能分配内存。测量期间不逐笔打印，循环结束后排序，按最近秩计算分位数。

## 基线

2026-11-03，Apple M4、macOS 26.3 arm64、Apple Clang 21.0.0，CMake Release（`-O3 -DNDEBUG`），单业务线程、未绑核。连续运行五轮，表中为各轮对应分位数的中位数，单位为纳秒。

| 路径 | P50 | P95 | P99 |
| --- | ---: | ---: | ---: |
| 行情 → 撤单 | 83 | 125 | 167 |
| 行情 → 替换报单 | 250 | 250 | 1583 |

复测命令：

```sh
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build-release --target yquant_scenarios
./build-release/yquant_scenarios --maker
```
