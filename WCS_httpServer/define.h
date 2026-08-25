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
//#define WMS_FEEDBACK_URL          "http://47.93.21.77:9090/gids5/service/thirdPartyData/dz_bxh_wcs_zs"        // 正式环境完整 URL（宏拼接）
//#define WMS_FEEDBACK_URL_TEST     "http://182.92.166.232/gids5/service/thirdPartyData/dz_bxh_wcs_cs"		  // 测试环境完整 URL（宏拼接）
#define WMS_FEEDBACK_URL          "https://wms.pelliot.com.cn/gwms5/service/openapi/product/skuClassificationTask/gwisSubProductClassifyOrder"        // 正式环境满箱回传 URL（H7 满箱同步到WMS）
#define WMS_FEEDBACK_URL_TEST     "https://wmstest.pelliot.com.cn:9090/gwms5/service/openapi/product/skuClassificationTask/gwisSubProductClassifyOrder"		  // 测试环境满箱回传 URL（H7 满箱同步到WMS）
#define WMS_FEEDBACK_END_URL      "https://wms.pelliot.com.cn/gwms5/service/openapi/productClasTask/gwisSubProductClassifyEndOrder"      // 正式环境完结回传 URL（H8 波次完结通知WMS）
#define WMS_FEEDBACK_END_URL_TEST "https://wmstest.pelliot.com.cn:9090/gwms5/service/openapi/productClasTask/gwisSubProductClassifyEndOrder"    // 测试环境完结回传 URL（H8 波次完结通知WMS）

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
#define WMS_METHOD_FULLBOX       "gwisSubProductClassifyOrder"    // 满箱回传（H7 满箱同步 method（WCS→WMS））
#define WMS_METHOD_END           "gwisSubProductClassifyEndOrder" // 完结回传（H8 完结回传 method（WCS→WMS））

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
// Outbox 出站配置（满箱回传/完结回传 可靠投递）
// ═══════════════════════════════════════════════════════════════════════════
// 注：OUTBOX_RETRY_MAX_DEFAULT / OUTBOX_RETRY_INTERVAL_SEC 已在 SQL 区域定义
#define OUTBOX_POLL_INTERVAL_SEC   5       // 出站调度扫描间隔（秒）
#define OUTBOX_POLL_BATCH_SIZE     10      // 每批处理消息数上限

// ═══════════════════════════════════════════════════════════════════════════
// 满箱回传（H7 满箱同步配置（WCS→WMS，需求 §10））
// ═══════════════════════════════════════════════════════════════════════════
// fromLocation 取值来源：config=使用配置项固定值, volu=使用波次明细中的 volu 字段
// TODO: RQ-04 待确认 volu 与 fromLocation 的映射关系（2026-08-04）
#define FULLBOX_FROM_LOCATION_SOURCE   "volu"   // fromLocation 取值来源（config/volu）
#define FULLBOX_DEFAULT_FROM_LOCATION  "A-66"   // 来源库位默认值（H7 fromLocation 兜底值）
#define FULLBOX_DEFAULT_TARGET_LOCATION "66"   // 目标库位默认值（H7 targetLocation 无容器号时兜底）

// ═══════════════════════════════════════════════════════════════════════════
// 分拣引擎配置
// ═══════════════════════════════════════════════════════════════════════════
#define SORTING_REQUIRE_BIND       false     // 开工闸门：未绑定禁止开工（true=强制, false=宽松）
#define SORTING_CONFLICT_POLICY    "STRICT_EXCEPTION"  // 冲突策略：STRICT_EXCEPTION=入异常口, LOOSE_FIRST=取首个匹配
#define SORTING_ALLOW_OVERRECV     false    // 是否允许超收（true=允许, false=拒收）
#define SORTING_STARTED_MODE       "first_piece"  // 已开始分拣判定口径：first_piece=首件落格, manual=人工开工, both=两者

// ═══════════════════════════════════════════════════════════════════════════
// S7 分拣增强配置（T-S7-02/06/07）
// ═══════════════════════════════════════════════════════════════════════════
#define SORTING_EPC_DEDUP          true     // EPC 任务内防重（true=按orderCode+epc去重, false=关闭）
#define SORTING_GRID_CAP_POLICY    "reject"  // 格口达计划上限策略：reject=拒收, exception=入异常口, allow=允许超收
#define SORTING_ORDERQTY_VALIDATE  "strict"  // orderQty校验模式：strict=严格(不一致则拒绝), loose=宽松(仅告警)

// ═══════════════════════════════════════════════════════════════════════════
// S8 对账/运维配置（T-S8-01/02/03/06）
// ═══════════════════════════════════════════════════════════════════════════
#define RECONCILE_DIFF_ALERT       true     // 对账差异告警开关（true=发现差异时告警, false=仅日志）
#define CANCEL_FIRST_PIECE_MUTEX       true     // 波次取消（H5取消与首件分拣互斥锁（true=加锁保证互斥, false=关闭））

