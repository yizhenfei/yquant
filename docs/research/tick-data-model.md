# Tick 数据分类与固定 Slot 存储

核对日期：2026-10-15。

Tick 行情按事件语义分类，资产和交易场所通过合约元数据关联。成交、报价和单条盘口更新通常可以编码成定长载荷；深度快照、更新批次和期权链需要明确容量或分片。固定大小的物理 Slot 可以覆盖这些数据，但不能假设所有逻辑消息都有同一个市场通用的最大长度。

## 参考实现

| 项目 | 实际对象和语义 | 对数据模型的启示 |
| --- | --- | --- |
| WonderTrader | `WTSTickStruct` 是最新价、统计量和十档盘口组成的综合快照；另有 `WTSOrdQueStruct`、`WTSOrdDtlStruct`、`WTSTransStruct`。 | 综合快照、价位委托队列、逐笔委托及成交／撤单流分别表达。其共享内存广播已经使用类型标签加定长 union。 |
| vn.py | `TickData` 包含最新成交信息、累计统计量和固定五档盘口；`ContractData` 保存合约规格。 | 常用快照可以统一，静态合约信息单独维护。Python 字符串和任意 `extra` 字典不能直接作为定长 IPC 布局。 |
| LEAN | `TickType` 分为 `Trade`、`Quote`、`OpenInterest`，由一个 `Tick` 类承载不同字段；Quote 是一档报价。 | 事件种类与 `SecurityType` 独立。固定数量的业务字段不意味着原生 C# 对象或其字符串表示定长。 |
| NautilusTrader | 分开定义 `QuoteTick`、`TradeTick`、`OrderBookDelta`、`OrderBookDeltas`、`OrderBookDepth`，并有参考价、资金费率、Greeks、状态等事件。 | 单条事件与批次容器分开；盘口粒度与资产类别独立；扩展数据通过独立类型保留。 |
| Backtrader | `TimeFrame.Ticks` 是时间粒度，默认数据线仍为 OHLC、volume、openinterest；IB 成交被映射成 O=H=L=C 的一条数据，OANDA 路径选取 bid 或 ask。 | 框架中的 Tick 也可能是驱动策略的数据更新粒度，不能据此推导一套完整的原始市场事件类型。 |
| Freqtrade | 主要围绕 OHLCV 工作，同时提供交易所 ticker、L2 orderbook、public trades、funding rate；公共成交被转换为七列记录。 | ticker、成交和订单簿是不同对象；列表、字典以及 DataFrame 属进程内表示，需另做 IPC 编码。 |

定义与消费路径：WonderTrader [行情结构][wt-struct]、[共享内存广播][wt-shm]；vn.py [Tick 与合约对象][vn-object]、[累计成交量处理][vn-bar]；LEAN [类型枚举][lean-types]、[Tick 字段与 reader][lean-tick]；Nautilus [Data 枚举][nt-data]；Backtrader [数据线][bt-lines]、[IB 成交适配][bt-ib]、[OANDA 报价适配][bt-oanda]；Freqtrade [行情类型][ft-types]、[公共成交列][ft-columns]、[成交转换][ft-converter]、[策略成交数据接口][ft-trades]。

WonderTrader 的快照 `volume` 可以由相邻累计量相减得到，不能直接解释成交易所的一笔成交；vn.py 的分钟线也使用相邻累计量求差。账户执行对象另行归类：vn.py 的 `TradeData` 是订单成交回报，`QuoteData` 是双边报价订单状态，都不属于公共市场的成交 Tick 或 BBO。[WonderTrader 切片语义][wt-parser]、[成交量求差][wt-volume]、[vn.py 对象定义][vn-object]。

`WTSTransStruct` 还承载来源逐笔流中的委托撤单。这类记录映射到委托更新的取消／删除动作，与撤销已成交记录的 `TradeAmendment` 分开。[XTP 成交与撤单转换][wt-transaction]。

## 事件类型与载荷对象

### 公共事件头

各类事件共享标识、来源、顺序和时间信息，业务字段放入对应载荷。下列结构是 YQuant 的布局建议，字段宽度用于后面的容量计算。

