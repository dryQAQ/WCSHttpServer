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
// ★ 2026-09-06：WMS 网关要求 URL 带 appkey=xxx&method=xxx（参数放 XML 配置：
//   <appkey>/<appkeyTest>/<feedbackMethod>/<feedbackEndMethod>，发送时由 HttpClient 拼接）
//   满箱/锁格/波次完成等回传（非完结）→ method=gwisSubProductClassifyOrder
//   完结回传（H8）               → method=gwisSubProductClassifyEndOrder
#define WMS_FEEDBACK_URL          "https://wms.pelliot.com.cn/gwms5/service/openapi/product/skuClassificationTask/gwisSubProductClassifyOrder"        // 正式环境满箱回传 base URL（H7 满箱同步到WMS）
#define WMS_FEEDBACK_URL_TEST     "https://wmstest.pelliot.com.cn:9090/gwms5/service/openapi/product/skuClassificationTask/gwisSubProductClassifyOrder"		  // 测试环境满箱回传 base URL（H7 满箱同步到WMS）
#define WMS_FEEDBACK_END_URL      "https://wms.pelliot.com.cn/gwms5/service/openapi/productClasTask/gwisSubProductClassifyEndOrder"      // 正式环境完结回传 base URL（H8 波次完结通知WMS）
#define WMS_FEEDBACK_END_URL_TEST "https://wmstest.pelliot.com.cn:9090/gwms5/service/openapi/productClasTask/gwisSubProductClassifyEndOrder"    // 测试环境完结回传 base URL（H8 波次完结通知WMS）

// ═══════════════════════════════════════════════════════════════════════════
// WMS 认证 AppKey（放入 HTTP Header: AppKey=xxx）
// ═══════════════════════════════════════════════════════════════════════════
#define WMS_APPKEY               "dz_bxh_wcs_zs"     // 正式环境 AppKey（HTTP Header + URL 参数 appkey 同值）
#define WMS_APPKEY_TEST          "dz_bxh_dmwcs_cs"   // ★ 2026-09-06 测试环境 AppKey（gwms5 openapi 账号，HTTP Header + URL 参数 appkey 同值）

// ═══════════════════════════════════════════════════════════════════════════
// WMS 回传请求参数
// ═══════════════════════════════════════════════════════════════════════════
// ★ 2026-09-06 客户样例确认：回传报文中 业务类型=01、仓库=H、货主=BXH_CS（测试/正式同套值）
#define WMS_ORDER_TYPE           "01"                 // 业务类型（01=收货分类，客户样例确认）
#define WMS_OPERUSER_CODE        "admin"              // 操作人编码（WMS 接口要求）
#define WMS_OPERUSER_NAME        "管理员"              // 操作人名称（WMS 接口要求）
#define WMS_WAREHOUSE_CODE       "H"                   // 仓库编码
#define WMS_GOODS_OWNER          "BXH_CS"              // 货主编码
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
#define FULLBOX_FROM_LOCATION_SOURCE   "sobi"   // fromLocation 取值来源（config/volu）
#define FULLBOX_DEFAULT_FROM_LOCATION  "A-01-02"   // 来源库位默认值（H7 fromLocation 兜底值）
#define FULLBOX_DEFAULT_TARGET_LOCATION "66"   // 目标库位默认值（H7 targetLocation 无容器号时兜底）

// ──── WMS 格口编码（2026-09-07）────
// 对外（WMS 报文/接口）格口号 = 前缀 + 格口号按宽度补零，如 前缀"22"+3位：格口号 5 → "22005"
// 内部（PLC 反馈 3 位、绑定 key、DB、UI）仍用原格口号；仅对外边界转换
#define WMS_GRID_CODE_PREFIX_DEFAULT   "22"    // 格口编码前缀（空 = 关闭转换，回退现网）
#define WMS_GRID_CODE_WIDTH_DEFAULT    3       // 格口号补零宽度（与 GRID_KEY_PADDING=3 一致）

// ═══════════════════════════════════════════════════════════════════════════
// 分拣引擎配置
// ═══════════════════════════════════════════════════════════════════════════
#define SORTING_REQUIRE_BIND       false     // 开工闸门：未绑定禁止开工（true=强制, false=宽松）
#define SORTING_CONFLICT_POLICY    "LOOSE_FIRST"  // 冲突策略：STRICT_EXCEPTION=入异常口, LOOSE_FIRST=取首个匹配
#define SORTING_ALLOW_OVERRECV     false    // 是否允许超收（true=允许, false=拒收）
#define SORTING_STARTED_MODE       "first_piece"  // 已开始分拣判定口径：first_piece=首件落格, manual=人工开工, both=两者

// ═══════════════════════════════════════════════════════════════════════════
// S7 分拣增强配置（T-S7-02/06/07）
// ═══════════════════════════════════════════════════════════════════════════
#define SORTING_EPC_DEDUP          true     // EPC 任务内防重（true=按orderCode+epc去重, false=关闭）
// ★ 2026-09-13 【已废弃】SORTING_GRID_CAP_POLICY：其「格口达计划上限」拦截曾因"多SKU共格时按 SKU
//   计划数误拒"于 2026-09-07 被整体移除（见 HttpServer.cpp「正常落格处理」处注释），此后全库只读不判。
//   现行机制改由下面两项承担——按「格口+SKU」的计划件数封顶，超出者改投物理异常口。
#define SORTING_GRID_CAP_POLICY    "reject"  // 【已废弃】格口达计划上限策略：reject/exception/allow
#define SORTING_OVERPLAN_POLICY    "exception" // 超计划处置：exception=改投异常口, block=拒发并标注, allow=仅标注
#define DEFAULT_EXCEPTION_GRID     ""        // 物理异常格口号（3位如"066"；空/"0"=未配置→自动降级为 block）
#define SORTING_ORDERQTY_VALIDATE  "strict"  // orderQty校验模式：strict=严格(不一致则拒绝), loose=宽松(仅告警)

// ═══════════════════════════════════════════════════════════════════════════
// ★ 2026-09-14 计划分配表（落格结构优化）配置
//
// 背景（客户口径）：同一 SKU 可以同时计划到「正常分拣（分类）格口」与
//   「发货格口」，**每个格口各有一份数量**；落格必须按各格口的数量分过去。
//   计划分配表把 H4 计划在波次开始时一次性编译成定长数组，运行期按额度
//   （计划 − 已落 − 在途认领）分配给每一件，从而：
//     · 多格口按各自数量分流（不再全投第一个格口）；
//     · 同时两件在线 / 人工多投时，"已落 + 在途 ≥ 计划"即改投异常口（66），
//       箱内实落不会再超过计划。
//
// 稳定性优先：任一开关都可在不改代码的前提下回退现场行为（改 XML 后重启生效）。
// ═══════════════════════════════════════════════════════════════════════════
#define ALLOC_ENABLED              true   // 计划分配表总开关（false=逐字回退改造前行为：恒取首个格口）
#define ALLOC_REQUIRE_PLAN_VALID   false  // true=开工预检发现「Σ每格口计划 ≠ H4 orderQty」即拒绝开工（默认只告警）
#define ALLOC_GAP_MOVE_ON_DISABLED true   // 计划格口满箱未重绑(禁用)时，把未完成计划件转给同 SKU 其它可用计划格口
#define ALLOC_GAP_MOVE_ON_LOCKED   true   // 计划格口物理锁格时同样转移（false=仅禁用格口转移）
#define ALLOC_MAX_INFLIGHT         2000   // 在途认领上限（超出→立即强制清扫 + 异常表留痕，防认领泄漏）
#define ALLOC_AUDIT_INTERVAL_MS    30000  // 不变量巡检 + 认领超时清扫周期(ms)
#define ALLOC_CLAIM_TIMEOUT_MS     30000  // 认领超时(ms)：下发后迟迟无落格反馈则释放额度（复用 PLC 在途超时口径）
#define ALLOC_PLAN_LOG_TAIL        30     // 「选格-按计划分配」成功日志前 N 条逐条输出
#define ALLOC_PLAN_LOG_STEP        500    // 之后每 N 条输出一条（异常/超计划/搬迁/兜底一律逐条保留）

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
#define PARSE_SKU_LOG_TAIL       30     // ★ 2026-09-09 超大波次日志节流：前N条逐条输出
#define PARSE_SKU_LOG_STEP       500    // ★ 2026-09-09 超大波次日志节流：此后每N条输出一条
#define PARSE_SKU_FULL_LOG_MAX   2000   // ★ 2026-09-09 item 数≤此值时逐条全量输出（小波次行为与原来完全一致）
#define WAVE_ITEM_PREP_REBUILD   2000   // ★ 2026-09-09 50000item落库：prepared 每N行重建（规避Qt绑值累积）
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
#define PLC_S7_IP               "192.168.100.10"  // S7 PLC IP 地址
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
#define DEFAULT_EXPECTED_BIND_COUNT 1      // 期望绑定数量默认值（波次下发时校验全部绑定用；★ 2026-09-11 现场口径=1）
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
// ★ 2026-09-08 开机自动接收任务
//   1 = 程序启动后自动执行一次「开始接收任务」（等价于人工点击一次按钮，
//       省去开机首点；仅本会话执行一次，之后的停止/再次开始照常由按钮控制）
//   0 = 保持原行为（启动后需人工点击才开始接收）
// ═══════════════════════════════════════════════════════════════════════════
#define AUTO_START_RECEIVE_ON_BOOT 1

