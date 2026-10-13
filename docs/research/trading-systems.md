# yquant 交易系统参考资料

更新日期：2026-10-13。

本目录长期维护项目设计与开发所参考的系统及其资料，覆盖策略框架、回测与实盘引擎、交易柜台、券商接口、交易所接口和商业量化平台。

各条目记录系统定位、官方资料入口和使用边界。参考范围包含国内与海外市场、证券、期货和数字资产，随资料收集与核验持续更新。

## 开源系统与组件

| 系统或产品 | 定位 | 官方资料 |
| --- | --- | --- |
| WonderTrader | C++ 核心的量化研发与交易框架，提供 Python 应用层 wtpy | [核心源码与架构说明](https://github.com/wondertrader/wondertrader)、[官方文档](https://wtdocs.readthedocs.io/zh/latest/)、[wtpy 源码](https://github.com/wondertrader/wtpy) |
| NautilusTrader | Rust 原生、多资产与多交易场所的事件驱动引擎；Python 用于策略和编排 | [源码](https://github.com/nautechsystems/nautilus_trader)、[文档](https://nautilustrader.io/docs/latest/) |
| VeighNa / vn.py | Python 量化交易开发框架，包含交易接口和策略应用 | [核心源码](https://github.com/vnpy/vnpy)、[中文文档](https://www.vnpy.com/docs/cn/index.html) |
| QuantConnect LEAN | C# 核心的事件驱动算法交易引擎，支持 C# 与 Python 策略 | [源码](https://github.com/QuantConnect/Lean)、[引擎文档](https://www.quantconnect.com/docs/v2/lean-engine)、[类参考](https://www.quantconnect.com/docs/v2/lean-engine/class-reference) |
| Hummingbot | 面向数字资产的交易框架，提供 CLOB 与 AMM 连接器 | [源码](https://github.com/hummingbot/hummingbot)、[官方文档](https://hummingbot.org/docs/) |
| QuickFIX/C++ | C++ FIX 协议引擎组件 | [源码](https://github.com/quickfix/quickfix)、[C++ 文档](https://quickfixengine.org/c/documentation/) |

开源项目中的核心框架、扩展插件、数据与交易接入依赖可能使用不同许可。确定复用代码时，应以所选版本和文件的 LICENSE 为准。

## 源码公开但使用受限的系统

| 系统或产品 | 定位 | 官方资料 |
| --- | --- | --- |
| RQAlpha | Python 算法交易与回测框架；仓库声明仅限非商业使用，商业使用需另行取得授权 | [源码与项目说明](https://github.com/ricequant/rqalpha)、[文档](https://rqalpha.readthedocs.io/) |

## 闭源系统

收录交易柜台、券商、交易所和量化平台的公开文档、API、SDK 与使用说明。

### 交易柜台

| 系统或产品 | 定位 | 官方资料 |
| --- | --- | --- |
| 上期技术 CTP | 国内期货交易柜台及客户端 API | [SimNow API 下载](https://www.simnow.com.cn/static/apiDownload.action)（2026-10-13：直接访问返回 403） |

### 券商交易接口

| 系统或产品 | 定位 | 官方资料 |
| --- | --- | --- |
| Interactive Brokers / IBKR | 多资产券商交易系统 | [官方 API 入口](https://www.interactivebrokers.com/campus/ibkr-api-page/) |
| Alpaca | 券商交易 API | [订单文档](https://docs.alpaca.markets/us/docs/orders-at-alpaca)、[开发者文档](https://docs.alpaca.markets/) |
| 富途 Futu OpenAPI | 券商行情与交易接口，使用 OpenD 网关 | [OpenAPI 介绍](https://openapi.futunn.com/futu-api-doc/intro/intro.html?lang=zh-cn)、[交易接口总览](https://openapi.futunn.com/futu-api-doc/trade/overview.html)（2026-10-13：读取超时）、[Python SDK 源码](https://github.com/FutunnOpen/py-futu-api) |

### 交易所接口

| 系统或产品 | 定位 | 官方资料 |
| --- | --- | --- |
| Binance Spot | 数字资产交易所公开 API | [官方 API 文档仓库](https://github.com/binance/binance-spot-api-docs)、[WebSocket 行情](https://developers.binance.com/docs/binance-spot-api-docs/web-socket-streams)、[WebSocket 交易 API](https://github.com/binance/binance-spot-api-docs/blob/master/web-socket-api.md) |

### 量化平台与终端

| 系统或产品 | 定位 | 官方资料 |
| --- | --- | --- |
| 迅投 QMT / XtQuant | QMT 为证券量化终端；XtQuant 为配套的 Python 行情与交易接口 | [交易 API](https://dict.thinktrader.net/nativeApi/xttrader.html)（2026-10-13：读取超时）、[SDK 版本与下载](https://dict.thinktrader.net/nativeApi/download_xtquant.html) |
| 米筐 RQAlpha Plus | 商业量化回测框架 | [公开文档入口](https://www.ricequant.com/doc/)、[快速上手与策略入口](https://www.ricequant.com/doc/rqalpha-plus/doc/quick-start) |
| 聚宽 JoinQuant | 量化研究、回测与模拟交易平台；JQData 为数据服务。官方 FAQ 检索内容说明不提供实盘及交易通道服务（2026-10-13） | [策略 API](https://www.joinquant.com/help/api/help?name=api)、[常见问题](https://www.joinquant.com/help/api/help?name=faq)（2026-10-13：主站返回地区限制）、[历史 API PDF](https://cdn.joinquant.com/help/img/JoinQuantAPI.pdf) |
| 掘金量化 MyQuant | 量化策略开发平台及 SDK；股票实盘支持与券商提供版本有关 | [Python 数据事件文档](https://myquant.cn/docs/python/python_data_event)（2026-10-13：直接访问返回 403）、[终端使用说明](https://myquant.cn/docs2/operatingInstruction/)、[实盘交易说明](https://www.myquant.cn/docs2/operatingInstruction/trading/%E5%AE%9E%E7%9B%98%E4%BA%A4%E6%98%93.html) |
| BigQuant / BigTrader | BigQuant 为量化研究平台；BigTrader 为其交易引擎，此处收录回测文档 | [BigTrader 回测引擎说明](https://fund.bigquant.com/wiki/doc/3gG2rg4jBd)（2026-10-13：读取超时）、[AIStudio 说明](https://fudan.bigquant.com/wiki/doc/NzAjgKapzW)、[历史文档与新版入口](https://bigquant.com/wiki/doc/Cia42tsp7O) |

## 待核实对象

| 系统或产品 | 定位 | 官方资料 |
| --- | --- | --- |
| 功夫量化 | 历史量化交易项目；交易版本的源码入口与对应文档待核实 | [历史文档](https://docs.libkungfu.cc/v2.7/Kungfu-Doc-v2.7.pdf)；2026-10-13：原 kungfu-trader/kungfu 地址返回 404，kungfu-origin/kungfu 重定向后的默认页面介绍 Agent 工作协作产品 |

## 维护方式

新增条目时，先检查已有分类和包含关系，再记录系统或产品的定位与官方资料入口。许可限制、历史版本和影响访问的状态写在对应条目旁；访问状态注明检查日期。已有条目更新时，调整对应内容并更新文档日期。
