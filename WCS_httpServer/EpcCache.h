#pragma once
// ============================================================================
// EpcCache.h — EPC 短缓存（T-S4-04）
//
// 功能：
//   - 缓存 EPC→barcode+carNum 映射，减少 RFID 调用
//   - TTL 过期自动失效（RFID_CACHE_TTL_SEC）
//   - 线程安全（mutex 保护）
//
// 使用：
//   EpcCache cache;
//   cache.set("EPC123", "barcode456", "002");
//   QString barcode = cache.get("EPC123");     // 返回 barcode
//   QString carNum  = cache.getCarNum("EPC123"); // 返回 carNum
// ============================================================================

#include <QString>
#include <QHash>
#include <QDateTime>
#include <QMutex>
#include <QMutexLocker>
#include "define.h"
#include "LogService.h"

// 辅助宏：将 carNum 数字转为 3 位补零字符串
#define CAR_NUM_STR(n) QString("%1").arg(n, 3, 10, QChar('0'))
#define DEFAULT_CAR_STR CAR_NUM_STR(DEFAULT_CAR_NUM)

// ──── EpcCache 专用日志宏（写入 ./log/EPC/epc.log）────
// （宏定义已移至 LogService.h 统一管理）

// ★ 缓存条目：barcode + carNum + SKU 绑定状态 + 过期时间 + 推送到达时间
struct EpcCacheEntry
{
    QString   barcode;
    QString   carNum;       // ★ 来自 RFID 的小车号，默认 DEFAULT_CAR_STR("001")
    bool      skuBound = false;  // ★ SKU-EPC 绑定是否完成（通过 RFID 查询获取）
    QDateTime expireTime;         // TTL 过期时间
    QDateTime receivedAt;         // ★ RFID 推送首次到达时间（用于 1s 超时判断，PLC_SEND_TIMEOUT_MS）
    QDateTime sentAt;             // ★ PLC 发送指令时间（发送动作执行后记录，用于追溯 开始处理→发送 耗时）
    QString   seq;                // ★ 2026-09-05 RFID 推送流水号（保存追溯用）

    bool isExpired()      const { return expireTime <= QDateTime::currentDateTime(); }
    bool isReadyForPlc()  const { return skuBound && !carNum.isEmpty(); }
    // ★ 从 RFID 推送到达起算，超过 PLC_SEND_TIMEOUT_MS 则视为超时（应入异常格口）
    bool isSendTimeout()  const { return receivedAt.msecsTo(QDateTime::currentDateTime()) > PLC_SEND_TIMEOUT_MS; }
};

class EpcCache
{
public:
    explicit EpcCache(int ttlSec = RFID_CACHE_TTL_SEC)
        : m_ttlSec(ttlSec)
    {
    }

    // 获取EPC对应的barcode，如果过期或不存在返回空字符串
    QString get(const QString& epc) const
    {
        QMutexLocker locker(&m_mutex);
        auto it = m_cache.find(epc);
        if (it == m_cache.end())
        {
            EPC_WARN("get 缓存未命中 epc=%s cacheSize=%d", epc.toLocal8Bit().data(), m_cache.size());
            return QString();
        }

        if (it->isExpired())
        {
            EPC_WARN("get 缓存已过期 epc=%s barcode=%s expire=%s cacheSize=%d",
                epc.toLocal8Bit().data(), it->barcode.toLocal8Bit().data(),
                it->expireTime.toString("HH:mm:ss").toLocal8Bit().data(), m_cache.size());
            m_cache.erase(it);
            return QString();
        }

        return it->barcode;
    }

    // ★ 获取EPC对应的小车号，如果过期或不存在返回空字符串
    //    返回空字符串表示缓存未命中，调用方应使用 DEFAULT_CAR_NUM 兜底
    QString getCarNum(const QString& epc) const
    {
        QMutexLocker locker(&m_mutex);
        auto it = m_cache.find(epc);
        if (it == m_cache.end())
        {
            EPC_WARN("getCarNum 缓存未命中 epc=%s cacheSize=%d", epc.toLocal8Bit().data(), m_cache.size());
            return QString();
        }

        if (it->isExpired())
        {
            EPC_WARN("getCarNum 缓存已过期 epc=%s carNum=%s expire=%s cacheSize=%d",
                epc.toLocal8Bit().data(), it->carNum.toLocal8Bit().data(),
                it->expireTime.toString("HH:mm:ss").toLocal8Bit().data(), m_cache.size());
            m_cache.erase(it);
            return QString();
        }

        QString carNum = it->carNum.isEmpty() ? DEFAULT_CAR_STR : it->carNum;
        EPC_INFO("getCarNum 命中 epc=%s carNum=%s barcode=%s expire=%s",
            epc.toLocal8Bit().data(), carNum.toLocal8Bit().data(),
            it->barcode.toLocal8Bit().data(), it->expireTime.toString("HH:mm:ss").toLocal8Bit().data());
        return carNum;
    }