// ═══════════════════════════════════════════════════════════════════════════
// RFID 缓存配置（RFID 主动推送模式，不再主动查询）
// ═══════════════════════════════════════════════════════════════════════════
#define RFID_CACHE_TTL_SEC         300      // EPC 本地缓存 TTL（秒，默认5分钟）
#define RFID_QUERY_URL             "http://172.31.10.201:9521/open-api/rfid/query"  // RFID SKU-EPC 绑定查询 URL（查询 EPC→barcode 映射，测试默认；生产地址在 http_server.xml 配置）
// ★ 2026-09-05：RFID 查询接口鉴权 —— Authorization 请求头的完整值（形如 "APP_KEYS <key>"）
//   生产环境由 http_server.xml 的 rfidAppkey 配置（整串直接使用，代码不拼凑）；
//   此默认值仅供本地 mock（空=不发送鉴权头）
#define RFID_APPKEY                "APP_KEYS e1235fda-029c-4270-913f-530342af2073"
// ★ 2026-09-04：RFID 推送服务端地址（WCS 作为 TCP 客户端主动连接 RFID 服务端接收 ASCII 帧）
//   帧格式（现场确认）：{流水号|小车号|epc}0D（0D 为帧尾字面字符）
//   EPC 未读到（无条码）时 epc 字段为 NOREAD：{流水号|小车号|NOREAD}0D
#define RFID_SERVER_IP             "192.168.100.125"
#define RFID_SERVER_PORT           2010
// ★ 2026-09-04：RFID 应用层心跳（RFID 服务端要求客户端每 2 秒发一次心跳保活）
//   心跳报文格式（现场确认）：按字面 ASCII 发送 "RFID{HEARTBEAT}0D"（末尾 0D 为字面字符，非回车）
#define RFID_HEARTBEAT_INTERVAL_MS  2000
#define RFID_HEARTBEAT_MSG          "RFID{HEARTBEAT}0D"
#define RFID_QUERY_TIMEOUT_MS      5000     // RFID 查询超时(ms)，默认5秒
#define SKU_QUERY_MAX_RETRY        2        // SKU 查询最大重试次数（超时/失败后最多重试2次）
#define SKU_QUERY_RETRY_INTERVAL_MS 3000    // SKU 查询重试间隔(ms)，默认3秒
#define NOT_READY_RETRY_MAX         3        // 未就绪(carNum未到)最大重试次数
#define NOT_READY_RETRY_INTERVAL_MS 5000     // 未就绪重试间隔(ms)，默认5秒
#define EPC_CACHE_ALERT_THRESHOLD   10000    // EpcCache 条目数告警阈值（健康日志观测：理论容量=推送速率×300sTTL，超阈值预警）
#define PLC_SEND_TIMEOUT_MS         1000     // RFID推送→PLC发送超时阈值(ms)，超过则入异常格口（现场实时性要求≤1s）
// ★ 2026-09-16 双读判定窗口(ms)：同一 EPC 再次推送时，只有"carNum 相同且在该窗口内"
//   才视为同一件的双读（保留首次 receivedAt，防止重复读把 1s 超时窗口起点一直推后）；
//   carNum 变化 或 超出该窗口 → 视为新件/新一轮，重新起算。
//   背景：格口满箱未重绑导致发送失败的件，其缓存条目会长时间留存，若沿用旧 receivedAt，
//   下一次推送（实测间隔 142s）会把间隔累加进 1s 窗口，误报 elapsed=142457ms 的"发送超时"。
#define DOUBLE_READ_WINDOW_MS       3000     // 双读判定窗口(ms)
// ★ 2026-09-16 「根因优先」保鲜期(ms)：某 EPC 因发送失败入异常终态后，在此时长内
//   若同一 EPC 再次推送并再次判定超时，异常记录沿用原根因（如"无可用格口"），
//   不再单独记一条"发送超时"把根因覆盖。取 300s（与 EpcCache TTL 同量级），
//   覆盖"格口满箱积压 → 同件再次上线"的现场节奏（实测间隔 142s）。
#define EPC_TERMINAL_EXCEPTION_KEEP_MS 300000

// ═══════════════════════════════════════════════════════════════════════════
// ★ EPC 码识别（RFID 推送报文中的 EPC 字段）
//
//   客户口径：『不管开头是不是 A101，只认识 A + 23 位数字组成的 EPC 码』
//     · 形态 = 大写字母 'A' + (长度-1) 位 ASCII 数字；长度 = rfidEpcTruncateLen（XML 可配）
//     · 默认 24 → 'A' + 23 位数字
//     · 在推送串中**定位**该形态（不是简单取前 N 位、也不假定它在串首）：
//         尾部附加数据（A10125010200876264980177 + 35303032）→ 正确取出前 24 位
//         头部杂串（0000A10125010200876264980177）           → 也能定位到真实 EPC
//     · 串中不含该形态时**不猜**：按原文继续处理并在 run.log 告警（原文另有留痕可查）
//     · < 2 = 不启用识别（整串原样使用，回退改造前行为）
//   单一实现源：WCS_httpServer/EpcCode.h（推送侧与绑定查询响应侧共用，改一处全生效）
// ═══════════════════════════════════════════════════════════════════════════
#define RFID_EPC_TRUNCATE_LEN       24       // EPC 码长度（字符数）；识别『A + (长度-1) 位数字』；默认 24

// ═══════════════════════════════════════════════════════════════════════════
// ★ RFID 原始推送报文留痕（需求：保留 RFID 推送的原始报文）
//
//   三层互为备份：① run.log（[原始报文] 整块 + 逐帧行）② 界面（实时面板悬停 / EPC 全信息弹窗）
//                ③ 数据库 rfid_raw 表（可按识别后 EPC / 识别前 EPC 双通道回查，长期追溯）
//   落库策略（不影响 RFID→PLC 主链路）：业务入口只入队（O(1) 不写盘），
//     满 RFID_RAW_FLUSH_MAX_ROWS 帧或每 RFID_RAW_FLUSH_INTERVAL_MS 落一次，退出前补写残留。
// ═══════════════════════════════════════════════════════════════════════════
#define RFID_RAW_FLUSH_MAX_ROWS     200      // 攒够 N 帧立即落库（单事务批量写入）
#define RFID_RAW_FLUSH_INTERVAL_MS  1000     // 未攒满时的落库周期(ms)
#define RFID_RAW_PENDING_MAX_ROWS   20000    // 待写队列上限（超限丢最旧并告警，防 DB 写不动时内存膨胀）
#define RFID_RAW_RETAIN_DAYS        7        // 原始报文保留天数（启动清一次 + 跨天空闲拍清一次）