// ═══════════════════════════════════════════════════════════════════════════
// 定时器/监控配置
// ═══════════════════════════════════════════════════════════════════════════
#define HEALTH_CHECK_INTERVAL_MS 60*1000  // 健康检查定时器周期(ms)
#define WORKER_WAIT_MS           3000   // 等待 ParseWorker 线程退出超时(ms)
#define CONN_LONG_DURATION_MS   10000   // 连接持续超过此值视为"长连接"(ms)
#define DOUBLE_BUFFER_CLEANUP_S     5   // DoubleBuffer 旧 Map 延迟清理时间(秒)

// ═══════════════════════════════════════════════════════════════════════════
// PLC 通信配置（与 WCSApp 一致，TCP 文本协议 + S7 协议）
//
// 识别码 = EPC编码，客户已确认EPC（商品编码）即EPC编码（2026-08-10）
// TODO: 小车号应由RFID提供，客户尚未提供RFID小车号字段，当前默认=1（2026-08-04）
// ═══════════════════════════════════════════════════════════════════════════
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
#define PLC_S7_CODE_MAX_LEN      25             // S7 包中EPC编码字段最大字节数（ASCII编码）
#define PLC_S7_CODE_OFFSET       10             // S7 包中EPC编码字段起始偏移（0-based）

// ═══════════════════════════════════════════════════════════════════════════
// 配置文件路径
// ═══════════════════════════════════════════════════════════════════════════
#define CONFIG_DIR               "config"                 // 配置文件目录（exe 同目录下）
#define CONFIG_FILE              "config/http_server.xml" // 配置文件路径（exe 同目录 config 文件夹内）
#define CONFIG_VERSION            1                        // 配置文件版本号（与软件版本匹配，不匹配时告警）

// ═══════════════════════════════════════════════════════════════════════════
// WMS → WCS HTTP API 路由（WMS 调用 WCS_httpServer 的接口路径）
// 所有路由前缀: /api/DispatchSortingCommand/
// ═══════════════════════════════════════════════════════════════════════════
#define API_PREFIX                "/api/DispatchSortingCommand"                // API 路由前缀
#define API_INSERT_WAVE_INFO      "/api/DispatchSortingCommand/InsertWaveInfo"  // ① 波次下发（H4 WMS 推送波次数据 (POST)）
#define API_BINDING_LATTICE_PORT  "/api/DispatchSortingCommand/BindingLatticePort" // ② 容器绑定（H6 WMS 绑定格口容器 (POST)）
#define API_INSERT_WAVE_IN        "/api/DispatchSortingCommand/InsertWaveIn"   // ③ 波次取消（H5 WMS 退货任务取消 (POST)）
#define API_RFID_CAR_NUM_REPORT   "/api/rfid/carNumReport"                    // ④ RFID 小车号推送（RFID 主动推送 EPC→barcode+carNum 映射 (POST)）

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
#define DEFAULT_EXPECTED_BIND_COUNT 66     // 期望绑定数量默认值（波次下发时校验全部绑定用，可配置）
#define FEEDBACK_DISPLAY_MAX       3      // PLC 反馈批量展示上限（日志中最多显示前N条详情）
#define GRID_KEY_PADDING           3      // 格口号零填充宽度（WMS 格式: "001"~"066"，与绑定/PLC/存储 key 一致）

// ═══════════════════════════════════════════════════════════════════════════
// ── 小车号配置 ──
// 小车号通过 RFID 查询返回（T-S4-02），格式为 3 位补零（如 "001"~"008"）
// RFID 未返回或查询失败时使用 DEFAULT_CAR_NUM 兜底
// 异常落格（无匹配/无绑定）使用 EXCEPTION_CAR_NUM，便于溯源
#define DEFAULT_CAR_NUM           1       // 兜底小车号（RFID未返回时使用），数字 1-999
#define EXCEPTION_CAR_NUM         0       // 异常口小车号（无匹配/无绑定时使用），数字 0=异常

// ═══════════════════════════════════════════════════════════════════════════
// UI 定时刷新间隔（MainWindow 每秒轮询所有面板）
// ═══════════════════════════════════════════════════════════════════════════
#define UI_REFRESH_INTERVAL_MS   1000     // UI 状态刷新周期(ms)：波次/PLC/绑定面板全量刷新
#define LOG_FLUSH_INTERVAL_MS     100     // 日志批量刷新周期(ms)：缓冲→QTextEdit，防高频卡死
#define LOG_FLUSH_MAX_BATCH_SIZE  100     // 单次日志刷新最大条数（防止一次刷太多卡UI）

