# SimNow 行情连接验收记录

## 2026-11-03 · macOS arm64

运行当前工作树的 `build/monod`，加载 CTP 行情 framework，通过公开行情前置空登录订阅 TF2612。验收脚本在独立的临时流文件目录内启动程序，逐条比较收到的盘口与验证策略输出，退出码为 0。

```sh
scripts/verify_simnow_market --anonymous --front tcp://182.254.243.31:30011 --symbol TF2612 --count 1 --timeout 25
```

```text
simnow_connected
simnow_logged_in
simnow_subscribed symbol=TF2612
simnow_quote symbol=TF2612 instrument=1 sequence=1 bid=106.83 ask=106.83499999999999 bid_qty=25 ask_qty=52
quote_intent instrument=1 sequence=1 bid=106.83 ask=106.83499999999999
simnow_summary symbol=TF2612 quotes=1
```

该前置在本次行情登录响应中返回请求编号 0；接入只在等待唯一登录请求时接受 0 或发出的请求编号 1，仍检查错误码和最终响应。Linux x86_64 正式运行验收单独记录。
