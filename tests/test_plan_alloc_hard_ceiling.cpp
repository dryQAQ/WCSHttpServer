// ============================================================================
// test_plan_alloc_hard_ceiling.cpp — ★ 2026-09-21 硬上限（可执行自测）
//   ★ 2026-09-26 更新：搬迁能力已**整体删除**（不是停用），并新增单元级硬上限 ⑧。
//
// 客户口径（2026-09-26 定稿）：**某个 SKU 在某个格口计划多少件，就只能分多少件（严格不大于）**；
//   额度的唯一单元 = **(SKU, 格口, 分拣类型)**，分类(0)与发货(2)各自独立、互不借用、**不可搬迁**。
//
// 现场问题（波次 PP202600000631，SKU 105301083212803）：
//   H4 计划 034(发货)=2 件 + 040(分类)=176 件，但"缺口搬迁"把 034 的**当前计划**抬到 83 件
//   （净搬入 81 件）→ 实拣 83 件，现场在查询页看到"计划 2 / 分拣记录 83"，且是跨属性搬迁。
//   ⇒ 整改：搬迁的**调用链整体删除**（PlanAllocTable::moveGap / HttpServer::moveAllocGap /
//     PlcManager 的 PlcMoveGapCallback 三处都不存在了），额度只由编译期写入。
//
// 本测试锁定整改后的硬约束（PlanAllocTable 纯逻辑，不依赖 Qt Widgets / 不查库）：
//   ① `planQtyH4` 为**不可变快照**：任何路径都不能抬高某单元上限（含"多轮认领后计划分毫未动"）；
//   ② `audit()` ⑤⑥⑦⑧：planQty==planQtyH4 / landed≤planQtyH4 / 每类型 Σlanded≤ΣplanQtyH4 /
//      每单元 已落+在途 ≤ planQtyH4（跨单元借用额度当场暴露）；
//   ③ 属性隔离：分类与发货各自封顶——发货路满额后，件不会"借用"分类路的额度，反之亦然；
//   ④ 源码契约：搬迁调用链不存在（连"返回 0 的空实现"都不留）。
//   ★ 2026-09-26 追加（业务确认：同一 (SKU,格口) 可以存在多种 grid_type）：
//   ⑦ 迟到反馈按「归属存根」记回原认领单元（认领被 30s 清扫后反馈才到，不得串到另一属性）；
//   ⑧ 恢复波次按单元编译（inputsFromCells：两行单元 → 2 个单元，旧数据退回格口级）；
//   ⑨ 恢复逐件归属：gridTypeHint 精确归属 / 无提示时 unitGuessed 留痕 / 释放不落存根 / clear 清存根。
//
// 构建：见 tests\run_tests.bat 第 [11/11] 步
// ============================================================================

#include <QCoreApplication>
#include <QFile>
#include <QString>
#include <QMap>
#include <QDateTime>
#include <cstdio>

#include "PlanAllocTable.h"
#include "define.h"          // BINDING_SLOT_COUNT = 66

static int g_pass = 0;
static int g_fail = 0;

static void check(bool ok, const QString& what, const QString& got = QString())
{
    if (ok) { ++g_pass; std::printf("  [PASS] %s\n", what.toUtf8().constData()); }
    else
    {
        ++g_fail;
        std::printf("  [FAIL] %s%s\n", what.toUtf8().constData(),
                    got.isEmpty() ? "" : ("  <- 实际: " + got).toUtf8().constData());
    }
}

// 现场案例：SKU → 034(发货,类型2) 计划 2 件 + 040(分类,类型0) 计划 176 件
static const char* kSku    = "105301083212803";
static const qint16 kG34   = 34;    // 发货
static const qint16 kG40   = 40;    // 分类
static const qint32 kPlan34 = 2;
static const qint32 kPlan40 = 176;

static bool buildPlan(PlanAllocTable& t)
{
    QVector<PlanGridInput> gs;
    { PlanGridInput c; c.grid = kG34; c.type = 2; c.qty = kPlan34; gs.append(c); }
    { PlanGridInput c; c.grid = kG40; c.type = 0; c.qty = kPlan40; gs.append(c); }
    QVector<QPair<QString, QVector<PlanGridInput>>> skuPlans;
    skuPlans.append(qMakePair(QString(kSku), gs));
    return t.build(BINDING_SLOT_COUNT, skuPlans);
}