// ═══════════════════════════════════════════════════════════════════════════
// RFID 缓存配置（RFID 主动推送模式，不再主动查询）
// ═══════════════════════════════════════════════════════════════════════════
#define RFID_CACHE_TTL_SEC         300      // EPC 本地缓存 TTL（秒，默认5分钟）
#define RFID_QUERY_URL             "http://127.0.0.1:9100/open-api/rfid/query"  // RFID SKU-EPC 绑定查询 URL（查询 EPC→barcode 映射）
#define RFID_QUERY_TIMEOUT_MS      5000     // RFID 查询超时(ms)，默认5秒
#define SKU_QUERY_MAX_RETRY        2        // SKU 查询最大重试次数（超时/失败后最多重试2次）
#define SKU_QUERY_RETRY_INTERVAL_MS 3000    // SKU 查询重试间隔(ms)，默认3秒
#define NOT_READY_RETRY_MAX         3        // 未就绪(carNum未到)最大重试次数
#define NOT_READY_RETRY_INTERVAL_MS 5000     // 未就绪重试间隔(ms)，默认5秒
#define PLC_SEND_TIMEOUT_MS         1000     // RFID推送→PLC发送超时阈值(ms)，超过则入异常格口（现场实时性要求≤1s）

// ═══════════════════════════════════════════════════════════════════════════
// WMS 回传响应日志截断（防止超长响应体撑满日志文件）
// ═══════════════════════════════════════════════════════════════════════════
#define RESP_BODY_LOG_TRUNCATE    200     // WMS 回传响应体在日志中截断长度（字符数）
#define RAW_REQ_BODY_LOG_LEN      500     // 原始请求 Body 在日志中截断长度（字符数，完整记录 queryString）

// ═══════════════════════════════════════════════════════════════════════════
// 调试模式开关（联调时改为 true，发布时改回 false）
// ═══════════════════════════════════════════════════════════════════════════
#define DEBUG_LOG_FULL_BODY       false   // 联调时改为 true，打印完整请求/响应体（不截断）

// ═══════════════════════════════════════════════════════════════════════════
// 分拣数据本地存储（SQLite）
// ═══════════════════════════════════════════════════════════════════════════
// 分拣数据本地存储（SQLite）—— 数据库路径
// ═══════════════════════════════════════════════════════════════════════════
#define SORTING_DB_DIR            "data"                    // 数据库文件目录（exe 同目录下）
#define SORTING_DB_FILE           "data/sorting_records.db" // 数据库文件路径（相对于 exe 目录）
#define SORTING_HISTORY_DB_FILE   "data/wave_history.db"    // 历史波次数据库（完结波次快照存档）
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
//   barcode    — EPC编码，用于查询追溯
//   grid_num   — 格口号，分拣落格的目标格口
//   car_num    — 小车号（3字段格式时使用，5字段格式时=首车）
//   first_car  — 首车号（5字段 PLC 反馈格式专用）
//   last_car   — 尾车号（5字段 PLC 反馈格式专用）
//   grid_count — 配货件数，该格口该EPC的配货数量
//   volu       — 来源库位，货物在原仓库的存放位置
//   sort_time  — 分拣完成时间，PLC 反馈落格的时间戳
//   create_time— 记录创建时间，写入数据库的时间
#define SQL_CREATE_TABLE_SORTING \
    "CREATE TABLE IF NOT EXISTS sorting_records (" \
    "  id          INTEGER PRIMARY KEY AUTOINCREMENT," \
    "  order_code  TEXT    NOT NULL DEFAULT ''," \
    "  barcode     TEXT    NOT NULL DEFAULT ''," \
    "  sku         TEXT    NOT NULL DEFAULT ''," \
    "  grid_num    TEXT    NOT NULL DEFAULT ''," \
    "  car_num     TEXT    NOT NULL DEFAULT '1'," \
    "  first_car   TEXT    NOT NULL DEFAULT ''," \
    "  last_car    TEXT    NOT NULL DEFAULT ''," \
    "  grid_count  INTEGER NOT NULL DEFAULT 0," \
    "  volu        TEXT    NOT NULL DEFAULT ''," \
    "  sort_time   TEXT    NOT NULL DEFAULT ''," \
    "  create_time TEXT    NOT NULL DEFAULT ''" \
    ")"

// ──── 索引：加速常用查询 ────
// 按EPC编码查询索引 — 加速按EPC编码搜索历史分拣记录
#define SQL_CREATE_INDEX_BARCODE   "CREATE INDEX IF NOT EXISTS idx_barcode    ON sorting_records(barcode)"
// 按波次号查询索引 — 加速按波次号查询该波次下所有分拣记录
#define SQL_CREATE_INDEX_ORDER     "CREATE INDEX IF NOT EXISTS idx_order_code ON sorting_records(order_code)"
// 按分拣时间查询索引 — 加速按时间范围查询（如查询某天的分拣记录）
#define SQL_CREATE_INDEX_TIME      "CREATE INDEX IF NOT EXISTS idx_sort_time  ON sorting_records(sort_time)"

// ──── 公共查询字段列表（SELECT 子句复用）────
// 查询所有字段，用于各种 SELECT 语句拼接，避免重复书写字段列表
#define SQL_SELECT_FIELDS  "SELECT id, order_code, barcode, sku, grid_num, car_num, first_car, last_car, grid_count, volu, sort_time, create_time "