// ═══════════════════════════════════════════════════════════════════════════
// ★ 2026-09-11 同波次「重扫重投」配置（拿起已落格的件重新上料 → 仍按原格口下发）
//   背景：此前 m_sentEpcs 以"本波次已发送过"永久去重，导致重投的件收不到格口指令；
//   改为「在途去重」：PLC 落格反馈到达即出在途，之后再被 RFID 读到允许按原格口重投。
// ★ 2026-09-13 调整（客户要求"已落格后的物件再次投放，WCS 依然要处理"）：
//   · 重投次数上限取消（默认 0 = 不限制），只要现场重新上料就重发原格口指令；
//   · 冷却默认置 0（不拦截）；如现场出现 RFID 同一次读头连读造成的重复下发，
//     把 rescanResendCooldownMs 改回 1000 即可恢复拦截，无需改代码。
// ═══════════════════════════════════════════════════════════════════════════
#define RESCAN_RESEND_ENABLED       true     // 重扫重投开关（false=回退"已发送过不再下发"的旧行为）
#define RESCAN_RESEND_COOLDOWN_MS   0        // 同一 EPC 两次下发最小间隔(ms)；0=不限制（防连读可设 1000）
#define RESCAN_RESEND_MAX_TIMES     0        // 同一 EPC 每波次最多重投次数；0=不限制（>0 时超限写异常表留痕）
#define PLC_INFLIGHT_TIMEOUT_MS     30000    // PLC 在途超时(ms)：迟迟未收到落格反馈时，超时后允许重投（防永久锁死）

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
#define SQL_PRAGMA_BUSY_TIMEOUT   "PRAGMA busy_timeout=3000"      // 只读连接等待写锁超时(ms)：WAL 下写不阻塞读，此值防极端锁竞争

// ──── 波次明细落库（异步）可靠性参数（2026-09-04 P0修复）────
#define WAVE_PERSIST_RETRY_MAX        3        // 波次明细落库最大重试次数（本地SQLite失败概率极低，重试后仍失败写异常表+保持CREATED）
#define WAVE_PERSIST_RETRY_INTERVAL_MS 1000    // 波次明细落库重试间隔(ms)
// ★ 2026-09-15 切出中的波次号保留时长：切出后内存波次为空，此窗口内到达的 H6 绑定
//   记为"刚切出的波次"（而不是空波次），避免绑定归属丢失导致该波次切回时取不回绑定
#define WAVE_SWITCH_OUT_KEEP_MS       120000   // 120s（足够覆盖 H4 与 H6 之间的下发间隔）

// ──── ★ 2026-09-17 界面页显隐开关（现场要求：可隐藏「实时面板」「计划分配表」两页）────
//   ★★ 只在**启动时读取一次**：改了 config/http_server.xml 必须**重启程序**才生效
//      （现场要求"不立刻生效"：运行中改 XML 不会让页面突然出现/消失）。
//   ★ 改法：关闭程序 → 编辑 <showLivePage> / <showPlanAllocPage> → 启动程序。
//     程序运行中改也会被下次启动读到，但**不会即时生效**；配置自动保存采用
//     "先比对外部改动再重载"策略（ConfigManager::save），不会覆盖手改内容。
//   ★ 置 false（默认）时：页面不加入标签页；实时面板连表都不创建 →
//     所有更新路径的 `if (!m_tblLive)` 守卫直接跳过 → 隐藏期间**零开销**。
#define UI_SHOW_LIVE_PAGE_DEFAULT        false  // 实时面板页（默认隐藏）
#define UI_SHOW_PLAN_ALLOC_PAGE_DEFAULT  false  // 计划分配表页（默认隐藏）

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
    "  create_time TEXT    NOT NULL DEFAULT ''," \
    "  boxcode     TEXT    NOT NULL DEFAULT ''" \
    ")"

// ★ 2026-09-09 需求6：旧库兼容——启动时检测缺 boxcode 列则补加（行进中换容器的记录容器号）
#define SQL_ALTER_SORTING_ADD_BOXCODE \
    "ALTER TABLE sorting_records ADD COLUMN boxcode TEXT NOT NULL DEFAULT ''"

// ──── 索引：加速常用查询 ────
// 按EPC编码查询索引 — 加速按EPC编码搜索历史分拣记录
#define SQL_CREATE_INDEX_BARCODE   "CREATE INDEX IF NOT EXISTS idx_barcode    ON sorting_records(barcode)"
// 按波次号查询索引 — 加速按波次号查询该波次下所有分拣记录
#define SQL_CREATE_INDEX_ORDER     "CREATE INDEX IF NOT EXISTS idx_order_code ON sorting_records(order_code)"
// 按分拣时间查询索引 — 加速按时间范围查询（如查询某天的分拣记录）
#define SQL_CREATE_INDEX_TIME      "CREATE INDEX IF NOT EXISTS idx_sort_time  ON sorting_records(sort_time)"

// ──── 公共查询字段列表（SELECT 子句复用）────
// 查询所有字段，用于各种 SELECT 语句拼接，避免重复书写字段列表
// ★ 2026-09-09 需求6：末尾新增 boxcode（容器号）——保持既有字段序号不变，解析处只需追加读取
#define SQL_SELECT_FIELDS  "SELECT id, order_code, barcode, sku, grid_num, car_num, first_car, last_car, grid_count, volu, sort_time, create_time, boxcode "

// ──── 插入记录：PLC 落格反馈时写入一条分拣记录 ────
// 使用参数化查询（?占位符），防止 SQL 注入，字段顺序与建表语句一致
#define SQL_INSERT_RECORD \
    "INSERT INTO sorting_records " \
    "(order_code, barcode, sku, grid_num, car_num, first_car, last_car, grid_count, volu, sort_time, create_time, boxcode) " \
    "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"

// ──── 查询：按不同条件检索分拣记录 ────
// 按EPC编码查询 — 输入EPC编码，返回该EPC编码的所有分拣历史（按时间倒序）→ 状态=已分拣
#define SQL_QUERY_BY_BARCODE       SQL_SELECT_FIELDS "FROM sorting_records WHERE barcode = ? ORDER BY id DESC LIMIT ?"
// 按时间范围查询 — 输入起始和结束时间，返回该时间段内的分拣记录（按时间倒序）→ 状态=已分拣
#define SQL_QUERY_BY_TIME          SQL_SELECT_FIELDS "FROM sorting_records WHERE sort_time >= ? AND sort_time <= ? ORDER BY id DESC LIMIT ?"
// 按波次号查询 — 输入波次号，返回该波次下的所有分拣记录（按时间倒序）→ 状态=已分拣
#define SQL_QUERY_BY_ORDER         SQL_SELECT_FIELDS "FROM sorting_records WHERE order_code = ? ORDER BY id DESC LIMIT ?"
// ★ 2026-09-09 需求2：按格口查询分拣明细（输入格口号，返回该格所有分拣记录）
// ★ 2026-09-10 查询兼容：输入支持三种写法，统一归一后再匹配
//   ① 内部 3 位 key "007"（PLC反馈/绑定/DB 存储格式）
//   ② 裸数字 "7"（旧数据/人工习惯）
//   ③ WMS 格口编码 "22007"（22+3位，现场 WMS 侧写法）
//   绑定顺序：?1=归一内部key("007")  ?2=用户原始输入  ?3=归一内部key(整数比较用)  ?4=limit
#define SQL_QUERY_BY_GRID \
    SQL_SELECT_FIELDS "FROM sorting_records " \
    "WHERE grid_num = ? "                                    /* 归一内部 key 精确匹配 "007" */ \
    "   OR grid_num = ? "                                    /* 用户原始输入（兼容历史存 WMS 编码的数据） */ \
    "   OR CAST(grid_num AS INTEGER) = CAST(? AS INTEGER) "  /* 整数比较：兼容 "7" / "007" 混存 */ \
    "ORDER BY id DESC LIMIT ?"