    // ★ 2026-09-05：保存该EPC最近一次推送的流水号（追溯用；随条目TTL过期清理）
    void setSeq(const QString& epc, const QString& seq)
    {
        if (epc.isEmpty() || seq.isEmpty()) return;
        QMutexLocker locker(&m_mutex);
        auto it = m_cache.find(epc);
        if (it == m_cache.end()) return;                 // 条目不存在（已被清理）→ 忽略
        if (it->isExpired())
        {
            m_cache.erase(it);
            return;
        }
        it->seq = seq;
    }

    // ★ 2026-09-05：读取EPC的流水号（无记录/已过期返回空）
    QString getSeq(const QString& epc) const
    {
        QMutexLocker locker(&m_mutex);
        auto it = m_cache.find(epc);
        if (it == m_cache.end()) return QString();
        if (it->isExpired())
        {
            m_cache.erase(it);
            return QString();
        }
        return it->seq;
    }

    // 检查EPC是否在缓存中且未过期
    bool contains(const QString& epc) const
    {
        QMutexLocker locker(&m_mutex);
        auto it = m_cache.find(epc);
        if (it == m_cache.end())
            return false;
        if (it->isExpired())
        {
            m_cache.erase(it);
            return false;
        }
        return true;
    }

    // 设置EPC→barcode+carNum映射，TTL从现在开始计时
    void set(const QString& epc, const QString& barcode, const QString& carNum = DEFAULT_CAR_STR)
    {
        if (epc.isEmpty() || barcode.isEmpty())
        {
            EPC_WARN("set 跳过空值 epc=%s barcode=%s", epc.toLocal8Bit().data(), barcode.toLocal8Bit().data());
            return;
        }
        QMutexLocker locker(&m_mutex);
        bool overwrite = m_cache.contains(epc);
        EpcCacheEntry entry;
        entry.barcode    = barcode;
        entry.carNum     = carNum.isEmpty() ? DEFAULT_CAR_STR : carNum;
        entry.expireTime = QDateTime::currentDateTime().addSecs(m_ttlSec);
        m_cache[epc] = entry;
        EPC_INFO("set %s epc=%s barcode=%s carNum=%s ttl=%ds cacheSize=%d",
            overwrite ? "覆盖" : "新增", epc.toLocal8Bit().data(), barcode.toLocal8Bit().data(),
            entry.carNum.toLocal8Bit().data(), m_ttlSec, m_cache.size());
    }

    // 批量设置（兼容旧接口：epcBarcodeMap 只含 barcode，carNum 默认 DEFAULT_CAR_STR）
    void setBatch(const QMap<QString, QString>& epcBarcodeMap)
    {
        if (epcBarcodeMap.isEmpty())
            return;
        QMutexLocker locker(&m_mutex);
        QDateTime expire = QDateTime::currentDateTime().addSecs(m_ttlSec);
        int skipCount = 0;
        for (auto it = epcBarcodeMap.constBegin(); it != epcBarcodeMap.constEnd(); ++it)
        {
            if (!it.key().isEmpty() && !it.value().isEmpty())
            {
                EpcCacheEntry entry;
                entry.barcode    = it.value();
                entry.carNum     = DEFAULT_CAR_STR;  // 旧接口无 carNum，默认 DEFAULT_CAR_STR
                entry.expireTime = expire;
                m_cache[it.key()] = entry;
            }
            else
            {
                skipCount++;
            }
        }
        EPC_INFO("setBatch 写入 %d/%d ttl=%ds cacheSize=%d %s",
            epcBarcodeMap.size() - skipCount, epcBarcodeMap.size(), m_ttlSec, m_cache.size(),
            skipCount > 0 ? QString("跳过空值%1条").arg(skipCount).toLocal8Bit().data() : "");
    }

