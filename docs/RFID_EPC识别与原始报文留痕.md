# RFID 推送 EPC 码识别（A+23 位数字）与 原始报文留痕 —— 实施/运维说明

> 适用版本：2026-09-15（需求：① RFID 推送的 EPC 码按配置长度识别，XML 可配，默认 24 位字符；② 保留 RFID 推送的原始报文）
> 相关文件：`WCS_httpServer/EpcCode.h`、`RfidPushClient.cpp/.h`、`ConfigManager.cpp/.h`、`define.h`、
> `SortingDatabase.cpp/.h`、`HttpServer.cpp/.h`、`MainWindow.cpp/.h`、`tests/*`
> 面向对象：现场实施/运维、后续接手开发的同事

---

## 一、需求与客户口径

### 1.1 需求原文

1. 将 RFID 推送的 EPC 码设置长度截断，配置在 XML 里，默认 24 位字符。
2. 保留 RFID 推送的原始报文。

### 1.2 客户补充口径（2026-09-15 现场确认）

> **「不管开头是不是 A101，只认识 A + 23 位数字组成的 EPC 码」**

由此确定 EPC 的**识别规则**（不是简单"取前 N 位"、也不假定 EPC 就在串首）：

- **形态**：大写字母 `A` + `(长度-1)` 位 ASCII 数字；长度 = XML `rfidEpcTruncateLen`，默认 **24**（即 `A` + 23 位数字）。
- **做法**：在 RFID 推送串中**定位**该形态并取出——附加数据在尾部、杂串在头部都能正确取出真实 EPC。
- **不猜**：串中不存在该形态时，**按原文继续处理并告警**（绝不臆造 EPC）；原文另有留痕可查。
- `A101` 只是现场当前的号段前缀，**不作为判定依据**。

### 1.3 现场样例对照（客户提供）

| 输入（RFID 推送串中的 EPC 字段） | 识别结果 | 说明 |
| --- | --- | --- |
| `A10126010300853174002539`（24 位，成功件） | `A10126010300853174002539`（原样） | `A` + 23 位数字，形态正确，不归一 |
| `A1012501020087626498017735303032`（32 位，失败件） | `A10125010200876264980177` | 尾部 8 位 `35303032` 是 ASCII `"5002"` 的十六进制附加数据，被丢弃 |
| `0000A101250102008762649801779999` | `A10125010200876264980177` | 头部/尾部杂串被丢弃（定位式识别） |
| `EPC001` / `123456789012345678901234567890` | 原样保留 + 告警 | 不含 `A`+23 位数字形态 → 不猜，按原文继续 |
| `NOREAD` | 不识别（仅留痕） | 未读到标签，不进入分拣 |

---

## 二、配置项

| XML 元素 | 默认 | 取值语义 | 生效方式 |
| --- | --- | --- | --- |
| `rfidEpcTruncateLen` | `24` | EPC 码长度（字符数）：识别 **`A` + (长度-1) 位数字**；`< 2` = 不识别（整串原样使用，回退改造前行为） | 改配置后点「保存并生效」即**热生效**，无需重启 |

- 配置文件：`{exe}/config/http_server.xml`
- 取值非法（空/非数字/负数/越界）时**保持默认 24** 并在 run.log 告警（避免误配成"不识别"导致长串 EPC 查不到绑定）。
- 仓库内已同步的样例配置：`release_WcsHttpServer/config/http_server.xml`、`mock_env/config_mock/http_server.xml`
- 启动横幅与 run.log 会打印当前生效口径（现场一眼确认）：

```
 RFID推送服务端:  192.168.100.125:2010 (WCS主动连接)
 EPC识别:         只识别『A + 23 位数字』共 24 位（rfidEpcTruncateLen；与开头是不是 A101 无关）
 原始报文留痕:    日志(log/Run/run.log) + 界面(实时面板悬停/EPC全信息弹窗) + 数据库(rfid_raw 表，保留 7 天)
```

---

## 三、识别口径与作用范围

### 3.1 统一口径

- 识别发生在**帧解析处**（`RfidPushClient::OnReceive`，实现见 `EpcCode.h`），因此下游**全链路同一口径**：

| 环节 | 使用的 EPC |
| --- | --- |
| EpcCache 缓存键 / 在途判定 / 防重 | 识别后 |
| RFID SKU-EPC 绑定查询（HTTP 请求 `epcList`） | 识别后 |
| 绑定查询**响应**里的 `epc`（`HttpClient`） | 同一规则归一后再入缓存（防"响应回整串、缓存键是归一串"导致查不到绑定） |
| PLC 下发指令 / 落格反馈匹配 | 识别后 |
| 落库（`sorting_records.barcode`、`exception_record.epc`、`rfid_raw.epc`） | 识别后 |
| 界面显示（实时面板 EPC 列、查询结果） | 识别后 |

