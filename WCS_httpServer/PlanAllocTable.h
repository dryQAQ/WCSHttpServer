#pragma once
// ============================================================================
// PlanAllocTable.h — 波次级「计划分配表」（★ 2026-09-14 落格结构优化）
//
// ═══════════════════════════════════════════════════════════════════════════
// 客户口径（本表要落地的不变量）
//   ① 每个格口都有"对应这个产品的数量"——同一 SKU 可以同时计划到
//      「正常分拣（分类）格口」与「发货格口」，两个格口各有一份数量；
//      落格必须**按各格口的数量**分过去，而不是全部投第一个格口。
//   ② 人工失误/同时两件在线导致"实落 > 计划"必须被系统拦住：
//      已落格 + 在途认领 ≥ 计划件数 → 后续件一律改投异常口（66 号）。
//
// ═══════════════════════════════════════════════════════════════════════════
// 为什么这样设计（算法层面：把"每次现算"改成"一次性编译"）
//
//   改造前的做法：计划散在 DoubleBuffer::GridEntry::planQtyPerGrid（QMap，key 为
//   "034" 字符串）+ HttpServer::m_gridLandedNum（QMap<SKU,QMap<格口,QSet<EPC>>>），
//   每次选格都要现查 QMap、现数 QSet、逐格口比较；而且**候选顺序取自 gridNum
//   逗号串的解析顺序** —— 同一份 H4 计划，items 下发顺序不同会得到不同的落格
//   分布，事后无法重现（不可追溯）。
//
//   本表把计划在**波次开始时一次性编译成定长数组**：
//     · 格口顺序在编译期按格口号升序固化 → 决策与 items 到达顺序无关（可重现）；
//     · 运行期只做「整数加减 + 数组下标」，无字符串 key 构造、无容器迭代；
//     · "是否全部满额"由每 SKU 的 skuRemain 计数器直接给出，不再遍历推导。
//
// ═══════════════════════════════════════════════════════════════════════════
// ★★ 线程规则（必须遵守，违反即数据错乱）★★
//   · claim / release / moveGap / build / clear / snapshot → **仅 Qt 主线程**
//     （选格链路 trySendToPlcForEpc 的全部调用点、波次生命周期回调都在主线程）
//   · commitOnLanded                                        → PLC 反馈接收线程池
//                                                             （唯一跨线程入口）
//   全表由调用方（HttpServer）用**单一互斥量**保护，且"判定 + 改数"必须在同一把
//   锁内完成 —— 这正是改造前 check-then-count（先查已落数、再另行登记）导致
//   同时两件在线时超计划的根因。
//
// ═══════════════════════════════════════════════════════════════════════════
// 空间换时间账（单波次 5 万件 / 2 万 SKU / 平均 2 个计划格口 ≈ 4 万计划单元）
//   m_cells 40,000×8B  ≈ 0.32 MB    m_quota 40,000×16B ≈ 0.64 MB
//   m_skus  20,000×16B ≈ 0.32 MB    m_skuIndex ≈ 1.0 MB
//   在途认领 ≤2000×~50B ≈ 0.10 MB          合计 ≈ 2.4 MB / 波次，峰值 < 5 MB
//   换来：每件选格由「1 次哈希 + G 次 QMap 查找 + G 次比较」降到
//        「1 次哈希 + 数组扫描（通常命中即停）+ 3 次整数加减」。
//
// ═══════════════════════════════════════════════════════════════════════════
// 复杂度
//   claim()           O(G)，G=该 SKU 计划格口数（通常 1~3）；带游标，命中即停
//   commitOnLanded()  O(1)：SKU 下标 + 数组下标，无需遍历格口
//   release()         O(1)：一次哈希取认领
//   audit()           O(计划单元)，仅在每 30s 巡检与波次报告时调用
// ============================================================================

#include <QString>
#include <QStringList>
#include <QHash>
#include <QVector>
#include <QSet>
#include <array>
#include <atomic>
#include <algorithm>
#include <cstdint>

// ──── 波次开始时一次性编译的不可变计划（运行期只读）────
struct PlanCell
{
    qint16  grid     = 0;   // 内部格口号 1..BINDING_SLOT_COUNT
    quint8  gridType = 0;   // 0=正常分拣(分类)  1=异常(66号)  2=发货
    qint32  planQty  = 0;   // H4 对该 (SKU,格口) 的计划件数（同格口多行求和）
};