// ★ 2026-09-10 需求1：按 SKU 查询落格明细（该 SKU 下所有 EPC 及其实际落格号）
//   每条落格记录 = 1 个 EPC，grid_num 即该 EPC 的实际落格号
#define SQL_QUERY_BY_SKU \
    SQL_SELECT_FIELDS "FROM sorting_records " \
    "WHERE TRIM(sku) = TRIM(?) COLLATE NOCASE ORDER BY id DESC LIMIT ?"
// ★ 2026-09-11 重扫重投：查某波次某 EPC 的**首条**落格号（重扫落格号一致性告警用）
#define SQL_SELECT_FIRST_GRID_BY_EPC \
    "SELECT grid_num FROM sorting_records WHERE order_code = ? AND barcode = ? ORDER BY id ASC LIMIT 1"
// ★ 2026-09-13 需求：按容器号查询该容器下的所有 EPC 物件明细
//   数据源 sorting_records.boxcode（落格时按当时绑定固化的容器号）
//   说明：容器号大小写/空格做归一（TRIM + COLLATE NOCASE），兼容人工输入的 h-t0131
#define SQL_QUERY_BY_BOXCODE \
    SQL_SELECT_FIELDS "FROM sorting_records " \
    "WHERE TRIM(boxcode) = TRIM(?) COLLATE NOCASE ORDER BY id ASC LIMIT ?"
// ★ 2026-09-13 需求（按容器号查询配套）：批量查一组 EPC 各自出现过的其他容器号
//   用途：只查本容器时，"同一 EPC 是否被分到过别的容器"无法从单表结果看出，需反查
//   绑定：?1=EPC  ?2=本容器（排除自身）  ?3=limit
#define SQL_QUERY_BOXCODES_BY_EPC \
    "SELECT DISTINCT TRIM(boxcode) FROM sorting_records " \
    "WHERE barcode = ? AND TRIM(boxcode) <> '' " \
    "  AND TRIM(boxcode) <> TRIM(?) COLLATE NOCASE " \
    "ORDER BY id ASC LIMIT ?"
// ═════════════════════════════════════════════════════════════════════════════
// ★ 2026-09-16 现场需求③「分拣记录查询：结合日期条件筛选」——带日期区间的查询变体
//
//   口径（客户确认）：
//     · 区间**必须生效**（界面上没有"全部/不筛日期"选项），默认起=今天−7天、止=今天；
//     · 命中列 = sorting_records.sort_time（落格时间），字符串区间比较
//       `>= 'yyyy-MM-dd 00:00:00' AND <= 'yyyy-MM-dd 23:59:59'`，与 SQL_QUERY_BY_TIME 完全一致，
//       走既有 idx_sort_time 索引；
//     · "待分拣"（return_wave_item 计划行）**没有落格时间，不受日期筛选影响**——
//       因此 SQL_QUERY_PENDING_BY_BARCODE / SQL_QUERY_ALL_PENDING 刻意**不**加区间；
//     · 派生量（格口汇总计数、SKU 去重 EPC、容器跨容器反查）一律基于**过滤后**的结果。
//
//   字段序与 SQL_SELECT_FIELDS 保持完全一致（解析处字段下标不变），只追加 WHERE 条件与参数。
//   绑定顺序（在各自原宏的参数之后追加两枚）：... , fromDate, toDate, ...
// ═════════════════════════════════════════════════════════════════════════════
// 按格口 + 日期区间：?1=归一内部key  ?2=用户原始输入  ?3=归一内部key(整数比较)  ?4=from  ?5=to  ?6=limit
#define SQL_QUERY_BY_GRID_RANGE \
    SQL_SELECT_FIELDS "FROM sorting_records " \
    "WHERE (grid_num = ? " \
    "   OR grid_num = ? " \
    "   OR CAST(grid_num AS INTEGER) = CAST(? AS INTEGER)) " \
    "  AND sort_time >= ? AND sort_time <= ? " \
    "ORDER BY id DESC LIMIT ?"
// 按 SKU + 日期区间：?1=sku  ?2=from  ?3=to  ?4=limit
#define SQL_QUERY_BY_SKU_RANGE \
    SQL_SELECT_FIELDS "FROM sorting_records " \
    "WHERE TRIM(sku) = TRIM(?) COLLATE NOCASE " \
    "  AND sort_time >= ? AND sort_time <= ? " \
    "ORDER BY id DESC LIMIT ?"
// 按容器号 + 日期区间：?1=boxcode  ?2=from  ?3=to  ?4=limit
#define SQL_QUERY_BY_BOXCODE_RANGE \
    SQL_SELECT_FIELDS "FROM sorting_records " \
    "WHERE TRIM(boxcode) = TRIM(?) COLLATE NOCASE " \
    "  AND sort_time >= ? AND sort_time <= ? " \
    "ORDER BY id ASC LIMIT ?"
// 按容器号配套反查（同区间）：判断"同一 EPC 是否被分到过别的容器"
//   绑定：?1=EPC  ?2=本容器（排除自身）  ?3=from  ?4=to  ?5=limit
#define SQL_QUERY_BOXCODES_BY_EPC_RANGE \
    "SELECT DISTINCT TRIM(boxcode) FROM sorting_records " \
    "WHERE barcode = ? AND TRIM(boxcode) <> '' " \
    "  AND TRIM(boxcode) <> TRIM(?) COLLATE NOCASE " \
    "  AND sort_time >= ? AND sort_time <= ? " \
    "ORDER BY id ASC LIMIT ?"
// ★ 2026-09-09 需求2：全格口汇总（每格一行：格口号/分拣件数/SKU数/最近容器号/最近分拣时间）
// ★ 2026-09-10 归一：按格口整数归组（"7"/"007" 不再重复成两行），显示统一为 3 位 key
#define SQL_QUERY_GRID_SUMMARY \
    "SELECT " \
    "  CASE WHEN CAST(s.grid_num AS INTEGER) > 0 " \
    "       THEN printf('%03d', CAST(s.grid_num AS INTEGER)) ELSE s.grid_num END AS grid_key, " \
    "  COUNT(*) AS cnt, COUNT(DISTINCT s.sku) AS sku_cnt, " \
    "  (SELECT s2.boxcode FROM sorting_records s2 " \
    "    WHERE s2.grid_num = s.grid_num " \
    "       OR CAST(s2.grid_num AS INTEGER) = CAST(s.grid_num AS INTEGER) " \
    "    ORDER BY s2.id DESC LIMIT 1) AS box, " \
    "  MAX(s.sort_time) AS last_t " \
    "FROM sorting_records s " \
    "GROUP BY CASE WHEN CAST(s.grid_num AS INTEGER) > 0 " \
    "              THEN printf('%03d', CAST(s.grid_num AS INTEGER)) ELSE s.grid_num END " \
    "ORDER BY 1"