static int claimOnce(PlanAllocTable& t, const PlanAllocTable::GridMask* mask, bool* okOut = nullptr)
{
    qint16 grid = -1, idx = -1;
    quint64 id = 0;
    const bool ok = t.claim(QString(kSku), &grid, &id, &idx, mask);
    if (okOut) *okOut = ok;
    return ok ? (int)grid : -1;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const QString sku(kSku);

    // ════════════════════════════════════════════════════════════════════
    std::printf("== ① 编译期快照不可变：搬迁能力已从代码中删除，额度抬不起来 ==\n");
    {
        PlanAllocTable t;
        check(buildPlan(t), QString("编译成功：034(发货)=2 件 + 040(分类)=176 件"));

        check(t.planQtyH4Of(sku, "034") == kPlan34 && t.planQtyH4Of(sku, "040") == kPlan40,
              QString("H4 计划快照 = 034:%1 / 040:%2").arg(kPlan34).arg(kPlan40),
              QString("034=%1 040=%2").arg(t.planQtyH4Of(sku, "034")).arg(t.planQtyH4Of(sku, "040")));

        // ★ 2026-09-26：现场事故入口 `moveGap`（把 034 未完成额度搬到 040）**已从代码中整体删除**
        //   —— 不是"返回 0 的空实现"，而是连调用点都不存在（见下方源码契约断言）。
        //   这里改为：多轮认领后**任何单元的计划都不被改动**（额度不搬迁），且账目自洽。
        for (int i = 0; i < 50; ++i) claimOnce(t, nullptr);
        check(t.planQtyOf(sku, "034") == kPlan34 && t.planQtyOf(sku, "040") == kPlan40,
              QString("50 次认领后计划分毫未动（034=%1 / 040=%2）")
                  .arg(t.planQtyOf(sku, "034")).arg(t.planQtyOf(sku, "040")),
              QString("034=%1 040=%2").arg(t.planQtyOf(sku, "034")).arg(t.planQtyOf(sku, "040")));
        check(t.planQtyOf(sku, "034") == t.planQtyH4Of(sku, "034")
                  && t.planQtyOf(sku, "040") == t.planQtyH4Of(sku, "040"),
              QString("不变量⑤：每单元 当前计划 == H4 计划（额度不可被搬动/抬高）"));
        check(t.audit().isEmpty(), QString("不变量巡检通过（①~⑧）"), t.audit().join(" / "));
    }

    // ════════════════════════════════════════════════════════════════════
    std::printf("== ①b 源码契约：搬迁调用链已整体删除（编译期不可达）==\n");
    {
        QFile fh("../WCS_httpServer/PlanAllocTable.h");
        QFile fp("../WCS_httpServer/PlcManager.cpp");
        QFile fs("../WCS_httpServer/HttpServer.cpp");
        QFile fsh("../WCS_httpServer/HttpServer.h");
        QString th, tp, ts, tsh;
        if (fh.open(QIODevice::ReadOnly)) th  = QString::fromUtf8(fh.readAll());
        if (fp.open(QIODevice::ReadOnly)) tp  = QString::fromUtf8(fp.readAll());
        if (fs.open(QIODevice::ReadOnly)) ts  = QString::fromUtf8(fs.readAll());
        if (fsh.open(QIODevice::ReadOnly)) tsh = QString::fromUtf8(fsh.readAll());
        check(!th.isEmpty() && !tp.isEmpty() && !ts.isEmpty(), "产品源码可读（契约断言前置）");
        check(!th.contains("int moveGap(") && !tp.contains("m_moveGapCb") && !tp.contains("moveGapCb("),
              "搬迁能力已删除：PlanAllocTable::moveGap / PlcManager 搬迁回调均不存在");
        check(!ts.contains("int HttpServer::moveAllocGap(const QString& sku")
                  && !tsh.contains("int  moveAllocGap(const QString& sku")
                  && !ts.contains("m_alloc.moveGap(") && !tp.contains("m_moveGapCb("),
              "HttpServer 的搬迁实现与搬迁调用已删除（函数签名/调用点均不存在；现场再见 [计划搬迁] 即为旧版）");
        check(th.contains("landed + q.reserv > c.planQtyH4") || th.contains("q.landed + q.reserv > c.planQtyH4"),
              "巡检新增单元级硬上限 ⑧：已落+在途 ≤ H4 计划（含同格口另一类型）");
    }

    // ════════════════════════════════════════════════════════════════════
    std::printf("== ② 认领严格不超过各自格口的计划件数（发货 2 件 / 分类 176 件）==\n");
    {
        PlanAllocTable t;
        buildPlan(t);

        int n34 = 0, n40 = 0;
        for (int i = 0; i < 200; ++i)
        {
            const int g = claimOnce(t, nullptr);
            if (g == (int)kG34)      ++n34;
            else if (g == (int)kG40) ++n40;
            else                     break;          // 全部满额
        }
        check(n34 == kPlan34, QString("034(发货) 认领次数 = 计划 %1 件（实际 %2）").arg(kPlan34).arg(n34),
              QString::number(n34));
        check(n40 == kPlan40, QString("040(分类) 认领次数 = 计划 %1 件（实际 %2）").arg(kPlan40).arg(n40),
              QString::number(n40));
        check(t.remainOf(sku, "034") == 0 && t.remainOf(sku, "040") == 0,
              QString("两格口余量归零（额度恰好用尽，未超发）"));
        check(t.audit().isEmpty(), QString("不变量巡检通过"), t.audit().join(" / "));
    }

    // ════════════════════════════════════════════════════════════════════
    std::printf("== ③ 属性隔离：发货路满额后不得借用分类路额度（反之亦然）==\n");
    {
        PlanAllocTable t;
        buildPlan(t);

        // 前 2 件：认领 034(发货) → 模拟 PLC 确认落格（commitOnLanded 才会推进 landed 计数）
        int ok34 = 0;
        for (int i = 0; i < kPlan34; ++i)
        {
            if (claimOnce(t, nullptr) != (int)kG34) break;
            int landedNow = 0, planQty = 0; bool mismatch = false;
            t.commitOnLanded(sku, "034", QString("EPC34-%1").arg(i), 0, &mismatch, &landedNow, &planQty);
            ++ok34;
        }
        check(ok34 == kPlan34, QString("发货口 034 认领并落格 %1 件（= 计划）").arg(kPlan34),
              QString::number(ok34));
        check(t.landedSumOfType(sku, 2) == kPlan34 && t.planQtyH4SumOfType(sku, 2) == kPlan34,
              QString("发货属性：已落合计 = 计划合计 = %1 件（不得超出）").arg(kPlan34),
              QString("已落=%1 计划=%2").arg(t.landedSumOfType(sku, 2)).arg(t.planQtyH4SumOfType(sku, 2)));

        // 发货路已满 → 下一件仍按计划落分类口 040，且**不占用发货额度**
        const int g = claimOnce(t, nullptr);
        check(g == (int)kG40, QString("发货路满额后，下一件落分类口 040（实际 %1）").arg(g));
        {
            int landedNow = 0, planQty = 0; bool mismatch = false;
            t.commitOnLanded(sku, "040", "EPC40-1", 0, &mismatch, &landedNow, &planQty);
        }
        check(t.landedSumOfType(sku, 0) == 1 && t.planQtyH4SumOfType(sku, 0) == kPlan40,
              QString("分类属性：已落 1 / 计划 %1（两路各自独立计数）").arg(kPlan40),
              QString("已落=%1 计划=%2").arg(t.landedSumOfType(sku, 0)).arg(t.planQtyH4SumOfType(sku, 0)));
        check(t.landedSumOfType(sku, 2) == kPlan34,
              QString("发货属性计数未被分类口的落格影响（仍为 %1）").arg(kPlan34),
              QString::number(t.landedSumOfType(sku, 2)));
        check(t.landedSumOfType(sku, 2) <= t.planQtyH4SumOfType(sku, 2) &&
              t.landedSumOfType(sku, 0) <= t.planQtyH4SumOfType(sku, 0),
              QString("两条属性红线均成立（Σ已落 ≤ Σ计划）"));
        check(t.audit().isEmpty(), QString("不变量巡检通过（⑦ 跨属性借用额度未发生）"), t.audit().join(" / "));
    }

    // ════════════════════════════════════════════════════════════════════
    std::printf("== ④ 巡检能抓出违规（人为构造「超计划落格」与「跨属性借用」）==\n");
    {
        PlanAllocTable t;
        buildPlan(t);
        check(t.audit().isEmpty(), QString("基线：新表巡检通过"));

        // 人为把 200 件塞进 034（模拟 PLC 偏投/人工硬塞：commitOnLanded 照实登记，账实优先）
        for (int i = 0; i < 200; ++i)
        {
            int landedNow = 0, planQty = 0;
            bool mismatch = false;
            t.commitOnLanded(sku, "034", QString("EPC%1").arg(i, 6, 10, QChar('0')), 0,
                             &mismatch, &landedNow, &planQty);
        }
        const QStringList bad = t.audit();
        const bool hasCeil  = bad.filter(QString::fromUtf8("⑥")).size() > 0;
        const bool hasType  = bad.filter(QString::fromUtf8("⑦")).size() > 0;
        check(!bad.isEmpty(), QString("巡检报出违规 %1 条").arg(bad.size()));
        check(hasCeil, QString::fromUtf8("⑥ 抓到「格口实落 > H4 计划」（发货口 034 计划 2 件）"),
              bad.join(" / "));
        check(hasType, QString::fromUtf8("⑦ 抓到「分类/发货属性 Σ实落 > Σ计划」（跨属性借用额度）"),
              bad.join(" / "));
    }

    // ════════════════════════════════════════════════════════════════════
    std::printf("== ⑤ 掩码仍生效（不可下发格口额度原样保留，零回归）==\n");
    {
        PlanAllocTable t;
        buildPlan(t);

        PlanAllocTable::GridMask mask{};        // 034 不可下发（未绑定容器/满箱未重绑）
        mask[(size_t)kG34] = true;
        int n40 = 0, nOther = 0;
        for (int i = 0; i < kPlan40 + 5; ++i)
        {
            const int g = claimOnce(t, &mask);
            if (g == (int)kG40)      ++n40;
            else if (g != -1)        ++nOther;
            else                     break;              // 全部无可分配额度
        }
        check(n40 == kPlan40 && nOther == 0,
              QString("掩码生效：%1 件全部由 040 承接（无一件落到被掩码的 034）").arg(kPlan40),
              QString("040=%1 其它=%2").arg(n40).arg(nOther));

        bool ok = true;
        const int g = claimOnce(t, &mask, &ok);
        check(!ok && g == -1, QString("040 用尽且 034 被掩码 → 认领失败（由上层改投异常口）"));
        check(t.remainOf(sku, "034") == kPlan34,
              QString("被掩码的 034 额度原样保留 %1 件").arg(kPlan34),
              QString::number(t.remainOf(sku, "034")));
        check(t.audit().isEmpty(), QString("不变量巡检通过（掩码不破坏任何不变量）"), t.audit().join(" / "));
    }

    // ════════════════════════════════════════════════════════════════════
    std::printf("== ⑥ ★ 2026-09-26 单元口径：(SKU,格口,类型) 同格口两类型各自保额、互不借用 ==\n");
    {
        // 现场口径：同一 (SKU,格口) 可能有两行（分类一份 + 发货一份）—— 必须分别保额。
        // 旧实现按格口去重（`grid==lastGrid → continue`）会把第二行**直接丢掉**，上限被放大。
        PlanAllocTable t;
        QVector<PlanGridInput> gs;
        { PlanGridInput c; c.grid = kG34; c.type = 0; c.qty = 2; gs.append(c); }   // 034|分类 = 2 件
        { PlanGridInput c; c.grid = kG34; c.type = 2; c.qty = 3; gs.append(c); }   // 034|发货 = 3 件
        QVector<QPair<QString, QVector<PlanGridInput>>> plans;
        plans.append(qMakePair(sku, gs));
        check(t.build(BINDING_SLOT_COUNT, plans), QString("编译成功：034|分类=2 + 034|发货=3（同格口两单元）"));
        check(t.cellCount() == 2, QString("两个单元都进表（不再按格口去重丢行）"),
              QString("cellCount=%1").arg(t.cellCount()));
        check(t.planQtyH4OfType(sku, "034", 0) == 2 && t.planQtyH4OfType(sku, "034", 2) == 3,
              QString("单元上限 = 034|分类:2 / 034|发货:3"),
              QString("分类=%1 发货=%2").arg(t.planQtyH4OfType(sku, "034", 0)).arg(t.planQtyH4OfType(sku, "034", 2)));
        check(t.planQtyH4Of(sku, "034") == 5, QString("(SKU,格口) 级视图 = 两单元之和 5（派生视图，供报表/裁剪）"),
              QString::number(t.planQtyH4Of(sku, "034")));

        // 认领 5 次：分类单元最多 2 次、发货单元最多 3 次，互不借用
        int nType0 = 0, nType2 = 0, nFail = 0;
        for (int i = 0; i < 8; ++i)
        {
            qint16 g = -1, idx = -1; quint64 id = 0; quint8 tp = 0;
            if (t.claim(sku, &g, &id, &idx, nullptr, &tp)) { if (tp == 0) ++nType0; else if (tp == 2) ++nType2; }
            else ++nFail;
        }
        check(nType0 == 2 && nType2 == 3, QString("认领严格按单元封顶：分类 2 件 + 发货 3 件（互不借用）"),
              QString("分类=%1 发货=%2").arg(nType0).arg(nType2));
        check(nFail == 3, QString("额度用尽后认领失败（不会借用另一类型/另一单元的额度）"),
              QString("fail=%1").arg(nFail));
        check(t.audit().isEmpty(), QString("不变量巡检通过（⑤⑥⑦⑧ 全过）"), t.audit().join(" / "));

        // ★ 落格记账也必须落在**认领的那个单元**上（原实现按 (SKU,格口) 记到首个单元 → 类型账错位）
        PlanAllocTable t2;
        t2.build(BINDING_SLOT_COUNT, plans);
        qint16 g = -1, idx = -1; quint64 cid = 0; quint8 tp = 0;
        check(t2.claim(sku, &g, &cid, &idx, nullptr, &tp) && tp == 0,
              QString("第 1 件认领到 034|分类 单元（tp=%1）").arg(tp));
        bool mm = true; int landedNow = 0, planQty = 0;
        t2.commitOnLanded(sku, "034", "EPC-LAND-1", cid, &mm, &landedNow, &planQty);
        check(t2.landedOfType(sku, "034", 0) == 1 && t2.landedOfType(sku, "034", 2) == 0,
              QString("落格记到「034|分类」单元（分类=1 / 发货=0）"),
              QString("分类=%1 发货=%2").arg(t2.landedOfType(sku, "034", 0)).arg(t2.landedOfType(sku, "034", 2)));
        check(t2.landedOf(sku, "034") == 1, QString("格口级已落 = 各单元之和 1"),
              QString::number(t2.landedOf(sku, "034")));
        check(t2.audit().isEmpty(), QString("落格后巡检仍通过（单元账自洽）"), t2.audit().join(" / "));
    }

    // ════════════════════════════════════════════════════════════════════
    std::printf("== ⑦ ★ 2026-09-26 迟到反馈按「归属存根」记回原单元（缺口 4 回归）==\n");
    {
        // 现场前提（业务确认）：**同一 (SKU,格口) 可以存在多种 grid_type**
        //   序列（与探针 A4 同形）：034|分类=1 + 034|发货=3
        //     EPC-A 认领"分类" → 反馈未到、30s 清扫释放额度（同时落归属存根）
        //     EPC-B 又认领"分类"并落格 → 分类 1/1 满
        //     EPC-A 的迟到反馈此刻才到 ⇒ 必须仍记在**分类**（原认领单元），不得串到"发货"
        PlanAllocTable t;
        QVector<PlanGridInput> gs;
        { PlanGridInput c; c.grid = kG34; c.type = 0; c.qty = 1; gs.append(c); }   // 034|分类 = 1 件
        { PlanGridInput c; c.grid = kG34; c.type = 2; c.qty = 3; gs.append(c); }   // 034|发货 = 3 件
        QVector<QPair<QString, QVector<PlanGridInput>>> plans;
        plans.append(qMakePair(sku, gs));
        check(t.build(BINDING_SLOT_COUNT, plans), "编译成功：034|分类=1 + 034|发货=3（同格口两单元）");

        const qint64 t0 = QDateTime::currentMSecsSinceEpoch();
        qint16 gA = -1, idxA = -1; quint64 idA = 0; quint8 tpA = 0;
        check(t.claim(sku, &gA, &idA, &idxA, nullptr, &tpA) && tpA == 0,
              QString("第 1 件 EPC-A 认领到 034|分类（tp=%1）").arg((int)tpA));
        t.noteIssued("EPC-A", t.skuIndexOf(sku), idxA, idA, t0, nullptr);

        const QStringList swept = t.sweepExpiredClaims(t0 + 40000, 30000);
        check(swept.contains("EPC-A"), "30s 无落格反馈 → 认领被清扫释放（额度归还，件仍可重投）");
        check(t.orphanCount() == 1, QString("释放时留下归属存根（存根数=%1，旧实现此处丢归属）")
                                        .arg(t.orphanCount()));
        check(t.remainOfType(sku, "034", 0) == 1, "分类额度已归还（余量回到 1）");

        qint16 gB = -1, idxB = -1; quint64 idB = 0; quint8 tpB = 0;
        check(t.claim(sku, &gB, &idB, &idxB, nullptr, &tpB) && tpB == 0,
              QString("第 2 件 EPC-B 又认领到 034|分类（tp=%1）").arg((int)tpB));
        t.noteIssued("EPC-B", t.skuIndexOf(sku), idxB, idB, t0 + 41000, nullptr);
        bool mm = false; int landedNow = 0, planQty = 0;
        t.commitOnLanded(sku, "034", "EPC-B", idB, &mm, &landedNow, &planQty);
        check(t.landedOfType(sku, "034", 0) == 1 && t.landedOfType(sku, "034", 2) == 0,
              "EPC-B 记入 034|分类（分类=1 发货=0）");

        bool lateStub = false, guessed = false; quint8 landedType = 0xFF;
        t.commitOnLanded(sku, "034", "EPC-A", idA, &mm, &landedNow, &planQty,
                         &lateStub, &guessed, 0xFF, &landedType);
        check(lateStub, "EPC-A 的迟到反馈命中归属存根（lateStubUsed=true）");
        check(!guessed, "未退化为启发式（unitGuessed=false）");
        check(landedType == 0, QString("记账单元类型 = 分类（实际 %1）").arg((int)landedType));
        check(t.landedOfType(sku, "034", 0) == 2 && t.landedOfType(sku, "034", 2) == 0,
              QString("★ 记回原单元、未串属性：分类=2 / 发货=0（实际 分类=%1 发货=%2）")
                  .arg(t.landedOfType(sku, "034", 0)).arg(t.landedOfType(sku, "034", 2)));
        check(t.landedOf(sku, "034") == 2, "格口级已落 = 2（与箱内件数一致）");
        check(t.orphanCount() == 0, "存根已被消费（不残留、不无界增长）");
        {
            const QStringList bad = t.audit();
            check(bad.filter(QString::fromUtf8("⑥")).size() > 0 &&
                  bad.filter(QString::fromUtf8("⑦")).size() > 0,
                  QString("巡检如实报出 ⑥/⑦（分类单元实落 2 > H4 计划 1）—— 账实优先，不掩盖"),
                  bad.join(" / "));
        }
        t.commitOnLanded(sku, "034", "EPC-A", idA, &mm, &landedNow, &planQty,
                         &lateStub, &guessed, 0xFF, &landedType);
        check(t.landedOfType(sku, "034", 0) == 2, "同一 EPC 重复反馈不重复计数（仍为 2）");
    }

    // ════════════════════════════════════════════════════════════════════
    std::printf("== ⑧ ★ 恢复波次按单元编译（inputsFromCells，缺口 2 回归）==\n");
    {
        // 恢复路径（切回/断电）此前只读格口级 planQtyPerGrid ⇒ 同格口两类型被合并成一个单元。
        QMap<QString, int> cells;      cells.insert(QStringLiteral("034|0"), 2);
        cells.insert(QStringLiteral("034|2"), 3);
        QMap<QString, int> grids;      grids.insert(QStringLiteral("034"), 5);   // 格口级合计（派生视图）
        QMap<QString, QString> gtypes; gtypes.insert(QStringLiteral("034"), QStringLiteral("2"));

        const QVector<PlanGridInput> gs = PlanAllocTable::inputsFromCells(cells, grids, gtypes, QStringLiteral("0"));
        check(gs.size() == 2, QString("两行单元 → 2 个编译输入（合并成 1 个即回归；实际 %1）").arg(gs.size()));

        PlanAllocTable t;
        QVector<QPair<QString, QVector<PlanGridInput>>> plans;
        plans.append(qMakePair(sku, gs));
        const bool built = t.build(BINDING_SLOT_COUNT, plans);   // 先编译再断言（避免实参求值顺序把 cellCount 打印成 0）
        check(built && t.cellCount() == 2,
              QString("编译后单元数 = 2（实际 %1）").arg(t.cellCount()));
        check(t.planQtyH4OfType(sku, "034", 0) == 2 && t.planQtyH4OfType(sku, "034", 2) == 3,
              QString("恢复后 034|分类=2 / 034|发货=3 各自保额（属性隔离成立）"),
              QString("分类=%1 发货=%2").arg(t.planQtyH4OfType(sku, "034", 0)).arg(t.planQtyH4OfType(sku, "034", 2)));
        check(t.planQtyH4Of(sku, "034") == 5, "格口级视图 = 两单元之和 5（总量封顶不受影响）");

        // 旧数据兜底：无单元表 → 退回格口级（与改造前逐字一致）
        const QVector<PlanGridInput> gs2 =
            PlanAllocTable::inputsFromCells(QMap<QString, int>(), grids, gtypes, QStringLiteral("0"));
        check(gs2.size() == 1 && gs2[0].grid == 34 && gs2[0].qty == 5 && gs2[0].type == 2,
              "旧数据（无 planQtyPerCell）→ 退回格口级编译，类型取该格口类型");
    }

    // ════════════════════════════════════════════════════════════════════
    std::printf("== ⑨ ★ 恢复逐件归属：gridTypeHint 精确归属 / 无提示留痕 / 释放不落存根 ==\n");
    {
        PlanAllocTable t;
        QVector<PlanGridInput> gs;
        { PlanGridInput c; c.grid = kG34; c.type = 0; c.qty = 2; gs.append(c); }   // 034|分类 = 2
        { PlanGridInput c; c.grid = kG34; c.type = 2; c.qty = 3; gs.append(c); }   // 034|发货 = 3
        QVector<QPair<QString, QVector<PlanGridInput>>> plans;
        plans.append(qMakePair(sku, gs));
        t.build(BINDING_SLOT_COUNT, plans);

        bool mm = false; int landedNow = 0, planQty = 0;
        bool lateStub = false, guessed = false; quint8 landedType = 0xFF;
        // ① 带类型提示（切回重建：落格明细里持久化了 grid_type=发货）
        t.commitOnLanded(sku, "034", "EPC-R1", 0, &mm, &landedNow, &planQty,
                         &lateStub, &guessed, /*gridTypeHint=*/2, &landedType);
        check(landedType == 2 && t.landedOfType(sku, "034", 2) == 1 && t.landedOfType(sku, "034", 0) == 0,
              QString("按提示精确归到 034|发货（发货=%1 分类=%2）")
                  .arg(t.landedOfType(sku, "034", 2)).arg(t.landedOfType(sku, "034", 0)));
        check(!guessed, "带提示时不算「属性归属未知」");
        // ② 无提示（旧数据无 grid_type）→ 启发式记账 + 留痕
        t.commitOnLanded(sku, "034", "EPC-R2", 0, &mm, &landedNow, &planQty,
                         &lateStub, &guessed, 0xFF, &landedType);
        check(guessed, "无提示且该格口多单元 ⇒ unitGuessed=true（调用方必须留痕）");
        check(!lateStub, "无在途认领且无存根时不得命中「迟到存根」路径");
        check(t.audit().isEmpty(), QString("表内账自洽（巡检 ①~⑧ 全过）"), t.audit().join(" / "));
        // ③ 释放但不落存根（调用方已确认该件没进计划格口：落 66 / PLC 无格口 / 落错格）
        qint16 g = -1, idx = -1; quint64 id = 0; quint8 tp = 0;
        check(t.claim(sku, &g, &id, &idx, nullptr, &tp), "再认领一件用于释放测试");
        t.noteIssued("EPC-X", t.skuIndexOf(sku), idx, id, QDateTime::currentMSecsSinceEpoch(), nullptr);
        t.releaseEpc("EPC-X", QDateTime::currentMSecsSinceEpoch(), /*stashAttribution=*/false);
        check(t.orphanCount() == 0, "stashAttribution=false ⇒ 不留存根（避免把后续无据反馈记错属性）");
        // ④ 波次清空：存根随波次清空（不得跨波次归属）
        t.clear();
        check(t.orphanCount() == 0 && t.cellCount() == 0, "clear() 清空存根与计划单元（波次隔离）");
    }

    std::printf("\n==== 结果：通过 %d / 失败 %d ====\n", g_pass, g_fail);
    if (g_fail == 0) std::printf("[OK] plan-alloc hard-ceiling + 属性隔离 全部通过\n");
    else             std::printf("[FAIL] 存在未通过项\n");
    return g_fail == 0 ? 0 : 1;
}