- 识别**只改"参与分拣的 EPC"**；识别前的 EPC 原文（`epcRaw`）与整帧原文（`raw`）继续全链路留痕，原文不丢失（见第四节）。
- 形态正确时行为完全不变（`normalized=false`），原文照留。
- 另：`handleRfidCarNumReport` 同时服务「TCP 推送」与「HTTP 直推」两个入口，HTTP 直推（不带 `raw` 字段）会在此再识别一次，保证两入口口径一致（对已归一的串是幂等操作）。

### 3.2 运行时日志

识别归一发生时（run.log，同时逐帧可见）：

```
[WARN ] EPC已识别归一 seq=SN0098 car=98 原文(32位)=A1012501020087626498017735303032 → EPC(24位)=A10125010200876264980177 丢弃前缀(0)= 丢弃后缀(8)=35303032 原始报文={SN0098|01|A1012501020087626498017735303032}0D
[INFO ] [解析] 数据帧 seq=SN0098 dev=01 car=98 epc=A10125010200876264980177(原文 A1012501020087626498017735303032) → 交业务处理 原始报文={SN0098|01|A1012501020087626498017735303032}0D
```

串中不含该形态时（按原文继续，不臆造）：

```
[WARN ] EPC未识别(24位=A+23位数字) seq=SN0041 car=41 原文=EPC001 原始报文={SN0041|01|EPC001}0D —— 按原文继续处理
```

---

## 四、原始报文保留（三层，互为备份）

| 层 | 位置 | 内容 | 特点 |
| --- | --- | --- | --- |
| ① 日志 | `{exe}/log/Run/run.log` | `[原始报文]` 整块原文（text + hex）；逐帧 `[解析]`/`EPC已识别归一` 行带整帧原文 | 排查最快；**已取消原 512 字节截断**（仅单次 >64KB 的异常块才截断并标注） |
| ② 界面 | 运行日志页 / 实时面板 / EPC 全信息弹窗 | 逐帧 `RFID数据帧 … 原始报文={…}0D`；实时面板 EPC 单元格**鼠标悬停提示**显示整帧原文；弹窗「RFID原始报文」页签列出该 EPC 全部原始帧 | 现场无需翻日志即可看到原文 |
| ③ 数据库 | `{exe}/data/sorting_records.db` 表 `rfid_raw` | 逐帧结构化留痕（时间/识别后 EPC/识别前 EPC/车号/流水号/设备编码/整帧原文/字节数/NOREAD 标记） | 可长期追溯、可按 EPC 反查、可导出核对 |

### 4.1 `rfid_raw` 表结构

| 列 | 含义 |
| --- | --- |
| `id` | 自增主键（时间顺序） |
| `time` | 落库时间 `yyyy-MM-dd HH:mm:ss` |
| `epc` | **识别归一后**用于分拣的 EPC（空 = 该帧为 NOREAD 未读到标签） |
| `epc_raw` | **识别前**的 EPC 原文（未归一/未识别时与 `epc` 相同） |
| `car_num` / `seq` / `dev_code` | 帧内解析出的小车号 / 流水号原文 / 设备编码 |
| `raw_frame` | **RFID 原样推送的整帧报文**（含帧头 `{`、帧尾 `}` 与协议字面帧尾 `0D`） |
| `bytes` | 整帧字节数 |
| `noread` | `1` = NOREAD 帧（仅留痕，不进入分拣） |

索引：`idx_rfid_raw_epc(epc)`、`idx_rfid_raw_epc_raw(epc_raw)`、`idx_rfid_raw_time(time)`。

### 4.2 落库策略（不影响 RFID 主链路）

- 业务入口（主线程）只做**入队**（O(1)，不写盘）；由定时器/满批触发**单事务批量写入**：
  - 满 `RFID_RAW_FLUSH_MAX_ROWS`(200) 帧立即落库；
  - 否则每 `RFID_RAW_FLUSH_INTERVAL_MS`(1000ms) 落一次；
  - 程序退出时（RFID 客户端停掉后）**补写残留队列**并打印累计统计。