```cpp
struct EventHeader {
    std::uint64_t sequence;          // 发布通道内的事件序号
    std::uint64_t source_sequence;   // 上游序号，是否提供由 flags 指示
    std::uint64_t ts_event_ns;       // 事件时间，时钟与精度由接入规范定义
    std::uint64_t ts_recv_ns;        // 本机接收时间，时钟域由接入规范定义
    std::uint64_t instrument_id;     // 内部合约 ID
    std::uint32_t source_id;         // 来源及通道配置 ID
    std::uint32_t source_session_id; // 来源重连或序号重置的代次
    std::uint16_t kind;              // 事件种类
    std::uint16_t schema_version;    // 载荷布局版本
    std::uint32_t payload_bytes;
    std::uint32_t metadata_version;  // 解释价格精度、数量单位等所需的元数据版本
    std::uint32_t flags;
}; // 按 8 字节对齐时为 64 B
```

接入规范还需定义上游序号的作用域、交易日和时间精度。交易所时间与本机单调时钟不能直接相减。缺失的时间、方向、数量和价格使用存在位图表达；零值不统一承担“缺失”的含义。

### 市场事件

表中的载荷名称是 YQuant 的建议分类；不同项目中的对应对象见参考实现及链接。类型之间按事件含义区分，某项字段是否存在由来源能力定义。

| 类型 | 建议载荷对象 | 主要内容 | 存储形态 |
| --- | --- | --- | --- |
| 成交 | `Trade` | 成交价、成交量、成交 ID、可选主动方向、交易条件；聚合成交另带聚合标记及来源提供的成交编号范围。 | 单条记录可定长。 |
| 成交撤销或更正 | `TradeAmendment` | 原成交引用、动作；更正时携带替代字段。已成交的撤销与未成交委托的撤单分别表达。 | 单条记录可定长；具体动作由源协议定义。 |
| 最佳买卖报价 | `Quote` | bid/ask 价格与数量、两侧有效标记、可选报价场所或参与者 ID。标明单场所报价或综合报价的范围。 | 单档可定长；部分外汇来源不提供数量。 |
| 按价位聚合的盘口快照 | `BookSnapshot` | 买卖两侧 `BookLevel[]`，每档价格、聚合数量、可选委托笔数；档数、视图深度、快照边界和对应上游序号。 | Top N 可定长；任意深度为变长集合。 |
| 按价位聚合的盘口增量 | `BookLevelUpdate` | 买卖方向、价位或档位、数量、可选委托笔数、增加／更新／删除／清空动作；明确数量是替换值还是变化量。 | 每个更新项可定长；一条上游消息可能包含多项。 |
| 按委托逐笔更新的盘口 | `OrderBookOrderEvent` | 委托 ID、动作、方向、价格、数量；替换时可含新旧 ID，成交时可含执行引用。 | 单条事件通常可定长；整个订单簿是集合。 |
| 某价位的委托队列 | `OrderQueue` | 方向、价格、该价位总订单数、实际携带项数、依次排列的委托量；仅在来源提供时携带各项委托 ID。 | 队列为变长；可以定义 Top K 视图。 |
| 交易统计快照 | `SessionStatistics` | 最新价量、开高低、累计成交量与成交额、前收、结算和前结算、涨跌停等，附统计区间和字段有效标记。 | 选定字段后可定长。累计量和区间量分开命名。 |
| 市场持仓量 | `OpenInterest` | 全市场未平仓数量、可选变化量、计量单位和统计时间。 | 标量可定长；与账户持仓分开。 |
| 参考价格 | `ReferencePrice` | 价格值、价格种类，如 mark、index、settlement；可选计算来源和有效区间。 | 每个价格更新可定长。 |
| 资金费率 | `FundingRate` | 费率、周期、预测或已确定状态、可选下一结算时间。 | 标量可定长；采用固定宽度十进制编码。 |
| 期权估值与敏感度 | `OptionAnalytics` | IV、delta/gamma/vega/theta/rho、标的参考价、计算约定及模型或数据来源。 | 固定字段集可定长；一整条期权链不是单条事件。 |
| 市场强平 | `Liquidation` | 方向、委托或执行价格、均价、最近与累计执行量等，保留来源的事件含义。 | 单条记录可定长；不能直接等同普通市场成交。 |
| 竞价指示 | `AuctionIndicative` | 参考价、近端／远端指示价格、匹配量、未匹配量及方向、竞价阶段。实际竞价成交仍归入成交事件。 | 固定字段集可定长。 |
| 合约交易状态 | `InstrumentStatus` | 开闭市、交易或报价许可、暂停、价格限制状态、原因代码及可选说明。 | 代码可定长，自由说明为变长扩展。 |
| 收盘或合约终止 | `InstrumentClose` | 收盘价格和关闭类型，如交易时段结束、合约到期。 | 单条记录可定长；保留关闭原因，区别于单纯参考价更新。 |

