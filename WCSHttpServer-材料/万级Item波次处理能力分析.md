# 万级 Item 波次处理能力分析

> 每次任务仅下发一条波次报文，内部 items[] 数组最多约 10,000 条。
> 单条 item 约 200 字节，完整 JSON body 约 2MB。

---

## 一、系统整体架构全景图

```mermaid
flowchart TB
    subgraph External["外部系统"]
        WMS["WMS 系统\n推送波次 + 绑定格口"]
        RFID["RFID 服务\n推送小车号 + SKU-EPC绑定查询"]
        PLC["PLC 设备\nTCP 连接 :8192\n接收分拣指令"]
    end

    subgraph WCS["WCS_httpServer (端口 8191)"]
        subgraph HP["HP-Socket HTTP Server (16 I/O Worker)"]
            OnBody["OnBody 分块接收"]
            OnComplete["OnMessageComplete 组装"]
            Router["路由分发"]
        end

        subgraph Queue["TaskQueue (容量=5)"]
            Push["入队 WaveTask"]
        end

        subgraph ParseW["ParseWorker 异步线程 ×1"]
            Parse["JSON 解析 + 遍历 items"]
            Build["构建 GridBuffer QMap"]
            Swap["DoubleBuffer 原子交换"]
            Log["WAVE_ITEM 日志"]
        end

        subgraph Main["主线程 (信号处理)"]
            WaveMgr["WaveManager 状态管理"]
            DB["SortingDatabase 持久化"]
            EPC["EpcCache EPC缓存"]
            HttpClient["HttpClient RFID查询"]
            PlcSend["PlcManager PLC发送"]
        end

        subgraph Pools["业务线程池"]
            Business["BusinessPool ×90\n回传/满箱/完结"]
            PlcRecv["PlcRecvPool ×4\nPLC反馈/数据库写入"]
        end
    end

    WMS -->|"POST InsertWaveInfo\n~2MB JSON"| OnBody
    RFID -->|"POST /api/rfid/carNumReport\n主动推送"| OnBody
    OnBody --> OnComplete --> Router
    Router -->|"绑定/取消/查询"| Main
    Router -->|"波次下发"| Push
    Push --> Parse
    Parse --> Build --> Swap --> Log
    Swap -->|"emit waveParsed\nQt::QueuedConnection"| WaveMgr
    WaveMgr --> DB
    WaveMgr --> EPC
    WaveMgr -->|"submitEpcBindingQueries"| HttpClient
    HttpClient -->|"POST RFID查询"| RFID
    RFID -->|"EPC→barcode 映射"| HttpClient
    HttpClient -->|"emit rfidBindingResult"| EPC
    EPC -->|"就绪检查"| PlcSend
    PlcSend -->|"TCP 文本: {EPC|格口|小车号}"| PLC
    PLC -->|"落格反馈"| PlcRecv
    PlcRecv --> DB
    Business -->|"HTTP 回传 WMS\n满箱/完结"| WMS
```

---

## 二、波次下发完整数据流 (10,000 items)

