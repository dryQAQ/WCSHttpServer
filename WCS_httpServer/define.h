#pragma once
// ============================================================================
// define.h — 全局配置宏定义
//
// 所有端口号、IP地址、URL、AppKey、数值阈值集中管理，便于设置和修改。
// 修改此处宏后重新编译即可生效，无需逐个文件查找硬编码。
// ============================================================================

// ═══════════════════════════════════════════════════════════════════════════
// WMS HTTP 服务端口
// ═══════════════════════════════════════════════════════════════════════════
#define WMS_LISTEN_PORT         8191     // WMS 波次推送 + 查询 API 监听端口

// ═══════════════════════════════════════════════════════════════════════════
// WMS 回传接口地址（波次完结时 WCS → WMS）
// ═══════════════════════════════════════════════════════════════════════════
#define WMS_FEEDBACK_HOST         "http://47.93.21.77:9090"                      // 正式环境 WMS 主机
#define WMS_FEEDBACK_HOST_TEST    "http://182.92.166.232"                        // 测试环境 WMS 主机
#define WMS_FEEDBACK_PATH         "/gids5/service/thirdPartyData/dz_bxh_wcs_zs" // 正式环境回传路径
#define WMS_FEEDBACK_PATH_TEST    "/gids5/service/thirdPartyData/dz_bxh_wcs_cs" // 测试环境回传路径
#define WMS_FEEDBACK_URL          "http://47.93.21.77:9090/gids5/service/thirdPartyData/dz_bxh_wcs_zs"        // 正式环境完整 URL（宏拼接）
#define WMS_FEEDBACK_URL_TEST     "http://182.92.166.232/gids5/service/thirdPartyData/dz_bxh_wcs_cs"		  // 测试环境完整 URL（宏拼接）

// ═══════════════════════════════════════════════════════════════════════════
// WMS 认证 AppKey（放入 HTTP Header: AppKey=xxx）
// ═══════════════════════════════════════════════════════════════════════════
#define WMS_APPKEY               "dz_bxh_wcs_zs"     // 正式环境 AppKey
#define WMS_APPKEY_TEST          "dz_bxh_wcs_cs"     // 测试环境 AppKey

// ═══════════════════════════════════════════════════════════════════════════
// WMS 回传请求参数
// ═══════════════════════════════════════════════════════════════════════════
#define WMS_ORDER_TYPE           "02"                 // 业务类型（02=退货分类）
#define WMS_OPERUSER_CODE        "admin"              // 操作人编码（WMS 接口要求）
#define WMS_OPERUSER_NAME        "管理员"              // 操作人名称（WMS 接口要求）
#define WMS_WAREHOUSE_CODE       "A"                   // 仓库编码
#define WMS_GOODS_OWNER          "BXH_ZS"              // 货主编码
#define WMS_METHOD_LOCK          "gwisSubProductClassifyOrder" // 锁格回传 method

// ═══════════════════════════════════════════════════════════════════════════
// HP-Socket 服务器配置
// ═══════════════════════════════════════════════════════════════════════════
#define HP_WORKER_THREADS       16       // I/O 工作线程数（16核机器，充分利用IOCP）
#define HP_MAX_CONNECTIONS     600      // 最大连接数，防止连接池耗尽
#define HP_KEEPALIVE_TIME_MS 30000      // KeepAlive 探活时间(ms)，检测死连接

// ═══════════════════════════════════════════════════════════════════════════
// 业务线程池配置
// ═══════════════════════════════════════════════════════════════════════════
#define BUSINESS_POOL_SIZE      90      // 业务线程池大小（16扫描仪×5并发 + HTTP回传 + 异常处理 + 余量）
#define TASK_QUEUE_MAX_SIZE      5      // 波次推送任务队列最大排队数（防止大波次突发撑爆内存）
#define POOL_OVERLOAD_MULTIPLIER  2      // 线程池过载倍数（积压任务 > 线程数×倍数 时告警）
#define PLC_SEND_POOL_SIZE       8      // PLC 发送专用线程池
#define PLC_RECV_POOL_SIZE       4      // PLC 反馈接收专用线程池（落格反馈→分拣标记→SQLite写入，参考WCSApp m_threadPoolPLCRecvPtr）
#define CAMERA_PROC_POOL_SIZE    4      // 相机数据处理专用线程池（相机扫描→格口查询→PLC发送，参考WCSApp m_threadPoolPtr）（S7 DBWrite 同步阻塞10~100ms，需异步化防I/O线程阻塞）

