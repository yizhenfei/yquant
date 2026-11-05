# 构建与运行

以下命令在仓库根目录执行。目标平台和首版范围见 [README](../README.md)。

## 构建与验证

使用支持 C++20 的编译器、CMake 3.20 或更高版本，以及与目标平台匹配的 CTP 行情和交易 SDK。Linux 和 macOS 的行情、交易接入组件均使用厂商动态库。先按[SDK 安装说明](design/simnow-market.md#构建依赖)准备 `vendor/ctp/`，或在配置时指定 `-DYQUANT_CTP_SDK_ROOT=/path/to/sdk`：

```sh
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
./build/yquant_scenarios
```

`yquant_scenarios` 在 `main` 中将行情 service 按 ID 注册到本地实例表，再把实例表注入 factory，取得行情 client。container 调度行情 service，由 service 内部接收两个品种交错的五条固定行情，只为订阅品种输出报价意图。`main` 向 container 提供退出条件，固定行情处理完成后结束运行，随后退订并退出。固定行情源位于 `examples/market/`，验证策略位于 `examples/strategies/`。

运行 `ctest --test-dir build -L v1_04_simnow_market --output-on-failure` 验证可控行情接入。使用有效 SimNow 行情前置和账户时，按[真实连接验收说明](design/simnow-market.md#验收)运行 `scripts/verify_simnow_market`，逐条比较接收的买卖价与策略输出。

多合约行情接入使用 `SimNowMarketConfig::subscriptions`。可控验收运行 `ctest --test-dir build -R v1_07_multi_market --output-on-failure`，真实只读程序见[多合约订阅](design/simnow-market.md#多合约订阅)。

独立测试程序中，个人 SimNow 交易账户的登录入口是 `simnow_trader_probe FRONT [SECONDS]`。查询启动资金、持仓和当日订单与成交使用 `simnow_trader_probe --snapshot FRONT [SECONDS [SYMBOL]]`。指定合约后也返回合约乘数和费率。两者复用同一组登录参数，见[交易前置登录](design/trading.md#交易前置登录)。

运行交易验证场景：

```sh
./build/yquant_scenarios --maker
./build/yquant_scenarios --orders
./build/yquant_scenarios --cancel
./build/yquant_scenarios --fills
./build/yquant_scenarios --funds
./build/yquant_scenarios --positions
./build/yquant_scenarios --reserve
./build/yquant_scenarios --startup
./build/yquant_scenarios --funds-lifecycle
./build/yquant_scenarios --positions-lifecycle
ctest --test-dir build -L v1_02_order --output-on-failure
```

`--maker` 在单进程中运行[简单做市策略](design/strategy.md)，输出双边报单、改价撤单、最终挂单及[延迟分位数](benchmarks/maker.md)。

交易场景通过本地 factory 创建 client，向两个模拟账户报单，验证委托接受、柜台拒绝和确定未发送，并验证发送失败后迟到的有效回报能够更新订单状态。委托回报与汇总写入 `stdout`，发送失败及迟到回报诊断写入 `stderr`。

`--cancel` 在已有委托上执行两个账户的撤单，分别验证柜台确认与拒绝，并输出原订单的撤销状态和各次撤单结果。

`--fills` 在两个账户中分别产生部分和全部成交，通过 client 查询两笔订单的最终状态，并输出成交明细和查询结果。

`--funds` 验证两个账户的资金查询，完成一次启动同步后核对各自的币种和金额；零资金与数据尚未就绪分别表达。

`--positions` 验证两个账户的完整及单项持仓查询，覆盖多个品种和有效空持仓，并核对账户间数据独立。

`--reserve` 验证共享账户的资金检查与下单预占：余额 100 时两笔各需 70 的订单只发送一笔；确定未发送的请求释放预占，后续订单可以使用释放的资金。

`--startup` 验证初始化失败、显式重试、资金与持仓共同就绪，以及迟到快照不覆盖本地数据。SimNow 有活动挂单时，执行 `v1_05_account_startup` 验证前后两次历史核对、冻结恢复及策略接管。真实账户的只读验收入口见[启动边界与挂单接管](design/trading.md#启动边界与挂单接管)。

`--funds-lifecycle` 验证部分成交记账、撤单释放剩余预占及平仓盈利，输出保证金、手续费、已实现盈亏和可用资金。初始非空持仓须提供今昨仓成本及保证金；本地按不含浮动盈亏的账面资金口径记账。

`--positions-lifecycle` 验证两个策略向同一账户各申请平仓 7 手，10 手持仓只允许发送一笔；部分成交后撤单，剩余可平量可再次使用。

测试构建获取固定版本的 GoogleTest；离线构建可通过 `-DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=/path/to/googletest` 指定已有源码。

## 生产程序构建

```sh
cmake -S . -B build-production -DBUILD_TESTING=OFF
cmake --build build-production --target monod yquant_market_maker
./build-production/monod --config /absolute/path/monod.conf
```

构建目录生成 `monod.conf` 和 `market_maker.conf`。复制这两个配置到部署目录，调整库路径、账户及合约映射；凭据通过交易登录环境变量提供。`monod.multi.conf` 和 `market_maker*.conf` 提供两个策略 service、三个实例共用账户的样例。生产运行方式与退出行为见[单进程运行](design/monod.md)。

验证场景在 `tests/scenarios/` 中组装，仅在 `BUILD_TESTING=ON` 时构建。`yquant_scenarios` 运行固定输入与延迟测量，`simnow_market_probe` 和 `simnow_trader_probe` 提供行情、登录与快照探测。既有 CTest 中以 `_monod` 结尾的场景名称保留，执行目标已切换到独立测试程序。

运行动态加载验收：

```sh
cmake --build build --target strategy_plugin_test
ctest --test-dir build -R '^v1_06_strategy_plugin$' --output-on-failure
```

库接口、兼容性和停止顺序见[策略设计](design/strategy.md#配置与动态加载)。多实例 service 的验收入口为 `ctest --test-dir build -R v1_07_strategy_instances --output-on-failure`。
