# Trading 设计草案

Trading 提供订单处理与账户状态查询能力。一个 `TradingService` 管理多个账户，每个请求指定账户；多个策略可以通过同一账户交易。

线程与部署模型遵循[系统架构](architecture.md)。本设计覆盖 N2 的本地交易路径，并定义模拟场所、SimNow 接入及跨进程调用的扩展位置。

## 类与职责

### 调用与业务处理

| 类 | 职责 |
| --- | --- |
| `OrderIdGenerator` | 由 caller service 持有，为该 service 发起的所有新订单生成统一 `OrderId`。 |
| `TradingClient` | 调用方使用的交易接口，提供报单、撤单和同步查询。 |
| `TradingListener` | 接收请求失败、委托、撤单和成交回报；回报携带原始请求的账户与归属。 |
| `LocalTradingClient` | 直接调用本地 `TradingService`，查询 service 中的订单和账户状态。 |
| `TradingServiceRegistry` | 保存本地 service ID 到 `TradingService*` 的注册关系。 |
| `TradingClientFactory` | 使用注入的实例表创建 client；跨进程阶段再接入部署拓扑。 |
| `TradingService` | 按账户选择接入对象，通过接入回调更新订单和账户状态，向调用方交付回报；`Run()` 推进需要延迟执行的工作。 |

`main` 创建具体对象、绑定账户与接入对象、建立 service 注册关系，再向 factory 注入实例表。container 只调用 `Service::Run()`，业务实例寻址由对应实例表负责。

### 柜台接入与模拟场所

| 类 | 类型 | 职责 |
| --- | --- | --- |
| `ITradingAdapter` | 接口类 | 一个账户的柜台接入接口，发送交易请求，通过注册的 Callback 交付回报。 |
| `ITradingAdapterCallback` | 回调接口类 | 以只读引用接收委托、撤单和成交回报，由账户上下文转交 service 处理。 |
| `SimulatedTradingAdapter` | 接口实现 | 保存一个模拟账户的委托，处理报撤单，按验证场景产生交易事件并控制交付时机。 |
| `SimNowTradingAdapter` | 接口实现 | 绑定一个实际交易账户，负责接入库调用、柜台标识关联和回调线程到业务线程的数据交接。 |

`SimulatedTradingAdapter` 和 `SimNowTradingAdapter` 实现 `ITradingAdapter`。`main` 为每个账户创建对应的接入实例，在运行前绑定到 `TradingService`。模拟接入实例保存本账户的委托；需要延迟交付的回报由该实例保留到交付时。

### Service 内部对象

| 结构 | 保存的内容 |
| --- | --- |
| `AccountContext` | 账户标识、借用的 adapter 指针及账户状态；实现 `ITradingAdapterCallback`，将回报连同绑定账户交给 service。 |
| `OrderRecord` | 统一 `OrderId`、原始订单参数、账户与策略归属、订单状态、成交数量、调用关联信息及回报接收方。 |
| `CancelRecord` | 撤单请求标识、目标订单、撤单处理状态、调用关联信息及该请求的回报接收方。 |

这些结构由 `TradingService` 管理。订单数量和状态由同一份 `OrderRecord` 提供给查询与回报处理。`OrderRecord` 保存可变的当前状态；`OrderSnapshot` 在查询或通知时复制生成，不作为内部状态存储。

## 标识与数据契约

YQuant 自行生成的序号和请求标识（如 `sequence`、`cancel_request_id`、`service_cancel_id` 和 `call_id`），当前在各自作用域内保证单次运行期间不重复。后续持久化标识生成状态，并明确作用域标识在重启后的延续规则，使同一作用域在进程重启后也不复用已分配的标识。正常退出和异常退出均需满足这一要求，具体机制在后续设计中细化。

### 订单标识

YQuant 内部使用 caller 生成的统一 `OrderId`。该标识在 client、service、adapter 和回报之间保持不变。

```text
OrderId = (caller_service_id, sequence)
```