// ──── 插入记录：PLC 落格反馈时写入一条分拣记录 ────
// 使用参数化查询（?占位符），防止 SQL 注入，字段顺序与建表语句一致
#define SQL_INSERT_RECORD \
    "INSERT INTO sorting_records " \
    "(order_code, barcode, sku, grid_num, car_num, first_car, last_car, grid_count, volu, sort_time, create_time) " \
    "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"

// ──── 查询：按不同条件检索分拣记录 ────
// 按EPC编码查询 — 输入EPC编码，返回该EPC编码的所有分拣历史（按时间倒序）→ 状态=已分拣
#define SQL_QUERY_BY_BARCODE       SQL_SELECT_FIELDS "FROM sorting_records WHERE barcode = ? ORDER BY id DESC LIMIT ?"
// 按时间范围查询 — 输入起始和结束时间，返回该时间段内的分拣记录（按时间倒序）→ 状态=已分拣
#define SQL_QUERY_BY_TIME          SQL_SELECT_FIELDS "FROM sorting_records WHERE sort_time >= ? AND sort_time <= ? ORDER BY id DESC LIMIT ?"
// 按波次号查询 — 输入波次号，返回该波次下的所有分拣记录（按时间倒序）→ 状态=已分拣
#define SQL_QUERY_BY_ORDER         SQL_SELECT_FIELDS "FROM sorting_records WHERE order_code = ? ORDER BY id DESC LIMIT ?"
// 查询全部记录 — 不设条件，返回最新的分拣记录（按时间倒序）→ 状态=已分拣
#define SQL_QUERY_ALL              SQL_SELECT_FIELDS "FROM sorting_records ORDER BY id DESC LIMIT ?"

// ──── UI 查询：待分拣明细（计划表 − 已落格，互斥去重）────
// 判定规则：
//   - sorting_records 有记录 → 已落格 → 「已分拣」
//   - return_wave_item 有计划且同波次同EPC编码不在 sorting_records → 「待分拣」
// 用 NOT EXISTS 保证同一 order_code+SKU 不会同时出现两种状态
#define SQL_QUERY_PENDING_BY_BARCODE \
    "SELECT i.order_code, i.inco, i.grid_num, i.plan_qty, i.volu " \
    "FROM return_wave_item i " \
    "WHERE i.inco = ? " \
    "AND NOT EXISTS (" \
    "  SELECT 1 FROM sorting_records s " \
    "  WHERE s.barcode = i.inco AND s.order_code = i.order_code" \
    ") " \
    "ORDER BY i.id DESC LIMIT ?"

// 按波次创建时间范围补充「待分拣」（UI 无EPC编码、按日期查询时）
#define SQL_QUERY_PENDING_BY_WAVE_TIME \
    "SELECT i.order_code, i.inco, i.grid_num, i.plan_qty, i.volu " \
    "FROM return_wave_item i " \
    "INNER JOIN return_wave w ON w.order_code = i.order_code " \
    "WHERE w.created_at >= ? AND w.created_at <= ? " \
    "AND NOT EXISTS (" \
    "  SELECT 1 FROM sorting_records s " \
    "  WHERE s.barcode = i.inco AND s.order_code = i.order_code" \
    ") " \
    "ORDER BY i.id DESC LIMIT ?"

// 按EPC编码查询待分拣明细（计划表 − 已落格，互斥去重）— 同上，用于 queryAllWithPending
// ──── 查询全部待分拣明细（无EPC编码过滤，用于留空查全部）────
// 判定规则同 SQL_QUERY_PENDING_BY_BARCODE，但不按EPC编码过滤
#define SQL_QUERY_ALL_PENDING \
    "SELECT i.order_code, i.inco, i.grid_num, i.plan_qty, i.volu " \
    "FROM return_wave_item i " \
    "WHERE NOT EXISTS (" \
    "  SELECT 1 FROM sorting_records s " \
    "  WHERE s.barcode = i.inco AND s.order_code = i.order_code" \
    ") " \
    "ORDER BY i.id DESC LIMIT ?"

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

// ──── 迁移：为旧版 sorting_records 表添加 first_car/last_car 列（V1→V2 兼容）────
// SQLite 不支持 ALTER TABLE ADD COLUMN IF NOT EXISTS，通过 try-exec 忽略重复列错误
#define SQL_ALTER_ADD_FIRST_CAR    "ALTER TABLE sorting_records ADD COLUMN first_car TEXT NOT NULL DEFAULT ''"
#define SQL_ALTER_ADD_LAST_CAR     "ALTER TABLE sorting_records ADD COLUMN last_car  TEXT NOT NULL DEFAULT ''"
#define SQL_ALTER_ADD_SKU          "ALTER TABLE sorting_records ADD COLUMN sku      TEXT NOT NULL DEFAULT ''"