```mermaid
sequenceDiagram
    autonumber
    participant WMS as WMS 系统
    participant HP as HP-Socket<br/>I/O Worker
    participant Q as TaskQueue
    participant PW as ParseWorker<br/>异步线程
    participant Main as 主线程
    participant DB as SQLite 数据库
    participant RFID as RFID 服务
    participant PLC as PLC 设备

    rect rgb(230, 245, 255)
        Note over WMS,HP: 阶段1: HTTP 接收 (~200ms)
        WMS->>HP: POST /api/DispatchSortingCommand/InsertWaveInfo
        Note over WMS: Body: ~2MB JSON<br/>items: 10,000 条
        loop 分块接收 (~16KB/块)
            HP->>HP: OnBody 追加数据块
        end
        HP->>HP: OnMessageComplete<br/>BOM剥离 + null过滤 + trimmed
        HP->>HP: QJsonDocument::fromJson (~50ms)
        HP->>HP: validateInsertWaveInfo 参数校验
        HP->>HP: areAllBindingsComplete 绑定检查
        HP->>HP: 幂等检查 (已分拣拒绝)
    end

    rect rgb(255, 245, 230)
        Note over HP,Q: 阶段2: 入队 (~1ms)
        HP->>Q: push(WaveTask) 入队
        HP-->>WMS: {"code":"200","message":"","sentTime":"..."}
        Note over WMS: HTTP 响应立即返回<br/>后续处理异步进行
    end

    rect rgb(230, 255, 230)
        Note over Q,PW: 阶段3: ParseWorker 异步解析 (~1s)
        Q->>PW: pop() 取任务
        PW->>PW: JSON 解析 + 遍历 10,000 items
        loop 10,000 次循环
            PW->>PW: 构建 GridEntry<br/>(inco→gridNum/gridType/volu等)
            PW->>PW: 同品多格口合并
            PW->>PW: WAVE_ITEM 日志写入
            PW->>PW: 收集 epcList
        end
        PW->>PW: 回填 skuCount
        PW->>PW: DoubleBuffer 原子交换
        PW->>Main: emit waveParsed<br/>(Qt::QueuedConnection)
    end

    rect rgb(255, 230, 255)
        Note over Main,DB: 阶段4: 主线程落库 (~500ms)
        Main->>Main: WaveManager.setWaveData
        Main->>Main: 自动推进 CREATED→BOUND→SORTING
        Main->>DB: upsertReturnWave 波次头
        loop 10,000 行
            Main->>DB: INSERT return_wave_item
        end
        Main->>Main: submitEpcBindingQueries(10,000 EPCs)
    end

    rect rgb(255, 255, 200)
        Note over Main,RFID: 阶段5: RFID SKU-EPC 绑定查询 (~3s)
        Main->>RFID: POST /open-api/rfid/query<br/>{"epcList":["EPC001",...]}
        RFID-->>Main: {"data":{"data":[{"epc":"..","barcode":".."},...]}}
        Main->>Main: onRfidBindingResult<br/>逐条 setSkuBinding(skuBound=true)
        Main->>Main: 就绪检查 → trySendToPlcForEpc
    end

    rect rgb(255, 230, 230)
        Note over Main,PLC: 阶段6: RFID 推送小车号 → PLC 发送
        RFID->>Main: POST /api/rfid/carNumReport<br/>{"data":[{"epc":"..","carNum":"002"},...]}
        Main->>Main: handleRfidCarNumReport<br/>setBatchWithCar (carNum)
        Main->>Main: 就绪检查 → trySendToPlcForEpc
        loop 10,000 条 × 1~5ms
            Main->>PLC: TCP 文本: {EPC|格口|小车号}
        end
    end
```

---

## 三、各阶段耗时分析

```mermaid
gantt
    title 10,000 Item 波次处理时间线 (总耗时 ~15~60s)
    dateFormat X
    axisFormat %s

    section ① HTTP 接收
    HP-Socket 分块接收 2MB body     :crit, a1, 0, 200
    JSON 解析 2MB                   :a2, after a1, 50
    参数校验 + 绑定检查              :a3, after a2, 5

    section ② 入队响应
    入队 + 返回 HTTP 200             :done, b1, after a3, 1

    section ③ ParseWorker 解析
    遍历 10,000 items 构建 GridEntry :c1, after b1, 100
    WAVE_ITEM 10,000条日志写入       :c2, after b1, 1000
    DoubleBuffer 原子交换            :c3, after c2, 1

    section ④ 主线程落库
    WaveManager 状态设置             :d1, after c3, 1
    upsertReturnWave 波次头          :d2, after d1, 10
    insertWaveItems 10,000行         :d3, after d2, 500

    section ⑤ RFID 查询
    HTTP 请求发送                    :e1, after d3, 50
    RFID 服务处理 10,000 EPCs        :e2, after e1, 3000
    onRfidBindingResult 逐条缓存     :e3, after e2, 100

    section ⑥ PLC 发送
    TCP 逐条发送 10,000条            :crit, f1, after e3, 10000
```

