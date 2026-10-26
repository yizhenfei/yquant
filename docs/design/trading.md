# 交易模块设计

交易模块为策略提供交易能力，并管理交易产生的业务状态。

## 功能需求

### 订单与成交管理

- 支持委托与撤单，并反馈处理结果。
- **交易结果处理**：根据回报更新订单状态和成交记录，并跟踪尚未确认结果的请求。
- 记录每笔订单和成交所属的账户与策略。
- 提供订单与成交查询，并在相关信息变化时通知策略。

### 账户管理

- 维护账户资金状态，分别维护账户整体持仓和各策略持仓。
- 明确账户数据的来源、计算规则和更新时机。
- 提供账户状态查询和变化通知。

### 交易环境支持

- 提供统一的交易接口，对接不同交易所。
- 支持真实交易和模拟交易。

### 交易记录与对账

- 保存交易记录，支持历史查询。
- 核对本地记录与柜台数据，识别并处理差异。

### 运行控制

- 启动时完成初始化，并报告是否具备交易条件。
- 支持暂停和恢复新交易请求的受理。

风控功能暂不实现。

## 实现概况

策略通过 `TradingService` 使用交易模块。当前部署中，每个策略进程运行一个策略实例，通过 `yquant-tradingd` 执行交易请求；网关维护账户及订单状态并驱动柜台，策略进程保存查询视图并分发回报。多个策略可以分布在不同机器上，共享同一个网关账户；各进程内部由主线程串行驱动。

现有交易环境包括股票现金模拟、本地期货模拟及 [SimNow 柜台接入](simnow-trading.md)。股票账本使用人民币现金账户，只允许足额买入和卖出现有可卖持仓。本地期货开平仓、保证金、手续费、损益及行情驱动撮合见[国内期货模拟交易](domestic-futures.md)；柜台账户保留查询的资金和持仓，不由本地模拟器重算。股票手续费与税费、融资融券及日切另行扩展。

| 需求相关能力 | 现有设计与待补充内容 |
| --- | --- |
| 交易环境 | 已接入本地模拟及 SimNow，真实柜台接入待扩展 |
| 账户管理 | 当前提供共享账户快照，策略按自身成交归集持仓；策略持仓查询、独立账户变化通知及更新时间说明待补充 |
| 运行控制 | 现有受理开关用于回调阶段和停止流程；独立的暂停、恢复受理接口待定义 |
| 对账 | SimNow 启动时查询柜台账户及持仓；独立对账流程待实现 |

订单、账户及柜台适配的公共业务规则在下文维护。共享部署的身份、传输、账户视图及停止协议见[共享交易网关](tradingd.md)。

行情模型与订阅规则见[行情驱动设计](market-data.md)，动态库所有权与配置初始化见[策略进程设计](strategyd.md)。

## 模块与所有权

| 对象 | 职责与所有权 |
| --- | --- |
| `StrategyContext` | 提供时间及行情、交易和定时服务的访问入口，回调期间借用 |
| `Trading` | 策略可见的交易接口，通过 `context.trading()` 访问 |
| `trading::TradingService` | 宿主使用的抽象交易服务接口，定义生命周期、请求处理与事件提交契约 |
| `trading::OrderCore` | 单笔订单的业务状态机，消费事件并产生提交订单、撤单及账务通知动作 |
| `trading::OrderRequestExecutor` | 执行新单和撤单命令，持有待发送队列及 in-flight 请求表，处理响应、超时和本地结束 |
| `trading::TradingEngine` | 网关的交易引擎，持有订单核心、请求执行器和权威账本，协调预占、索引及状态提交 |
| `trading::TradingAdapter` | 柜台接入边界，提供初始账户快照、发送请求和规范化回报，不调用策略 |
| `trading::MockTradingAdapter` | 以定时脚本生成受理、成交或撤单结果，由使用方持有 |
| `trading::CtpTradingAdapter` | 连接 SimNow 交易桥，接收规范化委托、成交及柜台账户快照 |
| `md::InstrumentScope` | 持有运行范围内的只读标的定义，供行情和交易独立查询 |
| `strategyd::TradingServiceImpl` | 生产环境的交易服务实现，通过 TCP 访问 tradingd，保存本策略订单缓存和网关账户视图 |
| `StrategyRunner` | 驱动请求、行情、交易回报及停止排空，串行调用策略 |
| `StrategyAssembly` | 读取启动配置，持有标的范围、共享内存行情源和网关客户端 |
| `testing::MockTradingService` | 单元测试替身，记录请求并提交预设回报与账户快照，不接入柜台 |

