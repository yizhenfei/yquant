# 单进程运行

`monod --config PATH` 读取静态部署配置，创建 SimNow 行情和交易接入，再由 `yquant_runtime` 组装行情、交易 service、本地 client 及动态策略实例。策略实例由 `StrategyService` 管理，加载使用[策略库接口](strategy.md#配置与动态加载)。一个进程可组装多个策略 service，每个 service 可管理多个策略实例，所有实例共享一个交易账户。每个 service 使用独立的 caller ID，组内实例共用订单序号生成器。

## 配置

配置使用 `key=value`，空行和整行注释可省略。重复键、未知键和非法值会使启动失败。策略配置路径相对于运行配置文件解析；策略库路径相对于策略配置文件解析。

| 配置项 | 用途 |
| --- | --- |
| `strategy.<caller_id>.<strategy_id>` | 一个策略实例的配置文件；相同 caller ID 的实例归入同一策略 service |
| `instrument.<instrument_id>` | `交易所 合约 [平仓优先顺序]`，用于行情订阅和交易启动恢复 |
| `market_front`、`trading_front` | SimNow 行情和交易前置 |
| `startup_seconds` | 等待账户就绪及可用行情的上限，默认 30 秒 |
| `shutdown_seconds` | 停策略后等待撤单与成交记账完成的上限，默认 10 秒 |
| `run_seconds` | 就绪并开始交付行情后的运行时长，0 表示运行到收到 SIGINT 或 SIGTERM |

平仓优先顺序取 `today`、`yesterday` 或 `unspecified`，省略时为 `unspecified`。所有策略和账户恢复所需的合约都须在映射中列出。内部合约 ID 与柜台合约一一对应，行情订阅无法区分的跨交易所同名合约会被拒绝。

例如，两个 service 共同运行三个实例，其中前两个交易同一合约：

```ini
strategy.63001.11=market_maker.conf
strategy.63001.12=market_maker_second.conf
strategy.63002.11=market_maker_other.conf
instrument.42=CFFEX TF2612 yesterday
instrument.81=CFFEX T2612 yesterday
```

策略文件中的 `strategy_id` 必须与部署键一致，所有实例的 `account_id` 必须相同。策略初始化时建立的订阅必须存在于合约映射中。策略 ID 在各自 service 内唯一；不同 service 可以复用策略 ID。重复的 caller/策略组合和合约标识在创建接入前报错；全部实例初始化后校验实际订阅映射，缺失映射时清理实例并拒绝启动业务循环。构建目录生成的 `monod.multi.conf` 和三个 `market_maker*.conf` 提供完整样例，运行时长为 1 秒。

单实例配置使用 `strategy` 和 `caller_service_id`，读取后转换成单实例组；合约同样通过 `instrument.<id>` 显式映射，可配置多个。单实例字段不能与 `strategy.<caller_id>.<strategy_id>` 混用。旧配置中的 `exchange`、`symbol`、`close_priority` 合并为 `instrument.<id>=交易所 合约 平仓优先顺序`，策略文件中的 `instrument_id` 改为做市插件的 `param.instrument_id`。

`YQUANT_CTP_BROKER_ID`、`YQUANT_CTP_USER_ID`、`YQUANT_CTP_PASSWORD`、`YQUANT_CTP_APP_ID` 和 `YQUANT_CTP_AUTH_CODE` 从环境读取。生产交易会话在启动同步前确认当日结算。macOS 前置需要终端系统信息时，通过 `YQUANT_CTP_SYSTEM_INFO_FILE` 指定文件。SDK 流文件写入进程当前目录，每个进程使用独立目录。

## 调度与退出

宿主先加载所有策略 service 的实例，并为每个实例创建独立 client；任一加载或初始化失败会清理此前创建的实例，业务循环不会启动。container 在一个业务线程上依次调度交易和行情 service。启动期间继续读取行情以推进登录和订阅；账户资金或整组行情订阅尚不可用时丢弃读到的行情。二者就绪后，第一条可用行情触发 `monod_started services=N strategies=N account=ID`，随后经各 client 交给订阅该合约的策略实例。

达到运行时长或收到终止信号时，运行循环结束。宿主先关闭所有实例的行情、交易 client，再销毁实例。退出撤单使用另一个绑定到宿主接收者的交易 client，按各实例的 caller、账户和策略 ID 收集所有合约的活动订单并撤销。若候选撤单 ID 已用过，改用下一个未用 ID；该重复检查发生在 service 内，不向柜台重发。每笔撤单保留对应的账户及策略归属，成功确认时输出 `monod_canceled`，包含 caller ID、策略 ID、订单序号、撤单请求 ID 和累计成交量，重复回报不重复输出。停止后继续调度交易回报，所有活动订单结束、撤单结果收齐且资金重新可查询时输出 `monod_stopped`。未知归属订单继续由账户独立记账，不加入这些实例的退出撤单集合。

某笔撤单被拒绝时仍等待其他实例的在途结果；存在未结束订单且本轮撤单结果已收齐、断连或超过关闭期限时，返回非零退出码。异常输出逐笔列出未确认订单及撤单的 caller ID、策略 ID、订单序号和撤单请求 ID。已成交的持仓由账户继续持有，正常退出不会平仓。

## 验收

`v1_06_simnow_monod` 在独立测试程序中注入可控接入，读取两层配置，加载实际做市动态库，验证就绪前无报单、报价、改价、成交及停策略后的撤单结果。生产目标只依赖组件库。

`v1_07_multi_runtime` 读取多服务配置，运行两个 service、三个实例，核对共享账户、独立 caller、组内序号与回报归属；覆盖配置冲突、整组加载回滚、全部实例就绪前的行情门槛、多实例退出撤单、超时诊断，以及实例销毁后的重复回报和迟到成交。

`v1_07_multi_strategy` 加载独立测试策略，通过同一账户检查资金竞争、持仓冻结、成交记账、释放复用及查询可用性，同时核对其他账户不受影响。

真实有界验收前，将配置中的 `run_seconds` 设为正数，并核对合约与策略每侧手数。执行：

```sh
scripts/verify_simnow_monod /absolute/path/monod.conf --binary build/monod
```

脚本使用独立临时流文件目录启动生产程序，检查就绪与退出输出，要求观察到已确认撤单或已记账费用，并在超时时发送终止信号让进程执行关闭流程。未取得真实成交的场景保留成交验收待完成。

多策略报撤单验收增加 `--require-all-strategies`：

```sh
scripts/verify_simnow_monod /absolute/path/monod.multi.conf --binary build/monod --require-all-strategies
```

该模式要求配置至少两个实例，并按 caller ID 与策略 ID 的组合检查每个实例都有退出撤单确认；同一实例的多条确认不能替代其他实例。若某实例全部成交而没有退出撤单，其成交验收需单独核对，此脚本不会把聚合费用作为该实例的验收依据。
