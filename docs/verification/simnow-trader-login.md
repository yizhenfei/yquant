# SimNow 交易账户验收记录

## 2026-11-03 · macOS arm64

### 交易登录

使用 `monod --simnow-trader-login tcp://182.254.243.31:30001 30` 和厂商交易 framework，在独立临时目录中通过应用认证及个人模拟账户登录。

```text
trader_logged_in trading_day=20261103 account_ready=false
```

进程退出码为 0。柜台确认交易日为 20261103；此次仅验证登录，没有同步资金和持仓，也没有报单或撤单。账户就绪由 N5.8 验收。

### 启动快照

同一账户在独立临时目录中运行 `monod --simnow-startup-snapshot tcp://182.254.243.31:30001 30`。柜台返回一个币种的资金记录，持仓查询正常结束且没有持仓行。进程退出码为 0。

```text
startup_snapshot trading_day=20261103 currencies=1 position_rows=0 account_ready=false
```

### 合约计费参数

运行 `monod --simnow-startup-snapshot tcp://182.254.243.31:30001 30 TF2612`，进程退出码为 0。交易日为 20261103，个人模拟账户的投机费率查询结果如下。

| 参数 | 柜台返回值 |
| --- | --- |
| 合约乘数 | 10000 |
| 多头、空头按金额保证金率 | 各 0.022 |
| 多头、空头每手保证金 | 各 0 |
| 每手开仓、平仓、平今手续费 | 3、3、0 |
| 按金额开仓、平仓、平今手续费率 | 均为 0 |