| 阶段 | 数据量 | 预估耗时 | 是否阻塞HTTP响应 | 瓶颈？ |
|------|--------|----------|:---:|--------|
| ① HTTP 接收 | 2MB body | ~200ms | 是 | ❌ 无 |
| ① JSON 解析 | 2MB JSON | ~50ms | 是 | ❌ 无 |
| ② 入队响应 | 1条 | ~1ms | 是 | ❌ 无 |
| ③ ParseWorker 遍历 | 10,000 条目 | ~100ms | 否 | ❌ 无 |
| ③ WAVE_ITEM 日志 | 10,000 条 | ~1s | 否 | ⚠️ 可优化 |
| ④ 数据库插入 | 10,000 行 | ~500ms | 否(主线程) | ⚠️ 可优化 |
| ⑤ RFID 查询 | 10,000 EPCs | ~3s | 否 | ⚠️ 依赖RFID |
| **⑥ PLC TCP 发送** | **10,000 条** | **~10~50s** | **否** | **🔴 最大瓶颈** |

---

## 四、ParseWorker 解析流程详解

```mermaid
flowchart TB
    subgraph Input["输入"]
        Raw["WaveTask.rawBody\n~2MB JSON"]
        FullUrl["WaveTask.fullUrl\n完整请求URL"]
    end

    subgraph Clean["数据清洗"]
        BOM["剥离 UTF-8 BOM\n(EF BB BF)"]
        Null["移除 null 字节\n(\\x00)"]
        Trim["去除首尾空白"]
        Parse["QJsonDocument::fromJson"]
    end

    subgraph Loop["遍历 items[] 数组 (10,000 次)"]
        direction TB
        Extract["提取字段:\ninco, gridNum, gridType\ngridNumber, volu, obxCode\nepcn (EPC值)"]
        Collect["收集 epcList\nepcn 优先, inco 兜底"]
        Build["构建 GridEntry"]
        Merge{"同品多格口?"}
        Merge -->|是| Combine["合并 gridNum\n逗号拼接"]
        Merge -->|否| Insert["newMap.insert(epc, entry)"]
        Combine --> Insert
        Insert --> WaveLog["WAVE_ITEM 日志\n单条 item JSON\n写入 wave_item.log"]
    end

    subgraph Final["收尾"]
        SkuCount["回填 skuCount\n= newMap.size()"]
        Swap["DoubleBuffer\nprepareSwap 原子交换"]
        Emit["emit waveParsed\n→ 主线程"]
    end

    Input --> Clean --> Parse
    Parse -->|解析成功| Loop
    Parse -->|解析失败| Error["emit parseError"]
    Loop --> Final
    Final --> Emit

    subgraph Time["耗时分析"]
        T1["遍历: ~100ms"]
        T2["日志: ~1000ms"]
        T3["交换: ~1ms"]
    end
```

### 单条 item 处理详情

```mermaid
flowchart LR
    subgraph Item["单个 item JSON"]
        J["{obxCode, inco, epcn,\n gridNum, gridNumber,\n gridType, volu}"]
    end

    subgraph Processing["处理步骤"]
        S1["① 提取字段值"]
        S2["② 校验 inco + gridNum 非空"]
        S3["③ 收集 EPC (epcn或inco)"]
        S4["④ 同品多格口合并"]
        S5["⑤ WAVE_ITEM 日志"]
        S6["⑥ 构建 GridEntry"]
    end

    subgraph Result["产出"]
        R1["epcList 追加"]
        R2["newMap[epc] = GridEntry"]
        R3["wave_item.log 一条"]
    end

    J --> S1 --> S2 --> S3 --> S4 --> S5 --> S6
    S3 --> R1
    S6 --> R2
    S5 --> R3
```

---

## 五、GridBuffer 双缓冲机制

```mermaid
flowchart TB
    subgraph Active["activeMap (当前生效)"]
        A1["inco: AAAAA → gridNum: 1,2<br/>gridType: 普通格口<br/>gridCount: 100<br/>orderCode: PB202203200003"]
        A2["inco: BBBBB → gridNum: 3<br/>gridType: 普通格口<br/>gridCount: 50<br/>orderCode: PB202203200003"]
        A3["... 10,000 条"]
    end

    subgraph Standby["standbyMap (待交换)"]
        B1["ParseWorker 构建新 Map"]
        B2["遍历 items 填充"]
        B3["同品多格口合并"]
    end

    subgraph Swap["原子交换"]
        C["prepareSwap(newMap)\n① 旧 activeMap 移入待清理队列\n② newMap 置为 activeMap\n③ 5秒后清理旧 Map"]
    end

    subgraph Reader["读取方"]
        R1["PLC 落格查格口\nm_pBuffer->get(code)"]
        R2["数据库 insertWaveItems\nm_pBuffer->activeMap()"]
        R3["RFID 小车号推送\nm_pBuffer->get(epc)"]
    end

    Standby --> Swap --> Active
    Active --> Reader
```