// ★ 2026-09-16 需求③：全格口汇总 + 日期区间（计数/SKU数/最近容器号/最近分拣时间**都只反映区间内**）
//   绑定：?1=from  ?2=to
#define SQL_QUERY_GRID_SUMMARY_RANGE \
    "SELECT " \
    "  CASE WHEN CAST(s.grid_num AS INTEGER) > 0 " \
    "       THEN printf('%03d', CAST(s.grid_num AS INTEGER)) ELSE s.grid_num END AS grid_key, " \
    "  COUNT(*) AS cnt, COUNT(DISTINCT s.sku) AS sku_cnt, " \
    "  (SELECT s2.boxcode FROM sorting_records s2 " \
    "    WHERE (s2.grid_num = s.grid_num " \
    "       OR CAST(s2.grid_num AS INTEGER) = CAST(s.grid_num AS INTEGER)) " \
    "      AND s2.sort_time >= ? AND s2.sort_time <= ? " \
    "    ORDER BY s2.id DESC LIMIT 1) AS box, " \
    "  MAX(s.sort_time) AS last_t " \
    "FROM sorting_records s " \
    "WHERE s.sort_time >= ? AND s.sort_time <= ? " \
    "GROUP BY CASE WHEN CAST(s.grid_num AS INTEGER) > 0 " \
    "              THEN printf('%03d', CAST(s.grid_num AS INTEGER)) ELSE s.grid_num END " \
    "ORDER BY 1"
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

// ★ 2026-09-06 修复：return_wave_item 旧库缺列迁移（波次明细落库失败 no such column）
//   老版本建的 return_wave_item 表可能缺后来新增的列（grid_type/plan_qty/sorted_qty/volu/obx_code），
//   CREATE TABLE IF NOT EXISTS 不会给已存在的表补列 → INSERT 稳定失败。
//   重复执行会报 duplicate column，try-exec 忽略即可（与上方迁移同模式）
#define SQL_ALTER_ADD_WI_GRID_TYPE  "ALTER TABLE return_wave_item ADD COLUMN grid_type TEXT NOT NULL DEFAULT '0'"
#define SQL_ALTER_ADD_WI_PLAN_QTY   "ALTER TABLE return_wave_item ADD COLUMN plan_qty INTEGER NOT NULL DEFAULT 0"
#define SQL_ALTER_ADD_WI_SORTED_QTY "ALTER TABLE return_wave_item ADD COLUMN sorted_qty INTEGER NOT NULL DEFAULT 0"
#define SQL_ALTER_ADD_WI_VOLU       "ALTER TABLE return_wave_item ADD COLUMN volu TEXT NOT NULL DEFAULT ''"
#define SQL_ALTER_ADD_WI_OBX_CODE   "ALTER TABLE return_wave_item ADD COLUMN obx_code TEXT NOT NULL DEFAULT ''"
// ★ 2026-09-08 保险：outbox_fullbox 旧库缺 grid 列（满箱回传对应格口号，供"失败格口下拉"直接读取，
//   免去运行时解析 payload JSON）；重复执行报 duplicate column，忽略即可
#define SQL_ALTER_ADD_OB_GRID       "ALTER TABLE outbox_fullbox ADD COLUMN grid TEXT NOT NULL DEFAULT ''"
// ★ 2026-09-06 保险：return_wave 波次头旧库缺列迁移
#define SQL_ALTER_ADD_RW_ORDER_QTY  "ALTER TABLE return_wave ADD COLUMN order_qty INTEGER NOT NULL DEFAULT 0"
#define SQL_ALTER_ADD_RW_STATUS     "ALTER TABLE return_wave ADD COLUMN status INTEGER NOT NULL DEFAULT 0"
#define SQL_ALTER_ADD_RW_CREATED_AT "ALTER TABLE return_wave ADD COLUMN created_at TEXT NOT NULL DEFAULT ''"
#define SQL_ALTER_ADD_RW_UPDATED_AT "ALTER TABLE return_wave ADD COLUMN updated_at TEXT NOT NULL DEFAULT ''"

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
    "  grid        TEXT    NOT NULL DEFAULT ''," \
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
// 查询面板展示的波次列表：未完成波次（排除已取消=6和已完成=8）
//   + 已完结/已取消但仍有未成功回传报文（H7/H8 status<>success）的波次（供手动补发）
#define SQL_SELECT_ALL_UNFINISHED_WAVES \
    "SELECT w.order_code, w.order_qty, w.status, w.created_at, w.updated_at FROM return_wave w " \
    "WHERE w.status NOT IN (6, 8) " \
    "   OR EXISTS (SELECT 1 FROM outbox_end e WHERE e.order_code = w.order_code AND e.status <> 'success') " \
    "   OR EXISTS (SELECT 1 FROM outbox_fullbox f WHERE f.order_code = w.order_code AND f.status <> 'success') " \
    "ORDER BY w.updated_at DESC"

// ★ 2026-09-06 UI「波次数据记录」：全部已传输波次（含已完成/已取消）+ 已分拣/异常计数子查询
//   （一次查询返回全量，避免 UI 逐行请求 DB）
#define SQL_SELECT_ALL_WAVES \
    "SELECT w.order_code, w.order_qty, w.status, w.created_at, w.updated_at, " \
    "  (SELECT COUNT(DISTINCT s.barcode) FROM sorting_records s WHERE s.order_code = w.order_code) AS sorted_count, " \
    "  (SELECT COUNT(DISTINCT e.epc) FROM exception_record e WHERE e.order_code = w.order_code) AS exc_count " \
    "FROM return_wave w ORDER BY w.updated_at DESC"

// ──── 波次恢复查询（上一波次任务恢复用）────
// 查询某波次全部已分拣 EPC（barcode=EPC；sorting_records 有记录即已分拣）
#define SQL_SELECT_SORTED_EPCS_BY_ORDER \
    "SELECT DISTINCT barcode FROM sorting_records WHERE order_code = ? AND barcode <> ''"
// 查询某波次全部异常 EPC
// ★ 2026-09-13 异常及时清理口径：该 EPC 若已成功落格（sorting_records 有记录），
//   说明它已从异常口闭环出来，恢复时不得再算回"异常"（否则重启后异常数又变多）
#define SQL_SELECT_EXCEPTION_EPCS_BY_ORDER \
    "SELECT DISTINCT e.epc FROM exception_record e " \
    "WHERE e.order_code = ? AND e.epc <> '' " \
    "  AND NOT EXISTS (SELECT 1 FROM sorting_records s " \
    "                  WHERE s.order_code = e.order_code AND s.barcode = e.epc)"
// ★ 2026-09-15 查询某波次全部已落格明细（按落格先后正序）——切回波次时补齐"已落格进度"的唯一持久权威
//   取数：sorting_records（PLC 落格反馈写入），带落格当时的容器号/SKU/库位/时间
//   返回**明细行本身**，不做去重：去重口径因目标结构而异，由调用方按各自键处理——
//     · H7 满箱明细 / 落格去重集合 → key = (格口, EPC)（换箱重投时旧箱件不再进明细）
//     · 计划额度 / 封顶计数          → key = (格口, SKU, EPC)（跨容器累计，换箱不重置额度）
//   ★ 历史库中的重复行（09-14 之前写入）由调用方按上述键合并，不会重复计数。
#define SQL_SELECT_LANDINGS_BY_ORDER \
    "SELECT s.barcode, s.sku, s.grid_num, s.boxcode, s.volu, s.sort_time FROM sorting_records s " \
    "WHERE s.order_code = ? AND s.barcode <> '' " \
    "ORDER BY s.rowid ASC LIMIT ?"
// 查询某波次是否存在成功满箱回传（H7）
#define SQL_SELECT_HAS_SUCCESS_FULLBOX \
    "SELECT COUNT(*) FROM outbox_fullbox WHERE order_code = ? AND status = 'success'"
// H4 原始报文单独落库（数据量大，独立表）
#define SQL_CREATE_TABLE_WAVE_RAW \
    "CREATE TABLE IF NOT EXISTS wave_raw (" \
    "  order_code  TEXT PRIMARY KEY," \
    "  raw_body    TEXT    NOT NULL DEFAULT ''," \
    "  received_at TEXT    NOT NULL DEFAULT ''" \
    ")"