// ═══════════════════════════════════════════════════════════════════════════
// 网络请求超时配置
// ═══════════════════════════════════════════════════════════════════════════
#define HTTP_TIMEOUT_MS          3000   // HTTP 回传请求超时(ms)
#define WAVE_COMPLETE_TIMEOUT_MS 2000   // 波次完成回传超时(ms) —— 遵循 project_memory 约束：回传 ≤2秒

// ═══════════════════════════════════════════════════════════════════════════
// 波次管理配置
// ═══════════════════════════════════════════════════════════════════════════
#define WAVE_MAX_RETRY           3      // 异常 SKU 最大重试次数
#define WAVE_TIMEOUT_MIN_DEFAULT 0      // 波次超时默认值(分钟，0=不超时)

// ═══════════════════════════════════════════════════════════════════════════
// 定时器/监控配置
// ═══════════════════════════════════════════════════════════════════════════
#define HEALTH_CHECK_INTERVAL_MS 10000  // 健康检查定时器周期(ms)
#define WORKER_WAIT_MS           3000   // 等待 ParseWorker 线程退出超时(ms)
#define CONN_LONG_DURATION_MS   10000   // 连接持续超过此值视为"长连接"(ms)
#define DOUBLE_BUFFER_CLEANUP_S     5   // DoubleBuffer 旧 Map 延迟清理时间(秒)

// ═══════════════════════════════════════════════════════════════════════════
// PLC 通信配置（与 WCSApp 一致，TCP 文本协议 + S7 协议）
// ═══════════════════════════════════════════════════════════════════════════
// ── 相机通信 ──
#define CAMERA_LISTEN_PORT      8193     // 相机 TCP 监听端口（相机主动连接到此端口）
#define CAMERA_MAX_CONNECTIONS    16     // 最大相机连接数

// ── TCP 文本协议 ──
#define PLC_LISTEN_PORT         8192     // PLC TCP 监听端口（PLC主动连接到此端口）
#define PLC_RECONNECT_INTERVAL_MS 3000   // PLC 断线重连间隔(ms)

// ── S7 协议 (Snap7) ──
#define PLC_S7_IP               "192.168.0.1"  // S7 PLC IP 地址
#define PLC_S7_RACK             0              // S7 机架号
#define PLC_S7_SLOT             1              // S7 槽位号
#define PLC_S7_DB_READ          77             // S7 读取DB号（锁格状态读取）
#define PLC_S7_DB_WRITE          1             // S7 写入DB号（分拣指令写入）
#define PLC_S7_DB_WRITE_OFFSET 1000            // S7 写入偏移地址
#define PLC_S7_DB_WRITE_SIZE    42             // S7 写入数据大小（字节）
#define PLC_S7_LOCK_READ_SIZE   25             // S7 锁格读取大小（字节，25字节=200位）
#define PLC_S7_LOCK_INTERVAL_MS 1000           // S7 锁格轮询间隔(ms)
#define PLC_S7_MAX_GRID_COUNT   200            // 最大格口数（锁格位图覆盖范围）
#define PLC_S7_CODE_MAX_LEN      25             // S7 包中条码字段最大字节数（ASCII编码）
#define PLC_S7_CODE_OFFSET       10             // S7 包中条码字段起始偏移（0-based）

// ═══════════════════════════════════════════════════════════════════════════
// 配置文件路径
// ═══════════════════════════════════════════════════════════════════════════
#define CONFIG_DIR               "config"                 // 配置文件目录（exe 同目录下）
#define CONFIG_FILE              "config/http_server.xml" // 配置文件路径（exe 同目录 config 文件夹内）

// ═══════════════════════════════════════════════════════════════════════════
// WMS → WCS HTTP API 路由（WMS 调用 WCS_httpServer 的接口路径）
// 所有路由前缀: /api/DispatchSortingCommand/
// ═══════════════════════════════════════════════════════════════════════════
#define API_PREFIX                "/api/DispatchSortingCommand"                // API 路由前缀
#define API_INSERT_WAVE_INFO      "/api/DispatchSortingCommand/InsertWaveInfo"  // ① WMS 推送波次数据 (POST)
#define API_BINDING_LATTICE_PORT  "/api/DispatchSortingCommand/BindingLatticePort" // ② WMS 绑定格口容器 (POST)
#define API_INSERT_WAVE_IN        "/api/DispatchSortingCommand/InsertWaveIn"   // ③ WMS 退货任务取消 (POST)