Nautilus 的 [成交][nt-ticks]、[报价][nt-quote]、[盘口增量][nt-delta]、[标记与指数价格][nt-prices]、[费率][nt-funding]、[期权数据][nt-options]、[状态][nt-status]、[收盘事件][nt-close]、[Binance 扩展对象][nt-binance-types]支撑这些对象的分离。其 Binance 适配器还把同一上游消息拆成多个标准事件，并单独保留供应商扩展字段。[解析路径][nt-binance-mark]。

成交撤销和竞价指示补充参考 Nasdaq TotalView ITCH 5.0 的 1.5.3、1.6 节：Broken Trade 引用已成交记录，NOII 提供竞价匹配与不平衡信息；这两者不能通过普通价量成交记录完整表达。[官方协议][nasdaq-itch]。

### 综合快照与集合

综合行情快照可以作为面向策略的独立订阅产品：`Quote + LastTradeState + SessionStatistics + 可选 OpenInterest + Top N Book`。这对应 WonderTrader 和 vn.py 的常见用法。各组成部分可能来自不同更新时间，应保留存在位图及必要的组成部分时间，避免把快照误认为所有字段都刚刚更新。

以下对象按集合或聚合数据处理：

| 对象 | 内容 | 与单条 Tick 的关系 |
| --- | --- | --- |
| `TradeBatch`、`BookUpdateBatch` | 多笔成交或多条盘口变更。 | 批次长度可变，成员通常可单独定长；保留同一来源事件的边界。 |
| `FullBookSnapshot` | 某时点完整的价位或委托集合。 | 大小随真实盘口变化，不能从单条更新大小推出完整盘口上限。 |
| `OptionChainSnapshot` | 某标的、系列或到期日下的一组期权合约状态。 | 合约集合动态变化，适合按合约拆分并发布集合版本。 |
| `Bar` | 按时间、成交笔数或其他规则聚合的 OHLCV。 | 保留为聚合行情类型，窗口或聚合规则是其身份的一部分。 |

LEAN 的 [Ticks 列表][lean-ticks]、[Slice 分组][lean-slice]和[期权链][lean-chain]都是容器；[OptionContract 的 Greeks][lean-option]可以来自定价模型。Nautilus 的 [OrderBookDeltas][nt-deltas]包含 `Vec`，[OrderBookDepth][nt-depth]支持任意深度，[OptionChainSlice][nt-options]包含按行权价组织的映射。

## 市场与交易标的的组织

### 分类维度

用四个独立维度描述数据，避免建立“每种资产一套成交和报价类”的重复体系：

| 维度 | 内容 | 组织位置 |
| --- | --- | --- |
| 事件语义 | 成交、报价、盘口快照、盘口更新、统计、状态等。 | `EventHeader.kind` 与相应 payload schema。 |
| 资产与合约形态 | 权益、外汇、商品、债务、数字资产等基础类别；现货、期货、期权、永续、价差等合约形态。 | `InstrumentDefinition`，通过 `instrument_id` 关联。 |
| 交易与数据来源 | 交易场所、数据供应商、单场所或综合市场范围、通道及会话。 | 合约身份与 `SourceDefinition`；不能只靠 symbol 字符串区分。 |
| 数据能力与视图 | L1 最佳报价、L2 按价位聚合、L3 按订单；Top N 或全量；快照或增量；合并频率与批次规则。 | `FeedProfile`、订阅配置和 schema 版本。 |

