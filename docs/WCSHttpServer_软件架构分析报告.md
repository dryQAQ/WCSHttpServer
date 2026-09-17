# WCSHttpServer 软件架构分析报告

- 分析日期：2026-09-12
- 项目目录：D:/WCS/WCSApp/WCS_httpServer
- 基线提交：75f66dd；分析对象为本次读取的工作区源码。
- 方法：构建清单核对、第一方源码阅读、调用与信号连接追踪、include 依赖图及强连通分量扫描、配置与测试脚本检查。
- 边界：本次未编译、未启动应用、未连接 WMS/RFID/PLC、未执行集成或压力测试。下文“已确认”指静态代码路径成立；故障发生频率、现场吞吐和二进制实际行为仍需隔离环境验证。
- 范围：以 WCS_httpServer/WCS_httpServer.vcxproj 纳入的实现和其头文件为准；src/ 是另一套未被该工程引用的旧实现。报告不复制配置中的密钥。
- 补充：第 7–8 节归纳设计理念、关键取舍和高技术价值；基于同一审阅范围，不将设计意图视为已验证的运行保证。
- 基线说明：补充期间工作区存在并行代码改动；本报告仍描述上一轮已审阅实现，新增模块及其集成未纳入本次架构复审。模块数量和规模统计对应原审阅基线，源码定位可能随后续修改漂移。

## 架构判断

当前系统是一个 **Windows 上的单进程 Qt Widgets 分拣控制应用**，同时承担操作界面、WMS 接口、RFID 接入、PLC 通信、波次调度、数据持久化和失败补传。已有模块拆分和部分异步隔离，但模块间的业务边界尚未被接口、独立构建单元或统一状态所有权约束。

最值得保留的是设备与任务接收的生命周期分离、数据库专用写线程、PLC 反馈批处理、EPC 在途控制、报文落库和人工恢复能力。最需要优先处理的是 **波次映射隔离、退出时的资源所有权、PLC 数据完整性、发送确认语义与持久化一致性**。

按同名头文件/实现文件合并，第一方共 **22 个模块、19,901 物理行**，包括注释、空行和遗留模块，不含供应商代码及 src/ 副本。HttpServer.cpp 为 5,501 行，MainWindow.cpp 为 3,635 行，SortingDatabase.cpp 为 2,122 行，PlcManager.cpp 为 1,229 行。前三者承担了大量跨层职责。

## 1. 系统上下文与外部依赖

### 1.1 系统边界

~~~mermaid
flowchart LR
    OP["现场操作员"]
    WMS["WMS / WMS 网关"]
    RFID["RFID 推送服务"]
    QUERY["RFID EPC→SKU 查询服务"]
    PLC["PLC / 分拣设备"]
    subgraph APP["WCSHttpServer.exe：同一个进程"]
        UI["MainWindow 操作界面"]
        CORE["HTTP 接收 + 波次/分拣编排"]
        OUT["HttpClient 回传与查询"]
        DEV["PlcManager / RfidPushClient"]
        DB["SortingDatabase"]
        UI --> CORE
        CORE --> OUT
        CORE <--> DEV
        CORE <--> DB
        UI --> DB
    end
    OP <--> UI
    WMS -->|"H4 波次 / H5 取消 / H6 绑定，HTTP POST"| CORE
    OUT -->|"H7 满箱 / H8 完结，HTTP(S)"| WMS
    RFID -->|"WCS 主动建立 TCP 后接收 EPC + carNum"| DEV
    OUT <-->|"HTTP EPC→SKU 查询"| QUERY
    DEV <-->|"TCP 分拣指令 / 落格反馈"| PLC
    DEV -->|"Snap7：S7 DB77 锁格读取"| PLC
    DB <--> DISK["本地 SQLite：data/sorting_records.db"]
    CORE --> CFG["XML 配置"]
    APP --> LOG["本地分类日志 / crash.log / minidump"]
~~~

设备通信、操作界面和接口运行在同一故障域。关闭主窗口会触发核心组件退出；窗口事件循环的长时间阻塞也会影响 Qt 网络回调、RFID 业务入口和定时任务。[入口](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/main.cpp:158)、[组件组装](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/MainWindow.cpp:1349)

### 1.2 外部交互清单

| 外部对象 | 方向与职责 | 当前实现/配置依据 |
|---|---|---|
| WMS H4 | WMS → WCS，POST /api/DispatchSortingCommand/InsertWaveInfo；提交波次与 SKU/格口计划 | HttpServer::processRequest、validateInsertWaveInfo；路径可配置 |
| WMS H5 | WMS → WCS，POST /api/DispatchSortingCommand/InsertWaveIn；取消波次 | 分拣开始后的取消有状态限制，并非任意时刻可取消 |
| WMS H6 | WMS → WCS，POST /api/DispatchSortingCommand/BindingLatticePort；绑定格口与容器 | 优先读 query 的 latticehole/boxcode，兼容 JSON body |
| WMS H7 | WCS → WMS；满箱及相关装箱明细回传 | HttpClient::sendGenericFeedback；网关 method 默认 gwisSubProductClassifyOrder |
| WMS H8 | WCS → WMS；波次完结回传 | HttpClient::sendEndFeedback；独立 URL/method，默认 gwisSubProductClassifyEndOrder |
| RFID TCP 服务 | WCS 主动连接，服务端推送 EPC 与小车号 | RfidPushClient；默认目标端口 2010；有重连和可配置心跳 |
| RFID 查询服务 | WCS 根据 EPC 查询 SKU/barcode 映射 | HttpClient 的异步 HTTP 查询；Authorization 值来自配置 |
| PLC TCP 对端 | PLC 主动连接 WCS 的 TCP 监听端口；WCS 下发 EPC/格口/小车号，PLC 返回落格结果 | PlcManager；发送 {EPC\|格口\|车号}；反馈支持 5 字段 {epc\|grid\|firstCar\|lastCar\|status} 与旧 3 字段格式 |
| PLC S7 对端 | WCS 通过 Snap7 读取锁格状态并监测重连 | 当前有效业务为 DB77、偏移 0、25 字节锁格位图；DB1 分拣写入代码已注释 |
| 操作员 | 接收启停、开始分拣、切换/新建任务、H7/H8 补传、清绑定、查询与配置编辑 | MainWindow；操作与应用服务调用直接相连 |
| 本地文件系统 | 保存状态、原始波次、待发报文、配置、日志与崩溃转储 | exe 目录下 config/、data/、log/；部分底层日志/崩溃路径还依赖当前工作目录 |

有效 HTTP 路由只有上述三个 WMS POST 入口。旧注释中的“查询 API”以及历史 RFID HTTP 推送入口不应作为当前接口能力；未知路径目前返回 500。[路由实现](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/HttpServer.cpp:2160)、[旧 RFID 路由删除说明与兜底](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/HttpServer.cpp:2376)、[S7 写入停用](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/PlcManager.cpp:190)

### 1.3 配置真值与部署差异

配置加载位置是 **实际 exe 所在目录/config/http_server.xml**，缺失时才使用 AppConfig/define.h 默认值生成配置。源码目录的样例不等于运行配置。[加载](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/ConfigManager.cpp:290)

| 配置项 | 源码默认/样例 | 仓库发布目录配置 |
|---|---|---|
| WMS HTTP 监听端口 | 8191 | 8191 |
| PLC TCP 监听端口 | 8192 | **2000** |
| RFID TCP 目标端口 | 2010 | 2010 |
| 环境开关 | useTestEnv = 1 | useTestEnv = 1 |
| 业务/PLC 发送/PLC 反馈池大小 | 90 / 8 / 4 | 90 / 8 / 4 |
| 业务格口数量 | define.h 固定为 66 | XML 不覆盖此宏 |
| WMS 格口编码 | 前缀 22，宽度 3；例如 22007 → 内部 007 | 同样配置 |
| 同波次重扫重投 | 开启；冷却 1000 ms；最多 3 次；在途超时 30000 ms | 同样配置 |

证据：[默认常量](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/define.h:53)、[业务格口](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/define.h:191)、[源码样例](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/config/http_server.xml:129)、[发布配置](D:/WCS/WCSApp/WCS_httpServer/release_WcsHttpServer/config/http_server.xml:51)。这些是文件中的值，本次没有读取运行进程，不能据此断言现场进程实际监听状态。

回传 URL/AppKey/method、环境、HTTP 超时、RFID 查询/心跳及部分波次参数可从 UI 应用热更新；端口、设备连接地址、线程池等需要重建相关组件或重启。热更新涉及共享配置的并发风险，见第 5 节。[热更新范围](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/MainWindow.cpp:1185)

### 1.4 软件与构建依赖

| 依赖 | 用途与核实结果 |
|---|---|
| Windows / MSVC | 单一 Visual Studio C++ 应用工程，Debug/Release 均为 x64，平台工具集 v142；依赖 Win32、Winsock、命名互斥体及 DbgHelp |
| Qt 5.15.2 msvc2019_64 | Core、Gui、Widgets、Network、Sql；Debug 另列 PrintSupport/Charts，Release 配置存在手工 PrintSupport 链接；Release 显式 C++17，Debug 未显式统一 |
| Qt VS Tools / QtMsBuild | 负责 moc/uic/rcc；工程依赖外部 Qt 安装及 QtMsBuild 路径 |
| HP-Socket | HTTP server、PLC TCP server、RFID TCP client；头文件标识 5.8.5.4；发布使用 HPSocket_U 等库 |
| Snap7 / CSiemensPLC | S7 通信；仓库两份 snap7.h/.cpp 内容相同，实际编译 WCS_httpServer/ 下副本 |
| Qt SQL 的 QSQLITE 驱动 | 当前数据库调用使用 QSqlDatabase/QSqlQuery，运行依赖匹配的 Qt SQL 插件 |
| sqlite3.c/.h | sqlite3.c 被纳入编译，附带头版本 3.46.0；第一方未直接调用 sqlite3_*，**不能以此推定 QSQLITE 内实际 SQLite 版本** |
| hlog / log4cxx | 分类日志宏及底层输出；发布目录还带相关运行库与 log4cxx.properties |
| QCustomPlot 2.1.1 | MainWindow.cpp 内效率图表对话框，直接编译供应商源码 |
| 其他随库/发布包附带文件 | HControl、QtXlsx、Vikey、tinyxml2，以及部分 QtWebSockets/OpenSSL/sqlite3 DLL 等；未发现全部都被当前第一方直接使用，是否为传递依赖需通过二进制导入分析确认 |