- 队列上限 `RFID_RAW_PENDING_MAX_ROWS`(20000) 帧：超限丢最旧并告警（防 DB 写不动时内存膨胀）。
- 保留期 `RFID_RAW_RETAIN_DAYS`(7) 天：启动时清一次，跨天运行在"队列空"的空闲拍再清（每天最多一次）。
- **实现细节（真实缺陷回归）**：落库前所有文本列做空串归一 —— Qt 会把 **null QString 绑定为 SQL NULL**，
  而列是 `NOT NULL`，不归一就会 `NOT NULL constraint failed` 而**静默丢帧**（NOREAD 帧的 `epc`、
  两段帧的 `dev_code`、流水号无数字时的 `car_num` 天然为空）。该用例已锁定在 `test_rfid_raw_db.cpp` ③b。

### 4.3 现场查库 SQL（只读）

```sql
-- 最近 20 帧原始报文（时间倒序）
SELECT time, epc, epc_raw, seq, car_num, raw_frame FROM rfid_raw ORDER BY id DESC LIMIT 20;

-- 某件的全部原始报文（识别前后都能命中，便于核对"丢弃了什么"）
SELECT time, epc, epc_raw, raw_frame FROM rfid_raw
 WHERE epc='A10125010200876264980177'
    OR epc_raw='A1012501020087626498017735303032' ORDER BY id DESC;

-- 所有发生过识别归一的帧（原文比识别结果长/不同）
SELECT time, epc, epc_raw, raw_frame FROM rfid_raw
 WHERE epc <> epc_raw ORDER BY id DESC LIMIT 50;

-- 读码格式异常（页面上看到的"EPC未识别"告警，原文在此核对）
SELECT time, epc, raw_frame FROM rfid_raw
 WHERE epc = epc_raw AND epc <> '' ORDER BY id DESC LIMIT 50;

-- 未读到标签的帧（NOREAD，仅留痕）
SELECT time, seq, car_num, raw_frame FROM rfid_raw WHERE noread=1 ORDER BY id DESC LIMIT 20;
```

---

## 五、界面怎么看

1. **运行日志页**：RFID 每帧一行，含识别前 EPC 原文与整帧原文
   `RFID数据帧 seq=SN0098 car=98 epc=A10125010200876264980177（原文 A1012501020087626498017735303032） 原始报文={SN0098|01|A1012501020087626498017735303032}0D`
2. **实时面板**：EPC 单元格文本仍是**参与分拣的 EPC**（占位行匹配依赖该文本，不能改），
   **鼠标悬停**即显示该帧原始报文与说明。
3. **EPC 全信息弹窗**（异常弹窗/查询结果双击行）：「RFID原始报文」页签列出该 EPC 的全部原始帧
   （同一 EPC 多行 = 双读/重扫重投；NOREAD 帧显示"未读到标签"）。

---

## 六、验证

### 6.1 自动化回归（可重复执行）

```
tests\run_tests.bat
```

- `tests/test_epc_recognize.cpp`：直接用真实 `RfidPushClient::OnReceive` 喂现场帧字节 —— **30 项断言**
  覆盖：32 位串尾部附加数据 → 识别出前 24 位 / 头部杂串 → 定位式识别 / 正常 24 位不归一 /
  `EPC001`、纯数字等非该形态原样保留（不误判）/ NOREAD 帧留痕 / TCP 分包合成 /
  粘包多帧各自原文 / `rfidEpcTruncateLen=0` 回退 / 长度可配（N=20）。
- `tests/test_rfid_raw_db.cpp`：真实 `SortingDatabase` —— **19 项断言**
  覆盖：建表 / 批量落库 / 按识别后与识别前 EPC 双通道回查 / 整帧原文一致 / NOREAD 帧留痕 /
  空字段帧落库（NOT NULL 归一回归）/ 超期清理只删保留期外的行。
