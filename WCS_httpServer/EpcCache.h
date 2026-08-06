#pragma once
// ============================================================================
// EpcCache.h — EPC 短缓存（T-S4-04）
//
// 功能：
//   - 缓存 EPC→barcode 映射，减少 RFID 调用
//   - TTL 过期自动失效（RFID_CACHE_TTL_SEC）
//   - 线程安全（mutex 保护）
//
// 使用：
//   EpcCache cache;
//   cache.set("EPC123", "barcode456");
//   QString barcode = cache.get("EPC123");  // 返回 barcode 或空字符串
// ============================================================================

#include <QString>
#include <QMap>
#include <QDateTime>
#include <QMutex>
#include <QMutexLocker>
#include "define.h"

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
            return QString();

        // 检查是否过期
        if (it->second <= QDateTime::currentDateTime())
        {
            m_cache.erase(it);  // 惰性清理
            return QString();
        }

        return it->first;  // 返回 barcode（key 是 epc, value 是 (barcode, expireTime)）
    }

    // 检查EPC是否在缓存中且未过期
    bool contains(const QString& epc) const
    {
        QMutexLocker locker(&m_mutex);
        auto it = m_cache.find(epc);
        if (it == m_cache.end())
            return false;
        if (it->second <= QDateTime::currentDateTime())
        {
            m_cache.erase(it);
            return false;
        }
        return true;
    }

    // 设置EPC→barcode映射，TTL从现在开始计时
    void set(const QString& epc, const QString& barcode)
    {
        if (epc.isEmpty() || barcode.isEmpty())
            return;
        QMutexLocker locker(&m_mutex);
        m_cache[epc] = {barcode, QDateTime::currentDateTime().addSecs(m_ttlSec)};
    }

    // 批量设置
    void setBatch(const QMap<QString, QString>& epcBarcodeMap)
    {
        if (epcBarcodeMap.isEmpty())
            return;
        QMutexLocker locker(&m_mutex);
        QDateTime expire = QDateTime::currentDateTime().addSecs(m_ttlSec);
        for (auto it = epcBarcodeMap.constBegin(); it != epcBarcodeMap.constEnd(); ++it)
        {
            if (!it.key().isEmpty() && !it.value().isEmpty())
                m_cache[it.key()] = {it.value(), expire};
        }
    }

    // 清理所有过期条目
    void purge()
    {
        QMutexLocker locker(&m_mutex);
        QDateTime now = QDateTime::currentDateTime();
        for (auto it = m_cache.begin(); it != m_cache.end(); )
        {
            if (it->second <= now)
                it = m_cache.erase(it);
            else
                ++it;
        }
    }

    // 清空所有缓存
    void clear()
    {
        QMutexLocker locker(&m_mutex);
        m_cache.clear();
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
    // key = epc, value = (barcode, expireTime)
    mutable QMap<QString, QPair<QString, QDateTime>> m_cache;
    mutable QMutex m_mutex;
    int m_ttlSec = RFID_CACHE_TTL_SEC;
};