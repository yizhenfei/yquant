# SimNow 交易账户登录验收记录

## 2026-11-03 · macOS arm64

使用 `monod --simnow-trader-login tcp://182.254.243.31:30001 30` 和厂商交易 framework，在独立临时目录中通过应用认证及个人模拟账户登录。

```text
trader_logged_in trading_day=20261103 account_ready=false
```

进程退出码为 0。柜台确认交易日为 20261103；此次仅验证登录，没有同步资金和持仓，也没有报单或撤单。账户就绪由 N5.2 验收。
