# 单进程运行

`monod --config PATH` 读取静态部署配置，创建 SimNow 行情和交易接入，再由 `yquant_runtime` 组装行情、交易 service、本地 client 及动态策略实例。策略加载使用[策略库接口](strategy.md#配置与动态加载)。当前入口运行一个账户、一个订阅合约和一个策略实例；该账户启动恢复所需的合约须包含在这一映射中。

## 配置

配置使用 `key=value`，空行和整行注释可省略。重复键、未知键和非法值会使启动失败。策略配置路径相对于运行配置文件解析；策略库路径相对于策略配置文件解析。

| 配置项 | 用途 |
| --- | --- |
| `strategy` | 策略配置文件 |
| `market_front`、`trading_front` | SimNow 行情和交易前置 |
| `exchange`、`symbol` | 柜台合约，映射到策略配置中的 `instrument_id` |
| `caller_service_id` | 同一进程共用的订单序号作用域 |
| `startup_seconds` | 等待账户就绪及可用行情的上限，默认 30 秒 |
| `shutdown_seconds` | 停策略后等待撤单与成交记账完成的上限，默认 10 秒 |
| `run_seconds` | 就绪并开始交付行情后的运行时长，0 表示运行到收到 SIGINT 或 SIGTERM |
| `close_priority` | 混合仓龄普通平仓的柜台优先顺序，取 `today`、`yesterday` 或 `unspecified` |

`YQUANT_CTP_BROKER_ID`、`YQUANT_CTP_USER_ID`、`YQUANT_CTP_PASSWORD`、`YQUANT_CTP_APP_ID` 和 `YQUANT_CTP_AUTH_CODE` 从环境读取。生产交易会话在启动同步前确认当日结算。macOS 前置需要终端系统信息时，通过 `YQUANT_CTP_SYSTEM_INFO_FILE` 指定文件。SDK 流文件写入进程当前目录，每个实例使用独立目录。

## 调度与退出

container 在一个业务线程上依次调度交易和行情 service。启动期间继续读取行情以推进登录和订阅；账户资金尚不可用时丢弃读到的行情。账户就绪后，第一条可用行情触发 `monod_started`，随后才交给策略。

达到运行时长或收到终止信号时，运行循环结束。宿主解除交易 listener、退订行情并销毁策略，然后通过 client 撤销属于本 caller、账户、策略和合约的活动订单。若候选撤单 ID 已用过，改用下一个未用 ID；该重复检查发生在 service 内，不向柜台重发。停止后继续调度交易回报，所有活动订单结束且资金重新可查询时输出 `monod_stopped`。

撤单拒绝、断连或超过关闭期限时返回非零退出码，并报告尚未确认的订单数量。已成交的持仓由账户继续持有，正常退出不会平仓。

## 验收

`v1_06_simnow_monod` 在独立测试程序中注入可控接入，读取两层配置，加载实际做市动态库，验证就绪前无报单、报价、改价、成交及停策略后的撤单结果。生产目标只依赖组件库。

真实有界验收前，将配置中的 `run_seconds` 设为正数，并核对合约与策略每侧手数。执行：

```sh
scripts/verify_simnow_monod /absolute/path/monod.conf --binary build/monod
```

脚本使用独立临时流文件目录启动生产程序，检查就绪与退出输出，要求观察到已确认撤单或已记账费用，并在超时时发送终止信号让进程执行关闭流程。未取得真实成交的场景保留成交验收待完成。
