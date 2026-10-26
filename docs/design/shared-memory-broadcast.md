# 共享内存行情广播组件

共享内存广播封装将[单写多读广播队列](broadcast-queue.md)用于本机进程间通信。行情层在字节记录上编解码事件，接入 [marketd 行情管线](marketd.md)。

## 组件职责

| 层次 | 组件 | 职责 |
| --- | --- | --- |
| 通用队列 | `BroadcastQueue`、`BroadcastWriter`、`BroadcastReader` | 外部内存布局、seqlock 读写、独立游标、覆盖缺口及恢复 |
| 共享内存资源 | `SharedMemoryMapping` | 创建、打开、预留后备空间、映射、验证同名对象、释放映射和创建者名称 |
| 共享内存注册 | `ShmStreamRegistry` | 安全目录、稳定写者锁、代次、manifest/pending、发现与失效名称清理、fork 所有权 |
| 共享内存封装 | `ShmBroadcastWriter`、`ShmBroadcastReader` | 组合映射、注册与队列，协调生命周期及心跳 |
| 行情适配 | `ShmPublisher`、`ShmReader` | 自持编解码缓冲区、事件校验、行情读取状态映射 |
| 事件编解码 | `EncodeEvent()`、`DecodeEvent()` | 行情事件头、schema、载荷长度及字段约束 |

通用队列及共享内存封装属于 `yquant::base`；行情适配属于 `yquant::market`，只调用字节广播接口，不直接操作槽位、原子标记或共享内存文件。

