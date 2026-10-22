# 国内期货 Tick 数据渠道

核查日期：2026-10-22。

## 数据口径

国内期货常见的 Tick 是周期推送的行情快照，包含最新价、累计成交量和盘口，
不能直接解释为每笔成交或每条委托事件。快照之间的累计成交量差只能表示该区间的
成交量，不能还原成交顺序、主动方向或真实排队位置。具体推送频率和盘口档数取决于
交易所、行情产品及接入权限，不统一假定为固定频率或五档。

买卖一价量（BBO）足以驱动当前双边报价策略的行情输入；逐笔委托、逐笔成交、
深度增量和队列位置需要另行选择相应的数据产品。事件分类见
[Tick 数据分类](tick-data-model.md)。

## 实时行情与历史数据服务

| 渠道 | 实时与历史能力 | 账户及费用条件 | 官方资料 |
| --- | --- | --- | --- |
| CTP／SimNow、期货公司 CTP | 行情 API 订阅推送期货快照；标准行情 API 不提供历史 Tick 批量下载，历史回放数据需自行录制或另购。 | 行情前置可配置为不校验账户，允许空登录；是否启用校验取决于服务端。SimNow 账户用于需要身份校验的接入及模拟交易；期货公司接入权限由其提供。SimNow 的生产时段环境与 API 测试环境用途、可用时段不同。 | [SimNow 环境说明](https://www.simnow.com.cn/product.action)、[官方 API 与文档下载](https://www.simnow.com.cn/static/apiDownload.action)、[openctp 空登录示例](https://github.com/openctp/openctp/blob/master/ctpapi-python/demo/md_demo.py)、[Jedore 当前环境配置](https://github.com/Jedore/ctp.examples/blob/main/openctp-ctp/config.py) |
| 天勤 TqSdk | 免费版提供国内期货实时行情；提供历史 Tick 和 Tick 级回测。批量历史下载属于专业版功能，可经 `DataDownloader` 或历史序列接口获取。 | 运行任何 TqSdk 程序均需快期账户并传入 `TqAuth`。专业版需申请试用或购买，官方提供 15 天试用入口；历史范围和具体权限按账户核对。 | [快期账户](https://doc.shinnytech.com/tqsdk/latest/usage/shinny_account.html)、[专业版功能](https://doc.shinnytech.com/tqsdk/latest/profession.html)、[API 参考](https://doc.shinnytech.com/tqsdk/latest/reference/tqsdk.api.html) |
| 米筐 RQData | 提供中国期货历史日线、分钟、Tick 及实时 Tick、分钟数据；历史查询可用 `get_price(..., frequency='tick')`，实时快照可用 `current_snapshot()`。 | 需要有效 RQData 授权并初始化登录。官方区分试用与付费账户，试用有每日流量配额；申请时需核对期货历史及实时权限。 | [数据范围](https://www.ricequant.com/doc/rqdata/python/)、[行情 API](https://www.ricequant.com/doc/rqdata/python/generic-api)、[初始化与配额](https://assets.ricequant.com/doc/rqdata/python/manual)、[授权要求](https://www.ricequant.com/doc/rqdata/cli/manual) |
| Tushare | 官方期货历史 Tick 产品提供全市场一档行情，页面称覆盖近 10 年并每日增量更新；以 CSV 网盘交付，当前不提供该产品的 API 下载。期货实时分钟是另一产品，不能替代实时 Tick。 | 历史 Tick 为单独数据服务，不属于积分权限；需联系官方获取交付范围及报价。普通 Pro 账户或积分不足以获得该产品。 | [期货历史 Tick](https://tushare.pro/document/2?doc_id=314)、[权限说明](https://tushare.pro/document/2?doc_id=290) |

openctp 维护者的行情示例使用空 `CThostFtdcReqUserLoginField` 登录并订阅行情。
本项目已验证 SimNow 第一套公开行情前置接受空登录并提供新鲜 BBO，实测记录见
下文。服务端可调整鉴权策略或前置地址，接入前应核对当前公开配置。公开网页报价、
第三方转发和未知公共 CTP 前置不作为默认接入来源。

## 字段与历史一致性

| 渠道 | 与策略直接相关的字段 | 接入及回放时需核对的口径 |
| --- | --- | --- |
| CTP | `BidPrice1/BidVolume1`、`AskPrice1/AskVolume1`；另有 `LastPrice`、`Volume`、`Turnover`、`OpenInterest`，时间涉及 `TradingDay`、`ActionDay`、`UpdateTime`、`UpdateMillisec`。 | API 结构中存在多档字段不代表所订阅产品提供所有档。区分交易日与夜盘自然日期；记录本机接收时间并处理缺侧及无效价。字段定义以所用版本的官方 SDK 为准。 |
| TqSdk | `Quote`／`Tick` 提供 `bid_price1/bid_volume1`、`ask_price1/ask_volume1`；Tick 还含时间、最新价、累计成交量、成交额及持仓量。 | 实时可用 `get_quote()`／`get_tick_serial()`；历史下载接口及权限单独核对，不能把实时滚动窗口当成任意区间历史下载。见[业务对象](https://doc.shinnytech.com/tqsdk/latest/reference/tqsdk.objs.html)。 |
| RQData | 历史 Tick 使用 `b1/b1_v`、`a1/a1_v`，另有 `datetime`、`last`、`volume`、`total_turnover`、`open_interest`；实时快照用 `bids/bid_vols`、`asks/ask_vols` 数组。 | 官方期货说明称五档目前仅上期所提供，范围为 2020-05-12 至今，其余交易所一档。历史与实时接口命名不同，需按实际合约、日期和权限核对。见[期货数据说明](https://www.ricequant.com/doc/rqdata/python/futures-mod)。 |
| Tushare | CSV 含 `InstrumentID`、买卖一价量、最新价、成交量、成交额、持仓量、涨跌停、交易日期及更新时间。 | 下载产品用于离线研究；接入其他实时源时，需另查两者的时间、采样、缺失值和累计量口径。官方页面未承诺该 CSV 与本项目 CTP 前置逐条一致。 |

同一供应商提供实时与历史，不自动保证两者逐条一致。跨供应商历史数据可以扩大
研究区间，但不能替代实时采集链路的回放验收。复现一次实时运行，应保存同一接入
会话实际收到的消息顺序、买卖一价量和时间信息。

## YQuant 选源

当前采用只读 CTP 行情桥，经 `marketd` 录制后转换为有限 CSV 回放，保留期货
双边报价策略。CTP 的 BBO 字段满足策略输入，自行录制可使实时运行与历史回放
使用同一批行情。接入方式与验收步骤集中维护于
[CTP 行情接入与同源回放](../design/ctp-market-data.md)。

行情桥通过显式 `--anonymous` 选择空登录，适用于当前允许此方式的公开行情前置；
启用身份校验的前置使用账户接入。前置地址与鉴权要求以服务提供方当前配置为准。

2026-10-22（北京时间），通过 SimNow 第一套行情前置
`tcp://182.254.243.31:30011` 匿名订阅 `CFFEX/TF2612`，12:58:18 开始采集，
13:00—13:01 完成原双边策略运行。共保存 61 帧，包含
1 帧不可报价的午间缓存和 60 帧新鲜双侧报价；本次实时更新约为每秒 1 次。
本机运行清单位于 `test_data/ctp/2026-10-22/tf2612_live/manifest.json`。
同源录制随后以 1 倍速度完成回放，61 帧的接收间隔、事件时间、买卖一价量和标志
逐条比对一致。实时及回放均驱动原策略提交买卖两侧委托、完成 4 笔模拟成交；
退出时净仓、总仓、预占、冻结量和活动委托数均为零。回放清单位于
`test_data/ctp/2026-10-22/tf2612_replay/manifest.json`。
录制与运行日志保存在本机 `test_data/`，不纳入 Git；重新采集及回放命令见接入设计。

同日 13:07:20—13:17:20 扩大采集 10 分钟，保存 661 帧有效双侧报价，包含
581 种买卖一价量组合和 28 次买卖一价格变化。导出 CSV 的时间、价量和标志与录制
逐条一致，CSV、录制清单及样本统计位于本机
`test_data/ctp/2026-10-22/tf2612_record_10m/`。

真实行情验收须在对应服务时段收到新鲜双侧报价并完成录制封存，再用该录制完成
原速回放。需要账户时，注册及环境启用时间以 SimNow 官方说明为准；测试环境的
行情不能直接当作当时真实市场行情。