- 产物写入 `.build_check\`（本地校验工作区，不入库），不影响 Visual Studio 增量编译。

### 6.2 现场验证步骤

1. 确认配置：`http_server.xml` 内 `rfidEpcTruncateLen=24`；启动后看启动横幅「EPC识别: 只识别『A + 23 位数字』…」。
2. 用模拟台推送一条现场失败件那样的 32 位串
   （`mock_env` 下 `python dev_probe.py --push-epc A1012501020087626498017735303032`），或直接在产线读一件超长标签。
3. 预期表现：
   - run.log 出现 `EPC已识别归一 … 原文(32位)=… → EPC(24位)=A10125010200876264980177 丢弃后缀(8)=35303032`；
   - 界面运行日志同一行可见原文；实时面板该行 EPC 为识别后的 24 位，悬停可见原文；
   - 该件若绑定存在 → SKU 查到、格口下发、正常落格（改造前会因整串查不到绑定而不落格）；
   - `SELECT time, epc, epc_raw, raw_frame FROM rfid_raw ORDER BY id DESC LIMIT 5;`
     可见整帧原文，`epc`=识别后 24 位、`epc_raw`=整串。
4. 若仍不落格：先按 `epc_raw` 在 `rfid_raw` 查原文核对形态——
   - 出现 `EPC未识别(24位=A+23位数字)` 告警 → 说明推送串不含 `A`+23 位数字（如实际号段前缀不是 `A`，
     或数字位数不同）→ 按现场实际形态调整 `rfidEpcTruncateLen`（数字位数 = 长度-1）；
   - 识别出的 EPC 正确但查不到绑定 → 属 RFID 系统的 EPC↔SKU 绑定数据问题，与本次改造无关，按原流程排查。

---

## 七、回滚与注意事项

- **回滚识别**：`rfidEpcTruncateLen` 改为 `0`（或任意 `< 2` 的值）即完全回退改造前行为（整串原样使用）；
  原始报文留痕继续生效。
- 识别只作用于 **RFID 推送帧里的 EPC 字段**，不改 WMS 下发的 SKU/计划数、不改 PLC 协议。
- 若现场号段不是 `A` 开头（例如改为 `B` 开头或长度变化），只需调整长度配置 + 扩展 `EpcCode.h` 的锚点字符即可，
  推送侧/绑定查询响应侧/测试同源，改一处全生效。
- 绑定查询响应侧的 EPC 归一与推送侧同源同参数：只调整一处配置即两边一致。
- `rfid_raw` 会随推送量增长（约 100 字节/帧 + 整帧原文），默认保留 7 天；需要长期保存时调整
  `RFID_RAW_RETAIN_DAYS`（`define.h`，改后需重启）。
- 原始报文的**唯一权威来源**是 `RfidPushClient` 解析出的 `raw` 字段：日志/界面/数据库三处都来自它，
  因此三处内容必然一致，可交叉核对。

---

## 八、涉及文件

| 文件 | 改动 |
| --- | --- |
| `WCS_httpServer/EpcCode.h`（新增） | EPC 识别单一实现源：定位「`A` + (N-1) 位数字」形态；推送侧与绑定查询响应侧共用 |
| `WCS_httpServer/define.h` | 新增 `RFID_EPC_TRUNCATE_LEN`（默认 24）、`RFID_RAW_*`（保留期/攒批/上限）宏、`rfid_raw` 建表与 SQL 宏 |
| `WCS_httpServer/ConfigManager.h/.cpp` | `AppConfig::rfidEpcTruncateLen` 读写（非法值保持默认并告警） |
| `WCS_httpServer/RfidPushClient.h/.cpp` | EPC 定位式识别 + `setEpcTruncateLen()` + 日志取消 512 截断 + 整帧原文提取（含帧尾 `0D`）+ NOREAD 帧上报 + JSON 带 `epcRaw/raw/devCode/normalized/noread` |
| `WCS_httpServer/SortingDatabase.h/.cpp` | `RfidRawRecord`、`insertRfidRawBatch`（单事务 + 空串归一）、`queryRfidRawByEpc`（双通道）、`cleanupOldRfidRaw`、`countRfidRaw`、建表与索引 |
| `WCS_httpServer/HttpServer.h/.cpp` | 原始报文队列 + 攒批落库定时器 + 退出补写 + 跨天清理；UI 运行日志带原文；HTTP 直推入口同口径识别 + 同表留痕；`setEpcTruncateLen` 应用（启动/热生效） |
| `WCS_httpServer/HttpClient.h/.cpp` | 绑定查询**响应** EPC 按同一规则归一（`setEpcTruncateLen`；防"响应回整串、缓存键是归一串"导致查不到绑定） |
| `WCS_httpServer/MainWindow.h/.cpp` | 实时面板 EPC 悬停显示原文；EPC 全信息弹窗新增「RFID原始报文」页签；热生效与启动横幅提示；启动清理 `rfid_raw` |
| `WCS_httpServer/WCS_httpServer.vcxproj(.filters)` | 登记新增头文件 `EpcCode.h` |
| `release_WcsHttpServer/config/http_server.xml`、`mock_env/config_mock/http_server.xml` | 补充 `rfidEpcTruncateLen` 配置样例 |
| `tests/test_epc_recognize.cpp`、`tests/test_rfid_raw_db.cpp`、`tests/run_tests.bat` | 可重复执行的自测（49 项断言） |
