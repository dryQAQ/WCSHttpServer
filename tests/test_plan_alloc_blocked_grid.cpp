// ============================================================================
// test_plan_alloc_blocked_grid.cpp — ★ 2026-09-20 现场问题④（no_bind，可执行自测）
//
// 现场问题：格口「已解锁但未绑定容器」时，软件仍把件下发到该格口（发送链路原本只校验
//   "满箱未重绑(禁用)"，没有容器校验）→ PLC 报成功、落格侧才发现无绑定（type=no_bind），
//   该件不属于任何容器 ⇒ 不进任何容器的 H7 ⇒ 换箱后新容器"满箱回传成功、人工复核多出一件"。
//
// 整改口径（客户确认 2026-09-20）：
//   · 下发前置条件 = 未禁用 && 已解锁 && 已绑定容器；
//   · 未绑定容器的格口**不下发**（判定期改投异常口），**额度原样保留**在该格口
//     —— WMS 重发 H6 绑定容器后自动恢复分配（这就是"不消耗额度"的落地方式）；
//   · 锁格：完全按原有逻辑处理（本表不参与该判定）。
//
// 本测试锁定「认领掩码」这一环的纯逻辑（PlanAllocTable 不依赖 Qt Widgets / 不查库）：
//   ① 掩码命中的格口**永不被认领**，件改由同 SKU 其它可分配格口承接；
//   ② 掩码命中的格口其 计划/已落/在途/余量 **一个都不动**（额度保留）；
//   ③ 全部计划格口的额度都停在掩码内时：认领失败，但 SKU 余量**不被清零**
//      （否则这些计划件会被永久判为"已满额"而全数改投异常口）；
//   ④ 掩码传 nullptr 时行为与改造前逐字一致（零回归保证）；
//   ⑤ 绑定容器（掩码撤销）后额度仍在 → 认领立刻恢复选中该格口；
//   ⑥ 不变量巡检 audit() 在以上全部场景保持为空（①③④ 全过）。
//
// ★ 掩码内容由 HttpServer::allocBlockedMask() 提供（判据与发送侧同源 isGridDispatchable）：
//   现场问题④的"已解锁且未绑定容器"、以及"满箱未重绑(禁用)"都会进掩码 ——
//   即"只有可下发格口能被认领"；本表只按掩码跳过，不关心掩码来源，故下列用例
//   同时覆盖这两种来源（把 kGrid34 当作任一"不可下发格口"即可）。
//
// 数据基线（09-14 现场真实案例）：SKU → 034(分类) 计划 1 件 + 048(发货) 计划 3 件。
// 构建：见 tests\run_tests.bat 第 [10/10] 步
// ============================================================================

#include <QCoreApplication>
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

static const char* kSku    = "106101134113101";
static const qint16 kGrid34 = 34;
static const qint16 kGrid48 = 48;

static bool buildPlan(PlanAllocTable& t)
{
    QVector<PlanGridInput> gs;
    { PlanGridInput c; c.grid = kGrid34; c.type = 0; c.qty = 1; gs.append(c); }
    { PlanGridInput c; c.grid = kGrid48; c.type = 2; c.qty = 3; gs.append(c); }
    QVector<QPair<QString, QVector<PlanGridInput>>> skuPlans;
    skuPlans.append(qMakePair(QString(kSku), gs));
    return t.build(BINDING_SLOT_COUNT, skuPlans);
}

// 认领一次；返回选中格口（失败返回 -1），*okOut 输出成功与否
static int claimOnce(PlanAllocTable& t, const PlanAllocTable::GridMask* mask, bool* okOut = nullptr)
{
    qint16 grid = -1, idx = -1;
    quint64 id = 0;
    const bool ok = t.claim(QString(kSku), &grid, &id, &idx, mask);
    if (okOut) *okOut = ok;
    return ok ? (int)grid : -1;
}