LEAN 的 `TickType` 与 `SecurityType` 分开；Nautilus 的 `AssetClass`、`InstrumentClass`、`BookType` 也各自定义。合约元数据单独承载币种、步长、乘数、到期和行权价。[LEAN 枚举][lean-types]、[Nautilus 枚举][nt-enums]、[Instrument 接口][nt-instrument]。

### 不同市场的字段差异

| 市场或合约 | 共用载荷 | 接入时需明确的差异 |
| --- | --- | --- |
| 股票、ETF 等权益产品 | Trade、Quote、Book、SessionStatistics、Status。 | 股数与交易手数、逐笔类型、交易条件、竞价、价格限制；不同来源提供的盘口深度和委托信息不同。 |
| 商品或金融期货 | 相同的成交与盘口对象，增加 OI、结算等更新。 | 张数、合约乘数、成交额计量、交易日与自然日、夜盘和交割。 |
| 股票或期货期权 | Trade、Quote、Book、OI；按来源另订阅 OptionAnalytics。 | 标的、行权价、方向、到期与行权方式放合约定义；IV/Greeks 的来源和计算约定随分析事件发布。 |
| OTC 外汇、CFD | 主要复用 Quote，来源支持时增加 Trade 或深度。 | 报价可能没有数量；场所或流动性提供者范围、价格单位和成交量可用性需显式描述。 |
| 数字资产现货 | Trade、Quote、Book、统计快照。 | 基础币数量与计价币成交额、成交聚合规则、价格数量精度、批量深度更新。 |
| 永续与数字资产期货 | 基础成交与盘口，加 mark/index、FundingRate、OI 和可选 Liquidation。 | 线性、反向或 quanto 合约的数量及结算单位不同，不能套用一个通用名义价值公式。 |

具体差异有源码依据：WonderTrader 的 [CTP 适配][wt-ctp]填入期货持仓、结算和五档报价，[OES 适配][wt-oes]分别处理股票十档和期权五档；LEAN [reader][lean-tick]按市场和资产读取不同字段；Freqtrade [资金费率接口][ft-provider]也明确返回字段取决于交易所。

YQuant 的合约定义保存符号原文、资产类别、场所、币种、价格与数量尺度、步长、乘数、合约数量单位及衍生品条款；`metadata_version` 使历史事件能够按当时的定义解释。动态价格限制、交易状态及结算值采用更新事件，不能一律当作永久静态字段。

数值可以采用固定宽度的定点整数或十进制表示。价格保留有符号能力；数量、成交额、费率分别定义单位与范围。64 位编码的价格范围和精度必须按接入合约验算；需要时提供 128 位 profile。Nautilus 的 [固定数值配置][nt-fixed]实际区分 64 位标准模式和 128 位高精度模式，精度变化并不必然要求变长字符串。

## 固定 Slot 与变长数据

### 原生对象和传输布局

原生对象的字段个数、`sizeof` 和序列化长度分别判断。Python/C# 字符串、Rust 的 `Vec`、超出内联容量的 `SmallVec`、映射及驻留字符串引用，都需要转换成自包含的 IPC 表示。静态符号可以登记为固定宽度 ID；原始成交 ID、条件和说明若需无损保存，保留字典映射或扩展载荷，不能依赖可能碰撞的裸哈希替代身份。

IPC schema 固定字段宽度、端序、对齐和数值编码，并通过版本区分布局；共享载荷不包含进程私有的堆地址。

WonderTrader 是现成的定长例子：当前四个载荷在 Apple clang 21、arm64、8 字节 packing 下分别为 Tick **512 B**、委托队列 **280 B**、逐笔委托 **96 B**、逐笔成交 **112 B**。其 `ShmCaster::DataItem` 使用 `uint32_t` 类型标签加 union，队列有 8192 个槽。这些数值说明该固定字段集可以定长编码；跨平台布局仍需检查对齐和字段宽度。[结构定义][wt-struct]、[广播队列][wt-shm]。

