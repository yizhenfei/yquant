# SimNow 单进程做市验收记录

## 2026-11-04 · macOS arm64

生产 `monod` 通过运行配置和策略配置加载 `libyquant_market_maker.dylib`，连接 SimNow 第一套交易与行情前置。账户初始化通过后，在 CFFEX `TF2612` 上每侧报价 1 手，运行 1 秒，再停止策略并等待关闭流程。caller 为 63001，策略 ID 为 11，逻辑账户 ID 为 7，内部合约 ID 为 42。

```text
monod_started strategy=11 account=7 instrument=42
monod_stopped canceled=2 remaining=0 commission=0 realized=0
simnow_monod_verified
```

验收脚本退出码为 0。两个活动订单均收到撤销确认，运行结束时活动订单为 0。未取得实际成交，真实成交路径仍待验收。

此前同样配置尝试 SHFE `ag2612`，行情登录及订阅成功，但在启动期限内没有可用盘口，进程报告超时，未发单。随后只读订阅确认 `TF2612` 有盘口后执行上述验收。Linux x86_64 运行待验收。