| 标识 | 含义与作用范围 |
| --- | --- |
| `caller_service_id` | 生成订单标识的 caller service ID，在部署中全局唯一。 |
| `sequence` | caller service 在本次运行期间生成的单调递增序号。 |
| `order_id` | 由上述两字段组成，在首版单次运行范围内唯一。 |
| `account_id` | 目标 TradingService 管理的账户，由组装层绑定到接入对象。 |
| `strategy_id` | 在调用方 service 内标识策略实例。 |
| `counter_order_id` | 柜台订单标识，类型名为 `CounterOrderId`。由具体 adapter 处理完整的账户和会话作用范围，并与统一 `OrderId` 关联。 |

一个 caller service 持有一个 `OrderIdGenerator`，所有策略、账户、client 及目标 TradingService 共用它分配序号。生成器在调用方业务线程内使用，每次新报单消耗一个序号；序号不因发送失败、切换账户或切换策略而复用。序号耗尽时生成失败，不允许回绕。

```text
caller service 100：生成 (100, 1)、(100, 2)、(100, 3)
caller service 200：生成 (200, 1)、(200, 2)、(200, 3)
```

不同 caller 可以独立使用相同序号，完整 `OrderId` 仍然不同。账户和策略是订单归属字段，不参与 `OrderId` 的生成；调用方 service ID 直接从 `OrderId` 取得。

每个 TradingService 对自己接收的完整 `OrderId` 查重。同一 ID 换账户或策略再次提交，也不能建立第二笔订单或覆盖原记录。跨目标 TradingService 的 ID 唯一性由 caller 的共享生成器保证。

caller 重启后，存活的 TradingService 仍按本次运行已保留的订单查重。

adapter 接收报单时即取得 `OrderId`，并建立与柜台请求及 `CounterOrderId` 的关联。即使柜台尚未分配 `CounterOrderId`，早期回报也必须能携带原始 `OrderId`。`CounterOrderId` 的底层表示与长度由接入实现处理。

每次撤单还带有独立的 `cancel_request_id`，在目标 `OrderId` 内唯一。TradingService 为 adapter 调用分配 `service_cancel_id`，用于关联多次撤单尝试的结果。

每次报单或撤单调用还生成内部请求关联标识 `call_id`，在该 client 实例内唯一。client 实例标识与 `call_id` 随内部调用上下文传递，保存在订单记录和内部通知中；跨进程时随响应封装传回，用于定位本次调用的 listener。即使两次提交使用同一 `OrderId`，也能把重复请求的失败交付给第二次调用。该关联标识由 client 管理，不放入业务回报；调用方使用统一的订单标识和自己的撤单请求标识。

### 成交标识

`TradeId` 由 `venue`、`session`、`value` 三个字符串组成：分别标识柜台市场、成交编号的复用作用域（例如交易日）和柜台成交编号。三个字段均非空，保留完整内容；实际接入负责将柜台的唯一性字段无损映射到这些字段。账户作为外层作用域，各账户独立维护成交哈希表；同一市场在不同作用域中复用编号属于不同成交。

### 请求与回报数据

领域模型独立于服务请求：`OrderInfo` 和 `OrderSnapshot` 定义在 `order.h`，`TradeId` 和 `TradeInfo` 定义在 `trade.h`，请求及业务回报数据定义在 `trading_service.h`。

- `OrderInfo` 保存 `order_id`、`account_id`、`strategy_id` 和合约、方向、开平、价格、数量等原始委托参数。
- `SubmitOrderRequest` 通过 `order` 字段携带一份 `OrderInfo`。
- `CancelOrderRequest` 包含目标 `order_id`、账户、策略及调用方撤单请求标识。
- `QueryOrderRequest` 包含目标 `order_id`、账户和策略，用于定位订单并核对归属。
- `OrderSnapshot` 通过 `order` 字段保存一份 `OrderInfo`，另含当前状态、累计成交数量和单调递增的状态版本，不保存服务请求对象。
- `RequestFailure` 表达某次已交付请求的处理失败，包含 `order_id`、错误码 `reason` 和可选的订单快照 `snapshot`。校验失败没有快照；已建立订单或已通过撤单校验后的发送异常携带订单快照。撤单失败另带本次 `cancel_request_id`，见撤单处理规则。
- `OrderReport` 通知订单状态的变化，例如委托接受、委托拒绝和订单撤销。
- `CancelReport` 表达某次撤单的结果，并关联原订单及撤单请求。
- `TradeInfo` 保存原始订单 ID、完整成交标识、成交价格和数量；账户来自 adapter 的绑定上下文。
- `TradeReport` 通过 `trade` 携带一份成交明细，通过 `snapshot` 携带合并后的订单快照。