// 编译入参（解析期临时结构，编译后即释放，不驻留）
struct PlanGridInput
{
    qint16 grid = 0;
    quint8 type = 0;
    qint32 qty  = 0;
};

// 编译结果：每个 SKU 的计划数组视图
struct SkuPlan
{
    qint32 planTotal  = 0;  // Σ planQty（= H4 该 SKU 下发总数，供不变量③校验）
    qint16 idxFirst   = 0;  // 在 m_cells 中的起始下标
    qint16 idxCount   = 0;  // 计划格口个数（通常 1~3）
    qint16 cursorFrom = 0;  // ★ 纯加速提示：跳过已满额格口（失效只会多扫几下，结果不变）
    qint32 skuRemain  = 0;  // ★ 该 SKU 剩余可用额度 → "是否全满"变成 1 次整数比较
    // ★ 本 SKU 的「格口号 → 本 SKU 计划下标」小表（64 字节/ SKU）：
    //   落格/查询路径据此 O(1) 定位，不再线性扫格口。
    //   注意：不能用"全表共享的格口号→单元下标"做这件事 —— 多个 SKU 会共用同一个
    //   格口（66 个格口 / 上万 SKU），共享表只能记住首个单元，对其它 SKU 都会落空。
    std::array<qint8, 67> gridOff{};   // -1 = 该格口不在本 SKU 计划内
};

// 运行期可变额度（与 m_cells 同下标）
struct CellQuota
{
    qint32 landed = 0;      // PLC 确认落格、已去重（唯一权威落格计数）
    qint32 reserv = 0;      // 在途认领（已下发 PLC、落格反馈未到）
    qint32 remain = 0;      // = planQty - landed - reserv（增量维护，可直接断言）
};

// 在途认领（只装"在途"；落格/释放/超时即 erase → 内存 O(在途)，与件数无关）
struct ClaimRef
{
    qint32  skuIdx  = -1;
    quint8  planIdx = 0;    // 指向 m_cells 的下标偏移（相对该 SKU idxFirst）
    quint32 seq     = 0;
    qint64  atMs    = 0;    // 认领时刻（超时清扫）
};

// 分配表统计（供日志与 UI 汇总）
struct PlanStats
{
    int skuCount    = 0;
    int cellCount   = 0;
    int planTotal   = 0;
    int landedTotal = 0;
    int reservTotal = 0;
    int remainTotal = 0;
    int inflightCnt = 0;
    int multiSkuCnt = 0;   // 计划格口数 > 1 的 SKU 数
};

// ════════════════════════════════════════════════════════════════════════════
// PlanAllocTable — 只做数据结构与纯计算；**不含互斥量**（由 HttpServer 统一加锁）
//   锁的粒度与"判定+改数"的原子性由唯一持有者掌控，避免两处锁交叉。
// ════════════════════════════════════════════════════════════════════════════
class PlanAllocTable
{
public:
    PlanAllocTable() {}

