# YQuant

YQuant 是使用 C++ 编写的多策略、低延迟量化交易系统。

## 目标平台

| 用途 | 操作系统 | CPU 架构 |
| --- | --- | --- |
| 运行与验收环境 | Linux | x86_64 |
| 开发环境 | macOS | arm64、x86_64 |

Linux 执行正式构建、功能及并发验收；macOS 支持本机构建、功能测试和调试。

## 构建与验证

使用支持 C++20 的编译器、CMake 3.20 或更高版本，以及与目标平台匹配的 CTP 行情和交易 SDK。Linux 和 macOS 的默认构建都链接两套厂商动态库。先按[SDK 安装说明](docs/design/simnow-market.md#构建依赖)准备 `vendor/ctp/`，或在配置时指定 `-DYQUANT_CTP_SDK_ROOT=/path/to/sdk`：

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
./build/monod
```

`monod` 在 `main` 中将行情 service 按 ID 注册到本地实例表，再把实例表注入 factory，取得行情 client。container 调度行情 service，由 service 内部接收两个品种交错的五条固定行情，只为订阅品种输出报价意图。`main` 向 container 提供退出条件，固定行情处理完成后结束运行，随后退订并退出。固定行情源位于 `examples/market/`，验证策略位于 `examples/strategies/`。

运行 `ctest --test-dir build -L v1_04_simnow_market --output-on-failure` 验证可控行情接入。使用有效 SimNow 行情前置和账户时，按[真实连接验收说明](docs/design/simnow-market.md#验收)运行 `scripts/verify_simnow_market`，逐条比较接收的买卖价与策略输出。

个人 SimNow 交易账户的登录入口是 `monod --simnow-trader-login FRONT [SECONDS]`。查询启动资金、持仓和当日订单与成交使用 `monod --simnow-startup-snapshot FRONT [SECONDS [SYMBOL]]`。指定合约后也返回合约乘数和费率。两者复用同一组登录参数，见[交易前置登录](docs/design/trading.md#交易前置登录)。

运行交易报单验证场景：

```sh
./build/monod --maker
./build/monod --orders
./build/monod --cancel
./build/monod --fills
./build/monod --funds
./build/monod --positions
./build/monod --reserve
./build/monod --startup
./build/monod --funds-lifecycle
./build/monod --positions-lifecycle
ctest --test-dir build -L v1_02_order --output-on-failure
```

`--maker` 在单进程中运行[简单做市策略](docs/design/strategy.md)，输出双边报单、改价撤单、最终挂单及[延迟分位数](docs/benchmarks/maker.md)。

交易场景通过本地 factory 创建 client，向两个模拟账户报单，验证委托接受、柜台拒绝和确定未发送，并验证发送失败后迟到的有效回报能够更新订单状态。委托回报与汇总写入 `stdout`，发送失败及迟到回报诊断写入 `stderr`。

`--cancel` 在已有委托上执行两个账户的撤单，分别验证柜台确认与拒绝，并输出原订单的撤销状态和各次撤单结果。

`--fills` 在两个账户中分别产生部分和全部成交，通过 client 查询两笔订单的最终状态，并输出成交明细和查询结果。

`--funds` 验证两个账户的资金查询，完成一次启动同步后核对各自的币种和金额；零资金与数据尚未就绪分别表达。

`--positions` 验证两个账户的完整及单项持仓查询，覆盖多个品种和有效空持仓，并核对账户间数据独立。

`--reserve` 验证共享账户的资金检查与下单预占：余额 100 时两笔各需 70 的订单只发送一笔；确定未发送的请求释放预占，后续订单可以使用释放的资金。

`--startup` 验证初始化失败、显式重试、资金与持仓共同就绪，以及迟到快照不覆盖本地数据。当前启动前活动订单或未关联冻结仍会阻止就绪；[N5.2 至 N5.8](docs/plans/v1.md#n52启动资金与持仓快照) 优先修正这一限制，从柜台重建挂单及冻结关联。

`--funds-lifecycle` 验证部分成交记账、撤单释放剩余预占及平仓盈利，输出保证金、手续费、已实现盈亏和可用资金。初始非空持仓须提供今昨仓成本及保证金；本地按不含浮动盈亏的账面资金口径记账。

`--positions-lifecycle` 验证两个策略向同一账户各申请平仓 7 手，10 手持仓只允许发送一笔；部分成交后撤单，剩余可平量可再次使用。

测试构建获取固定版本的 GoogleTest；离线构建可通过 `-DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=/path/to/googletest` 指定已有源码。

## 首版目标

在 SimNow 环境中运行多个简单做市策略，允许策略共用同一个交易账户，验证从接收行情、生成报价、提交委托到处理成交和撤单的完整交易闭环。

做市策略用于验证系统功能，保持简单，不设盈利目标。策略根据行情生成双边报价，在报价变化时撤单并重新报价，处理委托和成交回报；具体报价规则在策略设计中细化。

### 功能范围

| 能力 | 首版范围 |
| --- | --- |
| 行情接入 | 接入 SimNow 行情，向策略提供所需行情。 |
| 多策略运行 | 支持多个策略实例同时运行，各策略独立生成交易指令，并允许共用同一个交易账户。 |
| 交易执行 | 支持报单、撤单和回报处理，维护订单及成交状态，将订单和成交回报准确分发给所属策略。 |
| 账户状态 | 启动时同步账户资金和持仓，运行中由交易事件更新本地数据，提供带可用性的查询；共享账户统一检查与预占资金。运行中对账及持久化恢复由独立里程碑 N13 交付。 |

首版支持同进程、同机跨进程和跨机器部署，提供跨机点对点通信与单机广播，不包含跨机广播。部署拓扑通过静态配置文件提供。

首版不包含回测、研究和运行管理。风险控制中的停止报价与集中撤回挂单，以及运行管理中的配置分发、日志和运行状态管理，均不纳入首版功能范围。

### 验收标准

- 多个做市策略实例能够同时接收行情并独立生成交易指令。
- 多个策略能够通过同一个 SimNow 交易账户完成报单和撤单。
- 订单和成交回报能够正确归属并分发给对应策略。
- 策略能够查询账户资金及持仓，数据与对应的柜台快照一致；能区分数据尚不可用与有效的零资金、空持仓，接收后续更新且不同账户的数据相互独立。
- 报单前统一检查并预占账户资金，资金不足时拒绝发送；发送失败、拒单、撤单和成交后本地资金及持仓变化正确，不重复扣减或释放。
- 通过 N13 验证订单、资金、持仓的运行中对账、差异处理和持久化保存，进程重启后恢复并经柜台核实继续交易。
- 在同进程、同机跨进程和跨机器部署下，做市策略能够根据行情变化更新报价，完成行情驱动的交易闭环。
- 单机广播能够向本机多个策略分发行情。

## 项目文档

- [系统架构](docs/design/architecture.md)
- [交易接口](docs/design/trading.md)
- [首版执行计划](docs/plans/v1.md)
- [项目 TODO](TODO.md)