请求参数只借用到调用返回；接收方复制后续处理所需的数据。回报参数只借用到回调返回，调用方需要长期保留时复制快照。

业务回报保留原始 `OrderId`，并与本次处理产生的订单快照一同交付，远端 client 应用 service 计算的状态。重复 `OrderId` 等请求校验失败只产生本次调用的失败回报，不修改已有订单，也不携带用于覆盖原订单的状态快照。

### 接入异常处理

adapter 负责外部柜台的数据转换和标识关联，Trading 负责校验接入事件并执行相应的业务处理。处理规则覆盖请求发送、订单及成交回报、账户快照和连接状态；每类异常明确状态变化、业务通知、查询可用性与后续处理，并通过可控接入事件验证。具体场景随 [N2 各功能节点](../plans/v1.md#n2单进程交易与账户状态)细化，N5 补齐实际柜台的转换规则。

Trading 保留已经确认的订单事实。发送成功且尚无有效柜台回报时，订单保持 `kSent`；回报缺失或连接中断不能直接认定委托被拒绝、撤单成功或订单已经结束，也不能自动重报已经发出的请求。发送返回与后续回报不一致时，按有效回报核实实际委托结果，异常处理见报单处理规则。

无法关联、账户不符、字段非法或内容矛盾的事件，需要有明确的处理与诊断，不能误关联或污染已有订单。接入可用性按账户维护；同一次运行中重新连接后，核实未确认订单并刷新账户状态，再恢复相应可用性。一个账户的接入异常不改变其他账户的有效状态。

### 报单处理规则

`sequence` 从 1 开始；合约标识和数量必须为正，价格必须为有限正数，方向和开平值必须属于接口枚举。账户与策略标识按原值保存，不依赖全局递增顺序。

| 场景 | 已知事实与状态 | 业务通知与后续处理 |
| --- | --- | --- |
| 未绑定账户、重复完整 ID、非法参数 | 本次请求未建立订单，已有订单保持原值 | 请求已交付后，在调度阶段向本次调用的 listener 交付校验失败，不带订单快照。 |
| 确定未发送，且没有有效柜台回报 | 已建立记录转为发送失败 | 交付发送阶段的请求失败及快照，保留记录供后续查询。 |
| 已发送，尚无柜台回报 | 保存已发送状态，接受或拒绝尚待回报 | 不以发送成功替代委托回报；后续回报使用原始 ID。 |
| 发送失败后迟到有效委托回报 | 柜台事实修正此前发送判断，更新为接受或拒绝 | 诊断发送判断与后续回报不一致，保留早先失败通知的快照，再按顺序交付新的委托回报。 |
| 接受或拒绝回报重复 | 已确认状态不变，版本不增长 | 不重复通知。 |
| 未知 ID、错误账户、非法回报状态或接受与拒绝互相矛盾 | 不应用异常事件，保留已确认状态 | 向 stderr 诊断；不伪造订单结果，等待有效信息。账户断连、可用性与恢复核实在 N2.10 扩展。 |

柜台接受或拒绝由 `OnOrderReport` 表达。发送失败后若迟到有效柜台回报，诊断发送判断与回报不一致，再按柜台事实更新订单；已交付失败通知中的快照保持不变。

每次对外状态或累计成交量变化递增版本，重复与无效事件不递增。延迟通知保存当时快照；后续有效回报可以产生更高版本。查询读取同一订单记录；发送失败及已发送记录均保留，校验失败不会创建可查询记录。

当前报单实现预留哈希桶，以完整 ID 定位订单；待通知队列使用连续存储的循环队列，交付后立即复用槽位，避免调度时搬移剩余通知。新增订单仍需分配哈希节点，队列满时扩容；内存总预算、分配与容量不足处理由 N2.9 完成。

### 撤单处理规则

撤单请求携带原始 `OrderId`、账户、策略和非零 `cancel_request_id`。服务先核对订单及归属，再检查订单可撤销性及撤单 ID；同一订单已经建立的撤单 ID 不复用，包括发送失败的尝试。订单处于 `kSent`、`kAccepted` 或 `kPartiallyFilled` 时允许尝试撤单；发送撤单不改变订单状态。

每次已建立的尝试分配本服务内单调递增的 `service_cancel_id`，耗尽时失败且不回绕。adapter 按绑定账户、原始订单 ID 及该服务撤单 ID 发送请求，回报完整保留二者。服务按服务撤单 ID 定位尝试，再核对来源账户与订单，避免迟到回报完成其他尝试。

| 场景 | 订单与撤单记录 | 通知与后续处理 |
| --- | --- | --- |
| 未知订单、归属不符、订单不可撤、重复撤单 ID 或非法 ID | 不建立本次撤单记录，不改变订单 | 向本次调用的 listener 交付 `RequestFailure`，带订单 ID 和本次撤单 ID，不带快照。 |
| 服务撤单序号耗尽 | 不调用 adapter，不建立尝试 | 请求已经交付，后续通知 `kIdExhausted`。 |
| 确定未发送 | 保留本次尝试与原订单状态 | 交付带当前订单快照的发送失败通知；调用方下次尝试使用新撤单 ID。 |
| 已发送 | 本次尝试等待柜台结果，订单状态保持原值 | 请求返回前不通知业务 listener，后续通过 `OnCancelReport` 交付结果。 |
| 柜台拒绝 | 完成本次尝试，保留当前订单状态 | 向撤单 listener 交付拒绝结果及当前快照。 |
| 柜台确认撤销 | 完成本次尝试，将活动订单更新为 `kCanceled`，保留成交数量 | 先向原订单 listener 交付状态变化，再向本次撤单 listener 交付成功结果，两份通知使用本次事件的独立快照。 |
| 撤单结果前已全部成交 | 完成本次撤单尝试，保留 `kFilled` 与成交数量 | 撤单 listener 仍收到本次结果，不重复通知订单状态。 |
| 发送失败后迟到有效回报 | 诊断发送判断与回报不一致，再按有效回报处理本次尝试 | 已通知的失败快照不变，后续结果仍关联原撤单 ID，不自动重报。 |
| 重复、冲突或错误关联的回报 | 相同结果不重复通知；冲突结果、未知尝试、账户或订单不符及非法枚举均不应用 | 输出诊断，保留已有订单事实。另一次尝试已经撤销订单时，本次拒绝只结束自身尝试，不回退订单状态。 |

`RequestFailure` 使用可选的 `cancel_request_id`：报单失败为空，撤单失败携带本次请求 ID。撤单发送失败的快照反映订单本身；撤单尝试的结果由撤单 ID 和错误码表达，不将订单标记为报单发送失败。柜台撤单回报通过 `CancelReport` 携带原始订单 ID、调用方撤单 ID、结果及订单快照。

adapter 的 `CancelOrder` 与 `SubmitOrder` 都不得在调用期间触发 Callback。模拟接入保存委托状态，撤单结果在后续 `Run()` 中交付，并按同一个批量上限处理委托与撤单回报。各项记录与索引的容量控制沿用 N2.9 的统一预算方案。

## 接口

### TradingClient

关键签名：

```cpp
RequestResult SubmitOrder(const SubmitOrderRequest& request,
                          TradingListener& listener);
RequestResult CancelOrder(const CancelOrderRequest& request,
                          TradingListener& listener);
bool QueryOrder(const QueryOrderRequest& query, OrderSnapshot& snapshot) const;
```

`RequestResult` 表达请求是否进入当前调用路径：

- 未交付：调用方立即得到失败原因，本次请求不再产生回调。当前本地 client 在实例标识或调用序号耗尽时返回 `kIdExhausted`，不复用旧标识。
- 已交付（`kDelivered`）：后续处理结果通过回报交付，包括服务校验失败、柜台发送失败、柜台接受或拒绝。

本地路径将请求交给 service，跨进程路径将请求交给消息通道；成功返回均不代表柜台已经接受委托。服务侧处理失败由错误码表达原因；柜台拒绝通过委托回报交付。

`SubmitOrder` 和 `CancelOrder` 返回前不调用业务 listener。adapter 的 `SubmitOrder` 调用期间不得触发任何 Callback；柜台 SDK 在调用期间产生的回报先保存，由后续 `Run()` 交付。service 将校验失败和发送失败的通知排队，由请求返回后的调度阶段交付。同一订单已有待通知回报时，后续回报也排队，保持该订单的通知顺序；其余业务线程回报直接处理并通知 listener。

`QueryOrder` 按 `OrderId` 定位订单并核对请求中的账户和策略。记录可用且归属匹配时返回 true 并写入快照；否则返回 false，保持输出参数不变。本地 client 读取 service，远端 client 读取已经建立的本地缓存。

### TradingListener

```cpp
void OnRequestFailure(const RequestFailure& failure);
void OnOrderReport(const OrderReport& report);
void OnCancelReport(const CancelReport& report);
void OnTradeReport(const TradeReport& report);
```

各次请求失败交付给对应调用的 listener。`SubmitOrder` 的 listener 接收该订单的委托和成交回报；`CancelOrder` 的 listener 接收该次撤单请求的结果。撤单导致订单状态变化时，订单 listener 同时收到相应的订单回报。

服务校验失败或确定未发送通过 `OnRequestFailure` 通知；柜台接受或拒绝委托通过 `OnOrderReport` 通知。已经建立订单后确定发送失败时，失败回报同时携带对应状态快照，供 client 更新查询视图。账户可用性的通知与查询表达随对应功能节点细化。

回调前先更新订单状态。回调中可以提交新请求或查询订单；请求调用、回报交付或 `TradingService::Run()` 正在执行时，再次调用 `Run()` 直接返回。当前回调使用独立快照，回调内操作其他请求不会改变正在读取的回报内容。查询读取最近已处理的状态，可能比延迟交付的回报快照更新。

当前静态组装下，listener 的生命周期覆盖 service 的整个运行期。订单进入终态后仍可能交付此前成交的明细，listener 不以某一条终态回报作为销毁依据。

### TradingService 与账户绑定

```cpp
bool BindAccount(AccountId account_id, ITradingAdapter& adapter);
void Run();
```

`BindAccount` 在初始化阶段建立账户关系，重复账户标识返回失败；同一 adapter 实例只能绑定一个账户。service 创建具有稳定地址的 `AccountContext`，再将其注册为该 adapter 的 Callback。账户归属来自绑定的上下文，接入回调无需通过额外查询确定账户。

service 借用 adapter。`AccountContext` 的生命周期覆盖所有接入回调；组装层先停止交易请求与调度，并确保接入对象不再调用 Callback，再销毁 service 和接入对象。

service 的报单、撤单及查询入口供本地 client 和后续的 server 适配器使用，调用方通过 `TradingClient` 访问业务能力。

### ITradingAdapter

接口类只声明 `virtual` 函数，具体行为由接入实现提供。关键签名：

```cpp
virtual void SetCallback(ITradingAdapterCallback& callback) = 0;
virtual SendResult SubmitOrder(const SubmitOrderRequest& request) = 0;
virtual SendResult CancelOrder(const AdapterCancelOrderRequest& request) = 0;
virtual void Run() = 0;
```

报单路径复用 `SubmitOrderRequest`，带有统一 `OrderId`、归属和委托参数；`AdapterCancelOrderRequest` 带有目标 `OrderId` 和服务撤单标识。账户由 adapter 实例的绑定关系确定，adapter 事件保留原始 `OrderId`。

`SetCallback` 在初始化阶段注册借用的回调对象。`SendResult` 只包含已发送（`kSent`）和确定未发送（`kNotSent`）；具体柜台返回值的转换规则在实际接入时细化。确定未发送时由 service 产生请求失败回报；已发送时等待柜台回报。发送返回与回调不一致时，遵循接入异常处理规则。

`Run()` 在当前业务线程推进接入对象内部的延迟工作。接入实现使用内部批量上限控制每次处理量，当前没有工作或达到上限时返回；无效或重复输入也计入处理量。`SubmitOrder` 调用期间不得触发 Callback；即使柜台 SDK 同步产生回报，也必须先保存在接入对象中，待 `SubmitOrder` 返回后由 `Run()` 交付。

### ITradingAdapterCallback

```cpp
virtual void OnOrderReport(const AdapterOrderReport& report) = 0;
virtual void OnCancelReport(const AdapterCancelReport& report) = 0;
virtual void OnTradeReport(const TradeInfo& trade) = 0;
```

回报保留统一 `OrderId`，撤单回报还携带对应的服务撤单标识。所有 Callback 在 service 所属的业务线程调用。回报数据只在回调执行期间有效；adapter 在回调返回后才能复用相应缓冲。

接入状态变化和无法关联到具体订单的异常也需要传给 Trading，对应回调数据与接口随 N2.10 细化。

报单调用返回后，在业务线程产生的回报可以直接通过 `const&` 交付。service 在回调中更新需要长期保存的状态，不要求 adapter 先写入统一事件对象再复制给使用方。

来自其他线程的 SDK 回调在接入边界保存数据，由 adapter 的 `Run()` 在业务线程交付。需要格式转换时，尽量直接写入最终交接存储；交付时借用存储中的回报，Callback 返回后再释放或复用该空间。

## 关键运行流程

### 报单

1. caller 使用所属 service 的共享生成器取得 `OrderId`，将其连同账户、策略及订单参数交给 client。
2. client 将请求交付给 service；service 检查账户绑定及 `OrderId` 是否重复，保存订单信息和 listener，按原始 `OrderId` 建立订单索引。
3. service 调用对应账户的 adapter 发送请求，调用期间不发生 Callback。adapter 返回已发送时将订单更新为 `kSent`；返回确定未发送时更新为 `kSendFailed`，生成失败通知和订单快照，排队等待调度交付。
4. adapter 通过 Callback 将回报交给对应 `AccountContext`；service 核对账户和订单、更新状态，再按回调时机与订单通知顺序直接交付或保存通知。没有建立订单的校验失败只交付请求结果。

调用 adapter 前建立订单记录；adapter 返回后根据发送结果更新当前状态，后续回报通过原始 `OrderId` 定位订单。非法账户或重复 `OrderId` 的请求失败结果归属于本次调用，不修改原有订单。

### 撤单

1. 按 `OrderId` 定位原订单，核对请求的账户和策略归属，检查撤单请求标识，建立 `CancelRecord`。
2. 使用原订单绑定的账户 adapter 及原始 `OrderId` 发送撤单。
3. 接入回调交付撤单结果时，先关联本次撤单请求，再更新相应订单状态并交付回报。

撤单请求已发送与订单已撤销分别表达。adapter 的 `CancelOrder` 在调用期间不得触发 Callback，结果在后续调度中交付。撤单失败保留订单已经发生的成交和当前状态；撤单成功保留已成交部分，结束剩余委托。

### 成交与状态更新

`AdapterOrderReport.filled_quantity` 表达柜台已成交的累计数量，范围为 0 到原始委托数量；拒绝回报必须为 0。非法数量及已拒绝订单的接受回报诊断后忽略。

订单状态、累计成交和待处理撤单分别保存。撤单期间仍接收成交回报；后到达的接受回报不能把已成交或已撤销订单恢复为活动订单。

累计量为正且小于委托数量时，活动订单进入 `kPartiallyFilled`；达到委托数量时进入 `kFilled`。已撤销订单收到迟到部分成交时只更新数量，全部成交事实优先于已撤销状态。累计回报引起状态或对外累计量变化时递增版本并交付订单回报。

成交明细按账户及完整成交标识去重，内容一致的重复事件不再次累计数量或通知 listener；同一标识内容冲突时，诊断并忽略。对于通过校验的累计成交数量和成交明细，分别记录已报告累计数量的历史最大值与去重后明细总量，订单累计成交量取二者较大值，避免把同一成交计算两次。迟到的较小累计值不能使订单数量回退。adapter 负责将柜台字段转换到这一事件语义。

成交价格须为有限正数、数量须为正且明细总量不得超过委托量。校验失败、错误账户、未知订单及已拒绝订单的成交均诊断后忽略，不消耗成交标识。发送失败后收到有效成交时，诊断发送判断与柜台事实不一致，再更新订单。

每笔有效的新明细只向原订单 listener 交付一次 `OnTradeReport`，不额外交付相同变化的 `OnOrderReport`；回报带有更新后的快照。累计回报已使订单全部成交时，补到的唯一明细仍须交付，但对外状态与数量没有变化就不递增版本。

每个接入事件先核对来源账户、订单关联及数据有效性。通过校验后更新订单记录，生成回报快照并通知对应 listener；异常事件走对应处理路径。回调内查询能够看到已应用本次有效事件的订单状态。

### 多账户调度

`TradingService::Run()` 先按内部批量上限交付待通知回报，再依次调用各账户 adapter 的 `Run()`，每个账户调用一次。adapter 自行限制本次处理量，通过 Callback 交付回报。某个账户或待通知队列持续有工作时，其他账户仍能在本轮得到推进。

```text
按批量上限交付待通知回报
遍历每个账户：
    调用 adapter.Run()
    在 Callback 中接收并处理回报
```

待通知队列保存 service 产生的请求失败结果，以及因请求调用或同一订单仍有待通知回报而需要延迟交付的业务回报。需要排队时，保存该次通知内容和当时状态快照，连续产生的接受、成交等通知保持原顺序。交付已保存通知时不重复更新订单状态。

### 资金与持仓

资金与持仓按账户保存初始状态、后续更新及查询可用性。对应接入通知沿用 `ITradingAdapterCallback` 的线程和数据借用约定，分别随 N2.7、N2.8 细化回调数据及查询接口。

同一账户的多个策略读取该账户的共享状态。模拟接入提供可控的初始数据和更新，SimNow 接入获取实际账户数据；远端 client 通过接收到的状态更新维护本地查询视图。

## 订单存储与生命周期

账户表以连续存储保存稳定账户上下文的指针或索引，供 `Run()` 遍历，另建账户标识索引供请求定位。已注册为 Callback 的 `AccountContext` 地址保持稳定。订单存储以统一 `OrderId` 建立索引，查重、撤单、回报和查询均通过该索引定位。账户及策略归属保存在记录中，定位后再核对。查询和回报路径使用同一份订单状态。

本次运行期间建立的全部订单需要可查询，包括终态订单及已建立记录后发送失败的订单。订单链路的内存使用必须有明确上限，预算覆盖订单及索引、撤单关联、柜台标识映射、查重与成交去重信息，以及请求关联、待通知回报和接入交接缓冲。

终态订单的保留方式、内存回收条件和历史查询路径在 [N2.9 订单存储容量管理](../plans/v1.md#n29订单存储容量管理)中细化并实现。回收需要同时考虑待处理撤单、尚未交付的通知和迟到回报，保持订单查重、成交去重及查询契约。未交付或校验失败且没有建立订单的调用，只保留完成本次请求结果所需的临时信息；请求关联与通知数据不再被使用后及时释放。

存储按预算预分配并复用，必要的扩容受预算约束。`OrderId` 与存储槽位分离，保留 caller 生成的原值。回调读取事件快照，不暴露内部记录地址；快照仍被使用时，对应存储不能回收或覆盖。

接受请求前保证有足够存储保存请求及其结果，并为已接收订单的后续回报处理保留空间。本地 service 无法接收时，client 返回未交付；跨进程请求已经交付消息通道后，service 拒绝接收的结果由 server 转为 `RequestFailure`，关联原调用并通知 listener。容量不足不能通过覆盖已有记录或静默丢弃回报处理，回收出足够空间后恢复接收。

client 由创建它的调用方持有；实例表和 factory 用于初始化，可以先于已经创建的 client 销毁。service 在 client 和业务回报处理结束后销毁，adapter 的有效期覆盖 service。

## 模拟场所与扩展

### 可控验证

`SimulatedTradingAdapter` 接收报撤单并维护本账户的模拟委托。验证场景可以控制发送结果、委托接受或拒绝、撤单结果，触发指定价格和数量的成交，并控制回报的释放时机和交错顺序；同时支持注入异常字段、重复或矛盾回报、回报缺失及连接状态变化，验证 Trading 的处理规则。

`Fill(const TradeInfo&)` 在模拟柜台中验证活动订单、成交字段、重复标识与剩余数量，成功时保存独立明细并排队，失败时返回 false 且不改变柜台。`EmitTrade` 供验证场景直接注入迟到或异常明细，不改变模拟柜台状态。

模拟的委托、撤单和成交事件通过注册的 Callback 交付，在报撤单与 `Fill` 调用期间只保存数据，由 adapter 后续的 `Run()` 交付。每次最多处理 64 个事件，三类事件共用按入队顺序处理的缓冲。交付前移出当前事件，回调内提交请求导致缓冲扩容时也不会覆盖当前数据。模拟接入可以分别控制发送返回与回调内容，service 先处理发送结果，再处理后续回报。

固定行情继续使用现有行情源。成交由验证场景显式触发，N2 的报单、撤单、成交节点逐步补齐模拟接入能力。

### SimNow 接入

每个账户创建对应的 `SimNowTradingAdapter`，由 `main` 注入同一个 service。接入实现负责登录和交易就绪、原生标识映射及请求转换；报单调用期间产生的回报先保存在接入内部存储，由后续 `Run()` 交付；其他线程的回报也经接入内部存储转到业务线程。client 与业务订单处理使用同一组接口。

### 跨进程调用

跨进程阶段增加 `RemoteTradingClient` 和 `TradingServer`。server 将消息解码为 service 请求，并把回报编码给原调用方；统一 `OrderId`、账户、策略归属和撤单关联标识完整传递。

远端 client 依据调用上下文关联本次请求的 listener，订单缓存按统一 `OrderId` 索引。收到 service 提供的订单快照时，先按状态版本更新缓存，再调用业务 listener；只携带请求失败的回报不改变已有订单缓存。查询读取当前可用的缓存快照。factory 结合拓扑选择本地或远端 client；账户到 adapter 的绑定仍由目标 service 处理。

远端订单缓存和请求关联存储同样受容量预算约束。缓存回收后的查询可用性与数据获取方式沿用订单保留方案，在 N9 中实现并验证，N12 复用这些规则。

## N2 节点的设计重点

| 节点 | 核心实现与验收重点 |
| --- | --- |
| N2.1 统一订单标识与模拟接入 | 生成统一 ID，模拟接入保存委托，控制发送结果并在后续调度中交付回报。 |
| N2.2 多账户交易 service 报单处理 | 多账户路由、完整 ID 查重、订单状态和通知快照，验证发送失败、迟到回报、回调时机与重入。 |
| N2.3 本地交易 client 与完整报单场景 | 提供 client、factory 与注册表，通过 `monod` 验证完整报单路径和调用关联。 |
| N2.4 撤单与撤单回报 | 在既有订单记录上增加撤单请求关联，使用正确账户和原订单发送撤单，区分撤单结果与订单状态。 |
| N2.5 订单查询与累计成交 | 查询订单及可用性，按累计成交回报更新数量与状态，验证撤单交错及迟到回报。 |
| N2.6 成交明细与去重 | 按完整成交标识去重，合并明细与累计回报数量，验证冲突、越界及终态后补到明细。 |
| N2.7 账户资金查询与更新 | 接入可控的初始资金与后续更新，维护账户查询视图及可用性，验证多账户隔离。 |
| N2.8 账户持仓查询与更新 | 接入初始持仓快照和后续变化，维护持仓查询视图及可用性，验证完整快照与账户隔离。 |
| N2.9 订单存储容量管理 | 约束订单链路的内存使用，验证容量边界、可回收空间复用、早期订单查询及迟到回报处理。 |
| N2.10 异常接入事件处理与综合验收 | 实现无法关联、字段异常及状态矛盾事件的处理，验证账户接入变化及已发送请求的后续处理；综合核对多账户归属、回调时机、重入、通知顺序和查询状态。 |

请求返回语义区分 client 未交付和已交付；adapter 的发送结果区分已发送和确定未发送。请求已经交付后，后续处理结果通过回报表达。具体数据类型和处理分支随对应节点细化，并核对各节点的代码量预算。
