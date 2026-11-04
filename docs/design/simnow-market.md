# SimNow 行情接入

`simnow_market_probe` 使用 CTP 行情前置，将一个指定合约的盘口交给验证策略，输出行情及对应的报价意图。程序创建行情 API，交易接入通过独立的交易验收程序验证。

## 构建依赖

Linux x86_64 和 macOS 的行情、交易接入组件均使用厂商动态库。头文件必须与该平台的二进制来自同一套 SDK；不要混用 Linux 和 macOS 的头文件。SDK 由构建环境提供，默认根目录为 `vendor/ctp/`，也可通过 `-DYQUANT_CTP_SDK_ROOT=/path/to/sdk` 指定。缺少任一 SDK 的头文件或动态库时，CMake 配置失败。

| 平台 | SDK 根目录下的布局 |
| --- | --- |
| Linux x86_64 | 根目录或 `include/ctp/`、`include/` 下放置头文件；根目录或 `lib/` 下放置 `libthostmduserapi_se.so` 或 `thostmduserapi_se.so`，以及 `libthosttraderapi_se.so` 或 `thosttraderapi_se.so` |
| macOS arm64 / x86_64 | 完整的 `thostmduserapi_se.framework/` 和 `thosttraderapi_se.framework/`，包括其中的 Headers、对应架构的二进制及符号链接 |

头文件包括 `ThostFtdcMdApi.h`、`ThostFtdcTraderApi.h`、`ThostFtdcUserApiStruct.h` 和 `ThostFtdcUserApiDataType.h`。可使用[SimNow 发布的 SDK](https://www.simnow.com.cn/static/apiDownload.action)；本项目可控验收使用 [vnpy_ctp 分发的固定版本](https://github.com/vnpy/vnpy_ctp/tree/1be9bfb5292208e5c258cc769c90be4abe83f9a9/vnpy_ctp/api)。使用该版本时，直接以检出的 `vnpy_ctp/api` 为 SDK 根目录：

```sh
git init /tmp/yquant-ctp-sdk
git -C /tmp/yquant-ctp-sdk fetch --depth 1 https://github.com/vnpy/vnpy_ctp.git 1be9bfb5292208e5c258cc769c90be4abe83f9a9
git -C /tmp/yquant-ctp-sdk checkout --detach FETCH_HEAD
cmake -S . -B build -DYQUANT_CTP_SDK_ROOT=/tmp/yquant-ctp-sdk/vnpy_ctp/api
cmake --build build
```

运行时保留 SDK 动态库目录；部署机器也需要对应库。macOS 可用 `otool -L build/simnow_market_probe` 核对 framework 依赖，Linux 可用 `ldd build/simnow_market_probe` 核对共享库解析。

## 连接与行情交付

业务线程创建 `SimNowQuoteSource`，注册 SPI 和前置地址，启动行情 API。SDK 的连接回调通知业务线程发起登录；收到最终成功登录响应后（接受本次请求编号 1，或该行情前置返回的 0），业务线程发起合约订阅。全部配置合约确认成功后，匹配这些合约的深度回报才可进入队列。真实 SDK 的操作使用该发行版默认的环境模式，行情前置须与之匹配。

接入对象复制买一、卖一的价格和手数，将合约映射为组装层给定的内部标识，并为每个合约的有效行情分别分配从 1 开始的本地序号。序号表示本次连接中该合约的有效行情顺序，环形缓冲保持各合约回报的整体到达顺序。验证场景只交付可生成双边意图的盘口：忽略其他合约、缺侧、非有限价、CTP 缺省极值、非正价量及买价不低于卖价的快照。

SDK 回调不执行策略代码。回调将值写入预分配的 1,024 条环形缓冲；业务线程通过 `TryRead` 尝试取数，读到行情时返回 `true`；锁繁忙或暂时无数据时立即返回 `false`，输出参数保持不变。行情 service 在本线程按已有订阅分发，验证策略通过普通行情 client 的订阅收到数据。回调参数的指针不离开回调。

登录或订阅失败、断连、序号耗尽和队列满均终止本次会话，诊断写入 stderr；未读行情不再交付。超时未收到要求条数时验收失败。销毁接入对象时先注销 SPI，再释放 API，回调存储保留至 Release 返回。重试需重新建立连接与订阅。

## 多合约订阅

`SimNowMarketConfig::subscriptions` 保存多组完整柜台合约代码与内部合约 ID。使用该列表时，单合约字段 `symbol` 和 `instrument_id` 留空。原单合约配置仍可使用。空合约、非法标识、重复合约或重复内部 ID 在创建 SDK 前被拒绝。

接入在初始化时复制合约名称并构建只读哈希索引，通过同一个 SDK 对象完成一次登录和一组订阅。深度回报直接按合约代码查表，未知合约在复制盘口前被忽略。订阅回报逐项登记，重复成功确认不增加已确认数量；结束标记不能代替未收到的合约确认。`ready()` 仅在全组确认成功后为 true；全部确认前的行情不交付。任一订阅被拒、非法订阅响应或断连均使整组失败。尚未收齐回报的组保持未就绪，由调用方的启动截止时间结束等待。

订阅名称和发给 SDK 的字符指针只在初始化时分配，持续有效到 `Release` 完成。行情回调查表使用固定字符串视图，队列和各合约计数在同一互斥锁下更新；业务线程读取就绪、失败状态时使用原子状态。

## 验收

验收程序仅在 `BUILD_TESTING=ON` 时生成。

可控场景使用当前平台真实 SDK 头文件，以测试实现替换行情 API 对象。SPI、回调转换、行情 service、client、验证策略和 CLI 输出校验均复用实际接入路径。

```sh
ctest --test-dir build -L v1_04_simnow_market --output-on-failure
```

实际验收使用 Linux x86_64、SimNow 有效账户和能在当前时段提供行情的合约。设置 `YQUANT_CTP_BROKER_ID`、`YQUANT_CTP_USER_ID`、`YQUANT_CTP_PASSWORD`，前置与合约分别设置为 `YQUANT_CTP_MD_FRONT`、`YQUANT_CTP_SYMBOL`。密码只从环境读取。前置使用账户提供方指定的行情地址。允许空登录的公开行情前置可显式指定 `--anonymous`：SDK 收到空登录字段，即使环境中配置了账户也不会发送凭据。账户登录失败不会自动切换模式。

```sh
cmake --build build --target verify_simnow_market
# 或指定采样条数和超时：
./scripts/verify_simnow_market --probe build/simnow_market_probe --count 3 --timeout 60
```

脚本在临时目录中启动 `simnow_market_probe [--anonymous] FRONT SYMBOL COUNT TIMEOUT_SECONDS`，分别核对行情和意图的内部标识、序号及买卖价，并检查有效数量和汇总条数。此单合约程序使用内部标识 1。默认取 1 条、超时 30 秒；脚本另设进程退出时限。直接运行行情探测程序时，SDK 使用进程当前目录作为默认流文件目录。

多合约可控验收运行 `ctest --test-dir build -R '^v1_07_multi_market$' --output-on-failure`。独立只读程序支持显式指定各合约的内部 ID：

```sh
probe="$PWD/build/verify_simnow_multi_market"
flow_dir=$(mktemp -d)
(cd "$flow_dir" && "$probe" --anonymous tcp://182.254.243.31:30011 20 TF2612 42 T2612 81)
```

程序只有在全组就绪且每个合约至少收到一条行情后才返回 0。全部场景通过同一行情 service 和 client 分发。

真实连接结果见[SimNow 行情连接验收记录](../verification/simnow-market.md)。
