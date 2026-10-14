# 100 个 GitHub 交易与回测参考系统

快照日期：2026-10-14（Asia/Shanghai）。筛选标准：[项目数字初筛标准](#github-项目数字初筛标准)。

本轮采集 **153 个候选**，其中 **146 个**达到 Stars ≥ 100、Forks ≥ 20、创建日期 ≤ 2025-10-14 的数字门槛；范围检查与去重后有 **136 个**，再按 Stars、Forks 降序取 **100 个**。这 100 个候选按覆盖定位分组，**每组按 Stars 降序排列**，Stars 相同时按 Forks 降序。

定位围绕**研究、回测、实盘**三部分的主要使用流程；交易基础设施与市场仿真单列。主清单每个项目仅列一次，语言保留为辅助信息。各项保留采集日期、数字指标、能力说明和官方仓库链接。

## 定位概览

| 定位 | 数量 | 主要参考内容 |
| --- | ---: | --- |
| 研究＋回测＋实盘 | 30 | 研究实验、历史验证和真实策略执行形成主要使用流程。 |
| 研究＋回测 | 28 | 因子、模型、策略或参数实验与历史验证是主要覆盖。 |
| 回测＋实盘 | 17 | 侧重历史事件模拟与真实执行之间的策略复用。 |
| 研究＋实盘 | 1 | 提供研究分析及真实执行，历史回测边界另行标注。 |
| 实盘 | 12 | 侧重实时策略、订单、账户和运行管理。 |
| 研究 | 1 | 侧重数据与研究分析工作台，回测和真实策略执行另行标注。 |
| 回测与市场仿真 | 1 | 侧重历史模拟或合成市场、撮合和多智能体实验。 |
| 交易基础设施 | 10 | 交易所后台、P2P协议、撮合与账户服务等单独归类。 |
| 合计 | 100 | |

## GitHub 项目数字初筛标准

目标是快速缩小设计参考候选范围，先采集可批量获取的数字，再对少量候选检查源码、测试和架构。数字初筛结果表示值得进一步阅读，不代表已经完成工程质量评审。

### 范围与计数

- 接受其他语言的完整系统，按研究／回测／实盘主要覆盖定位分组，每组按 Stars 降序排列；语言作为辅助字段，混合系统注明核心与策略层语言。
- 包含量化交易平台、完整策略回测框架、自动交易机器人、市场仿真系统及可独立运行的交易所后台；各类用途明确区分。
- 纯 SDK、协议库、指标库、数据采集器、资料列表、课程片段和普通 Fork 不计入系统数量。独立维护的后继系统可以收录，但须说明与原系统的关系。
- 源码可见但许可受限或许可未声明的项目单独标注，不与通常开源许可混称。

### 门槛与排序

初始门槛为 **Stars ≥ 100、Forks ≥ 20、GitHub 仓库公开历史 ≥ 1 年**。历史长度先用仓库 `created_at` 近似；该日期不证明代码当时已经公开，私有转公开或迁移项目需另核实。2026-10-14 快照的创建日期门槛为 2025-10-14 或更早。

| 优先级 | 数字指标 | 用途与处理 |
| --- | --- | --- |
| 1 | Stars | 达到门槛后作为主要排序依据，保留精确数值 |
| 2 | Forks | 作为关注与修改意愿的辅助信号，Stars 相同时优先 Forks 较多者 |
| 3 | 仓库年龄 | 观察积累时间；记录创建日期及其近似含义 |
| 4 | 贡献者数量 | 观察多人参与情况；优先排除 GitHub 明确标记的 Bot；无法准确取得时标为未知 |
| 5 | 最近推送时间 | 使用 `pushed_at` 作为低成本维护信号；包含分支或自动化活动，不等同最近核心代码变更 |
| 6 | 提交数量、Release 数量 | 观察历史积累和发布习惯；提交数注明默认分支口径，Release 数不含只有 Tag 的版本 |
| 7 | Issue / PR 响应时间 | 放在第二轮采样；总数量不直接解释为维护质量 |

长期未更新和已归档作为降级标记保留。对同领域、同类型系统优先比较；候选数量不足时先扩大检索范围，若仍需调整门槛则明确记录调整及例外，避免静默放宽。第一次名单以门槛与 Stars 排序为准，其他指标用于复筛。

### 覆盖定位

数字初筛后的主视图按系统覆盖定位组织，各定位下按 Stars 降序、Forks 降序排列。语言与技术栈作为辅助信息保留。

| 部分 | 主要考察内容 |
| --- | --- |
| 研究 | 数据处理、因子与特征、模型训练、策略分析、参数实验 |
| 回测 | 历史回放、成交与账户模拟、成本假设及绩效评估 |
| 实盘 | 真实行情、策略订单、账户、风控及运行管理 |

常见定位包括研究＋回测＋实盘、研究＋回测、回测＋实盘和实盘；按实际覆盖补充研究、研究＋实盘。市场仿真与交易所/P2P等基础设施单列，每个系统只计一次。

三部分分别记录原生支持、部分支持、外部扩展、不支持、待核实；基础设施场景可标不适用。定位依据主要使用流程判断，辅助指标或报表不自动构成完整研究工作流。官方公开配套组件可计入系统覆盖，另库能力保留外接标记；通用接口、自行实现和私有商业功能单独说明。每项保存官方判断来源和能力边界。

### 记录与复核

每个项目记录仓库链接、覆盖定位、研究／回测／实盘支持程度、判断来源、用途、语言、Stars、Forks、创建日期、最近推送日期、归档状态、贡献者数、默认分支提交数、Release 数、许可和采集时间。能力说明使用简短完整的语句，例如“内置研究、回测和实盘功能”“内置研究和回测功能，实盘需外接”。重定向按规范仓库名去重。

采集过程数据放在临时目录，过程 JSON 不纳入项目。项目中保留最终文档、采集日期、数据口径和官方资料链接。

复筛再检查核心源码完整性、最小示例构建、关键状态测试和架构边界；记录“可借鉴模块、支持证据、已知缺口、未验证事项”。

指标来源：[GitHub 仓库 REST API](https://docs.github.com/en/rest/repos/repos#get-a-repository)、[贡献者 API](https://docs.github.com/en/rest/repos/repos#list-repository-contributors)、[GraphQL Repository](https://docs.github.com/en/graphql/reference/objects#repository)。

### 分类与指标口径

- **研究**：数据、因子或模型、策略分析、参数实验等研究工作流。**回测**：历史数据驱动的策略、成交与账户模拟。**实盘**：真实行情、策略订单、账户与运行管理。
- 组名体现主要覆盖流程；辅助行情分析和回测结果检查可记为部分研究。官方公开配套组件可以计入覆盖，并保留外接标记；只有接口、需要自行实现或依赖私有商业版本的能力单独注明。
- 能力说明使用完整语句，分别说明框架内置、部分支持、需要外接和待核实的环节。外接能力是否计入组名，取决于是否有已经公开的官方配套实现。
- 交易所后台和P2P协议等归入交易基础设施，说明撮合、账户、钱包或交易协议等参考内容；市场仿真注明合成市场与历史回测的边界。
- **组内排名**从 1 开始；**Stars / Forks**为原始 API 精确值。**提交 / Release**为默认分支可达提交数 / GitHub Release 数，只有 Tag 的版本不计为 Release。
- **贡献账号**为 REST API 可识别且 type 不为 Bot 的账号，不含匿名作者；GitHub 仅关联前 500 个作者邮箱，该数值不等于全部实际作者人数。
- **创建 / 最近推送**为 UTC 日期。创建日近似仓库历史，推送包含任意分支和自动化活动；已归档和推送逾一年的项目保留标记。
- **API 未识别**表示自动采集未确定许可，需要查看项目的许可文件或说明。定位依据为各项官方仓库与[补充官方资料](#补充官方资料)，能力边界写在对应条目中。采集过程数据使用临时目录，项目仅保留最终文档。

## 研究＋回测＋实盘（30）

研究实验、历史验证和真实策略执行形成主要使用流程。

| 组内排名 | 系统与覆盖说明（语言） | Stars / Forks | 贡献账号 | 提交 / Release | 创建 / 最近推送 | 状态与许可 |
| ---: | --- | ---: | ---: | ---: | --- | --- |
| 1 | [freqtrade/freqtrade](https://github.com/freqtrade/freqtrade)（Python）：内置研究、回测和实盘功能。内置历史数据、Hyperopt/FreqAI、策略回测与真实交易；模型训练及参数实验属于研究工作流。 | 55,362 / 11,438 | 339 | 33,178 / 118 | 2017-05-17 / 2026-10-13 | GPL-3.0 |
| 2 | [vnpy/vnpy](https://github.com/vnpy/vnpy)（Python）：内置研究和回测功能，实盘需外接。alpha因子模型与回测位于主仓库，CTA实盘和柜台通过官方公开app/gateway插件组合。 | 45,854 / 12,516 | 133 | 6,949 / 68 | 2015-03-02 / 2026-10-06 | MIT |
| 3 | [nautechsystems/nautilus_trader](https://github.com/nautechsystems/nautilus_trader)（Rust）：内置回测和实盘功能，提供部分研究功能。公开README明确统一research、确定性simulation和live；研究偏数据处理、Jupyter及交易结果分析，非完整因子平台。 | 29,955 / 4,003 | 209 | 21,778 / 142 | 2018-06-25 / 2026-10-14 | LGPL-3.0 |
| 4 | [mementum/backtrader](https://github.com/mementum/backtrader)（Python）：内置研究、回测和实盘功能。公开框架内置参数优化、分析器、历史模拟及IB/OANDA等历史实盘接入；接入可用性另核。 | 23,471 / 5,295 | 48 | 2,404 / 0 | 2015-01-10 / 2024-08-19 | 推送逾一年；GPL-3.0 |
| 5 | [QuantConnect/Lean](https://github.com/QuantConnect/Lean)（C#）：内置研究、回测和实盘功能。公开README提供本地research、optimize、backtest、live工作流；券商账户、数据及相应连接组件仍需配置，商业云产品能力不计入。 | 21,987 / 5,300 | 216 | 13,393 / 8 | 2014-11-28 / 2026-10-13 | Apache-2.0 |
| 6 | [AI4Finance-Foundation/FinRL](https://github.com/AI4Finance-Foundation/FinRL)（Python）：内置研究和回测功能，提供部分实盘功能。原生经典train-test-trade研究和仿真；当前README明确旧框架有basic Alpaca support，实盘仅部分支持，完整生产执行转FinRL-X。 | 16,638 / 3,544 | 126 | 3,284 / 1 | 2020-07-26 / 2026-10-13 | MIT |
| 7 | [Drakkar-Software/OctoBot](https://github.com/Drakkar-Software/OctoBot)（Python）：内置回测和实盘功能，提供部分研究功能。手动OctoBot模式内置回测引擎和策略测试优化，支持模拟与真实交易；研究偏策略配置实验，AI商业服务不计入。 | 6,781 / 1,292 | 18 | 6,046 / 125 | 2018-02-23 / 2026-10-13 | GPL-3.0 |
| 8 | [wondertrader/wondertrader](https://github.com/wondertrader/wondertrader)（C++）：内置回测和实盘功能，研究需外接。C++核心提供回测和执行，研究与Python参数实验通过官方公开wtpy配套；研究栏保留外接标记。 | 6,389 / 1,193 | 17 | 1,828 / 0 | 2020-03-30 / 2026-09-01 | MIT |
| 9 | [Superalgos/Superalgos](https://github.com/Superalgos/Superalgos)（JavaScript）：内置研究、回测和实盘功能。原生可视化数据挖掘、策略设计实验、回测/模拟及自动交易部署。 | 5,695 / 6,001 | 184 | 23,353 / 38 | 2019-08-12 / 2026-10-13 | Apache-2.0 |
| 10 | [gbeced/pyalgotrade](https://github.com/gbeced/pyalgotrade)（Python）：内置研究和回测功能，提供部分实盘功能。包含参数优化、分析器及回测；README列出历史Bitstamp实盘接入，现代市场适配和维护有限，标部分支持。 | 4,675 / 1,385 | 8 | 1,158 / 0 | 2012-03-07 / 2023-11-13 | 已归档；推送逾一年；API 未识别 |
| 11 | [AI4Finance-Foundation/FinRL-Trading](https://github.com/AI4Finance-Foundation/FinRL-Trading)（Python）：内置研究、回测和实盘功能。FinRL-X公开实现数据管线、ML选股/组合配置、bt回测和Alpaca多账户风险校验执行。 | 3,814 / 1,115 | 15 | 322 / 1 | 2020-07-26 / 2026-09-18 | Apache-2.0 |
| 12 | [pst-group/pysystemtrade](https://github.com/pst-group/pysystemtrade)（Python）：内置研究、回测和实盘功能。公开系统化期货研究/优化、组合风险、回测及IB自动实盘工作流。 | 3,552 / 1,085 | 57 | 4,854 / 5 | 2015-11-27 / 2026-10-08 | GPL-3.0 |
| 13 | [scrtlabs/catalyst](https://github.com/scrtlabs/catalyst)（Python）：内置研究、回测和实盘功能。历史数字资产研究、回测和交易所实盘框架；原有交易所接口的当前可用性未验证。 | 2,559 / 705 | 109 | 6,364 / 0 | 2017-06-13 / 2022-11-26 | 已归档；推送逾一年；Apache-2.0 |
| 14 | [blankly-finance/blankly](https://github.com/blankly-finance/blankly)（Python）：内置回测和实盘功能，提供部分研究功能。可使用价格、自定义事件和另类数据进行策略实验；研究工具较轻，代码可切换回测、模拟及实盘。 | 2,479 / 313 | 17 | 1,573 / 28 | 2021-03-09 / 2024-12-30 | 推送逾一年；LGPL-3.0 |
| 15 | [ranaroussi/qtpylib](https://github.com/ranaroussi/qtpylib)（Python）：内置回测和实盘功能，提供部分研究功能。MySQL保存Tick/Bar/成交供分析回测，并提供IB策略运行和交易监控；研究侧主要是数据与分析支持。 | 2,267 / 516 | 6 | 853 / 0 | 2016-08-18 / 2021-09-22 | 已归档；推送逾一年；Apache-2.0 |
| 16 | [coding-kitties/investing-algorithm-framework](https://github.com/coding-kitties/investing-algorithm-framework)（Python）：内置研究、回测和实盘功能。策略变体扫描、Studies/Universes、walk-forward/OOS实验、双回测引擎与模拟/实盘共用策略。 | 2,174 / 311 | 10 | 1,926 / 211 | 2019-12-18 / 2026-09-30 | Apache-2.0 |
| 17 | [enarjord/passivbot](https://github.com/enarjord/passivbot)（Rust）：内置研究、回测和实盘功能。原生历史数据缓存、进化参数优化、回测及多交易所执行；研究重点是做市配置实验。 | 2,130 / 679 | 54 | 12,142 / 116 | 2020-12-11 / 2026-10-14 | Unlicense |
| 18 | [Lumiwealth/lumibot](https://github.com/Lumiwealth/lumibot)（Python）：内置研究、回测和实盘功能。公开框架含研究agent、历史数据与回测产物、多券商策略执行；BotSpot托管服务另计。 | 2,119 / 414 | 37 | 6,025 / 115 | 2020-09-10 / 2026-10-14 | GPL-3.0 |
| 19 | [austin-starks/NextTrade](https://github.com/austin-starks/NextTrade)（TypeScript）：内置研究、回测和实盘功能。可视化策略、遗传参数优化、训练验证区间、回测和Tradier部署；NexusTrade云产品另计。 | 1,774 / 285 | 2 | 38 / 0 | 2022-09-17 / 2025-05-15 | 推送逾一年；MIT |
| 20 | [c9s/bbgo](https://github.com/c9s/bbgo)（Go）：内置研究、回测和实盘功能。内置历史数据同步、参数优化、K线回测、跨账户交易执行。 | 1,672 / 384 | 82 | 12,353 / 123 | 2020-10-05 / 2026-10-14 | AGPL-3.0 |
| 21 | [kieran-mackle/AutoTrader](https://github.com/kieran-mackle/AutoTrader)（Python）：内置研究、回测和实盘功能。策略开发优化、数据与交互分析、虚拟券商回测及多个场所的真实策略执行。 | 1,273 / 299 | 5 | 2,262 / 0 | 2021-07-11 / 2025-05-04 | 已归档；推送逾一年；GPL-3.0 |
| 22 | [AlexWan/OsEngine](https://github.com/AlexWan/OsEngine)（C#）：内置研究、回测和实盘功能。历史数据、机器人、walk-forward参数优化、回测与多柜台交易均是公开平台模块。 | 1,035 / 444 | 57 | 7,425 / 2 | 2017-02-28 / 2026-10-13 | API 未识别 |
| 23 | [ZENALC/algobot](https://github.com/ZENALC/algobot)（Python）：内置研究、回测和实盘功能。GUI策略编辑、内置优化器、自定义回测、模拟和live bot。 | 948 / 175 | 4 | 1,323 / 11 | 2020-07-27 / 2023-02-11 | 推送逾一年；GPL-3.0 |
| 24 | [amor71/LiuAlgoTrader](https://github.com/amor71/LiuAlgoTrader)（Python）：内置研究、回测和实盘功能。ML训练、超参优化、Notebook/Streamlit研究分析、回测与多进程真实交易。 | 913 / 149 | 10 | 3,749 / 36 | 2019-12-02 / 2023-08-13 | 推送逾一年；MIT |
| 25 | [Ekliptor/WolfBot](https://github.com/Ekliptor/WolfBot)（TypeScript）：内置研究、回测和实盘功能。策略组合、参数优化、历史回测与真实执行公开；部分社交Web插件另计。 | 788 / 225 | 7 | 542 / 0 | 2018-04-23 / 2023-02-17 | 推送逾一年；AGPL-3.0 |
| 26 | [banbox/banbot](https://github.com/banbox/banbot)（Go）：内置研究、回测和实盘功能。数据与因子研究、参数实验、回测和交易；新因子引擎实盘的传输及对账条件另核。 | 682 / 132 | 8 | 947 / 44 | 2023-12-07 / 2026-10-11 | AGPL-3.0 |
| 27 | [uniVocity/univocity-trader](https://github.com/uniVocity/univocity-trader)（Java）：内置研究、回测和实盘功能。公开参数组合实验、历史模拟和交易所执行；付费并行回测加速组件单独计。 | 600 / 145 | 7 | 565 / 0 | 2019-11-15 / 2022-06-21 | 推送逾一年；API 未识别 |
| 28 | [saeed349/Microservices-Based-Algorithmic-Trading-System](https://github.com/saeed349/Microservices-Based-Algorithmic-Trading-System)（Python）：内置研究、回测和实盘功能。集成MLflow研究、Backtrader回测、Airflow实盘调度和运行监控。 | 542 / 151 | 1 | 24 / 0 | 2020-01-06 / 2024-04-08 | 推送逾一年；BSD-3-Clause |
| 29 | [neurallayer/roboquant](https://github.com/neurallayer/roboquant)（Kotlin）：内置回测和实盘功能，提供部分研究功能。Jupyter数据与策略实验、指标数据集、历史回测及IB/Alpaca执行。 | 467 / 73 | 12 | 1,626 / 23 | 2021-09-15 / 2026-09-18 | Apache-2.0 |
| 30 | [AmpyFin/ampyfin](https://github.com/AmpyFin/ampyfin)（Python）：内置研究、回测和实盘功能。集成策略训练、历史验证和晋级实盘的工作流。 | 427 / 137 | 9 | 331 / 11 | 2024-09-25 / 2026-08-06 | MIT |

## 研究＋回测（28）

因子、模型、策略或参数实验与历史验证是主要覆盖。

| 组内排名 | 系统与覆盖说明（语言） | Stars / Forks | 贡献账号 | 提交 / Release | 创建 / 最近推送 | 状态与许可 |
| ---: | --- | ---: | ---: | ---: | --- | --- |
| 1 | [microsoft/qlib](https://github.com/microsoft/qlib)（Python）：内置研究和回测功能，实盘需外接。原生覆盖数据因子、模型训练、实验及模拟执行；online prediction/在线学习不等于券商真实订单，实盘需另接执行层。 | 49,349 / 7,789 | 140 | 2,070 / 24 | 2020-08-14 / 2026-10-08 | MIT |
| 2 | [bbfamily/abu](https://github.com/bbfamily/abu)（Python）：内置研究和回测功能，实盘能力待核实。公开abupy覆盖选股择时、机器学习、参数/组合分析及回测；README的商业AI系统和实盘效果描述不足以确认公开仓库真实券商执行。 | 18,920 / 4,745 | 2 | 73 / 1 | 2016-09-19 / 2026-01-24 | GPL-3.0 |
| 3 | [yutiansut/QUANTAXIS](https://github.com/yutiansut/QUANTAXIS)（Python）：内置研究和回测功能，实盘需外接。本地数据、QAFactor及账户回测原生；当前实盘生态通过QIFI/QATRADER等配套服务和柜台组件，按外部扩展标注。 | 11,279 / 3,478 | 61 | 7,517 / 62 | 2016-03-29 / 2026-09-18 | MIT |
| 4 | [kernc/backtesting.py](https://github.com/kernc/backtesting.py)（Python）：内置研究和回测功能，实盘需外接。内置参数优化、热图与回测分析；真实券商执行需用户另接，主仓库不提供实盘运行系统。 | 9,045 / 1,545 | 45 | 461 / 0 | 2019-01-02 / 2026-08-05 | AGPL-3.0 |
| 5 | [jesse-ai/jesse](https://github.com/jesse-ai/jesse)（Python）：内置研究和回测功能，实盘需外接。主包原生研究API、优化、Monte Carlo和回测；live/paper通过预编译jesse-live官方插件，需要有效license。 | 8,643 / 1,256 | 50 | 3,537 / 0 | 2018-11-09 / 2026-10-12 | MIT |
| 6 | [tensortrade-org/tensortrade](https://github.com/tensortrade-org/tensortrade)（Python）：内置研究和回测功能，实盘需外接。原生RL环境、训练、超参优化和walk-forward评价；当前README重点是模拟交易环境，真实券商执行需另接。 | 7,197 / 1,335 | 44 | 1,093 / 7 | 2019-07-30 / 2026-02-19 | Apache-2.0 |
| 7 | [zvtvz/zvt](https://github.com/zvtvz/zvt)（Python）：内置研究和回测功能，实盘需外接。原生数据、因子、选股与回测；README明确不适合实时行情/交互，真实策略执行需外接。 | 4,313 / 1,017 | 58 | 887 / 43 | 2019-04-04 / 2026-07-01 | MIT |
| 8 | [cuemacro/finmarketpy](https://github.com/cuemacro/finmarketpy)（Python）：内置研究和回测功能，实盘需外接。原生市场研究、事件/季节分析、波动率目标及策略回测；真实下单不在该包的核心公开工作流。 | 3,813 / 521 | 16 | 776 / 18 | 2015-02-19 / 2026-04-16 | Apache-2.0 |
| 9 | [edtechre/pybroker](https://github.com/edtechre/pybroker)（Python）：内置研究和回测功能，实盘需外接。原生机器学习模型、滚动训练、参数/策略实验及历史回测；真实券商执行需另接。 | 3,612 / 459 | 9 | 1,259 / 52 | 2023-01-16 / 2026-10-05 | API 未识别 |
| 10 | [fasiondog/hikyuu](https://github.com/fasiondog/hikyuu)（C++）：内置研究和回测功能，实盘需外接。原生指标/多因子、策略组合与回测；OrderBroker提供扩展接口，真实交易代理和柜台连接由用户/外部扩展落实。 | 3,561 / 835 | 23 | 6,508 / 75 | 2012-07-18 / 2026-10-13 | Apache-2.0 |
| 11 | [mhallsmoore/qstrader](https://github.com/mhallsmoore/qstrader)（Python）：内置回测功能，提供部分研究功能，实盘需外接。原生组合/再平衡策略研究样例和绩效分析、完整历史模拟；通用因子建模有限，真实券商执行需外接。 | 3,472 / 927 | 14 | 431 / 9 | 2015-12-04 / 2024-06-30 | 推送逾一年；MIT |
| 12 | [TradeMaster-NTU/TradeMaster](https://github.com/TradeMaster-NTU/TradeMaster)（Python）：内置研究和回测功能，实盘能力待核实。数据预处理、Alpha158、RL训练和市场模拟评估齐全；README的deployment表述未证明真实券商执行。 | 3,093 / 563 | 12 | 1,179 / 1 | 2022-08-23 / 2025-06-04 | 推送逾一年；Apache-2.0 |
| 13 | [pmorissette/bt](https://github.com/pmorissette/bt)（Python）：内置研究和回测功能，实盘能力待核实。树状资产配置策略、历史组合实验与结果分析；未找到公开主仓库的券商执行证据。 | 2,997 / 500 | 39 | 888 / 17 | 2014-06-19 / 2026-10-13 | MIT |
| 14 | [ta4j/ta4j](https://github.com/ta4j/ta4j)（Java）：内置研究和回测功能，实盘需外接。原生参数研究、批量策略、walk-forward、成本回测；真实数据和券商连接由使用方适配。 | 2,510 / 802 | 83 | 4,043 / 21 | 2017-09-14 / 2026-10-12 | API 未识别 |
| 15 | [stefan-jansen/zipline-reloaded](https://github.com/stefan-jansen/zipline-reloaded)（Python）：内置研究和回测功能，实盘能力待核实。事件驱动研究与回测、因子Pipeline生态；README所述Quantopian历史live engine不等于本仓库当前券商执行支持。 | 1,963 / 334 | 117 | 6,694 / 11 | 2021-02-25 / 2026-10-13 | 独立维护分支；Apache-2.0 |
| 16 | [Yvictor/TradingGym](https://github.com/Yvictor/TradingGym)（Python）：内置研究和回测功能，不提供实盘功能。原生RL训练环境、Tick/OHLC数据和回测；README将实时IB环境列为未来实现。 | 1,923 / 372 | 3 | 47 / 0 | 2017-05-01 / 2024-02-11 | 推送逾一年；MIT |
| 17 | [QuantFans/quantdigger](https://github.com/QuantFans/quantdigger)（Python）：内置回测功能，提供部分研究功能，不提供实盘功能。支持股票期货、选股套利及组合回测、K线策略图；README明确交易接口属未来计划。 | 1,825 / 651 | 8 | 419 / 0 | 2014-11-29 / 2020-05-02 | 推送逾一年；API 未识别 |
| 18 | [ScottfreeLLC/AlphaPy](https://github.com/ScottfreeLLC/AlphaPy)（Python）：内置研究和回测功能，实盘能力待核实。ModelFlow/MarketFlow建模、特征和策略系统流水线；主仓库未确认真实券商执行路径。 | 1,765 / 281 | 5 | 447 / 11 | 2016-02-14 / 2026-10-05 | Apache-2.0 |
| 19 | [goldspanlabs/optopsy](https://github.com/goldspanlabs/optopsy)（Python）：内置研究和回测功能，实盘能力待核实。期权策略筛选统计、逐笔资本模拟、多策略组合回测；live data provider只证明取数。 | 1,495 / 228 | 3 | 486 / 18 | 2017-09-17 / 2026-06-30 | AGPL-3.0 |
| 20 | [foolcage/fooltrader](https://github.com/foolcage/fooltrader)（Python）：内置研究功能，提供部分回测功能，实盘能力待核实。数据采集清洗、分析展示和研究流程明确；README称回测处于非常初期，真实交易接口仍待核。 | 1,198 / 323 | 5 | 351 / 6 | 2017-09-27 / 2023-05-22 | 推送逾一年；MIT |
| 21 | [Kismuz/btgym](https://github.com/Kismuz/btgym)（Python）：内置研究和回测功能，实盘能力待核实。原生RL训练、训练验证划分、合成数据与Backtrader事件回测；未据README确认真实broker执行。 | 1,037 / 257 | 8 | 635 / 0 | 2017-05-24 / 2021-08-28 | 推送逾一年；LGPL-3.0 |
| 22 | [idanya/algo-trader](https://github.com/idanya/algo-trader)（Python）：内置回测功能，提供部分研究功能，实盘需外接。数据处理流水线和资产相关性Processor可供分析；README明确实时交易未完成，需自行实现broker API。 | 870 / 107 | 3 | 76 / 6 | 2021-09-14 / 2023-11-20 | 推送逾一年；MIT |
| 23 | [ematvey/pybacktest](https://github.com/ematvey/pybacktest)（Python）：内置研究和回测功能，实盘能力待核实。Pandas信号、单资产回测、绩效与Notebook可视化；策略代码可用于production未证明主仓库自带真实执行。 | 823 / 239 | 6 | 178 / 0 | 2012-10-26 / 2021-11-11 | 推送逾一年；MIT |
| 24 | [constverum/Quantdom](https://github.com/constverum/Quantdom)（Python）：内置回测功能，提供部分研究功能，实盘能力待核实。GUI策略参数实验、组合与回测分析；未核实真实券商执行。 | 772 / 188 | 1 | 16 / 3 | 2017-11-05 / 2022-07-06 | 推送逾一年；Apache-2.0 |
| 25 | [jrmeier/fast-trade](https://github.com/jrmeier/fast-trade)（Python）：内置回测功能，提供部分研究功能，实盘能力待核实。配置化策略实验、优化与组合回测；真实执行待核。 | 597 / 65 | 3 | 361 / 25 | 2019-08-06 / 2026-10-06 | AGPL-3.0 |
| 26 | [fbertram/TuringTrader](https://github.com/fbertram/TuringTrader)（C#）：内置研究和回测功能，实盘能力待核实。数据与组合研究、多线程参数优化和历史模拟；真实执行待核。 | 538 / 118 | 1 | 1,266 / 0 | 2019-07-07 / 2026-05-19 | AGPL-3.0 |
| 27 | [systematicinvestor/SIT](https://github.com/systematicinvestor/SIT)（R）：内置研究和回测功能，实盘能力待核实。策略与资产配置研究、组合优化、历史调仓与回测。 | 494 / 379 | 1 | 299 / 0 | 2011-10-03 / 2023-06-03 | 推送逾一年；API 未识别 |
| 28 | [abbass2/pyqstrat](https://github.com/abbass2/pyqstrat)（Python）：内置回测功能，提供部分研究功能，实盘能力待核实。指标、信号、规则、撮合假设和自定义指标分析组成历史策略实验；实盘待核。 | 371 / 59 | 4 | 781 / 0 | 2018-08-01 / 2023-11-05 | 推送逾一年；BSD-3-Clause |

## 回测＋实盘（17）

侧重历史事件模拟与真实执行之间的策略复用。

| 组内排名 | 系统与覆盖说明（语言） | Stars / Forks | 贡献账号 | 提交 / Release | 创建 / 最近推送 | 状态与许可 |
| ---: | --- | ---: | ---: | ---: | --- | --- |
| 1 | [hummingbot/hummingbot](https://github.com/hummingbot/hummingbot)（Python）：内置实盘功能，提供部分回测功能，研究需外接。客户端原生执行，V2控制器具有有限回测能力；完整可视化回测及配置实验通过独立Dashboard/API等生态仓库，不把整套研究界面算主仓库原生。 | 20,438 / 4,994 | 232 | 28,074 / 111 | 2019-04-02 / 2026-10-06 | Apache-2.0 |
| 2 | [askmike/gekko](https://github.com/askmike/gekko)（JavaScript）：内置回测和实盘功能，提供部分研究功能。原生历史数据导入、策略回测和交易；研究仅简单技术分析/结果检查，系统化参数寻优多依赖生态工具，主定位回测到交易。 | 10,176 / 3,774 | 180 | 2,613 / 16 | 2013-05-12 / 2020-02-16 | 已归档；推送逾一年；MIT |
| 3 | [DeviaVir/zenbot](https://github.com/DeviaVir/zenbot)（JavaScript）：内置回测和实盘功能，提供部分研究功能。原生历史模拟、纸面交易和真实交易；研究以技术策略参数和模拟比较为主，完整因子/模型实验工作流不足。 | 8,257 / 1,960 | 170 | 3,885 / 4 | 2016-06-27 / 2022-02-14 | 已归档；推送逾一年；MIT |
| 4 | [chrisleekr/binance-trading-bot](https://github.com/chrisleekr/binance-trading-bot)（TypeScript）：内置回测和实盘功能，提供部分研究功能。当前主仓库原生策略配置、历史回测、账户和自动执行；研究偏操作台调参/结果检查，未将其提升为完整研究平台。 | 5,580 / 1,165 | 14 | 431 / 53 | 2020-08-10 / 2026-10-13 | Apache-2.0 |
| 5 | [nkaz001/hftbacktest](https://github.com/nkaz001/hftbacktest)（Rust）：内置回测和实盘功能，提供部分研究功能。原生全订单簿回放、队列/延迟模型与Rust真实交易机器人；研究偏微观结构模型和仿真分析，Python与Rust能力不同。 | 4,883 / 931 | 22 | 1,038 / 41 | 2022-08-25 / 2025-12-23 | MIT |
| 6 | [thrasher-corp/gocryptotrader](https://github.com/thrasher-corp/gocryptotrader)（Go）：内置回测和实盘功能，提供部分研究功能。原生行情、组合订单与交易执行，独立backtester应用支持历史策略迭代；研究偏交易数据/策略结果分析。 | 3,459 / 920 | 115 | 2,626 / 0 | 2014-10-28 / 2026-10-13 | MIT |
| 7 | [barter-rs/barter-rs](https://github.com/barter-rs/barter-rs)（Rust）：内置回测和实盘功能，研究能力待核实。原生数据、策略、订单、风控、回测及live/mock执行；并行回测和汇总尚不足证明完整研究工作流。 | 2,341 / 383 | 10 | 510 / 16 | 2022-10-09 / 2026-08-24 | MIT |
| 8 | [ivopetiz/algotrading](https://github.com/ivopetiz/algotrading)（Python）：内置回测和实盘功能，研究能力待核实。README明确历史回测及实时真实资金/模拟模式；数据读取和进出场绘图未自动算独立研究工作流。 | 1,695 / 204 | 2 | 220 / 0 | 2018-07-10 / 2026-01-07 | MIT |
| 9 | [rodrigo-brito/ninjabot](https://github.com/rodrigo-brito/ninjabot)（Go）：内置回测和实盘功能，提供部分研究功能。原生CSV回测、绩效与bootstrap区间分析、模拟钱包和真实订单；研究侧为轻量统计分析。 | 1,629 / 199 | 28 | 337 / 48 | 2021-04-24 / 2026-09-01 | MIT |
| 10 | [mhallsmoore/qsforex](https://github.com/mhallsmoore/qsforex)（Python）：内置回测和实盘功能，提供部分研究功能。Tick级外汇回测、权益/收益/回撤分析和OANDA执行；研究工具主要是基础数据与绩效检查，接口较旧。 | 857 / 358 | 1 | 23 / 0 | 2015-01-27 / 2022-06-21 | 推送逾一年；API 未识别 |
| 11 | [AsyncAlgoTrading/aat](https://github.com/AsyncAlgoTrading/aat)（Python）：内置回测和实盘功能，提供部分研究功能。原生异步回测、风控和交易执行；研究侧提供交易统计、交易中/后分析扩展，未宣称完整因子实验平台。 | 841 / 151 | 6 | 667 / 0 | 2019-05-20 / 2026-07-27 | Apache-2.0 |
| 12 | [letianzj/quanttrader](https://github.com/letianzj/quanttrader)（Python）：内置回测和实盘功能，研究需外接。事件回测、IB执行及监控；系统化研究可接外部分析工具。 | 782 / 140 | 4 | 128 / 0 | 2020-07-27 / 2024-06-20 | 推送逾一年；Apache-2.0 |
| 13 | [rburkholder/trade-frame](https://github.com/rburkholder/trade-frame)（C++）：内置回测和实盘功能，提供部分研究功能。离线模拟、IB执行和期权组合分析；研究以指标及分析应用为辅。 | 680 / 195 | 5 | 6,035 / 0 | 2016-07-24 / 2026-10-13 | [许可文本待核](https://github.com/rburkholder/trade-frame/blob/master/LICENSE.txt) |
| 14 | [cassandre-tech/cassandre-trading-bot](https://github.com/cassandre-tech/cassandre-trading-bot)（Java）：内置回测和实盘功能，研究需外接。公开test模块提供历史K线回测和dry mode，主框架提供策略与订单执行。 | 661 / 189 | 8 | 2,142 / 37 | 2020-02-06 / 2025-03-01 | 推送逾一年；GPL-3.0 |
| 15 | [physercoe/starquant](https://github.com/physercoe/starquant)（C++）：内置回测和实盘功能，研究需外接。基础历史回测与实盘公开；更新过的回测代码在backtest分支，Pro部分闭源。 | 608 / 176 | 2 | 190 / 1 | 2019-03-12 / 2026-07-04 | Apache-2.0 |
| 16 | [josephchenhk/qtrader](https://github.com/josephchenhk/qtrader)（Python）：内置回测和实盘功能，研究需外接。回测、模拟与实盘复用策略；独立因子或模型研究工作流另接。 | 549 / 109 | 2 | 317 / 0 | 2020-09-16 / 2026-09-07 | API 未识别 |
| 17 | [timolson/cointrader](https://github.com/timolson/cointrader)（Java）：内置回测和实盘功能，提供部分研究功能。事件交易后台、回测模拟与实盘；研究以数据导出和事件分析为辅。 | 452 / 166 | 9 | 512 / 0 | 2014-06-01 / 2023-06-14 | 推送逾一年；API 未识别 |

## 研究＋实盘（1）

提供研究分析及真实执行，历史回测边界另行标注。

| 组内排名 | 系统与覆盖说明（语言） | Stars / Forks | 贡献账号 | 提交 / Release | 创建 / 最近推送 | 状态与许可 |
| ---: | --- | ---: | ---: | ---: | --- | --- |
| 1 | [marketcalls/openalgo](https://github.com/marketcalls/openalgo)（Python）：内置研究和实盘功能，回测能力待核实。内置期权分析、Greeks、波动率和情景工具及策略托管、券商执行；Analyzer Mode为实时模拟，历史回测待核。 | 2,849 / 1,322 | 85 | 5,567 / 67 | 2024-02-19 / 2026-10-13 | AGPL-3.0 |

## 实盘（12）

侧重实时策略、订单、账户和运行管理。

| 组内排名 | 系统与覆盖说明（语言） | Stars / Forks | 贡献账号 | 提交 / Release | 创建 / 最近推送 | 状态与许可 |
| ---: | --- | ---: | ---: | ---: | --- | --- |
| 1 | [ctubio/Krypto-trading-bot](https://github.com/ctubio/Krypto-trading-bot)（C++）：内置实盘功能，提供部分研究功能，回测能力待核实。主要是可运行做市策略、库存/报价及交易所执行；操作界面调参不自动算完整研究工作流，未见原生历史回测承诺。 | 3,719 / 825 | 51 | 5,432 / 6 | 2017-06-13 / 2024-12-15 | 推送逾一年；[ISC](https://github.com/ctubio/Krypto-trading-bot/blob/master/LICENSE) |
| 2 | [shidenggui/easyquant](https://github.com/shidenggui/easyquant)（Python）：内置实盘功能，提供部分研究功能，回测需外接。行情推送、事件策略、日志与easytrader整合执行；研究仅行情/指标辅助，历史回测需外接其他引擎。 | 3,680 / 1,256 | 9 | 162 / 0 | 2015-12-20 / 2025-03-27 | 推送逾一年；API 未识别 |
| 3 | [Haehnchen/crypto-trading-bot](https://github.com/Haehnchen/crypto-trading-bot)（JavaScript）：内置实盘功能，提供部分研究功能，回测能力待核实。公开版可配置策略、信号及交易所执行；README未明确提供完整历史回测工作流，实盘为已证实主能力。 | 3,538 / 1,019 | 14 | 1,035 / 0 | 2018-09-25 / 2026-08-02 | MIT |
| 4 | [whittlem/pycryptobot](https://github.com/whittlem/pycryptobot)（Python）：内置实盘功能，研究和回测能力待核实。公开README确认自动交易及live test，使用资料转到作者站点；本次未据当前README确认历史回测或研究流程。 | 2,056 / 716 | 68 | 2,155 / 4 | 2020-12-07 / 2026-03-26 | Apache-2.0 |
| 5 | [stellar-deprecated/kelp](https://github.com/stellar-deprecated/kelp)（Go）：内置实盘功能，研究和回测能力待核实。原生做市策略、价格源、交易执行、会计和GUI；--sim模拟模式未证明历史回测。 | 1,124 / 257 | 14 | 833 / 25 | 2018-08-08 / 2023-11-03 | 已归档；推送逾一年；API 未识别 |
| 6 | [bitrinjani/r2](https://github.com/bitrinjani/r2)（TypeScript）：内置实盘功能，研究和回测能力待核实。专用跨所套利与订单执行；未核实内置历史回测或独立研究工作流。 | 814 / 251 | 7 | 277 / 0 | 2017-10-28 / 2023-04-19 | 推送逾一年；MIT |
| 7 | [JulyIghor/QtBitcoinTrader](https://github.com/JulyIghor/QtBitcoinTrader)（C++）：内置实盘功能，研究和回测能力待核实。交易终端可运行JavaScript自动策略；历史回测和系统化研究待核。 | 797 / 371 | 10 | 348 / 41 | 2013-05-14 / 2025-06-25 | 推送逾一年；API 未识别 |
| 8 | [alpacahq/pylivetrader](https://github.com/alpacahq/pylivetrader)（Python）：内置实盘功能，研究和回测需外接。复用Zipline策略接口的执行层；研究和历史回测由外部Zipline等提供。 | 685 / 197 | 17 | 537 / 37 | 2018-08-02 / 2022-10-04 | 推送逾一年；Apache-2.0 |
| 9 | [fremantle-industries/tai](https://github.com/fremantle-industries/tai)（Elixir）：内置实盘功能，研究和回测能力待核实。实时多场所行情与订单执行；未核实历史回测或专门研究工作流。 | 498 / 83 | 6 | 2,000 / 19 | 2017-07-28 / 2024-12-07 | 推送逾一年；MIT |
| 10 | [sky-ecosystem/market-maker-keeper](https://github.com/sky-ecosystem/market-maker-keeper)（Python）：内置实盘功能，研究和回测能力待核实。行情价格源、报价区间和余额约束驱动的做市执行。 | 494 / 184 | 14 | 1,184 / 0 | 2017-11-30 / 2023-03-15 | 已归档；推送逾一年；AGPL-3.0 |
| 11 | [magic8bot/magic8bot](https://github.com/magic8bot/magic8bot)（TypeScript）：内置实盘功能，研究和回测能力待核实。K线、策略信号与订单服务；README未明确完整历史回测流程，默认使用stable分支。 | 410 / 80 | 5 | 164 / 0 | 2018-07-09 / 2023-03-04 | 推送逾一年；MIT |
| 12 | [godzilla-foundation/godzilla-community](https://github.com/godzilla-foundation/godzilla-community)（C++）：内置实盘功能，研究和回测能力待核实。C++执行与Python策略，用于资金费率套利、做市；独立研究回测流程待核。 | 379 / 68 | 1 | 45 / 12 | 2025-02-28 / 2026-08-11 | Apache-2.0 |

## 研究（1）

侧重数据与研究分析工作台，回测和真实策略执行另行标注。

| 组内排名 | 系统与覆盖说明（语言） | Stars / Forks | 贡献账号 | 提交 / Release | 创建 / 最近推送 | 状态与许可 |
| ---: | --- | ---: | ---: | ---: | --- | --- |
| 1 | [Fincept-Corporation/FinceptTerminal](https://github.com/Fincept-Corporation/FinceptTerminal)（C++）：内置研究功能，实盘需外接，回测能力待核实。公开版原生研究分析工作台、数据连接及模拟交易；README将完整Backtest/Algo execution列入私有Enterprise，公开版策略回测边界待核，真实策略执行不按原生计。 | 32,461 / 4,600 | 40 | 1,122 / 34 | 2024-08-29 / 2026-10-13 | [AGPL-3.0-or-later；另有商标条款](https://github.com/Fincept-Corporation/FinceptTerminal#license) |

## 回测与市场仿真（1）

侧重历史模拟或合成市场、撮合和多智能体实验。

| 组内排名 | 系统与覆盖说明（语言） | Stars / Forks | 贡献账号 | 提交 / Release | 创建 / 最近推送 | 状态与许可 |
| ---: | --- | ---: | ---: | ---: | --- | --- |
| 1 | [abides-sim/abides](https://github.com/abides-sim/abides)（Python）：内置研究功能，提供部分回测功能，不提供实盘功能。多智能体市场微观结构研究；离散事件合成市场仿真，区别于历史策略回测。 | 584 / 166 | 3 | 43 / 3 | 2019-03-06 / 2023-07-06 | 推送逾一年；API 未识别 |

## 交易基础设施（10）

交易所后台、P2P协议、撮合与账户服务等单独归类。

| 组内排名 | 系统与覆盖说明（语言） | Stars / Forks | 贡献账号 | 提交 / Release | 创建 / 最近推送 | 状态与许可 |
| ---: | --- | ---: | ---: | ---: | --- | --- |
| 1 | [bisq-network/bisq](https://github.com/bisq-network/bisq)（Java）：提供交易基础设施。P2P报价、钱包、交易协议及仲裁；不将人工P2P真实交易等同量化策略实盘执行。 | 5,142 / 1,310 | 249 | 20,443 / 153 | 2014-03-12 / 2026-10-13 | AGPL-3.0 |
| 2 | [hpyhacking/peatio](https://github.com/hpyhacking/peatio)（Ruby）：提供交易基础设施。交易所账户钱包、撮合、充值提现与后台，非策略研究或量化实盘运行器。 | 3,604 / 2,717 | 24 | 3,048 / 1 | 2014-02-12 / 2026-07-09 | API 未识别 |
| 3 | [viabtc/viabtc_exchange_server](https://github.com/viabtc/viabtc_exchange_server)（C）：提供交易基础设施。交易所撮合、余额、行情及API后台；三个能力维度针对策略研发流程。 | 2,793 / 1,503 | 16 | 506 / 0 | 2017-03-06 / 2021-12-17 | 推送逾一年；MIT |
| 4 | [jammy928/CoinExchange_CryptoExchange_Java](https://github.com/jammy928/CoinExchange_CryptoExchange_Java)（Java）：提供交易基础设施。数字资产交易所撮合、钱包RPC、管理端和客户端。 | 1,732 / 1,309 | 1 | 10 / 0 | 2020-03-27 / 2024-05-12 | 推送逾一年；Apache-2.0 |
| 5 | [haveno-dex/haveno](https://github.com/haveno-dex/haveno)（Java）：提供交易基础设施。P2P交易、Monero多签和争议处理；交易协议部署不作为策略实盘覆盖。 | 1,404 / 190 | 50 | 3,056 / 30 | 2020-02-24 / 2026-10-14 | AGPL-3.0 |
| 6 | [openware/opendax](https://github.com/openware/opendax)（Ruby）：提供交易基础设施。多服务交易所部署，参考账户、撮合、钱包、网关与权限。 | 787 / 583 | 24 | 386 / 0 | 2019-07-24 / 2023-12-02 | 推送逾一年；Apache-2.0 |
| 7 | [robaho/go-trader](https://github.com/robaho/go-trader)（Go）：提供交易基础设施。提供模拟交易所、FIX、撮合和行情服务，供外部策略测试。 | 520 / 90 | 2 | 148 / 2 | 2018-08-24 / 2026-06-30 | GPL-3.0 |
| 8 | [paritytrading/parity](https://github.com/paritytrading/parity)（Java）：提供交易基础设施。交易所、FIX网关、行情和终端；参考服务端交易基础设施。 | 498 / 186 | 7 | 896 / 7 | 2014-04-03 / 2022-02-09 | 已归档；推送逾一年；Apache-2.0 |
| 9 | [blinktrade/bitex](https://github.com/blinktrade/bitex)（Python）：提供交易基础设施。撮合与HTTP/WebSocket网关等交易所后台。 | 424 / 230 | 8 | 849 / 0 | 2013-04-21 / 2017-04-17 | 推送逾一年；GPL-3.0 |
| 10 | [michaelliao/warpexchange](https://github.com/michaelliao/warpexchange)（Java）：提供交易基础设施。证券交易所教育工程，覆盖撮合、资产、清算与服务接口。 | 383 / 148 | 1 | 14 / 0 | 2022-06-05 / 2025-04-17 | 推送逾一年；GPL-3.0 |

## 补充官方资料

以下资料补充各条目中的官方仓库，供核对配套组件与能力边界。

| 项目 | 官方资料 |
| --- | --- |
| vnpy/vnpy | [CTA策略模块](https://github.com/vnpy/vnpy_ctastrategy)、[CTP交易网关](https://github.com/vnpy/vnpy_ctp) |
| wondertrader/wondertrader | [wtpy应用层](https://github.com/wondertrader/wtpy) |
| microsoft/qlib | [在线预测与更新](https://qlib.readthedocs.io/en/stable/component/online.html) |
| jesse-ai/jesse | [实盘插件说明](https://docs.jesse.trade/docs/livetrade) |
| fasiondog/hikyuu | [OrderBroker接口](https://hikyuu.readthedocs.io/zh-cn/latest/trade_manage/OrderBroker.html) |
| pmorissette/bt | [策略示例](https://pmorissette.github.io/bt/docs/source/examples.html) |
| stefan-jansen/zipline-reloaded | [API参考](https://zipline.ml4trading.io/api-reference.html) |
| ScottfreeLLC/AlphaPy | [官方文档](https://alphapy.readthedocs.io/_/downloads/en/stable/pdf/) |
| fbertram/TuringTrader | [项目资料入口](https://www.turingtrader.org/)、[研究与优化示例](https://www.turingtrader.org/wp-content/uploads/help/articles/qsg/FiveCentTour.html) |
| hummingbot/hummingbot | [Dashboard](https://hummingbot.org/dashboard/)、[回测说明](https://hummingbot.org/dashboard/backtest/) |
| thrasher-corp/gocryptotrader | [回测模块](https://github.com/thrasher-corp/gocryptotrader/tree/master/backtester) |
| cassandre-tech/cassandre-trading-bot | [历史回测与模拟说明](https://github.com/cassandre-tech/cassandre-trading-bot/blob/development/docs/docs/cassandre_basics/dry-mode-and-backtesting.md) |

## 已知边界与复筛入口

- FinceptTerminal 记录公开版的研究终端、模拟交易和接入设计；Enterprise 的实时执行等能力按商业版单独核对，不把商业功能自动计入公开源码。
- StarQuant 公开代码覆盖基础回测与实盘，部分 Pro 功能闭源，更新过的回测代码在 `backtest` 分支。Jesse 记录公开研究回测框架，实盘插件另核。
- TradeFrame 的根许可文件同时出现旧许可协议声明和 GPL 文字，许可状态单独标为待核；参考模块包括订单、持仓、期权组合和模拟应用。
- Zipline 家族仅计持续维护的 Zipline Reloaded；FinRL 与 FinRL-Trading 分别是训练研究环境和后继交易基础平台，所参考的层次分别注明。Haveno 为 Bisq 独立交易产品，保留并标明其衍生关系；其官方仓库面向测试开发，真实交易需第三方网络或自建主网。
- TA4J 以完整策略回测框架计入，实盘 OMS 需外接；ABIDES、TradingGym、TensorTrade、FinRL 等以仿真或研究系统计入。
- 回测、实盘、券商接入等用途依据官方 README 的功能定位；历史接口和第三方服务是否仍可用，需要后续实际验证。

## 达标但不计入主名单的项目

### 许可受限或源码镜像

| 项目 | Stars / Forks | 处理依据 |
| --- | ---: | --- |
| [ricequant/rqalpha](https://github.com/ricequant/rqalpha) | 6,824 / 1,791 | [非商业使用等许可限制，转入源码可见受限附录](https://github.com/ricequant/rqalpha) |
| [polakowo/vectorbt](https://github.com/polakowo/vectorbt) | 9,367 / 1,204 | [Apache-2.0附加Commons Clause，转入源码可见受限附录](https://github.com/polakowo/vectorbt#license) |
| [StockSharp/StockSharp](https://github.com/StockSharp/StockSharp) | 10,884 / 2,282 | [当前许可为proprietary/EULA，转入源码可见受限附录](https://github.com/StockSharp/StockSharp/blob/master/LICENSE) |
| [curtiszimmerman/AlgoTrader](https://github.com/curtiszimmerman/AlgoTrader) | 125 / 121 | [旧版源码镜像且历史许可争议；不计独立系统](https://github.com/curtiszimmerman/AlgoTrader) |

### 范围与同源去重

| 项目 | Stars / Forks | 处理依据 |
| --- | ---: | --- |
| [byrnexu/betterquant](https://github.com/byrnexu/betterquant) | 231 / 67 | 与BetterQuant2同一家族；本轮保留后继版作为候选 |
| [quantopian/zipline](https://github.com/quantopian/zipline) | 20,150 / 5,042 | 与Zipline Reloaded同一家族；本轮保留持续维护分支 |
| [exchange-core/exchange-core](https://github.com/exchange-core/exchange-core) | 2,646 / 975 | 可嵌入撮合/风控引擎，缺独立网关；移到组件参考 |
| [mark-sch/RedGekko](https://github.com/mark-sch/RedGekko) | 121 / 27 | Haehnchen交易机器人衍生；本轮只计原项目 |
| [fremantle-industries/workbench](https://github.com/fremantle-industries/workbench) | 122 / 22 | 配套Tai运营终端，避免与底层交易系统重复计数 |
| [Codaone/DEXBot](https://github.com/Codaone/DEXBot) | 247 / 127 | 历史独立衍生产品；本轮优先独立原始系统 |

### 数字未达到门槛

| 项目 | Stars / Forks | 创建日期 | 未达项 |
| --- | ---: | --- | --- |
| [frankfan007/thunder-trader](https://github.com/frankfan007/thunder-trader) | 82 / 52 | 2020-07-09 | Stars < 100 |
| [lanpishu6300/crypto-exchange](https://github.com/lanpishu6300/crypto-exchange) | 390 / 21 | 2025-12-02 | 仓库历史不足1年 |
| [quantiacs/toolbox](https://github.com/quantiacs/toolbox) | 84 / 23 | 2020-12-01 | Stars < 100 |
| [edeng23/binance-trade-bot](https://github.com/edeng23/binance-trade-bot) | 14 / 9 | 2025-02-20 | Stars < 100；Forks < 20 |
| [QUANTAXIS/quantaxis-rs](https://github.com/QUANTAXIS/quantaxis-rs) | 21 / 19 | 2020-12-11 | Stars < 100；Forks < 20 |
| [Marketcetera/marketcetera](https://github.com/Marketcetera/marketcetera) | 85 / 39 | 2020-11-17 | Stars < 100 |
| [apssouza22/trading-system](https://github.com/apssouza22/trading-system) | 76 / 15 | 2018-09-25 | Stars < 100；Forks < 20 |

另有 36 个符合范围且数字达标的候补，在本轮候选集中的排序未进入前 100。例如 QuantFabric（264 Stars / 85 Forks）达到初始门槛，按本轮统一排序进入候补。长期参考项目见[系统参考资料](trading-systems.md#开源系统与组件)。

## 维护记录

2026-10-14：保存数字初筛标准，采集 153 个候选并选取 100 个；补齐贡献账号数。随后按研究／回测／实盘主要覆盖定位重新分组，各组按 Stars、Forks 降序排列，语言作为辅助字段；补充公开组件、商业插件、实时模拟与历史回测的能力边界。文档数字采用当日采集值，后续更新另记采集日期。移除项目中的过程JSON，能力标记改为简短完整语句；长期重点参考项目与候选资料清单分别维护。