// ═══════════════════════════════════════════════════════════════════════════
// 连接监控阈值（健康检查日志分级输出 → HttpServer::logHealthStatus）
// ═══════════════════════════════════════════════════════════════════════════
#define CONN_LOG_THROTTLE_INTERVAL 100   // 连接日志节流间隔（每N个连接输出一次统计，防日志刷屏）
#define CONN_ACTIVE_WARN_THRESHOLD  50   // 活跃连接数 ≥ N → WARN 级别日志
#define CONN_ACTIVE_HIGH_THRESHOLD 100   // 活跃连接数 ≥ N → 高频日志告警（可能连接泄漏）
#define CONN_ACTIVE_ALERT_THRESHOLD 200  // 活跃连接数 ≥ N → 严重告警（连接池即将耗尽）

// ═══════════════════════════════════════════════════════════════════════════
// PLC 反馈批量缓冲（防止高频信号卡死UI）
// ═══════════════════════════════════════════════════════════════════════════
#define PLC_FEEDBACK_BATCH_INTERVAL_MS 100  // PLC 反馈批量刷新间隔(ms)
#define PLC_FEEDBACK_BATCH_MAX_SIZE    200  // PLC 反馈缓冲区上限（超过则丢弃最旧数据）

// ═══════════════════════════════════════════════════════════════════════════
// 容器格口绑定 → MainWindow 容器绑定面板
// ═══════════════════════════════════════════════════════════════════════════
#define BINDING_SLOT_COUNT        66      // 容器格口总数（= 分拣机格口数，可扩展）
#define FEEDBACK_DISPLAY_MAX       3      // PLC 反馈批量展示上限（日志中最多显示前N条详情）
#define GRID_KEY_PADDING           5      // 格口号零填充宽度（WMS 格式: "00001"~"00066"，与 UI/存储 key 一致）

// ═══════════════════════════════════════════════════════════════════════════
// UI 定时刷新间隔（MainWindow 每秒轮询所有面板）
// ═══════════════════════════════════════════════════════════════════════════
#define UI_REFRESH_INTERVAL_MS   1000     // UI 状态刷新周期(ms)：波次/PLC/绑定面板全量刷新
#define LOG_FLUSH_INTERVAL_MS     100     // 日志批量刷新周期(ms)：缓冲→QTextEdit，防高频卡死
#define LOG_FLUSH_MAX_BATCH_SIZE  100     // 单次日志刷新最大条数（防止一次刷太多卡UI）

// ═══════════════════════════════════════════════════════════════════════════
// RFID API 配置（EPC→SKU 查询）
// ═══════════════════════════════════════════════════════════════════════════
#define RFID_QUERY_URL          "http://{BaseURL}/open-api/rfid/query"  // EPC查询接口（{BaseURL} 通过 XML 配置替换）
#define RFID_TIMEOUT_MS         2000                                     // RFID查询超时(ms)
#define RFID_MAX_BATCH_SIZE     100                                      // 单次查询最大EPC数量

// ═══════════════════════════════════════════════════════════════════════════
// WMS 回传响应日志截断（防止超长响应体撑满日志文件）
// ═══════════════════════════════════════════════════════════════════════════
#define RESP_BODY_LOG_TRUNCATE    200     // WMS 回传响应体在日志中截断长度（字符数）
#define RAW_REQ_BODY_LOG_LEN      500     // 原始请求 Body 在日志中截断长度（字符数，完整记录 queryString）

// ═══════════════════════════════════════════════════════════════════════════
// 分拣数据本地存储（SQLite）
// ═══════════════════════════════════════════════════════════════════════════
// 分拣数据本地存储（SQLite）—— 数据库路径
// ═══════════════════════════════════════════════════════════════════════════
#define SORTING_DB_DIR            "data"                    // 数据库文件目录（exe 同目录下）
#define SORTING_DB_FILE           "data/sorting_records.db" // 数据库文件路径（相对于 exe 目录）
#define SORTING_DB_RETAIN_DAYS    90                        // 分拣记录保留天数
#define SORTING_QUERY_MAX_RESULTS 1000                      // 单次查询最大返回记录数
#define SORTING_QUERY_PAGE_SIZE   100                       // 表格每页显示记录数

// ═══════════════════════════════════════════════════════════════════════════
// 分拣数据本地存储（SQLite）—— SQL 语句宏
//
// 设计原则：所有 SQL 语句集中在 define.h，使用处用中文注释标注实际语句，
//          方便维护时直接理解 SQL 含义，无需跳转到宏定义。
// ═══════════════════════════════════════════════════════════════════════════

