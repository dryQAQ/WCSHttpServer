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
#define HP_MAX_CONNECTIONS     500      // 最大连接数，防止连接池耗尽
#define HP_KEEPALIVE_TIME_MS 30000      // KeepAlive 探活时间(ms)，检测死连接

// ═══════════════════════════════════════════════════════════════════════════
// 业务线程池配置
// ═══════════════════════════════════════════════════════════════════════════
#define BUSINESS_POOL_SIZE      90      // 业务线程池大小（16扫描仪×5并发 + HTTP回传 + 异常处理 + 余量）
#define TASK_QUEUE_MAX_SIZE      5      // 波次推送任务队列最大排队数（防止大波次突发撑爆内存）
#define POOL_OVERLOAD_MULTIPLIER  2      // 线程池过载倍数（积压任务 > 线程数×倍数 时告警）
#define PLC_SEND_POOL_SIZE       8      // PLC 发送专用线程池（S7 DBWrite 同步阻塞10~100ms，需异步化防I/O线程阻塞）

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