static PlanAllocTable::GridMask maskOf34()
{
    PlanAllocTable::GridMask m{};
    m[(size_t)kGrid34] = true;      // ★ 034 已解锁且未绑定容器 → 不可分配
    return m;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const QString sku(kSku);

    // ════════════════════════════════════════════════════════════════════
    std::printf("== ① 掩码命中 034（未绑定容器）→ 认领改由 048 承接，034 额度一个都不动 ==\n");
    {
        PlanAllocTable t;
        check(buildPlan(t), QString("分配表编译成功（034 计划1件 + 048 计划3件）"));

        const int p34 = t.planQtyOf(sku, "034");
        const int r34 = t.remainOf(sku, "034");
        const int l34 = t.landedOf(sku, "034");
        const int v34 = t.reservOf(sku, "034");

        const PlanAllocTable::GridMask mask = maskOf34();
        bool ok = false;
        const int g = claimOnce(t, &mask, &ok);
        check(ok && g == (int)kGrid48, QString("认领选中 048（实际 %1）").arg(g));

        check(t.planQtyOf(sku, "034") == p34 && t.remainOf(sku, "034") == r34 &&
              t.landedOf(sku, "034") == l34 && t.reservOf(sku, "034") == v34,
              QString("034 计划/已落/在途/余量完全未动（计划%1 余量%2 已落%3 在途%4）")
                  .arg(p34).arg(r34).arg(l34).arg(v34),
              QString("计划%1 余量%2 已落%3 在途%4")
                  .arg(t.planQtyOf(sku, "034")).arg(t.remainOf(sku, "034"))
                  .arg(t.landedOf(sku, "034")).arg(t.reservOf(sku, "034")));
        check(t.reservOf(sku, "048") == 1, QString("048 记 1 件在途认领"));
        check(t.audit().isEmpty(), QString("不变量巡检通过"), t.audit().join(" / "));
    }

    // ════════════════════════════════════════════════════════════════════
    std::printf("== ② 全部计划格口被掩码标记 → 认领失败但 SKU 余量不被清零（额度保留）==\n");
    {
        PlanAllocTable t;
        buildPlan(t);

        PlanAllocTable::GridMask mask = maskOf34();
        mask[(size_t)kGrid48] = true;

        bool ok = true;
        const int g = claimOnce(t, &mask, &ok);
        check(!ok && g == -1, QString("认领失败（无可分配格口）"));
        check(t.skuRemainOf(sku) == 4,
              QString("SKU 余量仍为 4（额度保留，未被误判为已满额）"),
              QString::number(t.skuRemainOf(sku)));
        check(t.remainOf(sku, "034") == 1 && t.remainOf(sku, "048") == 3,
              QString("两格余量均保留（034=1 / 048=3）"),
              QString("034=%1 048=%2").arg(t.remainOf(sku, "034")).arg(t.remainOf(sku, "048")));
        check(t.audit().isEmpty(), QString("不变量巡检通过（Σ余量 == SKU余量 未被破坏）"), t.audit().join(" / "));
    }

    // ════════════════════════════════════════════════════════════════════
    std::printf("== ③ 掩码传 nullptr → 与改造前逐字一致（选中首个计划格口 034）==\n");
    {
        PlanAllocTable t;
        buildPlan(t);
        bool ok = false;
        const int g = claimOnce(t, nullptr, &ok);
        check(ok && g == (int)kGrid34, QString("零回归：不传掩码时认领 034（实际 %1）").arg(g));
        check(t.audit().isEmpty(), QString("不变量巡检通过"), t.audit().join(" / "));
    }

    // ════════════════════════════════════════════════════════════════════
    std::printf("== ④ 034 被掩码占满 048 后撤销掩码（=WMS 重发 H6 绑定成功）→ 认领恢复选中 034 ==\n");
    {
        PlanAllocTable t;
        buildPlan(t);

        const PlanAllocTable::GridMask mask = maskOf34();
        for (int i = 0; i < 3; ++i)                       // 034 不可分配 → 3 件全部由 048 承接
            check(claimOnce(t, &mask) == (int)kGrid48, QString("第 %1 件由 048 承接").arg(i + 1));
        check(t.remainOf(sku, "048") == 0 && t.remainOf(sku, "034") == 1,
              QString("048 额度用尽、034 额度仍在（余 1 件）"),
              QString("048=%1 034=%2").arg(t.remainOf(sku, "048")).arg(t.remainOf(sku, "034")));

        PlanAllocTable::GridMask none{};                  // 掩码撤销
        bool ok = false;
        const int g = claimOnce(t, &none, &ok);
        check(ok && g == (int)kGrid34,
              QString("034 额度未被消耗 → 绑定容器后立即恢复分配（实际 %1）").arg(g));
        check(t.audit().isEmpty(), QString("不变量巡检通过"), t.audit().join(" / "));
    }

    // ════════════════════════════════════════════════════════════════════
    std::printf("== ⑤ 部分额度用掉后再全掩码 → 余量=1+2=3（既不为 0，也不丢额度）==\n");
    {
        PlanAllocTable t;
        buildPlan(t);

        const PlanAllocTable::GridMask mask = maskOf34();
        bool ok1 = false;
        check(claimOnce(t, &mask, &ok1) == (int)kGrid48 && ok1, QString("先由 048 承接 1 件"));

        PlanAllocTable::GridMask all = maskOf34();
        all[(size_t)kGrid48] = true;
        bool ok2 = true;
        const int g2 = claimOnce(t, &all, &ok2);
        check(!ok2 && g2 == -1, QString("全掩码 → 认领失败"));
        check(t.skuRemainOf(sku) == 3,
              QString("SKU 余量 = 3（034 的 1 件 + 048 的 2 件，未被清零）"),
              QString::number(t.skuRemainOf(sku)));
        check(t.audit().isEmpty(), QString("不变量巡检通过"), t.audit().join(" / "));
    }

    // ════════════════════════════════════════════════════════════════════
    std::printf("\n==== 结果：通过 %d / 失败 %d ====\n", g_pass, g_fail);
    if (g_fail == 0) std::printf("[OK] plan-alloc blocked-grid mask 全部通过\n");
    else             std::printf("[FAIL] 存在未通过项\n");
    return g_fail == 0 ? 0 : 1;
}