### 变长来源及上限

| 数据 | 实际变长来源 | 可以采用的上限或处理规则 |
| --- | --- | --- |
| 合约、场所、名称 | vn.py/LEAN 字符串、Nautilus Symbol 等。 | 高频载荷使用登记 ID；原文字节上限由已选接入协议和字典服务定义，超长明确拒绝或走扩展。 |
| 成交、委托 ID | 来源可能用数字，也可能用字符串。 | 数字原样定宽；文本按来源规范内联或建立无损映射。Nautilus [TradeId][nt-trade-id] 的 36 字节 ASCII 上限仅适用于其模型。 |
| 盘口档位 | Top N 或全量深度，实际 bid/ask 数量可不同。 | Top 5/10/20 等产品可固定数组加有效项数；全量盘口分片，或明确改订阅为有深度限制的视图。 |
| 委托队列 | 一个价位的订单数随市场变化。 | Top K 视图固定容量，保留真实总数、携带数和截断标记；完整队列通过分片传输。 |
| 更新或成交批次 | 同一上游消息携带多条记录。 | 拆成定长成员，或每槽容纳固定数量成员；保留批次身份、顺序及结束标记。 |
| 期权链、组合腿或多标的消息 | 成员数量随合约集合或产品定义变化。 | 按合约／腿拆分，发布集合版本和成员数；单条整链不设全市场统一小上限。 |
| 状态说明、供应商扩展、原始 JSON | 自由字符串、任意字典和自定义结构。 | 标准字段归一化；其余走版本化扩展通道，并规定字节预算及超限策略。 |

上限分成三个层次维护：**源协议硬上限**、**所订阅产品的深度或容量**、**本系统的内存预算**。容量超限可以导致分片、明确拒绝或切换视图；系统预算不能自动成为“完整数据”的截断依据。

以下边界已有直接证据：

- WonderTrader 使用 `volumes[50]`，XTP 适配器明确将队列截到 50 项，并保留总订单数。因此 50 是这一载荷视图的容量，不是完整价位队列的市场上限。[XTP 队列转换][wt-xtp]。
- 当前 Nautilus `OrderBookDepth` 的十档是 `SmallVec` 内联容量，超过十档会使用堆；Arrow 编码为 List。其 SBE 编码却要求恰好十档，Databento MBP10 输入也恰好十档。模型、编码格式和订阅产品的限制各自独立。[通用深度][nt-depth]、[Arrow][nt-arrow]、[SBE][nt-sbe-depth]、[MBP10 适配][nt-mbp10]。
- Freqtrade `OrderBook` 使用变长 bids/asks 列表；WebSocket 快照按调用方的 limit 切片，上限还取决于各交易所配置。[类型][ft-types]、[快照复制][ft-orderbook]、[请求深度调整][ft-limit]。
- Binance Spot 的部分深度产品提供 5、10、20 档；REST 深度接口的请求 limit 最大为 5000，这个返回深度限制不能证明实际完整订单簿也具有相同上限。[部分深度产品][binance-ws]、[REST 深度限制][binance-rest]。

### 容量计算示例

以下采用 64 B 事件头、32 B 快照前缀、64 位价格与数量。盘口每档定义为：

```cpp
struct BookLevel {
    std::int64_t price;
    std::uint64_t quantity;
    std::uint32_t order_count;
    std::uint32_t flags;       // 如 order_count 是否存在
}; // 24 B
```

双边 N 档的消息大小为 `64 + 32 + 2 × N × 24 = 96 + 48N` 字节。下表另给 8 B 槽位并发控制字留空间，槽位按 64 B 对齐；实际原子表示的宽度仍由目标平台实现验证。

| 视图 | 消息字节数 | 示例固定 Slot |
| --- | ---: | ---: |
| 双边五档 | 336 B | 512 B |
| 双边十档 | 576 B | 1024 B |
| 双边二十档 | 1056 B | 2048 B |
| 双边五十档 | 2496 B | 4096 B |

