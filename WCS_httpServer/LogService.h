#pragma once
// ============================================================================
// LogService.h — 统一日志服务（P3 重构：抽取 LogService，统一日志职责）
//
// 职责：
//   ① 集中管理所有模块日志宏，消除各 .cpp 文件中的重复宏定义
//   ② 封装 hlog（文件日志）+ LogCenter（GUI 日志）+ LifecycleTracker（生命周期）
//   ③ 统一的日志格式：[时间] [TID-xxx] [模块] [函数:行号] 消息
//
// 使用方式：
//   每个 .cpp 文件只需 #include "LogService.h"，即可使用所有模块日志宏
//   无需再单独 include "hlog1.h"、"log_center.h"、"LifecycleLogger.h"
//
// 文件日志输出目录（由 hlog 配置文件控制）：
//   ./log/WCS/WCS.log           — WCS_INFO / WCS_LOG_INFO / WCS_LOG_WARN / WCS_LOG_ERROR
//   ./log/HTTP/http.log         — HTTP_INFO / HTTP_LOG_INFO / HTTP_LOG_WARN / HTTP_LOG_ERROR
//   ./log/PLC/PLC.log           — PLC_INFO / PLC_LOG_INFO / PLC_LOG_WARN / PLC_LOG_ERROR
//   ./log/LIFECYCLE/lifecycle.log — LIFE_LOG / LIFE_STAGE
//   ./log/EPC/epc.log           — EPC_INFO / EPC_WARN / EPC_ERROR
//   ./log/DataBase/DataBase.log  — Data_INFO / Data_WARN / Data_ERROR
//   ./log/WAVE_ITEM/wave_item.log — WAVE_ITEM_INFO
//   ./log/Run/run.log            — LOG_INFO / LOG_WARN / LOG_ERROR（通用日志）
//
// 修改记录：
//   2026-08-11  从各文件提取分散的宏定义，集中管理
// ============================================================================

// ──── 底层日志库 ────
#include "hlog1.h"              // hlog_format / HLOG_LEVEL_* / WCS_INFO / PLC_INFO 等

// ──── 生命周期日志（WCS_LOG_* / PLC_LOG_* / HTTP_LOG_* / LIFE_LOG / LIFE_STAGE）────
#include "LifecycleLogger.h"

// ──── GUI 日志中心（Cross-thread safe UI logging）────
#include "log_center.h"

// ═══════════════════════════════════════════════════════════════════════════
// 模块专用日志宏（原分散在各 .cpp 文件中，现集中定义）
// ═══════════════════════════════════════════════════════════════════════════

// ──── HTTP 模块（请求分发、WMS 回传）────
// 写入 ./log/HTTP/http.log
#ifndef HTTP_INFO
#define HTTP_INFO(fmt, ...)  hlog_format(HLOG_LEVEL_INFO,  "HTTP", "\t" fmt, ##__VA_ARGS__)
#define HTTP_WARN(fmt, ...)  hlog_format(HLOG_LEVEL_WARN,  "HTTP", "\t" fmt, ##__VA_ARGS__)
#define HTTP_ERROR(fmt, ...) hlog_format(HLOG_LEVEL_ERROR, "HTTP", "\t" fmt, ##__VA_ARGS__)
#endif

// ──── EPC 缓存模块（EpcCache 读写日志）────
// 写入 ./log/EPC/epc.log
#ifndef EPC_INFO
#define EPC_INFO(fmt, ...)  hlog_format(HLOG_LEVEL_INFO,  "EPC", "\t" fmt, ##__VA_ARGS__)
#define EPC_WARN(fmt, ...)  hlog_format(HLOG_LEVEL_WARN,  "EPC", "\t" fmt, ##__VA_ARGS__)
#define EPC_ERROR(fmt, ...) hlog_format(HLOG_LEVEL_ERROR, "EPC", "\t" fmt, ##__VA_ARGS__)
#endif

// ──── 数据库模块（SortingDatabase 操作日志）────
// 写入 ./log/DataBase/DataBase.log
#ifndef Data_INFO
#define Data_INFO(fmt, ...)  hlog_format(HLOG_LEVEL_INFO,  "DataBase", "\t" fmt, ##__VA_ARGS__)
#define Data_WARN(fmt, ...)  hlog_format(HLOG_LEVEL_WARN,  "DataBase", "\t" fmt, ##__VA_ARGS__)
#define Data_ERROR(fmt, ...) hlog_format(HLOG_LEVEL_ERROR, "DataBase", "\t" fmt, ##__VA_ARGS__)
#endif

// ──── 波次明细日志（ParseWorker 每个 items[] 子项单独记录）────
// 写入 ./log/WAVE_ITEM/wave_item.log
#ifndef WAVE_ITEM_INFO
#define WAVE_ITEM_INFO(fmt, ...) hlog_format(HLOG_LEVEL_INFO, "WAVE_ITEM", "\t" fmt, ##__VA_ARGS__)
#endif

// ──── RFID 模块（RFID 推送 TCP 原始报文 + 解析）────
// ★ 2026-09-06：不再单独建 ./log/RFID/rfid.log（现场反馈独立文件无法写入/不便查看），
//   统一并入 run.log（与 LOG_* 相同文件），保留 RFID_* 宏名避免改调用点
#ifndef RFID_INFO
#define RFID_INFO(fmt, ...)  LOG_INFO(fmt, ##__VA_ARGS__)
#define RFID_WARN(fmt, ...)  LOG_WARN(fmt, ##__VA_ARGS__)
#define RFID_ERROR(fmt, ...) LOG_ERROR(fmt, ##__VA_ARGS__)
#endif

// ──── SEND 模块（WMS 出站回传报文归档）────
// ★ 2026-09-06：不再单独建 ./log/SEND/send.log（现场反馈独立文件无法写入/不便查看），
//   统一并入 run.log；发送/响应报文与运行日志同文件，检索 "[原始报文]" 即可定位
#ifndef SEND_INFO
#define SEND_INFO(fmt, ...)  LOG_INFO(fmt, ##__VA_ARGS__)
#define SEND_WARN(fmt, ...)  LOG_WARN(fmt, ##__VA_ARGS__)
#define SEND_ERROR(fmt, ...) LOG_ERROR(fmt, ##__VA_ARGS__)
#endif