    // ──── 编译：波次开始时调用一次（主线程，持锁）────
    // 入参顺序无关（本函数内部按 SKU、再按格口号排序固化）
    // 返回 false → 调用方保持 valid=false，全系统退回改造前老逻辑
    bool build(int slotCount, const QVector<QPair<QString, QVector<PlanGridInput>>>& skuPlans)
    {
        clear();

        QVector<QPair<QString, QVector<PlanGridInput>>> sorted = skuPlans;
        std::sort(sorted.begin(), sorted.end(),
                  [](const QPair<QString, QVector<PlanGridInput>>& a,
                     const QPair<QString, QVector<PlanGridInput>>& b) { return a.first < b.first; });

        m_skus.reserve(sorted.size());
        for (const auto& kv : sorted)
        {
            const QString& sku = kv.first;
            if (sku.isEmpty()) continue;

            QVector<PlanGridInput> gs = kv.second;
            std::sort(gs.begin(), gs.end(),
                      [](const PlanGridInput& a, const PlanGridInput& b) { return a.grid < b.grid; });

            SkuPlan sp;
            sp.idxFirst = (qint16)m_cells.size();
            sp.gridOff.fill(-1);

            qint32 sum = 0;
            qint16 lastGrid = -1;
            for (const PlanGridInput& g : gs)
            {
                if (g.grid < 1 || g.grid > slotCount) continue;   // 越界格口不进表（不变量⑤）
                if (g.grid == lastGrid) continue;                 // 防御：重复格口只取一次
                if (g.qty <= 0) continue;                         // 计划 0 件 = 不参与分配
                lastGrid = g.grid;

                PlanCell c;
                c.grid     = g.grid;
                c.gridType = g.type;
                c.planQty  = g.qty;
                m_cells.append(c);

                CellQuota q;
                q.remain = g.qty;                                 // 初始：全部额度可用
                m_quota.append(q);

                sp.gridOff[(size_t)g.grid] = (qint8)(m_cells.size() - 1 - sp.idxFirst);
                sum += g.qty;
            }

            sp.idxCount  = (qint16)(m_cells.size() - sp.idxFirst);
            sp.planTotal = sum;
            sp.skuRemain = sum;
            sp.cursorFrom = 0;
            if (sp.idxCount <= 0) continue;   // 无有效计划格口 → 不入表（走老逻辑兜底）

            m_skuIndex.insert(sku, (qint32)m_skus.size());
            m_skuNames.append(sku);
            m_skus.append(sp);
            if (sp.idxCount > 1) ++m_multiSkuCnt;
        }

        m_claimSeq = 0;             // 格口号反查表已随每个 SkuPlan.gridOff 一起构建
        ++m_version;
        return !m_skus.isEmpty();
    }

    // ──── 清空：波次切出/新波次/完结/取消/恢复（主线程，持锁）────
    void clear()
    {
        m_skuIndex.clear();
        m_skuNames.clear();
        m_skus.clear();
        m_cells.clear();
        m_quota.clear();
        m_claims.clear();
        m_landedEpcs.clear();
        m_claimSeq    = 0;
        m_multiSkuCnt = 0;
        ++m_version;
    }

    bool isEmpty() const { return m_skus.isEmpty(); }
    int  version() const { return m_version.load(); }

    // ──── 查询（只读）────
    int skuCount()  const { return m_skus.size(); }
    int cellCount() const { return m_cells.size(); }
    int inflightCount() const { return m_claims.size(); }
    // 该 SKU 在表内的下标（-1 = 不在表内）；供认领登记等需要 skuIdx 的场景
    int skuIndexOf(const QString& sku) const { return m_skuIndex.value(sku, -1); }
    bool skuHasPlan(const QString& sku) const { return m_skuIndex.contains(sku); }
    QString skuNameAt(int si) const
    {
        return (si >= 0 && si < m_skuNames.size()) ? m_skuNames[si] : QString();
    }

    // 该 (SKU,格口) 计划件数；**不在计划内返回 0**（绝不返回 SKU 计划总数）
    int planQtyOf(const QString& sku, const QString& gridKey) const
    {
        int si = -1, pi = -1;
        return locate(sku, gridKey, si, pi) ? m_cells[cellOf(si, pi)].planQty : 0;
    }
    int landedOf(const QString& sku, const QString& gridKey) const
    {
        int si = -1, pi = -1;
        return locate(sku, gridKey, si, pi) ? m_quota[cellOf(si, pi)].landed : 0;
    }
    int remainOf(const QString& sku, const QString& gridKey) const
    {
        int si = -1, pi = -1;
        return locate(sku, gridKey, si, pi) ? m_quota[cellOf(si, pi)].remain : 0;
    }
    int reservOf(const QString& sku, const QString& gridKey) const
    {
        int si = -1, pi = -1;
        return locate(sku, gridKey, si, pi) ? m_quota[cellOf(si, pi)].reserv : 0;
    }
    bool inPlanOf(const QString& sku, const QString& gridKey) const
    {
        int si = -1, pi = -1;
        return locate(sku, gridKey, si, pi);
    }

    // 该 SKU 在某格口计划中的类型数字（0=正常分拣 1=异常 2=发货）；不在计划内返回 0
    int gridTypeOf(const QString& sku, const QString& gridKey) const
    {
        int si = -1, pi = -1;
        if (!locate(sku, gridKey, si, pi)) return 0;
        return (int)m_cells[cellOf(si, pi)].gridType;
    }

