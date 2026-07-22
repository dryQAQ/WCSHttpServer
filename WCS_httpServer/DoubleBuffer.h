#pragma once
// ============================================================================
// DoubleBuffer.h — 双缓冲无锁读取Map
//
// 写入线程: ParseWorker 在 m_pBackup 上构建新数据，完成后原子交换
// 读取线程: WCSApp查询 / 内部状态查询，通过 load() 无锁读取 m_pActive
// 零锁竞争，零延迟，适合高频读取场景（分拣扫描）
// ============================================================================

#include <QMap>
#include <QString>
#include <QTimer>
#include <atomic>
#include <memory>

// ──── 格口映射条目 ────
struct GridEntry
{
    QString gridNum;        // 格口号（同品多格口则逗号分隔，如 "1,2,3"）
    QString gridType = "普通格口";
    int     gridCount = 0;
};

// ──── 双缓冲无锁Map ────
template<typename K, typename V>
class DoubleBuffer
{
public:
    DoubleBuffer() : m_pActive(nullptr) {}
    ~DoubleBuffer() { delete m_pActive.load(); }

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
        QMap<K, V>* old = m_pActive.exchange(newMap, std::memory_order_acq_rel);
        // 延迟释放旧Map：5000ms确保所有并发读取完成
        if (old)
        {
            QTimer::singleShot(5000, [old]() { delete old; });
        }
    }

    // 获取当前活动指针（用于需要直接遍历的场景）
    const QMap<K, V>* activeMap() const
    {
        return m_pActive.load(std::memory_order_acquire);
    }

private:
    std::atomic<QMap<K, V>*> m_pActive;
};

// ──── 类型别名 ────
using GridBuffer = DoubleBuffer<QString, GridEntry>;