构建路径大量写死为 D:/Qt 和 D:/WCS/...；Debug/Release 的 include、宏定义与链接清单也不一致。仓库未发现配套 CMake/qmake 工程、CI 构建流水线或可锁定全部依赖的清单。由此可以确认构建可迁移性不足，不能在未构建情况下断言某一配置必然失败。[工程配置](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/WCS_httpServer.vcxproj:20)、[链接与编译清单](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/WCS_httpServer.vcxproj:58)、[QSQLITE 创建](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/SortingDatabase.cpp:248)

## 2. 分层结构与所有模块职责

### 2.1 实际分层

~~~mermaid
flowchart TB
    BOOT["启动层：main"]
    UI["展示与部分应用编排：MainWindow / EfficiencyChartDialog"]
    APP["应用与接入混合层：HttpServer"]
    DOMAIN["波次状态：WaveManager"]
    PARSE["计划解析：ParseWorker"]
    ADAPTER["设备/外部接口：HttpClient / PlcManager / RfidPushClient"]
    DATA["持久化：SortingDatabase"]
    BASE["基础组件：TaskQueue / ThreadPool / GridBuffer / EpcCache"]
    GLOBAL["横切：ConfigManager / WmsGridCode / define / 日志"]
    BOOT --> UI
    UI --> APP
    UI --> ADAPTER
    UI --> DATA
    APP --> DOMAIN
    APP --> PARSE
    APP --> ADAPTER
    APP --> DATA
    APP --> BASE
    PARSE --> BASE
    DOMAIN --> BASE
    DATA --> GLOBAL
    APP --> GLOBAL
~~~

这是从实现抽取的逻辑层次，不是已经隔离的工程层次。整个系统只有一个应用构建目标。MainWindow 直接协调回传并访问数据库；HttpServer 同时处理接入协议、波次调度、恢复、EPC 路由和 Outbox；部分“工具”还反向依赖全局配置或 UI 日志头。

### 2.2 第一方模块完整清单

行数为同名 .h/.cpp 合计；头文件实现也计入。表中所有模块均属于当前工程直接编译或纳入的第一方源码。

| 模块 | 行数 | 主要职责与当前接入情况 |
|---|---:|---|
| main | 227 | QApplication 入口；异常/信号崩溃转储、Qt 消息钩子、同登录会话单实例、目录创建、配置加载与显示 MainWindow |
| MainWindow | 3,863 | 全部运行界面；组件组装；启停和补传操作；回传结果路由；绑定/波次/EPC/SKU/格口查询；内嵌 EfficiencyChartDialog 与实时数据表 |
| HttpServer | 5,940 | HP HTTP listener；三条 WMS 路由；拥有核心组件；波次排队/恢复/取消、RFID 查询编排、EPC 路由、PLC 反馈处理、H7/H8 构建与调度、效率统计和健康日志 |
| WaveManager | 998 | 波次状态、接收/分拣/异常集合与计数、状态快照、超时/重试、开始分拣、恢复和对账基础数据 |
| ParseWorker | 277 | 专用 QThread 消费 WaveTask，解析 H4，合并 SKU/格口数据、形成接收集合、发布 GridBuffer，并发送解析完成/异常信号 |
| HttpClient | 604 | QNetworkAccessManager；H7/H8 请求、网关参数、超时、业务回执解析；RFID EPC→SKU 查询 |
| PlcManager | 1,548 | PLC TCP 监听/连接管理、指令编码/发送、格口选择、反馈解析/批处理；S7 锁格轮询/重连与设备统计 |
| SiemensPLC / CSiemensPLC | 196 | Snap7 客户端包装；连接、读写 DB、错误处理；保留的 DB1 编码/写入能力在当前主发送链路未启用 |
| RfidPushClient | 444 | HP TCP client；主动连接 RFID、重连/心跳、ASCII 文本帧拼包/拆包；解析后转换为 QJsonObject 业务信号 |
| SortingDatabase | 2,514 | 单例数据库门面；专用写线程与线程局部查询连接；建表/兼容迁移、计划/实绩/绑定/报文/Outbox/异常/统计的读写 |
| OutboxManager | 361 | 泛化 H7/H8 持久化入队、任务扫描、重试与人工补传类；**已编译但未发现实例化，当前运行逻辑仍在 HttpServer** |
| DoubleBuffer / GridBuffer | 157 | 原子发布 SKU→GridEntry 映射；旧映射延后回收；提供快照、查询和活动裸指针 |
| EpcCache | 423 | EPC→SKU/小车号缓存、TTL、查询就绪及接收/发送计时；与 HttpServer 的在途/重投状态共同支撑分拣前置条件 |
| TaskQueue / WaveTask | 95 | 有界波次解析任务队列，QMutex/QWaitCondition 等待与停止 |
| ThreadPool | 141 | std::thread 通用线程池；带 future 与无等待任务提交，队列/活动统计；当前不提供容量上限或完整排空协议 |
| ConfigManager / AppConfig | 497 | XML 读写、默认值、2 秒合并保存、外部文件哈希变化检测；提供全局可变配置引用 |
| WmsGridCode | 115 | WMS 前缀格口编码与内部补零 key 的转换、归一；直接读取 ConfigManager |
| define | 908 | 网络/协议/容量/线程/超时/业务常量、日志宏及大量 SQL；集中但跨多种职责 |
| LogService | 87 | 统一引入分类日志接口和生命周期日志，供业务模块使用 |
| LifecycleLogger | 224 | EPC 生命周期事件格式化、关联记录和输出 |
| log_center / LogCenter | 254 | hlog/运行日志封装及 UI 日志辅助能力，头文件依赖 QTextEdit |
| WCS_httpServer | 28 | 旧 Designer 主窗口壳，只调用 ui.setupUi；仍编译/moc/uic，但实际 main 不使用 |

模块入口依据：[构建源文件清单](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/WCS_httpServer.vcxproj:112)、[核心对象创建](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/HttpServer.cpp:31)、[主窗口职责](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/MainWindow.h:36)、[旧窗口标记](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/WCS_httpServer.h:1)。

### 2.3 目录职责与遗留资产

| 目录/资产 | 角色 |
|---|---|
| WCS_httpServer/ | 当前 VS 工程、有效第一方源码、部分供应商源码和源配置样例 |
| src/ | 同名旧实现集合；未被当前 vcxproj 引用，不能作为现架构主实现 |
| include/、lib/ | 第三方头/导入库；含当前未发现直接使用的历史依赖 |
| release_WcsHttpServer/ | exe、DLL、Qt 插件、运行配置、数据库、日志/转储等部署资产混放；本次未读取或改写业务数据库内容 |
| test/ | Python/PowerShell/bat 模拟、压力与 E2E 脚本，以及历史结果；其中部分协议和端口已过时 |
| mock_env/ | RFID/WMS/PLC/S7 模拟、GUI、场景演练和模拟器自测；属于开发验证工具，不是生产 C++ 内部模块 |
| doc/、docs/、协议 PDF、提交信息.md | 设计/历史评估/协议与变更背景；结论优先服从当前源码，本报告写入 docs/ |

## 3. 核心业务数据流、状态与线程模型

### 3.1 核心业务实体

- **Wave/orderCode**：波次和任务上下文；计划数量由 orderQty 表示。
- **SKU/inco**：H4 中商品品种标识，解析后的主映射以 SKU 为 key。不能因旧注释把所有 inco/barcode 都称作 EPC。
- **EPC**：RFID 识别的单件编码；通过外部查询获取 SKU，并关联 carNum。
- **Grid**：内部通常归一为三位 key（如 007），对外可编码成 22007；业务配置 66 个，S7 位图覆盖范围为 200 位，两者不是同一个容量。
- **Container/boxcode**：格口当前容器绑定，决定 H7 装箱归属。
- **PLC command/feedback**：分拣动作提交与实际落格反馈；当前缺少贯穿两者的独立持久化 commandId。
- **Outbox msgId**：本地回传记录身份和回调关联；其存在不自动构成 WMS 端幂等协议。
- **sumLocation**：当前 WaveSnapshot 用于 H8 的落格计件数；旧 UI/注释中出现的“去重格口总数”不能作为业务口径。[定义](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/WaveManager.h:47)

### 3.2 启动与停止

启动顺序：

1. main 注册崩溃捕获和 Qt 日志钩子，创建 QApplication；通过 Local 命名互斥体限制同一登录会话重复打开。
2. 建立 exe 目录下配置/数据/日志目录，加载 XML。
3. MainWindow 创建界面及 HttpServer/HttpClient；HttpServer 创建队列、映射、WaveManager、ParseWorker、PLC/RFID 对象、缓存与线程池，打开数据库。
4. 注入接口配置、绑定、回调及信号连接；自动连接 PLC/RFID、启动解析线程。
5. 默认 AUTO_START_RECEIVE_ON_BOOT=1，在事件循环开始后自动执行一次“开始接收任务”。
6. 启动恢复会读数据库绑定、提示未完成波次；不会自动把历史波次完整恢复为当前分拣任务。

停止分成两层：

- **停止接收**：关闭/重置 WMS HTTP listener，必要时兜底写波次明细；PLC/RFID/解析组件和部分 Outbox 定时器保持。
- **退出进程**：停止设备/解析与定时器、关闭 DB、释放缓存和线程池。当前释放顺序有风险，见 R2。
- UI 的“结束任务”先发起 H8，再等回执/30 秒超时/人工操作进入 stopReceive；结束等待、HTTP 接收开关和波次状态是几套独立状态。

证据：[启动](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/main.cpp:158)、[自动接收](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/MainWindow.cpp:260)、[接收/设备生命周期](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/HttpServer.cpp:813)、[停止](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/HttpServer.cpp:926)、[启动恢复](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/HttpServer.cpp:1004)。

### 3.3 H4 波次接收与计划发布

~~~mermaid
sequenceDiagram
    participant W as WMS
    participant IO as HP HTTP worker
    participant B as 业务线程池
    participant Q as TaskQueue
    participant P as ParseWorker
    participant M as GridBuffer
    participant C as HttpServer/GUI
    participant D as DB 写线程
    W->>IO: H4 JSON
    IO->>B: 复制连接状态并提交 processRequest
    B->>B: JSON/字段/计划校验
    B->>Q: 提交 WaveTask
    B->>B: 提交原文保存任务（不等待完成）
    B-->>W: HTTP 200（入队受理）
    B->>D: 保存 wave_raw（另一个异步任务）
    Note over B,D: 原文落盘与 HTTP 200 的完成先后无保证
    Q->>P: 消费原始波次
    P->>M: 立即发布新 SKU 映射
    P-->>C: queued waveParsed
    alt 当前正在执行其他波次
        C->>C: 保存 PendingWave 到内存队列
        C->>D: 写新波次头；延后明细
    else 注册为当前波次
        C->>C: setWaveData / setRecvSet
        C->>D: 写波次头
        C->>B: 提交明细持久化任务
        B->>D: insertWaveItems + 失败重试
        B-->>C: wavePersistenceFinished
        C->>C: 校验归属并推进 BOUND
    end