// ═══════════════════════════════════════════════════════════════════════════
// 波次历史存档表（轻量证据，仅记录关键摘要，不复制全部数据）
// ═══════════════════════════════════════════════════════════════════════════
// 用途：主数据库查不到时作为追溯证据，一表一记录，不冗余
#define SQL_CREATE_TABLE_WAVE_HISTORY \
    "CREATE TABLE IF NOT EXISTS wave_history (" \
    "  order_code      TEXT    PRIMARY KEY," \
    "  order_qty       INTEGER NOT NULL DEFAULT 0," \
    "  status          INTEGER NOT NULL DEFAULT 0," \
    "  status_text     TEXT    NOT NULL DEFAULT ''," \
    "  start_time      TEXT    NOT NULL DEFAULT ''," \
    "  end_time        TEXT    NOT NULL DEFAULT ''," \
    "  total_items     INTEGER NOT NULL DEFAULT 0," \
    "  sorted_count    INTEGER NOT NULL DEFAULT 0," \
    "  exception_count INTEGER NOT NULL DEFAULT 0," \
    "  fullbox_count   INTEGER NOT NULL DEFAULT 0," \
    "  end_result      TEXT    NOT NULL DEFAULT ''," \
    "  created_at      TEXT    NOT NULL DEFAULT ''" \
    ")"
#define SQL_INSERT_WAVE_HISTORY \
    "INSERT OR REPLACE INTO wave_history " \
    "(order_code, order_qty, status, status_text, start_time, end_time, " \
    " total_items, sorted_count, exception_count, fullbox_count, end_result, created_at) " \
    "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"

// ═══════════════════════════════════════════════════════════════════════════
// 退货任务核心数据表（S0 阶段落地，需求 §12 数据模型）
// ═══════════════════════════════════════════════════════════════════════════

// ──── 建表：退货波次表 ────
// 存储 WMS 下发的波次头信息，一个波次对应一条记录
// orderCode  — 波次号（主键，UNIQUE）
// orderQty   — 波次总件数（WMS 推送值）
// status     — 波次状态码（见 WaveStatus 枚举：0=CREATED, 1=BOUND, 2=SORTING, 3=FULLBOX_SYNC, 4=CANCEL_PENDING, 5=CANCELLED, 6=ENDING, 7=FINISHED, 8=HELD）
// created_at — 波次创建时间（波次下发（H4 下发时间））
// updated_at — 最后更新时间（状态变更时更新）
#define SQL_CREATE_TABLE_RETURN_WAVE \
    "CREATE TABLE IF NOT EXISTS return_wave (" \
    "  order_code TEXT PRIMARY KEY," \
    "  order_qty  INTEGER NOT NULL DEFAULT 0," \
    "  status     INTEGER NOT NULL DEFAULT 0," \
    "  created_at TEXT NOT NULL DEFAULT ''," \
    "  updated_at TEXT NOT NULL DEFAULT ''" \
    ")"

// ──── 建表：退货波次明细表 ────
// 存储波次中每条商品→格口的分配明细
// order_code — 波次号（外键，关联 return_wave）
// inco       — SKU编码（客户确认 2026-08-14，H4 下发 inco 字段为 SKU 编码）
// grid_num   — 格口号
// grid_type  — 格口属性（0=分类, 1=异常, 2=发货状态）
// plan_qty   — 计划件数（WMS 下发的 gridNumber）
// sorted_qty — 已分拣件数（PLC 落格确认后 +1）
// volu       — 来源库位/体积
#define SQL_CREATE_TABLE_RETURN_WAVE_ITEM \
    "CREATE TABLE IF NOT EXISTS return_wave_item (" \
    "  id         INTEGER PRIMARY KEY AUTOINCREMENT," \
    "  order_code TEXT    NOT NULL DEFAULT ''," \
    "  inco       TEXT    NOT NULL DEFAULT ''," \
    "  grid_num   TEXT    NOT NULL DEFAULT ''," \
    "  grid_type  TEXT    NOT NULL DEFAULT '0'," \
    "  plan_qty   INTEGER NOT NULL DEFAULT 0," \
    "  sorted_qty INTEGER NOT NULL DEFAULT 0," \
    "  volu       TEXT    NOT NULL DEFAULT ''," \
    "  obx_code   TEXT    NOT NULL DEFAULT ''" \
    ")"