    // ★ 批量设置（含 carNum）：epc → {barcode, carNum}
    //    如果已有 SKU 绑定数据（skuBound=true），保留已有的 barcode，只更新 carNum
    void setBatchWithCar(const QMap<QString, QPair<QString, QString>>& epcDataMap)
    {
        if (epcDataMap.isEmpty())
        {
            EPC_WARN("setBatchWithCar 跳过空数据");
            return;
        }
        QMutexLocker locker(&m_mutex);
        QDateTime expire = QDateTime::currentDateTime().addSecs(m_ttlSec);
        int writeCount = 0, skipCount = 0, carCount = 0, readyCount = 0;
        for (auto it = epcDataMap.constBegin(); it != epcDataMap.constEnd(); ++it)
        {
            if (!it.key().isEmpty())
            {
                auto existing = m_cache.find(it.key());
                bool hasExisting = (existing != m_cache.end() && !existing->isExpired());

                EpcCacheEntry entry;
                // ★ 保留已有的 SKU 绑定数据（barcode + skuBound），不覆盖
                if (hasExisting && existing->skuBound)
                {
                    entry.barcode = existing->barcode;
                    entry.skuBound = true;
                }
                else if (!it.value().first.isEmpty())
                {
                    entry.barcode = it.value().first;
                    // ★ 纠正: RFID 推送中的 barcode 就是 SKU 编码（客户确认 2026-08-14）
                    //   如果推送带了 barcode，直接标记 skuBound=true，无需再单独查询
                    entry.skuBound = true;
                }
                // ★ 更新 carNum（RFID 推送的）
                entry.carNum = it.value().second.isEmpty() ? DEFAULT_CAR_STR : it.value().second;
                entry.expireTime = expire;
                // ★ 记录 RFID 推送到达时间（1s 超时判断起点，PLC_SEND_TIMEOUT_MS）
                //   重复推送分两类处理（★ 2026-09-09 需求7 按用户方案在"计时起点"处管理）：
                //   ① 双读（同一件仍在轨道上、尚未发出指令 sentAt 空）→ 保留首次到达时间，
                //      防止同一EPC多次读到被延后计时起点（超时永不触发）
                //   ② 二次上传（上次已发出 PLC 指令 sentAt 有效，件回线重扫/再次推送）
                //      → 计时起点归 0 重新单独计时：同一件的多次尝试各自计时、不叠加
                if (!hasExisting)
                {
                    entry.receivedAt = QDateTime::currentDateTime();
                }
                else if (existing->sentAt.isValid())
                {
                    entry.receivedAt = QDateTime::currentDateTime();  // 二次上传：重新起算（归0）
                    entry.sentAt     = QDateTime();                    // 清上次发送时间，getHandleSendMs 重新起算
                    EPC_WARN("setBatchWithCar 二次上传重新计时 epc=%s（上次已发送过，单独计时）",
                        it.key().toLocal8Bit().data());
                }
                else
                {
                    entry.receivedAt = existing->receivedAt;   // 双读：保留首次到达时间
                }
                m_cache[it.key()] = entry;
                writeCount++;
                if (!it.value().second.isEmpty() && it.value().second != DEFAULT_CAR_STR)
                    carCount++;
                if (entry.isReadyForPlc())
                    readyCount++;
            }
            else
            {
                skipCount++;
            }
        }
        EPC_INFO("setBatchWithCar 写入 %d/%d carNum=%d ready=%d ttl=%ds cacheSize=%d %s",
            writeCount, epcDataMap.size(), carCount, readyCount, m_ttlSec, m_cache.size(),
            skipCount > 0 ? QString("跳过空值%1条").arg(skipCount).toLocal8Bit().data() : "");
    }

    // ★ SKU-EPC 绑定：从 RFID 查询获取 EPC→barcode 映射（逐条调用，标记 skuBound=true）
    //    与 setBatchWithCar 的区别：setSkuBinding 只设置 barcode 和 skuBound 标记，
    //    不覆盖已有的 carNum（carNum 由 RFID 推送独立设置）
    void setSkuBinding(const QString& epc, const QString& barcode)
    {
        if (epc.isEmpty() || barcode.isEmpty())
        {
            EPC_WARN("setSkuBinding 跳过空值 epc=%s barcode=%s", epc.toLocal8Bit().data(), barcode.toLocal8Bit().data());
            return;
        }
        QMutexLocker locker(&m_mutex);
        bool exists = m_cache.contains(epc);
        EpcCacheEntry& entry = m_cache[epc];
        entry.barcode = barcode;
        entry.skuBound = true;
        if (!exists)
        {
            // ★ 极端情况：旧条目已过期被 erase，新条目丢失了 carNum
            //   TTL=300s，SKU 查询最长 21s，理论上不会发生，但加日志方便排查
            entry.expireTime = QDateTime::currentDateTime().addSecs(m_ttlSec);
            EPC_WARN("setSkuBinding 条目已过期重建 epc=%s barcode=%s carNum丢失(旧条目TTL过期)",
                epc.toLocal8Bit().data(), barcode.toLocal8Bit().data());
        }
        // 如果已有 carNum，标记为就绪
        bool ready = entry.isReadyForPlc();
        EPC_INFO("setSkuBinding %s epc=%s barcode=%s skuBound carNum=%s ready=%d cacheSize=%d",
            exists ? "覆盖" : "新增", epc.toLocal8Bit().data(), barcode.toLocal8Bit().data(),
            entry.carNum.toLocal8Bit().data(), ready, m_cache.size());
    }

