#pragma once
// ============================================================================
// LifecycleLogger.h — EPC编码/格口/波次全生命周期追踪日志
//
// 设计目标：
//   ① 每次报错携带函数名+行号+模块名，方便运维人员快速定位
//   ② 追溯每个EPC编码从"WMS推送→格口查询→PLC发送→PLC反馈→分拣完成"的完整链路
//   ③ 异常件/错分件等错误情况可还原完整生命周期
//   ④ 零外部依赖，基于 hlog 增强，日志文件可读
//
// 日志格式：
//   [时间] [TID-xxx] [模块] [函数:行号] 消息内容
//   示例: [15:30:45.123] [TID-a1b2c3d4] [PLC] [PlcManager::sendCodeInfo:145] 发送PLC指令 code=WV34S1 grid=122 car=1
//
// 生命周期日志格式：
//   [时间] [TID-xxx] [LIFECYCLE] [code=WV34S1] 阶段: WMS推送→格口查询(grid=122)→PLC发送→PLC反馈(落格)→分拣完成
// ============================================================================

#include "hlog1.h"
#include <QString>
#include <QDateTime>
#include <QUuid>
#include <QMap>
#include <mutex>
#include <QObject>

// ═══════════════════════════════════════════════════════════════════════════
// TraceID 生成器（线程局部存储）
// ═══════════════════════════════════════════════════════════════════════════
class TraceContext
{
public:
    static TraceContext& instance()
    {
        thread_local TraceContext ctx;
        return ctx;
    }

    void setTraceId(const QString& id) { m_traceId = id; }
    QString traceId() const { return m_traceId; }

    // 生成新TraceID: TID-{timestamp_ms}-{uuid8}
    QString generateTraceId()
    {
        QString tid = QString("TID-%1-%2")
            .arg(QDateTime::currentMSecsSinceEpoch())
            .arg(QUuid::createUuid().toString(QUuid::Id128).left(8));
        m_traceId = tid;
        return tid;
    }

    // 继承已有TraceID（跨线程传递）
    void inheritFrom(const QString& parentTid)
    {
        m_traceId = parentTid.isEmpty() ? generateTraceId() : parentTid;
    }

private:
    TraceContext() { generateTraceId(); }
    QString m_traceId;
};

// ═══════════════════════════════════════════════════════════════════════════
// 增强型日志宏 — 携带函数名+行号+TraceID，方便定位
// ═══════════════════════════════════════════════════════════════════════════