```mermaid
sequenceDiagram
    participant PW as ParseWorker
    participant GB as GridBuffer
    participant Old as 旧 activeMap
    participant New as 新 activeMap
    participant Reader as 读取方

    PW->>GB: prepareSwap(newMap)
    GB->>Old: 旧 activeMap 移入待清理队列
    GB->>New: newMap 置为 activeMap
    Note over GB: 原子操作完成

    Reader->>GB: get(code) 查格口
    GB->>New: 返回新 Map 数据
    Note over Reader: 读取方立即看到新数据

    Note over Old: 5秒后延迟清理
    GB->>Old: delete 释放内存
```

---

## 六、WAVE_ITEM 日志线程详解

```mermaid
flowchart TB
    subgraph Producer["ParseWorker 线程 (生产者)"]
        P1["遍历 items[]"]
        P2["构建单条 item JSON"]
        P3["WAVE_ITEM_INFO 宏"]
        P4["hlog_format 写入\n./log/WAVE_ITEM/wave_item.log"]
    end

    subgraph Format["日志格式"]
        F1["[时间] [TID-xxx] [WAVE_ITEM]"]
        F2["[原始报文]"]
        F3["http://192.168.3.53:8191/api/DispatchSortingCommand/InsertWaveInfo"]
        F4["body={orderCode,orderQty,items:[{...}]}(字节数)"]
    end

    subgraph Example["单条日志示例"]
        E1["[2026-08-11 10:30:01.234] [TID-35316] [WAVE_ITEM]"]
        E2["	[原始报文] http://192.168.3.53:8191/api/DispatchSortingCommand/InsertWaveInfo"]
        E3["	body={""orderCode"":""PB202203200003"",""orderQty"":12312,""items"":[{""obxCode"":""BOX202203200080"",""inco"":""WV34S1CN2001B60L"",""epcn"":""AC78A59C0C4010A22811029240003546"",""gridNum"":""1"",""gridNumber"":100,""gridType"":""普通格口"",""volu"":""1234""}]}(245字节)"]
    end

    Producer --> Format --> Example
```

### 日志写入性能分析

```mermaid
flowchart LR
    subgraph Current["当前: 逐条写入"]
        C1["10,000 次\nhlog_format 调用"]
        C2["每次: 打开→写入→关闭"]
        C3["磁盘 I/O: ~1~2s"]
        C1 --> C2 --> C3
    end

    subgraph Impact["影响评估"]
        I1["HTTP 响应: 不受影响 ✅"]
        I2["ParseWorker: 阻塞 1~2s ⚠️"]
        I3["主线程: 不受影响 ✅"]
        I4["后续波次: 需等 ParseWorker 完成 ⚠️"]
    end

    Current --> Impact
```

---

## 七、数据库写入流程

```mermaid
flowchart TB
    subgraph Input["waveParsed 信号触发"]
        I1["orderCode: PB202203200003"]
        I2["skuCount: 去重后 EPC 种类数"]
        I3["activeMap: 10,000 条 GridEntry"]
    end

    subgraph WaveHead["波次头 (UPSERT)"]
        W1["upsertReturnWave"]
        W2["INSERT OR REPLACE INTO\nreturn_wave (order_code, order_qty, status)"]
        W3["耗时: ~10ms"]
    end

    subgraph WaveItems["波次明细 (INSERT)"]
        D1["遍历 activeMap 10,000 条"]
        D2["拆分一品多格口\n(逗号分隔)"]
        D3["逐条 INSERT INTO\nreturn_wave_item"]
        D4["耗时: ~500ms\n(主线程, 无显式事务)"]
    end

    subgraph Tables["数据库表结构"]
        T1["return_wave\n波次头: order_code, order_qty, status, create_time"]
        T2["return_wave_item\n波次明细: order_code, inco, grid_num, grid_type, plan_qty, volu, obx_code"]
        T3["sorting_records\n分拣记录: order_code, barcode, grid_num, car_num, grid_count, volu, sort_time"]
    end

    Input --> WaveHead --> WaveItems
    WaveHead --> T1
    WaveItems --> T2
    WaveItems --> T3
```