    int skuPlanTotal(const QString& sku) const
    {
        const int si = m_skuIndex.value(sku, -1);
        return si < 0 ? 0 : m_skus[si].planTotal;
    }
    int skuRemainOf(const QString& sku) const
    {
        const int si = m_skuIndex.value(sku, -1);
        return si < 0 ? 0 : m_skus[si].skuRemain;
    }
    int skuLandedOf(const QString& sku) const
    {
        const int si = m_skuIndex.value(sku, -1);
        if (si < 0) return 0;
        const SkuPlan& sp = m_skus[si];
        qint32 n = 0;
        for (qint16 k = 0; k < sp.idxCount; ++k) n += m_quota[sp.idxFirst + k].landed;
        return n;
    }

    // 该 SKU 的计划格口（升序）——供选格拼候选集、供报告输出
    QVector<qint16> gridsOf(const QString& sku) const
    {
        QVector<qint16> out;
        const int si = m_skuIndex.value(sku, -1);
        if (si < 0) return out;
        const SkuPlan& sp = m_skus[si];
        out.reserve(sp.idxCount);
        for (qint16 k = 0; k < sp.idxCount; ++k) out.append(m_cells[sp.idxFirst + k].grid);
        return out;
    }

    // 该 EPC 是否已在本 (SKU,格口) 落格（重投回原格口的放行依据）
    bool epcLandedIn(const QString& sku, const QString& gridKey, const QString& epc) const
    {
        if (epc.isEmpty()) return false;
        int si = -1, pi = -1;
        if (!locate(sku, gridKey, si, pi)) return false;
        return m_landedEpcs.contains(dedupKey(cellOf(si, pi), epc));
    }

    // ──── ① 认领（主线程，持锁）────
    // 返回 true = 认领成功（claimGrid/claimId 输出，供 PLC 下发与回传提交）
    // 返回 false = 该 SKU 各计划格口均已满额（已落+在途 = 计划）→ 调用方按超计划处置
    bool claim(const QString& sku, qint16* claimGrid, quint64* claimIdOut, qint16* planIdxOut = nullptr)
    {
        const int si = m_skuIndex.value(sku, -1);
        if (si < 0) return false;

        SkuPlan& sp = m_skus[si];
        if (sp.skuRemain <= 0) return false;        // ★ 全满：1 次整数比较即可判定

        qint16 start = sp.cursorFrom;               // 游标仅为加速提示
        if (start < 0 || start >= sp.idxCount) start = 0;

        int chosenPi = -1;
        // 两段扫描：游标→末尾，再 0→游标（覆盖被跳过而实际仍有额度的格口）
        for (qint16 pass = 0; pass < 2 && chosenPi < 0; ++pass)
        {
            const qint16 from = (pass == 0) ? start : 0;
            const qint16 to   = (pass == 0) ? sp.idxCount : start;
            for (qint16 k = from; k < to; ++k)
            {
                if (m_quota[sp.idxFirst + k].remain > 0) { chosenPi = k; sp.cursorFrom = k; break; }
            }
        }
        if (chosenPi < 0) { sp.skuRemain = 0; return false; }   // 防御：与 skuRemain 不一致

        applyClaim(si, (qint16)chosenPi);
        if (claimGrid)  *claimGrid  = m_cells[sp.idxFirst + chosenPi].grid;
        if (planIdxOut) *planIdxOut = (qint16)chosenPi;
        if (claimIdOut) *claimIdOut = makeClaimId(si);
        return true;
    }

    // ──── ② 登记在途认领的归属（选格成功、PLC 指令发出后立即调用）────
    // 幂等：同一 EPC 已有在途认领（重扫重推/重复下发）→ 保留原认领并置 conflict，
    //       绝不重复挂账（否则认领会泄漏，额度永远回不来）。
    // 返回 true = 新登记成功；false = 幂等命中或参数非法（conflict 标记原因）
    bool noteIssued(const QString& epc, int skuIdx, qint16 planIdx, quint64 claimId,
                    qint64 nowMs, bool* conflictOut = nullptr)
    {
        if (conflictOut) *conflictOut = false;
        if (epc.isEmpty() || claimId == 0 || skuIdx < 0 || skuIdx >= m_skus.size())
        {
            if (conflictOut) *conflictOut = true;
            return false;
        }
        auto old = m_claims.constFind(epc);
        if (old != m_claims.constEnd())
        {
            if (conflictOut) *conflictOut = true;   // 幂等命中：不重复挂账
            return false;
        }
        if (planIdx < 0 || planIdx >= m_skus[skuIdx].idxCount)
        {
            if (conflictOut) *conflictOut = true;
            return false;
        }

        ClaimRef ref;
        ref.skuIdx  = skuIdx;
        ref.planIdx = (quint8)planIdx;
        ref.seq     = (quint32)(claimId >> 32);
        ref.atMs    = nowMs;
        m_claims.insert(epc, ref);
        return true;
    }

