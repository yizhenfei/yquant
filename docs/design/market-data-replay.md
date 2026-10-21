# 行情回放与录制

历史输入将真实报价送入 `marketd` 的统一管线；录制输出保存管线中的报价和本次运行的到达间隔，供问题复现和压测使用。组件组合与执行流程见[行情进程设计](marketd.md)。

## 历史输入

```json
{
  "source": {
    "adapter": "replay", "format": "dolphindb_csv",
    "path": "snapshot.csv", "source_id": 1,
    "instrument_catalog": "instruments.json",
    "speed": 1, "start_delay_ms": 3000, "max_records": 400000
  },
  "publishers": [
    {"adapter": "shm", "runtime_directory": "/tmp/yquant-md",
     "stream": "quotes", "capacity": 524288, "max_payload_size": 80},
    {"adapter": "record", "path": "capture.yqr", "queue_capacity": 67108864}
  ]
}
```

| 字段 | 规则 |
| --- | --- |
| `format` | 必填：`dolphindb_csv` 或 `recording` |
| `path` | 必填；相对进程工作目录解析，解析后最多 4095 字节 |
| `speed` | 默认 1；0 为最快，0.5 为半速，2 为两倍速；必须有限且非负 |
| `start_delay_ms` | 默认 0，范围 0 至 60000；首次轮询后等待指定时间再开始交付 |
| `max_records` | 默认 1000000，范围 1 至 2000000；超过上限时整次初始化失败 |
| `source_id` | CSV 必填，1 至 4294967295；录制格式禁止设置，使用文件中的来源 |
| `instrument_catalog` | CSV 必填；共享标的字典路径，相对进程工作目录解析，解析后最多 4095 字节；录制格式禁止设置 |