// ──── 数据库性能优化（PRAGMA 指令）────
#define SQL_PRAGMA_WAL            "PRAGMA journal_mode=WAL"       // 启用 WAL 日志模式，允许读写并发，提升高并发场景性能
#define SQL_PRAGMA_SYNC           "PRAGMA synchronous=NORMAL"     // 同步模式设为 NORMAL，在安全性和写入性能之间取得平衡
#define SQL_PRAGMA_CACHE          "PRAGMA cache_size=5000"        // 设置缓存大小为 5000 页（约 20MB），减少磁盘 I/O
#define SQL_PRAGMA_OPTIMIZE       "PRAGMA optimize"               // 执行数据库优化，清理删除记录后回收磁盘空间

// ──── 建表：分拣记录表 ────
// 创建分拣记录主表，存储每条 PLC 落格反馈的完整信息
// 字段说明：
//   id         — 自增主键，唯一标识每条记录
//   order_code — 波次号，关联 WMS 推送的波次
//   barcode    — 条码/SKU 编码，用于查询追溯
//   grid_num   — 格口号，分拣落格的目标格口
//   car_num    — 小车号，输送分拣的小车编号
//   grid_count — 配货件数，该格口该条码的配货数量
//   volu       — 来源库位，货物在原仓库的存放位置
//   sort_time  — 分拣完成时间，PLC 反馈落格的时间戳
//   create_time— 记录创建时间，写入数据库的时间
#define SQL_CREATE_TABLE_SORTING \
    "CREATE TABLE IF NOT EXISTS sorting_records (" \
    "  id          INTEGER PRIMARY KEY AUTOINCREMENT," \
    "  order_code  TEXT    NOT NULL DEFAULT ''," \
    "  barcode     TEXT    NOT NULL DEFAULT ''," \
    "  grid_num    TEXT    NOT NULL DEFAULT ''," \
    "  car_num     TEXT    NOT NULL DEFAULT '1'," \
    "  grid_count  INTEGER NOT NULL DEFAULT 0," \
    "  volu        TEXT    NOT NULL DEFAULT ''," \
    "  sort_time   TEXT    NOT NULL DEFAULT ''," \
    "  create_time TEXT    NOT NULL DEFAULT ''" \
    ")"

// ──── 索引：加速常用查询 ────
// 按条码查询索引 — 加速按条码搜索历史分拣记录
#define SQL_CREATE_INDEX_BARCODE   "CREATE INDEX IF NOT EXISTS idx_barcode    ON sorting_records(barcode)"
// 按波次号查询索引 — 加速按波次号查询该波次下所有分拣记录
#define SQL_CREATE_INDEX_ORDER     "CREATE INDEX IF NOT EXISTS idx_order_code ON sorting_records(order_code)"
// 按分拣时间查询索引 — 加速按时间范围查询（如查询某天的分拣记录）
#define SQL_CREATE_INDEX_TIME      "CREATE INDEX IF NOT EXISTS idx_sort_time  ON sorting_records(sort_time)"

// ──── 公共查询字段列表（SELECT 子句复用）────
// 查询所有字段，用于各种 SELECT 语句拼接，避免重复书写字段列表
#define SQL_SELECT_FIELDS  "SELECT id, order_code, barcode, grid_num, car_num, grid_count, volu, sort_time, create_time "

// ──── 插入记录：PLC 落格反馈时写入一条分拣记录 ────
// 使用参数化查询（?占位符），防止 SQL 注入，字段顺序与建表语句一致
#define SQL_INSERT_RECORD \
    "INSERT INTO sorting_records " \
    "(order_code, barcode, grid_num, car_num, grid_count, volu, sort_time, create_time) " \
    "VALUES (?, ?, ?, ?, ?, ?, ?, ?)"

// ──── 查询：按不同条件检索分拣记录 ────
// 按条码查询 — 输入条码，返回该条码的所有分拣历史（按时间倒序）
#define SQL_QUERY_BY_BARCODE       SQL_SELECT_FIELDS "FROM sorting_records WHERE barcode = ? ORDER BY id DESC LIMIT ?"
// 按时间范围查询 — 输入起始和结束时间，返回该时间段内的分拣记录（按时间倒序）
#define SQL_QUERY_BY_TIME          SQL_SELECT_FIELDS "FROM sorting_records WHERE sort_time >= ? AND sort_time <= ? ORDER BY id DESC LIMIT ?"
// 按波次号查询 — 输入波次号，返回该波次下的所有分拣记录（按时间倒序）
#define SQL_QUERY_BY_ORDER         SQL_SELECT_FIELDS "FROM sorting_records WHERE order_code = ? ORDER BY id DESC LIMIT ?"
// 查询全部记录 — 不设条件，返回最新的分拣记录（按时间倒序）
#define SQL_QUERY_ALL              SQL_SELECT_FIELDS "FROM sorting_records ORDER BY id DESC LIMIT ?"

