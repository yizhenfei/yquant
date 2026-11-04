# 策略设计

## 代码组织

策略框架的公共头文件位于 `include/yquant/strategy/`，实现位于 `src/strategy/`，使用 `yquant::strategy` 命名空间。具体策略按名称组织在 `include/yquant/strategies/<名称>/` 和 `src/strategies/<名称>/`，使用各自的命名空间；做市策略使用 `yquant::strategies::market_maker`。

`yquant_strategy_api` 提供策略接口的编译依赖，`yquant_strategy_host` 负责配置解析和动态库加载，`yquant_strategy` 负责策略 service。做市实现编译为 `yquant_market_maker_core`，供插件入口和测试使用；`yquant_market_maker` 是宿主动态加载的策略库。做市实现依赖框架接口，框架库不包含或链接具体策略实现。

## 配置与动态加载

`yquant_strategy_host` 从配置文件加载一个策略实例。配置采用 `key=value`，支持空行和以 `#` 开头的整行注释。重复键、未知键、缺少必需项及超出整数范围的标识均报错。动态库路径相对于配置文件解析。

| 配置项 | 含义 |
| --- | --- |
| `library` | 策略动态库路径 |
| `strategy_id` | 本实例的非零策略标识 |
| `account_id` | 组装层绑定的非零账户标识 |
| `param.<名称>` | 交给策略工厂校验的字符串参数 |

[做市策略配置模板](../../configs/market_maker.conf.in) 使用 `param.instrument_id` 指定合约，使用 `param.quantity` 指定每侧正整数手数，两者由做市插件解析和校验。公共策略配置不限定合约数量，框架不读取策略私有参数。CMake 的 `yquant_market_maker` 目标在 Linux 生成 `libyquant_market_maker.so`，在 macOS 生成 `libyquant_market_maker.dylib`，并在库的同一目录生成 `market_maker.conf`。宿主通过配置选择库路径。

### 库接口

动态库导出 C 链接入口 `yquant_strategy_plugin()`，返回接口版本、创建函数和销毁函数。宿主用 `RTLD_NOW | RTLD_LOCAL` 打开库，检查版本及函数入口后再创建实例。

```cpp
std::span<const std::uint32_t> Strategy::Subscriptions() const;
Strategy* create(const StrategyConfig&, TradingClient&,
                 OrderIdGenerator&, std::string& error);
void destroy(Strategy*);
```

实例实现行情和交易 listener 接口。交易 client 与 caller 共用的订单 ID 生成器由宿主注入。实例通过 `Subscriptions()` 返回初始订阅合约列表，可为空；合约 ID 必须非零且不重复。宿主复制列表，通过行情 client 建立订阅，并在停止时清理全部订阅。一个实例可以订阅多个合约。策略只能使用自身作为交易 listener，创建和销毁期间不发起交易或注册额外 listener。工厂创建失败时释放已取得资源并返回空指针，错误说明通过 `error` 返回。销毁函数在所属动态库仍加载时执行。

当前版本为 2，接口包含 C++ 虚类和标准库类型。宿主与策略库须使用同一版项目头文件、兼容编译器 ABI 和相同标准库。接口布局或语义发生不兼容变化时提升版本，并重新构建两端。

### 实例生命周期

组装层通过 `LoadedStrategy::Load(config_path, market, trading, ids, error)` 创建实例。配置和库校验成功后，宿主才订阅对应合约。多个实例分别拥有注册关系，共享的 client、service 和订单 ID 生成器由组装层管理。

停止发生在单业务线程的 service 调用之间。先解除交易服务保留的 listener 引用和待通知队列中的目标，再退订行情，最后销毁实例并卸载库。订单与资金继续由交易服务维护。停止发生在报价回调或交易 service 操作中时返回 false，实例继续运行；组装层应在停止调度后再销毁宿主对象。

`v1_06_strategy_plugin` 用两份配置加载做市库和不同的测试策略库，验证行为、参数及共享序号。失败场景覆盖无效路径、入口缺失、版本不符、创建失败和非法配置；卸载后继续交付排队回报，核对账本仍可更新且不再调用旧实例。

## 策略 service 实例管理