// ──── 建表：格口容器绑定表 ────
// 存储格口与容器的绑定关系，支持绑定历史追溯
// grid_num    — 格口号
// boxcode     — 容器号
// order_code  — 关联波次号（通过格口反查活跃任务）
// active      — 是否当前活跃绑定（1=活跃, 0=已归档）
// bind_time   — 绑定时间
// unbind_time — 解绑时间（旧绑定归档时填写）
#define SQL_CREATE_TABLE_GRID_BOX_BIND \
    "CREATE TABLE IF NOT EXISTS grid_box_bind (" \
    "  id          INTEGER PRIMARY KEY AUTOINCREMENT," \
    "  grid_num    TEXT    NOT NULL DEFAULT ''," \
    "  boxcode     TEXT    NOT NULL DEFAULT ''," \
    "  order_code  TEXT    NOT NULL DEFAULT ''," \
    "  active      INTEGER NOT NULL DEFAULT 1," \
    "  bind_time   TEXT    NOT NULL DEFAULT ''," \
    "  unbind_time TEXT    NOT NULL DEFAULT ''" \
    ")"

// ──── 建表：分拣流水表 ────
// 存储每次 PLC 落格反馈的完整分拣明细
// order_code — 波次号
// epc        — EPC 标签（用于防重，order_code+epc 唯一键）
// sku        — 商品编码/SKU
// grid_num   — 落格格口号
// boxcode    — 当前容器号
// result     — 分拣结果（success=成功, exception=异常）
// reason     — 失败原因（成功时为空）
// sort_time  — 分拣完成时间
#define SQL_CREATE_TABLE_SORT_TXN \
    "CREATE TABLE IF NOT EXISTS sort_txn (" \
    "  id         INTEGER PRIMARY KEY AUTOINCREMENT," \
    "  order_code TEXT    NOT NULL DEFAULT ''," \
    "  epc        TEXT    NOT NULL DEFAULT ''," \
    "  sku        TEXT    NOT NULL DEFAULT ''," \
    "  grid_num   TEXT    NOT NULL DEFAULT ''," \
    "  boxcode    TEXT    NOT NULL DEFAULT ''," \
    "  result     TEXT    NOT NULL DEFAULT 'success'," \
    "  reason     TEXT    NOT NULL DEFAULT ''," \
    "  sort_time  TEXT    NOT NULL DEFAULT ''" \
    ")"

// ──── 建表：满箱回传（H7） 满箱出站表 ────
// 存储满箱同步 WMS 的出站消息，支持失败重试
// msg_id     — 消息唯一标识（UUID，幂等键）
// order_code — 波次号
// boxcode    — 容器号
// payload    — 完整请求 JSON 报文
// status     — 状态（pending=待发送, success=成功, failed=失败耗尽重试）
// retry_count— 已重试次数
// next_retry — 下次重试时间
// created_at — 创建时间
#define SQL_CREATE_TABLE_OUTBOX_FULLBOX \
    "CREATE TABLE IF NOT EXISTS outbox_fullbox (" \
    "  msg_id      TEXT PRIMARY KEY," \
    "  order_code  TEXT    NOT NULL DEFAULT ''," \
    "  boxcode     TEXT    NOT NULL DEFAULT ''," \
    "  payload     TEXT    NOT NULL DEFAULT ''," \
    "  status      TEXT    NOT NULL DEFAULT 'pending'," \
    "  retry_count INTEGER NOT NULL DEFAULT 0," \
    "  next_retry  TEXT    NOT NULL DEFAULT ''," \
    "  created_at  TEXT    NOT NULL DEFAULT ''" \
    ")"

// ──── 建表：完结回传（H8） 完结出站表 ────
// 存储任务完结回传 WMS 的出站消息，支持失败重试
// 字段含义同 outbox_fullbox
#define SQL_CREATE_TABLE_OUTBOX_END \
    "CREATE TABLE IF NOT EXISTS outbox_end (" \
    "  msg_id      TEXT PRIMARY KEY," \
    "  order_code  TEXT    NOT NULL DEFAULT ''," \
    "  payload     TEXT    NOT NULL DEFAULT ''," \
    "  status      TEXT    NOT NULL DEFAULT 'pending'," \
    "  retry_count INTEGER NOT NULL DEFAULT 0," \
    "  next_retry  TEXT    NOT NULL DEFAULT ''," \
    "  created_at  TEXT    NOT NULL DEFAULT ''" \
    ")"

// ──── 建表：异常记录表 ────
// 存储分拣过程中的异常事件，供查询和人工结案
// type       — 异常类型（read_fail=读码失败, no_bind=无绑定, no_match=无匹配格口, conflict=冲突, device_fail=设备失败）
// order_code — 波次号
// epc        — EPC 标签
// sku        — 商品编码
// reason     — 异常原因描述
// handled    — 是否已人工处理（0=未处理, 1=已处理）
// time       — 异常发生时间
#define SQL_CREATE_TABLE_EXCEPTION_RECORD \
    "CREATE TABLE IF NOT EXISTS exception_record (" \
    "  id         INTEGER PRIMARY KEY AUTOINCREMENT," \
    "  type       TEXT    NOT NULL DEFAULT ''," \
    "  order_code TEXT    NOT NULL DEFAULT ''," \
    "  epc        TEXT    NOT NULL DEFAULT ''," \
    "  sku        TEXT    NOT NULL DEFAULT ''," \
    "  reason     TEXT    NOT NULL DEFAULT ''," \
    "  handled    INTEGER NOT NULL DEFAULT 0," \
    "  time       TEXT    NOT NULL DEFAULT ''" \
    ")"

