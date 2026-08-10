#pragma once
// ============================================================================
// DoubleBuffer.h — 双缓冲无锁读取Map
//
// 写入线程: ParseWorker 在 m_pBackup 上构建新数据，完成后原子交换
// 读取线程: API查询 / 内部状态查询，通过 load() 无锁读取 m_pActive
// 零锁竞争，零延迟，适合高频读取场景（分拣扫描）
//
// 内存管理: 使用 std::deque 存储待删除的旧Map指针和时间戳，
//           在 prepareSwap 时自动清理过期（>5秒）的指针
// ============================================================================

#include <QMap>
#include <QString>
#include <QSet>
#include <deque>
#include <chrono>
#include <atomic>
#include "define.h"

// ──── 格口映射条目 ────
// 通过条码一次查询即可获得：
//   ① 格口信息：落格位置、格口属性、格口件数
//   ② 批次信息：所属批次号、批次总件数、SKU种类数
struct GridEntry
{
    // ── 格口信息 ──
    QString gridNum;        // 格口号（同品多格口则逗号分隔，如 "1,2,3"）
    QString gridType = "普通格口";
    int     gridCount = 0;
    QString volu;           // 来源库位编码（fromLocation，锁格回传 WMS 用）
    QString obxCode;        // 容器号（WMS 下发时携带，后续可能用于追溯）

    // ── 批次信息 ──
    QString orderCode;      // 批次号（所属波次）
    int     orderQty  = 0;  // 该批次总件数
    int     skuCount  = 0;  // 该批次SKU种类数
};

// ──── 待删除指针记录 ────
template<typename T>
struct PendingDelete
{
    T* ptr;
    std::chrono::steady_clock::time_point deleteTime;
    PendingDelete(T* p) : ptr(p), deleteTime(std::chrono::steady_clock::now()) {}
};

// ──── 双缓冲无锁Map ────
template<typename K, typename V>
class DoubleBuffer
{
public:
    DoubleBuffer() : m_pActive(nullptr) {}
    ~DoubleBuffer()
    {
        delete m_pActive.load();
        std::lock_guard<std::mutex> lock(m_deleteMutex);
        for (auto& pd : m_pendingDeletes)
            delete pd.ptr;
    }

    // ──── 读操作（无锁，任意线程安全）────
    V get(const K& key, const V& defaultValue = V()) const
    {
        QMap<K, V>* pMap = m_pActive.load(std::memory_order_acquire);
        if (!pMap) return defaultValue;
        auto it = pMap->constFind(key);
        return (it != pMap->constEnd()) ? it.value() : defaultValue;
    }

    bool contains(const K& key) const
    {
        QMap<K, V>* pMap = m_pActive.load(std::memory_order_acquire);
        return pMap && pMap->contains(key);
    }

    int size() const
    {
        QMap<K, V>* pMap = m_pActive.load(std::memory_order_acquire);
        return pMap ? pMap->size() : 0;
    }

    QMap<K, V> snapshot() const
    {
        QMap<K, V>* pMap = m_pActive.load(std::memory_order_acquire);
        return pMap ? *pMap : QMap<K, V>();
    }

    int uniqueValueCount() const
    {
        QMap<K, V>* pMap = m_pActive.load(std::memory_order_acquire);
        if (!pMap) return 0;
        QSet<QString> unique;
        for (auto it = pMap->constBegin(); it != pMap->constEnd(); ++it)
        {
            QString v = it.value().gridNum;
            if (v.contains(','))
            {
                for (const QString& g : v.split(',', Qt::SkipEmptyParts))
                    unique.insert(g.trimmed());
            }
            else unique.insert(v);
        }
        return unique.size();
    }

    // ──── 写操作（仅解析线程调用）────
    // 在 m_pBackup 上构建完毕后调用 swap 原子切换
    void prepareSwap(QMap<K, V>* newMap)
    {
        cleanupOldMaps();

        QMap<K, V>* old = m_pActive.exchange(newMap, std::memory_order_acq_rel);
        // 将旧Map加入待删除队列，5秒后清理
        if (old)
        {
            std::lock_guard<std::mutex> lock(m_deleteMutex);
            m_pendingDeletes.emplace_back(old);
        }
    }

    // 获取当前活动指针（用于需要直接遍历的场景）
    const QMap<K, V>* activeMap() const
    {
        return m_pActive.load(std::memory_order_acquire);
    }

private:
    // 清理过期（>5秒）的旧Map指针
    void cleanupOldMaps()
    {
        std::lock_guard<std::mutex> lock(m_deleteMutex);
        auto now = std::chrono::steady_clock::now();
        auto it = m_pendingDeletes.begin();
        while (it != m_pendingDeletes.end())
        {
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - it->deleteTime);
            if (elapsed.count() >= DOUBLE_BUFFER_CLEANUP_S)
            {
                delete it->ptr;
                it = m_pendingDeletes.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    std::atomic<QMap<K, V>*> m_pActive;
    std::deque<PendingDelete<QMap<K, V>>> m_pendingDeletes;
    std::mutex m_deleteMutex;
};

// ──── 类型别名 ────
using GridBuffer = DoubleBuffer<QString, GridEntry>;
