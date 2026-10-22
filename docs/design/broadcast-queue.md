# 单写多读广播队列

`BroadcastQueue` 在调用方提供的内存上实现覆盖式广播。同一队列由一名写者提交字节记录，多名读者各自维护私有游标；慢读者不阻塞写者，落后导致的数据覆盖通过缺口报告。

## 组件边界

公共接口位于 [broadcast_queue.h](../../include/yquant/base/broadcast_queue.h)，实现位于 `yquant::base`。普通内存与共享内存使用相同的布局及 seqlock 协议。

| 类型 | 职责 |
| --- | --- |
| `BroadcastQueue` | 计算尺寸和对齐、初始化存储、校验布局和原子 ABI、提供非拥有视图 |
| `BroadcastWriter` | 维护写序号、按 seqlock 协议提交记录、正常结束队列 |
| `BroadcastReader` | 维护私有游标和暂存区、有界读取、检测覆盖、保存缺口和恢复状态 |

队列只处理有长度的字节记录。行情 schema、事件过滤、内存分配与释放、文件名、系统时钟和进程发现均由调用方或上层封装负责。[共享内存广播封装](shared-memory-broadcast.md)组合这些接口管理本机进程间传输。

## 内存与生命周期

`BroadcastConfig::capacity` 为非零的 2 的幂，单位为记录条数；`max_record_size` 限制单条记录的字节数，允许为零。通用队列没有 4096 字节的行情载荷限制。

调用方按 `RequiredBytes(config)` 和 `required_alignment()` 分配内存：

```text
size = BroadcastQueue::RequiredBytes(config)
allocate size.bytes with required_alignment()
BroadcastQueue::Initialize(memory, config, instance_id)
writer.Attach(memory)
reader.Attach(memory, private_scratch, start)
```

初始化构造队列中的原子对象。初始化期间需要独占存储；初始化完成与其他执行流的 attach 之间由调用方建立同步。队列初始化身份由调用方提供，在旧端点或 token 仍可能使用时不得复用。

端点不拥有队列内存。读者额外借用至少 `max_record_size` 字节的私有暂存区；它不能与队列或其他读者的暂存区重叠。普通内存可由定长数组或低频分配的连续缓冲区承载；共享内存可使用映射后的区间。

每个端点由单一执行流串行使用。调用方保证只有一名活动写者；写者转交所有权时，旧端点先停止使用并 detach，新端点从已提交尾部继续写入。detach 和析构不自动结束队列，只有 `Finish()` 发布结束状态。

释放或重新初始化存储前，需要停止全部访问并使所有旧端点 detach。跨进程场景使用新共享内存对象承载新实例，不原地重建仍有读者的旧队列。

## 记录读写

`TryWrite(record, sequence)` 在返回前复制输入。输入不得与队列存储重叠；超长、序号耗尽或状态错误不会提交记录，也不改变可选序号输出。空记录可以正常提交。

`TryRead(budget)` 每次最多返回一条一致记录，只在成功时推进游标。返回的 span 指向读者的私有暂存区，有效期到下一次 `TryRead()` 或 detach；下一次读取即使失败，也可能覆盖旧暂存内容。非成功结果不交付 span。

读取起点默认为连接时已提交尾部之后。指定 `At(sequence)` 可以读取保留的历史，目标已被覆盖时报告缺口。起点可同时指定预期实例身份，以拒绝误连。

| 状态 | 含义 |
| --- | --- |
| `kRecord` | 得到下一条记录及其队列序号，游标已推进 |
| `kEmpty` | 已读尽当前提交，队列仍可继续写入 |
| `kRetry` | 槽位写入或一致性冲突消耗完尝试预算，游标保持 |
| `kGap` | 目标记录已经被覆盖，暂停读取等待恢复 |
| `kEnd` | 写者已调用 Finish，且保留的提交已读尽 |
| `kDetached` | 当前端点未 attach |
| `kError` | 参数或队列数据错误，携带具体原因 |

零尝试预算属于可纠正的参数错误。非法状态、长度和槽位标记属于粘滞错误，后续调用继续报错。所有队列操作均不分配内存、不执行系统调用。