// ──── 索引：新表的常用查询索引 ────
// 按波次号查询明细 — 加速按波次查询所有分配明细
#define SQL_CREATE_INDEX_WAVE_ITEM_ORDER  "CREATE INDEX IF NOT EXISTS idx_wave_item_order  ON return_wave_item(order_code)"
// 按EPC编码查待分拣 — 加速 UI EPC编码查询
#define SQL_CREATE_INDEX_WAVE_ITEM_INCO   "CREATE INDEX IF NOT EXISTS idx_wave_item_inco   ON return_wave_item(inco)"
// 按波次号查询流水 — 加速按波次查询分拣流水
#define SQL_CREATE_INDEX_SORT_TXN_ORDER   "CREATE INDEX IF NOT EXISTS idx_sort_txn_order   ON sort_txn(order_code)"
// EPC+波次号联合唯一索引 — 防重：同一波次内同一 EPC 不重复扣减
#define SQL_CREATE_INDEX_SORT_TXN_EPC     "CREATE UNIQUE INDEX IF NOT EXISTS idx_sort_txn_epc     ON sort_txn(order_code, epc)"
// 按格口+活跃状态查询绑定 — 加速查询当前活跃绑定
#define SQL_CREATE_INDEX_BIND_GRID_ACTIVE "CREATE INDEX IF NOT EXISTS idx_bind_grid_active ON grid_box_bind(grid_num, active)"
// 按波次号查询出站 — 加速按波次查询出站消息
#define SQL_CREATE_INDEX_OUTBOX_ORDER     "CREATE INDEX IF NOT EXISTS idx_outbox_order     ON outbox_fullbox(order_code)"
// 按状态+重试时间查询 — 加速 Outbox 重试调度（查询待发送且已到重试时间的消息）
#define SQL_CREATE_INDEX_OUTBOX_RETRY     "CREATE INDEX IF NOT EXISTS idx_outbox_retry     ON outbox_fullbox(status, next_retry)"

// ──── 波次操作 ────
// 插入/更新波次头 — INSERT OR REPLACE，支持幂等覆盖（未分拣时）
#define SQL_UPSERT_RETURN_WAVE \
    "INSERT OR REPLACE INTO return_wave (order_code, order_qty, status, created_at, updated_at) VALUES (?, ?, ?, ?, ?)"
// 按波次号查询波次头
#define SQL_SELECT_WAVE_BY_ORDER \
    "SELECT order_code, order_qty, status, created_at, updated_at FROM return_wave WHERE order_code = ?"
// 更新波次状态
#define SQL_UPDATE_WAVE_STATUS \
    "UPDATE return_wave SET status = ?, updated_at = ? WHERE order_code = ?"
// 查询最近一条未完成波次（排除已取消 WAVE_CANCELLED=6 和已完成 WAVE_FINISHED=8）
// 用于软件重启后恢复未完成波次数据
#define SQL_SELECT_LATEST_UNFINISHED_WAVE \
    "SELECT order_code, order_qty, status, created_at, updated_at FROM return_wave " \
    "WHERE status NOT IN (6, 8) ORDER BY updated_at DESC LIMIT 1"

// ──── 波次明细操作 ────
// 插入明细行
#define SQL_INSERT_WAVE_ITEM \
    "INSERT INTO return_wave_item (order_code, inco, grid_num, grid_type, plan_qty, sorted_qty, volu, obx_code) VALUES (?, ?, ?, ?, ?, 0, ?, ?)"
// 按波次号查询所有明细
#define SQL_SELECT_WAVE_ITEMS \
    "SELECT id, order_code, inco, grid_num, grid_type, plan_qty, sorted_qty, volu, obx_code FROM return_wave_item WHERE order_code = ?"
// 更新已分拣件数（sorted_qty + 1）
// 格口按整数比较，兼容 WMS "3" 与 PLC 反馈 "003"
#define SQL_INCREMENT_SORTED_QTY \
    "UPDATE return_wave_item SET sorted_qty = sorted_qty + 1 " \
    "WHERE order_code = ? AND inco = ? AND CAST(grid_num AS INTEGER) = CAST(? AS INTEGER)"

// ──── 容器绑定操作 ────
// 插入新绑定（active=1）
#define SQL_INSERT_GRID_BIND \
    "INSERT INTO grid_box_bind (grid_num, boxcode, order_code, active, bind_time, unbind_time) VALUES (?, ?, ?, 1, ?, '')"
// 归档旧绑定（同格口旧绑定设为 active=0）
#define SQL_ARCHIVE_OLD_BIND \
    "UPDATE grid_box_bind SET active = 0, unbind_time = ? WHERE grid_num = ? AND active = 1"