    // ──── ③ 释放认领（发送失败 / 认领超时）— 只动在途，绝不触碰已落格 ────
    // 返回 false = 该 EPC 无在途认领（调用方记 WARN，不改其余计数）
    bool releaseEpc(const QString& epc)
    {
        if (epc.isEmpty()) return false;
        auto it = m_claims.find(epc);
        if (it == m_claims.end()) return false;
        const ClaimRef ref = it.value();
        m_claims.erase(it);
        applyRelease(ref.skuIdx, (qint16)ref.planIdx);
        return true;
    }
    bool releaseByClaimId(quint64 claimId)
    {
        if (claimId == 0) return false;
        for (auto it = m_claims.begin(); it != m_claims.end(); ++it)
        {
            if (((quint64)it.value().seq << 32) == (claimId & 0xFFFFFFFF00000000ull))
                return releaseEpc(it.key());
        }
        return false;
    }

    // ──── ④ 落格登记（唯一跨线程入口；调用方持锁）────
    // 返回 false = 该 (SKU,格口) 不在计划内（调用方走"落错格"分支）
    // 语义：landed 只增不减、同一 EPC 去重；有在途认领则一并释放。
    //       claimId 未知/不匹配（人工硬塞、或认领已被超时释放）→ **照实登记 landed**，
    //       并置 mismatch 供调用方留痕 —— 账实优先，绝不因认领异常而丢计数。
    bool commitOnLanded(const QString& sku, const QString& gridKey, const QString& epc,
                        quint64 claimId, bool* mismatchOut, int* landedNowOut, int* planQtyOut)
    {
        if (mismatchOut) *mismatchOut = false;
        if (landedNowOut) *landedNowOut = 0;
        if (planQtyOut)   *planQtyOut = 0;

        int si = -1, pi = -1;
        if (!locate(sku, gridKey, si, pi)) return false;

        const qint32 ci = cellOf(si, pi);
        if (planQtyOut) *planQtyOut = m_cells[ci].planQty;

        // ① 已落格去重 + 登记（★ 在途转已落：额度保持被占用，不归还）
        if (!epc.isEmpty())
        {
            const QString dk = dedupKey(ci, epc);
            if (!m_landedEpcs.contains(dk))
            {
                m_landedEpcs.insert(dk);
                if (!applyCommit(si, (qint16)pi))
                    applyCommitUnclaimed(si, (qint16)pi);   // 无在途认领 → 账实优先，只加已落
            }
            // 重复反馈：landed 不变（口径：同一 EPC 同波次同格口只算 1 件）
        }
        else
        {
            if (!applyCommit(si, (qint16)pi))
                applyCommitUnclaimed(si, (qint16)pi);
        }

        // ② 释放在途认领（额度已在 applyCommit 内从在途转为已落，此处不再归还）
        if (!epc.isEmpty())
        {
            auto it = m_claims.find(epc);
            if (it != m_claims.end())
            {
                const ClaimRef ref = it.value();
                m_claims.erase(it);
                if (mismatchOut && claimId != 0 &&
                    ref.skuIdx != (qint32)(claimId & 0xFFFF))
                    *mismatchOut = true;
            }
            else if (mismatchOut && claimId != 0)
            {
                *mismatchOut = true;      // 无在途认领（已超时释放 / 从未认领）
            }
        }

        if (landedNowOut) *landedNowOut = m_quota[ci].landed;
        return true;
    }