~~~

**时序图表示当前代码，包括其缺陷**：映射发布发生在忙碌判断之前。正常业务意图是排队波次不影响当前波次，但实现未保持该隔离，见 R1。

HTTP 200 代表接收路径已经受理，不代表解析、持久化、绑定完成或可分拣。原文虽然会保存到 wave_raw，但保存异步于接收；PendingWave 的 FIFO 顺序和执行位置没有完整的持久化恢复协议。[受理](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/HttpServer.cpp:2241)、[原文保存](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/HttpServer.cpp:2269)、[发布映射](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/ParseWorker.cpp:200)、[注册与排队](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/HttpServer.cpp:132)

BOUND 当前主要在明细成功落库后推进，并非严格证明所有目标格口都绑定完毕；实际分拣仍由“开始分拣”动作启动。[注册逻辑](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/HttpServer.cpp:193)、[持久化完成](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/HttpServer.cpp:1856)

### 3.4 RFID → SKU → PLC → 落格数据

1. RfidPushClient 从 TCP 数据流拼接并提取 ASCII 文本帧，转换成内部 JSON 对象后发出 rfidPushReceived；Qt queued connection 将业务处理送回 GUI 所在线程。当前文本帧为 {流水号|设备编码|EPC}0D，车号从第一段流水号提取，第二段是设备编码，不能直接当作 carNum；空 EPC/NOREAD 只记日志。[帧解析](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/RfidPushClient.cpp:299)
2. HttpServer 记录推送/效率、归一 EPC 和小车号，更新 EpcCache；对缺失 SKU 的 EPC 发起 HttpClient 查询。
3. RFID 查询异步返回 EPC→SKU 映射，更新缓存并再次尝试发送；缺 SKU、未到 SORTING、信息未齐等路径有不同挂起/重试处理。
4. trySendToPlcForEpc 校验当前波次、在途、缓存及重扫限制，查当前 SKU/格口映射。
5. PlcManager 按多格口、锁格/可用条件选格，取得小车号；提交发送池，通过 TCP 文本协议下发。
6. PLC 回传落格结果；PlcManager 解析并合并到反馈缓冲，批量信号进入专用反馈池。
7. HttpServer 处理成功、失败、未知 EPC/SKU、未绑定等分支，更新 WaveManager、EPC 在途状态、内存格口记录，再写 sorting_records 或 exception_record，并通知界面。

当前支持同波次重扫重投，并有冷却窗口、次数限制和在途超时。源码所称“原格重投”实际会读取当前 SKU 映射并再次经过 PlcManager 选格，不保证第一次实际落格不变；多格口或锁格变化时尤需明确语义。[重新读取映射](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/HttpServer.cpp:5321)、[再次选格](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/PlcManager.cpp:445)。**重投动作次数、反馈次数、唯一 EPC 件数不能混为同一个计数**；当前实现和恢复存在口径不一致，见 R8。

证据：[RFID 入口](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/HttpServer.cpp:4653)、[查询回执](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/HttpServer.cpp:5048)、[发送条件](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/HttpServer.cpp:5267)、[反馈处理](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/HttpServer.cpp:302)、[PLC 实际编码发送](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/PlcManager.cpp:165)。

### 3.5 H6、H7、H8 与恢复

| 流程 | 正常职责与实际行为 |
|---|---|
| H6 绑定 | 校验并更新内存格口→容器映射、通知配置/UI，然后另行提交 DB 绑定；HTTP 回应与 DB 成功不是同一个提交点 |
| H7 满箱 | 锁格边沿/人工满箱/收尾补发触发；按格口与容器组报文，写 outbox_fullbox，发送并处理回执；成功后更新报送/绑定相关状态 |
| H8 完结 | sendEnd 检查会话，处理未回传格口，建立完结报文与 outbox_end，进入 ENDING；回执、失败耗尽或超时驱动后续状态及 UI 接收停止 |
| 后台补传 | HttpServer 的两个 5 秒定时扫描负责实际调度；MainWindow 把信号转为 HttpClient 请求，并按 context 字符串前缀分发回执 |
| 历史恢复 | 人工选波次后，从波次头、计划/原文、实绩/异常、绑定等重建内存；终态可载入查看，未完成状态可恢复后继续操作 |
| 切波次/新任务 | 保存/清理当前运行上下文，维护待执行列表，终态后尝试重放下一条；会影响非当前波次自动重试资格 |

应把保障范围准确描述为：**已实现本地报文存储、有限重试和人工补偿，但尚未形成一致的持久化消息交付闭环**。原因包括写 Outbox 失败仍发网、非当前波次 pending 可被取消、状态写入分散，以及未验证 WMS 幂等契约。不能把 OutboxManager 的类名、sort_txn 的唯一索引或日志中的“补传”当作全链路保证。

证据：[H6](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/HttpServer.cpp:2447)、[H7](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/HttpServer.cpp:3222)、[H8](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/HttpServer.cpp:4039)、[H7 扫描](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/HttpServer.cpp:3934)、[H8 扫描](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/HttpServer.cpp:4503)、[人工恢复](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/HttpServer.cpp:1391)。

### 3.6 状态模型

WaveStatus 共十种状态，另有三个旧名称别名：

| 值 | 状态 | 当前解读 |
|---:|---|---|
| 0 | IDLE | 没有当前执行波次 |
| 1 | CREATED | 波次已注册，明细可能仍在持久化 |
| 2 | BOUND | 已准备到可开始阶段；名称比当前实际前置条件更强 |
| 3 | SORTING | 分拣运行态 |
| 4 | FULLBOX_SYNC | 保留的满箱同步态；当前 H7 已尽量从整波次状态迁移中解耦 |
| 5 | CANCEL_PENDING | 取消处理中 |
| 6 | CANCELLED | 取消终态 |
| 7 | ENDING | 完结回传中 |
| 8 | FINISHED | 本地完结态；部分兜底路径也可进入，不能仅凭此证明 WMS 已确认 |
| 9 | HELD | 异常挂起 |

主要操作路径为 IDLE → CREATED → BOUND → SORTING → ENDING；终态、失败挂起与人工恢复由多个位置驱动。早期 CREATED/BOUND 可进入取消流程。不能仅从枚举注释推导“所有 H7 必须阻塞分拣”或“FINISHED 必然 H8 成功”。[状态枚举](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/WaveManager.h:27)、[H8 会话超时](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/HttpServer.cpp:4220)、[H8 回执](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/HttpServer.cpp:4286)

实际还有并行状态：接收开关、UI StopPhase、当前/待执行波次、EPC 查询/在途/重投状态、格口物理锁定、业务禁用/绑定及 Outbox pending/success/failed/cancelled。这些状态目前分散在多个类内，缺少统一事件与不变量。

setState 的允许迁移如下；直接赋值/恢复/clearWave 路径并不全部受此表限制：[迁移实现](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/WaveManager.cpp:321)

| 当前状态 | setState 允许的目标状态 |
|---|---|
| IDLE | CREATED |
| CREATED | BOUND、ENDING、CANCELLED、HELD |
| BOUND | SORTING、ENDING、CANCELLED、HELD |
| SORTING | FULLBOX_SYNC、ENDING、HELD |
| FULLBOX_SYNC | SORTING、ENDING、HELD |
| CANCEL_PENDING | CANCELLED、CREATED、BOUND |
| CANCELLED / FINISHED | IDLE |
| ENDING | FINISHED、HELD |
| HELD | IDLE、FINISHED、BOUND |

状态字段虽使用 atomic，但 load→校验→store 整段迁移不是一次 CAS。开工与取消另外共用互斥锁；不能由此推断所有状态变化都被统一串行化。

### 3.7 持久化模型

| 表/数据 | 职责 | 需注意的边界 |
|---|---|---|
| return_wave | 波次头、计划数量与状态 | 头与明细/实绩/Outbox 不总在同一事务中更新 |
| return_wave_item | 波次 SKU/格口计划及数量字段 | 由解析后的聚合映射形成；需检查多格口逐格计划保真 |
| wave_raw | H4 原始 JSON | 为重建计划/恢复提供依据，但接收成功与其写入无统一提交点 |
| sorting_records | 成功落格实绩、EPC/SKU/格口/小车/容器/时间及报送信息 | 当前 H7 统计与历史查询的重要来源 |
| grid_box_bind | 格口容器绑定及历史 active 状态 | 同时存在 XML 和内存绑定副本 |
| outbox_fullbox | H7 报文、msgId、状态、重试时间/次数 | 当前执行由 HttpServer 承担 |
| outbox_end | H8 报文及投递状态 | 自动扫描按当前波次过滤 |
| exception_record | 未匹配、未绑定、失败、超时等异常/留痕 | 包含不同业务语义，恢复不能简单全部算作分拣失败 |
| sort_txn | 设计为单件分拣事务和波次/EPC 唯一性约束 | 有防重查询调用，但未发现生产路径调用 insertSortTxn；不能视为现行实绩事务主表 |
| wave_history（独立历史库） | 波次完结摘要 | 位于 data/wave_history.db，只存摘要；不是完整波次数据快照 |
| daily_peak | 每日效率峰值 | 当前效率以 RFID 推送窗口计量，不能直接等同 PLC 成功落格产能 |

主库路径是 exe/data/sorting_records.db，历史摘要另写 exe/data/wave_history.db。读写通过 Qt SQL；多数操作汇聚至一个 QThread，部分查询使用线程局部连接。写连接尝试配置 WAL、synchronous=NORMAL、cache_size=5000；线程局部查询连接名带线程 ID，并设置 busy_timeout=3000。所谓“只读查询连接”未强制只读，而且 cleanupOldRecords 会经该连接执行 DELETE/OPTIMIZE，因此当前不是严格的单写者模型。[连接实现](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/SortingDatabase.cpp:236)、[查询连接清理写入](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/SortingDatabase.cpp:858)、[独立历史归档](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/SortingDatabase.cpp:1848)、[SQL/表定义](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/define.h:271)