公共交易实现使用 `yquant::trading` 命名空间，构建为 `yquant_trading`。`TradingEngine` 由网关持有，借用柜台适配器、标的范围和时钟，其存活时间覆盖引擎。引擎不实现策略侧 `TradingService`。策略装配层持有独立的只读标的范围与 `TradingServiceImpl`，不创建引擎或柜台适配器；服务与上下文存活时间覆盖全部回调及实例销毁。价格精度与标的身份均来自[共享标的字典](market-data.md#共享标的字典)，构建归属见[目录与模块](engineering.md#目录与模块)。

单笔订单的业务状态由 `OrderCore` 维护，新单与撤单的排队、发送和响应跟踪由 `OrderRequestExecutor` 维护；二者的输入输出及事务边界见[订单核心与请求执行](order-core.md)。

`StrategyRunner` 必须借用一个 `TradingService`。运行时通过 `Init(config, dictionary)` 建立账户，通过 `ProcessRequests(now)` 推进自身传输，通过 `Poll(now, dispatch, event)` 返回已经提交的回调数据。`next_deadline()` 汇总传输与本地工作的期限，`has_ready_event(now)` 单独表示可轮询事件，`is_drained()` 包含账本和传输的排空状态。具体实现选择及所有权见[组件装配](strategyd.md#组件装配)。

## 委托模型

### 标识与计量

公共类型位于 `yquant::model`：

| 类型 | 约定 |
| --- | --- |
| `StrategyOrderId` | strategyd 交易服务分配的非零 `uint64_t`，策略会话内从 1 递增且不复用；策略提交、撤单和查询均使用它 |
| `ClientOrderId` | tradingd 的 `TradingEngine` 分配的非零 `uint64_t`，网关运行代次内单调递增且不复用 |
| `AdapterOrderId` | 适配器提供的稳定订单标识，最多 64 字节；可来自柜台原始编号或组合编码，尚未取得时为空 |
| `ExecutionId` | 适配器提供的单笔成交标识，最多 64 字节；同一订单内唯一，重复回报保持相同值 |
| `CancelRequestId` | 宿主分配的非零 `uint64_t`，区分同一委托的多次撤单尝试 |
| `OrderSide` | `Buy` 或 `Sell` |

柜台标识不能用本地委托 ID 代替，超长标识拒绝而不截断。柜台委托和成交标识的解释范围是当前交易接入会话；重启模拟柜台建立新会话，不继承上一进程的订单。共享网关通过代次与客户端身份区分跨进程的本地订单号，具体规则见[网关协议](tradingd.md#身份归属与协议)。

`StrategyOrderId` 与 `ClientOrderId` 是不同强类型，不隐式转换。策略订单使用 `(generation, client_id, strategy_order_id)` 关联，网关订单使用 `(generation, client_order_id)` 关联。两个数值字段在线路中均显式编码为 64 位小端整数；零为无效值，耗尽时报错，不回绕。网关保存策略订单到核心订单的映射，回报填入策略标识并保留核心标识。

`ExecutionId` 在 CTP bridge 中为 `(TradingDay, ExchangeID, OrderSysID, TradeID, Direction)` 的 NUL 分隔 UTF-8 编码经 SHA-256 得到的 64 字节十六进制文本；模拟柜台为 `mock-fill-<client_order_id>-<成交序号>`，期货模拟撮合为 `FE-<client_order_id>-<成交序号>`。同一成交的重复回报沿用原值。

价格使用 `int64_t`，按标的的 `price_decimals` 解释；数量使用正的 `int64_t`，股票单位为股，期货单位为手，与行情模型一致。交易请求中的标的必须存在于宿主字典中，允许未订阅该标的行情的策略提交委托。

### 请求与快照

`OrderRequest` 包含 `instrument_id`、`side`、`limit_price`、`quantity` 和 `position_effect`。提供限价单，价格和数量都必须大于零，并符合标的的价位及数量步长。股票意图为 `kNone`，本地期货意图见[开平仓](domestic-futures.md#开平仓意图)；柜台普通平仓 `kClose` 及其场所约束见 [SimNow 报单规则](simnow-trading.md#报单成交与撤单)。

`OrderSnapshot` 是可复制、自包含的定长快照：

| 字段 | 含义 |
| --- | --- |
| `strategy_order_id` | 策略会话内的订单标识；核心账本视图为零，路由回报时由网关填入 |
| `client_order_id` | 网关账本标识；本地排队或网关建账前拒绝时为零，取得后保持不变 |
| `adapter_order_id` | 适配器提供的订单标识 |
| `instrument_id`、`side`、`limit_price`、`position_effect` | 原始委托的标的、方向、价格和开平意图 |
| `original_qty` | 原始委托数量，不因成交或撤单改变 |
| `cumulative_filled_qty` | 已入账的累计成交数量 |
| `leaves_qty` | 仍可能成交的数量；非终态为原始数量减累计成交量，终态为零 |
| `state` | `PendingNew`、`Working`、`Filled`、`Canceled` 或 `Rejected` |
| `cancel_pending` | 是否仍有未决撤单请求，独立于委托状态 |
| `cancel_request_id` | 最近一次本地受理的撤单尝试；尚未撤单时为零 |
| `report_sequence` | 最后接受的规范化回报序号；尚无回报时为零 |
| `receive_time_ns` | 最后接受的柜台回报的本地单调时间，与 `Now()` 同基准；尚无回报时为零，本地操作不刷新它 |

例如，原始数量为 200、成交 100 后撤单成功，快照为 `Canceled`、累计成交 100、`leaves_qty=0`；未成交的 100 股不再形成活动委托。

### 状态与撤单

```mermaid
stateDiagram-v2
    [*] --> PendingNew: 本地接收下单请求
    PendingNew --> Working: 柜台受理或首次部分成交
    PendingNew --> Filled: 首次回报即全部成交
    PendingNew --> Rejected: 柜台拒单
    PendingNew --> Canceled: 停止时取消尚未发送的请求
    Working --> Working: 部分成交或撤单被拒绝
    Working --> Filled: 全部成交
    Working --> Canceled: 撤销剩余量成功
    Filled --> [*]
    Canceled --> [*]
    Rejected --> [*]
```

部分成交仍使用 `Working`，通过数量字段表达进度。撤单本地受理后设置 `cancel_pending`，不减少 `leaves_qty`，原委托仍可能成交。该标记覆盖排队、已尝试发送、结果不确定及本地结果尚未消费的阶段；匹配的柜台结果或本地结束结果清除对应标记。撤单拒绝保留委托状态和剩余量。

`Filled`、`Canceled` 和 `Rejected` 不恢复为活动状态。全部成交时，尚未发送的撤单由执行器取消发送，并通过 `CancelNotSent` 本地结果结束该次尝试；已经尝试发送或结果不确定的撤单继续等待匹配结果或对账。两条路径均保留 `Filled`，没有未决撤单时无需等待额外结果。

## 持仓与资金账本

本节描述股票现金账户；期货账户的保证金和四分持仓见[期货账本](domestic-futures.md#保证金与损益)。

### 初始账户与计量

网关先启动柜台会话，读取完整的初始账户快照，再建立账本。策略宿主通过握手取得当前账户视图，成功后才加载策略，因此 `OnStart()` 可查询资金与持仓；共享账户此时可以已有其他策略的交易和预占。

`TradingAccountSnapshot` 用于新柜台会话的账户初始化，包含非负 `cash_balance` 和最多 64 项持仓。每项持仓要求标的存在于行情字典、标的不重复、`0 <= available_qty <= total_qty` 且 `reserved_qty=0`。未列出的字典标的初始持仓为零。初始资金和持仓来自柜台配置，不放入策略参数。共享账户的远端视图另用 `AccountView`，允许已经存在的预占。

公共快照位于 `yquant::model`：

| 类型及字段 | 含义与不变量 |
| --- | --- |
| `CashSnapshot.balance` | 已确认现金余额；成交改变余额，本地下单不改变余额 |
| `CashSnapshot.reserved` | 全部未终结买单的剩余限价资金预占 |
| `CashSnapshot.available` | 可用于新买单的资金，等于 `balance - reserved` |
| `PositionSnapshot.instrument_id` | 持仓标的 |
| `PositionSnapshot.total_qty` | 已确认总持仓，包括本会话买入且尚不可卖的股份 |
| `PositionSnapshot.available_qty` | 当前可用于新卖单的数量，不含已预占数量 |
| `PositionSnapshot.reserved_qty` | 全部未终结卖单的剩余数量预占 |

所有数量和金额均为非负 `int64_t`。持仓满足 `available_qty + reserved_qty <= total_qty`；差额为不可卖持仓。资金统一以微元计量，`1 元 = 1,000,000 微元`，不使用浮点数。价格仍使用标的原有精度：

```text
成交金额或限价金额（微元）= price * quantity * 10^(6 - price_decimals)
```

例如精度为 2、价格为 1001、数量为 200，金额为 2,002,000,000 微元，即 2,002 元。每次乘法、加法及扣减都先检查范围，不通过整数回绕或截断处理溢出。

模拟账本采用本会话买入不可卖的规则：买入成交只增加总持仓，不增加可卖数量；卖出所得立即增加可用资金。新会话的可卖数量重新由初始快照提供。账本不计算可取资金，也不执行自动日切。

### 预占与成交

网关受理时将订单、请求队列及预占一次性提交。多笔买单共用同一份可用资金，多笔卖单共用该标的的可卖数量。仅挂出的卖单不增加可用资金。策略本地排队不修改账户缓存。

| 操作或回报 | 买单 | 卖单 |
| --- | --- | --- |
| 网关受理 | 按 `限价 × 数量` 预占资金，可用资金相应减少 | 从可卖数量转入预占数量 |
| 受理、撤单在途、拒撤 | 保持剩余资金预占 | 保持剩余数量预占 |
| 唯一成交 | 按实际成交额扣余额，释放本笔数量对应的限价预占；总持仓增加 | 总持仓及预占数量减少，按实际成交额增加现金余额 |
| 拒单、撤单成功、发送前取消 | 释放全部剩余资金预占 | 将全部剩余数量预占退回可卖数量 |

买入成交价改善时，限价预占与实际支出的差额立即可用，剩余预占仍为 `限价 × 未成交量`。全部成交后预占归零，即使还有未决撤单请求也不再占用资源。撤单请求本地受理不释放预占，只有确认撤销剩余量才释放。

例如，初始现金 3,000 元，以 10.01 元买入 200 股，随后 100 股以 10.00 元成交，再撤销剩余 100 股：

| 阶段 | 余额（元） | 预占（元） | 可用（元） |
| --- | --- | --- | --- |
| 初始 | 3,000 | 0 | 3,000 |
| 网关受理 | 3,000 | 2,002 | 998 |
| 成交 100 股 | 2,000 | 1,001 | 999 |
| 撤销剩余量 | 2,000 | 0 | 2,000 |

卖出侧，初始总持仓与可卖量均为 300 股，卖出 200 股后为可卖 100、预占 200；成交 100 股后为总持仓 200、可卖 100、预占 100；撤销剩余量后为总持仓及可卖量 200、预占 0。

发送失败可能已经到达柜台，排空超时也不能证明撤单成功；这两种情况保留订单与预占的最终已知事实，供 `OnStop()` 查询并输出诊断。关闭适配器不释放未确认的预占。

### 原子性与查询

每个新回报先校验订单状态、成交幂等、存储容量及全部账本运算，成功后一起提交订单、成交记录、持仓和资金，再执行策略回调。重复回报不重复扣款或增减持仓；协议错误、金额溢出及任一校验失败均保持整笔事件涉及的账本不变。

```cpp
bool TryGetCash(model::CashSnapshot& out) const noexcept;
bool TryGetPosition(
    model::InstrumentId id, model::PositionSnapshot& out) const noexcept;
```

接口通过 `context.trading()` 调用。账户未建立时返回 `false`；未知标的的持仓查询也返回 `false`，失败不改变输出。已建立账户中，字典内无持仓的标的返回零快照。查询在 `OnStart`、运行和排空回调、`OnStop` 中均可使用，输出不借用内部存储。策略侧服务在策略销毁后释放；共享账户账本由网关继续持有，不随某个策略退出而清空。

## 策略接口

### 服务与回调

`Trading` 位于 `yquant::strategy`，关键签名如下：

```cpp
bool accepting_orders() const noexcept;

base::ErrorCode SubmitOrder(
    const model::OrderRequest& request,
    model::StrategyOrderId& out) noexcept;

base::ErrorCode CancelOrder(model::StrategyOrderId id) noexcept;

bool TryGetOrder(
    model::StrategyOrderId id, model::OrderSnapshot& out) const noexcept;
```

上下文通过交易服务访问器返回同一宿主对象：

```cpp
Trading& trading() noexcept;
const Trading& trading() const noexcept;
```

策略提供两个默认返回 `kOk` 的交易回调：

```cpp
base::ErrorCode OnOrderUpdate(
    StrategyContext& context, const model::OrderUpdate& update) noexcept;

base::ErrorCode OnFill(
    StrategyContext& context, const model::Fill& fill) noexcept;
```

`OrderUpdate` 包含当前委托快照、更新类型、关联的撤单请求 ID 和定长原因。更新类型包括受理、拒单、成交、撤单成功、撤单拒绝、新单发送前取消及本地撤单未发送（`CancelNotSent`）。本地撤单未发送结果关联具体尝试，只结束该次请求，不改写订单终态，也不重复处理成交或资源释放。

原因码使用 `int32_t`，说明文字最多 128 字节，超出时保留原因码并标记文字截断。标识和数量不使用截断处理。

`Fill` 包含本地委托 ID、柜台委托 ID、成交 ID、标的、方向、开平意图、成交价格、本笔成交量及本地接收时间。市场事件时间可缺省；存在时保存 UTC 时间及有效标志，不与本地单调时间混用。

服务引用和回调参数仅在当前主线程回调期间借用。查询输出和回调数据均可复制到策略自身存储，不借用柜台 SDK 的缓冲区。

### 本地受理

`SubmitOrder()` 校验生命周期、标的、方向、价格、数量和本地容量，原子地登记 `PendingNew` 委托及待发送请求。成功时填写 `out`，该 ID 立即可查询。`kOk` 表示宿主接收了请求，网关资金检查和预占仍未完成；网关或柜台的受理、拒单由后续回报通知。

本地失败不登记委托、不排入请求、不修改查询视图或 `out`。必需的输出参数使用引用。参数无效或限价金额超出 `int64_t` 返回 `kInvalidArgument`，本地容量或 ID 不足返回 `kResourceExhausted`。资金或可卖持仓不足由网关作为异步拒单回送。

`CancelOrder()` 只接受 `Working` 且没有未决撤单的委托。成功时分配新的撤单请求 ID，登记请求并设置 `cancel_pending`；失败不改变委托或请求队列。未知 ID 返回 `kInvalidArgument`，待受理、终态及重复撤单返回 `kInvalidState`，请求容量或 ID 耗尽返回 `kResourceExhausted`。柜台及本地结果都必须关联具体撤单尝试，迟到结果不能清除后一次撤单的标记。

下单和撤单只复制请求到宿主的有界队列，发送发生在当前回调返回后的主循环中。这两个接口不会在调用栈内触发 `OnOrderUpdate()` 或 `OnFill()`。

`TryGetOrder()` 复制最新本地快照；未知 ID 返回 `false` 且保持输出不变。已终结的委托保留到本次运行结束，不自动淘汰。

### 生命周期限制

| 阶段 | 查询委托 | 策略下单、撤单 |
| --- | --- | --- |
| `OnStart` | 可查询，初始为空 | 返回 `kInvalidState` |
| 正常运行的 `OnTick`、`OnOrderUpdate`、`OnFill`、`OnTimer` | 可查询 | 按本地受理规则执行 |
| 停止排空中的委托、成交回调 | 可查询排空进展 | 返回 `kInvalidState`，撤单由宿主负责 |
| `OnStop` | 可查询最终已知状态 | 返回 `kInvalidState` |

`accepting_orders()` 仅在允许策略发出新委托且配置了交易接入时为 `true`，不代替参数与容量校验。策略在成交回调中准备继续下单时使用它判断是否进入停止阶段。

`OnStart` 可查询初始账户、初始化策略、声明行情订阅及登记定时器；交易请求由进入运行态后的业务回调触发。这样启动失败时仍可直接销毁实例，不会留下启动回调发出的委托。

策略 runner 使用 `strategy::CallbackPhase` 区分回调阶段，只在正常运行的业务回调期间通过 `SetAcceptingRequests(true)` 开放交易请求，回调结束即关闭。停止排空和启动、停止回调期间保持关闭。交易运行时只检查宿主给出的请求权限，不依赖策略回调阶段；`BeginStopping()` 会关闭请求，直至 `Reset()` 才可建立新会话。网关在完成身份、客户端状态和订单归属检查后开放单次请求入口，关闭入口后再推进发送与回报处理。

## 柜台回报与委托账本

### 规范化事件

柜台适配器向宿主提供单一的 `model::TradingEvent`。每条事件包含本地委托 ID、柜台委托 ID、逐委托 `report_sequence`、事件类型、累计成交量、本地接收时间，以及该类型需要的成交或撤单结果字段。

| 事件 | 必要内容与处理 |
| --- | --- |
| 受理 | 绑定柜台委托 ID；尚待受理时进入 `Working` |
| 拒单 | 原因；仅在尚无成交的 `PendingNew` 上进入 `Rejected` |
| 成交 | 唯一成交 ID、价格、本笔数量；更新累计量，进入 `Working` 或 `Filled` |
| 撤单成功 | 本地请求使用对应撤单 ID；柜台主动撤单可为零。最终累计成交量核对后，剩余量归零，进入 `Canceled` |
| 撤单拒绝 | 撤单请求 ID、原因；结束对应请求，保留原委托状态 |

规范化序号由适配器按委托从 1 开始连续递增，原生回报序号不直接作为宿主序号。第一次有效成交可以隐式确认受理，允许 `PendingNew` 直接进入 `Working` 或 `Filled`；随后到达的受理事件仅更新回报序号与接收时间，不重复通知，也不改变已确认的状态。受理和成交事件必须提供非空柜台委托 ID。

非成交事件的累计成交量必须等于账本中的累计量；成交事件必须满足：

```text
新累计成交量 = 原累计成交量 + 本笔成交量
0 < 本笔成交量 <= 原始数量 - 原累计成交量
```

累计量用于核对，本笔唯一成交推进账本一次。适配器若先收到累计量领先的状态回报，须在有界存储中等待对应成交明细，再发布完整事件；不能先发布终态再补入迟到成交。明细缺失无法完成关联时报告接入错误。多笔成交拆成多条事件，只有最后一笔处理完才能确认相应终态。

成交价格必须为正，买入成交价不高于委托限价，卖出成交价不低于委托限价。成交的标的、方向、开平意图及柜台委托 ID 与原单保持一致，价格和数量满足合约步长。回报的本地接收时间不得晚于当前 `Now()`；市场事件时间缺失时清除有效标志并将值置零。

### 校验与幂等

宿主先检查 ID、字段范围和时间域，再处理逐委托序号：

- 旧或重复序号不入账、不回调，仍占事件批次额度；不对旧事件执行依赖当前账本的状态转换校验。
- 新序号必须等于上次序号加一，序号缺口属于协议错误，不能套用完整行情快照允许跳号的规则。
- 新成交的去重键为 `(client_order_id, execution_id)`。同一成交的重传由适配器归一为旧事件；以新序号再次发布已存在的成交 ID 属于协议错误，账本不再次增加数量。
- 柜台委托 ID 绑定后不可改变。新序号的柜台撤单结果必须关联当前未决请求 ID；柜台模式允许没有本地未决请求的主动撤单。旧序号的迟到结果按序号去重，不能改变后一次撤单的标记。
- 本地完成结果按具体请求身份幂等消费，不占用核心的柜台回报序号，也不刷新最后柜台接收时间。其提交推进账本修订号，网关路由正常分配客户端报告序号，策略视图可以据此结清未决请求。
- 累计量倒退、超量成交、终态反转、未知订单、未知事件类型和非法时间戳均拒绝处理，保留当前账本并进入错误收尾。

### 落账与回调顺序

网关先校验事件及存储容量，再一次性提交委托、成交、请求和[持仓与资金变更](#预占与成交)。随后将订单更新及完整账户视图发送给所属策略。策略服务校验并提交本地查询视图后，runner 先调用 `OnOrderUpdate()`，成交事件再调用 `OnFill()`；两者之间不读取下一条交易事件。

进入 `OnOrderUpdate()` 时，本地委托查询与传入快照一致。柜台账户是最近完成查询的整批结果，可能领先该批中的逐笔委托回报；不能据此认定尚未收到终态的订单已经结束。该回调可以登记新的本地请求，随后的 `OnFill()` 委托查询也可包含前一个回调新增的请求状态。

回调失败不回滚已经接受的柜台事实。任一业务回调失败后，停止后续行情、委托和成交回调，宿主继续执行能够完成的撤单与入账；成功启动的实例最终仍调用一次 `OnStop()`。

## 柜台接入

### 宿主接入接口

柜台适配器由 `tradingd` 创建和驱动；策略进程只持有网关客户端。下面的接口属于网关与柜台之间的边界。

`TradingAdapter` 不跨策略动态库边界，关键操作为：

```text
Prepare(config) -> Status
Start(now) -> Status
ReadAccountSnapshot(TradingAccountSnapshot& out) -> Status
ReadBrokerAccountSnapshot(BrokerAccountSnapshot& out) -> Status
OnQuote(quote, now) -> Status
SendOrder(client_order_id, request, now) -> Status
SendCancel(client_order_id, adapter_order_id, cancel_request_id, now) -> Status
Poll(now, model::TradingEvent& out) -> Event / Account / Idle / Error
next_deadline() -> optional<单调时间期限>
is_drained() -> bool
BeginStopping()
Stop() -> Status
```

全部操作由主线程串行调用。`Prepare` 分配或初始化有界存储，`Start` 完成接入初始化。本地账本用 `ReadAccountSnapshot` 复制初始账户；柜台模式用 `ReadBrokerAccountSnapshot` 读取查询快照，保留其实际金额、成本和冻结。发送操作复制请求，不保留输入借用；`Poll` 非阻塞，每次至多返回一条自包含事件。`Account` 表示纯账户更新，不改变事件输出；`Idle` 或错误也保持 `out` 不变。

柜台模式在提交订单事件前校验对应账户版本和运行范围，随后一起提交本地订单与柜台视图；不进入模拟 FIFO 账本。`BeginStopping` 只登记停止状态，常规轮询继续提交最终账户，适配器确认柜台排空后才返回 `is_drained=true`。

`next_deadline()` 表示下一事件的期限，立即就绪时不晚于当前单调时间，无事件时为空；返回 `Idle` 时，下一期限必须为空或晚于传入该次 `Poll()` 的 `now`。`is_drained()` 只有在没有活动委托、未决请求及待发布事件时为真。

调用发送操作前，请求执行器将请求从 `Queued` 推进到 `InFlight`，引擎记录并提交该次尝试，再由执行器调用适配器。调用失败将请求标为 `Uncertain`，保留订单及撤单的未确认事实，进入错误收尾，不自动重发或回退到 `Queued`。`Stop()` 释放接入资源，不等价于柜台撤单。

`OnQuote` 在报价通过宿主校验、最新行情及期货估值更新后调用，早于策略 `OnTick`；它只计划交易回报，不直接调用策略。默认实现无操作，期货模拟交易所据此更新外部盘口及撮合。

### 模拟回报生成规则

`MockTradingAdapter` 使用单调时间定时发布回报，成交价取委托限价，行情不参与撮合。每一步发布后，以本次 `Poll(now)` 的 `now + interval` 安排下一步；不补发一串落后于当前时间的阶段。相同期限按计划登记顺序发布，使测试结果可重复。

| 模式 | 下单后的事件 |
| --- | --- |
| `fill` | 一个间隔后受理；再一个间隔成交 `floor(quantity / 2)`；再一个间隔成交剩余量。数量为 1 时，受理后的下一个间隔直接全部成交 |
| `working` | 一个间隔后受理，随后保持挂单直到撤单 |
| `reject` | 一个间隔后拒绝新委托，不产生成交 |

每张订单在 `fill` 模式最多产生两笔成交。取得撤单请求后，柜台安排一个间隔后的撤单结果；此期限前到期的成交仍正常发布。撤单生效时若仍有剩余量，撤销剩余量并移除后续成交计划；若已经全部成交，则返回该次撤单的拒绝结果。

模拟柜台的挂单、事件计划与 ID 均自持存储，定时调度采用预分配的有界结构，不在轮询路径扩容或执行无界清扫。故障测试通过注入测试适配器制造拒撤、重复回报和乱序，不将这些异常塞入正常配置。

## 主循环与收尾

策略进程驱动行情和交易网关客户端；网关驱动共享账户和柜台。两侧通过 `Stop` / `Drained` 协调收尾，详见[网关停止协议](tradingd.md#资源与故障边界)。

### 启动与事件调度

装配层依次校验策略、行情和网关客户端配置。runner 准备行情源，通过 `TradingService::Init()` 连接网关并完成身份、标的和账户同步，成功后才加载和初始化策略。`OnStart()` 成功后冻结订阅并启动行情源，全部成功后才允许交易请求。配置装配阶段检查停止信号；`LoadAndStart()` 不处理退出请求，成功后由 `Run()` 在业务分派前统一检查。

主循环先处理最多 64 个客户端待发送请求，再处理一批最多 64 个行情、交易或策略定时事件。三个来源通过持久轮转位置公平调度；旧行情、重复回报和客户端发送前取消同样占事件额度。回调中新建的请求在回调返回后的下一轮发送，不递归执行。网关按客户端限制请求处理额度，并分批消费柜台回报，提交权威账本后发布事件。

客户端轮询和行情维护均不等待新数据。没有立即工作时，以行情、维护、交易客户端和排空期限的最小值等待；每批结束及等待返回后检查停止信号。账户推送可独立于本策略订单到达，策略不能用本地订单缓存重建共享账户余额。

策略 runner 在 `Run()` 中通过 `EventLoopEnvironment::CheckStop()` 接收停止请求并完成排空与释放；重复检查到停止请求不延长期限。调用方应在 `Run()` 返回后再销毁 runner，析构不另建排空循环。

### 正常停止

1. 关闭策略请求入口，同步停止行情源，保留最后行情快照。
2. 客户端将尚未尝试发送的委托转换为发送前取消事件；已尝试发送的请求保持未确认事实，并向网关发送 `Stop`。
3. 网关为该客户端的未终结委托安排撤单，处理成交、拒撤和撤单结果；其他客户端继续运行。撤单重试与客户端归属由网关管理。
4. 策略服务持续提交回报及账户视图，runner 可在排空期间调用委托和成交回调，但请求入口保持关闭。
5. 所有本地请求处理完、委托状态终结并收到网关 `Drained` 确认后，调用一次 `OnStop()`，销毁策略并关闭客户端。

`OnStop()` 可查询最终已知视图。关闭客户端或适配器均不等价于柜台撤单；超时与断线不能把未确认订单改写为已撤销。

### 错误与超时

策略业务回调失败时关闭后续业务回调，仍由网关推进撤单，客户端接收回报，最终配对 `OnStop()`。柜台拒单与拒撤是正常业务结果，不直接导致进程失败。

排空截止时间从首次进入 `Stopping` 时开始，后续信号和重试不延长它。在循环入口和事件批次边界检查期限；尚未排空且到期时，保留未确认委托的 ID、状态、累计成交量、剩余量及撤单尝试，输出诊断并以失败状态退出，不把超时订单伪造为已撤销。检查不抢占用户回调，回调仍须及时返回。

接入、回报协议或等待设施错误使排空无法继续时，记录未应用事件及未确认请求，完成能够安全执行的释放后失败退出。调用接入发送操作前即标记“已尝试发送”，其失败也不能走发送前取消路径。始终保留首先发生的错误，关闭成功不能覆盖它。

启动阶段禁止策略交易，因此启动失败时没有需要撤销的策略委托；交易接入启动或初始账户失败不调用策略，`OnStart` 成功后的行情启动失败仍须配对 `OnStop`。

## 配置与示例策略

### 网关客户端配置

`--trading-config <path>` 是策略进程的必填参数。`TradingClientConfigLoader` 只接受 TCP 网关连接、客户端身份和超时配置，拒绝模拟柜台及初始账户字段；缺少参数返回用法错误，连接或握手失败在创建策略前退出。客户端格式见[网关配置与运行](tradingd.md#配置与运行)。

### 柜台配置

交易配置通过公共 `trading::TradingConfigLoader` 加载，并以 `trading::ValidateTradingConfig()` 校验类型化结果。模拟柜台字段如下，全部必填且拒绝未知字段：

```json
{
  "adapter": "mock",
  "mode": "fill",
  "report_interval_ms": 1,
  "shutdown_timeout_ms": 5000,
  "initial_cash": 100000000000000,
  "initial_positions": [
    {"venue": "SSE", "symbol": "600000", "total_qty": 1000, "available_qty": 800}
  ]
}
```

上例 `adapter` 为 `mock`，期货模拟使用 `simulated_futures`，见[期货运行与配置](domestic-futures.md#运行与配置)；`mode` 为 `fill`、`working` 或 `reject`；两个时间字段均为 `1..60000` 的整数。`initial_cash` 为非负 `int64_t` 微元，上例为 1 亿元。`initial_positions` 可为空，最多 64 项；条目只接受上例中的四个字段，场所、代码、数量及重复标的按[初始账户规则](#初始账户与计量)校验。文件读取、JSON 完整校验和字段诊断复用现有配置基础设施，配置错误在加载策略动态库前拒绝。远端模式选择 `adapter: "tcp"`，只配置连接身份和超时，不在策略配置中提供账户余额，见[网关客户端配置](tradingd.md#配置与运行)。

### 示例与预期轨迹

示例 [order_lifecycle_strategy.cc](../../examples/strategies/order_lifecycle_strategy.cc) 通过自身的[派生配置](../../examples/strategies/order_lifecycle_strategy.json)选择场所、代码和下单数量，行情来源由宿主配置绑定。在 `OnStart` 解析标的、订阅并查询初始账户，在第一条有效报价中按卖一价提交一次买入限价委托，随后只观察该委托，不追加交易。[模拟柜台配置](../../examples/tradingd/mock_trading.json)默认使用 `fill` 模式；完整运行命令见 [README](../../README.md#策略进程)。

```text
OnTick(context, tick):
    if 已提交过委托或 context.trading().accepting_orders() 为 false:
        return 成功
    if 卖侧不存在或不可交易:
        return 成功
    result = context.trading().SubmitOrder(
        买入本标的、价格 tick.ask_price、数量 config.quantity, order_id)
    if result 失败:
        return result
    保存 order_id，标记已提交
    return 成功

OnOrderUpdate(context, update):
    查询 order_id，检查快照与 update.order 一致
    查询账户，检查剩余预占与持仓已更新
    return 成功

OnFill(context, fill):
    记录本笔成交，检查查询累计量已包含该笔成交
    按实际成交额核对现金扣减和持仓增加
    return 成功
```

`fill` 模式、数量 200 时，预期正常轨迹为：

| 阶段 | 状态 | 累计成交 | 剩余量 | 回调 |
| --- | --- | --- | --- | --- |
| 本地受理 | `PendingNew` | 0 | 200 | 下单返回，可立即查询 |
| 柜台受理 | `Working` | 0 | 200 | `OnOrderUpdate` |
| 首笔成交 | `Working` | 100 | 100 | `OnOrderUpdate → OnFill` |
| 第二笔成交 | `Filled` | 200 | 0 | `OnOrderUpdate → OnFill` |
| 收到退出信号 | `Filled` | 200 | 0 | 排空后 `OnStop` |

委托成交后进程继续等待行情或停止信号，结束由宿主生命周期决定。

示例使用 `order_lifecycle: ready`、`order_lifecycle: submitted` 和 `order_lifecycle: working` 标记初始化、提交与柜台受理。数量 200 全部成交后输出 `order_lifecycle: terminal state=filled cumulative=200 leaves=0`，最终停止汇总为 `order_lifecycle: stopped fills=2 filled_qty=200 state=filled`。`working` 模式下，等到受理标记后停止，网关撤销挂单，最终状态为 `Canceled`。

示例还在初始化、成交和停止时输出 `order_lifecycle: account`，其中资金为微元、持仓为股；用于观测网关发布的账户变化，账户中可包含其他策略的交易。

## 容量与实现位置

持仓槽最多 64 项，与本进程运行标的上限一致。委托账本初始上限为 1024 张，唯一成交记录上限为 8192 条，待发送请求和本地待更新队列各为 256 条。存储在准备阶段建立，运行期不扩容；终态委托和成交去重记录保留至实例销毁，达到委托上限后拒绝新单。

股票脚本模拟单最多产生两笔成交，成交记录容量可覆盖全部受理订单。新的事件在校验和容量检查完成前不修改账本；异常事件导致容量耗尽时保留未应用事件的诊断并进入错误收尾。ID、回报序号和时间计算均检查溢出。

| 位置 | 职责 |
| --- | --- |
| `include/yquant/model/identifiers.h` | 本地委托、柜台委托、成交与撤单请求标识 |
| `include/yquant/model/account.h` | 资金和持仓快照 |
| `include/yquant/model/order.h` | 委托请求、快照与状态 |
| `include/yquant/model/trading_event.h` | 规范化交易事件、委托更新与成交记录 |
| `include/yquant/strategy/trading.h` | 策略交易接口 |
| `include/yquant/strategy/strategy_context.h` | 交易服务访问器 |
| `include/yquant/strategy/strategy.h` | 委托和成交回调 |
| `include/yquant/trading/trading_account.h` | 柜台初始账户快照 |
| `include/yquant/trading/trading_adapter.h` | 柜台接入与事件轮询接口 |
| `include/yquant/trading/trading_service.h`、`trading_dispatch.h` | 统一交易服务接口与已提交事件 |
| `include/yquant/trading/trading_engine.h`、`src/trading/trading_engine.cc` | 网关交易会话、账本、预占、请求与回报处理及停止排空 |
| `tests/mock_trading_service.h` | 记录请求并按脚本提交回报的单元测试服务 |
| `include/yquant/trading/trading_limits.h` | 交易记录和请求的容量上限 |
| `include/yquant/trading/mock_trading_adapter.h`、`src/trading/mock_trading_adapter.cc` | 定时模拟回报 |
| `include/yquant/trading/trading_config.h`、`src/trading/trading_config.cc` | 网关柜台配置加载与校验 |
| `include/yquant/trading/trading_client_config.h`、`src/trading/trading_client_config.cc` | 策略网关连接配置加载与校验 |
| `src/strategyd/trading_service_impl.h/.cc` | 策略侧远端交易请求与只读账户视图 |
| `src/strategyd/strategy_runner.h/.cc` | 行情、交易和定时事件调度、停止排空与最终释放 |
| `examples/strategies/order_lifecycle_strategy.cc` | 单笔委托闭环示例及配套 JSON |
| `tests/trading/` | 公共交易配置、账本、模拟柜台、协议及连接测试 |

公共接口签名或布局变化时更新 `kStrategyPluginAbiVersion`，宿主、示例与测试动态库统一重编译，不保留旧接口转发。

## 验证标准

| 场景 | 验收条件 |
| --- | --- |
| 初始账户 | `OnStart` 可查询初始快照和零持仓；未知、重复标的、非法数量或资金拒绝启动 |
| 资源预占 | 多笔买单共用现金、多笔卖单共用可卖量；不足或溢出时账本、队列和输出保持不变 |
| 成交落账 | 买入扣实付金额、释放价差且不增加可卖量；卖出消耗预占并增加现金；两个交易回调均可见新账本 |
| 预占释放 | 部分成交后撤单、拒单、发送前取消释放剩余；拒撤、发送不明及超时保持预占 |
| 本地受理 | 成功后立即可查 `PendingNew`；无同步回调；参数、阶段或容量失败保持账本、队列和输出不变 |
| 委托标识 | 本地 ID 不复用；柜台 ID 延迟绑定且保持一致；撤单结果关联正确尝试 |
| 正常成交 | 200 股依次受理、成交 100、成交 100；累计量、剩余量及双回调顺序正确 |
| 最小数量 | 数量 1 不生成零数量部分成交，直接全部成交 |
| 回调查询 | 更新回调入口的查询与参数一致；成交回调查询包含该笔成交；前一回调登记撤单后仍正确可见 |
| 拒单与撤单 | 拒单终结新委托；撤单在途不减少剩余量；部分成交后撤单正确保留成交量；拒撤不错误终结原单 |
| 成交竞争 | 首次成交可先于受理；迟到受理不重复通知或恢复终态；全成时未发撤单在本地结束且不发送，已发撤单等待匹配结果；本地结束正常清除未决标记并通知 |
| 回报幂等 | 旧与重复事件不再入账或回调；新序号重复成交 ID、序号缺口、累计量矛盾及终态反转被拒绝 |
| 公平调度 | 行情、交易和定时来源持续就绪时轮换处理，每批合计不超过 64 个事件；重复数据占额度；请求发送与发送前取消都受处理额度限制 |
| 独立唤醒 | 行情间隔很长时，交易回报仍按自身期限推进；无立即工作时等待最早期限 |
| 正常排空 | 未发送委托超过单轮扫描额度时仍不送柜台；待受理单、部分成交单及在途撤单均被处理；本地更新计入事件额度且清空后才 `OnStop` |
| 超时与错误 | 拒撤重试有间隔，等待受理和撤单时不空转；停止期限不被延长；发送失败不重发，接入错误和排空超时保留未确认状态并失败退出 |
| 回调失败 | 已确认事件不回滚；停止后续业务回调，宿主收尾，成功启动的实例仍执行一次 `OnStop` |
| 存储生命周期 | 撤单与回报不借用 SDK 缓冲区；停止回调可查询最终已知状态；ASan/UBSan 检查对象与动态库生命周期 |
| 内存活动 | 对请求登记、事件入账、查询和派发检查运行期 C++ 堆活动，预分配完成后不申请、释放或扩容 |
| 进程闭环 | 真实 `yquant-strategyd` 加载示例动态库，连接真实 tradingd，持续运行并在信号后排空退出；缺少交易配置时拒绝启动 |

组件测试注入时钟和规范化事件；主循环单元测试通过 `Run()` 驱动 `MockTradingService`，交易闭环测试启动真实 `tradingd`；进程测试使用真实动态库和信号握手，覆盖正常、拒单、撤单与停止排空。仅在协议边界注入故障，不公开宿主内部事件分派方法来适配测试。