    // ──── ⑤ 缺口搬迁（计划格口不可用时把未完成额度转给同 SKU 别的计划格口）────
    // 保证"该 SKU 落进计划格口的总件数"不因换箱/锁格而流失；landed 不动。
    // 返回实际搬迁件数（0 = 无需搬迁或非法入参）
    int moveGap(const QString& sku, qint16 fromGrid, qint16 toGrid)
    {
        if (fromGrid == toGrid) return 0;
        int siF = -1, piF = -1, siT = -1, piT = -1;
        if (!locate(sku, gridKeyOf(fromGrid), siF, piF)) return 0;
        if (!locate(sku, gridKeyOf(toGrid),   siT, piT)) return 0;
        if (siF != siT) return 0;

        const qint32 ciF = cellOf(siF, piF);
        const qint32 ciT = cellOf(siT, piT);
        const qint32 gap = m_quota[ciF].remain;      // 未完成额度（在途已被扣掉）
        if (gap <= 0) return 0;

        m_cells[ciF].planQty -= gap;
        m_quota[ciF].remain   = 0;
        m_cells[ciT].planQty += gap;
        m_quota[ciT].remain  += gap;
        // planTotal（SKU 总数）与 skuRemain（SKU 总余量）都不变：只是额度换了个格口
        ++m_version;
        return (int)gap;
    }

    // ──── ⑥ 认领超时清扫（主线程，持锁；每 30s 一次）────
    // 返回被释放的 EPC（供日志留痕）。只清在途，不触碰已落格。
    QStringList sweepExpiredClaims(qint64 nowMs, qint64 timeoutMs)
    {
        QStringList released;
        if (m_claims.isEmpty()) return released;

        QVector<QString> dead;
        dead.reserve(m_claims.size());
        for (auto it = m_claims.constBegin(); it != m_claims.constEnd(); ++it)
        {
            const qint64 t = it.value().atMs;
            if (t > 0 && nowMs - t >= timeoutMs) dead.append(it.key());
        }
        for (const QString& epc : dead)
        {
            if (releaseEpc(epc)) released.append(epc);
        }
        return released;
    }

    // ──── ⑦ 不变量巡检（只读，不修改任何状态）────
    //   ① landed ≤ planQty   ② landed+reserv ≤ planQty 且 remain 自洽
    //   ③ 每 SKU Σ planQty == planTotal   ④ Σ remain == skuRemain
    // 返回违规描述（空 = 全部通过），最多 20 条防日志爆炸
    QStringList audit() const
    {
        QStringList bad;
        if (m_cells.size() != m_quota.size())
        {
            bad << QStringLiteral("表结构异常：cells=%1 quota=%2 长度不一致")
                       .arg(m_cells.size()).arg(m_quota.size());
            return bad;
        }
        for (int si = 0; si < m_skus.size() && bad.size() < 20; ++si)
        {
            const SkuPlan& sp = m_skus[si];
            qint32 sumPlan = 0, sumRemain = 0;
            for (qint16 k = 0; k < sp.idxCount; ++k)
            {
                const qint32 ci = sp.idxFirst + k;
                const CellQuota& q = m_quota[ci];
                const PlanCell&  c = m_cells[ci];
                sumPlan   += c.planQty;
                sumRemain += q.remain;
                if (q.landed > c.planQty)
                    bad << QStringLiteral("①SKU%1 格口%2 已落%3>计划%4")
                               .arg(skuNameAt(si)).arg(gridKeyOf(c.grid)).arg(q.landed).arg(c.planQty);
                if (q.landed + q.reserv > c.planQty)
                    bad << QStringLiteral("②SKU%1 格口%2 已落%3+在途%4>计划%5")
                               .arg(skuNameAt(si)).arg(gridKeyOf(c.grid))
                               .arg(q.landed).arg(q.reserv).arg(c.planQty);
                if (q.remain != c.planQty - q.landed - q.reserv)
                    bad << QStringLiteral("②SKU%1 格口%2 余量%3≠计划%4-已落%5-在途%6")
                               .arg(skuNameAt(si)).arg(gridKeyOf(c.grid)).arg(q.remain)
                               .arg(c.planQty).arg(q.landed).arg(q.reserv);
            }
            if (sumPlan != sp.planTotal)
                bad << QStringLiteral("③SKU%1 Σ计划%2≠总数%3")
                           .arg(skuNameAt(si)).arg(sumPlan).arg(sp.planTotal);
            if (sumRemain != sp.skuRemain)
                bad << QStringLiteral("④SKU%1 Σ余量%2≠SKU余量%3")
                           .arg(skuNameAt(si)).arg(sumRemain).arg(sp.skuRemain);
        }
        return bad;
    }