    // ★ 判断 EPC 是否已就绪可发送 PLC（SKU 已绑定 + carNum 已获取）
    bool isReadyForPlc(const QString& epc) const
    {
        QMutexLocker locker(&m_mutex);
        auto it = m_cache.find(epc);
        if (it == m_cache.end()) return false;
        if (it->isExpired()) return false;
        return it->isReadyForPlc();
    }

    // ★ 判断 EPC 从 RFID 推送到现在是否已超过 PLC_SEND_TIMEOUT_MS（1s 超时）
    //   超时则不应再发送 PLC，应入异常格口
    bool isSendTimeout(const QString& epc) const
    {
        QMutexLocker locker(&m_mutex);
        auto it = m_cache.find(epc);
        if (it == m_cache.end()) return true;  // 不存在视为超时
        if (it->isExpired()) return true;       // 已过期视为超时
        return it->isSendTimeout();
    }

    // ★ 获取 EPC 的 RFID 推送到达时间（用于超时日志）
    qint64 getElapsedMs(const QString& epc) const
    {
        QMutexLocker locker(&m_mutex);
        auto it = m_cache.find(epc);
        if (it == m_cache.end()) return -1;
        return it->receivedAt.msecsTo(QDateTime::currentDateTime());
    }

    // ★ 记录 EPC 的 PLC 发送指令时间（发送动作执行后调用）
    void markSent(const QString& epc)
    {
        QMutexLocker locker(&m_mutex);
        auto it = m_cache.find(epc);
        if (it == m_cache.end()) return;
        it->sentAt = QDateTime::currentDateTime();
    }

    // ★ 2026-09-09 需求7：件落入异常口后重置计时（elapsed 归 0）
    //   receivedAt 置为当前时间（isSendTimeout/getElapsedMs 重新起算），sentAt 清空（重新发送时 markSent 重记）
    //   效果：该 EPC 二次上传（RFID 重推）时不再因旧的 receivedAt 立即判定超时
    void resetTiming(const QString& epc)
    {
        QMutexLocker locker(&m_mutex);
        auto it = m_cache.find(epc);
        if (it == m_cache.end()) return;
        it->receivedAt = QDateTime::currentDateTime();
        it->sentAt     = QDateTime();
        EPC_WARN("resetTiming 异常件计时归0 epc=%s", epc.toLocal8Bit().data());
    }

    // ★ 获取 开始处理(RFID首次到达) → PLC发送 的耗时；未发送返回 -1
    qint64 getHandleSendMs(const QString& epc) const
    {
        QMutexLocker locker(&m_mutex);
        auto it = m_cache.find(epc);
        if (it == m_cache.end()) return -1;
        if (it->sentAt.isNull()) return -1;   // 尚未发送
        return it->receivedAt.msecsTo(it->sentAt);
    }

    // ★ 获取 EPC 对应的 barcode 和 carNum（用于就绪后发送 PLC）
    //    返回 {barcode, carNum}，未就绪返回空
    QPair<QString, QString> getPlcData(const QString& epc) const
    {
        QMutexLocker locker(&m_mutex);
        auto it = m_cache.find(epc);
        if (it == m_cache.end()) return {};
        if (it->isExpired()) return {};
        if (!it->isReadyForPlc()) return {};
        return {it->barcode, it->carNum};
    }

    // 清理所有过期条目
    void purge()
    {
        QMutexLocker locker(&m_mutex);
        QDateTime now = QDateTime::currentDateTime();
        int beforeSize = m_cache.size();
        for (auto it = m_cache.begin(); it != m_cache.end(); )
        {
            if (it->expireTime <= now)
                it = m_cache.erase(it);
            else
                ++it;
        }
        int purged = beforeSize - m_cache.size();
        if (purged > 0)
        {
            EPC_INFO("purge 清理过期 %d/%d cacheSize=%d", purged, beforeSize, m_cache.size());
        }
    }

    // 清空所有缓存
    void clear()
    {
        QMutexLocker locker(&m_mutex);
        int beforeSize = m_cache.size();
        m_cache.clear();
        EPC_WARN("clear 清空全部缓存 beforeSize=%d", beforeSize);
    }

    // 当前缓存条目数
    int size() const
    {
        QMutexLocker locker(&m_mutex);
        return m_cache.size();
    }

    // 设置 TTL（秒）
    void setTtl(int sec) { m_ttlSec = sec; }

private:
    // key = epc, value = EpcCacheEntry
    // ★ QHash: O(1) 查找，比 QMap O(log n) 更快，适合高频 RFID 推送场景
    mutable QHash<QString, EpcCacheEntry> m_cache;
    mutable QMutex m_mutex;
    int m_ttlSec = RFID_CACHE_TTL_SEC;
};