### 数据库写入性能

```mermaid
flowchart LR
    subgraph Perf["当前性能 (WAL模式)"]
        P1["10,000 行 INSERT"]
        P2["逐条 exec, 无事务"]
        P3["~500ms"]
    end

    subgraph Opt["优化方向 (事务包裹)"]
        O1["BEGIN TRANSACTION"]
        O2["10,000 行 INSERT"]
        O3["COMMIT"]
        O4["~50ms (10x提升)"]
    end

    subgraph Risk["当前风险"]
        R1["主线程执行"]
        R2["阻塞 500ms"]
        R3["UI 可能卡顿"]
        R4["但不影响 HTTP 响应"]
    end

    Perf --> Risk
    Opt -.->|"建议优化"| Perf
```

---

## 八、RFID 双通道数据流

```mermaid
flowchart TB
    subgraph Channel1["通道1: SKU-EPC 绑定查询 (WCS→RFID)"]
        C1A["波次下发后\nsubmitEpcBindingQueries\n10,000 EPCs"]
        C1B["POST /open-api/rfid/query\n{epcList: [...]}"]
        C1C["RFID 响应\n{data:{data:[{epc,barcode}]}}"]
        C1D["onRfidBindingResult\n逐条 setSkuBinding"]
        C1E["EpcCache 状态:\nskuBound=true"]
        C1A --> C1B --> C1C --> C1D --> C1E
    end

    subgraph Channel2["通道2: 小车号推送 (RFID→WCS)"]
        C2A["RFID 主动推送\nPOST /api/rfid/carNumReport"]
        C2B["{data:[{epc,barcode,carNum}]}"]
        C2C["handleRfidCarNumReport\nsetBatchWithCar"]
        C2D["EpcCache 状态:\ncarNum=已设置"]
        C2A --> C2B --> C2C --> C2D
    end

    subgraph Check["就绪判断"]
        CH1["isReadyForPlc(epc)"]
        CH2{"skuBound=true\n&& carNum非空?"}
        CH3["是 → trySendToPlcForEpc"]
        CH4["否 → 等待另一通道完成"]
        CH1 --> CH2
        CH2 -->|是| CH3
        CH2 -->|否| CH4
    end

    C1E --> Check
    C2D --> Check
    CH3 --> PLC["PLC TCP 发送"]
```

### EpcCache 两阶段就绪模型

```mermaid
stateDiagram-v2
    [*] --> Empty: EPC 首次出现

    Empty --> SkuBound: RFID 绑定查询返回\nsetSkuBinding(epc, barcode)
    Empty --> CarNumSet: RFID 推送小车号\nsetBatchWithCar({epc, barcode, carNum})

    SkuBound --> Ready: RFID 推送小车号\nsetBatchWithCar
    CarNumSet --> Ready: RFID 绑定查询返回\nsetSkuBinding

    Ready --> Sent: trySendToPlcForEpc\n发送 PLC 指令

    Sent --> [*]: TTL 过期自动清理
    Empty --> [*]: TTL 过期自动清理
    SkuBound --> [*]: TTL 过期自动清理
    CarNumSet --> [*]: TTL 过期自动清理

    note right of Ready
        isReadyForPlc() = true
        skuBound=true && carNum非空
    end note

    note right of SkuBound
        等待 RFID 推送 carNum
        超时后用 DEFAULT_CAR_NUM
    end note
```

---

## 九、PLC TCP 发送流程