    // ──── 统计（O(单元)，仅波次报告/UI 刷新时调用）────
    PlanStats stats() const
    {
        PlanStats s;
        s.skuCount    = m_skus.size();
        s.cellCount   = m_cells.size();
        s.inflightCnt = m_claims.size();
        s.multiSkuCnt = m_multiSkuCnt;
        for (int i = 0; i < m_cells.size(); ++i)
        {
            s.planTotal   += m_cells[i].planQty;
            s.landedTotal += m_quota[i].landed;
            s.reservTotal += m_quota[i].reserv;
            s.remainTotal += m_quota[i].remain;
        }
        return s;
    }

    // ──── UI 只读快照（一次拷贝；主线程持锁调用）────
    struct UiCell
    {
        qint16 grid = 0; quint8 gridType = 0;
        qint32 planQty = 0, landed = 0, reserv = 0, remain = 0;
    };
    struct UiRow
    {
        QString sku;
        qint32  planTotal = 0, landedTotal = 0, reservTotal = 0, remainTotal = 0;
        QVector<UiCell> cells;     // 只含该 SKU 计划内格口（升序）
    };
    void snapshot(QVector<UiRow>* rows) const
    {
        if (!rows) return;
        rows->clear();
        rows->reserve(m_skus.size());
        for (int si = 0; si < m_skus.size(); ++si)
        {
            const SkuPlan& sp = m_skus[si];
            UiRow r;
            r.sku = skuNameAt(si);
            r.planTotal = sp.planTotal;
            r.cells.reserve(sp.idxCount);
            for (qint16 k = 0; k < sp.idxCount; ++k)
            {
                const qint32 ci = sp.idxFirst + k;
                UiCell c;
                c.grid     = m_cells[ci].grid;
                c.gridType = m_cells[ci].gridType;
                c.planQty  = m_cells[ci].planQty;
                c.landed   = m_quota[ci].landed;
                c.reserv   = m_quota[ci].reserv;
                c.remain   = m_quota[ci].remain;
                r.landedTotal += c.landed;
                r.reservTotal += c.reserv;
                r.remainTotal += c.remain;
                r.cells.append(c);
            }
            rows->append(r);
        }
    }

    // 全波次计划格口（升序去重）——供报告输出与 UI 列头
    QVector<qint16> allPlanGrids() const
    {
        QVector<qint16> out;
        for (const PlanCell& c : m_cells)
            if (!out.contains(c.grid)) out.append(c.grid);
        std::sort(out.begin(), out.end());
        return out;
    }

    // 某格口在**本波次任何 SKU 计划中**出现的类型名（仅供报告/UI 列头；无则返回空串）
    QString typeNameAtGrid(qint16 grid) const
    {
        for (const PlanCell& c : m_cells)
            if (c.grid == grid) return typeNameOf(c.gridType);
        return QString();
    }

    static QString gridKeyOf(qint16 grid) { return QString("%1").arg(grid, 3, 10, QChar('0')); }
    static QString typeNameOf(quint8 t)
    {
        if (t == 1) return QStringLiteral("异常");
        if (t == 2) return QStringLiteral("发货");
        return QStringLiteral("正常分拣");
    }

private:
    // 定位 (SKU,格口) → (skuIdx, planIdx)：走本 SKU 的 gridOff 小表，O(1)
    bool locate(const QString& sku, const QString& gridKey, int& siOut, int& piOut) const
    {
        const int si = m_skuIndex.value(sku, -1);
        if (si < 0) return false;
        bool ok = false;
        const int g = gridKey.trimmed().toInt(&ok);
        if (!ok || g < 1 || g >= (int)m_skus[si].gridOff.size()) return false;

        const qint8 off = m_skus[si].gridOff[(size_t)g];
        if (off < 0) return false;                       // 该格口不在本 SKU 计划内
        if (off >= m_skus[si].idxCount) return false;    // 防御：越界即视为不在计划内
        siOut = si;
        piOut = (int)off;
        return true;
    }

    qint32 cellOf(int si, int pi) const { return m_skus[si].idxFirst + pi; }