现有清理入口主要清理 sorting_records 旧数据；不能将“保留 90 天”理解为所有表、报文、异常和部署目录都会被统一清理。[清理实现](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/SortingDatabase.cpp:192)

### 3.8 实际线程与调度模型

| 执行域 | 数量/配置 | 承担的工作与边界 |
|---|---|---|
| GUI / Qt 主线程 | 1 | MainWindow、HttpServer/WaveManager/HttpClient 等 QObject 的归属线程；UI、QNAM 回执、RFID queued 入口、状态/Outbox/日志/反馈批处理/S7 锁格定时器 |
| HP HTTP I/O worker | 显式配置 16 | 接收 HTTP、按连接拼 body、提交完整请求到业务池；不等于应用全部 I/O 线程总数 |
| 业务 ThreadPool | 默认/发布配置 90 | HTTP 业务处理、部分数据库任务、报文构造/状态落库；同池不同请求和同波次状态更新可能并发 |
| ParseWorker::run | 1 个 QThread | 阻塞消费 TaskQueue 并解析 H4；QThread 对象本身由主线程持有，run 执行在线程内 |
| PLC 发送池 | 8 | 异步 TCP 发送；任务提交结果当前未传播实际发送完成结果 |
| PLC 反馈池 | 4 | 批量落格结果处理、内存状态更新和 DB 提交；批次间可能并行 |
| DB 写线程 | 1 个 QThread + QObject target | 写连接和通过 runOnDbThread 调用的操作串行执行；跨线程用 BlockingQueuedConnection，调用者会等待 |
| 线程局部查询连接 | 随调用线程懒创建 | 部分查询及旧记录清理仍在调用者线程同步执行；其他查询仍可能走 DB 线程；不会自动把 UI 查询转到后台 |
| S7 心跳线程 | 1 个 std::thread | 检查连接、断线重连；经信号回主线程启用锁格轮询 |
| HP PLC/RFID 网络线程 | 由库管理 | TCP listener/client 回调；不能把默认业务池数当作这些线程数 |
| hlog/log4cxx 内部执行 | 取决于库/配置 | 本次未核实库内部线程数量 |

~~~mermaid
flowchart LR
    HTTP["HP HTTP workers"] --> BP["业务池 90"]
    BP --> TQ["TaskQueue 上限 5"]
    TQ --> PW["ParseWorker 1"]
    PW -->|"queued waveParsed"| GUI["GUI / Qt 事件循环"]
    RF["HP RFID 回调"] -->|"queued"| GUI
    GUI --> HC["HttpClient / QNAM"]
    HC -->|"网络完成事件"| GUI
    GUI --> SP["PLC 发送池 8"]
    PLC["HP PLC 回调"] --> FB["反馈缓冲"]
    FB -->|"主线程定时批量取出"| GUI
    GUI --> RP["PLC 反馈池 4"]
    BP -->|"阻塞投递"| DB["DB 写线程 1"]
    RP -->|"阻塞投递"| DB
    GUI -->|"部分同步 DB 调用"| DB
    GUI -->|"定时同步读取"| S7["S7 DB77"]
~~~

并发保护采用 std::mutex、Qt mutex、atomic、连接状态锁、EpcCache 锁、反馈缓冲锁和波次内部锁等组合。它们能保护部分容器/字段，但不能自动保障跨对象操作原子性、波次归属、事件顺序和对象生命周期。

例如数据库内部串行只能保证“按到达 DB 队列的顺序执行”，不能保证 90 个业务线程发出的同一波次状态更新仍遵守原始事件顺序；UI 查询和 S7 定时读取仍可能同步占用 GUI。后者的实际延迟需实测。[DB 调度](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/SortingDatabase.h:346)、[GUI 中 S7 轮询](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/PlcManager.cpp:694)、[异步状态落库](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/HttpServer.cpp:105)

## 4. 模块依赖关系与循环依赖

### 4.1 静态依赖审计方法与结论

扫描第一方 .h/.cpp 的直接 include，将同名头/实现合并为模块，忽略模块自身头、Qt/标准库和供应商库边，再计算强连通分量。

**结果：22 个节点，没有包含两个及以上模块的强连通分量，即未发现第一方 include 循环。** 这不意味着没有运行时相互调用，也不意味着线程安全或分层合理。前向声明 HttpClient 仅用于减少头文件依赖，不能据此宣称有循环。

下表完整列出直接第一方 include 依赖，未列传递依赖、信号和回调：

| 模块 | 直接依赖的第一方模块 |
|---|---|
| main | ConfigManager、LogService、MainWindow |
| MainWindow | ConfigManager、HttpClient、HttpServer、LogService、PlcManager、SortingDatabase、WmsGridCode、define |
| HttpServer | ConfigManager、DoubleBuffer、EpcCache、HttpClient、LogService、ParseWorker、PlcManager、RfidPushClient、SortingDatabase、TaskQueue、ThreadPool、WaveManager、WmsGridCode、define |
| WaveManager | DoubleBuffer、LogService、define |
| ParseWorker | DoubleBuffer、LogService、TaskQueue、WmsGridCode、define |
| HttpClient | LogService、define |
| PlcManager | ConfigManager、EpcCache、LifecycleLogger、SiemensPLC、ThreadPool、define |
| SiemensPLC | LifecycleLogger、define |
| RfidPushClient | LogService、define |
| SortingDatabase | LogService、WmsGridCode、define |
| OutboxManager | SortingDatabase、define |
| DoubleBuffer | define |
| EpcCache | LogService、define |
| TaskQueue | 无 |
| ThreadPool | 无 |
| ConfigManager | LogService、define |
| WmsGridCode | ConfigManager、define |
| LogService | LifecycleLogger、log_center |
| LifecycleLogger | 无 |
| log_center | define |
| define | 无 |
| WCS_httpServer | 无；只依赖 Qt/生成 UI 头 |

HttpServer 直接依赖 14 个第一方模块，MainWindow 直接依赖 8 个，是依赖最集中的两个入口。这里的数量只表示结构集中程度，不用作单独的质量评分。

### 4.2 运行时闭环：单独标记

~~~mermaid
flowchart LR
    UI["MainWindow"]
    HS["HttpServer"]
    HC["HttpClient"]
    WM["WaveManager"]
    PM["PlcManager"]
    CACHE["HttpServer 持有的 EpcCache"]
    HS -->|"① H7/H8 ready 信号"| UI
    UI -->|"① 发送请求"| HC
    HC -->|"① reportResult"| UI
    UI -->|"① 分派回执"| HS
    HS -->|"② 状态/数据操作"| WM
    WM -->|"② waveStatusChanged"| HS
    HS -->|"③ 分拣指令"| PM
    PM -->|"③ 落格/锁格信号"| HS
    PM -.->|"③ carNum 回调"| CACHE
    HS -->|"④ RFID 查询"| HC
    HC -->|"④ rfidBindingResult"| HS
~~~

| 闭环 | 性质与影响 |
|---|---|
| ① HttpServer → MainWindow → HttpClient → MainWindow → HttpServer | 应用回传闭环；发送、回执分派和停止等待依赖 UI，妨碍无界面运行、独立测试和生命周期隔离 |
| ② HttpServer ↔ WaveManager | 协调器修改状态，状态信号反向触发写库/下一波次/挂起重放；需管理重入、顺序与状态归属 |
| ③ HttpServer ↔ PlcManager | 合理的命令/反馈协作；小车号回调捕获 HttpServer，增加间接生命周期依赖 |
| ④ HttpServer ↔ HttpClient | RFID 查询/结果闭环；当前主要通过 queued 信号保持 QNAM 线程归属 |

证据：[UI 回传中继](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/MainWindow.cpp:1391)、[状态反馈](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/HttpServer.cpp:105)、[PLC 小车号回调](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/HttpServer.cpp:76)、[回调使用](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/PlcManager.cpp:508)、[RFID 回执连接](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/HttpServer.cpp:4873)。

HttpServer 还注入了 setLookupCallback，但当前未发现 PlcManager 生产实现调用 m_lookupCb；因此没有把它画成正在使用的查询数据流。运行时反馈闭环本身是正常控制业务，不应全部标成应消除的“循环依赖”；真正要消除的是 UI 中继、隐含所有权和跨线程共享可变状态。

### 4.3 跨层依赖与所有权

- MainWindow 是事实上的组件装配入口，却也解析 fullbox_/end_/resendFullbox_/resendEnd_ 等字符串，承担回传协议的分派。
- HttpServer 通过 QObject parent 持有 WaveManager/ParseWorker/PlcManager/RfidPushClient，通过裸指针持有队列/映射/缓存/线程池，借用数据库单例和 HttpClient。
- WaveManager 只管理部分内存领域状态，真正业务规则分散在 HttpServer、PlcManager、ParseWorker 和数据库 SQL 中。
- WmsGridCode 依赖 ConfigManager，导致存储层也间接依赖运行配置；LogService 引入 log_center 的 QTextEdit 依赖，核心业务编译边界仍牵涉 Widgets。
- 独立 OutboxManager 未接入，新增修复集中到 HttpServer；存在“已有抽象但实际另走一套实现”的维护债务。

## 5. 架构优缺点、技术债务与风险

### 5.1 已有优点

1. **现场单机部署路径直接**：一个 exe 联接 WMS 和设备，无额外服务编排要求，界面集成配置、查询和人工补偿。
2. **设备常驻与任务接收分离**：停止接收不必反复 new/delete PLC/RFID，减少日常启停的对象变化。
3. **部分耗时操作已隔离**：HTTP I/O 与业务分离、H4 专用解析线程、PLC 发送/反馈分池、DB 写线程和批量事务。
4. **有追溯基础**：保存原始 H4、计划、成功落格、绑定历史、异常、Outbox 和波次摘要，并记录分类日志/崩溃转储。
5. **有恢复与人工处置入口**：能够看历史、恢复波次、补传 H7/H8、清理绑定；比完全依赖内存更易诊断。
6. **已考虑 RFID 重复和在途问题**：缓存、冷却、重投次数、映射复用与在途超时控制可以作为重构时的行为基线。
7. **已有验证工具**：包含 RFID/WMS/PLC/S7 模拟和带实际落格/数据库断言的重扫 E2E，可逐步转为自动回归。

这些是实现能力，并非本次对吞吐、无丢件或可靠交付的验收结果。

### 5.2 风险分级原则