成交、单档报价、单条盘口更新及标量统计通常可以采用更小的载荷族。可按载荷族设置固定槽尺寸，例如小事件通道与快照通道分开，避免每个成交都占用深度快照的空间。实际尺寸由选定 schema 的字段、128 位数值、内联 ID、扩展信息及同步开销共同计算，不能把上表当成所有 Tick 的上界。

如果统一使用一种物理 Slot，超过单槽容量的逻辑消息仍可以分片。分片至少包含 `message_id`、分片序号和总数或结束标记、总有效长度／成员数及布局版本；消费者只接受完整且版本一致的一组分片。每个成员可定长的批量更新，可以改为逐项发布，保留 `batch_id` 和事件结束标记。Nautilus 的 [Binance 盘口转换][nt-binance-delta]就是按实际项数生成 delta，再给最后一项添加结束标记。

分片总量仍需配置资源预算。超过预算时不发布貌似完整的部分结果，应产生明确的超限结果，并选择更大通道、流式快照或重新订阅有限视图。若使用外部载荷区，固定槽中的偏移引用还需绑定代次和独立的有效性协议；固定描述符不会自动解决载荷区复用问题。

### 跳过旧数据后的恢复

允许跳过数据时，按事件语义选择恢复方式：

- 完整报价或 Top N 快照：读取最新的完整版本即可恢复这个视图。
- L2/L3 增量：丢失中间更新后，取得匹配的完整快照和边界序号，再续接增量；最新一条 delta 不能恢复订单簿。
- 成交：最新一笔不能补回逐笔统计。需要完整成交序列的策略重新加载历史或重置依赖该序列的状态；累计成交量只能补回部分聚合信息。
- 分片消息：丢失任意片时丢弃整组，寻找下一组完整版本。

Slot 的大小只决定容量。读后校验方案还要求槽位有不会混淆复用代次的版本协议，以及允许并行覆盖的合法访问方式；定长结构和 `memcpy` 本身不提供这些同步保证。

## 源码版本

| 项目 | 核对提交 |
| --- | --- |
| WonderTrader | `08b230dd05facf6d650d949bfe51054115a2ecb1` |
| vn.py | `c6e231caf32b7fc97e6459817fff66458cf7e7c4` |
| Backtrader | `b853d7c90b6721476eb5a5ea3135224e33db1f14` |
| Freqtrade | `b33068949fc16e69e288e0a5bef5bd1f086f0a27` |
| LEAN | `80e7843f645673bcbeaab963049f76f20f6785e1` |
| NautilusTrader | `ce453eb995731e3873f0267dda5e4c3a7c514ac6` |

