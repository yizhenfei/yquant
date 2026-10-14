# yquant 交易系统参考资料

更新日期：2026-10-14。

记录 yquant 设计与开发参考的交易系统及其官方资源。

GitHub 候选清单与筛选标准见 [100 个 GitHub 参考系统](github-trading-systems-100.md)。

## 开源系统与组件

### 交易框架

| 系统 | 选取原因 | 官方资源 |
| --- | --- | --- |
| WonderTrader | C++ 量化框架，与 yquant 的技术路线接近 | [核心源码与架构说明](https://github.com/wondertrader/wondertrader)、[官方文档](https://wtdocs.readthedocs.io/zh/latest/)、[wtpy 源码](https://github.com/wondertrader/wtpy) |
| Freqtrade | 社区关注度高，可了解 Python 策略接口与自动交易流程 | [源码与项目说明](https://github.com/freqtrade/freqtrade) |
| VeighNa / vn.py | 社区关注度高，可了解 Python 策略接口与国内交易接入方式 | [核心源码](https://github.com/vnpy/vnpy)、[中文文档](https://www.vnpy.com/docs/cn/index.html) |
| Backtrader | 社区关注度高的事件驱动框架，可对照策略与回测接口 | [源码与项目说明](https://github.com/mementum/backtrader) |
| NautilusTrader | Rust 交易引擎，可对照系统编程语言实现的交易框架 | [源码](https://github.com/nautechsystems/nautilus_trader)、[文档](https://nautilustrader.io/docs/latest/) |
| QuantConnect LEAN | 多资产回测与实盘框架，可对照不同语言的实现 | [源码](https://github.com/QuantConnect/Lean)、[引擎文档](https://www.quantconnect.com/docs/v2/lean-engine)、[类参考](https://www.quantconnect.com/docs/v2/lean-engine/class-reference) |
| Hummingbot | 数字资产做市框架，可了解做市与套利交易场景 | [源码](https://github.com/hummingbot/hummingbot)、[官方文档](https://hummingbot.org/docs/) |

### 交易协议组件

| 组件 | 选取原因 | 官方资源 |
| --- | --- | --- |
| QuickFIX/C++ | C++ FIX 协议引擎，可了解标准交易协议接入 | [源码](https://github.com/quickfix/quickfix)、[C++ 文档](https://quickfixengine.org/c/documentation/) |

## 源码公开但使用受限的系统

| 系统或产品 | 选取原因 | 官方资源 |
| --- | --- | --- |
| RQAlpha | Python 回测框架，可对照策略接口；采用非商业许可 | [源码与项目说明](https://github.com/ricequant/rqalpha)、[文档](https://rqalpha.readthedocs.io/) |

## 闭源系统

### 交易柜台

| 系统或产品 | 选取原因 | 官方资源 |
| --- | --- | --- |
| 上期技术 CTP | 国内期货柜台，可了解期货行情与交易接口 | [SimNow API 下载](https://www.simnow.com.cn/static/apiDownload.action) |

### 券商交易接口

| 系统或产品 | 选取原因 | 官方资源 |
| --- | --- | --- |
| Interactive Brokers / IBKR | 多资产券商，可了解海外市场的交易 API | [官方 API 入口](https://www.interactivebrokers.com/campus/ibkr-api-page/) |
| Alpaca | API 驱动的券商，可了解程序化交易接口 | [订单文档](https://docs.alpaca.markets/us/docs/orders-at-alpaca)、[开发者文档](https://docs.alpaca.markets/) |
| 富途 Futu OpenAPI | 可了解券商网关与客户端 API 的配合方式 | [OpenAPI 介绍](https://openapi.futunn.com/futu-api-doc/intro/intro.html?lang=zh-cn)、[交易接口总览](https://openapi.futunn.com/futu-api-doc/trade/overview.html)、[Python SDK 源码](https://github.com/FutunnOpen/py-futu-api) |

### 交易所接口

| 系统或产品 | 选取原因 | 官方资源 |
| --- | --- | --- |
| Binance Spot | 数字资产交易所，可了解 REST 与 WebSocket 交易接口 | [官方 API 文档仓库](https://github.com/binance/binance-spot-api-docs)、[WebSocket 行情](https://developers.binance.com/docs/binance-spot-api-docs/web-socket-streams)、[WebSocket 交易 API](https://github.com/binance/binance-spot-api-docs/blob/master/web-socket-api.md) |

### 量化平台与终端

| 系统或产品 | 选取原因 | 官方资源 |
| --- | --- | --- |
| 迅投 QMT / XtQuant | 国内证券量化终端，可了解终端与策略 SDK 的配合方式 | [交易 API](https://dict.thinktrader.net/nativeApi/xttrader.html)、[SDK 版本与下载](https://dict.thinktrader.net/nativeApi/download_xtquant.html) |
| 米筐 RQAlpha Plus | 商业回测框架，可与 RQAlpha 的公开源码对照 | [公开文档入口](https://www.ricequant.com/doc/)、[快速上手与策略入口](https://www.ricequant.com/doc/rqalpha-plus/doc/quick-start) |
| 聚宽 JoinQuant | 量化研究平台，可了解面向策略用户的研究与回测接口 | [策略 API](https://www.joinquant.com/help/api/help?name=api)、[常见问题](https://www.joinquant.com/help/api/help?name=faq)、[历史 API PDF](https://cdn.joinquant.com/help/img/JoinQuantAPI.pdf) |
| 掘金量化 MyQuant | 量化平台与 SDK，可了解策略开发和实盘接入方式 | [Python 数据事件文档](https://myquant.cn/docs/python/python_data_event)、[终端使用说明](https://myquant.cn/docs2/operatingInstruction/)、[实盘交易说明](https://www.myquant.cn/docs2/operatingInstruction/trading/%E5%AE%9E%E7%9B%98%E4%BA%A4%E6%98%93.html) |
| BigQuant / BigTrader | 量化研究平台，可对照研究与回测的产品接口 | [BigTrader 回测引擎说明](https://fund.bigquant.com/wiki/doc/3gG2rg4jBd)、[AIStudio 说明](https://fudan.bigquant.com/wiki/doc/NzAjgKapzW)、[历史文档与新版入口](https://bigquant.com/wiki/doc/Cia42tsp7O) |

## 源码状态待核实的系统

| 系统或产品 | 选取原因 | 官方资源 |
| --- | --- | --- |
| 功夫量化 | C++ 交易框架的历史资料；交易版本源码入口待核实 | [历史文档](https://docs.libkungfu.cc/v2.7/Kungfu-Doc-v2.7.pdf) |