#define SQL_INSERT_WAVE_RAW \
    "INSERT OR REPLACE INTO wave_raw (order_code, raw_body, received_at) VALUES (?, ?, ?)"
#define SQL_SELECT_WAVE_RAW \
    "SELECT raw_body FROM wave_raw WHERE order_code = ?"

// ──── 每日峰值效率表（2026-09-07：波次面板峰值效率，每天最终最大值落库）────
// stat_date       — 统计日期 yyyy-MM-dd（主键）
// peak_per_minute — 当日 1 分钟窗口 RFID 推送件数峰值（原始口径）
// peak_per_hour   — 折算每小时 = peak_per_minute * 60（展示口径）
// updated_at      — 最后更新时间
#define SQL_CREATE_TABLE_DAILY_PEAK \
    "CREATE TABLE IF NOT EXISTS daily_peak (" \
    "  stat_date        TEXT PRIMARY KEY," \
    "  peak_per_minute  INTEGER NOT NULL DEFAULT 0," \
    "  peak_per_hour    INTEGER NOT NULL DEFAULT 0," \
    "  updated_at       TEXT    NOT NULL DEFAULT ''" \
    ")"
#define SQL_INSERT_DAILY_PEAK \
    "INSERT OR REPLACE INTO daily_peak (stat_date, peak_per_minute, peak_per_hour, updated_at) VALUES (?, ?, ?, ?)"
#define SQL_SELECT_DAILY_PEAK \
    "SELECT peak_per_minute FROM daily_peak WHERE stat_date = ?"

// ──── ★ RFID 原始推送报文留痕表（rfid_raw）────
// 逐帧结构化留痕，可长期追溯、可按 EPC 反查、可导出核对
// time       — 落库时间 yyyy-MM-dd HH:mm:ss
// epc        — **识别归一后**用于分拣的 EPC（空 = 该帧为 NOREAD 未读到标签）
// epc_raw    — **识别前**的 EPC 原文（未归一/未识别时与 epc 相同）
// car_num    — 帧内解析出的小车号（流水号中的数字，如 SN0098 → 98）
// seq        — 帧内流水号原文（如 SN0098）
// dev_code   — 帧内设备编码（如 01；两段帧天然为空）
// raw_frame  — RFID 原样推送的整帧报文（含帧头 {、帧尾 } 与协议字面帧尾 0D）
// bytes      — 整帧字节数
// noread     — 1 = NOREAD 帧（仅留痕，不进入分拣）
// 注：所有文本列 NOT NULL + 默认空串——Qt 会把 null QString 绑定为 SQL NULL，
//     不做空串归一就会 "NOT NULL constraint failed" 而静默丢帧
#define SQL_CREATE_TABLE_RFID_RAW \
    "CREATE TABLE IF NOT EXISTS rfid_raw (" \
    "  id        INTEGER PRIMARY KEY AUTOINCREMENT," \
    "  time      TEXT    NOT NULL DEFAULT ''," \
    "  epc       TEXT    NOT NULL DEFAULT ''," \
    "  epc_raw   TEXT    NOT NULL DEFAULT ''," \
    "  car_num   TEXT    NOT NULL DEFAULT ''," \
    "  seq       TEXT    NOT NULL DEFAULT ''," \
    "  dev_code  TEXT    NOT NULL DEFAULT ''," \
    "  raw_frame TEXT    NOT NULL DEFAULT ''," \
    "  bytes     INTEGER NOT NULL DEFAULT 0," \
    "  noread    INTEGER NOT NULL DEFAULT 0" \
    ")"
// 索引：按识别后 EPC / 识别前 EPC 双通道回查 + 按时间倒序取最近帧 + 超期清理
#define SQL_CREATE_INDEX_RFID_RAW_EPC \
    "CREATE INDEX IF NOT EXISTS idx_rfid_raw_epc ON rfid_raw(epc)"
#define SQL_CREATE_INDEX_RFID_RAW_EPC_RAW \
    "CREATE INDEX IF NOT EXISTS idx_rfid_raw_epc_raw ON rfid_raw(epc_raw)"
#define SQL_CREATE_INDEX_RFID_RAW_TIME \
    "CREATE INDEX IF NOT EXISTS idx_rfid_raw_time ON rfid_raw(time)"
// 插入一帧原始报文（批量写入用，事务内逐帧 exec）
#define SQL_INSERT_RFID_RAW \
    "INSERT INTO rfid_raw (time, epc, epc_raw, car_num, seq, dev_code, raw_frame, bytes, noread) " \
    "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)"
// 按 EPC 回查（识别后与识别前两条通道都命中，便于核对"丢弃了什么"），按 id 倒序 = 最近在前
#define SQL_QUERY_RFID_RAW_BY_EPC \
    "SELECT id, time, epc, epc_raw, car_num, seq, dev_code, raw_frame, bytes, noread " \
    "FROM rfid_raw WHERE epc = ? OR epc_raw = ? ORDER BY id DESC LIMIT ?"
// 超期清理（只删除保留期外的行）
#define SQL_CLEANUP_RFID_RAW \
    "DELETE FROM rfid_raw WHERE time <> '' AND time < ?"
// 表内总行数（页脚展示用）
#define SQL_COUNT_RFID_RAW \
    "SELECT COUNT(*) FROM rfid_raw"

// ★ 2026-09-15 波次列表批量统计（3 条 GROUP BY 覆盖全部波次，替代"每波次 3 次查询"）
//   口径与原逐波次统计严格一致：成功='success'、失败='failed'、其余(pending/cancelled/未知)='待发'
#define SQL_SELECT_FULLBOX_STATUS_COUNT_ALL \
    "SELECT order_code, " \
    "  SUM(CASE WHEN status = 'success' THEN 1 ELSE 0 END) AS c_success, " \
    "  SUM(CASE WHEN status = 'failed'  THEN 0 ELSE (CASE WHEN status = 'success' THEN 0 ELSE 1 END) END) AS c_pending, " \
    "  SUM(CASE WHEN status = 'failed'  THEN 1 ELSE 0 END) AS c_failed " \
    "FROM outbox_fullbox WHERE order_code <> '' GROUP BY order_code"
#define SQL_SELECT_END_STATUS_COUNT_ALL \
    "SELECT order_code, " \
    "  SUM(CASE WHEN status = 'success' THEN 1 ELSE 0 END) AS c_success, " \
    "  SUM(CASE WHEN status = 'failed'  THEN 0 ELSE (CASE WHEN status = 'success' THEN 0 ELSE 1 END) END) AS c_pending, " \
    "  SUM(CASE WHEN status = 'failed'  THEN 1 ELSE 0 END) AS c_failed " \
    "FROM outbox_end WHERE order_code <> '' GROUP BY order_code"
// 未闭环异常去重 EPC 数（与 getOpenExceptions 口径一致：仅 PLC 判定失败两类 + handled=0 + 按 EPC 去重）
#define SQL_SELECT_PENDING_EXC_COUNT_ALL \
    "SELECT order_code, COUNT(DISTINCT epc) FROM exception_record " \
    "WHERE order_code <> '' AND handled = 0 AND epc <> '' " \
    "  AND type IN ('plc_no_grid', 'plc_info_incomplete') " \
    "GROUP BY order_code"

// ──── 波次明细操作 ────
// 插入明细行
#define SQL_INSERT_WAVE_ITEM \
    "INSERT INTO return_wave_item (order_code, inco, grid_num, grid_type, plan_qty, sorted_qty, volu, obx_code) VALUES (?, ?, ?, ?, ?, 0, ?, ?)"