    // ──── 额度增量：**唯一写点**（集中审计，保证 remain 与 landed/reserv 恒自洽）────
    //   ★ 关键语义：remain = planQty - landed - reserv（未占用额度）
    //     · claim        ：reserv+1 → remain−1（占用额度）
    //     · commitOnLanded：landed+1 且 reserv−1 → **remain 不变**（在途转已落，额度已被占用）
    //       ★ 若在 commit 里也把 remain 重算为 plan−(landed+1)−(reserv−1) 之外的加法，
    //         或者把"释放认领"当成"归还额度"，就会把已经用掉的额度又还回去
    //         → 同一格口会被反复选满，超计划失守（离线仿真已复现该缺陷，故在此固化写法）。
    //     · release      ：reserv−1 → remain+1（真正归还，下发失败/认领超时）
    void applyClaim(int si, qint16 pi)
    {
        const qint32 ci = m_skus[si].idxFirst + pi;
        ++m_quota[ci].reserv;
        --m_quota[ci].remain;
        --m_skus[si].skuRemain;
        ++m_version;
    }

    // 在途认领转为已落格（额度保持被占用；同 EPC 去重由调用方保证）
    // 返回 false = 该单元当前没有在途认领（认领已被超时释放，或本件是人工硬塞）——
    //   此时不做"在途→已落"的转换，改由调用方走 applyCommitUnclaimed（只加已落）。
    bool applyCommit(int si, qint16 pi)
    {
        const qint32 ci = m_skus[si].idxFirst + pi;
        if (m_quota[ci].reserv <= 0) return false;      // 无在途可转 → 由调用方决定记哪种
        ++m_quota[ci].landed;
        --m_quota[ci].reserv;                           // 在途转已落（remain/skuRemain 不变）
        ++m_version;
        return true;
    }

    // 无认领落格（人工硬塞 / 认领已超时释放）：只加已落，余量按 plan−landed−reserv 重算
    //   —— 这是"账实优先"路径：即使没有在途认领，落格也必须照实登记；此时
    //      landed 会超过"计划−在途"，余量可能变负（不变量①会据此告警并留痕）。
    void applyCommitUnclaimed(int si, qint16 pi)
    {
        const qint32 ci = m_skus[si].idxFirst + pi;
        ++m_quota[ci].landed;
        m_quota[ci].remain = m_cells[ci].planQty - m_quota[ci].landed - m_quota[ci].reserv;
        const SkuPlan& sp = m_skus[si];
        qint32 sum = 0;
        for (qint16 k = 0; k < sp.idxCount; ++k) sum += m_quota[sp.idxFirst + k].remain;
        m_skus[si].skuRemain = sum;
        ++m_version;
    }

    // 归还额度（仅"下发失败/认领超时"这类未落格场景调用）
    void applyRelease(int si, qint16 pi)
    {
        const qint32 ci = m_skus[si].idxFirst + pi;
        if (m_quota[ci].reserv > 0) --m_quota[ci].reserv;
        ++m_quota[ci].remain;
        ++m_skus[si].skuRemain;
        ++m_version;
    }

    // 兜底自愈：把 remain 重算为 plan−landed−reserv（仅在巡检发现不自洽时调用）
    void recomputeRemain(int si)
    {
        const SkuPlan& sp = m_skus[si];
        qint32 sum = 0;
        for (qint16 k = 0; k < sp.idxCount; ++k)
        {
            const qint32 ci = sp.idxFirst + k;
            m_quota[ci].remain = m_cells[ci].planQty - m_quota[ci].landed - m_quota[ci].reserv;
            sum += m_quota[ci].remain;
        }
        m_skus[si].skuRemain = sum;
    }

    quint64 makeClaimId(int skuIdx) const
    {
        return ((quint64)(++m_claimSeq) << 32) | (quint64)((quint16)(skuIdx & 0xFFFF));
    }

    static QString dedupKey(qint32 cellIdx, const QString& epc)
    {
        return QString::number(cellIdx) + QLatin1Char('\n') + epc;
    }

    // ──── 数据成员 ────
    QHash<QString, qint32>    m_skuIndex;     // SKU → m_skus 下标
    QStringList               m_skuNames;     // 下标 → SKU（避免反查哈希）
    QVector<SkuPlan>          m_skus;
    QVector<PlanCell>         m_cells;        // 按 SKU 连续排布
    QVector<CellQuota>        m_quota;        // 与 m_cells 同下标

    QHash<QString, ClaimRef>  m_claims;       // 在途：EPC → 认领（只装在途）
    QSet<QString>             m_landedEpcs;   // "单元下标\nEPC" → 已落格（去重，不变量①）

    mutable quint32 m_claimSeq = 0;
    int             m_multiSkuCnt = 0;
    mutable std::atomic<int> m_version{0};
};