## 队列布局

```text
Queue
├─ ImmutableLayout
├─ State: Ready / Finished
├─ PublishedSequence
└─ Slot[capacity]
   ├─ stamp
   ├─ length
   └─ words[word_count]
```

布局包含格式版本、字节序、原子 ABI、队列尺寸、容量、单记录上限、槽位步长、偏移及实例身份。布局是固定宽度的不可变字段；attach 从字节副本重算并验证所有尺寸，在验证通过后才访问原子对象。

控制字段、槽位起点及步长按 128 字节对齐。队列尺寸不依赖系统页大小；页对齐及共享内存资源预算由映射层处理。

`stamp`、`length`、载荷单元和控制状态均通过 `std::atomic<std::uint64_t>` 访问。单条记录占一个槽位，位置为 `(sequence - 1) & (capacity - 1)`。记录序号范围为 `1..2^63-1`，耗尽后拒绝写入。

| stamp | 含义 |
| --- | --- |
| 0 | 尚未使用 |
| 2 × s − 1 | 正在写序号 s |
| 2 × s | 序号 s 完整提交 |

## seqlock 协议

写者先校验输入，再使目标槽位失效。奇数标记之后只进行有界的本地复制与原子操作：

```text
stamp.store(2 * s - 1, relaxed)
atomic_thread_fence(release)
length.store(record.size, relaxed)
for every configured payload word:
    word.store(input bytes padded with zero, relaxed)
stamp.store(2 * s, release)
published_sequence.store(s, release)
```

读者先以 acquire 读取结束状态，再读取尾序号，保证观察到 Finish 后也能观察到最终提交。存在目标记录时：

```text
a = stamp.load(acquire)
if a identifies a newer sequence: return Gap
if a != 2 * expected: retry within budget
length = slot.length.load(relaxed)
copy every configured atomic word to private scratch
atomic_thread_fence(acquire)
b = stamp.load(relaxed)
if b identifies a newer sequence: return Gap
if a != b: retry within budget
validate length
advance private cursor
return scratch[0:length]
```

每个非零标记还必须对应当前环形槽位。复制次数由已验证的配置决定，不使用未经验证的共享长度控制访存；只有两次标记均为目标记录的完整提交标记时，才交付记录。

载荷使用原子单元避免覆盖期间的数据竞争。写侧 release fence 与读侧 acquire fence 通过读自覆盖者的原子单元建立同步，保证读者能在二次校验时发现该次覆盖；两道 fence 都属于协议。[原子 fence 规则](https://eel.is/c++draft/atomics.fences)

编译期要求 64 位原子类型始终 lock-free；ABI 指纹包含标准库、原子尺寸和对齐、控制结构尺寸及字段偏移。跨进程使用还需验证目标操作系统、编译器与标准库在不同映射地址下的原子访问。[lock-free 与跨地址访问](https://eel.is/c++draft/atomics.lockfree)

## 缺口与恢复

覆盖判断同时检查槽位标记和尾序号。写者可能已经开始覆盖，却尚未提交新尾序号，读者必须根据奇数标记报告缺口。

目标为 `e`、观察到槽位正在写入或已提交 `q > e` 时，至少 `[e, q - capacity + 1)` 不可读。token 保存实例、读者 attach 身份、缺口编号、期望序号和候选恢复位置。

缺口保持到调用方显式 `Resume()`，可选择候选位置、指定序号或当前尾部之后。失败保持游标、缺口和输出不变；候选位置仍需重新校验，可能再次被覆盖。旧 token 不能恢复其他读者、其他 attach 会话或其他队列实例。

普通内存读者默认使用进程内的递增 attach 身份。跨进程调用方提供在 token 存活期间唯一的 `reader_identity`；共享内存封装在打开读者时生成该身份。核心队列不读取系统熵源。

## 验证

普通内存测试覆盖布局和溢出、可变长度及空记录、独立游标、写者转交、Finish、缺口恢复、重试预算、数据损坏和并发覆盖一致性。独立分配计数测试覆盖初始化、attach 和热路径。跨进程映射、发现和生命周期验收由共享内存封装测试承担。