`ReplaySource::Init(config)` 完整校验文件并预分配报价数组；CSV 输入先加载共享标的字典，按每个标的的定义转换价格。字典格式及维护规则见[行情驱动设计](market-data.md#标的与计量单位)。轮询路径没有文件读取或内存分配。内存按 `max_records` 上限分配 CSV 存储；录制输入按文件条数分配。`max_records` 是资源上限，不会截断文件；需要有限事件运行时使用 `--max-events N`。

首次 `Poll(now, quote)` 建立本进程单调时钟起点，预加载耗时不计入回放时间。正数速度将每条历史偏移除以速度，向下取整到纳秒；不能表示的时长导致初始化失败。最快模式保留记录顺序并取消记录间等待。到期前返回 `kIdle`，结束后返回 `kEnd`，这些结果保持调用方报价不变。文件只回放一次，避免循环时复用品种报价序号。

`start_delay_ms` 给订阅者留出连接时间，不代表消费者就绪确认。本机 `strategyd` 在 `marketd` 输出 `Running` 后启动，并在等待结束前完成订阅，才能接收第一条报价。共享内存容量仍需覆盖消费者可能落后的范围。

### DolphinDB CSV 转换

支持列名如下，顺序不限，其他列跳过：

| 列 | 转换 |
| --- | --- |
| `Date`、`Time` | 必填；`YYYY.MM.DD` 与 `HH:MM:SS[.小数]`，最多 9 位秒小数，按 UTC+08 转成 UTC 纳秒事件时间 |
| `SecurityID` | 必填；六位数字证券代码，保留前导零 |
| `SecurityIDSource` | 必填；`101` 为 SSE、`102` 为 SZSE，允许两端空白 |
| `BidPX1`、`OfferPX1` | 必填；非负十进制价格，按字典中该标的的 `price_decimals` 转成整数；多余位按半单位向上舍入 |
| `BidSize1`、`OfferSize1` | 必填；非负整数数量，沿用数据文件单位 |
| `RecvTime` | 可选；`YYYY.MM.DDTHH:MM:SS[.小数]`，UTC+08，用于到达偏移；缺少此列时使用事件时间 |

转换使用十进制整数计算，例如四位精度下 `4.309999999999999` 转为 `43100`。非法日期、负数、指数表示、非数字和溢出均报错，诊断包含 CSV 行号。数量为零时清除对应有效标志，并将该侧价格、数量置零。设置事件时间有效标志，不从盘口推断可交易状态。

品种标识使用项目统一的交易所与证券代码编码，CSV 中的每个标的都必须存在于共享字典。未知标的导致整次加载失败，保留调用方原有输出；不同标的可以使用不同价格精度。每个品种在文件内从 1 生成独立报价序号；CSV 没有还原供应商原始序号。来源使用配置的 `source_id`。

文件顺序决定交付顺序，不按事件时间重排。到达偏移相对第一条接收时间计算并保持非递减：历史时间回退时钳制到前一条偏移；事件时间本身保留原值。

导入器支持 UTF-8 BOM、CRLF、双引号字段、双引号转义和引号内换行；跳过空行。单条 CSV 记录最多 64 KiB、最多 256 列，拒绝重复列名、列数不一致和 NUL。只有表头的文件表示空回放，缺少表头时报错。当前只转换买卖一档，十档其余字段不进入 `QuoteTick`。

### 录制文件输入

录制输入使用 `format: "recording"`，路径指向 `.yqr` 文件，不设置 `source_id` 和 `instrument_catalog`。来源、品种、品种序号、事件时间、价量和标志全部保持，回放速度作用于录制时的到达偏移；这一过程不读取标的字典、不重标度价格。

每次回放用当前进程时钟重新赋值 `receive_time_ns`。该值随后仍按原共享内存或 TCP 接收规则在消费者进程重新赋值。以最快模式录制的历史数据保存的是加速运行中的间隔；要复现历史节奏，直接回放 CSV，或在原速运行中录制。

## 录制输出

```json
{"adapter": "record", "path": "capture.yqr", "queue_capacity": 1048576}
```

路径相对进程工作目录解析，父目录必须存在。输出采用独占创建，文件存在时失败，不覆盖或追加。每次运行使用新的文件名，文件轮转由启动环境管理。

`queue_capacity` 是 SPSC 队列的数据区字节容量，默认 1 MiB，范围 256 字节至 64 MiB，必须为 2 的幂。队列还包含帧头、对齐与回卷空间开销，容量不能直接除以文件记录大小来计算可容纳条数。

`RecordPublisher::Publish(quote)` 编码固定记录并入队，不分配内存、不访问磁盘、不调用时钟。后台线程每批最多写入 256 条，空闲时短暂休眠。到达偏移来自报价的本进程 `receive_time_ns`，第一条为零；本进程接收时间回退时报错。

队列满、非法报价或后台写入失败为终止错误，不静默漏记。`Service()` 检查后台错误；`Finish()` 停止接收、排空队列、等待线程、同步数据、写完成头、再次同步并关闭文件，最后同步父目录，返回延迟出现的错误。父目录描述符从创建时持有，目录改名不改变同步对象。重复调用保留第一次结果。析构和 `Close()` 进行尽力清理，需要验收落盘结果时调用并检查 `Finish()`。

初始化失败移除本次新建文件；运行失败后的文件保留供诊断。队列满或数据写入失败时不写完成头。完成标志表示文件布局写完；末次同步或关闭失败时仍可能存在该标志，持久化成功以 `Finish()` 的成功返回为准。进程崩溃留下的未完成文件不接受回放。

录制保存规范化的一档报价，不保存供应商原始报文或品种定义。CSV 转换器、模拟源与策略消费者使用同一份共享静态标的字典，保持价量单位和价格小数位一致。录制、TCP 和共享内存中继透传报价帧，不读取字典或转换价格。

## 文件格式

版本 1 使用固定 64 字节头和每条 96 字节记录，所有整数采用小端编码，不写入 C++ 结构体布局。

| 头偏移 | 长度 | 内容 |
| --- | --- | --- |
| 0 | 8 | ASCII `YQREC001` |
| 8 | 4 | 版本 1 |
| 12 | 4 | 头长度 64 |
| 16 | 4 | 记录长度 96 |
| 20 | 4 | 报价帧长度 80 |
| 24 | 4 | 完成标志：初始化 0，完成 1 |
| 28 | 4 | 保留，零 |
| 32 | 8 | 记录总条数 |
| 40 | 8 | 最后一条到达偏移，空文件为零 |
| 48 | 8 | 保留，零 |
| 56 | 8 | 头前 56 字节的 FNV-1a 64 校验值 |

| 记录偏移 | 长度 | 内容 |
| --- | --- | --- |
| 0 | 8 | 相对首条报价的到达偏移，纳秒 |
| 8 | 80 | 标准 [QuoteFrame](../../include/yquant/md/quote_codec.h) |
| 88 | 8 | 记录前 88 字节的 FNV-1a 64 校验值 |

FNV-1a 64 的初值为 `14695981039346656037`，乘数为 `1099511628211`。校验用于发现意外损坏。读取时检查版本、保留字段、完成标志、文件精确长度、各条报价与校验值、首条偏移零、偏移非递减以及最终偏移；全部成功后才替换加载输出。

## 使用示例

构建配置生成 `build/debug/marketd/replay_csv.json`，其中 CSV 路径取 CMake 的 `YQUANT_TEST_DATA_DIR`，字典引用 `examples/instruments.json`，包含历史样本全部标的的四位价格精度。准备数据后，最快导入官方历史样本并录制：

```sh
cmake --build --preset debug --target download_test_data
./build/debug/bin/yquant-marketd --config ./build/debug/marketd/replay_csv.json
```

在仓库根目录运行命令，该示例生成工作目录中的 `capture.yqr`，录制队列设为 64 MiB。再次运行前修改输出文件名或移动已有文件。可在配置中同时增加共享内存或 TCP 输出，或者用 `--max-events N` 录制有限前缀。

将录制文件发布到共享内存：

```sh
./build/debug/bin/yquant-marketd --config ./build/debug/marketd/replay_recording.json
```

在其 3 秒启动等待内，从另一终端启动观察策略：

```sh
./build/debug/bin/yquant-strategyd \
  --strategy ./build/debug/strategies/quote_observer_strategy.so \
  --config ./build/debug/strategies/replay_observer.json \
  --market-data-config ./build/debug/strategies/replay_market_data.json
```

示例观察 `SZSE/000557`，来源 1，价格四位小数。回放结束时 `marketd` 正常退出，策略仍按共享内存协议报告上游停止。若直接回放 CSV 到策略，将 CSV 输入的输出改为共享内存并设置启动等待和所需速度。

持续录制管线参考 [record_local.json](../../examples/marketd/record_local.json)。其输入可替换为 `shm` 或 `tcp`，录制组件不感知行情的接入或中继角色。