- **P0：优先修复**。静态路径涉及错波次路由、丢失业务反馈或访问失效资源，应在扩大现场负载或继续改造前处理。
- **P1：近期收敛**。影响发送确认、持久化、幂等、恢复、输入边界或状态一致性。
- **P2：持续治理**。影响维护成本、可测性、部署复现与观测质量。

优先级是本次架构评估建议。表内“已确认”不等于现场已复现每一种后果。

### 5.3 核心风险清单

| 编号/优先级 | 已确认实现与触发条件 | 影响及建议 |
|---|---|---|
| **R1 / P0** 波次上下文未隔离 | ParseWorker 先交换全局映射，GUI 后判断是否将新波次排队 | 正在执行 A 时收到 B，A 状态可能配上 B 映射。解析输出 ParsedWave，批准激活时一次切换 waveId/generation/map |
| **R2 / P0** 退出释放早于工作结束 | HttpServer 先关闭 DB/删除 EpcCache，再析构业务池/反馈池；ParseWorker wait 超时未阻断后续释放 | 存在空指针/释放后访问窗口，排队任务也可能未完成。建立停止生产者、排空/持久化、join、释放依赖的退出协议 |
| **R3 / P0** PLC 流完整性不足 | PLC 只正则匹配本次 rawData 的完整帧；跨 TCP 回调无拼包；反馈缓冲满 200 条不再加入新条目 | TCP 分片或 GUI 延迟可造成业务反馈丢失。按连接 framing，业务可靠队列与 UI 采样分开 |
| **R4 / P0** 待执行队列悬空引用 | first() 的 const 引用在 removeFirst() 后继续用于日志/信号 | 下一波次切换路径可能读取失效对象。先值拷贝/移出值对象，再移除容器元素 |
| **R5 / P1** 入队被当作发送成功 | sendBatchCodesWithEpcCache 不接收 sendCodeInfo 的最终 bool，直接 successCount++；上层随即 markSent/markInFlight | 断连或发送失败被视为在途，耗时不含真正发送排队。区分 queued、transport_sent、feedback_received/failed |
| **R6 / P1** 接收确认与可靠存储脱节 | H4 size 检查与 push 分开且忽略 push 返回；HTTP 200 早于原文/明细持久化；H6 回应早于 DB 绑定 | 竞争或崩溃窗口可能“已成功应答但未保存”。以原子 enqueue/持久化 Inbox 成功作为受理依据 |
| **R7 / P1** Outbox 交付边界不完整 | 常规 H7 写库失败仍发送；H8 不检查入库结果；非当前波次自动扫描改为 cancelled；H8 无全部 H7 成功确认屏障 | 无法稳定保证恢复补发，WMS 完结可能先于所有箱单确认。统一持久化投递状态，明确 H7/H8 前置契约 |
| **R8 / P1** 计件、幂等与恢复口径分裂 | 部分重复成功反馈只加计数不写明细；no_match/no_bind 成功只写异常表；恢复按成功/异常 DISTINCT EPC 重建；sort_txn 缺主路径写入 | UI/H8 与 H7/DB 数量可能不同，重启后数量/性质改变。分开反馈事件、唯一实物结果、诊断异常和装箱账本 |
| **R9 / P1** 多格口计划损失信息 | 用字符串 contains 判格口重复；合并后首行属性被复用于其他格口 | “12”包含“2”，合法格口可被丢掉；不同格口计划量/来源/类型可能丢失。采用 SKU→Allocation 列表和类型化 GridId |
| **R10 / P1** 共享状态和事件顺序缺统一所有权 | 配置 GUI 原地更新与业务池读取无统一同步；波次多字段/迁移分散；状态落库经并行池；同一 S7 client 被 GUI/心跳线程访问 | 存在数据竞争或状态倒序窗口。单业务事件线程、不可变配置快照、带版本写入；S7 在独立线程串行调用 |
| **R11 / P1** 映射回收依赖固定时间 | GridBuffer 暴露活动裸指针，旧对象按 5 秒延后回收，不跟踪读者 | 长遍历/线程挂起叠加换表可产生悬空读。使用共享所有权的不可变映射快照 |
| **R12 / P1** 数据库一致性和就绪条件不完整 | 绑定归档再插入不在同一事务；active 索引非唯一；DDL 失败检查不完整；清理可经查询连接写库 | 可能出现绑定缺失/多活跃、数据库误报就绪。版本迁移、逐步检查、事务替换与约束 |
| **R13 / P1** 背压与输入信任薄弱 | HTTP body、通用任务池、待执行波次等没有完整容量链；HTTP 监听所有网卡且路由未见鉴权；PLC 接受连接并广播 | 大请求/积压可能耗尽资源；暴露范围取决于网络隔离。设置字节/数量/并发上限、明确拒绝语义，核实部署访问控制 |
| **R14 / P1** 配置/日志暴露凭据 | 源码/配置含 AppKey，HttpClient 与 UI 存在完整 Key/URL 日志输出 | 凭据进入源码、日志和界面。密钥外置、日志脱敏；保留协议必要字段但避免无必要复制 |
| **R15 / P2** UI 和业务运行耦合 | H7/H8 中继、同步查询、S7 读取和多个定时器共处 GUI；波次列表存在逐波次查 Outbox | 查询/绘制/设备延迟会共同影响业务调度；抽出协调器、后台查询与聚合读模型 |
| **R16 / P2** 构建、测试和遗留实现漂移 | 硬编码路径、Debug/Release 差异、src 副本、未接入 OutboxManager、过时协议/测试和无调用方法并存 | 修改落错位置、误用历史测试结果、发版不易复现。单一源码入口、构建清单、参数化回归和遗留清理 |

### 5.4 关键风险源码证据与影响边界

**R1：波次 A 正在执行，B 排队却先发布映射。**

ParseWorker 的 prepareSwap 是立即交换指针，不是仅准备备份；waveParsed 直到其后才发出。HttpServer 在信号处理的 busy 分支只记录 B 并 return，不还原 A 的映射。buildWaveItems 同样从全局活动映射取数据，异步解析相邻任务也可破坏“头与明细来自同波次”的不变量。

证据：[先发布](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/ParseWorker.cpp:200)、[立即 exchange](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/DoubleBuffer.h:110)、[后判断排队](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/HttpServer.cpp:145)、[从活动映射建明细](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/HttpServer.cpp:1827)。这是两名独立分析者交叉确认的确定路径；实际错分范围取决于后续输入及运行时序。

**R2/R4/R11：所有权错误不能由 atomic 停止标志补救。**

HttpServer 析构 777–796 行的依赖释放顺序，遇到仍在执行的绑定任务或 PLC 反馈任务时，不能保证 lambda 里的 this 及其成员可用。ThreadPool 析构将运行标志置 false，其工作循环不保证排空所有待执行任务。ParseWorker 普通运行标志跨线程使用，超时后也没有安全地保留被引用资源。队列 first/removeFirst 的引用和固定时间回收映射则是另两类寿命问题。

证据：[析构顺序](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/HttpServer.cpp:764)、[仍使用 DB 的任务](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/HttpServer.cpp:2337)、[仍使用缓存的反馈任务](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/HttpServer.cpp:323)、[线程池退出](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/ThreadPool.h:45)、[等待解析线程](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/HttpServer.cpp:992)、[出队引用](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/HttpServer.cpp:1778)、[旧映射回收](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/DoubleBuffer.h:129)。

**R3/R5：指令发送与业务反馈都需要可验证的交付状态。**

TCP 的完整帧边界不能由一次 OnReceive 假定。当前 PLC 路径与已有 RFID 跨包缓冲能力不一致；而反馈满 200 条时不追加的行为是业务丢弃，不只是 UI 少展示。发送侧的 successCount 实际在提交任务后加一，因此上层 1 秒阈值也不能证明指令已在 1 秒内发到设备。

证据：[PLC 接收](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/PlcManager.cpp:944)、[正则匹配](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/PlcManager.cpp:1004)、[缓冲上限](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/PlcManager.cpp:1140)、[异步发送结果缺失](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/PlcManager.cpp:525)、[上层标记已发送](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/HttpServer.cpp:5455)。

**R6/R7：成功应答、写库成功、发出、对端接受是四个不同提交点。**

HTTP 接收路径没有将 push 的返回值作为最终成功条件；常规 H7 入库失败仍继续发网，H8 忽略写库返回值。虽然补发 H7 的另一个入口会检查写库结果，但多入口行为不一致。H8 发送前会触发 H7，但没有等待所有 H7 回执；H8 成功还会停止 H7 重试定时器。是否允许先完结再补箱需要与实际 WMS 契约核对，本次未作外部协议验收。

证据：[H4 容量/提交](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/HttpServer.cpp:2241)、[H7 入库与发送](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/HttpServer.cpp:3335)、[H8 入库与发送](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/HttpServer.cpp:4168)、[补发 H7 检查](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/HttpServer.cpp:3540)、[H8 完结处理](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/HttpServer.cpp:4308)。

**R7/R8：恢复不等于加载波次头。**

wave_raw 与波次头已有保存，但待执行顺序没有重建；切出会清理内存格口记录，而恢复主要重建映射和进度，未完整重建尚未打包 H7 的箱内记录。此时 sorting_records 可能仍有实绩，而 flushUnreportedFullboxes 依赖内存格口记录。历史 Outbox 又受“必须当前波次”过滤。因此需要恢复矩阵，覆盖“落格已写库、H7 尚未形成”的中间状态。

计数方面，运行中的按反馈累加与恢复时 DISTINCT EPC 数量不同；异常表又兼有成功留痕。这是计数模型问题，不能只改 UI 文案。

证据：[切出清理](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/HttpServer.cpp:1670)、[恢复集合](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/HttpServer.cpp:1587)、[H7 收尾数据源](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/HttpServer.cpp:3570)、[重复反馈计件](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/HttpServer.cpp:540)、[成功转异常留痕分支](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/HttpServer.cpp:380)、[恢复计数](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/WaveManager.cpp:69)。

**R9/R10/R12：跨层共享数据缺少明确不变量。**

多格口合并保留首行元数据，字符串包含不是集合成员关系。ConfigManager 的可变引用与原地 XML 重载没有并发快照机制。数据库虽有专用线程，但绑定两步写不构成事务；DDL 未逐项检查，不能将 open 的成功等同于所有业务表/索引正确。