// WCS模块（波次管理、分拣状态）
#define WCS_LOG_INFO(fmt, ...) \
    hlog_format(HLOG_LEVEL_INFO, "WCS", "[%s][%s:%d] " fmt, \
        TraceContext::instance().traceId().toLocal8Bit().data(), \
        __FUNCTION__, __LINE__, ##__VA_ARGS__)
#define WCS_LOG_WARN(fmt, ...) \
    hlog_format(HLOG_LEVEL_WARN, "WCS", "[%s][%s:%d] " fmt, \
        TraceContext::instance().traceId().toLocal8Bit().data(), \
        __FUNCTION__, __LINE__, ##__VA_ARGS__)
#define WCS_LOG_ERROR(fmt, ...) \
    hlog_format(HLOG_LEVEL_ERROR, "WCS", "[%s][%s:%d] " fmt, \
        TraceContext::instance().traceId().toLocal8Bit().data(), \
        __FUNCTION__, __LINE__, ##__VA_ARGS__)

// PLC模块（连接、收发、心跳）
#define PLC_LOG_INFO(fmt, ...) \
    hlog_format(HLOG_LEVEL_INFO, "PLC", "[%s][%s:%d] " fmt, \
        TraceContext::instance().traceId().toLocal8Bit().data(), \
        __FUNCTION__, __LINE__, ##__VA_ARGS__)
#define PLC_LOG_WARN(fmt, ...) \
    hlog_format(HLOG_LEVEL_WARN, "PLC", "[%s][%s:%d] " fmt, \
        TraceContext::instance().traceId().toLocal8Bit().data(), \
        __FUNCTION__, __LINE__, ##__VA_ARGS__)
#define PLC_LOG_ERROR(fmt, ...) \
    hlog_format(HLOG_LEVEL_ERROR, "PLC", "[%s][%s:%d] " fmt, \
        TraceContext::instance().traceId().toLocal8Bit().data(), \
        __FUNCTION__, __LINE__, ##__VA_ARGS__)

// CAM模块（相机通信、扫描结果）
#define CAM_LOG_INFO(fmt, ...) \
    hlog_format(HLOG_LEVEL_INFO, "CAM", "[%s][%s:%d] " fmt, \
        TraceContext::instance().traceId().toLocal8Bit().data(), \
        __FUNCTION__, __LINE__, ##__VA_ARGS__)
#define CAM_LOG_WARN(fmt, ...) \
    hlog_format(HLOG_LEVEL_WARN, "CAM", "[%s][%s:%d] " fmt, \
        TraceContext::instance().traceId().toLocal8Bit().data(), \
        __FUNCTION__, __LINE__, ##__VA_ARGS__)
#define CAM_LOG_ERROR(fmt, ...) \
    hlog_format(HLOG_LEVEL_ERROR, "CAM", "[%s][%s:%d] " fmt, \
        TraceContext::instance().traceId().toLocal8Bit().data(), \
        __FUNCTION__, __LINE__, ##__VA_ARGS__)

// HTTP模块（请求分发、WMS回传）
#define HTTP_LOG_INFO(fmt, ...) \
    hlog_format(HLOG_LEVEL_INFO, "HTTP", "[%s][%s:%d] " fmt, \
        TraceContext::instance().traceId().toLocal8Bit().data(), \
        __FUNCTION__, __LINE__, ##__VA_ARGS__)
#define HTTP_LOG_WARN(fmt, ...) \
    hlog_format(HLOG_LEVEL_WARN, "HTTP", "[%s][%s:%d] " fmt, \
        TraceContext::instance().traceId().toLocal8Bit().data(), \
        __FUNCTION__, __LINE__, ##__VA_ARGS__)
#define HTTP_LOG_ERROR(fmt, ...) \
    hlog_format(HLOG_LEVEL_ERROR, "HTTP", "[%s][%s:%d] " fmt, \
        TraceContext::instance().traceId().toLocal8Bit().data(), \
        __FUNCTION__, __LINE__, ##__VA_ARGS__)

// 生命周期模块（EPC编码全链路追踪）
#define LIFE_LOG(fmt, ...) \
    hlog_format(HLOG_LEVEL_INFO, "LIFECYCLE", "[%s] " fmt, \
        TraceContext::instance().traceId().toLocal8Bit().data(), ##__VA_ARGS__)

// ═══════════════════════════════════════════════════════════════════════════
// 生命周期跟踪器 — 记录每个EPC编码从"WMS推送→分拣完成"的完整链路
// ═══════════════════════════════════════════════════════════════════════════
class LifecycleTracker : public QObject
{
    Q_OBJECT
public:
    static LifecycleTracker* instance()
    {
        static LifecycleTracker s_instance;
        return &s_instance;
    }

    // 记录生命周期事件
    // stage: "WMS推送" / "格口查询" / "PLC发送" / "PLC反馈" / "分拣完成" / "异常标记" / "错分" / "波次回传"
    void logEvent(const QString& code, const QString& stage, const QString& detail = QString())
    {
        std::unique_lock<std::mutex> lock(m_lock);
        QStringList& history = m_mapLifecycle[code];
        history.append(stage + (detail.isEmpty() ? "" : "(" + detail + ")"));

        // 输出生命周期日志
        QString fullHistory = history.join(" → ");
        LIFE_LOG("[code=%s] 生命周期: %s", code.toLocal8Bit().data(), fullHistory.toLocal8Bit().data());
    }

    // 获取完整生命周期（用于异常追溯）
    QString getLifecycle(const QString& code) const
    {
        std::unique_lock<std::mutex> lock(m_lock);
        if (m_mapLifecycle.contains(code))
            return m_mapLifecycle[code].join(" → ");
        return QString("无记录");
    }

    // 清理已完成EPC编码的生命周期记录（波次完结后调用）
    void clearCode(const QString& code)
    {
        std::unique_lock<std::mutex> lock(m_lock);
        m_mapLifecycle.remove(code);
    }

    void clearAll()
    {
        std::unique_lock<std::mutex> lock(m_lock);
        m_mapLifecycle.clear();
    }

    // 输出异常件完整生命周期报告
    QString dumpExceptionReport(const QString& code) const
    {
        std::unique_lock<std::mutex> lock(m_lock);
        QString report;
        report += QString("\n╔══════════════════════════════════════════════════════╗\n");
        report += QString("║  异常件生命周期报告                                    ║\n");
        report += QString("╠══════════════════════════════════════════════════════╣\n");
        report += QString("║  EPC编码: %1").arg(code, -50).left(58) + "║\n";
        report += QString("║  时间: %1").arg(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz"), -48).left(58) + "║\n";
        report += QString("╠══════════════════════════════════════════════════════╣\n");
        if (m_mapLifecycle.contains(code))
        {
            const QStringList& history = m_mapLifecycle[code];
            for (int i = 0; i < history.size(); i++)
            {
                report += QString("║  %1. %2").arg(i + 1, 2).arg(history[i], -51).left(58) + "║\n";
            }
        }
        else
        {
            report += QString("║  (无生命周期记录)                                     ║\n");
        }
        report += QString("╚══════════════════════════════════════════════════════╝\n");
        return report;
    }

private:
    LifecycleTracker() {}
    mutable std::mutex m_lock;
    QMap<QString, QStringList> m_mapLifecycle;  // code → [阶段1, 阶段2, ...]
};

// ═══════════════════════════════════════════════════════════════════════════
// 便捷宏 — 一步记录生命周期事件
// ═══════════════════════════════════════════════════════════════════════════
#define LIFE_STAGE(code, stage, detail) \
    LifecycleTracker::instance()->logEvent(code, stage, detail)

// 预定义生命周期阶段
#define LIFE_STAGE_WMS_PUSH(code)          LIFE_STAGE(code, "WMS推送", "")
#define LIFE_STAGE_QUERY(code, grid)       LIFE_STAGE(code, "格口查询", "grid=" + grid)
#define LIFE_STAGE_QUERY_FAIL(code)        LIFE_STAGE(code, "格口查询失败", "未找到映射")
#define LIFE_STAGE_PLC_SEND(code, grid, car) LIFE_STAGE(code, "PLC发送", "grid=" + grid + " car=" + car)
#define LIFE_STAGE_PLC_SEND_FAIL(code)     LIFE_STAGE(code, "PLC发送失败", "连接断开")
#define LIFE_STAGE_PLC_FEEDBACK(code, grid) LIFE_STAGE(code, "PLC反馈落格", "grid=" + grid)
#define LIFE_STAGE_SORTED(code)            LIFE_STAGE(code, "分拣完成", "")
#define LIFE_STAGE_EXCEPTION(code, reason) LIFE_STAGE(code, "异常标记", reason)
#define LIFE_STAGE_WAVE_REPORT(code)       LIFE_STAGE(code, "波次回传WMS", "")