// 按波次号查询所有明细
#define SQL_SELECT_WAVE_ITEMS \
    "SELECT id, order_code, inco, grid_num, grid_type, plan_qty, sorted_qty, volu, obx_code FROM return_wave_item WHERE order_code = ?"
// ★ 按 SKU 查询格口分配 — "已分拣数量"列实时 COUNT sorting_records（每落格一个EPC=一条记录，即EPC件数）
//   不依赖运行时维护 sorted_qty 冗余列，历史数据也立即正确
#define SQL_QUERY_SKU_GRID_MAPPING \
    "SELECT i.id, i.order_code, i.inco, i.grid_num, i.grid_type, i.plan_qty, " \
    "(SELECT COUNT(*) FROM sorting_records s " \
    " WHERE s.order_code = i.order_code AND s.sku = i.inco " \
    " AND CAST(s.grid_num AS INTEGER) = CAST(i.grid_num AS INTEGER)) AS sorted_qty, " \
    "i.volu, i.obx_code " \
    "FROM return_wave_item i WHERE i.inco = ? ORDER BY i.order_code DESC, i.grid_num ASC"
#define SQL_QUERY_SKU_GRID_MAPPING_BY_ORDER \
    "SELECT i.id, i.order_code, i.inco, i.grid_num, i.grid_type, i.plan_qty, " \
    "(SELECT COUNT(*) FROM sorting_records s " \
    " WHERE s.order_code = i.order_code AND s.sku = i.inco " \
    " AND CAST(s.grid_num AS INTEGER) = CAST(i.grid_num AS INTEGER)) AS sorted_qty, " \
    "i.volu, i.obx_code " \
    "FROM return_wave_item i WHERE i.inco = ? AND i.order_code = ? ORDER BY i.grid_num ASC"
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
// 查询全部活跃绑定（程序重启后加载内存/UI 用）
#define SQL_SELECT_ALL_ACTIVE_BINDS \
    "SELECT grid_num, boxcode, order_code, bind_time FROM grid_box_bind WHERE active = 1 ORDER BY grid_num ASC"
// ★ 2026-09-06 按波次查询绑定快照：每格取该波次最新一条记录（含已归档）
//   —— 追溯口径：回答"该波次当时用过哪些容器"，**不**按 active 过滤
//   ★ 2026-09-17：补 boxcode <> '' —— 空箱号不是有效绑定（当前无墓碑行，纯防御），
//     并且该查询已升级为**切回波次恢复绑定的主口径**（见 SQL_SELECT_LAST_BINDS_BY_ORDER 注释）
#define SQL_SELECT_BINDS_BY_ORDER \
    "SELECT g1.grid_num, g1.boxcode FROM grid_box_bind g1 " \
    "WHERE g1.order_code = ? AND g1.boxcode <> '' AND g1.rowid = " \
    "  (SELECT MAX(g2.rowid) FROM grid_box_bind g2 WHERE g2.grid_num = g1.grid_num AND g2.order_code = ?) " \
    "ORDER BY CAST(g1.grid_num AS INTEGER) ASC"
// ★ 2026-09-15 按波次查询该波次的容器绑定（"本波次最后一条仍生效"的确认口径）
//   口径：**每格在全表中取最后一条记录**，且该记录属于本波次、容器号非空。
//   —— 回答"本波次结束后该格口是否仍绑着本波次的箱子"。
//   ★★ 2026-09-17 口径修正（现场取证，见 docs/格口绑定波次归属_根因与修复_20260917.md）：
//     本查询**不再是切回恢复绑定的主口径**。原因：只要该格口之后被任何一个更晚的波次绑过，
//     本查询就返回 0 行（实测真实库 14 个历史波次里 13 个返回 0 行）；而它一旦"部分命中"
//     （一部分格口最后一条属本波次、另一部分属更晚波次），调用方若据此整体替换内存绑定，
//     就会丢掉其余格口 —— 现场"切回后面板被清空"的来源之一。
//     主口径已改为 SQL_SELECT_LAST_BINDS_BY_ORDER（每格取**本波次内**最后一条），
//     本查询只用于打"是否已被更晚波次覆盖"的日志标签 / 命中数统计。
//   ★ 刻意**不按 active 过滤**：关闭软件/启动新任务会把绑定归档（active=0 + unbind_time 留痕），
//     若按 active 过滤，归档后就取不回绑定，"清空绑定关系"与"切回恢复绑定"将无法并存。
//   ★ 也不看 unbind_time：归档时间只作留痕，绑定归属由 order_code + 每格最后一条决定。
//   有界性：每格全表只可能命中一行 → 结果规模 ≤ 格口数(66)。旧行由 DBA 侧按需清理。
#define SQL_SELECT_BINDS_BY_ORDER_ACTIVE \
    "SELECT g1.grid_num, g1.boxcode FROM grid_box_bind g1 " \
    "WHERE g1.order_code = ? AND g1.boxcode <> '' AND g1.rowid = " \
    "  (SELECT MAX(g2.rowid) FROM grid_box_bind g2 WHERE g2.grid_num = g1.grid_num) " \
    "ORDER BY CAST(g1.grid_num AS INTEGER) ASC"

// ★ 2026-09-17 按波次查询"该波次自己绑过哪些格口、每格最后绑的是哪个容器"（切回恢复绑定的主口径）
//   口径：每格取**本波次内**最后一条记录（boxcode 非空，含已被更晚波次覆盖的格口）。
//     · 本波次绑过的格口 → 命中（含换箱后的最新箱）；
//     · 本波次从未绑过的格口 → 不返回（保持未绑定，不臆造）；
//     · 与 SQL_SELECT_BINDS_BY_ORDER_ACTIVE 的关系：后者 ⊂ 本查询（前者要求"全表最后一条属本波次"，
//       那它必然也是"本波次内最后一条"）→ 先取本查询**绝不会漏格口**，是"全有或全无"缺陷的根修。
//   有界性：结果规模 ≤ 格口数(66)。
#define SQL_SELECT_LAST_BINDS_BY_ORDER \
    "SELECT g1.id, g1.grid_num, g1.boxcode, g1.order_code, g1.active, g1.bind_time, g1.unbind_time " \
    "FROM grid_box_bind g1 " \
    "WHERE g1.order_code = ? AND g1.boxcode <> '' AND g1.rowid = " \
    "  (SELECT MAX(g2.rowid) FROM grid_box_bind g2 WHERE g2.grid_num = g1.grid_num AND g2.order_code = ?) " \
    "ORDER BY CAST(g1.grid_num AS INTEGER) ASC"

// ★ 2026-09-17 归属补齐/纠偏（唯一新增的写入路径，也是唯一允许改写 order_code 的地方）
//   时机：**H4 到达、波次落库完成时**（HttpServer::onWavePersistenceFinished → attributePendingBindsToWave）
//   目的：把"当时无法确定归属"的绑定行补上真正的波次号。现场两类真实故障：
//     ① H6 早于 H4（WMS 先发绑定再发波次）→ 内存无波次 → 归属写成空串 → 切回该波次取不到绑定；
//     ② 切出/新任务后 120s "切出窗口"内到达的 H6 被记到**刚切出的上一个波次**（现场 2026-09-15
//        23:53 的 66 行 H6 实为 456456 的绑定，却挂到了 6565656 名下）→ 新波次零绑定、旧波次被污染。
//   约束（与"历史不得粘贴进其它波次"铁律共存）：
//     · 只改 order_code，不动 active/bind_time/unbind_time，**不新增/不删除行**（行数只可能不变）；
//     · 只处理 active=1（此刻物理生效）且 bind_time >= 会话启动时刻的行（历史行永不改判）；
//     · 只处理"空归属"或"切出窗口内产生、属于本次投递的误归属"两类；
//     · 该格口若已存在本波次自己的记录，则**不改判**（避免把已在用记录改错）。
#define SQL_UPDATE_BINDS_ATTRIBUTE_TO_WAVE \
    "UPDATE grid_box_bind SET order_code = ? " \
    "WHERE active = 1 " \
    "  AND bind_time >= ? " \
    "  AND (order_code = '' OR (order_code = ? AND order_code <> '' AND bind_time >= ?)) " \
    "  AND grid_num NOT IN (SELECT grid_num FROM grid_box_bind WHERE order_code = ?)"