证据：[格口合并](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/ParseWorker.cpp:143)、[配置可变引用](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/ConfigManager.h:134)、[配置原地加载](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/ConfigManager.cpp:307)、[后台读取配置](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/HttpServer.cpp:2767)、[绑定替换](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/SortingDatabase.cpp:1324)、[建表与就绪](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/SortingDatabase.cpp:285)。

**R13/R14：现场部署保护边界尚未验证。**

本地代码未看到入站鉴权，不等于现场完全没有防火墙/VLAN/代理限制；需核实实际部署。出站把 AppKey 放在 URL 可能是现有网关契约要求，应先确认契约，再调整传输方式；日志和界面脱敏可以先做。报告只指出源码位置，不复述密钥。

证据：[HTTP body 累积](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/HttpServer.cpp:1933)、[HTTP 路由](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/HttpServer.cpp:2067)、[PLC 广播](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/PlcManager.cpp:229)、[密钥日志](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/HttpClient.cpp:47)、[UI 配置摘要](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/MainWindow.cpp:1900)。

### 5.5 其他技术债务

- 状态枚举、注释和实际行为漂移：BOUND 不是全绑定证明；FINISHED 不能等同 WMS 确认；CANCEL_PENDING 在迁移表中有出边但正常白名单未见入边；clearWave 可直接复位状态。
- UI 停止阶段判定重复：进入 StopEnding 后 m_bRunning 被置 false，但“再次点击取消等待”的判断放在 m_bRunning=true 分支内，第二次点击可能走重新开始接收逻辑。应让 StopPhase 成为停止流程唯一判断入口。[UI 停止逻辑](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/MainWindow.cpp:1743)
- 生命周期追踪为内存 EPC 历史列表，未发现主链路清理调用；每事件输出累积历史，长期运行的内存/日志增长需治理。[生命周期记录](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/LifecycleLogger.h:143)
- XML 保存直接写目标文件；不采用原子替换，加载失败也可能已经修改部分内存字段。应先解析到临时配置、验证后发布，再原子保存。[配置写入](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/ConfigManager.cpp:124)
- WMS HTTP 响应主要按 JSON success 解释，RFID 外层失败与数据解析的关系也需统一；建议同时分类网络错误、HTTP 状态、JSON 格式与业务码。[回执处理](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/HttpClient.cpp:138)
- 没有看到统一的 schema 版本迁移、所有表生命周期清理、数据备份恢复演练和部署清单校验。已有波次摘要归档不能替代主库完整备份。

### 5.6 测试资产及其能证明什么

| 资产 | 已有价值 | 当前限制 |
|---|---|---|
| test/ 下 mock_wms/mock_plc/mock_rfid 等 | 提供协议联调和流量生成基础 | 多个年代脚本并存，端口、格式、接口需要核对 |
| test/stress_test.py | HTTP 并发与请求统计 | 构造 101–200 格口，超出当前 1–66；仅以 HTTP 200 统计成功，不能证明异步分拣完成 |
| test/e2e_rescan_resend.py | 对真实被测 exe 配合 RFID/query/PLC 模拟对端；检查首投、冷却、原格重投、在途、上限及 DB | 固定路径/端口、人工启动及部分日志文案断言；仓库保存的结果包含失败，不代表当前源码通过状态 |
| mock_env/selftest.py | 验证模拟器协议与互联能力 | 使用假 WCS listener，不是实际 C++ 系统测试；有端口冲突时跳过并计成功的分支 |
| mock_env/mock_scenario.py | 支持 NOREAD、未知 EPC、延迟、PLC 异常、H7 失败等场景演练 | 多为人工“观察预期”，需要转成机器可判断的不变量断言 |
| 历史日志/上线评估文档 | 提供曾经测试与现场问题线索 | 不构成当前提交、当前配置下的回归结果 |

证据：[旧压力脚本](D:/WCS/WCSApp/WCS_httpServer/test/stress_test.py:64)、[E2E 断言](D:/WCS/WCSApp/WCS_httpServer/test/e2e_rescan_resend.py:273)、[模拟器自测](D:/WCS/WCSApp/WCS_httpServer/mock_env/selftest.py:150)、[场景演练](D:/WCS/WCSApp/WCS_httpServer/mock_env/mock_scenario.py:195)。

本次未运行上述测试，也未以现有历史 PASS 或注释中的“已修复”替代源码分析。

## 6. 架构优化建议

### 6.1 目标结构

建议先演进为 **边界明确、核心可脱离 GUI 运行的模块化单体**。当前业务依赖单机低延迟控制与本地状态，尚无证据说明需要立即拆成多个部署服务。

~~~mermaid
flowchart TB
    BOOT["CompositionRoot：组装与生命周期"]
    UI["Qt UI：命令输入 / 只读视图"]
    HTTP["WmsInboundAdapter"]
    RF["RfidAdapter"]
    PLC["PlcAdapter：单独 I/O 调度"]
    CORE["WcsCoordinator：单线程领域事件入口"]
    WAVE["WaveService / 不可变 WaveContext"]
    SORT["SortingService / 分配与重投规则"]
    BOX["BoxService / 箱内明细"]
    OUT["OutboxDispatcher"]
    REP["Repository / Inbox / 事件账本"]
    NET["WmsGateway / RfidQueryGateway"]
    READ["QueryService / 聚合读模型"]
    BOOT --> CORE
    BOOT --> PLC
    UI -->|"类型化命令"| CORE
    HTTP -->|"持久化受理后的命令"| CORE
    RF -->|"类型化事件"| CORE
    PLC -->|"带身份的发送结果/落格事件"| CORE
    CORE --> WAVE
    CORE --> SORT
    CORE --> BOX
    CORE -->|"带 commandId 的分拣指令"| PLC
    CORE --> REP
    OUT --> REP
    OUT --> NET
    CORE --> NET
    REP --> READ
    UI --> READ
~~~

推荐职责：

- **CompositionRoot**：创建/连接组件、验证配置、启动就绪门槛和有序退出；不放在 MainWindow 的 UI 实现里。
- **WcsCoordinator**：唯一接收领域命令和事件的线程，持有当前波次和状态迁移；后台任务只返回值对象，不直接修改领域状态。
- **WaveContext/ParsedWave**：waveId、generation、配置版本、SKU 分配、容器与状态快照整体一致；排队对象不触碰当前对象。
- **SortingService**：SKU→分配、锁格规则、重投与去重；区分 ItemId(EPC) 和 FeedbackEventId。
- **BoxService**：箱批次与明细持续持久化，H7 是箱清单快照；重启可从 DB 找回未打包/未投递记录。
- **OutboxDispatcher**：统一所有 H7/H8 入队、领取、发送、回执和人工重试逻辑；移除 MainWindow 的字符串前缀协议分派。
- **Repository/Inbox**：事务边界、幂等键、迁移、数据版本和持久化受理；UI 使用 QueryService 的分页/聚合读模型。
- **设备适配器**：负责 framing、连接、发送和明确结果，不读取可变全局业务上下文；S7 连接/轮询/断开在同一工作线程。
- **日志接口**：结构化事件输出到文件/指标，UI 为可选订阅者；不让基础库通过日志头依赖 Widgets。

这些类名是建议的职责边界，不要求一次重写全部系统。

### 6.2 必须建立的业务不变量

1. 活动波次 ID、generation、SKU 分配表和进度始终属于同一上下文；任何异步回执都携带并验证归属。
2. WMS 受理成功对应可恢复的 Inbox 记录；失败或队列满的响应能让发送方明确重试。
3. 计划数据保持每个 SKU/格口的数量、类型、来源和容器元数据，不通过逗号字符串压缩丢失。
4. “请求已入发送队列”“传输层已发出”“PLC 已反馈落格”分别记录；发送失败不能直接算在途成功。
5. 业务反馈不因 UI 卡顿被静默丢弃；若容量不足，必须报警并执行明确的限流/暂停/持久化策略。
6. 重复网络反馈与操作员主动重投分别定义；唯一实物件数、动作次数、异常诊断数各有唯一统计来源。
7. H7/H8 首次发送前 Outbox 事务已提交；其 payload、身份及对应箱/波次版本固定可追溯。
8. 波次“本地停止”和“WMS 已确认完结”是不同状态；H7/H8 的先后屏障按接口契约显式表达。
9. 同一物理格口至多有一个活跃容器绑定；解绑/新绑定及关联事件一起提交。
10. 所有消费者退出或其剩余工作被可靠保存后，才释放 DB、缓存、映射与网络对象。

### 6.3 分阶段实施路线

| 阶段 | 具体工作 | 完成标准 |
|---|---|---|
| **阶段 A：修复确定路径** | 修正波次发布时机、队列悬空引用、H4 push 结果、PLC 拼包/满队列处理、退出顺序、实际发送结果反馈；Outbox 写失败禁止发送 | A 分拣期间连续接收 B/C，A 映射不变；反馈拆包不丢；满队列不回假成功；退出时所有工作者停止后才释放资源 |
| **阶段 B：统一可靠状态** | 增加 wave generation、反馈身份、发送状态；修复多格口分配模型；绑定事务/唯一约束；统一计数与箱明细账本；持久化待执行顺序 | 重复/乱序事件不破坏计数；重启前后件数一致；所有已受理任务可查、可恢复；同格口无多个 active 绑定 |
| **阶段 C：抽离核心编排** | CompositionRoot/WcsCoordinator；MainWindow 仅发命令和订阅视图；合并实际 Outbox 逻辑；配置不可变快照；S7 独立线程；查询后台化 | 不创建 MainWindow 即可在测试中运行核心；UI 停更不影响 PLC 反馈与回传；每个业务状态只有一个写入入口 |
| **阶段 D：工程治理** | 统一 Debug/Release 构建参数和相对依赖路径；建立可复现依赖清单、版本信息、迁移脚本、脱敏配置样例与自动回归 | 干净环境能构建两种配置；部署包自检通过；迁移失败不进入 READY；测试可以用隔离配置/端口自动运行 |
| **阶段 E：容量与运维验证** | 测量各队列等待/执行延迟、数据库事务、网络与 GUI 占用；据实调整池大小；完善留存、备份/恢复、告警和发布回滚 | 指标可说明瓶颈；达到团队定义的负载与恢复目标；每次发布能关联源码版本、依赖、配置和数据库版本 |

不要把“增加线程”作为数据库单连接或 GUI 阻塞的默认解法。先记录排队时间和执行时间；将需要顺序的业务串行化，把独立网络/解析/查询并行化。建议的线程池规模应由测量结果决定，报告不把当前 90/8/4 推定为最佳参数。

### 6.4 优先回归与故障注入矩阵

