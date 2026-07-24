#pragma once
// ============================================================================
// define.h — 全局配置宏定义
// 所有数值阈值集中管理，方便修改调优
// ============================================================================

// ═══════════════════════════════════════════════════════════════════════════
// HP-Socket 服务器配置
// ═══════════════════════════════════════════════════════════════════════════
#define HP_WORKER_THREADS       16       // HP-Socket I/O工作线程数（16核机器，16线程可充分利用IOCP，同时避免过多上下文切换）
#define HP_MAX_CONNECTIONS     500      // 最大连接数，防止连接池耗尽
#define HP_KEEPALIVE_TIME_MS 10000      // KeepAlive探活时间(ms)，检测死连接

// ═══════════════════════════════════════════════════════════════════════════
// 业务线程池配置
// ═══════════════════════════════════════════════════════════════════════════
#define BUSINESS_POOL_SIZE      90      // 业务线程池大小（查询/分拣/异常/回传处理），选90=预留给16扫描仪×5并发+余量
#define TASK_QUEUE_MAX_SIZE      5      // 波次推送任务队列最大排队数

// ═══════════════════════════════════════════════════════════════════════════
// 网络请求超时配置
// ═══════════════════════════════════════════════════════════════════════════
#define HTTP_TIMEOUT_MS          3000   // HTTP回传请求超时(ms)
#define WAVE_COMPLETE_TIMEOUT_MS 2000   // 波次完成回传超时(ms) —— 遵循project_memory约束：回传≤2秒，避免上游系统超时

// ═══════════════════════════════════════════════════════════════════════════
// 波次管理配置
// ═══════════════════════════════════════════════════════════════════════════
#define WAVE_MAX_RETRY           3      // 异常SKU最大重试次数
#define WAVE_TIMEOUT_MIN_DEFAULT 0      // 波次超时默认值(分钟, 0=不超时)

// ═══════════════════════════════════════════════════════════════════════════
// 定时器/监控配置
// ═══════════════════════════════════════════════════════════════════════════
#define HEALTH_CHECK_INTERVAL_MS 10000  // 健康检查定时器周期(ms)
#define WORKER_WAIT_MS           3000   // 等待ParseWorker线程退出超时(ms)
#define CONN_LONG_DURATION_MS   10000   // 连接持续超过此值视为"长连接"(ms)，10s阈值：正常HTTP请求应在此时间内完成
#define DOUBLE_BUFFER_CLEANUP_S     5   // DoubleBuffer旧Map延迟清理时间(秒)，确保并发读线程安全

// ═══════════════════════════════════════════════════════════════════════════
// PLC直连配置
// ═══════════════════════════════════════════════════════════════════════════
#define PLC_LISTEN_PORT        8192     // PLC TCP监听端口（PLC主动连接此端口，接收反馈）
#define PLC_SEND_TIMEOUT_MS    5000     // PLC发送超时(ms)
#define PLC_RECONNECT_INTERVAL_MS 3000  // PLC断线重连间隔(ms)

// ═══════════════════════════════════════════════════════════════════════════
// PLC S7 直连配置（Snap7 库，与 WCSApp 一致）
// ═══════════════════════════════════════════════════════════════════════════
#define PLC_S7_IP          "192.168.0.1"  // PLC S7 连接 IP（需根据实际环境修改）
#define PLC_S7_RACK              0        // S7 机架号
#define PLC_S7_SLOT              1        // S7 槽位号
#define PLC_S7_DB_NUM            1        // 分拣数据 DB 块编号
#define PLC_S7_WRITE_OFFSET   1000        // 条码写入 DB1 偏移地址
#define PLC_S7_WRITE_SIZE       42        // 条码写入数据大小（字节）

// ═══════════════════════════════════════════════════════════════════════════
// UI/日志限制
// ═══════════════════════════════════════════════════════════════════════════
#define LOG_MAX_BLOCK_COUNT    5000     // 日志文本框最大行数
#define LOG_MAX_TEXT_SIZE    100000     // 日志文本框最大字符数
#define LOG_RETAIN_DAYS          30     // 日志文件保留天数

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
//     WCS_LOG_INFO("波次创建成功 waveId=%s", waveId.toStdString().c_str());
//     PLC_LOG_ERROR("发送PLC指令失败 code=%s err=%s", code, err);
//     HTTP_LOG_WARN("WMS回传超时 url=%s timeout=%dms", url, timeout);
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
