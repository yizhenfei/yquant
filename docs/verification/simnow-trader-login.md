# SimNow 交易账户验收记录

## 2026-11-03 · macOS arm64

### 交易登录

使用 `monod --simnow-trader-login tcp://182.254.243.31:30001 30` 和厂商交易 framework，在独立临时目录中通过应用认证及个人模拟账户登录。

```text
trader_logged_in trading_day=20261103 account_ready=false
```

进程退出码为 0。柜台确认交易日为 20261103；此次仅验证登录，没有同步资金和持仓，也没有报单或撤单。账户就绪由 N5.6 验收。

### 启动快照

同一账户在独立临时目录中运行 `monod --simnow-startup-snapshot tcp://182.254.243.31:30001 30`。柜台返回一个币种的资金记录，持仓查询正常结束且没有持仓行。进程退出码为 0。

```text
startup_snapshot trading_day=20261103 currencies=1 position_rows=0 account_ready=false
```