```mermaid
flowchart TB
    subgraph Trigger["触发条件"]
        T1["RFID 绑定查询返回\n→ trySendToPlcForEpc"]
        T2["RFID 推送小车号\n→ trySendToPlcForEpc"]
        T3["就绪条件:\nskuBound=true && carNum非空"]
    end

    subgraph Build["消息构建"]
        B1["从 EpcCache 获取:\nbarcode + carNum"]
        B2["从 GridBuffer 获取:\ngridNum (用 epc 查找)"]
        B3["组装 TCP 消息:\n{EPC|格口|小车号}"]
        B4["示例:\n{WV34S1CN2001B60L|001|002}"]
    end

    subgraph Send["发送"]
        S1["sendBatchCodesWithEpcCache"]
        S2["sendToClient\n逐条发送"]
        S3["格式: 3位零填充格口/小车号"]
    end

    subgraph Result["结果"]
        R1["发送成功 → 等待 PLC 落格反馈"]
        R2["发送失败 → LIFECYCLE 日志\n记录失败原因"]
    end

    Trigger --> Build --> Send --> Result
```

### PLC 发送耗时模型

```mermaid
flowchart LR
    subgraph Scenarios["不同网络环境"]
        S1["本地回环\n1ms/条 → ~10s"]
        S2["局域网\n2ms/条 → ~20s"]
        S3["慢速 PLC\n5ms/条 → ~50s"]
    end

    subgraph Note["注意"]
        N1["发送是异步的"]
        N2["不阻塞 HTTP 响应"]
        N3["不阻塞主线程"]
        N4["任务队列容量=5\n防止积压"]
    end

    Scenarios --> Note
```

---

## 十、内存占用全景

```mermaid
pie title 10,000 Item 波次内存占用 (~15MB)
    "JSON raw body (2MB)" : 2
    "QJsonDocument 解析 (4MB)" : 4
    "GridBuffer QMap (3MB)" : 3
    "EpcCache QMap (2MB)" : 2
    "WAVE_ITEM 日志缓冲 (1MB)" : 1
    "数据库连接+其他 (3MB)" : 3
```

| 数据结构 | 单条目 | ×10,000 | 总计 |
|----------|--------|---------|------|
| Raw JSON body | - | - | ~2MB |
| QJsonDocument | - | - | ~4MB |
| GridBuffer (QMap) | ~300B | 10,000 | ~3MB |
| EpcCache | ~200B | 10,000 | ~2MB |
| 日志缓冲 | ~100B | 10,000 | ~1MB |
| **合计** | | | **~12MB** |

> 内存占用完全可控，远低于 16GB 物理内存。

---

## 十一、线程模型与并发安全

```mermaid
flowchart TB
    subgraph Threads["线程模型"]
        direction TB
        T1["主线程 (1个)\n- HTTP 路由分发\n- waveParsed 信号处理\n- UI 日志刷新\n- 状态管理"]
        T2["HP-Socket I/O Worker (16个)\n- OnBody 分块接收\n- OnMessageComplete 组装\n- 路由分发"]
        T3["ParseWorker (1个)\n- JSON 解析\n- GridBuffer 构建\n- WAVE_ITEM 日志"]
        T4["BusinessPool (90个)\n- 回传 JSON 构建\n- 满箱/完结处理\n- 异常处理"]
        T5["PlcRecvPool (4个)\n- PLC 反馈解析\n- 分拣记录写入\n- 数据库跨线程操作"]
    end

    subgraph Safety["线程安全机制"]
        direction TB
        S1["DoubleBuffer\nprepareSwap 原子交换\n读写完全隔离"]
        S2["EpcCache\nQMutex 保护\n所有读写加锁"]
        S3["ContainerBindings\nstd::mutex 保护\nm_containerMutex"]
        S4["GridSortRecords\nstd::mutex 保护\nm_gridRecordMutex"]
        S5["Qt::QueuedConnection\n跨线程信号安全投递"]
        S6["SortingDatabase\nensureConnection()\n每个线程独立连接"]
    end

    T3 --> S1
    T1 --> S2
    T1 --> S3
    T1 --> S4
    T3 --> S5
    T5 --> S6
```

---

## 十二、HTTP 请求生命周期