[wt-struct]: https://github.com/wondertrader/wondertrader/blob/08b230dd05facf6d650d949bfe51054115a2ecb1/src/Includes/WTSStruct.h#L146-L290
[wt-shm]: https://github.com/wondertrader/wondertrader/blob/08b230dd05facf6d650d949bfe51054115a2ecb1/src/WtDtCore/ShmCaster.h#L16-L43
[wt-parser]: https://github.com/wondertrader/wondertrader/blob/08b230dd05facf6d650d949bfe51054115a2ecb1/src/Includes/IParserApi.h#L44-L49
[wt-volume]: https://github.com/wondertrader/wondertrader/blob/08b230dd05facf6d650d949bfe51054115a2ecb1/src/WtDataStorage/WtDataWriter.cpp#L1521-L1547
[wt-ctp]: https://github.com/wondertrader/wondertrader/blob/08b230dd05facf6d650d949bfe51054115a2ecb1/src/ParserCTP/ParserCTP.cpp#L292-L350
[wt-oes]: https://github.com/wondertrader/wondertrader/blob/08b230dd05facf6d650d949bfe51054115a2ecb1/src/ParserOES/ParserOES.cpp#L550-L800
[wt-xtp]: https://github.com/wondertrader/wondertrader/blob/08b230dd05facf6d650d949bfe51054115a2ecb1/src/ParserXTP/ParserXTP.cpp#L355-L410
[wt-transaction]: https://github.com/wondertrader/wondertrader/blob/08b230dd05facf6d650d949bfe51054115a2ecb1/src/ParserXTP/ParserXTP.cpp#L254-L285
[vn-object]: https://github.com/vnpy/vnpy/blob/c6e231caf32b7fc97e6459817fff66458cf7e7c4/vnpy/trader/object.py#L17-L300
[vn-bar]: https://github.com/vnpy/vnpy/blob/c6e231caf32b7fc97e6459817fff66458cf7e7c4/vnpy/trader/utility.py#L255-L310
[lean-types]: https://github.com/QuantConnect/Lean/blob/80e7843f645673bcbeaab963049f76f20f6785e1/Common/Global.cs#L352-L528
[lean-tick]: https://github.com/QuantConnect/Lean/blob/80e7843f645673bcbeaab963049f76f20f6785e1/Common/Data/Market/Tick.cs#L43-L537
[lean-ticks]: https://github.com/QuantConnect/Lean/blob/80e7843f645673bcbeaab963049f76f20f6785e1/Common/Data/Market/Ticks.cs#L21-L25
[lean-slice]: https://github.com/QuantConnect/Lean/blob/80e7843f645673bcbeaab963049f76f20f6785e1/Common/Data/Slice.cs#L669-L680
[lean-chain]: https://github.com/QuantConnect/Lean/blob/80e7843f645673bcbeaab963049f76f20f6785e1/Common/Data/Market/BaseChain.cs#L73-L114
[lean-option]: https://github.com/QuantConnect/Lean/blob/80e7843f645673bcbeaab963049f76f20f6785e1/Common/Data/Market/OptionContract.cs#L55-L108
[nt-data]: https://github.com/nautechsystems/nautilus_trader/blob/ce453eb995731e3873f0267dda5e4c3a7c514ac6/crates/model/src/data/mod.rs#L114
[nt-ticks]: https://github.com/nautechsystems/nautilus_trader/blob/ce453eb995731e3873f0267dda5e4c3a7c514ac6/crates/model/src/data/trade.rs#L44
[nt-quote]: https://github.com/nautechsystems/nautilus_trader/blob/ce453eb995731e3873f0267dda5e4c3a7c514ac6/crates/model/src/data/quote.rs#L51
[nt-trade-id]: https://github.com/nautechsystems/nautilus_trader/blob/ce453eb995731e3873f0267dda5e4c3a7c514ac6/crates/model/src/identifiers/trade_id.rs#L27-L56
[nt-delta]: https://github.com/nautechsystems/nautilus_trader/blob/ce453eb995731e3873f0267dda5e4c3a7c514ac6/crates/model/src/data/delta.rs#L45
[nt-deltas]: https://github.com/nautechsystems/nautilus_trader/blob/ce453eb995731e3873f0267dda5e4c3a7c514ac6/crates/model/src/data/deltas.rs#L33-L57
[nt-depth]: https://github.com/nautechsystems/nautilus_trader/blob/ce453eb995731e3873f0267dda5e4c3a7c514ac6/crates/model/src/data/depth.rs#L32-L75
[nt-prices]: https://github.com/nautechsystems/nautilus_trader/blob/ce453eb995731e3873f0267dda5e4c3a7c514ac6/crates/model/src/data/prices.rs#L42
[nt-funding]: https://github.com/nautechsystems/nautilus_trader/blob/ce453eb995731e3873f0267dda5e4c3a7c514ac6/crates/model/src/data/funding.rs#L40
[nt-options]: https://github.com/nautechsystems/nautilus_trader/blob/ce453eb995731e3873f0267dda5e4c3a7c514ac6/crates/model/src/data/option_chain.rs#L259-L388
[nt-status]: https://github.com/nautechsystems/nautilus_trader/blob/ce453eb995731e3873f0267dda5e4c3a7c514ac6/crates/model/src/data/status.rs#L40
[nt-close]: https://github.com/nautechsystems/nautilus_trader/blob/ce453eb995731e3873f0267dda5e4c3a7c514ac6/crates/model/src/data/close.rs#L43-L53
[nt-enums]: https://github.com/nautechsystems/nautilus_trader/blob/ce453eb995731e3873f0267dda5e4c3a7c514ac6/crates/model/src/enums.rs#L223
[nt-instrument]: https://github.com/nautechsystems/nautilus_trader/blob/ce453eb995731e3873f0267dda5e4c3a7c514ac6/crates/model/src/instruments/mod.rs#L268
[nt-fixed]: https://github.com/nautechsystems/nautilus_trader/blob/ce453eb995731e3873f0267dda5e4c3a7c514ac6/crates/model/src/types/fixed.rs#L85-L106
[nt-arrow]: https://github.com/nautechsystems/nautilus_trader/blob/ce453eb995731e3873f0267dda5e4c3a7c514ac6/crates/serialization/src/arrow/depth.rs#L47
[nt-sbe-depth]: https://github.com/nautechsystems/nautilus_trader/blob/ce453eb995731e3873f0267dda5e4c3a7c514ac6/crates/serialization/src/sbe/market/book.rs#L177-L195
[nt-mbp10]: https://github.com/nautechsystems/nautilus_trader/blob/ce453eb995731e3873f0267dda5e4c3a7c514ac6/crates/adapters/databento/src/decode/market_data.rs#L463-L519
[nt-binance-types]: https://github.com/nautechsystems/nautilus_trader/blob/ce453eb995731e3873f0267dda5e4c3a7c514ac6/crates/adapters/binance/src/data_types.rs#L44-L250
[nt-binance-mark]: https://github.com/nautechsystems/nautilus_trader/blob/ce453eb995731e3873f0267dda5e4c3a7c514ac6/crates/adapters/binance/src/futures/websocket/streams/parse_data.rs#L296-L380
[nt-binance-delta]: https://github.com/nautechsystems/nautilus_trader/blob/ce453eb995731e3873f0267dda5e4c3a7c514ac6/crates/adapters/binance/src/futures/websocket/streams/parse_data.rs#L216-L292
[bt-lines]: https://github.com/mementum/backtrader/blob/b853d7c90b6721476eb5a5ea3135224e33db1f14/backtrader/dataseries.py#L33-L112
[bt-ib]: https://github.com/mementum/backtrader/blob/b853d7c90b6721476eb5a5ea3135224e33db1f14/backtrader/feeds/ibdata.py#L684-L702
[bt-oanda]: https://github.com/mementum/backtrader/blob/b853d7c90b6721476eb5a5ea3135224e33db1f14/backtrader/feeds/oanda.py#L398-L416
[ft-types]: https://github.com/freqtrade/freqtrade/blob/b33068949fc16e69e288e0a5bef5bd1f086f0a27/freqtrade/exchange/exchange_types.py#L94-L116
[ft-columns]: https://github.com/freqtrade/freqtrade/blob/b33068949fc16e69e288e0a5bef5bd1f086f0a27/freqtrade/constants.py#L88-L113
[ft-converter]: https://github.com/freqtrade/freqtrade/blob/b33068949fc16e69e288e0a5bef5bd1f086f0a27/freqtrade/data/converter/trade_converter.py#L36-L89
[ft-provider]: https://github.com/freqtrade/freqtrade/blob/b33068949fc16e69e288e0a5bef5bd1f086f0a27/freqtrade/data/dataprovider.py#L571-L612
[ft-trades]: https://github.com/freqtrade/freqtrade/blob/b33068949fc16e69e288e0a5bef5bd1f086f0a27/freqtrade/data/dataprovider.py#L522-L550
[ft-orderbook]: https://github.com/freqtrade/freqtrade/blob/b33068949fc16e69e288e0a5bef5bd1f086f0a27/freqtrade/exchange/exchange_ws.py#L436-L454
[ft-limit]: https://github.com/freqtrade/freqtrade/blob/b33068949fc16e69e288e0a5bef5bd1f086f0a27/freqtrade/exchange/exchange.py#L2280-L2308
[nasdaq-itch]: https://www.nasdaqtrader.com/content/technicalsupport/specifications/dataproducts/NQTVITCHspecification.pdf
[binance-ws]: https://developers.binance.com/docs/binance-spot-api-docs/web-socket-streams
[binance-rest]: https://developers.binance.com/docs/binance-spot-api-docs/rest-api/market-data-endpoints