| 验证场景 | 必须断言的结果 |
|---|---|
| A 分拣中接收 B/C，并延迟 GUI waveParsed 处理 | A 的计划/路由不变；B/C 只有激活后才能发布；头和明细归属一致 |
| 同时提交超过解析队列容量的 H4 | 每个成功响应均有可恢复任务；拒绝有明确状态；无静默 push 失败 |
| PLC 一帧逐字节分片、多帧粘包、跨连接混合、断线半帧 | 有效反馈恰好形成预期业务事件；连接间缓冲隔离；非法帧可观测 |
| GUI 停顿、DB 长事务、瞬时反馈超过缓冲容量 | 无业务静默丢弃；积压和限流可观察；UI 可丢展示但不丢账 |
| PLC 未连接/中途断开/发送延迟 | queued、sent、failed、ack 含义一致；超时从正确时间点计算 |
| 重复反馈、主动重投、不同波次相同 EPC、迟到旧波次回执 | 唯一件数/动作数各正确，旧 generation 不污染新波次 |
| 同一 SKU 分配格口 12 和 2，且各自不同数量/来源/类型 | 所有逐格口属性保留；路由和 H7 明细与计划相符 |
| H7/H8 写库失败、网络超时、业务拒绝、回执丢失 | 未持久化不发网；持久化后可补传；幂等键与状态可追溯 |
| 进程在 H4 受理、落格写库、H7 创建/发出、H8 确认等阶段中断 | 重启后所有可恢复中间状态有定义；不把已发未确认误判成成功 |
| 非当前波次 Outbox 与手动切出/切回 | 自动/人工补传策略符合约定，不因 UI 当前选中任务丢失交付资格 |
| 退出时仍有解析/发送/反馈/DB 任务 | 正确停止或持久化；无释放后访问；线程全部退出 |
| 配置热更新、磁盘满、坏 XML、迁移失败、绑定插入失败 | 配置快照原子生效；旧有效配置可保留；DB 失败不误报就绪；绑定事务回滚 |
| 干净环境 Debug/Release 与发布包启动 | Qt 插件、DLL、配置和端口自检通过；日志不输出密钥 |

PLC 的物理执行去重和 WMS 的幂等接收需要对端协议配合。可以先让 WCS 提供稳定的事件/命令身份、持久化状态和重放规则，再联调验证对端语义，不宣称仅靠本地数据库就能得到端到端“恰好一次”。

### 6.5 应新增的观测指标

- H4 受理成功/拒绝/持久化失败；请求字节数；Inbox 与待执行波次水位。
- 解析时延、业务池/反馈池/发送池排队时延，队列上限、拒绝和溢出计数。
- RFID 接收→SKU 就绪→命令入队→实际发送→落格反馈的分段 P50/P95/P99，而非仅一个本地“发送耗时”。
- 按 waveId/generation/commandId/EPC/boxBatchId/msgId 关联的生命周期；日志中的认证信息脱敏。
- DB 事务时间、锁等待、SQL 失败、迁移版本；持久化状态版本倒退报警。
- 每个 Outbox 的年龄、尝试次数、最后错误、待人工原因，区分超时、传输失败、业务拒绝。
- 唯一成功 EPC、有效重投动作、失败落格、诊断异常、H7 已报件数、WMS 已确认完结数；统计名称匹配其来源。

## 7. 设计理念与设计思路

本节依据已审阅源码逆向归纳设计意图，并非对原作者历史决策的直接陈述。“现有选择”描述当前实现，“建议收敛”描述后续应建立的架构约束。现有机制的技术价值与第 5 节的实现缺陷可以同时成立。

### 7.1 核心理念：把业务计划转成可追溯的物理执行过程

项目的核心工作是衔接 WMS 的任务计划、RFID 的实物身份和 PLC 的物理执行。HTTP、TCP、S7 和 SQLite 分别承担信息交换与保存，业务价值来自这些信息能否在同一个波次中正确关联。

按业务语义，可将设计思路归纳为五步：

1. **建立计划上下文**：确定波次、SKU、计划数量、格口分配及容器归属。
2. **补齐单件信息**：将 EPC、小车号和异步获得的 SKU 关联，判断该件是否具备发送条件。
3. **形成设备动作**：综合当前波次、格口分配、业务禁用、物理锁格和重投条件，形成 PLC 指令。
4. **接收执行事实**：根据 PLC 落格反馈记录成功/异常，更新在途与进度；计划数量和发送数量均不能替代实际落格数量。
5. **形成业务确认**：将实绩汇集为箱单与波次完结报文，通过回执或人工处置完成与 WMS 的对接。

下面是设计意图的概念图，节点不代表当前已经独立实现的服务：

~~~mermaid
flowchart LR
    PLAN["WMS 计划：波次 / SKU / 格口"]
    EPC["RFID 识别：EPC / 小车号"]
    LOOKUP["查询结果：EPC→SKU"]
    JOIN["单件上下文汇合"]
    DECIDE["波次与分配规则"]
    CMD["PLC 分拣指令"]
    FACT["PLC 落格事实"]
    BOX["箱明细 / H7"]
    END["波次确认 / H8"]
    PLAN --> DECIDE
    EPC --> JOIN
    LOOKUP --> JOIN
    JOIN --> DECIDE
    DECIDE --> CMD
    CMD --> FACT
    FACT -->|"解除在途 / 更新进度"| DECIDE
    FACT --> BOX
    BOX --> END
    END -->|"回执 / 异常处置"| PLAN
~~~

概念上的“箱明细→波次确认”表达业务归属，不表示现有实现已经等待所有 H7 成功后才发送 H8；当前缺少该确认屏障，见 R7。上述五步在现代码中分散于 HttpServer、WaveManager、PlcManager、EpcCache 和 SortingDatabase。

### 7.2 可从实现归纳的六项设计原则

| 设计原则 | 当前设计思路 | 所解决的现场问题 | 取舍与应保留的边界 |
|---|---|---|---|
| **按生命周期划分资源** | PLC/RFID 与核心对象随程序常驻，WMS 任务接收可独立启停 | 一轮任务结束后仍需要观察设备、接收状态、处理补传，避免重复连接和对象重建 | 常驻设备、接收状态、波次状态需分开建模；最终进程退出仍必须有完整的依赖释放顺序 |
| **按耗时特征安排执行位置** | 网络接收、H4 解析、PLC 发送/反馈和数据库操作使用不同执行域 | 大波次解析、SQL 和外部接口等待不应长期占住 I/O 回调 | 多线程用于隔离等待；同一波次的状态迁移仍需保持顺序，不能只靠增加线程 |
| **读取面向当前状态，恢复依靠持久数据** | 活动计划和 EPC 就绪数据在内存查询，计划/实绩/原文/出站报文在 SQLite 留存 | 分拣路由需要快速访问，重启、补传和查询又需要历史依据 | 内存是工作状态，DB 是恢复依据；二者之间必须定义提交点、版本和重建规则 |
| **按信息齐备程度推进业务** | EPC 查询、小车号接收和波次开工不要求同时完成；条件未齐时挂起或重试 | 多个系统响应时延不同，实物识别与计划/商品信息可能先后到达 | “暂未就绪”“发送失败”“已执行但未确认”应是不同状态，分别设置超时和处置 |
| **自动恢复与人工处置共同完成运行闭环** | 定时重试之外，保留切换波次、恢复、重传和清理绑定等操作 | 外部系统持续失败、现场返工或设备异常时，需要可理解、可操作的处置入口 | 人工介入应带目标波次/箱/报文身份并留痕，不能绕开事务和归属校验 |
| **将诊断能力嵌入业务路径** | 记录原文、生命周期阶段、异常、回传结果、设备状态和崩溃转储 | 现场问题跨协议、跨线程，只有最终“成功/失败”难以定位 | 日志应能关联业务实体且有留存上限；界面刷新和详细日志不能反过来阻塞业务 |

证据：[设备与接收分离](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/HttpServer.cpp:813)、[解析与反馈线程分工](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/HttpServer.cpp:83)、[数据库连接布局](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/SortingDatabase.cpp:248)、[就绪条件](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/EpcCache.h:33)、[人工恢复](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/HttpServer.cpp:1391)、[生命周期记录](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/LifecycleLogger.h:143)。

### 7.3 关键设计取舍

**低延迟与完整性需要分开保证。** 当前通过内存映射、异步处理和反馈批次降低等待开销，属于具有时限意识的工程实现；Windows/Qt 事件循环、无界队列和同步设备调用并没有给出确定的最坏响应时间保证。应分别验收“处理有多快”和“失败时是否丢业务数据”。尤其不能为了 UI 流畅而丢落格反馈。当前 1 秒超时分支会记录异常、重置计时并返回，未显式下发异常格口指令；日志中的“入异常格口”不能视为设备动作证据。[超时分支](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/HttpServer.cpp:5371)

**计划可以按波次替换，执行事实需要按事件保留。** SKU 映射天然适合构建完成后整批发布，避免读者看到半份计划；落格、重投、绑箱和回执则有先后关系，适合持续追加带身份的事实。当前两类数据都有存储基础，但尚缺统一的事实账本和版本化上下文。

**短暂重复需要抑制，合法重投需要放行。** 对同一 EPC 永久拒绝重复会阻碍现场返工；每次扫码都发指令又可能产生重复动作。因此系统采用在途、冷却和次数等条件，并复用当前映射重新选格，这体现了业务语义驱动的去重思路。下一步应把“同一个实物”和“该实物的第几次动作”建成不同身份；若业务要求固定首次实际格口，还需独立保存和使用该事实。

**轻量本地部署与长期可维护性需要共同考虑。** 单进程和 SQLite 降低现场部署组件数量，便于本地查询与诊断；代价是共享故障域、状态集中和扩展边界较弱。适宜的演进方向是先把核心编排、设备适配、持久化和 UI 在进程内分清职责，再根据实际运维需求决定是否拆成独立进程。

**规则兼容性需要有明确归属。** WMS 格口编码、RFID 文本帧、小车号提取、PLC 反馈格式各有独立语义，应在协议边界归一；核心业务只处理明确类型的 EPC、SKU、GridId 和 WaveId。现有 WmsGridCode/RfidPushClient 已体现该方向，后续应避免兼容逻辑继续散入数据库和 UI。

### 7.4 建议延续的设计方法

后续改造宜按“业务不变量 → 状态所有权 → 提交点 → 执行位置 → 故障恢复 → 可验证指标”的顺序展开：