过滤由接收方的 [EventFilter](market-data.md#接收端事件过滤)独立执行。广播接口不保存订阅或过滤条件。

## 公共接口

字节广播接口见 [base/shm_broadcast.h](../../include/yquant/base/shm_broadcast.h)；行情接口见 [market/shm_broadcast.h](../../include/yquant/market/shm_broadcast.h)。

| 接口 | 语义 |
| --- | --- |
| `ShmBroadcastWriter::Create(config)` | 独占逻辑流，创建映射并初始化新队列实例 |
| `TryWrite(bytes, sequence)` | 提交一条字节记录，失败不占序号、不改变可选输出 |
| `Tick()` | 在冷路径更新心跳，空闲时继续调用 |
| `ShmBroadcastReader::Open(endpoint, scratch, start)` | 映射并校验当前实例，attach 读者和调用方暂存区 |
| `TryRead(budget)` | 返回队列读取结果；已发现替换时另行报告 stream_changed |
| `Resume(token, sequence, position)` | 委托队列显式恢复，代次替换后拒绝旧 token |
| `Refresh(result)` | 在冷路径检查发现入口和写者健康 |
| `Close()` | 幂等释放本端资源；写端先结束队列 |

每个对象由单一执行流串行使用。创建、打开和维护可以分配内存、执行文件 I/O；字节读写和行情发布读取热路径不分配内存、不执行系统调用。

`ShmBroadcastReader` 返回暂存区借用，与通用队列的有效期相同。行情 `ShmReader::TryNext(buffer)` 校验并复制到调用方的 `EventBuffer`，仅在成功时更新输出。非法行情事件使适配器停止后续交付，并保留失败事件的序号；字节队列不解释行情格式。

## 身份与数据语义

| 标识 | 作用 |
| --- | --- |
| generation | 注册层生成的实例身份，同时写入共享内存封装和队列布局；重启创建新身份 |
| 队列序号 | 当前实例内的读取顺序与覆盖检测，初始提交从 1 开始 |
| 行情来源与源序号 | 由事件 codec 保存的上游身份，与本机队列序号独立 |

每个读者独立读取所有字节记录；写者不等待读者。正常结束后，已有映射仍能读完保留记录。队列的起点、尝试预算、覆盖检测与恢复契约统一见[广播队列设计](broadcast-queue.md)。

## 共享内存布局

```text
SharedMapping
├─ ShmBroadcastControl
│  ├─ ImmutableEnvelope
│  └─ Heartbeat
└─ BroadcastQueue storage
   ├─ ImmutableLayout
   ├─ State
   ├─ PublishedSequence
   └─ Slot[capacity]
```

封装头保存版本、映射总长、队列区域偏移及长度、心跳原子的尺寸/对齐/偏移，以及 generation。队列区域拥有独立布局、版本和原子 ABI，不包含心跳或行情 schema。

接收方先校验封装头的普通字节字段和区域边界，再通过 `BroadcastQueue::Attach()` 校验队列布局及原子 ABI，最后才访问封装心跳。两层身份必须与 manifest 一致。队列长度必须匹配根据配置重算的尺寸，映射总长按系统页大小对齐，上限为 1 GiB。

行情事件载荷上限由 `kMaxEventPayloadSize = 4096` 定义。行情适配器配置的队列记录大小为事件头长度加该流的载荷上限，记录中的未使用部分清零；事件 schema 在记录内校验。通用队列和字节共享内存接口可以配置更大的记录。

## 流注册与唯一写者

运行目录必须是当前用户拥有的绝对路径，组和其他用户不可写入。逻辑流名称限定为一个安全路径元素；组件文件及共享内存对象使用当前用户权限。

`ShmStreamRegistry` 为每个流持有稳定的锁文件，通过 `flock(LOCK_EX | LOCK_NB)` 获取写者排他权。锁文件不删除或替换；失败通过 Status 保留系统错误。映射类不持有该锁，也不处理发现入口。

manifest 保存完整 generation 与唯一共享内存名称，通过临时文件加 rename 原子发布。pending 在创建共享内存之前持久记录预期名称，使下一任写者能够清理创建中途退出留下的资源。

fork 子进程通过进程私有所有权代次被禁止使用继承写端发布、更新心跳或结束队列。释放继承对象只丢弃本地映射和描述符，不撤销父进程入口，不显式解锁 flock。

## 创建与连接

写端创建顺序：

1. 注册层校验端点、获取写者锁，清理旧 manifest/pending 指向的失效资源。
2. 生成 generation，写入 pending。
3. 映射层创建独占名称，确定尺寸并映射；Linux 通过 posix_fallocate 预留空间，将空间不足作为创建错误返回。
4. 初始化封装头，调用通用队列 Initialize 构造槽位及控制原子，再 attach 写端。
5. 设置初始心跳，发布 manifest，移除 pending。

任一步失败均释放本次拥有的资源，写者锁保持到清理结束。命名碰撞进行有界重试。

读端连接顺序：

1. 读取 manifest，按共享对象的实际尺寸映射，检查对象所有者、权限及尺寸预算。
2. 校验封装头、队列区域和原子 ABI，核对队列尺寸及两层 generation。
3. 生成读者 attach 身份，调用通用读者 Attach，借用调用方暂存区。
4. 再次读取 manifest，并验证同名共享对象仍对应已映射的 inode 和尺寸；发现替换或名称消失时有界重试。

创建和连接错误不留下半打开的公开对象。读端不会自动跳到新实例。

## 停止与健康维护

写端关闭按顺序调用队列 Finish、detach 写端、撤销本实例发现入口、移除创建者名称并释放映射，最后释放稳定锁。原子对象不被析构或原地复用；已有读者映射继续保留结束代次。[共享内存对象生命周期](https://man7.org/linux/man-pages/man3/shm_open.3.html)

写端异常退出时，下一任持锁写者清理旧名称并创建新对象。旧读者在 Refresh 中发现 generation 变化，之后停止旧实例交付；旧缺口 token 失效。

Tick 和 Refresh 由控制周期驱动。Refresh 返回当前、发现缺失或实例替换状态，同时返回写者 ready、stale、stopped 健康状态。心跳超时不阻止读者读取仍完整的提交；队列槽位一直停在写入中时，有界返回 Retry，由上层结合健康状态决定终止。

`StreamChanged`、`EndOfGeneration` 等行情读取状态由适配层映射；其中实例替换来自发现层，正常读尽来自队列 Finish，两者分别维护。

## 容量与验证

队列容量按完整流的峰值速率、可容忍落后时间和突发余量估算，再向上取 2 的幂。接收方只关注少量标的时，也需要按完整流评估覆盖风险。

共享内存测试验证不同地址及跨进程原子访问、字节记录、布局拒绝、写者锁、pending 清理、fork、心跳、重启和正常读尽。行情测试验证编解码错误、调用方缓冲区不变和来源语义。并发 seqlock 协议及普通内存验收集中在[广播队列测试](broadcast-queue.md#验证)。