// 查询格口当前活跃绑定
#define SQL_SELECT_ACTIVE_BIND \
    "SELECT boxcode, order_code, bind_time FROM grid_box_bind WHERE grid_num = ? AND active = 1 LIMIT 1"

// ──── 分拣流水操作 ────
// 插入分拣流水
#define SQL_INSERT_SORT_TXN \
    "INSERT INTO sort_txn (order_code, epc, sku, grid_num, boxcode, result, reason, sort_time) VALUES (?, ?, ?, ?, ?, ?, ?, ?)"
// 按波次+EPC 查询流水（防重检查）
#define SQL_SELECT_TXN_BY_EPC \
    "SELECT id FROM sort_txn WHERE order_code = ? AND epc = ? AND result = 'success' LIMIT 1"

// ──── Outbox 出站操作 ────
// 插入出站消息（满箱回传（H7））
#define SQL_INSERT_OUTBOX_FULLBOX \
    "INSERT INTO outbox_fullbox (msg_id, order_code, boxcode, payload, status, retry_count, next_retry, created_at) VALUES (?, ?, ?, ?, 'pending', 0, ?, ?)"
// 插入出站消息（完结回传（H8））
#define SQL_INSERT_OUTBOX_END \
    "INSERT INTO outbox_end (msg_id, order_code, payload, status, retry_count, next_retry, created_at) VALUES (?, ?, ?, 'pending', 0, ?, ?)"
// 查询待重试的 满箱回传（H7） 出站消息
#define SQL_SELECT_OUTBOX_PENDING \
    "SELECT msg_id, order_code, boxcode, payload, retry_count FROM outbox_fullbox WHERE status = 'pending' AND next_retry <= ? ORDER BY created_at ASC LIMIT ?"
// 查询待重试的 完结回传（H8） 出站消息
#define SQL_SELECT_OUTBOX_END_PENDING \
    "SELECT msg_id, order_code, payload, retry_count FROM outbox_end WHERE status = 'pending' AND next_retry <= ? ORDER BY created_at ASC LIMIT ?"
// 更新出站消息状态（满箱回传（H7））
#define SQL_UPDATE_OUTBOX_STATUS \
    "UPDATE outbox_fullbox SET status = ?, retry_count = retry_count + 1, next_retry = ? WHERE msg_id = ?"
// 更新出站消息状态（完结回传（H8））
#define SQL_UPDATE_OUTBOX_END_STATUS \
    "UPDATE outbox_end SET status = ?, retry_count = retry_count + 1, next_retry = ? WHERE msg_id = ?"
// 标记出站成功（满箱回传（H7））
#define SQL_MARK_OUTBOX_SUCCESS \
    "UPDATE outbox_fullbox SET status = 'success' WHERE msg_id = ?"
// 标记出站成功（完结回传（H8））
#define SQL_MARK_OUTBOX_END_SUCCESS \
    "UPDATE outbox_end SET status = 'success' WHERE msg_id = ?"
// 按波次号查询所有出站消息（人工重发用）
#define SQL_SELECT_OUTBOX_BY_ORDER \
    "SELECT msg_id, order_code, boxcode, payload, retry_count FROM outbox_fullbox WHERE order_code = ? AND status = 'pending'"
// 按 msgId 查询单条出站消息（人工重发用）
#define SQL_SELECT_OUTBOX_BY_MSGID \
    "SELECT msg_id, order_code, boxcode, payload, retry_count FROM outbox_fullbox WHERE msg_id = ?"
// 按 msgId 查询单条 完结回传（H8 完结出站消息（S6 人工重发用））
#define SQL_SELECT_OUTBOX_END_BY_MSGID \
    "SELECT msg_id, order_code, payload, retry_count FROM outbox_end WHERE msg_id = ?"

// ──── 异常记录操作 ────
// 插入异常记录
#define SQL_INSERT_EXCEPTION \
    "INSERT INTO exception_record (type, order_code, epc, sku, reason, handled, time) VALUES (?, ?, ?, ?, ?, 0, ?)"
// 查询未处理异常
#define SQL_SELECT_OPEN_EXCEPTIONS \
    "SELECT id, type, order_code, epc, sku, reason, time FROM exception_record WHERE order_code = ? AND handled = 0 ORDER BY id DESC"

// ──── Outbox 配置 ────
// 出站重试次数上限（默认 10 次）
#define OUTBOX_RETRY_MAX_DEFAULT   3       // 完结回传（H8）最大重试次数（每波次仅1次，失败记录异常，不阻塞新波次）
#define OUTBOX_RETRY_MAX_H7        2       // 满箱回传（H7）最大重试次数（每波次可能多次触发，降低重试避免堆积）
// 出站重试间隔（秒，默认 30 秒）
#define OUTBOX_RETRY_INTERVAL_SEC  30

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
//   LIFE_LOG(fmt, ...)                  → LIFECYCLE模块（EPC编码全链路追踪） → ./log/LIFECYCLE/lifecycle.log
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