组装层创建 `StrategyService(caller_service_id, market_client, trading_client)`，再用 `Load(configs, error)` 一次加载整组策略配置。实例 ID 在同一 service 内唯一。每个 service 持有自己的订单序号生成器，组内策略共用该生成器及注入的交易 client；停止并重新加载实例不会重置序号。

service 先完成全部实例创建，再按合约向上游各订阅一次。任一配置或工厂失败时，销毁本次已创建实例，不发布上游订阅。行情分发表以合约 ID 为键，值为按配置顺序保存的 listener 列表；收到行情时只遍历该合约的实例，分发期间不分配内存。交易回报沿用交易服务中的原 listener 绑定，直接交给原策略。

`Stop()` 在同一业务线程的 service 调用之间执行，先解除全部实例的行情和交易回调，再销毁整组实例。分发行情期间调用停止会返回 false，继续完成本次分发。借用单个行情 listener 的旧构造方式仍由调用方管理策略及订阅生命周期。

`v1_07_strategy_instances` 验证同品种与不同品种分发、共用序号、回报归属、重复 ID、部分创建失败回滚、停止后的迟到回报，以及不同 service 的 caller 隔离。生产配置中的实例列表和多个 service 组装见[单进程运行](monod.md)。

## 简单做市策略

`MarketMaker` 为一个账户的一个合约维持一笔买单和一笔卖单。每侧使用固定手数，买单按买一价开仓，卖单按卖一价开仓。有效盘口要求两侧价格有限、买价大于零且低于卖价、两侧挂量均为正；忽略其他合约和无效盘口，保留最后一次有效目标价。盘口数量变化不触发改价。

组装层通过 factory 创建行情和交易 client，在账户启动同步完成后交付策略行情，并向策略注入交易 client 和 caller service 共用的订单 ID 生成器。`StrategyService` 接收订阅合约的行情并交给策略；策略通过交易 client 提交请求，以 listener 接收本策略的回报。请求期间不发生业务回调，接口契约见[交易设计](trading.md)。策略在一个业务线程中运行，报价路径使用两个固定的委托状态对象。

### 每侧委托的处理规则

| 当前状态 | 收到的事件 | 策略动作 |
| --- | --- | --- |
| 空闲 | 有目标价且账户资金可查询 | 分配新的订单 ID 并报单。若报价会与本策略尚未撤回的对侧单成交，则等对侧回报后重新检查。 |
| 等待报单结果 | 盘口变化 | 只更新目标价。接受回报或部分成交确认订单有效后，再检查是否需要撤单。 |
| 挂单中 | 价格未变，或只有部分成交 | 保留剩余挂单；部分成交不补足手数。 |
| 挂单中 | 目标价变化 | 对这一侧发起一次撤单，保留原订单 ID 和本次撤单请求 ID。 |
| 等待撤单结果 | 再次改价或收到部分成交 | 只更新目标；即使旧单已全部成交，也等本次撤单结果后再报替换单。 |
| 撤单成功，或挂单全部成交 | 旧单终结 | 按最新有效目标价和固定手数重新报价。若只有累计成交量而明细未到，资金查询暂不可用；成交明细到达后重新检查空闲侧。 |

报单被拒时释放该侧订单状态，同一目标价不反复报单。撤单被拒且原单仍有效时保留原单，同一目标价不重复撤单；等待期间目标已改变时，可直接处理新目标。交易 client 明确未交付的请求不会等待回调。报单发送失败后保留原 ID，以接收可能迟到的柜台回报；没有进一步柜台事实时，该侧不自动再报。旧订单的迟到成交不会替换当前订单，旧撤单尝试的结果也不能结束新的尝试。

### 可执行验证

`yquant_scenarios --maker` 在一个 container 中先调度行情 service，再调度交易 service。固定场景的前两条行情相同，只产生买卖各一笔；第三条仅提高买价，先撤原买单，收到确认后再发新买单，原卖单保持有效。程序逐笔核对提交与撤单请求的顺序、标识及报价，最后通过交易 client 查询两笔有效挂单。

`v1_03_maker` 使用真实进程内交易 client 和可控模拟柜台，覆盖延迟接受、连续改价、成交与撤单交错、拒绝及发送失败。运行 `ctest --test-dir build -L v1_03_maker --output-on-failure` 可同时执行状态机及重复运行场景。延迟测量方法和结果见[单进程报价延迟](../benchmarks/maker.md)。