// ★ 2026-09-17 归属补齐候选行（与上面 UPDATE 完全同条件，用于改判前打明细日志）
#define SQL_SELECT_BINDS_TO_ATTRIBUTE \
    "SELECT id, grid_num, boxcode, order_code, bind_time FROM grid_box_bind " \
    "WHERE active = 1 " \
    "  AND bind_time >= ? " \
    "  AND (order_code = '' OR (order_code = ? AND order_code <> '' AND bind_time >= ?)) " \
    "  AND grid_num NOT IN (SELECT grid_num FROM grid_box_bind WHERE order_code = ?) " \
    "ORDER BY CAST(grid_num AS INTEGER) ASC"

// ★ 2026-09-17 按波次统计"绑过几个格口"（UI「波次数据记录」列表新增「格口绑定」列的批量数据源）
//   —— 一次 GROUP BY 覆盖全部波次，避免列表刷新退化成逐波次查询（对齐既有 3 条批量统计的做法）。
//   口径：该波次名下出现过的不同格口数（含换箱的多次行；boxcode 非空）。0 → 该波次无绑定记录。
#define SQL_SELECT_BIND_COUNTS_BY_ORDER \
    "SELECT order_code, COUNT(DISTINCT grid_num) FROM grid_box_bind " \
    "WHERE order_code <> '' AND boxcode <> '' GROUP BY order_code"

// ★ 2026-09-17 人工修复历史误归属用（仅供 DBA/人工核查脚本按需执行，产品代码不调用）
#define SQL_UPDATE_BIND_ORDER_BY_ID \
    "UPDATE grid_box_bind SET order_code = ? WHERE id = ?"

// ★ 2026-09-17 绑定按波次过滤的索引（切回取数/归属补齐/列表统计三处都按 order_code 过滤）
#define SQL_CREATE_INDEX_BIND_ORDER \
    "CREATE INDEX IF NOT EXISTS idx_bind_order ON grid_box_bind(order_code)"
// ★ 2026-09-15 按波次查询绑定历史明细（含绑定/解绑时间）——只读回溯用
#define SQL_SELECT_BIND_HISTORY_BY_ORDER \
    "SELECT id, grid_num, boxcode, order_code, active, bind_time, unbind_time FROM grid_box_bind " \
    "WHERE order_code = ? ORDER BY CAST(grid_num AS INTEGER) ASC, id ASC"
// 归档全部活跃绑定（波次完结/取消时清空全部格口绑定）
#define SQL_ARCHIVE_ALL_BINDS \
    "UPDATE grid_box_bind SET active = 0, unbind_time = ? WHERE active = 1"

// ★ 2026-09-17 「新任务」清空格口绑定：只归档**已归属**的活跃行，保留"待补齐"行（order_code=''）
//   为什么不能直接用 SQL_ARCHIVE_ALL_BINDS（现场要求"新任务时清空绑定"时的关键取舍）：
//     H6 早于 H4 到达时，绑定行以 order_code='' 保留，等 H4 到达时由
//     attributeBindsToWave() 补齐归属。若在「新任务」时把它一并归档（active=0），
//     后续 H4 到达就**补齐不到**这些行 → 该波次又是 0 绑定 → 切回无绑定可恢复（老毛病回归）。
//   因此：已归属行（有波次号）照常归档留痕（行保留、归属不变 → 切回可恢复），
//        未归属行保持 active，等 H4 到达补齐后再参与正常流程。
//   （启动时仍用 SQL_ARCHIVE_ALL_BINDS 全量归档：那些行来自上一次会话，
//     归属补齐的"本会话"时间护栏本来就不会认领它们。）
#define SQL_ARCHIVE_ATTRIBUTED_BINDS \
    "UPDATE grid_box_bind SET active = 0, unbind_time = ? WHERE active = 1 AND order_code <> ''"

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
    "INSERT INTO outbox_fullbox (msg_id, order_code, boxcode, grid, payload, status, retry_count, next_retry, created_at) VALUES (?, ?, ?, ?, ?, 'pending', 0, ?, ?)"
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
    "SELECT msg_id, order_code, boxcode, grid, payload, retry_count FROM outbox_fullbox WHERE msg_id = ?"
// 按 msgId 查询单条 完结回传（H8 完结出站消息（S6 人工重发用））
#define SQL_SELECT_OUTBOX_END_BY_MSGID \
    "SELECT msg_id, order_code, payload, retry_count FROM outbox_end WHERE msg_id = ?"
// 查询某波次全部 满箱回传（H7）出站消息（含状态，供未完成波次面板展示/重传）
#define SQL_SELECT_OUTBOX_FULLBOX_BY_ORDER_ALL \
    "SELECT msg_id, order_code, boxcode, grid, payload, status, retry_count, created_at FROM outbox_fullbox WHERE order_code = ? ORDER BY created_at DESC"
// ★ 2026-09-16 现场需求⑤「重传满箱切换旁的下拉框只显示本波次数据」
//   （原"全部历史"宏 SQL_SELECT_FAILED_OUTBOX_FULLBOX 已随其唯一调用方一起删除）
//   同一口径（status IN ('failed','cancelled')，列序完全一致）但**只取指定波次**：
//   下拉里不再混入其他波次的历史失败报文（跨波次补发仍可从日志/DB 追溯）。
//   绑定：?1=orderCode  ?2=limit
#define SQL_SELECT_FAILED_OUTBOX_FULLBOX_BY_ORDER \
    "SELECT msg_id, order_code, boxcode, grid, payload, status, retry_count, created_at FROM outbox_fullbox " \
    "WHERE status IN ('failed','cancelled') AND order_code = ? ORDER BY created_at DESC LIMIT ?"
// ★ 2026-09-08 UI「重传任务完结(H8)」失败波次下拉：全部历史失败/已取消重试的完结报文
#define SQL_SELECT_FAILED_OUTBOX_END \
    "SELECT msg_id, order_code, payload, status, retry_count, created_at FROM outbox_end WHERE status IN ('failed','cancelled') ORDER BY created_at DESC LIMIT ?"
// 查询某波次全部 完结回传（H8）出站消息（含状态，供未完成波次面板展示/重传）
#define SQL_SELECT_OUTBOX_END_BY_ORDER_ALL \
    "SELECT msg_id, order_code, payload, status, retry_count, created_at FROM outbox_end WHERE order_code = ? ORDER BY created_at DESC"

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
// 出站重试间隔（秒，默认 30 秒）——满箱回传（H7）等后台消息使用
#define OUTBOX_RETRY_INTERVAL_SEC  30
// ★ H8 完结回传快速重试间隔（毫秒，2026-09-02 修复"结束任务卡死"）
//   点击"结束任务"后用户等待回传结果，失败重试须快速（5s 一拍），
//   避免旧逻辑 next_retry=+30s 造成长时间无重试的静默等待
#define OUTBOX_RETRY_INTERVAL_FAST_MS  5000
// ★ 结束任务等待上限（毫秒，2026-09-02 修复"结束任务卡死/闪退"）
//   点击"结束任务"触发 H8 后，UI 与 HttpServer 会话的最终兜底时长：
//   到期无论回传成功/失败/无响应都结束等待并停止服务（不卡死、不退出程序），
//   H8 未确认的消息保留在 outbox_end，下次启动自动补传
#define END_WAIT_TIMEOUT_MS          30000

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