// ──── 统计查询：汇总数据库整体情况 ────
// 统计总记录数 — 数据库中所有分拣记录的总条数
#define SQL_COUNT_ALL              "SELECT COUNT(*) FROM sorting_records"
// 统计今日记录数 — 当天（从 00:00:00 起）的分拣记录条数
#define SQL_COUNT_TODAY            "SELECT COUNT(*) FROM sorting_records WHERE sort_time >= ?"
// 统计波次总数 — 去重统计所有波次号的数量
#define SQL_COUNT_WAVES            "SELECT COUNT(DISTINCT order_code) FROM sorting_records"
// 统计格口使用数 — 去重统计所有使用过的格口号数量
#define SQL_COUNT_GRIDS            "SELECT COUNT(DISTINCT grid_num) FROM sorting_records"
// 查询最近分拣时间 — 获取最新一条分拣记录的时间，用于判断数据新鲜度
#define SQL_LAST_SORT_TIME         "SELECT sort_time FROM sorting_records ORDER BY id DESC LIMIT 1"

// ──── 清理：删除过期记录 ────
// 按分拣时间删除 N 天前的旧记录，防止数据库文件无限增长
#define SQL_DELETE_OLD             "DELETE FROM sorting_records WHERE sort_time < ?"

// ═══════════════════════════════════════════════════════════════════════════
// UI/日志限制
// ═══════════════════════════════════════════════════════════════════════════
#define LOG_MAX_BLOCK_COUNT     5000     // 日志文本框最大行数
#define LOG_MAX_TEXT_SIZE     100000     // 日志文本框最大字符数
#define LOG_RETAIN_DAYS           30     // 日志文件保留天数

// ═══════════════════════════════════════════════════════════════════════════
// 日志宏使用说明
// ═══════════════════════════════════════════════════════════════════════════
//
// 日志系统基于 hlog 框架，配置文件: log4cxx.properties
// 日志输出路径: ./log/<模块名>/<模块名>.log
//
// ─── 日志级别（由低到高）───
//   TRACE(1) < DEBUG(2) < INFO(3) < WARN(4) < ERROR(5) < FATAL(6) < OFF(7)
//
// ─── 两大宏体系 ───
//
// ① 增强型日志宏（推荐，定义在 LifecycleLogger.h）
//   格式: [时间] [TID-xxx] [模块] [函数:行号] 消息
//   特点: 携带 TraceID 用于全链路追踪，携带函数名+行号便于定位
//
//   WCS_LOG_INFO/WARN/ERROR(fmt, ...)   → WCS模块（波次管理、分拣状态） → ./log/WCS/WCS.log
//   PLC_LOG_INFO/WARN/ERROR(fmt, ...)   → PLC模块（连接、收发、心跳）   → ./log/PLC/PLC.log
//   HTTP_LOG_INFO/WARN/ERROR(fmt, ...)  → HTTP模块（请求分发、WMS回传） → ./log/HTTP/http.log
//   LIFE_LOG(fmt, ...)                  → LIFECYCLE模块（条码全链路追踪） → ./log/LIFECYCLE/lifecycle.log
//
//   使用示例:
//     WCS_LOG_INFO("波次创建成功 waveId=%s", ...);
//     PLC_LOG_ERROR("发送PLC指令失败 code=%s err=%s", ...);
//     HTTP_LOG_WARN("WMS回传超时 url=%s timeout=%dms", ...);
//
// ② 基础日志宏（定义在 hlog1.h）
//   WCS_INFO/ERROR/WARN/...  → WCS模块
//   PLC_INFO/ERROR/WARN/...  → PLC模块
//   HTTP_INFO/ERROR/WARN/... → HTTP模块 (定义在 main.cpp)
//   LOG_INFO/ERROR/WARN/...  → 默认(Run)模块
//
// ─── 使用原则 ───
//   - 新增代码优先使用增强型宏（WCS_LOG_*/PLC_LOG_*/HTTP_LOG_*/LIFE_LOG）
//   - /api/query 路径仅使用 HTTP_LOG_INFO/HTTP_LOG_WARN，避免重复日志
//   - TraceContext 自动管理线程级别的 TraceID，无需手动传递