```mermaid
sequenceDiagram
    participant WMS as WMS 系统
    participant HP as HP-Socket
    participant Main as 主线程
    participant Q as TaskQueue
    participant PW as ParseWorker

    WMS->>HP: POST /api/DispatchSortingCommand/InsertWaveInfo
    Note over HP: Content-Length: ~2,000,000

    activate HP
    loop 分块接收 body
        HP->>HP: OnBody(chunk) 追加
    end
    HP->>HP: OnMessageComplete
    HP->>HP: BOM剥离 + null过滤 + trimmed
    HP->>HP: QJsonDocument::fromJson

    alt JSON 解析失败
        HP-->>WMS: {"code":"500","message":"JSON解析失败: ..."}
    else 参数校验失败
        HP->>HP: validateInsertWaveInfo
        HP-->>WMS: {"code":"500","message":"..."}
    else 绑定不完整
        HP->>HP: areAllBindingsComplete
        HP-->>WMS: {"code":"500","message":"格口绑定不完整"}
    else 幂等拒绝 (已分拣)
        HP->>HP: 检查波次状态
        HP-->>WMS: {"code":"500","message":"波次已开始分拣"}
    else 队列已满
        HP->>HP: 检查 queue.size()
        HP-->>WMS: {"code":"500","message":"服务器繁忙"}
    else 成功
        HP->>Q: push(WaveTask)
        HP-->>WMS: {"code":"200","message":"","sentTime":"..."}
        deactivate HP

        Q->>PW: pop()
        activate PW
        PW->>PW: 遍历 10,000 items
        PW->>PW: 构建 GridBuffer
        PW->>PW: WAVE_ITEM 日志
        PW->>Main: emit waveParsed (QueuedConnection)
        deactivate PW

        activate Main
        Main->>Main: WaveManager 状态管理
        Main->>Main: 数据库落库
        Main->>Main: RFID 查询
        deactivate Main
    end
```

---

## 十三、综合评估

```mermaid
flowchart LR
    subgraph OK["✅ 可正常处理"]
        O1["HTTP 接收 2MB ✅"]
        O2["JSON 解析 2MB ✅"]
        O3["GridBuffer 10K ✅"]
        O4["内存 ~15MB ✅"]
        O5["数据库 10K 行 ✅"]
        O6["并发安全 ✅"]
    end

    subgraph WARN["⚠️ 需关注"]
        W1["WAVE_ITEM 日志\n10K 条 ≈ 1~2s"]
        W2["RFID 查询\n10K EPCs 可能超时"]
        W3["数据库插入\n主线程 ~500ms"]
    end

    subgraph CRIT["🔴 主要瓶颈"]
        C1["PLC TCP 发送\n10K 条 ≈ 10~50s"]
    end
```

### 最终结论

| 维度 | 评估 | 说明 |
|------|:---:|------|
| 能否接收 | ✅ | HP-Socket 无 body 大小限制，2MB 完全可接收 |
| 能否解析 | ✅ | QJsonDocument 处理 2MB JSON < 50ms |
| 能否存储 | ✅ | GridBuffer 10K 条目，内存 ~3MB |
| 能否落库 | ✅ | SQLite WAL 模式，10K 行 ~500ms |
| 能否发送 PLC | ✅ | 逐条 TCP 发送，异步不阻塞 |
| HTTP 响应时间 | ✅ | < 300ms 立即返回，不等待后续处理 |
| 整体耗时 | ⚠️ | ~15~60 秒，取决于 PLC 网络速度和 RFID 响应 |
| 用户体验 | ⚠️ | HTTP 响应立即返回，但 PLC 发送需 10~50s |

### 系统可以处理 10,000 item 的波次报文

核心瓶颈在 PLC 逐条 TCP 发送（10~50 秒），但这是业务特性决定的：
- **不影响 HTTP 响应**：响应在入队后立即返回（~300ms），WMS 不会超时
- **不影响系统稳定性**：任务队列容量 5 防止内存溢出，双缓冲保证读写安全
- **不影响后续波次**：单次任务只有一条波次，无排队问题

### 优化建议优先级

```mermaid
flowchart LR
    P1["P0: WAVE_ITEM 日志\n改为批量写入\n效果: 1~2s → 100ms"] --> P2["P1: 数据库事务包裹\n效果: 500ms → 50ms"]
    P2 --> P3["P2: RFID 分批查询\n每批 1000 条\n效果: 避免单次超时"]
    P3 --> P4["P3: PLC 发送优化\n(需业务确认)\n效果: 10~50s → 更短"]
```