| 设计步骤 | 在本项目中的具体产出 |
|---|---|
| 明确业务不变量 | 当前波次与计划一致；同格口仅一个活跃容器；唯一件数与动作次数分开 |
| 明确状态所有者 | 波次协调器负责活动上下文；设备适配器负责连接与实际发送；存储层负责事务 |
| 明确提交点 | Inbox 持久化后受理；发送完成后更新传输状态；落格反馈形成实绩；Outbox 提交后才发送 |
| 明确并发与时限 | 顺序相关事件在同一入口处理；独立 I/O/解析后台执行；每一段有容量和延迟指标 |
| 明确恢复规则 | 对“已接收未解析、已落格未打箱、已发未确认”等中间状态定义恢复来源与操作 |
| 明确验证办法 | 从拆包、重投、跨波次迟到反馈、磁盘失败和中途退出等场景检验不变量 |

这一方法把第 6 节的模块划分与具体业务行为对应起来，便于逐步替换实现，而不破坏现场已依赖的功能。

## 8. 高技术价值点

### 8.1 价值判断依据

本项目最有积累价值的部分，是 **将多来源信息、物理执行时序、业务状态和失败补偿组织成可验证的控制过程**。Qt、线程池、SQLite、HP-Socket 等提供实现基础；技术含量体现在如何使用它们维持领域语义与一致性。

以下“技术价值”评价的是解决问题的难度、可复用性和验证价值，不等同于已取得性能指标、算法原创性或成熟度认证。

### 8.2 八项值得保留与深化的技术能力

| 价值点 | 具体机制与技术难点 | 工程价值 | 当前落地边界与验证方向 |
|---|---|---|---|
| **V1：多来源单件上下文关联** | 以 EPC 关联异步 SKU 查询和 RFID 小车号，再衔接当前波次的 SKU 分配；缓存更新需保留已获得的信息 | 把多个接口返回组织为同一件货的执行上下文，可复用于扫码、视觉识别、称重等异步信息汇合场景 | 已有就绪条件、缓存和延迟重试；仍需补充波次 generation/动作身份，验证乱序结果与切波次不会串件 |
| **V2：重复采集与人工重投的语义区分** | 同一 EPC 的在途状态、反馈解除、超时、冷却和次数上限共同约束动作，重投时复用当前映射重新选格 | 同时支持防重复指令与现场返工；价值在于将物理操作语义纳入状态控制 | 基础机制已实现，不保证首次实际格口不变；发送确认和计数仍有 R5/R8 边界，应分别验证唯一件数、动作数和重复反馈数 |
| **V3：计划分配、物理锁格与业务禁用的组合决策** | 先排除满箱未重绑等业务禁用格口，再按映射与物理锁格选择；保留可解释的选格原因 | 把设备状态与仓储容器业务关联起来，使操作员能追溯“该件为什么去该格口” | 当前规则包含全部物理锁格时按需求照发，不能称为安全联锁；多格口属性模型还需修复 R9 |
| **V4：按工作类型隔离异步执行并批量交付反馈** | 网络回调快速转交业务，解析单独执行，PLC 发送/反馈分池，反馈按周期合并交付 | 控制高频事件对事件循环与界面的压力，便于按环节定位延迟和拥塞 | 分工已有实现；无界任务和反馈静默丢弃尚未解决。需以排队时延、溢出与业务丢失指标验收 |
| **V5：面向高频读取的整批计划发布** | ParseWorker 构建完整 SKU 映射后通过 atomic 指针交换发布；查询无需等待逐项构建 | 将低频计划更新与高频路由读取分开，提供一致快照的设计基础 | 当前只保证指针交换原子；波次发布时机和读对象寿命仍有 R1/R11 问题，应升级为带 waveId 的共享不可变快照 |
| **V6：面向现场恢复的本地持久化与批量事务** | 主要写操作归 DB 线程；部分查询独立连接；计划明细以 DELETE+INSERT 单事务替换并在失败时回滚 | 兼顾大波次整体更新、线程连接归属和本地查询，为故障恢复提供数据基础 | 已有实际事务与线程布局；不等于全系统单写或跨表事务完备。需验证事务启动/提交失败、大事务排队与迁移就绪 |
| **V7：固定业务报文、状态跟踪和人工补偿组合** | H7/H8 保存 payload、msgId、重试次数/时间/状态，再提供自动与人工重发入口 | 将补传从“临时重新组包”推进到“有证据的报文处置”，便于断网、接口失败和现场追查 | 实际逻辑在 HttpServer；写失败仍发送、历史自动补传过滤及恢复缺口需修复，不能称端到端恰好一次 |
| **V8：协议模拟、业务追溯与故障定位结合** | 模拟 WMS/RFID/PLC/S7；保存原文/实绩/异常，记录阶段日志和 minidump；已有实际 exe 的重扫 E2E | 将现场问题转为可重复输入，帮助区分协议、时序、数据和进程故障，形成可持续维护的验证资产 | 模拟器自测不等于整机验证，日志体系也不是完整分布式追踪；应把人工场景转为状态/DB/设备结果断言 |

源码依据：

- V1：[EPC 就绪结构](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/EpcCache.h:33)、[保留已知 SKU 并更新小车号](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/EpcCache.h:202)、[异步查询结果](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/HttpServer.cpp:5048)。
- V2：[在途与超时](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/HttpServer.cpp:5217)、[重投限制](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/HttpServer.cpp:5398)、[重复读取计时](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/EpcCache.h:236)。
- V3：[两阶段选格](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/PlcManager.cpp:445)、[选格原因输出](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/PlcManager.cpp:503)。
- V4：[HTTP 请求转交业务池](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/HttpServer.cpp:1967)、[反馈批量交付](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/PlcManager.cpp:1213)。
- V5：[完成构建后发布](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/ParseWorker.cpp:193)、[指针交换实现](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/DoubleBuffer.h:110)。
- V6：[数据库线程](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/SortingDatabase.cpp:248)、[整批明细事务](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/SortingDatabase.cpp:1164)。
- V7：[H7 固定报文与出站记录](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/HttpServer.cpp:3313)、[H8 出站记录](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/HttpServer.cpp:4160)、[人工补发](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/HttpServer.cpp:1110)。
- V8：[分类生命周期记录](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/LifecycleLogger.h:143)、[崩溃捕获](D:/WCS/WCSApp/WCS_httpServer/WCS_httpServer/main.cpp:73)、[实际 exe 的 E2E 断言](D:/WCS/WCSApp/WCS_httpServer/test/e2e_rescan_resend.py:273)、[场景模拟](D:/WCS/WCSApp/WCS_httpServer/mock_env/mock_scenario.py:195)。

### 8.3 三个最能体现技术深度的场景

**场景一：信息分批到达，但只能形成属于当前任务的一次有效动作。**

RFID 先提供 EPC 和小车号，SKU 查询稍后返回，波次又可能尚未开始。缓存需要合并已知信息，协调器需要在就绪、开工和未在途等条件成立时推进。其难点是保留已有信息、正确处理迟到结果、切换任务后隔离旧回调，并让每次推进有可追踪的原因。

当前代码实现了信息汇合和挂起重试基础；值得深化为带 waveId/generation/attemptId 的单件上下文。该能力的价值可以通过“任意到达顺序都得到同一合法结果”来验证。

**场景二：同一 EPC 再次出现，需要判断是采集重复还是新一轮物理操作。**

同一件仍在轨道上时重复识别，应抑制重复指令；操作员将已落格的件重新上料，则可能需要合法重投。现有代码保留首次采集时间、维护在途状态、按反馈解除，并以冷却和次数限制约束重投，已经超出简单的 EPC 集合判重。

进一步的技术价值在于把“实物身份、操作尝试、反馈事件”明确分开，既支持可控返工，又保持 WMS 件数与箱明细一致。应以状态转移和持久化结果证明正确性，不能只验证日志出现“重投成功”。

**场景三：业务已在物理世界发生，进程或外部接口却在确认前失败。**

一件货可能已经落格，而 H7 尚未形成；报文可能已经发出，但回执丢失；H8 可能已确认，本地更新尚未完成。这类故障无法靠重新执行整个 HTTP 请求统一处理，需要为每个中间状态定义恢复证据和补偿操作。

原始波次、实绩、出站 payload、绑定历史和人工恢复界面构成已有基础。最值得投资的下一步是把箱内明细与回传状态置于明确事务边界，再按中断位置验证恢复结果。完成后，这将成为项目中最难被通用框架替代、最能长期复用的领域能力。

### 8.4 如何将技术价值沉淀为可复用成果

| 可沉淀成果 | 对应价值 | 建议交付形式 |
|---|---|---|
| 单件执行上下文与重投规则 | V1/V2 | 类型化实体、状态迁移表、乱序/重复/迟到事件测试 |
| 可解释分配策略 | V3 | 独立规则函数，输入明确的计划/锁格/禁用快照，输出格口与原因码 |
| 通信与反馈处理组件 | V4 | 每连接拆包器、有界队列、实际发送完成事件及性能指标 |
| 波次计划快照组件 | V5 | 不可变计划、整体激活 API、明确读者所有权、跨波次隔离测试 |
| 恢复与报文投递组件 | V6/V7 | 事务仓储、Inbox/Outbox、箱明细账本、迁移与恢复矩阵 |
| 可重复的现场验证环境 | V8 | 参数化模拟器、隔离测试实例、故障注入场景及自动结果断言 |

面向技术评审，可将本项目的特点准确表述为：**围绕波次与单件实物，融合 RFID 识别、SKU 查询、PLC 执行和 WMS 回传，建立异步分拣控制及现场补偿流程；已具备线程分工、计划映射、在途重投、本地持久化和仿真验证基础，后续重点是把这些机制收敛为可证明一致性的组件。**

## 9. 本次交付与后续验证边界

本报告覆盖系统上下文、外部与构建依赖、完整第一方模块清单、实际分层、核心流程、状态/数据模型、线程模型、完整直接 include 关系、运行时闭环、架构优缺点与优化路线，并补充设计理念、关键取舍、八项高技术价值及可复用成果。

本次做了静态调用链交叉核查和 include 强连通分量分析；没有修改 C++/配置/数据库，也没有运行项目或访问现场设备。报告不替代协议验收、压力测试、长期稳定性测试和真实故障恢复演练。

当前最先落地的工作应是阶段 A 与对应回归用例；其后以单一波次上下文、可靠反馈/箱内账本和明确的消息提交点为中心抽离应用服务，再完成构建与运维治理。
